#include "fake_persistence_provider.h"
#include "internal/ucn_persistence_coordinator.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            return __LINE__;                                                \
        }                                                                   \
    } while (0)

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

typedef struct sink_state {
    ucn_persistence_owner_t *owner;
    ucn_i_coordinator_t *coordinator;
    ucn_i_persistence_coordinator_adapter_t *adapter;
    const ucn_persistence_request_t *reentrant_request;
    uint32_t calls;
    ucn_result_t forced_result;
    ucn_result_t proof_result;
    ucn_result_t reentrant_result;
    ucn_i_dependency_event_t event;
    ucn_persistence_proof_t proof;
    ucn_handle_t reentrant_handle;
} sink_state_t;

typedef struct fixture {
    ucn_persist_manifest_entry_t entry;
    ucn_persist_domain_binding_t binding;
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake;
    ucn_persistence_provider_t provider;
    ucn_persistence_config_t config;
    test_lock_t owner_lock;
    test_lock_t gate_lock;
    test_lock_t coordinator_lock;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace;
    ucn_persistence_storage_t owner_storage;
    ucn_persist_callback_gate_t *gate;
    ucn_persistence_owner_t *owner;
    ucn_i_coordinator_t coordinator;
    ucn_i_persistence_coordinator_adapter_t adapter;
    sink_state_t sink;
} fixture_t;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    if (lock != NULL) {
        lock->held = 0U;
    }
}

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

static ucn_i_lock_ops_t make_coordinator_lock(test_lock_t *state)
{
    ucn_i_lock_ops_t lock;
    memset(&lock, 0, sizeof(lock));
    lock.struct_size = sizeof(lock);
    lock.api_version = UCN_I_LOCK_OPS_VERSION;
    lock.context = state;
    lock.enter = lock_enter;
    lock.leave = lock_leave;
    return lock;
}

static ucn_result_t event_sink(
    void *context,
    uint16_t requester_owner_instance,
    const ucn_i_dependency_requirement_t *requirement,
    const ucn_i_dependency_event_t *event)
{
    sink_state_t *sink = (sink_state_t *)context;
    if (sink == NULL || requester_owner_instance != 9U ||
        requirement == NULL || event == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++sink->calls;
    sink->event = *event;
    memset(&sink->proof, 0, sizeof(sink->proof));
    sink->proof_result = ucn_i_persistence_proof_get(
        sink->owner, event->dependency_handle, &sink->proof);
    if (sink->reentrant_request != NULL) {
        const ucn_handle_t sentinel = sink->reentrant_handle;
        sink->reentrant_result = ucn_i_persistence_route_request(
            sink->coordinator, sink->adapter, sink->reentrant_request, 101U,
            &sink->reentrant_handle);
        if (memcmp(&sink->reentrant_handle, &sentinel, sizeof(sentinel)) != 0) {
            return UCN_ERR_STATE;
        }
    }
    return sink->forced_result;
}

static int fixture_init(fixture_t *fixture)
{
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    ucn_lock_ops_t owner_lock;
    ucn_lock_ops_t gate_lock;
    ucn_i_lock_ops_t coordinator_lock;
    uint16_t iteration;

    memset(fixture, 0, sizeof(*fixture));
    fixture->entry.struct_size = sizeof(fixture->entry);
    fixture->entry.api_version = UCN_PERSIST_API_VERSION;
    fixture->entry.domain.domain_kind =
        UCN_PERSIST_DOMAIN_IDENTITY_BINDING;
    fixture->entry.domain.domain_id = 1U;
    fixture->entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    fixture->entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    fixture->entry.schema_id = 1U;
    fixture->entry.schema_version = 1U;
    fixture->entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    fixture->entry.witness_policy =
        UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    fixture->entry.provider_atomicity_class =
        UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    fixture->binding.struct_size = sizeof(fixture->binding);
    fixture->binding.api_version = UCN_PERSIST_API_VERSION;
    fixture->binding.domain = fixture->entry.domain;
    fixture->binding.business_owner_instance = 9U;
    fixture->binding.domain_generation = 1U;
    fixture->manifest.struct_size = sizeof(fixture->manifest);
    fixture->manifest.api_version = UCN_PERSIST_API_VERSION;
    fixture->manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    fixture->manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    fixture->manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    fixture->manifest.entries = &fixture->entry;
    fixture->manifest.entry_count = 1U;
    fixture->manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &fixture->manifest, &fixture->digest_workspace, digest) ==
          UCN_OK);
    memcpy(fixture->manifest.expected_digest, digest, sizeof(digest));

    fake_persist_provider_init(&fixture->fake, &fixture->manifest, 0xFFU);
    fake_persist_provider_make_public(&fixture->fake, 0xFFU,
                                      &fixture->provider);
    owner_lock = make_lock(&fixture->owner_lock);
    gate_lock = make_lock(&fixture->gate_lock);
    coordinator_lock = make_coordinator_lock(&fixture->coordinator_lock);
    CHECK(ucn_persist_callback_gate_init_in_place(
              &fixture->gate_storage, sizeof(fixture->gate_storage),
              &gate_lock, &fixture->gate) == UCN_OK);
    fixture->config.struct_size = sizeof(fixture->config);
    fixture->config.api_version = UCN_PERSIST_API_VERSION;
    fixture->config.runtime_instance = 7U;
    fixture->config.owner_instance = 3U;
    fixture->config.required_domain_mask = 1U;
    fixture->config.manifest = &fixture->manifest;
    fixture->config.domain_bindings = &fixture->binding;
    fixture->config.domain_binding_count = 1U;
    fixture->config.provider = &fixture->provider;
    fixture->config.state_lock = owner_lock;
    fixture->config.shared_callback_gate = fixture->gate;
    fixture->config.digest_workspace = &fixture->digest_workspace;
    CHECK(ucn_persistence_init_in_place(
              &fixture->owner_storage, sizeof(fixture->owner_storage),
              &fixture->config, &fixture->owner) == UCN_OK);
    CHECK(ucn_i_persistence_start_recovery(fixture->owner) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture->owner, iteration + 1U, 8U,
                                     &step) == UCN_OK);
        if (step.owner_ready != 0U && step.made_progress == 0U) {
            break;
        }
    }
    CHECK(iteration < 32U);
    fixture->sink.owner = fixture->owner;
    fixture->sink.coordinator = &fixture->coordinator;
    fixture->sink.adapter = &fixture->adapter;
    CHECK(ucn_i_coordinator_init(
              &fixture->coordinator, 7U, 4U, &coordinator_lock,
              ucn_i_requirement_digest_default, event_sink,
              &fixture->sink) == UCN_OK);
    CHECK(ucn_i_persistence_coordinator_adapter_init(
              &fixture->adapter, fixture->owner) == UCN_OK);
    CHECK(ucn_i_persistence_bind_coordinator(
              &fixture->adapter, &fixture->coordinator) == UCN_OK);
    return 0;
}

