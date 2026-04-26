#ifndef LIB_DPA_CONTEXT_H
#define LIB_DPA_CONTEXT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <stdint.h>
#include <thread>
#include <unordered_map>

#include "dpa/allreduce.h"
#include "dpa/backend.h"
#include "dpa/device.h"
#include "dpa/task.h"
#include "dpa/util.h"

namespace dpa {

class Backend;
class BackendOptions;

class Context final {
public:
  enum State { CREATED = 1, INITIALIZED, FINALIZING, FINALIZED };
  friend class Backend;
  friend class SocketBackend;
  friend class Task;

public:
  const uint16_t rank;
  const uint16_t world;
  const uint32_t id;
  const std::string name;
  DeviceOptions device;

public:
  Context(uint16_t rank, uint16_t world, bool use_scheduler=true, DeviceOptions const& de = {}, BackendOptions const& be = {});
  Context(uint16_t rank, uint16_t world, DeviceOptions const &de, BackendOptions const &be);
  ~Context();
  void print();
  DeviceOptions const &getDevice() const { return device; }
  DeviceOptions::Session const &getDeviceSession() const { return device.session; }
  DeviceOptions::Session::Pool const &getDeviceSessionPool() const { return device.session.pool; }
  Backend const &getBackend() const { return *backend; }
  template <typename T> T const &getBackend() { return *std::dynamic_pointer_cast<T>(backend); }
  bool usesScheduler() { return use_scheduler; }
  // std::shared_ptr<Backend> getBackend() const { return backend; }
  // template <typename T> std::shared_ptr<T> getBackend() {
  //   return std::dynamic_pointer_cast<T>(backend);
  // }

  operator uint32_t() const { return id; }

  // virtual void log();

  Task::Status wait(std::shared_ptr<Task> task);
  Task::Status wait(std::shared_ptr<Task> task, std::chrono::milliseconds);
  int waitAll();

  // virtual bool isAlive() { return state < FINALIZING; }
  bool isInitialized() { return state == INITIALIZED; }
  bool isFinalized() { return state == FINALIZED; }

public:
  Task::Status AllReduce(void *in, void *out, uint32_t size, DataType type, AllReduceOptions const &opt);
  Task::Status AllReduce(void *ptr, uint32_t size, DataType type, AllReduceOptions const &opt) {
    return AllReduce(ptr, ptr, size, type, opt);
  }
  std::shared_ptr<Task> AllReduceAsync(void *in, void *out, uint32_t size, DataType type, AllReduceOptions const &opt);
  std::shared_ptr<Task> AllReduceAsync(void *ptr, uint32_t size, DataType ty, AllReduceOptions const &opt) {
    return AllReduceAsync(ptr, ptr, size, ty, opt);
  }
  std::shared_ptr<Task> AllReduceAsync(std::vector<float> &in, std::vector<float> &out, AllReduceOptions const &opt) {
    return AllReduceAsync(in.data(), out.data(), in.size(), DataType::FLOAT_32, opt);
  }
  std::shared_ptr<Task> AllReduceAsync(std::vector<float> &in, AllReduceOptions const &opt) {
    return AllReduceAsync(in.data(), in.data(), in.size(), DataType::FLOAT_32, opt);
  }
  std::shared_ptr<Task> AllReduceAsync(std::vector<uint32_t> &in, std::vector<uint32_t> &out,
                                       AllReduceOptions const &opt) {
    return AllReduceAsync(in.data(), out.data(), in.size(), DataType::UINT_32, opt);
  }
  std::shared_ptr<Task> AllReduceAsync(std::vector<uint32_t> &in, AllReduceOptions const &opt = DEFAULT_ALLREDUCE) {
    return AllReduceAsync(in.data(), in.data(), in.size(), DataType::UINT_32, opt);
  }

  std::shared_ptr<Task> AllReduceAsync(std::vector<int32_t> &in, std::vector<int32_t> &out,
                                       AllReduceOptions const &opt = DEFAULT_ALLREDUCE) {
    return AllReduceAsync(in.data(), out.data(), in.size(), DataType::INT_32, opt);
  }
  std::shared_ptr<Task> AllReduceAsync(std::vector<int32_t> &in, AllReduceOptions const &opt = DEFAULT_ALLREDUCE) {
    return AllReduceAsync(in.data(), in.data(), in.size(), DataType::INT_32, opt);
  }


private:
  /// The scheduler loop of this context, running on its own thread
  /// Ideally we should make the context accept a scheduler object, but this is
  /// fine for now.
  void start();
  void stop();
  void scheduler();
  /// Called internally to submit a task to the scheduler
  void schedule(std::shared_ptr<Task> t);

private:
  static inline AllReduceOptions DEFAULT_ALLREDUCE;
  bool use_scheduler;

protected:
  std::shared_ptr<Backend> backend = nullptr;

  std::atomic<Context::State> state;
  std::once_flag initFlag;
  std::once_flag finiFlag;

  std::thread schedulerThread;
  std::condition_variable stateCv;
  std::mutex stateMutex;
  std::mutex waitMutex;
  // std::mutex mutex;

  std::queue<std::shared_ptr<Task>> queue;
  std::unordered_map<uint64_t, std::shared_ptr<Task>> waitable;
};

} // namespace dpa

#define DPA_THROW_CONTEXT(ptr, fstr, ...)                                                                              \
  do {                                                                                                                 \
    std::lock_guard<std::mutex> lock(dpa::log::detail::mutex::errs());                                                 \
    std::string __ctx = (ptr) ? fmt::format("ctx-{}: ", static_cast<Context *>((ptr))->id) : "null";                   \
    auto fullfmt = fmt::format("dpa.FATAL: {}{}:{}: {}", __ctx, __FILE_NAME__, __LINE__, (fstr));                      \
    fmt::println(dpa::errs(), fullfmt __VA_OPT__(, ) __VA_ARGS__);                                                     \
    std::quick_exit(1);                                                                                                \
  } while (0);

#define DPA_THROW_CONTEXT_IF(pred, ptr, fstr, ...)                                                                     \
  do {                                                                                                                 \
    if (pred) {                                                                                                        \
      std::lock_guard<std::mutex> __lock(dpa::log::detail::mutex::errs());                                             \
      std::string __ctx = (ptr) ? fmt::format("ctx-{}: ", static_cast<Context *>((ptr))->id) : "null";                 \
      auto fullfmt = fmt::format("dpa.FATAL: {}{}:{}: {}", __ctx, __FILE_NAME__, __LINE__, fstr);                      \
      fmt::println(dpa::errs(), fullfmt __VA_OPT__(, ) __VA_ARGS__);                                                   \
      std::quick_exit(1);                                                                                              \
    }                                                                                                                  \
  } while (0);

#define DPA_THROW_CTX(ptr, fstr, ...) DPA_THROW_CONTEXT((ptr), (fstr), __VA_ARGS__)
#define DPA_THROW_CTX_IF(pred, ptr, fstr, ...) DPA_THROW_CONTEXT_IF((pred), (ptr), (fstr), __VA_ARGS__)

#endif
