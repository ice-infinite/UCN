#include <string.h>

#include "test_support.h"
#include "ucn/ucn_cluster_persist.h"

/* M04 runtime self-audit reaches these internal continuations directly so the
 * public tests can prove their pre-promise boundary without manufacturing a
 * large multi-node topology for each failure cut point. */
#include "../src/extended/cluster/ucn_cluster_internal.h"

enum {
    TEST_CRC_OFFSET = 12U,
    TEST_ACTIVE_EPOCH_CLUSTER_OFFSET = 17U,
    TEST_ACTIVE_EPOCH_TERM_OFFSET = 21U,
    TEST_CONFIG_GENERATION_OFFSET = 68U,
    TEST_BOOT_INCARNATION_OFFSET = 260U,
    TEST_LAST_OPERATION_FINGERPRINT_OFFSET = 269U
};

typedef struct cluster_persist_fake {
    ucn_cluster_persist_load_result_t stored;
    bool submit_pending;
    bool submit_fails;
    bool poll_fails;
    bool commit_not_visible;
    bool pending;
    uint8_t pending_polls_before_terminal;
    ucn_cluster_persist_token_t token;
    ucn_cluster_persist_request_t pending_request;
    /* External Provider callbacks may be synchronous/reentrant.  These test
     * controls execute public Cluster step/RX while load/submit/poll is still
     * on the stack, proving the I/O gate is installed before the callback. */
    ucn_cluster_t *reentry_cluster;
    bool reenter_on_load;
    bool reenter_on_submit;
    bool reenter_on_poll;
    bool reenter_via_receive;
    ucn_result_t reentry_result;
    uint32_t reentry_calls;
} cluster_persist_fake_t;

static void persist_fake_maybe_reenter(cluster_persist_fake_t *fake,
                                       bool *enabled)
{
    if (fake != NULL && enabled != NULL && *enabled &&
        fake->reentry_cluster != NULL) {
        *enabled = false;
        fake->reentry_calls++;
        if (fake->reenter_via_receive) {
            /* The persistence RX gate must run before peer lookup or wire
             * parsing, therefore no artificial valid message is required to
             * prove that a Provider callback cannot inject control traffic. */
            fake->reentry_result = ucn_cluster_receive(
                fake->reentry_cluster, 2U, true, NULL, 0U);
        } else {
            fake->reentry_result = ucn_cluster_step(fake->reentry_cluster);
        }
    }
}

static ucn_cluster_persist_completion_t persist_completion(
    ucn_cluster_persist_completion_state_t state,
    ucn_cluster_persist_token_t token,
    ucn_result_t failure)
{
    ucn_cluster_persist_completion_t completion;

    completion.state = state;
    completion.token = token;
    completion.failure = failure;
    return completion;
}

static void persist_fake_set_factory_empty(cluster_persist_fake_t *fake)
{
    (void)memset(fake, 0, sizeof(*fake));
    fake->stored.state = UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY;
}

static ucn_result_t cluster_persist_fake_load(
    void *context,
    ucn_cluster_persist_load_result_t *result)
{
    cluster_persist_fake_t *fake = (cluster_persist_fake_t *)context;

    if (fake == NULL || result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    persist_fake_maybe_reenter(fake, &fake->reenter_on_load);
    *result = fake->stored;
    return UCN_OK;
}

static ucn_cluster_persist_completion_t cluster_persist_fake_submit(
    void *context,
    const ucn_cluster_persist_request_t *request)
{
    cluster_persist_fake_t *fake = (cluster_persist_fake_t *)context;
    ucn_cluster_persist_request_admission_t admission;
    ucn_cluster_persist_state_t factory_empty_state;

    if (fake == NULL || !ucn_cluster_persist_request_is_valid(request)) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_ARGUMENT);
    }
    if (fake->submit_fails) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_STATE);
    }
    if (fake->stored.state == UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY) {
        ucn_cluster_persist_state_init_empty(&factory_empty_state);
        admission = ucn_cluster_persist_request_admit(&factory_empty_state,
                                                       request);
    } else if (fake->stored.state == UCN_CLUSTER_PERSIST_LOAD_READY) {
        admission = ucn_cluster_persist_request_admit(&fake->stored.snapshot,
                                                       request);
    } else {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_CONFIG);
    }
    if (admission == UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_INVALID) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_CONFIG);
    }
    if (admission == UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_REPLAY);
    }
    if (admission == UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_IDEMPOTENT) {
        return persist_completion(UCN_CLUSTER_PERSIST_COMMITTED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE, UCN_OK);
    }
    if (fake->submit_pending) {
        if (fake->pending) {
            return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                      UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                      UCN_ERR_NO_SPACE);
        }
        /* Provider-side copy is essential: caller storage may be reused while
         * Flash/DMA is in flight. */
        fake->pending_request = *request;
        fake->pending = true;
        fake->token++;
        if (fake->token == UCN_CLUSTER_PERSIST_TOKEN_NONE) {
            fake->token++;
        }
        persist_fake_maybe_reenter(fake, &fake->reenter_on_submit);
        return persist_completion(UCN_CLUSTER_PERSIST_PENDING, fake->token,
                                   UCN_OK);
    }
    if (!fake->commit_not_visible) {
        fake->stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
        fake->stored.snapshot = request->next_state;
    }
    return persist_completion(UCN_CLUSTER_PERSIST_COMMITTED,
                              UCN_CLUSTER_PERSIST_TOKEN_NONE, UCN_OK);
}

static ucn_cluster_persist_completion_t cluster_persist_fake_poll(
    void *context,
    ucn_cluster_persist_token_t token)
{
    cluster_persist_fake_t *fake = (cluster_persist_fake_t *)context;

    if (fake == NULL || !fake->pending || token == UCN_CLUSTER_PERSIST_TOKEN_NONE ||
        token != fake->token) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_ARGUMENT);
    }
    persist_fake_maybe_reenter(fake, &fake->reenter_on_poll);
    if (fake->pending_polls_before_terminal != 0U) {
        fake->pending_polls_before_terminal--;
        return persist_completion(UCN_CLUSTER_PERSIST_PENDING, token, UCN_OK);
    }
    fake->pending = false;
    if (fake->poll_fails) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_STATE);
    }
    fake->stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    fake->stored.snapshot = fake->pending_request.next_state;
    return persist_completion(UCN_CLUSTER_PERSIST_COMMITTED,
                              UCN_CLUSTER_PERSIST_TOKEN_NONE, UCN_OK);
}

static void persist_state_set_epoch(ucn_cluster_persist_state_t *state,
                                    uint32_t term)
{
    ucn_cluster_persist_state_init_empty(state);
    state->has_active_epoch = true;
    state->active_epoch.cluster_id = 1U;
    state->active_epoch.term = term;
    state->active_epoch.head_node_id = 7U;
    state->has_max_epoch = true;
    state->max_epoch = state->active_epoch;
    state->boot_incarnation = 9U;
}

static ucn_cluster_persist_request_t persist_request_from_state(
    uint32_t operation_id, ucn_cluster_persist_operation_t operation,
    const ucn_cluster_persist_state_t *next_state);

static ucn_cluster_persist_request_t persist_epoch_request(
    uint32_t operation_id,
    uint32_t term)
{
    ucn_cluster_persist_request_t request;

    (void)memset(&request, 0, sizeof(request));
    request.operation_id = operation_id;
    request.operation = UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT;
    persist_state_set_epoch(&request.next_state, term);
    (void)ucn_cluster_persist_request_finalize(&request);
    return request;
}

static ucn_cluster_persist_request_t persist_replay_incarnation_request(
    uint32_t operation_id, const ucn_cluster_persist_state_t *committed,
    uint32_t boot_incarnation)
{
    ucn_cluster_persist_state_t next = *committed;

    next.boot_incarnation = boot_incarnation;
    /* Any accepted next write upgrades a decoded v1/v2 record. */
    next.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    return persist_request_from_state(
        operation_id, UCN_CLUSTER_PERSIST_OPERATION_REPLAY_INCARNATION, &next);
}

static void persist_state_clear_cluster_scoped_state(
    ucn_cluster_persist_state_t *state)
{
    (void)memset(&state->last_vote, 0, sizeof(state->last_vote));
    (void)memset(&state->committed_config, 0, sizeof(state->committed_config));
    (void)memset(&state->config_transaction, 0,
                 sizeof(state->config_transaction));
    state->config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    (void)memset(&state->committed_rekey, 0, sizeof(state->committed_rekey));
    (void)memset(&state->rekey_transaction, 0,
                 sizeof(state->rekey_transaction));
    state->rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    (void)memset(&state->tombstone, 0, sizeof(state->tombstone));
}

static ucn_cluster_persist_request_t persist_cluster_create_request(
    uint32_t operation_id, const ucn_cluster_persist_state_t *committed,
    uint32_t cluster_id, ucn_node_id_t head_node_id)
{
    ucn_cluster_persist_state_t next = *committed;

    persist_state_clear_cluster_scoped_state(&next);
    next.has_active_epoch = true;
    next.active_epoch.cluster_id = cluster_id;
    next.active_epoch.term = 1U;
    next.active_epoch.head_node_id = head_node_id;
    next.has_max_epoch = true;
    next.max_epoch = next.active_epoch;
    return persist_request_from_state(
        operation_id, UCN_CLUSTER_PERSIST_OPERATION_CLUSTER_CREATE_COMMIT,
        &next);
}

static ucn_cluster_persist_request_t persist_epoch_request_from_state(
    uint32_t operation_id, const ucn_cluster_persist_state_t *committed,
    uint32_t term)
{
    ucn_cluster_persist_state_t next = *committed;

    next.active_epoch.term = term;
    next.max_epoch.term = term;
    return persist_request_from_state(
        operation_id, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
}

static ucn_cluster_persist_request_t persist_request_from_state(
    uint32_t operation_id, ucn_cluster_persist_operation_t operation,
    const ucn_cluster_persist_state_t *next_state)
{
    ucn_cluster_persist_request_t request;

    (void)memset(&request, 0, sizeof(request));
    request.operation_id = operation_id;
    request.operation = operation;
    request.next_state = *next_state;
    (void)ucn_cluster_persist_request_finalize(&request);
    return request;
}

static void test_write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void test_write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static uint32_t test_crc32_record(const uint8_t *record, size_t record_length)
{
    uint32_t crc = UINT32_MAX;
    size_t index;

    for (index = 0U; index < record_length; ++index) {
        uint8_t value = index >= TEST_CRC_OFFSET &&
                                index < TEST_CRC_OFFSET + 4U ?
                            0U : record[index];
        uint8_t bit;

        crc ^= value;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ?
                      (crc >> 1U) ^ UINT32_C(0xEDB88320) : crc >> 1U;
        }
    }
    return crc ^ UINT32_MAX;
}

static void test_refresh_record_crc(uint8_t *record, size_t record_length)
{
    test_write_u32_be(record + TEST_CRC_OFFSET,
                      test_crc32_record(record, record_length));
}

/* The public writer never emits v1. This fixture emulates a verified v1
 * record found after an upgrade by rewriting an otherwise canonical v3
 * payload and refreshing its test CRC. No production code calls this helper. */
