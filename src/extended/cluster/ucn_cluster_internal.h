/* CLV2-M02 OP-211 (02-01): Cluster private/internal header.
 *
 * Inter-module boundary for the src/extended/cluster/ decomposition
 * (task table CLV2-02-01).  This header is included ONLY by
 * src/extended/ sources; ucn_core must never include it (the dependency
 * graph stays Extended -> Core, never Core -> Extended).
 *
 * == Codec module boundary (02-02, this OP) ==
 * ucn_cluster_codec_v3.c exposes NOTHING beyond the public API: the two
 * functions it defines (ucn_cluster_message_encode() /
 * ucn_cluster_message_decode()) are already declared in
 * include/ucn/ucn_cluster.h (included below), and every byte-offset
 * define / validation / read-be / write-be helper is file-static inside
 * the codec module.  Nothing additional needs declaring here for 02-02;
 * later OPs append their inter-module interfaces below.
 *
 * == Module layout for the upcoming OPs (append here) ==
 * 02-03  ucn_cluster_fsm.c        cluster_transition, Phase handlers,
 *                                 Step dispatch, state invariants and
 *                                 Role mapping; ucn_cluster.c keeps the
 *                                 public facade / init / views.
 * 02-04  ucn_cluster_membership.c Join, Keepalive, Leave, member
 *                                 allocation/expiry, member query.
 * 02-05  ucn_cluster_backup.c     Backup selection / assignment /
 *                                 snapshot / delta / heartbeat / reject /
 *                                 resync.
 *        ucn_cluster_takeover.c   Takeover prepare / ACK / complete.
 * 02-06  ucn_cluster_recovery.c   Recovery quorum / declaration /
 *                                 arbitration / TTL.
 *        ucn_cluster_merge.c      Head offer / stepdown / score switch.
 */

#ifndef UCN_CLUSTER_INTERNAL_H
#define UCN_CLUSTER_INTERNAL_H

#include "ucn/ucn_cluster.h"
#include "ucn/ucn_cluster_persist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* == FSM module boundary (02-03, this OP) ==
 * ucn_cluster_fsm.c owns the explicit-phase transition framework and
 * exposes exactly these four helpers to the other Cluster modules.  They
 * had static linkage inside the former single ucn_cluster.c; M02 (02-03)
 * gives them external linkage through this header, nothing else changes
 * (bodies byte-identical, M01-frozen).  The DIRECT/OBSERVED matrices, the
 * reason tables, cluster_shadow_sync(), apply_legacy, validate and the
 * test hooks stay file-static inside the fsm module. */

ucn_cluster_phase_t cluster_phase_from_legacy_state(
    const ucn_cluster_t *cluster, uint32_t now_ms);
bool cluster_legacy_state_is_valid(const ucn_cluster_t *cluster);
ucn_result_t cluster_transition(ucn_cluster_t *cluster,
                                ucn_cluster_phase_t old_phase,
                                ucn_cluster_phase_t new_phase,
                                ucn_cluster_transition_reason_t reason,
                                uint32_t now_ms);
ucn_result_t cluster_transition_preflight(ucn_cluster_t *cluster,
                                          ucn_cluster_phase_t old_phase,
                                          ucn_cluster_phase_t new_phase,
                                          uint32_t now_ms);

/* Time + shadow-mirror helpers shared with the remaining ucn_cluster.c
 * modules (same M02 02-03 exposure rule: de-static only, bodies
 * untouched). */
uint32_t cluster_now(const ucn_cluster_t *cluster);
ucn_cluster_transition_reason_t cluster_rx_reason_from_type(
    ucn_cluster_message_type_t type);
void cluster_shadow_sync(ucn_cluster_t *cluster,
                         ucn_cluster_transition_reason_t reason_hint);

/* == M04 persistence runtime boundary ==
 * Provider I/O is centralized here.  One pending operation freezes Cluster
 * progress; a terminal COMMITTED result is accepted only after a fresh load
 * proves the same durable operation journal identity. */
