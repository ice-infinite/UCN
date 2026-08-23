#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_backup_sync.h"

#define ASSERT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                      \
            (void)fprintf(stderr, "ASSERT %s at %s:%d\\n", #condition,        \
                          __FILE__, __LINE__);                                   \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static bool make_owner_inputs(ucn_cluster_backup_epoch_t *epoch,
                              ucn_cluster_config_state_t *config)
{
    const ucn_node_id_t voters[] = {1U, 2U, 3U};

    (void)memset(epoch, 0, sizeof(*epoch));
    (void)memset(config, 0, sizeof(*config));
    epoch->cluster_id = 21U;
    epoch->term = 9U;
    epoch->head_node_id = 1U;
    epoch->backup_node_id = 2U;
    epoch->backup_generation = 4U;
    return ucn_cluster_config_state_init_stable(config, 6U, voters,
                                                sizeof(voters) / sizeof(voters[0U]));
}

static bool make_snapshot(ucn_cluster_snapshot_epoch_t *snapshot,
                          const ucn_cluster_backup_epoch_t *epoch,
                          const ucn_cluster_config_state_t *config,
                          uint32_t snapshot_id)
{
    return ucn_cluster_snapshot_epoch_from_config(snapshot, epoch, snapshot_id,
                                                  config);
}

static void seed_member(ucn_cluster_member_table_t *table, ucn_node_id_t node_id)
{
    ucn_cluster_member_t *member = &table->slots[0U];

    member->occupied = true;
    member->voting = false;
    member->provisional_deadline_armed = true;
    member->status = UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V3;
    member->node_id = node_id;
    member->lease_expires_at_ms = 100U;
    member->last_nonce = 1U;
    member->joined_at_ms = 2U;
    member->last_keepalive_at_ms = 3U;
    member->provisional_deadline_ms = 50U;
}

static void seed_sync_member(ucn_cluster_member_t *member, ucn_node_id_t node_id)
{
    ucn_cluster_member_table_t table;

    (void)memset(&table, 0, sizeof(table));
    seed_member(&table, node_id);
    *member = table.slots[0U];
}

static void make_full_coverage(ucn_cluster_backup_coverage_t *coverage)
{
    (void)memset(coverage, 0, sizeof(*coverage));
    coverage->count = 3U;
    coverage->entries[0U].node_id = 1U;
    coverage->entries[1U].node_id = 2U;
    coverage->entries[2U].node_id = 3U;
    coverage->entries[0U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
    coverage->entries[1U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
    coverage->entries[2U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
}

static void make_joint_coverage(ucn_cluster_backup_coverage_t *coverage)
{
    (void)memset(coverage, 0, sizeof(*coverage));
    coverage->count = 4U;
    coverage->entries[0U].node_id = 1U;
    coverage->entries[1U].node_id = 2U;
    coverage->entries[2U].node_id = 3U;
    coverage->entries[3U].node_id = 4U;
    coverage->entries[0U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
    coverage->entries[1U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
    coverage->entries[2U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
    coverage->entries[3U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
}

static int test_begin_only_opens_staging(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t committed_epoch;
    ucn_cluster_snapshot_epoch_t incoming_epoch;
    ucn_cluster_member_table_t committed_before;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&committed_epoch, &epoch, &config, 1U));
    seed_member(&owner.mirror.committed_members, 7U);
    owner.mirror.committed_epoch = committed_epoch;
    owner.mirror.committed_valid = true;
    committed_before = owner.mirror.committed_members;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_is_valid(&owner));

    ASSERT_TRUE(make_snapshot(&incoming_epoch, &epoch, &config, 2U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &incoming_epoch) == UCN_OK);
    ASSERT_TRUE(owner.mirror.staging_active);
    ASSERT_TRUE(memcmp(&owner.mirror.committed_members, &committed_before,
                       sizeof(committed_before)) == 0);
    ASSERT_TRUE(ucn_cluster_snapshot_epoch_is_exact(
        ucn_cluster_backup_mirror_staging_epoch(&owner.mirror), &incoming_epoch));
    return 0;
}

static int test_begin_rejections_do_not_write_owner(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_sync_owner_t before;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));

    before = owner;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, 99U, 0U, &snapshot) ==
                UCN_ERR_ACCESS);
    ASSERT_TRUE(memcmp(&owner, &before, sizeof(owner)) == 0);

    snapshot.backup_epoch.backup_generation++;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before, sizeof(owner)) == 0);

    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    snapshot.config_hash++;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before, sizeof(owner)) == 0);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    before = owner;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     1U, &snapshot) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before, sizeof(owner)) == 0);
    return 0;
}

