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

static int publish_bootstrap(ucn_i_cluster_requirement_t *requirement)
{
    ucn_handle_t handle;
    ucn_i_cluster_proof_t proof;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = 1U;
    handle.owner_instance = 9U;
    handle.slot = 1U;
    handle.generation = 1U;
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    CHECK(ucn_i_cluster_bind_persistence(
              &owner, handle, requirement->canonical_body_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = handle;
    proof.domain_id = requirement->durability.domain_id;
    proof.foundation_transaction_id = 1U;
    proof.record_generation = 1U;
    proof.witness_generation = 1U;
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
    CHECK(ucn_i_cluster_accept_proof(&owner, &proof, 10U) == UCN_OK);
    return 0;
}

static int setup_head(void)
{
    ucn_i_cluster_config_t owner_config;
    ucn_i_cluster_epoch_t epoch;
    ucn_i_cluster_config_view_t stable;
    ucn_i_cluster_member_fact_t local;
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
    memset(&epoch, 0, sizeof(epoch));
    epoch.cluster_id = 4U;
    epoch.term = 1U;
    epoch.head_binding_generation = 7U;
    principal(epoch.head_principal, 0x10U);
    memset(&stable, 0, sizeof(stable));
    stable.config_id = 5U;
    stable.generation = 1U;
    stable.member_count = 1U;
    stable.members[0].binding_generation = 7U;
    stable.members[0].flags = UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
                              UCN_I_CLUSTER_MEMBER_FLAG_VOTER;
    principal(stable.members[0].principal, 0x10U);
    memset(&durable, 0, sizeof(durable));
    durable.domain_id = 822U;
    durable.foundation_transaction_id = 1U;
    durable.absolute_deadline_us = 9000U;
    durable.persistence_domain_generation = 3U;
    durable.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    durable.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    CHECK(ucn_i_cluster_create_prepare(
              &owner, &epoch, &stable, UCN_I_CLUSTER_HEAD,
              &durable, &requirement) == UCN_OK);
    CHECK(publish_bootstrap(&requirement) == 0);
    memset(&local, 0, sizeof(local));
    local.lease_deadline_us = 10000U;
    local.capability_deadline_us = 10000U;
    local.binding_generation = 7U;
    local.session_generation = 2U;
    local.capability_generation = 3U;
    local.route_generation = 4U;
    local.link_generation = 5U;
    local.link_id = 1U;
    local.authenticated = 1U;
    local.current = 1U;
    principal(local.principal, 0x10U);
    memset(local.capability_digest, 0x91,
           sizeof(local.capability_digest));
    CHECK(ucn_i_cluster_member_observe(&owner, &local, 20U) == UCN_OK);
    return 0;
}

static ucn_i_cluster_directory_fact_t directory_fact(
    uint32_t cluster_id, uint8_t seed, uint64_t sequence)
{
    ucn_i_cluster_directory_fact_t value;
    memset(&value, 0, sizeof(value));
    value.remote_epoch.cluster_id = cluster_id;
    value.remote_epoch.term = 3U;
    value.remote_epoch.head_binding_generation = 20U + cluster_id;
    principal(value.remote_epoch.head_principal, seed);
    value.origin_sequence = sequence;
    value.authority_deadline_us = 1000U;
    value.capability_deadline_us = 1000U;
    value.flow_deadline_us = 1000U;
    value.remote_config_id = 30U + cluster_id;
    value.remote_config_generation = 2U;
    value.authority_generation = 4U;
    value.source_session_generation = 8U + cluster_id;
    value.capability_generation = 9U + cluster_id;
    value.route_generation = 10U + cluster_id;
    value.path_generation = 11U + cluster_id;
    value.link_generation = 12U + cluster_id;
    value.path_id = (uint16_t)(20U + cluster_id);
    value.link_id = (uint16_t)(30U + cluster_id);
    memset(value.capability_digest, (int)(uint8_t)(0x60U + cluster_id),
           sizeof(value.capability_digest));
    memset(value.authority_digest, (int)(uint8_t)(0x70U + cluster_id),
           sizeof(value.authority_digest));
    value.authenticated = 1U;
    value.quorum_verified = 1U;
    value.flow_active = 1U;
    return value;
}

static ucn_i_cluster_tunnel_request_t tunnel_request(
    const ucn_i_cluster_directory_fact_t *directory)
{
    ucn_i_cluster_tunnel_request_t value;
    memset(&value, 0, sizeof(value));
    value.tunnel_id = 1U;
    value.directory_origin_sequence = directory->origin_sequence;
    value.absolute_deadline_us = 900U;
    value.source_cluster_id = owner.state.epoch.cluster_id;
    value.destination_cluster_id = directory->remote_epoch.cluster_id;
    value.flow.deadline_us = 950U;
    value.flow.source_binding_generation =
        owner.state.epoch.head_binding_generation;
    value.flow.source_session_generation = 2U;
    value.flow.destination_binding_generation =
        directory->remote_epoch.head_binding_generation;
    value.flow.destination_session_generation =
        directory->source_session_generation;
    value.flow.capability_generation = directory->capability_generation;
    value.flow.route_generation = directory->route_generation;
    value.flow.path_generation = directory->path_generation;
    value.flow.link_generation = directory->link_generation;
    value.flow.path_id = directory->path_id;
    value.flow.link_id = directory->link_id;
    memcpy(value.flow.source_principal, owner.state.epoch.head_principal,
           sizeof(value.flow.source_principal));
    memcpy(value.flow.destination_principal,
           directory->remote_epoch.head_principal,
           sizeof(value.flow.destination_principal));
    memcpy(value.flow.capability_digest, directory->capability_digest,
           sizeof(value.flow.capability_digest));
    value.flow.active = 1U;
    value.flow.authenticated = 1U;
    return value;
}

static int test_directory_and_tunnel(void)
{
    ucn_i_cluster_directory_fact_t remote;
    ucn_i_cluster_directory_fact_t copied;
    ucn_i_cluster_directory_fact_t changed;
    ucn_i_cluster_tunnel_request_t request;
    ucn_i_cluster_tunnel_request_t output;
    ucn_i_cluster_step_result_t step;
    uint8_t index;

    CHECK(setup_head() == 0);
    remote = directory_fact(10U, 0x40U, 1U);
    CHECK(ucn_i_cluster_directory_install(&owner, &remote, 30U) == UCN_OK);
    memset(&copied, 0, sizeof(copied));
    CHECK(ucn_i_cluster_directory_copy(&owner, 10U, 30U, &copied) == UCN_OK);
    CHECK(memcmp(&copied, &remote, sizeof(remote)) == 0);
    CHECK(ucn_i_cluster_directory_install(&owner, &remote, 30U) == UCN_OK);
    changed = remote;
    changed.capability_digest[0] ^= 1U;
    CHECK(ucn_i_cluster_directory_install(&owner, &changed, 30U) ==
          UCN_ERR_SECURITY);
    changed = remote;
    changed.origin_sequence = 0U;
    CHECK(ucn_i_cluster_directory_install(&owner, &changed, 30U) ==
          UCN_ERR_ARGUMENT);

    request = tunnel_request(&remote);
    CHECK(ucn_i_cluster_tunnel_install(&owner, &request, 30U) == UCN_OK);
    memset(&output, 0, sizeof(output));
    CHECK(ucn_i_cluster_tunnel_preflight(
              &owner, 1U, &request.flow, 31U, &output) == UCN_OK);
    CHECK(memcmp(&output, &request, sizeof(request)) == 0);
    changed = remote;
    changed.origin_sequence = 2U;
    changed.route_generation++;
    CHECK(ucn_i_cluster_directory_install(&owner, &changed, 32U) == UCN_OK);
    {
        ucn_i_cluster_directory_fact_t rollback = changed;
        rollback.origin_sequence = 3U;
        rollback.authority_generation++;
        rollback.remote_epoch.term--;
        CHECK(ucn_i_cluster_directory_install(&owner, &rollback, 32U) ==
              UCN_ERR_SECURITY);
        rollback = changed;
        rollback.origin_sequence = 3U;
        principal(rollback.remote_epoch.head_principal, 0x50U);
        CHECK(ucn_i_cluster_directory_install(&owner, &rollback, 32U) ==
              UCN_ERR_SECURITY);
    }
    memset(&output, 0xA5, sizeof(output));
    CHECK(ucn_i_cluster_tunnel_preflight(
              &owner, 1U, &request.flow, 33U, &output) == UCN_ERR_ACCESS);
    {
        ucn_i_cluster_tunnel_request_t sentinel;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    }

    CHECK(ucn_i_cluster_dependency_revoke(
              &owner, changed.remote_epoch.head_principal,
              changed.remote_epoch.head_binding_generation,
              changed.source_session_generation,
              changed.capability_generation, changed.link_id,
              changed.link_generation) == UCN_OK);
    CHECK(ucn_i_cluster_directory_copy(&owner, 10U, 34U, &copied) ==
          UCN_ERR_NOT_FOUND);

    remote = directory_fact(10U, 0x40U, 3U);
    CHECK(ucn_i_cluster_directory_install(&owner, &remote, 35U) == UCN_OK);
    request = tunnel_request(&remote);
    request.directory_origin_sequence = remote.origin_sequence;
    CHECK(ucn_i_cluster_tunnel_install(&owner, &request, 35U) == UCN_OK);
    for (index = 1U; index < UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT; ++index) {
        changed = directory_fact((uint32_t)(10U + index),
                                 (uint8_t)(0x40U + index), 1U);
        CHECK(ucn_i_cluster_directory_install(&owner, &changed, 35U) ==
              UCN_OK);
    }
    changed = directory_fact(
        (uint32_t)(10U + UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT),
        (uint8_t)(0x40U + UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT), 1U);
    CHECK(ucn_i_cluster_directory_install(&owner, &changed, 35U) ==
          UCN_ERR_NO_SPACE);
    CHECK(ucn_i_cluster_step(&owner, 1000U, UINT16_MAX, &step) == UCN_OK);
    CHECK(step.directories_expired == UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT &&
          step.tunnels_expired == 1U);
    memset(&copied, 0xA5, sizeof(copied));
    CHECK(ucn_i_cluster_directory_copy(&owner, 10U, 1000U, &copied) ==
          UCN_ERR_NOT_FOUND);
    return 0;
}

int main(void)
{
    int result = test_directory_and_tunnel();
    if (result != 0) fprintf(stderr, "cluster directory failed: %d\n", result);
    return result;
}
