#include "ucn/ucn_cluster_config_joint.h"

#include <string.h>

static bool voter_sets_equal(const ucn_cluster_voter_set_t *left,
                             const ucn_cluster_voter_set_t *right)
{
    return left != NULL && right != NULL &&
           left->config_id == right->config_id && left->hash == right->hash &&
           left->count == right->count &&
           memcmp(left->node_ids, right->node_ids, sizeof(left->node_ids)) ==
               0;
}

static bool config_states_equal(const ucn_cluster_config_state_t *left,
                                const ucn_cluster_config_state_t *right)
{
    return left != NULL && right != NULL &&
           left->config_id == right->config_id &&
           left->old_set_hash == right->old_set_hash &&
           left->new_set_hash == right->new_set_hash &&
           left->phase == right->phase &&
           voter_sets_equal(&left->old_set, &right->old_set) &&
           voter_sets_equal(&left->new_set, &right->new_set);
}

static bool config_refs_equal(const ucn_cluster_persist_config_ref_t *left,
                              const ucn_cluster_persist_config_ref_t *right)
{
    return left != NULL && right != NULL && left->valid == right->valid &&
           (!left->valid ||
            (left->config_id == right->config_id &&
             left->generation == right->generation &&
             memcmp(left->digest, right->digest, sizeof(left->digest)) == 0));
}

