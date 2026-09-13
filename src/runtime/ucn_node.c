#include "internal/ucn_runtime.h"

#include "internal/ucn_checked.h"

#include <limits.h>
#include <string.h>

#if defined(_MSC_VER)
#define UCN_I_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define UCN_I_NOINLINE __attribute__((noinline))
#else
#define UCN_I_NOINLINE
#endif

#define UCN_I_NODE_MAGIC UINT32_C(0x55434E53)
#define UCN_I_OPERATION_API UINT16_C(1)

UCN_STATIC_ASSERT(sizeof(struct ucn_node) <= UCN_STORAGE_BYTES,
                  node_must_fit_public_storage);

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

static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static bool address_is_valid(uint32_t address, uint8_t width)
{
    uint32_t limit;

    if (width == 0U || width > 4U) {
        return false;
    }
    limit = width == 4U ? UINT32_MAX
                        : (UINT32_C(1) << (width * 8U)) - UINT32_C(1);
    return address != 0U && address < limit;
}

static bool public_lock_is_valid(const ucn_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_API_VERSION && lock->enter != NULL &&
           lock->leave != NULL;
}

static ucn_i_lock_ops_t make_internal_lock(const ucn_lock_ops_t *lock)
{
    ucn_i_lock_ops_t result;

    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.api_version = UCN_I_LOCK_OPS_VERSION;
    result.context = lock->context;
    result.enter = lock->enter;
    result.leave = lock->leave;
    return result;
}

static bool node_is_valid(const ucn_node_t *node)
{
    return node != NULL && node->magic == UCN_I_NODE_MAGIC &&
           node->runtime_instance != 0U &&
           node->owner_instance == UCN_I_CORE_OWNER_INSTANCE &&
           node->schema == UCN_I_NODE_SCHEMA &&
           node->storage_layout == UCN_STORAGE_LAYOUT &&
           node->compiled_manifest_hash == UCN_COMPILED_MANIFEST_HASH &&
           node->realm_id != 0U &&
           address_is_valid(node->local_address, node->address_width) &&
           node->local_binding_generation != 0U &&
           node->local_principal_digest != 0U && node->binding_count != 0U &&
           node->binding_count <= UCN_BINDING_COUNT && node->link_count != 0U &&
           node->link_count <= UCN_LINK_COUNT &&
           node->stats.struct_size == sizeof(node->stats) &&
           node->stats.api_version == UCN_API_VERSION;
}

static bool node_owned_state_is_valid(const ucn_node_t *node)
{
    uint16_t binding;
    uint8_t traffic_class;
    bool callback_scope_is_zero;

    callback_scope_is_zero = bytes_are_zero(
        &node->callback_read_snapshot.scope,
        sizeof(node->callback_read_snapshot.scope));

    if (node->lifecycle < UCN_LIFECYCLE_INITIALIZED ||
        node->lifecycle > UCN_LIFECYCLE_FAULT || node->work_cursor >= 4U ||
        node->scheduler_cursor >= 12U ||
        node->time_initialized > 1U ||
        node->callback_read_snapshot.active > 1U ||
        (node->callback_read_snapshot.active == 0U &&
         (!callback_scope_is_zero ||
          node->callback_read_snapshot.link_count != 0U)) ||
        (node->callback_read_snapshot.active == 1U &&
         (callback_scope_is_zero ||
          node->callback_read_snapshot.scope.runtime_instance !=
              node->runtime_instance ||
          node->callback_read_snapshot.scope.owner_instance !=
              node->owner_instance ||
          node->callback_read_snapshot.scope.reserved_zero != 0U ||
          node->callback_read_snapshot.scope.nonce == 0U ||
          node->callback_read_snapshot.scope.nonce !=
              node->callback_read_nonce ||
          node->callback_read_snapshot.link_count != node->link_count)) ||
        ((node->lifecycle == UCN_LIFECYCLE_RUNNING ||
          node->lifecycle == UCN_LIFECYCLE_STOPPING) &&
         node->time_initialized == 0U) ||
        node->endpoint_allocate_cursor >= UCN_ENDPOINT_COUNT ||
        node->path_allocate_cursor >= UCN_STATIC_PATH_COUNT ||
        node->request_allocate_cursor >= UCN_REQUEST_COUNT ||
        node->receipt_allocate_cursor >= UCN_RECEIPT_COUNT ||
        node->attempt_allocate_cursor >= UCN_ATTEMPT_COUNT ||
        node->buffer_allocate_cursor >= UCN_BUFFER_OBLIGATION_COUNT ||
        node->completion_cursor >= UCN_TX_SLOT_COUNT ||
         node->timer_cursor >= UCN_TX_SLOT_COUNT) {
        return false;
    }
    for (binding = 0U; binding < node->binding_count; ++binding) {
        const ucn_i_binding_t *entry = &node->bindings[binding];
        if ((entry->replay_highest_sequence == 0U) !=
                (entry->replay_bitmap == 0U) ||
            (entry->replay_highest_sequence != 0U &&
             (entry->replay_bitmap & UINT64_C(1)) == 0U)) {
            return false;
        }
    }
    for (traffic_class = 0U; traffic_class < UCN_TRAFFIC_CLASS_COUNT;
         ++traffic_class) {
        static const uint8_t counts[UCN_TRAFFIC_CLASS_COUNT] = {
            UCN_Q0_DEPTH, UCN_Q1_DEPTH, UCN_Q2_DEPTH, UCN_Q3_DEPTH};
        if (node->queue_cursor[traffic_class] >= counts[traffic_class] ||
            node->queue_allocate_cursor[traffic_class] >=
                counts[traffic_class]) {
            return false;
        }
    }
    return true;
}

static ucn_handle_t make_core_handle(const ucn_node_t *node,
                                     uint16_t slot,
                                     uint16_t generation,
                                     uint8_t kind)
{
    ucn_handle_t handle;

    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = node->runtime_instance;
    handle.owner_instance = node->owner_instance;
    handle.slot = (uint16_t)(slot + 1U);
    handle.generation = generation;
    handle.object_kind = kind;
    return handle;
}

static bool handle_matches(const ucn_node_t *node,
                           ucn_handle_t handle,
                           uint8_t kind,
                           uint16_t count,
                           uint16_t *slot_out)
{
    uint16_t slot;

    if (handle.runtime_instance != node->runtime_instance ||
        handle.owner_instance != node->owner_instance ||
        handle.object_kind != kind || handle.reserved_zero != 0U ||
        handle.slot == 0U || handle.generation == 0U) {
        return false;
    }
    slot = (uint16_t)(handle.slot - 1U);
    if (slot >= count) {
        return false;
    }
    if (slot_out != NULL) {
        *slot_out = slot;
    }
    return true;
}

static ucn_result_t next_generation(uint16_t current, uint16_t *next_out)
{
    if (next_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (current == UINT16_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *next_out = current == 0U ? 1U : (uint16_t)(current + 1U);
    return UCN_OK;
}

static ucn_result_t api_enter(ucn_node_t *node,
                              uint32_t operation_id,
                              ucn_i_callback_claim_t *claim_out)
{
    ucn_i_callback_claim_t claim;
    bool adapter_faulted;
    ucn_result_t result;

    if (!node_is_valid(node) || claim_out == NULL || operation_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&claim, 0, sizeof(claim));
    claim.owner_instance = node->owner_instance;
    claim.operation_id = operation_id;
    claim.operation_generation = 1U;
    claim.operation_kind = UCN_I_OPERATION_API;
    result = ucn_i_callback_gate_enter(&node->callback_gate, &claim);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!node_owned_state_is_valid(node)) {
        (void)ucn_i_callback_gate_leave(&node->callback_gate, &claim);
        return UCN_ERR_STATE;
    }
    result = ucn_i_adapter_faulted(&node->adapter, &adapter_faulted);
    if (result != UCN_OK) {
        (void)ucn_i_callback_gate_leave(&node->callback_gate, &claim);
        return UCN_ERR_STATE;
    }
    if (adapter_faulted) {
        node->faulted = 1U;
        if (node->lifecycle != UCN_LIFECYCLE_STOPPING &&
            node->lifecycle != UCN_LIFECYCLE_QUIESCENT) {
            node->lifecycle = UCN_LIFECYCLE_FAULT;
        }
    }
    *claim_out = claim;
    return UCN_OK;
}

static ucn_result_t api_leave(ucn_node_t *node,
                              const ucn_i_callback_claim_t *claim,
                              ucn_result_t result)
{
    if (ucn_i_callback_gate_leave(&node->callback_gate, claim) != UCN_OK) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return UCN_ERR_STATE;
    }
    return result;
}

