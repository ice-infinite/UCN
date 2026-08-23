#include "ucn/ucn_cluster_takeover.h"

#include "ucn_cluster_takeover_internal.h"

#include <string.h>

#define TAKEOVER_CERTIFICATE_DOMAIN "UCN-CL4-TAKEOVER-CERT"
#define TAKEOVER_CRC32_INIT UINT32_C(0xFFFFFFFF)
#define TAKEOVER_CRC32_XOROUT UINT32_C(0xFFFFFFFF)
#define TAKEOVER_CRC32_POLYNOMIAL UINT32_C(0xEDB88320)

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool term_next_is_valid(uint32_t old_term, uint32_t proposed_term)
{
    return serial_is_valid(old_term) && old_term < UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD &&
           proposed_term == old_term + 1U;
}

static size_t set_word_count(const ucn_cluster_voter_set_t *set)
{
    return set == NULL ? 0U : ((size_t)set->count + (size_t)31U) / (size_t)32U;
}

static uint32_t set_word_valid_mask(const ucn_cluster_voter_set_t *set,
                                    size_t word_index)
{
    size_t words;
    uint8_t remainder;

    if (!ucn_cluster_voter_set_is_valid(set)) {
        return 0U;
    }
    words = set_word_count(set);
    if (word_index >= words) {
        return 0U;
    }
    remainder = (uint8_t)(set->count % 32U);
    if (word_index + 1U != words || remainder == 0U) {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << remainder) - UINT32_C(1);
}

static bool words_are_valid(const ucn_cluster_voter_set_t *set,
                            const uint32_t words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS])
{
    size_t index;

    if (!ucn_cluster_voter_set_is_valid(set) || words == NULL) {
        return false;
    }
    for (index = 0U; index < UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS;
         ++index) {
        if ((words[index] & ~set_word_valid_mask(set, index)) != 0U) {
            return false;
        }
    }
    return true;
}

static bool words_do_not_overlap(
    const uint32_t left[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS],
    const uint32_t right[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS])
{
    size_t index;

    if (left == NULL || right == NULL) {
        return false;
    }
    for (index = 0U; index < UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS;
         ++index) {
        if ((left[index] & right[index]) != 0U) {
            return false;
        }
    }
    return true;
}

static uint8_t word_popcount(uint32_t value)
{
    uint8_t count = 0U;

    while (value != 0U) {
        value &= value - UINT32_C(1);
        ++count;
    }
    return count;
}

static uint8_t words_popcount(
    const uint32_t words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS])
{
    uint8_t count = 0U;
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS;
         ++index) {
        count = (uint8_t)(count + word_popcount(words[index]));
    }
    return count;
}

static bool set_has_node(const ucn_cluster_voter_set_t *set,
                         ucn_node_id_t node_id,
                         size_t *index_output)
{
    size_t index;

    if (!ucn_cluster_voter_set_is_valid(set) || !node_id_is_valid(node_id)) {
        return false;
    }
    for (index = 0U; index < set->count; ++index) {
        if (set->node_ids[index] == node_id) {
            if (index_output != NULL) {
                *index_output = index;
            }
            return true;
        }
    }
    return false;
}

static void words_set_node(const ucn_cluster_voter_set_t *set,
                           ucn_node_id_t node_id,
                           uint32_t words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS])
{
    size_t index;

    if (set_has_node(set, node_id, &index)) {
        words[index / 32U] |= UINT32_C(1) << (index % 32U);
    }
}

static bool words_have_node(const ucn_cluster_voter_set_t *set,
                            ucn_node_id_t node_id,
                            const uint32_t words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS])
{
    size_t index;

    return set_has_node(set, node_id, &index) &&
           (words[index / 32U] & (UINT32_C(1) << (index % 32U))) != 0U;
}

static bool required_set_is_reached(const ucn_cluster_voter_set_t *set,
                                    const uint32_t words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS])
{
    return ucn_cluster_voter_set_is_valid(set) && words_are_valid(set, words) &&
           words_popcount(words) >= ucn_cluster_voter_set_quorum(set);
}

