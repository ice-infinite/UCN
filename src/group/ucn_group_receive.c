#include "internal/ucn_group.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool digest_nonzero(const uint8_t digest[UCN_I_GROUP_DIGEST_BYTES])
{
    uint8_t index;

    for (index = 0U; index < UCN_I_GROUP_DIGEST_BYTES; ++index) {
        if (digest[index] != 0U) return true;
    }
    return false;
}

static uint16_t next_generation(uint16_t current)
{
    if (current == 0U) return 1U;
    if (current == UINT16_MAX) return 0U;
    return (uint16_t)(current + 1U);
}

static int find_context(const ucn_i_group_owner_t *owner,
                        const ucn_i_group_receive_facts_t *facts)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_CONTEXT_COUNT; ++index) {
        const ucn_i_group_context_slot_t *slot = &owner->contexts[index];
        if (slot->occupied && slot->phase == UCN_I_GROUP_ACTIVE &&
            slot->current.group_id == facts->group_id &&
            slot->current.group_generation == facts->group_generation &&
            slot->current.policy_generation == facts->policy_generation &&
            slot->current.member_generation == facts->member_generation) {
            return (int)index;
        }
    }
    return -1;
}

static int find_member(const ucn_i_group_context_config_t *config,
                       const ucn_i_group_receive_facts_t *facts)
{
    uint8_t index;

    for (index = 0U; index < config->member_count; ++index) {
        const ucn_i_group_member_t *member = &config->members[index];
        if (member->address == facts->source_address &&
            member->binding_generation == facts->source_binding_generation &&
            member->sender_slot == facts->sender_slot &&
            memcmp(member->principal, facts->source_principal,
                   UCN_I_GROUP_PRINCIPAL_BYTES) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int find_free_rx(const ucn_i_group_owner_t *owner)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_RX_COUNT; ++index) {
        if (!owner->receipts[index].occupied) return (int)index;
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

static uint8_t bitmap_count(uint32_t bitmap)
{
    uint8_t count = 0U;

    while (bitmap != 0U) {
        count = (uint8_t)(count + (uint8_t)(bitmap & 1U));
        bitmap >>= 1U;
    }
    return count;
}

static int reserve_rx_attempt(ucn_i_group_owner_t *owner,
                              uint16_t rx_slot, uint16_t member_index,
                              const ucn_i_group_member_t *target,
                              uint8_t plan)
{
    uint16_t index;
    uint16_t generation;

    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        if (!owner->attempts[index].occupied) {
            generation = next_generation(owner->attempts[index].handle_generation);
            if (generation == 0U) return -2;
            memset(&owner->attempts[index], 0,
                   sizeof(owner->attempts[index]));
            owner->attempts[index].send_id = owner->receipts[rx_slot].facts.send_id;
            owner->attempts[index].group_id = owner->receipts[rx_slot].facts.group_id;
            owner->attempts[index].handle_generation = generation;
            owner->attempts[index].parent_slot = rx_slot;
            owner->attempts[index].member_index = member_index;
            if (target != NULL) owner->attempts[index].target = *target;
            owner->attempts[index].parent_is_rx = 1U;
            owner->attempts[index].plan = plan;
            owner->attempts[index].phase = UCN_I_GROUP_ATTEMPT_HELD;
            owner->attempts[index].occupied = 1U;
            return (int)index;
        }
    }
    return -1;
}

void ucn_i_group_p_release_rx_attempts(ucn_i_group_owner_t *owner,
                                       uint16_t rx_slot,
                                       bool only_held)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        ucn_i_group_attempt_slot_t *attempt = &owner->attempts[index];
        if (attempt->occupied && attempt->parent_is_rx &&
            attempt->parent_slot == rx_slot &&
            (!only_held || attempt->phase == UCN_I_GROUP_ATTEMPT_HELD)) {
            uint16_t generation = attempt->handle_generation;
            memset(attempt, 0, sizeof(*attempt));
            attempt->handle_generation = generation;
        }
    }
}

