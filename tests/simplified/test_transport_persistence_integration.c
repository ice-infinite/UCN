#include "fake_persistence_provider.h"
#include "internal/ucn_persistence.h"
#include "internal/ucn_transport.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr_)                                                         \
    do {                                                                     \
        if (!(expr_)) {                                                      \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #expr_);                                       \
            return 1;                                                        \
        }                                                                    \
    } while (0)

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

typedef struct fixture {
    ucn_persist_manifest_entry_t entry;
    ucn_persist_domain_binding_t binding;
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake;
    ucn_persistence_provider_t provider;
    ucn_persistence_config_t persistence_config;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace;
    ucn_persistence_storage_t persistence_storage;
    ucn_persist_callback_gate_t *gate;
    ucn_persistence_owner_t *persistence;
    ucn_i_transport_owner_t transport;
    test_lock_t persistence_lock;
    test_lock_t gate_lock;
    test_lock_t transport_lock;
} fixture_t;

static fixture_t fixture;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
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

static int transport_init(void)
{
    ucn_i_transport_config_t config;
    memset(&fixture.transport, 0, sizeof(fixture.transport));
    memset(&fixture.transport_lock, 0, sizeof(fixture.transport_lock));
    memset(&config, 0, sizeof(config));
    config.reliable_lifetime_us = 1000U;
    config.reliable_retry_us = 100U;
    config.receipt_lifetime_us = 500U;
    config.runtime_instance = 31U;
    config.owner_instance = 32U;
    config.reliable_max_attempts = 3U;
    config.state_lock = internal_lock(&fixture.transport_lock);
    return ucn_i_transport_owner_init(&fixture.transport, &config) == UCN_OK ?
               0 : 1;
}

static int fixture_init(void)
{
    uint8_t manifest_digest[16];
    ucn_lock_ops_t owner_lock;
    ucn_lock_ops_t gate_lock;
    uint16_t iteration;

    memset(&fixture, 0, sizeof(fixture));
    fixture.entry.struct_size = sizeof(fixture.entry);
    fixture.entry.api_version = UCN_PERSIST_API_VERSION;
    fixture.entry.domain.domain_kind = UCN_PERSIST_DOMAIN_TRANSPORT_HIGH_WATER;
    fixture.entry.domain.domain_id = 100U;
    fixture.entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    fixture.entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    fixture.entry.schema_id = UCN_I_TRANSPORT_PARENT_SCHEMA_ID;
    fixture.entry.schema_version = UCN_I_TRANSPORT_SCHEMA;
    fixture.entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    fixture.entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    fixture.entry.provider_atomicity_class =
        UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    fixture.binding.struct_size = sizeof(fixture.binding);
    fixture.binding.api_version = UCN_PERSIST_API_VERSION;
    fixture.binding.domain = fixture.entry.domain;
    fixture.binding.business_owner_instance = 32U;
    fixture.binding.domain_generation = 3U;
    fixture.manifest.struct_size = sizeof(fixture.manifest);
    fixture.manifest.api_version = UCN_PERSIST_API_VERSION;
    fixture.manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    fixture.manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    fixture.manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    fixture.manifest.entries = &fixture.entry;
    fixture.manifest.entry_count = 1U;
    fixture.manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &fixture.manifest, &fixture.digest_workspace,
              manifest_digest) == UCN_OK);
    memcpy(fixture.manifest.expected_digest, manifest_digest, 16U);
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    fake_persist_provider_make_public(&fixture.fake, 0xFFU,
                                      &fixture.provider);
    owner_lock = public_lock(&fixture.persistence_lock);
    gate_lock = public_lock(&fixture.gate_lock);
    CHECK(ucn_persist_callback_gate_init_in_place(
              &fixture.gate_storage, sizeof(fixture.gate_storage),
              &gate_lock, &fixture.gate) == UCN_OK);
    fixture.persistence_config.struct_size =
        sizeof(fixture.persistence_config);
    fixture.persistence_config.api_version = UCN_PERSIST_API_VERSION;
    fixture.persistence_config.runtime_instance = 31U;
    fixture.persistence_config.owner_instance = 40U;
    fixture.persistence_config.required_domain_mask = 1U;
    fixture.persistence_config.manifest = &fixture.manifest;
    fixture.persistence_config.domain_bindings = &fixture.binding;
    fixture.persistence_config.domain_binding_count = 1U;
    fixture.persistence_config.provider = &fixture.provider;
    fixture.persistence_config.state_lock = owner_lock;
    fixture.persistence_config.shared_callback_gate = fixture.gate;
    fixture.persistence_config.digest_workspace = &fixture.digest_workspace;
    CHECK(ucn_persistence_init_in_place(
              &fixture.persistence_storage,
              sizeof(fixture.persistence_storage),
              &fixture.persistence_config,
              &fixture.persistence) == UCN_OK);
    CHECK(ucn_i_persistence_start_recovery(fixture.persistence) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence,
                                     iteration + 1U, 8U,
                                     &step) == UCN_OK);
        if (step.owner_ready != 0U && step.made_progress == 0U) {
            break;
        }
    }
    CHECK(iteration < 32U);
    return transport_init();
}

