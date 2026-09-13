#include "fake_persistence_provider.h"
#include "internal/ucn_persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            abort();                                                        \
        }                                                                   \
    } while (0)

typedef struct test_lock {
    unsigned held;
    unsigned fail_next_enter;
} test_lock_t;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    if (lock->fail_next_enter != 0U) {
        lock->fail_next_enter = 0U;
        return UCN_ERR_STATE;
    }
    if (lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    CHECK(lock->held == 1U);
    lock->held = 0U;
}

typedef struct fixture {
    ucn_persist_manifest_entry_t entries[2];
    ucn_persist_domain_binding_t bindings[2];
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake;
    ucn_persistence_provider_t provider;
    ucn_persistence_config_t config;
    test_lock_t owner_lock_state;
    test_lock_t gate_lock_state;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace;
    ucn_persistence_storage_t owner_storage;
    ucn_persist_callback_gate_t *gate;
    ucn_persistence_owner_t *owner;
} fixture_t;

static ucn_lock_ops_t make_lock(test_lock_t *state)
{
    ucn_lock_ops_t lock;
    memset(&lock, 0, sizeof(lock));
    lock.struct_size = sizeof(lock);
    lock.api_version = UCN_API_VERSION;
    lock.context = state;
    lock.enter = lock_enter;
    lock.leave = lock_leave;
    return lock;
}

static void fixture_build_manifest(fixture_t *fixture)
{
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    uint8_t index;
    memset(fixture, 0, sizeof(*fixture));
    for (index = 0U; index < 2U; ++index) {
        ucn_persist_manifest_entry_t *entry = &fixture->entries[index];
        entry->struct_size = sizeof(*entry);
        entry->api_version = UCN_PERSIST_API_VERSION;
        entry->domain.domain_kind =
            (uint16_t)(UCN_PERSIST_DOMAIN_IDENTITY_BINDING + index);
        entry->domain.domain_id = (uint64_t)index + 1U;
        entry->body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
        entry->slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
        entry->schema_id = (uint16_t)index + 1U;
        entry->schema_version = 1U;
        entry->digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
        entry->witness_policy =
            UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
        entry->provider_atomicity_class =
            UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
        fixture->bindings[index].struct_size =
            sizeof(fixture->bindings[index]);
        fixture->bindings[index].api_version = UCN_PERSIST_API_VERSION;
        fixture->bindings[index].domain = entry->domain;
        fixture->bindings[index].business_owner_instance =
            (uint16_t)(9U + index);
        fixture->bindings[index].domain_generation = 1U;
    }
    fixture->manifest.struct_size = sizeof(fixture->manifest);
    fixture->manifest.api_version = UCN_PERSIST_API_VERSION;
    fixture->manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    fixture->manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    fixture->manifest.composition_feature_bits =
        UCN_COMPILED_FEATURE_MASK;
    fixture->manifest.entries = fixture->entries;
    fixture->manifest.entry_count = 2U;
    fixture->manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &fixture->manifest, &fixture->digest_workspace, digest) ==
          UCN_OK);
    memcpy(fixture->manifest.expected_digest, digest, sizeof(digest));
}

static void fixture_init_runtime(fixture_t *fixture, bool reset_store)
{
    ucn_lock_ops_t owner_lock;
    ucn_lock_ops_t gate_lock;
    if (reset_store) {
        fake_persist_provider_init(&fixture->fake, &fixture->manifest, 0xFFU);
    }
    fake_persist_provider_make_public(&fixture->fake, 0xFFU,
                                      &fixture->provider);
    memset(&fixture->gate_storage, 0, sizeof(fixture->gate_storage));
    memset(&fixture->owner_storage, 0, sizeof(fixture->owner_storage));
    fixture->owner_lock_state.held = 0U;
    fixture->owner_lock_state.fail_next_enter = 0U;
    fixture->gate_lock_state.held = 0U;
    fixture->gate_lock_state.fail_next_enter = 0U;
    owner_lock = make_lock(&fixture->owner_lock_state);
    gate_lock = make_lock(&fixture->gate_lock_state);
    fixture->gate = NULL;
    fixture->owner = NULL;
    CHECK(ucn_persist_callback_gate_init_in_place(
               &fixture->gate_storage, sizeof(fixture->gate_storage),
               &gate_lock, &fixture->gate) == UCN_OK);
    memset(&fixture->config, 0, sizeof(fixture->config));
    fixture->config.struct_size = sizeof(fixture->config);
    fixture->config.api_version = UCN_PERSIST_API_VERSION;
    fixture->config.runtime_instance = 7U;
    fixture->config.owner_instance = 3U;
    fixture->config.required_domain_mask = 1U;
    fixture->config.manifest = &fixture->manifest;
    fixture->config.domain_bindings = fixture->bindings;
    fixture->config.domain_binding_count = 2U;
    fixture->config.provider = &fixture->provider;
    fixture->config.state_lock = owner_lock;
    fixture->config.shared_callback_gate = fixture->gate;
    fixture->config.digest_workspace = &fixture->digest_workspace;
    CHECK(ucn_persistence_init_in_place(
               &fixture->owner_storage, sizeof(fixture->owner_storage),
               &fixture->config, &fixture->owner) == UCN_OK);
}

static void run_until_idle(fixture_t *fixture)
{
    uint16_t iteration;
    for (iteration = 0U; iteration < 64U; ++iteration) {
        ucn_persistence_step_result_t result;
        CHECK(ucn_i_persistence_step(fixture->owner, 10U + iteration, 32U,
                                    &result) == UCN_OK);
        if (result.owner_ready != 0U && result.made_progress == 0U) {
            return;
        }
        if (result.proofs_ready != 0U) {
            return;
        }
    }
    CHECK(!"persistence state machine did not become idle");
}

static ucn_persistence_request_t make_request(
    fixture_t *fixture,
    uint64_t txid,
    uint64_t expected_generation,
    const uint8_t expected_digest[16],
    const uint8_t *body,
    size_t body_bytes)
{
    ucn_persistence_request_t request;
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.api_version = UCN_PERSIST_API_VERSION;
    request.runtime_instance = 7U;
    request.caller_owner_instance = 9U;
    request.domain_generation = 1U;
    request.domain = fixture->entries[0].domain;
    request.foundation_transaction_id = txid;
    request.expected_record_generation = expected_generation;
    request.absolute_deadline_us = 1000U;
    request.business_transition_digest = UINT64_C(0xB17B17B17B17B17);
    request.canonical_body = body;
    request.body_bytes = (uint32_t)body_bytes;
    request.schema_id = fixture->entries[0].schema_id;
    request.schema_version = 1U;
    request.operation_kind = 1U;
    memcpy(request.expected_body_digest, expected_digest, 16U);
    request.volatile_continuation.runtime_instance = 7U;
    request.volatile_continuation.owner_instance = 9U;
    request.volatile_continuation.slot = 1U;
    request.volatile_continuation.generation = 1U;
    request.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    return request;
}

static ucn_handle_t submit_body(fixture_t *fixture,
                                uint64_t txid,
                                uint64_t expected_generation,
                                const uint8_t expected_digest[16],
                                const uint8_t *body,
                                size_t body_bytes)
{
    ucn_persistence_request_t request = make_request(
        fixture, txid, expected_generation, expected_digest, body, body_bytes);
    ucn_handle_t handle;
    memset(&handle, 0xA5, sizeof(handle));
    CHECK(ucn_i_persistence_submit(fixture->owner, &request, 1U, &handle) ==
           UCN_OK);
    return handle;
}

