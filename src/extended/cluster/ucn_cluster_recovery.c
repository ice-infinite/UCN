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


/* CLV2-M12.1 (MAJOR-2): a Recovery Member that already follows a
 * winner ranks an incoming candidate from a DIFFERENT source against its
 * CURRENT accepted Head, never against itself.  v3 wire subset:
 * parent_term DESC (the member's Term mirrors the accepted Head's Term),
 * then head_node_id ASC. */
static bool recovery_candidate_outranks_current_head(
    const ucn_cluster_t *cluster,
    uint32_t candidate_term,
    ucn_node_id_t candidate_node)
{
    if (candidate_term != cluster->term) {
        return candidate_term > cluster->term;
    }
    return candidate_node < cluster->known_recovery_source;
}

/* CLV2-M12 (12-04): pure deterministic Recovery rank comparator.  Terms
 * are compared ONLY between equal non-zero parents (the M03 milestone
 * gate); different parents are UNRANKABLE and go to ordinary Merge
 * (M11 handover), never to a recovery-arbitration cross-yield. */
ucn_cluster_recovery_rank_relation_t ucn_cluster_recovery_rank_compare(
    const ucn_cluster_recovery_rank_t *a,
    const ucn_cluster_recovery_rank_t *b)
{
    if (a == NULL || b == NULL || a->parent_cluster_id == 0U ||
        b->parent_cluster_id == 0U ||
        a->parent_cluster_id != b->parent_cluster_id) {
        return UCN_CLUSTER_RECOVERY_RANK_UNRANKABLE;
    }
    if (a->parent_term != b->parent_term) {
        return a->parent_term > b->parent_term
                   ? UCN_CLUSTER_RECOVERY_RANK_A_WINS
                   : UCN_CLUSTER_RECOVERY_RANK_B_WINS;
    }
    if (a->parent_config_id != b->parent_config_id) {
        return a->parent_config_id > b->parent_config_id
                   ? UCN_CLUSTER_RECOVERY_RANK_A_WINS
                   : UCN_CLUSTER_RECOVERY_RANK_B_WINS;
    }
    if (a->score != b->score) {
        return a->score > b->score ? UCN_CLUSTER_RECOVERY_RANK_A_WINS
                                   : UCN_CLUSTER_RECOVERY_RANK_B_WINS;
    }
    if (a->node_id != b->node_id) {
        return a->node_id < b->node_id ? UCN_CLUSTER_RECOVERY_RANK_A_WINS
                                       : UCN_CLUSTER_RECOVERY_RANK_B_WINS;
    }
    return UCN_CLUSTER_RECOVERY_RANK_EQUAL;
}

uint32_t compute_recovery_backoff(const ucn_cluster_t *cluster)
{
    uint32_t base = cluster->config.recovery_backoff_base_ms;
    uint32_t exponent;
    uint32_t exponential;
    uint32_t jitter;
    uint32_t backoff;

    /* CLV2-M12 (12-03): bounded exponential escalation - attempt 0 waits
     * base, attempt 1 waits 2x, ... capped at 16x and then clamped to the
     * configured maximum.  This replaces the M01 zero-backoff degenerate
     * spin (node_id % max could compute 0 and never leave RECOVERY_OBSERVE).
     * The base is validated non-zero and <= max, so the result is always a
     * non-zero valid duration. */
    exponent = cluster->recovery_round < 4U ? cluster->recovery_round : 4U;
    exponential = base;
    while (exponent != 0U) {
        if (exponential >= cluster->config.recovery_backoff_max_ms / 2U) {
            exponential = cluster->config.recovery_backoff_max_ms;
            break;
        }
        exponential <<= 1U;
        exponent--;
    }
    /* Deterministic jitter derived from (parent, round, node): every node
     * and round computes the same value repeatedly, but different nodes /
     * rounds de-synchronise, so partitioned islands do not re-collide at a
     * fixed interval.  Bounded to one quarter of the base. */
    jitter = cluster_id_mix(cluster->parent_cluster_id ^
                            (cluster->recovery_round << 16U) ^
                            cluster->config.local_node_id) %
             (base / 4U + 1U);
    backoff = exponential + jitter;
    if (backoff > cluster->config.recovery_backoff_max_ms) {
        backoff = cluster->config.recovery_backoff_max_ms;
    }
    return backoff;
}

