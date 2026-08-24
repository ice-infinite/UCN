#include "test_support.h"

#include <string.h>

#include "ucn/ucn_cluster_invariant.h"
#include "ucn/ucn_cluster_storage.h"

static int expect_violation(const ucn_cluster_t *cluster, uint32_t expected)
{
    uint32_t mask = UINT32_C(0xFFFFFFFF);

    TEST_ASSERT(ucn_cluster_invariant_check(cluster, 0U, &mask) == UCN_OK);
    TEST_ASSERT((mask & expected) != 0U);
    return 0;
}

static int test_clean_and_invalid_arguments(void)
{
    ucn_cluster_t cluster;
    uint32_t mask = UINT32_C(0xA5A5A5A5);

    (void)memset(&cluster, 0, sizeof(cluster));
    TEST_ASSERT(ucn_cluster_invariant_check(&cluster, 0U, &mask) == UCN_OK);
    TEST_ASSERT(mask == 0U);
    mask = UINT32_C(0xA5A5A5A5);
    TEST_ASSERT(ucn_cluster_invariant_check(NULL, 0U, &mask) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(mask == UINT32_C(0xA5A5A5A5));
    return 0;
}

static int test_fault_categories(void)
{
    ucn_cluster_t cluster;

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.authority_active = true;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_1_SINGLE_AUTHORITY) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.authority_active = true;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_2_AUTHORITY_QUORUM) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    cluster.authority_active = true;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_3_TAKEOVER_MAJORITY) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.phase = UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    cluster.cluster_id = 44U;
    cluster.recovery_cluster_id = 44U;
    cluster.parent_cluster_id = 44U;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_4_RECOVERY_ISOLATION) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.member_voted_cluster_id = 9U;
    cluster.member_voted_term = 2U;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_5_PERSISTENT_VOTE) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.active_voter_set.config_id = 1U;
    cluster.active_voter_set.count = 1U;
    cluster.active_voter_set.node_ids[0] = 1U;
    cluster.active_voter_set.hash = 0U;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_6_CONFIG_SAFETY) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.last_cluster_id = 9U;
    cluster.max_seen_term = 2U;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_7_REPLAY_ISOLATION) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.authority_active = true;
    cluster.authority_phase = UCN_CLUSTER_PHASE_HEAD_FENCED;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_8_FENCE_ORDERING) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.backup_generation = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_9_NO_SERIAL_REUSE) == 0);

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.config.persistence_mode = UCN_CLUSTER_PERSISTENCE_REQUIRED;
    cluster.persistence_pending = true;
    TEST_ASSERT(expect_violation(
                    &cluster,
                    UCN_CLUSTER_INVARIANT_SAFETY_10_PERSIST_BEFORE_PROMISE) ==
                0);
    return 0;
}

static int test_network_single_authority(void)
{
    ucn_cluster_t first;
    ucn_cluster_t second;
    const ucn_cluster_t *nodes[2];
    uint32_t mask = 0U;

    (void)memset(&first, 0, sizeof(first));
    (void)memset(&second, 0, sizeof(second));
    first.cluster_id = 88U;
    second.cluster_id = 88U;
    first.authority_active = true;
    second.authority_active = true;
    nodes[0] = &first;
    nodes[1] = &second;
    TEST_ASSERT(ucn_cluster_invariant_check_network(nodes, 2U, 0U, &mask) ==
                UCN_OK);
    TEST_ASSERT((mask & UCN_CLUSTER_INVARIANT_SAFETY_1_SINGLE_AUTHORITY) != 0U);
    return 0;
}

int test_cluster_invariant(void)
{
    int result = 0;

    result |= test_clean_and_invalid_arguments();
    result |= test_fault_categories();
    result |= test_network_single_authority();
    return result;
}
