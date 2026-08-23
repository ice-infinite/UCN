#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_takeover_persist.h"

#include "../src/extended/cluster/ucn_cluster_takeover_internal.h"

#define TEST_PERSIST_CRC_OFFSET 12U
#define TEST_PERSIST_SCHEMA_OFFSET 4U
#define TEST_PERSIST_SIZE_OFFSET 6U

#define ASSERT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                      \
            (void)fprintf(stderr, "ASSERT %s at %s:%d\\n", #condition,       \
                          __FILE__, __LINE__);                                   \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static void test_write_u16_be(uint8_t *output, uint16_t value)
{
    output[0U] = (uint8_t)(value >> 8U);
    output[1U] = (uint8_t)value;
}

static void test_write_u32_be(uint8_t *output, uint32_t value)
{
    output[0U] = (uint8_t)(value >> 24U);
    output[1U] = (uint8_t)(value >> 16U);
    output[2U] = (uint8_t)(value >> 8U);
    output[3U] = (uint8_t)value;
}

static uint32_t test_persist_crc32(const uint8_t *record, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t index;

    for (index = 0U; index < length; ++index) {
        uint8_t value = index >= TEST_PERSIST_CRC_OFFSET &&
                                index < TEST_PERSIST_CRC_OFFSET + 4U ?
                            0U :
                            record[index];
        uint8_t bit;

        crc ^= value;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ?
                      (crc >> 1U) ^ UINT32_C(0xEDB88320) :
                      crc >> 1U;
        }
    }
    return crc ^ UINT32_MAX;
}

/* The public encoder intentionally never emits an old schema. This fixture
 * models an actual v2 slot embedded in a newly-sized (292 B) dual-slot
 * storage buffer: its trailing v3 extension is erased/zero and CRC still
 * covers only the old 280 B physical record. */
