/* UCN CLV2-M02 (02-05): Cluster Backup + Takeover module.
 *
 * STRUCTURAL REFACTOR ONLY (M02 mandate): the Backup selection /
 * assignment / snapshot / delta / heartbeat / reject / resync and the
 * Takeover prepare / ACK / complete lifecycle moved verbatim from the
 * former single ucn_cluster.c.  Every function body is UNCHANGED; M01
 * (human sign-off ab53b31/a7f4841) froze the FSM semantics including
 * the M01.0.2 takeover_active && backup_syncing combination.  Do NOT
 * "optimize" anything here.
 *
 * Cross-module (via ucn_cluster_internal.h, all de-static only):
 *   - calls into fsm:      cluster_transition / preflight /
 *     cluster_phase_from_legacy_state / cluster_now / cluster_shadow_sync
 *   - calls into membership: member_count_u16 / clear_members
 *   - calls into ucn_cluster.c core: set_detached / find_peer /
 *     send_cluster_message / next_nonce
 */

#include "ucn/ucn_cluster.h"

#include <assert.h>
#include <string.h>

#include "ucn/ucn_time.h"

#include "ucn_cluster_internal.h"


ucn_cluster_member_t *backup_allocate_mirror(ucn_cluster_t *cluster,
                                                      ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied &&
            cluster->members[index].node_id == node_id) {
            return &cluster->members[index];
        }
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (!cluster->members[index].occupied) {
            (void)memset(&cluster->members[index], 0,
                         sizeof(cluster->members[index]));
            cluster->members[index].occupied = true;
            cluster->members[index].node_id = node_id;
            return &cluster->members[index];
        }
    }
    return NULL;
}

/* C07.2 coverage gate: the Backup must reach every active mirrored Member
 * over its own one-hop admitted peers, without passing through the
 * Primary Head. */
bool backup_covers_all_members(const ucn_cluster_t *cluster)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (!cluster->members[index].occupied) {
            continue;
        }
        if (cluster->members[index].node_id == cluster->config.local_node_id) {
            continue; /* The Backup reaches itself trivially. */
        }
        {
            /* C07.7 P2: a SUSPECT neighbour does not count as coverage;
             * only a healthy ADMITTED one-hop link does. */
            const ucn_cluster_peer_t *peer =
                find_peer(cluster, cluster->members[index].node_id);

            if (peer == NULL ||
                peer->neighbor_state != UCN_NEIGHBOR_ADMITTED) {
                return false;
            }
        }
    }
    return true;
}

void backup_clear_sync(ucn_cluster_t *cluster, uint32_t now_ms)
{
    cluster->backup_syncing = false;
    cluster->backup_ready = false;
    cluster->backup_primary_node_id = 0U;
    cluster->backup_generation = 0U;
    cluster->membership_sequence = 0U;
    cluster->backup_primary_deadline_ms = 0U;
    cluster->backup_missed_heartbeats = 0U;
    (void)clear_members(cluster);
    set_detached(cluster, now_ms, cluster->config.recovery_observation_ms);
}

ucn_result_t send_backup_assign(
    ucn_cluster_t *cluster, ucn_node_id_t destination)
{
    ucn_cluster_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->config.local_node_id;
    message.backup_generation = cluster->backup_generation;
    /* Type 10 v3 carries the selected Backup ID so every Member can validate
     * a later TAKEOVER_PREPARE instead of trusting its sender assertion. */
    message.sync_token = cluster->backup_node_id;
    return send_cluster_message(cluster, destination, &message);
}

/* Head: select the best currently advertised head-capable Member. */
void assign_backup(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    const ucn_cluster_candidate_t *best = NULL;
    ucn_node_id_t best_node_id = 0U;
    uint32_t next_generation;

    if (cluster->backup_node_id != 0U) {
        return;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied) {
            const ucn_cluster_candidate_t *candidate;

            /* Only a head-capable member (one that advertised as a candidate)
             * may become Backup; skip otherwise.  A candidate that recently
             * rejected the assignment cools down before it is retried. */
            candidate = find_candidate(cluster, cluster->members[index].node_id);
            if (candidate == NULL ||
                (cluster->backup_candidate_cooldown_until_ms != 0U &&
                 !ucn_deadline_expired(now_ms,
                                       cluster->backup_candidate_cooldown_until_ms) &&
                 cluster->members[index].node_id ==
                     cluster->backup_rejected_node_id)) {
                continue;
            }
            if (best == NULL || candidate->head_score > best->head_score ||
                (candidate->head_score == best->head_score &&
                 cluster->members[index].node_id < best_node_id)) {
                best = candidate;
                best_node_id = cluster->members[index].node_id;
            }
        }
    }
    if (best == NULL) {
        cluster->backup_node_id = 0U;
        return;
    }
    if (cluster_serial_next_checked(cluster->backup_generation,
                                    &next_generation) != UCN_OK) {
        /* M13 will Rekey the Cluster.  Until then, leave the Head without a
         * newly assigned Backup rather than reuse an old generation. */
        return;
    }
    /* CLV2-01-04d.1 + CLV2-01-04d.7.1 (shadow-guard closure): the
     * selection commits as the NO_BACKUP -> ASSIGNING transition BEFORE
     * the phase-relevant node_id write (apply_legacy owns the role write
     * and arms assign_pending, so once the site writes node_id the derive
     * IS ASSIGNING - never a bogus SYNCING that would make the end-of-step
     * sync mint a new ASSIGNING->SYNCING pair).  SHADOW-GUARD RULE:
     * legacy/event decides WHICH transition should happen (on entry
     * node_id == 0, so legacy derives HEAD_NO_BACKUP); cluster_transition()/
     * preflight validates whether Shadow agrees.  The transition is called
     * UNCONDITIONALLY - never skipped because shadow_phase != NO_BACKUP: a
     * shadow-desync must fail-closed here (nothing committed) instead of
     * silently falling back to the legacy bool + end-of-step shadow_sync()
     * minting.  The no-candidate early-return above (node_id stays 0 ->
     * derive still NO_BACKUP) never reaches this call. */
    if (cluster_transition(cluster, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                           UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                           UCN_CLUSTER_REASON_BACKUP_ASSIGNED,
                           now_ms) != UCN_OK) {
        /* Fail closed: a rejected transition (shadow mismatch / illegal
         * pair / pre-mutated phase fields) leaves every field untouched -
         * do NOT commit the selection; the next step re-visits it. */
        return;
    }
    cluster->backup_node_id = best_node_id;
    cluster->backup_generation = next_generation;
    cluster->backup_ready = false;
    cluster->membership_sequence = 0U;
    cluster->backup_sync_cursor = 0U;
    start_backup_assignment_cycle(cluster, now_ms);
    cluster->next_backup_heartbeat_ms = now_ms;
    cluster->next_backup_sync_ms = now_ms;
    cluster->backup_resync_deadline_ms = ucn_deadline_from_now(
        now_ms, cluster->config.lease_ms);
#if !defined(NDEBUG)
    /* CLV2-01-04d.1 post-commit derive assert: after the transition AND
     * every site effect (node_id, generation, ready=false, cycle arming
     * pending) the legacy state must still derive ASSIGNING.  At least one
     * occupied member + valid candidate exists here (best != NULL), so
     * member_count >= 1 and the cycle keeps assign_pending == true. */
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
#endif
}

ucn_result_t handle_backup_assign(ucn_cluster_t *cluster,
                                  ucn_node_id_t source,
                                  const ucn_cluster_message_t *message,
                                  uint32_t now_ms)
{
    ucn_cluster_phase_t old_phase;

    {
        ucn_node_id_t expected_head = 0U;

        if (cluster->role == UCN_CLUSTER_ROLE_MEMBER) {
            expected_head = cluster->head_node_id;
        } else if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING) {
            expected_head = cluster->pending_head_node_id;
        } else if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
            expected_head = cluster->backup_primary_node_id;
        } else {
            return UCN_ERR_ACCESS;
        }
        if (source != expected_head || message->head_node_id != source ||
            message->sync_token == 0U ||
            message->sync_token == UCN_NODE_BROADCAST ||
            message->backup_generation == 0U ||
            message->backup_generation >
                UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
            return UCN_ERR_ACCESS;
        }
        /* An assignment is authority of the receiver's CURRENT epoch, not
         * a general-purpose epoch switch.  The normal Head-offer classifier
         * must remain the only route that changes a Member or pending Join to
         * another Cluster identity. */
        if ((cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING &&
             (message->cluster_id != cluster->pending_cluster_id ||
              message->term != cluster->pending_term)) ||
            (cluster->role != UCN_CLUSTER_ROLE_JOIN_PENDING &&
             (message->cluster_id != cluster->cluster_id ||
              message->term != cluster->term))) {
            cluster->stats.stale_messages++;
            return UCN_ERR_REPLAY;
        }
    }
    /* Every accepted recipient learns the protected assignment record.  Do
     * not write it before the self-assignment transition: a shadow rejection
     * must be an all-or-nothing Assignment failure. */
    if (message->sync_token != cluster->config.local_node_id) {
        cluster->known_backup_node_id = message->sync_token;
        cluster->known_backup_generation = message->backup_generation;
        return UCN_OK;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
        if (message->backup_generation != cluster->backup_generation) {
            cluster->stats.stale_messages++;
            return UCN_ERR_REPLAY;
        }
        cluster->known_backup_node_id = message->sync_token;
        cluster->known_backup_generation = message->backup_generation;
        return UCN_OK;
    }
    if (!cluster->config.head_capable) {
        return UCN_ERR_UNSUPPORTED;
    }
    /* CLV2-01-04e.1: the BACKUP_ASSIGN(self) IS the MEMBER_ACTIVE /
     * MEMBER_TAKEOVER_GRACE / JOIN_PENDING -> BACKUP_SYNCING transition
     * (BACKUP_ASSIGNED), committed BEFORE any primary/generation/mirror
     * write.  SHADOW-GUARD RULE: legacy/event decides WHICH transition
     * should happen (the Head assigned this node); cluster_transition()/
     * preflight validates whether Shadow agrees.  The transition is called
     * UNCONDITIONALLY - never skipped because shadow_phase != old: a
     * shadow-desync must fail closed here (UCN_ERR_STATE, nothing
     * committed) instead of silently falling back to the end-of-RX
     * shadow_sync() minting.  old_phase comes from the PRE-CALL legacy
     * state: a Member with the grace deadline armed is in TAKEOVER_GRACE,
     * otherwise MEMBER_ACTIVE; a JOIN_PENDING node stays JOIN_PENDING
     * (pre-assigned join path). */
    if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING) {
        old_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    } else {
        old_phase = (cluster->head_grace_deadline_ms != 0U)
                        ? UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE
                        : UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    }
    if (cluster_transition(cluster, old_phase,
                           UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                           UCN_CLUSTER_REASON_BACKUP_ASSIGNED,
                           now_ms) != UCN_OK) {
        /* Fail closed per the migration contract: a rejected transition
         * (shadow mismatch / illegal pair / pre-mutated phase fields)
         * commits no Assignment field, including the shared known-backup
         * record. */
        return UCN_ERR_STATE;
    }
    cluster->role = UCN_CLUSTER_ROLE_BACKUP;
    /* Commit the identity now: a JOIN_ACCEPT may be dropped on a lossy
     * link, and a stale head_node_id==local would otherwise make the
     * Backup send Keepalive to itself. */
    cluster->cluster_id = message->cluster_id;
    cluster->term = message->term;
    cluster->head_node_id = message->head_node_id;
    cluster_history_note_stable_epoch(cluster, cluster->cluster_id,
                                      cluster->term,
                                      cluster->head_node_id);
    cluster->backup_syncing = true;
    cluster->backup_ready = false;
    cluster->backup_primary_node_id = source;
    cluster->backup_generation = message->backup_generation;
    cluster->membership_sequence = 0U;
    /* A node may have left a former Backup takeover and re-entered as a
     * Member before this assignment arrives.  The BACKUP_SYNCING target
     * owns a clean takeover transaction; retaining its old active bit would
     * reinterpret the freshly committed phase as BACKUP_TAKEOVER. */
    cluster->backup_takeover_active = false;
    cluster->backup_takeover_deadline_ms = 0U;
    cluster->backup_takeover_ack_count = 0U;
    cluster->backup_takeover_acked = 0U;
    cluster->backup_takeover_prepare_cursor = 0U;
    cluster->backup_takeover_announce_cursor = 0U;
    cluster->backup_takeover_announce_remaining = 0U;
    cluster->backup_takeover_announce_active = false;
    cluster->known_backup_node_id = message->sync_token;
    cluster->known_backup_generation = message->backup_generation;
    cluster->backup_primary_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
    cluster->backup_primary_lease_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    cluster->backup_missed_heartbeats = 0U;
    (void)clear_members(cluster);
