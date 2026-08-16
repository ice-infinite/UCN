#include "ucn/ucn_cluster.h"

#include <assert.h>
#include <string.h>

#include "ucn/ucn_time.h"

static void assign_backup(ucn_cluster_t *cluster, uint32_t now_ms);
static void backup_resync(ucn_cluster_t *cluster);
static ucn_result_t send_join_reply(ucn_cluster_t *cluster,
                                    ucn_node_id_t destination,
                                    ucn_cluster_message_type_t type,
                                    uint32_t join_nonce);
static void backup_clear_sync(ucn_cluster_t *cluster, uint32_t now_ms);
static void start_backup_assignment_cycle(ucn_cluster_t *cluster,
                                          uint32_t now_ms);
static void queue_backup_assignment_for_member(ucn_cluster_t *cluster,
                                               ucn_node_id_t member_node_id,
                                               uint32_t now_ms);
static ucn_result_t send_cluster_message(ucn_cluster_t *cluster,
                                         ucn_node_id_t destination,
                                         const ucn_cluster_message_t *message);
static ucn_result_t send_backup_assign(ucn_cluster_t *cluster,
                                       ucn_node_id_t destination);
static ucn_result_t send_backup_resync_req(ucn_cluster_t *cluster);
static ucn_result_t send_backup_reject(ucn_cluster_t *cluster,
                                       uint8_t reason);

#define CLUSTER_VERSION_OFFSET ((size_t)0U)
#define CLUSTER_TYPE_OFFSET ((size_t)1U)
#define CLUSTER_ROLE_OFFSET ((size_t)2U)
#define CLUSTER_FLAGS_OFFSET ((size_t)3U)
#define CLUSTER_ID_OFFSET ((size_t)4U)
#define CLUSTER_TERM_OFFSET ((size_t)8U)
#define CLUSTER_HEAD_OFFSET ((size_t)12U)
#define CLUSTER_SCORE_OFFSET ((size_t)16U)
#define CLUSTER_CAPACITY_OFFSET ((size_t)18U)
#define CLUSTER_LEASE_OFFSET ((size_t)20U)
#define CLUSTER_NONCE_OFFSET ((size_t)24U)

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static bool role_is_valid(ucn_cluster_role_t role)
{
    return role >= UCN_CLUSTER_ROLE_DISABLED &&
           role <= UCN_CLUSTER_ROLE_RECOVERY_HEAD;
}

static bool message_type_is_valid(ucn_cluster_message_type_t type)
{
    return type >= UCN_CLUSTER_MSG_ADVERTISE &&
           type <= UCN_CLUSTER_MSG_BACKUP_REJECT;
}

static bool node_id_field_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool flags_are_valid(const ucn_cluster_message_t *message)
{
    /* C07.7 P1: strict whitelist.  Only the single marker values are legal
     * for Type 12; any combination (BEGIN|END, BEGIN|DELTA, END|DELTA)
     * is malformed so the handler can never mis-classify a frame. */
    if (message->type == UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC) {
        switch (message->flags) {
        case 0U:
        case UCN_CLUSTER_FLAG_SYNC_BEGIN:
        case UCN_CLUSTER_FLAG_SYNC_END:
        case UCN_CLUSTER_FLAG_SYNC_DELTA:
            return true;
        default:
            return false;
        }
    }
    if ((message->flags & (uint8_t)~UCN_CLUSTER_KNOWN_FLAGS) != 0U) {
        return false;
    }
    return message->flags == 0U;
}

static bool message_is_valid(const ucn_cluster_message_t *message)
{
    if (message == NULL || !message_type_is_valid(message->type) ||
        !role_is_valid(message->role) || !flags_are_valid(message) ||
        message->cluster_id == 0U || message->term == 0U ||
        !node_id_field_is_valid(message->head_node_id)) {
        return false;
    }
    switch (message->type) {
    case UCN_CLUSTER_MSG_ADVERTISE:
    case UCN_CLUSTER_MSG_JOIN_REQUEST:
    case UCN_CLUSTER_MSG_JOIN_ACCEPT:
    case UCN_CLUSTER_MSG_JOIN_REJECT:
    case UCN_CLUSTER_MSG_KEEPALIVE:
    case UCN_CLUSTER_MSG_LEAVE:
    case UCN_CLUSTER_MSG_HEAD_DECLARE:
    case UCN_CLUSTER_MSG_HEAD_STEPDOWN:
        return message->head_score <= UCN_CLUSTER_SCORE_MAX &&
               ucn_duration_is_valid(message->lease_ms) &&
               message->nonce != 0U;
    case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
        return message->backup_generation != 0U &&
               message->head_score <= UCN_CLUSTER_SCORE_MAX &&
               ucn_duration_is_valid(message->lease_ms);
    case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
        return message->backup_generation != 0U &&
               message->role == UCN_CLUSTER_ROLE_HEAD;
    case UCN_CLUSTER_MSG_BACKUP_READY:
        return message->role == UCN_CLUSTER_ROLE_BACKUP;
    case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
        return message->role == UCN_CLUSTER_ROLE_HEAD;
    case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
        /* C07.7 P1: Type 12 must come from the Head and must carry the
         * exact Backup generation.  Marker frames carry no member record
         * (member_node_id must be zero); data frames require a valid
         * member id and full 32-bit nonce.  The member lease is
         * implicit (config.lease_ms). */
        if (message->role != UCN_CLUSTER_ROLE_HEAD ||
            message->backup_generation == 0U) {
            return false;
        }
        if ((message->flags & (UCN_CLUSTER_FLAG_SYNC_BEGIN |
                               UCN_CLUSTER_FLAG_SYNC_END)) != 0U) {
            return message->member_node_id == 0U;
        }
        return node_id_field_is_valid(message->member_node_id) &&
               message->member_nonce != 0U;
    case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
        return message->backup_generation != 0U &&
               message->role == UCN_CLUSTER_ROLE_BACKUP;
    case UCN_CLUSTER_MSG_TAKEOVER_ACK:
        return message->backup_generation != 0U &&
               message->role == UCN_CLUSTER_ROLE_MEMBER;
    case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
    case UCN_CLUSTER_MSG_BACKUP_REJECT:
        return message->backup_generation != 0U &&
               message->role == UCN_CLUSTER_ROLE_BACKUP;
    case UCN_CLUSTER_MSG_RECOVERY_DECLARE:
        return message->recovery_nonce != 0U &&
               ucn_duration_is_valid(message->recovery_ttl_ms);
    case UCN_CLUSTER_MSG_RECOVERY_ACK:
        return true;
    default:
        return false;
    }
}

/* Encode the trailing 12 B (offset 16..27) according to Message Type. */
static void encode_trailing_12b(const ucn_cluster_message_t *message,
                                uint8_t *output)
{
    switch (message->type) {
    case UCN_CLUSTER_MSG_ADVERTISE:
    case UCN_CLUSTER_MSG_JOIN_REQUEST:
    case UCN_CLUSTER_MSG_JOIN_ACCEPT:
    case UCN_CLUSTER_MSG_JOIN_REJECT:
    case UCN_CLUSTER_MSG_KEEPALIVE:
    case UCN_CLUSTER_MSG_LEAVE:
    case UCN_CLUSTER_MSG_HEAD_DECLARE:
    case UCN_CLUSTER_MSG_HEAD_STEPDOWN:
        write_u16_be(output + 0U, message->head_score);
        write_u16_be(output + 2U, message->available_capacity);
        write_u32_be(output + 4U, message->lease_ms);
        write_u32_be(output + 8U, message->nonce);
        break;
    case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
        write_u32_be(output + 0U, message->backup_generation);
        write_u16_be(output + 4U, message->head_score);
        write_u16_be(output + 6U, message->available_capacity);
        write_u32_be(output + 8U, message->lease_ms);
        break;
    case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
        write_u32_be(output + 0U, message->backup_generation);
        write_u32_be(output + 4U, message->sync_token);
        write_u32_be(output + 8U, 0U);
        break;
    case UCN_CLUSTER_MSG_BACKUP_READY:
    case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
        /* C07.7 P1: generation + membership_sequence bind these messages
         * to the exact Backup epoch (replay fencing). */
        write_u32_be(output + 0U, message->backup_generation);
        write_u32_be(output + 4U, message->membership_sequence);
        write_u32_be(output + 8U, 0U);
        break;
    case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
        /* C07.7 P1: 16 B trailing (frame is 32 B): the Backup generation
         * binds every snapshot/delta frame to the exact BackupEpoch, then
         * member id + 32-bit sequence + 32-bit nonce.  The member lease
         * is implicit (config.lease_ms). */
        write_u32_be(output + 0U, message->backup_generation);
        write_u32_be(output + 4U, message->member_node_id);
        write_u32_be(output + 8U, message->membership_sequence);
        write_u32_be(output + 12U, message->member_nonce);
        break;
    case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
    case UCN_CLUSTER_MSG_BACKUP_REJECT:
        /* generation + sequence (resync) or reason (reject) bind the
         * request to the exact BackupEpoch. */
        write_u32_be(output + 0U, message->backup_generation);
        write_u32_be(output + 4U, message->membership_sequence);
        output[8U] = message->reject_reason;
        break;
    case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
    case UCN_CLUSTER_MSG_TAKEOVER_ACK:
        write_u32_be(output + 0U, message->backup_generation);
        write_u32_be(output + 4U, 0U);
        write_u32_be(output + 8U, 0U);
        break;
    case UCN_CLUSTER_MSG_RECOVERY_DECLARE:
        write_u32_be(output + 0U, message->recovery_nonce);
        write_u32_be(output + 4U, message->recovery_ttl_ms);
        write_u32_be(output + 8U, 0U);
        break;
    case UCN_CLUSTER_MSG_RECOVERY_ACK:
        write_u32_be(output + 0U, 0U);
        write_u32_be(output + 4U, 0U);
        write_u32_be(output + 8U, 0U);
        break;
    default:
        break;
    }
}

ucn_result_t ucn_cluster_message_encode(
    const ucn_cluster_message_t *message,
    uint8_t output[UCN_CLUSTER_MESSAGE_BYTES])
{
    if (!message_is_valid(message) || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(output, 0, UCN_CLUSTER_MESSAGE_BYTES);
    output[CLUSTER_VERSION_OFFSET] = UCN_CLUSTER_FORMAT_VERSION;
    output[CLUSTER_TYPE_OFFSET] = (uint8_t)message->type;
    output[CLUSTER_ROLE_OFFSET] = (uint8_t)message->role;
    output[CLUSTER_FLAGS_OFFSET] = message->flags;
    write_u32_be(output + CLUSTER_ID_OFFSET, message->cluster_id);
    write_u32_be(output + CLUSTER_TERM_OFFSET, message->term);
    write_u32_be(output + CLUSTER_HEAD_OFFSET, message->head_node_id);
    encode_trailing_12b(message, output + CLUSTER_SCORE_OFFSET);
    return UCN_OK;
}

/* Decode the trailing 12 B (offset 16..27) according to Message Type.  The
 * full struct was zeroed first, so unused fields stay zero. */
static void decode_trailing_12b(ucn_cluster_message_t *message,
                                const uint8_t *input)
{
    switch (message->type) {
    case UCN_CLUSTER_MSG_ADVERTISE:
    case UCN_CLUSTER_MSG_JOIN_REQUEST:
    case UCN_CLUSTER_MSG_JOIN_ACCEPT:
    case UCN_CLUSTER_MSG_JOIN_REJECT:
    case UCN_CLUSTER_MSG_KEEPALIVE:
    case UCN_CLUSTER_MSG_LEAVE:
    case UCN_CLUSTER_MSG_HEAD_DECLARE:
    case UCN_CLUSTER_MSG_HEAD_STEPDOWN:
        message->head_score = read_u16_be(input + 0U);
        message->available_capacity = read_u16_be(input + 2U);
        message->lease_ms = read_u32_be(input + 4U);
        message->nonce = read_u32_be(input + 8U);
        break;
    case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
        message->backup_generation = read_u32_be(input + 0U);
        message->head_score = read_u16_be(input + 4U);
        message->available_capacity = read_u16_be(input + 6U);
        message->lease_ms = read_u32_be(input + 8U);
        break;
    case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
        message->backup_generation = read_u32_be(input + 0U);
        message->sync_token = read_u32_be(input + 4U);
        break;
    case UCN_CLUSTER_MSG_BACKUP_READY:
    case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
        message->backup_generation = read_u32_be(input + 0U);
        message->membership_sequence = read_u32_be(input + 4U);
        break;
    case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
        message->backup_generation = read_u32_be(input + 0U);
        message->member_node_id = read_u32_be(input + 4U);
        message->membership_sequence = read_u32_be(input + 8U);
        message->member_nonce = read_u32_be(input + 12U);
        break;
    case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
    case UCN_CLUSTER_MSG_BACKUP_REJECT:
        message->backup_generation = read_u32_be(input + 0U);
        message->membership_sequence = read_u32_be(input + 4U);
        message->reject_reason = input[8U];
        break;
    case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
    case UCN_CLUSTER_MSG_TAKEOVER_ACK:
        message->backup_generation = read_u32_be(input + 0U);
        break;
    case UCN_CLUSTER_MSG_RECOVERY_DECLARE:
        message->recovery_nonce = read_u32_be(input + 0U);
        message->recovery_ttl_ms = read_u32_be(input + 4U);
        break;
    case UCN_CLUSTER_MSG_RECOVERY_ACK:
        break;
    default:
        break;
    }
}

ucn_result_t ucn_cluster_message_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_message_t *message)
{
    if (input == NULL || message == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length != UCN_CLUSTER_MESSAGE_BYTES ||
        input[CLUSTER_VERSION_OFFSET] != UCN_CLUSTER_FORMAT_VERSION) {
        return UCN_ERR_MALFORMED;
    }
    (void)memset(message, 0, sizeof(*message));
    message->type = (ucn_cluster_message_type_t)input[CLUSTER_TYPE_OFFSET];
    message->role = (ucn_cluster_role_t)input[CLUSTER_ROLE_OFFSET];
    message->flags = input[CLUSTER_FLAGS_OFFSET];
    message->cluster_id = read_u32_be(input + CLUSTER_ID_OFFSET);
    message->term = read_u32_be(input + CLUSTER_TERM_OFFSET);
    message->head_node_id = read_u32_be(input + CLUSTER_HEAD_OFFSET);
    decode_trailing_12b(message, input + CLUSTER_SCORE_OFFSET);
    return message_is_valid(message) ? UCN_OK : UCN_ERR_MALFORMED;
}

static uint32_t cluster_now(const ucn_cluster_t *cluster)
{
    return cluster->config.now_ms(cluster->config.now_context);
}