static ucn_result_t persist_encode_legacy_v1_fixture(
    const ucn_cluster_persist_state_t *legacy_state,
    uint32_t generation,
    uint8_t *record,
    size_t record_capacity)
{
    uint8_t writer_record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    ucn_cluster_persist_state_t writer_state;
    ucn_result_t result;

    if (legacy_state == NULL || record == NULL ||
        record_capacity < UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    writer_state = *legacy_state;
    writer_state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    result = ucn_cluster_persist_record_encode(&writer_state, generation,
                                                writer_record,
                                                sizeof(writer_record));
    if (result != UCN_OK) {
        return result;
    }
    (void)memcpy(record, writer_record,
                 UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES);
    test_write_u16_be(record + 4U,
                      UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1);
    test_write_u16_be(record + 6U,
                      (uint16_t)UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES);
    test_refresh_record_crc(record, UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES);
    return UCN_OK;
}

static void persist_config_ref_set(ucn_cluster_persist_config_ref_t *config,
                                   uint32_t config_id, uint32_t generation,
                                   uint8_t digest0)
{
    (void)memset(config, 0, sizeof(*config));
    config->valid = true;
    config->config_id = config_id;
    config->generation = generation;
    config->digest[0] = digest0;
}

static void persist_rekey_ref_set(ucn_cluster_persist_rekey_ref_t *rekey,
                                  uint32_t generation,
                                  uint32_t incarnation,
                                  const ucn_cluster_epoch_t *predecessor,
                                  const ucn_cluster_persist_config_ref_t *
                                      predecessor_config,
                                  const ucn_cluster_epoch_t *successor)
{
    (void)memset(rekey, 0, sizeof(*rekey));
    rekey->valid = true;
    rekey->generation = generation;
    rekey->next_incarnation = incarnation;
    rekey->prepare_nonce = 1U;
    rekey->allocation_history_fingerprint = UINT32_C(0x13579BDF);
    rekey->predecessor_epoch = *predecessor;
    rekey->predecessor_config = *predecessor_config;
    rekey->successor_epoch = *successor;
    persist_config_ref_set(&rekey->successor_config, 1U, 1U, 0xB1U);
}

/* These builders represent an already-decoded pre-R20 Record-v1 snapshot.
 * They are intentionally logical fixtures, not writer paths: production
 * record_encode() only emits schema v4. Legacy v1 bytes are created below by
 * a test-only header rewrite plus CRC refresh. */
static ucn_result_t persist_legacy_config_prepared_state(
    ucn_cluster_persist_state_t *state)
{
    if (state == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    persist_state_set_epoch(state, 5U);
    state->active_epoch.head_node_id = 1U;
    state->max_epoch.head_node_id = 1U;
    persist_config_ref_set(&state->committed_config, 1U, 1U, 0xA1U);
    state->record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1;
    state->config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    state->config_transaction.transaction_id = 1U;
    persist_config_ref_set(&state->config_transaction.staging_config, 2U, 2U,
                           0xA2U);
    return ucn_cluster_persist_state_is_valid(state) ? UCN_OK : UCN_ERR_STATE;
}

static ucn_result_t persist_legacy_rekey_prepared_state(
    ucn_cluster_persist_state_t *state)
{
    ucn_cluster_epoch_t successor;

    if (state == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    persist_state_set_epoch(state, 5U);
    state->active_epoch.head_node_id = 1U;
    state->max_epoch.head_node_id = 1U;
    persist_config_ref_set(&state->committed_config, 1U, 1U, 0xA1U);
    successor.cluster_id = 2U;
    successor.term = 1U;
    successor.head_node_id = 1U;
    state->record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1;
    state->rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    state->rekey_transaction.transaction_id = 2U;
    persist_rekey_ref_set(&state->rekey_transaction.staging_rekey, 1U, 10U,
                          &state->active_epoch, &state->committed_config,
                          &successor);
    return ucn_cluster_persist_state_is_valid(state) ? UCN_OK : UCN_ERR_STATE;
}

static int cluster_persist_test_contract_guards(
    const ucn_cluster_persist_provider_t *async_provider,
    const ucn_cluster_persist_provider_t *sync_provider)
{
    ucn_cluster_persist_completion_t completion;
    ucn_cluster_persist_load_result_t loaded;

    (void)memset(&completion, 0, sizeof(completion));
    (void)memset(&loaded, 0, sizeof(loaded));
    TEST_ASSERT(!ucn_cluster_persist_completion_is_valid(&completion));
    TEST_ASSERT(!ucn_cluster_persist_load_result_is_valid(&loaded));

    (void)memset(&loaded, 0, sizeof(loaded));
    loaded.state = UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY;
    TEST_ASSERT(ucn_cluster_persist_load_result_is_valid(&loaded));
    loaded.snapshot.boot_incarnation = 1U;
    TEST_ASSERT(!ucn_cluster_persist_load_result_is_valid(&loaded));

    ucn_cluster_persist_state_init_empty(&loaded.snapshot);
    loaded.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    TEST_ASSERT(ucn_cluster_persist_load_result_is_valid(&loaded));

    completion = persist_completion(UCN_CLUSTER_PERSIST_PENDING, 1U, UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_completion_is_valid(&completion));
    TEST_ASSERT(ucn_cluster_persist_provider_accepts_completion(
                    async_provider, &completion));
    TEST_ASSERT(!ucn_cluster_persist_provider_accepts_completion(
                    sync_provider, &completion));
    return 0;
}

static int cluster_persist_test_record_codec(void)
{
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t canonical[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t dirty[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t erased_zero[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t erased_ff[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    ucn_cluster_persist_state_t state;
    ucn_cluster_persist_state_t decoded;
    ucn_cluster_persist_request_t request;
    ucn_cluster_epoch_t successor;
    uint32_t generation = 0U;
    uint32_t next_generation = 0U;

    persist_state_set_epoch(&state, 2U);
    state.last_vote.valid = true;
    state.last_vote.epoch = state.active_epoch;
    state.last_vote.voted_for_node_id = 8U;
    state.last_vote.backup_generation = 6U;
    persist_config_ref_set(&state.committed_config, 12U, 3U, 0xA5U);
    state.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    state.config_transaction.transaction_id = 31U;
    persist_config_ref_set(&state.config_transaction.staging_config, 13U, 4U,
                           0xA6U);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 7U, record, sizeof(record)) == UCN_OK);
    TEST_ASSERT(record[4U] == 0U && record[5U] == 4U);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) == UCN_OK);
    TEST_ASSERT(generation == 7U &&
                decoded.record_schema_version ==
                    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
                decoded.has_active_epoch &&
                decoded.active_epoch.term == 2U && decoded.last_vote.valid &&
                decoded.last_vote.voted_for_node_id == 8U &&
                decoded.last_vote.backup_generation == 6U &&
                decoded.committed_config.valid &&
                decoded.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
                decoded.config_transaction.transaction_id == 31U &&
                decoded.config_transaction.staging_config.config_id == 13U &&
                decoded.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
                !decoded.tombstone.valid);

    /* Schema v1 remains a read-only legacy input. The exact same PREPARED
     * payload is decoded with v1 provenance so R23 can migrate it once; the
      * normal writer above always emits v3. */
    state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 71U, canonical, sizeof(canonical)) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(persist_encode_legacy_v1_fixture(
                    &state, 71U, record, sizeof(record)) == UCN_OK &&
                record[4U] == 0U && record[5U] == 1U);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES,
                    &generation, &decoded) == UCN_OK &&
                generation == 71U &&
                decoded.record_schema_version ==
                    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1 &&
                decoded.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED);
    state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;

    /* A terminal transaction retains its exact C_new reference as well as its
     * txid.  This binds CONFIG_ABORT replay after reset to the proposal that
     * was actually staged. */
    state.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 8U, record, sizeof(record)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) == UCN_OK);
    TEST_ASSERT(decoded.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
                decoded.config_transaction.transaction_id == 31U &&
                decoded.config_transaction.staging_config.valid &&
                decoded.config_transaction.staging_config.config_id == 13U);

    /* Rekey PREPARE carries both the old authority/config binding and the
     * successor identity.  It must be independently recoverable; Config and
     * Rekey PREPARE may not coexist. */
    persist_state_set_epoch(&state, 2U);
    persist_config_ref_set(&state.committed_config, 12U, 3U, 0xA5U);
    successor.cluster_id = 2U;
    successor.term = 1U;
    successor.head_node_id = 9U;
    state.rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    state.rekey_transaction.transaction_id = 32U;
    persist_rekey_ref_set(&state.rekey_transaction.staging_rekey, 5U, 10U,
                          &state.active_epoch, &state.committed_config,
                          &successor);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) == UCN_OK);
    TEST_ASSERT(decoded.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
                decoded.rekey_transaction.transaction_id == 32U &&
                decoded.rekey_transaction.staging_rekey.predecessor_config
                        .config_id == 12U &&
                decoded.rekey_transaction.staging_rekey.successor_epoch
                        .cluster_id == 2U &&
                decoded.rekey_transaction.staging_rekey.successor_epoch.term ==
                    1U &&
                decoded.rekey_transaction.staging_rekey.successor_config
                        .config_id == 1U &&
                decoded.rekey_transaction.staging_rekey.
                        allocation_history_fingerprint ==
                    UINT32_C(0x13579BDF));

    /* A Rekey COMMIT atomically records the successor Epoch, committed Rekey
     * identity and the tombstone that retires the predecessor. */
    state.active_epoch = successor;
    state.max_epoch = successor;
    state.committed_rekey = state.rekey_transaction.staging_rekey;
    state.committed_config = state.committed_rekey.successor_config;
    state.boot_incarnation = state.committed_rekey.next_incarnation;
    state.rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    (void)memset(&state.rekey_transaction.staging_rekey, 0,
                 sizeof(state.rekey_transaction.staging_rekey));
    state.tombstone.valid = true;
    state.tombstone.retired_epoch = state.committed_rekey.predecessor_epoch;
    state.tombstone.replacement_cluster_id = successor.cluster_id;
    state.tombstone.rekey_transaction_id = 32U;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 10U, record, sizeof(record)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) == UCN_OK);
    TEST_ASSERT(decoded.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
                decoded.rekey_transaction.transaction_id == 32U &&
                decoded.committed_rekey.successor_epoch.cluster_id == 2U &&
                decoded.committed_rekey.allocation_history_fingerprint ==
                    UINT32_C(0x13579BDF) &&
                decoded.tombstone.rekey_transaction_id == 32U &&
                decoded.tombstone.retired_epoch.cluster_id == 1U);

    /* Dirty values behind absent semantic flags must have exactly the same
     * physical record as the clean state. */
    persist_state_set_epoch(&state, 2U);
    state.has_active_epoch = false;
    state.has_max_epoch = false;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, canonical, sizeof(canonical)) == UCN_OK);
    state.active_epoch.cluster_id = 99U;
    state.active_epoch.term = 99U;
    state.active_epoch.head_node_id = 99U;
    state.committed_config.config_id = 99U;
    state.config_transaction.transaction_id = 99U;
    state.rekey_transaction.transaction_id = 99U;
    state.tombstone.retired_epoch.cluster_id = 99U;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, dirty, sizeof(dirty)) == UCN_OK);
    TEST_ASSERT(memcmp(canonical, dirty, sizeof(canonical)) == 0);
    dirty[TEST_ACTIVE_EPOCH_CLUSTER_OFFSET] = 1U;
    test_refresh_record_crc(dirty, sizeof(dirty));
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    dirty, sizeof(dirty), &generation, &decoded) ==
                UCN_ERR_MALFORMED);

    persist_state_set_epoch(&state, 2U);
    state.active_epoch.cluster_id = UCN_NODE_BROADCAST;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_ERR_CONFIG);
    persist_state_set_epoch(&state, UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_ERR_CONFIG);
    persist_state_set_epoch(&state, 2U);
    persist_config_ref_set(&state.committed_config, 12U,
                           UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U, 0U);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_ERR_CONFIG);
    persist_state_set_epoch(&state, 2U);
    state.last_vote.valid = true;
    state.last_vote.epoch = state.active_epoch;
    state.last_vote.voted_for_node_id = 8U;
    state.last_vote.backup_generation =
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_ERR_CONFIG);
    persist_state_set_epoch(&state, 2U);
    persist_config_ref_set(&state.committed_config, 12U, 3U, 0U);
    successor.cluster_id = 2U;
    successor.term = 1U;
    successor.head_node_id = 8U;
    persist_rekey_ref_set(&state.committed_rekey, 1U,
                          UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U,
                          &state.active_epoch, &state.committed_config,
                          &successor);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_ERR_CONFIG);
    persist_state_set_epoch(&state, 2U);
    persist_config_ref_set(&state.committed_config, 12U, 3U, 0U);
    successor.cluster_id = state.active_epoch.cluster_id;
    successor.term = 1U;
    successor.head_node_id = 8U;
    persist_rekey_ref_set(&state.committed_rekey, 1U, 2U,
                          &state.active_epoch, &state.committed_config,
                          &successor);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_ERR_CONFIG);
    persist_state_set_epoch(&state, 2U);
    state.boot_incarnation = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_ERR_CONFIG);

    /* Decode performs the same domain validation after a valid CRC, not only
     * the encode-side validation above. */
    persist_state_set_epoch(&state, 2U);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_OK);
    test_write_u32_be(record + TEST_ACTIVE_EPOCH_CLUSTER_OFFSET,
                      UCN_NODE_BROADCAST);
    test_refresh_record_crc(record, sizeof(record));
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) ==
                UCN_ERR_MALFORMED);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_OK);
    test_write_u32_be(record + TEST_ACTIVE_EPOCH_TERM_OFFSET,
                      UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U);
    test_refresh_record_crc(record, sizeof(record));
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) ==
                UCN_ERR_MALFORMED);
    persist_config_ref_set(&state.committed_config, 12U, 3U, 0U);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_OK);
    test_write_u32_be(record + TEST_CONFIG_GENERATION_OFFSET,
                      UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U);
    test_refresh_record_crc(record, sizeof(record));
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) ==
                UCN_ERR_MALFORMED);
    persist_state_set_epoch(&state, 2U);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 9U, record, sizeof(record)) == UCN_OK);
    test_write_u32_be(record + TEST_BOOT_INCARNATION_OFFSET,
                      UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U);
    test_refresh_record_crc(record, sizeof(record));
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) ==
                UCN_ERR_MALFORMED);
    request = persist_epoch_request(51U, 2U);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &request.next_state, 10U, record, sizeof(record)) == UCN_OK);
    record[TEST_LAST_OPERATION_FINGERPRINT_OFFSET + 3U] ^= 1U;
    test_refresh_record_crc(record, sizeof(record));
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) ==
                UCN_ERR_MALFORMED);

    TEST_ASSERT(ucn_cluster_persist_record_generation_next(0U,
                                                            &next_generation) == UCN_OK);
    TEST_ASSERT(next_generation == 1U &&
                ucn_cluster_persist_record_generation_is_newer(7U, 6U) &&
                !ucn_cluster_persist_record_generation_is_newer(6U, 7U) &&
                ucn_cluster_persist_record_generation_next(
                    UINT32_MAX, &next_generation) == UCN_ERR_EXHAUSTED);
    (void)memset(erased_zero, 0, sizeof(erased_zero));
    (void)memset(erased_ff, 0xFF, sizeof(erased_ff));
    TEST_ASSERT(ucn_cluster_persist_record_is_factory_empty(
                    erased_zero, sizeof(erased_zero)));
    TEST_ASSERT(ucn_cluster_persist_record_is_factory_empty(
                    erased_ff, sizeof(erased_ff)));
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    erased_zero, sizeof(erased_zero), &generation,
                    &decoded) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    erased_ff, sizeof(erased_ff), &generation,
                    &decoded) == UCN_ERR_NOT_FOUND);
    return 0;
}

