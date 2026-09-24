#include "internal/ucn_service.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression_) do { if (!(expression_)) return __LINE__; } while (0)

typedef struct test_lock { int held; } test_lock_t;
static test_lock_t lock_state;
static ucn_i_service_owner_t owner;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0) return UCN_ERR_STATE;
    lock->held = 1;
    return UCN_OK;
}
static void lock_leave(void *context) { ((test_lock_t *)context)->held = 0; }

static void fill_principal(uint8_t value[16], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) value[index] = (uint8_t)(seed + index);
}

static ucn_i_service_operation_key_t operation_key(uint64_t operation_id)
{
    ucn_i_service_operation_key_t key;
    memset(&key, 0, sizeof(key));
    key.service.client.address = 1U;
    key.service.client.generation = 2U;
    fill_principal(key.service.client.principal, 0x10U);
    key.service.server.address = 3U;
    key.service.server.generation = 4U;
    fill_principal(key.service.server.principal, 0x30U);
    key.service.security.session_generation = 5U;
    key.service.security.key_generation = 6U;
    key.service.security.policy_generation = 7U;
    key.service.security.origin_security = 1U;
    key.service.security.acl_authorized = 1U;
    key.service.operation_id = operation_id;
    key.service.realm = 8U;
    key.service.service_id = 9U;
    key.service.opcode = 10U;
    memset(key.request_digest, 0xA5, sizeof(key.request_digest));
    return key;
}

static ucn_i_service_operation_durability_t durability(uint64_t tx,
                                                        uint64_t generation)
{
    ucn_i_service_operation_durability_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = 0x1111222233334444ULL;
    value.foundation_transaction_id = tx;
    value.expected_record_generation = generation;
    value.absolute_deadline_us = 1000000U;
    value.volatile_continuation.runtime_instance = 1U;
    value.volatile_continuation.owner_instance = 7U;
    value.volatile_continuation.slot = 1U;
    value.volatile_continuation.generation = (uint16_t)tx;
    value.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    value.domain_generation = 3U;
    value.schema_id = 0x701U;
    value.schema_version = 1U;
    return value;
}

static ucn_handle_t persistence_handle(uint16_t generation)
{
    ucn_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = 1U;
    handle.owner_instance = 99U;
    handle.slot = 0U;
    handle.generation = generation;
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    return handle;
}

static ucn_i_service_operation_proof_t proof_for(
    const ucn_i_service_operation_requirement_t *requirement,
    ucn_handle_t persistence,
    const uint8_t published_digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    ucn_i_service_operation_proof_t proof;
    memset(&proof, 0, sizeof(proof));
    proof.continuation = requirement->continuation;
    proof.persistence_handle = persistence;
    proof.domain_id = requirement->durability.domain_id;
    proof.foundation_transaction_id =
        requirement->durability.foundation_transaction_id;
    proof.record_generation =
        requirement->durability.expected_record_generation + 1U;
    proof.witness_generation = proof.record_generation;
    proof.runtime_instance = requirement->runtime_instance;
    proof.body_bytes = requirement->body_bytes;
    proof.domain_generation = requirement->durability.domain_generation;
    proof.persistence_owner_instance = persistence.owner_instance;
    proof.caller_owner_instance = requirement->caller_owner_instance;
    proof.schema_id = requirement->durability.schema_id;
    proof.schema_version = requirement->durability.schema_version;
    proof.operation_kind = requirement->operation_kind;
    memcpy(proof.body_digest, published_digest,
           sizeof(proof.body_digest));
    proof.next_phase = requirement->next_phase;
    return proof;
}

static int configure(void)
{
    ucn_i_service_config_t config;
    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&config, 0, sizeof(config));
    config.receipt_lifetime_us = 1000U;
    config.runtime_instance = 1U;
    config.owner_instance = 7U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &lock_state;
    config.state_lock.enter = lock_enter;
    config.state_lock.leave = lock_leave;
    return ucn_i_service_owner_init(&owner, &config) == UCN_OK ? 0 : __LINE__;
}

static int configure_other(ucn_i_service_owner_t *other,
                           test_lock_t *other_lock,
                           uint16_t owner_instance)
{
    ucn_i_service_config_t config;
    memset(other, 0, sizeof(*other));
    memset(other_lock, 0, sizeof(*other_lock));
    memset(&config, 0, sizeof(config));
    config.receipt_lifetime_us = 1000U;
    config.runtime_instance = 1U;
    config.owner_instance = owner_instance;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = other_lock;
    config.state_lock.enter = lock_enter;
    config.state_lock.leave = lock_leave;
    return ucn_i_service_owner_init(other, &config) == UCN_OK ? 0 : __LINE__;
}

