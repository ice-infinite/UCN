#include "ucn/ucn_cluster_invariant.h"

#include "ucn/ucn_cluster_authority.h"
#include "ucn/ucn_cluster_membership.h"
#include "ucn/ucn_cluster_storage.h"

static bool serial_is_zero_or_valid(uint32_t value)
{
    return value == 0U || value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool triplet_is_all_zero(uint32_t first, uint32_t second,
                                uint32_t third)
{
    return first == 0U && second == 0U && third == 0U;
}

static bool triplet_is_all_valid(uint32_t first, uint32_t second,
                                 uint32_t third)
{
    return first != 0U && first != UCN_NODE_BROADCAST &&
           second != 0U && third != 0U &&
           serial_is_zero_or_valid(second) &&
           serial_is_zero_or_valid(third);
}

static bool authority_phase_is_non_writable(ucn_cluster_phase_t phase)
{
    return phase == UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE ||
           phase == UCN_CLUSTER_PHASE_HEAD_FENCED ||
           phase == UCN_CLUSTER_PHASE_JOIN_PENDING ||
           phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE ||
           phase == UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT;
}

static bool voter_set_is_empty(const ucn_cluster_voter_set_t *set)
{
    size_t index;

    if (set == NULL || set->config_id != 0U || set->hash != 0U ||
        set->count != 0U) {
        return false;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_VOTERS; ++index) {
        if (set->node_ids[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool serial_fields_are_valid(const ucn_cluster_t *cluster)
{
    return serial_is_zero_or_valid(cluster->term) &&
           serial_is_zero_or_valid(cluster->pending_term) &&
           serial_is_zero_or_valid(cluster->max_seen_term) &&
           serial_is_zero_or_valid(cluster->cluster_id_round) &&
           serial_is_zero_or_valid(cluster->backup_generation) &&
           serial_is_zero_or_valid(cluster->membership_sequence) &&
           serial_is_zero_or_valid(cluster->known_backup_generation) &&
           serial_is_zero_or_valid(cluster->member_voted_term) &&
           serial_is_zero_or_valid(cluster->member_voted_generation) &&
           serial_is_zero_or_valid(cluster->recovery_nonce) &&
           serial_is_zero_or_valid(cluster->accepted_recovery_nonce) &&
           serial_is_zero_or_valid(cluster->parent_term) &&
           serial_is_zero_or_valid(cluster->parent_config_id) &&
           serial_is_zero_or_valid(cluster->recovery_round) &&
           serial_is_zero_or_valid(cluster->lineage_config_binding);
}

static bool persistence_descriptor_is_valid(const ucn_cluster_t *cluster)
{
    bool required = cluster->config.persistence_mode ==
                    UCN_CLUSTER_PERSISTENCE_REQUIRED;

    if (!required) {
        return !cluster->persistence_pending &&
               !cluster->persistence_faulted &&
               !cluster->persistence_retry_pending &&
               !cluster->persistence_io_active;
    }
    if (cluster->persistence_pending) {
        if (cluster->persistence_pending_action == 0U ||
            cluster->persistence_pending_operation == 0U ||
            cluster->persistence_pending_token == 0U ||
            cluster->persistence_pending_operation_id == 0U ||
            cluster->persistence_pending_fingerprint == 0U) {
            return false;
        }
    } else if (cluster->persistence_pending_action != 0U ||
               cluster->persistence_pending_operation != 0U ||
               cluster->persistence_pending_destination != 0U ||
               cluster->persistence_pending_token != 0U ||
               cluster->persistence_pending_operation_id != 0U ||
               cluster->persistence_pending_fingerprint != 0U) {
        return false;
    }
    if (cluster->persistence_faulted &&
        (cluster->persistence_failure == UCN_OK ||
         cluster->persistence_pending || cluster->persistence_retry_pending ||
         cluster->persistence_io_active)) {
        return false;
    }
    if (cluster->persistence_pending && cluster->persistence_retry_pending) {
        return false;
    }
    if (cluster->persistence_retry_pending) {
        if (cluster->persistence_retry_action == 0U ||
            !node_id_is_valid(cluster->persistence_retry_destination)) {
            return false;
        }
    } else if (cluster->persistence_retry_dispatch ||
               cluster->persistence_retry_action != 0U ||
               cluster->persistence_retry_destination != 0U) {
        return false;
    }
    return true;
}

ucn_result_t ucn_cluster_invariant_check(
    const ucn_cluster_t *cluster,
    uint32_t now_ms,
    uint32_t *violation_mask)
{
    uint32_t violations = 0U;
    ucn_cluster_role_t role;
    bool vote_empty;
    bool vote_valid;
    bool history_empty;
    bool history_valid;
    bool pending_empty;
    bool pending_valid;

    if (cluster == NULL || violation_mask == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    role = ucn_cluster_get_role(cluster);

    if (cluster->authority_active &&
        (cluster->authority_runtime == NULL ||
         role != UCN_CLUSTER_ROLE_HEAD ||
         cluster->head_node_id != cluster->config.local_node_id ||
         cluster->persistence_faulted)) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_1_SINGLE_AUTHORITY;
    }
    if (cluster->authority_active &&
        !ucn_cluster_authority_runtime_quorum_met(cluster->authority_runtime,
                                                   now_ms)) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_2_AUTHORITY_QUORUM;
    }
    if (cluster->authority_active &&
        cluster->phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_3_TAKEOVER_MAJORITY;
    }
    if (ucn_cluster_recovery_scoped(cluster) &&
        (cluster->parent_cluster_id == 0U ||
         cluster->recovery_cluster_id == 0U ||
         cluster->recovery_cluster_id == cluster->parent_cluster_id ||
         cluster->cluster_id != cluster->recovery_cluster_id)) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_4_RECOVERY_ISOLATION;
    }

    vote_empty = triplet_is_all_zero(cluster->member_voted_cluster_id,
                                     cluster->member_voted_term,
                                     cluster->member_voted_generation);
    vote_valid = triplet_is_all_valid(cluster->member_voted_cluster_id,
                                      cluster->member_voted_term,
                                      cluster->member_voted_generation);
    if (!vote_empty && !vote_valid) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_5_PERSISTENT_VOTE;
    }
    if (!voter_set_is_empty(&cluster->active_voter_set) &&
        !ucn_cluster_voter_set_is_valid(&cluster->active_voter_set)) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_6_CONFIG_SAFETY;
    }

    history_empty = triplet_is_all_zero(cluster->last_cluster_id,
                                        cluster->max_seen_term,
                                        cluster->last_stable_head);
    history_valid = cluster->last_cluster_id != 0U &&
                    serial_is_zero_or_valid(cluster->max_seen_term) &&
                    cluster->max_seen_term != 0U &&
                    node_id_is_valid(cluster->last_stable_head);
    pending_empty = triplet_is_all_zero(cluster->pending_cluster_id,
                                        cluster->pending_term,
                                        cluster->pending_head_node_id);
    pending_valid = cluster->pending_cluster_id != 0U &&
                    serial_is_zero_or_valid(cluster->pending_term) &&
                    cluster->pending_term != 0U &&
                    node_id_is_valid(cluster->pending_head_node_id);
    if ((!history_empty && !history_valid) ||
        (!pending_empty && !pending_valid)) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_7_REPLAY_ISOLATION;
    }
    if (cluster->authority_active &&
        (authority_phase_is_non_writable(cluster->authority_phase) ||
         cluster->authority_fence_reason !=
             UCN_CLUSTER_AUTHORITY_FENCE_NONE ||
         cluster->persistence_faulted)) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_8_FENCE_ORDERING;
    }
    if (!serial_fields_are_valid(cluster)) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_9_NO_SERIAL_REUSE;
    }
    if (!persistence_descriptor_is_valid(cluster) ||
        ((cluster->persistence_pending || cluster->persistence_io_active ||
          cluster->persistence_faulted) && cluster->authority_active)) {
        violations |= UCN_CLUSTER_INVARIANT_SAFETY_10_PERSIST_BEFORE_PROMISE;
    }

    *violation_mask = violations;
    return UCN_OK;
}

