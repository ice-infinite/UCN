/* CLV2-13-06: default-OFF persist-before-promise owner for Rekey.
 * No function in this file is referenced by the production Cluster FSM. */

#include "ucn/ucn_cluster_rekey.h"

#include <string.h>

static ucn_cluster_rekey_persist_owner_t *provider_callback_owner;

static bool callback_enter(ucn_cluster_rekey_persist_owner_t *owner)
{
    if (owner == NULL || provider_callback_owner != NULL) {
        return false;
    }
    provider_callback_owner = owner;
    return true;
}

static void callback_leave(ucn_cluster_rekey_persist_owner_t *owner)
{
    if (provider_callback_owner == owner) {
        provider_callback_owner = NULL;
    }
}

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool epoch_equal(const ucn_cluster_epoch_t *left,
                        const ucn_cluster_epoch_t *right)
{
    return left != NULL && right != NULL &&
           left->cluster_id == right->cluster_id &&
           left->term == right->term &&
           left->head_node_id == right->head_node_id;
}

static bool config_ref_equal(const ucn_cluster_persist_config_ref_t *left,
                             const ucn_cluster_persist_config_ref_t *right)
{
    return left != NULL && right != NULL && left->valid == right->valid &&
           (!left->valid ||
            (left->config_id == right->config_id &&
             left->generation == right->generation &&
             memcmp(left->digest, right->digest,
                    sizeof(left->digest)) == 0));
}

static bool rekey_ref_equal(const ucn_cluster_persist_rekey_ref_t *left,
                            const ucn_cluster_persist_rekey_ref_t *right)
{
    return left != NULL && right != NULL && left->valid == right->valid &&
           (!left->valid ||
           (left->generation == right->generation &&
             left->next_incarnation == right->next_incarnation &&
             left->prepare_nonce == right->prepare_nonce &&
             left->allocation_history_fingerprint ==
                 right->allocation_history_fingerprint &&
             left->successor_backup_node_id ==
                 right->successor_backup_node_id &&
             epoch_equal(&left->predecessor_epoch,
                         &right->predecessor_epoch) &&
             config_ref_equal(&left->predecessor_config,
                              &right->predecessor_config) &&
             epoch_equal(&left->successor_epoch, &right->successor_epoch) &&
             config_ref_equal(&left->successor_config,
                              &right->successor_config)));
}

static bool owner_is_valid(const ucn_cluster_rekey_persist_owner_t *owner)
{
    return owner != NULL && !owner->faulted &&
           ucn_cluster_persist_provider_is_compatible(owner->provider) &&
           owner->authority != NULL && owner->authority->initialized &&
           owner->authority->cluster != NULL &&
           owner->authority->cluster->authority_runtime == owner->authority &&
           ucn_cluster_persist_state_is_valid(&owner->durable_state) &&
           owner->durable_state.record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
}

static bool owner_is_busy(const ucn_cluster_rekey_persist_owner_t *owner)
{
    return owner != NULL && (owner->io_active || owner->pending);
}

static void transaction_fence(ucn_cluster_rekey_persist_owner_t *owner,
                              ucn_cluster_rekey_transaction_t *transaction,
                              ucn_result_t failure,
                              uint32_t now_ms)
{
    ucn_cluster_t *cluster;

    if (owner == NULL) {
        return;
    }
    owner->faulted = true;
    owner->pending = false;
    owner->pending_token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    owner->pending_action = (uint8_t)UCN_CLUSTER_REKEY_PERSIST_ACTION_NONE;
    (void)memset(&owner->pending_request, 0,
                 sizeof(owner->pending_request));
    if (transaction != NULL &&
        ucn_cluster_rekey_transaction_is_valid(transaction)) {
        transaction->state = UCN_CLUSTER_REKEY_STATE_FENCED;
        transaction->authority_revoked = true;
    }
    if (owner->authority == NULL || owner->authority->cluster == NULL) {
        return;
    }
    cluster = owner->authority->cluster;
    cluster->persistence_faulted = true;
    cluster->persistence_failure = failure == UCN_OK ? UCN_ERR_STATE : failure;
    (void)ucn_cluster_authority_runtime_step(owner->authority, now_ms);
}

static void transaction_revoke_for_commit(
    ucn_cluster_rekey_persist_owner_t *owner,
    ucn_cluster_rekey_transaction_t *transaction)
{
    ucn_cluster_t *cluster = owner->authority->cluster;

    transaction->authority_revoked = true;
    cluster->authority_active = false;
    cluster->authority_phase = UCN_CLUSTER_PHASE_HEAD_FENCED;
    cluster->authority_fence_reason =
        UCN_CLUSTER_AUTHORITY_FENCE_REKEY_COMMIT;
    owner->authority->fence_latched = true;
}

