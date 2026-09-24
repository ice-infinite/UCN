#include "fake_persistence_provider.h"
#include "internal/ucn_group.h"
#include "internal/ucn_persistence.h"

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
    ucn_i_group_owner_t group;
    test_lock_t persist_lock;
    test_lock_t gate_lock;
    test_lock_t group_lock;
} fixture_t;

static fixture_t fixture;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}
static void lock_leave(void *context) { ((test_lock_t *)context)->held = 0U; }

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
    ucn_i_group_config_t group_config;
    ucn_lock_ops_t persist_lock;
    ucn_lock_ops_t gate_lock;
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    uint16_t iteration;

    memset(&fixture, 0, sizeof(fixture));
    fixture.entry.struct_size = sizeof(fixture.entry);
    fixture.entry.api_version = UCN_PERSIST_API_VERSION;
    fixture.entry.domain.domain_kind = UCN_PERSIST_DOMAIN_GROUP_POLICY_KEY;
    fixture.entry.domain.domain_id = 900U;
    fixture.entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    fixture.entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    fixture.entry.schema_id = UCN_I_GROUP_RECORD_SCHEMA_ID;
    fixture.entry.schema_version = UCN_I_GROUP_RECORD_SCHEMA;
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
    memset(&group_config, 0, sizeof(group_config));
    group_config.runtime_instance = 1U;
    group_config.realm_id = 2U;
    group_config.owner_instance = 8U;
    group_config.state_lock = private_lock(&fixture.group_lock);
    CHECK(ucn_i_group_owner_init(&fixture.group, &group_config) == UCN_OK);
    return 0;
}

static ucn_i_group_context_config_t dynamic_group(void)
{
    ucn_i_group_context_config_t config;
    memset(&config, 0, sizeof(config));
    config.realm_id = 2U;
    config.group_generation = 1U;
    config.policy_generation = 1U;
    config.member_generation = 1U;
    config.endpoint = 1U;
    config.opcode = 2U;
    config.mode = UCN_I_GROUP_DYNAMIC;
    config.member_count = 1U;
    config.unicast_fanout_limit = 1U;
    config.quorum_weight = 1U;
    config.allowed_scope_mask = UCN_I_GROUP_SCOPE_BIT(
        UCN_I_GROUP_SCOPE_LOCAL_ONLY);
    config.secure_required = 1U;
    config.security.runtime_instance = 1U;
    config.security.key_generation = 1U;
    config.security.owner_instance = 7U;
    config.security.slot = 1U;
    config.security.generation = 1U;
    config.security.valid = 1U;
    memset(config.security.context_digest, 0xA5,
           sizeof(config.security.context_digest));
    config.members[0].address = 10U;
    config.members[0].binding_generation = 1U;
    config.members[0].sender_slot = 1U;
    config.members[0].weight = 1U;
    memset(config.members[0].principal, 0x5A,
           sizeof(config.members[0].principal));
    return config;
}

static int test_foundation_round_trip(void)
{
    ucn_i_group_context_config_t config = dynamic_group();
    ucn_i_group_authority_facts_t authority;
    ucn_i_group_durability_t durability;
    ucn_i_group_requirement_t requirement;
    ucn_i_group_proof_t proof;
    ucn_persistence_request_t request;
    ucn_persistence_proof_t foundation;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_handle_t group;
    ucn_handle_t persistence_handle;
    uint8_t published_digest[UCN_PERSIST_DIGEST_BYTES];
    uint16_t iteration;

    memset(&authority, 0, sizeof(authority));
    authority.lease_deadline_us = 9000U;
    authority.realm_id = 2U;
    authority.authority_generation = 1U;
    authority.owner_instance = 6U;
    authority.authenticated = 1U;
    authority.quorum_met = 1U;
    authority.current = 1U;
    memset(authority.proof_digest, 0x33, sizeof(authority.proof_digest));
    memset(&durability, 0, sizeof(durability));
    durability.domain_id = fixture.entry.domain.domain_id;
    durability.foundation_transaction_id = 1U;
    durability.expected_record_generation = 0U;
    durability.absolute_deadline_us = 8000U;
    durability.persistence_domain_generation = 4U;
    durability.schema_id = UCN_I_GROUP_RECORD_SCHEMA_ID;
    durability.schema_version = UCN_I_GROUP_RECORD_SCHEMA;
    CHECK(ucn_i_group_admin_prepare(&fixture.group, &config, &authority,
                                    &durability, 100U, &group,
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
    request.business_transition_digest = UINT64_C(0x0802000000000001);
    request.canonical_body = requirement.body;
    request.body_bytes = requirement.body_bytes;
    request.schema_id = requirement.durability.schema_id;
    request.schema_version = requirement.durability.schema_version;
    request.operation_kind = requirement.operation_kind;
    memcpy(request.expected_body_digest, requirement.expected_body_digest,
           sizeof(request.expected_body_digest));
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
    CHECK(memcmp(published_digest, requirement.canonical_body_digest,
                 sizeof(published_digest)) != 0);
    CHECK(ucn_i_persistence_submit(fixture.persistence, &request, 100U,
                                   &persistence_handle) == UCN_OK);
    CHECK(ucn_i_group_bind_persistence(&fixture.group, group,
                                       persistence_handle,
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
    memcpy(proof.body_digest, foundation.body_digest,
           sizeof(proof.body_digest));
    CHECK(ucn_i_group_accept_proof(&fixture.group, group, &proof,
                                   &authority, 200U) == UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(fixture.persistence,
                                         persistence_handle) == UCN_OK);
    return 0;
}

int main(void)
{
    int result = fixture_init();
    if (result == 0) result = test_foundation_round_trip();
    if (result != 0) fprintf(stderr, "group persistence failed: %d\n", result);
    return result;
}
