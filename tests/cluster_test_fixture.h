/* CLV2-M00-04: cluster test fixture helpers.
 *
 * Tests must stop writing production struct fields directly so a later
 * M01/M02 refactor can change the internal layout without touching every
 * test.  These helpers centralize the common white-box state setups.
 * They remain test-only; production code never includes this header. */
#ifndef UCN_CLUSTER_TEST_FIXTURE_H
#define UCN_CLUSTER_TEST_FIXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "ucn/ucn_cluster_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Replace the active epoch identity (cluster_id/term/head) without
 * touching role or membership state. */
void cluster_fixture_set_epoch(ucn_cluster_t *cluster,
                              uint32_t cluster_id,
                              uint32_t term,
                              ucn_node_id_t head_node_id);

/* Force a public role (test setups only). */
void cluster_fixture_set_role(ucn_cluster_t *cluster,
                             ucn_cluster_role_t role);

/* Configure the Backup mirror identity + readiness flags. */
void cluster_fixture_set_backup(ucn_cluster_t *cluster,
                               ucn_node_id_t backup_primary,
                               uint32_t generation,
                               bool syncing,
                               bool ready);

/* Record a takeover vote identity (cluster, term, generation). */
void cluster_fixture_set_vote(ucn_cluster_t *cluster,
                              uint32_t cluster_id,
                              uint32_t term,
                              uint32_t generation);

#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS)
/* CLV2-01-04a: test-only access to the static single transition entry
 * point.  The hooks are compiled only into the ucn_tests copy of
 * ucn_cluster.c (see CMakeLists.txt); production archives never export
 * them. */
ucn_result_t ucn_cluster_test_transition(
    ucn_cluster_t *cluster,
    ucn_cluster_phase_t old_phase,
    ucn_cluster_phase_t new_phase,
    ucn_cluster_transition_reason_t reason,
    uint32_t now_ms);

/* Toggle the debug assert on illegal transitions so rejection tests can
 * exercise the fail-closed release path (UCN_ERR_STATE) without
 * aborting. */
void ucn_cluster_test_transition_asserts_set(bool enabled);

/* Test-only view of the BEST-EFFORT pair->reason table (F4 fallback). */
ucn_cluster_transition_reason_t ucn_cluster_test_reason_from_diff(
    ucn_cluster_phase_t old_phase,
    ucn_cluster_phase_t new_phase);

/* CLV2-01-04b NIT-1: test-only view of the PRODUCTION OBSERVED_ALLOWED
 * table (DIRECT union the tick-granularity compounds), so the T-A gate
 * checks the single production table instead of a test-side duplicate. */
bool ucn_cluster_test_observed_pair_allowed(
    ucn_cluster_phase_t old_phase,
    ucn_cluster_phase_t new_phase);

/* CLV2-01-04d.0: test-only view of the pure-validation preflight (NEVER
 * commits), so tests can prove a rejected preflight performs ZERO writes. */
ucn_result_t ucn_cluster_test_transition_preflight(
    ucn_cluster_t *cluster,
    ucn_cluster_phase_t old_phase,
    ucn_cluster_phase_t new_phase,
    uint32_t now_ms);

/* CLV2-01-04d.4: test-only views of the static d-group sites
 * remove_member() / expire_members() (see ucn_cluster.c), so tests can
 * drive the backup-eviction preflight pattern directly. */
void ucn_cluster_test_remove_member(ucn_cluster_t *cluster,
                                    ucn_node_id_t node_id,
                                    uint32_t now_ms);
void ucn_cluster_test_expire_members(ucn_cluster_t *cluster,
                                     uint32_t now_ms);

/* CLV2-01-04d.7: test-only views of the head-ladder sites wired in this
 * point (start_backup_assignment_cycle / send_backup_assignment_step). */
void ucn_cluster_test_start_backup_assignment_cycle(ucn_cluster_t *cluster,
                                                    uint32_t now_ms);
void ucn_cluster_test_send_backup_assignment_step(ucn_cluster_t *cluster,
                                                  uint32_t now_ms);
void ucn_cluster_test_assign_backup(ucn_cluster_t *cluster, uint32_t now_ms);
void ucn_cluster_test_queue_backup_assignment_for_member(
    ucn_cluster_t *cluster, ucn_node_id_t member_node_id, uint32_t now_ms);

/* CLV2-01-04e: test-only views of the takeover-lifecycle sites wired in
 * this point (start_takeover / complete_takeover), so tests can drive the
 * BACKUP_READY -> BACKUP_TAKEOVER and BACKUP_TAKEOVER -> HEAD_NO_BACKUP
 * transitions directly and verify the full site-side field effects (and
 * the fail-closed rejection with zero writes).  Return UCN_OK when the
 * transition committed, UCN_ERR_STATE on a rejected transition, or
 * UCN_ERR_EXHAUSTED when the next Term would violate the no-wrap gate. */
ucn_result_t ucn_cluster_test_start_takeover(ucn_cluster_t *cluster,
                                             uint32_t now_ms);
ucn_result_t ucn_cluster_test_complete_takeover(ucn_cluster_t *cluster,
                                                uint32_t now_ms);

/* CLV2-M11 (11-09): test-only view of the retired v3 score challenge.
 * It must return UCN_ERR_UNSUPPORTED with no writes; a durable planned
 * transfer belongs to the separate, default-OFF handover experiment. */
ucn_result_t ucn_cluster_test_backup_challenge(ucn_cluster_t *cluster,
                                               uint32_t now_ms);

/* CLV2-01-04f: test-only views of the static RECOVERY-domain offer sites
 * consider_head_offer() / begin_ordered_stepdown(), so tests can drive the
 * RECOVERY_* -> JOIN_PENDING and RECOVERY_HEAD -> STEPPING_DOWN transitions
 * directly and verify the full site-side field effects (and the fail-closed
 * rejection with zero writes) without an end-of-RX shadow sync re-aligning
 * the mirror.  Return UCN_OK when the transition committed, UCN_ERR_STATE
 * when it was rejected (fail closed). */
ucn_result_t ucn_cluster_test_consider_head_offer(ucn_cluster_t *cluster,
                                                  ucn_cluster_candidate_t *candidate,
                                                  uint32_t now_ms);
ucn_result_t ucn_cluster_test_begin_ordered_stepdown(ucn_cluster_t *cluster,
                                                     const ucn_cluster_candidate_t *candidate,
                                                     uint32_t now_ms);

/* CLV2-M12 (12-01): test-only view of cluster_lineage_capture(). */
void ucn_cluster_test_lineage_capture(ucn_cluster_t *cluster);
/* CLV2-M12 (12-02): test-only view of the recovery ID allocation entry. */
ucn_result_t ucn_cluster_test_make_next_recovery_id(
    ucn_cluster_t *cluster,
    uint32_t parent_cluster_id,
    uint32_t parent_term,
    uint32_t parent_config_id,
    uint32_t recovery_round,
    uint32_t *cluster_id);
#endif

#ifdef __cplusplus
}
#endif

#endif
