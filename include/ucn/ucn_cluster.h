#ifndef UCN_CLUSTER_H
#define UCN_CLUSTER_H

#include "ucn/ucn_node.h"

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
 * backoff.  The recovery Cluster ID is the declaring node ID (a fresh
 * domain, never impersonating the lost Cluster). */
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

/* Flags valid only on BACKUP_MEMBER_SYNC (Format v3). */
#define UCN_CLUSTER_FLAG_SYNC_BEGIN ((uint8_t)0x01U)
#define UCN_CLUSTER_FLAG_SYNC_END ((uint8_t)0x02U)
#define UCN_CLUSTER_KNOWN_FLAGS \
    (UCN_CLUSTER_FLAG_SYNC_BEGIN | UCN_CLUSTER_FLAG_SYNC_END)
#define UCN_CLUSTER_CONTROL_ENDPOINT ((ucn_endpoint_t)0xA0U)
#define UCN_CLUSTER_MESSAGE_BYTES ((size_t)28U)
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
    UCN_CLUSTER_ROLE_RECOVERY_HEAD = 8
} ucn_cluster_role_t;

/* DEFAULT is deliberately conservative and remains selected unless a product
 * explicitly applies another profile before ucn_cluster_init(). */
typedef enum ucn_cluster_timing_profile {
    UCN_CLUSTER_TIMING_PROFILE_DEFAULT = 0,
    UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED = 1
} ucn_cluster_timing_profile_t;

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
    UCN_CLUSTER_MSG_RECOVERY_ACK = 17
} ucn_cluster_message_type_t;

/* Fixed 28 B wire format v3.  The first 16 B are shared; the trailing
 * 12 B are reinterpreted per Message Type.  Types 1-7/9 retain the legacy
 * layout.  HEAD_TAKEOVER carries the Backup generation in v3, so a Member can
 * verify that a promotion came from its Head-announced Backup. */
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
    uint16_t member_nonce;
    uint32_t recovery_nonce;
    uint32_t recovery_ttl_ms;
    uint32_t sync_token;
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

typedef struct ucn_cluster_member {
    bool occupied;
    ucn_node_id_t node_id;
    uint32_t lease_expires_at_ms;
    uint32_t last_nonce;
} ucn_cluster_member_t;

/* Owner-context read-only snapshots for optional Cluster extensions.  They
 * avoid making an extension depend on the mutable ucn_cluster_t layout. */
typedef struct ucn_cluster_view {
    bool enabled;
    ucn_cluster_role_t role;
    ucn_node_id_t local_node_id;
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
    uint16_t current_head_score;
} ucn_cluster_view_t;

typedef struct ucn_cluster_member_summary {
    ucn_node_id_t node_id;
    uint32_t lease_expires_at_ms;
} ucn_cluster_member_summary_t;

typedef struct ucn_cluster_stats {
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
} ucn_cluster_stats_t;

typedef struct ucn_cluster {
    ucn_cluster_config_t config;
    ucn_cluster_role_t role;
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
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
    ucn_cluster_member_t members[UCN_CLUSTER_MAX_MEMBERS];
    /* C07.2 Backup state.  Head and Backup reuse members[] as the synced
     * membership mirror; only the assigned Backup keeps a live copy. */
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
    uint32_t member_voted_term;
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
    ucn_node_id_t known_recovery_source;
    uint8_t recovery_ack_count;
    uint32_t recovery_acked;
    ucn_cluster_stats_t stats;
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
const ucn_cluster_stats_t *ucn_cluster_get_stats(const ucn_cluster_t *cluster);

#ifdef __cplusplus
}
#endif

#endif
