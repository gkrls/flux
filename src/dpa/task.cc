#include "dpa/task.h"
#include "dpa/context.h"
#include "dpa/util/config.h"
#include "dpa/util/log.h"
#include "fmt/core.h"

#include "dpa/util.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <ratio>
#include <unordered_map>

using namespace dpa;

static uint64_t getUniqueID() {
  static std::atomic<uint64_t> count_(1);
  return count_.fetch_add(1);
}

std::string_view Task::getStatusString(Task::Status status) {
  switch (status) {
  default: return "Created";
  case Task::Running: return "Running";
  case Task::Aborted: return "Aborted";
  case Task::Submitted: return "Submitted";
  case Task::Completed: return "Completed";
  case Task::Failed: return "Failed";
  }
}

static DataType computeOutType(DataType type, AllReduceOptions const &opt) {
  if (type == DataType::INT_32 || type == DataType::UINT_32) {
    if (opt.sa_world && opt.averaging) return FLOAT_32;
    else if (opt.averaging && !opt.prescaled) return FLOAT_32;
  }
  return type;
}

/// Here we make sure that the task is configured correctly by checking its options and throwing if needed
/// Ideally we would have the corresponding backend do its checks because we could have different backends
/// supporting different things, but for now doing a big check here is fine
Task::Task(Context &ctx, void *in, void *out, uint64_t len, DataType type, bool async, AllReduceOptions const &opts)
    : id(getUniqueID()), name("allreduce." + std::to_string(id)), ctx(ctx), in(in), out(out), len(len), size(len * 4), async(async),
      type(type), opt(opts), stats{}, status(Task::Created) {

  DPA_THROW_IF(isPrescaled() && !isAveraged(), "requested task with prescaled input but no averaging");

  if (isStraggleAware()) {
    DPA_THROW_IF(opt.sa_world > ctx.world, "requested straggle.k ({}) is larger than the world size ({})", opt.sa_world, ctx.world);
    DPA_THROW_IF(!isAveraged(), "straggle-awareness only available with averaging because contribution tracking not implemented yet")
    // if (opt.straggle.k == ctx.world)
    //   dpa::Warn("requested straggle.k ({}) is equal to the world size, meaning NO straggle-awareness for task '{}'",
    //             name);
  } else {
    opt.sa_world = ctx.world;
  }

  std::string extra_info;

  if (isInteger()) {
    DPA_THROW_IF(isQuantized(), "requested quantization for int task '{}'", name);
    DPA_THROW_IF(isAveraged(), "averaging for int task '{}' changes output type and is not currently supported", name);
    DPA_THROW_IF(isStraggleAware() && isPrescaled(),
                 "straggle-awareness for prescaled int task '{}' changes output type and is not currently supported", name);
  } else {
    DPA_THROW_IF(!isQuantized(), "requested no quantization for float task '{}'", name);
    DPA_THROW_IF(opt.quantization > ctx.device.exponents,
                 "requested {}-block quantization but only {} exponents are available on device '{}'", opt.quantization,
                 ctx.device.exponents, ctx.device.name);
  }

  // We use the number of pipes requested by the user, or all the pipes
  // This wastes some space when task->size is small, but its fine for now
  if (!opt.pipes) opt.pipes = ctx.device.pipes;
  DPA_THROW_IF(opt.pipes > ctx.device.pipes, "requested pipes ({}) > device pipes ({})", opt.pipes, ctx.device.pipes);

  stats.time.create = std::chrono::steady_clock::now();

  // // Check if averaging is requested for integer inputs
  // DPA_THROW_CONTEXT_IF(type == DataType::INT_32 && opt.average, &ctx,
  //                      "Averaging is currently not supported for integer inputs");
  Debug("new-task {} [{} x {}] {} > {} {}-pipe{}{}{}{} {}", name, len, datatypeString(type), in, out, opt.pipes,
        opt.prescaled ? " prescaled" : "", opt.averaging ? " average" : "",
        opt.quantization ? fmt::format(" quant.{:d}", opt.quantization) : "",
        opt.sa_world == ctx.world ? " su" : fmt::format(" sa.{}{}", opt.sa_world, opt.sa_preemptive ? ".preemptive" : ""), extra_info);
}

