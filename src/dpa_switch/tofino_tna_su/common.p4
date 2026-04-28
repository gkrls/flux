#ifndef DPA_COMMON_P4
#define DPA_COMMON_P4

// ----------------------------------------------------------------------------
// ETH
// ----------------------------------------------------------------------------
typedef bit<48> mac_addr_t;
typedef bit<16> eth_type_t;

const eth_type_t ETH_IPV4   = 16w0x0800;
const eth_type_t ETH_ARP    = 16w0x0806;
const eth_type_t ETH_ROCEv1 = 16w0x8915;

header ethernet_h {
  mac_addr_t dst_addr;
  mac_addr_t src_addr;
  eth_type_t ether_type;
}
// ----------------------------------------------------------------------------
// ARP
// ----------------------------------------------------------------------------
typedef bit<16> arp_opcode_t;

const bit<16> ARP_HTYPE_ETH = 0x0001;
const bit<16> ARP_PTYPE_IP4 = ETH_IPV4;
const arp_opcode_t ARP_REQ = 1;
const arp_opcode_t ARP_RES = 2;

header arp_h {
  bit<16>      hw_type;
  eth_type_t   proto_type;
  bit<8>       hw_addr_len;
  bit<8>       proto_addr_len;
  arp_opcode_t opcode;
}
// ----------------------------------------------------------------------------
// IPv4
// ----------------------------------------------------------------------------
typedef bit<32> ip4_addr_t;
typedef bit<8>  ip4_proto_t;

const ip4_proto_t IP_ICMP   = 1;
const ip4_proto_t IP_UDP    = 17;

header arp_ip4_h {
  mac_addr_t  src_hw_addr;
  ip4_addr_t  src_proto_addr;
  mac_addr_t  dst_hw_addr;
  ip4_addr_t  dst_proto_addr;
}
header ip4_h {
  bit<4>       version;
  bit<4>       ihl;
  bit<8>       diffserv;
  bit<16>      total_len;
  bit<16>      identification;
  bit<3>       flags;
  bit<13>      frag_offset;
  bit<8>       ttl;
  ip4_proto_t  protocol;
  bit<16>      hdr_checksum;
  ip4_addr_t   src_addr;
  ip4_addr_t   dst_addr;
}
// ----------------------------------------------------------------------------
// ICMP
// ----------------------------------------------------------------------------
typedef bit<8>  icmp_type_t;

const icmp_type_t ICMP_ECHO_REQ = 8;
const icmp_type_t ICMP_ECHO_RES = 0;

header icmp_h {
  icmp_type_t msg_type;
  bit<8>      msg_code;
  bit<16>     checksum;
}

#define ICMP_OFFSET (IP4_OFFSET + IP4_SIZE)
#define ICMP_SIZE   (8 + 8 + 16)
// ----------------------------------------------------------------------------
// UDP
// ----------------------------------------------------------------------------
typedef bit<16> udp_port_t;
header udp_h {
  udp_port_t src_port;
  udp_port_t dst_port;
  bit<16> length;
  bit<16> checksum;
}

// const udp_port_t DPA_UDP_PORT = 4242;

// ----------------------------------------------------------------------------
// DPA
// ----------------------------------------------------------------------------


// #include "config.p4"

// TODO: Some checking of the settings above

// -------------


typedef bit<8>  dpa_pkt_t;
typedef bit<32> dpa_job_t;
typedef bit<32> dpa_seq_t;
typedef bit<16> dpa_slot_t;
typedef bit<32> dpa_bitmap_t;
typedef int<8>  dpa_exponent_t;
typedef int<32> dpa_value_t;
typedef int<8>  dpa_counter_t;
typedef bit<8>  dpa_count_result_t;
typedef int<8>  dpa_world_size_t;

enum dpa_pkt_t dpa_pkt {
  NONE = 0x0, 
  OUTPUT_0 = 0x1,
  OUTPUT_1 = 0x2,
  INPUT = 0x3,
  // SYN = 0x4,
  BAD = 0xff
}

@flexible
header dpa_metadata_h {
  MulticastGroupId_t  mgid;
  PortId_t            ingress_port;
  dpa_pkt_t           kind;
  dpa_count_result_t  count;
  // dpa_count_result_t  count;  // we store this as unsigned because we need to check the MSB with &&& and cannot do that with signed
  dpa_bitmap_t        bitmap_chk;
  dpa_bitmap_t        bitmap_pre;
  dpa_slot_t          bitmap_slot;
  bit<1>              first;
  bit<1>              dropsim;
}

#define DPA_METADATA_INIT(m, h)    \
  m.setValid();                    \
  m.ingress_port  = 0;             \
  m.kind          = dpa_pkt.INPUT; \
  m.mgid          = 0;             \
  m.count         = 0;             \
  m.bitmap_pre    = 0;             \
  m.bitmap_chk    = 0;             \
  m.bitmap_slot   = h.slot;        \
  m.first         = 0;             \
  m.dropsim       = 0;             \

/// These are the same flags as the straggle-aware program
/// We just repurpose u2 as re ver, and rename to u3, u4, the flags not used by this programs
struct dpa_flags_t {
  bit<1> bad;
  bit<1> old;
  bit<1> re;  // re
  bit<1> ver; // ver
  bit<1> u3;
  bit<1> u4;
  bit<2> pipes;   // 0: 1-pipe, 1: 2-pipe, ...
}

