#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_config_proposal.h"

#define ASSERT_TRUE(condition) do { if (!(condition)) { printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); return 1; } } while (0)

static bool make_stable(ucn_cluster_config_state_t *stable)
{
    static const ucn_node_id_t voters[] = { 1U, 4U, 9U };

    return ucn_cluster_config_state_init_stable(
        stable, 8U, voters, sizeof(voters) / sizeof(voters[0U]));
}

static void make_provisional(ucn_cluster_member_t *member,
                             ucn_node_id_t node_id)
{
    (void)memset(member, 0, sizeof(*member));
    member->occupied = true;
    member->provisional_deadline_armed = true;
    member->status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    member->node_id = node_id;
    member->provisional_deadline_ms = 1000U;
}

static void make_committed_v4(ucn_cluster_member_t *member,
                              ucn_node_id_t node_id)
{
    (void)memset(member, 0, sizeof(*member));
    member->occupied = true;
    member->voting = true;
    member->status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    member->node_id = node_id;
}

static int test_add_provisional_builds_joint_and_self_ack(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_member_t member;
    ucn_cluster_member_t member_before;

    ASSERT_TRUE(make_stable(&stable));
    make_provisional(&member, 21U);
    member_before = member;
    ucn_cluster_config_tx_init_empty(&tx);
    ASSERT_TRUE(ucn_cluster_config_tx_begin_add_provisional(
                    &tx, 55U, 4U, &member, &stable,
                    UCN_CLUSTER_MAX_VOTERS, 500U) == UCN_OK);
    ASSERT_TRUE(tx.proposal_kind ==
                    (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_ADD &&
                tx.proposal_node_id == 21U &&
                tx.proposed_config.phase ==
                    (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT &&
                tx.proposed_config.config_id == 9U &&
                tx.proposed_config.old_set.config_id == 8U &&
                tx.proposed_config.new_set.count == 4U &&
                ucn_cluster_voter_set_contains(&tx.proposed_config.new_set,
                                               21U));
    ASSERT_TRUE(tx.old_ack_bitmap == (UINT64_C(1) << 1U) &&
                tx.new_ack_bitmap == (UINT64_C(1) << 1U));
    ASSERT_TRUE(memcmp(&member, &member_before, sizeof(member)) == 0 &&
                member.status ==
                    (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL &&
                !member.voting);
    return 0;
}

static int test_invalid_candidate_or_capacity_has_no_write(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_tx_t before;
    ucn_cluster_member_t member;

    ASSERT_TRUE(make_stable(&stable));
    make_provisional(&member, 21U);
    ucn_cluster_config_tx_init_empty(&tx);
    before = tx;
    ASSERT_TRUE(ucn_cluster_config_tx_begin_add_provisional(
                    &tx, 55U, 77U, &member, &stable,
                    UCN_CLUSTER_MAX_VOTERS, 500U) == UCN_ERR_ARGUMENT &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    member.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V3;
    ASSERT_TRUE(ucn_cluster_config_tx_begin_add_provisional(
                    &tx, 55U, 4U, &member, &stable,
                    UCN_CLUSTER_MAX_VOTERS, 500U) == UCN_ERR_ARGUMENT &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    make_provisional(&member, 21U);
    ASSERT_TRUE(ucn_cluster_config_tx_begin_add_provisional(
                    &tx, 55U, 4U, &member, &stable, 3U, 500U) ==
                    UCN_ERR_ARGUMENT &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    return 0;
}

static int test_removal_retains_old_denominator_until_commit(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_member_t member;
    ucn_cluster_member_t member_before;

    ASSERT_TRUE(make_stable(&stable));
    make_committed_v4(&member, 9U);
    ASSERT_TRUE(ucn_cluster_config_member_mark_removing(&member) == UCN_OK &&
                member.voting &&
                member.status ==
                    (uint8_t)UCN_CLUSTER_MEMBER_STATUS_REMOVING);
    member_before = member;
    ucn_cluster_config_tx_init_empty(&tx);
    ASSERT_TRUE(ucn_cluster_config_tx_begin_remove_marked(
                    &tx, 56U, 4U, &member, &stable, 500U) == UCN_OK &&
                tx.proposal_kind ==
                    (uint8_t)UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE &&
                tx.proposed_config.old_set.count == 3U &&
                tx.proposed_config.new_set.count == 2U &&
                ucn_cluster_voter_set_contains(&tx.proposed_config.old_set,
                                               9U) &&
                !ucn_cluster_voter_set_contains(&tx.proposed_config.new_set,
                                                9U) &&
                tx.old_ack_bitmap == (UINT64_C(1) << 1U) &&
                tx.new_ack_bitmap == (UINT64_C(1) << 1U) &&
                memcmp(&member, &member_before, sizeof(member)) == 0);
    return 0;
}

static int test_removal_rejects_unmarked_or_head_without_write(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_tx_t before;
    ucn_cluster_member_t member;

    ASSERT_TRUE(make_stable(&stable));
    make_committed_v4(&member, 9U);
    ucn_cluster_config_tx_init_empty(&tx);
    before = tx;
    ASSERT_TRUE(ucn_cluster_config_tx_begin_remove_marked(
                    &tx, 56U, 4U, &member, &stable, 500U) ==
                    UCN_ERR_ARGUMENT &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    make_committed_v4(&member, 4U);
    ASSERT_TRUE(ucn_cluster_config_member_mark_removing(&member) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_config_tx_begin_remove_marked(
                    &tx, 56U, 4U, &member, &stable, 500U) ==
                    UCN_ERR_ARGUMENT &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    return 0;
}

static int test_rotation_boundary_requires_m13_without_write(void)
{
    static const ucn_node_id_t voters[] = { 1U, 4U, 9U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_tx_t before;
    ucn_cluster_member_t provisional;
    ucn_cluster_member_t removing;

    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
        &stable, UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD, voters,
        sizeof(voters) / sizeof(voters[0U])));
    make_provisional(&provisional, 21U);
    ucn_cluster_config_tx_init_empty(&tx);
    before = tx;
    ASSERT_TRUE(ucn_cluster_config_tx_begin_add_provisional(
                    &tx, 1U, 4U, &provisional, &stable,
                    UCN_CLUSTER_MAX_VOTERS, 500U) == UCN_ERR_EXHAUSTED &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    make_committed_v4(&removing, 9U);
    ASSERT_TRUE(ucn_cluster_config_member_mark_removing(&removing) == UCN_OK &&
                ucn_cluster_config_tx_begin_remove_marked(
                    &tx, 1U, 4U, &removing, &stable, 500U) ==
                    UCN_ERR_EXHAUSTED &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_add_provisional_builds_joint_and_self_ack();
    result |= test_invalid_candidate_or_capacity_has_no_write();
    result |= test_removal_retains_old_denominator_until_commit();
    result |= test_removal_rejects_unmarked_or_head_without_write();
    result |= test_rotation_boundary_requires_m13_without_write();
    if (result == 0) {
        printf("Cluster config proposal tests passed.\\n");
    }
    return result;
}
