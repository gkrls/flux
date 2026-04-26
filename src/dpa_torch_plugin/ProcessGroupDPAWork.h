#ifndef DPA_TORCH_PLUGIN_PROCESS_GROUP_DPA_WORK_H
#define DPA_TORCH_PLUGIN_PROCESS_GROUP_DPA_WORK_H

#include "c10/util/intrusive_ptr.h"
// #include "torch/csrc/distributed/c10d/Backend.hpp"
#include "dpa/allreduce.h"
#include "dpa/context.h"
#include "torch/csrc/distributed/c10d/Work.hpp"

#include "ProcessGroupDPAUtils.h"

namespace c10d {

// Base for all AllReduce Works
class DPAAllReduceWork : public c10d::Work {
public:
  bool wait(std::chrono::milliseconds timeout = kNoTimeout) override;
  void abort() override;
  bool isCompleted() override;
  bool isSuccess() const override;
  std::exception_ptr exception() const override;
  int sourceRank() const override;
  std::vector<at::Tensor> result() override;
  uint64_t getSequencenumber() const override;
  c10::intrusive_ptr<c10::ivalue::Future> getFuture() override;
  c10::intrusive_ptr<c10::ivalue::Future> getFutureResult() override;
  float getDuration() const override;
  void synchronize() override;

public:
  // Shared state between work and callbacks
  struct CallbackState {
    // Tensors
    std::vector<at::Tensor> tensors;
    std::vector<at::Tensor> tensors_cpu;
    bool is_cuda = false;

    // CUDA synchronization
    c10::Stream stream;
    // c10::Event event;        // Initial sync event
    // c10::Event d2h_event;    // D2H completion event
    // c10::Event h2d_event;    // H2D completion event
    c10::Event fence{c10::DeviceType::CUDA};
    c10::Event d2h_event{c10::DeviceType::CUDA};
    c10::Event h2d_event{c10::DeviceType::CUDA};

    // DPA context
    dpa::Context &ctx;
    uint64_t seqnum;

    // Completion tracking
    c10::intrusive_ptr<c10::ivalue::Future> future;
    std::atomic<bool> finalized{false};

    std::shared_ptr<dpa::Task> task = nullptr;

    CallbackState(dpa::Context &c, uint64_t seq);

    // Per-variant finalize. Set by the derived Work ctor.
    std::function<void(CallbackState &, bool, const std::string &)> finalize_fn;

    void finalize(bool success, const std::string &errmsg = "");
  };
  DPAAllReduceWork(dpa::Context &ctx);

protected:
  std::shared_ptr<CallbackState> state_ = nullptr;
  std::shared_ptr<PinnedMempool> pool_ = nullptr;
  std::chrono::steady_clock::time_point t_start;

public:
  ~DPAAllReduceWork() override = default; // not sure this is needed
  static c10::intrusive_ptr<Work> make(dpa::Context &ctx, std::vector<at::Tensor> tensors,
                                       dpa::AllReduceOptions const &opts, std::shared_ptr<PinnedMempool> pool);
};

// Derived — one per strategy
class DPAAllReduceWorkSimple : public DPAAllReduceWork {
public:
  DPAAllReduceWorkSimple(dpa::Context &ctx, std::vector<at::Tensor> tensors, dpa::AllReduceOptions const &opts,
                         std::shared_ptr<PinnedMempool> pool);
};
class DPAAllReduceWorkPipeline : public DPAAllReduceWork {
public:
  DPAAllReduceWorkPipeline(dpa::Context &ctx, std::vector<at::Tensor> tensors, dpa::AllReduceOptions const &opts,
                           std::shared_ptr<PinnedMempool> pool);
};
class DPAAllReduceWorkWorksteal : public DPAAllReduceWork {
public:
  DPAAllReduceWorkWorksteal(dpa::Context &ctx, std::vector<at::Tensor> tensors, dpa::AllReduceOptions const &opts,
                            std::shared_ptr<PinnedMempool> pool);
};
class DPAAllReduceWorkHybrid : public DPAAllReduceWork {
public:
  DPAAllReduceWorkHybrid(dpa::Context &ctx, std::vector<at::Tensor> tensors, dpa::AllReduceOptions const &opts,
                         std::shared_ptr<PinnedMempool> pool);
};

#endif

} // namespace c10d