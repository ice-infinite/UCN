#include "ucn/ucn_cluster_config_persistence.h"

#include "ucn/ucn_cluster_config_backup.h"
#include "ucn/ucn_cluster_config_joint.h"

#include <string.h>

#define UCN_CLUSTER_CONFIG_PERSIST_DIGEST_SEED_A UINT32_C(0x811C9DC5)
#define UCN_CLUSTER_CONFIG_PERSIST_DIGEST_SEED_B UINT32_C(0xA5A5A5A5)
#define UCN_CLUSTER_CONFIG_PERSIST_DIGEST_SEED_C UINT32_C(0x3C6EF372)
#define UCN_CLUSTER_CONFIG_PERSIST_DIGEST_SEED_D UINT32_C(0x9E3779B9)
#define UCN_CLUSTER_CONFIG_PERSIST_DIGEST_PRIME UINT32_C(16777619)

/* Provider callbacks are foreign code.  This scope is deliberately external
 * to ucn_cluster_config_persist_owner_t: owner_init() is permitted to receive
 * an uninitialized owner, so inspecting owner->io_active before first
 * initialization would be unsafe.  One callback at a time is fail-closed;
 * nested I/O for another owner is rejected as well. */
static ucn_cluster_config_persist_owner_t *provider_callback_owner;

static bool provider_callback_enter(ucn_cluster_config_persist_owner_t *owner)
{
    if (owner == NULL || provider_callback_owner != NULL) {
        return false;
    }
    provider_callback_owner = owner;
    return true;
}

static void provider_callback_leave(ucn_cluster_config_persist_owner_t *owner)
{
    if (provider_callback_owner == owner) {
        provider_callback_owner = NULL;
    }
}

static bool config_ref_equal(const ucn_cluster_persist_config_ref_t *left,
                             const ucn_cluster_persist_config_ref_t *right)
{
    return left != NULL && right != NULL && left->valid == right->valid &&
           (!left->valid ||
            (left->config_id == right->config_id &&
             left->generation == right->generation &&
             memcmp(left->digest, right->digest, sizeof(left->digest)) == 0));
}

static uint32_t digest_update(uint32_t hash, const uint8_t *bytes,
                              size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        hash = (hash ^ (uint32_t)bytes[index]) *
               UCN_CLUSTER_CONFIG_PERSIST_DIGEST_PRIME;
    }
    return hash;
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0U] = (uint8_t)(value >> 24U);
    output[1U] = (uint8_t)(value >> 16U);
    output[2U] = (uint8_t)(value >> 8U);
    output[3U] = (uint8_t)value;
}

static bool owner_is_valid(const ucn_cluster_config_persist_owner_t *owner)
{
    return owner != NULL &&
           ucn_cluster_persist_provider_is_compatible(owner->provider) &&
           owner->config_store != NULL &&
           ucn_cluster_persist_state_is_valid(&owner->durable_state) &&
           owner->durable_state.record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 &&
           owner->durable_state.committed_config.valid;
}

static bool owner_is_busy(const ucn_cluster_config_persist_owner_t *owner)
{
    return owner != NULL && (owner->io_active || owner->pending);
}

/* This is the only path to provider->load().  The reentrancy gate must be
 * established before foreign provider code gets control. */
static ucn_result_t owner_load(
    ucn_cluster_config_persist_owner_t *owner,
    ucn_cluster_persist_load_result_t *loaded)
{
    ucn_result_t result;

    if (owner == NULL || loaded == NULL || owner->io_active ||
        !ucn_cluster_persist_provider_is_compatible(owner->provider)) {
        return UCN_ERR_STATE;
    }
    if (!provider_callback_enter(owner)) {
        return UCN_ERR_STATE;
    }
    owner->io_active = true;
    result = owner->provider->load(owner->provider->context, loaded);
    owner->io_active = false;
    provider_callback_leave(owner);
    return result;
}

static ucn_result_t owner_reload_exact(
    ucn_cluster_config_persist_owner_t *owner,
    const ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_load_result_t loaded;
    ucn_result_t result;

    if (owner == NULL || request == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&loaded, 0, sizeof(loaded));
    result = owner_load(owner, &loaded);
    if (result != UCN_OK || !ucn_cluster_persist_load_result_is_valid(&loaded) ||
        loaded.state != UCN_CLUSTER_PERSIST_LOAD_READY ||
        memcmp(&loaded.snapshot, &request->next_state,
               sizeof(loaded.snapshot)) != 0) {
        return UCN_ERR_STATE;
    }
    {
        ucn_cluster_config_store_recovery_t recovery;

        if (ucn_cluster_config_store_recover(owner->config_store,
                                             &loaded.snapshot,
                                             &recovery) != UCN_OK) {
            return UCN_ERR_STATE;
        }
    }
    owner->durable_state = loaded.snapshot;
    return UCN_OK;
}

