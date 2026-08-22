#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_config_backup.h"
#include "ucn/ucn_cluster_config_proposal.h"

#define ASSERT_TRUE(condition) do { if (!(condition)) { printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); return 1; } } while (0)

static bool make_add_tx(ucn_cluster_config_tx_t *tx)
{
    static const ucn_node_id_t voters[] = { 1U, 4U, 9U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_member_t provisional;

    if (!ucn_cluster_config_state_init_stable(
            &stable, 1U, voters, sizeof(voters) / sizeof(voters[0U]))) {
        return false;
    }
    (void)memset(&provisional, 0, sizeof(provisional));
    provisional.occupied = true;
    provisional.provisional_deadline_armed = true;
    provisional.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    provisional.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    provisional.node_id = 21U;
    provisional.provisional_deadline_ms = 100U;
    ucn_cluster_config_tx_init_empty(tx);
    return ucn_cluster_config_tx_begin_add_provisional(
               tx, 1U, 4U, &provisional, &stable,
               UCN_CLUSTER_MAX_VOTERS, 1000U) == UCN_OK;
}

static bool make_conflicting_same_txid(const ucn_cluster_config_tx_t *base,
                                       ucn_cluster_config_tx_t *conflict)
{
    static const ucn_node_id_t voters[] = { 1U, 4U, 9U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_member_t provisional;

    if (base == NULL || conflict == NULL ||
        !ucn_cluster_config_state_init_stable(
            &stable, 1U, voters, sizeof(voters) / sizeof(voters[0U]))) {
        return false;
    }
    (void)memset(&provisional, 0, sizeof(provisional));
    provisional.occupied = true;
    provisional.provisional_deadline_armed = true;
    provisional.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    provisional.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    provisional.node_id = 22U;
    provisional.provisional_deadline_ms = 100U;
    ucn_cluster_config_tx_init_empty(conflict);
    return ucn_cluster_config_tx_begin_add_provisional(
               conflict, base->transaction_id, 4U, &provisional, &stable,
               UCN_CLUSTER_MAX_VOTERS, 1000U) == UCN_OK;
}

static void make_backup(ucn_cluster_member_t *member, ucn_node_id_t node_id)
{
    (void)memset(member, 0, sizeof(*member));
    member->occupied = true;
    member->voting = true;
    member->status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    member->node_id = node_id;
}

static int test_no_backup_is_explicitly_non_ha(void)
{
    ucn_cluster_config_backup_gate_t gate;
    ucn_cluster_config_tx_t tx;
    bool ha_ready = true;

    ASSERT_TRUE(make_add_tx(&tx));
    ucn_cluster_config_backup_gate_init(&gate, false);
    ASSERT_TRUE(ucn_cluster_config_backup_gate_stage(&gate, &tx) == UCN_OK &&
                ucn_cluster_config_backup_gate_commit_allowed(&gate, &ha_ready) &&
                ucn_cluster_config_backup_gate_commit_allowed_for_tx(
                    &gate, &tx, &ha_ready) &&
                !ha_ready);
    ucn_cluster_config_backup_gate_init(&gate, true);
    ASSERT_TRUE(ucn_cluster_config_backup_gate_stage(&gate, &tx) ==
                    UCN_ERR_STATE &&
                !ucn_cluster_config_backup_gate_commit_allowed(&gate, &ha_ready) &&
                !ucn_cluster_config_backup_gate_commit_allowed_for_tx(
                    &gate, &tx, &ha_ready) &&
                !ha_ready);
    return 0;
}

static int test_exact_backup_staging_ack_required(void)
{
    ucn_cluster_config_backup_gate_t gate;
    ucn_cluster_config_backup_gate_t before;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_member_t backup;
    ucn_cluster_persist_config_ref_t ref;
    ucn_cluster_config_tx_t conflicting_tx;
    bool ha_ready = false;

    ASSERT_TRUE(make_add_tx(&tx));
    make_backup(&backup, 9U);
    ucn_cluster_config_backup_gate_init(&gate, true);
    ASSERT_TRUE(ucn_cluster_config_backup_gate_set_backup(&gate, &backup) ==
                    UCN_OK &&
                ucn_cluster_config_backup_gate_stage(&gate, &tx) == UCN_OK &&
                !ucn_cluster_config_backup_gate_commit_allowed(&gate, &ha_ready));
    before = gate;
    ASSERT_TRUE(ucn_cluster_config_backup_gate_ack(
                    &gate, 4U, tx.transaction_id, &gate.staged_config) ==
                    UCN_ERR_ARGUMENT &&
                memcmp(&gate, &before, sizeof(gate)) == 0);
    ref = gate.staged_config;
    ref.digest[0U] ^= UINT8_C(1);
    ASSERT_TRUE(ucn_cluster_config_backup_gate_ack(
                    &gate, 9U, tx.transaction_id, &ref) == UCN_ERR_ARGUMENT &&
                memcmp(&gate, &before, sizeof(gate)) == 0);
    ASSERT_TRUE(ucn_cluster_config_backup_gate_ack(
                    &gate, 9U, tx.transaction_id, &gate.staged_config) ==
                    UCN_OK &&
                ucn_cluster_config_backup_gate_commit_allowed(&gate, &ha_ready) &&
                ucn_cluster_config_backup_gate_commit_allowed_for_tx(
                    &gate, &tx, &ha_ready) &&
                ha_ready);
    ASSERT_TRUE(make_conflicting_same_txid(&tx, &conflicting_tx));
    before = gate;
    ASSERT_TRUE(!ucn_cluster_config_backup_gate_commit_allowed_for_tx(
                    &gate, &conflicting_tx, &ha_ready) &&
                !ha_ready && memcmp(&gate, &before, sizeof(gate)) == 0);
    gate.transaction_id++;
    ASSERT_TRUE(!ucn_cluster_config_backup_gate_commit_allowed_for_tx(
                    &gate, &tx, &ha_ready) && !ha_ready);
    gate = before;
    gate.acknowledged_backup_node_id = 4U;
    ASSERT_TRUE(!ucn_cluster_config_backup_gate_commit_allowed_for_tx(
                    &gate, &tx, &ha_ready) && !ha_ready);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_no_backup_is_explicitly_non_ha();
    result |= test_exact_backup_staging_ack_required();
    if (result == 0) {
        printf("Cluster config backup tests passed.\n");
    }
    return result;
}