static void encode_store_record(fixture_t *fixture,
                                uint8_t domain_index,
                                uint8_t slot_index,
                                uint64_t generation,
                                uint64_t transaction_id,
                                uint8_t body_value,
                                bool publish_marker)
{
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t workspace;
    uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES];
    memset(&meta, 0, sizeof(meta));
    memset(&workspace, 0, sizeof(workspace));
    meta.domain = fixture->entries[domain_index].domain;
    meta.record_generation = generation;
    meta.transaction_id = transaction_id;
    meta.body_bytes = 1U;
    meta.schema_id = fixture->entries[domain_index].schema_id;
    meta.schema_version = fixture->entries[domain_index].schema_version;
    meta.operation_kind = 1U;
    CHECK(ucn_i_persist_record_encode(
              &meta, fixture->manifest.expected_digest, &body_value,
              fixture->entries[domain_index].body_capacity_bytes,
              fixture->entries[domain_index].slot_capacity_bytes, 0xFFU,
              fixture->fake.domains[domain_index].slots[slot_index],
              &workspace) == UCN_OK);
    if (publish_marker) {
        CHECK(ucn_i_persist_marker_encode(generation, marker) == UCN_OK);
        memcpy(&fixture->fake.domains[domain_index].slots[slot_index][
                   fixture->entries[domain_index].slot_capacity_bytes -
                   sizeof(marker)],
               marker, sizeof(marker));
    }
}

static void test_state_lock_must_be_distinct_from_callback_gate(void)
{
    fixture_t fixture;
    ucn_lock_ops_t shared_lock;
    uint8_t storage_before[UCN_PERSIST_STORAGE_BYTES];
    uint8_t gate_before[UCN_PERSIST_GATE_STORAGE_BYTES];
    ucn_persistence_owner_t *sentinel =
        (ucn_persistence_owner_t *)(uintptr_t)UINT32_C(0x1234);

    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    fake_persist_provider_make_public(&fixture.fake, 0xFFU,
                                      &fixture.provider);
    shared_lock = make_lock(&fixture.owner_lock_state);
    memset(&fixture.gate_storage, 0, sizeof(fixture.gate_storage));
    memset(&fixture.owner_storage, 0, sizeof(fixture.owner_storage));
    memset(storage_before, 0, sizeof(storage_before));
    CHECK(ucn_persist_callback_gate_init_in_place(
               &fixture.gate_storage, sizeof(fixture.gate_storage),
               &shared_lock, &fixture.gate) == UCN_OK);
    memset(&fixture.config, 0, sizeof(fixture.config));
    fixture.config.struct_size = sizeof(fixture.config);
    fixture.config.api_version = UCN_PERSIST_API_VERSION;
    fixture.config.runtime_instance = 7U;
    fixture.config.owner_instance = 3U;
    fixture.config.required_domain_mask = 1U;
    fixture.config.manifest = &fixture.manifest;
    fixture.config.domain_bindings = fixture.bindings;
    fixture.config.domain_binding_count = 2U;
    fixture.config.provider = &fixture.provider;
    fixture.config.state_lock = shared_lock;
    fixture.config.shared_callback_gate = fixture.gate;
    fixture.config.digest_workspace = &fixture.digest_workspace;
    CHECK(ucn_persistence_init_in_place(
               &fixture.owner_storage, sizeof(fixture.owner_storage),
               &fixture.config, &sentinel) == UCN_ERR_CONFIG);
    CHECK(memcmp(&fixture.owner_storage, storage_before,
                  sizeof(storage_before)) == 0);
    CHECK(sentinel ==
           (ucn_persistence_owner_t *)(uintptr_t)UINT32_C(0x1234));

    /* An output pointer inside the live gate must not let init corrupt the
     * gate after all other validation has succeeded. */
    fixture.config.state_lock = make_lock(&fixture.gate_lock_state);
    memcpy(gate_before, &fixture.gate_storage, sizeof(gate_before));
    CHECK(ucn_persistence_init_in_place(
               &fixture.owner_storage, sizeof(fixture.owner_storage),
               &fixture.config,
               (ucn_persistence_owner_t **)&fixture.gate_storage) ==
           UCN_ERR_CONFIG);
    CHECK(memcmp(&fixture.gate_storage, gate_before,
                  sizeof(gate_before)) == 0);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
}

static void test_init_rejects_digest_workspace_storage_alias(void)
{
    fixture_t fixture;
    uint8_t storage_before[UCN_PERSIST_STORAGE_BYTES];
    ucn_persistence_owner_t *sentinel =
        (ucn_persistence_owner_t *)(uintptr_t)UINT32_C(0x5678);

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    memcpy(storage_before, fixture.owner_storage.bytes,
           sizeof(storage_before));
    fixture.config.digest_workspace =
        (ucn_persistence_digest_workspace_t *)&fixture.owner_storage.bytes[8];
    CHECK(ucn_persistence_init_in_place(
              &fixture.owner_storage, sizeof(fixture.owner_storage),
              &fixture.config, &sentinel) == UCN_ERR_CONFIG);
    CHECK(memcmp(fixture.owner_storage.bytes, storage_before,
                 sizeof(storage_before)) == 0);
    CHECK(sentinel ==
          (ucn_persistence_owner_t *)(uintptr_t)UINT32_C(0x5678));
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
}

static void test_gate_lifetime_is_bound_to_owner_references(void)
{
    fixture_t fixture;
    uint8_t gate_before[UCN_PERSIST_GATE_STORAGE_BYTES];

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    memcpy(gate_before, fixture.gate_storage.bytes, sizeof(gate_before));
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_ERR_STATE);
    CHECK(memcmp(gate_before, fixture.gate_storage.bytes,
                 sizeof(gate_before)) == 0);
    CHECK(fixture.gate->owner_references == 1U);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(fixture.gate->owner_references == 0U);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
}

static void test_shared_gate_token_exhaustion_fails_closed(void)
{
    fixture_t fixture;
    static const uint8_t body[] = {0x19U};
    uint8_t empty_digest[UCN_PERSIST_DIGEST_BYTES] = {0};
    uint32_t writes_before;
    ucn_handle_t handle;
    ucn_persistence_step_result_t unchanged;
    ucn_persistence_domain_view_t view;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    handle = submit_body(&fixture, 1U, 0U, empty_digest, body, sizeof(body));
    writes_before = fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE];
    fixture.gate->next_io_token = UINT64_MAX;
    memset(&unchanged, 0xA5, sizeof(unchanged));
    CHECK(ucn_i_persistence_step(fixture.owner, 100U, 2U, &unchanged) ==
          UCN_ERR_STATE);
    CHECK(((const uint8_t *)&unchanged)[0] == 0xA5U);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE] == writes_before);
    CHECK(ucn_i_persistence_domain_get(fixture.owner,
                                       fixture.entries[0].domain,
                                       &view) == UCN_OK);
    CHECK(view.state == UCN_PERSIST_DOMAIN_FAULTED);
    CHECK(ucn_i_persistence_request_retire(fixture.owner, handle) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
}

