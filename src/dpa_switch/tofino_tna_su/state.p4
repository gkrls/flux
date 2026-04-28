#ifndef P4_STATE
#define P4_STATE


struct dpa_bitmap_pair_t {
  dpa_bitmap_t bitmap_0;
  dpa_bitmap_t bitmap_1;
}

control dpa_bitmap(inout headers_t H, inout ingress_metadata_t M, in ingress_intrinsic_metadata_t IM ) {

  Register<dpa_bitmap_pair_t, dpa_slot_t>(DPA_SLOTS / 2) bitmap;

  RegisterAction<dpa_bitmap_pair_t, dpa_slot_t, dpa_bitmap_t>(bitmap) process_bitmap_0 = {
    void apply(inout dpa_bitmap_pair_t bmp, out dpa_bitmap_t result) {
      result = bmp.bitmap_0;
      bmp.bitmap_0 = bmp.bitmap_0 | H.dpa.bitmap;
      bmp.bitmap_1 = bmp.bitmap_1 & (~H.dpa.bitmap);
    }
  };

  RegisterAction<dpa_bitmap_pair_t, dpa_slot_t, dpa_bitmap_t>(bitmap) process_bitmap_1 = {
    void apply(inout dpa_bitmap_pair_t bmp, out dpa_bitmap_t result) {
      result = bmp.bitmap_1;
      bmp.bitmap_0 = bmp.bitmap_0 & (~H.dpa.bitmap);
      bmp.bitmap_1 = bmp.bitmap_1 | H.dpa.bitmap;
    }
  };

  action bitmap_rec_and_check_0() {
    M.dpa.bitmap_pre = process_bitmap_0.execute(M.dpa.bitmap_slot);
    M.dpa.bitmap_chk = M.dpa.bitmap_pre & H.dpa.bitmap;
  }

  action bitmap_rec_and_check_1() {
    M.dpa.bitmap_pre = process_bitmap_1.execute(M.dpa.bitmap_slot);
    M.dpa.bitmap_chk = M.dpa.bitmap_pre & H.dpa.bitmap;
  }

  table bitmap_table {
    key = {
      // IM.ingress_port: ternary;
      M.dpa.kind: exact;
      H.dpa.flags.ver: exact;
    }
    actions = { NoAction; bitmap_rec_and_check_0; bitmap_rec_and_check_1; }
    const default_action = NoAction;
    const size = 2;
    const entries = {
      // Only non-recirc pipe0 ports have P_INPUT, so no ANY_PORT_PIPE_0 is enough
      ( /* ANY_PORT_PIPE_0, */ P_INPUT, 0) : bitmap_rec_and_check_0();
      ( /* ANY_PORT_PIPE_0, */ P_INPUT, 1) : bitmap_rec_and_check_1();
    }
  }

  apply { bitmap_table.apply(); }
}

struct dpa_counter_pair_t {
  dpa_counter_t fin;
  dpa_counter_t cnt;
}

