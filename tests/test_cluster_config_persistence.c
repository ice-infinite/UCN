#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_config_joint.h"
#include "ucn/ucn_cluster_config_proposal.h"

#define ASSERT_TRUE(condition) do { if (!(condition)) { printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); return 1; } } while (0)

typedef struct fake_provider {
    ucn_cluster_persist_state_t stored;
    ucn_cluster_persist_request_t pending_request;
    uint32_t submit_count;
    uint8_t pending_polls;
    bool fail_submit;
    bool pending;
    ucn_cluster_config_persist_owner_t *reentry_owner;
    const ucn_cluster_config_tx_t *reentry_tx;
    const ucn_cluster_persist_provider_t *reentry_provider;
    ucn_cluster_config_store_t *reentry_store;
    bool reenter_load;
    bool reenter_submit;
    bool reenter_poll;
    uint32_t reentry_calls;
    ucn_result_t reentry_prepare;
    ucn_result_t reentry_joint;
    ucn_result_t reentry_commit;
    ucn_result_t reentry_abort;
    ucn_result_t reentry_poll_result;
    ucn_result_t reentry_init;
    bool reentry_init_owner_unchanged;
} fake_provider_t;

static ucn_cluster_config_store_t config_store;

static void fake_try_reentry(fake_provider_t *fake)
{
    bool durable = false;
    ucn_cluster_config_persist_action_t action;
    ucn_cluster_config_persist_owner_t before;

    if (fake == NULL || fake->reentry_owner == NULL ||
        fake->reentry_tx == NULL) {
        return;
    }
    fake->reentry_calls++;
    if (fake->reentry_provider != NULL && fake->reentry_store != NULL) {
        before = *fake->reentry_owner;
        fake->reentry_init = ucn_cluster_config_persist_owner_init(
            fake->reentry_owner, fake->reentry_provider, fake->reentry_store);
        fake->reentry_init_owner_unchanged =
            memcmp(fake->reentry_owner, &before, sizeof(before)) == 0;
    }
    fake->reentry_prepare = ucn_cluster_config_persist_begin_prepare(
        fake->reentry_owner, 91U, fake->reentry_tx, &durable);
    fake->reentry_joint = ucn_cluster_config_persist_begin_joint(
        fake->reentry_owner, 92U, fake->reentry_tx, &durable);
    fake->reentry_commit = ucn_cluster_config_persist_begin_commit(
        fake->reentry_owner, 93U, fake->reentry_tx, NULL, NULL, &durable);
    fake->reentry_abort = ucn_cluster_config_persist_begin_abort(
        fake->reentry_owner, 94U, fake->reentry_tx,
        fake->reentry_tx->deadline_ms, &durable);
    fake->reentry_poll_result = ucn_cluster_config_persist_poll(
        fake->reentry_owner, &durable, &action);
}