static int cluster_persist_test_operation_admission(void)
{
    ucn_cluster_persist_state_t empty_state;
    ucn_cluster_persist_request_t replay;
    ucn_cluster_persist_request_t first;
    ucn_cluster_persist_request_t conflict;
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_state_t prepared;
    ucn_cluster_persist_state_t committed;
    ucn_cluster_persist_state_t next;
    ucn_cluster_persist_state_t detached;
    ucn_cluster_persist_config_ref_t config_a;
    ucn_cluster_persist_config_ref_t config_b;
    ucn_cluster_persist_config_ref_t config_c;
    ucn_cluster_persist_config_ref_t config_d;
    ucn_cluster_epoch_t successor;
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint32_t record_generation;
    ucn_cluster_persist_state_t reloaded;

    ucn_cluster_persist_state_init_empty(&empty_state);
    /* A factory-empty store has no usable replay domain.  It must first
     * durably establish boot incarnation, then create the first Cluster at
     * Term 1.  A normal EPOCH_COMMIT is deliberately not a shortcut. */
    request = persist_epoch_request(1U, 2U);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&empty_state, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    replay = persist_replay_incarnation_request(1U, &empty_state, 9U);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&replay));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&empty_state, &replay) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    first = persist_cluster_create_request(2U, &replay.next_state, 1U, 7U);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&first));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&replay.next_state, &first) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    TEST_ASSERT(ucn_cluster_persist_request_admit(
                    &first.next_state, &first) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_IDEMPOTENT);
    conflict = first;
    conflict.next_state.active_epoch.term = 2U;
    conflict.next_state.max_epoch.term = 2U;
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&conflict) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_request_admit(
                    &first.next_state, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    conflict = first;
    conflict.next_state.active_epoch.term =
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    conflict.next_state.max_epoch.term =
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    TEST_ASSERT(!ucn_cluster_persist_request_is_valid(&conflict));
    TEST_ASSERT(ucn_cluster_persist_request_admit(
                    &first.next_state, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_INVALID);

    /* A normal EPOCH_COMMIT is a same-identity, exact-next-Term promise.  It
     * cannot overwrite the durable max Epoch with an old Term or a new
     * Cluster identity. */
    persist_state_set_epoch(&committed, 5U);
    next = committed;
    next.active_epoch.term = 2U;
    next.max_epoch.term = 2U;
    request = persist_request_from_state(
        1U, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.active_epoch.cluster_id = 2U;
    next.max_epoch.cluster_id = 2U;
    next.active_epoch.term = 1U;
    next.max_epoch.term = 1U;
    request = persist_request_from_state(
        1U, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.active_epoch.head_node_id = 8U;
    next.max_epoch.head_node_id = 8U;
    request = persist_request_from_state(
        1U, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.active_epoch.term = 7U;
    next.max_epoch.term = 7U;
    request = persist_request_from_state(
        1U, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.active_epoch.term = 6U;
    TEST_ASSERT(!ucn_cluster_persist_state_is_valid(&next));
    next = committed;
    next.active_epoch.term = 6U;
    next.max_epoch.term = 6U;
    request = persist_request_from_state(
        1U, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);

    /* A detached/recovering node creates a distinct Cluster only through the
     * explicit Term-1 transition.  The parent ID cannot be reused, and a
     * rebooted record retains enough state to create another distinct identity
     * rather than falling back to EPOCH_COMMIT's same-Cluster path. */
    detached = committed;
    detached.has_active_epoch = false;
    (void)memset(&detached.active_epoch, 0, sizeof(detached.active_epoch));
    TEST_ASSERT(ucn_cluster_persist_state_is_valid(&detached));
    request = persist_cluster_create_request(2U, &detached, 2U, 8U);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&detached, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &request.next_state, 1U, record, sizeof(record)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &record_generation, &reloaded) ==
                UCN_OK);
    conflict = persist_cluster_create_request(3U, &reloaded, 2U, 9U);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&reloaded, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    conflict = persist_cluster_create_request(3U, &reloaded, 3U, 9U);
    conflict.next_state.active_epoch.term = 2U;
    conflict.next_state.max_epoch.term = 2U;
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&conflict) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&reloaded, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    request = persist_cluster_create_request(3U, &reloaded, 3U, 9U);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&reloaded, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);

    /* An incarnation change is never piggy-backed on a Cluster transition.
     * Replay is its sole monotonic write path, and it must not alter Epoch. */
    next = committed;
    next.active_epoch.term = 6U;
    next.max_epoch.term = 6U;
    next.boot_incarnation = 1U;
    request = persist_request_from_state(
        2U, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.active_epoch.term = 6U;
    next.max_epoch.term = 6U;
    next.boot_incarnation = 10U;
    request = persist_request_from_state(
        2U, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    request = persist_replay_incarnation_request(2U, &committed, 10U);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    next = committed;
    next.boot_incarnation = 10U;
    next.active_epoch.term = 6U;
    next.max_epoch.term = 6U;
    request = persist_request_from_state(
        2U, UCN_CLUSTER_PERSIST_OPERATION_REPLAY_INCARNATION, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);

    /* A Config COMMIT is legal only for the exact staging reference and txid
     * that were durably PREPARED.  This is the regression for Prepare-A /
     * Commit-B and txid-conflict bypasses. */
    persist_config_ref_set(&config_a, 10U, 1U, 0xA1U);
    persist_config_ref_set(&config_b, 11U, 2U, 0xB1U);
    persist_config_ref_set(&config_c, 12U, 3U, 0xC1U);
    persist_config_ref_set(&config_d, 13U, 4U, 0xD1U);
    conflict = persist_cluster_create_request(4U, &committed, 2U, 8U);
    conflict.next_state.committed_config = config_a;
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&conflict) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    committed = first.next_state;
    committed.last_completed_operation_id = 0U;
    committed.last_completed_operation = 0U;
    committed.last_completed_operation_fingerprint = 0U;
    committed.committed_config = config_a;
    prepared = committed;
    prepared.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    prepared.config_transaction.transaction_id = 1U;
    prepared.config_transaction.staging_config = config_b;
    request = persist_request_from_state(
        1U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE, &prepared);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    prepared = request.next_state;

    next = prepared;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    next.committed_config = config_c;
    next.config_transaction.staging_config = config_c;
    request = persist_request_from_state(
        2U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&prepared, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);

    next = prepared;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    next.config_transaction.transaction_id = 42U;
    next.committed_config = config_b;
    request = persist_request_from_state(
        2U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&prepared, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);

    next = prepared;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    next.committed_config = config_b;
    {
        ucn_cluster_persist_request_t joint_request = persist_request_from_state(
            2U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT, &prepared);

        TEST_ASSERT(ucn_cluster_persist_request_is_valid(&joint_request));
        TEST_ASSERT(ucn_cluster_persist_request_admit(&prepared, &joint_request) ==
                    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
        prepared = joint_request.next_state;
    }
    request = persist_request_from_state(
        3U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&prepared, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    committed = request.next_state;

    /* A new Config transaction must consume the next txid and the next
     * Config ID/generation.  This rejects an old transaction replay after a
     * restart instead of treating it as a fresh Prepare. */
    next = committed;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    next.config_transaction.transaction_id = 1U;
    next.config_transaction.staging_config = config_c;
    request = persist_request_from_state(
        3U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE, &next);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    next.config_transaction.transaction_id = 2U;
    next.config_transaction.staging_config = config_a;
    request = persist_request_from_state(
        3U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    next.config_transaction.transaction_id = 3U;
    next.config_transaction.staging_config = config_c;
    request = persist_request_from_state(
        3U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    next.config_transaction.transaction_id = 2U;
    next.config_transaction.staging_config = config_d;
    request = persist_request_from_state(
        3U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &committed, 1U, record, sizeof(record)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &record_generation, &reloaded) ==
                UCN_OK);
    next = reloaded;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    next.config_transaction.transaction_id = 1U;
    next.config_transaction.staging_config = config_c;
    request = persist_request_from_state(
        3U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&reloaded, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    next.config_transaction.transaction_id = 2U;
    next.config_transaction.staging_config = config_c;
    request = persist_request_from_state(
        4U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);

    /* Config and Rekey may not be PREPARED at the same time. */
    successor.cluster_id = 2U;
    successor.term = 1U;
    successor.head_node_id = 9U;
    next = prepared;
    next.rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    next.rekey_transaction.transaction_id = 61U;
    persist_rekey_ref_set(&next.rekey_transaction.staging_rekey, 7U, 12U,
                          &prepared.active_epoch, &config_a, &successor);
    request.operation_id = 3U;
    request.operation = UCN_CLUSTER_PERSIST_OPERATION_REKEY_PREPARE;
    request.next_state = next;
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&request) == UCN_ERR_CONFIG);

    /* Rekey PREPARE fixes the predecessor epoch/config and successor
     * identity.  Commit must replay exactly that transaction and atomically
     * create its matching tombstone. */
    next = committed;
    next.rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    next.rekey_transaction.transaction_id = 61U;
    persist_rekey_ref_set(&next.rekey_transaction.staging_rekey, 7U, 10U,
                          &committed.active_epoch, &committed.committed_config,
                          &successor);
    request = persist_request_from_state(
        4U, UCN_CLUSTER_PERSIST_OPERATION_REKEY_PREPARE, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    prepared = request.next_state;

    next = prepared;
    next.active_epoch = successor;
    next.max_epoch = successor;
    next.committed_rekey = prepared.rekey_transaction.staging_rekey;
    next.committed_rekey.successor_epoch.cluster_id = 3U;
    next.active_epoch = next.committed_rekey.successor_epoch;
    next.max_epoch = next.committed_rekey.successor_epoch;
    next.committed_config = next.committed_rekey.successor_config;
    next.boot_incarnation = next.committed_rekey.next_incarnation;
    (void)memset(&next.config_transaction, 0,
                 sizeof(next.config_transaction));
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    next.rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    (void)memset(&next.rekey_transaction.staging_rekey, 0,
                 sizeof(next.rekey_transaction.staging_rekey));
    next.tombstone.valid = true;
    next.tombstone.retired_epoch = prepared.active_epoch;
    next.tombstone.replacement_cluster_id =
        next.committed_rekey.successor_epoch.cluster_id;
    next.tombstone.rekey_transaction_id = 61U;
    request = persist_request_from_state(
        5U, UCN_CLUSTER_PERSIST_OPERATION_REKEY_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&prepared, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);

    next = prepared;
    next.active_epoch = successor;
    next.max_epoch = successor;
    next.committed_rekey = prepared.rekey_transaction.staging_rekey;
    next.committed_config = next.committed_rekey.successor_config;
    next.boot_incarnation = next.committed_rekey.next_incarnation;
    (void)memset(&next.config_transaction, 0,
                 sizeof(next.config_transaction));
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    next.rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    (void)memset(&next.rekey_transaction.staging_rekey, 0,
                 sizeof(next.rekey_transaction.staging_rekey));
    next.tombstone.valid = true;
    next.tombstone.retired_epoch = prepared.active_epoch;
    next.tombstone.replacement_cluster_id = successor.cluster_id;
    next.tombstone.rekey_transaction_id = 61U;
    request = persist_request_from_state(
        5U, UCN_CLUSTER_PERSIST_OPERATION_REKEY_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&prepared, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    committed = request.next_state;

    /* Record v1 has no retired-identity set.  Therefore a completed Rekey is
     * a hard M04 boundary: CLUSTER_CREATE must not erase its Tombstone or
     * recreate either the retired A identity or an unrelated C identity until
     * M12 provides durable lineage storage.  Reloading the record is part of
     * the regression: this must remain true after a restart. */
    TEST_ASSERT(committed.tombstone.valid && committed.committed_rekey.valid);
    conflict = persist_cluster_create_request(
        6U, &committed, committed.tombstone.retired_epoch.cluster_id, 8U);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&conflict));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    conflict = persist_cluster_create_request(6U, &committed, 3U, 8U);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    TEST_ASSERT(committed.tombstone.valid &&
                committed.tombstone.retired_epoch.cluster_id == 1U &&
                committed.tombstone.replacement_cluster_id == 2U);
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &committed, 2U, record, sizeof(record)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &record_generation, &reloaded) ==
                UCN_OK && reloaded.tombstone.valid &&
                reloaded.committed_rekey.valid);
    conflict = persist_cluster_create_request(6U, &reloaded, 3U, 8U);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&reloaded, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    TEST_ASSERT(reloaded.tombstone.retired_epoch.cluster_id == 1U &&
                reloaded.tombstone.replacement_cluster_id == 2U);

    request.operation_id = 6U;
    request.operation = UCN_CLUSTER_PERSIST_OPERATION_TOMBSTONE_COMMIT;
    request.next_state = committed;
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&request) == UCN_ERR_CONFIG);

    /* VoteId includes backup_generation.  A record reload preserves the
     * exact tuple, but every Active Epoch permits exactly one durable vote:
     * candidate/generation changes are conflicts, not a second promise. */
    committed = first.next_state;
    next = committed;
    next.last_vote.valid = true;
    next.last_vote.epoch = committed.active_epoch;
    next.last_vote.voted_for_node_id = 8U;
    next.last_vote.backup_generation = 17U;
    request = persist_request_from_state(
        3U, UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    committed = request.next_state;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &committed, 1U, record, sizeof(record)) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &record_generation, &reloaded) ==
                UCN_OK && reloaded.last_vote.valid &&
                reloaded.last_vote.backup_generation == 17U);

    next = committed;
    request = persist_request_from_state(
        4U, UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.last_vote.backup_generation = 18U;
    request = persist_request_from_state(
        4U, UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.last_vote.voted_for_node_id = 9U;
    request = persist_request_from_state(
        4U, UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    next.active_epoch.term = 2U;
    next.max_epoch.term = 2U;
    request = persist_request_from_state(
        4U, UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    committed = request.next_state;
    next = committed;
    next.last_vote.valid = true;
    next.last_vote.epoch = committed.active_epoch;
    next.last_vote.voted_for_node_id = 9U;
    next.last_vote.backup_generation = 1U;
    request = persist_request_from_state(
        5U, UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);

    /* R23 is the sole bridge for a valid old Record-v1 PREPARED state.  It
     * must reject the old generic replay, preserve authority fields exactly,
     * and clear only the prepared payload while advancing incarnation. */
    TEST_ASSERT(persist_legacy_config_prepared_state(&committed) == UCN_OK);
    replay = persist_replay_incarnation_request(8U, &committed, 10U);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &replay) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    next = committed;
    (void)memset(&next.config_transaction, 0,
                 sizeof(next.config_transaction));
    next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    next.boot_incarnation = 10U;
    next.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    request = persist_request_from_state(
        8U, UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    conflict = request;
    conflict.next_state.active_epoch.term = 6U;
    conflict.next_state.max_epoch.term = 6U;
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&conflict) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &conflict) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED);
    TEST_ASSERT(persist_legacy_rekey_prepared_state(&committed) == UCN_OK);
    next = committed;
    (void)memset(&next.rekey_transaction, 0,
                 sizeof(next.rekey_transaction));
    next.rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    next.boot_incarnation = 10U;
    next.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    request = persist_request_from_state(
        8U, UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT, &next);
    TEST_ASSERT(ucn_cluster_persist_request_admit(&committed, &request) ==
                UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW);
    return 0;
}

