#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_backup_profile.h"

#define ASSERT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                      \
            (void)fprintf(stderr, "ASSERT %s at %s:%d\\n", #condition,        \
                          __FILE__, __LINE__);                                   \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static ucn_cluster_backup_candidate_profile_t strict_candidate(ucn_node_id_t id)
{
    ucn_cluster_backup_candidate_profile_t candidate;

    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.node_id = id;
    candidate.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    candidate.capabilities = UCN_CLUSTER_BACKUP_REQUIRED_CAPABILITIES;
    candidate.mirror_member_capacity = (uint16_t)UCN_CLUSTER_MAX_MEMBERS;
    candidate.head_score = 100U;
    candidate.runtime_member = true;
    candidate.member_eligible = true;
    candidate.head_capable = true;
    candidate.core_admitted = true;
    return candidate;
}

static int test_strict_rejection_reasons(void)
{
    ucn_cluster_backup_candidate_profile_t candidate = strict_candidate(7U);
    ucn_cluster_backup_profile_reject_reason_t reason;

    ASSERT_TRUE(ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_NONE);
    candidate.runtime_member = false;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_RUNTIME_MEMBER);
    candidate = strict_candidate(7U);
    candidate.member_eligible = false;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_MEMBER_ELIGIBILITY);
    candidate = strict_candidate(7U);
    candidate.head_capable = false;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_HEAD_CAPABLE);
    candidate = strict_candidate(7U);
    candidate.core_admitted = false;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_CORE_ADMITTED);
    candidate = strict_candidate(7U);
    candidate.backup_cooldown = true;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_COOLDOWN);
    candidate = strict_candidate(7U);
    candidate.blacklisted = true;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_BLACKLISTED);
    candidate = strict_candidate(7U);
    candidate.head_score = (uint16_t)(UCN_CLUSTER_SCORE_MAX + 1U);
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_HEAD_SCORE);
    candidate = strict_candidate(7U);
    candidate.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V3;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_WIRE_V4);
    candidate = strict_candidate(7U);
    candidate.capabilities &= (uint16_t)~UCN_CLUSTER_BACKUP_CAPABILITY_PERSISTENCE;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_CAPABILITY);
    candidate = strict_candidate(7U);
    candidate.mirror_member_capacity--;
    ASSERT_TRUE(!ucn_cluster_backup_candidate_profile_is_strict(&candidate,
                                                                 &reason));
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_MIRROR_CAPACITY);
    return 0;
}

static int test_selection_is_deterministic_and_no_write_on_failure(void)
{
    ucn_cluster_backup_candidate_profile_t candidates[4U];
    ucn_cluster_backup_candidate_profile_t selected;
    ucn_cluster_backup_candidate_profile_t sentinel;
    ucn_cluster_backup_profile_reject_reason_t reason;

    candidates[0U] = strict_candidate(9U);
    candidates[1U] = strict_candidate(3U);
    candidates[1U].wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V3;
    candidates[2U] = strict_candidate(7U);
    candidates[3U] = strict_candidate(5U);
    candidates[0U].head_score = 100U;
    candidates[2U].head_score = 200U;
    candidates[3U].head_score = 200U;
    ASSERT_TRUE(ucn_cluster_backup_candidate_profile_select(candidates, 4U,
                                                             &selected,
                                                             &reason) == UCN_OK);
    ASSERT_TRUE(selected.node_id == 5U);
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_NONE);

    candidates[0U].wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V3;
    candidates[2U].capabilities = 0U;
    candidates[3U].mirror_member_capacity--;
    (void)memset(&sentinel, 0xA5, sizeof(sentinel));
    selected = sentinel;
    ASSERT_TRUE(ucn_cluster_backup_candidate_profile_select(candidates, 4U,
                                                             &selected,
                                                             &reason) ==
                UCN_ERR_NOT_FOUND);
    ASSERT_TRUE(memcmp(&selected, &sentinel, sizeof(selected)) == 0);
    ASSERT_TRUE(reason == UCN_CLUSTER_BACKUP_PROFILE_REJECT_WIRE_V4);
    return 0;
}

int main(void)
{
    ASSERT_TRUE(test_strict_rejection_reasons() == 0);
    ASSERT_TRUE(test_selection_is_deterministic_and_no_write_on_failure() == 0);
    (void)puts("All Cluster Backup profile tests passed");
    return 0;
}
