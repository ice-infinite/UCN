#include "ucn/ucn_cluster_config_tx.h"
#include "ucn/ucn_time.h"

#include <string.h>

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool voter_sets_equal(const ucn_cluster_voter_set_t *left,
                             const ucn_cluster_voter_set_t *right)
{
    return left != NULL && right != NULL &&
           left->config_id == right->config_id && left->hash == right->hash &&
           left->count == right->count &&
           memcmp(left->node_ids, right->node_ids, sizeof(left->node_ids)) ==
               0;
}

static bool proposal_matches_config_delta(
    ucn_cluster_config_proposal_kind_t proposal_kind,
    ucn_node_id_t proposal_node_id,
    const ucn_cluster_config_state_t *base_config,
    const ucn_cluster_config_state_t *proposed_config)
{
    size_t index;

    if (base_config == NULL || proposed_config == NULL) {
        return false;
    }
    if (proposal_kind == UCN_CLUSTER_CONFIG_PROPOSAL_ADD) {
        if (proposed_config->new_set.count !=
                (uint8_t)(base_config->old_set.count + 1U) ||
            ucn_cluster_voter_set_contains(&base_config->old_set,
                                           proposal_node_id) ||
            !ucn_cluster_voter_set_contains(&proposed_config->new_set,
                                            proposal_node_id)) {
            return false;
        }
        for (index = 0U; index < (size_t)base_config->old_set.count; ++index) {
            if (!ucn_cluster_voter_set_contains(
                    &proposed_config->new_set,
                    base_config->old_set.node_ids[index])) {
                return false;
            }
        }
        return true;
    }
    if (proposal_kind == UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE) {
        if (base_config->old_set.count <= 1U ||
            proposed_config->new_set.count !=
                (uint8_t)(base_config->old_set.count - 1U) ||
            !ucn_cluster_voter_set_contains(&base_config->old_set,
                                            proposal_node_id) ||
            ucn_cluster_voter_set_contains(&proposed_config->new_set,
                                           proposal_node_id)) {
            return false;
        }
        for (index = 0U; index < (size_t)proposed_config->new_set.count;
             ++index) {
            if (!ucn_cluster_voter_set_contains(
                    &base_config->old_set,
                    proposed_config->new_set.node_ids[index])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static uint64_t bitmap_mask_for_count(uint8_t count)
{
    return count >= 64U ? UINT64_MAX : ((UINT64_C(1) << count) - UINT64_C(1));
}

static bool tx_is_canonical_idle(const ucn_cluster_config_tx_t *tx)
{
    ucn_cluster_config_tx_t empty;

    if (tx == NULL) {
        return false;
    }
    ucn_cluster_config_tx_init_empty(&empty);
    return memcmp(tx, &empty, sizeof(empty)) == 0;
}

void ucn_cluster_config_tx_init_empty(ucn_cluster_config_tx_t *tx)
{
    if (tx == NULL) {
        return;
    }
    (void)memset(tx, 0, sizeof(*tx));
    tx->phase = (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_IDLE;
    tx->persist_stage = (uint8_t)UCN_CLUSTER_CONFIG_PERSIST_STAGE_NONE;
}

bool ucn_cluster_config_proposal_kind_is_valid(
    ucn_cluster_config_proposal_kind_t kind)
{
    return kind == UCN_CLUSTER_CONFIG_PROPOSAL_ADD ||
           kind == UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE;
}

bool ucn_cluster_config_tx_phase_is_valid(ucn_cluster_config_tx_phase_t phase)
{
    return phase == UCN_CLUSTER_CONFIG_TX_PHASE_IDLE ||
           phase == UCN_CLUSTER_CONFIG_TX_PHASE_PROPOSING ||
           phase == UCN_CLUSTER_CONFIG_TX_PHASE_PREPARED ||
           phase == UCN_CLUSTER_CONFIG_TX_PHASE_JOINT ||
           phase == UCN_CLUSTER_CONFIG_TX_PHASE_COMMITTED ||
           phase == UCN_CLUSTER_CONFIG_TX_PHASE_ABORTED;
}

bool ucn_cluster_config_persist_stage_is_valid(
    ucn_cluster_config_persist_stage_t stage)
{
    return stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_NONE ||
           stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_PREPARE_PENDING ||
           stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_PREPARED ||
           stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_JOINT_PENDING ||
           stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_JOINT_DURABLE ||
           stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_COMMIT_PENDING ||
           stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_COMMITTED ||
           stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_ABORT_PENDING ||
           stage == UCN_CLUSTER_CONFIG_PERSIST_STAGE_ABORTED;
}

bool ucn_cluster_config_tx_is_valid(const ucn_cluster_config_tx_t *tx)
{
    ucn_cluster_config_tx_phase_t phase;

    if (tx == NULL) {
        return false;
    }
    phase = (ucn_cluster_config_tx_phase_t)tx->phase;
    if (phase == UCN_CLUSTER_CONFIG_TX_PHASE_IDLE) {
        return tx_is_canonical_idle(tx);
    }
    if (!ucn_cluster_config_tx_phase_is_valid(phase) ||
        !ucn_cluster_config_proposal_kind_is_valid(
            (ucn_cluster_config_proposal_kind_t)tx->proposal_kind) ||
        !ucn_cluster_config_persist_stage_is_valid(
            (ucn_cluster_config_persist_stage_t)tx->persist_stage) ||
        !serial_is_valid(tx->transaction_id) ||
        !node_id_is_valid(tx->proposal_node_id) || tx->deadline_ms == 0U ||
        !ucn_cluster_config_state_is_valid(&tx->base_config) ||
        tx->base_config.phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        !ucn_cluster_config_state_is_valid(&tx->proposed_config) ||
        tx->proposed_config.phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT ||
        !voter_sets_equal(&tx->base_config.old_set,
                          &tx->proposed_config.old_set) ||
        !proposal_matches_config_delta(
            (ucn_cluster_config_proposal_kind_t)tx->proposal_kind,
            tx->proposal_node_id, &tx->base_config, &tx->proposed_config) ||
        (tx->old_ack_bitmap &
         ~bitmap_mask_for_count(tx->proposed_config.old_set.count)) != 0U ||
        (tx->new_ack_bitmap &
         ~bitmap_mask_for_count(tx->proposed_config.new_set.count)) != 0U) {
        return false;
    }
    return true;
}

bool ucn_cluster_config_tx_is_active(const ucn_cluster_config_tx_t *tx)
{
    return tx != NULL && ucn_cluster_config_tx_is_valid(tx) &&
           tx->phase != (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_IDLE &&
           tx->phase != (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_COMMITTED &&
           tx->phase != (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_ABORTED;
}

ucn_result_t ucn_cluster_config_tx_begin(
    ucn_cluster_config_tx_t *tx,
    uint32_t transaction_id,
    ucn_cluster_config_proposal_kind_t proposal_kind,
    ucn_node_id_t proposal_node_id,
    const ucn_cluster_config_state_t *base_config,
    const ucn_cluster_config_state_t *proposed_joint,
    uint32_t deadline_ms)
{
    ucn_cluster_config_tx_t candidate;

    if (tx == NULL || base_config == NULL || proposed_joint == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_config_tx_is_valid(tx) ||
        tx->phase != (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_IDLE) {
        return UCN_ERR_STATE;
    }
    if (!serial_is_valid(transaction_id) ||
        !ucn_cluster_config_proposal_kind_is_valid(proposal_kind) ||
        !node_id_is_valid(proposal_node_id) || deadline_ms == 0U ||
        !ucn_cluster_config_state_is_valid(base_config) ||
        base_config->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        !ucn_cluster_config_state_is_valid(proposed_joint) ||
        proposed_joint->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT ||
        !voter_sets_equal(&base_config->old_set, &proposed_joint->old_set) ||
        !proposal_matches_config_delta(proposal_kind, proposal_node_id,
                                       base_config, proposed_joint)) {
        return UCN_ERR_ARGUMENT;
    }
    ucn_cluster_config_tx_init_empty(&candidate);
    candidate.transaction_id = transaction_id;
    candidate.proposal_node_id = proposal_node_id;
    candidate.deadline_ms = deadline_ms;
    candidate.phase = (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_PROPOSING;
    candidate.proposal_kind = (uint8_t)proposal_kind;
    candidate.base_config = *base_config;
    candidate.proposed_config = *proposed_joint;
    if (!ucn_cluster_config_tx_is_valid(&candidate)) {
        return UCN_ERR_CONFIG;
    }
    *tx = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_tx_record_ack(ucn_cluster_config_tx_t *tx,
                                              ucn_node_id_t voter_node_id)
{
    ucn_cluster_config_tx_t candidate;
    uint64_t bitmap;

    if (tx == NULL || !node_id_is_valid(voter_node_id)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_config_tx_is_active(tx)) {
        return UCN_ERR_STATE;
    }
    candidate = *tx;
    if (ucn_cluster_voter_set_bitmap_for_node(&candidate.proposed_config.old_set,
                                              voter_node_id, &bitmap)) {
        candidate.old_ack_bitmap |= bitmap;
    }
    if (ucn_cluster_voter_set_bitmap_for_node(&candidate.proposed_config.new_set,
                                              voter_node_id, &bitmap)) {
        candidate.new_ack_bitmap |= bitmap;
    }
    if (candidate.old_ack_bitmap == tx->old_ack_bitmap &&
        candidate.new_ack_bitmap == tx->new_ack_bitmap) {
        return UCN_ERR_NOT_FOUND;
    }
    if (!ucn_cluster_config_tx_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *tx = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_tx_schedule_retry(
    ucn_cluster_config_tx_t *tx,
    uint32_t retry_due_ms)
{
    ucn_cluster_config_tx_t candidate;

    if (tx == NULL || retry_due_ms == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_config_tx_is_active(tx)) {
        return UCN_ERR_STATE;
    }
    if (tx->retry_count == UINT8_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    candidate = *tx;
    candidate.retry_due_ms = retry_due_ms;
    candidate.retry_count++;
    *tx = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_tx_set_persist_stage(
    ucn_cluster_config_tx_t *tx,
    ucn_cluster_config_persist_stage_t stage)
{
    ucn_cluster_config_tx_t candidate;

    if (tx == NULL || !ucn_cluster_config_persist_stage_is_valid(stage)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_config_tx_is_active(tx)) {
        return UCN_ERR_STATE;
    }
    candidate = *tx;
    candidate.persist_stage = (uint8_t)stage;
    if (!ucn_cluster_config_tx_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *tx = candidate;
    return UCN_OK;
}

bool ucn_cluster_config_tx_is_expired(const ucn_cluster_config_tx_t *tx,
                                      uint32_t now_ms)
{
    return ucn_cluster_config_tx_is_active(tx) &&
           ucn_deadline_expired(now_ms, tx->deadline_ms);
}
