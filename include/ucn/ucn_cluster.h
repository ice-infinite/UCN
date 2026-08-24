#ifndef UCN_CLUSTER_H
#define UCN_CLUSTER_H

#include "ucn/ucn_node.h"
#include "ucn/ucn_cluster_epoch.h"
#include "ucn/ucn_cluster_membership.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional single-level Cluster control plane.  It is a separate Extended
 * object: products that do not instantiate/link it pay no Cluster RAM. */
#ifndef UCN_CLUSTER_MAX_PEERS
#define UCN_CLUSTER_MAX_PEERS ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_MAX_CANDIDATES
#define UCN_CLUSTER_MAX_CANDIDATES ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_MAX_MEMBERS
#define UCN_CLUSTER_MAX_MEMBERS ((size_t)16U)
#endif
#ifndef UCN_CLUSTER_OBSERVATION_MS
#define UCN_CLUSTER_OBSERVATION_MS UINT32_C(5000)
#endif
#ifndef UCN_CLUSTER_RECOVERY_OBSERVATION_MS
#define UCN_CLUSTER_RECOVERY_OBSERVATION_MS UINT32_C(5000)
#endif
#ifndef UCN_CLUSTER_ELECTION_WINDOW_MS
#define UCN_CLUSTER_ELECTION_WINDOW_MS UINT32_C(3000)
#endif
#ifndef UCN_CLUSTER_ADVERTISE_INTERVAL_MS
#define UCN_CLUSTER_ADVERTISE_INTERVAL_MS UINT32_C(1000)
#endif
#ifndef UCN_CLUSTER_JOIN_RETRY_MS
#define UCN_CLUSTER_JOIN_RETRY_MS UINT32_C(1000)
#endif
#ifndef UCN_CLUSTER_KEEPALIVE_INTERVAL_MS
#define UCN_CLUSTER_KEEPALIVE_INTERVAL_MS UINT32_C(2000)
#endif
#ifndef UCN_CLUSTER_LEASE_MS
#define UCN_CLUSTER_LEASE_MS UINT32_C(8000)
#endif
#ifndef UCN_CLUSTER_HEAD_MIN_TENURE_MS
#define UCN_CLUSTER_HEAD_MIN_TENURE_MS UINT32_C(30000)
#endif
#ifndef UCN_CLUSTER_SWITCH_IMPROVEMENT_PERCENT
#define UCN_CLUSTER_SWITCH_IMPROVEMENT_PERCENT ((uint8_t)20U)
#endif
#ifndef UCN_CLUSTER_SWITCH_REQUIRED_SAMPLES
#define UCN_CLUSTER_SWITCH_REQUIRED_SAMPLES ((uint8_t)3U)
#endif
#ifndef UCN_CLUSTER_FAST_OBSERVATION_MS
#define UCN_CLUSTER_FAST_OBSERVATION_MS UINT32_C(1000)
#endif
#ifndef UCN_CLUSTER_FAST_RECOVERY_OBSERVATION_MS
#define UCN_CLUSTER_FAST_RECOVERY_OBSERVATION_MS UINT32_C(500)
#endif
#ifndef UCN_CLUSTER_FAST_ELECTION_WINDOW_MS
#define UCN_CLUSTER_FAST_ELECTION_WINDOW_MS UINT32_C(1000)
#endif
#ifndef UCN_CLUSTER_FAST_ADVERTISE_INTERVAL_MS
#define UCN_CLUSTER_FAST_ADVERTISE_INTERVAL_MS UINT32_C(250)
#endif
#ifndef UCN_CLUSTER_FAST_JOIN_RETRY_MS
#define UCN_CLUSTER_FAST_JOIN_RETRY_MS UINT32_C(250)
#endif
#ifndef UCN_CLUSTER_FAST_KEEPALIVE_INTERVAL_MS
#define UCN_CLUSTER_FAST_KEEPALIVE_INTERVAL_MS UINT32_C(500)
#endif
#ifndef UCN_CLUSTER_FAST_LEASE_MS
#define UCN_CLUSTER_FAST_LEASE_MS UINT32_C(2000)
#endif
#ifndef UCN_CLUSTER_FAST_HEAD_MIN_TENURE_MS
#define UCN_CLUSTER_FAST_HEAD_MIN_TENURE_MS UINT32_C(10000)
#endif

/* CLV2-M03 (03-07): Cluster control serials must never silently wrap in
 * the same Cluster identity.  M13 will replace this fail-closed guard with
 * a quorum-committed Rekey; until then, callers must stop before the
 * threshold rather than reuse a serial value. */
#ifndef UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD
#define UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD \
    (UINT32_MAX - UINT32_C(1024))
#endif
typedef char ucn_cluster_serial_rotation_threshold_must_leave_valid_range[
    UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD > 1U &&
            UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD < UINT32_MAX ? 1 : -1];

#define UCN_CLUSTER_FORMAT_VERSION ((uint8_t)3U)

/* C07.2 Backup sync control. */
#ifndef UCN_CLUSTER_BACKUP_MISS_LIMIT
#define UCN_CLUSTER_BACKUP_MISS_LIMIT ((uint8_t)3U)
#endif
#ifndef UCN_CLUSTER_TAKEOVER_WINDOW_MS
#define UCN_CLUSTER_TAKEOVER_WINDOW_MS UINT32_C(1000)
#endif
/* Upper bound for one Backup membership-sync frame.  The full snapshot is
 * still static/bounded, but this prevents a conservative multi-second lease
 * from delaying failover readiness for the entire lease duration. */
#ifndef UCN_CLUSTER_BACKUP_SYNC_MAX_SPACING_MS
#define UCN_CLUSTER_BACKUP_SYNC_MAX_SPACING_MS UINT32_C(250)
#endif

/* C07.5 independent control-plane Token Bucket (never consumes the business
 * Q1 queue).  A single aggregate pool bounds the total control rate to the
 * Cluster window budget; over-budget sends are deferred, never silently
 * broadcast.  Burst + refill-rate is the worst-case count in one 1 s window. */
#ifndef UCN_CLUSTER_TB_BURST
#define UCN_CLUSTER_TB_BURST ((uint16_t)8U)
#endif
#ifndef UCN_CLUSTER_TB_REFILL_MS
#define UCN_CLUSTER_TB_REFILL_MS UINT32_C(41)
#endif
#ifndef UCN_CLUSTER_FAST_TB_BURST
#define UCN_CLUSTER_FAST_TB_BURST ((uint16_t)8U)
#endif
#ifndef UCN_CLUSTER_FAST_TB_REFILL_MS
#define UCN_CLUSTER_FAST_TB_REFILL_MS UINT32_C(32)
#endif