static ucn_result_t test_encode_legacy_v2_fixture(
    const ucn_cluster_persist_state_t *state,
    uint32_t generation,
    uint8_t *output)
{
    ucn_cluster_persist_state_t writer;
    uint8_t v3_record[UCN_CLUSTER_PERSIST_RECORD_BYTES];

    if (state == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    writer = *state;
    writer.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3;
    if (ucn_cluster_persist_record_encode(&writer, generation, v3_record,
                                          sizeof(v3_record)) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    (void)memset(output, 0, UCN_CLUSTER_PERSIST_RECORD_BYTES);
    (void)memcpy(output, v3_record, UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES);
    test_write_u16_be(output + TEST_PERSIST_SCHEMA_OFFSET,
                      UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V2);
    test_write_u16_be(output + TEST_PERSIST_SIZE_OFFSET,
                      (uint16_t)UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES);
    test_write_u32_be(output + TEST_PERSIST_CRC_OFFSET,
                      test_persist_crc32(output,
                                         UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES));
    return UCN_OK;
}

static bool make_epoch(ucn_cluster_backup_epoch_t *epoch)
{
    if (epoch == NULL) {
        return false;
    }
    (void)memset(epoch, 0, sizeof(*epoch));
    epoch->cluster_id = 21U;
    epoch->term = 9U;
    epoch->head_node_id = 1U;
    epoch->backup_node_id = 2U;
    epoch->backup_generation = 4U;
    return true;
}

static bool make_stable_config(ucn_cluster_config_state_t *config)
{
    static const ucn_node_id_t voters[] = {1U, 2U, 3U};

    return config != NULL &&
           ucn_cluster_config_state_init_stable(config, 6U, voters,
                                                sizeof(voters) / sizeof(voters[0U]));
}

static bool make_joint_config(ucn_cluster_config_state_t *config)
{
    static const ucn_node_id_t old_voters[] = {1U, 2U, 3U};
    static const ucn_node_id_t new_voters[] = {2U, 3U, 4U};
    ucn_cluster_config_state_t stable;

    (void)memset(&stable, 0, sizeof(stable));
    return config != NULL &&
           ucn_cluster_config_state_init_stable(&stable, 6U, old_voters,
                                                sizeof(old_voters) / sizeof(old_voters[0U])) &&
           ucn_cluster_config_state_init_joint(config, &stable, new_voters,
                                               sizeof(new_voters) / sizeof(new_voters[0U]));
}

static bool make_committed_owner(ucn_cluster_backup_sync_owner_t *owner,
                                 bool joint)
{
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_snapshot_epoch_t snapshot;

    if (owner == NULL || !make_epoch(&epoch) ||
        !(joint ? make_joint_config(&config) : make_stable_config(&config)) ||
        ucn_cluster_backup_sync_owner_init(owner, &epoch, &config) != UCN_OK ||
        !ucn_cluster_snapshot_epoch_from_config(&snapshot, &epoch, 7U, &config)) {
        return false;
    }
    owner->mirror.committed_epoch = snapshot;
    owner->mirror.committed_valid = true;
    return ucn_cluster_backup_sync_owner_is_valid(owner) &&
           ucn_cluster_backup_sync_owner_takeover_eligible(owner);
}

static bool begin_transaction(ucn_cluster_takeover_transaction_t *transaction,
                              ucn_cluster_backup_sync_owner_t *owner,
                              bool joint,
                              uint32_t now_ms)
{
    (void)memset(owner, 0, sizeof(*owner));
    (void)memset(transaction, 0, sizeof(*transaction));
    return make_committed_owner(owner, joint) &&
           ucn_cluster_takeover_transaction_begin(transaction, owner, 31U,
                                                  now_ms, 100U) == UCN_OK;
}

static ucn_result_t note_remote_vote(ucn_cluster_takeover_transaction_t *transaction,
                                     const ucn_cluster_takeover_vote_id_t *vote_id,
                                     ucn_node_id_t voter_node_id)
{
    ucn_cluster_takeover_remote_vote_proof_t proof;

    (void)memset(&proof, 0, sizeof(proof));
    proof.member.voter_node_id = voter_node_id;
    proof.member.member_takeover_grace = true;
    proof.member.old_head_lease_expired = true;
    proof.member.committed_v4_voter = true;
    proof.exact_vote_durable = true;
    return ucn_cluster_takeover_transaction_note_durable_vote(transaction, vote_id,
                                                               &proof);
}

static int test_freeze_and_member_vote_gate(void)
{
    ucn_cluster_takeover_transaction_t transaction;
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_config_state_t frozen;
    ucn_cluster_takeover_member_vote_context_t context;
    ucn_cluster_takeover_remote_vote_proof_t proof;
    ucn_cluster_takeover_transaction_t before;

    ASSERT_TRUE(begin_transaction(&transaction, &owner, false, 10U));
    ASSERT_TRUE(transaction.vote_id.cluster_id == 21U &&
                transaction.vote_id.old_term == 9U &&
                transaction.vote_id.proposed_term == 10U &&
                transaction.vote_id.config_id == 6U &&
                transaction.vote_id.backup_node_id == 2U &&
                transaction.vote_id.backup_generation == 4U &&
                transaction.vote_id.snapshot_id == 7U);
    frozen = transaction.frozen_config;
    ASSERT_TRUE(make_joint_config(&owner.active_config));
    ASSERT_TRUE(memcmp(&transaction.frozen_config, &frozen, sizeof(frozen)) == 0);

    (void)memset(&context, 0, sizeof(context));
    context.voter_node_id = 3U;
    before = transaction;
    ASSERT_TRUE(ucn_cluster_takeover_member_vote_gate(&transaction, &context) ==
                UCN_ERR_ACCESS);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    context.member_takeover_grace = true;
    context.old_head_lease_expired = true;
    context.committed_v4_voter = true;
    ASSERT_TRUE(ucn_cluster_takeover_member_vote_gate(&transaction, &context) ==
                UCN_OK);
    context.voter_node_id = 99U;
    ASSERT_TRUE(ucn_cluster_takeover_member_vote_gate(&transaction, &context) ==
                UCN_ERR_ACCESS);
    (void)memset(&proof, 0, sizeof(proof));
    proof.member.voter_node_id = 3U;
    proof.member.member_takeover_grace = true;
    proof.member.old_head_lease_expired = true;
    proof.exact_vote_durable = true;
    before = transaction;
    ASSERT_TRUE(ucn_cluster_takeover_transaction_note_durable_vote(
                    &transaction, &transaction.vote_id, &proof) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    return 0;
}

static int test_stable_durable_quorum_certificate_and_fence(void)
{
    ucn_cluster_takeover_transaction_t transaction;
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_takeover_certificate_t certificate;
    ucn_cluster_takeover_certificate_t tampered;
    ucn_cluster_takeover_certificate_fragment_t fragment;
    ucn_cluster_takeover_certificate_fragment_t sentinel;
    ucn_cluster_takeover_old_primary_fence_t fence;
    ucn_cluster_epoch_t old_primary_epoch;

    ASSERT_TRUE(begin_transaction(&transaction, &owner, false, 10U));
    ASSERT_TRUE(!ucn_cluster_takeover_transaction_quorum_reached(&transaction));
    ASSERT_TRUE(note_remote_vote(&transaction, &transaction.vote_id, 3U) == UCN_OK);
    ASSERT_TRUE(!ucn_cluster_takeover_transaction_quorum_reached(&transaction));
    ASSERT_TRUE(ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
                    &transaction, &transaction.vote_id) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_quorum_reached(&transaction));
    (void)memset(&certificate, 0, sizeof(certificate));
    ASSERT_TRUE(ucn_cluster_takeover_certificate_build(&transaction, &certificate) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_takeover_certificate_is_valid(&transaction, &certificate));
    ASSERT_TRUE(ucn_cluster_takeover_certificate_fragment_get(
                    &transaction, &certificate, UCN_CLUSTER_TAKEOVER_SET_OLD,
                    0U, &fragment) == UCN_OK);
    ASSERT_TRUE(fragment.fragment_count == 1U && fragment.config_id == 6U &&
                ucn_cluster_takeover_certificate_fragment_is_valid(
                    &transaction, &certificate, &fragment));
    sentinel = fragment;
    ASSERT_TRUE(ucn_cluster_takeover_certificate_fragment_get(
                    &transaction, &certificate, UCN_CLUSTER_TAKEOVER_SET_NEW,
                    0U, &fragment) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&fragment, &sentinel, sizeof(fragment)) == 0);
    tampered = certificate;
    tampered.old_vote_words[0U] &= ~(UINT32_C(1) << 1U);
    ASSERT_TRUE(!ucn_cluster_takeover_certificate_is_valid(&transaction, &tampered));

    (void)memset(&old_primary_epoch, 0, sizeof(old_primary_epoch));
    old_primary_epoch.cluster_id = 21U;
    old_primary_epoch.term = 9U;
    old_primary_epoch.head_node_id = 1U;
    ucn_cluster_takeover_old_primary_fence_reset(&fence);
    ASSERT_TRUE(ucn_cluster_takeover_old_primary_fence_accept(
                    &fence, &old_primary_epoch, &transaction, &certificate) == UCN_OK);
    ASSERT_TRUE(fence.fenced && fence.join_required && fence.accepted_epoch.term == 10U);
    ASSERT_TRUE(ucn_cluster_takeover_old_primary_fence_accept(
                    &fence, &old_primary_epoch, &transaction, &certificate) == UCN_OK);
    old_primary_epoch.term = 8U;
    ASSERT_TRUE(ucn_cluster_takeover_old_primary_fence_accept(
                    &fence, &old_primary_epoch, &transaction, &certificate) ==
                UCN_ERR_ACCESS);
    return 0;
}

static int test_joint_double_quorum_and_fragment_binding(void)
{
    ucn_cluster_takeover_transaction_t transaction;
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_takeover_certificate_t certificate;
    ucn_cluster_takeover_certificate_fragment_t fragment;
    ucn_cluster_takeover_certificate_fragment_t altered;

    ASSERT_TRUE(begin_transaction(&transaction, &owner, true, 20U));
    ASSERT_TRUE(ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
                    &transaction, &transaction.vote_id) == UCN_OK);
    ASSERT_TRUE(note_remote_vote(&transaction, &transaction.vote_id, 3U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_quorum_reached(&transaction));
    ASSERT_TRUE(ucn_cluster_takeover_certificate_build(&transaction, &certificate) ==
                UCN_OK);
    ASSERT_TRUE(certificate.required_set_mask ==
                (UCN_CLUSTER_TAKEOVER_SET_OLD | UCN_CLUSTER_TAKEOVER_SET_NEW));
    ASSERT_TRUE(certificate.certificate_anchor_config_id == 7U);
    ASSERT_TRUE(ucn_cluster_takeover_certificate_fragment_get(
                    &transaction, &certificate, UCN_CLUSTER_TAKEOVER_SET_NEW,
                    0U, &fragment) == UCN_OK);
    ASSERT_TRUE(fragment.config_id == 7U && fragment.fragment_count == 1U);
    altered = fragment;
    altered.config_hash++;
    ASSERT_TRUE(!ucn_cluster_takeover_certificate_fragment_is_valid(
        &transaction, &certificate, &altered));
    return 0;
}

static int test_impossible_timeout_and_durable_epoch(void)
{
    ucn_cluster_takeover_transaction_t transaction;
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_takeover_transaction_t before;

    ASSERT_TRUE(begin_transaction(&transaction, &owner, false, UINT32_MAX - 50U));
    before = transaction;
    ASSERT_TRUE(ucn_cluster_takeover_transaction_note_voter_unreachable(
                    &transaction, 2U) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_note_voter_unreachable(
                    &transaction, 3U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_note_voter_unreachable(
                    &transaction, 1U) == UCN_ERR_STATE);
    ASSERT_TRUE(!transaction.active && transaction.recovery_required &&
                transaction.frozen_config.old_set.count == 3U);
    ASSERT_TRUE(note_remote_vote(&transaction, &before.vote_id, 1U) == UCN_ERR_REPLAY);

    ASSERT_TRUE(begin_transaction(&transaction, &owner, false, UINT32_MAX - 50U));
    ASSERT_TRUE(ucn_cluster_takeover_transaction_step(&transaction, 48U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_step(&transaction, 49U) == UCN_ERR_STATE);
    ASSERT_TRUE(!transaction.active && transaction.recovery_required);

    ASSERT_TRUE(begin_transaction(&transaction, &owner, false, 100U));
    ASSERT_TRUE(ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
                    &transaction, &transaction.vote_id) == UCN_OK);
    ASSERT_TRUE(note_remote_vote(&transaction, &transaction.vote_id, 3U) == UCN_OK);
    ASSERT_TRUE(!ucn_cluster_takeover_transaction_head_result_ready(&transaction));
    ASSERT_TRUE(ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
                    &transaction, &transaction.vote_id) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_head_result_ready(&transaction));
    /* R32/R34: a durable successor is terminal. Check both before and after
     * its deadline. The unreachable node is deliberately unvoted (node 1),
     * so rejection proves the terminal gate rather than the older
     * vote/unreachable-bitmap-overlap guard. */
    before = transaction;
    ASSERT_TRUE(ucn_cluster_takeover_transaction_step(&transaction, 150U) == UCN_OK);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_step(&transaction, 1000U) == UCN_OK);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ASSERT_TRUE(note_remote_vote(&transaction, &transaction.vote_id, 1U) ==
                UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_note_voter_unreachable(
                    &transaction, 1U) == UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
                    &transaction, &transaction.vote_id) == UCN_OK);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0 &&
                ucn_cluster_takeover_transaction_head_result_ready(&transaction));
    return 0;
}