/* M04 runtime tests intentionally use the public Cluster entry points rather
 * than invoking the bridge directly.  This proves that no actual control
 * packet enters the send callback before the Provider has reloaded the
 * matching durable operation journal. */
typedef struct cluster_persist_runtime_probe {
    cluster_persist_fake_t store;
    uint32_t now_ms;
    uint32_t send_attempts;
    uint8_t send_no_space_before_success;
    uint8_t send_link_down_before_success;
    uint32_t sent_count;
    ucn_node_id_t destinations[4];
    uint8_t payloads[4][UCN_CLUSTER_MESSAGE_BYTES];
} cluster_persist_runtime_probe_t;

static uint32_t cluster_persist_runtime_now(void *context)
{
    return ((cluster_persist_runtime_probe_t *)context)->now_ms;
}

static ucn_result_t cluster_persist_runtime_send(
    void *context,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length)
{
    cluster_persist_runtime_probe_t *probe =
        (cluster_persist_runtime_probe_t *)context;
    uint32_t slot;

    if (probe == NULL || payload == NULL ||
        endpoint != UCN_CLUSTER_CONTROL_ENDPOINT ||
        payload_length != UCN_CLUSTER_MESSAGE_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    probe->send_attempts++;
    if (probe->send_no_space_before_success != 0U) {
        probe->send_no_space_before_success--;
        return UCN_ERR_NO_SPACE;
    }
    if (probe->send_link_down_before_success != 0U) {
        probe->send_link_down_before_success--;
        return UCN_ERR_LINK_DOWN;
    }
    slot = probe->sent_count;
    if (slot >= 4U) {
        return UCN_ERR_NO_SPACE;
    }
    probe->destinations[slot] = destination;
    (void)memcpy(probe->payloads[slot], payload,
                 UCN_CLUSTER_MESSAGE_BYTES);
    probe->sent_count++;
    return UCN_OK;
}

static void cluster_persist_runtime_provider(
    ucn_cluster_persist_provider_t *provider,
    cluster_persist_runtime_probe_t *probe)
{
    (void)memset(provider, 0, sizeof(*provider));
    provider->struct_size = (uint16_t)sizeof(*provider);
    provider->api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider->load = cluster_persist_fake_load;
    provider->submit = cluster_persist_fake_submit;
    provider->poll = cluster_persist_fake_poll;
    provider->context = &probe->store;
}

static void cluster_persist_runtime_config(
    ucn_cluster_config_t *config,
    cluster_persist_runtime_probe_t *probe,
    const ucn_cluster_persist_provider_t *provider,
    bool head_capable)
{
    (void)memset(config, 0, sizeof(*config));
    config->local_node_id = 1U;
    config->enabled = true;
    config->head_capable = head_capable;
    config->member_capacity = head_capable ? 1U : 0U;
    config->now_ms = cluster_persist_runtime_now;
    config->now_context = probe;
    config->send = cluster_persist_runtime_send;
    config->send_context = probe;
    config->persistence_mode = UCN_CLUSTER_PERSISTENCE_REQUIRED;
    config->persistence_provider = provider;
    (void)ucn_cluster_config_apply_timing_profile(
        config, UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED);
}

static void cluster_persist_runtime_admit_peer(ucn_cluster_t *cluster,
                                               ucn_node_id_t node_id,
                                               size_t index)
{
    cluster->peers[index].occupied = true;
    cluster->peers[index].node_id = node_id;
    cluster->peers[index].neighbor_state = UCN_NEIGHBOR_ADMITTED;
}

static int cluster_persist_test_runtime_epoch_gate(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    ucn_cluster_message_t message;

    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    TEST_ASSERT(probe.store.stored.state == UCN_CLUSTER_PERSIST_LOAD_READY &&
                probe.store.stored.snapshot.boot_incarnation == 1U);
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    probe.now_ms = UINT32_C(100000);
    probe.store.submit_pending = true;
    probe.store.pending_polls_before_terminal = 1U;
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK);
    TEST_ASSERT(cluster.persistence_pending && !cluster.persistence_faulted &&
                probe.sent_count == 0U && cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK && probe.sent_count == 0U &&
                cluster.persistence_pending);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK && !cluster.persistence_pending &&
                cluster.role == UCN_CLUSTER_ROLE_CANDIDATE && probe.sent_count == 1U);
    TEST_ASSERT(ucn_cluster_message_decode(probe.payloads[0],
                                           UCN_CLUSTER_MESSAGE_BYTES,
                                           &message) == UCN_OK &&
                message.type == UCN_CLUSTER_MSG_ADVERTISE &&
                message.role == UCN_CLUSTER_ROLE_CANDIDATE &&
                message.term == 1U);
    TEST_ASSERT(probe.store.stored.snapshot.has_active_epoch &&
                probe.store.stored.snapshot.active_epoch.term == 1U &&
                probe.store.stored.snapshot.active_epoch.head_node_id == 1U);

    /* A claimed COMMITTED that is not observable after the mandatory reload
     * is fail-closed, not an implicit successful election. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    probe.now_ms = UINT32_C(100000);
    probe.store.commit_not_visible = true;
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_ERR_STATE);
    TEST_ASSERT(cluster.persistence_faulted && !cluster.persistence_pending &&
                probe.sent_count == 0U && cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    return 0;
}

static int cluster_persist_test_runtime_boot_incarnation_gate(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t first_boot;
    ucn_cluster_t second_boot;

    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    probe.store.submit_pending = true;
    TEST_ASSERT(ucn_cluster_init(&first_boot, &config) == UCN_OK &&
                first_boot.persistence_pending && probe.sent_count == 0U);
    TEST_ASSERT(ucn_cluster_step(&first_boot) == UCN_OK &&
                !first_boot.persistence_pending &&
                first_boot.config.cluster_id_incarnation == 1U &&
                probe.sent_count == 0U);
    /* A second controlled boot is a distinct replay domain even though the
     * high-frequency nonce starts over in the freshly initialized object. */
    probe.store.submit_pending = false;
    TEST_ASSERT(ucn_cluster_init(&second_boot, &config) == UCN_OK &&
                second_boot.config.cluster_id_incarnation == 2U &&
                probe.store.stored.snapshot.boot_incarnation == 2U);
    return 0;
}