static void test_provider_geometry_and_optional_poll_contract(void)
{
    fixture_t fixture;
    ucn_persistence_provider_vtable_t synchronous_vtable;
    ucn_persistence_owner_t *sentinel =
        (ucn_persistence_owner_t *)(uintptr_t)UINT32_C(0x1357);
    uint8_t storage_before[UCN_PERSIST_STORAGE_BYTES];
    ucn_persistence_step_result_t step;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);

    memcpy(storage_before, fixture.owner_storage.bytes,
           sizeof(storage_before));
    fixture.owner = sentinel;
    fixture.provider.minimum_write_alignment = 7U;
    CHECK(ucn_persistence_init_in_place(
              &fixture.owner_storage, sizeof(fixture.owner_storage),
              &fixture.config, &fixture.owner) == UCN_ERR_CONFIG);
    CHECK(fixture.owner == sentinel);
    CHECK(memcmp(storage_before, fixture.owner_storage.bytes,
                 sizeof(storage_before)) == 0);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_LOAD_WITNESS] == 0U);

    fixture.provider.minimum_write_alignment = 1U;
    fixture.provider.minimum_erase_alignment = 7U;
    CHECK(ucn_persistence_init_in_place(
              &fixture.owner_storage, sizeof(fixture.owner_storage),
              &fixture.config, &fixture.owner) == UCN_ERR_CONFIG);
    CHECK(fixture.owner == sentinel);
    CHECK(memcmp(storage_before, fixture.owner_storage.bytes,
                 sizeof(storage_before)) == 0);

    fixture.provider.minimum_erase_alignment = 1U;
    synchronous_vtable = *fixture.provider.vtable;
    synchronous_vtable.poll = NULL;
    fixture.provider.vtable = &synchronous_vtable;
    CHECK(ucn_persistence_init_in_place(
              &fixture.owner_storage, sizeof(fixture.owner_storage),
              &fixture.config, &fixture.owner) == UCN_OK);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);

    memset(&fixture.owner_storage, 0, sizeof(fixture.owner_storage));
    fixture.owner = NULL;
    fixture.fake.pending_once_phase = UCN_PERSIST_IO_LOAD_WITNESS;
    CHECK(ucn_persistence_init_in_place(
              &fixture.owner_storage, sizeof(fixture.owner_storage),
              &fixture.config, &fixture.owner) == UCN_OK);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    CHECK(ucn_i_persistence_step(fixture.owner, 1U, 1U, &step) == UCN_OK);
    memset(&step, 0xA5, sizeof(step));
    CHECK(ucn_i_persistence_step(fixture.owner, 2U, 1U, &step) != UCN_OK);
    CHECK(((const uint8_t *)&step)[0] == 0xA5U);
}

static void test_duplicate_conflict_timeout_and_cancel(void)
{
    fixture_t fixture;
    static const uint8_t body[] = {0x21U, 0x22U};
    static const uint8_t conflict_body[] = {0x21U, 0x23U};
    uint8_t empty_digest[16] = {0};
    ucn_persistence_request_t request;
    ucn_persistence_request_t conflict;
    ucn_handle_t handle;
    ucn_handle_t duplicate;
    ucn_persistence_proof_t proof;
    ucn_persistence_proof_t durable_proof;
    ucn_persistence_request_view_t request_view;
    uint32_t calls_before;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);

    request = make_request(&fixture, 1U, 0U, empty_digest,
                           body, sizeof(body));
    duplicate.runtime_instance = UINT32_C(0xA5A5A5A5);
    duplicate.owner_instance = UINT16_C(0xA5A5);
    duplicate.slot = UINT16_C(0xA5A5);
    duplicate.generation = UINT16_C(0xA5A5);
    duplicate.object_kind = UINT8_C(0xA5);
    duplicate.reserved_zero = UINT8_C(0xA5);
    conflict = request;
    conflict.caller_owner_instance++;
    calls_before = fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE];
    CHECK(ucn_i_persistence_submit(fixture.owner, &conflict, 1U,
                                  &duplicate) == UCN_ERR_ARGUMENT);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE] == calls_before);
    CHECK(duplicate.runtime_instance == UINT32_C(0xA5A5A5A5));
    conflict = request;
    conflict.domain_generation++;
    CHECK(ucn_i_persistence_submit(fixture.owner, &conflict, 1U,
                                  &duplicate) == UCN_ERR_ARGUMENT);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE] == calls_before);
    conflict = request;
    conflict.volatile_continuation.object_kind = UINT8_MAX;
    CHECK(ucn_i_persistence_submit(fixture.owner, &conflict, 1U,
                                  &duplicate) == UCN_ERR_ARGUMENT);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE] == calls_before);
    conflict = request;
    conflict.business_transition_digest = 0U;
    CHECK(ucn_i_persistence_submit(fixture.owner, &conflict, 1U,
                                    &duplicate) == UCN_ERR_ARGUMENT);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE] == calls_before);
    CHECK(ucn_i_persistence_submit(fixture.owner, &request, 1U, &handle) == UCN_OK);
    CHECK(ucn_i_persistence_submit(fixture.owner, &request, 1U, &duplicate) ==
           UCN_OK);
    CHECK(memcmp(&handle, &duplicate, sizeof(handle)) == 0);
    conflict = make_request(&fixture, 1U, 0U, empty_digest,
                            conflict_body, sizeof(conflict_body));
    calls_before = fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE];
    CHECK(ucn_i_persistence_submit(fixture.owner, &conflict, 1U, &duplicate) ==
           UCN_ERR_STATE);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE] == calls_before);
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_proof_get(fixture.owner, handle, &proof) == UCN_OK);
    durable_proof = proof;
    CHECK(ucn_i_persistence_proof_retire(fixture.owner, handle) == UCN_OK);

    /* Replay of the current durable identity returns the same durable proof
     * without another Provider write. Deadline, transition reference and
     * continuation are fresh volatile correlation, never proof fields. */
    calls_before = fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE];
    duplicate = handle;
    request.absolute_deadline_us = 10U;
    CHECK(ucn_i_persistence_submit(fixture.owner, &request, 10U, &duplicate) ==
           UCN_ERR_TIMEOUT);
    CHECK(memcmp(&duplicate, &handle, sizeof(handle)) == 0);
    request.absolute_deadline_us = 1000U;
    request.business_transition_digest ^= UINT64_C(0x0101010101010101);
    ++request.volatile_continuation.generation;
    CHECK(ucn_i_persistence_submit(fixture.owner, &request, 10U, &handle) == UCN_OK);
    CHECK(ucn_i_persistence_proof_get(fixture.owner, handle, &proof) == UCN_OK);
    CHECK(memcmp(&proof, &durable_proof, sizeof(proof)) == 0);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE] == calls_before);
    CHECK(ucn_i_persistence_request_retire(fixture.owner, handle) == UCN_OK);

    request = make_request(&fixture, 2U, 1U, proof.body_digest,
                           conflict_body, sizeof(conflict_body));
    request.absolute_deadline_us = 20U;
    CHECK(ucn_i_persistence_submit(fixture.owner, &request, 1U, &handle) == UCN_OK);
    {
        ucn_persistence_step_result_t step_result;
        CHECK(ucn_i_persistence_step(fixture.owner, 20U, 1U,
                                    &step_result) == UCN_OK);
    }
    CHECK(ucn_i_persistence_request_get(fixture.owner, handle,
                                       &request_view) == UCN_OK);
    CHECK(request_view.state == UCN_PERSIST_REQUEST_FAILED);
    CHECK(request_view.terminal_result == UCN_ERR_TIMEOUT);
    CHECK(ucn_i_persistence_request_retire(fixture.owner, handle) == UCN_OK);

    request.absolute_deadline_us = 1000U;
    CHECK(ucn_i_persistence_submit(fixture.owner, &request, 1U, &handle) == UCN_OK);
    CHECK(ucn_i_persistence_cancel(fixture.owner, handle) == UCN_OK);
    CHECK(ucn_i_persistence_request_get(fixture.owner, handle,
                                       &request_view) == UCN_OK);
    CHECK(request_view.terminal_result == UCN_ERR_CANCELLED);
    CHECK(ucn_i_persistence_request_retire(fixture.owner, handle) == UCN_OK);
}

