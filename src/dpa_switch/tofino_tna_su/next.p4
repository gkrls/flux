#ifndef P4_NEXT
#define P4_NEXT
control dpa_next( inout headers_t H, inout ingress_metadata_t M,
                  in ingress_intrinsic_metadata_t IM,
                  inout ingress_intrinsic_metadata_for_deparser_t DIM,
                  inout ingress_intrinsic_metadata_for_tm_t TIM ) {

  action drop() {
    DIM.drop_ctl[0:0] = 1;
  }
  action multicast() {
    TIM.mcast_grp_a = M.dpa.mgid;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.bitmap = M.dpa.bitmap_pre | H.dpa.bitmap;
    H.dpa.flags.re = 0; // re
    M.dpa.setInvalid();
  }
  action unicast()   {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    // For the ns program this bitmap should always be ...1111.
    H.dpa.bitmap = M.dpa.bitmap_pre | H.dpa.bitmap;
    H.dpa.flags.re = 1; // re
    H.dpa.flags.old = 0;
    M.dpa.setInvalid();
  }
  action unicast_bad() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.flags.old = 0;
    H.dpa.flags.re = 0;
    M.dpa.setInvalid();
  }
  action unicast_bad_complete_earlier() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.flags.bad = 1;
    H.dpa.flags.old = 0;
    H.dpa.flags.re = 0;
    M.dpa.setInvalid();
  }
  action unicast_bad_complete_now() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.flags.bad = 1;
    H.dpa.flags.old = 0;
    H.dpa.flags.re = 0;
    // H.dpa.flags.syn = 0;
    M.dpa.setInvalid();
  }

  // IMPORTANT: Do NOT name this (or any) argument "pipe" because the compiler
  //            generates control plane code where "pipe" is actually a parameter
  //            and for some CRAZY reason our parameter name is used verbatim, so
  //            we get a conflict. This causes the userspace driver (e.g bfshell)
  //            to fail to start.
  action continue_output_0(bit<2> next_pipe) {
    TIM.ucast_egress_port = RECIRCULATION_PORT(next_pipe);
    TIM.bypass_egress = 1;
    DIM.drop_ctl[0:0] = 0;
    M.dpa.kind = P_OUTPUT_0;
    // H.dpa.world.n = M.dpa.world_n;
  }
  action continue_output_1(bit<2> next_pipe) {
    TIM.ucast_egress_port = RECIRCULATION_PORT(next_pipe);
    TIM.bypass_egress = 1;
    DIM.drop_ctl[0:0] = 0;
    M.dpa.kind = P_OUTPUT_1;
    H.dpa_payload_1.setInvalid();
  }
  action continue_input(bit<2> next_pipe) {
    TIM.ucast_egress_port = LOOPBACK_PORT(next_pipe);
    TIM.bypass_egress = 1;
    DIM.drop_ctl[0:0] = 0;
    M.dpa.kind = P_INPUT;
    H.dpa_payload_0.setInvalid();
    H.dpa_payload_1.setInvalid();
  }

