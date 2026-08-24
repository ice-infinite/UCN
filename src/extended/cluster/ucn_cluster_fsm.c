/* UCN CLV2-M02 (02-03): Cluster FSM module.
 *
 * STRUCTURAL REFACTOR ONLY (M02 mandate): the explicit-phase transition
 * framework - legacy derive, reason tables, shadow mirror, DIRECT /
 * OBSERVED matrices, cluster_transition()/validate/preflight/apply_legacy
 * and the test hooks - extracted verbatim from the former single
 * ucn_cluster.c.  Semantics, matrix contents, reason mapping and every
 * function body are UNCHANGED; M01 (human sign-off ab53b31/a7f4841)
 * froze them.  Do NOT "optimize" anything here.
 *
 * Exposed to the other Cluster modules via ucn_cluster_internal.h:
 *   cluster_phase_from_legacy_state / cluster_legacy_state_is_valid /
 *   cluster_transition / cluster_transition_preflight.
 */

#include "ucn/ucn_cluster.h"

#include <assert.h>
#include <string.h>

#include "ucn/ucn_time.h"

#include "ucn_cluster_internal.h"


uint32_t cluster_now(const ucn_cluster_t *cluster)
{
    return cluster->config.now_ms(cluster->config.now_context);
}

ucn_cluster_role_t ucn_cluster_phase_to_role(ucn_cluster_phase_t phase)
{
    switch (phase) {
    case UCN_CLUSTER_PHASE_DISABLED:
        return UCN_CLUSTER_ROLE_DISABLED;
    case UCN_CLUSTER_PHASE_DETACHED_OBSERVE:
    case UCN_CLUSTER_PHASE_RECOVERY_OBSERVE:
    case UCN_CLUSTER_PHASE_RECOVERY_ELECTION:
        return UCN_CLUSTER_ROLE_DETACHED;
    case UCN_CLUSTER_PHASE_ELECTION:
        return UCN_CLUSTER_ROLE_CANDIDATE;
    case UCN_CLUSTER_PHASE_JOIN_PENDING:
        return UCN_CLUSTER_ROLE_JOIN_PENDING;
    case UCN_CLUSTER_PHASE_MEMBER_ACTIVE:
    case UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE:
        return UCN_CLUSTER_ROLE_MEMBER;
    case UCN_CLUSTER_PHASE_HEAD_NO_BACKUP:
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING:
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
    case UCN_CLUSTER_PHASE_HEAD_STABLE:
    case UCN_CLUSTER_PHASE_HEAD_RECONFIGURING:
    case UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE:
    case UCN_CLUSTER_PHASE_HEAD_FENCED:
    case UCN_CLUSTER_PHASE_HEAD_REKEYING:
        return UCN_CLUSTER_ROLE_HEAD;
    case UCN_CLUSTER_PHASE_BACKUP_SYNCING:
    case UCN_CLUSTER_PHASE_BACKUP_READY:
    case UCN_CLUSTER_PHASE_BACKUP_TAKEOVER:
        return UCN_CLUSTER_ROLE_BACKUP;
    case UCN_CLUSTER_PHASE_STEPPING_DOWN:
        return UCN_CLUSTER_ROLE_STEPPING_DOWN;
    case UCN_CLUSTER_PHASE_RECOVERY_HEAD:
        return UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    case UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT:
        return UCN_CLUSTER_ROLE_TERM_CONFLICT;
    default:
        return UCN_CLUSTER_ROLE_DISABLED;
    }
}

bool cluster_phase_backup_ready(ucn_cluster_phase_t phase)
{
    return phase == UCN_CLUSTER_PHASE_HEAD_STABLE ||
           phase == UCN_CLUSTER_PHASE_BACKUP_READY;
}

bool cluster_phase_backup_syncing(ucn_cluster_phase_t phase)
{
    return phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING;
}

bool cluster_backup_assignment_pending(const ucn_cluster_t *cluster)
{
    /* The assignment sweep is bounded transaction work, not a second
     * lifecycle state.  A stable Head may periodically refresh the Backup
     * binding without revoking the already-proved snapshot.  The former
     * backup_assign_pending bool merely mirrored this remaining counter. */
    return cluster != NULL && cluster->backup_assign_remaining != 0U;
}

bool cluster_phase_backup_takeover_active(ucn_cluster_phase_t phase)
{
    return phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
}

bool cluster_phase_recovery_eligible(ucn_cluster_phase_t phase)
{
    return phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE ||
           phase == UCN_CLUSTER_PHASE_RECOVERY_ELECTION ||
           phase == UCN_CLUSTER_PHASE_RECOVERY_HEAD;
}

/* CLV2-M14 (14-01): phase is the sole lifecycle state.  The former
 * role+bool -> phase reverse mapper and end-of-Step/RX shadow repair were
 * intentionally deleted.  A lifecycle change that does not pass through
 * cluster_transition() can therefore no longer be hidden by a later sync. */

/* BEST-EFFORT ONLY reason inference for a legacy transition, from the
 * phase pair alone.  NOT AUTHORITATIVE: the same old/new pair can be
 * reached by different events, so this table can mislabel individual
 * cases (e.g. a member detached by HEAD_STEPDOWN vs. by lease expiry).
 * It MUST NOT drive the FSM; the single transition entry point
 * (CLV2-01-04) will replace it with explicit per-event reasons.  The
 * shadow tests only require 'phase changed => reason != UNKNOWN'. */