typedef enum cluster_persistence_action {
    CLUSTER_PERSIST_ACTION_NONE = 0,
    CLUSTER_PERSIST_ACTION_ELECTION_START = 1,
    CLUSTER_PERSIST_ACTION_BACKUP_CHALLENGE = 2,
    CLUSTER_PERSIST_ACTION_TAKEOVER_COMMIT = 3,
    CLUSTER_PERSIST_ACTION_RECOVERY_DECLARE = 4,
    CLUSTER_PERSIST_ACTION_TAKEOVER_ACK = 5,
    CLUSTER_PERSIST_ACTION_BOOT_INCARNATION = 6
} cluster_persistence_action_t;

typedef struct cluster_persistence_resolution {
    cluster_persistence_action_t action;
    ucn_node_id_t destination;
    ucn_cluster_persist_state_t durable_state;
} cluster_persistence_resolution_t;

bool cluster_persistence_outbound_allowed(const ucn_cluster_t *cluster);
bool cluster_persistence_progress_blocked(const ucn_cluster_t *cluster);
void cluster_persistence_schedule_retry(ucn_cluster_t *cluster,
                                        cluster_persistence_action_t action,
                                        ucn_node_id_t destination);
/* A durable TAKEOVER_ACK may be delayed by a local queue or temporarily
 * unavailable bearer.  These are transport outcomes, never persistence
 * failures; the retry descriptor owns the only permitted outbound path. */
bool cluster_persistence_takeover_ack_send_is_retryable(ucn_result_t result);
ucn_result_t cluster_persistence_retry_pending(ucn_cluster_t *cluster);
ucn_result_t cluster_persistence_load_snapshot(
    ucn_cluster_t *cluster,
    ucn_cluster_persist_state_t *state);
ucn_result_t cluster_persistence_load_snapshot_ex(
    ucn_cluster_t *cluster,
    ucn_cluster_persist_state_t *state,
    bool *factory_empty);
ucn_result_t cluster_persistence_begin_epoch(
    ucn_cluster_t *cluster,
    const ucn_cluster_epoch_t *epoch,
    cluster_persistence_action_t action,
    ucn_node_id_t destination,
    bool *committed,
    ucn_cluster_persist_state_t *durable_state);
ucn_result_t cluster_persistence_begin_vote(
    ucn_cluster_t *cluster,
    const ucn_cluster_persist_vote_t *vote,
    cluster_persistence_action_t action,
    ucn_node_id_t destination,
    bool *committed,
    ucn_cluster_persist_state_t *durable_state);
ucn_result_t cluster_persistence_begin_state(
    ucn_cluster_t *cluster,
    const ucn_cluster_persist_state_t *current_state,
    ucn_cluster_persist_operation_t operation,
    const ucn_cluster_persist_state_t *next_state,
    cluster_persistence_action_t action,
    ucn_node_id_t destination,
    bool *committed,
    ucn_cluster_persist_state_t *durable_state);
ucn_result_t cluster_persistence_poll(
    ucn_cluster_t *cluster,
    bool *resolved,
    cluster_persistence_resolution_t *resolution);
void cluster_persistence_fail_closed(ucn_cluster_t *cluster,
                                     ucn_result_t failure);

/* == Membership module boundary (02-04, this OP) ==
 * ucn_cluster_membership.c owns the member table (allocation / expiry /
 * query) and the Join / Keepalive / Leave handlers.  Exposed here
 * (de-static only) so the remaining ucn_cluster.c modules (receive
 * dispatch, step, backup) can call them. */
uint16_t primary_member_count_u16(const ucn_cluster_t *cluster);
uint16_t primary_member_protected_voter_count_u16(const ucn_cluster_t *cluster);
uint16_t primary_member_available_capacity(const ucn_cluster_t *cluster);
ucn_cluster_member_t *primary_member_find(ucn_cluster_t *cluster,
                                          ucn_node_id_t node_id);
ucn_cluster_member_t *primary_member_allocate(ucn_cluster_t *cluster,
                                              ucn_node_id_t node_id,
                                              uint32_t now_ms);
/* CLV2-M06 (06-04): this is deliberately a post-validation admission
 * primitive, not a wire receive entry point.  A future RX owner may call it
 * only after it has completed every v4 type/role/source/epoch/capability
 * check.  It records a Runtime PROVISIONAL member and never modifies the
 * active voter set, Backup state or Authority. */
ucn_result_t cluster_admit_verified_v4_provisional_member(
    ucn_cluster_t *cluster,
    ucn_node_id_t node_id,
    uint16_t capabilities,
    uint32_t now_ms,
    ucn_cluster_member_admission_reason_t *reason);
