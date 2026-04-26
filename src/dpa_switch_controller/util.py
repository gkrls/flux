import math
from ipaddress import IPv4Address

__all__ = [
    "IPAddress", "EUI",
    "tsc_for_ms",
    "PK_NONE", "PK_INPUT", "PK_OUTPUT_0", "PK_OUTPUT_1", "PK_SYN", "PK_BAD",
]

PK_NONE = 0x0
PK_INPUT = 0x1
PK_OUTPUT_0 = 0x2
PK_OUTPUT_1 = 0x3
PK_SYN = 0x4
PK_BAD = 0xff

def tsc_for_ms(ms, tick_bits=16, ts_bits=48):
    """Convert milliseconds to Tofino switch clock ticks."""
    assert tick_bits <= ts_bits, "tick_bits must be <= ts_bits"
    if ms == 0:
        return 0
    tick = 2 ** tick_bits
    nanos = ms * 1_000_000
    tsc = math.ceil(nanos / tick)
    max_tsc = 2 ** (ts_bits - tick_bits) - 1
    if tsc > max_tsc:
        raise ValueError(
            f"resulting tsc ({tsc}) requires more than {ts_bits - tick_bits} bits"
        )
    return tsc

# class EUI:
#     """Simple MAC address class to replace netaddr.EUI"""
#     def __init__(self, mac):
#         if isinstance(mac, str):
#             # Normalize MAC address format
#             mac = mac.replace('-', ':').replace('.', ':').lower()
#             if len(mac.split(':')) == 6:
#                 self.mac = mac
#             else:
#                 raise ValueError(f"Invalid MAC address format: {mac}")
#         else:
#             self.mac = str(mac)

#     def __str__(self):
#         return self.mac

#     def __repr__(self):
#         return f"EUI('{self.mac}')"

# class IPAddress:
#     """Simple IP address class to replace netaddr.IPAddress"""
#     def __init__(self, ip):
#         if isinstance(ip, str):
#             self.ip = IPv4Address(ip)
#         else:
#             self.ip = IPv4Address(str(ip))

#     def __str__(self):
#         return str(self.ip)

#     def __repr__(self):
#         return f"IPAddress('{self.ip}')"


class TofinoModel:
    PIPES = 4
    FPORTS = 64
    FPORTS_PER_PIPE = 16
    DPORTS_PER_PIPE = 72
    FPORT_CHANNELS = 4
    RPORTS = {
        # These are the Quad 17 ports
        # We ignore Quad 16 ports
        0: [68, 69, 70, 71],
        1: [196, 197, 198, 199],
        2: [324, 235, 326, 327],
        3: [452, 453, 454, 455]
    }
    # Quad 16 ports in pipes 0, 2 are CPU ports
    CPORTS_ETH = [64, 65, 66, 67]
    CPORTS_PCI = [320]

    @staticmethod
    def get_pipe_from_fport(fport):
        assert fport in range(1, TofinoModel.FPORTS + 1), "invalid fport"
        return (fport - 1) // TofinoModel.FPORTS_PER_PIPE

    @staticmethod
    def get_pport_from_fport(fport, channel=0):
        assert fport in range(1, TofinoModel.FPORTS + 1), "invalid fport"
        assert channel in range(TofinoModel.FPORT_CHANNELS), "invalid channel"
        return ((fport - 1) % TofinoModel.FPORTS_PER_PIPE) * TofinoModel.FPORT_CHANNELS + channel

    @staticmethod
    def get_dport_from_pport(pipe, pipe_port):
        assert pipe in range(TofinoModel.PIPES), "invalid pipe"
        assert pipe_port in range(72), "invalid pipe_port"
        return pipe * 128 + pipe_port

    @staticmethod
    def get_dport_from_fport(fport, channel=0):
        if isinstance(fport, str):
            channel = int(fport.split('/')[1])
            fport = int(fport.split('/')[0])
        assert fport in range(1, TofinoModel.FPORTS + 1), "invalid fport"
        assert channel in range(TofinoModel.FPORT_CHANNELS), "invalid channel"
        return ((TofinoModel.get_pipe_from_fport(fport) & 3) << 7) | (TofinoModel.get_pport_from_fport(fport, channel) & 0x7F)
