#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_config_joint.h"
#include "ucn/ucn_cluster_config_proposal.h"

#define ASSERT_TRUE(condition) do { if (!(condition)) { printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); return 1; } } while (0)

typedef struct fake_provider {
    ucn_cluster_persist_state_t stored;
    uint32_t submit_count;
} fake_provider_t;

static ucn_cluster_config_store_t config_store;

static ucn_result_t fake_load(void *context,
                              ucn_cluster_persist_load_result_t *result)
{
    fake_provider_t *fake = (fake_provider_t *)context;

    if (fake == NULL || result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(result, 0, sizeof(*result));
    result->state = UCN_CLUSTER_PERSIST_LOAD_READY;
    result->snapshot = fake->stored;
    return UCN_OK;
}

static ucn_cluster_persist_completion_t fake_submit(
    void *context,
    const ucn_cluster_persist_request_t *request)
{
    fake_provider_t *fake = (fake_provider_t *)context;
    ucn_cluster_persist_completion_t completion;

    (void)memset(&completion, 0, sizeof(completion));
    if (fake == NULL || request == NULL) {
        completion.state = UCN_CLUSTER_PERSIST_FAILED;
        completion.failure = UCN_ERR_ARGUMENT;
        return completion;
    }
    fake->submit_count++;
    fake->stored = request->next_state;
    completion.state = UCN_CLUSTER_PERSIST_COMMITTED;
    return completion;
}

static bool make_fixture(ucn_cluster_config_state_t *stable,
                         ucn_cluster_config_tx_t *tx,
                         fake_provider_t *fake,
                         ucn_cluster_persist_provider_t *provider)
{
    static const ucn_node_id_t voters[] = { 1U, 4U, 9U };
    ucn_cluster_member_t provisional;
    ucn_cluster_persist_config_ref_t base_ref;

    if (!ucn_cluster_config_state_init_stable(
            stable, 1U, voters, sizeof(voters) / sizeof(voters[0U]))) {
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
    if (ucn_cluster_config_tx_begin_add_provisional(
            tx, 1U, 4U, &provisional, stable, UCN_CLUSTER_MAX_VOTERS,
            1000U) != UCN_OK ||
        ucn_cluster_config_persist_ref_from_state(stable, &base_ref) != UCN_OK) {
        return false;
    }
    (void)memset(fake, 0, sizeof(*fake));
    ucn_cluster_config_store_init_empty(&config_store);
    if (ucn_cluster_config_store_write_stable(&config_store, stable, &base_ref) !=
        UCN_OK) {
        return false;
    }
    ucn_cluster_persist_state_init_empty(&fake->stored);
    fake->stored.boot_incarnation = 1U;
    fake->stored.committed_config = base_ref;
    (void)memset(provider, 0, sizeof(*provider));
    provider->struct_size = (uint16_t)sizeof(*provider);
    provider->api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider->load = fake_load;
    provider->submit = fake_submit;
    provider->context = fake;
    return ucn_cluster_persist_state_is_valid(&fake->stored);
}

static bool make_removal_fixture(ucn_cluster_config_state_t *stable,
                                 ucn_cluster_config_tx_t *tx,
                                 ucn_cluster_member_t *removing_member,
                                 fake_provider_t *fake,
                                 ucn_cluster_persist_provider_t *provider)
{
    static const ucn_node_id_t voters[] = { 1U, 4U, 9U };
    ucn_cluster_persist_config_ref_t base_ref;

    if (!ucn_cluster_config_state_init_stable(
            stable, 1U, voters, sizeof(voters) / sizeof(voters[0U]))) {
        return false;
    }
    (void)memset(removing_member, 0, sizeof(*removing_member));
    removing_member->occupied = true;
    removing_member->voting = true;
    removing_member->status =
        (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    removing_member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    removing_member->node_id = 9U;
    if (ucn_cluster_config_member_mark_removing(removing_member) != UCN_OK) {
        return false;
    }
    ucn_cluster_config_tx_init_empty(tx);
    if (ucn_cluster_config_tx_begin_remove_marked(
            tx, 1U, 1U, removing_member, stable, 1000U) != UCN_OK ||
        ucn_cluster_config_persist_ref_from_state(stable, &base_ref) != UCN_OK) {
        return false;
    }
    (void)memset(fake, 0, sizeof(*fake));
    ucn_cluster_config_store_init_empty(&config_store);
    if (ucn_cluster_config_store_write_stable(&config_store, stable, &base_ref) !=
        UCN_OK) {
        return false;
    }
    ucn_cluster_persist_state_init_empty(&fake->stored);
    fake->stored.boot_incarnation = 1U;
    fake->stored.committed_config = base_ref;
    (void)memset(provider, 0, sizeof(*provider));
    provider->struct_size = (uint16_t)sizeof(*provider);
    provider->api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider->load = fake_load;
    provider->submit = fake_submit;
    provider->context = fake;
    return ucn_cluster_persist_state_is_valid(&fake->stored);
}

static int test_joint_requires_quorum_and_durable_prepare(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_joint_runtime_t runtime;
    ucn_cluster_config_joint_runtime_t before;
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_backup_gate_t backup_gate;
    ucn_cluster_config_backup_gate_t stale_gate;
    ucn_cluster_member_t provisional;
    ucn_cluster_member_t backup_member;
    bool durable;

    ASSERT_TRUE(make_fixture(&stable, &tx, &fake, &provider));
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_init(&runtime, &stable) ==
                UCN_OK &&
                ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                    UCN_OK);
    before = runtime;
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_enter(&runtime, &owner, &tx) ==
                    UCN_ERR_ARGUMENT &&
                memcmp(&runtime, &before, sizeof(runtime)) == 0);
    ASSERT_TRUE(ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_tx_record_ack(&tx, 1U) == UCN_OK &&
                ucn_cluster_config_tx_record_ack(&tx, 21U) == UCN_OK &&
                ucn_cluster_config_persist_begin_joint(
                    &owner, 2U, &tx, &durable) == UCN_OK && durable);
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_enter(&runtime, &owner, &tx) ==
                    UCN_OK &&
                runtime.joint_active &&
                runtime.active_config.phase ==
                    (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT &&
                runtime.transaction.persist_stage ==
                    (uint8_t)UCN_CLUSTER_CONFIG_PERSIST_STAGE_JOINT_DURABLE);
    (void)memset(&provisional, 0, sizeof(provisional));
    provisional.occupied = true;
    provisional.provisional_deadline_armed = true;
    provisional.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    provisional.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    provisional.node_id = 21U;
    provisional.provisional_deadline_ms = 100U;
    (void)memset(&backup_member, 0, sizeof(backup_member));
    backup_member.occupied = true;
    backup_member.voting = true;
    backup_member.status =
        (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    backup_member.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    backup_member.node_id = 9U;
    ucn_cluster_config_backup_gate_init(&backup_gate, true);
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &backup_gate, &durable) == UCN_ERR_STATE &&
                !durable &&
                ucn_cluster_config_backup_gate_set_backup(&backup_gate,
                                                          &backup_member) == UCN_OK &&
                ucn_cluster_config_backup_gate_stage(&backup_gate, &tx) == UCN_OK &&
                ucn_cluster_config_backup_gate_ack(
                    &backup_gate, 9U, tx.transaction_id,
                    &backup_gate.staged_config) == UCN_OK &&
                ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &backup_gate, &durable) == UCN_OK && durable);
    stale_gate = backup_gate;
    stale_gate.acknowledged_backup_node_id = 4U;
    before = runtime;
    {
        const ucn_cluster_member_t member_before = provisional;

        ASSERT_TRUE(ucn_cluster_config_joint_runtime_commit(
                        &runtime, &owner, &stale_gate, &provisional) ==
                        UCN_ERR_ARGUMENT &&
                    memcmp(&runtime, &before, sizeof(runtime)) == 0 &&
                    memcmp(&provisional, &member_before,
                           sizeof(provisional)) == 0);
    }
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_commit(
                    &runtime, &owner, &backup_gate, &provisional) == UCN_OK &&
                !runtime.joint_active &&
                runtime.active_config.phase ==
                    (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE &&
                provisional.voting &&
                provisional.status ==
                    (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED);
    before = runtime;
    {
        const ucn_cluster_member_t member_before = provisional;
        const uint32_t submit_before = fake.submit_count;
        const ucn_result_t replay_result =
            ucn_cluster_config_persist_begin_commit(&owner, 3U, &tx,
                                                    &runtime, &backup_gate,
                                                    &durable);
        const ucn_result_t apply_result =
            replay_result == UCN_OK ? ucn_cluster_config_joint_runtime_commit(
                                      &runtime, &owner, &backup_gate,
                                      &provisional) :
                                      UCN_ERR_STATE;

        ASSERT_TRUE(replay_result == UCN_OK && durable &&
                    fake.submit_count == submit_before &&
                    apply_result == UCN_OK &&
                    memcmp(&runtime, &before, sizeof(runtime)) == 0 &&
                    memcmp(&provisional, &member_before,
                           sizeof(provisional)) == 0);
    }
    return 0;
}

