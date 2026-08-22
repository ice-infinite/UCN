#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_config_proposal.h"
#include "ucn/ucn_cluster_config_quorum.h"

#define ASSERT_TRUE(condition) do { if (!(condition)) { printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); return 1; } } while (0)

static bool make_add_tx(ucn_cluster_config_tx_t *tx)
{
    static const ucn_node_id_t voters[] = { 1U, 4U, 9U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_member_t provisional;

    if (!ucn_cluster_config_state_init_stable(
            &stable, 8U, voters, sizeof(voters) / sizeof(voters[0U]))) {
        return false;
    }
    (void)memset(&provisional, 0, sizeof(provisional));
    provisional.occupied = true;
    provisional.provisional_deadline_armed = true;
    provisional.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    provisional.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    provisional.node_id = 21U;
    provisional.provisional_deadline_ms = 500U;
    ucn_cluster_config_tx_init_empty(tx);
    return ucn_cluster_config_tx_begin_add_provisional(
               tx, 55U, 4U, &provisional, &stable,
               UCN_CLUSTER_MAX_VOTERS, 1000U) == UCN_OK;
}

static int test_both_quorums_required(void)
{
    ucn_cluster_config_tx_t tx;

    ASSERT_TRUE(make_add_tx(&tx));
    /* Head=4 self ACK is one vote in both sets. */
    ASSERT_TRUE(!ucn_cluster_config_joint_quorum_reached(&tx));
    ASSERT_TRUE(ucn_cluster_config_tx_record_ack(&tx, 1U) == UCN_OK);
    /* C_old reaches 2/3, C_new only has 2/4. */
    ASSERT_TRUE(ucn_cluster_config_bitmap_reaches_quorum(
                    &tx.proposed_config.old_set, tx.old_ack_bitmap) &&
                !ucn_cluster_config_bitmap_reaches_quorum(
                    &tx.proposed_config.new_set, tx.new_ack_bitmap) &&
                !ucn_cluster_config_joint_quorum_reached(&tx));
    ASSERT_TRUE(ucn_cluster_config_tx_record_ack(&tx, 21U) == UCN_OK &&
                ucn_cluster_config_joint_quorum_reached(&tx));
    return 0;
}

static int test_new_only_votes_cannot_replace_old_quorum(void)
{
    ucn_cluster_config_tx_t tx;

    ASSERT_TRUE(make_add_tx(&tx));
    tx.old_ack_bitmap = UINT64_C(1) << 1U;
    tx.new_ack_bitmap = (UINT64_C(1) << 0U) | (UINT64_C(1) << 2U) |
                        (UINT64_C(1) << 3U);
    ASSERT_TRUE(ucn_cluster_config_tx_is_valid(&tx) &&
                !ucn_cluster_config_bitmap_reaches_quorum(
                    &tx.proposed_config.old_set, tx.old_ack_bitmap) &&
                ucn_cluster_config_bitmap_reaches_quorum(
                    &tx.proposed_config.new_set, tx.new_ack_bitmap) &&
                !ucn_cluster_config_joint_quorum_reached(&tx));
    tx.new_ack_bitmap |= UINT64_C(1) << 63U;
    ASSERT_TRUE(!ucn_cluster_config_bitmap_reaches_quorum(
        &tx.proposed_config.new_set, tx.new_ack_bitmap));
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_both_quorums_required();
    result |= test_new_only_votes_cannot_replace_old_quorum();
    if (result == 0) {
        printf("Cluster config quorum tests passed.\n");
    }
    return result;
}
