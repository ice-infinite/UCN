#ifndef UCN_NODE_H
#define UCN_NODE_H

#include "ucn/ucn_link.h"
#include "ucn/ucn_endpoint.h"
#include "ucn/ucn_neighbor.h"
#include "ucn/ucn_security.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UCN_MAX_LINKS
#define UCN_MAX_LINKS ((size_t)4U)
#endif

#ifndef UCN_TX_Q0_DEPTH
#define UCN_TX_Q0_DEPTH ((size_t)4U)
#endif

#ifndef UCN_TX_Q1_DEPTH
#define UCN_TX_Q1_DEPTH ((size_t)4U)
#endif

#ifndef UCN_MAX_ROUTES
#define UCN_MAX_ROUTES ((size_t)8U)
#endif

#ifndef UCN_SEEN_CACHE_SIZE
#define UCN_SEEN_CACHE_SIZE ((size_t)8U)
#endif

#ifndef UCN_MAX_ROUTE_DISCOVERIES
#define UCN_MAX_ROUTE_DISCOVERIES ((size_t)4U)
#endif

#ifndef UCN_MAX_CANDIDATE_ROUTES
#define UCN_MAX_CANDIDATE_ROUTES ((size_t)8U)
#endif

#ifndef UCN_ROUTE_ENTRY_LIFETIME_MS
#define UCN_ROUTE_ENTRY_LIFETIME_MS UINT32_C(30000)
#endif

#ifndef UCN_ROUTE_REQUEST_TIMEOUT_MS
#define UCN_ROUTE_REQUEST_TIMEOUT_MS UINT32_C(1000)
#endif

#ifndef UCN_ROUTE_REQUEST_MIN_INTERVAL_MS
#define UCN_ROUTE_REQUEST_MIN_INTERVAL_MS UINT32_C(100)
#endif

#ifndef UCN_ROUTE_REFRESH_ADVANCE_MS
#define UCN_ROUTE_REFRESH_ADVANCE_MS UINT32_C(6000)
#endif

#ifndef UCN_ROUTE_REFRESH_MIN_INTERVAL_MS
#define UCN_ROUTE_REFRESH_MIN_INTERVAL_MS UINT32_C(5000)
#endif

#ifndef UCN_ROUTE_CANDIDATE_TIMEOUT_MS
#define UCN_ROUTE_CANDIDATE_TIMEOUT_MS UINT32_C(3000)
#endif

#ifndef UCN_PATH_PROBE_REQUIRED_ACKS
#define UCN_PATH_PROBE_REQUIRED_ACKS ((uint8_t)3U)
#endif

#ifndef UCN_PATH_PROBE_INTERVAL_MS
#define UCN_PATH_PROBE_INTERVAL_MS UINT32_C(100)
#endif

#ifndef UCN_ROUTE_SWITCH_IMPROVEMENT_PERCENT
#define UCN_ROUTE_SWITCH_IMPROVEMENT_PERCENT ((uint8_t)20U)
#endif

#ifndef UCN_ROUTE_EPOCH_GRACE_MS
#define UCN_ROUTE_EPOCH_GRACE_MS UINT32_C(1000)
#endif

typedef char ucn_path_probe_required_acks_must_be_positive[
    UCN_PATH_PROBE_REQUIRED_ACKS > 0U ? 1 : -1];
typedef char ucn_route_refresh_advance_must_fit_lifetime[
    UCN_ROUTE_REFRESH_ADVANCE_MS < UCN_ROUTE_ENTRY_LIFETIME_MS ? 1 : -1];
typedef char ucn_route_switch_improvement_percent_must_be_less_than_100[
    UCN_ROUTE_SWITCH_IMPROVEMENT_PERCENT < 100U ? 1 : -1];

#ifndef UCN_UNKNOWN_LINK_ROUTE_COST
#define UCN_UNKNOWN_LINK_ROUTE_COST ((uint16_t)1000U)
#endif

#ifndef UCN_CONTROL_TOKEN_BURST
#define UCN_CONTROL_TOKEN_BURST ((uint8_t)4U)
#endif

#ifndef UCN_CONTROL_TOKEN_REFILL_MS
#define UCN_CONTROL_TOKEN_REFILL_MS UINT32_C(100)
#endif

#ifndef UCN_PENDING_Q1_DEPTH
#define UCN_PENDING_Q1_DEPTH ((size_t)4U)
#endif