/* C07.5 RECOVERY_HEAD: short-lived emergency Head TTL and declaration
 * backoff.  M03-08 allocates a fresh recovery Cluster ID through the
 * identity provider (never impersonating the lost Cluster). */
#ifndef UCN_CLUSTER_RECOVERY_HEAD_TTL_MS
#define UCN_CLUSTER_RECOVERY_HEAD_TTL_MS UINT32_C(30000)
#endif
#ifndef UCN_CLUSTER_RECOVERY_BACKOFF_MAX_MS
#define UCN_CLUSTER_RECOVERY_BACKOFF_MAX_MS UINT32_C(2000)
#endif
#ifndef UCN_CLUSTER_FAST_RECOVERY_HEAD_TTL_MS
#define UCN_CLUSTER_FAST_RECOVERY_HEAD_TTL_MS UINT32_C(10000)
#endif
#ifndef UCN_CLUSTER_FAST_RECOVERY_BACKOFF_MAX_MS
#define UCN_CLUSTER_FAST_RECOVERY_BACKOFF_MAX_MS UINT32_C(500)
#endif
/* CLV2-M12 (12-03): bounded exponential recovery backoff base (attempt 0)
 * and the sustained-stable-join period after which the recovery lineage
 * and round are reset.  Zero selects the profile default. */
#ifndef UCN_CLUSTER_RECOVERY_BACKOFF_BASE_MS
#define UCN_CLUSTER_RECOVERY_BACKOFF_BASE_MS UINT32_C(200)
#endif
#ifndef UCN_CLUSTER_RECOVERY_LINEAGE_RESET_MS
#define UCN_CLUSTER_RECOVERY_LINEAGE_RESET_MS UINT32_C(30000)
#endif
#ifndef UCN_CLUSTER_FAST_RECOVERY_BACKOFF_BASE_MS
#define UCN_CLUSTER_FAST_RECOVERY_BACKOFF_BASE_MS UINT32_C(100)
#endif
#ifndef UCN_CLUSTER_FAST_RECOVERY_LINEAGE_RESET_MS
#define UCN_CLUSTER_FAST_RECOVERY_LINEAGE_RESET_MS UINT32_C(15000)
#endif
/* CLV2-M12 (12-08): minimum visible ADMITTED peers for a plain-member
 * Recovery self-declaration (the default forbids a fully isolated node). */
#ifndef UCN_CLUSTER_MIN_RECOVERY_PEERS
#define UCN_CLUSTER_MIN_RECOVERY_PEERS UINT32_C(1)
#endif

/* Flags valid only on BACKUP_MEMBER_SYNC (Format v3). */
#define UCN_CLUSTER_FLAG_SYNC_BEGIN ((uint8_t)0x01U)
#define UCN_CLUSTER_FLAG_SYNC_END ((uint8_t)0x02U)
/* C07.7 P1: a live (post-READY) incremental refresh frame: carries one
 * member's current nonce/lease without resetting the Backup's syncing
 * state, so a periodic refresh can never strand a ready Backup. */
#define UCN_CLUSTER_FLAG_SYNC_DELTA ((uint8_t)0x04U)
#define UCN_CLUSTER_KNOWN_FLAGS \
    (UCN_CLUSTER_FLAG_SYNC_BEGIN | UCN_CLUSTER_FLAG_SYNC_END | \
     UCN_CLUSTER_FLAG_SYNC_DELTA)
#define UCN_CLUSTER_CONTROL_ENDPOINT ((ucn_endpoint_t)0xA0U)
/* C07.7 P1: BACKUP_REJECT reasons. */
#define UCN_CLUSTER_BACKUP_REJECT_COVERAGE ((uint8_t)1U)
#define UCN_CLUSTER_BACKUP_REJECT_NO_SPACE ((uint8_t)2U)
#define UCN_CLUSTER_BACKUP_REJECT_UNSUPPORTED ((uint8_t)3U)
#define UCN_CLUSTER_BACKUP_REJECT_EPOCH_CONFLICT ((uint8_t)4U)
/* The active production Cluster wire is v3 and remains exactly 32 B.  Keep
 * the established name as an alias so existing callers cannot accidentally
 * start accepting a future wire format before its RX/FSM integration is
 * explicitly enabled. */
#define UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES ((size_t)32U)
#define UCN_CLUSTER_MESSAGE_BYTES UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES
#define UCN_CLUSTER_SCORE_MAX ((uint16_t)10000U)

typedef char ucn_cluster_peers_must_be_1_to_255[
    UCN_CLUSTER_MAX_PEERS >= 1U && UCN_CLUSTER_MAX_PEERS <= 255U ? 1 : -1];
typedef char ucn_cluster_candidates_must_be_1_to_255[
    UCN_CLUSTER_MAX_CANDIDATES >= 1U &&
            UCN_CLUSTER_MAX_CANDIDATES <= 255U ? 1 : -1];
/* C07 takeover acknowledgment storage is a static 32-bit bitmap.  A larger
 * per-Cluster membership table would need a deliberately sized bitmap and
 * wider counters, not silent undefined shifts. */
typedef char ucn_cluster_members_must_be_1_to_32[
    UCN_CLUSTER_MAX_MEMBERS >= 1U &&
            UCN_CLUSTER_MAX_MEMBERS <= 32U ? 1 : -1];

typedef enum ucn_cluster_role {
    UCN_CLUSTER_ROLE_DISABLED = 0,
    UCN_CLUSTER_ROLE_DETACHED = 1,
    UCN_CLUSTER_ROLE_JOIN_PENDING = 2,
    UCN_CLUSTER_ROLE_MEMBER = 3,
    UCN_CLUSTER_ROLE_CANDIDATE = 4,
    UCN_CLUSTER_ROLE_HEAD = 5,
    UCN_CLUSTER_ROLE_BACKUP = 6,
    UCN_CLUSTER_ROLE_STEPPING_DOWN = 7,
    UCN_CLUSTER_ROLE_RECOVERY_HEAD = 8,
    /* CLV2-M03 (03-05): local-only safe wait after observing two different
     * Head identities for the same stable (cluster_id, term).  It is never
     * advertised on the v3 wire; a higher-Term authority is required to
     * leave it. */
    UCN_CLUSTER_ROLE_TERM_CONFLICT = 9
} ucn_cluster_role_t;

