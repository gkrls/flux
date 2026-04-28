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
    H.dpa.bitmap = H.dpa.bitmap | M.dpa.bitmap_pre;
    // We only multicast when the packet triggers completion (normal or forced)
    // Thus, the count is always positive
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.world.k = (dpa_world_size_t) M.dpa.count;
    H.dpa.flags.u1 = 0; // re
    H.dpa.flags.u2 = 0;
    H.dpa.flags.old = 0;
    H.dpa.flags.syn = 0;
    M.dpa.setInvalid();
  }
  action unicast()   {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    /// This bitmap contains the workers that are part of this slot
    /// Note that this COULD include stragglers for the current sequence
    /// Even though the bitmap might contain them, the counter is the ground truth
    /// Setting the bitmap here only says: "These workers i have seen for this sequence number"
    /// Setting the bitmap like this does not really affect the protocol, but it can help
    /// us with some debugging and testing
    H.dpa.bitmap = M.dpa.bitmap_pre;
    // We only unicast if the slot has been completed earlier, i.e count is negative, so we need to flip it
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.world.k = -((dpa_world_size_t) M.dpa.count);
    H.dpa.flags.u1 = 1; // re
    H.dpa.flags.old = 0;
    H.dpa.flags.syn = 0;
    M.dpa.setInvalid();
  }
  // BAD
  action unicast_bad() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.flags.bad = 1;
    M.dpa.setInvalid();
  }
  action unicast_bad_1() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.offset = M.dpa.sequence;
    H.dpa.flags.bad = 1;
    H.dpa.flags.u1 = 1;
    M.dpa.setInvalid();
  }
  action unicast_bad_2() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.flags.bad = 1;
    H.dpa.flags.u1 = 1;
    H.dpa.flags.u2 = 1;
    M.dpa.setInvalid();
  }
  action unicast_syn_complete_exp() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.bitmap = M.dpa.bitmap_pre;
    // H.dpa.offset = H.dpa.offset + M.dpa.sequence;
    H.dpa.offset = H.dpa.seq;
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.world.k = -((dpa_world_size_t) M.dpa.count); // SYN cannot complete a slot. Thus, count is negative, so we need to flip it
    H.dpa.flags.u1 = 0; // re
    H.dpa.flags.syn = 1;
    M.dpa.setInvalid();
  }
  action unicast_syn_complete_old() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.bitmap = M.dpa.bitmap_pre;
    H.dpa.offset = H.dpa.seq - M.dpa.sequence; // old seq => M.dpa.sequence negative => H.dpa.seq = H.dpa.seq + -M.dpa.sequence
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.world.k = -((dpa_world_size_t) M.dpa.count); // SYN cannot complete a slot. Thus, count is negative, so we need to flip it
    H.dpa.flags.bad = 0;
    H.dpa.flags.u1  = 0; // re
    H.dpa.flags.syn = 1;
    M.dpa.setInvalid();
  }
  // OLD
  action unicast_old_complete() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    // H.dpa.seq = -M.dpa.sequence;
    H.dpa.bitmap = M.dpa.bitmap_pre;
    H.dpa.offset = H.dpa.seq - M.dpa.sequence; // old seq => M.dpa.sequence negative => H.dpa.seq = H.dpa.seq + -M.dpa.sequence
    // H.dpa.offset = H.dpa.seq + M.dpa.sequence;
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.world.k = -((dpa_world_size_t) M.dpa.count);
    H.dpa.flags.bad = 0;
    H.dpa.flags.u1  = 0; // re
    H.dpa.flags.old = 1;
    H.dpa.flags.syn = 1; // can be used as SYN
    M.dpa.setInvalid();
    /// Values are unusable.
    /// We should probably invalidate them to send a smaller packet 
  }
  action unicast_old_incomplete() {
    TIM.ucast_egress_port = M.dpa.ingress_port;
    TIM.bypass_egress = 0;
    DIM.drop_ctl[0:0] = 0;
    H.dpa.world.n = M.dpa.world_n;
    H.dpa.bitmap = M.dpa.bitmap_pre;
    H.dpa.offset = H.dpa.seq - M.dpa.sequence; // old seq => M.dpa.sequence negative => H.dpa.seq = H.dpa.seq + -M.dpa.sequence
    // H.dpa.offset = H.dpa.seq + M.dpa.sequence;
    H.dpa.world.k = -((dpa_world_size_t) M.dpa.count);
    H.dpa.flags.bad = 0;
    H.dpa.flags.u1  = 0; // re
    H.dpa.flags.old = 1;
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
    H.dpa.world.n = M.dpa.world_n;
  }
  action continue_output_1(bit<2> next_pipe) {
    TIM.ucast_egress_port = RECIRCULATION_PORT(next_pipe);
    TIM.bypass_egress = 1;
    DIM.drop_ctl[0:0] = 0;
    M.dpa.kind = P_OUTPUT_1;
    H.dpa.world.n = M.dpa.world_n;
    /// As we go backwards dpa_payload_1 is generally invalid because we never parse it
    /// Except in the last pipe (the one that starts the backwards pass)
    /// Because on that pipe, output_0 is coupled with input, both dpa_payload_0 and dpa_payload_1
    /// are valid, dpa_payload_0 is written on the input pass, and then continue_output_1 is
    /// issued. So we need to invalidate dpa_payload_1 to "make space", i.e. not emit it.
    /// The header dpa_payload_1 will be valid by the next ingress parser, and thus will be
    /// emitted by the next deparser.
    /// Note that this is only relevant when we have dual mode reducer. In single mode
    /// dpa_values_1 is always invalid and this action is never called, so it doesn't matter.
    H.dpa_payload_1.setInvalid();
  }
  action continue_input(bit<2> next_pipe) {
    TIM.ucast_egress_port = LOOPBACK_PORT(next_pipe);
    TIM.bypass_egress = 1;
    DIM.drop_ctl[0:0] = 0;
    M.dpa.kind = P_INPUT;
    H.dpa.world.n = M.dpa.world_n;
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
            M.dpa.sequence   : ternary;
            M.dpa.bitmap     : ternary;
            M.dpa.count      : ternary; }
    actions = { NoAction; drop; multicast;
                unicast; unicast_bad; unicast_bad_1; unicast_bad_2;
                unicast_syn_complete_exp; unicast_syn_complete_old; // unicast_syn_incomplete; 
                unicast_old_complete; unicast_old_incomplete; 
                continue_output_0; continue_output_1; continue_input; }
    const default_action = drop;

    const entries =  {
      /// If we have a SYN a packet we either drop or unicast, both in the first pipe only.
      /// If the slot is incomplete the drop is obvious.
      /// If the slot is complete however, we unicast early, without reading results because results are unusable to the straggler unless
      /// it has the exponents of the previous result (i.e. the other "version" of the slot), which is completely unlikely/unrealistic, 
      /// unless we start considering microstraggle events (much less than 0.5 - 2x iteration time).
      /// Thus only exponents are usable and those can read just from pipe 0.
      ///
      /// Note that this also means that when we aggregate, the straggler loses 1 packet of updates.
      /// But that's fine for now, we only train with floats anyway
      ///
      /// IMPORTANT: If we ever want to read results for a SYN response we might be able to do a (faster) traversal like this:
      ///
      ///   [forward] OUT1 -> OUT1 -> OUT1 -> OUT1 -> [backward] OUT2 -> OUT2 -> OUT2 -> OUT2
      ///
      /// However, there are nasty races lurking. For instance, if the SYN is right behind the packet that completes the slot
      /// it can overtake it when moving across pipes, due to queueing at different ports. 
      /// Currenly the rest of the program only reads state for SYN packets.
      /// So things could go wrong. Best case the SYN result just gets dropped somewhere after the first pipe.
      /// Bottomline, reading results for SYN packets might not be that simple and needs to be given some thought!
      ( _  , ANY_PORT_PIPE_0,   P_BAD   , _      , _          , _                  ) : unicast_bad();
      ( _  , ANY_PORT_PIPE_0,   P_SYN   , _      , _          , C_INCOMPLETE       ) : drop();
      // ( _  , ANY_PORT_PIPE_0,   P_SYN   , _      , _          , _                  ) : unicast_syn();
      ( _  , ANY_PORT_PIPE_0,   P_SYN   , S_EXP  , _          , _                  ) : unicast_syn_complete_exp(); // this should never really happen i think
      ( _  , ANY_PORT_PIPE_0,   P_SYN   , S_OLD  , _          , _                  ) : unicast_syn_complete_old();
      ( _  , ANY_PORT_PIPE_0,   P_SYN   , S_HI   , _          , _                  ) : unicast_bad(); // this should definitely never happen

      /// For bad and old packets we also unicast immediately (i think its safe)
      ( _  , ANY_PORT_PIPE_0, P_INPUT   , S_OLD  ,  W_UNSEEN, C_INCOMPLETE       ) : unicast_old_incomplete();   // this will cause the worker to enter SYN
      ( _  , ANY_PORT_PIPE_0, P_INPUT   , S_OLD  ,  W_UNSEEN, _                  ) : unicast_old_complete();     // this CAN be used by the worker as implicit SYN
      ( _  , ANY_PORT_PIPE_0, P_INPUT   , S_OLD  ,      W_SEEN, _                  ) : unicast_bad_2(); //NoAction(); // drop();

#if DPA_REDUCER_MODE == DPA_REDUCER_SINGLE
      /// 1 pipe
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , C_INCOMPLETE       ) : drop();
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , C_COMPLETE_NOW     ) : multicast();
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , C_COMPLETE_EARLIER ) : unicast();
      /// 2 pipes
      ( _2_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_1);
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , S_OK    , _         , C_INCOMPLETE       ) : drop();
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , S_OK    , _         , _                  ) : continue_output_0(PIPE_0);
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , C_COMPLETE_NOW     ) : multicast();
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , C_COMPLETE_EARLIER ) : unicast();
  #ifdef QUAD_PIPE
      /// 3 pipes
      ( _3_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_2);
      ( _3_, ANY_PORT_PIPE_2, P_INPUT   , S_OK    , _         , C_INCOMPLETE       ) : drop();
      ( _3_, ANY_PORT_PIPE_2, P_INPUT   , S_OK    , _         , _                  ) : continue_output_0(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_0(PIPE_0);
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , C_COMPLETE_NOW     ) : multicast();
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , C_COMPLETE_EARLIER ) : unicast();
      /// 4 pipes
      ( _4_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_3);
      ( _4_, ANY_PORT_PIPE_3, P_INPUT   , S_OK    , _         , C_INCOMPLETE       ) : drop();
      ( _4_, ANY_PORT_PIPE_3, P_INPUT   , S_OK    , _         , _                  ) : continue_output_0(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_0(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_0(PIPE_0);
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , C_COMPLETE_NOW     ) : multicast();
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , C_COMPLETE_EARLIER ) : unicast();
  #endif
