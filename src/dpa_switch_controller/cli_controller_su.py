"""Straggle-unaware DPA controller."""
from .cli_controller import CLIController
from netaddr import IPAddress, EUI

class CLIControllerSU(CLIController):
    """Handles logic specific to the straggle-unaware DPA program."""

    def setup_dpa(self):
        print("\n** DPA_su ******************************************************\n")

        # Clear ingress state
        self.IN.dpa.state.bitmap.bitmap.clear()
        self.IN.dpa.state.counter.counter.clear()
        for i in range(self.conf["switch"]["program"]["config"]["dpa_reducers"]):
            getattr(self.IN.dpa.reduce, f"value_{i:02d}").R.clear()
        for i in range(self.conf["switch"]["program"]["config"]["dpa_exponents"]):
            getattr(self.IN.dpa.reduce, f"exponent_{i:01d}").R.clear()

        # Clear egress state
        self.EG.dpa.sender.sender_table.clear()
        try:
            self.EG.dpa.dropsim.dropsim_thres.clear()
            self.EG.dpa.dropsim.dropsim_table.clear()
        except AttributeError as e:
            print(f"   error: {e}")

        # Clear counters (if enabled)
        try:
            self.IN.dpa.next.pkt_counter.clear()
            self.IN.dpa.state.counter.pkt_counter.clear()
            self.IN.dpa.state.counter.count_result_histo.clear()
        except Exception:
            pass

        for session_name, session_config in self.conf["switch"]["sessions"].items():
            session_id = session_config["id"]
            session_mgid = session_config["mgid"]
            switch_mac = EUI(self.conf["switch"]["mac"])
            switch_ip = IPAddress(self.conf["switch"]["ip"])

            print(f"\n  Session {session_id}")

            for fport in session_config["ports"]:
                hostname = self.conf["switch"]["ports"][fport]["host"]
                hostinfo = self.conf["hosts"][hostname]
                mac, ip = EUI(hostinfo["mac"]), IPAddress(hostinfo["ip"])
                self.EG.dpa.sender.sender_table.add_with_send(
                    egress_port=self.get_port(fport), session=session_id,
                    switch_mac=switch_mac, switch_ip=switch_ip,
                    worker_mac=mac, worker_ip=ip,
                )
                print(f"  . added sender entries for '{hostname}' {mac}/{ip}")

            self.add_multicast_group(
                session_mgid,
                [self.get_port(fp) for fp in session_config["ports"]],
                f"session '{session_name}'",
            )

            try:
                eg_drop_thres = (2 ** 16 - 1) * session_config["dropsim"]["egress"]
                self.EG.dpa.dropsim.dropsim_table.add_with_dropsim(session=1, internal_job_id=1)
                self.EG.dpa.dropsim.dropsim_thres.add(
                    REGISTER_INDEX=(session_id - 1), f1=eg_drop_thres
                )
                print(f"  . added {eg_drop_thres} egress dropsim")
            except AttributeError as e:
                print(f"  error: {e}")

    # ---------- counters ----------

    def read_counters(self):
        print()
        print("  ***************************************")
        print("  *    DPA_SU switch packet counters    *")
        print("  ***************************************")
        if not self.has_counters:
            print("  n/a")
            return
        print()
        self._read_ingress_receiver_counters()
        self._read_ingress_state_counters()
        self._read_ingress_reduce_counters()
        self._read_ingress_next_counters()

    def _read_ingress_receiver_counters(self):
        print("COUNTER: ingress.dpa.receiver")
        print("-----------------------------")
        print("  n/a\n")

    def _read_ingress_state_counters(self):
        print("COUNTER: ingress.dpa.state.counter.pkt_counter")
        print("----------------------------------------------")
        c = self.IN.dpa.state.counter

        def get(idx):
            return c.pkt_counter.get(
                COUNTER_INDEX=idx, from_hw=True, print_ents=False
            ).data[b"$COUNTER_SPEC_PKTS"]

        p0_miss, p0_first, p0_unseen, p0_seen = get(0), get(1), get(2), get(3)
        print(f" 0_first  : {p0_first}")
        print(f" 0_unseen : {p0_unseen}")
        print(f" 0_seen   : {p0_seen}")
        print(f" 0_miss   : {p0_miss}")
        print()

        print("COUNTER: ingress.dpa.state.counter.count_result_histo")
        print("-----------------------------------------------------")

        def geth(idx):
            return c.count_result_histo.get(
                COUNTER_INDEX=idx, from_hw=True, print_ents=False
            ).data[b"$COUNTER_SPEC_PKTS"]

        h0_incomp, h0_comp_now = geth(0), geth(1)
        h0_comp_earlier = geth(2)
        # NOTE: original code reused index 2 for "other" — fix to 3 if that's a real slot.
        h0_other = geth(3)
        print(f" 0_incomplete      : {h0_incomp}")
        print(f" 0_complete_now    : {h0_comp_now}")
        print(f" 0_complete_earlier: {h0_comp_earlier}")
        print(f" 0_other           : {h0_other}")
        print()

    def _read_ingress_reduce_counters(self):
        print("ingress.dpa.reduce.pkt_counter")
        print("------------------------------")
        counter = self.IN.dpa.reduce.pkt_counter
        num_counters = 8

        def get(idx):
            return counter.get(
                COUNTER_INDEX=idx, from_hw=True, print_ents=False
            ).data[b"$COUNTER_SPEC_PKTS"]

        for i in range(2):
            base = i * num_counters
            miss    = get(base + 0)
            out0    = get(base + 1)
            out1    = get(base + 2)
            c_write = get(base + 3)
            c_updat = get(base + 4)
            c_read  = get(base + 5)
            wtf1    = get(base + 6)
            wtf2    = get(base + 7)
            if i > 0:
                print()
            print(f"  {i}       miss: {miss}")
            print(f"  {i}      write: {c_write}")
            print(f"  {i}     update: {c_updat}")
            print(f"  {i}    read_in: {c_read}")
            print(f"  {i} read_out_0: {out0}")
            print(f"  {i} read_out_1: {out1}")
            print(f"  {i} should_not_happen_1: {wtf1}")
            print(f"  {i} should_not_happen_2: {wtf2}")
        print()

    def _read_ingress_next_counters(self):
        print("COUNTER: ingress.dpa.next")
        print("-------------------------")

        def get(idx):
            return self.IN.dpa.next.pkt_counter.get(
                COUNTER_INDEX=idx, from_hw=True, print_ents=False
            ).data[b"$COUNTER_SPEC_PKTS"]

        miss, rxi, rxc = get(0), get(1), get(2)
        rtxi, rtxc, unic, mult = get(3), get(4), get(5), get(6)
        print(f" ok_incomplete: {rxi}")
        print(f"   ok_complete: {rxc}")
        print(f" re_incomplete: {rtxi}")
        print(f"   re_complete: {rtxc}")
        print(f"       unicast: {unic}")
        print(f"     multicast: {mult}")
        print(f"          miss: {miss}")
        print()