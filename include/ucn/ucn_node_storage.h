#ifndef UCN_NODE_STORAGE_H
#define UCN_NODE_STORAGE_H

#include "ucn/ucn_node.h"

/*
 * Static storage view for the single Protocol Task that owns a Node.
 *
 * Application tasks, Adapter declarations and pointer-only users should
 * include ucn_node.h instead.  Products that allocate ucn_node_t statically
 * include this header in exactly the owner translation unit.  The fields
 * below are implementation storage, not an application ABI: never read or
 * mutate them outside the Core/Port owner and always compile every translation
 * unit with the same UCN_PROFILE and UCN_FEATURE_SERVICE definitions.
 */
#define UCN_NODE_STORAGE_LAYOUT_VERSION UINT32_C(5)

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

typedef enum ucn_control_rx_budget_type {
    UCN_CONTROL_RX_ROUTE_REQUEST = 0,
    UCN_CONTROL_RX_HEARTBEAT_REQUEST = 1,
    UCN_CONTROL_RX_PATH_TRACE_REQUEST = 2,
    UCN_CONTROL_RX_BUDGET_TYPE_COUNT = 3
} ucn_control_rx_budget_type_t;

typedef struct ucn_control_rx_budget {
    uint8_t tokens;
    uint32_t last_refill_ms;
} ucn_control_rx_budget_t;

typedef struct ucn_control_rx_peer_budget {
    bool occupied;
    ucn_node_id_t peer_node_id;
    ucn_control_rx_budget_t budgets[UCN_CONTROL_RX_BUDGET_TYPE_COUNT];
} ucn_control_rx_peer_budget_t;

typedef struct ucn_tx_item {
    bool occupied;
    ucn_node_id_t destination;
    uint8_t message_type;
    ucn_traffic_class_t traffic_class;
    ucn_delivery_semantic_t delivery;
    uint32_t deadline_ms;
    uint32_t next_attempt_ms;
    uint32_t order;
    uint8_t backpressure_retries;
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

typedef struct ucn_path_control_source_budget {
    bool occupied;
    uint8_t tokens[UCN_PATH_CONTROL_OPERATION_COUNT];
    ucn_node_id_t source;
    ucn_session_id_t session_id;
    uint32_t last_refill_ms[UCN_PATH_CONTROL_OPERATION_COUNT];
    uint32_t last_seen_ms;
} ucn_path_control_source_budget_t;

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
    ucn_wire_profile_t wire_profile;
    ucn_node_id_t origin;
    uint32_t query_id;
    ucn_link_t *egress_link;
    uint32_t due_at_ms;
    uint32_t expires_at_ms;
} ucn_node_snapshot_reply_pending_t;

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

typedef struct ucn_route_entry {
    bool valid;
    bool is_static;
    ucn_node_id_t destination;
    ucn_link_t *egress_link;
#if UCN_FEATURE_DYNAMIC_MESH
    uint32_t expires_at_ms;
    uint32_t last_used_at_ms;
    uint32_t last_refresh_started_ms;
    ucn_route_cost_t route_cost;
    uint8_t hop_count;
    bool verified_rtt_valid;
    uint16_t verified_rtt_ms;
    uint16_t route_epoch;
    bool previous_valid;
    ucn_link_t *previous_egress_link;
    uint16_t previous_route_epoch;
    uint32_t previous_expires_at_ms;
#endif
} ucn_route_entry_t;

typedef struct ucn_route_discovery {
    bool active;
    ucn_node_id_t destination;
    uint32_t request_id;
    uint32_t overall_started_at_ms;
    uint32_t started_at_ms;
    uint32_t deadline_ms;
    uint8_t current_hop_limit;
    uint8_t maximum_hop_limit;
#if UCN_FEATURE_CANDIDATE_ROUTING
    bool is_candidate;
    bool require_verified_rtt;
#endif
} ucn_route_discovery_t;

typedef struct ucn_candidate_route {
    bool valid;
    bool originated_here;
    bool activation_sent;
    ucn_wire_profile_t wire_profile;
    ucn_node_id_t destination;
    uint32_t candidate_id;
    ucn_link_t *egress_link;
    uint32_t expires_at_ms;
    uint32_t next_probe_at_ms;
    ucn_route_cost_t route_cost;
    uint8_t hop_count;
    uint8_t probes_sent;
    uint8_t probes_acked;
    bool verified_rtt_valid;
    uint16_t verified_rtt_ms;
    uint16_t route_epoch;
} ucn_candidate_route_t;

/* Volatile network duplicate suppression only.  Each fixed Source/Session
 * window accepts bounded reordering without retaining every frame.  It is
 * neither durable anti-replay nor an authentication decision. */
#if UCN_DUPLICATE_WINDOW_BITS == 64U
typedef uint64_t ucn_duplicate_bitmap_t;
#else
typedef uint32_t ucn_duplicate_bitmap_t;
#endif

typedef struct ucn_duplicate_source_window {
    bool valid;
    ucn_node_id_t source;
    ucn_session_id_t session_id;
    ucn_sequence_t highest_sequence;
    ucn_duplicate_bitmap_t received_bitmap;
    uint32_t last_observed_ms;
} ucn_duplicate_source_window_t;

