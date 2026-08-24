#include "ucn/ucn_cluster_takeover_persist.h"

#include "ucn_cluster_takeover_internal.h"

#include <string.h>

/* This is intentionally module-wide rather than stored in a caller's owner:
 * a Provider may synchronously reenter while init/submit/load/poll is on the
 * stack, including before a fresh owner object has a safe initialized value.
 * M10 is a controlled single-owner experiment; fail-closed global exclusion is
 * preferable to relying on an uninitialized per-object flag. */
static bool takeover_persist_provider_callback_active = false;

static bool epoch_is_exact(const ucn_cluster_epoch_t *left,
                           const ucn_cluster_epoch_t *right)
{
    return left != NULL && right != NULL &&
           left->cluster_id == right->cluster_id && left->term == right->term &&
           left->head_node_id == right->head_node_id;
}

static bool transaction_persistence_context_is_valid(
    const ucn_cluster_persist_state_t *state,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    const ucn_cluster_backup_epoch_t *backup_epoch;

    if (!ucn_cluster_persist_state_is_valid(state) ||
        !ucn_cluster_takeover_transaction_is_active(transaction) ||
        !state->has_active_epoch || !state->has_max_epoch ||
        !epoch_is_exact(&state->active_epoch, &state->max_epoch) ||
        !state->committed_config.valid ||
        state->committed_config.config_id != transaction->vote_id.config_id ||
        state->config_transaction.phase == UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        state->rekey_transaction.phase == UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        return false;
    }
    backup_epoch = &transaction->frozen_snapshot_epoch.backup_epoch;
    return state->active_epoch.cluster_id == backup_epoch->cluster_id &&
           state->active_epoch.term == backup_epoch->term &&
           state->active_epoch.head_node_id == backup_epoch->head_node_id;
}

static void vote_from_transaction(ucn_cluster_persist_vote_t *output,
                                  const ucn_cluster_takeover_transaction_t *transaction)
{
    (void)memset(output, 0, sizeof(*output));
    output->valid = true;
    output->epoch.cluster_id = transaction->vote_id.cluster_id;
    output->epoch.term = transaction->vote_id.old_term;
    output->epoch.head_node_id =
        transaction->frozen_snapshot_epoch.backup_epoch.head_node_id;
    output->voted_for_node_id = transaction->vote_id.backup_node_id;
    output->backup_generation = transaction->vote_id.backup_generation;
    output->proposed_term = transaction->vote_id.proposed_term;
    output->config_id = transaction->vote_id.config_id;
    output->snapshot_id = transaction->vote_id.snapshot_id;
}

static bool vote_matches_transaction(
    const ucn_cluster_persist_vote_t *vote,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    ucn_cluster_persist_vote_t expected;

    if (vote == NULL || transaction == NULL) {
        return false;
    }
    vote_from_transaction(&expected, transaction);
    return ucn_cluster_persist_vote_is_complete_takeover(vote) &&
           vote->epoch.cluster_id == expected.epoch.cluster_id &&
           vote->epoch.term == expected.epoch.term &&
           vote->epoch.head_node_id == expected.epoch.head_node_id &&
           vote->voted_for_node_id == expected.voted_for_node_id &&
           vote->backup_generation == expected.backup_generation &&
           vote->proposed_term == expected.proposed_term &&
           vote->config_id == expected.config_id &&
           vote->snapshot_id == expected.snapshot_id;
}