static ucn_cluster_transition_reason_t cluster_reason_from_diff(
    ucn_cluster_phase_t old_phase, ucn_cluster_phase_t new_phase)
{
    if (new_phase == UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT) {
        return UCN_CLUSTER_REASON_TERM_CONFLICT;
    }
    if (old_phase == UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT &&
        new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
        return UCN_CLUSTER_REASON_HIGHER_AUTHORITY;
    }
    switch (old_phase) {
    case UCN_CLUSTER_PHASE_JOIN_PENDING:
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            return UCN_CLUSTER_REASON_JOIN_ACCEPTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE) {
            return UCN_CLUSTER_REASON_JOIN_REJECTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING) {
            /* BACKUP_ASSIGN(self) can arrive before JOIN_ACCEPT. */
            return UCN_CLUSTER_REASON_BACKUP_ASSIGNED;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_DETACHED_OBSERVE:
        if (new_phase == UCN_CLUSTER_PHASE_ELECTION) {
            return UCN_CLUSTER_REASON_ELECTION_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            /* Joined a recovery Head (handle_recovery_declare). */
            return UCN_CLUSTER_REASON_RECOVERY_WIN;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_ELECTION:
        /* Winning an election lands on HEAD_*; which backup sub-phase
         * depends on whether a backup assignment survived the election
         * (restart/recovery paths can keep one). */
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP ||
            new_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING ||
            new_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING ||
            new_phase == UCN_CLUSTER_PHASE_HEAD_STABLE) {
            return UCN_CLUSTER_REASON_ELECTION_WON;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE) {
            return UCN_CLUSTER_REASON_ELECTION_LOST;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_MEMBER_ACTIVE:
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE) {
            return UCN_CLUSTER_REASON_HEAD_LEASE_EXPIRED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING) {
            return UCN_CLUSTER_REASON_BACKUP_ASSIGNED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE) {
            return UCN_CLUSTER_REASON_RESET;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE:
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            return UCN_CLUSTER_REASON_HEAD_LEASE_RENEWED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE ||
            new_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) {
            return UCN_CLUSTER_REASON_GRACE_TIMEOUT;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING) {
            return UCN_CLUSTER_REASON_BACKUP_ASSIGNED;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_HEAD_NO_BACKUP:
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING:
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING) {
            return UCN_CLUSTER_REASON_BACKUP_ASSIGNED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING) {
            return UCN_CLUSTER_REASON_BACKUP_SYNC_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_STABLE) {
            return UCN_CLUSTER_REASON_SNAPSHOT_READY;
        }
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) {
            return UCN_CLUSTER_REASON_BACKUP_LOST;
        }
        if (new_phase == UCN_CLUSTER_PHASE_STEPPING_DOWN) {
            return UCN_CLUSTER_REASON_STEPDOWN_ORDERED;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_HEAD_STABLE:
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) {
            return UCN_CLUSTER_REASON_BACKUP_LOST;
        }
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING ||
            new_phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING) {
            return UCN_CLUSTER_REASON_RESYNC_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_STEPPING_DOWN) {
            return UCN_CLUSTER_REASON_STEPDOWN_ORDERED;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_BACKUP_SYNCING:
        if (new_phase == UCN_CLUSTER_PHASE_BACKUP_READY) {
            return UCN_CLUSTER_REASON_SNAPSHOT_READY;
        }
        if (new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE ||
            new_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) {
            return UCN_CLUSTER_REASON_PRIMARY_LOST;
        }
        if (new_phase == UCN_CLUSTER_PHASE_ELECTION) {
            return UCN_CLUSTER_REASON_ELECTION_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            /* handle_head_takeover() / handle_recovery_declare(). */
            return UCN_CLUSTER_REASON_TAKEOVER_STARTED;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_BACKUP_READY:
        if (new_phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER) {
            return UCN_CLUSTER_REASON_TAKEOVER_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING) {
            return UCN_CLUSTER_REASON_RESYNC_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_ELECTION) {
            return UCN_CLUSTER_REASON_ELECTION_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE) {
            return UCN_CLUSTER_REASON_PRIMARY_LOST;
        }
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            return UCN_CLUSTER_REASON_TAKEOVER_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) {
            /* Observed tick compound: takeover started+completed in one
             * tick (golden trace t=179). */
            return UCN_CLUSTER_REASON_TAKEOVER_QUORUM;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_BACKUP_TAKEOVER:
        /* complete_takeover() always lands on HEAD_NO_BACKUP (it clears
         * backup_node_id/ready), never on a populated Head sub-phase. */
        if (new_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) {
            return UCN_CLUSTER_REASON_TAKEOVER_QUORUM;
        }
        /* A takeover-active Backup always has recovery_eligible == false,
         * so the timeout path lands on DETACHED_OBSERVE only. */
        if (new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE) {
            return UCN_CLUSTER_REASON_TAKEOVER_TIMEOUT;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            return UCN_CLUSTER_REASON_TAKEOVER_STARTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_ELECTION) {
            return UCN_CLUSTER_REASON_ELECTION_STARTED;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_STEPPING_DOWN:
        /* The stepdown deadline always moves to JOIN_PENDING first; the
         * DETACHED_OBSERVE / MEMBER_ACTIVE destinations are OBSERVED tick
         * compounds (deadline + JOIN_REJECT / JOIN_ACCEPT in one tick). */
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING ||
            new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE ||
            new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            return UCN_CLUSTER_REASON_STEPDOWN_COMPLETE;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_RECOVERY_OBSERVE:
        if (new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE) {
            return UCN_CLUSTER_REASON_RECOVERY_SERIAL_EXHAUSTED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_RECOVERY_ELECTION) {
            return UCN_CLUSTER_REASON_RECOVERY_BACKOFF;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            return UCN_CLUSTER_REASON_RECOVERY_WIN;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_RECOVERY_ELECTION:
        if (new_phase == UCN_CLUSTER_PHASE_RECOVERY_HEAD) {
            return UCN_CLUSTER_REASON_RECOVERY_WIN;
        }
        if (new_phase == UCN_CLUSTER_PHASE_JOIN_PENDING) {
            return UCN_CLUSTER_REASON_JOIN_INITIATED;
        }
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            return UCN_CLUSTER_REASON_RECOVERY_WIN;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_RECOVERY_HEAD:
        /* stepdown_recovery_head() keeps recovery_eligible == true, so the
         * TTL expiry lands on RECOVERY_OBSERVE, never DETACHED_OBSERVE. */
        if (new_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) {
            return UCN_CLUSTER_REASON_RECOVERY_TTL_EXPIRED;
        }
        /* CLV2-M01.0.1: lost the Recovery arbitration and joined the
         * winner's Cluster.  RECOVERY_WIN is reserved for the node that
         * actually won. */
        if (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            return UCN_CLUSTER_REASON_RECOVERY_YIELDED;
        }
        /* A stable Head reclaiming the domain (begin_ordered_stepdown). */
        if (new_phase == UCN_CLUSTER_PHASE_STEPPING_DOWN) {
            return UCN_CLUSTER_REASON_STEPDOWN_ORDERED;
        }
        return UCN_CLUSTER_REASON_UNKNOWN;
    case UCN_CLUSTER_PHASE_DISABLED:
        return UCN_CLUSTER_REASON_INIT;
    default:
        return UCN_CLUSTER_REASON_UNKNOWN;
    }
}

/* RX hint: the most typical reason a message of this type would change
 * the phase.  It is only consulted when the diff table above has no
 * entry, so a wrong hint can never overwrite an exact match. */
ucn_cluster_transition_reason_t cluster_rx_reason_from_type(
    ucn_cluster_message_type_t type)
{
    switch (type) {
    case UCN_CLUSTER_MSG_JOIN_ACCEPT:
        return UCN_CLUSTER_REASON_JOIN_ACCEPTED;
    case UCN_CLUSTER_MSG_JOIN_REJECT:
        return UCN_CLUSTER_REASON_JOIN_REJECTED;
    case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
        return UCN_CLUSTER_REASON_BACKUP_ASSIGNED;
    case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
        return UCN_CLUSTER_REASON_SNAPSHOT_READY;
    case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
        return UCN_CLUSTER_REASON_TAKEOVER_STARTED;
    case UCN_CLUSTER_MSG_TAKEOVER_ACK:
        return UCN_CLUSTER_REASON_TAKEOVER_QUORUM;
    case UCN_CLUSTER_MSG_HEAD_STEPDOWN:
        return UCN_CLUSTER_REASON_STEPDOWN_ORDERED;
    case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
        return UCN_CLUSTER_REASON_PRIMARY_RENEWED;
    case UCN_CLUSTER_MSG_KEEPALIVE:
        return UCN_CLUSTER_REASON_HEAD_LEASE_RENEWED;
    case UCN_CLUSTER_MSG_RECOVERY_DECLARE:
        return UCN_CLUSTER_REASON_RECOVERY_WIN;
    case UCN_CLUSTER_MSG_LEAVE:
        return UCN_CLUSTER_REASON_LEAVE;
    default:
        return UCN_CLUSTER_REASON_UNKNOWN;
    }
}

/* ================= CLV2-01-04a: single transition entry point ===========
 *
 * M01 proved the 17-phase mapping is total and consistent.  This stage
 * adds the ONE entry point that later stages (CLV2-01-04b..f) will wire
 * into the legacy transition sites so every state change flows through a
 * single validated transition.  It is deliberately NOT called by any
 * production site yet: it is built, unit-tested in isolation, and must
 * not change current behaviour in any way.
 *
 * CLV2-01-04a.1 splits legality into TWO tables (human audit hold):
 * CLUSTER_TRANSITION_DIRECT_ALLOWED (edges a SINGLE site performs as one
 * cluster_transition() call - the only table the entry point consults)
 * and CLUSTER_TRANSITION_OBSERVED_ALLOWED (DIRECT union the tick-
 * granularity compound pairs the T-A collector observes - gate-only,
 * never callable).  BACKUP_TAKEOVER stays legal even while
 * takeover_active && backup_syncing holds (CLV2-M01.0.2); the entry
 * point must never clear backup_syncing or otherwise 'fix' that
 * reachable combination (deferred to M09/M10). */

#if defined(__GNUC__) || defined(__clang__)
#define CLV2_01_04_UNUSED __attribute__((unused))
#else
#define CLV2_01_04_UNUSED
#endif

#define CLUSTER_TERM_CONFLICT_EDGE \
    (UINT32_C(1) << UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT)

/* =====================================================================
 * CLV2-01-04a.1 Framework Closure: TWO legality tables.
 *
 * CLUSTER_TRANSITION_DIRECT_ALLOWED  - every edge a SINGLE production
 *   transition site can perform as ONE cluster_transition() call.  This
 *   is the ONLY table cluster_transition() consults: the entry point
 *   rejects anything not in it.  Each edge cites the site (function +
 *   line) that performs the role/phase switch.  Edges whose mapping
 *   fields are caller-provided (e.g. ELECTION -> HEAD_BACKUP_* relies on
 *   the caller's backup_* state, exactly as complete_election() leaves
 *   it) are still DIRECT: the site performs the transition, the caller
 *   state decides the destination sub-phase.
 *
 *   NOTE: the Lxxxx citations are best-effort and drift with refactors;
 *   the function names are authoritative.
 *
 * CLUSTER_TRANSITION_OBSERVED_ALLOWED - DIRECT union the tick-granularity
 *   COMPOUND pairs the T-A collector legitimately observes (one tick can
 *   span several single transitions, e.g. start_takeover + complete_
 *   takeover).  It is used ONLY by the observed-pairs gate
 *   (observed SUBSET-OF OBSERVED_ALLOWED); it is NOT callable.
 *
 * WIRING DISCIPLINE: 01-04b..f must never call cluster_transition() for
 * a compound pair - the compounds are realized by their constituent
 * DIRECT edges in sequence.  A bitmask keeps each table a small fixed
 * rodata table (17 * 4 bytes) on MCU targets; no self-loops.
 *
 * Deliberately EXCLUDED pairs (never allowed, review A/B):
 *   HEAD_NO_BACKUP / HEAD_BACKUP_ASSIGNING / HEAD_BACKUP_SYNCING /
 *   HEAD_STABLE -> ELECTION   : role CANDIDATE is written by
 *       start_election (reached only from DETACHED with
 *       recovery_eligible == false); no HEAD path.
 *   HEAD_NO_BACKUP / HEAD_BACKUP_ASSIGNING / HEAD_BACKUP_SYNCING /
 *   HEAD_STABLE -> DETACHED_OBSERVE : set_detached() is never called
 *       from a HEAD-role site.
 *   RECOVERY_OBSERVE -> ELECTION : a recovery-eligible node never elects.
 *   RECOVERY_ELECTION -> DETACHED_OBSERVE: serial exhaustion is checked
 *       before RECOVERY_OBSERVE enters ELECTION; no committed Election can
 *       later clear eligibility in place.
 *   RECOVERY_ELECTION -> RECOVERY_OBSERVE : no site clears the armed
 *       backoff while staying DETACHED+eligible.
 *   STEPPING_DOWN -> ELECTION : the only stepdown exit is the deadline.
 *   RECOVERY_HEAD -> DETACHED_OBSERVE : TTL expiry derives RECOVERY_OBSERVE.
 *   RECOVERY_HEAD -> JOIN_PENDING : exits are STEPPING_DOWN/MEMBER/
 *       RECOVERY_OBSERVE only (never observed).
 *   DETACHED_OBSERVE -> RECOVERY_OBSERVE : recovery_eligible is only set
 *       in the same statement that writes role=DETACHED (never observed).
 *   DISABLED <-> DETACHED_OBSERVE (both directions) : init-only.
 *   BACKUP_TAKEOVER -> HEAD_STABLE, -> HEAD_BACKUP_ASSIGNING :
 *       complete_takeover() always clears backup_node_id/ready.
 *   BACKUP_TAKEOVER -> RECOVERY_OBSERVE, BACKUP_READY ->
 *   RECOVERY_OBSERVE  : a BACKUP-role node always has recovery_eligible
 *       == false.
 *   HEAD_NO_BACKUP -> HEAD_BACKUP_SYNCING / -> HEAD_STABLE : no single
 *       site performs them (assign_backup always enters ASSIGNING first;
 *       a READY requires an already-selected backup); never observed.
 * These exclusions are mirrored in the tests and pinned by
 * cluster_test_transition_matrix(). */
static const uint32_t CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_COUNT]
    CLV2_01_04_UNUSED = {
    [UCN_CLUSTER_PHASE_DISABLED] =
        /* init-only phase: no runtime transition ever leaves it. */
        0U,

    [UCN_CLUSTER_PHASE_DETACHED_OBSERVE] =

        /* observe timeout (head_capable): start_election() L4055 (transition L4062) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_ELECTION) |
        /* stable Head offer: cluster_transition() via consider_head_offer() DETACHED (!recovery_eligible, 01-04b.3) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* joins a recovery Head: handle_recovery_declare() L3637 (role=MEMBER L4062) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_ELECTION] =

        /* CLV2-M03 03-05: any active local epoch that sees a same-Term
         * conflicting Head must stop its election/authority actions. */
        CLUSTER_TERM_CONFLICT_EDGE |

        /* win: complete_election() L4094 (win dispatch L4142); the HEAD

         * sub-phase is dispatched from the pre-call backup_* state. */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING) |
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING) |
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_STABLE) |
        /* stable Head offer: cluster_transition() L2580 via consider_head_offer() L2435 CANDIDATE (!recovery_eligible) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |

        /* loss: complete_election() L4094 -> set_detached() L2006 (role=DETACHED L2011) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE),

    [UCN_CLUSTER_PHASE_JOIN_PENDING] =
        CLUSTER_TERM_CONFLICT_EDGE |
        /* exact JOIN_ACCEPT: handle_join_accept() L2239 (transition L2273);
         * apply_legacy writes role + grace=0 (CLV2-01-04b.4) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |

        /* exact JOIN_REJECT (receive_inner L3766, transition L3855) /
         * HEAD_STEPDOWN -> set_detached() L2006 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* BACKUP_ASSIGN(self) arrives first: handle_backup_assign() L2851 (transition L2909, role=BACKUP L2919) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_SYNCING),

    [UCN_CLUSTER_PHASE_MEMBER_ACTIVE] =

        CLUSTER_TERM_CONFLICT_EDGE |

        /* Head lease expired: ucn_cluster_step_inner() L4899 (grace armed L4935) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE) |
        /* BACKUP_ASSIGN(self): handle_backup_assign() L2851 (transition L2909) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_SYNCING) |
        /* stepdown / reset: HEAD_STEPDOWN (receive_inner L3902,
         * transition L3971, CLV2-01-04c.5) -> set_detached() L2006 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* better Head switch: cluster_transition() L2646 via consider_head_offer() L2435 MEMBER (!grace) + begin_join_prepare_fields() L2111 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING),

    [UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE] =
        CLUSTER_TERM_CONFLICT_EDGE |
        /* Head lease renewed: cluster_transition() L2458 via consider_head_offer() L2435
         * refresh (grace=0 site write) / handle_head_takeover() L3504 (grace=0 L3578) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |
        /* HEAD_STEPDOWN (receive_inner L3902, transition L3971,
         * CLV2-01-04c.5) -> set_detached() L2006 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |

        /* grace timeout: step L4945 (transition L4957) + set_detached() L2006 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) |
        /* better Head switch: cluster_transition() L2635 via consider_head_offer() L2435 GRACE + begin_join_prepare_fields() L2111 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* BACKUP_ASSIGN(self): handle_backup_assign() L2851 (transition L2909) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_SYNCING),

    [UCN_CLUSTER_PHASE_HEAD_NO_BACKUP] =

        CLUSTER_TERM_CONFLICT_EDGE |

        /* Backup selected: assign_backup() L2768 (transition 01-04d.1
         * before node_id L2829; apply_legacy arms assign_pending) +
         * start_backup_assignment_cycle() L4241 (idempotent pending) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING) |
        /* ordered stepdown: begin_ordered_stepdown() L2356 (role=STEPPING_DOWN L2390) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING] =

        CLUSTER_TERM_CONFLICT_EDGE |

        /* assignment sweep done: send_backup_assignment_step() L4369 (transition 01-04d.2 before pending=false) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING) |
        /* READY during sweep: handle_backup_ready() L2970 (transition L3005, ready=true L3014) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_STABLE) |
        /* Backup lost: remove_member() L2111 (node_id=0 L2153) / expire_members() L4783 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |
        /* ordered stepdown: begin_ordered_stepdown() L2356 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING] =
        CLUSTER_TERM_CONFLICT_EDGE |
        /* snapshot READY: handle_backup_ready() L2970 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_STABLE) |

        /* periodic re-assign: start_backup_assignment_cycle() L4241 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING) |
        /* Backup lost: remove_member() L2111 / expire_members() L4783 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |
        /* ordered stepdown: begin_ordered_stepdown() L2356 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_HEAD_STABLE] =
        CLUSTER_TERM_CONFLICT_EDGE |
        /* Backup lost: remove_member() L2111 / expire_members() L4783 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |

        /* resync with an armed sweep: backup_resync() L4673 target dispatch
         * (CLV2-01-04d.7 MAJOR 2 - STABLE->ASSIGNING is now a REAL direct
         * transition, promoted from the observed-compound list) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING) |
        /* resync: backup_resync() (transition, ready=false); line numbers
         * best-effort per drift policy */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING) |
        /* ordered stepdown: begin_ordered_stepdown() L2356 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_BACKUP_SYNCING] =
        CLUSTER_TERM_CONFLICT_EDGE |
        /* snapshot READY: handle_backup_member_sync() SYNC_END (CLV2-01-04e.2
         * transition before syncing=false/ready=true; line numbers best-effort) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_READY) |
        /* Primary lost / stepdown: HEAD_STEPDOWN -> backup_clear_sync() L2736 -> set_detached() L2006 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |

        /* Primary lost (eligible): step L5028 (eligible=true L5030) + backup_clear_sync() L2736 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) |
        /* Reserved legacy relation; M11 disables v3 score challenge. */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_ELECTION) |
        /* newer-Term Head: process_higher_authority() + backup_clear_sync()
         * + begin_join() (CLV2-M03 03-04 RX pre-dispatch). */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* HEAD_TAKEOVER / recovery: handle_head_takeover() L3504 (role=MEMBER L3571) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_BACKUP_READY] =
        CLUSTER_TERM_CONFLICT_EDGE |
        /* Primary lease lapsed: start_takeover() L3293 (takeover=true L3315) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_TAKEOVER) |
        /* DELTA gap / resync: handle_backup_member_sync() DELTA-gap /
         * fresh-SYNC_BEGIN / snapshot-seq-gap paths re-enter SYNCING via
         * the explicit READY->SYNCING transition (CLV2-01-04e.7,
         * RESYNC_STARTED) - the pair was DIRECT all along and is now
         * actually committed at the site */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_SYNCING) |
        /* Reserved legacy relation; M11 disables v3 score challenge. */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_ELECTION) |
        /* newer-Term Head: process_higher_authority() (CLV2-M03 03-04). */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* Primary lost / stepdown: HEAD_STEPDOWN -> backup_clear_sync() L2736 -> set_detached() L2006 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* HEAD_TAKEOVER / recovery: handle_head_takeover() L3504 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_BACKUP_TAKEOVER] =
        CLUSTER_TERM_CONFLICT_EDGE |
        /* majority reached: complete_takeover() L3226 (role=HEAD L3247, node_id=0 L3257) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |

        /* timeout / stepdown: step L5156 -> backup_clear_sync() L2738 -> set_detached() L2008 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* newer-Term Head: process_higher_authority() (CLV2-M03 03-04). */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* HEAD_TAKEOVER / recovery: handle_head_takeover() L3504 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |
        /* Reserved legacy relation; M11 disables v3 score challenge. */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_ELECTION),

    [UCN_CLUSTER_PHASE_STEPPING_DOWN] =

        CLUSTER_TERM_CONFLICT_EDGE |

        /* stepdown deadline: ucn_cluster_step_inner() L4899 (role=JOIN_PENDING L5103) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING),

    [UCN_CLUSTER_PHASE_RECOVERY_OBSERVE] =
        /* M13 replay-domain exhaustion: fail closed until a fresh identity
         * domain can be persisted/created. */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* backoff armed: start_recovery_backoff() L3591 (deadline L3594) via step L5060 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_ELECTION) |
        /* Head offer: cluster_transition() via consider_head_offer() RECOVERY_* (01-04f) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* recovery Head join: handle_recovery_declare() L3637 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_RECOVERY_ELECTION] =
        /* quorum, declare: declare_recovery_head() L3598 (role=RECOVERY_HEAD L3603) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_HEAD) |
        /* no-quorum backoff re-arm stays RECOVERY_ELECTION (self-write,
         * phase-preserving, 01-04f.1) - no direct edge needed */
        /* Head offer: cluster_transition() via consider_head_offer() RECOVERY_* (01-04f) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* recovery Head join: handle_recovery_declare() L3637 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_RECOVERY_HEAD] =
        CLUSTER_TERM_CONFLICT_EDGE |
        /* TTL expired (cooldown): stepdown_recovery_head() L3621 -> set_detached() L2006 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) |
        /* lost arbitration / HEAD_TAKEOVER: handle_recovery_declare() L3637 /
         * handle_head_takeover() L3455 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |
        /* stable Head reclaims: begin_ordered_stepdown() L2356 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT] =
        /* Only a proven higher-Term normal Head offer can re-open a join.
         * M08 adds immediate Authority fencing, but does not replace this
         * control-plane safe wait.  The two layers remain complementary. */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING),
};