/* C07.7 P0-2: a Recovery Head may only be declared when this node can see
 * a meaningful part of the headless domain.  A Backup that still holds a
 * membership mirror requires a visible REMOTE majority of that mirror -
 * this is a conservative isolation threshold (the Backup itself is not one
 * of its own peers), deliberately NOT a Config-majority/quorum claim; a
 * plain member requires at least one visible ADMITTED peer so a completely
 * isolated node can never self-declare. */
bool recovery_quorum_met(const ucn_cluster_t *cluster)
{
    size_t index;
    size_t member_index;
    uint16_t mirror_count = primary_member_protected_voter_count_u16(cluster);
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
            if (primary_member_is_protected_voter(
                    &cluster->primary_members.slots[member_index]) &&
                cluster->primary_members.slots[member_index].node_id ==
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

        /* CLV2-M12 (12-08): plain member (no Backup mirror): the product
         * configures min_recovery_peers (default 1, which forbids a fully
         * isolated self-declaration); a higher value raises the island
         * formation bar.  A Backup with a mirror keeps the DISTINCT
         * majority threshold below. */
        for (index = 0U; index < UCN_CLUSTER_MAX_PEERS; ++index) {
            if (cluster->peers[index].occupied &&
                cluster->peers[index].neighbor_state ==
                    UCN_NEIGHBOR_ADMITTED) {
                visible_any++;
            }
        }
        return visible_any >= cluster->config.min_recovery_peers;
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
    message.term = cluster->term; /* mirrors parent_term when lineage is set */
    message.head_node_id = cluster->config.local_node_id;
    message.recovery_nonce = cluster->recovery_nonce;
    message.recovery_ttl_ms = cluster->config.recovery_head_ttl_ms;
    /* CLV2-M12 (12-04): same-parent rank arbitration needs the parent
     * identity on the wire (0 = legacy frame, ranked via the old
     * (nonce, node_id) fallback). */
    message.recovery_parent_cluster_id = cluster->parent_cluster_id;
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

void declare_recovery_head(ucn_cluster_t *cluster,
                           const ucn_cluster_epoch_t *durable_epoch,
                           uint32_t now_ms)
{
    if (cluster->recovery_nonce == 0U) {
        cluster->recovery_nonce = next_nonce(cluster);
    }
    /* CLV2-M12.1 (MAJOR-1): the RAM/wire epoch IS the durable promise.
     * The Term is adopted from the persisted recovery Epoch - it is never
     * recomputed from mutable lineage state, so persist-before-promise
     * (M04) and the published authority Epoch can never diverge. */
    cluster->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    cluster->recovery_cluster_id = durable_epoch->cluster_id;
    cluster->cluster_id = durable_epoch->cluster_id;
    cluster->term = durable_epoch->term;
    cluster->head_node_id = durable_epoch->head_node_id;
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
    /* CLV2-M12 (12-03): every TTL expiry or arbitration loss escalates the
     * attempt counter, which both escalates the exponential backoff and
     * derives a fresh Recovery ID (12-02).  A sustained stable join resets
     * the round via cluster_lineage_reset(). */
    cluster->recovery_round++;
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
    /* CLV2-M12 (12-06): a recovery-domain member rejects an old-round
     * DECLARE from its CURRENT Head.  recovery_nonce is a monotonic
     * per-node counter (next_nonce), so the same source with a smaller
     * nonce is a delayed old round -> REPLAY with zero writes; the same
     * nonce with a different cluster_id is inconsistent -> REPLAY.  A
     * strictly larger nonce is the Head's next round and follows the
     * normal rank/join path below. */
    if (cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
        cluster->recovery_cluster_id != 0U &&
        cluster->known_recovery_source == source &&
        message->recovery_nonce != 0U &&
        (message->recovery_nonce < cluster->accepted_recovery_nonce ||
         (message->recovery_nonce == cluster->accepted_recovery_nonce &&
          message->cluster_id != cluster->recovery_cluster_id))) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    /* CLV2-M12 (12-04): lineage-aware deterministic rank arbitration.
     * - Legacy frames (no parent lineage on the wire, or no local
     *   lineage) keep the M01 (recovery_nonce, node_id) fallback.
     * - Lineage frames from a DIFFERENT parent are never rank-merged:
     *   each island keeps contending and ordinary Merge (M11 handover)
     *   owns cross-parent convergence.  Terms are never compared across
     *   different parents (M03 milestone gate).
     * - Same-parent lineage frames rank by parent_term DESC, then
     *   node_id ASC.  (score/config are not carried on the frozen v3
     *   DECLARE wire; the full comparator contract serves local/v4
     *   ranking - see ucn_cluster_recovery_rank_compare.) */
    if (message->recovery_parent_cluster_id != 0U &&
        cluster->parent_cluster_id != 0U &&
        message->recovery_parent_cluster_id != cluster->parent_cluster_id) {
        return UCN_OK; /* different parent: keep contending, never cross-yield */
    }
    if (message->recovery_parent_cluster_id != 0U &&
        cluster->parent_cluster_id != 0U &&
        cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
        cluster->recovery_cluster_id != 0U &&
        cluster->known_recovery_source != 0U &&
        source != cluster->known_recovery_source) {
        /* CLV2-M12.1 (MAJOR-2): a Member that already follows a winner
         * only switches when the incoming candidate outranks the CURRENT
         * accepted Head (parent_term DESC, node ASC).  A delayed loser
         * can never pull an already-converged island apart again. */
        if (!recovery_candidate_outranks_current_head(cluster, message->term,
                                                      source)) {
            cluster->stats.stale_messages++;
            return UCN_ERR_REPLAY;
        }
        /* The candidate outranks the current Head: fall through to the
         * join block and switch. */
    } else if (message->recovery_parent_cluster_id != 0U &&
               cluster->parent_cluster_id != 0U &&
               cluster->recovery_nonce != 0U) {
        /* Same-parent lineage arbitration for contenders (a node that has
         * started its own backoff ranks the candidate against ITSELF).  A
         * non-contending node that is not already following a winner still
         * always accepts (legacy rule). */
        bool yield_to_candidate =
            (message->term > cluster->parent_term) ||
            (message->term == cluster->parent_term &&
             source < cluster->config.local_node_id);

        if (!yield_to_candidate) {
            return UCN_OK; /* we keep contending; ignore this candidate */
        }
    } else if (cluster->recovery_nonce != 0U &&
               !(message->recovery_nonce < cluster->recovery_nonce ||
                 (message->recovery_nonce == cluster->recovery_nonce &&
                  source < cluster->config.local_node_id))) {
        return UCN_OK; /* legacy fallback: we keep contending */
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
    /* CLV2-M12 (12-06): the ACK echoes the exact declare round (nonce) and
     * lineage binding (parent), so the Head can reject old-round ACKs. */
    ack.recovery_nonce = message->recovery_nonce;
    ack.recovery_parent_cluster_id = message->recovery_parent_cluster_id;
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
    /* CLV2-M12 (12-06): the ACK is bound to the exact declare round and
     * lineage.  A non-zero nonce that differs from the current round is an
     * old-round ACK -> REPLAY with zero writes.  A non-zero parent binding
     * that differs from ours is a different-parent island -> ACCESS.
     * Zero fields keep the legacy/staged frame tolerance. */
    if (message->recovery_nonce != 0U &&
        message->recovery_nonce != cluster->recovery_nonce) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    if (message->recovery_parent_cluster_id != 0U &&
        message->recovery_parent_cluster_id != cluster->parent_cluster_id) {
        return UCN_ERR_ACCESS;
    }
    /* C07.7 P0-1: track the acknowledged member so the recovery Cluster
     * has an actual membership to maintain and to hand to a future
     * takeover/stepdown.  A repeated ACK only refreshes the lease.
     * Recovery uses every member slot directly: the declaring node was
     * likely a plain member with member_capacity 0, so the normal
     * capacity-gated primary_member_allocate() would always refuse survivors. */
    member = primary_member_find(cluster, source);
    if (member == NULL) {
        size_t index;

        for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
            if (!cluster->primary_members.slots[index].occupied) {
                if (!member_initialize_legacy(&cluster->primary_members.slots[index],
                                              source, now_ms,
                                              cluster->config.provisional_timeout_ms)) {
                    return UCN_ERR_ACCESS;
                }
                member = &cluster->primary_members.slots[index];
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
    member_note_legacy_keepalive(member, now_ms);
    return UCN_OK;
}
