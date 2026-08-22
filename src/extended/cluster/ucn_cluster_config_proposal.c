#include "ucn/ucn_cluster_config_proposal.h"

#include <string.h>

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool member_is_v4_provisional(const ucn_cluster_member_t *member)
{
    return ucn_cluster_member_record_is_valid(member) && member->occupied &&
           !member->voting &&
           member->status ==
               (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL &&
           member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
}

static bool member_is_v4_removing_voter(const ucn_cluster_member_t *member)
{
    return ucn_cluster_member_record_is_valid(member) && member->occupied &&
           member->voting &&
           member->status ==
               (uint8_t)UCN_CLUSTER_MEMBER_STATUS_REMOVING &&
           member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
}

ucn_result_t ucn_cluster_config_tx_begin_add_provisional(
    ucn_cluster_config_tx_t *tx,
    uint32_t transaction_id,
    ucn_node_id_t head_node_id,
    const ucn_cluster_member_t *provisional_member,
    const ucn_cluster_config_state_t *stable_old,
    uint8_t voter_capacity,
    uint32_t deadline_ms)
{
    ucn_cluster_config_tx_t candidate;
    ucn_cluster_config_state_t joint;
    ucn_node_id_t new_node_ids[UCN_CLUSTER_MAX_VOTERS];
    size_t index;
    size_t new_count;
    ucn_result_t result;

    if (tx == NULL || provisional_member == NULL || stable_old == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_config_tx_is_valid(tx) ||
        tx->phase != (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_IDLE) {
        return UCN_ERR_STATE;
    }
    if (!node_id_is_valid(head_node_id) ||
        !member_is_v4_provisional(provisional_member) ||
        !ucn_cluster_config_state_is_valid(stable_old) ||
        stable_old->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        !ucn_cluster_voter_set_contains(&stable_old->old_set, head_node_id) ||
        ucn_cluster_voter_set_contains(&stable_old->old_set,
                                       provisional_member->node_id) ||
        voter_capacity == 0U || voter_capacity > UCN_CLUSTER_MAX_VOTERS ||
        stable_old->old_set.count >= voter_capacity ||
        stable_old->old_set.count >= UCN_CLUSTER_MAX_VOTERS) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_cluster_config_state_rekey_required(stable_old)) {
        return UCN_ERR_EXHAUSTED;
    }
    (void)memset(new_node_ids, 0, sizeof(new_node_ids));
    for (index = 0U; index < (size_t)stable_old->old_set.count; ++index) {
        new_node_ids[index] = stable_old->old_set.node_ids[index];
    }
    new_count = (size_t)stable_old->old_set.count;
    new_node_ids[new_count++] = provisional_member->node_id;
    if (!ucn_cluster_config_state_init_joint(&joint, stable_old, new_node_ids,
                                             new_count)) {
        return UCN_ERR_CONFIG;
    }

    candidate = *tx;
    result = ucn_cluster_config_tx_begin(
        &candidate, transaction_id, UCN_CLUSTER_CONFIG_PROPOSAL_ADD,
        provisional_member->node_id, stable_old, &joint, deadline_ms);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_cluster_config_tx_record_ack(&candidate, head_node_id);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    *tx = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_member_mark_removing(
    ucn_cluster_member_t *member)
{
    ucn_cluster_member_t candidate;

    if (member == NULL || !ucn_cluster_member_record_is_valid(member) ||
        !member->occupied || !member->voting ||
        member->status != (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED ||
        member->wire_version != UCN_CLUSTER_MEMBER_WIRE_VERSION_V4) {
        return UCN_ERR_ARGUMENT;
    }
    candidate = *member;
    candidate.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_REMOVING;
    if (!ucn_cluster_member_record_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *member = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_tx_begin_remove_marked(
    ucn_cluster_config_tx_t *tx,
    uint32_t transaction_id,
    ucn_node_id_t head_node_id,
    const ucn_cluster_member_t *removing_member,
    const ucn_cluster_config_state_t *stable_old,
    uint32_t deadline_ms)
{
    ucn_cluster_config_tx_t candidate;
    ucn_cluster_config_state_t joint;
    ucn_node_id_t new_node_ids[UCN_CLUSTER_MAX_VOTERS];
    size_t index;
    size_t new_count = 0U;
    ucn_result_t result;

    if (tx == NULL || removing_member == NULL || stable_old == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_config_tx_is_valid(tx) ||
        tx->phase != (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_IDLE) {
        return UCN_ERR_STATE;
    }
    if (!node_id_is_valid(head_node_id) ||
        !member_is_v4_removing_voter(removing_member) ||
        !ucn_cluster_config_state_is_valid(stable_old) ||
        stable_old->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        !ucn_cluster_voter_set_contains(&stable_old->old_set, head_node_id) ||
        !ucn_cluster_voter_set_contains(&stable_old->old_set,
                                        removing_member->node_id) ||
        removing_member->node_id == head_node_id ||
        stable_old->old_set.count <= 1U) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_cluster_config_state_rekey_required(stable_old)) {
        return UCN_ERR_EXHAUSTED;
    }
    (void)memset(new_node_ids, 0, sizeof(new_node_ids));
    for (index = 0U; index < (size_t)stable_old->old_set.count; ++index) {
        ucn_node_id_t node_id = stable_old->old_set.node_ids[index];

        if (node_id != removing_member->node_id) {
            new_node_ids[new_count++] = node_id;
        }
    }
    if (!ucn_cluster_config_state_init_joint(&joint, stable_old, new_node_ids,
                                             new_count)) {
        return UCN_ERR_CONFIG;
    }
    candidate = *tx;
    result = ucn_cluster_config_tx_begin(
        &candidate, transaction_id, UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE,
        removing_member->node_id, stable_old, &joint, deadline_ms);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_cluster_config_tx_record_ack(&candidate, head_node_id);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    *tx = candidate;
    return UCN_OK;
}
