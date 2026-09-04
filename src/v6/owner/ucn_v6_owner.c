#include "../internal/ucn_v6_owner_private.h"

#include <string.h>

typedef char ucn_v6_protocol_owner_storage_size_check[
    sizeof(ucn_v6_protocol_owner_t) <= UCN_V6_PROTOCOL_OWNER_STORAGE_BYTES ?
        1 : -1];

static bool event_is_valid(ucn_v6_owner_event_t event)
{
    return event >= UCN_V6_OWNER_EVENT_RX &&
           event <= UCN_V6_OWNER_EVENT_PROVIDER;
}

static bool lock_ops_are_valid(const ucn_v6_owner_lock_ops_t *ops)
{
    return ops != NULL && ops->try_lock != NULL && ops->unlock != NULL &&
           ops->notify != NULL;
}

static bool owner_storage_is_valid(const ucn_v6_protocol_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_PROTOCOL_OWNER_MAGIC &&
           owner->schema == UCN_V6_STORAGE_LAYOUT &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->canary == UCN_V6_PROTOCOL_OWNER_CANARY &&
           owner->event_depth != 0U;
}

ucn_v6_result_t ucn_v6_protocol_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_owner_lock_ops_t *lock_ops,
    ucn_v6_protocol_owner_t **owner_out)
{
    ucn_v6_protocol_owner_t *owner;
    ucn_v6_result_t result;

    if (owner_out == NULL || !lock_ops_are_valid(lock_ops)) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(*owner),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    if (!lock_ops->try_lock(lock_ops->context, false)) {
        return UCN_V6_ERR_STATE;
    }
    owner = (ucn_v6_protocol_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_PROTOCOL_OWNER_MAGIC;
    owner->schema = UCN_V6_STORAGE_LAYOUT;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->lock_ops = *lock_ops;
    owner->event_depth = manifest->owner_event_depth;
    owner->canary = UCN_V6_PROTOCOL_OWNER_CANARY;
    *owner_out = owner;
    lock_ops->unlock(lock_ops->context, false);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_protocol_owner_post(
    ucn_v6_protocol_owner_t *owner,
    ucn_v6_owner_event_t event,
    bool from_isr)
{
    size_t index;
    ucn_v6_owner_lock_ops_t ops;

    if (owner == NULL || !event_is_valid(event)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_storage_is_valid(owner)) {
        return UCN_V6_ERR_STATE;
    }
    ops = owner->lock_ops;
    if (!lock_ops_are_valid(&ops) || !ops.try_lock(ops.context, from_isr)) {
        return UCN_V6_ERR_STATE;
    }
    if (owner->faulted) {
        ops.unlock(ops.context, from_isr);
        return UCN_V6_ERR_STATE;
    }
    if (owner->pending_total >= owner->event_depth) {
        ops.unlock(ops.context, from_isr);
        return UCN_V6_ERR_NO_SPACE;
    }
    index = (size_t)event - 1U;
    if (owner->pending_by_event[index] == UINT16_MAX) {
        owner->faulted = true;
        ops.unlock(ops.context, from_isr);
        return UCN_V6_ERR_EXHAUSTED;
    }
    ++owner->pending_by_event[index];
    ++owner->pending_total;
    ops.unlock(ops.context, from_isr);
    ops.notify(ops.context, from_isr);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_protocol_owner_run(
    ucn_v6_protocol_owner_t *owner,
    uint16_t budget,
    ucn_v6_owner_event_handler_t handler,
    void *handler_context,
    uint16_t *processed)
{
    uint16_t count = 0U;
    ucn_v6_result_t result = UCN_V6_OK;
    ucn_v6_owner_lock_ops_t ops;

    if (owner == NULL || budget == 0U || handler == NULL ||
        processed == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_storage_is_valid(owner)) {
        return UCN_V6_ERR_STATE;
    }
    ops = owner->lock_ops;
    if (!lock_ops_are_valid(&ops) ||
        !ops.try_lock(ops.context, false)) {
        return UCN_V6_ERR_STATE;
    }
    if (!owner_storage_is_valid(owner) || owner->faulted || owner->running) {
        ops.unlock(ops.context, false);
        return UCN_V6_ERR_STATE;
    }
    owner->running = true;
    ops.unlock(ops.context, false);

    while (count < budget) {
        size_t offset;
        size_t selected = UCN_V6_OWNER_EVENT_COUNT;

        if (!ops.try_lock(ops.context, false)) {
            result = UCN_V6_ERR_STATE;
            break;
        }
        if (!owner_storage_is_valid(owner) || owner->faulted) {
            ops.unlock(ops.context, false);
            result = UCN_V6_ERR_STATE;
            break;
        }
        for (offset = 0U; offset < UCN_V6_OWNER_EVENT_COUNT; ++offset) {
            size_t candidate =
                ((size_t)owner->next_event_index + offset) %
                UCN_V6_OWNER_EVENT_COUNT;
            if (owner->pending_by_event[candidate] != 0U) {
                selected = candidate;
                break;
            }
        }
        ops.unlock(ops.context, false);
        if (selected == UCN_V6_OWNER_EVENT_COUNT) {
            break;
        }

        result = handler(handler_context,
                         (ucn_v6_owner_event_t)(selected + 1U));
        if (result != UCN_V6_OK) {
            break;
        }
        if (!ops.try_lock(ops.context, false)) {
            result = UCN_V6_ERR_STATE;
            break;
        }
        if (!owner_storage_is_valid(owner) ||
            owner->pending_by_event[selected] == 0U ||
            owner->pending_total == 0U) {
            owner->faulted = true;
            ops.unlock(ops.context, false);
            result = UCN_V6_ERR_STATE;
            break;
        }
        --owner->pending_by_event[selected];
        --owner->pending_total;
        owner->next_event_index =
            (uint8_t)((selected + 1U) % UCN_V6_OWNER_EVENT_COUNT);
        ops.unlock(ops.context, false);
        ++count;
    }

    if (!ops.try_lock(ops.context, false)) {
        return UCN_V6_ERR_STATE;
    }
    if (owner_storage_is_valid(owner)) {
        owner->running = false;
    }
    ops.unlock(ops.context, false);
    *processed = count;
    return result;
}

ucn_v6_result_t ucn_v6_protocol_owner_copy_view(
    ucn_v6_protocol_owner_t *owner,
    ucn_v6_protocol_owner_view_t *view)
{
    ucn_v6_protocol_owner_view_t next;
    ucn_v6_owner_lock_ops_t ops;
    size_t index;

    if (owner == NULL || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_storage_is_valid(owner)) {
        return UCN_V6_ERR_STATE;
    }
    ops = owner->lock_ops;
    if (!lock_ops_are_valid(&ops) || !ops.try_lock(ops.context, false)) {
        return UCN_V6_ERR_STATE;
    }
    if (!owner_storage_is_valid(owner)) {
        ops.unlock(ops.context, false);
        return UCN_V6_ERR_STATE;
    }
    memset(&next, 0, sizeof(next));
    next.pending_total = owner->pending_total;
    next.next_event_index = owner->next_event_index;
    next.running = owner->running;
    next.faulted = owner->faulted;
    for (index = 0U; index < UCN_V6_OWNER_EVENT_COUNT; ++index) {
        next.pending_by_event[index] = owner->pending_by_event[index];
    }
    ops.unlock(ops.context, false);
    *view = next;
    return UCN_V6_OK;
}
