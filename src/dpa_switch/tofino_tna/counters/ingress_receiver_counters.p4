#if DPA_COUNTERS

#define COUNTER_SIZE 32
Counter<bit<64>, bit<8>>(COUNTER_SIZE, CounterType_t.PACKETS) ingress_receiver_pkt_counter;

action count_recv_pipe0_none()      { ingress_receiver_pkt_counter.count(0); }

action count_recv_pipe0_input_old() { ingress_receiver_pkt_counter.count(1); }
action count_recv_pipe0_input_exp() { ingress_receiver_pkt_counter.count(2); }
action count_recv_pipe0_input_new() { ingress_receiver_pkt_counter.count(3); }
action count_recv_pipe0_input_hi()  { ingress_receiver_pkt_counter.count(4); }

action count_recv_pipe0_syn_old() { ingress_receiver_pkt_counter.count(5); }
action count_recv_pipe0_syn_exp() { ingress_receiver_pkt_counter.count(6); }
action count_recv_pipe0_syn_new() { ingress_receiver_pkt_counter.count(7); }
action count_recv_pipe0_syn_hi()  { ingress_receiver_pkt_counter.count(8); }

action count_recv_pipe0_bad()     { ingress_receiver_pkt_counter.count(9);}
action count_recv_pipe0_other()   { ingress_receiver_pkt_counter.count(19);}

action count_recv_pipeN_none()    { ingress_receiver_pkt_counter.count(20); }
action count_recv_pipeN_syn()     { ingress_receiver_pkt_counter.count(21); }
action count_recv_pipeN_bad()     { ingress_receiver_pkt_counter.count(22); }

action count_recv_pipeN_other()   { ingress_receiver_pkt_counter.count(29); }

table ingress_receiver_pkt_counter_table {
  key = { IM.ingress_port        : ternary;
          M.dpa.kind             : ternary;
          M.dpa.sequence         : ternary; }
  actions = {NoAction; 
             count_recv_pipe0_none; count_recv_pipe0_bad; count_recv_pipe0_other; 
             count_recv_pipe0_input_old; count_recv_pipe0_input_exp; count_recv_pipe0_input_new; count_recv_pipe0_input_hi;
             count_recv_pipe0_syn_old; count_recv_pipe0_syn_new; count_recv_pipe0_syn_exp; count_recv_pipe0_syn_hi;
             count_recv_pipeN_none; count_recv_pipeN_syn; count_recv_pipeN_bad; count_recv_pipeN_other; }
  const size = COUNTER_SIZE;
  const default_action = NoAction;
  const entries = {
    ( ANY_PORT_PIPE_0, P_NONE , _     ) : count_recv_pipe0_none();
    ( ANY_PORT_PIPE_0, P_INPUT, S_OLD ) : count_recv_pipe0_input_old();
    ( ANY_PORT_PIPE_0, P_INPUT, S_EXP ) : count_recv_pipe0_input_exp();
    ( ANY_PORT_PIPE_0, P_INPUT, S_NEW ) : count_recv_pipe0_input_new();
    ( ANY_PORT_PIPE_0, P_INPUT, S_HI  ) : count_recv_pipe0_input_hi();


    ( ANY_PORT_PIPE_0, P_SYN  , S_OLD ) : count_recv_pipe0_syn_old(); 
    ( ANY_PORT_PIPE_0, P_SYN  , S_NEW ) : count_recv_pipe0_syn_new();
    ( ANY_PORT_PIPE_0, P_SYN  , S_EXP ) : count_recv_pipe0_syn_exp();
    ( ANY_PORT_PIPE_0, P_SYN  , S_EXP ) : count_recv_pipe0_syn_hi();
    ( ANY_PORT_PIPE_0, P_BAD  , _     ) : count_recv_pipe0_bad();
    ( ANY_PORT_PIPE_0, _      , _     ) : count_recv_pipe0_other();

    (               _, P_NONE , _     ) : count_recv_pipeN_none();
    (               _, P_SYN  , _     ) : count_recv_pipeN_syn();
    (               _, P_BAD  , _     ) : count_recv_pipeN_bad();
    (               _,     _  , _     ) : count_recv_pipeN_other();

  }
}

#undef COUNTER_SIZE



#define COUNTER_SIZE 32
Counter<bit<64>, bit<8>>(COUNTER_SIZE, CounterType_t.PACKETS) ingress_receiver_old_counter;

action count_old_port(bit<8> idx) { ingress_receiver_old_counter.count(idx); }

table ingress_receiver_old_counter_table {
  key = { IM.ingress_port        : ternary;
          M.dpa.kind             : ternary;
          M.dpa.sequence         : ternary; }
  actions = {NoAction; count_old_port; }
  const size = 6;
  const default_action = NoAction;
  const entries = {
    (136, P_INPUT, S_OLD ) : count_old_port(0);
    (144, P_INPUT, S_OLD ) : count_old_port(1);
    (152, P_INPUT, S_OLD ) : count_old_port(2);
    (160, P_INPUT, S_OLD ) : count_old_port(3);
    (168, P_INPUT, S_OLD ) : count_old_port(4);
    (176, P_INPUT, S_OLD ) : count_old_port(5);
  }
}

Counter<bit<64>, bit<8>>(COUNTER_SIZE, CounterType_t.PACKETS) ingress_receiver_syn_counter;

action count_syn_port(bit<8> idx) { ingress_receiver_syn_counter.count(idx); }

table ingress_receiver_syn_counter_table {
  key = { IM.ingress_port        : ternary;
          M.dpa.kind             : ternary; }
  actions = {NoAction; count_syn_port; }
  const size = 6;
  const default_action = NoAction;
  const entries = {
    (136, P_SYN) : count_syn_port(0);
    (144, P_SYN) : count_syn_port(1);
    (152, P_SYN) : count_syn_port(2);
    (160, P_SYN) : count_syn_port(3);
    (168, P_SYN) : count_syn_port(4);
    (176, P_SYN) : count_syn_port(5);
  }
}

#undef COUNTER_SIZE

#endif

