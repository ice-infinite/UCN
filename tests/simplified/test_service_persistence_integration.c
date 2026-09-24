#include "fake_persistence_provider.h"
#include "internal/ucn_persistence.h"
#include "internal/ucn_service.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression_)                                                     \
    do {                                                                       \
        if (!(expression_)) {                                                  \
            fprintf(stderr, "check failed at line %d: %s\n", __LINE__,        \
                    #expression_);                                             \
            return __LINE__;                                                   \
        }                                                                      \
    } while (0)

typedef struct test_lock { uint8_t held; } test_lock_t;

typedef struct fixture {
    ucn_persist_manifest_entry_t entries[2];
    ucn_persist_domain_binding_t bindings[2];
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake;
    ucn_persistence_provider_t provider;
    ucn_persistence_config_t persistence_config;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace;
    ucn_persistence_storage_t persistence_storage;
    ucn_persist_callback_gate_t *gate;
    ucn_persistence_owner_t *persistence;
    ucn_i_service_owner_t service;
    test_lock_t persistence_lock;
    test_lock_t gate_lock;
    test_lock_t service_lock;
} fixture_t;

static fixture_t fixture;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    ((test_lock_t *)context)->held = 0U;
}

static ucn_lock_ops_t public_lock(test_lock_t *state)
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

static ucn_i_lock_ops_t internal_lock(test_lock_t *state)
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

static int service_init(void)
{
    ucn_i_service_config_t config;
    memset(&fixture.service, 0, sizeof(fixture.service));
    memset(&fixture.service_lock, 0, sizeof(fixture.service_lock));
    memset(&config, 0, sizeof(config));
    config.receipt_lifetime_us = 1000U;
    config.runtime_instance = 51U;
    config.owner_instance = 52U;
    config.state_lock = internal_lock(&fixture.service_lock);
    return ucn_i_service_owner_init(&fixture.service, &config) == UCN_OK ?
               0 : 1;
}

static int fixture_init(void)
{
    uint8_t manifest_digest[16];
    ucn_lock_ops_t owner_lock;
    ucn_lock_ops_t gate_lock;
    uint16_t iteration;

    memset(&fixture, 0, sizeof(fixture));
    fixture.entries[0].struct_size = sizeof(fixture.entries[0]);
    fixture.entries[0].api_version = UCN_PERSIST_API_VERSION;
    fixture.entries[0].domain.domain_kind = UCN_PERSIST_DOMAIN_OPERATION_JOURNAL;
    fixture.entries[0].domain.domain_id = 700U;
    fixture.entries[0].body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    fixture.entries[0].slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    fixture.entries[0].schema_id = 0x0701U;
    fixture.entries[0].schema_version = UCN_I_SERVICE_OPERATION_SCHEMA;
    fixture.entries[0].digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    fixture.entries[0].witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    fixture.entries[0].provider_atomicity_class =
        UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    fixture.entries[1] = fixture.entries[0];
    fixture.entries[1].domain.domain_id = 701U;
    fixture.entries[1].schema_id = 0x0702U;
    fixture.bindings[0].struct_size = sizeof(fixture.bindings[0]);
    fixture.bindings[0].api_version = UCN_PERSIST_API_VERSION;
    fixture.bindings[0].domain = fixture.entries[0].domain;
    fixture.bindings[0].business_owner_instance = 52U;
    fixture.bindings[0].domain_generation = 4U;
    fixture.bindings[1] = fixture.bindings[0];
    fixture.bindings[1].domain = fixture.entries[1].domain;
    fixture.bindings[1].domain_generation = 5U;
    fixture.manifest.struct_size = sizeof(fixture.manifest);
    fixture.manifest.api_version = UCN_PERSIST_API_VERSION;
    fixture.manifest.protocol_manifest_version = UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    fixture.manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    fixture.manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    fixture.manifest.entries = fixture.entries;
    fixture.manifest.entry_count = 2U;
    fixture.manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(&fixture.manifest,
                                          &fixture.digest_workspace,
                                          manifest_digest) == UCN_OK);
    memcpy(fixture.manifest.expected_digest, manifest_digest, 16U);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    fake_persist_provider_make_public(&fixture.fake, 0xFFU, &fixture.provider);
    owner_lock = public_lock(&fixture.persistence_lock);
    gate_lock = public_lock(&fixture.gate_lock);
    CHECK(ucn_persist_callback_gate_init_in_place(
              &fixture.gate_storage, sizeof(fixture.gate_storage),
              &gate_lock, &fixture.gate) == UCN_OK);
    fixture.persistence_config.struct_size = sizeof(fixture.persistence_config);
    fixture.persistence_config.api_version = UCN_PERSIST_API_VERSION;
    fixture.persistence_config.runtime_instance = 51U;
    fixture.persistence_config.owner_instance = 60U;
    fixture.persistence_config.required_domain_mask = 3U;
    fixture.persistence_config.manifest = &fixture.manifest;
    fixture.persistence_config.domain_bindings = fixture.bindings;
    fixture.persistence_config.domain_binding_count = 2U;
    fixture.persistence_config.provider = &fixture.provider;
    fixture.persistence_config.state_lock = owner_lock;
    fixture.persistence_config.shared_callback_gate = fixture.gate;
    fixture.persistence_config.digest_workspace = &fixture.digest_workspace;
    CHECK(ucn_persistence_init_in_place(
              &fixture.persistence_storage, sizeof(fixture.persistence_storage),
              &fixture.persistence_config, &fixture.persistence) == UCN_OK);
    CHECK(ucn_i_persistence_start_recovery(fixture.persistence) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence, iteration + 1U,
                                     8U, &step) == UCN_OK);
        if (step.owner_ready != 0U && step.made_progress == 0U) break;
    }
    CHECK(iteration < 32U);
    return service_init();
}

