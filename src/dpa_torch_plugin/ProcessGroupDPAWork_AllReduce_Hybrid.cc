// ProcessGroupDPAWork_Hybrid.cc
#include "ProcessGroupDPAWork.h"

#include <c10/cuda/CUDACachingAllocator.h>
#include <c10/cuda/CUDAGuard.h>
#include <cuda_runtime.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

using namespace c10d;

#ifndef DPA_TORCH_PIPELINE_CHUNKS
#define DPA_TORCH_PIPELINE_CHUNKS 4
#endif

#ifndef DPA_TORCH_PIPELINE_THRESH
#define DPA_TORCH_PIPELINE_THRESH (4 * 1024 * 1024)
#endif

namespace {
const int g_pipeline_chunks = []() {
  const char *v = std::getenv("DPA_TORCH_PIPELINE_CHUNKS");
  int n = v ? std::atoi(v) : 0;
  // if (v) { std::cout << "dpa.torch: Using DPA_TORCH_PIPELINE_CHUNKS=" << n << " (user)"; }
  // else   { std::cout << "dpa.torch: Using DPA_TORCH_PIPELINE_CHUNKS=" << n << " (default)"; }
  return n;
}();


const int64_t g_pipeline_thresh = []() {
  const char *v = std::getenv("DPA_TORCH_PIPELINE_THRESH");
  int64_t n = v ? std::atoll(v) : DPA_TORCH_PIPELINE_THRESH;
  if (n <= 0) n = DPA_TORCH_PIPELINE_THRESH;
  // if (v) { std::cout << "dpa.torch: Using DPA_TORCH_PIPELINE_THRESH=" << n << " (user)"; }
  // else   { std::cout << "dpa.torch: Using DPA_TORCH_PIPELINE_THRESH=" << n << " (default)"; }
  return n;
}();
struct WorkItem {
  std::shared_ptr<DPAAllReduceWork::CallbackState> state;
  dpa::AllReduceOptions options;
  std::shared_ptr<PinnedMempool> pool;
};

std::deque<WorkItem> g_queue;
std::mutex g_queue_m;
std::condition_variable g_queue_cv;

void worker_loop() { // exits when process exits
  while (true) {
    WorkItem item;
    {
      std::unique_lock<std::mutex> lk(g_queue_m);
      g_queue_cv.wait(lk, [] { return !g_queue.empty(); });
      item = std::move(g_queue.front());
      g_queue.pop_front();
    }

    auto &state = item.state;
    auto &options = item.options;
    auto &pool = item.pool;

    try {
      if (!state->is_cuda) {
        // CPU path
        auto dt = dpa_dtype(state->tensors[0].scalar_type());
        auto len = static_cast<uint32_t>(state->tensors[0].numel());
        void *ptr = state->tensors[0].data_ptr();

        state->task = state->ctx.AllReduceAsync(ptr, ptr, len, dt, options);
        auto on_complete = [state](dpa::Task &) { state->finalize(true); };
        auto c = state->task->setCompletionCallback(on_complete);
        if (!c && state->task->isCompleted()) on_complete(*state->task);
        continue;
      }

      const int dev_idx = state->tensors[0].get_device();
      c10::cuda::CUDAGuard dg(dev_idx);

      const int64_t total_elems = state->tensors[0].numel();
      const int64_t elem_size = state->tensors[0].element_size();
      const int64_t total_bytes = total_elems * elem_size;
      auto dt = dpa_dtype(state->tensors[0].scalar_type());

      uint8_t *gpu = static_cast<uint8_t *>(state->tensors[0].data_ptr());
      uint8_t *cpu = static_cast<uint8_t *>(state->tensors_cpu[0].data_ptr());

      c10::cuda::CUDAStream d2h_stream(state->stream);
      state->fence.block(d2h_stream);

      // Decide mode: throughput (single-shot) if small, latency (chunked) if large
      const int num_chunks = total_bytes >= g_pipeline_thresh ? g_pipeline_chunks : 1;

      if (num_chunks <= 1) {
        // THROUGHPUT MODE (small ops): pipelines across ops.
        //
        //   D2H:  [op0][op1][op2]
        //   net:       [op0][op1][op2]
        //   H2D:            [op0][op1][op2]   (runs in callback)
        //
        // Worker fires op, moves on. Callback finishes it.
        cudaMemcpyAsync(cpu, gpu, total_bytes, cudaMemcpyDeviceToHost, d2h_stream.stream());
        cudaStreamSynchronize(d2h_stream.stream());

        state->task = state->ctx.AllReduceAsync(cpu, cpu, static_cast<uint32_t>(total_elems), dt, options);

        // Callbacks run when backend finishes Net. They launch new tasks in Torch's thread pool
        // to handle waiting and completions in order to not delay dataplane (e.g. DPDK) threads
        auto on_complete = [state, pool](dpa::Task &) {
          at::launch([state, pool]() {
            const int d = state->tensors[0].get_device();
            c10::cuda::CUDAGuard dg(d);
            {
              c10::cuda::CUDAStreamGuard sg(state->stream);
              state->tensors[0].copy_(state->tensors_cpu[0], /*non_blocking=*/true);
              state->h2d_event.record(state->stream);
              auto *st = state->tensors[0].storage().unsafeGetStorageImpl();
              c10::cuda::CUDACachingAllocator::recordStream(st->data_ptr(), c10::cuda::CUDAStream(state->stream));
            }
            state->finalize(true);
            state->h2d_event.synchronize();
            if (pool) pool->release(state->tensors_cpu[0]);
          });
        };
        auto on_error = [state, pool](dpa::Task &) {
          at::launch([state, pool]() {
            state->finalize(false, "DPA allreduce failed");
            if (pool) pool->release(state->tensors_cpu[0]);
          });
        };
        auto c = state->task ? state->task->setCompletionCallback(on_complete) : true;
        auto e = state->task ? state->task->setErrorCallback(on_error) : true;
        if (!c && state->task->isCompleted()) on_complete(*state->task);
        if (!e && state->task->isFailed()) on_error(*state->task);
      } else {
        // LATENCY MODE (large ops): pipelines chunks within one op.
        //
        //   D2H:  [c0][c1][c2][c3]
        //   net:      [c0][c1][c2][c3]
        //   H2D:          [c0][c1][c2][c3]
        //
        // Worker drives each chunk: wait D2H -> net (blocks) -> H2D.
        const int64_t chunk_elems = (total_elems + num_chunks - 1) / num_chunks;
        c10::cuda::CUDAStream h2d_stream = c10::cuda::getStreamFromPool(/*isHighPriority=*/true, dev_idx);

        std::vector<cudaEvent_t> d2h_done(num_chunks);
        for (auto &ev : d2h_done) cudaEventCreateWithFlags(&ev, cudaEventDisableTiming);

        // Kick off D2H for chunk 0
        {
          int64_t n = std::min(chunk_elems, total_elems);
          cudaMemcpyAsync(cpu, gpu, n * elem_size, cudaMemcpyDeviceToHost, d2h_stream.stream());
          cudaEventRecord(d2h_done[0], d2h_stream.stream());
        }

        for (int c = 0; c < num_chunks; c++) {
          const int64_t off = c * chunk_elems;
          const int64_t elems = std::min(chunk_elems, total_elems - off);
          if (elems <= 0) break;

          // Wait for this chunk's D2H
          cudaEventSynchronize(d2h_done[c]);

          // Kick off D2H for next chunk (overlaps with network)
          if (c + 1 < num_chunks) {
            int64_t noff = (c + 1) * chunk_elems;
            int64_t ne = std::min(chunk_elems, total_elems - noff);
            if (ne > 0) {
              cudaMemcpyAsync(cpu + noff * elem_size, gpu + noff * elem_size, ne * elem_size, cudaMemcpyDeviceToHost,
                              d2h_stream.stream());
              cudaEventRecord(d2h_done[c + 1], d2h_stream.stream());
            }
          }

          // Network allreduce this chunk (blocks until done)
          auto task = state->ctx.AllReduceAsync(cpu + off * elem_size, cpu + off * elem_size,
                                                static_cast<uint32_t>(elems), dt, options);
          if (task) task->wait();

          // H2D for this chunk (overlaps with next D2H + network)
          cudaMemcpyAsync(gpu + off * elem_size, cpu + off * elem_size, elems * elem_size, cudaMemcpyHostToDevice,
                          h2d_stream.stream());
        }

        for (auto &ev : d2h_done) cudaEventDestroy(ev);

        // Wait for all H2D before releasing pinned buffer
        state->h2d_event.record(h2d_stream);
        auto *st = state->tensors[0].storage().unsafeGetStorageImpl();
        c10::cuda::CUDACachingAllocator::recordStream(st->data_ptr(), h2d_stream);

        state->finalize(true);
        state->h2d_event.synchronize();
        if (pool) pool->release(state->tensors_cpu[0]);
      }
    } catch (const std::exception &e) {
      state->finalize(false, std::string("DPA hybrid: ") + e.what());
      if (pool) pool->release(state->tensors_cpu[0]);
    }
  }
}

std::once_flag g_worker_once;
void ensure_worker() {
  std::call_once(g_worker_once, [] { std::thread(worker_loop).detach(); });
}

} // namespace

