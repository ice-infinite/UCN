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

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context) { ((test_lock_t *)context)->held = 0U; }

static ucn_i_lock_ops_t lock_ops(test_lock_t *state)
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

static void principal(uint8_t out[UCN_I_CLUSTER_PRINCIPAL_BYTES],
                      uint8_t seed)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_PRINCIPAL_BYTES; ++index) {
        out[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_cluster_config_view_t cluster_config(uint32_t generation)
{
    ucn_i_cluster_config_view_t config;
    memset(&config, 0, sizeof(config));
    config.config_id = 9U;
    config.generation = generation;
    config.member_count = 1U;
    config.members[0].binding_generation = 7U;
    config.members[0].flags =
        UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
        UCN_I_CLUSTER_MEMBER_FLAG_VOTER;
    principal(config.members[0].principal, 0x10U);
    return config;
}

static ucn_i_cluster_epoch_t epoch(void)
{
    ucn_i_cluster_epoch_t value;
    memset(&value, 0, sizeof(value));
    value.cluster_id = 3U;
    value.term = 1U;
    value.head_binding_generation = 7U;
    principal(value.head_principal, 0x10U);
    return value;
}

static ucn_i_cluster_durability_t durability(uint64_t tx,
                                               uint64_t generation)
{
    ucn_i_cluster_durability_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = 800U;
    value.foundation_transaction_id = tx;
    value.expected_record_generation = generation;
    value.absolute_deadline_us = 9000U;
    value.persistence_domain_generation = 2U;
    value.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    value.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    return value;
}

static ucn_handle_t persistence_handle(uint16_t generation)
{
    ucn_handle_t value;
    memset(&value, 0, sizeof(value));
    value.runtime_instance = 1U;
    value.owner_instance = 99U;
    value.slot = 1U;
    value.generation = generation;
    value.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    return value;
}

static int publish(ucn_i_cluster_requirement_t *requirement,
                   uint64_t now_us)
{
    ucn_i_cluster_proof_t proof;
    ucn_handle_t handle = persistence_handle(
        (uint16_t)requirement->durability.foundation_transaction_id);
    CHECK(ucn_i_cluster_bind_persistence(
              &owner, handle,
              requirement->canonical_body_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = handle;
    proof.domain_id = requirement->durability.domain_id;
    proof.foundation_transaction_id =
        requirement->durability.foundation_transaction_id;
    proof.record_generation =
        requirement->durability.expected_record_generation + 1U;
    proof.witness_generation = proof.record_generation;
    proof.runtime_instance = 1U;
    proof.body_bytes = UCN_I_CLUSTER_RECORD_BYTES;
    proof.persistence_domain_generation =
        requirement->durability.persistence_domain_generation;
    proof.persistence_owner_instance = handle.owner_instance;
    proof.caller_owner_instance = 8U;
    proof.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    proof.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    proof.operation_kind = requirement->operation_kind;
    memcpy(proof.body_digest, requirement->canonical_body_digest,
           sizeof(proof.body_digest));
    CHECK(ucn_i_cluster_accept_proof(&owner, &proof, now_us) == UCN_OK);
    return 0;
}

static ucn_i_cluster_member_fact_t local_fact(uint64_t deadline)
{
    ucn_i_cluster_member_fact_t fact;
    memset(&fact, 0, sizeof(fact));
    fact.lease_deadline_us = deadline;
    fact.capability_deadline_us = deadline;
    fact.binding_generation = 7U;
    fact.session_generation = 4U;
    fact.capability_generation = 5U;
    fact.route_generation = 6U;
    fact.link_generation = 7U;
    fact.link_id = 1U;
    fact.authenticated = 1U;
    fact.current = 1U;
    principal(fact.principal, 0x10U);
    memset(fact.capability_digest, 0xA5,
           sizeof(fact.capability_digest));
    return fact;
}

static int configure(void)
{
    ucn_i_cluster_config_t config;
    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&config, 0, sizeof(config));
    config.runtime_instance = 1U;
    config.owner_instance = 8U;
    principal(config.local_principal, 0x10U);
    config.state_lock = lock_ops(&lock_state);
    CHECK(ucn_i_cluster_owner_init(&owner, &config) == UCN_OK);
    return 0;
}

static int test_authority_and_joint(void)
{
    ucn_i_cluster_epoch_t active = epoch();
    ucn_i_cluster_config_view_t stable = cluster_config(1U);
    ucn_i_cluster_config_view_t next = cluster_config(2U);
    ucn_i_cluster_member_fact_t fact = local_fact(1000U);
    ucn_i_cluster_durability_t durable = durability(1U, 0U);
    ucn_i_cluster_requirement_t requirement;
    ucn_i_cluster_authority_view_t view;
    ucn_i_cluster_state_t snapshot;

    CHECK(configure() == 0);
    active.term = 2U;
    snapshot = owner.state;
    CHECK(ucn_i_cluster_create_prepare(
              &owner, &active, &stable, UCN_I_CLUSTER_HEAD,
              &durable, &requirement) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&snapshot, &owner.state, sizeof(snapshot)) == 0 &&
          owner.pending_valid == 0U);
    active.term = 1U;
    CHECK(ucn_i_cluster_create_prepare(
              &owner, &active, &stable, UCN_I_CLUSTER_HEAD,
              &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 10U) == 0);
    CHECK(ucn_i_cluster_member_observe(&owner, &fact, 20U) == UCN_OK);
    CHECK(ucn_i_cluster_authority_preflight(&owner, 20U, &view) == UCN_OK);
    CHECK(view.authority_active == 1U && view.live_old_voters == 1U);
    memset(&view, 0xA5, sizeof(view));
    CHECK(ucn_i_cluster_authority_preflight(&owner, 1000U, &view) ==
          UCN_ERR_ACCESS);
    CHECK(view.authority_active == 0U);
    fact = local_fact(8000U);
    CHECK(ucn_i_cluster_member_observe(&owner, &fact, 1001U) == UCN_OK);

    durable = durability(2U, 1U);
    CHECK(ucn_i_cluster_config_prepare(&owner, &next, 1U, 1100U,
                                       &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 1200U) == 0);
    snapshot = owner.state;
    durable = durability(3U, 2U);
    CHECK(ucn_i_cluster_config_commit_prepare(
              &owner, 1U, 1300U, &durable, &requirement) == UCN_ERR_STATE);
    CHECK(memcmp(&snapshot, &owner.state, sizeof(snapshot)) == 0);
    CHECK(ucn_i_cluster_config_enter_joint_prepare(
              &owner, 1U, 1300U, &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 1400U) == 0);
    CHECK(owner.state.joint_valid == 1U &&
          owner.state.phase == UCN_I_CLUSTER_JOINT);
    durable = durability(4U, 3U);
    CHECK(ucn_i_cluster_config_commit_prepare(
              &owner, 1U, 1500U, &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 1600U) == 0);
    CHECK(owner.state.phase == UCN_I_CLUSTER_STABLE &&
          owner.state.joint_valid == 0U &&
          owner.state.stable_config.generation == 2U &&
          owner.state.transition.kind == UCN_I_CLUSTER_TRANSITION_NONE);

    next = cluster_config(3U);
    durable = durability(5U, 4U);
    CHECK(ucn_i_cluster_config_prepare(&owner, &next, 2U, 1700U,
                                       &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 1800U) == 0);
    snapshot = owner.state;
    durable = durability(6U, 5U);
    CHECK(ucn_i_cluster_config_abort_prepare(
              &owner, 2U, 9U, 99U, &durable,
              &requirement) == UCN_ERR_STATE);
    CHECK(memcmp(&snapshot, &owner.state, sizeof(snapshot)) == 0);
    CHECK(ucn_i_cluster_config_abort_prepare(
              &owner, 2U, next.config_id, next.generation,
              &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 1900U) == 0);
    CHECK(owner.state.phase == UCN_I_CLUSTER_STABLE &&
          owner.state.stable_config.generation == 2U);
    return 0;
}

int main(void)
{
    int result = test_authority_and_joint();
    if (result != 0) fprintf(stderr, "cluster context failed: %d\n", result);
    return result;
}
