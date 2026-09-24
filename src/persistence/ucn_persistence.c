#include "internal/ucn_persistence.h"

#include <string.h>

UCN_STATIC_ASSERT(sizeof(struct ucn_persistence_owner) <=
                      UCN_PERSIST_STORAGE_BYTES,
                  persistence_owner_must_fit_public_storage);
UCN_STATIC_ASSERT(sizeof(struct ucn_persist_callback_gate) <=
                      UCN_PERSIST_GATE_STORAGE_BYTES,
                  persistence_gate_must_fit_public_storage);
UCN_STATIC_ASSERT(sizeof(ucn_i_persist_hash_workspace_t) <=
                      UCN_PERSIST_DIGEST_WORKSPACE_BYTES,
                  persistence_digest_workspace_must_fit_public_storage);

typedef uint8_t ucn_i_io_progress_t;
enum {
    UCN_I_IO_DONE = 1,
    UCN_I_IO_WAITING = 2,
    UCN_I_IO_GATE_BUSY = 3,
    UCN_I_IO_FAILED = 4,
    UCN_I_IO_LOCK_LOST = 5
};

static bool bytes_are_zero(const void *memory, size_t bytes)
{
    const uint8_t *cursor = (const uint8_t *)memory;
    size_t index;
    if (memory == NULL) {
        return false;
    }
    for (index = 0U; index < bytes; ++index) {
        if (cursor[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool range_is_aligned(const void *memory, size_t alignment)
{
    return memory != NULL && alignment != 0U &&
           ((uintptr_t)memory % alignment) == 0U;
}

static bool ranges_overlap(const void *left,
                           size_t left_bytes,
                           const void *right,
                           size_t right_bytes)
{
    const uintptr_t left_start = (uintptr_t)left;
    const uintptr_t right_start = (uintptr_t)right;
    if (left == NULL || right == NULL || left_bytes == 0U || right_bytes == 0U) {
        return false;
    }
    if (left_start > UINTPTR_MAX - left_bytes ||
        right_start > UINTPTR_MAX - right_bytes) {
        return true;
    }
    return left_start < right_start + right_bytes &&
           right_start < left_start + left_bytes;
}

static bool lock_is_valid(const ucn_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_API_VERSION && lock->enter != NULL &&
           lock->leave != NULL;
}

static bool locks_are_same(const ucn_lock_ops_t *left,
                           const ucn_lock_ops_t *right)
{
    return left != NULL && right != NULL &&
           left->context == right->context && left->enter == right->enter &&
           left->leave == right->leave;
}

ucn_result_t ucn_i_persistence_owner_lock(ucn_persistence_owner_t *owner)
{
    ucn_result_t result;
    if (owner == NULL || owner->magic != UCN_I_PERSIST_OWNER_MAGIC ||
        owner->schema != UCN_I_PERSIST_OWNER_SCHEMA ||
        !lock_is_valid(&owner->lock)) {
        return UCN_ERR_STATE;
    }
    result = owner->lock.enter(owner->lock.context);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->magic != UCN_I_PERSIST_OWNER_MAGIC ||
        owner->schema != UCN_I_PERSIST_OWNER_SCHEMA) {
        owner->lock.leave(owner->lock.context);
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

void ucn_i_persistence_owner_unlock(ucn_persistence_owner_t *owner)
{
    owner->lock.leave(owner->lock.context);
}

static ucn_result_t gate_lock(ucn_persist_callback_gate_t *gate)
{
    ucn_result_t result;
    if (gate == NULL || gate->magic != UCN_I_PERSIST_GATE_MAGIC ||
        gate->schema != UCN_I_PERSIST_GATE_SCHEMA ||
        !lock_is_valid(&gate->lock)) {
        return UCN_ERR_STATE;
    }
    result = gate->lock.enter(gate->lock.context);
    if (result != UCN_OK) {
        return result;
    }
    if (gate->magic != UCN_I_PERSIST_GATE_MAGIC ||
        gate->schema != UCN_I_PERSIST_GATE_SCHEMA) {
        gate->lock.leave(gate->lock.context);
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

static void gate_unlock(ucn_persist_callback_gate_t *gate)
{
    gate->lock.leave(gate->lock.context);
}

static ucn_result_t callback_gate_require_idle(
    ucn_persist_callback_gate_t *gate)
{
    ucn_result_t result = gate_lock(gate);
    if (result != UCN_OK) {
        return result;
    }
    if (gate->active != 0U) {
        gate_unlock(gate);
        return UCN_ERR_STATE;
    }
    gate_unlock(gate);
    return UCN_OK;
}

static ucn_result_t callback_gate_attach_owner(
    ucn_persist_callback_gate_t *gate)
{
    ucn_result_t result = gate_lock(gate);
    if (result != UCN_OK) {
        return result;
    }
    if (gate->active != 0U || gate->owner_references == UINT8_MAX) {
        gate_unlock(gate);
        return UCN_ERR_STATE;
    }
    ++gate->owner_references;
    gate_unlock(gate);
    return UCN_OK;
}

static ucn_result_t callback_gate_detach_owner(
    ucn_persist_callback_gate_t *gate)
{
    ucn_result_t result = gate_lock(gate);
    if (result != UCN_OK) {
        return result;
    }
    if (gate->active != 0U || gate->owner_references == 0U) {
        gate_unlock(gate);
        return UCN_ERR_STATE;
    }
    --gate->owner_references;
    gate_unlock(gate);
    return UCN_OK;
}

static ucn_result_t callback_gate_enter(ucn_persistence_owner_t *owner,
                                        uint64_t requested_token,
                                        uint8_t phase,
                                        uint64_t *token_out)
{
    ucn_persist_callback_gate_t *gate = owner->callback_gate;
    uint64_t token = requested_token;
    ucn_result_t result = gate_lock(gate);
    if (result != UCN_OK) {
        return result;
    }
    if (gate->active != 0U) {
        gate_unlock(gate);
        return UCN_ERR_STATE;
    }
    if (token == 0U) {
        if (gate->next_io_token == 0U ||
            gate->next_io_token == UINT64_MAX) {
            gate_unlock(gate);
            return UCN_ERR_EXHAUSTED;
        }
        token = gate->next_io_token;
        ++gate->next_io_token;
    } else if (token == UINT64_MAX || token >= gate->next_io_token) {
        gate_unlock(gate);
        return UCN_ERR_STATE;
    }
    gate->active = 1U;
    gate->runtime_instance = owner->runtime_instance;
    gate->owner_instance = owner->owner_instance;
    gate->phase = phase;
    gate->io_token = token;
    gate_unlock(gate);
    *token_out = token;
    return UCN_OK;
}

static ucn_result_t callback_gate_leave(
    ucn_persist_callback_gate_t *gate,
    uint32_t runtime_instance,
    uint16_t owner_instance,
    uint64_t token,
    uint8_t phase)
{
    ucn_result_t result = gate_lock(gate);
    if (result != UCN_OK) {
        return result;
    }
    if (gate->active == 0U || gate->runtime_instance != runtime_instance ||
        gate->owner_instance != owner_instance || gate->phase != phase ||
        gate->io_token != token) {
        gate_unlock(gate);
        return UCN_ERR_STATE;
    }
    gate->active = 0U;
    gate->runtime_instance = 0U;
    gate->owner_instance = 0U;
    gate->phase = 0U;
    gate->io_token = 0U;
    gate_unlock(gate);
    return UCN_OK;
}

static bool provider_is_valid(const ucn_persistence_provider_t *provider)
{
    const ucn_persistence_provider_vtable_t *vtable;
    if (provider == NULL || provider->struct_size != sizeof(*provider) ||
        provider->api_version != UCN_PERSIST_API_VERSION ||
        provider->context == NULL || provider->vtable == NULL ||
        provider->minimum_write_alignment == 0U ||
        provider->minimum_erase_alignment == 0U ||
        provider->maximum_slot_bytes <
            UCN_PERSIST_ENVELOPE_BYTES + UCN_PERSIST_COMMIT_MARKER_BYTES ||
        provider->atomic_marker_bytes < UCN_PERSIST_COMMIT_MARKER_BYTES ||
        provider->reserved_zero != 0U) {
        return false;
    }
    vtable = provider->vtable;
    return vtable->struct_size == sizeof(*vtable) &&
           vtable->api_version == UCN_PERSIST_API_VERSION &&
           vtable->begin_load_slot != NULL &&
           vtable->begin_write_inactive != NULL &&
           vtable->begin_readback != NULL &&
           vtable->begin_publish_commit_marker != NULL &&
           vtable->begin_load_witness != NULL &&
           vtable->begin_advance_witness != NULL;
}

static bool digest_workspace_is_valid(
    const ucn_persistence_config_t *config)
{
    ucn_persistence_digest_workspace_t *workspace = config->digest_workspace;
    const size_t entry_bytes =
        (size_t)config->manifest->entry_count *
        sizeof(config->manifest->entries[0]);
    const size_t binding_bytes =
        (size_t)config->domain_binding_count *
        sizeof(config->domain_bindings[0]);
    return workspace != NULL &&
           !ranges_overlap(workspace, sizeof(*workspace), config,
                           sizeof(*config)) &&
           !ranges_overlap(workspace, sizeof(*workspace), config->manifest,
                           sizeof(*config->manifest)) &&
           !ranges_overlap(workspace, sizeof(*workspace),
                           config->manifest->entries, entry_bytes) &&
           !ranges_overlap(workspace, sizeof(*workspace),
                           config->domain_bindings, binding_bytes) &&
           !ranges_overlap(workspace, sizeof(*workspace), config->provider,
                           sizeof(*config->provider)) &&
           !ranges_overlap(workspace, sizeof(*workspace),
                           config->provider->vtable,
                           sizeof(*config->provider->vtable)) &&
           !ranges_overlap(workspace, sizeof(*workspace),
                           config->provider->context, 1U) &&
           !ranges_overlap(workspace, sizeof(*workspace),
                           config->state_lock.context, 1U) &&
           !ranges_overlap(workspace, sizeof(*workspace),
                           config->shared_callback_gate,
                           UCN_PERSIST_GATE_STORAGE_BYTES) &&
           !ranges_overlap(workspace, sizeof(*workspace),
                           config->shared_callback_gate->lock.context, 1U);
}

static bool owner_config_is_valid(const ucn_persistence_config_t *config,
                                  uint8_t digest[UCN_PERSIST_DIGEST_BYTES])
{
    uint16_t index;
    if (config == NULL || config->struct_size != sizeof(*config) ||
        config->api_version != UCN_PERSIST_API_VERSION ||
        config->runtime_instance == 0U || config->owner_instance == 0U ||
        config->reserved_zero != 0U || config->reserved_zero2 != 0U ||
        config->manifest == NULL || config->domain_bindings == NULL ||
        config->shared_callback_gate == NULL ||
        !lock_is_valid(&config->state_lock) ||
        !lock_is_valid(&config->shared_callback_gate->lock) ||
        locks_are_same(&config->state_lock,
                       &config->shared_callback_gate->lock) ||
        !provider_is_valid(config->provider) ||
        !digest_workspace_is_valid(config) ||
        ucn_persistence_manifest_digest(config->manifest,
                                        config->digest_workspace,
                                        digest) != UCN_OK ||
        memcmp(digest, config->manifest->expected_digest,
               UCN_PERSIST_DIGEST_BYTES) != 0 ||
        config->domain_binding_count != config->manifest->entry_count ||
        (config->manifest->entry_count < 32U &&
         (config->required_domain_mask >> config->manifest->entry_count) !=
             0U)) {
        return false;
    }
    for (index = 0U; index < config->manifest->entry_count; ++index) {
        const ucn_persist_domain_binding_t *binding =
            &config->domain_bindings[index];
        if (config->manifest->entries[index].slot_capacity_bytes >
                config->provider->maximum_slot_bytes ||
            config->manifest->entries[index].slot_capacity_bytes %
                    config->provider->minimum_write_alignment !=
                0U ||
            config->manifest->entries[index].slot_capacity_bytes %
                    config->provider->minimum_erase_alignment !=
                0U ||
            binding->struct_size != sizeof(*binding) ||
            binding->api_version != UCN_PERSIST_API_VERSION ||
            !ucn_i_persist_domain_equal(
                binding->domain, config->manifest->entries[index].domain) ||
            binding->business_owner_instance == 0U ||
            binding->domain_generation == 0U ||
            binding->domain_generation == UINT16_MAX ||
            binding->reserved_zero != 0U) {
            return false;
        }
    }
    return true;
}

size_t ucn_persistence_storage_required(void)
{
    return UCN_PERSIST_STORAGE_BYTES;
}

size_t ucn_persist_gate_storage_required(void)
{
    return UCN_PERSIST_GATE_STORAGE_BYTES;
}

ucn_result_t ucn_persist_callback_gate_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_lock_ops_t *lock,
    ucn_persist_callback_gate_t **gate_out)
{
    ucn_persist_callback_gate_t *gate;
    if (storage == NULL || gate_out == NULL ||
        storage_bytes < UCN_PERSIST_GATE_STORAGE_BYTES ||
        !range_is_aligned(storage, UCN_PERSIST_GATE_STORAGE_ALIGNMENT) ||
        !bytes_are_zero(storage, UCN_PERSIST_GATE_STORAGE_BYTES) ||
        !lock_is_valid(lock) ||
        ranges_overlap(storage, UCN_PERSIST_GATE_STORAGE_BYTES,
                       lock->context, 1U) ||
        ranges_overlap(storage, UCN_PERSIST_GATE_STORAGE_BYTES, lock,
                       sizeof(*lock)) ||
        ranges_overlap(storage, UCN_PERSIST_GATE_STORAGE_BYTES, gate_out,
                       sizeof(*gate_out))) {
        return UCN_ERR_CONFIG;
    }
    gate = (ucn_persist_callback_gate_t *)storage;
    memset(gate, 0, sizeof(*gate));
    gate->magic = UCN_I_PERSIST_GATE_MAGIC;
    gate->schema = UCN_I_PERSIST_GATE_SCHEMA;
    gate->next_io_token = 1U;
    gate->lock = *lock;
    *gate_out = gate;
    return UCN_OK;
}

ucn_result_t ucn_persist_callback_gate_deinit(
    ucn_persist_callback_gate_t *gate)
{
    ucn_result_t result = gate_lock(gate);
    if (result != UCN_OK) {
        return result;
    }
    if (gate->active != 0U || gate->owner_references != 0U) {
        gate_unlock(gate);
        return UCN_ERR_STATE;
    }
    gate->magic = 0U;
    gate->schema = 0U;
    gate_unlock(gate);
    memset(gate, 0, UCN_PERSIST_GATE_STORAGE_BYTES);
    return UCN_OK;
}

ucn_result_t ucn_persistence_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_persistence_config_t *config,
    ucn_persistence_owner_t **owner_out)
{
    ucn_persistence_owner_t *owner;
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    uint16_t index;
    if (storage == NULL || owner_out == NULL || config == NULL ||
        config->digest_workspace == NULL ||
        storage_bytes < UCN_PERSIST_STORAGE_BYTES ||
        !range_is_aligned(storage, UCN_PERSIST_STORAGE_ALIGNMENT) ||
        !bytes_are_zero(storage, UCN_PERSIST_STORAGE_BYTES) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES,
                       config->digest_workspace,
                       sizeof(*config->digest_workspace)) ||
        ranges_overlap(config->digest_workspace,
                       sizeof(*config->digest_workspace), owner_out,
                       sizeof(*owner_out)) ||
        !owner_config_is_valid(config, digest) ||
        callback_gate_require_idle(config->shared_callback_gate) != UCN_OK ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES, config,
                       sizeof(*config)) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES, owner_out,
                       sizeof(*owner_out)) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES, config->manifest,
                       sizeof(*config->manifest)) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES,
                       config->manifest->entries,
                       (size_t)config->manifest->entry_count *
                           sizeof(config->manifest->entries[0])) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES,
                       config->domain_bindings,
                       (size_t)config->domain_binding_count *
                           sizeof(config->domain_bindings[0])) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES, config->provider,
                       sizeof(*config->provider)) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES,
                       config->provider->vtable,
                       sizeof(*config->provider->vtable)) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES,
                       config->provider->context, 1U) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES,
                       config->state_lock.context, 1U) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES,
                       config->shared_callback_gate->lock.context, 1U) ||
        ranges_overlap(storage, UCN_PERSIST_STORAGE_BYTES,
                       config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES) ||
        ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES,
                       config->provider->context, 1U) ||
        ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES,
                       config->state_lock.context, 1U)) {
        return UCN_ERR_CONFIG;
    }
    if (ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES, config,
                       sizeof(*config)) ||
        ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES, owner_out,
                       sizeof(*owner_out)) ||
        ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES, config->manifest,
                       sizeof(*config->manifest)) ||
        ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES,
                       config->manifest->entries,
                       (size_t)config->manifest->entry_count *
                           sizeof(config->manifest->entries[0])) ||
        ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES,
                       config->domain_bindings,
                       (size_t)config->domain_binding_count *
                           sizeof(config->domain_bindings[0])) ||
        ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES, config->provider,
                       sizeof(*config->provider)) ||
        ranges_overlap(config->shared_callback_gate,
                       UCN_PERSIST_GATE_STORAGE_BYTES,
                       config->provider->vtable,
                       sizeof(*config->provider->vtable))) {
        return UCN_ERR_CONFIG;
    }
    if (callback_gate_attach_owner(config->shared_callback_gate) != UCN_OK) {
        return UCN_ERR_CONFIG;
    }
    owner = (ucn_persistence_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_I_PERSIST_OWNER_MAGIC;
    owner->schema = UCN_I_PERSIST_OWNER_SCHEMA;
    owner->owner_instance = config->owner_instance;
    owner->runtime_instance = config->runtime_instance;
    owner->owner_phase = UCN_I_PERSIST_OWNER_INITIALIZED;
    owner->domain_count = (uint8_t)config->manifest->entry_count;
    owner->active_domain = UCN_I_PERSIST_INVALID_INDEX;
    owner->lock = config->state_lock;
    owner->callback_gate = config->shared_callback_gate;
    owner->provider = *config->provider;
    memcpy(owner->durable_manifest_digest, digest, sizeof(digest));
    for (index = 0U; index < config->manifest->entry_count; ++index) {
        ucn_i_persist_domain_t *domain = &owner->domains[index];
        domain->manifest = config->manifest->entries[index];
        domain->business_owner_instance =
            config->domain_bindings[index].business_owner_instance;
        domain->domain_generation =
            config->domain_bindings[index].domain_generation;
        domain->next_handle_generation = 1U;
        domain->state = UCN_PERSIST_DOMAIN_RECOVERING;
        domain->active_slot = UCN_I_PERSIST_INVALID_INDEX;
        domain->required =
            (uint8_t)((config->required_domain_mask >> index) & 1U);
    }
    *owner_out = owner;
    return UCN_OK;
}

