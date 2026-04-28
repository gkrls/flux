#ifndef P4_REDUCE
#define P4_REDUCE


control dpa_value_reducer_single ( in ingress_metadata_t M, in dpa_slot_t idx, in dpa_value_t val, inout dpa_value_t res)
                                 ( bit<32> SIZE ) {

  Register<dpa_value_t, dpa_slot_t>(SIZE) R;

  RegisterAction<dpa_value_t, dpa_slot_t, dpa_value_t>(R) read_only = {
    void apply(inout dpa_value_t reg, out dpa_value_t ret) {
      ret = reg;
    }
  };

  RegisterAction<dpa_value_t, dpa_slot_t, dpa_value_t>(R) write_read = {
    void apply(inout dpa_value_t reg, out dpa_value_t ret) {
      reg = val;
      ret = reg;
    }
  };
  RegisterAction<dpa_value_t, dpa_slot_t, dpa_value_t>(R) update_read = {
    void apply(inout dpa_value_t reg, out dpa_value_t ret) {
      reg = reg |+| val;
      ret = reg;
    }
  };

  action should_never_happen_1() {}
  action should_never_happen_2() {}
  action read()   { res = read_only.execute(idx);   }
  action write()  { res = write_read.execute(idx);  }
  action update() { res = update_read.execute(idx); }

  table reducer_table {
    key = { M.dpa.kind      : ternary;
            M.dpa.bitmap_chk: ternary;
            M.dpa.count     : ternary;
            M.dpa.first     : ternary;       }
    actions = { NoAction; read; write; update; should_never_happen_1; should_never_happen_2; }
    const size = 8;
    const default_action = NoAction;
    const entries = {
      ( P_OUTPUT_0, _       , _                 , _) : read();
      ( P_INPUT   , B_UNSEEN, C_COMPLETE_EARLIER, _) : should_never_happen_1(); 
      ( P_INPUT   , B_UNSEEN, C_COMPLETE_NOW    , 1) : write();  // first packet completion -- single-worker jobs
      ( P_INPUT   , B_UNSEEN, C_COMPLETE_NOW    , 0) : update(); // normal completion
      ( P_INPUT   , B_UNSEEN, C_INCOMPLETE      , 1) : write();  // first packet
      ( P_INPUT   , B_UNSEEN, C_INCOMPLETE      , 0) : update(); // intermediate packet
      ( P_INPUT   , B_SEEN  , C_COMPLETE_EARLIER, _) : read();   // retransmission for complete slot
      ( P_INPUT   , B_SEEN  , C_COMPLETE_NOW    , _) : should_never_happen_2(); // impossible
      // ( P_INPUT   , B_SEEN  , C_INCOMPLETE       ) : NoAction(); // unecessary, only for completeness
    }
  }

  apply { reducer_table.apply(); }
}

struct dpa_value_pair_t {
  dpa_value_t hi;
  dpa_value_t lo;
}