#if !defined(NDEBUG)
    /* CLV2-01-04e.1 post-commit derive assert: after the transition AND
     * every site effect (role/identity, syncing=true, ready=false,
     * primary/generation, deadlines, members cleared) the legacy state
     * must still derive BACKUP_SYNCING.  The target deliberately clears a
     * stale former takeover transaction, while the already-BACKUP replay
     * path above preserves a live same-generation takeover. */
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_BACKUP_SYNCING);
#endif
    return UCN_OK;
}

ucn_result_t send_backup_ready(ucn_cluster_t *cluster)
{
    ucn_cluster_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_READY;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->backup_primary_node_id;
    /* C07.7 P1: bind the readiness proof to the exact snapshot epoch so
     * a delayed/replayed old READY cannot mark a stale mirror as ready. */
    message.backup_generation = cluster->backup_generation;
    message.membership_sequence = cluster->membership_sequence;
    return send_cluster_message(cluster, cluster->backup_primary_node_id,
                                  &message);
}

ucn_result_t handle_backup_ready(ucn_cluster_t *cluster,
                                          ucn_node_id_t source,
                                          const ucn_cluster_message_t *message,
                                          uint32_t now_ms)
{
    ucn_cluster_phase_t old_phase;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        source != cluster->backup_node_id) {
        return UCN_ERR_ACCESS;
    }
    /* C07.7 P1: only a READY for the exact (cluster, term, generation,
     * membership_sequence) of the current snapshot counts; a delayed or
     * replayed READY from an older epoch is discarded so the Backup is
     * never marked ready against a stale mirror. */
    if (message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term ||
        message->backup_generation != cluster->backup_generation ||
        message->membership_sequence != cluster->membership_sequence) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    /* CLV2-01-04d.3: the READY is the (ASSIGNING|SYNCING) -> STABLE
     * transition, run BEFORE the phase-relevant ready=true write.  A Head
     * already STABLE (ready==true) receiving a duplicate same-epoch READY
     * keeps the legacy idempotent no-op - the STABLE self-loop is not a
     * DIRECT edge, so it must never reach the entry point. */
    if (cluster->backup_ready) {
        return UCN_OK;
    }
    /* old_phase is derived from the PRE-CALL state: a selected Backup
     * with the assignment sweep still armed is HEAD_BACKUP_ASSIGNING,
     * otherwise HEAD_BACKUP_SYNCING (snapshot in flight). */
    old_phase = (cluster->backup_assign_pending)
                    ? UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING
                    : UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    if (cluster_transition(cluster, old_phase,
                           UCN_CLUSTER_PHASE_HEAD_STABLE,
                           UCN_CLUSTER_REASON_SNAPSHOT_READY,
                           now_ms) != UCN_OK) {
        /* Fail closed: a rejected transition (shadow mismatch / illegal
         * pair / pre-mutated phase fields) leaves every field untouched
         * including backup_ready - the Backup is never marked ready. */
        return UCN_ERR_STATE;
    }
    cluster->backup_ready = true;
#if !defined(NDEBUG)
    /* CLV2-01-04d.3 post-commit derive assert: after the transition AND
     * the site's ready=true write the Head must derive HEAD_STABLE. */
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_HEAD_STABLE);
#endif
    return UCN_OK;
}

