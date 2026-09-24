#include "internal/ucn_cluster.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;
    if (bytes == NULL) return false;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) return true;
    }
    return false;
}

static bool object_zero(const void *object, size_t bytes)
{
    const uint8_t *value = (const uint8_t *)object;
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        if (value[index] != 0U) return false;
    }
    return true;
}

static bool lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

bool ucn_i_cluster_p_principal_equal(const uint8_t *left,
                                     const uint8_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, UCN_I_CLUSTER_PRINCIPAL_BYTES) == 0;
}

static bool config_member_valid(const ucn_i_cluster_config_member_t *member)
{
    uint8_t legal = UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
                    UCN_I_CLUSTER_MEMBER_FLAG_VOTER |
                    UCN_I_CLUSTER_MEMBER_FLAG_BACKUP;
    if (member == NULL || member->binding_generation == 0U ||
        !bytes_nonzero(member->principal, sizeof(member->principal)) ||
        (member->flags & ~legal) != 0U ||
        (member->flags & UCN_I_CLUSTER_MEMBER_FLAG_MEMBER) == 0U ||
        !object_zero(member->reserved_zero, sizeof(member->reserved_zero))) {
        return false;
    }
    if ((member->flags & UCN_I_CLUSTER_MEMBER_FLAG_BACKUP) != 0U &&
        (member->flags & UCN_I_CLUSTER_MEMBER_FLAG_VOTER) == 0U) {
        return false;
    }
    return true;
}

bool ucn_i_cluster_p_config_valid(const ucn_i_cluster_config_view_t *config)
{
    uint8_t index;
    uint8_t voters = 0U;
    uint8_t backups = 0U;

    if (config == NULL || config->config_id == 0U ||
        config->generation == 0U || config->member_count == 0U ||
        config->member_count > UCN_I_CLUSTER_CONFIG_MEMBER_COUNT ||
        !object_zero(config->reserved_zero, sizeof(config->reserved_zero))) {
        return false;
    }
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        if (index < config->member_count) {
            if (!config_member_valid(&config->members[index])) return false;
            if (index != 0U &&
                memcmp(config->members[index - 1U].principal,
                       config->members[index].principal,
                       UCN_I_CLUSTER_PRINCIPAL_BYTES) >= 0) return false;
            if ((config->members[index].flags &
                 UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U) voters++;
            if ((config->members[index].flags &
                 UCN_I_CLUSTER_MEMBER_FLAG_BACKUP) != 0U) backups++;
        } else if (!object_zero(&config->members[index],
                                sizeof(config->members[index]))) {
            return false;
        }
    }
    return voters != 0U && backups <= 1U;
}

bool ucn_i_cluster_p_epoch_valid(const ucn_i_cluster_epoch_t *epoch)
{
    return epoch != NULL && epoch->cluster_id != 0U && epoch->term != 0U &&
           epoch->head_binding_generation != 0U &&
           bytes_nonzero(epoch->head_principal,
                         sizeof(epoch->head_principal));
}

static int config_find(const ucn_i_cluster_config_view_t *config,
                       const uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES])
{
    uint8_t index;
    for (index = 0U; index < config->member_count; ++index) {
        if (ucn_i_cluster_p_principal_equal(config->members[index].principal,
                                            principal)) return (int)index;
    }
    return -1;
}