ucn_result_t ucn_cluster_takeover_persist_vote_request_build(
    const ucn_cluster_persist_state_t *committed_state,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    ucn_cluster_persist_request_t *output)
{
    ucn_cluster_persist_request_t candidate;

    if (output == NULL || operation_id == 0U ||
        !transaction_persistence_context_is_valid(committed_state, transaction) ||
        /* A Vote from the current Active Epoch is a live one-vote promise.
         * Historical VoteIds (including v1/v2 partial records) must not
         * permanently prevent a new Epoch from opening its own full vote. */
        (committed_state->last_vote.valid &&
         epoch_is_exact(&committed_state->last_vote.epoch,
                        &committed_state->active_epoch))) {
        return UCN_ERR_STATE;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.operation_id = operation_id;
    candidate.operation = UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_VOTE_COMMIT;
    candidate.next_state = *committed_state;
    candidate.next_state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    vote_from_transaction(&candidate.next_state.last_vote, transaction);
    if (ucn_cluster_persist_request_finalize(&candidate) != UCN_OK ||
        !ucn_cluster_persist_request_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *output = candidate;
    return UCN_OK;
}

bool ucn_cluster_takeover_persist_vote_matches(
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    return durable_state != NULL && durable_state->record_schema_version ==
                                      UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
           transaction_persistence_context_is_valid(durable_state, transaction) &&
           vote_matches_transaction(&durable_state->last_vote, transaction);
}

ucn_result_t ucn_cluster_takeover_persist_epoch_request_build(
    const ucn_cluster_persist_state_t *committed_state,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    ucn_cluster_persist_request_t *output)
{
    ucn_cluster_persist_request_t candidate;

    if (output == NULL || operation_id == 0U ||
        !ucn_cluster_takeover_transaction_quorum_reached(transaction) ||
        !ucn_cluster_takeover_persist_vote_matches(committed_state, transaction)) {
        return UCN_ERR_STATE;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.operation_id = operation_id;
    candidate.operation = UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_EPOCH_COMMIT;
    candidate.next_state = *committed_state;
    candidate.next_state.record_schema_version =
        UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION;
    candidate.next_state.has_active_epoch = true;
    candidate.next_state.active_epoch = transaction->proposed_epoch;
    candidate.next_state.has_max_epoch = true;
    candidate.next_state.max_epoch = transaction->proposed_epoch;
    if (ucn_cluster_persist_request_finalize(&candidate) != UCN_OK ||
        !ucn_cluster_persist_request_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *output = candidate;
    return UCN_OK;
}

bool ucn_cluster_takeover_persist_epoch_matches(
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    return durable_state != NULL && transaction != NULL &&
           ucn_cluster_persist_state_is_valid(durable_state) &&
           durable_state->record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
           durable_state->committed_config.valid &&
           durable_state->committed_config.config_id == transaction->vote_id.config_id &&
           vote_matches_transaction(&durable_state->last_vote, transaction) &&
           durable_state->has_active_epoch && durable_state->has_max_epoch &&
           epoch_is_exact(&durable_state->active_epoch,
                          &transaction->proposed_epoch) &&
           epoch_is_exact(&durable_state->max_epoch,
                          &transaction->proposed_epoch);
}

static bool owner_state_matches_request(
    const ucn_cluster_persist_state_t *loaded,
    const ucn_cluster_persist_request_t *request)
{
    uint8_t expected_record[UCN_CLUSTER_PERSIST_RECORD_BYTES];
    uint8_t loaded_record[UCN_CLUSTER_PERSIST_RECORD_BYTES];

    if (loaded == NULL || request == NULL ||
        loaded->last_completed_operation_id != request->operation_id ||
        loaded->last_completed_operation != (uint8_t)request->operation ||
        loaded->last_completed_operation_fingerprint !=
            request->next_state.last_completed_operation_fingerprint) {
        return false;
    }
    return ucn_cluster_persist_record_encode(&request->next_state, 1U,
                                             expected_record,
                                             sizeof(expected_record)) == UCN_OK &&
           ucn_cluster_persist_record_encode(loaded, 1U, loaded_record,
                                             sizeof(loaded_record)) == UCN_OK &&
           memcmp(expected_record, loaded_record, sizeof(expected_record)) == 0;
}

static void owner_fail_closed(ucn_cluster_takeover_persist_owner_t *owner)
{
    if (owner->persistence_failures != UINT32_MAX) {
        ++owner->persistence_failures;
    }
    owner->pending = false;
    owner->io_active = false;
    owner->pending_token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    owner->phase = (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_NONE;
    (void)memset(&owner->pending_request, 0, sizeof(owner->pending_request));
    owner->faulted = true;
}

static ucn_result_t owner_load_ready(
    const ucn_cluster_persist_provider_t *provider,
    ucn_cluster_persist_state_t *output)
{
    ucn_cluster_persist_load_result_t loaded;
    ucn_result_t result;

    if (provider == NULL || output == NULL ||
        !ucn_cluster_persist_provider_is_compatible(provider) ||
        takeover_persist_provider_callback_active) {
        return UCN_ERR_STATE;
    }
    (void)memset(&loaded, 0, sizeof(loaded));
    takeover_persist_provider_callback_active = true;
    result = provider->load(provider->context, &loaded);
    takeover_persist_provider_callback_active = false;
    if (result != UCN_OK || !ucn_cluster_persist_load_result_is_valid(&loaded) ||
        loaded.state != UCN_CLUSTER_PERSIST_LOAD_READY) {
        return UCN_ERR_STATE;
    }
    *output = loaded.snapshot;
    return UCN_OK;
}

bool ucn_cluster_takeover_persist_owner_is_valid(
    const ucn_cluster_takeover_persist_owner_t *owner)
{
    return owner != NULL && owner->initialized &&
           ucn_cluster_persist_provider_is_compatible(owner->provider) &&
           ucn_cluster_persist_state_is_valid(&owner->durable_state) &&
           (owner->phase == (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_NONE ||
            owner->phase == (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_VOTE ||
            owner->phase == (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_EPOCH) &&
           (!owner->pending ||
            (owner->pending_token != UCN_CLUSTER_PERSIST_TOKEN_NONE &&
             owner->phase != (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_NONE &&
             ucn_cluster_persist_request_is_valid(&owner->pending_request)));
}

ucn_result_t ucn_cluster_takeover_persist_owner_init(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_persist_provider_t *provider)
{
    ucn_cluster_takeover_persist_owner_t candidate;
    ucn_cluster_persist_state_t loaded;

    if (owner == NULL || provider == NULL ||
        !ucn_cluster_persist_provider_is_compatible(provider) ||
        takeover_persist_provider_callback_active) {
        return UCN_ERR_STATE;
    }
    if (owner_load_ready(provider, &loaded) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.provider = provider;
    candidate.durable_state = loaded;
    candidate.initialized = true;
    candidate.phase = (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_NONE;
    if (!ucn_cluster_takeover_persist_owner_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *owner = candidate;
    return UCN_OK;
}

static ucn_result_t owner_reload_prove(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    ucn_cluster_persist_state_t loaded;
    bool proof_ok;

    if (owner_load_ready(owner->provider, &loaded) != UCN_OK ||
        !owner_state_matches_request(&loaded, &owner->pending_request)) {
        owner_fail_closed(owner);
        return UCN_ERR_STATE;
    }
    proof_ok = owner->phase == (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_VOTE ?
                   ucn_cluster_takeover_persist_vote_matches(&loaded, transaction) :
                   ucn_cluster_takeover_persist_epoch_matches(&loaded, transaction);
    if (!proof_ok) {
        owner_fail_closed(owner);
        return UCN_ERR_STATE;
    }
    owner->durable_state = loaded;
    owner->pending = false;
    owner->pending_token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    owner->phase = (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_NONE;
    (void)memset(&owner->pending_request, 0, sizeof(owner->pending_request));
    return UCN_OK;
}

static ucn_result_t owner_begin_request(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    bool epoch,
    bool *committed)
{
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_completion_t completion;
    ucn_result_t result;

    if (committed == NULL || takeover_persist_provider_callback_active ||
        !ucn_cluster_takeover_persist_owner_is_valid(owner) || owner->faulted ||
        owner->pending || owner->io_active) {
        return UCN_ERR_STATE;
    }
    result = epoch ?
                 ucn_cluster_takeover_persist_epoch_request_build(
                     &owner->durable_state, transaction, operation_id, &request) :
                 ucn_cluster_takeover_persist_vote_request_build(
                     &owner->durable_state, transaction, operation_id, &request);
    if (result != UCN_OK) {
        return result;
    }
    owner->pending = true;
    owner->io_active = true;
    owner->phase = epoch ? (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_EPOCH :
                           (uint8_t)UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_VOTE;
    owner->pending_request = request;
    takeover_persist_provider_callback_active = true;
    completion = owner->provider->submit(owner->provider->context,
                                         &owner->pending_request);
    takeover_persist_provider_callback_active = false;
    owner->io_active = false;
    if (!ucn_cluster_persist_provider_accepts_completion(owner->provider,
                                                          &completion) ||
        completion.state == UCN_CLUSTER_PERSIST_FAILED) {
        owner_fail_closed(owner);
        return UCN_ERR_STATE;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_PENDING) {
        owner->pending_token = completion.token;
        *committed = false;
        return UCN_OK;
    }
    if (owner_reload_prove(owner, transaction) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    *committed = true;
    return UCN_OK;
}

ucn_result_t ucn_cluster_takeover_persist_owner_begin_vote(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    bool *committed)
{
    return owner_begin_request(owner, transaction, operation_id, false, committed);
}

ucn_result_t ucn_cluster_takeover_persist_owner_begin_epoch(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    bool *committed)
{
    return owner_begin_request(owner, transaction, operation_id, true, committed);
}

ucn_result_t ucn_cluster_takeover_persist_owner_step(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    ucn_cluster_persist_completion_t completion;

    if (takeover_persist_provider_callback_active ||
        !ucn_cluster_takeover_persist_owner_is_valid(owner) || owner->faulted ||
        !owner->pending || owner->io_active ||
        !ucn_cluster_persist_provider_supports_async(owner->provider)) {
        return UCN_ERR_STATE;
    }
    owner->io_active = true;
    takeover_persist_provider_callback_active = true;
    completion = owner->provider->poll(owner->provider->context, owner->pending_token);
    takeover_persist_provider_callback_active = false;
    owner->io_active = false;
    if (!ucn_cluster_persist_provider_accepts_completion(owner->provider,
                                                          &completion) ||
        completion.state == UCN_CLUSTER_PERSIST_FAILED) {
        owner_fail_closed(owner);
        return UCN_ERR_STATE;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_PENDING) {
        return UCN_OK;
    }
    return owner_reload_prove(owner, transaction);
}

bool ucn_cluster_takeover_persist_owner_vote_is_durable(
    const ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    return ucn_cluster_takeover_persist_owner_is_valid(owner) && !owner->faulted &&
           !owner->pending &&
           ucn_cluster_takeover_persist_vote_matches(&owner->durable_state,
                                                      transaction);
}

bool ucn_cluster_takeover_persist_owner_epoch_is_durable(
    const ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction)
{
    return ucn_cluster_takeover_persist_owner_is_valid(owner) && !owner->faulted &&
           !owner->pending &&
           ucn_cluster_takeover_persist_epoch_matches(&owner->durable_state,
                                                       transaction);
}

ucn_result_t ucn_cluster_takeover_persist_owner_apply_durable_vote(
    const ucn_cluster_takeover_persist_owner_t *owner,
    ucn_cluster_takeover_transaction_t *transaction)
{
    if (!ucn_cluster_takeover_persist_owner_vote_is_durable(owner, transaction)) {
        return UCN_ERR_STATE;
    }
    return ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
        transaction, &transaction->vote_id);
}

ucn_result_t ucn_cluster_takeover_persist_owner_apply_durable_epoch(
    const ucn_cluster_takeover_persist_owner_t *owner,
    ucn_cluster_takeover_transaction_t *transaction)
{
    if (!ucn_cluster_takeover_persist_owner_epoch_is_durable(owner, transaction)) {
        return UCN_ERR_STATE;
    }
    return ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
        transaction, &transaction->vote_id);
}