static void fill_principal(uint8_t principal[16], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        principal[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_transport_parent_context_t parent_context(void)
{
    ucn_i_transport_parent_context_t value;
    memset(&value, 0, sizeof(value));
    value.source.address = 1U;
    value.source.generation = 2U;
    fill_principal(value.source.principal, 0x10U);
    value.destination.address = 3U;
    value.destination.generation = 4U;
    fill_principal(value.destination.principal, 0x30U);
    value.realm = 5U;
    value.transport_policy_generation = 6U;
    value.security_session_generation = 7U;
    value.security_key_generation = 8U;
    value.parent_generation = 9U;
    value.origin_security = 1U;
    return value;
}

static void request_from_requirement(
    const ucn_i_transport_parent_requirement_t *requirement,
    ucn_persistence_request_t *request)
{
    memset(request, 0, sizeof(*request));
    request->struct_size = sizeof(*request);
    request->api_version = UCN_PERSIST_API_VERSION;
    request->runtime_instance = requirement->runtime_instance;
    request->caller_owner_instance = requirement->caller_owner_instance;
    request->domain_generation = requirement->domain_generation;
    request->domain = fixture.entry.domain;
    request->foundation_transaction_id =
        requirement->foundation_transaction_id;
    request->expected_record_generation =
        requirement->expected_record_generation;
    request->absolute_deadline_us = requirement->absolute_deadline_us;
    request->business_transition_digest =
        requirement->transition_fingerprint;
    request->canonical_body = requirement->canonical_body;
    request->body_bytes = requirement->body_bytes;
    request->schema_id = requirement->schema_id;
    request->schema_version = requirement->schema_version;
    request->operation_kind = requirement->operation_kind;
    memcpy(request->expected_body_digest,
           requirement->expected_body_digest, 16U);
    request->volatile_continuation = requirement->volatile_continuation;
}

static void transport_proof_from_foundation(
    const ucn_i_transport_parent_requirement_t *requirement,
    ucn_handle_t persistence_handle,
    const ucn_persistence_proof_t *foundation,
    ucn_i_transport_parent_proof_t *proof)
{
    memset(proof, 0, sizeof(*proof));
    proof->persistence_handle = persistence_handle;
    proof->domain_id = foundation->domain.domain_id;
    proof->record_generation = foundation->record_generation;
    proof->foundation_transaction_id = foundation->foundation_transaction_id;
    proof->witness_generation = foundation->witness_generation;
    proof->transition_fingerprint = requirement->transition_fingerprint;
    proof->runtime_instance = foundation->runtime_instance;
    proof->body_bytes = foundation->body_bytes;
    proof->volatile_continuation = requirement->volatile_continuation;
    proof->persistence_owner_instance = foundation->persistence_owner_instance;
    proof->caller_owner_instance = foundation->caller_owner_instance;
    proof->domain_generation = foundation->domain_generation;
    proof->schema_id = requirement->schema_id;
    proof->schema_version = requirement->schema_version;
    proof->operation_kind = foundation->operation_kind;
    memcpy(proof->body_digest, foundation->body_digest, 16U);
}

static int commit_requirement(
    ucn_handle_t parent,
    const ucn_i_transport_parent_requirement_t *requirement,
    uint64_t now_us,
    uint32_t *transfer_id_out,
    ucn_persistence_proof_t *foundation_out)
{
    ucn_persistence_request_t request;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_i_transport_parent_proof_t proof;
    ucn_handle_t persistence_handle;
    uint8_t expected_published_digest[16];
    uint16_t iteration;

    request_from_requirement(requirement, &request);
    memset(&meta, 0, sizeof(meta));
    memset(&codec, 0, sizeof(codec));
    meta.domain = request.domain;
    meta.record_generation = request.expected_record_generation + 1U;
    meta.transaction_id = request.foundation_transaction_id;
    meta.body_bytes = request.body_bytes;
    meta.schema_id = request.schema_id;
    meta.schema_version = request.schema_version;
    meta.operation_kind = request.operation_kind;
    CHECK(ucn_i_persist_body_digest(
              &meta, request.canonical_body, expected_published_digest,
              &codec) == UCN_OK);
    CHECK(ucn_i_persistence_submit(
              fixture.persistence, &request, now_us,
              &persistence_handle) == UCN_OK);
    CHECK(ucn_i_transport_parent_bind_persistence(
              &fixture.transport, parent, persistence_handle,
              expected_published_digest) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence,
                                     now_us + 1U + iteration, 8U,
                                     &step) == UCN_OK);
        if (step.proofs_ready != 0U) {
            break;
        }
    }
    CHECK(iteration < 32U);
    CHECK(ucn_i_persistence_proof_get(
              fixture.persistence, persistence_handle,
              foundation_out) == UCN_OK);
    transport_proof_from_foundation(requirement, persistence_handle,
                                    foundation_out, &proof);
    CHECK(proof.domain_id == fixture.transport.parents[parent.slot].domain_id);
    CHECK(proof.record_generation ==
          fixture.transport.parents[parent.slot].record_generation + 1U);
    CHECK(proof.foundation_transaction_id ==
          fixture.transport.parents[parent.slot]
              .durability.next_foundation_transaction_id);
    CHECK(proof.witness_generation == proof.record_generation);
    CHECK(proof.transition_fingerprint ==
          fixture.transport.parents[parent.slot].transition_fingerprint);
    CHECK(proof.runtime_instance == fixture.transport.runtime_instance);
    CHECK(proof.body_bytes == UCN_I_TRANSPORT_PARENT_RECORD_BYTES);
    CHECK(memcmp(&proof.persistence_handle,
                 &fixture.transport.parents[parent.slot].persistence_handle,
                 sizeof(proof.persistence_handle)) == 0);
    CHECK(memcmp(&proof.volatile_continuation,
                 &fixture.transport.parents[parent.slot]
                      .durability.volatile_continuation,
                 sizeof(proof.volatile_continuation)) == 0);
    CHECK(proof.persistence_owner_instance ==
          fixture.transport.parents[parent.slot]
              .persistence_handle.owner_instance);
    CHECK(proof.caller_owner_instance == fixture.transport.owner_instance);
    CHECK(proof.domain_generation ==
          fixture.transport.parents[parent.slot].domain_generation);
    CHECK(proof.schema_id == UCN_I_TRANSPORT_PARENT_SCHEMA_ID);
    CHECK(proof.schema_version == UCN_I_TRANSPORT_SCHEMA);
    CHECK(proof.operation_kind == UCN_I_TRANSPORT_PARENT_OPERATION_KIND);
    CHECK(proof.reserved_zero == 0U);
    CHECK(memcmp(proof.body_digest,
                 fixture.transport.parents[parent.slot]
                     .pending_published_digest, 16U) == 0);
    CHECK(ucn_i_transport_parent_activate_next(
              &fixture.transport, parent, &proof,
              now_us + 100U, transfer_id_out) == UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(
              fixture.persistence, persistence_handle) == UCN_OK);
    return 0;
}

