#include "ucn/ucn_cluster.h"
#include "ucn/ucn_cluster_authority.h"
#include "ucn/ucn_cluster_persist.h"

#include <assert.h>
#include <string.h>

#include "ucn/ucn_time.h"

/* CLV2-M02 (02-03): the transition framework lives in the FSM module;
 * ucn_cluster.c calls the four exposed helpers through the Cluster
 * private header (Extended-only).  (02-04): the member-table / join /
 * keepalive handlers live in the membership module; this file calls them
 * through the same internal header. */
#include "cluster/ucn_cluster_internal.h"

/* CLV2-M02 (02-05): the Backup/Takeover module owns the backup senders,
 * handlers and takeover lifecycle; only the send infra that stays here
 * is declared locally (the rest comes from ucn_cluster_internal.h). */
void assign_backup(ucn_cluster_t *cluster, uint32_t now_ms);
void backup_resync(ucn_cluster_t *cluster);
void backup_clear_sync(ucn_cluster_t *cluster, uint32_t now_ms);
void queue_backup_assignment_for_member(ucn_cluster_t *cluster,
                                               ucn_node_id_t member_node_id,
                                               uint32_t now_ms);
ucn_result_t send_cluster_message(ucn_cluster_t *cluster,
                                         ucn_node_id_t destination,
                                         const ucn_cluster_message_t *message);
void send_takeover_prepare_step(ucn_cluster_t *cluster);
void send_takeover_announce_step(ucn_cluster_t *cluster);



uint32_t next_nonce(ucn_cluster_t *cluster)
{
    uint32_t nonce = cluster->next_nonce;

    if (nonce == 0U || nonce == UINT32_MAX) {
        nonce = 1U;
    }
    cluster->next_nonce = nonce + 1U;
    return nonce;
}

/* CLV2-M03 (03-07): this is the one checked increment primitive for
 * Cluster safety serials.  Zero is an uninitialized value, so it may start
 * at one; once a serial has reached the configured rotation threshold, it
 * has no successor in this Cluster identity.  M13 replaces EXHAUSTED with
 * a committed Rekey. */