typedef struct reenter_probe {
    ucn_persistence_owner_t *owner;
    ucn_persist_domain_key_t domain;
    ucn_result_t result;
    unsigned calls;
} reenter_probe_t;

static void fail_next_owner_lock_enter(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    lock->fail_next_enter = 1U;
}

static void test_post_provider_lock_failure_is_terminally_fail_closed(void)
{
    fixture_t fixture;
    static const uint8_t body[] = {0x30U};
    uint8_t empty_digest[UCN_PERSIST_DIGEST_BYTES] = {0};
    ucn_handle_t handle;
    ucn_persistence_step_result_t unchanged;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    handle = submit_body(&fixture, 1U, 0U, empty_digest, body, sizeof(body));
    (void)handle;

    fixture.fake.reenter_hook = fail_next_owner_lock_enter;
    fixture.fake.reenter_context = &fixture.owner_lock_state;
    memset(&unchanged, 0xA5, sizeof(unchanged));
    CHECK(ucn_i_persistence_step(fixture.owner, 100U, 2U, &unchanged) ==
          UCN_ERR_STATE);
    CHECK(((const uint8_t *)&unchanged)[0] == 0xA5U);
    CHECK(fixture.owner_lock_state.held == 0U);
    CHECK(fixture.gate->active == 0U);
    CHECK(fixture.owner->io.call_active != 0U);
    CHECK(fixture.fake.calls[UCN_PERSIST_IO_WRITE_INACTIVE] == 1U);
}

static void reenter_domain_get(void *context)
{
    reenter_probe_t *probe = (reenter_probe_t *)context;
    ucn_persistence_domain_view_t view;
    probe->result = ucn_i_persistence_domain_get(probe->owner, probe->domain,
                                                &view);
    ++probe->calls;
}

static void test_reentry_failure_and_fault_scope(void)
{
    fixture_t fixture;
    static const uint8_t body[] = {0x31U};
    uint8_t empty_digest[16] = {0};
    ucn_handle_t handle;
    ucn_persistence_proof_t proof;
    ucn_persistence_request_view_t request_view;
    ucn_persistence_domain_view_t domain_view;
    reenter_probe_t probe;
    ucn_persistence_step_result_t unchanged;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    memset(&probe, 0, sizeof(probe));
    probe.owner = fixture.owner;
    probe.domain = fixture.entries[0].domain;
    fixture.fake.reenter_hook = reenter_domain_get;
    fixture.fake.reenter_context = &probe;
    handle = submit_body(&fixture, 1U, 0U, empty_digest, body, sizeof(body));
    run_until_idle(&fixture);
    CHECK(probe.calls != 0U);
    CHECK(probe.result == UCN_ERR_STATE);
    CHECK(ucn_i_persistence_proof_get(fixture.owner, handle, &proof) == UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(fixture.owner, handle) == UCN_OK);
    fixture.fake.reenter_hook = NULL;

    /* A readback mismatch happens before Marker publication, preserves the
     * old snapshot and returns no forged proof. */
    handle = submit_body(&fixture, 2U, 1U, proof.body_digest,
                         (const uint8_t *)"2", 1U);
    fixture.fake.corrupt_readback_once = 1U;
    memset(&unchanged, 0xA5, sizeof(unchanged));
    CHECK(ucn_i_persistence_step(fixture.owner, 100U, 32U, &unchanged) ==
           UCN_ERR_MALFORMED);
    CHECK(((const uint8_t *)&unchanged)[0] == 0xA5U);
    CHECK(ucn_i_persistence_request_get(fixture.owner, handle,
                                       &request_view) == UCN_OK);
    CHECK(request_view.state == UCN_PERSIST_REQUEST_FAILED);
    CHECK(request_view.terminal_result == UCN_ERR_IN_DOUBT);
    CHECK(ucn_i_persistence_domain_get(fixture.owner, fixture.entries[0].domain,
                                      &domain_view) == UCN_OK);
    CHECK(domain_view.state == UCN_PERSIST_DOMAIN_FAULTED);
    CHECK(domain_view.record_generation == 1U);
    CHECK(ucn_i_persistence_request_retire(fixture.owner, handle) == UCN_OK);
}

static void test_provider_completion_state_and_output_aliases(void)
{
    fixture_t fixture;
    static uint8_t owner_snapshot[UCN_PERSIST_STORAGE_BYTES];
    static const uint8_t body[] = {0x41U, 0x42U};
    uint8_t empty_digest[UCN_PERSIST_DIGEST_BYTES] = {0};
    ucn_handle_t handle;
    ucn_persistence_proof_t proof;
    ucn_persistence_request_view_t request_view;
    ucn_persistence_domain_view_t domain_view;
    ucn_persistence_step_result_t step_result;
    size_t copied_bytes = 0xA5A5U;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    handle = submit_body(&fixture, 1U, 0U, empty_digest, body, sizeof(body));
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_proof_get(fixture.owner, handle, &proof) == UCN_OK);

    memcpy(owner_snapshot, fixture.owner_storage.bytes,
           sizeof(owner_snapshot));
    CHECK(ucn_i_persistence_step(
               fixture.owner, 100U, 1U,
               (ucn_persistence_step_result_t *)fixture.owner_storage.bytes) ==
           UCN_ERR_ARGUMENT);
    CHECK(ucn_i_persistence_domain_get(
               fixture.owner, fixture.entries[0].domain,
               (ucn_persistence_domain_view_t *)fixture.owner_storage.bytes) ==
           UCN_ERR_ARGUMENT);
    CHECK(ucn_i_persistence_domain_copy_body(
               fixture.owner, fixture.entries[0].domain,
               fixture.owner_storage.bytes, 1U, &copied_bytes) ==
           UCN_ERR_ARGUMENT);
    CHECK(copied_bytes == 0xA5A5U);
    CHECK(ucn_i_persistence_domain_copy_body(
               fixture.owner, fixture.entries[0].domain, NULL, 0U,
               (size_t *)fixture.owner_storage.bytes) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_persistence_proof_get(
               fixture.owner, handle,
               (ucn_persistence_proof_t *)fixture.owner_storage.bytes) ==
           UCN_ERR_ARGUMENT);
    CHECK(ucn_i_persistence_request_get(
               fixture.owner, handle,
               (ucn_persistence_request_view_t *)fixture.owner_storage.bytes) ==
           UCN_ERR_ARGUMENT);
    CHECK(memcmp(owner_snapshot, fixture.owner_storage.bytes,
                  sizeof(owner_snapshot)) == 0);

    CHECK(ucn_i_persistence_domain_get(fixture.owner,
                                      fixture.entries[0].domain,
                                      &domain_view) == UCN_OK);
    CHECK(ucn_i_persistence_request_get(fixture.owner, handle,
                                       &request_view) == UCN_OK);
    CHECK(ucn_i_persistence_step(fixture.owner, 100U, 1U,
                                &step_result) == UCN_OK);
    CHECK(domain_view.record_generation == 1U);
    CHECK(request_view.state == UCN_PERSIST_REQUEST_PROOF_READY);
    CHECK(ucn_i_persistence_proof_retire(fixture.owner, handle) == UCN_OK);

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    fixture.fake.invalid_blob_once_phase = UCN_PERSIST_IO_READBACK;
    fixture.fake.invalid_blob_state = UCN_PERSIST_BLOB_EMPTY;
    handle = submit_body(&fixture, 1U, 0U, empty_digest, body, sizeof(body));
    CHECK(ucn_i_persistence_step(fixture.owner, 100U, 32U,
                                &step_result) == UCN_ERR_STATE);
    CHECK(ucn_i_persistence_request_get(fixture.owner, handle,
                                       &request_view) == UCN_OK);
    CHECK(request_view.state == UCN_PERSIST_REQUEST_FAILED);
    CHECK(ucn_i_persistence_domain_get(fixture.owner,
                                      fixture.entries[0].domain,
                                      &domain_view) == UCN_OK);
    CHECK(domain_view.state == UCN_PERSIST_DOMAIN_FAULTED);
    CHECK(domain_view.record_generation == 0U);
}