/* Bounds-checked lookup into the DIRECT legality table (single-site
 * edges only; CLV2-01-04a.1 Item 1). */
static bool cluster_transition_is_allowed(ucn_cluster_phase_t old_phase,
                                          ucn_cluster_phase_t new_phase)
    CLV2_01_04_UNUSED;

static bool cluster_transition_is_allowed(ucn_cluster_phase_t old_phase,
                                          ucn_cluster_phase_t new_phase)
{
    if ((unsigned int)old_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT ||
        (unsigned int)new_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT) {
        return false;
    }
    return (CLUSTER_TRANSITION_DIRECT_ALLOWED[old_phase] &
            (UINT32_C(1) << (unsigned int)new_phase)) != 0U;
}

/* Make the phase-relevant legacy fields consistent with the new phase.
 *
 * CLV2-01-04a.1 (Item 2, human audit): apply_legacy() writes ONLY the
 * public role field (the phase's legacy projection) plus fields whose
 * target value is ABSOLUTELY IDENTICAL for every inbound edge of that
 * phase (minimal common invariant; the site citation is per case).  All
 * destination-based backup-mirror / known-backup cleanup is REMOVED:
 * handle_join_accept() does NOT clear known_backup_* (a JOIN_PENDING node
 * that saw BACKUP_ASSIGN for another node KEEPS that knowledge through
 * MEMBER_ACTIVE), begin_join() does NOT clear the mirror (only the
 * BACKUP-specific same-cluster-higher-term path runs backup_clear_sync()
 * BEFORE begin_join, at the site).  M11 retires the old score-challenge
 * site, so no default-product path clears Backup state merely for score.
 * The remaining special exits stay at the SITES during 01-04b..f wiring. Only
 * after every inbound site of a phase is migrated may a common cleanup be
 * re-merged into a single entry action here.  CLV2-01-04e.7 (human NIT):
 * the former "fields a site writes after the call are fine: the
 * end-of-step/RX shadow sync re-aligns" is DELETED - a migrated phase
 * change must NEVER depend on shadow_sync() minting (the validate-side
 * comment states the precise principle: migrated sites may perform
 * caller-owned post-transition writes only when they preserve the
 * committed new phase).
 *
 * Entering a HEAD_BACKUP_* / HEAD_STABLE sub-phase requires the caller to
 * have the matching backup_* state (assign_pending / ready / node_id), as
 * every real site does; RECOVERY_ELECTION requires a caller-provided
 * armed backoff deadline (CLV2-01-04a.1 Item 4 - never auto-minted).
 * CLV2-01-04d.1: HEAD_BACKUP_ASSIGNING is the one sub-phase whose
 * phase-defining invariant (assign_pending == true) IS provably common to
 * every inbound edge (assign_backup / complete_election caller state /
 * periodic re-assign), so apply_legacy arms it there - see the case. */
