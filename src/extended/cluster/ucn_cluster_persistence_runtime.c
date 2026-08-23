/* CLV2-M04 (04-05): bounded runtime bridge for the persistence Provider.
 *
 * The Provider owns the complete 280 B logical Record.  Cluster keeps only
 * one small pending-operation descriptor, then reloads the authoritative
 * snapshot after COMMITTED before it changes any outward-facing FSM state.
 * This is deliberately stricter than trusting submit() alone: a buggy
 * provider which reports COMMITTED without making the matching journal entry
 * visible is treated as a fail-closed persistence fault.
 */

#include "ucn/ucn_cluster_persist.h"

#include <string.h>

#include "ucn_cluster_internal.h"

static bool persistence_is_required(const ucn_cluster_t *cluster)
{
    return cluster != NULL &&
           cluster->config.persistence_mode == UCN_CLUSTER_PERSISTENCE_REQUIRED;
}

static void clear_pending(ucn_cluster_t *cluster)
{
    cluster->persistence_pending = false;
    cluster->persistence_pending_action = (uint8_t)CLUSTER_PERSIST_ACTION_NONE;
    cluster->persistence_pending_operation = 0U;
    cluster->persistence_pending_destination = 0U;
    cluster->persistence_pending_token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    cluster->persistence_pending_operation_id = 0U;
    cluster->persistence_pending_fingerprint = 0U;
}

static void clear_retry(ucn_cluster_t *cluster)
{
    cluster->persistence_retry_pending = false;
    cluster->persistence_retry_dispatch = false;
    cluster->persistence_retry_action = (uint8_t)CLUSTER_PERSIST_ACTION_NONE;
    cluster->persistence_retry_destination = 0U;
}

/* Provider callbacks are external code.  Set this gate before entering every
 * load/submit/poll dynamic extent, not after a callback reports PENDING:
 * otherwise a synchronous callback can re-enter step() and advertise an old
 * Epoch before the normal pending gate has been installed. */
static bool persistence_io_enter(ucn_cluster_t *cluster)
{
    if (cluster == NULL || cluster->persistence_io_active) {
        return false;
    }
    cluster->persistence_io_active = true;
    return true;
}

static void persistence_io_leave(ucn_cluster_t *cluster)
{
    if (cluster != NULL) {
        cluster->persistence_io_active = false;
    }
}

/* A head which loses persistence must stop being an authority immediately.
 * TERM_CONFLICT_WAIT is the complementary wire-silent control-plane
 * containment state: it preserves diagnostic epoch fields but has no outbound
 * Cluster role.  M08 Authority Fence does not replace this state.  Member and
 * Backup paths need no role rewrite because the universal progress/TX gates
 * below already prevent vote/takeover completion. */
static void revoke_head_authority_on_persistence_fault(ucn_cluster_t *cluster)
{
    ucn_cluster_phase_t phase;

    if (cluster == NULL ||
        (cluster->role != UCN_CLUSTER_ROLE_HEAD &&
         cluster->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD)) {
        return;
    }
    phase = cluster_phase_from_legacy_state(cluster, cluster_now(cluster));
    (void)cluster_transition(cluster, phase,
                             UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT,
                             UCN_CLUSTER_REASON_TERM_CONFLICT,
                             cluster_now(cluster));
}

bool cluster_persistence_outbound_allowed(const ucn_cluster_t *cluster)
{
    return cluster != NULL &&
           (!persistence_is_required(cluster) ||
            (!cluster->persistence_pending && !cluster->persistence_faulted &&
             !cluster->persistence_io_active &&
             (!cluster->persistence_retry_pending ||
              cluster->persistence_retry_dispatch)));
}

bool cluster_persistence_progress_blocked(const ucn_cluster_t *cluster)
{
    return persistence_is_required(cluster) &&
           (cluster->persistence_pending || cluster->persistence_faulted ||
            cluster->persistence_io_active || cluster->persistence_retry_pending);
}

void cluster_persistence_fail_closed(ucn_cluster_t *cluster,
                                     ucn_result_t failure)
{
    if (cluster == NULL || !persistence_is_required(cluster)) {
        return;
    }
    if (!cluster->persistence_faulted) {
        cluster->stats.persistence_failures++;
    }
    clear_pending(cluster);
    clear_retry(cluster);
    cluster->persistence_faulted = true;
    cluster->persistence_failure = failure == UCN_OK ? UCN_ERR_STATE : failure;
    revoke_head_authority_on_persistence_fault(cluster);
}