static void test_every_submit_phase_can_resume_from_pending(void)
{
    uint8_t phase;
    static const uint8_t body[] = {0x51U, 0x52U};
    for (phase = UCN_PERSIST_IO_LOAD_SLOT;
         phase <= UCN_PERSIST_IO_ADVANCE_WITNESS; ++phase) {
        fixture_t fixture;
        uint8_t empty_digest[16] = {0};
        ucn_handle_t handle;
        ucn_persistence_proof_t proof;
        fixture_build_manifest(&fixture);
        fixture_init_runtime(&fixture, true);
        CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
        run_until_idle(&fixture);
        fixture.fake.pending_once_phase = phase;
        handle = submit_body(&fixture, 1U, 0U, empty_digest,
                             body, sizeof(body));
        run_until_idle(&fixture);
        CHECK(fixture.fake.pending.valid == 0U);
        CHECK(ucn_i_persistence_proof_get(fixture.owner, handle, &proof) ==
               UCN_OK);
        CHECK(proof.record_generation == 1U);
        CHECK(ucn_i_persistence_proof_retire(fixture.owner, handle) == UCN_OK);
    }
}

static void test_poll_can_remain_pending_with_exact_continuation(void)
{
    fixture_t fixture;
    ucn_persistence_step_result_t step;
    uint64_t token;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    fixture.fake.pending_once_phase = UCN_PERSIST_IO_LOAD_WITNESS;
    fixture.fake.poll_pending_remaining = 2U;
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    CHECK(ucn_i_persistence_step(fixture.owner, 1U, 1U, &step) == UCN_OK);
    CHECK(fixture.fake.pending.valid != 0U);
    token = fixture.fake.pending.token;
    CHECK(ucn_i_persistence_step(fixture.owner, 2U, 1U, &step) == UCN_OK);
    CHECK(fixture.fake.pending.valid != 0U);
    CHECK(fixture.fake.pending.token == token);
    CHECK(fixture.fake.pending.phase == UCN_PERSIST_IO_LOAD_WITNESS);
    CHECK(ucn_i_persistence_step(fixture.owner, 3U, 1U, &step) == UCN_OK);
    CHECK(fixture.fake.pending.valid != 0U);
    CHECK(fixture.fake.pending.token == token);
    CHECK(ucn_i_persistence_step(fixture.owner, 4U, 1U, &step) == UCN_OK);
    CHECK(fixture.fake.pending.valid == 0U);
    run_until_idle(&fixture);
}

static void test_initial_recovery_phases_resume_exact_pending(void)
{
    uint8_t phase;
    for (phase = UCN_PERSIST_IO_LOAD_SLOT;
         phase <= UCN_PERSIST_IO_LOAD_WITNESS; ++phase) {
        fixture_t fixture;
        ucn_persistence_domain_view_t view;
        if (phase != UCN_PERSIST_IO_LOAD_SLOT &&
            phase != UCN_PERSIST_IO_LOAD_WITNESS) {
            continue;
        }
        fixture_build_manifest(&fixture);
        fixture_init_runtime(&fixture, true);
        fixture.fake.pending_once_phase = phase;
        CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
        run_until_idle(&fixture);
        CHECK(fixture.fake.pending.valid == 0U);
        CHECK(ucn_i_persistence_domain_get(
                   fixture.owner, fixture.entries[0].domain, &view) ==
               UCN_OK);
        CHECK(view.state == UCN_PERSIST_DOMAIN_READY);
        CHECK(view.record_generation == 0U);
    }
}

static void test_submit_failure_power_cut_matrix(void)
{
    uint8_t phase;
    static const uint8_t body[] = {0x55U, 0x66U, 0x77U};
    for (phase = UCN_PERSIST_IO_LOAD_SLOT;
         phase <= UCN_PERSIST_IO_ADVANCE_WITNESS; ++phase) {
        fixture_t fixture;
        uint8_t empty_digest[UCN_PERSIST_DIGEST_BYTES] = {0};
        uint8_t copied[sizeof(body)];
        size_t copied_bytes = UINT32_MAX;
        ucn_handle_t handle;
        ucn_persistence_request_view_t request_view;
        ucn_persistence_domain_view_t view;
        ucn_persistence_step_result_t unchanged;
        const uint64_t expected_generation =
            phase == UCN_PERSIST_IO_ADVANCE_WITNESS ||
                    phase == UCN_PERSIST_IO_LOAD_WITNESS ||
                    phase == UCN_PERSIST_IO_LOAD_SLOT
                ? 1U
                : 0U;

        fixture_build_manifest(&fixture);
        fixture_init_runtime(&fixture, true);
        CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
        run_until_idle(&fixture);
        fixture.fake.fail_once_phase = phase;
        handle = submit_body(&fixture, 1U, 0U, empty_digest,
                             body, sizeof(body));
        memset(&unchanged, 0xA5, sizeof(unchanged));
        CHECK(ucn_i_persistence_step(fixture.owner, 100U, 32U,
                                      &unchanged) != UCN_OK);
        CHECK(((const uint8_t *)&unchanged)[0] == 0xA5U);
        CHECK(ucn_i_persistence_request_get(fixture.owner, handle,
                                             &request_view) == UCN_OK);
        CHECK(request_view.state == UCN_PERSIST_REQUEST_FAILED);
        CHECK(request_view.terminal_result == UCN_ERR_IN_DOUBT);

        /* Power cut discards only volatile Owner/Gate state. The fake store
         * retains exactly the durable bytes written before the failed phase. */
        fixture_init_runtime(&fixture, false);
        CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
        run_until_idle(&fixture);
        CHECK(ucn_i_persistence_domain_get(
                   fixture.owner, fixture.entries[0].domain, &view) ==
               UCN_OK);
        CHECK(view.record_generation == expected_generation);
        if (expected_generation == 0U) {
            CHECK(view.body_bytes == 0U);
        } else {
            CHECK(ucn_i_persistence_domain_copy_body(
                       fixture.owner, fixture.entries[0].domain, copied,
                       sizeof(copied), &copied_bytes) == UCN_OK);
            CHECK(copied_bytes == sizeof(body));
            CHECK(memcmp(copied, body, sizeof(body)) == 0);
        }
    }
}