static void make_persist_state_from_transaction(
    ucn_cluster_persist_state_t *state,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    ucn_cluster_persist_state_init_empty(state);
    state->boot_incarnation = 1U;
    state->has_active_epoch = true;
    state->active_epoch.cluster_id = transaction->vote_id.cluster_id;
    state->active_epoch.term = transaction->vote_id.old_term;
    state->active_epoch.head_node_id =
        transaction->frozen_snapshot_epoch.backup_epoch.head_node_id;
    state->has_max_epoch = true;
    state->max_epoch = state->active_epoch;
    state->committed_config.valid = true;
    state->committed_config.config_id = transaction->vote_id.config_id;
    state->committed_config.generation = transaction->vote_id.config_id;
    state->committed_config.digest[0U] = 0xA5U;
}

typedef struct takeover_persist_fake {
    ucn_cluster_persist_state_t stored;
    ucn_cluster_persist_request_t pending_request;
    ucn_cluster_takeover_persist_owner_t *reentry_owner;
    const ucn_cluster_takeover_transaction_t *reentry_transaction;
    const ucn_cluster_persist_provider_t *provider;
    int reentry_result;
    uint8_t pending_polls;
    bool pending;
    bool fail_submit;
    bool fail_poll;
    bool reenter_load;
    bool reenter_submit;
    bool reenter_poll;
} takeover_persist_fake_t;

