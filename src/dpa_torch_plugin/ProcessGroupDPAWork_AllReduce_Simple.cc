
#include "ProcessGroupDPAWork.h"

#include <c10/cuda/CUDACachingAllocator.h>
#include <c10/cuda/CUDAGuard.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace c10d;

namespace {

// Global submit-order gate: serialize AllReduceAsync in seqnum order.
std::mutex g_submit_m;
std::condition_variable g_submit_cv;
std::atomic<uint64_t> g_next_to_submit{1};

} // namespace

DPAAllReduceWorkSimple::DPAAllReduceWorkSimple(dpa::Context &ctx, std::vector<at::Tensor> tensors,
                                               dpa::AllReduceOptions const &options,
                                               std::shared_ptr<PinnedMempool> pool)
    : DPAAllReduceWork(ctx) {
  pool_ = std::move(pool);

  TORCH_CHECK(tensors.size() == 1, "Expected single tensor");
  TORCH_CHECK(tensors[0].defined() && tensors[0].is_contiguous(), "Invalid tensor");
  TORCH_CHECK(dpa_supports_dtype(tensors[0].scalar_type()), "Unsupported dtype");

  state_->tensors = std::move(tensors);
  state_->is_cuda = state_->tensors[0].is_cuda();

  // Install the per-variant finalize logic
  state_->finalize_fn = [](CallbackState &state, bool success, const std::string &errmsg) {
    if (!state.finalized.exchange(true)) {
      if (success) {
        if (state.is_cuda) {
          // run H2D on the high-priority stream
          c10::cuda::CUDAStreamGuard guard(state.stream);
          state.tensors[0].copy_(state.tensors_cpu[0], /*non_blocking=*/true);
          // Record event after H2D copy
          state.h2d_event.record(state.stream);
          // Attach stream to tensor's storage so later streams will wait
          auto *s = state.tensors[0].storage().unsafeGetStorageImpl();
          c10::cuda::CUDACachingAllocator::recordStream(s->data_ptr(), c10::cuda::CUDAStream(state.stream));
        }
      } else {
        state.future->setError(std::make_exception_ptr(std::runtime_error(errmsg)));
      }
    }
  };

  if (state_->is_cuda) {
    const int device = state_->tensors[0].get_device();
    c10::Device dev(c10::kCUDA, device);

    std::vector<c10::Device> future_devices{dev};
    state_->future = c10::make_intrusive<c10::ivalue::Future>(c10::TensorType::get(), std::move(future_devices));

    c10::OptionalDeviceGuard guard(dev);
    c10::impl::VirtualGuardImpl impl(dev.type());
    // c10::Stream caller = impl.getStream(dev);
    state_->stream = impl.getStreamFromGlobalPool(dev, /*isHighPriority=*/true);

    // Acquire pinned host buffer
    state_->tensors_cpu.resize(1);
    state_->tensors_cpu[0] = pool_ ? pool_->acquire(state_->tensors[0]) : pinnedLike(state_->tensors[0]);
    TORCH_CHECK(state_->tensors_cpu[0].is_pinned(), "CPU buffer must be pinned");

    // Sync D2H on the work's hi priority stream
    {
      c10::cuda::CUDAStreamGuard sg(state_->stream);
      state_->tensors_cpu[0].copy_(state_->tensors[0], /*non_blocking=*/false);
      state_->d2h_event.record(state_->stream); // Record for completeness; already done at this point.
    }

    // Submit gate: enforce strict in-rank order
    {
      std::unique_lock<std::mutex> lk(g_submit_m);
      g_submit_cv.wait(lk, [&] { return state_->seqnum == g_next_to_submit.load(std::memory_order_acquire); });
    }

    // Submit AllReduceAsync on the (potentially pinned) host buffer
    auto dt = dpa_dtype(state_->tensors[0].scalar_type());
    auto len = static_cast<uint32_t>(state_->tensors[0].numel());
    void *ptr = state_->tensors_cpu[0].data_ptr();

    state_->task = ctx.AllReduceAsync(ptr, ptr, len, dt, options);

    auto on_complete = [st = state_, pool = pool_](dpa::Task &) {
      at::launch([st, pool]() {
        st->finalize(true);
        // Wait for h2d to complete
        st->h2d_event.synchronize();
        // Return pinned buffer to the pool if needed
        if (pool) pool->release(st->tensors_cpu[0]);
        st->future->markCompleted(c10::IValue(st->tensors[0]));
      });
    };

    auto on_error = [st = state_, pool = pool_](dpa::Task &) {
      at::launch([st, pool]() {
        st->finalize(false, "DPA allreduce failed");
        if (st->is_cuda && pool) pool->release(st->tensors_cpu[0]);
      });
    };

    auto c = state_->task ? state_->task->setCompletionCallback(on_complete) : true;
    auto e = state_->task ? state_->task->setErrorCallback(on_error) : true;
    if (!c && state_->task->isCompleted()) on_complete(*state_->task);
    if (!e && state_->task->isFailed()) on_error(*state_->task);

    // Advance submit ticket
    {
      std::lock_guard<std::mutex> lk(g_submit_m);
      g_next_to_submit.fetch_add(1, std::memory_order_release);
    }
    g_submit_cv.notify_all();

  } else {
    // CPU path
    state_->future = c10::make_intrusive<c10::ivalue::Future>(c10::TensorType::get());

    {
      std::unique_lock<std::mutex> lk(g_submit_m);
      g_submit_cv.wait(lk, [&] { return state_->seqnum == g_next_to_submit.load(std::memory_order_acquire); });
    }

    auto dt = dpa_dtype(state_->tensors[0].scalar_type());
    auto len = static_cast<uint32_t>(state_->tensors[0].numel());
    void *ptr = state_->tensors[0].data_ptr();

    state_->task = ctx.AllReduceAsync(ptr, ptr, len, static_cast<dpa::DataType>(dt), options);

    auto on_complete = [st = state_](dpa::Task &) { st->finalize(true); };
    auto c = state_->task->setCompletionCallback(on_complete);
    if (!c && state_->task->isCompleted()) on_complete(*state_->task);

    {
      std::lock_guard<std::mutex> lk(g_submit_m);
      g_next_to_submit.fetch_add(1, std::memory_order_release);
    }
    g_submit_cv.notify_all();
  }
}