static void cluster_transition_apply_phase(ucn_cluster_t *cluster,
                                           ucn_cluster_phase_t new_phase,
                                           uint32_t now_ms)
    CLV2_01_04_UNUSED;

static void cluster_transition_apply_phase(ucn_cluster_t *cluster,
                                           ucn_cluster_phase_t new_phase,
                                           uint32_t now_ms)
{
    switch (new_phase) {
    case UCN_CLUSTER_PHASE_DISABLED:
        /* init-only; no runtime site toggles enabled. */
        break;
    case UCN_CLUSTER_PHASE_DETACHED_OBSERVE:
        /* Every inbound edge goes through set_detached() L2006
         * (role=DETACHED L2011, known_backup cleared L2020, grace L2028);
         * recovery_eligible is false on every inbound edge (the only
         * eligible=true writers produce RECOVERY_OBSERVE) and no inbound
         * edge arms backoff. */
        cluster->recovery_backoff_deadline_ms = 0U;
        cluster->head_grace_deadline_ms = 0U;
        cluster->known_backup_node_id = 0U;
        cluster->known_backup_generation = 0U;
        break;
    case UCN_CLUSTER_PHASE_ELECTION:

        /* role only: start_election() writes role=CANDIDATE; its mirror
         * clears are site-owned and
         * must NOT be replayed here (members[]/backup_generation survive
         * a challenge, exactly as the real site leaves them). */
        break;
    case UCN_CLUSTER_PHASE_JOIN_PENDING:
        /* CLV2-01-04b.3 + 01-04c.4 + 01-04f: DETACHED/ELECTION
         * (!recovery_eligible), MEMBER/GRACE and RECOVERY_* sources
         * transition via cluster_transition() at consider_head_offer()
         * (apply_legacy writes role + eligible=false + backoff=0); the
         * BACKUP newer-Term and JOIN_PENDING re-target sources keep
         * begin_join() L2059 (role L2067; candidacy abandon lives in the
         * shared field helper begin_join_prepare_fields() L2048).
         * begin_join() does NOT clear the mirror/known_backup (only the
         * BACKUP higher-Term path does backup_clear_sync() BEFORE the
         * join, at the site). */
        cluster->recovery_backoff_deadline_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_MEMBER_ACTIVE:
        /* handle_join_accept() L2239 (transition L2273, grace=0 via
         * apply_legacy) and handle_head_takeover() L3504 (role L3571,
         * grace=0 L3578) both write role+grace; recovery_eligible is
         * false on every inbound edge.  known_backup_* are NOT cleared
         * by handle_join_accept() (retained-state Test A). */
        cluster->head_grace_deadline_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE:

        /* Sole inbound edge: ucn_cluster_step_inner() L4899 arms the
         * grace deadline L4935. */

        if (cluster->head_grace_deadline_ms == 0U) {
            cluster->head_grace_deadline_ms = ucn_deadline_from_now(
                now_ms, cluster->config.keepalive_interval_ms);
        }
        break;
    case UCN_CLUSTER_PHASE_HEAD_NO_BACKUP:
        /* Entering HEAD_NO_BACKUP canonicalizes node_id=0 / ready=false
         * as a DESTINATION invariant: HEAD_NO_BACKUP is DEFINED by
         * backup_node_id == 0, so the hook normalizes it regardless of
         * which inbound site ran (remove_member() L2111 / expire_members()
         * L4783 / complete_takeover() L3226 / handle_backup_reject()).
         * Note this is NOT true of every inbound edge's own writes: the

         * ELECTION inbound (complete_election() L4094) LEAVES the

         * candidate's preserved backup_* state, which decides the actual
         * destination sub-phase - the 01-04b complete_election wiring must
         * dispatch on that caller state (NO_BACKUP only when node_id==0,
         * otherwise ASSIGNING/SYNCING/STABLE).  syncing/takeover/primary/
         * known_backup are NOT cleared here - site-owned. */
        cluster->backup_node_id = 0U;
        break;
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING:
        /* CLV2-01-04d.1: role + the phase-defining invariant.  ASSIGNING is
         * DEFINED by assign_pending == true && backup_node_id != 0, and the
         * only d-group site that transitions INTO it is assign_backup() L2768
         * (NO_BACKUP -> ASSIGNING); the other two direct inbound edges -
         * complete_election() L4094 caller-state dispatch and the periodic
         * re-assign start_backup_assignment_cycle() L4241 - already carry
         * node_id != 0 and arm assign_pending when they choose ASSIGNING, so
         * arming it here is the provably-common destination invariant on
         * EVERY inbound edge.  The d.1 transition runs BEFORE the node_id
         * write, so without this write the derive would stay NO_BACKUP (then
         * SYNCING after the site node_id write) instead of ASSIGNING and the
         * end-of-step sync would mint a bogus ASSIGNING->SYNCING pair.  The
         * site's own assign_pending=true in start_backup_assignment_cycle()
         * stays (idempotent same value). */
        break;
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
        /* role only: caller-provided node_id/assign_pending/ready state

         * decides the sub-phase (complete_election / backup_resync /
         * assignment sweep; line numbers best-effort per drift policy). */

        break;
    case UCN_CLUSTER_PHASE_HEAD_STABLE:
        /* role only: the caller provides ready=true (handle_backup_ready
         * L3014 / complete_election caller state). */
        break;
    case UCN_CLUSTER_PHASE_BACKUP_SYNCING:
        /* handle_backup_assign() (e.1 transition, role=BACKUP, syncing=true,
         * ready=false) and handle_backup_member_sync() re-entry (e.7
         * EXPLICIT READY->SYNCING transition for fresh SYNC_BEGIN / DELTA
         * gap / snapshot seq gap; the SYNCING/TAKEOVER pre-states run no
         * transition - self / M01.0.2 takeover precedence) both write
         * role+syncing+ready; takeover is never set on an inbound edge of
         * this phase. */
        break;
    case UCN_CLUSTER_PHASE_BACKUP_READY:
        /* handle_backup_member_sync() SYNC_END (e.2 transition, then
         * syncing=false/ready=true site writes). */
        break;
    case UCN_CLUSTER_PHASE_BACKUP_TAKEOVER:
        /* start_takeover() L3293: role=BACKUP, takeover=true L3315;
         * ready/syncing are NOT cleared (CLV2-M01.0.2: the takeover_active
         * && syncing combo is reachable and must be expressed). */
        break;
    case UCN_CLUSTER_PHASE_STEPPING_DOWN:
        /* begin_ordered_stepdown() L2356: role=STEPPING_DOWN (via
         * apply_legacy on both the HEAD_* and RECOVERY_HEAD sources,
         * 01-04d.6/01-04f; the site's own role write stays, idempotent),
         * yields Recovery candidacy L2387-2317; the Head keeps its Backup
         * selection (node_id/ready) until the deadline. */
        cluster->recovery_backoff_deadline_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_RECOVERY_OBSERVE:

        /* Every inbound edge (grace timeout L4945 + set_detached() L2006;
         * backup missed-heartbeat L5028 + backup_clear_sync() L2736;
         * RECOVERY_HEAD TTL stepdown_recovery_head L3621 -> set_detached()

         * L2006) results in role=DETACHED, eligible=true, backoff=0, and
         * set_detached() clears grace + known_backup. */
        cluster->recovery_backoff_deadline_ms = 0U;
        cluster->head_grace_deadline_ms = 0U;
        cluster->known_backup_node_id = 0U;
        cluster->known_backup_generation = 0U;
        break;
    case UCN_CLUSTER_PHASE_RECOVERY_ELECTION:
        /* role + eligibility only (CLV2-01-04a.1 Item 4): the armed
         * backoff deadline is CALLER-PROVIDED - the 01-04f recovery site
         * supplies the Current-computed deadline/nonce; apply_legacy never
         * mints one, and no inbound edge writes the cooldown here. */
        break;
    case UCN_CLUSTER_PHASE_RECOVERY_HEAD:
        break;
    case UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT:
        /* Preserve the conflicting epoch for comparison and diagnostics, but
         * revoke all role-driven control activity.  This role has no v3 wire
         * representation and cannot be reactivated by a same-Term message. */
        cluster->role_since_ms = now_ms;
        cluster->next_advertise_ms = 0U;
        cluster->next_keepalive_ms = 0U;
        cluster->next_join_retry_ms = 0U;
        cluster->election_deadline_ms = 0U;
        cluster->stepdown_deadline_ms = 0U;
        cluster->head_grace_deadline_ms = 0U;
        cluster->backup_takeover_announce_active = false;
        cluster->backup_takeover_announce_remaining = 0U;
        cluster->recovery_backoff_deadline_ms = 0U;
        break;
    default:
        break;
    }
}