static int test_timeout_abort_returns_to_old_config(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_joint_runtime_t runtime;
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_member_t provisional;
    bool durable;

    ASSERT_TRUE(make_fixture(&stable, &tx, &fake, &provider));
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_init(&runtime, &stable) ==
                    UCN_OK &&
                ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                    UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_tx_record_ack(&tx, 1U) == UCN_OK &&
                ucn_cluster_config_tx_record_ack(&tx, 21U) == UCN_OK &&
                ucn_cluster_config_persist_begin_joint(
                    &owner, 2U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_joint_runtime_enter(&runtime, &owner, &tx) ==
                    UCN_OK);
    (void)memset(&provisional, 0, sizeof(provisional));
    provisional.occupied = true;
    provisional.provisional_deadline_armed = true;
    provisional.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    provisional.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    provisional.node_id = 21U;
    provisional.provisional_deadline_ms = 100U;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_abort(
                    &owner, 3U, &tx, tx.deadline_ms, &durable) == UCN_OK &&
                durable &&
                ucn_cluster_config_joint_runtime_abort(
                    &runtime, &owner, &provisional) == UCN_OK &&
                !runtime.joint_active &&
                runtime.active_config.config_id == stable.config_id &&
                runtime.transaction.phase ==
                    (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_ABORTED &&
                provisional.status ==
                    (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL &&
                !provisional.voting);
    {
        const ucn_cluster_config_joint_runtime_t runtime_before = runtime;
        const ucn_cluster_member_t member_before = provisional;
        const uint32_t submit_before = fake.submit_count;

        ASSERT_TRUE(ucn_cluster_config_persist_begin_abort(
                        &owner, 3U, &tx, tx.deadline_ms, &durable) == UCN_OK &&
                    durable && fake.submit_count == submit_before &&
                    ucn_cluster_config_joint_runtime_abort(
                        &runtime, &owner, &provisional) == UCN_OK &&
                    memcmp(&runtime, &runtime_before, sizeof(runtime)) == 0 &&
                    memcmp(&provisional, &member_before,
                           sizeof(provisional)) == 0);
    }
    return 0;
}

static int test_removal_commit_and_abort_have_terminal_member_rules(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_joint_runtime_t runtime;
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_backup_gate_t backup_gate;
    ucn_cluster_member_t removing_member;
    ucn_cluster_member_t empty_member;
    bool durable;

    ASSERT_TRUE(make_removal_fixture(&stable, &tx, &removing_member, &fake,
                                     &provider));
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_init(&runtime, &stable) ==
                    UCN_OK &&
                ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                    UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_tx_record_ack(&tx, 4U) == UCN_OK &&
                ucn_cluster_config_persist_begin_joint(
                    &owner, 2U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_joint_runtime_enter(&runtime, &owner, &tx) ==
                    UCN_OK);
    ucn_cluster_config_backup_gate_init(&backup_gate, false);
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &backup_gate, &durable) == UCN_OK && durable &&
                ucn_cluster_config_joint_runtime_commit(
                    &runtime, &owner, &backup_gate, &removing_member) == UCN_OK);
    (void)memset(&empty_member, 0, sizeof(empty_member));
    ASSERT_TRUE(memcmp(&removing_member, &empty_member,
                       sizeof(removing_member)) == 0 &&
                runtime.active_config.old_set.count == 2U &&
                !ucn_cluster_voter_set_contains(&runtime.active_config.old_set,
                                                9U));

    ASSERT_TRUE(make_removal_fixture(&stable, &tx, &removing_member, &fake,
                                     &provider));
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_init(&runtime, &stable) ==
                    UCN_OK &&
                ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                    UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_tx_record_ack(&tx, 4U) == UCN_OK &&
                ucn_cluster_config_persist_begin_joint(
                    &owner, 2U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_joint_runtime_enter(&runtime, &owner, &tx) ==
                    UCN_OK &&
                ucn_cluster_config_persist_begin_abort(
                    &owner, 3U, &tx, tx.deadline_ms, &durable) == UCN_OK &&
                durable && ucn_cluster_config_joint_runtime_abort(
                    &runtime, &owner, &removing_member) == UCN_OK &&
                removing_member.status ==
                    (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED &&
                removing_member.voting && runtime.active_config.old_set.count ==
                    3U && ucn_cluster_voter_set_contains(
                              &runtime.active_config.old_set, 9U));
    return 0;
}

