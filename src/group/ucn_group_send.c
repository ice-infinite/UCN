#include "internal/ucn_group.h"

#include "internal/ucn_checked.h"

#include <string.h>

static uint16_t next_generation(uint16_t current)
{
    if (current == 0U) return 1U;
    if (current == UINT16_MAX) return 0U;
    return (uint16_t)(current + 1U);
}

static bool digest_nonzero(const uint8_t digest[UCN_I_GROUP_DIGEST_BYTES])
{
    uint8_t index;

    for (index = 0U; index < UCN_I_GROUP_DIGEST_BYTES; ++index) {
        if (digest[index] != 0U) return true;
    }
    return false;
}

static int free_send_slot(const ucn_i_group_owner_t *owner)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_SEND_COUNT; ++index) {
        if (!owner->sends[index].occupied) return (int)index;
    }
    return -1;
}

static uint16_t free_attempt_count(const ucn_i_group_owner_t *owner)
{
    uint16_t count = 0U;
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        if (!owner->attempts[index].occupied) count++;
    }
    return count;
}

static int reserve_attempt(ucn_i_group_owner_t *owner, uint16_t parent_slot,
                           uint16_t member_index, uint8_t plan)
{
    uint16_t index;
    uint16_t generation;

    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        if (!owner->attempts[index].occupied) {
            generation = next_generation(owner->attempts[index].handle_generation);
            if (generation == 0U) return -2;
            memset(&owner->attempts[index], 0,
                   sizeof(owner->attempts[index]));
            owner->attempts[index].send_id =
                owner->sends[parent_slot].send_id;
            owner->attempts[index].group_id =
                owner->sends[parent_slot].group_id;
            owner->attempts[index].handle_generation = generation;
            owner->attempts[index].parent_slot = parent_slot;
            owner->attempts[index].member_index = member_index;
            if (member_index < UCN_I_GROUP_MEMBER_COUNT) {
                owner->attempts[index].target =
                    owner->sends[parent_slot].members[member_index];
            }
            owner->attempts[index].plan = plan;
            owner->attempts[index].phase = UCN_I_GROUP_ATTEMPT_READY;
            owner->attempts[index].occupied = 1U;
            return (int)index;
        }
    }
    return -1;
}

static void release_send_attempts(ucn_i_group_owner_t *owner,
                                  uint16_t send_slot)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        ucn_i_group_attempt_slot_t *attempt = &owner->attempts[index];
        if (attempt->occupied && !attempt->parent_is_rx &&
            attempt->parent_slot == send_slot) {
            uint16_t generation = attempt->handle_generation;
            memset(attempt, 0, sizeof(*attempt));
            attempt->handle_generation = generation;
        }
    }
}

static bool attempts_terminal(const ucn_i_group_owner_t *owner,
                              uint16_t send_slot)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        const ucn_i_group_attempt_slot_t *attempt = &owner->attempts[index];
        if (attempt->occupied && !attempt->parent_is_rx &&
            attempt->parent_slot == send_slot &&
            attempt->phase != UCN_I_GROUP_ATTEMPT_SUCCEEDED &&
            attempt->phase != UCN_I_GROUP_ATTEMPT_FAILED &&
            attempt->phase != UCN_I_GROUP_ATTEMPT_CANCELLED) {
            return false;
        }
    }
    return true;
}

static bool send_handle_matches(const ucn_i_group_owner_t *owner,
                                ucn_handle_t handle, uint16_t *slot_out)
{
    if (!ucn_i_group_p_owner_valid(owner) ||
        !ucn_i_handle_matches(&handle, owner->runtime_instance,
                              owner->owner_instance,
                              UCN_I_GROUP_SEND_COUNT,
                              UCN_OBJECT_KIND_SEND) ||
        !owner->sends[handle.slot].occupied ||
        owner->sends[handle.slot].handle_generation != handle.generation) {
        return false;
    }
    if (slot_out != NULL) *slot_out = handle.slot;
    return true;
}

bool ucn_i_group_p_attempt_handle_matches(
    const ucn_i_group_owner_t *owner, ucn_handle_t handle,
    uint16_t *slot_out)
{
    if (!ucn_i_group_p_owner_valid(owner) ||
        !ucn_i_handle_matches(&handle, owner->runtime_instance,
                              owner->owner_instance,
                              UCN_I_GROUP_ATTEMPT_COUNT,
                              UCN_OBJECT_KIND_SEND) ||
        !owner->attempts[handle.slot].occupied ||
        owner->attempts[handle.slot].handle_generation != handle.generation) {
        return false;
    }
    if (slot_out != NULL) *slot_out = handle.slot;
    return true;
}

