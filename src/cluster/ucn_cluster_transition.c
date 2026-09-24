#include "internal/ucn_cluster.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool object_zero(const void *object, size_t bytes)
{
    const uint8_t *value = (const uint8_t *)object;
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        if (value[index] != 0U) return false;
    }
    return true;
}

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) return true;
    }
    return false;
}

static bool config_equal(const ucn_i_cluster_config_view_t *left,
                         const ucn_i_cluster_config_view_t *right)
{
    uint8_t index;
    if (left->config_id != right->config_id ||
        left->generation != right->generation ||
        left->member_count != right->member_count) return false;
    for (index = 0U; index < left->member_count; ++index) {
        if (left->members[index].binding_generation !=
                right->members[index].binding_generation ||
            left->members[index].flags != right->members[index].flags ||
            memcmp(left->members[index].principal,
                   right->members[index].principal,
                   UCN_I_CLUSTER_PRINCIPAL_BYTES) != 0) return false;
    }
    return true;
}

static uint32_t voter_mask(const ucn_i_cluster_config_view_t *config)
{
    uint32_t mask = 0U;
    uint8_t index;
    for (index = 0U; index < config->member_count; ++index) {
        if ((config->members[index].flags &
             UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U) {
            mask |= UINT32_C(1) << index;
        }
    }
    return mask;
}

static int backup_index(const ucn_i_cluster_config_view_t *config)
{
    uint8_t index;
    for (index = 0U; index < config->member_count; ++index) {
        if ((config->members[index].flags &
             UCN_I_CLUSTER_MEMBER_FLAG_BACKUP) != 0U) return (int)index;
    }
    return -1;
}

static bool vote_structural_valid(
    const ucn_i_cluster_vote_evidence_t *vote)
{
    return vote != NULL && vote->valid == 1U && vote->vote_id != 0U &&
           vote->binding_generation != 0U &&
           vote->session_generation != 0U &&
           vote->capability_generation != 0U &&
           object_zero(vote->reserved_zero, sizeof(vote->reserved_zero)) &&
           bytes_nonzero(vote->principal, sizeof(vote->principal)) &&
           bytes_nonzero(vote->canonical_digest,
                         sizeof(vote->canonical_digest));
}

static void recompute_vote_masks(ucn_i_cluster_owner_t *owner,
                                 ucn_i_cluster_state_t *state,
                                 uint64_t now_us)
{
    uint8_t index;
    state->transition.old_vote_mask = 0U;
    state->transition.new_vote_mask = 0U;
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        if (state->transition.old_votes[index].valid != 0U &&
            ucn_i_cluster_p_live_vote(
                owner, &state->stable_config, index,
                &state->transition.old_votes[index], now_us)) {
            state->transition.old_vote_mask |= UINT32_C(1) << index;
        }
        if (state->transition.new_votes[index].valid != 0U &&
            ucn_i_cluster_p_live_vote(
                owner, &state->transition.target_config, index,
                &state->transition.new_votes[index], now_us)) {
            state->transition.new_vote_mask |= UINT32_C(1) << index;
        }
    }
}

static bool transition_quorum(const ucn_i_cluster_state_t *state)
{
    return ucn_i_cluster_p_quorum(&state->stable_config,
                                  state->transition.old_vote_mask) &&
           ucn_i_cluster_p_quorum(&state->transition.target_config,
                                  state->transition.new_vote_mask);
}

static bool handover_ready_current(const ucn_i_cluster_owner_t *owner,
                                   uint64_t now_us);

