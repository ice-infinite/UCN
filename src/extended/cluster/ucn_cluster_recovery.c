/* UCN CLV2-M02 (02-06): Cluster Recovery module.
 *
 * STRUCTURAL REFACTOR ONLY (M02 mandate): the RECOVERY_HEAD quorum /
 * declaration / arbitration / TTL lifecycle moved verbatim from the
 * former single ucn_cluster.c.  Every function body is UNCHANGED; M01
 * (human sign-off ab53b31/a7f4841) froze the semantics including the
 * zero-backoff degenerate spin (a documented Current deficiency, M12
 * will fix it - do NOT change it here).  Do NOT "optimize" anything.
 *
 * Cross-module (via ucn_cluster_internal.h, all de-static only):
 *   - calls into fsm / membership / ucn_cluster.c core as declared.
 */

#include "ucn/ucn_cluster.h"

#include <assert.h>
#include <string.h>

#include "ucn/ucn_time.h"

#include "ucn_cluster_internal.h"


uint32_t compute_recovery_backoff(const ucn_cluster_t *cluster)
{
    /* Lower Node ID declares first in the same visibility domain.  The
     * recovery nonce participates in the conflict arbitration below
     * (handlers compare (nonce, node_id) lexicographically), so the
     * backoff only needs to break the initial tie. */
    return cluster->config.local_node_id %
           cluster->config.recovery_backoff_max_ms;
}

/* C07.7 P0-2: a Recovery Head may only be declared when this node can see
 * a meaningful part of the headless domain.  A Backup that still holds a
 * membership mirror requires a visible majority of that mirror; a plain
 * member requires at least one visible ADMITTED peer so a completely
 * isolated node can never self-declare. */
bool recovery_quorum_met(const ucn_cluster_t *cluster)
{
    size_t index;
    size_t member_index;
    uint16_t mirror_count = member_count_u16(cluster);
    uint16_t visible_mirror = 0U;
    uint16_t required;

    for (index = 0U; index < UCN_CLUSTER_MAX_PEERS; ++index) {
        bool in_mirror = false;

        if (!cluster->peers[index].occupied ||
            cluster->peers[index].neighbor_state != UCN_NEIGHBOR_ADMITTED) {
            continue;
        }
        for (member_index = 0U; member_index < UCN_CLUSTER_MAX_MEMBERS;
             ++member_index) {
            if (cluster->members[member_index].occupied &&
                cluster->members[member_index].node_id ==
                    cluster->peers[index].node_id) {
                in_mirror = true;
                break;
            }
        }
        if (in_mirror) {
            visible_mirror++;
        }
    }
    if (mirror_count == 0U) {
        size_t visible_any = 0U;

        /* No membership mirror (plain member): any visible ADMITTED peer
         * proves this node is not fully isolated. */
        for (index = 0U; index < UCN_CLUSTER_MAX_PEERS; ++index) {
            if (cluster->peers[index].occupied &&
                cluster->peers[index].neighbor_state ==
                    UCN_NEIGHBOR_ADMITTED) {
                visible_any++;
            }
        }
        return visible_any >= 1U;
    }
    required = (uint16_t)(mirror_count / 2U + 1U);
    return visible_mirror >= required;
}

void send_recovery_declare(ucn_cluster_t *cluster)
{
    ucn_cluster_message_t message;
    size_t index;


    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = 1U;
    message.head_node_id = cluster->config.local_node_id;
    message.recovery_nonce = cluster->recovery_nonce;
    message.recovery_ttl_ms = cluster->config.recovery_head_ttl_ms;
    for (index = 0U; index < UCN_CLUSTER_MAX_PEERS; ++index) {
        if (cluster->peers[index].occupied &&
            cluster->peers[index].neighbor_state == UCN_NEIGHBOR_ADMITTED) {
            (void)send_cluster_message(cluster, cluster->peers[index].node_id,
                                       &message);
        }
    }
}

void start_recovery_backoff(ucn_cluster_t *cluster, uint32_t now_ms)
{
    cluster->recovery_nonce = next_nonce(cluster);
    cluster->recovery_backoff_deadline_ms = ucn_deadline_from_now(
        now_ms, compute_recovery_backoff(cluster));
}

void declare_recovery_head(ucn_cluster_t *cluster, uint32_t recovery_cluster_id,
                           uint32_t now_ms)
{
    if (cluster->recovery_nonce == 0U) {
        cluster->recovery_nonce = next_nonce(cluster);
    }
    cluster->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    cluster->recovery_cluster_id = recovery_cluster_id;
    cluster->cluster_id = cluster->recovery_cluster_id;
    cluster->term = 1U;
    cluster->head_node_id = cluster->config.local_node_id;
    cluster->current_head_score = cluster->config.head_score;
    cluster->role_since_ms = now_ms;
    cluster->election_deadline_ms = 0U;
    cluster->recovery_backoff_deadline_ms = 0U;
    cluster->recovery_ack_count = 0U;
    cluster->recovery_acked = 0U;
    cluster->recovery_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.recovery_head_ttl_ms);
    cluster->next_advertise_ms = now_ms;
    (void)send_recovery_declare(cluster);
    cluster->stats.elections_started++;
}