struct dpa_qvcnt_t {
  bit<8> quants;
  bit<8> values;
}

struct dpa_quant_t {
  // For exponent-based quantization this is a struct of 4 exponents
  dpa_exponent_t q3;
  dpa_exponent_t q2;
  dpa_exponent_t q1;
  dpa_exponent_t q0;
}

header dpa_h {
  bit<32> session;        // 32 bits
  bit<32> opid;           // 32 bits - unused by device
  bit<32> bitmap;         // 32 bits
  bit<32> offset;         // 32 bits
  bit<16> slot;           // 16 bits
  dpa_world_size_t world; // 8 bits
  dpa_flags_t flags;      // 8 bits
  bit<8>  unused;         // 8 bits  - unused by device
  dpa_qvcnt_t count;      // 16 bits - unused by device
  dpa_quant_t quant;      // 32 bits
}

header dpa_payload_h {
#if DPA_REDUCERS == 1
  dpa_value_t v00;
#elif DPA_REDUCERS == 2
  dpa_value_t v00; dpa_value_t v01;
#elif DPA_REDUCERS == 4
  dpa_value_t v00; dpa_value_t v01; dpa_value_t v02; dpa_value_t v03;
#elif DPA_REDUCERS == 8
  dpa_value_t v00; dpa_value_t v01; dpa_value_t v02; dpa_value_t v03;
  dpa_value_t v04; dpa_value_t v05; dpa_value_t v06; dpa_value_t v07;
#elif DPA_REDUCERS == 16
  dpa_value_t v00; dpa_value_t v01; dpa_value_t v02; dpa_value_t v03;
  dpa_value_t v04; dpa_value_t v05; dpa_value_t v06; dpa_value_t v07;
  dpa_value_t v08; dpa_value_t v09; dpa_value_t v10; dpa_value_t v11;
  dpa_value_t v12; dpa_value_t v13; dpa_value_t v14; dpa_value_t v15;
#elif DPA_REDUCERS == 32
  dpa_value_t v00; dpa_value_t v01; dpa_value_t v02; dpa_value_t v03;
  dpa_value_t v04; dpa_value_t v05; dpa_value_t v06; dpa_value_t v07;
  dpa_value_t v08; dpa_value_t v09; dpa_value_t v10; dpa_value_t v11;
  dpa_value_t v12; dpa_value_t v13; dpa_value_t v14; dpa_value_t v15;
  dpa_value_t v16; dpa_value_t v17; dpa_value_t v18; dpa_value_t v19;
  dpa_value_t v20; dpa_value_t v21; dpa_value_t v22; dpa_value_t v23;
  dpa_value_t v24; dpa_value_t v25; dpa_value_t v26; dpa_value_t v27;
  dpa_value_t v28; dpa_value_t v29; dpa_value_t v30; dpa_value_t v31;
#else
  #error "DPA_REDUCERS must be one of 1,2,4,8,16,32"
#endif
}

// Headers and metadata

@flexible
struct net_metadata_t {
  bool ip_checksum_error;
  bool ip_checksum_compute;
  bool icmp_checksum_compute;
  bit<16> icmp_checksum_pre;
}

#define NET_METADATA_INIT(m) \
  m.ip_checksum_error = false;     \
  m.ip_checksum_compute = false;   \
  m.icmp_checksum_compute = false; \
  m.icmp_checksum_pre = 0;

@flexible
struct ingress_metadata_t {
  dpa_metadata_h dpa;
  net_metadata_t net;
}

@flexible
struct egress_metadata_t {
}

#define INGRESS_METADATA_INIT(m) \
  DPA_METADATA_INIT(m.dpa)       \
  NET_METADATA_INIT(m.net)

#define EGRESS_METADATA_INIT(m) \
  DPA_METADATA_INIT(m.dpa)


struct headers_t {
  ethernet_h    eth;
  arp_h         arp;
  arp_ip4_h     arp_ip4;
  ip4_h         ip4;
  icmp_h        icmp;
  udp_h         udp;
  dpa_h         dpa;
  dpa_payload_h dpa_payload_1;
  dpa_payload_h dpa_payload_0;
}

// Helper macros

#define B_UNSEEN   0
#define B_SEEN     _
#define C_INCOMPLETE       8w0x00 &&& 8w0xFF // zer // 8w0x00 &&& 8w0xFF //0
#define C_COMPLETE_EARLIER 8w0x80 &&& 8w0x80 // neg // 8w0x02 &&& 8w0xFF //0xFF
#define C_COMPLETE_NOW     8w0x00 &&& 8w0x80 // pos // 8w0x01 &&& 8w0xFF //1


// #define C_INCOMPLETE       MATCH_ZER_i8
// #define C_COMPLETE_EARLIER MATCH_NEG_i8
// #define C_COMPLETE_NOW     MATCH_POS_i8

#define P_NONE       dpa_pkt.NONE
#define P_BAD        dpa_pkt.BAD
#define P_SYN        dpa_pkt.SYN
#define P_INPUT      dpa_pkt.INPUT
#define P_OUTPUT_0   dpa_pkt.OUTPUT_0
#define P_OUTPUT_1   dpa_pkt.OUTPUT_1

#endif // DPA_COMMON_P4