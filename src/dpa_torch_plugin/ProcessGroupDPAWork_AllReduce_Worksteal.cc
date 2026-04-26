// ProcessGroupDPAWork_Worksteal.cc
#include "ProcessGroupDPAWork.h"

#include <c10/cuda/CUDACachingAllocator.h>
#include <c10/cuda/CUDAGuard.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

using namespace c10d;

namespace {

std::deque<std::shared_ptr<DPAAllReduceWork::CallbackState>> work_queue;
std::mutex work_queue_mutex;
std::mutex g_submit_m;
std::condition_variable g_submit_cv;
std::atomic<uint64_t> g_next_to_submit{1};

} // namespace

DPAAllReduceWorkWorksteal::DPAAllReduceWorkWorksteal(dpa::Context &ctx, std::vector<at::Tensor> tensors,
                                                     dpa::AllReduceOptions const &options,
                                                     std::shared_ptr<PinnedMempool> pool)
    : DPAAllReduceWork(ctx) {
  pool_ = std::move(pool);

  TORCH_CHECK(tensors.size() == 1, "Expected single tensor");
  TORCH_CHECK(tensors[0].defined() && tensors[0].is_contiguous(), "Invalid tensor");
  TORCH_CHECK(dpa_supports_dtype(tensors[0].scalar_type()), "Unsupported dtype");

  state_->tensors = std::move(tensors);
  state_->is_cuda = state_->tensors[0].is_cuda();

  // Worksteal finalize: sync H2D + recordStream + mark future complete
  state_->finalize_fn = [](CallbackState &st, bool success, const std::string &errmsg) {
    if (!st.finalized.exchange(true)) {
      if (success) {
        if (st.is_cuda) {
          c10::cuda::CUDAStreamGuard guard(st.stream);
          st.tensors[0].copy_(st.tensors_cpu[0], /*non_blocking=*/false);
          st.h2d_event.record(st.stream);
          auto *s = st.tensors[0].storage().unsafeGetStorageImpl();
          c10::cuda::CUDACachingAllocator::recordStream(s->data_ptr(), c10::cuda::CUDAStream(st.stream));
        }
        st.future->markCompleted(c10::IValue(st.tensors[0]));
      } else {
        st.future->setError(std::make_exception_ptr(std::runtime_error(errmsg)));
      }
    }
  };

  if (state_->is_cuda) {
    const int device = state_->tensors[0].get_device();
    c10::Device dev(c10::kCUDA, device);

    state_->future = c10::make_intrusive<c10::ivalue::Future>(c10::TensorType::get(), std::vector<c10::Device>{dev});

    c10::OptionalDeviceGuard guard(dev);
    c10::impl::VirtualGuardImpl impl(dev.type());
    c10::Stream caller = impl.getStream(dev);
    state_->stream = impl.getStreamFromGlobalPool(dev, /*isHighPriority=*/true);

    state_->tensors_cpu.resize(1);
    state_->tensors_cpu[0] = pool_ ? pool_->acquire(state_->tensors[0]) : pinnedLike(state_->tensors[0]);
    TORCH_CHECK(state_->tensors_cpu[0].is_pinned(), "CPU buffer must be pinned");
    TORCH_CHECK(state_->tensors[0].is_contiguous(), "GPU tensor not contiguous");
    TORCH_CHECK(state_->tensors_cpu[0].is_contiguous(), "CPU tensor not contiguous");

    {
      c10::cuda::CUDAStreamGuard sg(state_->stream);
      state_->fence.record(caller);
      state_->fence.block(state_->stream);
      state_->tensors_cpu[0].copy_(state_->tensors[0], /*non_blocking=*/false);
      state_->d2h_event.record(state_->stream);
    }

    // Caller: Enqueue the work
    {
      std::lock_guard<std::mutex> lock(work_queue_mutex);
      work_queue.push_back(state_);
    }

    // Worker: pick next work from queue (may not be the same), wait D2H, gate backend calls
    at::launch([options, pool = pool_]() {
      std::shared_ptr<CallbackState> work_state;
      {
        std::unique_lock<std::mutex> lock(work_queue_mutex);
        if (!work_queue.empty()) {
          work_state = work_queue.front();
          work_queue.pop_front();
        }
      }
      if (!work_state) return;

      // wait the D2H copy
      {
        const int dev = work_state->is_cuda ? work_state->tensors[0].get_device() : 0;
        c10::cuda::CUDAGuard dg(dev);
        work_state->d2h_event.synchronize();
      }

      // wait until next in order is the dequeued work's seqnum
      {
        std::unique_lock<std::mutex> lk(g_submit_m);
        g_submit_cv.wait(lk, [&] { return work_state->seqnum == g_next_to_submit.load(std::memory_order_acquire); });
      }

      auto dt = dpa_dtype(work_state->tensors[0].scalar_type());
      auto len = static_cast<uint32_t>(work_state->tensors[0].numel());
      void *ptr = work_state->tensors_cpu[0].data_ptr();
      work_state->task = work_state->ctx.AllReduceAsync(ptr, ptr, len, dt, options);

      // Backend finishes the op -> completion callback fires on a backend thread;
      // Use torch's pool for H2D + finalize to avoid blocking the backend.
      auto on_complete = [work_state, pool](dpa::Task &) {
        at::launch([work_state, pool]() {
          work_state->finalize(true);
          work_state->h2d_event.synchronize();
          if (pool) pool->release(work_state->tensors_cpu[0]);
        });
      };
      auto on_error = [work_state, pool](dpa::Task &) {
        at::launch([work_state, pool]() {
          work_state->finalize(false, "DPA allreduce failed");
          if (work_state->is_cuda && pool) pool->release(work_state->tensors_cpu[0]);
        });
      };

      auto c = work_state->task ? work_state->task->setCompletionCallback(on_complete) : true;
      auto e = work_state->task ? work_state->task->setErrorCallback(on_error) : true;
      if (!c && work_state->task->isCompleted()) on_complete(*work_state->task);
      if (!e && work_state->task->isFailed()) on_error(*work_state->task);

      {
        std::lock_guard<std::mutex> lk(g_submit_m);
        g_next_to_submit.fetch_add(1, std::memory_order_release);
      }
      g_submit_cv.notify_all();
    });

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

    state_->task = ctx.AllReduceAsync(ptr, ptr, len, dt, options);

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