static int persist_current(ucn_handle_t operation,
                           ucn_handle_t persistence,
                           uint8_t expected_phase)
{
    ucn_i_service_operation_requirement_t requirement;
    ucn_i_service_operation_proof_t proof;
    ucn_i_service_operation_view_t before;
    ucn_i_service_operation_view_t after;
    uint8_t published_digest[UCN_I_SERVICE_DIGEST_BYTES];

    CHECK(ucn_i_service_operation_requirement_get(
              &owner, operation, &requirement) == UCN_OK);
    CHECK(requirement.next_phase == expected_phase);
    CHECK(requirement.body_bytes >= 110U &&
          requirement.body_bytes <= UCN_I_SERVICE_OPERATION_BODY_BYTES);
    memcpy(published_digest, requirement.canonical_body_digest,
           sizeof(published_digest));
    published_digest[15] ^= UINT8_C(0x5A);
    CHECK(ucn_i_service_operation_bind_persistence(
              &owner, operation, persistence, published_digest) == UCN_OK);
    proof = proof_for(&requirement, persistence, published_digest);
    CHECK(ucn_i_service_operation_view(&owner, operation, &before) == UCN_OK);
    proof.body_digest[0] ^= 1U;
    CHECK(ucn_i_service_operation_accept_proof(
              &owner, operation, &proof) == UCN_ERR_STATE);
    CHECK(ucn_i_service_operation_view(&owner, operation, &after) == UCN_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    proof.body_digest[0] ^= 1U;
    CHECK(ucn_i_service_operation_accept_proof(
              &owner, operation, &proof) == UCN_OK);
    return 0;
}

static int test_committed_and_tombstone(void)
{
    ucn_i_service_operation_key_t key = operation_key(100U);
    ucn_i_service_operation_key_t conflict = key;
    ucn_i_service_operation_durability_t durable = durability(1U, 0U);
    ucn_i_service_operation_view_t view;
    ucn_i_service_result_t result;
    ucn_handle_t operation;
    ucn_handle_t duplicate;

    CHECK(ucn_i_service_operation_begin(&owner, &key, &durable,
                                         &operation) == UCN_OK);
    CHECK(ucn_i_service_operation_begin(&owner, &key, &durable,
                                         &duplicate) == UCN_OK);
    CHECK(memcmp(&operation, &duplicate, sizeof(operation)) == 0);
    conflict.request_digest[0] ^= 1U;
    CHECK(ucn_i_service_operation_begin(&owner, &conflict, &durable,
                                         &duplicate) == UCN_ERR_REPLAY);
    CHECK(persist_current(operation, persistence_handle(1U),
                          UCN_I_SERVICE_OPERATION_PREPARED) == 0);
    durable = durability(2U, 1U);
    CHECK(ucn_i_service_operation_prepare_executing(
              &owner, operation, &durable) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(2U),
                          UCN_I_SERVICE_OPERATION_EXECUTING) == 0);
    CHECK(ucn_i_service_operation_mark_executor_observed(
              &owner, operation) == UCN_OK);
    memset(&result, 0xCC, sizeof(result));
    result.application_result = UCN_OK;
    result.bytes = 2U;
    memcpy(result.payload, "ok", 2U);
    durable = durability(3U, 2U);
    CHECK(ucn_i_service_operation_prepare_terminal(
              &owner, operation, UCN_I_SERVICE_OPERATION_COMMITTED_RESULT,
              &result, &durable) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(3U),
                          UCN_I_SERVICE_OPERATION_COMMITTED_RESULT) == 0);
    CHECK(ucn_i_service_operation_view(&owner, operation, &view) == UCN_OK);
    CHECK(view.reply_ready == 1U && view.result.bytes == 2U &&
          view.result.payload[2] == 0U);
    durable = durability(4U, 3U);
    CHECK(ucn_i_service_operation_prepare_terminal(
              &owner, operation, UCN_I_SERVICE_OPERATION_TOMBSTONED,
              NULL, &durable) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(4U),
                          UCN_I_SERVICE_OPERATION_TOMBSTONED) == 0);
    CHECK(ucn_i_service_operation_retire(&owner, operation, true, true,
                                          key.service.operation_id) == UCN_OK);
    return 0;
}