uint8_t ucn_i_persistence_find_domain_index(
    const ucn_persistence_owner_t *owner,
    ucn_persist_domain_key_t key)
{
    uint8_t index;
    for (index = 0U; index < owner->domain_count; ++index) {
        if (ucn_i_persist_domain_equal(owner->domains[index].manifest.domain,
                                       key)) {
            return index;
        }
    }
    return UCN_I_PERSIST_INVALID_INDEX;
}

bool ucn_i_persistence_handle_matches(
    const ucn_persistence_owner_t *owner,
    ucn_handle_t handle,
    uint8_t *domain_index_out)
{
    uint8_t index;
    if (handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.object_kind != UCN_OBJECT_KIND_PERSISTENCE ||
        handle.reserved_zero != 0U || handle.slot == 0U ||
        handle.slot > owner->domain_count || handle.generation == 0U) {
        return false;
    }
    index = (uint8_t)(handle.slot - 1U);
    if (owner->domains[index].pending.valid == 0U ||
        owner->domains[index].pending.handle_generation != handle.generation) {
        return false;
    }
    if (domain_index_out != NULL) {
        *domain_index_out = index;
    }
    return true;
}

static ucn_handle_t make_handle(const ucn_persistence_owner_t *owner,
                                uint8_t domain_index,
                                uint16_t generation)
{
    ucn_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = (uint16_t)domain_index + 1U;
    handle.generation = generation;
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    return handle;
}