/* ================= CLV2-01-01..03: M01 shadow phase ===================
 *
 * During M01 the legacy role+bool+deadline fields still drive the FSM.
 * cluster_phase_from_legacy_state() derives the explicit phase name for
 * every implicit combination, and cluster_shadow_sync() keeps the
 * shadow mirror aligned after each Step/RX.  Production logic MUST NOT
 * read shadow_phase to make decisions until CLV2-01-04+; the mirror only
 * exists so tests can prove the mapping is total, unique and consistent
 * under the fault model. */

static ucn_cluster_phase_t cluster_phase_from_legacy_state(
    const ucn_cluster_t *cluster, uint32_t now_ms)
{
    if (!cluster->config.enabled) {
        return UCN_CLUSTER_PHASE_DISABLED;
    }
    switch (cluster->role) {
    case UCN_CLUSTER_ROLE_DISABLED:
        return UCN_CLUSTER_PHASE_DISABLED;
    case UCN_CLUSTER_ROLE_DETACHED:
        if (cluster->recovery_eligible) {
            /* Cooling down after a Recovery stepdown still observes;
             * once the backoff timer is armed the node is walking the
             * recovery election path. */
            if (cluster->recovery_backoff_deadline_ms != 0U &&
                (cluster->recovery_cooldown_until_ms == 0U ||
                 ucn_deadline_expired(now_ms,
                                      cluster->recovery_cooldown_until_ms))) {
                return UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
            }
            return UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
        }
        return UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    case UCN_CLUSTER_ROLE_CANDIDATE:
        return UCN_CLUSTER_PHASE_ELECTION;
    case UCN_CLUSTER_ROLE_JOIN_PENDING:
        return UCN_CLUSTER_PHASE_JOIN_PENDING;
    case UCN_CLUSTER_ROLE_MEMBER:
        /* CLV2-M01.0.1: arming the grace deadline IS the phase change.
         * Timer expiry is an event the FSM owner consumes (timeout
         * action -> DETACHED/RECOVERY); it must never silently derive
         * the phase back to MEMBER_ACTIVE. */
        if (cluster->head_grace_deadline_ms != 0U) {
            return UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE;
        }
        return UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    case UCN_CLUSTER_ROLE_HEAD:
        /* CLV2-M01.0.1: the Head-side phase ladder follows the REAL
         * assignment/snapshot fields: no Backup yet -> NO_BACKUP;
         * assignment cycle armed -> ASSIGNING; assignment done but no
         * READY yet -> SYNCING (snapshot in flight); READY -> STABLE.
         * The mirror flag backup_syncing is Backup-side state and must
         * never drive the Head phase. */
        if (cluster->backup_node_id == 0U) {
            return UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
        }
        if (cluster->backup_ready) {
            return UCN_CLUSTER_PHASE_HEAD_STABLE;
        }
        if (cluster->backup_assign_pending) {
            return UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
        }
        return UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    case UCN_CLUSTER_ROLE_BACKUP:
        if (cluster->backup_takeover_active) {
            return UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
        }
        if (cluster->backup_ready) {
            return UCN_CLUSTER_PHASE_BACKUP_READY;
        }
        return UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    case UCN_CLUSTER_ROLE_STEPPING_DOWN:
        return UCN_CLUSTER_PHASE_STEPPING_DOWN;
    case UCN_CLUSTER_ROLE_RECOVERY_HEAD:
        return UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    default:
        return UCN_CLUSTER_PHASE_DISABLED;
    }
}

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
static ucn_cluster_transition_reason_t cluster_rx_reason_from_type(
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

/* CLV2-M01.0.1: contradictory legacy combinations the current FSM must
 * never produce.  The shadow gate refuses to mint a transition from an
 * invalid combination instead of silently naming it. */
static bool cluster_legacy_state_is_valid(const ucn_cluster_t *cluster)
{
    if (cluster->role == UCN_CLUSTER_ROLE_HEAD) {
        /* READY without a selected Backup is contradictory. */
        if (cluster->backup_ready && cluster->backup_node_id == 0U) {
            return false;
        }
        /* backup_syncing is Backup-side mirror state; a Head must never
         * carry it. */
        if (cluster->backup_syncing) {
            return false;
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
        /* The mirror is either syncing or ready, never both. */
        if (cluster->backup_ready && cluster->backup_syncing) {
            return false;
        }
        /* NOTE (M01.0.2): takeover_active && backup_syncing is REACHABLE
         * in the Current FSM: a delayed same-generation Type12 from the
         * old Primary (e.g. SYNC_BEGIN) can re-arm backup_syncing while
         * takeover is already active, because handle_backup_member_sync()
         * has no takeover guard.  Shadow must express it, not reject it;
         * the late-sync-can-mutate-mirror deficiency is deferred to the
         * M09 committed/staging mirror + M10 frozen TakeoverConfig. */
    }
    return true;
}

static void cluster_shadow_sync(ucn_cluster_t *cluster,
                                ucn_cluster_transition_reason_t hint)
{
    uint32_t now_ms = cluster_now(cluster);
    ucn_cluster_phase_t derived;
    ucn_cluster_transition_reason_t reason;

    if (!cluster_legacy_state_is_valid(cluster)) {
        /* Fail closed: never mint a shadow transition from a
         * contradictory legacy combination. */
        return;
    }
    derived = cluster_phase_from_legacy_state(cluster, now_ms);
    if (derived == cluster->shadow_phase) {
        return;
    }
    reason = cluster_reason_from_diff(cluster->shadow_phase, derived);
    if (reason == UCN_CLUSTER_REASON_UNKNOWN) {
        reason = hint;
    }
    cluster->shadow_phase = derived;
    cluster->transition_reason = reason;
    cluster->shadow_transition_count++;
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
 *   HEAD_STABLE -> ELECTION   : role CANDIDATE is written only by
 *       backup_challenge (BACKUP-only) and start_election (reached only
 *       from DETACHED with recovery_eligible == false); no HEAD path.
 *   HEAD_NO_BACKUP / HEAD_BACKUP_ASSIGNING / HEAD_BACKUP_SYNCING /
 *   HEAD_STABLE -> DETACHED_OBSERVE : set_detached() is never called
 *       from a HEAD-role site.
 *   RECOVERY_OBSERVE -> ELECTION : a recovery-eligible node never elects.
 *   RECOVERY_OBSERVE -> DETACHED_OBSERVE, RECOVERY_ELECTION ->
 *   DETACHED_OBSERVE  : no site clears recovery_eligible while keeping
 *       role DETACHED.
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

        /* observe timeout (head_capable): start_election() L3694 (transition L3703) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_ELECTION) |
        /* stable Head offer: cluster_transition() L2414 via consider_head_offer() L2269 DETACHED (!recovery_eligible); RECOVERY_* keeps begin_join() L1972 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* joins a recovery Head: handle_recovery_declare() L3311 (role=MEMBER L3377) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_ELECTION] =

        /* win: complete_election() L3735 (win dispatch L3782); the HEAD

         * sub-phase is dispatched from the pre-call backup_* state. */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING) |
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING) |
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_STABLE) |
        /* stable Head offer: cluster_transition() L2414 via consider_head_offer() L2269 CANDIDATE (!recovery_eligible) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |

        /* loss: complete_election() L3735 -> set_detached() L1915 (role=DETACHED L1920) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE),

    [UCN_CLUSTER_PHASE_JOIN_PENDING] =
        /* exact JOIN_ACCEPT: handle_join_accept() L2102 (transition L2136);
         * apply_legacy writes role + grace=0 (CLV2-01-04b.4) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |

        /* exact JOIN_REJECT (receive_inner L3440, transition L3529) /
         * HEAD_STEPDOWN -> set_detached() L1915 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* BACKUP_ASSIGN(self) arrives first: handle_backup_assign() L2585 (role=BACKUP L2621) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_SYNCING),

    [UCN_CLUSTER_PHASE_MEMBER_ACTIVE] =

        /* Head lease expired: ucn_cluster_step_inner() L4231 (grace armed L4269) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE) |
        /* BACKUP_ASSIGN(self): handle_backup_assign() L2585 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_SYNCING) |
        /* stepdown / reset: HEAD_STEPDOWN -> set_detached() L1915 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* better Head switch: cluster_transition() L2480 via consider_head_offer() L2269 MEMBER (!grace) + begin_join_prepare_fields() L1956 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING),

    [UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE] =
        /* Head lease renewed: cluster_transition() L2292 via consider_head_offer() L2269
         * refresh (grace=0 site write) / handle_head_takeover() L3129 (grace=0 L3164) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |
        /* HEAD_STEPDOWN -> set_detached() L1915 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |

        /* grace timeout: step L4279 (transition L4291) + set_detached() L1915 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) |
        /* better Head switch: cluster_transition() L2469 via consider_head_offer() L2269 GRACE + begin_join_prepare_fields() L1956 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* BACKUP_ASSIGN(self): handle_backup_assign() L2585 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_SYNCING),

    [UCN_CLUSTER_PHASE_HEAD_NO_BACKUP] =

        /* Backup selected: assign_backup() L2535 (node_id L2572) +
         * start_backup_assignment_cycle() L3837 (assign_pending L3874) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING) |
        /* ordered stepdown: begin_ordered_stepdown() L2219 (role=STEPPING_DOWN L2227) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING] =

        /* assignment sweep done: send_backup_assignment_step() L3881 (assign_pending=false L3908) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING) |
        /* READY during sweep: handle_backup_ready() L2660 (ready=true L2679) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_STABLE) |
        /* Backup lost: remove_member() L2019 (node_id=0 L2027) / expire_members() L4167 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |
        /* ordered stepdown: begin_ordered_stepdown() L2219 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING] =
        /* snapshot READY: handle_backup_ready() L2660 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_STABLE) |

        /* periodic re-assign: start_backup_assignment_cycle() L3837 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING) |
        /* Backup lost: remove_member() L2019 / expire_members() L4167 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |
        /* ordered stepdown: begin_ordered_stepdown() L2219 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_HEAD_STABLE] =
        /* Backup lost: remove_member() L2019 / expire_members() L4167 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |

        /* resync: backup_resync() L4114 (ready=false L4121) */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING) |
        /* ordered stepdown: begin_ordered_stepdown() L2219 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),

    [UCN_CLUSTER_PHASE_BACKUP_SYNCING] =
        /* snapshot READY: handle_backup_member_sync() L2683 SYNC_END (syncing=false L2759, ready=true L2760) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_READY) |
        /* Primary lost / stepdown: HEAD_STEPDOWN -> backup_clear_sync() L2503 -> set_detached() L1915 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |

        /* Primary lost (eligible): step L4362 (eligible=true L4364) + backup_clear_sync() L2503 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) |
        /* score challenge: backup_challenge() L2241 (role=CANDIDATE L2253) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_ELECTION) |
        /* newer-Term Head: consider_head_offer() L2194 (takeover=false L2314) + backup_clear_sync() L2503 + begin_join() L1967 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* HEAD_TAKEOVER / recovery: handle_head_takeover() L3129 (role=MEMBER L3157) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_BACKUP_READY] =
        /* Primary lease lapsed: start_takeover() L2942 (takeover=true L2947) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_TAKEOVER) |
        /* DELTA gap / resync: handle_backup_member_sync() L2683 (DELTA gap L2712-2713, SYNC_BEGIN L2734-2735) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_BACKUP_SYNCING) |
        /* score challenge: backup_challenge() L2241 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_ELECTION) |
        /* newer-Term Head: consider_head_offer() L2194 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* Primary lost / stepdown: HEAD_STEPDOWN -> backup_clear_sync() L2503 -> set_detached() L1915 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* HEAD_TAKEOVER / recovery: handle_head_takeover() L3129 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_BACKUP_TAKEOVER] =
        /* majority reached: complete_takeover() L2899 (role=HEAD L2903, node_id=0 L2913) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP) |

        /* timeout / stepdown: step L4374 -> backup_clear_sync() L2503 -> set_detached() L1915 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE) |
        /* newer-Term Head: consider_head_offer() L2194 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* HEAD_TAKEOVER / recovery: handle_head_takeover() L3129 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |
        /* score challenge: backup_challenge() L2241 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_ELECTION),

    [UCN_CLUSTER_PHASE_STEPPING_DOWN] =

        /* stepdown deadline: ucn_cluster_step_inner() L4231 (role=JOIN_PENDING L4437) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING),

    [UCN_CLUSTER_PHASE_RECOVERY_OBSERVE] =
        /* backoff armed: start_recovery_backoff() L3265 (deadline L3268) via step L4392 */

        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_ELECTION) |
        /* Head offer: begin_join() L1967 via consider_head_offer() L2194 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* recovery Head join: handle_recovery_declare() L3311 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_RECOVERY_ELECTION] =
        /* quorum, declare: declare_recovery_head() L3272 (role=RECOVERY_HEAD L3277) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_HEAD) |
        /* Head offer: begin_join() L1967 via consider_head_offer() L2194 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_JOIN_PENDING) |
        /* recovery Head join: handle_recovery_declare() L3311 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE),

    [UCN_CLUSTER_PHASE_RECOVERY_HEAD] =
        /* TTL expired (cooldown): stepdown_recovery_head() L3295 -> set_detached() L1915 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_RECOVERY_OBSERVE) |
        /* lost arbitration / HEAD_TAKEOVER: handle_recovery_declare() L3311 /
         * handle_head_takeover() L3129 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |
        /* stable Head reclaims: begin_ordered_stepdown() L2219 */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_STEPPING_DOWN),
};

/* OBSERVED table: DIRECT union the tick-granularity COMPOUND pairs the
 * T-A collector observes.  NOT callable - wiring must realize them via
 * their DIRECT constituent edges in sequence. */
