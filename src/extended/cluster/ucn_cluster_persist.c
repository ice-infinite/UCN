#include <string.h>

#include "ucn/ucn_cluster_persist.h"

/* Record v1 is deliberately an explicit byte layout.  Do not replace these
 * offsets with a packed C struct: persistence must remain ABI-independent. */
enum {
    PERSIST_MAGIC_OFFSET = 0U,
    PERSIST_SCHEMA_OFFSET = 4U,
    PERSIST_SIZE_OFFSET = 6U,
    PERSIST_GENERATION_OFFSET = 8U,
    PERSIST_CRC_OFFSET = 12U,
    PERSIST_ACTIVE_VALID_OFFSET = 16U,
    PERSIST_ACTIVE_EPOCH_OFFSET = 17U,
    PERSIST_MAX_VALID_OFFSET = 29U,
    PERSIST_MAX_EPOCH_OFFSET = 30U,
    PERSIST_VOTE_VALID_OFFSET = 42U,
    PERSIST_VOTE_EPOCH_OFFSET = 43U,
    PERSIST_VOTE_FOR_OFFSET = 55U,
    PERSIST_VOTE_GENERATION_OFFSET = 59U,
    PERSIST_CONFIG_VALID_OFFSET = 63U,
    PERSIST_CONFIG_ID_OFFSET = 64U,
    PERSIST_CONFIG_GENERATION_OFFSET = 68U,
    PERSIST_CONFIG_DIGEST_OFFSET = 72U,
    PERSIST_CONFIG_TRANSACTION_PHASE_OFFSET = 88U,
    PERSIST_CONFIG_TRANSACTION_ID_OFFSET = 89U,
    PERSIST_CONFIG_STAGING_VALID_OFFSET = 93U,
    PERSIST_REKEY_VALID_OFFSET = 118U,
    PERSIST_REKEY_GENERATION_OFFSET = 119U,
    PERSIST_REKEY_INCARNATION_OFFSET = 123U,
    PERSIST_REKEY_PREDECESSOR_OFFSET = 127U,
    PERSIST_REKEY_PREDECESSOR_CONFIG_OFFSET = 139U,
    PERSIST_REKEY_SUCCESSOR_OFFSET = 164U,
    PERSIST_REKEY_TRANSACTION_PHASE_OFFSET = 176U,
    PERSIST_REKEY_TRANSACTION_ID_OFFSET = 177U,
    PERSIST_REKEY_STAGING_VALID_OFFSET = 181U,
    PERSIST_TOMBSTONE_VALID_OFFSET = 239U,
    PERSIST_TOMBSTONE_EPOCH_OFFSET = 240U,
    PERSIST_TOMBSTONE_REPLACEMENT_OFFSET = 252U,
    PERSIST_TOMBSTONE_REKEY_TRANSACTION_ID_OFFSET = 256U,
    PERSIST_BOOT_INCARNATION_OFFSET = 260U,
    PERSIST_LAST_OPERATION_ID_OFFSET = 264U,
    PERSIST_LAST_OPERATION_KIND_OFFSET = 268U,
    PERSIST_LAST_OPERATION_FINGERPRINT_OFFSET = 269U,
    PERSIST_RESERVED_OFFSET = 273U,
    PERSIST_RESERVED_BYTES = 7U
};

typedef char ucn_cluster_persist_record_layout_must_be_280_bytes[
    PERSIST_RESERVED_OFFSET + PERSIST_RESERVED_BYTES ==
            UCN_CLUSTER_PERSIST_RECORD_BYTES ? 1 : -1];

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) |
           input[3];
}