static ucn_result_t owner_submit(
    ucn_cluster_config_persist_owner_t *owner,
    const ucn_cluster_persist_request_t *request,
    ucn_cluster_config_persist_action_t action,
    bool *durable)
{
    ucn_cluster_persist_completion_t completion;
    ucn_result_t result;

    if (owner == NULL || request == NULL || durable == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner->io_active || owner->pending) {
        return UCN_ERR_STATE;
    }
    if (!provider_callback_enter(owner)) {
        return UCN_ERR_STATE;
    }
    *durable = false;
    /* PENDING is only known after submit returns. io_active is deliberately
     * live before the callback, so a synchronous reentry cannot bypass the
     * future pending gate or submit a second request. */
    owner->io_active = true;
    completion = owner->provider->submit(owner->provider->context, request);
    owner->io_active = false;
    provider_callback_leave(owner);
    if (!ucn_cluster_persist_provider_accepts_completion(owner->provider,
                                                          &completion)) {
        return UCN_ERR_STATE;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_FAILED) {
        return completion.failure;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_PENDING) {
        owner->pending_request = *request;
        owner->pending_token = completion.token;
        owner->pending_action = (uint8_t)action;
        owner->pending = true;
        return UCN_OK;
    }
    result = owner_reload_exact(owner, request);
    if (result != UCN_OK) {
        return result;
    }
    *durable = true;
    return UCN_OK;
}

static ucn_result_t prepare_request(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_config_ref_t expected_base;
    ucn_cluster_persist_config_ref_t staging;

    if (owner == NULL || tx == NULL || request == NULL ||
        operation_id == 0U ||
        operation_id > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        !ucn_cluster_config_tx_is_active(tx) ||
        ucn_cluster_config_persist_ref_from_state(&tx->base_config,
                                                  &expected_base) != UCN_OK ||
        !config_ref_equal(&owner->durable_state.committed_config,
                          &expected_base) ||
        ucn_cluster_config_persist_ref_from_joint_new(&tx->proposed_config,
                                                      &staging) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner->durable_state.config_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        owner->durable_state.rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        return UCN_ERR_STATE;
    }
    (void)memset(request, 0, sizeof(*request));
    request->operation_id = operation_id;
    request->operation = UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE;
    request->next_state = owner->durable_state;
    request->next_state.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    request->next_state.config_transaction.transaction_id = tx->transaction_id;
    request->next_state.config_transaction.staging_config = staging;
    return ucn_cluster_persist_request_finalize(request);
}

static ucn_result_t commit_request(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_config_ref_t expected_staging;

    if (owner == NULL || tx == NULL || request == NULL ||
        operation_id == 0U ||
        operation_id > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        !ucn_cluster_config_joint_quorum_reached(tx) ||
        ucn_cluster_config_persist_ref_from_joint_new(&tx->proposed_config,
                                                      &expected_staging) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner->durable_state.config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        owner->durable_state.config_transaction.transaction_id !=
            tx->transaction_id ||
        owner->durable_state.last_completed_operation !=
            UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT ||
        !config_ref_equal(
            &owner->durable_state.config_transaction.staging_config,
            &expected_staging)) {
        return UCN_ERR_STATE;
    }
    (void)memset(request, 0, sizeof(*request));
    request->operation_id = operation_id;
    request->operation = UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT;
    request->next_state = owner->durable_state;
    request->next_state.committed_config = expected_staging;
    request->next_state.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    return ucn_cluster_persist_request_finalize(request);
}

static ucn_result_t joint_request(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_config_ref_t expected_staging;

    if (owner == NULL || tx == NULL || request == NULL ||
        operation_id == 0U ||
        operation_id > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        !ucn_cluster_config_tx_is_active(tx) ||
        !ucn_cluster_config_joint_quorum_reached(tx) ||
        ucn_cluster_config_persist_ref_from_joint_new(&tx->proposed_config,
                                                      &expected_staging) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner->durable_state.config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        owner->durable_state.config_transaction.transaction_id !=
            tx->transaction_id ||
        !config_ref_equal(
            &owner->durable_state.config_transaction.staging_config,
            &expected_staging)) {
        return UCN_ERR_STATE;
    }
    (void)memset(request, 0, sizeof(*request));
    request->operation_id = operation_id;
    request->operation = UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT;
    request->next_state = owner->durable_state;
    return ucn_cluster_persist_request_finalize(request);
}

