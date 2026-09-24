#include "internal/ucn_cluster.h"

#include "internal/ucn_checked.h"

#include <string.h>

static void clear_pending_stage(ucn_i_cluster_owner_t *owner)
{
    memset(&owner->pending_state, 0, sizeof(owner->pending_state));
    memset(&owner->pending_durability, 0,
           sizeof(owner->pending_durability));
    memset(&owner->persistence_handle, 0,
           sizeof(owner->persistence_handle));
    memset(owner->pending_body_digest, 0,
           sizeof(owner->pending_body_digest));
    owner->pending_operation_kind = 0U;
    owner->pending_valid = 0U;
    owner->persistence_bound = 0U;
}

static bool local_role_valid(uint8_t role)
{
    return role == UCN_I_CLUSTER_MEMBER ||
           role == UCN_I_CLUSTER_VOTER ||
           role == UCN_I_CLUSTER_BACKUP ||
           role == UCN_I_CLUSTER_HEAD;
}

static bool role_matches_config(
    const ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_epoch_t *epoch,
    const ucn_i_cluster_config_view_t *config, uint8_t role)
{
    int index = ucn_i_cluster_p_config_find(config,
                                            owner->local_principal);
    uint8_t flags;
    if (index < 0) return false;
    flags = config->members[index].flags;
    if (role == UCN_I_CLUSTER_HEAD) {
        return (flags & UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U &&
               ucn_i_cluster_p_principal_equal(
                   epoch->head_principal, owner->local_principal) &&
               epoch->head_binding_generation ==
                   config->members[index].binding_generation;
    }
    if (role == UCN_I_CLUSTER_BACKUP) {
        return (flags & UCN_I_CLUSTER_MEMBER_FLAG_BACKUP) != 0U;
    }
    if (role == UCN_I_CLUSTER_VOTER) {
        return (flags & UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U;
    }
    return (flags & UCN_I_CLUSTER_MEMBER_FLAG_MEMBER) != 0U;
}

static ucn_result_t authority_locked(ucn_i_cluster_owner_t *owner,
                                     uint64_t now_us)
{
    uint32_t old_live;
    uint32_t new_live;
    ucn_result_t result = ucn_i_cluster_p_refresh(owner, now_us,
                                                  &old_live, &new_live);
    if (result != UCN_OK) return result;
    return owner->authority_active != 0U ? UCN_OK : UCN_ERR_ACCESS;
}

static ucn_result_t prepare_staged(
    ucn_i_cluster_owner_t *owner, uint16_t operation_kind,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_result_t result = ucn_i_cluster_p_prepare_requirement(
        owner, operation_kind, durability, requirement_out);
    if (result != UCN_OK) clear_pending_stage(owner);
    return result;
}

ucn_result_t ucn_i_cluster_create_prepare(
    ucn_i_cluster_owner_t *owner, const ucn_i_cluster_epoch_t *epoch,
    const ucn_i_cluster_config_view_t *config, uint8_t local_role,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        !ucn_i_cluster_p_epoch_valid(epoch) ||
        epoch->term != 1U ||
        !ucn_i_cluster_p_config_valid(config) ||
        !local_role_valid(local_role) || durability == NULL ||
        requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), epoch,
                             sizeof(*epoch)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config,
                             sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->state.epoch.cluster_id != 0U || owner->pending_valid != 0U ||
        !role_matches_config(owner, epoch, config, local_role)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    memset(&owner->pending_state, 0, sizeof(owner->pending_state));
    owner->pending_state.epoch = *epoch;
    owner->pending_state.stable_config = *config;
    owner->pending_state.role = local_role;
    owner->pending_state.phase = UCN_I_CLUSTER_STABLE;
    owner->pending_state.authority_fenced = 0U;
    result = prepare_staged(owner, UCN_I_CLUSTER_PERSIST_CREATE_COMMIT,
                            durability, requirement_out);
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_config_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_config_view_t *target_config,
    uint64_t transaction_id, uint64_t now_us,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_result_t result;
    int head;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        !ucn_i_cluster_p_config_valid(target_config) ||
        transaction_id == 0U || durability == NULL ||
        requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), target_config,
                             sizeof(*target_config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    head = ucn_i_cluster_p_config_find(
        target_config, owner->state.epoch.head_principal);
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_STABLE ||
        owner->state.joint_valid != 0U ||
        owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_NONE ||
        transaction_id != owner->state.transaction_high_water + 1U ||
        owner->state.transaction_high_water == UINT64_MAX ||
        target_config->generation !=
            owner->state.stable_config.generation + 1U ||
        owner->state.stable_config.generation == UINT32_MAX ||
        head < 0 ||
        (target_config->members[head].flags &
         UCN_I_CLUSTER_MEMBER_FLAG_VOTER) == 0U ||
        target_config->members[head].binding_generation !=
            owner->state.epoch.head_binding_generation ||
        authority_locked(owner, now_us) != UCN_OK) {
        result = UCN_ERR_STATE;
        goto done;
    }
    owner->pending_state = owner->state;
    memset(&owner->pending_state.transition, 0,
           sizeof(owner->pending_state.transition));
    owner->pending_state.transition.kind =
        UCN_I_CLUSTER_TRANSITION_CONFIG;
    owner->pending_state.transition.phase = UCN_I_CLUSTER_PREPARED;
    owner->pending_state.transition.transaction_id = transaction_id;
    owner->pending_state.transition.absolute_deadline_us =
        durability->absolute_deadline_us;
    owner->pending_state.transition.target_epoch = owner->state.epoch;
    owner->pending_state.transition.target_config = *target_config;
    owner->pending_state.transaction_high_water = transaction_id;
    owner->pending_state.phase = UCN_I_CLUSTER_PREPARED;
    result = prepare_staged(owner,
                            UCN_I_CLUSTER_PERSIST_CONFIG_PREPARED,
                            durability, requirement_out);
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_config_enter_joint_prepare(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us, const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || transaction_id == 0U ||
        durability == NULL || requirement_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_PREPARED ||
        owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_CONFIG ||
        owner->state.transition.transaction_id != transaction_id ||
        owner->state.joint_valid != 0U ||
        now_us >= owner->state.transition.absolute_deadline_us ||
        authority_locked(owner, now_us) != UCN_OK) {
        result = UCN_ERR_STATE;
        goto done;
    }
    owner->pending_state = owner->state;
    owner->pending_state.phase = UCN_I_CLUSTER_JOINT;
    owner->pending_state.transition.phase = UCN_I_CLUSTER_JOINT;
    owner->pending_state.joint_valid = 1U;
    result = prepare_staged(owner, UCN_I_CLUSTER_PERSIST_CONFIG_JOINT,
                            durability, requirement_out);
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_config_commit_prepare(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us, const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || transaction_id == 0U ||
        durability == NULL || requirement_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_JOINT ||
        owner->state.joint_valid == 0U ||
        owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_CONFIG ||
        owner->state.transition.transaction_id != transaction_id ||
        now_us >= owner->state.transition.absolute_deadline_us ||
        authority_locked(owner, now_us) != UCN_OK) {
        result = UCN_ERR_STATE;
        goto done;
    }
    owner->pending_state = owner->state;
    owner->pending_state.stable_config =
        owner->state.transition.target_config;
    memset(&owner->pending_state.transition, 0,
           sizeof(owner->pending_state.transition));
    owner->pending_state.phase = UCN_I_CLUSTER_STABLE;
    owner->pending_state.joint_valid = 0U;
    owner->pending_state.backup_ready = 0U;
    owner->pending_state.backup_assignment_generation = 0U;
    owner->pending_state.backup_coverage_mask = 0U;
    memset(owner->pending_state.backup_snapshot_digest, 0,
           sizeof(owner->pending_state.backup_snapshot_digest));
    result = prepare_staged(owner, UCN_I_CLUSTER_PERSIST_CONFIG_COMMIT,
                            durability, requirement_out);
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_config_abort_prepare(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint32_t target_config_id, uint32_t target_config_generation,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || transaction_id == 0U ||
        target_config_id == 0U || target_config_generation == 0U ||
        durability == NULL || requirement_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_PREPARED ||
        owner->state.joint_valid != 0U ||
        owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_CONFIG ||
        owner->state.transition.transaction_id != transaction_id ||
        owner->state.transition.target_config.config_id != target_config_id ||
        owner->state.transition.target_config.generation !=
            target_config_generation) {
        result = UCN_ERR_STATE;
        goto done;
    }
    owner->pending_state = owner->state;
    memset(&owner->pending_state.transition, 0,
           sizeof(owner->pending_state.transition));
    owner->pending_state.phase = UCN_I_CLUSTER_STABLE;
    result = prepare_staged(owner, UCN_I_CLUSTER_PERSIST_ABORT,
                            durability, requirement_out);
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}