static bool member_matches_add_proposal(const ucn_cluster_member_t *member,
                                       const ucn_cluster_config_tx_t *tx)
{
    return ucn_cluster_member_record_is_valid(member) && member->occupied &&
           !member->voting &&
           member->status ==
               (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL &&
           member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4 &&
           member->node_id == tx->proposal_node_id;
}

static bool member_matches_remove_proposal(const ucn_cluster_member_t *member,
                                          const ucn_cluster_config_tx_t *tx)
{
    return ucn_cluster_member_record_is_valid(member) && member->occupied &&
           member->voting &&
           member->status ==
               (uint8_t)UCN_CLUSTER_MEMBER_STATUS_REMOVING &&
           member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4 &&
           member->node_id == tx->proposal_node_id;
}

static bool member_matches_committed_add(const ucn_cluster_member_t *member,
                                         const ucn_cluster_config_tx_t *tx)
{
    return ucn_cluster_member_record_is_valid(member) && member->occupied &&
           member->voting &&
           member->status == (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED &&
           member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4 &&
           member->node_id == tx->proposal_node_id &&
           !member->provisional_deadline_armed &&
           member->provisional_deadline_ms == 0U;
}

static bool member_is_canonical_empty(const ucn_cluster_member_t *member)
{
    ucn_cluster_member_t empty;

    if (member == NULL) {
        return false;
    }
    (void)memset(&empty, 0, sizeof(empty));
    return memcmp(member, &empty, sizeof(empty)) == 0;
}

static bool member_matches_aborted_remove(const ucn_cluster_member_t *member,
                                          const ucn_cluster_config_tx_t *tx)
{
    return ucn_cluster_member_record_is_valid(member) && member->occupied &&
           member->voting &&
           member->status == (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED &&
           member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4 &&
           member->node_id == tx->proposal_node_id;
}

bool ucn_cluster_config_joint_runtime_is_valid(
    const ucn_cluster_config_joint_runtime_t *runtime)
{
    if (runtime == NULL || !ucn_cluster_config_state_is_valid(
                               &runtime->active_config)) {
        return false;
    }
    if (!runtime->joint_active) {
        if (runtime->active_config.phase !=
                (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
            !ucn_cluster_config_tx_is_valid(&runtime->transaction)) {
            return false;
        }
        if (runtime->transaction.phase ==
            (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_IDLE) {
            return true;
        }
        if (runtime->transaction.phase ==
            (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_ABORTED) {
            return config_states_equal(&runtime->active_config,
                                       &runtime->transaction.base_config);
        }
        if (runtime->transaction.phase ==
            (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_COMMITTED) {
            ucn_cluster_config_state_t promoted;

            return ucn_cluster_config_state_promote_joint(
                       &promoted, &runtime->transaction.proposed_config) &&
                   config_states_equal(&runtime->active_config, &promoted);
        }
        return false;
    }
    return runtime->active_config.phase ==
               (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT &&
           runtime->transaction.phase ==
               (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_JOINT &&
           ucn_cluster_config_tx_is_valid(&runtime->transaction) &&
           config_states_equal(&runtime->active_config,
                               &runtime->transaction.proposed_config);
}

ucn_result_t ucn_cluster_config_joint_runtime_init(
    ucn_cluster_config_joint_runtime_t *runtime,
    const ucn_cluster_config_state_t *stable_config)
{
    ucn_cluster_config_joint_runtime_t candidate;

    if (runtime == NULL || stable_config == NULL ||
        !ucn_cluster_config_state_is_valid(stable_config) ||
        stable_config->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.active_config = *stable_config;
    ucn_cluster_config_tx_init_empty(&candidate.transaction);
    if (!ucn_cluster_config_joint_runtime_is_valid(&candidate)) {
        return UCN_ERR_CONFIG;
    }
    *runtime = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_joint_runtime_commit(
    ucn_cluster_config_joint_runtime_t *runtime,
    const ucn_cluster_config_persist_owner_t *persistence_owner,
    const ucn_cluster_config_backup_gate_t *backup_gate,
    ucn_cluster_member_t *proposal_member)
{
    const ucn_cluster_persist_state_t *durable_state;
    ucn_cluster_config_joint_runtime_t candidate;
    ucn_cluster_member_t member_candidate;
    ucn_cluster_config_state_t stable;
    ucn_cluster_persist_config_ref_t expected;
    bool ha_ready;

    if (runtime == NULL || persistence_owner == NULL || backup_gate == NULL ||
        proposal_member == NULL ||
        !ucn_cluster_config_joint_runtime_is_valid(runtime)) {
        return UCN_ERR_ARGUMENT;
    }
    durable_state = ucn_cluster_config_persist_owner_state(persistence_owner);
    if (durable_state == NULL) {
        return UCN_ERR_STATE;
    }
    /* Commit application is idempotent only after the exact durable Commit
     * journal is visible.  The second call must not need a newly available
     * Backup gate or mutate Runtime/member storage again. */
    if (!runtime->joint_active) {
        if (runtime->transaction.phase !=
                (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_COMMITTED ||
            ucn_cluster_config_persist_ref_from_state(
                &runtime->active_config, &expected) != UCN_OK ||
            durable_state->config_transaction.phase !=
                UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED ||
            durable_state->config_transaction.transaction_id !=
                runtime->transaction.transaction_id ||
            durable_state->last_completed_operation !=
                UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT ||
            !config_refs_equal(&durable_state->committed_config, &expected)) {
            return UCN_ERR_STATE;
        }
        if (runtime->transaction.proposal_kind ==
            (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_ADD) {
            return member_matches_committed_add(proposal_member,
                                                &runtime->transaction) ?
                       UCN_OK :
                       UCN_ERR_ARGUMENT;
        }
        if (runtime->transaction.proposal_kind ==
            (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE) {
            return member_is_canonical_empty(proposal_member) ? UCN_OK :
                                                             UCN_ERR_ARGUMENT;
        }
        return UCN_ERR_STATE;
    }
    if (!ucn_cluster_config_backup_gate_commit_allowed_for_tx(
            backup_gate, &runtime->transaction, &ha_ready) ||
        ucn_cluster_config_persist_ref_from_joint_new(&runtime->active_config,
                                                      &expected) != UCN_OK ||
        !ucn_cluster_config_state_promote_joint(&stable,
                                                 &runtime->active_config)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)ha_ready;
    if (
        durable_state->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED ||
        durable_state->config_transaction.transaction_id !=
            runtime->transaction.transaction_id ||
        !config_refs_equal(&durable_state->committed_config, &expected)) {
        return UCN_ERR_STATE;
    }
    member_candidate = *proposal_member;
    if (runtime->transaction.proposal_kind ==
        (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_ADD) {
        if (!member_matches_add_proposal(proposal_member,
                                         &runtime->transaction)) {
            return UCN_ERR_ARGUMENT;
        }
        member_candidate.status =
            (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
        member_candidate.voting = true;
        member_candidate.provisional_deadline_armed = false;
        member_candidate.provisional_deadline_ms = 0U;
    } else if (runtime->transaction.proposal_kind ==
               (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE) {
        if (!member_matches_remove_proposal(proposal_member,
                                            &runtime->transaction)) {
            return UCN_ERR_ARGUMENT;
        }
        (void)memset(&member_candidate, 0, sizeof(member_candidate));
    } else {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_member_record_is_valid(&member_candidate)) {
        return UCN_ERR_STATE;
    }
    candidate = *runtime;
    candidate.active_config = stable;
    candidate.transaction.phase = (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_COMMITTED;
    candidate.transaction.persist_stage =
        (uint8_t)UCN_CLUSTER_CONFIG_PERSIST_STAGE_COMMITTED;
    candidate.joint_active = false;
    if (!ucn_cluster_config_joint_runtime_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *proposal_member = member_candidate;
    *runtime = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_joint_runtime_abort(
    ucn_cluster_config_joint_runtime_t *runtime,
    const ucn_cluster_config_persist_owner_t *persistence_owner,
    ucn_cluster_member_t *proposal_member)
{
    const ucn_cluster_persist_state_t *durable_state;
    ucn_cluster_config_joint_runtime_t candidate;
    ucn_cluster_member_t member_candidate;
    ucn_cluster_persist_config_ref_t base_ref;

    if (runtime == NULL || persistence_owner == NULL || proposal_member == NULL ||
        !ucn_cluster_config_joint_runtime_is_valid(runtime)) {
        return UCN_ERR_ARGUMENT;
    }
    durable_state = ucn_cluster_config_persist_owner_state(persistence_owner);
    if (durable_state == NULL) {
        return UCN_ERR_STATE;
    }
    /* Abort application is likewise a no-op only after the exact durable
     * Abort journal is present.  A later/different transaction cannot reuse
     * this terminal Runtime value. */
    if (!runtime->joint_active) {
        if (runtime->transaction.phase !=
                (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_ABORTED ||
            ucn_cluster_config_persist_ref_from_state(
                &runtime->active_config, &base_ref) != UCN_OK ||
            durable_state->config_transaction.phase !=
                UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED ||
            durable_state->config_transaction.transaction_id !=
                runtime->transaction.transaction_id ||
            durable_state->last_completed_operation !=
                UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT ||
            !config_refs_equal(&durable_state->committed_config, &base_ref)) {
            return UCN_ERR_STATE;
        }
        if (runtime->transaction.proposal_kind ==
            (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_ADD) {
            return member_matches_add_proposal(proposal_member,
                                               &runtime->transaction) ?
                       UCN_OK :
                       UCN_ERR_ARGUMENT;
        }
        if (runtime->transaction.proposal_kind ==
            (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE) {
            return member_matches_aborted_remove(proposal_member,
                                                 &runtime->transaction) ?
                       UCN_OK :
                       UCN_ERR_ARGUMENT;
        }
        return UCN_ERR_STATE;
    }
    if (ucn_cluster_config_persist_ref_from_state(
            &runtime->transaction.base_config, &base_ref) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    if (
        durable_state->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED ||
        durable_state->config_transaction.transaction_id !=
            runtime->transaction.transaction_id ||
        durable_state->last_completed_operation !=
            UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT ||
        !config_refs_equal(&durable_state->committed_config, &base_ref)) {
        return UCN_ERR_STATE;
    }
    member_candidate = *proposal_member;
    if (runtime->transaction.proposal_kind ==
        (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_ADD) {
        if (!member_matches_add_proposal(proposal_member,
                                         &runtime->transaction)) {
            return UCN_ERR_ARGUMENT;
        }
    } else if (runtime->transaction.proposal_kind ==
               (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE) {
        if (!member_matches_remove_proposal(proposal_member,
                                            &runtime->transaction)) {
            return UCN_ERR_ARGUMENT;
        }
        member_candidate.status =
            (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    } else {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_member_record_is_valid(&member_candidate)) {
        return UCN_ERR_STATE;
    }
    candidate = *runtime;
    candidate.active_config = candidate.transaction.base_config;
    candidate.transaction.phase = (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_ABORTED;
    candidate.transaction.persist_stage =
        (uint8_t)UCN_CLUSTER_CONFIG_PERSIST_STAGE_ABORTED;
    candidate.joint_active = false;
    if (!ucn_cluster_config_joint_runtime_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *proposal_member = member_candidate;
    *runtime = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_joint_runtime_enter(
    ucn_cluster_config_joint_runtime_t *runtime,
    const ucn_cluster_config_persist_owner_t *persistence_owner,
    const ucn_cluster_config_tx_t *transaction)
{
    const ucn_cluster_persist_state_t *durable_state;
    ucn_cluster_config_joint_runtime_t candidate;
    ucn_cluster_persist_config_ref_t expected_staging;

    if (runtime == NULL || persistence_owner == NULL || transaction == NULL ||
        !ucn_cluster_config_joint_runtime_is_valid(runtime) ||
        runtime->joint_active || !ucn_cluster_config_tx_is_active(transaction) ||
        !ucn_cluster_config_joint_quorum_reached(transaction) ||
        !config_states_equal(&runtime->active_config,
                             &transaction->base_config) ||
        ucn_cluster_config_persist_ref_from_joint_new(&transaction->proposed_config,
                                                      &expected_staging) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    durable_state = ucn_cluster_config_persist_owner_state(persistence_owner);
    if (durable_state == NULL ||
        durable_state->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        durable_state->config_transaction.transaction_id !=
            transaction->transaction_id ||
        durable_state->last_completed_operation !=
            UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT ||
        !config_refs_equal(&durable_state->config_transaction.staging_config,
                           &expected_staging)) {
        return UCN_ERR_STATE;
    }
    candidate = *runtime;
    candidate.active_config = transaction->proposed_config;
    candidate.transaction = *transaction;
    candidate.transaction.phase = (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_JOINT;
    candidate.transaction.persist_stage =
        (uint8_t)UCN_CLUSTER_CONFIG_PERSIST_STAGE_JOINT_DURABLE;
    candidate.joint_active = true;
    if (!ucn_cluster_config_joint_runtime_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *runtime = candidate;
    return UCN_OK;
}
