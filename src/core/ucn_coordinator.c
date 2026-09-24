#include "internal/ucn_coordinator.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_REQUIREMENT_CANONICAL_BYTES 56U
#define UCN_I_TERMINAL_NONE UINT8_C(0)
#define UCN_I_TERMINAL_STAGED UINT8_C(1)
#define UCN_I_TERMINAL_DELIVERED UINT8_C(2)

UCN_STATIC_ASSERT(UCN_I_DEPENDENCY_KIND_COUNT == UCN_I_DEP_PERSISTENCE,
                  dependency_kind_count_must_match_registry);

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

static void write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void write_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static void write_u64_le(uint8_t *output, uint64_t value)
{
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (8U * index));
    }
}

static bool dependency_kind_is_valid(ucn_i_dependency_kind_t kind)
{
    return kind >= UCN_I_DEP_IDENTITY_BINDING &&
           kind <= UCN_I_DEP_PERSISTENCE;
}

static ucn_i_owner_id_t owner_for_dependency(ucn_i_dependency_kind_t kind)
{
    static const uint8_t owner_by_kind[UCN_I_DEPENDENCY_KIND_COUNT] = {
        UCN_I_OWNER_IDENTITY,
        UCN_I_OWNER_SECURITY,
        UCN_I_OWNER_CAPABILITY,
        UCN_I_OWNER_ROUTE,
        UCN_I_OWNER_FLOW,
        UCN_I_OWNER_TRANSPORT,
        UCN_I_OWNER_GROUP,
        UCN_I_OWNER_TIME,
        UCN_I_OWNER_PERSISTENCE
    };

    return dependency_kind_is_valid(kind) ? owner_by_kind[kind - 1U] : 0U;
}

static void requirement_canonical_bytes_from_fields(
    ucn_i_dependency_kind_t kind,
    uint32_t runtime_instance,
    uint16_t requester_owner_instance,
    uint64_t absolute_deadline_us,
    uint64_t policy_digest,
    const uint8_t *exact,
    uint8_t exact_length,
    uint8_t output[UCN_I_REQUIREMENT_CANONICAL_BYTES])
{
    memset(output, 0, UCN_I_REQUIREMENT_CANONICAL_BYTES);
    output[0] = kind;
    write_u32_le(&output[1], runtime_instance);
    write_u16_le(&output[5], requester_owner_instance);
    write_u64_le(&output[7], absolute_deadline_us);
    write_u64_le(&output[15], policy_digest);
    output[23] = exact_length;
    memcpy(&output[24], exact, exact_length);
}

static void requirement_canonical_bytes(
    const ucn_i_dependency_requirement_t *requirement,
    uint8_t output[UCN_I_REQUIREMENT_CANONICAL_BYTES])
{
    requirement_canonical_bytes_from_fields(
        requirement->kind, requirement->runtime_instance,
        requirement->requester_owner_instance,
        requirement->absolute_deadline_us, requirement->policy_digest,
        requirement->exact, requirement->exact_length, output);
}

