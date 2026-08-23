#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_backup_mirror.h"

#define ASSERT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                      \
            (void)fprintf(stderr, "ASSERT %s at %s:%d\\n", #condition,        \
                          __FILE__, __LINE__);                                   \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static void seed_provisional_member(ucn_cluster_member_table_t *table,
                                    ucn_node_id_t node_id)
{
    ucn_cluster_member_t *member = &table->slots[0];

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

static bool make_snapshot_epoch(ucn_cluster_snapshot_epoch_t *output,
                                uint32_t snapshot_id,
                                uint32_t config_id)
{
    const ucn_node_id_t voters[] = {1U, 2U, 3U};
    ucn_cluster_config_state_t config;
    ucn_cluster_backup_epoch_t backup_epoch;

    (void)memset(&config, 0, sizeof(config));
    (void)memset(&backup_epoch, 0, sizeof(backup_epoch));
    backup_epoch.cluster_id = 11U;
    backup_epoch.term = 7U;
    backup_epoch.head_node_id = 1U;
    backup_epoch.backup_node_id = 2U;
    backup_epoch.backup_generation = 3U;
    return ucn_cluster_config_state_init_stable(&config, config_id, voters,
                                                sizeof(voters) / sizeof(voters[0U])) &&
           ucn_cluster_snapshot_epoch_from_config(output, &backup_epoch,
                                                  snapshot_id, &config);
}

static int test_reset_is_canonical(void)
{
    ucn_cluster_backup_mirror_t mirror;
    ucn_cluster_backup_mirror_t expected;

    (void)memset(&mirror, 0xA5, sizeof(mirror));
    (void)memset(&expected, 0, sizeof(expected));
    ucn_cluster_backup_mirror_reset(&mirror);
    ASSERT_TRUE(memcmp(&mirror, &expected, sizeof(mirror)) == 0);
    ASSERT_TRUE(ucn_cluster_backup_mirror_is_valid(&mirror));
    ASSERT_TRUE(ucn_cluster_backup_mirror_committed(&mirror) == NULL);
    ASSERT_TRUE(ucn_cluster_backup_mirror_staging(&mirror) == NULL);
    return 0;
}

static int test_staging_never_mutates_committed(void)
{
    ucn_cluster_backup_mirror_t mirror;
    ucn_cluster_member_table_t committed_before;
    ucn_cluster_snapshot_epoch_t staging_epoch;
    ucn_cluster_member_table_t *staging;

    ucn_cluster_backup_mirror_reset(&mirror);
    seed_provisional_member(&mirror.committed_members, 7U);
    mirror.committed_valid = true;
    ASSERT_TRUE(make_snapshot_epoch(&mirror.committed_epoch, 1U, 4U));
    ASSERT_TRUE(ucn_cluster_backup_mirror_is_valid(&mirror));
    committed_before = mirror.committed_members;

    ASSERT_TRUE(make_snapshot_epoch(&staging_epoch, 2U, 4U));
    ASSERT_TRUE(ucn_cluster_backup_mirror_begin_staging(&mirror,
                                                        &staging_epoch) == UCN_OK);
    ASSERT_TRUE(mirror.staging_active);
    ASSERT_TRUE(memcmp(&mirror.committed_members, &committed_before,
                       sizeof(committed_before)) == 0);
    ASSERT_TRUE(ucn_cluster_backup_mirror_committed(&mirror) ==
                &mirror.committed_members);
    staging = ucn_cluster_backup_mirror_staging(&mirror);
    ASSERT_TRUE(staging == &mirror.staging_members);
    seed_provisional_member(staging, 9U);
    ASSERT_TRUE(ucn_cluster_backup_mirror_is_valid(&mirror));

    ASSERT_TRUE(ucn_cluster_backup_mirror_abort_staging(&mirror) == UCN_OK);
    ASSERT_TRUE(!mirror.staging_active);
    ASSERT_TRUE(ucn_cluster_backup_mirror_staging(&mirror) == NULL);
    ASSERT_TRUE(ucn_cluster_backup_mirror_staging_epoch(&mirror) == NULL);
    ASSERT_TRUE(memcmp(&mirror.committed_members, &committed_before,
                       sizeof(committed_before)) == 0);
    ASSERT_TRUE(ucn_cluster_backup_mirror_is_valid(&mirror));
    return 0;
}

static int test_invalid_state_fails_closed(void)
{
    ucn_cluster_backup_mirror_t mirror;
    ucn_cluster_backup_mirror_t before;
    ucn_cluster_snapshot_epoch_t staging_epoch;

    ucn_cluster_backup_mirror_reset(&mirror);
    mirror.committed_members.slots[0].node_id = 9U;
    before = mirror;
    ASSERT_TRUE(!ucn_cluster_backup_mirror_is_valid(&mirror));
    ASSERT_TRUE(make_snapshot_epoch(&staging_epoch, 1U, 4U));
    ASSERT_TRUE(ucn_cluster_backup_mirror_begin_staging(&mirror,
                                                        &staging_epoch) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&mirror, &before, sizeof(mirror)) == 0);
    ASSERT_TRUE(ucn_cluster_backup_mirror_abort_staging(&mirror) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&mirror, &before, sizeof(mirror)) == 0);
    return 0;
}