static ucn_result_t takeover_fake_load(void *context,
                                       ucn_cluster_persist_load_result_t *result)
{
    takeover_persist_fake_t *fake = (takeover_persist_fake_t *)context;

    if (fake == NULL || result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (fake->reenter_load) {
        fake->reentry_result = ucn_cluster_takeover_persist_owner_init(
            fake->reentry_owner, fake->provider);
    }
    (void)memset(result, 0, sizeof(*result));
    result->state = UCN_CLUSTER_PERSIST_LOAD_READY;
    result->snapshot = fake->stored;
    return UCN_OK;
}

static ucn_cluster_persist_completion_t takeover_fake_completion(
    ucn_cluster_persist_completion_state_t state, ucn_result_t failure)
{
    ucn_cluster_persist_completion_t completion;

    (void)memset(&completion, 0, sizeof(completion));
    completion.state = state;
    completion.token = state == UCN_CLUSTER_PERSIST_PENDING ? 1U : 0U;
    completion.failure = failure;
    return completion;
}

static ucn_cluster_persist_completion_t takeover_fake_submit(
    void *context, const ucn_cluster_persist_request_t *request)
{
    takeover_persist_fake_t *fake = (takeover_persist_fake_t *)context;

    if (fake == NULL || request == NULL || fake->fail_submit) {
        return takeover_fake_completion(UCN_CLUSTER_PERSIST_FAILED, UCN_ERR_STATE);
    }
    if (fake->reenter_submit) {
        fake->reentry_result = ucn_cluster_takeover_persist_owner_step(
            fake->reentry_owner, fake->reentry_transaction);
    }
    if (ucn_cluster_persist_request_admit(&fake->stored, request) !=
        UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW) {
        return takeover_fake_completion(UCN_CLUSTER_PERSIST_FAILED, UCN_ERR_REPLAY);
    }
    if (fake->pending_polls != 0U) {
        fake->pending_request = *request;
        fake->pending = true;
        return takeover_fake_completion(UCN_CLUSTER_PERSIST_PENDING, UCN_OK);
    }
    fake->stored = request->next_state;
    return takeover_fake_completion(UCN_CLUSTER_PERSIST_COMMITTED, UCN_OK);
}

static ucn_cluster_persist_completion_t takeover_fake_poll(void *context,
                                                            ucn_cluster_persist_token_t token)
{
    takeover_persist_fake_t *fake = (takeover_persist_fake_t *)context;

    if (fake == NULL || token != 1U || !fake->pending || fake->fail_poll) {
        return takeover_fake_completion(UCN_CLUSTER_PERSIST_FAILED, UCN_ERR_STATE);
    }
    if (fake->reenter_poll) {
        fake->reentry_result = ucn_cluster_takeover_persist_owner_step(
            fake->reentry_owner, fake->reentry_transaction);
    }
    --fake->pending_polls;
    if (fake->pending_polls != 0U) {
        return takeover_fake_completion(UCN_CLUSTER_PERSIST_PENDING, UCN_OK);
    }
    fake->stored = fake->pending_request.next_state;
    fake->pending = false;
    return takeover_fake_completion(UCN_CLUSTER_PERSIST_COMMITTED, UCN_OK);
}

static void takeover_fake_provider(ucn_cluster_persist_provider_t *provider,
                                   takeover_persist_fake_t *fake)
{
    (void)memset(provider, 0, sizeof(*provider));
    provider->struct_size = (uint16_t)sizeof(*provider);
    provider->api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider->load = takeover_fake_load;
    provider->submit = takeover_fake_submit;
    provider->poll = takeover_fake_poll;
    provider->context = fake;
    fake->provider = provider;
}

static int test_persisted_full_vote_and_epoch_barriers(void)
{
    ucn_cluster_takeover_transaction_t transaction;
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_persist_state_t state;
    ucn_cluster_persist_state_t reloaded;
    ucn_cluster_persist_request_t vote_request;
    ucn_cluster_persist_request_t epoch_request;
    ucn_cluster_persist_request_t generic_epoch_request;
    ucn_cluster_persist_request_t second_vote_request;
    ucn_cluster_persist_request_t sentinel;
    ucn_cluster_persist_state_t before_generic;
    ucn_cluster_takeover_transaction_t second_transaction;
    ucn_cluster_backup_sync_owner_t second_owner;
    ucn_cluster_backup_epoch_t second_epoch;
    ucn_cluster_config_state_t second_config;
    ucn_cluster_snapshot_epoch_t second_snapshot;
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t corrupted_record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    ucn_cluster_persist_request_t conflicting_replay;
    uint32_t generation = 0U;

    ASSERT_TRUE(begin_transaction(&transaction, &owner, false, 100U));
    make_persist_state_from_transaction(&state, &transaction);
    ASSERT_TRUE(ucn_cluster_persist_state_is_valid(&state));
    (void)memset(&vote_request, 0, sizeof(vote_request));
    ASSERT_TRUE(ucn_cluster_takeover_persist_vote_request_build(
                    &state, &transaction, 41U, &vote_request) == UCN_OK);
    ASSERT_TRUE(vote_request.operation ==
                UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_VOTE_COMMIT &&
                ucn_cluster_persist_request_admit(&state, &vote_request) ==
                    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    state = vote_request.next_state;
    ASSERT_TRUE(ucn_cluster_persist_record_encode(&state, 9U, record,
                                                  sizeof(record)) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_persist_record_decode(record, sizeof(record),
                                                  &generation, &reloaded) == UCN_OK &&
                generation == 9U &&
                ucn_cluster_takeover_persist_vote_matches(&reloaded, &transaction) &&
                reloaded.last_vote.proposed_term == 10U &&
                reloaded.last_vote.config_id == 6U &&
                reloaded.last_vote.snapshot_id == 7U);
    ASSERT_TRUE(ucn_cluster_persist_request_admit(&state, &vote_request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_IDEMPOTENT);
    conflicting_replay = vote_request;
    conflicting_replay.next_state.last_vote.snapshot_id++;
    ASSERT_TRUE(ucn_cluster_persist_request_finalize(&conflicting_replay) == UCN_OK &&
                ucn_cluster_persist_request_admit(&state, &conflicting_replay) !=
                    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW &&
                ucn_cluster_persist_request_admit(&state, &conflicting_replay) !=
                    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_IDEMPOTENT);
    (void)memcpy(corrupted_record, record, sizeof(corrupted_record));
    corrupted_record[sizeof(corrupted_record) - 1U] ^= UINT8_C(0x01);
    ASSERT_TRUE(ucn_cluster_persist_record_decode(
                    corrupted_record, sizeof(corrupted_record), &generation,
                    &reloaded) == UCN_ERR_CRC);
    /* R31: a complete current-Epoch M10 VoteId fences the generic election
     * operation. Its arbitrary successor Head cannot bypass the dedicated
     * certificate-bound TAKEOVER_EPOCH_COMMIT path. */
    (void)memset(&generic_epoch_request, 0, sizeof(generic_epoch_request));
    generic_epoch_request.operation_id = 42U;
    generic_epoch_request.operation = UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT;
    generic_epoch_request.next_state = reloaded;
    generic_epoch_request.next_state.active_epoch.term = 10U;
    generic_epoch_request.next_state.active_epoch.head_node_id = 99U;
    generic_epoch_request.next_state.max_epoch =
        generic_epoch_request.next_state.active_epoch;
    ASSERT_TRUE(ucn_cluster_persist_request_finalize(&generic_epoch_request) == UCN_OK &&
                ucn_cluster_persist_request_is_valid(&generic_epoch_request));
    before_generic = reloaded;
    ASSERT_TRUE(ucn_cluster_persist_request_admit(&reloaded,
                                                  &generic_epoch_request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    ASSERT_TRUE(memcmp(&reloaded, &before_generic, sizeof(reloaded)) == 0);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
                    &transaction, &transaction.vote_id) == UCN_OK);
    ASSERT_TRUE(note_remote_vote(&transaction, &transaction.vote_id, 3U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_takeover_transaction_quorum_reached(&transaction));

    (void)memset(&epoch_request, 0, sizeof(epoch_request));
    ASSERT_TRUE(ucn_cluster_takeover_persist_epoch_request_build(
                    &reloaded, &transaction, 43U, &epoch_request) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_persist_request_admit(&reloaded, &epoch_request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    reloaded = epoch_request.next_state;
    ASSERT_TRUE(ucn_cluster_takeover_persist_epoch_matches(&reloaded, &transaction));
    ASSERT_TRUE(ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
                    &transaction, &transaction.vote_id) == UCN_OK &&
                ucn_cluster_takeover_transaction_head_result_ready(&transaction));

    /* R33: the first dedicated Epoch commit retains VoteId for audit, but
     * the vote is now historical (term 9 versus current term 10). A refreshed
     * Backup snapshot may therefore atomically open a new full M10 Vote. */
    (void)memset(&second_epoch, 0, sizeof(second_epoch));
    second_epoch.cluster_id = 21U;
    second_epoch.term = 10U;
    second_epoch.head_node_id = 2U;
    second_epoch.backup_node_id = 3U;
    second_epoch.backup_generation = 5U;
    (void)memset(&second_owner, 0, sizeof(second_owner));
    ASSERT_TRUE(make_stable_config(&second_config) &&
                ucn_cluster_backup_sync_owner_init(&second_owner, &second_epoch,
                                                    &second_config) == UCN_OK &&
                ucn_cluster_snapshot_epoch_from_config(&second_snapshot,
                                                       &second_epoch, 8U,
                                                       &second_config));
    second_owner.mirror.committed_epoch = second_snapshot;
    second_owner.mirror.committed_valid = true;
    ASSERT_TRUE(ucn_cluster_takeover_transaction_begin(
                    &second_transaction, &second_owner, 44U, 200U, 100U) == UCN_OK &&
                second_transaction.vote_id.old_term == 10U &&
                second_transaction.vote_id.proposed_term == 11U &&
                second_transaction.vote_id.backup_node_id == 3U &&
                reloaded.last_vote.valid && reloaded.last_vote.epoch.term == 9U);
    (void)memset(&second_vote_request, 0, sizeof(second_vote_request));
    ASSERT_TRUE(ucn_cluster_takeover_persist_vote_request_build(
                    &reloaded, &second_transaction, 45U, &second_vote_request) == UCN_OK &&
                ucn_cluster_persist_request_admit(&reloaded, &second_vote_request) ==
                    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW &&
                second_vote_request.next_state.last_vote.epoch.term == 10U &&
                second_vote_request.next_state.last_vote.proposed_term == 11U);

    sentinel = vote_request;
    (void)memset(&state.last_vote, 0, sizeof(state.last_vote));
    state.last_vote.valid = true;
    state.last_vote.epoch = state.active_epoch;
    state.last_vote.voted_for_node_id = 2U;
    state.last_vote.backup_generation = 4U;
    ASSERT_TRUE(!ucn_cluster_persist_vote_is_complete_takeover(&state.last_vote));
    ASSERT_TRUE(ucn_cluster_takeover_persist_vote_request_build(
                    &state, &transaction, 43U, &vote_request) == UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&vote_request, &sentinel, sizeof(vote_request)) == 0);
    return 0;
}

static int test_legacy_v2_partial_vote_never_becomes_m10_proof(void)
{
    ucn_cluster_takeover_transaction_t transaction;
    ucn_cluster_backup_sync_owner_t owner;
    ucn_cluster_persist_state_t state;
    ucn_cluster_persist_state_t decoded;
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_request_t sentinel;
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint32_t generation = 0U;

    ASSERT_TRUE(begin_transaction(&transaction, &owner, false, 100U));
    make_persist_state_from_transaction(&state, &transaction);
    state.last_vote.valid = true;
    state.last_vote.epoch = state.active_epoch;
    state.last_vote.voted_for_node_id = transaction.vote_id.backup_node_id;
    state.last_vote.backup_generation = transaction.vote_id.backup_generation;
    ASSERT_TRUE(!ucn_cluster_persist_vote_is_complete_takeover(&state.last_vote));
    ASSERT_TRUE(test_encode_legacy_v2_fixture(&state, 7U, record) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_persist_record_decode(record, sizeof(record),
                                                  &generation, &decoded) == UCN_OK &&
                generation == 7U &&
                decoded.record_schema_version ==
                    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V2 &&
                decoded.last_vote.valid &&
                !ucn_cluster_persist_vote_is_complete_takeover(&decoded.last_vote) &&
                !ucn_cluster_takeover_persist_vote_matches(&decoded,
                                                            &transaction));
    /* A partial vote for the current active Epoch remains a one-vote fence. */
    (void)memset(&request, 0xA5, sizeof(request));
    sentinel = request;
    ASSERT_TRUE(ucn_cluster_takeover_persist_vote_request_build(
                    &decoded, &transaction, 61U, &request) == UCN_ERR_STATE &&
                memcmp(&request, &sentinel, sizeof(request)) == 0);

    /* R33: a v2 partial vote from an older Epoch is read-only historical
     * evidence, not a permanent ban on the first full v3 M10 Vote. */
    state.last_vote.epoch.term = 8U;
    ASSERT_TRUE(test_encode_legacy_v2_fixture(&state, 8U, record) == UCN_OK &&
                ucn_cluster_persist_record_decode(record, sizeof(record),
                                                  &generation, &decoded) == UCN_OK &&
                decoded.record_schema_version ==
                    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V2 &&
                decoded.last_vote.epoch.term == 8U);
    (void)memset(&request, 0, sizeof(request));
    ASSERT_TRUE(ucn_cluster_takeover_persist_vote_request_build(
                    &decoded, &transaction, 62U, &request) == UCN_OK &&
                request.next_state.record_schema_version ==
                    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3 &&
                ucn_cluster_persist_request_admit(&decoded, &request) ==
                    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    return 0;
}

static int test_provider_owner_pending_reentry_and_failure(void)
{
    ucn_cluster_takeover_transaction_t transaction;
    ucn_cluster_backup_sync_owner_t backup_owner;
    ucn_cluster_takeover_persist_owner_t owner;
    ucn_cluster_persist_provider_t provider;
    takeover_persist_fake_t fake;
    bool committed = true;

    ASSERT_TRUE(begin_transaction(&transaction, &backup_owner, false, 100U));
    (void)memset(&fake, 0, sizeof(fake));
    make_persist_state_from_transaction(&fake.stored, &transaction);
    takeover_fake_provider(&provider, &fake);
    fake.reentry_owner = &owner;
    fake.reentry_transaction = &transaction;
    fake.reenter_load = true;
    (void)memset(&owner, 0xA5, sizeof(owner));
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_init(&owner, &provider) == UCN_OK);
    ASSERT_TRUE(fake.reentry_result == UCN_ERR_STATE);

    fake.reenter_load = false;
    fake.reenter_submit = true;
    fake.pending_polls = 2U;
    committed = true;
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_begin_vote(
                    &owner, &transaction, 41U, &committed) == UCN_OK && !committed &&
                owner.pending && fake.reentry_result == UCN_ERR_STATE &&
                !ucn_cluster_takeover_persist_owner_vote_is_durable(&owner,
                                                                     &transaction));
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_step(&owner, &transaction) ==
                UCN_OK && owner.pending);
    fake.reenter_submit = false;
    fake.reenter_poll = true;
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_step(&owner, &transaction) ==
                UCN_OK && !owner.pending && fake.reentry_result == UCN_ERR_STATE &&
                ucn_cluster_takeover_persist_owner_vote_is_durable(&owner,
                                                                    &transaction));
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_apply_durable_vote(
                    &owner, &transaction) == UCN_OK &&
                note_remote_vote(&transaction, &transaction.vote_id, 3U) == UCN_OK);
    committed = false;
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_begin_epoch(
                    &owner, &transaction, 42U, &committed) == UCN_OK && committed &&
                ucn_cluster_takeover_persist_owner_epoch_is_durable(&owner,
                                                                     &transaction));
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_apply_durable_epoch(
                    &owner, &transaction) == UCN_OK);

    ASSERT_TRUE(begin_transaction(&transaction, &backup_owner, false, 200U));
    (void)memset(&fake, 0, sizeof(fake));
    make_persist_state_from_transaction(&fake.stored, &transaction);
    takeover_fake_provider(&provider, &fake);
    (void)memset(&owner, 0, sizeof(owner));
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_init(&owner, &provider) == UCN_OK);
    fake.pending_polls = 1U;
    committed = true;
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_begin_vote(
                    &owner, &transaction, 51U, &committed) == UCN_OK && !committed);
    fake.fail_poll = true;
    ASSERT_TRUE(ucn_cluster_takeover_persist_owner_step(&owner, &transaction) ==
                UCN_ERR_STATE && owner.faulted && owner.persistence_failures == 1U &&
                !ucn_cluster_takeover_persist_owner_vote_is_durable(&owner,
                                                                     &transaction));
    return 0;
}