static bool epoch_head_is_voter(const ucn_i_cluster_epoch_t *epoch,
                                const ucn_i_cluster_config_view_t *config)
{
    int index = config_find(config, epoch->head_principal);
    return index >= 0 &&
           config->members[index].binding_generation ==
               epoch->head_binding_generation &&
           (config->members[index].flags &
            UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U;
}

static bool vote_valid(const ucn_i_cluster_vote_evidence_t *vote,
                       const ucn_i_cluster_config_view_t *config,
                       uint8_t expected_index)
{
    if (vote == NULL || vote->valid > 1U ||
        !object_zero(vote->reserved_zero, sizeof(vote->reserved_zero))) {
        return false;
    }
    if (vote->valid == 0U) return object_zero(vote, sizeof(*vote));
    return vote->voter_index == expected_index &&
           expected_index < config->member_count && vote->vote_id != 0U &&
           vote->binding_generation ==
               config->members[expected_index].binding_generation &&
           vote->session_generation != 0U &&
           vote->capability_generation != 0U &&
           bytes_nonzero(vote->canonical_digest,
                         sizeof(vote->canonical_digest)) &&
           ucn_i_cluster_p_principal_equal(
               vote->principal, config->members[expected_index].principal);
}

static uint32_t config_voter_mask(
    const ucn_i_cluster_config_view_t *config)
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

bool ucn_i_cluster_p_state_valid(const ucn_i_cluster_state_t *state)
{
    uint8_t index;
    uint32_t allowed_mask;

    if (state == NULL || !ucn_i_cluster_p_epoch_valid(&state->epoch) ||
        !ucn_i_cluster_p_config_valid(&state->stable_config) ||
        !epoch_head_is_voter(&state->epoch, &state->stable_config) ||
        state->role > UCN_I_CLUSTER_FENCED ||
        state->phase < UCN_I_CLUSTER_STABLE ||
        state->phase > UCN_I_CLUSTER_FAULT || state->joint_valid > 1U ||
        state->authority_fenced > 1U || state->backup_ready > 1U ||
        !object_zero(state->reserved_zero, sizeof(state->reserved_zero))) {
        return false;
    }
    if (state->joint_valid != 0U &&
        !ucn_i_cluster_p_config_valid(&state->transition.target_config)) {
        return false;
    }
    if (state->backup_ready == 0U) {
        if (state->backup_assignment_generation != 0U ||
            state->backup_coverage_mask != 0U ||
            !object_zero(state->backup_snapshot_digest,
                         sizeof(state->backup_snapshot_digest))) return false;
    } else if (state->role != UCN_I_CLUSTER_BACKUP ||
               state->backup_assignment_generation == 0U ||
               state->backup_coverage_mask !=
                   config_voter_mask(&state->stable_config) ||
               !bytes_nonzero(state->backup_snapshot_digest,
                              sizeof(state->backup_snapshot_digest))) {
        return false;
    }
    if ((state->lineage_generation == 0U) !=
            (state->retired_cluster_high_water == 0U) ||
        state->transition.kind > UCN_I_CLUSTER_TRANSITION_MERGE ||
        state->transition.handover_ready_valid > 1U ||
        state->transition.reserved_zero != 0U) return false;
    if (state->backup_ready != 0U &&
        state->transition.handover_ready_valid != 0U) return false;
    if (state->transition.kind == UCN_I_CLUSTER_TRANSITION_NONE) {
        if (!object_zero(&state->transition,
                         sizeof(state->transition))) return false;
    } else if (state->transition.transaction_id == 0U ||
               state->transition.absolute_deadline_us == 0U ||
               state->transition.phase != state->phase ||
               !ucn_i_cluster_p_epoch_valid(
                   &state->transition.target_epoch) ||
               !ucn_i_cluster_p_config_valid(
                   &state->transition.target_config) ||
               !epoch_head_is_voter(
                   &state->transition.target_epoch,
                   &state->transition.target_config)) {
        return false;
    }
    if (state->transition.handover_ready_valid != 0U) {
        const ucn_i_cluster_handover_ready_t *ready =
            &state->transition.handover_ready;
        if ((state->transition.kind != UCN_I_CLUSTER_TRANSITION_HANDOVER &&
             state->transition.kind != UCN_I_CLUSTER_TRANSITION_MERGE) ||
            ready->transaction_id != state->transition.transaction_id ||
            ready->target_cluster_id !=
                state->transition.target_epoch.cluster_id ||
            ready->target_term != state->transition.target_epoch.term ||
            ready->target_binding_generation !=
                state->transition.target_epoch.head_binding_generation ||
            ready->target_config_id !=
                state->transition.target_config.config_id ||
            ready->target_config_generation !=
                state->transition.target_config.generation ||
            !ucn_i_cluster_p_principal_equal(
                ready->target_principal,
                state->transition.target_epoch.head_principal) ||
            !bytes_nonzero(ready->proof_digest,
                           sizeof(ready->proof_digest))) {
            return false;
        }
    }
    if (state->transition.kind == UCN_I_CLUSTER_TRANSITION_MERGE) {
        if (state->phase != UCN_I_CLUSTER_EPOCH_DURABLE ||
            state->role != UCN_I_CLUSTER_FENCED ||
            state->authority_fenced == 0U ||
            state->retired_cluster_high_water != state->epoch.cluster_id ||
            state->transition.target_epoch.cluster_id <=
                state->retired_cluster_high_water) {
            return false;
        }
    } else if (state->retired_cluster_high_water >= state->epoch.cluster_id &&
               state->retired_cluster_high_water != 0U) {
        return false;
    }
    allowed_mask = state->stable_config.member_count == 32U ? UINT32_MAX :
                   ((UINT32_C(1) << state->stable_config.member_count) - 1U);
    if ((state->transition.old_vote_mask & ~allowed_mask) != 0U) return false;
    if (state->joint_valid != 0U ||
        state->transition.kind != UCN_I_CLUSTER_TRANSITION_NONE) {
        allowed_mask = state->transition.target_config.member_count == 32U ?
                       UINT32_MAX :
                       ((UINT32_C(1) <<
                         state->transition.target_config.member_count) - 1U);
        if ((state->transition.new_vote_mask & ~allowed_mask) != 0U) {
            return false;
        }
    } else if (state->transition.new_vote_mask != 0U) return false;
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        bool old_set = (state->transition.old_vote_mask &
                        (UINT32_C(1) << index)) != 0U;
        bool new_set = (state->transition.new_vote_mask &
                        (UINT32_C(1) << index)) != 0U;
        if (old_set) {
            if (!vote_valid(&state->transition.old_votes[index],
                            &state->stable_config, index)) return false;
        } else if (!object_zero(&state->transition.old_votes[index],
                                sizeof(state->transition.old_votes[index]))) {
            return false;
        }
        if (state->joint_valid != 0U ||
            state->transition.kind != UCN_I_CLUSTER_TRANSITION_NONE) {
            if (new_set) {
                if (!vote_valid(&state->transition.new_votes[index],
                                &state->transition.target_config,
                                index)) return false;
            } else if (!object_zero(
                           &state->transition.new_votes[index],
                           sizeof(state->transition.new_votes[index]))) {
                return false;
            }
        } else if (new_set ||
                   !object_zero(&state->transition.new_votes[index],
                                sizeof(state->transition.new_votes[index]))) {
            return false;
        }
    }
    return true;
}

