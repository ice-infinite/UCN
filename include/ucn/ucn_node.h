#ifndef UCN_NODE_H
#define UCN_NODE_H

#include "ucn/ucn_profile.h"
#include "ucn/ucn_link.h"
#include "ucn/ucn_endpoint.h"
#include "ucn/ucn_neighbor.h"
#include "ucn/ucn_security.h"
#include "ucn/ucn_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Product builds may force the fail-closed gate on from Node initialization,
 * so an omitted runtime call cannot silently leave the deployment in the
 * development-compatible plain mode. */
#ifndef UCN_SECURITY_REQUIRED_BY_DEFAULT
#define UCN_SECURITY_REQUIRED_BY_DEFAULT 0
#endif

#if UCN_SECURITY_REQUIRED_BY_DEFAULT != 0 && \
    UCN_SECURITY_REQUIRED_BY_DEFAULT != 1
#error "UCN_SECURITY_REQUIRED_BY_DEFAULT must be 0 or 1"
#endif
#if UCN_SECURITY_REQUIRED_BY_DEFAULT && !UCN_FEATURE_SECURITY
#error "A security-required product must use Lite or Full"
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

/* A due liveness/path maintenance control frame may use one scheduling slot
 * after this many business transmissions.  It is not a periodic reservation:
 * when no maintenance is due, Q0/Q1 continue without an injected frame.
 * Snapshot and policy diagnostics never use this exception. */
#ifndef UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE
#define UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE ((uint8_t)4U)
#endif

/* Product/Port scheduling contract.  The sole Protocol Task must call
 * ucn_node_step() at least this often even when there is no RX or business
 * traffic.  Runtime overruns are observed; an unsafe compile-time maintenance
 * bound is rejected below. */
#ifndef UCN_MAX_STEP_INTERVAL_MS
#define UCN_MAX_STEP_INTERVAL_MS UINT32_C(10)
#endif

/* Optional Q0 local-admission retry.  A request must explicitly select
 * UCN_DELIVERY_RETRY_ON_BACKPRESSURE and provide a non-zero absolute
 * deadline.  Only UCN_ERR_NO_SPACE is retried; every other error is final. */
#ifndef UCN_Q0_BACKPRESSURE_MAX_RETRIES
#define UCN_Q0_BACKPRESSURE_MAX_RETRIES ((uint8_t)3U)
#endif

#ifndef UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS
#define UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS UINT32_C(5)
#endif

#ifndef UCN_MAX_ROUTES
#define UCN_MAX_ROUTES ((size_t)8U)
#endif

#ifndef UCN_DUPLICATE_SOURCE_WINDOWS
#ifdef UCN_SEEN_CACHE_SIZE
#define UCN_DUPLICATE_SOURCE_WINDOWS UCN_SEEN_CACHE_SIZE
#elif UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_DUPLICATE_SOURCE_WINDOWS ((size_t)4U)
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_DUPLICATE_SOURCE_WINDOWS ((size_t)16U)
#else
#define UCN_DUPLICATE_SOURCE_WINDOWS ((size_t)32U)
#endif
#endif

#ifndef UCN_DUPLICATE_WINDOW_BITS
#if UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_DUPLICATE_WINDOW_BITS 64U
#else
#define UCN_DUPLICATE_WINDOW_BITS 32U
#endif
#endif

#ifndef UCN_DUPLICATE_SOURCE_TIMEOUT_MS
#define UCN_DUPLICATE_SOURCE_TIMEOUT_MS UINT32_C(60000)
#endif

/* Compatibility alias for products that sized the old Seen ring.  The new
 * value counts Source/Session windows, not individual frames. */
#ifndef UCN_SEEN_CACHE_SIZE
#define UCN_SEEN_CACHE_SIZE UCN_DUPLICATE_SOURCE_WINDOWS
#endif

#if UCN_FEATURE_DYNAMIC_MESH
#ifndef UCN_RREQ_CACHE_SIZE
#if UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_RREQ_CACHE_SIZE ((size_t)16U)
#else
#define UCN_RREQ_CACHE_SIZE ((size_t)8U)
#endif
#endif

