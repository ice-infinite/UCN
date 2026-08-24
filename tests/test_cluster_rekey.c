#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_rekey.h"

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, \
                   __LINE__); \
            return 1; \
        } \
    } while (0)

typedef struct fixture {
    uint32_t now_ms;
    uint32_t provider_value;
    uint32_t provider_calls;
    ucn_cluster_id_request_t provider_request;
    ucn_cluster_t cluster;
    ucn_cluster_authority_runtime_t authority;
    ucn_cluster_authority_timing_t timing;
    ucn_cluster_config_state_t config;
    ucn_cluster_persist_state_t durable;
    ucn_cluster_id_history_t id_history;
    uint32_t id_history_generation;
    ucn_cluster_rekey_voter_profile_t profiles[3U];
    size_t profile_count;
} fixture_t;

typedef struct persist_fake {
    ucn_cluster_persist_load_result_t stored;
    ucn_cluster_persist_request_t pending_request;
    ucn_cluster_rekey_persist_owner_t *owner;
    ucn_cluster_rekey_transaction_t *transaction;
    uint32_t load_calls;
    uint32_t submit_calls;
    uint32_t poll_calls;
    bool submit_pending;
    bool poll_pending_once;
    bool fail_submit;
    bool reenter_load;
    bool reenter_submit;
    bool reenter_poll;
    const ucn_cluster_persist_provider_t *provider;
    ucn_cluster_authority_runtime_t *authority;
    ucn_result_t reentry_init_result;
    ucn_result_t reentry_prepare_result;
    ucn_result_t reentry_commit_result;
    ucn_result_t reentry_abort_result;
    ucn_result_t reentry_poll_result;
} persist_fake_t;

static ucn_cluster_persist_completion_t persist_completion(
    ucn_cluster_persist_completion_state_t state,
    ucn_cluster_persist_token_t token,
    ucn_result_t failure)
{
    ucn_cluster_persist_completion_t result;

    (void)memset(&result, 0, sizeof(result));
    result.state = state;
    result.token = token;
    result.failure = failure;
    return result;
}

static ucn_result_t persist_fake_load(
    void *context,
    ucn_cluster_persist_load_result_t *result)
{
    persist_fake_t *fake = (persist_fake_t *)context;

    if (fake == NULL || result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    fake->load_calls++;
    if (fake->reenter_load) {
        fake->reentry_init_result = ucn_cluster_rekey_persist_owner_init(
            fake->owner, fake->provider, fake->authority);
    }
    *result = fake->stored;
    return UCN_OK;
}

static ucn_cluster_persist_completion_t persist_fake_submit(
    void *context,
    const ucn_cluster_persist_request_t *request)
{
    persist_fake_t *fake = (persist_fake_t *)context;
    bool durable = false;

    if (fake == NULL || request == NULL) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_ARGUMENT);
    }
    fake->submit_calls++;
    fake->pending_request = *request;
    if (fake->reenter_submit) {
        ucn_cluster_rekey_persist_action_t action;

        fake->reentry_prepare_result = ucn_cluster_rekey_persist_begin_prepare(
            fake->owner, request->operation_id, fake->transaction, 1U,
            &durable);
        fake->reentry_commit_result = ucn_cluster_rekey_persist_begin_commit(
            fake->owner, request->operation_id, fake->transaction, 1U,
            &durable);
        fake->reentry_abort_result = ucn_cluster_rekey_persist_begin_abort(
            fake->owner, request->operation_id, fake->transaction, 1U,
            &durable);
        fake->reentry_poll_result = ucn_cluster_rekey_persist_poll(
            fake->owner, fake->transaction, 1U, &durable, &action);
        fake->reentry_init_result = ucn_cluster_rekey_persist_owner_init(
            fake->owner, fake->provider, fake->authority);
    }
    if (fake->fail_submit) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_NETWORK);
    }
    if (fake->submit_pending) {
        return persist_completion(UCN_CLUSTER_PERSIST_PENDING, 1U, UCN_OK);
    }
    fake->stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    fake->stored.snapshot = request->next_state;
    return persist_completion(UCN_CLUSTER_PERSIST_COMMITTED,
                              UCN_CLUSTER_PERSIST_TOKEN_NONE, UCN_OK);
}

static ucn_cluster_persist_completion_t persist_fake_poll(
    void *context,
    ucn_cluster_persist_token_t token)
{
    persist_fake_t *fake = (persist_fake_t *)context;

    if (fake == NULL || token != 1U) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_ARGUMENT);
    }
    fake->poll_calls++;
    if (fake->reenter_poll) {
        bool durable = false;
        ucn_cluster_rekey_persist_action_t action;

        fake->reentry_prepare_result = ucn_cluster_rekey_persist_begin_prepare(
            fake->owner, fake->pending_request.operation_id,
            fake->transaction, 1U, &durable);
        fake->reentry_commit_result = ucn_cluster_rekey_persist_begin_commit(
            fake->owner, fake->pending_request.operation_id,
            fake->transaction, 1U, &durable);
        fake->reentry_abort_result = ucn_cluster_rekey_persist_begin_abort(
            fake->owner, fake->pending_request.operation_id,
            fake->transaction, 1U, &durable);
        fake->reentry_poll_result = ucn_cluster_rekey_persist_poll(
            fake->owner, fake->transaction, 1U, &durable, &action);
        fake->reentry_init_result = ucn_cluster_rekey_persist_owner_init(
            fake->owner, fake->provider, fake->authority);
    }
    if (fake->poll_pending_once) {
        fake->poll_pending_once = false;
        return persist_completion(UCN_CLUSTER_PERSIST_PENDING, 1U, UCN_OK);
    }
    fake->stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    fake->stored.snapshot = fake->pending_request.next_state;
    return persist_completion(UCN_CLUSTER_PERSIST_COMMITTED,
                              UCN_CLUSTER_PERSIST_TOKEN_NONE, UCN_OK);
}

static void persist_provider_init(ucn_cluster_persist_provider_t *provider,
                                  persist_fake_t *fake,
                                  const ucn_cluster_persist_state_t *state)
{
    (void)memset(fake, 0, sizeof(*fake));
    (void)memset(provider, 0, sizeof(*provider));
    fake->stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    fake->stored.snapshot = *state;
    provider->struct_size = (uint16_t)sizeof(*provider);
    provider->api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider->context = fake;
    provider->load = persist_fake_load;
    provider->submit = persist_fake_submit;
    provider->poll = persist_fake_poll;
    fake->provider = provider;
}

