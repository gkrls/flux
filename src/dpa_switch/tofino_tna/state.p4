#ifndef P4_STATE
#define P4_STATE

struct dpa_time_t {
  bit<32> force;
  dpa_timestamp_t ts;
}

control dpa_timer( inout headers_t H, inout ingress_metadata_t M, in ingress_intrinsic_metadata_t IM ) {

  // At the moment we can only have 1 value for tolerance
  // Ideally we would want a different tolerance per job but it not
  // obvious how to implement this.
  //
  // Things i tried and did not work:
  //   1. Read tolerance at the receiver table into metadata:
  //      But, the RegisterAction has too many inputs from PHV
  //   2. Use a struct {bit<32>, bit<32>} to store tolerance+timestamp per slot:
  //      But, in double-value register we can only subtract to write back to
  //      the register, not for output (write to the PHV)
  //
  // Probably the best (or only) way to currently support different tolerances
  // is to apply N tables at ones, one per job. If we use the job_id as key
  // only 1 of those tables will actually hit.
  // The downside is that since those tables use a register, we can only have at
  // most N = 4 tables (and thus jobs) because that's the max registers in a stage
  RegisterParam<dpa_timestamp_t>(0) tolerance;
  Register<dpa_timestamp_t, dpa_slot_t>(DPA_SLOTS) time;

  RegisterAction<dpa_timestamp_t, dpa_slot_t, bit<1>>(time) check = {
    void apply(inout dpa_timestamp_t t, out bit<1> result) {
      int<32> now = (int<32>) M.dpa.timestamp;
      int<32> old = (int<32>) t;
      int<32> dif = (int<32>) (now - old);
      if ( dif > (int<32>) tolerance.read())
        result = T_TIMEOUT;
      else if (dif < 0)
        result = T_TIMEOUT;
      else
        result = T_NO_TIMEOUT;
    }
  };

  RegisterAction<dpa_timestamp_t, dpa_slot_t, bit<1>>(time) record = {
    void apply(inout dpa_timestamp_t t, out bit<1> result) {
      result = 0; // T_NO_TIMEOUT
      t = M.dpa.timestamp;
    }
  };

  action timeout_record() { M.dpa.timeout = record.execute(H.dpa.slot); }
  action timeout_check()  { M.dpa.timeout = check.execute(H.dpa.slot);  }

  // ONLY PIPE 0
  table timeout_table {
    key = {
      IM.ingress_port : ternary;
      M.dpa.kind : exact;
      M.dpa.sequence : ternary;
    }
    actions = { timeout_check; timeout_record; NoAction; }
    const default_action = NoAction;
    const size = 2;
    const entries = {
      ( ANY_PORT_PIPE_0, P_INPUT, S_EXP) : timeout_check();
      ( ANY_PORT_PIPE_0, P_INPUT, S_NEW) : timeout_record();
    }
  }

  apply {
    // M.dpa.world = H.dpa.world.n; // save it to restore it later if needed
    timeout_table.apply();
  }
}

control dpa_bitmap( inout headers_t H, inout ingress_metadata_t M,
                   in ingress_intrinsic_metadata_t IM ) {

  Register<dpa_bitmap_t, dpa_slot_t>(DPA_SLOTS) bitmap;

  RegisterAction<dpa_bitmap_t, dpa_slot_t, dpa_bitmap_t>(bitmap) bitmap_set = {
    void apply(inout dpa_bitmap_t bmp, out dpa_bitmap_t result) {
      bmp = H.dpa.bitmap;
      result = 0;
    }
  };

  RegisterAction<dpa_bitmap_t, dpa_slot_t, dpa_bitmap_t>(bitmap) bitmap_rec = {
    void apply(inout dpa_bitmap_t bmp, out dpa_bitmap_t result) {
      result = bmp;
      bmp = bmp | H.dpa.bitmap;
    }
  };

  action bitmap_new() {
    dpa_bitmap_t tmp = bitmap_set.execute(H.dpa.slot);
    M.dpa.bitmap_pre = tmp;
    M.dpa.bitmap     = W_NOT_SEEN;
  }
  action bitmap_rec_and_check() {
    dpa_bitmap_t tmp = bitmap_rec.execute(H.dpa.slot);
    M.dpa.bitmap_pre = tmp;
    M.dpa.bitmap     = tmp & H.dpa.bitmap;
  }
  action bitmap_get_and_check() {
    dpa_bitmap_t tmp = bitmap.read(H.dpa.slot);
    M.dpa.bitmap_pre = tmp;
    M.dpa.bitmap     = tmp & H.dpa.bitmap;
  }
  action bitmap_get() {
    M.dpa.bitmap_pre = bitmap.read(H.dpa.slot);
  }

  // ONLY PIPE 0
  table bitmap_table {
    key = {
      IM.ingress_port: ternary;
      M.dpa.kind: exact;
      M.dpa.sequence: ternary;
    }
    actions = { NoAction; bitmap_new; bitmap_get; bitmap_rec_and_check; bitmap_get_and_check; }
    const default_action = NoAction;
    const size = 5;
    const entries = {
      ( ANY_PORT_PIPE_0, P_INPUT, S_EXP ) : bitmap_rec_and_check();
      ( ANY_PORT_PIPE_0, P_INPUT, S_OLD ) : bitmap_get_and_check();
      ( ANY_PORT_PIPE_0, P_INPUT, S_NEW ) : bitmap_new();
      ( ANY_PORT_PIPE_0, P_INPUT, S_HI  ) : NoAction();
      ( ANY_PORT_PIPE_0, P_SYN  , _     ) : bitmap_get();
    }
  }

  apply { bitmap_table.apply(); }
}

