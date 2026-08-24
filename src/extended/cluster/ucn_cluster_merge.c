/* UCN CLV2-M02 (02-06): Cluster merge / head-offer module.
 *
 * STRUCTURAL REFACTOR ONLY (M02 mandate): the candidate table, score
 * comparison, ordered stepdown and the head-offer / score-switch logic
 * moved verbatim from the former single ucn_cluster.c.  Every function
 * body is UNCHANGED; M01 froze the FSM semantics.  Do NOT
 * "optimize" anything here.
 *
 * CLV2-M03 (03-03, human-audited semantic change): the HEAD branch of
 * consider_head_offer() now classifies offers by Epoch relation
 * (ucn_cluster_epoch_compare) instead of raw Term.  Foreign Clusters
 * truncate the comparison domain - their Term is NEVER authority
 * (Cluster A term 2 must not surrender to Cluster B term 100); every
 * same-cluster relation funnels into one authority path.  This is the
 * ONLY 03-03 semantic delta: all other branches are untouched.
 *
 * Cross-module (via ucn_cluster_internal.h, all de-static only):
 *   - calls into fsm / membership / backup / ucn_cluster.c core.
 */

#include "ucn/ucn_cluster.h"

#include <assert.h>
#include <string.h>

#include "ucn/ucn_time.h"

#include "ucn_cluster_internal.h"

#include "ucn/ucn_cluster_authority.h"

/* CLV2-M02 (02-06): intra-module forward declarations - the extracted
 * function order differs from the former single file, so the helpers
 * used before their definitions are declared here (bodies untouched). */
bool score_improves_by(uint16_t candidate_score, uint16_t current_score,
                              uint8_t percent);
void send_head_stepdown(ucn_cluster_t *cluster);

ucn_result_t backup_challenge(ucn_cluster_t *cluster, uint32_t now_ms)
{
    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)now_ms;
    /* CLV2-M11 (11-09): a live Backup must never manufacture a competing
     * same-Cluster Term merely because its local score is better.  Planned
     * leadership transfer requires the separately gated v4 handover
     * transaction, exact Backup identity and durable target Epoch proof.
     * This default-product compatibility entry therefore remains a no-write,
     * fail-closed stub until a future audited owner connects that protocol. */
    return UCN_ERR_UNSUPPORTED;
}

static void begin_ordered_stepdown_with_reason(
    ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms,
    ucn_cluster_transition_reason_t reason)
{
    /* CLV2-01-04d.6 (HEAD_* sources) + CLV2-01-04f (RECOVERY_HEAD offer
     * source, SITE B): a Head source yields through the entry point BEFORE
     * any phase-relevant write - the transition (STEPDOWN_ORDERED) commits
     * first and apply_legacy(STEPPING_DOWN) owns the role write; the site
     * keeps eligible=false / backoff=0 / stepdown_deadline in their
     * original order.  The legacy reclaim event decides THAT the stepdown
     * runs; cluster_transition() validates whether the shadow agrees (a
     * caller NEVER uses the shadow to decide whether to SKIP the call). */
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_HEAD ||
        ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        ucn_cluster_phase_t old_phase =
            cluster->phase;

        if (old_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP ||
            old_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING ||
            old_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING ||
            old_phase == UCN_CLUSTER_PHASE_HEAD_STABLE ||
            old_phase == UCN_CLUSTER_PHASE_RECOVERY_HEAD) {
            if (cluster_transition(cluster, old_phase,
                                   UCN_CLUSTER_PHASE_STEPPING_DOWN,
                                    reason,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition (shadow mismatch /
                 * illegal pair / pre-mutated phase fields) leaves every
                 * field untouched - do NOT yield or notify members. */
                return;
            }
        }
    }
    /* Yielding to a stable Head abandons any Recovery candidacy. */
    cluster->recovery_backoff_deadline_ms = 0U;
    (void)send_head_stepdown(cluster);
    cluster->role_since_ms = now_ms;
    cluster->stepdown_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
    cluster->pending_head_node_id = candidate->head_node_id;
    cluster->pending_cluster_id = candidate->cluster_id;
    cluster->pending_term = candidate->term;
    cluster->pending_head_score = candidate->head_score;
    cluster->stats.head_switches++;
#if !defined(NDEBUG)
    /* CLV2-01-04d.6/01-04f post-commit derive assert: after the
     * transition AND every site effect the node must derive STEPPING_DOWN
     * (the role write at the site is idempotent with
     * apply_legacy(STEPPING_DOWN), so the assert holds for both the
     * HEAD_* and RECOVERY_HEAD sources). */
    assert(cluster->phase ==
           UCN_CLUSTER_PHASE_STEPPING_DOWN);
#endif
}

