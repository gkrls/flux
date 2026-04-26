#ifndef DPA_UTIL_LOG_H
#define DPA_UTIL_LOG_H

#include "fmt/ostream.h"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <algorithm>

// #include "dpa/util/config.h"
#include "dpa/util/os.h"

namespace dpa {

///
/// Logging
///
namespace log {

inline std::ostream &outs() { return std::cout; }
inline std::ostream &errs() { return std::cerr; }

enum Level : uint8_t {
  NONE = 0,
  ERROR = 1,
  WARNING,
  INFO,
  DEBUG,
  TRACE,
#if DPA_DEBUG
  DEFAULT = DEBUG
#else
  DEFAULT = WARNING
#endif
};

namespace detail {

inline std::atomic<dpa::log::Level> _loggingLevel = DEFAULT;
// inline std::shared_ptr<std::ofstream> _outfs = nullptr;
// inline std::shared_ptr<std::ofstream> _errfs = nullptr;

namespace mutex {
inline std::mutex &outs() {
  static std::mutex mutex;
  return mutex;
}
inline std::mutex &errs() {
  static std::mutex mutex;
  return mutex;
}

} // namespace mutex
} // namespace detail

template <typename... T> __attribute__((always_inline)) inline void Write(std::ostream &os, fmt::format_string<T...> f, T &&...args) {
  fmt::print(os, f, std::forward<T>(args)...);
}

template <typename... T> __attribute__((always_inline)) inline void WriteLine(std::ostream &os, fmt::format_string<T...> f, T &&...args) {
  fmt::println(os, f, std::forward<T>(args)...);
}

template <typename... T>
__attribute__((always_inline)) inline void WriteSync(std::ostream &os, std::mutex &mtx, fmt::format_string<T...> f, T &&...args) {
  std::lock_guard<std::mutex> lock(mtx);
  fmt::print(os, f, std::forward<T>(args)...);
}

template <typename... T>
__attribute__((always_inline)) inline void WriteLineSync(std::ostream &os, std::mutex &mtx, fmt::format_string<T...> f, T &&...args) {
  std::lock_guard<std::mutex> lock(mtx);
  fmt::println(os, f, std::forward<T>(args)...);
}

template <typename... T> __attribute__((always_inline)) inline void WriteLineSync(fmt::format_string<T...> f, T &&...args) {
  return WriteLineSync(outs(), detail::mutex::outs(), f, std::forward<T>(args)...);
}

struct Prefix {
public:
  std::string_view v;
  Prefix(std::string_view sv) : v(sv) {}
};
inline Prefix prefix(std::string_view v) { return Prefix{v}; }

} // namespace log

using Prefix = log::Prefix;

inline constexpr std::string_view kDefaultPrefix = "dpa";

inline log::Level getLoggingLevel();

inline std::ostream &outs() { return log::outs(); }
inline std::ostream &errs() { return log::errs(); }

// inline std::shared_ptr<std::ofstream> outfs() { return log::detail::_outfs; }
// inline std::shared_ptr<std::ofstream> errfs() { return log::detail::_errfs; }

// template <typename... T> __attribute__((always_inline)) inline void Error(fmt::format_string<T...> f, T &&...args) {
//   std::string message = fmt::format("dpa: [error] {}", fmt::format(f, std::forward<T>(args)...));

//   log::WriteLineSync(errs(), log::detail::mutex::errs(), message);
//   // if (log::errfs()) log::WriteLineSync(*log::errfs(), log::detail::mutex::errfs(), message);
//   std::quick_exit(1);
// }

// template <typename... T> __attribute__((always_inline)) inline void Warn(fmt::format_string<T...> f, T &&...args) {
//   if (getLoggingLevel() >= log::WARNING) {
//     std::string message = fmt::format("dpa: [warn] {}", fmt::format(f, std::forward<T>(args)...));

//     log::WriteLineSync(outs(), log::detail::mutex::errs(), message);
//     // if (log::outs()) log::WriteLineSync(*log::outfs(), log::detail::mutex::errfs(), message);
//   }
// }

template <typename... T> __attribute__((always_inline)) inline void WriteLn(fmt::format_string<T...> f, T &&...args) {
  if (getLoggingLevel() >= log::WARNING) {
    std::string message = fmt::format("dpa: {}", fmt::format(f, std::forward<T>(args)...));
    log::WriteLineSync(outs(), log::detail::mutex::outs(), message);
    // if (log::outs()) log::WriteLineSync(*log::outfs(), log::detail::mutex::errfs(), message);
  }
}

// template <typename... T> __attribute__((always_inline)) inline void Info(fmt::format_string<T...> f = "", T
// &&...args) {
//   if (getLoggingLevel() >= log::INFO) {
//     std::string message = fmt::format("dpa: {}", fmt::format(f, std::forward<T>(args)...));

//     log::WriteLineSync(outs(), log::detail::mutex::errs(), message);
//     // if (log::outfs()) log::WriteLineSync(*log::outfs(), log::detail::mutex::outfs(), "{}", message);
//   }
// }

// using namespace std::literals;

namespace detail {
template <typename... Args>
__attribute__((always_inline)) inline void LogImpl(log::Level level, std::string_view prefix, fmt::format_string<Args...> fmt,
                                                   Args &&...args) {
  if (getLoggingLevel() >= level) { // (maybe you meant DEBUG here?)
    std::string message = fmt::format("{}: {}", prefix, fmt::format(fmt, std::forward<Args>(args)...));
    log::WriteLineSync(outs(), log::detail::mutex::outs(), message);
  }
}
template <typename... Args>
__attribute__((always_inline)) inline void WarnImpl(std::string_view prefix, fmt::format_string<Args...> fmt, Args &&...args) {
  if (getLoggingLevel() >= log::WARNING) { // (maybe you meant DEBUG here?)
    std::string message = fmt::format("{}: [warn] {}", prefix, fmt::format(fmt, std::forward<Args>(args)...));
    log::WriteLineSync(outs(), log::detail::mutex::outs(), message);
  }
}
template <typename... Args>
__attribute__((always_inline)) inline void ErrorImpl(std::string_view prefix, fmt::format_string<Args...> fmt, Args &&...args) {
  // if (getLoggingLevel() >= log::WARNING) { // (maybe you meant DEBUG here?)
  std::string message = fmt::format("{}: [error] {}", prefix, fmt::format(fmt, std::forward<Args>(args)...));
  log::WriteLineSync(outs(), log::detail::mutex::outs(), message);
  // }
}
} // namespace detail

template <typename... Args>
__attribute__((always_inline)) inline void Info(log::Prefix pre, fmt::format_string<Args...> fmt, Args &&...args) {
  dpa::detail::LogImpl(log::INFO, pre.v, fmt, std::forward<Args>(args)...);
}

template <typename... Args> __attribute__((always_inline)) inline void Info(fmt::format_string<Args...> fmt, Args &&...args) {
  dpa::detail::LogImpl(log::INFO, kDefaultPrefix, fmt, std::forward<Args>(args)...);
}

__attribute__((always_inline)) inline void Info(std::string_view msg) { dpa::detail::LogImpl(log::INFO, kDefaultPrefix, "{}", msg); }

template <typename... Args>
__attribute__((always_inline)) inline void Debug(log::Prefix pre, fmt::format_string<Args...> fmt, Args &&...args) {
  dpa::detail::LogImpl(log::DEBUG, pre.v, fmt, std::forward<Args>(args)...);
}

template <typename... Args> __attribute__((always_inline)) inline void Debug(fmt::format_string<Args...> fmt, Args &&...args) {
  dpa::detail::LogImpl(log::DEBUG, kDefaultPrefix, fmt, std::forward<Args>(args)...);
}

__attribute__((always_inline)) inline void Debug(std::string_view msg) { dpa::detail::LogImpl(log::DEBUG, kDefaultPrefix, "{}", msg); }

template <typename... Args>
__attribute__((always_inline)) inline void Warn(log::Prefix pre, fmt::format_string<Args...> fmt, Args &&...args) {
  dpa::detail::WarnImpl(pre.v, fmt, std::forward<Args>(args)...);
}

template <typename... Args> __attribute__((always_inline)) inline void Warn(fmt::format_string<Args...> fmt, Args &&...args) {
  dpa::detail::WarnImpl(kDefaultPrefix, fmt, std::forward<Args>(args)...);
}

__attribute__((always_inline)) inline void Warn(std::string_view msg) { dpa::detail::WarnImpl(kDefaultPrefix, "{}", msg); }

template <typename... Args>
__attribute__((always_inline)) inline void Error(log::Prefix pre, fmt::format_string<Args...> fmt, Args &&...args) {
  dpa::detail::ErrorImpl(pre.v, fmt, std::forward<Args>(args)...);
}

template <typename... Args> __attribute__((always_inline)) inline void Error(fmt::format_string<Args...> fmt, Args &&...args) {
  dpa::detail::ErrorImpl(kDefaultPrefix, fmt, std::forward<Args>(args)...);
}

__attribute__((always_inline)) inline void Error(std::string_view msg) { dpa::detail::ErrorImpl(kDefaultPrefix, "{}", msg); }

// template <typename... T> __attribute__((always_inline)) inline void Debug(fmt::format_string<T...> f, T &&...args) {
// #if DPA_DEBUG
//   if (getLoggingLevel() >= log::DEBUG) {
//     // std::string message = fmt::format("{}. {}", os::tid(), fmt::format(f, std::forward<T>(args)...));
//     std::string message = fmt::format("dpa: {}", fmt::format(f, std::forward<T>(args)...));
//     log::WriteLineSync(outs(), log::detail::mutex::outs(), message);
//     // if (log::outfs()) log::WriteLineSync(*log::outfs(), log::detail::mutex::outfs(), message);
//   }
// #endif
// }

// template <typename... T>
// __attribute__((always_inline)) inline void Debug(uint32_t ctx, fmt::format_string<T...> f, T &&...args) {
// #if DPA_DEBUG
//   if (getLoggingLevel() >= log::DEBUG) {
//     std::string message = fmt::format("{}. ({}) {}", os::tid(), ctx, fmt::format(f, std::forward<T>(args)...));
//     log::WriteLineSync(outs(), log::detail::mutex::outs(), message);
//     // if (log::outfs()) log::WriteLineSync(*log::outfs(), log::detail::mutex::outfs(), message);
//   }
// #endif
// }

template <typename... T> __attribute__((always_inline)) inline void Trace(fmt::format_string<T...> f, T &&...args) {

  if (getLoggingLevel() >= log::TRACE) {
    std::string message = fmt::format("dpa.Trace: {}", fmt::format(f, std::forward<T>(args)...));
    log::WriteLineSync(outs(), log::detail::mutex::outs(), message);
    // if (log::outfs()) log::WriteLineSync(*log::outfs(), log::detail::mutex::outfs(), message);
  }
}

inline log::Level getLoggingLevel() { return log::detail::_loggingLevel; }
inline std::string getLoggingLevelString(log::Level level) {
  switch (level) {
  case log::ERROR: return "Error";
  case log::WARNING: return "Warning";
  case log::INFO: return "Info";
  case log::DEBUG: return "Debug";
  case log::TRACE: return "Trace";
  default: break;
  }
  return "<unknown>";
}
inline log::Level getLoggingLevelFromString(std::string level) {
  std::transform(level.begin(), level.end(), level.begin(),[](unsigned char c) { return std::tolower(c); });
  if (level == "error") return log::ERROR;
  if (level == "warn" or level == "warning") return log::WARNING;
  if (level == "info") return log::INFO;
  if (level == "debug" or level == "dbg") return log::DEBUG;
  if (level == "trace" or level == "trc") return log::TRACE;
  return log::NONE;
}

inline bool initLogging(log::Level level=log::DEFAULT, std::string const &outfile = "", std::string const &errfile = "") {
  static std::once_flag initflag;
  bool ret = false;
  std::call_once(initflag, [&] {
    // Set up logging
    log::detail::_loggingLevel = level;
    if (const char* env = std::getenv("DPA_LOG"); env != nullptr) {
      std::string value(env);
      if (auto env_level = getLoggingLevelFromString(value); env_level != log::NONE)
        log::detail::_loggingLevel = env_level;
    }
    fmt::println("dpa: Logging level set to '{}'", getLoggingLevelString(getLoggingLevel()));
    if (outfile.size() || errfile.size()) dpa::Warn("logging to files not implemented yet");
    ret = true;
  });
  return ret;
}

} // namespace dpa