static void test_malformed_poll_continuation_fails_closed(void)
{
    fixture_t fixture;
    static const uint8_t body[] = {0x91U};
    uint8_t empty_digest[UCN_PERSIST_DIGEST_BYTES] = {0};
    ucn_handle_t handle;
    ucn_persistence_request_view_t request_view;
    ucn_persistence_domain_view_t view;
    ucn_persistence_step_result_t step;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    fixture.fake.pending_once_phase = UCN_PERSIST_IO_WRITE_INACTIVE;
    handle = submit_body(&fixture, 1U, 0U, empty_digest,
                         body, sizeof(body));
    CHECK(ucn_i_persistence_step(fixture.owner, 10U, 2U, &step) == UCN_OK);
    CHECK(fixture.fake.pending.valid != 0U);
    ++fixture.fake.pending.token;
    memset(&step, 0xA5, sizeof(step));
    CHECK(ucn_i_persistence_step(fixture.owner, 11U, 1U, &step) != UCN_OK);
    CHECK(((const uint8_t *)&step)[0] == 0xA5U);
    CHECK(ucn_i_persistence_request_get(fixture.owner, handle,
                                         &request_view) == UCN_OK);
    CHECK(request_view.state == UCN_PERSIST_REQUEST_FAILED);
    CHECK(ucn_i_persistence_domain_get(
               fixture.owner, fixture.entries[0].domain, &view) == UCN_OK);
    CHECK(view.state == UCN_PERSIST_DOMAIN_FAULTED);
    CHECK(view.record_generation == 0U);
}

static void test_optional_domain_fault_is_local(void)
{
    fixture_t fixture;
    ucn_persistence_domain_view_t required_view;
    ucn_persistence_domain_view_t optional_view;
    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    fixture.fake.domains[1].slots[0][fixture.entries[1].slot_capacity_bytes -
                                      UCN_PERSIST_COMMIT_MARKER_BYTES] = 0U;
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_domain_get(fixture.owner, fixture.entries[0].domain,
                                      &required_view) == UCN_OK);
    CHECK(ucn_i_persistence_domain_get(fixture.owner, fixture.entries[1].domain,
                                      &optional_view) == UCN_OK);
    CHECK(required_view.state == UCN_PERSIST_DOMAIN_READY);
    CHECK(optional_view.state == UCN_PERSIST_DOMAIN_FAULTED);
}

static void prepare_committed_store(fixture_t *fixture,
                                    ucn_persistence_proof_t *proof_out)
{
    static const uint8_t body[] = {0x61U, 0x62U};
    uint8_t empty_digest[16] = {0};
    ucn_handle_t handle;
    fixture_build_manifest(fixture);
    fixture_init_runtime(fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture->owner) == UCN_OK);
    run_until_idle(fixture);
    handle = submit_body(fixture, 1U, 0U, empty_digest, body, sizeof(body));
    run_until_idle(fixture);
    CHECK(ucn_i_persistence_proof_get(fixture->owner, handle, proof_out) ==
           UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(fixture->owner, handle) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture->owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture->gate) == UCN_OK);
}

static void assert_recovery_fails_closed(fixture_t *fixture)
{
    ucn_persistence_step_result_t unchanged;
    ucn_persistence_domain_view_t view;
    uint8_t body_sentinel = 0xA5U;
    size_t body_bytes_sentinel = 0xA5A5U;
    fixture_init_runtime(fixture, false);
    CHECK(ucn_i_persistence_start_recovery(fixture->owner) == UCN_OK);
    memset(&unchanged, 0xA5, sizeof(unchanged));
    CHECK(ucn_i_persistence_step(fixture->owner, 1U, 32U, &unchanged) !=
           UCN_OK);
    CHECK(((const uint8_t *)&unchanged)[0] == 0xA5U);
    CHECK(ucn_i_persistence_domain_get(fixture->owner,
                                      fixture->entries[0].domain,
                                      &view) == UCN_OK);
    CHECK(view.state == UCN_PERSIST_DOMAIN_FAULTED);
    CHECK(view.record_generation == 0U);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture->owner, fixture->entries[0].domain, &body_sentinel,
              sizeof(body_sentinel), &body_bytes_sentinel) == UCN_ERR_STATE);
    CHECK(body_sentinel == 0xA5U && body_bytes_sentinel == 0xA5A5U);
}

static void test_anti_rollback_failure_matrix(void)
{
    fixture_t fixture;
    ucn_persistence_proof_t proof;

    prepare_committed_store(&fixture, &proof);
    fixture.fake.domains[0].slots[0][0] ^= 1U;
    assert_recovery_fails_closed(&fixture);

    prepare_committed_store(&fixture, &proof);
    fixture.fake.domains[0].witness_generation = 2U;
    assert_recovery_fails_closed(&fixture);

    /* Two erased slots do not prove factory state when the independent
     * anti-rollback witness is missing. */
    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    fixture.fake.domains[0].witness_state = UCN_PERSIST_BLOB_EMPTY;
    assert_recovery_fails_closed(&fixture);

    prepare_committed_store(&fixture, &proof);
    {
        ucn_i_persist_record_meta_t meta;
        ucn_i_persist_codec_workspace_t workspace;
        static const uint8_t conflict_body[] = {0x71U};
        uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES];
        memset(&meta, 0, sizeof(meta));
        meta.domain = fixture.entries[0].domain;
        meta.record_generation = 1U;
        meta.transaction_id = 2U;
        meta.body_bytes = sizeof(conflict_body);
        meta.schema_id = fixture.entries[0].schema_id;
        meta.schema_version = fixture.entries[0].schema_version;
        meta.operation_kind = 1U;
        CHECK(ucn_i_persist_record_encode(
                   &meta, fixture.manifest.expected_digest, conflict_body,
                   fixture.entries[0].body_capacity_bytes,
                   fixture.entries[0].slot_capacity_bytes, 0xFFU,
                   fixture.fake.domains[0].slots[1], &workspace) == UCN_OK);
        CHECK(ucn_i_persist_marker_encode(1U, marker) == UCN_OK);
        memcpy(&fixture.fake.domains[0].slots[1][
                   fixture.entries[0].slot_capacity_bytes - sizeof(marker)],
               marker, sizeof(marker));
    }
    assert_recovery_fails_closed(&fixture);

    prepare_committed_store(&fixture, &proof);
    memset(fixture.fake.domains[0].slots[1], 0x11,
           fixture.entries[0].slot_capacity_bytes);
    assert_recovery_fails_closed(&fixture);
}