void begin_ordered_stepdown(ucn_cluster_t *cluster,
                            const ucn_cluster_candidate_t *candidate,
                            uint32_t now_ms)
{
    begin_ordered_stepdown_with_reason(cluster, candidate, now_ms,
                                       UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
}

bool candidate_better(
    uint16_t candidate_score,
    ucn_node_id_t candidate_node,
    uint16_t current_score,
    ucn_node_id_t current_node)
{
    return candidate_score > current_score ||
           (candidate_score == current_score && candidate_node < current_node);
}

/* CLV2-M03 (03-03): the unified SAME-CLUSTER authority path for Head
 * offers.  Every same-cluster Epoch relation (HIGHER / LOWER / SAME /
 * CONFLICT) funnels through here - a higher Term wins immediately, a
 * stale Term is ignored, and a same-Term offer (SAME or CONFLICT) is
 * arbitrated by the deterministic score/Node-ID ordering, repeated
 * samples and minimum tenure.  CONFLICT (same cluster, same term,
 * different Head) is deliberately NOT a foreign merge (03-05 owns the
 * final conflict handling; until then the score arbitration below
 * decides, exactly as the pre-03-03 same-term path did). */
static void classify_same_cluster_authority(
    ucn_cluster_t *cluster,
    ucn_cluster_candidate_t *candidate,
    uint32_t now_ms,
    ucn_cluster_epoch_relation_t relation)
{
    /* relation is (local, remote): LOWER means the REMOTE offer has the
     * higher Term (the local epoch is lower) - the newer-generation Head
     * wins immediately (S5.3: the older Head must defer rather than
     * reclaim by raw score).  HIGHER means the remote is stale. */
    if (relation == UCN_CLUSTER_EPOCH_RELATION_LOWER) {
        begin_ordered_stepdown(cluster, candidate, now_ms);
        return;
    }
    if (relation == UCN_CLUSTER_EPOCH_RELATION_HIGHER) {
        /* A stale Head must not be followed, even with a high score. */
        return;
    }
    /* SAME / CONFLICT: same Cluster, same Term.  Packet loss can let two
     * candidates finish the same local election.  A worse Head must
     * eventually yield, otherwise that transient split brain becomes
     * permanent.  The deterministic score/Node-ID ordering, repeated
     * samples and minimum tenure keep this convergence bounded without
     * making a single RSSI sample flap an established Head. */
    if (!score_improves_by(candidate->head_score,
                           cluster->config.head_score,
                           cluster->config.switch_improvement_percent)) {
        candidate->better_samples = 0U;
        return;
    }
    if (candidate->better_samples < UINT8_MAX) {
        candidate->better_samples++;
    }
    if (candidate->better_samples >=
            cluster->config.switch_required_samples &&
        ucn_elapsed_at_least(now_ms, cluster->role_since_ms,
                             cluster->config.head_min_tenure_ms)) {
        begin_ordered_stepdown(cluster, candidate, now_ms);
        candidate->better_samples = 0U;
    }
}

/* CLV2-M03 (03-03): a FOREIGN Cluster's Head offer carries NO authority
 * over this Cluster - the node NEVER surrenders to it and its Term is
 * NEVER compared with ours (the M03 milestone gate: terms of different
 * cluster_ids are never directly compared).  Cross-cluster merge
 * negotiation is a later OP; until then the foreign offer is observed
 * and deliberately ignored (no state mutation, no score bookkeeping). */
static void classify_foreign_cluster_merge(
    ucn_cluster_t *cluster,
    ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    (void)cluster;
    (void)candidate;
    (void)now_ms;
}

/* CLV2-M03 (03-04): all active local roles consume a protected,
 * same-Cluster, strictly newer stable-Head offer before ordinary RX dispatch.
 * The Epoch comparator is the only authority classifier here: FOREIGN,
 * SAME, HIGHER (remote stale) and CONFLICT deliberately fall through to the
 * normal handler.  CONFLICT is owned by 03-05, never resolved here.
 *
 * This remains a Current-FSM migration, not an early implementation of
 * Target fencing/persistence: HEAD/RECOVERY_HEAD retain the existing ordered
 * stepdown behavior, while MEMBER/BACKUP start the existing join procedure.
 * What changes is that all those paths are selected once, at RX priority,
 * and their Phase transition carries the same HIGHER_AUTHORITY reason. */
static bool begin_higher_authority_join(
    ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    ucn_cluster_phase_t old_phase;

    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_TERM_CONFLICT) {
        /* TERM_CONFLICT_WAIT is deliberately sticky for same-Term traffic.
         * A strictly higher same-Cluster Term is the first permitted exit in
         * this Current-FSM stage; M08 adds complementary Authority fencing
         * but does not replace this control-plane safe wait. */
        if (cluster_transition(cluster,
                               UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT,
                               UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_REASON_HIGHER_AUTHORITY,
                               now_ms) == UCN_OK) {
            begin_join_prepare_fields(cluster, candidate, now_ms);
        }
        return true;
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_JOIN_PENDING) {
        /* The phase is already JOIN_PENDING.  Retarget its pending epoch
         * without manufacturing a self-transition. */
        begin_join(cluster, candidate, now_ms);
        return true;
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_STEPPING_DOWN) {
        /* Duplicate offers for the Head already selected by the ordered
         * yield must not collapse its grace window.  A strictly newer pending
         * Term, however, supersedes the old target immediately. */
        if (candidate->cluster_id == cluster->pending_cluster_id &&
            candidate->term <= cluster->pending_term) {
            return true;
        }
        /* The old ordered-yield target is no longer authoritative once a
         * protected newer Term of the same Cluster arrives.  Reuse the
         * existing STEPPING_DOWN -> JOIN_PENDING edge and replace the pending
         * epoch immediately instead of waiting for the obsolete deadline. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_STEPPING_DOWN,
                               UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_REASON_HIGHER_AUTHORITY,
                               now_ms) == UCN_OK) {
            begin_join_prepare_fields(cluster, candidate, now_ms);
        }
        return true;
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_CANDIDATE) {
        /* A Candidate already owns a local same-Cluster Election Epoch.
         * A protected remote higher Term must use the same global authority
         * transition as Member/Backup, rather than falling through to the
         * generic Candidate join path with JOIN_INITIATED. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_ELECTION,
                               UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_REASON_HIGHER_AUTHORITY,
                               now_ms) == UCN_OK) {
            begin_join_prepare_fields(cluster, candidate, now_ms);
        }
        return true;
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_MEMBER) {
        old_phase = cluster->phase;
        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_REASON_HIGHER_AUTHORITY,
                               now_ms) == UCN_OK) {
            begin_join_prepare_fields(cluster, candidate, now_ms);
        }
        /* A rejected transition is fail-closed.  It was still the terminal
         * higher-authority event; normal score routing must not run after it. */
        return true;
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_BACKUP) {
        old_phase = cluster->phase;
        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_REASON_HIGHER_AUTHORITY,
                               now_ms) != UCN_OK) {
            return true;
        }
        /* Preserve the existing Backup abandonment ordering: clear the
         * mirror only after its phase has committed, then seed the pending
         * target.  M09 later replaces this Current mirror with staging. */
        backup_clear_sync(cluster, now_ms);
        begin_join(cluster, candidate, now_ms);
        return true;
    }
    return false;
}