#ifndef UCN_RREQ_CACHE_TIMEOUT_MS
#define UCN_RREQ_CACHE_TIMEOUT_MS UINT32_C(5000)
#endif
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

#if UCN_FEATURE_CANDIDATE_ROUTING
typedef char ucn_path_probe_required_acks_must_be_positive[
    UCN_PATH_PROBE_REQUIRED_ACKS > 0U ? 1 : -1];
#endif
typedef char ucn_business_tx_burst_before_maintenance_must_be_positive[
    UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE > 0U ? 1 : -1];
typedef char ucn_q0_backpressure_max_retries_must_be_positive[
    UCN_Q0_BACKPRESSURE_MAX_RETRIES > 0U ? 1 : -1];
typedef char ucn_q0_backpressure_retry_interval_must_be_valid[
    UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS > 0U &&
            UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS <= INT32_MAX ?
        1 : -1];
#if UCN_FEATURE_DYNAMIC_MESH
typedef char ucn_route_refresh_advance_must_fit_lifetime[
    UCN_ROUTE_REFRESH_ADVANCE_MS < UCN_ROUTE_ENTRY_LIFETIME_MS ? 1 : -1];
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
typedef char ucn_route_switch_improvement_percent_must_be_less_than_100[
    UCN_ROUTE_SWITCH_IMPROVEMENT_PERCENT < 100U ? 1 : -1];
#endif

#ifndef UCN_UNKNOWN_LINK_ROUTE_COST
#define UCN_UNKNOWN_LINK_ROUTE_COST UCN_LINK_ROUTE_COST_UNKNOWN
#endif

typedef char ucn_unknown_route_cost_must_use_reserved_sentinel[
    UCN_UNKNOWN_LINK_ROUTE_COST == UCN_LINK_ROUTE_COST_UNKNOWN ? 1 : -1];

#ifndef UCN_CONTROL_TOKEN_BURST
#define UCN_CONTROL_TOKEN_BURST ((uint8_t)4U)
#endif

#ifndef UCN_CONTROL_TOKEN_REFILL_MS
#define UCN_CONTROL_TOKEN_REFILL_MS UINT32_C(100)
#endif

/* Independent inbound budgets stop one admitted but faulty peer from turning
 * unique control IDs into unbounded relay/reply work.  They are deliberately
 * separate from the local-origin control budget above. */
#ifndef UCN_ROUTE_REQUEST_RX_TOKEN_BURST
#define UCN_ROUTE_REQUEST_RX_TOKEN_BURST ((uint8_t)5U)
#endif
#ifndef UCN_ROUTE_REQUEST_RX_TOKEN_REFILL_MS
#define UCN_ROUTE_REQUEST_RX_TOKEN_REFILL_MS UINT32_C(200)
#endif
#ifndef UCN_HEARTBEAT_RX_TOKEN_BURST
#define UCN_HEARTBEAT_RX_TOKEN_BURST ((uint8_t)4U)
#endif
#ifndef UCN_HEARTBEAT_RX_TOKEN_REFILL_MS
#define UCN_HEARTBEAT_RX_TOKEN_REFILL_MS UINT32_C(100)
#endif
#ifndef UCN_PATH_TRACE_RX_TOKEN_BURST
#define UCN_PATH_TRACE_RX_TOKEN_BURST ((uint8_t)1U)
#endif
#ifndef UCN_PATH_TRACE_RX_TOKEN_REFILL_MS
#define UCN_PATH_TRACE_RX_TOKEN_REFILL_MS UINT32_C(1000)
#endif

/* Authenticated PATH_INSTALL/PATH_REVOKE writes use a source/session budget
 * that is independent from the ingress Link.  This prevents one admitted
 * management source from bypassing the write-rate bound by changing Bearer. */
