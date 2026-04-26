"""Base DPA switch controller — shared config, networking, utilities."""
import json
from abc import ABC, abstractmethod
from typing import List
from netaddr import IPAddress, EUI

class CLIController(ABC):
    """Base DPA Switch Controller.

    Meant to run inside a BFRT CLI session; requires a `bfrt` object.
    """

    def __init__(self, bfrt, config_path: str):
        if not config_path:
            raise ValueError("No configuration provided")
        with open(config_path, "r") as f:
            self.conf = json.load(f)

        self.straggle_aware = bool(
            self.conf["switch"]["program"].get("straggle_aware", False)
        )
        self.has_counters = self.conf["switch"]["program"]["config"].get(
            "dpa_counters", False
        )
        self.is_asic = bool(self.conf["switch"].get("asic", False))

        print("\n" + "*" * 64)
        print("Using config file:", config_path)
        print("P4 device: Tofino", self.conf["switch"]["tofino"],
              "asic" if self.is_asic else "model")
        print("P4 program version:", "dpa_sa" if self.straggle_aware else "dpa_su")
        print("P4 program config:")
        print(json.dumps(self.conf["switch"]["program"]["config"], indent=2))
        print("DPA session config:")
        if self.conf["switch"].get("sessions", {}).get("main"):
            print(json.dumps(self.conf["switch"]["sessions"]["main"], indent=2))
        else:
            print("  n/a")

        self.bfrt = bfrt
        self.P4 = self.bfrt.switch.pipe
        self.IN = self.P4.ingress
        self.EG = self.P4.egress

    # ---------- utilities ----------

    def get_port(self, fport):
        s = str(fport)
        if "/" in s:
            fp, ch = int(s.split("/")[0]), int(s.split("/")[1])
        else:
            fp, ch = int(s), 0
        return self.bfrt.port.port_hdl_info.get(
            CONN_ID=fp, CHNL_ID=ch, print_ents=False
        ).data[b"$DEV_PORT"]

    def add_port(self, port, speed, enable=True, an=False,
                 fec="BF_FEC_TYP_NONE", loopback=None):
        self.bfrt.port.port.add(
            DEV_PORT=port, SPEED=f"BF_SPEED_{speed}G", FEC=fec,
            PORT_ENABLE=enable,
            AUTO_NEGOTIATION="PM_AN_FORCE_%s" % ("ENABLE" if an else "DISABLE"),
            LOOPBACK_MODE=loopback,
        )

    def add_fport(self, fport, speed, enable=True, an=False,
                  fec="BF_FEC_TYP_NONE", loopback=None):
        self.add_port(self.get_port(fport), speed, enable, an, fec, loopback)

    def add_multicast_group(self, mgid: int, ports: List[int], name=None):
        try:
            self.bfrt.pre.node.entry(
                MULTICAST_NODE_ID=mgid, MULTICAST_RID=mgid,
                MULTICAST_LAG_ID=[], DEV_PORT=ports,
            ).push()
            self.bfrt.pre.mgid.add(
                MGID=mgid, MULTICAST_NODE_ID=[mgid],
                MULTICAST_NODE_L1_XID_VALID=[False], MULTICAST_NODE_L1_XID=[0],
            )
            suffix = f" ({name})" if name is not None else ""
            print(f"  . added multicast group {mgid} -> {ports}{suffix}")
        except Exception as e:
            print(f"  failed to add multicast group {mgid} -> {ports}", e)

    def get_port_in_pipe(self, dport, pipe):
        assert pipe < 4, "pipe cannot be >= 4"
        PORT_MASK = (1 << 7) - 1
        PIPE_MASK = (1 << 2) - 1
        return ((pipe & PIPE_MASK) << 7) | (dport & PORT_MASK)

    # ---------- shared network setup ----------

    def setup_network(self):
        print("  Cleanup")
        if self.is_asic:
            self.bfrt.port.port.clear()
            print("  . cleared device ports")

        if self.bfrt.pre.node.info(return_info=True, print_info=False)["usage"]:
            for e in self.bfrt.pre.node.get(regex=True, return_ents=True, print_ents=False):
                e.remove()
        if self.bfrt.pre.mgid.info(return_info=True, print_info=False)["usage"]:
            for e in self.bfrt.pre.mgid.get(regex=True, return_ents=True, print_ents=False):
                e.remove()

        self.IN.net.forwarding.clear()
        self.IN.net.icmp.clear()
        self.IN.net.arp.clear()
        print("  . cleared forwarding,multicast,icmp,arp tables\n")

        print("\n** NET *********************************************************\n")

        if self.is_asic:
            pipes = self.conf["switch"]["pipes"]

            print("  Forward ports")
            for fport, info in self.conf["switch"]["ports"].items():
                port = self.get_port(fport)
                rest = [(pipe, self.get_port_in_pipe(port, pipe)) for pipe in pipes[1:]]
                s = f"    [{fport}] {port} ({pipes[0]})"
                self.add_port(port, info["speed"], info["enable"], info["auto-negotiation"])
                for pipe, p in rest:
                    self.add_port(p, info["speed"], info["enable"],
                                  info["auto-negotiation"], loopback="BF_LPBK_MAC_NEAR")
                    s += f" --> {p:>3} ({pipe})"
                print(s, f" , {info['speed']}G")

            print("  Backward ports")
            for fport, info in self.conf["switch"].get("lpbk_ports", {}).items():
                port = self.get_port(fport)
                rest = [(pipe, self.get_port_in_pipe(port, pipe)) for pipe in pipes[1:]]
                default_speed = 100 if self.conf["switch"].get("tofino") == 1 else 400
                speed = info.get("speed", default_speed)
                s = f"    [{fport}] {port} ({pipes[0]})"
                self.add_port(port, speed, enable=True, fec="BF_FEC_TYP_RS",
                              loopback="BF_LPBK_MAC_NEAR")
                for pipe, p in rest:
                    self.add_port(p, speed, enable=True, fec="BF_FEC_TYP_RS",
                                  loopback="BF_LPBK_MAC_NEAR")
                    s += f" --> {p:>3} ({pipe})"
                print(s, f" , {speed}G")

        print("  Multicast")
        self.add_multicast_group(
            self.conf["switch"]["flood_mgid"],
            [self.get_port(fp) for fp in self.conf["switch"]["ports"]],
            "flood",
        )

        print("  Forwarding")
        for fport, port_cfg in self.conf["switch"]["ports"].items():
            if "host" not in port_cfg:
                print(f"  . skipped forwarding rule for port {fport}, no host")
                continue
            hostname = port_cfg["host"]
            if hostname not in self.conf["hosts"]:
                print(f"  . skipped forwarding rule for port {fport}, host not found")
                continue
            hostinfo = self.conf["hosts"][hostname]
            mac, port = EUI(hostinfo["mac"]), self.get_port(fport)
            self.IN.net.forwarding.add_with_send_to_port(dst_addr=mac, port=port)
            print(f"  . added forwarding rule {mac} -> {port}")

        print("  ARP")
        for fport, port_cfg in self.conf["switch"]["ports"].items():
            if "host" not in port_cfg:
                print(f"  . skipped arp resolution rule for port {fport}, no host")
                continue
            hostname = port_cfg["host"]
            if hostname not in self.conf["hosts"]:
                print(f"  . skipped arp resolution rule for port {fport}, host not found")
                continue
            hostinfo = self.conf["hosts"][hostname]
            mac, ip = EUI(hostinfo["mac"]), IPAddress(hostinfo["ip"])
            self.IN.net.arp.add_with_arp_resolve(dst_proto_addr=ip, mac=mac)
            print(f"  . added arp resolution rule {mac} -> {ip}")

        switch_mac = EUI(self.conf["switch"]["mac"])
        switch_ip = IPAddress(self.conf["switch"]["ip"])
        self.IN.net.arp.add_with_arp_resolve(dst_proto_addr=switch_ip, mac=switch_mac)
        print(f"  . added arp resolution rule {switch_mac} -> {switch_ip}")

        print("  ICMP")
        self.IN.net.icmp.add_with_icmp_echo_respond(dst_addr=switch_ip)
        print(f"  . added icmp entry for switch ip {switch_ip}")

    # ---------- required subclass hooks ----------

    @abstractmethod
    def setup_dpa(self): ...

    @abstractmethod
    def read_counters(self): ...