ucn_result_t ucn_i_persistence_start_recovery(ucn_persistence_owner_t *owner)
{
    ucn_result_t result = ucn_i_persistence_owner_lock(owner);
    uint8_t index;
    if (result != UCN_OK) {
        return result;
    }
    if (owner->io.call_active != 0U || owner->io.active != 0U ||
        owner->owner_phase != UCN_I_PERSIST_OWNER_INITIALIZED ||
        callback_gate_require_idle(owner->callback_gate) != UCN_OK) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->owner_phase = UCN_I_PERSIST_OWNER_RECOVERING;
    owner->recovery_cursor = 0U;
    owner->active_domain = 0U;
    owner->work_phase = UCN_I_PERSIST_RECOVERY_LOAD_WITNESS;
    memset(owner->slot_state, 0, sizeof(owner->slot_state));
    for (index = 0U; index < owner->domain_count; ++index) {
        owner->domains[index].state = UCN_PERSIST_DOMAIN_RECOVERING;
    }
    ucn_i_persistence_owner_unlock(owner);
    return UCN_OK;
}

static bool completion_is_valid(const ucn_persistence_owner_t *owner,
                                 uint32_t expected_bytes)
{
    const ucn_persist_io_completion_t *completion = &owner->completion;
    bool state_is_valid;
    if (owner->io.phase == UCN_PERSIST_IO_LOAD_WITNESS) {
        state_is_valid = completion->blob_state >= UCN_PERSIST_BLOB_PRESENT &&
                         completion->blob_state <= UCN_PERSIST_BLOB_FAULT;
    } else if (owner->io.phase == UCN_PERSIST_IO_LOAD_SLOT) {
        /* A slot load always returns the exact raw medium bytes.  Foundation,
         * not Provider-private metadata, classifies the commit marker. */
        state_is_valid = completion->blob_state == UCN_PERSIST_BLOB_PRESENT;
    } else {
        state_is_valid = completion->blob_state == UCN_PERSIST_BLOB_PRESENT;
    }
    return state_is_valid &&
           completion->struct_size == sizeof(*completion) &&
           completion->api_version == UCN_PERSIST_API_VERSION &&
           completion->io_token == owner->io.token &&
           completion->phase == owner->io.phase &&
           completion->slot_index == owner->io.slot_index &&
           completion->reserved_zero == 0U &&
           completion->result == UCN_OK &&
           completion->exact_bytes == expected_bytes;
}

static ucn_persist_io_start_t call_provider(ucn_persistence_owner_t *owner,
                                            bool poll)
{
    const ucn_persistence_provider_vtable_t *vtable = owner->provider.vtable;
    const uint8_t index = owner->io.domain_index;
    const ucn_i_persist_domain_t *domain = &owner->domains[index];
    const size_t slot_bytes = domain->manifest.slot_capacity_bytes;
    const ucn_persist_domain_key_t key = domain->manifest.domain;
    if (poll) {
        if (vtable->poll == NULL) {
            return UCN_PERSIST_IO_FAILED;
        }
        return vtable->poll(owner->provider.context, owner->io.token,
                            owner->io.phase, &owner->completion);
    }
    switch (owner->io.phase) {
    case UCN_PERSIST_IO_LOAD_SLOT:
        return vtable->begin_load_slot(
            owner->provider.context, key, owner->io.slot_index,
            owner->slot_buffer[owner->io.slot_index], slot_bytes,
            owner->io.token, &owner->completion);
    case UCN_PERSIST_IO_WRITE_INACTIVE:
        return vtable->begin_write_inactive(
            owner->provider.context, key, owner->io.slot_index,
            owner->write_buffer, slot_bytes, owner->io.token,
            &owner->completion);
    case UCN_PERSIST_IO_READBACK:
        return vtable->begin_readback(
            owner->provider.context, key, owner->io.slot_index,
            owner->readback_buffer, slot_bytes, owner->io.token,
            &owner->completion);
    case UCN_PERSIST_IO_PUBLISH_MARKER:
        return vtable->begin_publish_commit_marker(
            owner->provider.context, key, owner->io.slot_index,
            owner->marker_buffer, owner->io.token, &owner->completion);
    case UCN_PERSIST_IO_LOAD_WITNESS:
        return vtable->begin_load_witness(
            owner->provider.context, key, &owner->witness, owner->io.token,
            &owner->completion);
    case UCN_PERSIST_IO_ADVANCE_WITNESS:
        return vtable->begin_advance_witness(
            owner->provider.context, key,
            domain->record_generation,
            domain->record_generation + 1U,
            owner->io.token, &owner->completion);
    default:
        return UCN_PERSIST_IO_FAILED;
    }
}

static uint32_t phase_exact_bytes(const ucn_persistence_owner_t *owner,
                                  uint8_t phase,
                                  uint8_t domain_index)
{
    switch (phase) {
    case UCN_PERSIST_IO_LOAD_SLOT:
    case UCN_PERSIST_IO_WRITE_INACTIVE:
    case UCN_PERSIST_IO_READBACK:
        return owner->domains[domain_index].manifest.slot_capacity_bytes;
    case UCN_PERSIST_IO_PUBLISH_MARKER:
        return UCN_PERSIST_COMMIT_MARKER_BYTES;
    case UCN_PERSIST_IO_LOAD_WITNESS:
        return (uint32_t)sizeof(ucn_persist_witness_view_t);
    case UCN_PERSIST_IO_ADVANCE_WITNESS:
        return (uint32_t)sizeof(uint64_t);
    default:
        return 0U;
    }
}

/* Called with the Owner lock held and returns with it held. */
static ucn_i_io_progress_t perform_io_locked(ucn_persistence_owner_t *owner,
                                             uint8_t phase,
                                             uint8_t domain_index,
                                             uint8_t slot_index,
                                             bool *owner_lock_held_out)
{
    ucn_persist_io_start_t start;
    ucn_result_t lock_result;
    bool polling = owner->io.active != 0U;
    ucn_persist_callback_gate_t *const callback_gate = owner->callback_gate;
    const uint32_t runtime_instance = owner->runtime_instance;
    const uint16_t owner_instance = owner->owner_instance;
    uint64_t token;
    uint32_t exact_bytes;

    *owner_lock_held_out = true;
    if (domain_index >= owner->domain_count ||
        ((phase == UCN_PERSIST_IO_LOAD_SLOT ||
          phase == UCN_PERSIST_IO_WRITE_INACTIVE ||
          phase == UCN_PERSIST_IO_READBACK ||
          phase == UCN_PERSIST_IO_PUBLISH_MARKER) &&
         slot_index >= UCN_PERSIST_SLOT_COUNT)) {
        return UCN_I_IO_FAILED;
    }
    exact_bytes = phase_exact_bytes(owner, phase, domain_index);
    if (exact_bytes == 0U) {
        return UCN_I_IO_FAILED;
    }
    if (polling) {
        if (owner->io.phase != phase ||
            owner->io.domain_index != domain_index ||
            owner->io.slot_index != slot_index ||
            owner->io.exact_bytes != exact_bytes) {
            return UCN_I_IO_FAILED;
        }
        token = owner->io.token;
    } else {
        token = 0U;
    }
    lock_result = callback_gate_enter(owner, token, phase, &token);
    if (lock_result == UCN_ERR_EXHAUSTED) {
        return UCN_I_IO_FAILED;
    }
    if (lock_result != UCN_OK) {
        return UCN_I_IO_GATE_BUSY;
    }
    if (!polling) {
        owner->io.token = token;
        owner->io.phase = phase;
        owner->io.domain_index = domain_index;
        owner->io.slot_index = slot_index;
        owner->io.exact_bytes = exact_bytes;
    }
    owner->io.call_active = 1U;
    memset(&owner->completion, 0, sizeof(owner->completion));
    if (phase == UCN_PERSIST_IO_LOAD_WITNESS) {
        memset(&owner->witness, 0, sizeof(owner->witness));
    } else if (phase == UCN_PERSIST_IO_LOAD_SLOT) {
        memset(owner->slot_buffer[slot_index], owner->provider.erased_value,
               owner->domains[domain_index].manifest.slot_capacity_bytes);
    } else if (phase == UCN_PERSIST_IO_READBACK) {
        memset(owner->readback_buffer, owner->provider.erased_value,
               owner->domains[domain_index].manifest.slot_capacity_bytes);
    }
    ucn_i_persistence_owner_unlock(owner);
    start = call_provider(owner, polling);
    lock_result = ucn_i_persistence_owner_lock(owner);
    if (lock_result != UCN_OK) {
        (void)callback_gate_leave(callback_gate, runtime_instance,
                                  owner_instance, token, phase);
        *owner_lock_held_out = false;
        return UCN_I_IO_LOCK_LOST;
    }
    owner->io.call_active = 0U;
    if (callback_gate_leave(callback_gate, runtime_instance, owner_instance,
                            token, phase) != UCN_OK) {
        owner->io.active = 0U;
        return UCN_I_IO_FAILED;
    }
    if (start == UCN_PERSIST_IO_PENDING) {
        owner->io.active = 1U;
        return UCN_I_IO_WAITING;
    }
    owner->io.active = 0U;
    if (start != UCN_PERSIST_IO_COMPLETED ||
        !completion_is_valid(owner, exact_bytes)) {
        return UCN_I_IO_FAILED;
    }
    return UCN_I_IO_DONE;
}