uint64_t ucn_i_requirement_digest_default(const uint8_t *bytes,
                                          size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;

    if (bytes == NULL && length != 0U) {
        return 0U;
    }
    for (index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash == 0U ? UINT64_C(1) : hash;
}

static uint64_t requirement_digest(
    ucn_i_requirement_digest_fn digest,
    const ucn_i_dependency_requirement_t *requirement)
{
    uint8_t canonical[UCN_I_REQUIREMENT_CANONICAL_BYTES];

    requirement_canonical_bytes(requirement, canonical);
    return digest(canonical, sizeof(canonical));
}

static bool requirement_shape_is_valid(
    const ucn_i_dependency_requirement_t *requirement)
{
    uint8_t index;

    if (requirement == NULL ||
        !dependency_kind_is_valid(requirement->kind) ||
        requirement->runtime_instance == 0U ||
        requirement->requester_owner_instance == 0U ||
        requirement->absolute_deadline_us == 0U ||
        requirement->policy_digest == 0U ||
        requirement->requirement_digest == 0U ||
        requirement->exact_length == 0U ||
        requirement->exact_length > UCN_I_DEPENDENCY_EXACT_BYTES) {
        return false;
    }
    for (index = requirement->exact_length;
         index < UCN_I_DEPENDENCY_EXACT_BYTES; ++index) {
        if (requirement->exact[index] != 0U) {
            return false;
        }
    }
    return true;
}

ucn_result_t ucn_i_dependency_requirement_build(
    ucn_i_requirement_digest_fn digest,
    ucn_i_dependency_kind_t kind,
    uint32_t runtime_instance,
    uint16_t requester_owner_instance,
    uint64_t absolute_deadline_us,
    uint64_t policy_digest,
    const uint8_t *exact,
    uint8_t exact_length,
    ucn_i_dependency_requirement_t *requirement_out)
{
    uint8_t canonical[UCN_I_REQUIREMENT_CANONICAL_BYTES];
    uint64_t requirement_digest_value;

    if (digest == NULL || !dependency_kind_is_valid(kind) ||
        runtime_instance == 0U || requester_owner_instance == 0U ||
        absolute_deadline_us == 0U || policy_digest == 0U || exact == NULL ||
        exact_length == 0U || exact_length > UCN_I_DEPENDENCY_EXACT_BYTES ||
        requirement_out == NULL ||
        ucn_i_ranges_overlap(exact, exact_length, requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    requirement_canonical_bytes_from_fields(
        kind, runtime_instance, requester_owner_instance,
        absolute_deadline_us, policy_digest, exact, exact_length, canonical);
    requirement_digest_value = digest(canonical, sizeof(canonical));
    if (requirement_digest_value == 0U) {
        return UCN_ERR_CONFIG;
    }
    memset(requirement_out, 0, sizeof(*requirement_out));
    requirement_out->requirement_digest = requirement_digest_value;
    requirement_out->policy_digest = policy_digest;
    requirement_out->absolute_deadline_us = absolute_deadline_us;
    requirement_out->runtime_instance = runtime_instance;
    requirement_out->requester_owner_instance = requester_owner_instance;
    requirement_out->kind = kind;
    requirement_out->exact_length = exact_length;
    memcpy(requirement_out->exact, exact, exact_length);
    return UCN_OK;
}

bool ucn_i_dependency_requirement_equal(
    const ucn_i_dependency_requirement_t *left,
    const ucn_i_dependency_requirement_t *right)
{
    return left != NULL && right != NULL &&
           left->requirement_digest == right->requirement_digest &&
           left->policy_digest == right->policy_digest &&
           left->absolute_deadline_us == right->absolute_deadline_us &&
           left->runtime_instance == right->runtime_instance &&
           left->requester_owner_instance ==
               right->requester_owner_instance &&
           left->kind == right->kind &&
           left->exact_length == right->exact_length &&
           memcmp(left->exact, right->exact,
                  UCN_I_DEPENDENCY_EXACT_BYTES) == 0;
}

static bool lock_ops_are_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->enter != NULL && lock->leave != NULL;
}

static bool pointer_is_inside_object(const void *pointer,
                                     const void *object,
                                     size_t object_size)
{
    return pointer != NULL &&
           ucn_i_ranges_overlap(object, object_size, pointer, 1U);
}

static bool coordinator_header_is_valid(const ucn_i_coordinator_t *coordinator)
{
    return coordinator != NULL &&
           coordinator->magic == UCN_I_COORDINATOR_MAGIC &&
           coordinator->runtime_instance != 0U &&
           coordinator->owner_instance != 0U &&
           coordinator->schema == UCN_I_COORDINATOR_SCHEMA &&
           coordinator->reserved_zero == 0U &&
           coordinator->digest != NULL && coordinator->event_sink != NULL &&
           lock_ops_are_valid(&coordinator->lock);
}

static bool dependency_event_shape_is_valid(
    const ucn_i_dependency_event_t *event);

static bool binding_is_valid(const ucn_i_owner_binding_t *binding)
{
    return binding != NULL && binding->ensure != NULL &&
           binding->retire != NULL &&
           binding->owner_instance != 0U && binding->slot_limit != 0U &&
           binding->owner_id >= UCN_I_OWNER_IDENTITY &&
           binding->owner_id <= UCN_I_OWNER_PERSISTENCE &&
           ((binding->object_kind >= UCN_OBJECT_KIND_SEND &&
             binding->object_kind <= UCN_OBJECT_KIND_TIME_DOMAIN) ||
            binding->object_kind == UCN_OBJECT_KIND_PERSISTENCE ||
            binding->object_kind == UCN_OBJECT_KIND_CLUSTER) &&
           binding->reserved_zero == 0U;
}

static bool coordinator_is_valid_locked(const ucn_i_coordinator_t *coordinator)
{
    size_t index;

    if (!coordinator_header_is_valid(coordinator) ||
        coordinator->route_active > 1U || coordinator->faulted > 1U) {
        return false;
    }
    for (index = 0U; index < UCN_I_DEPENDENCY_KIND_COUNT; ++index) {
        if (!bytes_are_zero(&coordinator->bindings[index],
                            sizeof(coordinator->bindings[index])) &&
            !binding_is_valid(&coordinator->bindings[index])) {
            return false;
        }
    }
    for (index = 0U; index < UCN_I_COORDINATOR_PENDING_LIMIT; ++index) {
        const ucn_i_coordinator_slot_t *slot = &coordinator->slots[index];
        if (slot->valid > 1U || !bytes_are_zero(slot->reserved_zero,
                                               sizeof(slot->reserved_zero))) {
            return false;
        }
        if (slot->valid == 0U) {
            if (!bytes_are_zero(&slot->requirement, sizeof(slot->requirement)) ||
                !bytes_are_zero(&slot->dependency_handle,
                                sizeof(slot->dependency_handle)) ||
                !bytes_are_zero(&slot->terminal_event,
                                sizeof(slot->terminal_event)) ||
                slot->terminal_state != UCN_I_TERMINAL_NONE) {
                return false;
            }
        } else {
            const ucn_i_owner_binding_t *binding;
            if (slot->terminal_state > UCN_I_TERMINAL_DELIVERED ||
                !requirement_shape_is_valid(&slot->requirement) ||
                requirement_digest(coordinator->digest,
                                   &slot->requirement) !=
                    slot->requirement.requirement_digest) {
                return false;
            }
            binding = &coordinator->bindings[slot->requirement.kind - 1U];
            if (!binding_is_valid(binding) ||
                binding->owner_id !=
                    owner_for_dependency(slot->requirement.kind) ||
                !ucn_i_handle_matches(&slot->dependency_handle,
                                      coordinator->runtime_instance,
                                      binding->owner_instance,
                                      binding->slot_limit,
                                      binding->object_kind)) {
                return false;
            }
            if (slot->terminal_state == UCN_I_TERMINAL_NONE) {
                if (!bytes_are_zero(&slot->terminal_event,
                                    sizeof(slot->terminal_event))) {
                    return false;
                }
            } else if (!dependency_event_shape_is_valid(
                           &slot->terminal_event) ||
                       slot->terminal_event.requirement_digest !=
                           slot->requirement.requirement_digest ||
                       slot->terminal_event.runtime_instance !=
                           coordinator->runtime_instance ||
                       slot->terminal_event.owner_instance !=
                           binding->owner_instance ||
                       slot->terminal_event.dependency_kind !=
                           slot->requirement.kind ||
                       memcmp(&slot->terminal_event.dependency_handle,
                              &slot->dependency_handle,
                              sizeof(slot->dependency_handle)) != 0) {
                return false;
            }
        }
    }
    return true;
}

ucn_result_t ucn_i_coordinator_init(
    ucn_i_coordinator_t *coordinator,
    uint32_t runtime_instance,
    uint16_t owner_instance,
    const ucn_i_lock_ops_t *lock,
    ucn_i_requirement_digest_fn digest,
    ucn_i_dependency_event_sink_fn event_sink,
    void *event_sink_context)
{
    if (coordinator == NULL || runtime_instance == 0U || owner_instance == 0U ||
        !lock_ops_are_valid(lock) || digest == NULL || event_sink == NULL ||
        ucn_i_ranges_overlap(coordinator, sizeof(*coordinator), lock,
                             sizeof(*lock)) ||
        pointer_is_inside_object(lock->context, coordinator,
                                 sizeof(*coordinator)) ||
        pointer_is_inside_object(event_sink_context, coordinator,
                                 sizeof(*coordinator))) {
        return UCN_ERR_ARGUMENT;
    }
    if (!bytes_are_zero(coordinator, sizeof(*coordinator))) {
        return UCN_ERR_STATE;
    }
    /* No fallible operation remains after this point.  Initialize the
     * caller-owned object in place instead of duplicating the complete fixed
     * slot table on the task stack. */
    memset(coordinator, 0, sizeof(*coordinator));
    coordinator->magic = UCN_I_COORDINATOR_MAGIC;
    coordinator->runtime_instance = runtime_instance;
    coordinator->owner_instance = owner_instance;
    coordinator->schema = UCN_I_COORDINATOR_SCHEMA;
    coordinator->event_sink_context = event_sink_context;
    coordinator->digest = digest;
    coordinator->event_sink = event_sink;
    coordinator->lock = *lock;
    return UCN_OK;
}

ucn_result_t ucn_i_coordinator_bind_owner(
    ucn_i_coordinator_t *coordinator,
    const ucn_i_owner_binding_t *binding)
{
    uint8_t kind;
    ucn_result_t result;

    if (!coordinator_header_is_valid(coordinator) ||
        !binding_is_valid(binding) ||
        ucn_i_ranges_overlap(coordinator, sizeof(*coordinator), binding,
                             sizeof(*binding)) ||
        pointer_is_inside_object(binding->context, coordinator,
                                 sizeof(*coordinator))) {
        return UCN_ERR_ARGUMENT;
    }
    result = coordinator->lock.enter(coordinator->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!coordinator_is_valid_locked(coordinator) ||
        coordinator->route_active != 0U || coordinator->faulted != 0U) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_STATE;
    }
    for (kind = UCN_I_DEP_IDENTITY_BINDING;
         kind <= UCN_I_DEP_PERSISTENCE; ++kind) {
        if (owner_for_dependency(kind) == binding->owner_id) {
            ucn_i_owner_binding_t *target = &coordinator->bindings[kind - 1U];
            size_t slot;
            for (slot = 0U; slot < UCN_I_COORDINATOR_PENDING_LIMIT; ++slot) {
                if (coordinator->slots[slot].valid != 0U &&
                    coordinator->slots[slot].requirement.kind == kind) {
                    coordinator->lock.leave(coordinator->lock.context);
                    return UCN_ERR_STATE;
                }
            }
            if (!bytes_are_zero(target, sizeof(*target))) {
                coordinator->lock.leave(coordinator->lock.context);
                return UCN_ERR_STATE;
            }
            *target = *binding;
            coordinator->lock.leave(coordinator->lock.context);
            return UCN_OK;
        }
    }
    coordinator->lock.leave(coordinator->lock.context);
    return UCN_ERR_ARGUMENT;
}

static bool requirement_is_valid(
    const ucn_i_coordinator_t *coordinator,
    const ucn_i_dependency_requirement_t *requirement)
{
    return requirement_shape_is_valid(requirement) &&
           requirement->runtime_instance == coordinator->runtime_instance &&
           requirement_digest(coordinator->digest, requirement) ==
               requirement->requirement_digest;
}

ucn_result_t ucn_i_coordinator_route_requirement(
    ucn_i_coordinator_t *coordinator,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    const ucn_i_owner_binding_t *binding;
    ucn_handle_t handle;
    size_t free_slot = UCN_I_COORDINATOR_PENDING_LIMIT;
    size_t index;
    ucn_result_t result;

    if (!coordinator_header_is_valid(coordinator) || requirement == NULL ||
        handle_out == NULL ||
        ucn_i_ranges_overlap(coordinator, sizeof(*coordinator), requirement,
                             sizeof(*requirement)) ||
        ucn_i_ranges_overlap(coordinator, sizeof(*coordinator), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(requirement, sizeof(*requirement), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = coordinator->lock.enter(coordinator->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!coordinator_is_valid_locked(coordinator) ||
        coordinator->route_active != 0U || coordinator->faulted != 0U ||
        !requirement_is_valid(coordinator, requirement)) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_STATE;
    }
    if (ucn_i_deadline_expired_us(now_us,
                                  requirement->absolute_deadline_us)) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_TIMEOUT;
    }
    for (index = 0U; index < UCN_I_COORDINATOR_PENDING_LIMIT; ++index) {
        ucn_i_coordinator_slot_t *slot = &coordinator->slots[index];
        if (slot->valid == 0U) {
            if (free_slot == UCN_I_COORDINATOR_PENDING_LIMIT) {
                free_slot = index;
            }
        } else if (slot->requirement.requirement_digest ==
                   requirement->requirement_digest) {
            if (!ucn_i_dependency_requirement_equal(&slot->requirement,
                                                     requirement)) {
                coordinator->lock.leave(coordinator->lock.context);
                return UCN_ERR_STATE;
            }
            handle = slot->dependency_handle;
            coordinator->lock.leave(coordinator->lock.context);
            *handle_out = handle;
            return UCN_OK;
        }
    }
    if (free_slot == UCN_I_COORDINATOR_PENDING_LIMIT) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_NO_SPACE;
    }
    binding = &coordinator->bindings[requirement->kind - 1U];
    if (!binding_is_valid(binding) ||
        binding->owner_id != owner_for_dependency(requirement->kind)) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_UNSUPPORTED;
    }
    coordinator->route_active = 1U;
    coordinator->lock.leave(coordinator->lock.context);

    memset(&handle, 0, sizeof(handle));
    result = binding->ensure(binding->context, requirement, now_us, &handle);

    if (coordinator->lock.enter(coordinator->lock.context) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!coordinator_is_valid_locked(coordinator) ||
        coordinator->route_active == 0U) {
        coordinator->faulted = 1U;
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_STATE;
    }
    if (result != UCN_OK) {
        coordinator->route_active = 0U;
        coordinator->lock.leave(coordinator->lock.context);
        return result;
    }
    if (!ucn_i_handle_matches(&handle, coordinator->runtime_instance,
                              binding->owner_instance, binding->slot_limit,
                              binding->object_kind)) {
        coordinator->route_active = 0U;
        coordinator->faulted = 1U;
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_COORDINATOR_PENDING_LIMIT; ++index) {
        if (coordinator->slots[index].valid != 0U &&
            memcmp(&coordinator->slots[index].dependency_handle, &handle,
                   sizeof(handle)) == 0) {
            /* A dependency Handle names one exact transaction.  Reusing it
             * for a different canonical requirement would make completion
             * routing ambiguous, so treat the Owner response as corrupt. */
            coordinator->route_active = 0U;
            coordinator->faulted = 1U;
            coordinator->lock.leave(coordinator->lock.context);
            return UCN_ERR_STATE;
        }
    }
    coordinator->slots[free_slot].requirement = *requirement;
    coordinator->slots[free_slot].dependency_handle = handle;
    coordinator->slots[free_slot].valid = 1U;
    coordinator->route_active = 0U;
    coordinator->lock.leave(coordinator->lock.context);
    *handle_out = handle;
    return UCN_OK;
}

