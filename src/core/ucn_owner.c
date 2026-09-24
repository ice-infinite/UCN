#include "internal/ucn_owner.h"

#include "internal/ucn_checked.h"

#include <limits.h>
#include <string.h>

#define UCN_I_OWNER_MAILBOX_MAGIC UINT32_C(0x55434D42)

static bool bytes_are_zero(const void *object, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool lock_ops_are_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->enter != NULL && lock->leave != NULL;
}

static bool lock_context_overlaps_object(const ucn_i_lock_ops_t *lock,
                                         const void *object,
                                         size_t object_size)
{
    return lock != NULL && lock->context != NULL &&
           ucn_i_ranges_overlap(object, object_size, lock->context, 1U);
}

static bool claim_is_valid(const ucn_i_callback_claim_t *claim)
{
    return claim != NULL && claim->owner_instance != 0U &&
           claim->operation_id != 0U && claim->operation_generation != 0U &&
           claim->operation_kind != 0U;
}

static bool claims_are_equal(const ucn_i_callback_claim_t *left,
                             const ucn_i_callback_claim_t *right)
{
    return left->owner_instance == right->owner_instance &&
           left->operation_id == right->operation_id &&
           left->operation_generation == right->operation_generation &&
           left->operation_kind == right->operation_kind;
}

static bool callback_gate_header_is_valid(const ucn_i_callback_gate_t *gate)
{
    return gate != NULL && gate->magic == UCN_I_CALLBACK_GATE_MAGIC &&
           gate->gate_instance != 0U &&
           gate->schema == UCN_I_CALLBACK_GATE_SCHEMA &&
           gate->reserved_zero == 0U && lock_ops_are_valid(&gate->lock);
}

static bool callback_gate_is_valid_locked(const ucn_i_callback_gate_t *gate)
{
    return callback_gate_header_is_valid(gate) &&
           (bytes_are_zero(&gate->active_claim, sizeof(gate->active_claim)) ||
            claim_is_valid(&gate->active_claim));
}