static void fault_domain_locked(ucn_persistence_owner_t *owner,
                                uint8_t domain_index)
{
    ucn_i_persist_domain_t *domain = &owner->domains[domain_index];
    domain->state = UCN_PERSIST_DOMAIN_FAULTED;
    if (domain->pending.valid != 0U && domain->pending.proof_ready == 0U) {
        domain->pending.terminal = 1U;
        domain->pending.terminal_result = UCN_ERR_IN_DOUBT;
    }
    owner->io.active = 0U;
    owner->io.call_active = 0U;
    owner->active_domain = UCN_I_PERSIST_INVALID_INDEX;
    owner->work_phase = UCN_I_PERSIST_WORK_IDLE;
    if (domain->required != 0U) {
        owner->owner_phase = UCN_I_PERSIST_OWNER_FAULTED;
    }
}

static ucn_result_t decode_loaded_slot_locked(
    ucn_persistence_owner_t *owner,
    uint8_t domain_index,
    uint8_t slot_index,
    ucn_i_persist_record_meta_t *meta_out,
    uint8_t *body_out)
{
    const ucn_i_persist_domain_t *domain = &owner->domains[domain_index];
    const size_t marker_offset =
        domain->manifest.slot_capacity_bytes - UCN_PERSIST_COMMIT_MARKER_BYTES;
    if (owner->slot_state[slot_index] != UCN_PERSIST_BLOB_PRESENT) {
        return UCN_ERR_MALFORMED;
    }
    if (ucn_i_persist_bytes_are_value(
            &owner->slot_buffer[slot_index][marker_offset],
            UCN_PERSIST_COMMIT_MARKER_BYTES, owner->provider.erased_value)) {
        /* An erased marker is the only uncommitted representation.  The body
         * may be empty, partial, or complete; none of it is published. */
        return UCN_ERR_NOT_FOUND;
    }
    owner->codec_decode_request.slot = owner->slot_buffer[slot_index];
    owner->codec_decode_request.manifest = &domain->manifest;
    owner->codec_decode_request.durable_manifest_digest =
        owner->durable_manifest_digest;
    owner->codec_decode_request.meta_out = meta_out;
    owner->codec_decode_request.body_out = body_out;
    owner->codec_decode_request.slot_bytes =
        domain->manifest.slot_capacity_bytes;
    owner->codec_decode_request.body_capacity =
        domain->manifest.body_capacity_bytes;
    owner->codec_decode_request.erased_value = owner->provider.erased_value;
    return ucn_i_persist_record_decode(&owner->codec_decode_request,
                                       &owner->codec_workspace);
}

static bool recovered_history_is_valid(
    const ucn_i_persist_record_meta_t *left,
    const ucn_i_persist_record_meta_t *right)
{
    const ucn_i_persist_record_meta_t *older;
    const ucn_i_persist_record_meta_t *newer;
    if (left->record_generation < right->record_generation) {
        older = left;
        newer = right;
    } else {
        older = right;
        newer = left;
    }
    return ucn_i_persist_domain_equal(older->domain, newer->domain) &&
           older->schema_id == newer->schema_id &&
           older->schema_version == newer->schema_version &&
           older->record_generation < UINT64_MAX - 1U &&
           newer->record_generation == older->record_generation + 1U &&
           newer->transaction_id > older->transaction_id;
}

/* Selects only a state proven by the witness. It never falls back below it. */
static ucn_result_t decode_recovered_slot_locked(
    ucn_persistence_owner_t *owner,
    uint8_t domain_index,
    uint8_t slot_index)
{
    ucn_result_t decoded;
    uint8_t *body_out;

    if (slot_index >= UCN_PERSIST_SLOT_COUNT ||
        owner->slot_state[slot_index] != UCN_PERSIST_BLOB_PRESENT) {
        return UCN_ERR_MALFORMED;
    }
    if (slot_index == 0U) {
        memset(owner->recovered_meta, 0, sizeof(owner->recovered_meta));
        memset(owner->recovered_decode, 0, sizeof(owner->recovered_decode));
        body_out = owner->readback_buffer;
    } else {
        body_out = owner->write_buffer;
    }
    decoded = decode_loaded_slot_locked(
        owner, domain_index, slot_index,
        &owner->recovered_meta[slot_index], body_out);
    if (decoded != UCN_OK && decoded != UCN_ERR_NOT_FOUND) {
        return UCN_ERR_MALFORMED;
    }
    owner->recovered_decode[slot_index] = decoded;
    return UCN_OK;
}