/* DEFAULT is deliberately conservative and remains selected unless a product
 * explicitly applies another profile before ucn_cluster_init(). */
typedef enum ucn_cluster_timing_profile {
    UCN_CLUSTER_TIMING_PROFILE_DEFAULT = 0,
    UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED = 1
} ucn_cluster_timing_profile_t;

/* CLV2-01-01..CLV2-08-01: explicit phase names for the Cluster lifecycle.
 * Values 0..17 are frozen Current-FSM diagnostics; M08 appends rather than
 * renumbers its Head phases.  The Current FSM still owns shadow_phase; M08
 * uses the separate authority_phase state until the Authority Owner is
 * explicitly installed. */
typedef enum ucn_cluster_phase {
    UCN_CLUSTER_PHASE_DISABLED = 0,
    UCN_CLUSTER_PHASE_DETACHED_OBSERVE = 1,
    UCN_CLUSTER_PHASE_ELECTION = 2,
    UCN_CLUSTER_PHASE_JOIN_PENDING = 3,
    UCN_CLUSTER_PHASE_MEMBER_ACTIVE = 4,
    UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE = 5,
    UCN_CLUSTER_PHASE_HEAD_NO_BACKUP = 6,
    UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING = 7,
    UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING = 8,
    UCN_CLUSTER_PHASE_HEAD_STABLE = 9,
    UCN_CLUSTER_PHASE_BACKUP_SYNCING = 10,
    UCN_CLUSTER_PHASE_BACKUP_READY = 11,
    UCN_CLUSTER_PHASE_BACKUP_TAKEOVER = 12,
    UCN_CLUSTER_PHASE_STEPPING_DOWN = 13,
    UCN_CLUSTER_PHASE_RECOVERY_OBSERVE = 14,
    UCN_CLUSTER_PHASE_RECOVERY_ELECTION = 15,
    UCN_CLUSTER_PHASE_RECOVERY_HEAD = 16,
    UCN_CLUSTER_PHASE_TERM_CONFLICT_WAIT = 17,
    /* CLV2-08-01: no legacy role/bool combination may fabricate these.
     * HEAD_RECONFIGURING is M07 Config-owned; GRACE/FENCED imply no write
     * authority and are driven by the M08 Authority Owner. */
    UCN_CLUSTER_PHASE_HEAD_RECONFIGURING = 18,
    UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE = 19,
    UCN_CLUSTER_PHASE_HEAD_FENCED = 20,
    UCN_CLUSTER_PHASE_COUNT = 21
} ucn_cluster_phase_t;

/* A Head may retain identity context after it lost write authority.  This is
 * a public diagnostic reason, deliberately distinct from the legacy shadow
 * transition reason. */
typedef enum ucn_cluster_authority_fence_reason {
    UCN_CLUSTER_AUTHORITY_FENCE_NONE = 0,
    UCN_CLUSTER_AUTHORITY_FENCE_QUORUM_LOST = 1,
    UCN_CLUSTER_AUTHORITY_FENCE_GRACE_EXPIRED = 2,
    UCN_CLUSTER_AUTHORITY_FENCE_HIGHER_AUTHORITY = 3,
    UCN_CLUSTER_AUTHORITY_FENCE_TERM_CONFLICT = 4,
    UCN_CLUSTER_AUTHORITY_FENCE_PERSISTENCE_FAULT = 5,
    UCN_CLUSTER_AUTHORITY_FENCE_OWNER_STEP_BUDGET = 6
} ucn_cluster_authority_fence_reason_t;

/* CLV2-01-03 (M01 shadow phase): why the last shadow phase transition
 * happened.  During the shadow stage the reason is inferred from the
 * message type / legacy field delta; after CLV2-01-04 it will be set
 * explicitly by the single transition entry point. */
typedef enum ucn_cluster_transition_reason {
    UCN_CLUSTER_REASON_UNKNOWN = 0,
    UCN_CLUSTER_REASON_INIT = 1,
    UCN_CLUSTER_REASON_ELECTION_STARTED = 2,
    UCN_CLUSTER_REASON_ELECTION_WON = 3,
    UCN_CLUSTER_REASON_ELECTION_LOST = 4,
    UCN_CLUSTER_REASON_JOIN_INITIATED = 5,
    UCN_CLUSTER_REASON_JOIN_ACCEPTED = 6,
    UCN_CLUSTER_REASON_JOIN_REJECTED = 7,
    UCN_CLUSTER_REASON_HEAD_LEASE_EXPIRED = 8,
    UCN_CLUSTER_REASON_HEAD_LEASE_RENEWED = 9,
    UCN_CLUSTER_REASON_GRACE_TIMEOUT = 10,
    UCN_CLUSTER_REASON_BACKUP_ASSIGNED = 11,
    UCN_CLUSTER_REASON_BACKUP_SYNC_STARTED = 12,
    UCN_CLUSTER_REASON_SNAPSHOT_READY = 13,
    UCN_CLUSTER_REASON_BACKUP_LOST = 14,
    UCN_CLUSTER_REASON_RESYNC_STARTED = 15,
    UCN_CLUSTER_REASON_TAKEOVER_STARTED = 16,
    UCN_CLUSTER_REASON_TAKEOVER_QUORUM = 17,
    UCN_CLUSTER_REASON_TAKEOVER_TIMEOUT = 18,
    UCN_CLUSTER_REASON_STEPDOWN_ORDERED = 19,
    UCN_CLUSTER_REASON_STEPDOWN_COMPLETE = 20,
    UCN_CLUSTER_REASON_RECOVERY_ELIGIBLE = 21,
    UCN_CLUSTER_REASON_RECOVERY_BACKOFF = 22,
    UCN_CLUSTER_REASON_RECOVERY_WIN = 23,
    UCN_CLUSTER_REASON_RECOVERY_TTL_EXPIRED = 24,
    UCN_CLUSTER_REASON_PRIMARY_LOST = 25,
    UCN_CLUSTER_REASON_PRIMARY_RENEWED = 26,
    UCN_CLUSTER_REASON_LEAVE = 27,
    UCN_CLUSTER_REASON_RESET = 28,
    /* CLV2-M01.0.1: a Recovery contender that LOST the arbitration and
     * joined the winner.  Deliberately distinct from RECOVERY_WIN, which
     * only names the node that won. */
    UCN_CLUSTER_REASON_RECOVERY_YIELDED = 29,
    /* CLV2-M03 (03-04): a protected same-Cluster Head authority proof
     * carried a strictly higher Term.  This is intentionally distinct from
     * a score-driven JOIN_INITIATED or a peer-requested STEPDOWN_ORDERED so
     * every active phase can be audited as one global RX rule. */
    UCN_CLUSTER_REASON_HIGHER_AUTHORITY = 30,
    /* CLV2-M03 (03-05): same Cluster + same Term + different Head is a
     * safety conflict, never a score/Node-ID arbitration input. */
    UCN_CLUSTER_REASON_TERM_CONFLICT = 31,
    /* CLV2-M12 (12-07): a recovery-domain Member reclaims to a legal
     * stable Head of its parent lineage (stable precedence; no score
     * comparison is involved). */
    UCN_CLUSTER_REASON_STABLE_RECLAIM = 32,
    UCN_CLUSTER_REASON_COUNT = 33
} ucn_cluster_transition_reason_t;

