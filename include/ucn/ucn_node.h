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

#include "ucn/ucn_policy.h"
#include "ucn/ucn_path.h"

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

/* POLICY_DIAGNOSTIC is an explicitly authorized, unicast view of one Node's
 * existing fixed Policy/Path/Flow/quality state.  It never adds fields to
 * normal business frames and its 32 B reply exactly fits the 64 B profile. */
#define UCN_POLICY_DIAGNOSTIC_REQUEST_PAYLOAD_BYTES ((size_t)8U)
#define UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES ((size_t)32U)
#define UCN_POLICY_DIAGNOSTIC_RECORD_BYTES ((size_t)24U)

#ifndef UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH
#define UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH ((size_t)2U)
#endif

#ifndef UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH
#define UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH ((size_t)2U)
#endif

#ifndef UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS
#define UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS UINT32_C(1000)
#endif

#ifndef UCN_POLICY_DIAGNOSTIC_TOKEN_BURST
#define UCN_POLICY_DIAGNOSTIC_TOKEN_BURST ((uint8_t)1U)
#endif

#ifndef UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS
#define UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS UINT32_C(1000)
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
typedef char ucn_policy_diagnostic_reply_must_fit_frame[
    UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES <= UCN_MAX_PAYLOAD_BYTES ? 1 : -1];
typedef char ucn_policy_diagnostic_pending_must_be_positive[
    UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH > 0U ? 1 : -1];
typedef char ucn_policy_diagnostic_reply_queue_must_be_positive[
    UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH > 0U ? 1 : -1];
typedef char ucn_policy_diagnostic_timeout_must_be_positive[
    UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS > 0U ? 1 : -1];