#ifndef UCN_PATH_CONTROL_RX_SOURCE_DEPTH
#define UCN_PATH_CONTROL_RX_SOURCE_DEPTH ((size_t)4U)
#endif
#ifndef UCN_PATH_CONTROL_RX_TOKEN_BURST
#define UCN_PATH_CONTROL_RX_TOKEN_BURST ((uint8_t)4U)
#endif
#ifndef UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS
#define UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS UINT32_C(1000)
#endif
#ifndef UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS
#define UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS UINT32_C(60000)
#endif

#ifndef UCN_PENDING_Q1_DEPTH
#define UCN_PENDING_Q1_DEPTH ((size_t)4U)
#endif

#ifndef UCN_PENDING_Q1_TIMEOUT_MS
#define UCN_PENDING_Q1_TIMEOUT_MS UINT32_C(1000)
#endif

#ifndef UCN_SEQUENCE_ROTATION_THRESHOLD
#define UCN_SEQUENCE_ROTATION_THRESHOLD (UINT32_MAX - UINT32_C(1024))
#endif

/* PATH_TRACE is an on-demand diagnostic.  Its records live in the frame
 * payload, so the maximum is bounded by both the wire MTU and hop limit. */
#define UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES ((size_t)8U)
#define UCN_PATH_TRACE_MIN_FRAME_BYTES \
    (UCN_FRAME_HEADER_SIZE + UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES + \
     2U * sizeof(ucn_node_id_t))
#define UCN_PATH_TRACE_NODE_CAPACITY_BY_FRAME \
    (UCN_MAX_PAYLOAD_BYTES >= UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES ? \
         ((UCN_MAX_PAYLOAD_BYTES - UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES) / \
          sizeof(ucn_node_id_t)) : \
         0U)
#if UCN_FEATURE_DIAGNOSTICS
#define UCN_PATH_TRACE_MAX_NODES \
    ((size_t)(UCN_PATH_TRACE_NODE_CAPACITY_BY_FRAME < \
                      ((size_t)UCN_MAX_HOPS + 1U) ? \
                  UCN_PATH_TRACE_NODE_CAPACITY_BY_FRAME : \
                  ((size_t)UCN_MAX_HOPS + 1U)))
#else
/* Keep disabled-profile API types valid without allocating diagnostic state
 * inside ucn_node_t.  Calls are provided by the explicit CONFIG stubs. */
#define UCN_PATH_TRACE_MAX_NODES ((size_t)1U)
#endif

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
#define UCN_DYNAMIC_MESH_MIN_FRAME_BYTES \
    (UCN_FRAME_HEADER_SIZE + (size_t)18U)

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

#if UCN_FEATURE_DYNAMIC_MESH
typedef char ucn_dynamic_mesh_control_must_fit_frame[
    UCN_MAX_FRAME_BYTES >= UCN_DYNAMIC_MESH_MIN_FRAME_BYTES &&
            UCN_MAX_PAYLOAD_BYTES >= (size_t)18U ? 1 : -1];
typedef char ucn_rreq_cache_must_not_be_empty[
    UCN_RREQ_CACHE_SIZE > 0U ? 1 : -1];
typedef char ucn_rreq_cache_timeout_must_be_valid[
    UCN_RREQ_CACHE_TIMEOUT_MS > 0U &&
            UCN_RREQ_CACHE_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS ? 1 : -1];