static ucn_result_t callback_read_snapshot_begin(ucn_node_t *node,
                                                 ucn_callback_scope_t *scope_out)
{
    ucn_i_callback_read_snapshot_t *snapshot;
    uint16_t index;
    ucn_result_t result;

    if (!node_is_valid(node) || scope_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    snapshot = &node->callback_read_snapshot;
    result = node->lock.enter(node->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (snapshot->active != 0U) {
        node->lock.leave(node->lock.context);
        return UCN_ERR_STATE;
    }
    node->lock.leave(node->lock.context);
    memset(snapshot, 0, sizeof(*snapshot));
    if (node->callback_read_nonce == UINT64_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    ++node->callback_read_nonce;
    snapshot->scope.runtime_instance = node->runtime_instance;
    snapshot->scope.owner_instance = node->owner_instance;
    snapshot->scope.nonce = node->callback_read_nonce;
    snapshot->stats = node->stats;
    for (index = 0U; index < UCN_REQUEST_COUNT; ++index) {
        uint16_t receipt_slot;

        if (!node->requests[index].valid) {
            continue;
        }
        receipt_slot = node->requests[index].receipt_slot;
        if (receipt_slot >= UCN_RECEIPT_COUNT ||
            !node->receipts[receipt_slot].valid) {
            memset(snapshot, 0, sizeof(*snapshot));
            return UCN_ERR_STATE;
        }
        snapshot->sends[index].valid = 1U;
        snapshot->sends[index].request_generation =
            node->requests[index].generation;
        snapshot->sends[index].view = node->receipts[receipt_slot].view;
    }
    for (index = 0U; index < node->link_count; ++index) {
        result = ucn_i_adapter_link_handle(&node->adapter, index,
                                           &snapshot->links[index]);
        if (result != UCN_OK) {
            memset(snapshot, 0, sizeof(*snapshot));
            return UCN_ERR_STATE;
        }
    }
    snapshot->link_count = node->link_count;
    result = node->lock.enter(node->lock.context);
    if (result != UCN_OK) {
        memset(snapshot, 0, sizeof(*snapshot));
        return UCN_ERR_STATE;
    }
    snapshot->active = 1U;
    node->lock.leave(node->lock.context);
    *scope_out = snapshot->scope;
    return UCN_OK;
}

static bool callback_read_scope_matches(const ucn_node_t *node,
                                        ucn_callback_scope_t scope)
{
    return scope.runtime_instance == node->runtime_instance &&
           scope.owner_instance == node->owner_instance &&
           scope.reserved_zero == 0U && scope.nonce != 0U &&
           node->callback_read_snapshot.active == 1U &&
           node->callback_read_snapshot.scope.runtime_instance ==
               scope.runtime_instance &&
           node->callback_read_snapshot.scope.owner_instance ==
               scope.owner_instance &&
           node->callback_read_snapshot.scope.reserved_zero == 0U &&
           node->callback_read_snapshot.scope.nonce == scope.nonce;
}

static ucn_result_t callback_read_snapshot_end(ucn_node_t *node)
{
    ucn_result_t result;

    result = node->lock.enter(node->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (node->callback_read_snapshot.active != 1U) {
        node->lock.leave(node->lock.context);
        return UCN_ERR_STATE;
    }
    memset(&node->callback_read_snapshot.scope, 0,
           sizeof(node->callback_read_snapshot.scope));
    node->callback_read_snapshot.link_count = 0U;
    node->callback_read_snapshot.active = 0U;
    node->lock.leave(node->lock.context);
    return UCN_OK;
}

static ucn_result_t callback_read_stats(const ucn_node_t *node,
                                        ucn_callback_scope_t scope,
                                        ucn_stats_t *stats_out)
{
    ucn_node_t *mutable_node = (ucn_node_t *)node;
    ucn_stats_t stats;
    ucn_result_t result;

    result = mutable_node->lock.enter(mutable_node->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!callback_read_scope_matches(mutable_node, scope)) {
        mutable_node->lock.leave(mutable_node->lock.context);
        return UCN_ERR_STATE;
    }
    stats = mutable_node->callback_read_snapshot.stats;
    mutable_node->lock.leave(mutable_node->lock.context);
    *stats_out = stats;
    return UCN_OK;
}

static ucn_result_t callback_read_link(const ucn_node_t *node,
                                       ucn_callback_scope_t scope,
                                       uint16_t link_index,
                                       ucn_link_handle_t *link_out)
{
    ucn_node_t *mutable_node = (ucn_node_t *)node;
    ucn_result_t result;

    result = mutable_node->lock.enter(mutable_node->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!callback_read_scope_matches(mutable_node, scope)) {
        mutable_node->lock.leave(mutable_node->lock.context);
        return UCN_ERR_STATE;
    }
    if (link_index >= mutable_node->callback_read_snapshot.link_count) {
        mutable_node->lock.leave(mutable_node->lock.context);
        return UCN_ERR_NOT_FOUND;
    }
    *link_out = mutable_node->callback_read_snapshot.links[link_index];
    mutable_node->lock.leave(mutable_node->lock.context);
    return UCN_OK;
}

static ucn_result_t callback_read_send(const ucn_node_t *node,
                                       ucn_callback_scope_t scope,
                                       ucn_send_handle_t handle,
                                       ucn_send_view_t *view_out)
{
    ucn_node_t *mutable_node = (ucn_node_t *)node;
    ucn_send_view_t view;
    uint16_t slot;
    ucn_result_t result;

    result = mutable_node->lock.enter(mutable_node->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!callback_read_scope_matches(mutable_node, scope)) {
        mutable_node->lock.leave(mutable_node->lock.context);
        return UCN_ERR_STATE;
    }
    if (!handle_matches(node, handle, UCN_OBJECT_KIND_SEND,
                        UCN_REQUEST_COUNT, &slot) ||
        !mutable_node->callback_read_snapshot.sends[slot].valid ||
        mutable_node->callback_read_snapshot.sends[slot]
                .request_generation != handle.generation) {
        mutable_node->lock.leave(mutable_node->lock.context);
        return UCN_ERR_NOT_FOUND;
    }
    view = mutable_node->callback_read_snapshot.sends[slot].view;
    mutable_node->lock.leave(mutable_node->lock.context);
    *view_out = view;
    return UCN_OK;
}

static bool binding_is_present(const ucn_node_t *node,
                               uint32_t address,
                               uint32_t generation)
{
    uint16_t index;

    for (index = 0U; index < node->binding_count; ++index) {
        if (node->bindings[index].address == address &&
            node->bindings[index].binding_generation == generation) {
            return true;
        }
    }
    return false;
}

static bool source_binding_find(const ucn_node_t *node,
                                uint32_t address,
                                uint16_t *slot_out)
{
    uint16_t index;

    for (index = 0U; index < node->binding_count; ++index) {
        if (node->bindings[index].address == address) {
            *slot_out = index;
            return true;
        }
    }
    return false;
}

static bool config_is_valid(const void *storage,
                            size_t storage_bytes,
                            const ucn_config_t *config,
                            const ucn_ports_t *ports,
                            ucn_node_t **out_node)
{
    uint16_t left;
    uint16_t right;
    uint16_t local_matches = 0U;

    if (storage == NULL || storage_bytes < UCN_STORAGE_BYTES ||
        ((uintptr_t)storage % UCN_STORAGE_ALIGNMENT) != 0U || config == NULL ||
        ports == NULL || out_node == NULL ||
        config->struct_size != sizeof(*config) ||
        config->api_version != UCN_API_VERSION ||
        config->storage_layout != UCN_STORAGE_LAYOUT ||
        config->manifest_reserved_zero != 0U ||
        config->compiled_manifest_hash != UCN_COMPILED_MANIFEST_HASH ||
        config->reserved_zero != 0U ||
        config->runtime_instance == 0U || config->realm_id == 0U ||
        !address_is_valid(config->local_address, config->address_width) ||
        config->local_binding_generation == 0U ||
        config->local_principal_digest == 0U ||
        config->trusted_o0_network != 1U || config->bindings == NULL ||
        config->binding_count == 0U ||
        config->binding_count > UCN_BINDING_COUNT ||
        ports->struct_size != sizeof(*ports) ||
        ports->api_version != UCN_API_VERSION || ports->reserved_zero != 0U ||
        ports->links == NULL || ports->link_count == 0U ||
        ports->link_count > UCN_LINK_COUNT ||
        !public_lock_is_valid(&ports->state_lock) ||
        !public_lock_is_valid(&ports->driver_callback_gate) ||
        (ports->state_lock.context ==
             ports->driver_callback_gate.context &&
         ports->state_lock.enter == ports->driver_callback_gate.enter &&
         ports->state_lock.leave == ports->driver_callback_gate.leave) ||
        ucn_i_ranges_overlap(storage, UCN_STORAGE_BYTES, config,
                             sizeof(*config)) ||
        ucn_i_ranges_overlap(storage, UCN_STORAGE_BYTES, ports,
                             sizeof(*ports)) ||
        ucn_i_ranges_overlap(storage, UCN_STORAGE_BYTES, config->bindings,
                             config->binding_count * sizeof(config->bindings[0])) ||
        ucn_i_ranges_overlap(storage, UCN_STORAGE_BYTES, ports->links,
                             ports->link_count * sizeof(ports->links[0])) ||
        ucn_i_ranges_overlap(storage, UCN_STORAGE_BYTES, out_node,
                             sizeof(*out_node)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), ports,
                             sizeof(*ports)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), config->bindings,
                             config->binding_count * sizeof(config->bindings[0])) ||
        ucn_i_ranges_overlap(config, sizeof(*config), ports->links,
                             ports->link_count * sizeof(ports->links[0])) ||
        ucn_i_ranges_overlap(ports, sizeof(*ports), config->bindings,
                             config->binding_count * sizeof(config->bindings[0])) ||
        ucn_i_ranges_overlap(ports, sizeof(*ports), ports->links,
                             ports->link_count * sizeof(ports->links[0])) ||
        ucn_i_ranges_overlap(config->bindings,
                             config->binding_count * sizeof(config->bindings[0]),
                             ports->links,
                             ports->link_count * sizeof(ports->links[0])) ||
        ucn_i_ranges_overlap(out_node, sizeof(*out_node), config,
                             sizeof(*config)) ||
        ucn_i_ranges_overlap(out_node, sizeof(*out_node), ports,
                             sizeof(*ports)) ||
        ucn_i_ranges_overlap(out_node, sizeof(*out_node), config->bindings,
                             config->binding_count * sizeof(config->bindings[0])) ||
        ucn_i_ranges_overlap(out_node, sizeof(*out_node), ports->links,
                             ports->link_count * sizeof(ports->links[0])) ||
        (ports->state_lock.context != NULL &&
         ucn_i_ranges_overlap(out_node, sizeof(*out_node),
                              ports->state_lock.context, 1U)) ||
        (ports->driver_callback_gate.context != NULL &&
         ucn_i_ranges_overlap(out_node, sizeof(*out_node),
                              ports->driver_callback_gate.context, 1U)) ||
        (ports->state_lock.context != NULL &&
         ucn_i_ranges_overlap(storage, UCN_STORAGE_BYTES,
                              ports->state_lock.context, 1U)) ||
        (ports->driver_callback_gate.context != NULL &&
         ucn_i_ranges_overlap(storage, UCN_STORAGE_BYTES,
                              ports->driver_callback_gate.context, 1U)) ||
        !bytes_are_zero(storage, UCN_STORAGE_BYTES)) {
        return false;
    }
    for (left = 0U; left < config->binding_count; ++left) {
        const ucn_static_binding_t *binding = &config->bindings[left];
        if (binding->struct_size != sizeof(*binding) ||
            binding->api_version != UCN_API_VERSION ||
            !address_is_valid(binding->address, config->address_width) ||
            binding->binding_generation == 0U ||
            binding->principal_digest == 0U) {
            return false;
        }
        if (binding->address == config->local_address &&
            binding->binding_generation == config->local_binding_generation &&
            binding->principal_digest == config->local_principal_digest) {
            ++local_matches;
        }
        for (right = (uint16_t)(left + 1U); right < config->binding_count;
             ++right) {
            if (binding->address == config->bindings[right].address ||
                binding->principal_digest ==
                    config->bindings[right].principal_digest) {
                return false;
            }
        }
    }
    for (left = 0U; left < ports->link_count; ++left) {
        if (ports->links[left].context != NULL) {
            if (ucn_i_ranges_overlap(storage, UCN_STORAGE_BYTES,
                                     ports->links[left].context, 1U) ||
                ucn_i_ranges_overlap(out_node, sizeof(*out_node),
                                     ports->links[left].context, 1U)) {
                return false;
            }
        }
    }
    return local_matches == 1U;
}

size_t ucn_storage_required(void)
{
    return UCN_STORAGE_BYTES;
}

ucn_result_t ucn_init(void *storage,
                      size_t storage_bytes,
                      const ucn_config_t *config,
                      const ucn_ports_t *ports,
                      ucn_node_t **out_node)
{
    ucn_node_t *node;
    uint16_t index;
    ucn_result_t result;

    if (!config_is_valid(storage, storage_bytes, config, ports, out_node)) {
        return UCN_ERR_CONFIG;
    }
    node = (ucn_node_t *)storage;
    node->runtime_instance = config->runtime_instance;
    node->realm_id = config->realm_id;
    node->local_address = config->local_address;
    node->local_binding_generation = config->local_binding_generation;
    node->local_principal_digest = config->local_principal_digest;
    node->compiled_manifest_hash = config->compiled_manifest_hash;
    node->schema = UCN_I_NODE_SCHEMA;
    node->storage_layout = config->storage_layout;
    node->owner_instance = UCN_I_CORE_OWNER_INSTANCE;
    node->binding_count = config->binding_count;
    node->link_count = ports->link_count;
    node->address_width = config->address_width;
    node->trusted_o0_network = config->trusted_o0_network;
    node->lifecycle = UCN_LIFECYCLE_INITIALIZED;
    node->lock = make_internal_lock(&ports->state_lock);
    node->stats.struct_size = sizeof(node->stats);
    node->stats.api_version = UCN_API_VERSION;
    for (index = 0U; index < config->binding_count; ++index) {
        node->bindings[index].address = config->bindings[index].address;
        node->bindings[index].binding_generation =
            config->bindings[index].binding_generation;
        node->bindings[index].principal_digest =
            config->bindings[index].principal_digest;
    }
    result = ucn_i_adapter_init(&node->adapter, node->runtime_instance,
                                UCN_I_ADAPTER_OWNER_INSTANCE, ports);
    if (result != UCN_OK) {
        memset(storage, 0, UCN_STORAGE_BYTES);
        return result;
    }
    result = ucn_i_callback_gate_init(&node->callback_gate,
                                      node->runtime_instance, &node->lock);
    if (result != UCN_OK) {
        (void)ucn_i_adapter_destroy(&node->adapter);
        memset(storage, 0, UCN_STORAGE_BYTES);
        return result;
    }
    node->magic = UCN_I_NODE_MAGIC;
    *out_node = node;
    return UCN_OK;
}

ucn_result_t ucn_start(ucn_node_t *node, uint64_t now_us)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    result = api_enter(node, 1U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    if (node->faulted || node->lifecycle != UCN_LIFECYCLE_INITIALIZED) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    if (node->time_initialized && now_us < node->last_now_us) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    if (ucn_i_adapter_set_rx_enabled(&node->adapter, true) != UCN_OK) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    node->last_now_us = now_us;
    node->time_initialized = 1U;
    node->lifecycle = UCN_LIFECYCLE_RUNNING;
    return api_leave(node, &claim, UCN_OK);
}

static bool endpoint_handle_matches(const ucn_node_t *node,
                                    ucn_endpoint_handle_t handle,
                                    uint16_t *slot_out)
{
    uint16_t slot;

    if (!handle_matches(node, handle, UCN_OBJECT_KIND_ENDPOINT,
                        UCN_ENDPOINT_COUNT, &slot) ||
        !node->endpoints[slot].valid ||
        node->endpoints[slot].generation != handle.generation) {
        return false;
    }
    *slot_out = slot;
    return true;
}

ucn_result_t ucn_endpoint_add(ucn_node_t *node,
                              const ucn_endpoint_config_t *config,
                              ucn_endpoint_handle_t *out_endpoint)
{
    ucn_i_callback_claim_t claim;
    uint16_t index;
    uint16_t free_slot = UCN_I_NO_SLOT;
    uint16_t generation;
    bool busy = false;
    bool exhausted = false;
    ucn_endpoint_handle_t result_handle;
    ucn_result_t result;

    if (!node_is_valid(node) || config == NULL || out_endpoint == NULL ||
        config->struct_size != sizeof(*config) ||
        config->api_version != UCN_API_VERSION || config->reserved_zero != 0U ||
        config->service_id == 0U || config->service_id == UINT16_MAX ||
        config->receive == NULL ||
        ucn_i_ranges_overlap(node, sizeof(*node), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_endpoint,
                             sizeof(*out_endpoint)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), out_endpoint,
                             sizeof(*out_endpoint)) ||
        (config->context != NULL &&
         ucn_i_ranges_overlap(config->context, 1U, out_endpoint,
                              sizeof(*out_endpoint))) ||
        (config->context != NULL &&
         ucn_i_ranges_overlap(node, sizeof(*node), config->context, 1U))) {
        return UCN_ERR_ARGUMENT;
    }
    result = api_enter(node, 2U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    if ((node->lifecycle != UCN_LIFECYCLE_INITIALIZED &&
         node->lifecycle != UCN_LIFECYCLE_RUNNING) ||
        node->faulted) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    for (index = 0U; index < UCN_ENDPOINT_COUNT; ++index) {
        uint16_t candidate = (uint16_t)(
            (node->endpoint_allocate_cursor + index) % UCN_ENDPOINT_COUNT);
        if (node->endpoints[candidate].valid &&
            node->endpoints[candidate].service_id == config->service_id) {
            return api_leave(node, &claim, UCN_ERR_STATE);
        }
        if (node->endpoints[candidate].valid) {
            busy = true;
        } else if (node->endpoints[candidate].generation == UINT16_MAX) {
            exhausted = true;
        } else if (free_slot == UCN_I_NO_SLOT) {
            free_slot = candidate;
        }
    }
    if (free_slot == UCN_I_NO_SLOT) {
        if (!busy && exhausted) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return api_leave(node, &claim, UCN_ERR_EXHAUSTED);
        }
        return api_leave(node, &claim, UCN_ERR_NO_SPACE);
    }
    result = next_generation(node->endpoints[free_slot].generation,
                             &generation);
    if (result != UCN_OK) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return api_leave(node, &claim, result);
    }
    memset(&node->endpoints[free_slot], 0, sizeof(node->endpoints[free_slot]));
    node->endpoints[free_slot].receive = config->receive;
    node->endpoints[free_slot].context = config->context;
    node->endpoints[free_slot].service_id = config->service_id;
    node->endpoints[free_slot].generation = generation;
    node->endpoints[free_slot].valid = 1U;
    node->endpoint_allocate_cursor =
        (uint16_t)((free_slot + 1U) % UCN_ENDPOINT_COUNT);
    result_handle = make_core_handle(node, free_slot, generation,
                                     UCN_OBJECT_KIND_ENDPOINT);
    result = api_leave(node, &claim, UCN_OK);
    if (result == UCN_OK) {
        *out_endpoint = result_handle;
    }
    return result;
}

