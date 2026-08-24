/* CLV2-M13 default-OFF Rekey owner.  This file has no wire/Adapter/FSM call
 * site; production integration remains blocked by M05 AUDIT HOLD. */

#include "ucn/ucn_cluster_rekey.h"
#include "ucn/ucn_cluster_storage.h"
#include "ucn/ucn_time.h"

#include <string.h>

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static ucn_result_t serial_next_checked(uint32_t current, uint32_t *next)
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

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool epoch_is_valid(const ucn_cluster_epoch_t *epoch)
{
    return epoch != NULL && epoch->cluster_id != 0U &&
           serial_is_valid(epoch->term) &&
           node_id_is_valid(epoch->head_node_id);
}

static bool epoch_is_equal(const ucn_cluster_epoch_t *left,
                           const ucn_cluster_epoch_t *right)
{
    return left != NULL && right != NULL &&
           left->cluster_id == right->cluster_id &&
           left->term == right->term &&
           left->head_node_id == right->head_node_id;
}

static bool config_ref_is_equal(
    const ucn_cluster_persist_config_ref_t *left,
    const ucn_cluster_persist_config_ref_t *right)
{
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->config_id == right->config_id &&
           left->generation == right->generation &&
           memcmp(left->digest, right->digest, sizeof(left->digest)) == 0;
}

static bool id_request_is_equal(const ucn_cluster_id_request_t *left,
                                const ucn_cluster_id_request_t *right)
{
    return left != NULL && right != NULL &&
           left->purpose == right->purpose &&
           left->local_node_id == right->local_node_id &&
           left->parent_cluster_id == right->parent_cluster_id &&
           left->parent_term == right->parent_term &&
           left->incarnation == right->incarnation &&
           left->round == right->round &&
           left->parent_config_id == right->parent_config_id &&
           left->recovery_round == right->recovery_round;
}

static const ucn_cluster_id_history_entry_t *history_find_cluster_id(
    const ucn_cluster_id_history_t *history, uint32_t cluster_id)
{
    size_t index;

    if (history == NULL || history->count > UCN_CLUSTER_ID_HISTORY_CAPACITY) {
        return NULL;
    }
    for (index = 0U; index < history->count; ++index) {
        if (history->entries[index].cluster_id == cluster_id) {
            return &history->entries[index];
        }
    }
    return NULL;
}

static bool storage_is_zero(const void *storage, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)storage;
    size_t index;

    if (storage == NULL) {
        return false;
    }
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

ucn_result_t ucn_cluster_rekey_threshold_evaluate(
    const ucn_cluster_rekey_serial_view_t *view,
    ucn_cluster_rekey_threshold_decision_t *decision)
{
    ucn_cluster_rekey_threshold_decision_t candidate;

    if (view == NULL || decision == NULL || !serial_is_valid(view->term) ||
        !serial_is_valid(view->config_id) ||
        (view->has_backup && !serial_is_valid(view->backup_generation)) ||
        (!view->has_backup && view->backup_generation != 0U) ||
        (view->has_snapshot &&
         (!view->has_backup || !serial_is_valid(view->snapshot_id))) ||
        (!view->has_snapshot && view->snapshot_id != 0U)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    if (view->term >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        candidate.trigger_mask |= (uint8_t)UCN_CLUSTER_REKEY_TRIGGER_TERM;
    }
    if (view->config_id >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        candidate.trigger_mask |= (uint8_t)UCN_CLUSTER_REKEY_TRIGGER_CONFIG_ID;
    }
    if (view->has_backup &&
        view->backup_generation >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        candidate.trigger_mask |=
            (uint8_t)UCN_CLUSTER_REKEY_TRIGGER_BACKUP_GENERATION;
    }
    candidate.rekey_required = candidate.trigger_mask != 0U;
    candidate.snapshot_generation_rotation_required =
        !candidate.rekey_required && view->has_snapshot &&
        view->snapshot_id >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    *decision = candidate;
    return UCN_OK;
}

static bool durable_predecessor_is_valid(
    const ucn_cluster_persist_state_t *durable,
    const ucn_cluster_epoch_t *epoch,
    const ucn_cluster_persist_config_ref_t *config_ref)
{
    return durable != NULL &&
           ucn_cluster_persist_state_is_valid(durable) &&
           durable->record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
           durable->has_active_epoch && durable->has_max_epoch &&
           epoch_is_equal(&durable->active_epoch, epoch) &&
           epoch_is_equal(&durable->max_epoch, epoch) &&
           config_ref_is_equal(&durable->committed_config, config_ref) &&
           durable->config_transaction.phase !=
               UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
           durable->rekey_transaction.phase !=
               UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
           !durable->committed_rekey.valid && !durable->tombstone.valid;
}

static bool rekey_ref_is_equal(const ucn_cluster_persist_rekey_ref_t *left,
                               const ucn_cluster_persist_rekey_ref_t *right)
{
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->generation == right->generation &&
           left->next_incarnation == right->next_incarnation &&
           left->prepare_nonce == right->prepare_nonce &&
           left->allocation_history_fingerprint ==
               right->allocation_history_fingerprint &&
           left->successor_backup_node_id ==
               right->successor_backup_node_id &&
           epoch_is_equal(&left->predecessor_epoch,
                          &right->predecessor_epoch) &&
            config_ref_is_equal(&left->predecessor_config,
                               &right->predecessor_config) &&
           epoch_is_equal(&left->successor_epoch, &right->successor_epoch) &&
           config_ref_is_equal(&left->successor_config,
                               &right->successor_config);
}

static bool voter_profile_is_valid(
    const ucn_cluster_rekey_voter_profile_t *profile)
{
    return profile != NULL && node_id_is_valid(profile->node_id) &&
           profile->wire_format == UCN_CLUSTER_WIRE_V4_FORMAT_VERSION &&
           (profile->capabilities & UCN_CLUSTER_REKEY_REQUIRED_CAPABILITIES) ==
               UCN_CLUSTER_REKEY_REQUIRED_CAPABILITIES &&
           serial_is_valid(profile->persistence_generation);
}

static bool voter_profiles_match_config(
    const ucn_cluster_config_state_t *config,
    const ucn_cluster_rekey_voter_profile_t *profiles,
    size_t count)
{
    size_t index;

    if (config == NULL || profiles == NULL ||
        !ucn_cluster_config_state_is_valid(config) ||
        config->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        count != config->old_set.count || count > UCN_CLUSTER_MAX_VOTERS) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (!voter_profile_is_valid(&profiles[index]) ||
            profiles[index].node_id != config->old_set.node_ids[index]) {
            return false;
        }
    }
    return true;
}

