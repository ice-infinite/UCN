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

static ucn_i_transport_owner_t owner;
static test_lock_t test_lock;

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

static int start_owner(uint32_t runtime_instance)
{
    ucn_i_transport_config_t config;
    ucn_i_lock_ops_t ops;

    memset(&owner, 0, sizeof(owner));
    memset(&test_lock, 0, sizeof(test_lock));
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = &test_lock;
    ops.enter = lock_enter;
    ops.leave = lock_leave;
    memset(&config, 0, sizeof(config));
    config.reliable_lifetime_us = 1000U;
    config.reliable_retry_us = 100U;
    config.receipt_lifetime_us = 500U;
    config.runtime_instance = runtime_instance;
    config.owner_instance = 32U;
    config.reliable_max_attempts = 3U;
    config.state_lock = ops;
    return ucn_i_transport_owner_init(&owner, &config) == UCN_OK ? 0 : 1;
}

static void fill_principal(uint8_t principal[16], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        principal[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_transport_parent_context_t context(void)
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

static ucn_i_transport_transfer_facts_t c1_facts(
    const ucn_i_transport_parent_context_t *parent_context)
{
    ucn_i_transport_transfer_facts_t facts;

    memset(&facts, 0, sizeof(facts));
    facts.source = parent_context->source;
    facts.destination = parent_context->destination;
    facts.security.session_generation =
        parent_context->security_session_generation;
    facts.security.key_generation = parent_context->security_key_generation;
    facts.security.origin_security = parent_context->origin_security;
    facts.realm = parent_context->realm;
    facts.transport_policy_generation =
        parent_context->transport_policy_generation;
    facts.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_C1;
    return facts;
}

static void digest_for(const ucn_i_transport_parent_context_t *parent_context,
                       uint32_t high_water,
                       uint64_t foundation_transaction_id,
                       uint8_t digest[16])
{
    uint8_t body[UCN_I_TRANSPORT_PARENT_RECORD_BYTES];
    ucn_i_sha256_workspace_t workspace;
    memset(&workspace, 0, sizeof(workspace));
    (void)ucn_i_transport_parent_record_encode(
        parent_context, high_water, foundation_transaction_id, body);
    (void)ucn_i_sha256_128(body, sizeof(body), digest, &workspace);
}

static ucn_i_transport_parent_durability_base_t durability_for(
    const ucn_i_transport_parent_context_t *parent_context,
    uint32_t prior_high_water,
    uint64_t record_generation,
    uint64_t foundation_transaction_id,
    uint32_t runtime_instance)
{
    ucn_i_transport_parent_durability_base_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = 100U;
    value.prior_foundation_transaction_id = foundation_transaction_id;
    value.next_foundation_transaction_id = foundation_transaction_id + 1U;
    value.expected_record_generation = record_generation;
    value.absolute_deadline_us = 10000U;
    value.prior_transfer_high_water = prior_high_water;
    value.domain_generation = 3U;
    digest_for(parent_context, prior_high_water,
               foundation_transaction_id, value.expected_body_digest);
    value.volatile_continuation.runtime_instance = runtime_instance;
    value.volatile_continuation.owner_instance = 32U;
    value.volatile_continuation.slot = 1U;
    value.volatile_continuation.generation = 1U;
    value.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    return value;
}

static ucn_handle_t persistence_handle(uint32_t runtime_instance,
                                       uint16_t generation)
{
    ucn_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = runtime_instance;
    handle.owner_instance = 40U;
    handle.slot = 1U;
    handle.generation = generation;
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    return handle;
}

static ucn_i_transport_parent_proof_t proof_for(
    const ucn_i_transport_parent_requirement_t *requirement,
    ucn_handle_t persist,
    const uint8_t published_digest[16])
{
    ucn_i_transport_parent_proof_t proof;
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persist;
    proof.domain_id = requirement->domain_id;
    proof.record_generation = requirement->expected_record_generation + 1U;
    proof.foundation_transaction_id = requirement->foundation_transaction_id;
    proof.witness_generation = proof.record_generation;
    proof.transition_fingerprint = requirement->transition_fingerprint;
    proof.runtime_instance = requirement->runtime_instance;
    proof.body_bytes = requirement->body_bytes;
    proof.volatile_continuation = requirement->volatile_continuation;
    proof.persistence_owner_instance = persist.owner_instance;
    proof.caller_owner_instance = requirement->caller_owner_instance;
    proof.domain_generation = requirement->domain_generation;
    proof.schema_id = requirement->schema_id;
    proof.schema_version = requirement->schema_version;
    proof.operation_kind = requirement->operation_kind;
    memcpy(proof.body_digest, published_digest, 16U);
    return proof;
}

static ucn_i_transport_parent_failure_t failure_for(
    const ucn_i_transport_parent_requirement_t *requirement,
    ucn_handle_t persist)
{
    ucn_i_transport_parent_failure_t failure;

    memset(&failure, 0, sizeof(failure));
    failure.persistence_handle = persist;
    failure.volatile_continuation = requirement->volatile_continuation;
    failure.domain_id = requirement->domain_id;
    failure.foundation_transaction_id =
        requirement->foundation_transaction_id;
    failure.transition_fingerprint = requirement->transition_fingerprint;
    failure.runtime_instance = requirement->runtime_instance;
    failure.terminal_result = UCN_ERR_TIMEOUT;
    failure.persistence_owner_instance = persist.owner_instance;
    failure.caller_owner_instance = requirement->caller_owner_instance;
    failure.domain_generation = requirement->domain_generation;
    failure.schema_id = requirement->schema_id;
    failure.schema_version = requirement->schema_version;
    failure.operation_kind = requirement->operation_kind;
    failure.request_terminal = 1U;
    return failure;
}

static ucn_i_transport_transfer_setup_t c1_setup_for(
    const uint8_t *message,
    uint32_t message_bytes,
    uint32_t transfer_id)
{
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_sha256_workspace_t workspace;

    memset(&setup, 0, sizeof(setup));
    memset(&workspace, 0, sizeof(workspace));
    setup.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_C1;
    setup.parent_generation = 9U;
    setup.transfer_id = transfer_id;
    setup.service_id = 11U;
    setup.total_length = message_bytes;
    setup.delivery = UCN_DELIVERY_RELIABLE;
    setup.interaction = UCN_INTERACTION_ONE_WAY;
    setup.fragment_budget = 4U;
    setup.fragment_count = (uint16_t)((message_bytes + 3U) / 4U);
    setup.lifetime_ms = 1000U;
    (void)ucn_i_sha256_128(message, message_bytes, setup.message_digest,
                           &workspace);
    return setup;
}

static int test_parent_high_water(void)
{
    static const uint8_t message[] = {1U, 2U, 3U, 4U};
    ucn_i_transport_parent_context_t parent_context = context();
    ucn_i_transport_parent_durability_base_t durability;
    ucn_i_transport_parent_requirement_t requirement;
    ucn_i_transport_parent_proof_t proof;
    ucn_i_transport_parent_proof_t old_proof;
    ucn_i_transport_parent_view_t view;
    ucn_i_transport_parent_context_t decoded;
    uint8_t published_digest[16];
    uint8_t current_digest[16];
    uint32_t decoded_high_water = UINT32_MAX;
    uint64_t decoded_transaction = 0U;
    uint32_t transfer_id = UINT32_MAX;
    ucn_handle_t parent;
    ucn_handle_t persist;
    ucn_handle_t transfer;

    digest_for(&parent_context, 0U, 1U, current_digest);
    CHECK(ucn_i_transport_parent_import(
              &owner, &parent_context, 0U, 100U, 1U, 1U, 3U,
              current_digest,
              &parent) == UCN_OK);
    CHECK(ucn_i_transport_parent_view(&owner, parent, &view) == UCN_OK);
    CHECK(view.transfer_high_water == 0U);
    durability = durability_for(&parent_context, 0U, 1U, 1U, 31U);
    CHECK(ucn_i_transport_parent_prepare_next(
              &owner, parent, &durability, 100U) == UCN_OK);
    CHECK(ucn_i_transport_parent_requirement_get(
              &owner, parent, &requirement) == UCN_OK);
    CHECK(requirement.foundation_transaction_id == 2U);
    CHECK(requirement.expected_record_generation == 1U);
    CHECK(ucn_i_transport_parent_record_decode(
              requirement.canonical_body, &decoded,
              &decoded_high_water, &decoded_transaction) == UCN_OK);
    CHECK(decoded_high_water == 1U);
    CHECK(decoded_transaction == 2U);
    CHECK(memcmp(&decoded, &parent_context, sizeof(decoded)) == 0);
    digest_for(&parent_context, 1U, 2U, published_digest);
    persist = persistence_handle(31U, 1U);
    CHECK(ucn_i_transport_parent_bind_persistence(
              &owner, parent, persist, published_digest) == UCN_OK);
    proof = proof_for(&requirement, persist, published_digest);
    old_proof = proof;
    proof.record_generation = 99U;
    CHECK(ucn_i_transport_parent_activate_next(
              &owner, parent, &proof, 101U, &transfer_id) ==
          UCN_ERR_STATE);
    CHECK(transfer_id == UINT32_MAX);
    CHECK(ucn_i_transport_parent_view(&owner, parent, &view) == UCN_OK);
    CHECK(view.transfer_high_water == 0U);
    proof = old_proof;
    CHECK(ucn_i_transport_parent_activate_next(
              &owner, parent, &proof, 101U, &transfer_id) == UCN_OK);
    CHECK(transfer_id == 1U);
    CHECK(ucn_i_transport_parent_view(&owner, parent, &view) == UCN_OK);
    CHECK(view.record_generation == 2U);
    CHECK(view.foundation_transaction_id == 2U);
    CHECK(view.grant_available != 0U && view.granted_transfer_id == 1U);

    {
        ucn_i_transport_transfer_setup_t setup =
            c1_setup_for(message, sizeof(message), transfer_id);
        ucn_i_transport_transfer_facts_t facts = c1_facts(&parent_context);
        ucn_i_transport_transfer_facts_t wrong_facts = facts;
        ucn_i_transport_transfer_setup_t malformed = setup;
        ucn_i_transport_parent_durability_base_t blocked_durability =
            durability_for(&parent_context, 1U, 2U, 2U, 31U);
        ucn_handle_t unchanged;
        ucn_handle_t sentinel;
        ucn_handle_t receive;
        bool exact_duplicate = true;

        memset(&unchanged, 0xA5, sizeof(unchanged));
        memset(&sentinel, 0xA5, sizeof(sentinel));
        CHECK(ucn_i_transport_parent_prepare_next(
                  &owner, parent, &blocked_durability, 149U) ==
              UCN_ERR_STATE);
        malformed.message_digest[0] ^= 1U;
        CHECK(ucn_i_transport_c1_transfer_tx_begin(
                  &owner, parent, &malformed, &facts, message,
                  sizeof(message), 149U, &unchanged) == UCN_ERR_MALFORMED);
        CHECK(memcmp(&unchanged, &sentinel, sizeof(unchanged)) == 0);
        CHECK(ucn_i_transport_parent_view(&owner, parent, &view) == UCN_OK);
        CHECK(view.grant_available != 0U && view.granted_transfer_id == 1U);
        ++wrong_facts.realm;
        CHECK(ucn_i_transport_c1_transfer_tx_begin(
                  &owner, parent, &setup, &wrong_facts, message,
                  sizeof(message), 149U, &unchanged) == UCN_ERR_STATE);
        CHECK(memcmp(&unchanged, &sentinel, sizeof(unchanged)) == 0);
        CHECK(ucn_i_transport_parent_view(&owner, parent, &view) == UCN_OK);
        CHECK(view.grant_available != 0U && view.granted_transfer_id == 1U);
        CHECK(ucn_i_transport_transfer_tx_begin(
                  &owner, &setup, &facts, message, sizeof(message), 150U,
                  &unchanged) == UCN_ERR_STATE);
        CHECK(memcmp(&unchanged, &sentinel, sizeof(unchanged)) == 0);
        CHECK(ucn_i_transport_c1_transfer_tx_begin(
                  &owner, parent, &setup, &facts, message, sizeof(message),
                  150U, &transfer) == UCN_OK);
        CHECK(ucn_i_transport_parent_view(&owner, parent, &view) == UCN_OK);
        CHECK(view.grant_available == 0U &&
              view.granted_transfer_id == 0U);
        CHECK(ucn_i_transport_c1_transfer_tx_begin(
                  &owner, parent, &setup, &facts, message, sizeof(message),
                  151U, &unchanged) == UCN_ERR_STATE);
        CHECK(ucn_i_transport_transfer_rx_setup(
                  &owner, &setup, &facts, 151U, &unchanged,
                  &exact_duplicate) == UCN_ERR_STATE);
        CHECK(ucn_i_transport_c1_transfer_rx_setup(
                  &owner, parent, &setup, &facts, 151U, &receive,
                  &exact_duplicate) == UCN_OK);
        CHECK(!exact_duplicate);
        CHECK(ucn_i_transport_transfer_abort(&owner, receive) == UCN_OK);
        CHECK(ucn_i_transport_transfer_retire(&owner, receive) == UCN_OK);
        CHECK(ucn_i_transport_transfer_abort(&owner, transfer) == UCN_OK);
        CHECK(ucn_i_transport_transfer_retire(&owner, transfer) == UCN_OK);
    }

    durability = durability_for(&parent_context, 1U, 2U, 2U, 31U);
    CHECK(ucn_i_transport_parent_prepare_next(
              &owner, parent, &durability, 200U) == UCN_OK);
    CHECK(ucn_i_transport_parent_requirement_get(
              &owner, parent, &requirement) == UCN_OK);
    digest_for(&parent_context, 2U, 3U, published_digest);
    persist = persistence_handle(31U, 2U);
    CHECK(ucn_i_transport_parent_bind_persistence(
              &owner, parent, persist, published_digest) == UCN_OK);
    proof = proof_for(&requirement, persist, published_digest);
    CHECK(ucn_i_transport_parent_activate_next(
              &owner, parent, &proof, 201U, &transfer_id) == UCN_OK);
    CHECK(transfer_id == 2U);
    CHECK(ucn_i_transport_parent_discard_grant(
              &owner, parent, transfer_id) == UCN_OK);
    return 0;
}

static int test_restart_rejects_old_proof(void)
{
    ucn_i_transport_parent_context_t parent_context = context();
    ucn_i_transport_parent_durability_base_t durability;
    ucn_i_transport_parent_requirement_t requirement;
    ucn_i_transport_parent_proof_t proof;
    uint8_t published_digest[16];
    uint8_t current_digest[16];
    uint32_t transfer_id = 0U;
    ucn_handle_t parent;
    ucn_handle_t persist;

    CHECK(ucn_i_transport_owner_destroy(&owner) == UCN_OK);
    CHECK(start_owner(31U) == 0);
    digest_for(&parent_context, 2U, 3U, current_digest);
    CHECK(ucn_i_transport_parent_import(
              &owner, &parent_context, 2U, 100U, 3U, 3U, 3U,
              current_digest,
              &parent) == UCN_OK);
    durability = durability_for(&parent_context, 2U, 3U, 3U, 31U);
    CHECK(ucn_i_transport_parent_prepare_next(
              &owner, parent, &durability, 300U) == UCN_OK);
    CHECK(ucn_i_transport_parent_requirement_get(
              &owner, parent, &requirement) == UCN_OK);
    digest_for(&parent_context, 3U, 4U, published_digest);
    persist = persistence_handle(31U, 3U);
    CHECK(ucn_i_transport_parent_bind_persistence(
              &owner, parent, persist, published_digest) == UCN_OK);
    proof = proof_for(&requirement, persist, published_digest);
    proof.foundation_transaction_id = 2U;
    CHECK(ucn_i_transport_parent_activate_next(
              &owner, parent, &proof, 301U, &transfer_id) ==
          UCN_ERR_STATE);
    CHECK(transfer_id == 0U);
    proof = proof_for(&requirement, persist, published_digest);
    CHECK(ucn_i_transport_parent_activate_next(
              &owner, parent, &proof, 301U, &transfer_id) == UCN_OK);
    CHECK(transfer_id == 3U);
    CHECK(ucn_i_transport_parent_discard_grant(
              &owner, parent, transfer_id) == UCN_OK);
    return 0;
}

static int test_abort_and_terminal_failure_restore_active(void)
{
    ucn_i_transport_parent_context_t parent_context = context();
    ucn_i_transport_parent_durability_base_t durability;
    ucn_i_transport_parent_requirement_t requirement;
    ucn_i_transport_parent_failure_t failure;
    ucn_i_transport_parent_view_t view;
    uint8_t published_digest[16];
    ucn_handle_t parent;
    ucn_handle_t persist;

    digest_for(&parent_context, 3U, 4U, published_digest);
    CHECK(ucn_i_transport_parent_import(
              &owner, &parent_context, 3U, 100U, 4U, 4U, 3U,
              published_digest, &parent) == UCN_OK);
    durability = durability_for(&parent_context, 3U, 4U, 4U, 31U);
    CHECK(ucn_i_transport_parent_prepare_next(
              &owner, parent, &durability, 400U) == UCN_OK);
    CHECK(ucn_i_transport_parent_abort_unsubmitted(&owner, parent) == UCN_OK);
    CHECK(ucn_i_transport_parent_view(&owner, parent, &view) == UCN_OK);
    CHECK(view.active != 0U && view.transfer_high_water == 3U &&
          view.record_generation == 4U &&
          view.foundation_transaction_id == 4U);

    CHECK(ucn_i_transport_parent_prepare_next(
              &owner, parent, &durability, 401U) == UCN_OK);
    CHECK(ucn_i_transport_parent_requirement_get(
              &owner, parent, &requirement) == UCN_OK);
    digest_for(&parent_context, 4U, 5U, published_digest);
    persist = persistence_handle(31U, 4U);
    CHECK(ucn_i_transport_parent_bind_persistence(
              &owner, parent, persist, published_digest) == UCN_OK);
    failure = failure_for(&requirement, persist);
    failure.foundation_transaction_id--;
    CHECK(ucn_i_transport_parent_fail_persistence(
              &owner, parent, &failure) == UCN_ERR_STATE);
    CHECK(ucn_i_transport_parent_requirement_get(
              &owner, parent, &requirement) == UCN_OK);
    failure = failure_for(&requirement, persist);
    CHECK(ucn_i_transport_parent_fail_persistence(
              &owner, parent, &failure) == UCN_OK);
    CHECK(ucn_i_transport_parent_view(&owner, parent, &view) == UCN_OK);
    CHECK(view.active != 0U && view.transfer_high_water == 3U &&
          view.record_generation == 4U &&
          view.foundation_transaction_id == 4U);
    CHECK(ucn_i_transport_parent_requirement_get(
              &owner, parent, &requirement) == UCN_ERR_STATE);
    return 0;
}

static int test_decode_alias_and_generation_exhaustion(void)
{
    typedef union aligned_output {
        ucn_i_transport_parent_context_t context;
        uint64_t transaction;
        uint32_t high_water;
    } aligned_output_t;
    ucn_i_transport_parent_context_t parent_context = context();
    aligned_output_t output;
    uint8_t body[UCN_I_TRANSPORT_PARENT_RECORD_BYTES];
    uint8_t published_digest[16];
    ucn_handle_t handle;
    size_t index;

    CHECK(ucn_i_transport_parent_record_encode(
              &parent_context, 3U, 4U, body) == UCN_OK);
    memset(&output, 0xA5, sizeof(output));
    CHECK(ucn_i_transport_parent_record_decode(
              body, &output.context, &output.high_water,
              &output.transaction) == UCN_ERR_ARGUMENT);

    CHECK(ucn_i_transport_owner_destroy(&owner) == UCN_OK);
    CHECK(start_owner(31U) == 0);
    for (index = 0U; index < UCN_I_TRANSPORT_PARENT_COUNT; ++index) {
        owner.parents[index].generation = UINT16_MAX;
    }
    digest_for(&parent_context, 3U, 4U, published_digest);
    memset(&handle, 0xA5, sizeof(handle));
    CHECK(ucn_i_transport_parent_import(
              &owner, &parent_context, 3U, 100U, 4U, 4U, 3U,
              published_digest, &handle) == UCN_ERR_EXHAUSTED);
    {
        ucn_handle_t sentinel;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        CHECK(memcmp(&handle, &sentinel, sizeof(handle)) == 0);
    }
    return 0;
}

int main(void)
{
    CHECK(start_owner(31U) == 0);
    CHECK(test_parent_high_water() == 0);
    CHECK(test_restart_rejects_old_proof() == 0);
    CHECK(test_abort_and_terminal_failure_restore_active() == 0);
    CHECK(test_decode_alias_and_generation_exhaustion() == 0);
    CHECK(ucn_i_transport_owner_destroy(&owner) == UCN_OK);
    puts("transport parent tests passed");
    return 0;
}