static ucn_result_t select_recovered_decoded_locked(
    ucn_persistence_owner_t *owner,
    uint8_t domain_index,
    bool allow_repair,
    bool *repair_out)
{
    ucn_i_persist_record_meta_t *meta = owner->recovered_meta;
    ucn_result_t *decoded = owner->recovered_decode;
    ucn_i_persist_domain_t *domain = &owner->domains[domain_index];
    uint8_t selected = UCN_I_PERSIST_INVALID_INDEX;
    uint8_t repair_selected = UCN_I_PERSIST_INVALID_INDEX;
    uint8_t index;
    uint64_t witness_generation;

    *repair_out = false;
    if (decoded[0] == UCN_OK && decoded[1] == UCN_OK &&
        !recovered_history_is_valid(&meta[0], &meta[1])) {
        return UCN_ERR_MALFORMED;
    }
    if (owner->witness.state == UCN_PERSIST_BLOB_EMPTY) {
        /* Empty media is not, by itself, provisioning evidence.  Factory
         * empty requires a valid, integrity-checked generation-zero witness;
         * otherwise a lost witness could silently reset the generation
         * domain and reopen ABA after a power loss. */
        return UCN_ERR_MALFORMED;
    }
    if (owner->witness.state != UCN_PERSIST_BLOB_PRESENT ||
        !ucn_i_persist_domain_equal(owner->witness.domain,
                                    domain->manifest.domain) ||
        owner->witness.struct_size != sizeof(owner->witness) ||
        owner->witness.api_version != UCN_PERSIST_API_VERSION ||
        !ucn_i_persist_bytes_are_value(owner->witness.reserved_zero,
                                       sizeof(owner->witness.reserved_zero),
                                       0U)) {
        return UCN_ERR_MALFORMED;
    }
    witness_generation = owner->witness.highest_maybe_published_generation;
    if (witness_generation == UINT64_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    for (index = 0U; index < UCN_PERSIST_SLOT_COUNT; ++index) {
        if (decoded[index] == UCN_OK &&
            meta[index].record_generation == witness_generation) {
            selected = index;
        }
        if (decoded[index] == UCN_OK && witness_generation != UINT64_MAX &&
            meta[index].record_generation == witness_generation + 1U) {
            repair_selected = index;
        }
        if (decoded[index] == UCN_OK &&
            meta[index].record_generation > witness_generation + 1U) {
            return UCN_ERR_MALFORMED;
        }
    }
    if (allow_repair && repair_selected != UCN_I_PERSIST_INVALID_INDEX) {
        /* A witness may trail a fully published record by exactly one only
         * after the corresponding predecessor has already been published.
         * Without that predecessor, the successor transaction cannot prove
         * strict monotonicity and must not advance the anti-rollback witness.
         * Generation zero is the sole exception: provisioning plus a
         * generation-one record is the first commit and has no predecessor. */
        if (witness_generation != 0U &&
            (selected == UCN_I_PERSIST_INVALID_INDEX ||
             !recovered_history_is_valid(&meta[selected],
                                         &meta[repair_selected]))) {
            return UCN_ERR_MALFORMED;
        }
        *repair_out = true;
        return UCN_OK;
    }
    if (selected == UCN_I_PERSIST_INVALID_INDEX) {
        if (witness_generation == 0U && decoded[0] == UCN_ERR_NOT_FOUND &&
            decoded[1] == UCN_ERR_NOT_FOUND) {
            memset(domain->body, 0, sizeof(domain->body));
            memset(domain->body_digest, 0, sizeof(domain->body_digest));
            domain->record_generation = 0U;
            domain->current_transaction_id = 0U;
            domain->current_operation_kind = 0U;
            domain->body_bytes = 0U;
            domain->active_slot = UCN_I_PERSIST_INVALID_INDEX;
            domain->state = UCN_PERSIST_DOMAIN_READY;
            return UCN_OK;
        }
        return UCN_ERR_MALFORMED;
    }
    domain->record_generation = meta[selected].record_generation;
    domain->current_transaction_id = meta[selected].transaction_id;
    domain->current_operation_kind = meta[selected].operation_kind;
    domain->body_bytes = meta[selected].body_bytes;
    domain->active_slot = selected;
    memcpy(domain->body_digest, meta[selected].body_digest,
           UCN_PERSIST_DIGEST_BYTES);
    if (domain->body_bytes != 0U) {
        const uint8_t *selected_body =
            selected == 0U ? owner->readback_buffer : owner->write_buffer;
        memcpy(domain->body, selected_body, domain->body_bytes);
    }
    if (domain->body_bytes < sizeof(domain->body)) {
        memset(&domain->body[domain->body_bytes], 0,
               sizeof(domain->body) - domain->body_bytes);
    }
    domain->state = UCN_PERSIST_DOMAIN_READY;
    return UCN_OK;
}

static void finish_recovery_domain_locked(ucn_persistence_owner_t *owner)
{
    ++owner->recovery_cursor;
    if (owner->recovery_cursor >= owner->domain_count) {
        owner->owner_phase = UCN_I_PERSIST_OWNER_READY;
        owner->active_domain = UCN_I_PERSIST_INVALID_INDEX;
        owner->work_phase = UCN_I_PERSIST_WORK_IDLE;
    } else {
        owner->active_domain = owner->recovery_cursor;
        owner->work_phase = UCN_I_PERSIST_RECOVERY_LOAD_WITNESS;
        memset(owner->slot_state, 0, sizeof(owner->slot_state));
    }
}

static ucn_result_t fail_recovery_domain_locked(
    ucn_persistence_owner_t *owner,
    uint8_t domain_index,
    ucn_result_t error,
    bool *progress_out)
{
    const bool required = owner->domains[domain_index].required != 0U;
    fault_domain_locked(owner, domain_index);
    if (required) {
        return error;
    }
    finish_recovery_domain_locked(owner);
    *progress_out = true;
    return UCN_OK;
}

static ucn_i_io_progress_t do_phase_io_locked(ucn_persistence_owner_t *owner,
                                              uint8_t io_phase,
                                              uint8_t slot,
                                              bool *owner_lock_held_out)
{
    return perform_io_locked(owner, io_phase, owner->active_domain, slot,
                             owner_lock_held_out);
}

static ucn_result_t advance_recovery_locked(ucn_persistence_owner_t *owner,
                                            bool *progress_out,
                                            bool *owner_lock_held_out)
{
    ucn_i_io_progress_t io_result;
    uint8_t index = owner->active_domain;
    *progress_out = false;
    switch (owner->work_phase) {
    case UCN_I_PERSIST_RECOVERY_LOAD_WITNESS:
        io_result = do_phase_io_locked(owner, UCN_PERSIST_IO_LOAD_WITNESS, 0U,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            if (owner->completion.blob_state != owner->witness.state) {
                return fail_recovery_domain_locked(
                    owner, index, UCN_ERR_MALFORMED, progress_out);
            }
            owner->work_phase = UCN_I_PERSIST_RECOVERY_LOAD_SLOT0;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_RECOVERY_LOAD_SLOT0:
        io_result = do_phase_io_locked(owner, UCN_PERSIST_IO_LOAD_SLOT, 0U,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            owner->slot_state[0] = owner->completion.blob_state;
            owner->work_phase = UCN_I_PERSIST_RECOVERY_LOAD_SLOT1;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_RECOVERY_LOAD_SLOT1:
        io_result = do_phase_io_locked(owner, UCN_PERSIST_IO_LOAD_SLOT, 1U,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            owner->slot_state[1] = owner->completion.blob_state;
            owner->work_phase = UCN_I_PERSIST_RECOVERY_DECODE_SLOT0;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_RECOVERY_REPAIR_WITNESS:
        io_result = do_phase_io_locked(owner,
                                       UCN_PERSIST_IO_ADVANCE_WITNESS, 0U,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            owner->work_phase = UCN_I_PERSIST_RECOVERY_LOAD_WITNESS;
            memset(owner->slot_state, 0, sizeof(owner->slot_state));
            *progress_out = true;
        }
        break;
    default:
        return fail_recovery_domain_locked(owner, index, UCN_ERR_STATE,
                                           progress_out);
    }
    if (io_result == UCN_I_IO_LOCK_LOST) {
        return UCN_ERR_STATE;
    }
    if (io_result == UCN_I_IO_FAILED) {
        return fail_recovery_domain_locked(owner, index, UCN_ERR_STATE,
                                           progress_out);
    }
    if (io_result == UCN_I_IO_GATE_BUSY) {
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

bool ucn_i_persistence_request_is_well_formed(
    const ucn_persistence_owner_t *owner,
    uint8_t domain_index,
    const ucn_persistence_request_t *request)
{
    const ucn_i_persist_domain_t *domain = &owner->domains[domain_index];
    return request != NULL && request->struct_size == sizeof(*request) &&
           request->api_version == UCN_PERSIST_API_VERSION &&
           request->runtime_instance == owner->runtime_instance &&
           request->caller_owner_instance ==
               domain->business_owner_instance &&
           request->domain_generation == domain->domain_generation &&
           request->foundation_transaction_id != 0U &&
           request->foundation_transaction_id != UINT64_MAX &&
           request->absolute_deadline_us != 0U &&
           request->business_transition_digest != 0U &&
           request->schema_id == domain->manifest.schema_id &&
           request->schema_version == domain->manifest.schema_version &&
           request->operation_kind != 0U && request->reserved_zero == 0U &&
           request->body_bytes <= domain->manifest.body_capacity_bytes &&
           (request->body_bytes == 0U || request->canonical_body != NULL) &&
           request->volatile_continuation.runtime_instance ==
               owner->runtime_instance &&
           request->volatile_continuation.owner_instance ==
               request->caller_owner_instance &&
           request->volatile_continuation.generation != 0U &&
           request->volatile_continuation.object_kind >=
               UCN_OBJECT_KIND_SEND &&
           request->volatile_continuation.object_kind <=
               UCN_OBJECT_KIND_CLUSTER &&
           request->volatile_continuation.reserved_zero == 0U;
}

bool ucn_i_persistence_pending_request_equal(
    const ucn_i_persist_pending_t *pending,
    const ucn_persistence_request_t *request)
{
    return pending->caller_runtime_instance == request->runtime_instance &&
           pending->caller_owner_instance == request->caller_owner_instance &&
           pending->domain_generation == request->domain_generation &&
           pending->transaction_id == request->foundation_transaction_id &&
           pending->expected_record_generation ==
               request->expected_record_generation &&
           pending->absolute_deadline_us == request->absolute_deadline_us &&
           pending->business_transition_digest ==
               request->business_transition_digest &&
           pending->body_bytes == request->body_bytes &&
           pending->operation_kind == request->operation_kind &&
           memcmp(pending->expected_body_digest,
                  request->expected_body_digest,
                  UCN_PERSIST_DIGEST_BYTES) == 0 &&
           memcmp(&pending->volatile_continuation,
                  &request->volatile_continuation,
                  sizeof(request->volatile_continuation)) == 0 &&
           (request->body_bytes == 0U ||
            memcmp(pending->body, request->canonical_body,
                   request->body_bytes) == 0);
}

ucn_result_t ucn_i_persistence_attach_consumer(
    ucn_persistence_owner_t *owner)
{
    ucn_result_t result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->io.call_active != 0U ||
        owner->owner_phase != UCN_I_PERSIST_OWNER_READY ||
        owner->consumer_references == UINT8_MAX) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    ++owner->consumer_references;
    ucn_i_persistence_owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_detach_consumer(
    ucn_persistence_owner_t *owner)
{
    ucn_result_t result = ucn_i_persistence_owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->io.call_active != 0U || owner->consumer_references == 0U) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    --owner->consumer_references;
    ucn_i_persistence_owner_unlock(owner);
    return UCN_OK;
}

static void fill_proof_locked(const ucn_persistence_owner_t *owner,
                              uint8_t domain_index,
                              ucn_i_persist_pending_t *pending)
{
    const ucn_i_persist_domain_t *domain = &owner->domains[domain_index];
    ucn_persistence_proof_t *proof = &pending->proof;
    memset(proof, 0, sizeof(*proof));
    proof->struct_size = sizeof(*proof);
    proof->api_version = UCN_PERSIST_API_VERSION;
    proof->runtime_instance = owner->runtime_instance;
    proof->persistence_owner_instance = owner->owner_instance;
    proof->caller_owner_instance = pending->caller_owner_instance;
    proof->domain_generation = domain->domain_generation;
    proof->operation_kind = pending->operation_kind;
    proof->domain = domain->manifest.domain;
    proof->record_generation = domain->record_generation;
    proof->foundation_transaction_id = pending->transaction_id;
    proof->witness_generation = domain->record_generation;
    proof->body_bytes = domain->body_bytes;
    proof->active_slot = domain->active_slot;
    memcpy(proof->body_digest, domain->body_digest,
           UCN_PERSIST_DIGEST_BYTES);
    pending->proof_ready = 1U;
    pending->terminal = 1U;
    pending->terminal_result = UCN_OK;
}

ucn_result_t ucn_i_persistence_submit(
    ucn_persistence_owner_t *owner,
    const ucn_persistence_request_t *request,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    ucn_result_t result = ucn_i_persistence_owner_lock(owner);
    uint8_t index;
    ucn_i_persist_domain_t *domain;
    ucn_i_persist_record_meta_t *meta;
    uint8_t *digest;
    uint16_t generation;
    if (result != UCN_OK) {
        return result;
    }
    if (owner->io.call_active != 0U ||
        owner->owner_phase != UCN_I_PERSIST_OWNER_READY ||
        request == NULL || handle_out == NULL ||
        callback_gate_require_idle(owner->callback_gate) != UCN_OK ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, request,
                       sizeof(*request)) ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, handle_out,
                       sizeof(*handle_out)) ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES,
                       request->canonical_body, request->body_bytes) ||
        ranges_overlap(request, sizeof(*request), handle_out,
                       sizeof(*handle_out)) ||
        ranges_overlap(request->canonical_body, request->body_bytes,
                       handle_out, sizeof(*handle_out))) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    index = ucn_i_persistence_find_domain_index(owner, request->domain);
    if (index == UCN_I_PERSIST_INVALID_INDEX) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    domain = &owner->domains[index];
    if (domain->state != UCN_PERSIST_DOMAIN_READY ||
        !ucn_i_persistence_request_is_well_formed(owner, index, request)) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_ARGUMENT;
    }
    if (domain->pending.valid != 0U) {
        if (ucn_i_persistence_pending_request_equal(&domain->pending, request)) {
            *handle_out = make_handle(owner, index,
                                      domain->pending.handle_generation);
            ucn_i_persistence_owner_unlock(owner);
            return UCN_OK;
        }
        ucn_i_persistence_owner_unlock(owner);
        return request->foundation_transaction_id ==
                       domain->pending.transaction_id
                   ? UCN_ERR_STATE
                   : UCN_ERR_NO_SPACE;
    }
    if (now_us >= request->absolute_deadline_us) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }

    meta = &owner->codec_workspace.meta;
    digest = owner->codec_workspace.digest;
    memset(meta, 0, sizeof(*meta));
    meta->domain = domain->manifest.domain;
    meta->schema_id = domain->manifest.schema_id;
    meta->schema_version = domain->manifest.schema_version;
    meta->transaction_id = request->foundation_transaction_id;
    meta->operation_kind = request->operation_kind;
    meta->body_bytes = request->body_bytes;
    if (request->foundation_transaction_id == domain->current_transaction_id &&
        domain->record_generation != 0U) {
        meta->record_generation = domain->record_generation;
        if (ucn_i_persist_body_digest(meta, request->canonical_body, digest,
                                      &owner->codec_workspace) !=
                UCN_OK ||
            meta->operation_kind != domain->current_operation_kind ||
            request->body_bytes != domain->body_bytes ||
            memcmp(digest, domain->body_digest,
                   UCN_PERSIST_DIGEST_BYTES) != 0 ||
            (request->body_bytes != 0U &&
             memcmp(request->canonical_body, domain->body,
                    request->body_bytes) != 0)) {
            ucn_i_persistence_owner_unlock(owner);
            return UCN_ERR_STATE;
        }
        generation = domain->next_handle_generation;
        if (generation == 0U || generation == UINT16_MAX) {
            ucn_i_persistence_owner_unlock(owner);
            return UCN_ERR_EXHAUSTED;
        }
        ++domain->next_handle_generation;
        memset(&domain->pending, 0, sizeof(domain->pending));
        domain->pending.valid = 1U;
        domain->pending.handle_generation = generation;
        domain->pending.caller_runtime_instance = request->runtime_instance;
        domain->pending.caller_owner_instance = request->caller_owner_instance;
        domain->pending.domain_generation = request->domain_generation;
        domain->pending.transaction_id = request->foundation_transaction_id;
        domain->pending.expected_record_generation =
            request->expected_record_generation;
        domain->pending.absolute_deadline_us = request->absolute_deadline_us;
        domain->pending.body_bytes = request->body_bytes;
        domain->pending.operation_kind = request->operation_kind;
        domain->pending.business_transition_digest =
            request->business_transition_digest;
        memcpy(domain->pending.expected_body_digest,
               request->expected_body_digest, UCN_PERSIST_DIGEST_BYTES);
        memcpy(domain->pending.next_body_digest, digest,
               UCN_PERSIST_DIGEST_BYTES);
        domain->pending.volatile_continuation =
            request->volatile_continuation;
        if (request->body_bytes != 0U) {
            memcpy(domain->pending.body, request->canonical_body,
                   request->body_bytes);
        }
        fill_proof_locked(owner, index, &domain->pending);
        *handle_out = make_handle(owner, index, generation);
        ucn_i_persistence_owner_unlock(owner);
        return UCN_OK;
    }
    if (request->foundation_transaction_id <= domain->current_transaction_id ||
        request->expected_record_generation != domain->record_generation ||
        memcmp(request->expected_body_digest, domain->body_digest,
               UCN_PERSIST_DIGEST_BYTES) != 0 ||
        domain->record_generation == UINT64_MAX - 1U) {
        ucn_i_persistence_owner_unlock(owner);
        return domain->record_generation == UINT64_MAX - 1U
                   ? UCN_ERR_EXHAUSTED
                   : UCN_ERR_STATE;
    }
    generation = domain->next_handle_generation;
    if (generation == 0U || generation == UINT16_MAX) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    meta->record_generation = domain->record_generation + 1U;
    if (ucn_i_persist_body_digest(meta, request->canonical_body, digest,
                                  &owner->codec_workspace) !=
        UCN_OK) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_ARGUMENT;
    }
    ++domain->next_handle_generation;
    memset(&domain->pending, 0, sizeof(domain->pending));
    domain->pending.valid = 1U;
    domain->pending.handle_generation = generation;
    domain->pending.target_slot =
        domain->active_slot == UCN_I_PERSIST_INVALID_INDEX
            ? 0U
            : (uint8_t)(domain->active_slot ^ 1U);
    domain->pending.caller_runtime_instance = request->runtime_instance;
    domain->pending.caller_owner_instance = request->caller_owner_instance;
    domain->pending.domain_generation = request->domain_generation;
    domain->pending.transaction_id = request->foundation_transaction_id;
    domain->pending.expected_record_generation =
        request->expected_record_generation;
    domain->pending.absolute_deadline_us = request->absolute_deadline_us;
    domain->pending.business_transition_digest =
        request->business_transition_digest;
    domain->pending.body_bytes = request->body_bytes;
    domain->pending.operation_kind = request->operation_kind;
    memcpy(domain->pending.expected_body_digest,
           request->expected_body_digest, UCN_PERSIST_DIGEST_BYTES);
    memcpy(domain->pending.next_body_digest, digest,
           UCN_PERSIST_DIGEST_BYTES);
    domain->pending.volatile_continuation = request->volatile_continuation;
    if (request->body_bytes != 0U) {
        memcpy(domain->pending.body, request->canonical_body,
               request->body_bytes);
    }
    *handle_out = make_handle(owner, index, generation);
    ucn_i_persistence_owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_cancel(ucn_persistence_owner_t *owner,
                                      ucn_handle_t handle)
{
    ucn_result_t result = ucn_i_persistence_owner_lock(owner);
    uint8_t index;
    if (result != UCN_OK) {
        return result;
    }
    if (owner->io.call_active != 0U || !ucn_i_persistence_handle_matches(owner, handle, &index) ||
        owner->domains[index].pending.started != 0U ||
        owner->domains[index].pending.proof_ready != 0U) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->domains[index].pending.terminal = 1U;
    owner->domains[index].pending.terminal_result = UCN_ERR_CANCELLED;
    ucn_i_persistence_owner_unlock(owner);
    return UCN_OK;
}