static int transaction_voter_index(
    const ucn_cluster_rekey_transaction_t *transaction,
    ucn_node_id_t node_id)
{
    uint8_t index;

    if (transaction == NULL) {
        return -1;
    }
    for (index = 0U; index < transaction->voter_count; ++index) {
        if (transaction->voter_profiles[index].node_id == node_id) {
            return (int)index;
        }
    }
    return -1;
}

static bool durable_successor_is_valid(
    const ucn_cluster_persist_state_t *durable,
    const ucn_cluster_rekey_transaction_t *transaction)
{
    return durable != NULL && transaction != NULL &&
           ucn_cluster_persist_state_is_valid(durable) &&
           durable->record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
           durable->has_active_epoch && durable->has_max_epoch &&
           epoch_is_equal(&durable->active_epoch,
                          &transaction->successor_epoch) &&
           epoch_is_equal(&durable->max_epoch,
                          &transaction->successor_epoch) &&
           config_ref_is_equal(&durable->committed_config,
                               &transaction->successor_config_ref) &&
           rekey_ref_is_equal(&durable->committed_rekey,
                              &transaction->durable_rekey_ref) &&
           durable->rekey_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED &&
           durable->rekey_transaction.transaction_id ==
               transaction->transaction_id && durable->tombstone.valid &&
           epoch_is_equal(&durable->tombstone.retired_epoch,
                          &transaction->predecessor_epoch) &&
           durable->tombstone.replacement_cluster_id ==
               transaction->successor_epoch.cluster_id &&
           durable->tombstone.rekey_transaction_id ==
               transaction->transaction_id;
}

static bool durable_prepared_is_valid(
    const ucn_cluster_persist_state_t *durable,
    const ucn_cluster_rekey_transaction_t *transaction);

static void rekey_frame_fill(
    const ucn_cluster_rekey_transaction_t *transaction,
    uint8_t type,
    ucn_cluster_wire_v4_frame_t *output)
{
    ucn_cluster_wire_v4_frame_t candidate;

    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.type = type;
    candidate.role = UCN_CLUSTER_ROLE_HEAD;
    candidate.cluster_id = transaction->predecessor_epoch.cluster_id;
    candidate.term = transaction->predecessor_epoch.term;
    candidate.head_node_id = transaction->predecessor_epoch.head_node_id;
    candidate.words[0U] = transaction->successor_epoch.cluster_id;
    candidate.words[1U] = transaction->successor_epoch.term;
    candidate.words[2U] = transaction->transaction_id;
    candidate.words[3U] = transaction->predecessor_config.config_id;
    candidate.words[4U] = transaction->successor_config.config_id;
    candidate.words[5U] = transaction->nonce;
    *output = candidate;
}