static int test_init_reassignment_discards_local_mirror(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_backup_epoch_t replacement_epoch;
    ucn_cluster_config_state_t config;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    owner.mirror.committed_valid = true;
    owner.mirror.committed_epoch.backup_epoch = epoch;
    owner.mirror.committed_epoch.snapshot_id = 1U;
    owner.mirror.committed_epoch.config_id = config.config_id;
    owner.mirror.committed_epoch.config_phase = config.phase;
    owner.mirror.committed_epoch.config_hash =
        ucn_cluster_config_state_hash(&config);
    replacement_epoch = epoch;
    replacement_epoch.backup_node_id = 4U;
    replacement_epoch.backup_generation++;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &replacement_epoch,
                                                    &config) == UCN_OK);
    ASSERT_TRUE(owner.assigned_epoch.backup_node_id == 4U);
    ASSERT_TRUE(!owner.mirror.committed_valid);
    ASSERT_TRUE(!owner.mirror.staging_active);
    ASSERT_TRUE(ucn_cluster_backup_mirror_committed(&owner.mirror) == NULL);
    return 0;
}

static int test_member_end_only_swap_after_full_proof(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_sync_owner_t before_end;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;
    ucn_cluster_member_t member;
    ucn_cluster_backup_coverage_t coverage;
    uint32_t snapshot_hash;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    seed_sync_member(&member, 7U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot, 2U, &member) ==
                UCN_ERR_REPLAY);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot, 1U, &member) ==
                UCN_OK);
    snapshot_hash = ucn_cluster_backup_sync_member_hash_update(
        UINT32_C(2166136261), &member);
    ASSERT_TRUE(snapshot_hash != 0U);
    make_full_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_coverage_is_valid(&coverage));
    before_end = owner;
    /* END is a distinct control record after MEMBER #1, so it must carry
     * sequence 2 rather than repeat the final Member sequence. */
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 1U, 1U,
                                                   snapshot_hash, &coverage) ==
                UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_end, sizeof(owner)) == 0);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   snapshot_hash + 1U,
                                                   &coverage) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_end, sizeof(owner)) == 0);
    coverage.entries[2U].state = UCN_CLUSTER_BACKUP_PEER_SUSPECT;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   snapshot_hash, &coverage) ==
                UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_end, sizeof(owner)) == 0);
    coverage.entries[2U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   snapshot_hash, &coverage) ==
                UCN_OK);
    ASSERT_TRUE(owner.mirror.committed_valid);
    ASSERT_TRUE(!owner.mirror.staging_active);
    ASSERT_TRUE(owner.committed_final_sequence == 2U);
    ASSERT_TRUE(owner.committed_snapshot_hash == snapshot_hash);
    ASSERT_TRUE(owner.next_delta_sequence == 3U);
    ASSERT_TRUE(ucn_cluster_backup_mirror_committed(&owner.mirror)->slots[0U]
                    .node_id == 7U);
    return 0;
}

static int test_ready_binds_exact_committed_snapshot(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;
    ucn_cluster_member_t member;
    ucn_cluster_backup_coverage_t coverage;
    ucn_cluster_backup_ready_t ready;
    uint32_t snapshot_hash;

    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&ready, 0, sizeof(ready));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    seed_sync_member(&member, 7U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot, 1U, &member) ==
                UCN_OK);
    snapshot_hash = ucn_cluster_backup_sync_member_hash_update(
        UINT32_C(2166136261), &member);
    make_full_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   snapshot_hash, &coverage) ==
                UCN_OK);
    ready.source_node_id = epoch.backup_node_id;
    ready.snapshot_epoch = snapshot;
    ready.final_sequence = 2U;
    ready.snapshot_hash = snapshot_hash;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_verify_ready(&owner, &ready) ==
                UCN_OK);
    ready.source_node_id = 7U;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_verify_ready(&owner, &ready) ==
                UCN_ERR_REPLAY);
    ready.source_node_id = epoch.backup_node_id;
    ready.final_sequence++;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_verify_ready(&owner, &ready) ==
                UCN_ERR_REPLAY);
    ready.final_sequence = 2U;
    ready.snapshot_epoch.snapshot_id++;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_verify_ready(&owner, &ready) ==
                UCN_ERR_REPLAY);
    return 0;
}