static bool dependency_event_shape_is_valid(
    const ucn_i_dependency_event_t *event)
{
    return event != NULL && dependency_kind_is_valid(event->dependency_kind) &&
           event->runtime_instance != 0U && event->owner_instance != 0U &&
           event->requirement_digest != 0U &&
           event->dependency_handle.reserved_zero == 0U &&
           event->outcome >= UCN_I_DEPENDENCY_READY &&
           event->outcome <= UCN_I_DEPENDENCY_FENCED &&
           ((event->outcome == UCN_I_DEPENDENCY_READY &&
             event->result == UCN_OK) ||
            (event->outcome != UCN_I_DEPENDENCY_READY &&
             event->result != UCN_OK));
}

static bool terminal_accepts_owner_event(
    const ucn_i_dependency_event_t *terminal,
    const ucn_i_dependency_event_t *owner_event)
{
    if (memcmp(terminal, owner_event, sizeof(*terminal)) == 0) {
        return true;
    }
    return terminal->outcome == UCN_I_DEPENDENCY_FAILED &&
           terminal->result == UCN_ERR_TIMEOUT &&
           owner_event->outcome == UCN_I_DEPENDENCY_READY &&
           owner_event->result == UCN_OK &&
           terminal->requirement_digest == owner_event->requirement_digest &&
           terminal->runtime_instance == owner_event->runtime_instance &&
           terminal->owner_instance == owner_event->owner_instance &&
           terminal->dependency_kind == owner_event->dependency_kind &&
           memcmp(&terminal->dependency_handle,
                  &owner_event->dependency_handle,
                  sizeof(terminal->dependency_handle)) == 0;
}

