#include "ProcessGroupDPA.h"
#include "ProcessGroupDPAWork.h"
#include "ProcessGroupDPAUtils.h"
#include "dpa/allreduce.h"
#include "dpa/util/log.h"
#include "torch/csrc/distributed/c10d/Types.hpp"

using namespace c10d;

static const auto PREFIX = dpa::log::Prefix("dpa.torch");

/////////////////////////////////////////////////////////////////
// Utilities
/////////////////////////////////////////////////////////////////

static c10::intrusive_ptr<ProcessGroupGloo::Options> prepareGlooOptions(c10::intrusive_ptr<ProcessGroupGloo::Options> opt) {
  auto opts = opt ? opt : c10d::ProcessGroupGloo::Options::create();

  if (opts->threads < 2) opts->threads = 2;
  if (opts->devices.empty()) {
    const char *ifname = std::getenv("GLOO_SOCKET_IFNAME");
    try {
      if (ifname && *ifname) {
        opts->devices.push_back(c10d::ProcessGroupGloo::createDeviceForInterface(ifname));
      } else {
        opts->devices.push_back(c10d::ProcessGroupGloo::createDefaultDevice());
      }
    } catch (const std::exception &e) {
      DPA_THROW("prepareGlooOptions failed to create Gloo device {}", e.what())
    }
  }
  return opts;
}

static bool can_use_dpa(const std::vector<at::Tensor> &tensors, const AllreduceOptions &opts, std::string &reason) {
  if (DataplaneContext::getOptions() == nullptr) {
    reason = "no active DPA context";
    return false;
  }
  if (!(opts.reduceOp == ReduceOp::SUM || opts.reduceOp == ReduceOp::AVG)) {
    reason = "unsupported reduceOp";
    return false;
  }
  if (tensors.empty()) {
    reason = "empty tensor list";
    return false;
  }
  if (!dpa_supports_dtype(tensors[0].scalar_type())) {
    reason = "unsupported dtype";
    return false;
  }
  // TODO: Add a check that scalar_type is 4-bytes, int/uint/float
  return true;
}

/////////////////////////////////////////////////////////////////
// ProcessGroupDPASocket
/////////////////////////////////////////////////////////////////

ProcessGroupDPASocket::ProcessGroupDPASocket(const c10d::DistributedBackendOptions &dist, const c10d::ProcessGroupDPASocket::Options &opts)
    : ProcessGroupGloo(dist.store, dist.group_rank, dist.group_size, prepareGlooOptions(opts.gloo)), opts(opts) {
  dpa = std::make_shared<dpa::Context>(dist.group_rank, dist.group_size, opts.dpa_device, opts.dpa_backend);
  // pinned_pool = std::make_unique<SimplePinnedMempool2>(400'000'000, 30);
#if DPA_TORCH_PINNEDPOOL
  pinned_pool = std::make_shared<SimplePinnedMempool2>(opts.hint_pinned_tensor_size, opts.hint_pinned_tensor_pool_size);
#endif
}

c10::intrusive_ptr<Work> ProcessGroupDPASocket::allreduce(std::vector<at::Tensor> &tensors, const c10d::AllreduceOptions &opts) {
  std::string reason;
  if (!can_use_dpa(tensors, opts, reason)) {
    DEBUG("dpa.torch: falling back to Gloo allreduce: {}", reason);
    return ProcessGroupGloo::allreduce(tensors, opts);
  }
  DEBUG("dpa.torch: using dataplane allreduce");

  // DPAAllreduceOptions dpa_opts;
  // dpa_opts.reduceOp = opts.reduceOp; // SUM
  // dpa_opts.dpa = *DataplaneContext::getOptions();
  // dpa_opts.asyncOp = opts.asyncOp;   // respect caller's async preference
  // if (std::dynamic_cast<const DPAAllReduceOptions*>(&opts)) {
  //     // opts is a DPAAllReduceOptions or a subclass instance
  // }
  // return dataplane_allreduce(tensors, dpa_opts);
  // return c10::make_intrusive<DPAAllReduceWork>(*dpa, tensors, opts, pinned_pool.get());
  // return c10::make_intrusive<DPAAllReduceWork>(*dpa, tensors, *DataplaneContext::getOptions(), pinned_pool);
  return DPAAllReduceWork::make(*dpa, tensors, *DataplaneContext::getOptions(), pinned_pool);
}

/////////////////////////////////////////////////////////////////
// ProcessGroupDPADpdk
/////////////////////////////////////////////////////////////////

ProcessGroupDPADpdk::ProcessGroupDPADpdk(const c10d::DistributedBackendOptions &dist, const c10d::ProcessGroupDPADpdk::Options &opts)
    : ProcessGroupGloo(dist.store, dist.group_rank, dist.group_size, prepareGlooOptions(opts.gloo)), opts(opts) {
  dpa = std::make_shared<dpa::Context>(dist.group_rank, dist.group_size, opts.dpa_device, opts.dpa_backend);
  // pinned_pool = std::make_unique<SimplePinnedMempool2>(400'000'000, 30);
#if DPA_TORCH_PINNEDPOOL
  pinned_pool = std::make_shared<SimplePinnedMempool2>(opts.hint_pinned_tensor_size, opts.hint_pinned_tensor_pool_size);
#endif
}

c10::intrusive_ptr<Work> ProcessGroupDPADpdk::allreduce(std::vector<at::Tensor> &tensors, const c10d::AllreduceOptions &opts) {
  std::string reason;
  if (!can_use_dpa(tensors, opts, reason)) {
    DEBUG("dpa.torch: falling back to Gloo allreduce: {}", reason);
    return ProcessGroupGloo::allreduce(tensors, opts);
  }

  // std::cout << "DATAPLANE ALLREDUCE CALLED BY THREAD " << std::this_thread::get_id() << '\n';

  DEBUG("dpa.torch: using dataplane allreduce");

  dpa::AllReduceOptions const &dpa_opts = *DataplaneContext::getOptions();

  // Unify AVG/SUM with dpa_opts.averaging. Make sure they match
  // Any other option missmatches will be checked by the backend
  DPA_THROW_IF(opts.reduceOp == ReduceOp::AVG && !dpa_opts.averaging, "Requested ReduceOp::AVG but DataplaneContext has no averaging");
  DPA_THROW_IF(opts.reduceOp == ReduceOp::SUM && dpa_opts.averaging, "Requested ReduceOp::SUM but DataplaneContext has averaging enabled");

  // if (opts.reduceOp == ReduceOp::AVG and dpa_opts.averaging) DPAAllreduceOptions dpa_opts;
  // dpa_opts.reduceOp = opts.reduceOp;
  // dpa_opts.dpa = *DataplaneContext::getOptions();
  
  // return dataplane_allreduce(tensors, dpa_opts);
  // return c10::make_intrusive<DPAAllReduceWork>(*dpa, tensors, dpa_opts, pinned_pool);
  return DPAAllReduceWork::make(*dpa, tensors, *DataplaneContext::getOptions(), pinned_pool);
}