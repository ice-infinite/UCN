#include "ucn/ucn_cluster_config_backup.h"

#include <string.h>

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool config_ref_equal(const ucn_cluster_persist_config_ref_t *left,
                             const ucn_cluster_persist_config_ref_t *right)
{
    return left != NULL && right != NULL && left->valid == right->valid &&
           (!left->valid ||
            (left->config_id == right->config_id &&
             left->generation == right->generation &&
             memcmp(left->digest, right->digest, sizeof(left->digest)) == 0));
}

static bool backup_member_is_eligible(const ucn_cluster_member_t *member)
{
    return ucn_cluster_member_record_is_valid(member) && member->occupied &&
           member->voting &&
           member->status ==
               (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED &&
           member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4 &&
           node_id_is_valid(member->node_id);
}

void ucn_cluster_config_backup_gate_init(
    ucn_cluster_config_backup_gate_t *gate,
    bool require_backup_for_config)
{
    if (gate == NULL) {
        return;
    }
    (void)memset(gate, 0, sizeof(*gate));
    gate->require_backup_for_config = require_backup_for_config;
}

ucn_result_t ucn_cluster_config_backup_gate_set_backup(
    ucn_cluster_config_backup_gate_t *gate,
    const ucn_cluster_member_t *backup_member)
{
    ucn_cluster_config_backup_gate_t candidate;

    if (gate == NULL || !backup_member_is_eligible(backup_member)) {
        return UCN_ERR_ARGUMENT;
    }
    if (gate->staged || gate->acknowledged) {
        return UCN_ERR_STATE;
    }
    candidate = *gate;
    candidate.backup_node_id = backup_member->node_id;
    candidate.backup_present = true;
    *gate = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_backup_gate_stage(
    ucn_cluster_config_backup_gate_t *gate,
    const ucn_cluster_config_tx_t *tx)
{
    ucn_cluster_config_backup_gate_t candidate;
    ucn_result_t result;

    if (gate == NULL || tx == NULL || !ucn_cluster_config_tx_is_active(tx)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!gate->backup_present) {
        return gate->require_backup_for_config ? UCN_ERR_STATE : UCN_OK;
    }
    if (gate->staged || gate->acknowledged) {
        return UCN_ERR_STATE;
    }
    candidate = *gate;
    result = ucn_cluster_config_persist_ref_from_joint_new(
        &tx->proposed_config, &candidate.staged_config);
    if (result != UCN_OK) {
        return result;
    }
    candidate.transaction_id = tx->transaction_id;
    candidate.staged = true;
    *gate = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_backup_gate_ack(
    ucn_cluster_config_backup_gate_t *gate,
    ucn_node_id_t source_node_id,
    uint32_t transaction_id,
    const ucn_cluster_persist_config_ref_t *staged_config)
{
    ucn_cluster_config_backup_gate_t candidate;

    if (gate == NULL || staged_config == NULL || !gate->backup_present ||
        !gate->staged || gate->acknowledged ||
        source_node_id != gate->backup_node_id ||
        transaction_id != gate->transaction_id ||
        !config_ref_equal(staged_config, &gate->staged_config)) {
        return UCN_ERR_ARGUMENT;
    }
    candidate = *gate;
    candidate.acknowledged_backup_node_id = source_node_id;
    candidate.acknowledged = true;
    *gate = candidate;
    return UCN_OK;
}

bool ucn_cluster_config_backup_gate_commit_allowed(
    const ucn_cluster_config_backup_gate_t *gate,
    bool *ha_ready)
{
    if (ha_ready != NULL) {
        *ha_ready = false;
    }
    if (gate == NULL) {
        return false;
    }
    if (!gate->backup_present) {
        return !gate->require_backup_for_config;
    }
    if (!gate->staged || !gate->acknowledged) {
        return false;
    }
    if (ha_ready != NULL) {
        *ha_ready = true;
    }
    return true;
}

bool ucn_cluster_config_backup_gate_commit_allowed_for_tx(
    const ucn_cluster_config_backup_gate_t *gate,
    const ucn_cluster_config_tx_t *tx,
    bool *ha_ready)
{
    ucn_cluster_persist_config_ref_t expected;

    if (ha_ready != NULL) {
        *ha_ready = false;
    }
    if (gate == NULL || tx == NULL || !ucn_cluster_config_tx_is_active(tx)) {
        return false;
    }
    if (!gate->backup_present) {
        return !gate->require_backup_for_config;
    }
    if (!node_id_is_valid(gate->backup_node_id) || !gate->staged ||
        !gate->acknowledged ||
        gate->acknowledged_backup_node_id != gate->backup_node_id ||
        gate->transaction_id != tx->transaction_id ||
        ucn_cluster_config_persist_ref_from_joint_new(&tx->proposed_config,
                                                      &expected) != UCN_OK ||
        !config_ref_equal(&gate->staged_config, &expected)) {
        return false;
    }
    if (ha_ready != NULL) {
        *ha_ready = true;
    }
    return true;
}
