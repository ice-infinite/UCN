/* UCN CLV2-M02 (02-04): Cluster membership module.
 *
 * STRUCTURAL REFACTOR ONLY (M02 mandate): Join / Keepalive / Leave /
 * member-table allocation & expiry moved verbatim from the former
 * single ucn_cluster.c.  Every function body is UNCHANGED; M01 froze
 * the FSM semantics.  Do NOT "optimize" anything here.
 *
 * Cross-module: the member-table / join / keepalive entry points are
 * exposed via ucn_cluster_internal.h (de-static only); the send
 * infrastructure (send_message / send_cluster_message / next_nonce)
 * and the backup-domain helpers it calls (backup_resync /
 * assign_backup / queue_backup_assignment_for_member) stay in
 * ucn_cluster.c and are declared in the internal header.
 */

#include "ucn/ucn_cluster.h"

#include <assert.h>
#include <string.h>

#include "ucn/ucn_time.h"

#include "ucn_cluster_internal.h"


uint16_t member_count_u16(const ucn_cluster_t *cluster)
{
    size_t index;
    uint16_t count = 0U;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied) {
            ++count;
        }
    }
    return count;
}

uint16_t available_capacity(const ucn_cluster_t *cluster)
{
    uint16_t used = member_count_u16(cluster);

    return used >= cluster->config.member_capacity ?
               0U : (uint16_t)(cluster->config.member_capacity - used);
}

ucn_cluster_member_t *find_member(
    ucn_cluster_t *cluster,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied &&
            cluster->members[index].node_id == node_id) {
            return &cluster->members[index];
        }
    }
    return NULL;
}

ucn_cluster_member_t *allocate_member(
    ucn_cluster_t *cluster,
    ucn_node_id_t node_id)
{
    size_t index;
    ucn_cluster_member_t *member = find_member(cluster, node_id);

    if (member != NULL) {
        return member;
    }
    if (member_count_u16(cluster) >= cluster->config.member_capacity) {
        return NULL;
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

void remove_member(ucn_cluster_t *cluster, ucn_node_id_t node_id,
                          uint32_t now_ms)
{
    ucn_cluster_member_t *member = find_member(cluster, node_id);

    if (cluster->backup_node_id == node_id) {
        /* CLV2-01-04d.4: preflight pattern (human auditor design) for the
         * backup-eviction branch - the FIRST irreversible-site wiring.
         * The preflight validates the PRE-CALL state with ZERO writes, so
         * a rejected validation (shadow desync / illegal pair / pre-mutated
         * phase fields) aborts BEFORE the member slot or the backup fields
         * are touched.  The transition then commits BEFORE the phase-
         * relevant clears: cluster_transition_validate() re-derives
         * old_phase from the legacy fields at commit time, so a site that
         * already cleared backup_node_id would be rejected as
         * 'pre-mutated'.  apply_legacy(HEAD_NO_BACKUP) writes role +
         * node_id=0 + ready=false, so the site's own clears below are
         * idempotent (d.0 framework note) and run in the original order. */
        ucn_cluster_phase_t old_phase =
            cluster_phase_from_legacy_state(cluster, now_ms);

        if (cluster_transition_preflight(cluster, old_phase,
                                         UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                                         now_ms) != UCN_OK) {
            /* Fail closed: NOTHING is touched - the member slot stays
             * occupied and the backup identity is untouched. */
            return;
        }
        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                               UCN_CLUSTER_REASON_BACKUP_LOST,
                               now_ms) != UCN_OK) {
            /* Fail closed: a rejected commit also leaves every field
             * untouched (nothing was mutated yet). */
            return;
        }
        /* Current irreversible side effects in original order: free the
         * member slot, clear the backup identity (idempotent with
         * apply_legacy), then resync (early-returns: node_id == 0). */
        if (member != NULL) {
            (void)memset(member, 0, sizeof(*member));
        }
        cluster->backup_node_id = 0U;
        cluster->backup_ready = false;
        backup_resync(cluster);
#if !defined(NDEBUG)
        /* CLV2-01-04d.4 post-commit derive assert: after the transition
         * AND the site clears the legacy state must derive HEAD_NO_BACKUP. */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
#endif
        return;
    }
    if (member != NULL) {
        (void)memset(member, 0, sizeof(*member));
    }
    backup_resync(cluster);
}

void clear_members(ucn_cluster_t *cluster)
{
    (void)memset(cluster->members, 0, sizeof(cluster->members));
}

