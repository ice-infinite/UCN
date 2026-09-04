#include "ucn/v6/ucn_v6_identity.h"

#include <limits.h>
#include <string.h>

static bool bytes_are_nontrivial(const uint8_t *bytes, size_t length)
{
    size_t index;
    bool any_nonzero = false;
    bool any_not_ff = false;

    if (bytes == NULL || length == 0U) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        any_nonzero = any_nonzero || bytes[index] != 0U;
        any_not_ff = any_not_ff || bytes[index] != UINT8_MAX;
    }
    return any_nonzero && any_not_ff;
}

static bool address_is_valid(uint32_t address)
{
    return address != 0U && address != UINT32_MAX;
}

static bool address_mode_is_valid(ucn_v6_address_mode_t mode)
{
    return mode == UCN_V6_ADDRESS_STATIC || mode == UCN_V6_ADDRESS_LEASED ||
           mode == UCN_V6_ADDRESS_SELF_PROPOSED;
}

static bool authority_epoch_is_valid(const ucn_v6_authority_epoch_t *epoch)
{
    return epoch != NULL &&
           ucn_v6_principal_is_valid(&epoch->authority_principal) &&
           epoch->authority_generation != 0U &&
           epoch->authority_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_are_nontrivial(epoch->durable_fence_token,
                                sizeof(epoch->durable_fence_token)) &&
           epoch->lease_sequence != 0U &&
           epoch->lease_sequence <= UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           epoch->lease_duration_us != 0U &&
           bytes_are_nontrivial(epoch->allocation_high_water_digest,
                                sizeof(epoch->allocation_high_water_digest)) &&
           epoch->quorum_proven && epoch->durable;
}

static bool authority_epoch_identity_equal(
    const ucn_v6_authority_epoch_t *left,
    const ucn_v6_authority_epoch_t *right)
{
    return left->authority_generation == right->authority_generation &&
           left->lease_sequence == right->lease_sequence &&
           left->lease_duration_us == right->lease_duration_us &&
           memcmp(left->authority_principal.bytes,
                  right->authority_principal.bytes,
                  sizeof(left->authority_principal.bytes)) == 0 &&
           memcmp(left->durable_fence_token,
                  right->durable_fence_token,
                  sizeof(left->durable_fence_token)) == 0 &&
           memcmp(left->allocation_high_water_digest,
                  right->allocation_high_water_digest,
                  sizeof(left->allocation_high_water_digest)) == 0;
}

static bool callback_gate_is_valid(const ucn_v6_callback_gate_t *gate)
{
    return gate != NULL && gate->initialized && gate->lock != NULL &&
           gate->unlock != NULL;
}

static bool callback_enter(ucn_v6_identity_authority_t *authority)
{
    return ucn_v6_callback_gate_try_enter(authority->callback_gate,
                                           authority) == UCN_V6_OK;
}

static void callback_exit(ucn_v6_identity_authority_t *authority)
{
    (void)ucn_v6_callback_gate_leave(authority->callback_gate, authority);
}

static bool authority_can_write(
    const ucn_v6_identity_authority_t *authority,
    uint64_t now_us)
{
    return authority != NULL && authority->epoch_valid && !authority->faulted &&
           callback_gate_is_valid(authority->callback_gate) &&
           !ucn_v6_callback_gate_is_active(authority->callback_gate) &&
           authority->epoch.quorum_proven &&
           authority->epoch.durable &&
           ucn_v6_lease_deadline_is_live(now_us,
                                         authority->local_lease_deadline_us);
}

static ucn_v6_binding_slot_t *find_binding_slot(
    ucn_v6_identity_authority_t *authority,
    uint32_t address,
    bool allow_empty)
{
    size_t index;
    ucn_v6_binding_slot_t *empty = NULL;

    for (index = 0U; index < UCN_V6_MAX_BINDING_SLOTS; ++index) {
        ucn_v6_binding_slot_t *slot = &authority->bindings[index];
        if (slot->occupied && slot->node_address == address) {
            return slot;
        }
        if (!slot->occupied && empty == NULL) {
            empty = slot;
        }
    }
    return allow_empty ? empty : NULL;
}