static ucn_result_t abort_request(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    uint32_t now_ms,
    ucn_cluster_persist_request_t *request)
{
    ucn_cluster_persist_config_ref_t expected_staging;

    if (owner == NULL || tx == NULL || request == NULL ||
        operation_id == 0U ||
        operation_id > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        !ucn_cluster_config_tx_is_active(tx) ||
        !ucn_cluster_config_tx_is_expired(tx, now_ms)) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner->durable_state.config_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        owner->durable_state.config_transaction.transaction_id !=
            tx->transaction_id ||
        ucn_cluster_config_persist_ref_from_joint_new(&tx->proposed_config,
                                                      &expected_staging) != UCN_OK ||
        !config_ref_equal(
            &owner->durable_state.config_transaction.staging_config,
            &expected_staging)) {
        return UCN_ERR_STATE;
    }
    (void)memset(request, 0, sizeof(*request));
    request->operation_id = operation_id;
    request->operation = UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT;
    request->next_state = owner->durable_state;
    request->next_state.config_transaction.phase =
        UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    return ucn_cluster_persist_request_finalize(request);
}

static bool completed_operation_matches(
    const ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    ucn_cluster_persist_operation_t operation,
    uint32_t transaction_id)
{
    return owner != NULL &&
           owner->durable_state.last_completed_operation_id == operation_id &&
           owner->durable_state.last_completed_operation == (uint8_t)operation &&
           owner->durable_state.config_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
           owner->durable_state.config_transaction.transaction_id == transaction_id;
}

static bool completed_joint_matches(
    const ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    uint32_t transaction_id,
    const ucn_cluster_persist_config_ref_t *staging)
{
    return owner != NULL && staging != NULL &&
           owner->durable_state.last_completed_operation_id == operation_id &&
           owner->durable_state.last_completed_operation ==
               UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT &&
           owner->durable_state.config_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
           owner->durable_state.config_transaction.transaction_id ==
               transaction_id &&
           config_ref_equal(&owner->durable_state.config_transaction.staging_config,
                            staging);
}

static bool joint_runtime_matches(
    const struct ucn_cluster_config_joint_runtime *joint_runtime,
    const ucn_cluster_config_tx_t *tx)
{
    ucn_cluster_persist_config_ref_t runtime_base;
    ucn_cluster_persist_config_ref_t tx_base;
    ucn_cluster_persist_config_ref_t runtime_new;
    ucn_cluster_persist_config_ref_t tx_new;

    return joint_runtime != NULL && tx != NULL &&
           ucn_cluster_config_joint_runtime_is_valid(joint_runtime) &&
           joint_runtime->joint_active &&
           joint_runtime->transaction.phase ==
               (uint8_t)UCN_CLUSTER_CONFIG_TX_PHASE_JOINT &&
           joint_runtime->transaction.transaction_id == tx->transaction_id &&
           ucn_cluster_config_persist_ref_from_state(
               &joint_runtime->transaction.base_config, &runtime_base) == UCN_OK &&
           ucn_cluster_config_persist_ref_from_state(&tx->base_config,
                                                      &tx_base) == UCN_OK &&
           ucn_cluster_config_persist_ref_from_joint_new(
               &joint_runtime->active_config, &runtime_new) == UCN_OK &&
           ucn_cluster_config_persist_ref_from_joint_new(&tx->proposed_config,
                                                          &tx_new) == UCN_OK &&
           config_ref_equal(&runtime_base, &tx_base) &&
           config_ref_equal(&runtime_new, &tx_new);
}

