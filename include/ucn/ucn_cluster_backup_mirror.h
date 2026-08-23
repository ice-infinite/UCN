#ifndef UCN_CLUSTER_BACKUP_MIRROR_H
#define UCN_CLUSTER_BACKUP_MIRROR_H

/* CLV2-M09 (09-01/09-02): Backup mirror value model.
 *
 * This header owns only fixed-capacity mirror storage.  It has no wire,
 * Authority, persistence, vote or takeover behaviour.  A future snapshot
 * owner must validate its complete SnapshotEpoch/Config/Coverage proof before
 * it may atomically replace committed_members; that operation is deliberately
 * not exposed by 09-01/09-02.
 */

#include <stdbool.h>

#include "ucn/ucn_cluster_config_state.h"
#include "ucn/ucn_cluster_membership.h"
#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exact BackupEpoch from the target Cluster FSM: Snapshot identity is not
 * merely a sequence number.  It is scoped by the Cluster authority, selected
 * Backup and monotonically allocated generation. */
typedef struct ucn_cluster_backup_epoch {
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
    ucn_node_id_t backup_node_id;
    uint32_t backup_generation;
} ucn_cluster_backup_epoch_t;

/* SnapshotEpoch = BackupEpoch + snapshot_id, with a compact, exact binding
 * to the canonical active Config.  The Config body is intentionally not
 * duplicated in every mirror: config_id/phase/hash uniquely bind it and the
 * owner must compare them against its frozen Config before accepting traffic.
 */
typedef struct ucn_cluster_snapshot_epoch {
    ucn_cluster_backup_epoch_t backup_epoch;
    uint32_t snapshot_id;
    uint32_t config_id;
    uint32_t config_hash;
    uint8_t config_phase;
} ucn_cluster_snapshot_epoch_t;

typedef struct ucn_cluster_backup_mirror {
    /* Last fully verified mirror.  SYNC_BEGIN and invalid staging traffic
     * must never write this table. */
    ucn_cluster_member_table_t committed_members;
    /* Isolated work area for the next full snapshot. */
    ucn_cluster_member_table_t staging_members;
    /* Metadata is canonical byte-zero whenever its corresponding table is
     * not valid/active.  A partial or stale SnapshotEpoch therefore cannot be
     * mistaken for committed input by a future M10 owner. */
    ucn_cluster_snapshot_epoch_t committed_epoch;
    ucn_cluster_snapshot_epoch_t staging_epoch;
    bool committed_valid;
    bool staging_active;
} ucn_cluster_backup_mirror_t;

/* The eventual Cluster role storage is a union: a Head/Member needs one
 * Runtime table, while a Backup needs a committed+staging pair.  Keeping the
 * type explicit lets M09 size tests prove there is no Head Runtime table plus
 * a second, permanently allocated Backup pair.  Integration into
 * ucn_cluster_t is deferred until the strict snapshot owner (09-03/09-04),
 * so the old v3 receiver cannot accidentally gain new authority semantics. */
typedef union ucn_cluster_member_role_storage {
    ucn_cluster_member_table_t runtime_members;
    ucn_cluster_backup_mirror_t backup_mirror;
} ucn_cluster_member_role_storage_t;

typedef char ucn_cluster_backup_role_storage_must_fit_backup_state[
    sizeof(ucn_cluster_member_role_storage_t) ==
            sizeof(ucn_cluster_backup_mirror_t) ? 1 : -1];

/* Canonical reset: both tables become byte-zero, no committed snapshot and no
 * open staging transaction. */
void ucn_cluster_backup_mirror_reset(ucn_cluster_backup_mirror_t *mirror);

/* Validity includes canonical empty storage whenever the corresponding valid
 * or active flag is false.  This makes stale staging bytes diagnosable and
 * prevents a future owner from treating an unproved table as committed. */
bool ucn_cluster_backup_mirror_is_valid(
    const ucn_cluster_backup_mirror_t *mirror);