bool process_term_conflict(
    ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    ucn_cluster_epoch_t local_epoch;
    ucn_cluster_epoch_t remote_epoch;
    ucn_cluster_phase_t old_phase;

    if (cluster == NULL || candidate == NULL ||
        candidate->head_node_id == cluster->config.local_node_id) {
        return false;
    }
    local_epoch = ucn_cluster_active_epoch_get(cluster);
    remote_epoch.cluster_id = candidate->cluster_id;
    remote_epoch.term = candidate->term;
    remote_epoch.head_node_id = candidate->head_node_id;
    if (ucn_cluster_epoch_compare(&local_epoch, &remote_epoch) !=
        UCN_CLUSTER_EPOCH_RELATION_CONFLICT) {
        return false;
    }
    if (cluster->authority_runtime != NULL) {
        /* The M08 Owner fences before legacy phase cleanup.  This safety-only
         * observation never grants v3 authority; it closes the current Head
         * TX window while M05 keeps v4 production RX/FSM disabled. */
        (void)ucn_cluster_authority_runtime_note_term_conflict(
            cluster->authority_runtime, candidate->cluster_id, candidate->term,
            candidate->head_node_id, now_ms);
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_TERM_CONFLICT) {
        /* Idempotent: additional advertisements from either same-Term Head
         * cannot re-enable control actions or manufacture transitions. */
        return true;
    }
    old_phase = cluster->phase;
    if (old_phase == UCN_CLUSTER_PHASE_DISABLED ||
        old_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE ||
        old_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE ||
        old_phase == UCN_CLUSTER_PHASE_RECOVERY_ELECTION) {
        return false;
    }
    /* Safety dominates score, Node ID and ordinary lifecycle handlers.  A
     * rejection remains terminal so no normal handler can continue after a
     * shadow/legacy mismatch. */
    (void)cluster_transition(cluster, old_phase,
                             UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT,
                             UCN_CLUSTER_REASON_TERM_CONFLICT, now_ms);
    return true;
}

