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

static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static void write_be64(uint8_t *out, uint64_t value)
{
    write_be32(out, (uint32_t)(value >> 32U));
    write_be32(&out[4], (uint32_t)value);
}

static ucn_i_cluster_durability_t durability(uint64_t transaction)
{
    ucn_i_cluster_durability_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = 820U;
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

static int configure_backup(void)
{
    ucn_i_cluster_config_t owner_config;
    ucn_i_cluster_epoch_t epoch;
    ucn_i_cluster_config_view_t config;
    ucn_i_cluster_durability_t durable;
    ucn_i_cluster_requirement_t requirement;
    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&owner_config, 0, sizeof(owner_config));
    owner_config.runtime_instance = 1U;
    owner_config.owner_instance = 8U;
    principal(owner_config.local_principal, 0x20U);
    owner_config.state_lock = lock_ops();
    CHECK(ucn_i_cluster_owner_init(&owner, &owner_config) == UCN_OK);
    memset(&epoch, 0, sizeof(epoch));
    epoch.cluster_id = 4U;
    epoch.term = 1U;
    epoch.head_binding_generation = 7U;
    principal(epoch.head_principal, 0x20U);
    memset(&config, 0, sizeof(config));
    config.config_id = 5U;
    config.generation = 1U;
    config.member_count = 1U;
    config.members[0].binding_generation = 7U;
    config.members[0].flags = UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
                              UCN_I_CLUSTER_MEMBER_FLAG_VOTER |
                              UCN_I_CLUSTER_MEMBER_FLAG_BACKUP;
    principal(config.members[0].principal, 0x20U);
    record_generation = 0U;
    durable = durability(1U);
    CHECK(ucn_i_cluster_create_prepare(
              &owner, &epoch, &config, UCN_I_CLUSTER_BACKUP,
              &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 10U) == 0);
    return 0;
}

static void make_snapshot(uint32_t assignment, uint32_t sequence,
                          uint64_t deadline_us,
                          ucn_i_cluster_snapshot_header_t *header,
                          ucn_i_cluster_snapshot_member_t *member)
{
    ucn_i_sha256_workspace_t workspace;
    uint8_t canonical[UCN_I_CLUSTER_SNAPSHOT_HEADER_BYTES +
                      UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES];
    memset(header, 0, sizeof(*header));
    memset(member, 0, sizeof(*member));
    memset(&workspace, 0, sizeof(workspace));
    memset(canonical, 0, sizeof(canonical));
    header->absolute_deadline_us = deadline_us;
    header->cluster_id = owner.state.epoch.cluster_id;
    header->term = owner.state.epoch.term;
    header->config_id = owner.state.stable_config.config_id;
    header->config_generation = owner.state.stable_config.generation;
    header->assignment_generation = assignment;
    header->snapshot_sequence = sequence;
    header->protected_voter_bitmap = 1U;
    header->member_count = 1U;
    write_be32(&canonical[0], header->cluster_id);
    write_be32(&canonical[4], header->term);
    write_be32(&canonical[8], header->config_id);
    write_be32(&canonical[12], header->config_generation);
    write_be32(&canonical[16], assignment);
    write_be32(&canonical[20], sequence);
    write_be32(&canonical[24], 1U);
    canonical[28] = 1U;
    write_be64(&canonical[32], deadline_us);
    member->binding_generation = 7U;
    member->session_generation = 2U + sequence;
    member->capability_generation = 4U + sequence;
    member->route_generation = 6U + sequence;
    member->link_generation = 8U + sequence;
    member->link_id = 1U;
    member->flags = owner.state.stable_config.members[0].flags;
    principal(member->principal, 0x20U);
    memset(member->capability_digest, (int)(0xA0U + sequence),
           sizeof(member->capability_digest));
    memcpy(&canonical[40], member->principal, sizeof(member->principal));
    write_be32(&canonical[56], member->binding_generation);
    write_be32(&canonical[60], member->session_generation);
    write_be32(&canonical[64], member->capability_generation);
    write_be32(&canonical[68], member->route_generation);
    write_be32(&canonical[72], member->link_generation);
    write_be16(&canonical[76], member->link_id);
    canonical[78] = member->flags;
    memcpy(&canonical[80], member->capability_digest,
           sizeof(member->capability_digest));
    (void)ucn_i_sha256_128(canonical, sizeof(canonical),
                           header->expected_digest, &workspace);
}