static int cluster_persist_test_runtime_vote_gate(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    ucn_cluster_message_t prepare;
    ucn_cluster_message_t reply;
    uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES];

    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe.store.stored.snapshot, 5U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    cluster.cluster_id = 1U;
    cluster.term = 5U;
    cluster.head_node_id = 7U;
    cluster.known_backup_node_id = 2U;
    cluster.known_backup_generation = 3U;
    cluster.head_lease_expires_at_ms = UINT32_C(200000);
    cluster.next_keepalive_ms = UINT32_C(200000);
    cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    (void)memset(&prepare, 0, sizeof(prepare));
    prepare.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
    prepare.role = UCN_CLUSTER_ROLE_BACKUP;
    prepare.cluster_id = 1U;
    prepare.term = 5U;
    prepare.head_node_id = 7U;
    prepare.backup_generation = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&prepare, payload) == UCN_OK);
    probe.store.submit_pending = true;
    probe.store.pending_polls_before_terminal = 1U;
    TEST_ASSERT(ucn_cluster_receive(&cluster, 2U, true, payload,
                                    sizeof(payload)) == UCN_OK);
    TEST_ASSERT(cluster.persistence_pending && probe.sent_count == 0U);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK && probe.sent_count == 0U);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK && !cluster.persistence_pending &&
                probe.sent_count == 1U);
    TEST_ASSERT(ucn_cluster_message_decode(probe.payloads[0],
                                           UCN_CLUSTER_MESSAGE_BYTES,
                                           &reply) == UCN_OK &&
                reply.type == UCN_CLUSTER_MSG_TAKEOVER_ACK &&
                reply.backup_generation == 3U &&
                probe.store.stored.snapshot.last_vote.valid &&
                probe.store.stored.snapshot.last_vote.voted_for_node_id == 2U);
    /* Exact durable replay can send the lost ACK again without another write. */
    TEST_ASSERT(ucn_cluster_receive(&cluster, 2U, true, payload,
                                    sizeof(payload)) == UCN_OK &&
                probe.sent_count == 2U);
    cluster.known_backup_node_id = 3U;
    cluster.known_backup_generation = 4U;
    cluster_persist_runtime_admit_peer(&cluster, 3U, 1U);
    prepare.backup_generation = 4U;
    TEST_ASSERT(ucn_cluster_message_encode(&prepare, payload) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&cluster, 3U, true, payload,
                                    sizeof(payload)) == UCN_ERR_REPLAY &&
                probe.sent_count == 2U);

    /* A RAM member whose current authority is not the Provider's durable
     * active Epoch must not vote.  M05 will install a Join Epoch through the
     * Provider; M04 deliberately chooses availability loss over a restart
     * unsafe ACK. */
    cluster.term = 6U;
    prepare.term = 6U;
    TEST_ASSERT(ucn_cluster_message_encode(&prepare, payload) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&cluster, 3U, true, payload,
                                    sizeof(payload)) == UCN_ERR_STATE &&
                probe.sent_count == 2U && !cluster.persistence_faulted);

    /* A failed asynchronous Vote is a failed promise: no ACK may leak and
     * the Member remains frozen for the life of this initialized object. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe.store.stored.snapshot, 5U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    cluster.cluster_id = 1U;
    cluster.term = 5U;
    cluster.head_node_id = 7U;
    cluster.known_backup_node_id = 2U;
    cluster.known_backup_generation = 3U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    (void)memset(&prepare, 0, sizeof(prepare));
    prepare.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
    prepare.role = UCN_CLUSTER_ROLE_BACKUP;
    prepare.cluster_id = 1U;
    prepare.term = 5U;
    prepare.head_node_id = 7U;
    prepare.backup_generation = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&prepare, payload) == UCN_OK);
    probe.store.submit_pending = true;
    TEST_ASSERT(ucn_cluster_receive(&cluster, 2U, true, payload,
                                    sizeof(payload)) == UCN_OK &&
                cluster.persistence_pending && probe.sent_count == 0U);
    probe.store.poll_fails = true;
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_ERR_STATE &&
                cluster.persistence_faulted && !cluster.persistence_pending &&
                cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
                probe.sent_count == 0U);

    /* Vote persistence is complete before a local adapter/token back-pressure
     * result.  Preserve the durable response and retry it; do not confuse
     * UCN_ERR_NO_SPACE with a failed Flash promise. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe.store.stored.snapshot, 5U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    cluster.cluster_id = 1U;
    cluster.term = 5U;
    cluster.head_node_id = 7U;
    cluster.known_backup_node_id = 2U;
    cluster.known_backup_generation = 3U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    (void)memset(&prepare, 0, sizeof(prepare));
    prepare.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
    prepare.role = UCN_CLUSTER_ROLE_BACKUP;
    prepare.cluster_id = 1U;
    prepare.term = 5U;
    prepare.head_node_id = 7U;
    prepare.backup_generation = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&prepare, payload) == UCN_OK);
    probe.store.submit_pending = true;
    probe.send_no_space_before_success = 1U;
    TEST_ASSERT(ucn_cluster_receive(&cluster, 2U, true, payload,
                                    sizeof(payload)) == UCN_OK &&
                cluster.persistence_pending);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_ERR_NO_SPACE &&
                probe.store.stored.snapshot.last_vote.valid &&
                cluster.persistence_retry_pending &&
                !cluster.persistence_faulted && probe.sent_count == 0U &&
                probe.send_attempts == 1U);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK &&
                !cluster.persistence_retry_pending &&
                !cluster.persistence_faulted && probe.sent_count == 1U &&
                probe.send_attempts == 2U);

    /* R21: a Vote restored from durable storage must use exactly the same
     * dispatcher.  A lost replay ACK may hit local back-pressure before any
     * new Vote write is attempted, then must reload/prove/retry on step. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe.store.stored.snapshot, 5U);
    probe.store.stored.snapshot.last_vote.valid = true;
    probe.store.stored.snapshot.last_vote.epoch =
        probe.store.stored.snapshot.active_epoch;
    probe.store.stored.snapshot.last_vote.voted_for_node_id = 2U;
    probe.store.stored.snapshot.last_vote.backup_generation = 3U;
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    cluster.cluster_id = 1U;
    cluster.term = 5U;
    cluster.head_node_id = 7U;
    cluster.known_backup_node_id = 2U;
    cluster.known_backup_generation = 3U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    (void)memset(&prepare, 0, sizeof(prepare));
    prepare.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
    prepare.role = UCN_CLUSTER_ROLE_BACKUP;
    prepare.cluster_id = 1U;
    prepare.term = 5U;
    prepare.head_node_id = 7U;
    prepare.backup_generation = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&prepare, payload) == UCN_OK);
    probe.send_no_space_before_success = 1U;
    TEST_ASSERT(ucn_cluster_receive(&cluster, 2U, true, payload,
                                    sizeof(payload)) == UCN_ERR_NO_SPACE &&
                cluster.persistence_retry_pending && !cluster.persistence_faulted &&
                probe.send_attempts == 1U);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK &&
                !cluster.persistence_retry_pending && !cluster.persistence_faulted &&
                probe.sent_count == 1U && probe.send_attempts == 2U);

    /* R22: a direct bearer outage after the asynchronous Vote has become
     * durable is likewise a retryable transport result, never a persistence
     * failure.  Restoring the bearer lets the next step send the same ACK. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe.store.stored.snapshot, 5U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    cluster.cluster_id = 1U;
    cluster.term = 5U;
    cluster.head_node_id = 7U;
    cluster.known_backup_node_id = 2U;
    cluster.known_backup_generation = 3U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    (void)memset(&prepare, 0, sizeof(prepare));
    prepare.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
    prepare.role = UCN_CLUSTER_ROLE_BACKUP;
    prepare.cluster_id = 1U;
    prepare.term = 5U;
    prepare.head_node_id = 7U;
    prepare.backup_generation = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&prepare, payload) == UCN_OK);
    probe.store.submit_pending = true;
    probe.send_link_down_before_success = 1U;
    TEST_ASSERT(ucn_cluster_receive(&cluster, 2U, true, payload,
                                    sizeof(payload)) == UCN_OK &&
                cluster.persistence_pending);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_ERR_LINK_DOWN &&
                probe.store.stored.snapshot.last_vote.valid &&
                cluster.persistence_retry_pending && !cluster.persistence_faulted &&
                cluster.stats.persistence_failures == 0U &&
                probe.send_attempts == 1U);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK &&
                !cluster.persistence_retry_pending && !cluster.persistence_faulted &&
                cluster.stats.persistence_failures == 0U && probe.sent_count == 1U &&
                probe.send_attempts == 2U);
    return 0;
}

static int cluster_persist_test_runtime_config_rekey_hooks(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    ucn_cluster_t restarted;
    ucn_cluster_persist_config_ref_t config_ref;
    ucn_cluster_persist_rekey_ref_t rekey;
    ucn_cluster_epoch_t successor;
    uint32_t submitted_before;
    uint32_t boot_after_first_init;
    bool committed;

    /* R20: M07/M13 have not supplied Prepare recovery (resume/abort/commit)
     * semantics.  Both synchronous and async-capable providers must therefore
     * see every public Prepare/Commit rejected before I/O, leaving no durable
     * PREPARED record that can block a later REPLAY_INCARNATION boot. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe.store.stored.snapshot, 1U);
    probe.store.stored.snapshot.active_epoch.head_node_id = 1U;
    probe.store.stored.snapshot.max_epoch.head_node_id = 1U;
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    persist_config_ref_set(&config_ref, 1U, 1U, 0xA1U);
    submitted_before = cluster.stats.persistence_submitted;
    boot_after_first_init = probe.store.stored.snapshot.boot_incarnation;
    TEST_ASSERT(ucn_cluster_persist_config_prepare(&cluster, 1U, &config_ref,
                                                   &committed) == UCN_ERR_CONFIG &&
                !committed && !cluster.persistence_pending &&
                !cluster.persistence_faulted &&
                cluster.stats.persistence_submitted == submitted_before &&
                probe.store.stored.snapshot.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE);
    TEST_ASSERT(ucn_cluster_persist_config_commit(&cluster, 1U, &committed) ==
                    UCN_ERR_CONFIG && !committed &&
                !probe.store.stored.snapshot.committed_config.valid &&
                cluster.stats.persistence_submitted == submitted_before &&
                !cluster.persistence_faulted);
    /* A rejected synchronous Config Prepare leaves a next boot fully usable. */
    TEST_ASSERT(ucn_cluster_init(&restarted, &config) == UCN_OK &&
                restarted.config.cluster_id_incarnation ==
                    boot_after_first_init + 1U &&
                probe.store.stored.snapshot.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE);

    /* Repeat for a provider configured to complete asynchronous writes.  The
     * ReKey Prepare must still never issue submit(), then reset safely. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe.store.stored.snapshot, 1U);
    persist_config_ref_set(&probe.store.stored.snapshot.committed_config,
                           1U, 1U, 0xA1U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    successor.cluster_id = 2U;
    successor.term = 1U;
    successor.head_node_id = 1U;
    persist_rekey_ref_set(&rekey, 1U, 2U,
                          &probe.store.stored.snapshot.active_epoch,
                          &probe.store.stored.snapshot.committed_config,
                          &successor);
    submitted_before = cluster.stats.persistence_submitted;
    boot_after_first_init = probe.store.stored.snapshot.boot_incarnation;
    probe.store.submit_pending = true;
    TEST_ASSERT(ucn_cluster_persist_rekey_prepare(&cluster, 1U, &rekey,
                                                   &committed) == UCN_ERR_CONFIG &&
                !committed && !cluster.persistence_pending &&
                cluster.stats.persistence_submitted == submitted_before &&
                probe.store.stored.snapshot.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE);
    TEST_ASSERT(ucn_cluster_persist_rekey_commit(&cluster, 1U, &committed) ==
                    UCN_ERR_CONFIG && !committed &&
                !probe.store.stored.snapshot.committed_rekey.valid &&
                !probe.store.stored.snapshot.tombstone.valid &&
                probe.store.stored.snapshot.active_epoch.cluster_id == 1U &&
                cluster.stats.persistence_submitted == submitted_before);
    probe.store.submit_pending = false;
    TEST_ASSERT(ucn_cluster_init(&restarted, &config) == UCN_OK &&
                restarted.config.cluster_id_incarnation ==
                    boot_after_first_init + 1U &&
                probe.store.stored.snapshot.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE);

    /* Explicit VOLATILE_TEST is equally unavailable: M04 cannot claim an
     * in-RAM Prepare is recoverable. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_config(&config, &probe, NULL, false);
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_config_prepare(&cluster, 1U, &config_ref,
                                                   &committed) == UCN_ERR_CONFIG &&
                !committed && !cluster.persistence_faulted);
    return 0;
}

/* R23: A valid, canonical Record-v1 PREPARED snapshot must not brick a
 * REQUIRED node after reset.  The controlled boot writes one atomic legacy
 * abort + replay-incarnation record before any role/timer/wire activity.
 * Config uses a synchronous Provider; Rekey exercises the PENDING path. */