#ifndef UCN_PENDING_Q1_TIMEOUT_MS
#define UCN_PENDING_Q1_TIMEOUT_MS UINT32_C(1000)
#endif

/* PATH_TRACE is an on-demand diagnostic.  Its records live in the frame
 * payload, so the maximum is bounded by both the wire MTU and hop limit. */
#define UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES ((size_t)8U)
#define UCN_PATH_TRACE_NODE_CAPACITY_BY_FRAME \
    ((UCN_MAX_FRAME_BYTES - UCN_FRAME_HEADER_SIZE - \
      UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES) / sizeof(ucn_node_id_t))
#define UCN_PATH_TRACE_MAX_NODES \
    ((size_t)(UCN_PATH_TRACE_NODE_CAPACITY_BY_FRAME < \
                      ((size_t)UCN_MAX_HOPS + 1U) ? \
                  UCN_PATH_TRACE_NODE_CAPACITY_BY_FRAME : \
                  ((size_t)UCN_MAX_HOPS + 1U)))

#ifndef UCN_PATH_TRACE_PENDING_DEPTH
#define UCN_PATH_TRACE_PENDING_DEPTH ((size_t)2U)
#endif

#ifndef UCN_PATH_TRACE_REVERSE_DEPTH
#define UCN_PATH_TRACE_REVERSE_DEPTH ((size_t)4U)
#endif

#ifndef UCN_PATH_TRACE_TIMEOUT_MS
#define UCN_PATH_TRACE_TIMEOUT_MS UINT32_C(1000)
#endif

#ifndef UCN_PATH_TRACE_TOKEN_BURST
#define UCN_PATH_TRACE_TOKEN_BURST ((uint8_t)1U)
#endif

#ifndef UCN_PATH_TRACE_TOKEN_REFILL_MS
#define UCN_PATH_TRACE_TOKEN_REFILL_MS UINT32_C(1000)
#endif

/* NODE_SNAPSHOT is a low-frequency diagnostic flood.  It records no permanent
 * topology: only the origin keeps a bounded result array, while relays keep
 * short-lived reverse entries for replies. */
#define UCN_NODE_SNAPSHOT_REQUEST_PAYLOAD_BYTES ((size_t)8U)
#define UCN_NODE_SNAPSHOT_REPLY_PAYLOAD_BYTES ((size_t)12U)

#ifndef UCN_NODE_SNAPSHOT_MAX_RESULTS
#define UCN_NODE_SNAPSHOT_MAX_RESULTS ((size_t)8U)
#endif

#ifndef UCN_NODE_SNAPSHOT_PENDING_DEPTH
#define UCN_NODE_SNAPSHOT_PENDING_DEPTH ((size_t)1U)
#endif

#ifndef UCN_NODE_SNAPSHOT_REVERSE_DEPTH
#define UCN_NODE_SNAPSHOT_REVERSE_DEPTH ((size_t)2U)
#endif

#ifndef UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH
#define UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH ((size_t)2U)
#endif

#ifndef UCN_NODE_SNAPSHOT_TIMEOUT_MS
#define UCN_NODE_SNAPSHOT_TIMEOUT_MS UINT32_C(2000)
#endif

#ifndef UCN_NODE_SNAPSHOT_TOKEN_BURST
#define UCN_NODE_SNAPSHOT_TOKEN_BURST ((uint8_t)1U)
#endif

#ifndef UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS
#define UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS UINT32_C(10000)
#endif

#ifndef UCN_NODE_SNAPSHOT_REPLY_JITTER_MS
#define UCN_NODE_SNAPSHOT_REPLY_JITTER_MS UINT32_C(200)
#endif

