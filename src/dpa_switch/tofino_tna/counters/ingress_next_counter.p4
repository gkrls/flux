#if DPA_COUNTERS
  Counter<bit<64>, bit<8>>(32, CounterType_t.PACKETS) ingress_next_pre_pkt_counter;

  action count_other()                { ingress_next_pre_pkt_counter.count(0); }

  action count_bad_other()            { ingress_next_pre_pkt_counter.count(1); }
  action count_bad_input_old_seen()   { ingress_next_pre_pkt_counter.count(2); }
  action count_bad_input_hi_seen()    { ingress_next_pre_pkt_counter.count(3); }
  action count_bad_input_hi_unseen()  { ingress_next_pre_pkt_counter.count(4); }
  action count_bad_syn_hi()           { ingress_next_pre_pkt_counter.count(5); }
  action count_bad_syn_complete_now() { ingress_next_pre_pkt_counter.count(6); }

  action count_syn_incomplete()       { ingress_next_pre_pkt_counter.count(7); }
  action count_syn_complete()         { ingress_next_pre_pkt_counter.count(8); }

  action count_old_incomplete()       { ingress_next_pre_pkt_counter.count(9); }
  action count_old_complete()         { ingress_next_pre_pkt_counter.count(10); }

  action count_new()                  { ingress_next_pre_pkt_counter.count(11); }
  action count_exp()                  { ingress_next_pre_pkt_counter.count(12); }

  table pkt_counter_table {
    key = { H.dpa.flags.pipes: ternary;
            IM.ingress_port  : ternary;
            M.dpa.kind       : ternary;
            M.dpa.sequence   : ternary;
            M.dpa.bitmap     : ternary;
            M.dpa.count      : ternary; }
    actions = { count_other; count_new; count_exp;
                count_bad_other; count_bad_input_old_seen; count_bad_input_hi_seen; count_bad_input_hi_unseen; 
                count_bad_syn_hi; count_bad_syn_complete_now;
                count_syn_incomplete; count_syn_complete; count_old_incomplete; count_old_complete; }
    const default_action = count_other;
    const size = 32;
    const entries = {
      ( _  , ANY_PORT_PIPE_0,   P_BAD,     _,          _, _                ) : count_bad_other();

      ( _  , ANY_PORT_PIPE_0, P_INPUT, S_OLD, W_NOT_SEEN, C_INCOMPLETE     ) : count_old_incomplete();
      ( _  , ANY_PORT_PIPE_0, P_INPUT, S_OLD, W_NOT_SEEN, _                ) : count_old_complete(); // guard for entry below
      ( _  , ANY_PORT_PIPE_0, P_INPUT, S_OLD,     W_SEEN, _                ) : count_bad_input_old_seen();

      ( _  , ANY_PORT_PIPE_0, P_INPUT, S_EXP,          _, _                ) : count_exp();
      ( _  , ANY_PORT_PIPE_0, P_INPUT, S_NEW,          _, _                ) : count_new(); 
      ( _  , ANY_PORT_PIPE_0, P_INPUT,  S_HI, W_NOT_SEEN, _                ) : count_bad_input_hi_unseen();
      ( _  , ANY_PORT_PIPE_0, P_INPUT,  S_HI,     W_SEEN, _                ) : count_bad_input_hi_seen();

      ( _  , ANY_PORT_PIPE_0,   P_SYN, S_EXP,         _, C_INCOMPLETE      ) : count_syn_incomplete();
      ( _  , ANY_PORT_PIPE_0,   P_SYN, S_OLD,         _, C_INCOMPLETE      ) : count_syn_incomplete();
      ( _  , ANY_PORT_PIPE_0,   P_SYN, S_EXP,         _, C_COMPLETE_EARLIER) : count_syn_complete();
      ( _  , ANY_PORT_PIPE_0,   P_SYN, S_OLD,         _, C_COMPLETE_EARLIER) : count_syn_complete();
      ( _  , ANY_PORT_PIPE_0,   P_SYN, _    ,         _, C_COMPLETE_NOW    ) : count_bad_syn_complete_now();
      ( _  , ANY_PORT_PIPE_0,   P_SYN, S_HI ,         _, _                 ) : count_bad_syn_hi();
      
    }
  }
#endif