ucn_result_t ucn_endpoint_remove(ucn_node_t *node,
                                 ucn_endpoint_handle_t endpoint)
{
    ucn_i_callback_claim_t claim;
    uint16_t slot;
    uint16_t generation;
    ucn_result_t result = api_enter(node, 3U, &claim);

    if (result != UCN_OK) {
        return result;
    }
    if (!endpoint_handle_matches(node, endpoint, &slot)) {
        return api_leave(node, &claim, UCN_ERR_NOT_FOUND);
    }
    if (node->endpoints[slot].callback_active) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    generation = node->endpoints[slot].generation;
    memset(&node->endpoints[slot], 0, sizeof(node->endpoints[slot]));
    node->endpoints[slot].generation = generation;
    return api_leave(node, &claim, UCN_OK);
}

static bool path_handle_matches(const ucn_node_t *node,
                                ucn_path_handle_t handle,
                                uint16_t *slot_out)
{
    uint16_t slot;

    if (!handle_matches(node, handle, UCN_OBJECT_KIND_PATH,
                        UCN_STATIC_PATH_COUNT, &slot) ||
        !node->paths[slot].valid ||
        node->paths[slot].generation != handle.generation) {
        return false;
    }
    *slot_out = slot;
    return true;
}

ucn_result_t ucn_static_path_add(ucn_node_t *node,
                                 const ucn_static_path_t *path,
                                 ucn_path_handle_t *out_path)
{
    ucn_i_callback_claim_t claim;
    uint32_t link_instance;
    uint16_t link_mtu;
    uint16_t slot;
    uint16_t free_slot = UCN_I_NO_SLOT;
    uint16_t generation;
    ucn_path_handle_t result_handle;
    size_t header_bytes;
    bool busy = false;
    bool exhausted = false;
    bool link_ready;
    ucn_result_t result;

    if (!node_is_valid(node) || path == NULL || out_path == NULL ||
        path->struct_size != sizeof(*path) ||
        path->api_version != UCN_API_VERSION ||
        path->destination_address == node->local_address ||
        !binding_is_present(node, path->destination_address,
                            path->destination_binding_generation) ||
        path->link_index >= node->link_count ||
        ucn_i_ranges_overlap(node, sizeof(*node), path, sizeof(*path)) ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_path,
                             sizeof(*out_path)) ||
        ucn_i_ranges_overlap(path, sizeof(*path), out_path,
                             sizeof(*out_path)) ||
        ucn_i_c1_header_bytes(node->address_width, &header_bytes) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_adapter_link_snapshot(&node->adapter, path->link_index,
                                         &link_instance, &link_mtu,
                                         &link_ready);
    if (result != UCN_OK || !link_ready ||
        path->path_frame_mtu < header_bytes ||
        path->path_frame_mtu > link_mtu) {
        if (result != UCN_OK) {
            return result;
        }
        return !link_ready ? UCN_ERR_STATE : UCN_ERR_CONFIG;
    }
    result = api_enter(node, 4U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    if ((node->lifecycle != UCN_LIFECYCLE_INITIALIZED &&
         node->lifecycle != UCN_LIFECYCLE_RUNNING) ||
        node->faulted) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    for (slot = 0U; slot < UCN_STATIC_PATH_COUNT; ++slot) {
        uint16_t candidate = (uint16_t)(
            (node->path_allocate_cursor + slot) % UCN_STATIC_PATH_COUNT);
        if (node->paths[candidate].valid &&
            node->paths[candidate].destination_address == path->destination_address &&
            node->paths[candidate].destination_binding_generation ==
                path->destination_binding_generation) {
            return api_leave(node, &claim, UCN_ERR_STATE);
        }
        if (node->paths[candidate].valid) {
            busy = true;
        } else if (node->paths[candidate].generation == UINT16_MAX) {
            exhausted = true;
        } else if (free_slot == UCN_I_NO_SLOT) {
            free_slot = candidate;
        }
    }
    if (free_slot == UCN_I_NO_SLOT) {
        if (!busy && exhausted) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return api_leave(node, &claim, UCN_ERR_EXHAUSTED);
        }
        return api_leave(node, &claim, UCN_ERR_NO_SPACE);
    }
    slot = free_slot;
    result = next_generation(node->paths[slot].generation, &generation);
    if (result != UCN_OK) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return api_leave(node, &claim, result);
    }
    memset(&node->paths[slot], 0, sizeof(node->paths[slot]));
    node->paths[slot].destination_address = path->destination_address;
    node->paths[slot].destination_binding_generation =
        path->destination_binding_generation;
    node->paths[slot].link_instance_generation = link_instance;
    node->paths[slot].link_index = path->link_index;
    node->paths[slot].path_frame_mtu = path->path_frame_mtu;
    node->paths[slot].generation = generation;
    node->paths[slot].valid = 1U;
    node->path_allocate_cursor =
        (uint16_t)((slot + 1U) % UCN_STATIC_PATH_COUNT);
    result_handle = make_core_handle(node, slot, generation,
                                     UCN_OBJECT_KIND_PATH);
    result = api_leave(node, &claim, UCN_OK);
    if (result == UCN_OK) {
        *out_path = result_handle;
    }
    return result;
}

ucn_result_t ucn_static_path_remove(ucn_node_t *node,
                                    ucn_path_handle_t path)
{
    ucn_i_callback_claim_t claim;
    uint16_t slot;
    uint16_t generation;
    ucn_result_t result = api_enter(node, 5U, &claim);

    if (result != UCN_OK) {
        return result;
    }
    if (!path_handle_matches(node, path, &slot)) {
        return api_leave(node, &claim, UCN_ERR_NOT_FOUND);
    }
    if (node->paths[slot].references != 0U) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    generation = node->paths[slot].generation;
    memset(&node->paths[slot], 0, sizeof(node->paths[slot]));
    node->paths[slot].generation = generation;
    return api_leave(node, &claim, UCN_OK);
}