static int test_empty_snapshot_end_uses_control_sequence_one(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;
    ucn_cluster_backup_coverage_t coverage;
    ucn_cluster_backup_sync_owner_t before_end;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    make_full_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    before_end = owner;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(
                    &owner, epoch.head_node_id, &snapshot, 0U, 0U,
                    UINT32_C(2166136261), &coverage) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_end, sizeof(owner)) == 0);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(
                    &owner, epoch.head_node_id, &snapshot, 1U, 0U,
                    UINT32_C(2166136261), &coverage) == UCN_OK);
    ASSERT_TRUE(owner.committed_final_sequence == 1U);
    ASSERT_TRUE(owner.next_delta_sequence == 2U);
    return 0;
}

static int test_delta_requires_exact_snapshot_and_gap_resync(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_sync_owner_t before_gap;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;
    ucn_cluster_member_t member;
    ucn_cluster_backup_coverage_t coverage;
    ucn_cluster_backup_delta_t delta;
    uint32_t initial_hash;

    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&delta, 0, sizeof(delta));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    seed_sync_member(&member, 7U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot, 1U, &member) ==
                UCN_OK);
    initial_hash = ucn_cluster_backup_sync_member_hash_update(UINT32_C(2166136261),
                                                               &member);
    make_full_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   initial_hash, &coverage) == UCN_OK);
    delta.snapshot_epoch = snapshot;
    delta.sequence = 3U;
    delta.previous_snapshot_hash = initial_hash;
    delta.member = member;
    delta.member.last_nonce = 2U;
    delta.resulting_snapshot_hash = ucn_cluster_backup_sync_member_hash_update(
        UINT32_C(2166136261), &delta.member);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &delta) == UCN_OK);
    ASSERT_TRUE(owner.committed_snapshot_hash == delta.resulting_snapshot_hash);
    ASSERT_TRUE(owner.mirror.committed_members.slots[0U].last_nonce == 2U);
    ASSERT_TRUE(owner.next_delta_sequence == 4U);

    /* A Delta cannot manufacture a new member that was not proven by the
     * committed full Snapshot. */
    before_gap = owner;
    delta.sequence = 4U;
    delta.previous_snapshot_hash = owner.committed_snapshot_hash;
    delta.member.node_id = 8U;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &delta) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_gap, sizeof(owner)) == 0);

    /* Nor can it alter static member identity/eligibility beneath the
     * committed Snapshot. */
    delta.member.node_id = 7U;
    delta.member.voting = true;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &delta) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_gap, sizeof(owner)) == 0);

    delta.member.voting = false;
    delta.member.last_nonce = 1U;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &delta) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_gap, sizeof(owner)) == 0);

    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 2U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    before_gap = owner;
    delta.sequence = 3U;
    delta.previous_snapshot_hash = owner.committed_snapshot_hash;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &delta) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_gap, sizeof(owner)) == 0);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_abort(&owner) == UCN_OK);

    before_gap = owner;
    delta.sequence = 3U;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &delta) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&owner, &before_gap, sizeof(owner)) == 0);
    delta.sequence = 5U;
    delta.previous_snapshot_hash = owner.committed_snapshot_hash;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &delta) == UCN_ERR_REPLAY);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_resync_required(&owner));
    ASSERT_TRUE(owner.mirror.committed_members.slots[0U].last_nonce == 2U);
    delta.sequence = 4U;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &delta) == UCN_ERR_REPLAY);
    return 0;
}

