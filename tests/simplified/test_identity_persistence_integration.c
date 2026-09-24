#include "fake_persistence_provider.h"
#include "internal/ucn_identity.h"
#include "internal/ucn_persistence.h"

#include <stdio.h>
#include <string.h>

UCN_STATIC_ASSERT(
    UCN_I_IDENTITY_PERSISTENCE_REQUEST_FAILED == UCN_PERSIST_REQUEST_FAILED,
    identity_failure_view_must_match_persistence_state);

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
    ucn_i_identity_domain_rule_t rule;
    ucn_i_identity_config_t identity_config;
    ucn_i_identity_owner_t identity;
    test_lock_t persistence_lock;
    test_lock_t gate_lock;
    test_lock_t identity_lock;
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
    test_lock_t *lock = context;

    lock->held = 0U;
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

static void fill(uint8_t *bytes, size_t length, uint8_t value)
{
    memset(bytes, value, length);
}

static int fixture_init(void)
{
    uint8_t manifest_digest[16];
    ucn_lock_ops_t owner_lock;
    ucn_lock_ops_t callback_lock;
    uint16_t iteration;

    memset(&fixture, 0, sizeof(fixture));
    fixture.entry.struct_size = sizeof(fixture.entry);
    fixture.entry.api_version = UCN_PERSIST_API_VERSION;
    fixture.entry.domain.domain_kind = UCN_PERSIST_DOMAIN_IDENTITY_BINDING;
    fixture.entry.domain.domain_id = UINT64_C(0x9001);
    fixture.entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    fixture.entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    fixture.entry.schema_id = UCN_I_IDENTITY_BINDING_SCHEMA_ID;
    fixture.entry.schema_version = UCN_I_IDENTITY_BINDING_RECORD_SCHEMA;
    fixture.entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    fixture.entry.witness_policy =
        UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    fixture.entry.provider_atomicity_class =
        UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    fixture.binding.struct_size = sizeof(fixture.binding);
    fixture.binding.api_version = UCN_PERSIST_API_VERSION;
    fixture.binding.domain = fixture.entry.domain;
    fixture.binding.business_owner_instance = 21U;
    fixture.binding.domain_generation = 1U;
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
    callback_lock = public_lock(&fixture.gate_lock);
    CHECK(ucn_persist_callback_gate_init_in_place(
              &fixture.gate_storage, sizeof(fixture.gate_storage),
              &callback_lock, &fixture.gate) == UCN_OK);
    fixture.persistence_config.struct_size =
        sizeof(fixture.persistence_config);
    fixture.persistence_config.api_version = UCN_PERSIST_API_VERSION;
    fixture.persistence_config.runtime_instance = 10U;
    fixture.persistence_config.owner_instance = 22U;
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

    fixture.rule.domain_id = fixture.entry.domain.domain_id;
    fixture.identity_config.struct_size = sizeof(fixture.identity_config);
    fixture.identity_config.api_version = UCN_API_VERSION;
    fixture.identity_config.runtime_instance = 10U;
    fixture.identity_config.realm_id = 42U;
    fixture.identity_config.owner_instance = 21U;
    fixture.identity_config.authority_owner_instance = 19U;
    fixture.identity_config.persistence_business_owner_instance = 21U;
    fixture.identity_config.persistence_owner_instance = 22U;
    fixture.identity_config.address_width = 2U;
    fixture.identity_config.domain_rules = &fixture.rule;
    fixture.identity_config.domain_rule_count = 1U;
    fixture.identity_config.state_lock =
        internal_lock(&fixture.identity_lock);
    CHECK(ucn_i_identity_owner_init(
              &fixture.identity, &fixture.identity_config) == UCN_OK);
    return 0;
}