ucn_result_t handle_join_request(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    const ucn_cluster_message_t *message,
    uint32_t now_ms)
{
    ucn_cluster_member_t *member;
    bool member_was_present;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        message->head_node_id != cluster->config.local_node_id ||
        message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term) {
        cluster->stats.joins_rejected++;
        return UCN_ERR_ACCESS;
    }
    member = allocate_member(cluster, source);
    if (member == NULL) {
        cluster->stats.joins_rejected++;
        (void)send_join_reply(cluster, source, UCN_CLUSTER_MSG_JOIN_REJECT,
                              message->nonce);
        return UCN_ERR_NO_SPACE;
    }
    member_was_present = member->last_nonce != 0U;
    if (member->last_nonce != 0U && message->nonce <= member->last_nonce) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    member->last_nonce = message->nonce;
    member->lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    cluster->stats.joins_accepted++;
    assign_backup(cluster, now_ms);
    if (!member_was_present) {
        backup_resync(cluster);
        queue_backup_assignment_for_member(cluster, source, now_ms);
    }
    /* C07.7 P1: echo the request nonce (join txid). */
    return send_join_reply(cluster, source, UCN_CLUSTER_MSG_JOIN_ACCEPT,
                           message->nonce);
}

ucn_result_t send_join_reply(ucn_cluster_t *cluster,
                                    ucn_node_id_t destination,
                                    ucn_cluster_message_type_t type,
                                    uint32_t join_nonce)
{
    ucn_cluster_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.type = type;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->config.local_node_id;
    message.head_score = cluster->config.head_score;
    message.available_capacity = available_capacity(cluster);
    message.lease_ms = cluster->config.lease_ms;
    message.nonce = join_nonce;
    return send_cluster_message(cluster, destination, &message);
}

ucn_result_t handle_join_accept(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    const ucn_cluster_message_t *message,
    uint32_t now_ms)
{
    {
        bool pre_assigned_backup = (cluster->role == UCN_CLUSTER_ROLE_BACKUP &&
                                     cluster->backup_syncing);

        if (!pre_assigned_backup &&
            cluster->role != UCN_CLUSTER_ROLE_JOIN_PENDING) {
            return UCN_ERR_ACCESS;
        }
        if (source != cluster->pending_head_node_id ||
            message->head_node_id != source ||
            message->cluster_id != cluster->pending_cluster_id ||
            message->term != cluster->pending_term ||
            /* C07.7 P1: the accept must echo the exact join txid. */
            message->nonce != cluster->pending_join_nonce) {
            return UCN_ERR_ACCESS;
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
        /* CLV2-01-04b.4: a pre-assigned Backup node (BACKUP_ASSIGN(self)
         * won the race against this late JOIN_ACCEPT) must NOT transition
         * - it only refreshes the epoch fields below (keep current
         * behaviour; the shadow stays BACKUP_SYNCING). */
    } else {
        /* CLV2-01-04b.4: the role write IS the JOIN_PENDING ->
         * MEMBER_ACTIVE transition.  The runtime pre-derive (shadow ==
         * JOIN_PENDING + legacy derives JOIN_PENDING) rejects in Debug if
         * a site pre-mutated phase-relevant fields; fail closed BEFORE
         * the epoch refresh. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                               UCN_CLUSTER_REASON_JOIN_ACCEPTED,
                               now_ms) != UCN_OK) {
            /* Fail closed per the migration contract: a rejected
             * transition (shadow mismatch / illegal pair / pre-mutated
             * phase fields) leaves every field untouched, so do NOT run
             * the accept side effects on a non-MEMBER node. */
            return UCN_ERR_STATE;
        }
    }
    cluster->role_since_ms = now_ms;
    cluster->cluster_id = message->cluster_id;
    cluster->term = message->term;
    cluster->head_node_id = source;
    cluster->current_head_score = message->head_score;
    cluster->head_lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, message->lease_ms);
    cluster->head_grace_deadline_ms = 0U;
    cluster->next_keepalive_ms = now_ms;
    cluster->pending_head_node_id = 0U;
    cluster->pending_cluster_id = 0U;
    cluster->pending_term = 0U;
    cluster->stats.joins_accepted++;
#if !defined(NDEBUG)
    /* CLV2-01-04b.4 post-commit derive assert: after the transition AND
     * every site side effect the join path must still derive
     * MEMBER_ACTIVE (derive depends only on role == MEMBER with no armed
     * grace deadline).  The pre-assigned Backup path performs no
     * transition, so its (unchanged) BACKUP_SYNCING shadow is untouched. */
    if (cluster->role != UCN_CLUSTER_ROLE_BACKUP) {
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    }
#endif
    return UCN_OK;
}