ucn_result_t handle_backup_member_sync(ucn_cluster_t *cluster,
                                                ucn_node_id_t source,
                                                const ucn_cluster_message_t *message,
                                                uint32_t now_ms)
{
    ucn_cluster_member_t *member;
    ucn_cluster_phase_t old_phase;

    if (cluster->role != UCN_CLUSTER_ROLE_BACKUP ||
        source != cluster->backup_primary_node_id) {
        return UCN_ERR_ACCESS;
    }
    /* C07.7 P1: every Type 12 frame (BEGIN / member / END / DELTA) must
     * belong to the exact current Backup generation; a delayed frame of
     * an older generation is replayed and cannot poison the mirror. */
    if (message->backup_generation != cluster->backup_generation) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    /* Sender paths use cluster_serial_next_checked(); receiver paths must
     * impose the same non-zero, bounded domain before any ordering test so a
     * malformed Type 12 frame cannot manufacture a 32-bit wrap. */
    if (message->membership_sequence == 0U ||
        message->membership_sequence >
            UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    if ((message->flags & UCN_CLUSTER_FLAG_SYNC_DELTA) != 0U) {
        uint32_t expected_sequence;

        /* C07.7 P1: live incremental refresh: update the member's nonce
         * without touching syncing/ready so a periodic refresh can never
         * strand a ready Backup during a Primary failure.  A stale DELTA
         * (already applied sequence) is ignored.  A sequence gap means a
         * DELTA was lost: the mirror may be missing a member nonce, so
         * request a full resync instead of silently continuing. */
        if (message->membership_sequence <= cluster->membership_sequence) {
            return UCN_OK;
        }
        if (cluster_serial_next_checked(cluster->membership_sequence,
                                        &expected_sequence) != UCN_OK ||
            message->membership_sequence != expected_sequence) {
            /* CLV2-01-04e.7: a DELTA gap re-enters SYNCING - the
             * BACKUP_READY -> BACKUP_SYNCING transition (RESYNC_STARTED),
             * committed BEFORE the site's ready=false/syncing=true writes,
             * UNCONDITIONAL on the legacy event (the derived pre-phase
             * decides: BACKUP_READY -> explicit transition, BACKUP_SYNCING
             * -> self no-op, BACKUP_TAKEOVER (M01.0.2 late-sync) -> NO
             * transition, takeover precedence - the legacy body still
             * applies the resync).  Shadow-Guard RULE: the caller never
             * skips the transition because shadow_phase differs; a shadow
             * desync must fail closed (UCN_ERR_STATE, zero re-entry
             * writes) instead of silently falling back to the end-of-RX
             * shadow_sync() minting (d.7.1-forbidden). */
            if (cluster_phase_from_legacy_state(cluster, now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY) {
                if (cluster_transition(
                        cluster, UCN_CLUSTER_PHASE_BACKUP_READY,
                        UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                        UCN_CLUSTER_REASON_RESYNC_STARTED,
                        now_ms) != UCN_OK) {
                    return UCN_ERR_STATE;
                }
            }
            cluster->backup_ready = false;
            cluster->backup_syncing = true;
            (void)send_backup_resync_req(cluster);
            return UCN_ERR_REPLAY;
        }
        member = backup_allocate_mirror(cluster, message->member_node_id);
        if (member == NULL) {
            return UCN_ERR_NO_SPACE;
        }
        member->last_nonce = message->member_nonce;
        member->lease_expires_at_ms =
            ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
        cluster->membership_sequence = message->membership_sequence;
        cluster->backup_primary_deadline_ms = ucn_deadline_from_now(
            now_ms, cluster->config.keepalive_interval_ms);
        return UCN_OK;
    }
    if ((message->flags & UCN_CLUSTER_FLAG_SYNC_BEGIN) != 0U) {
        /* CLV2-01-04e.7: a fresh snapshot BEGIN re-enters SYNCING - the
         * BACKUP_READY -> BACKUP_SYNCING transition (RESYNC_STARTED),
         * committed BEFORE the site drops the mirror and writes
         * syncing=true/ready=false, UNCONDITIONAL on the legacy event
         * (derived pre-phase decides, takeover precedence for M01.0.2).
         * Fail closed: a rejected transition (shadow desync) returns
         * UCN_ERR_STATE with ZERO re-entry writes (mirror + sequence +
         * ready/syncing all untouched) - the end-of-RX sync then re-aligns
         * to the unchanged phase. */
        if (cluster_phase_from_legacy_state(cluster, now_ms) ==
            UCN_CLUSTER_PHASE_BACKUP_READY) {
            if (cluster_transition(
                    cluster, UCN_CLUSTER_PHASE_BACKUP_READY,
                    UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                    UCN_CLUSTER_REASON_RESYNC_STARTED,
                    now_ms) != UCN_OK) {
                return UCN_ERR_STATE;
            }
        }
        /* A fresh snapshot re-enters SYNCING and drops any stale mirror.
         * The new snapshot restarts its own membership_sequence. */
        (void)clear_members(cluster);
        cluster->membership_sequence = message->membership_sequence;
        cluster->backup_syncing = true;
        cluster->backup_ready = false;
        cluster->backup_primary_deadline_ms =
            ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
        cluster->backup_primary_lease_deadline_ms =
            ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
        return UCN_OK;
    }
    {
        uint32_t expected_sequence;

        if (cluster_serial_next_checked(cluster->membership_sequence,
                                        &expected_sequence) != UCN_OK ||
            message->membership_sequence != expected_sequence) {
            /* CLV2-01-04e.7: a dropped/reordered snapshot frame re-enters
             * SYNCING - the BACKUP_READY -> BACKUP_SYNCING transition
             * (RESYNC_STARTED), committed BEFORE the site's syncing=true/
             * ready=false writes, UNCONDITIONAL on the legacy event (derived
             * pre-phase decides, takeover precedence for M01.0.2).  Fail
             * closed: a rejected transition returns UCN_ERR_STATE with ZERO
             * re-entry writes. */
            if (cluster_phase_from_legacy_state(cluster, now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_READY) {
                if (cluster_transition(
                        cluster, UCN_CLUSTER_PHASE_BACKUP_READY,
                        UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                        UCN_CLUSTER_REASON_RESYNC_STARTED,
                        now_ms) != UCN_OK) {
                    return UCN_ERR_STATE;
                }
            }
            /* A dropped/reordered snapshot frame on a lossy link: stay syncing
             * and await the bounded snapshot retransmit (a fresh BEGIN resets
             * the sequence) instead of detaching. */
            cluster->stats.malformed_messages++;
            cluster->backup_syncing = true;
            cluster->backup_ready = false;
            cluster->membership_sequence = 0U;
            cluster->backup_primary_deadline_ms =
                ucn_deadline_from_now(now_ms,
                                      cluster->config.keepalive_interval_ms);
            cluster->backup_primary_lease_deadline_ms =
                ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
            return UCN_ERR_REPLAY;
        }
    }
    cluster->membership_sequence = message->membership_sequence;
    if ((message->flags & UCN_CLUSTER_FLAG_SYNC_END) != 0U) {
        if (backup_covers_all_members(cluster)) {
            /* CLV2-01-04e.2: the SYNC_END-with-full-coverage event IS the
             * BACKUP_SYNCING -> BACKUP_READY transition (SNAPSHOT_READY),
             * committed through the single entry point BEFORE the site's
             * syncing=false/ready=true writes.  The LEGACY event decides
             * which transition happens; cluster_transition() validates
             * whether the shadow agrees and fails closed (UCN_ERR_STATE,
             * zero writes) on a mismatch - the Backup is never marked
             * ready against a desynced shadow.  The pre-call derive check
             * only excludes the M01.0.2 takeover-precedence case: a node
             * with takeover_active derives BACKUP_TAKEOVER, for which no
             * SYNCING->READY edge exists - the legacy body below still
             * applies the sync frames per Current behaviour (the phase
             * stays BACKUP_TAKEOVER). */
            if (cluster_phase_from_legacy_state(cluster, now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING) {
                if (cluster_transition(
                        cluster, UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                        UCN_CLUSTER_PHASE_BACKUP_READY,
                        UCN_CLUSTER_REASON_SNAPSHOT_READY,
                        now_ms) != UCN_OK) {
                    /* Fail closed: a rejected transition (shadow mismatch
                     * / pre-mutated phase fields) leaves every field
                     * untouched including backup_ready - the Backup is
                     * never marked ready. */
                    return UCN_ERR_STATE;
                }
#if !defined(NDEBUG)
                /* CLV2-01-04e.2 post-commit derive assert: after the
                 * transition (apply_legacy wrote syncing=false/ready=true)
                 * the legacy state must derive BACKUP_READY; the site's
                 * idempotent writes below keep it there. */
                assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                       UCN_CLUSTER_PHASE_BACKUP_READY);
#endif
            }
            cluster->backup_syncing = false;
            cluster->backup_ready = true;
            return send_backup_ready(cluster);
        }
        /* CLV2-01-04e.7: the coverage-failed SYNC_END detaches the Backup
         * ((SYNCING|READY|TAKEOVER) -> DETACHED_OBSERVE).  d.4 preflight
         * pattern: validate the PRE-CALL state with ZERO writes, run the
         * Current-order stats/send, then commit BEFORE backup_clear_sync()
         * (which is then idempotent cleanup; set_detached()'s role rewrite
         * is redundant-but-harmless per the b.6/c.5 precedent).
         * Reason choice (human auditor): PRIMARY_LOST for EVERY pre-state -
         * the primary's sync stream failed.  For the TAKEOVER pre-state
         * (reachable via the M01.0.2 late-sync combo) TAKEOVER_TIMEOUT
         * would be a lie: the takeover did NOT time out, so the honest
         * reason is the sync-stream failure.  The TAKEOVER pre-state is
         * NEVER rejected here just to avoid the edge - if the legacy body
         * detaches, the transition must express it. */
        old_phase = cluster_phase_from_legacy_state(cluster, now_ms);
        if (cluster_transition_preflight(
                cluster, old_phase,
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                now_ms) != UCN_OK) {
            /* Fail closed: NOTHING is touched - no stats++, no reject
             * sent, no detach; the end-of-RX sync re-aligns the shadow. */
            return UCN_ERR_STATE;
        }
        cluster->stats.joins_rejected++;
        /* C07.7 P1: tell the Head immediately so it can pick the next
         * candidate instead of waiting for the member lease to expire. */
        (void)send_backup_reject(cluster,
                                  UCN_CLUSTER_BACKUP_REJECT_COVERAGE);
        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                               UCN_CLUSTER_REASON_PRIMARY_LOST,
                               now_ms) != UCN_OK) {
            /* Fail closed: nothing phase-relevant was touched yet. */
            return UCN_ERR_STATE;
        }
        backup_clear_sync(cluster, now_ms);
#if !defined(NDEBUG)
        /* CLV2-01-04e.7 post-commit derive assert: after the transition
         * AND backup_clear_sync() the legacy state must derive
         * DETACHED_OBSERVE. */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
        return UCN_OK;
    }
    member = backup_allocate_mirror(cluster, message->member_node_id);
    if (member == NULL) {
        /* CLV2-01-04e.7: a mirror-allocation failure detaches the Backup
         * ((SYNCING|READY|TAKEOVER) -> DETACHED_OBSERVE) - the same d.4
         * preflight pattern and PRIMARY_LOST reason as the coverage-failed
         * END (the primary's sync stream failed; TAKEOVER_TIMEOUT would be
         * a lie for a sync failure during takeover).  Preflight validates
         * with ZERO writes before the Current-order reject send; the
         * commit runs BEFORE the idempotent backup_clear_sync(). */
        old_phase = cluster_phase_from_legacy_state(cluster, now_ms);
        if (cluster_transition_preflight(
                cluster, old_phase,
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                now_ms) != UCN_OK) {
            /* Fail closed: NOTHING is touched. */
            return UCN_ERR_STATE;
        }
        (void)send_backup_reject(cluster,
                                  UCN_CLUSTER_BACKUP_REJECT_NO_SPACE);
        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                               UCN_CLUSTER_REASON_PRIMARY_LOST,
                               now_ms) != UCN_OK) {
            /* Fail closed: nothing phase-relevant was touched yet. */
            return UCN_ERR_STATE;
        }
        backup_clear_sync(cluster, now_ms);
#if !defined(NDEBUG)
        /* CLV2-01-04e.7 post-commit derive assert: after the transition
         * AND backup_clear_sync() the legacy state must derive
         * DETACHED_OBSERVE. */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
        return UCN_ERR_NO_SPACE;
    }
    member->lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    member->last_nonce = message->member_nonce;
    cluster->backup_primary_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
    cluster->backup_primary_lease_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    return UCN_OK;
}

ucn_result_t handle_primary_heartbeat(ucn_cluster_t *cluster,
                                               ucn_node_id_t source,
                                               const ucn_cluster_message_t *message,
                                               uint32_t now_ms)
{
    if (cluster->role != UCN_CLUSTER_ROLE_BACKUP ||
        source != cluster->backup_primary_node_id) {
        return UCN_ERR_ACCESS;
    }
    /* C07.7 P1: only heartbeats of the exact Backup epoch refresh this
     * Backup's liveness; a replayed heartbeat from an older generation,
     * a different term, or a stale membership_sequence of the same
     * generation cannot mask a genuinely dead Primary. */
    if (message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term ||
        message->backup_generation != cluster->backup_generation ||
        message->membership_sequence < cluster->membership_sequence) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    /* A heartbeat may carry a newer sequence if a DELTA was lost and the
     * mirror later resynced; adopt it so liveness and epoch state stay
     * in lockstep. */
    cluster->membership_sequence = message->membership_sequence;
    cluster->backup_primary_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
    cluster->backup_primary_lease_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    cluster->backup_missed_heartbeats = 0U;
    return UCN_OK;
}

/* C07.7 P1: Backup -> Head, request a full resync after a DELTA gap. */
ucn_result_t send_backup_resync_req(ucn_cluster_t *cluster)
{
    ucn_cluster_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->backup_primary_node_id;
    message.backup_generation = cluster->backup_generation;
    message.membership_sequence = cluster->membership_sequence;
    return send_cluster_message(cluster, cluster->backup_primary_node_id,
                                 &message);
}

