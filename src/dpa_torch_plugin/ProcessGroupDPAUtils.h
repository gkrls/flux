#ifndef DPA_TORCH_PLUGIN_PROCESS_GROUP_DPA_UTILS_H
#define DPA_TORCH_PLUGIN_PROCESS_GROUP_DPA_UTILS_H

// #include "dpa/allreduce.h"
#define USE_C10D_GLOO

#include "torch/torch.h"

#include "torch/csrc/distributed/c10d/Types.hpp"

#include "dpa/dpa.h"

#include <mutex>
#include <queue>
#include <set>

namespace c10d {
namespace detail {
inline const std::unordered_map<at::ScalarType, ::dpa::AllReduceDType> dpaDTypes{{at::kFloat, ::dpa::AllReduceDType::FLOAT_32},
                                                                                 {at::kInt, ::dpa::AllReduceDType::INT_32}};
inline std::atomic<uint64_t> dpaGlobalSeqnum{0};
} // namespace detail

/// Check if the Torch dtype @param t is supported by DPA
inline bool dpa_supports_dtype(const at::ScalarType &t) noexcept { return (bool)(detail::dpaDTypes.find(t) != detail::dpaDTypes.end()); }

/// Get a DPA dtype from a gived Torch type scalar type @param t
/// Throws if the @param t is unsupported
inline const dpa::DataType dpa_dtype(const at::ScalarType &t) {
  auto it = detail::dpaDTypes.find(t);
  TORCH_CHECK(it != detail::dpaDTypes.end(), "DPA unsupported dtype: ", t, " (only float32 or int32 supported)");
  return it->second;
}

/// Create a CPU tensor with pinned memory storage that matches the layout of @param t
at::Tensor pinnedLike(const at::Tensor &t);

/// @brief Base class for pinned tensor mempools
/// A pinned mempool (pre) allocates pinned CPU tensors to avoid calling pinnedLike on the hot path
/// Implementation can range from fixed-sized reusable tensors, to full blown pinned allocators
class PinnedMempool {
public:
  virtual ~PinnedMempool() = default;
  virtual at::Tensor acquire(const at::Tensor &src) = 0;
  virtual void release(const at::Tensor &src) = 0;
};

/// @brief Simple
class SimplePinnedMempool : public PinnedMempool {
protected:
  struct Buffer {
    at::Tensor tensor; // The actual pinned tensor
    bool in_use = false;
  };
  std::vector<Buffer> buffers;
  std::mutex mutex;
  size_t buffer_size_bytes;
public:
  SimplePinnedMempool(size_t buffer_size_bytes, size_t num_buffers);
  at::Tensor acquire(const at::Tensor &src) override;
  void release(const at::Tensor &t) override;
};

class SimplePinnedMempool2 : public PinnedMempool {
private:
  struct Buffer {
    c10::Storage storage;
    bool in_use = false;
  };
  std::vector<Buffer> buffers;
  std::mutex mutex;
  size_t buffer_size_bytes;

public:
  SimplePinnedMempool2(size_t buffer_size_bytes, size_t num_buffers);
  at::Tensor acquire(const at::Tensor &src) override;
  void release(const at::Tensor &t) override;
};

} // namespace c10d

#endif