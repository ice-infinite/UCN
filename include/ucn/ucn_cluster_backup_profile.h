#ifndef UCN_CLUSTER_BACKUP_PROFILE_H
#define UCN_CLUSTER_BACKUP_PROFILE_H

/* CLV2-M09 (09-10): strict Backup candidate capability/profile gate.
 *
 * This is a pure selection value model.  It consumes no v4 frame, does not
 * grant Authority and does not select a production Backup by itself.  The
 * capability bit values mirror frozen RFC4 offer bits without enabling the
 * v4 encoder or exposing its private semantic builder.
 */

#include "ucn/ucn_cluster.h"
#include "ucn/ucn_cluster_membership.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_CLUSTER_BACKUP_CAPABILITY_BACKUP ((uint16_t)0x0001U)
#define UCN_CLUSTER_BACKUP_CAPABILITY_JOINT_CONFIG ((uint16_t)0x0004U)
#define UCN_CLUSTER_BACKUP_CAPABILITY_PERSISTENCE ((uint16_t)0x0008U)
#define UCN_CLUSTER_BACKUP_REQUIRED_CAPABILITIES \
    (UCN_CLUSTER_BACKUP_CAPABILITY_BACKUP | \
     UCN_CLUSTER_BACKUP_CAPABILITY_JOINT_CONFIG | \
     UCN_CLUSTER_BACKUP_CAPABILITY_PERSISTENCE)

typedef enum ucn_cluster_backup_profile_reject_reason {
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_NONE = 0,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_NODE_ID = 1,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_RUNTIME_MEMBER = 2,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_MEMBER_ELIGIBILITY = 3,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_HEAD_CAPABLE = 4,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_CORE_ADMITTED = 5,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_COOLDOWN = 6,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_BLACKLISTED = 7,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_WIRE_V4 = 8,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_CAPABILITY = 9,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_MIRROR_CAPACITY = 10,
    UCN_CLUSTER_BACKUP_PROFILE_REJECT_HEAD_SCORE = 11
} ucn_cluster_backup_profile_reject_reason_t;

typedef struct ucn_cluster_backup_candidate_profile {
    ucn_node_id_t node_id;
    uint16_t capabilities;
    uint16_t mirror_member_capacity;
    uint16_t head_score;
    uint8_t wire_version;
    /* These are already-normalized owner inputs.  This pure profile neither
     * queries Core nor changes Member/Authority state. */
    bool runtime_member;
    bool member_eligible;
    bool head_capable;
    bool core_admitted;
    bool backup_cooldown;
    bool blacklisted;
} ucn_cluster_backup_candidate_profile_t;

bool ucn_cluster_backup_candidate_profile_is_strict(
    const ucn_cluster_backup_candidate_profile_t *candidate,
    ucn_cluster_backup_profile_reject_reason_t *reason);

/* Deterministic Target-FSM BackupRank is head_score descending, then valid
 * Node ID ascending, independent of caller input order.  If no candidate
 * qualifies, output stays untouched and the reason records the first failure
 * in ascending Node-ID order. */
ucn_result_t ucn_cluster_backup_candidate_profile_select(
    const ucn_cluster_backup_candidate_profile_t *candidates,
    size_t candidate_count,
    ucn_cluster_backup_candidate_profile_t *output,
    ucn_cluster_backup_profile_reject_reason_t *reason);

#ifdef __cplusplus
}
#endif

#endif
