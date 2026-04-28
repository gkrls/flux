#ifndef DPA_DROPSIM_P4
#define DPA_DROPSIM_P4

typedef bit<16> dpa_dropsim_t;

control dpa_dropsim(inout headers_t H) {

  dpa_dropsim_t rand = 0;
  Random<dpa_dropsim_t>() rng;
  Register<dpa_dropsim_t, bit<8>>(DPA_SESSIONS) dropsim_thres;
  RegisterAction<dpa_dropsim_t, bit<16>, bit<1>>(dropsim_thres) sim = {
    void apply(inout dpa_dropsim_t thresh, out bit<1> result) {
      if ( rand < thresh)
        result = 1;
      else
        result = 0;
    }
  };

  action dropsim(bit<16> internal_job_id) {
    // TODO
  }

  table dropsim_table {
    key = { H.dpa.session: exact; }
    actions = { NoAction; dropsim; }
    const default_action = NoAction;
    const size = DPA_SESSIONS;
  }

  apply {
    rand = rng.get();
    dropsim_table.apply();
  }
}

control dpa_dropsim_egress(inout headers_t H, inout egress_intrinsic_metadata_for_deparser_t DIM) {

  dpa_dropsim_t rand = 0;
  Random<dpa_dropsim_t>() rng;
  Register<dpa_dropsim_t, bit<8>>(DPA_SESSIONS) dropsim_thres;
  RegisterAction<dpa_dropsim_t, bit<16>, bit<1>>(dropsim_thres) sim = {
    void apply(inout dpa_dropsim_t thresh, out bit<1> result) {
      if ( rand < thresh)
        result = 1;
      else
        result = 0;
    }
  };

  action dropsim(bit<16> internal_job_id) {
    // Reuse the debug flags, no need for metadata
    // H.dpa.flags.u2 = sim.execute(internal_job_id);
    // M.dpa.dropsim = sim.execute(internal_job_id);
    DIM.drop_ctl[0:0] = sim.execute(internal_job_id);
  }

  table dropsim_table {
    key = { H.dpa.session: exact; }
    actions = { NoAction; dropsim; }
    const default_action = NoAction;
    const size = DPA_SESSIONS;
  }

  apply {
    rand = rng.get();
    dropsim_table.apply();
  }
}



#endif