void stepdown_recovery_head(ucn_cluster_t *cluster, uint32_t now_ms)
{
    cluster->recovery_cooldown_until_ms = ucn_deadline_from_now(
        now_ms, cluster->config.recovery_observation_ms);
    cluster->recovery_cluster_id = 0U;
    cluster->recovery_deadline_ms = 0U;
    cluster->recovery_backoff_deadline_ms = 0U;
    cluster->recovery_ack_count = 0U;
    cluster->recovery_acked = 0U;
    cluster->accepted_recovery_nonce = 0U;
    cluster->known_recovery_source = 0U;
    /* Keep recovery_eligible so a still-headless domain re-backs off with
     * bounded retries instead of silently giving up (§7.2). */
    set_detached(cluster, now_ms, cluster->config.recovery_observation_ms);
}

ucn_result_t handle_recovery_declare(ucn_cluster_t *cluster,
                                              ucn_node_id_t source,
                                              const ucn_cluster_message_t *message,
                                              uint32_t now_ms)
{
    ucn_cluster_message_t ack;
    bool phase_committed = false;

    if (message->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD ||
        message->head_node_id != source ||
        message->term == 0U) {
        return UCN_ERR_MALFORMED;
    }
    /* Only a headless Member/Backup/DETACHED node acknowledges a recovery
     * declaration; a node already under a live Head stays put.  A
     * RECOVERY_HEAD also participates so two contenders can converge on
     * the deterministic winner (see the arbitration below). */
    if (cluster->role != UCN_CLUSTER_ROLE_MEMBER &&
        cluster->role != UCN_CLUSTER_ROLE_BACKUP &&
        cluster->role != UCN_CLUSTER_ROLE_DETACHED &&
        cluster->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        return UCN_ERR_ACCESS;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
        cluster->recovery_cluster_id == 0U &&
        !ucn_deadline_expired(now_ms, cluster->head_lease_expires_at_ms)) {
        return UCN_ERR_ACCESS;
    }
    /* C07.7 P0-3: deterministic (recovery_nonce, node-id) arbitration.
     * A node that already started its own recovery backoff only defers to
     * a strictly smaller (nonce, node_id) contender; a node that has not
     * started backoff yet (recovery_nonce == 0) always accepts.  The
     * comparison is anti-symmetric so two contenders can never both join
     * each other and never both keep declaring.  A RECOVERY_HEAD that sees
     * a strictly smaller contender yields the role and joins it. */
    if (cluster->recovery_nonce != 0U &&
        !(message->recovery_nonce < cluster->recovery_nonce ||
          (message->recovery_nonce == cluster->recovery_nonce &&
           source < cluster->config.local_node_id))) {
        return UCN_OK; /* we keep contending; ignore this candidate */
    }
    if (cluster->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        /* CLV2-01-04f.4: losing the Recovery arbitration to a strictly
         * smaller (nonce, node_id) contender IS the RECOVERY_HEAD ->
         * MEMBER_ACTIVE transition (RECOVERY_YIELDED, a DIRECT edge).  It
         * runs FIRST through the single entry point; apply_legacy writes
         * role=MEMBER + grace=0 + eligible=false.  On rejection nothing
         * changes: the node stays RECOVERY_HEAD and a later smaller
         * contender may still win (tested). */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_RECOVERY_HEAD,
                               UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                               UCN_CLUSTER_REASON_RECOVERY_YIELDED,
                               now_ms) != UCN_OK) {
            return UCN_ERR_STATE;
        }
        phase_committed = true;
        /* Yield the temporary Head role before joining the winner.  The
         * cooldown arm, the recovery clears and set_detached()'s role
         * rewrite are redundant-but-harmless after apply_legacy
         * (role=MEMBER); they stay site-owned in original order. */
        stepdown_recovery_head(cluster, now_ms);
    }
    /* Re-declaration of the same recovery Head refreshes the member
     * lease (the Recovery Head re-advertises periodically). */
    if (cluster->accepted_recovery_nonce == message->recovery_nonce &&
        cluster->known_recovery_source == source) {
        cluster->head_lease_expires_at_ms =
            ucn_deadline_from_now(now_ms, message->recovery_ttl_ms);
        cluster->head_grace_deadline_ms = 0U;
        return UCN_OK;
    }
    /* CLV2-01-04f.4: the plain join.  Every headless source derives a
     * pre-phase (RECOVERY_OBSERVE / RECOVERY_ELECTION / DETACHED_OBSERVE
     * / BACKUP_SYNCING / BACKUP_READY / BACKUP_TAKEOVER - the M01.0.2
     * takeover_active && syncing combo derives BACKUP_TAKEOVER and must
     * never be rejected for phase reasons - or MEMBER_TAKEOVER_GRACE)
     * and commits -> MEMBER_ACTIVE (RECOVERY_WIN) through the single
     * entry point BEFORE any join-block write, fail-closed.  A MEMBER
     * with an expired lease already derives MEMBER_ACTIVE: self, no
     * transition (the join refresh below keeps it MEMBER_ACTIVE).  The
     * yield path above already committed (phase_committed), so it must
     * not run a second transition against the now-stale shadow.  The
     * accepted_recovery_nonce/known_recovery_source writes below are NOT
     * phase-relevant (derive reads role/eligible/backoff only), but they
     * run AFTER the transition so a rejection leaves them untouched too. */
    if (!phase_committed) {
        ucn_cluster_phase_t pre_phase =
            cluster_phase_from_legacy_state(cluster, now_ms);

        if (pre_phase != UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            if (cluster_transition(cluster, pre_phase,
                                   UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                                   UCN_CLUSTER_REASON_RECOVERY_WIN,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition leaves every field
                 * untouched (accepted_recovery_nonce/known_recovery_
                 * source included) - the node stays headless and a later
                 * declaration may still be accepted. */
                return UCN_ERR_STATE;
            }
        }
    }
    cluster->accepted_recovery_nonce = message->recovery_nonce;
    cluster->known_recovery_source = source;
    /* C07.7 P0-1: actually join the recovery Cluster.  Its fresh ID was
     * allocated by the declaring node's M03-08 provider; it never
     * impersonates the lost Cluster.  The member keeps its role_since/lease
     * so the recovery domain has a live membership. */
    if (cluster->role != UCN_CLUSTER_ROLE_MEMBER ||
        cluster->recovery_cluster_id != message->cluster_id) {
        cluster->recovery_cluster_id = message->cluster_id;
        cluster->cluster_id = message->cluster_id;
        cluster->term = message->term;
        cluster->head_node_id = source;
        cluster->current_head_score = cluster->config.head_score;
        cluster->role = UCN_CLUSTER_ROLE_MEMBER;
        cluster->role_since_ms = now_ms;
        cluster->election_deadline_ms = 0U;
    }
    cluster->head_lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, message->recovery_ttl_ms);
    cluster->head_grace_deadline_ms = 0U;
    /* Joining a recovery Head abandons our own recovery candidacy. */
    cluster->recovery_eligible = false;
    cluster->recovery_backoff_deadline_ms = 0U;