control dpa_counter( inout headers_t H, inout ingress_metadata_t M, in ingress_intrinsic_metadata_t IM) {

  Register<dpa_counter_pair_t, dpa_slot_t>(DPA_SLOTS) counter;
  RegisterAction<dpa_counter_pair_t, dpa_slot_t, dpa_counter_t>(counter) count_first = {
    void apply(inout dpa_counter_pair_t c, out dpa_counter_t result) {
      if ( H.dpa.world == 1) {
        // Special case for single worker jobs
        c.fin = 1;
        c.cnt = 1;
        result = 1; // c.fini;
      } else {
        c.fin = 0;
        c.cnt = 1;
        result = 0; // c.fini;
      }
    }
  };
  RegisterAction<dpa_counter_pair_t, dpa_slot_t, dpa_counter_t>(counter) count = {
    void apply(inout dpa_counter_pair_t c, out dpa_counter_t result) {
      if ( c.fin == 0) {
        if ( c.cnt == H.dpa.world - 1) {
          c.fin  = H.dpa.world;
          c.cnt = H.dpa.world;
          result = c.fin; // H.dpa.world.n; // 
        } else {
          c.cnt = c.cnt + 1;
          result = c.fin; // 0
        }
      } else {
        c.fin = -c.cnt;
        // result = -c.fini;
        result = c.fin;
      }
    }
  };
  RegisterAction<dpa_counter_pair_t, dpa_slot_t, dpa_counter_t>(counter) read = {
    void apply(inout dpa_counter_pair_t c, out dpa_counter_t result) {
      if ( c.fin == 0 ) {
        result = 0;
      } else {
        c.fin = -c.cnt;
        result = c.fin;
        // result = -c.count;
      }
    }
  };

  action counter_read()         { M.dpa.count = (dpa_count_result_t) read.execute(H.dpa.slot);        }
  action counter_count()        { M.dpa.count = (dpa_count_result_t) count.execute(H.dpa.slot);       }
  action counter_count_first()  { M.dpa.count = (dpa_count_result_t) count_first.execute(H.dpa.slot); M.dpa.first = 1; }
  

  table counter_table {
    key = {
      M.dpa.kind          : exact;
      M.dpa.bitmap_pre    : ternary;
      M.dpa.bitmap_chk    : ternary;
    }
    actions = { NoAction;
                counter_read;
                counter_count;
                counter_count_first;
              }
    const size = 3;
    const default_action = NoAction;
    const entries = {
      // B_UNSEEN must preceed B_SEEN
      ( P_INPUT, 0, B_UNSEEN ) : counter_count_first();
      ( P_INPUT, _, B_UNSEEN ) : counter_count();
      ( P_INPUT, _, _        ) : counter_read();
    }
  }

// #if DPA_COUNTERS
//   Counter<bit<64>, bit<8>>(4, CounterType_t.PACKETS) pkt_counter;
//   action pkt_count_miss()   { pkt_counter.count(0); }
//   action pkt_count_first()  { pkt_counter.count(1); }
//   action pkt_count_unseen() { pkt_counter.count(2); }
//   action pkt_count_seen()   { pkt_counter.count(3); }
//   table pkt_counter_table {
//     key = { IM.ingress_port  : ternary;
//             M.dpa.kind       : ternary;
//             M.dpa.bitmap_pre : ternary;
//             M.dpa.bitmap_chk : ternary; }
//     actions = { pkt_count_miss; pkt_count_first; pkt_count_unseen; pkt_count_seen; NoAction; }
//     const size = 6;
//     const default_action = NoAction;
//     const entries = {
//       ( ANY_PORT_PIPE_0, P_INPUT,    0, B_UNSEEN) : pkt_count_first();
//       ( ANY_PORT_PIPE_0, P_INPUT,    _, B_UNSEEN) : pkt_count_unseen();
//       ( ANY_PORT_PIPE_0, P_INPUT,    _, _       ) : pkt_count_seen();
//       ( ANY_PORT_PIPE_0, P_OUTPUT_0, _, _       ) : NoAction();
//       ( ANY_PORT_PIPE_0, P_OUTPUT_1, _, _       ) : NoAction();
//       ( ANY_PORT_PIPE_0, _         , _, _       ) : pkt_count_miss();
//     }
//   }

//   Counter<bit<64>, bit<8>>(4, CounterType_t.PACKETS) count_result_histo;
//   action count_result_incomplete()       { count_result_histo.count(0); }
//   action count_result_complete_now()     { count_result_histo.count(1); }
//   action count_result_complete_earlier() { count_result_histo.count(2); }
//   action count_result_other()            { count_result_histo.count(3); }
//   table pkt_count_result_histo_table {
//     key = { IM.ingress_port : ternary;
//              M.dpa.kind     : exact;
//              M.dpa.count    : ternary; }
//     actions = { count_result_incomplete; count_result_complete_now; count_result_complete_earlier; count_result_other; NoAction; }
//     const default_action = NoAction;
//     const size = 4;
//     const entries = {
//       ( ANY_PORT_PIPE_0, P_INPUT, C_INCOMPLETE) : count_result_incomplete();
//       ( ANY_PORT_PIPE_0, P_INPUT, C_COMPLETE_NOW) : count_result_complete_now();
//       ( ANY_PORT_PIPE_0, P_INPUT, C_COMPLETE_EARLIER) : count_result_complete_earlier();
//       ( ANY_PORT_PIPE_0, P_INPUT, _) : count_result_other();
//     }
//   }
// #endif

  apply {
    counter_table.apply();
// #if DPA_COUNTERS
//     pkt_counter_table.apply();
//     pkt_count_result_histo_table.apply();
// #endif
  }
}

control dpa_state(inout headers_t H, inout ingress_metadata_t M,
                  in ingress_intrinsic_metadata_t IM,
                  in ingress_intrinsic_metadata_from_parser_t PIM) {
  dpa_bitmap() bitmap;
  dpa_counter() counter;
  apply {
    bitmap.apply(H, M, IM);
    counter.apply(H, M, IM);
  }
}

#endif