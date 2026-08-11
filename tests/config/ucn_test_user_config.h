#ifndef UCN_TEST_USER_CONFIG_H
#define UCN_TEST_USER_CONFIG_H

/* Representative product overrides.  Unlisted values must continue to use
 * the defaults in ucn_config.h. */
#ifndef UCN_MAX_FRAME_BYTES
#define UCN_MAX_FRAME_BYTES ((size_t)128U)
#endif
#ifndef UCN_MAX_PAYLOAD_BYTES
#define UCN_MAX_PAYLOAD_BYTES ((size_t)64U)
#endif
#ifndef UCN_MAX_HOPS
#define UCN_MAX_HOPS ((uint8_t)8U)
#endif
#ifndef UCN_ADAPTER_RX_QUEUE_DEPTH
#define UCN_ADAPTER_RX_QUEUE_DEPTH 3U
#endif
#ifndef UCN_MAX_LINKS
#define UCN_MAX_LINKS ((size_t)3U)
#endif
#ifndef UCN_TX_Q0_DEPTH
#define UCN_TX_Q0_DEPTH ((size_t)2U)
#endif
#ifndef UCN_MAX_NEIGHBORS
#define UCN_MAX_NEIGHBORS ((size_t)4U)
#endif
#ifndef UCN_MAX_BEARERS_PER_NEIGHBOR
#define UCN_MAX_BEARERS_PER_NEIGHBOR ((size_t)1U)
#endif
#ifndef UCN_SERVICE_MAX_BINDINGS
#define UCN_SERVICE_MAX_BINDINGS ((uint8_t)4U)
#endif
#ifndef UCN_SERVICE_MAX_Q0_BINDINGS
#define UCN_SERVICE_MAX_Q0_BINDINGS ((uint8_t)1U)
#endif
#ifndef UCN_SERVICE_MAX_Q1_BINDINGS
#define UCN_SERVICE_MAX_Q1_BINDINGS ((uint8_t)3U)
#endif
#ifndef UCN_SERVICE_BRIDGE_MAX_VALIDATORS
#define UCN_SERVICE_BRIDGE_MAX_VALIDATORS ((uint8_t)1U)
#endif

#endif
