#include "dpa/device.h"

#include "dpa/util/error.h"
#include "dpa/util/log.h"
#include "dpa/util/net.h"
#include "nlohmann/json.hpp"

using namespace dpa;
using json = nlohmann::json;

void DeviceOptions::print(bool detailed) const {
  WriteLn("Device : ({}) addr: {}:{} pipes: {} quants: {} reducers: {}/{} slots: {} straggle_aware: {}", name, ip.str(), port, pipes, exponents,
       reducers, reducer_mode, slots, straggle_aware);
  if (!detailed) return;
  WriteLn("         (session-{}) pool: [{},{}) seqnums: [{}] straggle_timeout: {} dropsim: {:.2f}/{:.2f} %", session.id, session.pool.base,
       session.pool.base + session.pool.size, pp::headtail(session.pool.seqnums, 4),
       straggle_aware ? fmt::format("{:.3f} ms", session.straggleTimeout) : "n/a", session.dropsimIngress, session.dropsimIngress);
  // Info("         session.{}", session.id);
  // Info("          slots_pool_alloc: {}-{}", session.pool.base, session.pool.base + session.pool.size);
  // Info("          starting_seqnums: {}", pp::headtail(session.pool.seqnums, 4));
  // Info("          straggle_timeout: {}", straggle_aware ? fmt::format("{:.2f} ms", session.straggleTimeout) : "n/a");
  // Info("          gress_drops_prob: {:.2f}/{:.2f} %", session.dropsimIngress, session.dropsimIngress);
}

DeviceOptions DeviceOptions::fromConfig(const std::string &path) {

#define get(out, jroot, path, Type)                                                                                                        \
  do {                                                                                                                                     \
    try {                                                                                                                                  \
      (out) = (jroot)path.get<Type>();                                                                                                     \
    } catch (const nlohmann::json::exception &e) {                                                                                         \
      DPA_THROW("config parse error at " #path " (expected " #Type "): {}", std::string(e.what()));                                        \
    }                                                                                                                                      \
  } while (0)

  DeviceOptions opt;
  std::ifstream f(path);
  if (!f.is_open()) DPA_THROW("failed to open config json: {}", path)

  try {
    json data = json::parse(f);

    get(opt.name, data, ["switch"]["name"], std::string);
    get(opt.mac, data, ["switch"]["mac"], std::string);
    get(opt.ip, data, ["switch"]["ip"], std::string);
    get(opt.port, data, ["switch"]["port"], uint16_t);
    get(opt.straggle_aware, data, ["switch"]["program"]["straggle_aware"], bool);
    get(opt.pipes, data, ["switch"]["program"]["config"]["dpa_pipes"], uint8_t);
    get(opt.exponents, data, ["switch"]["program"]["config"]["dpa_exponents"], uint8_t);
    get(opt.reducers, data, ["switch"]["program"]["config"]["dpa_reducers"], uint16_t);
    get(opt.reducer_mode, data, ["switch"]["program"]["config"]["dpa_reducer_mode"], uint8_t);
    get(opt.slots, data, ["switch"]["program"]["config"]["dpa_slots"], uint16_t);
    get(opt.session.id, data, ["switch"]["sessions"]["main"]["id"], uint32_t);
    get(opt.session.pool.base, data, ["switch"]["sessions"]["main"]["pool_base"], uint16_t);
    get(opt.session.pool.size, data, ["switch"]["sessions"]["main"]["pool_size"], uint16_t);
    if (opt.straggle_aware) get(opt.session.straggleTimeout, data, ["switch"]["sessions"]["main"]["straggle_timeout_ms"], float);
    get(opt.session.dropsimIngress, data, ["switch"]["sessions"]["main"]["dropsim"]["ingress"], float);
    get(opt.session.dropsimEgress, data, ["switch"]["sessions"]["main"]["dropsim"]["egress"], float);

  } catch (const json::parse_error &e) { DPA_THROW("failed to parse config json {}: {}", path, std::string(e.what())); }
  return opt;
}