#ifndef DPA_UTIL_ERROR_H
#define DPA_UTIL_ERROR_H


#include "dpa/util/log.h"

#ifndef __FILE_NAME__
#define __FILE_NAME__ __FILE__
#endif


// Runtime error macros - for conditions that should never happen in production
// These are always enabled and will terminate the program if triggered
#define DPA_THROW(fstr, ...)                                                                                           \
  do {                                                                                                                 \
    std::lock_guard<std::mutex> lock(dpa::log::detail::mutex::errs());                                                 \
    auto fullfmt = fmt::format("dpa: [FATAL ERROR] {}:{}: {}", __FILE_NAME__, __LINE__, (fstr));                       \
    fmt::println(dpa::errs(), fullfmt __VA_OPT__(, ) __VA_ARGS__);                                                     \
    std::quick_exit(1);                                                                                                \
  } while (0);

#define DPA_THROW_IF(pred, fstr, ...)                                                                                  \
  do {                                                                                                                 \
    if ((pred)) {                                                                                                      \
      std::lock_guard<std::mutex> lock(dpa::log::detail::mutex::errs());                                               \
      auto fullfmt = fmt::format("dpa: [FATAL ERROR] {}:{}: {}", __FILE_NAME__, __LINE__, (fstr));                     \
      fmt::println(dpa::errs(), fullfmt __VA_OPT__(, ) __VA_ARGS__);                                                   \
      std::quick_exit(1);                                                                                              \
    }                                                                                                                  \
  } while (0);

// Development-time assertion macro with nice formatting
// Only enabled in debug builds (when NDEBUG is not defined)
// #ifdef NDEBUG
// #define DPA_ASSERT(pred, fstr, ...) ((void)0)
// #else
#if DPA_DEBUG
#define DPA_ASSERT(pred, fstr, ...)                                                                                    \
  do {                                                                                                                 \
    if (!(pred)) {                                                                                                     \
      std::lock_guard<std::mutex> _lock(dpa::log::detail::mutex::errs());                                              \
      auto fullfmt = fmt::format("[ASSERTION FAILED] {}:{}: {}", __FILE_NAME__, __LINE__, (fstr));                     \
      fmt::println(dpa::errs(), fullfmt __VA_OPT__(, ) __VA_ARGS__);                                                   \
      std::quick_exit(1);                                                                                              \
    }                                                                                                                  \
  } while (0);
#else
#define DPA_ASSERT(pred, fstr, ...)                                                                                    \
  do {                                                                                                                 \
  } while (0);
#endif

#endif

// Usage examples:
// 1. For runtime errors that should never happen in production:
//    DPA_THROW_IF(buffer == nullptr, "Buffer cannot be null");
//    DPA_THROW_IF(size > MAX_SIZE, "Size {} exceeds maximum {}", size,
//    MAX_SIZE);
//
// 2. For context-aware runtime errors:
//    DPA_THROW_CTX(context, buffer == nullptr, "Buffer cannot be null");
//
// 3. For development-time assertions (only enabled in debug builds):
//    DPA_ASSERT(value > 0, "Value must be positive");
//    DPA_ASSERT((index >= 0 && index < size), "Index out of bounds");
//    DPA_ASSERT(ptr != nullptr, "Pointer is null");