static ucn_result_t fake_load(void *context,
                              ucn_cluster_persist_load_result_t *result)
{
    fake_provider_t *fake = (fake_provider_t *)context;

    if (fake == NULL || result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (fake->reenter_load) {
        fake_try_reentry(fake);
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
    if (fake == NULL || request == NULL || fake->fail_submit) {
        completion.state = UCN_CLUSTER_PERSIST_FAILED;
        completion.failure = UCN_ERR_NETWORK;
        return completion;
    }
    if (fake->reenter_submit) {
        fake_try_reentry(fake);
    }
    fake->submit_count++;
    if (fake->pending_polls != 0U) {
        fake->pending_request = *request;
        fake->pending = true;
        completion.state = UCN_CLUSTER_PERSIST_PENDING;
        completion.token = 1U;
        return completion;
    }
    fake->stored = request->next_state;
    completion.state = UCN_CLUSTER_PERSIST_COMMITTED;
    return completion;
}

static ucn_cluster_persist_completion_t fake_poll(void *context,
                                                   ucn_cluster_persist_token_t token)
{
    fake_provider_t *fake = (fake_provider_t *)context;
    ucn_cluster_persist_completion_t completion;

    (void)memset(&completion, 0, sizeof(completion));
    if (fake == NULL || !fake->pending || token != 1U) {
        completion.state = UCN_CLUSTER_PERSIST_FAILED;
        completion.failure = UCN_ERR_STATE;
        return completion;
    }
    if (fake->reenter_poll) {
        fake_try_reentry(fake);
    }
    if (fake->pending_polls > 1U) {
        fake->pending_polls--;
        completion.state = UCN_CLUSTER_PERSIST_PENDING;
        completion.token = 1U;
        return completion;
    }
    fake->pending_polls = 0U;
    fake->pending = false;
    fake->stored = fake->pending_request.next_state;
    (void)memset(&fake->pending_request, 0, sizeof(fake->pending_request));
    completion.state = UCN_CLUSTER_PERSIST_COMMITTED;
    return completion;
}

static void make_provider(ucn_cluster_persist_provider_t *provider,
                          fake_provider_t *fake)
{
    (void)memset(provider, 0, sizeof(*provider));
    provider->struct_size = (uint16_t)sizeof(*provider);
    provider->api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider->load = fake_load;
    provider->submit = fake_submit;
    provider->poll = fake_poll;
    provider->context = fake;
}

static bool make_base_and_tx(ucn_cluster_config_state_t *stable,
                             ucn_cluster_config_tx_t *tx)
{
    static const ucn_node_id_t voters[] = { 1U, 4U, 9U };
    ucn_cluster_member_t provisional;

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
    return ucn_cluster_config_tx_begin_add_provisional(
               tx, 1U, 4U, &provisional, stable,
               UCN_CLUSTER_MAX_VOTERS, 1000U) == UCN_OK;
}

static bool make_conflicting_same_txid(
    const ucn_cluster_config_state_t *stable,
    uint32_t transaction_id,
    ucn_cluster_config_tx_t *tx)
{
    ucn_cluster_member_t provisional;

    if (stable == NULL || tx == NULL) {
        return false;
    }
    (void)memset(&provisional, 0, sizeof(provisional));
    provisional.occupied = true;
    provisional.provisional_deadline_armed = true;
    provisional.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    provisional.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    provisional.node_id = 22U;
    provisional.provisional_deadline_ms = 100U;
    ucn_cluster_config_tx_init_empty(tx);
    return ucn_cluster_config_tx_begin_add_provisional(
               tx, transaction_id, 4U, &provisional, stable,
               UCN_CLUSTER_MAX_VOTERS, 1000U) == UCN_OK;
}

static bool seed_durable_base(fake_provider_t *fake,
                              const ucn_cluster_config_state_t *stable)
{
    ucn_cluster_persist_config_ref_t ref;

    if (ucn_cluster_config_persist_ref_from_state(stable, &ref) != UCN_OK) {
        return false;
    }
    (void)memset(fake, 0, sizeof(*fake));
    ucn_cluster_config_store_init_empty(&config_store);
    if (ucn_cluster_config_store_write_stable(&config_store, stable, &ref) !=
        UCN_OK) {
        return false;
    }
    ucn_cluster_persist_state_init_empty(&fake->stored);
    fake->stored.boot_incarnation = 1U;
    fake->stored.committed_config = ref;
    return ucn_cluster_persist_state_is_valid(&fake->stored);
}

static int test_prepare_then_joint_quorum_then_commit(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_joint_runtime_t runtime;
    ucn_cluster_config_backup_gate_t backup_gate;
    ucn_cluster_persist_config_ref_t joint_ref;
    bool durable;
    uint32_t submit_before;

    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                seed_durable_base(&fake, &stable));
    make_provider(&provider, &fake);
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_config_joint_runtime_init(&runtime, &stable) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                owner.durable_state.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED);
    submit_before = fake.submit_count;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, NULL, NULL, &durable) == UCN_ERR_STATE &&
                !durable && fake.submit_count == submit_before);
    ASSERT_TRUE(ucn_cluster_config_tx_record_ack(&tx, 1U) == UCN_OK &&
                ucn_cluster_config_tx_record_ack(&tx, 21U) == UCN_OK &&
                ucn_cluster_config_joint_quorum_reached(&tx));
    /* Dual quorum alone cannot bypass the durable Joint barrier. */
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, NULL, NULL, &durable) == UCN_ERR_STATE &&
                !durable && fake.submit_count == submit_before &&
                ucn_cluster_config_persist_begin_joint(
                    &owner, 2U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_joint_runtime_enter(&runtime, &owner, &tx) ==
                    UCN_OK);
    ucn_cluster_config_backup_gate_init(&backup_gate, false);
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &backup_gate, &durable) == UCN_OK && durable &&
                owner.durable_state.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
                ucn_cluster_config_persist_ref_from_joint_new(&tx.proposed_config,
                                                              &joint_ref) == UCN_OK &&
                memcmp(&owner.durable_state.committed_config, &joint_ref,
                       sizeof(joint_ref)) == 0);
    return 0;
}

