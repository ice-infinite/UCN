#ifndef UCN_SERVICE_H
#define UCN_SERVICE_H

#include "ucn/ucn_endpoint.h"
#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* T25.1 Service Router lives outside the UCN Core/RTOS boundary.  The
 * defaults match the R1 contract, but every limit remains a compile-time
 * profile decision. */
#ifndef UCN_SERVICE_MAX_BINDINGS
#define UCN_SERVICE_MAX_BINDINGS ((uint8_t)6U)
#endif

#ifndef UCN_SERVICE_MAX_PAYLOAD_BYTES
#define UCN_SERVICE_MAX_PAYLOAD_BYTES ((uint16_t)32U)
#endif

#ifndef UCN_SERVICE_REMOTE_TX_Q0_DEPTH
#define UCN_SERVICE_REMOTE_TX_Q0_DEPTH ((uint8_t)4U)
#endif

#ifndef UCN_SERVICE_REMOTE_TX_Q1_DEPTH
#define UCN_SERVICE_REMOTE_TX_Q1_DEPTH ((uint8_t)4U)
#endif

#ifndef UCN_SERVICE_MAX_Q0_BINDINGS
#define UCN_SERVICE_MAX_Q0_BINDINGS ((uint8_t)2U)
#endif

#ifndef UCN_SERVICE_MAX_Q1_BINDINGS
#define UCN_SERVICE_MAX_Q1_BINDINGS ((uint8_t)4U)
#endif

#ifndef UCN_SERVICE_Q0_INBOX_DEPTH
#define UCN_SERVICE_Q0_INBOX_DEPTH ((uint8_t)4U)
#endif

#define UCN_SERVICE_ID_NONE ((uint8_t)0U)
#define UCN_SERVICE_ID_MAX ((uint8_t)31U)
#define UCN_SERVICE_BINDING_INDEX_NONE UINT8_MAX
#define UCN_SERVICE_TRAFFIC_MASK(traffic_class) \
    ((uint8_t)(UINT8_C(1) << (uint8_t)(traffic_class)))
#define UCN_SERVICE_SOURCE_MASK(service_id) \
    (UINT32_C(1) << (uint32_t)(service_id))

typedef char ucn_service_payload_must_fit_core_frame[
    UCN_SERVICE_MAX_PAYLOAD_BYTES <= UCN_MAX_PAYLOAD_BYTES ? 1 : -1];
typedef char ucn_service_q0_binding_limit_must_fit[
    UCN_SERVICE_MAX_Q0_BINDINGS <= UCN_SERVICE_MAX_BINDINGS ? 1 : -1];
typedef char ucn_service_q1_binding_limit_must_fit[
    UCN_SERVICE_MAX_Q1_BINDINGS <= UCN_SERVICE_MAX_BINDINGS ? 1 : -1];

typedef uint8_t ucn_service_id_t;

typedef enum ucn_service_delivery_mode {
    UCN_SERVICE_DELIVERY_Q0_FIFO = 0,
    UCN_SERVICE_DELIVERY_Q1_LATEST = 1
} ucn_service_delivery_mode_t;

/* One static Endpoint has one local owner in R1.  A source Service mask is
 * checked only for local Fast-Path/outbound calls; remote source authority
 * remains the existing Core Security/Endpoint ACL responsibility. */
typedef struct ucn_service_binding {
    ucn_endpoint_t endpoint;
    ucn_service_id_t owner_service_id;
    uint16_t max_payload_length;
    uint8_t allowed_traffic_mask;
    ucn_service_delivery_mode_t delivery_mode;
    uint32_t allowed_local_source_mask;
    bool accept_remote;
    bool enabled_at_boot;
} ucn_service_binding_t;

typedef struct ucn_service_router_config {
    ucn_node_id_t local_node_id;
    const ucn_service_binding_t *bindings;
    uint8_t binding_count;
} ucn_service_router_config_t;

/* A Router owns every queued payload.  A caller may immediately reuse its
 * input buffer after a successful send/deliver call. */
typedef struct ucn_service_message {
    ucn_node_id_t source_node_id;
    ucn_node_id_t destination_node_id;
    ucn_service_id_t source_service_id;
    ucn_endpoint_t endpoint;
    ucn_traffic_class_t traffic_class;
    uint16_t payload_length;
    uint8_t payload[UCN_SERVICE_MAX_PAYLOAD_BYTES];
} ucn_service_message_t;

typedef struct ucn_service_q0_inbox {
    ucn_service_message_t messages[UCN_SERVICE_Q0_INBOX_DEPTH];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} ucn_service_q0_inbox_t;

typedef struct ucn_service_q1_inbox {
    bool occupied;
    ucn_service_message_t latest;
} ucn_service_q1_inbox_t;

typedef struct ucn_service_remote_q1_slot {
    bool occupied;
    ucn_service_message_t message;
} ucn_service_remote_q1_slot_t;

typedef struct ucn_service_binding_state {
    bool ready;
    uint8_t q0_inbox_index;
    uint8_t q1_inbox_index;
} ucn_service_binding_state_t;

typedef struct ucn_service_stats {
    uint32_t local_delivered;
    uint32_t remote_enqueued;
    uint32_t inbound_delivered;
    uint32_t unknown_endpoint;
    uint32_t not_ready;
    uint32_t local_acl_rejected;
    uint32_t remote_rejected;
    uint32_t length_rejected;
    uint32_t traffic_rejected;
    uint32_t q0_inbox_full;
    uint32_t q1_overwrites;
    uint32_t remote_q0_full;
    uint32_t remote_q1_full;
    uint32_t remote_q1_overwrites;
    uint32_t inbox_reads;
    uint32_t remote_tx_reads;
} ucn_service_stats_t;

typedef struct ucn_service_router {
    ucn_service_router_config_t config;
    ucn_service_binding_state_t binding_states[UCN_SERVICE_MAX_BINDINGS];
    ucn_service_q0_inbox_t q0_inboxes[UCN_SERVICE_MAX_Q0_BINDINGS];
    ucn_service_q1_inbox_t q1_inboxes[UCN_SERVICE_MAX_Q1_BINDINGS];
    ucn_service_q0_inbox_t remote_q0;
    ucn_service_remote_q1_slot_t remote_q1[UCN_SERVICE_REMOTE_TX_Q1_DEPTH];
    ucn_service_stats_t stats;
} ucn_service_router_t;

ucn_result_t ucn_service_router_init(ucn_service_router_t *router,
                                     const ucn_service_router_config_t *config);
ucn_result_t ucn_service_set_ready(ucn_service_router_t *router,
                                   ucn_endpoint_t endpoint,
                                   bool ready);

/* Local destination: copy to the target Inbox.  Remote destination: copy to
 * the fixed Remote TX queue.  This function never calls a Link or touches
 * ucn_node_t. */
ucn_result_t ucn_service_send(ucn_service_router_t *router,
                              ucn_node_id_t destination,
                              ucn_service_id_t source_service_id,
                              ucn_endpoint_t endpoint,
                              ucn_traffic_class_t traffic_class,
                              const uint8_t *payload,
                              uint16_t payload_length);

/* Called by the later Protocol-Task bridge after Core has accepted an
 * endpoint frame for this Node.  It never performs routing or decryption. */
ucn_result_t ucn_service_deliver_remote(ucn_service_router_t *router,
                                        const ucn_frame_t *frame);

/* Consumer/Port reads a message from its only bound Endpoint.  Q0 is FIFO;
 * Q1 consumes the current Latest Value. */
ucn_result_t ucn_service_inbox_take(ucn_service_router_t *router,
                                    ucn_service_id_t owner_service_id,
                                    ucn_endpoint_t endpoint,
                                    ucn_service_message_t *message);

/* Only the future Protocol Task bridge may call this.  Q0 is always selected
 * before Q1; obtaining a message transfers its fixed copy to the caller. */
ucn_result_t ucn_service_remote_tx_take(ucn_service_router_t *router,
                                        ucn_service_message_t *message);

const ucn_service_stats_t *ucn_service_get_stats(const ucn_service_router_t *router);

#ifdef __cplusplus
}
#endif

#endif
