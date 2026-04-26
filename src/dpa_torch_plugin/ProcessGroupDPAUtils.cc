#include "ProcessGroupDPAUtils.h"

#include "dpa/util/log.h"
#include "torch/csrc/distributed/c10d/Types.hpp"

using namespace c10d;

// static std::unordered_map<at::ScalarType, dpa::AllReduceDType> dtype_map = {{at::kFloat, dpa::AllReduceDType::FLOAT_32},
//                                                                             {at::kInt, dpa::AllReduceDType::INT_32}};

// bool c10d::dpa_supports_dtype(const at::ScalarType &t) { return (bool)(dtype_map.find(t) != dtype_map.end()); }

// dpa::AllReduceDType c10d::dpa_dtype(const at::ScalarType &t) {
//   auto it = dtype_map.find(t);
//   TORCH_CHECK(it != dtype_map.end(), "DPA unsupported dtype: ", t, " (only float32 or int32 supported)");
//   return it->second;
// }

// uint64_t c10d::dpa_seqnum() {
//   static std::atomic<uint64_t> global_seqnum_{0};
//   return ++global_seqnum_;
// }

// static at::Tensor pinnedLike(const at::Tensor &src) {
//   auto opts = src.options().device(at::kCPU).pinned_memory(true);
//   return at::empty_strided(src.sizes(), src.strides(), opts);
// }

// Implementation taken from ProcessGroupGloo
at::Tensor c10d::pinnedLike(const at::Tensor &t) {
  auto *allocator = at::detail::getCUDAHooks().getPinnedMemoryAllocator();
  auto nbytes = static_cast<int64_t>(at::detail::computeStorageNbytes(t.sizes(), t.strides(), t.dtype().itemsize()));
  c10::Storage st(c10::Storage::use_byte_size_t(), nbytes, allocator, /*resizable=*/false);
  return at::empty({0}, t.options().device(at::kCPU)).set_(st, /*storage_offset=*/0, t.sizes(), t.strides());
}

// ProcessGroupDPAUtils.cpp
SimplePinnedMempool::SimplePinnedMempool(size_t buffer_size_bytes, size_t num_buffers) : buffer_size_bytes(buffer_size_bytes) {
  buffers.reserve(num_buffers);
  // Allocate using the same method as pinnedLike
  auto *allocator = at::detail::getCUDAHooks().getPinnedMemoryAllocator();
  for (size_t i = 0; i < num_buffers; i++) {
    Buffer buf;
    c10::Storage storage(c10::Storage::use_byte_size_t(), buffer_size_bytes, allocator, /*resizable=*/false);
    // Create a tensor that owns this storage
    // Start with empty tensor, then set the storage
    buf.tensor = at::empty({0}, at::TensorOptions().device(at::kCPU));
    buf.tensor.set_(storage);
    buf.in_use = false;
    buffers.push_back(std::move(buf));
  }
}

at::Tensor SimplePinnedMempool::acquire(const at::Tensor &src) {
  size_t required_bytes = src.nbytes();

  if (required_bytes > buffer_size_bytes) { return pinnedLike(src); }

  std::lock_guard<std::mutex> lock(mutex);

  for (auto &buf : buffers) {
    if (!buf.in_use) {
      buf.in_use = true;
      // Use the same set_ method as pinnedLike. This ensures the tensor is properly configured for copy_()
      at::Tensor result = at::empty({0}, src.options().device(at::kCPU));
      result.set_(buf.tensor.storage(), /*storage_offset=*/0, src.sizes(), src.strides()); // Use the pre-allocated storage
      result.data_ptr();                                                                   // This forces internal initialization
      result.is_contiguous();                                                              // This too
      return result;
    }
  }

  return pinnedLike(src);
}

void SimplePinnedMempool::release(const at::Tensor &t) {
  void *data = t.data_ptr();
  std::lock_guard<std::mutex> lock(mutex);
  for (auto &buf : buffers) {
    // Check if this tensor uses our buffer's storage
    if (buf.tensor.storage().data_ptr().get() == data) {
      buf.in_use = false;
      return;
    }
  }
}