ucn_result_t ucn_cluster_invariant_check_network(
    const ucn_cluster_t *const *clusters,
    size_t cluster_count,
    uint32_t now_ms,
    uint32_t *violation_mask)
{
    uint32_t violations = 0U;
    size_t left;
    size_t right;

    if (clusters == NULL || violation_mask == NULL || cluster_count == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    for (left = 0U; left < cluster_count; ++left) {
        uint32_t local;
        ucn_result_t result;

        if (clusters[left] == NULL) {
            return UCN_ERR_ARGUMENT;
        }
        result = ucn_cluster_invariant_check(clusters[left], now_ms, &local);
        if (result != UCN_OK) {
            return result;
        }
        violations |= local;
        if (!clusters[left]->authority_active ||
            clusters[left]->cluster_id == 0U) {
            continue;
        }
        for (right = left + 1U; right < cluster_count; ++right) {
            if (clusters[right] == NULL) {
                return UCN_ERR_ARGUMENT;
            }
            if (clusters[right]->authority_active &&
                clusters[right]->cluster_id == clusters[left]->cluster_id) {
                violations |=
                    UCN_CLUSTER_INVARIANT_SAFETY_1_SINGLE_AUTHORITY;
            }
        }
    }
    *violation_mask = violations;
    return UCN_OK;
}