static bool required_set_is_possible(
    const ucn_cluster_voter_set_t *set,
    const uint32_t votes[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS],
    const uint32_t unreachable[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS])
{
    uint8_t reachable = 0U;
    size_t index;

    if (!ucn_cluster_voter_set_is_valid(set) || !words_are_valid(set, votes) ||
        !words_are_valid(set, unreachable)) {
        return false;
    }
    for (index = 0U; index < UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS;
         ++index) {
        reachable = (uint8_t)(reachable + word_popcount(
            set_word_valid_mask(set, index) & ~unreachable[index]));
    }
    return reachable >= ucn_cluster_voter_set_quorum(set);
}

static uint8_t required_set_mask(const ucn_cluster_config_state_t *config)
{
    if (!ucn_cluster_config_state_is_valid(config)) {
        return 0U;
    }
    return config->phase == (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ?
               UCN_CLUSTER_TAKEOVER_SET_OLD :
               (UCN_CLUSTER_TAKEOVER_SET_OLD | UCN_CLUSTER_TAKEOVER_SET_NEW);
}

static bool epoch_is_exact(const ucn_cluster_epoch_t *left,
                           const ucn_cluster_epoch_t *right)
{
    return left != NULL && right != NULL &&
           left->cluster_id == right->cluster_id && left->term == right->term &&
           left->head_node_id == right->head_node_id;
}

static bool transaction_fields_are_valid(
    const ucn_cluster_takeover_transaction_t *transaction)
{
    const ucn_cluster_snapshot_epoch_t *snapshot;
    const ucn_cluster_backup_epoch_t *backup_epoch;
    const ucn_cluster_config_state_t *config;

    if (transaction == NULL || !transaction->active) {
        return false;
    }
    snapshot = &transaction->frozen_snapshot_epoch;
    backup_epoch = &snapshot->backup_epoch;
    config = &transaction->frozen_config;
    if (!ucn_cluster_snapshot_epoch_is_valid(snapshot) ||
        !ucn_cluster_snapshot_epoch_matches_config(snapshot, config) ||
        !ucn_cluster_config_state_is_valid(config) ||
        !ucn_cluster_takeover_vote_id_is_valid(&transaction->vote_id) ||
        transaction->deadline_ms == 0U || transaction->transaction_id == 0U ||
        transaction->vote_id.cluster_id != backup_epoch->cluster_id ||
        transaction->vote_id.old_term != backup_epoch->term ||
        transaction->vote_id.config_id != config->config_id ||
        transaction->vote_id.backup_node_id != backup_epoch->backup_node_id ||
        transaction->vote_id.backup_generation != backup_epoch->backup_generation ||
        transaction->vote_id.snapshot_id != snapshot->snapshot_id ||
        transaction->proposed_epoch.cluster_id != backup_epoch->cluster_id ||
        transaction->proposed_epoch.term != transaction->vote_id.proposed_term ||
        transaction->proposed_epoch.head_node_id != backup_epoch->backup_node_id ||
        !words_are_valid(&config->old_set, transaction->old_vote_words) ||
        !words_are_valid(&config->new_set, transaction->new_vote_words) ||
        !words_are_valid(&config->old_set, transaction->old_unreachable_words) ||
        !words_are_valid(&config->new_set, transaction->new_unreachable_words) ||
        !words_do_not_overlap(transaction->old_vote_words,
                              transaction->old_unreachable_words) ||
        !words_do_not_overlap(transaction->new_vote_words,
                              transaction->new_unreachable_words)) {
        return false;
    }
    if (transaction->state != (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_COLLECTING &&
        transaction->state != (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_QUORUM &&
        transaction->state !=
            (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE) {
        return false;
    }
    if ((transaction->state ==
             (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE) !=
        transaction->proposed_epoch_durable) {
        return false;
    }
    if ((required_set_mask(config) & UCN_CLUSTER_TAKEOVER_SET_OLD) != 0U &&
        !words_have_node(&config->old_set, backup_epoch->backup_node_id,
                         transaction->old_vote_words)) {
        return false;
    }
    if ((required_set_mask(config) & UCN_CLUSTER_TAKEOVER_SET_NEW) != 0U &&
        !words_have_node(&config->new_set, backup_epoch->backup_node_id,
                         transaction->new_vote_words)) {
        return false;
    }
    return true;
}

static uint32_t crc32_byte(uint32_t crc, uint8_t value)
{
    uint8_t bit;

    crc ^= value;
    for (bit = 0U; bit < 8U; ++bit) {
        crc = (crc & UINT32_C(1)) != 0U ?
                  (crc >> 1U) ^ TAKEOVER_CRC32_POLYNOMIAL :
                  (crc >> 1U);
    }
    return crc;
}

static uint32_t crc32_u32_be(uint32_t crc, uint32_t value)
{
    crc = crc32_byte(crc, (uint8_t)(value >> 24U));
    crc = crc32_byte(crc, (uint8_t)(value >> 16U));
    crc = crc32_byte(crc, (uint8_t)(value >> 8U));
    return crc32_byte(crc, (uint8_t)value);
}

static uint32_t certificate_crc32(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate)
{
    static const char domain[] = TAKEOVER_CERTIFICATE_DOMAIN;
    const ucn_cluster_config_state_t *config = &transaction->frozen_config;
    uint32_t crc = TAKEOVER_CRC32_INIT;
    size_t index;
    uint8_t set_mask;

    for (index = 0U; index < sizeof(domain) - 1U; ++index) {
        crc = crc32_byte(crc, (uint8_t)domain[index]);
    }
    crc = crc32_u32_be(crc, certificate->vote_id.cluster_id);
    crc = crc32_u32_be(crc, certificate->vote_id.proposed_term);
    crc = crc32_u32_be(crc, certificate->vote_id.backup_node_id);
    crc = crc32_u32_be(crc, certificate->vote_id.backup_generation);
    crc = crc32_u32_be(crc, certificate->vote_id.snapshot_id);
    crc = crc32_u32_be(crc, transaction->transaction_id);
    crc = crc32_u32_be(crc, certificate->certificate_anchor_config_id);
    crc = crc32_u32_be(crc, certificate->required_set_mask);
    for (set_mask = UCN_CLUSTER_TAKEOVER_SET_OLD;
         set_mask <= UCN_CLUSTER_TAKEOVER_SET_NEW;
         set_mask = (uint8_t)(set_mask << 1U)) {
        const ucn_cluster_voter_set_t *set;
        const uint32_t *words;
        size_t word_count;

        if ((certificate->required_set_mask & set_mask) == 0U) {
            continue;
        }
        set = set_mask == UCN_CLUSTER_TAKEOVER_SET_OLD ?
                  &config->old_set : &config->new_set;
        words = set_mask == UCN_CLUSTER_TAKEOVER_SET_OLD ?
                    certificate->old_vote_words : certificate->new_vote_words;
        word_count = set_word_count(set);
        crc = crc32_byte(crc, set_mask);
        crc = crc32_u32_be(crc, set->config_id);
        crc = crc32_u32_be(crc, set->hash);
        crc = crc32_u32_be(crc, set->count);
        crc = crc32_u32_be(crc, (uint32_t)word_count);
        for (index = 0U; index < word_count; ++index) {
            crc = crc32_u32_be(crc, words[index]);
        }
    }
    return crc ^ TAKEOVER_CRC32_XOROUT;
}

static void transaction_abort(ucn_cluster_takeover_transaction_t *transaction)
{
    transaction->active = false;
    transaction->self_vote_durable = false;
    transaction->proposed_epoch_durable = false;
    transaction->recovery_required = true;
    transaction->state = (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_ABORTED;
}

void ucn_cluster_takeover_transaction_reset(
    ucn_cluster_takeover_transaction_t *transaction)
{
    if (transaction != NULL) {
        (void)memset(transaction, 0, sizeof(*transaction));
    }
}

bool ucn_cluster_takeover_vote_id_is_valid(
    const ucn_cluster_takeover_vote_id_t *vote_id)
{
    return vote_id != NULL && node_id_is_valid(vote_id->cluster_id) &&
           term_next_is_valid(vote_id->old_term, vote_id->proposed_term) &&
           serial_is_valid(vote_id->config_id) &&
           node_id_is_valid(vote_id->backup_node_id) &&
           serial_is_valid(vote_id->backup_generation) &&
           serial_is_valid(vote_id->snapshot_id);
}

bool ucn_cluster_takeover_vote_id_is_exact(
    const ucn_cluster_takeover_vote_id_t *left,
    const ucn_cluster_takeover_vote_id_t *right)
{
    return ucn_cluster_takeover_vote_id_is_valid(left) &&
           ucn_cluster_takeover_vote_id_is_valid(right) &&
           left->cluster_id == right->cluster_id &&
           left->old_term == right->old_term &&
           left->proposed_term == right->proposed_term &&
           left->config_id == right->config_id &&
           left->backup_node_id == right->backup_node_id &&
           left->backup_generation == right->backup_generation &&
           left->snapshot_id == right->snapshot_id;
}

bool ucn_cluster_takeover_transaction_is_active(
    const ucn_cluster_takeover_transaction_t *transaction)
{
    return transaction_fields_are_valid(transaction);
}

ucn_result_t ucn_cluster_takeover_transaction_begin(
    ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_backup_sync_owner_t *backup_owner,
    uint32_t takeover_transaction_id,
    uint32_t now_ms,
    uint32_t takeover_window_ms)
{
    ucn_cluster_takeover_transaction_t candidate;
    const ucn_cluster_snapshot_epoch_t *snapshot;
    uint32_t deadline;

    if (transaction == NULL || backup_owner == NULL ||
        !ucn_cluster_backup_sync_owner_is_valid(backup_owner) ||
        !ucn_cluster_backup_sync_owner_takeover_eligible(backup_owner) ||
        takeover_transaction_id == 0U ||
        takeover_transaction_id > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        takeover_window_ms == 0U ||
        takeover_window_ms > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return UCN_ERR_ARGUMENT;
    }
    snapshot = ucn_cluster_backup_mirror_committed_epoch(&backup_owner->mirror);
    if (snapshot == NULL || !backup_owner->mirror.committed_valid ||
        !ucn_cluster_snapshot_epoch_is_exact(snapshot,
                                             &backup_owner->mirror.committed_epoch) ||
        !ucn_cluster_snapshot_epoch_matches_config(snapshot,
                                                   &backup_owner->active_config) ||
        !ucn_cluster_backup_epoch_is_exact(&snapshot->backup_epoch,
                                           &backup_owner->assigned_epoch) ||
        snapshot->backup_epoch.term >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return UCN_ERR_STATE;
    }
    deadline = ucn_deadline_from_now(now_ms, takeover_window_ms);
    if (deadline == 0U) {
        return UCN_ERR_EXHAUSTED;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.frozen_snapshot_epoch = *snapshot;
    candidate.frozen_config = backup_owner->active_config;
    candidate.vote_id.cluster_id = snapshot->backup_epoch.cluster_id;
    candidate.vote_id.old_term = snapshot->backup_epoch.term;
    candidate.vote_id.proposed_term = snapshot->backup_epoch.term + 1U;
    candidate.vote_id.config_id = candidate.frozen_config.config_id;
    candidate.vote_id.backup_node_id = snapshot->backup_epoch.backup_node_id;
    candidate.vote_id.backup_generation = snapshot->backup_epoch.backup_generation;
    candidate.vote_id.snapshot_id = snapshot->snapshot_id;
    candidate.proposed_epoch.cluster_id = candidate.vote_id.cluster_id;
    candidate.proposed_epoch.term = candidate.vote_id.proposed_term;
    candidate.proposed_epoch.head_node_id = candidate.vote_id.backup_node_id;
    candidate.deadline_ms = deadline;
    candidate.transaction_id = takeover_transaction_id;
    candidate.active = true;
    candidate.state = (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_COLLECTING;
    if (!set_has_node(&candidate.frozen_config.old_set,
                      candidate.vote_id.backup_node_id, NULL) ||
        ((required_set_mask(&candidate.frozen_config) &
          UCN_CLUSTER_TAKEOVER_SET_NEW) != 0U &&
         !set_has_node(&candidate.frozen_config.new_set,
                       candidate.vote_id.backup_node_id, NULL))) {
        return UCN_ERR_ACCESS;
    }
    words_set_node(&candidate.frozen_config.old_set, candidate.vote_id.backup_node_id,
                   candidate.old_vote_words);
    if ((required_set_mask(&candidate.frozen_config) &
         UCN_CLUSTER_TAKEOVER_SET_NEW) != 0U) {
        words_set_node(&candidate.frozen_config.new_set,
                       candidate.vote_id.backup_node_id, candidate.new_vote_words);
    }
    if (!transaction_fields_are_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *transaction = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
    ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_vote_id_t *vote_id)
{
    if (!transaction_fields_are_valid(transaction) ||
        transaction->state ==
            (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE ||
        !ucn_cluster_takeover_vote_id_is_exact(&transaction->vote_id, vote_id)) {
        return UCN_ERR_REPLAY;
    }
    transaction->self_vote_durable = true;
    return UCN_OK;
}

ucn_result_t ucn_cluster_takeover_transaction_note_durable_vote(
    ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_vote_id_t *vote_id,
    const ucn_cluster_takeover_remote_vote_proof_t *proof)
{
    ucn_cluster_takeover_transaction_t candidate;
    bool accepted = false;

    if (!transaction_fields_are_valid(transaction) ||
        transaction->state ==
            (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE ||
        !ucn_cluster_takeover_vote_id_is_exact(&transaction->vote_id, vote_id) ||
        proof == NULL || !proof->exact_vote_durable ||
        ucn_cluster_takeover_member_vote_gate(transaction, &proof->member) != UCN_OK) {
        return UCN_ERR_REPLAY;
    }
    candidate = *transaction;
    if (set_has_node(&candidate.frozen_config.old_set, proof->member.voter_node_id,
                     NULL)) {
        words_set_node(&candidate.frozen_config.old_set, proof->member.voter_node_id,
                       candidate.old_vote_words);
        accepted = true;
    }
    if ((required_set_mask(&candidate.frozen_config) &
         UCN_CLUSTER_TAKEOVER_SET_NEW) != 0U &&
        set_has_node(&candidate.frozen_config.new_set,
                     proof->member.voter_node_id, NULL)) {
        words_set_node(&candidate.frozen_config.new_set,
                       proof->member.voter_node_id,
                        candidate.new_vote_words);
        accepted = true;
    }
    if (!accepted || !transaction_fields_are_valid(&candidate)) {
        return UCN_ERR_ACCESS;
    }
    if (candidate.self_vote_durable &&
        ucn_cluster_takeover_transaction_quorum_reached(&candidate)) {
        candidate.state = (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_QUORUM;
    }
    *transaction = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_takeover_member_vote_gate(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_member_vote_context_t *context)
{
    bool frozen_voter;

    if (!transaction_fields_are_valid(transaction) || context == NULL ||
        !node_id_is_valid(context->voter_node_id)) {
        return UCN_ERR_ARGUMENT;
    }
    frozen_voter = set_has_node(&transaction->frozen_config.old_set,
                                context->voter_node_id, NULL) ||
                   ((required_set_mask(&transaction->frozen_config) &
                     UCN_CLUSTER_TAKEOVER_SET_NEW) != 0U &&
                    set_has_node(&transaction->frozen_config.new_set,
                                 context->voter_node_id, NULL));
    return context->member_takeover_grace && context->old_head_lease_expired &&
                   context->committed_v4_voter && frozen_voter ?
               UCN_OK :
               UCN_ERR_ACCESS;
}

bool ucn_cluster_takeover_transaction_quorum_reached(
    const ucn_cluster_takeover_transaction_t *transaction)
{
    uint8_t mask;

    if (!transaction_fields_are_valid(transaction) || !transaction->self_vote_durable) {
        return false;
    }
    mask = required_set_mask(&transaction->frozen_config);
    return (mask & UCN_CLUSTER_TAKEOVER_SET_OLD) != 0U &&
           required_set_is_reached(&transaction->frozen_config.old_set,
                                   transaction->old_vote_words) &&
           ((mask & UCN_CLUSTER_TAKEOVER_SET_NEW) == 0U ||
            required_set_is_reached(&transaction->frozen_config.new_set,
                                    transaction->new_vote_words));
}

bool ucn_cluster_takeover_transaction_quorum_possible(
    const ucn_cluster_takeover_transaction_t *transaction)
{
    uint8_t mask;

    if (!transaction_fields_are_valid(transaction)) {
        return false;
    }
    mask = required_set_mask(&transaction->frozen_config);
    return (mask & UCN_CLUSTER_TAKEOVER_SET_OLD) != 0U &&
           required_set_is_possible(&transaction->frozen_config.old_set,
                                    transaction->old_vote_words,
                                    transaction->old_unreachable_words) &&
           ((mask & UCN_CLUSTER_TAKEOVER_SET_NEW) == 0U ||
            required_set_is_possible(&transaction->frozen_config.new_set,
                                     transaction->new_vote_words,
                                     transaction->new_unreachable_words));
}

ucn_result_t ucn_cluster_takeover_transaction_note_voter_unreachable(
    ucn_cluster_takeover_transaction_t *transaction,
    ucn_node_id_t voter_node_id)
{
    ucn_cluster_takeover_transaction_t candidate;
    bool found = false;

    if (!transaction_fields_are_valid(transaction) || !node_id_is_valid(voter_node_id)) {
        return UCN_ERR_ARGUMENT;
    }
    if (transaction->state ==
        (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE) {
        return UCN_ERR_REPLAY;
    }
    candidate = *transaction;
    if (set_has_node(&candidate.frozen_config.old_set, voter_node_id, NULL)) {
        words_set_node(&candidate.frozen_config.old_set, voter_node_id,
                       candidate.old_unreachable_words);
        found = true;
    }
    if ((required_set_mask(&candidate.frozen_config) &
         UCN_CLUSTER_TAKEOVER_SET_NEW) != 0U &&
        set_has_node(&candidate.frozen_config.new_set, voter_node_id, NULL)) {
        words_set_node(&candidate.frozen_config.new_set, voter_node_id,
                       candidate.new_unreachable_words);
        found = true;
    }
    if (!found) {
        return UCN_ERR_ACCESS;
    }
    if (!words_do_not_overlap(candidate.old_vote_words,
                              candidate.old_unreachable_words) ||
        !words_do_not_overlap(candidate.new_vote_words,
                              candidate.new_unreachable_words)) {
        return UCN_ERR_REPLAY;
    }
    if (!transaction_fields_are_valid(&candidate)) {
        return UCN_ERR_ACCESS;
    }
    if (!ucn_cluster_takeover_transaction_quorum_possible(&candidate)) {
        transaction_abort(&candidate);
        *transaction = candidate;
        return UCN_ERR_STATE;
    }
    *transaction = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_takeover_transaction_step(
    ucn_cluster_takeover_transaction_t *transaction,
    uint32_t now_ms)
{
    if (!transaction_fields_are_valid(transaction)) {
        return UCN_ERR_STATE;
    }
    if (transaction->state ==
        (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE) {
        /* A durable successor is terminal. Deadline and late collection
         * events must not detach RAM state from the persisted Epoch. */
        return UCN_OK;
    }
    if (!ucn_cluster_takeover_transaction_quorum_possible(transaction) ||
        ucn_deadline_expired(now_ms, transaction->deadline_ms)) {
        transaction_abort(transaction);
        return UCN_ERR_STATE;
    }
    if (ucn_cluster_takeover_transaction_quorum_reached(transaction)) {
        transaction->state = (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_QUORUM;
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_takeover_certificate_build(
    const ucn_cluster_takeover_transaction_t *transaction,
    ucn_cluster_takeover_certificate_t *output)
{
    ucn_cluster_takeover_certificate_t candidate;

    if (transaction == NULL || output == NULL ||
        !ucn_cluster_takeover_transaction_quorum_reached(transaction)) {
        return UCN_ERR_STATE;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.vote_id = transaction->vote_id;
    candidate.required_set_mask = required_set_mask(&transaction->frozen_config);
    candidate.certificate_anchor_config_id =
        candidate.required_set_mask == UCN_CLUSTER_TAKEOVER_SET_OLD ?
            transaction->frozen_config.old_set.config_id :
            transaction->frozen_config.new_set.config_id;
    (void)memcpy(candidate.old_vote_words, transaction->old_vote_words,
                 sizeof(candidate.old_vote_words));
    (void)memcpy(candidate.new_vote_words, transaction->new_vote_words,
                 sizeof(candidate.new_vote_words));
    candidate.canonical_crc32 = certificate_crc32(transaction, &candidate);
    if (!ucn_cluster_takeover_certificate_is_valid(transaction, &candidate)) {
        return UCN_ERR_STATE;
    }
    *output = candidate;
    return UCN_OK;
}

bool ucn_cluster_takeover_certificate_is_valid(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate)
{
    uint8_t required;
    uint32_t expected_anchor;

    if (!transaction_fields_are_valid(transaction) || certificate == NULL ||
        !ucn_cluster_takeover_vote_id_is_exact(&transaction->vote_id,
                                               &certificate->vote_id)) {
        return false;
    }
    required = required_set_mask(&transaction->frozen_config);
    expected_anchor = required == UCN_CLUSTER_TAKEOVER_SET_OLD ?
                          transaction->frozen_config.old_set.config_id :
                          transaction->frozen_config.new_set.config_id;
    return certificate->required_set_mask == required &&
           certificate->certificate_anchor_config_id == expected_anchor &&
           words_are_valid(&transaction->frozen_config.old_set,
                           certificate->old_vote_words) &&
           words_are_valid(&transaction->frozen_config.new_set,
                           certificate->new_vote_words) &&
           required_set_is_reached(&transaction->frozen_config.old_set,
                                   certificate->old_vote_words) &&
           words_have_node(&transaction->frozen_config.old_set,
                           transaction->vote_id.backup_node_id,
                           certificate->old_vote_words) &&
           ((required & UCN_CLUSTER_TAKEOVER_SET_NEW) == 0U ||
            (required_set_is_reached(&transaction->frozen_config.new_set,
                                     certificate->new_vote_words) &&
             words_have_node(&transaction->frozen_config.new_set,
                             transaction->vote_id.backup_node_id,
                             certificate->new_vote_words))) &&
           certificate->canonical_crc32 == certificate_crc32(transaction, certificate);
}

ucn_result_t ucn_cluster_takeover_certificate_fragment_get(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate,
    uint8_t set_mask,
    uint8_t fragment_index,
    ucn_cluster_takeover_certificate_fragment_t *output)
{
    const ucn_cluster_voter_set_t *set;
    const uint32_t *words;
    ucn_cluster_takeover_certificate_fragment_t candidate;
    size_t count;

    if (output == NULL || !ucn_cluster_takeover_certificate_is_valid(transaction,
                                                                      certificate) ||
        (set_mask != UCN_CLUSTER_TAKEOVER_SET_OLD &&
         set_mask != UCN_CLUSTER_TAKEOVER_SET_NEW) ||
        (certificate->required_set_mask & set_mask) == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    set = set_mask == UCN_CLUSTER_TAKEOVER_SET_OLD ?
              &transaction->frozen_config.old_set :
              &transaction->frozen_config.new_set;
    words = set_mask == UCN_CLUSTER_TAKEOVER_SET_OLD ?
                certificate->old_vote_words : certificate->new_vote_words;
    count = set_word_count(set);
    if (fragment_index >= count) {
        return UCN_ERR_REPLAY;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.set_mask = set_mask;
    candidate.fragment_index = fragment_index;
    candidate.fragment_count = (uint8_t)count;
    candidate.config_id = set->config_id;
    candidate.config_hash = set->hash;
    candidate.vote_bitmap_word = words[fragment_index];
    *output = candidate;
    return UCN_OK;
}

bool ucn_cluster_takeover_certificate_fragment_is_valid(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate,
    const ucn_cluster_takeover_certificate_fragment_t *fragment)
{
    ucn_cluster_takeover_certificate_fragment_t expected;

    return fragment != NULL &&
           ucn_cluster_takeover_certificate_fragment_get(
               transaction, certificate, fragment->set_mask,
               fragment->fragment_index, &expected) == UCN_OK &&
           expected.fragment_count == fragment->fragment_count &&
           expected.config_id == fragment->config_id &&
           expected.config_hash == fragment->config_hash &&
           expected.vote_bitmap_word == fragment->vote_bitmap_word;
}

ucn_result_t ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
    ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_vote_id_t *vote_id)
{
    if (!transaction_fields_are_valid(transaction) ||
        !ucn_cluster_takeover_vote_id_is_exact(&transaction->vote_id, vote_id)) {
        return UCN_ERR_STATE;
    }
    if (transaction->state ==
            (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE &&
        transaction->proposed_epoch_durable) {
        return UCN_OK;
    }
    if (!ucn_cluster_takeover_transaction_quorum_reached(transaction)) {
        return UCN_ERR_STATE;
    }
    transaction->proposed_epoch_durable = true;
    transaction->state = (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE;
    return UCN_OK;
}

bool ucn_cluster_takeover_transaction_head_result_ready(
    const ucn_cluster_takeover_transaction_t *transaction)
{
    return transaction_fields_are_valid(transaction) &&
           transaction->proposed_epoch_durable &&
           transaction->state == (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE;
}

void ucn_cluster_takeover_old_primary_fence_reset(
    ucn_cluster_takeover_old_primary_fence_t *fence)
{
    if (fence != NULL) {
        (void)memset(fence, 0, sizeof(*fence));
    }
}

ucn_result_t ucn_cluster_takeover_old_primary_fence_accept(
    ucn_cluster_takeover_old_primary_fence_t *fence,
    const ucn_cluster_epoch_t *old_primary_epoch,
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate)
{
    ucn_cluster_takeover_old_primary_fence_t candidate;

    if (fence == NULL || old_primary_epoch == NULL ||
        !ucn_cluster_takeover_certificate_is_valid(transaction, certificate) ||
        old_primary_epoch->cluster_id != transaction->frozen_snapshot_epoch.backup_epoch.cluster_id ||
        old_primary_epoch->term != transaction->frozen_snapshot_epoch.backup_epoch.term ||
        old_primary_epoch->head_node_id != transaction->frozen_snapshot_epoch.backup_epoch.head_node_id ||
        !term_next_is_valid(old_primary_epoch->term, transaction->proposed_epoch.term) ||
        transaction->proposed_epoch.cluster_id != old_primary_epoch->cluster_id) {
        return UCN_ERR_ACCESS;
    }
    if (fence->fenced) {
        return epoch_is_exact(&fence->accepted_epoch, &transaction->proposed_epoch) &&
                       fence->accepted_certificate_crc32 == certificate->canonical_crc32 ?
                   UCN_OK :
                   UCN_ERR_REPLAY;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.accepted_epoch = transaction->proposed_epoch;
    candidate.accepted_certificate_crc32 = certificate->canonical_crc32;
    candidate.fenced = true;
    candidate.join_required = true;
    *fence = candidate;
    return UCN_OK;
}