static ucn_result_t fixture_make_id(
    void *context,
    const ucn_cluster_id_request_t *request,
    uint32_t *cluster_id)
{
    fixture_t *fixture = (fixture_t *)context;

    if (fixture == NULL || request == NULL || cluster_id == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    fixture->provider_calls++;
    fixture->provider_request = *request;
    *cluster_id = fixture->provider_value;
    return UCN_OK;
}

static uint32_t fixture_now(void *context)
{
    fixture_t *fixture = (fixture_t *)context;
    return fixture == NULL ? 0U : fixture->now_ms;
}

static ucn_result_t fixture_send(void *context, ucn_node_id_t destination,
                                 ucn_endpoint_t endpoint,
                                 const uint8_t *payload,
                                 uint16_t payload_length)
{
    (void)context;
    (void)destination;
    (void)endpoint;
    (void)payload;
    (void)payload_length;
    return UCN_OK;
}

static int fixture_init(fixture_t *fixture)
{
    static const ucn_node_id_t voters[] = { 1U, 2U, 3U };
    const ucn_cluster_timing_budget_t budget = { 5U, 5U, 5U, 5U, 5U, 5U };
    ucn_cluster_config_t cluster_config;

    (void)memset(fixture, 0, sizeof(*fixture));
    (void)memset(&cluster_config, 0, sizeof(cluster_config));
    cluster_config.local_node_id = 1U;
    cluster_config.enabled = false;
    cluster_config.head_capable = true;
    cluster_config.member_capacity = 4U;
    cluster_config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    cluster_config.now_ms = fixture_now;
    cluster_config.now_context = fixture;
    cluster_config.send = fixture_send;
    cluster_config.send_context = fixture;
    cluster_config.make_cluster_id = fixture_make_id;
    cluster_config.cluster_id_context = fixture;
    ASSERT_TRUE(ucn_cluster_init(&fixture->cluster, &cluster_config) == UCN_OK);
    fixture->cluster.config.enabled = true;
    fixture->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    fixture->cluster.cluster_id = 77U;
    fixture->cluster.term = 5U;
    fixture->cluster.head_node_id = 1U;
    fixture->cluster.backup_node_id = 2U;
    fixture->cluster.backup_ready = true;
    fixture->provider_value = 88U;
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
        &fixture->config, 10U, voters, sizeof(voters) / sizeof(voters[0U])));
    ASSERT_TRUE(ucn_cluster_authority_timing_derive(&budget, &fixture->timing) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_authority_runtime_init(
        &fixture->authority, &fixture->cluster, &fixture->config,
        &fixture->timing, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
        &fixture->authority, 2U, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture->authority, 0U) ==
                UCN_OK);
    ASSERT_TRUE(fixture->cluster.authority_active &&
                fixture->cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_HEAD_STABLE);

    (void)memset(&fixture->durable, 0, sizeof(fixture->durable));
    fixture->durable.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    fixture->durable.has_active_epoch = true;
    fixture->durable.active_epoch.cluster_id = 77U;
    fixture->durable.active_epoch.term = 5U;
    fixture->durable.active_epoch.head_node_id = 1U;
    fixture->durable.has_max_epoch = true;
    fixture->durable.max_epoch = fixture->durable.active_epoch;
    fixture->durable.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    fixture->durable.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    fixture->durable.boot_incarnation = 1U;
    ASSERT_TRUE(ucn_cluster_config_persist_ref_from_state(
        &fixture->config, &fixture->durable.committed_config) == UCN_OK);
    fixture->profile_count = 3U;
    ucn_cluster_id_history_init(&fixture->id_history);
    fixture->profiles[0U].node_id = 1U;
    fixture->profiles[1U].node_id = 2U;
    fixture->profiles[2U].node_id = 3U;
    for (size_t index = 0U;
         index < sizeof(fixture->profiles) / sizeof(fixture->profiles[0U]);
         ++index) {
        fixture->profiles[index].wire_format =
            UCN_CLUSTER_WIRE_V4_FORMAT_VERSION;
        fixture->profiles[index].capabilities =
            UCN_CLUSTER_REKEY_REQUIRED_CAPABILITIES;
        fixture->profiles[index].persistence_generation = 9U;
    }
    return 0;
}

static ucn_result_t fixture_begin(fixture_t *fixture,
                                  ucn_cluster_rekey_transaction_t *transaction,
                                  uint32_t transaction_id,
                                  uint32_t nonce,
                                  uint32_t now_ms)
{
    ucn_cluster_id_history_t reloaded;
    uint8_t record[UCN_CLUSTER_ID_HISTORY_RECORD_BYTES];
    uint32_t generation = 0U;
    ucn_result_t result;

    result = ucn_cluster_rekey_transaction_begin(
        transaction, &fixture->authority, &fixture->durable,
        fixture->profiles, fixture->profile_count, transaction_id, nonce,
        now_ms, &fixture->id_history, fixture->id_history_generation);
    if (result != UCN_OK) {
        return result;
    }
    if (ucn_cluster_id_history_record_encode(
            &fixture->id_history,
            transaction->allocation_history_generation,
            record, sizeof(record)) != UCN_OK ||
        ucn_cluster_id_history_record_decode(
            record, sizeof(record), &generation, &reloaded) != UCN_OK ||
        ucn_cluster_rekey_transaction_confirm_id_history_durable(
            transaction, &reloaded, generation) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    fixture->id_history = reloaded;
    fixture->id_history_generation = generation;
    return UCN_OK;
}

static int test_valid_begin_and_single_owner(void)
{
    fixture_t fixture;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t before;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_rekey_transaction_is_valid(&transaction) &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED &&
                transaction.phase == UCN_CLUSTER_PHASE_HEAD_REKEYING &&
                transaction.predecessor_epoch.cluster_id == 77U &&
                transaction.predecessor_config.config_id == 10U &&
                transaction.successor_epoch.cluster_id == 88U &&
                transaction.successor_epoch.term == 1U &&
                transaction.successor_config.config_id == 1U &&
                transaction.durable_rekey_ref.next_incarnation == 2U &&
                transaction.allocation_history_generation == 1U &&
                transaction.allocation_history_fingerprint != 0U &&
                transaction.durable_rekey_ref.allocation_history_fingerprint ==
                    transaction.allocation_history_fingerprint &&
                fixture.cluster.cluster_id_round == 1U &&
                fixture.provider_calls == 1U &&
                fixture.provider_request.purpose ==
                    UCN_CLUSTER_ID_PURPOSE_REKEY &&
                fixture.provider_request.parent_cluster_id == 77U &&
                fixture.provider_request.parent_term == 5U &&
                fixture.provider_request.parent_config_id == 10U &&
                fixture.provider_request.round == 1U);
    before = transaction;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 2U, 8U, 1U) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    return 0;
}

