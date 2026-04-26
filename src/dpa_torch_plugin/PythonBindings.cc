#include <torch/python.h>

#include "ProcessGroupDPA.h"
#include "PythonBindings.h"
#include "PythonDocstrings.h"

#include "c10/util/intrusive_ptr.h"
#include "dpa/allreduce.h"
#include "dpa/backend.h"
#include "dpa/backend_dpdk/backend_dpdk.h"
#include "dpa/backend_socket/backend_socket.h"
#include "dpa/device.h"
#include "dpa/util/net.h"
#include "torch/csrc/distributed/c10d/Backend.hpp"
#include "torch/csrc/distributed/c10d/Types.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.doc() = docs::kModule;
  static bool module_initialized = false;
  if (module_initialized) {
    return; // Module already initialized, skip
  }
  module_initialized = true;
  // DataplaneContext
  py::class_<c10d::DataplaneContext>(m, "DataplaneContext")
      .def(py::init<uint8_t, bool, bool, uint8_t, bool, uint8_t>(), py::arg("quantization") = 0,
           py::arg("averaging") = false, py::arg("prescaled") = false, py::arg("sa_world") = 0,
           py::arg("sa_preemptive") = false, py::arg("pipes") = 0)
      .def("enter", &c10d::DataplaneContext::enter)
      .def("exit", &c10d::DataplaneContext::exit)
      .def("__enter__",
           [](c10d::DataplaneContext &self) {
             self.enter();
             return &self; // Return self for 'with' statement
           })
      .def("__exit__", [](c10d::DataplaneContext &self, py::object, py::object, py::object) {
        self.exit();
        return false;
      });

  // auto Class_DPAAllReduceOptions =
  //     py::class_<c10d::DPAAllreduceOptions, c10d::AllreduceOptions>(m, "DPAAllreduceOptions")
  //         .def(py::init([](c10d::ReduceOp reduceOp, bool asyncOp, uint8_t quantization, bool averaging, bool
  //         prescaled, uint8_t straggle,
  //                          uint8_t pipes) {
  //                c10d::DPAAllreduceOptions opts;
  //                // Set base class fields
  //                opts.reduceOp = reduceOp;
  //                opts.asyncOp = asyncOp;
  //                // Set internal dpa fields
  //                opts.dpa.quantization = quantization;
  //                opts.dpa.averaging = averaging;
  //                opts.dpa.prescaled = prescaled;
  //                opts.dpa.pipes = pipes;
  //                opts.dpa.straggle_k = straggle;
  //                return opts;
  //              }),
  //              py::arg("reduceOp") = c10d::ReduceOp::SUM, py::arg("asyncOp") = false, py::arg("quantization") = 1,
  //              py::arg("averaging") = true, py::arg("prescaled") = false, py::arg("straggle_k") = 0, py::arg("pipes")
  //              = 0)
  //         .def_readwrite("reduceOp", &c10d::DPAAllreduceOptions::reduceOp)
  //         .def_readwrite("asyncOp", &c10d::DPAAllreduceOptions::asyncOp)
  //         .def_property(
  //             "quantization", [](const c10d::DPAAllreduceOptions &self) { return self.dpa.quantization; },
  //             [](c10d::DPAAllreduceOptions &self, uint8_t value) { self.dpa.quantization = value; })
  //         .def_property(
  //             "averaging", [](const c10d::DPAAllreduceOptions &self) { return self.dpa.averaging; },
  //             [](c10d::DPAAllreduceOptions &self, bool value) { self.dpa.averaging = value; })
  //         .def_property(
  //             "prescaled", [](const c10d::DPAAllreduceOptions &self) { return self.dpa.prescaled; },
  //             [](c10d::DPAAllreduceOptions &self, bool value) { self.dpa.prescaled = value; })
  //         .def_property(
  //             "pipes", [](const c10d::DPAAllreduceOptions &self) { return self.dpa.pipes; },
  //             [](c10d::DPAAllreduceOptions &self, uint8_t value) { self.dpa.pipes = value; })
  //         .def_property(
  //             "straggle_k", [](const c10d::DPAAllreduceOptions &self) { return self.dpa.straggle_k; },
  //             [](c10d::DPAAllreduceOptions &self, uint8_t value) { self.dpa.straggle_k = value; });

  // make_dict_like(Class_DPAAllReduceOptions);

  // DPADeviceSession
  auto Class_DPADeviceSession =
      py::class_<dpa::DeviceOptions::Session>(m, "DPADeviceSession")
          .def(py::init([](uint32_t id, uint32_t pool_base, uint32_t pool_size, py::object pool_seqnums,
                           float straggle_timeout, float dropsim_ingress, float dropsim_egress) {
                 dpa::DeviceOptions::Session s;
                 s.id = id;
                 s.pool.base = pool_base;
                 s.pool.size = pool_size;
                 if (!pool_seqnums.is_none()) {
                   s.pool.seqnums = pool_seqnums.cast<std::vector<uint32_t>>();
                 } else {
                   s.pool.seqnums.assign(pool_size * 2, 0);
                 }
                 s.straggleTimeout = straggle_timeout; // Note: float, not uint32_t
                 s.dropsimIngress = dropsim_ingress;
                 s.dropsimEgress = dropsim_egress;
                 return s;
               }),
               py::arg("id") = 1, py::arg("pool_base") = 0, py::arg("pool_size") = 2,
               py::arg("pool_seqnums") = py::none(),
               py::arg("straggle_timeout") = 0.0f, // float
               py::arg("dropsim_ingress") = 0.0f, py::arg("dropsim_egress") = 0.0f)
          .def_readwrite("id", &dpa::DeviceOptions::Session::id)
          .def_readwrite("straggle_timeout", &dpa::DeviceOptions::Session::straggleTimeout)
          .def_readwrite("dropsim_ingress", &dpa::DeviceOptions::Session::dropsimIngress)
          .def_readwrite("dropsim_egress", &dpa::DeviceOptions::Session::dropsimEgress)
          .def_property(
              "pool_base", [](dpa::DeviceOptions::Session &self) { return self.pool.base; },
              [](dpa::DeviceOptions::Session &self, uint32_t v) { self.pool.base = v; })
          .def_property(
              "pool_size", [](dpa::DeviceOptions::Session &self) { return self.pool.size; },
              [](dpa::DeviceOptions::Session &self, uint32_t v) { self.pool.size = v; })
          .def_property(
              "pool_seqnums",
              [](dpa::DeviceOptions::Session &self) -> std::vector<uint32_t> & { return self.pool.seqnums; },
              [](dpa::DeviceOptions::Session &self, const std::vector<uint32_t> &v) { self.pool.seqnums = v; })
          .def("__getitem__", [](const dpa::DeviceOptions::Session &self, const std::string &key) -> py::object {
            if (key == "id") return py::cast(self.id);
            if (key == "pool_base") return py::cast(self.pool.base);
            if (key == "pool_size") return py::cast(self.pool.size);
            if (key == "pool_seqnums") return py::cast(self.pool.seqnums);
            if (key == "straggle_timeout") return py::cast(self.straggleTimeout);
            if (key == "dropsim_ingress") return py::cast(self.dropsimIngress);
            if (key == "dropsim_egress") return py::cast(self.dropsimEgress);
            throw py::key_error("'" + key + "'");
          });
  make_dict_like(Class_DPADeviceSession);

  // DPADeviceOptions
  auto Class_DPADeviceOptions =
      py::class_<dpa::DeviceOptions>(m, "DPADeviceOptions")
          .def(py::init<>()) // Add default constructor
          .def(py::init([](std::string name, uint16_t port, std::string mac, std::string ip, uint16_t pipes,
                           uint16_t exponents, // Note: uint16_t not uint8_t
                           uint16_t reducers, uint16_t reducer_mode, uint16_t slots, bool straggle_aware,
                           const dpa::DeviceOptions::Session &session) {
                 dpa::DeviceOptions o;
                 o.name = std::move(name);
                 o.port = port;
                 o.mac = dpa::net::MacAddress(mac);
                 o.ip = dpa::net::IPAddress(ip);
                 o.pipes = pipes;
                 o.exponents = exponents; // uint16_t
                 o.reducers = reducers;
                 o.reducer_mode = reducer_mode;
                 o.slots = slots;
                 o.straggle_aware = straggle_aware;
                 o.session = session;
                 return o;
               }),
               py::arg("name") = "",
               py::arg("port") = 4242, // Use actual default
               py::arg("mac") = "00:00:00:00:00:00", py::arg("ip") = "0.0.0.0",
               py::arg("pipes") = 4,        // Use actual default
               py::arg("exponents") = 2,    // Use actual default
               py::arg("reducers") = 32,    // Use actual default
               py::arg("reducer_mode") = 1, // Use actual default
               py::arg("slots") = 32768,    // Use actual default
               py::arg("straggle_aware") = true, py::arg("session") = dpa::DeviceOptions::Session{})
          .def_readwrite("name", &dpa::DeviceOptions::name)
          .def_readwrite("port", &dpa::DeviceOptions::port)
          .def_readwrite("pipes", &dpa::DeviceOptions::pipes)
          .def_readwrite("exponents", &dpa::DeviceOptions::exponents)
          .def_readwrite("reducers", &dpa::DeviceOptions::reducers)
          .def_readwrite("reducer_mode", &dpa::DeviceOptions::reducer_mode)
          .def_readwrite("slots", &dpa::DeviceOptions::slots)
          .def_readwrite("straggle_aware", &dpa::DeviceOptions::straggle_aware)
          .def_readwrite("session", &dpa::DeviceOptions::session)
          .def_property(
              "mac", [](const dpa::DeviceOptions &self) { return self.mac.str(); },
              [](dpa::DeviceOptions &self, const std::string &s) { self.mac = dpa::net::MacAddress(s); })
          .def_property(
              "ip", [](const dpa::DeviceOptions &self) { return self.ip.str(); },
              [](dpa::DeviceOptions &self, const std::string &s) { self.ip = dpa::net::IPAddress(s); })
          // Add the utility methods
          .def("values_per_pipe", &dpa::DeviceOptions::valuesPerPipe)
          .def("min_values", &dpa::DeviceOptions::minValues)
          .def("max_values", &dpa::DeviceOptions::maxValues)
          // Add static method
          .def_static("from_config", &dpa::DeviceOptions::fromConfig, py::arg("path"));
  make_dict_like(Class_DPADeviceOptions);

  // DpdkBackendOptions
  auto Class_DPADpdkBackendOptions =
      py::class_<dpa::DpdkBackendOptions>(m, "DPADpdkBackendOptions")
          .def(py::init<>())
          .def(py::init([](std::string iface, std::string addr, uint16_t port, uint16_t threads, bool async,
                           uint16_t window, bool debug_trace_packet, bool debug_trace_packet_rtx, uint64_t timeout_us,
                           float timeout_init_scaling, int64_t rx_interval_us, int64_t tx_interval_us,
                           uint16_t rx_burst, uint16_t tx_burst, uint16_t tx_attempts, uint32_t rx_ring_size,
                           uint32_t rx_pool_size, uint32_t rx_pool_cache, uint32_t tx_ring_size, uint32_t tx_pool_size,
                           uint32_t tx_pool_cache, std::string eal_port, std::string eal_iface,
                           std::vector<std::string> eal_extra_args, uint32_t profile_skip) {
                 dpa::DpdkBackendOptions o;
                 o.iface = iface;
                 o.addr = addr;
                 o.port = port;
                 o.threads = threads;
                 o.async = async;
                 o.window = window;
                 o.debug_trace_packet = debug_trace_packet;
                 o.debug_trace_packet_rtx = debug_trace_packet_rtx;
                 o.timeout = std::chrono::microseconds(timeout_us);
                 o.timeout_init_scaling = timeout_init_scaling;
                 o.rx_interval = std::chrono::microseconds(rx_interval_us);
                 o.tx_interval = std::chrono::microseconds(tx_interval_us);
                 o.rx_burst = rx_burst;
                 o.tx_burst = tx_burst;
                 o.tx_attempts = tx_attempts;
                 o.rx_ring_size = rx_ring_size;
                 o.rx_pool_size = rx_pool_size;
                 o.rx_pool_cache = rx_pool_cache;
                 o.tx_ring_size = tx_ring_size;
                 o.tx_pool_size = tx_pool_size;
                 o.tx_pool_cache = tx_pool_cache;
                 o.eal_port = eal_port;
                 o.eal_iface = eal_iface;
                 o.eal_extra_args = eal_extra_args;
                 o.profile_skip = profile_skip;
                 return o;
               }),
               py::arg("iface") = "eth0", py::arg("addr") = "42.0.0.1", py::arg("port") = 4242, py::arg("threads") = 1,
               py::arg("async") = false, py::arg("window") = 0, py::arg("debug_trace_packet") = false,
               py::arg("debug_trace_packet_rtx") = false,
               py::arg("timeout_us") = dpa::DpdkBackendOptions::DEFAULT_TIMEOUT_US, // DEFAULT_TIMEOUT_US
               py::arg("timeout_init_scaling") = dpa::DpdkBackendOptions::DEFAULT_TIMEOUT_INIT_SCALING,
               py::arg("rx_interval_us") = dpa::DpdkBackendOptions::DEFAULT_RX_INTERVAL_US, // DEFAULT_RX_INTERVAL_US
               py::arg("tx_interval_us") = dpa::DpdkBackendOptions::DEFAULT_TX_INTERVAL_US, // DEFAULT_TX_INTERVAL_US
               py::arg("rx_burst") = 1, py::arg("tx_burst") = 1, py::arg("tx_attempts") = 5,
               py::arg("rx_ring_size") = 0, py::arg("rx_pool_size") = 0, py::arg("rx_pool_cache") = 0,
               py::arg("tx_ring_size") = 0, py::arg("tx_pool_size") = 0, py::arg("tx_pool_cache") = 0,
               py::arg("eal_port") = "", py::arg("eal_iface") = "",
               py::arg("eal_extra_args") = std::vector<std::string>{}, py::arg("profile_skip") = 0)

          // Direct fields
          .def_readwrite("iface", &dpa::DpdkBackendOptions::iface)
          .def_readwrite("addr", &dpa::DpdkBackendOptions::addr)
          .def_readwrite("port", &dpa::DpdkBackendOptions::port)
          .def_readwrite("threads", &dpa::DpdkBackendOptions::threads)
          .def_readwrite("async", &dpa::DpdkBackendOptions::async)
          .def_readwrite("window", &dpa::DpdkBackendOptions::window)
          .def_readwrite("debug_trace_packet", &dpa::DpdkBackendOptions::debug_trace_packet)
          .def_readwrite("debug_trace_packet_rtx", &dpa::DpdkBackendOptions::debug_trace_packet_rtx)
          .def_readwrite("timeout_us", &dpa::DpdkBackendOptions::timeout)
          .def_readwrite("timeout_init_scaling", &dpa::DpdkBackendOptions::timeout_init_scaling)
          .def_readwrite("rx_burst", &dpa::DpdkBackendOptions::rx_burst)
          .def_readwrite("tx_burst", &dpa::DpdkBackendOptions::tx_burst)
          .def_readwrite("tx_attempts", &dpa::DpdkBackendOptions::tx_attempts)
          .def_readwrite("rx_ring_size", &dpa::DpdkBackendOptions::rx_ring_size)
          .def_readwrite("rx_pool_size", &dpa::DpdkBackendOptions::rx_pool_size)
          .def_readwrite("rx_pool_cache", &dpa::DpdkBackendOptions::rx_pool_cache)
          .def_readwrite("tx_ring_size", &dpa::DpdkBackendOptions::tx_ring_size)
          .def_readwrite("tx_pool_size", &dpa::DpdkBackendOptions::tx_pool_size)
          .def_readwrite("tx_pool_cache", &dpa::DpdkBackendOptions::tx_pool_cache)
          .def_readwrite("eal_port", &dpa::DpdkBackendOptions::eal_port)
          .def_readwrite("eal_iface", &dpa::DpdkBackendOptions::eal_iface)
          .def_readwrite("eal_extra_args", &dpa::DpdkBackendOptions::eal_extra_args)
          .def_readwrite("profile_skip", &dpa::DpdkBackendOptions::profile_skip)

          // Properties for chrono types
          .def_property(
              "timeout_us", [](const dpa::DpdkBackendOptions &self) { return self.timeout.count(); },
              [](dpa::DpdkBackendOptions &self, int64_t us) { self.timeout = std::chrono::microseconds(us); })
          .def_property(
              "rx_interval_us", [](const dpa::DpdkBackendOptions &self) { return self.rx_interval.count(); },
              [](dpa::DpdkBackendOptions &self, int64_t us) { self.rx_interval = std::chrono::microseconds(us); })
          .def_property(
              "tx_interval_us", [](const dpa::DpdkBackendOptions &self) { return self.tx_interval.count(); },
              [](dpa::DpdkBackendOptions &self, int64_t us) { self.tx_interval = std::chrono::microseconds(us); })
          .def_static("from_config", &dpa::DpdkBackendOptions::fromConfig, py::arg("path"));
  make_dict_like(Class_DPADpdkBackendOptions);

  // ProcessGroupDPADpdkOptions
  auto Class_ProcessGroupDPADpdkOptions =
      py::class_<c10d::ProcessGroupDPADpdk::Options>(m, "ProcessGroupDPADpdkOptions")
          .def(py::init<>(), "Create empty ProcessGroupDPADpdkOptions")
          .def(py::init([](const dpa::DeviceOptions &dpa_device, const dpa::DpdkBackendOptions &dpa_backend,
                           size_t hint_pinned_tensor_size = 0, size_t hint_pinned_tensor_pool_size = 0) {
                 c10d::ProcessGroupDPADpdk::Options o;
                 o.dpa_device = dpa_device;
                 o.dpa_backend = dpa_backend;
                 o.hint_pinned_tensor_size = hint_pinned_tensor_size;
                 o.hint_pinned_tensor_pool_size = hint_pinned_tensor_pool_size;
                 return o;
               }),
               py::arg("dpa_device"), py::arg("dpa_backend"), py::kw_only(), py::arg("hint_pinned_tensor_size") = 0,
               py::arg("hint_pinned_tensor_pool_size") = 0)
          .def_readwrite("dpa_device", &c10d::ProcessGroupDPADpdk::Options::dpa_device)
          .def_readwrite("dpa_backend", &c10d::ProcessGroupDPADpdk::Options::dpa_backend)
          .def_readwrite("hint_pinned_tensor_size", &c10d::ProcessGroupDPADpdk::Options::hint_pinned_tensor_size)
          .def_readwrite("hint_pinned_tensor_pool_size",
                         &c10d::ProcessGroupDPADpdk::Options::hint_pinned_tensor_pool_size);
  make_dict_like(Class_ProcessGroupDPADpdkOptions);

  // ProcessGroupDPADpdk
  py::class_<c10d::ProcessGroupDPADpdk, c10d::ProcessGroupGloo, c10::intrusive_ptr<c10d::ProcessGroupDPADpdk>>(
      m, "ProcessGroupDPADpdk");
  // .def("dataplane_allreduce", &c10d::ProcessGroupDPADpdk::dataplane_allreduce, py::arg("tensors"),
  //      py::arg("opts") = c10d::DPAAllreduceOptions());

  // DPASocketBackendOptions
  auto Class_DPASocketBackendOptions =
      py::class_<dpa::SocketBackendOptions>(m, "DPASocketBackendOptions") // Fix name from DPASocketBackend
          .def(py::init<>())
          .def(py::init([](std::string iface, std::string addr, uint16_t port, uint16_t threads, bool pinned,
                           bool async, uint8_t window, int64_t timeout_us, int64_t rx_interval_us,
                           int64_t tx_interval_us, uint8_t rx_burst, uint8_t tx_burst, uint8_t tx_attempts,
                           float rx_dropsim, float tx_dropsim, bool debug_trace_packet, bool debug_trace_packet_rtx) {
                 dpa::SocketBackendOptions o;
                 o.iface = iface;
                 o.addr = addr;
                 o.port = port;
                 o.threads = threads;
                 o.pinned = pinned;
                 o.async = async;
                 o.window = window;
                 o.timeout = std::chrono::microseconds(timeout_us);
                 o.rx_interval = std::chrono::microseconds(rx_interval_us);
                 o.tx_interval = std::chrono::microseconds(tx_interval_us);
                 o.rx_burst = rx_burst;
                 o.tx_burst = tx_burst;
                 o.tx_attempts = tx_attempts;
                 o.rx_dropsim = rx_dropsim;
                 o.tx_dropsim = tx_dropsim;
                 o.debug_trace_packet = debug_trace_packet;
                 o.debug_trace_packet_rtx = debug_trace_packet_rtx;
                 return o;
               }),
               py::arg("iface") = "eth0", py::arg("addr") = "42.0.0.1", py::arg("port") = 4242, py::arg("threads") = 1,
               py::arg("pinned") = false, py::arg("async") = false, py::arg("window") = 0, py::arg("timeout_us") = 2000,
               py::arg("rx_interval_us") = 50, py::arg("tx_interval_us") = 20, py::arg("rx_burst") = 1,
               py::arg("tx_burst") = 1, py::arg("tx_attempts") = 5, py::arg("rx_dropsim") = 0.0f,
               py::arg("tx_dropsim") = 0.0f, py::arg("debug_trace_packet") = false,
               py::arg("debug_trace_packet_rtx") = false)
          // ... all the def_readwrite fields ...
          .def_readwrite("iface", &dpa::SocketBackendOptions::iface)
          .def_readwrite("addr", &dpa::SocketBackendOptions::addr)
          .def_readwrite("port", &dpa::SocketBackendOptions::port)
          .def_readwrite("threads", &dpa::SocketBackendOptions::threads)
          .def_readwrite("pinned", &dpa::SocketBackendOptions::pinned)
          .def_readwrite("async", &dpa::SocketBackendOptions::async)
          .def_readwrite("window", &dpa::SocketBackendOptions::window)
          .def_readwrite("rx_burst", &dpa::SocketBackendOptions::rx_burst)
          .def_readwrite("tx_burst", &dpa::SocketBackendOptions::tx_burst)
          .def_readwrite("tx_attempts", &dpa::SocketBackendOptions::tx_attempts)
          .def_readwrite("rx_dropsim", &dpa::SocketBackendOptions::rx_dropsim)
          .def_readwrite("tx_dropsim", &dpa::SocketBackendOptions::tx_dropsim)
          .def_readwrite("debug_trace_packet", &dpa::SocketBackendOptions::debug_trace_packet)
          .def_readwrite("debug_trace_packet_rtx", &dpa::SocketBackendOptions::debug_trace_packet_rtx)
          // ... chrono properties ...
          .def_property(
              "timeout_us", [](const dpa::SocketBackendOptions &self) { return self.timeout.count(); },
              [](dpa::SocketBackendOptions &self, int64_t us) { self.timeout = std::chrono::microseconds(us); })
          .def_property(
              "rx_interval_us", [](const dpa::SocketBackendOptions &self) { return self.rx_interval.count(); },
              [](dpa::SocketBackendOptions &self, int64_t us) { self.rx_interval = std::chrono::microseconds(us); })
          .def_property(
              "tx_interval_us", [](const dpa::SocketBackendOptions &self) { return self.tx_interval.count(); },
              [](dpa::SocketBackendOptions &self, int64_t us) { self.tx_interval = std::chrono::microseconds(us); })
          // Add static method
          .def_static("from_config", &dpa::SocketBackendOptions::fromConfig, py::arg("path"));
  make_dict_like(Class_DPASocketBackendOptions);

  // ProcessGroupDPASocketOptions
  auto Class_ProcessGroupDPASocketOptions =
      py::class_<c10d::ProcessGroupDPASocket::Options>(m, "ProcessGroupDPASocketOptions")
          .def(py::init<>(), "Create empty ProcessGroupDPASocketOptions; fill dpa_device and dpa_backend afterwards")
          .def(py::init([](const dpa::DeviceOptions &dpa_device, const dpa::SocketBackendOptions &dpa_backend,
                           size_t hint_pinned_tensor_size = 0, size_t hint_pinned_tensor_pool_size = 0) {
                 c10d::ProcessGroupDPASocket::Options o;
                 o.dpa_device = dpa_device;
                 o.dpa_backend = dpa_backend;
                 o.hint_pinned_tensor_size = hint_pinned_tensor_size;
                 o.hint_pinned_tensor_pool_size = hint_pinned_tensor_pool_size;
                 return o;
               }),
               py::arg("dpa_device"), py::arg("dpa_backend"), py::kw_only(), py::arg("hint_pinned_tensor_size") = 0,
               py::arg("hint_pinned_tensor_pool_size") = 0)
          .def_readwrite("dpa_device", &c10d::ProcessGroupDPASocket::Options::dpa_device)
          .def_readwrite("dpa_backend", &c10d::ProcessGroupDPASocket::Options::dpa_backend);
  make_dict_like(Class_ProcessGroupDPASocketOptions);

  // ProcessGroupDPASocket
  py::class_<c10d::ProcessGroupDPASocket, c10d::ProcessGroupGloo, c10::intrusive_ptr<c10d::ProcessGroupDPASocket>>(
      m, "ProcessGroupDPASocket");
  // .def("dataplane_allreduce", &c10d::ProcessGroupDPASocket::dataplane_allreduce, py::arg("tensors"),
  //      py::arg("opts") = c10d::DPAAllreduceOptions());

  m.def("createProcessGroupDPADpdk", &c10d::ProcessGroupDPADpdk::createProcessGroupDPADpdk);
  m.def("createProcessGroupDPADpdkStandalone", &c10d::ProcessGroupDPADpdk::createProcessGroupDPADpdkStandalone);
  // m.def("createProcessGroupDPASocket", &c10d::ProcessGroupDPASocket::createProcessGroupDPASocket);
  m.def("createProcessGroupDPASocket", [](py::args args, py::kwargs kwargs) {
    std::cout << "\n=== DEBUG createProcessGroupDPASocket ===" << std::endl;
    std::cout << "Number of args: " << args.size() << std::endl;

    for (size_t i = 0; i < args.size(); ++i) {
      std::cout << "arg[" << i << "] type: " << py::str(args[i].get_type()) << std::endl;
    }

    std::cout << "Keyword arguments:" << std::endl;
    for (auto item : kwargs) {
      std::cout << "  " << py::str(item.first) << " = " << py::str(item.second.get_type()) << std::endl;
    }

    // Now try to call the actual function
    if (args.size() >= 2) {
      auto dist = args[0].cast<c10d::DistributedBackendOptions>();
      auto opts = args[1].cast<c10d::ProcessGroupDPASocket::Options>();
      return c10d::ProcessGroupDPASocket::createProcessGroupDPASocket(dist, opts);
    } else {
      throw std::runtime_error("Expected 2 arguments");
    }
  });
  m.def("createProcessGroupDPASocketStandalone", &c10d::ProcessGroupDPASocket::createProcessGroupDPASocketStandalone);
}

