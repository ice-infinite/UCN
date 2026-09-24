#include "internal/ucn_cluster.h"

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
static test_lock_t lock_state;
static ucn_i_cluster_owner_t owner;
static uint64_t record_generation;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context) { ((test_lock_t *)context)->held = 0U; }

static ucn_i_lock_ops_t lock_ops(void)
{
    ucn_i_lock_ops_t lock;
    memset(&lock, 0, sizeof(lock));
    lock.struct_size = sizeof(lock);
    lock.api_version = UCN_I_LOCK_OPS_VERSION;
    lock.context = &lock_state;
    lock.enter = lock_enter;
    lock.leave = lock_leave;
    return lock;
}

static void principal(uint8_t out[UCN_I_CLUSTER_PRINCIPAL_BYTES],
                      uint8_t seed)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_PRINCIPAL_BYTES; ++index) {
        out[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_cluster_epoch_t epoch(uint32_t cluster_id, uint8_t seed,
                                    uint32_t binding)
{
    ucn_i_cluster_epoch_t value;
    memset(&value, 0, sizeof(value));
    value.cluster_id = cluster_id;
    value.term = 1U;
    value.head_binding_generation = binding;
    principal(value.head_principal, seed);
    return value;
}

static ucn_i_cluster_config_view_t config(uint32_t id, uint32_t generation,
                                           uint8_t seed, uint32_t binding)
{
    ucn_i_cluster_config_view_t value;
    memset(&value, 0, sizeof(value));
    value.config_id = id;
    value.generation = generation;
    value.member_count = 1U;
    value.members[0].binding_generation = binding;
    value.members[0].flags = UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
                             UCN_I_CLUSTER_MEMBER_FLAG_VOTER;
    principal(value.members[0].principal, seed);
    return value;
}

static ucn_i_cluster_durability_t durability(uint64_t transaction)
{
    ucn_i_cluster_durability_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = 821U;
    value.foundation_transaction_id = transaction;
    value.expected_record_generation = record_generation;
    value.absolute_deadline_us = 9000U;
    value.persistence_domain_generation = 3U;
    value.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    value.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    return value;
}

static int publish(ucn_i_cluster_requirement_t *requirement,
                   uint64_t now_us)
{
    ucn_handle_t handle;
    ucn_i_cluster_proof_t proof;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = 1U;
    handle.owner_instance = 9U;
    handle.slot = 1U;
    handle.generation = (uint16_t)requirement->durability.foundation_transaction_id;
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    CHECK(ucn_i_cluster_bind_persistence(
              &owner, handle, requirement->canonical_body_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = handle;
    proof.domain_id = requirement->durability.domain_id;
    proof.foundation_transaction_id =
        requirement->durability.foundation_transaction_id;
    proof.record_generation = ++record_generation;
    proof.witness_generation = record_generation;
    proof.runtime_instance = 1U;
    proof.body_bytes = UCN_I_CLUSTER_RECORD_BYTES;
    proof.persistence_domain_generation = 3U;
    proof.persistence_owner_instance = 9U;
    proof.caller_owner_instance = 8U;
    proof.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    proof.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    proof.operation_kind = requirement->operation_kind;
    memcpy(proof.body_digest, requirement->canonical_body_digest,
           sizeof(proof.body_digest));
    CHECK(ucn_i_cluster_accept_proof(&owner, &proof, now_us) == UCN_OK);
    return 0;
}

static int setup_head(void)
{
    ucn_i_cluster_config_t owner_config;
    ucn_i_cluster_epoch_t active = epoch(4U, 0x10U, 7U);
    ucn_i_cluster_config_view_t stable = config(5U, 1U, 0x10U, 7U);
    ucn_i_cluster_member_fact_t fact;
    ucn_i_cluster_durability_t durable;
    ucn_i_cluster_requirement_t requirement;
    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&owner_config, 0, sizeof(owner_config));
    owner_config.runtime_instance = 1U;
    owner_config.owner_instance = 8U;
    principal(owner_config.local_principal, 0x10U);
    owner_config.state_lock = lock_ops();
    CHECK(ucn_i_cluster_owner_init(&owner, &owner_config) == UCN_OK);
    record_generation = 0U;
    durable = durability(1U);
    CHECK(ucn_i_cluster_create_prepare(
              &owner, &active, &stable, UCN_I_CLUSTER_HEAD,
              &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 10U) == 0);
    memset(&fact, 0, sizeof(fact));
    fact.lease_deadline_us = 8000U;
    fact.capability_deadline_us = 8000U;
    fact.binding_generation = 7U;
    fact.session_generation = 2U;
    fact.capability_generation = 3U;
    fact.route_generation = 4U;
    fact.link_generation = 5U;
    fact.link_id = 1U;
    fact.authenticated = 1U;
    fact.current = 1U;
    principal(fact.principal, 0x10U);
    memset(fact.capability_digest, 0xA5,
           sizeof(fact.capability_digest));
    CHECK(ucn_i_cluster_member_observe(&owner, &fact, 20U) == UCN_OK);
    return 0;
}

static int test_rekey_and_merge_lineage(void)
{
    ucn_i_cluster_epoch_t successor;
    ucn_i_cluster_config_view_t successor_config;
    ucn_i_cluster_handover_ready_t ready;
    ucn_i_cluster_durability_t durable;
    ucn_i_cluster_requirement_t requirement;
    ucn_i_cluster_owner_t before;
    ucn_i_cluster_authority_view_t view;

    CHECK(setup_head() == 0);
    successor = epoch(6U, 0x10U, 7U);
    successor_config = config(6U, 2U, 0x10U, 7U);
    before = owner;
    durable = durability(2U);
    CHECK(ucn_i_cluster_rekey_prepare(
              &owner, &successor, &successor_config, 1U, 30U,
              &durable, &requirement) == UCN_ERR_STATE);
    CHECK(memcmp(&owner, &before, sizeof(owner)) == 0);

    successor.cluster_id = 5U;
    CHECK(ucn_i_cluster_rekey_prepare(
              &owner, &successor, &successor_config, 1U, 30U,
              &durable, &requirement) == UCN_OK);
    CHECK(requirement.body[104] == 0U && requirement.body[105] == 0U &&
          requirement.body[106] == 0U && requirement.body[107] == 1U &&
          requirement.body[108] == 0U && requirement.body[109] == 0U &&
          requirement.body[110] == 0U && requirement.body[111] == 4U &&
          requirement.body[138] == UCN_I_CLUSTER_TRANSITION_NONE);
    CHECK(owner.state.epoch.cluster_id == 4U && owner.pending_valid == 1U);
    owner.snapshot_sync.buffers[0].valid = 1U;
    owner.directory[0].occupied = 1U;
    owner.tunnels[0].occupied = 1U;
    CHECK(publish(&requirement, 31U) == 0);
    CHECK(owner.state.epoch.cluster_id == 5U &&
          owner.state.retired_cluster_high_water == 4U &&
          owner.state.lineage_generation == 1U &&
          owner.state.stable_config.config_id == 6U &&
          owner.snapshot_sync.buffers[0].valid == 0U &&
          owner.directory[0].occupied == 0U &&
          owner.tunnels[0].occupied == 0U);

    successor = epoch(6U, 0x10U, 7U);
    successor_config = config(7U, 3U, 0x10U, 7U);
    durable = durability(3U);
    CHECK(ucn_i_cluster_rekey_prepare(
              &owner, &successor, &successor_config, 2U, 40U,
              &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 41U) == 0);
    CHECK(owner.state.epoch.cluster_id == 6U &&
          owner.state.retired_cluster_high_water == 5U &&
          owner.state.lineage_generation == 2U);

    successor = epoch(10U, 0x40U, 20U);
    successor_config = config(50U, 1U, 0x40U, 20U);
    memset(&ready, 0, sizeof(ready));
    ready.transaction_id = 3U;
    ready.lease_deadline_us = 7000U;
    ready.target_cluster_id = successor.cluster_id;
    ready.target_term = successor.term;
    ready.target_binding_generation = successor.head_binding_generation;
    ready.target_config_id = successor_config.config_id;
    ready.target_config_generation = successor_config.generation;
    ready.capability_generation = 9U;
    ready.authority_generation = 4U;
    ready.authenticated = 1U;
    ready.quorum_verified = 1U;
    ready.durable_continuation = 1U;
    principal(ready.target_principal, 0x40U);
    memset(ready.proof_digest, 0xC3, sizeof(ready.proof_digest));
    durable = durability(4U);
    before = owner;
    ready.target_cluster_id++;
    CHECK(ucn_i_cluster_merge_retire_prepare(
              &owner, &successor, &successor_config, &ready, 3U, 50U,
              &durable, &requirement) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&owner, &before, sizeof(owner)) == 0);
    ready.target_cluster_id--;
    CHECK(ucn_i_cluster_merge_retire_prepare(
              &owner, &successor, &successor_config, &ready, 3U, 50U,
              &durable, &requirement) == UCN_OK);
    CHECK(requirement.body[107] == 3U && requirement.body[111] == 6U &&
          requirement.body[138] == UCN_I_CLUSTER_TRANSITION_MERGE &&
          requirement.body[142] == 0U && requirement.body[143] == 1U);
    CHECK(publish(&requirement, 51U) == 0);
    CHECK(owner.state.epoch.cluster_id == 6U &&
          owner.state.retired_cluster_high_water == 6U &&
          owner.state.lineage_generation == 3U &&
          owner.state.role == UCN_I_CLUSTER_FENCED &&
          owner.state.authority_fenced == 1U &&
          owner.state.transition.kind == UCN_I_CLUSTER_TRANSITION_MERGE);
    CHECK(ucn_i_cluster_authority_preflight(&owner, 52U, &view) ==
          UCN_ERR_ACCESS);
    CHECK(ucn_i_cluster_merge_continuation_preflight(&owner, 3U, 52U) ==
          UCN_ERR_ACCESS);
    ready.proof_digest[0] ^= 1U;
    CHECK(ucn_i_cluster_merge_ready_accept(&owner, &ready, 52U) ==
          UCN_ERR_SECURITY);
    ready.proof_digest[0] ^= 1U;
    CHECK(ucn_i_cluster_merge_ready_accept(&owner, &ready, 52U) == UCN_OK);
    CHECK(ucn_i_cluster_merge_continuation_preflight(&owner, 3U, 53U) ==
          UCN_OK);
    CHECK(ucn_i_cluster_merge_continuation_preflight(&owner, 3U, 7000U) ==
          UCN_ERR_ACCESS);
    successor = epoch(7U, 0x10U, 7U);
    successor_config = config(8U, 4U, 0x10U, 7U);
    durable = durability(5U);
    CHECK(ucn_i_cluster_rekey_prepare(
              &owner, &successor, &successor_config, 4U, 60U,
              &durable, &requirement) == UCN_ERR_STATE);
    return 0;
}

int main(void)
{
    int result = test_rekey_and_merge_lineage();
    if (result != 0) fprintf(stderr, "cluster lineage failed: %d\n", result);
    return result;
}