#define _1_ 0 // using 1 pipe
#define _2_ 1 // using 2 pipes
#define _3_ 2 // using 3 pipes
#define _4_ 3 // using 4 pipes

  table next_table {
    key = { H.dpa.flags.pipes: ternary;
            IM.ingress_port  : ternary;
            M.dpa.kind       : ternary;
            M.dpa.bitmap_chk : ternary;
            M.dpa.count      : ternary; }
    actions = { drop; multicast;
                unicast; unicast_bad; unicast_bad_complete_earlier; unicast_bad_complete_now;
                continue_output_0; continue_output_1; continue_input; }
    const default_action = drop;

    const entries =  {
      ( _  , ANY_PORT_PIPE_0, P_BAD     , _       , _                  ) : unicast_bad();
      ( _  , _              , _         , B_UNSEEN, C_COMPLETE_EARLIER ) : unicast_bad_complete_earlier(); // sanity check
    // ( _  , ANY_PORT_PIPE_0, P_INPUT   , B_SEEN, C_INCOMPLETE       ) : drop(); // drop at first pipe if seen and incomplete?
      // ( _  , _              , _         , B_SEEN  , C_COMPLETE_NOW     ) : unicast_bad_complete_now(); // sanity check
#if DPA_REDUCER_MODE == DPA_REDUCER_SINGLE
      /// 1 pipe
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : unicast();
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_NOW    ) : multicast();
      /// 2 pipes
      ( _2_, ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN , _                 ) : continue_input(PIPE_1);
      ( _2_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_input(PIPE_1);
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_output_0(PIPE_0);
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , _        , C_COMPLETE_NOW    ) : continue_output_0(PIPE_0);
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , C_COMPLETE_EARLIER) : unicast();
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , C_COMPLETE_NOW    ) : multicast();
  #ifdef QUAD_PIPE
      /// 3 pipes
      ( _3_, ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN , _                 ) : continue_input(PIPE_1);
      ( _3_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_input(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_INPUT   , _        , _                 ) : continue_input(PIPE_2);
      ( _3_, ANY_PORT_PIPE_2, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_output_0(PIPE_1);
      ( _3_, ANY_PORT_PIPE_2, P_INPUT   , _        , C_COMPLETE_NOW    ) : continue_output_0(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_OUTPUT_0, _        , _                 ) : continue_output_0(PIPE_0);
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , C_COMPLETE_EARLIER) : unicast();
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , C_COMPLETE_NOW    ) : multicast();
      /// 4 pipes
      ( _4_, ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN , _                 ) : continue_input(PIPE_1);
      ( _4_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_input(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_INPUT   , _        , _                 ) : continue_input(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_INPUT   , _        , _                 ) : continue_input(PIPE_3);
      ( _4_, ANY_PORT_PIPE_3, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_output_0(PIPE_2);
      ( _4_, ANY_PORT_PIPE_3, P_INPUT   , _        , C_COMPLETE_NOW    ) : continue_output_0(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_OUTPUT_0, _        , _                 ) : continue_output_0(PIPE_0);
      ( _4_, ANY_PORT_PIPE_1, P_OUTPUT_0, _        , _                 ) : continue_output_0(PIPE_0);
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , C_COMPLETE_EARLIER) : unicast();
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , C_COMPLETE_NOW    ) : multicast();
  #endif
#else
      /// 1 pipe
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_output_1(PIPE_0);
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_NOW    ) : continue_output_1(PIPE_0);
      ( _1_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_EARLIER) : unicast();
      ( _1_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_NOW    ) : multicast();
      /// 2 pipes
      ( _2_, ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN , _                 ) : continue_input(PIPE_1);
      ( _2_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_input(PIPE_1);
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_output_1(PIPE_1);
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , _        , C_COMPLETE_NOW    ) : continue_output_1(PIPE_1);
      ( _2_, ANY_PORT_PIPE_1, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_0);
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_0);
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_EARLIER) : unicast();
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_NOW    ) : multicast();
  #ifdef QUAD_PIPE
      /// 3 pipes
      ( _2_, ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN , _                 ) : continue_input(PIPE_1);
      ( _2_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_input(PIPE_1);
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , _        , _                 ) : continue_input(PIPE_2);
      ( _3_, ANY_PORT_PIPE_2, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_output_1(PIPE_2);
      ( _3_, ANY_PORT_PIPE_2, P_INPUT   , _        , C_COMPLETE_NOW    ) : continue_output_1(PIPE_2);
      ( _3_, ANY_PORT_PIPE_2, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_0);
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_0);
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_EARLIER) : unicast();
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_NOW    ) : multicast();
      /// 4 pipes
      // ( _4_, ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN , _                 ) : continue_input(PIPE_1);
      // ( _4_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_input(PIPE_1);
      // ( _4_, ANY_PORT_PIPE_1, P_INPUT   , _        , _                 ) : continue_input(PIPE_2);
      // ( _4_, ANY_PORT_PIPE_2, P_INPUT   , _        , _                 ) : continue_input(PIPE_3);
      // ( _4_, ANY_PORT_PIPE_3, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_output_1(PIPE_3);
      // ( _4_, ANY_PORT_PIPE_3, P_INPUT   , _        , C_COMPLETE_NOW    ) : continue_output_1(PIPE_3);
      // ( _4_, ANY_PORT_PIPE_3, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_2);
      // ( _4_, ANY_PORT_PIPE_2, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_2);
      // ( _4_, ANY_PORT_PIPE_2, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_1);
      // ( _4_, ANY_PORT_PIPE_1, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_1);
      // ( _4_, ANY_PORT_PIPE_1, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_0);
      // ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_0);
      // ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_EARLIER) : unicast();
      // ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_NOW    ) : multicast();
      ( _4_, ANY_PORT_PIPE_0, P_INPUT   , B_UNSEEN , _                 ) : continue_input(PIPE_1);
      ( _4_, ANY_PORT_PIPE_0, P_INPUT   , _        , C_COMPLETE_EARLIER) : continue_input(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_INPUT   , _        , _                 ) : continue_input(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_INPUT   , _        , _                 ) : continue_input(PIPE_3);
      ( _4_, ANY_PORT_PIPE_3, P_INPUT   , _        , C_INCOMPLETE      ) : drop();
      ( _4_, ANY_PORT_PIPE_3, P_INPUT   , _        , _                 ) : continue_output_1(PIPE_3);
      ( _4_, ANY_PORT_PIPE_3, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_OUTPUT_1, _        , _                 ) : continue_output_0(PIPE_0);
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_0, _        , _                 ) : continue_output_1(PIPE_0);
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_EARLIER) : unicast();
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_1, _        , C_COMPLETE_NOW    ) : multicast();
  #endif