ucn_result_t ucn_cluster_config_persist_ref_from_state(
    const ucn_cluster_config_state_t *config_state,
    ucn_cluster_persist_config_ref_t *output)
{
    uint8_t serialized[UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES];
    ucn_cluster_persist_config_ref_t candidate;
    uint32_t hashes[4U];
    size_t index;

    if (config_state == NULL || output == NULL ||
        !ucn_cluster_config_state_is_valid(config_state) ||
        ucn_cluster_config_state_serialize(config_state, serialized,
                                           sizeof(serialized)) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    hashes[0U] = digest_update(UCN_CLUSTER_CONFIG_PERSIST_DIGEST_SEED_A,
                               serialized, sizeof(serialized));
    hashes[1U] = digest_update(UCN_CLUSTER_CONFIG_PERSIST_DIGEST_SEED_B,
                               serialized, sizeof(serialized));
    hashes[2U] = digest_update(UCN_CLUSTER_CONFIG_PERSIST_DIGEST_SEED_C,
                               serialized, sizeof(serialized));
    hashes[3U] = digest_update(UCN_CLUSTER_CONFIG_PERSIST_DIGEST_SEED_D,
                               serialized, sizeof(serialized));
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.valid = true;
    candidate.config_id = config_state->config_id;
    candidate.generation = config_state->config_id;
    for (index = 0U; index < 4U; ++index) {
        write_u32_be(candidate.digest + (index * 4U), hashes[index]);
    }
    *output = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_persist_ref_from_joint_new(
    const ucn_cluster_config_state_t *joint_state,
    ucn_cluster_persist_config_ref_t *output)
{
    ucn_cluster_config_state_t stable_new;

    if (joint_state == NULL || output == NULL ||
        !ucn_cluster_config_state_is_valid(joint_state) ||
        joint_state->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT ||
        !ucn_cluster_config_state_promote_joint(&stable_new, joint_state)) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_cluster_config_persist_ref_from_state(&stable_new, output);
}

ucn_result_t ucn_cluster_config_persist_owner_init(
    ucn_cluster_config_persist_owner_t *owner,
    const ucn_cluster_persist_provider_t *provider,
    ucn_cluster_config_store_t *config_store)
{
    uint8_t previous_owner[sizeof(*owner)];
    ucn_cluster_persist_load_result_t loaded;
    ucn_cluster_config_store_recovery_t recovery;
    ucn_result_t result;

    if (owner == NULL || config_store == NULL ||
        !ucn_cluster_persist_provider_is_compatible(provider)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!provider_callback_enter(owner)) {
        return UCN_ERR_STATE;
    }
    /* Make even the initial provider load reentrancy-safe, while preserving
     * the public no-write-on-init-failure contract. */
    (void)memcpy(previous_owner, owner, sizeof(previous_owner));
    (void)memset(owner, 0, sizeof(*owner));
    owner->provider = provider;
    owner->config_store = config_store;
    owner->io_active = true;
    (void)memset(&loaded, 0, sizeof(loaded));
    result = provider->load(provider->context, &loaded);
    owner->io_active = false;
    provider_callback_leave(owner);
    if (result != UCN_OK || !ucn_cluster_persist_load_result_is_valid(&loaded) ||
        loaded.state != UCN_CLUSTER_PERSIST_LOAD_READY ||
        !loaded.snapshot.committed_config.valid ||
        loaded.snapshot.record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2) {
        (void)memcpy(owner, previous_owner, sizeof(previous_owner));
        return UCN_ERR_STATE;
    }
    if (ucn_cluster_config_store_recover(config_store, &loaded.snapshot,
                                         &recovery) != UCN_OK) {
        (void)memcpy(owner, previous_owner, sizeof(previous_owner));
        return UCN_ERR_STATE;
    }
    owner->durable_state = loaded.snapshot;
    return UCN_OK;
}

bool ucn_cluster_config_persist_owner_is_pending(
    const ucn_cluster_config_persist_owner_t *owner)
{
    return owner != NULL && owner->pending;
}

const ucn_cluster_persist_state_t *ucn_cluster_config_persist_owner_state(
    const ucn_cluster_config_persist_owner_t *owner)
{
    return !owner_is_busy(owner) && owner_is_valid(owner) ?
               &owner->durable_state :
               NULL;
}

ucn_result_t ucn_cluster_config_persist_begin_prepare(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    bool *durable)
{
    ucn_cluster_persist_request_t request;
    ucn_result_t result;

    if (durable != NULL) {
        *durable = false;
    }
    if (owner == NULL || durable == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner_is_busy(owner)) {
        return UCN_ERR_STATE;
    }
    if (!owner_is_valid(owner)) {
        return UCN_ERR_ARGUMENT;
    }
    result = prepare_request(owner, operation_id, tx, &request);
    if (result != UCN_OK) {
        return result;
    }
    {
        ucn_cluster_config_state_t stable_new;

        if (!ucn_cluster_config_state_promote_joint(&stable_new,
                                                    &tx->proposed_config) ||
            ucn_cluster_config_store_write_stable(
                owner->config_store, &stable_new,
                &request.next_state.config_transaction.staging_config) != UCN_OK) {
            return UCN_ERR_STATE;
        }
    }
    return owner_submit(owner, &request,
                        UCN_CLUSTER_CONFIG_PERSIST_ACTION_PREPARE, durable);
}

ucn_result_t ucn_cluster_config_persist_begin_joint(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    bool *durable)
{
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_config_ref_t expected_staging;
    ucn_result_t result;

    if (durable != NULL) {
        *durable = false;
    }
    if (owner == NULL || durable == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner_is_busy(owner)) {
        return UCN_ERR_STATE;
    }
    if (!owner_is_valid(owner)) {
        return UCN_ERR_ARGUMENT;
    }
    if (tx != NULL && ucn_cluster_config_tx_is_valid(tx) &&
        ucn_cluster_config_persist_ref_from_joint_new(&tx->proposed_config,
                                                      &expected_staging) == UCN_OK &&
        completed_joint_matches(owner, operation_id, tx->transaction_id,
                                &expected_staging)) {
        *durable = true;
        return UCN_OK;
    }
    result = joint_request(owner, operation_id, tx, &request);
    if (result != UCN_OK) {
        return result;
    }
    return owner_submit(owner, &request,
                        UCN_CLUSTER_CONFIG_PERSIST_ACTION_JOINT, durable);
}

ucn_result_t ucn_cluster_config_persist_begin_commit(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    const struct ucn_cluster_config_joint_runtime *joint_runtime,
    const struct ucn_cluster_config_backup_gate *backup_gate,
    bool *durable)
{
    ucn_cluster_persist_request_t request;
    ucn_result_t result;
    bool ha_ready;

    if (durable != NULL) {
        *durable = false;
    }
    if (owner == NULL || durable == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner_is_busy(owner)) {
        return UCN_ERR_STATE;
    }
    if (!owner_is_valid(owner)) {
        return UCN_ERR_ARGUMENT;
    }
    /* A reset after a committed submit/load proof may replay exactly the same
     * owner request.  It is already durable, so do not submit a second write.
     * Any distinct operation ID or Config identity remains fail-closed below. */
    if (tx != NULL && ucn_cluster_config_tx_is_valid(tx) &&
        completed_operation_matches(
                          owner, operation_id,
                          UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT,
                          tx->transaction_id)) {
        ucn_cluster_persist_config_ref_t expected;

        if (ucn_cluster_config_persist_ref_from_joint_new(&tx->proposed_config,
                                                          &expected) == UCN_OK &&
            config_ref_equal(&owner->durable_state.committed_config,
                             &expected)) {
            ucn_cluster_config_store_recovery_t recovery;

            if (ucn_cluster_config_store_recover(owner->config_store,
                                                 &owner->durable_state,
                                                 &recovery) != UCN_OK) {
                return UCN_ERR_STATE;
            }
            *durable = true;
            return UCN_OK;
        }
        return UCN_ERR_STATE;
    }
    /* This is deliberately before Config-store recovery and before submit.
     * A missing/mismatched/still-unacked Backup must leave the durable state
     * PREPARED; runtime_commit retains the same check as defense in depth. */
    if (!joint_runtime_matches(joint_runtime, tx) ||
        !ucn_cluster_config_backup_gate_commit_allowed_for_tx(backup_gate, tx,
                                                               &ha_ready)) {
        return UCN_ERR_STATE;
    }
    (void)ha_ready;
    {
        ucn_cluster_config_store_recovery_t recovery;

        if (ucn_cluster_config_store_recover(owner->config_store,
                                             &owner->durable_state,
                                             &recovery) != UCN_OK) {
            return UCN_ERR_STATE;
        }
    }
    result = commit_request(owner, operation_id, tx, &request);
    if (result != UCN_OK) {
        return result;
    }
    return owner_submit(owner, &request,
                        UCN_CLUSTER_CONFIG_PERSIST_ACTION_COMMIT, durable);
}

ucn_result_t ucn_cluster_config_persist_begin_abort(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    uint32_t now_ms,
    bool *durable)
{
    ucn_cluster_persist_request_t request;
    ucn_cluster_persist_config_ref_t expected_staging;
    ucn_result_t result;

    if (durable != NULL) {
        *durable = false;
    }
    if (owner == NULL || durable == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner_is_busy(owner)) {
        return UCN_ERR_STATE;
    }
    if (!owner_is_valid(owner)) {
        return UCN_ERR_ARGUMENT;
    }
    /* CONFIG_ABORT exact replay binds both C_old and the terminally retained
     * C_new staging identity.  A caller cannot reuse a txid/operation ID for
     * a different proposed Config after restart. */
    if (tx != NULL && ucn_cluster_config_tx_is_valid(tx) &&
        completed_operation_matches(
                          owner, operation_id,
                          UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT,
                          tx->transaction_id)) {
        ucn_cluster_persist_config_ref_t expected;

        if (ucn_cluster_config_persist_ref_from_state(&tx->base_config,
                                                      &expected) == UCN_OK &&
            ucn_cluster_config_persist_ref_from_joint_new(
                &tx->proposed_config,
                &expected_staging) == UCN_OK &&
            config_ref_equal(&owner->durable_state.committed_config,
                             &expected) &&
            config_ref_equal(
                &owner->durable_state.config_transaction.staging_config,
                &expected_staging)) {
            ucn_cluster_config_store_recovery_t recovery;

            if (ucn_cluster_config_store_recover(owner->config_store,
                                                 &owner->durable_state,
                                                 &recovery) != UCN_OK) {
                return UCN_ERR_STATE;
            }
            *durable = true;
            return UCN_OK;
        }
        return UCN_ERR_STATE;
    }
    {
        ucn_cluster_config_store_recovery_t recovery;

        if (ucn_cluster_config_store_recover(owner->config_store,
                                             &owner->durable_state,
                                             &recovery) != UCN_OK) {
            return UCN_ERR_STATE;
        }
    }
    result = abort_request(owner, operation_id, tx, now_ms, &request);
    if (result != UCN_OK) {
        return result;
    }
    return owner_submit(owner, &request,
                        UCN_CLUSTER_CONFIG_PERSIST_ACTION_ABORT, durable);
}

ucn_result_t ucn_cluster_config_persist_poll(
    ucn_cluster_config_persist_owner_t *owner,
    bool *durable,
    ucn_cluster_config_persist_action_t *completed_action)
{
    ucn_cluster_persist_completion_t completion;
    ucn_result_t result;
    uint8_t action;

    if (durable != NULL) {
        *durable = false;
    }
    if (completed_action != NULL) {
        *completed_action = UCN_CLUSTER_CONFIG_PERSIST_ACTION_NONE;
    }
    if (owner == NULL || durable == NULL || completed_action == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (owner->io_active) {
        return UCN_ERR_STATE;
    }
    if (!owner_is_valid(owner) || !owner->pending ||
        !ucn_cluster_persist_provider_supports_async(owner->provider)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!provider_callback_enter(owner)) {
        return UCN_ERR_STATE;
    }
    owner->io_active = true;
    completion = owner->provider->poll(owner->provider->context,
                                       owner->pending_token);
    owner->io_active = false;
    provider_callback_leave(owner);
    if (!ucn_cluster_persist_provider_accepts_completion(owner->provider,
                                                          &completion)) {
        return UCN_ERR_STATE;
    }
    if (completion.state == UCN_CLUSTER_PERSIST_PENDING) {
        return UCN_OK;
    }
    action = owner->pending_action;
    if (completion.state == UCN_CLUSTER_PERSIST_FAILED) {
        owner->pending = false;
        owner->pending_token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
        owner->pending_action = (uint8_t)UCN_CLUSTER_CONFIG_PERSIST_ACTION_NONE;
        (void)memset(&owner->pending_request, 0,
                     sizeof(owner->pending_request));
        return completion.failure;
    }
    result = owner_reload_exact(owner, &owner->pending_request);
    if (result != UCN_OK) {
        /* The provider claims completion but exact durable proof failed.
         * Retain pending state to keep the owner fail-closed and permit only
         * a future poll/reload retry; never open a new Config transaction. */
        return result;
    }
    owner->pending = false;
    owner->pending_token = UCN_CLUSTER_PERSIST_TOKEN_NONE;
    owner->pending_action = (uint8_t)UCN_CLUSTER_CONFIG_PERSIST_ACTION_NONE;
    (void)memset(&owner->pending_request, 0, sizeof(owner->pending_request));
    *durable = true;
    *completed_action = (ucn_cluster_config_persist_action_t)action;
    return UCN_OK;
}