static bool rx_attempts_terminal(const ucn_i_group_owner_t *owner,
                                 uint16_t rx_slot)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        const ucn_i_group_attempt_slot_t *attempt = &owner->attempts[index];
        if (attempt->occupied && attempt->parent_is_rx &&
            attempt->parent_slot == rx_slot &&
            attempt->phase != UCN_I_GROUP_ATTEMPT_SUCCEEDED &&
            attempt->phase != UCN_I_GROUP_ATTEMPT_FAILED &&
            attempt->phase != UCN_I_GROUP_ATTEMPT_CANCELLED) {
            return false;
        }
    }
    return true;
}

bool ucn_i_group_p_rx_handle_matches(const ucn_i_group_owner_t *owner,
                                     ucn_handle_t handle,
                                     uint16_t *slot_out)
{
    if (!ucn_i_group_p_owner_valid(owner) ||
        !ucn_i_handle_matches(&handle, owner->runtime_instance,
                              owner->owner_instance,
                              UCN_I_GROUP_RX_COUNT,
                              UCN_OBJECT_KIND_SEND) ||
        !owner->receipts[handle.slot].occupied ||
        owner->receipts[handle.slot].handle_generation != handle.generation) {
        return false;
    }
    if (slot_out != NULL) *slot_out = handle.slot;
    return true;
}

static bool receive_facts_valid(const ucn_i_group_receive_facts_t *facts,
                                uint64_t now_us)
{
    return facts != NULL && facts->send_id != 0U &&
           facts->absolute_deadline_us != 0U &&
           now_us < facts->absolute_deadline_us &&
           facts->group_id != 0U && facts->group_generation != 0U &&
           facts->policy_generation != 0U &&
           facts->member_generation != 0U && facts->source_address != 0U &&
           facts->source_binding_generation != 0U &&
           facts->endpoint != 0U && facts->opcode != 0U &&
           facts->sender_slot != 0U &&
           facts->fresh_authenticated <= 1U &&
           facts->authenticated_replay_candidate <= 1U &&
           facts->public_unsecured <= 1U &&
           facts->acl_authorized <= 1U && facts->exact_principal <= 1U &&
           facts->local_member <= 1U &&
           facts->relay_egress_count <= UCN_I_GROUP_MEMBER_COUNT &&
           facts->reserved_zero == 0U &&
           digest_nonzero(facts->source_principal) &&
           digest_nonzero(facts->payload_digest) &&
           (uint8_t)(facts->fresh_authenticated +
                     facts->authenticated_replay_candidate +
                     facts->public_unsecured) == 1U &&
           (facts->local_member != 0U || facts->relay_egress_count != 0U);
}

static bool receipt_same_delivery_key(
    const ucn_i_group_rx_slot_t *receipt,
    const ucn_i_group_receive_facts_t *facts)
{
    return receipt->occupied &&
           receipt->facts.send_id == facts->send_id &&
           receipt->facts.group_id == facts->group_id &&
           receipt->facts.group_generation == facts->group_generation &&
           receipt->facts.source_address == facts->source_address &&
           receipt->facts.source_binding_generation ==
               facts->source_binding_generation &&
           receipt->facts.sender_slot == facts->sender_slot &&
           memcmp(receipt->facts.source_principal, facts->source_principal,
                  UCN_I_GROUP_PRINCIPAL_BYTES) == 0;
}

static bool receipt_matches(const ucn_i_group_rx_slot_t *receipt,
                            const ucn_i_group_receive_facts_t *facts)
{
    return receipt_same_delivery_key(receipt, facts) &&
           receipt->facts.policy_generation == facts->policy_generation &&
           receipt->facts.member_generation == facts->member_generation &&
           receipt->facts.endpoint == facts->endpoint &&
           receipt->facts.opcode == facts->opcode &&
           memcmp(receipt->facts.payload_digest, facts->payload_digest,
                  UCN_I_GROUP_DIGEST_BYTES) == 0;
}

