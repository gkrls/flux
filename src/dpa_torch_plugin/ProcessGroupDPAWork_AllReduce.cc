#include "ProcessGroupDPAWork.h"
#include "c10/cuda/CUDAGuard.h"
// #include "dpa/util/log.h"
// #include "dpa/allreduce.h"
// #include "c10/cuda/CUDACachingAllocator.h"
// #include "ATen/cuda/CUDAContext.h"
#include <chrono>
// #include <memory>

using namespace c10d;

static inline std::uint64_t dpa_work_seqnum() noexcept {
  static std::atomic<std::uint64_t> g{0};
  return 1 + g.fetch_add(1, std::memory_order_relaxed);
}

static inline uint64_t now_us() {
  using clock = std::chrono::steady_clock;
  return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count();
}

namespace {
const std::string g_mode = [] {
  const char *e = std::getenv("DPA_TORCH_MODE");
  const char *v = e ? e : "worksteal";
  printf("dpa.torch: Using DPA_TORCH_MODE=%s (%s)\n", v, e ? "user" : "default");
  return v;
}();
} // namespace

DPAAllReduceWork::CallbackState::CallbackState(dpa::Context &c, uint64_t seq)
    : stream(c10::Stream::Default(), c10::Device(c10::kCPU)), ctx(c), seqnum(seq) {}

void DPAAllReduceWork::CallbackState::finalize(bool success, const std::string &errmsg) {
  if (finalize_fn) finalize_fn(*this, success, errmsg);
}

DPAAllReduceWork::DPAAllReduceWork(dpa::Context &ctx) : Work(ctx.rank, OpType::ALLREDUCE) {
  state_ = std::make_shared<CallbackState>(ctx, dpa_work_seqnum());
  t_start = std::chrono::steady_clock::now();
}

c10::intrusive_ptr<Work> DPAAllReduceWork::make(dpa::Context &ctx, std::vector<at::Tensor> tensors,
                                                dpa::AllReduceOptions const &options,
                                                std::shared_ptr<PinnedMempool> pool) {
  if (g_mode == "simple")
    return c10::make_intrusive<DPAAllReduceWorkSimple>(ctx, std::move(tensors), options, std::move(pool));
  if (g_mode == "hybrid")
    return c10::make_intrusive<DPAAllReduceWorkHybrid>(ctx, std::move(tensors), options, std::move(pool));
  if (g_mode == "pipeline" or g_mode == "latency")
    return c10::make_intrusive<DPAAllReduceWorkPipeline>(ctx, std::move(tensors), options, std::move(pool));
  if (g_mode == "worksteal" or g_mode == "throughput")
    return c10::make_intrusive<DPAAllReduceWorkWorksteal>(ctx, std::move(tensors), options, std::move(pool));
  TORCH_CHECK(false, "Unknown DPA_TORCH_MODE: ", g_mode);
}

bool DPAAllReduceWork::wait(std::chrono::milliseconds timeout) {
  if (state_->future->completed()) {
    synchronize();
    return true;
  }

  if (state_->task) {
    if (timeout == kNoTimeout) {
      state_->task->wait();
    } else if (state_->task->wait(timeout) != dpa::Task::Completed) {
      return false;
    }
  }

  state_->future->wait();
  synchronize();
  return true;
}

void DPAAllReduceWork::synchronize() {
  if (!state_->future->completed()) { state_->future->wait(); }

  if (state_->is_cuda) {
    // Only make user stream wait for H2D when they need the result
    const int device = state_->tensors[0].get_device();
    c10::cuda::CUDAGuard dev_guard(device);
    c10::impl::VirtualGuardImpl impl(c10::kCUDA);
    c10::Stream user = impl.getStream(c10::Device(c10::kCUDA, device));
    state_->h2d_event.block(user);
  }
}

void DPAAllReduceWork::abort() {
  if (state_->task) { state_->task->abort(); }
  state_->finalize(false, "Aborted");
}

int DPAAllReduceWork::sourceRank() const { return -1; }

uint64_t DPAAllReduceWork::getSequencenumber() const { return state_->seqnum; }

bool DPAAllReduceWork::isCompleted() { return state_->future->completed(); }

bool DPAAllReduceWork::isSuccess() const { return state_->future->completed() && !state_->future->hasError(); }

std::exception_ptr DPAAllReduceWork::exception() const {
  return state_->future->hasError() ? state_->future->exception_ptr() : nullptr;
}

std::vector<at::Tensor> DPAAllReduceWork::result() { return state_->tensors; }

c10::intrusive_ptr<c10::ivalue::Future> DPAAllReduceWork::getFuture() { return state_->future; }

c10::intrusive_ptr<c10::ivalue::Future> DPAAllReduceWork::getFutureResult() { return state_->future; }

float DPAAllReduceWork::getDuration() const {
  auto end = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(end - t_start).count() / 1000.0f;
}