static ucn_persistence_request_t make_request(
    fixture_t *fixture,
    uint64_t transaction_id,
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
    request.domain = fixture->entry.domain;
    request.foundation_transaction_id = transaction_id;
    request.absolute_deadline_us = 1000U;
    request.business_transition_digest = UINT64_C(0x123456789ABCDEF0);
    request.canonical_body = body;
    request.body_bytes = (uint32_t)body_bytes;
    request.schema_id = 1U;
    request.schema_version = 1U;
    request.operation_kind = 1U;
    request.volatile_continuation.runtime_instance = 7U;
    request.volatile_continuation.owner_instance = 9U;
    request.volatile_continuation.slot = 1U;
    request.volatile_continuation.generation = 1U;
    request.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    return request;
}

static int test_exact_route_proof_and_retire_retry(void)
{
    fixture_t fixture;
    const uint8_t body_a[] = {1U, 2U, 3U};
    const uint8_t body_b[] = {1U, 2U, 4U};
    ucn_persistence_request_t request;
    ucn_persistence_request_t collision;
    ucn_handle_t handle;
    ucn_handle_t duplicate;
    ucn_handle_t sentinel;
    uint16_t iteration;

    CHECK(fixture_init(&fixture) == 0);
    request = make_request(&fixture, 1U, body_a, sizeof(body_a));
    collision = request;
    collision.canonical_body = body_b;
    memset(&sentinel, 0xA5, sizeof(sentinel));
    handle = sentinel;
    CHECK(ucn_i_persistence_route_request(
              &fixture.coordinator, &fixture.adapter, &request, 10U,
              &handle) == UCN_OK);
    duplicate = sentinel;
    CHECK(ucn_i_persistence_route_request(
              &fixture.coordinator, &fixture.adapter, &request, 11U,
              &duplicate) == UCN_OK);
    CHECK(memcmp(&handle, &duplicate, sizeof(handle)) == 0);
    duplicate = sentinel;
    CHECK(ucn_i_persistence_route_request(
              &fixture.coordinator, &fixture.adapter, &collision, 11U,
              &duplicate) == UCN_ERR_STATE);
    CHECK(memcmp(&duplicate, &sentinel, sizeof(duplicate)) == 0);

    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.owner, 20U + iteration, 8U,
                                     &step) == UCN_OK);
        if (step.proofs_ready != 0U) {
            break;
        }
    }
    CHECK(iteration < 32U);
    fixture.sink.forced_result = UCN_ERR_STATE;
    CHECK(ucn_i_persistence_route_terminal(
              &fixture.coordinator, &fixture.adapter, handle, 100U) ==
          UCN_ERR_STATE);
    CHECK(fixture.sink.calls == 1U);
    CHECK(fixture.sink.proof_result == UCN_OK);
    CHECK(fixture.sink.proof.foundation_transaction_id ==
          request.foundation_transaction_id);
    CHECK(fixture.sink.proof.operation_kind == request.operation_kind);
    CHECK(fixture.sink.proof.body_bytes == sizeof(body_a));
    fixture.sink.forced_result = UCN_OK;
    CHECK(ucn_i_persistence_route_terminal(
              &fixture.coordinator, &fixture.adapter, handle, 101U) == UCN_OK);
    CHECK(fixture.sink.calls == 2U);
    CHECK(ucn_i_persistence_proof_get(fixture.owner, handle,
                                      &fixture.sink.proof) == UCN_ERR_STATE);
    CHECK(ucn_i_coordinator_destroy(&fixture.coordinator) == UCN_OK);
    CHECK(ucn_i_persistence_coordinator_adapter_deinit(
              &fixture.adapter, &fixture.coordinator) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
    return 0;
}