/* CLV2-01-01: compile-time fences so an accidental enum extension is
 * caught at build time, not in a test run (C99-compatible assertion). */
typedef char ucn_cluster_phase_count_must_be_21[
    UCN_CLUSTER_PHASE_COUNT == 21 ? 1 : -1];
/* CLV2-M12 (12-07): extended by exactly one reason (STABLE_RECLAIM). */
typedef char ucn_cluster_reason_count_must_be_33[
    UCN_CLUSTER_REASON_COUNT == 33 ? 1 : -1];

typedef enum ucn_cluster_message_type {
    UCN_CLUSTER_MSG_ADVERTISE = 1,
    UCN_CLUSTER_MSG_JOIN_REQUEST = 2,
    UCN_CLUSTER_MSG_JOIN_ACCEPT = 3,
    UCN_CLUSTER_MSG_JOIN_REJECT = 4,
    UCN_CLUSTER_MSG_KEEPALIVE = 5,
    UCN_CLUSTER_MSG_LEAVE = 6,
    UCN_CLUSTER_MSG_HEAD_DECLARE = 7,
    UCN_CLUSTER_MSG_HEAD_TAKEOVER = 8,
    UCN_CLUSTER_MSG_HEAD_STEPDOWN = 9,
    UCN_CLUSTER_MSG_BACKUP_ASSIGN = 10,
    UCN_CLUSTER_MSG_BACKUP_READY = 11,
    UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC = 12,
    UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT = 13,
    UCN_CLUSTER_MSG_TAKEOVER_PREPARE = 14,
    UCN_CLUSTER_MSG_TAKEOVER_ACK = 15,
    UCN_CLUSTER_MSG_RECOVERY_DECLARE = 16,
    UCN_CLUSTER_MSG_RECOVERY_ACK = 17,
    /* C07.7 P1: Backup -> Head, requests a full resync after a DELTA gap. */
    UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ = 18,
    /* C07.7 P1: Backup -> Head, rejects the assignment (coverage / no
     * space / unsupported) so the Head can immediately pick the next
     * candidate instead of waiting for the member lease to expire. */
    UCN_CLUSTER_MSG_BACKUP_REJECT = 19
} ucn_cluster_message_type_t;

/* Fixed 32 B wire format v3.  The first 16 B are shared; the trailing
 * 16 B are reinterpreted per Message Type.  Types 1-7/9 retain the legacy
 * layout.  HEAD_TAKEOVER carries the Backup generation in v3, so a Member can
 * verify that a promotion came from its Head-announced Backup.  Type 12 uses
 * the full 16 B trailing for backup_generation + member_node_id +
 * membership_sequence + member_nonce (all 32-bit). */
typedef struct ucn_cluster_message {
    ucn_cluster_message_type_t type;
    ucn_cluster_role_t role;
    uint8_t flags;
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
    /* Type 1-7/9 legacy trailing 12 B; Type 8 is secure v3 takeover data. */
    uint16_t head_score;
    uint16_t available_capacity;
    uint32_t lease_ms;
    uint32_t nonce;
    /* Type 10-17 trailing 12 B (selected by type at encode/decode). */
    uint32_t backup_generation;
    uint32_t membership_sequence;
    ucn_node_id_t member_node_id;
    uint32_t member_lease_ms;
    /* C07.7 P1: full 32-bit member nonce on the wire; a 16-bit field
     * truncated the member counter so a takeover could re-accept stale
     * nonces after wrap (anti-replay hole). */
    uint32_t member_nonce;
    uint32_t recovery_nonce;
    uint32_t recovery_ttl_ms;
    /* CLV2-M12 (12-04): the declaring Recovery Head's parent lineage
     * identity, carried in the previously zeroed trailing word.  0 =
     * legacy/unknown parent (old frames).  Terms are only ever compared
     * between equal non-zero parents. */
    uint32_t recovery_parent_cluster_id;
    uint32_t sync_token;
    /* C07.7 P1: BACKUP_REJECT reason (COVERAGE_FAILED / NO_SPACE /
     * UNSUPPORTED / EPOCH_CONFLICT). */
    uint8_t reject_reason;
} ucn_cluster_message_t;

/* C07.5 control-plane Token Bucket state.  The Snapshot is sequence-
 * sensitive: send_backup_snapshot_step() commits its cursor only after a
 * successful transmit so a deferred frame never leaves a sequence gap. */
typedef struct ucn_cluster_token_bucket {
    uint16_t tokens;
    uint32_t last_refill_ms;
} ucn_cluster_token_bucket_t;

/* Product override for the C07.5 control-plane budget.  Zero fields select
 * the profile defaults (see UCN_CLUSTER_TB_BURST / UCN_CLUSTER_TB_REFILL_MS
 * and the FAST_ variants).  Set a large burst and 1 ms refill to disable
 * throttling for unit fixtures that compress real time. */
typedef struct ucn_cluster_token_bucket_config {
    uint16_t burst;
    uint32_t refill_ms;
} ucn_cluster_token_bucket_config_t;

typedef ucn_result_t (*ucn_cluster_send_fn)(
    void *context,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length);
typedef uint32_t (*ucn_cluster_now_ms_fn)(void *context);

/* CLV2-M03 (03-08): a product may own Cluster identity generation without
 * making the Cluster core depend on a filesystem, Flash SDK or OS RNG.
 * `incarnation` is application supplied (normally a boot counter); `round`
 * is monotonically allocated by this Cluster object for each new identity.
 * The callback must return a non-zero, non-broadcast ID distinct from
 * `parent_cluster_id` when that parent is non-zero. */