bool ucn_cluster_backup_epoch_is_valid(
    const ucn_cluster_backup_epoch_t *epoch);
bool ucn_cluster_backup_epoch_is_exact(
    const ucn_cluster_backup_epoch_t *left,
    const ucn_cluster_backup_epoch_t *right);

/* Build an exact SnapshotEpoch from a canonical Config.  Output remains
 * untouched on invalid input. */
bool ucn_cluster_snapshot_epoch_from_config(
    ucn_cluster_snapshot_epoch_t *output,
    const ucn_cluster_backup_epoch_t *backup_epoch,
    uint32_t snapshot_id,
    const ucn_cluster_config_state_t *config);
bool ucn_cluster_snapshot_epoch_is_valid(
    const ucn_cluster_snapshot_epoch_t *epoch);
bool ucn_cluster_snapshot_epoch_is_exact(
    const ucn_cluster_snapshot_epoch_t *left,
    const ucn_cluster_snapshot_epoch_t *right);
bool ucn_cluster_snapshot_epoch_matches_config(
    const ucn_cluster_snapshot_epoch_t *epoch,
    const ucn_cluster_config_state_t *config);

/* Reserved serial values remain readable for diagnosis, but cannot begin a
 * new snapshot.  Snapshot exhaustion requires a new Backup generation; a
 * generation at the same boundary requires M13 Rekey rather than wrap. */
bool ucn_cluster_snapshot_epoch_rotation_required(
    const ucn_cluster_snapshot_epoch_t *epoch);
bool ucn_cluster_backup_epoch_rekey_required(
    const ucn_cluster_backup_epoch_t *epoch);
/* Allocates the next Backup generation without wrapping.  A caller then
 * starts a new full snapshot at snapshot_id=1 through a fresh assignment
 * owner.  At the rotation boundary this fails closed and requires M13 Rekey. */
ucn_result_t ucn_cluster_backup_epoch_next_generation(
    ucn_cluster_backup_epoch_t *output,
    const ucn_cluster_backup_epoch_t *current);

/* Open/discard a new staging transaction.  begin only clears staging and
 * deliberately preserves committed_members/committed_valid.  An active
 * transaction is never overwritten.  When a committed SnapshotEpoch exists,
 * the candidate must use the exact same BackupEpoch and a strictly newer
 * snapshot_id; a new BackupEpoch requires an explicit owner reset/reassign.
 */
ucn_result_t ucn_cluster_backup_mirror_begin_staging(
    ucn_cluster_backup_mirror_t *mirror,
    const ucn_cluster_snapshot_epoch_t *snapshot_epoch);
ucn_result_t ucn_cluster_backup_mirror_abort_staging(
    ucn_cluster_backup_mirror_t *mirror);

/* The sync owner may call this only after it has proven sequence/count/hash,
 * Config and coverage.  The operation is one local atomic value replacement:
 * either the previous committed mirror survives unchanged, or the complete
 * staging table plus its exact SnapshotEpoch becomes committed. */
ucn_result_t ucn_cluster_backup_mirror_commit_staging_exact(
    ucn_cluster_backup_mirror_t *mirror,
    const ucn_cluster_snapshot_epoch_t *expected_epoch);

const ucn_cluster_member_table_t *ucn_cluster_backup_mirror_committed(
    const ucn_cluster_backup_mirror_t *mirror);
ucn_cluster_member_table_t *ucn_cluster_backup_mirror_staging(
    ucn_cluster_backup_mirror_t *mirror);
const ucn_cluster_snapshot_epoch_t *ucn_cluster_backup_mirror_committed_epoch(
    const ucn_cluster_backup_mirror_t *mirror);
const ucn_cluster_snapshot_epoch_t *ucn_cluster_backup_mirror_staging_epoch(
    const ucn_cluster_backup_mirror_t *mirror);

#ifdef __cplusplus
}
#endif

#endif