static const uint32_t CLUSTER_TRANSITION_OBSERVED_ALLOWED[UCN_CLUSTER_PHASE_COUNT]
    CLV2_01_04_UNUSED = {
    [UCN_CLUSTER_PHASE_DETACHED_OBSERVE] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_DETACHED_OBSERVE],
    [UCN_CLUSTER_PHASE_ELECTION] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_ELECTION],
    [UCN_CLUSTER_PHASE_JOIN_PENDING] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_JOIN_PENDING],
    [UCN_CLUSTER_PHASE_MEMBER_ACTIVE] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_MEMBER_ACTIVE],
    [UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE],
    [UCN_CLUSTER_PHASE_HEAD_NO_BACKUP] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_HEAD_NO_BACKUP],
    [UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING],
    [UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING],
    [UCN_CLUSTER_PHASE_HEAD_STABLE] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_HEAD_STABLE] |
        /* compound: Backup lost + immediate re-assign in one tick (T-A) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING),
    [UCN_CLUSTER_PHASE_BACKUP_SYNCING] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_BACKUP_SYNCING],
    [UCN_CLUSTER_PHASE_BACKUP_READY] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_BACKUP_READY] |
        /* compound: takeover started + completed in one tick (T-A, golden t=179) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_HEAD_NO_BACKUP),
    [UCN_CLUSTER_PHASE_BACKUP_TAKEOVER] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_BACKUP_TAKEOVER],
    [UCN_CLUSTER_PHASE_STEPPING_DOWN] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_STEPPING_DOWN] |
        /* compounds: deadline -> JOIN_PENDING -> JOIN_ACCEPT / JOIN_REJECT in one tick (T-A) */
        (UINT32_C(1) << UCN_CLUSTER_PHASE_MEMBER_ACTIVE) |
        (UINT32_C(1) << UCN_CLUSTER_PHASE_DETACHED_OBSERVE),
    [UCN_CLUSTER_PHASE_RECOVERY_OBSERVE] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_RECOVERY_OBSERVE],
    [UCN_CLUSTER_PHASE_RECOVERY_ELECTION] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_RECOVERY_ELECTION],
    [UCN_CLUSTER_PHASE_RECOVERY_HEAD] =
        CLUSTER_TRANSITION_DIRECT_ALLOWED[UCN_CLUSTER_PHASE_RECOVERY_HEAD],
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
 * BEFORE begin_join, at the site), and backup_challenge() clears
 * ready/syncing/deadlines/takeover but NOT members[] or backup_generation
 * - so the special exits stay at the SITES during 01-04b..f wiring.  Only
 * after every inbound site of a phase is migrated may a common cleanup be
 * re-merged into a single entry action here.  Fields a site writes AFTER
 * the call are fine: the end-of-step/RX shadow sync re-aligns.
 *
 * Entering a HEAD_BACKUP_* / HEAD_STABLE sub-phase requires the caller to
 * have the matching backup_* state (assign_pending / ready / node_id), as
 * every real site does; RECOVERY_ELECTION requires a caller-provided
 * armed backoff deadline (CLV2-01-04a.1 Item 4 - never auto-minted). */
static void cluster_transition_apply_legacy(ucn_cluster_t *cluster,
                                            ucn_cluster_phase_t new_phase,
                                            uint32_t now_ms)
    CLV2_01_04_UNUSED;