void ucn_i_group_p_recompute_send(ucn_i_group_send_slot_t *send)
{
    uint16_t success_weight = 0U;
    uint8_t index;
    bool satisfied = false;

    if (send == NULL || !send->occupied) return;
    switch (send->success_scope) {
    case UCN_I_GROUP_SCOPE_LOCAL_ONLY:
        satisfied = true;
        break;
    case UCN_I_GROUP_SCOPE_ANY_MEMBER:
        satisfied = send->success_bitmap != 0U;
        break;
    case UCN_I_GROUP_SCOPE_ALL_MEMBERS:
    case UCN_I_GROUP_SCOPE_SUBSET:
        satisfied = (send->success_bitmap & send->requested_bitmap) ==
                    send->requested_bitmap;
        break;
    case UCN_I_GROUP_SCOPE_QUORUM:
        for (index = 0U; index < send->member_count; ++index) {
            if ((send->success_bitmap & (UINT32_C(1) << index)) != 0U) {
                success_weight =
                    (uint16_t)(success_weight + send->members[index].weight);
            }
        }
        satisfied = success_weight >= send->quorum_weight;
        break;
    default:
        break;
    }
    send->completion_satisfied = satisfied ? 1U : 0U;
    if (satisfied) send->phase = UCN_I_GROUP_SEND_COMPLETE;
}

