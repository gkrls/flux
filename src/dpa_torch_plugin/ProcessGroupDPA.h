#ifndef DPA_TORCH_PLUGIN_PROCESS_GROUP_DPA_H
#define DPA_TORCH_PLUGIN_PROCESS_GROUP_DPA_H

#include "dpa/allreduce.h"
#include "torch/csrc/distributed/c10d/Types.hpp"
#define USE_C10D_GLOO
#include <torch/csrc/distributed/c10d/ProcessGroupGloo.hpp>

#include "ProcessGroupDPAUtils.h"

#include "c10/util/intrusive_ptr.h"
#include "torch/csrc/distributed/c10d/Backend.hpp"
#include "torch/csrc/distributed/c10d/Work.hpp"
// #include <ATen/cuda/CUDAEvent.h>
// #include <ATen/cuda/CUDAStream.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>

#include "dpa/dpa.h"
#include <atomic>
#include <chrono>
#include <memory>
// #include <optional>
// #include <thread>

namespace c10d {

constexpr const char *DPA_SOCK_BACKEND_NAME = "dpa_sock";
constexpr const char *DPA_DPDK_BACKEND_NAME = "dpa_dpdk";
constexpr const char *DPA_RDMA_BACKEND_NAME = "dpa_rdma";

/// Helper class that allows us to create a python context that enables dataplane_allreduce.
/// Outside this context the DPA processgroup will run allreduce, which fallbacks to the Gloo impementation (for socket/dpdk backends)
///
/// The reason this context exist is that at the Python level, dist.allreduce looks like this:
///
///     https://github.com/pytorch/pytorch/blob/e2d141dbde55c2a4370fac5165b0561b6af4798b/torch/distributed/distributed_c10d.py#L2722
///
///     def all_reduce(tensor, op=ReduceOp.SUM, group=None, async_op=False):
///        ...
///
/// Internally, it will create a AllreduceOptions instance and pass it to the pg allreduce like this:
///
///     opts = AllreduceOptions()
///     ...
///     work = group.allreduce([tensor], opts)
///
/// Thus, we need a way (without changing Pytorch's Python code) to control our dataplane allreduce, which has more options
/// Calling allreduce inside a Dataplane context achieves exactly this
class DataplaneContext {
private:
  dpa::AllReduceOptions opts;
  static inline thread_local DataplaneContext *ctx = nullptr;
  DataplaneContext *prev = nullptr;

public:
  DataplaneContext(uint8_t quantization, bool averaging, bool prescaled, uint8_t sa_world, bool sa_preemptive, uint8_t pipes) {
    opts.pipes = pipes;
    opts.averaging = averaging;
    opts.quantization = quantization;
    opts.prescaled = prescaled;
    opts.sa_world = sa_world;
    opts.sa_preemptive = sa_preemptive;
  }

  void enter() {
    prev = ctx;
    ctx = this;
  }
  void exit() {
    ctx = prev;
    prev = nullptr;
  }
  dpa::AllReduceOptions &options() { return opts; }

  static DataplaneContext *get() { return ctx; }
  static dpa::AllReduceOptions *getOptions() { return ctx ? &ctx->opts : nullptr; }
};

/// Options for an allreduce Work in the DPA ProcessGroups
/// It is basically an extensions of the Torch AllReduceOptions
/// with the addition of a dpa::AllReduceOptions object
// class DPAAllreduceOptions : public c10d::AllreduceOptions {
// public:
//   /// asyncOp is part c10d::AllReduceOptions only on Torch >= 2.8
//   /// for earlier versions (like 2.7.1 that we use), it is only in dist.all_reduce and that function itself handles the async like so:
//   /// https://github.com/pytorch/pytorch/blob/e2d141dbde55c2a4370fac5165b0561b6af4798b/torch/distributed/distributed_c10d.py#L2812
//   ///    ...
//   ///    if async_op:
//   ///      return work
//   ///    else:
//   ///      return work.wait()
//   ///
//   /// In other words all Work objects are assumed async by default. For this reason our DPAAllReduceWork allways calls AllReduceAsync.
//   /// so this field is not currently used anywhere. We still store it however just in case we want to handle this differently in the
//   future bool asyncOp = false; dpa::AllReduceOptions dpa;
// };

/// This class is instantiated when we do init_process_group("dpa-sock", ...) from Python
class ProcessGroupDPASocket : public ProcessGroupGloo {
public:
  friend class DPAAllReduceWork;
  /// Options for the ProcessGroupDPASocket
  class Options : public Backend::Options {
  public:
    c10::intrusive_ptr<ProcessGroupGloo::Options> gloo = nullptr;
    dpa::DeviceOptions dpa_device;
    dpa::SocketBackendOptions dpa_backend;
    size_t hint_pinned_tensor_size = 0;      // bytes
    size_t hint_pinned_tensor_pool_size = 0; // num tensors
    Options() : Backend::Options(DPA_SOCK_BACKEND_NAME) {}
  };

