#ifndef DPA_UTIL_OS_H
#define DPA_UTIL_OS_H

#include "fmt/core.h"
#include <cstdint>
#include <exception>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

// #include "dpa/util/log.h"
// #include "dpa/util/error.h"
// #include "fmt/core.h"

namespace dpa {
///
/// OS utilities
///
namespace os {
inline pid_t tid() { return gettid(); }
inline pid_t pid() { return getpid(); }
inline bool mainThread() {
#ifdef __unix__
  return tid() == pid();
#else
#error "unsupported OS"
#endif
}

inline void pinCurrentThread(uint8_t core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) < 0) throw fmt::format("failed to pin to core {}", core);
}

inline std::string getenv(const char *name, std::string_view defaultValue) {
  const char *value = std::getenv(name);
  return value ? std::string(value) : std::string(defaultValue);
}

inline void addenv(const std::vector<std::string> &envs) {
  auto trim = [](const std::string &str) -> std::string {
    size_t start = str.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r\f\v");
    return str.substr(start, end - start + 1);
  };

  for (const auto &kv : envs) {
    auto pos = kv.find('=');
    if (pos == std::string::npos) continue;
    std::string key = trim(kv.substr(0, pos));
    std::string val = trim(kv.substr(pos + 1));
    if (!key.empty() && setenv(key.c_str(), val.c_str(), 1) != 0) {
      throw std::runtime_error(fmt::format("failed to set env '{}'", kv));
      // DPA_THROW("failed to set env '{}'", kv);
    } else {
      fmt::print("Added env: {}={}\n", key, val);
    }
  }
}

} // namespace os
} // namespace dpa

#endif