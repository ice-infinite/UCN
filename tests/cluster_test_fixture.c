/* CLV2-M00-04: fixture implementation (test-only). */
#include "cluster_test_fixture.h"

void cluster_fixture_set_epoch(ucn_cluster_t *cluster,
                              uint32_t cluster_id,
                              uint32_t term,
                              ucn_node_id_t head_node_id) {
  if (cluster == NULL) {
    return;
  }
  cluster->cluster_id = cluster_id;
  cluster->term = term;
  cluster->head_node_id = head_node_id;
}

void cluster_fixture_set_role(ucn_cluster_t *cluster,
                             ucn_cluster_role_t role) {
  if (cluster == NULL) {
    return;
  }
  cluster->role = role;
  cluster->role_since_ms = 0U;
}

void cluster_fixture_set_backup(ucn_cluster_t *cluster,
                               ucn_node_id_t backup_primary,
                               uint32_t generation,
                               bool syncing,
                               bool ready) {
  if (cluster == NULL) {
    return;
  }
  cluster->backup_primary_node_id = backup_primary;
  cluster->backup_generation = generation;
  cluster->backup_syncing = syncing;
  cluster->backup_ready = ready;
}

void cluster_fixture_set_vote(ucn_cluster_t *cluster,
                              uint32_t cluster_id,
                              uint32_t term,
                              uint32_t generation) {
  if (cluster == NULL) {
    return;
  }
  cluster->member_voted_cluster_id = cluster_id;
  cluster->member_voted_term = term;
  cluster->member_voted_generation = generation;
}