typedef char ucn_control_token_burst_must_be_positive[
    UCN_CONTROL_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_control_token_refill_must_be_positive[
    UCN_CONTROL_TOKEN_REFILL_MS > 0U ? 1 : -1];
typedef char ucn_route_request_rx_token_burst_must_be_positive[
    UCN_ROUTE_REQUEST_RX_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_route_request_rx_token_refill_must_be_positive[
    UCN_ROUTE_REQUEST_RX_TOKEN_REFILL_MS > 0U ? 1 : -1];
typedef char ucn_heartbeat_rx_token_burst_must_be_positive[
    UCN_HEARTBEAT_RX_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_heartbeat_rx_token_refill_must_be_positive[
    UCN_HEARTBEAT_RX_TOKEN_REFILL_MS > 0U ? 1 : -1];
#endif
typedef char ucn_duplicate_source_windows_must_not_be_empty[
    UCN_DUPLICATE_SOURCE_WINDOWS > 0U ? 1 : -1];
typedef char ucn_duplicate_window_bits_must_be_32_or_64[
    UCN_DUPLICATE_WINDOW_BITS == 32U || UCN_DUPLICATE_WINDOW_BITS == 64U ? 1 : -1];
typedef char ucn_duplicate_source_timeout_must_be_valid[
    UCN_DUPLICATE_SOURCE_TIMEOUT_MS > 0U &&
            UCN_DUPLICATE_SOURCE_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS ? 1 : -1];
#if UCN_FEATURE_DIAGNOSTICS
typedef char ucn_path_trace_rx_token_burst_must_be_positive[
    UCN_PATH_TRACE_RX_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_path_trace_rx_token_refill_must_be_positive[
    UCN_PATH_TRACE_RX_TOKEN_REFILL_MS > 0U ? 1 : -1];
typedef char ucn_path_trace_must_fit_two_node_ids[
    UCN_MAX_FRAME_BYTES >= UCN_PATH_TRACE_MIN_FRAME_BYTES &&
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
#endif
#if UCN_FEATURE_PATH
typedef char ucn_path_control_rx_source_depth_must_be_positive[
    UCN_PATH_CONTROL_RX_SOURCE_DEPTH > 0U ? 1 : -1];
typedef char ucn_path_control_rx_token_burst_must_be_positive[
    UCN_PATH_CONTROL_RX_TOKEN_BURST > 0U ? 1 : -1];
typedef char ucn_path_control_rx_token_refill_must_be_positive[
    UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS > 0U ? 1 : -1];
typedef char ucn_path_control_rx_source_idle_must_be_positive[
    UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS > 0U ? 1 : -1];
#endif
#if UCN_FEATURE_DYNAMIC_MESH
typedef char ucn_pending_q1_timeout_must_be_positive[
    UCN_PENDING_Q1_TIMEOUT_MS > 0U ? 1 : -1];
#endif
#if UCN_FEATURE_SECURITY
typedef char ucn_sequence_rotation_threshold_must_leave_valid_range[
    UCN_SEQUENCE_ROTATION_THRESHOLD > 1U &&
            UCN_SEQUENCE_ROTATION_THRESHOLD < UINT32_MAX ? 1 : -1];
#endif

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

/* Conservative S16 service bound: one actually-due maintenance action may
 * follow each business burst, and the scheduler may need to visit every
 * configured Neighbor/Bearer slot.  It intentionally overestimates products
 * whose UCN_MAX_LINKS is smaller than that product. */
#define UCN_MAINTENANCE_SERVICE_BOUND_CALC_MS \
    (UINT64_C(1) * \
     ((uint64_t)UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE + UINT64_C(1)) * \
     (uint64_t)UCN_MAX_STEP_INTERVAL_MS * (uint64_t)UCN_MAX_NEIGHBORS * \
     (uint64_t)UCN_MAX_BEARERS_PER_NEIGHBOR)
#define UCN_MAINTENANCE_SERVICE_BOUND_MS \
    ((uint32_t)UCN_MAINTENANCE_SERVICE_BOUND_CALC_MS)

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

#if UCN_FEATURE_DYNAMIC_MESH
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
typedef char ucn_max_step_interval_must_be_wrap_safe[
    UCN_MAX_STEP_INTERVAL_MS > 0U &&
            UCN_MAX_STEP_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS ?
        1 : -1];
typedef char ucn_maintenance_service_bound_must_fit_u32[
    UCN_MAINTENANCE_SERVICE_BOUND_CALC_MS <= UINT32_MAX ? 1 : -1];
typedef char ucn_maintenance_service_bound_must_precede_suspect[
    UINT64_C(1) * UCN_HEARTBEAT_INTERVAL_MS +
                UCN_MAINTENANCE_SERVICE_BOUND_CALC_MS <
            UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS ?
        1 : -1];
#endif
typedef char ucn_node_relative_durations_must_be_wrap_safe[
    UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS &&
#if UCN_FEATURE_DYNAMIC_MESH
    UCN_ROUTE_ENTRY_LIFETIME_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_ROUTE_REQUEST_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_ROUTE_REQUEST_MIN_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_ROUTE_REFRESH_ADVANCE_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_ROUTE_REFRESH_MIN_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_ROUTE_EPOCH_GRACE_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_CONTROL_TOKEN_REFILL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_ROUTE_REQUEST_RX_TOKEN_REFILL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_HEARTBEAT_RX_TOKEN_REFILL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_PENDING_Q1_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_HEARTBEAT_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_NEIGHBOR_REMOVE_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_BEARER_QUALITY_SAMPLE_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_BEARER_QUALITY_PROBE_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS &&
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
    UCN_ROUTE_CANDIDATE_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_PATH_PROBE_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS &&
#endif
#if UCN_FEATURE_DIAGNOSTICS
    UCN_PATH_TRACE_RX_TOKEN_REFILL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_PATH_TRACE_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_PATH_TRACE_TOKEN_REFILL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_NODE_SNAPSHOT_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_NODE_SNAPSHOT_REPLY_JITTER_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS <= UCN_MAX_SAFE_DURATION_MS &&
#endif
#if UCN_FEATURE_PATH
    UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS <= UCN_MAX_SAFE_DURATION_MS &&
#endif
    UCN_MAX_STEP_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS ? 1 : -1];

#ifndef UCN_MAX_ENDPOINT_HANDLERS
#define UCN_MAX_ENDPOINT_HANDLERS ((size_t)8U)
#endif

#ifndef UCN_MAX_ENDPOINT_SECURITY_POLICIES
#define UCN_MAX_ENDPOINT_SECURITY_POLICIES ((size_t)8U)
#endif

typedef struct ucn_node ucn_node_t;

/* API-only declaration.  Include ucn_node_storage.h only in the Protocol Task
 * owner translation unit that must allocate ucn_node_t statically. */

typedef void (*ucn_rx_handler_t)(void *context, const ucn_frame_t *frame);
typedef void (*ucn_endpoint_rx_handler_t)(void *context, const ucn_frame_t *frame);

typedef struct ucn_send_request {
    ucn_node_id_t destination;
    uint8_t message_type;
    ucn_traffic_class_t traffic_class;
    ucn_delivery_semantic_t delivery;
    uint32_t deadline_ms;
    const uint8_t *payload;
    uint16_t payload_length;
} ucn_send_request_t;

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

/* A NULL authorizer disables remote PATH_TRACE_REQ handling.  The hook is
 * evaluated at every relay/target before reverse state or a reply is created. */
typedef bool (*ucn_path_trace_authorize_fn)(void *context,
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
    UCN_PATH_CONTROL_REVOKE = 1,
    UCN_PATH_CONTROL_OPERATION_COUNT = 2
} ucn_path_control_operation_t;

typedef ucn_result_t (*ucn_path_control_authorize_fn)(
    void *context,
    const ucn_link_t *ingress_link,
    const ucn_frame_t *frame,
    ucn_path_control_operation_t operation,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop);

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

typedef struct ucn_node_stats {
    uint32_t tx_sent;
    uint32_t tx_expired_dropped;
    uint32_t tx_error_dropped;
    uint32_t q0_backpressure_retries;
    uint32_t q0_backpressure_exhausted;
    uint32_t q0_backpressure_expired;
    uint32_t q0_backpressure_terminal_failed;
#if UCN_FEATURE_DYNAMIC_MESH
    uint32_t maintenance_preemptions;
#endif
    uint32_t last_step_ms;
    uint32_t max_step_gap_ms;
    uint32_t step_interval_violations;
#if UCN_FEATURE_DYNAMIC_MESH
    uint32_t max_heartbeat_service_delay_ms;
    uint32_t max_probe_service_delay_ms;
#endif
    uint32_t rx_delivered;
    uint32_t duplicate_frames_dropped;
    uint32_t duplicate_source_window_full;
#if UCN_FEATURE_DYNAMIC_MESH
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
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
    uint32_t candidate_routes_learned;
    uint32_t path_probes_sent;
    uint32_t path_probe_acks_received;
    uint32_t route_switches;
    uint32_t candidate_rejected;
#endif
#if UCN_FEATURE_DYNAMIC_MESH
    uint32_t route_epoch_rejected;
#endif
#if UCN_FEATURE_SECURITY
    uint32_t e2e_protected_forwarded;
#endif
#if UCN_FEATURE_DYNAMIC_MESH
    uint32_t control_budget_dropped;
    uint32_t route_request_rx_rate_dropped;
    uint32_t route_request_replayed;
    uint32_t route_request_cache_full;
    uint32_t heartbeat_rx_rate_dropped;
    uint32_t path_trace_rx_rate_dropped;
    uint32_t q1_route_wait_queued;
    uint32_t q1_route_wait_expired;
#endif
#if UCN_FEATURE_DIAGNOSTICS
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
#endif
#if UCN_FEATURE_PATH
    uint32_t path_installs_sent;
    uint32_t path_installs_received;
    uint32_t path_revokes_sent;
    uint32_t path_revokes_received;
    uint32_t path_install_authorization_rejected;
    uint32_t path_revoke_authorization_rejected;
    uint32_t path_install_budget_rejected;
    uint32_t path_revoke_budget_rejected;
    uint32_t path_control_budget_source_full;
    uint32_t path_control_budget_session_rotations;
    uint32_t path_control_budget_sources_reclaimed;
    uint32_t path_install_table_full;
    uint32_t path_forwards;
    uint32_t path_rejected;
    uint32_t path_route_errors_sent;
#endif
#if UCN_FEATURE_SECURITY
    uint32_t session_rotations;
#endif
} ucn_node_stats_t;

ucn_result_t ucn_node_init(ucn_node_t *node, const ucn_config_t *config);
/* A new Node defaults to fixed W3 for source compatibility.  Configure a
 * smaller fixed domain before registering links or installing Security.
 * max_receive_profile may be wider than tx_profile so a gateway-capable Node
 * can accept and transparently forward narrower frames without truncation. */
ucn_result_t ucn_node_set_wire_profiles(
    ucn_node_t *node,
    ucn_wire_profile_t tx_profile,
    ucn_wire_profile_t max_receive_profile);
ucn_wire_profile_t ucn_node_get_tx_wire_profile(const ucn_node_t *node);
ucn_wire_profile_t ucn_node_get_max_receive_wire_profile(
    const ucn_node_t *node);
/* Automatic selection is disabled by default.  Fixed TX remains the maximum
 * and the fallback/domain-discovery profile. */
ucn_result_t ucn_node_set_wire_profile_auto(ucn_node_t *node, bool enabled);
bool ucn_node_wire_profile_auto(const ucn_node_t *node);
/* A registered Link may carry an explicit/HELLO-learned peer ceiling. */
ucn_result_t ucn_node_set_link_wire_profile_limit(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_wire_profile_t maximum_profile);
ucn_wire_profile_t ucn_node_get_link_wire_profile_limit(
    const ucn_node_t *node,
    const ucn_link_t *link);
/* Plain deployments should set a non-zero boot/session value before network
 * traffic so a reboot cannot reuse an old Source/Session sequence window.
 * A Security Provider replaces this value with its authenticated Session. */
ucn_result_t ucn_node_set_plain_session_id(ucn_node_t *node,
                                           ucn_session_id_t session_id);
/* Development remains compatible by default.  When required=true, Lite/Full
 * refuse protocol traffic until a Provider with authorization, persistent
 * sequence/session state and seal/open callbacks is installed, and the Node
 * plus every Endpoint override forbids plain TX/RX/forwarding.  This gate
 * cannot prove that the product's cryptography is audited. */
ucn_result_t ucn_node_set_security_required(ucn_node_t *node, bool required);
bool ucn_node_security_ready(const ucn_node_t *node);
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
ucn_result_t ucn_node_set_path_trace_authorizer(
    ucn_node_t *node,
    ucn_path_trace_authorize_fn authorize,
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
