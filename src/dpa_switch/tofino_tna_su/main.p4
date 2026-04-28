#include <core.p4>
#include <tna.p4>

#include "config.p4"
#include "common.p4"
#include "endpoint.p4"
#include "state.p4"
#include "reduce.p4"
#include "next.p4"
#include "dropsim.p4"

#warning "Using straggle-unaware program (tofino_tna_su)"

parser ingress_parser( packet_in P, out headers_t H,
                       out ingress_metadata_t M,
                       out ingress_intrinsic_metadata_t IM) {

  Checksum() ip_checksum;
  Checksum() icmp_checksum;

  state start {
    P.extract(IM);
    P.advance(PORT_METADATA_SIZE);
    transition parse_ethernet;
  }

  state parse_ethernet {
    P.extract(H.eth);
    transition select(H.eth.ether_type) {
        ETH_ARP  : parse_arp;
        ETH_IPV4 : parse_ip4;
         default : accept;
    }
  }

  state parse_arp {
    P.extract(H.arp);
    transition select(H.arp.hw_type, H.arp.proto_type) {
      (ARP_HTYPE_ETH, ARP_PTYPE_IP4) : parse_arp_ip4;
                             default : accept;
    }
  }

  state parse_arp_ip4 {
    P.extract(H.arp_ip4);
    transition accept;
  }

  state parse_ip4 {
    NET_METADATA_INIT(M.net);
    P.extract(H.ip4);
    ip_checksum.add(H.ip4);
    M.net.ip_checksum_error = ip_checksum.verify();
    // IP packets with no options and no fragmentation
    transition select(H.ip4.ihl, H.ip4.protocol) {
      (5, IP_ICMP) : parse_icmp;
      (5, IP_UDP ) : parse_udp;
           default : accept;
    }
  }

  state parse_icmp {
    P.extract(H.icmp);
    icmp_checksum.subtract({H.icmp.checksum});
    icmp_checksum.subtract({H.icmp.msg_type, H.icmp.msg_code});
    icmp_checksum.subtract_all_and_deposit(M.net.icmp_checksum_pre);
    transition accept;
  }

  state parse_udp {
    P.extract(H.udp);
    transition select(H.udp.dst_port) {
      DPA_UDP_PORT : parse_dpa;
          default : accept;
    }
  }

  state parse_dpa {
    P.extract(H.dpa);
    transition parse_dpa_payload;
  }

  state parse_dpa_payload {
    transition select(IM.ingress_port) {
      REC_PORT_PIPE_0 : parse_dpa_recirculation;
      ANY_PORT_PIPE_0 : parse_dpa_first;
              default : parse_dpa_recirculation;
    }
  }

  state parse_dpa_first {
    /// DPA metadata is created only at pipe 0, when a packet is first received from a worker
    DPA_METADATA_INIT(M.dpa, H.dpa);
    transition parse_dpa_input;
  }



  state parse_dpa_recirculation {
    /// Recirculated packets must read DPA metadata created earlier
    P.extract(M.dpa);
    transition select(M.dpa.kind) {
      dpa_pkt.INPUT    : parse_dpa_input;
      dpa_pkt.OUTPUT_0 : parse_dpa_output_0;
      dpa_pkt.OUTPUT_1 : parse_dpa_output_1;
              default : accept;
    }
  }

  state parse_dpa_input {
#if DPA_REDUCER_MODE == DPA_REDUCER_DOUBLE
    P.extract(H.dpa_payload_1);
#endif
    P.extract(H.dpa_payload_0);
    transition accept;
  }

  state parse_dpa_output_0 { H.dpa_payload_0.setValid(); transition accept; }
  state parse_dpa_output_1 { H.dpa_payload_1.setValid(); transition accept; }
}

control ingress_deparser( packet_out P, inout headers_t H,
                          in ingress_metadata_t M,
                          in ingress_intrinsic_metadata_for_deparser_t DIM) {
  Checksum() ip4_checksum;
  Checksum() icmp_checksum;

  apply {
    if (M.net.ip_checksum_compute) {
      H.ip4.hdr_checksum = ip4_checksum.update(
        { H.ip4.version,
          H.ip4.ihl,
          H.ip4.diffserv,
          H.ip4.total_len,
          H.ip4.identification,
          H.ip4.flags,
          H.ip4.frag_offset,
          H.ip4.ttl,
          H.ip4.protocol,
          H.ip4.src_addr,
          H.ip4.dst_addr } );
    }

    if (M.net.icmp_checksum_compute) {
      H.icmp.checksum = icmp_checksum.update(
        { H.icmp.msg_type,
          H.icmp.msg_code,
          M.net.icmp_checksum_pre } );
    }

    P.emit(H.eth);
    P.emit(H.arp);
    P.emit(H.arp_ip4);
    P.emit(H.ip4);
    P.emit(H.icmp);
    P.emit(H.udp);
    P.emit(H.dpa);
    // P.emit(H.dpa_quants);
    /// The DPA metadata will be emitted only for packets that get recirculated
    /// across the ingresses for input or output packets. When we finally reach
    /// an egress, the ml metadata will be invalidated (and thus not emitted).
    /// Currently the only egress we should be hitting is the egress of PIPE_0
    /// All other egresses are (should be) bypassed.
    /// See the reflect and multicast actions in dpa_next.p4
    P.emit(M.dpa);
    P.emit(H.dpa_payload_1);
    P.emit(H.dpa_payload_0);
  }
}

