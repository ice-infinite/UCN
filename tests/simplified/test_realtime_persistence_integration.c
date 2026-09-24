#include "fake_persistence_provider.h"
#include "internal/ucn_persistence.h"
#include "internal/ucn_realtime.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression_)                                                     \
    do {                                                                       \
        if (!(expression_)) {                                                  \
            fprintf(stderr, "check failed at %d: %s\n", __LINE__,            \
                    #expression_);                                             \
            return __LINE__;                                                   \
        }                                                                      \
    } while (0)

typedef struct test_lock { uint8_t held; } test_lock_t;

typedef struct fixture {
    ucn_persist_manifest_entry_t entry;
    ucn_persist_domain_binding_t binding;
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake;
    ucn_persistence_provider_t provider;
    ucn_persistence_config_t persist_config;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace;
    ucn_persistence_storage_t persistence_storage;
    ucn_persist_callback_gate_t *gate;
    ucn_persistence_owner_t *persistence;
    ucn_i_realtime_owner_t realtime;
    test_lock_t persist_lock;
    test_lock_t gate_lock;
    test_lock_t realtime_lock;
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

static ucn_i_lock_ops_t private_lock(test_lock_t *state)
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

static int fixture_init(void)
{
    ucn_i_realtime_config_t realtime_config;
    ucn_lock_ops_t persist_lock;
    ucn_lock_ops_t gate_lock;
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    uint16_t iteration;

    memset(&fixture, 0, sizeof(fixture));
    fixture.entry.struct_size = sizeof(fixture.entry);
    fixture.entry.api_version = UCN_PERSIST_API_VERSION;
    fixture.entry.domain.domain_kind = UCN_PERSIST_DOMAIN_TIME_AUTHORITY;
    fixture.entry.domain.domain_id = 800U;
    fixture.entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    fixture.entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    fixture.entry.schema_id = UCN_I_REALTIME_DOMAIN_SCHEMA_ID;
    fixture.entry.schema_version = UCN_I_REALTIME_DOMAIN_RECORD_SCHEMA;
    fixture.entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    fixture.entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    fixture.entry.provider_atomicity_class = UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    fixture.binding.struct_size = sizeof(fixture.binding);
    fixture.binding.api_version = UCN_PERSIST_API_VERSION;
    fixture.binding.domain = fixture.entry.domain;
    fixture.binding.business_owner_instance = 8U;
    fixture.binding.domain_generation = 4U;
    fixture.manifest.struct_size = sizeof(fixture.manifest);
    fixture.manifest.api_version = UCN_PERSIST_API_VERSION;
    fixture.manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    fixture.manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    fixture.manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    fixture.manifest.entries = &fixture.entry;
    fixture.manifest.entry_count = 1U;
    fixture.manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(&fixture.manifest,
                                          &fixture.digest_workspace,
                                          digest) == UCN_OK);
    memcpy(fixture.manifest.expected_digest, digest, sizeof(digest));
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    fake_persist_provider_make_public(&fixture.fake, 0xFFU, &fixture.provider);
    persist_lock = public_lock(&fixture.persist_lock);
    gate_lock = public_lock(&fixture.gate_lock);
    CHECK(ucn_persist_callback_gate_init_in_place(
              &fixture.gate_storage, sizeof(fixture.gate_storage),
              &gate_lock, &fixture.gate) == UCN_OK);
    fixture.persist_config.struct_size = sizeof(fixture.persist_config);
    fixture.persist_config.api_version = UCN_PERSIST_API_VERSION;
    fixture.persist_config.runtime_instance = 1U;
    fixture.persist_config.owner_instance = 9U;
    fixture.persist_config.required_domain_mask = 1U;
    fixture.persist_config.manifest = &fixture.manifest;
    fixture.persist_config.domain_bindings = &fixture.binding;
    fixture.persist_config.domain_binding_count = 1U;
    fixture.persist_config.provider = &fixture.provider;
    fixture.persist_config.state_lock = persist_lock;
    fixture.persist_config.shared_callback_gate = fixture.gate;
    fixture.persist_config.digest_workspace = &fixture.digest_workspace;
    CHECK(ucn_persistence_init_in_place(
              &fixture.persistence_storage, sizeof(fixture.persistence_storage),
              &fixture.persist_config, &fixture.persistence) == UCN_OK);
    CHECK(ucn_i_persistence_start_recovery(fixture.persistence) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence, iteration + 1U,
                                     8U, &step) == UCN_OK);
        if (step.owner_ready != 0U && step.made_progress == 0U) break;
    }
    CHECK(iteration < 32U);
    memset(&realtime_config, 0, sizeof(realtime_config));
    realtime_config.runtime_instance = 1U;
    realtime_config.owner_instance = 8U;
    realtime_config.state_lock = private_lock(&fixture.realtime_lock);
    CHECK(ucn_i_realtime_owner_init(&fixture.realtime,
                                     &realtime_config) == UCN_OK);
    return 0;
}

