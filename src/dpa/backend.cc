#include "dpa/backend.h"
#include "dpa/backend_socket/backend_socket.h"
#include "dpa/util/error.h"

#ifdef DPA_DPDK
#include "dpa/backend_dpdk/backend_dpdk.h"
#endif

#include <memory>

using namespace dpa;

std::string BackendOptions::getBackendName() { return Backend::getName(kind); }

std::shared_ptr<Backend> Backend::create(Context &ctx, BackendOptions const &opts) {
  if (auto *socketopts = dynamic_cast<SocketBackendOptions const *>(&opts)) {
    DPA_THROW("SocketBackend currently not available. Please use DPDK");
    // return std::shared_ptr<Backend>(new SocketBackend(ctx, *socketopts)); // std::make_shared<SocketBackend>(ctx, *sockeOpts);
  }
#ifdef DPA_DPDK
  else if (auto *dpdkopts = dynamic_cast<DpdkBackendOptions const *>(&opts)) {
    return std::make_shared<DpdkBackend>(ctx, *dpdkopts);
  }
#endif
  return nullptr;
}

std::string Backend::getName(BackendKind kind) {
  switch (kind) {
  default: break;
  case SOCKET: return SocketBackend::Name;
#ifdef DPA_DPDK
  case DPDK: return DpdkBackend::Name;
#endif
  }
  return Backend::Name;
}