/* M06 capacity-only preflight for the future M07 Config Commit owner.  It
 * never changes member status or the active voter set. */
ucn_result_t cluster_preflight_provisional_voter_commit(
    const ucn_cluster_t *cluster,
    ucn_node_id_t node_id,
    ucn_cluster_member_admission_reason_t *reason);
/* Expire only bounded Runtime PROVISIONAL records.  It cannot modify a
 * committed record or the active voter set; the caller owns any later
 * backup-mirror synchronization. */
size_t primary_member_expire_provisionals(ucn_cluster_t *cluster,
                                          uint32_t now_ms);
/* CLV2-M06 (06-06): production v3 membership initializes as bounded
 * PROVISIONAL/non-voting legacy state.  The test-hook-only branch preserves
 * old v3 fixtures until M07 replaces it with genuine Config Commit. */
bool member_initialize_legacy(ucn_cluster_member_t *member,
                              ucn_node_id_t node_id,
                              uint32_t now_ms,
                              uint32_t provisional_timeout_ms);
void member_note_legacy_keepalive(ucn_cluster_member_t *member,
                                  uint32_t now_ms);
bool primary_member_is_protected_voter(const ucn_cluster_member_t *member);
void remove_member(ucn_cluster_t *cluster, ucn_node_id_t node_id,
                   uint32_t now_ms);
void primary_member_table_clear(ucn_cluster_t *cluster);
ucn_result_t handle_join_request(ucn_cluster_t *cluster,
                                 ucn_node_id_t source,
                                 const ucn_cluster_message_t *message,
                                 uint32_t now_ms);
ucn_result_t send_join_reply(ucn_cluster_t *cluster,
                             ucn_node_id_t destination,
                             ucn_cluster_message_type_t type,
                             uint32_t join_nonce);
ucn_result_t handle_join_accept(ucn_cluster_t *cluster,
                                ucn_node_id_t source,
                                const ucn_cluster_message_t *message,
                                uint32_t now_ms);
ucn_result_t handle_keepalive(ucn_cluster_t *cluster,
                              ucn_node_id_t source,
                              const ucn_cluster_message_t *message,
                              uint32_t now_ms);
void expire_members(ucn_cluster_t *cluster, uint32_t now_ms);
void send_join_request(ucn_cluster_t *cluster, uint32_t now_ms);
void send_keepalive(ucn_cluster_t *cluster, uint32_t now_ms);

/* Send / token / backup helpers that the membership module calls but
 * which stay in ucn_cluster.c until their own OPs (02-05 backup).  Same
 * exposure rule: de-static only, bodies untouched. */
ucn_result_t send_cluster_message(ucn_cluster_t *cluster,
                                  ucn_node_id_t destination,
                                  const ucn_cluster_message_t *message);
ucn_result_t send_message(ucn_cluster_t *cluster,
                          ucn_node_id_t destination,
                          ucn_cluster_message_type_t type,
                          ucn_cluster_role_t role, uint32_t cluster_id,
                          uint32_t term, ucn_node_id_t head_node_id,
                          uint16_t head_score, uint16_t capacity);
uint32_t next_nonce(ucn_cluster_t *cluster);
ucn_result_t cluster_serial_next_checked(uint32_t current,
                                         uint32_t *next);
ucn_result_t cluster_make_next_id(ucn_cluster_t *cluster,
                                  ucn_cluster_id_purpose_t purpose,
                                  uint32_t parent_cluster_id,
                                  uint32_t parent_term,
                                  uint32_t *cluster_id);
void backup_resync(ucn_cluster_t *cluster);
void assign_backup(ucn_cluster_t *cluster, uint32_t now_ms);
void queue_backup_assignment_for_member(ucn_cluster_t *cluster,
                                        ucn_node_id_t member_node_id,
                                        uint32_t now_ms);
void backup_clear_sync(ucn_cluster_t *cluster, uint32_t now_ms);
const ucn_cluster_peer_t *find_peer(const ucn_cluster_t *cluster,
                                    ucn_node_id_t node_id);
ucn_cluster_candidate_t *find_candidate(ucn_cluster_t *cluster,
                                        ucn_node_id_t node_id);
void set_detached(ucn_cluster_t *cluster, uint32_t now_ms,
                  uint32_t observation_ms);
