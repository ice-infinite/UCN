#include "internal/ucn_persistence_coordinator.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool bytes_are_zero(const void *bytes, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)bytes;
    size_t index;

    if (bytes == NULL) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (cursor[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool adapter_header_is_valid(
    const ucn_i_persistence_coordinator_adapter_t *adapter)
{
    return adapter != NULL &&
           adapter->magic == UCN_I_PERSIST_COORDINATOR_ADAPTER_MAGIC &&
           adapter->schema == UCN_I_PERSIST_COORDINATOR_ADAPTER_SCHEMA &&
           adapter->route_active <= 1U && adapter->bound <= 1U &&
           adapter->owner != NULL &&
           ((adapter->bound == 0U && adapter->coordinator == NULL) ||
            (adapter->bound != 0U && adapter->coordinator != NULL));
}

static void exact_write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void exact_write_u64(uint8_t *output, uint64_t value)
{
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - 8U * index));
    }
}

static ucn_result_t requirement_build(
    const ucn_i_coordinator_t *coordinator,
    const ucn_persistence_request_t *request,
    uint8_t exact[UCN_I_DEPENDENCY_EXACT_BYTES],
    ucn_i_dependency_requirement_t *requirement_out)
{
    if (coordinator == NULL || coordinator->digest == NULL || request == NULL ||
        exact == NULL || requirement_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    memset(exact, 0, UCN_I_DEPENDENCY_EXACT_BYTES);
    exact_write_u16(&exact[0], request->domain.domain_kind);
    exact_write_u64(&exact[4], request->domain.domain_id);
    exact_write_u64(&exact[12], request->foundation_transaction_id);
    exact_write_u64(&exact[20], request->expected_record_generation);
    exact_write_u16(&exact[28], request->operation_kind);
    exact_write_u16(&exact[30], request->schema_id);
    return ucn_i_dependency_requirement_build(
        coordinator->digest, UCN_I_DEP_PERSISTENCE,
        request->runtime_instance, request->caller_owner_instance,
        request->absolute_deadline_us, request->business_transition_digest,
        exact, UCN_I_DEPENDENCY_EXACT_BYTES, requirement_out);
}

static ucn_result_t coordinator_ensure(
    void *context,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    ucn_i_persistence_coordinator_adapter_t *adapter =
        (ucn_i_persistence_coordinator_adapter_t *)context;
    ucn_persistence_owner_t *owner;
    const ucn_persistence_request_t *request;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || handle_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    owner = adapter->owner;
    result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (adapter->bound == 0U || adapter->route_active == 0U ||
        adapter->route_request == NULL || now_us != adapter->route_now_us ||
        requirement == NULL ||
        !ucn_i_dependency_requirement_equal(
            requirement, &adapter->route_requirement)) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    request = adapter->route_request;
    ucn_i_persistence_owner_unlock(owner);

    result = ucn_i_persistence_submit(owner, request, now_us, handle_out);
    if (result == UCN_OK) {
        ucn_i_persist_route_binding_t *binding;
        uint8_t index;

        result = ucn_i_persistence_owner_lock(owner);
        if (result != UCN_OK) {
            return result;
        }
        if (!ucn_i_persistence_handle_matches(owner, *handle_out, &index) ||
            !ucn_i_persistence_pending_request_equal(
                &owner->domains[index].pending, request)) {
            ucn_i_persistence_owner_unlock(owner);
            return UCN_ERR_STATE;
        }
        binding = &adapter->bindings[index];
        if (binding->valid != 0U &&
            (memcmp(&binding->handle, handle_out, sizeof(*handle_out)) != 0 ||
             binding->requirement_digest !=
                 requirement->requirement_digest)) {
            ucn_i_persistence_owner_unlock(owner);
            return UCN_ERR_STATE;
        }
        binding->handle = *handle_out;
        binding->requirement_digest = requirement->requirement_digest;
        binding->valid = 1U;
        ucn_i_persistence_owner_unlock(owner);
    }
    return result;
}

static ucn_result_t coordinator_retire(void *context,
                                       const ucn_handle_t *handle)
{
    ucn_i_persistence_coordinator_adapter_t *adapter =
        (ucn_i_persistence_coordinator_adapter_t *)context;
    ucn_persistence_owner_t *owner;
    uint8_t index;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || handle == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    owner = adapter->owner;
    result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!ucn_i_persistence_handle_matches(owner, *handle, &index) ||
        adapter->bindings[index].valid == 0U ||
        memcmp(&adapter->bindings[index].handle, handle, sizeof(*handle)) != 0) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    ucn_i_persistence_owner_unlock(owner);

    result = ucn_i_persistence_request_retire(owner, *handle);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (adapter->bindings[index].valid == 0U ||
        memcmp(&adapter->bindings[index].handle, handle, sizeof(*handle)) != 0) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&adapter->bindings[index], 0, sizeof(adapter->bindings[index]));
    ucn_i_persistence_owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_coordinator_adapter_init(
    ucn_i_persistence_coordinator_adapter_t *adapter,
    ucn_persistence_owner_t *owner)
{
    ucn_result_t result;

    if (adapter == NULL || owner == NULL ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), owner,
                             UCN_PERSIST_STORAGE_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!bytes_are_zero(adapter, sizeof(*adapter))) {
        return UCN_ERR_STATE;
    }
    result = ucn_i_persistence_attach_consumer(owner);
    if (result != UCN_OK) {
        return result;
    }
    adapter->magic = UCN_I_PERSIST_COORDINATOR_ADAPTER_MAGIC;
    adapter->schema = UCN_I_PERSIST_COORDINATOR_ADAPTER_SCHEMA;
    adapter->owner = owner;
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_coordinator_adapter_deinit(
    ucn_i_persistence_coordinator_adapter_t *adapter,
    const ucn_i_coordinator_t *destroyed_coordinator)
{
    ucn_persistence_owner_t *owner;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    owner = adapter->owner;
    result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (adapter->route_active != 0U ||
        !bytes_are_zero(adapter->bindings, sizeof(adapter->bindings)) ||
        (adapter->bound == 0U && destroyed_coordinator != NULL) ||
        (adapter->bound != 0U &&
         (destroyed_coordinator != adapter->coordinator ||
          !bytes_are_zero(destroyed_coordinator,
                          sizeof(*destroyed_coordinator))))) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    ucn_i_persistence_owner_unlock(owner);
    result = ucn_i_persistence_detach_consumer(owner);
    if (result != UCN_OK) {
        return result;
    }
    memset(adapter, 0, sizeof(*adapter));
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_bind_coordinator(
    ucn_i_persistence_coordinator_adapter_t *adapter,
    ucn_i_coordinator_t *coordinator)
{
    ucn_i_owner_binding_t binding;
    ucn_persistence_owner_t *owner;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || coordinator == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    owner = adapter->owner;
    result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (adapter->bound != 0U || adapter->route_active != 0U ||
        owner->io.call_active != 0U || owner->domain_count == UINT8_MAX ||
        coordinator->runtime_instance != owner->runtime_instance) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&binding, 0, sizeof(binding));
    binding.context = adapter;
    binding.ensure = coordinator_ensure;
    binding.retire = coordinator_retire;
    binding.owner_instance = owner->owner_instance;
    binding.slot_limit = (uint16_t)owner->domain_count + 1U;
    binding.owner_id = UCN_I_OWNER_PERSISTENCE;
    binding.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    result = ucn_i_coordinator_bind_owner(coordinator, &binding);
    if (result == UCN_OK) {
        adapter->coordinator = coordinator;
        adapter->bound = 1U;
    }
    ucn_i_persistence_owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_persistence_route_request(
    ucn_i_coordinator_t *coordinator,
    ucn_i_persistence_coordinator_adapter_t *adapter,
    const ucn_persistence_request_t *request,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    ucn_persistence_owner_t *owner;
    ucn_result_t result;
    uint8_t index;

    if (!adapter_header_is_valid(adapter) || coordinator == NULL ||
        coordinator != adapter->coordinator ||
        request == NULL || handle_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    owner = adapter->owner;
    if (ucn_i_ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, coordinator,
                             sizeof(*coordinator)) ||
        ucn_i_ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, adapter,
                             sizeof(*adapter)) ||
        ucn_i_ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(request->canonical_body, request->body_bytes,
                             handle_out, sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    index = ucn_i_persistence_find_domain_index(owner, request->domain);
    if (adapter->bound == 0U || adapter->route_active != 0U ||
        owner->io.call_active != 0U ||
        owner->owner_phase != UCN_I_PERSIST_OWNER_READY ||
        index == UCN_I_PERSIST_INVALID_INDEX ||
        !ucn_i_persistence_request_is_well_formed(owner, index, request)) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (owner->domains[index].pending.valid != 0U &&
        !ucn_i_persistence_pending_request_equal(
            &owner->domains[index].pending, request)) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    adapter->route_active = 1U;
    adapter->route_request = request;
    adapter->route_now_us = now_us;
    memset(adapter->route_exact, 0, sizeof(adapter->route_exact));
    memset(&adapter->route_requirement, 0,
           sizeof(adapter->route_requirement));
    memset(&adapter->route_handle, 0, sizeof(adapter->route_handle));
    ucn_i_persistence_owner_unlock(owner);

    result = requirement_build(coordinator, request, adapter->route_exact,
                               &adapter->route_requirement);
    if (result == UCN_OK) {
        result = ucn_i_coordinator_route_requirement(
            coordinator, &adapter->route_requirement, now_us,
            &adapter->route_handle);
    }

    if (ucn_i_persistence_owner_lock(owner) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (adapter->route_active == 0U || adapter->route_request != request) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result == UCN_OK &&
        (!ucn_i_persistence_handle_matches(
             owner, adapter->route_handle, &index) ||
         !ucn_i_persistence_pending_request_equal(
             &owner->domains[index].pending, request) ||
         adapter->bindings[index].valid == 0U ||
         memcmp(&adapter->bindings[index].handle, &adapter->route_handle,
                sizeof(adapter->route_handle)) != 0 ||
         adapter->bindings[index].requirement_digest !=
             adapter->route_requirement.requirement_digest)) {
        result = UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        *handle_out = adapter->route_handle;
    }
    adapter->route_active = 0U;
    adapter->route_request = NULL;
    memset(adapter->route_exact, 0, sizeof(adapter->route_exact));
    memset(&adapter->route_handle, 0, sizeof(adapter->route_handle));
    memset(&adapter->route_requirement, 0,
           sizeof(adapter->route_requirement));
    adapter->route_now_us = 0U;
    ucn_i_persistence_owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_persistence_route_terminal(
    ucn_i_coordinator_t *coordinator,
    ucn_i_persistence_coordinator_adapter_t *adapter,
    ucn_handle_t handle,
    uint64_t now_us)
{
    ucn_i_dependency_event_t event;
    ucn_i_persist_pending_t *pending;
    ucn_i_persist_route_binding_t *binding;
    ucn_persistence_owner_t *owner;
    uint8_t index;
    ucn_result_t result;

    if (!adapter_header_is_valid(adapter) || coordinator == NULL ||
        coordinator != adapter->coordinator) {
        return UCN_ERR_ARGUMENT;
    }
    owner = adapter->owner;
    result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (adapter->bound == 0U || owner->io.call_active != 0U ||
        adapter->route_active != 0U ||
        !ucn_i_persistence_handle_matches(owner, handle, &index)) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    pending = &owner->domains[index].pending;
    binding = &adapter->bindings[index];
    if (pending->terminal == 0U || binding->valid == 0U ||
        binding->requirement_digest == 0U ||
        memcmp(&binding->handle, &handle, sizeof(handle)) != 0) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&event, 0, sizeof(event));
    event.requirement_digest = binding->requirement_digest;
    event.runtime_instance = owner->runtime_instance;
    event.result = pending->terminal_result;
    event.dependency_handle = handle;
    event.owner_instance = owner->owner_instance;
    event.dependency_kind = UCN_I_DEP_PERSISTENCE;
    event.outcome = pending->proof_ready != 0U
                        ? UCN_I_DEPENDENCY_READY
                        : UCN_I_DEPENDENCY_FAILED;
    ucn_i_persistence_owner_unlock(owner);
    return ucn_i_coordinator_route_event(coordinator, &event, now_us);
}