static int test_abort_before_executor(void)
{
    ucn_i_service_operation_key_t key = operation_key(101U);
    ucn_i_service_operation_durability_t durable = durability(5U, 4U);
    ucn_i_service_operation_view_t view;
    ucn_handle_t operation;

    CHECK(ucn_i_service_operation_begin(&owner, &key, &durable,
                                         &operation) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(5U),
                          UCN_I_SERVICE_OPERATION_PREPARED) == 0);
    durable = durability(6U, 5U);
    CHECK(ucn_i_service_operation_prepare_terminal(
              &owner, operation, UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT,
              NULL, &durable) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(6U),
                          UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT) == 0);
    CHECK(ucn_i_service_operation_view(&owner, operation, &view) == UCN_OK);
    CHECK(view.reply_ready == 1U &&
          view.result.application_result == UCN_ERR_CANCELLED);
    durable = durability(7U, 6U);
    CHECK(ucn_i_service_operation_prepare_terminal(
              &owner, operation, UCN_I_SERVICE_OPERATION_TOMBSTONED,
              NULL, &durable) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(7U),
                          UCN_I_SERVICE_OPERATION_TOMBSTONED) == 0);
    CHECK(ucn_i_service_operation_retire(&owner, operation, true, true,
                                          key.service.operation_id) == UCN_OK);
    return 0;
}

static int test_executing_restart_forces_in_doubt(void)
{
    ucn_i_service_owner_t recovered;
    test_lock_t recovered_lock;
    ucn_i_service_operation_key_t key = operation_key(102U);
    ucn_i_service_operation_durability_t durable = durability(8U, 7U);
    ucn_i_service_operation_durability_t loaded;
    ucn_i_service_operation_requirement_t executing_requirement;
    ucn_i_service_operation_proof_t proof;
    ucn_i_service_operation_view_t view;
    ucn_handle_t operation;
    ucn_handle_t imported;
    ucn_handle_t persist;
    uint8_t published_digest[UCN_I_SERVICE_DIGEST_BYTES];

    CHECK(configure_other(&recovered, &recovered_lock, 8U) == 0);
    CHECK(ucn_i_service_operation_begin(&owner, &key, &durable,
                                         &operation) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(8U),
                          UCN_I_SERVICE_OPERATION_PREPARED) == 0);
    durable = durability(9U, 8U);
    CHECK(ucn_i_service_operation_prepare_executing(
              &owner, operation, &durable) == UCN_OK);
    CHECK(ucn_i_service_operation_requirement_get(
              &owner, operation, &executing_requirement) == UCN_OK);
    persist = persistence_handle(9U);
    memcpy(published_digest, executing_requirement.canonical_body_digest,
           sizeof(published_digest));
    published_digest[15] ^= UINT8_C(0x5A);
    CHECK(ucn_i_service_operation_bind_persistence(
              &owner, operation, persist, published_digest) == UCN_OK);
    proof = proof_for(&executing_requirement, persist, published_digest);
    CHECK(ucn_i_service_operation_accept_proof(
              &owner, operation, &proof) == UCN_OK);

    loaded = durability(9U, 9U);
    loaded.volatile_continuation.owner_instance = 8U;
    {
        uint8_t malformed[UCN_I_SERVICE_OPERATION_BODY_BYTES];
        ucn_handle_t sentinel;
        ucn_handle_t unchanged;
        memcpy(malformed, executing_requirement.body,
               executing_requirement.body_bytes);
        malformed[0] ^= 1U;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        unchanged = sentinel;
        CHECK(ucn_i_service_operation_import(
                  &recovered, malformed, executing_requirement.body_bytes,
                  &loaded, published_digest,
                  &unchanged) == UCN_ERR_MALFORMED);
        CHECK(memcmp(&sentinel, &unchanged, sizeof(sentinel)) == 0);
    }
    CHECK(ucn_i_service_operation_import(
              &recovered, executing_requirement.body,
              executing_requirement.body_bytes, &loaded, published_digest,
              &imported) == UCN_OK);
    CHECK(ucn_i_service_operation_view(&recovered, imported, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SERVICE_OPERATION_EXECUTING &&
          view.executor_observed == 1U);
    durable = durability(10U, 9U);
    durable.volatile_continuation.owner_instance = 8U;
    CHECK(ucn_i_service_operation_prepare_terminal(
              &recovered, imported,
              UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT,
              NULL, &durable) == UCN_ERR_STATE);
    CHECK(ucn_i_service_operation_prepare_terminal(
              &recovered, imported, UCN_I_SERVICE_OPERATION_IN_DOUBT,
              NULL, &durable) == UCN_OK);
    {
        ucn_i_service_operation_requirement_t requirement;
        ucn_i_service_operation_proof_t recovered_proof;
        ucn_handle_t recovered_persist = persistence_handle(10U);
        uint8_t recovered_digest[UCN_I_SERVICE_DIGEST_BYTES];
        CHECK(ucn_i_service_operation_requirement_get(
                  &recovered, imported, &requirement) == UCN_OK);
        memcpy(recovered_digest, requirement.canonical_body_digest,
               sizeof(recovered_digest));
        recovered_digest[15] ^= UINT8_C(0x5A);
        CHECK(ucn_i_service_operation_bind_persistence(
                  &recovered, imported, recovered_persist,
                  recovered_digest) == UCN_OK);
        recovered_proof = proof_for(&requirement, recovered_persist,
                                    recovered_digest);
        CHECK(ucn_i_service_operation_accept_proof(
                  &recovered, imported, &recovered_proof) == UCN_OK);
    }
    durable = durability(11U, 10U);
    durable.volatile_continuation.owner_instance = 8U;
    CHECK(ucn_i_service_operation_prepare_terminal(
              &recovered, imported, UCN_I_SERVICE_OPERATION_TOMBSTONED,
              NULL, &durable) == UCN_OK);
    {
        ucn_i_service_operation_requirement_t requirement;
        ucn_i_service_operation_proof_t recovered_proof;
        ucn_handle_t recovered_persist = persistence_handle(11U);
        uint8_t recovered_digest[UCN_I_SERVICE_DIGEST_BYTES];
        CHECK(ucn_i_service_operation_requirement_get(
                  &recovered, imported, &requirement) == UCN_OK);
        memcpy(recovered_digest, requirement.canonical_body_digest,
               sizeof(recovered_digest));
        recovered_digest[15] ^= UINT8_C(0x5A);
        CHECK(ucn_i_service_operation_bind_persistence(
                  &recovered, imported, recovered_persist,
                  recovered_digest) == UCN_OK);
        recovered_proof = proof_for(&requirement, recovered_persist,
                                    recovered_digest);
        CHECK(ucn_i_service_operation_accept_proof(
                  &recovered, imported, &recovered_proof) == UCN_OK);
    }
    CHECK(ucn_i_service_operation_retire(&recovered, imported,
                                          false, true, 102U) == UCN_ERR_STATE);
    CHECK(ucn_i_service_operation_retire(&recovered, imported,
                                          true, true, 102U) == UCN_OK);
    CHECK(ucn_i_service_owner_destroy(&recovered) == UCN_OK);

    CHECK(ucn_i_service_operation_mark_executor_observed(
              &owner, operation) == UCN_OK);
    durable = durability(10U, 9U);
    CHECK(ucn_i_service_operation_prepare_terminal(
              &owner, operation, UCN_I_SERVICE_OPERATION_IN_DOUBT,
              NULL, &durable) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(10U),
                          UCN_I_SERVICE_OPERATION_IN_DOUBT) == 0);
    durable = durability(11U, 10U);
    CHECK(ucn_i_service_operation_prepare_terminal(
              &owner, operation, UCN_I_SERVICE_OPERATION_TOMBSTONED,
              NULL, &durable) == UCN_OK);
    CHECK(persist_current(operation, persistence_handle(11U),
                          UCN_I_SERVICE_OPERATION_TOMBSTONED) == 0);
    CHECK(ucn_i_service_operation_retire(&owner, operation,
                                          true, true, 102U) == UCN_OK);
    return 0;
}