#else
      /// 1 pipe
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , C_INCOMPLETE       ) : drop();
      ( _1_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , _                  ) : continue_output_1(PIPE_0);
      ( _1_, ANY_PORT_PIPE_0, P_OUTPUT_1, S_OK    , _         , C_COMPLETE_NOW     ) : multicast();
      ( _1_, ANY_PORT_PIPE_0, P_OUTPUT_1, S_OK    , _         , C_COMPLETE_EARLIER ) : unicast();
      /// 2 pipes
      ( _2_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_1);
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , S_OK    , _         , C_INCOMPLETE       ) : drop();
      ( _2_, ANY_PORT_PIPE_1, P_INPUT   , S_OK    , _         , _                  ) : continue_output_1(PIPE_1);
      ( _2_, ANY_PORT_PIPE_1, P_OUTPUT_1, S_OK    , _         , _                  ) : continue_output_0(PIPE_0);
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_1(PIPE_0);
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_1, S_OK    , _         , C_COMPLETE_NOW     ) : multicast();
      ( _2_, ANY_PORT_PIPE_0, P_OUTPUT_1, S_OK    , _         , C_COMPLETE_EARLIER ) : unicast();
  #ifdef QUAD_PIPE
      /// 3 pipes
      ( _3_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_2);
      ( _3_, ANY_PORT_PIPE_2, P_INPUT   , S_OK    , _         , C_INCOMPLETE       ) : drop();
      ( _3_, ANY_PORT_PIPE_2, P_INPUT   , S_OK    , _         , _                  ) : continue_output_1(PIPE_2);
      ( _3_, ANY_PORT_PIPE_2, P_OUTPUT_1, S_OK    , _         , _                  ) : continue_output_0(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_1(PIPE_1);
      ( _3_, ANY_PORT_PIPE_1, P_OUTPUT_1, S_OK    , _         , _                  ) : continue_output_0(PIPE_0);
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_1(PIPE_0);
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_1, S_OK    , _         , C_COMPLETE_NOW     ) : multicast();
      ( _3_, ANY_PORT_PIPE_0, P_OUTPUT_1, S_OK    , _         , C_COMPLETE_EARLIER ) : unicast();
      /// 4 pipes
      ( _4_, ANY_PORT_PIPE_0, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_INPUT   , S_OK    , _         , _                  ) : continue_input(PIPE_3);
      ( _4_, ANY_PORT_PIPE_3, P_INPUT   , S_OK    , _         , C_INCOMPLETE       ) : drop();
      ( _4_, ANY_PORT_PIPE_3, P_INPUT   , S_OK    , _         , _                  ) : continue_output_1(PIPE_3);
      ( _4_, ANY_PORT_PIPE_3, P_OUTPUT_1, S_OK    , _         , _                  ) : continue_output_0(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_1(PIPE_2);
      ( _4_, ANY_PORT_PIPE_2, P_OUTPUT_1, S_OK    , _         , _                  ) : continue_output_0(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_1(PIPE_1);
      ( _4_, ANY_PORT_PIPE_1, P_OUTPUT_1, S_OK    , _         , _                  ) : continue_output_0(PIPE_0);
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_0, S_OK    , _         , _                  ) : continue_output_1(PIPE_0);
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_1, S_OK    , _         , C_COMPLETE_NOW     ) : multicast();
      ( _4_, ANY_PORT_PIPE_0, P_OUTPUT_1, S_OK    , _         , C_COMPLETE_EARLIER ) : unicast();
  #endif
#endif
      ( _  , ANY_PORT_PIPE_0, P_INPUT   , S_HI    , _         , _                  ) : unicast_bad_1();
    }
  }

  #include "counters/ingress_next_counter.p4"

  apply {
#if DPA_COUNTERS
    pkt_counter_table.apply();
#endif
    next_table.apply();
  }
}

#endif