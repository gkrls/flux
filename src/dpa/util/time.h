#ifndef DPA_UTIL_TIME_H
#define DPA_UTIL_TIME_H

#include <cstdint>
#include <time.h>
#include <chrono>

namespace dpa {
///
/// Time utilities
///
namespace time {

inline struct timespec timespec_seconds(float seconds) {
  struct timespec ts;
  ts.tv_sec = (time_t)seconds;                             // Integer part (seconds)
  ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1000000000); // Fractional part (nanoseconds)
  return ts;
}
inline struct timespec timespec_millis(uint32_t millis) {
  struct timespec ts;
  ts.tv_sec = millis / 1000; // Convert ms to seconds
  ts.tv_nsec = (millis % 1000) * 1000000;
  return ts;
}
/// @brief Sleep for exact duration (nanoseconds)
inline void ns_sleep(uint64_t ns) {
  if (ns == 0) return;

  auto start = std::chrono::steady_clock::now();

  // For short durations (<30μs) just busy wait
  if (ns < 30000) {
    while (static_cast<uint64_t>((std::chrono::steady_clock::now() - start).count()) < ns) asm volatile("" ::: "memory");
    return;
  } else {
    // For longer durations, hybrid approach
    constexpr uint64_t BUSY_WAIT_WINDOW = 40000; // 40μs
    timespec req;
    req.tv_sec = 0;
    req.tv_nsec = static_cast<long>(ns - BUSY_WAIT_WINDOW);
    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
    // Busy wait only the final microseconds
    while (static_cast<uint64_t>((std::chrono::steady_clock::now() - start).count()) < ns) asm volatile("" ::: "memory");
  }
}

/// @brief Sleep until ns nanoseconds have elapsed since prev
/// @return Return a fresh timestamp
inline std::chrono::steady_clock::time_point ns_sleep(uint64_t ns, std::chrono::steady_clock::time_point prev) {
  if (ns == 0) return std::chrono::steady_clock::now();

  auto current = std::chrono::steady_clock::now();
  auto elapsed = current - prev;
  uint64_t elapsed_ns = static_cast<uint64_t>(elapsed.count());

  if (elapsed_ns >= ns) return current;

  uint64_t remaining_ns = ns - elapsed_ns;
  auto wait_until = prev + std::chrono::nanoseconds(ns);

  // For short intervals (<30μs) just busy wait
  if (remaining_ns < 30000) {
    while (std::chrono::steady_clock::now() < wait_until) {
      // asm volatile("" ::: "memory");
      asm volatile("pause" ::: "memory");
    }
  } else {
    // Sleep for most of the time using relative sleep
    constexpr uint64_t BUSY_WAIT_WINDOW = 20000; // 20μs final busy wait

    timespec req = {0, static_cast<long>(remaining_ns - BUSY_WAIT_WINDOW)};
    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);

    // Busy wait only the final microseconds
    while (std::chrono::steady_clock::now() < wait_until) {
      // asm volatile("" ::: "memory");
      asm volatile("pause" ::: "memory");
    }
  }

  return wait_until;

  // auto end = std::chrono::steady_clock::now();
  // std::cout << "Interval: " <<
  // std::chrono::duration_cast<std::chrono::nanoseconds>(end - prev).count()
  //           << "ns (target " << ns << "ns)" << std::endl;
}

inline std::chrono::steady_clock::time_point ns_sleep(std::chrono::nanoseconds ns,
                                                      std::chrono::steady_clock::time_point prev) {
  // return ns_sleep(ns.count(), prev);
  if (!ns.count()) return std::chrono::steady_clock::now();

  auto current = std::chrono::steady_clock::now();
  auto elapsed = current - prev;
  // uint64_t elapsed_ns = static_cast<uint64_t>(elapsed.count());

  if (elapsed >= ns) return current;

  auto remaining = ns - elapsed;
  auto wait_until = prev + std::chrono::nanoseconds(ns);

  // For short intervals (<30μs) just busy wait
  if (remaining.count() < 30000) {
    while (std::chrono::steady_clock::now() < wait_until) {
      // asm volatile("" ::: "memory");
      asm volatile("pause" ::: "memory");
    }
  } else {
    // Sleep for most of the time using relative sleep
    constexpr uint64_t BUSY_WAIT_WINDOW = 20000; // 20μs final busy wait

    timespec req = {0, static_cast<long>(remaining.count() - BUSY_WAIT_WINDOW)};
    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);

    // Busy wait only the final microseconds
    while (std::chrono::steady_clock::now() < wait_until) {
      // asm volatile("" ::: "memory");
      asm volatile("pause" ::: "memory");
    }
  }

  return wait_until;
}

} // namespace time

} // namespace dpa

#endif