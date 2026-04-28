#ifndef DPA_ENDPOINT_P4
#define DPA_ENDPOINT_P4

control dpa_receiver_udp( inout headers_t H, inout ingress_metadata_t M,
                         in ingress_intrinsic_metadata_t IM,
                         in ingress_intrinsic_metadata_from_parser_t PIM ) {

  action recv_pipe0(MulticastGroupId_t mgid) {
    M.dpa.mgid = mgid;
    M.dpa.ingress_port = IM.ingress_port;
    // This calculates the index for the bitmap register based on slot+version
    // It assumes that the SlotAlt mapper is used by the client, which allows
    // to simply shift right (div by 2 and floor), e.g. both 2,3 -> 1, 4,5 -> 2, etc
    // For other mappings this calculation needs to be changed!!!
    M.dpa.bitmap_slot = H.dpa.slot >> 1; 
  }

  action recv_bad() { M.dpa.kind = dpa_pkt.BAD; }
  action recv_ignore() { M.dpa.kind = dpa_pkt.NONE; }
  action recv_noop() { }

  // This table is currently configured to only handle a single sessions (sessid=1, mgid=2)
  table receiver_table {
    key = { IM.ingress_port        : ternary;
            M.dpa.kind             : ternary; 
            H.dpa.session          : ternary; }
            // H.dpa.flags.d.drop: exact; }
    actions = { recv_pipe0; recv_bad; recv_noop; recv_ignore; NoAction;}
    const size = 4; //128;
    const default_action = recv_bad;
    const entries = {
      (              _, P_NONE , _) : recv_ignore();
      (ANY_PORT_PIPE_0, P_INPUT, 1) : recv_pipe0(2);
      (              _,       _, 1) : NoAction();
    }
  }

  apply { receiver_table.apply(); }
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
    key = { IM.egress_port     : ternary;
            H.dpa.session      : ternary; }
    actions = { send; drop; }
    // const size = DPA_SESSIONS * DPA_MAX_WORKERS;
    const size = DPA_MAX_WORKERS;
    const default_action = drop;
  }
  apply {
    sender_table.apply();
  }
}

#endif // DPA_ENDPOINT_P4