static ucn_i_identity_binding_issue_t make_issue(void)
{
    ucn_i_identity_binding_issue_t issue;

    memset(&issue, 0, sizeof(issue));
    issue.transaction_id = 100U;
    issue.challenge_started_local_us = 100U;
    issue.challenge_deadline_us = 5000U;
    issue.lease_duration_us = 2000U;
    issue.authority_lease_sequence = 6U;
    issue.runtime_instance = 10U;
    issue.realm_id = 42U;
    issue.address = 9U;
    issue.binding_generation = 1U;
    issue.authority_generation = 3U;
    issue.link_generation = 9U;
    issue.admission_generation = 1U;
    issue.admission_owner_instance = 20U;
    issue.address_width = 2U;
    issue.mode = UCN_I_IDENTITY_ADDRESS_LEASED;
    fill(issue.principal, sizeof(issue.principal), 0x44U);
    fill(issue.authority_principal,
         sizeof(issue.authority_principal), 0x22U);
    fill(issue.lease_id, sizeof(issue.lease_id), 0x55U);
    fill(issue.transcript_digest, sizeof(issue.transcript_digest), 0x77U);
    return issue;
}

static ucn_i_identity_authority_view_t make_authority(void)
{
    ucn_i_identity_authority_view_t authority;

    memset(&authority, 0, sizeof(authority));
    authority.lease_sequence = 6U;
    authority.local_deadline_us = 10000U;
    authority.record_generation = 1U;
    authority.foundation_transaction_id = 1U;
    authority.witness_generation = 1U;
    authority.runtime_instance = 10U;
    authority.realm_id = 42U;
    authority.authority_generation = 3U;
    authority.authority_owner_instance = 19U;
    authority.persistence_owner_instance = 22U;
    authority.schema_id = UCN_I_IDENTITY_AUTHORITY_SCHEMA_ID;
    authority.schema_version = 1U;
    fill(authority.principal, sizeof(authority.principal), 0x22U);
    fill(authority.body_digest, sizeof(authority.body_digest), 0x33U);
    return authority;
}