  ProcessGroupDPASocket(const c10d::DistributedBackendOptions &dist, const c10d::ProcessGroupDPASocket::Options &opts);

  virtual c10::intrusive_ptr<c10d::Work> allreduce(std::vector<at::Tensor> &tensors,
                                                   const AllreduceOptions &opts = AllreduceOptions()) override;
  // virtual c10::intrusive_ptr<c10d::Work> dataplane_allreduce(std::vector<at::Tensor> &tensors,
  //                                                            const DPAAllreduceOptions &opts = DPAAllreduceOptions());
  static c10::intrusive_ptr<Backend> createProcessGroupDPASocket(c10d::DistributedBackendOptions const &dist,
                                                                 c10d::ProcessGroupDPASocket::Options const &opts) {
    return c10::make_intrusive<ProcessGroupDPASocket>(dist, opts);
  }

  static c10::intrusive_ptr<Backend> createProcessGroupDPASocketStandalone(c10::intrusive_ptr<Store> store, int rank, int world_size,
                                                                           c10d::ProcessGroupDPASocket::Options const &pg_options) {
    c10d::DistributedBackendOptions opts;
    opts.store = std::move(store);
    opts.group_rank = rank;
    opts.group_size = world_size;
    opts.timeout = kBackendDefaultTimeout;
    return c10::make_intrusive<ProcessGroupDPASocket>(opts, pg_options);
  }

private:
  ProcessGroupDPASocket::Options opts;
  std::shared_ptr<dpa::Context> dpa = nullptr;
  std::shared_ptr<PinnedMempool> pinned_pool = nullptr;
};

/// This class is instantiated when we do init_process_group("dpa-dpdk", ...) from Python
class ProcessGroupDPADpdk : public ProcessGroupGloo {
public:
  friend class DPAAllReduceWork;
  /// Options for the ProcessGroupDPASocket
  class Options : public Backend::Options {
  public:
    c10::intrusive_ptr<ProcessGroupGloo::Options> gloo = nullptr;
    dpa::DeviceOptions dpa_device;
    dpa::DpdkBackendOptions dpa_backend;
    size_t hint_pinned_tensor_size = 0;      // bytes
    size_t hint_pinned_tensor_pool_size = 0; // num tensors
    Options() : Backend::Options(DPA_DPDK_BACKEND_NAME) {}
  };

  ProcessGroupDPADpdk(const c10d::DistributedBackendOptions &dist, const c10d::ProcessGroupDPADpdk::Options &opts = {});
  virtual c10::intrusive_ptr<c10d::Work> allreduce(std::vector<at::Tensor> &tensors,
                                                   const AllreduceOptions &opts = AllreduceOptions()) override;
  // virtual c10::intrusive_ptr<c10d::Work> dataplane_allreduce(std::vector<at::Tensor> &tensors,
  //                                                            const DPAAllreduceOptions &opts = DPAAllreduceOptions());
  static c10::intrusive_ptr<Backend> createProcessGroupDPADpdk(c10d::DistributedBackendOptions const &dist,
                                                               c10d::ProcessGroupDPADpdk::Options const &opts) {
    return c10::make_intrusive<ProcessGroupDPADpdk>(dist, opts);
  }
  static c10::intrusive_ptr<Backend> createProcessGroupDPADpdkStandalone(c10::intrusive_ptr<Store> store, int rank, int world_size,
                                                                         c10d::ProcessGroupDPADpdk::Options const &pg_options) {
    c10d::DistributedBackendOptions opts;
    opts.store = std::move(store);
    opts.group_rank = rank;
    opts.group_size = world_size;
    opts.timeout = kBackendDefaultTimeout;
    return c10::make_intrusive<ProcessGroupDPADpdk>(opts, pg_options);
  }

private:
  ProcessGroupDPADpdk::Options opts;
  std::shared_ptr<dpa::Context> dpa = nullptr;
  std::shared_ptr<PinnedMempool> pinned_pool = nullptr;
};

} // namespace c10d

#endif