static ucn_result_t owner_load(
    ucn_cluster_rekey_persist_owner_t *owner,
    ucn_cluster_persist_load_result_t *loaded)
{
    ucn_result_t result;

    if (owner == NULL || loaded == NULL || owner->io_active ||
        !ucn_cluster_persist_provider_is_compatible(owner->provider) ||
        !callback_enter(owner)) {
        return UCN_ERR_STATE;
    }
    owner->io_active = true;
    result = owner->provider->load(owner->provider->context, loaded);
    owner->io_active = false;
    callback_leave(owner);
    return result;
}

static ucn_result_t owner_reload_exact(
    ucn_cluster_rekey_persist_owner_t *owner,
    const ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_load_result_t loaded;
    ucn_result_t result;

    (void)memset(&loaded, 0, sizeof(loaded));
    result = owner_load(owner, &loaded);
    if (result != UCN_OK ||
        !ucn_cluster_persist_load_result_is_valid(&loaded) ||
        loaded.state != UCN_CLUSTER_PERSIST_LOAD_READY ||
        memcmp(&loaded.snapshot, &request->next_state,
               sizeof(loaded.snapshot)) != 0) {
        return UCN_ERR_STATE;
    }
    owner->durable_state = loaded.snapshot;
    return UCN_OK;
}

static bool prepared_matches(
    const ucn_cluster_persist_state_t *state,
    const ucn_cluster_rekey_transaction_t *transaction,
    uint32_t operation_id)
{
    return state != NULL && transaction != NULL &&
           state->record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
           state->has_active_epoch && state->has_max_epoch &&
           epoch_equal(&state->active_epoch,
                       &transaction->predecessor_epoch) &&
           epoch_equal(&state->max_epoch,
                       &transaction->predecessor_epoch) &&
           config_ref_equal(&state->committed_config,
                            &transaction->predecessor_config_ref) &&
           state->rekey_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
           state->rekey_transaction.transaction_id ==
               transaction->transaction_id &&
           rekey_ref_equal(&state->rekey_transaction.staging_rekey,
                           &transaction->durable_rekey_ref) &&
           state->last_completed_operation_id == operation_id &&
           state->last_completed_operation ==
               UCN_CLUSTER_PERSIST_OPERATION_REKEY_PREPARE;
}

static bool committed_matches(
    const ucn_cluster_persist_state_t *state,
    const ucn_cluster_rekey_transaction_t *transaction,
    uint32_t operation_id)
{
    return state != NULL && transaction != NULL &&
           state->record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
           state->has_active_epoch && state->has_max_epoch &&
           epoch_equal(&state->active_epoch, &transaction->successor_epoch) &&
           epoch_equal(&state->max_epoch, &transaction->successor_epoch) &&
           config_ref_equal(&state->committed_config,
                            &transaction->successor_config_ref) &&
           rekey_ref_equal(&state->committed_rekey,
                           &transaction->durable_rekey_ref) &&
           state->rekey_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
           state->rekey_transaction.transaction_id ==
               transaction->transaction_id && state->tombstone.valid &&
           epoch_equal(&state->tombstone.retired_epoch,
                       &transaction->predecessor_epoch) &&
           state->tombstone.replacement_cluster_id ==
               transaction->successor_epoch.cluster_id &&
           state->tombstone.rekey_transaction_id ==
               transaction->transaction_id &&
           state->boot_incarnation ==
               transaction->durable_rekey_ref.next_incarnation &&
           state->last_completed_operation_id == operation_id &&
           state->last_completed_operation ==
               UCN_CLUSTER_PERSIST_OPERATION_REKEY_COMMIT;
}

static bool aborted_matches(
    const ucn_cluster_persist_state_t *state,
    const ucn_cluster_rekey_transaction_t *transaction,
    uint32_t operation_id)
{
    return state != NULL && transaction != NULL &&
           state->record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
           state->rekey_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED &&
           state->rekey_transaction.transaction_id ==
               transaction->transaction_id &&
           rekey_ref_equal(&state->rekey_transaction.staging_rekey,
                           &transaction->durable_rekey_ref) &&
           state->last_completed_operation_id == operation_id &&
           state->last_completed_operation ==
               UCN_CLUSTER_PERSIST_OPERATION_REKEY_ABORT;
}