bool Task::isRunning() { return status == Status::Running; }
bool Task::isFinished() { return status > Status::Running; }
bool Task::isCompleted() { return status == Status::Completed; }
bool Task::isFailed() { return status == Status::Failed; }
bool Task::isAborted() { return status == Status::Aborted; }
bool Task::isFloatingPoint() { return type == DataType::FLOAT_32; }
bool Task::isInteger() { return !isFloatingPoint(); }
bool Task::isSigned() { return type != DataType::UINT_32; }
bool Task::isUnsigned() { return type == DataType::UINT_32; }
bool Task::isPrescaled() { return opt.prescaled; }
bool Task::isQuantized() { return this->opt.quantization > 0; }
bool Task::isAveraged() { return this->opt.averaging; }
bool Task::isStraggleAware() { return this->opt.sa_world > 0 && opt.sa_world < ctx.world; }

bool Task::setCallbacks(std::function<void(Task &)> on_complete, std::function<void(Task &)> on_error,
                        std::function<void(Task &)> on_abort) {
  std::lock_guard<std::mutex> lock(statusMutex);
  if (!isFinished()) {
    on_complete_cb = std::move(on_complete);
    on_error_cb = std::move(on_error);
    on_abort_cb = std::move(on_abort);
    return true;
  }
  return false;
}

bool Task::setCompletionCallback(std::function<void(Task &)> cb) {
  std::lock_guard<std::mutex> lock(statusMutex);
  if (!isFinished()) {
    on_complete_cb = std::move(cb);
    return true;
  }
  return false;
}

bool Task::setAbortCallback(std::function<void(Task &)> cb) {
  std::lock_guard<std::mutex> lock(statusMutex);
  if (!isFinished()) {
    on_abort_cb = std::move(cb);
    return true;
  }
  return false;
}

bool Task::setErrorCallback(std::function<void(Task &)> cb) {
  std::lock_guard<std::mutex> lock(statusMutex);
  if (!isFinished()) {
    on_error_cb = std::move(cb);
    return true;
  }
  return false;
}

Task::Status Task::setStatus(Status status) {
  Status old;
  {
    std::lock_guard<std::mutex> lock(statusMutex);
    old = this->status;
    if (status == Status::Aborted && old > status) return old; // no error its fine
    // else if (status == Status::Running) { stats.time.start = std::chrono::steady_clock::now(); }
    DPA_THROW_TASK_IF(static_cast<int>(status) < static_cast<int>(old), this, "attemped invalid status transition '{}' > '{}'",
                      getStatusString(this->status), getStatusString(status));
    if (status > old) {
      if (status == Status::Running) { stats.time.start = std::chrono::steady_clock::now(); }
      this->status = status;
      if (isFinished()) {
        // DEBUG("task {} finished", name);
        if (status == Task::Completed) stats.time.finish = std::chrono::steady_clock::now();
        statusCv.notify_all();
      }
      if (isAborted()) {
        if (on_abort_cb) on_abort_cb(*this);
      } else if (isCompleted()) {
        if (on_complete_cb) on_complete_cb(*this);
      } else if (isFailed()) {
        if (on_error_cb) on_error_cb(*this);
      }
    }
  }
  // if (cb) cb(*this);
  return old;
}

Task::Status Task::abort() { return setStatus(Status::Aborted); }

Task::Status Task::wait() {
  // DEBUG("thread-{}{} waiting completion of {}", os::tid(), os::mainThread() ? " (main)" : "", name);
  // Debug(ctx, "thread {}{} waiting completion of {}-{}...", util::os::tid(),
  //       util::os::currentThreadIsMain() ? " (main)" : "", name, id);

  std::unique_lock<std::mutex> lock(statusMutex);
  statusCv.wait(lock, [this] { return this->isFinished(); });
  return status;
}

// Task::Status Task::wait(uint64_t ms) {
//   auto duration = std::chrono::duration<float, std::milli>(ms);
//   return wait(std::chrono::milliseconds());

//   std::unique_lock<std::mutex> lock(statusMutex);
//   statusCv.wait_for(lock, duration, [this] { return this->isFinished(); });
//   return status;
// }

Task::Status Task::wait(std::chrono::milliseconds ms) {
  std::unique_lock<std::mutex> lock(statusMutex);
  statusCv.wait_for(lock, ms, [this] { return this->isFinished(); });
  return status;
}