typedef enum ucn_cluster_id_purpose {
    UCN_CLUSTER_ID_PURPOSE_ELECTION = 1,
    UCN_CLUSTER_ID_PURPOSE_RECOVERY = 2,
    UCN_CLUSTER_ID_PURPOSE_REKEY = 3
} ucn_cluster_id_purpose_t;

typedef struct ucn_cluster_id_request {
    ucn_cluster_id_purpose_t purpose;
    ucn_node_id_t local_node_id;
    uint32_t parent_cluster_id;
    uint32_t parent_term;
    uint32_t incarnation;
    uint32_t round;
    /* CLV2-M12 (12-02): recovery lineage inputs, appended for source
     * compatibility.  Zero for non-Recovery purposes; a Provider may key
     * distinct Recovery identities on them. */
    uint32_t parent_config_id;
    uint32_t recovery_round;
} ucn_cluster_id_request_t;

typedef ucn_result_t (*ucn_cluster_make_id_fn)(
    void *context,
    const ucn_cluster_id_request_t *request,
    uint32_t *cluster_id);

/* The persistence provider is defined by ucn_cluster_persist.h.  Keep only a
 * forward declaration here so applications can configure Cluster without
 * pulling the Record-v1 codec or a storage implementation into every user of
 * this public header. */
struct ucn_cluster_persist_provider;
struct ucn_cluster_authority_runtime;

/* M04-03 configuration boundary.  The zero/default value is intentionally
 * fail-closed: a production Cluster must supply a compatible persistence
 * provider before init succeeds.  VOLATILE_TEST is an explicit host/unit-test
 * opt-in; it authorizes no reboot safety and must never be used to claim a
 * persist-before-promise deployment. */
typedef enum ucn_cluster_persistence_mode {
    UCN_CLUSTER_PERSISTENCE_REQUIRED = 0,
    UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST = 1
} ucn_cluster_persistence_mode_t;

/* The result of the synchronous M04-04 init-time load.  NOT_APPLICABLE means
 * the caller explicitly selected VOLATILE_TEST; it never means an unknown or
 * failed REQUIRED load, because those cases fail init and leave no Cluster
 * object to run. */
typedef enum ucn_cluster_persistence_restore_state {
    UCN_CLUSTER_PERSISTENCE_RESTORE_NOT_APPLICABLE = 0,
    UCN_CLUSTER_PERSISTENCE_RESTORE_FACTORY_EMPTY = 1,
    UCN_CLUSTER_PERSISTENCE_RESTORE_READY = 2
} ucn_cluster_persistence_restore_state_t;

typedef struct ucn_cluster_config {
    ucn_node_id_t local_node_id;
    bool enabled;
    bool head_capable;
    bool require_protected_control;
    uint16_t head_score;
    uint16_t member_capacity;
    uint32_t observation_ms;
    uint32_t election_window_ms;
    uint32_t advertise_interval_ms;
    uint32_t join_retry_ms;
    uint32_t keepalive_interval_ms;
    uint32_t lease_ms;
    uint32_t head_min_tenure_ms;
    uint8_t switch_improvement_percent;
    uint8_t switch_required_samples;
    ucn_cluster_now_ms_fn now_ms;
    void *now_context;
    ucn_cluster_send_fn send;
    void *send_context;
    /* Appended for source compatibility with existing positional config
     * initializers. Zero selects UCN_CLUSTER_RECOVERY_OBSERVATION_MS. */
    uint32_t recovery_observation_ms;
    /* C07.5 independent control-plane Token Bucket override; zero fields
     * select the §9.1 defaults. */
    ucn_cluster_token_bucket_config_t token_bucket;
    /* C07.5 RECOVERY_HEAD TTL and declaration backoff (zero selects the
     * profile default). */
    uint32_t recovery_head_ttl_ms;
    uint32_t recovery_backoff_max_ms;
    /* CLV2-M12 (12-03): exponential backoff base (attempt 0) and the
     * sustained-stable-join period after which the recovery lineage and
     * round are reset.  Zero selects the profile default. */
    uint32_t recovery_backoff_base_ms;
    uint32_t recovery_lineage_reset_ms;
    /* CLV2-M12 (12-08): minimum visible ADMITTED peers a PLAIN member
     * (no Backup mirror) must see before it may self-declare a Recovery
     * Head.  Zero selects the profile default (1).  A Backup with a
     * membership mirror keeps its own distinct majority threshold. */
    uint32_t min_recovery_peers;
    /* Optional identity provider.  With NULL, UCN uses a deterministic Host
     * default derived from this request.  Set incarnation from a product's
     * boot counter/RNG/secure store when distinct IDs across reboot matter;
     * M04 later makes that value persistence-backed. */
    ucn_cluster_make_id_fn make_cluster_id;
    void *cluster_id_context;
    uint32_t cluster_id_incarnation;
    /* Appended for source compatibility with existing positional config
     * initializers.  Zero selects REQUIRED and therefore fails closed unless
     * persistence_provider is compatible. */
    ucn_cluster_persistence_mode_t persistence_mode;
    const struct ucn_cluster_persist_provider *persistence_provider;
    /* Appended for positional-initializer compatibility.  Bounded lifetime
     * of a Runtime PROVISIONAL member; zero selects the profile default and
     * is intentionally independent from the ordinary post-commit lease. */
    uint32_t provisional_timeout_ms;
    /* Appended for positional-initializer compatibility.  Protected voter
     * capacity includes the Head.  Zero selects member_capacity + 1 for a
     * head-capable product; it does not change Runtime capacity. */
    uint16_t voter_capacity;
} ucn_cluster_config_t;

typedef struct ucn_cluster_peer {
    bool occupied;
    ucn_node_id_t node_id;
    ucn_neighbor_state_t neighbor_state;
    uint32_t last_seen_ms;
} ucn_cluster_peer_t;

typedef struct ucn_cluster_candidate {
    bool occupied;
    ucn_node_id_t head_node_id;
    uint32_t cluster_id;
    uint32_t term;
    uint16_t head_score;
    uint16_t available_capacity;
    uint32_t expires_at_ms;
    uint32_t last_nonce;
    ucn_cluster_role_t role;
    uint8_t better_samples;
} ucn_cluster_candidate_t;

/* Owner-context read-only snapshots for optional Cluster extensions.  They
 * avoid making an extension depend on the mutable ucn_cluster_t layout. */