parser egress_parser( packet_in P, out headers_t H,
                      out egress_metadata_t M,
                      out egress_intrinsic_metadata_t IM) {
  state start {
    P.extract(IM);
    transition parse_ethernet;
  }

  state parse_ethernet {
    P.extract(H.eth);
    transition select(H.eth.ether_type) {
      ETH_IPV4 : parse_ip4;
       default : accept;
    }
  }

  state parse_ip4 {
    P.extract(H.ip4);
    transition select(H.ip4.protocol) {
       IP_UDP : parse_udp;
      IP_ICMP : parse_icmp;
      default : accept;
    }
  }

  state parse_icmp {
    P.extract(H.icmp);
    transition accept;
  }

  state parse_udp {
    P.extract(H.udp);
    transition select(H.udp.dst_port) {
      // NB: At this point the packet still contains the switch's UDP ports as dst_port
      // Only the sender modifies the packet with the worker's info (see endpoint.p4)
      DPA_UDP_PORT : parse_dpa;
           default : accept;
    }
  }

  state parse_dpa {
    // This state is (should) only be reached when at PIPE_0
    // If we reach PIPE_0 egress we are done so there is no need to parse values
    P.extract(H.dpa);
    transition accept;
  }
}

control egress_deparser( packet_out P, inout headers_t H,
                         in egress_metadata_t M,
                         in egress_intrinsic_metadata_for_deparser_t DIM) {

  Checksum() ip4_checksum;
  apply {
    if (H.ip4.isValid()) {
      H.ip4.hdr_checksum = ip4_checksum.update(
        { H.ip4.version,
          H.ip4.ihl,
          H.ip4.diffserv,
          H.ip4.total_len,
          H.ip4.identification,
          H.ip4.flags,
          H.ip4.frag_offset,
          H.ip4.ttl,
          H.ip4.protocol,
          H.ip4.src_addr,
          H.ip4.dst_addr } );
    }
    P.emit(H.eth);
    P.emit(H.ip4);
    P.emit(H.icmp);
    P.emit(H.udp);
    P.emit(H.dpa);
  }
}

control dpa_ingress( inout headers_t H, inout ingress_metadata_t M,
                    in ingress_intrinsic_metadata_t IM,
                    in ingress_intrinsic_metadata_from_parser_t PIM,
                    inout ingress_intrinsic_metadata_for_deparser_t DIM,
                    inout ingress_intrinsic_metadata_for_tm_t TIM ) {
#if DPA_COMM == DPA_COMM_UDP
#define dpa_receiver dpa_receiver_udp
#define dpa_sender   dpa_sender_udp
#else
#error "Only DPA_COMM_UDP supported"
#endif

  dpa_receiver() receiver;
  dpa_state() state;
  dpa_reduce() reduce;
  dpa_next() next;
  apply {
    receiver.apply(H, M, IM, PIM);
    state.apply(H, M, IM, PIM);
    reduce.apply(H, M, IM);
    next.apply(H, M, IM, DIM, TIM);
  }
}

control dpa_egress(inout headers_t H, inout egress_metadata_t M,
                   in egress_intrinsic_metadata_t IM,
                   in egress_intrinsic_metadata_from_parser_t PIM,
                   inout egress_intrinsic_metadata_for_deparser_t DIM,
                   inout egress_intrinsic_metadata_for_output_port_t OPIM ) {
  // dpa_dropsim_egress() dropsim;
  dpa_sender() sender;
  apply {
    // dropsim.apply(H, DIM);
    sender.apply(H, M, IM, PIM, DIM);
  }
}

