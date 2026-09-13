#include "internal/ucn_adapter.h"

#include "internal/ucn_checked.h"

#include <limits.h>
#include <string.h>

#define UCN_I_ADAPTER_MAGIC UINT32_C(0x55434144)
#define UCN_I_ADAPTER_OWNER_KIND UINT16_C(0x0101)

static bool bytes_are_zero(const void *object, size_t bytes)
{
    const uint8_t *cursor = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (cursor[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool lock_is_valid(const ucn_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_API_VERSION && lock->enter != NULL &&
           lock->leave != NULL;
}

static ucn_i_lock_ops_t internal_lock(const ucn_lock_ops_t *lock)
{
    ucn_i_lock_ops_t converted;

    memset(&converted, 0, sizeof(converted));
    converted.struct_size = sizeof(converted);
    converted.api_version = UCN_I_LOCK_OPS_VERSION;
    converted.context = lock->context;
    converted.enter = lock->enter;
    converted.leave = lock->leave;
    return converted;
}

static bool tx_vtable_is_valid(const ucn_tx_port_vtable_t *tx)
{
    return tx != NULL && tx->struct_size == sizeof(*tx) &&
           tx->api_version == UCN_API_VERSION && tx->submit != NULL;
}

static bool adapter_header_is_valid(const ucn_i_adapter_t *adapter)
{
    return adapter != NULL && adapter->magic == UCN_I_ADAPTER_MAGIC &&
           adapter->runtime_instance != 0U && adapter->owner_instance != 0U &&
           adapter->schema == UCN_I_ADAPTER_SCHEMA &&
           adapter->link_count != 0U && adapter->link_count <= UCN_LINK_COUNT &&
           adapter->lock.enter != NULL && adapter->lock.leave != NULL &&
           adapter->driver_callback_gate.enter != NULL &&
           adapter->driver_callback_gate.leave != NULL;
}

static bool adapter_owned_state_is_valid_locked(
    const ucn_i_adapter_t *adapter)
{
    return adapter->tx_allocate_cursor < UCN_ADAPTER_TX_SLOT_COUNT &&
           adapter->rx_allocate_cursor < UCN_ADAPTER_RX_SLOT_COUNT &&
           adapter->rx_claim_cursor < UCN_ADAPTER_RX_SLOT_COUNT &&
           adapter->faulted <= 1U && adapter->rx_enabled <= 1U;
}

static bool terminal_result_is_valid(ucn_result_t result)
{
    return result <= UCN_OK && result >= UCN_ERR_UNSUPPORTED;
}

static ucn_result_t lock_enter(ucn_i_adapter_t *adapter)
{
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    result = adapter->lock.enter(adapter->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!adapter_owned_state_is_valid_locked(adapter)) {
        adapter->lock.leave(adapter->lock.context);
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

static void lock_leave(ucn_i_adapter_t *adapter)
{
    adapter->lock.leave(adapter->lock.context);
}

static ucn_result_t driver_gate_enter(ucn_i_adapter_t *adapter)
{
    return adapter->driver_callback_gate.enter(
               adapter->driver_callback_gate.context) == UCN_OK
               ? UCN_OK
               : UCN_ERR_STATE;
}

static void driver_gate_leave(ucn_i_adapter_t *adapter)
{
    adapter->driver_callback_gate.leave(
        adapter->driver_callback_gate.context);
}

static ucn_handle_t make_handle(const ucn_i_adapter_t *adapter,
                                uint16_t slot,
                                uint16_t generation,
                                uint8_t kind)
{
    ucn_handle_t handle;

    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = adapter->runtime_instance;
    handle.owner_instance = adapter->owner_instance;
    handle.slot = slot;
    handle.generation = generation;
    handle.object_kind = kind;
    return handle;
}

static bool link_handle_matches_locked(const ucn_i_adapter_t *adapter,
                                       ucn_link_handle_t handle,
                                       uint16_t *slot_out)
{
    uint16_t slot;

    if (handle.runtime_instance != adapter->runtime_instance ||
        handle.owner_instance != adapter->owner_instance ||
        handle.object_kind != UCN_OBJECT_KIND_LINK ||
        handle.reserved_zero != 0U || handle.slot == 0U) {
        return false;
    }
    slot = (uint16_t)(handle.slot - 1U);
    if (slot >= adapter->link_count || !adapter->links[slot].valid ||
        adapter->links[slot].handle_generation != handle.generation) {
        return false;
    }
    if (slot_out != NULL) {
        *slot_out = slot;
    }
    return true;
}

static bool tx_handle_matches_locked(const ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token,
                                     uint16_t *slot_out)
{
    uint16_t slot;

    if (token.runtime_instance != adapter->runtime_instance ||
        token.owner_instance != adapter->owner_instance ||
        token.object_kind != UCN_OBJECT_KIND_ADAPTER_TX ||
        token.reserved_zero != 0U || token.slot == 0U) {
        return false;
    }
    slot = (uint16_t)(token.slot - 1U);
    if (slot >= UCN_ADAPTER_TX_SLOT_COUNT ||
        adapter->tx_tokens[slot].state == UCN_I_ADAPTER_TX_FREE ||
        adapter->tx_tokens[slot].generation != token.generation) {
        return false;
    }
    if (slot_out != NULL) {
        *slot_out = slot;
    }
    return true;
}

static bool rx_handle_matches_locked(const ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token,
                                     uint16_t *slot_out)
{
    uint16_t slot;

    if (token.runtime_instance != adapter->runtime_instance ||
        token.owner_instance != adapter->owner_instance ||
        token.object_kind != UCN_OBJECT_KIND_ADAPTER_RX ||
        token.reserved_zero != 0U || token.slot == 0U) {
        return false;
    }
    slot = (uint16_t)(token.slot - 1U);
    if (slot >= UCN_ADAPTER_RX_SLOT_COUNT ||
        adapter->rx_slots[slot].state == 0U ||
        adapter->rx_slots[slot].generation != token.generation) {
        return false;
    }
    if (slot_out != NULL) {
        *slot_out = slot;
    }
    return true;
}

ucn_result_t ucn_i_adapter_init(ucn_i_adapter_t *adapter,
                                uint32_t runtime_instance,
                                uint16_t owner_instance,
                                const ucn_ports_t *ports)
{
    uint16_t index;

    if (adapter == NULL || ports == NULL || runtime_instance == 0U ||
        owner_instance == 0U || ports->struct_size != sizeof(*ports) ||
        ports->api_version != UCN_API_VERSION || ports->reserved_zero != 0U ||
        ports->links == NULL || ports->link_count == 0U ||
        ports->link_count > UCN_LINK_COUNT || !lock_is_valid(&ports->state_lock) ||
        !lock_is_valid(&ports->driver_callback_gate) ||
        (ports->state_lock.context == ports->driver_callback_gate.context &&
         ports->state_lock.enter == ports->driver_callback_gate.enter &&
         ports->state_lock.leave == ports->driver_callback_gate.leave) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), ports, sizeof(*ports)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), ports->links,
                             ports->link_count * sizeof(ports->links[0])) ||
        (ports->state_lock.context != NULL &&
         ucn_i_ranges_overlap(adapter, sizeof(*adapter),
                              ports->state_lock.context, 1U)) ||
        (ports->driver_callback_gate.context != NULL &&
         ucn_i_ranges_overlap(adapter, sizeof(*adapter),
                              ports->driver_callback_gate.context, 1U)) ||
        !bytes_are_zero(adapter, sizeof(*adapter))) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < ports->link_count; ++index) {
        const ucn_link_port_t *link = &ports->links[index];
        if (link->struct_size != sizeof(*link) ||
            link->api_version != UCN_API_VERSION || link->reserved_zero != 0U ||
            link->link_instance == 0U || link->frame_mtu == 0U ||
            link->frame_mtu > UCN_ADAPTER_FRAME_BYTES ||
            !tx_vtable_is_valid(&link->tx) ||
            (link->context != NULL &&
             ucn_i_ranges_overlap(adapter, sizeof(*adapter), link->context,
                                  1U))) {
            return UCN_ERR_CONFIG;
        }
    }
    adapter->runtime_instance = runtime_instance;
    adapter->owner_instance = owner_instance;
    adapter->schema = UCN_I_ADAPTER_SCHEMA;
    adapter->link_count = ports->link_count;
    adapter->lock = internal_lock(&ports->state_lock);
    adapter->driver_callback_gate =
        internal_lock(&ports->driver_callback_gate);
    for (index = 0U; index < ports->link_count; ++index) {
        ucn_i_link_t *target = &adapter->links[index];
        const ucn_link_port_t *source = &ports->links[index];
        target->driver_context = source->context;
        target->tx = source->tx;
        target->instance_generation = source->link_instance;
        target->handle_generation = 1U;
        target->frame_mtu = source->frame_mtu;
        target->valid = 1U;
        target->up = 1U;
    }
    adapter->magic = UCN_I_ADAPTER_MAGIC;
    if (ucn_i_owner_mailbox_init(&adapter->mailbox, 5U, &adapter->lock) !=
        UCN_OK) {
        memset(adapter, 0, sizeof(*adapter));
        return UCN_ERR_CONFIG;
    }
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_link_handle(const ucn_i_adapter_t *adapter,
                                       uint16_t link_index,
                                       ucn_link_handle_t *handle_out)
{
    ucn_i_adapter_t *mutable_adapter = (ucn_i_adapter_t *)adapter;
    ucn_handle_t handle;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || handle_out == NULL ||
        link_index >= adapter->link_count ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(mutable_adapter);
    if (result != UCN_OK) {
        return result;
    }
    if (!adapter->links[link_index].valid) {
        lock_leave(mutable_adapter);
        return UCN_ERR_NOT_FOUND;
    }
    handle = make_handle(adapter, (uint16_t)(link_index + 1U),
                         adapter->links[link_index].handle_generation,
                         UCN_OBJECT_KIND_LINK);
    lock_leave(mutable_adapter);
    *handle_out = handle;
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_link_snapshot(const ucn_i_adapter_t *adapter,
                                         uint16_t link_index,
                                         uint32_t *instance_out,
                                         uint16_t *mtu_out,
                                         bool *ready_out)
{
    ucn_i_adapter_t *mutable_adapter = (ucn_i_adapter_t *)adapter;
    uint32_t instance;
    uint16_t mtu;
    bool ready;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || instance_out == NULL ||
        mtu_out == NULL || ready_out == NULL || link_index >= adapter->link_count ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), instance_out,
                             sizeof(*instance_out)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), mtu_out,
                             sizeof(*mtu_out)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), ready_out,
                             sizeof(*ready_out)) ||
        ucn_i_ranges_overlap(instance_out, sizeof(*instance_out), mtu_out,
                             sizeof(*mtu_out)) ||
        ucn_i_ranges_overlap(instance_out, sizeof(*instance_out), ready_out,
                             sizeof(*ready_out)) ||
        ucn_i_ranges_overlap(mtu_out, sizeof(*mtu_out), ready_out,
                             sizeof(*ready_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(mutable_adapter);
    if (result != UCN_OK) {
        return result;
    }
    instance = adapter->links[link_index].instance_generation;
    mtu = adapter->links[link_index].frame_mtu;
    ready = adapter->links[link_index].valid && adapter->links[link_index].up &&
            !adapter->links[link_index].fenced && !adapter->faulted;
    lock_leave(mutable_adapter);
    *instance_out = instance;
    *mtu_out = mtu;
    *ready_out = ready;
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_set_rx_enabled(ucn_i_adapter_t *adapter,
                                          bool enabled)
{
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    adapter->rx_enabled = enabled ? 1U : 0U;
    lock_leave(adapter);
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_tx_reserve(ucn_i_adapter_t *adapter,
                                      uint16_t link_index,
                                      uint32_t link_instance_generation,
                                      uint16_t core_tx_slot,
                                      ucn_driver_token_t *token_out)
{
    uint16_t index;
    bool busy = false;
    bool exhausted = false;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || token_out == NULL ||
        link_instance_generation == 0U ||
        link_index >= adapter->link_count ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), token_out,
                             sizeof(*token_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    if (adapter->faulted || !adapter->links[link_index].valid ||
        !adapter->links[link_index].up || adapter->links[link_index].fenced) {
        lock_leave(adapter);
        return UCN_ERR_STATE;
    }
    if (adapter->links[link_index].instance_generation !=
        link_instance_generation) {
        lock_leave(adapter);
        return UCN_ERR_NOT_FOUND;
    }
    for (index = 0U; index < UCN_ADAPTER_TX_SLOT_COUNT; ++index) {
        uint16_t slot = (uint16_t)(
            (adapter->tx_allocate_cursor + index) % UCN_ADAPTER_TX_SLOT_COUNT);
        ucn_i_adapter_tx_token_t *entry = &adapter->tx_tokens[slot];
        uint16_t next_generation;
        if (entry->state != UCN_I_ADAPTER_TX_FREE) {
            busy = true;
            continue;
        }
        if (entry->generation == 0U) {
            next_generation = 1U;
        } else if (entry->generation == UINT16_MAX) {
            exhausted = true;
            continue;
        } else {
            next_generation = (uint16_t)(entry->generation + 1U);
        }
        memset(entry, 0, sizeof(*entry));
        entry->generation = next_generation;
        entry->link_slot = link_index;
        entry->link_instance_generation = link_instance_generation;
        entry->core_tx_slot = core_tx_slot;
        entry->state = UCN_I_ADAPTER_TX_RESERVED;
        *token_out = make_handle(adapter, (uint16_t)(slot + 1U),
                                  next_generation,
                                  UCN_OBJECT_KIND_ADAPTER_TX);
        adapter->tx_allocate_cursor = (uint16_t)(
            (slot + 1U) % UCN_ADAPTER_TX_SLOT_COUNT);
        lock_leave(adapter);
        return UCN_OK;
    }
    if (!busy && exhausted) {
        adapter->faulted = 1U;
        lock_leave(adapter);
        return UCN_ERR_EXHAUSTED;
    }
    lock_leave(adapter);
    return UCN_ERR_NO_SPACE;
}

static ucn_i_adapter_tx_view_t tx_view_from_entry(
    const ucn_i_adapter_tx_token_t *entry)
{
    ucn_i_adapter_tx_view_t view;

    memset(&view, 0, sizeof(view));
    view.terminal_result = entry->terminal_result;
    view.core_tx_slot = entry->core_tx_slot;
    view.state = entry->state;
    view.terminal_latched = entry->terminal_latched;
    return view;
}

ucn_result_t ucn_i_adapter_tx_submit(ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token,
                                     const uint8_t *frame,
                                     size_t frame_bytes,
                                     ucn_i_adapter_tx_view_t *view_out)
{
    ucn_tx_submit_fn submit;
    void *context;
    ucn_link_handle_t link_handle;
    ucn_driver_submit_result_t driver_result;
    ucn_i_adapter_tx_view_t view;
    uint16_t token_slot;
    uint16_t link_slot;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || frame == NULL || frame_bytes == 0U ||
        view_out == NULL ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), frame, frame_bytes) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), view_out,
                             sizeof(*view_out)) ||
        ucn_i_ranges_overlap(frame, frame_bytes, view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = driver_gate_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        driver_gate_leave(adapter);
        return result;
    }
    if (!tx_handle_matches_locked(adapter, token, &token_slot) ||
        adapter->tx_tokens[token_slot].state != UCN_I_ADAPTER_TX_RESERVED) {
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return UCN_ERR_STATE;
    }
    link_slot = adapter->tx_tokens[token_slot].link_slot;
    if (link_slot >= adapter->link_count || frame_bytes > adapter->links[link_slot].frame_mtu ||
        !adapter->links[link_slot].up || adapter->links[link_slot].fenced ||
        adapter->tx_tokens[token_slot].link_instance_generation !=
            adapter->links[link_slot].instance_generation) {
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return UCN_ERR_STATE;
    }
    adapter->tx_tokens[token_slot].state = UCN_I_ADAPTER_TX_SUBMITTING;
    submit = adapter->links[link_slot].tx.submit;
    context = adapter->links[link_slot].driver_context;
    link_handle = make_handle(adapter, (uint16_t)(link_slot + 1U),
                              adapter->links[link_slot].handle_generation,
                              UCN_OBJECT_KIND_LINK);
    lock_leave(adapter);

    driver_result = submit(context, link_handle, frame, frame_bytes, token);

    result = lock_enter(adapter);
    if (result != UCN_OK) {
        driver_gate_leave(adapter);
        return result;
    }
    if (!tx_handle_matches_locked(adapter, token, &token_slot)) {
        adapter->faulted = 1U;
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return UCN_ERR_STATE;
    }
    if (adapter->tx_tokens[token_slot].state == UCN_I_ADAPTER_TX_IN_DOUBT ||
        adapter->tx_tokens[token_slot].state == UCN_I_ADAPTER_TX_COMPLETED) {
        view = tx_view_from_entry(&adapter->tx_tokens[token_slot]);
        lock_leave(adapter);
        driver_gate_leave(adapter);
        *view_out = view;
        return UCN_OK;
    }
    if (adapter->tx_tokens[token_slot].state != UCN_I_ADAPTER_TX_SUBMITTING) {
        adapter->faulted = 1U;
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return UCN_ERR_STATE;
    }
    if (adapter->tx_tokens[token_slot].terminal_latched) {
        adapter->tx_tokens[token_slot].state = UCN_I_ADAPTER_TX_COMPLETED;
        if (driver_result != UCN_DRIVER_COMPLETE &&
            driver_result != UCN_DRIVER_SUBMITTED) {
            adapter->links[link_slot].fenced = 1U;
        }
    } else if (driver_result == UCN_DRIVER_SUBMITTED) {
        adapter->tx_tokens[token_slot].state = UCN_I_ADAPTER_TX_SUBMITTED;
    } else if (driver_result == UCN_DRIVER_COMPLETE) {
        adapter->tx_tokens[token_slot].terminal_latched = 1U;
        adapter->tx_tokens[token_slot].terminal_result = UCN_OK;
        adapter->tx_tokens[token_slot].state = UCN_I_ADAPTER_TX_COMPLETED;
    } else if (driver_result == UCN_ERR_IN_DOUBT ||
               driver_result == UCN_DRIVER_SUBMIT_UNKNOWN) {
        adapter->tx_tokens[token_slot].state = UCN_I_ADAPTER_TX_IN_DOUBT;
        adapter->links[link_slot].fenced = 1U;
    } else if (driver_result == UCN_DRIVER_NOT_SUBMITTED) {
        /* A definitive NOT_SUBMITTED result proves that the Driver produced
         * no physical side effect and no longer owns this token.  It is a
         * retryable Adapter outcome, not a Core completion. */
        adapter->tx_tokens[token_slot].state =
            UCN_I_ADAPTER_TX_NOT_SUBMITTED;
    } else if (driver_result < 0 &&
               terminal_result_is_valid((ucn_result_t)driver_result)) {
        adapter->tx_tokens[token_slot].terminal_latched = 1U;
        adapter->tx_tokens[token_slot].terminal_result =
            (ucn_result_t)driver_result;
        adapter->tx_tokens[token_slot].state = UCN_I_ADAPTER_TX_NOT_SUBMITTED;
    } else {
        adapter->tx_tokens[token_slot].state = UCN_I_ADAPTER_TX_IN_DOUBT;
        adapter->links[link_slot].fenced = 1U;
    }
    view = tx_view_from_entry(&adapter->tx_tokens[token_slot]);
    lock_leave(adapter);
    driver_gate_leave(adapter);
    *view_out = view;
    (void)ucn_i_owner_mailbox_publish(&adapter->mailbox,
                                      UCN_I_OWNER_WORK_COMPLETION);
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_tx_complete(ucn_i_adapter_t *adapter,
                                       ucn_driver_token_t token,
                                       ucn_result_t result,
                                       const ucn_tx_meta_t *meta)
{
    uint16_t slot;
    ucn_result_t lock_result;

    if (!adapter_header_is_valid(adapter) ||
        !terminal_result_is_valid(result) ||
        (meta != NULL &&
         (meta->struct_size != sizeof(*meta) ||
          meta->api_version != UCN_API_VERSION || meta->reserved_zero != 0U ||
          ucn_i_ranges_overlap(adapter, sizeof(*adapter), meta,
                               sizeof(*meta))))) {
        return UCN_ERR_ARGUMENT;
    }
    lock_result = lock_enter(adapter);
    if (lock_result != UCN_OK) {
        return lock_result;
    }
    if (!tx_handle_matches_locked(adapter, token, &slot)) {
        lock_leave(adapter);
        return UCN_ERR_STATE;
    }
    if (adapter->tx_tokens[slot].state == UCN_I_ADAPTER_TX_COMPLETED &&
        adapter->tx_tokens[slot].terminal_latched) {
        if (adapter->tx_tokens[slot].link_slot >= adapter->link_count ||
            adapter->tx_tokens[slot].link_instance_generation !=
                adapter->links[adapter->tx_tokens[slot].link_slot]
                    .instance_generation) {
            lock_leave(adapter);
            return UCN_ERR_NOT_FOUND;
        }
        if (adapter->tx_tokens[slot].terminal_result == result) {
            lock_leave(adapter);
            return UCN_OK;
        }
        adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_IN_DOUBT;
        adapter->links[adapter->tx_tokens[slot].link_slot].fenced = 1U;
        lock_leave(adapter);
        return UCN_ERR_IN_DOUBT;
    }
    if (adapter->tx_tokens[slot].state != UCN_I_ADAPTER_TX_SUBMITTING &&
        adapter->tx_tokens[slot].state != UCN_I_ADAPTER_TX_SUBMITTED) {
        lock_leave(adapter);
        return UCN_ERR_STATE;
    }
    if (adapter->tx_tokens[slot].terminal_latched) {
        if (adapter->tx_tokens[slot].terminal_result != result) {
            adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_IN_DOUBT;
            adapter->links[adapter->tx_tokens[slot].link_slot].fenced = 1U;
            lock_leave(adapter);
            return UCN_ERR_IN_DOUBT;
        }
        lock_leave(adapter);
        return UCN_OK;
    }
    adapter->tx_tokens[slot].terminal_latched = 1U;
    adapter->tx_tokens[slot].terminal_result = result;
    if (adapter->tx_tokens[slot].state == UCN_I_ADAPTER_TX_SUBMITTED) {
        adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_COMPLETED;
    }
    lock_leave(adapter);
    (void)ucn_i_owner_mailbox_publish(&adapter->mailbox,
                                      UCN_I_OWNER_WORK_COMPLETION);
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_tx_view(ucn_i_adapter_t *adapter,
                                   ucn_driver_token_t token,
                                   ucn_i_adapter_tx_view_t *view_out)
{
    ucn_i_adapter_tx_view_t view;
    uint16_t slot;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || view_out == NULL ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    if (!tx_handle_matches_locked(adapter, token, &slot)) {
        lock_leave(adapter);
        return UCN_ERR_NOT_FOUND;
    }
    if (adapter->tx_tokens[slot].state == UCN_I_ADAPTER_TX_SUBMITTING &&
        adapter->tx_tokens[slot].terminal_latched) {
        adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_COMPLETED;
    }
    view = tx_view_from_entry(&adapter->tx_tokens[slot]);
    lock_leave(adapter);
    *view_out = view;
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_tx_cancel(ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token)
{
    ucn_tx_cancel_fn cancel;
    void *context;
    uint16_t slot;
    uint16_t link_slot;
    uint8_t state;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    if (!tx_handle_matches_locked(adapter, token, &slot)) {
        lock_leave(adapter);
        return UCN_ERR_NOT_FOUND;
    }
    state = adapter->tx_tokens[slot].state;
    if (state == UCN_I_ADAPTER_TX_RESERVED ||
        state == UCN_I_ADAPTER_TX_NOT_SUBMITTED) {
        adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_CANCELLED;
        adapter->tx_tokens[slot].terminal_latched = 1U;
        adapter->tx_tokens[slot].terminal_result = UCN_ERR_CANCELLED;
        lock_leave(adapter);
        return UCN_OK;
    }
    if (state == UCN_I_ADAPTER_TX_CANCELLED) {
        lock_leave(adapter);
        return UCN_OK;
    }
    if (state != UCN_I_ADAPTER_TX_SUBMITTED) {
        lock_leave(adapter);
        return state == UCN_I_ADAPTER_TX_COMPLETED ? UCN_ERR_STATE
                                                   : UCN_ERR_IN_DOUBT;
    }
    link_slot = adapter->tx_tokens[slot].link_slot;
    if (link_slot >= adapter->link_count) {
        adapter->faulted = 1U;
        lock_leave(adapter);
        return UCN_ERR_STATE;
    }
    cancel = adapter->links[link_slot].tx.cancel;
    if (cancel == NULL) {
        adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_IN_DOUBT;
        adapter->links[link_slot].fenced = 1U;
        lock_leave(adapter);
        return UCN_ERR_IN_DOUBT;
    }
    lock_leave(adapter);

    result = driver_gate_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        driver_gate_leave(adapter);
        return result;
    }
    if (!tx_handle_matches_locked(adapter, token, &slot) ||
        adapter->tx_tokens[slot].state != UCN_I_ADAPTER_TX_SUBMITTED ||
        adapter->tx_tokens[slot].link_slot >= adapter->link_count) {
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return UCN_ERR_STATE;
    }
    link_slot = adapter->tx_tokens[slot].link_slot;
    cancel = adapter->links[link_slot].tx.cancel;
    context = adapter->links[link_slot].driver_context;
    if (cancel == NULL) {
        adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_IN_DOUBT;
        adapter->links[link_slot].fenced = 1U;
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return UCN_ERR_IN_DOUBT;
    }
    lock_leave(adapter);
    result = cancel(context, token);
    if (lock_enter(adapter) != UCN_OK) {
        driver_gate_leave(adapter);
        return UCN_ERR_STATE;
    }
    if (!tx_handle_matches_locked(adapter, token, &slot)) {
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return UCN_ERR_STATE;
    }
    if (adapter->tx_tokens[slot].state == UCN_I_ADAPTER_TX_COMPLETED &&
        adapter->tx_tokens[slot].terminal_latched) {
        ucn_result_t terminal = adapter->tx_tokens[slot].terminal_result;
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return result == UCN_OK && terminal == UCN_ERR_CANCELLED
                   ? UCN_OK
                   : UCN_ERR_STATE;
    }
    if (adapter->tx_tokens[slot].state != UCN_I_ADAPTER_TX_SUBMITTED) {
        lock_leave(adapter);
        driver_gate_leave(adapter);
        return UCN_ERR_IN_DOUBT;
    }
    if (result == UCN_OK) {
        adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_CANCELLED;
        adapter->tx_tokens[slot].terminal_latched = 1U;
        adapter->tx_tokens[slot].terminal_result = UCN_ERR_CANCELLED;
    } else {
        adapter->tx_tokens[slot].state = UCN_I_ADAPTER_TX_IN_DOUBT;
        adapter->links[adapter->tx_tokens[slot].link_slot].fenced = 1U;
    }
    lock_leave(adapter);
    driver_gate_leave(adapter);
    return result == UCN_OK ? UCN_OK : UCN_ERR_IN_DOUBT;
}

ucn_result_t ucn_i_adapter_tx_retire(ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token)
{
    uint16_t slot;
    uint16_t generation;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    if (!tx_handle_matches_locked(adapter, token, &slot)) {
        lock_leave(adapter);
        return UCN_ERR_NOT_FOUND;
    }
    if (adapter->tx_tokens[slot].state != UCN_I_ADAPTER_TX_COMPLETED &&
        adapter->tx_tokens[slot].state != UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
        adapter->tx_tokens[slot].state != UCN_I_ADAPTER_TX_CANCELLED) {
        lock_leave(adapter);
        return UCN_ERR_STATE;
    }
    generation = adapter->tx_tokens[slot].generation;
    memset(&adapter->tx_tokens[slot], 0, sizeof(adapter->tx_tokens[slot]));
    adapter->tx_tokens[slot].generation = generation;
    lock_leave(adapter);
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_rx_publish(ucn_i_adapter_t *adapter,
                                      ucn_link_handle_t link,
                                      const uint8_t *frame,
                                      size_t frame_bytes,
                                      const ucn_rx_meta_t *meta)
{
    uint16_t link_slot;
    uint16_t index;
    bool busy = false;
    bool exhausted = false;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || frame == NULL || frame_bytes == 0U ||
        frame_bytes > UCN_ADAPTER_FRAME_BYTES || meta == NULL ||
        meta->struct_size != sizeof(*meta) ||
        meta->api_version != UCN_API_VERSION || meta->reserved_zero != 0U ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), frame, frame_bytes) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), meta, sizeof(*meta)) ||
        ucn_i_ranges_overlap(frame, frame_bytes, meta, sizeof(*meta))) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    if (!adapter->rx_enabled ||
        !link_handle_matches_locked(adapter, link, &link_slot) ||
        !adapter->links[link_slot].up || adapter->links[link_slot].fenced ||
        frame_bytes > adapter->links[link_slot].frame_mtu) {
        lock_leave(adapter);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_ADAPTER_RX_SLOT_COUNT; ++index) {
        uint16_t slot_index = (uint16_t)(
            (adapter->rx_allocate_cursor + index) % UCN_ADAPTER_RX_SLOT_COUNT);
        ucn_i_adapter_rx_slot_t *slot = &adapter->rx_slots[slot_index];
        uint16_t next_generation;
        if (slot->state != 0U) {
            busy = true;
            continue;
        }
        if (slot->generation == UINT16_MAX) {
            exhausted = true;
            continue;
        }
        next_generation = slot->generation == 0U ? 1U
                                                  : (uint16_t)(slot->generation + 1U);
        slot->generation = next_generation;
        slot->link_slot = link_slot;
        slot->link_instance_generation =
            adapter->links[link_slot].instance_generation;
        slot->frame_bytes = (uint16_t)frame_bytes;
        slot->meta = *meta;
        memcpy(slot->frame, frame, frame_bytes);
        slot->state = 1U;
        adapter->rx_allocate_cursor = (uint16_t)(
            (slot_index + 1U) % UCN_ADAPTER_RX_SLOT_COUNT);
        lock_leave(adapter);
        (void)ucn_i_owner_mailbox_publish(&adapter->mailbox,
                                          UCN_I_OWNER_WORK_REQUEST);
        return UCN_OK;
    }
    if (!busy && exhausted) {
        adapter->faulted = 1U;
        lock_leave(adapter);
        return UCN_ERR_EXHAUSTED;
    }
    lock_leave(adapter);
    return UCN_ERR_NO_SPACE;
}

