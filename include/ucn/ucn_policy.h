#ifndef UCN_POLICY_H
#define UCN_POLICY_H

#include "ucn/ucn_endpoint.h"
#include "ucn/ucn_link.h"
#include "ucn/ucn_path.h"
#include "ucn/ucn_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* T22 uses small, compile-time bounded tables.  Products may lower these
 * limits for smaller MCUs; no Policy, Path, Flow or quality entry allocates
 * memory at runtime. */
#ifndef UCN_MAX_LINKS
#define UCN_MAX_LINKS ((size_t)4U)
#endif

#ifndef UCN_MAX_ROUTE_POLICIES
#define UCN_MAX_ROUTE_POLICIES ((size_t)8U)
#endif

#ifndef UCN_MAX_POLICY_PATHS
#define UCN_MAX_POLICY_PATHS ((size_t)8U)
#endif

#ifndef UCN_MAX_POLICY_FLOWS
#define UCN_MAX_POLICY_FLOWS ((size_t)8U)
#endif

#ifndef UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS
#define UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS UINT32_C(500)
#endif

#ifndef UCN_POLICY_QUALITY_EWMA_ALPHA_PERCENT
#define UCN_POLICY_QUALITY_EWMA_ALPHA_PERCENT ((uint8_t)25U)
#endif

/* AUTO_BALANCE is deliberately Q1-only and uses the existing fixed Flow
 * table.  A product may override these bounded defaults per MCU profile. */
#ifndef UCN_POLICY_BALANCE_FLOW_LEASE_MS
#define UCN_POLICY_BALANCE_FLOW_LEASE_MS UINT32_C(2000)
#endif

#ifndef UCN_POLICY_BALANCE_QUEUE_PRESSURE_THRESHOLD_PER_MILLE
#define UCN_POLICY_BALANCE_QUEUE_PRESSURE_THRESHOLD_PER_MILLE ((uint16_t)800U)
#endif

#ifndef UCN_POLICY_BALANCE_CONGESTED_SAMPLE_LIMIT
#define UCN_POLICY_BALANCE_CONGESTED_SAMPLE_LIMIT ((uint8_t)3U)
#endif

typedef char ucn_max_route_policies_must_be_positive[
    UCN_MAX_ROUTE_POLICIES > 0U ? 1 : -1];
typedef char ucn_max_policy_paths_must_be_positive[
    UCN_MAX_POLICY_PATHS > 0U ? 1 : -1];
typedef char ucn_max_policy_flows_must_be_positive[
    UCN_MAX_POLICY_FLOWS > 0U ? 1 : -1];
typedef char ucn_policy_quality_sample_interval_must_be_positive[
    UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS > 0U ? 1 : -1];
typedef char ucn_policy_quality_ewma_alpha_must_be_valid[
    UCN_POLICY_QUALITY_EWMA_ALPHA_PERCENT > 0U &&
    UCN_POLICY_QUALITY_EWMA_ALPHA_PERCENT <= 100U ? 1 : -1];
typedef char ucn_policy_balance_flow_lease_must_be_positive[
    UCN_POLICY_BALANCE_FLOW_LEASE_MS > 0U ? 1 : -1];
typedef char ucn_policy_balance_queue_pressure_threshold_must_be_valid[
    UCN_POLICY_BALANCE_QUEUE_PRESSURE_THRESHOLD_PER_MILLE <=
        UCN_LINK_METRIC_PER_MILLE_MAX ? 1 : -1];
typedef char ucn_policy_balance_congestion_samples_must_be_positive[
    UCN_POLICY_BALANCE_CONGESTED_SAMPLE_LIMIT > 0U ? 1 : -1];
typedef char ucn_policy_relative_durations_must_be_wrap_safe[
    UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS <= UCN_MAX_SAFE_DURATION_MS &&
    UCN_POLICY_BALANCE_FLOW_LEASE_MS <= UCN_MAX_SAFE_DURATION_MS ? 1 : -1];

/* A wildcard applies only to the traffic-class portion of a policy key.
 * Destination and Endpoint always remain explicit, so a product cannot
 * accidentally make every service follow one policy. */
#define UCN_POLICY_ANY_TRAFFIC_CLASS UINT8_MAX

typedef enum ucn_route_policy_mode {
    UCN_ROUTE_POLICY_AUTO_BEST = 0,
    UCN_ROUTE_POLICY_PINNED_STRICT = 1,
    UCN_ROUTE_POLICY_PINNED_FAILOVER = 2,
    UCN_ROUTE_POLICY_AUTO_BALANCE = 3
} ucn_route_policy_mode_t;

typedef enum ucn_policy_path_state {
    UCN_POLICY_PATH_EMPTY = 0,
    UCN_POLICY_PATH_CANDIDATE = 1,
    UCN_POLICY_PATH_VERIFIED = 2,
    UCN_POLICY_PATH_SUSPECT = 3,
    UCN_POLICY_PATH_DOWN = 4
} ucn_policy_path_state_t;