static int test_cancel_routes_failure_once(void)
{
    fixture_t fixture;
    const uint8_t body[] = {9U};
    ucn_persistence_request_t request;
    ucn_handle_t handle;

    CHECK(fixture_init(&fixture) == 0);
    request = make_request(&fixture, 1U, body, sizeof(body));
    CHECK(ucn_i_persistence_route_request(
              &fixture.coordinator, &fixture.adapter, &request, 10U,
              &handle) == UCN_OK);
    CHECK(ucn_i_persistence_cancel(fixture.owner, handle) == UCN_OK);
    CHECK(ucn_i_persistence_route_terminal(
              &fixture.coordinator, &fixture.adapter, handle, 20U) == UCN_OK);
    CHECK(fixture.sink.calls == 1U);
    CHECK(fixture.sink.event.outcome == UCN_I_DEPENDENCY_FAILED);
    CHECK(fixture.sink.event.result == UCN_ERR_CANCELLED);
    CHECK(fixture.sink.proof_result == UCN_ERR_STATE);
    CHECK(ucn_i_coordinator_destroy(&fixture.coordinator) == UCN_OK);
    CHECK(ucn_i_persistence_coordinator_adapter_deinit(
              &fixture.adapter, &fixture.coordinator) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
    return 0;
}

static int test_event_callback_reentry_and_route_lifecycle_gate(void)
{
    fixture_t fixture;
    const uint8_t body[] = {5U, 6U};
    ucn_persistence_request_t request;
    ucn_handle_t handle;
    ucn_handle_t sentinel;
    ucn_persistence_storage_t owner_before;
    ucn_i_persistence_coordinator_adapter_t adapter_before;
    uint16_t iteration;

    CHECK(fixture_init(&fixture) == 0);
    request = make_request(&fixture, 1U, body, sizeof(body));
    CHECK(ucn_i_persistence_route_request(
              &fixture.coordinator, &fixture.adapter, &request, 10U,
              &handle) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.owner, 20U + iteration, 8U,
                                     &step) == UCN_OK);
        if (step.proofs_ready != 0U) {
            break;
        }
    }
    CHECK(iteration < 32U);

    memset(&sentinel, 0xA5, sizeof(sentinel));
    fixture.sink.reentrant_request = &request;
    fixture.sink.reentrant_handle = sentinel;
    CHECK(ucn_i_persistence_route_terminal(
              &fixture.coordinator, &fixture.adapter, handle, 100U) == UCN_OK);
    CHECK(fixture.sink.reentrant_result == UCN_ERR_STATE);
    CHECK(memcmp(&fixture.sink.reentrant_handle, &sentinel,
                 sizeof(sentinel)) == 0);

    memcpy(&owner_before, &fixture.owner_storage, sizeof(owner_before));
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_ERR_STATE);
    CHECK(memcmp(&fixture.owner_storage, &owner_before,
                 sizeof(owner_before)) == 0);

    memcpy(&adapter_before, &fixture.adapter, sizeof(adapter_before));
    CHECK(ucn_i_persistence_coordinator_adapter_deinit(
              &fixture.adapter, &fixture.coordinator) == UCN_ERR_STATE);
    CHECK(memcmp(&fixture.adapter, &adapter_before,
                 sizeof(adapter_before)) == 0);
    CHECK(memcmp(&fixture.owner_storage, &owner_before,
                 sizeof(owner_before)) == 0);

    CHECK(ucn_i_coordinator_destroy(&fixture.coordinator) == UCN_OK);
    CHECK(ucn_i_persistence_coordinator_adapter_deinit(
              &fixture.adapter, &fixture.coordinator) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.owner) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
    return 0;
}

int main(void)
{
    int result = test_exact_route_proof_and_retire_retry();
    if (result == 0) {
        result = test_cancel_routes_failure_once();
    }
    if (result == 0) {
        result = test_event_callback_reentry_and_route_lifecycle_gate();
    }
    if (result == 0) {
        puts("persistence coordinator tests passed");
    }
    return result;
}