ucn_result_t ucn_link_get(const ucn_node_t *node,
                          uint16_t link_index,
                          ucn_link_handle_t *out_link)
{
    ucn_node_t *mutable_node = (ucn_node_t *)node;
    ucn_i_callback_claim_t claim;
    ucn_link_handle_t link;
    ucn_result_t result;

    if (!node_is_valid(node) || out_link == NULL ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_link,
                             sizeof(*out_link))) {
        return UCN_ERR_ARGUMENT;
    }
    result = api_enter(mutable_node, 12U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_adapter_link_handle(&mutable_node->adapter, link_index,
                                       &link);
    result = api_leave(mutable_node, &claim, result);
    if (result == UCN_OK) {
        *out_link = link;
    }
    return result;
}

static bool handle_is_zero(ucn_handle_t handle)
{
    return bytes_are_zero(&handle, sizeof(handle));
}

static bool send_options_are_well_formed(const ucn_send_options_t *options)
{
    return options != NULL && options->struct_size == sizeof(*options) &&
           options->api_version == UCN_API_VERSION &&
           options->traffic_class < UCN_TRAFFIC_CLASS_COUNT &&
           options->delivery_guarantee <= UCN_DELIVERY_RELIABLE &&
           options->interaction_role <= UCN_INTERACTION_ERROR &&
           options->hop_limit != 0U && options->hop_limit <= 63U &&
           options->copy_payload <= 1U &&
           bytes_are_zero(options->reserved_zero,
                          sizeof(options->reserved_zero));
}

static bool send_options_are_supported(const ucn_send_options_t *options)
{
    return options->delivery_guarantee == UCN_DELIVERY_BEST_EFFORT &&
           options->interaction_role == UCN_INTERACTION_ONE_WAY &&
           options->copy_payload == 1U;
}