bool process_higher_authority(
    ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    ucn_cluster_epoch_t local_epoch;
    ucn_cluster_epoch_t remote_epoch;

    if (cluster == NULL || candidate == NULL ||
        candidate->head_node_id == cluster->config.local_node_id) {
        return false;
    }
    local_epoch = ucn_cluster_active_epoch_get(cluster);
    remote_epoch.cluster_id = candidate->cluster_id;
    remote_epoch.term = candidate->term;
    remote_epoch.head_node_id = candidate->head_node_id;
    /* compare(local, remote) == LOWER means exactly that remote has the
     * higher same-Cluster Term.  Do not spell this as raw term arithmetic:
     * the comparator cuts FOREIGN domains before any Term comparison. */
    if (ucn_cluster_epoch_compare(&local_epoch, &remote_epoch) !=
        UCN_CLUSTER_EPOCH_RELATION_LOWER) {
        return false;
    }
    if (cluster->authority_runtime != NULL) {
        /* Current v3 offers are only a safety observation.  They cannot
         * claim the M10 frozen-Config certificate needed for JOIN_PENDING. */
        (void)ucn_cluster_authority_runtime_note_higher_term_observed(
            cluster->authority_runtime, candidate->cluster_id, candidate->term,
            candidate->head_node_id, now_ms);
    }
    /* A protected, replay-admitted higher same-Cluster Term is a safety
     * observation even when the following Current-FSM transition later
     * rejects on a shadow mismatch.  Keep it before any role-local cleanup
     * so a subsequent detach cannot accept the older epoch again. */
    cluster_history_note_stable_epoch(cluster, candidate->cluster_id,
                                      candidate->term,
                                      candidate->head_node_id);
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_TERM_CONFLICT) {
        return begin_higher_authority_join(cluster, candidate, now_ms);
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_HEAD ||
        ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        begin_ordered_stepdown_with_reason(
            cluster, candidate, now_ms,
            UCN_CLUSTER_REASON_HIGHER_AUTHORITY);
        return true;
    }
    return begin_higher_authority_join(cluster, candidate, now_ms);
}