static void make_rx_view(const ucn_i_group_owner_t *owner,
                         uint16_t slot_index,
                         ucn_i_group_rx_view_t *view)
{
    const ucn_i_group_rx_slot_t *slot = &owner->receipts[slot_index];

    memset(view, 0, sizeof(*view));
    ucn_i_group_p_make_handle(owner, slot_index, slot->handle_generation,
                              UCN_OBJECT_KIND_SEND, &view->receipt);
    view->send_id = slot->facts.send_id;
    view->group_id = slot->facts.group_id;
    view->application_result = slot->application_result;
    view->action = slot->action;
    view->application_complete = slot->application_complete;
    view->stable_receipt = slot->published;
}

ucn_result_t ucn_i_group_receive_preflight(
    ucn_i_group_owner_t *owner,
    const ucn_i_group_receive_facts_t *facts, uint64_t now_us,
    ucn_handle_t *reservation_out,
    ucn_i_group_rx_view_t *replay_out)
{
    const ucn_i_group_context_config_t *config;
    ucn_i_group_rx_slot_t *receipt;
    ucn_handle_t reservation;
    ucn_i_group_rx_view_t replay;
    int context_index;
    int rx_index;
    int source_member_index;
    uint32_t legal_member_bitmap;
    uint16_t required_attempts;
    uint16_t generation;
    uint16_t index;
    uint8_t action;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) ||
        !receive_facts_valid(facts, now_us) || reservation_out == NULL ||
        replay_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), reservation_out,
                             sizeof(*reservation_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), replay_out,
                             sizeof(*replay_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), reservation_out,
                             sizeof(*reservation_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), replay_out,
                             sizeof(*replay_out)) ||
        ucn_i_ranges_overlap(reservation_out, sizeof(*reservation_out),
                             replay_out, sizeof(*replay_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    context_index = find_context(owner, facts);
    if (context_index < 0) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    config = &owner->contexts[context_index].current;
    source_member_index = find_member(config, facts);
    legal_member_bitmap = config->member_count == 32U ? UINT32_MAX :
        ((UINT32_C(1) << config->member_count) - 1U);
    if (source_member_index < 0 || config->endpoint != facts->endpoint ||
        config->opcode != facts->opcode ||
        facts->key_generation != config->security.key_generation ||
        (config->secure_required != 0U &&
         (facts->public_unsecured != 0U ||
          facts->acl_authorized != 1U)) ||
        (config->secure_required == 0U &&
         (config->public_static == 0U || facts->public_unsecured != 1U)) ||
        (config->security.exact_principal != 0U &&
         facts->exact_principal != 1U)) {
        result = UCN_ERR_SECURITY;
        goto done;
    }
    if (facts->relay_egress_count == 0U) {
        if (facts->relay_member_bitmap != 0U) {
            result = UCN_ERR_ARGUMENT;
            goto done;
        }
        required_attempts = 0U;
    } else if (config->tree.valid != 0U) {
        if (facts->relay_member_bitmap != 0U) {
            result = UCN_ERR_ARGUMENT;
            goto done;
        }
        required_attempts = 1U;
    } else {
        if ((facts->relay_member_bitmap & ~legal_member_bitmap) != 0U ||
            (facts->relay_member_bitmap &
             (UINT32_C(1) << (uint8_t)source_member_index)) != 0U ||
            bitmap_count(facts->relay_member_bitmap) !=
                facts->relay_egress_count) {
            result = UCN_ERR_ARGUMENT;
            goto done;
        }
        required_attempts = facts->relay_egress_count;
    }
    if (facts->authenticated_replay_candidate || facts->public_unsecured) {
        result = facts->authenticated_replay_candidate ?
            UCN_ERR_REPLAY : UCN_ERR_NOT_FOUND;
        for (index = 0U; index < UCN_I_GROUP_RX_COUNT; ++index) {
            if (!receipt_same_delivery_key(&owner->receipts[index], facts)) {
                continue;
            }
            if (!receipt_matches(&owner->receipts[index], facts)) {
                result = UCN_ERR_REPLAY;
                break;
            }
            if (owner->receipts[index].published &&
                owner->receipts[index].application_complete) {
                make_rx_view(owner, index, &replay);
                replay.action = UCN_I_GROUP_RX_REPLAY_RECEIPT;
                memset(&reservation, 0, sizeof(reservation));
                *reservation_out = reservation;
                *replay_out = replay;
                result = UCN_OK;
                break;
            }
            result = UCN_ERR_STATE;
            break;
        }
        if (result != UCN_ERR_NOT_FOUND ||
            facts->authenticated_replay_candidate) {
            goto done;
        }
    }
    if (config->secure_required == 0U && config->public_static == 0U) {
        result = UCN_ERR_SECURITY;
        goto done;
    }
    rx_index = find_free_rx(owner);
    if (rx_index < 0 || free_attempt_count(owner) < required_attempts) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    generation = next_generation(owner->receipts[rx_index].handle_generation);
    if (generation == 0U) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    memset(&owner->receipts[rx_index], 0,
           sizeof(owner->receipts[rx_index]));
    receipt = &owner->receipts[rx_index];
    receipt->facts = *facts;
    receipt->security = config->security;
    receipt->tree = config->tree;
    receipt->handle_generation = generation;
    if (facts->local_member && facts->relay_egress_count != 0U) {
        action = UCN_I_GROUP_RX_LOCAL_AND_RELAY;
    } else if (facts->local_member) {
        action = UCN_I_GROUP_RX_LOCAL;
    } else {
        action = UCN_I_GROUP_RX_RELAY;
    }
    receipt->action = action;
    receipt->phase = UCN_I_GROUP_PENDING;
    receipt->occupied = 1U;
    if (required_attempts == 1U && config->tree.valid != 0U) {
        if (reserve_rx_attempt(owner, (uint16_t)rx_index, UINT16_MAX,
                               NULL, UCN_I_GROUP_PLAN_TREE) < 0) {
            ucn_i_group_p_release_rx_attempts(owner, (uint16_t)rx_index,
                                              true);
            memset(receipt, 0, sizeof(*receipt));
            receipt->handle_generation = generation;
            result = UCN_ERR_STATE;
            goto done;
        }
    } else {
        for (index = 0U; index < config->member_count; ++index) {
            if ((facts->relay_member_bitmap &
                 (UINT32_C(1) << index)) != 0U &&
                reserve_rx_attempt(owner, (uint16_t)rx_index, index,
                                   &config->members[index],
                                   UCN_I_GROUP_PLAN_UNICAST) < 0) {
                ucn_i_group_p_release_rx_attempts(owner,
                                                  (uint16_t)rx_index, true);
                memset(receipt, 0, sizeof(*receipt));
                receipt->handle_generation = generation;
                result = UCN_ERR_STATE;
                goto done;
            }
        }
    }
    ucn_i_group_p_make_handle(owner, (uint16_t)rx_index, generation,
                              UCN_OBJECT_KIND_SEND, &reservation);
    memset(&replay, 0, sizeof(replay));
    *reservation_out = reservation;
    *replay_out = replay;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_receive_publish(
    ucn_i_group_owner_t *owner, ucn_handle_t reservation,
    const ucn_i_group_security_commit_t *commit)
{
    ucn_i_group_rx_slot_t *receipt;
    int context_index;
    uint16_t slot;
    uint16_t index;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || commit == NULL ||
        commit->replay_committed > 1U || commit->reserved_zero != 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), commit,
                             sizeof(*commit))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_rx_handle_matches(owner, reservation, &slot)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    receipt = &owner->receipts[slot];
    context_index = find_context(owner, &receipt->facts);
    if (receipt->published || context_index < 0 ||
        commit->send_id != receipt->facts.send_id ||
        commit->group_id != receipt->facts.group_id ||
        commit->group_generation != receipt->facts.group_generation ||
        commit->key_generation != receipt->facts.key_generation ||
        memcmp(commit->payload_digest, receipt->facts.payload_digest,
               UCN_I_GROUP_DIGEST_BYTES) != 0) {
        result = UCN_ERR_SECURITY;
        goto done;
    }
    if ((owner->contexts[context_index].current.secure_required != 0U &&
         (commit->replay_committed != 1U ||
          commit->security_owner_instance !=
              owner->contexts[context_index].current.security.owner_instance)) ||
        (owner->contexts[context_index].current.secure_required == 0U &&
         (commit->replay_committed != 0U ||
          commit->security_owner_instance != 0U))) {
        result = UCN_ERR_SECURITY;
        goto done;
    }
    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        ucn_i_group_attempt_slot_t *attempt = &owner->attempts[index];
        if (attempt->occupied && attempt->parent_is_rx &&
            attempt->parent_slot == slot &&
            attempt->phase == UCN_I_GROUP_ATTEMPT_HELD) {
            attempt->phase = UCN_I_GROUP_ATTEMPT_READY;
        }
    }
    receipt->published = 1U;
    receipt->phase = UCN_I_GROUP_ACTIVE;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_receive_abort(
    ucn_i_group_owner_t *owner, ucn_handle_t reservation)
{
    uint16_t generation;
    uint16_t slot;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner)) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_rx_handle_matches(owner, reservation, &slot) ||
        owner->receipts[slot].published) {
        result = UCN_ERR_STATE;
    } else {
        generation = owner->receipts[slot].handle_generation;
        ucn_i_group_p_release_rx_attempts(owner, slot, true);
        memset(&owner->receipts[slot], 0, sizeof(owner->receipts[slot]));
        owner->receipts[slot].handle_generation = generation;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_receive_complete(
    ucn_i_group_owner_t *owner, ucn_handle_t receipt_handle,
    ucn_result_t application_result)
{
    ucn_i_group_rx_slot_t *receipt;
    uint16_t slot;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || application_result > UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_rx_handle_matches(owner, receipt_handle, &slot)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    receipt = &owner->receipts[slot];
    if (!receipt->published ||
        (receipt->action != UCN_I_GROUP_RX_LOCAL &&
         receipt->action != UCN_I_GROUP_RX_LOCAL_AND_RELAY)) {
        result = UCN_ERR_STATE;
    } else if (receipt->application_complete) {
        result = receipt->application_result == application_result ?
            UCN_OK : UCN_ERR_REPLAY;
    } else {
        receipt->application_result = application_result;
        receipt->application_complete = 1U;
        result = UCN_OK;
    }
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_receive_view(
    ucn_i_group_owner_t *owner, ucn_handle_t receipt_handle,
    ucn_i_group_rx_view_t *view_out)
{
    ucn_i_group_rx_view_t view;
    uint16_t slot;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_rx_handle_matches(owner, receipt_handle, &slot) ||
        !owner->receipts[slot].published) {
        result = UCN_ERR_STATE;
    } else {
        make_rx_view(owner, slot, &view);
        *view_out = view;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_receive_retire(
    ucn_i_group_owner_t *owner, ucn_handle_t receipt_handle)
{
    ucn_i_group_rx_slot_t *receipt;
    uint16_t generation;
    uint16_t slot;
    bool local;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner)) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_rx_handle_matches(owner, receipt_handle, &slot)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    receipt = &owner->receipts[slot];
    local = receipt->action == UCN_I_GROUP_RX_LOCAL ||
            receipt->action == UCN_I_GROUP_RX_LOCAL_AND_RELAY;
    if (!receipt->published || (local && !receipt->application_complete) ||
        !rx_attempts_terminal(owner, slot)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    generation = receipt->handle_generation;
    ucn_i_group_p_release_rx_attempts(owner, slot, false);
    memset(receipt, 0, sizeof(*receipt));
    receipt->handle_generation = generation;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}
