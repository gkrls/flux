#include "dpa/backend_socket/backend_socket.h"

#include "dpa/util/error.h"

using namespace dpa;
using namespace nlohmann;

SocketBackendOptions SocketBackendOptions::fromConfig(const std::string &path) {
  dpa::Info("reading SOCK backend options from config.json: {}", path);

  std::ifstream f(path);
  if (!f.is_open()) DPA_THROW("failed to open config json: {}", path)

  SocketBackendOptions opt;

  try {
    json data = json::parse(f);
    if (!data.contains("sock")) DPA_THROW("config '{}' does not contain dpa options", path);

    conf::read_if_present<std::string>(opt.iface, data, "/sock/iface");
    conf::read_if_present<uint16_t>(opt.port, data, "/sock/port");
    conf::read_if_present<uint16_t>(opt.threads, data, "/sock/threads");
    conf::read_if_present<bool>(opt.async, data, "/sock/async");
    conf::read_if_present<bool>(opt.pinned, data, "/sock/pinned");
    conf::read_if_present<uint8_t>(opt.window, data, "/sock/window");

    uint64_t timeout_us, tx_interval_us, rx_interval_us;
    if (dpa::conf::read_if_present<uint64_t>(timeout_us, data, "/sock/timeout_us"))
      opt.timeout = std::chrono::microseconds(timeout_us);
    if (dpa::conf::read_if_present<uint64_t>(tx_interval_us, data, "/sock/tx_interval_us"))
      opt.tx_interval = std::chrono::microseconds(tx_interval_us);
    if (dpa::conf::read_if_present<uint64_t>(rx_interval_us, data, "/sock/rx_interval_us"))
      opt.rx_interval = std::chrono::microseconds(rx_interval_us);

    dpa::conf::read_if_present<uint8_t>(opt.tx_burst, data, "/sock/tx_burst");
    dpa::conf::read_if_present<uint8_t>(opt.rx_burst, data, "/sock/rx_burst");
    dpa::conf::read_if_present<bool>(opt.debug_trace_packet, data, "/sock/debug_trace_packet");
    dpa::conf::read_if_present<bool>(opt.debug_trace_packet_rtx, data, "/sock/debug_trace_packet_rtx");

  } catch (const json::parse_error &e) { DPA_THROW("failed to parse config json {}: {}", path, std::string(e.what())); }

  return opt;
}

std::string SocketBackendOptions::str() const {
  return fmt::format("{}:{}, threads: {}{}, window: {}, required_slots: {}, timeout: {}us", addr, port, threads,
                     pinned ? " (pinned)" : "", window, requiredSlots(), timeout.count());
}

SocketBackend::SocketBackend(Context &ctx, SocketBackendOptions opts) : Backend(ctx), opt(std::move(opts)) {
  DPA_THROW("SocketBackend not implemented yet!");
}