/* C07.7 P1: Backup -> Head, reject the assignment so the Head can
 * immediately pick the next candidate. */
ucn_result_t send_backup_reject(ucn_cluster_t *cluster,
                                       uint8_t reason)
{
    ucn_cluster_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_REJECT;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->backup_primary_node_id;
    message.backup_generation = cluster->backup_generation;
    message.reject_reason = reason;
    return send_cluster_message(cluster, cluster->backup_primary_node_id,
                                 &message);
}

/* C07.7 P1: Head-side handler for BACKUP_RESYNC_REQ: restart the
 * snapshot so the Backup converges again after a lost DELTA. */
ucn_result_t handle_backup_resync_req(ucn_cluster_t *cluster,
                                              ucn_node_id_t source,
                                              const ucn_cluster_message_t *message)
{
    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        source != cluster->backup_node_id ||
        message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term ||
        message->backup_generation != cluster->backup_generation) {
        return UCN_ERR_ACCESS;
    }
    backup_resync(cluster);
    return UCN_OK;
}

/* C07.7 P1: Head-side handler for BACKUP_REJECT: cool the candidate down
 * and immediately select the next one instead of waiting for the member
 * lease to expire. */
ucn_result_t handle_backup_reject(ucn_cluster_t *cluster,
                                          ucn_node_id_t source,
                                          const ucn_cluster_message_t *message,
                                          uint32_t now_ms)
{
    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        source != cluster->backup_node_id ||
        message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term ||
        message->backup_generation != cluster->backup_generation) {
        return UCN_ERR_ACCESS;
    }
    /* CLV2-01-04d.7 (MAJOR 1C): the rejected Backup IS the current one
     * (source == backup_node_id, so node_id != 0 here), so the rejection
     * is the HEAD_BACKUP_* -> HEAD_NO_BACKUP phase change and must flow
     * through the entry point BEFORE any phase-relevant legacy write
     * (apply_legacy(NO_BACKUP) makes the site's node_id=0/ready=false
     * idempotent).  The reslection below (assign_backup) then commits the
     * NO_BACKUP -> ASSIGNING transition with a now-aligned shadow guard:
     * the whole chain is two explicit transitions, no stale-shadow
     * fallback. */
    {
        ucn_cluster_phase_t pre_phase =
            cluster_phase_from_legacy_state(cluster, now_ms);

        if (pre_phase != UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) {
            if (cluster_transition(cluster, pre_phase,
                                   UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                                   UCN_CLUSTER_REASON_BACKUP_LOST,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition leaves every field
                 * untouched - do NOT run the reject side effects. */
                return UCN_ERR_STATE;
            }
        }
    }
    cluster->backup_candidate_cooldown_until_ms = ucn_deadline_from_now(
        now_ms, cluster->config.keepalive_interval_ms);
    cluster->backup_rejected_node_id = source;
    cluster->backup_node_id = 0U;
    cluster->backup_ready = false;
#if !defined(NDEBUG)
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
#endif
    assign_backup(cluster, now_ms);
    return UCN_OK;
}

/* C07.3 majority-confirmed takeover. */

ucn_result_t complete_takeover(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    uint32_t next_term;

    if (cluster_serial_next_checked(cluster->term, &next_term) != UCN_OK) {
        return UCN_ERR_EXHAUSTED;
    }

    /* CLV2-01-04e.4 (F1 anchor, human e-group focus 3): the quorum IS
     * the BACKUP_TAKEOVER -> HEAD_NO_BACKUP transition - call it FIRST,
     * UNCONDITIONALLY, fail closed.  apply_legacy(HEAD_NO_BACKUP) writes
     * role + backup_node_id=0 + ready=false ONLY; everything else below
     * is caller-owned and stays AT THE SITE in original order
     * (takeover_active / syncing / primary / known_backup_* / term /
     * head / deadlines / cursors / stats).  On a rejected transition
     * NOTHING of the clear set runs - the takeover stays active and the
     * ack stays counted (the shadow anomaly surfaces in Debug). */
    if (cluster_transition(cluster, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                           UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                           UCN_CLUSTER_REASON_TAKEOVER_QUORUM,
                           now_ms) != UCN_OK) {
        /* Fail closed per the migration contract: do NOT promote, do NOT
         * clear any takeover / mirror state. */
        return UCN_ERR_STATE;
    }
    cluster->role = UCN_CLUSTER_ROLE_HEAD;
    cluster->term = next_term;
    cluster->head_node_id = cluster->config.local_node_id;
    cluster_history_note_stable_epoch(cluster, cluster->cluster_id,
                                      cluster->term,
                                      cluster->head_node_id);
    cluster->current_head_score = cluster->config.head_score;
    cluster->role_since_ms = now_ms;
    cluster->election_deadline_ms = 0U;
    cluster->next_advertise_ms = now_ms;
    cluster->backup_takeover_active = false;
    cluster->backup_syncing = false;
    cluster->backup_ready = false;
    cluster->backup_node_id = 0U;
    cluster->backup_primary_node_id = 0U;
    cluster->known_backup_node_id = 0U;
    cluster->known_backup_generation = 0U;
    cluster->stats.elections_won++;
    cluster->stats.head_switches++;
    /* The new Head is not its own member; renew the rest so takeover
     * does not immediately expire the inherited membership mirror. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (!cluster->members[index].occupied) {
            continue;
        }
        if (cluster->members[index].node_id == cluster->config.local_node_id) {
            (void)memset(&cluster->members[index], 0,
                         sizeof(cluster->members[index]));
        } else {
            cluster->members[index].lease_expires_at_ms =
                ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
        }
    }
    /* Send one member at a time from step().  This keeps Token Bucket
     * back-pressure recoverable instead of losing a tail of the broadcast. */
    cluster->backup_takeover_announce_cursor = 0U;
    cluster->backup_takeover_announce_remaining =
        (uint8_t)member_count_u16(cluster);
    cluster->backup_takeover_announce_active =
        cluster->backup_takeover_announce_remaining != 0U;
#if !defined(NDEBUG)
    /* CLV2-01-04e.4 post-commit derive assert: after the transition AND
     * every site write the node must derive HEAD_NO_BACKUP (role == HEAD
     * with backup_node_id == 0; ready was cleared). */
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
#endif
    return UCN_OK;
}

void start_takeover(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    bool self_in_mirror = false;

    /* CLV2-01-04e.3: the lapsed Primary lease IS the BACKUP_READY ->
     * BACKUP_TAKEOVER transition - call it FIRST, UNCONDITIONALLY, fail
     * closed.  apply_legacy(BACKUP_TAKEOVER) writes role + takeover_
     * active ONLY - it NEVER touches syncing/ready (CLV2-M01.0.2: a late
     * same-generation Type12 can re-arm syncing while takeover is active
     * and the takeover_active && syncing combo must stay expressible).
     * All deadline/cursor/ack state stays caller-owned at the site in
     * original order. */
    if (cluster_transition(cluster, UCN_CLUSTER_PHASE_BACKUP_READY,
                           UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                           UCN_CLUSTER_REASON_TAKEOVER_STARTED,
                           now_ms) != UCN_OK) {
        /* Fail closed: do NOT arm takeover / reset the ack bookkeeping on
         * a rejected transition (shadow mismatch / illegal pair /
         * pre-mutated phase fields); the next step re-visits the lease. */
        return;
    }
    cluster->backup_takeover_active = true;
    cluster->backup_takeover_ack_count = 0U;
    cluster->backup_takeover_acked = 0U;
    cluster->backup_takeover_prepare_cursor = 0U;
    cluster->backup_takeover_deadline_ms =
        ucn_deadline_from_now(now_ms, UCN_CLUSTER_TAKEOVER_WINDOW_MS);
    /* C07.7 P1: the Backup is a member of its own mirror, so majority
     * (active/2+1 with active including the Backup) is only reachable if
     * the Backup's own vote counts.  Guard on the mirror actually
     * containing the Backup. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied &&
            cluster->members[index].node_id == cluster->config.local_node_id) {
            self_in_mirror = true;
            break;
        }
    }
    if (self_in_mirror) {
        cluster->backup_takeover_ack_count = 1U;
    }
#if !defined(NDEBUG)
    /* CLV2-01-04e.3 post-commit derive assert: after the transition AND
     * every site write the node must derive BACKUP_TAKEOVER (role ==
     * BACKUP with takeover_active == true). */
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
#endif
}

void send_takeover_prepare_step(ucn_cluster_t *cluster)
{
    size_t examined;

    if (!cluster->backup_takeover_active) {
        return;
    }
    for (examined = 0U; examined < UCN_CLUSTER_MAX_MEMBERS; ++examined) {
        size_t index = (cluster->backup_takeover_prepare_cursor + examined) %
                       UCN_CLUSTER_MAX_MEMBERS;
        ucn_cluster_message_t message;

        if (!cluster->members[index].occupied ||
            cluster->members[index].node_id == cluster->config.local_node_id ||
            (cluster->backup_takeover_acked & (UINT32_C(1) << index)) != 0U) {
            continue;
        }
        (void)memset(&message, 0, sizeof(message));
        message.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
        message.role = UCN_CLUSTER_ROLE_BACKUP;
        message.cluster_id = cluster->cluster_id;
        message.term = cluster->term;
        message.head_node_id = cluster->backup_primary_node_id;
        message.backup_generation = cluster->backup_generation;
        if (send_cluster_message(cluster, cluster->members[index].node_id,
                                 &message) == UCN_OK) {
            cluster->backup_takeover_prepare_cursor =
                (uint8_t)((index + 1U) % UCN_CLUSTER_MAX_MEMBERS);
        }
        return;
    }
}