static int test_coverage_grace_and_serial_exhaustion(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_sync_owner_t before;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_backup_epoch_t exhausted_epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;
    ucn_cluster_snapshot_epoch_t exhausted_snapshot;
    ucn_cluster_member_t member;
    ucn_cluster_backup_coverage_t coverage;
    uint32_t snapshot_hash;
    uint32_t now_ms = UINT32_MAX - UINT32_C(1000);

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    seed_sync_member(&member, 7U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot, 1U, &member) ==
                UCN_OK);
    snapshot_hash = ucn_cluster_backup_sync_member_hash_update(UINT32_C(2166136261),
                                                               &member);
    make_full_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   snapshot_hash, &coverage) == UCN_OK);
    coverage.entries[2U].state = UCN_CLUSTER_BACKUP_PEER_SUSPECT;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(&owner, &coverage,
                                                               now_ms) == UCN_OK);
    ASSERT_TRUE(owner.coverage_grace_armed);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_takeover_eligible(&owner));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(
                    &owner, &coverage, owner.coverage_grace_deadline_ms - 1U) ==
                UCN_OK);
    coverage.entries[2U].state = UCN_CLUSTER_BACKUP_PEER_ADMITTED;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(
                    &owner, &coverage, owner.coverage_grace_deadline_ms - 1U) ==
                UCN_OK);
    ASSERT_TRUE(!owner.coverage_grace_armed);
    coverage.entries[2U].state = UCN_CLUSTER_BACKUP_PEER_SUSPECT;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(&owner, &coverage,
                                                                now_ms) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(
                    &owner, &coverage, owner.coverage_grace_deadline_ms) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(!ucn_cluster_backup_sync_owner_takeover_eligible(&owner));

    ASSERT_TRUE(make_snapshot(&exhausted_snapshot, &epoch, &config,
                              UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD));
    before = owner;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &exhausted_snapshot) ==
                UCN_ERR_EXHAUSTED);
    ASSERT_TRUE(memcmp(&owner, &before, sizeof(owner)) == 0);
    exhausted_epoch = epoch;
    exhausted_epoch.backup_generation = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    ASSERT_TRUE(ucn_cluster_backup_epoch_rekey_required(&exhausted_epoch));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &exhausted_epoch,
                                                    &config) == UCN_ERR_EXHAUSTED);
    ASSERT_TRUE(ucn_cluster_backup_epoch_next_generation(&exhausted_epoch,
                                                          &epoch) == UCN_OK);
    ASSERT_TRUE(exhausted_epoch.backup_generation == epoch.backup_generation + 1U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &exhausted_epoch,
                                                    &config) == UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &exhausted_epoch, &config, 1U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner,
                                                     exhausted_epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    exhausted_epoch.backup_generation =
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD - 1U;
    ASSERT_TRUE(ucn_cluster_backup_epoch_next_generation(&epoch,
                                                          &exhausted_epoch) ==
                UCN_ERR_EXHAUSTED);
    return 0;
}

static int test_removed_protected_voter_is_immediately_ineligible(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;
    ucn_cluster_member_t member;
    ucn_cluster_backup_coverage_t coverage;
    uint32_t snapshot_hash;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    seed_sync_member(&member, 7U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot, 1U, &member) ==
                UCN_OK);
    snapshot_hash = ucn_cluster_backup_sync_member_hash_update(UINT32_C(2166136261),
                                                               &member);
    make_full_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   snapshot_hash, &coverage) == UCN_OK);
    coverage.entries[2U].state = UCN_CLUSTER_BACKUP_PEER_REMOVED;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(&owner, &coverage,
                                                                UINT32_C(100)) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(!owner.coverage_grace_armed);
    ASSERT_TRUE(!ucn_cluster_backup_sync_owner_takeover_eligible(&owner));
    return 0;
}