static int cluster_persist_test_legacy_prepared_boot_migration(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    ucn_cluster_persist_state_t state;
    ucn_cluster_persist_state_t decoded;
    ucn_cluster_persist_request_t request;
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint32_t generation;

    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    TEST_ASSERT(persist_legacy_config_prepared_state(&state) == UCN_OK);
    TEST_ASSERT(persist_encode_legacy_v1_fixture(
                    &state, 7U, record, sizeof(record)) == UCN_OK);
    (void)memset(&decoded, 0, sizeof(decoded));
    generation = 0U;
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES,
                    &generation, &decoded) == UCN_OK &&
                generation == 7U &&
                decoded.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
                decoded.last_completed_operation == 0U);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    probe.store.stored.snapshot = decoded;
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK &&
                !cluster.persistence_pending &&
                cluster.config.cluster_id_incarnation == 10U &&
                probe.store.stored.snapshot.boot_incarnation == 10U &&
                probe.store.stored.snapshot.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
                probe.store.stored.snapshot.last_completed_operation ==
                    UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT);

    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    TEST_ASSERT(persist_legacy_rekey_prepared_state(&state) == UCN_OK);
    TEST_ASSERT(persist_encode_legacy_v1_fixture(
                    &state, 11U, record, sizeof(record)) == UCN_OK);
    (void)memset(&decoded, 0, sizeof(decoded));
    generation = 0U;
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES,
                    &generation, &decoded) == UCN_OK &&
                generation == 11U &&
                decoded.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
                decoded.last_completed_operation == 0U);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    probe.store.stored.snapshot = decoded;
    probe.store.submit_pending = true;
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK &&
                cluster.persistence_pending && probe.sent_count == 0U);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK &&
                !cluster.persistence_pending &&
                cluster.config.cluster_id_incarnation == 10U &&
                probe.store.stored.snapshot.boot_incarnation == 10U &&
                probe.store.stored.snapshot.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
                probe.store.stored.snapshot.last_completed_operation ==
                    UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT &&
                probe.sent_count == 0U);

    /* A current-schema Config PREPARED is deliberately not eligible for R23
     * or M13. It survives boot unchanged until the M07 owner resumes it. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    TEST_ASSERT(persist_legacy_config_prepared_state(&state) == UCN_OK);
    state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    state.last_completed_operation_id = 0U;
    state.last_completed_operation = 0U;
    state.last_completed_operation_fingerprint = 0U;
    request = persist_request_from_state(
        7U, UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE, &state);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    state = request.next_state;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &state, 17U, record, sizeof(record)) == UCN_OK &&
                 record[4U] == 0U && record[5U] == 4U);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    record, sizeof(record), &generation, &decoded) == UCN_OK &&
                decoded.record_schema_version ==
                    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
                decoded.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    probe.store.stored.snapshot = decoded;
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_ERR_STATE &&
                probe.store.stored.snapshot.record_schema_version ==
                    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
                probe.store.stored.snapshot.boot_incarnation == 9U &&
                probe.store.stored.snapshot.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
                probe.sent_count == 0U);

    /* Current-schema M13 Rekey PREPARED has an explicit exact Abort
     * transition. Controlled boot closes it and advances incarnation in one
     * atomic request before any role/timer/wire activity. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    TEST_ASSERT(persist_legacy_rekey_prepared_state(&state) == UCN_OK);
    state.record_schema_version = UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    state.last_completed_operation_id = 0U;
    state.last_completed_operation = 0U;
    state.last_completed_operation_fingerprint = 0U;
    request = persist_request_from_state(
        7U, UCN_CLUSTER_PERSIST_OPERATION_REKEY_PREPARE, &state);
    TEST_ASSERT(ucn_cluster_persist_request_is_valid(&request));
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    probe.store.stored.snapshot = request.next_state;
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, false);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK &&
                !cluster.persistence_pending && probe.sent_count == 0U &&
                cluster.config.cluster_id_incarnation == 10U &&
                probe.store.stored.snapshot.boot_incarnation == 10U &&
                probe.store.stored.snapshot.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED &&
                probe.store.stored.snapshot.last_completed_operation ==
                    UCN_CLUSTER_PERSIST_OPERATION_REKEY_ABORT);
    return 0;
}

static void cluster_persist_runtime_seed_active(
    cluster_persist_runtime_probe_t *probe,
    uint32_t term,
    ucn_node_id_t head_node_id);

/* Drives the generic persistence bridge without relying on the deliberately
 * unavailable M07/M13 Config/Rekey public Hooks.  ACTION_NONE is safe here:
 * this test-only Vote changes no runtime authority and exists solely to put a
 * Provider callback on stack for the R18 reentrancy boundary. */
static ucn_result_t cluster_persist_runtime_begin_test_vote(
    ucn_cluster_t *cluster,
    bool *committed)
{
    ucn_cluster_persist_vote_t vote;
    ucn_cluster_persist_state_t durable_state;

    if (cluster == NULL || committed == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&vote, 0, sizeof(vote));
    vote.valid = true;
    vote.epoch.cluster_id = cluster->cluster_id;
    vote.epoch.term = cluster->term;
    vote.epoch.head_node_id = cluster->head_node_id;
    vote.voted_for_node_id = 2U;
    vote.backup_generation = 1U;
    return cluster_persistence_begin_vote(
        cluster, &vote, CLUSTER_PERSIST_ACTION_NONE, 0U, committed,
        &durable_state);
}

static int cluster_persist_test_runtime_reentrancy_gate(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    bool committed;

    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 1U, 1U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    /* Even the first init-time load is a Provider callback boundary.  Verify
     * RX specifically: it must be rejected before parsing/peer side effects. */
    probe.store.reentry_cluster = &cluster;
    probe.store.reenter_on_load = true;
    probe.store.reenter_via_receive = true;
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK &&
                probe.store.reentry_calls == 1U &&
                probe.store.reentry_result == UCN_ERR_STATE &&
                probe.sent_count == 0U);
    cluster.role = UCN_CLUSTER_ROLE_HEAD;
    cluster.cluster_id = 1U;
    cluster.term = 1U;
    cluster.head_node_id = 1U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    cluster.next_advertise_ms = UINT32_C(200000);
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    /* Provider load() is external code too.  Its synchronous step reentry
     * observes the I/O gate before any old Head A advertisement can escape. */
    probe.store.reentry_cluster = &cluster;
    probe.store.reenter_on_load = true;
    probe.store.reenter_via_receive = false;
    TEST_ASSERT(cluster_persist_runtime_begin_test_vote(&cluster, &committed) ==
                    UCN_OK &&
                committed && probe.store.reentry_calls == 2U &&
                probe.store.reentry_result == UCN_ERR_STATE &&
                probe.sent_count == 0U && !cluster.persistence_io_active &&
                !cluster.persistence_faulted);

    /* A submit callback can only report PENDING after it returns.  The gate
     * must nevertheless already block a nested step before that point. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 1U, 1U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_HEAD;
    cluster.cluster_id = 1U;
    cluster.term = 1U;
    cluster.head_node_id = 1U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    cluster.next_advertise_ms = UINT32_C(200000);
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    probe.store.submit_pending = true;
    probe.store.reentry_cluster = &cluster;
    probe.store.reenter_on_submit = true;
    TEST_ASSERT(cluster_persist_runtime_begin_test_vote(&cluster, &committed) ==
                    UCN_OK &&
                !committed && cluster.persistence_pending &&
                probe.store.reentry_calls == 1U &&
                probe.store.reentry_result == UCN_ERR_STATE &&
                probe.sent_count == 0U && !cluster.persistence_io_active);

    /* poll() has the same dynamic I/O gate; a recursive poll/step is busy,
     * while the owning step can still resolve safely after the callback. */
    probe.store.reenter_on_poll = true;
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK &&
                !cluster.persistence_pending && probe.store.reentry_calls == 2U &&
                probe.store.reentry_result == UCN_ERR_STATE &&
                probe.sent_count == 0U && !cluster.persistence_io_active &&
                !cluster.persistence_faulted);
    return 0;
}

static int cluster_persist_test_runtime_failure_containment(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    bool committed;

    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    probe.store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe.store.stored.snapshot, 1U);
    probe.store.stored.snapshot.active_epoch.head_node_id = 1U;
    probe.store.stored.snapshot.max_epoch.head_node_id = 1U;
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    /* Represent a live Head whose next durable bridge operation fails.  The
     * generic test Vote is used because M07/M13 hooks are intentionally
     * disabled until their recovery continuations exist. */
    cluster.role = UCN_CLUSTER_ROLE_HEAD;
    cluster.cluster_id = 1U;
    cluster.term = 1U;
    cluster.head_node_id = 1U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    cluster_persist_runtime_admit_peer(&cluster, 2U, 0U);
    probe.store.submit_pending = true;
    TEST_ASSERT(cluster_persist_runtime_begin_test_vote(&cluster, &committed) ==
                    UCN_OK &&
                !committed && cluster.persistence_pending &&
                probe.sent_count == 0U);
    probe.store.poll_fails = true;
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_ERR_STATE);
    TEST_ASSERT(cluster.persistence_faulted && !cluster.persistence_pending &&
                cluster.role == UCN_CLUSTER_ROLE_TERM_CONFLICT &&
                probe.sent_count == 0U &&
                cluster.stats.persistence_failures == 1U);
    /* Fault remains sticky: neither a periodic step nor a direct inbound
     * control frame is allowed to resurrect a promise in this object. */
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_ERR_STATE &&
                probe.sent_count == 0U);
    return 0;
}

static void cluster_persist_runtime_seed_active(
    cluster_persist_runtime_probe_t *probe,
    uint32_t term,
    ucn_node_id_t head_node_id)
{
    persist_fake_set_factory_empty(&probe->store);
    probe->store.stored.state = UCN_CLUSTER_PERSIST_LOAD_READY;
    persist_state_set_epoch(&probe->store.stored.snapshot, term);
    probe->store.stored.snapshot.active_epoch.head_node_id = head_node_id;
    probe->store.stored.snapshot.max_epoch.head_node_id = head_node_id;
}

/* CLV2-M12 (12-09): recovery identity non-reuse across a controlled boot.
 * Boot 1 establishes a durable incarnation; the recovery ID derives from it.
 * Boot 2 (restart) loads the persisted snapshot, establishes a strictly
 * higher incarnation, and derives a DIFFERENT recovery ID - so a restart
 * can never re-derive the pre-restart recovery identity. */
static int cluster_persist_test_recovery_identity_restart(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    uint32_t id1;
    uint32_t id2;
    uint32_t incarnation1;

    /* Boot 1: factory empty -> a durable incarnation is established. */
    (void)memset(&probe, 0, sizeof(probe));
    persist_fake_set_factory_empty(&probe.store);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    incarnation1 = cluster.config.cluster_id_incarnation;
    TEST_ASSERT(incarnation1 != 0U);
    TEST_ASSERT(probe.store.stored.snapshot.boot_incarnation == incarnation1);
    TEST_ASSERT(cluster_make_next_recovery_id(
                    &cluster, 9U, 7U, 0U, 0U, &id1) == UCN_OK);
    TEST_ASSERT(id1 != 9U && id1 != 0U && id1 != UCN_NODE_BROADCAST);

    /* Boot 2 (restart): the persisted snapshot carries incarnation1; the
     * boot boundary derives strictly-higher incarnation1+1. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 9U, 1U);
    probe.store.stored.snapshot.boot_incarnation = incarnation1;
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    TEST_ASSERT(cluster.config.cluster_id_incarnation == incarnation1 + 1U);
    TEST_ASSERT(probe.store.stored.snapshot.boot_incarnation ==
                incarnation1 + 1U);
    TEST_ASSERT(cluster_make_next_recovery_id(
                    &cluster, 9U, 7U, 0U, 0U, &id2) == UCN_OK);
    TEST_ASSERT(id2 != id1);
    TEST_ASSERT(id2 != 9U && id2 != 0U && id2 != UCN_NODE_BROADCAST);
    return 0;
}

static int cluster_persist_test_runtime_head_paths(void)
{
    cluster_persist_runtime_probe_t probe;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    ucn_cluster_epoch_t recovery_epoch;
    ucn_cluster_persist_state_t durable_state;
    bool committed;

    /* CLV2-M11: a live Backup score must not create a competing same-Cluster
     * Term. The retired entry point performs zero Provider I/O. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 5U, 7U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    cluster.cluster_id = 1U;
    cluster.term = 5U;
    cluster.head_node_id = 7U;
    cluster.backup_ready = true;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_READY;
    {
        ucn_cluster_t before = cluster;

        TEST_ASSERT(backup_challenge(&cluster, probe.now_ms) ==
                    UCN_ERR_UNSUPPORTED);
        TEST_ASSERT(memcmp(&cluster, &before, sizeof(before)) == 0 &&
                    !cluster.persistence_pending && probe.sent_count == 0U);
    }

    /* Majority takeover uses the same gate: it cannot promote to Head until
     * the new Term+local Head Epoch has been reloaded from storage. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 5U, 7U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    cluster.cluster_id = 1U;
    cluster.term = 5U;
    cluster.head_node_id = 7U;
    cluster.backup_takeover_active = true;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    probe.store.submit_pending = true;
    TEST_ASSERT(complete_takeover(&cluster, probe.now_ms) == UCN_OK &&
                cluster.persistence_pending &&
                cluster.role == UCN_CLUSTER_ROLE_BACKUP && probe.sent_count == 0U);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK &&
                !cluster.persistence_pending && cluster.role == UCN_CLUSTER_ROLE_HEAD &&
                cluster.term == 6U && cluster.head_node_id == 1U &&
                probe.sent_count == 0U);

    /* Recovery creates a distinct Term-1 identity.  The deferred declaration
     * must leave RECOVERY_ELECTION untouched until the create record commits. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 5U, 7U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    cluster.recovery_eligible = true;
    cluster.recovery_backoff_deadline_ms = 1U;
    cluster.recovery_nonce = 1U;
    cluster.cluster_id_round = 1U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
    (void)memset(&recovery_epoch, 0, sizeof(recovery_epoch));
    recovery_epoch.cluster_id = 2U;
    recovery_epoch.term = 1U;
    recovery_epoch.head_node_id = 1U;
    probe.store.submit_pending = true;
    TEST_ASSERT(cluster_persistence_begin_epoch(
                    &cluster, &recovery_epoch,
                    CLUSTER_PERSIST_ACTION_RECOVERY_DECLARE, 0U, &committed,
                    &durable_state) == UCN_OK && !committed &&
                cluster.persistence_pending &&
                cluster.role == UCN_CLUSTER_ROLE_DETACHED && probe.sent_count == 0U);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK &&
                !cluster.persistence_pending &&
                cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD &&
                cluster.cluster_id == 2U && cluster.term == 1U &&
                probe.sent_count == 0U);

    /* CLV2-M12.1 (MAJOR-1): with a captured parent lineage A/T9, the
     * persisted recovery Epoch MUST carry Term 9 - the deferred
     * continuation adopts the durable promise and the RAM term equals
     * the published term. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 5U, 7U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    cluster.recovery_eligible = true;
    cluster.recovery_backoff_deadline_ms = 1U;
    cluster.recovery_nonce = 1U;
    cluster.cluster_id_round = 1U;
    cluster.parent_cluster_id = 5U;
    cluster.parent_term = 9U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
    (void)memset(&recovery_epoch, 0, sizeof(recovery_epoch));
    recovery_epoch.cluster_id = 2U;
    recovery_epoch.term = 9U; /* mirrors the parent term */
    recovery_epoch.head_node_id = 1U;
    probe.store.submit_pending = true;
    TEST_ASSERT(cluster_persistence_begin_epoch(
                    &cluster, &recovery_epoch,
                    CLUSTER_PERSIST_ACTION_RECOVERY_DECLARE, 0U, &committed,
                    &durable_state) == UCN_OK && !committed &&
                cluster.persistence_pending);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_OK &&
                !cluster.persistence_pending &&
                cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD &&
                cluster.cluster_id == 2U && cluster.term == 9U &&
                cluster.head_node_id == 1U);

    /* A persisted Term that diverges from the computed recovery Term
     * (parent T9 but a legacy Term-1 record) must FAIL CLOSED: no
     * RECOVERY_HEAD, no publish, persistence fault recorded. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 5U, 7U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    cluster.recovery_eligible = true;
    cluster.recovery_backoff_deadline_ms = 1U;
    cluster.recovery_nonce = 1U;
    cluster.cluster_id_round = 1U;
    cluster.parent_cluster_id = 5U;
    cluster.parent_term = 9U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
    (void)memset(&recovery_epoch, 0, sizeof(recovery_epoch));
    recovery_epoch.cluster_id = 2U;
    recovery_epoch.term = 1U; /* mismatched legacy record */
    recovery_epoch.head_node_id = 1U;
    probe.store.submit_pending = true;
    TEST_ASSERT(cluster_persistence_begin_epoch(
                    &cluster, &recovery_epoch,
                    CLUSTER_PERSIST_ACTION_RECOVERY_DECLARE, 0U, &committed,
                    &durable_state) == UCN_OK && !committed &&
                cluster.persistence_pending);
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_ERR_STATE &&
                cluster.persistence_faulted &&
                cluster.role == UCN_CLUSTER_ROLE_DETACHED &&
                probe.sent_count == 0U);

    /* A failed pending takeover cannot promote a Backup after its local
     * quorum has already been observed. */
    (void)memset(&probe, 0, sizeof(probe));
    cluster_persist_runtime_seed_active(&probe, 5U, 7U);
    cluster_persist_runtime_provider(&provider, &probe);
    cluster_persist_runtime_config(&config, &probe, &provider, true);
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    cluster.cluster_id = 1U;
    cluster.term = 5U;
    cluster.head_node_id = 7U;
    cluster.backup_takeover_active = true;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
    probe.store.submit_pending = true;
    TEST_ASSERT(complete_takeover(&cluster, probe.now_ms) == UCN_OK &&
                cluster.persistence_pending &&
                cluster.role == UCN_CLUSTER_ROLE_BACKUP && probe.sent_count == 0U);
    probe.store.poll_fails = true;
    TEST_ASSERT(ucn_cluster_step(&cluster) == UCN_ERR_STATE &&
                cluster.persistence_faulted &&
                cluster.role == UCN_CLUSTER_ROLE_BACKUP &&
                probe.sent_count == 0U);
    return 0;
}

