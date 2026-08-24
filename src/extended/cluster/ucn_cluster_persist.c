#include <string.h>

#include "ucn/ucn_cluster_persist.h"

#if defined(_MSC_VER)
#define UCN_CLUSTER_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define UCN_CLUSTER_NOINLINE __attribute__((noinline))
#else
#define UCN_CLUSTER_NOINLINE
#endif

/* Record v1/v2/v3/v4 are deliberately explicit byte layouts. Do not replace
 * these offsets with a packed C struct: persistence must remain
 * ABI-independent. v3 appends M10 VoteId fields after the v1/v2 280 B body;
 * v4 appends the exact M13 successor Config reference plus explicit
 * Stable/Recovery scope and Recovery retirement evidence after v3. */
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
    PERSIST_RESERVED_BYTES = 7U,
    PERSIST_V3_VOTE_PROPOSED_TERM_OFFSET = 280U,
    PERSIST_V3_VOTE_CONFIG_ID_OFFSET = 284U,
    PERSIST_V3_VOTE_SNAPSHOT_ID_OFFSET = 288U,
    PERSIST_V3_EXTENSION_BYTES = 12U,
    PERSIST_V4_REKEY_SUCCESSOR_CONFIG_OFFSET = 292U,
    PERSIST_V4_REKEY_SUCCESSOR_CONFIG_BYTES = 25U,
    PERSIST_V4_REKEY_PREPARE_NONCE_OFFSET = 317U,
    PERSIST_V4_REKEY_SUCCESSOR_BACKUP_OFFSET = 321U,
    PERSIST_V4_EPOCH_SCOPE_OFFSET = 325U,
    PERSIST_V4_RECOVERY_VALID_OFFSET = 326U,
    PERSIST_V4_RECOVERY_EPOCH_OFFSET = 327U,
    PERSIST_V4_RECOVERY_PARENT_CLUSTER_OFFSET = 339U,
    PERSIST_V4_RECOVERY_PARENT_TERM_OFFSET = 343U,
    PERSIST_V4_RECOVERY_PARENT_CONFIG_OFFSET = 347U,
    PERSIST_V4_RECOVERY_ROUND_OFFSET = 351U,
    PERSIST_V4_RECOVERY_NONCE_OFFSET = 355U,
    PERSIST_V4_RECOVERY_CLUSTER_ID_ROUND_OFFSET = 359U,
    PERSIST_V4_RECOVERY_TOMBSTONE_VALID_OFFSET = 363U,
    PERSIST_V4_RECOVERY_TOMBSTONE_EPOCH_OFFSET = 364U,
    PERSIST_V4_RECOVERY_TOMBSTONE_REPLACEMENT_OFFSET = 376U,
    PERSIST_V4_RECOVERY_TOMBSTONE_ROUND_OFFSET = 380U,
    PERSIST_V4_REKEY_HISTORY_FINGERPRINT_OFFSET = 384U,
    PERSIST_V4_EXTENSION_BYTES = 96U
};

typedef char ucn_cluster_persist_record_legacy_layout_must_be_280_bytes[
    PERSIST_RESERVED_OFFSET + PERSIST_RESERVED_BYTES ==
            UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES ? 1 : -1];
typedef char ucn_cluster_persist_record_v3_layout_must_be_292_bytes[
    PERSIST_V3_VOTE_PROPOSED_TERM_OFFSET + PERSIST_V3_EXTENSION_BYTES ==
            UCN_CLUSTER_PERSIST_RECORD_V3_BYTES ? 1 : -1];
