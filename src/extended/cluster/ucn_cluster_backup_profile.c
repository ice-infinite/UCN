/* CLV2-M09 (09-10): pure strict Backup capability/profile gate. */

#include "ucn/ucn_cluster_backup_profile.h"

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

bool ucn_cluster_backup_candidate_profile_is_strict(
    const ucn_cluster_backup_candidate_profile_t *candidate,
    ucn_cluster_backup_profile_reject_reason_t *reason)
{
    ucn_cluster_backup_profile_reject_reason_t local_reason =
        UCN_CLUSTER_BACKUP_PROFILE_REJECT_NONE;

    if (candidate == NULL || !node_id_is_valid(candidate->node_id)) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_NODE_ID;
    } else if (!candidate->runtime_member) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_RUNTIME_MEMBER;
    } else if (!candidate->member_eligible) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_MEMBER_ELIGIBILITY;
    } else if (!candidate->head_capable) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_HEAD_CAPABLE;
    } else if (!candidate->core_admitted) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_CORE_ADMITTED;
    } else if (candidate->backup_cooldown) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_COOLDOWN;
    } else if (candidate->blacklisted) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_BLACKLISTED;
    } else if (candidate->head_score > UCN_CLUSTER_SCORE_MAX) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_HEAD_SCORE;
    } else if (candidate->wire_version != UCN_CLUSTER_MEMBER_WIRE_VERSION_V4) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_WIRE_V4;
    } else if ((candidate->capabilities & UCN_CLUSTER_BACKUP_REQUIRED_CAPABILITIES) !=
               UCN_CLUSTER_BACKUP_REQUIRED_CAPABILITIES) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_CAPABILITY;
    } else if ((size_t)candidate->mirror_member_capacity <
               UCN_CLUSTER_MAX_MEMBERS) {
        local_reason = UCN_CLUSTER_BACKUP_PROFILE_REJECT_MIRROR_CAPACITY;
    }
    if (reason != NULL) {
        *reason = local_reason;
    }
    return local_reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_NONE;
}

ucn_result_t ucn_cluster_backup_candidate_profile_select(
    const ucn_cluster_backup_candidate_profile_t *candidates,
    size_t candidate_count,
    ucn_cluster_backup_candidate_profile_t *output,
    ucn_cluster_backup_profile_reject_reason_t *reason)
{
    size_t index;
    size_t selected_index = candidate_count;
    size_t first_failure_index = candidate_count;
    ucn_cluster_backup_profile_reject_reason_t selected_reason =
        UCN_CLUSTER_BACKUP_PROFILE_REJECT_NONE;
    ucn_cluster_backup_profile_reject_reason_t first_failure_reason =
        UCN_CLUSTER_BACKUP_PROFILE_REJECT_NODE_ID;

    if (candidates == NULL || candidate_count == 0U || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < candidate_count; ++index) {
        ucn_cluster_backup_profile_reject_reason_t candidate_reason;
        bool strict = ucn_cluster_backup_candidate_profile_is_strict(
            &candidates[index], &candidate_reason);

        if (strict &&
            (selected_index == candidate_count ||
             candidates[index].head_score > candidates[selected_index].head_score ||
             (candidates[index].head_score == candidates[selected_index].head_score &&
              candidates[index].node_id < candidates[selected_index].node_id))) {
            selected_index = index;
            selected_reason = candidate_reason;
        }
        if (!strict &&
            (first_failure_index == candidate_count ||
             candidates[index].node_id < candidates[first_failure_index].node_id)) {
            first_failure_index = index;
            first_failure_reason = candidate_reason;
        }
    }
    if (selected_index == candidate_count) {
        if (reason != NULL) {
            *reason = first_failure_reason;
        }
        return UCN_ERR_NOT_FOUND;
    }
    *output = candidates[selected_index];
    if (reason != NULL) {
        *reason = selected_reason;
    }
    return UCN_OK;
}