static ucn_i_service_operation_id_proof_t operation_id_proof_for(
    const ucn_i_service_operation_id_requirement_t *requirement,
    ucn_handle_t persistence,
    const uint8_t published_digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    ucn_i_service_operation_id_proof_t proof;
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence;
    proof.domain_id = requirement->durability.domain_id;
    proof.foundation_transaction_id =
        requirement->durability.foundation_transaction_id;
    proof.record_generation =
        requirement->durability.expected_record_generation + 1U;
    proof.witness_generation = proof.record_generation;
    proof.runtime_instance = requirement->runtime_instance;
    proof.body_bytes = UCN_I_SERVICE_OPERATION_ID_BODY_BYTES;
    proof.domain_generation = requirement->durability.domain_generation;
    proof.persistence_owner_instance = persistence.owner_instance;
    proof.caller_owner_instance = requirement->caller_owner_instance;
    proof.schema_id = requirement->durability.schema_id;
    proof.schema_version = requirement->durability.schema_version;
    proof.operation_kind = requirement->operation_kind;
    memcpy(proof.body_digest, published_digest, sizeof(proof.body_digest));
    return proof;
}

static int grant_operation_id_interval(
    ucn_i_service_owner_t *target,
    uint32_t parent_generation,
    const ucn_i_service_operation_durability_t *durable,
    ucn_handle_t persistence,
    ucn_i_service_operation_id_requirement_t *requirement_out,
    uint8_t published_digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    ucn_i_service_operation_id_proof_t proof;
    CHECK(ucn_i_service_operation_id_prepare_interval(
              target, parent_generation, durable, requirement_out) == UCN_OK);
    memcpy(published_digest, requirement_out->canonical_body_digest,
           UCN_I_SERVICE_DIGEST_BYTES);
    published_digest[15] ^= UINT8_C(0x3C);
    CHECK(ucn_i_service_operation_id_bind_persistence(
              target, persistence, published_digest) == UCN_OK);
    proof = operation_id_proof_for(requirement_out, persistence,
                                   published_digest);
    proof.body_digest[0] ^= 1U;
    CHECK(ucn_i_service_operation_id_accept_proof(target, &proof) ==
          UCN_ERR_STATE);
    proof.body_digest[0] ^= 1U;
    CHECK(ucn_i_service_operation_id_accept_proof(target, &proof) == UCN_OK);
    return 0;
}

