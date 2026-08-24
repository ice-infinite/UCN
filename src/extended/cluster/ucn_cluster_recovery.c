/* UCN Cluster Recovery module.
 *
 * M02 originally extracted the legacy Recovery lifecycle from
 * ucn_cluster.c.  M12 now owns the lineage, bounded-backoff, exact-round
 * membership and Stable-precedence rules in this module.  Cross-module
 * helpers remain private through ucn_cluster_internal.h. */

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

/* CLV2-M12.3: legacy/unknown-lineage contenders still need a CURRENT
 * winner fence.  recovery_nonce is only comparable inside this legacy
 * fallback; lineage-aware candidates continue to use parent Term + Head
 * Node ordering above. */
static bool recovery_legacy_candidate_outranks_current_head(
    const ucn_cluster_t *cluster,
    uint32_t candidate_nonce,
    ucn_node_id_t candidate_node)
{
    if (candidate_nonce != cluster->accepted_recovery_nonce) {
        return candidate_nonce < cluster->accepted_recovery_nonce;
    }
    return candidate_node < cluster->known_recovery_source;
}

static ucn_result_t send_recovery_ack(
    ucn_cluster_t *cluster,
    ucn_node_id_t destination,
    const ucn_cluster_message_t *declare)
{
    ucn_cluster_message_t ack;

    (void)memset(&ack, 0, sizeof(ack));
    ack.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    ack.role = UCN_CLUSTER_ROLE_MEMBER;
    ack.cluster_id = declare->cluster_id;
    ack.term = declare->term;
    ack.head_node_id = destination;
    ack.recovery_nonce = declare->recovery_nonce;
    ack.recovery_parent_cluster_id = declare->recovery_parent_cluster_id;
    return send_cluster_message(cluster, destination, &ack);
}

/* CLV2-M12.2 (MAJOR): a Recovery DECLARE is the only v3 wire evidence a
 * parentless/lagging survivor has after it follows a Recovery Head.  Adopt
 * its lineage only after every format, parent-domain, rank and phase gate has
 * accepted the declaration.  v3 does not carry parent_config_id, so never
 * manufacture one here. */