static ucn_result_t select_path(const ucn_node_t *node,
                                const ucn_target_t *target,
                                ucn_path_handle_t pinned,
                                uint16_t *path_slot_out)
{
    uint16_t slot;

    if (!handle_is_zero(pinned)) {
        if (!path_handle_matches(node, pinned, &slot) ||
            node->paths[slot].destination_address != target->address ||
            node->paths[slot].destination_binding_generation !=
                target->binding_generation) {
            return UCN_ERR_NOT_FOUND;
        }
        *path_slot_out = slot;
        return UCN_OK;
    }
    for (slot = 0U; slot < UCN_STATIC_PATH_COUNT; ++slot) {
        if (node->paths[slot].valid &&
            node->paths[slot].destination_address == target->address &&
            node->paths[slot].destination_binding_generation ==
                target->binding_generation) {
            *path_slot_out = slot;
            return UCN_OK;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

static void queue_bounds(uint8_t traffic_class,
                         uint16_t *begin_out,
                         uint16_t *count_out)
{
    static const uint16_t counts[UCN_TRAFFIC_CLASS_COUNT] = {
        UCN_Q0_DEPTH, UCN_Q1_DEPTH, UCN_Q2_DEPTH, UCN_Q3_DEPTH};
    uint16_t begin = 0U;
    uint8_t index;

    for (index = 0U; index < traffic_class; ++index) {
        begin = (uint16_t)(begin + counts[index]);
    }
    *begin_out = begin;
    *count_out = counts[traffic_class];
}

static ucn_result_t find_free_tx(const ucn_node_t *node,
                                 uint8_t traffic_class,
                                 uint16_t *slot_out)
{
    uint16_t begin;
    uint16_t count;
    uint16_t index;

    queue_bounds(traffic_class, &begin, &count);
    for (index = 0U; index < count; ++index) {
        uint16_t offset = (uint16_t)(
            (node->queue_allocate_cursor[traffic_class] + index) % count);
        uint16_t slot = (uint16_t)(begin + offset);
        if (node->tx_slots[slot].state == UCN_I_TX_FREE) {
            *slot_out = slot;
            return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

static UCN_I_NOINLINE ucn_result_t find_free_request(const ucn_node_t *node,
                                      uint16_t *request_out,
                                      uint16_t *receipt_out,
                                      uint16_t *attempt_out,
                                      uint16_t *buffer_out,
                                      uint16_t *request_generation_out,
                                      uint16_t *receipt_generation_out,
                                      uint16_t *attempt_generation_out,
                                      uint16_t *buffer_generation_out)
{
    uint16_t request;
    uint16_t receipt;
    uint16_t attempt;
    uint16_t buffer;
    bool request_busy = false;
    bool receipt_busy = false;
    bool attempt_busy = false;
    bool buffer_busy = false;
    bool request_exhausted = false;
    bool receipt_exhausted = false;
    bool attempt_exhausted = false;
    bool buffer_exhausted = false;

    for (request = 0U; request < UCN_REQUEST_COUNT; ++request) {
        uint16_t candidate = (uint16_t)(
            (node->request_allocate_cursor + request) % UCN_REQUEST_COUNT);
        if (node->requests[candidate].valid) {
            request_busy = true;
        } else if (node->requests[candidate].generation == UINT16_MAX) {
            request_exhausted = true;
        } else {
            request = candidate;
            break;
        }
    }
    for (receipt = 0U; receipt < UCN_RECEIPT_COUNT; ++receipt) {
        uint16_t candidate = (uint16_t)(
            (node->receipt_allocate_cursor + receipt) % UCN_RECEIPT_COUNT);
        if (node->receipts[candidate].valid) {
            receipt_busy = true;
        } else if (node->receipts[candidate].generation == UINT16_MAX) {
            receipt_exhausted = true;
        } else {
            receipt = candidate;
            break;
        }
    }
    for (attempt = 0U; attempt < UCN_ATTEMPT_COUNT; ++attempt) {
        uint16_t candidate = (uint16_t)(
            (node->attempt_allocate_cursor + attempt) % UCN_ATTEMPT_COUNT);
        if (node->attempts[candidate].valid) {
            attempt_busy = true;
        } else if (node->attempts[candidate].generation == UINT16_MAX) {
            attempt_exhausted = true;
        } else {
            attempt = candidate;
            break;
        }
    }
    for (buffer = 0U; buffer < UCN_BUFFER_OBLIGATION_COUNT; ++buffer) {
        uint16_t candidate = (uint16_t)(
            (node->buffer_allocate_cursor + buffer) %
            UCN_BUFFER_OBLIGATION_COUNT);
        if (node->buffers[candidate].valid) {
            buffer_busy = true;
        } else if (node->buffers[candidate].generation == UINT16_MAX) {
            buffer_exhausted = true;
        } else {
            buffer = candidate;
            break;
        }
    }
    if (request == UCN_REQUEST_COUNT || receipt == UCN_RECEIPT_COUNT ||
        attempt == UCN_ATTEMPT_COUNT ||
        buffer == UCN_BUFFER_OBLIGATION_COUNT) {
        if ((request == UCN_REQUEST_COUNT && !request_busy &&
             request_exhausted) ||
            (receipt == UCN_RECEIPT_COUNT && !receipt_busy &&
             receipt_exhausted) ||
            (attempt == UCN_ATTEMPT_COUNT && !attempt_busy &&
             attempt_exhausted) ||
            (buffer == UCN_BUFFER_OBLIGATION_COUNT && !buffer_busy &&
             buffer_exhausted)) {
            return UCN_ERR_EXHAUSTED;
        }
        return UCN_ERR_NO_SPACE;
    }
    if (next_generation(node->requests[request].generation,
                        request_generation_out) != UCN_OK ||
        next_generation(node->receipts[receipt].generation,
                        receipt_generation_out) != UCN_OK ||
        next_generation(node->attempts[attempt].generation,
                        attempt_generation_out) != UCN_OK ||
        next_generation(node->buffers[buffer].generation,
                        buffer_generation_out) != UCN_OK) {
        return UCN_ERR_EXHAUSTED;
    }
    *request_out = request;
    *receipt_out = receipt;
    *attempt_out = attempt;
    *buffer_out = buffer;
    return UCN_OK;
}

static ucn_result_t next_sequence(const ucn_node_t *node,
                                  uint32_t *sequence_out)
{
    if (node->last_origin_sequence == UINT32_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *sequence_out = node->last_origin_sequence + 1U;
    return UCN_OK;
}

static UCN_I_NOINLINE bool publish_inputs_are_valid(
    const ucn_node_t *node,
    const ucn_target_t *target,
    const void *payload,
    size_t payload_bytes,
    const ucn_send_options_t *options,
    const ucn_send_handle_t *out_handle)
{
    return node_is_valid(node) && target != NULL &&
           target->struct_size == sizeof(*target) &&
           target->api_version == UCN_API_VERSION &&
           target->reserved_zero == 0U &&
           address_is_valid(target->address, node->address_width) &&
           target->binding_generation != 0U && target->service_id != 0U &&
           target->service_id != UINT16_MAX &&
           (payload != NULL || payload_bytes == 0U) &&
           send_options_are_well_formed(options) &&
           !ucn_i_ranges_overlap(node, sizeof(*node), target,
                                 sizeof(*target)) &&
           !ucn_i_ranges_overlap(node, sizeof(*node), options,
                                 sizeof(*options)) &&
           (payload_bytes == 0U ||
            !ucn_i_ranges_overlap(node, sizeof(*node), payload,
                                  payload_bytes)) &&
           (out_handle == NULL ||
            (!ucn_i_ranges_overlap(node, sizeof(*node), out_handle,
                                   sizeof(*out_handle)) &&
             !ucn_i_ranges_overlap(target, sizeof(*target), out_handle,
                                   sizeof(*out_handle)) &&
             !ucn_i_ranges_overlap(options, sizeof(*options), out_handle,
                                   sizeof(*out_handle)) &&
             (payload_bytes == 0U ||
              !ucn_i_ranges_overlap(payload, payload_bytes, out_handle,
                                    sizeof(*out_handle))) &&
             (options->completion_context == NULL ||
              !ucn_i_ranges_overlap(options->completion_context, 1U,
                                    out_handle, sizeof(*out_handle))))) &&
           (options->completion_context == NULL ||
            !ucn_i_ranges_overlap(node, sizeof(*node),
                                  options->completion_context, 1U));
}

static UCN_I_NOINLINE ucn_result_t publish_admit_locked(
    ucn_node_t *node,
    const ucn_target_t *target,
    const void *payload,
    size_t payload_bytes,
    const ucn_send_options_t *options,
    size_t frame_bytes,
    bool tracked,
    ucn_send_handle_t *result_handle_out)
{
    uint16_t path_slot;
    uint16_t tx_slot;
    uint16_t request_slot = UCN_I_NO_SLOT;
    uint16_t receipt_slot = UCN_I_NO_SLOT;
    uint16_t attempt_slot = UCN_I_NO_SLOT;
    uint16_t buffer_slot = UCN_I_NO_SLOT;
    uint16_t request_generation = 0U;
    uint16_t receipt_generation = 0U;
    uint16_t attempt_generation = 0U;
    uint16_t buffer_generation = 0U;
    uint32_t sequence;
    ucn_result_t result;

    if (node->lifecycle != UCN_LIFECYCLE_RUNNING || node->faulted) {
        return UCN_ERR_STATE;
    }
    if (options->absolute_deadline_us != 0U &&
        ucn_i_deadline_expired_us(node->last_now_us,
                                  options->absolute_deadline_us)) {
        return UCN_ERR_TIMEOUT;
    }
    if (target->address == node->local_address ||
        !binding_is_present(node, target->address,
                            target->binding_generation)) {
        return UCN_ERR_NOT_FOUND;
    }
    result = select_path(node, target, options->pinned_path, &path_slot);
    if (result != UCN_OK) {
        return result;
    }
    {
        uint32_t link_instance;
        uint16_t link_mtu;
        bool link_ready;
        result = ucn_i_adapter_link_snapshot(
            &node->adapter, node->paths[path_slot].link_index,
            &link_instance, &link_mtu, &link_ready);
        if (result != UCN_OK || !link_ready ||
            link_instance != node->paths[path_slot].link_instance_generation) {
            return result == UCN_OK ? UCN_ERR_NOT_FOUND : result;
        }
        (void)link_mtu;
    }
    if (frame_bytes > node->paths[path_slot].path_frame_mtu) {
        return UCN_ERR_NO_SPACE;
    }
    result = find_free_tx(node, options->traffic_class, &tx_slot);
    if (result != UCN_OK) {
        increment_saturated(&node->stats.no_space);
        return result;
    }
    if (tracked) {
        result = find_free_request(
            node, &request_slot, &receipt_slot, &attempt_slot, &buffer_slot,
            &request_generation, &receipt_generation, &attempt_generation,
            &buffer_generation);
        if (result != UCN_OK) {
            increment_saturated(&node->stats.no_space);
            if (result == UCN_ERR_EXHAUSTED) {
                node->faulted = 1U;
                node->lifecycle = UCN_LIFECYCLE_FAULT;
            }
            return result;
        }
    }
    result = next_sequence(node, &sequence);
    if (result != UCN_OK) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return result;
    }
    memset(&node->tx_slots[tx_slot], 0, sizeof(node->tx_slots[tx_slot]));
    node->tx_slots[tx_slot].absolute_deadline_us =
        options->absolute_deadline_us;
    node->tx_slots[tx_slot].destination_address = target->address;
    node->tx_slots[tx_slot].destination_binding_generation =
        target->binding_generation;
    node->tx_slots[tx_slot].origin_sequence = sequence;
    node->tx_slots[tx_slot].service_id = target->service_id;
    node->tx_slots[tx_slot].payload_bytes = (uint16_t)payload_bytes;
    node->tx_slots[tx_slot].path_slot = path_slot;
    node->tx_slots[tx_slot].request_slot = request_slot;
    node->tx_slots[tx_slot].traffic_class = options->traffic_class;
    node->tx_slots[tx_slot].hop_limit = options->hop_limit;
    node->tx_slots[tx_slot].state = UCN_I_TX_QUEUED;
    node->tx_slots[tx_slot].tracked = tracked ? 1U : 0U;
    if (payload_bytes != 0U) {
        memcpy(node->tx_slots[tx_slot].frame, payload, payload_bytes);
    }
    if (tracked) {
        memset(&node->requests[request_slot], 0,
               sizeof(node->requests[request_slot]));
        node->requests[request_slot].completion = options->completion;
        node->requests[request_slot].completion_context =
            options->completion_context;
        node->requests[request_slot].generation = request_generation;
        node->requests[request_slot].tx_slot = tx_slot;
        node->requests[request_slot].receipt_slot = receipt_slot;
        node->requests[request_slot].attempt_slot = attempt_slot;
        node->requests[request_slot].buffer_slot = buffer_slot;
        node->requests[request_slot].valid = 1U;
        memset(&node->receipts[receipt_slot], 0,
               sizeof(node->receipts[receipt_slot]));
        node->receipts[receipt_slot].generation = receipt_generation;
        node->receipts[receipt_slot].valid = 1U;
        node->receipts[receipt_slot].view.struct_size =
            sizeof(node->receipts[receipt_slot].view);
        node->receipts[receipt_slot].view.api_version = UCN_API_VERSION;
        node->receipts[receipt_slot].view.origin_sequence = sequence;
        node->receipts[receipt_slot].view.admission = UCN_SEND_ADMITTED;
        node->receipts[receipt_slot].view.link_outcome =
            UCN_LINK_OUTCOME_PENDING;
        memset(&node->attempts[attempt_slot], 0,
               sizeof(node->attempts[attempt_slot]));
        node->attempts[attempt_slot].generation = attempt_generation;
        node->attempts[attempt_slot].tx_slot = tx_slot;
        node->attempts[attempt_slot].path_slot = path_slot;
        node->attempts[attempt_slot].valid = 1U;
        memset(&node->buffers[buffer_slot], 0,
               sizeof(node->buffers[buffer_slot]));
        node->buffers[buffer_slot].generation = buffer_generation;
        node->buffers[buffer_slot].tx_slot = tx_slot;
        node->buffers[buffer_slot].valid = 1U;
        node->request_allocate_cursor =
            (uint16_t)((request_slot + 1U) % UCN_REQUEST_COUNT);
        node->receipt_allocate_cursor =
            (uint16_t)((receipt_slot + 1U) % UCN_RECEIPT_COUNT);
        node->attempt_allocate_cursor =
            (uint16_t)((attempt_slot + 1U) % UCN_ATTEMPT_COUNT);
        node->buffer_allocate_cursor = (uint16_t)(
            (buffer_slot + 1U) % UCN_BUFFER_OBLIGATION_COUNT);
    }
    {
        uint16_t queue_begin;
        uint16_t queue_count;
        queue_bounds(options->traffic_class, &queue_begin, &queue_count);
        node->queue_allocate_cursor[options->traffic_class] = (uint8_t)(
            (tx_slot - queue_begin + 1U) % queue_count);
    }
    ++node->paths[path_slot].references;
    node->last_origin_sequence = sequence;
    increment_saturated(&node->stats.tx_admitted);
    if (tracked && result_handle_out != NULL) {
        *result_handle_out = make_core_handle(node, request_slot,
                                              request_generation,
                                              UCN_OBJECT_KIND_SEND);
    }
    return UCN_OK;
}

ucn_result_t ucn_publish(ucn_node_t *node,
                         const ucn_target_t *target,
                         const void *payload,
                         size_t payload_bytes,
                         const ucn_send_options_t *options,
                         ucn_send_handle_t *out_handle)
{
    ucn_i_callback_claim_t claim;
    ucn_send_handle_t result_handle;
    size_t frame_bytes;
    bool tracked;
    ucn_result_t result;

    if (!publish_inputs_are_valid(node, target, payload, payload_bytes,
                                  options, out_handle)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!send_options_are_supported(options)) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (ucn_i_c1_encoded_size(node->address_width, payload_bytes,
                              &frame_bytes) != UCN_OK ||
        frame_bytes > UCN_ADAPTER_FRAME_BYTES) {
        return UCN_ERR_NO_SPACE;
    }
    tracked = out_handle != NULL || options->completion != NULL;
    memset(&result_handle, 0, sizeof(result_handle));
    result = api_enter(node, 6U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = publish_admit_locked(node, target, payload, payload_bytes,
                                  options, frame_bytes, tracked,
                                  &result_handle);
    result = api_leave(node, &claim, result);
    if (result == UCN_OK && tracked && out_handle != NULL) {
        *out_handle = result_handle;
    }
    return result;
}

static bool request_handle_matches(const ucn_node_t *node,
                                   ucn_send_handle_t handle,
                                   uint16_t *slot_out)
{
    uint16_t slot;

    if (!handle_matches(node, handle, UCN_OBJECT_KIND_SEND,
                        UCN_REQUEST_COUNT, &slot) ||
        !node->requests[slot].valid ||
        node->requests[slot].generation != handle.generation) {
        return false;
    }
    *slot_out = slot;
    return true;
}

ucn_result_t ucn_send_query(const ucn_node_t *node,
                            ucn_send_handle_t handle,
                            ucn_send_view_t *out_view)
{
    ucn_node_t *mutable_node = (ucn_node_t *)node;
    ucn_i_callback_claim_t claim;
    ucn_send_view_t view;
    uint16_t request_slot;
    uint16_t receipt_slot;
    ucn_result_t result;

    if (!node_is_valid(node) || out_view == NULL ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_view,
                             sizeof(*out_view))) {
        return UCN_ERR_ARGUMENT;
    }
    result = api_enter(mutable_node, 13U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    if (!request_handle_matches(node, handle, &request_slot)) {
        return api_leave(mutable_node, &claim, UCN_ERR_NOT_FOUND);
    }
    receipt_slot = node->requests[request_slot].receipt_slot;
    if (receipt_slot >= UCN_RECEIPT_COUNT ||
        !node->receipts[receipt_slot].valid) {
        return api_leave(mutable_node, &claim, UCN_ERR_STATE);
    }
    view = node->receipts[receipt_slot].view;
    result = api_leave(mutable_node, &claim, UCN_OK);
    if (result == UCN_OK) {
        *out_view = view;
    }
    return result;
}

ucn_result_t ucn_send_cancel(ucn_node_t *node, ucn_send_handle_t handle)
{
    ucn_i_callback_claim_t claim;
    uint16_t request_slot;
    uint16_t tx_slot;
    ucn_result_t result = api_enter(node, 7U, &claim);

    if (result != UCN_OK) {
        return result;
    }
    if (!request_handle_matches(node, handle, &request_slot)) {
        return api_leave(node, &claim, UCN_ERR_NOT_FOUND);
    }
    if (node->requests[request_slot].terminal) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    tx_slot = node->requests[request_slot].tx_slot;
    if (tx_slot >= UCN_TX_SLOT_COUNT ||
        node->tx_slots[tx_slot].state == UCN_I_TX_FREE) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    node->tx_slots[tx_slot].cancel_required = 1U;
    node->tx_slots[tx_slot].absolute_deadline_us = 0U;
    return api_leave(node, &claim, UCN_OK);
}

ucn_result_t ucn_send_forget(ucn_node_t *node, ucn_send_handle_t handle)
{
    ucn_i_callback_claim_t claim;
    uint16_t request_slot;
    uint16_t receipt_slot;
    uint16_t request_generation;
    uint16_t receipt_generation;
    ucn_result_t result = api_enter(node, 8U, &claim);

    if (result != UCN_OK) {
        return result;
    }
    if (!request_handle_matches(node, handle, &request_slot)) {
        return api_leave(node, &claim, UCN_ERR_NOT_FOUND);
    }
    if (!node->requests[request_slot].terminal) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    receipt_slot = node->requests[request_slot].receipt_slot;
    if (receipt_slot >= UCN_RECEIPT_COUNT ||
        !node->receipts[receipt_slot].valid) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    request_generation = node->requests[request_slot].generation;
    receipt_generation = node->receipts[receipt_slot].generation;
    memset(&node->requests[request_slot], 0,
           sizeof(node->requests[request_slot]));
    node->requests[request_slot].generation = request_generation;
    memset(&node->receipts[receipt_slot], 0,
           sizeof(node->receipts[receipt_slot]));
    node->receipts[receipt_slot].generation = receipt_generation;
    return api_leave(node, &claim, UCN_OK);
}

static ucn_link_outcome_t outcome_from_result(ucn_result_t result)
{
    if (result == UCN_OK) {
        return UCN_LINK_OUTCOME_COMPLETE;
    }
    if (result == UCN_ERR_CANCELLED || result == UCN_ERR_TIMEOUT) {
        return UCN_LINK_OUTCOME_CANCELLED;
    }
    if (result == UCN_ERR_IN_DOUBT) {
        return UCN_LINK_OUTCOME_IN_DOUBT;
    }
    return UCN_LINK_OUTCOME_FAILED;
}

static void clear_tx_slot(ucn_i_tx_slot_t *slot)
{
    memset(slot, 0, sizeof(*slot));
}

static ucn_result_t terminalize_tx(ucn_node_t *node,
                                   uint16_t tx_slot,
                                   ucn_result_t terminal_result)
{
    ucn_i_tx_slot_t *tx = &node->tx_slots[tx_slot];
    ucn_send_completion_fn completion = NULL;
    void *completion_context = NULL;
    ucn_send_handle_t handle;
    ucn_send_view_t *view = NULL;
    uint16_t request_slot = tx->request_slot;
    ucn_result_t result;

    memset(&handle, 0, sizeof(handle));

    if (tx->tracked) {
        uint16_t receipt_slot;
        uint16_t attempt_slot;
        uint16_t buffer_slot;
        if (request_slot >= UCN_REQUEST_COUNT ||
            !node->requests[request_slot].valid ||
            node->requests[request_slot].tx_slot != tx_slot) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return UCN_ERR_STATE;
        }
        receipt_slot = node->requests[request_slot].receipt_slot;
        attempt_slot = node->requests[request_slot].attempt_slot;
        buffer_slot = node->requests[request_slot].buffer_slot;
        if (receipt_slot >= UCN_RECEIPT_COUNT ||
            !node->receipts[receipt_slot].valid ||
            attempt_slot >= UCN_ATTEMPT_COUNT ||
            !node->attempts[attempt_slot].valid ||
            node->attempts[attempt_slot].tx_slot != tx_slot ||
            memcmp(&node->attempts[attempt_slot].adapter_token,
                   &tx->adapter_token, sizeof(tx->adapter_token)) != 0 ||
            buffer_slot >= UCN_BUFFER_OBLIGATION_COUNT ||
            !node->buffers[buffer_slot].valid ||
            node->buffers[buffer_slot].tx_slot != tx_slot) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return UCN_ERR_STATE;
        }
    } else if (request_slot != UCN_I_NO_SLOT) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return UCN_ERR_STATE;
    }

    if (!handle_is_zero(tx->adapter_token)) {
        result = ucn_i_adapter_tx_retire(&node->adapter, tx->adapter_token);
        if (result != UCN_OK) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return result;
        }
    }
    if (tx->path_slot < UCN_STATIC_PATH_COUNT &&
        node->paths[tx->path_slot].references != 0U) {
        --node->paths[tx->path_slot].references;
    }
    if (tx->tracked && request_slot < UCN_REQUEST_COUNT &&
        node->requests[request_slot].valid) {
        uint16_t receipt_slot = node->requests[request_slot].receipt_slot;
        uint16_t attempt_slot = node->requests[request_slot].attempt_slot;
        uint16_t buffer_slot = node->requests[request_slot].buffer_slot;
        uint16_t attempt_generation;
        uint16_t buffer_generation;
        view = &node->receipts[receipt_slot].view;
        view->terminal_result = terminal_result;
        view->link_outcome = outcome_from_result(terminal_result);
        view->buffer_released = 1U;
        view->callback_delivered =
            node->requests[request_slot].completion != NULL ? 1U : 0U;
        node->requests[request_slot].terminal = 1U;
        node->requests[request_slot].tx_slot = UCN_I_NO_SLOT;
        attempt_generation = node->attempts[attempt_slot].generation;
        memset(&node->attempts[attempt_slot], 0,
               sizeof(node->attempts[attempt_slot]));
        node->attempts[attempt_slot].generation = attempt_generation;
        buffer_generation = node->buffers[buffer_slot].generation;
        memset(&node->buffers[buffer_slot], 0,
               sizeof(node->buffers[buffer_slot]));
        node->buffers[buffer_slot].generation = buffer_generation;
        completion = node->requests[request_slot].completion;
        completion_context = node->requests[request_slot].completion_context;
        handle = make_core_handle(node, request_slot,
                                  node->requests[request_slot].generation,
                                  UCN_OBJECT_KIND_SEND);
    }
    clear_tx_slot(tx);
    if (terminal_result == UCN_OK) {
        increment_saturated(&node->stats.tx_completed);
    } else {
        increment_saturated(&node->stats.tx_failed);
    }
    if (completion != NULL) {
        completion(completion_context, handle, view);
    }
    return UCN_OK;
}

static ucn_result_t requeue_not_submitted_tx(ucn_node_t *node,
                                             uint16_t tx_slot)
{
    ucn_i_tx_slot_t *tx;
    ucn_driver_token_t token;
    uint16_t attempt_slot = UCN_I_NO_SLOT;
    uint16_t receipt_slot = UCN_I_NO_SLOT;
    ucn_result_t result;

    if (node == NULL || tx_slot >= UCN_TX_SLOT_COUNT) {
        return UCN_ERR_ARGUMENT;
    }
    tx = &node->tx_slots[tx_slot];
    if (tx->state == UCN_I_TX_FREE || handle_is_zero(tx->adapter_token)) {
        return UCN_ERR_STATE;
    }
    token = tx->adapter_token;
    /* Validate every tracked edge before retiring the Adapter token.  This
     * keeps corruption handling fail-closed: a broken ownership graph cannot
     * leave Core and Adapter half-updated. */
    if (tx->tracked) {
        uint16_t request_slot = tx->request_slot;

        if (request_slot >= UCN_REQUEST_COUNT ||
            !node->requests[request_slot].valid ||
            node->requests[request_slot].tx_slot != tx_slot) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return UCN_ERR_STATE;
        }
        attempt_slot = node->requests[request_slot].attempt_slot;
        receipt_slot = node->requests[request_slot].receipt_slot;
        if (attempt_slot >= UCN_ATTEMPT_COUNT ||
            receipt_slot >= UCN_RECEIPT_COUNT ||
            !node->attempts[attempt_slot].valid ||
            node->attempts[attempt_slot].tx_slot != tx_slot ||
            !node->receipts[receipt_slot].valid ||
            memcmp(&node->attempts[attempt_slot].adapter_token, &token,
                   sizeof(token)) != 0) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return UCN_ERR_STATE;
        }
    } else if (tx->request_slot != UCN_I_NO_SLOT) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return UCN_ERR_STATE;
    }
    result = ucn_i_adapter_tx_retire(&node->adapter, token);
    if (result != UCN_OK) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return result;
    }
    memset(&tx->adapter_token, 0, sizeof(tx->adapter_token));
    tx->state = UCN_I_TX_QUEUED;
    if (tx->tracked) {
        memset(&node->attempts[attempt_slot].adapter_token, 0,
               sizeof(node->attempts[attempt_slot].adapter_token));
        node->receipts[receipt_slot].view.link_outcome =
            UCN_LINK_OUTCOME_PENDING;
    }
    return UCN_OK;
}