typedef char ucn_policy_diagnostic_token_burst_must_be_positive[
    UCN_POLICY_DIAGNOSTIC_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_policy_diagnostic_token_refill_must_be_positive[
    UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS > 0U ? 1 : -1];

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

#ifndef UCN_BEARER_SWITCH_IMPROVEMENT_PERCENT
#define UCN_BEARER_SWITCH_IMPROVEMENT_PERCENT \
    UCN_ROUTE_SWITCH_IMPROVEMENT_PERCENT
#endif

#ifndef UCN_BEARER_QUALITY_SAMPLE_INTERVAL_MS
#define UCN_BEARER_QUALITY_SAMPLE_INTERVAL_MS UINT32_C(500)
#endif

#ifndef UCN_BEARER_QUALITY_STABLE_SAMPLES
#define UCN_BEARER_QUALITY_STABLE_SAMPLES ((uint8_t)3U)
#endif

#ifndef UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS
#define UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS ((uint8_t)2U)
#endif

#ifndef UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS
#define UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS ((uint8_t)3U)
#endif

#ifndef UCN_BEARER_QUALITY_PROBE_INTERVAL_MS
#define UCN_BEARER_QUALITY_PROBE_INTERVAL_MS UINT32_C(100)
#endif

typedef char ucn_neighbor_remove_must_follow_suspect[
    UCN_NEIGHBOR_REMOVE_TIMEOUT_MS > UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS ? 1 : -1];
typedef char ucn_bearer_switch_improvement_percent_must_be_less_than_100[
    UCN_BEARER_SWITCH_IMPROVEMENT_PERCENT < 100U ? 1 : -1];
typedef char ucn_bearer_quality_sample_interval_must_be_positive[
    UCN_BEARER_QUALITY_SAMPLE_INTERVAL_MS > 0U ? 1 : -1];
typedef char ucn_bearer_quality_stable_samples_must_be_positive[
    UCN_BEARER_QUALITY_STABLE_SAMPLES > 0U ? 1 : -1];
typedef char ucn_bearer_quality_probe_required_acks_must_be_positive[
    UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS > 0U ? 1 : -1];
typedef char ucn_bearer_quality_probe_attempts_must_cover_acks[
    UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS >=
    UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS ? 1 : -1];
typedef char ucn_bearer_quality_probe_interval_must_be_positive[
    UCN_BEARER_QUALITY_PROBE_INTERVAL_MS > 0U ? 1 : -1];

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

/* A NULL authorizer disables remote POLICY_DIAGNOSTIC_REQ handling.  This
 * report may reveal local strategy/quality state, so it is never public by
 * default; products admit only designated management Node IDs. */
typedef bool (*ucn_policy_diagnostic_authorize_fn)(void *context,
                                                    ucn_node_id_t requester);

/* Remote Path table changes are denied unless both the normal security
 * Provider and this explicit product authorization hook accept them. */
typedef enum ucn_path_control_operation {
    UCN_PATH_CONTROL_INSTALL = 0,
    UCN_PATH_CONTROL_REVOKE = 1
} ucn_path_control_operation_t;

typedef ucn_result_t (*ucn_path_control_authorize_fn)(
    void *context,
    const ucn_link_t *ingress_link,
    const ucn_frame_t *frame,
    ucn_path_control_operation_t operation,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop);

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

typedef enum ucn_policy_diagnostic_section {
    /* Index selects one of three fixed pages, each containing six counters. */
    UCN_POLICY_DIAGNOSTIC_SUMMARY = 0,
    /* Index selects a slot in the corresponding fixed local table. */
    UCN_POLICY_DIAGNOSTIC_POLICY = 1,
    UCN_POLICY_DIAGNOSTIC_PATH = 2,
    UCN_POLICY_DIAGNOSTIC_FLOW = 3,
    UCN_POLICY_DIAGNOSTIC_LINK_QUALITY = 4
} ucn_policy_diagnostic_section_t;

typedef enum ucn_policy_diagnostic_status {
    UCN_POLICY_DIAGNOSTIC_STATUS_OK = 0,
    UCN_POLICY_DIAGNOSTIC_STATUS_EMPTY = 1,
    UCN_POLICY_DIAGNOSTIC_STATUS_TIMEOUT = 2
} ucn_policy_diagnostic_status_t;

typedef struct ucn_policy_diagnostic_summary_record {
    uint32_t counters[6];
} ucn_policy_diagnostic_summary_record_t;

typedef struct ucn_policy_diagnostic_policy_record {
    ucn_route_policy_key_t key;
    ucn_route_policy_mode_t mode;
    bool allow_discovery_on_hard_failure;
    uint16_t primary_local_path_id;
    uint16_t backup_local_path_id;
    uint32_t balance_flow_lease_ms;
    uint32_t match_hits;
} ucn_policy_diagnostic_policy_record_t;

typedef struct ucn_policy_diagnostic_path_record {
    uint16_t local_path_id;
    ucn_path_id_t wire_path_id;
    ucn_node_id_t destination;
    ucn_policy_path_state_t state;
    uint8_t congestion_samples;
    uint8_t configured_egress_link_id;
    uint8_t active_bearer_link_id;
    bool active_bearer_is_up;
    bool route_cost_valid;
    uint16_t route_cost;
    bool rtt_valid;
    uint16_t rtt_ewma_ms;
    bool tx_failure_rate_valid;
    uint16_t tx_failure_ewma_per_mille;
    bool queue_pressure_valid;
    uint16_t queue_pressure_ewma_per_mille;
} ucn_policy_diagnostic_path_record_t;

typedef struct ucn_policy_diagnostic_flow_record {
    ucn_policy_flow_key_t key;
    uint16_t local_path_id;
    uint32_t expires_at_ms;
    uint32_t last_used_at_ms;
    uint32_t remaining_ms;
    ucn_policy_path_state_t path_state;
    uint8_t active_bearer_link_id;
} ucn_policy_diagnostic_flow_record_t;

typedef struct ucn_policy_diagnostic_link_quality_record {
    uint8_t link_id;
    bool is_up;
    bool route_cost_valid;
    uint16_t route_cost;
    bool rtt_valid;
    uint16_t rtt_ewma_ms;
    bool tx_failure_rate_valid;
    uint16_t tx_failure_ewma_per_mille;
    bool queue_pressure_valid;
    uint16_t queue_pressure_ewma_per_mille;
    uint32_t sampled_at_ms;
} ucn_policy_diagnostic_link_quality_record_t;

typedef union ucn_policy_diagnostic_record {
    ucn_policy_diagnostic_summary_record_t summary;
    ucn_policy_diagnostic_policy_record_t policy;
    ucn_policy_diagnostic_path_record_t path;
    ucn_policy_diagnostic_flow_record_t flow;
    ucn_policy_diagnostic_link_quality_record_t link_quality;
} ucn_policy_diagnostic_record_t;

typedef struct ucn_policy_diagnostic_result {
    uint32_t request_id;
    ucn_node_id_t node_id;
    ucn_policy_diagnostic_section_t section;
    uint8_t index;
    ucn_policy_diagnostic_status_t status;
    ucn_policy_diagnostic_record_t record;
} ucn_policy_diagnostic_result_t;

typedef void (*ucn_policy_diagnostic_handler_t)(
    void *context, const ucn_policy_diagnostic_result_t *result);

typedef struct ucn_policy_diagnostic_pending {
    bool occupied;
    bool sent;
    ucn_node_id_t destination;
    uint32_t request_id;
    uint32_t deadline_ms;
    ucn_policy_diagnostic_section_t section;
    uint8_t index;
    ucn_policy_diagnostic_handler_t handler;
    void *context;
} ucn_policy_diagnostic_pending_t;

typedef struct ucn_policy_diagnostic_reply_pending {
    bool occupied;
    ucn_node_id_t destination;
    uint32_t expires_at_ms;
    uint8_t payload[UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES];
} ucn_policy_diagnostic_reply_pending_t;

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
    uint32_t bearer_quality_probes_sent;
    uint32_t bearer_quality_probe_acks_received;
    uint32_t bearer_quality_switches;
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
    uint32_t policy_diagnostic_requests_sent;
    uint32_t policy_diagnostic_requests_received;
    uint32_t policy_diagnostic_replies_sent;
    uint32_t policy_diagnostic_replies_received;
    uint32_t policy_diagnostic_completed;
    uint32_t policy_diagnostic_timeouts;
    uint32_t policy_diagnostic_rejected;
    uint32_t policy_diagnostic_rate_dropped;
    uint32_t path_installs_sent;
    uint32_t path_installs_received;
    uint32_t path_revokes_sent;
    uint32_t path_revokes_received;
    uint32_t path_forwards;
    uint32_t path_rejected;
    uint32_t path_route_errors_sent;
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
    ucn_policy_diagnostic_pending_t
        policy_diagnostic_pending[UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH];
    ucn_policy_diagnostic_reply_pending_t
        policy_diagnostic_replies[UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH];
    ucn_route_entry_t routes[UCN_MAX_ROUTES];
    ucn_candidate_route_t candidates[UCN_MAX_CANDIDATE_ROUTES];
    ucn_route_discovery_t discoveries[UCN_MAX_ROUTE_DISCOVERIES];
    ucn_neighbor_entry_t neighbors[UCN_MAX_NEIGHBORS];
    ucn_policy_state_t policy_state;
    ucn_path_state_t path_state;
    ucn_seen_frame_t seen[UCN_SEEN_CACHE_SIZE];
    size_t next_seen_index;
    uint32_t now_ms;
    uint32_t next_route_request_id;
    uint16_t next_route_epoch;
    uint32_t next_heartbeat_id;
    uint32_t next_path_trace_id;
    uint32_t next_node_snapshot_id;
    uint32_t next_policy_diagnostic_id;
    uint8_t control_tokens;
    uint32_t control_last_refill_ms;
    uint8_t path_trace_tokens;
    uint32_t path_trace_last_refill_ms;
    uint8_t node_snapshot_tokens;
    uint32_t node_snapshot_last_refill_ms;
    uint8_t policy_diagnostic_tokens;
    uint32_t policy_diagnostic_last_refill_ms;
    const ucn_security_ops_t *security_ops;
    void *security_context;
    ucn_session_id_t session_id;
    ucn_join_policy_t join_policy;
    ucn_neighbor_authorize_fn neighbor_authorize;
    void *neighbor_authorize_context;
    ucn_node_snapshot_authorize_fn node_snapshot_authorize;
    void *node_snapshot_authorize_context;
    ucn_policy_diagnostic_authorize_fn policy_diagnostic_authorize;
    void *policy_diagnostic_authorize_context;
    ucn_path_control_authorize_fn path_control_authorize;
    void *path_control_authorize_context;
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
ucn_result_t ucn_node_set_policy_diagnostic_authorizer(
    ucn_node_t *node,
    ucn_policy_diagnostic_authorize_fn authorize,
    void *context);
ucn_result_t ucn_node_set_path_control_authorizer(
    ucn_node_t *node,
    ucn_path_control_authorize_fn authorize,
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
/* T22.3 enforces PINNED_STRICT and PINNED_FAILOVER for the Endpoint send API.
 * Policies reference local handles; each selected Path must also carry a
 * verified T22.2 authenticated wire_path_id.  AUTO_BEST and AUTO_BALANCE
 * retain their prior behavior until their respective policy stages. */
ucn_result_t ucn_node_set_route_policy(ucn_node_t *node,
                                       const ucn_route_policy_config_t *config);
ucn_result_t ucn_node_clear_route_policy(ucn_node_t *node,
                                         const ucn_route_policy_key_t *key);
const ucn_route_policy_entry_t *ucn_node_find_route_policy(
    const ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class);
ucn_result_t ucn_node_set_policy_path(ucn_node_t *node,
                                      const ucn_policy_path_config_t *config);
ucn_result_t ucn_node_clear_policy_path(ucn_node_t *node,
                                        uint16_t local_path_id);
const ucn_policy_path_entry_t *ucn_node_find_policy_path(
    const ucn_node_t *node,
    uint16_t local_path_id);
ucn_result_t ucn_node_bind_q1_flow(ucn_node_t *node,
                                   ucn_node_id_t destination,
                                   ucn_endpoint_t endpoint,
                                   uint16_t local_path_id,
                                   uint32_t lease_ms);
const ucn_policy_flow_binding_t *ucn_node_find_q1_flow(
    const ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint);
const ucn_policy_link_quality_snapshot_t *ucn_node_get_link_quality(
    const ucn_node_t *node,
    const ucn_link_t *link);
const ucn_policy_stats_t *ucn_node_get_policy_stats(const ucn_node_t *node);
/* T22.2 provisioning API.  Local installation only creates this node's
 * source-side entry.  Every relay and the terminal must receive an authorized
 * PATH_INSTALL control frame before a business frame can traverse the Path. */
ucn_result_t ucn_node_install_local_path(ucn_node_t *node,
                                         ucn_path_id_t path_id,
                                         ucn_node_id_t destination,
                                         ucn_node_id_t next_hop,
                                         uint32_t lease_ms);
ucn_result_t ucn_node_revoke_local_path(ucn_node_t *node,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination);
ucn_result_t ucn_node_send_path_install(ucn_node_t *node,
                                        ucn_node_id_t control_target,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination,
                                        ucn_node_id_t next_hop,
                                        uint32_t lease_ms);
ucn_result_t ucn_node_send_path_revoke(ucn_node_t *node,
                                       ucn_node_id_t control_target,
                                       ucn_path_id_t path_id,
                                       ucn_node_id_t destination);
const ucn_path_forward_entry_t *ucn_node_find_path_forward(
    const ucn_node_t *node,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination);
/* Low-level provisioning/test send.  T22.3 also uses this primitive after a
 * PINNED policy has resolved its local handle to a verified wire Path ID. */
ucn_result_t ucn_node_send_path(ucn_node_t *node,
                                ucn_node_id_t destination,
                                uint8_t message_type,
                                ucn_traffic_class_t traffic_class,
                                ucn_path_id_t path_id,
                                const uint8_t *payload,
                                uint16_t payload_length);
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
/* T22.6 management query.  The request is queued at diagnostic priority and
 * only sent after normal Q0/Q1 work; a remote target must opt in through the
 * explicit authorizer above.  `section` and `index` select one bounded page
 * or one fixed table slot, never a complete permanent topology dump. */
ucn_result_t ucn_node_request_policy_diagnostic(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_policy_diagnostic_section_t section,
    uint8_t index,
    ucn_policy_diagnostic_handler_t handler,
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