__attribute__((constructor)) static void register_dpa_process_groups() {
  py::gil_scoped_acquire acquire; // Make sure we have the GIL

  try {
    py::object module = py::module::import("torch.distributed");
    py::object register_backend = module.attr("Backend").attr("register_backend");

    // Use the debug version temporarily
    register_backend("dpa_sock", py::cpp_function(c10d::ProcessGroupDPASocket::createProcessGroupDPASocket),
                     py::arg("extended_api") = true,
#ifdef DPA_CUDA
                     py::arg("devices") = py::make_tuple("cpu", "cuda")
#else
                     py::arg("devices") = py::make_tuple("cpu")
#endif
    );

    // Keep the original dpdk one as is for now
    register_backend("dpa_dpdk", py::cpp_function(c10d::ProcessGroupDPADpdk::createProcessGroupDPADpdk),
                     py::arg("extended_api") = true,
#ifdef DPA_CUDA
                     py::arg("devices") = py::make_tuple("cpu", "cuda")
#else
                     py::arg("devices") = py::make_tuple("cpu")
#endif
    );
  } catch (const py::error_already_set &e) { std::cerr << "Error registering backends: " << e.what() << std::endl; }
}

// __attribute__((constructor)) void register_dpa_process_groups() {
//   py::gil_scoped_acquire acquire;
//   py::object module = py::module::import("torch.distributed");
//   py::object register_backend = module.attr("Backend").attr("register_backend");

//   register_backend("dpa_sock", py::cpp_function(c10d::ProcessGroupDPASocket::createProcessGroupDPASocket),
//   py::arg("extended_api") = true,
// #ifdef DPA_CUDA
//                    py::arg("devices") = py::make_tuple("cpu", "cuda")
// #else
//                    py::arg("devices") = py::make_tuple("cpu")
// #endif
//   );

//   register_backend("dpa_dpdk", py::cpp_function(c10d::ProcessGroupDPADpdk::createProcessGroupDPADpdk),
//   py::arg("extended_api") = true,
// #ifdef DPA_CUDA
//                    py::arg("devices") = py::make_tuple("cpu", "cuda")
// #else
//                    py::arg("devices") = py::make_tuple("cpu")
// #endif
//   );
// }