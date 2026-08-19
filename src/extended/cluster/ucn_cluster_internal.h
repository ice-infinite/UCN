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

/* == Module layout for the remaining OPs (append here) ==
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

#ifdef __cplusplus
}
#endif

#endif /* UCN_CLUSTER_INTERNAL_H */