static int test_id_history_durable_gate(void)
{
    fixture_t fixture;
    persist_fake_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_rekey_persist_owner_t owner;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t before;
    ucn_cluster_id_history_t reloaded;
    ucn_cluster_id_history_t wrong;
    uint8_t record[UCN_CLUSTER_ID_HISTORY_RECORD_BYTES];
    uint32_t generation = 0U;
    bool durable = true;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(ucn_cluster_rekey_transaction_begin(
                    &transaction, &fixture.authority, &fixture.durable,
                    fixture.profiles, fixture.profile_count, 1U, 7U, 1U,
                    &fixture.id_history, fixture.id_history_generation) ==
                    UCN_OK &&
                transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_ID_HISTORY_DURABLE_REQUIRED &&
                transaction.allocation_history_generation == 1U);
    persist_provider_init(&provider, &fake, &fixture.durable);
    ASSERT_TRUE(ucn_cluster_rekey_persist_owner_init(
                    &owner, &provider, &fixture.authority) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, 1U, &durable) ==
                    UCN_ERR_STATE &&
                !durable && fake.submit_calls == 0U);

    wrong = fixture.id_history;
    wrong.entries[0U].cluster_id = 89U;
    before = transaction;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_confirm_id_history_durable(
                    &transaction, &wrong, 1U) == UCN_ERR_STATE &&
                memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ASSERT_TRUE(ucn_cluster_rekey_transaction_confirm_id_history_durable(
                    &transaction, &fixture.id_history, 2U) == UCN_ERR_STATE &&
                memcmp(&transaction, &before, sizeof(transaction)) == 0);

    ASSERT_TRUE(ucn_cluster_id_history_record_encode(
                    &fixture.id_history, 1U, record, sizeof(record)) == UCN_OK &&
                ucn_cluster_id_history_record_decode(
                    record, sizeof(record), &generation, &reloaded) == UCN_OK &&
                ucn_cluster_rekey_transaction_confirm_id_history_durable(
                    &transaction, &reloaded, generation) == UCN_OK &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED);
    durable = false;
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, 1U, &durable) == UCN_OK &&
                durable && fake.submit_calls == 1U);
    return 0;
}

static int test_provider_validation_and_round_atomicity(void)
{
    fixture_t fixture;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t zero;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&transaction, 0, sizeof(transaction));
    zero = transaction;
    fixture.provider_value = 0U;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_CONFIG);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0 &&
                fixture.cluster.cluster_id_round == 0U);
    fixture.provider_value = 77U;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_CONFIG);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0 &&
                fixture.cluster.cluster_id_round == 0U);
    fixture.provider_value = 88U;
    fixture.cluster.cluster_id_round =
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_EXHAUSTED);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0 &&
                fixture.cluster.cluster_id_round ==
                    UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD);
    fixture.cluster.cluster_id_round = 0U;
    fixture.cluster.config.make_cluster_id = NULL;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_CONFIG);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0 &&
                fixture.cluster.cluster_id_round == 0U);
    return 0;
}

static void make_ack_frame(const ucn_cluster_rekey_transaction_t *transaction,
                           ucn_cluster_wire_v4_frame_t *frame)
{
    (void)memset(frame, 0, sizeof(*frame));
    frame->type = UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK;
    frame->role = UCN_CLUSTER_ROLE_MEMBER;
    frame->cluster_id = transaction->predecessor_epoch.cluster_id;
    frame->term = transaction->predecessor_epoch.term;
    frame->head_node_id = transaction->predecessor_epoch.head_node_id;
    frame->words[0U] = transaction->successor_epoch.cluster_id;
    frame->words[1U] = transaction->successor_epoch.term;
    frame->words[2U] = transaction->transaction_id;
    frame->words[3U] = transaction->successor_config.config_id;
    frame->words[4U] = 9U;
    frame->words[5U] = 12U;
}

