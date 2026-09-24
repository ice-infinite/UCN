#include "internal/ucn_identity.h"

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

static ucn_i_lock_ops_t lock_ops(test_lock_t *lock)
{
    ucn_i_lock_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = lock_enter;
    ops.leave = lock_leave;
    return ops;
}

static void fill(uint8_t *bytes, size_t length, uint8_t value)
{
    memset(bytes, value, length);
}

static ucn_i_identity_binding_issue_t make_issue(uint32_t generation)
{
    ucn_i_identity_binding_issue_t issue;

    memset(&issue, 0, sizeof(issue));
    issue.transaction_id = UINT64_C(0x1112131415161718);
    issue.challenge_started_local_us = 100U;
    issue.challenge_deadline_us = 5100U;
    issue.lease_duration_us = 2000U;
    issue.authority_lease_sequence = 6U;
    issue.runtime_instance = 10U;
    issue.realm_id = 42U;
    issue.address = 9U;
    issue.binding_generation = generation;
    issue.authority_generation = 3U;
    issue.link_generation = 9U;
    issue.admission_generation = 1U;
    issue.admission_owner_instance = 20U;
    issue.address_width = 2U;
    issue.mode = UCN_I_IDENTITY_ADDRESS_LEASED;
    fill(issue.principal, sizeof(issue.principal), 0x44U);
    fill(issue.authority_principal,
         sizeof(issue.authority_principal), 0x22U);
    fill(issue.lease_id, sizeof(issue.lease_id),
         (uint8_t)(0x50U + generation));
    fill(issue.transcript_digest, sizeof(issue.transcript_digest), 0x77U);
    return issue;
}

static ucn_i_identity_authority_view_t make_authority(void)
{
    ucn_i_identity_authority_view_t view;

    memset(&view, 0, sizeof(view));
    view.lease_sequence = 6U;
    view.local_deadline_us = 10000U;
    view.record_generation = 2U;
    view.foundation_transaction_id = 2U;
    view.witness_generation = 2U;
    view.runtime_instance = 10U;
    view.realm_id = 42U;
    view.authority_generation = 3U;
    view.authority_owner_instance = 19U;
    view.persistence_owner_instance = 22U;
    view.schema_id = UCN_I_IDENTITY_AUTHORITY_SCHEMA_ID;
    view.schema_version = 1U;
    fill(view.principal, sizeof(view.principal), 0x22U);
    fill(view.body_digest, sizeof(view.body_digest), 0x33U);
    return view;
}

static ucn_i_identity_durability_base_t make_factory_base(uint64_t domain)
{
    ucn_i_identity_durability_base_t base;

    memset(&base, 0, sizeof(base));
    base.domain_id = domain;
    base.next_transaction_id = 1U;
    base.absolute_deadline_us = 4000U;
    base.domain_generation = 1U;
    base.volatile_continuation.runtime_instance = 10U;
    base.volatile_continuation.owner_instance = 21U;
    base.volatile_continuation.slot = 1U;
    base.volatile_continuation.generation = 1U;
    base.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    return base;
}

static ucn_handle_t make_persistence_handle(uint16_t slot,
                                             uint16_t generation)
{
    ucn_handle_t handle;

    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = 10U;
    handle.owner_instance = 22U;
    handle.slot = slot;
    handle.generation = generation;
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    return handle;
}

static ucn_i_identity_durability_proof_t make_proof(
    const ucn_i_identity_requirement_view_t *requirement,
    ucn_handle_t persistence,
    const uint8_t digest[16])
{
    ucn_i_identity_durability_proof_t proof;

    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence;
    proof.domain_id = requirement->domain_id;
    proof.record_generation = requirement->expected_record_generation + 1U;
    proof.foundation_transaction_id = requirement->foundation_transaction_id;
    proof.witness_generation = proof.record_generation;
    proof.transition_fingerprint = requirement->transition_fingerprint;
    proof.runtime_instance = requirement->runtime_instance;
    proof.body_bytes = requirement->body_bytes;
    proof.volatile_continuation = requirement->volatile_continuation;
    proof.persistence_owner_instance = persistence.owner_instance;
    proof.caller_owner_instance = requirement->caller_owner_instance;
    proof.domain_generation = requirement->domain_generation;
    proof.schema_id = requirement->schema_id;
    proof.schema_version = requirement->schema_version;
    proof.operation_kind = requirement->operation_kind;
    memcpy(proof.body_digest, digest, 16U);
    return proof;
}

