/* UCN CLV2-M02 (02-02): Cluster control-plane Codec module.
 *
 * STRUCTURAL REFACTOR ONLY (M02 mandate): this module is the 32-byte
 * Wire v5 (Format v3) encode/decode extracted verbatim from the former
 * single ucn_cluster.c.  Byte layout, validation rules and every
 * function body are UNCHANGED - the codec golden vectors and the
 * golden trace prove byte-identical output.  Do not "optimize" any
 * wire byte or validation here; M02 is a pure structure change.
 */

#include "ucn/ucn_cluster.h"

#include <string.h>

#include "ucn/ucn_time.h"


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
        /* CLV2-M12 (12-04): previously zeroed; now carries the declaring
         * Head's parent lineage identity for same-parent rank. */
        write_u32_be(output + 8U, message->recovery_parent_cluster_id);
        break;
    case UCN_CLUSTER_MSG_RECOVERY_ACK:
        /* CLV2-M12 (12-06): the ACK echoes the declare round (nonce) and
         * the lineage binding (parent) so old-round ACKs are replayable. */
        write_u32_be(output + 0U, message->recovery_nonce);
        write_u32_be(output + 4U, message->recovery_parent_cluster_id);
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
        /* CLV2-M12 (12-04): parent lineage identity; old frames read 0. */
        message->recovery_parent_cluster_id = read_u32_be(input + 8U);
        break;
    case UCN_CLUSTER_MSG_RECOVERY_ACK:
        /* CLV2-M12 (12-06): round echo + lineage binding; old ACK frames
         * decode as 0 and keep the legacy-tolerant path. */
        message->recovery_nonce = read_u32_be(input + 0U);
        message->recovery_parent_cluster_id = read_u32_be(input + 4U);
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