typedef char ucn_control_token_burst_must_be_positive[
    UCN_CONTROL_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_control_token_refill_must_be_positive[
    UCN_CONTROL_TOKEN_REFILL_MS > 0U ? 1 : -1];
typedef char ucn_pending_q1_timeout_must_be_positive[
    UCN_PENDING_Q1_TIMEOUT_MS > 0U ? 1 : -1];
typedef char ucn_path_trace_must_fit_two_node_ids[
    UCN_PATH_TRACE_MAX_NODES >= 2U ? 1 : -1];
typedef char ucn_path_trace_timeout_must_be_positive[
    UCN_PATH_TRACE_TIMEOUT_MS > 0U ? 1 : -1];
typedef char ucn_path_trace_token_burst_must_be_positive[
    UCN_PATH_TRACE_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_path_trace_token_refill_must_be_positive[
    UCN_PATH_TRACE_TOKEN_REFILL_MS > 0U ? 1 : -1];
typedef char ucn_node_snapshot_max_results_must_be_positive[
    UCN_NODE_SNAPSHOT_MAX_RESULTS > 0U ? 1 : -1];
typedef char ucn_node_snapshot_timeout_must_be_positive[
    UCN_NODE_SNAPSHOT_TIMEOUT_MS > 0U ? 1 : -1];
typedef char ucn_node_snapshot_token_burst_must_be_positive[
    UCN_NODE_SNAPSHOT_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_node_snapshot_token_refill_must_be_positive[
    UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS > 0U ? 1 : -1];
typedef char ucn_node_snapshot_link_count_must_fit_u8[
    UCN_MAX_LINKS <= UINT8_MAX ? 1 : -1];

#ifndef UCN_MAX_NEIGHBORS
#define UCN_MAX_NEIGHBORS ((size_t)8U)
#endif

#ifndef UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS
#define UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS UINT32_C(5000)
#endif

#ifndef UCN_HEARTBEAT_INTERVAL_MS
#define UCN_HEARTBEAT_INTERVAL_MS UINT32_C(1000)
#endif

#ifndef UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS
#define UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS UINT32_C(3000)
#endif

#ifndef UCN_NEIGHBOR_REMOVE_TIMEOUT_MS
#define UCN_NEIGHBOR_REMOVE_TIMEOUT_MS UINT32_C(4000)
#endif

typedef char ucn_neighbor_remove_must_follow_suspect[
    UCN_NEIGHBOR_REMOVE_TIMEOUT_MS > UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS ? 1 : -1];

#ifndef UCN_MAX_ENDPOINT_HANDLERS
#define UCN_MAX_ENDPOINT_HANDLERS ((size_t)8U)
#endif

#ifndef UCN_MAX_ENDPOINT_SECURITY_POLICIES
#define UCN_MAX_ENDPOINT_SECURITY_POLICIES ((size_t)8U)
#endif

typedef struct ucn_node ucn_node_t;

typedef void (*ucn_rx_handler_t)(void *context, const ucn_frame_t *frame);
typedef void (*ucn_endpoint_rx_handler_t)(void *context, const ucn_frame_t *frame);

typedef struct ucn_endpoint_handler_entry {
    bool occupied;
    ucn_endpoint_t endpoint;
    ucn_endpoint_rx_handler_t handler;
    void *context;
} ucn_endpoint_handler_entry_t;

typedef struct ucn_endpoint_security_policy_entry {
    bool occupied;
    ucn_endpoint_t endpoint;
    ucn_security_policy_t policy;
} ucn_endpoint_security_policy_entry_t;

typedef struct ucn_send_request {
    ucn_node_id_t destination;
    uint8_t message_type;
    ucn_traffic_class_t traffic_class;
    ucn_delivery_semantic_t delivery;
    uint32_t deadline_ms;
    const uint8_t *payload;
    uint16_t payload_length;
} ucn_send_request_t;

typedef struct ucn_tx_item {
    bool occupied;
    ucn_node_id_t destination;
    uint8_t message_type;
    ucn_traffic_class_t traffic_class;
    ucn_delivery_semantic_t delivery;
    uint32_t deadline_ms;
    uint32_t order;
    uint16_t payload_length;
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
} ucn_tx_item_t;

typedef struct ucn_pending_q1_item {
    bool occupied;
    ucn_node_id_t destination;
    uint8_t message_type;
    uint32_t deadline_ms;
    uint16_t payload_length;
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
} ucn_pending_q1_item_t;

typedef enum ucn_path_trace_status {
    UCN_PATH_TRACE_STATUS_OK = 0,
    UCN_PATH_TRACE_STATUS_NO_ROUTE = 1,
    UCN_PATH_TRACE_STATUS_TTL_EXCEEDED = 2,
    UCN_PATH_TRACE_STATUS_TRUNCATED = 3,
    UCN_PATH_TRACE_STATUS_TIMEOUT = 4
} ucn_path_trace_status_t;

typedef struct ucn_path_trace_result {
    ucn_path_trace_status_t status;
    uint32_t trace_id;
    uint8_t node_count;
    ucn_node_id_t node_ids[UCN_PATH_TRACE_MAX_NODES];
} ucn_path_trace_result_t;

typedef void (*ucn_path_trace_handler_t)(void *context,
                                         const ucn_path_trace_result_t *result);

typedef struct ucn_path_trace_pending {
    bool occupied;
    ucn_node_id_t destination;
    uint32_t trace_id;
    uint32_t deadline_ms;
    ucn_path_trace_handler_t handler;
    void *context;
} ucn_path_trace_pending_t;

typedef struct ucn_path_trace_reverse {
    bool occupied;
    ucn_node_id_t origin;
    uint32_t trace_id;
    ucn_link_t *ingress_link;
    uint32_t expires_at_ms;
} ucn_path_trace_reverse_t;

typedef enum ucn_node_snapshot_status {
    UCN_NODE_SNAPSHOT_STATUS_COMPLETE = 0,
    UCN_NODE_SNAPSHOT_STATUS_TRUNCATED = 1
} ucn_node_snapshot_status_t;

typedef struct ucn_node_snapshot_entry {
    ucn_node_id_t node_id;
    uint8_t direct_link_count;
    uint8_t flags;
} ucn_node_snapshot_entry_t;

typedef struct ucn_node_snapshot_result {
    ucn_node_snapshot_status_t status;
    uint32_t query_id;
    uint8_t node_count;
    ucn_node_snapshot_entry_t entries[UCN_NODE_SNAPSHOT_MAX_RESULTS];
} ucn_node_snapshot_result_t;

typedef void (*ucn_node_snapshot_handler_t)(
    void *context, const ucn_node_snapshot_result_t *result);

/* A NULL authorizer disables remote NODE_SNAPSHOT_REQ handling.  Each product
 * can admit only its designated diagnostic/management Node IDs. */
typedef bool (*ucn_node_snapshot_authorize_fn)(void *context,
                                               ucn_node_id_t requester);

typedef struct ucn_node_snapshot_pending {
    bool occupied;
    bool truncated;
    uint8_t result_limit;
    uint32_t query_id;
    uint32_t deadline_ms;
    uint8_t node_count;
    ucn_node_snapshot_entry_t entries[UCN_NODE_SNAPSHOT_MAX_RESULTS];
    ucn_node_snapshot_handler_t handler;
    void *context;
} ucn_node_snapshot_pending_t;

typedef struct ucn_node_snapshot_reverse {
    bool occupied;
    ucn_node_id_t origin;
    uint32_t query_id;
    ucn_link_t *ingress_link;
    uint32_t expires_at_ms;
} ucn_node_snapshot_reverse_t;

typedef struct ucn_node_snapshot_reply_pending {
    bool occupied;
    ucn_node_id_t origin;
    uint32_t query_id;
    ucn_link_t *egress_link;
    uint32_t due_at_ms;
    uint32_t expires_at_ms;
} ucn_node_snapshot_reply_pending_t;

typedef struct ucn_node_stats {
    uint32_t tx_sent;
    uint32_t tx_expired_dropped;
    uint32_t tx_error_dropped;
    uint32_t rx_delivered;
    uint32_t route_requests_sent;
    uint32_t route_replies_sent;
    uint32_t route_errors_sent;
    uint32_t heartbeat_requests_sent;
    uint32_t heartbeat_acks_sent;
    uint32_t heartbeat_received;
    uint32_t neighbor_suspected;
    uint32_t neighbor_removed;
    uint32_t route_refreshes_started;
    uint32_t candidate_routes_learned;
    uint32_t path_probes_sent;
    uint32_t path_probe_acks_received;
    uint32_t route_switches;
    uint32_t candidate_rejected;
    uint32_t route_epoch_rejected;
    uint32_t e2e_protected_forwarded;
    uint32_t control_budget_dropped;
    uint32_t q1_route_wait_queued;
    uint32_t q1_route_wait_expired;
    uint32_t path_trace_requests_sent;
    uint32_t path_trace_replies_sent;
    uint32_t path_trace_completed;
    uint32_t path_trace_timeouts;
    uint32_t path_trace_rejected;
    uint32_t path_trace_rate_dropped;
    uint32_t node_snapshot_requests_sent;
    uint32_t node_snapshot_requests_received;
    uint32_t node_snapshot_replies_sent;
    uint32_t node_snapshot_replies_received;
    uint32_t node_snapshot_completed;
    uint32_t node_snapshot_rejected;
    uint32_t node_snapshot_rate_dropped;
    uint32_t node_snapshot_result_truncated;
} ucn_node_stats_t;

typedef struct ucn_route_entry {
    bool valid;
    bool is_static;
    ucn_node_id_t destination;
    ucn_link_t *egress_link;
    uint32_t expires_at_ms;
    uint32_t last_used_at_ms;
    uint32_t last_refresh_started_ms;
    uint16_t route_cost;
    uint8_t hop_count;
    uint16_t route_epoch;
    bool previous_valid;
    ucn_link_t *previous_egress_link;
    uint16_t previous_route_epoch;
    uint32_t previous_expires_at_ms;
} ucn_route_entry_t;

typedef struct ucn_route_discovery {
    bool active;
    ucn_node_id_t destination;
    uint32_t request_id;
    uint32_t started_at_ms;
    uint32_t deadline_ms;
    bool is_candidate;
} ucn_route_discovery_t;

typedef struct ucn_candidate_route {
    bool valid;
    bool originated_here;
    bool activation_sent;
    ucn_node_id_t destination;
    uint32_t candidate_id;
    ucn_link_t *egress_link;
    uint32_t expires_at_ms;
    uint32_t next_probe_at_ms;
    uint16_t route_cost;
    uint8_t hop_count;
    uint8_t probes_sent;
    uint8_t probes_acked;
    uint16_t route_epoch;
} ucn_candidate_route_t;

typedef struct ucn_seen_frame {
    bool valid;
    ucn_node_id_t source;
    ucn_sequence_t sequence;
    uint16_t best_route_request_cost;
} ucn_seen_frame_t;

struct ucn_node {
    ucn_config_t config;
    ucn_link_t *links[UCN_MAX_LINKS];
    size_t link_count;
    ucn_sequence_t next_sequence;
    uint32_t next_queue_order;
    ucn_tx_item_t q0[UCN_TX_Q0_DEPTH];
    ucn_tx_item_t q1[UCN_TX_Q1_DEPTH];
    ucn_pending_q1_item_t pending_q1[UCN_PENDING_Q1_DEPTH];
    ucn_path_trace_pending_t path_trace_pending[UCN_PATH_TRACE_PENDING_DEPTH];
    ucn_path_trace_reverse_t path_trace_reverse[UCN_PATH_TRACE_REVERSE_DEPTH];
    ucn_node_snapshot_pending_t
        node_snapshot_pending[UCN_NODE_SNAPSHOT_PENDING_DEPTH];
    ucn_node_snapshot_reverse_t
        node_snapshot_reverse[UCN_NODE_SNAPSHOT_REVERSE_DEPTH];
    ucn_node_snapshot_reply_pending_t
        node_snapshot_replies[UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH];
    ucn_route_entry_t routes[UCN_MAX_ROUTES];
    ucn_candidate_route_t candidates[UCN_MAX_CANDIDATE_ROUTES];
    ucn_route_discovery_t discoveries[UCN_MAX_ROUTE_DISCOVERIES];
    ucn_neighbor_entry_t neighbors[UCN_MAX_NEIGHBORS];
    ucn_seen_frame_t seen[UCN_SEEN_CACHE_SIZE];
    size_t next_seen_index;
    uint32_t now_ms;
    uint32_t next_route_request_id;
    uint16_t next_route_epoch;
    uint32_t next_heartbeat_id;
    uint32_t next_path_trace_id;
    uint32_t next_node_snapshot_id;
    uint8_t control_tokens;
    uint32_t control_last_refill_ms;
    uint8_t path_trace_tokens;
    uint32_t path_trace_last_refill_ms;
    uint8_t node_snapshot_tokens;
    uint32_t node_snapshot_last_refill_ms;
    const ucn_security_ops_t *security_ops;
    void *security_context;
    ucn_session_id_t session_id;
    ucn_join_policy_t join_policy;
    ucn_neighbor_authorize_fn neighbor_authorize;
    void *neighbor_authorize_context;
    ucn_node_snapshot_authorize_fn node_snapshot_authorize;
    void *node_snapshot_authorize_context;
    ucn_endpoint_handler_entry_t endpoint_handlers[UCN_MAX_ENDPOINT_HANDLERS];
    ucn_security_policy_t security_policy;
    ucn_endpoint_security_policy_entry_t
        endpoint_security_policies[UCN_MAX_ENDPOINT_SECURITY_POLICIES];
    ucn_node_stats_t stats;
    ucn_rx_handler_t rx_handler;
    void *rx_context;
};

ucn_result_t ucn_node_init(ucn_node_t *node, const ucn_config_t *config);
ucn_result_t ucn_node_set_security(ucn_node_t *node,
                                   const ucn_security_ops_t *ops,
                                   void *context);
ucn_result_t ucn_node_set_security_policy(ucn_node_t *node,
                                          const ucn_security_policy_t *policy);
ucn_result_t ucn_node_set_endpoint_security_policy(
    ucn_node_t *node,
    ucn_endpoint_t endpoint,
    const ucn_security_policy_t *policy);
ucn_result_t ucn_node_set_join_policy(ucn_node_t *node,
                                      ucn_join_policy_t policy,
                                      ucn_neighbor_authorize_fn authorize,
                                      void *context);
ucn_result_t ucn_node_set_node_snapshot_authorizer(
    ucn_node_t *node,
    ucn_node_snapshot_authorize_fn authorize,
    void *context);
ucn_result_t ucn_node_observe_neighbor(ucn_node_t *node,
                                       ucn_link_t *link,
                                       uint32_t now_ms);
ucn_result_t ucn_node_probe_neighbor(ucn_node_t *node,
                                     ucn_link_t *link,
                                     uint32_t now_ms);
ucn_result_t ucn_node_broadcast_hello(ucn_node_t *node,
                                      ucn_link_t *link,
                                      uint32_t now_ms);
ucn_result_t ucn_node_admit_neighbor(ucn_node_t *node,
                                     ucn_node_id_t peer_node_id);
ucn_result_t ucn_node_reject_neighbor(ucn_node_t *node,
                                      ucn_node_id_t peer_node_id);
size_t ucn_node_neighbor_count(const ucn_node_t *node,
                               ucn_neighbor_state_t state);
ucn_result_t ucn_node_register_link(ucn_node_t *node, ucn_link_t *link);
ucn_result_t ucn_node_add_route(ucn_node_t *node,
                                ucn_node_id_t destination,
                                ucn_link_t *egress_link);
ucn_result_t ucn_node_discover_route(ucn_node_t *node,
                                     ucn_node_id_t destination,
                                     uint32_t now_ms);
ucn_result_t ucn_node_refresh_route(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    uint32_t now_ms);
bool ucn_node_route_pending(const ucn_node_t *node,
                            ucn_node_id_t destination);
void ucn_node_set_rx_handler(ucn_node_t *node,
                             ucn_rx_handler_t handler,
                             void *context);
ucn_result_t ucn_node_set_endpoint_handler(ucn_node_t *node,
                                            ucn_endpoint_t endpoint,
                                            ucn_endpoint_rx_handler_t handler,
                                            void *context);
ucn_result_t ucn_node_send(ucn_node_t *node,
                           ucn_node_id_t destination,
                           uint8_t message_type,
                           ucn_traffic_class_t traffic_class,
                           const uint8_t *payload,
                           uint16_t payload_length);
ucn_result_t ucn_node_send_endpoint(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    ucn_endpoint_t endpoint,
                                    ucn_traffic_class_t traffic_class,
                                    const uint8_t *payload,
                                    uint16_t payload_length);
ucn_result_t ucn_node_enqueue(ucn_node_t *node,
                              const ucn_send_request_t *request);
ucn_result_t ucn_node_request_path_trace(ucn_node_t *node,
                                         ucn_node_id_t destination,
                                         uint8_t record_limit,
                                         ucn_path_trace_handler_t handler,
                                         void *context);
ucn_result_t ucn_node_request_node_snapshot(
    ucn_node_t *node,
    uint8_t result_limit,
    ucn_node_snapshot_handler_t handler,
    void *context);
ucn_result_t ucn_node_step(ucn_node_t *node, uint32_t now_ms);
const ucn_node_stats_t *ucn_node_get_stats(const ucn_node_t *node);
ucn_result_t ucn_node_receive(ucn_node_t *node,
                              ucn_link_t *ingress_link,
                              const uint8_t *data,
                              size_t length);

#ifdef __cplusplus
}
#endif

#endif