static ucn_result_t build_prepare_request(
    const ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_rekey_transaction_t *transaction,
    ucn_cluster_persist_request_t *request)
{
    if (!serial_is_valid(operation_id) || transaction == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED ||
        !epoch_equal(&owner->durable_state.active_epoch,
                     &transaction->predecessor_epoch) ||
        !config_ref_equal(&owner->durable_state.committed_config,
                          &transaction->predecessor_config_ref) ||
        owner->durable_state.rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        return UCN_ERR_STATE;
    }
    (void)memset(request, 0, sizeof(*request));
    request->operation_id = operation_id;
    request->operation = UCN_CLUSTER_PERSIST_OPERATION_REKEY_PREPARE;
    request->next_state = owner->durable_state;
    request->next_state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    request->next_state.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    request->next_state.rekey_transaction.transaction_id =
        transaction->transaction_id;
    request->next_state.rekey_transaction.staging_rekey =
        transaction->durable_rekey_ref;
    return ucn_cluster_persist_request_finalize(request);
}

static ucn_result_t build_commit_request(
    const ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_rekey_transaction_t *transaction,
    ucn_cluster_persist_request_t *request)
{
    if (!serial_is_valid(operation_id) || transaction == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_REKEY_STATE_QUORUM ||
        !prepared_matches(&owner->durable_state, transaction,
                          owner->durable_state.last_completed_operation_id)) {
        return UCN_ERR_STATE;
    }
    (void)memset(request, 0, sizeof(*request));
    request->operation_id = operation_id;
    request->operation = UCN_CLUSTER_PERSIST_OPERATION_REKEY_COMMIT;
    request->next_state = owner->durable_state;
    request->next_state.has_active_epoch = true;
    request->next_state.active_epoch = transaction->successor_epoch;
    request->next_state.has_max_epoch = true;
    request->next_state.max_epoch = transaction->successor_epoch;
    (void)memset(&request->next_state.last_vote, 0,
                 sizeof(request->next_state.last_vote));
    request->next_state.committed_config =
        transaction->successor_config_ref;
    (void)memset(&request->next_state.config_transaction, 0,
                 sizeof(request->next_state.config_transaction));
    request->next_state.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    request->next_state.committed_rekey = transaction->durable_rekey_ref;
    request->next_state.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    request->next_state.rekey_transaction.transaction_id =
        transaction->transaction_id;
    (void)memset(&request->next_state.rekey_transaction.staging_rekey, 0,
                 sizeof(request->next_state.rekey_transaction.staging_rekey));
    request->next_state.tombstone.valid = true;
    request->next_state.tombstone.retired_epoch =
        transaction->predecessor_epoch;
    request->next_state.tombstone.replacement_cluster_id =
        transaction->successor_epoch.cluster_id;
    request->next_state.tombstone.rekey_transaction_id =
        transaction->transaction_id;
    request->next_state.boot_incarnation =
        transaction->durable_rekey_ref.next_incarnation;
    return ucn_cluster_persist_request_finalize(request);
}

static ucn_result_t build_abort_request(
    const ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_rekey_transaction_t *transaction,
    ucn_cluster_persist_request_t *request)
{
    if (!serial_is_valid(operation_id) || transaction == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_REKEY_STATE_ABORTED ||
        owner->durable_state.rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        owner->durable_state.rekey_transaction.transaction_id !=
            transaction->transaction_id ||
        !rekey_ref_equal(
            &owner->durable_state.rekey_transaction.staging_rekey,
            &transaction->durable_rekey_ref)) {
        return UCN_ERR_STATE;
    }
    (void)memset(request, 0, sizeof(*request));
    request->operation_id = operation_id;
    request->operation = UCN_CLUSTER_PERSIST_OPERATION_REKEY_ABORT;
    request->next_state = owner->durable_state;
    request->next_state.rekey_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED;
    return ucn_cluster_persist_request_finalize(request);
}

static ucn_result_t resolve_committed(
    ucn_cluster_rekey_persist_owner_t *owner,
    ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_request_t *request,
    ucn_cluster_rekey_persist_action_t action,
    uint32_t now_ms)
{
    ucn_result_t result = owner_reload_exact(owner, request);

    if (result != UCN_OK) {
        transaction_fence(owner, transaction, result, now_ms);
        return result;
    }
    if (action == UCN_CLUSTER_REKEY_PERSIST_ACTION_PREPARE) {
        transaction->state = UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED;
        result = ucn_cluster_rekey_transaction_begin_collection(
            transaction, &owner->durable_state);
        if (result != UCN_OK) {
            transaction_fence(owner, transaction, result, now_ms);
        }
        return result;
    }
    if (action == UCN_CLUSTER_REKEY_PERSIST_ACTION_ABORT &&
        aborted_matches(&owner->durable_state, transaction,
                        request->operation_id)) {
        transaction->state = UCN_CLUSTER_REKEY_STATE_ABORTED;
        return UCN_OK;
    }
    if (action == UCN_CLUSTER_REKEY_PERSIST_ACTION_COMMIT &&
        committed_matches(&owner->durable_state, transaction,
                          request->operation_id)) {
        transaction->state = UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE;
        return UCN_OK;
    }
    transaction_fence(owner, transaction, UCN_ERR_STATE, now_ms);
    return UCN_ERR_STATE;
}

