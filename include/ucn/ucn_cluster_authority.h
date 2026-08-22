#ifndef UCN_CLUSTER_AUTHORITY_H
#define UCN_CLUSTER_AUTHORITY_H

/* CLV2-M08: controlled Authority / Quorum / Fence Owner.
 *
 * This is intentionally an opt-in Extended owner.  It consumes an already
 * canonical M07 Config value and explicit Cluster keepalive evidence; it
 * neither parses v4 frames nor manufactures a Config, persistence proof,
 * voter or Authority from legacy v3 traffic.  REQUIRED product integration
 * remains blocked by M05's top-level AUDIT HOLD. */

#include "ucn/ucn_cluster_config_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_cluster_timing_budget {
    uint32_t owner_step_budget_ms;
    uint32_t one_way_network_budget_ms;
    uint32_t retry_budget_ms;
    uint32_t scheduler_jitter_ms;
    uint32_t clock_drift_budget_ms;
    uint32_t safety_margin_ms;
} ucn_cluster_timing_budget_t;

/* All values are derived, never independently guessed by a caller.  A
 * voter lease covers three complete budget windows; Grace, restore hold and
 * dissolution use separate bounded windows so no timer is an implicit
 * permission to keep writing. */
typedef struct ucn_cluster_authority_timing {
    uint32_t owner_step_budget_ms;
    uint32_t control_window_ms;
    uint32_t voter_lease_ms;
    uint32_t authority_grace_ms;
    uint32_t quorum_restore_hold_ms;
    uint32_t member_takeover_grace_ms;
    uint32_t fenced_dissolve_ms;
} ucn_cluster_authority_timing_t;

typedef struct ucn_cluster_authority_runtime {
    ucn_cluster_t *cluster;
    ucn_cluster_config_state_t active_config;
    ucn_cluster_authority_timing_t timing;
    ucn_node_id_t voter_node_ids[UCN_CLUSTER_MAX_VOTERS];
    uint32_t voter_lease_deadlines_ms[UCN_CLUSTER_MAX_VOTERS];
    uint8_t voter_count;
    bool initialized;
    bool owner_step_seen;
    bool restore_hold_armed;
    bool fence_latched;
    bool higher_authority_seen;
    uint32_t last_owner_step_ms;
} ucn_cluster_authority_runtime_t;

/* Each nonzero budget component must be a valid modular-clock duration.
 * Output is not written on failure. */
ucn_result_t ucn_cluster_authority_timing_derive(
    const ucn_cluster_timing_budget_t *budget,
    ucn_cluster_authority_timing_t *output);

/* The Member side must never leave its post-Head-lease wait before the
 * Backup had time to finish its bounded vote window.  The result is exactly
 * max(0, backup_lease - member_lease) + takeover_window + control_window.
 * Output remains untouched on invalid input or arithmetic overflow. */
ucn_result_t ucn_cluster_authority_member_takeover_grace_derive(
    const ucn_cluster_authority_timing_t *timing,
    uint32_t backup_lease_ms,
    uint32_t member_lease_ms,
    uint32_t takeover_window_ms,
    uint32_t *output);

/* Attaches one Authority Owner to a VOLATILE_TEST Cluster for controlled
 * Host/experiment integration.  The caller-owned runtime storage must stay
 * valid until the Cluster is no longer stepped or transmitted.  It requires
 * a canonical Config containing the local Head in both sets.  REQUIRED
 * product wiring must later bind the M07 durable Config owner explicitly;
 * M08 deliberately rejects it here rather than presenting a value-only
 * Config as persistence proof. */
ucn_result_t ucn_cluster_authority_runtime_init(
    ucn_cluster_authority_runtime_t *runtime,
    ucn_cluster_t *cluster,
    const ucn_cluster_config_state_t *config,
    const ucn_cluster_authority_timing_t *timing,
    uint32_t now_ms);

/* Replaces the immutable Config snapshot only while the runtime is not
 * fenced.  The transition is atomic at now_ms: old Authority is first
 * revoked, then the candidate Stable/Joint quorum is evaluated against its
 * retained voter leases.  A missing candidate quorum enters Grace without
 * leaving a stale active permission. */
ucn_result_t ucn_cluster_authority_runtime_install_config(
    ucn_cluster_authority_runtime_t *runtime,
    const ucn_cluster_config_state_t *config,
    uint32_t now_ms);

/* Protocol-level liveness evidence.  Core Neighbor SUSPECT is deliberately
 * not an input.  A non-voter is rejected and does not alter runtime state. */
ucn_result_t ucn_cluster_authority_runtime_note_voter_keepalive(
    ucn_cluster_authority_runtime_t *runtime,
    ucn_node_id_t voter_node_id,
    uint32_t now_ms);

/* Runs one Owner tick.  It is the only normal path that enables Authority,
 * and it revokes Authority before entering quorum Grace in the same call. */
ucn_result_t ucn_cluster_authority_runtime_step(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t now_ms);

/* Refreshes Authority at the caller's current clock before an RX/TX or
 * Federation authority side effect.  An unmanaged Cluster is a no-op; an
 * installed Owner performs the same bounded evaluation as an Owner step. */
ucn_result_t ucn_cluster_authority_runtime_preflight(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t now_ms);

/* A merely observed higher same-Cluster Term is sufficient to revoke local
 * Authority, but cannot select JOIN_PENDING: legacy v3 or an unauthenticated
 * offer is never evidence of the M10 frozen-Config certificate. */
ucn_result_t ucn_cluster_authority_runtime_note_higher_term_observed(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id,
    uint32_t now_ms);

/* Call only after the future M10/M11 RX Owner validated a higher Stable
 * Authority against its frozen Config/certificate.  It immediately latches
 * the Fence and selects JOIN_PENDING on the following Owner step; it never
 * itself changes epoch/role. */
ucn_result_t ucn_cluster_authority_runtime_note_higher_authority(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id,
    uint32_t now_ms);
ucn_result_t ucn_cluster_authority_runtime_note_term_conflict(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t conflicting_head_node_id,
    uint32_t now_ms);

bool ucn_cluster_authority_runtime_quorum_met(
    const ucn_cluster_authority_runtime_t *runtime,
    uint32_t now_ms);
bool ucn_cluster_authority_runtime_tx_allowed(
    const ucn_cluster_authority_runtime_t *runtime,
    ucn_cluster_message_type_t type,
    ucn_cluster_role_t sender_role);

#ifdef __cplusplus
}
#endif

#endif