ucn_result_t ucn_i_callback_gate_init(ucn_i_callback_gate_t *gate,
                                      uint32_t gate_instance,
                                      const ucn_i_lock_ops_t *lock)
{
    ucn_i_callback_gate_t initialized;

    if (gate == NULL || !lock_ops_are_valid(lock) || gate_instance == 0U ||
        ucn_i_ranges_overlap(gate, sizeof(*gate), lock, sizeof(*lock)) ||
        lock_context_overlaps_object(lock, gate, sizeof(*gate))) {
        return UCN_ERR_ARGUMENT;
    }
    if (!bytes_are_zero(gate, sizeof(*gate))) {
        return UCN_ERR_STATE;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_I_CALLBACK_GATE_MAGIC;
    initialized.gate_instance = gate_instance;
    initialized.schema = UCN_I_CALLBACK_GATE_SCHEMA;
    initialized.lock = *lock;
    *gate = initialized;
    return UCN_OK;
}

ucn_result_t ucn_i_callback_gate_enter(
    ucn_i_callback_gate_t *gate,
    const ucn_i_callback_claim_t *claim)
{
    ucn_result_t result;

    if (!callback_gate_header_is_valid(gate) || !claim_is_valid(claim) ||
        ucn_i_ranges_overlap(gate, sizeof(*gate), claim, sizeof(*claim))) {
        return UCN_ERR_ARGUMENT;
    }
    result = gate->lock.enter(gate->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!callback_gate_is_valid_locked(gate) ||
        !bytes_are_zero(&gate->active_claim, sizeof(gate->active_claim))) {
        gate->lock.leave(gate->lock.context);
        return UCN_ERR_STATE;
    }
    gate->active_claim = *claim;
    gate->lock.leave(gate->lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_callback_gate_leave(
    ucn_i_callback_gate_t *gate,
    const ucn_i_callback_claim_t *claim)
{
    ucn_result_t result;

    if (!callback_gate_header_is_valid(gate) || !claim_is_valid(claim) ||
        ucn_i_ranges_overlap(gate, sizeof(*gate), claim, sizeof(*claim))) {
        return UCN_ERR_ARGUMENT;
    }
    result = gate->lock.enter(gate->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!callback_gate_is_valid_locked(gate) ||
        !claims_are_equal(&gate->active_claim, claim)) {
        gate->lock.leave(gate->lock.context);
        return UCN_ERR_STATE;
    }
    memset(&gate->active_claim, 0, sizeof(gate->active_claim));
    gate->lock.leave(gate->lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_callback_gate_view(
    ucn_i_callback_gate_t *gate,
    bool *active_out,
    ucn_i_callback_claim_t *claim_out)
{
    ucn_i_callback_claim_t claim;
    bool active;
    ucn_result_t result;

    if (!callback_gate_header_is_valid(gate) || active_out == NULL ||
        claim_out == NULL ||
        ucn_i_ranges_overlap(gate, sizeof(*gate), active_out,
                             sizeof(*active_out)) ||
        ucn_i_ranges_overlap(gate, sizeof(*gate), claim_out,
                             sizeof(*claim_out)) ||
        ucn_i_ranges_overlap(active_out, sizeof(*active_out), claim_out,
                             sizeof(*claim_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = gate->lock.enter(gate->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!callback_gate_is_valid_locked(gate)) {
        gate->lock.leave(gate->lock.context);
        return UCN_ERR_STATE;
    }
    claim = gate->active_claim;
    active = !bytes_are_zero(&claim, sizeof(claim));
    gate->lock.leave(gate->lock.context);
    *active_out = active;
    *claim_out = claim;
    return UCN_OK;
}

ucn_result_t ucn_i_callback_gate_destroy(ucn_i_callback_gate_t *gate)
{
    ucn_i_lock_ops_t lock;
    ucn_result_t result;

    if (!callback_gate_header_is_valid(gate)) {
        return UCN_ERR_ARGUMENT;
    }
    result = gate->lock.enter(gate->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!callback_gate_is_valid_locked(gate) ||
        !bytes_are_zero(&gate->active_claim, sizeof(gate->active_claim))) {
        gate->lock.leave(gate->lock.context);
        return UCN_ERR_STATE;
    }
    lock = gate->lock;
    memset(gate, 0, sizeof(*gate));
    lock.leave(lock.context);
    return UCN_OK;
}

static bool owner_mailbox_header_is_valid(const ucn_i_owner_mailbox_t *mailbox)
{
    return mailbox != NULL && mailbox->magic == UCN_I_OWNER_MAILBOX_MAGIC &&
           mailbox->schema == UCN_I_OWNER_MAILBOX_SCHEMA &&
           mailbox->class_count != 0U &&
           mailbox->class_count <= UCN_I_OWNER_WORK_CLASS_LIMIT &&
           lock_ops_are_valid(&mailbox->lock);
}

static bool owner_mailbox_is_valid_locked(const ucn_i_owner_mailbox_t *mailbox)
{
    uint8_t index;
    uint32_t valid_mask;

    if (!owner_mailbox_header_is_valid(mailbox) ||
        mailbox->cursor >= mailbox->class_count) {
        return false;
    }
    valid_mask = (UINT32_C(1) << mailbox->class_count) - UINT32_C(1);
    if ((mailbox->pending_mask & ~valid_mask) != 0U) {
        return false;
    }
    for (index = 0U; index < UCN_I_OWNER_WORK_CLASS_LIMIT; ++index) {
        bool is_pending = (mailbox->pending_mask &
                           (UINT32_C(1) << index)) != 0U;
        if (index < mailbox->class_count) {
            if (is_pending != (mailbox->occurrences[index] != 0U)) {
                return false;
            }
        } else if (mailbox->occurrences[index] != 0U) {
            return false;
        }
    }
    return true;
}

ucn_result_t ucn_i_owner_mailbox_init(ucn_i_owner_mailbox_t *mailbox,
                                      uint8_t class_count,
                                      const ucn_i_lock_ops_t *lock)
{
    ucn_i_owner_mailbox_t initialized;

    if (mailbox == NULL || !lock_ops_are_valid(lock) || class_count == 0U ||
        class_count > UCN_I_OWNER_WORK_CLASS_LIMIT ||
        ucn_i_ranges_overlap(mailbox, sizeof(*mailbox), lock, sizeof(*lock)) ||
        lock_context_overlaps_object(lock, mailbox, sizeof(*mailbox))) {
        return UCN_ERR_ARGUMENT;
    }
    if (!bytes_are_zero(mailbox, sizeof(*mailbox))) {
        return UCN_ERR_STATE;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_I_OWNER_MAILBOX_MAGIC;
    initialized.schema = UCN_I_OWNER_MAILBOX_SCHEMA;
    initialized.class_count = class_count;
    initialized.lock = *lock;
    *mailbox = initialized;
    return UCN_OK;
}

ucn_result_t ucn_i_owner_mailbox_publish(
    ucn_i_owner_mailbox_t *mailbox,
    ucn_i_owner_work_class_t work_class)
{
    uint32_t bit;
    ucn_result_t result;

    if (!owner_mailbox_header_is_valid(mailbox) ||
        work_class >= mailbox->class_count) {
        return UCN_ERR_ARGUMENT;
    }
    result = mailbox->lock.enter(mailbox->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!owner_mailbox_is_valid_locked(mailbox)) {
        mailbox->lock.leave(mailbox->lock.context);
        return UCN_ERR_STATE;
    }
    bit = UINT32_C(1) << work_class;
    mailbox->pending_mask |= bit;
    if (mailbox->occurrences[work_class] != UINT32_MAX) {
        ++mailbox->occurrences[work_class];
    }
    mailbox->lock.leave(mailbox->lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_owner_mailbox_take(
    ucn_i_owner_mailbox_t *mailbox,
    ucn_i_owner_work_hint_t *hint_out)
{
    ucn_i_owner_work_hint_t hint;
    uint8_t offset;
    ucn_result_t result;

    if (!owner_mailbox_header_is_valid(mailbox) || hint_out == NULL ||
        ucn_i_ranges_overlap(mailbox, sizeof(*mailbox), hint_out,
                             sizeof(*hint_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = mailbox->lock.enter(mailbox->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!owner_mailbox_is_valid_locked(mailbox)) {
        mailbox->lock.leave(mailbox->lock.context);
        return UCN_ERR_STATE;
    }
    for (offset = 0U; offset < mailbox->class_count; ++offset) {
        uint8_t work_class = (uint8_t)(mailbox->cursor + offset);
        uint32_t bit;

        if (work_class >= mailbox->class_count) {
            work_class = (uint8_t)(work_class - mailbox->class_count);
        }
        bit = UINT32_C(1) << work_class;
        if ((mailbox->pending_mask & bit) != 0U) {
            memset(&hint, 0, sizeof(hint));
            hint.work_class = work_class;
            hint.occurrences = mailbox->occurrences[work_class];
            mailbox->pending_mask &= ~bit;
            mailbox->occurrences[work_class] = 0U;
            mailbox->cursor = (uint8_t)(work_class + 1U);
            if (mailbox->cursor == mailbox->class_count) {
                mailbox->cursor = 0U;
            }
            mailbox->lock.leave(mailbox->lock.context);
            *hint_out = hint;
            return UCN_OK;
        }
    }
    mailbox->lock.leave(mailbox->lock.context);
    return UCN_ERR_NOT_FOUND;
}

ucn_result_t ucn_i_owner_mailbox_destroy(ucn_i_owner_mailbox_t *mailbox)
{
    ucn_i_lock_ops_t lock;
    ucn_result_t result;

    if (!owner_mailbox_header_is_valid(mailbox)) {
        return UCN_ERR_ARGUMENT;
    }
    result = mailbox->lock.enter(mailbox->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!owner_mailbox_is_valid_locked(mailbox) ||
        mailbox->pending_mask != 0U) {
        mailbox->lock.leave(mailbox->lock.context);
        return UCN_ERR_STATE;
    }
    lock = mailbox->lock;
    memset(mailbox, 0, sizeof(*mailbox));
    lock.leave(lock.context);
    return UCN_OK;
}