static int test_wire_prepare_ack_commit_binding(void)
{
    fixture_t fixture;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t bypass;
    ucn_cluster_rekey_transaction_t transaction_before;
    ucn_cluster_wire_v4_frame_t prepare;
    ucn_cluster_wire_v4_frame_t ack_frame;
    ucn_cluster_wire_v4_frame_t commit;
    ucn_cluster_wire_v4_frame_t frame_before;
    ucn_cluster_rekey_ack_t ack;
    ucn_cluster_rekey_ack_t ack_before;
    ucn_cluster_persist_state_t prepared;
    ucn_cluster_persist_state_t committed;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_OK);
    bypass = transaction;
    bypass.state = UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS;
    make_ack_frame(&bypass, &ack_frame);
    transaction_before = bypass;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
                    &bypass, &fixture.durable, &ack_frame, 2U, 1U) ==
                    UCN_ERR_STATE &&
                memcmp(&bypass, &transaction_before, sizeof(bypass)) == 0);
    prepared = fixture.durable;
    prepared.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    prepared.rekey_transaction.transaction_id = transaction.transaction_id;
    prepared.rekey_transaction.staging_rekey = transaction.durable_rekey_ref;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_begin_collection(
                    &transaction, &prepared) == UCN_OK);
    (void)memset(&prepare, 0xA5, sizeof(prepare));
    ASSERT_TRUE(ucn_cluster_rekey_prepare_frame_build(
                    &transaction, &prepared, 1U, &prepare) == UCN_OK);
    ASSERT_TRUE(prepare.type == UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE &&
                prepare.role == UCN_CLUSTER_ROLE_HEAD &&
                prepare.cluster_id == 77U && prepare.term == 5U &&
                prepare.head_node_id == 1U && prepare.words[0U] == 88U &&
                prepare.words[1U] == 1U && prepare.words[2U] == 1U &&
                prepare.words[3U] == 10U && prepare.words[4U] == 1U &&
                prepare.words[5U] == 7U &&
                ucn_cluster_wire_v4_frame_is_valid(&prepare));

    make_ack_frame(&transaction, &ack_frame);
    (void)memset(&ack, 0x5A, sizeof(ack));
    ASSERT_TRUE(ucn_cluster_rekey_ack_frame_admit(
        &transaction, &ack_frame, 2U, &ack) == UCN_OK);
    ASSERT_TRUE(ack.source_node_id == 2U && ack.member_nonce == 12U &&
                ack.persistence_generation == 9U);
    ack_before = ack;
    ack_frame.words[2U]++;
    ASSERT_TRUE(ucn_cluster_rekey_ack_frame_admit(
        &transaction, &ack_frame, 2U, &ack) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&ack, &ack_before, sizeof(ack)) == 0);
    ack_frame.words[2U]--;
    ASSERT_TRUE(ucn_cluster_rekey_ack_frame_admit(
        &transaction, &ack_frame, 4U, &ack) == UCN_ERR_ACCESS);
    ASSERT_TRUE(memcmp(&ack, &ack_before, sizeof(ack)) == 0);

    (void)memset(&commit, 0xA5, sizeof(commit));
    frame_before = commit;
    ASSERT_TRUE(ucn_cluster_rekey_commit_frame_build(
        &transaction, &fixture.durable, &commit) == UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&commit, &frame_before, sizeof(commit)) == 0);
    committed = fixture.durable;
    committed.active_epoch = transaction.successor_epoch;
    committed.max_epoch = transaction.successor_epoch;
    committed.committed_config = transaction.successor_config_ref;
    committed.committed_rekey = transaction.durable_rekey_ref;
    committed.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    committed.rekey_transaction.transaction_id = transaction.transaction_id;
    committed.tombstone.valid = true;
    committed.tombstone.retired_epoch = transaction.predecessor_epoch;
    committed.tombstone.replacement_cluster_id =
        transaction.successor_epoch.cluster_id;
    committed.tombstone.rekey_transaction_id = transaction.transaction_id;
    committed.boot_incarnation =
        transaction.durable_rekey_ref.next_incarnation;
    frame_before = commit;
    ASSERT_TRUE(ucn_cluster_rekey_commit_frame_build(
        &transaction, &committed, &commit) == UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&commit, &frame_before, sizeof(commit)) == 0);
    transaction.state = UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE;
    transaction.authority_revoked = true;
    ASSERT_TRUE(ucn_cluster_rekey_commit_frame_build(
        &transaction, &committed, &commit) == UCN_OK);
    ASSERT_TRUE(commit.type == UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT &&
                commit.words[2U] == transaction.transaction_id &&
                ucn_cluster_wire_v4_frame_is_valid(&commit));
    return 0;
}

static int test_profile_gate_and_old_config_quorum(void)
{
    fixture_t fixture;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t before;
    ucn_cluster_persist_state_t prepared;
    ucn_cluster_wire_v4_frame_t ack_frame;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&transaction, 0, sizeof(transaction));
    fixture.profiles[1U].wire_format = UCN_CLUSTER_WIRE_V3_FORMAT_VERSION;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_ACCESS);
    ASSERT_TRUE(fixture.provider_calls == 0U &&
                fixture.cluster.cluster_id_round == 0U);
    fixture.profiles[1U].wire_format = UCN_CLUSTER_WIRE_V4_FORMAT_VERSION;
    fixture.profiles[2U].capabilities =
        UCN_CLUSTER_REKEY_CAPABILITY_PERSISTENCE;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_ACCESS);
    ASSERT_TRUE(fixture.provider_calls == 0U &&
                fixture.cluster.cluster_id_round == 0U);

    fixture.profiles[2U].capabilities =
        UCN_CLUSTER_REKEY_REQUIRED_CAPABILITIES;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) == UCN_OK);
    prepared = fixture.durable;
    prepared.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    prepared.rekey_transaction.transaction_id = transaction.transaction_id;
    prepared.rekey_transaction.staging_rekey = transaction.durable_rekey_ref;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_begin_collection(
        &transaction, &prepared) == UCN_OK);
    ASSERT_TRUE(transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS &&
                transaction.ack_bitmap == UINT64_C(1) &&
                !ucn_cluster_rekey_transaction_quorum_reached(&transaction));

    make_ack_frame(&transaction, &ack_frame);
    before = transaction;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
        &transaction, &prepared, &ack_frame, 4U, 1U) == UCN_ERR_ACCESS);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ack_frame.words[2U]++;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
        &transaction, &prepared, &ack_frame, 2U, 1U) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ack_frame.words[2U]--;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
        &transaction, &prepared, &ack_frame, 2U, 1U) == UCN_OK);
    ASSERT_TRUE(transaction.state == UCN_CLUSTER_REKEY_STATE_QUORUM &&
                ucn_cluster_rekey_transaction_quorum_reached(&transaction));
    before = transaction;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
        &transaction, &prepared, &ack_frame, 2U, 1U) == UCN_OK);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ack_frame.words[5U]--;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
        &transaction, &prepared, &ack_frame, 2U, 1U) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    return 0;
}

static int test_begin_rejects_invalid_authority_and_record(void)
{
    fixture_t fixture;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t zero;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&transaction, 0, sizeof(transaction));
    zero = transaction;

    fixture.cluster.authority_phase = UCN_CLUSTER_PHASE_HEAD_RECONFIGURING;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0);

    fixture.cluster.authority_phase = UCN_CLUSTER_PHASE_HEAD_STABLE;
    fixture.durable.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0);

    fixture.durable.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    fixture.durable.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0);
    return 0;
}

static int test_begin_refreshes_quorum_and_checks_serial(void)
{
    fixture_t fixture;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t zero;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&transaction, 0, sizeof(transaction));
    zero = transaction;
    fixture.now_ms = 91U;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 91U) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(!fixture.cluster.authority_active &&
                memcmp(&transaction, &zero, sizeof(transaction)) == 0);

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 0U, 7U, 1U) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0);
    ASSERT_TRUE(fixture_begin(
        &fixture, &transaction,
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U, 7U, 1U) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&transaction, &zero, sizeof(transaction)) == 0);
    return 0;
}