static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static bool bytes_are_zero(const uint8_t *input, size_t length)
{
    size_t index;

    if (input == NULL) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (input[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool serial_is_valid(uint32_t serial)
{
    return serial != 0U &&
           serial <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool record_schema_version_is_valid(uint16_t version)
{
    return version == UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1 ||
           version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2;
}

/* Persisted protocol serials advance by exactly one.  Accepting a merely
 * larger value would make it possible to skip a durable predecessor and later
 * reuse its operation/config identity after a restart. */
static bool serial_is_next(uint32_t current, uint32_t candidate)
{
    return serial_is_valid(current) && current <
           UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD &&
           candidate == current + 1U;
}

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool epoch_is_valid(const ucn_cluster_epoch_t *epoch)
{
    return epoch != NULL && epoch->cluster_id != 0U &&
           epoch->cluster_id != UCN_NODE_BROADCAST &&
           serial_is_valid(epoch->term) &&
           node_id_is_valid(epoch->head_node_id);
}

static bool epoch_is_equal(const ucn_cluster_epoch_t *a,
                           const ucn_cluster_epoch_t *b)
{
    return a != NULL && b != NULL && a->cluster_id == b->cluster_id &&
           a->term == b->term && a->head_node_id == b->head_node_id;
}

static bool config_ref_is_present_valid(
    const ucn_cluster_persist_config_ref_t *config)
{
    return config != NULL && config->valid && config->config_id != 0U &&
           config->config_id != UCN_NODE_BROADCAST &&
           serial_is_valid(config->config_id) &&
           serial_is_valid(config->generation);
}

static bool config_ref_is_equal(const ucn_cluster_persist_config_ref_t *a,
                                const ucn_cluster_persist_config_ref_t *b)
{
    if (a == NULL || b == NULL || a->valid != b->valid) {
        return false;
    }
    return !a->valid ||
           (a->config_id == b->config_id &&
            a->generation == b->generation &&
            memcmp(a->digest, b->digest, sizeof(a->digest)) == 0);
}

static bool config_ref_is_strict_successor(
    const ucn_cluster_persist_config_ref_t *committed,
    const ucn_cluster_persist_config_ref_t *candidate)
{
    if (committed == NULL || candidate == NULL || !candidate->valid) {
        return false;
    }
    if (!committed->valid) {
        return candidate->config_id == 1U && candidate->generation == 1U;
    }
    return serial_is_next(committed->config_id, candidate->config_id) &&
           serial_is_next(committed->generation, candidate->generation);
}

static bool rekey_ref_is_present_valid(
    const ucn_cluster_persist_rekey_ref_t *rekey)
{
    return rekey != NULL && rekey->valid && serial_is_valid(rekey->generation) &&
           serial_is_valid(rekey->next_incarnation) &&
           epoch_is_valid(&rekey->predecessor_epoch) &&
           config_ref_is_present_valid(&rekey->predecessor_config) &&
           epoch_is_valid(&rekey->successor_epoch) &&
           rekey->successor_epoch.term == 1U &&
           rekey->successor_epoch.cluster_id !=
               rekey->predecessor_epoch.cluster_id;
}

static bool rekey_ref_is_absent(const ucn_cluster_persist_rekey_ref_t *rekey)
{
    return rekey != NULL && !rekey->valid;
}

static bool rekey_ref_is_equal(const ucn_cluster_persist_rekey_ref_t *a,
                               const ucn_cluster_persist_rekey_ref_t *b)
{
    if (a == NULL || b == NULL || a->valid != b->valid) {
        return false;
    }
    return !a->valid ||
           (a->generation == b->generation &&
            a->next_incarnation == b->next_incarnation &&
            epoch_is_equal(&a->predecessor_epoch, &b->predecessor_epoch) &&
            config_ref_is_equal(&a->predecessor_config,
                                &b->predecessor_config) &&
            epoch_is_equal(&a->successor_epoch, &b->successor_epoch));
}

static bool persist_operation_is_valid(uint8_t operation)
{
    return (operation >= UCN_CLUSTER_PERSIST_OPERATION_REPLAY_INCARNATION &&
            operation <= UCN_CLUSTER_PERSIST_OPERATION_CLUSTER_CREATE_COMMIT) ||
           operation == UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT ||
           operation == UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT ||
           operation == UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT;
}

static bool transaction_phase_is_valid(
    ucn_cluster_persist_transaction_phase_t phase)
{
    return phase == UCN_CLUSTER_PERSIST_TRANSACTION_NONE ||
           phase == UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
           phase == UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
}

static bool config_transaction_is_valid(
    const ucn_cluster_persist_config_transaction_t *transaction,
    const ucn_cluster_persist_config_ref_t *committed)
{
    if (transaction == NULL || committed == NULL ||
        !transaction_phase_is_valid(transaction->phase)) {
        return false;
    }
    if (transaction->phase == UCN_CLUSTER_PERSIST_TRANSACTION_NONE) {
        return true;
    }
    if (!serial_is_valid(transaction->transaction_id)) {
        return false;
    }
    if (transaction->phase == UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        return config_ref_is_present_valid(&transaction->staging_config);
    }
    /* A terminal Config journal retains the immutable C_new reference.  It
     * binds CONFIG_ABORT replay to the staged proposal after a restart and
     * also makes a committed C_new explicit.  This repository has no
     * released v2 media contract; old pre-M07 terminal records are therefore
     * deliberately fail-closed rather than being silently interpreted as a
     * bound abort. */
    return config_ref_is_present_valid(&transaction->staging_config) &&
           config_ref_is_present_valid(committed);
}

static bool rekey_transaction_is_valid(
    const ucn_cluster_persist_rekey_transaction_t *transaction,
    const ucn_cluster_persist_rekey_ref_t *committed)
{
    if (transaction == NULL || committed == NULL ||
        !transaction_phase_is_valid(transaction->phase)) {
        return false;
    }
    if (transaction->phase == UCN_CLUSTER_PERSIST_TRANSACTION_NONE) {
        return true;
    }
    if (!serial_is_valid(transaction->transaction_id)) {
        return false;
    }
    if (transaction->phase == UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        return rekey_ref_is_present_valid(&transaction->staging_rekey);
    }
    return rekey_ref_is_absent(&transaction->staging_rekey) &&
           rekey_ref_is_present_valid(committed);
}

static bool tombstone_matches_committed_rekey(
    const ucn_cluster_persist_tombstone_t *tombstone,
    const ucn_cluster_persist_rekey_transaction_t *transaction,
    const ucn_cluster_persist_rekey_ref_t *rekey)
{
    return tombstone != NULL && transaction != NULL && rekey != NULL &&
           tombstone->valid &&
           transaction->phase == UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
           tombstone->rekey_transaction_id == transaction->transaction_id &&
           epoch_is_equal(&tombstone->retired_epoch,
                          &rekey->predecessor_epoch) &&
           tombstone->replacement_cluster_id ==
               rekey->successor_epoch.cluster_id;
}

static uint32_t request_fingerprint(
    uint8_t operation,
    const ucn_cluster_persist_state_t *state);

static bool completed_operation_journal_is_consistent(
    const ucn_cluster_persist_state_t *state)
{
    ucn_cluster_persist_state_t canonical_state;

    if (state->last_completed_operation_id == 0U) {
        return true;
    }
    canonical_state = *state;
    canonical_state.last_completed_operation_id = 0U;
    canonical_state.last_completed_operation = 0U;
    canonical_state.last_completed_operation_fingerprint = 0U;
    return request_fingerprint(state->last_completed_operation,
                               &canonical_state) ==
               state->last_completed_operation_fingerprint;
}

void ucn_cluster_persist_state_init_empty(ucn_cluster_persist_state_t *state)
{
    if (state == NULL) {
        return;
    }
    (void)memset(state, 0, sizeof(*state));
    state->record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2;
    state->config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    state->rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
}

/* Logical-state callers may reuse a structure.  Absent fields therefore do
 * not participate in the semantics; this helper gives every accepted write a
 * single canonical representation before it is fingerprinted or persisted. */
static void state_normalize_absent_fields(ucn_cluster_persist_state_t *state)
{
    /* Hand-built next states from pre-release callers historically did not
     * carry schema provenance. An absent value requests only the current
     * writer schema; an explicitly decoded v1 state retains its source. */
    if (state->record_schema_version == 0U) {
        state->record_schema_version =
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2;
    }
    if (!state->has_active_epoch) {
        (void)memset(&state->active_epoch, 0, sizeof(state->active_epoch));
    }
    if (!state->has_max_epoch) {
        (void)memset(&state->max_epoch, 0, sizeof(state->max_epoch));
    }
    if (!state->last_vote.valid) {
        (void)memset(&state->last_vote.epoch, 0,
                     sizeof(state->last_vote.epoch));
        state->last_vote.voted_for_node_id = 0U;
        state->last_vote.backup_generation = 0U;
    }
    if (!state->committed_config.valid) {
        (void)memset(&state->committed_config, 0,
                     sizeof(state->committed_config));
    }
    if (state->config_transaction.phase == UCN_CLUSTER_PERSIST_TRANSACTION_NONE) {
        state->config_transaction.transaction_id = 0U;
        (void)memset(&state->config_transaction.staging_config, 0,
                     sizeof(state->config_transaction.staging_config));
    }
    if (!state->committed_rekey.valid) {
        (void)memset(&state->committed_rekey, 0,
                     sizeof(state->committed_rekey));
    }
    if (state->rekey_transaction.phase == UCN_CLUSTER_PERSIST_TRANSACTION_NONE) {
        state->rekey_transaction.transaction_id = 0U;
        (void)memset(&state->rekey_transaction.staging_rekey, 0,
                     sizeof(state->rekey_transaction.staging_rekey));
    } else if (state->rekey_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED) {
        (void)memset(&state->rekey_transaction.staging_rekey, 0,
                     sizeof(state->rekey_transaction.staging_rekey));
    }
    if (!state->tombstone.valid) {
        (void)memset(&state->tombstone, 0, sizeof(state->tombstone));
    }
    if (state->last_completed_operation_id == 0U) {
        state->last_completed_operation = 0U;
        state->last_completed_operation_fingerprint = 0U;
    }
}

bool ucn_cluster_persist_state_is_valid(
    const ucn_cluster_persist_state_t *state)
{
    if (state == NULL ||
        !record_schema_version_is_valid(state->record_schema_version) ||
        (state->has_active_epoch && !epoch_is_valid(&state->active_epoch)) ||
        (state->has_max_epoch && !epoch_is_valid(&state->max_epoch)) ||
        (state->has_active_epoch &&
         (!state->has_max_epoch ||
          !epoch_is_equal(&state->active_epoch, &state->max_epoch))) ||
        (state->last_vote.valid &&
         (!epoch_is_valid(&state->last_vote.epoch) ||
          !node_id_is_valid(state->last_vote.voted_for_node_id) ||
          !serial_is_valid(state->last_vote.backup_generation))) ||
        (state->committed_config.valid &&
         !config_ref_is_present_valid(&state->committed_config)) ||
        !config_transaction_is_valid(&state->config_transaction,
                                     &state->committed_config) ||
        (state->config_transaction.phase ==
             UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
         !config_ref_is_equal(&state->config_transaction.staging_config,
                              &state->committed_config) &&
         !config_ref_is_strict_successor(
             &state->committed_config,
             &state->config_transaction.staging_config)) ||
        (state->committed_rekey.valid &&
         !rekey_ref_is_present_valid(&state->committed_rekey)) ||
        !rekey_transaction_is_valid(&state->rekey_transaction,
                                    &state->committed_rekey) ||
        (state->config_transaction.phase ==
             UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
         state->rekey_transaction.phase ==
             UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) ||
        (state->tombstone.valid &&
         (!epoch_is_valid(&state->tombstone.retired_epoch) ||
          state->tombstone.replacement_cluster_id == UCN_NODE_BROADCAST ||
          !serial_is_valid(state->tombstone.rekey_transaction_id) ||
          !tombstone_matches_committed_rekey(
              &state->tombstone, &state->rekey_transaction,
              &state->committed_rekey))) ||
        (state->rekey_transaction.phase ==
             UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
         !tombstone_matches_committed_rekey(
             &state->tombstone, &state->rekey_transaction,
             &state->committed_rekey)) ||
        (state->boot_incarnation != 0U &&
         !serial_is_valid(state->boot_incarnation))) {
        return false;
    }
    if (state->last_completed_operation_id == 0U) {
        return true;
    }
    return serial_is_valid(state->last_completed_operation_id) &&
           persist_operation_is_valid(state->last_completed_operation) &&
           state->last_completed_operation_fingerprint != 0U &&
           completed_operation_journal_is_consistent(state);
}

bool ucn_cluster_persist_load_result_is_valid(
    const ucn_cluster_persist_load_result_t *result)
{
    if (result == NULL) {
        return false;
    }
    if (result->state == UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY) {
        return bytes_are_zero((const uint8_t *)&result->snapshot,
                              sizeof(result->snapshot));
    }
    return result->state == UCN_CLUSTER_PERSIST_LOAD_READY &&
           ucn_cluster_persist_state_is_valid(&result->snapshot);
}

static void write_epoch(uint8_t *output, const ucn_cluster_epoch_t *epoch)
{
    write_u32_be(output, epoch->cluster_id);
    write_u32_be(output + 4U, epoch->term);
    write_u32_be(output + 8U, epoch->head_node_id);
}

static void read_epoch(const uint8_t *input, ucn_cluster_epoch_t *epoch)
{
    epoch->cluster_id = read_u32_be(input);
    epoch->term = read_u32_be(input + 4U);
    epoch->head_node_id = read_u32_be(input + 8U);
}

static void write_config_ref(uint8_t *output,
                             const ucn_cluster_persist_config_ref_t *config)
{
    output[0] = config->valid ? 1U : 0U;
    write_u32_be(output + 1U, config->config_id);
    write_u32_be(output + 5U, config->generation);
    (void)memcpy(output + 9U, config->digest, sizeof(config->digest));
}

static bool read_config_ref(const uint8_t *input,
                            ucn_cluster_persist_config_ref_t *config)
{
    if (input[0] > 1U) {
        return false;
    }
    config->valid = input[0] != 0U;
    config->config_id = read_u32_be(input + 1U);
    config->generation = read_u32_be(input + 5U);
    (void)memcpy(config->digest, input + 9U, sizeof(config->digest));
    return true;
}

static void write_rekey_ref(uint8_t *output,
                            const ucn_cluster_persist_rekey_ref_t *rekey)
{
    output[0] = rekey->valid ? 1U : 0U;
    write_u32_be(output + 1U, rekey->generation);
    write_u32_be(output + 5U, rekey->next_incarnation);
    write_epoch(output + 9U, &rekey->predecessor_epoch);
    write_config_ref(output + 21U, &rekey->predecessor_config);
    write_epoch(output + 46U, &rekey->successor_epoch);
}

static bool read_rekey_ref(const uint8_t *input,
                           ucn_cluster_persist_rekey_ref_t *rekey)
{
    if (input[0] > 1U) {
        return false;
    }
    rekey->valid = input[0] != 0U;
    rekey->generation = read_u32_be(input + 1U);
    rekey->next_incarnation = read_u32_be(input + 5U);
    read_epoch(input + 9U, &rekey->predecessor_epoch);
    if (!read_config_ref(input + 21U, &rekey->predecessor_config)) {
        return false;
    }
    read_epoch(input + 46U, &rekey->successor_epoch);
    return true;
}

static uint32_t crc32_bytes(const uint8_t *input, size_t length,
                            size_t zero_offset, size_t zero_length)
{
    uint32_t crc = UINT32_MAX;
    size_t index;

    for (index = 0U; index < length; ++index) {
        uint8_t value = index >= zero_offset &&
                                index < zero_offset + zero_length ?
                            0U : input[index];
        uint8_t bit;

        crc ^= value;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ?
                      (crc >> 1U) ^ UINT32_C(0xEDB88320) : crc >> 1U;
        }
    }
    return crc ^ UINT32_MAX;
}

static uint32_t crc32_record(const uint8_t *record)
{
    return crc32_bytes(record, UCN_CLUSTER_PERSIST_RECORD_BYTES,
                       PERSIST_CRC_OFFSET, 4U);
}

static bool decode_bool(const uint8_t *record, size_t offset, bool *value)
{
    if (record[offset] > 1U) {
        return false;
    }
    *value = record[offset] != 0U;
    return true;
}

static uint32_t request_fingerprint(
    uint8_t operation,
    const ucn_cluster_persist_state_t *state)
{
    uint8_t record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint32_t fingerprint;

    if (ucn_cluster_persist_record_encode(state, 1U, record,
                                          sizeof(record)) != UCN_OK) {
        return 0U;
    }
    fingerprint = crc32_bytes(record, sizeof(record), sizeof(record), 0U);
    fingerprint ^= ((uint32_t)operation * UINT32_C(0x9E3779B1));
    fingerprint ^= fingerprint >> 16U;
    return fingerprint == 0U ? 1U : fingerprint;
}

ucn_result_t ucn_cluster_persist_request_finalize(
    ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_state_t canonical_state;
    uint32_t fingerprint;

    if (request == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!serial_is_valid(request->operation_id) ||
        !persist_operation_is_valid((uint8_t)request->operation)) {
        return UCN_ERR_CONFIG;
    }
    canonical_state = request->next_state;
    canonical_state.last_completed_operation_id = 0U;
    canonical_state.last_completed_operation = 0U;
    canonical_state.last_completed_operation_fingerprint = 0U;
    state_normalize_absent_fields(&canonical_state);
    if (!ucn_cluster_persist_state_is_valid(&canonical_state)) {
        return UCN_ERR_CONFIG;
    }
    fingerprint = request_fingerprint((uint8_t)request->operation,
                                      &canonical_state);
    if (fingerprint == 0U) {
        return UCN_ERR_STATE;
    }
    canonical_state.last_completed_operation_id = request->operation_id;
    canonical_state.last_completed_operation = (uint8_t)request->operation;
    canonical_state.last_completed_operation_fingerprint = fingerprint;
    if (!ucn_cluster_persist_state_is_valid(&canonical_state)) {
        return UCN_ERR_STATE;
    }
    request->next_state = canonical_state;
    return UCN_OK;
}

bool ucn_cluster_persist_request_is_valid(
    const ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_state_t canonical_state;

    if (request == NULL || !serial_is_valid(request->operation_id) ||
        !persist_operation_is_valid((uint8_t)request->operation) ||
        !ucn_cluster_persist_state_is_valid(&request->next_state) ||
        request->next_state.last_completed_operation_id != request->operation_id ||
        request->next_state.last_completed_operation !=
            (uint8_t)request->operation) {
        return false;
    }
    canonical_state = request->next_state;
    canonical_state.last_completed_operation_id = 0U;
    canonical_state.last_completed_operation = 0U;
    canonical_state.last_completed_operation_fingerprint = 0U;
    return ucn_cluster_persist_state_is_valid(&canonical_state) &&
           request_fingerprint((uint8_t)request->operation,
                               &canonical_state) ==
               request->next_state.last_completed_operation_fingerprint;
}

static bool state_canonical_equal(const ucn_cluster_persist_state_t *a,
                                  const ucn_cluster_persist_state_t *b)
{
    uint8_t record_a[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t record_b[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    ucn_cluster_persist_state_t canonical_a;
    ucn_cluster_persist_state_t canonical_b;

    if (a == NULL || b == NULL) {
        return false;
    }
    /* A legacy record becomes v2 on its next accepted write. Schema is
     * migration provenance, not a logical Config/Epoch authority field. */
    canonical_a = *a;
    canonical_b = *b;
    canonical_a.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2;
    canonical_b.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2;

    return ucn_cluster_persist_record_encode(&canonical_a, 1U, record_a,
                                             sizeof(record_a)) == UCN_OK &&
           ucn_cluster_persist_record_encode(&canonical_b, 1U, record_b,
                                             sizeof(record_b)) == UCN_OK &&
           memcmp(record_a, record_b, sizeof(record_a)) == 0;
}

static void state_clear_completed_operation(
    ucn_cluster_persist_state_t *state)
{
    state->last_completed_operation_id = 0U;
    state->last_completed_operation = 0U;
    state->last_completed_operation_fingerprint = 0U;
}

static bool state_equal_ignoring_completed_operation(
    const ucn_cluster_persist_state_t *a,
    const ucn_cluster_persist_state_t *b)
{
    ucn_cluster_persist_state_t canonical_a = *a;
    ucn_cluster_persist_state_t canonical_b = *b;

    state_clear_completed_operation(&canonical_a);
    state_clear_completed_operation(&canonical_b);
    return state_canonical_equal(&canonical_a, &canonical_b);
}

static bool state_has_prepared_transaction(
    const ucn_cluster_persist_state_t *state)
{
    return state->config_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
           state->rekey_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
}

/* A new ordinary-election Cluster has no ordinary authority state inherited
 * from its parent.  A parent VoteId could make the next Cluster look as if it
 * had already acknowledged a takeover, and parent Config cannot become a
 * Config of a different Cluster.  Rekey/Tombstone evidence is intentionally
 * not cleared here: CLUSTER_CREATE rejects such source states before reaching
 * this helper, because Record v1 cannot retain a retired-identity set.  The
 * operation journal and boot incarnation also remain intact. */
static void state_clear_ordinary_cluster_state(ucn_cluster_persist_state_t *state)
{
    (void)memset(&state->last_vote, 0, sizeof(state->last_vote));
    (void)memset(&state->committed_config, 0, sizeof(state->committed_config));
    (void)memset(&state->config_transaction, 0,
                 sizeof(state->config_transaction));
    state->config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
}

static bool config_prepare_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected;

    if (next->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        committed->config_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        committed->rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        next->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        !config_ref_is_strict_successor(
            &committed->committed_config,
            &next->config_transaction.staging_config) ||
        (committed->config_transaction.phase ==
                 UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
         next->config_transaction.transaction_id != 1U) ||
        (committed->config_transaction.phase ==
                 UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
         !serial_is_next(committed->config_transaction.transaction_id,
                         next->config_transaction.transaction_id))) {
        return false;
    }
    expected = *committed;
    expected.config_transaction = next->config_transaction;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool config_commit_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected;

    if (committed->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        next->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        committed->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        committed->rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        next->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED ||
        next->config_transaction.transaction_id !=
            committed->config_transaction.transaction_id ||
        !config_ref_is_equal(&next->committed_config,
                             &committed->config_transaction.staging_config)) {
        return false;
    }
    expected = *committed;
    expected.committed_config = committed->config_transaction.staging_config;
    expected.config_transaction = committed->config_transaction;
    expected.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    return state_equal_ignoring_completed_operation(&expected, next);
}

/* CONFIG_JOINT does not mutate the Config identity.  It is nevertheless a
 * real durable transition because its completed-operation journal is the
 * restart-safe proof that the exact PREPARED transaction reached Joint. */
static bool config_joint_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    if (committed->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        next->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        committed->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        committed->rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        return false;
    }
    return state_equal_ignoring_completed_operation(committed, next);
}

static bool config_abort_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected;

    if (committed->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        next->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        committed->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        next->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED ||
        next->config_transaction.transaction_id !=
            committed->config_transaction.transaction_id) {
        return false;
    }
    expected = *committed;
    expected.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool rekey_prepare_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    const ucn_cluster_persist_rekey_ref_t *staging =
        &next->rekey_transaction.staging_rekey;
    ucn_cluster_persist_state_t expected;

    if (next->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        state_has_prepared_transaction(committed) || committed->tombstone.valid ||
        !committed->has_active_epoch || !committed->committed_config.valid ||
        next->rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        !epoch_is_equal(&staging->predecessor_epoch,
                        &committed->active_epoch) ||
        !config_ref_is_equal(&staging->predecessor_config,
                             &committed->committed_config)) {
        return false;
    }
    expected = *committed;
    expected.rekey_transaction = next->rekey_transaction;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool rekey_commit_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    const ucn_cluster_persist_rekey_ref_t *staging =
        &committed->rekey_transaction.staging_rekey;
    ucn_cluster_persist_state_t expected;

    if (committed->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        next->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        committed->config_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        committed->rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        next->rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED ||
        next->rekey_transaction.transaction_id !=
            committed->rekey_transaction.transaction_id ||
        !rekey_ref_is_equal(&next->committed_rekey, staging) ||
        !next->has_active_epoch || !next->has_max_epoch ||
        !epoch_is_equal(&next->active_epoch, &staging->successor_epoch) ||
        !epoch_is_equal(&next->max_epoch, &staging->successor_epoch) ||
        !next->tombstone.valid ||
        next->tombstone.rekey_transaction_id !=
            committed->rekey_transaction.transaction_id ||
        !epoch_is_equal(&next->tombstone.retired_epoch,
                        &staging->predecessor_epoch) ||
        next->tombstone.replacement_cluster_id !=
            staging->successor_epoch.cluster_id) {
        return false;
    }
    expected = *committed;
    expected.has_active_epoch = true;
    expected.active_epoch = staging->successor_epoch;
    expected.has_max_epoch = true;
    expected.max_epoch = staging->successor_epoch;
    expected.committed_rekey = *staging;
    expected.rekey_transaction = next->rekey_transaction;
    expected.tombstone = next->tombstone;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool vote_commit_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected;

    if (state_has_prepared_transaction(committed) || !committed->has_active_epoch ||
        !next->last_vote.valid ||
        !epoch_is_equal(&next->last_vote.epoch, &committed->active_epoch) ||
        (committed->last_vote.valid &&
         epoch_is_equal(&committed->last_vote.epoch,
                        &committed->active_epoch))) {
        return false;
    }
    expected = *committed;
    expected.last_vote = next->last_vote;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool epoch_commit_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected;

    if (state_has_prepared_transaction(committed) ||
        !serial_is_valid(committed->boot_incarnation) ||
        !committed->has_active_epoch ||
        !next->has_active_epoch || !next->has_max_epoch ||
        !epoch_is_equal(&next->active_epoch, &next->max_epoch) ||
        (committed->has_active_epoch != committed->has_max_epoch) ||
        (committed->has_active_epoch &&
         (!epoch_is_equal(&committed->active_epoch, &committed->max_epoch) ||
          next->active_epoch.cluster_id !=
              committed->active_epoch.cluster_id ||
          !serial_is_next(committed->max_epoch.term,
                          next->active_epoch.term))) ||
        (!committed->has_active_epoch && committed->has_max_epoch)) {
        return false;
    }
    expected = *committed;
    expected.has_active_epoch = next->has_active_epoch;
    expected.active_epoch = next->active_epoch;
    expected.has_max_epoch = next->has_max_epoch;
    expected.max_epoch = next->max_epoch;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool cluster_create_commit_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    const ucn_cluster_epoch_t *parent = NULL;
    ucn_cluster_persist_state_t expected;

    if (state_has_prepared_transaction(committed) ||
        committed->committed_rekey.valid || committed->tombstone.valid ||
        committed->rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_NONE ||
        !serial_is_valid(committed->boot_incarnation) ||
        !next->has_active_epoch || !next->has_max_epoch ||
        !epoch_is_equal(&next->active_epoch, &next->max_epoch) ||
        next->active_epoch.term != 1U) {
        return false;
    }
    if (committed->has_active_epoch) {
        parent = &committed->active_epoch;
    } else if (committed->has_max_epoch) {
        parent = &committed->max_epoch;
    }
    if (parent != NULL && next->active_epoch.cluster_id == parent->cluster_id) {
        return false;
    }

    expected = *committed;
    state_clear_ordinary_cluster_state(&expected);
    expected.has_active_epoch = true;
    expected.active_epoch = next->active_epoch;
    expected.has_max_epoch = true;
    expected.max_epoch = next->max_epoch;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool replay_incarnation_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected = *committed;

    if (state_has_prepared_transaction(committed) ||
        !serial_is_valid(next->boot_incarnation) ||
        next->boot_incarnation <= committed->boot_incarnation) {
        return false;
    }
    expected.boot_incarnation = next->boot_incarnation;
    return state_equal_ignoring_completed_operation(&expected, next);
}

/* M04 R23 legacy Record-v1 migration.  Earlier M04 work deliberately made
 * PREPARED a complete, canonical durable snapshot.  Before M07/M13 supplies
 * a resume/commit protocol, such a snapshot cannot remain live across a
 * controlled reboot.  This operation is therefore intentionally narrower
 * than a general abort: it clears exactly the old PREPARED transaction and
 * creates a fresh replay domain in the same atomic Record write. */
static bool legacy_prepared_abort_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected = *committed;

    if (committed->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1 ||
        next->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        !state_has_prepared_transaction(committed) ||
        !serial_is_valid(next->boot_incarnation) ||
        next->boot_incarnation <= committed->boot_incarnation) {
        return false;
    }
    if (expected.config_transaction.phase ==
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        (void)memset(&expected.config_transaction, 0,
                     sizeof(expected.config_transaction));
        expected.config_transaction.phase =
            UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    }
    if (expected.rekey_transaction.phase ==
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        (void)memset(&expected.rekey_transaction, 0,
                     sizeof(expected.rekey_transaction));
        expected.rekey_transaction.phase =
            UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    }
    expected.boot_incarnation = next->boot_incarnation;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool request_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_request_t *request)
{
    /* Incarnation allocation is deliberately isolated.  No authority/config
     * transition may smuggle a lower (or merely different) replay domain into
     * an otherwise valid next snapshot.  A factory-empty store must therefore
     * first commit REPLAY_INCARNATION before it can create a Cluster. */
    if (request->operation != UCN_CLUSTER_PERSIST_OPERATION_REPLAY_INCARNATION &&
        request->operation !=
            UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT &&
        (!serial_is_valid(committed->boot_incarnation) ||
         request->next_state.boot_incarnation != committed->boot_incarnation)) {
        return false;
    }
    switch (request->operation) {
    case UCN_CLUSTER_PERSIST_OPERATION_REPLAY_INCARNATION:
        return replay_incarnation_transition_is_valid(committed,
                                                      &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT:
        return legacy_prepared_abort_transition_is_valid(committed,
                                                          &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT:
        return epoch_commit_transition_is_valid(committed, &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_CLUSTER_CREATE_COMMIT:
        return cluster_create_commit_transition_is_valid(committed,
                                                         &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT:
        return vote_commit_transition_is_valid(committed, &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE:
        return config_prepare_transition_is_valid(committed,
                                                  &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT:
        return config_commit_transition_is_valid(committed,
                                                 &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT:
        return config_abort_transition_is_valid(committed,
                                                &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT:
        return config_joint_transition_is_valid(committed,
                                                &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_REKEY_PREPARE:
        return rekey_prepare_transition_is_valid(committed,
                                                 &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_REKEY_COMMIT:
        return rekey_commit_transition_is_valid(committed,
                                                &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_TOMBSTONE_COMMIT:
    default:
        /* Tombstone is only legal as part of REKEY_COMMIT's atomic next
         * snapshot; a standalone Tombstone operation is intentionally
         * fail-closed. */
        return false;
    }
}

ucn_cluster_persist_request_admission_t ucn_cluster_persist_request_admit(
    const ucn_cluster_persist_state_t *committed_state,
    const ucn_cluster_persist_request_t *request)
{
    if (!ucn_cluster_persist_state_is_valid(committed_state) ||
        !ucn_cluster_persist_request_is_valid(request)) {
        return UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_INVALID;
    }
    if (committed_state->last_completed_operation_id == 0U) {
        return request_transition_is_valid(committed_state, request) ?
                   UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW :
                   UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED;
    }
    if (request->operation_id == committed_state->last_completed_operation_id) {
        return request->next_state.last_completed_operation ==
                   committed_state->last_completed_operation &&
               request->next_state.last_completed_operation_fingerprint ==
                   committed_state->last_completed_operation_fingerprint &&
               state_canonical_equal(&request->next_state, committed_state) ?
                   UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_IDEMPOTENT :
                   UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED;
    }
    if (request->operation_id < committed_state->last_completed_operation_id) {
        return UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED;
    }
    return request_transition_is_valid(committed_state, request) ?
               UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW :
               UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED;
}

bool ucn_cluster_persist_record_is_factory_empty(
    const uint8_t *record,
    size_t record_length)
{
    bool all_zero = true;
    bool all_ff = true;
    size_t index;

    if (record == NULL || record_length != UCN_CLUSTER_PERSIST_RECORD_BYTES) {
        return false;
    }
    for (index = 0U; index < record_length; ++index) {
        if (record[index] != 0U) {
            all_zero = false;
        }
        if (record[index] != UINT8_MAX) {
            all_ff = false;
        }
    }
    return all_zero || all_ff;
}

ucn_result_t ucn_cluster_persist_record_generation_next(
    uint32_t current_generation,
    uint32_t *next_generation)
{
    if (next_generation == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (current_generation == UINT32_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *next_generation = current_generation + 1U;
    return UCN_OK;
}

bool ucn_cluster_persist_record_generation_is_newer(
    uint32_t candidate_generation,
    uint32_t baseline_generation)
{
    return candidate_generation != 0U && candidate_generation > baseline_generation;
}

ucn_result_t ucn_cluster_persist_record_encode(
    const ucn_cluster_persist_state_t *state,
    uint32_t record_generation,
    uint8_t *output,
    size_t output_capacity)
{
    uint32_t crc;

    if (state == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (output_capacity < UCN_CLUSTER_PERSIST_RECORD_BYTES) {
        return UCN_ERR_NO_SPACE;
    }
    if (record_generation == 0U ||
        state->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 ||
        !ucn_cluster_persist_state_is_valid(state)) {
        return UCN_ERR_CONFIG;
    }
    (void)memset(output, 0, UCN_CLUSTER_PERSIST_RECORD_BYTES);
    write_u32_be(output + PERSIST_MAGIC_OFFSET, UCN_CLUSTER_PERSIST_RECORD_MAGIC);
    write_u16_be(output + PERSIST_SCHEMA_OFFSET, state->record_schema_version);
    write_u16_be(output + PERSIST_SIZE_OFFSET,
                 (uint16_t)UCN_CLUSTER_PERSIST_RECORD_BYTES);
    write_u32_be(output + PERSIST_GENERATION_OFFSET, record_generation);
    output[PERSIST_ACTIVE_VALID_OFFSET] = state->has_active_epoch ? 1U : 0U;
    if (state->has_active_epoch) {
        write_epoch(output + PERSIST_ACTIVE_EPOCH_OFFSET, &state->active_epoch);
    }
    output[PERSIST_MAX_VALID_OFFSET] = state->has_max_epoch ? 1U : 0U;
    if (state->has_max_epoch) {
        write_epoch(output + PERSIST_MAX_EPOCH_OFFSET, &state->max_epoch);
    }
    output[PERSIST_VOTE_VALID_OFFSET] = state->last_vote.valid ? 1U : 0U;
    if (state->last_vote.valid) {
        write_epoch(output + PERSIST_VOTE_EPOCH_OFFSET,
                    &state->last_vote.epoch);
        write_u32_be(output + PERSIST_VOTE_FOR_OFFSET,
                     state->last_vote.voted_for_node_id);
        write_u32_be(output + PERSIST_VOTE_GENERATION_OFFSET,
                     state->last_vote.backup_generation);
    }
    if (state->committed_config.valid) {
        write_config_ref(output + PERSIST_CONFIG_VALID_OFFSET,
                         &state->committed_config);
    }
    output[PERSIST_CONFIG_TRANSACTION_PHASE_OFFSET] =
        (uint8_t)state->config_transaction.phase;
    if (state->config_transaction.phase != UCN_CLUSTER_PERSIST_TRANSACTION_NONE) {
        write_u32_be(output + PERSIST_CONFIG_TRANSACTION_ID_OFFSET,
                     state->config_transaction.transaction_id);
        if (state->config_transaction.phase ==
                UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
            state->config_transaction.phase ==
                UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED) {
            write_config_ref(output + PERSIST_CONFIG_STAGING_VALID_OFFSET,
                             &state->config_transaction.staging_config);
        }
    }
    if (state->committed_rekey.valid) {
        write_rekey_ref(output + PERSIST_REKEY_VALID_OFFSET,
                        &state->committed_rekey);
    }
    output[PERSIST_REKEY_TRANSACTION_PHASE_OFFSET] =
        (uint8_t)state->rekey_transaction.phase;
    if (state->rekey_transaction.phase != UCN_CLUSTER_PERSIST_TRANSACTION_NONE) {
        write_u32_be(output + PERSIST_REKEY_TRANSACTION_ID_OFFSET,
                     state->rekey_transaction.transaction_id);
        if (state->rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
            write_rekey_ref(output + PERSIST_REKEY_STAGING_VALID_OFFSET,
                            &state->rekey_transaction.staging_rekey);
        }
    }
    output[PERSIST_TOMBSTONE_VALID_OFFSET] = state->tombstone.valid ? 1U : 0U;
    if (state->tombstone.valid) {
        write_epoch(output + PERSIST_TOMBSTONE_EPOCH_OFFSET,
                    &state->tombstone.retired_epoch);
        write_u32_be(output + PERSIST_TOMBSTONE_REPLACEMENT_OFFSET,
                     state->tombstone.replacement_cluster_id);
        write_u32_be(output + PERSIST_TOMBSTONE_REKEY_TRANSACTION_ID_OFFSET,
                     state->tombstone.rekey_transaction_id);
    }
    write_u32_be(output + PERSIST_BOOT_INCARNATION_OFFSET,
                 state->boot_incarnation);
    write_u32_be(output + PERSIST_LAST_OPERATION_ID_OFFSET,
                 state->last_completed_operation_id);
    output[PERSIST_LAST_OPERATION_KIND_OFFSET] =
        state->last_completed_operation;
    write_u32_be(output + PERSIST_LAST_OPERATION_FINGERPRINT_OFFSET,
                 state->last_completed_operation_fingerprint);
    crc = crc32_record(output);
    write_u32_be(output + PERSIST_CRC_OFFSET, crc);
    return UCN_OK;
}

static bool decode_transaction_phase(
    uint8_t value,
    ucn_cluster_persist_transaction_phase_t *phase)
{
    if (value != UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
        value != UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
        value != UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED) {
        return false;
    }
    *phase = (ucn_cluster_persist_transaction_phase_t)value;
    return true;
}

ucn_result_t ucn_cluster_persist_record_decode(
    const uint8_t *record,
    size_t record_length,
    uint32_t *record_generation,
    ucn_cluster_persist_state_t *state)
{
    ucn_cluster_persist_state_t decoded;
    uint32_t stored_crc;

    if (record == NULL || record_generation == NULL || state == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (record_length != UCN_CLUSTER_PERSIST_RECORD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    if (ucn_cluster_persist_record_is_factory_empty(record, record_length)) {
        return UCN_ERR_NOT_FOUND;
    }
    if (read_u32_be(record + PERSIST_MAGIC_OFFSET) !=
        UCN_CLUSTER_PERSIST_RECORD_MAGIC) {
        return UCN_ERR_MALFORMED;
    }
    if (!record_schema_version_is_valid(
            read_u16_be(record + PERSIST_SCHEMA_OFFSET))) {
        return UCN_ERR_VERSION;
    }
    if (read_u16_be(record + PERSIST_SIZE_OFFSET) !=
        UCN_CLUSTER_PERSIST_RECORD_BYTES ||
        read_u32_be(record + PERSIST_GENERATION_OFFSET) == 0U) {
        return UCN_ERR_MALFORMED;
    }
    stored_crc = read_u32_be(record + PERSIST_CRC_OFFSET);
    if (stored_crc != crc32_record(record)) {
        return UCN_ERR_CRC;
    }
    if (!bytes_are_zero(record + PERSIST_RESERVED_OFFSET,
                        PERSIST_RESERVED_BYTES)) {
        return UCN_ERR_MALFORMED;
    }
    ucn_cluster_persist_state_init_empty(&decoded);
    decoded.record_schema_version = read_u16_be(record + PERSIST_SCHEMA_OFFSET);
    if (!decode_bool(record, PERSIST_ACTIVE_VALID_OFFSET,
                     &decoded.has_active_epoch) ||
        !decode_bool(record, PERSIST_MAX_VALID_OFFSET,
                     &decoded.has_max_epoch) ||
        !decode_bool(record, PERSIST_VOTE_VALID_OFFSET,
                     &decoded.last_vote.valid) ||
        !read_config_ref(record + PERSIST_CONFIG_VALID_OFFSET,
                         &decoded.committed_config) ||
        !decode_transaction_phase(
            record[PERSIST_CONFIG_TRANSACTION_PHASE_OFFSET],
            &decoded.config_transaction.phase) ||
        !read_rekey_ref(record + PERSIST_REKEY_VALID_OFFSET,
                        &decoded.committed_rekey) ||
        !decode_transaction_phase(
            record[PERSIST_REKEY_TRANSACTION_PHASE_OFFSET],
            &decoded.rekey_transaction.phase) ||
        !decode_bool(record, PERSIST_TOMBSTONE_VALID_OFFSET,
                     &decoded.tombstone.valid)) {
        return UCN_ERR_MALFORMED;
    }
    if (decoded.has_active_epoch) {
        read_epoch(record + PERSIST_ACTIVE_EPOCH_OFFSET, &decoded.active_epoch);
    } else if (!bytes_are_zero(record + PERSIST_ACTIVE_EPOCH_OFFSET, 12U)) {
        return UCN_ERR_MALFORMED;
    }
    if (decoded.has_max_epoch) {
        read_epoch(record + PERSIST_MAX_EPOCH_OFFSET, &decoded.max_epoch);
    } else if (!bytes_are_zero(record + PERSIST_MAX_EPOCH_OFFSET, 12U)) {
        return UCN_ERR_MALFORMED;
    }
    if (decoded.last_vote.valid) {
        read_epoch(record + PERSIST_VOTE_EPOCH_OFFSET, &decoded.last_vote.epoch);
        decoded.last_vote.voted_for_node_id =
            read_u32_be(record + PERSIST_VOTE_FOR_OFFSET);
        decoded.last_vote.backup_generation =
            read_u32_be(record + PERSIST_VOTE_GENERATION_OFFSET);
    } else if (!bytes_are_zero(record + PERSIST_VOTE_EPOCH_OFFSET, 20U)) {
        return UCN_ERR_MALFORMED;
    }
    if (!decoded.committed_config.valid &&
        !bytes_are_zero(record + PERSIST_CONFIG_ID_OFFSET, 24U)) {
        return UCN_ERR_MALFORMED;
    }
    if (decoded.config_transaction.phase ==
        UCN_CLUSTER_PERSIST_TRANSACTION_NONE) {
        if (!bytes_are_zero(record + PERSIST_CONFIG_TRANSACTION_ID_OFFSET,
                            29U)) {
            return UCN_ERR_MALFORMED;
        }
    } else {
        decoded.config_transaction.transaction_id =
            read_u32_be(record + PERSIST_CONFIG_TRANSACTION_ID_OFFSET);
        if (decoded.config_transaction.phase ==
                UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
            decoded.config_transaction.phase ==
                UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED) {
            if (!read_config_ref(record + PERSIST_CONFIG_STAGING_VALID_OFFSET,
                                 &decoded.config_transaction.staging_config)) {
                return UCN_ERR_MALFORMED;
            }
        } else if (!bytes_are_zero(record + PERSIST_CONFIG_STAGING_VALID_OFFSET,
                                   25U)) {
            return UCN_ERR_MALFORMED;
        }
    }
    if (!decoded.committed_rekey.valid &&
        !bytes_are_zero(record + PERSIST_REKEY_GENERATION_OFFSET, 57U)) {
        return UCN_ERR_MALFORMED;
    }
    if (decoded.rekey_transaction.phase ==
        UCN_CLUSTER_PERSIST_TRANSACTION_NONE) {
        if (!bytes_are_zero(record + PERSIST_REKEY_TRANSACTION_ID_OFFSET,
                            62U)) {
            return UCN_ERR_MALFORMED;
        }
    } else {
        decoded.rekey_transaction.transaction_id =
            read_u32_be(record + PERSIST_REKEY_TRANSACTION_ID_OFFSET);
        if (decoded.rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
            if (!read_rekey_ref(record + PERSIST_REKEY_STAGING_VALID_OFFSET,
                                &decoded.rekey_transaction.staging_rekey)) {
                return UCN_ERR_MALFORMED;
            }
        } else if (!bytes_are_zero(record + PERSIST_REKEY_STAGING_VALID_OFFSET,
                                   58U)) {
            return UCN_ERR_MALFORMED;
        }
    }
    if (decoded.tombstone.valid) {
        read_epoch(record + PERSIST_TOMBSTONE_EPOCH_OFFSET,
                   &decoded.tombstone.retired_epoch);
        decoded.tombstone.replacement_cluster_id =
            read_u32_be(record + PERSIST_TOMBSTONE_REPLACEMENT_OFFSET);
        decoded.tombstone.rekey_transaction_id =
            read_u32_be(record + PERSIST_TOMBSTONE_REKEY_TRANSACTION_ID_OFFSET);
    } else if (!bytes_are_zero(record + PERSIST_TOMBSTONE_EPOCH_OFFSET, 20U)) {
        return UCN_ERR_MALFORMED;
    }
    decoded.boot_incarnation =
        read_u32_be(record + PERSIST_BOOT_INCARNATION_OFFSET);
    decoded.last_completed_operation_id =
        read_u32_be(record + PERSIST_LAST_OPERATION_ID_OFFSET);
    decoded.last_completed_operation =
        record[PERSIST_LAST_OPERATION_KIND_OFFSET];
    decoded.last_completed_operation_fingerprint =
        read_u32_be(record + PERSIST_LAST_OPERATION_FINGERPRINT_OFFSET);
    if (!ucn_cluster_persist_state_is_valid(&decoded)) {
        return UCN_ERR_MALFORMED;
    }
    *record_generation = read_u32_be(record + PERSIST_GENERATION_OFFSET);
    *state = decoded;
    return UCN_OK;
}