static ucn_result_t owner_submit(
    ucn_cluster_rekey_persist_owner_t *owner,
    ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_request_t *request,
    ucn_cluster_rekey_persist_action_t action,
    uint32_t now_ms,
    bool *durable)
{
    ucn_cluster_persist_completion_t completion;

    if (!callback_enter(owner)) {
        return UCN_ERR_STATE;
    }
    *durable = false;
    if (action == UCN_CLUSTER_REKEY_PERSIST_ACTION_PREPARE) {
        transaction->state = UCN_CLUSTER_REKEY_STATE_PREPARE_PENDING;
    } else if (action == UCN_CLUSTER_REKEY_PERSIST_ACTION_COMMIT) {
        transaction_revoke_for_commit(owner, transaction);
        transaction->state = UCN_CLUSTER_REKEY_STATE_COMMIT_PENDING;
    }
    owner->io_active = true;
    completion = owner->provider->submit(owner->provider->context, request);
    owner->io_active = false;
    callback_leave(owner);
    if (!ucn_cluster_persist_provider_accepts_completion(owner->provider,
                                                          &completion)) {
        transaction_fence(owner, transaction, UCN_ERR_STATE, now_ms);
        return UCN_ERR_STATE;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_FAILED) {
        transaction_fence(owner, transaction, completion.failure, now_ms);
        return completion.failure;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_PENDING) {
        owner->pending_request = *request;
        owner->pending_token = completion.token;
        owner->pending_action = (uint8_t)action;
        owner->pending = true;
        return UCN_OK;
    }
    if (resolve_committed(owner, transaction, request, action, now_ms) !=
        UCN_OK) {
        return UCN_ERR_STATE;
    }
    *durable = true;
    return UCN_OK;
}

ucn_result_t ucn_cluster_rekey_persist_owner_init(
    ucn_cluster_rekey_persist_owner_t *owner,
    const ucn_cluster_persist_provider_t *provider,
    ucn_cluster_authority_runtime_t *authority)
{
    uint8_t previous[sizeof(*owner)];
    ucn_cluster_persist_load_result_t loaded;
    ucn_result_t result;

    if (owner == NULL || authority == NULL || !authority->initialized ||
        authority->cluster == NULL || authority->cluster->authority_runtime !=
                                           authority ||
        !ucn_cluster_persist_provider_is_compatible(provider)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!callback_enter(owner)) {
        return UCN_ERR_STATE;
    }
    (void)memcpy(previous, owner, sizeof(previous));
    (void)memset(owner, 0, sizeof(*owner));
    owner->provider = provider;
    owner->authority = authority;
    owner->io_active = true;
    (void)memset(&loaded, 0, sizeof(loaded));
    result = provider->load(provider->context, &loaded);
    owner->io_active = false;
    callback_leave(owner);
    if (result != UCN_OK ||
        !ucn_cluster_persist_load_result_is_valid(&loaded) ||
        loaded.state != UCN_CLUSTER_PERSIST_LOAD_READY ||
        loaded.snapshot.record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION) {
        (void)memcpy(owner, previous, sizeof(previous));
        return UCN_ERR_STATE;
    }
    owner->durable_state = loaded.snapshot;
    return UCN_OK;
}

const ucn_cluster_persist_state_t *ucn_cluster_rekey_persist_owner_state(
    const ucn_cluster_rekey_persist_owner_t *owner)
{
    return owner_is_valid(owner) && !owner_is_busy(owner) ?
               &owner->durable_state : NULL;
}

ucn_result_t ucn_cluster_rekey_persist_begin_prepare(
    ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms,
    bool *durable)
{
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_request_admission_t admission;
    ucn_result_t result;

    if (durable != NULL) {
        *durable = false;
    }
    if (owner == NULL || transaction == NULL || durable == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!owner_is_valid(owner) || owner_is_busy(owner)) {
        return UCN_ERR_STATE;
    }
    result = ucn_cluster_rekey_transaction_step(transaction, now_ms);
    if (result != UCN_OK) {
        return result;
    }
    if (prepared_matches(&owner->durable_state, transaction, operation_id)) {
        result = ucn_cluster_rekey_transaction_begin_collection(
            transaction, &owner->durable_state);
        if (result == UCN_OK) {
            *durable = true;
        }
        return result;
    }
    result = build_prepare_request(owner, operation_id, transaction, &request);
    if (result != UCN_OK) {
        return result;
    }
    admission = ucn_cluster_persist_request_admit(&owner->durable_state,
                                                   &request);
    if (admission != UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW) {
        return UCN_ERR_STATE;
    }
    return owner_submit(owner, transaction, &request,
                        UCN_CLUSTER_REKEY_PERSIST_ACTION_PREPARE,
                        now_ms, durable);
}

