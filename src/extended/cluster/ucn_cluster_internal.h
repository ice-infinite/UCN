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

#endif /* UCN_CLUSTER_INTERNAL_H */