static int test_threshold_routing_and_no_wrap(void)
{
    ucn_cluster_rekey_serial_view_t view;
    ucn_cluster_rekey_threshold_decision_t decision;
    ucn_cluster_rekey_threshold_decision_t before;

    (void)memset(&view, 0, sizeof(view));
    (void)memset(&decision, 0xA5, sizeof(decision));
    view.term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD - 1U;
    view.config_id = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD - 1U;
    ASSERT_TRUE(ucn_cluster_rekey_threshold_evaluate(&view, &decision) ==
                UCN_OK);
    ASSERT_TRUE(!decision.rekey_required && decision.trigger_mask == 0U &&
                !decision.snapshot_generation_rotation_required);

    view.term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    ASSERT_TRUE(ucn_cluster_rekey_threshold_evaluate(&view, &decision) ==
                UCN_OK);
    ASSERT_TRUE(decision.rekey_required &&
                decision.trigger_mask == UCN_CLUSTER_REKEY_TRIGGER_TERM);

    view.term = 1U;
    view.config_id = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    ASSERT_TRUE(ucn_cluster_rekey_threshold_evaluate(&view, &decision) ==
                UCN_OK);
    ASSERT_TRUE(decision.trigger_mask ==
                UCN_CLUSTER_REKEY_TRIGGER_CONFIG_ID);

    view.config_id = 1U;
    view.has_backup = true;
    view.backup_generation = 4U;
    view.has_snapshot = true;
    view.snapshot_id = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    ASSERT_TRUE(ucn_cluster_rekey_threshold_evaluate(&view, &decision) ==
                UCN_OK);
    ASSERT_TRUE(!decision.rekey_required &&
                decision.snapshot_generation_rotation_required);

    view.backup_generation = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    ASSERT_TRUE(ucn_cluster_rekey_threshold_evaluate(&view, &decision) ==
                UCN_OK);
    ASSERT_TRUE(decision.rekey_required &&
                decision.trigger_mask ==
                    UCN_CLUSTER_REKEY_TRIGGER_BACKUP_GENERATION &&
                !decision.snapshot_generation_rotation_required);

    before = decision;
    view.term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    ASSERT_TRUE(ucn_cluster_rekey_threshold_evaluate(&view, &decision) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&decision, &before, sizeof(decision)) == 0);
    view.term = 1U;
    view.has_backup = false;
    ASSERT_TRUE(ucn_cluster_rekey_threshold_evaluate(&view, &decision) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&decision, &before, sizeof(decision)) == 0);
    return 0;
}

static int test_persist_sync_prepare_commit_and_reload(void)
{
    fixture_t fixture;
    persist_fake_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_rekey_persist_owner_t owner;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_wire_v4_frame_t ack;
    ucn_cluster_wire_v4_frame_t commit;
    ucn_cluster_rekey_successor_state_t successor_state;
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    ucn_cluster_persist_state_t reloaded;
    uint32_t record_generation = 0U;
    bool durable = false;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_OK);
    persist_provider_init(&provider, &fake, &fixture.durable);
    ASSERT_TRUE(ucn_cluster_rekey_persist_owner_init(
                    &owner, &provider, &fixture.authority) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, 1U, &durable) == UCN_OK &&
                durable && fake.submit_calls == 1U &&
                transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS);
    make_ack_frame(&transaction, &ack);
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
                    &transaction, &owner.durable_state, &ack, 2U, 2U) ==
                    UCN_OK &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_QUORUM);
    durable = false;
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_commit(
                    &owner, 2U, &transaction, 2U, &durable) == UCN_OK &&
                durable && fake.submit_calls == 2U &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE &&
                transaction.authority_revoked &&
                !fixture.cluster.authority_active &&
                fixture.cluster.authority_fence_reason ==
                    UCN_CLUSTER_AUTHORITY_FENCE_REKEY_COMMIT &&
                owner.durable_state.tombstone.valid &&
                owner.durable_state.boot_incarnation == 2U);
    (void)memset(&commit, 0xA5, sizeof(commit));
    ASSERT_TRUE(ucn_cluster_rekey_commit_frame_build(
                    &transaction, &owner.durable_state, &commit) == UCN_OK &&
                commit.type == UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT);
    ASSERT_TRUE(ucn_cluster_rekey_tombstone_admit_frame(
                    &owner.durable_state, &commit) == UCN_ERR_REPLAY);
    commit.term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&commit) &&
                ucn_cluster_rekey_tombstone_admit_frame(
                    &owner.durable_state, &commit) == UCN_ERR_REPLAY);
    ASSERT_TRUE(ucn_cluster_persist_record_encode(
                    &owner.durable_state, 2U, record, sizeof(record)) == UCN_OK &&
                ucn_cluster_persist_record_decode(
                    record, sizeof(record), &record_generation, &reloaded) ==
                    UCN_OK && record_generation == 2U &&
                ucn_cluster_rekey_tombstone_admit_frame(
                    &reloaded, &commit) == UCN_ERR_REPLAY);
    fixture.cluster.backup_node_id = 3U; /* replacement after frozen Prepare */
    (void)memset(&successor_state, 0xA5, sizeof(successor_state));
    ASSERT_TRUE(ucn_cluster_rekey_successor_materialize(
                    &transaction, &reloaded, &successor_state) == UCN_OK &&
                successor_state.epoch.cluster_id == 88U &&
                successor_state.epoch.term == 1U &&
                successor_state.config.config_id == 1U &&
                successor_state.voters.config_id == 1U &&
                successor_state.backup_node_id == 2U &&
                successor_state.backup_generation == 1U &&
                successor_state.snapshot_generation == 1U &&
                successor_state.snapshot_id == 1U &&
                ucn_cluster_member_table_count(&successor_state.members) == 2U);
    return 0;
}

