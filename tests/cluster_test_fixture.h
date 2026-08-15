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

#include "ucn/ucn_cluster.h"

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

#ifdef __cplusplus
}
#endif

#endif