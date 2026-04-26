#ifndef LIB_DPA_BACKEND_H
#define LIB_DPA_BACKEND_H

// #include "dpa/backend-dpdk/backend_dpdk.h"
// #include "dpa/backend-socket/backend_socket.h"
#include "dpa/config.h"
#include "dpa/task.h"
#include <memory>
#include <stdint.h>
#include <string>


namespace dpa {

class Context;

///
/// The available DPA backens
/// Each backend should have an entry on this enum
///
enum BackendKind { SOCKET, DPDK };

///
/// Base class for all backend options
/// Each backend should provide its own options class that extends this
///
class BackendOptions {
public:
  friend class Backend;
  friend class SocketBackend;
  friend class DpdkBackend;
  friend class Context;


private:
  /// What kind of BackendOptions is this
  BackendKind kind;

public:
  /// Name of this backend
  // std::string name;

  // Options common to all backends
  
protected:
  BackendOptions() = default;
  BackendOptions(BackendKind kind) : kind(kind) {} // name(name) {}
  BackendOptions(BackendOptions &&) = default;
  BackendOptions(BackendOptions const &) = default;

public:
  virtual ~BackendOptions() = default;
  BackendOptions &operator=(const BackendOptions &) = default;
  virtual std::string str() const { return "unknown-backend-options-str"; }
  bool is(BackendKind kind) { return kind == this->kind; }
  std::string getBackendName();
};

///
/// Base abstract class for all backend implementations
/// Only the Context is meant to create and access it
///
class Backend {

public:
  enum State { Created = 1, Initialized, Finalizing, Finalized };

  inline static const std::string Name = "unknown-backend";
  friend class Context;
  friend class BackendWorker;

protected:
  Backend(Context &ctx) : context(ctx) {}
  Backend() = delete;
  Backend(Backend &&) = delete;
  Backend(Backend const &) = delete;
  virtual ~Backend() = default;
  void operator=(Backend const &) = delete;
  Backend &operator=(Backend &&) = delete;

  virtual std::string name() const = 0;

  /**
   * @brief Start the backend, creating all necessary resources
   *        After this call the backend is ready to execute tasks
   */
  virtual void start() = 0;
  /**
   * @brief Stop the backend, destroying resources etc
   *        After this call the backend cannot accept tasks
   */
  virtual void stop() = 0;
  /**
   * Submit a task to the backend for execution
   * This is a non-blocking call that should return immediatelly
   * Task status is handled by the Task object itself
   */
  virtual bool push(std::shared_ptr<Task> task) = 0;
  /**
   * Backend threads working on a task notify the backend about per-thread status changes
   * For instance when a thread actually works on a task it should notify the backend with
   * Task::Running, and with Task::Completed or Task::Failed when it finishes with it.
   */
  // virtual void notify(uint16_t tid, std::shared_ptr<Task> task, Task::Status status) = 0;

public:
  /**
   * @brief Retrieve this backend's options
   */
  virtual const BackendOptions &options() const = 0;

  /**
   * @brief Print a summary about his backend to stdout
   */
  virtual void print(bool details) const = 0;

  /**
   * Create a backend instance from BackendOptions
   * If opts is a subclass of BackendOptions the apropriate backend is created and returned
   * If not, nullptr is returned, signifying an error
   */
  static std::shared_ptr<Backend> create(Context &ctx, BackendOptions const &opts);

  /**
   * Retrieve a backend's name
   */
  static std::string getName(BackendOptions opts) { return getName(opts.kind); }
  static std::string getName(BackendKind kind);

  // virtual bool prepare(std::shared_ptr<Task> task) { return true; }

public:
  Context &context;
};

///
/// Base class for backend worker threads
/// Each backend should extend this class
///
class BackendWorker {
public:
  const uint16_t tid;

protected:
  Backend &backend;
  //   std::atomic<bool> running;

  // private:
  //   std::once_flag init_flag;
  //   std::once_flag fini_flag;
  //   std::thread thread;
  //   std::mutex mutex;
  //   std::condition_variable cv;
  //   std::queue<std::shared_ptr<Task>> queue;

public:
  BackendWorker(uint16_t tid, Backend &backend) : tid(tid), backend(backend) {}
  /// Make the current thread wait for the worker to finish
  virtual void join() = 0;
  /// Start the worker. Intempotent with run exactly-once semantics
  // virtual void init(std::function<void()> handler);
  // virtual void init() { init(nullptr); }
  virtual void start() = 0;
  /// Stop the worker clearing up potential resources etc.
  /// Similar to init, intempotent with run exactly-once semantics
  // virtual void fini(std::function<void()> handler);
  // virtual void fini() { fini(nullptr); }
  virtual void stop() = 0;
  /// Push a new task to the worker's.
  virtual bool push(std::shared_ptr<Task> task) = 0;
  /// Execute a task
  /// This function should return:
  ///  -1: Error, task failed
  ///   0: No error but did no work for task
  ///   1: Worked on task and finished with success
  // virtual int exec(std::shared_ptr<Task> task) = 0;
};

} // namespace dpa

#endif