static int test_persist_async_blocks_wire_and_reentry(void)
{
    fixture_t fixture;
    persist_fake_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_rekey_persist_owner_t owner;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_wire_v4_frame_t frame;
    ucn_cluster_wire_v4_frame_t before;
    ucn_cluster_wire_v4_frame_t ack;
    ucn_cluster_rekey_persist_action_t action;
    bool durable = false;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_OK);
    persist_provider_init(&provider, &fake, &fixture.durable);
    fake.submit_pending = true;
    fake.poll_pending_once = true;
    fake.reenter_load = true;
    fake.reenter_submit = true;
    fake.reenter_poll = true;
    fake.owner = &owner;
    fake.transaction = &transaction;
    fake.authority = &fixture.authority;
    ASSERT_TRUE(ucn_cluster_rekey_persist_owner_init(
                    &owner, &provider, &fixture.authority) == UCN_OK &&
                fake.load_calls == 1U &&
                fake.reentry_init_result == UCN_ERR_STATE);
    fake.reenter_load = false;
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, 1U, &durable) == UCN_OK &&
                !durable && owner.pending && fake.submit_calls == 1U &&
                fake.reentry_prepare_result == UCN_ERR_STATE &&
                fake.reentry_commit_result == UCN_ERR_STATE &&
                fake.reentry_abort_result == UCN_ERR_STATE &&
                fake.reentry_poll_result == UCN_ERR_STATE &&
                fake.reentry_init_result == UCN_ERR_STATE &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_PREPARE_PENDING);
    (void)memset(&frame, 0x5A, sizeof(frame));
    before = frame;
    ASSERT_TRUE(ucn_cluster_rekey_prepare_frame_build(
                    &transaction, &owner.durable_state, 1U, &frame) ==
                    UCN_ERR_STATE &&
                memcmp(&frame, &before, sizeof(frame)) == 0);
    ASSERT_TRUE(ucn_cluster_rekey_persist_poll(
                    &owner, &transaction, 2U, &durable, &action) == UCN_OK &&
                !durable && action == UCN_CLUSTER_REKEY_PERSIST_ACTION_NONE &&
                owner.pending && fake.reentry_prepare_result == UCN_ERR_STATE &&
                fake.reentry_commit_result == UCN_ERR_STATE &&
                fake.reentry_abort_result == UCN_ERR_STATE &&
                fake.reentry_poll_result == UCN_ERR_STATE &&
                fake.reentry_init_result == UCN_ERR_STATE);
    fake.reenter_poll = false;
    ASSERT_TRUE(ucn_cluster_rekey_persist_poll(
                    &owner, &transaction, 3U, &durable, &action) == UCN_OK &&
                durable && action ==
                    UCN_CLUSTER_REKEY_PERSIST_ACTION_PREPARE &&
                !owner.pending && transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS);
    make_ack_frame(&transaction, &ack);
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
                    &transaction, &owner.durable_state, &ack, 2U, 3U) ==
                    UCN_OK);
    fake.poll_pending_once = false;
    durable = false;
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_commit(
                    &owner, 2U, &transaction, 4U, &durable) == UCN_OK &&
                !durable && owner.pending && transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COMMIT_PENDING &&
                transaction.authority_revoked);
    (void)memset(&frame, 0x5A, sizeof(frame));
    before = frame;
    ASSERT_TRUE(ucn_cluster_rekey_commit_frame_build(
                    &transaction, &owner.durable_state, &frame) ==
                    UCN_ERR_STATE &&
                memcmp(&frame, &before, sizeof(frame)) == 0);
    ASSERT_TRUE(ucn_cluster_rekey_persist_poll(
                    &owner, &transaction, 5U, &durable, &action) == UCN_OK &&
                durable && action == UCN_CLUSTER_REKEY_PERSIST_ACTION_COMMIT &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE);
    return 0;
}

static int test_prepared_record_restart_resume(void)
{
    fixture_t fixture;
    persist_fake_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_rekey_persist_owner_t owner;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t resumed;
    ucn_cluster_rekey_transaction_t zero;
    ucn_cluster_persist_state_t reloaded;
    ucn_cluster_wire_v4_frame_t prepare;
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint32_t generation = 0U;
    ucn_result_t resume_result;
    bool durable = false;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&transaction, 0, sizeof(transaction));
    (void)memset(&resumed, 0, sizeof(resumed));
    zero = resumed;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_OK);
    persist_provider_init(&provider, &fake, &fixture.durable);
    ASSERT_TRUE(ucn_cluster_rekey_persist_owner_init(
                    &owner, &provider, &fixture.authority) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, 1U, &durable) == UCN_OK &&
                durable && transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS);
    ASSERT_TRUE(ucn_cluster_persist_record_encode(
                    &owner.durable_state, 9U, record, sizeof(record)) ==
                    UCN_OK &&
                ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &reloaded) == UCN_OK &&
                generation == 9U &&
                reloaded.rekey_transaction.staging_rekey.prepare_nonce == 7U &&
                reloaded.rekey_transaction.staging_rekey.
                    successor_backup_node_id == 2U &&
                reloaded.rekey_transaction.staging_rekey.
                    allocation_history_fingerprint ==
                    transaction.allocation_history_fingerprint);

    /* ACK progress is volatile across reset. Resume proves the exact durable
     * PREPARE and restarts from the local Head vote only. */
    resume_result = ucn_cluster_rekey_transaction_resume_prepared(
        &resumed, &fixture.authority, &reloaded,
        fixture.profiles, fixture.profile_count, &fixture.id_history,
        fixture.id_history_generation, 2U);
    ASSERT_TRUE(resume_result == UCN_OK &&
                resumed.state == UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS &&
                resumed.transaction_id == 1U && resumed.nonce == 7U &&
                resumed.successor_backup_node_id == 2U &&
                resumed.ack_bitmap == UINT64_C(1) &&
                resumed.deadline_ms == 1002U);
    (void)memset(&prepare, 0xA5, sizeof(prepare));
    ASSERT_TRUE(ucn_cluster_rekey_prepare_frame_build(
                    &resumed, &reloaded, 2U, &prepare) == UCN_OK &&
                prepare.words[2U] == 1U && prepare.words[5U] == 7U);

    (void)memset(&resumed, 0, sizeof(resumed));
    reloaded.rekey_transaction.staging_rekey.successor_backup_node_id = 4U;
    ASSERT_TRUE(ucn_cluster_rekey_transaction_resume_prepared(
                    &resumed, &fixture.authority, &reloaded,
                    fixture.profiles, fixture.profile_count,
                    &fixture.id_history, fixture.id_history_generation,
                    10U) ==
                    UCN_ERR_STATE &&
                memcmp(&resumed, &zero, sizeof(resumed)) == 0);
    return 0;
}

