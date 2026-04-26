// // ProcessGroupDPAWork_Pipeline.cc
// #pragma message("DPA_TORCH_IMPLEMENTATION: PIPELINE (latency)")

#include "ProcessGroupDPAWork.h"

#include "c10/cuda/CUDACachingAllocator.h"
#include "c10/cuda/CUDAGuard.h"
// #include "cuda_runtime.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>

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
  printf("dpa.torch: Using DPA_TORCH_PIPELINE_CHUNKS=%d (%s)\n", n, v? "user" : "default");
  return n;
}();


const int64_t g_pipeline_thresh = []() {
  const char *v = std::getenv("DPA_TORCH_PIPELINE_THRESH");
  int64_t n = v ? std::atoll(v) : DPA_TORCH_PIPELINE_THRESH;
  if (n <= 0) n = DPA_TORCH_PIPELINE_THRESH;
  printf("dpa.torch: Using DPA_TORCH_PIPELINE_THRESH=%ld (%s)\n", n, v? "user" : "default");
  return n;
}();

std::mutex g_submit_m;
std::condition_variable g_submit_cv;
std::atomic<uint64_t> g_next_to_submit{1};

} // namespace

DPAAllReduceWorkPipeline::DPAAllReduceWorkPipeline(dpa::Context &ctx, std::vector<at::Tensor> tensors,
                                                   dpa::AllReduceOptions const &options,
                                                   std::shared_ptr<PinnedMempool> pool)
    : DPAAllReduceWork(ctx) {
  pool_ = std::move(pool);

  TORCH_CHECK(tensors.size() == 1, "Expected single tensor");
  TORCH_CHECK(tensors[0].defined() && tensors[0].is_contiguous(), "Invalid tensor");
  TORCH_CHECK(dpa_supports_dtype(tensors[0].scalar_type()), "Unsupported dtype");

  state_->tensors = std::move(tensors);
  state_->is_cuda = state_->tensors[0].is_cuda();

  // Finalize: H2D is done in the pipeline, not here.
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

    // Ensure D2H stream waits for the caller's compute (e.g. gradient kernels) to finish.
    state_->fence.record(caller);

    // Launch a worker to run the pipeline
    at::launch([state = state_, options, pool = pool_]() {
      try {
        const int dev_idx = state->tensors[0].get_device();
        c10::cuda::CUDAGuard dg(dev_idx);

        const int64_t total_elems = state->tensors[0].numel();
        const int64_t elem_size = state->tensors[0].element_size();
        const int64_t total_bytes = total_elems * elem_size;
        auto dt = dpa_dtype(state->tensors[0].scalar_type());

        uint8_t *gpu = static_cast<uint8_t *>(state->tensors[0].data_ptr());
        uint8_t *cpu = static_cast<uint8_t *>(state->tensors_cpu[0].data_ptr());

        // Two high-priority streams, one for D2H, one for H2D
        // This lets D2H and H2D run concurrently on the PCIe bus
        c10::cuda::CUDAStream d2h_stream(state->stream);
        c10::cuda::CUDAStream h2d_stream = c10::cuda::getStreamFromPool(/*isHighPriority=*/true, dev_idx);

        state->fence.block(d2h_stream);

        // Decide whether/how to chunk
        const int num_chunks = g_pipeline_chunks > 0
                                   ? g_pipeline_chunks
                                   : (total_bytes >= DPA_TORCH_PIPELINE_THRESH ? DPA_TORCH_PIPELINE_CHUNKS : 1);
        const int64_t chunk_elems = (total_elems + num_chunks - 1) / num_chunks;
        
        // wait our turn
        {
          std::unique_lock<std::mutex> lk(g_submit_m);
          g_submit_cv.wait(lk, [&] { return state->seqnum == g_next_to_submit.load(std::memory_order_acquire); });
        }

        std::vector<cudaEvent_t> d2h_done(num_chunks);
        for (auto &ev : d2h_done) cudaEventCreateWithFlags(&ev, cudaEventDisableTiming);

        // kick off D2H for chunk 0 
        {
          int64_t n = std::min(chunk_elems, total_elems);
          cudaMemcpyAsync(cpu, gpu, n * elem_size, cudaMemcpyDeviceToHost, d2h_stream.stream());
          cudaEventRecord(d2h_done[0], d2h_stream.stream());
        }


        // Main pipeline loop
        //
        //  Timeline for chunk c:
        //
        //    GPU DMA (d2h_stream)
        //       D2H chunk c+1 (async, overlaps network)
        //  
        //    CPU thread
        //       wait D2H[c] -> AllReduceAsync(c)
        //  
        //    GPU DMA (h2d_stream)
        //       H2D chunk c (async, overlaps next iter)
        //
        for (int c = 0; c < num_chunks; c++) {
          const int64_t off = c * chunk_elems;
          const int64_t elems = std::min(chunk_elems, total_elems - off);
          if (elems <= 0) break;

          // 1) CPU waits for this chunk's D2H to land in pinned memory
          cudaEventSynchronize(d2h_done[c]);

          // 2) Immediately enqueue D2H for the NEXT chunk
          if (c + 1 < num_chunks) {
            const int64_t noff = (c + 1) * chunk_elems;
            const int64_t ne = std::min(chunk_elems, total_elems - noff);
            if (ne > 0) {
              cudaMemcpyAsync(cpu + noff * elem_size, gpu + noff * elem_size, ne * elem_size, cudaMemcpyDeviceToHost,
                              d2h_stream.stream());
              cudaEventRecord(d2h_done[c + 1], d2h_stream.stream());
            }
          }

          // 3) Network: allreduce this chunk (in-place on pinned buffer)
          //    Blocks this thread; D2H[c+1] and H2D[c-1] run in parallel
          auto task = state->ctx.AllReduceAsync(cpu + off * elem_size, cpu + off * elem_size,
                                                static_cast<uint32_t>(elems), dt, options);
          if (task) task->wait();


          // 4) Enqueue H2D for this chunk back to GPU
          //    - Runs while we loop back to wait on D2H[c+1] and do network[c+1]
          cudaMemcpyAsync(gpu + off * elem_size, cpu + off * elem_size, elems * elem_size, cudaMemcpyHostToDevice,
                          h2d_stream.stream());
        }

        // Cleanup chunk events
        for (auto &ev : d2h_done) cudaEventDestroy(ev);

        // Record final H2D completion on the H2D stream
        state->h2d_event.record(h2d_stream);

        // Tell the caching allocator the GPU tensor is in use by h2d_stream
        auto *st = state->tensors[0].storage().unsafeGetStorageImpl();
        c10::cuda::CUDACachingAllocator::recordStream(st->data_ptr(), h2d_stream);

        // advance
        {
          std::lock_guard<std::mutex> lk(g_submit_m);
          g_next_to_submit.fetch_add(1, std::memory_order_release);
        }
        g_submit_cv.notify_all();

        state->finalize(true);

        state->h2d_event.synchronize();
        if (pool) pool->release(state->tensors_cpu[0]);

      } catch (const std::exception &e) {
        // Always advance the gate on error to avoid deadlocking other work items
        {
          std::lock_guard<std::mutex> lk(g_submit_m);
          g_next_to_submit.fetch_add(1, std::memory_order_release);
        }
        g_submit_cv.notify_all();

        state->finalize(false, std::string("DPA chunked allreduce: ") + e.what());
        if (pool) pool->release(state->tensors_cpu[0]);
      }
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
    state_->task = state_->ctx.AllReduceAsync(ptr, ptr, len, dt, options);

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