DPAAllReduceWorkHybrid::DPAAllReduceWorkHybrid(dpa::Context &ctx, std::vector<at::Tensor> tensors,
                                               dpa::AllReduceOptions const &options,
                                               std::shared_ptr<PinnedMempool> pool)
    : DPAAllReduceWork(ctx) {
  pool_ = std::move(pool);

  TORCH_CHECK(tensors.size() == 1, "Expected single tensor");
  TORCH_CHECK(tensors[0].defined() && tensors[0].is_contiguous(), "Invalid tensor");
  TORCH_CHECK(dpa_supports_dtype(tensors[0].scalar_type()), "Unsupported dtype");

  state_->tensors = std::move(tensors);
  state_->is_cuda = state_->tensors[0].is_cuda();

  // Hybrid finalize: just complete the future. Both modes handle H2D in their own path.
  state_->finalize_fn = [](CallbackState &st, bool success, const std::string &errmsg) {
    if (!st.finalized.exchange(true)) {
      if (success) {
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

    state_->fence.record(caller);
  } else {
    state_->future = c10::make_intrusive<c10::ivalue::Future>(c10::TensorType::get());
  }

  ensure_worker();
  {
    std::lock_guard<std::mutex> lk(g_queue_m);
    g_queue.push_back({state_, options, pool_});
  }
  g_queue_cv.notify_one();
}