static UCN_I_NOINLINE bool process_one_completion(ucn_node_t *node)
{
    uint16_t index;

    for (index = 0U; index < UCN_TX_SLOT_COUNT; ++index) {
        uint16_t slot = (uint16_t)(
            (node->completion_cursor + index) % UCN_TX_SLOT_COUNT);
        ucn_i_tx_slot_t *tx = &node->tx_slots[slot];
        ucn_i_adapter_tx_view_t adapter_view;
        if (tx->state == UCN_I_TX_FREE ||
            handle_is_zero(tx->adapter_token)) {
            continue;
        }
        if (ucn_i_adapter_tx_view(&node->adapter, tx->adapter_token,
                                  &adapter_view) != UCN_OK) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            node->completion_cursor =
                (uint16_t)((slot + 1U) % UCN_TX_SLOT_COUNT);
            return true;
        }
        if (adapter_view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
            !adapter_view.terminal_latched) {
            node->completion_cursor =
                (uint16_t)((slot + 1U) % UCN_TX_SLOT_COUNT);
            (void)requeue_not_submitted_tx(node, slot);
            return true;
        }
        if (adapter_view.state == UCN_I_ADAPTER_TX_COMPLETED ||
            adapter_view.state == UCN_I_ADAPTER_TX_CANCELLED ||
            (adapter_view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
             adapter_view.terminal_latched)) {
            ucn_result_t terminal_result = adapter_view.terminal_result;
            node->completion_cursor =
                (uint16_t)((slot + 1U) % UCN_TX_SLOT_COUNT);
            if (terminal_result != UCN_ERR_IN_DOUBT &&
                tx->timeout_requested) {
                terminal_result = UCN_ERR_TIMEOUT;
            }
            (void)terminalize_tx(
                node, slot, terminal_result);
            return true;
        }
    }
    return false;
}

static UCN_I_NOINLINE bool process_one_timer(ucn_node_t *node,
                                             uint64_t now_us)
{
    uint16_t index;

    for (index = 0U; index < UCN_TX_SLOT_COUNT; ++index) {
        uint16_t slot =
            (uint16_t)((node->timer_cursor + index) % UCN_TX_SLOT_COUNT);
        ucn_i_tx_slot_t *tx = &node->tx_slots[slot];
        ucn_i_adapter_tx_view_t view;
        bool stopping = node->lifecycle == UCN_LIFECYCLE_STOPPING;
        bool deadline_expired =
            tx->absolute_deadline_us != 0U &&
            ucn_i_deadline_expired_us(now_us, tx->absolute_deadline_us);
        ucn_result_t result;
        bool cancel_required = tx->cancel_required != 0U;
        if (tx->state == UCN_I_TX_FREE || tx->cancel_requested ||
            (!stopping && !deadline_expired && !cancel_required)) {
            continue;
        }
        if (handle_is_zero(tx->adapter_token)) {
            tx->cancel_required = 0U;
            tx->absolute_deadline_us = 0U;
            node->timer_cursor =
                (uint16_t)((slot + 1U) % UCN_TX_SLOT_COUNT);
            (void)terminalize_tx(
                node, slot,
                deadline_expired ? UCN_ERR_TIMEOUT : UCN_ERR_CANCELLED);
            return true;
        }
        result = ucn_i_adapter_tx_cancel(&node->adapter, tx->adapter_token);
        if (result == UCN_OK || result == UCN_ERR_IN_DOUBT) {
            tx->cancel_requested = 1U;
            tx->cancel_required = 0U;
            tx->absolute_deadline_us = 0U;
            node->timer_cursor =
                (uint16_t)((slot + 1U) % UCN_TX_SLOT_COUNT);
            if (!stopping && deadline_expired) {
                tx->timeout_requested = 1U;
            }
            if (result == UCN_OK && tx->state == UCN_I_TX_QUEUED) {
                (void)terminalize_tx(
                    node, slot,
                    deadline_expired ? UCN_ERR_TIMEOUT
                                     : UCN_ERR_CANCELLED);
            }
            return true;
        }
        if (result == UCN_ERR_STATE &&
            ucn_i_adapter_tx_view(&node->adapter, tx->adapter_token,
                                  &view) == UCN_OK &&
            (view.state == UCN_I_ADAPTER_TX_RESERVED ||
             view.state == UCN_I_ADAPTER_TX_SUBMITTED ||
             view.state == UCN_I_ADAPTER_TX_COMPLETED ||
             view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED ||
             view.state == UCN_I_ADAPTER_TX_CANCELLED)) {
            continue;
        }
        if (result != UCN_ERR_STATE) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return true;
        }
    }
    return false;
}