bool ucn_i_cluster_p_owner_valid(const ucn_i_cluster_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_I_CLUSTER_MAGIC &&
           owner->schema == UCN_I_CLUSTER_SCHEMA &&
           owner->runtime_instance != 0U && owner->owner_instance != 0U &&
           bytes_nonzero(owner->local_principal,
                         sizeof(owner->local_principal)) &&
           lock_valid(&owner->state_lock);
}

static bool member_fact_valid(const ucn_i_cluster_member_fact_t *fact,
                              uint64_t now_us)
{
    return fact != NULL && fact->lease_deadline_us > now_us &&
           fact->capability_deadline_us > now_us &&
           fact->binding_generation != 0U &&
           fact->session_generation != 0U &&
           fact->capability_generation != 0U &&
           fact->route_generation != 0U && fact->link_generation != 0U &&
           fact->link_id != 0U && fact->link_id != UINT16_MAX &&
           fact->authenticated == 1U && fact->current == 1U &&
           fact->backup_eligible <= 1U && fact->reserved_zero == 0U &&
           bytes_nonzero(fact->principal, sizeof(fact->principal)) &&
           bytes_nonzero(fact->capability_digest,
                         sizeof(fact->capability_digest));
}

static int runtime_member_find(const ucn_i_cluster_owner_t *owner,
                               const uint8_t *principal)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT; ++index) {
        if (owner->members[index].occupied != 0U &&
            ucn_i_cluster_p_principal_equal(
                owner->members[index].fact.principal, principal)) {
            return (int)index;
        }
    }
    return -1;
}

static int runtime_member_empty(const ucn_i_cluster_owner_t *owner)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT; ++index) {
        if (owner->members[index].occupied == 0U) return (int)index;
    }
    return -1;
}

static bool fact_in_config(const ucn_i_cluster_config_view_t *config,
                           const ucn_i_cluster_member_fact_t *fact)
{
    int index = config_find(config, fact->principal);
    return index >= 0 &&
           config->members[index].binding_generation ==
               fact->binding_generation;
}

static void invalidate_votes_for_principal(
    ucn_i_cluster_state_t *state,
    const uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES])
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        if (state->transition.old_votes[index].valid != 0U &&
            ucn_i_cluster_p_principal_equal(
                state->transition.old_votes[index].principal, principal)) {
            memset(&state->transition.old_votes[index], 0,
                   sizeof(state->transition.old_votes[index]));
            state->transition.old_vote_mask &= ~(UINT32_C(1) << index);
        }
        if (state->transition.new_votes[index].valid != 0U &&
            ucn_i_cluster_p_principal_equal(
                state->transition.new_votes[index].principal, principal)) {
            memset(&state->transition.new_votes[index], 0,
                   sizeof(state->transition.new_votes[index]));
            state->transition.new_vote_mask &= ~(UINT32_C(1) << index);
        }
    }
    if (state->phase == UCN_I_CLUSTER_QUORUM) {
        state->phase = UCN_I_CLUSTER_COLLECTING;
        state->transition.phase = UCN_I_CLUSTER_COLLECTING;
    }
}

