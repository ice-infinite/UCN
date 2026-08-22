#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_config_tx.h"

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\\n", #condition, __FILE__, \
                   __LINE__); \
            return 1; \
        } \
    } while (0)

static bool make_configs(ucn_cluster_config_state_t *stable,
                         ucn_cluster_config_state_t *joint)
{
    static const ucn_node_id_t old_nodes[] = { 1U, 4U, 9U };
    static const ucn_node_id_t new_nodes[] = { 1U, 4U, 9U, 21U };

    return ucn_cluster_config_state_init_stable(
               stable, 8U, old_nodes,
               sizeof(old_nodes) / sizeof(old_nodes[0U])) &&
           ucn_cluster_config_state_init_joint(
               joint, stable, new_nodes,
               sizeof(new_nodes) / sizeof(new_nodes[0U]));
}

static int test_single_transaction_and_ack_binding(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_state_t joint;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_tx_t before;

    ASSERT_TRUE(make_configs(&stable, &joint));
    ucn_cluster_config_tx_init_empty(&tx);
    ASSERT_TRUE(ucn_cluster_config_tx_is_valid(&tx) &&
                !ucn_cluster_config_tx_is_active(&tx));
    ASSERT_TRUE(ucn_cluster_config_tx_begin(
                    &tx, 55U, UCN_CLUSTER_CONFIG_PROPOSAL_ADD, 21U, &stable,
                    &joint, 1000U) == UCN_OK &&
                ucn_cluster_config_tx_is_active(&tx));
    before = tx;
    ASSERT_TRUE(ucn_cluster_config_tx_begin(
                    &tx, 56U, UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE, 4U,
                    &stable, &joint, 2000U) == UCN_ERR_STATE &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    ASSERT_TRUE(ucn_cluster_config_tx_record_ack(&tx, 1U) == UCN_OK &&
                tx.old_ack_bitmap == UINT64_C(1) &&
                tx.new_ack_bitmap == UINT64_C(1));
    ASSERT_TRUE(ucn_cluster_config_tx_record_ack(&tx, 21U) == UCN_OK &&
                tx.old_ack_bitmap == UINT64_C(1) &&
                tx.new_ack_bitmap ==
                    (UINT64_C(1) | (UINT64_C(1) << 3U)));
    before = tx;
    ASSERT_TRUE(ucn_cluster_config_tx_record_ack(&tx, 77U) == UCN_ERR_NOT_FOUND &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    ASSERT_TRUE(ucn_cluster_config_tx_record_ack(&tx, 21U) == UCN_ERR_NOT_FOUND &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    return 0;
}

static int test_deadline_retry_persistence_and_no_write(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_state_t joint;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_tx_t before;

    ASSERT_TRUE(make_configs(&stable, &joint));
    ucn_cluster_config_tx_init_empty(&tx);
    ASSERT_TRUE(ucn_cluster_config_tx_begin(
                    &tx, 55U, UCN_CLUSTER_CONFIG_PROPOSAL_ADD, 21U, &stable,
                    &joint, 1000U) == UCN_OK);
    ASSERT_TRUE(!ucn_cluster_config_tx_is_expired(&tx, 999U) &&
                ucn_cluster_config_tx_is_expired(&tx, 1000U));
    /* Modular time must not make a transaction expire early while the clock
     * crosses UINT32_MAX.  This models a deadline created just before wrap. */
    tx.deadline_ms = 3U;
    ASSERT_TRUE(!ucn_cluster_config_tx_is_expired(&tx, UINT32_MAX - 1U) &&
                !ucn_cluster_config_tx_is_expired(&tx, 0U) &&
                !ucn_cluster_config_tx_is_expired(&tx, 2U) &&
                ucn_cluster_config_tx_is_expired(&tx, 3U));
    ASSERT_TRUE(ucn_cluster_config_tx_schedule_retry(&tx, 400U) == UCN_OK &&
                tx.retry_due_ms == 400U && tx.retry_count == 1U);
    ASSERT_TRUE(ucn_cluster_config_tx_set_persist_stage(
                    &tx, UCN_CLUSTER_CONFIG_PERSIST_STAGE_PREPARE_PENDING) ==
                UCN_OK &&
                tx.persist_stage ==
                    (uint8_t)UCN_CLUSTER_CONFIG_PERSIST_STAGE_PREPARE_PENDING);
    before = tx;
    tx.retry_count = UINT8_MAX;
    ASSERT_TRUE(ucn_cluster_config_tx_schedule_retry(&tx, 401U) ==
                UCN_ERR_EXHAUSTED && tx.retry_due_ms == before.retry_due_ms &&
                tx.retry_count == UINT8_MAX);
    tx = before;
    tx.old_ack_bitmap = UINT64_C(1) << 63U;
    ASSERT_TRUE(!ucn_cluster_config_tx_is_valid(&tx));
    return 0;
}

static int test_rejects_multi_member_or_wrong_kind_delta(void)
{
    static const ucn_node_id_t replacement_nodes[] = { 1U, 4U, 22U, 23U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_state_t malformed_add;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_tx_t before;

    ASSERT_TRUE(make_configs(&stable, &malformed_add));
    ASSERT_TRUE(ucn_cluster_config_state_init_joint(
        &malformed_add, &stable, replacement_nodes,
        sizeof(replacement_nodes) / sizeof(replacement_nodes[0U])));
    ucn_cluster_config_tx_init_empty(&tx);
    before = tx;
    ASSERT_TRUE(ucn_cluster_config_tx_begin(
                    &tx, 56U, UCN_CLUSTER_CONFIG_PROPOSAL_ADD, 21U, &stable,
                    &malformed_add, 1000U) == UCN_ERR_ARGUMENT &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    ASSERT_TRUE(ucn_cluster_config_tx_begin(
                    &tx, 56U, UCN_CLUSTER_CONFIG_PROPOSAL_REMOVE, 9U,
                    &stable, &malformed_add, 1000U) == UCN_ERR_ARGUMENT &&
                memcmp(&tx, &before, sizeof(tx)) == 0);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_single_transaction_and_ack_binding();
    result |= test_deadline_retry_persistence_and_no_write();
    result |= test_rejects_multi_member_or_wrong_kind_delta();
    if (result == 0) {
        printf("Cluster config transaction tests passed.\n");
    }
    return result;
}