ucn_result_t ucn_i_adapter_rx_claim(ucn_i_adapter_t *adapter,
                                    ucn_i_adapter_rx_view_t *view_out)
{
    ucn_i_adapter_rx_view_t view;
    uint16_t index;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || view_out == NULL ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_ADAPTER_RX_SLOT_COUNT; ++index) {
        uint16_t slot_index = (uint16_t)(
            (adapter->rx_claim_cursor + index) % UCN_ADAPTER_RX_SLOT_COUNT);
        ucn_i_adapter_rx_slot_t *slot = &adapter->rx_slots[slot_index];
        if (slot->state != 1U) {
            continue;
        }
        memset(&view, 0, sizeof(view));
        slot->state = 2U;
        view.frame = slot->frame;
        view.frame_bytes = slot->frame_bytes;
        view.meta = slot->meta;
        view.link_slot = slot->link_slot;
        view.link_instance_generation = slot->link_instance_generation;
        view.token = make_handle(adapter, (uint16_t)(slot_index + 1U),
                                  slot->generation,
                                  UCN_OBJECT_KIND_ADAPTER_RX);
        adapter->rx_claim_cursor = (uint16_t)(
            (slot_index + 1U) % UCN_ADAPTER_RX_SLOT_COUNT);
        lock_leave(adapter);
        *view_out = view;
        return UCN_OK;
    }
    lock_leave(adapter);
    return UCN_ERR_NOT_FOUND;
}