#if DPA_TRACE
#define TRACE(fstr, ...)                                                                                                                   \
  do {                                                                                                                                     \
    std::string fullfmt = fmt::format("  .. {}", fmt::format(fstr, ##__VA_ARGS__));                                                        \
    dpa::log::WriteLineSync(dpa::outs(), dpa::log::detail::mutex::outs(), fullfmt);                                                        \
  } while (0);
#define TRACE_IF(condition, fstr, ...)                                                                                                     \
  do {                                                                                                                                     \
    if (condition) {                                                                                                                       \
      std::string fullfmt = fmt::format("  .. {}", fmt::format(fstr, ##__VA_ARGS__));                                                      \
      dpa::log::WriteLineSync(dpa::outs(), dpa::log::detail::mutex::outs(), fullfmt);                                                      \
    }                                                                                                                                      \
  } while (0);
#else
#define TRACE(fstr, ...)                                                                                                                   \
  do {                                                                                                                                     \
  } while (0);
#define TRACE_IF(condition, fstr, ...)                                                                                                     \
  do {                                                                                                                                     \
  } while (0);
#endif

#if DPA_DEBUG

#define DEBUG(fstr, ...)                                                                                                                   \
  do { dpa::Debug(fmt::format(fstr, ##__VA_ARGS__)); } while (0);
#define DEBUG_IF(condition, fstr, ...)                                                                                                     \
  do {                                                                                                                                     \
    if (condition) { dpa::Debug(fmt::format(fstr, ##__VA_ARGS__)); }                                                                       \
  } while (0);
#else
#define DEBUG(fstr, ...)                                                                                                                   \
  do {                                                                                                                                     \
  } while (0);
#define DEBUG_IF(condition, fstr, ...)                                                                                                     \
  do {                                                                                                                                     \
  } while (0);
#endif

#endif // DPA_UTIL_LOG_H