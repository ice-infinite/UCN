#include "internal/ucn_cluster.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool bytes_nonzero(const uint8_t *bytes, size_t count)
{
    size_t index;
    uint8_t value = 0U;
    for (index = 0U; index < count; ++index) value |= bytes[index];
    return value != 0U;
}

static bool member_equal(const ucn_i_cluster_config_member_t *left,
                         const ucn_i_cluster_config_member_t *right)
{
    return left->binding_generation == right->binding_generation &&
           left->flags == right->flags &&
           ucn_i_cluster_p_principal_equal(left->principal,
                                           right->principal);
}

static bool rekey_config_checked_next(
    const ucn_i_cluster_config_view_t *current,
    const ucn_i_cluster_config_view_t *successor)
{
    uint8_t index;
    if (!ucn_i_cluster_p_config_valid(successor) ||
        current->config_id == UINT32_MAX ||
        current->generation == UINT32_MAX ||
        successor->config_id != current->config_id + 1U ||
        successor->generation != current->generation + 1U ||
        successor->member_count != current->member_count) {
        return false;
    }
    for (index = 0U; index < current->member_count; ++index) {
        if (!member_equal(&current->members[index],
                          &successor->members[index])) return false;
    }
    return true;
}

static bool epoch_head_is_voter(
    const ucn_i_cluster_epoch_t *epoch,
    const ucn_i_cluster_config_view_t *config)
{
    int index = ucn_i_cluster_p_config_find(config, epoch->head_principal);
    return index >= 0 &&
           config->members[index].binding_generation ==
               epoch->head_binding_generation &&
           (config->members[index].flags &
            UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U;
}

static ucn_result_t current_authority_locked(ucn_i_cluster_owner_t *owner,
                                             uint64_t now_us)
{
    uint32_t old_live;
    uint32_t new_live;
    ucn_result_t result = ucn_i_cluster_p_refresh(owner, now_us,
                                                  &old_live, &new_live);
    if (result != UCN_OK) return result;
    return owner->authority_active != 0U ? UCN_OK : UCN_ERR_ACCESS;
}

ucn_result_t ucn_i_cluster_rekey_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_epoch_t *successor_epoch,
    const ucn_i_cluster_config_view_t *successor_config,
    uint64_t transaction_id, uint64_t now_us,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        !ucn_i_cluster_p_epoch_valid(successor_epoch) ||
        !ucn_i_cluster_p_config_valid(successor_config) ||
        transaction_id == 0U || durability == NULL ||
        requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), successor_epoch,
                             sizeof(*successor_epoch)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), successor_config,
                             sizeof(*successor_config))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_STABLE ||
        owner->state.joint_valid != 0U ||
        owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_NONE ||
        owner->state.role != UCN_I_CLUSTER_HEAD ||
        owner->state.authority_fenced != 0U ||
        owner->state.transaction_high_water == UINT64_MAX ||
        transaction_id != owner->state.transaction_high_water + 1U ||
        owner->state.epoch.cluster_id == UINT32_MAX ||
        successor_epoch->cluster_id != owner->state.epoch.cluster_id + 1U ||
        successor_epoch->term != 1U ||
        !ucn_i_cluster_p_principal_equal(successor_epoch->head_principal,
                                         owner->local_principal) ||
        !epoch_head_is_voter(successor_epoch, successor_config) ||
        !rekey_config_checked_next(&owner->state.stable_config,
                                   successor_config) ||
        owner->state.lineage_generation == UINT32_MAX ||
        current_authority_locked(owner, now_us) != UCN_OK) {
        result = UCN_ERR_STATE;
        goto done;
    }
    owner->pending_state = owner->state;
    owner->pending_state.epoch = *successor_epoch;
    owner->pending_state.stable_config = *successor_config;
    memset(&owner->pending_state.transition, 0,
           sizeof(owner->pending_state.transition));
    owner->pending_state.transaction_high_water = transaction_id;
    owner->pending_state.retired_cluster_high_water =
        owner->state.epoch.cluster_id;
    owner->pending_state.lineage_generation =
        owner->state.lineage_generation + 1U;
    owner->pending_state.backup_ready = 0U;
    owner->pending_state.backup_assignment_generation = 0U;
    owner->pending_state.backup_coverage_mask = 0U;
    memset(owner->pending_state.backup_snapshot_digest, 0,
           sizeof(owner->pending_state.backup_snapshot_digest));
    owner->pending_state.phase = UCN_I_CLUSTER_STABLE;
    owner->pending_state.role = UCN_I_CLUSTER_HEAD;
    owner->pending_state.authority_fenced = 0U;
    result = ucn_i_cluster_p_prepare_requirement(
        owner, UCN_I_CLUSTER_PERSIST_REKEY_COMMIT, durability,
        requirement_out);
    if (result != UCN_OK) memset(&owner->pending_state, 0,
                                 sizeof(owner->pending_state));
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

