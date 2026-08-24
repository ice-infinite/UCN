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
  switch (role) {
  case UCN_CLUSTER_ROLE_DISABLED:
    cluster->phase = UCN_CLUSTER_PHASE_DISABLED;
    break;
  case UCN_CLUSTER_ROLE_DETACHED:
    cluster->phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    break;
  case UCN_CLUSTER_ROLE_CANDIDATE:
    cluster->phase = UCN_CLUSTER_PHASE_ELECTION;
    break;
  case UCN_CLUSTER_ROLE_JOIN_PENDING:
    cluster->phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    break;
  case UCN_CLUSTER_ROLE_MEMBER:
    cluster->phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    break;
  case UCN_CLUSTER_ROLE_HEAD:
    cluster->phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    break;
  case UCN_CLUSTER_ROLE_BACKUP:
    cluster->phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    break;
  case UCN_CLUSTER_ROLE_STEPPING_DOWN:
    cluster->phase = UCN_CLUSTER_PHASE_STEPPING_DOWN;
    break;
  case UCN_CLUSTER_ROLE_RECOVERY_HEAD:
    cluster->phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    break;
  case UCN_CLUSTER_ROLE_TERM_CONFLICT:
    cluster->phase = UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT;
    break;
  default:
    cluster->phase = UCN_CLUSTER_PHASE_DISABLED;
    break;
  }
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
  if (ready) {
    cluster->phase = UCN_CLUSTER_PHASE_BACKUP_READY;
  } else if (syncing) {
    cluster->phase = UCN_CLUSTER_PHASE_BACKUP_SYNCING;
  }
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