ucn_result_t ucn_i_cluster_owner_init(
    ucn_i_cluster_owner_t *owner, const ucn_i_cluster_config_t *config)
{
    if (owner == NULL || config == NULL ||
        !object_zero(owner, sizeof(*owner)) || config->runtime_instance == 0U ||
        config->owner_instance == 0U || config->reserved_zero != 0U ||
        !bytes_nonzero(config->local_principal,
                       sizeof(config->local_principal)) ||
        !lock_valid(&config->state_lock) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config,
                             sizeof(*config))) return UCN_ERR_ARGUMENT;
    owner->magic = UCN_I_CLUSTER_MAGIC;
    owner->schema = UCN_I_CLUSTER_SCHEMA;
    owner->runtime_instance = config->runtime_instance;
    owner->owner_instance = config->owner_instance;
    memcpy(owner->local_principal, config->local_principal,
           sizeof(owner->local_principal));
    owner->state_lock = config->state_lock;
    return UCN_OK;
}

ucn_result_t ucn_i_cluster_owner_reset(ucn_i_cluster_owner_t *owner)
{
    ucn_i_lock_ops_t lock;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner)) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->state.epoch.cluster_id != 0U || owner->pending_valid != 0U) {
        owner->state_lock.leave(owner->state_lock.context);
        return UCN_ERR_STATE;
    }
    lock = owner->state_lock;
    memset(owner, 0, sizeof(*owner));
    lock.leave(lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_cluster_member_observe(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_member_fact_t *fact, uint64_t now_us)
{
    int found;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        !member_fact_valid(fact, now_us) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), fact, sizeof(*fact))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->state.epoch.cluster_id == 0U ||
        (!fact_in_config(&owner->state.stable_config, fact) &&
         !(owner->state.joint_valid != 0U &&
           fact_in_config(&owner->state.transition.target_config, fact)))) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    found = runtime_member_find(owner, fact->principal);
    if (found < 0) found = runtime_member_empty(owner);
    if (found < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    if (owner->members[found].occupied != 0U &&
        (owner->members[found].fact.binding_generation !=
             fact->binding_generation ||
         owner->members[found].fact.session_generation !=
             fact->session_generation ||
         owner->members[found].fact.capability_generation !=
             fact->capability_generation ||
         owner->members[found].fact.link_id != fact->link_id ||
         owner->members[found].fact.link_generation !=
             fact->link_generation)) {
        invalidate_votes_for_principal(&owner->state, fact->principal);
    }
    owner->members[found].fact = *fact;
    owner->members[found].occupied = 1U;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

static bool live_config_member(const ucn_i_cluster_owner_t *owner,
                               const ucn_i_cluster_config_member_t *member,
                               uint64_t now_us,
                               const ucn_i_cluster_vote_evidence_t *vote)
{
    int found = runtime_member_find(owner, member->principal);
    const ucn_i_cluster_member_fact_t *fact;
    if (found < 0) return false;
    fact = &owner->members[found].fact;
    if (!member_fact_valid(fact, now_us) ||
        fact->binding_generation != member->binding_generation) return false;
    if (vote == NULL) return true;
    return vote->valid == 1U &&
           vote->binding_generation == fact->binding_generation &&
           vote->session_generation == fact->session_generation &&
           vote->capability_generation == fact->capability_generation &&
           ucn_i_cluster_p_principal_equal(vote->principal,
                                           fact->principal) &&
           memcmp(vote->canonical_digest, fact->capability_digest,
                  UCN_I_CLUSTER_DIGEST_BYTES) == 0;
}

static uint32_t live_voter_mask(const ucn_i_cluster_owner_t *owner,
                                const ucn_i_cluster_config_view_t *config,
                                uint64_t now_us)
{
    uint32_t mask = 0U;
    uint8_t index;
    for (index = 0U; index < config->member_count; ++index) {
        if ((config->members[index].flags &
             UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U &&
            live_config_member(owner, &config->members[index], now_us,
                               NULL)) mask |= UINT32_C(1) << index;
    }
    return mask;
}

static uint8_t voter_count(const ucn_i_cluster_config_view_t *config)
{
    uint8_t count = 0U;
    uint8_t index;
    for (index = 0U; index < config->member_count; ++index) {
        if ((config->members[index].flags &
             UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U) count++;
    }
    return count;
}

static uint8_t popcount32(uint32_t value)
{
    uint8_t count = 0U;
    while (value != 0U) {
        value &= value - 1U;
        count++;
    }
    return count;
}

static bool quorum_met(const ucn_i_cluster_config_view_t *config,
                       uint32_t live_mask)
{
    uint8_t total = voter_count(config);
    return popcount32(live_mask) >= (uint8_t)(total / 2U + 1U);
}

ucn_result_t ucn_i_cluster_p_refresh(
    ucn_i_cluster_owner_t *owner, uint64_t now_us,
    uint32_t *live_old_out, uint32_t *live_new_out)
{
    uint32_t live_old;
    uint32_t live_new = 0U;
    bool active;
    if (!ucn_i_cluster_p_owner_valid(owner) || live_old_out == NULL ||
        live_new_out == NULL || owner->state.epoch.cluster_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    live_old = live_voter_mask(owner, &owner->state.stable_config, now_us);
    if (owner->state.joint_valid != 0U) {
        live_new = live_voter_mask(owner,
                                  &owner->state.transition.target_config,
                                  now_us);
    }
    active = owner->state.role == UCN_I_CLUSTER_HEAD &&
             owner->state.authority_fenced == 0U &&
             owner->state.phase != UCN_I_CLUSTER_FAULT &&
             ucn_i_cluster_p_principal_equal(
                 owner->state.epoch.head_principal,
                 owner->local_principal) &&
             quorum_met(&owner->state.stable_config, live_old) &&
             (owner->state.joint_valid == 0U ||
              quorum_met(&owner->state.transition.target_config, live_new));
    owner->authority_active = active ? 1U : 0U;
    *live_old_out = live_old;
    *live_new_out = live_new;
    return UCN_OK;
}

static void fill_authority_view(const ucn_i_cluster_owner_t *owner,
                                uint32_t live_old, uint32_t live_new,
                                ucn_i_cluster_authority_view_t *view)
{
    memset(view, 0, sizeof(*view));
    view->epoch = owner->state.epoch;
    view->stable_config_id = owner->state.stable_config.config_id;
    view->stable_config_generation = owner->state.stable_config.generation;
    if (owner->state.joint_valid != 0U) {
        view->joint_config_id =
            owner->state.transition.target_config.config_id;
        view->joint_config_generation =
            owner->state.transition.target_config.generation;
    }
    view->live_old_voters = live_old;
    view->live_new_voters = live_new;
    view->role = owner->state.role;
    view->phase = owner->state.phase;
    view->authority_active = owner->authority_active;
    view->joint_valid = owner->state.joint_valid;
    view->fenced = owner->state.authority_fenced;
}

ucn_result_t ucn_i_cluster_authority_preflight(
    ucn_i_cluster_owner_t *owner, uint64_t now_us,
    ucn_i_cluster_authority_view_t *view_out)
{
    uint32_t old_live;
    uint32_t new_live;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    result = ucn_i_cluster_p_refresh(owner, now_us, &old_live, &new_live);
    if (result == UCN_OK) fill_authority_view(owner, old_live, new_live,
                                              view_out);
    if (result == UCN_OK && owner->authority_active == 0U) {
        result = UCN_ERR_ACCESS;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_get_view(
    ucn_i_cluster_owner_t *owner,
    ucn_i_cluster_authority_view_t *view_out)
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->state.epoch.cluster_id == 0U) result = UCN_ERR_STATE;
    else {
        fill_authority_view(owner, 0U, 0U, view_out);
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

/* Shared within Cluster implementation. */
int ucn_i_cluster_p_config_find(
    const ucn_i_cluster_config_view_t *config,
    const uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES])
{
    return config_find(config, principal);
}

bool ucn_i_cluster_p_live_vote(
    const ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_config_view_t *config, uint8_t index,
    const ucn_i_cluster_vote_evidence_t *vote, uint64_t now_us)
{
    return index < config->member_count &&
           live_config_member(owner, &config->members[index], now_us, vote);
}

bool ucn_i_cluster_p_quorum(
    const ucn_i_cluster_config_view_t *config, uint32_t mask)
{
    return quorum_met(config, mask);
}