ucn_result_t cluster_persistence_load_snapshot_ex(
    ucn_cluster_t *cluster,
    ucn_cluster_persist_state_t *state,
    bool *factory_empty)
{
    const ucn_cluster_persist_provider_t *provider;
    ucn_cluster_persist_load_result_t result;
    ucn_result_t load_result;

    if (cluster == NULL || state == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (factory_empty != NULL) {
        *factory_empty = false;
    }
    if (!persistence_is_required(cluster)) {
        return UCN_ERR_CONFIG;
    }
    provider = cluster->config.persistence_provider;
    if (!ucn_cluster_persist_provider_is_compatible(provider)) {
        return UCN_ERR_CONFIG;
    }
    (void)memset(&result, 0, sizeof(result));
    if (!persistence_io_enter(cluster)) {
        return UCN_ERR_STATE;
    }
    load_result = provider->load(provider->context, &result);
    persistence_io_leave(cluster);
    if (load_result != UCN_OK ||
        !ucn_cluster_persist_load_result_is_valid(&result)) {
        return load_result == UCN_OK ? UCN_ERR_CONFIG : load_result;
    }
    if (result.state == UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY) {
        ucn_cluster_persist_state_init_empty(state);
        if (factory_empty != NULL) {
            *factory_empty = true;
        }
    } else {
        *state = result.snapshot;
    }
    return UCN_OK;
}

ucn_result_t cluster_persistence_load_snapshot(
    ucn_cluster_t *cluster,
    ucn_cluster_persist_state_t *state)
{
    return cluster_persistence_load_snapshot_ex(cluster, state, NULL);
}

static ucn_result_t next_operation_id(
    const ucn_cluster_persist_state_t *current,
    uint32_t *operation_id)
{
    if (current == NULL || operation_id == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (current->last_completed_operation_id == 0U) {
        *operation_id = 1U;
        return UCN_OK;
    }
    return cluster_serial_next_checked(current->last_completed_operation_id,
                                       operation_id);
}

static bool durable_request_matches(
    const ucn_cluster_persist_state_t *state,
    uint32_t operation_id,
    uint8_t operation,
    uint32_t fingerprint)
{
    return state != NULL &&
           state->last_completed_operation_id == operation_id &&
           state->last_completed_operation == operation &&
           state->last_completed_operation_fingerprint == fingerprint;
}

static ucn_result_t verify_committed_request(
    ucn_cluster_t *cluster,
    uint32_t operation_id,
    uint8_t operation,
    uint32_t fingerprint,
    ucn_cluster_persist_state_t *durable_state)
{
    ucn_result_t result = cluster_persistence_load_snapshot(cluster, durable_state);

    if (result != UCN_OK) {
        return result;
    }
    return durable_request_matches(durable_state, operation_id, operation,
                                   fingerprint) ? UCN_OK : UCN_ERR_STATE;
}

ucn_result_t cluster_persistence_begin_state(
    ucn_cluster_t *cluster,
    const ucn_cluster_persist_state_t *current_state,
    ucn_cluster_persist_operation_t operation,
    const ucn_cluster_persist_state_t *next_state,
    cluster_persistence_action_t action,
    ucn_node_id_t destination,
    bool *committed,
    ucn_cluster_persist_state_t *durable_state)
{
    const ucn_cluster_persist_provider_t *provider;
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_completion_t completion;
    ucn_cluster_persist_request_admission_t admission;
    ucn_result_t result;

    if (cluster == NULL || current_state == NULL || next_state == NULL ||
        committed == NULL || durable_state == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *committed = false;
    (void)memset(durable_state, 0, sizeof(*durable_state));
    if (!persistence_is_required(cluster)) {
        *committed = true;
        *durable_state = *next_state;
        return UCN_OK;
    }
    if (cluster->persistence_pending || cluster->persistence_faulted ||
        cluster->persistence_io_active || cluster->persistence_retry_pending) {
        return UCN_ERR_STATE;
    }
    provider = cluster->config.persistence_provider;
    if (!ucn_cluster_persist_provider_is_compatible(provider)) {
        cluster_persistence_fail_closed(cluster, UCN_ERR_CONFIG);
        return UCN_ERR_CONFIG;
    }
    (void)memset(&request, 0, sizeof(request));
    result = next_operation_id(current_state, &request.operation_id);
    if (result != UCN_OK) {
        cluster_persistence_fail_closed(cluster, result);
        return result;
    }
    request.operation = operation;
    request.next_state = *next_state;
    /* Record v1 is read-only migration input. A normal runtime write always
     * emits schema v2, including the controlled legacy-abort transition. */
    request.next_state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2;
    result = ucn_cluster_persist_request_finalize(&request);
    if (result != UCN_OK) {
        cluster_persistence_fail_closed(cluster, result);
        return result;
    }
    admission = ucn_cluster_persist_request_admit(current_state, &request);
    if (admission != UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW &&
        admission != UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_IDEMPOTENT) {
        cluster_persistence_fail_closed(cluster, UCN_ERR_STATE);
        return UCN_ERR_STATE;
    }
    if (!persistence_io_enter(cluster)) {
        return UCN_ERR_STATE;
    }
    completion = provider->submit(provider->context, &request);
    persistence_io_leave(cluster);
    cluster->stats.persistence_submitted++;
    if (!ucn_cluster_persist_provider_accepts_completion(provider, &completion)) {
        cluster_persistence_fail_closed(cluster, UCN_ERR_CONFIG);
        return UCN_ERR_CONFIG;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_COMMITTED) {
        result = verify_committed_request(
            cluster, request.operation_id, (uint8_t)request.operation,
            request.next_state.last_completed_operation_fingerprint,
            durable_state);
        if (result != UCN_OK) {
            cluster_persistence_fail_closed(cluster, result);
            return result;
        }
        cluster->stats.persistence_committed++;
        *committed = true;
        return UCN_OK;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_PENDING) {
        cluster->persistence_pending = true;
        cluster->persistence_pending_action = (uint8_t)action;
        cluster->persistence_pending_operation = (uint8_t)request.operation;
        cluster->persistence_pending_destination = destination;
        cluster->persistence_pending_token = completion.token;
        cluster->persistence_pending_operation_id = request.operation_id;
        cluster->persistence_pending_fingerprint =
            request.next_state.last_completed_operation_fingerprint;
        cluster->stats.persistence_pending++;
        return UCN_OK;
    }
    cluster_persistence_fail_closed(cluster, completion.failure);
    return completion.failure;
}

ucn_result_t cluster_persistence_begin_epoch(
    ucn_cluster_t *cluster,
    const ucn_cluster_epoch_t *epoch,
    cluster_persistence_action_t action,
    ucn_node_id_t destination,
    bool *committed,
    ucn_cluster_persist_state_t *durable_state)
{
    ucn_cluster_persist_state_t current;
    ucn_cluster_persist_state_t next;
    ucn_cluster_persist_operation_t operation;
    ucn_result_t result;

    if (cluster == NULL || epoch == NULL || committed == NULL ||
        durable_state == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!persistence_is_required(cluster)) {
        ucn_cluster_persist_state_init_empty(&next);
        next.has_active_epoch = true;
        next.active_epoch = *epoch;
        next.has_max_epoch = true;
        next.max_epoch = *epoch;
        *committed = true;
        *durable_state = next;
        return UCN_OK;
    }
    result = cluster_persistence_load_snapshot(cluster, &current);
    if (result != UCN_OK) {
        cluster_persistence_fail_closed(cluster, result);
        return result;
    }
    next = current;
    next.has_active_epoch = true;
    next.active_epoch = *epoch;
    next.has_max_epoch = true;
    next.max_epoch = *epoch;
    if (current.has_active_epoch &&
        current.active_epoch.cluster_id == epoch->cluster_id) {
        operation = UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT;
    } else {
        /* Match CLUSTER_CREATE_COMMIT's exact clear policy without touching
         * Rekey/Tombstone lineage evidence; the transition validator rejects
         * an unsafe lineage rather than allowing this runtime bridge to erase
         * it. */
        (void)memset(&next.last_vote, 0, sizeof(next.last_vote));
        (void)memset(&next.committed_config, 0,
                     sizeof(next.committed_config));
        (void)memset(&next.config_transaction, 0,
                     sizeof(next.config_transaction));
        next.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
        operation = UCN_CLUSTER_PERSIST_OPERATION_CLUSTER_CREATE_COMMIT;
    }
    return cluster_persistence_begin_state(cluster, &current, operation, &next,
                                           action, destination, committed,
                                           durable_state);
}

ucn_result_t cluster_persistence_begin_vote(
    ucn_cluster_t *cluster,
    const ucn_cluster_persist_vote_t *vote,
    cluster_persistence_action_t action,
    ucn_node_id_t destination,
    bool *committed,
    ucn_cluster_persist_state_t *durable_state)
{
    ucn_cluster_persist_state_t current;
    ucn_cluster_persist_state_t next;
    ucn_result_t result;

    if (cluster == NULL || vote == NULL || committed == NULL ||
        durable_state == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!persistence_is_required(cluster)) {
        ucn_cluster_persist_state_init_empty(&next);
        next.last_vote = *vote;
        *committed = true;
        *durable_state = next;
        return UCN_OK;
    }
    result = cluster_persistence_load_snapshot(cluster, &current);
    if (result != UCN_OK) {
        cluster_persistence_fail_closed(cluster, result);
        return result;
    }
    next = current;
    next.last_vote = *vote;
    return cluster_persistence_begin_state(
        cluster, &current, UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT, &next,
        action, destination, committed, durable_state);
}

ucn_result_t cluster_persistence_poll(
    ucn_cluster_t *cluster,
    bool *resolved,
    cluster_persistence_resolution_t *resolution)
{
    const ucn_cluster_persist_provider_t *provider;
    ucn_cluster_persist_completion_t completion;
    ucn_result_t result;

    if (cluster == NULL || resolved == NULL || resolution == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *resolved = false;
    (void)memset(resolution, 0, sizeof(*resolution));
    if (persistence_is_required(cluster) && cluster->persistence_io_active) {
        return UCN_ERR_STATE;
    }
    if (!persistence_is_required(cluster) || !cluster->persistence_pending) {
        return UCN_OK;
    }
    provider = cluster->config.persistence_provider;
    if (!ucn_cluster_persist_provider_supports_async(provider)) {
        cluster_persistence_fail_closed(cluster, UCN_ERR_CONFIG);
        return UCN_ERR_CONFIG;
    }
    if (!persistence_io_enter(cluster)) {
        return UCN_ERR_STATE;
    }
    completion = provider->poll(provider->context,
                                cluster->persistence_pending_token);
    persistence_io_leave(cluster);
    if (!ucn_cluster_persist_provider_accepts_completion(provider, &completion)) {
        cluster_persistence_fail_closed(cluster, UCN_ERR_CONFIG);
        return UCN_ERR_CONFIG;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_PENDING) {
        return UCN_OK;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_FAILED) {
        cluster_persistence_fail_closed(cluster, completion.failure);
        return completion.failure;
    }
    result = verify_committed_request(
        cluster, cluster->persistence_pending_operation_id,
        cluster->persistence_pending_operation,
        cluster->persistence_pending_fingerprint, &resolution->durable_state);
    if (result != UCN_OK) {
        cluster_persistence_fail_closed(cluster, result);
        return result;
    }
    resolution->action =
        (cluster_persistence_action_t)cluster->persistence_pending_action;
    resolution->destination = cluster->persistence_pending_destination;
    clear_pending(cluster);
    cluster->stats.persistence_committed++;
    *resolved = true;
    return UCN_OK;
}

void cluster_persistence_schedule_retry(ucn_cluster_t *cluster,
                                        cluster_persistence_action_t action,
                                        ucn_node_id_t destination)
{
    if (cluster == NULL || !persistence_is_required(cluster)) {
        return;
    }
    cluster->persistence_retry_pending = true;
    cluster->persistence_retry_dispatch = false;
    cluster->persistence_retry_action = (uint8_t)action;
    cluster->persistence_retry_destination = destination;
}

bool cluster_persistence_takeover_ack_send_is_retryable(ucn_result_t result)
{
    /* The Cluster control send callback reports local queue pressure and a
     * temporarily unavailable direct bearer with these two results.  Neither
     * says anything about the already reload-proved Vote journal. */
    return result == UCN_ERR_NO_SPACE || result == UCN_ERR_LINK_DOWN;
}

/* A durable Vote remains valid even if its first wire ACK is delayed by the
 * local control token bucket or adapter queue.  Reload it before every retry:
 * retry metadata is deliberately small and never becomes a second Record
 * cache.  Only the retry dispatcher may pass the outbound gate while this
 * state is set, so unrelated old control traffic cannot escape meanwhile. */
ucn_result_t cluster_persistence_retry_pending(ucn_cluster_t *cluster)
{
    ucn_cluster_persist_state_t durable_state;
    ucn_result_t result;

    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!persistence_is_required(cluster) || !cluster->persistence_retry_pending) {
        return UCN_OK;
    }
    if (cluster->persistence_io_active || cluster->persistence_pending ||
        cluster->persistence_faulted) {
        return UCN_ERR_STATE;
    }
    if ((cluster_persistence_action_t)cluster->persistence_retry_action !=
            CLUSTER_PERSIST_ACTION_TAKEOVER_ACK ||
        cluster->persistence_retry_destination == 0U ||
        cluster->persistence_retry_destination == UCN_NODE_BROADCAST) {
        cluster_persistence_fail_closed(cluster, UCN_ERR_STATE);
        return UCN_ERR_STATE;
    }
    result = cluster_persistence_load_snapshot(cluster, &durable_state);
    if (result != UCN_OK) {
        cluster_persistence_fail_closed(cluster, result);
        return result;
    }
    cluster->persistence_retry_dispatch = true;
    result = send_takeover_ack_after_persistence(
        cluster, cluster->persistence_retry_destination, &durable_state);
    cluster->persistence_retry_dispatch = false;
    cluster->stats.persistence_retry_attempts++;
    if (result == UCN_OK) {
        clear_retry(cluster);
        return UCN_OK;
    }
    if (cluster_persistence_takeover_ack_send_is_retryable(result)) {
        return result;
    }
    /* A retry which can no longer prove the Vote/epoch is not a mere
     * transport back-pressure condition. */
    cluster_persistence_fail_closed(cluster, result);
    return result;
}

static bool persistence_transaction_id_is_valid(uint32_t transaction_id)
{
    return transaction_id != 0U &&
           transaction_id <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

ucn_result_t ucn_cluster_persist_config_prepare(
    ucn_cluster_t *cluster,
    uint32_t transaction_id,
    const ucn_cluster_persist_config_ref_t *staging_config,
    bool *committed)
{
    if (cluster == NULL || staging_config == NULL || committed == NULL ||
        !persistence_transaction_id_is_valid(transaction_id)) {
        return UCN_ERR_ARGUMENT;
    }
    /* M07 owns not only Config Commit but also the durable PREPARE recovery
     * contract (resume/abort/commit after reset).  Leaving PREPARED Record-v1
     * state behind while M07 is absent would make the next boot incarnation
     * transaction impossible.  Reject before Provider I/O instead of writing
     * an orphaned staging record. */
    *committed = false;
    return UCN_ERR_CONFIG;
}

ucn_result_t ucn_cluster_persist_config_commit(
    ucn_cluster_t *cluster,
    uint32_t transaction_id,
    bool *committed)
{
    if (cluster == NULL || committed == NULL ||
        !persistence_transaction_id_is_valid(transaction_id)) {
        return UCN_ERR_ARGUMENT;
    }
    /* M07 owns application of a committed Config to the runtime/wire FSM.
     * Until that continuation exists, a generic ACTION_NONE completion would
     * make a new Config durable then resume the old runtime contract.  Reject
     * before Provider I/O just as Rekey Commit does for M13. */
    *committed = false;
    return UCN_ERR_CONFIG;
}

ucn_result_t ucn_cluster_persist_rekey_prepare(
    ucn_cluster_t *cluster,
    uint32_t transaction_id,
    const ucn_cluster_persist_rekey_ref_t *staging_rekey,
    bool *committed)
{
    if (cluster == NULL || staging_rekey == NULL || committed == NULL ||
        !persistence_transaction_id_is_valid(transaction_id)) {
        return UCN_ERR_ARGUMENT;
    }
    /* M13 owns successor-Epoch PREPARE recovery and commit/abort semantics.
     * Until then this Hook must not create a durable state which no public
     * continuation can resolve after reset. */
    *committed = false;
    return UCN_ERR_CONFIG;
}

ucn_result_t ucn_cluster_persist_rekey_commit(
    ucn_cluster_t *cluster,
    uint32_t transaction_id,
    bool *committed)
{
    if (cluster == NULL || committed == NULL ||
        !persistence_transaction_id_is_valid(transaction_id)) {
        return UCN_ERR_ARGUMENT;
    }
    /* M13 owns the successor-epoch runtime continuation.  Until it exists,
     * allowing this public Hook to make A->B durable would leave the Current
     * FSM running A and could advertise a retired Epoch after the generic
     * ACTION_NONE resolution.  Reject before any Provider I/O instead of
     * exposing a half-integrated Rekey Commit.  Record-v1's lower-level
     * transition remains tested separately; this is the runtime owner fence. */
    *committed = false;
    return UCN_ERR_CONFIG;
}