control dpa_value_reducer_double ( in ingress_metadata_t M, in dpa_slot_t idx,
                                   in dpa_value_t val_hi, in dpa_value_t val_lo, inout dpa_value_t res_hi, inout dpa_value_t res_lo )
                                 ( bit<32> SIZE ) {

  Register<dpa_value_pair_t, dpa_slot_t>(SIZE) R;

  RegisterAction<dpa_value_pair_t, dpa_slot_t, dpa_value_t>(R) read_only_hi = {
    void apply(inout dpa_value_pair_t reg, out dpa_value_t ret) {
      ret = reg.hi;
    }
  };

  RegisterAction<dpa_value_pair_t, dpa_slot_t, dpa_value_t>(R) read_only_lo = {
    void apply(inout dpa_value_pair_t reg, out dpa_value_t ret) {
      ret = reg.lo;
    }
  };

  RegisterAction<dpa_value_pair_t, dpa_slot_t, dpa_value_t>(R) write_both_read_lo = {
    void apply(inout dpa_value_pair_t reg, out dpa_value_t ret) {
      reg.hi = val_hi;
      reg.lo = val_lo;
      ret = reg.lo;
    }
  };

  RegisterAction<dpa_value_pair_t, dpa_slot_t, dpa_value_t>(R) update_both_read_lo = {
    void apply(inout dpa_value_pair_t reg, out dpa_value_t ret) {
      reg.hi = reg.hi |+| val_hi;
      reg.lo = reg.lo |+| val_lo;
      ret = reg.lo;
    }
  };

  action should_never_happen_1() {}
  action should_never_happen_2() {}
  action read_hi() { res_hi = read_only_hi.execute(idx); }
  action read_lo() { res_lo = read_only_lo.execute(idx); }
  action write()   { res_lo = write_both_read_lo.execute(idx); }
  action update()  { res_lo = update_both_read_lo.execute(idx); }

  table reducer_table {
    key = { M.dpa.kind      : ternary;
            M.dpa.bitmap_chk: ternary;
            M.dpa.count     : ternary;
            M.dpa.first     : ternary;       }
    actions = { NoAction; read_lo; read_hi; write; update; should_never_happen_1; should_never_happen_2; }
    const size = 9;
    const default_action = NoAction;
    const entries = {
      ( P_OUTPUT_0, _       , _                 , _) : read_lo();
      ( P_OUTPUT_1, _       , _                 , _) : read_hi();

      ( P_INPUT   , B_UNSEEN, C_COMPLETE_EARLIER, _) : should_never_happen_2();
      ( P_INPUT   , B_UNSEEN, _                 , 1) : write();  // first packet, C_COMPLETE_NOW = single worker job, C_INCOMPLETE = n workers
      ( P_INPUT   , B_UNSEEN, _                 , 0) : update(); // intermediate packet
      // ( P_INPUT   , B_UNSEEN, C_COMPLETE_NOW    , 1) : write();   // first packet completion -- single-worker jobs
      // ( P_INPUT   , B_UNSEEN, C_COMPLETE_NOW    , 0) : update();  // normal completion -> agg and read lo

      ( P_INPUT   , B_SEEN  , C_COMPLETE_EARLIER, _) : read_lo(); // retransmission for complete slot, nothing to do on input
      ( P_INPUT   , B_SEEN  , C_COMPLETE_NOW    , _) : should_never_happen_1(); // impossible
      
      // ( P_INPUT   , B_SEEN  , C_INCOMPLETE       ) : NoAction(); // unecessary, only for completeness
    }
  }

  apply { reducer_table.apply(); }
}


// Currently the exponent reducer is only meant to run on pipe 0 which is why we need to pass the headers.
control dpa_exponent_reducer( in headers_t H, in ingress_metadata_t M, in dpa_slot_t idx,
                              in dpa_exponent_t val, inout dpa_exponent_t res)
                            ( bit<32> SIZE ) {

  Register<dpa_exponent_t, dpa_slot_t>(SIZE) R;

  RegisterAction<dpa_exponent_t, dpa_slot_t, dpa_exponent_t>(R) read_only = {
    void apply(inout dpa_exponent_t reg, out dpa_exponent_t ret) {
      ret = reg;
    }
  };

  RegisterAction<dpa_exponent_t, dpa_slot_t, dpa_exponent_t>(R) write_read = {
    void apply(inout dpa_exponent_t reg, out dpa_exponent_t ret) {
      reg = val;
      ret = reg;
    }
  };

  RegisterAction<dpa_exponent_t, dpa_slot_t, dpa_exponent_t>(R) update_read = {
    void apply(inout dpa_exponent_t reg, out dpa_exponent_t ret) {
      reg = max(reg, val);
      ret = reg;
    }
  };

  action should_never_happen_1() {}
  action should_never_happen_2() {}
  action read()   { res = read_only.execute(idx);   }
  action write()  { res = write_read.execute(idx);  }
  action update() { res = update_read.execute(idx); }
  action should_never_happen() {}

  table reducer_table {
    key = { M.dpa.kind      : ternary;
            M.dpa.bitmap_chk: ternary;
            M.dpa.count     : ternary;
            M.dpa.first     : ternary; }
    actions = { NoAction; read; write; update; should_never_happen_1; should_never_happen_2; }
    const size = 10;
    const entries = {

#if DPA_REDUCER_MODE == DPA_REDUCER_DOUBLE
      ( P_OUTPUT_1, _       ,  _                , _) : read();
#else
      ( P_OUTPUT_0, _       ,  _                , _) : read();
#endif
      ( P_INPUT   , B_UNSEEN, C_COMPLETE_EARLIER, _) : should_never_happen_1(); 
      // ( P_INPUT   , B_UNSEEN, C_COMPLETE_NOW     ) : update();  // normal completion -> agg and read lo
      ( P_INPUT   , B_UNSEEN, C_COMPLETE_NOW    , 1) : write();   // first packet completion -- single-worker jobs
      ( P_INPUT   , B_UNSEEN, C_COMPLETE_NOW    , 0) : update();  // normal completion -> agg and read lo
      ( P_INPUT   , B_UNSEEN, C_INCOMPLETE      , 1) : write();  // first packet
      ( P_INPUT   , B_UNSEEN, C_INCOMPLETE      , 0) : update(); // intermediate packet
      ( P_INPUT   , B_UNSEEN, C_INCOMPLETE      , _) : update();  // intermediate
      ( P_INPUT   , B_SEEN  , C_COMPLETE_EARLIER, _) : read();    // retransmission for complete slot
      ( P_INPUT   , B_SEEN  , C_COMPLETE_NOW    , _) : should_never_happen_2(); // impossible
      // ( P_INPUT   , B_SEEN  , C_INCOMPLETE       ) : NoAction(); // unecessary, only for completeness
    }
  }

  apply { reducer_table.apply(); }
}