static int test_operation_id_intervals(void)
{
    ucn_i_service_owner_t recovered;
    test_lock_t recovered_lock;
    ucn_i_service_operation_durability_t durable = durability(20U, 0U);
    ucn_i_service_operation_id_requirement_t first;
    ucn_i_service_operation_id_requirement_t second;
    ucn_i_service_operation_id_requirement_t third;
    ucn_i_service_operation_id_view_t view;
    uint8_t first_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t second_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t third_digest[UCN_I_SERVICE_DIGEST_BYTES];
    uint64_t operation_id = UINT64_C(0xA5A5A5A5A5A5A5A5);
    uint64_t index;

    CHECK(ucn_i_service_operation_id_take(&owner, &operation_id) ==
          UCN_ERR_STATE);
    CHECK(operation_id == UINT64_C(0xA5A5A5A5A5A5A5A5));
    CHECK(grant_operation_id_interval(&owner, 9U, &durable,
                                      persistence_handle(20U), &first,
                                      first_digest) == 0);
    CHECK(first.proposed_high_water ==
          UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE);
    for (index = 1U; index <= UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE;
         ++index) {
        CHECK(ucn_i_service_operation_id_take(&owner, &operation_id) == UCN_OK);
        CHECK(operation_id == index);
    }
    CHECK(ucn_i_service_operation_id_take(&owner, &operation_id) ==
          UCN_ERR_STATE);
    durable = durability(21U, 1U);
    CHECK(grant_operation_id_interval(&owner, 9U, &durable,
                                      persistence_handle(21U), &second,
                                      second_digest) == 0);
    CHECK(memcmp(second.expected_body_digest, first_digest,
                 sizeof(first_digest)) == 0);
    CHECK(ucn_i_service_operation_id_take(&owner, &operation_id) == UCN_OK);
    CHECK(operation_id ==
          (uint64_t)UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE + 1U);

    CHECK(configure_other(&recovered, &recovered_lock, 8U) == 0);
    durable = durability(21U, 2U);
    durable.volatile_continuation.owner_instance = 8U;
    CHECK(ucn_i_service_operation_id_import(
              &recovered, second.body, &durable, second_digest) == UCN_OK);
    CHECK(ucn_i_service_operation_id_view(&recovered, &view) == UCN_OK);
    CHECK(view.next_id == second.proposed_high_water + 1U &&
          view.reserved_through == second.proposed_high_water);
    CHECK(ucn_i_service_operation_id_take(&recovered, &operation_id) ==
          UCN_ERR_STATE);
    durable = durability(22U, 2U);
    durable.volatile_continuation.owner_instance = 8U;
    CHECK(grant_operation_id_interval(&recovered, 9U, &durable,
                                      persistence_handle(22U), &third,
                                      third_digest) == 0);
    CHECK(ucn_i_service_operation_id_take(&recovered, &operation_id) == UCN_OK);
    CHECK(operation_id == second.proposed_high_water + 1U);
    CHECK(ucn_i_service_owner_destroy(&recovered) == UCN_OK);
    return 0;
}

int main(void)
{
    int result = configure();
    if (result == 0) result = test_committed_and_tombstone();
    if (result == 0) result = test_abort_before_executor();
    if (result == 0) result = test_executing_restart_forces_in_doubt();
    if (result == 0) result = test_operation_id_intervals();
    if (result == 0 && ucn_i_service_owner_destroy(&owner) != UCN_OK) result = __LINE__;
    if (result != 0) return result;
    puts("operation tests passed");
    return 0;
}
