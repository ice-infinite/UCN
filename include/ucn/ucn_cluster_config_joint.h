#ifndef UCN_CLUSTER_CONFIG_JOINT_H
#define UCN_CLUSTER_CONFIG_JOINT_H

/* CLV2-07-08: test/experiment Joint Config runtime gate.
 *
 * A Joint runtime may be entered only after the M04 record holds both the
 * current committed C_old ref and exact PREPARED C_new ref/transaction ID.
 * It is a value model only: no production voter set, Authority, Takeover,
 * RX/TX or Backup mirror is modified here.
 */

#include "ucn/ucn_cluster_config_backup.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_cluster_config_joint_runtime {
    ucn_cluster_config_state_t active_config;
    ucn_cluster_config_tx_t transaction;
    bool joint_active;
} ucn_cluster_config_joint_runtime_t;

ucn_result_t ucn_cluster_config_joint_runtime_init(
    ucn_cluster_config_joint_runtime_t *runtime,
    const ucn_cluster_config_state_t *stable_config);

/* Requires dual quorum plus explicit durable CONFIG_JOINT proof for the same
 * PREPARED C_new identity; leaves output unchanged if either proof is
 * missing. */
ucn_result_t ucn_cluster_config_joint_runtime_enter(
    ucn_cluster_config_joint_runtime_t *runtime,
    const ucn_cluster_config_persist_owner_t *persistence_owner,
    const ucn_cluster_config_tx_t *transaction);

bool ucn_cluster_config_joint_runtime_is_valid(
    const ucn_cluster_config_joint_runtime_t *runtime);

/* Applies the already durable CONFIG_COMMIT to caller-owned member storage.
 * The Backup staging gate must explicitly allow the commit. */
ucn_result_t ucn_cluster_config_joint_runtime_commit(
    ucn_cluster_config_joint_runtime_t *runtime,
    const ucn_cluster_config_persist_owner_t *persistence_owner,
    const ucn_cluster_config_backup_gate_t *backup_gate,
    ucn_cluster_member_t *proposal_member);

/* Restores C_old only after the timeout Abort was proven durable. ADD stays
 * provisional; REMOVE returns from REMOVING to COMMITTED/voting. */
ucn_result_t ucn_cluster_config_joint_runtime_abort(
    ucn_cluster_config_joint_runtime_t *runtime,
    const ucn_cluster_config_persist_owner_t *persistence_owner,
    ucn_cluster_member_t *proposal_member);

#ifdef __cplusplus
}
#endif

#endif