int main(void)
{
    ucn_i_transport_parent_context_t context = parent_context();
    ucn_i_transport_parent_durability_base_t durability;
    ucn_i_transport_parent_requirement_t requirement;
    ucn_i_transport_parent_view_t view;
    ucn_persistence_proof_t foundation;
    ucn_persistence_domain_view_t domain_view;
    ucn_i_transport_parent_context_t recovered_context;
    ucn_handle_t parent;
    uint8_t body[UCN_I_TRANSPORT_PARENT_RECORD_BYTES];
    size_t body_bytes = 0U;
    uint32_t high_water = 0U;
    uint32_t transfer_id = 0U;
    uint64_t transaction_id = 0U;

    CHECK(fixture_init() == 0);
    memset(&durability, 0, sizeof(durability));
    durability.domain_id = fixture.entry.domain.domain_id;
    durability.next_foundation_transaction_id = 1U;
    durability.absolute_deadline_us = 10000U;
    durability.domain_generation = 3U;
    durability.volatile_continuation.runtime_instance = 31U;
    durability.volatile_continuation.owner_instance = 32U;
    durability.volatile_continuation.slot = 1U;
    durability.volatile_continuation.generation = 1U;
    durability.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    CHECK(ucn_i_transport_parent_prepare_first(
              &fixture.transport, &context, &durability, 100U,
              &parent) == UCN_OK);
    CHECK(ucn_i_transport_parent_requirement_get(
              &fixture.transport, parent, &requirement) == UCN_OK);
    CHECK(commit_requirement(parent, &requirement, 110U,
                             &transfer_id, &foundation) == 0);
    CHECK(transfer_id == 1U);
    CHECK(ucn_i_transport_parent_discard_grant(
              &fixture.transport, parent, transfer_id) == UCN_OK);

    memset(&durability, 0, sizeof(durability));
    durability.domain_id = fixture.entry.domain.domain_id;
    durability.prior_foundation_transaction_id = 1U;
    durability.next_foundation_transaction_id = 2U;
    durability.expected_record_generation = 1U;
    durability.absolute_deadline_us = 20000U;
    durability.prior_transfer_high_water = 1U;
    durability.domain_generation = 3U;
    memcpy(durability.expected_body_digest, foundation.body_digest, 16U);
    durability.volatile_continuation.runtime_instance = 31U;
    durability.volatile_continuation.owner_instance = 32U;
    durability.volatile_continuation.slot = 2U;
    durability.volatile_continuation.generation = 1U;
    durability.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    CHECK(ucn_i_transport_parent_prepare_next(
              &fixture.transport, parent, &durability, 1000U) == UCN_OK);
    CHECK(ucn_i_transport_parent_requirement_get(
              &fixture.transport, parent, &requirement) == UCN_OK);
    CHECK(commit_requirement(parent, &requirement, 1010U,
                             &transfer_id, &foundation) == 0);
    CHECK(transfer_id == 2U);
    CHECK(ucn_i_transport_parent_discard_grant(
              &fixture.transport, parent, transfer_id) == UCN_OK);

    CHECK(ucn_i_persistence_domain_get(
              fixture.persistence, fixture.entry.domain,
              &domain_view) == UCN_OK);
    CHECK(domain_view.record_generation == 2U);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.persistence, fixture.entry.domain, body,
              sizeof(body), &body_bytes) == UCN_OK);
    CHECK(body_bytes == sizeof(body));
    CHECK(ucn_i_transport_parent_record_decode(
              body, &recovered_context, &high_water,
              &transaction_id) == UCN_OK);
    CHECK(high_water == 2U && transaction_id == 2U);
    CHECK(ucn_i_transport_owner_destroy(&fixture.transport) == UCN_OK);
    CHECK(transport_init() == 0);
    CHECK(ucn_i_transport_parent_import(
              &fixture.transport, &recovered_context, high_water,
              domain_view.domain.domain_id, domain_view.record_generation,
              transaction_id, domain_view.domain_generation,
              domain_view.body_digest, &parent) == UCN_OK);
    CHECK(ucn_i_transport_parent_view(
              &fixture.transport, parent, &view) == UCN_OK);
    CHECK(view.transfer_high_water == 2U);
    CHECK(view.foundation_transaction_id == 2U);
    CHECK(view.domain_id == fixture.entry.domain.domain_id);

    CHECK(ucn_i_transport_owner_destroy(&fixture.transport) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.persistence) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
    puts("transport persistence integration passed");
    return 0;
}