static bool merge_ready_valid(
    const ucn_i_cluster_handover_ready_t *ready,
    const ucn_i_cluster_epoch_t *successor_epoch,
    const ucn_i_cluster_config_view_t *successor_config,
    uint64_t transaction_id, uint64_t now_us)
{
    return ready != NULL && ready->transaction_id == transaction_id &&
           ready->lease_deadline_us > now_us &&
           ready->target_cluster_id == successor_epoch->cluster_id &&
           ready->target_term == successor_epoch->term &&
           ready->target_binding_generation ==
               successor_epoch->head_binding_generation &&
           ready->target_config_id == successor_config->config_id &&
           ready->target_config_generation == successor_config->generation &&
           ready->capability_generation != 0U &&
           ready->authority_generation != 0U &&
           ready->authenticated == 1U && ready->quorum_verified == 1U &&
           ready->durable_continuation == 1U &&
           ready->reserved_zero == 0U &&
           ucn_i_cluster_p_principal_equal(
               ready->target_principal,
               successor_epoch->head_principal) &&
           bytes_nonzero(ready->proof_digest,
                         sizeof(ready->proof_digest));
}

ucn_result_t ucn_i_cluster_merge_retire_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_epoch_t *successor_epoch,
    const ucn_i_cluster_config_view_t *successor_config,
    const ucn_i_cluster_handover_ready_t *ready,
    uint64_t transaction_id, uint64_t now_us,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_i_cluster_handover_ready_t persisted_ready;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        !ucn_i_cluster_p_epoch_valid(successor_epoch) ||
        !ucn_i_cluster_p_config_valid(successor_config) ||
        !merge_ready_valid(ready, successor_epoch, successor_config,
                           transaction_id, now_us) ||
        durability == NULL || requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), successor_epoch,
                             sizeof(*successor_epoch)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), successor_config,
                             sizeof(*successor_config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), ready,
                             sizeof(*ready))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_STABLE ||
        owner->state.joint_valid != 0U ||
        owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_NONE ||
        owner->state.role != UCN_I_CLUSTER_HEAD ||
        owner->state.transaction_high_water == UINT64_MAX ||
        transaction_id != owner->state.transaction_high_water + 1U ||
        successor_epoch->cluster_id <= owner->state.epoch.cluster_id ||
        successor_epoch->cluster_id <=
            owner->state.retired_cluster_high_water ||
        successor_epoch->term != 1U ||
        !epoch_head_is_voter(successor_epoch, successor_config) ||
        owner->state.lineage_generation == UINT32_MAX ||
        current_authority_locked(owner, now_us) != UCN_OK) {
        result = UCN_ERR_STATE;
        goto done;
    }
    owner->pending_state = owner->state;
    memset(&owner->pending_state.transition, 0,
           sizeof(owner->pending_state.transition));
    owner->pending_state.transition.kind =
        UCN_I_CLUSTER_TRANSITION_MERGE;
    owner->pending_state.transition.phase =
        UCN_I_CLUSTER_EPOCH_DURABLE;
    owner->pending_state.transition.target_epoch = *successor_epoch;
    owner->pending_state.transition.target_config = *successor_config;
    owner->pending_state.transition.transaction_id = transaction_id;
    owner->pending_state.transition.absolute_deadline_us =
        ready->lease_deadline_us;
    memset(&persisted_ready, 0, sizeof(persisted_ready));
    persisted_ready.transaction_id = transaction_id;
    persisted_ready.target_cluster_id = successor_epoch->cluster_id;
    persisted_ready.target_term = successor_epoch->term;
    persisted_ready.target_binding_generation =
        successor_epoch->head_binding_generation;
    persisted_ready.target_config_id = successor_config->config_id;
    persisted_ready.target_config_generation = successor_config->generation;
    memcpy(persisted_ready.target_principal,
           successor_epoch->head_principal,
           sizeof(persisted_ready.target_principal));
    memcpy(persisted_ready.proof_digest, ready->proof_digest,
           sizeof(persisted_ready.proof_digest));
    owner->pending_state.transition.handover_ready = persisted_ready;
    owner->pending_state.transition.handover_ready_valid = 1U;
    owner->pending_state.transaction_high_water = transaction_id;
    owner->pending_state.retired_cluster_high_water =
        owner->state.epoch.cluster_id;
    owner->pending_state.lineage_generation =
        owner->state.lineage_generation + 1U;
    owner->pending_state.backup_ready = 0U;
    owner->pending_state.backup_assignment_generation = 0U;
    owner->pending_state.backup_coverage_mask = 0U;
    memset(owner->pending_state.backup_snapshot_digest, 0,
           sizeof(owner->pending_state.backup_snapshot_digest));
    owner->pending_state.phase = UCN_I_CLUSTER_EPOCH_DURABLE;
    owner->pending_state.role = UCN_I_CLUSTER_FENCED;
    owner->pending_state.authority_fenced = 1U;
    result = ucn_i_cluster_p_prepare_requirement(
        owner, UCN_I_CLUSTER_PERSIST_MERGE_RETIRE, durability,
        requirement_out);
    if (result != UCN_OK) memset(&owner->pending_state, 0,
                                 sizeof(owner->pending_state));
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_merge_ready_accept(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_handover_ready_t *ready, uint64_t now_us)
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || ready == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), ready,
                             sizeof(*ready))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_MERGE ||
        owner->state.phase != UCN_I_CLUSTER_EPOCH_DURABLE ||
        !merge_ready_valid(ready, &owner->state.transition.target_epoch,
                           &owner->state.transition.target_config,
                           owner->state.transition.transaction_id,
                           now_us)) {
        result = UCN_ERR_STATE;
    } else if (memcmp(owner->state.transition.handover_ready.proof_digest,
                      ready->proof_digest,
                      UCN_I_CLUSTER_DIGEST_BYTES) != 0) {
        result = UCN_ERR_SECURITY;
    } else {
        owner->state.transition.handover_ready = *ready;
        owner->state.transition.handover_ready_valid = 1U;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_merge_continuation_preflight(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us)
{
    const ucn_i_cluster_handover_ready_t *ready;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || transaction_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    ready = &owner->state.transition.handover_ready;
    if (owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_MERGE ||
        owner->state.transition.transaction_id != transaction_id ||
        owner->state.phase != UCN_I_CLUSTER_EPOCH_DURABLE ||
        owner->state.authority_fenced == 0U ||
        owner->state.role != UCN_I_CLUSTER_FENCED ||
        ready->target_cluster_id !=
            owner->state.transition.target_epoch.cluster_id ||
        ready->target_term != owner->state.transition.target_epoch.term ||
        ready->target_binding_generation !=
            owner->state.transition.target_epoch.head_binding_generation ||
        ready->lease_deadline_us <= now_us ||
        ready->authenticated != 1U || ready->quorum_verified != 1U ||
        ready->durable_continuation != 1U) {
        result = UCN_ERR_ACCESS;
    } else {
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}