typedef char ucn_cluster_persist_record_v4_layout_must_be_388_bytes[
    PERSIST_V4_REKEY_SUCCESSOR_CONFIG_OFFSET + PERSIST_V4_EXTENSION_BYTES ==
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

static bool bytes_are_value(const uint8_t *input, size_t length, uint8_t value)
{
    size_t index;

    if (input == NULL) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (input[index] != value) {
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
           version == UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V2 ||
           version == UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3 ||
           version == UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4;
}

static size_t record_bytes_for_schema(uint16_t schema_version)
{
    if (schema_version ==
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4) {
        return UCN_CLUSTER_PERSIST_RECORD_BYTES;
    }
    return schema_version ==
                   UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3 ?
               UCN_CLUSTER_PERSIST_RECORD_V3_BYTES :
               UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES;
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

bool ucn_cluster_persist_vote_is_complete_takeover(
    const ucn_cluster_persist_vote_t *vote)
{
    return vote != NULL && vote->valid && epoch_is_valid(&vote->epoch) &&
           node_id_is_valid(vote->voted_for_node_id) &&
           serial_is_valid(vote->backup_generation) &&
           serial_is_next(vote->epoch.term, vote->proposed_term) &&
           serial_is_valid(vote->config_id) && serial_is_valid(vote->snapshot_id);
}

static bool vote_is_partial_legacy(const ucn_cluster_persist_vote_t *vote)
{
    return vote != NULL && vote->valid && epoch_is_valid(&vote->epoch) &&
           node_id_is_valid(vote->voted_for_node_id) &&
           serial_is_valid(vote->backup_generation) && vote->proposed_term == 0U &&
           vote->config_id == 0U && vote->snapshot_id == 0U;
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
    const ucn_cluster_persist_rekey_ref_t *rekey,
    bool require_successor_config)
{
    return rekey != NULL && rekey->valid && serial_is_valid(rekey->generation) &&
           serial_is_valid(rekey->next_incarnation) &&
           (!require_successor_config ||
             (serial_is_valid(rekey->prepare_nonce) &&
              rekey->allocation_history_fingerprint != 0U)) &&
           (rekey->successor_backup_node_id == 0U ||
            (rekey->successor_backup_node_id != UCN_NODE_BROADCAST &&
             rekey->successor_backup_node_id !=
                 rekey->successor_epoch.head_node_id)) &&
           epoch_is_valid(&rekey->predecessor_epoch) &&
           config_ref_is_present_valid(&rekey->predecessor_config) &&
           epoch_is_valid(&rekey->successor_epoch) &&
           rekey->successor_epoch.term == 1U &&
           rekey->successor_epoch.cluster_id !=
               rekey->predecessor_epoch.cluster_id &&
           (!require_successor_config ||
            config_ref_is_present_valid(&rekey->successor_config));
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
            a->prepare_nonce == b->prepare_nonce &&
            a->allocation_history_fingerprint ==
                b->allocation_history_fingerprint &&
            a->successor_backup_node_id == b->successor_backup_node_id &&
            epoch_is_equal(&a->predecessor_epoch, &b->predecessor_epoch) &&
            config_ref_is_equal(&a->predecessor_config,
                                &b->predecessor_config) &&
            epoch_is_equal(&a->successor_epoch, &b->successor_epoch) &&
            config_ref_is_equal(&a->successor_config,
                                &b->successor_config));
}

static bool persist_operation_is_valid(uint8_t operation)
{
    return (operation >= UCN_CLUSTER_PERSIST_OPERATION_REPLAY_INCARNATION &&
            operation <= UCN_CLUSTER_PERSIST_OPERATION_CLUSTER_CREATE_COMMIT) ||
            operation == UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT ||
            operation == UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT ||
            operation == UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT ||
            operation == UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_VOTE_COMMIT ||
            operation == UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_EPOCH_COMMIT ||
            operation == UCN_CLUSTER_PERSIST_OPERATION_RECOVERY_CREATE_COMMIT ||
            operation == UCN_CLUSTER_PERSIST_OPERATION_REKEY_ABORT;
}

static bool transaction_phase_is_valid(
    ucn_cluster_persist_transaction_phase_t phase)
{
    return phase == UCN_CLUSTER_PERSIST_TRANSACTION_NONE ||
           phase == UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
           phase == UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED ||
           phase == UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED;
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
    if (transaction->phase == UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED) {
        return false;
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
    const ucn_cluster_persist_rekey_ref_t *committed,
    bool require_successor_config)
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
        return rekey_ref_is_present_valid(&transaction->staging_rekey,
                                          require_successor_config);
    }
    if (transaction->phase == UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED) {
        return rekey_ref_is_absent(committed) &&
               rekey_ref_is_present_valid(&transaction->staging_rekey,
                                          require_successor_config);
    }
    return rekey_ref_is_absent(&transaction->staging_rekey) &&
           rekey_ref_is_present_valid(committed, require_successor_config);
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

static bool recovery_identity_is_absent(
    const ucn_cluster_persist_recovery_identity_t *identity)
{
    ucn_cluster_persist_recovery_identity_t zero;

    (void)memset(&zero, 0, sizeof(zero));
    return identity != NULL && memcmp(identity, &zero, sizeof(zero)) == 0;
}

static bool recovery_identity_is_valid(
    const ucn_cluster_persist_recovery_identity_t *identity)
{
    bool parent_valid;

    if (identity == NULL || !identity->valid ||
        !epoch_is_valid(&identity->epoch) ||
        identity->recovery_round > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        !serial_is_valid(identity->recovery_nonce) ||
        !serial_is_valid(identity->cluster_id_round)) {
        return false;
    }
    parent_valid = identity->parent_cluster_id != 0U &&
                   identity->parent_cluster_id != UCN_NODE_BROADCAST &&
                   serial_is_valid(identity->parent_term) &&
                   identity->parent_config_id <=
                       UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD &&
                   identity->parent_cluster_id != identity->epoch.cluster_id;
    return parent_valid ||
           (identity->parent_cluster_id == 0U && identity->parent_term == 0U &&
            identity->parent_config_id == 0U);
}

static bool recovery_tombstone_is_absent(
    const ucn_cluster_persist_recovery_tombstone_t *tombstone)
{
    ucn_cluster_persist_recovery_tombstone_t zero;

    (void)memset(&zero, 0, sizeof(zero));
    return tombstone != NULL && memcmp(tombstone, &zero, sizeof(zero)) == 0;
}

static bool recovery_tombstone_is_valid(
    const ucn_cluster_persist_recovery_tombstone_t *tombstone)
{
    return tombstone != NULL && tombstone->valid &&
           epoch_is_valid(&tombstone->retired_epoch) &&
           tombstone->replacement_cluster_id != 0U &&
           tombstone->replacement_cluster_id != UCN_NODE_BROADCAST &&
           tombstone->replacement_cluster_id !=
               tombstone->retired_epoch.cluster_id &&
           tombstone->recovery_round <=
               UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
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
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
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
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
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
        state->last_vote.proposed_term = 0U;
        state->last_vote.config_id = 0U;
        state->last_vote.snapshot_id = 0U;
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
    if (!state->recovery_identity.valid) {
        (void)memset(&state->recovery_identity, 0,
                     sizeof(state->recovery_identity));
    }
    if (!state->recovery_tombstone.valid) {
        (void)memset(&state->recovery_tombstone, 0,
                     sizeof(state->recovery_tombstone));
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
        (state->record_schema_version >=
             UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4 &&
         state->epoch_scope != UCN_CLUSTER_PERSIST_EPOCH_SCOPE_STABLE &&
         state->epoch_scope != UCN_CLUSTER_PERSIST_EPOCH_SCOPE_RECOVERY) ||
        (state->record_schema_version >=
             UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4 &&
         state->epoch_scope == UCN_CLUSTER_PERSIST_EPOCH_SCOPE_STABLE &&
         !recovery_identity_is_absent(&state->recovery_identity)) ||
        (state->record_schema_version >=
             UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4 &&
         state->epoch_scope == UCN_CLUSTER_PERSIST_EPOCH_SCOPE_RECOVERY &&
         (!state->has_active_epoch ||
          !recovery_identity_is_valid(&state->recovery_identity) ||
          !epoch_is_equal(&state->active_epoch,
                          &state->recovery_identity.epoch))) ||
        (state->recovery_tombstone.valid &&
         !recovery_tombstone_is_valid(&state->recovery_tombstone)) ||
        (!state->recovery_tombstone.valid &&
         !recovery_tombstone_is_absent(&state->recovery_tombstone)) ||
        (state->last_vote.valid &&
         !(vote_is_partial_legacy(&state->last_vote) ||
           ucn_cluster_persist_vote_is_complete_takeover(&state->last_vote))) ||
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
         !rekey_ref_is_present_valid(
             &state->committed_rekey,
             state->record_schema_version >=
                 UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4)) ||
        !rekey_transaction_is_valid(&state->rekey_transaction,
                                    &state->committed_rekey,
                                    state->record_schema_version >=
                                        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4) ||
        (state->record_schema_version >=
             UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4 &&
         state->rekey_transaction.phase ==
             UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
         (!state->has_active_epoch ||
          !epoch_is_equal(
              &state->active_epoch,
              &state->rekey_transaction.staging_rekey.predecessor_epoch) ||
          !config_ref_is_equal(
              &state->committed_config,
              &state->rekey_transaction.staging_rekey.predecessor_config) ||
          !serial_is_next(
              state->boot_incarnation,
              state->rekey_transaction.staging_rekey.next_incarnation))) ||
        (state->record_schema_version >=
             UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4 &&
         state->committed_rekey.valid &&
         (!state->has_active_epoch ||
          !epoch_is_equal(&state->active_epoch,
                          &state->committed_rekey.successor_epoch) ||
          !config_ref_is_equal(&state->committed_config,
                               &state->committed_rekey.successor_config) ||
          state->boot_incarnation !=
              state->committed_rekey.next_incarnation)) ||
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

static uint32_t crc32_record(const uint8_t *record, size_t record_length)
{
    return crc32_bytes(record, record_length,
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

    ucn_cluster_persist_state_t canonical_state;

    if (state == NULL) {
        return 0U;
    }
    canonical_state = *state;
    canonical_state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    if (ucn_cluster_persist_record_encode(&canonical_state, 1U, record,
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

static UCN_CLUSTER_NOINLINE bool state_canonical_equal(
    const ucn_cluster_persist_state_t *a,
    const ucn_cluster_persist_state_t *b)
{
    uint8_t record_a[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t record_b[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    ucn_cluster_persist_state_t canonical_a;
    ucn_cluster_persist_state_t canonical_b;

    if (a == NULL || b == NULL) {
        return false;
    }
    /* A legacy record becomes the current writer schema on its next accepted
     * write. Schema is
     * migration provenance, not a logical Config/Epoch authority field. */
    canonical_a = *a;
    canonical_b = *b;
    canonical_a.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    canonical_b.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;

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

static UCN_CLUSTER_NOINLINE bool state_equal_ignoring_completed_operation(
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

    if (next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
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

    if (committed->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
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
    if (committed->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
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

    if (committed->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
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

    if (next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        state_has_prepared_transaction(committed) || committed->tombstone.valid ||
        !committed->has_active_epoch || !committed->committed_config.valid ||
        next->rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        !epoch_is_equal(&staging->predecessor_epoch,
                        &committed->active_epoch) ||
        !config_ref_is_equal(&staging->predecessor_config,
                             &committed->committed_config) ||
        !config_ref_is_present_valid(&staging->successor_config) ||
        staging->successor_config.config_id != 1U ||
        staging->successor_config.generation != 1U ||
        (committed->rekey_transaction.phase !=
             UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
         committed->rekey_transaction.phase !=
             UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED) ||
        (committed->rekey_transaction.phase ==
             UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED &&
         next->rekey_transaction.transaction_id <=
             committed->rekey_transaction.transaction_id)) {
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

    if (committed->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        committed->config_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        next->config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_NONE ||
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
        !config_ref_is_equal(&next->committed_config,
                             &staging->successor_config) ||
        !serial_is_next(committed->boot_incarnation,
                        staging->next_incarnation) ||
        next->boot_incarnation != staging->next_incarnation ||
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
    expected.committed_config = staging->successor_config;
    expected.boot_incarnation = staging->next_incarnation;
    (void)memset(&expected.last_vote, 0, sizeof(expected.last_vote));
    (void)memset(&expected.config_transaction, 0,
                 sizeof(expected.config_transaction));
    expected.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
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
        !vote_is_partial_legacy(&next->last_vote) ||
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

/* M10 has a separate operation class because its VoteId is materially wider
 * than the historic v3 Backup ACK promise.  Treating a partial legacy vote as
 * a quorum vote after reset would re-open exactly the mixed-version path M06
 * fenced.  The full identity is therefore written before the local self-bit
 * may be counted by the caller-owned takeover model. */
static bool takeover_vote_commit_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected;

    if (next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        state_has_prepared_transaction(committed) || !committed->has_active_epoch ||
        !committed->has_max_epoch ||
        !epoch_is_equal(&committed->active_epoch, &committed->max_epoch) ||
        !committed->committed_config.valid ||
        /* One current-Epoch durable Vote is a single-vote promise. A vote
         * attached to an older Epoch is only historical audit evidence and
         * is atomically replaced by the next full M10 VoteId. */
        (committed->last_vote.valid &&
         epoch_is_equal(&committed->last_vote.epoch,
                        &committed->active_epoch)) ||
        !ucn_cluster_persist_vote_is_complete_takeover(&next->last_vote) ||
        !epoch_is_equal(&next->last_vote.epoch, &committed->active_epoch) ||
        next->last_vote.config_id != committed->committed_config.config_id) {
        return false;
    }
    expected = *committed;
    expected.last_vote = next->last_vote;
    return state_equal_ignoring_completed_operation(&expected, next);
}

/* A resulting Head Epoch is a distinct durable promise. It must be an exact
 * successor of the complete VoteId's frozen old Epoch, retain that VoteId for
 * restart audit, and cannot smuggle a Config/Rekey/Incarnation update. */
static bool takeover_epoch_commit_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected;
    ucn_cluster_epoch_t proposed;

    if (committed->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        state_has_prepared_transaction(committed) || !committed->has_active_epoch ||
        !committed->has_max_epoch ||
        !epoch_is_equal(&committed->active_epoch, &committed->max_epoch) ||
        !committed->committed_config.valid ||
        !ucn_cluster_persist_vote_is_complete_takeover(&committed->last_vote) ||
        !epoch_is_equal(&committed->last_vote.epoch, &committed->active_epoch) ||
        committed->last_vote.config_id != committed->committed_config.config_id ||
        !next->has_active_epoch || !next->has_max_epoch ||
        !epoch_is_equal(&next->active_epoch, &next->max_epoch) ||
        !ucn_cluster_persist_vote_is_complete_takeover(&next->last_vote) ||
        memcmp(&next->last_vote, &committed->last_vote,
               sizeof(next->last_vote)) != 0) {
        return false;
    }
    proposed.cluster_id = committed->last_vote.epoch.cluster_id;
    proposed.term = committed->last_vote.proposed_term;
    proposed.head_node_id = committed->last_vote.voted_for_node_id;
    if (!epoch_is_equal(&next->active_epoch, &proposed)) {
        return false;
    }
    expected = *committed;
    expected.has_active_epoch = true;
    expected.active_epoch = proposed;
    expected.has_max_epoch = true;
    expected.max_epoch = proposed;
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
        /* A complete M10 VoteId for the current Epoch is an exclusive
         * promise to its named Backup. The generic election transition has
         * no certificate parameter, so only TAKEOVER_EPOCH_COMMIT may select
         * a successor while that promise is live. */
        (committed->last_vote.valid &&
         ucn_cluster_persist_vote_is_complete_takeover(&committed->last_vote) &&
         epoch_is_equal(&committed->last_vote.epoch,
                        &committed->active_epoch)) ||
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
        next->active_epoch.term != 1U ||
        next->epoch_scope != UCN_CLUSTER_PERSIST_EPOCH_SCOPE_STABLE ||
        !recovery_identity_is_absent(&next->recovery_identity)) {
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
    expected.epoch_scope = UCN_CLUSTER_PERSIST_EPOCH_SCOPE_STABLE;
    (void)memset(&expected.recovery_identity, 0,
                 sizeof(expected.recovery_identity));
    expected.recovery_tombstone = next->recovery_tombstone;
    return state_equal_ignoring_completed_operation(&expected, next);
}

static bool rekey_abort_transition_is_valid(
    const ucn_cluster_persist_state_t *committed,
    const ucn_cluster_persist_state_t *next)
{
    ucn_cluster_persist_state_t expected;

    if (committed->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        next->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        committed->rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        next->rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED ||
        next->rekey_transaction.transaction_id !=
            committed->rekey_transaction.transaction_id ||
        !rekey_ref_is_equal(&next->rekey_transaction.staging_rekey,
                            &committed->rekey_transaction.staging_rekey) ||
        (next->boot_incarnation != committed->boot_incarnation &&
         !serial_is_next(committed->boot_incarnation,
                         next->boot_incarnation))) {
        return false;
    }
    expected = *committed;
    expected.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED;
    expected.boot_incarnation = next->boot_incarnation;
    return state_equal_ignoring_completed_operation(&expected, next);
}

ucn_result_t ucn_cluster_persist_recovery_identity_admit(
    const ucn_cluster_persist_state_t *state, uint32_t cluster_id,
    uint32_t recovery_round, uint32_t recovery_nonce)
{
    if (state == NULL || !ucn_cluster_persist_state_is_valid(state) ||
        cluster_id == 0U || cluster_id == UCN_NODE_BROADCAST ||
        recovery_nonce == 0U ||
        recovery_nonce > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        recovery_round > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return UCN_ERR_ARGUMENT;
    }
    if (state->recovery_tombstone.valid &&
        state->recovery_tombstone.retired_epoch.cluster_id == cluster_id) {
        return UCN_ERR_REPLAY;
    }
    if (state->epoch_scope != UCN_CLUSTER_PERSIST_EPOCH_SCOPE_RECOVERY ||
        !state->recovery_identity.valid) {
        return UCN_ERR_NOT_FOUND;
    }
    if (state->recovery_identity.epoch.cluster_id != cluster_id ||
        state->recovery_identity.recovery_round != recovery_round ||
        state->recovery_identity.recovery_nonce != recovery_nonce) {
        return UCN_ERR_REPLAY;
    }
    return UCN_OK;
}

/* CLV2-M12.1 (MAJOR-1): the Recovery create is the ONLY new-identity
 * transition that may carry a non-1 Term.  The Term is the captured
 * parent Term (a valid serial) and is exactly what the Recovery Head
 * publishes, so the durable promise and the wire authority Epoch agree.
 * Every other gate (no prepared transaction, no committed Rekey/
 * Tombstone, parent identity must differ, ordinary-state clear policy)
 * is identical to CLUSTER_CREATE_COMMIT. */
static bool recovery_create_commit_transition_is_valid(
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
        !serial_is_valid(next->active_epoch.term) ||
        next->epoch_scope != UCN_CLUSTER_PERSIST_EPOCH_SCOPE_RECOVERY ||
        !recovery_identity_is_valid(&next->recovery_identity) ||
        !epoch_is_equal(&next->active_epoch,
                        &next->recovery_identity.epoch)) {
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
    expected.epoch_scope = UCN_CLUSTER_PERSIST_EPOCH_SCOPE_RECOVERY;
    expected.recovery_identity = next->recovery_identity;
    expected.recovery_tombstone = next->recovery_tombstone;
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
        next->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
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
        request->operation != UCN_CLUSTER_PERSIST_OPERATION_REKEY_COMMIT &&
        request->operation != UCN_CLUSTER_PERSIST_OPERATION_REKEY_ABORT &&
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
    case UCN_CLUSTER_PERSIST_OPERATION_RECOVERY_CREATE_COMMIT:
        return recovery_create_commit_transition_is_valid(
            committed, &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT:
        return vote_commit_transition_is_valid(committed, &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_VOTE_COMMIT:
        return takeover_vote_commit_transition_is_valid(
            committed, &request->next_state);
    case UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_EPOCH_COMMIT:
        return takeover_epoch_commit_transition_is_valid(
            committed, &request->next_state);
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
    case UCN_CLUSTER_PERSIST_OPERATION_REKEY_ABORT:
        return rekey_abort_transition_is_valid(committed,
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

    if (record == NULL ||
        (record_length != UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES &&
         record_length != UCN_CLUSTER_PERSIST_RECORD_V3_BYTES &&
         record_length != UCN_CLUSTER_PERSIST_RECORD_BYTES)) {
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
        state->record_schema_version != UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
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
        if (ucn_cluster_persist_vote_is_complete_takeover(&state->last_vote)) {
            write_u32_be(output + PERSIST_V3_VOTE_PROPOSED_TERM_OFFSET,
                         state->last_vote.proposed_term);
            write_u32_be(output + PERSIST_V3_VOTE_CONFIG_ID_OFFSET,
                         state->last_vote.config_id);
            write_u32_be(output + PERSIST_V3_VOTE_SNAPSHOT_ID_OFFSET,
                         state->last_vote.snapshot_id);
        }
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
                UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
            state->rekey_transaction.phase ==
                UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED) {
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
    if (state->committed_rekey.valid) {
        write_config_ref(output + PERSIST_V4_REKEY_SUCCESSOR_CONFIG_OFFSET,
                         &state->committed_rekey.successor_config);
        write_u32_be(output + PERSIST_V4_REKEY_PREPARE_NONCE_OFFSET,
                     state->committed_rekey.prepare_nonce);
        write_u32_be(output + PERSIST_V4_REKEY_SUCCESSOR_BACKUP_OFFSET,
                      state->committed_rekey.successor_backup_node_id);
        write_u32_be(output + PERSIST_V4_REKEY_HISTORY_FINGERPRINT_OFFSET,
                     state->committed_rekey.allocation_history_fingerprint);
    } else if (state->rekey_transaction.phase ==
                   UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
               state->rekey_transaction.phase ==
                   UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED) {
        write_config_ref(output + PERSIST_V4_REKEY_SUCCESSOR_CONFIG_OFFSET,
                         &state->rekey_transaction.staging_rekey.successor_config);
        write_u32_be(output + PERSIST_V4_REKEY_PREPARE_NONCE_OFFSET,
                     state->rekey_transaction.staging_rekey.prepare_nonce);
        write_u32_be(output + PERSIST_V4_REKEY_SUCCESSOR_BACKUP_OFFSET,
                      state->rekey_transaction.staging_rekey.
                          successor_backup_node_id);
        write_u32_be(
            output + PERSIST_V4_REKEY_HISTORY_FINGERPRINT_OFFSET,
            state->rekey_transaction.staging_rekey.
                allocation_history_fingerprint);
    }
    output[PERSIST_V4_EPOCH_SCOPE_OFFSET] = (uint8_t)state->epoch_scope;
    output[PERSIST_V4_RECOVERY_VALID_OFFSET] =
        state->recovery_identity.valid ? 1U : 0U;
    if (state->recovery_identity.valid) {
        write_epoch(output + PERSIST_V4_RECOVERY_EPOCH_OFFSET,
                    &state->recovery_identity.epoch);
        write_u32_be(output + PERSIST_V4_RECOVERY_PARENT_CLUSTER_OFFSET,
                     state->recovery_identity.parent_cluster_id);
        write_u32_be(output + PERSIST_V4_RECOVERY_PARENT_TERM_OFFSET,
                     state->recovery_identity.parent_term);
        write_u32_be(output + PERSIST_V4_RECOVERY_PARENT_CONFIG_OFFSET,
                     state->recovery_identity.parent_config_id);
        write_u32_be(output + PERSIST_V4_RECOVERY_ROUND_OFFSET,
                     state->recovery_identity.recovery_round);
        write_u32_be(output + PERSIST_V4_RECOVERY_NONCE_OFFSET,
                     state->recovery_identity.recovery_nonce);
        write_u32_be(output + PERSIST_V4_RECOVERY_CLUSTER_ID_ROUND_OFFSET,
                     state->recovery_identity.cluster_id_round);
    }
    output[PERSIST_V4_RECOVERY_TOMBSTONE_VALID_OFFSET] =
        state->recovery_tombstone.valid ? 1U : 0U;
    if (state->recovery_tombstone.valid) {
        write_epoch(output + PERSIST_V4_RECOVERY_TOMBSTONE_EPOCH_OFFSET,
                    &state->recovery_tombstone.retired_epoch);
        write_u32_be(output + PERSIST_V4_RECOVERY_TOMBSTONE_REPLACEMENT_OFFSET,
                     state->recovery_tombstone.replacement_cluster_id);
        write_u32_be(output + PERSIST_V4_RECOVERY_TOMBSTONE_ROUND_OFFSET,
                     state->recovery_tombstone.recovery_round);
    }
    crc = crc32_record(output, UCN_CLUSTER_PERSIST_RECORD_BYTES);
    write_u32_be(output + PERSIST_CRC_OFFSET, crc);
    return UCN_OK;
}

static bool decode_transaction_phase(
    uint8_t value,
    ucn_cluster_persist_transaction_phase_t *phase)
{
    if (value != UCN_CLUSTER_PERSIST_TRANSACTION_NONE &&
        value != UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
        value != UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
        value != UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED) {
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
    uint16_t schema_version;
    size_t expected_record_bytes;
    size_t logical_record_bytes;

    if (record == NULL || record_generation == NULL || state == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (record_length != UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES &&
        record_length != UCN_CLUSTER_PERSIST_RECORD_V3_BYTES &&
        record_length != UCN_CLUSTER_PERSIST_RECORD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    if (ucn_cluster_persist_record_is_factory_empty(record, record_length)) {
        return UCN_ERR_NOT_FOUND;
    }
    if (read_u32_be(record + PERSIST_MAGIC_OFFSET) !=
        UCN_CLUSTER_PERSIST_RECORD_MAGIC) {
        return UCN_ERR_MALFORMED;
    }
    schema_version = read_u16_be(record + PERSIST_SCHEMA_OFFSET);
    if (!record_schema_version_is_valid(schema_version)) {
        return UCN_ERR_VERSION;
    }
    expected_record_bytes = record_bytes_for_schema(schema_version);
    logical_record_bytes = expected_record_bytes;
    if ((record_length != expected_record_bytes &&
         !(record_length == UCN_CLUSTER_PERSIST_RECORD_BYTES &&
           expected_record_bytes < record_length &&
           (bytes_are_zero(record + expected_record_bytes,
                           record_length - expected_record_bytes) ||
            bytes_are_value(record + expected_record_bytes,
                            record_length - expected_record_bytes,
                            UINT8_MAX)))) ||
        read_u16_be(record + PERSIST_SIZE_OFFSET) != expected_record_bytes ||
        read_u32_be(record + PERSIST_GENERATION_OFFSET) == 0U) {
        return UCN_ERR_MALFORMED;
    }
    stored_crc = read_u32_be(record + PERSIST_CRC_OFFSET);
    if (stored_crc != crc32_record(record, logical_record_bytes)) {
        return UCN_ERR_CRC;
    }
    if (!bytes_are_zero(record + PERSIST_RESERVED_OFFSET,
                        PERSIST_RESERVED_BYTES)) {
        return UCN_ERR_MALFORMED;
    }
    ucn_cluster_persist_state_init_empty(&decoded);
    decoded.record_schema_version = schema_version;
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
        if (schema_version >=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3) {
            decoded.last_vote.proposed_term =
                read_u32_be(record + PERSIST_V3_VOTE_PROPOSED_TERM_OFFSET);
            decoded.last_vote.config_id =
                read_u32_be(record + PERSIST_V3_VOTE_CONFIG_ID_OFFSET);
            decoded.last_vote.snapshot_id =
                read_u32_be(record + PERSIST_V3_VOTE_SNAPSHOT_ID_OFFSET);
        }
    } else if (!bytes_are_zero(record + PERSIST_VOTE_EPOCH_OFFSET, 20U)) {
        return UCN_ERR_MALFORMED;
    }
    if (schema_version >= UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3 &&
        !decoded.last_vote.valid &&
        !bytes_are_zero(record + PERSIST_V3_VOTE_PROPOSED_TERM_OFFSET,
                        PERSIST_V3_EXTENSION_BYTES)) {
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
                UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
            decoded.rekey_transaction.phase ==
                UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED) {
            if (!read_rekey_ref(record + PERSIST_REKEY_STAGING_VALID_OFFSET,
                                &decoded.rekey_transaction.staging_rekey)) {
                return UCN_ERR_MALFORMED;
            }
        } else if (!bytes_are_zero(record + PERSIST_REKEY_STAGING_VALID_OFFSET,
                                   58U)) {
            return UCN_ERR_MALFORMED;
        }
    }
    if (schema_version >=
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V4) {
        ucn_cluster_persist_config_ref_t successor_config;

        if (!read_config_ref(
                record + PERSIST_V4_REKEY_SUCCESSOR_CONFIG_OFFSET,
                &successor_config)) {
            return UCN_ERR_MALFORMED;
        }
        if (decoded.committed_rekey.valid) {
            if (!successor_config.valid) {
                return UCN_ERR_MALFORMED;
            }
            decoded.committed_rekey.successor_config = successor_config;
            decoded.committed_rekey.prepare_nonce = read_u32_be(
                record + PERSIST_V4_REKEY_PREPARE_NONCE_OFFSET);
            decoded.committed_rekey.successor_backup_node_id = read_u32_be(
                record + PERSIST_V4_REKEY_SUCCESSOR_BACKUP_OFFSET);
            decoded.committed_rekey.allocation_history_fingerprint =
                read_u32_be(
                    record + PERSIST_V4_REKEY_HISTORY_FINGERPRINT_OFFSET);
        } else if (decoded.rekey_transaction.phase ==
                       UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
                   decoded.rekey_transaction.phase ==
                       UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED) {
            if (!successor_config.valid) {
                return UCN_ERR_MALFORMED;
            }
            decoded.rekey_transaction.staging_rekey.successor_config =
                successor_config;
            decoded.rekey_transaction.staging_rekey.prepare_nonce = read_u32_be(
                record + PERSIST_V4_REKEY_PREPARE_NONCE_OFFSET);
            decoded.rekey_transaction.staging_rekey.
                successor_backup_node_id = read_u32_be(
                    record + PERSIST_V4_REKEY_SUCCESSOR_BACKUP_OFFSET);
            decoded.rekey_transaction.staging_rekey.
                allocation_history_fingerprint = read_u32_be(
                    record + PERSIST_V4_REKEY_HISTORY_FINGERPRINT_OFFSET);
        } else if (successor_config.valid ||
                   !bytes_are_zero(
                       record + PERSIST_V4_REKEY_SUCCESSOR_CONFIG_OFFSET,
                        PERSIST_V4_REKEY_SUCCESSOR_CONFIG_BYTES + 8U) ||
                   !bytes_are_zero(
                       record + PERSIST_V4_REKEY_HISTORY_FINGERPRINT_OFFSET,
                       4U)) {
            return UCN_ERR_MALFORMED;
        }
        decoded.epoch_scope =
            (ucn_cluster_persist_epoch_scope_t)
                record[PERSIST_V4_EPOCH_SCOPE_OFFSET];
        if (!decode_bool(record, PERSIST_V4_RECOVERY_VALID_OFFSET,
                         &decoded.recovery_identity.valid) ||
            !decode_bool(record, PERSIST_V4_RECOVERY_TOMBSTONE_VALID_OFFSET,
                         &decoded.recovery_tombstone.valid)) {
            return UCN_ERR_MALFORMED;
        }
        if (decoded.recovery_identity.valid) {
            read_epoch(record + PERSIST_V4_RECOVERY_EPOCH_OFFSET,
                       &decoded.recovery_identity.epoch);
            decoded.recovery_identity.parent_cluster_id = read_u32_be(
                record + PERSIST_V4_RECOVERY_PARENT_CLUSTER_OFFSET);
            decoded.recovery_identity.parent_term = read_u32_be(
                record + PERSIST_V4_RECOVERY_PARENT_TERM_OFFSET);
            decoded.recovery_identity.parent_config_id = read_u32_be(
                record + PERSIST_V4_RECOVERY_PARENT_CONFIG_OFFSET);
            decoded.recovery_identity.recovery_round = read_u32_be(
                record + PERSIST_V4_RECOVERY_ROUND_OFFSET);
            decoded.recovery_identity.recovery_nonce = read_u32_be(
                record + PERSIST_V4_RECOVERY_NONCE_OFFSET);
            decoded.recovery_identity.cluster_id_round = read_u32_be(
                record + PERSIST_V4_RECOVERY_CLUSTER_ID_ROUND_OFFSET);
        } else if (!bytes_are_zero(record + PERSIST_V4_RECOVERY_EPOCH_OFFSET,
                                   36U)) {
            return UCN_ERR_MALFORMED;
        }
        if (decoded.recovery_tombstone.valid) {
            read_epoch(record + PERSIST_V4_RECOVERY_TOMBSTONE_EPOCH_OFFSET,
                       &decoded.recovery_tombstone.retired_epoch);
            decoded.recovery_tombstone.replacement_cluster_id = read_u32_be(
                record + PERSIST_V4_RECOVERY_TOMBSTONE_REPLACEMENT_OFFSET);
            decoded.recovery_tombstone.recovery_round = read_u32_be(
                record + PERSIST_V4_RECOVERY_TOMBSTONE_ROUND_OFFSET);
        } else if (!bytes_are_zero(
                       record + PERSIST_V4_RECOVERY_TOMBSTONE_EPOCH_OFFSET,
                       20U)) {
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
