#include "ucn/ucn_cluster_handover.h"

#include <string.h>

#include "ucn/ucn_time.h"

#define UCN_CLUSTER_HANDOVER_AUTHORITY_REENTRY_FENCE UINT32_C(0x55434E46)

static bool transaction_authority_is_fenced(
    const ucn_cluster_handover_transaction_t *transaction);

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool cluster_id_is_valid(uint32_t cluster_id)
{
    return cluster_id != 0U && cluster_id != UCN_NODE_BROADCAST;
}

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool nonce_is_valid(uint32_t value)
{
    return value != 0U;
}

static bool epoch_is_valid(const ucn_cluster_epoch_t *epoch)
{
    return epoch != NULL && cluster_id_is_valid(epoch->cluster_id) &&
           serial_is_valid(epoch->term) && node_id_is_valid(epoch->head_node_id);
}

static bool epoch_is_exact(const ucn_cluster_epoch_t *left,
                           const ucn_cluster_epoch_t *right)
{
    return epoch_is_valid(left) && epoch_is_valid(right) &&
           left->cluster_id == right->cluster_id && left->term == right->term &&
           left->head_node_id == right->head_node_id;
}

static bool offer_is_valid(const ucn_cluster_handover_offer_t *offer)
{
    return offer != NULL && epoch_is_valid(&offer->epoch) &&
           serial_is_valid(offer->config_id) && offer->config_hash != 0U &&
           nonce_is_valid(offer->nonce);
}

static bool policy_is_valid(const ucn_cluster_handover_policy_t *policy)
{
    return policy != NULL && policy->required_samples != 0U &&
           ucn_duration_is_valid(policy->head_min_tenure_ms) &&
           ucn_duration_is_valid(policy->merge_hold_down_ms) &&
           ucn_duration_is_valid(policy->retry_interval_ms) &&
           ucn_duration_is_valid(policy->transaction_timeout_ms);
}

static bool mode_is_valid(uint8_t mode)
{
    return mode == (uint8_t)UCN_CLUSTER_HANDOVER_MODE_FOREIGN_MERGE ||
           mode == (uint8_t)UCN_CLUSTER_HANDOVER_MODE_SAME_CLUSTER_PLANNED;
}

static void trace_append(ucn_cluster_handover_transaction_t *transaction,
                         ucn_cluster_handover_trace_event_t event)
{
    if (transaction != NULL && transaction->trace_count <
                                   UCN_CLUSTER_HANDOVER_TRACE_CAPACITY) {
        transaction->trace[transaction->trace_count++] = event;
    }
}

static bool transaction_identity_matches(
    const ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_message_t *message)
{
    return transaction != NULL && message != NULL && transaction->active &&
           transaction->mode == message->mode &&
           transaction->transaction_id == message->transaction_id &&
           transaction->target_config_id == message->target_config_id &&
           transaction->target_config_hash == message->target_config_hash &&
           epoch_is_exact(&transaction->old_epoch, &message->old_epoch) &&
           epoch_is_exact(&transaction->target_epoch, &message->target_epoch);
}

static void message_from_transaction(
    const ucn_cluster_handover_transaction_t *transaction,
    uint8_t type,
    uint8_t sender_role,
    ucn_node_id_t source_node_id,
    ucn_cluster_handover_message_t *output)
{
    (void)memset(output, 0, sizeof(*output));
    output->type = type;
    output->sender_role = sender_role;
    output->mode = transaction->mode;
    output->source_node_id = source_node_id;
    output->old_epoch = transaction->old_epoch;
    output->target_epoch = transaction->target_epoch;
    output->transaction_id = transaction->transaction_id;
    if (type == UCN_CLUSTER_HANDOVER_MESSAGE_PREPARE ||
        type == UCN_CLUSTER_HANDOVER_MESSAGE_READY ||
        type == UCN_CLUSTER_HANDOVER_MESSAGE_COMMIT) {
        output->target_config_id = transaction->target_config_id;
        output->target_config_hash = transaction->target_config_hash;
    }
    if (type == UCN_CLUSTER_HANDOVER_MESSAGE_STEPDOWN ||
        type == UCN_CLUSTER_HANDOVER_MESSAGE_WITHDRAW) {
        output->stepdown_nonce = transaction->stepdown_nonce;
    }
}

void ucn_cluster_handover_transaction_reset(
    ucn_cluster_handover_transaction_t *transaction)
{
    /* Treat every nonzero fence value as sealed.  An invalid public value must
     * fail closed rather than be cleared into a new Authority-capable object. */
    if (transaction != NULL && transaction->authority_reentry_fence == 0U) {
        (void)memset(transaction, 0, sizeof(*transaction));
    }
}