static bool find_endpoint(const ucn_node_t *node,
                          uint16_t service_id,
                          uint16_t *slot_out)
{
    uint16_t index;

    for (index = 0U; index < UCN_ENDPOINT_COUNT; ++index) {
        if (node->endpoints[index].valid &&
            node->endpoints[index].service_id == service_id) {
            *slot_out = index;
            return true;
        }
    }
    return false;
}

static bool binding_replay_accept(ucn_i_binding_t *binding,
                                  uint32_t sequence)
{
    uint32_t distance;
    uint64_t bit;

    if (sequence == 0U) {
        return false;
    }
    if (binding->replay_highest_sequence == 0U) {
        binding->replay_highest_sequence = sequence;
        binding->replay_bitmap = UINT64_C(1);
        return true;
    }
    if (sequence > binding->replay_highest_sequence) {
        distance = sequence - binding->replay_highest_sequence;
        binding->replay_bitmap =
            distance >= 64U ? UINT64_C(1)
                            : (binding->replay_bitmap << distance) |
                                  UINT64_C(1);
        binding->replay_highest_sequence = sequence;
        return true;
    }
    distance = binding->replay_highest_sequence - sequence;
    if (distance >= 64U) {
        return false;
    }
    bit = UINT64_C(1) << distance;
    if ((binding->replay_bitmap & bit) != 0U) {
        return false;
    }
    binding->replay_bitmap |= bit;
    return true;
}

static UCN_I_NOINLINE ucn_endpoint_disposition_t invoke_endpoint_callback(
    ucn_node_t *node,
    uint16_t endpoint_slot,
    uint16_t binding_slot,
    const ucn_i_adapter_rx_view_t *rx,
    const ucn_i_c1_frame_t *frame)
{
    ucn_endpoint_message_t message;
    ucn_endpoint_disposition_t disposition;
    ucn_callback_scope_t scope;

    memset(&message, 0, sizeof(message));
    message.struct_size = sizeof(message);
    message.api_version = UCN_API_VERSION;
    message.source_address = frame->source_address;
    message.source_binding_generation =
        node->bindings[binding_slot].binding_generation;
    message.service_id = frame->service_id;
    message.traffic_class = frame->traffic_class;
    message.hop_limit = frame->hop_limit;
    message.payload = frame->payload;
    message.payload_bytes = frame->payload_bytes;
    message.receive_timestamp_us = rx->meta.timestamp_us;
    memset(&scope, 0, sizeof(scope));
    if (callback_read_snapshot_begin(node, &scope) != UCN_OK) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return UCN_ENDPOINT_DROP;
    }
    message.callback_scope = scope;
    node->endpoints[endpoint_slot].callback_active = 1U;
    disposition = node->endpoints[endpoint_slot].receive(
        node->endpoints[endpoint_slot].context, &message);
    node->endpoints[endpoint_slot].callback_active = 0U;
    if (callback_read_snapshot_end(node) != UCN_OK) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
    }
    return disposition;
}

static UCN_I_NOINLINE bool process_one_rx(ucn_node_t *node)
{
    ucn_i_adapter_rx_view_t rx;
    ucn_i_c1_frame_t frame;
    uint32_t current_link_instance;
    uint16_t binding_slot;
    uint16_t endpoint_slot;
    uint16_t current_link_mtu;
    bool link_ready;
    ucn_result_t result;

    result = ucn_i_adapter_rx_claim(&node->adapter, &rx);
    if (result != UCN_OK) {
        return false;
    }
    increment_saturated(&node->stats.rx_published);
    if (node->lifecycle == UCN_LIFECYCLE_STOPPING) {
        increment_saturated(&node->stats.rx_dropped);
        if (ucn_i_adapter_rx_retire(&node->adapter, rx.token) != UCN_OK) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
        }
        return true;
    }
    result = ucn_i_adapter_link_snapshot(
        &node->adapter, rx.link_slot, &current_link_instance,
        &current_link_mtu, &link_ready);
    if (result != UCN_OK || !link_ready ||
        current_link_instance != rx.link_instance_generation) {
        increment_saturated(&node->stats.rx_dropped);
    } else if (ucn_i_c1_decode(rx.frame, rx.frame_bytes, node->address_width,
                               &frame) != UCN_OK) {
        increment_saturated(&node->stats.malformed);
        increment_saturated(&node->stats.rx_dropped);
    } else if (frame.destination_address != node->local_address ||
               !source_binding_find(node, frame.source_address,
                                    &binding_slot) ||
               !find_endpoint(node, frame.service_id, &endpoint_slot)) {
        increment_saturated(&node->stats.rx_dropped);
    } else if (!binding_replay_accept(&node->bindings[binding_slot],
                                      frame.origin_sequence)) {
        increment_saturated(&node->stats.rx_dropped);
    } else {
        ucn_endpoint_disposition_t disposition = invoke_endpoint_callback(
            node, endpoint_slot, binding_slot, &rx, &frame);
        if (disposition == UCN_ENDPOINT_ACCEPT) {
            increment_saturated(&node->stats.rx_delivered);
        } else {
            increment_saturated(&node->stats.rx_dropped);
        }
    }
    if (ucn_i_adapter_rx_retire(&node->adapter, rx.token) != UCN_OK) {
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
    }
    (void)current_link_mtu;
    return true;
}

static bool next_queued_tx(ucn_node_t *node, uint16_t *slot_out)
{
    static const uint8_t schedule[12] = {0U, 0U, 1U, 0U, 2U, 0U,
                                         1U, 0U, 3U, 1U, 2U, 0U};
    uint8_t attempt;

    for (attempt = 0U; attempt < sizeof(schedule); ++attempt) {
        uint8_t schedule_slot =
            (uint8_t)((node->scheduler_cursor + attempt) % sizeof(schedule));
        uint8_t traffic_class = schedule[schedule_slot];
        uint16_t begin;
        uint16_t count;
        uint16_t scan;
        queue_bounds(traffic_class, &begin, &count);
        for (scan = 0U; scan < count; ++scan) {
            uint16_t offset =
                (uint16_t)((node->queue_cursor[traffic_class] + scan) % count);
            uint16_t slot = (uint16_t)(begin + offset);
            if (node->tx_slots[slot].state == UCN_I_TX_QUEUED) {
                node->scheduler_cursor =
                    (uint8_t)((schedule_slot + 1U) % sizeof(schedule));
                node->queue_cursor[traffic_class] =
                    (uint8_t)((offset + 1U) % count);
                *slot_out = slot;
                return true;
            }
        }
    }
    return false;
}

static UCN_I_NOINLINE bool process_one_tx(ucn_node_t *node,
                                          uint64_t now_us)
{
    ucn_i_tx_slot_t *tx;
    ucn_i_c1_frame_t frame;
    ucn_i_adapter_tx_view_t view;
    uint16_t slot;
    size_t frame_bytes;
    ucn_result_t result;

    if (!next_queued_tx(node, &slot)) {
        return false;
    }
    tx = &node->tx_slots[slot];
    if (tx->cancel_required) {
        return false;
    }
    if (tx->absolute_deadline_us != 0U &&
        ucn_i_deadline_expired_us(now_us, tx->absolute_deadline_us)) {
        (void)terminalize_tx(node, slot, UCN_ERR_TIMEOUT);
        return true;
    }
    if (handle_is_zero(tx->adapter_token)) {
        ucn_driver_token_t adapter_token;

        result = ucn_i_adapter_tx_reserve(
            &node->adapter, node->paths[tx->path_slot].link_index,
            node->paths[tx->path_slot].link_instance_generation, slot,
            &adapter_token);
        if (result == UCN_ERR_NO_SPACE || result == UCN_ERR_STATE) {
            return false;
        }
        if (result != UCN_OK) {
            if (result == UCN_ERR_EXHAUSTED) {
                node->faulted = 1U;
                node->lifecycle = UCN_LIFECYCLE_FAULT;
            }
            (void)terminalize_tx(node, slot, result);
            return true;
        }
        tx->adapter_token = adapter_token;
        if (tx->tracked) {
            uint16_t attempt_slot =
                node->requests[tx->request_slot].attempt_slot;
            node->attempts[attempt_slot].adapter_token = adapter_token;
        }
    }
    if (!tx->encoded) {
        memset(&frame, 0, sizeof(frame));
        frame.payload = tx->frame;
        frame.payload_bytes = tx->payload_bytes;
        frame.source_address = node->local_address;
        frame.destination_address = tx->destination_address;
        frame.origin_sequence = tx->origin_sequence;
        frame.service_id = tx->service_id;
        frame.traffic_class = tx->traffic_class;
        frame.hop_limit = tx->hop_limit;
        result = ucn_i_c1_encode_in_place(&frame, node->address_width,
                                          tx->frame, sizeof(tx->frame),
                                          &frame_bytes);
        if (result != UCN_OK) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return true;
        }
        tx->frame_bytes = (uint16_t)frame_bytes;
        tx->encoded = 1U;
    }
    result = ucn_i_adapter_tx_submit(&node->adapter, tx->adapter_token,
                                     tx->frame, tx->frame_bytes, &view);
    if (result != UCN_OK) {
        if (ucn_i_adapter_tx_view(&node->adapter, tx->adapter_token,
                                  &view) != UCN_OK) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return true;
        }
        if (view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
            !view.terminal_latched) {
            (void)requeue_not_submitted_tx(node, slot);
            return true;
        }
        if (view.state == UCN_I_ADAPTER_TX_COMPLETED ||
            view.state == UCN_I_ADAPTER_TX_CANCELLED ||
            (view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
             view.terminal_latched)) {
            (void)terminalize_tx(node, slot, view.terminal_result);
            return true;
        }
        if (view.state == UCN_I_ADAPTER_TX_IN_DOUBT ||
            view.state == UCN_I_ADAPTER_TX_SUBMITTED) {
            tx->state = UCN_I_TX_SUBMITTED;
            if (tx->tracked) {
                uint16_t receipt_slot =
                    node->requests[tx->request_slot].receipt_slot;
                node->receipts[receipt_slot].view.link_outcome =
                    view.state == UCN_I_ADAPTER_TX_SUBMITTED
                        ? UCN_LINK_OUTCOME_SUBMITTED
                        : UCN_LINK_OUTCOME_IN_DOUBT;
            }
            return true;
        }
        if (result == UCN_ERR_STATE &&
            view.state == UCN_I_ADAPTER_TX_RESERVED) {
            return false;
        }
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return true;
    }
    if (view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
        !view.terminal_latched) {
        (void)requeue_not_submitted_tx(node, slot);
        return true;
    }
    tx->state = UCN_I_TX_SUBMITTED;
    if (view.state == UCN_I_ADAPTER_TX_COMPLETED ||
        view.state == UCN_I_ADAPTER_TX_CANCELLED ||
        (view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
         view.terminal_latched)) {
        (void)terminalize_tx(node, slot, view.terminal_result);
    } else if (tx->tracked) {
        uint16_t receipt_slot =
            node->requests[tx->request_slot].receipt_slot;
        node->receipts[receipt_slot].view.link_outcome =
            view.state == UCN_I_ADAPTER_TX_SUBMITTED
                ? UCN_LINK_OUTCOME_SUBMITTED
                : UCN_LINK_OUTCOME_IN_DOUBT;
    }
    return true;
}