ucn_result_t ucn_cluster_rekey_prepare_frame_build(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state,
    uint32_t now_ms,
    ucn_cluster_wire_v4_frame_t *output)
{
    ucn_cluster_wire_v4_frame_t candidate;

    if (transaction == NULL || durable_state == NULL || output == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        !durable_prepared_is_valid(durable_state, transaction) ||
        (transaction->state != UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS &&
         transaction->state != UCN_CLUSTER_REKEY_STATE_QUORUM)) {
        return UCN_ERR_STATE;
    }
    if (transaction->state == UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS &&
        ucn_deadline_expired(now_ms, transaction->deadline_ms)) {
        return UCN_ERR_TTL;
    }
    rekey_frame_fill(transaction, UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE,
                     &candidate);
    if (!ucn_cluster_wire_v4_frame_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *output = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_rekey_transaction_resume_prepared(
    ucn_cluster_rekey_transaction_t *transaction,
    ucn_cluster_authority_runtime_t *authority,
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_rekey_voter_profile_t *voter_profiles,
    size_t voter_profile_count,
    const ucn_cluster_id_history_t *durable_history,
    uint32_t durable_history_generation,
    uint32_t now_ms)
{
    ucn_cluster_rekey_transaction_t candidate;
    ucn_cluster_persist_config_ref_t active_config_ref;
    ucn_cluster_persist_config_ref_t successor_config_ref;
    const ucn_cluster_persist_rekey_ref_t *staging;
    const ucn_cluster_id_history_entry_t *allocation_entry;
    ucn_cluster_t *cluster;
    ucn_result_t result;

    if (transaction == NULL || authority == NULL || durable_state == NULL ||
        durable_history == NULL ||
        !storage_is_zero(transaction, sizeof(*transaction)) ||
        durable_state->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION ||
        durable_state->rekey_transaction.phase !=
            UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED ||
        !serial_is_valid(durable_state->rekey_transaction.transaction_id)) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_cluster_authority_runtime_preflight(authority, now_ms);
    if (result != UCN_OK) {
        return result;
    }
    cluster = authority->cluster;
    staging = &durable_state->rekey_transaction.staging_rekey;
    allocation_entry = history_find_cluster_id(
        durable_history, staging->successor_epoch.cluster_id);
    if (!authority->initialized || cluster == NULL ||
        cluster->authority_runtime != authority || cluster->persistence_faulted ||
        ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_HEAD ||
        !cluster->authority_active ||
        cluster->authority_phase != UCN_CLUSTER_PHASE_HEAD_STABLE ||
        !ucn_cluster_authority_runtime_quorum_met(authority, now_ms) ||
        !voter_profiles_match_config(&authority->active_config,
                                     voter_profiles, voter_profile_count) ||
        ucn_cluster_config_persist_ref_from_state(
            &authority->active_config, &active_config_ref) != UCN_OK ||
        !durable_state->has_active_epoch || !durable_state->has_max_epoch ||
        !epoch_is_equal(&durable_state->active_epoch,
                        &staging->predecessor_epoch) ||
        !epoch_is_equal(&durable_state->max_epoch,
                        &staging->predecessor_epoch) ||
        !config_ref_is_equal(&active_config_ref,
                             &staging->predecessor_config) ||
        !config_ref_is_equal(&durable_state->committed_config,
                             &staging->predecessor_config) ||
        !serial_is_valid(staging->prepare_nonce) ||
        !serial_is_valid(durable_history_generation) ||
        staging->generation != durable_history_generation ||
        ucn_cluster_id_history_fingerprint(durable_history,
                                            durable_history_generation) == 0U ||
        staging->allocation_history_fingerprint !=
            ucn_cluster_id_history_fingerprint(
                durable_history, durable_history_generation) ||
        allocation_entry == NULL ||
        allocation_entry->identity.purpose != UCN_CLUSTER_ID_PURPOSE_REKEY ||
        allocation_entry->identity.local_node_id !=
            staging->predecessor_epoch.head_node_id ||
        allocation_entry->identity.parent_cluster_id !=
            staging->predecessor_epoch.cluster_id ||
        allocation_entry->identity.parent_term !=
            staging->predecessor_epoch.term ||
        allocation_entry->identity.parent_config_id !=
            staging->predecessor_config.config_id ||
        allocation_entry->identity.incarnation !=
            durable_state->boot_incarnation ||
        !epoch_is_valid(&staging->successor_epoch) ||
        staging->successor_epoch.term != 1U ||
        staging->successor_epoch.head_node_id !=
            staging->predecessor_epoch.head_node_id ||
        (staging->successor_backup_node_id != 0U &&
         !ucn_cluster_voter_set_contains(&authority->active_config.old_set,
                                         staging->successor_backup_node_id))) {
        return UCN_ERR_STATE;
    }

    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.state = UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED;
    candidate.phase = UCN_CLUSTER_PHASE_HEAD_REKEYING;
    candidate.predecessor_epoch = staging->predecessor_epoch;
    candidate.predecessor_config = authority->active_config;
    candidate.predecessor_config_ref = active_config_ref;
    candidate.successor_epoch = staging->successor_epoch;
    if (!ucn_cluster_config_state_init_stable(
            &candidate.successor_config, 1U,
            authority->active_config.old_set.node_ids,
            authority->active_config.old_set.count) ||
        ucn_cluster_config_persist_ref_from_state(
            &candidate.successor_config, &successor_config_ref) != UCN_OK ||
        !config_ref_is_equal(&successor_config_ref,
                             &staging->successor_config)) {
        return UCN_ERR_STATE;
    }
    candidate.successor_config_ref = successor_config_ref;
    candidate.durable_rekey_ref = *staging;
    candidate.allocation_identity = allocation_entry->identity;
    (void)memcpy(candidate.voter_profiles, voter_profiles,
                 voter_profile_count * sizeof(voter_profiles[0U]));
    candidate.voter_count = (uint8_t)voter_profile_count;
    candidate.transaction_id = durable_state->rekey_transaction.transaction_id;
    candidate.nonce = staging->prepare_nonce;
    candidate.allocation_history_generation = durable_history_generation;
    candidate.allocation_history_fingerprint =
        ucn_cluster_id_history_fingerprint(durable_history,
                                            durable_history_generation);
    candidate.started_ms = now_ms;
    candidate.deadline_ms =
        ucn_deadline_from_now(now_ms, UCN_CLUSTER_REKEY_ACK_TIMEOUT_MS);
    candidate.successor_backup_node_id = staging->successor_backup_node_id;
    if (!ucn_cluster_rekey_transaction_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    result = ucn_cluster_rekey_transaction_begin_collection(
        &candidate, durable_state);
    if (result != UCN_OK) {
        return result;
    }
    *transaction = candidate;
    return UCN_OK;
}

static bool ack_frame_matches_transaction(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_wire_v4_frame_t *frame)
{
    return frame->type == UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK &&
           (frame->role == UCN_CLUSTER_ROLE_MEMBER ||
            frame->role == UCN_CLUSTER_ROLE_BACKUP) &&
           frame->flags == 0U &&
           frame->cluster_id == transaction->predecessor_epoch.cluster_id &&
           frame->term == transaction->predecessor_epoch.term &&
           frame->head_node_id == transaction->predecessor_epoch.head_node_id &&
           frame->words[0U] == transaction->successor_epoch.cluster_id &&
           frame->words[1U] == transaction->successor_epoch.term &&
           frame->words[2U] == transaction->transaction_id &&
           frame->words[3U] == transaction->successor_config.config_id;
}

ucn_result_t ucn_cluster_rekey_ack_frame_admit(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_node_id_t outer_source,
    ucn_cluster_rekey_ack_t *output)
{
    ucn_cluster_rekey_ack_t candidate;
    const ucn_cluster_rekey_voter_profile_t *profile;
    int index;

    if (transaction == NULL || frame == NULL ||
        output == NULL || !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        !ucn_cluster_wire_v4_frame_is_valid(frame) ||
        (transaction->state != UCN_CLUSTER_REKEY_STATE_PREPARED_DURABLE &&
         transaction->state != UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS &&
         transaction->state != UCN_CLUSTER_REKEY_STATE_QUORUM)) {
        return UCN_ERR_ARGUMENT;
    }
    index = transaction_voter_index(transaction, outer_source);
    if (index < 0) {
        return UCN_ERR_ACCESS;
    }
    profile = &transaction->voter_profiles[index];
    if (!node_id_is_valid(outer_source) || outer_source ==
            transaction->predecessor_epoch.head_node_id ||
        profile->node_id != outer_source ||
        profile->wire_format != UCN_CLUSTER_WIRE_V4_FORMAT_VERSION ||
        (profile->capabilities & UCN_CLUSTER_REKEY_REQUIRED_CAPABILITIES) !=
            UCN_CLUSTER_REKEY_REQUIRED_CAPABILITIES ||
        !serial_is_valid(profile->persistence_generation)) {
        return UCN_ERR_ACCESS;
    }
    if (!ack_frame_matches_transaction(transaction, frame) ||
        frame->words[4U] != profile->persistence_generation) {
        return UCN_ERR_REPLAY;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.source_node_id = outer_source;
    candidate.source_role = frame->role;
    candidate.persistence_generation = frame->words[4U];
    candidate.member_nonce = frame->words[5U];
    *output = candidate;
    return UCN_OK;
}

static bool durable_prepared_is_valid(
    const ucn_cluster_persist_state_t *durable,
    const ucn_cluster_rekey_transaction_t *transaction)
{
    return durable != NULL && transaction != NULL &&
           ucn_cluster_persist_state_is_valid(durable) &&
           durable->record_schema_version ==
               UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION &&
           durable->has_active_epoch && durable->has_max_epoch &&
           epoch_is_equal(&durable->active_epoch,
                          &transaction->predecessor_epoch) &&
           epoch_is_equal(&durable->max_epoch,
                          &transaction->predecessor_epoch) &&
           config_ref_is_equal(&durable->committed_config,
                               &transaction->predecessor_config_ref) &&
           durable->config_transaction.phase !=
               UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
           durable->rekey_transaction.phase ==
               UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED &&
           durable->rekey_transaction.transaction_id ==
               transaction->transaction_id &&
           rekey_ref_is_equal(&durable->rekey_transaction.staging_rekey,
                              &transaction->durable_rekey_ref);
}

bool ucn_cluster_rekey_transaction_quorum_reached(
    const ucn_cluster_rekey_transaction_t *transaction)
{
    return ucn_cluster_rekey_transaction_is_valid(transaction) &&
           ucn_cluster_config_bitmap_reaches_quorum(
               &transaction->predecessor_config.old_set,
               transaction->ack_bitmap);
}

ucn_result_t ucn_cluster_rekey_transaction_begin_collection(
    ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state)
{
    ucn_cluster_rekey_transaction_t candidate;
    uint64_t self_bit;

    if (transaction == NULL || durable_state == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED ||
        !durable_prepared_is_valid(durable_state, transaction) ||
        !ucn_cluster_voter_set_bitmap_for_node(
            &transaction->predecessor_config.old_set,
            transaction->predecessor_epoch.head_node_id, &self_bit)) {
        return UCN_ERR_STATE;
    }
    candidate = *transaction;
    candidate.ack_bitmap = self_bit;
    candidate.state = UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS;
    if (ucn_cluster_config_bitmap_reaches_quorum(
            &candidate.predecessor_config.old_set, candidate.ack_bitmap)) {
        candidate.state = UCN_CLUSTER_REKEY_STATE_QUORUM;
    }
    *transaction = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_rekey_transaction_note_ack(
    ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_node_id_t outer_source,
    uint32_t now_ms)
{
    ucn_cluster_rekey_transaction_t candidate;
    ucn_cluster_rekey_ack_t ack;
    uint64_t voter_bit;
    int index;
    ucn_result_t result;

    if (transaction == NULL || durable_state == NULL || frame == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        !durable_prepared_is_valid(durable_state, transaction) ||
        (transaction->state != UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS &&
         transaction->state != UCN_CLUSTER_REKEY_STATE_QUORUM)) {
        return UCN_ERR_STATE;
    }
    if (transaction->state == UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS &&
        ucn_deadline_expired(now_ms, transaction->deadline_ms)) {
        transaction->state = UCN_CLUSTER_REKEY_STATE_ABORTED;
        return UCN_ERR_TTL;
    }
    result = ucn_cluster_rekey_ack_frame_admit(transaction, frame,
                                               outer_source, &ack);
    if (result != UCN_OK) {
        return result;
    }
    index = transaction_voter_index(transaction, outer_source);
    if (index < 0 || !ucn_cluster_voter_set_bitmap_for_node(
                         &transaction->predecessor_config.old_set,
                         outer_source, &voter_bit)) {
        return UCN_ERR_ACCESS;
    }
    if (transaction->ack_nonce_high_water[index] != 0U &&
        ack.member_nonce < transaction->ack_nonce_high_water[index]) {
        return UCN_ERR_REPLAY;
    }
    if (transaction->ack_nonce_high_water[index] == ack.member_nonce &&
        (transaction->ack_bitmap & voter_bit) != 0U) {
        return UCN_OK;
    }
    candidate = *transaction;
    candidate.ack_nonce_high_water[index] = ack.member_nonce;
    candidate.ack_bitmap |= voter_bit;
    if (ucn_cluster_config_bitmap_reaches_quorum(
            &candidate.predecessor_config.old_set, candidate.ack_bitmap)) {
        candidate.state = UCN_CLUSTER_REKEY_STATE_QUORUM;
    }
    *transaction = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_rekey_commit_frame_build(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state,
    ucn_cluster_wire_v4_frame_t *output)
{
    ucn_cluster_wire_v4_frame_t candidate;

    if (transaction == NULL || durable_state == NULL || output == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        (transaction->state != UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE &&
         transaction->state != UCN_CLUSTER_REKEY_STATE_COMMITTED) ||
        !transaction->authority_revoked ||
        !durable_successor_is_valid(durable_state, transaction)) {
        return UCN_ERR_STATE;
    }
    rekey_frame_fill(transaction, UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT,
                     &candidate);
    if (!ucn_cluster_wire_v4_frame_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *output = candidate;
    return UCN_OK;
}

bool ucn_cluster_rekey_transaction_is_valid(
    const ucn_cluster_rekey_transaction_t *transaction)
{
    if (transaction == NULL ||
        transaction->state <
            UCN_CLUSTER_REKEY_STATE_ID_HISTORY_DURABLE_REQUIRED ||
        transaction->state > UCN_CLUSTER_REKEY_STATE_ABORTED ||
        transaction->phase != UCN_CLUSTER_PHASE_HEAD_REKEYING ||
        !epoch_is_valid(&transaction->predecessor_epoch) ||
        !ucn_cluster_config_state_is_valid(
            &transaction->predecessor_config) ||
        transaction->predecessor_config.phase !=
            (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        !transaction->predecessor_config_ref.valid ||
        !epoch_is_valid(&transaction->successor_epoch) ||
        transaction->successor_epoch.cluster_id ==
            transaction->predecessor_epoch.cluster_id ||
        transaction->successor_epoch.term != 1U ||
        transaction->successor_epoch.head_node_id !=
            transaction->predecessor_epoch.head_node_id ||
        !ucn_cluster_config_state_is_valid(&transaction->successor_config) ||
        transaction->successor_config.phase !=
            (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        transaction->successor_config.config_id != 1U ||
        !transaction->successor_config_ref.valid ||
        !transaction->durable_rekey_ref.valid ||
        !serial_is_valid(transaction->durable_rekey_ref.generation) ||
        transaction->durable_rekey_ref.generation !=
            transaction->allocation_history_generation ||
        transaction->allocation_history_fingerprint == 0U ||
        transaction->durable_rekey_ref.allocation_history_fingerprint !=
            transaction->allocation_history_fingerprint ||
        transaction->allocation_identity.purpose !=
            UCN_CLUSTER_ID_PURPOSE_REKEY ||
        transaction->allocation_identity.local_node_id !=
            transaction->predecessor_epoch.head_node_id ||
        transaction->allocation_identity.parent_cluster_id !=
            transaction->predecessor_epoch.cluster_id ||
        transaction->allocation_identity.parent_term !=
            transaction->predecessor_epoch.term ||
        transaction->allocation_identity.parent_config_id !=
            transaction->predecessor_config.config_id ||
        transaction->allocation_identity.incarnation == 0U ||
        !serial_is_valid(transaction->allocation_identity.round) ||
        transaction->allocation_identity.recovery_round != 0U ||
        transaction->durable_rekey_ref.prepare_nonce != transaction->nonce ||
        transaction->durable_rekey_ref.successor_backup_node_id !=
            transaction->successor_backup_node_id ||
        !epoch_is_equal(&transaction->durable_rekey_ref.predecessor_epoch,
                        &transaction->predecessor_epoch) ||
        !config_ref_is_equal(
            &transaction->durable_rekey_ref.predecessor_config,
            &transaction->predecessor_config_ref) ||
        !epoch_is_equal(&transaction->durable_rekey_ref.successor_epoch,
                        &transaction->successor_epoch) ||
        !config_ref_is_equal(
            &transaction->durable_rekey_ref.successor_config,
            &transaction->successor_config_ref) ||
        !voter_profiles_match_config(&transaction->predecessor_config,
                                     transaction->voter_profiles,
                                     transaction->voter_count) ||
        (transaction->successor_backup_node_id != 0U &&
         !ucn_cluster_voter_set_contains(
             &transaction->successor_config.old_set,
             transaction->successor_backup_node_id)) ||
        !serial_is_valid(transaction->transaction_id) ||
        !serial_is_valid(transaction->nonce) ||
        transaction->deadline_ms == 0U) {
        return false;
    }
    return true;
}

ucn_result_t ucn_cluster_rekey_transaction_begin(
    ucn_cluster_rekey_transaction_t *transaction,
    ucn_cluster_authority_runtime_t *authority,
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_rekey_voter_profile_t *voter_profiles,
    size_t voter_profile_count,
    uint32_t transaction_id,
    uint32_t nonce,
    uint32_t now_ms,
    ucn_cluster_id_history_t *allocation_history,
    uint32_t loaded_history_generation)
{
    ucn_cluster_rekey_transaction_t candidate;
    ucn_cluster_persist_config_ref_t config_ref;
    ucn_cluster_persist_config_ref_t successor_config_ref;
    ucn_cluster_t *cluster;
    ucn_cluster_epoch_t epoch;
    ucn_cluster_id_request_t id_request;
    uint32_t successor_cluster_id = 0U;
    uint32_t next_round;
    uint32_t next_incarnation;
    uint32_t next_history_generation;
    ucn_cluster_id_history_t candidate_history;
    ucn_result_t result;

    if (transaction == NULL || authority == NULL || durable_state == NULL ||
        allocation_history == NULL ||
        !storage_is_zero(transaction, sizeof(*transaction)) ||
        !serial_is_valid(transaction_id) || !serial_is_valid(nonce) ||
        (allocation_history->count != 0U &&
         !serial_is_valid(loaded_history_generation))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_cluster_authority_runtime_preflight(authority, now_ms);
    if (result != UCN_OK) {
        return result;
    }
    cluster = authority->cluster;
    if (!authority->initialized || cluster == NULL ||
        cluster->authority_runtime != authority ||
        cluster->persistence_faulted ||
        ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_HEAD ||
        cluster->head_node_id != cluster->config.local_node_id ||
        !cluster->authority_active ||
        cluster->authority_phase != UCN_CLUSTER_PHASE_HEAD_STABLE ||
        !ucn_cluster_config_state_is_valid(&authority->active_config) ||
        authority->active_config.phase !=
            (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        !ucn_cluster_authority_runtime_quorum_met(authority, now_ms)) {
        return UCN_ERR_STATE;
    }
    if (!voter_profiles_match_config(&authority->active_config, voter_profiles,
                                     voter_profile_count)) {
        return UCN_ERR_ACCESS;
    }
    if (ucn_cluster_config_persist_ref_from_state(&authority->active_config,
                                                   &config_ref) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    epoch.cluster_id = cluster->cluster_id;
    epoch.term = cluster->term;
    epoch.head_node_id = cluster->head_node_id;
    if (!epoch_is_valid(&epoch) ||
        !durable_predecessor_is_valid(durable_state, &epoch, &config_ref)) {
        return UCN_ERR_STATE;
    }
    if (durable_state->rekey_transaction.phase ==
            UCN_CLUSTER_PERSIST_TRANSACTION_ABORTED &&
        transaction_id <= durable_state->rekey_transaction.transaction_id) {
        return UCN_ERR_REPLAY;
    }

    /* M13 refuses the compact default mixer: only a product Provider can
     * make a Rekey candidate. The candidate then remains behind the
     * ID_HISTORY_DURABLE_REQUIRED gate until the complete bounded history is
     * committed and reloaded exactly. */
    if (cluster->config.make_cluster_id == NULL ||
        !serial_is_valid(durable_state->boot_incarnation)) {
        return UCN_ERR_CONFIG;
    }
    result = serial_next_checked(cluster->cluster_id_round, &next_round);
    if (result != UCN_OK) {
        return result;
    }
    result = serial_next_checked(durable_state->boot_incarnation,
                                 &next_incarnation);
    if (result != UCN_OK) {
        return result;
    }
    result = serial_next_checked(loaded_history_generation,
                                 &next_history_generation);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(&id_request, 0, sizeof(id_request));
    id_request.purpose = UCN_CLUSTER_ID_PURPOSE_REKEY;
    id_request.local_node_id = cluster->config.local_node_id;
    id_request.parent_cluster_id = epoch.cluster_id;
    id_request.parent_term = epoch.term;
    id_request.parent_config_id = authority->active_config.config_id;
    id_request.incarnation = durable_state->boot_incarnation;
    id_request.round = next_round;
    result = cluster->config.make_cluster_id(cluster->config.cluster_id_context,
                                             &id_request,
                                             &successor_cluster_id);
    if (result != UCN_OK) {
        return result;
    }
    if (successor_cluster_id == 0U ||
        successor_cluster_id == UCN_NODE_BROADCAST ||
        successor_cluster_id == epoch.cluster_id) {
        return UCN_ERR_CONFIG;
    }

    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.state =
        UCN_CLUSTER_REKEY_STATE_ID_HISTORY_DURABLE_REQUIRED;
    candidate.phase = UCN_CLUSTER_PHASE_HEAD_REKEYING;
    candidate.predecessor_epoch = epoch;
    candidate.predecessor_config = authority->active_config;
    candidate.predecessor_config_ref = config_ref;
    candidate.successor_epoch.cluster_id = successor_cluster_id;
    candidate.successor_epoch.term = 1U;
    candidate.successor_epoch.head_node_id = epoch.head_node_id;
    if (!ucn_cluster_config_state_init_stable(
            &candidate.successor_config, 1U,
            authority->active_config.old_set.node_ids,
            authority->active_config.old_set.count) ||
        ucn_cluster_config_persist_ref_from_state(
            &candidate.successor_config, &successor_config_ref) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    candidate.successor_config_ref = successor_config_ref;
    candidate.durable_rekey_ref.valid = true;
    candidate.durable_rekey_ref.generation = next_history_generation;
    candidate.durable_rekey_ref.next_incarnation = next_incarnation;
    candidate.durable_rekey_ref.prepare_nonce = nonce;
    candidate.durable_rekey_ref.predecessor_epoch = epoch;
    candidate.durable_rekey_ref.predecessor_config = config_ref;
    candidate.durable_rekey_ref.successor_epoch = candidate.successor_epoch;
    candidate.durable_rekey_ref.successor_config = successor_config_ref;
    (void)memcpy(candidate.voter_profiles, voter_profiles,
                 voter_profile_count * sizeof(voter_profiles[0U]));
    candidate.voter_count = (uint8_t)voter_profile_count;
    candidate.transaction_id = transaction_id;
    candidate.nonce = nonce;
    candidate.allocation_identity = id_request;
    candidate.allocation_history_generation = next_history_generation;
    candidate.started_ms = now_ms;
    candidate.deadline_ms =
        ucn_deadline_from_now(now_ms, UCN_CLUSTER_REKEY_ACK_TIMEOUT_MS);
    if (cluster->phase == UCN_CLUSTER_PHASE_HEAD_STABLE &&
        ucn_cluster_voter_set_contains(&candidate.successor_config.old_set,
                                       cluster->backup_node_id)) {
        candidate.successor_backup_node_id = cluster->backup_node_id;
    }
    candidate.durable_rekey_ref.successor_backup_node_id =
        candidate.successor_backup_node_id;
    candidate_history = *allocation_history;
    result = ucn_cluster_id_history_admit(&candidate_history, &id_request,
                                          successor_cluster_id);
    if (result != UCN_OK) {
        if (result == UCN_ERR_REPLAY) {
            /* The candidate is a valid ID but belongs to another durable
             * allocation identity. Consume this allocation round so the next
             * product call must derive a different identity/candidate. */
            cluster->cluster_id_round = next_round;
        }
        return result;
    }
    candidate.allocation_history_fingerprint =
        ucn_cluster_id_history_fingerprint(&candidate_history,
                                            next_history_generation);
    candidate.durable_rekey_ref.allocation_history_fingerprint =
        candidate.allocation_history_fingerprint;
    if (!ucn_cluster_rekey_transaction_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    cluster->cluster_id_round = next_round;
    *allocation_history = candidate_history;
    *transaction = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_rekey_transaction_confirm_id_history_durable(
    ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_id_history_t *durable_history,
    uint32_t durable_generation)
{
    const ucn_cluster_id_history_entry_t *entry;
    uint32_t fingerprint;

    if (transaction == NULL || durable_history == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        transaction->state !=
            UCN_CLUSTER_REKEY_STATE_ID_HISTORY_DURABLE_REQUIRED ||
        durable_generation != transaction->allocation_history_generation) {
        return UCN_ERR_STATE;
    }
    fingerprint = ucn_cluster_id_history_fingerprint(durable_history,
                                                       durable_generation);
    entry = history_find_cluster_id(durable_history,
                                    transaction->successor_epoch.cluster_id);
    if (fingerprint == 0U ||
        fingerprint != transaction->allocation_history_fingerprint ||
        entry == NULL ||
        !id_request_is_equal(&entry->identity,
                             &transaction->allocation_identity)) {
        return UCN_ERR_STATE;
    }
    transaction->state = UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED;
    return UCN_OK;
}

ucn_result_t ucn_cluster_rekey_transaction_step(
    ucn_cluster_rekey_transaction_t *transaction, uint32_t now_ms)
{
    if (transaction == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction)) {
        return UCN_ERR_ARGUMENT;
    }
    if (transaction->state == UCN_CLUSTER_REKEY_STATE_ABORTED ||
        transaction->state == UCN_CLUSTER_REKEY_STATE_FENCED ||
        transaction->state == UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE ||
        transaction->state == UCN_CLUSTER_REKEY_STATE_COMMITTED) {
        return UCN_OK;
    }
    if (transaction->state ==
            UCN_CLUSTER_REKEY_STATE_ID_HISTORY_DURABLE_REQUIRED ||
        transaction->state == UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED ||
        transaction->state == UCN_CLUSTER_REKEY_STATE_PREPARED_DURABLE ||
        transaction->state == UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS) {
        if (ucn_deadline_expired(now_ms, transaction->deadline_ms)) {
            transaction->state = UCN_CLUSTER_REKEY_STATE_ABORTED;
            return UCN_ERR_TTL;
        }
        return UCN_OK;
    }
    /* Provider-owned PENDING and quorum/commit phases cannot be timed out by
     * this pure transaction helper; their persistence owner must resolve or
     * fence them. */
    return UCN_ERR_STATE;
}

ucn_result_t ucn_cluster_rekey_tombstone_admit_frame(
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_wire_v4_frame_t *frame)
{
    if (durable_state == NULL || frame == NULL ||
        !ucn_cluster_persist_state_is_valid(durable_state) ||
        !ucn_cluster_wire_v4_frame_is_valid(frame)) {
        return UCN_ERR_ARGUMENT;
    }
    if (durable_state->tombstone.valid &&
        frame->cluster_id ==
            durable_state->tombstone.retired_epoch.cluster_id) {
        return UCN_ERR_REPLAY;
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_rekey_successor_materialize(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state,
    ucn_cluster_rekey_successor_state_t *output)
{
    ucn_cluster_rekey_successor_state_t candidate;
    size_t member_index = 0U;
    uint8_t voter_index;

    if (transaction == NULL || durable_state == NULL || output == NULL ||
        !ucn_cluster_rekey_transaction_is_valid(transaction) ||
        (transaction->state != UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE &&
         transaction->state != UCN_CLUSTER_REKEY_STATE_COMMITTED) ||
        !transaction->authority_revoked ||
        !durable_successor_is_valid(durable_state, transaction)) {
        return UCN_ERR_STATE;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.epoch = transaction->successor_epoch;
    candidate.config = transaction->successor_config;
    candidate.config_ref = transaction->successor_config_ref;
    candidate.voters = transaction->successor_config.old_set;
    candidate.backup_node_id = transaction->successor_backup_node_id;
    if (candidate.backup_node_id != 0U) {
        candidate.backup_generation = 1U;
        candidate.membership_sequence = 1U;
        candidate.snapshot_generation = 1U;
        candidate.snapshot_id = 1U;
    }
    for (voter_index = 0U; voter_index < transaction->voter_count;
         ++voter_index) {
        const ucn_cluster_rekey_voter_profile_t *profile =
            &transaction->voter_profiles[voter_index];
        ucn_cluster_member_t *member;

        if (profile->node_id == transaction->successor_epoch.head_node_id) {
            continue;
        }
        if (member_index >= UCN_CLUSTER_MAX_MEMBERS) {
            return UCN_ERR_NO_SPACE;
        }
        member = &candidate.members.slots[member_index++];
        member->occupied = true;
        member->voting = true;
        member->status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
        member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
        member->capabilities = profile->capabilities;
        member->node_id = profile->node_id;
        member->last_nonce =
            transaction->ack_nonce_high_water[voter_index];
    }
    if (!ucn_cluster_config_state_is_valid(&candidate.config) ||
        !config_ref_is_equal(&candidate.config_ref,
                             &transaction->successor_config_ref) ||
        !ucn_cluster_voter_set_is_valid(&candidate.voters) ||
        !ucn_cluster_member_table_is_valid(&candidate.members)) {
        return UCN_ERR_STATE;
    }
    *output = candidate;
    return UCN_OK;
}