#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS)
/* Debug assert gate for the fail-closed rejection paths.  Rejection tests
 * toggle this off so they can verify the release behaviour (UCN_ERR_STATE)
 * without aborting; production builds never compile the knob. */
static bool cluster_transition_assert_enabled = true;
#endif

/* Fail-closed assert idiom (CLV2-01-04a review A, F3): debug builds abort
 * on an illegal transition; non-debug (NDEBUG) builds skip the assert and
 * return UCN_ERR_STATE without aborting.  Under the test hooks the knob
 * can additionally silence the assert so rejection tests can verify the
 * release path. */
#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS) && !defined(NDEBUG)
#define CLV2_01_04_ASSERT_FAIL(msg) \
    do { if (cluster_transition_assert_enabled) { assert(0 && (msg)); } } while (0)
#elif !defined(NDEBUG)
#define CLV2_01_04_ASSERT_FAIL(msg) do { assert(0 && (msg)); } while (0)
#else
#define CLV2_01_04_ASSERT_FAIL(msg) do { (void)0; } while (0)
#endif

/* CLV2-01-04d.0 (human auditor recommendation for the d-group's
 * irreversible-site hazards): the validation chain is extracted into a
 * shared static helper with ZERO writes and exposed as
 * cluster_transition_preflight(), so d-group sites (remove_member() /
 * expire_members() and other irreversible side-effect sites) can validate
 * BEFORE running their Current-order irreversible writes - a rejected
 * preflight aborts the site BEFORE any auxiliary state is committed
 * (no b.6-style half-commit). */