static void cluster_transition_apply_legacy(ucn_cluster_t *cluster,
                                            ucn_cluster_phase_t new_phase,
                                            uint32_t now_ms)
{
    switch (new_phase) {
    case UCN_CLUSTER_PHASE_DISABLED:
        /* init-only; no runtime site toggles enabled. */
        cluster->role = UCN_CLUSTER_ROLE_DISABLED;
        break;
    case UCN_CLUSTER_PHASE_DETACHED_OBSERVE:
        /* Every inbound edge goes through set_detached() L1915
         * (role=DETACHED L1920, known_backup cleared L1929, grace L1937);
         * recovery_eligible is false on every inbound edge (the only
         * eligible=true writers produce RECOVERY_OBSERVE) and no inbound
         * edge arms backoff. */
        cluster->role = UCN_CLUSTER_ROLE_DETACHED;
        cluster->recovery_eligible = false;
        cluster->recovery_backoff_deadline_ms = 0U;
        cluster->head_grace_deadline_ms = 0U;
        cluster->known_backup_node_id = 0U;
        cluster->known_backup_generation = 0U;
        break;
    case UCN_CLUSTER_PHASE_ELECTION:

        /* role only: start_election() L3694 / backup_challenge() L2241

         * write role=CANDIDATE; their mirror clears are site-owned and
         * must NOT be replayed here (members[]/backup_generation survive
         * a challenge, exactly as the real site leaves them). */
        cluster->role = UCN_CLUSTER_ROLE_CANDIDATE;
        break;
    case UCN_CLUSTER_PHASE_JOIN_PENDING:
        /* CLV2-01-04b.3: DETACHED/ELECTION (!recovery_eligible) and
         * MEMBER/GRACE (01-04c.4) sources transition via cluster_transition()
         * at consider_head_offer() L2414/L2469/L2480 (apply_legacy writes
         * role + eligible=false + backoff=0); RECOVERY / BACKUP newer-Term /
         * JOIN_PENDING re-target sources keep begin_join() L1972 (role L1980;
         * candidacy abandon lives in the shared field helper
         * begin_join_prepare_fields() L1956).  begin_join() does NOT clear
         * the mirror/known_backup (only the BACKUP higher-Term path does
         * backup_clear_sync() BEFORE the join, at the site). */
        cluster->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
        cluster->recovery_eligible = false;
        cluster->recovery_backoff_deadline_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_MEMBER_ACTIVE:
        /* handle_join_accept() L2102 (transition L2136, grace=0 via
         * apply_legacy) and handle_head_takeover() L3129 (role L3157,
         * grace=0 L3164) both write role+grace; recovery_eligible is
         * false on every inbound edge.  known_backup_* are NOT cleared
         * by handle_join_accept() (retained-state Test A). */
        cluster->role = UCN_CLUSTER_ROLE_MEMBER;
        cluster->head_grace_deadline_ms = 0U;
        cluster->recovery_eligible = false;
        break;
    case UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE:

        /* Sole inbound edge: ucn_cluster_step_inner() L4231 arms the
         * grace deadline L4269. */

        cluster->role = UCN_CLUSTER_ROLE_MEMBER;
        if (cluster->head_grace_deadline_ms == 0U) {
            cluster->head_grace_deadline_ms = ucn_deadline_from_now(
                now_ms, cluster->config.keepalive_interval_ms);
        }
        break;
    case UCN_CLUSTER_PHASE_HEAD_NO_BACKUP:
        /* Entering HEAD_NO_BACKUP canonicalizes node_id=0 / ready=false
         * as a DESTINATION invariant: HEAD_NO_BACKUP is DEFINED by
         * backup_node_id == 0, so the hook normalizes it regardless of
         * which inbound site ran (remove_member() L2019 / expire_members()
         * L4167 / complete_takeover() L2899 / handle_backup_reject()).
         * Note this is NOT true of every inbound edge's own writes: the

         * ELECTION inbound (complete_election() L3735) LEAVES the

         * candidate's preserved backup_* state, which decides the actual
         * destination sub-phase - the 01-04b complete_election wiring must
         * dispatch on that caller state (NO_BACKUP only when node_id==0,
         * otherwise ASSIGNING/SYNCING/STABLE).  syncing/takeover/primary/
         * known_backup are NOT cleared here - site-owned. */
        cluster->role = UCN_CLUSTER_ROLE_HEAD;
        cluster->backup_node_id = 0U;
        cluster->backup_ready = false;
        break;
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING:

        /* role only: assign_backup() L2535 writes node_id/ready and
         * start_backup_assignment_cycle() L3837 sets assign_pending at the
         * SITE; complete_election() L3735 leaves the caller's backup state

         * (which decides the sub-phase). */
        cluster->role = UCN_CLUSTER_ROLE_HEAD;
        break;
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
        /* role only: caller-provided node_id/assign_pending/ready state

         * decides the sub-phase (complete_election L3735 / backup_resync
         * L4114 / assignment sweep L3908). */

        cluster->role = UCN_CLUSTER_ROLE_HEAD;
        break;
    case UCN_CLUSTER_PHASE_HEAD_STABLE:
        /* role only: the caller provides ready=true (handle_backup_ready
         * L2679 / complete_election caller state). */
        cluster->role = UCN_CLUSTER_ROLE_HEAD;
        break;
    case UCN_CLUSTER_PHASE_BACKUP_SYNCING:
        /* handle_backup_assign() L2585 (role=BACKUP L2621, syncing=true
         * L2628, ready=false L2629) and handle_backup_member_sync() L2683
         * BEGIN/gap (L2734-2735, L2747-2748) both write role+syncing+ready;
         * takeover is never set on an inbound edge of this phase. */
        cluster->role = UCN_CLUSTER_ROLE_BACKUP;
        cluster->backup_ready = false;
        cluster->backup_syncing = true;
        break;
    case UCN_CLUSTER_PHASE_BACKUP_READY:
        /* handle_backup_member_sync() L2683 SYNC_END: role=BACKUP,
         * syncing=false L2759, ready=true L2760. */
        cluster->role = UCN_CLUSTER_ROLE_BACKUP;
        cluster->backup_ready = true;
        cluster->backup_syncing = false;
        break;
    case UCN_CLUSTER_PHASE_BACKUP_TAKEOVER:
        /* start_takeover() L2942: role=BACKUP, takeover=true L2947;
         * ready/syncing are NOT cleared (CLV2-M01.0.2: the takeover_active
         * && syncing combo is reachable and must be expressed). */
        cluster->role = UCN_CLUSTER_ROLE_BACKUP;
        cluster->backup_takeover_active = true;
        break;
    case UCN_CLUSTER_PHASE_STEPPING_DOWN:
        /* begin_ordered_stepdown() L2219: role=STEPPING_DOWN L2227, yields
         * Recovery candidacy L2224-2225; the Head keeps its Backup
         * selection (node_id/ready) until the deadline. */
        cluster->role = UCN_CLUSTER_ROLE_STEPPING_DOWN;
        cluster->recovery_eligible = false;
        cluster->recovery_backoff_deadline_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_RECOVERY_OBSERVE:

        /* Every inbound edge (grace timeout L4279 + set_detached() L1915;
         * backup missed-heartbeat L4362 + backup_clear_sync() L2503;
         * RECOVERY_HEAD TTL stepdown_recovery_head L3295 -> set_detached()

         * L1915) results in role=DETACHED, eligible=true, backoff=0, and
         * set_detached() clears grace + known_backup. */
        cluster->role = UCN_CLUSTER_ROLE_DETACHED;
        cluster->recovery_eligible = true;
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
        cluster->role = UCN_CLUSTER_ROLE_DETACHED;
        cluster->recovery_eligible = true;
        break;
    case UCN_CLUSTER_PHASE_RECOVERY_HEAD:
        cluster->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
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

/* CLV2-01-04 RULE: cluster_transition() may centralize existing state
 * transitions, but MUST NOT create new protocol semantics.  Entry/exit
 * actions may only reproduce effects already performed by the migrated
 * legacy transition site. */
static ucn_result_t cluster_transition(ucn_cluster_t *cluster,
                                       ucn_cluster_phase_t old_phase,
                                       ucn_cluster_phase_t new_phase,
                                       ucn_cluster_transition_reason_t reason,
                                       uint32_t now_ms)
    CLV2_01_04_UNUSED;

static ucn_result_t cluster_transition(ucn_cluster_t *cluster,
                                       ucn_cluster_phase_t old_phase,
                                       ucn_cluster_phase_t new_phase,
                                       ucn_cluster_transition_reason_t reason,
                                       uint32_t now_ms)
{
    /* CLV2-01-04 RULE: cluster_transition() may centralize existing state
     * transitions, but MUST NOT create new protocol semantics.  Entry/exit
     * actions may only reproduce effects already performed by the migrated
     * legacy transition site. */

    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    /* Fail closed: the caller's claimed old phase must match the current
     * shadow, and the pair must be legal.  Nothing is written before both
     * checks pass, so a rejection leaves every field untouched. */
    if ((unsigned int)old_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT ||
        (unsigned int)new_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT ||
        cluster->shadow_phase != old_phase ||
        !cluster_transition_is_allowed(old_phase, new_phase)) {
        CLV2_01_04_ASSERT_FAIL(
            "cluster_transition: illegal or mismatched phase transition");
        return UCN_ERR_STATE;
    }
    /* CLV2-01-04a.1 (Item 3) + CLV2-01-04b.2 (human MINOR): the
     * pre-transition discipline is now a REAL runtime validation in BOTH
     * build modes, before any write - the current legacy state must be
     * valid and must still derive the claimed old phase, so a site that
     * already mutated phase-relevant legacy fields is caught and fails
     * closed (UCN_ERR_STATE, nothing committed).  Fields a site sets
     * AFTER the call are fine (the end-of-step/RX shadow sync re-aligns). */
    if (!cluster_legacy_state_is_valid(cluster) ||
        cluster_phase_from_legacy_state(cluster, now_ms) != old_phase) {
#if !defined(NDEBUG)
        assert(0 && "cluster_transition: legacy does not derive old_phase "
               "(site pre-mutated phase-relevant fields?)");
#endif
        return UCN_ERR_STATE;
    }
    /* 1) Commit the shadow mirror (explicit reason replaces the M01
     *    BEST-EFFORT diff inference).  CLV2-01-04a review A (F4): an
     *    UNKNOWN or out-of-range reason must never be recorded on an
     *    accepted transition, so fall back to the BEST-EFFORT pair table.
     *    A non-UNKNOWN caller reason is accepted as-is (the table is not
     *    authoritative: a pair can be reached by several events, and the
     *    wiring stages will pass exact per-event reasons). */
    if (reason == UCN_CLUSTER_REASON_UNKNOWN ||
        (unsigned int)reason >= (unsigned int)UCN_CLUSTER_REASON_COUNT) {
        reason = cluster_reason_from_diff(old_phase, new_phase);
    }
    cluster->shadow_phase = new_phase;
    cluster->transition_reason = reason;
    cluster->shadow_transition_count++;
    /* 2) Keep the legacy fields consistent with the new phase (entry
     *    actions).  Exit actions are added by the wiring stages that
     *    replace the direct legacy writes site by site. */
    cluster_transition_apply_legacy(cluster, new_phase, now_ms);
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

/* Test-only view of the BEST-EFFORT pair->reason table, so the matrix
 * test can pass a real per-pair reason (F4) instead of UNKNOWN. */
ucn_cluster_transition_reason_t ucn_cluster_test_reason_from_diff(
    ucn_cluster_phase_t old_phase, ucn_cluster_phase_t new_phase)
{
    return cluster_reason_from_diff(old_phase, new_phase);
}

/* CLV2-01-04b NIT-1: test-only view of the PRODUCTION OBSERVED table
 * (CLUSTER_TRANSITION_OBSERVED_ALLOWED = DIRECT union the tick-
 * granularity compounds), so the T-A observed-pairs gate checks the
 * SINGLE production table instead of duplicating it in the tests. */
bool ucn_cluster_test_observed_pair_allowed(ucn_cluster_phase_t old_phase,
                                            ucn_cluster_phase_t new_phase)
{
    if ((unsigned int)old_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT ||
        (unsigned int)new_phase >= (unsigned int)UCN_CLUSTER_PHASE_COUNT) {
        return false;
    }
    return (CLUSTER_TRANSITION_OBSERVED_ALLOWED[old_phase] &
            (UINT32_C(1) << (unsigned int)new_phase)) != 0U;
}
#endif

#undef CLV2_01_04_UNUSED

static uint16_t member_count_u16(const ucn_cluster_t *cluster)
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

static uint16_t available_capacity(const ucn_cluster_t *cluster)
{
    uint16_t used = member_count_u16(cluster);

    return used >= cluster->config.member_capacity ?
               0U : (uint16_t)(cluster->config.member_capacity - used);
}

static uint32_t next_nonce(ucn_cluster_t *cluster)
{
    uint32_t nonce = cluster->next_nonce;

    if (nonce == 0U || nonce == UINT32_MAX) {
        nonce = 1U;
    }
    cluster->next_nonce = nonce + 1U;
    return nonce;
}

/* C07.5 control-plane Token Bucket.  One aggregate pool bounds the total
 * control rate to the Cluster window budget (burst + refill rate per 1 s). */
static void token_bucket_refill(ucn_cluster_token_bucket_t *bucket,
                                uint32_t now_ms, uint16_t burst,
                                uint32_t refill_ms)
{
    uint32_t elapsed;
    uint32_t added;

    if (bucket->last_refill_ms == 0U) {
        /* C07.7 P2: cold start already carries a full burst; only stamp the
         * clock here so t=0 consumption cannot be followed by a second,
         * spurious full-burst refill at t=1. */
        bucket->last_refill_ms = now_ms;
        return;
    }
    elapsed = now_ms - bucket->last_refill_ms;
    if (elapsed < refill_ms) {
        return;
    }
    /* Advance the clock by the FULL elapsed interval (do NOT cap `added`
     * at burst); otherwise last_refill_ms lags and idle credit leaks out
     * as repeated burst tokens, defeating the 1 s bound. */
    added = elapsed / refill_ms;
    {
        uint32_t total = (uint32_t)bucket->tokens + added;

        bucket->tokens = total > (uint32_t)burst ? burst : (uint16_t)total;
    }
    bucket->last_refill_ms += added * refill_ms;
}

static bool token_bucket_take(ucn_cluster_t *cluster)
{
    ucn_cluster_token_bucket_t *tb = &cluster->token_bucket;

    token_bucket_refill(tb, cluster_now(cluster),
                        cluster->config.token_bucket.burst,
                        cluster->config.token_bucket.refill_ms);
    if (tb->tokens == 0U) {
        cluster->stats.token_deferred++;
        return false;
    }
    tb->tokens--;
    return true;
}

static ucn_result_t cluster_transmit(
    ucn_cluster_t *cluster, ucn_node_id_t destination,
    const ucn_cluster_message_t *message,
    const uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES])
{
    ucn_result_t result;

    (void)message;
    if (!token_bucket_take(cluster)) {
        return UCN_ERR_NO_SPACE;
    }
    result = cluster->config.send(cluster->config.send_context, destination,
                                  UCN_CLUSTER_CONTROL_ENDPOINT, payload,
                                  (uint16_t)UCN_CLUSTER_MESSAGE_BYTES);
    if (result == UCN_OK) {
        cluster->stats.messages_sent++;
    } else {
        cluster->stats.send_failures++;
    }
    return result;
}

static ucn_result_t send_message(
    ucn_cluster_t *cluster,
    ucn_node_id_t destination,
    ucn_cluster_message_type_t type,
    ucn_cluster_role_t role,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id,
    uint16_t head_score,
    uint16_t capacity)
{
    ucn_cluster_message_t message;
    uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_result_t result;

    (void)memset(&message, 0, sizeof(message));
    message.type = type;
    message.role = role;
    message.cluster_id = cluster_id;
    message.term = term;
    message.head_node_id = head_node_id;
    message.head_score = head_score;
    message.available_capacity = capacity;
    message.lease_ms = cluster->config.lease_ms;
    message.nonce = next_nonce(cluster);
    result = ucn_cluster_message_encode(&message, payload);
    if (result != UCN_OK) {
        return result;
    }
    return cluster_transmit(cluster, destination, &message, payload);
}

static bool config_is_valid(const ucn_cluster_config_t *config)
{
    if (config == NULL || config->local_node_id == 0U ||
        config->local_node_id == UCN_NODE_BROADCAST ||
        config->head_score > UCN_CLUSTER_SCORE_MAX) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    if (config->now_ms == NULL || config->send == NULL ||
         (config->head_capable &&
          (config->member_capacity == 0U ||
           config->member_capacity > UCN_CLUSTER_MAX_MEMBERS ||
           config->member_capacity > UCN_CLUSTER_MAX_PEERS)) ||
        (!config->head_capable && config->member_capacity != 0U)) {
        return false;
    }
    return true;
}

ucn_result_t ucn_cluster_config_apply_timing_profile(
    ucn_cluster_config_t *config,
    ucn_cluster_timing_profile_t profile)
{
    if (config == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    switch (profile) {
        case UCN_CLUSTER_TIMING_PROFILE_DEFAULT:
            config->observation_ms = UCN_CLUSTER_OBSERVATION_MS;
            config->recovery_observation_ms =
                UCN_CLUSTER_RECOVERY_OBSERVATION_MS;
            config->election_window_ms = UCN_CLUSTER_ELECTION_WINDOW_MS;
            config->advertise_interval_ms = UCN_CLUSTER_ADVERTISE_INTERVAL_MS;
            config->join_retry_ms = UCN_CLUSTER_JOIN_RETRY_MS;
            config->keepalive_interval_ms = UCN_CLUSTER_KEEPALIVE_INTERVAL_MS;
            config->lease_ms = UCN_CLUSTER_LEASE_MS;
            config->head_min_tenure_ms = UCN_CLUSTER_HEAD_MIN_TENURE_MS;
            config->token_bucket.burst = UCN_CLUSTER_TB_BURST;
            config->token_bucket.refill_ms = UCN_CLUSTER_TB_REFILL_MS;
            config->recovery_head_ttl_ms =
                UCN_CLUSTER_RECOVERY_HEAD_TTL_MS;
            config->recovery_backoff_max_ms =
                UCN_CLUSTER_RECOVERY_BACKOFF_MAX_MS;
            return UCN_OK;
        case UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED:
            config->observation_ms = UCN_CLUSTER_FAST_OBSERVATION_MS;
            config->recovery_observation_ms =
                UCN_CLUSTER_FAST_RECOVERY_OBSERVATION_MS;
            config->election_window_ms = UCN_CLUSTER_FAST_ELECTION_WINDOW_MS;
            config->advertise_interval_ms =
                UCN_CLUSTER_FAST_ADVERTISE_INTERVAL_MS;
            config->join_retry_ms = UCN_CLUSTER_FAST_JOIN_RETRY_MS;
            config->keepalive_interval_ms =
                UCN_CLUSTER_FAST_KEEPALIVE_INTERVAL_MS;
            config->lease_ms = UCN_CLUSTER_FAST_LEASE_MS;
            config->head_min_tenure_ms =
                UCN_CLUSTER_FAST_HEAD_MIN_TENURE_MS;
            config->token_bucket.burst = UCN_CLUSTER_FAST_TB_BURST;
            config->token_bucket.refill_ms =
                UCN_CLUSTER_FAST_TB_REFILL_MS;
            config->recovery_head_ttl_ms =
                UCN_CLUSTER_FAST_RECOVERY_HEAD_TTL_MS;
            config->recovery_backoff_max_ms =
                UCN_CLUSTER_FAST_RECOVERY_BACKOFF_MAX_MS;
            return UCN_OK;
        default:
            return UCN_ERR_ARGUMENT;
    }
}

static void apply_config_defaults(ucn_cluster_config_t *config)
{
    if (config->observation_ms == 0U) {
        config->observation_ms = UCN_CLUSTER_OBSERVATION_MS;
    }
    if (config->recovery_observation_ms == 0U) {
        config->recovery_observation_ms =
            UCN_CLUSTER_RECOVERY_OBSERVATION_MS;
    }
    if (config->election_window_ms == 0U) {
        config->election_window_ms = UCN_CLUSTER_ELECTION_WINDOW_MS;
    }
    if (config->advertise_interval_ms == 0U) {
        config->advertise_interval_ms = UCN_CLUSTER_ADVERTISE_INTERVAL_MS;
    }
    if (config->join_retry_ms == 0U) {
        config->join_retry_ms = UCN_CLUSTER_JOIN_RETRY_MS;
    }
    if (config->keepalive_interval_ms == 0U) {
        config->keepalive_interval_ms = UCN_CLUSTER_KEEPALIVE_INTERVAL_MS;
    }
    if (config->lease_ms == 0U) {
        config->lease_ms = UCN_CLUSTER_LEASE_MS;
    }
    if (config->head_min_tenure_ms == 0U) {
        config->head_min_tenure_ms = UCN_CLUSTER_HEAD_MIN_TENURE_MS;
    }
    if (config->switch_improvement_percent == 0U) {
        config->switch_improvement_percent =
            UCN_CLUSTER_SWITCH_IMPROVEMENT_PERCENT;
    }
    if (config->switch_required_samples == 0U) {
        config->switch_required_samples = UCN_CLUSTER_SWITCH_REQUIRED_SAMPLES;
    }
    if (config->token_bucket.burst == 0U) {
        config->token_bucket.burst = UCN_CLUSTER_TB_BURST;
    }
    if (config->token_bucket.refill_ms == 0U) {
        config->token_bucket.refill_ms = UCN_CLUSTER_TB_REFILL_MS;
    }
    if (config->recovery_head_ttl_ms == 0U) {
        config->recovery_head_ttl_ms = UCN_CLUSTER_RECOVERY_HEAD_TTL_MS;
    }
    if (config->recovery_backoff_max_ms == 0U) {
        config->recovery_backoff_max_ms =
            UCN_CLUSTER_RECOVERY_BACKOFF_MAX_MS;
    }
}

static bool normalized_config_is_valid(const ucn_cluster_config_t *config)
{
    return ucn_duration_is_valid(config->observation_ms) &&
           ucn_duration_is_valid(config->recovery_observation_ms) &&
           ucn_duration_is_valid(config->election_window_ms) &&
           ucn_duration_is_valid(config->advertise_interval_ms) &&
           ucn_duration_is_valid(config->join_retry_ms) &&
           ucn_duration_is_valid(config->keepalive_interval_ms) &&
           ucn_duration_is_valid(config->lease_ms) &&
           ucn_duration_is_valid(config->head_min_tenure_ms) &&
           config->advertise_interval_ms <= config->lease_ms / 3U &&
           config->keepalive_interval_ms <= config->lease_ms / 3U &&
           config->switch_improvement_percent < 100U &&
           config->switch_required_samples != 0U &&
           config->token_bucket.burst != 0U &&
           ucn_duration_is_valid(config->token_bucket.refill_ms) &&
           ucn_duration_is_valid(config->recovery_head_ttl_ms) &&
           ucn_duration_is_valid(config->recovery_backoff_max_ms);
}

ucn_result_t ucn_cluster_init(
    ucn_cluster_t *cluster,
    const ucn_cluster_config_t *config)
{
    uint32_t now_ms = 0U;

    if (cluster == NULL || !config_is_valid(config)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(cluster, 0, sizeof(*cluster));
    cluster->config = *config;
    apply_config_defaults(&cluster->config);
    if (cluster->config.enabled && !normalized_config_is_valid(&cluster->config)) {
        (void)memset(cluster, 0, sizeof(*cluster));
        return UCN_ERR_CONFIG;
    }
    if (cluster->config.enabled) {
        now_ms = cluster_now(cluster);
    }
    cluster->role = cluster->config.enabled ? UCN_CLUSTER_ROLE_DETACHED :
                                               UCN_CLUSTER_ROLE_DISABLED;
    cluster->observation_deadline_ms = ucn_deadline_from_now(
        now_ms, cluster->config.observation_ms);
    cluster->role_since_ms = now_ms;
    cluster->next_nonce = 1U;
    /* Start the Token Bucket full so cold-start election is not throttled
     * below its already-bounded advertise budget. */
    cluster->token_bucket.tokens = cluster->config.token_bucket.burst;
    /* CLV2-01-02: seed the shadow phase mirror from the initial legacy
     * state (DETACHED_OBSERVE or DISABLED). */
    cluster->shadow_phase = cluster_phase_from_legacy_state(cluster, now_ms);
    cluster->transition_reason = UCN_CLUSTER_REASON_INIT;
    cluster->shadow_transition_count = 0U;
    return UCN_OK;
}

ucn_result_t ucn_cluster_set_head_score(
    ucn_cluster_t *cluster,
    uint16_t head_score)
{
    if (cluster == NULL || head_score > UCN_CLUSTER_SCORE_MAX) {
        return UCN_ERR_ARGUMENT;
    }
    cluster->config.head_score = head_score;
    if ((cluster->role == UCN_CLUSTER_ROLE_CANDIDATE ||
         cluster->role == UCN_CLUSTER_ROLE_HEAD) &&
        cluster->head_node_id == cluster->config.local_node_id) {
        cluster->current_head_score = head_score;
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_sync_neighbors(
    ucn_cluster_t *cluster,
    const ucn_neighbor_summary_t *neighbors,
    size_t neighbor_count)
{
    size_t input_index;
    size_t output_index = 0U;

    if (cluster == NULL || (neighbors == NULL && neighbor_count != 0U)) {
        return UCN_ERR_ARGUMENT;
    }
    {
        /* C07.7 P2: stage the new peer table and commit only on success so
         * an overflow can never leave a half-written table behind. */
        ucn_cluster_peer_t staged[UCN_CLUSTER_MAX_PEERS];
        size_t staged_count = 0U;

        (void)memset(staged, 0, sizeof(staged));
        for (input_index = 0U; input_index < neighbor_count; ++input_index) {
            if (neighbors[input_index].state != UCN_NEIGHBOR_ADMITTED &&
                neighbors[input_index].state != UCN_NEIGHBOR_SUSPECT) {
                continue;
            }
            if (staged_count >= UCN_CLUSTER_MAX_PEERS) {
                return UCN_ERR_NO_SPACE;
            }
            staged[staged_count].occupied = true;
            staged[staged_count].node_id =
                neighbors[input_index].peer_node_id;
            staged[staged_count].neighbor_state =
                neighbors[input_index].state;
            staged[staged_count].last_seen_ms =
                neighbors[input_index].last_seen_ms;
            ++staged_count;
        }
        (void)memcpy(cluster->peers, staged, sizeof(staged));
        output_index = staged_count;
    }
    if (cluster->advertise_cursor >= output_index) {
        cluster->advertise_cursor = 0U;
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_sync_node_neighbors(
    ucn_cluster_t *cluster,
    const ucn_node_t *node)
{
    ucn_neighbor_summary_t summaries[UCN_MAX_NEIGHBORS];
    size_t count;

    if (cluster == NULL || node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    count = ucn_node_copy_neighbor_summaries(node, summaries,
                                             UCN_MAX_NEIGHBORS);
    return ucn_cluster_sync_neighbors(cluster, summaries, count);
}

static const ucn_cluster_peer_t *find_peer(
    const ucn_cluster_t *cluster,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_PEERS; ++index) {
        if (cluster->peers[index].occupied &&
            cluster->peers[index].node_id == node_id) {
            return &cluster->peers[index];
        }
    }
    return NULL;
}

static ucn_cluster_candidate_t *find_candidate(
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

static ucn_cluster_candidate_t *allocate_candidate(
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

static ucn_result_t observe_candidate(
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

static bool candidate_better(
    uint16_t candidate_score,
    ucn_node_id_t candidate_node,
    uint16_t current_score,
    ucn_node_id_t current_node)
{
    return candidate_score > current_score ||
           (candidate_score == current_score && candidate_node < current_node);
}

static bool score_improves_by(
    uint16_t candidate_score,
    uint16_t current_score,
    uint8_t percent)
{
    uint32_t required = (uint32_t)current_score * (100U + percent);

    return (uint32_t)candidate_score * 100U >= required;
}

static void set_detached(
    ucn_cluster_t *cluster,
    uint32_t now_ms,
    uint32_t observation_ms)
{
    cluster->role = UCN_CLUSTER_ROLE_DETACHED;
    cluster->cluster_id = 0U;
    cluster->term = 0U;
    cluster->head_node_id = 0U;
    cluster->current_head_score = 0U;
    cluster->pending_head_node_id = 0U;
    cluster->pending_cluster_id = 0U;
    cluster->pending_term = 0U;
    cluster->pending_head_score = 0U;
    cluster->known_backup_node_id = 0U;
    cluster->known_backup_generation = 0U;
    /* C07.7 P1: detaching resets the takeover vote identity so a later
     * Cluster at the same term number is not suppressed by an old vote. */
    cluster->member_voted_term = 0U;
    cluster->member_voted_cluster_id = 0U;
    cluster->member_voted_generation = 0U;
    cluster->head_lease_expires_at_ms = 0U;
    cluster->head_grace_deadline_ms = 0U;
    cluster->election_deadline_ms = 0U;
    cluster->role_since_ms = now_ms;
    cluster->observation_deadline_ms =
        ucn_deadline_from_now(now_ms, observation_ms);
}

/* CLV2-01-04b.3: begin_join()'s field payload WITHOUT the role write.
 * The migrated consider_head_offer() callers (DETACHED_OBSERVE/ELECTION
 * !recovery_eligible, MEMBER/GRACE better-Head switch 01-04c.4) run
 * cluster_transition() FIRST (which owns the role write via apply_legacy)
 * and then apply the remaining site payload through this helper.
 * begin_join() still applies ALL fields (helper + role) for its
 * not-yet-migrated callers (BACKUP newer-Term, JOIN_PENDING re-target,
 * RECOVERY_* sources). */
static void begin_join_prepare_fields(
    ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    /* Joining a live Head abandons any pending Recovery candidacy. */
    cluster->recovery_eligible = false;
    cluster->recovery_backoff_deadline_ms = 0U;
    cluster->role_since_ms = now_ms;
    cluster->pending_head_node_id = candidate->head_node_id;
    cluster->pending_cluster_id = candidate->cluster_id;
    cluster->pending_term = candidate->term;
    cluster->pending_head_score = candidate->head_score;
    cluster->next_join_retry_ms = now_ms;
}

static void begin_join(
    ucn_cluster_t *cluster,
    const ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    /* CLV2-01-04b.3: role write + the field payload; the write order is
     * irrelevant (all plain field writes, no interleaved side effects),
     * so the shared helper is reused as-is. */
    cluster->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    begin_join_prepare_fields(cluster, candidate, now_ms);
}

static ucn_cluster_member_t *find_member(
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

static ucn_cluster_member_t *allocate_member(
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

static void remove_member(ucn_cluster_t *cluster, ucn_node_id_t node_id)
{
    ucn_cluster_member_t *member = find_member(cluster, node_id);

    if (member != NULL) {
        (void)memset(member, 0, sizeof(*member));
    }
    if (cluster->backup_node_id == node_id) {
        cluster->backup_node_id = 0U;
        cluster->backup_ready = false;
    }
    backup_resync(cluster);
}

static void clear_members(ucn_cluster_t *cluster)
{
    (void)memset(cluster->members, 0, sizeof(cluster->members));
}

static ucn_result_t handle_join_request(
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

/* C07.7 P1: JOIN_ACCEPT/JOIN_REJECT carry the join transaction id by
 * echoing the request nonce, so a stale reply cannot match a newer join. */
static ucn_result_t send_join_reply(ucn_cluster_t *cluster,
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

static ucn_result_t handle_join_accept(
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

static ucn_result_t handle_keepalive(
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

/* §8 ordered switchback: notify members before abandoning the Head role. */
static void send_head_stepdown(ucn_cluster_t *cluster)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->members[index].occupied) {
            (void)send_message(cluster, cluster->members[index].node_id,
                               UCN_CLUSTER_MSG_HEAD_STEPDOWN,
                               UCN_CLUSTER_ROLE_HEAD, cluster->cluster_id,
                               cluster->term, cluster->config.local_node_id,
                               cluster->config.head_score, 0U);
        }
    }
}

/* Ordered yield to a stable Head: notify members, then enter STEPPING_DOWN. */
static void begin_ordered_stepdown(ucn_cluster_t *cluster,
                                    const ucn_cluster_candidate_t *candidate,
                                    uint32_t now_ms)
{
    /* Yielding to a stable Head abandons any Recovery candidacy. */
    cluster->recovery_eligible = false;
    cluster->recovery_backoff_deadline_ms = 0U;
    (void)send_head_stepdown(cluster);
    cluster->role = UCN_CLUSTER_ROLE_STEPPING_DOWN;
    cluster->role_since_ms = now_ms;
    cluster->stepdown_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
    cluster->pending_head_node_id = candidate->head_node_id;
    cluster->pending_cluster_id = candidate->cluster_id;
    cluster->pending_term = candidate->term;
    cluster->pending_head_score = candidate->head_score;
    cluster->stats.head_switches++;
}

/* §8.3 Backup score challenge: a Backup that is significantly better than
 * its live Primary (and after the minimum tenure) leaves the Backup role and
 * re-enters election so the current Head can observe and yield to it. */
static void backup_challenge(ucn_cluster_t *cluster, uint32_t now_ms)
{
    cluster->backup_ready = false;
    cluster->backup_syncing = false;
    cluster->backup_primary_node_id = 0U;
    cluster->backup_primary_deadline_ms = 0U;
    cluster->backup_primary_lease_deadline_ms = 0U;
    cluster->backup_missed_heartbeats = 0U;
    cluster->backup_takeover_active = false;
    cluster->stats.head_switches++;
    /* Re-enter election in the SAME Cluster (keep cluster_id, bump Term) so
     * the current Head can observe the higher score and yield to us. */
    cluster->role = UCN_CLUSTER_ROLE_CANDIDATE;
    cluster->term = cluster->term == UINT32_MAX ? 1U : cluster->term + 1U;
    cluster->head_node_id = cluster->config.local_node_id;
    cluster->current_head_score = cluster->config.head_score;
    cluster->role_since_ms = now_ms;
    cluster->election_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.election_window_ms);
    cluster->next_advertise_ms = now_ms;
    cluster->stats.elections_started++;
}

static void consider_head_offer(
    ucn_cluster_t *cluster,
    ucn_cluster_candidate_t *candidate,
    uint32_t now_ms)
{
    if (candidate->head_node_id == cluster->config.local_node_id) {
        return;
    }
    /* A full Head must keep refreshing existing members.  Capacity zero only
     * rejects new joins; treating it as an unavailable current Head causes
     * valid members to expire their lease and create a split brain. */
    if (cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
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
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
#endif
        return;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
        if (candidate->head_node_id == cluster->backup_primary_node_id &&
            candidate->cluster_id == cluster->cluster_id &&
            candidate->term == cluster->term) {
            /* A protected Head ADVERTISE is independent liveness evidence
             * in addition to the direct Primary heartbeat.  Refreshing the
             * lease here prevents a Backup from falsely taking over merely
             * because several heartbeat unicasts were lost on a live link. */
            cluster->backup_primary_lease_deadline_ms =
                ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
            /* §8.3: a Backup that is significantly better than its live
             * Primary (after minimum tenure) challenges by re-entering
             * election; this must not be gated by Primary capacity. */
            if (score_improves_by(cluster->config.head_score,
                                  candidate->head_score,
                                  cluster->config.switch_improvement_percent) &&
                ucn_elapsed_at_least(now_ms, cluster->role_since_ms,
                                     cluster->config.head_min_tenure_ms)) {
                backup_challenge(cluster, now_ms);
            }
        } else if (candidate->cluster_id == cluster->cluster_id &&
                   candidate->term > cluster->term) {
            /* C07.7 P1: only a same-Cluster, legitimately newer-Term Head
             * interrupts a pending takeover; the Backup abandons it and
             * joins the newer Head instead of risking split brain.  A
             * different Cluster's term is NOT comparable (Target §8.3):
             * cross-Cluster convergence is owned by Head-to-Head Merge,
             * never by a Backup jumping at a foreign term. */
            cluster->backup_takeover_active = false;
            backup_clear_sync(cluster, now_ms);
            begin_join(cluster, candidate, now_ms);
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
    if (cluster->role == UCN_CLUSTER_ROLE_HEAD) {
        if (candidate->term > cluster->term) {
            /* A newer-generation Head wins immediately (§5.3: the older
             * Head must defer rather than reclaim by raw score). */
            begin_ordered_stepdown(cluster, candidate, now_ms);
            return;
        }
        if (candidate->term < cluster->term) {
            /* A stale Head must not be followed, even with a high score. */
            return;
        }
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
        return;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        /* A stable Head reclaims the domain from a temporary Recovery
         * Head; ordered stepdown switches back to the original Cluster. */
        begin_ordered_stepdown(cluster, candidate, now_ms);
        return;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_DETACHED ||
        cluster->role == UCN_CLUSTER_ROLE_CANDIDATE) {
        /* C07.7 P1: available_capacity == 0 gates new JOINs only; it must
         * never block epoch convergence between existing Heads (handled
         * above), so the capacity check lives here at the join point. */
        if (candidate->available_capacity == 0U) {
            return;
        }
        /* CLV2-01-04b.3: a DETACHED_OBSERVE or ELECTION node accepting a
         * stable Head offer performs the DETACHED_OBSERVE/ELECTION ->
         * JOIN_PENDING transition through the single entry point BEFORE
         * any phase-relevant legacy mutation (the role write is owned by
         * apply_legacy); the remaining begin_join() field payload follows
         * at the site.  RECOVERY_* sources (recovery_eligible) keep the
         * legacy begin_join() until 01-04f. */
        if (!cluster->recovery_eligible) {
            ucn_cluster_phase_t old_phase =
                (cluster->role == UCN_CLUSTER_ROLE_CANDIDATE)
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
            assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                   UCN_CLUSTER_PHASE_JOIN_PENDING);
#endif
            return;
        }
        begin_join(cluster, candidate, now_ms);
        return;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING) {
        /* Re-target a stale pending Head (e.g. after a takeover): switch
         * only when the observed Head differs or has a higher Term. */
        if (candidate->head_node_id != cluster->pending_head_node_id ||
            candidate->term > cluster->pending_term) {
            begin_join(cluster, candidate, now_ms);
        }
        return;
    }
    if (cluster->role != UCN_CLUSTER_ROLE_MEMBER) {
        return;
    }
    if (!score_improves_by(candidate->head_score,
                           cluster->current_head_score,
                           cluster->config.switch_improvement_percent)) {
        candidate->better_samples = 0U;
        return;
    }
    if (candidate->better_samples < UINT8_MAX) {
        candidate->better_samples++;
    }
    if (candidate->better_samples >= cluster->config.switch_required_samples) {
        /* CLV2-01-04c.4 (human-ordered): the LEAVE notice to the old Head
         * stays FIRST, then stats.head_switches++, THEN the transition
         * (MEMBER_ACTIVE or MEMBER_TAKEOVER_GRACE -> JOIN_PENDING) runs
         * fail-closed, and only afterwards is the begin_join() field
         * payload applied through begin_join_prepare_fields() (apply_legacy
         * owns the role write).  Neither the LEAVE send nor head_switches++
         * is a phase-relevant mutation, so the pre-transition derive check
         * still passes. */
        (void)send_message(cluster, cluster->head_node_id,
                           UCN_CLUSTER_MSG_LEAVE, UCN_CLUSTER_ROLE_MEMBER,
                           cluster->cluster_id, cluster->term,
                           cluster->head_node_id, cluster->current_head_score, 0U);
        cluster->stats.head_switches++;
        if (cluster->head_grace_deadline_ms != 0U) {
            if (cluster_transition(cluster,
                                   UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                                   UCN_CLUSTER_PHASE_JOIN_PENDING,
                                   UCN_CLUSTER_REASON_JOIN_INITIATED,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition (shadow mismatch /
                 * illegal pair / pre-mutated phase fields) leaves every
                 * field untouched - do NOT apply the join payload. */
                return;
            }
        } else {
            if (cluster_transition(cluster,
                                   UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                                   UCN_CLUSTER_PHASE_JOIN_PENDING,
                                   UCN_CLUSTER_REASON_JOIN_INITIATED,
                                   now_ms) != UCN_OK) {
                /* Fail closed: see the GRACE branch above. */
                return;
            }
        }
        begin_join_prepare_fields(cluster, candidate, now_ms);
#if !defined(NDEBUG)
        /* CLV2-01-04c.4 post-commit derive assert: after the transition
         * and the site join payload the legacy state must still derive
         * JOIN_PENDING. */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_JOIN_PENDING);
#endif
    }
}

/* C07.2 Backup state machine helpers. */

static ucn_result_t send_cluster_message(
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
static ucn_cluster_member_t *backup_allocate_mirror(ucn_cluster_t *cluster,
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
static bool backup_covers_all_members(const ucn_cluster_t *cluster)
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

static void backup_clear_sync(ucn_cluster_t *cluster, uint32_t now_ms)
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

static ucn_result_t send_backup_assign(
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
static void assign_backup(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    const ucn_cluster_candidate_t *best = NULL;
    ucn_node_id_t best_node_id = 0U;

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
    cluster->backup_node_id = best_node_id;
    cluster->backup_generation = cluster->backup_generation == UINT32_MAX ?
                                     1U : cluster->backup_generation + 1U;
    cluster->backup_ready = false;
    cluster->membership_sequence = 0U;
    cluster->backup_sync_cursor = 0U;
    start_backup_assignment_cycle(cluster, now_ms);
    cluster->next_backup_heartbeat_ms = now_ms;
    cluster->next_backup_sync_ms = now_ms;
    cluster->backup_resync_deadline_ms = ucn_deadline_from_now(
        now_ms, cluster->config.lease_ms);
}

static ucn_result_t handle_backup_assign(ucn_cluster_t *cluster,
                                           ucn_node_id_t source,
                                           const ucn_cluster_message_t *message,
                                           uint32_t now_ms)
{
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
            message->sync_token == UCN_NODE_BROADCAST) {
            return UCN_ERR_ACCESS;
        }
    }
    /* Every current Member receives the same protected assignment record. */
    cluster->known_backup_node_id = message->sync_token;
    cluster->known_backup_generation = message->backup_generation;
    if (message->sync_token != cluster->config.local_node_id) {
        return UCN_OK;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
        return message->backup_generation == cluster->backup_generation ?
                   UCN_OK : UCN_ERR_REPLAY;
    }
    if (!cluster->config.head_capable) {
        return UCN_ERR_UNSUPPORTED;
    }
    cluster->role = UCN_CLUSTER_ROLE_BACKUP;
    /* Commit the identity now: a JOIN_ACCEPT may be dropped on a lossy
     * link, and a stale head_node_id==local would otherwise make the
     * Backup send Keepalive to itself. */
    cluster->cluster_id = message->cluster_id;
    cluster->term = message->term;
    cluster->head_node_id = message->head_node_id;
    cluster->backup_syncing = true;
    cluster->backup_ready = false;
    cluster->backup_primary_node_id = source;
    cluster->backup_generation = message->backup_generation;
    cluster->membership_sequence = 0U;
    cluster->backup_primary_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
    cluster->backup_primary_lease_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    cluster->backup_missed_heartbeats = 0U;
    (void)clear_members(cluster);
    return UCN_OK;
}

static ucn_result_t send_backup_ready(ucn_cluster_t *cluster)
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

static ucn_result_t handle_backup_ready(ucn_cluster_t *cluster,
                                          ucn_node_id_t source,
                                          const ucn_cluster_message_t *message)
{
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
    cluster->backup_ready = true;
    return UCN_OK;
}

static ucn_result_t handle_backup_member_sync(ucn_cluster_t *cluster,
                                                ucn_node_id_t source,
                                                const ucn_cluster_message_t *message,
                                                uint32_t now_ms)
{
    ucn_cluster_member_t *member;

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
    if ((message->flags & UCN_CLUSTER_FLAG_SYNC_DELTA) != 0U) {
        /* C07.7 P1: live incremental refresh: update the member's nonce
         * without touching syncing/ready so a periodic refresh can never
         * strand a ready Backup during a Primary failure.  A stale DELTA
         * (already applied sequence) is ignored.  A sequence gap means a
         * DELTA was lost: the mirror may be missing a member nonce, so
         * request a full resync instead of silently continuing. */
        if (message->membership_sequence <= cluster->membership_sequence) {
            return UCN_OK;
        }
        if (message->membership_sequence != cluster->membership_sequence + 1U) {
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
    if (message->membership_sequence != cluster->membership_sequence + 1U) {
        /* A dropped/reordered snapshot frame on a lossy link: stay syncing
         * and await the bounded snapshot retransmit (a fresh BEGIN resets
         * the sequence) instead of detaching. */
        cluster->stats.malformed_messages++;
        cluster->backup_syncing = true;
        cluster->backup_ready = false;
        cluster->membership_sequence = 0U;
        cluster->backup_primary_deadline_ms =
            ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
        cluster->backup_primary_lease_deadline_ms =
            ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
        return UCN_ERR_REPLAY;
    }
    cluster->membership_sequence = message->membership_sequence;
    if ((message->flags & UCN_CLUSTER_FLAG_SYNC_END) != 0U) {
        if (backup_covers_all_members(cluster)) {
            cluster->backup_syncing = false;
            cluster->backup_ready = true;
            return send_backup_ready(cluster);
        }
        cluster->stats.joins_rejected++;
        /* C07.7 P1: tell the Head immediately so it can pick the next
         * candidate instead of waiting for the member lease to expire. */
        (void)send_backup_reject(cluster,
                                  UCN_CLUSTER_BACKUP_REJECT_COVERAGE);
        backup_clear_sync(cluster, now_ms);
        return UCN_OK;
    }
    member = backup_allocate_mirror(cluster, message->member_node_id);
    if (member == NULL) {
        (void)send_backup_reject(cluster,
                                  UCN_CLUSTER_BACKUP_REJECT_NO_SPACE);
        backup_clear_sync(cluster, now_ms);
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

static ucn_result_t handle_primary_heartbeat(ucn_cluster_t *cluster,
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
static ucn_result_t send_backup_resync_req(ucn_cluster_t *cluster)
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
static ucn_result_t send_backup_reject(ucn_cluster_t *cluster,
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
static ucn_result_t handle_backup_resync_req(ucn_cluster_t *cluster,
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
static ucn_result_t handle_backup_reject(ucn_cluster_t *cluster,
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
    cluster->backup_candidate_cooldown_until_ms = ucn_deadline_from_now(
        now_ms, cluster->config.keepalive_interval_ms);
    cluster->backup_rejected_node_id = source;
    cluster->backup_node_id = 0U;
    cluster->backup_ready = false;
    assign_backup(cluster, now_ms);
    return UCN_OK;
}

/* C07.3 majority-confirmed takeover. */

static void complete_takeover(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;

    cluster->role = UCN_CLUSTER_ROLE_HEAD;
    cluster->term = cluster->term == UINT32_MAX ? 1U : cluster->term + 1U;
    cluster->head_node_id = cluster->config.local_node_id;
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
}

static void start_takeover(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    bool self_in_mirror = false;

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
}

static void send_takeover_prepare_step(ucn_cluster_t *cluster)
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

static void send_takeover_announce_step(ucn_cluster_t *cluster)
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

static ucn_result_t handle_takeover_prepare(ucn_cluster_t *cluster,
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

static ucn_result_t handle_takeover_ack(ucn_cluster_t *cluster,
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
        complete_takeover(cluster, now_ms);
    }
    return UCN_OK;
}

static ucn_result_t handle_head_takeover(ucn_cluster_t *cluster,
                                           ucn_node_id_t source,
                                           const ucn_cluster_message_t *message,
                                           uint32_t now_ms)
{
    if (message->role != UCN_CLUSTER_ROLE_HEAD) {
        return UCN_ERR_MALFORMED;
    }
    if ((cluster->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD &&
         message->cluster_id != cluster->cluster_id) ||
        source != cluster->known_backup_node_id ||
        message->backup_generation != cluster->known_backup_generation) {
        return UCN_ERR_ACCESS;
    }
    if (message->term <= cluster->term) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    if (cluster->role != UCN_CLUSTER_ROLE_MEMBER &&
        cluster->role != UCN_CLUSTER_ROLE_BACKUP &&
        cluster->role != UCN_CLUSTER_ROLE_JOIN_PENDING &&
        cluster->role != UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        return UCN_ERR_ACCESS;
    }
    /* A Recovery Head defers to the stable higher-Term Head immediately. */
    cluster->recovery_eligible = false;
    cluster->recovery_cluster_id = 0U;
    cluster->recovery_deadline_ms = 0U;
    cluster->role = UCN_CLUSTER_ROLE_MEMBER;
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
    cluster->backup_takeover_active = false;
    cluster->backup_syncing = false;
    cluster->backup_ready = false;
    cluster->known_backup_node_id = 0U;
    cluster->known_backup_generation = 0U;
    return UCN_OK;
}

/* C07.5 RECOVERY_HEAD: a short-lived emergency Head formed only after both
 * the Primary and Backup are lost.  The recovery Cluster ID is the
 * declaring node ID, so it never impersonates the lost Cluster. */

static uint32_t compute_recovery_backoff(const ucn_cluster_t *cluster)
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
static bool recovery_quorum_met(const ucn_cluster_t *cluster)
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

static void send_recovery_declare(ucn_cluster_t *cluster)
{
    ucn_cluster_message_t message;
    size_t index;


    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    message.cluster_id = cluster->config.local_node_id;
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

static void start_recovery_backoff(ucn_cluster_t *cluster, uint32_t now_ms)
{
    cluster->recovery_nonce = next_nonce(cluster);
    cluster->recovery_backoff_deadline_ms = ucn_deadline_from_now(
        now_ms, compute_recovery_backoff(cluster));
}

static void declare_recovery_head(ucn_cluster_t *cluster, uint32_t now_ms)
{
    if (cluster->recovery_nonce == 0U) {
        cluster->recovery_nonce = next_nonce(cluster);
    }
    cluster->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    cluster->recovery_cluster_id = cluster->config.local_node_id;
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

static void stepdown_recovery_head(ucn_cluster_t *cluster, uint32_t now_ms)
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

static ucn_result_t handle_recovery_declare(ucn_cluster_t *cluster,
                                              ucn_node_id_t source,
                                              const ucn_cluster_message_t *message,
                                              uint32_t now_ms)
{
    ucn_cluster_message_t ack;

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
        /* Yield the temporary Head role before joining the winner. */
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
    cluster->accepted_recovery_nonce = message->recovery_nonce;
    cluster->known_recovery_source = source;
    /* C07.7 P0-1: actually join the recovery Cluster.  The recovery
     * Cluster ID is the declaring node ID; it never impersonates the
     * lost Cluster.  The member keeps its role_since/lease so the
     * recovery domain has a live membership. */
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
    (void)memset(&ack, 0, sizeof(ack));
    ack.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    ack.role = UCN_CLUSTER_ROLE_MEMBER;
    ack.cluster_id = message->cluster_id;
    ack.term = message->term;
    ack.head_node_id = source;
    return send_cluster_message(cluster, source, &ack);
}

static ucn_result_t handle_recovery_ack(ucn_cluster_t *cluster,
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

static ucn_result_t ucn_cluster_receive_inner(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    bool protected_control,
    const uint8_t *payload,
    size_t payload_length)
{
    ucn_cluster_message_t message;
    ucn_cluster_candidate_t *candidate;
    uint32_t now_ms;
    ucn_result_t result;

    if (cluster == NULL || !cluster->config.enabled || source == 0U ||
        source == UCN_NODE_BROADCAST || source == cluster->config.local_node_id) {
        return UCN_ERR_ARGUMENT;
    }
    if (cluster->config.require_protected_control && !protected_control) {
        cluster->stats.security_rejected++;
        return UCN_ERR_SECURITY;
    }
    if (find_peer(cluster, source) == NULL ||
        find_peer(cluster, source)->neighbor_state != UCN_NEIGHBOR_ADMITTED) {
        return UCN_ERR_ACCESS;
    }
    result = ucn_cluster_message_decode(payload, payload_length, &message);
    if (result != UCN_OK) {
        cluster->stats.malformed_messages++;
        return result;
    }
    if (message.head_node_id != source &&
        message.type != UCN_CLUSTER_MSG_JOIN_REQUEST &&
        message.type != UCN_CLUSTER_MSG_KEEPALIVE &&
        message.type != UCN_CLUSTER_MSG_LEAVE &&
        message.type != UCN_CLUSTER_MSG_BACKUP_READY &&
        message.type != UCN_CLUSTER_MSG_TAKEOVER_PREPARE &&
        message.type != UCN_CLUSTER_MSG_TAKEOVER_ACK &&
        message.type != UCN_CLUSTER_MSG_RECOVERY_ACK &&
        message.type != UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ &&
        message.type != UCN_CLUSTER_MSG_BACKUP_REJECT) {
        cluster->stats.malformed_messages++;
        return UCN_ERR_MALFORMED;
    }
    now_ms = cluster_now(cluster);
    cluster->stats.messages_received++;
    switch (message.type) {
        case UCN_CLUSTER_MSG_ADVERTISE:
        case UCN_CLUSTER_MSG_HEAD_DECLARE:
            if (message.role != UCN_CLUSTER_ROLE_HEAD &&
                message.role != UCN_CLUSTER_ROLE_CANDIDATE) {
                return UCN_ERR_MALFORMED;
            }
            result = observe_candidate(cluster, source, &message, now_ms);
            if (result != UCN_OK) {
                return result;
            }
            candidate = find_candidate(cluster, source);
            if (candidate != NULL && message.role == UCN_CLUSTER_ROLE_HEAD) {
                if (cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
                    candidate->cluster_id == cluster->cluster_id &&
                    candidate->term < cluster->term) {
                    cluster->stats.stale_messages++;
                    return UCN_ERR_REPLAY;
                }
                consider_head_offer(cluster, candidate, now_ms);
            }
            return UCN_OK;
        case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
            return handle_head_takeover(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_JOIN_REQUEST:
            return handle_join_request(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_JOIN_ACCEPT:
            return handle_join_accept(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_JOIN_REJECT:
            /* C07.7 P1: only a REJECT of the exact pending Head epoch
             * ends the join attempt; a stale REJECT is ignored. */
            if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING &&
                source == cluster->pending_head_node_id &&
                message.cluster_id == cluster->pending_cluster_id &&
                message.term == cluster->pending_term &&
                /* C07.7 P1: only a reject of the exact join txid ends the
                 * attempt; a stale reject of an earlier join is ignored. */
                message.nonce == cluster->pending_join_nonce) {
                /* CLV2-01-04b.4: the detach IS the JOIN_PENDING ->
                 * DETACHED_OBSERVE transition - call it BEFORE
                 * set_detached() (set_detached() writes role=DETACHED,
                 * which would violate the pre-transition derive
                 * discipline if it ran first).  set_detached()'s
                 * epoch/vote/lease/grace/known_backup clears stay
                 * site-owned and run after, in original order. */
                if (cluster_transition(cluster,
                                       UCN_CLUSTER_PHASE_JOIN_PENDING,
                                       UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                                       UCN_CLUSTER_REASON_JOIN_REJECTED,
                                       now_ms) != UCN_OK) {
                    /* Fail closed: a rejected transition (shadow mismatch
                     * / illegal pair / pre-mutated phase fields) leaves
                     * every field untouched - do NOT run the detach side
                     * effects; the next Step re-visits the join retry. */
                    return UCN_ERR_STATE;
                }
                cluster->stats.joins_rejected++;
                set_detached(cluster, now_ms,
                             cluster->config.observation_ms);
#if !defined(NDEBUG)
                /* CLV2-01-04b.4 post-commit derive assert: after the
                 * transition AND set_detached() the legacy state must
                 * still derive DETACHED_OBSERVE. */
                assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                       UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
                return UCN_OK;
            }
            return UCN_ERR_ACCESS;
        case UCN_CLUSTER_MSG_KEEPALIVE:
            return handle_keepalive(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_LEAVE:
            if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
                message.cluster_id != cluster->cluster_id ||
                message.term != cluster->term) {
                return UCN_ERR_ACCESS;
            }
            /* C07.7 P1: a replayed LEAVE with an old nonce must not evict
             * a member that has since re-joined and advanced its nonce. */
            {
                ucn_cluster_member_t *member = find_member(cluster, source);

                if (member == NULL) {
                    return UCN_ERR_NOT_FOUND;
                }
                if (message.nonce <= member->last_nonce) {
                    cluster->stats.stale_messages++;
                    return UCN_ERR_REPLAY;
                }
            }
            remove_member(cluster, source);
            return UCN_OK;
        case UCN_CLUSTER_MSG_HEAD_STEPDOWN:
            /* C07.7 P1: the Backup must also leave with the members on an
             * ordered stepdown (it cannot ACK its own takeover against a
             * Head that is deliberately switching away).  The epoch must
             * match to fence stale STEPDOWN frames. */
            if ((cluster->role == UCN_CLUSTER_ROLE_MEMBER ||
                 cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING ||
                 cluster->role == UCN_CLUSTER_ROLE_BACKUP) &&
                source == cluster->head_node_id &&
                message.cluster_id == cluster->cluster_id &&
                message.term == cluster->term &&
                /* C07.7 P1: a replayed STEPDOWN of the same epoch is
                 * ignored via the strictly increasing stepdown nonce. */
                message.nonce > cluster->last_stepdown_nonce) {
                if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING) {
                    /* CLV2-01-04b.6 (human MAJOR): the JOIN_PENDING
                     * sub-branch consumes the anti-replay fence ONLY AFTER
                     * the transition succeeds.  Previously the nonce
                     * advanced before the role branch, so a rejected
                     * transition (shadow/legacy drift) still consumed the
                     * fence - a half-commit contradicting 'rejected
                     * transition leaves every field untouched'.
                     * Transition FIRST: the legacy detach writes
                     * role=DETACHED, which would trip the pre-transition
                     * derive check; set_detached()'s role rewrite
                     * afterwards is redundant-but-harmless (its epoch/
                     * vote/lease/deadline/known_backup clears stay
                     * site-owned).  Reason is EXPLICIT STEPDOWN_ORDERED:
                     * the BEST-EFFORT fallback for this pair would mint
                     * JOIN_REJECTED, semantically wrong for an ordered
                     * stepdown. */
                    if (cluster_transition(cluster,
                                           UCN_CLUSTER_PHASE_JOIN_PENDING,
                                           UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                                           UCN_CLUSTER_REASON_STEPDOWN_ORDERED,
                                           now_ms) != UCN_OK) {
                        /* Fail closed: a rejected transition leaves every
                         * field untouched INCLUDING the anti-replay fence
                         * - the node stays JOIN_PENDING and a later
                         * well-formed STEPDOWN may still be accepted. */
                        return UCN_ERR_STATE;
                    }
                    cluster->last_stepdown_nonce = message.nonce;
                    set_detached(cluster, now_ms,
                                 cluster->config.observation_ms);
#if !defined(NDEBUG)
                    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                           UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
                } else {
                    /* MEMBER/BACKUP sub-branches stay CURRENT legacy order
                     * (write the nonce first, then branch) - they are not
                     * migrated yet. */
                    cluster->last_stepdown_nonce = message.nonce;
                    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
                        backup_clear_sync(cluster, now_ms);
                    } else {
                        set_detached(cluster, now_ms,
                                     cluster->config.observation_ms);
                    }
                }
                return UCN_OK;
            }
            return UCN_ERR_ACCESS;
        case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
            return handle_backup_assign(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_BACKUP_READY:
            return handle_backup_ready(cluster, source, &message);
        case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
            return handle_backup_member_sync(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
            return handle_primary_heartbeat(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
            return handle_backup_resync_req(cluster, source, &message);
        case UCN_CLUSTER_MSG_BACKUP_REJECT:
            return handle_backup_reject(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
            return handle_takeover_prepare(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_TAKEOVER_ACK:
            return handle_takeover_ack(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_RECOVERY_DECLARE:
            return handle_recovery_declare(cluster, source, &message, now_ms);
        case UCN_CLUSTER_MSG_RECOVERY_ACK:
            return handle_recovery_ack(cluster, source, &message, now_ms);
        default:
            return UCN_ERR_UNSUPPORTED;
    }
}

ucn_result_t ucn_cluster_receive(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    bool protected_control,
    const uint8_t *payload,
    size_t payload_length)
{
    ucn_cluster_transition_reason_t hint = UCN_CLUSTER_REASON_UNKNOWN;
    ucn_result_t result;

    /* Peek the wire type (payload byte 1) for a best-effort reason hint;
     * the hint is only used when the exact diff table has no entry. */
    if (cluster != NULL && payload != NULL && payload_length >= 2U &&
        cluster->config.enabled) {
        hint = cluster_rx_reason_from_type(
            (ucn_cluster_message_type_t)payload[1U]);
    }
    result = ucn_cluster_receive_inner(cluster, source, protected_control,
                                       payload, payload_length);
    /* CLV2-01-02: keep the shadow mirror aligned after every RX that could
     * have changed state (rejections like ERR_REPLAY may still have moved
     * backup sync flags, so sync on everything except ARGUMENT). */
    if (cluster != NULL && cluster->config.enabled &&
        result != UCN_ERR_ARGUMENT) {
        cluster_shadow_sync(cluster, hint);
    }
    return result;
}

static void start_election(ucn_cluster_t *cluster, uint32_t now_ms)
{
    /* CLV2-01-04b.1: the role write IS the DETACHED_OBSERVE -> ELECTION
     * transition.  The ONLY call site is ucn_cluster_step_inner()'s
     * DETACHED + !recovery_eligible branch (L4357), so the claimed old
     * phase is always DETACHED_OBSERVE; all other side effects below
     * stay in the site, in their original order. */
    if (cluster_transition(cluster, UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                           UCN_CLUSTER_PHASE_ELECTION,
                           UCN_CLUSTER_REASON_ELECTION_STARTED,
                           now_ms) != UCN_OK) {
        /* Fail closed per the migration contract: a rejected transition
         * (shadow mismatch / illegal pair / pre-mutated phase fields)
         * leaves every field untouched, so do NOT run the election side
         * effects on a non-CANDIDATE node.  The next Step re-visits the
         * DETACHED observation branch. */
        return;
    }
    cluster->cluster_id = cluster->config.local_node_id;
    cluster->term = cluster->term == UINT32_MAX ? 1U : cluster->term + 1U;
    if (cluster->term == 0U) {
        cluster->term = 1U;
    }
    cluster->head_node_id = cluster->config.local_node_id;
    cluster->current_head_score = cluster->config.head_score;
    cluster->role_since_ms = now_ms;
    cluster->election_deadline_ms =
        ucn_deadline_from_now(now_ms, cluster->config.election_window_ms);
    cluster->next_advertise_ms = now_ms;
    cluster->stats.elections_started++;
    /* CLV2-01-04b (roadmap note): post-commit derive assert - after the
     * transition AND every site side effect, the legacy state must still
     * derive ELECTION (derive depends only on role == CANDIDATE). */
#if !defined(NDEBUG)
    assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
           UCN_CLUSTER_PHASE_ELECTION);
#endif
}

static void complete_election(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    ucn_node_id_t best_node = cluster->config.local_node_id;
    uint16_t best_score = cluster->config.head_score;
    ucn_cluster_phase_t target_phase;

    for (index = 0U; index < UCN_CLUSTER_MAX_CANDIDATES; ++index) {
        const ucn_cluster_candidate_t *candidate = &cluster->candidates[index];

        if (!candidate->occupied ||
            ucn_deadline_expired(now_ms, candidate->expires_at_ms) ||
            candidate->role != UCN_CLUSTER_ROLE_CANDIDATE) {
            continue;
        }
        if (candidate_better(candidate->head_score, candidate->head_node_id,
                             best_score, best_node)) {
            best_score = candidate->head_score;
            best_node = candidate->head_node_id;
        }
    }
    if (best_node != cluster->config.local_node_id) {
        /* CLV2-01-04b.2 loss path (human TRAP 1): transition FIRST,
         * set_detached() second - set_detached() writes role=DETACHED,
         * which would violate the pre-transition derive discipline if it
         * ran before the transition.  After the transition its role
         * rewrite is redundant-but-harmless; its epoch/vote/lease/
         * deadline/grace/known_backup clears stay site-owned. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_ELECTION,
                               UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                               UCN_CLUSTER_REASON_ELECTION_LOST,
                               now_ms) != UCN_OK) {
            /* Fail closed: rejected transition leaves every field
             * untouched - do not run the detach side effects. */
            return;
        }
        set_detached(cluster, now_ms, cluster->config.observation_ms);
#if !defined(NDEBUG)
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
#endif
        return;
    }
    /* CLV2-01-04b.2 win path (human TRAP 2): the destination HEAD
     * sub-phase is dispatched from the PRE-CALL preserved backup_* state
     * (read BEFORE the transition - apply_legacy(HEAD_NO_BACKUP) forces
     * backup_node_id=0, so post-call reads would be wrong). */
    if (cluster->backup_node_id == 0U) {
        target_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    } else if (cluster->backup_ready) {
        target_phase = UCN_CLUSTER_PHASE_HEAD_STABLE;
    } else if (cluster->backup_assign_pending) {
        target_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
    } else {
        target_phase = UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    }
    if (cluster_transition(cluster, UCN_CLUSTER_PHASE_ELECTION,
                           target_phase, UCN_CLUSTER_REASON_ELECTION_WON,
                           now_ms) != UCN_OK) {
        /* Fail closed: do not run the win side effects on a non-HEAD
         * node. */
        return;
    }
    cluster->role_since_ms = now_ms;
    cluster->election_deadline_ms = 0U;
    cluster->next_advertise_ms = now_ms;
    cluster->stats.elections_won++;
#if !defined(NDEBUG)
    assert(cluster_phase_from_legacy_state(cluster, now_ms) == target_phase);
#endif
}

static size_t admitted_peer_count(const ucn_cluster_t *cluster)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < UCN_CLUSTER_MAX_PEERS; ++index) {
        if (cluster->peers[index].occupied &&
            cluster->peers[index].neighbor_state == UCN_NEIGHBOR_ADMITTED) {
            ++count;
        }
    }
    return count;
}

/* Head-side Backup control: assignment announcement, heartbeat and one
 * snapshot record per step.  The cursor advances only after the message has
 * entered the normal UCN send path, so Token Bucket back-pressure never
 * silently loses the selected Backup identity. */
static uint32_t backup_control_spacing_ms(const ucn_cluster_t *cluster,
                                          size_t member_count)
{
    uint32_t divisor = member_count == 0U ? 1U : (uint32_t)member_count;
    uint32_t spacing = cluster->config.lease_ms / divisor;

    if (spacing < cluster->config.token_bucket.refill_ms) {
        spacing = cluster->config.token_bucket.refill_ms;
    }
    return spacing;
}

static void start_backup_assignment_cycle(ucn_cluster_t *cluster,
                                          uint32_t now_ms)
{
    size_t index;

    cluster->backup_assign_cursor = 0U;
    cluster->backup_assign_remaining = (uint8_t)member_count_u16(cluster);
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
}

static void queue_backup_assignment_for_member(ucn_cluster_t *cluster,
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
            /* A newly admitted member gets a retriable targeted assignment.
             * Do not restart a complete sweep for harmless JOIN retries. */
            cluster->backup_assign_cursor = (uint8_t)index;
            cluster->backup_assign_remaining = 1U;
            cluster->backup_assign_pending = true;
            cluster->next_backup_assign_ms = now_ms;
            return;
        }
    }
}

static void send_backup_assignment_step(ucn_cluster_t *cluster,
                                        uint32_t now_ms)
{
    size_t examined;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        cluster->backup_node_id == 0U || !cluster->backup_assign_pending ||
        cluster->backup_assign_remaining == 0U) {
        return;
    }
    if (cluster->next_backup_assign_ms != 0U &&
        !ucn_deadline_expired(now_ms, cluster->next_backup_assign_ms)) {
        return;
    }
    for (examined = 0U; examined < UCN_CLUSTER_MAX_MEMBERS; ++examined) {
        size_t index = (cluster->backup_assign_cursor + examined) %
                       UCN_CLUSTER_MAX_MEMBERS;

        if (cluster->members[index].occupied) {
            ucn_result_t result = send_backup_assign(
                cluster, cluster->members[index].node_id);

            if (result == UCN_OK) {
                cluster->backup_assign_cursor =
                    (uint8_t)((index + 1U) % UCN_CLUSTER_MAX_MEMBERS);
                cluster->backup_assign_remaining--;
                if (cluster->backup_assign_remaining == 0U) {
                    cluster->backup_assign_pending = false;
                    cluster->next_backup_assign_ms = ucn_deadline_from_now(
                        now_ms, cluster->config.lease_ms);
                } else {
                    cluster->next_backup_assign_ms = ucn_deadline_from_now(
                        now_ms, backup_control_spacing_ms(
                                    cluster, member_count_u16(cluster)));
                }
            } else {
                cluster->next_backup_assign_ms = ucn_deadline_from_now(
                    now_ms, cluster->config.token_bucket.refill_ms);
            }
            return;
        }
    }
    cluster->backup_assign_pending = false;
    cluster->backup_assign_remaining = 0U;
    cluster->next_backup_assign_ms = ucn_deadline_from_now(
        now_ms, cluster->config.lease_ms);
}

/* Head-side Backup control: heartbeat + one snapshot record per step. */
static void send_backup_heartbeat(ucn_cluster_t *cluster, uint32_t now_ms)
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
static void send_backup_delta_step(ucn_cluster_t *cluster)
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
        return;
    }
    member_count = member_count_u16(cluster);
    if (member_count == 0U) {
        return;
    }
    now_ms = cluster_now(cluster);
    if (cluster->next_backup_delta_ms == 0U) {
        cluster->next_backup_delta_ms = now_ms;
        return;
    }
    if (!ucn_deadline_expired(now_ms, cluster->next_backup_delta_ms)) {
        return;
    }
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->config.local_node_id;
    message.backup_generation = cluster->backup_generation;
    message.flags = UCN_CLUSTER_FLAG_SYNC_DELTA;
    next_sequence = cluster->membership_sequence == UINT32_MAX ?
                    1U : cluster->membership_sequence + 1U;
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
}

static uint32_t backup_sync_spacing_ms(const ucn_cluster_t *cluster,
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

static void send_backup_snapshot_step(ucn_cluster_t *cluster)
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
        return;
    }
    now_ms = cluster_now(cluster);
    if (cluster->next_backup_sync_ms != 0U &&
        !ucn_deadline_expired(now_ms, cluster->next_backup_sync_ms)) {
        return;
    }
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->config.local_node_id;
    message.backup_generation = cluster->backup_generation;
    /* Build the next frame without committing sequence/cursor yet. */
    next_sequence = cluster->membership_sequence == UINT32_MAX ?
                    1U : cluster->membership_sequence + 1U;
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
}

/* Reset a running snapshot so the Backup converges to the current members. */
static void backup_resync(ucn_cluster_t *cluster)
{
    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        cluster->backup_node_id == 0U) {
        return;
    }
    cluster->backup_sync_cursor = 0U;
    cluster->backup_ready = false;
    cluster->next_backup_sync_ms = cluster_now(cluster);
    cluster->backup_resync_deadline_ms = ucn_deadline_from_now(
        cluster_now(cluster), cluster->config.lease_ms);
}

static void send_next_advertisement(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t examined;
    size_t peer_count = admitted_peer_count(cluster);
    uint32_t slice_ms;

    if (peer_count == 0U) {
        cluster->next_advertise_ms = ucn_deadline_from_now(
            now_ms, cluster->config.advertise_interval_ms);
        return;
    }
    for (examined = 0U; examined < UCN_CLUSTER_MAX_PEERS; ++examined) {
        size_t index = (cluster->advertise_cursor + examined) %
                       UCN_CLUSTER_MAX_PEERS;
        const ucn_cluster_peer_t *peer = &cluster->peers[index];

        if (!peer->occupied ||
            peer->neighbor_state != UCN_NEIGHBOR_ADMITTED) {
            continue;
        }
        (void)send_message(cluster, peer->node_id, UCN_CLUSTER_MSG_ADVERTISE,
                           cluster->role, cluster->cluster_id, cluster->term,
                           cluster->config.local_node_id,
                           cluster->config.head_score,
                           cluster->role == UCN_CLUSTER_ROLE_HEAD ?
                               available_capacity(cluster) :
                               cluster->config.member_capacity);
        cluster->advertise_cursor =
            (uint8_t)((index + 1U) % UCN_CLUSTER_MAX_PEERS);
        break;
    }
    /* The advertised interval is one full peer refresh cycle.  C07.4's
     * throttled Backup snapshot keeps this normal discovery traffic from
     * being starved, while preserving enough repeated Head offers for a
     * large lossy cluster to converge within one lease. */
    slice_ms = cluster->config.advertise_interval_ms / (uint32_t)peer_count;
    if (slice_ms == 0U) {
        slice_ms = 1U;
    }
    cluster->next_advertise_ms = ucn_deadline_from_now(now_ms, slice_ms);
}

static void expire_members(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    bool changed = false;

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
}

static void send_join_request(ucn_cluster_t *cluster, uint32_t now_ms)
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

static void send_keepalive(ucn_cluster_t *cluster, uint32_t now_ms)
{
    (void)send_message(cluster, cluster->head_node_id,
                       UCN_CLUSTER_MSG_KEEPALIVE, UCN_CLUSTER_ROLE_MEMBER,
                       cluster->cluster_id, cluster->term,
                       cluster->head_node_id, cluster->current_head_score, 0U);
    cluster->next_keepalive_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
}

static ucn_result_t ucn_cluster_step_inner(ucn_cluster_t *cluster)
{
    uint32_t now_ms;

    if (cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!cluster->config.enabled) {
        return UCN_OK;
    }
    now_ms = cluster_now(cluster);
    if (cluster->role == UCN_CLUSTER_ROLE_MEMBER &&
        ucn_deadline_expired(now_ms, cluster->head_lease_expires_at_ms)) {
        /* Grace period: a Backup may be mid-takeover exactly when the
         * member lease lapses; stay MEMBER briefly so the member can still
         * ACK TAKEOVER_PREPARE / switch on HEAD_TAKEOVER before detaching. */
        if (cluster->head_grace_deadline_ms == 0U) {
            /* CLV2-01-04c.1: arming the grace deadline IS the
             * MEMBER_ACTIVE -> MEMBER_TAKEOVER_GRACE transition - call it
             * BEFORE any phase-relevant write.  apply_legacy(GRACE)
             * writes role + the deadline_from_now(now, keepalive) the
             * site used (verified identical), so the site rewrite below
             * is idempotent with the same value (kept authoritative,
             * original order). */
            if (cluster_transition(cluster,
                                   UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                                   UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                                   UCN_CLUSTER_REASON_HEAD_LEASE_EXPIRED,
                                   now_ms) != UCN_OK) {
                /* Fail closed: a rejected transition (shadow mismatch /
                 * illegal pair / pre-mutated phase fields) leaves every
                 * field untouched - do NOT arm grace or count the expiry;
                 * the next Step re-visits the lease check. */
                return UCN_ERR_STATE;
            }
            cluster->stats.head_leases_expired++;
            cluster->head_grace_deadline_ms = ucn_deadline_from_now(
                now_ms, cluster->config.keepalive_interval_ms);
#if !defined(NDEBUG)
            /* CLV2-01-04c.1 post-commit derive assert: after the
             * transition AND the site's deadline write the node must
             * still derive MEMBER_TAKEOVER_GRACE (role == MEMBER with an
             * armed grace deadline). */
            assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                   UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
#endif
        } else if (ucn_deadline_expired(now_ms,
                                       cluster->head_grace_deadline_ms)) {
            /* CLV2-01-04c.3: the grace timeout IS the
             * MEMBER_TAKEOVER_GRACE -> RECOVERY_OBSERVE transition - call
             * it FIRST.  apply_legacy(RECOVERY_OBSERVE) writes role=
             * DETACHED + eligible=true + backoff=0 + grace=0 +
             * known_backup_*=0, so the site's eligible=true rewrite below
             * is redundant-but-harmless and set_detached() re-applies the
             * same epoch/vote/lease/grace clears with the observation
             * deadline.  The pre-derive requires the legacy to STILL
             * derive MEMBER_TAKEOVER_GRACE (eligible must stay false), so
             * recovery_eligible is written only AFTER the call. */
            if (cluster_transition(cluster,
                                   UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                                   UCN_CLUSTER_PHASE_RECOVERY_OBSERVE,
                                   UCN_CLUSTER_REASON_GRACE_TIMEOUT,
                                   now_ms) != UCN_OK) {
                /* Fail closed: do NOT detach on a rejected transition;
                 * the node stays in grace and the next Step re-visits. */
                return UCN_ERR_STATE;
            }
            cluster->recovery_eligible = true;
            set_detached(cluster, now_ms,
                         cluster->config.recovery_observation_ms);
#if !defined(NDEBUG)
            /* CLV2-01-04c.3 post-commit derive assert: after the
             * transition AND every site effect the node must derive
             * RECOVERY_OBSERVE (role == DETACHED, eligible, no backoff). */
            assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
                   UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
#endif
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_HEAD) {
        expire_members(cluster, now_ms);
        if (cluster->next_advertise_ms == 0U ||
            ucn_deadline_expired(now_ms, cluster->next_advertise_ms)) {
            /* Discovery / lease renewal has priority over background backup
             * replication, otherwise a lossy large cluster can exhaust its
             * own control budget before pending JOINs are observed. */
            send_next_advertisement(cluster, now_ms);
        }
        if (cluster->backup_node_id == 0U) {
            /* C07.7 P1: automatically re-select a Backup when the previous
             * one left or expired, even without a new JOIN; the selection
             * itself stays candidate/score ordered and idempotent. */
            assign_backup(cluster, now_ms);
        }
        if (cluster->backup_node_id != 0U && !cluster->backup_assign_pending &&
            (cluster->next_backup_assign_ms == 0U ||
             ucn_deadline_expired(now_ms,
                                  cluster->next_backup_assign_ms))) {
            start_backup_assignment_cycle(cluster, now_ms);
        }
        send_backup_delta_step(cluster);
        send_backup_heartbeat(cluster, now_ms);
        send_takeover_announce_step(cluster);
        send_backup_assignment_step(cluster, now_ms);
        send_backup_snapshot_step(cluster);
        /* Bounded snapshot retransmit: if a frame was dropped the Backup
         * never becomes READY, so resend the snapshot on a fixed timer. */
        if (cluster->backup_node_id != 0U && !cluster->backup_ready &&
            cluster->backup_sync_cursor > member_count_u16(cluster) + 1U &&
            ucn_deadline_expired(now_ms,
                                  cluster->backup_resync_deadline_ms)) {
            /* The snapshot completed but a frame was dropped: resend it. */
            backup_resync(cluster);
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP &&
        cluster->backup_primary_deadline_ms != 0U &&
        ucn_deadline_expired(now_ms, cluster->backup_primary_deadline_ms)) {
        cluster->backup_missed_heartbeats++;
        if (cluster->backup_missed_heartbeats >= UCN_CLUSTER_BACKUP_MISS_LIMIT) {
            if (cluster->backup_ready && !cluster->backup_takeover_active) {
                if (ucn_deadline_expired(
                        now_ms, cluster->backup_primary_lease_deadline_ms)) {
                    /* §5.1: also wait for the Primary lease to expire so
                     * a burst of dropped heartbeats cannot preempt a live
                     * Head. */
                    start_takeover(cluster, now_ms);
                }
                /* else: keep waiting for the lease; stay BACKUP. */
            } else if (!cluster->backup_takeover_active) {
                cluster->stats.head_leases_expired++;
                cluster->recovery_eligible = true;
                backup_clear_sync(cluster, now_ms);
                return UCN_OK;
            }
        }
        cluster->backup_primary_deadline_ms =
            ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP &&
        cluster->backup_takeover_active &&
        ucn_deadline_expired(now_ms, cluster->backup_takeover_deadline_ms)) {
        /* Majority not reached in the window -> fall back to re-election. */
        cluster->stats.head_leases_expired++;
        backup_clear_sync(cluster, now_ms);
        return UCN_OK;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP &&
        cluster->backup_takeover_active) {
        send_takeover_prepare_step(cluster);
    }
    if (cluster->role == UCN_CLUSTER_ROLE_DETACHED &&
        cluster->config.head_capable &&
         ucn_deadline_expired(now_ms, cluster->observation_deadline_ms)) {
        if (cluster->recovery_eligible &&
            cluster->recovery_cooldown_until_ms != 0U &&
            !ucn_deadline_expired(now_ms,
                                  cluster->recovery_cooldown_until_ms)) {
            /* Still cooling down after a Recovery stepdown: wait. */
        } else if (cluster->recovery_eligible) {
            if (cluster->recovery_backoff_deadline_ms == 0U) {
                start_recovery_backoff(cluster, now_ms);
            } else if (ucn_deadline_expired(
                           now_ms, cluster->recovery_backoff_deadline_ms)) {
                if (recovery_quorum_met(cluster)) {
                    declare_recovery_head(cluster, now_ms);
                } else {
                    /* C07.7 P0-2: no visible quorum (fully isolated node):
                     * do NOT self-declare; retry after another bounded
                     * backoff so the domain converges when peers return. */
                    cluster->recovery_backoff_deadline_ms =
                        ucn_deadline_from_now(
                            now_ms, cluster->config.recovery_backoff_max_ms);
                }
            }
        } else {
            start_election(cluster, now_ms);
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_CANDIDATE &&
        ucn_deadline_expired(now_ms, cluster->election_deadline_ms)) {
        complete_election(cluster, now_ms);
    }
    if (cluster->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
        /* C07.7 P0-1: the recovery Cluster is a real (short-lived) Cluster:
         * expire members and periodically re-advertise the declaration so
         * surviving members keep their leases and late survivors can join. */
        expire_members(cluster, now_ms);
        if (cluster->next_advertise_ms == 0U ||
            ucn_deadline_expired(now_ms, cluster->next_advertise_ms)) {
            cluster->next_advertise_ms = ucn_deadline_from_now(
                now_ms, cluster->config.advertise_interval_ms);
            (void)send_recovery_declare(cluster);
        }
        if (ucn_deadline_expired(now_ms, cluster->recovery_deadline_ms)) {
            stepdown_recovery_head(cluster, now_ms);
            return UCN_OK;
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_STEPPING_DOWN &&
        ucn_deadline_expired(now_ms, cluster->stepdown_deadline_ms)) {
        /* Ordered switchback completes: leave members, join the better
         * Head that was already announced via HEAD_STEPDOWN. */
        clear_members(cluster);
        cluster->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
        cluster->role_since_ms = now_ms;
        cluster->next_join_retry_ms = now_ms;
        cluster->stepdown_deadline_ms = 0U;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_CANDIDATE &&
        (cluster->next_advertise_ms == 0U ||
         ucn_deadline_expired(now_ms, cluster->next_advertise_ms))) {
        send_next_advertisement(cluster, now_ms);
    }
    if (cluster->role == UCN_CLUSTER_ROLE_JOIN_PENDING &&
        (cluster->next_join_retry_ms == 0U ||
         ucn_deadline_expired(now_ms, cluster->next_join_retry_ms))) {
        send_join_request(cluster, now_ms);
    }
    if ((cluster->role == UCN_CLUSTER_ROLE_MEMBER ||
         cluster->role == UCN_CLUSTER_ROLE_BACKUP) &&
        (cluster->next_keepalive_ms == 0U ||
         ucn_deadline_expired(now_ms, cluster->next_keepalive_ms))) {
        send_keepalive(cluster, now_ms);
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_step(ucn_cluster_t *cluster)
{
    ucn_result_t result = ucn_cluster_step_inner(cluster);

    /* CLV2-01-02: keep the shadow phase mirror aligned after every
     * Step.  The mirror never drives behaviour; it only exists for the
     * consistency gate. */
    if (result == UCN_OK && cluster != NULL && cluster->config.enabled) {
        cluster_shadow_sync(cluster, UCN_CLUSTER_REASON_UNKNOWN);
    }
    return result;
}

ucn_cluster_role_t ucn_cluster_get_role(const ucn_cluster_t *cluster)
{
    return cluster == NULL ? UCN_CLUSTER_ROLE_DISABLED : cluster->role;
}

size_t ucn_cluster_member_count(const ucn_cluster_t *cluster)
{
    return cluster == NULL ? 0U : member_count_u16(cluster);
}

ucn_result_t ucn_cluster_get_view(const ucn_cluster_t *cluster,
                                  ucn_cluster_view_t *view)
{
    if (cluster == NULL || view == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    view->enabled = cluster->config.enabled;
    view->role = cluster->role;
    view->local_node_id = cluster->config.local_node_id;
    view->cluster_id = cluster->cluster_id;
    view->term = cluster->term;
    view->head_node_id = cluster->head_node_id;
    view->current_head_score = cluster->current_head_score;
    return UCN_OK;
}

size_t ucn_cluster_copy_member_summaries(
    const ucn_cluster_t *cluster,
    ucn_cluster_member_summary_t *output,
    size_t capacity)
{
    size_t index;
    size_t count = 0U;

    if (cluster == NULL || (output == NULL && capacity != 0U)) {
        return 0U;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        const ucn_cluster_member_t *member = &cluster->members[index];

        if (!member->occupied) {
            continue;
        }
        if (output == NULL) {
            ++count;
            continue;
        }
        if (count >= capacity) {
            break;
        }
        output[count].node_id = member->node_id;
        output[count].lease_expires_at_ms = member->lease_expires_at_ms;
        ++count;
    }
    return count;
}

ucn_result_t ucn_cluster_get_member_summary_at(
    const ucn_cluster_t *cluster,
    size_t table_index,
    ucn_cluster_member_summary_t *summary)
{
    const ucn_cluster_member_t *member;

    if (cluster == NULL || summary == NULL ||
        table_index >= UCN_CLUSTER_MAX_MEMBERS) {
        return UCN_ERR_ARGUMENT;
    }
    member = &cluster->members[table_index];
    if (!member->occupied) {
        return UCN_ERR_NOT_FOUND;
    }
    summary->node_id = member->node_id;
    summary->lease_expires_at_ms = member->lease_expires_at_ms;
    return UCN_OK;
}

const ucn_cluster_stats_t *ucn_cluster_get_stats(const ucn_cluster_t *cluster)
{
    return cluster == NULL ? NULL : &cluster->stats;
}