static int test_snapshot_epoch_config_binding_and_replay(void)
{
    const ucn_node_id_t voters[] = {1U, 2U, 3U};
    ucn_cluster_backup_mirror_t mirror;
    ucn_cluster_config_state_t config;
    ucn_cluster_config_state_t different_config;
    ucn_cluster_snapshot_epoch_t epoch;
    ucn_cluster_snapshot_epoch_t changed_epoch;
    ucn_cluster_backup_mirror_t before;

    (void)memset(&config, 0, sizeof(config));
    (void)memset(&different_config, 0, sizeof(different_config));
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(&config, 4U, voters,
                                                     sizeof(voters) / sizeof(voters[0U])));
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(&different_config, 5U,
                                                     voters,
                                                     sizeof(voters) / sizeof(voters[0U])));
    ASSERT_TRUE(make_snapshot_epoch(&epoch, 1U, 4U));
    ASSERT_TRUE(ucn_cluster_snapshot_epoch_is_valid(&epoch));
    ASSERT_TRUE(ucn_cluster_snapshot_epoch_matches_config(&epoch, &config));
    ASSERT_TRUE(!ucn_cluster_snapshot_epoch_matches_config(&epoch,
                                                           &different_config));

    ucn_cluster_backup_mirror_reset(&mirror);
    ASSERT_TRUE(ucn_cluster_backup_mirror_begin_staging(&mirror, &epoch) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_backup_mirror_staging_epoch(&mirror) != NULL);
    ASSERT_TRUE(ucn_cluster_snapshot_epoch_is_exact(
        ucn_cluster_backup_mirror_staging_epoch(&mirror), &epoch));
    ASSERT_TRUE(ucn_cluster_backup_mirror_begin_staging(&mirror, &epoch) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(ucn_cluster_backup_mirror_abort_staging(&mirror) == UCN_OK);

    seed_provisional_member(&mirror.committed_members, 7U);
    mirror.committed_epoch = epoch;
    mirror.committed_valid = true;
    ASSERT_TRUE(ucn_cluster_backup_mirror_is_valid(&mirror));
    before = mirror;
    ASSERT_TRUE(ucn_cluster_backup_mirror_begin_staging(&mirror, &epoch) ==
                UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&mirror, &before, sizeof(mirror)) == 0);
    changed_epoch = epoch;
    changed_epoch.snapshot_id = 2U;
    ASSERT_TRUE(ucn_cluster_backup_mirror_begin_staging(&mirror,
                                                        &changed_epoch) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_snapshot_epoch_is_exact(
        ucn_cluster_backup_mirror_staging_epoch(&mirror), &changed_epoch));
    return 0;
}

static int test_exact_commit_atomically_swaps_staging(void)
{
    ucn_cluster_backup_mirror_t mirror;
    ucn_cluster_snapshot_epoch_t staging_epoch;
    ucn_cluster_snapshot_epoch_t wrong_epoch;
    ucn_cluster_backup_mirror_t before;

    ucn_cluster_backup_mirror_reset(&mirror);
    ASSERT_TRUE(make_snapshot_epoch(&staging_epoch, 1U, 4U));
    ASSERT_TRUE(ucn_cluster_backup_mirror_begin_staging(&mirror,
                                                        &staging_epoch) == UCN_OK);
    seed_provisional_member(&mirror.staging_members, 9U);
    wrong_epoch = staging_epoch;
    wrong_epoch.snapshot_id++;
    before = mirror;
    ASSERT_TRUE(ucn_cluster_backup_mirror_commit_staging_exact(&mirror,
                                                                &wrong_epoch) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&mirror, &before, sizeof(mirror)) == 0);
    ASSERT_TRUE(ucn_cluster_backup_mirror_commit_staging_exact(&mirror,
                                                                &staging_epoch) ==
                UCN_OK);
    ASSERT_TRUE(mirror.committed_valid);
    ASSERT_TRUE(!mirror.staging_active);
    ASSERT_TRUE(ucn_cluster_backup_mirror_committed(&mirror)->slots[0U].node_id ==
                9U);
    ASSERT_TRUE(ucn_cluster_backup_mirror_staging(&mirror) == NULL);
    ASSERT_TRUE(ucn_cluster_snapshot_epoch_is_exact(
        ucn_cluster_backup_mirror_committed_epoch(&mirror), &staging_epoch));
    return 0;
}

static int test_role_storage_union_budget(void)
{
    ASSERT_TRUE(sizeof(ucn_cluster_member_role_storage_t) ==
                sizeof(ucn_cluster_backup_mirror_t));
    ASSERT_TRUE(sizeof(ucn_cluster_member_role_storage_t) >=
                sizeof(ucn_cluster_member_table_t));
    return 0;
}

int main(void)
{
    ASSERT_TRUE(test_reset_is_canonical() == 0);
    ASSERT_TRUE(test_staging_never_mutates_committed() == 0);
    ASSERT_TRUE(test_invalid_state_fails_closed() == 0);
    ASSERT_TRUE(test_snapshot_epoch_config_binding_and_replay() == 0);
    ASSERT_TRUE(test_exact_commit_atomically_swaps_staging() == 0);
    ASSERT_TRUE(test_role_storage_union_budget() == 0);
    (void)puts("All Cluster Backup mirror tests passed");
    return 0;
}