ucn_result_t ucn_i_group_send_begin(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    const ucn_i_group_send_request_t *request,
    uint64_t now_us, ucn_handle_t *send_out)
{
    const ucn_i_group_context_config_t *config;
    ucn_i_group_send_slot_t *send;
    uint32_t requested_bitmap;
    uint16_t group_slot;
    uint16_t attempt_count;
    uint16_t send_slot;
    uint16_t generation;
    uint8_t plan;
    uint8_t index;
    int free_slot;
    ucn_handle_t handle;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || request == NULL ||
        send_out == NULL || request->send_id == 0U ||
        request->absolute_deadline_us == 0U ||
        now_us >= request->absolute_deadline_us ||
        request->success_scope >= UCN_I_GROUP_SCOPE_COUNT ||
        request->reliable_receipts > 1U ||
        request->authenticated_delivery > 1U ||
        request->exact_principal_delivery > 1U ||
        !digest_nonzero(request->payload_digest) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), send_out,
                             sizeof(*send_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), send_out,
                             sizeof(*send_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_handle_matches(owner, group, &group_slot) ||
        owner->contexts[group_slot].phase != UCN_I_GROUP_ACTIVE) {
        result = UCN_ERR_STATE;
        goto done;
    }
    config = &owner->contexts[group_slot].current;
    if (request->expected_group_generation != config->group_generation ||
        request->expected_policy_generation != config->policy_generation ||
        request->expected_member_generation != config->member_generation ||
        request->expected_key_generation !=
            config->security.key_generation ||
        request->expected_tree_generation != config->tree.tree_generation ||
        (config->allowed_scope_mask &
         UCN_I_GROUP_SCOPE_BIT(request->success_scope)) == 0U ||
        (config->secure_required != 0U &&
         request->authenticated_delivery == 0U) ||
        (config->security.exact_principal != 0U &&
         request->exact_principal_delivery == 0U) ||
        (request->success_scope != UCN_I_GROUP_SCOPE_LOCAL_ONLY &&
         (request->reliable_receipts == 0U ||
          request->authenticated_delivery == 0U))) {
        result = UCN_ERR_POLICY;
        goto done;
    }
    requested_bitmap = config->member_count == 32U ? UINT32_MAX :
        ((UINT32_C(1) << config->member_count) - 1U);
    if (request->success_scope == UCN_I_GROUP_SCOPE_SUBSET) {
        if (request->subset_bitmap == 0U ||
            (request->subset_bitmap & ~requested_bitmap) != 0U) {
            result = UCN_ERR_ARGUMENT;
            goto done;
        }
        requested_bitmap = request->subset_bitmap;
    } else if (request->subset_bitmap != 0U) {
        result = UCN_ERR_ARGUMENT;
        goto done;
    }
    if (config->tree.valid) {
        plan = UCN_I_GROUP_PLAN_TREE;
        attempt_count = 1U;
    } else if (config->member_count <= config->unicast_fanout_limit) {
        plan = UCN_I_GROUP_PLAN_UNICAST;
        attempt_count = config->member_count;
    } else {
        result = UCN_ERR_UNSUPPORTED;
        goto done;
    }
    free_slot = free_send_slot(owner);
    if (free_slot < 0 || free_attempt_count(owner) < attempt_count) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    send_slot = (uint16_t)free_slot;
    generation = next_generation(owner->sends[send_slot].handle_generation);
    if (generation == 0U) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    memset(&owner->sends[send_slot], 0, sizeof(owner->sends[send_slot]));
    send = &owner->sends[send_slot];
    memcpy(send->members, config->members,
           (size_t)config->member_count * sizeof(config->members[0]));
    send->security = config->security;
    send->tree = config->tree;
    memcpy(send->payload_digest, request->payload_digest,
           sizeof(send->payload_digest));
    send->send_id = request->send_id;
    send->deadline_us = request->absolute_deadline_us;
    send->group_id = config->group_id;
    send->group_generation = config->group_generation;
    send->policy_generation = config->policy_generation;
    send->member_generation = config->member_generation;
    send->requested_bitmap = requested_bitmap;
    send->endpoint = config->endpoint;
    send->opcode = config->opcode;
    send->handle_generation = generation;
    send->member_count = config->member_count;
    send->quorum_weight = config->quorum_weight;
    send->success_scope = request->success_scope;
    send->plan = plan;
    send->phase = UCN_I_GROUP_SEND_ACTIVE;
    send->occupied = 1U;
    if (plan == UCN_I_GROUP_PLAN_TREE) {
        if (reserve_attempt(owner, send_slot, UINT16_MAX, plan) < 0) {
            memset(send, 0, sizeof(*send));
            send->handle_generation = generation;
            result = UCN_ERR_STATE;
            goto done;
        }
    } else {
        for (index = 0U; index < config->member_count; ++index) {
            if (reserve_attempt(owner, send_slot, index, plan) < 0) {
                release_send_attempts(owner, send_slot);
                memset(send, 0, sizeof(*send));
                send->handle_generation = generation;
                result = UCN_ERR_STATE;
                goto done;
            }
        }
    }
    ucn_i_group_p_recompute_send(send);
    ucn_i_group_p_make_handle(owner, send_slot, generation,
                              UCN_OBJECT_KIND_SEND, &handle);
    *send_out = handle;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_attempt_peek(
    ucn_i_group_owner_t *owner,
    ucn_i_group_attempt_view_t *attempt_out)
{
    ucn_i_group_attempt_view_t view;
    uint16_t visited;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || attempt_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), attempt_out,
                             sizeof(*attempt_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    result = UCN_ERR_NOT_FOUND;
    for (visited = 0U; visited < UCN_I_GROUP_ATTEMPT_COUNT; ++visited) {
        uint16_t index = (uint16_t)((owner->attempt_cursor + visited) %
                                    UCN_I_GROUP_ATTEMPT_COUNT);
        const ucn_i_group_attempt_slot_t *attempt = &owner->attempts[index];
        if (attempt->occupied && attempt->phase == UCN_I_GROUP_ATTEMPT_READY) {
            memset(&view, 0, sizeof(view));
            ucn_i_group_p_make_handle(owner, index,
                                      attempt->handle_generation,
                                      UCN_OBJECT_KIND_SEND, &view.attempt);
            view.send_id = attempt->send_id;
            view.group_id = attempt->group_id;
            view.plan = attempt->plan;
            if (!attempt->parent_is_rx) {
                const ucn_i_group_send_slot_t *parent =
                    &owner->sends[attempt->parent_slot];
                view.group_generation = parent->group_generation;
                view.policy_generation = parent->policy_generation;
                view.member_generation = parent->member_generation;
                view.security = parent->security;
                view.tree = parent->tree;
                memcpy(view.payload_digest, parent->payload_digest,
                       sizeof(view.payload_digest));
                view.endpoint = parent->endpoint;
                view.opcode = parent->opcode;
            } else {
                const ucn_i_group_rx_slot_t *parent =
                    &owner->receipts[attempt->parent_slot];
                view.group_generation = parent->facts.group_generation;
                view.policy_generation = parent->facts.policy_generation;
                view.member_generation = parent->facts.member_generation;
                view.security = parent->security;
                view.tree = parent->tree;
                memcpy(view.payload_digest, parent->facts.payload_digest,
                       sizeof(view.payload_digest));
                view.endpoint = parent->facts.endpoint;
                view.opcode = parent->facts.opcode;
            }
            if (attempt->member_index < UCN_I_GROUP_MEMBER_COUNT) {
                view.member_address = attempt->target.address;
                view.member_binding_generation =
                    attempt->target.binding_generation;
                view.sender_slot = attempt->target.sender_slot;
            }
            owner->attempt_cursor = (uint16_t)((index + 1U) %
                                               UCN_I_GROUP_ATTEMPT_COUNT);
            *attempt_out = view;
            result = UCN_OK;
            break;
        }
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_attempt_note_submitted(
    ucn_i_group_owner_t *owner, ucn_handle_t attempt)
{
    uint16_t slot;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner)) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_attempt_handle_matches(owner, attempt, &slot) ||
        owner->attempts[slot].phase != UCN_I_GROUP_ATTEMPT_READY) {
        result = UCN_ERR_STATE;
    } else {
        owner->attempts[slot].phase = UCN_I_GROUP_ATTEMPT_SUBMITTED;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_attempt_complete(
    ucn_i_group_owner_t *owner, ucn_handle_t attempt,
    ucn_result_t terminal_result)
{
    ucn_i_group_attempt_slot_t *slot;
    uint16_t index;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || terminal_result > UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_attempt_handle_matches(owner, attempt, &index)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    slot = &owner->attempts[index];
    if (slot->phase == UCN_I_GROUP_ATTEMPT_SUCCEEDED ||
        slot->phase == UCN_I_GROUP_ATTEMPT_FAILED) {
        result = ((slot->phase == UCN_I_GROUP_ATTEMPT_SUCCEEDED) ==
                  (terminal_result == UCN_OK)) ? UCN_OK : UCN_ERR_STATE;
    } else if (slot->phase != UCN_I_GROUP_ATTEMPT_SUBMITTED) {
        result = UCN_ERR_STATE;
    } else {
        slot->phase = terminal_result == UCN_OK ?
            UCN_I_GROUP_ATTEMPT_SUCCEEDED : UCN_I_GROUP_ATTEMPT_FAILED;
        result = UCN_OK;
    }
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_send_accept_receipt(
    ucn_i_group_owner_t *owner, ucn_handle_t send_handle,
    const ucn_i_group_member_receipt_t *receipt)
{
    ucn_i_group_send_slot_t *send;
    uint16_t send_slot;
    uint8_t index;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || receipt == NULL ||
        receipt->terminal > 1U || receipt->authenticated > 1U ||
        receipt->terminal_result > UCN_OK ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), receipt,
                             sizeof(*receipt))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!send_handle_matches(owner, send_handle, &send_slot)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    send = &owner->sends[send_slot];
    if (receipt->send_id != send->send_id ||
        receipt->group_id != send->group_id ||
        receipt->group_generation != send->group_generation ||
        receipt->authenticated != 1U || receipt->terminal != 1U ||
        memcmp(receipt->payload_digest, send->payload_digest,
               UCN_I_GROUP_DIGEST_BYTES) != 0) {
        result = UCN_ERR_SECURITY;
        goto done;
    }
    result = UCN_ERR_NOT_FOUND;
    for (index = 0U; index < send->member_count; ++index) {
        uint32_t bit = UINT32_C(1) << index;
        const ucn_i_group_member_t *member = &send->members[index];
        if (member->address == receipt->member_address &&
            member->binding_generation == receipt->member_binding_generation &&
            memcmp(member->principal, receipt->member_principal,
                   UCN_I_GROUP_PRINCIPAL_BYTES) == 0) {
            if ((send->result_bitmap & bit) != 0U) {
                result = send->member_results[index] ==
                         receipt->terminal_result ? UCN_OK : UCN_ERR_REPLAY;
            } else {
                send->member_results[index] = receipt->terminal_result;
                send->result_bitmap |= bit;
                if (receipt->terminal_result == UCN_OK) {
                    send->success_bitmap |= bit;
                }
                ucn_i_group_p_recompute_send(send);
                result = UCN_OK;
            }
            break;
        }
    }
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_send_view(ucn_i_group_owner_t *owner,
                                   ucn_handle_t send_handle,
                                   ucn_i_group_send_view_t *view_out)
{
    ucn_i_group_send_view_t view;
    ucn_i_group_send_slot_t *send;
    uint16_t slot;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!send_handle_matches(owner, send_handle, &slot)) {
        result = UCN_ERR_STATE;
    } else {
        send = &owner->sends[slot];
        memset(&view, 0, sizeof(view));
        view.send_id = send->send_id;
        view.group_id = send->group_id;
        view.requested_bitmap = send->requested_bitmap;
        view.result_bitmap = send->result_bitmap;
        view.success_bitmap = send->success_bitmap;
        view.success_scope = send->success_scope;
        view.plan = send->plan;
        view.phase = send->phase;
        view.completion_satisfied = send->completion_satisfied;
        *view_out = view;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_send_retire(ucn_i_group_owner_t *owner,
                                     ucn_handle_t send_handle)
{
    uint16_t generation;
    uint16_t slot;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner)) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!send_handle_matches(owner, send_handle, &slot) ||
        (owner->sends[slot].phase != UCN_I_GROUP_SEND_COMPLETE &&
         owner->sends[slot].phase != UCN_I_GROUP_SEND_FAILED) ||
        !attempts_terminal(owner, slot)) {
        result = UCN_ERR_STATE;
    } else {
        generation = owner->sends[slot].handle_generation;
        release_send_attempts(owner, slot);
        memset(&owner->sends[slot], 0, sizeof(owner->sends[slot]));
        owner->sends[slot].handle_generation = generation;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}