typedef struct ucn_cluster_view {
    bool enabled;
    /* Exposes REQUIRED versus explicit VOLATILE_TEST operation to diagnostics.
     * The paired restore state says whether synchronous M04-04 init recovered
     * Factory Empty or a validated READY record. */
    ucn_cluster_persistence_mode_t persistence_mode;
    ucn_cluster_persistence_restore_state_t persistence_restore_state;
    /* M04 runtime progress/fault visibility.  A REQUIRED pending write blocks
     * every outward Cluster promise until poll() proves it committed. */
    bool persistence_pending;
    bool persistence_faulted;
    /* A durable response whose previous transport attempt was back-pressured.
     * It is diagnostic-only: no uncommitted state is represented here. */
    bool persistence_retry_pending;
    ucn_result_t persistence_failure;
    ucn_cluster_role_t role;
    ucn_node_id_t local_node_id;
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
    /* Volatile history snapshot for diagnostics and safe rejoin decisions.
     * It is RAM-only until the M04 persistence provider is enabled. */
    uint32_t last_cluster_id;
    uint32_t max_seen_term;
    ucn_node_id_t last_stable_head;
    /* Monotonic allocation round for optional Cluster ID generation.  It is
     * RAM-only until M04 persistence; never reuse within this object. */
    uint32_t cluster_id_round;
    /* CLV2-M12 (12-01): recovery lineage snapshot (see the struct fields). */
    uint32_t parent_cluster_id;
    uint32_t parent_term;
    uint32_t parent_config_id;
    uint32_t recovery_round;
    uint16_t current_head_score;
    /* CLV2-08-02: role indicates retained identity context only.  These
     * fields are the sole public authority decision; applications and
     * extensions must not infer write permission from role == HEAD. */
    bool authority_active;
    ucn_cluster_phase_t authority_phase;
    ucn_cluster_authority_fence_reason_t authority_fence_reason;
} ucn_cluster_view_t;

typedef struct ucn_cluster_member_summary {
    ucn_node_id_t node_id;
    uint32_t lease_expires_at_ms;
    /* CLV2-M06 (06-07): owner-context diagnostics only.  status/voting are
     * copied from the member record; config_id is the current canonical
     * active voter-set ID, or zero before M07 has installed one. */
    uint8_t status;
    bool voting;
    uint32_t config_id;
} ucn_cluster_member_summary_t;

typedef struct ucn_cluster_member_capacity_view {
    uint16_t runtime_capacity;
    uint16_t runtime_used;
    uint16_t runtime_available;
    /* Includes Head; current used is zero before M07 installs a canonical
     * active voter set. */
    uint16_t voter_capacity;
    uint16_t voter_used;
    uint16_t voter_available;
} ucn_cluster_member_capacity_view_t;

typedef struct ucn_cluster_stats {
    /* Mirrors the configured persistence contract so test-only volatile
     * operation cannot be mistaken for a production-backed instance. */
    ucn_cluster_persistence_mode_t persistence_mode;
    ucn_cluster_persistence_restore_state_t persistence_restore_state;
    uint32_t persistence_submitted;
    uint32_t persistence_committed;
    uint32_t persistence_pending;
    uint32_t persistence_retry_attempts;
    uint32_t persistence_failures;
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t malformed_messages;
    uint32_t security_rejected;
    uint32_t stale_messages;
    uint32_t send_failures;
    uint32_t elections_started;
    uint32_t elections_won;
    uint32_t joins_requested;
    uint32_t joins_accepted;
    uint32_t joins_rejected;
    uint32_t member_leases_expired;
    uint32_t head_leases_expired;
    uint32_t head_switches;
    uint32_t token_deferred;
    /* Appended so legacy positional diagnostic initializers remain valid. */
    uint32_t provisional_members_expired;
} ucn_cluster_stats_t;

