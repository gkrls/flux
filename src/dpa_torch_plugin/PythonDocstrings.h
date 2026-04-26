// docstrings.h
#pragma once

// Toggle docstrings at compile time (e.g., -DENABLE_PYDOC_STRINGS=ON for Debug)
#if defined(ENABLE_PYDOC_STRINGS)
#define DOC(x) x
#else
#define DOC(x) nullptr
#endif

namespace docs {

// Module
inline constexpr const char *kModule = DOC(R"pbdoc(
DPA backends for torch.distributed (socket transport).
Provides device/backend options, a ProcessGroup wrapper, and helpers.
)pbdoc");

// ---------- DPADeviceSession ----------
namespace DPADeviceSession {
inline constexpr const char *kClass = DOC(R"pbdoc(
Switch session configuration. Ideally this should be obtained through external
mechanisms, but for now it has to be provided by the user.

Notes
-----
`pool_seqnums`:
  If None, it's auto-filled with ones of length `pool_size * 2`.
)pbdoc");

inline constexpr const char *kInit = DOC(R"pbdoc(
Create a DPADeviceSession.

Parameters
----------
id : int
    Session identifier.
pool_base : int
    Base index of the session's slot pool.
pool_size : int
    Number of slots in the session's pool.
pool_seqnums : List[int] | None
    Optional sequence number for the session's pool.
straggle_timeout : int
    Amount of microseconds after the first packet for a slot, after which the switch
    assumes there are stragglers.
dropsim_ingress : float
    Probability [0,1] to drop ingress packets.
dropsim_egress : float
    Probability [0,1] to drop egress packets.
)pbdoc");

inline constexpr const char *kPoolBase = DOC("Base index of the session pool.");
inline constexpr const char *kPoolSize = DOC("Number of entries in the pool.");
inline constexpr const char *kPoolSeqnum = DOC("Mutable list of seqnums (len=pool_size*2).");
inline constexpr const char *kTimeout = DOC("Straggler timeout in microseconds.");
inline constexpr const char *kDropsIn = DOC("Ingress drop probability [0,1] (sim).");
inline constexpr const char *kDropsEg = DOC("Egress drop probability [0,1] (sim).");
} // namespace DPADeviceSession

// ---------- DPADeviceOptions ----------
namespace DPADeviceOptions {
inline constexpr const char *kClass = DOC(R"pbdoc(
Options for the DPA device (control plane + dataplane session).
)pbdoc");

inline constexpr const char *kInit = DOC(R"pbdoc(
Create DPADeviceOptions.

Parameters
----------
name : str
    Logical device name.
port : int
    Control-plane port.
mac : str
    MAC address as "aa:bb:cc:dd:ee:ff".
ip : str
    IPv4 address as "x.y.z.w".
pipes : int
    Number of dataplane pipes.
exponents : List[int] | None
    Optional per-pipe quantization exponents.
reducers : int
    Number of reducer cores.
reducer_mode : int
    Implementation-specific reducer mode.
slots : int
    Slot count for session queues.
session : DPADeviceSession
    Session/queue configuration.
)pbdoc");

inline constexpr const char *kIp = DOC("Device IPv4 address as string.");
inline constexpr const char *kMac = DOC("Device MAC address as string.");
inline constexpr const char *kName = DOC("Logical device name.");
// (…add more per-field strings if you like…)
} // namespace DPADeviceOptions

// ---------- DPASocketBackendOptions ----------
namespace DPASocketBackendOptions {
inline constexpr const char *kClass = DOC(R"pbdoc(
Transport (socket) backend options for host-side runtime.
)pbdoc");

inline constexpr const char *kInit = DOC(R"pbdoc(
Create DPASocketBackendOptions.

Parameters
----------
addr : str
    Remote IPv4 address.
port : int
    UDP/TCP port (impl-specific).
threads : int
    Worker threads.
pinned : bool
    Pin workers to CPUs.
window : int
    Sliding window size (packets).
rx_burst : int
    RX burst size.
tx_burst : int
    TX burst size.
timeout_us : int
    IO timeout in microseconds.
rx_interval_us : int
    RX polling interval in microseconds.
tx_interval_us : int
    TX pacing interval in microseconds.
log_retransmissions : bool
    Enable retransmission debug logs.
)pbdoc");

inline constexpr const char *kAddr = DOC("Remote IPv4 address as string.");
inline constexpr const char *kTimeoutUS = DOC("IO timeout (microseconds).");
inline constexpr const char *kRxInterval = DOC("RX polling interval (microseconds).");
inline constexpr const char *kTxInterval = DOC("TX pacing interval (microseconds).");
} // namespace DPASocketBackendOptions

// ---------- ProcessGroupDPASocketOptions ----------
namespace ProcessGroupDPASocketOptions {
inline constexpr const char *kClass = DOC(R"pbdoc(
ProcessGroup options for the DPA socket backend.

Includes base Backend.Options fields (e.g., timeout) and an optional
embedded Gloo options object for control operations.
)pbdoc");

inline constexpr const char *kInit = DOC(R"pbdoc(
Create ProcessGroupDPASocketOptions.

Parameters
----------
dpa_device : DPADeviceOptions
    Device configuration.
dpa_backend : DPASocketBackendOptions
    Host transport configuration.
gloo : torch._C._distributed_c10d.ProcessGroupGloo.Options | None
    Optional Gloo options object.
timeout_ms : int
    Base backend timeout in milliseconds.
)pbdoc");

inline constexpr const char *kTimeout = DOC("Backend timeout (milliseconds).");
inline constexpr const char *kBackend = DOC("Backend name (read-only).");
inline constexpr const char *kGloo = DOC("Optional Gloo options object.");
} // namespace ProcessGroupDPASocketOptions

} // namespace docs