static ucn_i_realtime_domain_config_t domain_config(void)
{
    ucn_i_realtime_domain_config_t config;
    memset(&config, 0, sizeof(config));
    config.clock_domain_id = 7U;
    config.domain_generation = UINT32_C(0x01020304);
    config.lock_sample_count = 2U;
    config.sync_timeout_us = 1000U;
    config.max_holdover_us = 5000U;
    config.oscillator_uncertainty_ppb = 100U;
    config.max_offset_jump_us = 1000U;
    config.base_uncertainty.known_mask = UCN_I_REALTIME_KNOWN_ALL;
    config.base_uncertainty.timer_resolution_bound_us = 1U;
    config.base_uncertainty.link_timestamp_capture_bound_us = 2U;
    config.base_uncertainty.filter_residual_bound_us = 3U;
    config.base_uncertainty.arithmetic_rounding_bound_us = 1U;
    config.base_uncertainty.sample_capture_bound_us = 2U;
    config.base_uncertainty.path_asymmetry_bound_us = 5U;
    config.path.route_causal_id = 11U;
    config.path.route_generation = 12U;
    config.path.session_generation = 13U;
    config.path.capability_generation = 14U;
    config.path.forward_link_generation = 15U;
    config.path.reverse_link_generation = 16U;
    config.path.forward_link_id = 1U;
    config.path.reverse_link_id = 2U;
    memset(config.path.capability_digest, 0xA5, 16U);
    config.path.authenticated = 1U;
    config.path.frozen = 1U;
    config.path.directional = 1U;
    config.path.asymmetry_known = 1U;
    config.path.max_asymmetry_us = 5U;
    return config;
}

static int test_real_foundation_round_trip(void)
{
    ucn_i_realtime_domain_config_t config = domain_config();
    ucn_i_realtime_durability_t durability;
    ucn_i_realtime_requirement_t requirement;
    ucn_i_realtime_proof_t proof;
    ucn_persistence_request_t request;
    ucn_persistence_proof_t foundation;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_handle_t domain;
    ucn_handle_t persistence_handle;
    uint8_t published_digest[16];
    uint16_t iteration;

    memset(&durability, 0, sizeof(durability));
    durability.domain_id = fixture.entry.domain.domain_id;
    durability.foundation_transaction_id = 1U;
    durability.expected_record_generation = 0U;
    durability.absolute_deadline_us = 10000U;
    durability.volatile_continuation.runtime_instance = 1U;
    durability.volatile_continuation.owner_instance = 8U;
    durability.volatile_continuation.slot = 1U;
    durability.volatile_continuation.generation = 1U;
    durability.volatile_continuation.object_kind = UCN_OBJECT_KIND_TIME_DOMAIN;
    durability.persistence_domain_generation = 4U;
    durability.schema_id = UCN_I_REALTIME_DOMAIN_SCHEMA_ID;
    durability.schema_version = UCN_I_REALTIME_DOMAIN_RECORD_SCHEMA;
    CHECK(ucn_i_realtime_domain_prepare(&fixture.realtime, &config,
                                         &durability, &domain,
                                         &requirement) == UCN_OK);
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.api_version = UCN_PERSIST_API_VERSION;
    request.runtime_instance = requirement.runtime_instance;
    request.caller_owner_instance = requirement.caller_owner_instance;
    request.domain_generation =
        requirement.durability.persistence_domain_generation;
    request.domain = fixture.entry.domain;
    request.foundation_transaction_id =
        requirement.durability.foundation_transaction_id;
    request.expected_record_generation =
        requirement.durability.expected_record_generation;
    request.absolute_deadline_us = requirement.durability.absolute_deadline_us;
    request.business_transition_digest = UINT64_C(0x0801000000000001);
    request.canonical_body = requirement.body;
    request.body_bytes = requirement.body_bytes;
    request.schema_id = requirement.durability.schema_id;
    request.schema_version = requirement.durability.schema_version;
    request.operation_kind = requirement.operation_kind;
    memcpy(request.expected_body_digest, requirement.expected_body_digest, 16U);
    request.volatile_continuation =
        requirement.durability.volatile_continuation;
    memset(&meta, 0, sizeof(meta));
    memset(&codec, 0, sizeof(codec));
    meta.domain = request.domain;
    meta.record_generation = 1U;
    meta.transaction_id = request.foundation_transaction_id;
    meta.body_bytes = request.body_bytes;
    meta.schema_id = request.schema_id;
    meta.schema_version = request.schema_version;
    meta.operation_kind = request.operation_kind;
    CHECK(ucn_i_persist_body_digest(&meta, request.canonical_body,
                                    published_digest, &codec) == UCN_OK);
    CHECK(ucn_i_persistence_submit(fixture.persistence, &request, 100U,
                                   &persistence_handle) == UCN_OK);
    CHECK(ucn_i_realtime_domain_bind_persistence(
              &fixture.realtime, domain, persistence_handle,
              published_digest) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence,
                                     101U + iteration, 8U, &step) == UCN_OK);
        if (step.proofs_ready != 0U) break;
    }
    CHECK(iteration < 32U);
    CHECK(ucn_i_persistence_proof_get(fixture.persistence,
                                      persistence_handle,
                                      &foundation) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence_handle;
    proof.domain_id = foundation.domain.domain_id;
    proof.foundation_transaction_id = foundation.foundation_transaction_id;
    proof.record_generation = foundation.record_generation;
    proof.witness_generation = foundation.witness_generation;
    proof.runtime_instance = foundation.runtime_instance;
    proof.body_bytes = foundation.body_bytes;
    proof.persistence_domain_generation = foundation.domain_generation;
    proof.persistence_owner_instance = foundation.persistence_owner_instance;
    proof.caller_owner_instance = foundation.caller_owner_instance;
    proof.schema_id = request.schema_id;
    proof.schema_version = request.schema_version;
    proof.operation_kind = foundation.operation_kind;
    memcpy(proof.body_digest, foundation.body_digest, 16U);
    CHECK(ucn_i_realtime_domain_accept_proof(&fixture.realtime, domain,
                                              &proof, 200U) == UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(fixture.persistence,
                                         persistence_handle) == UCN_OK);
    return 0;
}

int main(void)
{
    int result = fixture_init();
    if (result == 0) result = test_real_foundation_round_trip();
    if (result != 0) fprintf(stderr, "realtime persistence failed: %d\n", result);
    return result;
}
