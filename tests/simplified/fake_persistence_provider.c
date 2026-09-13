#include "fake_persistence_provider.h"

#include <string.h>

static fake_persist_domain_store_t *find_domain(fake_persist_provider_t *provider,
                                                 ucn_persist_domain_key_t key)
{
    uint8_t index;
    for (index = 0U; index < provider->domain_count; ++index) {
        if (provider->domains[index].key.domain_kind == key.domain_kind &&
            provider->domains[index].key.domain_id == key.domain_id) {
            return &provider->domains[index];
        }
    }
    return NULL;
}

static void completion(ucn_persist_io_completion_t *output,
                       uint64_t token,
                       uint8_t phase,
                       uint8_t slot,
                       uint32_t exact_bytes,
                       uint8_t state)
{
    memset(output, 0, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->api_version = UCN_PERSIST_API_VERSION;
    output->io_token = token;
    output->result = UCN_OK;
    output->exact_bytes = exact_bytes;
    output->phase = phase;
    output->blob_state = state;
    output->slot_index = slot;
}

static void maybe_override_blob_state(
    fake_persist_provider_t *provider,
    uint8_t phase,
    ucn_persist_io_completion_t *output)
{
    if (provider->invalid_blob_once_phase == phase) {
        provider->invalid_blob_once_phase = 0U;
        output->blob_state = provider->invalid_blob_state;
    }
}

static bool should_defer(fake_persist_provider_t *provider,
                         uint8_t phase,
                         fake_persist_pending_io_t *pending)
{
    if (provider->pending_once_phase != phase) {
        return false;
    }
    provider->pending_once_phase = 0U;
    provider->pending = *pending;
    provider->pending.valid = 1U;
    return true;
}

static bool should_fail(fake_persist_provider_t *provider, uint8_t phase)
{
    if (provider->fail_once_phase != phase) {
        return false;
    }
    provider->fail_once_phase = 0U;
    return true;
}

static void call_reenter(fake_persist_provider_t *provider)
{
    if (provider->reenter_hook != NULL) {
        provider->reenter_hook(provider->reenter_context);
    }
}

static ucn_persist_io_start_t begin_load_slot(
    void *context,
    ucn_persist_domain_key_t domain,
    uint8_t slot_index,
    uint8_t *output_buffer,
    size_t exact_bytes,
    uint64_t token,
    ucn_persist_io_completion_t *completion_out)
{
    fake_persist_provider_t *provider = (fake_persist_provider_t *)context;
    fake_persist_domain_store_t *store = find_domain(provider, domain);
    fake_persist_pending_io_t pending;
    ++provider->calls[UCN_PERSIST_IO_LOAD_SLOT];
    call_reenter(provider);
    if (store == NULL || slot_index >= UCN_PERSIST_SLOT_COUNT ||
        exact_bytes > UCN_PERSIST_SLOT_BYTES ||
        should_fail(provider, UCN_PERSIST_IO_LOAD_SLOT)) {
        return UCN_PERSIST_IO_FAILED;
    }
    memset(&pending, 0, sizeof(pending));
    pending.phase = UCN_PERSIST_IO_LOAD_SLOT;
    pending.slot = slot_index;
    pending.token = token;
    pending.domain = domain;
    pending.output_buffer = output_buffer;
    pending.exact_bytes = exact_bytes;
    if (should_defer(provider, pending.phase, &pending)) {
        return UCN_PERSIST_IO_PENDING;
    }
    memcpy(output_buffer, store->slots[slot_index], exact_bytes);
    completion(completion_out, token, pending.phase, slot_index,
               (uint32_t)exact_bytes, UCN_PERSIST_BLOB_PRESENT);
    maybe_override_blob_state(provider, pending.phase, completion_out);
    return UCN_PERSIST_IO_COMPLETED;
}

static ucn_persist_io_start_t begin_write_inactive(
    void *context,
    ucn_persist_domain_key_t domain,
    uint8_t slot_index,
    const uint8_t *input_buffer,
    size_t exact_bytes,
    uint64_t token,
    ucn_persist_io_completion_t *completion_out)
{
    fake_persist_provider_t *provider = (fake_persist_provider_t *)context;
    fake_persist_domain_store_t *store = find_domain(provider, domain);
    fake_persist_pending_io_t pending;
    ++provider->calls[UCN_PERSIST_IO_WRITE_INACTIVE];
    call_reenter(provider);
    if (store == NULL || slot_index >= UCN_PERSIST_SLOT_COUNT ||
        exact_bytes > UCN_PERSIST_SLOT_BYTES ||
        should_fail(provider, UCN_PERSIST_IO_WRITE_INACTIVE)) {
        return UCN_PERSIST_IO_FAILED;
    }
    memset(&pending, 0, sizeof(pending));
    pending.phase = UCN_PERSIST_IO_WRITE_INACTIVE;
    pending.slot = slot_index;
    pending.token = token;
    pending.domain = domain;
    pending.input_buffer = input_buffer;
    pending.exact_bytes = exact_bytes;
    if (should_defer(provider, pending.phase, &pending)) {
        return UCN_PERSIST_IO_PENDING;
    }
    memcpy(store->slots[slot_index], input_buffer, exact_bytes);
    completion(completion_out, token, pending.phase, slot_index,
               (uint32_t)exact_bytes, UCN_PERSIST_BLOB_PRESENT);
    maybe_override_blob_state(provider, pending.phase, completion_out);
    return UCN_PERSIST_IO_COMPLETED;
}

static ucn_persist_io_start_t begin_readback(
    void *context,
    ucn_persist_domain_key_t domain,
    uint8_t slot_index,
    uint8_t *output_buffer,
    size_t exact_bytes,
    uint64_t token,
    ucn_persist_io_completion_t *completion_out)
{
    fake_persist_provider_t *provider = (fake_persist_provider_t *)context;
    fake_persist_domain_store_t *store = find_domain(provider, domain);
    fake_persist_pending_io_t pending;
    ++provider->calls[UCN_PERSIST_IO_READBACK];
    call_reenter(provider);
    if (store == NULL || slot_index >= UCN_PERSIST_SLOT_COUNT ||
        exact_bytes > UCN_PERSIST_SLOT_BYTES ||
        should_fail(provider, UCN_PERSIST_IO_READBACK)) {
        return UCN_PERSIST_IO_FAILED;
    }
    memset(&pending, 0, sizeof(pending));
    pending.phase = UCN_PERSIST_IO_READBACK;
    pending.slot = slot_index;
    pending.token = token;
    pending.domain = domain;
    pending.output_buffer = output_buffer;
    pending.exact_bytes = exact_bytes;
    if (should_defer(provider, pending.phase, &pending)) {
        return UCN_PERSIST_IO_PENDING;
    }
    memcpy(output_buffer, store->slots[slot_index], exact_bytes);
    if (provider->corrupt_readback_once != 0U && exact_bytes != 0U) {
        provider->corrupt_readback_once = 0U;
        output_buffer[0] ^= 1U;
    }
    completion(completion_out, token, pending.phase, slot_index,
               (uint32_t)exact_bytes, UCN_PERSIST_BLOB_PRESENT);
    maybe_override_blob_state(provider, pending.phase, completion_out);
    return UCN_PERSIST_IO_COMPLETED;
}

static ucn_persist_io_start_t begin_publish_marker(
    void *context,
    ucn_persist_domain_key_t domain,
    uint8_t slot_index,
    const uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES],
    uint64_t token,
    ucn_persist_io_completion_t *completion_out)
{
    fake_persist_provider_t *provider = (fake_persist_provider_t *)context;
    fake_persist_domain_store_t *store = find_domain(provider, domain);
    fake_persist_pending_io_t pending;
    ++provider->calls[UCN_PERSIST_IO_PUBLISH_MARKER];
    call_reenter(provider);
    if (store == NULL || slot_index >= UCN_PERSIST_SLOT_COUNT ||
        should_fail(provider, UCN_PERSIST_IO_PUBLISH_MARKER)) {
        return UCN_PERSIST_IO_FAILED;
    }
    memset(&pending, 0, sizeof(pending));
    pending.phase = UCN_PERSIST_IO_PUBLISH_MARKER;
    pending.slot = slot_index;
    pending.token = token;
    pending.domain = domain;
    pending.input_buffer = marker;
    pending.exact_bytes = UCN_PERSIST_COMMIT_MARKER_BYTES;
    if (should_defer(provider, pending.phase, &pending)) {
        return UCN_PERSIST_IO_PENDING;
    }
    memcpy(&store->slots[slot_index][store->slot_bytes -
                                     UCN_PERSIST_COMMIT_MARKER_BYTES],
           marker, UCN_PERSIST_COMMIT_MARKER_BYTES);
    completion(completion_out, token, pending.phase, slot_index,
               UCN_PERSIST_COMMIT_MARKER_BYTES, UCN_PERSIST_BLOB_PRESENT);
    maybe_override_blob_state(provider, pending.phase, completion_out);
    return UCN_PERSIST_IO_COMPLETED;
}