ucn_cluster_handover_offer_class_t ucn_cluster_handover_offer_classify(
    const ucn_cluster_epoch_t *local_epoch,
    const ucn_cluster_handover_offer_t *offer)
{
    if (!epoch_is_valid(local_epoch) || !offer_is_valid(offer)) {
        return UCN_CLUSTER_HANDOVER_OFFER_INVALID;
    }
    /* Deliberately do not call the Epoch comparator for FOREIGN.  This makes
     * the no-cross-Cluster-Term rule evident at the public model boundary. */
    return local_epoch->cluster_id == offer->epoch.cluster_id ?
               UCN_CLUSTER_HANDOVER_OFFER_SAME_CLUSTER_AUTHORITY :
               UCN_CLUSTER_HANDOVER_OFFER_FOREIGN_MERGE;
}

void ucn_cluster_handover_candidate_table_reset(
    ucn_cluster_handover_candidate_table_t *table)
{
    if (table != NULL) {
        (void)memset(table, 0, sizeof(*table));
    }
}

void ucn_cluster_handover_candidate_expire(
    ucn_cluster_handover_candidate_table_t *table,
    uint32_t now_ms,
    uint32_t expiry_ms)
{
    size_t index;

    if (table == NULL || !ucn_duration_is_valid(expiry_ms)) {
        return;
    }
    for (index = 0U; index < UCN_CLUSTER_HANDOVER_MAX_CANDIDATES; ++index) {
        if (table->slots[index].occupied &&
            ucn_elapsed_at_least(now_ms, table->slots[index].last_seen_ms,
                                 expiry_ms)) {
            (void)memset(&table->slots[index], 0, sizeof(table->slots[index]));
        }
    }
}

ucn_result_t ucn_cluster_handover_candidate_observe(
    ucn_cluster_handover_candidate_table_t *table,
    const ucn_cluster_epoch_t *local_epoch,
    const ucn_cluster_handover_offer_t *offer,
    const ucn_cluster_handover_policy_t *policy,
    uint32_t now_ms,
    ucn_cluster_handover_candidate_t **output)
{
    size_t index;
    ucn_cluster_handover_candidate_t *free_slot = NULL;

    if (output != NULL) {
        *output = NULL;
    }
    if (table == NULL || !policy_is_valid(policy) ||
        ucn_cluster_handover_offer_classify(local_epoch, offer) !=
            UCN_CLUSTER_HANDOVER_OFFER_FOREIGN_MERGE) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_CLUSTER_HANDOVER_MAX_CANDIDATES; ++index) {
        ucn_cluster_handover_candidate_t *candidate = &table->slots[index];

        if (!candidate->occupied) {
            if (free_slot == NULL) {
                free_slot = candidate;
            }
            continue;
        }
        if (candidate->offer.epoch.cluster_id == offer->epoch.cluster_id &&
            candidate->offer.epoch.head_node_id == offer->epoch.head_node_id) {
            if (offer->nonce <= candidate->offer.nonce) {
                return UCN_ERR_REPLAY;
            }
            candidate->offer = *offer;
            candidate->last_seen_ms = now_ms;
            if (candidate->score_samples < UINT8_MAX) {
                ++candidate->score_samples;
            }
            if (output != NULL) {
                *output = candidate;
            }
            return UCN_OK;
        }
    }
    if (free_slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->offer = *offer;
    free_slot->score_samples = 1U;
    free_slot->first_seen_ms = now_ms;
    free_slot->last_seen_ms = now_ms;
    if (output != NULL) {
        *output = free_slot;
    }
    return UCN_OK;
}

bool ucn_cluster_handover_candidate_is_eligible(
    const ucn_cluster_handover_candidate_t *candidate,
    uint16_t local_head_score,
    uint32_t local_head_since_ms,
    const ucn_cluster_handover_policy_t *policy,
    uint32_t now_ms)
{
    uint32_t required;

    if (candidate == NULL || !candidate->occupied || !policy_is_valid(policy) ||
        candidate->score_samples < policy->required_samples ||
        !ucn_elapsed_at_least(now_ms, local_head_since_ms,
                              policy->head_min_tenure_ms) ||
        (candidate->hold_down_until_ms != 0U &&
         !ucn_deadline_expired(now_ms, candidate->hold_down_until_ms))) {
        return false;
    }
    required = (uint32_t)local_head_score *
               ((uint32_t)100U + policy->improvement_percent);
    return (uint32_t)candidate->offer.head_score * UINT32_C(100) >= required;
}