#if UCN_FEATURE_DYNAMIC_MESH
typedef struct ucn_rreq_cache_entry {
    bool valid;
    ucn_node_id_t origin;
    ucn_session_id_t session_id;
    uint32_t request_id;
    ucn_route_cost_t best_route_request_cost;
    uint32_t last_observed_ms;
} ucn_rreq_cache_entry_t;
#endif

struct ucn_node {
    ucn_config_t config;
    ucn_wire_profile_t tx_wire_profile;
    ucn_wire_profile_t max_receive_wire_profile;
    bool automatic_wire_profile;
    ucn_link_t *links[UCN_MAX_LINKS];
    size_t link_count;
    ucn_sequence_t next_sequence;
    uint32_t next_queue_order;
    ucn_tx_item_t q0[UCN_TX_Q0_DEPTH];
    ucn_tx_item_t q1[UCN_TX_Q1_DEPTH];
#if UCN_FEATURE_DYNAMIC_MESH
    ucn_pending_q1_item_t pending_q1[UCN_PENDING_Q1_DEPTH];
    ucn_route_constraints_t default_route_constraints;
#endif
#if UCN_FEATURE_DIAGNOSTICS
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
#endif
    ucn_route_entry_t routes[UCN_MAX_ROUTES];
#if UCN_FEATURE_CANDIDATE_ROUTING
    ucn_candidate_route_t candidates[UCN_MAX_CANDIDATE_ROUTES];
#endif
#if UCN_FEATURE_DYNAMIC_MESH
    ucn_route_discovery_t discoveries[UCN_MAX_ROUTE_DISCOVERIES];
    ucn_neighbor_entry_t neighbors[UCN_MAX_NEIGHBORS];
#endif
#if UCN_FEATURE_POLICY
    ucn_policy_state_t policy_state;
#endif
#if UCN_FEATURE_PATH
    ucn_path_state_t path_state;
#endif
    ucn_duplicate_source_window_t
        duplicate_windows[UCN_DUPLICATE_SOURCE_WINDOWS];
#if UCN_FEATURE_DYNAMIC_MESH
    ucn_rreq_cache_entry_t rreq_cache[UCN_RREQ_CACHE_SIZE];
#endif
    uint32_t now_ms;
#if UCN_FEATURE_DYNAMIC_MESH
    uint32_t next_route_request_id;
    uint16_t next_route_epoch;
    uint32_t next_heartbeat_id;
#endif
#if UCN_FEATURE_DIAGNOSTICS
    uint32_t next_path_trace_id;
    uint32_t next_node_snapshot_id;
    uint32_t next_policy_diagnostic_id;
#endif
#if UCN_FEATURE_DYNAMIC_MESH
    uint8_t business_tx_since_maintenance;
#endif
    bool step_observation_started;
#if UCN_FEATURE_DYNAMIC_MESH
    uint8_t control_tokens;
    uint32_t control_last_refill_ms;
    ucn_control_rx_peer_budget_t control_rx_peer_budgets[UCN_MAX_NEIGHBORS];
#endif
#if UCN_FEATURE_PATH
    ucn_path_control_source_budget_t
        path_control_source_budgets[UCN_PATH_CONTROL_RX_SOURCE_DEPTH];
#endif
#if UCN_FEATURE_DIAGNOSTICS
    uint8_t path_trace_tokens;
    uint32_t path_trace_last_refill_ms;
    uint8_t node_snapshot_tokens;
    uint32_t node_snapshot_last_refill_ms;
    uint8_t policy_diagnostic_tokens;
    uint32_t policy_diagnostic_last_refill_ms;
#endif
#if UCN_FEATURE_SECURITY
    const ucn_security_ops_t *security_ops;
    void *security_context;
    bool security_required;
#endif
    ucn_session_id_t session_id;
#if UCN_FEATURE_DYNAMIC_MESH
    ucn_join_policy_t join_policy;
    ucn_neighbor_authorize_fn neighbor_authorize;
    void *neighbor_authorize_context;
#endif
#if UCN_FEATURE_DIAGNOSTICS
    ucn_node_snapshot_authorize_fn node_snapshot_authorize;
    void *node_snapshot_authorize_context;
    ucn_path_trace_authorize_fn path_trace_authorize;
    void *path_trace_authorize_context;
    ucn_policy_diagnostic_authorize_fn policy_diagnostic_authorize;
    void *policy_diagnostic_authorize_context;
#endif
#if UCN_FEATURE_PATH
    ucn_path_control_authorize_fn path_control_authorize;
    void *path_control_authorize_context;
#endif
    ucn_endpoint_handler_entry_t endpoint_handlers[UCN_MAX_ENDPOINT_HANDLERS];
#if UCN_FEATURE_SECURITY
    ucn_security_policy_t security_policy;
    ucn_endpoint_security_policy_entry_t
        endpoint_security_policies[UCN_MAX_ENDPOINT_SECURITY_POLICIES];
#endif
    ucn_node_stats_t stats;
    ucn_rx_handler_t rx_handler;
    void *rx_context;
};

#endif