static void fill_witness(fake_persist_domain_store_t *store,
                         ucn_persist_witness_view_t *output)
{
    memset(output, 0, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->api_version = UCN_PERSIST_API_VERSION;
    output->domain = store->key;
    output->highest_maybe_published_generation = store->witness_generation;
    output->state = store->witness_state;
}

static ucn_persist_io_start_t begin_load_witness(
    void *context,
    ucn_persist_domain_key_t domain,
    ucn_persist_witness_view_t *witness_out,
    uint64_t token,
    ucn_persist_io_completion_t *completion_out)
{
    fake_persist_provider_t *provider = (fake_persist_provider_t *)context;
    fake_persist_domain_store_t *store = find_domain(provider, domain);
    fake_persist_pending_io_t pending;
    ++provider->calls[UCN_PERSIST_IO_LOAD_WITNESS];
    call_reenter(provider);
    if (store == NULL || should_fail(provider, UCN_PERSIST_IO_LOAD_WITNESS)) {
        return UCN_PERSIST_IO_FAILED;
    }
    memset(&pending, 0, sizeof(pending));
    pending.phase = UCN_PERSIST_IO_LOAD_WITNESS;
    pending.token = token;
    pending.domain = domain;
    pending.witness_output = witness_out;
    pending.exact_bytes = sizeof(*witness_out);
    if (should_defer(provider, pending.phase, &pending)) {
        return UCN_PERSIST_IO_PENDING;
    }
    fill_witness(store, witness_out);
    completion(completion_out, token, pending.phase, 0U,
               (uint32_t)sizeof(*witness_out), store->witness_state);
    maybe_override_blob_state(provider, pending.phase, completion_out);
    return UCN_PERSIST_IO_COMPLETED;
}

static ucn_persist_io_start_t begin_advance_witness(
    void *context,
    ucn_persist_domain_key_t domain,
    uint64_t expected_old,
    uint64_t exact_new,
    uint64_t token,
    ucn_persist_io_completion_t *completion_out)
{
    fake_persist_provider_t *provider = (fake_persist_provider_t *)context;
    fake_persist_domain_store_t *store = find_domain(provider, domain);
    fake_persist_pending_io_t pending;
    ++provider->calls[UCN_PERSIST_IO_ADVANCE_WITNESS];
    call_reenter(provider);
    if (store == NULL || should_fail(provider,
                                     UCN_PERSIST_IO_ADVANCE_WITNESS)) {
        return UCN_PERSIST_IO_FAILED;
    }
    memset(&pending, 0, sizeof(pending));
    pending.phase = UCN_PERSIST_IO_ADVANCE_WITNESS;
    pending.token = token;
    pending.domain = domain;
    pending.expected_old = expected_old;
    pending.exact_new = exact_new;
    pending.exact_bytes = sizeof(uint64_t);
    if (should_defer(provider, pending.phase, &pending)) {
        return UCN_PERSIST_IO_PENDING;
    }
    if (store->witness_state == UCN_PERSIST_BLOB_EMPTY && expected_old == 0U) {
        store->witness_generation = 0U;
        store->witness_state = UCN_PERSIST_BLOB_PRESENT;
    }
    if (store->witness_state != UCN_PERSIST_BLOB_PRESENT ||
        store->witness_generation != expected_old ||
        exact_new != expected_old + 1U) {
        return UCN_PERSIST_IO_FAILED;
    }
    store->witness_generation = exact_new;
    completion(completion_out, token, pending.phase, 0U,
               (uint32_t)sizeof(uint64_t), UCN_PERSIST_BLOB_PRESENT);
    maybe_override_blob_state(provider, pending.phase, completion_out);
    return UCN_PERSIST_IO_COMPLETED;
}

static ucn_persist_io_start_t poll_io(
    void *context,
    uint64_t token,
    ucn_persist_io_phase_t expected_phase,
    ucn_persist_io_completion_t *completion_out)
{
    fake_persist_provider_t *provider = (fake_persist_provider_t *)context;
    fake_persist_pending_io_t pending = provider->pending;
    fake_persist_domain_store_t *store;
    if (pending.valid == 0U || pending.token != token ||
        pending.phase != expected_phase) {
        return UCN_PERSIST_IO_FAILED;
    }
    if (provider->poll_pending_remaining != 0U) {
        --provider->poll_pending_remaining;
        return UCN_PERSIST_IO_PENDING;
    }
    provider->pending.valid = 0U;
    store = find_domain(provider, pending.domain);
    if (store == NULL) {
        return UCN_PERSIST_IO_FAILED;
    }
    switch (pending.phase) {
    case UCN_PERSIST_IO_LOAD_SLOT:
        memcpy(pending.output_buffer, store->slots[pending.slot],
               pending.exact_bytes);
        completion(completion_out, token, pending.phase, pending.slot,
                   (uint32_t)pending.exact_bytes,
                   UCN_PERSIST_BLOB_PRESENT);
        break;
    case UCN_PERSIST_IO_WRITE_INACTIVE:
        memcpy(store->slots[pending.slot], pending.input_buffer,
               pending.exact_bytes);
        completion(completion_out, token, pending.phase, pending.slot,
                   (uint32_t)pending.exact_bytes,
                   UCN_PERSIST_BLOB_PRESENT);
        break;
    case UCN_PERSIST_IO_READBACK:
        memcpy(pending.output_buffer, store->slots[pending.slot],
               pending.exact_bytes);
        completion(completion_out, token, pending.phase, pending.slot,
                   (uint32_t)pending.exact_bytes,
                   UCN_PERSIST_BLOB_PRESENT);
        break;
    case UCN_PERSIST_IO_PUBLISH_MARKER:
        memcpy(&store->slots[pending.slot][store->slot_bytes -
                                           UCN_PERSIST_COMMIT_MARKER_BYTES],
               pending.input_buffer, UCN_PERSIST_COMMIT_MARKER_BYTES);
        completion(completion_out, token, pending.phase, pending.slot,
                   UCN_PERSIST_COMMIT_MARKER_BYTES,
                   UCN_PERSIST_BLOB_PRESENT);
        break;
    case UCN_PERSIST_IO_LOAD_WITNESS:
        fill_witness(store, pending.witness_output);
        completion(completion_out, token, pending.phase, 0U,
                   (uint32_t)sizeof(*pending.witness_output),
                   store->witness_state);
        break;
    case UCN_PERSIST_IO_ADVANCE_WITNESS:
        if (store->witness_state == UCN_PERSIST_BLOB_EMPTY &&
            pending.expected_old == 0U) {
            store->witness_generation = 0U;
            store->witness_state = UCN_PERSIST_BLOB_PRESENT;
        }
        if (store->witness_state != UCN_PERSIST_BLOB_PRESENT ||
            store->witness_generation != pending.expected_old ||
            pending.exact_new != pending.expected_old + 1U) {
            return UCN_PERSIST_IO_FAILED;
        }
        store->witness_generation = pending.exact_new;
        completion(completion_out, token, pending.phase, 0U,
                   (uint32_t)sizeof(uint64_t), UCN_PERSIST_BLOB_PRESENT);
        break;
    default:
        return UCN_PERSIST_IO_FAILED;
    }
    maybe_override_blob_state(provider, pending.phase, completion_out);
    return UCN_PERSIST_IO_COMPLETED;
}

static const ucn_persistence_provider_vtable_t fake_vtable = {
    sizeof(ucn_persistence_provider_vtable_t),
    UCN_PERSIST_API_VERSION,
    begin_load_slot,
    begin_write_inactive,
    begin_readback,
    begin_publish_marker,
    begin_load_witness,
    begin_advance_witness,
    poll_io};

void fake_persist_provider_init(fake_persist_provider_t *provider,
                                const ucn_persist_manifest_t *manifest,
                                uint8_t erased_value)
{
    uint16_t index;
    memset(provider, 0, sizeof(*provider));
    provider->domain_count = (uint8_t)manifest->entry_count;
    for (index = 0U; index < manifest->entry_count; ++index) {
        fake_persist_domain_store_t *domain = &provider->domains[index];
        domain->key = manifest->entries[index].domain;
        domain->slot_bytes = manifest->entries[index].slot_capacity_bytes;
        memset(domain->slots, erased_value, sizeof(domain->slots));
        /* A valid generation-zero witness is the explicit provisioning proof
         * for an empty domain. It is distinct from a missing/corrupt witness. */
        domain->witness_state = UCN_PERSIST_BLOB_PRESENT;
    }
}

void fake_persist_provider_make_public(
    fake_persist_provider_t *provider,
    uint8_t erased_value,
    ucn_persistence_provider_t *public_provider_out)
{
    memset(public_provider_out, 0, sizeof(*public_provider_out));
    public_provider_out->struct_size = sizeof(*public_provider_out);
    public_provider_out->api_version = UCN_PERSIST_API_VERSION;
    public_provider_out->context = provider;
    public_provider_out->vtable = &fake_vtable;
    public_provider_out->minimum_write_alignment = 1U;
    public_provider_out->minimum_erase_alignment = 1U;
    public_provider_out->maximum_slot_bytes = UCN_PERSIST_SLOT_BYTES;
    public_provider_out->atomic_marker_bytes =
        UCN_PERSIST_COMMIT_MARKER_BYTES;
    public_provider_out->erased_value = erased_value;
}

const ucn_persistence_provider_vtable_t *fake_persist_provider_vtable(void)
{
    return &fake_vtable;
}