ucn_result_t handle_keepalive(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    const ucn_cluster_message_t *message,
    uint32_t now_ms)
{
    ucn_cluster_member_t *member;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        message->head_node_id != cluster->config.local_node_id ||
        message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term) {
        return UCN_ERR_ACCESS;
    }
    member = find_member(cluster, source);
    if (member == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (message->nonce <= member->last_nonce) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    member->last_nonce = message->nonce;
    member->lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    return UCN_OK;
}

void expire_members(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    bool changed = false;
    bool backup_expired = false;

    /* CLV2-01-04d.4: preflight pattern for the backup-expiry branch.  The
     * backup's expiry is detected on the PRE-CALL state (read-only), the
     * transition is validated + committed BEFORE the phase-relevant clears
     * (the d.0 derive check rejects a post-mutation commit; apply_legacy
     * clears node_id/ready, so the eviction loop's own clears below are
     * idempotent), and the Current irreversible side effects run after in
     * original order.  A non-backup expiry keeps the legacy path below: no
     * transition, no shadow write. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->backup_node_id != 0U &&
            cluster->members[index].occupied &&
            cluster->members[index].node_id == cluster->backup_node_id &&
            ucn_deadline_expired(now_ms,
                                 cluster->members[index].lease_expires_at_ms)) {
            backup_expired = true;
            break;
        }
    }

    if (backup_expired) {
        ucn_cluster_phase_t old_phase =
            cluster_phase_from_legacy_state(cluster, now_ms);

        if (cluster_transition_preflight(cluster, old_phase,
                                         UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                                         now_ms) != UCN_OK) {
            /* Fail closed: a rejected preflight leaves every member slot
             * AND the backup fields untouched (no partial eviction). */
            return;
        }
        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                               UCN_CLUSTER_REASON_BACKUP_LOST,
                               now_ms) != UCN_OK) {
            /* Fail closed: a rejected commit leaves every field untouched. */
            return;
        }
    }

    /* Current irreversible side effects in original order: evict every
     * expired member (the Backup included when it expired), clear the
     * backup identity, then resync. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied &&
            ucn_deadline_expired(now_ms,
                                 cluster->members[index].lease_expires_at_ms)) {
            ucn_node_id_t expired_id = cluster->members[index].node_id;

            (void)memset(&cluster->members[index], 0,
                         sizeof(cluster->members[index]));
            cluster->stats.member_leases_expired++;
            changed = true;
            if (cluster->backup_node_id == expired_id) {
                cluster->backup_node_id = 0U;
                cluster->backup_ready = false;
            }
        }
    }
    if (changed) {
        backup_resync(cluster);
    }
#if !defined(NDEBUG)
    if (backup_expired) {
        /* CLV2-01-04d.4 post-commit derive assert: after the transition
         * AND the eviction loop the legacy state must derive
         * HEAD_NO_BACKUP. */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    }
#endif
}

void send_join_request(ucn_cluster_t *cluster, uint32_t now_ms)
{
    ucn_cluster_message_t message;
    ucn_result_t result;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_REQUEST;
    message.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    message.cluster_id = cluster->pending_cluster_id;
    message.term = cluster->pending_term;
    message.head_node_id = cluster->pending_head_node_id;
    message.head_score = cluster->pending_head_score;
    message.lease_ms = cluster->config.lease_ms;
    /* C07.7 P1: the request nonce is the join transaction id;
     * JOIN_ACCEPT/JOIN_REJECT must echo it so a stale reject of an
     * earlier attempt cannot abort a newer one. */
    message.nonce = next_nonce(cluster);
    cluster->pending_join_nonce = message.nonce;
    result = send_cluster_message(cluster, cluster->pending_head_node_id,
                                   &message);

    if (result == UCN_OK) {
        cluster->stats.joins_requested++;
    }
    cluster->next_join_retry_ms =
        ucn_deadline_from_now(now_ms, cluster->config.join_retry_ms);
}

void send_keepalive(ucn_cluster_t *cluster, uint32_t now_ms)
{
    (void)send_message(cluster, cluster->head_node_id,
                       UCN_CLUSTER_MSG_KEEPALIVE, UCN_CLUSTER_ROLE_MEMBER,
                       cluster->cluster_id, cluster->term,
                       cluster->head_node_id, cluster->current_head_score, 0U);
    cluster->next_keepalive_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
}