int main(void)
{
    ucn_i_identity_binding_issue_t issue = make_issue();
    ucn_i_identity_authority_view_t authority = make_authority();
    ucn_i_identity_durability_base_t durability;
    ucn_i_identity_handle_t identity_handle;
    ucn_i_identity_requirement_view_t requirement;
    ucn_persistence_request_t request;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_handle_t persistence_handle;
    ucn_persistence_proof_t foundation_proof;
    ucn_i_identity_durability_proof_t identity_proof;
    ucn_i_identity_binding_view_t view;
    ucn_persistence_domain_view_t domain_view;
    uint8_t expected_published_digest[16];
    uint8_t copied_body[UCN_I_IDENTITY_BINDING_RECORD_BYTES];
    size_t copied_bytes = 0U;
    uint16_t iteration;

    CHECK(fixture_init() == 0);
    memset(&durability, 0, sizeof(durability));
    durability.domain_id = fixture.entry.domain.domain_id;
    durability.next_transaction_id = 1U;
    durability.absolute_deadline_us = 4000U;
    durability.domain_generation = 1U;
    durability.volatile_continuation.runtime_instance = 10U;
    durability.volatile_continuation.owner_instance = 21U;
    durability.volatile_continuation.slot = 1U;
    durability.volatile_continuation.generation = 1U;
    durability.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    CHECK(ucn_i_identity_prepare_binding(
              &fixture.identity, &issue, &authority, &durability,
              150U, &identity_handle) == UCN_OK);
    CHECK(ucn_i_identity_requirement_get(
              &fixture.identity, identity_handle, &requirement) == UCN_OK);

    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.api_version = UCN_PERSIST_API_VERSION;
    request.runtime_instance = requirement.runtime_instance;
    request.caller_owner_instance = requirement.caller_owner_instance;
    request.domain_generation = requirement.domain_generation;
    request.domain = fixture.entry.domain;
    request.foundation_transaction_id =
        requirement.foundation_transaction_id;
    request.expected_record_generation =
        requirement.expected_record_generation;
    request.absolute_deadline_us = requirement.absolute_deadline_us;
    request.business_transition_digest =
        requirement.transition_fingerprint;
    request.canonical_body = requirement.canonical_body;
    request.body_bytes = requirement.body_bytes;
    request.schema_id = requirement.schema_id;
    request.schema_version = requirement.schema_version;
    request.operation_kind = requirement.operation_kind;
    memcpy(request.expected_body_digest,
           requirement.expected_body_digest, 16U);
    request.volatile_continuation = requirement.volatile_continuation;

    memset(&meta, 0, sizeof(meta));
    memset(&codec, 0, sizeof(codec));
    meta.domain = request.domain;
    meta.record_generation = 1U;
    meta.transaction_id = request.foundation_transaction_id;
    meta.body_bytes = request.body_bytes;
    meta.schema_id = request.schema_id;
    meta.schema_version = request.schema_version;
    meta.operation_kind = request.operation_kind;
    CHECK(ucn_i_persist_body_digest(
              &meta, request.canonical_body, expected_published_digest,
              &codec) == UCN_OK);
    CHECK(ucn_i_persistence_submit(
              fixture.persistence, &request, 160U,
              &persistence_handle) == UCN_OK);
    CHECK(ucn_i_identity_bind_persistence(
              &fixture.identity, identity_handle, persistence_handle,
              expected_published_digest) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence,
                                     170U + iteration, 8U,
                                     &step) == UCN_OK);
        if (step.proofs_ready != 0U) {
            break;
        }
    }
    CHECK(iteration < 32U);
    CHECK(ucn_i_persistence_proof_get(
              fixture.persistence, persistence_handle,
              &foundation_proof) == UCN_OK);
    memset(&identity_proof, 0, sizeof(identity_proof));
    identity_proof.persistence_handle = persistence_handle;
    identity_proof.domain_id = foundation_proof.domain.domain_id;
    identity_proof.record_generation = foundation_proof.record_generation;
    identity_proof.foundation_transaction_id =
        foundation_proof.foundation_transaction_id;
    identity_proof.witness_generation =
        foundation_proof.witness_generation;
    identity_proof.transition_fingerprint =
        requirement.transition_fingerprint;
    identity_proof.runtime_instance = foundation_proof.runtime_instance;
    identity_proof.body_bytes = foundation_proof.body_bytes;
    identity_proof.volatile_continuation =
        requirement.volatile_continuation;
    identity_proof.persistence_owner_instance =
        foundation_proof.persistence_owner_instance;
    identity_proof.caller_owner_instance =
        foundation_proof.caller_owner_instance;
    identity_proof.domain_generation = foundation_proof.domain_generation;
    identity_proof.schema_id = requirement.schema_id;
    identity_proof.schema_version = requirement.schema_version;
    identity_proof.operation_kind = foundation_proof.operation_kind;
    memcpy(identity_proof.body_digest,
           foundation_proof.body_digest, 16U);
    CHECK(ucn_i_identity_activate_binding(
              &fixture.identity, identity_handle, &identity_proof,
              &authority, 250U, &view) == UCN_OK);
    CHECK(view.record_generation == 1U);
    CHECK(view.foundation_transaction_id == 1U);
    CHECK(view.witness_generation == 1U);
    CHECK(view.local_deadline_us == 2100U);
    CHECK(memcmp(view.body_digest, expected_published_digest, 16U) == 0);
    CHECK(ucn_i_persistence_domain_get(
              fixture.persistence, fixture.entry.domain,
              &domain_view) == UCN_OK);
    CHECK(domain_view.state == UCN_PERSIST_DOMAIN_READY);
    CHECK(domain_view.record_generation == 1U);
    CHECK(ucn_i_persistence_domain_copy_body(
              fixture.persistence, fixture.entry.domain, copied_body,
              sizeof(copied_body), &copied_bytes) == UCN_OK);
    CHECK(copied_bytes == UCN_I_IDENTITY_BINDING_RECORD_BYTES);
    CHECK(memcmp(copied_body, requirement.canonical_body,
                 copied_bytes) == 0);

    CHECK(ucn_i_persistence_proof_retire(
              fixture.persistence, persistence_handle) == UCN_OK);
    CHECK(ucn_i_identity_expire(
              &fixture.identity, 2100U, 1U,
              &iteration, &iteration) == UCN_ERR_ARGUMENT);
    {
        uint16_t inspected;
        uint16_t fenced;
        CHECK(ucn_i_identity_expire(
                  &fixture.identity, 2100U, 1U,
                  &inspected, &fenced) == UCN_OK);
        CHECK(inspected == 1U && fenced == 1U);
    }
    CHECK(ucn_i_identity_retire_fenced(
              &fixture.identity, identity_handle) == UCN_OK);
    CHECK(ucn_i_identity_owner_destroy(&fixture.identity) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.persistence) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
    puts("identity persistence integration passed");
    return 0;
}