/* CLV2-M12 (12-01): recovery lineage capture at a Member/Backup fence
 * exit (site-owned; must run BEFORE the Active/Pending clear). */
void cluster_lineage_capture(ucn_cluster_t *cluster);
/* CLV2-M12 (12-02): Recovery identity keyed on the full lineage. */
ucn_result_t cluster_make_next_recovery_id(ucn_cluster_t *cluster,
                                           uint32_t parent_cluster_id,
                                           uint32_t parent_term,
                                           uint32_t parent_config_id,
                                           uint32_t recovery_round,
                                           uint32_t *cluster_id);
/* CLV2-M12 (12-03): sustained-stable-join lineage reset + avalanche mix
 * shared with the recovery backoff jitter. */
void cluster_lineage_reset_arm(ucn_cluster_t *cluster, uint32_t now_ms);
void cluster_lineage_reset(ucn_cluster_t *cluster);
uint32_t cluster_id_mix(uint32_t value);
void cluster_history_note_stable_epoch(ucn_cluster_t *cluster,
                                       uint32_t cluster_id,
                                       uint32_t term,
                                       ucn_node_id_t head_node_id);
bool cluster_history_offer_is_stale(const ucn_cluster_t *cluster,
                                    const ucn_cluster_candidate_t *candidate);
void consider_head_offer(ucn_cluster_t *cluster,
                          ucn_cluster_candidate_t *candidate,
                          uint32_t now_ms);
bool process_higher_authority(ucn_cluster_t *cluster,
                               const ucn_cluster_candidate_t *candidate,
                               uint32_t now_ms);
bool process_term_conflict(ucn_cluster_t *cluster,
                           const ucn_cluster_candidate_t *candidate,
                           uint32_t now_ms);

/* == Backup/Takeover module boundary (02-05, this OP) ==
 * ucn_cluster_backup.c owns the Backup selection / assignment / snapshot /
 * delta / heartbeat / reject / resync and the Takeover prepare / ACK /
 * complete lifecycle.  Exposed here (de-static only) for the remaining
 * ucn_cluster.c modules (receive dispatch, step). */
ucn_result_t send_backup_assign(ucn_cluster_t *cluster,
                                ucn_node_id_t destination);
ucn_result_t send_backup_ready(ucn_cluster_t *cluster);
ucn_result_t send_backup_resync_req(ucn_cluster_t *cluster);
ucn_result_t send_backup_reject(ucn_cluster_t *cluster, uint8_t reason);
ucn_result_t handle_backup_assign(ucn_cluster_t *cluster,
                                  ucn_node_id_t source,
                                  const ucn_cluster_message_t *message,
                                  uint32_t now_ms);
ucn_result_t handle_backup_ready(ucn_cluster_t *cluster,
                                 ucn_node_id_t source,
                                 const ucn_cluster_message_t *message,
                                 uint32_t now_ms);
ucn_result_t handle_backup_member_sync(ucn_cluster_t *cluster,
                                       ucn_node_id_t source,
                                       const ucn_cluster_message_t *message,
                                       uint32_t now_ms);
ucn_result_t handle_primary_heartbeat(ucn_cluster_t *cluster,
                                      ucn_node_id_t source,
                                      const ucn_cluster_message_t *message,
                                      uint32_t now_ms);
ucn_result_t handle_backup_resync_req(ucn_cluster_t *cluster,
                                      ucn_node_id_t source,
                                      const ucn_cluster_message_t *message);
ucn_result_t handle_backup_reject(ucn_cluster_t *cluster,
                                  ucn_node_id_t source,
                                  const ucn_cluster_message_t *message,
                                  uint32_t now_ms);
ucn_result_t handle_takeover_prepare(ucn_cluster_t *cluster,
                                     ucn_node_id_t source,
                                     const ucn_cluster_message_t *message,
                                     uint32_t now_ms);
ucn_result_t send_takeover_ack_after_persistence(
    ucn_cluster_t *cluster,
    ucn_node_id_t destination,
    const ucn_cluster_persist_state_t *durable_state);
ucn_result_t handle_takeover_ack(ucn_cluster_t *cluster,
                                 ucn_node_id_t source,
                                 const ucn_cluster_message_t *message,
                                 uint32_t now_ms);