Task::Status wait(float ms);
Task::Status wait(std::chrono::milliseconds ms);

std::unordered_map<std::string, float> Task::getStats() {
  std::unordered_map<std::string, float> out;

  // Time (ms)
  float time_ms = 0.0f;
  if (stats.time.start.time_since_epoch().count() && stats.time.finish.time_since_epoch().count() &&
      stats.time.finish >= stats.time.start) {
    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(stats.time.finish - stats.time.start).count();
    time_ms = static_cast<float>(dur / 1000.0);
  }

  // Avoid zero division
  float time_s = time_ms > 0.0f ? time_ms / 1000.0f : 1e-9f;

  // Threads (may be 0 if not set yet)
  float threads = static_cast<float>(stats.perf.threads.load());

  // Elements / Bytes
  float elements = static_cast<float>(len);
  float bytes = static_cast<float>(size);

  // Throughput
  float elems_per_s = elements / time_s;
  float bytes_per_s = bytes / time_s;

  out["time_ms"] = time_ms;
  out["threads"] = threads;
  out["elements"] = elements;
  out["bytes"] = bytes;
  out["elems_per_s"] = elems_per_s;
  out["bytes_per_s"] = bytes_per_s;

  return out;
}

void Task::printStats(const std::vector<std::shared_ptr<dpa::Task>> &tasks) {
  if (tasks.empty()) {
    fmt::println("\nStatistics: (no tasks)");
    return;
  }

  std::vector<std::string> headers = {"Task", "Threads", "Elements", "Time(ms)", "Elems/s", "B/s"};
  std::vector<size_t> widths = {headers[0].size(), headers[1].size(), headers[2].size(),
                                headers[3].size(), headers[4].size(), headers[5].size()};

  uint64_t total_elems = 0, total_bytes = 0;
  double total_time_ms = 0.0;

  // Pass 1: compute widths & totals
  for (size_t i = 0; i < tasks.size(); ++i) {
    auto m = tasks[i]->getStats();
    auto get = [&](const char *k) {
      auto it = m.find(k);
      return it == m.end() ? 0.f : it->second;
    };

    float time_ms = get("time_ms");
    float threads = get("threads");
    float elems = get("elements");
    float bytes = get("bytes");
    float eps = get("elems_per_s");
    float bps = get("bytes_per_s");

    std::string c0 = std::to_string(i);
    std::string c1 = std::to_string((uint64_t)threads);
    std::string c2 = std::to_string((uint64_t)elems);
    std::string c3 = std::to_string((uint64_t)time_ms);
    std::string c4 = fmt::format("{:.2f}", eps);
    std::string c5 = fmt::format("{:.2f}", bps);

    widths[0] = std::max(widths[0], c0.size());
    widths[1] = std::max(widths[1], c1.size());
    widths[2] = std::max(widths[2], c2.size());
    widths[3] = std::max(widths[3], c3.size());
    widths[4] = std::max(widths[4], c4.size());
    widths[5] = std::max(widths[5], c5.size());

    total_elems += (uint64_t)elems;
    total_bytes += (uint64_t)bytes;
    total_time_ms += time_ms;
  }

  std::string row_fmt = fmt::format("{{:<{}}}  {{:<{}}}  {{:<{}}}  {{:<{}}}  {{:<{}}}  {{:<{}}}", widths[0], widths[1], widths[2],
                                    widths[3], widths[4], widths[5]);

  size_t total_width = 0;
  for (auto w : widths) total_width += w;
  total_width += 2 * (headers.size() - 1);
  std::string sep(total_width, '-');

  fmt::println("\nStatistics:");
  fmt::println("{}", sep);
  fmt::println(row_fmt, headers[0], headers[1], headers[2], headers[3], headers[4], headers[5]);
  fmt::println("{}", sep);

  // Pass 2: print rows
  for (size_t i = 0; i < tasks.size(); ++i) {
    auto m = tasks[i]->getStats();
    auto get = [&](const char *k) {
      auto it = m.find(k);
      return it == m.end() ? 0.f : it->second;
    };
    fmt::println(row_fmt, i, (uint64_t)get("threads"), (uint64_t)get("elements"), (uint64_t)get("time_ms"),
                 fmt::format("{:.2f}", get("elems_per_s")), fmt::format("{:.2f}", get("bytes_per_s")));
  }

  fmt::println("{}", sep);
  double avg_elems_per_s = total_time_ms > 0 ? total_elems / (total_time_ms / 1000.0) : 0.0;
  double avg_bytes_per_s = total_time_ms > 0 ? total_bytes / (total_time_ms / 1000.0) : 0.0;
  fmt::println("Total: {} elements, {:.2f} KB in {:.2f} ms", total_elems, total_bytes / 1024.0, total_time_ms);
  fmt::println("Avg throughput: {:.2f} elements/s, {:.2f} B/s", avg_elems_per_s, avg_bytes_per_s);
  fmt::println("{}", sep);
}

