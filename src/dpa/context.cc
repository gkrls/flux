#include "dpa/context.h"
// #include "dpa/config.h"
#include "dpa/util/error.h"
#include "dpa/util/log.h"
#include <chrono>
#include <string>
#include <string_view>

using namespace dpa;

static uint64_t getUniqueID() {
  static std::atomic<uint64_t> count_(1);
  return count_.fetch_add(1);
}

static void PrintContextInfo(Context &ctx) {
  auto &dev = ctx.device;
#if defined(__AVX512F__)
#define avx_str "512"
#elif defined(__AVX2__)
#define avx_str "2"
#else
#define avx_str "no"
#endif

  Info(std::string(100, '='));
  Info("Context: rank={} world={} scheduler={} build={} avx={} ", ctx.rank, ctx.world, ctx.usesScheduler() ? "on" : "off",
       dpa::VERSION_STRING_FULL, avx_str);
  ctx.getDevice().print(true);
  ctx.getBackend().print(true);
  Info(std::string(100, '='));
}

void Context::print() {
  WriteLn(std::string(100, '='));
  WriteLn("Context: rank={} world={} scheduler={} build={} avx={} ", rank, world, usesScheduler() ? "on" : "off",
       dpa::VERSION_STRING_FULL, avx_str);
  getDevice().print(true);
  getBackend().print(true);
  WriteLn(std::string(100, '='));
}

Context::Context(uint16_t rank, uint16_t world, DeviceOptions const &de, BackendOptions const &be)
    : dpa::Context(rank, world, true, de, be) {}

Context::Context(uint16_t rank, uint16_t world, bool use_scheduler, DeviceOptions const &de, BackendOptions const &be)
    : rank(rank), world(world), use_scheduler(use_scheduler), id(getUniqueID()), name(std::string("ctx-") + std::to_string(id)), device(de),
      state(Context::CREATED) {
  DPA_THROW_CONTEXT_IF(id > 1, this, "multiple contexts not supported yet");

  // dpa::initLogging(dpa::log::DEBUG);
  dpa::initLogging();

  // Check for DPA_SCHEDULER env var
  if (const char *env = std::getenv("DPA_SCHEDULER"); env != nullptr) {
    std::string val(env);
    std::transform(val.begin(), val.end(), val.begin(), [](unsigned char c) { return std::tolower(c); });
    if (use_scheduler and (val == "0" or val == "off")) {
      dpa::Warn("Context scheduler disabled by env DPA_SCHEDULER={}", env);
      this->use_scheduler = false;
    } else if (!use_scheduler and (val == "1" or val == "on")) {
      dpa::Warn("Context scheduler enabled by env DPA_SCHEDULER={}", env);
      this->use_scheduler = true;
    }
  }

  /// We must have at least an even number of slot in the session
  /// Backends can apply extra checks, for instance to make sure
  /// there are enough slots for all workers give the window size
  if (device.session.pool.size == 0 || (device.session.pool.size % 2) != 0)
    DPA_THROW_CONTEXT(this, "session pool size must be non-zero and even");

  if (device.session.pool.seqnums.size() != device.session.pool.size) {
    Warn("session.pool.seqnums not supplied => assuming 1,1,1...");
    device.session.pool.seqnums = std::vector<uint32_t>(device.session.pool.size, 1);
  }
  if (!device.straggle_aware) Warn("device is not straggle aware => all straggle aware options will be ignored...");
  backend = Backend::create(*this, be);
  DPA_THROW_CONTEXT_IF(!backend, this, "failed to create backend '{}'", backend->name()); // options().name);
  // PrintContextInfo(*this);
  print();
  start();
}

Context::~Context() {
  // std::cout << "DESTRUCTOR\n";
  stop();
}

void Context::start() {
  std::call_once(initFlag, [this] {
    backend->start();

    if (use_scheduler) schedulerThread = std::thread([this] { scheduler(); });

    {
      std::lock_guard<std::mutex> lock(stateMutex);
      state = Context::INITIALIZED;
      stateCv.notify_one();
    }
  });
}

void Context::stop() {
  std::call_once(finiFlag, [this] {
    DEBUG("Context '{}' stopping...", this->name);
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      state = Context::FINALIZING;
      stateCv.notify_one();

      // just empty the queue for now
      queue = std::queue<std::shared_ptr<Task>>();
    }
    backend->stop();
    {
      std::lock_guard<std::mutex> lock(waitMutex);
      waitable.clear();
    }
    if (schedulerThread.joinable()) schedulerThread.join();
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      state = Context::FINALIZED;
      stateCv.notify_one();
    }
    Info("ctx-{} killed by thread {}, {} tasks ignored", id, os::tid(), queue.size());
  });
}

