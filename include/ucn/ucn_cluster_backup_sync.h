#ifndef UCN_CLUSTER_BACKUP_SYNC_H
#define UCN_CLUSTER_BACKUP_SYNC_H

/* CLV2-M09 (09-03): isolated Backup full-snapshot receiver owner.
 *
 * It is deliberately wire-agnostic: a future v4 RX adapter must first pass
 * decoded fields to this owner, and M10 remains the sole owner of any
 * Takeover decision.  The object accepts a full snapshot only from its
 * assigned Head, for its exact BackupEpoch and currently frozen Config.
 */

#include "ucn/ucn_cluster_backup_mirror.h"

#ifndef UCN_CLUSTER_BACKUP_COVERAGE_GRACE_MS
#define UCN_CLUSTER_BACKUP_COVERAGE_GRACE_MS UINT32_C(2000)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_cluster_backup_sync_owner {
    ucn_cluster_backup_mirror_t mirror;
    ucn_cluster_backup_epoch_t assigned_epoch;
    ucn_cluster_config_state_t active_config;
    uint32_t next_member_sequence;
    uint32_t received_member_count;
    uint32_t running_snapshot_hash;
    uint32_t committed_final_sequence;
    uint32_t committed_snapshot_hash;
    uint32_t next_delta_sequence;
    uint32_t coverage_grace_deadline_ms;
    bool coverage_grace_armed;
    bool takeover_ineligible;
    bool resync_required;
    bool initialized;
} ucn_cluster_backup_sync_owner_t;

typedef enum ucn_cluster_backup_peer_state {
    UCN_CLUSTER_BACKUP_PEER_NONE = 0,
    UCN_CLUSTER_BACKUP_PEER_ADMITTED = 1,
    UCN_CLUSTER_BACKUP_PEER_SUSPECT = 2,
    UCN_CLUSTER_BACKUP_PEER_REMOVED = 3
} ucn_cluster_backup_peer_state_t;

typedef struct ucn_cluster_backup_coverage_entry {
    ucn_node_id_t node_id;
    uint8_t state;
} ucn_cluster_backup_coverage_entry_t;

/* Fixed Core-peer view supplied by a future adapter/owner.  Entries are
 * sorted by Node ID; all tail entries are zero.  M09 intentionally requires
 * every voter in the active Stable/Joint Config to be ADMITTED on initial
 * sync.  Non-voters and provisional legacy members do not participate in
 * this first-ready predicate. */
typedef struct ucn_cluster_backup_coverage {
    ucn_cluster_backup_coverage_entry_t entries[UCN_CLUSTER_MAX_VOTERS];
    uint8_t count;
} ucn_cluster_backup_coverage_t;

/* The Head-side acceptance view of a decoded BACKUP_READY.  SnapshotEpoch
 * already carries cluster/term/head/backup/generation and exact Config ref;
 * this record adds the claimed sender and the final sync proof. */
typedef struct ucn_cluster_backup_ready {
    ucn_node_id_t source_node_id;
    ucn_cluster_snapshot_epoch_t snapshot_epoch;
    uint32_t final_sequence;
    uint32_t snapshot_hash;
} ucn_cluster_backup_ready_t;

/* A Delta never changes SnapshotEpoch.  It identifies its exact committed
 * base by both SnapshotEpoch and prior hash; the resulting hash is checked
 * before the committed mirror is replaced. */
typedef struct ucn_cluster_backup_delta {
    ucn_cluster_snapshot_epoch_t snapshot_epoch;
    uint32_t sequence;
    uint32_t previous_snapshot_hash;
    uint32_t resulting_snapshot_hash;
    ucn_cluster_member_t member;
} ucn_cluster_backup_delta_t;

/* Initializes or deliberately replaces one local Backup assignment.  The
 * function never reads the prior owner storage, so a caller may pass fresh
 * uninitialised storage; a replacement atomically discards its old local
 * mirror rather than carrying it into a different BackupEpoch.  Output stays
 * untouched on invalid input. */
ucn_result_t ucn_cluster_backup_sync_owner_init(
    ucn_cluster_backup_sync_owner_t *owner,
    const ucn_cluster_backup_epoch_t *assigned_epoch,
    const ucn_cluster_config_state_t *active_config);

bool ucn_cluster_backup_sync_owner_is_valid(
    const ucn_cluster_backup_sync_owner_t *owner);