ucn_result_t ucn_i_adapter_rx_retire(ucn_i_adapter_t *adapter,
                                     ucn_driver_token_t token)
{
    uint16_t slot;
    uint16_t generation;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    if (!rx_handle_matches_locked(adapter, token, &slot)) {
        lock_leave(adapter);
        return UCN_ERR_NOT_FOUND;
    }
    if (adapter->rx_slots[slot].state != 2U) {
        lock_leave(adapter);
        return UCN_ERR_STATE;
    }
    generation = adapter->rx_slots[slot].generation;
    memset(&adapter->rx_slots[slot], 0, sizeof(adapter->rx_slots[slot]));
    adapter->rx_slots[slot].generation = generation;
    lock_leave(adapter);
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_link_event(ucn_i_adapter_t *adapter,
                                      ucn_link_handle_t link,
                                      ucn_driver_link_event_t event,
                                      const ucn_link_event_meta_t *meta)
{
    uint16_t slot;
    uint16_t index;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) ||
        (event != UCN_DRIVER_LINK_UP && event != UCN_DRIVER_LINK_DOWN &&
         event != UCN_DRIVER_LINK_REOPENED) ||
        (meta != NULL &&
         (meta->struct_size != sizeof(*meta) ||
          meta->api_version != UCN_API_VERSION || meta->reserved_zero != 0U ||
          ucn_i_ranges_overlap(adapter, sizeof(*adapter), meta,
                               sizeof(*meta))))) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    if (!link_handle_matches_locked(adapter, link, &slot)) {
        lock_leave(adapter);
        return UCN_ERR_NOT_FOUND;
    }
    if (event == UCN_DRIVER_LINK_REOPENED) {
        if (meta == NULL || meta->new_link_instance == 0U) {
            lock_leave(adapter);
            return UCN_ERR_ARGUMENT;
        }
        if (adapter->links[slot].instance_generation == UINT32_MAX ||
            adapter->links[slot].handle_generation == UINT16_MAX) {
            adapter->links[slot].fenced = 1U;
            adapter->faulted = 1U;
            lock_leave(adapter);
            return UCN_ERR_EXHAUSTED;
        }
        if (meta->new_link_instance <=
            adapter->links[slot].instance_generation) {
            lock_leave(adapter);
            return UCN_ERR_ARGUMENT;
        }
        adapter->links[slot].instance_generation = meta->new_link_instance;
        ++adapter->links[slot].handle_generation;
        adapter->links[slot].fenced = 0U;
        adapter->links[slot].up = 1U;
    } else {
        adapter->links[slot].up = event == UCN_DRIVER_LINK_UP ? 1U : 0U;
    }
    if (event != UCN_DRIVER_LINK_UP) {
        for (index = 0U; index < UCN_ADAPTER_TX_SLOT_COUNT; ++index) {
            if (adapter->tx_tokens[index].state != UCN_I_ADAPTER_TX_FREE &&
                adapter->tx_tokens[index].link_slot == slot &&
                adapter->tx_tokens[index].state != UCN_I_ADAPTER_TX_COMPLETED &&
                adapter->tx_tokens[index].state != UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
                adapter->tx_tokens[index].state != UCN_I_ADAPTER_TX_CANCELLED) {
                if (adapter->tx_tokens[index].state ==
                    UCN_I_ADAPTER_TX_RESERVED) {
                    adapter->tx_tokens[index].state =
                        UCN_I_ADAPTER_TX_NOT_SUBMITTED;
                } else if (event == UCN_DRIVER_LINK_REOPENED) {
                    adapter->tx_tokens[index].state =
                        UCN_I_ADAPTER_TX_COMPLETED;
                    adapter->tx_tokens[index].terminal_latched = 1U;
                    adapter->tx_tokens[index].terminal_result =
                        UCN_ERR_IN_DOUBT;
                } else {
                    adapter->tx_tokens[index].state =
                        UCN_I_ADAPTER_TX_IN_DOUBT;
                    adapter->links[slot].fenced = 1U;
                }
            }
        }
    }
    lock_leave(adapter);
    (void)ucn_i_owner_mailbox_publish(&adapter->mailbox,
                                      UCN_I_OWNER_WORK_INVALIDATION);
    return UCN_OK;
}