ucn_result_t handle_head_takeover(ucn_cluster_t *cluster,
                                  ucn_node_id_t source,
                                  const ucn_cluster_message_t *message,
                                  uint32_t now_ms);
ucn_result_t complete_takeover(ucn_cluster_t *cluster, uint32_t now_ms);
ucn_result_t complete_takeover_after_persistence(
    ucn_cluster_t *cluster,
    const ucn_cluster_persist_state_t *durable_state,
    uint32_t now_ms);
void start_takeover(ucn_cluster_t *cluster, uint32_t now_ms);
void start_backup_assignment_cycle(ucn_cluster_t *cluster, uint32_t now_ms);
void send_backup_assignment_step(ucn_cluster_t *cluster, uint32_t now_ms);
void send_backup_heartbeat(ucn_cluster_t *cluster, uint32_t now_ms);
ucn_result_t send_backup_delta_step(ucn_cluster_t *cluster);
ucn_result_t send_backup_snapshot_step(ucn_cluster_t *cluster);

/* == Recovery module boundary (02-06, this OP) ==
 * ucn_cluster_recovery.c owns the RECOVERY_HEAD quorum / declaration /
 * arbitration / TTL lifecycle.  Exposed here (de-static only) for the
 * remaining ucn_cluster.c modules (receive dispatch, step). */
uint32_t compute_recovery_backoff(const ucn_cluster_t *cluster);
bool recovery_quorum_met(const ucn_cluster_t *cluster);
void start_recovery_backoff(ucn_cluster_t *cluster, uint32_t now_ms);
void declare_recovery_head(ucn_cluster_t *cluster,
                           const ucn_cluster_epoch_t *durable_epoch,
                           uint32_t now_ms);
void stepdown_recovery_head(ucn_cluster_t *cluster, uint32_t now_ms);
void send_recovery_declare(ucn_cluster_t *cluster);
ucn_result_t handle_recovery_declare(ucn_cluster_t *cluster,
                                     ucn_node_id_t source,
                                     const ucn_cluster_message_t *message,
                                     uint32_t now_ms);
ucn_result_t handle_recovery_ack(ucn_cluster_t *cluster,
                                 ucn_node_id_t source,
                                 const ucn_cluster_message_t *message,
                                 uint32_t now_ms);

/* == Merge / head-offer module boundary (02-06, this OP) ==
 * ucn_cluster_merge.c owns the candidate table, score comparison, the
 * ordered stepdown and the head-offer / score-switch logic.  Exposed here
 * (de-static only) for the remaining ucn_cluster.c modules (receive
 * dispatch, step) and the test hooks. */
ucn_cluster_candidate_t *allocate_candidate(
    ucn_cluster_t *cluster, ucn_node_id_t node_id, uint32_t now_ms);
ucn_result_t observe_candidate(ucn_cluster_t *cluster,
                               ucn_node_id_t source,
                               const ucn_cluster_message_t *message,
                               uint32_t now_ms);
bool candidate_better(uint16_t candidate_score, ucn_node_id_t candidate_node,
                      uint16_t current_score, ucn_node_id_t current_node);
void send_head_stepdown(ucn_cluster_t *cluster);
void begin_ordered_stepdown(ucn_cluster_t *cluster,
                            const ucn_cluster_candidate_t *candidate,
                            uint32_t now_ms);
ucn_result_t backup_challenge(ucn_cluster_t *cluster, uint32_t now_ms);

/* Helpers the merge module calls but which stay in ucn_cluster.c core
 * (same exposure rule: de-static only, bodies untouched). */
ucn_result_t cluster_transmit(
    ucn_cluster_t *cluster, ucn_node_id_t destination,
    const ucn_cluster_message_t *message,
    const uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES]);
void begin_join_prepare_fields(ucn_cluster_t *cluster,
                               const ucn_cluster_candidate_t *candidate,
                               uint32_t now_ms);
void begin_join(ucn_cluster_t *cluster,
                const ucn_cluster_candidate_t *candidate,
                uint32_t now_ms);

/* == Module layout complete (02-01..02-06) ==
 * ucn_cluster.c keeps the public facade / init / views / receive dispatch
 * / step / shared infra (config, token bucket, transmit).
 */

#ifdef __cplusplus
}
#endif

#endif /* UCN_CLUSTER_INTERNAL_H */
