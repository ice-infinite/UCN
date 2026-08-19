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

/* == Membership module boundary (02-04, this OP) ==
 * ucn_cluster_membership.c owns the member table (allocation / expiry /
 * query) and the Join / Keepalive / Leave handlers.  Exposed here
 * (de-static only) so the remaining ucn_cluster.c modules (receive
 * dispatch, step, backup) can call them. */
uint16_t member_count_u16(const ucn_cluster_t *cluster);
uint16_t available_capacity(const ucn_cluster_t *cluster);
ucn_cluster_member_t *find_member(ucn_cluster_t *cluster,
                                  ucn_node_id_t node_id);
ucn_cluster_member_t *allocate_member(ucn_cluster_t *cluster,
                                      ucn_node_id_t node_id);
void remove_member(ucn_cluster_t *cluster, ucn_node_id_t node_id,
                   uint32_t now_ms);
void clear_members(ucn_cluster_t *cluster);
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
void consider_head_offer(ucn_cluster_t *cluster,
                         ucn_cluster_candidate_t *candidate,
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
ucn_result_t handle_takeover_ack(ucn_cluster_t *cluster,
                                 ucn_node_id_t source,
                                 const ucn_cluster_message_t *message,
                                 uint32_t now_ms);
ucn_result_t handle_head_takeover(ucn_cluster_t *cluster,
                                  ucn_node_id_t source,
                                  const ucn_cluster_message_t *message,
                                  uint32_t now_ms);
void complete_takeover(ucn_cluster_t *cluster, uint32_t now_ms);
void start_takeover(ucn_cluster_t *cluster, uint32_t now_ms);
void start_backup_assignment_cycle(ucn_cluster_t *cluster, uint32_t now_ms);
void send_backup_assignment_step(ucn_cluster_t *cluster, uint32_t now_ms);
void send_backup_heartbeat(ucn_cluster_t *cluster, uint32_t now_ms);
void send_backup_delta_step(ucn_cluster_t *cluster);
void send_backup_snapshot_step(ucn_cluster_t *cluster);

/* == Module layout for the remaining OPs (append here) ==
 * 02-06  ucn_cluster_recovery.c   Recovery quorum / declaration /
 *                                 arbitration / TTL.
 *        ucn_cluster_merge.c      Head offer / stepdown / score switch.
 */

#ifdef __cplusplus
}
#endif

#endif /* UCN_CLUSTER_INTERNAL_H */
