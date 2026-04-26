#ifndef DPA_ENDPOINT_P4
#define DPA_ENDPOINT_P4

control dpa_receiver_udp( inout headers_t H, inout ingress_metadata_t M,
                         in ingress_intrinsic_metadata_t IM,
                         in ingress_intrinsic_metadata_from_parser_t PIM ) {


  Register<bit<32>, bit<32>>(DPA_SLOTS, 0) sequence;
  RegisterAction<bit<32>, dpa_slot_t, dpa_seq_t>(sequence) process_sequence = {
    void apply(inout bit<32> prev, out dpa_seq_t result) {
      // int<32> diff = ((int<32>) H.dpa.seq - (int<32>) prev);
      int<32> diff = (int<32>)(H.dpa.seq - prev);
      result = (bit<32>) diff;
      if (diff == 1)
        prev = prev + 1;
      // prev = max(prev, H.dpa.seq);
    }
  };

  RegisterAction<bit<32>, dpa_slot_t, dpa_seq_t>(sequence) process_sequence_syn = {
    void apply(inout bit<32> prev, out dpa_seq_t result) {
      int<32> diff = (int<32>)(H.dpa.seq - prev);
      result = (bit<32>) diff; 
      // result = prev;
    }
  };

  action recv() { M.dpa.sequence = process_sequence.execute(H.dpa.slot); }

  action recv_pipe0(MulticastGroupId_t mgid) {
    M.dpa.mgid = mgid;
    M.dpa.ingress_port = IM.ingress_port;
    @in_hash { M.dpa.timestamp = PIM.global_tstamp[TS_SLICE]; }
    M.dpa.sequence = process_sequence.execute(H.dpa.slot);
  }

  action recv_pipe0_syn() {
    M.dpa.kind = dpa_pkt.SYN;
    M.dpa.ingress_port = IM.ingress_port;
    @in_hash { M.dpa.timestamp = PIM.global_tstamp[TS_SLICE]; }
    M.dpa.sequence = process_sequence_syn.execute(H.dpa.slot);
  }

  action recv_bad() { M.dpa.kind = dpa_pkt.BAD;}
  action recv_ignore() { M.dpa.kind = dpa_pkt.NONE; }




  // This table is currently configured to only handle a single sessions (sessid=1)
  table receiver_table {
    key = { IM.ingress_port        : ternary;
            M.dpa.kind             : ternary;
            H.dpa.session          : ternary;
            H.dpa.flags.syn        : ternary; }
    actions = { recv; recv_pipe0; recv_pipe0_syn; recv_bad; recv_ignore; NoAction;}
    const size = 7; //128;
    const default_action = recv_bad;
    const entries = {
      (               _, P_NONE , _, _) : recv_ignore(); // received NONE
      ( ANY_PORT_PIPE_0, P_INPUT, 1, 0) : recv_pipe0(2);
      ( ANY_PORT_PIPE_0, P_INPUT, 1, 1) : recv_pipe0_syn();
      (               _, P_INPUT, 1, 0) : recv();
      (               _, P_INPUT, 1, 1) : NoAction(); // NoAction();
      (               _,       _, 1, _) : NoAction();
    }
  }

 
  #include "counters/ingress_receiver_counters.p4"

  
  apply {
    receiver_table.apply();
    #if DPA_COUNTERS
    ingress_receiver_pkt_counter_table.apply();
    ingress_receiver_old_counter_table.apply();
    ingress_receiver_syn_counter_table.apply();
    #endif
  }
}

control dpa_sender_udp( inout headers_t H, inout egress_metadata_t M,
                       in egress_intrinsic_metadata_t IM,
                       in egress_intrinsic_metadata_from_parser_t PIM,
                       inout egress_intrinsic_metadata_for_deparser_t DIM) {

  action drop() { DIM.drop_ctl[0:0] = 1w1; }

  action send( mac_addr_t switch_mac, ip4_addr_t switch_ip,
               mac_addr_t worker_mac, ip4_addr_t worker_ip) {
    H.eth.src_addr = switch_mac;
    H.eth.dst_addr = worker_mac;
    H.ip4.src_addr = switch_ip;
    H.ip4.dst_addr = worker_ip;

    udp_port_t tmp = H.udp.src_port;
    H.udp.src_port = H.udp.dst_port;
    H.udp.dst_port = tmp;
    H.udp.checksum = 0;
  }

  table sender_table {
    key = { IM.egress_port: ternary;
            // H.dpa.flags.d.drop: ternary;
            H.dpa.flags.u2: ternary;
            H.dpa.session: ternary; }
    actions = { send; drop; }
    // const size = DPA_SESSIONS * DPA_MAX_WORKERS;
    const size = DPA_MAX_WORKERS;
    const default_action = drop;
  }

#if DPA_COUNTERS
  int<8> k_diff = 0;
  #define SY 0
  #define OL 1
  #define SY_OL 2
  #define BA 3
  #define RE 4
  #define RN 5
  #define RK 6
  #define MISS 7
  Counter<bit<64>, bit<8>>(15, CounterType_t.PACKETS) pkt_counter;
  action pkt_count_syn()     { pkt_counter.count(SY);   }
  action pkt_count_old()     { pkt_counter.count(OL);   }
  action pkt_count_syn_old() { pkt_counter.count(SY_OL);}
  action pkt_count_bad()     { pkt_counter.count(BA);   }
  action pkt_count_rtx()     { pkt_counter.count(RE);   }
  action pkt_count_res_n()   { pkt_counter.count(RN);   }
  action pkt_count_res_k()   { pkt_counter.count(RK);   }
  action pkt_count_miss()    { pkt_counter.count(MISS); }
  table pkt_counter_table {
    key = {H.dpa.flags.syn: ternary;
           H.dpa.flags.old: ternary;
           H.dpa.flags.bad: ternary;
           H.dpa.flags.u1 : ternary;
           k_diff         : ternary;}
    actions = { pkt_count_syn; pkt_count_old; pkt_count_syn_old; pkt_count_bad; 
                pkt_count_rtx; pkt_count_res_n; pkt_count_res_k; pkt_count_miss; }
    const default_action = pkt_count_miss;
    const entries = {
      (1, 1, _, _, _) : pkt_count_syn_old();
      (1, _, _, _, _) : pkt_count_syn();
      (_, 1, _, _, _) : pkt_count_old();
      (_, _, 1, _, _) : pkt_count_bad();
      (_, _, _, 1, _) : pkt_count_rtx();
      (0, 0, 0, 0, 0) : pkt_count_res_n();
      (0, 0, 0, 0, _) : pkt_count_res_k();
    }
  }
  #undef SY
  #undef OL
  #undef BA
  #undef RE
  #undef RN
  #undef RK
  #undef MISS
#endif

  apply {
#if DPA_COUNTERS
    k_diff = H.dpa.world.n - H.dpa.world.k;
#endif
    sender_table.apply();
#if DPA_COUNTERS
    pkt_counter_table.apply();
#endif
  }
}

#endif // DPA_ENDPOINT_P4