static uint8_t pick_pending_domain_locked(ucn_persistence_owner_t *owner)
{
    uint8_t offset;
    for (offset = 0U; offset < owner->domain_count; ++offset) {
        const uint8_t index =
            (uint8_t)((owner->work_cursor + offset) % owner->domain_count);
        ucn_i_persist_pending_t *pending = &owner->domains[index].pending;
        if (pending->valid != 0U && pending->terminal == 0U) {
            owner->work_cursor = (uint8_t)((index + 1U) % owner->domain_count);
            return index;
        }
    }
    return UCN_I_PERSIST_INVALID_INDEX;
}

static ucn_result_t prepare_submit_locked(ucn_persistence_owner_t *owner,
                                          uint8_t index)
{
    ucn_i_persist_domain_t *domain = &owner->domains[index];
    ucn_i_persist_pending_t *pending = &domain->pending;
    ucn_i_persist_record_meta_t *meta = &owner->codec_workspace.meta;
    memset(meta, 0, sizeof(*meta));
    meta->domain = domain->manifest.domain;
    meta->record_generation = domain->record_generation + 1U;
    meta->transaction_id = pending->transaction_id;
    meta->body_bytes = pending->body_bytes;
    meta->schema_id = domain->manifest.schema_id;
    meta->schema_version = domain->manifest.schema_version;
    meta->operation_kind = pending->operation_kind;
    if (ucn_i_persist_record_encode(
            meta, owner->durable_manifest_digest, pending->body,
            domain->manifest.body_capacity_bytes,
            domain->manifest.slot_capacity_bytes,
            owner->provider.erased_value, owner->write_buffer,
            &owner->codec_workspace) != UCN_OK ||
        ucn_i_persist_marker_encode(meta->record_generation,
                                    owner->marker_buffer) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    pending->started = 1U;
    owner->active_domain = index;
    owner->work_phase = UCN_I_PERSIST_SUBMIT_WRITE;
    return UCN_OK;
}

static ucn_result_t finalize_submitted_locked(ucn_persistence_owner_t *owner,
                                              uint8_t index)
{
    ucn_i_persist_domain_t *domain = &owner->domains[index];
    ucn_i_persist_pending_t *pending = &domain->pending;
    if (domain->record_generation != pending->expected_record_generation + 1U ||
        domain->current_transaction_id != pending->transaction_id ||
        domain->current_operation_kind != pending->operation_kind ||
        domain->body_bytes != pending->body_bytes ||
        memcmp(domain->body_digest, pending->next_body_digest,
               UCN_PERSIST_DIGEST_BYTES) != 0 ||
        (domain->body_bytes != 0U &&
         memcmp(domain->body, pending->body, domain->body_bytes) != 0) ||
        owner->witness.highest_maybe_published_generation !=
            domain->record_generation) {
        return UCN_ERR_MALFORMED;
    }
    fill_proof_locked(owner, index, pending);
    owner->active_domain = UCN_I_PERSIST_INVALID_INDEX;
    owner->work_phase = UCN_I_PERSIST_WORK_IDLE;
    return UCN_OK;
}

static ucn_result_t advance_submit_locked(ucn_persistence_owner_t *owner,
                                          bool *progress_out,
                                          bool *owner_lock_held_out)
{
    const uint8_t index = owner->active_domain;
    ucn_i_persist_domain_t *domain = &owner->domains[index];
    ucn_i_persist_pending_t *pending = &domain->pending;
    ucn_i_io_progress_t io_result = UCN_I_IO_FAILED;
    const uint8_t slot = pending->target_slot;
    *progress_out = false;
    switch (owner->work_phase) {
    case UCN_I_PERSIST_SUBMIT_WRITE:
        io_result = do_phase_io_locked(owner,
                                       UCN_PERSIST_IO_WRITE_INACTIVE, slot,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            owner->work_phase = UCN_I_PERSIST_SUBMIT_READBACK;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_SUBMIT_READBACK:
        io_result = do_phase_io_locked(owner, UCN_PERSIST_IO_READBACK, slot,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            if (memcmp(owner->write_buffer, owner->readback_buffer,
                       domain->manifest.slot_capacity_bytes) != 0) {
                fault_domain_locked(owner, index);
                return UCN_ERR_MALFORMED;
            }
            owner->work_phase = UCN_I_PERSIST_SUBMIT_MARKER;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_SUBMIT_MARKER:
        io_result = do_phase_io_locked(owner,
                                       UCN_PERSIST_IO_PUBLISH_MARKER, slot,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            owner->work_phase = UCN_I_PERSIST_SUBMIT_WITNESS;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_SUBMIT_WITNESS:
        io_result = do_phase_io_locked(owner,
                                       UCN_PERSIST_IO_ADVANCE_WITNESS, 0U,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            owner->work_phase = UCN_I_PERSIST_SUBMIT_RELOAD_WITNESS;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_SUBMIT_RELOAD_WITNESS:
        io_result = do_phase_io_locked(owner, UCN_PERSIST_IO_LOAD_WITNESS, 0U,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            if (owner->completion.blob_state != owner->witness.state) {
                fault_domain_locked(owner, index);
                return UCN_ERR_MALFORMED;
            }
            owner->work_phase = UCN_I_PERSIST_SUBMIT_RELOAD_SLOT0;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_SUBMIT_RELOAD_SLOT0:
        io_result = do_phase_io_locked(owner, UCN_PERSIST_IO_LOAD_SLOT, 0U,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            owner->slot_state[0] = owner->completion.blob_state;
            owner->work_phase = UCN_I_PERSIST_SUBMIT_RELOAD_SLOT1;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_SUBMIT_RELOAD_SLOT1:
        io_result = do_phase_io_locked(owner, UCN_PERSIST_IO_LOAD_SLOT, 1U,
                                       owner_lock_held_out);
        if (io_result == UCN_I_IO_DONE) {
            owner->slot_state[1] = owner->completion.blob_state;
            owner->work_phase = UCN_I_PERSIST_SUBMIT_DECODE_SLOT0;
            *progress_out = true;
        }
        break;
    case UCN_I_PERSIST_SUBMIT_FINALIZE:
        if (finalize_submitted_locked(owner, index) != UCN_OK) {
            fault_domain_locked(owner, index);
            return UCN_ERR_MALFORMED;
        }
        *progress_out = true;
        return UCN_OK;
    default:
        fault_domain_locked(owner, index);
        return UCN_ERR_STATE;
    }
    if (io_result == UCN_I_IO_LOCK_LOST) {
        return UCN_ERR_STATE;
    }
    if (io_result == UCN_I_IO_FAILED) {
        fault_domain_locked(owner, index);
        return UCN_ERR_STATE;
    }
    if (io_result == UCN_I_IO_GATE_BUSY) {
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

static void summarize_locked(const ucn_persistence_owner_t *owner,
                             ucn_persistence_step_result_t *result)
{
    uint8_t index;
    for (index = 0U; index < owner->domain_count; ++index) {
        if (owner->domains[index].state == UCN_PERSIST_DOMAIN_READY) {
            ++result->domains_ready;
        } else if (owner->domains[index].state ==
                   UCN_PERSIST_DOMAIN_FAULTED) {
            ++result->domains_faulted;
        }
        if (owner->domains[index].pending.proof_ready != 0U) {
            ++result->proofs_ready;
        }
    }
    result->owner_ready =
        (uint8_t)(owner->owner_phase == UCN_I_PERSIST_OWNER_READY);
}

ucn_result_t ucn_i_persistence_step(
    ucn_persistence_owner_t *owner,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_persistence_step_result_t *result_out)
{
    ucn_persistence_step_result_t *result;
    ucn_result_t lock_result;
    ucn_result_t step_result = UCN_OK;
    uint16_t operation;
    if (owner == NULL || result_out == NULL || operation_budget == 0U ||
        operation_budget > UCN_PERSIST_MAX_STEP_OPERATIONS ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, result_out,
                       sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    lock_result = ucn_i_persistence_owner_lock(owner);
    if (lock_result != UCN_OK) {
        return lock_result;
    }
    if (owner->io.call_active != 0U ||
        (owner->owner_phase != UCN_I_PERSIST_OWNER_RECOVERING &&
         owner->owner_phase != UCN_I_PERSIST_OWNER_READY) ||
        callback_gate_require_idle(owner->callback_gate) != UCN_OK) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = &owner->step_result_staging;
    memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->api_version = UCN_PERSIST_API_VERSION;
    for (operation = 0U; operation < operation_budget; ++operation) {
        bool progressed = false;
        bool owner_lock_held = true;
        if (owner->owner_phase == UCN_I_PERSIST_OWNER_RECOVERING) {
            if (owner->work_phase == UCN_I_PERSIST_RECOVERY_DECODE_SLOT0 ||
                owner->work_phase == UCN_I_PERSIST_RECOVERY_DECODE_SLOT1) {
                const uint8_t slot =
                    owner->work_phase == UCN_I_PERSIST_RECOVERY_DECODE_SLOT0
                        ? 0U
                        : 1U;
                const uint8_t index = owner->active_domain;
                step_result = decode_recovered_slot_locked(
                    owner, index, slot);
                if (step_result != UCN_OK) {
                    step_result = fail_recovery_domain_locked(
                        owner, index, UCN_ERR_MALFORMED, &progressed);
                } else {
                    owner->work_phase =
                        slot == 0U
                            ? UCN_I_PERSIST_RECOVERY_DECODE_SLOT1
                            : UCN_I_PERSIST_RECOVERY_SELECT;
                    progressed = true;
                }
            } else if (owner->work_phase == UCN_I_PERSIST_RECOVERY_SELECT) {
                bool repair = false;
                const uint8_t index = owner->active_domain;
                step_result = select_recovered_decoded_locked(
                    owner, index, true, &repair);
                if (step_result != UCN_OK) {
                    step_result = fail_recovery_domain_locked(
                        owner, index, UCN_ERR_MALFORMED, &progressed);
                } else {
                    if (repair) {
                        owner->domains[index].record_generation =
                            owner->witness
                                .highest_maybe_published_generation;
                        owner->work_phase =
                            UCN_I_PERSIST_RECOVERY_REPAIR_WITNESS;
                    } else {
                        finish_recovery_domain_locked(owner);
                    }
                    progressed = true;
                }
            } else {
                step_result = advance_recovery_locked(owner, &progressed,
                                                      &owner_lock_held);
            }
        } else {
            if (owner->active_domain == UCN_I_PERSIST_INVALID_INDEX) {
                const uint8_t index = pick_pending_domain_locked(owner);
                if (index == UCN_I_PERSIST_INVALID_INDEX) {
                    break;
                }
                if (owner->domains[index].pending.started == 0U &&
                    now_us >= owner->domains[index].pending.absolute_deadline_us) {
                    owner->domains[index].pending.terminal = 1U;
                    owner->domains[index].pending.terminal_result =
                        UCN_ERR_TIMEOUT;
                    progressed = true;
                } else {
                    step_result = prepare_submit_locked(owner, index);
                    progressed = step_result == UCN_OK;
                }
            } else if (owner->work_phase == UCN_I_PERSIST_SUBMIT_DECODE_SLOT0 ||
                       owner->work_phase == UCN_I_PERSIST_SUBMIT_DECODE_SLOT1) {
                const uint8_t slot =
                    owner->work_phase == UCN_I_PERSIST_SUBMIT_DECODE_SLOT0
                        ? 0U
                        : 1U;
                const uint8_t index = owner->active_domain;
                step_result = decode_recovered_slot_locked(
                    owner, index, slot);
                if (step_result != UCN_OK) {
                    fault_domain_locked(owner, index);
                    step_result = UCN_ERR_MALFORMED;
                } else {
                    owner->work_phase =
                        slot == 0U
                            ? UCN_I_PERSIST_SUBMIT_DECODE_SLOT1
                            : UCN_I_PERSIST_SUBMIT_SELECT;
                    progressed = true;
                }
            } else if (owner->work_phase == UCN_I_PERSIST_SUBMIT_SELECT) {
                bool repair = false;
                const uint8_t index = owner->active_domain;
                step_result = select_recovered_decoded_locked(
                    owner, index, false, &repair);
                if (step_result != UCN_OK || repair) {
                    fault_domain_locked(owner, index);
                    step_result = UCN_ERR_MALFORMED;
                } else {
                    owner->work_phase = UCN_I_PERSIST_SUBMIT_FINALIZE;
                    progressed = true;
                }
            } else {
                step_result = advance_submit_locked(owner, &progressed,
                                                    &owner_lock_held);
            }
        }
        if (!owner_lock_held) {
            return UCN_ERR_STATE;
        }
        if (step_result != UCN_OK) {
            break;
        }
        if (!progressed) {
            break;
        }
        ++result->operations_performed;
        result->made_progress = 1U;
    }
    summarize_locked(owner, result);
    if (step_result == UCN_OK) {
        *result_out = *result;
    }
    ucn_i_persistence_owner_unlock(owner);
    if (step_result != UCN_OK) {
        return step_result;
    }
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_domain_get(
    const ucn_persistence_owner_t *owner,
    ucn_persist_domain_key_t key,
    ucn_persistence_domain_view_t *view_out)
{
    ucn_persistence_owner_t *mutable_owner = (ucn_persistence_owner_t *)owner;
    ucn_persistence_domain_view_t view;
    ucn_result_t result;
    uint8_t index;
    if (owner == NULL || view_out == NULL ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, view_out,
                       sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_persistence_owner_lock(mutable_owner);
    if (result != UCN_OK) {
        return result;
    }
    if (mutable_owner->io.call_active != 0U ||
        (mutable_owner->owner_phase != UCN_I_PERSIST_OWNER_READY &&
         mutable_owner->owner_phase != UCN_I_PERSIST_OWNER_FAULTED)) {
        ucn_i_persistence_owner_unlock(mutable_owner);
        return UCN_ERR_STATE;
    }
    index = ucn_i_persistence_find_domain_index(owner, key);
    if (index == UCN_I_PERSIST_INVALID_INDEX) {
        ucn_i_persistence_owner_unlock(mutable_owner);
        return UCN_ERR_NOT_FOUND;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    view.api_version = UCN_PERSIST_API_VERSION;
    view.domain = mutable_owner->domains[index].manifest.domain;
    view.record_generation = mutable_owner->domains[index].record_generation;
    view.body_bytes = mutable_owner->domains[index].body_bytes;
    view.schema_id = mutable_owner->domains[index].manifest.schema_id;
    view.schema_version = mutable_owner->domains[index].manifest.schema_version;
    view.domain_generation = mutable_owner->domains[index].domain_generation;
    view.state = mutable_owner->domains[index].state;
    view.active_slot = mutable_owner->domains[index].active_slot;
    view.required = mutable_owner->domains[index].required;
    view.pending = mutable_owner->domains[index].pending.valid;
    memcpy(view.body_digest, mutable_owner->domains[index].body_digest,
           UCN_PERSIST_DIGEST_BYTES);
    ucn_i_persistence_owner_unlock(mutable_owner);
    *view_out = view;
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_domain_copy_body(
    const ucn_persistence_owner_t *owner,
    ucn_persist_domain_key_t key,
    uint8_t *body_out,
    size_t body_capacity,
    size_t *body_bytes_out)
{
    ucn_persistence_owner_t *mutable_owner = (ucn_persistence_owner_t *)owner;
    ucn_result_t result;
    uint8_t index;
    size_t bytes;
    if (owner == NULL || body_bytes_out == NULL ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, body_out,
                       body_capacity) ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, body_bytes_out,
                       sizeof(*body_bytes_out)) ||
        ranges_overlap(body_out, body_capacity, body_bytes_out,
                       sizeof(*body_bytes_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_persistence_owner_lock(mutable_owner);
    if (result != UCN_OK) {
        return result;
    }
    if (mutable_owner->io.call_active != 0U ||
        mutable_owner->owner_phase != UCN_I_PERSIST_OWNER_READY) {
        ucn_i_persistence_owner_unlock(mutable_owner);
        return UCN_ERR_STATE;
    }
    index = ucn_i_persistence_find_domain_index(owner, key);
    if (index == UCN_I_PERSIST_INVALID_INDEX) {
        ucn_i_persistence_owner_unlock(mutable_owner);
        return UCN_ERR_NOT_FOUND;
    }
    if (mutable_owner->domains[index].state != UCN_PERSIST_DOMAIN_READY) {
        ucn_i_persistence_owner_unlock(mutable_owner);
        return UCN_ERR_STATE;
    }
    bytes = mutable_owner->domains[index].body_bytes;
    if (bytes > body_capacity || (bytes != 0U && body_out == NULL)) {
        ucn_i_persistence_owner_unlock(mutable_owner);
        return UCN_ERR_NO_SPACE;
    }
    if (bytes != 0U) {
        memcpy(body_out, mutable_owner->domains[index].body, bytes);
    }
    ucn_i_persistence_owner_unlock(mutable_owner);
    *body_bytes_out = bytes;
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_proof_get(
    const ucn_persistence_owner_t *owner,
    ucn_handle_t handle,
    ucn_persistence_proof_t *proof_out)
{
    ucn_persistence_owner_t *mutable_owner = (ucn_persistence_owner_t *)owner;
    ucn_result_t result;
    uint8_t index;
    ucn_persistence_proof_t proof;
    if (owner == NULL || proof_out == NULL ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, proof_out,
                       sizeof(*proof_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_persistence_owner_lock(mutable_owner);
    if (result != UCN_OK) {
        return result;
    }
    if (mutable_owner->io.call_active != 0U ||
        !ucn_i_persistence_handle_matches(owner, handle, &index) ||
        mutable_owner->domains[index].pending.proof_ready == 0U) {
        ucn_i_persistence_owner_unlock(mutable_owner);
        return UCN_ERR_STATE;
    }
    proof = mutable_owner->domains[index].pending.proof;
    ucn_i_persistence_owner_unlock(mutable_owner);
    *proof_out = proof;
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_request_get(
    const ucn_persistence_owner_t *owner,
    ucn_handle_t handle,
    ucn_persistence_request_view_t *view_out)
{
    ucn_persistence_owner_t *mutable_owner = (ucn_persistence_owner_t *)owner;
    ucn_result_t result;
    ucn_persistence_request_view_t view;
    uint8_t index;
    const ucn_i_persist_pending_t *pending;
    if (owner == NULL || view_out == NULL ||
        ranges_overlap(owner, UCN_PERSIST_STORAGE_BYTES, view_out,
                       sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_persistence_owner_lock(mutable_owner);
    if (result != UCN_OK) {
        return result;
    }
    if (mutable_owner->io.call_active != 0U ||
        !ucn_i_persistence_handle_matches(owner, handle, &index)) {
        ucn_i_persistence_owner_unlock(mutable_owner);
        return UCN_ERR_STATE;
    }
    pending = &mutable_owner->domains[index].pending;
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    view.api_version = UCN_PERSIST_API_VERSION;
    view.terminal_result = pending->terminal_result;
    if (pending->proof_ready != 0U) {
        view.state = UCN_PERSIST_REQUEST_PROOF_READY;
    } else if (pending->terminal != 0U) {
        view.state = UCN_PERSIST_REQUEST_FAILED;
    } else if (pending->started != 0U) {
        view.state = UCN_PERSIST_REQUEST_IN_PROGRESS;
    } else {
        view.state = UCN_PERSIST_REQUEST_QUEUED;
    }
    if (mutable_owner->active_domain == index) {
        view.provider_phase = mutable_owner->work_phase;
    }
    ucn_i_persistence_owner_unlock(mutable_owner);
    *view_out = view;
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_proof_retire(
    ucn_persistence_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_result_t result = ucn_i_persistence_owner_lock(owner);
    uint8_t index;
    if (result != UCN_OK) {
        return result;
    }
    if (owner->io.call_active != 0U || !ucn_i_persistence_handle_matches(owner, handle, &index) ||
        owner->domains[index].pending.proof_ready == 0U) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&owner->domains[index].pending, 0,
           sizeof(owner->domains[index].pending));
    ucn_i_persistence_owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_persistence_request_retire(
    ucn_persistence_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_result_t result = ucn_i_persistence_owner_lock(owner);
    uint8_t index;
    if (result != UCN_OK) {
        return result;
    }
    if (owner->io.call_active != 0U || !ucn_i_persistence_handle_matches(owner, handle, &index) ||
        owner->domains[index].pending.terminal == 0U) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&owner->domains[index].pending, 0,
           sizeof(owner->domains[index].pending));
    ucn_i_persistence_owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_persistence_deinit(ucn_persistence_owner_t *owner)
{
    ucn_result_t result = ucn_i_persistence_owner_lock(owner);
    uint8_t index;
    if (result != UCN_OK) {
        return result;
    }
    if (owner->io.call_active != 0U || owner->io.active != 0U ||
        owner->consumer_references != 0U ||
        owner->active_domain != UCN_I_PERSIST_INVALID_INDEX) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < owner->domain_count; ++index) {
        if (owner->domains[index].pending.valid != 0U) {
            ucn_i_persistence_owner_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    if (callback_gate_detach_owner(owner->callback_gate) != UCN_OK) {
        ucn_i_persistence_owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->magic = 0U;
    owner->schema = 0U;
    ucn_i_persistence_owner_unlock(owner);
    memset(owner, 0, UCN_PERSIST_STORAGE_BYTES);
    return UCN_OK;
}