static int test_missing_stable_protected_voter_is_immediately_ineligible(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;
    ucn_cluster_member_t member;
    ucn_cluster_backup_coverage_t coverage;
    uint32_t snapshot_hash;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &config, 1U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    seed_sync_member(&member, 7U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot, 1U, &member) ==
                UCN_OK);
    snapshot_hash = ucn_cluster_backup_sync_member_hash_update(UINT32_C(2166136261),
                                                               &member);
    make_full_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   snapshot_hash, &coverage) == UCN_OK);

    /* A canonical but incomplete view is not SUSPECT and cannot borrow grace. */
    coverage.count = 2U;
    (void)memset(&coverage.entries[2U], 0,
                 sizeof(coverage.entries) - 2U * sizeof(coverage.entries[0U]));
    ASSERT_TRUE(ucn_cluster_backup_coverage_is_valid(&coverage));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(&owner, &coverage,
                                                                UINT32_C(100)) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(!owner.coverage_grace_armed);
    ASSERT_TRUE(!ucn_cluster_backup_sync_owner_takeover_eligible(&owner));

    /* Returning to ADMITTED cannot revive the retired assignment. */
    make_full_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(&owner, &coverage,
                                                                UINT32_C(101)) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(!ucn_cluster_backup_sync_owner_takeover_eligible(&owner));
    return 0;
}

static int test_missing_joint_protected_voter_is_immediately_ineligible(void)
{
    const ucn_node_id_t new_voters[] = {1U, 2U, 4U};
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t stable_config;
    ucn_cluster_config_state_t joint_config;
    ucn_cluster_snapshot_epoch_t snapshot;
    ucn_cluster_member_t member;
    ucn_cluster_backup_coverage_t coverage;
    uint32_t snapshot_hash;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_owner_inputs(&epoch, &stable_config));
    ASSERT_TRUE(ucn_cluster_config_state_init_joint(&joint_config, &stable_config,
                                                     new_voters,
                                                     sizeof(new_voters) /
                                                         sizeof(new_voters[0U])));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch,
                                                    &joint_config) == UCN_OK);
    ASSERT_TRUE(make_snapshot(&snapshot, &epoch, &joint_config, 1U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot) == UCN_OK);
    seed_sync_member(&member, 7U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot, 1U, &member) ==
                UCN_OK);
    snapshot_hash = ucn_cluster_backup_sync_member_hash_update(UINT32_C(2166136261),
                                                               &member);
    make_joint_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot, 2U, 1U,
                                                   snapshot_hash, &coverage) == UCN_OK);

    /* Node 4 is protected by C_new.  Removing only its coverage row must
     * fail closed even though all C_old voters remain ADMITTED. */
    coverage.count = 3U;
    (void)memset(&coverage.entries[3U], 0, sizeof(coverage.entries[3U]));
    ASSERT_TRUE(ucn_cluster_backup_coverage_is_valid(&coverage));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(&owner, &coverage,
                                                                UINT32_C(200)) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(!owner.coverage_grace_armed);
    ASSERT_TRUE(!ucn_cluster_backup_sync_owner_takeover_eligible(&owner));

    /* A later complete Joint ADMITTED view cannot revive the fenced
     * assignment; only a fresh Backup assignment may create a new owner. */
    make_joint_coverage(&coverage);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_update_coverage(&owner, &coverage,
                                                                UINT32_C(201)) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(!ucn_cluster_backup_sync_owner_takeover_eligible(&owner));
    return 0;
}