void send_takeover_announce_step(ucn_cluster_t *cluster)
{
    size_t examined;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        !cluster->backup_takeover_announce_active) {
        return;
    }
    for (examined = 0U; examined < UCN_CLUSTER_MAX_MEMBERS; ++examined) {
        size_t index = (cluster->backup_takeover_announce_cursor + examined) %
                       UCN_CLUSTER_MAX_MEMBERS;
        ucn_cluster_message_t message;

        if (!cluster->members[index].occupied) {
            continue;
        }
        (void)memset(&message, 0, sizeof(message));
        message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
        message.role = UCN_CLUSTER_ROLE_HEAD;
        message.cluster_id = cluster->cluster_id;
        message.term = cluster->term;
        message.head_node_id = cluster->config.local_node_id;
        message.head_score = cluster->config.head_score;
        message.lease_ms = cluster->config.lease_ms;
        message.backup_generation = cluster->backup_generation;
        if (send_cluster_message(cluster, cluster->members[index].node_id,
                                 &message) == UCN_OK) {
            cluster->backup_takeover_announce_cursor =
                (uint8_t)((index + 1U) % UCN_CLUSTER_MAX_MEMBERS);
            cluster->backup_takeover_announce_remaining--;
            if (cluster->backup_takeover_announce_remaining == 0U) {
                cluster->backup_takeover_announce_active = false;
            }
        }
        return;
    }
    cluster->backup_takeover_announce_active = false;
    cluster->backup_takeover_announce_remaining = 0U;
}

ucn_result_t handle_takeover_prepare(ucn_cluster_t *cluster,
                                              ucn_node_id_t source,
                                              const ucn_cluster_message_t *message,
                                              uint32_t now_ms)
{
    ucn_cluster_message_t ack;

    (void)now_ms;

    if (cluster->role != UCN_CLUSTER_ROLE_MEMBER) {
        return UCN_ERR_ACCESS;
    }
    if (message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term ||
        message->head_node_id != cluster->head_node_id ||
        source != cluster->known_backup_node_id ||
        message->backup_generation != cluster->known_backup_generation) {
        return UCN_ERR_ACCESS;
    }
    /* C07.7 P1: the vote identity is (cluster_id, term, generation), so a
     * vote cast in one Cluster cannot suppress a legitimate takeover in a
     * different Cluster that shares the same term number. */
    if (cluster->member_voted_term == cluster->term &&
        cluster->member_voted_cluster_id == cluster->cluster_id &&
        cluster->member_voted_generation == message->backup_generation) {
        return UCN_OK; /* already acknowledged this epoch */
    }
    (void)memset(&ack, 0, sizeof(ack));
    ack.type = UCN_CLUSTER_MSG_TAKEOVER_ACK;
    ack.role = UCN_CLUSTER_ROLE_MEMBER;
    ack.cluster_id = cluster->cluster_id;
    ack.term = cluster->term;
    ack.head_node_id = cluster->head_node_id;
    ack.backup_generation = message->backup_generation;
    {
        ucn_result_t result = send_cluster_message(cluster, source, &ack);

        if (result != UCN_OK) {
            return result;
        }
        cluster->member_voted_term = cluster->term;
        cluster->member_voted_cluster_id = cluster->cluster_id;
        cluster->member_voted_generation = message->backup_generation;
        return UCN_OK;
    }
}

ucn_result_t handle_takeover_ack(ucn_cluster_t *cluster,
                                          ucn_node_id_t source,
                                          const ucn_cluster_message_t *message,
                                          uint32_t now_ms)
{
    size_t index;
    size_t member_index = UCN_CLUSTER_MAX_MEMBERS;
    uint16_t active = member_count_u16(cluster);
    uint16_t majority = (uint16_t)(active / 2U + 1U);

    if (cluster->role != UCN_CLUSTER_ROLE_BACKUP ||
        !cluster->backup_takeover_active) {
        return UCN_ERR_ACCESS;
    }
    if (message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term ||
        message->backup_generation != cluster->backup_generation) {
        return UCN_ERR_ACCESS;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied &&
            cluster->members[index].node_id == source) {
            member_index = index;
            break;
        }
    }
    if (member_index >= UCN_CLUSTER_MAX_MEMBERS) {
        return UCN_ERR_NOT_FOUND;
    }
    if ((cluster->backup_takeover_acked & (UINT32_C(1) << member_index)) != 0U) {
        return UCN_OK; /* already counted */
    }
    cluster->backup_takeover_acked |= (UINT32_C(1) << member_index);
    cluster->backup_takeover_ack_count++;
    if (cluster->backup_takeover_ack_count >= majority) {
        return complete_takeover(cluster, now_ms);
    }
    return UCN_OK;
}

ucn_result_t handle_head_takeover(ucn_cluster_t *cluster,
                                           ucn_node_id_t source,
                                           const ucn_cluster_message_t *message,
                                           uint32_t now_ms)
{
    bool recovery_historical_takeover;

    if (message->role != UCN_CLUSTER_ROLE_HEAD) {
        return UCN_ERR_MALFORMED;
    }
    recovery_historical_takeover =
        cluster->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD &&
        message->cluster_id != cluster->cluster_id;
    if ((cluster->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD &&
         message->cluster_id != cluster->cluster_id) ||
        (recovery_historical_takeover &&
         (cluster->last_cluster_id == 0U ||
          message->cluster_id != cluster->last_cluster_id ||
          cluster->last_stable_head == 0U)) ||
        source != cluster->known_backup_node_id ||
        message->head_node_id != source ||
        message->backup_generation != cluster->known_backup_generation) {
        return UCN_ERR_ACCESS;
    }
    /* A Recovery Cluster has a deliberately fresh local identity/Term.  A
     * stable takeover from its remembered parent domain is comparable only
     * with that domain's recorded Term, never with the Recovery Term. */
    if ((!recovery_historical_takeover && message->term <= cluster->term) ||
        (recovery_historical_takeover &&
         message->term <= cluster->max_seen_term)) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    if (cluster->role != UCN_CLUSTER_ROLE_MEMBER &&
        cluster->role != UCN_CLUSTER_ROLE_BACKUP &&
        cluster->role != UCN_CLUSTER_ROLE_JOIN_PENDING &&
        cluster->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        return UCN_ERR_ACCESS;
    }
    /* CLV2-01-04e.6: the BACKUP_* and GRACE inbound phases are migrated.
     * old_phase is derived from the PRE-CALL legacy state (role + flags),
     * NEVER from the shadow mirror, and the transition is called
     * UNCONDITIONALLY (fail closed - a shadow mismatch trips the validate
     * gate).  apply_legacy(MEMBER_ACTIVE) writes role + grace=0 +
     * eligible=false ONLY; the full clear set (takeover/syncing/ready/
     * known_backup_*) and the epoch refresh stay AT THE SITE below in
     * original order (F1 anchor).  A plain MEMBER_ACTIVE inbound (role ==
     * MEMBER, no armed grace) performs NO transition - no self-loop
     * exists.  CLV2-01-04f.5: the RECOVERY_HEAD inbound edge is migrated
     * HERE (RECOVERY_YIELDED, the same DIRECT edge as the f.4
     * handle_recovery_declare yield path).  CLV2-01-04f.6 (review A MINOR
     * 1, human's 01-04f plan "JOIN_PENDING/RECOVERY_HEAD->MEMBER_ACTIVE
     * inbound edges"): the JOIN_PENDING inbound IS migrated HERE too - a
     * higher-Term HEAD_TAKEOVER switching a join-pending node to
     * MEMBER_ACTIVE is a REAL second production site for the
     * JOIN_PENDING->MEMBER_ACTIVE DIRECT edge (the 01-04b join-accept
     * site is the other), and the end-of-RX shadow sync must no longer
     * mint it.  Reason TAKEOVER_STARTED (a takeover switched the node,
     * not a join accept; the JOIN_ACCEPTED diff-table fallback would be
     * semantically wrong for a HEAD_TAKEOVER event). */
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
        ucn_cluster_phase_t old_phase = cluster->backup_takeover_active
                                            ? UCN_CLUSTER_PHASE_BACKUP_TAKEOVER
                                            : (cluster->backup_ready
                                                   ? UCN_CLUSTER_PHASE_BACKUP_READY
                                                   : UCN_CLUSTER_PHASE_BACKUP_SYNCING);

        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                               UCN_CLUSTER_REASON_TAKEOVER_STARTED,
                               now_ms) != UCN_OK) {
            /* Fail closed: do NOT refresh the epoch / clear the mirror on
             * a rejected transition (shadow mismatch / illegal pair /
             * pre-mutated phase fields). */
            return UCN_ERR_STATE;
        }
    } else if (cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
               cluster->head_grace_deadline_ms != 0U) {
        if (cluster_transition(cluster,
                               UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                               UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                               UCN_CLUSTER_REASON_TAKEOVER_STARTED,
                               now_ms) != UCN_OK) {
            /* Fail closed: do NOT refresh the epoch on a rejected
             * transition; the node stays in grace. */
            return UCN_ERR_STATE;
        }
    } else if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING) {
        /* CLV2-01-04f.6 (review A MINOR 1): a JOIN_PENDING node switched
         * by the higher-Term HEAD_TAKEOVER commits the JOIN_PENDING ->
         * MEMBER_ACTIVE transition (TAKEOVER_STARTED) FIRST through the
         * single entry point, BEFORE any site write.  apply_legacy
         * (MEMBER_ACTIVE) writes role=MEMBER + grace=0 + eligible=false;
         * the site's epoch refresh and the pending-head + known-backup
         * clears below stay site-owned in original order (the role write
         * is idempotent).  Fail closed: a rejected transition (shadow
         * desync / illegal pair / pre-mutated phase fields) returns
         * UCN_ERR_STATE with NOTHING touched - the node stays JOIN_PENDING
         * with its pending Head and a later well-formed HEAD_TAKEOVER may
         * still be accepted. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                               UCN_CLUSTER_REASON_TAKEOVER_STARTED,
                               now_ms) != UCN_OK) {
            return UCN_ERR_STATE;
        }
    } else if (cluster->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        /* CLV2-01-04f.5: a Recovery Head defers to the stable higher-Term
         * Head immediately - the RECOVERY_HEAD -> MEMBER_ACTIVE edge
         * (RECOVERY_YIELDED, the same DIRECT edge the f.4
         * handle_recovery_declare yield path commits) runs FIRST through
         * the single entry point, BEFORE any site write.  apply_legacy
         * (MEMBER_ACTIVE) writes role=MEMBER + grace=0 + eligible=false;
         * the site's recovery clears (eligible/cluster_id/deadline_ms),
         * the idempotent role write, the epoch refresh and the
         * known_backup/pending clears stay site-owned below in original
         * order.  Fail closed: a rejected transition (shadow desync /
         * illegal pair / pre-mutated phase fields) returns UCN_ERR_STATE
         * with NOTHING touched - the Recovery Head keeps its role and
         * recovery state, and a later well-formed HEAD_TAKEOVER may still
         * be accepted (the end-of-RX sync only re-aligns to the unchanged
         * RECOVERY_HEAD phase). */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_RECOVERY_HEAD,
                               UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                               UCN_CLUSTER_REASON_RECOVERY_YIELDED,
                               now_ms) != UCN_OK) {
            return UCN_ERR_STATE;
        }
    }
    /* A Recovery Head defers to the stable higher-Term Head immediately
     * (the transition above committed RECOVERY_HEAD -> MEMBER_ACTIVE; the
     * writes below are the site-owned clears + epoch refresh, idempotent
     * after apply_legacy). */
    cluster->recovery_eligible = false;
    cluster->recovery_cluster_id = 0U;
    cluster->recovery_deadline_ms = 0U;
    cluster->role = UCN_CLUSTER_ROLE_MEMBER;
    cluster->cluster_id = message->cluster_id;
    cluster->term = message->term;
    cluster->head_node_id = source;
    cluster_history_note_stable_epoch(cluster, cluster->cluster_id,
                                      cluster->term,
                                      cluster->head_node_id);
    cluster->current_head_score = message->head_score;
    cluster->head_lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, message->lease_ms);
    cluster->head_grace_deadline_ms = 0U;
    cluster->next_keepalive_ms = now_ms;
    cluster->pending_head_node_id = 0U;
    cluster->pending_cluster_id = 0U;
    cluster->pending_term = 0U;
    cluster->backup_takeover_active = false;
    cluster->backup_syncing = false;
    cluster->backup_ready = false;
    cluster->known_backup_node_id = 0U;
    cluster->known_backup_generation = 0U;
