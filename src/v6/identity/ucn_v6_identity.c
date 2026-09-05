#include "../internal/ucn_v6_identity_private.h"

#include "ucn/v6/ucn_v6_config.h"

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

typedef char ucn_v6_identity_storage_size_check[
    sizeof(ucn_v6_identity_authority_t) <=
            UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES ? 1 : -1];

static bool authority_storage_is_valid(
    const ucn_v6_identity_authority_t *authority)
{
    return authority != NULL &&
           authority->magic == UCN_V6_IDENTITY_AUTHORITY_MAGIC &&
           authority->schema == UCN_V6_STORAGE_LAYOUT &&
           authority->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           authority->canary == UCN_V6_IDENTITY_AUTHORITY_CANARY;
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

static bool authority_epoch_semantic_equal(
    const ucn_v6_authority_epoch_t *left,
    const ucn_v6_authority_epoch_t *right)
{
    return authority_epoch_identity_equal(left, right) &&
           left->quorum_proven == right->quorum_proven &&
           left->durable == right->durable;
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
    return authority_storage_is_valid(authority) && authority->epoch_valid &&
           !authority->faulted &&
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

static bool identity_store_is_valid(const ucn_v6_identity_store_ops_t *store)
{
    return store != NULL && store->load_generation_witness != NULL &&
           store->reserve_generation_witness != NULL && store->load != NULL &&
           store->submit != NULL;
}

static bool binding_certificate_is_valid(
    const ucn_v6_binding_certificate_t *certificate,
    uint32_t realm_id)
{
    return certificate != NULL &&
           ucn_v6_principal_is_valid(&certificate->device_principal) &&
           ucn_v6_principal_is_valid(&certificate->authority_principal) &&
           ucn_v6_binding_key_is_valid(&certificate->binding) &&
           certificate->binding.realm_id == realm_id &&
           certificate->authority_generation != 0U &&
           certificate->authority_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_are_nontrivial(certificate->lease_id,
                                sizeof(certificate->lease_id)) &&
           certificate->lease_duration_us != 0U &&
           certificate->authority_lease_sequence != 0U &&
           certificate->authority_lease_sequence <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           address_mode_is_valid(certificate->mode);
}

static bool binding_certificate_semantic_equal(
    const ucn_v6_binding_certificate_t *left,
    const ucn_v6_binding_certificate_t *right)
{
    return memcmp(left->device_principal.bytes,
                  right->device_principal.bytes,
                  sizeof(left->device_principal.bytes)) == 0 &&
           memcmp(left->authority_principal.bytes,
                  right->authority_principal.bytes,
                  sizeof(left->authority_principal.bytes)) == 0 &&
           ucn_v6_binding_key_equal(&left->binding, &right->binding) &&
           left->authority_generation == right->authority_generation &&
           memcmp(left->lease_id, right->lease_id,
                  sizeof(left->lease_id)) == 0 &&
           left->lease_duration_us == right->lease_duration_us &&
           left->authority_lease_sequence ==
               right->authority_lease_sequence &&
           left->mode == right->mode;
}

static bool binding_slot_semantic_equal(
    const ucn_v6_binding_slot_t *left,
    const ucn_v6_binding_slot_t *right)
{
    return left->occupied == right->occupied &&
           left->active == right->active &&
           left->node_address == right->node_address &&
           left->generation_high_water == right->generation_high_water &&
           binding_certificate_semantic_equal(&left->certificate,
                                              &right->certificate);
}

static bool group_allocator_is_valid(
    const ucn_v6_group_allocator_t *groups)
{
    size_t left;
    size_t right;

    if (groups == NULL || groups->dynamic_group_id_high_water >
                              UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return false;
    }
    for (left = 0U; left < UCN_V6_MAX_ACTIVE_GROUPS; ++left) {
        const uint32_t group_id = groups->active_group_ids[left];
        if (group_id == 0U) {
            continue;
        }
        if (!address_is_valid(group_id) ||
            group_id > groups->dynamic_group_id_high_water) {
            return false;
        }
        for (right = left + 1U; right < UCN_V6_MAX_ACTIVE_GROUPS; ++right) {
            if (groups->active_group_ids[right] == group_id) {
                return false;
            }
        }
    }
    return true;
}

static bool identity_snapshot_semantic_equal(
    const ucn_v6_identity_snapshot_t *left,
    const ucn_v6_identity_snapshot_t *right)
{
    size_t index;

    if (left->magic != right->magic || left->schema != right->schema ||
        left->reserved != right->reserved ||
        left->record_generation != right->record_generation ||
        left->realm_id != right->realm_id ||
        left->epoch_valid != right->epoch_valid ||
        !authority_epoch_semantic_equal(&left->epoch, &right->epoch) ||
        left->groups.dynamic_group_id_high_water !=
            right->groups.dynamic_group_id_high_water) {
        return false;
    }
    for (index = 0U; index < UCN_V6_MAX_BINDING_SLOTS; ++index) {
        if (!binding_slot_semantic_equal(&left->bindings[index],
                                         &right->bindings[index])) {
            return false;
        }
    }
    for (index = 0U; index < UCN_V6_MAX_ACTIVE_GROUPS; ++index) {
        if (left->groups.active_group_ids[index] !=
            right->groups.active_group_ids[index]) {
            return false;
        }
    }
    return true;
}

static bool identity_snapshot_is_valid(
    const ucn_v6_identity_snapshot_t *snapshot,
    uint32_t realm_id,
    bool allow_factory_empty)
{
    size_t left;
    size_t right;
    ucn_v6_identity_snapshot_t empty;

    if (snapshot == NULL || snapshot->magic != UCN_V6_IDENTITY_SNAPSHOT_MAGIC ||
        snapshot->schema != UCN_V6_IDENTITY_SNAPSHOT_SCHEMA ||
        snapshot->reserved != 0U || snapshot->realm_id != realm_id ||
        snapshot->record_generation > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        !group_allocator_is_valid(&snapshot->groups)) {
        return false;
    }
    if (snapshot->record_generation == 0U) {
        if (!allow_factory_empty) {
            return false;
        }
        memset(&empty, 0, sizeof(empty));
        empty.magic = UCN_V6_IDENTITY_SNAPSHOT_MAGIC;
        empty.schema = UCN_V6_IDENTITY_SNAPSHOT_SCHEMA;
        empty.realm_id = realm_id;
        return identity_snapshot_semantic_equal(snapshot, &empty);
    }
    if (snapshot->epoch_valid) {
        if (!authority_epoch_is_valid(&snapshot->epoch)) {
            return false;
        }
    } else {
        ucn_v6_authority_epoch_t empty_epoch;
        memset(&empty_epoch, 0, sizeof(empty_epoch));
        if (!authority_epoch_semantic_equal(&snapshot->epoch,
                                            &empty_epoch)) {
            return false;
        }
    }
    for (left = 0U; left < UCN_V6_MAX_BINDING_SLOTS; ++left) {
        const ucn_v6_binding_slot_t *slot = &snapshot->bindings[left];
        ucn_v6_binding_slot_t empty_slot;

        memset(&empty_slot, 0, sizeof(empty_slot));
        if (!slot->occupied) {
            if (!binding_slot_semantic_equal(slot, &empty_slot)) {
                return false;
            }
            continue;
        }
        if (!address_is_valid(slot->node_address) ||
            slot->generation_high_water == 0U ||
            slot->generation_high_water > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
            !binding_certificate_is_valid(&slot->certificate, realm_id) ||
            slot->certificate.binding.node_address != slot->node_address ||
            slot->certificate.binding.binding_generation !=
                slot->generation_high_water ||
            (slot->active && !snapshot->epoch_valid)) {
            return false;
        }
        for (right = left + 1U; right < UCN_V6_MAX_BINDING_SLOTS; ++right) {
            if (snapshot->bindings[right].occupied &&
                snapshot->bindings[right].node_address == slot->node_address) {
                return false;
            }
        }
    }
    return true;
}

static void snapshot_from_authority(
    const ucn_v6_identity_authority_t *authority,
    ucn_v6_identity_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = UCN_V6_IDENTITY_SNAPSHOT_MAGIC;
    snapshot->schema = UCN_V6_IDENTITY_SNAPSHOT_SCHEMA;
    snapshot->record_generation = authority->record_generation;
    snapshot->realm_id = authority->realm_id;
    snapshot->epoch_valid = authority->epoch_valid;
    snapshot->epoch = authority->epoch;
    memcpy(snapshot->bindings, authority->bindings,
           sizeof(snapshot->bindings));
    snapshot->groups = authority->groups;
}

static void authority_apply_snapshot(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_identity_snapshot_t *snapshot)
{
    authority->record_generation = snapshot->record_generation;
    authority->realm_id = snapshot->realm_id;
    authority->epoch_valid = snapshot->epoch_valid;
    authority->epoch = snapshot->epoch;
    memcpy(authority->bindings, snapshot->bindings,
           sizeof(authority->bindings));
    authority->groups = snapshot->groups;
}

static ucn_v6_result_t persist_snapshot(
    ucn_v6_identity_authority_t *authority,
    ucn_v6_identity_snapshot_t *candidate)
{
    ucn_v6_identity_snapshot_t loaded;
    uint64_t witness = 0U;
    ucn_v6_result_t result;

    if (authority->record_generation >=
        UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        authority->faulted = true;
        return UCN_V6_ERR_EXHAUSTED;
    }
    candidate->record_generation = authority->record_generation + 1U;
    if (!identity_snapshot_is_valid(candidate, authority->realm_id, false) ||
        !callback_enter(authority)) {
        return UCN_V6_ERR_STATE;
    }

    result = authority->store.reserve_generation_witness(
        authority->store.context, candidate->record_generation);
    if (result == UCN_V6_OK) {
        result = authority->store.submit(authority->store.context, candidate);
    }
    if (result == UCN_V6_OK) {
        memset(&loaded, 0, sizeof(loaded));
        result = authority->store.load_generation_witness(
            authority->store.context, &witness);
    }
    if (result == UCN_V6_OK) {
        result = authority->store.load(authority->store.context, &loaded);
    }
    callback_exit(authority);

    if (result != UCN_V6_OK || witness != candidate->record_generation ||
        !identity_snapshot_is_valid(&loaded, authority->realm_id, false) ||
        !identity_snapshot_semantic_equal(&loaded, candidate)) {
        authority->faulted = true;
        return result == UCN_V6_OK ? UCN_V6_ERR_STATE : result;
    }
    authority_apply_snapshot(authority, &loaded);
    return UCN_V6_OK;
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

ucn_v6_result_t ucn_v6_identity_authority_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    uint32_t realm_id,
    const ucn_v6_identity_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_identity_authority_t **authority_out)
{
    ucn_v6_identity_authority_t initialized;
    ucn_v6_identity_snapshot_t loaded;
    uint64_t witness = 0U;
    ucn_v6_result_t witness_result;
    ucn_v6_result_t load_result;
    ucn_v6_result_t result;

    if (authority_out == NULL || realm_id == 0U || realm_id == UINT32_MAX ||
        !identity_store_is_valid(store) ||
        !callback_gate_is_valid(callback_gate)) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(
        (const ucn_v6_feature_manifest_t *)manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(initialized),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    if (ucn_v6_callback_gate_try_enter(callback_gate, storage) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    memset(&loaded, 0, sizeof(loaded));
    witness_result = store->load_generation_witness(store->context, &witness);
    load_result = store->load(store->context, &loaded);
    (void)ucn_v6_callback_gate_leave(callback_gate, storage);

    if (witness_result == UCN_V6_ERR_NOT_FOUND &&
        load_result == UCN_V6_ERR_NOT_FOUND) {
        memset(&loaded, 0, sizeof(loaded));
        loaded.magic = UCN_V6_IDENTITY_SNAPSHOT_MAGIC;
        loaded.schema = UCN_V6_IDENTITY_SNAPSHOT_SCHEMA;
        loaded.realm_id = realm_id;
    } else if (witness_result != UCN_V6_OK || load_result != UCN_V6_OK ||
               witness == 0U || witness != loaded.record_generation ||
               !identity_snapshot_is_valid(&loaded, realm_id, false)) {
        return UCN_V6_ERR_STATE;
    }

    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_V6_IDENTITY_AUTHORITY_MAGIC;
    initialized.schema = UCN_V6_STORAGE_LAYOUT;
    initialized.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    initialized.store = *store;
    initialized.callback_gate = callback_gate;
    initialized.canary = UCN_V6_IDENTITY_AUTHORITY_CANARY;
    authority_apply_snapshot(&initialized, &loaded);
    initialized.local_lease_deadline_us = 0U;
    memcpy(storage, &initialized, sizeof(initialized));
    *authority_out = (ucn_v6_identity_authority_t *)storage;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_install_epoch(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    uint64_t local_lease_deadline_us)
{
    ucn_v6_identity_snapshot_t candidate;
    uint32_t expected_generation;
    ucn_v6_result_t result;

    if (!authority_storage_is_valid(authority) || authority->realm_id == 0U ||
        authority->faulted || !callback_gate_is_valid(authority->callback_gate) ||
        ucn_v6_callback_gate_is_active(authority->callback_gate) ||
        !authority_epoch_is_valid(epoch) || local_lease_deadline_us == 0U) {
        return UCN_V6_ERR_STATE;
    }
    if (authority->epoch_valid) {
        if (authority_epoch_identity_equal(&authority->epoch, epoch)) {
            if (authority->local_lease_deadline_us == 0U) {
                authority->local_lease_deadline_us = local_lease_deadline_us;
                return UCN_V6_OK;
            }
            return authority->local_lease_deadline_us ==
                           local_lease_deadline_us ?
                       UCN_V6_OK :
                       UCN_V6_ERR_REPLAY;
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

    snapshot_from_authority(authority, &candidate);
    candidate.epoch = *epoch;
    candidate.epoch_valid = true;
    result = persist_snapshot(authority, &candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
    authority->local_lease_deadline_us = local_lease_deadline_us;
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
    size_t slot_index;
    ucn_v6_identity_snapshot_t candidate;
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
    slot_index = (size_t)(slot - authority->bindings);
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

    snapshot_from_authority(authority, &candidate);
    candidate.bindings[slot_index] = next_slot;
    result = persist_snapshot(authority, &candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
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
    size_t slot_index;
    ucn_v6_identity_snapshot_t candidate;
    ucn_v6_binding_slot_t retired;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    slot = find_binding_slot(authority, node_address, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    slot_index = (size_t)(slot - authority->bindings);
    if (!slot->active || slot->generation_high_water != binding_generation) {
        return UCN_V6_ERR_REPLAY;
    }
    retired = *slot;
    retired.active = false;
    snapshot_from_authority(authority, &candidate);
    candidate.bindings[slot_index] = retired;
    return persist_snapshot(authority, &candidate);
}

ucn_v6_result_t ucn_v6_identity_authority_allocate_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t *group_id)
{
    size_t index;
    ucn_v6_identity_snapshot_t candidate;
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
    snapshot_from_authority(authority, &candidate);
    candidate.groups.dynamic_group_id_high_water = next;
    candidate.groups.active_group_ids[index] = next;
    result = persist_snapshot(authority, &candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
    *group_id = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_retire_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t group_id)
{
    size_t index;
    ucn_v6_identity_snapshot_t candidate;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    if (group_id == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_MAX_ACTIVE_GROUPS; ++index) {
        if (authority->groups.active_group_ids[index] == group_id) {
            snapshot_from_authority(authority, &candidate);
            candidate.groups.active_group_ids[index] = 0U;
            return persist_snapshot(authority, &candidate);
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_identity_authority_copy_view(
    const ucn_v6_identity_authority_t *authority,
    ucn_v6_identity_authority_view_t *view)
{
    ucn_v6_identity_authority_view_t next;
    size_t index;

    if (!authority_storage_is_valid(authority) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&next, 0, sizeof(next));
    next.record_generation = authority->record_generation;
    next.realm_id = authority->realm_id;
    next.epoch = authority->epoch;
    next.local_lease_deadline_us = authority->local_lease_deadline_us;
    next.dynamic_group_id_high_water =
        authority->groups.dynamic_group_id_high_water;
    next.epoch_valid = authority->epoch_valid;
    next.faulted = authority->faulted;
    for (index = 0U; index < UCN_V6_MAX_BINDING_SLOTS; ++index) {
        if (authority->bindings[index].occupied) {
            ++next.occupied_bindings;
        }
    }
    *view = next;
    return UCN_V6_OK;
}