int main(void)
{
    static const uint8_t binding_golden[96] = {
        0x55, 0x43, 0x36, 0x42, 0x00, 0x01, 0x01, 0x02,
        0x00, 0x00, 0x00, 0x2A, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01,
        0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
        0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
        0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
        0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
        0x00, 0x00, 0x00, 0x03,
        0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51,
        0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xD0,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x00
    };
    ucn_i_identity_owner_t owner;
    ucn_i_identity_config_t config;
    ucn_i_identity_domain_rule_t rules[UCN_BINDING_COUNT];
    ucn_i_identity_binding_issue_t issue = make_issue(1U);
    ucn_i_identity_binding_issue_t decoded;
    ucn_i_identity_authority_view_t authority = make_authority();
    ucn_i_identity_durability_base_t base = make_factory_base(0x9001U);
    ucn_i_identity_handle_t handle;
    ucn_i_identity_handle_t old_handle;
    ucn_i_identity_requirement_view_t requirement;
    ucn_i_identity_durability_proof_t proof;
    ucn_i_identity_binding_view_t view;
    ucn_i_identity_binding_view_t sentinel;
    ucn_i_identity_binding_view_t sentinel_before;
    ucn_handle_t persistence = make_persistence_handle(0U, 1U);
    test_lock_t state_lock = {0};
    uint8_t published_digest[16];
    uint8_t bytes[96];
    uint8_t before[96];
    uint16_t inspected;
    uint16_t fenced;
    uint16_t index;

    memset(&owner, 0, sizeof(owner));
    memset(&config, 0, sizeof(config));
    memset(&decoded, 0, sizeof(decoded));
    memset(&handle, 0, sizeof(handle));
    memset(&requirement, 0, sizeof(requirement));
    memset(&view, 0, sizeof(view));
    fill(published_digest, sizeof(published_digest), 0x66U);
    for (index = 0U; index < UCN_BINDING_COUNT; ++index) {
        rules[index].domain_id = UINT64_C(0x9001) + index;
    }
    config.struct_size = sizeof(config);
    config.api_version = UCN_API_VERSION;
    config.runtime_instance = 10U;
    config.realm_id = 42U;
    config.owner_instance = 21U;
    config.authority_owner_instance = 19U;
    config.persistence_business_owner_instance = 21U;
    config.persistence_owner_instance = 22U;
    config.address_width = 2U;
    config.domain_rules = rules;
    config.domain_rule_count = UCN_BINDING_COUNT;
    config.state_lock = lock_ops(&state_lock);

    CHECK(ucn_i_identity_binding_record_encode(&issue, bytes) == UCN_OK);
    CHECK(memcmp(bytes, binding_golden, sizeof(bytes)) == 0);
    CHECK(ucn_i_identity_binding_record_decode(
              binding_golden, &decoded) == UCN_OK);
    CHECK(decoded.realm_id == issue.realm_id);
    CHECK(decoded.address == issue.address);
    CHECK(decoded.binding_generation == issue.binding_generation);
    CHECK(memcmp(decoded.principal, issue.principal,
                 sizeof(issue.principal)) == 0);
    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    issue.binding_generation = 0U;
    CHECK(ucn_i_identity_binding_record_encode(
              &issue, bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    issue = make_issue(1U);

    CHECK(ucn_i_identity_owner_init(&owner, &config) == UCN_OK);
    {
        ucn_i_identity_binding_issue_t future = issue;
        ucn_i_identity_handle_t output;
        ucn_i_identity_handle_t output_before;
        future.challenge_started_local_us = 200U;
        memset(&output, 0xA5, sizeof(output));
        output_before = output;
        CHECK(ucn_i_identity_prepare_binding(
                  &owner, &future, &authority, &base, 150U,
                  &output) == UCN_ERR_STATE);
        CHECK(memcmp(&output, &output_before, sizeof(output)) == 0);
    }
    CHECK(ucn_i_identity_prepare_binding(
              &owner, &issue, &authority, &base, 150U, &handle) == UCN_OK);
    CHECK(ucn_i_identity_requirement_get(
              &owner, handle, &requirement) == UCN_OK);
    CHECK(requirement.body_bytes == 96U);
    CHECK(requirement.schema_id == UCN_I_IDENTITY_BINDING_SCHEMA_ID);
    CHECK(memcmp(requirement.canonical_body, binding_golden, 96U) == 0);
    CHECK(ucn_i_identity_bind_persistence(
              &owner, handle, persistence, published_digest) == UCN_OK);
    proof = make_proof(&requirement, persistence, published_digest);
    memset(&sentinel, 0xA5, sizeof(sentinel));
    sentinel_before = sentinel;
    proof.body_digest[0] ^= 1U;
    CHECK(ucn_i_identity_activate_binding(
              &owner, handle, &proof, &authority, 200U,
              &sentinel) == UCN_ERR_STATE);
    CHECK(memcmp(&sentinel, &sentinel_before, sizeof(sentinel)) == 0);
    proof.body_digest[0] ^= 1U;
    {
        ucn_i_identity_authority_view_t wrong = authority;
        wrong.authority_generation++;
        CHECK(ucn_i_identity_activate_binding(
                  &owner, handle, &proof, &wrong, 200U,
                  &sentinel) == UCN_ERR_STATE);
        CHECK(memcmp(&sentinel, &sentinel_before, sizeof(sentinel)) == 0);
    }
    CHECK(ucn_i_identity_activate_binding(
              &owner, handle, &proof, &authority, 200U, &view) == UCN_OK);
    CHECK(view.local_deadline_us == 2100U);
    CHECK(view.binding_generation == 1U);
    CHECK(view.schema_id == UCN_I_IDENTITY_BINDING_SCHEMA_ID);
    CHECK(ucn_i_identity_binding_get(
              &owner, handle, 2099U, &view) == UCN_OK);
    CHECK(ucn_i_identity_binding_get(
              &owner, handle, 2100U, &view) == UCN_ERR_ACCESS);

    /* A delayed caller cannot replay the same durable proof with a fresh
     * local lease origin: the start/deadline are part of Owner state. */
    memset(&sentinel, 0xA5, sizeof(sentinel));
    sentinel_before = sentinel;
    CHECK(ucn_i_identity_activate_binding(
              &owner, handle, &proof, &authority, 500U,
              &sentinel) == UCN_ERR_STATE);
    CHECK(memcmp(&sentinel, &sentinel_before, sizeof(sentinel)) == 0);

    /* Prepare checked-next on the same durable Domain. Transaction continuity
     * is exact, and a bound Persistence failure must be acknowledged with the
     * exact handle before the previous active lease is restored. */
    old_handle = handle;
    issue = make_issue(2U);
    issue.challenge_started_local_us = 300U;
    issue.challenge_deadline_us = 2300U;
    issue.lease_duration_us = 1800U;
    base.next_transaction_id = 2U;
    base.prior_foundation_transaction_id = 1U;
    base.expected_record_generation = 1U;
    base.absolute_deadline_us = 2000U;
    base.prior_address = 9U;
    base.prior_binding_generation = 1U;
    base.prior_authority_lease_sequence = 6U;
    base.prior_address_width = 2U;
    fill(base.prior_principal, sizeof(base.prior_principal), 0x44U);
    fill(base.prior_lease_id, sizeof(base.prior_lease_id), 0x51U);
    fill(base.expected_body_digest, sizeof(base.expected_body_digest), 0x66U);
    {
        ucn_i_identity_handle_t output;
        ucn_i_identity_handle_t output_before;
        base.next_transaction_id = 3U;
        memset(&output, 0xA5, sizeof(output));
        output_before = output;
        CHECK(ucn_i_identity_prepare_binding(
                  &owner, &issue, &authority, &base, 350U,
                  &output) == UCN_ERR_STATE);
        CHECK(memcmp(&output, &output_before, sizeof(output)) == 0);
        base.next_transaction_id = 2U;
    }
    CHECK(ucn_i_identity_prepare_binding(
              &owner, &issue, &authority, &base, 350U, &handle) == UCN_OK);
    CHECK(handle.slot_generation > old_handle.slot_generation);
    persistence = make_persistence_handle(0U, 2U);
    CHECK(ucn_i_identity_bind_persistence(
              &owner, handle, persistence, published_digest) == UCN_OK);
    CHECK(ucn_i_identity_expire(
              &owner, 2000U, UCN_BINDING_COUNT,
              &inspected, &fenced) == UCN_OK);
    CHECK(fenced == 1U);
    CHECK(ucn_i_identity_retire_fenced(&owner, handle) == UCN_ERR_STATE);
    {
        ucn_i_identity_persistence_failure_t failure;
        ucn_i_identity_slot_t slot_before;

        memset(&failure, 0, sizeof(failure));
        failure.persistence_handle = persistence;
        failure.volatile_continuation = base.volatile_continuation;
        failure.domain_id = base.domain_id;
        failure.foundation_transaction_id = base.next_transaction_id;
        failure.transition_fingerprint =
            owner.bindings[handle.slot].transition_fingerprint;
        failure.runtime_instance = 10U;
        failure.terminal_result = UCN_ERR_TIMEOUT;
        failure.persistence_owner_instance = 22U;
        failure.caller_owner_instance = 21U;
        failure.domain_generation = base.domain_generation;
        failure.schema_id = UCN_I_IDENTITY_BINDING_SCHEMA_ID;
        failure.schema_version = UCN_I_IDENTITY_BINDING_RECORD_SCHEMA;
        failure.operation_kind = UCN_I_IDENTITY_BINDING_OPERATION_KIND;
        failure.request_state =
            UCN_I_IDENTITY_PERSISTENCE_REQUEST_FAILED;
        failure.persistence_handle.generation++;
        CHECK(ucn_i_identity_fail_persistence(
                  &owner, handle, &failure) == UCN_ERR_STATE);
        failure.persistence_handle = persistence;
        failure.terminal_result = UCN_OK;
        CHECK(ucn_i_identity_fail_persistence(
                  &owner, handle, &failure) == UCN_ERR_STATE);
        failure.terminal_result = UCN_ERR_TIMEOUT;

        slot_before = owner.bindings[handle.slot];
        failure.transition_fingerprint ^= UINT64_C(1);
        CHECK(ucn_i_identity_fail_persistence(
                  &owner, handle, &failure) == UCN_ERR_STATE);
        CHECK(memcmp(&owner.bindings[handle.slot], &slot_before,
                     sizeof(slot_before)) == 0);
        failure.transition_fingerprint ^= UINT64_C(1);

        failure.request_state ^= UINT8_C(1);
        CHECK(ucn_i_identity_fail_persistence(
                  &owner, handle, &failure) == UCN_ERR_STATE);
        CHECK(memcmp(&owner.bindings[handle.slot], &slot_before,
                     sizeof(slot_before)) == 0);
        failure.request_state ^= UINT8_C(1);

        failure.domain_id++;
        CHECK(ucn_i_identity_fail_persistence(
                  &owner, handle, &failure) == UCN_ERR_STATE);
        CHECK(memcmp(&owner.bindings[handle.slot], &slot_before,
                     sizeof(slot_before)) == 0);
        failure.domain_id--;

        failure.foundation_transaction_id++;
        CHECK(ucn_i_identity_fail_persistence(
                  &owner, handle, &failure) == UCN_ERR_STATE);
        CHECK(memcmp(&owner.bindings[handle.slot], &slot_before,
                     sizeof(slot_before)) == 0);
        failure.foundation_transaction_id--;

        CHECK(ucn_i_identity_fail_persistence(
                  &owner, handle, &failure) == UCN_OK);
    }
    CHECK(ucn_i_identity_binding_get(
              &owner, old_handle, 2050U, &view) == UCN_OK);

    CHECK(ucn_i_identity_expire(
              &owner, 2100U, UCN_BINDING_COUNT,
              &inspected, &fenced) == UCN_OK);
    CHECK(inspected == UCN_BINDING_COUNT);
    CHECK(fenced == 1U);
    CHECK(ucn_i_identity_retire_fenced(&owner, old_handle) == UCN_OK);
    CHECK(ucn_i_identity_owner_destroy(&owner) == UCN_OK);

    /* Factory generation one may use every configured slot. A duplicate
     * request against a busy durable Domain is rejected and never evicts the
     * existing candidate. */
    CHECK(ucn_i_identity_owner_init(&owner, &config) == UCN_OK);
    {
        ucn_i_identity_handle_t handles[UCN_BINDING_COUNT];
        for (index = 0U; index < UCN_BINDING_COUNT; ++index) {
            ucn_i_identity_binding_issue_t item = make_issue(1U);
            ucn_i_identity_durability_base_t item_base =
                make_factory_base(rules[index].domain_id);

            item.address = (uint32_t)index + 10U;
            item.transaction_id += index;
            item.admission_generation += index;
            memset(item.principal, (uint8_t)(index + 1U),
                   sizeof(item.principal));
            memset(item.lease_id, (uint8_t)(0x70U + index),
                   sizeof(item.lease_id));
            CHECK(ucn_i_identity_prepare_binding(
                      &owner, &item, &authority, &item_base, 150U,
                      &handles[index]) == UCN_OK);
        }
        {
            ucn_i_identity_binding_issue_t extra = make_issue(1U);
            ucn_i_identity_durability_base_t extra_base =
                make_factory_base(rules[0].domain_id);
            ucn_i_identity_handle_t output;
            ucn_i_identity_handle_t output_before;
            extra.address = 100U;
            fill(extra.principal, sizeof(extra.principal), 0x55U);
            memset(&output, 0xA5, sizeof(output));
            output_before = output;
            CHECK(ucn_i_identity_prepare_binding(
                      &owner, &extra, &authority, &extra_base, 150U,
                      &output) == UCN_ERR_STATE);
            CHECK(memcmp(&output, &output_before, sizeof(output)) == 0);
        }
        for (index = 0U; index < UCN_BINDING_COUNT; ++index) {
            CHECK(ucn_i_identity_abort_unsubmitted(
                      &owner, handles[index]) == UCN_OK);
        }
    }
    CHECK(ucn_i_identity_owner_destroy(&owner) == UCN_OK);
    puts("identity binding tests passed");
    return 0;
}