static int test_staging_and_frozen_quorum_property(void)
{
    ucn_node_id_t voters[UCN_CLUSTER_MAX_VOTERS];
    size_t count;

    for (count = 1U; count <= UCN_CLUSTER_MAX_VOTERS; ++count) {
        ucn_cluster_takeover_transaction_t transaction;
        ucn_cluster_backup_sync_owner_t owner;
        ucn_cluster_backup_epoch_t epoch;
        ucn_cluster_config_state_t config;
        ucn_cluster_snapshot_epoch_t snapshot;
        ucn_cluster_takeover_certificate_t certificate;
        size_t index;
        uint8_t quorum;

        voters[0U] = 2U;
        for (index = 1U; index < count; ++index) {
            voters[index] = (ucn_node_id_t)(index + 2U);
        }
        (void)memset(&owner, 0, sizeof(owner));
        ASSERT_TRUE(make_epoch(&epoch));
        ASSERT_TRUE(ucn_cluster_config_state_init_stable(&config, 6U, voters,
                                                         count));
        ASSERT_TRUE(ucn_cluster_backup_sync_owner_init(&owner, &epoch, &config) ==
                    UCN_OK);
        ASSERT_TRUE(ucn_cluster_snapshot_epoch_from_config(&snapshot, &epoch,
                                                           7U, &config));
        owner.mirror.committed_epoch = snapshot;
        owner.mirror.committed_valid = true;
        (void)memset(&transaction, 0, sizeof(transaction));
        ASSERT_TRUE(ucn_cluster_takeover_transaction_begin(
                        &transaction, &owner, 1U, 1U, 100U) == UCN_OK);
        ASSERT_TRUE(ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
                        &transaction, &transaction.vote_id) == UCN_OK);
        quorum = ucn_cluster_voter_set_quorum(&config.old_set);
        for (index = 1U; index < (size_t)quorum; ++index) {
            ASSERT_TRUE(note_remote_vote(&transaction, &transaction.vote_id,
                                         voters[index]) == UCN_OK);
        }
        ASSERT_TRUE(ucn_cluster_takeover_transaction_quorum_reached(&transaction));
        ASSERT_TRUE(ucn_cluster_takeover_certificate_build(&transaction,
                                                            &certificate) == UCN_OK);
        if (count < 32U) {
            certificate.old_vote_words[0U] |= UINT32_C(1) << count;
            ASSERT_TRUE(!ucn_cluster_takeover_certificate_is_valid(&transaction,
                                                                    &certificate));
        }
    }

    {
        ucn_cluster_takeover_transaction_t transaction;
        ucn_cluster_takeover_transaction_t before;
        ucn_cluster_backup_sync_owner_t owner;

        (void)memset(&owner, 0, sizeof(owner));
        ASSERT_TRUE(make_committed_owner(&owner, false));
        ASSERT_TRUE(make_joint_config(&owner.active_config));
        (void)memset(&transaction, 0xA5, sizeof(transaction));
        before = transaction;
        ASSERT_TRUE(ucn_cluster_takeover_transaction_begin(
                        &transaction, &owner, 88U, 1U, 100U) == UCN_ERR_STATE);
        ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    }
    return 0;
}

int main(void)
{
    if (test_freeze_and_member_vote_gate() != 0 ||
        test_stable_durable_quorum_certificate_and_fence() != 0 ||
        test_joint_double_quorum_and_fragment_binding() != 0 ||
        test_impossible_timeout_and_durable_epoch() != 0 ||
        test_persisted_full_vote_and_epoch_barriers() != 0 ||
        test_legacy_v2_partial_vote_never_becomes_m10_proof() != 0 ||
        test_provider_owner_pending_reentry_and_failure() != 0 ||
        test_staging_and_frozen_quorum_property() != 0) {
        return 1;
    }
    (void)puts("UCN cluster takeover tests passed");
    return 0;
}