ucn_result_t ucn_cluster_rekey_persist_begin_commit(
    ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms,
    bool *durable)
{
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_request_admission_t admission;
    ucn_result_t result;

    if (durable != NULL) {
        *durable = false;
    }
    if (owner == NULL || transaction == NULL || durable == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!owner_is_valid(owner) || owner_is_busy(owner)) {
        return UCN_ERR_STATE;
    }
    if (committed_matches(&owner->durable_state, transaction, operation_id)) {
        transaction_revoke_for_commit(owner, transaction);
        transaction->state = UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE;
        *durable = true;
        return UCN_OK;
    }
    result = build_commit_request(owner, operation_id, transaction, &request);
    if (result != UCN_OK) {
        return result;
    }
    admission = ucn_cluster_persist_request_admit(&owner->durable_state,
                                                   &request);
    if (admission != UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW) {
        return UCN_ERR_STATE;
    }
    return owner_submit(owner, transaction, &request,
                        UCN_CLUSTER_REKEY_PERSIST_ACTION_COMMIT,
                        now_ms, durable);
}

ucn_result_t ucn_cluster_rekey_persist_begin_abort(
    ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms,
    bool *durable)
{
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_request_admission_t admission;
    ucn_result_t result;

    if (durable != NULL) {
        *durable = false;
    }
    if (owner == NULL || transaction == NULL || durable == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!owner_is_valid(owner) || owner_is_busy(owner)) {
        return UCN_ERR_STATE;
    }
    if (aborted_matches(&owner->durable_state, transaction, operation_id)) {
        *durable = true;
        return UCN_OK;
    }
    result = build_abort_request(owner, operation_id, transaction, &request);
    if (result != UCN_OK) {
        return result;
    }
    admission = ucn_cluster_persist_request_admit(&owner->durable_state,
                                                   &request);
    if (admission != UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW) {
        return UCN_ERR_STATE;
    }
    return owner_submit(owner, transaction, &request,
                        UCN_CLUSTER_REKEY_PERSIST_ACTION_ABORT,
                        now_ms, durable);
}

ucn_result_t ucn_cluster_rekey_persist_poll(
    ucn_cluster_rekey_persist_owner_t *owner,
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms,
    bool *durable,
    ucn_cluster_rekey_persist_action_t *completed_action)
{
    ucn_cluster_persist_completion_t completion;
    ucn_cluster_rekey_persist_action_t action;
    ucn_result_t result;

    if (durable != NULL) {
        *durable = false;
    }
    if (completed_action != NULL) {
        *completed_action = UCN_CLUSTER_REKEY_PERSIST_ACTION_NONE;
    }
    if (owner == NULL || transaction == NULL || durable == NULL ||
        completed_action == NULL || !owner_is_valid(owner) ||
        owner->io_active || !owner->pending ||
        !ucn_cluster_persist_provider_supports_async(owner->provider) ||
        !callback_enter(owner)) {
        return UCN_ERR_STATE;
    }
    owner->io_active = true;
    completion = owner->provider->poll(owner->provider->context,
                                       owner->pending_token);
    owner->io_active = false;
    callback_leave(owner);
    if (!ucn_cluster_persist_provider_accepts_completion(owner->provider,
                                                          &completion)) {
        transaction_fence(owner, transaction, UCN_ERR_STATE, now_ms);
        return UCN_ERR_STATE;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_PENDING) {
        return UCN_OK;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_FAILED) {
        transaction_fence(owner, transaction, completion.failure, now_ms);
        return completion.failure;
    }
    action = (ucn_cluster_rekey_persist_action_t)owner->pending_action;
    result = resolve_committed(owner, transaction, &owner->pending_request,
                               action, now_ms);
    if (result != UCN_OK) {
        return result;
    }
    owner->pending = false;
    owner->pending_token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    owner->pending_action = (uint8_t)UCN_CLUSTER_REKEY_PERSIST_ACTION_NONE;
    (void)memset(&owner->pending_request, 0,
                 sizeof(owner->pending_request));
    *durable = true;
    *completed_action = action;
    return UCN_OK;
}