static void recovery_adopt_lineage_from_declare(
    ucn_cluster_t *cluster,
    const ucn_cluster_message_t *message)
{
    if (cluster == NULL || message == NULL ||
        message->recovery_parent_cluster_id == 0U) {
        return;
    }
    if (cluster->parent_cluster_id == 0U) {
        /* This is newly learned lineage evidence.  Do not alter round or
         * Config here: the v3 DECLARE proves only parent identity and Term;
         * round escalation remains owned by the recovery timeout path. */
        cluster->parent_cluster_id = message->recovery_parent_cluster_id;
        cluster->parent_term = message->term;
        return;
    }
    if (cluster->parent_cluster_id == message->recovery_parent_cluster_id &&
        message->term > cluster->parent_term) {
        /* Same-parent evidence is forward-only.  A stale DECLARE can never
         * lower lineage even if it reached this helper in a future path. */
        cluster->parent_term = message->term;
    }
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

ucn_result_t start_recovery_backoff(ucn_cluster_t *cluster, uint32_t now_ms)
{
    uint32_t next_recovery_nonce;
    ucn_result_t result;

    result = cluster_serial_next_checked(cluster->recovery_nonce,
                                         &next_recovery_nonce);
    if (result != UCN_OK) {
        /* CLV2-13-13: an exhausted Recovery replay domain is not allowed to
         * wrap through the generic message nonce generator.  The caller may
         * only continue after a persisted identity rotation/rekey. */
        if (cluster->phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) {
            if (cluster_transition(
                    cluster, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE,
                    UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                    UCN_CLUSTER_REASON_RECOVERY_SERIAL_EXHAUSTED,
                    now_ms) != UCN_OK) {
                return UCN_ERR_STATE;
            }
        }
        cluster->recovery_backoff_deadline_ms = 0U;
        return result;
    }
    cluster->recovery_nonce = next_recovery_nonce;
    cluster->recovery_backoff_deadline_ms = ucn_deadline_from_now(
        now_ms, compute_recovery_backoff(cluster));
    return UCN_OK;
}

void declare_recovery_head(ucn_cluster_t *cluster,
                           const ucn_cluster_epoch_t *durable_epoch,
                           uint32_t now_ms)
{
    if (cluster->recovery_nonce == 0U) {
        uint32_t first_nonce;

        /* declare_recovery_after_persistence preflights this path before
         * changing phase. Keep the assignment checked here as a second,
         * no-wrap safety boundary. */
        if (cluster_serial_next_checked(0U, &first_nonce) != UCN_OK) {
            return;
        }
        cluster->recovery_nonce = first_nonce;
    }
    /* CLV2-M12.1 (MAJOR-1): the RAM/wire epoch IS the durable promise.
     * The Term is adopted from the persisted recovery Epoch - it is never
     * recomputed from mutable lineage state, so persist-before-promise
     * (M04) and the published authority Epoch can never diverge. */
    cluster->recovery_cluster_id = durable_epoch->cluster_id;
    cluster->cluster_id = durable_epoch->cluster_id;
    cluster->term = durable_epoch->term;
    cluster->head_node_id = durable_epoch->head_node_id;
    cluster->current_head_score = cluster->config.head_score;
    cluster->role_since_ms = now_ms;
    cluster->election_deadline_ms = 0U;
    cluster->recovery_backoff_deadline_ms = 0U;
    /* A Recovery identity owns a distinct membership round.  No slot from a
     * prior Recovery ID (or an abandoned stable mirror) is evidence for this
     * new authority domain; every member must ACK the current nonce again. */
    primary_member_table_clear(cluster);
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
    uint32_t next_round;

    /* CLV2-M12 (12-03): every TTL expiry or arbitration loss escalates the
     * attempt counter, which both escalates the exponential backoff and
     * derives a fresh Recovery ID (12-02).  A sustained stable join resets
     * the round via cluster_lineage_reset(). */
    if (cluster_serial_next_checked(cluster->recovery_round, &next_round) ==
        UCN_OK) {
        cluster->recovery_round = next_round;
    } else {
        /* Fail closed at the reserved serial boundary. A later M13 runtime
         * owner may persist a fresh identity domain, but this Recovery FSM
         * must never roll the round back to zero/one on its own. */
        if (cluster->phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) {
            (void)cluster_transition(
                cluster, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE,
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                UCN_CLUSTER_REASON_RECOVERY_SERIAL_EXHAUSTED, now_ms);
        }
    }
    cluster->recovery_cooldown_until_ms = ucn_deadline_from_now(
        now_ms, cluster->config.recovery_observation_ms);
    cluster->recovery_cluster_id = 0U;
    cluster->recovery_deadline_ms = 0U;
    cluster->recovery_backoff_deadline_ms = 0U;
    cluster->recovery_ack_count = 0U;
    cluster->recovery_acked = 0U;
    primary_member_table_clear(cluster);
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
    bool phase_committed = false;
    bool follows_recovery_head;

    if (message->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD ||
        message->head_node_id != source ||
        message->term == 0U) {
        return UCN_ERR_MALFORMED;
    }
    /* Only a headless Member/Backup/DETACHED node acknowledges a recovery
     * declaration; a node already under a live Head stays put.  A
     * RECOVERY_HEAD also participates so two contenders can converge on
     * the deterministic winner (see the arbitration below). */
    if (ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_MEMBER &&
        ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_BACKUP &&
        ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_DETACHED &&
        ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        return UCN_ERR_ACCESS;
    }
    follows_recovery_head =
        ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_MEMBER &&
        ucn_cluster_recovery_scoped(cluster) &&
        cluster->known_recovery_source != 0U &&
        cluster->accepted_recovery_nonce != 0U;
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_MEMBER &&
        !ucn_cluster_recovery_scoped(cluster) &&
        !ucn_deadline_expired(now_ms, cluster->head_lease_expires_at_ms)) {
        return UCN_ERR_ACCESS;
    }
    /* A Backup whose Primary lease is still live belongs to a Stable
     * authority domain.  A temporary Recovery declaration cannot peel it
     * away before the existing Backup FSM has fenced that Primary.  Once
     * the lease has expired (and takeover is not already active), the
     * headless Backup may use the legacy Recovery join path. */
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_BACKUP &&
        (cluster_phase_backup_takeover_active(cluster->phase) ||
         cluster->backup_primary_lease_deadline_ms == 0U ||
         !ucn_deadline_expired(now_ms,
                               cluster->backup_primary_lease_deadline_ms))) {
        return UCN_ERR_ACCESS;
    }
    /* CLV2-M12 (12-06): a recovery-domain member rejects an old or
     * inconsistent exact-round DECLARE from its CURRENT Head.
     * recovery_nonce is a monotonic per-node counter (next_nonce), so a
     * smaller nonce is delayed old round -> REPLAY.  An equal nonce must
     * also preserve recovery ID, Term and parent lineage.  Rejection may
     * increment stale statistics, but cannot refresh the lease or alter
     * Recovery membership/identity.  A strictly larger nonce is the Head's
     * next round and follows the normal rank/join path below. */
    if (follows_recovery_head &&
        message->cluster_id == cluster->recovery_cluster_id &&
        (source != cluster->known_recovery_source ||
         message->recovery_nonce != cluster->accepted_recovery_nonce ||
         message->term != cluster->term ||
         message->recovery_parent_cluster_id != cluster->parent_cluster_id)) {
        /* One Recovery ID denotes one exact round/Head/Epoch.  Reusing it
         * with another source, nonce, Term or lineage is an identity
         * collision, not a new round. */
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    if (follows_recovery_head &&
        cluster->known_recovery_source == source &&
        message->cluster_id != cluster->recovery_cluster_id &&
        message->recovery_nonce <= cluster->accepted_recovery_nonce) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    /* Once a node has authoritative lineage evidence, an unscoped legacy
     * candidate cannot erase that domain and a same-parent older Term cannot
     * recruit an idle survivor merely because it has not started its own
     * recovery backoff yet.  The former implementation applied this rank
     * fence only to active contenders/current followers, leaving the
     * headless-but-lineaged window open. */
    if (cluster->parent_cluster_id != 0U &&
        message->recovery_parent_cluster_id == 0U) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    if (cluster->parent_cluster_id != 0U &&
        message->recovery_parent_cluster_id == cluster->parent_cluster_id &&
        message->term < cluster->parent_term) {
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
    if (follows_recovery_head && source != cluster->known_recovery_source) {
        bool yield_to_candidate;

        /* CLV2-M12.1 (MAJOR-2): a Member that already follows a winner
         * only switches when the incoming candidate outranks the CURRENT
         * accepted Head.  Equal known lineage ranks by parent_term DESC,
         * node ASC.  If local lineage is still unknown, the documented
         * legacy (nonce,node) ordering is used against the accepted Head,
         * never against this member's own candidacy.  Once lineage is
         * known, an unknown-parent frame cannot downgrade that evidence. */
        if (cluster->parent_cluster_id != 0U &&
            message->recovery_parent_cluster_id == 0U) {
            cluster->stats.stale_messages++;
            return UCN_ERR_REPLAY;
        }
        if (cluster->parent_cluster_id != 0U &&
            message->recovery_parent_cluster_id != 0U) {
            yield_to_candidate = recovery_candidate_outranks_current_head(
                cluster, message->term, source);
        } else {
            yield_to_candidate =
                recovery_legacy_candidate_outranks_current_head(
                    cluster, message->recovery_nonce, source);
        }
        if (!yield_to_candidate) {
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
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
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
    if (follows_recovery_head &&
        cluster->accepted_recovery_nonce == message->recovery_nonce &&
        cluster->known_recovery_source == source &&
        cluster->recovery_cluster_id == message->cluster_id &&
        cluster->term == message->term &&
        cluster->parent_cluster_id == message->recovery_parent_cluster_id) {
        cluster->head_lease_expires_at_ms =
            ucn_deadline_from_now(now_ms, message->recovery_ttl_ms);
        cluster->head_grace_deadline_ms = 0U;
        /* A lost/backpressured first ACK must be recoverable from the
         * periodic re-declaration.  The Head side is idempotent, so every
         * exact refresh re-sends the ACK. */
        return send_recovery_ack(cluster, source, message);
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
            cluster->phase;

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
    /* All rejecting rank/phase transitions are above.  The accepted
     * declaration is now trustworthy lineage evidence for a parentless or
     * lagging survivor; adopt it before committing the Recovery join. */
    recovery_adopt_lineage_from_declare(cluster, message);
    cluster->accepted_recovery_nonce = message->recovery_nonce;
    cluster->known_recovery_source = source;
    /* C07.7 P0-1: actually join the recovery Cluster.  Its fresh ID was
     * allocated by the declaring node's M03-08 provider; it never
     * impersonates the lost Cluster.  The member keeps its role_since/lease
     * so the recovery domain has a live membership. */
    if (ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_MEMBER ||
        cluster->cluster_id != message->cluster_id ||
        cluster->recovery_cluster_id != message->cluster_id) {
        cluster->recovery_cluster_id = message->cluster_id;
        cluster->cluster_id = message->cluster_id;
        cluster->term = message->term;
        cluster->head_node_id = source;
        cluster->current_head_score = cluster->config.head_score;
        cluster->role_since_ms = now_ms;
        cluster->election_deadline_ms = 0U;
    }
    cluster->head_lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, message->recovery_ttl_ms);
    cluster->head_grace_deadline_ms = 0U;
    /* A stable JOIN_ACCEPT timer must never survive a direct lease-expired
     * switch into a Recovery domain and later erase the newly adopted
     * lineage. */
    cluster->lineage_reset_deadline_ms = 0U;
    /* Joining a recovery Head abandons our own recovery candidacy. */
    cluster->recovery_backoff_deadline_ms = 0U;
#if !defined(NDEBUG)
    /* CLV2-01-04f.4 post-commit derive assert: after the transition AND
     * every site side effect the legacy state must still derive
     * MEMBER_ACTIVE (derive depends only on role/grace/eligible). */
    assert(cluster->phase ==
           UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
#endif
    return send_recovery_ack(cluster, source, message);
}

ucn_result_t handle_recovery_ack(ucn_cluster_t *cluster,
                                          ucn_node_id_t source,
                                          const ucn_cluster_message_t *message,
                                          uint32_t now_ms)
{
    ucn_cluster_member_t *member;

    if (ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_RECOVERY_HEAD ||
        message->role != UCN_CLUSTER_ROLE_MEMBER ||
        message->cluster_id != cluster->recovery_cluster_id ||
        message->head_node_id != cluster->config.local_node_id) {
        return UCN_ERR_ACCESS;
    }
    if (message->term != cluster->term) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    /* CLV2-M12.3: an ACK is an exact-round membership proof.  Legacy zero
     * nonce cannot be accepted by a live Recovery Head; parent 0 remains a
     * valid exact value only for a genuinely parentless Recovery domain. */
    if (message->recovery_nonce != cluster->recovery_nonce) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    if (message->recovery_parent_cluster_id != cluster->parent_cluster_id) {
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