/* CLV2-01-04 RULE: cluster_transition() may centralize existing state
 * transitions, but MUST NOT create new protocol semantics.  Entry/exit
 * actions may only reproduce effects already performed by the migrated
 * legacy transition site. */
ucn_result_t cluster_transition(ucn_cluster_t *cluster,
                                       ucn_cluster_phase_t old_phase,
                                       ucn_cluster_phase_t new_phase,
                                       ucn_cluster_transition_reason_t reason,
                                       uint32_t now_ms)
    CLV2_01_04_UNUSED;

/* CLV2-01-04d.0: the full validation chain - ALL checks, NO writes.  A
 * rejection returns UCN_ERR_STATE (or UCN_ERR_ARGUMENT for NULL) and
 * leaves every field untouched. */
static ucn_result_t cluster_transition_validate(ucn_cluster_t *cluster,
                                                ucn_cluster_phase_t old_phase,
                                                ucn_cluster_phase_t new_phase,
                                                uint32_t now_ms)
{
    (void)now_ms;
    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    /* Fail closed: the caller's claimed old phase must match the current
     * shadow, and the pair must be legal.  Nothing is written before both
     * checks pass, so a rejection leaves every field untouched. */
    if ((unsigned int)old_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT ||
        (unsigned int)new_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT ||
        cluster->phase != old_phase ||
        !cluster_transition_is_allowed(old_phase, new_phase)) {
        CLV2_01_04_ASSERT_FAIL(
            "cluster_transition: illegal or mismatched phase transition");
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

/* CLV2-01-04d.0: pure validation, NEVER commits.  A d-group site calls
 * this BEFORE its irreversible Current-order side effects; if it rejects,
 * the site must abort BEFORE any auxiliary write so nothing is
 * half-committed.  The phase commit itself still goes through
 * cluster_transition(). */
ucn_result_t cluster_transition_preflight(ucn_cluster_t *cluster,
                                                 ucn_cluster_phase_t old_phase,
                                                 ucn_cluster_phase_t new_phase,
                                                 uint32_t now_ms)
    CLV2_01_04_UNUSED;

ucn_result_t cluster_transition_preflight(ucn_cluster_t *cluster,
                                                 ucn_cluster_phase_t old_phase,
                                                 ucn_cluster_phase_t new_phase,
                                                 uint32_t now_ms)
{
    return cluster_transition_validate(cluster, old_phase, new_phase, now_ms);
}

ucn_result_t cluster_transition(ucn_cluster_t *cluster,
                                       ucn_cluster_phase_t old_phase,
                                       ucn_cluster_phase_t new_phase,
                                       ucn_cluster_transition_reason_t reason,
                                       uint32_t now_ms)
{
    ucn_result_t result;

    /* CLV2-01-04 RULE: cluster_transition() may centralize existing state
     * transitions, but MUST NOT create new protocol semantics.  Entry/exit
     * actions may only reproduce effects already performed by the migrated
     * legacy transition site. */
    result = cluster_transition_validate(cluster, old_phase, new_phase,
                                         now_ms);
    if (result != UCN_OK) {
        return result;
    }
    /* 1) Commit the authoritative phase.  CLV2-01-04a review A (F4): an
     *    UNKNOWN or out-of-range reason must never be recorded on an
     *    accepted transition, so fall back to the BEST-EFFORT pair table.
     *    A non-UNKNOWN caller reason is accepted as-is (the table is not
     *    authoritative: a pair can be reached by several events, and the
     *    wiring stages will pass exact per-event reasons). */
    if (reason == UCN_CLUSTER_REASON_UNKNOWN ||
        (unsigned int)reason >= (unsigned int)UCN_CLUSTER_REASON_COUNT) {
        reason = cluster_reason_from_diff(old_phase, new_phase);
    }
    cluster->phase = new_phase;
    cluster->transition_reason = reason;
    cluster->transition_count++;
    /* 2) Apply phase entry invariants.  These are bounded side effects of
     *    the validated edge; they must never recreate a second role state. */
    cluster_transition_apply_phase(cluster, new_phase, now_ms);
    return UCN_OK;
}

#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS)
/* Test-only access to the static entry point.  Compiled only into the
 * ucn_tests copy of ucn_cluster.c (UCN_CLUSTER_ENABLE_TEST_HOOKS); the
 * production ucn_cluster archive keeps cluster_transition() static and
 * unreachable, so current behaviour is unchanged. */
ucn_result_t ucn_cluster_test_transition(ucn_cluster_t *cluster,
                                         ucn_cluster_phase_t old_phase,
                                         ucn_cluster_phase_t new_phase,
                                         ucn_cluster_transition_reason_t reason,
                                         uint32_t now_ms)
{
    return cluster_transition(cluster, old_phase, new_phase, reason, now_ms);
}

void ucn_cluster_test_transition_asserts_set(bool enabled)
{
    cluster_transition_assert_enabled = enabled;
}

/* CLV2-01-04d.0: test-only view of the pure-validation preflight (NEVER
 * commits), so tests can prove that a rejected preflight performs ZERO
 * writes. */
ucn_result_t ucn_cluster_test_transition_preflight(
    ucn_cluster_t *cluster,
    ucn_cluster_phase_t old_phase,
    ucn_cluster_phase_t new_phase,
    uint32_t now_ms)
{
    return cluster_transition_preflight(cluster, old_phase, new_phase,
                                        now_ms);
}

/* Test-only view of the BEST-EFFORT pair->reason table, so the matrix
 * test can pass a real per-pair reason (F4) instead of UNKNOWN. */
ucn_cluster_transition_reason_t ucn_cluster_test_reason_from_diff(
    ucn_cluster_phase_t old_phase, ucn_cluster_phase_t new_phase)
{
    return cluster_reason_from_diff(old_phase, new_phase);
}

/* CLV2-01-04b NIT-1: test-only observed-pair view.  It is the DIRECT
 * legality table plus the three tick-granularity compound observations that
 * the T-A collector can see.  Keep this as a function, rather than a static
 * table initialized from another static table: the latter is accepted by GCC
 * as an extension but rejected by MSVC as a non-constant initializer. */
bool ucn_cluster_test_observed_pair_allowed(ucn_cluster_phase_t old_phase,
                                            ucn_cluster_phase_t new_phase)
{
    if ((unsigned int)old_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT ||
        (unsigned int)new_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT) {
        return false;
    }
    if ((CLUSTER_TRANSITION_DIRECT_ALLOWED[old_phase] &
         (UINT32_C(1) << (unsigned int)new_phase)) != 0U) {
        return true;
    }
    return (old_phase == UCN_CLUSTER_PHASE_BACKUP_READY &&
            new_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) ||
           (old_phase == UCN_CLUSTER_PHASE_STEPPING_DOWN &&
            (new_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE ||
             new_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE));
}
#endif

#undef CLV2_01_04_UNUSED
#undef CLUSTER_TERM_CONFLICT_EDGE