typedef struct ucn_cluster {
    ucn_cluster_config_t config;
    /* M04 runtime transaction metadata.  The full logical Record remains
     * Provider-owned: after a PENDING completion UCN reloads it and verifies
     * this durable journal identity before any FSM action can proceed. */
    bool persistence_pending;
    bool persistence_faulted;
    bool persistence_retry_pending;
    /* Covers the dynamic extent of every Provider load/submit/poll callback.
     * A callback cannot synchronously re-enter the Cluster state machine. */
    bool persistence_io_active;
    bool persistence_retry_dispatch;
    uint8_t persistence_pending_action;
    uint8_t persistence_pending_operation;
    uint8_t persistence_retry_action;
    ucn_result_t persistence_failure;
    ucn_node_id_t persistence_pending_destination;
    ucn_node_id_t persistence_retry_destination;
    uint32_t persistence_pending_token;
    uint32_t persistence_pending_operation_id;
    uint32_t persistence_pending_fingerprint;
    ucn_cluster_role_t role;
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
    /* CLV2-M03 (03-06): volatile safety history intentionally survives
     * set_detached().  It guards one most-recent stable Cluster domain until
     * M04 replaces this RAM-only state with a persistence-backed record. */
    uint32_t last_cluster_id;
    uint32_t max_seen_term;
    ucn_node_id_t last_stable_head;
    /* Monotonic allocation round for optional Cluster ID generation.  It is
     * RAM-only until M04 persistence; never reuse within this object. */
    uint32_t cluster_id_round;
    uint16_t current_head_score;
    uint32_t observation_deadline_ms;
    uint32_t role_since_ms;
    uint32_t election_deadline_ms;
    uint32_t head_lease_expires_at_ms;
    uint32_t head_grace_deadline_ms;
    uint32_t next_advertise_ms;
    uint32_t next_join_retry_ms;
    uint32_t next_keepalive_ms;
    uint32_t next_nonce;
    uint8_t advertise_cursor;
    ucn_node_id_t pending_head_node_id;
    uint32_t pending_cluster_id;
    uint32_t pending_term;
    uint16_t pending_head_score;
    ucn_cluster_peer_t peers[UCN_CLUSTER_MAX_PEERS];
    ucn_cluster_candidate_t candidates[UCN_CLUSTER_MAX_CANDIDATES];
    /* CLV2-M06 (06-02): a single fixed primary table is retained for RAM
     * compatibility.  Head/Recovery Head interpret it as their Runtime
     * Member table; Backup interprets it as its committed mirror.  M09 will
     * add an explicit staging mirror instead of overloading this table. */
    ucn_cluster_member_table_t primary_members;
    /* CLV2-M06 (06-03): future protected configuration data is physically
     * separate from the Runtime member table.  M06 does not yet let this
     * value grant a vote, a certificate, Backup eligibility or Authority;
     * M07/M10 own those transition and quorum semantics. */
    ucn_cluster_voter_set_t active_voter_set;
    /* C07.2 Backup state.  Only the assigned Backup keeps this primary
     * table as a live synchronized committed mirror. */
    ucn_node_id_t backup_node_id;
    uint32_t backup_generation;
    uint32_t membership_sequence;
    bool backup_ready;
    bool backup_syncing;
    bool backup_assign_pending;
    uint8_t backup_assign_cursor;
    uint8_t backup_assign_remaining;
    uint32_t next_backup_assign_ms;
    ucn_node_id_t backup_primary_node_id;
    uint32_t backup_sync_cursor;
    uint32_t next_backup_heartbeat_ms;
    uint32_t next_backup_sync_ms;
    /* C07.7 P1: post-READY incremental refresh state (round-robin DELTA). */
    uint8_t backup_delta_cursor;
    uint32_t next_backup_delta_ms;
    uint32_t backup_resync_deadline_ms;
    uint32_t backup_primary_deadline_ms;
    uint32_t backup_primary_lease_deadline_ms;
    uint8_t backup_missed_heartbeats;
    /* C07.3 majority-confirmed takeover. */
    bool backup_takeover_active;
    uint32_t backup_takeover_deadline_ms;
    uint8_t backup_takeover_ack_count;
    uint32_t backup_takeover_acked;
    uint8_t backup_takeover_prepare_cursor;
    uint8_t backup_takeover_announce_cursor;
    uint8_t backup_takeover_announce_remaining;
    bool backup_takeover_announce_active;
    ucn_node_id_t known_backup_node_id;
    uint32_t known_backup_generation;
    /* C07.7 P1: the nonce of the JOIN_REQUEST this node last sent;
     * JOIN_ACCEPT/JOIN_REJECT must echo it back as the join transaction
     * id so a stale reject of an earlier attempt cannot abort a new one. */
    uint32_t pending_join_nonce;
    /* C07.7 P1: last accepted HEAD_STEPDOWN nonce (member side) so a
     * replayed stepdown of the same epoch is ignored. */
    uint32_t last_stepdown_nonce;
    /* C07.7 P1: Head-side cooldown for a Backup candidate that rejected
     * the assignment; skip it while cooling down. */
    uint32_t backup_candidate_cooldown_until_ms;
    ucn_node_id_t backup_rejected_node_id;
    /* C07.7 P1: the member's takeover vote is keyed on
     * (cluster_id, term, backup_generation), not term alone, so a vote in
     * one Cluster cannot block a legitimate takeover in another Cluster
     * that happens to share the same term number. */
    uint32_t member_voted_term;
    uint32_t member_voted_cluster_id;
    uint32_t member_voted_generation;
    /* C07.5 control-plane token bucket. */
    ucn_cluster_token_bucket_t token_bucket;
    /* C07.5 RECOVERY_HEAD state. */
    bool recovery_eligible;
    uint32_t stepdown_deadline_ms;
    uint32_t recovery_cluster_id;
    uint32_t recovery_deadline_ms;
    uint32_t recovery_cooldown_until_ms;
    uint32_t recovery_backoff_deadline_ms;
    uint32_t recovery_nonce;
    /* C07.7 P0-3: the remote recovery nonce this node accepted, kept
     * separate from the local election nonce so a remote RECOVERY_DECLARE
     * can never clobber our own candidacy nonce. */
    uint32_t accepted_recovery_nonce;
    ucn_node_id_t known_recovery_source;
    /* Saturating diagnostic evidence for the current Recovery identity.
     * It is not a live-quorum counter: current member liveness and expiry
     * are owned by the membership table. */
    uint8_t recovery_ack_count;
    uint32_t recovery_acked;
    /* CLV2-M12 (12-01): recovery lineage captured at the fence exit from
     * the parent cluster, BEFORE Active/Pending identity is cleared.  It
     * survives detach (like the 03-06 history) and drives recovery ID
     * generation (12-02), same-parent rank arbitration (12-04) and stable
     * reclaim precedence (12-07).  Cleared only after a sustained stable
     * join (12-03 reset).  A recovery domain itself is never a parent. */
    uint32_t parent_cluster_id;
    uint32_t parent_term;
    uint32_t parent_config_id;
    uint32_t recovery_round;
    /* Pending canonical config_id binding consumed by the next lineage
     * capture (ucn_cluster_lineage_bind_config).  0 = no config lineage
     * (the v3 default product has no Config owner). */
    uint32_t lineage_config_binding;
    /* CLV2-M12 (12-03): armed when a stable join completes; expiry resets
     * the recovery lineage + round.  Cancelled by any detach. */
    uint32_t lineage_reset_deadline_ms;
    ucn_cluster_stats_t stats;
    /* CLV2-01-01..03 (M01 shadow): derived-only mirror of the legacy
     * role+bool+deadline state.  Production logic MUST NOT read these
     * fields to make decisions during M01; they exist so the shadow
     * consistency gate can prove the phase mapping before CLV2-01-04
     * makes Phase the real driver. */
    ucn_cluster_phase_t shadow_phase;
    ucn_cluster_transition_reason_t transition_reason;
    uint32_t shadow_transition_count;
    /* CLV2-08-01: M08 Authority/Fence skeleton.  authority_active stays
     * false until 08-02/03 bind a canonical Config and lease Owner; no old
     * role-derived path is allowed to set it. */
    bool authority_active;
    ucn_cluster_phase_t authority_phase;
    ucn_cluster_authority_fence_reason_t authority_fence_reason;
    ucn_cluster_phase_t head_resume_phase;
    uint32_t quorum_loss_deadline_ms;
    uint32_t quorum_restore_since_ms;
    uint32_t fenced_dissolve_deadline_ms;
    /* Non-NULL only after a caller explicitly installs the M08 controlled
     * Authority Owner.  This keeps M05's production-v4 hold separate from
     * Host/experiment authority tests. */
    struct ucn_cluster_authority_runtime *authority_runtime;
} ucn_cluster_t;

ucn_result_t ucn_cluster_message_encode(
    const ucn_cluster_message_t *message,
    uint8_t output[UCN_CLUSTER_MESSAGE_BYTES]);
ucn_result_t ucn_cluster_message_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_message_t *message);
ucn_result_t ucn_cluster_init(
    ucn_cluster_t *cluster,
    const ucn_cluster_config_t *config);
/* Fill only timing fields in an application-owned configuration.  Apply this
 * before ucn_cluster_init(); it never changes identity, security, capacity,
 * score or callback fields. */