ucn_result_t cluster_serial_next_checked(uint32_t current, uint32_t *next)
{
    if (next == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (current >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return UCN_ERR_EXHAUSTED;
    }
    *next = current + 1U;
    return UCN_OK;
}

static uint32_t cluster_id_mix(uint32_t value)
{
    value ^= value >> 16U;
    value *= UINT32_C(0x7FEB352D);
    value ^= value >> 15U;
    value *= UINT32_C(0x846CA68B);
    value ^= value >> 16U;
    return value;
}

static bool cluster_id_is_valid(uint32_t cluster_id,
                                uint32_t parent_cluster_id)
{
    return cluster_id != 0U && cluster_id != UCN_NODE_BROADCAST &&
           (parent_cluster_id == 0U || cluster_id != parent_cluster_id);
}

/* The no-provider default keeps the first-ever stable election identity at
 * local_node_id for migration continuity.  Every later round, or any
 * non-zero application incarnation, derives a deterministic new value.
 * Recovery always derives a distinct ID because it has a parent domain. */
static uint32_t cluster_default_next_id(const ucn_cluster_id_request_t *request)
{
    uint32_t candidate;
    uint32_t attempt;

    if (request->purpose == UCN_CLUSTER_ID_PURPOSE_ELECTION &&
        request->incarnation == 0U && request->parent_cluster_id == 0U &&
        request->round == 1U) {
        return request->local_node_id;
    }
    candidate = UINT32_C(0xA511E9B3) ^ request->local_node_id ^
                request->parent_cluster_id ^ request->parent_term ^
                request->incarnation ^ request->round ^
                ((uint32_t)request->purpose << 24U);
    for (attempt = 0U; attempt < 4U; ++attempt) {
        candidate = cluster_id_mix(candidate +
                                   UINT32_C(0x9E3779B9) + attempt);
        if (cluster_id_is_valid(candidate, request->parent_cluster_id)) {
            return candidate;
        }
    }
    return 0U;
}

ucn_result_t cluster_make_next_id(ucn_cluster_t *cluster,
                                  ucn_cluster_id_purpose_t purpose,
                                  uint32_t parent_cluster_id,
                                  uint32_t parent_term,
                                  uint32_t *cluster_id)
{
    ucn_cluster_id_request_t request;
    uint32_t next_round;
    uint32_t candidate = 0U;
    ucn_result_t result;

    if (cluster == NULL || cluster_id == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = cluster_serial_next_checked(cluster->cluster_id_round,
                                         &next_round);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(&request, 0, sizeof(request));
    request.purpose = purpose;
    request.local_node_id = cluster->config.local_node_id;
    request.parent_cluster_id = parent_cluster_id;
    request.parent_term = parent_term;
    request.incarnation = cluster->config.cluster_id_incarnation;
    request.round = next_round;
    if (cluster->config.make_cluster_id != NULL) {
        result = cluster->config.make_cluster_id(
            cluster->config.cluster_id_context, &request, &candidate);
        if (result != UCN_OK) {
            return result;
        }
    } else {
        candidate = cluster_default_next_id(&request);
    }
    if (!cluster_id_is_valid(candidate, parent_cluster_id)) {
        return UCN_ERR_CONFIG;
    }
    cluster->cluster_id_round = next_round;
    *cluster_id = candidate;
    return UCN_OK;
}

/* C07.5 control-plane Token Bucket.  One aggregate pool bounds the total
 * control rate to the Cluster window budget (burst + refill rate per 1 s). */
static void token_bucket_refill(ucn_cluster_token_bucket_t *bucket,
                                uint32_t now_ms, uint16_t burst,
                                uint32_t refill_ms)
{
    uint32_t elapsed;
    uint32_t added;

    if (bucket->last_refill_ms == 0U) {
        /* C07.7 P2: cold start already carries a full burst; only stamp the
         * clock here so t=0 consumption cannot be followed by a second,
         * spurious full-burst refill at t=1. */
        bucket->last_refill_ms = now_ms;
        return;
    }
    elapsed = now_ms - bucket->last_refill_ms;
    if (elapsed < refill_ms) {
        return;
    }
    /* Advance the clock by the FULL elapsed interval (do NOT cap `added`
     * at burst); otherwise last_refill_ms lags and idle credit leaks out
     * as repeated burst tokens, defeating the 1 s bound. */
    added = elapsed / refill_ms;
    {
        uint32_t total = (uint32_t)bucket->tokens + added;

        bucket->tokens = total > (uint32_t)burst ? burst : (uint16_t)total;
    }
    bucket->last_refill_ms += added * refill_ms;
}

static bool token_bucket_take(ucn_cluster_t *cluster)
{
    ucn_cluster_token_bucket_t *tb = &cluster->token_bucket;

    token_bucket_refill(tb, cluster_now(cluster),
                        cluster->config.token_bucket.burst,
                        cluster->config.token_bucket.refill_ms);
    if (tb->tokens == 0U) {
        cluster->stats.token_deferred++;
        return false;
    }
    tb->tokens--;
    return true;
}

ucn_result_t cluster_transmit(
    ucn_cluster_t *cluster, ucn_node_id_t destination,
    const ucn_cluster_message_t *message,
    const uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES])
{
    ucn_result_t result;

    if (cluster == NULL || message == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    /* M04: a REQUIRED Provider owns the authority boundary.  Do this before
     * taking a token so a pending/failed write cannot silently consume the
     * control budget while suppressing its own outbound promise. */
    if (!cluster_persistence_outbound_allowed(cluster)) {
        return cluster != NULL && cluster->persistence_faulted ?
                   cluster->persistence_failure : UCN_ERR_STATE;
    }
    /* M08 freshness must be evaluated at the actual send boundary, not only
     * at a previous ucn_cluster_step().  This is before the token bucket so
     * stale Authority cannot consume control capacity. */
    result = ucn_cluster_authority_runtime_preflight(
        cluster->authority_runtime, cluster_now(cluster));
    if (result != UCN_OK) {
        return result;
    }
    /* CLV2-08-05: an explicitly installed M08 Authority Owner owns the
     * Head write gate.  Unmanaged legacy instances retain their existing
     * path until M05 authorizes production-v4 integration; managed Grace or
     * Fenced instances are denied before spending a control token. */
    if (!ucn_cluster_authority_runtime_tx_allowed(
            cluster->authority_runtime, message->type, message->role)) {
        return UCN_ERR_ACCESS;
    }
    if (!token_bucket_take(cluster)) {
        return UCN_ERR_NO_SPACE;
    }
    result = cluster->config.send(cluster->config.send_context, destination,
                                  UCN_CLUSTER_CONTROL_ENDPOINT, payload,
                                  (uint16_t)UCN_CLUSTER_MESSAGE_BYTES);
    if (result == UCN_OK) {
        cluster->stats.messages_sent++;
    } else {
        cluster->stats.send_failures++;
    }
    return result;
}

ucn_result_t send_message(
    ucn_cluster_t *cluster,
    ucn_node_id_t destination,
    ucn_cluster_message_type_t type,
    ucn_cluster_role_t role,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id,
    uint16_t head_score,
    uint16_t capacity)
{
    ucn_cluster_message_t message;
    uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_result_t result;

    (void)memset(&message, 0, sizeof(message));
    message.type = type;
    message.role = role;
    message.cluster_id = cluster_id;
    message.term = term;
    message.head_node_id = head_node_id;
    message.head_score = head_score;
    message.available_capacity = capacity;
    message.lease_ms = cluster->config.lease_ms;
    message.nonce = next_nonce(cluster);
    result = ucn_cluster_message_encode(&message, payload);
    if (result != UCN_OK) {
        return result;
    }
    return cluster_transmit(cluster, destination, &message, payload);
}

static bool config_is_valid(const ucn_cluster_config_t *config)
{
    if (config == NULL || config->local_node_id == 0U ||
        config->local_node_id == UCN_NODE_BROADCAST ||
        config->head_score > UCN_CLUSTER_SCORE_MAX) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    if (config->now_ms == NULL || config->send == NULL ||
         (config->head_capable &&
         (config->member_capacity == 0U ||
           config->member_capacity > UCN_CLUSTER_MAX_MEMBERS ||
           config->member_capacity > UCN_CLUSTER_MAX_PEERS ||
           config->voter_capacity > UCN_CLUSTER_MAX_VOTERS)) ||
        (!config->head_capable &&
         (config->member_capacity != 0U || config->voter_capacity != 0U))) {
        return false;
    }
    return true;
}

static bool persistence_mode_is_valid(ucn_cluster_persistence_mode_t mode)
{
    return mode == UCN_CLUSTER_PERSISTENCE_REQUIRED ||
           mode == UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
}

static bool required_persistence_is_configured(
    const ucn_cluster_config_t *config)
{
    return config->persistence_mode != UCN_CLUSTER_PERSISTENCE_REQUIRED ||
           ucn_cluster_persist_provider_is_compatible(
               config->persistence_provider);
}

/* M04-04 intentionally restores only safety inputs that the Current FSM
 * already owns.  Config/Rekey/Tombstone are fully validated by the Provider
 * result but have no Current-FSM storage or transition consumer yet; 04-07,
 * M07 and M13 own their runtime integration.  Keeping them provider-owned is
 * safer than inventing a partial in-memory mirror that an old transition
 * could silently overwrite. */
static void restore_current_fsm_safety_inputs(
    ucn_cluster_t *cluster,
    const ucn_cluster_persist_state_t *state)
{
    if (state->has_max_epoch) {
        cluster->last_cluster_id = state->max_epoch.cluster_id;
        cluster->max_seen_term = state->max_epoch.term;
        cluster->last_stable_head = state->max_epoch.head_node_id;
    }
    if (state->last_vote.valid) {
        /* Current M03 only has this replay-suppression key.  The persisted
         * voted_for_node_id remains Provider authority until 04-06 replaces
         * the legacy TAKEOVER_ACK path with the full durable VoteId gate. */
        cluster->member_voted_cluster_id = state->last_vote.epoch.cluster_id;
        cluster->member_voted_term = state->last_vote.epoch.term;
        cluster->member_voted_generation = state->last_vote.backup_generation;
    }
    /* The durable boot boundary overrides an application-supplied hint after
     * a READY load.  A Factory Empty state has no established incarnation. */
    cluster->config.cluster_id_incarnation = state->boot_incarnation;
}

static ucn_result_t load_persistence_before_init(ucn_cluster_t *cluster)
{
    ucn_cluster_persist_state_t loaded;
    bool factory_empty;
    ucn_result_t result;

    if (cluster->config.persistence_mode ==
        UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST) {
        return UCN_OK;
    }
    result = cluster_persistence_load_snapshot_ex(cluster, &loaded,
                                                   &factory_empty);
    if (result != UCN_OK) {
        return result;
    }
    if (factory_empty) {
        cluster->stats.persistence_restore_state =
            UCN_CLUSTER_PERSISTENCE_RESTORE_FACTORY_EMPTY;
        return UCN_OK;
    }
    restore_current_fsm_safety_inputs(cluster, &loaded);
    cluster->stats.persistence_restore_state =
        UCN_CLUSTER_PERSISTENCE_RESTORE_READY;
    return UCN_OK;
}

/* A controlled boot allocates one durable replay incarnation before any role,
 * timer or wire-visible Cluster activity exists.  It is intentionally a
 * separate Record operation: Epoch/Vote/Config transitions may never smuggle
 * a changed incarnation through their normal authority write. */
static ucn_result_t establish_boot_incarnation_before_init(
    ucn_cluster_t *cluster,
    bool *committed)
{
    ucn_cluster_persist_state_t current;
    ucn_cluster_persist_state_t next;
    ucn_cluster_persist_state_t durable_state;
    uint32_t next_incarnation;
    ucn_cluster_persist_operation_t operation;
    ucn_result_t result;

    if (cluster == NULL || committed == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *committed = true;
    if (cluster->config.persistence_mode ==
        UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST) {
        return UCN_OK;
    }
    result = cluster_persistence_load_snapshot(cluster, &current);
    if (result != UCN_OK) {
        return result;
    }
    result = cluster_serial_next_checked(current.boot_incarnation,
                                         &next_incarnation);
    if (result != UCN_OK) {
        return result;
    }
    next = current;
    next.boot_incarnation = next_incarnation;
    operation = UCN_CLUSTER_PERSIST_OPERATION_REPLAY_INCARNATION;
    /* Only physical Record-v1 PREPARED is R23 migration input. Schema v2
     * belongs to its owning M07/M13 transaction and may never be mistaken for
     * a legacy abort; until that owner restores it, boot remains fail-closed. */
    if ((current.config_transaction.phase ==
             UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
         current.rekey_transaction.phase ==
             UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) &&
        current.record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1) {
        return UCN_ERR_STATE;
    }
    if (current.config_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
        current.record_schema_version ==
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1) {
        (void)memset(&next.config_transaction, 0,
                     sizeof(next.config_transaction));
        next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
        operation = UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT;
    }
    if (current.rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
        current.record_schema_version ==
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1) {
        (void)memset(&next.rekey_transaction, 0,
                     sizeof(next.rekey_transaction));
        next.rekey_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
        operation = UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT;
    }
    result = cluster_persistence_begin_state(
        cluster, &current, operation,
        &next, CLUSTER_PERSIST_ACTION_BOOT_INCARNATION, 0U, committed,
        &durable_state);
    if (result == UCN_OK && *committed) {
        cluster->config.cluster_id_incarnation = durable_state.boot_incarnation;
    }
    return result;
}

ucn_result_t ucn_cluster_config_apply_timing_profile(
    ucn_cluster_config_t *config,
    ucn_cluster_timing_profile_t profile)
{
    if (config == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    switch (profile) {
        case UCN_CLUSTER_TIMING_PROFILE_DEFAULT:
            config->observation_ms = UCN_CLUSTER_OBSERVATION_MS;
            config->recovery_observation_ms =
                UCN_CLUSTER_RECOVERY_OBSERVATION_MS;
            config->election_window_ms = UCN_CLUSTER_ELECTION_WINDOW_MS;
            config->advertise_interval_ms = UCN_CLUSTER_ADVERTISE_INTERVAL_MS;
            config->join_retry_ms = UCN_CLUSTER_JOIN_RETRY_MS;
            config->keepalive_interval_ms = UCN_CLUSTER_KEEPALIVE_INTERVAL_MS;
            config->lease_ms = UCN_CLUSTER_LEASE_MS;
            config->provisional_timeout_ms =
                UCN_CLUSTER_PROVISIONAL_TIMEOUT_MS;
            config->head_min_tenure_ms = UCN_CLUSTER_HEAD_MIN_TENURE_MS;
            config->token_bucket.burst = UCN_CLUSTER_TB_BURST;
            config->token_bucket.refill_ms = UCN_CLUSTER_TB_REFILL_MS;
            config->recovery_head_ttl_ms =
                UCN_CLUSTER_RECOVERY_HEAD_TTL_MS;
            config->recovery_backoff_max_ms =
                UCN_CLUSTER_RECOVERY_BACKOFF_MAX_MS;
            return UCN_OK;
        case UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED:
            config->observation_ms = UCN_CLUSTER_FAST_OBSERVATION_MS;
            config->recovery_observation_ms =
                UCN_CLUSTER_FAST_RECOVERY_OBSERVATION_MS;
            config->election_window_ms = UCN_CLUSTER_FAST_ELECTION_WINDOW_MS;
            config->advertise_interval_ms =
                UCN_CLUSTER_FAST_ADVERTISE_INTERVAL_MS;
            config->join_retry_ms = UCN_CLUSTER_FAST_JOIN_RETRY_MS;
            config->keepalive_interval_ms =
                UCN_CLUSTER_FAST_KEEPALIVE_INTERVAL_MS;
            config->lease_ms = UCN_CLUSTER_FAST_LEASE_MS;
            config->provisional_timeout_ms =
                UCN_CLUSTER_FAST_PROVISIONAL_TIMEOUT_MS;
            config->head_min_tenure_ms =
                UCN_CLUSTER_FAST_HEAD_MIN_TENURE_MS;
            config->token_bucket.burst = UCN_CLUSTER_FAST_TB_BURST;
            config->token_bucket.refill_ms =
                UCN_CLUSTER_FAST_TB_REFILL_MS;
            config->recovery_head_ttl_ms =
                UCN_CLUSTER_FAST_RECOVERY_HEAD_TTL_MS;
            config->recovery_backoff_max_ms =
                UCN_CLUSTER_FAST_RECOVERY_BACKOFF_MAX_MS;
            return UCN_OK;
        default:
            return UCN_ERR_ARGUMENT;
    }
}

static void apply_config_defaults(ucn_cluster_config_t *config)
{
    if (config->observation_ms == 0U) {
        config->observation_ms = UCN_CLUSTER_OBSERVATION_MS;
    }
    if (config->recovery_observation_ms == 0U) {
        config->recovery_observation_ms =
            UCN_CLUSTER_RECOVERY_OBSERVATION_MS;
    }
    if (config->election_window_ms == 0U) {
        config->election_window_ms = UCN_CLUSTER_ELECTION_WINDOW_MS;
    }
    if (config->advertise_interval_ms == 0U) {
        config->advertise_interval_ms = UCN_CLUSTER_ADVERTISE_INTERVAL_MS;
    }
    if (config->join_retry_ms == 0U) {
        config->join_retry_ms = UCN_CLUSTER_JOIN_RETRY_MS;
    }
    if (config->keepalive_interval_ms == 0U) {
        config->keepalive_interval_ms = UCN_CLUSTER_KEEPALIVE_INTERVAL_MS;
    }
    if (config->lease_ms == 0U) {
        config->lease_ms = UCN_CLUSTER_LEASE_MS;
    }
    if (config->provisional_timeout_ms == 0U) {
        config->provisional_timeout_ms = UCN_CLUSTER_PROVISIONAL_TIMEOUT_MS;
    }
    if (config->head_capable && config->voter_capacity == 0U) {
        config->voter_capacity = (uint16_t)(config->member_capacity + 1U);
    }
    if (config->head_min_tenure_ms == 0U) {
        config->head_min_tenure_ms = UCN_CLUSTER_HEAD_MIN_TENURE_MS;
    }
    if (config->switch_improvement_percent == 0U) {
        config->switch_improvement_percent =
            UCN_CLUSTER_SWITCH_IMPROVEMENT_PERCENT;
    }
    if (config->switch_required_samples == 0U) {
        config->switch_required_samples = UCN_CLUSTER_SWITCH_REQUIRED_SAMPLES;
    }
    if (config->token_bucket.burst == 0U) {
        config->token_bucket.burst = UCN_CLUSTER_TB_BURST;
    }
    if (config->token_bucket.refill_ms == 0U) {
        config->token_bucket.refill_ms = UCN_CLUSTER_TB_REFILL_MS;
    }
    if (config->recovery_head_ttl_ms == 0U) {
        config->recovery_head_ttl_ms = UCN_CLUSTER_RECOVERY_HEAD_TTL_MS;
    }
    if (config->recovery_backoff_max_ms == 0U) {
        config->recovery_backoff_max_ms =
            UCN_CLUSTER_RECOVERY_BACKOFF_MAX_MS;
    }
}

static bool normalized_config_is_valid(const ucn_cluster_config_t *config)
{
    return ucn_duration_is_valid(config->observation_ms) &&
           ucn_duration_is_valid(config->recovery_observation_ms) &&
           ucn_duration_is_valid(config->election_window_ms) &&
           ucn_duration_is_valid(config->advertise_interval_ms) &&
           ucn_duration_is_valid(config->join_retry_ms) &&
           ucn_duration_is_valid(config->keepalive_interval_ms) &&
           ucn_duration_is_valid(config->lease_ms) &&
           ucn_duration_is_valid(config->provisional_timeout_ms) &&
           ucn_duration_is_valid(config->head_min_tenure_ms) &&
           config->advertise_interval_ms <= config->lease_ms / 3U &&
           config->keepalive_interval_ms <= config->lease_ms / 3U &&
           config->switch_improvement_percent < 100U &&
           config->switch_required_samples != 0U &&
           ((!config->head_capable && config->voter_capacity == 0U) ||
            (config->head_capable && config->voter_capacity >= 1U &&
             config->voter_capacity <= UCN_CLUSTER_MAX_VOTERS)) &&
           config->token_bucket.burst != 0U &&
           ucn_duration_is_valid(config->token_bucket.refill_ms) &&
           ucn_duration_is_valid(config->recovery_head_ttl_ms) &&
           ucn_duration_is_valid(config->recovery_backoff_max_ms);
}

ucn_result_t ucn_cluster_init(
    ucn_cluster_t *cluster,
    const ucn_cluster_config_t *config)
{
    uint32_t now_ms = 0U;
    ucn_result_t result;
    bool boot_incarnation_committed;

    if (cluster == NULL || !config_is_valid(config)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(cluster, 0, sizeof(*cluster));
    cluster->config = *config;
    apply_config_defaults(&cluster->config);
    if (!persistence_mode_is_valid(cluster->config.persistence_mode) ||
        !required_persistence_is_configured(&cluster->config)) {
        (void)memset(cluster, 0, sizeof(*cluster));
        return UCN_ERR_CONFIG;
    }
    if (cluster->config.enabled && !normalized_config_is_valid(&cluster->config)) {
        (void)memset(cluster, 0, sizeof(*cluster));
        return UCN_ERR_CONFIG;
    }
    cluster->stats.persistence_mode = cluster->config.persistence_mode;
    /* A REQUIRED instance cannot establish even its detached initial state
     * until the Provider returns a valid Factory Empty or READY snapshot.
     * No Cluster timer, role or outbound path is initialized before this
     * synchronous gate succeeds. */
    result = load_persistence_before_init(cluster);
    if (result != UCN_OK) {
        (void)memset(cluster, 0, sizeof(*cluster));
        return result;
    }
    result = establish_boot_incarnation_before_init(cluster,
                                                    &boot_incarnation_committed);
    if (result != UCN_OK) {
        (void)memset(cluster, 0, sizeof(*cluster));
        return result;
    }
    if (cluster->config.enabled) {
        now_ms = cluster_now(cluster);
    }
    cluster->role = cluster->config.enabled ? UCN_CLUSTER_ROLE_DETACHED :
                                               UCN_CLUSTER_ROLE_DISABLED;
    cluster->observation_deadline_ms = ucn_deadline_from_now(
        now_ms, cluster->config.observation_ms);
    cluster->role_since_ms = now_ms;
    cluster->next_nonce = 1U;
    /* Start the Token Bucket full so cold-start election is not throttled
     * below its already-bounded advertise budget. */
    cluster->token_bucket.tokens = cluster->config.token_bucket.burst;
    /* A PENDING boot write is allowed to finish after init, but the generic
     * persistence gate freezes every role-driven action until poll() reloads
     * and verifies the matching REPLAY_INCARNATION journal entry. */
    (void)boot_incarnation_committed;
    /* CLV2-01-02: seed the shadow phase mirror from the initial legacy
     * state (DETACHED_OBSERVE or DISABLED). */
    cluster->shadow_phase = cluster_phase_from_legacy_state(cluster, now_ms);
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    cluster->shadow_transition_count = 0U;
    return UCN_OK;
}

ucn_result_t ucn_cluster_set_head_score(
    ucn_cluster_t *cluster,
    uint16_t head_score)
{
    if (cluster == NULL || head_score > UCN_CLUSTER_SCORE_MAX) {
        return UCN_ERR_ARGUMENT;
    }
    cluster->config.head_score = head_score;
    if ((cluster->role == UCN_CLUSTER_ROLE_CANDIDATE ||
         cluster->role == UCN_CLUSTER_ROLE_HEAD) &&
        cluster->head_node_id == cluster->config.local_node_id) {
        cluster->current_head_score = head_score;
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_sync_neighbors(
    ucn_cluster_t *cluster,
    const ucn_neighbor_summary_t *neighbors,
    size_t neighbor_count)
{
    size_t input_index;
    size_t output_index = 0U;

    if (cluster == NULL || (neighbors == NULL && neighbor_count != 0U)) {
        return UCN_ERR_ARGUMENT;
    }
    {
        /* C07.7 P2: stage the new peer table and commit only on success so
         * an overflow can never leave a half-written table behind. */
        ucn_cluster_peer_t staged[UCN_CLUSTER_MAX_PEERS];
        size_t staged_count = 0U;

        (void)memset(staged, 0, sizeof(staged));
        for (input_index = 0U; input_index < neighbor_count; ++input_index) {
            if (neighbors[input_index].state != UCN_NEIGHBOR_ADMITTED &&
                neighbors[input_index].state != UCN_NEIGHBOR_SUSPECT) {
                continue;
            }
            if (staged_count >= UCN_CLUSTER_MAX_PEERS) {
                return UCN_ERR_NO_SPACE;
            }
            staged[staged_count].occupied = true;
            staged[staged_count].node_id =
                neighbors[input_index].peer_node_id;
            staged[staged_count].neighbor_state =
                neighbors[input_index].state;
            staged[staged_count].last_seen_ms =
                neighbors[input_index].last_seen_ms;
            ++staged_count;
        }
        (void)memcpy(cluster->peers, staged, sizeof(staged));
        output_index = staged_count;
    }
    if (cluster->advertise_cursor >= output_index) {
        cluster->advertise_cursor = 0U;
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_sync_node_neighbors(
    ucn_cluster_t *cluster,
    const ucn_node_t *node)
{
    ucn_neighbor_summary_t summaries[UCN_MAX_NEIGHBORS];
    size_t count;

    if (cluster == NULL || node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    count = ucn_node_copy_neighbor_summaries(node, summaries,
                                             UCN_MAX_NEIGHBORS);
    return ucn_cluster_sync_neighbors(cluster, summaries, count);
}

const ucn_cluster_peer_t *find_peer(
    const ucn_cluster_t *cluster,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_PEERS; ++index) {
        if (cluster->peers[index].occupied &&
            cluster->peers[index].node_id == node_id) {
            return &cluster->peers[index];
        }
    }
    return NULL;
}





void set_detached(
    ucn_cluster_t *cluster,
    uint32_t now_ms,
    uint32_t observation_ms)
{
    /* CLV2-M03 (03-06): Active/Pending identity is intentionally cleared
     * below, but the most recent stable epoch survives in RAM so an old
     * same-Cluster offer cannot re-enter after a detach/rejoin cycle.  A
     * Recovery domain is not a stable Cluster and therefore must not erase
     * the remembered stable domain.  M04 persists this boundary across boot. */
    if (cluster->role != UCN_CLUSTER_ROLE_CANDIDATE &&
        (cluster->recovery_cluster_id == 0U ||
         cluster->recovery_cluster_id != cluster->cluster_id)) {
        cluster_history_note_stable_epoch(cluster, cluster->cluster_id,
                                          cluster->term,
                                          cluster->head_node_id);
    }
    cluster->role = UCN_CLUSTER_ROLE_DETACHED;
    cluster->cluster_id = 0U;
    cluster->term = 0U;
    cluster->head_node_id = 0U;
    cluster->current_head_score = 0U;
    cluster->pending_head_node_id = 0U;
    cluster->pending_cluster_id = 0U;
    cluster->pending_term = 0U;
    cluster->pending_head_score = 0U;
    cluster->known_backup_node_id = 0U;
    cluster->known_backup_generation = 0U;
    /* C07.7 P1: detaching resets the takeover vote identity so a later
     * Cluster at the same term number is not suppressed by an old vote. */
    cluster->member_voted_term = 0U;
    cluster->member_voted_cluster_id = 0U;
    cluster->member_voted_generation = 0U;
    cluster->head_lease_expires_at_ms = 0U;
    cluster->head_grace_deadline_ms = 0U;
    cluster->election_deadline_ms = 0U;
    cluster->role_since_ms = now_ms;
    cluster->observation_deadline_ms =
        ucn_deadline_from_now(now_ms, observation_ms);
}

void cluster_history_note_stable_epoch(
    ucn_cluster_t *cluster,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id)
{
    if (cluster == NULL || cluster_id == 0U || term == 0U ||
        head_node_id == 0U || head_node_id == UCN_NODE_BROADCAST) {
        return;
    }
    if (cluster->last_cluster_id != cluster_id ||
        term > cluster->max_seen_term) {
        cluster->last_cluster_id = cluster_id;
        cluster->max_seen_term = term;
        cluster->last_stable_head = head_node_id;
        return;
    }
    if (term == cluster->max_seen_term &&
        cluster->last_stable_head == 0U) {
        /* A partially initialized legacy/test state may lack the Head
         * companion.  Fill it only for the exact highest observed Term;
         * never replace a remembered Head with a same-Term challenger. */
        cluster->last_stable_head = head_node_id;
    }
}

bool cluster_history_offer_is_stale(
    const ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate)
{
    ucn_cluster_epoch_t local_epoch;

    if (cluster == NULL || candidate == NULL ||
        cluster->last_cluster_id == 0U || cluster->max_seen_term == 0U ||
        candidate->cluster_id != cluster->last_cluster_id) {
        return false;
    }
    if (candidate->term < cluster->max_seen_term) {
        return true;
    }
    if (candidate->term != cluster->max_seen_term ||
        cluster->last_stable_head == 0U ||
        candidate->head_node_id == cluster->last_stable_head) {
        return false;
    }
    /* An active node seeing a second Head for its own current highest
     * epoch must take the 03-05 TERM_CONFLICT path, not silently classify it
     * as an old offer.  Detached/Candidate/Join states have no such active
     * authority and reject the incompatible remembered identity. */
    local_epoch = ucn_cluster_active_epoch_get(cluster);
    return local_epoch.cluster_id != candidate->cluster_id ||
           local_epoch.term != candidate->term ||
           local_epoch.head_node_id == candidate->head_node_id;
}

/* CLV2-01-04b.3: begin_join()'s field payload WITHOUT the role write.
 * The migrated consider_head_offer() callers (DETACHED_OBSERVE/ELECTION
 * !recovery_eligible, MEMBER/GRACE better-Head switch 01-04c.4, and the
 * RECOVERY_* sources 01-04f) run cluster_transition() FIRST (which owns
 * the role write via apply_legacy) and then apply the remaining site
 * payload through this helper.  begin_join() still applies ALL fields
 * (helper + role) for its not-yet-migrated callers (BACKUP newer-Term,
 * JOIN_PENDING re-target). */
void begin_join_prepare_fields(
    ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    /* Joining a live Head abandons any pending Recovery candidacy. */
    cluster->recovery_eligible = false;
    cluster->recovery_backoff_deadline_ms = 0U;
    cluster->role_since_ms = now_ms;
    cluster->pending_head_node_id = candidate->head_node_id;
    cluster->pending_cluster_id = candidate->cluster_id;
    cluster->pending_term = candidate->term;
    cluster->pending_head_score = candidate->head_score;
    cluster->next_join_retry_ms = now_ms;
}

void begin_join(
    ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    /* CLV2-01-04b.3: role write + the field payload; the write order is
     * irrelevant (all plain field writes, no interleaved side effects),
     * so the shared helper is reused as-is. */
    cluster->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    begin_join_prepare_fields(cluster, candidate, now_ms);
}






/* C07.7 P1: JOIN_ACCEPT/JOIN_REJECT carry the join transaction id by
 * echoing the request nonce, so a stale reply cannot match a newer join. */



/* §8 ordered switchback: notify members before abandoning the Head role. */

/* Ordered yield to a stable Head: notify members, then enter STEPPING_DOWN. */

/* §8.3 Backup score challenge: a Backup that is significantly better than
 * its live Primary (and after the minimum tenure) leaves the Backup role and
 * re-enters election so the current Head can observe and yield to it.
 * CLV2-01-04e.7 (MAJOR 2.A): the score improvement IS the BACKUP_* ->
 * ELECTION transition.  The pre-phase derives from the CURRENT legacy
 * state (takeover_active -> BACKUP_TAKEOVER, ready -> BACKUP_READY, else
 * -> BACKUP_SYNCING) so ONE call site covers all three DIRECT sources;
 * the legacy score event decides THAT the transition runs, and
 * cluster_transition() validates whether the shadow agrees (a caller
 * NEVER uses the shadow to decide whether to SKIP the call).  The
 * transition is called FIRST, UNCONDITIONALLY, fail closed.  A
 * takeover-active Backup CAN receive the same-primary same-cluster
 * same-term ADVERTISE from its still-live Primary (lease evidence via
 * the ADVERTISE), so BACKUP_TAKEOVER -> ELECTION is REAL and wired too;
 * the M01.0.2 takeover_active && syncing combo stays expressible until
 * the site clears it below - a late Type12 during takeover must never be
 * rejected for phase reasons. */

static bool v3_backup_authority_frame_must_be_rejected(
    ucn_cluster_message_type_t type)
{
#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS)
    /* Historical Current-FSM fixture only.  No production target, simulator,
     * public header or product configuration enables this test hook. */
    (void)type;
    return false;
#else
    /* M06 has no production v4 Config/CommittedVoterSet RX owner yet.  A
     * decoded 32 B control frame is therefore necessarily legacy v3 and
     * cannot establish or consume Backup/Takeover authority.  This fence is
     * deliberately at the public RX owner, rather than only at Head-side
     * Backup selection: it covers self/non-self assignment, mirror sync,
     * liveness, voting and takeover installation before any handler writes
     * role, known_backup_*, mirror, deadline, Head or Term state. */
    switch (type) {
    case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
    case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
    case UCN_CLUSTER_MSG_BACKUP_READY:
    case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
    case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
    case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
    case UCN_CLUSTER_MSG_TAKEOVER_ACK:
    case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
    case UCN_CLUSTER_MSG_BACKUP_REJECT:
        return true;
    default:
        return false;
    }
#endif
}

static ucn_result_t ucn_cluster_receive_inner(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    bool protected_control,
    const uint8_t *payload,
    size_t payload_length,
    bool *authority_fenced)
{
    ucn_cluster_message_t message;
    ucn_cluster_candidate_t *candidate;
    uint32_t now_ms;
    ucn_result_t result;

    if (authority_fenced != NULL) {
        *authority_fenced = false;
    }
    if (cluster == NULL || !cluster->config.enabled || source == 0U ||
        source == UCN_NODE_BROADCAST || source == cluster->config.local_node_id) {
        return UCN_ERR_ARGUMENT;
    }
    /* No inbound message is allowed to advance a state machine or cause an
     * ACK while a REQUIRED persistence transaction has not been proved
     * durable.  Parsing is intentionally skipped as well: a pending owner
     * retains no hidden replay/peer side effect. */
    if (cluster_persistence_progress_blocked(cluster)) {
        return cluster->persistence_faulted ? cluster->persistence_failure :
                                              UCN_ERR_STATE;
    }
    if (cluster->config.require_protected_control && !protected_control) {
        cluster->stats.security_rejected++;
        return UCN_ERR_SECURITY;
    }
    if (find_peer(cluster, source) == NULL ||
        find_peer(cluster, source)->neighbor_state != UCN_NEIGHBOR_ADMITTED) {
        return UCN_ERR_ACCESS;
    }
    result = ucn_cluster_message_decode(payload, payload_length, &message);
    if (result != UCN_OK) {
        cluster->stats.malformed_messages++;
        return result;
    }
    if (message.head_node_id != source &&
        message.type != UCN_CLUSTER_MSG_JOIN_REQUEST &&
        message.type != UCN_CLUSTER_MSG_KEEPALIVE &&
        message.type != UCN_CLUSTER_MSG_LEAVE &&
        message.type != UCN_CLUSTER_MSG_BACKUP_READY &&
        message.type != UCN_CLUSTER_MSG_TAKEOVER_PREPARE &&
        message.type != UCN_CLUSTER_MSG_TAKEOVER_ACK &&
        message.type != UCN_CLUSTER_MSG_RECOVERY_ACK &&
        message.type != UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ &&
        message.type != UCN_CLUSTER_MSG_BACKUP_REJECT) {
        cluster->stats.malformed_messages++;
        return UCN_ERR_MALFORMED;
    }
    if (v3_backup_authority_frame_must_be_rejected(message.type)) {
        /* Fail closed before time, stats, shadow or any handler state is
         * touched.  M07 can replace this temporary gate only after a real
         * committed-v4 Config owner validates the complete authority proof. */
        if (authority_fenced != NULL) {
            *authority_fenced = true;
        }
        return UCN_ERR_ACCESS;
    }
    now_ms = cluster_now(cluster);
    /* Refresh the managed Owner before any local-Head handler can allocate,
     * renew or remove a member, or construct a reply.  A mere cached active
     * value from an earlier Step is never a permission at this RX boundary. */
    result = ucn_cluster_authority_runtime_preflight(
        cluster->authority_runtime, now_ms);
    if (result != UCN_OK) {
        return result;
    }
    if (ucn_cluster_authority_is_managed(cluster) &&
        !ucn_cluster_authority_active(cluster) &&
        cluster->role == UCN_CLUSTER_ROLE_HEAD &&
        (message.type == UCN_CLUSTER_MSG_JOIN_REQUEST ||
         message.type == UCN_CLUSTER_MSG_KEEPALIVE ||
         message.type == UCN_CLUSTER_MSG_LEAVE ||
         message.type == UCN_CLUSTER_MSG_BACKUP_READY ||
         message.type == UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC ||
         message.type == UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ ||
         message.type == UCN_CLUSTER_MSG_BACKUP_REJECT)) {
        return UCN_ERR_ACCESS;
    }
    cluster->stats.messages_received++;
    /* CLV2-M03 (03-05): once a local epoch is in TERM_CONFLICT_WAIT, no
     * ordinary lifecycle/voting/control message may revive or mutate it.
     * Only a normal protected Head offer is admitted to the two epoch gates
     * below: same-Term conflict is idempotently silent and a strictly higher
     * same-Cluster Term can start the controlled JOIN_PENDING recovery.
     * M08 later replaces this temporary Current-FSM hold with persisted
     * fencing and quorum-based convergence. */
    if (cluster->role == UCN_CLUSTER_ROLE_TERM_CONFLICT &&
        message.type != UCN_CLUSTER_MSG_ADVERTISE &&
        message.type != UCN_CLUSTER_MSG_HEAD_DECLARE) {
        return UCN_ERR_STATE;
    }
    switch (message.type) {
        case UCN_CLUSTER_MSG_ADVERTISE:
        case UCN_CLUSTER_MSG_HEAD_DECLARE:
            if (message.role != UCN_CLUSTER_ROLE_HEAD &&
                message.role != UCN_CLUSTER_ROLE_CANDIDATE) {
                return UCN_ERR_MALFORMED;
            }
            result = observe_candidate(cluster, source, &message, now_ms);
            if (result != UCN_OK) {
                return result;
            }
            candidate = find_candidate(cluster, source);
            if (candidate != NULL && message.role == UCN_CLUSTER_ROLE_HEAD) {
                /* CLV2-M03 (03-06): candidate replay admission above is
                 * deliberately retained, then volatile detached-history
                 * rejects an older same-Cluster Term before it reaches any
                 * role-local score/join handler.  An active same-Term second
                 * Head remains eligible for the 03-05 conflict gate. */
                if (cluster_history_offer_is_stale(cluster, candidate)) {
                    cluster->stats.stale_messages++;
                    return UCN_ERR_REPLAY;
                }
                if (cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
                    candidate->cluster_id == cluster->cluster_id &&
                    candidate->term < cluster->term) {
                    cluster->stats.stale_messages++;
                    return UCN_ERR_REPLAY;
                }
                /* CLV2-M03 (03-04): priority after parser/security/source
                 * validation and candidate replay admission, but BEFORE the
                 * normal role-specific offer handler.  The helper uses the
                 * Epoch comparator, so a foreign Cluster can never arrive
                 * here merely because its numeric Term is larger. */
                /* Same-Term conflicting Head identity has a higher safety
                 * priority than ordinary score handling, and higher-Term
                 * authority comes next.  Both helpers are terminal. */
                if (process_term_conflict(cluster, candidate, now_ms) ||
                    process_higher_authority(cluster, candidate, now_ms)) {
                    return UCN_OK;
                }
                consider_head_offer(cluster, candidate, now_ms);
            }
            return UCN_OK;
        case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
            return handle_head_takeover(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_JOIN_REQUEST:
            return handle_join_request(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_JOIN_ACCEPT:
            return handle_join_accept(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_JOIN_REJECT:
            /* C07.7 P1: only a REJECT of the exact pending Head epoch
             * ends the join attempt; a stale REJECT is ignored. */
            if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING &&
                source == cluster->pending_head_node_id &&
                message.cluster_id == cluster->pending_cluster_id &&
                message.term == cluster->pending_term &&
                /* C07.7 P1: only a reject of the exact join txid ends the
                 * attempt; a stale reject of an earlier join is ignored. */
                message.nonce == cluster->pending_join_nonce) {
                /* CLV2-01-04b.4: the detach IS the JOIN_PENDING ->
                 * DETACHED_OBSERVE transition - call it BEFORE
                 * set_detached() (set_detached() writes role=DETACHED,
                 * which would violate the pre-transition derive
                 * discipline if it ran first).  set_detached()'s
                 * epoch/vote/lease/grace/known_backup clears stay
                 * site-owned and run after, in original order. */
                if (cluster_transition(cluster,
                                       UCN_CLUSTER_PHASE_JOIN_PENDING,
                                       UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                                       UCN_CLUSTER_REASON_JOIN_REJECTED,
                                       now_ms) != UCN_OK) {
                    /* Fail closed: a rejected transition (shadow mismatch
                     * / illegal pair / pre-mutated phase fields) leaves
                     * every field untouched - do NOT run the detach side
                     * effects; the next Step re-visits the join retry. */
                    return UCN_ERR_STATE;
                }
                cluster->stats.joins_rejected++;
                set_detached(cluster, now_ms,
                             cluster->config.observation_ms);
#if !defined(NDEBUG)
                /* CLV2-01-04b.4 post-commit derive assert: after the
                 * transition AND set_detached() the legacy state must
                 * still derive DETACHED_OBSERVE. */
                assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                       UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
                return UCN_OK;
            }
            return UCN_ERR_ACCESS;
        case UCN_CLUSTER_MSG_KEEPALIVE:
            return handle_keepalive(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_LEAVE:
            if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
                message.cluster_id != cluster->cluster_id ||
                message.term != cluster->term) {
                return UCN_ERR_ACCESS;
            }
            /* C07.7 P1: a replayed LEAVE with an old nonce must not evict
             * a member that has since re-joined and advanced its nonce. */
            {
                ucn_cluster_member_t *member = primary_member_find(cluster, source);

                if (member == NULL) {
                    return UCN_ERR_NOT_FOUND;
                }
                if (message.nonce <= member->last_nonce) {
                    cluster->stats.stale_messages++;
                    return UCN_ERR_REPLAY;
                }
            }
            remove_member(cluster, source, now_ms);
            return UCN_OK;
        case UCN_CLUSTER_MSG_HEAD_STEPDOWN:
            /* C07.7 P1: the Backup must also leave with the members on an
             * ordered stepdown (it cannot ACK its own takeover against a
             * Head that is deliberately switching away).  The epoch must
             * match to fence stale STEPDOWN frames. */
            if ((cluster->role == UCN_CLUSTER_ROLE_MEMBER ||
                 cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING ||
                 cluster->role == UCN_CLUSTER_ROLE_BACKUP) &&
                source == cluster->head_node_id &&
                message.cluster_id == cluster->cluster_id &&
                message.term == cluster->term &&
                /* C07.7 P1: a replayed STEPDOWN of the same epoch is
                 * ignored via the strictly increasing stepdown nonce. */
                message.nonce > cluster->last_stepdown_nonce) {
                if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING) {
                    /* CLV2-01-04b.6 (human MAJOR): the JOIN_PENDING
                     * sub-branch consumes the anti-replay fence ONLY AFTER
                     * the transition succeeds.  Previously the nonce
                     * advanced before the role branch, so a rejected
                     * transition (shadow/legacy drift) still consumed the
                     * fence - a half-commit contradicting 'rejected
                     * transition leaves every field untouched'.
                     * Transition FIRST: the legacy detach writes
                     * role=DETACHED, which would trip the pre-transition
                     * derive check; set_detached()'s role rewrite
                     * afterwards is redundant-but-harmless (its epoch/
                     * vote/lease/deadline/known_backup clears stay
                     * site-owned).  Reason is EXPLICIT STEPDOWN_ORDERED:
                     * the BEST-EFFORT fallback for this pair would mint
                     * JOIN_REJECTED, semantically wrong for an ordered
                     * stepdown. */
                    if (cluster_transition(cluster,
                                           UCN_CLUSTER_PHASE_JOIN_PENDING,
                                           UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                                           UCN_CLUSTER_REASON_STEPDOWN_ORDERED,
                                           now_ms) != UCN_OK) {
                        /* Fail closed: a rejected transition leaves every
                         * field untouched INCLUDING the anti-replay fence
                         * - the node stays JOIN_PENDING and a later
                         * well-formed STEPDOWN may still be accepted. */
                        return UCN_ERR_STATE;
                    }
                    cluster->last_stepdown_nonce = message.nonce;
                    set_detached(cluster, now_ms,
                                 cluster->config.observation_ms);
#if !defined(NDEBUG)
                    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                           UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
                } else if (cluster->role == UCN_CLUSTER_ROLE_MEMBER) {
                    /* CLV2-01-04c.5: the MEMBER sub-branch migrates with
                     * the b.6 fail-closed lesson.  The old phase is
                     * derived from the PRE-CALL state (role==MEMBER: an
                     * armed grace deadline means MEMBER_TAKEOVER_GRACE,
                     * otherwise MEMBER_ACTIVE) and the transition runs
                     * FIRST - set_detached() writes role=DETACHED, which
                     * would trip the pre-transition derive check if it ran
                     * first.  Reason is EXPLICIT STEPDOWN_ORDERED (the
                     * BEST-EFFORT fallback for this pair would mint
                     * RESET/GRACE_TIMEOUT, semantically wrong for an
                     * ordered stepdown).  On success the site consumes the
                     * anti-replay fence and runs set_detached() in the
                     * original order; set_detached()'s epoch/vote/lease/
                     * deadline/known_backup clears stay site-owned. */
                    ucn_cluster_phase_t old_phase =
                        (cluster->head_grace_deadline_ms != 0U)
                            ? UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE
                            : UCN_CLUSTER_PHASE_MEMBER_ACTIVE;

                    if (cluster_transition(cluster, old_phase,
                                           UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                                           UCN_CLUSTER_REASON_STEPDOWN_ORDERED,
                                           now_ms) != UCN_OK) {
                        /* Fail closed: a rejected transition leaves every
                         * field untouched INCLUDING the anti-replay fence
                         * - the node stays MEMBER and a later well-formed
                         * STEPDOWN may still be accepted. */
                        return UCN_ERR_STATE;
                    }
                    cluster->last_stepdown_nonce = message.nonce;
                    set_detached(cluster, now_ms,
                                 cluster->config.observation_ms);
#if !defined(NDEBUG)
                    /* CLV2-01-04c.5 post-commit derive assert: after the
                     * transition AND set_detached() the legacy state must
                     * still derive DETACHED_OBSERVE. */
                    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                           UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
                } else {
                    /* CLV2-01-04e.7 (human audit MAJOR 2.C): the BACKUP
                     * sub-branch migrates with the b.6 fail-closed lesson.
                     * The old phase is derived from the PRE-CALL state
                     * (takeover_active -> BACKUP_TAKEOVER, ready ->
                     * BACKUP_READY, else BACKUP_SYNCING - cluster_phase_
                     * from_legacy_state() IS this derivation for role
                     * BACKUP) and the transition runs FIRST with the
                     * EXPLICIT STEPDOWN_ORDERED reason (an ordered
                     * stepdown is an ordered stepdown regardless of role;
                     * the BEST-EFFORT fallback would mint PRIMARY_LOST /
                     * TAKEOVER_TIMEOUT, semantically wrong).  backup_
                     * clear_sync() writes role=DETACHED, which would trip
                     * the pre-transition derive check if it ran first;
                     * its role rewrite afterwards is redundant-but-harmless
                     * (its mirror clears stay site-owned).  On success the
                     * site consumes the anti-replay fence and runs
                     * backup_clear_sync() in the original order; the
                     * M01.0.2 takeover_active && syncing combo is a valid
                     * BACKUP_TAKEOVER pre-state and must never be rejected
                     * for phase reasons (a late Type12 during takeover is
                     * fine - the DIRECT edge TAKEOVER->DETACHED exists for
                     * the stepdown path). */
                    ucn_cluster_phase_t pre_phase =
                        cluster_phase_from_legacy_state(cluster, now_ms);

                    if (cluster_transition(cluster, pre_phase,
                                           UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                                           UCN_CLUSTER_REASON_STEPDOWN_ORDERED,
                                           now_ms) != UCN_OK) {
                        /* Fail closed: a rejected transition leaves every
                         * field untouched INCLUDING the anti-replay fence
                         * - the node stays BACKUP and a later well-formed
                         * STEPDOWN may still be accepted. */
                        return UCN_ERR_STATE;
                    }
                    cluster->last_stepdown_nonce = message.nonce;
                    backup_clear_sync(cluster, now_ms);
#if !defined(NDEBUG)
                    /* CLV2-01-04e.7 post-commit derive assert: after the
                     * transition AND backup_clear_sync() the legacy state
                     * must still derive DETACHED_OBSERVE. */
                    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                           UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
                }
                return UCN_OK;
            }
            return UCN_ERR_ACCESS;
        case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
            return handle_backup_assign(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_BACKUP_READY:
            return handle_backup_ready(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
            return handle_backup_member_sync(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
            return handle_primary_heartbeat(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
            return handle_backup_resync_req(cluster, source, &message);
        case UCN_CLUSTER_MSG_BACKUP_REJECT:
            return handle_backup_reject(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
            return handle_takeover_prepare(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_TAKEOVER_ACK:
            return handle_takeover_ack(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_RECOVERY_DECLARE:
            return handle_recovery_declare(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_RECOVERY_ACK:
            return handle_recovery_ack(cluster, source, &message, now_ms);
        default:
            return UCN_ERR_UNSUPPORTED;
    }
}

ucn_result_t ucn_cluster_receive(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    bool protected_control,
    const uint8_t *payload,
    size_t payload_length)
{
    ucn_cluster_transition_reason_t hint = UCN_CLUSTER_REASON_UNKNOWN;
    ucn_result_t result;
    bool authority_fenced = false;

    /* Peek the wire type (payload byte 1) for a best-effort reason hint;
     * the hint is only used when the exact diff table has no entry. */
    if (cluster != NULL && payload != NULL && payload_length >= 2U &&
        cluster->config.enabled) {
        hint = cluster_rx_reason_from_type(
            (ucn_cluster_message_type_t)payload[1U]);
    }
    result = ucn_cluster_receive_inner(cluster, source, protected_control,
                                       payload, payload_length,
                                       &authority_fenced);
    /* CLV2-01-02: keep the shadow mirror aligned after every RX that could
     * have changed state (rejections like ERR_REPLAY may still have moved
     * backup sync flags, so sync on everything except ARGUMENT). */
    if (!authority_fenced && cluster != NULL && cluster->config.enabled &&
        result != UCN_ERR_ARGUMENT) {
        cluster_shadow_sync(cluster, hint);
    }
    return result;
}

static ucn_result_t start_election_after_persistence(
    ucn_cluster_t *cluster,
    const ucn_cluster_persist_state_t *durable_state,
    uint32_t now_ms)
{
    const ucn_cluster_epoch_t *epoch;

    if (cluster == NULL || durable_state == NULL ||
        !durable_state->has_active_epoch ||
        durable_state->active_epoch.head_node_id != cluster->config.local_node_id) {
        return UCN_ERR_STATE;
    }
    epoch = &durable_state->active_epoch;
    /* CLV2-01-04b.1: the role write IS the DETACHED_OBSERVE -> ELECTION
     * transition.  The ONLY call site is ucn_cluster_step_inner()'s
     * DETACHED + !recovery_eligible branch (L5075), so the claimed old
     * phase is always DETACHED_OBSERVE; all other side effects below
     * stay in the site, in their original order. */
    if (cluster_transition(cluster, UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                           UCN_CLUSTER_PHASE_ELECTION,
                           UCN_CLUSTER_REASON_ELECTION_STARTED,
                           now_ms) != UCN_OK) {
        /* Fail closed per the migration contract: a rejected transition
         * (shadow mismatch / illegal pair / pre-mutated phase fields)
         * leaves every field untouched, so do NOT run the election side
         * effects on a non-CANDIDATE node.  The next Step re-visits the
         * DETACHED observation branch. */
        return UCN_ERR_STATE;
    }
    cluster->cluster_id = epoch->cluster_id;
    cluster->term = epoch->term;
    cluster->head_node_id = cluster->config.local_node_id;
    cluster->current_head_score = cluster->config.head_score;
    cluster->role_since_ms = now_ms;
    cluster->election_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.election_window_ms);
    cluster->next_advertise_ms = now_ms;
    cluster->stats.elections_started++;
    /* CLV2-01-04b (roadmap note): post-commit derive assert - after the
     * transition AND every site side effect, the legacy state must still
     * derive ELECTION (derive depends only on role == CANDIDATE). */
#if !defined(NDEBUG)
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_ELECTION);
#endif
    return UCN_OK;
}

static ucn_result_t start_election(ucn_cluster_t *cluster, uint32_t now_ms)
{
    ucn_cluster_epoch_t epoch;
    ucn_cluster_persist_state_t durable_state;
    uint32_t election_cluster_id;
    uint32_t next_term;
    bool committed;
    ucn_result_t result;

    result = cluster_make_next_id(cluster, UCN_CLUSTER_ID_PURPOSE_ELECTION,
                                  cluster->last_cluster_id,
                                  cluster->max_seen_term,
                                  &election_cluster_id);
    if (result != UCN_OK) {
        return result;
    }
    result = cluster_serial_next_checked(cluster->term, &next_term);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(&epoch, 0, sizeof(epoch));
    epoch.cluster_id = election_cluster_id;
    epoch.term = next_term;
    epoch.head_node_id = cluster->config.local_node_id;
    /* Validate the irreversible FSM edge before Flash I/O.  Once the record
     * is durable, a transition rejection must not leave a fresh epoch with no
     * local continuation path. */
    result = cluster_transition_preflight(cluster,
                                          UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                                          UCN_CLUSTER_PHASE_ELECTION,
                                          now_ms);
    if (result != UCN_OK) {
        return result;
    }
    result = cluster_persistence_begin_epoch(
        cluster, &epoch, CLUSTER_PERSIST_ACTION_ELECTION_START, 0U,
        &committed, &durable_state);
    if (result != UCN_OK || !committed) {
        return result;
    }
    return start_election_after_persistence(cluster, &durable_state, now_ms);
}

static ucn_result_t declare_recovery_after_persistence(
    ucn_cluster_t *cluster,
    const ucn_cluster_persist_state_t *durable_state,
    uint32_t now_ms)
{
    const ucn_cluster_epoch_t *epoch;

    if (cluster == NULL || durable_state == NULL ||
        !durable_state->has_active_epoch ||
        durable_state->active_epoch.term != 1U ||
        durable_state->active_epoch.head_node_id != cluster->config.local_node_id) {
        return UCN_ERR_STATE;
    }
    epoch = &durable_state->active_epoch;
    if (cluster_transition(cluster, UCN_CLUSTER_PHASE_RECOVERY_ELECTION,
                           UCN_CLUSTER_PHASE_RECOVERY_HEAD,
                           UCN_CLUSTER_REASON_RECOVERY_WIN,
                           now_ms) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    declare_recovery_head(cluster, epoch->cluster_id, now_ms);
#if !defined(NDEBUG)
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_RECOVERY_HEAD);
#endif
    return UCN_OK;
}

static void complete_election(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    ucn_node_id_t best_node = cluster->config.local_node_id;
    uint16_t best_score = cluster->config.head_score;
    ucn_cluster_phase_t target_phase;

    for (index = 0U; index < UCN_CLUSTER_MAX_CANDIDATES; ++index) {
        const ucn_cluster_candidate_t *candidate = &cluster->candidates[index];

        if (!candidate->occupied ||
            ucn_deadline_expired(now_ms, candidate->expires_at_ms) ||
            candidate->role != UCN_CLUSTER_ROLE_CANDIDATE) {
            continue;
        }
        if (candidate_better(candidate->head_score, candidate->head_node_id,
                             best_score, best_node)) {
            best_score = candidate->head_score;
            best_node = candidate->head_node_id;
        }
    }
    if (best_node != cluster->config.local_node_id) {
        /* CLV2-01-04b.2 loss path (human TRAP 1): transition FIRST,
         * set_detached() second - set_detached() writes role=DETACHED,
         * which would violate the pre-transition derive discipline if it
         * ran before the transition.  After the transition its role
         * rewrite is redundant-but-harmless; its epoch/vote/lease/
         * deadline/grace/known_backup clears stay site-owned. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_ELECTION,
                               UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                               UCN_CLUSTER_REASON_ELECTION_LOST,
                               now_ms) != UCN_OK) {
            /* Fail closed: rejected transition leaves every field
             * untouched - do not run the detach side effects. */
            return;
        }
        set_detached(cluster, now_ms, cluster->config.observation_ms);
#if !defined(NDEBUG)
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
        return;
    }
    /* CLV2-01-04b.2 win path (human TRAP 2): the destination HEAD
     * sub-phase is dispatched from the PRE-CALL preserved backup_* state
     * (read BEFORE the transition - apply_legacy(HEAD_NO_BACKUP) forces
     * backup_node_id=0, so post-call reads would be wrong). */
    if (cluster->backup_node_id == 0U) {
        target_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    } else if (cluster->backup_ready) {
        target_phase = UCN_CLUSTER_PHASE_HEAD_STABLE;
    } else if (cluster->backup_assign_pending) {
        target_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
    } else {
        target_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    }
    if (cluster_transition(cluster, UCN_CLUSTER_PHASE_ELECTION,
                           target_phase, UCN_CLUSTER_REASON_ELECTION_WON,
                           now_ms) != UCN_OK) {
        /* Fail closed: do not run the win side effects on a non-HEAD
         * node. */
        return;
    }
    cluster->role_since_ms = now_ms;
    cluster->election_deadline_ms = 0U;
    cluster->next_advertise_ms = now_ms;
    cluster_history_note_stable_epoch(cluster, cluster->cluster_id,
                                      cluster->term,
                                      cluster->head_node_id);
    cluster->stats.elections_won++;
#if !defined(NDEBUG)
    assert(cluster_phase_from_legacy_state(cluster, now_ms) == target_phase);
#endif
}

/* A deferred action is applied only after the persistence runtime has loaded
 * the record again and matched its completed-operation journal.  The pending
 * descriptor itself never caches mutable FSM state. */
static ucn_result_t apply_persistence_resolution(
    ucn_cluster_t *cluster,
    const cluster_persistence_resolution_t *resolution,
    uint32_t now_ms)
{
    ucn_result_t result;

    if (cluster == NULL || resolution == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    switch (resolution->action) {
    case CLUSTER_PERSIST_ACTION_ELECTION_START:
        return start_election_after_persistence(cluster,
                                                &resolution->durable_state,
                                                now_ms);
    case CLUSTER_PERSIST_ACTION_BACKUP_CHALLENGE:
        return backup_challenge_after_persistence(cluster,
                                                  &resolution->durable_state,
                                                  now_ms);
    case CLUSTER_PERSIST_ACTION_TAKEOVER_COMMIT:
        return complete_takeover_after_persistence(cluster,
                                                   &resolution->durable_state,
                                                   now_ms);
    case CLUSTER_PERSIST_ACTION_RECOVERY_DECLARE:
        return declare_recovery_after_persistence(cluster,
                                                  &resolution->durable_state,
                                                  now_ms);
    case CLUSTER_PERSIST_ACTION_TAKEOVER_ACK:
        result = send_takeover_ack_after_persistence(
            cluster, resolution->destination, &resolution->durable_state);
        return result;
    case CLUSTER_PERSIST_ACTION_BOOT_INCARNATION:
        if (resolution->durable_state.boot_incarnation == 0U) {
            return UCN_ERR_STATE;
        }
        cluster->config.cluster_id_incarnation =
            resolution->durable_state.boot_incarnation;
        return UCN_OK;
    case CLUSTER_PERSIST_ACTION_NONE:
        return UCN_OK;
    default:
        /* An action is registered only after its owning continuation is
         * compiled into this dispatcher. */
        return UCN_ERR_STATE;
    }
}

static size_t admitted_peer_count(const ucn_cluster_t *cluster)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < UCN_CLUSTER_MAX_PEERS; ++index) {
        if (cluster->peers[index].occupied &&
            cluster->peers[index].neighbor_state == UCN_NEIGHBOR_ADMITTED) {
            ++count;
        }
    }
    return count;
}

/* Head-side Backup control: assignment announcement, heartbeat and one
 * snapshot record per step.  The cursor advances only after the message has
 * entered the normal UCN send path, so Token Bucket back-pressure never
 * silently loses the selected Backup identity. */
static void send_next_advertisement(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t examined;
    size_t peer_count = admitted_peer_count(cluster);
    uint32_t slice_ms;

    if (peer_count == 0U) {
        cluster->next_advertise_ms = ucn_deadline_from_now(
            now_ms, cluster->config.advertise_interval_ms);
        return;
    }
    for (examined = 0U; examined < UCN_CLUSTER_MAX_PEERS; ++examined) {
        size_t index = (cluster->advertise_cursor + examined) %
                       UCN_CLUSTER_MAX_PEERS;
        const ucn_cluster_peer_t *peer = &cluster->peers[index];

        if (!peer->occupied ||
            peer->neighbor_state != UCN_NEIGHBOR_ADMITTED) {
            continue;
        }
        (void)send_message(cluster, peer->node_id, UCN_CLUSTER_MSG_ADVERTISE,
                           cluster->role, cluster->cluster_id, cluster->term,
                           cluster->config.local_node_id,
                           cluster->config.head_score,
                           cluster->role == UCN_CLUSTER_ROLE_HEAD ?
                               primary_member_available_capacity(cluster) :
                               cluster->config.member_capacity);
        cluster->advertise_cursor =
            (uint8_t)((index + 1U) % UCN_CLUSTER_MAX_PEERS);
        break;
    }
    /* The advertised interval is one full peer refresh cycle.  C07.4's
     * throttled Backup snapshot keeps this normal discovery traffic from
     * being starved, while preserving enough repeated Head offers for a
     * large lossy cluster to converge within one lease. */
    slice_ms = cluster->config.advertise_interval_ms / (uint32_t)peer_count;
    if (slice_ms == 0U) {
        slice_ms = 1U;
    }
    cluster->next_advertise_ms = ucn_deadline_from_now(now_ms, slice_ms);
}




static ucn_result_t ucn_cluster_step_inner(ucn_cluster_t *cluster)
{
    uint32_t now_ms;
    ucn_result_t result;
    bool persistence_resolved;
    cluster_persistence_resolution_t persistence_resolution;

    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!cluster->config.enabled) {
        return UCN_OK;
    }
    now_ms = cluster_now(cluster);
    result = cluster_persistence_poll(cluster, &persistence_resolved,
                                      &persistence_resolution);
    if (result != UCN_OK) {
        return result;
    }
    if (persistence_resolved) {
        result = apply_persistence_resolution(cluster, &persistence_resolution,
                                              now_ms);
        if (result != UCN_OK) {
            if (persistence_resolution.action ==
                    CLUSTER_PERSIST_ACTION_TAKEOVER_ACK &&
                cluster_persistence_takeover_ack_send_is_retryable(result) &&
                cluster->persistence_retry_pending) {
                return result;
            }
            cluster_persistence_fail_closed(cluster, result);
            return result;
        }
    }
    if (cluster->persistence_retry_pending) {
        return cluster_persistence_retry_pending(cluster);
    }
    if (cluster_persistence_progress_blocked(cluster)) {
        return cluster->persistence_faulted ? cluster->persistence_failure :
                                              UCN_OK;
    }
    /* The M08 Owner must run before any role-driven Head send path below.
     * Thus a loss observed in this Step revokes Authority before advertise,
     * Directory, Backup or Join authority traffic can consume a token. */
    if (cluster->authority_runtime != NULL) {
        result = ucn_cluster_authority_runtime_preflight(
            cluster->authority_runtime, now_ms);
        if (result != UCN_OK) {
            return result;
        }
    }
    /* CLV2-M03 (03-05): this is a local-only safety wait.  It publishes no
     * Head/Member/Backup control traffic, starts no election/takeover and
     * cannot leave on same-Term traffic; only the RX higher-authority gate
     * may transition it to JOIN_PENDING. */
    if (cluster->role == UCN_CLUSTER_ROLE_TERM_CONFLICT) {
        return UCN_OK;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
        ucn_deadline_expired(now_ms, cluster->head_lease_expires_at_ms)) {
        /* Grace period: a Backup may be mid-takeover exactly when the
         * member lease lapses; stay MEMBER briefly so the member can still
         * ACK TAKEOVER_PREPARE / switch on HEAD_TAKEOVER before detaching. */
        if (cluster->head_grace_deadline_ms == 0U) {
            /* CLV2-01-04c.1: arming the grace deadline IS the
             * MEMBER_ACTIVE -> MEMBER_TAKEOVER_GRACE transition - call it
             * BEFORE any phase-relevant write.  apply_legacy(GRACE)
             * writes role + the deadline_from_now(now, keepalive) the
             * site used (verified identical), so the site rewrite below
             * is idempotent with the same value (kept authoritative,
             * original order). */
            if (cluster_transition(cluster,
                                   UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                                   UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                                   UCN_CLUSTER_REASON_HEAD_LEASE_EXPIRED,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition (shadow mismatch /
                 * illegal pair / pre-mutated phase fields) leaves every
                 * field untouched - do NOT arm grace or count the expiry;
                 * the next Step re-visits the lease check. */
                return UCN_ERR_STATE;
            }
            cluster->stats.head_leases_expired++;
            cluster->head_grace_deadline_ms = ucn_deadline_from_now(
                now_ms, cluster->config.keepalive_interval_ms);
#if !defined(NDEBUG)
            /* CLV2-01-04c.1 post-commit derive assert: after the
             * transition AND the site's deadline write the node must
             * still derive MEMBER_TAKEOVER_GRACE (role == MEMBER with an
             * armed grace deadline). */
            assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                   UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
#endif
        } else if (ucn_deadline_expired(now_ms,
                                       cluster->head_grace_deadline_ms)) {
            /* CLV2-01-04c.3: the grace timeout IS the
             * MEMBER_TAKEOVER_GRACE -> RECOVERY_OBSERVE transition - call
             * it FIRST.  apply_legacy(RECOVERY_OBSERVE) writes role=
             * DETACHED + eligible=true + backoff=0 + grace=0 +
             * known_backup_*=0, so the site's eligible=true rewrite below
             * is redundant-but-harmless and set_detached() re-applies the
             * same epoch/vote/lease/grace clears with the observation
             * deadline.  The pre-derive requires the legacy to STILL
             * derive MEMBER_TAKEOVER_GRACE (eligible must stay false), so
             * recovery_eligible is written only AFTER the call. */
            if (cluster_transition(cluster,
                                   UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                                   UCN_CLUSTER_PHASE_RECOVERY_OBSERVE,
                                   UCN_CLUSTER_REASON_GRACE_TIMEOUT,
                                   now_ms) != UCN_OK) {
                /* Fail closed: do NOT detach on a rejected transition;
                 * the node stays in grace and the next Step re-visits. */
                return UCN_ERR_STATE;
            }
            cluster->recovery_eligible = true;
            set_detached(cluster, now_ms,
                         cluster->config.recovery_observation_ms);
#if !defined(NDEBUG)
            /* CLV2-01-04c.3 post-commit derive assert: after the
             * transition AND every site effect the node must derive
             * RECOVERY_OBSERVE (role == DETACHED, eligible, no backoff). */
            assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                   UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
#endif
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_HEAD) {
        expire_members(cluster, now_ms);
        if (cluster->next_advertise_ms == 0U ||
            ucn_deadline_expired(now_ms, cluster->next_advertise_ms)) {
            /* Discovery / lease renewal has priority over background backup
             * replication, otherwise a lossy large cluster can exhaust its
             * own control budget before pending JOINs are observed. */
            send_next_advertisement(cluster, now_ms);
        }
        if (cluster->backup_node_id == 0U) {
            /* C07.7 P1: automatically re-select a Backup when the previous
             * one left or expired, even without a new JOIN; the selection
             * itself stays candidate/score ordered and idempotent. */
            assign_backup(cluster, now_ms);
        }
        if (cluster->backup_node_id != 0U && !cluster->backup_assign_pending &&
            (cluster->next_backup_assign_ms == 0U ||
             ucn_deadline_expired(now_ms,
                                  cluster->next_backup_assign_ms))) {
            start_backup_assignment_cycle(cluster, now_ms);
        }
        result = send_backup_delta_step(cluster);
        if (result != UCN_OK) {
            return result;
        }
        send_backup_heartbeat(cluster, now_ms);
        send_takeover_announce_step(cluster);
        send_backup_assignment_step(cluster, now_ms);
        result = send_backup_snapshot_step(cluster);
        if (result != UCN_OK) {
            return result;
        }
        /* Bounded snapshot retransmit: if a frame was dropped the Backup
         * never becomes READY, so resend the snapshot on a fixed timer. */
        if (cluster->backup_node_id != 0U && !cluster->backup_ready &&
            cluster->backup_sync_cursor > primary_member_count_u16(cluster) + 1U &&
            ucn_deadline_expired(now_ms,
                                  cluster->backup_resync_deadline_ms)) {
            /* The snapshot completed but a frame was dropped: resend it. */
            backup_resync(cluster);
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP &&
        cluster->backup_primary_deadline_ms != 0U &&
        ucn_deadline_expired(now_ms, cluster->backup_primary_deadline_ms)) {
        cluster->backup_missed_heartbeats++;
        if (cluster->backup_missed_heartbeats >= UCN_CLUSTER_BACKUP_MISS_LIMIT) {
            if (cluster->backup_ready && !cluster->backup_takeover_active) {
                if (ucn_deadline_expired(
                        now_ms, cluster->backup_primary_lease_deadline_ms)) {
                    /* §5.1: also wait for the Primary lease to expire so
                     * a burst of dropped heartbeats cannot preempt a live
                     * Head. */
                    start_takeover(cluster, now_ms);
                }
                /* else: keep waiting for the lease; stay BACKUP. */
            } else if (!cluster->backup_takeover_active) {
                /* CLV2-01-04f.2: the missed-heartbeat limit hit IS the
                 * BACKUP_SYNCING -> RECOVERY_OBSERVE transition (the
                 * eligible branch only fires when !ready && !takeover) -
                 * call it FIRST, UNCONDITIONALLY, fail closed.
                 * apply_legacy(RECOVERY_OBSERVE) writes role=DETACHED +
                 * recovery_eligible=true + backoff/grace/known_backup_*=0,
                 * so the site's recovery_eligible=true below is
                 * redundant-but-harmless; backup_clear_sync() then
                 * re-applies the mirror clears + set_detached() in
                 * original order.  Reason PRIMARY_LOST (the primary's
                 * heartbeat stream failed), NOT TAKEOVER_TIMEOUT (no
                 * takeover here).  On a rejected transition NOTHING of
                 * the branch runs - the backup stays, and the next step
                 * re-visits the still-expired deadline. */
                if (cluster_transition(cluster,
                                       UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                                       UCN_CLUSTER_PHASE_RECOVERY_OBSERVE,
                                       UCN_CLUSTER_REASON_PRIMARY_LOST,
                                       now_ms) != UCN_OK) {
                    /* Fail closed: do NOT count the expiry or clear the
                     * mirror on a rejected transition (shadow mismatch /
                     * illegal pair / pre-mutated phase fields). */
                    return UCN_ERR_STATE;
                }
                cluster->stats.head_leases_expired++;
                cluster->recovery_eligible = true;
                backup_clear_sync(cluster, now_ms);
#if !defined(NDEBUG)
                /* CLV2-01-04f.2 post-commit derive assert: after the
                 * transition AND every site effect the node must derive
                 * RECOVERY_OBSERVE (role == DETACHED, eligible, no
                 * backoff). */
                assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                       UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
#endif
                return UCN_OK;
            }
        }
        cluster->backup_primary_deadline_ms =
            ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP &&
        cluster->backup_takeover_active &&
        ucn_deadline_expired(now_ms, cluster->backup_takeover_deadline_ms)) {
        /* CLV2-01-04e.5: the expired takeover window IS the BACKUP_TAKEOVER
         * -> DETACHED_OBSERVE transition - call it FIRST, UNCONDITIONALLY,
         * fail closed.  apply_legacy(DETACHED_OBSERVE) writes role=DETACHED
         * + recovery_eligible=false + backoff/grace/known_backup_*=0, so
         * the site does NOT set eligible (matching Current: a takeover-
         * active Backup is never recovery-eligible); backup_clear_sync()
         * then re-applies the mirror clears + set_detached() in original
         * order.  On a rejected transition NOTHING runs - the takeover
         * stays active and the next step re-visits the deadline. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                               UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                               UCN_CLUSTER_REASON_TAKEOVER_TIMEOUT,
                               now_ms) != UCN_OK) {
            /* Fail closed: do NOT count the expiry or clear the mirror on
             * a rejected transition (shadow mismatch / illegal pair /
             * pre-mutated phase fields). */
            return UCN_ERR_STATE;
        }
        /* Majority not reached in the window -> fall back to re-election. */
        cluster->stats.head_leases_expired++;
        backup_clear_sync(cluster, now_ms);
#if !defined(NDEBUG)
        /* CLV2-01-04e.5 post-commit derive assert: after the transition
         * AND every site effect the node must derive DETACHED_OBSERVE
         * (role == DETACHED, recovery_eligible == false, no backoff). */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
        return UCN_OK;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP &&
        cluster->backup_takeover_active) {
        send_takeover_prepare_step(cluster);
    }
    if (cluster->role == UCN_CLUSTER_ROLE_DETACHED &&
        cluster->config.head_capable &&
         ucn_deadline_expired(now_ms, cluster->observation_deadline_ms)) {
        if (cluster->recovery_eligible &&
            cluster->recovery_cooldown_until_ms != 0U &&
            !ucn_deadline_expired(now_ms,
                                  cluster->recovery_cooldown_until_ms)) {
            /* Still cooling down after a Recovery stepdown: wait. */
        } else if (cluster->recovery_eligible) {
            if (cluster->recovery_backoff_deadline_ms == 0U) {
                /* CLV2-01-04f: arming a NON-ZERO backoff IS the
                 * RECOVERY_OBSERVE -> RECOVERY_ELECTION transition - call
                 * it FIRST, UNCONDITIONALLY on the legacy event, fail
                 * closed (Shadow-Guard: the legacy event decides WHICH
                 * transition, the shadow is only validated here).
                 * apply_legacy(RECOVERY_ELECTION) writes role=DETACHED +
                 * eligible=true ONLY; the caller-provided
                 * recovery_nonce / backoff_deadline writes stay in
                 * start_recovery_backoff() (CLV2-01-04a.1 Item 4: never
                 * auto-mint the backoff).  Degenerate-config guard: a
                 * node_id that is a multiple of recovery_backoff_max_ms
                 * computes backoff 0, and ucn_deadline_from_now(now, 0)
                 * returns 0 (ucn_duration_is_valid rejects 0), so the
                 * derive would stay RECOVERY_OBSERVE - a committed
                 * RECOVERY_ELECTION would trip the derive assert below /
                 * shadow-flap in release.  Old code spun in RECOVERY_OBSERVE
                 * (re-arming the zero deadline every Step) without ever
                 * minting a phase change; preserve that EXACTLY: no
                 * transition when the computed backoff is zero, while
                 * start_recovery_backoff() still runs (the nonce bump is
                 * preserved) and the phase stays put. */
                {
                    /* CLV2-01-04f: the guard below is a LEGACY-side
                     * discriminator (the event "arm a non-zero backoff"
                     * only exists when compute_recovery_backoff != 0); it
                     * never reads the shadow mirror, so the Shadow-Guard
                     * rule is intact. */
                    bool backoff_is_nonzero =
                        compute_recovery_backoff(cluster) != 0U;

                    if (backoff_is_nonzero) {
                        if (cluster_transition(
                                cluster, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE,
                                UCN_CLUSTER_PHASE_RECOVERY_ELECTION,
                                UCN_CLUSTER_REASON_RECOVERY_BACKOFF,
                                now_ms) != UCN_OK) {
                            /* Fail closed: do NOT arm the backoff on a
                             * rejected transition (shadow mismatch /
                             * illegal pair / pre-mutated phase fields);
                             * the node stays observing and the next Step
                             * re-visits the deadline. */
                            return UCN_ERR_STATE;
                        }
                    }
                    /* The site still owns the armed backoff (nonce +
                     * deadline), in original order; in the degenerate
                     * zero-backoff config it re-arms a zero deadline
                     * exactly as before (nonce bump preserved) and the
                     * phase stays RECOVERY_OBSERVE. */
                    start_recovery_backoff(cluster, now_ms);
#if !defined(NDEBUG)
                    /* CLV2-01-04f post-commit derive assert: after the
                     * transition AND the site-owned nonce/deadline writes
                     * the node must derive RECOVERY_ELECTION (role
                     * DETACHED + eligible + armed NON-ZERO backoff).  Only
                     * asserted when the transition RAN - the degenerate
                     * zero-backoff config skips it entirely (see the
                     * guard comment above) and derives RECOVERY_OBSERVE. */
                    if (backoff_is_nonzero) {
                        assert(cluster_phase_from_legacy_state(cluster,
                                                               now_ms) ==
                               UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
                    }
#endif
                }
            } else if (ucn_deadline_expired(
                           now_ms, cluster->recovery_backoff_deadline_ms)) {
                if (recovery_quorum_met(cluster)) {
                    uint32_t recovery_cluster_id;
                    ucn_cluster_epoch_t recovery_epoch;
                    ucn_cluster_persist_state_t durable_state;
                    bool committed;

                    result = cluster_make_next_id(
                        cluster, UCN_CLUSTER_ID_PURPOSE_RECOVERY,
                        cluster->last_cluster_id, cluster->max_seen_term,
                        &recovery_cluster_id);
                    if (result != UCN_OK) {
                        return result;
                    }
                    (void)memset(&recovery_epoch, 0, sizeof(recovery_epoch));
                    recovery_epoch.cluster_id = recovery_cluster_id;
                    recovery_epoch.term = 1U;
                    recovery_epoch.head_node_id = cluster->config.local_node_id;
                    /* A recovery declaration is a new authority promise.  Its
                     * Cluster-create record must be durable before either the
                     * RECOVERY_HEAD phase or RECOVERY_DECLARE becomes visible. */
                    result = cluster_transition_preflight(
                        cluster, UCN_CLUSTER_PHASE_RECOVERY_ELECTION,
                        UCN_CLUSTER_PHASE_RECOVERY_HEAD, now_ms);
                    if (result != UCN_OK) {
                        return result;
                    }
                    result = cluster_persistence_begin_epoch(
                        cluster, &recovery_epoch,
                        CLUSTER_PERSIST_ACTION_RECOVERY_DECLARE, 0U,
                        &committed, &durable_state);
                    if (result != UCN_OK || !committed) {
                        return result;
                    }
                    result = declare_recovery_after_persistence(
                        cluster, &durable_state, now_ms);
                    if (result != UCN_OK) {
                        cluster_persistence_fail_closed(cluster, result);
                        return result;
                    }
                } else {
                    /* C07.7 P0-2: no visible quorum (fully isolated node):
                     * do NOT self-declare; retry after another bounded
                     * backoff so the domain converges when peers return. */
                    cluster->recovery_backoff_deadline_ms =
                        ucn_deadline_from_now(
                            now_ms, cluster->config.recovery_backoff_max_ms);
                }
            }
        } else {
            result = start_election(cluster, now_ms);
            if (result != UCN_OK) {
                return result;
            }
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_CANDIDATE &&
        ucn_deadline_expired(now_ms, cluster->election_deadline_ms)) {
        complete_election(cluster, now_ms);
    }
    if (cluster->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        /* C07.7 P0-1: the recovery Cluster is a real (short-lived) Cluster:
         * expire members and periodically re-advertise the declaration so
         * surviving members keep their leases and late survivors can join. */
        expire_members(cluster, now_ms);
        if (cluster->next_advertise_ms == 0U ||
            ucn_deadline_expired(now_ms, cluster->next_advertise_ms)) {
            cluster->next_advertise_ms = ucn_deadline_from_now(
                now_ms, cluster->config.advertise_interval_ms);
            (void)send_recovery_declare(cluster);
        }
        if (ucn_deadline_expired(now_ms, cluster->recovery_deadline_ms)) {
            /* CLV2-01-04f: the expired TTL IS the RECOVERY_HEAD ->
             * RECOVERY_OBSERVE transition - call it FIRST, UNCONDITIONALLY,
             * fail closed.  apply_legacy(RECOVERY_OBSERVE) writes role=
             * DETACHED + eligible=true + backoff=0 + grace=0 +
             * known_backup=0; stepdown_recovery_head()'s cooldown/clears +
             * set_detached() (role rewrite redundant-but-harmless, b.6/c.5
             * precedent) stay site-owned in original order.  stepdown keeps
             * recovery_eligible=true, so the end state derives
             * RECOVERY_OBSERVE exactly as the shadow committed. */
            if (cluster_transition(cluster, UCN_CLUSTER_PHASE_RECOVERY_HEAD,
                                   UCN_CLUSTER_PHASE_RECOVERY_OBSERVE,
                                   UCN_CLUSTER_REASON_RECOVERY_TTL_EXPIRED,
                                   now_ms) != UCN_OK) {
                /* Fail closed: do NOT count the TTL expiry or run the
                 * stepdown on a rejected transition (shadow mismatch /
                 * illegal pair / pre-mutated phase fields); the node stays
                 * RECOVERY_HEAD and the next Step re-visits the deadline. */
                return UCN_ERR_STATE;
            }
            stepdown_recovery_head(cluster, now_ms);
#if !defined(NDEBUG)
            /* CLV2-01-04f post-commit derive assert: after the transition
             * AND every site effect the node must derive RECOVERY_OBSERVE
             * (role == DETACHED, eligible, no backoff, cooldown armed). */
            assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                   UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
#endif
            return UCN_OK;
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_STEPPING_DOWN &&
        ucn_deadline_expired(now_ms, cluster->stepdown_deadline_ms)) {
        /* CLV2-01-04f.2: the expired stepdown deadline IS the
         * STEPPING_DOWN -> JOIN_PENDING transition - call it FIRST,
         * UNCONDITIONALLY, fail closed.  apply_legacy(JOIN_PENDING)
         * writes role=JOIN_PENDING + recovery_eligible=false +
         * backoff=0; the site's primary_member_table_clear() + timer writes then run
         * in original order (the role write below is idempotent).  On a
         * rejected transition NOTHING runs - the members stay intact,
         * the role stays STEPPING_DOWN, and the next step re-visits the
         * deadline. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_STEPPING_DOWN,
                               UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_REASON_STEPDOWN_COMPLETE,
                               now_ms) != UCN_OK) {
            /* Fail closed: do NOT clear the members or touch the timers
             * on a rejected transition (shadow mismatch / illegal pair /
             * pre-mutated phase fields). */
            return UCN_ERR_STATE;
        }
        /* Ordered switchback completes: leave members, join the better
         * Head that was already announced via HEAD_STEPDOWN. */
        primary_member_table_clear(cluster);
        cluster->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
        cluster->role_since_ms = now_ms;
        cluster->next_join_retry_ms = now_ms;
        cluster->stepdown_deadline_ms = 0U;
#if !defined(NDEBUG)
        /* CLV2-01-04f.2 post-commit derive assert: after the transition
         * AND every site effect the node must derive JOIN_PENDING. */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_JOIN_PENDING);
#endif
    }
    if (cluster->role == UCN_CLUSTER_ROLE_CANDIDATE &&
        (cluster->next_advertise_ms == 0U ||
         ucn_deadline_expired(now_ms, cluster->next_advertise_ms))) {
        send_next_advertisement(cluster, now_ms);
    }
    if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING &&
        (cluster->next_join_retry_ms == 0U ||
         ucn_deadline_expired(now_ms, cluster->next_join_retry_ms))) {
        send_join_request(cluster, now_ms);
    }
    if ((cluster->role == UCN_CLUSTER_ROLE_MEMBER ||
         cluster->role == UCN_CLUSTER_ROLE_BACKUP) &&
        (cluster->next_keepalive_ms == 0U ||
         ucn_deadline_expired(now_ms, cluster->next_keepalive_ms))) {
        send_keepalive(cluster, now_ms);
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_step(ucn_cluster_t *cluster)
{
    ucn_result_t result = ucn_cluster_step_inner(cluster);

    /* CLV2-01-02: keep the shadow phase mirror aligned after every
     * Step.  The mirror never drives behaviour; it only exists for the
     * consistency gate. */
    if (result == UCN_OK && cluster != NULL && cluster->config.enabled) {
        cluster_shadow_sync(cluster, UCN_CLUSTER_REASON_UNKNOWN);
    }
    return result;
}

ucn_cluster_role_t ucn_cluster_get_role(const ucn_cluster_t *cluster)
{
    return cluster == NULL ? UCN_CLUSTER_ROLE_DISABLED : cluster->role;
}

/* CLV2-M03 (03-02): the active epoch is the logical unification of the
 * node's current cluster_id / term / head_node_id.  PHYSICAL STORAGE IS
 * UNCHANGED - the struct keeps the three scalar fields (struct size /
 * cluster_bytes frozen), this accessor is the single read point.  03-03+
 * will drive Head-Offer / Merge / Higher-Authority decisions through
 * ucn_cluster_epoch_compare() on this value.
 *
 * CONTRACT (03-02 MINOR cleanup, human audit): {0,0,0} for NULL; for a
 * non-NULL object this is a RAW READ PROJECTION of the three scalars -
 * no role inspection, no liveness/validity check (comparator !=
 * validator, 03-01).  A DETACHED node exposes {0,0,0} only because
 * set_detached() clears the three fields (a state invariant), NOT
 * because this getter enforces it. */

ucn_cluster_epoch_t ucn_cluster_active_epoch_get(const ucn_cluster_t *cluster)
{
    ucn_cluster_epoch_t epoch;

    (void)memset(&epoch, 0, sizeof(epoch));
    if (cluster == NULL) {
        return epoch;
    }
    epoch.cluster_id = cluster->cluster_id;
    epoch.term = cluster->term;
    epoch.head_node_id = cluster->head_node_id;
    return epoch;
}

size_t ucn_cluster_member_count(const ucn_cluster_t *cluster)
{
    return cluster == NULL ? 0U : primary_member_count_u16(cluster);
}

static void member_summary_fill(const ucn_cluster_t *cluster,
                                const ucn_cluster_member_t *member,
                                ucn_cluster_member_summary_t *summary)
{
    summary->node_id = member->node_id;
    summary->lease_expires_at_ms = member->lease_expires_at_ms;
    summary->status = member->status;
    summary->voting = member->voting;
    summary->config_id = ucn_cluster_voter_set_is_valid(
                             &cluster->active_voter_set) ?
                             cluster->active_voter_set.config_id : 0U;
}

ucn_result_t ucn_cluster_get_view(const ucn_cluster_t *cluster,
                                  ucn_cluster_view_t *view)
{
    if (cluster == NULL || view == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    view->enabled = cluster->config.enabled;
    view->persistence_mode = cluster->config.persistence_mode;
    view->persistence_restore_state =
        cluster->stats.persistence_restore_state;
    view->persistence_pending = cluster->persistence_pending;
    view->persistence_faulted = cluster->persistence_faulted;
    view->persistence_retry_pending = cluster->persistence_retry_pending;
    view->persistence_failure = cluster->persistence_failure;
    view->role = cluster->role;
    view->local_node_id = cluster->config.local_node_id;
    view->cluster_id = cluster->cluster_id;
    view->term = cluster->term;
    view->head_node_id = cluster->head_node_id;
    view->last_cluster_id = cluster->last_cluster_id;
    view->max_seen_term = cluster->max_seen_term;
    view->last_stable_head = cluster->last_stable_head;
    view->cluster_id_round = cluster->cluster_id_round;
    view->current_head_score = cluster->current_head_score;
    view->authority_active = ucn_cluster_authority_active(cluster);
    view->authority_phase = cluster->authority_phase;
    view->authority_fence_reason = cluster->authority_fence_reason;
    return UCN_OK;
}

size_t ucn_cluster_copy_member_summaries(
    const ucn_cluster_t *cluster,
    ucn_cluster_member_summary_t *output,
    size_t capacity)
{
    size_t index;
    size_t count = 0U;

    if (cluster == NULL || (output == NULL && capacity != 0U)) {
        return 0U;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        const ucn_cluster_member_t *member = &cluster->primary_members.slots[index];

        if (!member->occupied) {
            continue;
        }
        if (output == NULL) {
            ++count;
            continue;
        }
        if (count >= capacity) {
            break;
        }
        member_summary_fill(cluster, member, &output[count]);
        ++count;
    }
    return count;
}

ucn_result_t ucn_cluster_get_member_summary_at(
    const ucn_cluster_t *cluster,
    size_t table_index,
    ucn_cluster_member_summary_t *summary)
{
    const ucn_cluster_member_t *member;

    if (cluster == NULL || summary == NULL ||
        table_index >= UCN_CLUSTER_MAX_MEMBERS) {
        return UCN_ERR_ARGUMENT;
    }
    member = &cluster->primary_members.slots[table_index];
    if (!member->occupied) {
        return UCN_ERR_NOT_FOUND;
    }
    member_summary_fill(cluster, member, summary);
    return UCN_OK;
}

ucn_result_t ucn_cluster_get_member_capacity_view(
    const ucn_cluster_t *cluster,
    ucn_cluster_member_capacity_view_t *view)
{
    uint16_t runtime_used;
    uint16_t voter_used = 0U;

    if (cluster == NULL || view == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    runtime_used = primary_member_count_u16(cluster);
    if (ucn_cluster_voter_set_is_valid(&cluster->active_voter_set)) {
        voter_used = cluster->active_voter_set.count;
    }
    view->runtime_capacity = cluster->config.member_capacity;
    view->runtime_used = runtime_used;
    view->runtime_available = runtime_used >= view->runtime_capacity ? 0U :
                              (uint16_t)(view->runtime_capacity - runtime_used);
    view->voter_capacity = cluster->config.voter_capacity;
    view->voter_used = voter_used;
    view->voter_available = voter_used >= view->voter_capacity ? 0U :
                            (uint16_t)(view->voter_capacity - voter_used);
    return UCN_OK;
}

const ucn_cluster_stats_t *ucn_cluster_get_stats(const ucn_cluster_t *cluster)
{
    return cluster == NULL ? NULL : &cluster->stats;
}

#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS)
/* CLV2-01-04d.4: test-only views of the static d-group sites
 * remove_member() / expire_members(), so tests can drive the
 * backup-eviction preflight pattern directly (no network tick, no
 * observed-pair recording) and prove the zero-side-effect invariant. */
void ucn_cluster_test_remove_member(ucn_cluster_t *cluster,
                                    ucn_node_id_t node_id,
                                    uint32_t now_ms)
{
    remove_member(cluster, node_id, now_ms);
}

void ucn_cluster_test_expire_members(ucn_cluster_t *cluster,
                                     uint32_t now_ms)
{
    expire_members(cluster, now_ms);
}

/* CLV2-01-04d.7: test-only views of the head-ladder sites wired in this
 * point (start_backup_assignment_cycle / send_backup_assignment_step /
 * assign_backup), so tests can drive the SYNCING->ASSIGNING arming, the
 * sweep-done last-frame preflight and the NO_BACKUP->ASSIGNING selection
 * directly. */
void ucn_cluster_test_start_backup_assignment_cycle(ucn_cluster_t *cluster,
                                                    uint32_t now_ms)
{
    start_backup_assignment_cycle(cluster, now_ms);
}

void ucn_cluster_test_send_backup_assignment_step(ucn_cluster_t *cluster,
                                                  uint32_t now_ms)
{
    send_backup_assignment_step(cluster, now_ms);
}

void ucn_cluster_test_assign_backup(ucn_cluster_t *cluster, uint32_t now_ms)
{
    assign_backup(cluster, now_ms);
}

void ucn_cluster_test_queue_backup_assignment_for_member(
    ucn_cluster_t *cluster, ucn_node_id_t member_node_id, uint32_t now_ms)
{
    queue_backup_assignment_for_member(cluster, member_node_id, now_ms);
}

/* CLV2-01-04e: test-only views of the takeover-lifecycle sites wired in
 * this point (start_takeover / complete_takeover), so tests can drive the
 * BACKUP_READY -> BACKUP_TAKEOVER and BACKUP_TAKEOVER -> HEAD_NO_BACKUP
 * transitions directly and verify the full site-side field effects (and
 * the fail-closed rejection with zero writes). */
ucn_result_t ucn_cluster_test_start_takeover(ucn_cluster_t *cluster,
                                             uint32_t now_ms)
{
    uint32_t before;

    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    before = cluster->shadow_transition_count;
    start_takeover(cluster, now_ms);
    /* start_takeover() is void: report the transition outcome by whether
     * the entry point committed (shadow_transition_count++ on success; a
     * rejected transition performs ZERO writes and leaves the count
     * untouched). */
    return cluster->shadow_transition_count == before ? UCN_ERR_STATE
                                                      : UCN_OK;
}

ucn_result_t ucn_cluster_test_complete_takeover(ucn_cluster_t *cluster,
                                                uint32_t now_ms)
{
    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    return complete_takeover(cluster, now_ms);
}

/* CLV2-01-04e.7: test-only view of the static score-challenge site
 * backup_challenge(), so tests can drive it directly and verify the full
 * site-side field effects (and the fail-closed rejection with zero
 * writes) without an end-of-RX shadow sync re-aligning the mirror.
 * backup_challenge() reports the outcome itself (UCN_OK on commit,
 * UCN_ERR_STATE on a rejected transition). */
ucn_result_t ucn_cluster_test_backup_challenge(ucn_cluster_t *cluster,
                                               uint32_t now_ms)
{
    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    return backup_challenge(cluster, now_ms);
}

/* CLV2-01-04f: test-only views of the static RECOVERY-domain offer sites
 * consider_head_offer() / begin_ordered_stepdown(), so tests can drive the
 * RECOVERY_* -> JOIN_PENDING (SITE A) and RECOVERY_HEAD -> STEPPING_DOWN
 * (SITE B) transitions directly and verify the full site-side field effects
 * (and the fail-closed rejection with zero writes) without an end-of-RX
 * shadow sync re-aligning the mirror.  Both sites are void: report the
 * transition outcome by whether the entry point committed
 * (shadow_transition_count++ on success; a rejected transition performs
 * ZERO writes and leaves the count untouched). */
ucn_result_t ucn_cluster_test_consider_head_offer(
    ucn_cluster_t *cluster, ucn_cluster_candidate_t *candidate, uint32_t now_ms)
{
    uint32_t before;

    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    before = cluster->shadow_transition_count;
    consider_head_offer(cluster, candidate, now_ms);
    return cluster->shadow_transition_count == before ? UCN_ERR_STATE : UCN_OK;
}

ucn_result_t ucn_cluster_test_begin_ordered_stepdown(
    ucn_cluster_t *cluster, const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    uint32_t before;

    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    before = cluster->shadow_transition_count;
    begin_ordered_stepdown(cluster, candidate, now_ms);
    return cluster->shadow_transition_count == before ? UCN_ERR_STATE : UCN_OK;
}
#endif