void Task::printStats2(const std::vector<std::shared_ptr<dpa::Task>> &tasks) {
  if (tasks.empty()) {
    fmt::println("\nStatistics: (no tasks)");
    return;
  }

  std::vector<std::string> headers = {"Task", "Threads", "Elements", "Time(ms)", "Elems/s", "B/s"};
  std::vector<size_t> widths = {4, 7, 8, 8, 7, 3}; // Header lengths

  // Compute max widths and accumulate totals
  uint64_t total_elems = 0, total_bytes = 0;
  double total_time = 0.0;
  for (size_t i = 0; i < tasks.size(); ++i) {
    const auto &t = tasks[i];
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t->stats.time.finish - t->stats.time.start).count();
    double duration_s = duration_ms > 0 ? duration_ms / 1000.0 : 1e-6;
    double throughput_elems = t->len / duration_s;
    double throughput_bytes = t->size / duration_s; // B/s

    // Update max widths
    widths[0] = std::max(widths[0], std::to_string(i).length());
    widths[1] = std::max(widths[1], std::to_string(t->stats.perf.threads.load()).length());
    widths[2] = std::max(widths[2], std::to_string(t->len).length());
    widths[3] = std::max(widths[3], std::to_string(duration_ms).length());
    widths[4] = std::max(widths[4], fmt::format("{:.2f}", throughput_elems).length());
    widths[5] = std::max(widths[5], fmt::format("{:.2f}", throughput_bytes).length());

    total_elems += t->len;
    total_bytes += t->size;
    total_time += duration_ms;
  }

  // Total table width: sum of max widths + 2 spaces between columns
  size_t total_width = 0;
  for (size_t w : widths) total_width += w;
  total_width += 2 * 5; // 2 spaces * 5 gaps
  std::string separator(total_width, '-');

  // Dynamic format string with 2 spaces between columns
  std::string fmt = fmt::format("{{:<{}}}  {{:<{}}}  {{:<{}}}  {{:<{}}}  {{:<{}}}  {{:<{}}}", widths[0], widths[1], widths[2], widths[3],
                                widths[4], widths[5]);

  // Print table
  fmt::println("\nStatistics:");
  fmt::println("{}", separator);
  fmt::println(fmt, headers[0], headers[1], headers[2], headers[3], headers[4], headers[5]);
  fmt::println("{}", separator);

  for (size_t i = 0; i < tasks.size(); ++i) {
    const auto &t = tasks[i];
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t->stats.time.finish - t->stats.time.start).count();
    double duration_s = duration_ms > 0 ? duration_ms / 1000.0 : 1e-6;
    double throughput_elems = t->len / duration_s;
    double throughput_bytes = t->size / duration_s;

    fmt::println(fmt, i, t->stats.perf.threads.load(), t->len, duration_ms, fmt::format("{:.2f}", throughput_elems),
                 fmt::format("{:.2f}", throughput_bytes));
  }

  fmt::println("{}", separator);
  fmt::println("Total: {} elements, {:.2f} KB in {:.2f} ms", total_elems, total_bytes / 1024.0, total_time);
  fmt::println("Avg throughput: {:.2f} elements/s, {:.2f} B/s", total_time > 0 ? total_elems / (total_time / 1000.0) : 0.0,
               total_time > 0 ? total_bytes / (total_time / 1000.0) : 0.0);
  fmt::println("{}", separator);
}

// bool Task::isStraggleAware() { return opt.straggle.k < ctx.world; }
