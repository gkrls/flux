"""Straggle-aware DPA controller."""
from .cli_controller import CLIController
from netaddr import IPAddress, EUI
from .util import tsc_for_ms


class CLIControllerSA(CLIController):
    """Handles logic specific to the straggle-aware DPA program."""

    def setup_dpa(self):
        print("\n** DPA *********************************************************\n")

        # Clear all dpa ingress state
        self.IN.dpa.receiver.sequence.clear()
        self.IN.dpa.state.timer.time.clear()
        self.IN.dpa.state.timer.tolerance.set_default(0)
        self.IN.dpa.state.bitmap.bitmap.clear()
        self.IN.dpa.state.counter.counter.clear()
        self.IN.dpa.state.counter.counter_last.clear()
        for i in range(self.conf["switch"]["program"]["config"]["dpa_reducers"]):
            getattr(self.IN.dpa.reduce, f"value_{i:02d}").R.clear()
        for i in range(self.conf["switch"]["program"]["config"]["dpa_exponents"]):
            getattr(self.IN.dpa.reduce, f"exponent_{i:01d}").R.clear()

        # Clear all dpa egress state
        self.EG.dpa.sender.sender_table.clear()
        try:
            self.EG.dpa.dropsim.dropsim_thres.clear()
            self.EG.dpa.dropsim.dropsim_table.clear()
        except AttributeError as e:
            print(f"   error: {e}")

        # Clear counters (if enabled)
        try:
            self.IN.dpa.receiver.ingress_receiver_pkt_counter.clear()
            self.IN.dpa.receiver.ingress_receiver_old_counter.clear()
            self.IN.dpa.receiver.ingress_receiver_syn_counter.clear()
            self.IN.dpa.state.counter.pkt_counter.clear()
            self.IN.dpa.next.ingress_next_pre_pkt_counter.clear()
            self.EG.dpa.sender.pkt_counter.clear()
        except Exception:
            pass

        for session_name, session_config in self.conf["switch"]["sessions"].items():
            session_id = session_config["id"]
            session_mgid = session_config["mgid"]
            switch_mac = EUI(self.conf["switch"]["mac"])
            switch_ip = IPAddress(self.conf["switch"]["ip"])

            print(f"\n  Session {session_id}")

            self.EG.dpa.sender.sender_table.add_with_drop(u2=1)
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

            tsc = tsc_for_ms(session_config["straggle_timeout_ms"], 16, 48)
            self.IN.dpa.state.timer.tolerance.set_default(tsc)
            print(f"  . added straggle-timeout of {session_config['straggle_timeout_ms']}ms"
                  f" -> tsc={tsc} ticks of {2 ** 16}ns actual= {tsc * 2 ** 16 / 1_000_000}ms")

            try:
                eg_drop_thres = (2 ** 16 - 1) * session_config["dropsim"]["egress"]
                self.EG.dpa.dropsim.dropsim_table.add_with_dropsim(session=1, internal_job_id=1)
                self.EG.dpa.dropsim.dropsim_thres.add(
                    REGISTER_INDEX=(session_id - 1), f1=eg_drop_thres
                )
                print(f"  . added {eg_drop_thres} egress dropsim")
            except AttributeError as e:
                print(f"   error: {e}")

    # ---------- counters ----------

    def read_counters(self):
        print()
        print("  *************************************")
        print("  *   DPA_SA switch packet counters   *")
        print("  *************************************")
        if not self.has_counters:
            print("  n/a\n")
            return
        self._read_ingress_receiver_counters()
        self._read_ingress_state_counters()
        self._read_ingress_next_counters()
        self._read_egress_sender_counters()

    def _get_rx(self, idx):
        return self.IN.dpa.receiver.ingress_receiver_pkt_counter.get(
            COUNTER_INDEX=idx, from_hw=True, print_ents=False
        ).data[b"$COUNTER_SPEC_PKTS"]

    def _read_ingress_receiver_counters(self):
        print("---------------------------------------")
        print("PKT_COUNT: ingress.dpa.receiver.counter")
        print("---------------------------------------")

        pipe0 = {
            "inp_old": self._get_rx(1),
            "inp_exp": self._get_rx(2),
            "inp_new": self._get_rx(3),
            "inp_hi":  self._get_rx(4),
            "syn_old": self._get_rx(5),
            "syn_exp": self._get_rx(6),
            "syn_new": self._get_rx(7),
            "syn_hi":  self._get_rx(8),
            "bad":     self._get_rx(9),
            "none":    self._get_rx(0),
            "other":   self._get_rx(19),
        }
        pipeN = {
            "syn":   self._get_rx(21),
            "bad":   self._get_rx(22),
            "none":  self._get_rx(20),
            "other": self._get_rx(29),
        }

        print(" [pipe0]")
        for k, v in pipe0.items():
            print(f"   {k:<8} | {v}")
        print()

        old = [
            self.IN.dpa.receiver.ingress_receiver_old_counter.get(
                COUNTER_INDEX=i, from_hw=True, print_ents=False
            ).data[b"$COUNTER_SPEC_PKTS"]
            for i in range(6)
        ]
        syn = [
            self.IN.dpa.receiver.ingress_receiver_syn_counter.get(
                COUNTER_INDEX=i, from_hw=True, print_ents=False
            ).data[b"$COUNTER_SPEC_PKTS"]
            for i in range(6)
        ]
        print("   old_per_port: " + "".join(f"[{i}] {o:<14}" for i, o in enumerate(old)))
        print("   syn_per_port: " + "".join(f"[{i}] {o:<14}" for i, o in enumerate(syn)))

        print("\n [pipeN]:")
        for k, v in pipeN.items():
            print(f"   {k:<8} | {v}")

    def _read_ingress_state_counters(self):
        print("------------------------------------")
        print("PKT_COUNT: ingress.dpa.state.counter")
        print("------------------------------------")
        NUM_COUNTERS = 9
        OK, OK_TO = 0, 1
        RE, RE_TO = 2, 3
        SY, OL, HI, RC = 4, 5, 6, 7
        MISS = 8

        def col(offset):
            return [
                self.IN.dpa.state.counter.pkt_counter.get(
                    COUNTER_INDEX=(i * NUM_COUNTERS + offset),
                    from_hw=True, print_ents=False,
                ).data[b"$COUNTER_SPEC_PKTS"]
                for i in range(4)
            ]

        labels = ["ok", "ok_to", "re", "re_to", "sy", "ol", "hi", "rc", "miss"]
        values = [col(OK), col(OK_TO), col(RE), col(RE_TO),
                  col(SY), col(OL), col(HI), col(RC), col(MISS)]

        col_widths = [max(len(str(v)) for v in column) for column in zip(*values)]
        for label, row in zip(labels, values):
            line = " | ".join(str(row[i]).ljust(col_widths[i]) for i in range(len(row)))
            print(f"{label:>8} | {line}")
        print()

    def _read_ingress_next_counters(self):
        print("---------------------------------")
        print("PKT_COUNT: ingress.dpa.next (pre)")
        print("---------------------------------")

        def get(idx):
            return self.IN.dpa.next.ingress_next_pre_pkt_counter.get(
                COUNTER_INDEX=idx, from_hw=True, print_ents=False
            ).data[b"$COUNTER_SPEC_PKTS"]

        data = {
            "syn_incomplete":       get(7),
            "syn_complete":         get(8),
            "old_incomplete":       get(9),
            "old_complete":         get(10),
            "new":                  get(11),
            "exp":                  get(12),
            "bad_input_old_seen":   get(2),
            "bad_input_hi_seen":    get(3),
            "bad_input_hi_unseen":  get(4),
            "bad_syn_hi":           get(5),
            "bad_syn_complete_now": get(6),
            "bad_other":            get(1),
            "other":                get(0),
        }
        for k, v in data.items():
            print(f"  {k:<20} | {v}")
        print()

    def _read_egress_sender_counters(self):
        print("----------------------------")
        print("PKT_COUNT: egress.dpa.sender")
        print("----------------------------")

        def get(idx):
            return self.EG.dpa.sender.pkt_counter.get(
                COUNTER_INDEX=idx, from_hw=True, print_ents=False
            ).data[b"$COUNTER_SPEC_PKTS"]

        sy, ol, so, ba = get(0), get(1), get(2), get(3)
        re, rn, rk, miss = get(4), get(5), get(6), get(7)
        print(f"    syn  : {sy}")
        print(f"    old  : {ol}")
        print(f"syn_old  : {so}")
        print(f"    bad  : {ba}")
        print(f"    rtx  : {re}")
        print(f"    res_n: {rn}")
        print(f"    res_k: {rk}")
        print(f"    miss : {miss}")
        print()