static int test_submit_failure_has_no_ack_or_commit_permission(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_persist_state_t before;
    bool durable;

    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                seed_durable_base(&fake, &stable));
    make_provider(&provider, &fake);
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                UCN_OK);
    before = owner.durable_state;
    fake.fail_submit = true;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_ERR_NETWORK &&
                !durable && !ucn_cluster_config_persist_owner_is_pending(&owner) &&
                memcmp(&owner.durable_state, &before, sizeof(before)) == 0);
    return 0;
}

static int test_pending_blocks_permission_until_reload_proof(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_persist_action_t action;
    bool durable;

    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                seed_durable_base(&fake, &stable));
    fake.pending_polls = 2U;
    make_provider(&provider, &fake);
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && !durable &&
                ucn_cluster_config_persist_owner_is_pending(&owner));
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 2U, &tx, NULL, NULL, &durable) == UCN_ERR_STATE &&
                !durable);
    ASSERT_TRUE(ucn_cluster_config_persist_poll(&owner, &durable, &action) ==
                    UCN_OK &&
                !durable && action ==
                    UCN_CLUSTER_CONFIG_PERSIST_ACTION_NONE);
    ASSERT_TRUE(ucn_cluster_config_persist_poll(&owner, &durable, &action) ==
                    UCN_OK &&
                durable && action == UCN_CLUSTER_CONFIG_PERSIST_ACTION_PREPARE &&
                !ucn_cluster_config_persist_owner_is_pending(&owner));
    return 0;
}

static int test_timeout_abort_is_durable_and_retains_old_config(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_persist_owner_t restart_owner;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_persist_config_ref_t base_ref;
    bool durable;
    uint32_t submit_before;

    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                seed_durable_base(&fake, &stable));
    ASSERT_TRUE(ucn_cluster_config_persist_ref_from_state(&stable, &base_ref) ==
                UCN_OK);
    make_provider(&provider, &fake);
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable);
    ASSERT_TRUE(ucn_cluster_config_persist_begin_abort(
                    &owner, 2U, &tx, tx.deadline_ms, &durable) == UCN_OK &&
                durable &&
                owner.durable_state.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
                owner.durable_state.config_transaction.transaction_id ==
                    tx.transaction_id &&
                memcmp(&owner.durable_state.committed_config, &base_ref,
                       sizeof(base_ref)) == 0 &&
                owner.durable_state.last_completed_operation ==
                    UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT);
    submit_before = fake.submit_count;
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(
                    &restart_owner, &provider, &config_store) == UCN_OK &&
                ucn_cluster_config_persist_begin_abort(
                    &restart_owner, 2U, &tx, tx.deadline_ms, &durable) == UCN_OK &&
                durable && fake.submit_count == submit_before);
    return 0;
}

static int test_pending_timeout_abort_has_no_early_restore_permission(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_persist_action_t action;
    bool durable;

    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                seed_durable_base(&fake, &stable));
    make_provider(&provider, &fake);
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) ==
                    UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable);
    fake.pending_polls = 2U;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_abort(
                    &owner, 2U, &tx, tx.deadline_ms, &durable) == UCN_OK &&
                !durable && ucn_cluster_config_persist_owner_is_pending(&owner) &&
                ucn_cluster_config_persist_poll(&owner, &durable, &action) ==
                    UCN_OK && !durable &&
                action == UCN_CLUSTER_CONFIG_PERSIST_ACTION_NONE &&
                ucn_cluster_config_persist_poll(&owner, &durable, &action) ==
                    UCN_OK && durable &&
                action == UCN_CLUSTER_CONFIG_PERSIST_ACTION_ABORT &&
                owner.durable_state.last_completed_operation ==
                    UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT);
    return 0;
}

static int test_restart_uses_body_refs_and_fails_closed_on_torn_staging(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_persist_owner_t restart_owner;
    ucn_cluster_config_persist_owner_t before;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    bool durable;

    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                seed_durable_base(&fake, &stable));
    make_provider(&provider, &fake);
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) == UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_persist_owner_init(
                    &restart_owner, &provider, &config_store) == UCN_OK &&
                restart_owner.durable_state.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED);
    (void)memset(&restart_owner, 0xA5, sizeof(restart_owner));
    before = restart_owner;
    /* Base occupies slot zero in this deterministic fixture; corrupting slot
     * one models a torn C_new body while M04 still names it as PREPARED. */
    config_store.slots[1U][UCN_CLUSTER_CONFIG_STORE_RECORD_HEADER_BYTES] ^= 1U;
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(
                    &restart_owner, &provider, &config_store) == UCN_ERR_STATE &&
                memcmp(&restart_owner, &before, sizeof(restart_owner)) == 0);
    return 0;
}

