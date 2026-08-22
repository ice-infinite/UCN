#ifndef UCN_CLUSTER_MEMBERSHIP_H
#define UCN_CLUSTER_MEMBERSHIP_H

/* CLV2-M06 (06-01): bounded Cluster member value model.
 *
 * This header deliberately models membership data only.  It neither includes
 * the v4 wire header nor grants a member any Cluster authority.  In a
 * production build M06 records legacy v3 producer paths as bounded,
 * non-voting provisional members.  Test-only bridges may preserve older
 * Current-FSM fixtures, while later milestones own Config Commit, quorum and
 * any v4 RX/TX/FSM integration. */

#include "ucn/ucn_config.h"
#include "ucn/ucn_types.h"

/* Keep this public value-model header independently includable when a
 * configuration-contract build suppresses ucn_config.h defaults. */
#ifndef UCN_CLUSTER_MAX_MEMBERS
#define UCN_CLUSTER_MAX_MEMBERS ((size_t)16U)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_CLUSTER_MEMBER_WIRE_VERSION_V3 ((uint8_t)3U)
#define UCN_CLUSTER_MEMBER_WIRE_VERSION_V4 ((uint8_t)4U)

/* A protected voter configuration always includes its Head.  The Runtime
 * member table holds remote members only, so its maximum needs one extra
 * position here.  M06 deliberately uses a 64-bit logical bitmap: the
 * existing v3 takeover bitmap remains a separate legacy mechanism until M10
 * replaces it with a configuration-bound certificate. */
#define UCN_CLUSTER_MAX_VOTERS (UCN_CLUSTER_MAX_MEMBERS + (size_t)1U)
typedef char ucn_cluster_voters_must_fit_u64_bitmap[
    UCN_CLUSTER_MAX_VOTERS >= (size_t)1U &&
            UCN_CLUSTER_MAX_VOTERS <= (size_t)64U ? 1 : -1];

typedef enum ucn_cluster_member_status {
    UCN_CLUSTER_MEMBER_STATUS_NONE = 0,
    UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL = 1,
    UCN_CLUSTER_MEMBER_STATUS_COMMITTED = 2,
    UCN_CLUSTER_MEMBER_STATUS_REMOVING = 3
} ucn_cluster_member_status_t;

/* `status` remains a byte instead of a C enum so the per-member table stays
 * compact on MCUs where an enum has int width.  Its value is always one of
 * ucn_cluster_member_status_t and is checked through record_is_valid(). */
typedef struct ucn_cluster_member {
    bool occupied;
    bool voting;
    bool provisional_deadline_armed;
    uint8_t status;
    uint8_t wire_version;
    uint16_t capabilities;
    ucn_node_id_t node_id;
    uint32_t lease_expires_at_ms;
    uint32_t last_nonce;
    uint32_t joined_at_ms;
    uint32_t last_keepalive_at_ms;
    uint32_t provisional_deadline_ms;
} ucn_cluster_member_t;

/* CLV2-M06 (06-02): fixed-capacity member-table storage.  The owning
 * ucn_cluster_t names its current primary table explicitly so a Head's
 * Runtime table and a Backup's committed mirror are no longer represented
 * by an unqualified `members[]` array.  This table has no implicit voter,
 * quorum, authority, staging, or Wire-v4 semantics. */
typedef struct ucn_cluster_member_table {
    ucn_cluster_member_t slots[UCN_CLUSTER_MAX_MEMBERS];
} ucn_cluster_member_table_t;

/* CLV2-M06 (06-03): compact canonical representation of one protected voter
 * set.  `node_ids[0..count)` is strictly ascending and contains no invalid
 * or duplicate node ID; the remaining entries are zero.  `config_id` is a
 * caller-owned identity (zero is permitted for the pre-M07 legacy bridge),
 * while `hash` is the deterministic FNV-1a fingerprint of config_id, count
 * and sorted node IDs.  It is a local consistency aid, not a wire
 * certificate and not an Authority decision. */
typedef struct ucn_cluster_voter_set {
    ucn_node_id_t node_ids[UCN_CLUSTER_MAX_VOTERS];
    uint32_t config_id;
    uint32_t hash;
    uint8_t count;
} ucn_cluster_voter_set_t;

/* Detailed owner-side admission outcomes.  They make capacity rejection
 * diagnosable without conflating Runtime table pressure with the future
 * protected-voter configuration limit. */
typedef enum ucn_cluster_member_admission_reason {
    UCN_CLUSTER_MEMBER_ADMISSION_NONE = 0,
    UCN_CLUSTER_MEMBER_ADMISSION_ARGUMENT = 1,
    UCN_CLUSTER_MEMBER_ADMISSION_NOT_HEAD = 2,
    UCN_CLUSTER_MEMBER_ADMISSION_RUNTIME_CAPACITY = 3,
    UCN_CLUSTER_MEMBER_ADMISSION_VOTER_CAPACITY = 4,
    UCN_CLUSTER_MEMBER_ADMISSION_MEMBER_CONFLICT = 5,
    UCN_CLUSTER_MEMBER_ADMISSION_CONFIG_UNAVAILABLE = 6
} ucn_cluster_member_admission_reason_t;

bool ucn_cluster_member_status_is_valid(ucn_cluster_member_status_t status);
bool ucn_cluster_member_transition_is_valid(
    ucn_cluster_member_status_t previous,
    ucn_cluster_member_status_t next);
bool ucn_cluster_member_record_is_valid(const ucn_cluster_member_t *member);
bool ucn_cluster_member_table_is_valid(
    const ucn_cluster_member_table_t *table);
size_t ucn_cluster_member_table_count(
    const ucn_cluster_member_table_t *table);

bool ucn_cluster_voter_set_is_valid(const ucn_cluster_voter_set_t *set);
bool ucn_cluster_voter_set_build(ucn_cluster_voter_set_t *output,
                                 uint32_t config_id,
                                 const ucn_node_id_t *node_ids,
                                 size_t count);
bool ucn_cluster_voter_set_contains(const ucn_cluster_voter_set_t *set,
                                    ucn_node_id_t node_id);
uint8_t ucn_cluster_voter_set_quorum(const ucn_cluster_voter_set_t *set);
bool ucn_cluster_voter_set_bitmap_for_node(const ucn_cluster_voter_set_t *set,
                                           ucn_node_id_t node_id,
                                           uint64_t *bitmap);

#ifdef __cplusplus
}
#endif

#endif