struct dpa_counter_state_t {
  dpa_counter_t fini;
  dpa_counter_t count;
}

control dpa_counter( inout headers_t H, inout ingress_metadata_t M, in ingress_intrinsic_metadata_t IM) {

  Register<dpa_counter_t, dpa_slot_t>(DPA_SLOTS) counter_last;
  RegisterAction<dpa_counter_t, dpa_slot_t, dpa_counter_t>(counter_last) count_last_setk = {
    void apply(inout dpa_counter_t last, out dpa_counter_t result) {
      last = H.dpa.world.k;
      result = H.dpa.world.k;
    }
  };
  RegisterAction<dpa_counter_t, dpa_slot_t, dpa_counter_t>(counter_last) count_last_setn = {
    void apply(inout dpa_counter_t last, out dpa_counter_t result) {
      last = H.dpa.world.n;
      result = H.dpa.world.n;
    }
  };
  RegisterAction<dpa_counter_t, dpa_slot_t, dpa_counter_t>(counter_last) count_last_read = {
    void apply(inout dpa_counter_t last, out dpa_counter_t result) {
      result = last;
    }
  };

  action count_last_set_k() { H.dpa.world.n = count_last_setk.execute(H.dpa.slot); }
  action count_last_set_n() { H.dpa.world.n = count_last_setn.execute(H.dpa.slot); }
  action count_last_get()   { H.dpa.world.n = count_last_read.execute(H.dpa.slot); }

  /// This table calculates the value we are counting up to, i.e. N or K, and stores the result in H.dpa.world.n.
  /// This in turn is used by the counter_table below to perform the counting and find if the slot is complete.
  /// Note storing this in metadata, for some reason, does not work, so we have to store it in a header.
  /// The original H.dpa.world.n is stored in metadata M.dpa.world_n, so it can be restored later (see dpa_next unicast/multicast)
  table counter_last_table {
    key = {
      M.dpa.kind       : exact;
      M.dpa.sequence   : ternary;
      H.dpa.flags.cntk : ternary;
    }
    actions = { count_last_set_k; count_last_set_n; count_last_get; NoAction; }
    const default_action = NoAction;
    const size = 4;
    const entries = {
      ( P_INPUT, S_NEW, 1) : count_last_set_k();
      ( P_INPUT, S_NEW, 0) : count_last_set_n();
      ( P_INPUT, _,     _) : count_last_get();
      ( P_SYN,   _,     _) : count_last_get();
    }
  }


  Register<dpa_counter_state_t, dpa_slot_t>(DPA_SLOTS) counter;

  RegisterAction<dpa_counter_state_t, dpa_slot_t, dpa_counter_t>(counter) counter_start = {
    void apply(inout dpa_counter_state_t c, out dpa_counter_t result) {
      if ( H.dpa.world.n == 1) {
        // Special case for single worker jobs
        c.fini = 1;
        c.count = 1;
        result = 1; // c.fini;
      } else {
        c.fini = 0;
        c.count = 1;
        result = 0; // c.fini;
      }
    }
  };

  RegisterAction<dpa_counter_state_t, dpa_slot_t, dpa_counter_t>(counter) counter_read = {
    void apply(inout dpa_counter_state_t c, out dpa_counter_t result) {
      if ( c.fini == 0 ) {
        result = 0;
      } else {
        c.fini = -c.count;
        result = c.fini;
        // result = -c.count;
      }
    }
  };

  // 0 -> incomplete
  // + -> completed now
  // - -> completed earlier
  RegisterAction<dpa_counter_state_t, dpa_slot_t, dpa_counter_t>(counter) counter_count_check_last = {
    void apply ( inout dpa_counter_state_t c, out dpa_counter_t result) {
      if ( c.fini == 0) {
        if ( c.count == H.dpa.world.n - 1) {
          c.fini  = H.dpa.world.n;
          c.count = H.dpa.world.n;
          result = c.fini; // H.dpa.world.n; 
        } else {
          c.count = c.count + 1;
          result = c.fini; // 0
        }
      } else {
        c.fini = -c.count;
        // result = -c.fini;
        result = c.fini;
      }
    }
  };

  RegisterAction<dpa_counter_state_t, dpa_slot_t, dpa_counter_t>(counter) counter_check_k = {
    void apply ( inout dpa_counter_state_t c, out dpa_counter_t result) {
      if ( c.fini == 0) {
        if ( c.count >= H.dpa.world.k) {
          c.fini = c.count;
          result = c.fini; // c.count; 
        } else {
          result = c.fini; // 0
        }
      } else {
        c.fini = -c.count;
        // result = -c.fini;
        result = c.fini;
      }
    }
  };

  action count_syn()                    { M.dpa.count = (dpa_count_result_t) counter_read.execute(H.dpa.slot); }
  action count_old()                    { M.dpa.count = (dpa_count_result_t) counter_read.execute(H.dpa.slot); }
  action count_first()                  { M.dpa.count = (dpa_count_result_t) counter_start.execute(H.dpa.slot); }
  action count_intermediate()           { M.dpa.count = (dpa_count_result_t) counter_count_check_last.execute(H.dpa.slot); }
  action count_intermediate_timeout()   { M.dpa.count = (dpa_count_result_t) counter_count_check_last.execute(H.dpa.slot); } // counter_count_check_k.execute(H.dpa.slot); }
  action count_retransmission()         { M.dpa.count = (dpa_count_result_t) counter_read.execute(H.dpa.slot); }
  action count_retransmission_timeout() { M.dpa.count = (dpa_count_result_t) counter_check_k.execute(H.dpa.slot); }

  // ALL PIPES
  table counter_table {
    key = {
      // IM.ingress_port     : ternary;
      M.dpa.kind          : exact;
      M.dpa.sequence      : ternary;
      M.dpa.bitmap        : ternary;
      M.dpa.timeout       : ternary;
    }
    actions = { NoAction;
                count_old;
                count_syn;
                count_first;
                count_intermediate;
                count_intermediate_timeout;
                count_retransmission;
                count_retransmission_timeout;
              }
    const size = 16;
    const default_action = NoAction;
    const entries = {
      ( /* ANY_PORT_PIPE_0 , */  P_INPUT, S_EXP ,  W_NOT_SEEN, T_NO_TIMEOUT ) : count_intermediate();
      ( /* ANY_PORT_PIPE_0 , */  P_INPUT, S_EXP ,  W_NOT_SEEN,    T_TIMEOUT ) : count_intermediate_timeout(); // !!!!
      ( /* ANY_PORT_PIPE_0 , */  P_INPUT, S_EXP ,      W_SEEN, T_NO_TIMEOUT ) : count_retransmission();
      ( /* ANY_PORT_PIPE_0 , */  P_INPUT, S_EXP ,      W_SEEN,    T_TIMEOUT ) : count_retransmission_timeout();
      ( /* ANY_PORT_PIPE_0 , */  P_INPUT, S_NEW ,           _,            _ ) : count_first();
      ( /* ANY_PORT_PIPE_0 , */  P_INPUT, S_OLD ,           _,            _ ) : count_old();
      ( /* ANY_PORT_PIPE_0 , */  P_INPUT, S_HI  ,           _,            _ ) : NoAction();
      ( /* ANY_PORT_PIPE_0 , */  P_SYN  ,  _    ,           _,            _ ) : count_syn();
    }
  }



#if DPA_COUNTERS
  #define DPA_NUM_COUNTERS 9
  #define OK 0
  #define OK_TIMEOUT 1
  #define RE 2
  #define RE_TIMEOUT 3
  #define SY 4
  #define OL 5
  #define HI 6
  #define RC 7
  #define MISS 8
  Counter<bit<64>, bit<8>>(4 * DPA_NUM_COUNTERS, CounterType_t.PACKETS) pkt_counter;
  action pkt_count(bit<8> idx)  { pkt_counter.count(idx); }                            
  table pkt_counter_table {
    key = {
      IM.ingress_port     : ternary;
      M.dpa.kind          : ternary;
      M.dpa.sequence      : ternary;
      M.dpa.bitmap        : ternary;
      M.dpa.timeout       : ternary; 
    }
    actions = { pkt_count; NoAction; }

    const size = 64;
    const default_action = NoAction;
    const entries = {
      ( REC_PORT_PIPE_0 ,  _      , _     ,  _         ,            _ ) : pkt_count(RC);
      ( ANY_PORT_PIPE_0 ,  P_INPUT, S_NEW ,           _,            _ ) : pkt_count(OK);
      ( ANY_PORT_PIPE_0 ,  P_INPUT, S_EXP ,  W_NOT_SEEN, T_NO_TIMEOUT ) : pkt_count(OK);
      ( ANY_PORT_PIPE_0 ,  P_INPUT, S_EXP ,  W_NOT_SEEN,    T_TIMEOUT ) : pkt_count(OK_TIMEOUT);
      ( ANY_PORT_PIPE_0 ,  P_INPUT, S_EXP ,      W_SEEN, T_NO_TIMEOUT ) : pkt_count(RE);
      ( ANY_PORT_PIPE_0 ,  P_INPUT, S_EXP ,      W_SEEN,    T_TIMEOUT ) : pkt_count(RE_TIMEOUT);
      ( ANY_PORT_PIPE_0 ,  P_INPUT, S_OLD ,           _,            _ ) : pkt_count(OL);
      ( ANY_PORT_PIPE_0 ,  P_INPUT, S_HI  ,           _,            _ ) : pkt_count(HI);
      ( ANY_PORT_PIPE_0 ,  P_SYN  ,  _    ,           _,            _ ) : pkt_count(SY);
      ( ANY_PORT_PIPE_0 ,         _,  _    ,           _,           _ ) : pkt_count(MISS);

      ( REC_PORT_PIPE_1 ,  _      , _     ,  _         ,            _ ) : pkt_count(1 * DPA_NUM_COUNTERS + RC);
      ( ANY_PORT_PIPE_1 ,  P_INPUT, S_NEW ,           _,            _ ) : pkt_count(1 * DPA_NUM_COUNTERS + OK);
      ( ANY_PORT_PIPE_1 ,  P_INPUT, S_EXP ,  W_NOT_SEEN, T_NO_TIMEOUT ) : pkt_count(1 * DPA_NUM_COUNTERS + OK);
      ( ANY_PORT_PIPE_1 ,  P_INPUT, S_EXP ,  W_NOT_SEEN,    T_TIMEOUT ) : pkt_count(1 * DPA_NUM_COUNTERS + OK_TIMEOUT);
      ( ANY_PORT_PIPE_1 ,  P_INPUT, S_EXP ,      W_SEEN, T_NO_TIMEOUT ) : pkt_count(1 * DPA_NUM_COUNTERS + RE);
      ( ANY_PORT_PIPE_1 ,  P_INPUT, S_EXP ,      W_SEEN,    T_TIMEOUT ) : pkt_count(1 * DPA_NUM_COUNTERS + RE_TIMEOUT);
      ( ANY_PORT_PIPE_1 ,  P_INPUT, S_OLD ,           _,            _ ) : pkt_count(1 * DPA_NUM_COUNTERS + OL);
      ( ANY_PORT_PIPE_1 ,  P_INPUT, S_HI  ,           _,            _ ) : pkt_count(1 * DPA_NUM_COUNTERS + HI);
      ( ANY_PORT_PIPE_1 ,  P_SYN  ,  _    ,           _,            _ ) : pkt_count(1 * DPA_NUM_COUNTERS + SY);
      ( ANY_PORT_PIPE_1 ,        _,  _    ,           _,            _ ) : pkt_count(1 * DPA_NUM_COUNTERS + MISS);

      ( REC_PORT_PIPE_2 ,  _      , _     ,  _         ,            _ ) : pkt_count(2 * DPA_NUM_COUNTERS + RC);
      ( ANY_PORT_PIPE_2 ,  P_INPUT, S_NEW ,           _,            _ ) : pkt_count(2 * DPA_NUM_COUNTERS + OK);
      ( ANY_PORT_PIPE_2 ,  P_INPUT, S_EXP ,  W_NOT_SEEN, T_NO_TIMEOUT ) : pkt_count(2 * DPA_NUM_COUNTERS + OK);
      ( ANY_PORT_PIPE_2 ,  P_INPUT, S_EXP ,  W_NOT_SEEN,    T_TIMEOUT ) : pkt_count(2 * DPA_NUM_COUNTERS + OK_TIMEOUT);
      ( ANY_PORT_PIPE_2 ,  P_INPUT, S_EXP ,      W_SEEN, T_NO_TIMEOUT ) : pkt_count(2 * DPA_NUM_COUNTERS + RE);
      ( ANY_PORT_PIPE_2 ,  P_INPUT, S_EXP ,      W_SEEN,    T_TIMEOUT ) : pkt_count(2 * DPA_NUM_COUNTERS + RE_TIMEOUT);
      ( ANY_PORT_PIPE_2 ,  P_INPUT, S_OLD ,           _,            _ ) : pkt_count(2 * DPA_NUM_COUNTERS + OL);
      ( ANY_PORT_PIPE_2 ,  P_INPUT, S_HI  ,           _,            _ ) : pkt_count(2 * DPA_NUM_COUNTERS + HI);
      ( ANY_PORT_PIPE_2 ,  P_SYN  ,  _    ,           _,            _ ) : pkt_count(2 * DPA_NUM_COUNTERS + SY);
      ( ANY_PORT_PIPE_2 ,         _,  _    ,           _,           _ ) : pkt_count(2 * DPA_NUM_COUNTERS + MISS);

      ( REC_PORT_PIPE_3 ,  _      , _     ,  _         ,            _ ) : pkt_count(3 * DPA_NUM_COUNTERS + RC);
      ( ANY_PORT_PIPE_3 ,  P_INPUT, S_NEW ,           _,            _ ) : pkt_count(3 * DPA_NUM_COUNTERS + OK);
      ( ANY_PORT_PIPE_3 ,  P_INPUT, S_EXP ,  W_NOT_SEEN, T_NO_TIMEOUT ) : pkt_count(3 * DPA_NUM_COUNTERS + OK);
      ( ANY_PORT_PIPE_3 ,  P_INPUT, S_EXP ,  W_NOT_SEEN,    T_TIMEOUT ) : pkt_count(3 * DPA_NUM_COUNTERS + OK_TIMEOUT);
      ( ANY_PORT_PIPE_3 ,  P_INPUT, S_EXP ,      W_SEEN, T_NO_TIMEOUT ) : pkt_count(3 * DPA_NUM_COUNTERS + RE);
      ( ANY_PORT_PIPE_3 ,  P_INPUT, S_EXP ,      W_SEEN,    T_TIMEOUT ) : pkt_count(3 * DPA_NUM_COUNTERS + RE_TIMEOUT);
      ( ANY_PORT_PIPE_3 ,  P_INPUT, S_OLD ,           _,            _ ) : pkt_count(3 * DPA_NUM_COUNTERS + OL);
      ( ANY_PORT_PIPE_3 ,  P_INPUT, S_HI  ,           _,            _ ) : pkt_count(3 * DPA_NUM_COUNTERS + HI);
      ( ANY_PORT_PIPE_3 ,  P_SYN  ,  _    ,           _,            _ ) : pkt_count(3 * DPA_NUM_COUNTERS + SY);
      ( ANY_PORT_PIPE_3 ,        _,  _    ,           _,            _ ) : pkt_count(3 * DPA_NUM_COUNTERS + MISS);
    }
  }
  #undef DPA_NUM_COUNTERS
#endif

  apply {
    M.dpa.world_n = H.dpa.world.n; // save it to restore it later if needed
    counter_last_table.apply();
    counter_table.apply();
#if DPA_COUNTERS
    pkt_counter_table.apply();
#endif
  }
}

control dpa_state( inout headers_t H, inout ingress_metadata_t M,
                  in ingress_intrinsic_metadata_t IM,
                  in ingress_intrinsic_metadata_from_parser_t PIM) {
  dpa_timer() timer;
  dpa_bitmap() bitmap;
  dpa_counter() counter;
  apply {
    timer.apply(H, M, IM);
    bitmap.apply(H, M, IM);
    counter.apply(H, M, IM);
  }
}

#endif