void ucn_cluster_handover_candidate_note_result(
    ucn_cluster_handover_candidate_t *candidate,
    bool handover_started,
    const ucn_cluster_handover_policy_t *policy,
    uint32_t now_ms)
{
    if (candidate == NULL || !candidate->occupied || !policy_is_valid(policy)) {
        return;
    }
    candidate->score_samples = 0U;
    if (handover_started) {
        candidate->hold_down_until_ms =
            ucn_deadline_from_now(now_ms, policy->merge_hold_down_ms);
    }
}

ucn_result_t ucn_cluster_handover_feasibility_evaluate(
    const ucn_cluster_handover_offer_t *target,
    uint16_t losing_cluster_size,
    const ucn_cluster_handover_policy_t *policy,
    ucn_cluster_handover_feasibility_t *output)
{
    ucn_cluster_handover_feasibility_t result;

    if (output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&result, 0, sizeof(result));
    result.reason = UCN_CLUSTER_HANDOVER_FEASIBILITY_ARGUMENT;
    if (!offer_is_valid(target) || !policy_is_valid(policy) ||
        losing_cluster_size == 0U) {
        *output = result;
        return UCN_ERR_ARGUMENT;
    }
    if (target->available_capacity < losing_cluster_size) {
        result.reason = UCN_CLUSTER_HANDOVER_FEASIBILITY_CAPACITY;
    } else if (target->wire_format != UCN_CLUSTER_HANDOVER_WIRE_FORMAT_V4) {
        result.reason = UCN_CLUSTER_HANDOVER_FEASIBILITY_WIRE;
    } else if ((target->capabilities & policy->required_capabilities) !=
               policy->required_capabilities) {
        result.reason = UCN_CLUSTER_HANDOVER_FEASIBILITY_CAPABILITIES;
    } else if (target->config_id == 0U || target->config_hash == 0U) {
        result.reason = UCN_CLUSTER_HANDOVER_FEASIBILITY_CONFIG;
    } else if (!target->backup_policy_compatible) {
        result.reason = UCN_CLUSTER_HANDOVER_FEASIBILITY_BACKUP_POLICY;
    } else {
        result.admitted = true;
        result.reason = UCN_CLUSTER_HANDOVER_FEASIBILITY_OK;
    }
    *output = result;
    return UCN_OK;
}

static bool target_epoch_is_valid_for_mode(const ucn_cluster_epoch_t *old_epoch,
                                           const ucn_cluster_epoch_t *target_epoch,
                                           uint8_t mode)
{
    if (!epoch_is_valid(old_epoch) || !epoch_is_valid(target_epoch) ||
        !mode_is_valid(mode) || old_epoch->head_node_id == target_epoch->head_node_id) {
        return false;
    }
    if (mode == (uint8_t)UCN_CLUSTER_HANDOVER_MODE_FOREIGN_MERGE) {
        return old_epoch->cluster_id != target_epoch->cluster_id;
    }
    return old_epoch->cluster_id == target_epoch->cluster_id &&
           old_epoch->term < UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD &&
           target_epoch->term == old_epoch->term + 1U;
}

static bool transaction_state_is_valid(uint8_t state)
{
    return state >= (uint8_t)UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT &&
           state <= (uint8_t)UCN_CLUSTER_HANDOVER_STATE_ABORTED;
}

static bool trace_event_is_valid(ucn_cluster_handover_trace_event_t event)
{
    return event >= UCN_CLUSTER_HANDOVER_TRACE_READY_VERIFIED &&
           event <= UCN_CLUSTER_HANDOVER_TRACE_ABORTED;
}

static bool transaction_is_reset(
    const ucn_cluster_handover_transaction_t *transaction)
{
    static const ucn_cluster_handover_transaction_t zero_transaction = {0};

    return transaction != NULL &&
           memcmp(transaction, &zero_transaction, sizeof(*transaction)) == 0;
}

static bool transaction_authority_is_fenced(
    const ucn_cluster_handover_transaction_t *transaction)
{
    return transaction != NULL &&
           transaction->authority_reentry_fence ==
               UCN_CLUSTER_HANDOVER_AUTHORITY_REENTRY_FENCE;
}

/* All externally reachable transaction helpers start from this validator.
 * The model is caller-owned, so it must reject a corrupted public value before
 * it can inspect trace[] or create a message from stale state. */
