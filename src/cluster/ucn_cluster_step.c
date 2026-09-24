#include "internal/ucn_cluster.h"

#include "internal/ucn_checked.h"

#include <string.h>

static uint16_t invalidate_principal(
    ucn_i_cluster_state_t *state,
    const uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES])
{
    uint16_t removed = 0U;
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        if (state->transition.old_votes[index].valid != 0U &&
            ucn_i_cluster_p_principal_equal(
                state->transition.old_votes[index].principal, principal)) {
            memset(&state->transition.old_votes[index], 0,
                   sizeof(state->transition.old_votes[index]));
            state->transition.old_vote_mask &= ~(UINT32_C(1) << index);
            removed++;
        }
        if (state->transition.new_votes[index].valid != 0U &&
            ucn_i_cluster_p_principal_equal(
                state->transition.new_votes[index].principal, principal)) {
            memset(&state->transition.new_votes[index], 0,
                   sizeof(state->transition.new_votes[index]));
            state->transition.new_vote_mask &= ~(UINT32_C(1) << index);
            removed++;
        }
    }
    if (removed != 0U && state->phase == UCN_I_CLUSTER_QUORUM) {
        state->phase = UCN_I_CLUSTER_COLLECTING;
        state->transition.phase = UCN_I_CLUSTER_COLLECTING;
    }
    return removed;
}

ucn_result_t ucn_i_cluster_step(
    ucn_i_cluster_owner_t *owner, uint64_t now_us, uint16_t budget,
    ucn_i_cluster_step_result_t *result_out)
{
    const uint16_t total_slots =
        UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT +
        UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT +
        UCN_I_CLUSTER_TUNNEL_SLOT_COUNT + 1U;
    ucn_i_cluster_step_result_t result_value;
    uint16_t scans;
    uint32_t old_live;
    uint32_t new_live;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || result_out == NULL ||
        budget == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), result_out,
                             sizeof(*result_out))) return UCN_ERR_ARGUMENT;
    memset(&result_value, 0, sizeof(result_value));
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    for (scans = 0U; scans < budget && scans < total_slots; ++scans) {
        uint16_t flat = owner->step_cursor;
        owner->step_cursor = (uint8_t)((owner->step_cursor + 1U) %
                                       total_slots);
        result_value.operations++;
        if (flat < UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT) {
            ucn_i_cluster_member_slot_t *slot = &owner->members[flat];
            if (slot->occupied != 0U &&
                (slot->fact.lease_deadline_us <= now_us ||
                 slot->fact.capability_deadline_us <= now_us)) {
                result_value.votes_invalidated = (uint16_t)(
                    result_value.votes_invalidated +
                    invalidate_principal(&owner->state,
                                         slot->fact.principal));
                memset(slot, 0, sizeof(*slot));
                result_value.members_expired++;
                result_value.made_progress = 1U;
            }
        } else if (flat < UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT +
                              UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT) {
            uint16_t index = flat - UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT;
            ucn_i_cluster_directory_slot_t *slot = &owner->directory[index];
            if (slot->occupied != 0U &&
                (slot->fact.authority_deadline_us <= now_us ||
                 slot->fact.capability_deadline_us <= now_us ||
                 slot->fact.flow_deadline_us <= now_us)) {
                memset(slot, 0, sizeof(*slot));
                result_value.directories_expired++;
                result_value.made_progress = 1U;
            }
        } else if (flat < UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT +
                              UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT +
                              UCN_I_CLUSTER_TUNNEL_SLOT_COUNT) {
            uint16_t index = flat - UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT -
                             UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT;
            ucn_i_cluster_tunnel_slot_t *slot = &owner->tunnels[index];
            if (slot->occupied != 0U &&
                (slot->value.absolute_deadline_us <= now_us ||
                 slot->value.flow.deadline_us <= now_us)) {
                memset(slot, 0, sizeof(*slot));
                result_value.tunnels_expired++;
                result_value.made_progress = 1U;
            }
        } else if (owner->snapshot_sync.building != 0U) {
            ucn_i_cluster_snapshot_buffer_t *buffer =
                &owner->snapshot_sync.buffers[
                    owner->snapshot_sync.building_index];
            if (buffer->header.absolute_deadline_us <= now_us) {
                memset(buffer, 0, sizeof(*buffer));
                owner->snapshot_sync.building = 0U;
                result_value.snapshots_aborted++;
                result_value.made_progress = 1U;
            }
        }
    }
    if (owner->pending_valid != 0U &&
        now_us >= owner->pending_durability.absolute_deadline_us) {
        owner->state.authority_fenced = 1U;
        owner->state.phase = UCN_I_CLUSTER_FAULT;
        owner->authority_active = 0U;
        result_value.fenced = 1U;
        result_value.made_progress = 1U;
    } else if (owner->state.transition.kind !=
                   UCN_I_CLUSTER_TRANSITION_NONE &&
               owner->state.phase != UCN_I_CLUSTER_EPOCH_DURABLE &&
               owner->state.transition.absolute_deadline_us <= now_us) {
        owner->state.authority_fenced = 1U;
        owner->state.phase = UCN_I_CLUSTER_FAULT;
        owner->state.transition.phase = UCN_I_CLUSTER_FAULT;
        owner->authority_active = 0U;
        result_value.fenced = 1U;
        result_value.made_progress = 1U;
    }
    if (owner->state.epoch.cluster_id != 0U) {
        result = ucn_i_cluster_p_refresh(owner, now_us,
                                         &old_live, &new_live);
        if (result != UCN_OK) goto done;
    }
    result_value.authority_active = owner->authority_active;
    result_value.fenced = owner->state.authority_fenced;
    *result_out = result_value;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}
