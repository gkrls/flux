#ifndef DPA_UTIL_PP_H
#define DPA_UTIL_PP_H

#include "fmt/core.h"
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace dpa {

///
/// Pretty printing utilities
///
namespace pp {

inline std::string join(const std::vector<std::string> &parts, const std::string &delim = " ", bool quote_if_space = false) {
  std::string out;
  out.reserve(parts.size() * 8);
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += delim;
    const auto &p = parts[i];
    if (quote_if_space && p.find(' ') != std::string::npos) out += '\"' + p + '\"';
    else out += p;
  }
  return out;
}

template <typename T> std::string head(std::vector<T> const &vec, uint32_t lim = 0, bool spaced = false) {
  if (vec.empty()) return "";

  if (!lim) lim = vec.size();

  auto sep = spaced ? ", " : ",";

  std::stringstream ss;
  for(size_t i = 0; i < lim; ++i) {
    ss << vec[i];
    if (i < lim - 1) ss << sep;
  }

  if (lim < vec.size()) ss << "...";

  return ss.str();
}

template <typename T> std::string head(T const *vec, uint32_t len, uint32_t lim = 0, bool spaced = false) {
  if (!len) return "";

  if (!lim) lim = len;
  else lim = std::min(lim, len);

  auto sep = spaced ? ", " : ",";

  std::stringstream ss;
  ss << std::fixed << std::setprecision(2);
  for(size_t i = 0; i < lim; ++i) {
    ss << vec[i];
    if (i < lim - 1) ss << sep;
  }

  if (lim < len) ss << "...";

  return ss.str();
}

template <typename T> std::string tail(std::vector<T> const &vec, uint32_t lim = 0, bool spaced = false) {
  if (vec.empty()) return "";

  if (!lim) lim = vec.size();

  auto sep = spaced ? ", " : ",";

  std::stringstream ss;

  if (lim < vec.size()) ss << "...";

  for(size_t i = lim; lim < vec.size(); ++i) {
    ss << vec[i];
    if (i < vec.size() - 1) ss << sep;
  }

  return ss.str();
}

template <typename T> std::string headtail(std::vector<T> const &vec, uint32_t lim = 0, bool spaced = false) {
  if (vec.empty()) return "";

  auto llim = lim;
  auto rlim = lim;

  if (llim + rlim + 3 > vec.size()) {
    llim = vec.size();
    rlim = 0;
  }

  auto sep = spaced ? ", " : ",";

  std::stringstream ss;
  for (size_t i = 0; i < llim; ++i) {
    ss << vec[i];
    if (i < llim - 1) ss << sep;
  }

  if (rlim) ss << " ... ";

  for(size_t i = vec.size() - rlim; i < vec.size(); ++i) {
    ss << vec[i];
    if (i < vec.size() - 1) ss << sep;
  }

  return ss.str();
}

template <typename T> std::string aligned(const std::vector<T> &vec, uint32_t elementsPerLine = 8, uint32_t lim = 0, bool spaced = false) {
  if (vec.empty()) return "";

  if (!elementsPerLine) elementsPerLine = 1;
  size_t limit = lim ? std::min(lim, static_cast<uint32_t>(vec.size())) : vec.size();

  // Calculate maximum width
  size_t maxWidth = 0;
  for (size_t i = 0; i < limit; ++i) {
    std::string s;
    if constexpr (std::is_same_v<T, int>) {
      s = std::to_string(vec[i]);
    } else if constexpr (std::is_same_v<T, float>) {
      std::stringstream ss;
      ss << std::fixed << std::setprecision(2) << vec[i];
      s = ss.str();
    }
    maxWidth = std::max(maxWidth, s.size());
  }
  maxWidth += spaced ? 3 : 2; // 3 for ", " or 2 for ","

  // Build output string
  std::stringstream ss;
  ss << std::fixed << std::setprecision(2);
  auto sep = spaced ? ", " : ",";
  for (size_t i = 0; i < limit; ++i) {
    ss << std::right << std::setw(maxWidth);
    if constexpr (std::is_same_v<T, int>) {
      ss << vec[i];
    } else if constexpr (std::is_same_v<T, float>) {
      ss << vec[i];
    }
    if (i + 1 < limit && (i + 1) % elementsPerLine != 0) { ss << sep; }
    if ((i + 1) % elementsPerLine == 0 || i + 1 == vec.size() || i + 1 == limit) { ss << '\n'; }
  }
  if (limit < vec.size()) { ss << "...\n"; }

  return ss.str();
}

inline std::string formatBytes(uint64_t bytes) {
  static const char *units[] = {"B", "kB", "MB", "GB", "TB", "PB"};

  if (bytes < 1000) { return std::to_string(bytes) + " B"; }

  int unitIndex = 0;
  double size = bytes;

  while (size >= 1000.0 && unitIndex < 5) {
    size /= 1000.0;
    unitIndex++;
  }

  char buffer[16];
  if (size - static_cast<int>(size) < 0.1) {
    snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(size));
  } else {
    snprintf(buffer, sizeof(buffer), "%.1f", size);
  }

  return std::string(buffer) + " " + units[unitIndex];
}

inline std::string formatTime(std::chrono::steady_clock::time_point t) {
  auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch());
  auto now_s = std::chrono::duration_cast<std::chrono::seconds>(now_us);
  auto us = now_us.count() % 1000000;
  std::time_t now_time = now_s.count();
  std::tm *now_tm = std::localtime(&now_time);
  return fmt::format("{:02d}:{:02d}:{:02d}.{:06d}", now_tm->tm_hour, now_tm->tm_min, now_tm->tm_sec, us);
}

inline std::string formatTime(std::chrono::system_clock::time_point t) {
  auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch());
  auto now_s = std::chrono::duration_cast<std::chrono::seconds>(now_us);
  auto us = now_us.count() % 1000000;
  std::time_t now_time = now_s.count();
  std::tm *now_tm = std::localtime(&now_time);
  return fmt::format("{:02d}:{:02d}:{:02d}.{:06d}", now_tm->tm_hour, now_tm->tm_min, now_tm->tm_sec, us);
}

inline std::string formatTime(struct timespec ts) {
  int hours = (ts.tv_sec / 3600) % 24;
  int minutes = (ts.tv_sec / 60) % 60;
  int seconds = ts.tv_sec % 60;
  long microseconds = ts.tv_nsec / 1000; // Convert nanoseconds to microseconds
  return fmt::format("{:02d}:{:02d}:{:02d}.{:06d}", hours, minutes, seconds, microseconds);
}

} // namespace pp
} // namespace dpa

#endif // DPA_UTIL_PP_H