static void test_recovery_prefers_published_witness_successor(void)
{
    fixture_t fixture;
    ucn_persistence_proof_t first_proof;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t workspace;
    static const uint8_t second_body[] = {0x81U, 0x82U, 0x83U};
    uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES];
    uint8_t copied[sizeof(second_body)];
    size_t copied_bytes = 0U;
    ucn_persistence_domain_view_t view;

    prepare_committed_store(&fixture, &first_proof);
    memset(&meta, 0, sizeof(meta));
    meta.domain = fixture.entries[0].domain;
    meta.record_generation = 2U;
    meta.transaction_id = 2U;
    meta.body_bytes = sizeof(second_body);
    meta.schema_id = fixture.entries[0].schema_id;
    meta.schema_version = fixture.entries[0].schema_version;
    meta.operation_kind = 1U;
    CHECK(ucn_i_persist_record_encode(
               &meta, fixture.manifest.expected_digest, second_body,
               fixture.entries[0].body_capacity_bytes,
               fixture.entries[0].slot_capacity_bytes, 0xFFU,
               fixture.fake.domains[0].slots[1], &workspace) == UCN_OK);
    CHECK(ucn_i_persist_marker_encode(2U, marker) == UCN_OK);
    memcpy(&fixture.fake.domains[0].slots[1][
               fixture.entries[0].slot_capacity_bytes - sizeof(marker)],
           marker, sizeof(marker));
    fixture.fake.domains[0].witness_generation = 1U;

    fixture_init_runtime(&fixture, false);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(fixture.fake.domains[0].witness_generation == 2U);
    CHECK(ucn_i_persistence_domain_get(fixture.owner,
                                      fixture.entries[0].domain,
                                      &view) == UCN_OK);
    CHECK(view.record_generation == 2U);
    CHECK(view.active_slot == 1U);
    CHECK(ucn_i_persistence_domain_copy_body(
               fixture.owner, fixture.entries[0].domain, copied,
               sizeof(copied), &copied_bytes) == UCN_OK);
    CHECK(copied_bytes == sizeof(second_body));
    CHECK(memcmp(copied, second_body, sizeof(second_body)) == 0);
}

static void test_witness_repair_requires_predecessor_proof(void)
{
    fixture_t fixture;
    ucn_persistence_domain_view_t view;
    uint8_t body = 0U;
    size_t body_bytes = 0U;

    /* A lone witness+1 record cannot prove that its transaction follows the
     * missing predecessor, so neither the witness nor a body may publish. */
    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 1U, 2U, 1U, 0x21U, true);
    fixture.fake.domains[0].witness_generation = 1U;
    assert_recovery_fails_closed(&fixture);
    CHECK(fixture.fake.domains[0].witness_generation == 1U);

    /* A valid adjacent predecessor proves both generation and transaction
     * monotonicity, so repairing the lagging witness is safe. */
    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 0U, 1U, 10U, 0x11U, true);
    encode_store_record(&fixture, 0U, 1U, 2U, 11U, 0x22U, true);
    fixture.fake.domains[0].witness_generation = 1U;
    fixture_init_runtime(&fixture, false);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(fixture.fake.domains[0].witness_generation == 2U);
    CHECK(ucn_i_persistence_domain_get(fixture.owner,
                                      fixture.entries[0].domain,
                                      &view) == UCN_OK);
    CHECK(view.state == UCN_PERSIST_DOMAIN_READY);
    CHECK(view.record_generation == 2U);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.owner, fixture.entries[0].domain, &body,
              sizeof(body), &body_bytes) == UCN_OK);
    CHECK(body_bytes == 1U && body == 0x22U);

    /* Equal or regressing transaction IDs never prove a witness repair. */
    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 0U, 1U, 10U, 0x11U, true);
    encode_store_record(&fixture, 0U, 1U, 2U, 10U, 0x22U, true);
    fixture.fake.domains[0].witness_generation = 1U;
    assert_recovery_fails_closed(&fixture);
    CHECK(fixture.fake.domains[0].witness_generation == 1U);

    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 0U, 1U, 10U, 0x11U, true);
    encode_store_record(&fixture, 0U, 1U, 2U, 9U, 0x22U, true);
    fixture.fake.domains[0].witness_generation = 1U;
    assert_recovery_fails_closed(&fixture);
    CHECK(fixture.fake.domains[0].witness_generation == 1U);

    /* Provisioned generation zero is the sole predecessor-free case: a
     * generation-one record represents the first committed transaction. */
    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 1U, 1U, 1U, 0x31U, true);
    fixture.fake.domains[0].witness_generation = 0U;
    fixture_init_runtime(&fixture, false);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(fixture.fake.domains[0].witness_generation == 1U);
    CHECK(ucn_i_persistence_domain_get(fixture.owner,
                                      fixture.entries[0].domain,
                                      &view) == UCN_OK);
    CHECK(view.state == UCN_PERSIST_DOMAIN_READY);
    CHECK(view.record_generation == 1U);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.owner, fixture.entries[0].domain, &body,
              sizeof(body), &body_bytes) == UCN_OK);
    CHECK(body_bytes == 1U && body == 0x31U);
}

static void test_recovery_rejects_non_contiguous_history(void)
{
    fixture_t fixture;

    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 0U, 1U, 100U, 0x11U, true);
    encode_store_record(&fixture, 0U, 1U, 2U, 50U, 0x22U, true);
    fixture.fake.domains[0].witness_generation = 2U;
    assert_recovery_fails_closed(&fixture);

    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 0U, 1U, 100U, 0x11U, true);
    encode_store_record(&fixture, 0U, 1U, 2U, 100U, 0x22U, true);
    fixture.fake.domains[0].witness_generation = 2U;
    assert_recovery_fails_closed(&fixture);

    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 0U, 1U, 100U, 0x11U, true);
    encode_store_record(&fixture, 0U, 1U, 1U, 100U, 0x11U, true);
    fixture.fake.domains[0].witness_generation = 1U;
    assert_recovery_fails_closed(&fixture);

    fixture_build_manifest(&fixture);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    encode_store_record(&fixture, 0U, 1U, 1U, 10U, 0x11U, true);
    encode_store_record(&fixture, 0U, 0U, 3U, 30U, 0x33U, true);
    fixture.fake.domains[0].witness_generation = 3U;
    assert_recovery_fails_closed(&fixture);
}

static void test_raw_marker_classifies_uncommitted_slots(void)
{
    fixture_t fixture;
    ucn_persistence_proof_t proof;
    ucn_persistence_domain_view_t view;
    size_t marker_offset;

    prepare_committed_store(&fixture, &proof);
    encode_store_record(&fixture, 0U, 1U, 2U, 2U, 0x22U, false);
    fixture_init_runtime(&fixture, false);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_domain_get(fixture.owner,
                                       fixture.entries[0].domain,
                                       &view) == UCN_OK);
    CHECK(view.record_generation == 1U);
    CHECK(memcmp(view.body_digest, proof.body_digest,
                 UCN_PERSIST_DIGEST_BYTES) == 0);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);

    prepare_committed_store(&fixture, &proof);
    memset(fixture.fake.domains[0].slots[1], 0x3CU,
           fixture.entries[0].slot_capacity_bytes -
               UCN_PERSIST_COMMIT_MARKER_BYTES);
    fixture_init_runtime(&fixture, false);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_domain_get(fixture.owner,
                                       fixture.entries[0].domain,
                                       &view) == UCN_OK);
    CHECK(view.record_generation == 1U);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);

    prepare_committed_store(&fixture, &proof);
    marker_offset = fixture.entries[0].slot_capacity_bytes -
                    UCN_PERSIST_COMMIT_MARKER_BYTES;
    fixture.fake.domains[0].slots[1][marker_offset] = 0U;
    assert_recovery_fails_closed(&fixture);
}

