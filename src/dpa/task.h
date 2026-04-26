#ifndef LIB_DPA_TASK_H
#define LIB_DPA_TASK_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "dpa/allreduce.h"
// #include "dpa/datatype.h"

namespace dpa {

class Context;

class Task {
public:
  struct Stats {
    struct {
      std::chrono::steady_clock::time_point create;
      std::chrono::steady_clock::time_point submit;
      std::chrono::steady_clock::time_point start;
      std::chrono::steady_clock::time_point finish;
    } time;
    struct {
      std::atomic<int> threads{0};
      std::unordered_map<uint16_t, float> throughput_per_thread;
    } perf;
  };

  using id_t = uint64_t;

  enum Status : int {
    Created = -4,   // task is created but not handled by some backend yet
    Submitted = -3, // task handled by some backend but not yet running
    Running = -2,   // task is running by a backend
    Aborted = -1,   // task is aborted by a backend
    Completed = 0,  // task completed
    Failed = 1,     // task failed
    DidNotRun = 2
  };

  friend class Context;

private:
  std::atomic<Status> status; // atomic for reads
  std::mutex statusMutex;     // mutex for writes
  std::condition_variable statusCv;
  std::function<void(Task &)> on_complete_cb; // completion callback
  std::function<void(Task &)> on_abort_cb;    // abort callback
  std::function<void(Task &)> on_error_cb;

public:
  Context &ctx;
  void *const in;
  void *const out;
  const uint64_t len = 0;
  const uint64_t size = 0;
  const DataType type;
  const uint32_t id;
  const std::string name;
  const bool async;
  AllReduceOptions opt;
  Stats stats;

public: // protected:
  static std::string_view getStatusString(Status status);

  static inline std::shared_ptr<Task> Create(Context &ctx, void *in, void *out, uint32_t len, DataType type, bool async,
                                             AllReduceOptions const &opt) {
    return std::shared_ptr<Task>(new Task(ctx, in, out, len, type, async, opt));
  }
  static inline std::shared_ptr<Task> Create(Context &ctx, void *in, void *out, uint32_t len, DataType type,
                                             AllReduceOptions const &opt) {
    return Create(ctx, in, out, len, type, false, opt);
  }

protected:
  Task(Context &ctx, void *in, void *out, uint64_t len, DataType type, bool async, AllReduceOptions const &opt);

public:
  Task() = delete;
  ~Task() = default;
  Task(Task const &) = delete;
  void operator=(Task const &) = delete;
  Task(Task &&) = delete;
  Task &operator=(Task &&) = delete;

  /// @brief  Abort the
  /// @return
  Task::Status abort();

  /// Waits indefinitely for the task to finish (Failed or Completed).
  /// @return The final status of the task (Completed or Failed)
  Task::Status wait();

  /// Wait until the task is finished or the amount of ms elapsed.
  /// Whichever comes first.
  /// @return The current status of the task.
  // Task::Status wait(uint64_t ms);
  Task::Status wait(std::chrono::milliseconds ms);

  bool isRunning();
  bool isFinished();
  bool isCompleted();
  bool isFailed();
  bool isAborted();
  bool isFloatingPoint();
  bool isInteger();
  bool isSigned();
  bool isUnsigned();
  bool isPrescaled();
  bool isQuantized();
  bool isAveraged();
  bool isStraggleAware();

  Context &getContext() { return ctx; }
  Status setStatus(Status status);
  Status getStatus() { return status; }
  std::string_view getStatusString() { return Task::getStatusString(status); }

  uint32_t getLength() { return len; }
  DataType getType() { return type; }
  std::string_view getName() const { return name; };
  uint64_t getId() const { return id; }

  void *getInput() { return in; }
  void *getOutput() { return out; }

  bool setCallbacks(std::function<void(Task &)> on_complete, std::function<void(Task &)> on_error = nullptr,
                         std::function<void(Task &)> on_abort = nullptr);
  bool setCompletionCallback(std::function<void(Task &)> cb);
  bool setAbortCallback(std::function<void(Task &)> cb);
  bool setErrorCallback(std::function<void(Task &)> cb);
  void clearCallbacks() { on_complete_cb = {}; on_abort_cb = {}; on_error_cb = {}; }

public:
  std::unordered_map<std::string, float> getStats();
  static void printStats(std::vector<std::shared_ptr<Task>> const &asks);
  static void printStats2(std::vector<std::shared_ptr<Task>> const &asks);
};

} // namespace dpa

#define DPA_THROW_TASK(ptr, fstr, ...)                                                                                 \
  do {                                                                                                                 \
    std::lock_guard<std::mutex> lock(dpa::log::detail::mutex::errs());                                                 \
    std::string task =                                                                                                 \
        (ptr) ? fmt::format("ctx-{}/{}: ", static_cast<Task *>((ptr))->getContext().id, (ptr)->getId()) : "null";      \
    auto fullfmt = fmt::format("dpa.Fatal: {}{}:{}: {}", task, __FILE_NAME__, __LINE__, (fstr));                       \
    fmt::println(dpa::errs(), fullfmt __VA_OPT__(, ) __VA_ARGS__);                                                     \
  } while (0);

#define DPA_THROW_TASK_IF(pred, ptr, fstr, ...)                                                                        \
  do {                                                                                                                 \
    if ((pred)) {                                                                                                      \
      std::lock_guard<std::mutex> lock(dpa::log::detail::mutex::errs());                                               \
      std::string task =                                                                                               \
          (ptr) ? fmt::format("ctx-{}/{}: ", static_cast<Task *>((ptr))->getContext().id, (ptr)->getId()) : "null";    \
      auto fullfmt = fmt::format("dpa.Fatal: {}{}:{}: {}", task, __FILE_NAME__, __LINE__, (fstr));                     \
      fmt::println(dpa::errs(), fullfmt __VA_OPT__(, ) __VA_ARGS__);                                                   \
    }                                                                                                                  \
  } while (0);

#endif