ucn_result_t ucn_i_coordinator_route_event(
    ucn_i_coordinator_t *coordinator,
    const ucn_i_dependency_event_t *event,
    uint64_t now_us)
{
    const ucn_i_owner_binding_t *binding;
    ucn_i_coordinator_slot_t *slot;
    size_t matched_slot = UCN_I_COORDINATOR_PENDING_LIMIT;
    size_t index;
    bool event_was_delivered;
    ucn_result_t result;

    if (!coordinator_header_is_valid(coordinator) ||
        !dependency_event_shape_is_valid(event) ||
        ucn_i_ranges_overlap(coordinator, sizeof(*coordinator), event,
                             sizeof(*event))) {
        return UCN_ERR_ARGUMENT;
    }
    result = coordinator->lock.enter(coordinator->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!coordinator_is_valid_locked(coordinator) ||
        coordinator->route_active != 0U || coordinator->faulted != 0U ||
        event->runtime_instance != coordinator->runtime_instance) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_COORDINATOR_PENDING_LIMIT; ++index) {
        const ucn_i_coordinator_slot_t *candidate_slot =
            &coordinator->slots[index];
        if (candidate_slot->valid != 0U &&
            memcmp(&candidate_slot->dependency_handle,
                   &event->dependency_handle,
                   sizeof(event->dependency_handle)) == 0) {
            matched_slot = index;
            break;
        }
    }
    if (matched_slot == UCN_I_COORDINATOR_PENDING_LIMIT) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_NOT_FOUND;
    }
    slot = &coordinator->slots[matched_slot];
    binding = &coordinator->bindings[slot->requirement.kind - 1U];
    if (event->requirement_digest != slot->requirement.requirement_digest ||
        event->dependency_kind != slot->requirement.kind ||
        event->owner_instance != event->dependency_handle.owner_instance ||
        (slot->terminal_state != UCN_I_TERMINAL_NONE &&
         !terminal_accepts_owner_event(&slot->terminal_event, event))) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_STATE;
    }
    event_was_delivered =
        slot->terminal_state == UCN_I_TERMINAL_DELIVERED;
    if (slot->terminal_state == UCN_I_TERMINAL_NONE) {
        slot->terminal_event = *event;
        if (event->outcome == UCN_I_DEPENDENCY_READY &&
            ucn_i_deadline_expired_us(
                now_us, slot->requirement.absolute_deadline_us)) {
            slot->terminal_event.result = UCN_ERR_TIMEOUT;
            slot->terminal_event.outcome = UCN_I_DEPENDENCY_FAILED;
        }
        slot->terminal_state = UCN_I_TERMINAL_STAGED;
    }
    coordinator->route_active = 1U;
    coordinator->lock.leave(coordinator->lock.context);

    if (!event_was_delivered) {
        result = coordinator->event_sink(
            coordinator->event_sink_context,
            slot->requirement.requester_owner_instance,
            &slot->requirement, &slot->terminal_event);

        if (coordinator->lock.enter(coordinator->lock.context) != UCN_OK) {
            return UCN_ERR_STATE;
        }
        if (!coordinator_is_valid_locked(coordinator) ||
            coordinator->route_active == 0U || slot->valid == 0U ||
            slot->terminal_state != UCN_I_TERMINAL_STAGED) {
            coordinator->faulted = 1U;
            coordinator->lock.leave(coordinator->lock.context);
            return UCN_ERR_STATE;
        }
        if (result != UCN_OK) {
            memset(&slot->terminal_event, 0, sizeof(slot->terminal_event));
            slot->terminal_state = UCN_I_TERMINAL_NONE;
            coordinator->route_active = 0U;
            coordinator->lock.leave(coordinator->lock.context);
            return result;
        }
        slot->terminal_state = UCN_I_TERMINAL_DELIVERED;
        coordinator->lock.leave(coordinator->lock.context);
    }

    result = binding->retire(binding->context, &slot->dependency_handle);

    if (coordinator->lock.enter(coordinator->lock.context) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!coordinator_is_valid_locked(coordinator) ||
        coordinator->route_active == 0U || slot->valid == 0U ||
        slot->terminal_state != UCN_I_TERMINAL_DELIVERED ||
        !terminal_accepts_owner_event(&slot->terminal_event, event)) {
        coordinator->faulted = 1U;
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_STATE;
    }
    coordinator->route_active = 0U;
    if (result == UCN_OK) {
        memset(slot, 0, sizeof(*slot));
    }
    coordinator->lock.leave(coordinator->lock.context);
    return result;
}

ucn_result_t ucn_i_coordinator_destroy(ucn_i_coordinator_t *coordinator)
{
    ucn_i_lock_ops_t lock;
    size_t index;
    ucn_result_t result;

    if (!coordinator_header_is_valid(coordinator)) {
        return UCN_ERR_ARGUMENT;
    }
    result = coordinator->lock.enter(coordinator->lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!coordinator_is_valid_locked(coordinator) ||
        coordinator->route_active != 0U) {
        coordinator->lock.leave(coordinator->lock.context);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_COORDINATOR_PENDING_LIMIT; ++index) {
        if (coordinator->slots[index].valid != 0U) {
            coordinator->lock.leave(coordinator->lock.context);
            return UCN_ERR_STATE;
        }
    }
    lock = coordinator->lock;
    memset(coordinator, 0, sizeof(*coordinator));
    lock.leave(lock.context);
    return UCN_OK;
}
