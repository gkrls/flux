#ifndef DPA_CONFIG_P4
#define DPA_CONFIG_P4

// CONSTANTS/SETTINGS (for Tofino 1)
#define REGISTER_MAX_ALLOC_64 16384
#define REGISTER_MAX_ALLOC_32 2 * REGISTER_MAX_ALLOC_64
#define REGISTER_MAX_ALLOC_16 2 * REGISTER_MAX_ALLOC_32
#define REGISTER_MAX_ALLOC_8  2 * REGISTER_MAX_ALLOC_16


#define PIPE_MASK 0x180

#define DPA_COMM_UDP 1
#define DPA_COMM_RDMA 2
#define DPA_MAX_WORKERS 32
#define DPA_REDUCER_SINGLE 1
#define DPA_REDUCER_DOUBLE 2


// -------------

#if  defined(DPA_CONFIG)
#warning "Using Configuration (file)"
#include DPA_CONFIG
#elif defined(DPA_CONFIG_RAW)
#warning "Using Configuration (raw)"
#else
#error "No user config"
#include DPA_CONFIG_DEFAULT
#endif

// -------------


#if defined(PIPE_2) && defined(PIPE_3)
#define QUAD_PIPE 1
#warning "QUAD_PIPE"
#else
#define DUAL_PIPE 1
// define PIPE_2 PIPE_3 so that we don't have to if/else the macros below.
// This is fine because they will not be used anywhere
#warning "DUAL_PIPE: defining PIPE_2 and PIPE_3 as PIPE_1"
#define PIPE_2 PIPE_1
#define PIPE_3 PIPE_1
#endif

#define ANY_PORT_ANY_PIPE _
#define ANY_PORT_PIPE_0 (PIPE_0 << 7) &&& PIPE_MASK
#define ANY_PORT_PIPE_1 (PIPE_1 << 7) &&& PIPE_MASK
#define ANY_PORT_PIPE_2 (PIPE_2 << 7) &&& PIPE_MASK
#define ANY_PORT_PIPE_3 (PIPE_3 << 7) &&& PIPE_MASK


// #define REC_PORT_PIPE_0 68
// #define REC_PORT_PIPE_1 196
// #define REC_PORT_PIPE_2 324
// #define REC_PORT_PIPE_3 452

#ifdef DPA_BACKWARD_PORT
#warning "RECIRCULATION PORT BASE: User-defined backward loopback port. See backward loopback chain in bfrt cli output"
#define REC_PORT_BASE DPA_BACKWARD_PORT
#elif TOFINO == 1
#warning "RECIRCULATION PORT BASE: 68"
#define REC_PORT_BASE 68
#else
#warning "RECIRCULATION PORT BASE: 6"
#define REC_PORT_BASE 6
#endif


#define REC_PORT_PIPE_0 (((bit<2>) PIPE_0) ++ ((bit<7>) REC_PORT_BASE))
#define REC_PORT_PIPE_1 (((bit<2>) PIPE_1) ++ ((bit<7>) REC_PORT_BASE))
#define REC_PORT_PIPE_2 (((bit<2>) PIPE_2) ++ ((bit<7>) REC_PORT_BASE))
#define REC_PORT_PIPE_3 (((bit<2>) PIPE_3) ++ ((bit<7>) REC_PORT_BASE))
#define RECIRCULATION_PORT(pipe) (((bit<2>) pipe) ++ ((bit<7>) REC_PORT_BASE))

#if defined(TOFINO_MODEL) && (TOFINO_MODEL > 0)
#warning "TOFINO MODEL"
    // We cannot set ports in loopback mode in the model, so we will
    // use the internal recirculation ports for the forward passes
    #define LPB_PORT_PIPE_0 REC_PORT_PIPE_0
    #define LPB_PORT_PIPE_1 REC_PORT_PIPE_1
    #define LPB_PORT_PIPE_2 REC_PORT_PIPE_2
    #define LPB_PORT_PIPE_3 REC_PORT_PIPE_3
    #define LOOPBACK_PORT(pipe) RECIRCULATION_PORT(pipe)
#else
#warning "TOFINO ASIC"
    // The loopback port is basically the same "local" ingress port but
    // at the specified pipe. Tofino gives us a nice way to figure this
    // port number by only changing the top 2 bits of the ingress_port.
    // Bottom 7 bits remain the same for the same port across all pipes.
    #define LPB_PORT_PIPE_0 ((bit<2>) PIPE_0) ++ IM.ingress_port[6:0]
    #define LPB_PORT_PIPE_1 ((bit<2>) PIPE_1) ++ IM.ingress_port[6:0]
    #define LPB_PORT_PIPE_2 ((bit<2>) PIPE_2) ++ IM.ingress_port[6:0]
    #define LPB_PORT_PIPE_3 ((bit<2>) PIPE_3) ++ IM.ingress_port[6:0]
    #define LOOPBACK_PORT(pipe) ((bit<2>) (pipe)) ++ IM.ingress_port[6:0]
#endif



#ifndef DPA_COUNTERS
#define DPA_COUNTERS 0
#endif

#endif // DPA_CONFIG_P4