#endif
    }
  }

// #if DPA_COUNTERS
//   Counter<bit<64>, bit<8>>(7, CounterType_t.PACKETS) pkt_counter;
//   action count_miss()                      { pkt_counter.count(0); }
//   action count_normal_incomplete()         { pkt_counter.count(1); }
//   action count_normal_complete()           { pkt_counter.count(2); }
//   action count_retransmission_incomplete() { pkt_counter.count(3); }
//   action count_retransmission_complete()   { pkt_counter.count(4); }
//   action count_unicast()                   { pkt_counter.count(5); }
//   action count_multicast()                 { pkt_counter.count(6); }

//   table pkt_counter_table {
//     key = {IM.ingress_port   : ternary;
//             M.dpa.kind       : ternary;
//             M.dpa.bitmap_chk : ternary;
//             M.dpa.count      : ternary; }
//     actions = {count_miss; count_normal_incomplete; count_normal_complete; count_retransmission_complete; count_retransmission_incomplete; 
//                count_unicast; count_multicast; NoAction; }
//     const default_action = NoAction;
//     const size = 16;
//     const entries = {
//       ( ANY_PORT_PIPE_0, P_INPUT,    B_UNSEEN, C_INCOMPLETE      ) : count_normal_incomplete();
//       ( ANY_PORT_PIPE_0, P_INPUT,    B_UNSEEN, C_COMPLETE_NOW    ) : count_normal_complete();
//       ( ANY_PORT_PIPE_0, P_INPUT,    B_SEEN  , C_INCOMPLETE      ) : count_retransmission_incomplete();
//       ( ANY_PORT_PIPE_0, P_INPUT,    B_SEEN  , C_COMPLETE_EARLIER) : count_retransmission_complete();
//       // ( ANY_PORT_PIPE_0, P_OUTPUT_0, _       , C_INCOMPLETE      ) : count_miss();
//       ( ANY_PORT_PIPE_0, P_OUTPUT_1, _       , C_COMPLETE_EARLIER) : count_unicast();
//       ( ANY_PORT_PIPE_0, P_OUTPUT_1, B_UNSEEN, C_COMPLETE_NOW    ) : count_multicast();
//       ( ANY_PORT_PIPE_0, P_OUTPUT_1, _       , _                 ) : count_miss();
//     }
//   }

// #endif

  apply {
// #if DPA_COUNTERS
//     pkt_counter_table.apply();
// #endif
    next_table.apply();
  }
}

#endif