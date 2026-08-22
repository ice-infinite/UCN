#ifndef UCN_CLUSTER_CONFIG_BACKUP_H
#define UCN_CLUSTER_CONFIG_BACKUP_H

/* CLV2-07-07: Config HA-backup staging gate.
 *
 * This value model does not assign a production Backup, mirror member tables,
 * or allow Takeover. It makes the Config owner's eligibility explicit:
 * absent Backup may be allowed only as non-HA, while a configured Backup
 * needs an exact C_new staging acknowledgement before Commit can proceed.
 */

#include "ucn/ucn_cluster_config_persistence.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_cluster_config_backup_gate {
    ucn_node_id_t backup_node_id;
    /* Identity that produced the accepted ACK.  It is stored separately from
     * backup_node_id so a stale acknowledgement cannot authorize Commit after
     * the configured Backup changes. */
    ucn_node_id_t acknowledged_backup_node_id;
    uint32_t transaction_id;
    ucn_cluster_persist_config_ref_t staged_config;
    bool backup_present;
    bool require_backup_for_config;
    bool staged;
    bool acknowledged;
} ucn_cluster_config_backup_gate_t;

void ucn_cluster_config_backup_gate_init(
    ucn_cluster_config_backup_gate_t *gate,
    bool require_backup_for_config);

/* Binds an already selected v4 committed voter as the only Backup identity
 * relevant to this Config transaction. It does not alter runtime backup FSM. */
ucn_result_t ucn_cluster_config_backup_gate_set_backup(
    ucn_cluster_config_backup_gate_t *gate,
    const ucn_cluster_member_t *backup_member);

/* Copies exact planned C_new identity into the staging gate. */
ucn_result_t ucn_cluster_config_backup_gate_stage(
    ucn_cluster_config_backup_gate_t *gate,
    const ucn_cluster_config_tx_t *tx);

/* Accepts staging acknowledgement only from the bound Backup and exact tx. */
ucn_result_t ucn_cluster_config_backup_gate_ack(
    ucn_cluster_config_backup_gate_t *gate,
    ucn_node_id_t source_node_id,
    uint32_t transaction_id,
    const ucn_cluster_persist_config_ref_t *staged_config);

/* Returns only the generic gate state for diagnostics and unit tests.  It is
 * not a Commit authorization because it has no transaction context. */
bool ucn_cluster_config_backup_gate_commit_allowed(
    const ucn_cluster_config_backup_gate_t *gate,
    bool *ha_ready);

/* Exact Commit admission.  When HA is present, the staged C_new, transaction
 * id and Backup that ACKed it must all match tx.  Production-like Config
 * Commit paths must use this function rather than the context-free helper. */
bool ucn_cluster_config_backup_gate_commit_allowed_for_tx(
    const ucn_cluster_config_backup_gate_t *gate,
    const ucn_cluster_config_tx_t *tx,
    bool *ha_ready);

#ifdef __cplusplus
}
#endif

#endif
