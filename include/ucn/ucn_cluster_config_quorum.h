#ifndef UCN_CLUSTER_CONFIG_QUORUM_H
#define UCN_CLUSTER_CONFIG_QUORUM_H

/* CLV2-07-05: pure Joint Config quorum helpers.
 *
 * These helpers inspect only canonical voter sets and local ACK bitmaps.
 * They do not grant Head authority, emit CONFIG_COMMIT, or interpret a
 * network certificate; M08/M10 own those decisions.
 */

#include "ucn/ucn_cluster_config_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ucn_cluster_config_bitmap_reaches_quorum(
    const ucn_cluster_voter_set_t *set,
    uint64_t bitmap);

/* Requires one valid active transaction. Both C_old and C_new quorum must
 * independently pass; no single bitmap can substitute for the other. */
bool ucn_cluster_config_joint_quorum_reached(
    const ucn_cluster_config_tx_t *tx);

#ifdef __cplusplus
}
#endif

#endif
