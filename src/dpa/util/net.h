#ifndef DPA_UTIL_NET_H
#define DPA_UTIL_NET_H

#include "fmt/format.h"
#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace dpa {

namespace net {
class MacAddress {
private:
  std::array<uint8_t, 6> bytes_;
  std::string str_;

public:
  MacAddress() : MacAddress("00:00:00:00:00:00") {}
  MacAddress(const char *address) : MacAddress(std::string(address)) {}
  MacAddress(std::string const &address) : str_(address) {
    if (!MacAddress::parse(address, this->bytes_)) throw std::invalid_argument(address + " is not a valid MAC address");
  }
  std::string const &str() const { return str_; }
  std::array<uint8_t, 6> const &bytes() { return this->bytes_; };

public:
  static inline bool parse(std::string const &str, std::array<uint8_t, 6> &bytes) {
    std::stringstream ss(str);
    std::string byteStr;
    size_t i = 0;
    std::array<uint8_t, 6> tmp;
    while (std::getline(ss, byteStr, ':') && i < 6) {
      auto val = std::stoul(byteStr, nullptr, 16); // Parse as hex
      if (val < 0 || val > 255) break;
      tmp[i++] = static_cast<uint8_t>(val);
    }
    if (i == 6) bytes = tmp;
    return i == 6;
  }

  static inline std::string tostring(const uint64_t addr) {

    std::string mac_str = "", hex;

    hex = fmt::format("{x}", ((addr & ((uint64_t)0xFF << 40)) >> 40));
    mac_str += hex.substr(sizeof(uint64_t) * 2 - 2, 2);

    for (int i = 4; i >= 0; i++) {

      hex = fmt::format("{x}", (addr & ((uint64_t)0xFF << 8 * i)) >> 8 * i);
      mac_str += ":" + hex.substr(sizeof(uint64_t) * 2 - 2, 2);
    }

    return mac_str;
  }

  static inline bool parse(std::string const &str, MacAddress &addr) { return MacAddress::parse(str, addr.bytes_); }
};

class IPAddress {
private:
  std::array<uint8_t, 4> bytes_;
  std::string str_;

public:
  IPAddress() : IPAddress("0.0.0.0") {}
  IPAddress(const char *address) : IPAddress(std::string(address)) {}
  IPAddress(std::string const &address) : str_(address) {
    if (!IPAddress::parse(address, this->bytes_)) throw std::invalid_argument(address + " is not a valid IPv4 address");
  }
  std::string const &str() const { return str_; }
  std::array<uint8_t, 4> const &bytes() { return this->bytes_; }

public:
  static inline bool parse(std::string const &str, std::array<uint8_t, 4> &bytes) {
    std::stringstream ss(str);
    std::string byteStr;
    size_t i = 0;
    std::array<uint8_t, 4> tmp;
    while (std::getline(ss, byteStr, '.') && i < 4) {
      int val = std::stoi(byteStr);
      if (val < 0 || val > 255) break;
      tmp[i++] = static_cast<uint8_t>(val);
    }
    if (i == 4) bytes = tmp;
    return i == 4;
  }
  static inline bool parse(std::string const &str, IPAddress &addr) { return IPAddress::parse(str, addr.bytes_); }
  static inline std::string toString(std::array<uint8_t, 4> const &bytes) {
    std::stringstream ss;
    for (size_t i = 0; i < bytes.size(); ++i) {
      ss << (int)bytes[i];
      if (i < bytes.size() - 1) ss << ".";
    }
    return ss.str();
  }
};

static inline std::string iface_for_ipv4(const std::string &ip_str, bool require_up = true, bool allow_loopback = true) {
  in_addr want{};
  if (inet_pton(AF_INET, ip_str.c_str(), &want) != 1) return "";

  ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) return "";

  std::string result;
  for (auto *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;

    unsigned flags = ifa->ifa_flags;
    if (require_up && !(flags & IFF_UP)) continue;
    if (!allow_loopback && (flags & IFF_LOOPBACK)) continue;

    auto *sin = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
    if (std::memcmp(&sin->sin_addr, &want, sizeof(in_addr)) == 0) {
      result = ifa->ifa_name; // e.g. "eth0", "en0", "lo0"
      break;
    }
  }
  freeifaddrs(ifaddr);
  return result;
}

static inline std::string ipv4_for_iface(const std::string &ifname) {
  ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) return "";

  std::string out;
  for (auto *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (std::strcmp(ifa->ifa_name, ifname.c_str()) != 0) continue;

    char buf[INET_ADDRSTRLEN] = {0};
    auto *sin = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
    if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
      out = buf;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return out;
}

static inline bool iface_exists(const std::string &ifname) { return !ifname.empty() && if_nametoindex(ifname.c_str()) != 0; }

} // namespace net
} // namespace dpa

#endif // DPA_UTIL_NET_H