#if !defined(NDEBUG)
    /* CLV2-01-04f.4 post-commit derive assert: after the transition AND
     * every site side effect the legacy state must still derive
     * MEMBER_ACTIVE (derive depends only on role/grace/eligible). */
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
#endif
    (void)memset(&ack, 0, sizeof(ack));
    ack.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    ack.role = UCN_CLUSTER_ROLE_MEMBER;
    ack.cluster_id = message->cluster_id;
    ack.term = message->term;
    ack.head_node_id = source;
    return send_cluster_message(cluster, source, &ack);
}

ucn_result_t handle_recovery_ack(ucn_cluster_t *cluster,
                                          ucn_node_id_t source,
                                          const ucn_cluster_message_t *message,
                                          uint32_t now_ms)
{
    ucn_cluster_member_t *member;

    if (cluster->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD ||
        message->cluster_id != cluster->recovery_cluster_id ||
        message->head_node_id != cluster->config.local_node_id) {
        return UCN_ERR_ACCESS;
    }
    /* C07.7 P0-1: track the acknowledged member so the recovery Cluster
     * has an actual membership to maintain and to hand to a future
     * takeover/stepdown.  A repeated ACK only refreshes the lease.
     * Recovery uses every member slot directly: the declaring node was
     * likely a plain member with member_capacity 0, so the normal
     * capacity-gated allocate_member() would always refuse survivors. */
    member = find_member(cluster, source);
    if (member == NULL) {
        size_t index;

        for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
            if (!cluster->members[index].occupied) {
                (void)memset(&cluster->members[index], 0,
                             sizeof(cluster->members[index]));
                cluster->members[index].occupied = true;
                cluster->members[index].node_id = source;
                member = &cluster->members[index];
                break;
            }
        }
        if (member == NULL) {
            return UCN_ERR_NO_SPACE;
        }
        if (cluster->recovery_ack_count != UINT8_MAX) {
            cluster->recovery_ack_count++;
        }
    }
    member->lease_expires_at_ms = ucn_deadline_from_now(
        now_ms, cluster->config.recovery_head_ttl_ms);
    return UCN_OK;
}