/* M04-10 Host double-slot provider.  The physical record remains encoded with
 * the public Record-v1 codec; this fake only supplies the atomic slot choice
 * and crash cut points a board-specific Flash provider must preserve. */
typedef enum cluster_persist_dual_fault {
    CLUSTER_PERSIST_DUAL_OK = 0,
    CLUSTER_PERSIST_DUAL_FAIL_BEFORE_WRITE = 1,
    CLUSTER_PERSIST_DUAL_TORN_WRITE = 2,
    CLUSTER_PERSIST_DUAL_FAIL_AFTER_FULL_WRITE = 3,
    CLUSTER_PERSIST_DUAL_CORRUPT_NEW_SLOT = 4
} cluster_persist_dual_fault_t;

typedef struct cluster_persist_dual_slot {
    uint8_t bytes[UCN_CLUSTER_PERSIST_RECORD_BYTES];
} cluster_persist_dual_slot_t;

typedef struct cluster_persist_dual_fake {
    cluster_persist_dual_slot_t slot[2];
    cluster_persist_dual_fault_t fault;
} cluster_persist_dual_fake_t;

static ucn_result_t cluster_persist_dual_load(
    void *context,
    ucn_cluster_persist_load_result_t *result)
{
    cluster_persist_dual_fake_t *fake = (cluster_persist_dual_fake_t *)context;
    ucn_cluster_persist_state_t best_state;
    uint32_t best_generation = 0U;
    bool have_best = false;
    bool saw_damaged = false;
    size_t index;

    if (fake == NULL || result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(result, 0, sizeof(*result));
    for (index = 0U; index < 2U; ++index) {
        ucn_cluster_persist_state_t candidate;
        uint32_t generation = 0U;
        ucn_result_t decoded = ucn_cluster_persist_record_decode(
            fake->slot[index].bytes, sizeof(fake->slot[index].bytes),
            &generation, &candidate);

        if (decoded == UCN_ERR_NOT_FOUND) {
            continue;
        }
        if (decoded != UCN_OK) {
            saw_damaged = true;
            continue;
        }
        if (!have_best ||
            ucn_cluster_persist_record_generation_is_newer(generation,
                                                            best_generation)) {
            best_state = candidate;
            best_generation = generation;
            have_best = true;
        }
    }
    if (have_best) {
        result->state = UCN_CLUSTER_PERSIST_LOAD_READY;
        result->snapshot = best_state;
        return UCN_OK;
    }
    if (saw_damaged) {
        return UCN_ERR_CRC;
    }
    result->state = UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY;
    return UCN_OK;
}

static ucn_cluster_persist_completion_t cluster_persist_dual_submit(
    void *context,
    const ucn_cluster_persist_request_t *request)
{
    cluster_persist_dual_fake_t *fake = (cluster_persist_dual_fake_t *)context;
    ucn_cluster_persist_load_result_t loaded;
    ucn_cluster_persist_state_t current;
    ucn_cluster_persist_request_admission_t admission;
    uint32_t highest_generation = 0U;
    size_t highest_index = 0U;
    size_t target_index;
    uint8_t encoded[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    size_t index;

    if (fake == NULL || !ucn_cluster_persist_request_is_valid(request) ||
        cluster_persist_dual_load(fake, &loaded) != UCN_OK) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_ARGUMENT);
    }
    if (loaded.state == UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY) {
        ucn_cluster_persist_state_init_empty(&current);
    } else {
        current = loaded.snapshot;
    }
    admission = ucn_cluster_persist_request_admit(&current, request);
    if (admission == UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED ||
        admission == UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_INVALID) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_REPLAY);
    }
    if (admission == UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_IDEMPOTENT) {
        return persist_completion(UCN_CLUSTER_PERSIST_COMMITTED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE, UCN_OK);
    }
    for (index = 0U; index < 2U; ++index) {
        ucn_cluster_persist_state_t ignored;
        uint32_t generation = 0U;

        if (ucn_cluster_persist_record_decode(
                fake->slot[index].bytes, sizeof(fake->slot[index].bytes),
                &generation, &ignored) == UCN_OK &&
            (highest_generation == 0U ||
             ucn_cluster_persist_record_generation_is_newer(
                 generation, highest_generation))) {
            highest_generation = generation;
            highest_index = index;
        }
    }
    target_index = highest_generation == 0U ? 0U : 1U - highest_index;
    if (ucn_cluster_persist_record_encode(
            &request->next_state, highest_generation + 1U, encoded,
            sizeof(encoded)) != UCN_OK) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_CONFIG);
    }
    if (fake->fault == CLUSTER_PERSIST_DUAL_FAIL_BEFORE_WRITE) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_STATE);
    }
    if (fake->fault == CLUSTER_PERSIST_DUAL_TORN_WRITE) {
        (void)memset(fake->slot[target_index].bytes, 0,
                     sizeof(fake->slot[target_index].bytes));
        (void)memcpy(fake->slot[target_index].bytes, encoded,
                     sizeof(encoded) / 2U);
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_STATE);
    }
    (void)memcpy(fake->slot[target_index].bytes, encoded, sizeof(encoded));
    if (fake->fault == CLUSTER_PERSIST_DUAL_CORRUPT_NEW_SLOT) {
        fake->slot[target_index].bytes[TEST_CRC_OFFSET] ^= UINT8_C(0x5A);
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_CRC);
    }
    if (fake->fault == CLUSTER_PERSIST_DUAL_FAIL_AFTER_FULL_WRITE) {
        return persist_completion(UCN_CLUSTER_PERSIST_FAILED,
                                  UCN_CLUSTER_PERSIST_TOKEN_NONE,
                                  UCN_ERR_STATE);
    }
    return persist_completion(UCN_CLUSTER_PERSIST_COMMITTED,
                              UCN_CLUSTER_PERSIST_TOKEN_NONE, UCN_OK);
}

static int cluster_persist_dual_seed_epoch(cluster_persist_dual_fake_t *fake)
{
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_persist_state_t empty;
    ucn_cluster_persist_load_result_t loaded;
    ucn_cluster_persist_completion_t completion;
    ucn_cluster_persist_request_t request;

    (void)memset(fake, 0, sizeof(*fake));
    (void)memset(&provider, 0, sizeof(provider));
    provider.struct_size = (uint16_t)sizeof(provider);
    provider.api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider.load = cluster_persist_dual_load;
    provider.submit = cluster_persist_dual_submit;
    provider.context = fake;
    ucn_cluster_persist_state_init_empty(&empty);
    request = persist_replay_incarnation_request(1U, &empty, 1U);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK);
    request = persist_cluster_create_request(2U, &loaded.snapshot, 1U, 7U);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);
    return 0;
}

static int cluster_persist_test_dual_slot_crash_matrix(void)
{
    cluster_persist_dual_fake_t fake;
    ucn_cluster_persist_load_result_t loaded;
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_completion_t completion;
    cluster_persist_dual_fault_t fault;

    for (fault = CLUSTER_PERSIST_DUAL_FAIL_BEFORE_WRITE;
         fault <= CLUSTER_PERSIST_DUAL_CORRUPT_NEW_SLOT;
         fault = (cluster_persist_dual_fault_t)(fault + 1)) {
        TEST_ASSERT(cluster_persist_dual_seed_epoch(&fake) == 0);
        TEST_ASSERT(cluster_persist_dual_load(&fake, &loaded) == UCN_OK &&
                    loaded.snapshot.active_epoch.term == 1U);
        request = persist_epoch_request_from_state(3U, &loaded.snapshot, 2U);
        fake.fault = fault;
        completion = cluster_persist_dual_submit(&fake, &request);
        TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_FAILED);
        TEST_ASSERT(cluster_persist_dual_load(&fake, &loaded) == UCN_OK);
        if (fault == CLUSTER_PERSIST_DUAL_FAIL_AFTER_FULL_WRITE) {
            /* Crash after the new slot became durable: reboot sees the new
             * journal and exact retry is idempotent, never half-old. */
            TEST_ASSERT(loaded.snapshot.active_epoch.term == 2U);
            fake.fault = CLUSTER_PERSIST_DUAL_OK;
            completion = cluster_persist_dual_submit(&fake, &request);
            TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);
        } else {
            TEST_ASSERT(loaded.snapshot.active_epoch.term == 1U);
        }
    }
    return 0;
}

/* R23 double-slot restart recovery.  A torn migration write leaves the old
 * PREPARED Record selectable; the next controlled boot retries the same
 * atomic abort+incarnation transition.  Cover both historical transaction
 * kinds rather than assuming Config and Rekey have identical record bodies. */