static bool transaction_is_valid(
    const ucn_cluster_handover_transaction_t *transaction)
{
    size_t index;
    bool old_config_required;
    bool nonce_required;
    bool authority_fence_required;

    if (transaction == NULL || !transaction->active ||
        !transaction_state_is_valid(transaction->state) ||
        !mode_is_valid(transaction->mode) ||
        transaction->trace_count > UCN_CLUSTER_HANDOVER_TRACE_CAPACITY ||
        !target_epoch_is_valid_for_mode(&transaction->old_epoch,
                                        &transaction->target_epoch,
                                        transaction->mode) ||
        !serial_is_valid(transaction->target_config_id) ||
        transaction->target_config_hash == 0U ||
        !serial_is_valid(transaction->transaction_id) ||
        transaction->retry_deadline_ms == 0U || transaction->deadline_ms == 0U) {
        return false;
    }
    if (transaction->authority_reentry_fence != 0U &&
        !transaction_authority_is_fenced(transaction)) {
        return false;
    }
    for (index = 0U; index < transaction->trace_count; ++index) {
        if (!trace_event_is_valid(transaction->trace[index])) {
            return false;
        }
    }

    old_config_required =
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT ||
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_READY_VERIFIED ||
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_AUTHORITY_REVOKED ||
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_STEPDOWN_SENT ||
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_COMMIT_SENT;
    if (old_config_required) {
        if (!serial_is_valid(transaction->old_config_id) ||
            transaction->old_config_hash == 0U) {
            return false;
        }
    } else if (transaction->state == UCN_CLUSTER_HANDOVER_STATE_READY_SENT ||
               transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED ||
               transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_DURABLE) {
        /* Type 26 carries target Config only.  Target-side state must not
         * invent an old Config identity it cannot prove from the wire. */
        if (transaction->old_config_id != 0U || transaction->old_config_hash != 0U) {
            return false;
        }
    }

    nonce_required =
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_AUTHORITY_REVOKED ||
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_STEPDOWN_SENT ||
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_COMMIT_SENT;
    if (nonce_required) {
        if (!nonce_is_valid(transaction->stepdown_nonce)) {
            return false;
        }
    } else if (transaction->state != UCN_CLUSTER_HANDOVER_STATE_ABORTED &&
               transaction->stepdown_nonce != 0U) {
        return false;
    }

    /* The old Head seals itself before Type 9/29.  A later public reset must
     * not erase this fact.  Target-side ABORTED state has no Stepdown nonce
     * and therefore deliberately remains unfenced. */
    authority_fence_required = nonce_required ||
        (transaction->state == UCN_CLUSTER_HANDOVER_STATE_ABORTED &&
         nonce_is_valid(transaction->stepdown_nonce));
    if (authority_fence_required != transaction_authority_is_fenced(transaction)) {
        return false;
    }

    if ((transaction->state == UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_READY_VERIFIED) &&
        !transaction->local_authority_active) {
        return false;
    }
    if ((transaction->state == UCN_CLUSTER_HANDOVER_STATE_READY_SENT ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_DURABLE ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_AUTHORITY_REVOKED ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_STEPDOWN_SENT ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_COMMIT_SENT) &&
        transaction->local_authority_active) {
        return false;
    }
    return transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_DURABLE ?
               transaction->target_epoch_durable :
               !transaction->target_epoch_durable;
}