control dpa_reduce( inout headers_t H, inout ingress_metadata_t M, in ingress_intrinsic_metadata_t IM) {

#define EXPONENT_REDUCER(i)        dpa_exponent_reducer(DPA_SLOTS) exponent_##i;
#define EXPONENT_REDUCER_APPLY(i)  exponent_##i.apply(H, M, H.dpa.slot, H.dpa.quant.q##i, H.dpa.quant.q##i);

#if DPA_REDUCER_MODE == DPA_REDUCER_DOUBLE
  #define VALUE_REDUCER(i)       dpa_value_reducer_double(DPA_SLOTS) value_##i;
  #define VALUE_REDUCER_APPLY(i) value_##i.apply(M, H.dpa.slot, H.dpa_payload_1.v##i, H.dpa_payload_0.v##i, H.dpa_payload_1.v##i, H.dpa_payload_0.v##i);
#else
  #define VALUE_REDUCER(i)       dpa_value_reducer_single(DPA_SLOTS) value_##i;
  #define VALUE_REDUCER_APPLY(i) value_##i.apply(M, H.dpa.slot, H.dpa_payload_0.v##i, H.dpa_payload_0.v##i);
#endif


#if DPA_EXPONENTS == 1
  EXPONENT_REDUCER(0)
#elif DPA_EXPONENTS == 2
  EXPONENT_REDUCER(0)
  EXPONENT_REDUCER(1)
#elif DPA_EXPONENTS == 4
  EXPONENT_REDUCER(0)
  EXPONENT_REDUCER(1)
  EXPONENT_REDUCER(2)
  EXPONENT_REDUCER(3)
#else
  #error "Invalid number of DPA_EXPONENTS"
#endif

#if (DPA_REDUCERS == 1)
  VALUE_REDUCER(00)
#elif (DPA_REDUCERS == 2)
  VALUE_REDUCER(00)
  VALUE_REDUCER(01)
#elif (DPA_REDUCERS == 4)
  VALUE_REDUCER(00)
  VALUE_REDUCER(01)
  VALUE_REDUCER(02)
  VALUE_REDUCER(03)
#elif (DPA_REDUCERS == 8)
  VALUE_REDUCER(00)
  VALUE_REDUCER(01)
  VALUE_REDUCER(02)
  VALUE_REDUCER(03)
  VALUE_REDUCER(04)
  VALUE_REDUCER(05)
  VALUE_REDUCER(06)
  VALUE_REDUCER(07)
#elif (DPA_REDUCERS == 16)
  VALUE_REDUCER(00)
  VALUE_REDUCER(01)
  VALUE_REDUCER(02)
  VALUE_REDUCER(03)
  VALUE_REDUCER(04)
  VALUE_REDUCER(05)
  VALUE_REDUCER(06)
  VALUE_REDUCER(07)
  VALUE_REDUCER(08)
  VALUE_REDUCER(09)
  VALUE_REDUCER(10)
  VALUE_REDUCER(11)
  VALUE_REDUCER(12)
  VALUE_REDUCER(13)
  VALUE_REDUCER(14)
  VALUE_REDUCER(15)
#else
  VALUE_REDUCER(00)
  VALUE_REDUCER(01)
  VALUE_REDUCER(02)
  VALUE_REDUCER(03)
  VALUE_REDUCER(04)
  VALUE_REDUCER(05)
  VALUE_REDUCER(06)
  VALUE_REDUCER(07)
  VALUE_REDUCER(08)
  VALUE_REDUCER(09)
  VALUE_REDUCER(10)
  VALUE_REDUCER(11)
  VALUE_REDUCER(12)
  VALUE_REDUCER(13)
  VALUE_REDUCER(14)
  VALUE_REDUCER(15)
  VALUE_REDUCER(16)
  VALUE_REDUCER(17)
  VALUE_REDUCER(18)
  VALUE_REDUCER(19)
  VALUE_REDUCER(20)
  VALUE_REDUCER(21)
  VALUE_REDUCER(22)
  VALUE_REDUCER(23)
  VALUE_REDUCER(24)
  VALUE_REDUCER(25)
  VALUE_REDUCER(26)
  VALUE_REDUCER(27)
  VALUE_REDUCER(28)
  VALUE_REDUCER(29)
  VALUE_REDUCER(30)
  VALUE_REDUCER(31)
#endif


// #if DPA_COUNTERS
//   #define DPA_NUM_COUNTERS 8
//   #define SHOULD_NOT_HAPPEN_1 6
//   #define SHOULD_NOT_HAPPEN_2 7
//   Counter<bit<64>, bit<8>>(4 * DPA_NUM_COUNTERS, CounterType_t.PACKETS) pkt_counter;
//   action pkt_count(bit<8> idx) { pkt_counter.count(idx); }
//   table pkt_counter_table {
//     key = { IM.ingress_port : ternary;
//             M.dpa.kind      : ternary;
//             M.dpa.bitmap_chk: ternary;
//             M.dpa.count     : ternary;
//             M.dpa.first     : ternary;       }
//     actions = { pkt_count; NoAction; }
//     const size = 32;
//     const default_action = NoAction;
//     const entries = {
//       ( ANY_PORT_PIPE_0, P_OUTPUT_0, _       , _                 , _) : pkt_count(1);
//       ( ANY_PORT_PIPE_0, P_OUTPUT_1, _       , _                 , _) : pkt_count(2);
//       ( ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN, C_COMPLETE_EARLIER, _) : pkt_count(SHOULD_NOT_HAPPEN_1);
//       ( ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN, _                 , 1) : pkt_count(3);
//       ( ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN, _                 , 0) : pkt_count(4); // intermediate packet
//       ( ANY_PORT_PIPE_0, P_INPUT   , B_SEEN  , C_COMPLETE_EARLIER, _) : pkt_count(5); // retransmission for complete slot, nothing to do on input
//       ( ANY_PORT_PIPE_0, P_INPUT   , B_SEEN  , C_COMPLETE_NOW    , _) : pkt_count(SHOULD_NOT_HAPPEN_2); // impossible
//       ( ANY_PORT_PIPE_0, _         , _       , _                 , _) : pkt_count(0);

//       ( ANY_PORT_PIPE_1, P_OUTPUT_0, _       , _                 , _) : pkt_count(DPA_NUM_COUNTERS + 1); 
//       ( ANY_PORT_PIPE_1, P_OUTPUT_1, _       , _                 , _) : pkt_count(DPA_NUM_COUNTERS + 2); 
//       ( ANY_PORT_PIPE_1, P_INPUT   , B_UNSEEN, C_COMPLETE_EARLIER, _) : pkt_count(DPA_NUM_COUNTERS + SHOULD_NOT_HAPPEN_1);
//       ( ANY_PORT_PIPE_1, P_INPUT   , B_UNSEEN, _                 , 1) : pkt_count(DPA_NUM_COUNTERS + 3);
//       ( ANY_PORT_PIPE_1, P_INPUT   , B_UNSEEN, _                 , 0) : pkt_count(DPA_NUM_COUNTERS + 4);
//       ( ANY_PORT_PIPE_1, P_INPUT   , B_SEEN  , C_COMPLETE_EARLIER, _) : pkt_count(DPA_NUM_COUNTERS + 5);
//       ( ANY_PORT_PIPE_1, P_INPUT   , B_SEEN  , C_COMPLETE_NOW    , _) : pkt_count(DPA_NUM_COUNTERS + SHOULD_NOT_HAPPEN_2); // impossible
//       ( ANY_PORT_PIPE_1, _         , _       , _                 , _) : pkt_count(0);
//     }
//   }
//   #undef DPA_NUM_COUNTERS
// #endif

  apply {
// #if DPA_COUNTERS
//   pkt_counter_table.apply();
// #endif
    // if ( H.ml.flags.dbg_dry == 0) {
#if DPA_EXPONENTS == 1
  EXPONENT_REDUCER_APPLY(0)
#elif DPA_EXPONENTS == 2
  EXPONENT_REDUCER_APPLY(0)
  EXPONENT_REDUCER_APPLY(1)
#elif DPA_EXPONENTS == 4
  EXPONENT_REDUCER_APPLY(0)
  EXPONENT_REDUCER_APPLY(1)
  EXPONENT_REDUCER_APPLY(2)
  EXPONENT_REDUCER_APPLY(3)
#else
  #error "Invalid number of DPA_EXPONENTS"
#endif

#if (DPA_REDUCERS == 1)
  VALUE_REDUCER_APPLY(00)
#elif (DPA_REDUCERS == 2)
  VALUE_REDUCER_APPLY(00)
  VALUE_REDUCER_APPLY(01)
#elif (DPA_REDUCERS == 4)
  VALUE_REDUCER_APPLY(00)
  VALUE_REDUCER_APPLY(01)
  VALUE_REDUCER_APPLY(02)
  VALUE_REDUCER_APPLY(03)
#elif (DPA_REDUCERS == 8)
  VALUE_REDUCER_APPLY(00)
  VALUE_REDUCER_APPLY(01)
  VALUE_REDUCER_APPLY(02)
  VALUE_REDUCER_APPLY(03)
  VALUE_REDUCER_APPLY(04)
  VALUE_REDUCER_APPLY(05)
  VALUE_REDUCER_APPLY(06)
  VALUE_REDUCER_APPLY(07)
#elif (DPA_REDUCERS == 16)
  VALUE_REDUCER_APPLY(00)
  VALUE_REDUCER_APPLY(01)
  VALUE_REDUCER_APPLY(02)
  VALUE_REDUCER_APPLY(03)
  VALUE_REDUCER_APPLY(04)
  VALUE_REDUCER_APPLY(05)
  VALUE_REDUCER_APPLY(06)
  VALUE_REDUCER_APPLY(07)
  VALUE_REDUCER_APPLY(08)
  VALUE_REDUCER_APPLY(09)
  VALUE_REDUCER_APPLY(10)
  VALUE_REDUCER_APPLY(11)
  VALUE_REDUCER_APPLY(12)
  VALUE_REDUCER_APPLY(13)
  VALUE_REDUCER_APPLY(14)
  VALUE_REDUCER_APPLY(15)
#else
  VALUE_REDUCER_APPLY(00)
  VALUE_REDUCER_APPLY(01)
  VALUE_REDUCER_APPLY(02)
  VALUE_REDUCER_APPLY(03)
  VALUE_REDUCER_APPLY(04)
  VALUE_REDUCER_APPLY(05)
  VALUE_REDUCER_APPLY(06)
  VALUE_REDUCER_APPLY(07)
  VALUE_REDUCER_APPLY(08)
  VALUE_REDUCER_APPLY(09)
  VALUE_REDUCER_APPLY(10)
  VALUE_REDUCER_APPLY(11)
  VALUE_REDUCER_APPLY(12)
  VALUE_REDUCER_APPLY(13)
  VALUE_REDUCER_APPLY(14)
  VALUE_REDUCER_APPLY(15)
  VALUE_REDUCER_APPLY(16)
  VALUE_REDUCER_APPLY(17)
  VALUE_REDUCER_APPLY(18)
  VALUE_REDUCER_APPLY(19)
  VALUE_REDUCER_APPLY(20)
  VALUE_REDUCER_APPLY(21)
  VALUE_REDUCER_APPLY(22)
  VALUE_REDUCER_APPLY(23)
  VALUE_REDUCER_APPLY(24)
  VALUE_REDUCER_APPLY(25)
  VALUE_REDUCER_APPLY(26)
  VALUE_REDUCER_APPLY(27)
  VALUE_REDUCER_APPLY(28)
  VALUE_REDUCER_APPLY(29)
  VALUE_REDUCER_APPLY(30)
  VALUE_REDUCER_APPLY(31)
#endif





  }

#undef VALUE_REDUCER
#undef VALUE_REDUCER_APPLY
#undef EXPONENT_REDUCER
#undef EXPONENT_REDUCER_APPLY

}

#endif