bool ucn_i_adapter_has_work(ucn_i_adapter_t *adapter)
{
    uint16_t index;
    bool work = false;

    if (lock_enter(adapter) != UCN_OK) {
        return true;
    }
    for (index = 0U; index < UCN_ADAPTER_RX_SLOT_COUNT; ++index) {
        if (adapter->rx_slots[index].state != 0U) {
            work = true;
            break;
        }
    }
    if (!work) {
        for (index = 0U; index < UCN_ADAPTER_TX_SLOT_COUNT; ++index) {
            if (adapter->tx_tokens[index].state != UCN_I_ADAPTER_TX_FREE) {
                work = true;
                break;
            }
        }
    }
    lock_leave(adapter);
    return work;
}

bool ucn_i_adapter_has_runnable_work(ucn_i_adapter_t *adapter)
{
    uint16_t index;
    bool work = false;

    if (lock_enter(adapter) != UCN_OK) {
        return true;
    }
    for (index = 0U; index < UCN_ADAPTER_RX_SLOT_COUNT; ++index) {
        if (adapter->rx_slots[index].state == 1U) {
            work = true;
            break;
        }
    }
    if (!work) {
        for (index = 0U; index < UCN_ADAPTER_TX_SLOT_COUNT; ++index) {
            uint8_t state = adapter->tx_tokens[index].state;
            if (state == UCN_I_ADAPTER_TX_COMPLETED ||
                state == UCN_I_ADAPTER_TX_NOT_SUBMITTED ||
                state == UCN_I_ADAPTER_TX_CANCELLED) {
                work = true;
                break;
            }
        }
    }
    lock_leave(adapter);
    return work;
}

