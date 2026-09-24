#include "internal/ucn_group.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool dependency_event_valid(
    const ucn_i_group_dependency_event_t *event)
{
    return event != NULL && event->group_id != 0U &&
           event->group_generation != 0U && event->policy_generation != 0U &&
           event->member_generation != 0U &&
           event->security_current <= 1U && event->tree_current <= 1U &&
           event->authority_current <= 1U && event->reserved_zero == 0U;
}

static void cancel_group_attempts(ucn_i_group_owner_t *owner,
                                  uint32_t group_id)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_ATTEMPT_COUNT; ++index) {
        ucn_i_group_attempt_slot_t *attempt = &owner->attempts[index];
        if (attempt->occupied && attempt->group_id == group_id &&
            (attempt->phase == UCN_I_GROUP_ATTEMPT_HELD ||
             attempt->phase == UCN_I_GROUP_ATTEMPT_READY)) {
            attempt->phase = UCN_I_GROUP_ATTEMPT_CANCELLED;
        }
    }
}

static void fail_group_sends(ucn_i_group_owner_t *owner,
                             uint32_t group_id)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_SEND_COUNT; ++index) {
        if (owner->sends[index].occupied &&
            owner->sends[index].group_id == group_id &&
            owner->sends[index].phase == UCN_I_GROUP_SEND_ACTIVE) {
            owner->sends[index].phase = UCN_I_GROUP_SEND_FAILED;
        }
    }
}

ucn_result_t ucn_i_group_dependency_change(
    ucn_i_group_owner_t *owner,
    const ucn_i_group_dependency_event_t *event)
{
    ucn_i_group_context_slot_t *slot = NULL;
    uint16_t index;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) ||
        !dependency_event_valid(event) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), event, sizeof(*event))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    for (index = 0U; index < UCN_I_GROUP_CONTEXT_COUNT; ++index) {
        if (owner->contexts[index].occupied &&
            owner->contexts[index].current.group_id == event->group_id) {
            slot = &owner->contexts[index];
            break;
        }
    }
    if (slot == NULL || slot->phase != UCN_I_GROUP_ACTIVE) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    if (slot->current.group_generation == event->group_generation &&
        slot->current.policy_generation == event->policy_generation &&
        slot->current.member_generation == event->member_generation &&
        slot->current.security.key_generation == event->key_generation &&
        slot->current.tree.tree_generation == event->tree_generation &&
        (slot->current.secure_required == 0U ||
         event->security_current == 1U) &&
        (slot->current.tree.valid == 0U || event->tree_current == 1U) &&
        (slot->current.mode == UCN_I_GROUP_STATIC ||
         event->authority_current == 1U)) {
        result = UCN_OK;
        goto done;
    }
    slot->phase = UCN_I_GROUP_FENCED;
    fail_group_sends(owner, event->group_id);
    cancel_group_attempts(owner, event->group_id);
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

static bool expire_send(ucn_i_group_owner_t *owner, uint16_t index,
                        uint64_t now_us)
{
    ucn_i_group_send_slot_t *send = &owner->sends[index];
    uint16_t attempt_index;

    if (!send->occupied || send->phase != UCN_I_GROUP_SEND_ACTIVE ||
        !ucn_i_deadline_expired_us(now_us, send->deadline_us)) {
        return false;
    }
    send->phase = UCN_I_GROUP_SEND_FAILED;
    for (attempt_index = 0U;
         attempt_index < UCN_I_GROUP_ATTEMPT_COUNT; ++attempt_index) {
        ucn_i_group_attempt_slot_t *attempt = &owner->attempts[attempt_index];
        if (attempt->occupied && !attempt->parent_is_rx &&
            attempt->parent_slot == index &&
            attempt->phase == UCN_I_GROUP_ATTEMPT_READY) {
            attempt->phase = UCN_I_GROUP_ATTEMPT_CANCELLED;
        }
    }
    return true;
}

static bool expire_rx(ucn_i_group_owner_t *owner, uint16_t index,
                      uint64_t now_us)
{
    ucn_i_group_rx_slot_t *receipt = &owner->receipts[index];
    uint16_t generation;

    if (!receipt->occupied || receipt->published ||
        !ucn_i_deadline_expired_us(now_us,
                                   receipt->facts.absolute_deadline_us)) {
        return false;
    }
    generation = receipt->handle_generation;
    ucn_i_group_p_release_rx_attempts(owner, index, true);
    memset(receipt, 0, sizeof(*receipt));
    receipt->handle_generation = generation;
    return true;
}

ucn_result_t ucn_i_group_step(ucn_i_group_owner_t *owner,
                              uint64_t now_us, uint16_t budget,
                              uint16_t *inspected_out,
                              uint16_t *changed_out)
{
    const uint16_t total = UCN_I_GROUP_SEND_COUNT + UCN_I_GROUP_RX_COUNT;
    uint16_t inspected = 0U;
    uint16_t changed = 0U;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || budget == 0U ||
        inspected_out == NULL || changed_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), changed_out,
                             sizeof(*changed_out)) ||
        ucn_i_ranges_overlap(inspected_out, sizeof(*inspected_out),
                             changed_out, sizeof(*changed_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    while (inspected < budget && inspected < total) {
        uint16_t cursor = owner->step_cursor;
        bool did_change;

        if (cursor < UCN_I_GROUP_SEND_COUNT) {
            did_change = expire_send(owner, cursor, now_us);
        } else {
            did_change = expire_rx(owner,
                (uint16_t)(cursor - UCN_I_GROUP_SEND_COUNT), now_us);
        }
        if (did_change) changed++;
        owner->step_cursor = (uint16_t)((cursor + 1U) % total);
        inspected++;
    }
    *inspected_out = inspected;
    *changed_out = changed;
    owner->state_lock.leave(owner->state_lock.context);
    return UCN_OK;
}