#if !defined(NDEBUG)
    /* CLV2-01-04e.6 post-commit derive assert: after the transition (or
     * the legacy no-transition path) AND every site write the node must
     * derive MEMBER_ACTIVE (role == MEMBER, no armed grace deadline). */
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
#endif
    return UCN_OK;
}

uint32_t backup_control_spacing_ms(const ucn_cluster_t *cluster,
                                          size_t member_count)
{
    uint32_t divisor = member_count == 0U ? 1U : (uint32_t)member_count;
    uint32_t spacing = cluster->config.lease_ms / divisor;

    if (spacing < cluster->config.token_bucket.refill_ms) {
        spacing = cluster->config.token_bucket.refill_ms;
    }
    return spacing;
}

/* =====================================================================
 * CLV2-01-04d.7 HEAD-Ladder write-site audit (AUDIT HOLD closure).
 *
 * SHADOW-GUARD RULE (CLV2-01-04d.7.1, human auditor):
 *   Legacy/event decides WHICH transition should happen;
 *   cluster_transition()/preflight validates whether Shadow agrees.
 *   A caller must NEVER use shadow_phase to decide whether to SKIP calling
 *   the transition - otherwise a corrupted Shadow bypasses the very gate
 *   meant to detect it.
 *
 * Every phase-relevant write to the HEAD-ladder fields
 * (backup_node_id / backup_ready / backup_assign_pending while the node
 * is role HEAD) is classified below - each is either part of an explicit
 * cluster_transition()/preflight commit or keeps the phase unchanged.
 * NO HEAD-ladder phase change depends on the end-of-step
 * cluster_shadow_sync() minting anymore.
 *
 *   backup_node_id = 0U:
 *     apply_legacy(HEAD_NO_BACKUP) commit           -> transition commit
 *     remove_member() after HEAD_*->NOB transition   -> explicit (d.4)
 *     expire_members() after HEAD_*->NOB transition  -> explicit (d.4)
 *     handle_backup_reject() after HEAD_*->NOB        -> explicit (d.7 ITEM 3)
 *     assign_backup() no-candidate (node_id already 0) -> phase unchanged
 *     complete_takeover()                            -> explicit (01-04e.4:
 *        BACKUP_TAKEOVER -> HEAD_NO_BACKUP transition commit; the
 *        HEAD-ladder sub-phase ladder itself is untouched)
 *   backup_node_id = best_node_id:
 *     assign_backup() after NOB->ASSIGNING transition -> explicit (d.1)
 *   backup_ready = false:
 *     apply_legacy(HEAD_NO_BACKUP) commit            -> transition commit
 *     remove_member() / expire_members() (post-transition, idempotent)
 *                                                     -> explicit (d.4)
 *     handle_backup_reject() (post HEAD_*->NOB, idempotent) -> explicit (d.7 ITEM 3)
 *     assign_backup() (post NOB->ASSIGNING, idempotent) -> explicit (d.1)
 *     backup_resync() (post STABLE->target, idempotent) -> explicit (d.7 ITEM 4)
 *     complete_takeover()                            -> explicit (01-04e.4)
 *     BACKUP-side sites (backup_challenge / backup_clear_sync /
 *       handle_backup_assign / handle_backup_member_sync /
 *       handle_head_takeover clears)                 -> not head-ladder
 *   backup_ready = true:
 *     handle_backup_ready() after SYNCING->STABLE transition -> explicit (d.3)
 *   backup_assign_pending:
 *     apply_legacy(HEAD_BACKUP_ASSIGNING) commit     -> transition commit
 *     start_backup_assignment_cycle() after SYNCING->ASSIGNING (d.7 ITEM 1)
 *     queue_backup_assignment_for_member() after
 *       SYNCING->ASSIGNING (d.7 ITEM 2)
 *     send_backup_assignment_step() after
 *       ASSIGNING->SYNCING (d.7 ITEM 6 + d.2), both the last-frame and
 *       the loop-exhausted branches
 *   STABLE->ASSIGNING (armed-sweep resync): REAL direct transition via
 *     backup_resync() target dispatch (d.7 ITEM 4) - promoted to DIRECT
 *     in CLUSTER_TRANSITION_DIRECT_ALLOWED (d.7 ITEM 5).
 * =====================================================================
 *
 * CLV2-01-04e.7 (human audit item 5): BACKUP-side phase-defining write
 * audit - every write to backup_ready / backup_syncing /
 * backup_takeover_active / role (while leaving a BACKUP role) is either
 * part of an explicit cluster_transition()/preflight commit or keeps the
 * phase unchanged.  NO BACKUP-side phase change depends on the end-of-RX
 * cluster_shadow_sync() minting anymore.
 *
 *   backup_ready = true:
 *     handle_backup_member_sync() SYNC_END after SYNCING->READY (e.2)
 *                                                    -> explicit transition
 *   backup_ready = false:
 *     apply_legacy(BACKUP_SYNCING) commit            -> transition commit
 *     apply_legacy(HEAD_NO_BACKUP) commit (complete_takeover) -> e.4 commit
 *     handle_backup_assign() after *->BACKUP_SYNCING  -> explicit (e.1)
 *     handle_backup_member_sync() re-entry (BEGIN/DELTA-gap/seq-gap)
 *       after READY->SYNCING                         -> explicit (e.7)
 *     backup_challenge() after *->ELECTION           -> explicit (e.7)
 *     handle_head_takeover() clears after *->MEMBER_ACTIVE -> explicit (e.6)
 *     start_takeover() (backup_ready stays, M01.0.2) -> not written
 *     backup_clear_sync() (idempotent post-transition cleanup, incl.
 *       the e.7 detach paths and the e.5 timeout site) -> post-commit
 *   backup_syncing = true:
 *     apply_legacy(BACKUP_SYNCING) commit            -> transition commit
 *     handle_backup_assign() after *->BACKUP_SYNCING  -> explicit (e.1)
 *     handle_backup_member_sync() re-entry (BEGIN/DELTA-gap/seq-gap)
 *       after READY->SYNCING                         -> explicit (e.7)
 *   backup_syncing = false:
 *     apply_legacy(BACKUP_READY) commit              -> transition commit
 *     apply_legacy(HEAD_NO_BACKUP) commit (complete_takeover) -> e.4 commit
 *     handle_backup_member_sync() SYNC_END after SYNCING->READY (e.2)
 *                                                    -> explicit transition
 *     backup_challenge() after *->ELECTION           -> explicit (e.7)
 *     handle_head_takeover() clears after *->MEMBER_ACTIVE -> explicit (e.6)
 *     backup_clear_sync() (idempotent post-transition cleanup) -> post-commit
 *   backup_takeover_active = true:
 *     apply_legacy(BACKUP_TAKEOVER) commit           -> transition commit
 *     start_takeover() after READY->TAKEOVER         -> explicit (e.3)
 *   backup_takeover_active = false:
 *     apply_legacy(HEAD_NO_BACKUP) commit (complete_takeover) -> e.4 commit
 *     backup_challenge() after TAKEOVER->ELECTION    -> explicit (e.7)
 *     consider_head_offer() higher-Term after *->JOIN_PENDING -> explicit (e.7)
 *     handle_head_takeover() after TAKEOVER->MEMBER_ACTIVE -> explicit (e.6)
 *     backup_clear_sync() (idempotent cleanup; NOT cleared by the e.5
 *       timeout site itself - matches Current)       -> post-commit
 *   role leaving BACKUP (CANDIDATE / DETACHED / JOIN_PENDING / MEMBER /
 *     HEAD):
 *     apply_legacy commits (ELECTION/DETACHED_OBSERVE/JOIN_PENDING/
 *       MEMBER_ACTIVE/HEAD_NO_BACKUP)                -> transition commits
 *     backup_challenge() (e.7), consider_head_offer() (e.7),
 *       handle_head_takeover() (e.6), HEAD_STEPDOWN BACKUP branch (e.7),
 *       member_sync detach preflight+commit (e.7), start_takeover keeps
 *       role BACKUP (e.3), complete_takeover (e.4)   -> explicit
 * ===================================================================== */