void consider_head_offer(
    ucn_cluster_t *cluster,
    ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    ucn_cluster_epoch_t local_epoch;
    ucn_cluster_epoch_t remote_epoch;

    if (candidate->head_node_id == cluster->config.local_node_id) {
        return;
    }
    if (cluster_history_offer_is_stale(cluster, candidate)) {
        cluster->stats.stale_messages++;
        return;
    }
    /* RX calls this only after its global pre-dispatch.  Keep the same
     * terminal gate here as well because the test hook deliberately invokes
     * this helper directly.  This prevents a second role-local higher-Term
     * implementation from drifting away from process_higher_authority(). */
    if (process_term_conflict(cluster, candidate, now_ms) ||
        process_higher_authority(cluster, candidate, now_ms)) {
        return;
    }
    /* A Candidate can already own a same-Cluster Election Epoch.  Do not let
     * the legacy role-local join path retarget it to an older Head offer:
     * only a remote HIGHER Term may cause the global authority transition.
     * FOREIGN stays outside this comparison domain and detached nodes still
     * discover a first Cluster normally. */
    local_epoch = ucn_cluster_active_epoch_get(cluster);
    remote_epoch.cluster_id = candidate->cluster_id;
    remote_epoch.term = candidate->term;
    remote_epoch.head_node_id = candidate->head_node_id;
    if (ucn_cluster_epoch_compare(&local_epoch, &remote_epoch) ==
        UCN_CLUSTER_EPOCH_RELATION_HIGHER) {
        cluster->stats.stale_messages++;
        return;
    }
    /* CLV2-M12 (12-07): stable precedence.  A recovery-domain Member that
     * sees a legal stable Head offer of its own parent lineage reclaims
     * to it via the ordered JOIN_PENDING path - no score comparison, no
     * capacity gate, no 11-08 Member freeze.  The 03-06 stale gate has
     * already rejected lower-Term parent offers, so term >= parent_term
     * is a legal stable successor.  Foreign stable Heads remain excluded
     * (they are not the parent lineage). */
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_MEMBER &&
        ucn_cluster_recovery_scoped(cluster) &&
        cluster->parent_cluster_id != 0U &&
        candidate->cluster_id == cluster->parent_cluster_id &&
        candidate->term >= cluster->parent_term) {
        ucn_cluster_phase_t reclaim_phase =
            cluster->phase;

        if (cluster_transition(cluster, reclaim_phase,
                               UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_REASON_STABLE_RECLAIM,
                               now_ms) == UCN_OK) {
            begin_join_prepare_fields(cluster, candidate, now_ms);
        }
        return;
    }
    /* A full Head must keep refreshing existing members.  Capacity zero only
     * rejects new joins; treating it as an unavailable current Head causes
     * valid members to expire their lease and create a split brain. */
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_MEMBER &&
        candidate->head_node_id == cluster->head_node_id &&
        candidate->cluster_id == cluster->cluster_id &&
        candidate->term == cluster->term) {
        /* CLV2-01-04c.2: a same-cluster same-term Head offer while the
         * node is in takeover grace IS the MEMBER_TAKEOVER_GRACE ->
         * MEMBER_ACTIVE lease-renewal transition: run it FIRST (fail
         * closed) and keep the site's lease refresh + grace=0 writes in
         * original order.  A MEMBER_ACTIVE node performs no transition
         * (the grace=0 write is then a no-op); apply_legacy writes
         * role+grace=0 for the GRACE inbound. */
        if (cluster->head_grace_deadline_ms != 0U) {
            if (cluster_transition(cluster,
                                   UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                                   UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                                   UCN_CLUSTER_REASON_HEAD_LEASE_RENEWED,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition (shadow mismatch /
                 * illegal pair / pre-mutated phase fields) leaves every
                 * field untouched - do NOT refresh the lease. */
                return;
            }
        }
        cluster->head_lease_expires_at_ms =
            ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
        cluster->head_grace_deadline_ms = 0U;
        cluster->current_head_score = candidate->head_score;
        candidate->better_samples = 0U;
#if !defined(NDEBUG)
        /* CLV2-01-04c.2 post-commit derive assert: after the transition
         * (when applicable) and every site write the legacy state must
         * still derive MEMBER_ACTIVE (role == MEMBER, no armed grace). */
        assert(cluster->phase ==
               UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
#endif
        return;
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_BACKUP) {
        if (candidate->head_node_id == cluster->backup_primary_node_id &&
            candidate->cluster_id == cluster->cluster_id &&
            candidate->term == cluster->term) {
            /* A protected Head ADVERTISE is independent liveness evidence
             * in addition to the direct Primary heartbeat.  Refreshing the
             * lease here prevents a Backup from falsely taking over merely
             * because several heartbeat unicasts were lost on a live link. */
            cluster->backup_primary_lease_deadline_ms =
                ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
            /* CLV2-M11 (11-09): score is only discovery evidence while a
             * primary is live.  Do not use it to bump a Term or promote a
             * Backup: only the isolated planned-handover owner may later
             * execute a verified, persisted transfer. */
            candidate->better_samples = 0U;
        }
        return;
    }
    /* Packet loss can let two candidates finish the same local election.  A
     * worse Head must eventually yield, otherwise that transient split brain
     * becomes permanent.  The deterministic score/Node-ID ordering, repeated
     * samples and minimum tenure keep this convergence bounded without making
     * a single RSSI sample flap an established Head.  Member notification is
     * deliberately lease-based in this first stage; C07 owns coordinated
     * backup/merge/stepdown signalling. */
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_HEAD) {
        /* CLV2-M03 (03-03): classify the offer by Epoch relation, never
         * by raw Term.  FOREIGN truncates the comparison domain FIRST -
         * a foreign Cluster's Term is not authority (Cluster A term 2
         * must not surrender to Cluster B term 100); every same-cluster
         * relation (HIGHER / LOWER / SAME / CONFLICT) funnels into the
         * unified authority path.  CONFLICT is NOT a foreign merge. */
        ucn_cluster_epoch_t local_epoch =
            ucn_cluster_active_epoch_get(cluster);
        ucn_cluster_epoch_t remote_epoch;
        ucn_cluster_epoch_relation_t relation;

        remote_epoch.cluster_id = candidate->cluster_id;
        remote_epoch.term = candidate->term;
        remote_epoch.head_node_id = candidate->head_node_id;
        relation = ucn_cluster_epoch_compare(&local_epoch, &remote_epoch);
        if (relation == UCN_CLUSTER_EPOCH_RELATION_FOREIGN) {
            classify_foreign_cluster_merge(cluster, candidate, now_ms);
            return;
        }
        classify_same_cluster_authority(cluster, candidate, now_ms, relation);
        return;
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        /* A stable Head reclaims the domain from a temporary Recovery
         * Head; ordered stepdown switches back to the original Cluster. */
        begin_ordered_stepdown(cluster, candidate, now_ms);
        return;
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_DETACHED ||
        ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_CANDIDATE) {
        /* C07.7 P1: available_capacity == 0 gates new JOINs only; it must
         * never block epoch convergence between existing Heads (handled
         * above), so the capacity check lives here at the join point. */
        if (candidate->available_capacity == 0U) {
            return;
        }
        /* CLV2-01-04b.3 (DETACHED_OBSERVE/ELECTION) + CLV2-01-04f SITE A
         * (RECOVERY_*): a detached/election node accepting a stable Head
         * offer performs the -> JOIN_PENDING transition through the single
         * entry point BEFORE any phase-relevant legacy mutation (the role
         * write is owned by apply_legacy); the remaining begin_join()
         * field payload follows at the site via begin_join_prepare_fields().
         * The legacy stable-Head offer event decides THAT the join runs;
         * cluster_transition() validates whether the shadow agrees (a
         * caller NEVER uses the shadow to decide whether to SKIP the
         * call). */
        if (!cluster_phase_recovery_eligible(cluster->phase)) {
            ucn_cluster_phase_t old_phase =
                (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_CANDIDATE)
                    ? UCN_CLUSTER_PHASE_ELECTION
                    : UCN_CLUSTER_PHASE_DETACHED_OBSERVE;

            if (cluster_transition(cluster, old_phase,
                                   UCN_CLUSTER_PHASE_JOIN_PENDING,
                                   UCN_CLUSTER_REASON_JOIN_INITIATED,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition (shadow mismatch /
                 * illegal pair / pre-mutated phase fields) leaves every
                 * field untouched - do not apply the join payload. */
                return;
            }
            begin_join_prepare_fields(cluster, candidate, now_ms);
#if !defined(NDEBUG)
            assert(cluster->phase ==
                   UCN_CLUSTER_PHASE_JOIN_PENDING);
#endif
            return;
        }
        /* CLV2-01-04f SITE A: a RECOVERY_OBSERVE / RECOVERY_ELECTION node
         * (role DETACHED + recovery_eligible; the armed backoff decides the
         * sub-phase) accepting a stable-Head offer commits RECOVERY_* ->
         * JOIN_PENDING (JOIN_INITIATED) through the single entry point
         * BEFORE any site write; apply_legacy(JOIN_PENDING) writes role +
         * eligible=false + backoff=0, then the begin_join() field payload
         * follows at the site. */
        {
            ucn_cluster_phase_t old_phase =
                cluster->phase;

            if (cluster_transition(cluster, old_phase,
                                   UCN_CLUSTER_PHASE_JOIN_PENDING,
                                   UCN_CLUSTER_REASON_JOIN_INITIATED,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition (shadow mismatch /
                 * illegal pair / pre-mutated phase fields) leaves every
                 * field untouched - do not apply the join payload. */
                return;
            }
            begin_join_prepare_fields(cluster, candidate, now_ms);
#if !defined(NDEBUG)
            assert(cluster->phase ==
                   UCN_CLUSTER_PHASE_JOIN_PENDING);
#endif
            return;
        }
    }
    if (ucn_cluster_get_role(cluster) == UCN_CLUSTER_ROLE_JOIN_PENDING) {
        ucn_cluster_epoch_t pending_epoch;
        ucn_cluster_epoch_t remote_epoch;
        ucn_cluster_epoch_relation_t relation;

        /* CLV2-03-R10: JOIN_PENDING is still an Epoch decision.  Build the
         * pending target explicitly and use the one comparator so a foreign
         * Cluster is selected by policy before any Term is inspected. */
        pending_epoch.cluster_id = cluster->pending_cluster_id;
        pending_epoch.term = cluster->pending_term;
        pending_epoch.head_node_id = cluster->pending_head_node_id;
        remote_epoch.cluster_id = candidate->cluster_id;
        remote_epoch.term = candidate->term;
        remote_epoch.head_node_id = candidate->head_node_id;
        relation = ucn_cluster_epoch_compare(&pending_epoch, &remote_epoch);

        /* FOREIGN means a different Join target, independent of both Term
         * values.  Within one Cluster only a newer remote Epoch (LOWER from
         * compare(pending, remote)) or a same-Term Head conflict re-targets;
         * SAME and a stale remote HIGHER leave the in-flight Join untouched. */
        if (relation == UCN_CLUSTER_EPOCH_RELATION_FOREIGN ||
            relation == UCN_CLUSTER_EPOCH_RELATION_LOWER ||
            relation == UCN_CLUSTER_EPOCH_RELATION_CONFLICT) {
            begin_join(cluster, candidate, now_ms);
        }
        return;
    }
    if (ucn_cluster_get_role(cluster) != UCN_CLUSTER_ROLE_MEMBER) {
        return;
    }
    /* CLV2-M11 (11-08): a Member never chooses a foreign Head from score
     * samples.  Different Members can observe different RF conditions; an
     * autonomous LEAVE->JOIN here tears one Cluster apart.  The current Head
     * lease remains authoritative until a verified Stepdown/Takeover/lease
     * failure path owns the transition. */
    (void)now_ms;
    candidate->better_samples = 0U;
}

/* C07.2 Backup state machine helpers. */

ucn_result_t send_cluster_message(
    ucn_cluster_t *cluster,
    ucn_node_id_t destination,
    const ucn_cluster_message_t *message)
{
    uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_result_t result = ucn_cluster_message_encode(message, payload);

    if (result != UCN_OK) {
        return result;
    }
    return cluster_transmit(cluster, destination, message, payload);
}

/* Allocate a Backup mirror slot ignoring the product soft member_capacity;
 * only the compile-time physical table bound applies. */

/* C07.5 RECOVERY_HEAD: a short-lived emergency Head formed only after both
 * the Primary and Backup are lost.  Its provider-allocated Cluster ID is
 * fresh, so it never impersonates the lost Cluster. */


ucn_cluster_candidate_t *find_candidate(
    ucn_cluster_t *cluster,
    ucn_node_id_t head_node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_CANDIDATES; ++index) {
        if (cluster->candidates[index].occupied &&
            cluster->candidates[index].head_node_id == head_node_id) {
            return &cluster->candidates[index];
        }
    }
    return NULL;
}

ucn_cluster_candidate_t *allocate_candidate(
    ucn_cluster_t *cluster,
    ucn_node_id_t head_node_id,
    uint32_t now_ms)
{
    size_t index;
    ucn_cluster_candidate_t *candidate = find_candidate(cluster, head_node_id);

    if (candidate != NULL) {
        return candidate;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_CANDIDATES; ++index) {
        if (!cluster->candidates[index].occupied ||
            ucn_deadline_expired(now_ms,
                                 cluster->candidates[index].expires_at_ms)) {
            (void)memset(&cluster->candidates[index], 0,
                         sizeof(cluster->candidates[index]));
            cluster->candidates[index].occupied = true;
            cluster->candidates[index].head_node_id = head_node_id;
            return &cluster->candidates[index];
        }
    }
    return NULL;
}

ucn_result_t observe_candidate(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    const ucn_cluster_message_t *message,
    uint32_t now_ms)
{
    ucn_cluster_candidate_t *candidate =
        allocate_candidate(cluster, source, now_ms);

    if (candidate == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    if (candidate->last_nonce != 0U &&
        candidate->cluster_id == message->cluster_id &&
        candidate->term == message->term &&
        message->nonce <= candidate->last_nonce) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    candidate->head_node_id = source;
    candidate->cluster_id = message->cluster_id;
    candidate->term = message->term;
    candidate->head_score = message->head_score;
    candidate->available_capacity = message->available_capacity;
    candidate->expires_at_ms = ucn_deadline_from_now(now_ms, message->lease_ms);
    candidate->last_nonce = message->nonce;
    candidate->role = message->role;
    return UCN_OK;
}

bool score_improves_by(
    uint16_t candidate_score,
    uint16_t current_score,
    uint8_t percent)
{
    uint32_t required = (uint32_t)current_score * (100U + percent);

    return (uint32_t)candidate_score * 100U >= required;
}

void send_head_stepdown(ucn_cluster_t *cluster)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->primary_members.slots[index].occupied) {
            (void)send_message(cluster, cluster->primary_members.slots[index].node_id,
                               UCN_CLUSTER_MSG_HEAD_STEPDOWN,
                               UCN_CLUSTER_ROLE_HEAD, cluster->cluster_id,
                               cluster->term, cluster->config.local_node_id,
                               cluster->config.head_score, 0U);
        }
    }
}