control net_ingress( inout headers_t H, inout ingress_metadata_t M,
                     in ingress_intrinsic_metadata_t IM,
                     in ingress_intrinsic_metadata_from_parser_t PIM,
                     inout ingress_intrinsic_metadata_for_deparser_t DIM,
                     inout ingress_intrinsic_metadata_for_tm_t TIM ) {
  action icmp_echo_respond() {
    // swap mac addresses
    mac_addr_t tmp = H.eth.src_addr;
    H.eth.src_addr = H.eth.dst_addr;
    H.eth.dst_addr = tmp;

    // swap ip addresses
    ip4_addr_t tmp2 = H.ip4.src_addr;
    H.ip4.src_addr  = H.ip4.dst_addr;
    H.ip4.dst_addr  = tmp2;
    H.ip4.ttl = H.ip4.ttl |-| 1;

    // swap ip addresses
    H.icmp.msg_type = ICMP_ECHO_RES;
    M.net.icmp_checksum_compute = true;
    M.net.ip_checksum_compute = true;
  }

  table icmp {
    key = { H.ip4.dst_addr: exact; }
    const size = NET_ICMP_TABLE_SIZE;
    actions = {icmp_echo_respond; NoAction;}
  }

  action arp_resolve(mac_addr_t mac) {
    H.arp.opcode = ARP_RES;
    H.arp_ip4.dst_hw_addr = H.arp_ip4.src_hw_addr;
    H.arp_ip4.src_hw_addr = mac;
    ip4_addr_t tmp = H.arp_ip4.dst_proto_addr;
    H.arp_ip4.dst_proto_addr = H.arp_ip4.src_proto_addr;
    H.arp_ip4.src_proto_addr = tmp;
    H.eth.dst_addr = H.eth.src_addr;
    H.eth.src_addr = mac;
  }

  table arp {
    key = { H.arp_ip4.dst_proto_addr: exact; }
    const size = NET_ARP_TABLE_SIZE;
    actions = {arp_resolve; NoAction;}
    default_action = NoAction;
  }

  action send_to_port(PortId_t port) {
    TIM.ucast_egress_port = port;
    DIM.drop_ctl[0:0] = 0;
  }

  action multicast(MulticastGroupId_t mgid) {
    TIM.mcast_grp_a = mgid;
    DIM.drop_ctl[0:0] = 0;
  }

  action flood() {
    TIM.level1_exclusion_id = (bit<16>) IM.ingress_port;
    multicast(1);
  }

  action drop() { DIM.drop_ctl[0:0] = 1; }

  table forwarding {
    key = { H.eth.dst_addr: exact; }
    actions = {send_to_port; flood; drop; NoAction;}
    default_action = flood();
    const size = NET_FORWARDING_TABLE_SIZE;
  }

  apply {
    if (H.arp_ip4.isValid() && H.arp.opcode == ARP_REQ)
      arp.apply();
    else if (H.icmp.isValid())
      icmp.apply();
    forwarding.apply();
    TIM.bypass_egress = 1w1;
  }
}

control net_egress( inout headers_t H, inout egress_metadata_t M,
                    in egress_intrinsic_metadata_t IM,
                    in egress_intrinsic_metadata_from_parser_t PIM,
                    inout egress_intrinsic_metadata_for_deparser_t DIM,
                    inout egress_intrinsic_metadata_for_output_port_t OPIM ) {
  apply { /* empty */ }
}

control ingress( inout headers_t H, inout ingress_metadata_t M,
                 in ingress_intrinsic_metadata_t IM,
                 in ingress_intrinsic_metadata_from_parser_t PIM,
                 inout ingress_intrinsic_metadata_for_deparser_t DIM,
                 inout ingress_intrinsic_metadata_for_tm_t TIM ) {
  dpa_ingress() dpa;
  net_ingress() net;
  apply {
    if (H.dpa.isValid() && !M.net.ip_checksum_error) {
      dpa.apply(H, M, IM, PIM, DIM, TIM); // DPA traffic
    } else {
      net.apply(H, M, IM, PIM, DIM, TIM); // Regular traffic
    }
  }
}


control egress( inout headers_t H, inout egress_metadata_t M,
                in egress_intrinsic_metadata_t IM,
                in egress_intrinsic_metadata_from_parser_t PIM,
                inout egress_intrinsic_metadata_for_deparser_t DIM,
                inout egress_intrinsic_metadata_for_output_port_t OPIM ) {
  dpa_egress() dpa;
  net_egress() net;
  apply {
    if (H.dpa.isValid()) {
      dpa.apply(H, M, IM, PIM, DIM, OPIM);
    } else {
      net.apply(H, M, IM, PIM, DIM, OPIM);
    }
  }
}

Pipeline(ingress_parser(), ingress(), ingress_deparser(), egress_parser(), egress(), egress_deparser()) pipe;

Switch(pipe) main;