static int test_persist_failure_fences_authority(void)
{
    fixture_t fixture;
    persist_fake_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_rekey_persist_owner_t owner;
    ucn_cluster_rekey_transaction_t transaction;
    bool durable = false;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                UCN_OK);
    persist_provider_init(&provider, &fake, &fixture.durable);
    fake.fail_submit = true;
    ASSERT_TRUE(ucn_cluster_rekey_persist_owner_init(
                    &owner, &provider, &fixture.authority) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, 1U, &durable) == UCN_ERR_NETWORK &&
                !durable && owner.faulted &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_FENCED &&
                transaction.authority_revoked &&
                fixture.cluster.persistence_faulted &&
                !fixture.cluster.authority_active);
    return 0;
}

static int test_ack_loss_times_out_without_commit(void)
{
    fixture_t fixture;
    persist_fake_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_rekey_persist_owner_t owner;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_rekey_transaction_t next_transaction;
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    ucn_cluster_persist_state_t reloaded;
    uint32_t generation = 0U;
    bool durable = false;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) == UCN_OK);
    persist_provider_init(&provider, &fake, &fixture.durable);
    ASSERT_TRUE(ucn_cluster_rekey_persist_owner_init(
                    &owner, &provider, &fixture.authority) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, 1U, &durable) == UCN_OK &&
                durable && fake.submit_calls == 1U &&
                transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS);
    ASSERT_TRUE(ucn_cluster_rekey_transaction_step(
                    &transaction, transaction.deadline_ms - 1U) == UCN_OK &&
                transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS);
    ASSERT_TRUE(ucn_cluster_rekey_transaction_step(
                    &transaction, transaction.deadline_ms) == UCN_ERR_TTL &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_ABORTED &&
                fake.submit_calls == 1U && fixture.cluster.authority_active);
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_abort(
                    &owner, 2U, &transaction, transaction.deadline_ms,
                    &durable) == UCN_OK &&
                durable && fake.submit_calls == 2U &&
                owner.durable_state.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED &&
                owner.durable_state.rekey_transaction.transaction_id == 1U &&
                fixture.cluster.authority_active &&
                !owner.durable_state.tombstone.valid);
    ASSERT_TRUE(ucn_cluster_persist_record_encode(
                    &owner.durable_state, 3U, record, sizeof(record)) == UCN_OK &&
                ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &reloaded) == UCN_OK &&
                reloaded.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED);

    /* The closed txid cannot be reused, while the exact next txid may start
     * from the unchanged predecessor Authority after a new unique ID. */
    fixture.durable = reloaded;
    fixture.provider_value = 89U;
    (void)memset(&next_transaction, 0, sizeof(next_transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &next_transaction, 1U, 8U, 2U) ==
                    UCN_ERR_REPLAY &&
                next_transaction.state == UCN_CLUSTER_REKEY_STATE_INVALID);
    ASSERT_TRUE(fixture_begin(&fixture, &next_transaction, 2U, 8U,
                              2U) == UCN_OK &&
                next_transaction.successor_epoch.cluster_id == 89U);
    return 0;
}

static int test_expired_prepare_and_ack_are_rejected(void)
{
    fixture_t fixture;
    persist_fake_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_rekey_persist_owner_t owner;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_wire_v4_frame_t ack;
    ucn_cluster_wire_v4_frame_t prepare;
    ucn_cluster_wire_v4_frame_t before_prepare;
    uint64_t before_bitmap;
    bool durable = false;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) == UCN_OK);
    persist_provider_init(&provider, &fake, &fixture.durable);
    ASSERT_TRUE(ucn_cluster_rekey_persist_owner_init(
                    &owner, &provider, &fixture.authority) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, 1U, &durable) == UCN_OK &&
                durable && transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS);
    make_ack_frame(&transaction, &ack);
    (void)memset(&prepare, 0xA5, sizeof(prepare));
    before_prepare = prepare;
    before_bitmap = transaction.ack_bitmap;
    ASSERT_TRUE(ucn_cluster_rekey_prepare_frame_build(
                    &transaction, &owner.durable_state,
                    transaction.deadline_ms, &prepare) == UCN_ERR_TTL &&
                memcmp(&prepare, &before_prepare, sizeof(prepare)) == 0 &&
                transaction.state ==
                    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS);
    ASSERT_TRUE(ucn_cluster_rekey_transaction_note_ack(
                    &transaction, &owner.durable_state, &ack, 2U,
                    transaction.deadline_ms) ==
                    UCN_ERR_TTL &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_ABORTED &&
                transaction.ack_bitmap == before_bitmap &&
                fake.submit_calls == 1U);
    return 0;
}

static int test_expired_prepare_has_zero_provider_io(void)
{
    fixture_t fixture;
    persist_fake_t fake;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_rekey_persist_owner_t owner;
    ucn_cluster_rekey_transaction_t transaction;
    bool durable = true;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&owner, 0, sizeof(owner));
    (void)memset(&transaction, 0, sizeof(transaction));
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) == UCN_OK);
    persist_provider_init(&provider, &fake, &fixture.durable);
    ASSERT_TRUE(ucn_cluster_rekey_persist_owner_init(
                    &owner, &provider, &fixture.authority) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_rekey_persist_begin_prepare(
                    &owner, 1U, &transaction, transaction.deadline_ms,
                    &durable) == UCN_ERR_TTL &&
                !durable && fake.submit_calls == 0U && !owner.pending &&
                owner.durable_state.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_ABORTED);
    return 0;
}

static int test_rekey_history_collision_requires_new_round(void)
{
    fixture_t fixture;
    ucn_cluster_rekey_transaction_t transaction;
    ucn_cluster_id_request_t other;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    (void)memset(&transaction, 0, sizeof(transaction));
    (void)memset(&other, 0, sizeof(other));
    other.purpose = UCN_CLUSTER_ID_PURPOSE_REKEY;
    other.local_node_id = 9U;
    other.parent_cluster_id = 66U;
    other.parent_term = 4U;
    other.parent_config_id = 8U;
    other.incarnation = 1U;
    other.round = 1U;
    ASSERT_TRUE(ucn_cluster_id_history_admit(
                    &fixture.id_history, &other, 88U) == UCN_OK);
    fixture.id_history_generation = 1U;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 1U) ==
                    UCN_ERR_REPLAY &&
                fixture.cluster.cluster_id_round == 1U &&
                transaction.state == UCN_CLUSTER_REKEY_STATE_INVALID);
    fixture.provider_value = 89U;
    ASSERT_TRUE(fixture_begin(&fixture, &transaction, 1U, 7U, 2U) == UCN_OK &&
                fixture.provider_request.round == 2U &&
                transaction.successor_epoch.cluster_id == 89U);
    return 0;
}