ucn_result_t ucn_cluster_config_apply_timing_profile(
    ucn_cluster_config_t *config,
    ucn_cluster_timing_profile_t profile);
/* Owner-context update for a product-computed, filtered Head capability score.
 * It updates advertisements immediately but does not by itself force a Head
 * to step down; merge/stepdown policy remains a separate lifecycle decision. */
ucn_result_t ucn_cluster_set_head_score(
    ucn_cluster_t *cluster,
    uint16_t head_score);
ucn_result_t ucn_cluster_sync_neighbors(
    ucn_cluster_t *cluster,
    const ucn_neighbor_summary_t *neighbors,
    size_t neighbor_count);
ucn_result_t ucn_cluster_sync_node_neighbors(
    ucn_cluster_t *cluster,
    const ucn_node_t *node);
ucn_result_t ucn_cluster_receive(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    bool protected_control,
    const uint8_t *payload,
    size_t payload_length);
ucn_result_t ucn_cluster_step(ucn_cluster_t *cluster);
ucn_cluster_role_t ucn_cluster_get_role(const ucn_cluster_t *cluster);
/* The only public write-authority predicate.  It returns false for NULL and
 * for every Cluster without an installed M08 Authority Owner. */
bool ucn_cluster_authority_active(const ucn_cluster_t *cluster);
/* Lets extensions preserve their legacy read-only behaviour when M08 has not
 * been explicitly installed, while requiring an active authority once it is
 * managed.  It is not a production-v4 enable switch. */
bool ucn_cluster_authority_is_managed(const ucn_cluster_t *cluster);
size_t ucn_cluster_member_count(const ucn_cluster_t *cluster);
/* Owner-context snapshots.  With output == NULL and capacity == 0, member
 * copy returns the number of occupied slots; otherwise it copies at most
 * capacity entries and returns the number copied. */
ucn_result_t ucn_cluster_get_view(const ucn_cluster_t *cluster,
                                  ucn_cluster_view_t *view);
size_t ucn_cluster_copy_member_summaries(
    const ucn_cluster_t *cluster,
    ucn_cluster_member_summary_t *output,
    size_t capacity);
/* Return one occupied Member table slot without exposing mutable storage.
 * table_index is the fixed Member-table index, not a compact ordinal. */
ucn_result_t ucn_cluster_get_member_summary_at(
    const ucn_cluster_t *cluster,
    size_t table_index,
    ucn_cluster_member_summary_t *summary);
ucn_result_t ucn_cluster_get_member_capacity_view(
    const ucn_cluster_t *cluster,
    ucn_cluster_member_capacity_view_t *view);
const ucn_cluster_stats_t *ucn_cluster_get_stats(const ucn_cluster_t *cluster);

/* CLV2-M03 (03-02): the ACTIVE epoch - the logical unification of the
 * node's current cluster_id / term / head_node_id.  PHYSICAL STORAGE IS
 * UNCHANGED (the struct still holds the three scalar fields; the struct
 * size / cluster_bytes is frozen by the M01/M02 gates).  This accessor is
 * the single read point for the active epoch, and 03-03+ will drive
 * Head-Offer / Merge / Higher-Authority decisions through
 * ucn_cluster_epoch_compare() on it instead of hand-written combined
 * comparisons.
 *
 * CONTRACT (03-02 MINOR cleanup, human audit): returns {0,0,0} for NULL.
 * For a non-NULL object this is a RAW READ PROJECTION of cluster_id /
 * term / head_node_id - it does NOT inspect the role and does NOT
 * validate liveness or validity (comparator != validator, 03-01).  Under
 * normal Cluster invariants a DETACHED node exposes {0,0,0} because
 * set_detached() clears those three fields - that is a STATE INVARIANT,
 * not a guarantee enforced by this getter.  Callers (03-03+) must not
 * treat "getter returned non-zero" as "this is a valid Active Epoch";
 * they call it from a Role/state that already has legitimate active
 * authority, or define validity at the call site. */
ucn_cluster_epoch_t ucn_cluster_active_epoch_get(const ucn_cluster_t *cluster);

/* CLV2-M12 (12-01): record the parent cluster's canonical config_id so the
 * NEXT recovery-lineage capture carries it.  0 = no config lineage (the v3
 * default product has no Config owner).  The experimental M07 Config owner
 * is the intended caller; the value is consumed by cluster_lineage_capture
 * at the Member/Backup fence exit and reset when the parent changes. */
void ucn_cluster_lineage_bind_config(ucn_cluster_t *cluster,
                                     uint32_t config_id);

/* CLV2-M12 (12-05): true while the node is inside a recovery control
 * domain (RECOVERY_HEAD role, or a member whose active cluster_id is the
 * recovery domain ID).  A recovery-scoped node only ever holds
 * recovery-local authority: the M08 Authority Owner must reject it, the
 * Federation must not publish its Directory records, and experimental
 * owners (M07 Config commit, M10 takeover) must use this predicate to
 * refuse targeting the parent cluster identity. */
bool ucn_cluster_recovery_scoped(const ucn_cluster_t *cluster);

/* CLV2-M12 (12-04): deterministic Recovery rank.  Same parent ranks by
 * parent_term DESC, parent_config_id DESC, score DESC, node_id ASC;
 * different parents are UNRANKABLE (ordinary Merge is the M11 handover
 * path, never a recovery-arbitration cross-yield).  A zero parent means
 * "unknown lineage" and is never rankable against a non-zero parent. */
typedef struct ucn_cluster_recovery_rank {
    uint32_t parent_cluster_id;
    uint32_t parent_term;
    uint32_t parent_config_id;
    uint16_t score;
    ucn_node_id_t node_id;
} ucn_cluster_recovery_rank_t;

typedef enum ucn_cluster_recovery_rank_relation {
    UCN_CLUSTER_RECOVERY_RANK_UNRANKABLE = 0,
    UCN_CLUSTER_RECOVERY_RANK_A_WINS = 1,
    UCN_CLUSTER_RECOVERY_RANK_B_WINS = 2,
    UCN_CLUSTER_RECOVERY_RANK_EQUAL = 3
} ucn_cluster_recovery_rank_relation_t;

ucn_cluster_recovery_rank_relation_t ucn_cluster_recovery_rank_compare(
    const ucn_cluster_recovery_rank_t *a,
    const ucn_cluster_recovery_rank_t *b);

#ifdef __cplusplus
}
#endif

#endif