static int cluster_persist_test_recovery_scope_record(void)
{
    ucn_cluster_persist_state_t stable;
    ucn_cluster_persist_state_t recovery;
    ucn_cluster_persist_state_t decoded;
    uint8_t stable_record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t recovery_record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint32_t generation = 0U;

    ucn_cluster_persist_state_init_empty(&stable);
    stable.has_active_epoch = true;
    stable.active_epoch.cluster_id = 5U;
    stable.active_epoch.term = 5U;
    stable.active_epoch.head_node_id = 1U;
    stable.has_max_epoch = true;
    stable.max_epoch = stable.active_epoch;
    stable.boot_incarnation = 1U;
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &stable, 1U, stable_record, sizeof(stable_record)) == UCN_OK);

    recovery = stable;
    recovery.epoch_scope = UCN_CLUSTER_PERSIST_EPOCH_SCOPE_RECOVERY;
    recovery.active_epoch.cluster_id = 21U;
    recovery.max_epoch = recovery.active_epoch;
    recovery.recovery_identity.valid = true;
    recovery.recovery_identity.epoch = recovery.active_epoch;
    recovery.recovery_identity.parent_cluster_id = 5U;
    recovery.recovery_identity.parent_term = 5U;
    recovery.recovery_identity.parent_config_id = 1U;
    recovery.recovery_identity.recovery_round = 3U;
    recovery.recovery_identity.recovery_nonce = 4U;
    recovery.recovery_identity.cluster_id_round = 2U;
    recovery.recovery_tombstone.valid = true;
    recovery.recovery_tombstone.retired_epoch.cluster_id = 20U;
    recovery.recovery_tombstone.retired_epoch.term = 5U;
    recovery.recovery_tombstone.retired_epoch.head_node_id = 1U;
    recovery.recovery_tombstone.replacement_cluster_id = 21U;
    recovery.recovery_tombstone.recovery_round = 2U;
    TEST_ASSERT(ucn_cluster_persist_state_is_valid(&recovery));
    TEST_ASSERT(ucn_cluster_persist_record_encode(
                    &recovery, 2U, recovery_record, sizeof(recovery_record)) ==
                UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    recovery_record, sizeof(recovery_record), &generation,
                    &decoded) == UCN_OK && generation == 2U &&
                decoded.epoch_scope ==
                    UCN_CLUSTER_PERSIST_EPOCH_SCOPE_RECOVERY &&
                decoded.recovery_identity.parent_cluster_id == 5U &&
                decoded.recovery_identity.recovery_round == 3U &&
                ucn_cluster_persist_recovery_identity_admit(
                    &decoded, 20U, 2U, 3U) == UCN_ERR_REPLAY &&
                ucn_cluster_persist_recovery_identity_admit(
                    &decoded, 21U, 3U, 4U) == UCN_OK);
    /* Torn newest slot cannot reinterpret Recovery as Stable; the older
     * complete slot remains independently decodable. */
    recovery_record[350U] ^= 1U;
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    recovery_record, sizeof(recovery_record), &generation,
                    &decoded) == UCN_ERR_CRC);
    TEST_ASSERT(ucn_cluster_persist_record_decode(
                    stable_record, sizeof(stable_record), &generation,
                    &decoded) == UCN_OK && generation == 1U &&
                decoded.epoch_scope == UCN_CLUSTER_PERSIST_EPOCH_SCOPE_STABLE &&
                decoded.active_epoch.cluster_id == 5U);
    return 0;
}

static int cluster_persist_test_dual_slot_legacy_prepared_recovery(void)
{
    cluster_persist_dual_fake_t fake;
    cluster_persist_runtime_probe_t runtime;
    ucn_cluster_persist_provider_t provider;
    ucn_cluster_config_t config;
    ucn_cluster_t cluster;
    ucn_cluster_persist_state_t state;
    ucn_cluster_persist_load_result_t loaded;

    (void)memset(&provider, 0, sizeof(provider));
    provider.struct_size = (uint16_t)sizeof(provider);
    provider.api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION;
    provider.load = cluster_persist_dual_load;
    provider.submit = cluster_persist_dual_submit;
    provider.poll = NULL;
    provider.context = &fake;
    (void)memset(&runtime, 0, sizeof(runtime));
    cluster_persist_runtime_config(&config, &runtime, &provider, false);

    (void)memset(&fake, 0, sizeof(fake));
    TEST_ASSERT(persist_legacy_config_prepared_state(&state) == UCN_OK);
    TEST_ASSERT(persist_encode_legacy_v1_fixture(
                    &state, 7U, fake.slot[0].bytes,
                    sizeof(fake.slot[0].bytes)) == UCN_OK);
    fake.fault = CLUSTER_PERSIST_DUAL_TORN_WRITE;
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_ERR_STATE);
    TEST_ASSERT(cluster_persist_dual_load(&fake, &loaded) == UCN_OK &&
                loaded.snapshot.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
                loaded.snapshot.boot_incarnation == 9U);
    fake.fault = CLUSTER_PERSIST_DUAL_OK;
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK &&
                cluster.config.cluster_id_incarnation == 10U &&
                cluster_persist_dual_load(&fake, &loaded) == UCN_OK &&
                loaded.snapshot.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
                loaded.snapshot.boot_incarnation == 10U);

    (void)memset(&fake, 0, sizeof(fake));
    TEST_ASSERT(persist_legacy_rekey_prepared_state(&state) == UCN_OK);
    TEST_ASSERT(persist_encode_legacy_v1_fixture(
                    &state, 11U, fake.slot[0].bytes,
                    sizeof(fake.slot[0].bytes)) == UCN_OK);
    fake.fault = CLUSTER_PERSIST_DUAL_TORN_WRITE;
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_ERR_STATE);
    TEST_ASSERT(cluster_persist_dual_load(&fake, &loaded) == UCN_OK &&
                loaded.snapshot.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
                loaded.snapshot.boot_incarnation == 9U);
    fake.fault = CLUSTER_PERSIST_DUAL_OK;
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK &&
                cluster.config.cluster_id_incarnation == 10U &&
                cluster_persist_dual_load(&fake, &loaded) == UCN_OK &&
                loaded.snapshot.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
                loaded.snapshot.boot_incarnation == 10U);
    return 0;
}

int test_cluster_persist(void)
{
    cluster_persist_fake_t fake;
    const ucn_cluster_persist_provider_t provider = {
        .struct_size = (uint16_t)sizeof(ucn_cluster_persist_provider_t),
        .api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION,
        .load = cluster_persist_fake_load,
        .submit = cluster_persist_fake_submit,
        .poll = cluster_persist_fake_poll,
        .context = &fake
    };
    const ucn_cluster_persist_provider_t sync_provider = {
        .struct_size = (uint16_t)sizeof(ucn_cluster_persist_provider_t),
        .api_version = UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION,
        .load = cluster_persist_fake_load,
        .submit = cluster_persist_fake_submit,
        .poll = NULL,
        .context = &fake
    };
    ucn_cluster_persist_load_result_t loaded;
    ucn_cluster_persist_completion_t completion;
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_state_t factory_empty;
    cluster_persist_fake_t restarted;
    ucn_cluster_epoch_t successor;

    persist_fake_set_factory_empty(&fake);
    TEST_ASSERT(ucn_cluster_persist_provider_is_compatible(&provider));
    TEST_ASSERT(ucn_cluster_persist_provider_supports_async(&provider));
    TEST_ASSERT(ucn_cluster_persist_provider_is_compatible(&sync_provider));
    TEST_ASSERT(!ucn_cluster_persist_provider_supports_async(&sync_provider));
    TEST_ASSERT(cluster_persist_test_contract_guards(&provider,
                                                     &sync_provider) == 0);
    TEST_ASSERT(cluster_persist_test_record_codec() == 0);
    TEST_ASSERT(cluster_persist_test_operation_admission() == 0);
    TEST_ASSERT(cluster_persist_test_runtime_epoch_gate() == 0);
    TEST_ASSERT(cluster_persist_test_runtime_boot_incarnation_gate() == 0);
    TEST_ASSERT(cluster_persist_test_runtime_vote_gate() == 0);
    TEST_ASSERT(cluster_persist_test_runtime_config_rekey_hooks() == 0);
    TEST_ASSERT(cluster_persist_test_legacy_prepared_boot_migration() == 0);
    TEST_ASSERT(cluster_persist_test_runtime_reentrancy_gate() == 0);
    TEST_ASSERT(cluster_persist_test_runtime_failure_containment() == 0);
    TEST_ASSERT(cluster_persist_test_runtime_head_paths() == 0);
    TEST_ASSERT(cluster_persist_test_recovery_scope_record() == 0);
    /* CLV2-M12 (12-09): recovery identity non-reuse across restart. */
    TEST_ASSERT(cluster_persist_test_recovery_identity_restart() == 0);
    TEST_ASSERT(cluster_persist_test_dual_slot_crash_matrix() == 0);
    TEST_ASSERT(cluster_persist_test_dual_slot_legacy_prepared_recovery() == 0);
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_persist_load_result_is_valid(&loaded) &&
                loaded.state == UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY);

    /* The provider-visible first boot path is deliberately two durable
     * operations: establish the replay incarnation, then create Cluster 1 at
     * Term 1.  Only afterwards may the normal same-Cluster Epoch path advance
     * to Term 2. */
    ucn_cluster_persist_state_init_empty(&factory_empty);
    request = persist_replay_incarnation_request(1U, &factory_empty, 9U);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(ucn_cluster_persist_provider_accepts_completion(
                    &provider, &completion));
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK);
    TEST_ASSERT(loaded.state == UCN_CLUSTER_PERSIST_LOAD_READY &&
                loaded.snapshot.boot_incarnation == 9U &&
                !loaded.snapshot.has_active_epoch);
    request = persist_cluster_create_request(2U, &loaded.snapshot, 1U, 7U);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK);
    TEST_ASSERT(loaded.state == UCN_CLUSTER_PERSIST_LOAD_READY &&
                loaded.snapshot.active_epoch.term == 1U);
    request = persist_epoch_request_from_state(3U, &loaded.snapshot, 2U);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK);
    TEST_ASSERT(loaded.state == UCN_CLUSTER_PERSIST_LOAD_READY &&
                loaded.snapshot.active_epoch.term == 2U);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);

    /* PENDING is not a promise.  A restart before commit reloads the old
     * state; repeated PENDING polls preserve the same opaque token. */
    fake.submit_pending = true;
    fake.pending_polls_before_terminal = 2U;
    request.next_state = loaded.snapshot;
    request.operation_id = 4U;
    request.operation = UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE;
    request.next_state.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    request.next_state.config_transaction.transaction_id = 1U;
    persist_config_ref_set(&request.next_state.config_transaction.staging_config,
                           1U, 1U, 0xB1U);
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&request) == UCN_OK);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_PENDING &&
                ucn_cluster_persist_provider_accepts_completion(
                    &provider, &completion));
    TEST_ASSERT(!ucn_cluster_persist_provider_accepts_completion(
                    &sync_provider, &completion));
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK &&
                loaded.snapshot.active_epoch.term == 2U);
    restarted = fake;
    restarted.pending = false;
    TEST_ASSERT(cluster_persist_fake_load(&restarted, &loaded) == UCN_OK &&
                loaded.snapshot.active_epoch.term == 2U);
    completion = provider.poll(provider.context, completion.token);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_PENDING);
    completion = provider.poll(provider.context, completion.token);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_PENDING);
    completion = provider.poll(provider.context, completion.token);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK &&
                loaded.snapshot.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
                loaded.snapshot.config_transaction.transaction_id == 1U);

    fake.submit_pending = false;
    request.operation_id = 5U;
    request.operation = UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT;
    request.next_state.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    persist_config_ref_set(&request.next_state.committed_config, 1U, 1U,
                           0xB1U);
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&request) == UCN_OK);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_COMMITTED);
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK &&
                loaded.snapshot.config_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
                loaded.snapshot.committed_config.config_id == 1U);

    fake.submit_fails = true;
    request.operation_id = 6U;
    request.operation = UCN_CLUSTER_PERSIST_OPERATION_REKEY_PREPARE;
    request.next_state.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    request.next_state.rekey_transaction.transaction_id = 42U;
    successor.cluster_id = 2U;
    successor.term = 1U;
    successor.head_node_id = 9U;
    persist_rekey_ref_set(&request.next_state.rekey_transaction.staging_rekey,
                          6U, request.next_state.boot_incarnation + 1U,
                          &request.next_state.active_epoch,
                          &request.next_state.committed_config, &successor);
    TEST_ASSERT(ucn_cluster_persist_request_finalize(&request) == UCN_OK);
    completion = provider.submit(provider.context, &request);
    TEST_ASSERT(completion.state == UCN_CLUSTER_PERSIST_FAILED);
    TEST_ASSERT(provider.load(provider.context, &loaded) == UCN_OK &&
                loaded.snapshot.rekey_transaction.phase ==
                    UCN_CLUSTER_PERSIST_TRANSACTION_NONE);
    return 0;
}