static int test_restart_requires_durable_joint_proof_before_commit(void)
{
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_joint_runtime_t runtime;
    ucn_cluster_config_joint_runtime_t before;
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_persist_owner_t restart_owner;
    ucn_cluster_config_backup_gate_t backup_gate;
    uint32_t submit_before;
    bool durable;

    ASSERT_TRUE(make_fixture(&stable, &tx, &fake, &provider) &&
                ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) == UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_tx_record_ack(&tx, 1U) == UCN_OK &&
                ucn_cluster_config_tx_record_ack(&tx, 21U) == UCN_OK &&
                ucn_cluster_config_persist_owner_init(
                    &restart_owner, &provider, &config_store) == UCN_OK &&
                ucn_cluster_config_joint_runtime_init(&runtime, &stable) == UCN_OK);
    before = runtime;
    submit_before = fake.submit_count;
    ucn_cluster_config_backup_gate_init(&backup_gate, false);
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_enter(
                    &runtime, &restart_owner, &tx) == UCN_ERR_STATE &&
                memcmp(&runtime, &before, sizeof(runtime)) == 0 &&
                ucn_cluster_config_persist_begin_commit(
                    &restart_owner, 3U, &tx, &runtime, &backup_gate,
                    &durable) == UCN_ERR_STATE &&
                !durable && fake.submit_count == submit_before &&
                ucn_cluster_config_persist_begin_joint(
                    &restart_owner, 2U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_joint_runtime_enter(
                    &runtime, &restart_owner, &tx) == UCN_OK);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_joint_requires_quorum_and_durable_prepare();
    result |= test_timeout_abort_returns_to_old_config();
    result |= test_removal_commit_and_abort_have_terminal_member_rules();
    result |= test_restart_requires_durable_joint_proof_before_commit();

    if (result == 0) {
        printf("Cluster config joint tests passed.\n");
    }
    return result;
}