ucn_result_t ucn_cluster_handover_transaction_begin(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_epoch_t *old_epoch,
    const ucn_cluster_handover_offer_t *target,
    const ucn_cluster_handover_policy_t *policy,
    uint16_t losing_cluster_size,
    uint32_t old_config_id,
    uint32_t old_config_hash,
    uint32_t transaction_id,
    uint32_t now_ms)
{
    ucn_cluster_handover_feasibility_t feasibility;
    uint8_t mode;
    uint32_t retry_deadline_ms;
    uint32_t deadline_ms;

    if (transaction == NULL || !epoch_is_valid(old_epoch) || !offer_is_valid(target) ||
        !policy_is_valid(policy) || losing_cluster_size == 0U ||
        !serial_is_valid(old_config_id) || old_config_hash == 0U ||
        !serial_is_valid(transaction_id) ||
        ucn_cluster_handover_feasibility_evaluate(target, losing_cluster_size,
                                                   policy, &feasibility) != UCN_OK ||
        !feasibility.admitted) {
        return UCN_ERR_ARGUMENT;
    }
    /* A caller must explicitly reset before starting a new transaction.  In
     * particular, never erase an AUTHORITY_REVOKED/STEPDOWN/COMMIT trace and
     * recreate local_authority_active through a second begin(). */
    if (!transaction_is_reset(transaction)) {
        return UCN_ERR_STATE;
    }
    mode = old_epoch->cluster_id == target->epoch.cluster_id ?
               (uint8_t)UCN_CLUSTER_HANDOVER_MODE_SAME_CLUSTER_PLANNED :
               (uint8_t)UCN_CLUSTER_HANDOVER_MODE_FOREIGN_MERGE;
    if (!target_epoch_is_valid_for_mode(old_epoch, &target->epoch, mode)) {
        return UCN_ERR_STATE;
    }
    if (mode == (uint8_t)UCN_CLUSTER_HANDOVER_MODE_SAME_CLUSTER_PLANNED &&
        (old_config_id != target->config_id || old_config_hash != target->config_hash)) {
        return UCN_ERR_STATE;
    }
    retry_deadline_ms = ucn_deadline_from_now(now_ms, policy->retry_interval_ms);
    deadline_ms = ucn_deadline_from_now(now_ms, policy->transaction_timeout_ms);
    if (retry_deadline_ms == 0U || deadline_ms == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    ucn_cluster_handover_transaction_reset(transaction);
    transaction->active = true;
    transaction->local_authority_active = true;
    transaction->mode = mode;
    transaction->state = UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT;
    transaction->old_epoch = *old_epoch;
    transaction->target_epoch = target->epoch;
    transaction->old_config_id = old_config_id;
    transaction->old_config_hash = old_config_hash;
    transaction->target_config_id = target->config_id;
    transaction->target_config_hash = target->config_hash;
    transaction->transaction_id = transaction_id;
    transaction->retry_deadline_ms = retry_deadline_ms;
    transaction->deadline_ms = deadline_ms;
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_build_prepare(
    const ucn_cluster_handover_transaction_t *transaction,
    ucn_cluster_handover_message_t *output)
{
    if (transaction == NULL || output == NULL || !transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT) {
        return UCN_ERR_STATE;
    }
    message_from_transaction(transaction, UCN_CLUSTER_HANDOVER_MESSAGE_PREPARE,
                             UCN_CLUSTER_HANDOVER_ROLE_HEAD,
                             transaction->old_epoch.head_node_id, output);
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_accept_prepare(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_receiver_context_t *receiver,
    const ucn_cluster_handover_message_t *prepare,
    const ucn_cluster_handover_policy_t *policy,
    uint16_t losing_cluster_size,
    uint32_t now_ms,
    ucn_cluster_handover_message_t *ready_output)
{
    ucn_cluster_handover_offer_t target;
    ucn_cluster_handover_feasibility_t feasibility;
    uint8_t required_role;
    uint32_t retry_deadline_ms;
    uint32_t deadline_ms;

    if (transaction == NULL || receiver == NULL || prepare == NULL ||
        ready_output == NULL || !policy_is_valid(policy) || losing_cluster_size == 0U ||
        (transaction->active ? !transaction_is_valid(transaction) :
                               !transaction_is_reset(transaction)) ||
        prepare->type != UCN_CLUSTER_HANDOVER_MESSAGE_PREPARE ||
        prepare->sender_role != UCN_CLUSTER_HANDOVER_ROLE_HEAD ||
        !node_id_is_valid(prepare->source_node_id) ||
        !serial_is_valid(prepare->transaction_id) ||
        !serial_is_valid(prepare->target_config_id) || prepare->target_config_hash == 0U ||
        prepare->stepdown_nonce != 0U || !epoch_is_valid(&receiver->local_epoch) ||
        !epoch_is_valid(&receiver->expected_target_epoch) ||
        !serial_is_valid(receiver->active_config_id) ||
        receiver->active_config_hash == 0U || !target_epoch_is_valid_for_mode(
            &prepare->old_epoch, &prepare->target_epoch, prepare->mode) ||
        prepare->source_node_id != prepare->old_epoch.head_node_id ||
        !epoch_is_exact(&receiver->expected_target_epoch, &prepare->target_epoch) ||
        receiver->active_config_id != prepare->target_config_id ||
        receiver->active_config_hash != prepare->target_config_hash) {
        return UCN_ERR_ARGUMENT;
    }
    if ((prepare->mode == UCN_CLUSTER_HANDOVER_MODE_FOREIGN_MERGE &&
         !epoch_is_exact(&receiver->local_epoch, &prepare->target_epoch)) ||
        (prepare->mode == UCN_CLUSTER_HANDOVER_MODE_SAME_CLUSTER_PLANNED &&
         !epoch_is_exact(&receiver->local_epoch, &prepare->old_epoch))) {
        return UCN_ERR_STATE;
    }
    required_role = prepare->mode == UCN_CLUSTER_HANDOVER_MODE_FOREIGN_MERGE ?
                        UCN_CLUSTER_HANDOVER_ROLE_HEAD :
                        UCN_CLUSTER_HANDOVER_ROLE_BACKUP;
    if (receiver->local_role != required_role ||
        (required_role == UCN_CLUSTER_HANDOVER_ROLE_BACKUP &&
         !receiver->confirmed_backup)) {
        return UCN_ERR_ACCESS;
    }
    if (transaction->active) {
        if (!transaction_identity_matches(transaction, prepare) ||
            transaction->state != UCN_CLUSTER_HANDOVER_STATE_READY_SENT) {
            return UCN_ERR_REPLAY;
        }
        message_from_transaction(transaction, UCN_CLUSTER_HANDOVER_MESSAGE_READY,
                                 required_role, transaction->target_epoch.head_node_id,
                                 ready_output);
        return UCN_OK;
    }
    (void)memset(&target, 0, sizeof(target));
    target.epoch = receiver->expected_target_epoch;
    target.config_id = receiver->active_config_id;
    target.config_hash = receiver->active_config_hash;
    /* Type 26 has no nonce.  This local feasibility value only needs a
     * syntactically valid offer identity; it must never create a wire
     * binding that RFC4 does not carry. */
    target.nonce = 1U;
    target.cluster_size = 1U;
    target.available_capacity = receiver->available_capacity;
    target.capabilities = receiver->capabilities;
    target.wire_format = receiver->wire_format;
    target.backup_policy_compatible = receiver->backup_policy_compatible;
    if (ucn_cluster_handover_feasibility_evaluate(&target, losing_cluster_size, policy,
                                                  &feasibility) != UCN_OK ||
        !feasibility.admitted) {
        return UCN_ERR_ACCESS;
    }
    retry_deadline_ms = ucn_deadline_from_now(now_ms, policy->retry_interval_ms);
    deadline_ms = ucn_deadline_from_now(now_ms, policy->transaction_timeout_ms);
    if (retry_deadline_ms == 0U || deadline_ms == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    ucn_cluster_handover_transaction_reset(transaction);
    transaction->active = true;
    transaction->mode = prepare->mode;
    transaction->state = UCN_CLUSTER_HANDOVER_STATE_READY_SENT;
    transaction->old_epoch = prepare->old_epoch;
    transaction->target_epoch = prepare->target_epoch;
    transaction->target_config_id = prepare->target_config_id;
    transaction->target_config_hash = prepare->target_config_hash;
    transaction->transaction_id = prepare->transaction_id;
    transaction->retry_deadline_ms = retry_deadline_ms;
    transaction->deadline_ms = deadline_ms;
    message_from_transaction(transaction, UCN_CLUSTER_HANDOVER_MESSAGE_READY,
                             required_role, transaction->target_epoch.head_node_id,
                             ready_output);
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_accept_ready(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_message_t *ready)
{
    uint8_t expected_role;

    if (transaction == NULL || ready == NULL || !transaction_is_valid(transaction) ||
        ready->type != UCN_CLUSTER_HANDOVER_MESSAGE_READY ||
        ready->stepdown_nonce != 0U ||
        !transaction_identity_matches(transaction, ready) ||
        ready->source_node_id != transaction->target_epoch.head_node_id) {
        return UCN_ERR_ARGUMENT;
    }
    expected_role = transaction->mode == UCN_CLUSTER_HANDOVER_MODE_FOREIGN_MERGE ?
                        UCN_CLUSTER_HANDOVER_ROLE_HEAD :
                        UCN_CLUSTER_HANDOVER_ROLE_BACKUP;
    if (ready->sender_role != expected_role) {
        return UCN_ERR_ACCESS;
    }
    if (transaction->state == UCN_CLUSTER_HANDOVER_STATE_READY_VERIFIED) {
        return UCN_OK;
    }
    if (transaction->state != UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT) {
        return UCN_ERR_STATE;
    }
    transaction->state = UCN_CLUSTER_HANDOVER_STATE_READY_VERIFIED;
    trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_READY_VERIFIED);
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_revoke_authority(
    ucn_cluster_handover_transaction_t *transaction,
    uint32_t stepdown_nonce)
{
    if (transaction == NULL || !transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_HANDOVER_STATE_READY_VERIFIED ||
        !nonce_is_valid(stepdown_nonce)) {
        return UCN_ERR_STATE;
    }
    transaction->local_authority_active = false;
    transaction->authority_reentry_fence =
        UCN_CLUSTER_HANDOVER_AUTHORITY_REENTRY_FENCE;
    transaction->stepdown_nonce = stepdown_nonce;
    transaction->state = UCN_CLUSTER_HANDOVER_STATE_AUTHORITY_REVOKED;
    trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_AUTHORITY_REVOKED);
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_build_stepdown(
    ucn_cluster_handover_transaction_t *transaction,
    ucn_cluster_handover_message_t *output)
{
    if (transaction == NULL || output == NULL || !transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_HANDOVER_STATE_AUTHORITY_REVOKED ||
        transaction->local_authority_active) {
        return UCN_ERR_STATE;
    }
    message_from_transaction(transaction, UCN_CLUSTER_HANDOVER_MESSAGE_STEPDOWN,
                             UCN_CLUSTER_HANDOVER_ROLE_HEAD,
                             transaction->old_epoch.head_node_id, output);
    transaction->state = UCN_CLUSTER_HANDOVER_STATE_STEPDOWN_SENT;
    trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_STEPDOWN_EMITTED);
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_build_commit(
    ucn_cluster_handover_transaction_t *transaction,
    ucn_cluster_handover_message_t *output)
{
    if (transaction == NULL || output == NULL || !transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_HANDOVER_STATE_STEPDOWN_SENT ||
        transaction->local_authority_active) {
        return UCN_ERR_STATE;
    }
    message_from_transaction(transaction, UCN_CLUSTER_HANDOVER_MESSAGE_COMMIT,
                             UCN_CLUSTER_HANDOVER_ROLE_HEAD,
                             transaction->old_epoch.head_node_id, output);
    transaction->state = UCN_CLUSTER_HANDOVER_STATE_COMMIT_SENT;
    trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_COMMIT_EMITTED);
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_accept_commit(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_message_t *commit,
    uint32_t now_ms)
{
    if (transaction == NULL || commit == NULL || !transaction_is_valid(transaction) ||
        commit->type != UCN_CLUSTER_HANDOVER_MESSAGE_COMMIT ||
        commit->sender_role != UCN_CLUSTER_HANDOVER_ROLE_HEAD ||
        commit->stepdown_nonce != 0U ||
        commit->source_node_id != transaction->old_epoch.head_node_id ||
        !transaction_identity_matches(transaction, commit)) {
        return UCN_ERR_ARGUMENT;
    }
    if (transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED ||
        transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_DURABLE) {
        if (transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED &&
            ucn_deadline_expired(now_ms, transaction->deadline_ms)) {
            transaction->state = UCN_CLUSTER_HANDOVER_STATE_ABORTED;
            transaction->recovery_observe_required = true;
            trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_ABORTED);
            return UCN_ERR_STATE;
        }
        return UCN_OK;
    }
    if (transaction->state != UCN_CLUSTER_HANDOVER_STATE_READY_SENT) {
        return UCN_ERR_STATE;
    }
    if (ucn_deadline_expired(now_ms, transaction->deadline_ms)) {
        transaction->state = UCN_CLUSTER_HANDOVER_STATE_ABORTED;
        transaction->recovery_observe_required = true;
        trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_ABORTED);
        return UCN_ERR_STATE;
    }
    transaction->state = UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED;
    trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_TARGET_COMMITTED);
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_mark_target_epoch_durable(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_epoch_t *durable_epoch,
    uint32_t now_ms)
{
    if (transaction == NULL || !transaction_is_valid(transaction) || durable_epoch == NULL ||
        transaction->state != UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED ||
        !epoch_is_exact(&transaction->target_epoch, durable_epoch)) {
        return UCN_ERR_STATE;
    }
    if (ucn_deadline_expired(now_ms, transaction->deadline_ms)) {
        transaction->state = UCN_CLUSTER_HANDOVER_STATE_ABORTED;
        transaction->recovery_observe_required = true;
        trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_ABORTED);
        return UCN_ERR_STATE;
    }
    /* An Epoch equality supplied by the caller is not a durable proof.  M11
     * has no M04 Provider submit+reload continuation, so fail closed rather
     * than creating an authority-bearing terminal state in this model. */
    return UCN_ERR_UNSUPPORTED;
}

bool ucn_cluster_handover_transaction_target_authority_ready(
    const ucn_cluster_handover_transaction_t *transaction)
{
    (void)transaction;
    /* No M04 reload proof exists in the default-OFF M11 archive. */
    return false;
}

bool ucn_cluster_handover_transaction_retry_due(
    const ucn_cluster_handover_transaction_t *transaction,
    uint32_t now_ms)
{
    return transaction_is_valid(transaction) &&
           transaction->state == UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT &&
           ucn_deadline_expired(now_ms, transaction->retry_deadline_ms) &&
           !ucn_deadline_expired(now_ms, transaction->deadline_ms);
}

ucn_result_t ucn_cluster_handover_transaction_note_prepare_retransmitted(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_policy_t *policy,
    uint32_t now_ms)
{
    uint32_t next_retry_deadline_ms;

    if (transaction == NULL || !transaction_is_valid(transaction) ||
        transaction->state != UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT ||
        !policy_is_valid(policy) ||
        !ucn_cluster_handover_transaction_retry_due(transaction, now_ms)) {
        return UCN_ERR_STATE;
    }
    next_retry_deadline_ms =
        ucn_deadline_from_now(now_ms, policy->retry_interval_ms);
    if (next_retry_deadline_ms == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (transaction->retry_count < UINT8_MAX) {
        ++transaction->retry_count;
    }
    transaction->retry_deadline_ms = next_retry_deadline_ms;
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_transaction_step(
    ucn_cluster_handover_transaction_t *transaction,
    uint32_t now_ms)
{
    if (transaction == NULL || !transaction_is_valid(transaction)) {
        return UCN_ERR_ARGUMENT;
    }
    if ((transaction->state == UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_READY_SENT ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_READY_VERIFIED ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_AUTHORITY_REVOKED ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_STEPDOWN_SENT ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_COMMIT_SENT ||
         transaction->state == UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED) &&
        ucn_deadline_expired(now_ms, transaction->deadline_ms)) {
        transaction->state = UCN_CLUSTER_HANDOVER_STATE_ABORTED;
        /* Once the old Head has fenced itself, timeout must never restore its
         * Authority.  Recovery is therefore an Observe path, not a rollback. */
        transaction->recovery_observe_required = !transaction->local_authority_active;
        trace_append(transaction, UCN_CLUSTER_HANDOVER_TRACE_ABORTED);
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_handover_member_accept_stepdown(
    const ucn_cluster_epoch_t *current_epoch,
    uint32_t active_config_id,
    uint32_t active_config_hash,
    ucn_node_id_t expected_old_head,
    uint8_t local_role,
    const ucn_cluster_handover_message_t *stepdown,
    ucn_cluster_handover_member_result_t *output)
{
    ucn_cluster_handover_member_result_t result;

    if (output == NULL || !epoch_is_valid(current_epoch) ||
        !node_id_is_valid(expected_old_head) || stepdown == NULL ||
        (local_role != UCN_CLUSTER_HANDOVER_ROLE_MEMBER &&
         local_role != UCN_CLUSTER_HANDOVER_ROLE_PROVISIONAL &&
         local_role != UCN_CLUSTER_HANDOVER_ROLE_BACKUP) ||
        stepdown->type != UCN_CLUSTER_HANDOVER_MESSAGE_STEPDOWN ||
        stepdown->sender_role != UCN_CLUSTER_HANDOVER_ROLE_HEAD ||
        stepdown->source_node_id != expected_old_head ||
        !target_epoch_is_valid_for_mode(&stepdown->old_epoch,
                                        &stepdown->target_epoch,
                                        stepdown->mode) ||
        !epoch_is_exact(current_epoch, &stepdown->old_epoch) ||
        !serial_is_valid(active_config_id) || active_config_hash == 0U ||
        stepdown->target_config_id != 0U || stepdown->target_config_hash != 0U ||
        !serial_is_valid(stepdown->transaction_id) ||
        !nonce_is_valid(stepdown->stepdown_nonce)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&result, 0, sizeof(result));
    result.join_target = true;
    result.target_epoch = stepdown->target_epoch;
    result.transaction_id = stepdown->transaction_id;
    result.stepdown_nonce = stepdown->stepdown_nonce;
    *output = result;
    return UCN_OK;
}

void ucn_cluster_handover_member_note_target_lost(
    ucn_cluster_handover_member_result_t *member)
{
    if (member != NULL && member->join_target) {
        member->join_target = false;
        member->observe_target = true;
    }
}

bool ucn_cluster_handover_trace_order_is_valid(
    const ucn_cluster_handover_transaction_t *transaction)
{
    size_t index;
    size_t ready = UCN_CLUSTER_HANDOVER_TRACE_CAPACITY;
    size_t revoke = UCN_CLUSTER_HANDOVER_TRACE_CAPACITY;
    size_t stepdown = UCN_CLUSTER_HANDOVER_TRACE_CAPACITY;
    size_t commit = UCN_CLUSTER_HANDOVER_TRACE_CAPACITY;

    if (!transaction_is_valid(transaction)) {
        return false;
    }
    for (index = 0U; index < transaction->trace_count; ++index) {
        switch (transaction->trace[index]) {
        case UCN_CLUSTER_HANDOVER_TRACE_READY_VERIFIED: ready = index; break;
        case UCN_CLUSTER_HANDOVER_TRACE_AUTHORITY_REVOKED: revoke = index; break;
        case UCN_CLUSTER_HANDOVER_TRACE_STEPDOWN_EMITTED: stepdown = index; break;
        case UCN_CLUSTER_HANDOVER_TRACE_COMMIT_EMITTED: commit = index; break;
        default: break;
        }
    }
    if (commit == UCN_CLUSTER_HANDOVER_TRACE_CAPACITY) {
        return true;
    }
    return ready < revoke && revoke < stepdown && stepdown < commit;
}