ucn_result_t ucn_i_cluster_backup_ready_prepare(
    ucn_i_cluster_owner_t *owner, uint32_t assignment_generation,
    const ucn_i_cluster_config_view_t *snapshot_config,
    uint32_t protected_voter_bitmap,
    const uint8_t snapshot_digest[UCN_I_CLUSTER_DIGEST_BYTES],
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    uint32_t required;
    int backup;
    int local;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        assignment_generation == 0U ||
        !ucn_i_cluster_p_config_valid(snapshot_config) ||
        !bytes_nonzero(snapshot_digest, UCN_I_CLUSTER_DIGEST_BYTES) ||
        durability == NULL || requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), snapshot_config,
                             sizeof(*snapshot_config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), snapshot_digest,
                             UCN_I_CLUSTER_DIGEST_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    backup = backup_index(&owner->state.stable_config);
    local = ucn_i_cluster_p_config_find(&owner->state.stable_config,
                                        owner->local_principal);
    required = voter_mask(&owner->state.stable_config);
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_STABLE || backup < 0 ||
        local != backup || owner->state.role != UCN_I_CLUSTER_BACKUP ||
        !config_equal(snapshot_config, &owner->state.stable_config) ||
        protected_voter_bitmap != required ||
        assignment_generation <=
            owner->state.backup_assignment_generation) {
        result = UCN_ERR_STATE;
    } else {
        owner->pending_state = owner->state;
        owner->pending_state.backup_assignment_generation =
            assignment_generation;
        owner->pending_state.backup_coverage_mask =
            protected_voter_bitmap;
        memcpy(owner->pending_state.backup_snapshot_digest,
               snapshot_digest, UCN_I_CLUSTER_DIGEST_BYTES);
        owner->pending_state.backup_ready = 1U;
        result = ucn_i_cluster_p_prepare_requirement(
            owner, UCN_I_CLUSTER_PERSIST_BACKUP_READY, durability,
            requirement_out);
        if (result != UCN_OK) {
            memset(&owner->pending_state, 0,
                   sizeof(owner->pending_state));
        }
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_handover_continuation_preflight(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us)
{
    int target;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || transaction_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    recompute_vote_masks(owner, &owner->state, now_us);
    target = ucn_i_cluster_p_config_find(
        &owner->state.transition.target_config,
        owner->state.transition.target_epoch.head_principal);
    if (owner->pending_valid != 0U ||
        owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_HANDOVER ||
        owner->state.transition.transaction_id != transaction_id ||
        owner->state.phase != UCN_I_CLUSTER_EPOCH_DURABLE ||
        owner->state.transition.phase != UCN_I_CLUSTER_EPOCH_DURABLE ||
        owner->state.authority_fenced == 0U ||
        owner->state.role != UCN_I_CLUSTER_FENCED ||
        now_us >= owner->state.transition.absolute_deadline_us ||
        !handover_ready_current(owner, now_us) ||
        !transition_quorum(&owner->state) || target < 0 ||
        !ucn_i_cluster_p_live_vote(
            owner, &owner->state.transition.target_config,
            (uint8_t)target,
            &owner->state.transition.new_votes[target], now_us)) {
        result = UCN_ERR_ACCESS;
    } else {
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_transition_begin(
    ucn_i_cluster_owner_t *owner, uint8_t kind,
    const ucn_i_cluster_epoch_t *target_epoch,
    const ucn_i_cluster_config_view_t *target_config,
    uint64_t transaction_id, uint64_t absolute_deadline_us,
    uint64_t now_us)
{
    int target;
    int backup;
    uint32_t old_live;
    uint32_t new_live;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        (kind != UCN_I_CLUSTER_TRANSITION_TAKEOVER &&
         kind != UCN_I_CLUSTER_TRANSITION_RECOVERY &&
         kind != UCN_I_CLUSTER_TRANSITION_HANDOVER) ||
        !ucn_i_cluster_p_epoch_valid(target_epoch) ||
        !ucn_i_cluster_p_config_valid(target_config) ||
        transaction_id == 0U || absolute_deadline_us <= now_us ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), target_epoch,
                             sizeof(*target_epoch)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), target_config,
                             sizeof(*target_config))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    target = ucn_i_cluster_p_config_find(
        target_config, target_epoch->head_principal);
    backup = backup_index(&owner->state.stable_config);
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_STABLE ||
        owner->state.joint_valid != 0U ||
        owner->state.transition.kind != UCN_I_CLUSTER_TRANSITION_NONE ||
        transaction_id != owner->state.transaction_high_water + 1U ||
        owner->state.transaction_high_water == UINT64_MAX ||
        target_epoch->cluster_id != owner->state.epoch.cluster_id ||
        owner->state.epoch.term == UINT32_MAX ||
        target_epoch->term != owner->state.epoch.term + 1U || target < 0 ||
        (target_config->members[target].flags &
         UCN_I_CLUSTER_MEMBER_FLAG_VOTER) == 0U ||
        target_config->members[target].binding_generation !=
            target_epoch->head_binding_generation ||
        !config_equal(target_config, &owner->state.stable_config)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (kind == UCN_I_CLUSTER_TRANSITION_TAKEOVER &&
        (backup < 0 || target != backup ||
         !ucn_i_cluster_p_principal_equal(target_epoch->head_principal,
                                           owner->local_principal) ||
         owner->state.role != UCN_I_CLUSTER_BACKUP ||
         owner->state.backup_ready == 0U ||
         !ucn_i_cluster_p_snapshot_current(owner) ||
         owner->state.backup_coverage_mask !=
             voter_mask(&owner->state.stable_config))) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    if (kind == UCN_I_CLUSTER_TRANSITION_RECOVERY &&
        !ucn_i_cluster_p_principal_equal(target_epoch->head_principal,
                                         owner->local_principal)) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    if (kind == UCN_I_CLUSTER_TRANSITION_HANDOVER) {
        result = ucn_i_cluster_p_refresh(owner, now_us,
                                         &old_live, &new_live);
        if (result != UCN_OK || owner->authority_active == 0U ||
            target != backup) {
            result = UCN_ERR_ACCESS;
            goto done;
        }
    }
    memset(&owner->state.transition, 0, sizeof(owner->state.transition));
    owner->state.transition.kind = kind;
    owner->state.transition.phase = UCN_I_CLUSTER_COLLECTING;
    owner->state.transition.target_epoch = *target_epoch;
    owner->state.transition.target_config = *target_config;
    owner->state.transition.transaction_id = transaction_id;
    owner->state.transition.absolute_deadline_us = absolute_deadline_us;
    owner->state.transaction_high_water = transaction_id;
    owner->state.phase = UCN_I_CLUSTER_COLLECTING;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_transition_vote_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_vote_evidence_t *vote, bool target_config_vote,
    uint64_t now_us, const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    const ucn_i_cluster_config_view_t *config;
    int index;
    ucn_i_cluster_vote_evidence_t *slot;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        !vote_structural_valid(vote) || durability == NULL ||
        requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), vote,
                             sizeof(*vote))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    config = target_config_vote ? &owner->state.transition.target_config :
                                  &owner->state.stable_config;
    index = ucn_i_cluster_p_config_find(config, vote->principal);
    if (owner->pending_valid != 0U ||
        owner->state.transition.kind == UCN_I_CLUSTER_TRANSITION_NONE ||
        (owner->state.phase != UCN_I_CLUSTER_COLLECTING &&
         owner->state.phase != UCN_I_CLUSTER_QUORUM) ||
        now_us >= owner->state.transition.absolute_deadline_us || index < 0 ||
        vote->voter_index != (uint8_t)index ||
        (config->members[index].flags &
         UCN_I_CLUSTER_MEMBER_FLAG_VOTER) == 0U ||
        !ucn_i_cluster_p_live_vote(owner, config, (uint8_t)index,
                                   vote, now_us)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    slot = target_config_vote ?
           &owner->state.transition.new_votes[index] :
           &owner->state.transition.old_votes[index];
    if (slot->valid != 0U) {
        if (slot->vote_id == vote->vote_id &&
            slot->binding_generation == vote->binding_generation &&
            slot->session_generation == vote->session_generation &&
            slot->capability_generation == vote->capability_generation &&
            memcmp(slot->canonical_digest, vote->canonical_digest,
                   UCN_I_CLUSTER_DIGEST_BYTES) == 0) {
            result = UCN_ERR_STATE;
        } else {
            owner->state.authority_fenced = 1U;
            owner->authority_active = 0U;
            result = UCN_ERR_SECURITY;
        }
        goto done;
    }
    owner->pending_state = owner->state;
    slot = target_config_vote ?
           &owner->pending_state.transition.new_votes[index] :
           &owner->pending_state.transition.old_votes[index];
    *slot = *vote;
    if (config_equal(&owner->state.stable_config,
                     &owner->state.transition.target_config)) {
        ucn_i_cluster_vote_evidence_t *other = target_config_vote ?
            &owner->pending_state.transition.old_votes[index] :
            &owner->pending_state.transition.new_votes[index];
        if (other->valid == 0U) *other = *vote;
    }
    recompute_vote_masks(owner, &owner->pending_state, now_us);
    if (transition_quorum(&owner->pending_state)) {
        owner->pending_state.phase = UCN_I_CLUSTER_QUORUM;
        owner->pending_state.transition.phase = UCN_I_CLUSTER_QUORUM;
    } else {
        owner->pending_state.phase = UCN_I_CLUSTER_COLLECTING;
        owner->pending_state.transition.phase = UCN_I_CLUSTER_COLLECTING;
    }
    result = ucn_i_cluster_p_prepare_requirement(
        owner, UCN_I_CLUSTER_PERSIST_VOTE, durability, requirement_out);
    if (result != UCN_OK) memset(&owner->pending_state, 0,
                                 sizeof(owner->pending_state));
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_handover_ready_accept(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_handover_ready_t *ready, uint64_t now_us)
{
    int target;
    ucn_i_cluster_member_fact_t *fact;
    int member;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || ready == NULL ||
        ready->transaction_id == 0U || ready->lease_deadline_us <= now_us ||
        ready->capability_generation == 0U ||
        ready->authority_generation == 0U ||
        ready->authenticated != 1U || ready->quorum_verified != 1U ||
        ready->durable_continuation != 1U || ready->reserved_zero != 0U ||
        !bytes_nonzero(ready->proof_digest, sizeof(ready->proof_digest)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), ready,
                             sizeof(*ready))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    target = ucn_i_cluster_p_config_find(
        &owner->state.transition.target_config,
        ready->target_principal);
    member = target < 0 ? -1 : target;
    if (owner->state.transition.kind !=
            UCN_I_CLUSTER_TRANSITION_HANDOVER ||
        owner->state.transition.transaction_id != ready->transaction_id ||
        owner->state.transition.target_epoch.cluster_id !=
            ready->target_cluster_id ||
        owner->state.transition.target_epoch.term != ready->target_term ||
        owner->state.transition.target_epoch.head_binding_generation !=
            ready->target_binding_generation ||
        owner->state.transition.target_config.config_id !=
            ready->target_config_id ||
        owner->state.transition.target_config.generation !=
            ready->target_config_generation || target < 0 ||
        !ucn_i_cluster_p_principal_equal(
            owner->state.transition.target_epoch.head_principal,
            ready->target_principal)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (owner->state.phase == UCN_I_CLUSTER_EPOCH_DURABLE &&
        owner->state.transition.handover_ready_valid != 0U &&
        memcmp(owner->state.transition.handover_ready.proof_digest,
               ready->proof_digest,
               UCN_I_CLUSTER_DIGEST_BYTES) != 0) {
        result = UCN_ERR_SECURITY;
        goto done;
    }
    for (member = 0; member < (int)UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT;
         ++member) {
        if (owner->members[member].occupied != 0U &&
            ucn_i_cluster_p_principal_equal(
                owner->members[member].fact.principal,
                ready->target_principal)) break;
    }
    if (member == (int)UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    fact = &owner->members[member].fact;
    if (fact->lease_deadline_us <= now_us ||
        fact->capability_deadline_us <= now_us ||
        fact->capability_generation != ready->capability_generation ||
        fact->backup_eligible == 0U) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    owner->state.transition.handover_ready = *ready;
    owner->state.transition.handover_ready_valid = 1U;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

static bool handover_ready_current(const ucn_i_cluster_owner_t *owner,
                                   uint64_t now_us)
{
    const ucn_i_cluster_handover_ready_t *ready =
        &owner->state.transition.handover_ready;
    uint8_t index;
    if (owner->state.transition.handover_ready_valid == 0U ||
        ready->target_cluster_id !=
            owner->state.transition.target_epoch.cluster_id ||
        ready->target_term != owner->state.transition.target_epoch.term ||
        ready->target_binding_generation !=
            owner->state.transition.target_epoch.head_binding_generation ||
        ready->lease_deadline_us <= now_us || ready->authenticated != 1U ||
        ready->quorum_verified != 1U ||
        ready->durable_continuation != 1U) return false;
    for (index = 0U; index < UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT; ++index) {
        const ucn_i_cluster_member_fact_t *fact;
        if (owner->members[index].occupied == 0U) continue;
        fact = &owner->members[index].fact;
        if (ucn_i_cluster_p_principal_equal(
                fact->principal, ready->target_principal)) {
            return fact->lease_deadline_us > now_us &&
                   fact->capability_deadline_us > now_us &&
                   fact->capability_generation ==
                       ready->capability_generation &&
                   fact->backup_eligible != 0U;
        }
    }
    return false;
}

ucn_result_t ucn_i_cluster_transition_commit_prepare(
    ucn_i_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us, const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    int target;
    uint16_t operation_kind;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || transaction_id == 0U ||
        durability == NULL || requirement_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    recompute_vote_masks(owner, &owner->state, now_us);
    target = ucn_i_cluster_p_config_find(
        &owner->state.transition.target_config,
        owner->state.transition.target_epoch.head_principal);
    if (owner->pending_valid != 0U ||
        owner->state.phase != UCN_I_CLUSTER_QUORUM ||
        owner->state.transition.phase != UCN_I_CLUSTER_QUORUM ||
        owner->state.transition.transaction_id != transaction_id ||
        now_us >= owner->state.transition.absolute_deadline_us ||
        !transition_quorum(&owner->state) || target < 0 ||
        !ucn_i_cluster_p_live_vote(
            owner, &owner->state.transition.target_config,
            (uint8_t)target,
            &owner->state.transition.new_votes[target], now_us)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (owner->state.transition.kind ==
        UCN_I_CLUSTER_TRANSITION_HANDOVER) {
        ucn_i_cluster_handover_ready_t persisted_ready;
        if (!handover_ready_current(owner, now_us)) {
            result = UCN_ERR_ACCESS;
            goto done;
        }
        owner->pending_state = owner->state;
        owner->pending_state.role = UCN_I_CLUSTER_FENCED;
        owner->pending_state.authority_fenced = 1U;
        owner->pending_state.phase = UCN_I_CLUSTER_EPOCH_DURABLE;
        owner->pending_state.transition.phase =
            UCN_I_CLUSTER_EPOCH_DURABLE;
        memset(&persisted_ready, 0, sizeof(persisted_ready));
        persisted_ready.transaction_id =
            owner->state.transition.handover_ready.transaction_id;
        persisted_ready.target_cluster_id =
            owner->state.transition.target_epoch.cluster_id;
        persisted_ready.target_term =
            owner->state.transition.target_epoch.term;
        persisted_ready.target_binding_generation =
            owner->state.transition.target_epoch.head_binding_generation;
        persisted_ready.target_config_id =
            owner->state.transition.handover_ready.target_config_id;
        persisted_ready.target_config_generation =
            owner->state.transition.handover_ready.
                target_config_generation;
        memcpy(persisted_ready.target_principal,
               owner->state.transition.handover_ready.target_principal,
               sizeof(persisted_ready.target_principal));
        memcpy(persisted_ready.proof_digest,
               owner->state.transition.handover_ready.proof_digest,
               sizeof(persisted_ready.proof_digest));
        owner->pending_state.transition.handover_ready = persisted_ready;
        operation_kind = UCN_I_CLUSTER_PERSIST_HANDOVER_FENCE;
    } else {
        if (!ucn_i_cluster_p_principal_equal(
                owner->state.transition.target_epoch.head_principal,
                owner->local_principal)) {
            result = UCN_ERR_ACCESS;
            goto done;
        }
        owner->pending_state = owner->state;
        owner->pending_state.epoch = owner->state.transition.target_epoch;
        owner->pending_state.stable_config =
            owner->state.transition.target_config;
        owner->pending_state.role = UCN_I_CLUSTER_HEAD;
        owner->pending_state.authority_fenced = 0U;
        owner->pending_state.backup_ready = 0U;
        owner->pending_state.backup_assignment_generation = 0U;
        owner->pending_state.backup_coverage_mask = 0U;
        memset(owner->pending_state.backup_snapshot_digest, 0,
               sizeof(owner->pending_state.backup_snapshot_digest));
        owner->pending_state.phase = UCN_I_CLUSTER_EPOCH_DURABLE;
        owner->pending_state.transition.phase =
            UCN_I_CLUSTER_EPOCH_DURABLE;
        operation_kind = UCN_I_CLUSTER_PERSIST_EPOCH_COMMIT;
    }
    result = ucn_i_cluster_p_prepare_requirement(
        owner, operation_kind, durability, requirement_out);
    if (result != UCN_OK) memset(&owner->pending_state, 0,
                                 sizeof(owner->pending_state));
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}