typedef struct ucn_route_policy_key {
    ucn_node_id_t destination;
    ucn_endpoint_t endpoint;
    uint8_t traffic_class;
} ucn_route_policy_key_t;

typedef struct ucn_route_policy_config {
    ucn_route_policy_key_t key;
    ucn_route_policy_mode_t mode;
    /* Local handles into the fixed policy Path table.  T22.3 resolves these
     * to authenticated wire Path IDs only when a PINNED mode is selected. */
    uint16_t primary_local_path_id;
    uint16_t backup_local_path_id;
    /* Applies only to PINNED_FAILOVER and only after both configured Paths
     * have a hard failure.  Q0 never starts discovery. */
    bool allow_discovery_on_hard_failure;
    /* AUTO_BALANCE only: zero selects the profile default.  A Flow stays on
     * its selected Path for this lease unless that Path is hard-down or
     * remains congested for the configured sample count. */
    uint32_t balance_flow_lease_ms;
} ucn_route_policy_config_t;

typedef struct ucn_route_policy_entry {
    bool occupied;
    ucn_route_policy_config_t config;
    /* T22.6 keeps a per-policy hit counter so a management query can show
     * whether this exact rule, rather than merely some policy, was selected. */
    uint32_t match_hits;
} ucn_route_policy_entry_t;

/* local_path_id remains a local fixed-table handle.  T22.2's authenticated
 * wire_path_id is separate so the product can replace a Path without changing
 * every Policy reference.  A zero wire_path_id is allowed for T22.1 metadata,
 * but a PINNED policy cannot send through it. */
typedef struct ucn_policy_path_config {
    uint16_t local_path_id;
    ucn_path_id_t wire_path_id;
    ucn_node_id_t destination;
    ucn_link_t *egress_link;
    bool verified;
} ucn_policy_path_config_t;

typedef struct ucn_policy_path_entry {
    bool occupied;
    uint16_t local_path_id;
    ucn_path_id_t wire_path_id;
    ucn_node_id_t destination;
    ucn_link_t *egress_link;
    ucn_policy_path_state_t state;
    uint8_t congestion_samples;
    uint32_t configured_at_ms;
} ucn_policy_path_entry_t;

typedef struct ucn_policy_flow_key {
    ucn_node_id_t destination;
    ucn_endpoint_t endpoint;
    ucn_traffic_class_t traffic_class;
} ucn_policy_flow_key_t;

typedef struct ucn_policy_flow_binding {
    bool occupied;
    ucn_policy_flow_key_t key;
    uint16_t local_path_id;
    uint32_t expires_at_ms;
    uint32_t last_used_at_ms;
} ucn_policy_flow_binding_t;

typedef struct ucn_policy_link_quality_snapshot {
    bool occupied;
    ucn_link_t *link;
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
} ucn_policy_link_quality_snapshot_t;

typedef struct ucn_policy_stats {
    uint32_t policy_config_updates;
    uint32_t policy_match_hits;
    uint32_t path_config_updates;
    uint32_t flow_bindings_created;
    uint32_t flow_bindings_expired;
    uint32_t quality_samples;
    uint32_t quality_metrics_unavailable;
    uint32_t quality_link_down;
    uint32_t pinned_strict_sends;
    uint32_t pinned_strict_failures;
    uint32_t pinned_failover_primary_sends;
    uint32_t pinned_failover_backup_sends;
    uint32_t pinned_failover_hard_failures;
    uint32_t pinned_failover_discovery_fallbacks;
    uint32_t pinned_policy_config_errors;
    uint32_t auto_balance_sends;
    uint32_t auto_balance_flow_bindings;
    uint32_t auto_balance_rebindings;
    uint32_t auto_balance_congestion_rebindings;
    uint32_t auto_balance_down_rebindings;
    uint32_t auto_balance_selection_failures;
} ucn_policy_stats_t;

typedef struct ucn_policy_state {
    ucn_route_policy_entry_t policies[UCN_MAX_ROUTE_POLICIES];
    ucn_policy_path_entry_t paths[UCN_MAX_POLICY_PATHS];
    ucn_policy_flow_binding_t flows[UCN_MAX_POLICY_FLOWS];
    ucn_policy_link_quality_snapshot_t quality[UCN_MAX_LINKS];
    bool quality_sampled;
    uint32_t last_quality_sample_ms;
    ucn_policy_stats_t stats;
} ucn_policy_state_t;

/* Internal Core maintenance hooks used by ucn_node_step(). */
void ucn_policy_refresh_link_quality(ucn_policy_state_t *state,
                                     ucn_link_t *const *links,
                                     size_t link_count,
                                     uint32_t now_ms);
void ucn_policy_expire_flows(ucn_policy_state_t *state, uint32_t now_ms);
void ucn_policy_mark_path_down(ucn_policy_state_t *state,
                               uint16_t local_path_id);
void ucn_policy_touch_q1_flow(ucn_policy_state_t *state,
                              ucn_node_id_t destination,
                              ucn_endpoint_t endpoint,
                              uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