static int test_provider_reentry_is_closed_before_io_returns(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_persist_action_t action;
    bool durable;

    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                seed_durable_base(&fake, &stable));
    make_provider(&provider, &fake);
    fake.reentry_owner = &owner;
    fake.reentry_tx = &tx;
    fake.reentry_provider = &provider;
    fake.reentry_store = &config_store;
    fake.reenter_load = true;
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) == UCN_OK &&
                fake.reentry_calls == 1U &&
                fake.reentry_prepare == UCN_ERR_STATE &&
                fake.reentry_init == UCN_ERR_STATE &&
                fake.reentry_init_owner_unchanged &&
                fake.reentry_joint == UCN_ERR_STATE &&
                fake.reentry_commit == UCN_ERR_STATE &&
                fake.reentry_abort == UCN_ERR_STATE &&
                fake.reentry_poll_result == UCN_ERR_STATE &&
                fake.submit_count == 0U);

    fake.reenter_load = false;
    fake.reenter_submit = true;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                fake.submit_count == 1U &&
                fake.reentry_prepare == UCN_ERR_STATE &&
                fake.reentry_init == UCN_ERR_STATE &&
                fake.reentry_init_owner_unchanged &&
                fake.reentry_joint == UCN_ERR_STATE &&
                fake.reentry_commit == UCN_ERR_STATE &&
                fake.reentry_abort == UCN_ERR_STATE &&
                fake.reentry_poll_result == UCN_ERR_STATE);

    fake.reenter_submit = false;
    fake.reenter_poll = true;
    fake.pending_polls = 1U;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_abort(
                    &owner, 2U, &tx, tx.deadline_ms, &durable) == UCN_OK &&
                !durable && fake.submit_count == 2U &&
                ucn_cluster_config_persist_owner_is_pending(&owner) &&
                ucn_cluster_config_persist_poll(&owner, &durable, &action) ==
                    UCN_OK && durable &&
                action == UCN_CLUSTER_CONFIG_PERSIST_ACTION_ABORT &&
                fake.reentry_prepare == UCN_ERR_STATE &&
                fake.reentry_init == UCN_ERR_STATE &&
                fake.reentry_init_owner_unchanged &&
                fake.reentry_joint == UCN_ERR_STATE &&
                fake.reentry_commit == UCN_ERR_STATE &&
                fake.reentry_abort == UCN_ERR_STATE &&
                fake.reentry_poll_result == UCN_ERR_STATE &&
                !ucn_cluster_config_persist_owner_is_pending(&owner));
    return 0;
}

static int test_abort_binds_txid_and_staged_c_new(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_tx_t conflicting_tx;
    ucn_cluster_persist_state_t before;
    bool durable;
    uint32_t submit_before;

    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                make_conflicting_same_txid(&stable, tx.transaction_id,
                                           &conflicting_tx) &&
                seed_durable_base(&fake, &stable));
    make_provider(&provider, &fake);
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) == UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable);
    before = owner.durable_state;
    submit_before = fake.submit_count;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_abort(
                    &owner, 2U, &conflicting_tx,
                    conflicting_tx.deadline_ms, &durable) == UCN_ERR_STATE &&
                !durable && fake.submit_count == submit_before &&
                memcmp(&owner.durable_state, &before, sizeof(before)) == 0 &&
                ucn_cluster_config_persist_begin_abort(
                    &owner, 2U, &tx, tx.deadline_ms, &durable) == UCN_OK &&
                durable && owner.durable_state.last_completed_operation ==
                    UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT);
    return 0;
}