static int test_cluster_id_history_collision_restart_and_capacity(void)
{
    ucn_cluster_id_history_t history;
    ucn_cluster_id_history_t reloaded;
    ucn_cluster_id_request_t identity;
    uint8_t record[UCN_CLUSTER_ID_HISTORY_RECORD_BYTES];
    uint32_t generation = 0U;
    uint32_t index;

    ucn_cluster_id_history_init(&history);
    (void)memset(&identity, 0, sizeof(identity));
    identity.purpose = UCN_CLUSTER_ID_PURPOSE_REKEY;
    identity.local_node_id = 1U;
    identity.parent_cluster_id = 77U;
    identity.parent_term = 5U;
    identity.parent_config_id = 10U;
    identity.incarnation = 1U;
    identity.round = 1U;
    ASSERT_TRUE(ucn_cluster_id_history_admit(&history, &identity, 88U) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_id_history_admit(&history, &identity, 88U) ==
                UCN_OK && history.count == 1U);
    ASSERT_TRUE(ucn_cluster_id_history_admit(&history, &identity, 90U) ==
                UCN_ERR_REPLAY && history.count == 1U);
    identity.local_node_id = 2U;
    ASSERT_TRUE(ucn_cluster_id_history_admit(&history, &identity, 88U) ==
                UCN_ERR_REPLAY && history.count == 1U);
    identity.round = 2U;
    ASSERT_TRUE(ucn_cluster_id_history_admit(&history, &identity, 89U) ==
                UCN_OK && history.count == 2U);
    ASSERT_TRUE(ucn_cluster_id_history_record_encode(
                    &history, 3U, record, sizeof(record)) == UCN_OK &&
                ucn_cluster_id_history_record_decode(
                    record, sizeof(record), &generation, &reloaded) == UCN_OK &&
                generation == 3U && reloaded.count == 2U);
    identity.local_node_id = 3U;
    identity.round = 3U;
    ASSERT_TRUE(ucn_cluster_id_history_admit(&reloaded, &identity, 88U) ==
                UCN_ERR_REPLAY);
    for (index = reloaded.count; index < UCN_CLUSTER_ID_HISTORY_CAPACITY;
         ++index) {
        identity.local_node_id = index + 10U;
        identity.round = index + 10U;
        ASSERT_TRUE(ucn_cluster_id_history_admit(
                        &reloaded, &identity, UINT32_C(1000) + index) == UCN_OK);
    }
    identity.local_node_id = 99U;
    identity.round = 99U;
    ASSERT_TRUE(ucn_cluster_id_history_admit(&reloaded, &identity, 2000U) ==
                UCN_ERR_NO_SPACE);
    record[20U] ^= 1U;
    ASSERT_TRUE(ucn_cluster_id_history_record_decode(
                    record, sizeof(record), &generation, &history) ==
                UCN_ERR_MALFORMED);

    /* The canonical empty history is a valid first durable generation. */
    ucn_cluster_id_history_init(&history);
    (void)memset(record, 0xA5, sizeof(record));
    ASSERT_TRUE(ucn_cluster_id_history_record_encode(
                    &history, 1U, record, sizeof(record)) == UCN_OK);
    (void)memset(&reloaded, 0xA5, sizeof(reloaded));
    generation = 0U;
    ASSERT_TRUE(ucn_cluster_id_history_record_decode(
                    record, sizeof(record), &generation, &reloaded) ==
                UCN_OK);
    ASSERT_TRUE(generation == 1U && reloaded.count == 0U);

    /* Encode must not mint a CRC-valid record from a hand-built invalid or
     * conflicting history. */
    ucn_cluster_id_history_init(&history);
    history.count = 1U;
    history.entries[0U].cluster_id = 0U;
    history.entries[0U].identity = identity;
    ASSERT_TRUE(ucn_cluster_id_history_record_encode(
                    &history, 2U, record, sizeof(record)) == UCN_ERR_ARGUMENT);
    history.entries[0U].cluster_id = 88U;
    history.count = 2U;
    history.entries[1U] = history.entries[0U];
    ASSERT_TRUE(ucn_cluster_id_history_record_encode(
                    &history, 2U, record, sizeof(record)) == UCN_ERR_ARGUMENT);
    history.entries[1U].cluster_id = 89U;
    ASSERT_TRUE(ucn_cluster_id_history_record_encode(
                    &history, 2U, record, sizeof(record)) == UCN_ERR_ARGUMENT);
    history.entries[1U] = history.entries[0U];
    history.entries[1U].identity.round++;
    ASSERT_TRUE(ucn_cluster_id_history_record_encode(
                    &history, 2U, record, sizeof(record)) == UCN_ERR_ARGUMENT);
    history.count = 1U;
    history.entries[0U].identity.parent_cluster_id = UCN_NODE_BROADCAST;
    ASSERT_TRUE(ucn_cluster_id_history_record_encode(
                    &history, 2U, record, sizeof(record)) == UCN_ERR_ARGUMENT);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_valid_begin_and_single_owner();
    result |= test_id_history_durable_gate();
    result |= test_begin_rejects_invalid_authority_and_record();
    result |= test_begin_refreshes_quorum_and_checks_serial();
    result |= test_provider_validation_and_round_atomicity();
    result |= test_wire_prepare_ack_commit_binding();
    result |= test_profile_gate_and_old_config_quorum();
    result |= test_threshold_routing_and_no_wrap();
    result |= test_persist_sync_prepare_commit_and_reload();
    result |= test_persist_async_blocks_wire_and_reentry();
    result |= test_prepared_record_restart_resume();
    result |= test_persist_failure_fences_authority();
    result |= test_ack_loss_times_out_without_commit();
    result |= test_expired_prepare_and_ack_are_rejected();
    result |= test_expired_prepare_has_zero_provider_io();
    result |= test_rekey_history_collision_requires_new_round();
    result |= test_cluster_id_history_collision_restart_and_capacity();
    if (result == 0) {
        printf("All UCN Cluster M13 rekey tests passed.\n");
    }
    return result;
}