void start_backup_assignment_cycle(ucn_cluster_t *cluster,
                                           uint32_t now_ms)
{
    size_t index;
    uint16_t member_count;
    ucn_cluster_phase_t pre_phase;
    bool needs_transition;

    /* CLV2-01-04d.7 (MAJOR 1A) + CLV2-01-04d.7.1 (shadow-guard closure):
     * arming the sweep IS the SYNCING -> ASSIGNING phase change, so when
     * the LEGACY derives SYNCING the transition is called UNCONDITIONALLY
     * (fail-closed) - never skipped because Shadow disagrees: a corrupted
     * Shadow must trip the validate gate, not be bypassed.  pre_phase ==
     * STABLE (ready-precedence: arming pending does NOT change the phase)
     * and pre_phase == ASSIGNING (true self re-arm) keep the idempotent
     * legacy body with no transition.  No end-of-step shadow_sync()
     * minting is relied on. */
    pre_phase = cluster_phase_from_legacy_state(cluster, now_ms);
    member_count = member_count_u16(cluster);
    /* No member means there is no assignment sweep to arm.  In particular,
     * do not commit SYNCING -> ASSIGNING and immediately clear pending again:
     * that is a false state transition and made the post-condition assert
     * fail in the fast impaired simulator after the last member left. */
    needs_transition = pre_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING &&
                       member_count != 0U;
    if (needs_transition) {
        if (cluster_transition(cluster,
                               UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                               UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                               UCN_CLUSTER_REASON_BACKUP_ASSIGNED,
                               now_ms) != UCN_OK) {
            /* Fail closed: a rejected transition leaves every field
             * untouched - do NOT arm the sweep; the next step re-visits. */
            return;
        }
    }
    cluster->backup_assign_cursor = 0U;
    cluster->backup_assign_remaining = (uint8_t)member_count;
    cluster->backup_assign_pending = cluster->backup_assign_remaining != 0U;
    /* The designated Backup must receive the identity record first; it is
     * the only recipient that may begin state mirroring and later take over. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied &&
            cluster->members[index].node_id == cluster->backup_node_id) {
            cluster->backup_assign_cursor = (uint8_t)index;
            break;
        }
    }
    cluster->next_backup_assign_ms = now_ms;
#if !defined(NDEBUG)
    /* Post-commit derive assert: the armed sweep must derive ASSIGNING
     * when the transition fired; without a transition the derive either
     * stays put (STABLE ready-precedence, ASSIGNING self re-arm) or moves
     * to ASSIGNING from the pending write alone (assign_backup() flow:
     * shadow was already ASSIGNING, derive was SYNCING mid-tick). */
    {
        ucn_cluster_phase_t derived =
            cluster_phase_from_legacy_state(cluster, now_ms);

        if (needs_transition) {
            assert(derived == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
        } else {
            assert(derived == pre_phase ||
                   derived == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
        }
    }
#endif
}

void queue_backup_assignment_for_member(ucn_cluster_t *cluster,
                                               ucn_node_id_t member_node_id,
                                               uint32_t now_ms)
{
    size_t index;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        cluster->backup_node_id == 0U) {
        return;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied &&
            cluster->members[index].node_id == member_node_id) {
            /* CLV2-01-04d.7 (MAJOR 1B) + CLV2-01-04d.7.1 (shadow-guard
             * closure): a newly admitted member gets a retriable targeted
             * assignment - arming the sweep IS the SYNCING -> ASSIGNING
             * phase change, so when the LEGACY derives SYNCING the
             * transition is called UNCONDITIONALLY (fail-closed), never
             * skipped on a Shadow mismatch.  pre_phase == ASSIGNING (true
             * self re-arm) keeps the idempotent body.  Do not restart a
             * complete sweep for harmless JOIN retries. */
            {
                ucn_cluster_phase_t pre_phase =
                    cluster_phase_from_legacy_state(cluster, now_ms);
                bool needs_transition =
                    pre_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;

                if (needs_transition) {
                    if (cluster_transition(
                            cluster,
                            UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                            UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                            UCN_CLUSTER_REASON_BACKUP_ASSIGNED,
                            now_ms) != UCN_OK) {
                        /* Fail closed: do NOT arm the targeted assignment. */
                        return;
                    }
                }
                cluster->backup_assign_cursor = (uint8_t)index;
                cluster->backup_assign_remaining = 1U;
                cluster->backup_assign_pending = true;
                cluster->next_backup_assign_ms = now_ms;
#if !defined(NDEBUG)
                {
                    ucn_cluster_phase_t derived =
                        cluster_phase_from_legacy_state(cluster, now_ms);

                    if (needs_transition) {
                        assert(derived ==
                               UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
                    } else {
                        assert(derived == pre_phase ||
                               derived ==
                                   UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
                    }
                }
#endif
            }
            return;
        }
    }
}

void send_backup_assignment_step(ucn_cluster_t *cluster,
                                        uint32_t now_ms)
{
    size_t examined;
    ucn_cluster_phase_t pre_phase;
    bool transition_required;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        cluster->backup_node_id == 0U || !cluster->backup_assign_pending ||
        cluster->backup_assign_remaining == 0U) {
        return;
    }
    if (cluster->next_backup_assign_ms != 0U &&
        !ucn_deadline_expired(now_ms, cluster->next_backup_assign_ms)) {
        return;
    }
    /* CLV2-01-04d.7.1 (shadow-guard closure): the LEGACY decides whether
     * the sweep-done ASSIGNING -> SYNCING transition is required; Shadow
     * is only validated by cluster_transition()/preflight.  A corrupted
     * Shadow must fail-closed, never be bypassed by a shadow-based skip.
     * transition_required is computed PRE-SEND from the legacy derive and
     * reused by the preflight, the post-send commit and the loop-exhausted
     * branch. */
    pre_phase = cluster_phase_from_legacy_state(cluster, now_ms);
    transition_required =
        pre_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
    for (examined = 0U; examined < UCN_CLUSTER_MAX_MEMBERS; ++examined) {
        size_t index = (cluster->backup_assign_cursor + examined) %
                       UCN_CLUSTER_MAX_MEMBERS;

        if (cluster->members[index].occupied) {
            /* CLV2-01-04d.7 (MAJOR 3) + CLV2-01-04d.7.1: the LAST frame of
             * a sweep is preflighted BEFORE it is sent when the LEGACY
             * derives ASSIGNING (unconditional - no shadow guard): if the
             * ASSIGNING -> SYNCING commit cannot run, the frame must not
             * be sent and no sweep state may move (a send-then-reject
             * would strand pending=true with remaining=0 - the next call
             * early-returns forever).  The preflight performs the full
             * validation with ZERO writes; the commit after the send
             * cannot then reject (nothing phase-relevant changes between).
             * CLV2-01-04e M02 note: this preflight->commit window MUST
             * NOT invoke a callback capable of mutating Cluster phase
             * state (send_backup_assign -> send_cluster_message -> the
             * platform send hook is phase-agnostic by contract). */
            if (cluster->backup_assign_remaining == 1U &&
                transition_required &&
                cluster_transition_preflight(
                    cluster,
                    UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                    UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                    now_ms) != UCN_OK) {
                /* Fail closed BEFORE any write: nothing sent, cursor /
                 * remaining / pending untouched; the next step re-visits
                 * the sweep. */
                return;
            }
            {
                ucn_result_t result = send_backup_assign(
                    cluster, cluster->members[index].node_id);

                if (result == UCN_OK) {
                    cluster->backup_assign_cursor =
                        (uint8_t)((index + 1U) % UCN_CLUSTER_MAX_MEMBERS);
                    cluster->backup_assign_remaining--;
                    if (cluster->backup_assign_remaining == 0U) {
                        /* CLV2-01-04d.2 + CLV2-01-04d.7.1 (sweep done): the
                         * last ASSIGN frame was sent, so the sweep commits
                         * as the ASSIGNING -> SYNCING transition BEFORE the
                         * pending=false write.  The decision is the LEGACY
                         * pre_phase (transition_required), NOT a shadow
                         * check: a READY that landed mid-sweep made
                         * pre_phase == STABLE (ready precedence) -> no
                         * transition, idempotent pending=false (phase
                         * unchanged).  The preflight above guarantees this
                         * commit cannot be rejected. */
                        if (transition_required &&
                            cluster_transition(
                                cluster,
                                UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                                UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                                UCN_CLUSTER_REASON_BACKUP_SYNC_STARTED,
                                now_ms) != UCN_OK) {
                            /* Fail closed (defensive - the preflight above
                             * already validated this exact pair): do NOT
                             * clear the sweep; the next step re-visits it. */
                            return;
                        }
                        cluster->backup_assign_pending = false;
                        cluster->next_backup_assign_ms =
                            ucn_deadline_from_now(
                                now_ms, cluster->config.lease_ms);
#if !defined(NDEBUG)
                        /* CLV2-01-04d.2 post-commit derive assert: after the
                         * transition AND the pending=false site write the
                         * legacy state must still derive SYNCING (runs only
                         * when the transition actually fired -
                         * transition_required). */
                        if (transition_required) {
                            assert(cluster_phase_from_legacy_state(
                                       cluster, now_ms) ==
                                   UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
                        }
#endif
                    } else {
                        cluster->next_backup_assign_ms =
                            ucn_deadline_from_now(
                                now_ms, backup_control_spacing_ms(
                                            cluster,
                                            member_count_u16(cluster)));
                    }
                } else {
                    cluster->next_backup_assign_ms =
                        ucn_deadline_from_now(
                            now_ms, cluster->config.token_bucket.refill_ms);
                }
                return;
            }
        }
    }
    /* CLV2-01-04d.2 + CLV2-01-04d.7.1 (loop exhausted - no occupied
     * member left to sweep): same sweep-done transition before the pending
     * clear, decided by the LEGACY pre_phase (transition_required) - NOT a
     * shadow check; unconditional transition when required, fail-closed
     * before pending=false/remaining=0. */
    if (transition_required &&
        cluster_transition(cluster, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                           UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                           UCN_CLUSTER_REASON_BACKUP_SYNC_STARTED,
                           now_ms) != UCN_OK) {
        /* Fail closed: see the sweep-done branch above. */
        return;
    }
    cluster->backup_assign_pending = false;
    cluster->backup_assign_remaining = 0U;
    cluster->next_backup_assign_ms = ucn_deadline_from_now(
        now_ms, cluster->config.lease_ms);
}

/* Head-side Backup control: heartbeat + one snapshot record per step. */
void send_backup_heartbeat(ucn_cluster_t *cluster, uint32_t now_ms)
{
    ucn_cluster_message_t message;

    if (cluster->backup_node_id == 0U ||
        (cluster->next_backup_heartbeat_ms != 0U &&
         !ucn_deadline_expired(now_ms, cluster->next_backup_heartbeat_ms))) {
        return;
    }
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->config.local_node_id;
    /* C07.7 P1: carry the backup generation so a stale heartbeat from a
     * previous Backup generation cannot refresh this one's liveness. */
    message.backup_generation = cluster->backup_generation;
    message.membership_sequence = cluster->membership_sequence;
    (void)send_cluster_message(cluster, cluster->backup_node_id, &message);
    cluster->next_backup_heartbeat_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
}

/* C07.7 P1: after the Backup is READY, refresh one member's current nonce
 * per keepalive interval (round-robin) so the mirror tracks
 * KEEPALIVE-driven nonce advancement without resetting the ready Backup
 * into a full resync (which would strand it mid-takeover if the Primary
 * died during the refresh). */
ucn_result_t send_backup_delta_step(ucn_cluster_t *cluster)
{
    size_t member_count;
    ucn_cluster_message_t message;
    ucn_result_t result;
    uint32_t next_sequence;
    uint32_t now_ms;
    size_t index;
    size_t ordinal = 0U;

    if (cluster->backup_node_id == 0U || !cluster->backup_ready ||
        cluster->backup_assign_pending) {
        return UCN_OK;
    }
    member_count = member_count_u16(cluster);
    if (member_count == 0U) {
        return UCN_OK;
    }
    now_ms = cluster_now(cluster);
    if (cluster->next_backup_delta_ms == 0U) {
        cluster->next_backup_delta_ms = now_ms;
        return UCN_OK;
    }
    if (!ucn_deadline_expired(now_ms, cluster->next_backup_delta_ms)) {
        return UCN_OK;
    }
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->config.local_node_id;
    message.backup_generation = cluster->backup_generation;
    message.flags = UCN_CLUSTER_FLAG_SYNC_DELTA;
    if (cluster_serial_next_checked(cluster->membership_sequence,
                                    &next_sequence) != UCN_OK) {
        return UCN_ERR_EXHAUSTED;
    }
    message.membership_sequence = next_sequence;
    if (cluster->backup_delta_cursor >= member_count) {
        cluster->backup_delta_cursor = 0U;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (!cluster->members[index].occupied) {
            continue;
        }
        if (ordinal == cluster->backup_delta_cursor) {
            message.member_node_id = cluster->members[index].node_id;
            message.member_nonce = cluster->members[index].last_nonce;
            break;
        }
        ordinal++;
    }
    result = send_cluster_message(cluster, cluster->backup_node_id,
                                   &message);
    if (result == UCN_OK) {
        cluster->membership_sequence = next_sequence;
        cluster->backup_delta_cursor =
            (uint8_t)((cluster->backup_delta_cursor + 1U) % member_count);
        cluster->next_backup_delta_ms = ucn_deadline_from_now(
            now_ms, cluster->config.keepalive_interval_ms);
    } else {
        cluster->next_backup_delta_ms = ucn_deadline_from_now(
            now_ms, cluster->config.token_bucket.refill_ms);
    }
    return UCN_OK;
}

uint32_t backup_sync_spacing_ms(const ucn_cluster_t *cluster,
                                       size_t member_count)
{
    uint32_t divisor = (uint32_t)member_count + 2U;
    uint32_t spacing = cluster->config.lease_ms / divisor;

    if (spacing > UCN_CLUSTER_BACKUP_SYNC_MAX_SPACING_MS) {
        spacing = UCN_CLUSTER_BACKUP_SYNC_MAX_SPACING_MS;
    }
    if (spacing < cluster->config.token_bucket.refill_ms) {
        spacing = cluster->config.token_bucket.refill_ms;
    }
    return spacing;
}

ucn_result_t send_backup_snapshot_step(ucn_cluster_t *cluster)
{
    size_t member_count = member_count_u16(cluster);
    ucn_cluster_message_t message;
    ucn_result_t result;
    uint32_t next_sequence;
    uint32_t now_ms;
    size_t index;
    size_t ordinal = 0U;

    if (cluster->backup_node_id == 0U || cluster->backup_ready ||
        cluster->backup_assign_pending ||
        cluster->backup_sync_cursor > member_count + 1U) {
        return UCN_OK;
    }
    now_ms = cluster_now(cluster);
    if (cluster->next_backup_sync_ms != 0U &&
        !ucn_deadline_expired(now_ms, cluster->next_backup_sync_ms)) {
        return UCN_OK;
    }
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->config.local_node_id;
    message.backup_generation = cluster->backup_generation;
    /* Build the next frame without committing sequence/cursor yet. */
    if (cluster_serial_next_checked(cluster->membership_sequence,
                                    &next_sequence) != UCN_OK) {
        return UCN_ERR_EXHAUSTED;
    }
    message.membership_sequence = next_sequence;
    if (cluster->backup_sync_cursor == 0U) {
        message.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN;
    } else if (cluster->backup_sync_cursor <= member_count) {
        for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
            if (!cluster->members[index].occupied) {
                continue;
            }
            if (ordinal == cluster->backup_sync_cursor - 1U) {
                message.member_node_id = cluster->members[index].node_id;
                message.member_lease_ms = cluster->config.lease_ms;
                message.member_nonce = cluster->members[index].last_nonce;
                break;
            }
            ordinal++;
        }
    } else {
        message.flags = UCN_CLUSTER_FLAG_SYNC_END;
    }
    result = send_cluster_message(cluster, cluster->backup_node_id,
                                   &message);
    if (result == UCN_OK) {
        /* Commit only after a real transmit: a Token-Bucket defer keeps
         * the same cursor/sequence so the Backup never sees a gap. */
        cluster->membership_sequence = next_sequence;
        if (cluster->backup_sync_cursor == 0U) {
            cluster->backup_sync_cursor = 1U;
        } else if (cluster->backup_sync_cursor <= member_count) {
            cluster->backup_sync_cursor++;
        } else {
            cluster->backup_sync_cursor = member_count + 2U;
        }
        cluster->next_backup_sync_ms = ucn_deadline_from_now(
            now_ms, backup_sync_spacing_ms(cluster, member_count));
    } else {
        /* Do not consume every step after a token defer; retry after the
         * configured refill interval and leave the sequence untouched. */
        cluster->next_backup_sync_ms = ucn_deadline_from_now(
            now_ms, cluster->config.token_bucket.refill_ms);
    }
    return UCN_OK;
}

/* Reset a running snapshot so the Backup converges to the current members. */
void backup_resync(ucn_cluster_t *cluster)
{
    uint32_t now_ms;
    ucn_cluster_phase_t pre_phase;
    ucn_cluster_phase_t target_phase =
        UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        cluster->backup_node_id == 0U) {
        return;
    }
    now_ms = cluster_now(cluster);
    pre_phase = cluster_phase_from_legacy_state(cluster, now_ms);
    /* CLV2-01-04d.5 + CLV2-01-04d.7 (MAJOR 2): a READY Backup must
     * restart its snapshot - the HEAD_STABLE -> HEAD_BACKUP_* transition
     * runs through the entry point BEFORE the ready=false write
     * (apply_legacy owns the role write; the caller's node_id/
     * assign_pending decide the sub-phase).  The destination is dispatched
     * from the PRE-CALL state: an armed assignment sweep (assign_pending
     * == true - the step's periodic re-assign armed it while ready was
     * still true, so derive stayed STABLE via ready precedence) makes
     * this a REAL direct STABLE -> ASSIGNING transition; otherwise
     * STABLE -> SYNCING.  No end-of-step shadow_sync() minting is relied
     * on.  When the pre-call derive is NOT STABLE (already SYNCING/
     * ASSIGNING because remove_member()/expire_members() cleared ready
     * first, or a resync is already in flight) NO transition runs - the
     * legacy body alone re-arms the snapshot, exactly as before. */
    if (pre_phase == UCN_CLUSTER_PHASE_HEAD_STABLE) {
        target_phase = cluster->backup_assign_pending
                           ? UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING
                           : UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_HEAD_STABLE,
                               target_phase,
                               UCN_CLUSTER_REASON_RESYNC_STARTED,
                               now_ms) != UCN_OK) {
            /* Fail closed: a rejected transition (shadow mismatch /
             * illegal pair / pre-mutated phase fields) leaves every
             * field untouched - do NOT re-arm the snapshot. */
            return;
        }
    }
    cluster->backup_sync_cursor = 0U;
    cluster->backup_ready = false;
    cluster->next_backup_sync_ms = cluster_now(cluster);
    cluster->backup_resync_deadline_ms = ucn_deadline_from_now(
        cluster_now(cluster), cluster->config.lease_ms);
#if !defined(NDEBUG)
    {
        ucn_cluster_phase_t derived =
            cluster_phase_from_legacy_state(cluster, now_ms);

        if (pre_phase == UCN_CLUSTER_PHASE_HEAD_STABLE) {
            /* CLV2-01-04d.7 (MAJOR 2): strict form restored - the
             * STABLE-origin transition + the site's ready=false write
             * must land EXACTLY on the pre-dispatched target (no
             * SYNCING||ASSIGNING relaxation, no end-of-step minting). */
            assert(derived == target_phase);
        } else {
            /* No transition ran: the legacy body alone must NOT move
             * the phase - an already-SYNCING/ASSIGNING head (e.g.
             * assign_backup() left the sweep pending, remove_member()
             * cleared ready first) stays where it was. */
            assert(derived == pre_phase);
        }
    }
#endif
}