static uint64_t earliest_deadline(const ucn_node_t *node)
{
    uint64_t earliest = 0U;
    uint16_t index;

    for (index = 0U; index < UCN_TX_SLOT_COUNT; ++index) {
        uint64_t deadline = node->tx_slots[index].absolute_deadline_us;
        if (node->tx_slots[index].state != UCN_I_TX_FREE && deadline != 0U &&
            (earliest == 0U || deadline < earliest)) {
            earliest = deadline;
        }
    }
    return earliest;
}

static bool local_protocol_obligations_exist(const ucn_node_t *node)
{
    uint16_t index;

    for (index = 0U; index < UCN_TX_SLOT_COUNT; ++index) {
        if (node->tx_slots[index].state != UCN_I_TX_FREE) {
            return true;
        }
    }
    for (index = 0U; index < UCN_REQUEST_COUNT; ++index) {
        if (node->requests[index].valid) {
            return true;
        }
    }
    for (index = 0U; index < UCN_RECEIPT_COUNT; ++index) {
        if (node->receipts[index].valid) {
            return true;
        }
    }
    for (index = 0U; index < UCN_ATTEMPT_COUNT; ++index) {
        if (node->attempts[index].valid) {
            return true;
        }
    }
    for (index = 0U; index < UCN_BUFFER_OBLIGATION_COUNT; ++index) {
        if (node->buffers[index].valid) {
            return true;
        }
    }
    return false;
}

static bool local_runnable_work(ucn_node_t *node, uint64_t now_us)
{
    uint16_t index;

    if (ucn_i_adapter_has_runnable_work(&node->adapter)) {
        return true;
    }
    for (index = 0U; index < UCN_TX_SLOT_COUNT; ++index) {
        const ucn_i_tx_slot_t *tx = &node->tx_slots[index];
        if (tx->state == UCN_I_TX_FREE) {
            continue;
        }
        if (node->lifecycle == UCN_LIFECYCLE_RUNNING &&
            tx->state == UCN_I_TX_QUEUED) {
            return true;
        }
        if (!tx->cancel_requested &&
            (tx->cancel_required ||
             node->lifecycle == UCN_LIFECYCLE_STOPPING ||
             (tx->absolute_deadline_us != 0U &&
              ucn_i_deadline_expired_us(now_us,
                                        tx->absolute_deadline_us)))) {
            return true;
        }
    }
    return false;
}

static void drain_adapter_hints(ucn_node_t *node)
{
    ucn_i_owner_work_hint_t hint;
    uint8_t count;

    for (count = 0U; count < UCN_I_OWNER_WORK_CLASS_LIMIT; ++count) {
        if (ucn_i_owner_mailbox_take(&node->adapter.mailbox, &hint) != UCN_OK) {
            break;
        }
    }
}

ucn_result_t ucn_step(ucn_node_t *node,
                      uint64_t now_us,
                      const ucn_step_budget_t *budget,
                      ucn_step_result_t *out_result)
{
    ucn_i_callback_claim_t claim;
    ucn_step_result_t result_view;
    uint16_t work;
    uint8_t phase;
    ucn_result_t result;

    if (!node_is_valid(node) || budget == NULL || out_result == NULL ||
        budget->struct_size != sizeof(*budget) ||
        budget->api_version != UCN_API_VERSION || budget->max_work == 0U ||
        budget->reserved_zero != 0U ||
        ucn_i_ranges_overlap(node, sizeof(*node), budget, sizeof(*budget)) ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_result,
                             sizeof(*out_result)) ||
        ucn_i_ranges_overlap(budget, sizeof(*budget), out_result,
                             sizeof(*out_result))) {
        return UCN_ERR_ARGUMENT;
    }
    result = api_enter(node, 9U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    if (node->lifecycle != UCN_LIFECYCLE_RUNNING &&
        node->lifecycle != UCN_LIFECYCLE_STOPPING) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    if (!node->time_initialized || now_us < node->last_now_us) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    node->last_now_us = now_us;
    memset(&result_view, 0, sizeof(result_view));
    result_view.struct_size = sizeof(result_view);
    result_view.api_version = UCN_API_VERSION;
    drain_adapter_hints(node);
    phase = node->work_cursor;
    for (work = 0U; work < budget->max_work; ++work) {
        bool progressed = false;
        uint8_t attempt;
        for (attempt = 0U; attempt < 4U; ++attempt) {
            uint8_t selected = (uint8_t)((phase + attempt) % 4U);
            if (selected == 0U) {
                progressed = process_one_completion(node);
            } else if (selected == 1U) {
                progressed = process_one_timer(node, now_us);
            } else if (selected == 2U) {
                progressed = process_one_rx(node);
            } else if (node->lifecycle == UCN_LIFECYCLE_RUNNING) {
                progressed = process_one_tx(node, now_us);
            }
            if (progressed) {
                phase = (uint8_t)((selected + 1U) % 4U);
                break;
            }
        }
        if (!progressed) {
            break;
        }
        ++result_view.work_done;
        if (node->faulted) {
            break;
        }
    }
    if (node->lifecycle == UCN_LIFECYCLE_STOPPING &&
        !local_protocol_obligations_exist(node) &&
        !ucn_i_adapter_has_work(&node->adapter)) {
        node->lifecycle = UCN_LIFECYCLE_QUIESCENT;
    }
    result_view.next_deadline_us = earliest_deadline(node);
    result_view.more_work = local_runnable_work(node, now_us) ? 1U : 0U;
    result_view.lifecycle = node->lifecycle;
    node->work_cursor = phase;
    result = api_leave(node, &claim,
                       node->lifecycle == UCN_LIFECYCLE_FAULT
                           ? UCN_ERR_STATE
                           : UCN_OK);
    if (result == UCN_OK) {
        *out_result = result_view;
    }
    return result;
}

ucn_result_t ucn_stop(ucn_node_t *node)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result = api_enter(node, 10U, &claim);

    if (result != UCN_OK) {
        return result;
    }
    if (node->lifecycle != UCN_LIFECYCLE_INITIALIZED &&
        node->lifecycle != UCN_LIFECYCLE_RUNNING &&
        node->lifecycle != UCN_LIFECYCLE_FAULT) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    if (ucn_i_adapter_set_rx_enabled(&node->adapter, false) != UCN_OK) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    node->lifecycle = UCN_LIFECYCLE_STOPPING;
    drain_adapter_hints(node);
    if (!local_protocol_obligations_exist(node) &&
        !ucn_i_adapter_has_work(&node->adapter)) {
        node->lifecycle = UCN_LIFECYCLE_QUIESCENT;
    }
    return api_leave(node, &claim, UCN_OK);
}

ucn_result_t ucn_deinit(ucn_node_t *node)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    result = api_enter(node, 11U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    if (node->lifecycle != UCN_LIFECYCLE_QUIESCENT) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    if (local_protocol_obligations_exist(node) ||
        ucn_i_adapter_has_work(&node->adapter)) {
        return api_leave(node, &claim, UCN_ERR_STATE);
    }
    drain_adapter_hints(node);
    /* Publish an irreversible local fence before releasing the Core gate.
     * No Public API or Driver fact may enter while embedded owners are being
     * destroyed and the caller-owned Storage is being returned. */
    node->magic = 0U;
    result = api_leave(node, &claim, UCN_OK);
    if (result != UCN_OK ||
        ucn_i_callback_gate_destroy(&node->callback_gate) != UCN_OK ||
        ucn_i_adapter_destroy(&node->adapter) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    memset(node, 0, UCN_STORAGE_BYTES);
    return UCN_OK;
}

ucn_result_t ucn_get_stats(const ucn_node_t *node, ucn_stats_t *out_stats)
{
    ucn_node_t *mutable_node = (ucn_node_t *)node;
    ucn_i_callback_claim_t claim;
    ucn_stats_t stats;
    ucn_result_t result;

    if (!node_is_valid(node) || out_stats == NULL ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_stats,
                             sizeof(*out_stats))) {
        return UCN_ERR_ARGUMENT;
    }
    result = api_enter(mutable_node, 14U, &claim);
    if (result != UCN_OK) {
        return result;
    }
    stats = node->stats;
    result = api_leave(mutable_node, &claim, UCN_OK);
    if (result == UCN_OK) {
        *out_stats = stats;
    }
    return result;
}

ucn_result_t ucn_callback_link_get(const ucn_node_t *node,
                                   ucn_callback_scope_t scope,
                                   uint16_t link_index,
                                   ucn_link_handle_t *out_link)
{
    if (!node_is_valid(node) || out_link == NULL ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_link,
                             sizeof(*out_link))) {
        return UCN_ERR_ARGUMENT;
    }
    return callback_read_link(node, scope, link_index, out_link);
}

ucn_result_t ucn_callback_send_query(const ucn_node_t *node,
                                     ucn_callback_scope_t scope,
                                     ucn_send_handle_t handle,
                                     ucn_send_view_t *out_view)
{
    if (!node_is_valid(node) || out_view == NULL ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_view,
                             sizeof(*out_view))) {
        return UCN_ERR_ARGUMENT;
    }
    return callback_read_send(node, scope, handle, out_view);
}

ucn_result_t ucn_callback_get_stats(const ucn_node_t *node,
                                    ucn_callback_scope_t scope,
                                    ucn_stats_t *out_stats)
{
    if (!node_is_valid(node) || out_stats == NULL ||
        ucn_i_ranges_overlap(node, sizeof(*node), out_stats,
                             sizeof(*out_stats))) {
        return UCN_ERR_ARGUMENT;
    }
    return callback_read_stats(node, scope, out_stats);
}

ucn_result_t ucn_driver_rx_publish(ucn_node_t *node,
                                   ucn_link_handle_t link,
                                   const uint8_t *bytes,
                                   size_t length,
                                   const ucn_rx_meta_t *meta)
{
    if (!node_is_valid(node)) {
        return UCN_ERR_STATE;
    }
    if (bytes == NULL || meta == NULL ||
        ucn_i_ranges_overlap(node, sizeof(*node), bytes, length) ||
        ucn_i_ranges_overlap(node, sizeof(*node), meta, sizeof(*meta))) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_i_adapter_rx_publish(&node->adapter, link, bytes, length, meta);
}

ucn_result_t ucn_driver_tx_complete(ucn_node_t *node,
                                    ucn_driver_token_t token,
                                    ucn_result_t result,
                                    const ucn_tx_meta_t *meta)
{
    if (!node_is_valid(node)) {
        return UCN_ERR_STATE;
    }
    if (meta != NULL &&
        ucn_i_ranges_overlap(node, sizeof(*node), meta, sizeof(*meta))) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_i_adapter_tx_complete(&node->adapter, token, result, meta);
}

ucn_result_t ucn_driver_link_event(ucn_node_t *node,
                                   ucn_link_handle_t link,
                                   ucn_driver_link_event_t event,
                                   const ucn_link_event_meta_t *meta)
{
    if (!node_is_valid(node)) {
        return UCN_ERR_STATE;
    }
    if (meta != NULL &&
        ucn_i_ranges_overlap(node, sizeof(*node), meta, sizeof(*meta))) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_i_adapter_link_event(&node->adapter, link, event, meta);
}