bool ucn_v6_principal_is_valid(const ucn_v6_principal_t *principal)
{
    return principal != NULL &&
           bytes_are_nontrivial(principal->bytes, sizeof(principal->bytes));
}

bool ucn_v6_binding_key_is_valid(const ucn_v6_binding_key_t *binding)
{
    return binding != NULL && binding->realm_id != 0U &&
           binding->realm_id != UINT32_MAX &&
           address_is_valid(binding->node_address) &&
           binding->binding_generation != 0U &&
           binding->binding_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

bool ucn_v6_binding_key_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right)
{
    return left != NULL && right != NULL &&
           left->realm_id == right->realm_id &&
           left->node_address == right->node_address &&
           left->binding_generation == right->binding_generation;
}

ucn_v6_result_t ucn_v6_serial_checked_next(uint32_t current, uint32_t *next)
{
    if (next == NULL || current > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (current == UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *next = current + 1U;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_lease_deadline_build(
    uint64_t challenge_started_local_us,
    uint64_t max_remaining_lease_us,
    const ucn_v6_lease_verifier_policy_t *policy,
    uint64_t *local_deadline_us)
{
    uint64_t ppm_product;
    uint64_t clock_margin;
    uint64_t read_margin;
    uint64_t quantization_margin;
    uint64_t safe_duration;
    uint64_t effective_duration;

    if (policy == NULL || local_deadline_us == NULL ||
        max_remaining_lease_us == 0U ||
        policy->local_timer_resolution_us == 0U ||
        !policy->timer_read_uncertainty_known ||
        policy->local_policy_max_lease_us == 0U ||
        policy->local_timer_max_slow_ppm > UINT32_C(1000000)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (policy->local_timer_max_slow_ppm != 0U &&
        max_remaining_lease_us >
            UINT64_MAX / (uint64_t)policy->local_timer_max_slow_ppm) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    ppm_product = max_remaining_lease_us *
                  (uint64_t)policy->local_timer_max_slow_ppm;
    clock_margin = ppm_product / UINT64_C(1000000);
    if (ppm_product % UINT64_C(1000000) != 0U) {
        ++clock_margin;
    }
    if (policy->local_timer_read_uncertainty_us > UINT64_MAX / 2U) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    read_margin = policy->local_timer_read_uncertainty_us * 2U;
    if (policy->local_timer_resolution_us > UINT64_MAX - read_margin) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    quantization_margin = policy->local_timer_resolution_us + read_margin;
    if (clock_margin >= max_remaining_lease_us ||
        quantization_margin >= max_remaining_lease_us - clock_margin) {
        return UCN_V6_ERR_TIMEOUT;
    }
    safe_duration = max_remaining_lease_us - clock_margin -
                    quantization_margin;
    effective_duration = safe_duration < policy->local_policy_max_lease_us ?
                             safe_duration :
                             policy->local_policy_max_lease_us;
    if (effective_duration == 0U ||
        challenge_started_local_us > UINT64_MAX - effective_duration) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *local_deadline_us = challenge_started_local_us + effective_duration;
    if (*local_deadline_us == 0U) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    return UCN_V6_OK;
}

bool ucn_v6_lease_deadline_is_live(uint64_t now_us, uint64_t deadline_us)
{
    return deadline_us != 0U && now_us < deadline_us;
}

ucn_v6_result_t ucn_v6_callback_gate_init(
    ucn_v6_callback_gate_t *gate,
    void *context,
    void (*lock)(void *context),
    void (*unlock)(void *context))
{
    ucn_v6_callback_gate_t initialized;

    if (gate == NULL || lock == NULL || unlock == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.context = context;
    initialized.lock = lock;
    initialized.unlock = unlock;
    initialized.initialized = true;
    *gate = initialized;
    return UCN_V6_OK;
}

bool ucn_v6_callback_gate_is_active(ucn_v6_callback_gate_t *gate)
{
    bool active;

    if (!callback_gate_is_valid(gate)) {
        return true;
    }
    gate->lock(gate->context);
    active = gate->active;
    gate->unlock(gate->context);
    return active;
}

ucn_v6_result_t ucn_v6_callback_gate_try_enter(
    ucn_v6_callback_gate_t *gate,
    const void *owner)
{
    if (!callback_gate_is_valid(gate) || owner == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    gate->lock(gate->context);
    if (gate->active) {
        gate->unlock(gate->context);
        return UCN_V6_ERR_STATE;
    }
    gate->active = true;
    gate->active_owner = owner;
    gate->unlock(gate->context);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_callback_gate_leave(
    ucn_v6_callback_gate_t *gate,
    const void *owner)
{
    if (!callback_gate_is_valid(gate) || owner == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    gate->lock(gate->context);
    if (!gate->active || gate->active_owner != owner) {
        gate->unlock(gate->context);
        return UCN_V6_ERR_STATE;
    }
    gate->active = false;
    gate->active_owner = NULL;
    gate->unlock(gate->context);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_init(
    ucn_v6_identity_authority_t *authority,
    uint32_t realm_id,
    const ucn_v6_identity_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate)
{
    if (authority == NULL || realm_id == 0U || realm_id == UINT32_MAX ||
        store == NULL || store->persist_authority_epoch == NULL ||
        store->persist_binding_slot == NULL ||
        store->persist_group_high_water == NULL ||
        !callback_gate_is_valid(callback_gate)) {
        return UCN_V6_ERR_CONFIG;
    }
    callback_gate->lock(callback_gate->context);
    if (callback_gate->active) {
        callback_gate->unlock(callback_gate->context);
        return UCN_V6_ERR_STATE;
    }
    memset(authority, 0, sizeof(*authority));
    authority->realm_id = realm_id;
    authority->store = *store;
    authority->callback_gate = callback_gate;
    callback_gate->unlock(callback_gate->context);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_install_epoch(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    uint64_t local_lease_deadline_us)
{
    uint32_t expected_generation;
    ucn_v6_result_t result;

    if (authority == NULL || authority->realm_id == 0U ||
        authority->faulted || !callback_gate_is_valid(authority->callback_gate) ||
        ucn_v6_callback_gate_is_active(authority->callback_gate) ||
        !authority_epoch_is_valid(epoch) || local_lease_deadline_us == 0U) {
        return UCN_V6_ERR_STATE;
    }
    if (authority->epoch_valid) {
        if (authority_epoch_identity_equal(&authority->epoch, epoch)) {
            return authority->local_lease_deadline_us == local_lease_deadline_us ?
                       UCN_V6_OK : UCN_V6_ERR_REPLAY;
        }
        result = ucn_v6_serial_checked_next(
            authority->epoch.authority_generation, &expected_generation);
        if (result != UCN_V6_OK ||
            epoch->authority_generation != expected_generation ||
            memcmp(authority->epoch.durable_fence_token,
                   epoch->durable_fence_token,
                   sizeof(epoch->durable_fence_token)) == 0 ||
            epoch->lease_sequence <= authority->epoch.lease_sequence) {
            return UCN_V6_ERR_REPLAY;
        }
    } else if (epoch->authority_generation != 1U) {
        return UCN_V6_ERR_STATE;
    }

    if (!callback_enter(authority)) {
        return UCN_V6_ERR_STATE;
    }
    result = authority->store.persist_authority_epoch(authority->store.context,
                                                       epoch);
    callback_exit(authority);
    if (result != UCN_V6_OK) {
        authority->faulted = true;
        return result;
    }
    authority->epoch = *epoch;
    authority->local_lease_deadline_us = local_lease_deadline_us;
    authority->epoch_valid = true;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_allocate_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    const ucn_v6_principal_t *device_principal,
    ucn_v6_address_mode_t mode,
    const uint8_t lease_id[16],
    uint64_t lease_duration_us,
    ucn_v6_binding_certificate_t *certificate)
{
    ucn_v6_binding_slot_t *slot;
    ucn_v6_binding_slot_t next_slot;
    uint32_t generation;
    ucn_v6_result_t result;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    if (!address_is_valid(node_address) ||
        !ucn_v6_principal_is_valid(device_principal) ||
        !address_mode_is_valid(mode) ||
        !bytes_are_nontrivial(lease_id, 16U) || lease_duration_us == 0U ||
        lease_duration_us > authority->epoch.lease_duration_us ||
        certificate == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_binding_slot(authority, node_address, true);
    if (slot == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (slot->occupied && slot->active) {
        return UCN_V6_ERR_STATE;
    }
    if (slot->occupied) {
        result = ucn_v6_serial_checked_next(slot->generation_high_water,
                                             &generation);
        if (result != UCN_V6_OK) {
            return result;
        }
    } else {
        generation = 1U;
    }

    memset(&next_slot, 0, sizeof(next_slot));
    next_slot.occupied = true;
    next_slot.active = true;
    next_slot.node_address = node_address;
    next_slot.generation_high_water = generation;
    next_slot.certificate.device_principal = *device_principal;
    next_slot.certificate.authority_principal =
        authority->epoch.authority_principal;
    next_slot.certificate.binding.realm_id = authority->realm_id;
    next_slot.certificate.binding.node_address = node_address;
    next_slot.certificate.binding.binding_generation = generation;
    next_slot.certificate.authority_generation =
        authority->epoch.authority_generation;
    memcpy(next_slot.certificate.lease_id, lease_id,
           sizeof(next_slot.certificate.lease_id));
    next_slot.certificate.lease_duration_us = lease_duration_us;
    next_slot.certificate.authority_lease_sequence =
        authority->epoch.lease_sequence;
    next_slot.certificate.mode = mode;

    if (!callback_enter(authority)) {
        return UCN_V6_ERR_STATE;
    }
    result = authority->store.persist_binding_slot(authority->store.context,
                                                    &next_slot);
    callback_exit(authority);
    if (result != UCN_V6_OK) {
        authority->faulted = true;
        return result;
    }
    *slot = next_slot;
    *certificate = next_slot.certificate;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_retire_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    uint32_t binding_generation)
{
    ucn_v6_binding_slot_t *slot;
    ucn_v6_binding_slot_t retired;
    ucn_v6_result_t result;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    slot = find_binding_slot(authority, node_address, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (!slot->active || slot->generation_high_water != binding_generation) {
        return UCN_V6_ERR_REPLAY;
    }
    retired = *slot;
    retired.active = false;
    if (!callback_enter(authority)) {
        return UCN_V6_ERR_STATE;
    }
    result = authority->store.persist_binding_slot(authority->store.context,
                                                    &retired);
    callback_exit(authority);
    if (result != UCN_V6_OK) {
        authority->faulted = true;
        return result;
    }
    *slot = retired;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_allocate_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t *group_id)
{
    size_t index;
    uint32_t next;
    ucn_v6_result_t result;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    if (group_id == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_MAX_ACTIVE_GROUPS; ++index) {
        if (authority->groups.active_group_ids[index] == 0U) {
            break;
        }
    }
    if (index == UCN_V6_MAX_ACTIVE_GROUPS) {
        return UCN_V6_ERR_NO_SPACE;
    }
    result = ucn_v6_serial_checked_next(
        authority->groups.dynamic_group_id_high_water, &next);
    if (result != UCN_V6_OK || !address_is_valid(next)) {
        return result == UCN_V6_OK ? UCN_V6_ERR_EXHAUSTED : result;
    }
    if (!callback_enter(authority)) {
        return UCN_V6_ERR_STATE;
    }
    result = authority->store.persist_group_high_water(authority->store.context,
                                                        next);
    callback_exit(authority);
    if (result != UCN_V6_OK) {
        authority->faulted = true;
        return result;
    }
    authority->groups.dynamic_group_id_high_water = next;
    authority->groups.active_group_ids[index] = next;
    *group_id = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_retire_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint32_t group_id)
{
    size_t index;

    if (authority == NULL || group_id == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_MAX_ACTIVE_GROUPS; ++index) {
        if (authority->groups.active_group_ids[index] == group_id) {
            authority->groups.active_group_ids[index] = 0U;
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}