ucn_result_t ucn_i_adapter_faulted(ucn_i_adapter_t *adapter,
                                   bool *faulted_out)
{
    bool faulted;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || faulted_out == NULL ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), faulted_out,
                             sizeof(*faulted_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    faulted = adapter->faulted != 0U;
    lock_leave(adapter);
    *faulted_out = faulted;
    return UCN_OK;
}

ucn_result_t ucn_i_adapter_destroy(ucn_i_adapter_t *adapter)
{
    uint16_t index;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    result = lock_enter(adapter);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_ADAPTER_TX_SLOT_COUNT; ++index) {
        if (adapter->tx_tokens[index].state != UCN_I_ADAPTER_TX_FREE) {
            lock_leave(adapter);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_ADAPTER_RX_SLOT_COUNT; ++index) {
        if (adapter->rx_slots[index].state != 0U) {
            lock_leave(adapter);
            return UCN_ERR_STATE;
        }
    }
    lock_leave(adapter);
    if (ucn_i_owner_mailbox_destroy(&adapter->mailbox) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    memset(adapter, 0, sizeof(*adapter));
    return UCN_OK;
}

UCN_STATIC_ASSERT(sizeof(ucn_i_lock_ops_t) == sizeof(ucn_lock_ops_t),
                  public_and_internal_lock_layout_must_match);