/* Receives a decoded SYNC_BEGIN.  `begin_sequence` is the decoded control
 * sequence and must be exactly zero; Member records subsequently start at
 * one and SYNC_END uses N+1.  The source must be the assigned Head, the
 * supplied SnapshotEpoch must bind the exact assignment and active Config.
 * On all failures the entire owner remains byte-for-byte unchanged.  A valid
 * begin only clears/opens staging; it never touches committed mirror data. */
ucn_result_t ucn_cluster_backup_sync_owner_begin(
    ucn_cluster_backup_sync_owner_t *owner,
    ucn_node_id_t source_node_id,
    uint32_t begin_sequence,
    const ucn_cluster_snapshot_epoch_t *snapshot_epoch);

/* Local timeout/reject cleanup.  It discards only active staging plus its
 * sequence/hash work state; the last committed mirror and Delta base remain
 * available for safe Recovery/next full sync. */
ucn_result_t ucn_cluster_backup_sync_owner_abort(
    ucn_cluster_backup_sync_owner_t *owner);

bool ucn_cluster_backup_coverage_is_valid(
    const ucn_cluster_backup_coverage_t *coverage);
bool ucn_cluster_backup_coverage_initial_ready(
    const ucn_cluster_config_state_t *config,
    const ucn_cluster_backup_coverage_t *coverage);

/* Deterministic, padding-free FNV-1a accumulator for the strict on-wire
 * member record order.  It returns zero for an invalid/non-occupied member;
 * callers must never hash raw C object bytes. */
uint32_t ucn_cluster_backup_sync_member_hash_update(
    uint32_t hash, const ucn_cluster_member_t *member);

/* Strict full-sync records.  Member `sequence` starts at 1 for each
 * SnapshotEpoch; only an exact next value is accepted.  The END control
 * sequence must be exactly N+1 (including 1 for an empty snapshot).  END
 * atomically swaps staging only after count/hash/coverage all verify. */
ucn_result_t ucn_cluster_backup_sync_owner_member(
    ucn_cluster_backup_sync_owner_t *owner,
    ucn_node_id_t source_node_id,
    const ucn_cluster_snapshot_epoch_t *snapshot_epoch,
    uint32_t sequence,
    const ucn_cluster_member_t *member);
ucn_result_t ucn_cluster_backup_sync_owner_end(
    ucn_cluster_backup_sync_owner_t *owner,
    ucn_node_id_t source_node_id,
    const ucn_cluster_snapshot_epoch_t *snapshot_epoch,
    uint32_t final_sequence,
    uint32_t member_count,
    uint32_t snapshot_hash,
    const ucn_cluster_backup_coverage_t *coverage);

/* Pure Head-side readiness check.  It never changes owner state: a delayed
 * READY only matches the exact currently committed SnapshotEpoch, final
 * sequence and hash, and must originate from the assigned Backup. */
ucn_result_t ucn_cluster_backup_sync_owner_verify_ready(
    const ucn_cluster_backup_sync_owner_t *owner,
    const ucn_cluster_backup_ready_t *ready);

/* Delta only updates the exact committed SnapshotEpoch/Config and an existing
 * committed member's dynamic freshness fields (nonce/lease/keepalive).  It
 * cannot add a member, change any static membership/eligibility field or
 * lower that member's nonce.  A sequence gap preserves the committed mirror,
 * sets resync_required and rejects the delta; stale deltas are rejected
 * without changing any state. */
ucn_result_t ucn_cluster_backup_sync_owner_delta(
    ucn_cluster_backup_sync_owner_t *owner,
    ucn_node_id_t source_node_id,
    const ucn_cluster_backup_delta_t *delta);
bool ucn_cluster_backup_sync_owner_resync_required(
    const ucn_cluster_backup_sync_owner_t *owner);

/* After an initially covered committed mirror, a protected voter transitioning
 * to SUSPECT arms a bounded grace timer.  Recovery during grace preserves
 * eligibility; prolonged SUSPECT permanently marks this assignment ineligible.
 * A Core-confirmed protected-voter REMOVED state, or a missing protected-voter
 * coverage entry, marks it ineligible immediately; neither may borrow the
 * SUSPECT grace.  A future Head owner must select a new Backup. */
ucn_result_t ucn_cluster_backup_sync_owner_update_coverage(
    ucn_cluster_backup_sync_owner_t *owner,
    const ucn_cluster_backup_coverage_t *coverage,
    uint32_t now_ms);
bool ucn_cluster_backup_sync_owner_takeover_eligible(
    const ucn_cluster_backup_sync_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif
