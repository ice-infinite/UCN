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

/* Optional product payload prefix for high-risk Q0 commands.  It is not part
 * of every Service message and therefore adds no Router RAM or normal-frame
 * overhead.  issued_at_ms is meaningful only inside a product-defined shared
 * 32-bit millisecond time domain. */
#define UCN_SERVICE_COMMAND_GUARD_BYTES ((size_t)12U)
/* Optional business Result Endpoint payload prefix.  It is not a transport
 * ACK and is present only when a product chooses to report a command stage. */
#define UCN_SERVICE_RESULT_HEADER_BYTES ((size_t)8U)

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

/* This is an ownership/acceptance result, never an end-to-end delivery ACK.
 * REMOTE_ENQUEUED means only that the local Router now owns a fixed copy; a
 * later Bridge/Core/Link operation may still fail. */
typedef enum ucn_service_acceptance {
    UCN_SERVICE_ACCEPTANCE_NONE = 0,
    UCN_SERVICE_ACCEPTANCE_LOCAL_DELIVERED = 1,
    UCN_SERVICE_ACCEPTANCE_REMOTE_ENQUEUED = 2
} ucn_service_acceptance_t;

/* Stable asynchronous-stage vocabulary.  Only the first two stages are
 * returned synchronously by ucn_service_send_ex().  LINK_QUEUE_ACCEPTED is a
 * local Bridge/Core/Link admission event.  The two REMOTE stages require an
 * explicit business Result Endpoint message and are never inferred from
 * UCN_OK or from the transport itself. */
typedef enum ucn_service_async_stage {
    UCN_SERVICE_STAGE_NONE = 0,
    UCN_SERVICE_STAGE_LOCAL_INBOXED = 1,
    UCN_SERVICE_STAGE_REMOTE_ROUTER_QUEUED = 2,
    UCN_SERVICE_STAGE_LINK_QUEUE_ACCEPTED = 3,
    UCN_SERVICE_STAGE_REMOTE_INBOXED = 4,
    UCN_SERVICE_STAGE_REMOTE_EXECUTED = 5
} ucn_service_async_stage_t;

/* Minimal product-neutral status carried by the optional Result Endpoint
 * header.  detail_code is left to the product ABI. */
typedef enum ucn_service_result_status {
    UCN_SERVICE_RESULT_ACCEPTED = 0,
    UCN_SERVICE_RESULT_SUCCEEDED = 1,
    UCN_SERVICE_RESULT_REJECTED = 2,
    UCN_SERVICE_RESULT_FAILED = 3,
    UCN_SERVICE_RESULT_EXPIRED = 4
} ucn_service_result_status_t;

typedef struct ucn_service_command_guard {
    uint32_t command_id;
    uint32_t issued_at_ms;
    uint16_t valid_for_ms;
    ucn_endpoint_t result_endpoint;
    uint8_t flags;
} ucn_service_command_guard_t;

typedef struct ucn_service_result_header {
    uint32_t command_id;
    ucn_service_async_stage_t stage;
    ucn_service_result_status_t status;
    uint16_t detail_code;
} ucn_service_result_header_t;

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
    /* Fail closed at Bridge handler installation unless this remote Q0
     * Endpoint has a product Validator.  Local Fast Path intentionally
     * bypasses the Bridge; the owning execution Task must still validate. */
    bool require_remote_q0_validator;
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
    uint32_t binding_purges;
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
/* Setting ready=false also purges this Binding's Q0 FIFO or Q1 Latest value.
 * A restarted Task can therefore never consume a command/sample queued before
 * it became not-ready.  The caller/RTOS Port must serialize Router access. */
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

/* Backward-compatible extended form that makes the successful ownership
 * boundary explicit.  acceptance may be NULL. */
ucn_result_t ucn_service_send_ex(ucn_service_router_t *router,
                                 ucn_node_id_t destination,
                                 ucn_service_id_t source_service_id,
                                 ucn_endpoint_t endpoint,
                                 ucn_traffic_class_t traffic_class,
                                 const uint8_t *payload,
                                 uint16_t payload_length,
                                 ucn_service_acceptance_t *acceptance);

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

/* Maps the synchronous ownership result onto the shared stage vocabulary.
 * NONE and invalid values map to UCN_SERVICE_STAGE_NONE. */
ucn_service_async_stage_t ucn_service_acceptance_stage(
    ucn_service_acceptance_t acceptance);

ucn_result_t ucn_service_command_guard_encode(
    const ucn_service_command_guard_t *guard,
    uint8_t output[UCN_SERVICE_COMMAND_GUARD_BYTES]);
ucn_result_t ucn_service_command_guard_decode(
    const uint8_t *payload,
    size_t payload_length,
    ucn_service_command_guard_t *guard);
/* Consumer-side validation for products that use the optional guard.  A
 * product must supply a shared/synchronized issued_at clock; otherwise use a
 * product generation/lease rule instead of pretending local uptimes match. */
ucn_result_t ucn_service_command_guard_validate(
    const ucn_service_command_guard_t *guard,
    uint32_t now_ms,
    bool has_last_command_id,
    uint32_t last_command_id);

/* Optional business Result Endpoint header.  REMOTE_INBOXED is valid only
 * with ACCEPTED; REMOTE_EXECUTED requires a terminal status.  Encoding is
 * fixed big-endian and does not alter the UCN v4 frame header. */
ucn_result_t ucn_service_result_header_encode(
    const ucn_service_result_header_t *header,
    uint8_t output[UCN_SERVICE_RESULT_HEADER_BYTES]);
ucn_result_t ucn_service_result_header_decode(
    const uint8_t *payload,
    size_t payload_length,
    ucn_service_result_header_t *header);
/* Correlation is deliberately stateless: the product owns timeout/pending
 * command storage.  Source Node/session checks remain product policy. */
bool ucn_service_result_matches_command(
    const ucn_service_command_guard_t *command,
    ucn_endpoint_t received_endpoint,
    const ucn_service_result_header_t *result);

#ifdef __cplusplus
}
#endif

#endif
