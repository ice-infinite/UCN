#include "../internal/ucn_v6_adapter_private.h"

#include <string.h>

typedef char ucn_v6_adapter_storage_size_check[
    sizeof(ucn_v6_adapter_owner_t) <= UCN_V6_ADAPTER_OWNER_STORAGE_BYTES ?
        1 : -1];

static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) ++*value;
}

static bool serial_valid(uint32_t value)
{
    return value != 0U && value <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool result_valid(ucn_v6_result_t result)
{
    return (int)result >= (int)UCN_V6_ERR_CANCELLED &&
           (int)result <= (int)UCN_V6_OK;
}

static bool key_equal(
    const ucn_v6_driver_event_key_t *left,
    const ucn_v6_driver_event_key_t *right)
{
    return left->link_id == right->link_id &&
           left->link_generation == right->link_generation &&
           left->event_token == right->event_token;
}

static bool timestamp_is_canonical(
    const ucn_v6_driver_timestamp_t *timestamp, bool hardware_supported)
{
    if (timestamp == NULL) return true;
    if (!timestamp->valid) {
        return timestamp->timestamp_us == 0U &&
               timestamp->uncertainty_us == 0U && !timestamp->hardware;
    }
    return timestamp->uncertainty_us != 0U &&
           (!timestamp->hardware || hardware_supported);
}

static bool runtime_ops_valid(const ucn_v6_driver_runtime_ops_t *ops)
{
    return ops != NULL && ops->lock_task != NULL &&
           ops->try_lock_from_isr != NULL && ops->unlock_task != NULL &&
           ops->unlock_from_isr != NULL &&
           ops->post_owner_event != NULL;
}

static bool link_ops_valid(const ucn_v6_driver_link_ops_t *ops)
{
    return ops != NULL &&
           ops->struct_size == sizeof(ucn_v6_driver_link_ops_t) &&
           ops->api_version == UCN_V6_ADAPTER_API_VERSION &&
           ops->submit != NULL && ops->quiesce != NULL;
}

static bool owner_valid(const ucn_v6_adapter_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_ADAPTER_MAGIC &&
           owner->schema == UCN_V6_ADAPTER_SCHEMA &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->initialized && owner->canary == UCN_V6_ADAPTER_CANARY;
}

static bool lock_owner(ucn_v6_adapter_owner_t *owner, bool from_isr)
{
    if (!owner_valid(owner)) return false;
    if (from_isr) {
        return owner->runtime.try_lock_from_isr(owner->runtime.context);
    }
    return owner->runtime.lock_task(owner->runtime.context) == UCN_V6_OK;
}

static void unlock_owner(ucn_v6_adapter_owner_t *owner, bool from_isr)
{
    if (from_isr) owner->runtime.unlock_from_isr(owner->runtime.context);
    else owner->runtime.unlock_task(owner->runtime.context);
}

static ucn_v6_adapter_link_slot_t *find_link(
    ucn_v6_adapter_owner_t *owner, uint16_t link_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_LINKS; ++index) {
        if (owner->links[index].occupied &&
            owner->links[index].config.link_id == link_id) {
            return &owner->links[index];
        }
    }
    return NULL;
}

static bool any_io_active(const ucn_v6_adapter_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_LINKS; ++index) {
        if (owner->links[index].occupied && owner->links[index].io_active) {
            return true;
        }
    }
    return false;
}

static ucn_v6_result_t allocate_key(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_adapter_link_slot_t *link,
    ucn_v6_driver_event_key_t *key)
{
    if (owner->next_event_token == 0U ||
        owner->next_event_token > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        owner->stats.faulted = true;
        return UCN_V6_ERR_EXHAUSTED;
    }
    key->link_id = link->config.link_id;
    key->link_generation = link->config.initial_generation;
    key->event_token = owner->next_event_token++;
    return UCN_V6_OK;
}

static void post_event(
    ucn_v6_adapter_owner_t *owner,
    ucn_v6_owner_event_t event,
    bool from_isr)
{
    if (owner->runtime.post_owner_event(owner->runtime.context, event,
                                        from_isr) != UCN_V6_OK &&
        lock_owner(owner, from_isr)) {
        increment_saturated(&owner->stats.notification_failures);
        unlock_owner(owner, from_isr);
    }
}

ucn_v6_result_t ucn_v6_adapter_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_driver_runtime_ops_t *runtime_ops,
    ucn_v6_adapter_owner_t **owner_out)
{
    ucn_v6_adapter_owner_t *owner;
    ucn_v6_result_t result;
    if (owner_out != NULL) *owner_out = NULL;
    if (owner_out == NULL || !runtime_ops_valid(runtime_ops) ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        (manifest->feature_bits & UCN_V6_FEATURE_ADAPTER) == 0U) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     UCN_V6_ADAPTER_OWNER_STORAGE_BYTES,
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) return result;
    if (runtime_ops->lock_task(runtime_ops->context) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    memset(storage, 0, storage_bytes);
    owner = (ucn_v6_adapter_owner_t *)storage;
    owner->magic = UCN_V6_ADAPTER_MAGIC;
    owner->schema = UCN_V6_ADAPTER_SCHEMA;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->runtime = *runtime_ops;
    owner->next_order = 1U;
    owner->next_event_token = 1U;
    owner->initialized = true;
    owner->canary = UCN_V6_ADAPTER_CANARY;
    *owner_out = owner;
    runtime_ops->unlock_task(runtime_ops->context);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_adapter_register_link(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_link_config_t *config)
{
    size_t index;
    ucn_v6_adapter_link_slot_t *free_slot = NULL;
    if (!owner_valid(owner) || config == NULL || config->link_id == 0U ||
        !serial_valid(config->initial_generation) ||
        config->bearer < UCN_V6_BEARER_UART ||
        (config->bearer > UCN_V6_BEARER_USB &&
         config->bearer != UCN_V6_BEARER_CUSTOM) ||
        config->nominal_bitrate_bps == 0U || config->carrier_mtu == 0U ||
        config->link_frame_mtu < config->carrier_mtu ||
        config->link_frame_mtu > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES ||
        config->hardware_priority_count == 0U ||
        config->hardware_priority_count > 8U ||
        config->rx_slot_quota == 0U ||
        config->rx_slot_quota > UCN_V6_CONFIG_ADAPTER_RX_SLOTS ||
        config->tx_slot_quota == 0U ||
        config->tx_slot_quota > UCN_V6_CONFIG_ADAPTER_TX_SLOTS ||
        !link_ops_valid(&config->ops)) {
        return UCN_V6_ERR_CONFIG;
    }
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    if (any_io_active(owner) || owner->stats.faulted) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_LINKS; ++index) {
        if (owner->links[index].occupied &&
            owner->links[index].config.link_id == config->link_id) {
            unlock_owner(owner, false);
            return UCN_V6_ERR_REPLAY;
        }
        if (!owner->links[index].occupied && free_slot == NULL) {
            free_slot = &owner->links[index];
        }
    }
    if (free_slot == NULL) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->readiness = UCN_V6_LINK_STARTING;
    free_slot->config = *config;
    unlock_owner(owner, false);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_adapter_set_link_readiness(
    ucn_v6_adapter_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation,
    ucn_v6_driver_link_readiness_t readiness)
{
    ucn_v6_adapter_link_slot_t *link;
    bool allowed = false;
    bool changed;
    if (!owner_valid(owner) ||
        (readiness != UCN_V6_LINK_READY &&
         readiness != UCN_V6_LINK_OFFLINE &&
         readiness != UCN_V6_LINK_FAULTED)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    link = find_link(owner, link_id);
    if (link == NULL || link->config.initial_generation != link_generation ||
        link->io_active) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    changed = link->readiness != readiness;
    if (!changed) allowed = true;
    else if (link->readiness == UCN_V6_LINK_STARTING &&
             readiness == UCN_V6_LINK_READY) allowed = true;
    else if (link->readiness == UCN_V6_LINK_READY &&
             (readiness == UCN_V6_LINK_OFFLINE ||
              readiness == UCN_V6_LINK_FAULTED)) allowed = true;
    if (!allowed) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    link->readiness = readiness;
    unlock_owner(owner, false);
    if (changed) post_event(owner, UCN_V6_OWNER_EVENT_PROVIDER, false);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_adapter_publish_rx(
    ucn_v6_adapter_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation,
    const uint8_t *frame,
    size_t frame_length,
    const ucn_v6_driver_timestamp_t *timestamp,
    bool from_isr,
    ucn_v6_driver_event_key_t *published_key)
{
    ucn_v6_adapter_link_slot_t *link;
    ucn_v6_adapter_rx_slot_t *slot = NULL;
    ucn_v6_driver_event_key_t key;
    size_t index;
    if (!owner_valid(owner) || frame == NULL || frame_length == 0U ||
        published_key == NULL) return UCN_V6_ERR_ARGUMENT;
    if (!lock_owner(owner, from_isr)) return UCN_V6_ERR_STATE;
    link = find_link(owner, link_id);
    if (link == NULL) {
        unlock_owner(owner, from_isr);
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (link->config.initial_generation != link_generation) {
        increment_saturated(&owner->stats.rx_stale);
        unlock_owner(owner, from_isr);
        return UCN_V6_ERR_REPLAY;
    }
    if (link->readiness != UCN_V6_LINK_READY) {
        unlock_owner(owner, from_isr);
        return UCN_V6_ERR_STATE;
    }
    if (frame_length > link->config.link_frame_mtu ||
        frame_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES) {
        unlock_owner(owner, from_isr);
        return UCN_V6_ERR_MALFORMED;
    }
    if (!timestamp_is_canonical(
            timestamp, link->config.rx_timestamp_hardware)) {
        unlock_owner(owner, from_isr);
        return UCN_V6_ERR_MALFORMED;
    }
    if (link->rx_count >= link->config.rx_slot_quota) {
        increment_saturated(&owner->stats.rx_dropped_full);
        unlock_owner(owner, from_isr);
        return UCN_V6_ERR_NO_SPACE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_RX_SLOTS; ++index) {
        if (!owner->rx[index].occupied) {
            slot = &owner->rx[index];
            break;
        }
    }
    if (slot == NULL || allocate_key(owner, link, &key) != UCN_V6_OK ||
        owner->next_order == 0U) {
        if (slot == NULL) increment_saturated(&owner->stats.rx_dropped_full);
        else owner->stats.faulted = true;
        unlock_owner(owner, from_isr);
        return slot == NULL ? UCN_V6_ERR_NO_SPACE : UCN_V6_ERR_EXHAUSTED;
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->order = owner->next_order++;
    slot->key = key;
    slot->bearer = link->config.bearer;
    slot->frame_length = (uint16_t)frame_length;
    if (timestamp != NULL) slot->timestamp = *timestamp;
    memcpy(slot->frame, frame, frame_length);
    ++link->rx_count;
    increment_saturated(&owner->stats.rx_published);
    *published_key = key;
    unlock_owner(owner, from_isr);
    post_event(owner, UCN_V6_OWNER_EVENT_RX, from_isr);
    return UCN_V6_OK;
}

static ucn_v6_adapter_rx_slot_t *oldest_rx(ucn_v6_adapter_owner_t *owner)
{
    ucn_v6_adapter_rx_slot_t *selected = NULL;
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_RX_SLOTS; ++index) {
        if (owner->rx[index].occupied &&
            (selected == NULL || owner->rx[index].order < selected->order)) {
            selected = &owner->rx[index];
        }
    }
    return selected;
}

ucn_v6_result_t ucn_v6_adapter_peek_rx(
    ucn_v6_adapter_owner_t *owner,
    uint8_t *frame,
    size_t frame_capacity,
    ucn_v6_driver_rx_view_t *view)
{
    ucn_v6_adapter_rx_slot_t *slot;
    ucn_v6_driver_rx_view_t next;
    if (!owner_valid(owner) || frame == NULL || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    slot = oldest_rx(owner);
    if (slot == NULL) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (frame_capacity < slot->frame_length) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(&next, 0, sizeof(next));
    next.key = slot->key;
    next.timestamp = slot->timestamp;
    next.bearer = slot->bearer;
    next.frame_length = slot->frame_length;
    memcpy(frame, slot->frame, slot->frame_length);
    *view = next;
    unlock_owner(owner, false);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_adapter_retire_rx(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_event_key_t *key)
{
    size_t index;
    if (!owner_valid(owner) || key == NULL) return UCN_V6_ERR_ARGUMENT;
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_RX_SLOTS; ++index) {
        ucn_v6_adapter_rx_slot_t *slot = &owner->rx[index];
        if (slot->occupied && key_equal(&slot->key, key)) {
            ucn_v6_adapter_link_slot_t *link = find_link(owner, key->link_id);
            if (link == NULL || link->rx_count == 0U) {
                owner->stats.faulted = true;
                unlock_owner(owner, false);
                return UCN_V6_ERR_STATE;
            }
            --link->rx_count;
            memset(slot, 0, sizeof(*slot));
            increment_saturated(&owner->stats.rx_retired);
            unlock_owner(owner, false);
            return UCN_V6_OK;
        }
    }
    unlock_owner(owner, false);
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_adapter_enqueue_tx(
    ucn_v6_adapter_owner_t *owner,
    uint16_t link_id,
    uint64_t buffer_token,
    const uint8_t *frame,
    size_t frame_length,
    ucn_v6_traffic_class_t traffic_class,
    bool request_timestamp,
    ucn_v6_driver_event_key_t *key)
{
    ucn_v6_adapter_link_slot_t *link;
    ucn_v6_adapter_tx_slot_t *slot = NULL;
    ucn_v6_driver_event_key_t next_key;
    size_t index;
    if (!owner_valid(owner) || buffer_token == 0U || frame == NULL ||
        frame_length == 0U || key == NULL ||
        (uint32_t)traffic_class > (uint32_t)UCN_V6_TRAFFIC_Q3) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    link = find_link(owner, link_id);
    if (link == NULL || link->readiness != UCN_V6_LINK_READY ||
        any_io_active(owner) ||
        owner->stats.faulted) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    if (frame_length > link->config.link_frame_mtu ||
        frame_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_MALFORMED;
    }
    if (request_timestamp && !link->config.tx_timestamp_hardware) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_CONFIG;
    }
    if (link->tx_count >= link->config.tx_slot_quota) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_NO_SPACE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            owner->tx[index].buffer_token == buffer_token) {
            unlock_owner(owner, false);
            return UCN_V6_ERR_REPLAY;
        }
        if (!owner->tx[index].occupied && slot == NULL) slot = &owner->tx[index];
    }
    if (slot == NULL || allocate_key(owner, link, &next_key) != UCN_V6_OK ||
        owner->next_order == 0U) {
        if (slot != NULL) owner->stats.faulted = true;
        unlock_owner(owner, false);
        return slot == NULL ? UCN_V6_ERR_NO_SPACE : UCN_V6_ERR_EXHAUSTED;
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->order = owner->next_order++;
    slot->key = next_key;
    slot->buffer_token = buffer_token;
    slot->state = UCN_V6_DRIVER_TX_QUEUED;
    slot->traffic_class = traffic_class;
    slot->frame_length = (uint16_t)frame_length;
    slot->request_timestamp = request_timestamp;
    memcpy(slot->frame, frame, frame_length);
    ++link->tx_count;
    increment_saturated(&owner->stats.tx_queued);
    *key = next_key;
    unlock_owner(owner, false);
    post_event(owner, UCN_V6_OWNER_EVENT_TX, false);
    return UCN_V6_OK;
}

static ucn_v6_adapter_tx_slot_t *oldest_tx_state(
    ucn_v6_adapter_owner_t *owner, ucn_v6_driver_tx_state_t state)
{
    ucn_v6_adapter_tx_slot_t *selected = NULL;
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied && owner->tx[index].state == state &&
            (selected == NULL || owner->tx[index].order < selected->order)) {
            selected = &owner->tx[index];
        }
    }
    return selected;
}

ucn_v6_result_t ucn_v6_adapter_service_tx(
    ucn_v6_adapter_owner_t *owner,
    bool *submitted)
{
    ucn_v6_adapter_tx_slot_t *slot;
    ucn_v6_adapter_link_slot_t *link;
    ucn_v6_driver_link_ops_t ops;
    ucn_v6_driver_event_key_t key;
    uint8_t priority;
    ucn_v6_result_t result;
    if (!owner_valid(owner) || submitted == NULL) return UCN_V6_ERR_ARGUMENT;
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    if (any_io_active(owner) || owner->stats.faulted) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    slot = oldest_tx_state(owner, UCN_V6_DRIVER_TX_QUEUED);
    if (slot == NULL) {
        *submitted = false;
        unlock_owner(owner, false);
        return UCN_V6_OK;
    }
    link = find_link(owner, slot->key.link_id);
    if (link == NULL || link->readiness != UCN_V6_LINK_READY ||
        link->config.initial_generation != slot->key.link_generation) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    link->io_active = true;
    slot->state = UCN_V6_DRIVER_TX_SUBMITTING;
    ops = link->config.ops;
    key = slot->key;
    priority = link->config.hardware_priority_count == 1U ? 0U :
        (uint8_t)(((uint32_t)UCN_V6_TRAFFIC_Q3 -
                   (uint32_t)slot->traffic_class) *
                  (uint32_t)(link->config.hardware_priority_count - 1U) /
                  (uint32_t)UCN_V6_TRAFFIC_Q3);
    unlock_owner(owner, false);

    result = ops.submit(ops.context, &key, slot->frame, slot->frame_length,
                        priority, slot->request_timestamp);

    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    link = find_link(owner, key.link_id);
    slot = NULL;
    {
        size_t index;
        for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
            if (owner->tx[index].occupied &&
                key_equal(&owner->tx[index].key, &key)) {
                slot = &owner->tx[index];
                break;
            }
        }
    }
    if (link == NULL || slot == NULL || !link->io_active) {
        owner->stats.faulted = true;
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    link->io_active = false;
    if (result == UCN_V6_OK) {
        if (slot->state == UCN_V6_DRIVER_TX_SUBMITTING) {
            slot->state = UCN_V6_DRIVER_TX_SUBMITTED;
        } else if (slot->state != UCN_V6_DRIVER_TX_COMPLETED) {
            owner->stats.faulted = true;
            unlock_owner(owner, false);
            return UCN_V6_ERR_STATE;
        }
        increment_saturated(&owner->stats.tx_submitted);
        *submitted = true;
    } else if (slot->state == UCN_V6_DRIVER_TX_SUBMITTING) {
        slot->state = UCN_V6_DRIVER_TX_QUEUED;
        increment_saturated(&owner->stats.tx_submit_failed);
        *submitted = false;
    } else {
        owner->stats.faulted = true;
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    unlock_owner(owner, false);
    return result;
}

ucn_v6_result_t ucn_v6_adapter_cancel_tx(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_event_key_t *key)
{
    ucn_v6_adapter_tx_slot_t *slot = NULL;
    ucn_v6_adapter_link_slot_t *link;
    ucn_v6_driver_link_ops_t ops;
    size_t index;
    ucn_v6_result_t result;
    if (!owner_valid(owner) || key == NULL) return UCN_V6_ERR_ARGUMENT;
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    if (any_io_active(owner) || owner->stats.faulted) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            key_equal(&owner->tx[index].key, key)) {
            slot = &owner->tx[index];
            break;
        }
    }
    if (slot == NULL) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (slot->state == UCN_V6_DRIVER_TX_CANCELLED ||
        slot->state == UCN_V6_DRIVER_TX_COMPLETED) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_REPLAY;
    }
    if (slot->state == UCN_V6_DRIVER_TX_QUEUED) {
        slot->state = UCN_V6_DRIVER_TX_CANCELLED;
        slot->completion = UCN_V6_ERR_CANCELLED;
        unlock_owner(owner, false);
        post_event(owner, UCN_V6_OWNER_EVENT_COMPLETION, false);
        return UCN_V6_OK;
    }
    if (slot->state != UCN_V6_DRIVER_TX_SUBMITTED) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    link = find_link(owner, key->link_id);
    if (link == NULL || link->config.initial_generation != key->link_generation ||
        link->config.ops.cancel == NULL) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_CONFIG;
    }
    link->io_active = true;
    ops = link->config.ops;
    unlock_owner(owner, false);
    result = ops.cancel(ops.context, key);
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    link = find_link(owner, key->link_id);
    slot = NULL;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            key_equal(&owner->tx[index].key, key)) {
            slot = &owner->tx[index];
            break;
        }
    }
    if (link == NULL || slot == NULL || !link->io_active) {
        owner->stats.faulted = true;
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    link->io_active = false;
    if (slot->state == UCN_V6_DRIVER_TX_COMPLETED) {
        unlock_owner(owner, false);
        return UCN_V6_OK;
    }
    if (result == UCN_V6_OK) {
        if (slot->state == UCN_V6_DRIVER_TX_SUBMITTED) {
            slot->state = UCN_V6_DRIVER_TX_CANCELLED;
            slot->completion = UCN_V6_ERR_CANCELLED;
            unlock_owner(owner, false);
            post_event(owner, UCN_V6_OWNER_EVENT_COMPLETION, false);
            return UCN_V6_OK;
        }
        if (slot->state == UCN_V6_DRIVER_TX_COMPLETED) {
            unlock_owner(owner, false);
            return UCN_V6_OK;
        }
        owner->stats.faulted = true;
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    if (slot->state != UCN_V6_DRIVER_TX_SUBMITTED) {
        owner->stats.faulted = true;
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    unlock_owner(owner, false);
    return result;
}

ucn_v6_result_t ucn_v6_adapter_publish_tx_completion(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_event_key_t *key,
    ucn_v6_result_t result,
    const ucn_v6_driver_timestamp_t *timestamp,
    bool from_isr)
{
    size_t index;
    if (!owner_valid(owner) || key == NULL || !result_valid(result)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!lock_owner(owner, from_isr)) return UCN_V6_ERR_STATE;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
        ucn_v6_adapter_tx_slot_t *slot = &owner->tx[index];
        if (slot->occupied && key_equal(&slot->key, key)) {
            ucn_v6_adapter_link_slot_t *link = find_link(owner, key->link_id);
            if (link == NULL || !timestamp_is_canonical(
                    timestamp, link->config.tx_timestamp_hardware)) {
                unlock_owner(owner, from_isr);
                return UCN_V6_ERR_MALFORMED;
            }
            if (slot->state == UCN_V6_DRIVER_TX_COMPLETED ||
                slot->state == UCN_V6_DRIVER_TX_CANCELLED) {
                unlock_owner(owner, from_isr);
                return UCN_V6_ERR_REPLAY;
            }
            if (slot->state != UCN_V6_DRIVER_TX_SUBMITTING &&
                slot->state != UCN_V6_DRIVER_TX_SUBMITTED) {
                unlock_owner(owner, from_isr);
                return UCN_V6_ERR_STATE;
            }
            slot->state = UCN_V6_DRIVER_TX_COMPLETED;
            slot->completion = result;
            if (timestamp != NULL) slot->timestamp = *timestamp;
            increment_saturated(&owner->stats.tx_completed);
            unlock_owner(owner, from_isr);
            post_event(owner, UCN_V6_OWNER_EVENT_COMPLETION, from_isr);
            return UCN_V6_OK;
        }
    }
    increment_saturated(&owner->stats.tx_stale_completion);
    unlock_owner(owner, from_isr);
    return UCN_V6_ERR_REPLAY;
}

ucn_v6_result_t ucn_v6_adapter_peek_tx_completion(
    ucn_v6_adapter_owner_t *owner,
    ucn_v6_driver_tx_completion_t *completion)
{
    ucn_v6_adapter_tx_slot_t *slot;
    ucn_v6_driver_tx_completion_t next;
    if (!owner_valid(owner) || completion == NULL) return UCN_V6_ERR_ARGUMENT;
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    slot = oldest_tx_state(owner, UCN_V6_DRIVER_TX_COMPLETED);
    if (slot == NULL) slot = oldest_tx_state(owner, UCN_V6_DRIVER_TX_CANCELLED);
    if (slot == NULL) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_NOT_FOUND;
    }
    memset(&next, 0, sizeof(next));
    next.key = slot->key;
    next.timestamp = slot->timestamp;
    next.buffer_token = slot->buffer_token;
    next.result = slot->completion;
    *completion = next;
    unlock_owner(owner, false);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_adapter_retire_tx_completion(
    ucn_v6_adapter_owner_t *owner,
    const ucn_v6_driver_event_key_t *key,
    uint64_t *buffer_token)
{
    size_t index;
    if (!owner_valid(owner) || key == NULL || buffer_token == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
        ucn_v6_adapter_tx_slot_t *slot = &owner->tx[index];
        if (slot->occupied && key_equal(&slot->key, key)) {
            ucn_v6_adapter_link_slot_t *link;
            uint64_t token;
            if (slot->state != UCN_V6_DRIVER_TX_COMPLETED &&
                slot->state != UCN_V6_DRIVER_TX_CANCELLED) {
                unlock_owner(owner, false);
                return UCN_V6_ERR_STATE;
            }
            link = find_link(owner, key->link_id);
            if (link == NULL || link->tx_count == 0U) {
                owner->stats.faulted = true;
                unlock_owner(owner, false);
                return UCN_V6_ERR_STATE;
            }
            token = slot->buffer_token;
            --link->tx_count;
            memset(slot, 0, sizeof(*slot));
            increment_saturated(&owner->stats.tx_retired);
            *buffer_token = token;
            unlock_owner(owner, false);
            return UCN_V6_OK;
        }
    }
    unlock_owner(owner, false);
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_adapter_reopen_link(
    ucn_v6_adapter_owner_t *owner,
    uint16_t link_id,
    uint64_t *retired_buffer_tokens,
    size_t retired_capacity,
    size_t *retired_count,
    uint32_t *new_generation)
{
    ucn_v6_adapter_link_slot_t *link;
    ucn_v6_driver_link_ops_t ops;
    size_t needed = 0U;
    size_t index;
    ucn_v6_result_t result;
    if (!owner_valid(owner) || retired_count == NULL || new_generation == NULL ||
        (retired_capacity != 0U && retired_buffer_tokens == NULL)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    link = find_link(owner, link_id);
    if (link == NULL || any_io_active(owner) || owner->stats.faulted ||
        link->config.initial_generation >= UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            owner->tx[index].key.link_id == link_id) ++needed;
    }
    if (needed > retired_capacity) {
        unlock_owner(owner, false);
        return UCN_V6_ERR_NO_SPACE;
    }
    link->io_active = true;
    link->readiness = UCN_V6_LINK_QUIESCING;
    ops = link->config.ops;
    unlock_owner(owner, false);
    result = ops.quiesce(ops.context);
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    link = find_link(owner, link_id);
    if (link == NULL || !link->io_active) {
        owner->stats.faulted = true;
        unlock_owner(owner, false);
        return UCN_V6_ERR_STATE;
    }
    link->io_active = false;
    if (result != UCN_V6_OK) {
        link->readiness = UCN_V6_LINK_OFFLINE;
        unlock_owner(owner, false);
        return result;
    }
    needed = 0U;
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            owner->tx[index].key.link_id == link_id) {
            retired_buffer_tokens[needed++] = owner->tx[index].buffer_token;
            memset(&owner->tx[index], 0, sizeof(owner->tx[index]));
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ADAPTER_RX_SLOTS; ++index) {
        if (owner->rx[index].occupied &&
            owner->rx[index].key.link_id == link_id) {
            memset(&owner->rx[index], 0, sizeof(owner->rx[index]));
        }
    }
    link->rx_count = 0U;
    link->tx_count = 0U;
    ++link->config.initial_generation;
    link->readiness = UCN_V6_LINK_STARTING;
    increment_saturated(&owner->stats.link_reopens);
    *retired_count = needed;
    *new_generation = link->config.initial_generation;
    unlock_owner(owner, false);
    post_event(owner, UCN_V6_OWNER_EVENT_COMPLETION, false);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_adapter_copy_stats(
    ucn_v6_adapter_owner_t *owner,
    ucn_v6_adapter_stats_t *stats)
{
    ucn_v6_adapter_stats_t next;
    if (!owner_valid(owner) || stats == NULL) return UCN_V6_ERR_ARGUMENT;
    if (!lock_owner(owner, false)) return UCN_V6_ERR_STATE;
    next = owner->stats;
    unlock_owner(owner, false);
    *stats = next;
    return UCN_V6_OK;
}