static int test_backup_gate_rejects_before_durable_commit(void)
{
    fake_provider_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_persist_owner_t owner;
    ucn_cluster_config_joint_runtime_t runtime;
    ucn_cluster_config_joint_runtime_t conflicting_runtime;
    ucn_cluster_config_backup_gate_t gate;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_tx_t tx;
    ucn_cluster_config_tx_t conflicting_tx;
    ucn_cluster_member_t backup;
    ucn_cluster_config_backup_gate_t stale_gate;
    ucn_cluster_persist_state_t prepared;
    uint32_t submit_before;
    bool durable;

    ASSERT_TRUE(make_base_and_tx(&stable, &tx) &&
                make_conflicting_same_txid(&stable, tx.transaction_id,
                                           &conflicting_tx) &&
                seed_durable_base(&fake, &stable));
    make_provider(&provider, &fake);
    ASSERT_TRUE(ucn_cluster_config_persist_owner_init(&owner, &provider,
                                                       &config_store) == UCN_OK &&
                ucn_cluster_config_joint_runtime_init(&runtime, &stable) == UCN_OK &&
                ucn_cluster_config_persist_begin_prepare(
                    &owner, 1U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_tx_record_ack(&tx, 1U) == UCN_OK &&
                ucn_cluster_config_tx_record_ack(&tx, 21U) == UCN_OK &&
                ucn_cluster_config_persist_begin_joint(
                    &owner, 2U, &tx, &durable) == UCN_OK && durable &&
                ucn_cluster_config_joint_runtime_enter(&runtime, &owner, &tx) ==
                    UCN_OK);
    prepared = owner.durable_state;
    submit_before = fake.submit_count;
    ucn_cluster_config_backup_gate_init(&gate, true);
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &gate, &durable) == UCN_ERR_STATE &&
                !durable && fake.submit_count == submit_before &&
                memcmp(&owner.durable_state, &prepared, sizeof(prepared)) == 0);

    (void)memset(&backup, 0, sizeof(backup));
    backup.occupied = true;
    backup.voting = true;
    backup.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    backup.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    backup.node_id = 9U;
    ASSERT_TRUE(ucn_cluster_config_backup_gate_set_backup(&gate, &backup) == UCN_OK &&
                ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &gate, &durable) == UCN_ERR_STATE &&
                !durable && fake.submit_count == submit_before &&
                memcmp(&owner.durable_state, &prepared, sizeof(prepared)) == 0 &&
                ucn_cluster_config_backup_gate_stage(&gate, &tx) == UCN_OK &&
                ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &gate, &durable) == UCN_ERR_STATE &&
                !durable && fake.submit_count == submit_before &&
                memcmp(&owner.durable_state, &prepared, sizeof(prepared)) == 0 &&
                ucn_cluster_config_backup_gate_ack(
                    &gate, 9U, tx.transaction_id, &gate.staged_config) == UCN_OK &&
                ucn_cluster_config_backup_gate_commit_allowed_for_tx(
                    &gate, &tx, NULL) &&
                !ucn_cluster_config_backup_gate_commit_allowed_for_tx(
                    &gate, &conflicting_tx, NULL));
    conflicting_runtime = runtime;
    conflicting_runtime.transaction = conflicting_tx;
    conflicting_runtime.active_config = conflicting_tx.proposed_config;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &conflicting_tx, &conflicting_runtime, &gate,
                    &durable) == UCN_ERR_STATE && !durable &&
                fake.submit_count == submit_before &&
                memcmp(&owner.durable_state, &prepared, sizeof(prepared)) == 0);
    stale_gate = gate;
    stale_gate.transaction_id++;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &stale_gate, &durable) ==
                    UCN_ERR_STATE && !durable &&
                fake.submit_count == submit_before &&
                memcmp(&owner.durable_state, &prepared, sizeof(prepared)) == 0);
    stale_gate = gate;
    stale_gate.acknowledged_backup_node_id = 4U;
    ASSERT_TRUE(ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &stale_gate, &durable) ==
                    UCN_ERR_STATE && !durable &&
                fake.submit_count == submit_before &&
                memcmp(&owner.durable_state, &prepared, sizeof(prepared)) == 0 &&
                ucn_cluster_config_persist_begin_commit(
                    &owner, 3U, &tx, &runtime, &gate, &durable) == UCN_OK &&
                durable && fake.submit_count == submit_before + 1U);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_prepare_then_joint_quorum_then_commit();
    result |= test_submit_failure_has_no_ack_or_commit_permission();
    result |= test_pending_blocks_permission_until_reload_proof();
    result |= test_timeout_abort_is_durable_and_retains_old_config();
    result |= test_pending_timeout_abort_has_no_early_restore_permission();
    result |= test_restart_uses_body_refs_and_fails_closed_on_torn_staging();
    result |= test_provider_reentry_is_closed_before_io_returns();
    result |= test_abort_binds_txid_and_staged_c_new();
    result |= test_backup_gate_rejects_before_durable_commit();
    if (result == 0) {
        printf("Cluster config persistence tests passed.\n");
    }
    return result;
}