static int test_double_buffer_and_ready(void)
{
    ucn_i_cluster_snapshot_header_t header;
    ucn_i_cluster_snapshot_member_t member;
    ucn_i_cluster_snapshot_sync_t old_sync;
    ucn_i_cluster_durability_t durable;
    ucn_i_cluster_requirement_t requirement;
    ucn_i_cluster_step_result_t step;

    CHECK(configure_backup() == 0);
    make_snapshot(1U, 1U, 1000U, &header, &member);
    CHECK(ucn_i_cluster_snapshot_begin(&owner, &header, 20U) == UCN_OK);
    CHECK(ucn_i_cluster_snapshot_member(&owner, &member, 21U) == UCN_OK);
    CHECK(ucn_i_cluster_snapshot_end(&owner, 22U) == UCN_OK);
    durable = durability(2U);
    CHECK(ucn_i_cluster_backup_ready_from_mirror_prepare(
              &owner, &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 23U) == 0);
    CHECK(ucn_i_cluster_p_snapshot_current(&owner));

    old_sync = owner.snapshot_sync;
    make_snapshot(2U, 1U, 1000U, &header, &member);
    CHECK(ucn_i_cluster_snapshot_begin(&owner, &header, 30U) ==
          UCN_ERR_STATE);
    CHECK(memcmp(&owner.snapshot_sync, &old_sync, sizeof(old_sync)) == 0);

    make_snapshot(2U, 2U, 1000U, &header, &member);
    header.expected_digest[0] ^= 1U;
    CHECK(ucn_i_cluster_snapshot_begin(&owner, &header, 30U) == UCN_OK);
    CHECK(ucn_i_cluster_snapshot_member(&owner, &member, 31U) == UCN_OK);
    CHECK(ucn_i_cluster_snapshot_end(&owner, 32U) == UCN_ERR_SECURITY);
    CHECK(ucn_i_cluster_p_snapshot_current(&owner));

    make_snapshot(2U, 2U, 40U, &header, &member);
    CHECK(ucn_i_cluster_snapshot_begin(&owner, &header, 33U) == UCN_OK);
    CHECK(ucn_i_cluster_snapshot_member(&owner, &member, 34U) == UCN_OK);
    CHECK(ucn_i_cluster_step(&owner, 40U, UINT16_MAX, &step) == UCN_OK);
    CHECK(step.snapshots_aborted == 1U &&
          owner.snapshot_sync.building == 0U &&
          ucn_i_cluster_p_snapshot_current(&owner));

    make_snapshot(2U, 2U, 1000U, &header, &member);
    CHECK(ucn_i_cluster_snapshot_begin(&owner, &header, 50U) == UCN_OK);
    CHECK(ucn_i_cluster_snapshot_member(&owner, &member, 51U) == UCN_OK);
    CHECK(ucn_i_cluster_snapshot_end(&owner, 52U) == UCN_OK);
    CHECK(!ucn_i_cluster_p_snapshot_current(&owner));
    durable = durability(3U);
    CHECK(ucn_i_cluster_backup_ready_from_mirror_prepare(
              &owner, &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 53U) == 0);
    CHECK(owner.state.backup_assignment_generation == 2U &&
          ucn_i_cluster_p_snapshot_current(&owner));
    return 0;
}

int main(void)
{
    int result = test_double_buffer_and_ready();
    if (result != 0) fprintf(stderr, "cluster snapshot failed: %d\n", result);
    return result;
}