static void test_body_copy_requires_ready_state(void)
{
    fixture_t fixture;
    uint8_t output = 0xA5U;
    size_t output_bytes = 0xA5A5U;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.owner, fixture.entries[0].domain, &output,
              sizeof(output), &output_bytes) == UCN_ERR_STATE);
    CHECK(output == 0xA5U && output_bytes == 0xA5A5U);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.owner, fixture.entries[0].domain, &output,
              sizeof(output), &output_bytes) == UCN_ERR_STATE);
    CHECK(output == 0xA5U && output_bytes == 0xA5A5U);
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.owner, fixture.entries[0].domain, &output,
              sizeof(output), &output_bytes) == UCN_OK);
    CHECK(output == 0xA5U && output_bytes == 0U);

}

static void test_timeout_terminalization_consumes_step_budget(void)
{
    fixture_t fixture;
    ucn_persistence_request_t first;
    ucn_persistence_request_t second;
    ucn_persistence_step_result_t step;
    ucn_persistence_request_view_t first_view;
    ucn_persistence_request_view_t second_view;
    ucn_handle_t first_handle;
    ucn_handle_t second_handle;
    uint8_t empty_digest[UCN_PERSIST_DIGEST_BYTES] = {0};
    static const uint8_t first_body = 0x41U;
    static const uint8_t second_body = 0x42U;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    first = make_request(&fixture, 1U, 0U, empty_digest,
                         &first_body, sizeof(first_body));
    second = first;
    second.caller_owner_instance = fixture.bindings[1].business_owner_instance;
    second.volatile_continuation.owner_instance =
        second.caller_owner_instance;
    second.domain = fixture.entries[1].domain;
    second.schema_id = fixture.entries[1].schema_id;
    second.canonical_body = &second_body;
    first.absolute_deadline_us = 20U;
    second.absolute_deadline_us = 20U;
    CHECK(ucn_i_persistence_submit(fixture.owner, &first, 1U,
                                   &first_handle) == UCN_OK);
    CHECK(ucn_i_persistence_submit(fixture.owner, &second, 1U,
                                   &second_handle) == UCN_OK);

    CHECK(ucn_i_persistence_step(fixture.owner, 20U, 1U, &step) == UCN_OK);
    CHECK(step.operations_performed == 1U && step.made_progress == 1U);
    CHECK(ucn_i_persistence_request_get(fixture.owner, first_handle,
                                        &first_view) == UCN_OK);
    CHECK(ucn_i_persistence_request_get(fixture.owner, second_handle,
                                        &second_view) == UCN_OK);
    CHECK(first_view.state == UCN_PERSIST_REQUEST_FAILED);
    CHECK(first_view.terminal_result == UCN_ERR_TIMEOUT);
    CHECK(second_view.state == UCN_PERSIST_REQUEST_QUEUED);

    CHECK(ucn_i_persistence_step(fixture.owner, 20U, 1U, &step) == UCN_OK);
    CHECK(step.operations_performed == 1U && step.made_progress == 1U);
    CHECK(ucn_i_persistence_request_get(fixture.owner, second_handle,
                                        &second_view) == UCN_OK);
    CHECK(second_view.state == UCN_PERSIST_REQUEST_FAILED);
    CHECK(second_view.terminal_result == UCN_ERR_TIMEOUT);
}

static void test_sync_commit_and_reboot(void)
{
    fixture_t fixture;
    static const uint8_t first[] = {0x10U, 0x20U, 0x30U};
    uint8_t empty_digest[16] = {0};
    uint8_t copied[8];
    size_t copied_bytes = 0U;
    ucn_handle_t handle;
    ucn_persistence_proof_t proof;
    ucn_persistence_domain_view_t view;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_domain_get(fixture.owner, fixture.entries[0].domain,
                                      &view) == UCN_OK);
    CHECK(view.state == UCN_PERSIST_DOMAIN_READY);
    CHECK(view.record_generation == 0U);

    handle = submit_body(&fixture, 1U, 0U, empty_digest,
                         first, sizeof(first));
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_proof_get(fixture.owner, handle, &proof) == UCN_OK);
    CHECK(proof.record_generation == 1U);
    CHECK(proof.witness_generation == 1U);
    CHECK(proof.foundation_transaction_id == 1U);
    CHECK(ucn_i_persistence_domain_copy_body(
               fixture.owner, fixture.entries[0].domain, copied,
               sizeof(copied), &copied_bytes) == UCN_OK);
    CHECK(copied_bytes == sizeof(first));
    CHECK(memcmp(copied, first, sizeof(first)) == 0);
    CHECK(ucn_i_persistence_proof_retire(fixture.owner, handle) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);

    fixture_init_runtime(&fixture, false);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_domain_get(fixture.owner, fixture.entries[0].domain,
                                      &view) == UCN_OK);
    CHECK(view.record_generation == 1U);
    CHECK(view.body_bytes == sizeof(first));
    CHECK(memcmp(view.body_digest, proof.body_digest, 16U) == 0);
}

static void test_pending_and_witness_repair(void)
{
    fixture_t fixture;
    static const uint8_t body[] = {0x44U};
    uint8_t empty_digest[16] = {0};
    ucn_handle_t handle;
    ucn_persistence_proof_t proof;
    ucn_persistence_domain_view_t view;

    fixture_build_manifest(&fixture);
    fixture_init_runtime(&fixture, true);
    fixture.fake.pending_once_phase = UCN_PERSIST_IO_LOAD_WITNESS;
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    fixture.fake.pending_once_phase = UCN_PERSIST_IO_WRITE_INACTIVE;
    handle = submit_body(&fixture, 1U, 0U, empty_digest, body, sizeof(body));
    run_until_idle(&fixture);
    CHECK(ucn_i_persistence_proof_get(fixture.owner, handle, &proof) == UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(fixture.owner, handle) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);

    fixture.fake.domains[0].witness_state = UCN_PERSIST_BLOB_PRESENT;
    fixture.fake.domains[0].witness_generation = 0U;
    fixture_init_runtime(&fixture, false);
    CHECK(ucn_i_persistence_start_recovery(fixture.owner) == UCN_OK);
    run_until_idle(&fixture);
    CHECK(fixture.fake.domains[0].witness_generation == 1U);
    CHECK(ucn_i_persistence_domain_get(fixture.owner, fixture.entries[0].domain,
                                      &view) == UCN_OK);
    CHECK(view.record_generation == 1U);
}

int main(void)
{
    test_state_lock_must_be_distinct_from_callback_gate();
    test_init_rejects_digest_workspace_storage_alias();
    test_gate_lifetime_is_bound_to_owner_references();
    test_shared_gate_token_exhaustion_fails_closed();
    test_provider_geometry_and_optional_poll_contract();
    test_sync_commit_and_reboot();
    test_pending_and_witness_repair();
    test_duplicate_conflict_timeout_and_cancel();
    test_post_provider_lock_failure_is_terminally_fail_closed();
    test_reentry_failure_and_fault_scope();
    test_provider_completion_state_and_output_aliases();
    test_every_submit_phase_can_resume_from_pending();
    test_poll_can_remain_pending_with_exact_continuation();
    test_initial_recovery_phases_resume_exact_pending();
    test_submit_failure_power_cut_matrix();
    test_malformed_poll_continuation_fails_closed();
    test_optional_domain_fault_is_local();
    test_anti_rollback_failure_matrix();
    test_recovery_prefers_published_witness_successor();
    test_witness_repair_requires_predecessor_proof();
    test_recovery_rejects_non_contiguous_history();
    test_raw_marker_classifies_uncommitted_slots();
    test_body_copy_requires_ready_state();
    test_timeout_terminalization_consumes_step_budget();
    return 0;
}