static int test_primary_failure_matrix_preserves_last_committed(void)
{
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_config_state_t refreshed_config;
    ucn_cluster_snapshot_epoch_t snapshot_one;
    ucn_cluster_snapshot_epoch_t snapshot_two;
    ucn_cluster_member_t member_one;
    ucn_cluster_member_t member_two;
    ucn_cluster_backup_coverage_t coverage;
    ucn_cluster_backup_ready_t old_ready;
    ucn_cluster_backup_delta_t bad_delta;
    uint32_t hash_one;
    uint32_t hash_two;

    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&old_ready, 0, sizeof(old_ready));
    (void)memset(&bad_delta, 0, sizeof(bad_delta));
    ASSERT_TRUE(make_owner_inputs(&epoch, &config));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                UCN_OK);
    make_full_coverage(&coverage);
    ASSERT_TRUE(make_snapshot(&snapshot_one, &epoch, &config, 1U));
    seed_sync_member(&member_one, 7U);
    hash_one = ucn_cluster_backup_sync_member_hash_update(UINT32_C(2166136261),
                                                           &member_one);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot_one) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot_one, 1U,
                                                      &member_one) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot_one, 2U, 1U,
                                                   hash_one, &coverage) == UCN_OK);
    old_ready.source_node_id = epoch.backup_node_id;
    old_ready.snapshot_epoch = snapshot_one;
    old_ready.final_sequence = 2U;
    old_ready.snapshot_hash = hash_one;

    ASSERT_TRUE(make_snapshot(&snapshot_two, &epoch, &config, 2U));
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot_two) == UCN_OK);
    ASSERT_TRUE(owner.mirror.committed_epoch.snapshot_id == 1U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_abort(&owner) == UCN_OK);
    ASSERT_TRUE(owner.mirror.committed_epoch.snapshot_id == 1U);

    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot_two) == UCN_OK);
    seed_sync_member(&member_two, 8U);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot_two, 1U,
                                                      &member_two) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_abort(&owner) == UCN_OK);
    ASSERT_TRUE(owner.mirror.committed_members.slots[0U].node_id == 7U);

    ASSERT_TRUE(ucn_cluster_backup_sync_owner_begin(&owner, epoch.head_node_id,
                                                     0U, &snapshot_two) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_member(&owner, epoch.head_node_id,
                                                      &snapshot_two, 1U,
                                                      &member_two) == UCN_OK);
    hash_two = ucn_cluster_backup_sync_member_hash_update(UINT32_C(2166136261),
                                                           &member_two);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot_two, 3U, 1U,
                                                   hash_two, &coverage) ==
                UCN_ERR_REPLAY);
    ASSERT_TRUE(owner.mirror.committed_epoch.snapshot_id == 1U);
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
        &refreshed_config, 7U, (const ucn_node_id_t[]){1U, 2U, 3U}, 3U));
    owner.active_config = refreshed_config;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_end(&owner, epoch.head_node_id,
                                                   &snapshot_two, 2U, 1U,
                                                   hash_two, &coverage) ==
                UCN_ERR_REPLAY);
    ASSERT_TRUE(owner.mirror.committed_epoch.snapshot_id == 1U);
    owner.active_config = config;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_abort(&owner) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_verify_ready(&owner, &old_ready) ==
                UCN_OK);

    bad_delta.snapshot_epoch = snapshot_one;
    bad_delta.snapshot_epoch.config_hash++;
    bad_delta.sequence = 2U;
    bad_delta.previous_snapshot_hash = hash_one;
    bad_delta.resulting_snapshot_hash = hash_two;
    bad_delta.member = member_two;
    ASSERT_TRUE(ucn_cluster_backup_sync_owner_delta(&owner, epoch.head_node_id,
                                                     &bad_delta) == UCN_ERR_REPLAY);
    ASSERT_TRUE(owner.mirror.committed_epoch.snapshot_id == 1U);

    ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch,
                                                    &refreshed_config) == UCN_OK);
    ASSERT_TRUE(!ucn_cluster_backup_sync_owner_takeover_eligible(&owner));
    return 0;
}

int main(void)
{
    ASSERT_TRUE(test_begin_only_opens_staging() == 0);
    ASSERT_TRUE(test_begin_rejections_do_not_write_owner() == 0);
    ASSERT_TRUE(test_init_reassignment_discards_local_mirror() == 0);
    ASSERT_TRUE(test_member_end_only_swap_after_full_proof() == 0);
    ASSERT_TRUE(test_ready_binds_exact_committed_snapshot() == 0);
    ASSERT_TRUE(test_empty_snapshot_end_uses_control_sequence_one() == 0);
    ASSERT_TRUE(test_delta_requires_exact_snapshot_and_gap_resync() == 0);
    ASSERT_TRUE(test_coverage_grace_and_serial_exhaustion() == 0);
    ASSERT_TRUE(test_removed_protected_voter_is_immediately_ineligible() == 0);
    ASSERT_TRUE(test_missing_stable_protected_voter_is_immediately_ineligible() ==
                0);
    ASSERT_TRUE(test_missing_joint_protected_voter_is_immediately_ineligible() ==
                0);
    ASSERT_TRUE(test_primary_failure_matrix_preserves_last_committed() == 0);
    (void)puts("All Cluster Backup sync tests passed");
    return 0;
}
