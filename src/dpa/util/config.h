#ifndef DPA_UTIL_CONFIG_H
#define DPA_UTIL_CONFIG_H

#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include <stdexcept>
#include <string_view>

namespace dpa {
namespace conf {

template <typename T> bool read_if_present(T &out, const nlohmann::json &root, const char *ptr_str) {
  const nlohmann::json::json_pointer ptr{ptr_str};
  if (!root.contains(ptr)) return false;
  try {
    out = root.at(ptr).get<T>();
    return true;
  } catch (const nlohmann::json::exception &e) {
    throw std::runtime_error(fmt::format("config parse error at {} (expected {}) : {}", ptr.to_string(),
                                         typeid(T).name(), std::string(e.what())));
  }
}

} // namespace conf
}

#endif