// Touches one byte per page of a pinned Storage to page it in (once).
// page_bytes defaults to 4096.
static inline void warmPinnedStorage(const c10::Storage &storage, size_t bytes, size_t page_bytes = 4096) {
  if (bytes == 0) return;

  // Make a byte view over the whole storage (CPU + pinned)
  auto opts = at::TensorOptions().device(at::kCPU).dtype(at::kByte).pinned_memory(true);
  at::Tensor view = at::empty({0}, opts);
  view.set_(storage, /*storage_offset=*/0,
            /*sizes=*/{static_cast<long>(bytes)},
            /*strides=*/{1});

  // Volatile prevents the compiler from optimizing the accesses away.
  volatile uint8_t *p = view.data_ptr<uint8_t>();

  // Touch one byte per page + the last byte.
  for (size_t off = 0; off < bytes; off += page_bytes) { (void)p[off]; }
  (void)p[bytes - 1];
}

SimplePinnedMempool2::SimplePinnedMempool2(size_t bytes, size_t num) : buffer_size_bytes(bytes) {
  if (bytes && num) {
    buffers.reserve(num);
    auto *allocator = at::detail::getCUDAHooks().getPinnedMemoryAllocator();
    for (size_t i = 0; i < num; ++i) {
      Buffer b;
      b.storage = c10::Storage(c10::Storage::use_byte_size_t(), buffer_size_bytes, allocator, /*resizable=*/false);
      b.in_use = false;

#if DPA_TORCH_PINNEDPOOL_PRETOUCH
      warmPinnedStorage(b.storage, buffer_size_bytes);
#endif
      buffers.push_back(std::move(b));
    }
    dpa::Info(dpa::log::Prefix("dpa.torch"), "pinned mempool enabled with {} tensors of {} bytes", num, bytes);
  } else {
    dpa::Info(dpa::log::Prefix("dpa.torch"), "pinned mempool disabled");
  }
}

at::Tensor SimplePinnedMempool2::acquire(const at::Tensor &src) {
  // how many bytes this exact size/stride needs
  const int64_t need = static_cast<int64_t>(at::detail::computeStorageNbytes(src.sizes(), src.strides(), src.dtype().itemsize()));

  // if (static_cast<size_t>(need) > buffer_size_bytes) {
  //   // fallback to single-shot pinned allocation
  //   return pinnedLike(src);
  // }

  if (static_cast<size_t>(need) <= buffer_size_bytes) {
    std::lock_guard<std::mutex> g(mutex);
    for (auto &b : buffers) {
      if (!b.in_use) {
        b.in_use = true;
        // Make a real pinned CPU tensor backed by our pinned storage
        auto opts = src.options().device(at::kCPU).pinned_memory(true);
        at::Tensor t = at::empty({0}, opts);
        t.set_(b.storage, /*storage_offset=*/0, src.sizes(), src.strides());
        TORCH_INTERNAL_ASSERT(t.is_pinned(), "pool tensor must be pinned");
        return t;
      }
    }
  }

  std::cout << "RETURNING PINNED - " << "need: " << need << " got: " << buffer_size_bytes << " !!!\n";
  // fallback to pinnedlike
  return pinnedLike(src);
}

void SimplePinnedMempool2::release(const at::Tensor &t) {
  const int64_t sz = static_cast<int64_t>(at::detail::computeStorageNbytes(t.sizes(), t.strides(), t.dtype().itemsize()));
  if (static_cast<size_t>(sz) <= buffer_size_bytes) {
    std::lock_guard<std::mutex> g(mutex);
    auto *impl = t.storage().unsafeGetStorageImpl();
    for (auto &b : buffers) {
      if (b.storage.unsafeGetStorageImpl() == impl) {
        b.in_use = false;
        return;
      }
    }
  }
}