void Context::scheduler() {
  std::unique_lock<std::mutex> lock(stateMutex);
  stateCv.wait(lock, [this] { return state >= Context::INITIALIZED; });

  // DEBUG("scheduler started");
  while (true) {
    stateCv.wait(lock, [this] { return !queue.empty() || state >= Context::FINALIZING; });
    // Check for termination after waking up
    if (state >= Context::FINALIZING) break;
    auto task = queue.front();
    queue.pop();
    lock.unlock();
    backend->push(task);
    lock.lock();
  }
}

void Context::schedule(std::shared_ptr<Task> task) {
  DPA_THROW_CONTEXT_IF(&task->getContext() != this, this, "submitted task {}/{} not created by this context", task->id, task->name);
  DPA_THROW_CONTEXT_IF(task->getStatus() > Task::Created, this, "submitted task {}/{} with status '{}' !!!", task->id, task->name,
                       task->getStatusString());
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    DPA_THROW_CONTEXT_IF(state >= Context::FINALIZING, this, "cannot schedule task on finalizing/finalized context");
  }
  // Mark task waitable first, to avoid task finishing really fast before we can wait on it
  {
    std::lock_guard<std::mutex> lock(waitMutex);
    DPA_THROW_CONTEXT_IF(task->getStatus() > Task::Created, this, "submitted task {}/{} with status '{}' !!!", task->id, task->name,
                         task->getStatusString());
    waitable[task->id] = task;
  }
  // Queue the task
  if (use_scheduler) {
    std::lock_guard<std::mutex> lock(stateMutex);
    // DEBUG("Scheduling task '{}'", task->name);
    queue.push(task);
    stateCv.notify_one();
  } else {
    backend->push(task);
  }
}

Task::Status Context::wait(std::shared_ptr<Task> task) {
  DPA_THROW_CONTEXT_IF(&task->getContext() != this, this, "cannot wait on task {}/{} not created by this context", task->id, task->name);
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    DPA_THROW_CONTEXT_IF(this->isFinalized(), this, "context is finalized");
  }

  auto status = task->wait();
  {
    std::lock_guard<std::mutex> lock(waitMutex);
    waitable.erase(task->id);
  }

  return status;
}

Task::Status Context::wait(std::shared_ptr<Task> task, std::chrono::milliseconds timeout) {
  DPA_THROW_CONTEXT_IF(&task->getContext() != this, this, "cannot wait on task {}/{} not created by this context", task->id, task->name);
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    DPA_THROW_CONTEXT_IF(this->isFinalized(), this, "context is finalized");
  }

  auto status = task->wait(timeout);
  if (status > Task::Running) {
    std::lock_guard<std::mutex> lock(waitMutex);
    waitable.erase(task->id);
  }

  return status;
}

int Context::waitAll() {
  std::unique_lock<std::mutex> lock(waitMutex);
  if (waitable.empty()) return 0;
  std::vector<std::shared_ptr<Task>> tasksToWait;
  tasksToWait.reserve(waitable.size());
  for (auto &[id, task] : waitable) tasksToWait.emplace_back(task);
  waitable.clear();
  lock.unlock();
  for (auto &task : tasksToWait) task->wait();
  return tasksToWait.size();
}

std::shared_ptr<Task> Context::AllReduceAsync(void *in, void *out, uint32_t size, DataType type, AllReduceOptions const &opt) {
  DPA_THROW_CONTEXT_IF(!in || !out || !size, this, "Invalid task input or size");
  // init();
  // printf("Starting allreduce on buffer in:  %p, out: %p\n", in, out);
  auto task = Task::Create(*this, in, out, size, type, true, opt);
  schedule(task);
  return task;
}

Task::Status Context::AllReduce(void *in, void *out, uint32_t size, DataType type, AllReduceOptions const &opt) {
  DPA_THROW_CONTEXT_IF(!in || !out || !size, this, "Invalid task input or size");
  // init();
  // printf("Starting allreduce on buffer in:  %p, out: %p\n", in, out);
  auto task = Task::Create(*this, in, out, size, type, opt);
  schedule(task);
  return wait(task);
}