static void fill_principal(uint8_t principal[16], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        principal[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_service_operation_key_t operation_key(void)
{
    ucn_i_service_operation_key_t key;
    memset(&key, 0, sizeof(key));
    key.service.client.address = 10U;
    key.service.client.generation = 11U;
    fill_principal(key.service.client.principal, 0x10U);
    key.service.server.address = 20U;
    key.service.server.generation = 21U;
    fill_principal(key.service.server.principal, 0x30U);
    key.service.security.session_generation = 30U;
    key.service.security.key_generation = 31U;
    key.service.security.policy_generation = 32U;
    key.service.security.origin_security = 1U;
    key.service.security.acl_authorized = 1U;
    key.service.operation_id = 1000U;
    key.service.realm = 40U;
    key.service.service_id = 41U;
    key.service.opcode = 42U;
    memset(key.request_digest, 0xA5, sizeof(key.request_digest));
    return key;
}

static ucn_i_service_operation_durability_t durability(uint64_t transaction,
                                                        uint64_t generation)
{
    ucn_i_service_operation_durability_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = fixture.entries[0].domain.domain_id;
    value.foundation_transaction_id = transaction;
    value.expected_record_generation = generation;
    value.absolute_deadline_us = 100000U;
    value.volatile_continuation.runtime_instance = 51U;
    value.volatile_continuation.owner_instance = 52U;
    value.volatile_continuation.slot = 1U;
    value.volatile_continuation.generation = (uint16_t)transaction;
    value.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    value.domain_generation = fixture.bindings[0].domain_generation;
    value.schema_id = fixture.entries[0].schema_id;
    value.schema_version = fixture.entries[0].schema_version;
    return value;
}

static void request_from_requirement(
    const ucn_i_service_operation_requirement_t *requirement,
    ucn_persistence_request_t *request)
{
    uint64_t transition = 0U;
    size_t index;
    memset(request, 0, sizeof(*request));
    for (index = 0U; index < 8U; ++index) {
        transition = (transition << 8U) |
                     requirement->canonical_body_digest[index];
    }
    request->struct_size = sizeof(*request);
    request->api_version = UCN_PERSIST_API_VERSION;
    request->runtime_instance = requirement->runtime_instance;
    request->caller_owner_instance = requirement->caller_owner_instance;
    request->domain_generation = requirement->durability.domain_generation;
    request->domain = fixture.entries[0].domain;
    request->foundation_transaction_id =
        requirement->durability.foundation_transaction_id;
    request->expected_record_generation =
        requirement->durability.expected_record_generation;
    request->absolute_deadline_us = requirement->durability.absolute_deadline_us;
    request->business_transition_digest = transition;
    request->canonical_body = requirement->body;
    request->body_bytes = requirement->body_bytes;
    request->schema_id = requirement->durability.schema_id;
    request->schema_version = requirement->durability.schema_version;
    request->operation_kind = requirement->operation_kind;
    memcpy(request->expected_body_digest, requirement->expected_body_digest,
           sizeof(request->expected_body_digest));
    request->volatile_continuation =
        requirement->durability.volatile_continuation;
}

static int commit_operation(ucn_handle_t operation, uint64_t now_us,
                            ucn_persistence_proof_t *foundation_out)
{
    ucn_i_service_operation_requirement_t requirement;
    ucn_i_service_operation_proof_t proof;
    ucn_persistence_request_t request;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_handle_t persistence_handle;
    uint8_t published_digest[16];
    uint16_t iteration;

    CHECK(ucn_i_service_operation_requirement_get(
              &fixture.service, operation, &requirement) == UCN_OK);
    request_from_requirement(&requirement, &request);
    memset(&meta, 0, sizeof(meta));
    memset(&codec, 0, sizeof(codec));
    meta.domain = request.domain;
    meta.record_generation = request.expected_record_generation + 1U;
    meta.transaction_id = request.foundation_transaction_id;
    meta.body_bytes = request.body_bytes;
    meta.schema_id = request.schema_id;
    meta.schema_version = request.schema_version;
    meta.operation_kind = request.operation_kind;
    CHECK(ucn_i_persist_body_digest(&meta, request.canonical_body,
                                    published_digest, &codec) == UCN_OK);
    CHECK(ucn_i_persistence_submit(fixture.persistence, &request, now_us,
                                   &persistence_handle) == UCN_OK);
    CHECK(ucn_i_service_operation_bind_persistence(
              &fixture.service, operation, persistence_handle,
              published_digest) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence,
                                     now_us + 1U + iteration, 8U,
                                     &step) == UCN_OK);
        if (step.proofs_ready != 0U) break;
    }
    CHECK(iteration < 32U);
    CHECK(ucn_i_persistence_proof_get(fixture.persistence,
                                      persistence_handle,
                                      foundation_out) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.continuation = requirement.continuation;
    proof.persistence_handle = persistence_handle;
    proof.domain_id = foundation_out->domain.domain_id;
    proof.foundation_transaction_id = foundation_out->foundation_transaction_id;
    proof.record_generation = foundation_out->record_generation;
    proof.witness_generation = foundation_out->witness_generation;
    proof.runtime_instance = foundation_out->runtime_instance;
    proof.body_bytes = foundation_out->body_bytes;
    proof.domain_generation = foundation_out->domain_generation;
    proof.persistence_owner_instance = foundation_out->persistence_owner_instance;
    proof.caller_owner_instance = foundation_out->caller_owner_instance;
    proof.schema_id = request.schema_id;
    proof.schema_version = request.schema_version;
    proof.operation_kind = foundation_out->operation_kind;
    memcpy(proof.body_digest, foundation_out->body_digest,
           sizeof(proof.body_digest));
    proof.next_phase = requirement.next_phase;
    CHECK(ucn_i_service_operation_accept_proof(
              &fixture.service, operation, &proof) == UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(fixture.persistence,
                                         persistence_handle) == UCN_OK);
    return 0;
}

static ucn_i_service_operation_durability_t id_durability(
    uint64_t transaction, uint64_t generation)
{
    ucn_i_service_operation_durability_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = fixture.entries[1].domain.domain_id;
    value.foundation_transaction_id = transaction;
    value.expected_record_generation = generation;
    value.absolute_deadline_us = 100000U;
    value.volatile_continuation.runtime_instance = 51U;
    value.volatile_continuation.owner_instance = 52U;
    value.volatile_continuation.slot = 2U;
    value.volatile_continuation.generation = (uint16_t)transaction;
    value.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    value.domain_generation = fixture.bindings[1].domain_generation;
    value.schema_id = fixture.entries[1].schema_id;
    value.schema_version = fixture.entries[1].schema_version;
    return value;
}

static void id_request_from_requirement(
    const ucn_i_service_operation_id_requirement_t *requirement,
    ucn_persistence_request_t *request)
{
    uint64_t transition = 0U;
    size_t index;
    memset(request, 0, sizeof(*request));
    for (index = 0U; index < 8U; ++index) {
        transition = (transition << 8U) |
                     requirement->canonical_body_digest[index];
    }
    request->struct_size = sizeof(*request);
    request->api_version = UCN_PERSIST_API_VERSION;
    request->runtime_instance = requirement->runtime_instance;
    request->caller_owner_instance = requirement->caller_owner_instance;
    request->domain_generation = requirement->durability.domain_generation;
    request->domain.domain_kind = UCN_PERSIST_DOMAIN_OPERATION_JOURNAL;
    request->domain.domain_id = requirement->durability.domain_id;
    request->foundation_transaction_id =
        requirement->durability.foundation_transaction_id;
    request->expected_record_generation =
        requirement->durability.expected_record_generation;
    request->absolute_deadline_us = requirement->durability.absolute_deadline_us;
    request->business_transition_digest = transition;
    request->canonical_body = requirement->body;
    request->body_bytes = UCN_I_SERVICE_OPERATION_ID_BODY_BYTES;
    request->schema_id = requirement->durability.schema_id;
    request->schema_version = requirement->durability.schema_version;
    request->operation_kind = requirement->operation_kind;
    memcpy(request->expected_body_digest, requirement->expected_body_digest,
           sizeof(request->expected_body_digest));
    request->volatile_continuation =
        requirement->durability.volatile_continuation;
}

static int commit_id_interval(
    const ucn_i_service_operation_id_requirement_t *requirement,
    uint64_t now_us, ucn_persistence_proof_t *foundation_out)
{
    ucn_i_service_operation_id_proof_t proof;
    ucn_persistence_request_t request;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_handle_t persistence_handle;
    uint8_t published_digest[16];
    uint16_t iteration;

    id_request_from_requirement(requirement, &request);
    memset(&meta, 0, sizeof(meta));
    memset(&codec, 0, sizeof(codec));
    meta.domain = request.domain;
    meta.record_generation = request.expected_record_generation + 1U;
    meta.transaction_id = request.foundation_transaction_id;
    meta.body_bytes = request.body_bytes;
    meta.schema_id = request.schema_id;
    meta.schema_version = request.schema_version;
    meta.operation_kind = request.operation_kind;
    CHECK(ucn_i_persist_body_digest(&meta, request.canonical_body,
                                    published_digest, &codec) == UCN_OK);
    CHECK(ucn_i_persistence_submit(fixture.persistence, &request, now_us,
                                   &persistence_handle) == UCN_OK);
    CHECK(ucn_i_service_operation_id_bind_persistence(
              &fixture.service, persistence_handle,
              published_digest) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence,
                                     now_us + 1U + iteration, 8U,
                                     &step) == UCN_OK);
        if (step.proofs_ready != 0U) break;
    }
    CHECK(iteration < 32U);
    CHECK(ucn_i_persistence_proof_get(fixture.persistence,
                                      persistence_handle,
                                      foundation_out) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence_handle;
    proof.domain_id = foundation_out->domain.domain_id;
    proof.foundation_transaction_id = foundation_out->foundation_transaction_id;
    proof.record_generation = foundation_out->record_generation;
    proof.witness_generation = foundation_out->witness_generation;
    proof.runtime_instance = foundation_out->runtime_instance;
    proof.body_bytes = foundation_out->body_bytes;
    proof.domain_generation = foundation_out->domain_generation;
    proof.persistence_owner_instance = foundation_out->persistence_owner_instance;
    proof.caller_owner_instance = foundation_out->caller_owner_instance;
    proof.schema_id = request.schema_id;
    proof.schema_version = request.schema_version;
    proof.operation_kind = foundation_out->operation_kind;
    memcpy(proof.body_digest, foundation_out->body_digest,
           sizeof(proof.body_digest));
    CHECK(ucn_i_service_operation_id_accept_proof(
              &fixture.service, &proof) == UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(fixture.persistence,
                                         persistence_handle) == UCN_OK);
    return 0;
}

int main(void)
{
    ucn_i_service_operation_key_t key = operation_key();
    ucn_i_service_operation_durability_t durable;
    ucn_i_service_operation_view_t view;
    ucn_persistence_domain_view_t domain_view;
    ucn_persistence_domain_view_t id_domain_view;
    ucn_persistence_proof_t foundation;
    ucn_i_service_operation_id_requirement_t id_requirement;
    ucn_i_service_operation_id_view_t id_view;
    ucn_handle_t operation;
    ucn_handle_t imported;
    uint8_t body[UCN_I_SERVICE_OPERATION_BODY_BYTES];
    uint8_t id_body[UCN_I_SERVICE_OPERATION_ID_BODY_BYTES];
    size_t body_bytes = 0U;
    size_t id_body_bytes = 0U;
    uint64_t operation_id;

    CHECK(fixture_init() == 0);
    durable = durability(1U, 0U);
    CHECK(ucn_i_service_operation_begin(&fixture.service, &key, &durable,
                                         &operation) == UCN_OK);
    CHECK(commit_operation(operation, 100U, &foundation) == 0);
    durable = durability(2U, 1U);
    CHECK(ucn_i_service_operation_prepare_executing(
              &fixture.service, operation, &durable) == UCN_OK);
    CHECK(commit_operation(operation, 200U, &foundation) == 0);
    CHECK(ucn_i_persistence_domain_get(fixture.persistence,
                                       fixture.entries[0].domain,
                                       &domain_view) == UCN_OK);
    CHECK(domain_view.record_generation == 2U);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.persistence, fixture.entries[0].domain, body,
              sizeof(body),
              &body_bytes) == UCN_OK);
    CHECK(body_bytes >= 110U && body_bytes <= sizeof(body));

    operation_id = UINT64_C(0xA5A5A5A5A5A5A5A5);
    CHECK(ucn_i_service_operation_id_take(&fixture.service,
                                           &operation_id) == UCN_ERR_STATE);
    CHECK(operation_id == UINT64_C(0xA5A5A5A5A5A5A5A5));
    durable = id_durability(1U, 0U);
    CHECK(ucn_i_service_operation_id_prepare_interval(
              &fixture.service, 9U, &durable, &id_requirement) == UCN_OK);
    CHECK(commit_id_interval(&id_requirement, 220U, &foundation) == 0);
    CHECK(ucn_i_service_operation_id_take(&fixture.service,
                                           &operation_id) == UCN_OK);
    CHECK(operation_id == 1U);
    CHECK(ucn_i_persistence_domain_get(fixture.persistence,
                                       fixture.entries[1].domain,
                                       &id_domain_view) == UCN_OK);
    CHECK(id_domain_view.record_generation == 1U);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.persistence, fixture.entries[1].domain, id_body,
              sizeof(id_body), &id_body_bytes) == UCN_OK);
    CHECK(id_body_bytes == sizeof(id_body));

    CHECK(ucn_i_service_operation_view(&fixture.service, operation,
                                        &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SERVICE_OPERATION_EXECUTING);
    CHECK(ucn_i_service_owner_destroy(&fixture.service) == UCN_ERR_STATE);
    memset(&fixture.service, 0, sizeof(fixture.service));
    CHECK(service_init() == 0);
    durable = durability(2U, 2U);
    CHECK(ucn_i_service_operation_import(&fixture.service, body,
                                          (uint32_t)body_bytes, &durable,
                                          domain_view.body_digest,
                                          &imported) == UCN_OK);
    durable = id_durability(1U, 1U);
    CHECK(ucn_i_service_operation_id_import(
              &fixture.service, id_body, &durable,
              id_domain_view.body_digest) == UCN_OK);
    CHECK(ucn_i_service_operation_id_view(&fixture.service,
                                           &id_view) == UCN_OK);
    CHECK(id_view.next_id ==
          (uint64_t)UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE + 1U);
    CHECK(id_view.reserved_through ==
          (uint64_t)UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE);
    operation_id = UINT64_C(0x5A5A5A5A5A5A5A5A);
    CHECK(ucn_i_service_operation_id_take(&fixture.service,
                                           &operation_id) == UCN_ERR_STATE);
    CHECK(operation_id == UINT64_C(0x5A5A5A5A5A5A5A5A));
    durable = id_durability(2U, 1U);
    CHECK(ucn_i_service_operation_id_prepare_interval(
              &fixture.service, 9U, &durable, &id_requirement) == UCN_OK);
    CHECK(commit_id_interval(&id_requirement, 260U, &foundation) == 0);
    CHECK(ucn_i_service_operation_id_take(&fixture.service,
                                           &operation_id) == UCN_OK);
    CHECK(operation_id ==
          (uint64_t)UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE + 1U);
    CHECK(ucn_i_service_operation_view(&fixture.service, imported,
                                        &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SERVICE_OPERATION_EXECUTING &&
          view.executor_observed == 1U);
    durable = durability(3U, 2U);
    CHECK(ucn_i_service_operation_prepare_terminal(
              &fixture.service, imported, UCN_I_SERVICE_OPERATION_IN_DOUBT,
              NULL, &durable) == UCN_OK);
    CHECK(commit_operation(imported, 300U, &foundation) == 0);
    CHECK(ucn_i_service_operation_view(&fixture.service, imported,
                                        &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SERVICE_OPERATION_IN_DOUBT &&
          view.reply_ready == 1U);
    CHECK(ucn_i_persistence_domain_get(fixture.persistence,
                                       fixture.entries[0].domain,
                                       &domain_view) == UCN_OK);
    CHECK(domain_view.record_generation == 3U);
    CHECK(ucn_i_persistence_domain_get(fixture.persistence,
                                       fixture.entries[1].domain,
                                       &id_domain_view) == UCN_OK);
    CHECK(id_domain_view.record_generation == 2U);

    CHECK(ucn_persistence_deinit(fixture.persistence) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
    puts("service persistence integration passed");
    return 0;
}
