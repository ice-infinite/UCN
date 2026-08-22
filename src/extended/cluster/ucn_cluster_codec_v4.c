/* CLV2-05-02: isolated RFC4 Cluster Wire codec.
 *
 * This file deliberately has no dependency on Cluster FSM internals and no
 * caller from the production Cluster receive/send path.  It supplies strict
 * byte-level validation, version dispatch, bounded certificate fragment
 * storage, and M05-03 private semantic builders. Later milestones own any
 * FSM admission.
 */

#include "ucn_cluster_wire_v4_semantic.h"

#include <string.h>

#include "ucn/ucn_time.h"

#define V4_VERSION_OFFSET ((size_t)0U)
#define V4_TYPE_OFFSET ((size_t)1U)
#define V4_ROLE_OFFSET ((size_t)2U)
#define V4_FLAGS_OFFSET ((size_t)3U)
#define V4_CLUSTER_ID_OFFSET ((size_t)4U)
#define V4_TERM_OFFSET ((size_t)8U)
#define V4_HEAD_ID_OFFSET ((size_t)12U)
#define V4_WORDS_OFFSET ((size_t)16U)

#define V4_CERT_SET_OLD_INDEX ((size_t)0U)
#define V4_CERT_SET_NEW_INDEX ((size_t)1U)
#define V4_CERT_REQUIRED_OLD ((uint32_t)0x01U)
#define V4_CERT_REQUIRED_NEW ((uint32_t)0x02U)

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

#if UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED
static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}
#endif

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool cluster_id_is_valid(uint32_t cluster_id)
{
    return cluster_id != 0U && cluster_id != UCN_NODE_BROADCAST;
}

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool nonce_is_valid(uint32_t value)
{
    return value != 0U;
}

bool ucn_cluster_wire_v4_certificate_admission_is_valid(
    const ucn_cluster_wire_v4_certificate_admission_t *admission)
{
    return admission != NULL && admission->source_admitted &&
           admission->frozen_config_admitted &&
           node_id_is_valid(admission->outer_source) &&
           serial_is_valid(admission->old_config_id) &&
           (admission->new_config_id == 0U ||
            (serial_is_valid(admission->new_config_id) &&
             admission->new_config_id != admission->old_config_id));
}

static bool role_is_one_of(ucn_cluster_role_t role,
                           ucn_cluster_role_t first,
                           ucn_cluster_role_t second)
{
    return role == first || role == second;
}

static bool score_capacity_is_valid(uint32_t value)
{
    return (uint16_t)(value >> 16U) <= UCN_CLUSTER_SCORE_MAX;
}

static bool capability_bitmap_is_valid(uint32_t value)
{
    return (value & UINT32_C(0xFFFF0000)) == 0U &&
           (value & (uint32_t)~UINT32_C(0x003F)) == 0U;
}

static bool wire_offer_is_valid(uint32_t value)
{
    uint8_t minimum = (uint8_t)(value >> 24U);
    uint8_t maximum = (uint8_t)(value >> 16U);

    return minimum != 0U && maximum != 0U && minimum <= maximum &&
           minimum <= UCN_CLUSTER_WIRE_V4_FORMAT_VERSION &&
           maximum >= UCN_CLUSTER_WIRE_V4_FORMAT_VERSION &&
           capability_bitmap_is_valid(value & UINT32_C(0xFFFF));
}

static bool selected_wire_offer_is_valid(uint32_t value)
{
    uint8_t selected = (uint8_t)(value >> 16U);

    return (value & UINT32_C(0xFF000000)) == 0U && selected != 0U &&
           capability_bitmap_is_valid(value & UINT32_C(0xFFFF));
}

static bool reason_is_valid(uint32_t value)
{
    return (value & UINT32_C(0xFFFFFF00)) == 0U && value >= 1U &&
           value <= 10U;
}

static bool voter_count_is_valid(uint32_t count)
{
    return count >= 1U && count <= (uint32_t)UCN_CLUSTER_MAX_MEMBERS + 1U;
}

static bool voter_count_pair_is_valid(uint32_t value)
{
    return voter_count_is_valid(value >> 16U) &&
           voter_count_is_valid(value & UINT32_C(0xFFFF));
}

static bool ordinal_count_is_valid(uint32_t value)
{
    uint32_t ordinal = value >> 16U;
    uint32_t total = value & UINT32_C(0xFFFF);

    return total >= 1U && total <= (uint32_t)UCN_CLUSTER_MAX_MEMBERS &&
           ordinal < total;
}

static bool config_phase_is_valid(uint32_t value)
{
    return (value & UINT32_C(0xFFFFFF00)) == 0U && value >= 1U &&
           value <= 3U;
}

static bool type_is_valid(uint8_t type)
{
    return type >= UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE &&
           type <= UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE;
}

static bool flags_are_valid(const ucn_cluster_wire_v4_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }
    switch (frame->type) {
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC:
        return frame->flags == 0U ||
               frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN ||
               frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END ||
               frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_DELTA;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
        return frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_ADD ||
               frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_REMOVE;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        return frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD ||
               frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW;
    default:
        return frame->flags == 0U;
    }
}

static bool role_is_valid_for_type(const ucn_cluster_wire_v4_frame_t *frame)
{
    uint32_t target_cluster_id;

    if (frame == NULL) {
        return false;
    }
    switch (frame->type) {
    case UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE:
        return role_is_one_of(frame->role, UCN_CLUSTER_ROLE_CANDIDATE,
                              UCN_CLUSTER_ROLE_HEAD);
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST:
        return frame->role == UCN_CLUSTER_ROLE_JOIN_PENDING;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT:
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT:
        return role_is_one_of(frame->role, UCN_CLUSTER_ROLE_HEAD,
                              UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    case UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE:
    case UCN_CLUSTER_WIRE_V4_MSG_LEAVE:
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK:
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK:
        return frame->role == UCN_CLUSTER_ROLE_MEMBER;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE:
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER:
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN:
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN:
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC:
    case UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT:
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN:
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT:
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT:
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT:
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW:
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT:
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        return frame->role == UCN_CLUSTER_ROLE_HEAD;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY:
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ:
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT:
        return frame->role == UCN_CLUSTER_ROLE_BACKUP;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK:
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK:
        return role_is_one_of(frame->role, UCN_CLUSTER_ROLE_MEMBER,
                              UCN_CLUSTER_ROLE_BACKUP);
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE:
        return frame->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY:
        target_cluster_id = frame->words[1U];
        return target_cluster_id == frame->cluster_id ?
                   frame->role == UCN_CLUSTER_ROLE_BACKUP :
                   frame->role == UCN_CLUSTER_ROLE_HEAD;
    default:
        return false;
    }
}

static bool handover_fields_are_valid(const ucn_cluster_wire_v4_frame_t *frame,
                                      bool has_config)
{
    if (!serial_is_valid(frame->words[0U]) ||
        !cluster_id_is_valid(frame->words[1U]) ||
        !serial_is_valid(frame->words[2U]) ||
        !node_id_is_valid(frame->words[3U])) {
        return false;
    }
    if (has_config) {
        return serial_is_valid(frame->words[4U]) &&
               nonce_is_valid(frame->words[5U]);
    }
    return nonce_is_valid(frame->words[4U]) && frame->words[5U] == 0U;
}

static bool rekey_fields_are_valid(const ucn_cluster_wire_v4_frame_t *frame,
                                   bool has_persistence_generation)
{
    if (!cluster_id_is_valid(frame->words[0U]) || frame->words[0U] == frame->cluster_id ||
        frame->words[1U] != 1U || !serial_is_valid(frame->words[2U]) ||
        !serial_is_valid(frame->words[3U]) || !serial_is_valid(frame->words[4U])) {
        return false;
    }
    return has_persistence_generation ? serial_is_valid(frame->words[4U]) &&
                                              nonce_is_valid(frame->words[5U]) :
                                        nonce_is_valid(frame->words[5U]);
}

bool ucn_cluster_wire_v4_frame_is_valid(
    const ucn_cluster_wire_v4_frame_t *frame)
{
    uint32_t descriptor;
    uint32_t fragment_index;
    uint32_t fragment_count;

    if (frame == NULL || !type_is_valid(frame->type) ||
        !flags_are_valid(frame) || !cluster_id_is_valid(frame->cluster_id) ||
        !serial_is_valid(frame->term) || !node_id_is_valid(frame->head_node_id) ||
        !role_is_valid_for_type(frame)) {
        return false;
    }

    switch (frame->type) {
    case UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE:
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE:
        return score_capacity_is_valid(frame->words[0U]) &&
               ucn_duration_is_valid(frame->words[1U]) &&
               nonce_is_valid(frame->words[2U]) &&
               wire_offer_is_valid(frame->words[3U]) && frame->words[4U] == 0U &&
               frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST:
        return serial_is_valid(frame->words[0U]) &&
               wire_offer_is_valid(frame->words[1U]) &&
               (frame->words[2U] == 0U || serial_is_valid(frame->words[2U])) &&
               serial_is_valid(frame->words[3U]) &&
               score_capacity_is_valid(frame->words[4U]) &&
               nonce_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT:
        return serial_is_valid(frame->words[0U]) &&
               serial_is_valid(frame->words[1U]) &&
               ucn_duration_is_valid(frame->words[2U]) &&
               (frame->words[3U] & (uint32_t)~UINT32_C(0x01)) == 0U &&
               selected_wire_offer_is_valid(frame->words[4U]) &&
               nonce_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT:
        return nonce_is_valid(frame->words[0U]) && reason_is_valid(frame->words[1U]) &&
               ucn_duration_is_valid(frame->words[2U]) && frame->words[3U] == 0U &&
               frame->words[4U] == 0U && frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE:
        return ucn_duration_is_valid(frame->words[0U]) &&
               nonce_is_valid(frame->words[1U]) && frame->words[2U] == 0U &&
               frame->words[3U] == 0U && frame->words[4U] == 0U &&
               frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_LEAVE:
        return nonce_is_valid(frame->words[0U]) && reason_is_valid(frame->words[1U]) &&
               frame->words[2U] == 0U && frame->words[3U] == 0U &&
               frame->words[4U] == 0U && frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && serial_is_valid(frame->words[3U]) &&
               (frame->words[4U] == V4_CERT_REQUIRED_OLD ||
                frame->words[4U] == (V4_CERT_REQUIRED_OLD | V4_CERT_REQUIRED_NEW));
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN:
        return handover_fields_are_valid(frame, false);
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN:
        return serial_is_valid(frame->words[0U]) &&
               node_id_is_valid(frame->words[1U]) && serial_is_valid(frame->words[2U]) &&
               serial_is_valid(frame->words[3U]) && nonce_is_valid(frame->words[4U]) &&
               frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && serial_is_valid(frame->words[3U]) &&
               nonce_is_valid(frame->words[4U]) && nonce_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC:
        if (!serial_is_valid(frame->words[0U]) || !serial_is_valid(frame->words[1U]) ||
            !serial_is_valid(frame->words[2U])) {
            return false;
        }
        if (frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN ||
            frame->flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END) {
            return frame->words[3U] == 0U && frame->words[4U] == 0U &&
                   frame->words[5U] == 0U;
        }
        return node_id_is_valid(frame->words[3U]) && nonce_is_valid(frame->words[4U]) &&
               ucn_duration_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && serial_is_valid(frame->words[3U]) &&
               ucn_duration_is_valid(frame->words[4U]) &&
               nonce_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && serial_is_valid(frame->words[3U]) &&
               serial_is_valid(frame->words[4U]) && nonce_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE:
        return cluster_id_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && serial_is_valid(frame->words[3U]) &&
               nonce_is_valid(frame->words[4U]) && ucn_duration_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK:
        return nonce_is_valid(frame->words[0U]) && nonce_is_valid(frame->words[1U]) &&
               frame->words[2U] == 0U && frame->words[3U] == 0U &&
               frame->words[4U] == 0U && frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && nonce_is_valid(frame->words[3U]) &&
               frame->words[4U] == 0U && frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               reason_is_valid(frame->words[2U]) && nonce_is_valid(frame->words[3U]) &&
               frame->words[4U] == 0U && frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && nonce_is_valid(frame->words[3U]) &&
               nonce_is_valid(frame->words[4U]) && nonce_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               node_id_is_valid(frame->words[2U]) && nonce_is_valid(frame->words[3U]) &&
               capability_bitmap_is_valid(frame->words[4U]) &&
               ordinal_count_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE:
        return serial_is_valid(frame->words[0U]) && nonce_is_valid(frame->words[1U]) &&
               nonce_is_valid(frame->words[2U]) && voter_count_pair_is_valid(frame->words[3U]) &&
               serial_is_valid(frame->words[4U]) && nonce_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               frame->words[2U] < (uint32_t)UCN_CLUSTER_MAX_MEMBERS + 1U &&
               config_phase_is_valid(frame->words[3U]) && serial_is_valid(frame->words[4U]) &&
               nonce_is_valid(frame->words[5U]);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               nonce_is_valid(frame->words[2U]) && voter_count_is_valid(frame->words[3U]) &&
               nonce_is_valid(frame->words[4U]) && frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT:
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && reason_is_valid(frame->words[3U]) &&
               nonce_is_valid(frame->words[4U]) && frame->words[5U] == 0U;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY:
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT:
        return handover_fields_are_valid(frame, true);
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW:
        return handover_fields_are_valid(frame, false);
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT:
        return rekey_fields_are_valid(frame, false);
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK:
        return rekey_fields_are_valid(frame, true);
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        descriptor = frame->words[4U];
        fragment_index = descriptor >> 16U;
        fragment_count = descriptor & UINT32_C(0xFFFF);
        return serial_is_valid(frame->words[0U]) && serial_is_valid(frame->words[1U]) &&
               serial_is_valid(frame->words[2U]) && serial_is_valid(frame->words[3U]) &&
               fragment_count >= 1U &&
               fragment_count <= UCN_CLUSTER_WIRE_V4_MAX_CERTIFICATE_FRAGMENTS &&
               fragment_index < fragment_count;
    default:
        return false;
    }
}

ucn_result_t ucn_cluster_wire_v4_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_wire_v4_frame_t *output)
{
    ucn_cluster_wire_v4_frame_t decoded;
    size_t index;

    if (input == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length != UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES ||
        input[V4_VERSION_OFFSET] != UCN_CLUSTER_WIRE_V4_FORMAT_VERSION) {
        return UCN_ERR_MALFORMED;
    }

    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.type = input[V4_TYPE_OFFSET];
    decoded.role = (ucn_cluster_role_t)input[V4_ROLE_OFFSET];
    decoded.flags = input[V4_FLAGS_OFFSET];
    decoded.cluster_id = read_u32_be(input + V4_CLUSTER_ID_OFFSET);
    decoded.term = read_u32_be(input + V4_TERM_OFFSET);
    decoded.head_node_id = read_u32_be(input + V4_HEAD_ID_OFFSET);
    for (index = 0U; index < UCN_CLUSTER_WIRE_V4_WORD_COUNT; ++index) {
        decoded.words[index] = read_u32_be(input + V4_WORDS_OFFSET + index * 4U);
    }
    if (!ucn_cluster_wire_v4_frame_is_valid(&decoded)) {
        return UCN_ERR_MALFORMED;
    }
    *output = decoded;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_encode(
    const ucn_cluster_wire_v4_frame_t *frame,
    uint8_t output[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES])
{
    if (frame == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
#if !UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED
    (void)frame;
    (void)output;
    return UCN_ERR_CONFIG;
#else
    size_t index;

    if (!ucn_cluster_wire_v4_frame_is_valid(frame)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(output, 0, UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES);
    output[V4_VERSION_OFFSET] = UCN_CLUSTER_WIRE_V4_FORMAT_VERSION;
    output[V4_TYPE_OFFSET] = frame->type;
    output[V4_ROLE_OFFSET] = (uint8_t)frame->role;
    output[V4_FLAGS_OFFSET] = frame->flags;
    write_u32_be(output + V4_CLUSTER_ID_OFFSET, frame->cluster_id);
    write_u32_be(output + V4_TERM_OFFSET, frame->term);
    write_u32_be(output + V4_HEAD_ID_OFFSET, frame->head_node_id);
    for (index = 0U; index < UCN_CLUSTER_WIRE_V4_WORD_COUNT; ++index) {
        write_u32_be(output + V4_WORDS_OFFSET + index * 4U, frame->words[index]);
    }
    return UCN_OK;
#endif
}

size_t ucn_cluster_wire_v4_semantic_payload_size(uint8_t type)
{
    switch (type) {
    case UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE:
        return sizeof(ucn_cluster_wire_v4_advertise_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST:
        return sizeof(ucn_cluster_wire_v4_join_request_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT:
        return sizeof(ucn_cluster_wire_v4_join_accept_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT:
        return sizeof(ucn_cluster_wire_v4_join_reject_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE:
        return sizeof(ucn_cluster_wire_v4_keepalive_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_LEAVE:
        return sizeof(ucn_cluster_wire_v4_leave_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE:
        return sizeof(ucn_cluster_wire_v4_head_declare_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER:
        return sizeof(ucn_cluster_wire_v4_head_takeover_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN:
        return sizeof(ucn_cluster_wire_v4_head_stepdown_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN:
        return sizeof(ucn_cluster_wire_v4_backup_assign_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY:
        return sizeof(ucn_cluster_wire_v4_backup_ready_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC:
        return sizeof(ucn_cluster_wire_v4_backup_member_sync_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT:
        return sizeof(ucn_cluster_wire_v4_primary_heartbeat_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK:
        return sizeof(ucn_cluster_wire_v4_takeover_vote_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE:
        return sizeof(ucn_cluster_wire_v4_recovery_declare_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK:
        return sizeof(ucn_cluster_wire_v4_recovery_ack_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ:
        return sizeof(ucn_cluster_wire_v4_backup_resync_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT:
        return sizeof(ucn_cluster_wire_v4_backup_reject_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN:
        return sizeof(ucn_cluster_wire_v4_config_begin_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
        return sizeof(ucn_cluster_wire_v4_config_member_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE:
        return sizeof(ucn_cluster_wire_v4_config_prepare_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK:
        return sizeof(ucn_cluster_wire_v4_config_ack_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT:
        return sizeof(ucn_cluster_wire_v4_config_commit_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT:
        return sizeof(ucn_cluster_wire_v4_config_abort_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY:
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT:
        return sizeof(ucn_cluster_wire_v4_handover_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW:
        return sizeof(ucn_cluster_wire_v4_head_withdraw_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT:
        return sizeof(ucn_cluster_wire_v4_rekey_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK:
        return sizeof(ucn_cluster_wire_v4_rekey_ack_payload_t);
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        return sizeof(ucn_cluster_wire_v4_takeover_certificate_payload_t);
    default:
        return 0U;
    }
}

static void semantic_header_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_semantic_header_t *header)
{
    header->type = frame->type;
    header->role = frame->role;
    header->flags = frame->flags;
    header->cluster_id = frame->cluster_id;
    header->term = frame->term;
    header->head_node_id = frame->head_node_id;
}

static void semantic_header_to_frame(
    const ucn_cluster_wire_v4_semantic_header_t *header,
    ucn_cluster_wire_v4_frame_t *frame)
{
    frame->type = header->type;
    frame->role = header->role;
    frame->flags = header->flags;
    frame->cluster_id = header->cluster_id;
    frame->term = header->term;
    frame->head_node_id = header->head_node_id;
}

ucn_result_t ucn_cluster_wire_v4_semantic_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_semantic_message_t *output)
{
    ucn_cluster_wire_v4_semantic_message_t decoded;

    if (frame == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_wire_v4_frame_is_valid(frame)) {
        return UCN_ERR_MALFORMED;
    }
    (void)memset(&decoded, 0, sizeof(decoded));
    semantic_header_from_frame(frame, &decoded.header);

    switch (frame->type) {
    case UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE:
        decoded.payload.advertise.score_capacity = frame->words[0U];
        decoded.payload.advertise.lease_ms = frame->words[1U];
        decoded.payload.advertise.advertise_nonce = frame->words[2U];
        decoded.payload.advertise.wire_offer = frame->words[3U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST:
        decoded.payload.join_request.join_txid = frame->words[0U];
        decoded.payload.join_request.wire_offer = frame->words[1U];
        decoded.payload.join_request.current_config_id = frame->words[2U];
        decoded.payload.join_request.boot_incarnation = frame->words[3U];
        decoded.payload.join_request.score_capacity = frame->words[4U];
        decoded.payload.join_request.join_nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT:
        decoded.payload.join_accept.join_txid = frame->words[0U];
        decoded.payload.join_accept.target_config_id = frame->words[1U];
        decoded.payload.join_accept.lease_ms = frame->words[2U];
        decoded.payload.join_accept.member_flags = frame->words[3U];
        decoded.payload.join_accept.selected_wire_offer = frame->words[4U];
        decoded.payload.join_accept.member_nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT:
        decoded.payload.join_reject.join_nonce = frame->words[0U];
        decoded.payload.join_reject.reason = frame->words[1U];
        decoded.payload.join_reject.retry_after_ms = frame->words[2U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE:
        decoded.payload.keepalive.lease_ms = frame->words[0U];
        decoded.payload.keepalive.keepalive_nonce = frame->words[1U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_LEAVE:
        decoded.payload.leave.leave_nonce = frame->words[0U];
        decoded.payload.leave.reason = frame->words[1U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE:
        decoded.payload.head_declare.score_capacity = frame->words[0U];
        decoded.payload.head_declare.lease_ms = frame->words[1U];
        decoded.payload.head_declare.declare_nonce = frame->words[2U];
        decoded.payload.head_declare.wire_offer = frame->words[3U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER:
        decoded.payload.head_takeover.backup_generation = frame->words[0U];
        decoded.payload.head_takeover.snapshot_id = frame->words[1U];
        decoded.payload.head_takeover.certificate_anchor_config_id = frame->words[2U];
        decoded.payload.head_takeover.takeover_txid = frame->words[3U];
        decoded.payload.head_takeover.required_set_mask = frame->words[4U];
        decoded.payload.head_takeover.certificate_crc32 = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN:
        decoded.payload.head_stepdown.handover_txid = frame->words[0U];
        decoded.payload.head_stepdown.target_cluster_id = frame->words[1U];
        decoded.payload.head_stepdown.target_term = frame->words[2U];
        decoded.payload.head_stepdown.target_head_node_id = frame->words[3U];
        decoded.payload.head_stepdown.stepdown_nonce = frame->words[4U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN:
        decoded.payload.backup_assign.backup_generation = frame->words[0U];
        decoded.payload.backup_assign.backup_node_id = frame->words[1U];
        decoded.payload.backup_assign.sync_token = frame->words[2U];
        decoded.payload.backup_assign.config_id = frame->words[3U];
        decoded.payload.backup_assign.config_hash = frame->words[4U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY:
        decoded.payload.backup_ready.backup_generation = frame->words[0U];
        decoded.payload.backup_ready.snapshot_id = frame->words[1U];
        decoded.payload.backup_ready.membership_sequence = frame->words[2U];
        decoded.payload.backup_ready.config_id = frame->words[3U];
        decoded.payload.backup_ready.config_hash = frame->words[4U];
        decoded.payload.backup_ready.ready_nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC:
        decoded.payload.backup_member_sync.backup_generation = frame->words[0U];
        decoded.payload.backup_member_sync.snapshot_id = frame->words[1U];
        decoded.payload.backup_member_sync.membership_sequence = frame->words[2U];
        decoded.payload.backup_member_sync.member_node_id = frame->words[3U];
        decoded.payload.backup_member_sync.member_nonce = frame->words[4U];
        decoded.payload.backup_member_sync.member_lease_ms = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT:
        decoded.payload.primary_heartbeat.backup_generation = frame->words[0U];
        decoded.payload.primary_heartbeat.config_id = frame->words[1U];
        decoded.payload.primary_heartbeat.snapshot_id = frame->words[2U];
        decoded.payload.primary_heartbeat.membership_sequence = frame->words[3U];
        decoded.payload.primary_heartbeat.lease_ms = frame->words[4U];
        decoded.payload.primary_heartbeat.heartbeat_nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE:
        decoded.payload.takeover_prepare.backup_generation = frame->words[0U];
        decoded.payload.takeover_prepare.snapshot_id = frame->words[1U];
        decoded.payload.takeover_prepare.config_id = frame->words[2U];
        decoded.payload.takeover_prepare.proposed_term = frame->words[3U];
        decoded.payload.takeover_prepare.takeover_txid = frame->words[4U];
        decoded.payload.takeover_prepare.nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK:
        decoded.payload.takeover_ack.backup_generation = frame->words[0U];
        decoded.payload.takeover_ack.snapshot_id = frame->words[1U];
        decoded.payload.takeover_ack.config_id = frame->words[2U];
        decoded.payload.takeover_ack.proposed_term = frame->words[3U];
        decoded.payload.takeover_ack.takeover_txid = frame->words[4U];
        decoded.payload.takeover_ack.nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE:
        decoded.payload.recovery_declare.parent_cluster_id = frame->words[0U];
        decoded.payload.recovery_declare.parent_term = frame->words[1U];
        decoded.payload.recovery_declare.parent_config_id = frame->words[2U];
        decoded.payload.recovery_declare.recovery_round = frame->words[3U];
        decoded.payload.recovery_declare.recovery_nonce = frame->words[4U];
        decoded.payload.recovery_declare.recovery_ttl_ms = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK:
        decoded.payload.recovery_ack.recovery_nonce = frame->words[0U];
        decoded.payload.recovery_ack.member_nonce = frame->words[1U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ:
        decoded.payload.backup_resync.backup_generation = frame->words[0U];
        decoded.payload.backup_resync.snapshot_id = frame->words[1U];
        decoded.payload.backup_resync.expected_membership_sequence = frame->words[2U];
        decoded.payload.backup_resync.request_nonce = frame->words[3U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT:
        decoded.payload.backup_reject.backup_generation = frame->words[0U];
        decoded.payload.backup_reject.config_id = frame->words[1U];
        decoded.payload.backup_reject.reason = frame->words[2U];
        decoded.payload.backup_reject.reject_nonce = frame->words[3U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN:
        decoded.payload.config_begin.config_txid = frame->words[0U];
        decoded.payload.config_begin.old_config_id = frame->words[1U];
        decoded.payload.config_begin.proposed_config_id = frame->words[2U];
        decoded.payload.config_begin.old_config_hash = frame->words[3U];
        decoded.payload.config_begin.proposed_config_hash = frame->words[4U];
        decoded.payload.config_begin.proposal_nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
        decoded.payload.config_member.config_txid = frame->words[0U];
        decoded.payload.config_member.target_config_id = frame->words[1U];
        decoded.payload.config_member.member_node_id = frame->words[2U];
        decoded.payload.config_member.member_nonce = frame->words[3U];
        decoded.payload.config_member.member_capabilities = frame->words[4U];
        decoded.payload.config_member.ordinal_count = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE:
        decoded.payload.config_prepare.proposed_config_id = frame->words[0U];
        decoded.payload.config_prepare.old_config_hash = frame->words[1U];
        decoded.payload.config_prepare.proposed_config_hash = frame->words[2U];
        decoded.payload.config_prepare.old_new_voter_count = frame->words[3U];
        decoded.payload.config_prepare.config_txid = frame->words[4U];
        decoded.payload.config_prepare.prepare_nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK:
        decoded.payload.config_ack.proposed_config_id = frame->words[0U];
        decoded.payload.config_ack.config_txid = frame->words[1U];
        decoded.payload.config_ack.voter_slot = frame->words[2U];
        decoded.payload.config_ack.config_phase = frame->words[3U];
        decoded.payload.config_ack.persistence_generation = frame->words[4U];
        decoded.payload.config_ack.ack_nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT:
        decoded.payload.config_commit.committed_config_id = frame->words[0U];
        decoded.payload.config_commit.config_txid = frame->words[1U];
        decoded.payload.config_commit.committed_config_hash = frame->words[2U];
        decoded.payload.config_commit.committed_voter_count = frame->words[3U];
        decoded.payload.config_commit.commit_nonce = frame->words[4U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT:
        decoded.payload.config_abort.config_txid = frame->words[0U];
        decoded.payload.config_abort.old_config_id = frame->words[1U];
        decoded.payload.config_abort.aborted_config_id = frame->words[2U];
        decoded.payload.config_abort.reason = frame->words[3U];
        decoded.payload.config_abort.abort_nonce = frame->words[4U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE:
        decoded.payload.handover_prepare.handover_txid = frame->words[0U];
        decoded.payload.handover_prepare.target_cluster_id = frame->words[1U];
        decoded.payload.handover_prepare.target_term = frame->words[2U];
        decoded.payload.handover_prepare.target_head_node_id = frame->words[3U];
        decoded.payload.handover_prepare.target_config_id = frame->words[4U];
        decoded.payload.handover_prepare.target_config_hash = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY:
        decoded.payload.handover_ready.handover_txid = frame->words[0U];
        decoded.payload.handover_ready.target_cluster_id = frame->words[1U];
        decoded.payload.handover_ready.target_term = frame->words[2U];
        decoded.payload.handover_ready.target_head_node_id = frame->words[3U];
        decoded.payload.handover_ready.target_config_id = frame->words[4U];
        decoded.payload.handover_ready.target_config_hash = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT:
        decoded.payload.handover_commit.handover_txid = frame->words[0U];
        decoded.payload.handover_commit.target_cluster_id = frame->words[1U];
        decoded.payload.handover_commit.target_term = frame->words[2U];
        decoded.payload.handover_commit.target_head_node_id = frame->words[3U];
        decoded.payload.handover_commit.target_config_id = frame->words[4U];
        decoded.payload.handover_commit.target_config_hash = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW:
        decoded.payload.head_withdraw.handover_txid = frame->words[0U];
        decoded.payload.head_withdraw.target_cluster_id = frame->words[1U];
        decoded.payload.head_withdraw.target_term = frame->words[2U];
        decoded.payload.head_withdraw.target_head_node_id = frame->words[3U];
        decoded.payload.head_withdraw.withdraw_nonce = frame->words[4U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE:
        decoded.payload.rekey_prepare.successor_cluster_id = frame->words[0U];
        decoded.payload.rekey_prepare.successor_term = frame->words[1U];
        decoded.payload.rekey_prepare.rekey_txid = frame->words[2U];
        decoded.payload.rekey_prepare.old_config_id = frame->words[3U];
        decoded.payload.rekey_prepare.successor_config_id = frame->words[4U];
        decoded.payload.rekey_prepare.nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK:
        decoded.payload.rekey_ack.successor_cluster_id = frame->words[0U];
        decoded.payload.rekey_ack.successor_term = frame->words[1U];
        decoded.payload.rekey_ack.rekey_txid = frame->words[2U];
        decoded.payload.rekey_ack.successor_config_id = frame->words[3U];
        decoded.payload.rekey_ack.persistence_generation = frame->words[4U];
        decoded.payload.rekey_ack.member_nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT:
        decoded.payload.rekey_commit.successor_cluster_id = frame->words[0U];
        decoded.payload.rekey_commit.successor_term = frame->words[1U];
        decoded.payload.rekey_commit.rekey_txid = frame->words[2U];
        decoded.payload.rekey_commit.old_config_id = frame->words[3U];
        decoded.payload.rekey_commit.successor_config_id = frame->words[4U];
        decoded.payload.rekey_commit.nonce = frame->words[5U];
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        decoded.payload.takeover_certificate.backup_generation = frame->words[0U];
        decoded.payload.takeover_certificate.snapshot_id = frame->words[1U];
        decoded.payload.takeover_certificate.config_id = frame->words[2U];
        decoded.payload.takeover_certificate.takeover_txid = frame->words[3U];
        decoded.payload.takeover_certificate.fragment_descriptor = frame->words[4U];
        decoded.payload.takeover_certificate.vote_bitmap_word = frame->words[5U];
        break;
    default:
        return UCN_ERR_MALFORMED;
    }

    *output = decoded;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_semantic_to_frame(
    const ucn_cluster_wire_v4_semantic_message_t *message,
    ucn_cluster_wire_v4_frame_t *output)
{
    ucn_cluster_wire_v4_frame_t encoded;

    if (message == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_cluster_wire_v4_semantic_payload_size(message->header.type) == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&encoded, 0, sizeof(encoded));
    semantic_header_to_frame(&message->header, &encoded);

    switch (message->header.type) {
    case UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE:
        encoded.words[0U] = message->payload.advertise.score_capacity;
        encoded.words[1U] = message->payload.advertise.lease_ms;
        encoded.words[2U] = message->payload.advertise.advertise_nonce;
        encoded.words[3U] = message->payload.advertise.wire_offer;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST:
        encoded.words[0U] = message->payload.join_request.join_txid;
        encoded.words[1U] = message->payload.join_request.wire_offer;
        encoded.words[2U] = message->payload.join_request.current_config_id;
        encoded.words[3U] = message->payload.join_request.boot_incarnation;
        encoded.words[4U] = message->payload.join_request.score_capacity;
        encoded.words[5U] = message->payload.join_request.join_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT:
        encoded.words[0U] = message->payload.join_accept.join_txid;
        encoded.words[1U] = message->payload.join_accept.target_config_id;
        encoded.words[2U] = message->payload.join_accept.lease_ms;
        encoded.words[3U] = message->payload.join_accept.member_flags;
        encoded.words[4U] = message->payload.join_accept.selected_wire_offer;
        encoded.words[5U] = message->payload.join_accept.member_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT:
        encoded.words[0U] = message->payload.join_reject.join_nonce;
        encoded.words[1U] = message->payload.join_reject.reason;
        encoded.words[2U] = message->payload.join_reject.retry_after_ms;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE:
        encoded.words[0U] = message->payload.keepalive.lease_ms;
        encoded.words[1U] = message->payload.keepalive.keepalive_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_LEAVE:
        encoded.words[0U] = message->payload.leave.leave_nonce;
        encoded.words[1U] = message->payload.leave.reason;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE:
        encoded.words[0U] = message->payload.head_declare.score_capacity;
        encoded.words[1U] = message->payload.head_declare.lease_ms;
        encoded.words[2U] = message->payload.head_declare.declare_nonce;
        encoded.words[3U] = message->payload.head_declare.wire_offer;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER:
        encoded.words[0U] = message->payload.head_takeover.backup_generation;
        encoded.words[1U] = message->payload.head_takeover.snapshot_id;
        encoded.words[2U] = message->payload.head_takeover.certificate_anchor_config_id;
        encoded.words[3U] = message->payload.head_takeover.takeover_txid;
        encoded.words[4U] = message->payload.head_takeover.required_set_mask;
        encoded.words[5U] = message->payload.head_takeover.certificate_crc32;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN:
        encoded.words[0U] = message->payload.head_stepdown.handover_txid;
        encoded.words[1U] = message->payload.head_stepdown.target_cluster_id;
        encoded.words[2U] = message->payload.head_stepdown.target_term;
        encoded.words[3U] = message->payload.head_stepdown.target_head_node_id;
        encoded.words[4U] = message->payload.head_stepdown.stepdown_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN:
        encoded.words[0U] = message->payload.backup_assign.backup_generation;
        encoded.words[1U] = message->payload.backup_assign.backup_node_id;
        encoded.words[2U] = message->payload.backup_assign.sync_token;
        encoded.words[3U] = message->payload.backup_assign.config_id;
        encoded.words[4U] = message->payload.backup_assign.config_hash;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY:
        encoded.words[0U] = message->payload.backup_ready.backup_generation;
        encoded.words[1U] = message->payload.backup_ready.snapshot_id;
        encoded.words[2U] = message->payload.backup_ready.membership_sequence;
        encoded.words[3U] = message->payload.backup_ready.config_id;
        encoded.words[4U] = message->payload.backup_ready.config_hash;
        encoded.words[5U] = message->payload.backup_ready.ready_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC:
        encoded.words[0U] = message->payload.backup_member_sync.backup_generation;
        encoded.words[1U] = message->payload.backup_member_sync.snapshot_id;
        encoded.words[2U] = message->payload.backup_member_sync.membership_sequence;
        encoded.words[3U] = message->payload.backup_member_sync.member_node_id;
        encoded.words[4U] = message->payload.backup_member_sync.member_nonce;
        encoded.words[5U] = message->payload.backup_member_sync.member_lease_ms;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT:
        encoded.words[0U] = message->payload.primary_heartbeat.backup_generation;
        encoded.words[1U] = message->payload.primary_heartbeat.config_id;
        encoded.words[2U] = message->payload.primary_heartbeat.snapshot_id;
        encoded.words[3U] = message->payload.primary_heartbeat.membership_sequence;
        encoded.words[4U] = message->payload.primary_heartbeat.lease_ms;
        encoded.words[5U] = message->payload.primary_heartbeat.heartbeat_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE:
        encoded.words[0U] = message->payload.takeover_prepare.backup_generation;
        encoded.words[1U] = message->payload.takeover_prepare.snapshot_id;
        encoded.words[2U] = message->payload.takeover_prepare.config_id;
        encoded.words[3U] = message->payload.takeover_prepare.proposed_term;
        encoded.words[4U] = message->payload.takeover_prepare.takeover_txid;
        encoded.words[5U] = message->payload.takeover_prepare.nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK:
        encoded.words[0U] = message->payload.takeover_ack.backup_generation;
        encoded.words[1U] = message->payload.takeover_ack.snapshot_id;
        encoded.words[2U] = message->payload.takeover_ack.config_id;
        encoded.words[3U] = message->payload.takeover_ack.proposed_term;
        encoded.words[4U] = message->payload.takeover_ack.takeover_txid;
        encoded.words[5U] = message->payload.takeover_ack.nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE:
        encoded.words[0U] = message->payload.recovery_declare.parent_cluster_id;
        encoded.words[1U] = message->payload.recovery_declare.parent_term;
        encoded.words[2U] = message->payload.recovery_declare.parent_config_id;
        encoded.words[3U] = message->payload.recovery_declare.recovery_round;
        encoded.words[4U] = message->payload.recovery_declare.recovery_nonce;
        encoded.words[5U] = message->payload.recovery_declare.recovery_ttl_ms;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK:
        encoded.words[0U] = message->payload.recovery_ack.recovery_nonce;
        encoded.words[1U] = message->payload.recovery_ack.member_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ:
        encoded.words[0U] = message->payload.backup_resync.backup_generation;
        encoded.words[1U] = message->payload.backup_resync.snapshot_id;
        encoded.words[2U] = message->payload.backup_resync.expected_membership_sequence;
        encoded.words[3U] = message->payload.backup_resync.request_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT:
        encoded.words[0U] = message->payload.backup_reject.backup_generation;
        encoded.words[1U] = message->payload.backup_reject.config_id;
        encoded.words[2U] = message->payload.backup_reject.reason;
        encoded.words[3U] = message->payload.backup_reject.reject_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN:
        encoded.words[0U] = message->payload.config_begin.config_txid;
        encoded.words[1U] = message->payload.config_begin.old_config_id;
        encoded.words[2U] = message->payload.config_begin.proposed_config_id;
        encoded.words[3U] = message->payload.config_begin.old_config_hash;
        encoded.words[4U] = message->payload.config_begin.proposed_config_hash;
        encoded.words[5U] = message->payload.config_begin.proposal_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
        encoded.words[0U] = message->payload.config_member.config_txid;
        encoded.words[1U] = message->payload.config_member.target_config_id;
        encoded.words[2U] = message->payload.config_member.member_node_id;
        encoded.words[3U] = message->payload.config_member.member_nonce;
        encoded.words[4U] = message->payload.config_member.member_capabilities;
        encoded.words[5U] = message->payload.config_member.ordinal_count;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE:
        encoded.words[0U] = message->payload.config_prepare.proposed_config_id;
        encoded.words[1U] = message->payload.config_prepare.old_config_hash;
        encoded.words[2U] = message->payload.config_prepare.proposed_config_hash;
        encoded.words[3U] = message->payload.config_prepare.old_new_voter_count;
        encoded.words[4U] = message->payload.config_prepare.config_txid;
        encoded.words[5U] = message->payload.config_prepare.prepare_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK:
        encoded.words[0U] = message->payload.config_ack.proposed_config_id;
        encoded.words[1U] = message->payload.config_ack.config_txid;
        encoded.words[2U] = message->payload.config_ack.voter_slot;
        encoded.words[3U] = message->payload.config_ack.config_phase;
        encoded.words[4U] = message->payload.config_ack.persistence_generation;
        encoded.words[5U] = message->payload.config_ack.ack_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT:
        encoded.words[0U] = message->payload.config_commit.committed_config_id;
        encoded.words[1U] = message->payload.config_commit.config_txid;
        encoded.words[2U] = message->payload.config_commit.committed_config_hash;
        encoded.words[3U] = message->payload.config_commit.committed_voter_count;
        encoded.words[4U] = message->payload.config_commit.commit_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT:
        encoded.words[0U] = message->payload.config_abort.config_txid;
        encoded.words[1U] = message->payload.config_abort.old_config_id;
        encoded.words[2U] = message->payload.config_abort.aborted_config_id;
        encoded.words[3U] = message->payload.config_abort.reason;
        encoded.words[4U] = message->payload.config_abort.abort_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE:
        encoded.words[0U] = message->payload.handover_prepare.handover_txid;
        encoded.words[1U] = message->payload.handover_prepare.target_cluster_id;
        encoded.words[2U] = message->payload.handover_prepare.target_term;
        encoded.words[3U] = message->payload.handover_prepare.target_head_node_id;
        encoded.words[4U] = message->payload.handover_prepare.target_config_id;
        encoded.words[5U] = message->payload.handover_prepare.target_config_hash;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY:
        encoded.words[0U] = message->payload.handover_ready.handover_txid;
        encoded.words[1U] = message->payload.handover_ready.target_cluster_id;
        encoded.words[2U] = message->payload.handover_ready.target_term;
        encoded.words[3U] = message->payload.handover_ready.target_head_node_id;
        encoded.words[4U] = message->payload.handover_ready.target_config_id;
        encoded.words[5U] = message->payload.handover_ready.target_config_hash;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT:
        encoded.words[0U] = message->payload.handover_commit.handover_txid;
        encoded.words[1U] = message->payload.handover_commit.target_cluster_id;
        encoded.words[2U] = message->payload.handover_commit.target_term;
        encoded.words[3U] = message->payload.handover_commit.target_head_node_id;
        encoded.words[4U] = message->payload.handover_commit.target_config_id;
        encoded.words[5U] = message->payload.handover_commit.target_config_hash;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW:
        encoded.words[0U] = message->payload.head_withdraw.handover_txid;
        encoded.words[1U] = message->payload.head_withdraw.target_cluster_id;
        encoded.words[2U] = message->payload.head_withdraw.target_term;
        encoded.words[3U] = message->payload.head_withdraw.target_head_node_id;
        encoded.words[4U] = message->payload.head_withdraw.withdraw_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE:
        encoded.words[0U] = message->payload.rekey_prepare.successor_cluster_id;
        encoded.words[1U] = message->payload.rekey_prepare.successor_term;
        encoded.words[2U] = message->payload.rekey_prepare.rekey_txid;
        encoded.words[3U] = message->payload.rekey_prepare.old_config_id;
        encoded.words[4U] = message->payload.rekey_prepare.successor_config_id;
        encoded.words[5U] = message->payload.rekey_prepare.nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK:
        encoded.words[0U] = message->payload.rekey_ack.successor_cluster_id;
        encoded.words[1U] = message->payload.rekey_ack.successor_term;
        encoded.words[2U] = message->payload.rekey_ack.rekey_txid;
        encoded.words[3U] = message->payload.rekey_ack.successor_config_id;
        encoded.words[4U] = message->payload.rekey_ack.persistence_generation;
        encoded.words[5U] = message->payload.rekey_ack.member_nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT:
        encoded.words[0U] = message->payload.rekey_commit.successor_cluster_id;
        encoded.words[1U] = message->payload.rekey_commit.successor_term;
        encoded.words[2U] = message->payload.rekey_commit.rekey_txid;
        encoded.words[3U] = message->payload.rekey_commit.old_config_id;
        encoded.words[4U] = message->payload.rekey_commit.successor_config_id;
        encoded.words[5U] = message->payload.rekey_commit.nonce;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        encoded.words[0U] = message->payload.takeover_certificate.backup_generation;
        encoded.words[1U] = message->payload.takeover_certificate.snapshot_id;
        encoded.words[2U] = message->payload.takeover_certificate.config_id;
        encoded.words[3U] = message->payload.takeover_certificate.takeover_txid;
        encoded.words[4U] = message->payload.takeover_certificate.fragment_descriptor;
        encoded.words[5U] = message->payload.takeover_certificate.vote_bitmap_word;
        break;
    default:
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_wire_v4_frame_is_valid(&encoded)) {
        return UCN_ERR_ARGUMENT;
    }
    *output = encoded;
    return UCN_OK;
}

static bool snapshot_kind_from_flags(
    uint8_t flags,
    ucn_cluster_wire_v4_snapshot_kind_t *kind)
{
    if (kind == NULL) {
        return false;
    }
    switch (flags) {
    case 0U:
        *kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_MEMBER;
        return true;
    case UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN:
        *kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_BEGIN;
        return true;
    case UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END:
        *kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_END;
        return true;
    case UCN_CLUSTER_WIRE_V4_FLAG_SYNC_DELTA:
        *kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_DELTA;
        return true;
    default:
        return false;
    }
}

static bool snapshot_flags_from_kind(
    ucn_cluster_wire_v4_snapshot_kind_t kind,
    uint8_t *flags)
{
    if (flags == NULL) {
        return false;
    }
    switch (kind) {
    case UCN_CLUSTER_WIRE_V4_SNAPSHOT_MEMBER:
        *flags = 0U;
        return true;
    case UCN_CLUSTER_WIRE_V4_SNAPSHOT_BEGIN:
        *flags = UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN;
        return true;
    case UCN_CLUSTER_WIRE_V4_SNAPSHOT_END:
        *flags = UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END;
        return true;
    case UCN_CLUSTER_WIRE_V4_SNAPSHOT_DELTA:
        *flags = UCN_CLUSTER_WIRE_V4_FLAG_SYNC_DELTA;
        return true;
    default:
        return false;
    }
}

ucn_result_t ucn_cluster_wire_v4_snapshot_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_snapshot_t *output)
{
    ucn_cluster_wire_v4_semantic_message_t semantic;
    ucn_cluster_wire_v4_snapshot_t decoded;
    ucn_cluster_wire_v4_snapshot_kind_t kind;
    ucn_result_t result;

    if (frame == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_cluster_wire_v4_semantic_from_frame(frame, &semantic);
    if (result != UCN_OK) {
        return result;
    }
    if (semantic.header.type != UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC ||
        !snapshot_kind_from_flags(semantic.header.flags, &kind)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.cluster_id = semantic.header.cluster_id;
    decoded.term = semantic.header.term;
    decoded.head_node_id = semantic.header.head_node_id;
    decoded.backup_generation = semantic.payload.backup_member_sync.backup_generation;
    decoded.snapshot_id = semantic.payload.backup_member_sync.snapshot_id;
    decoded.membership_sequence =
        semantic.payload.backup_member_sync.membership_sequence;
    decoded.kind = kind;
    decoded.member_node_id = semantic.payload.backup_member_sync.member_node_id;
    decoded.member_nonce = semantic.payload.backup_member_sync.member_nonce;
    decoded.member_lease_ms = semantic.payload.backup_member_sync.member_lease_ms;
    *output = decoded;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_snapshot_to_frame(
    const ucn_cluster_wire_v4_snapshot_t *snapshot,
    ucn_cluster_wire_v4_frame_t *output)
{
    ucn_cluster_wire_v4_semantic_message_t semantic;
    uint8_t flags;

    if (snapshot == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!snapshot_flags_from_kind(snapshot->kind, &flags)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&semantic, 0, sizeof(semantic));
    semantic.header.type = UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC;
    semantic.header.role = UCN_CLUSTER_ROLE_HEAD;
    semantic.header.flags = flags;
    semantic.header.cluster_id = snapshot->cluster_id;
    semantic.header.term = snapshot->term;
    semantic.header.head_node_id = snapshot->head_node_id;
    semantic.payload.backup_member_sync.backup_generation =
        snapshot->backup_generation;
    semantic.payload.backup_member_sync.snapshot_id = snapshot->snapshot_id;
    semantic.payload.backup_member_sync.membership_sequence =
        snapshot->membership_sequence;
    semantic.payload.backup_member_sync.member_node_id = snapshot->member_node_id;
    semantic.payload.backup_member_sync.member_nonce = snapshot->member_nonce;
    semantic.payload.backup_member_sync.member_lease_ms = snapshot->member_lease_ms;
    return ucn_cluster_wire_v4_semantic_to_frame(&semantic, output);
}

static bool certificate_set_from_flags(
    uint8_t flags,
    ucn_cluster_wire_v4_certificate_set_t *set)
{
    if (set == NULL) {
        return false;
    }
    if (flags == UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD) {
        *set = UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_OLD;
        return true;
    }
    if (flags == UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW) {
        *set = UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_NEW;
        return true;
    }
    return false;
}

static bool certificate_flags_from_set(
    ucn_cluster_wire_v4_certificate_set_t set,
    uint8_t *flags)
{
    if (flags == NULL) {
        return false;
    }
    if (set == UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_OLD) {
        *flags = UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD;
        return true;
    }
    if (set == UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_NEW) {
        *flags = UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW;
        return true;
    }
    return false;
}

ucn_result_t ucn_cluster_wire_v4_takeover_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_takeover_t *output)
{
    ucn_cluster_wire_v4_semantic_message_t semantic;
    ucn_cluster_wire_v4_takeover_t decoded;
    ucn_result_t result;

    if (frame == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_cluster_wire_v4_semantic_from_frame(frame, &semantic);
    if (result != UCN_OK) {
        return result;
    }
    if (semantic.header.type != UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.cluster_id = semantic.header.cluster_id;
    decoded.proposed_term = semantic.header.term;
    decoded.proposed_head_node_id = semantic.header.head_node_id;
    decoded.backup_generation = semantic.payload.head_takeover.backup_generation;
    decoded.snapshot_id = semantic.payload.head_takeover.snapshot_id;
    decoded.certificate_anchor_config_id =
        semantic.payload.head_takeover.certificate_anchor_config_id;
    decoded.takeover_txid = semantic.payload.head_takeover.takeover_txid;
    decoded.required_set_mask = semantic.payload.head_takeover.required_set_mask;
    decoded.certificate_crc32 = semantic.payload.head_takeover.certificate_crc32;
    *output = decoded;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_takeover_to_frame(
    const ucn_cluster_wire_v4_takeover_t *takeover,
    ucn_cluster_wire_v4_frame_t *output)
{
    ucn_cluster_wire_v4_semantic_message_t semantic;

    if (takeover == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&semantic, 0, sizeof(semantic));
    semantic.header.type = UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER;
    semantic.header.role = UCN_CLUSTER_ROLE_HEAD;
    semantic.header.cluster_id = takeover->cluster_id;
    semantic.header.term = takeover->proposed_term;
    semantic.header.head_node_id = takeover->proposed_head_node_id;
    semantic.payload.head_takeover.backup_generation = takeover->backup_generation;
    semantic.payload.head_takeover.snapshot_id = takeover->snapshot_id;
    semantic.payload.head_takeover.certificate_anchor_config_id =
        takeover->certificate_anchor_config_id;
    semantic.payload.head_takeover.takeover_txid = takeover->takeover_txid;
    semantic.payload.head_takeover.required_set_mask = takeover->required_set_mask;
    semantic.payload.head_takeover.certificate_crc32 = takeover->certificate_crc32;
    return ucn_cluster_wire_v4_semantic_to_frame(&semantic, output);
}

ucn_result_t ucn_cluster_wire_v4_takeover_fragment_from_frame(
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_cluster_wire_v4_takeover_fragment_t *output)
{
    ucn_cluster_wire_v4_semantic_message_t semantic;
    ucn_cluster_wire_v4_takeover_fragment_t decoded;
    ucn_cluster_wire_v4_certificate_set_t certificate_set;
    uint32_t descriptor;
    ucn_result_t result;

    if (frame == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_cluster_wire_v4_semantic_from_frame(frame, &semantic);
    if (result != UCN_OK) {
        return result;
    }
    if (semantic.header.type != UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE ||
        !certificate_set_from_flags(semantic.header.flags, &certificate_set)) {
        return UCN_ERR_ARGUMENT;
    }
    descriptor = semantic.payload.takeover_certificate.fragment_descriptor;
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.cluster_id = semantic.header.cluster_id;
    decoded.proposed_term = semantic.header.term;
    decoded.proposed_head_node_id = semantic.header.head_node_id;
    decoded.backup_generation =
        semantic.payload.takeover_certificate.backup_generation;
    decoded.snapshot_id = semantic.payload.takeover_certificate.snapshot_id;
    decoded.config_id = semantic.payload.takeover_certificate.config_id;
    decoded.takeover_txid = semantic.payload.takeover_certificate.takeover_txid;
    decoded.certificate_set = certificate_set;
    decoded.fragment_index = (uint16_t)(descriptor >> 16U);
    decoded.fragment_count = (uint16_t)descriptor;
    decoded.vote_bitmap_word = semantic.payload.takeover_certificate.vote_bitmap_word;
    *output = decoded;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_takeover_fragment_to_frame(
    const ucn_cluster_wire_v4_takeover_fragment_t *fragment,
    ucn_cluster_wire_v4_frame_t *output)
{
    ucn_cluster_wire_v4_semantic_message_t semantic;
    uint8_t flags;

    if (fragment == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!certificate_flags_from_set(fragment->certificate_set, &flags) ||
        fragment->fragment_count == 0U ||
        fragment->fragment_count > UCN_CLUSTER_WIRE_V4_MAX_CERTIFICATE_FRAGMENTS ||
        fragment->fragment_index >= fragment->fragment_count) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&semantic, 0, sizeof(semantic));
    semantic.header.type = UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE;
    semantic.header.role = UCN_CLUSTER_ROLE_HEAD;
    semantic.header.flags = flags;
    semantic.header.cluster_id = fragment->cluster_id;
    semantic.header.term = fragment->proposed_term;
    semantic.header.head_node_id = fragment->proposed_head_node_id;
    semantic.payload.takeover_certificate.backup_generation =
        fragment->backup_generation;
    semantic.payload.takeover_certificate.snapshot_id = fragment->snapshot_id;
    semantic.payload.takeover_certificate.config_id = fragment->config_id;
    semantic.payload.takeover_certificate.takeover_txid = fragment->takeover_txid;
    semantic.payload.takeover_certificate.fragment_descriptor =
        ((uint32_t)fragment->fragment_index << 16U) |
        (uint32_t)fragment->fragment_count;
    semantic.payload.takeover_certificate.vote_bitmap_word =
        fragment->vote_bitmap_word;
    return ucn_cluster_wire_v4_semantic_to_frame(&semantic, output);
}

bool ucn_cluster_wire_v4_takeover_fragment_matches_admission(
    const ucn_cluster_wire_v4_takeover_t *takeover,
    const ucn_cluster_wire_v4_takeover_fragment_t *fragment,
    const ucn_cluster_wire_v4_certificate_admission_t *admission)
{
    ucn_cluster_wire_v4_frame_t takeover_frame;
    ucn_cluster_wire_v4_frame_t fragment_frame;

    if (takeover == NULL || fragment == NULL ||
        !ucn_cluster_wire_v4_certificate_admission_is_valid(admission)) {
        return false;
    }
    if (ucn_cluster_wire_v4_takeover_to_frame(takeover, &takeover_frame) != UCN_OK ||
        ucn_cluster_wire_v4_takeover_fragment_to_frame(fragment, &fragment_frame) !=
            UCN_OK) {
        return false;
    }
    if (admission->outer_source != takeover->proposed_head_node_id ||
        fragment->cluster_id != takeover->cluster_id ||
        fragment->proposed_term != takeover->proposed_term ||
        fragment->proposed_head_node_id != takeover->proposed_head_node_id ||
        fragment->backup_generation != takeover->backup_generation ||
        fragment->snapshot_id != takeover->snapshot_id ||
        fragment->takeover_txid != takeover->takeover_txid) {
        return false;
    }
    if (takeover->required_set_mask == V4_CERT_REQUIRED_OLD) {
        return admission->new_config_id == 0U &&
               takeover->certificate_anchor_config_id == admission->old_config_id &&
               fragment->certificate_set == UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_OLD &&
               fragment->config_id == admission->old_config_id;
    }
    if (takeover->required_set_mask ==
        (V4_CERT_REQUIRED_OLD | V4_CERT_REQUIRED_NEW)) {
        if (admission->new_config_id == 0U ||
            takeover->certificate_anchor_config_id != admission->new_config_id) {
            return false;
        }
        if (fragment->certificate_set == UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_OLD) {
            return fragment->config_id == admission->old_config_id;
        }
        return fragment->certificate_set == UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_NEW &&
               fragment->config_id == admission->new_config_id;
    }
    return false;
}

static bool required_capabilities_are_valid(uint16_t required_capabilities)
{
    return capability_bitmap_is_valid((uint32_t)required_capabilities);
}

static bool mixed_version_policy_is_valid(
    ucn_cluster_wire_v4_mixed_version_policy_t policy)
{
    return policy == UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4 ||
           policy ==
               UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY;
}

static bool peer_contract_class_is_valid(
    ucn_cluster_wire_v4_peer_contract_class_t requested_class)
{
    return requested_class ==
               UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER ||
           requested_class == UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_VOTER ||
           requested_class == UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_BACKUP ||
           requested_class == UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_HEAD;
}

ucn_result_t ucn_cluster_wire_v4_wire_offer_from_word(
    uint32_t word,
    ucn_cluster_wire_v4_wire_offer_t *output)
{
    ucn_cluster_wire_v4_wire_offer_t decoded;

    if (output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!wire_offer_is_valid(word)) {
        return UCN_ERR_ARGUMENT;
    }
    decoded.minimum_format = (uint8_t)(word >> 24U);
    decoded.maximum_format = (uint8_t)(word >> 16U);
    decoded.capabilities = (uint16_t)word;
    *output = decoded;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_wire_offer_to_word(
    const ucn_cluster_wire_v4_wire_offer_t *offer,
    uint32_t *output)
{
    uint32_t encoded;

    if (offer == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    encoded = ((uint32_t)offer->minimum_format << 24U) |
              ((uint32_t)offer->maximum_format << 16U) |
              (uint32_t)offer->capabilities;
    if (!wire_offer_is_valid(encoded)) {
        return UCN_ERR_ARGUMENT;
    }
    *output = encoded;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_selected_wire_offer_from_word(
    uint32_t word,
    ucn_cluster_wire_v4_selected_wire_offer_t *output)
{
    ucn_cluster_wire_v4_selected_wire_offer_t decoded;

    if (output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!selected_wire_offer_is_valid(word)) {
        return UCN_ERR_ARGUMENT;
    }
    decoded.format = (uint8_t)(word >> 16U);
    decoded.capabilities = (uint16_t)word;
    *output = decoded;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_selected_wire_offer_to_word(
    const ucn_cluster_wire_v4_selected_wire_offer_t *offer,
    uint32_t *output)
{
    uint32_t encoded;

    if (offer == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    encoded = ((uint32_t)offer->format << 16U) |
              (uint32_t)offer->capabilities;
    if (!selected_wire_offer_is_valid(encoded)) {
        return UCN_ERR_ARGUMENT;
    }
    *output = encoded;
    return UCN_OK;
}

bool ucn_cluster_wire_v4_wire_offer_supports(
    const ucn_cluster_wire_v4_wire_offer_t *offer,
    uint16_t required_capabilities)
{
    uint32_t ignored_word;

    return required_capabilities_are_valid(required_capabilities) &&
           ucn_cluster_wire_v4_wire_offer_to_word(offer, &ignored_word) == UCN_OK &&
           (offer->capabilities & required_capabilities) == required_capabilities;
}

ucn_result_t ucn_cluster_wire_v4_wire_offer_negotiate(
    const ucn_cluster_wire_v4_wire_offer_t *local_offer,
    const ucn_cluster_wire_v4_wire_offer_t *peer_offer,
    uint16_t required_capabilities,
    ucn_cluster_wire_v4_selected_wire_offer_t *output)
{
    ucn_cluster_wire_v4_selected_wire_offer_t selected;
    uint32_t ignored_local_word;
    uint32_t ignored_peer_word;
    uint8_t minimum_common_format;
    uint8_t maximum_common_format;
    uint16_t common_capabilities;

    if (local_offer == NULL || peer_offer == NULL || output == NULL ||
        !required_capabilities_are_valid(required_capabilities) ||
        ucn_cluster_wire_v4_wire_offer_to_word(local_offer,
                                               &ignored_local_word) != UCN_OK ||
        ucn_cluster_wire_v4_wire_offer_to_word(peer_offer,
                                               &ignored_peer_word) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    minimum_common_format = local_offer->minimum_format > peer_offer->minimum_format ?
                                local_offer->minimum_format :
                                peer_offer->minimum_format;
    maximum_common_format = local_offer->maximum_format < peer_offer->maximum_format ?
                                local_offer->maximum_format :
                                peer_offer->maximum_format;
    common_capabilities = (uint16_t)(local_offer->capabilities &
                                     peer_offer->capabilities);
    if (minimum_common_format > maximum_common_format ||
        (common_capabilities & required_capabilities) != required_capabilities) {
        return UCN_ERR_STATE;
    }
    selected.format = maximum_common_format;
    selected.capabilities = common_capabilities;
    *output = selected;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
    ucn_cluster_wire_v4_mixed_version_policy_t policy,
    ucn_cluster_wire_format_t peer_format,
    const ucn_cluster_wire_v4_wire_offer_t *peer_offer,
    ucn_cluster_wire_v4_peer_contract_class_t requested_class,
    uint16_t required_v4_capabilities)
{
    uint32_t ignored_offer_word;

    if (!mixed_version_policy_is_valid(policy) ||
        !peer_contract_class_is_valid(requested_class) ||
        !required_capabilities_are_valid(required_v4_capabilities)) {
        return UCN_ERR_ARGUMENT;
    }
    if (peer_format == UCN_CLUSTER_WIRE_FORMAT_V3) {
        if (peer_offer != NULL || required_v4_capabilities != 0U) {
            return UCN_ERR_ARGUMENT;
        }
        return policy ==
                       UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY &&
                       requested_class ==
                           UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER ?
                   UCN_OK :
                   UCN_ERR_STATE;
    }
    if (peer_format != UCN_CLUSTER_WIRE_FORMAT_V4 || peer_offer == NULL ||
        ucn_cluster_wire_v4_wire_offer_to_word(peer_offer, &ignored_offer_word) !=
            UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_cluster_wire_v4_wire_offer_supports(peer_offer,
                                                    required_v4_capabilities) ?
               UCN_OK :
               UCN_ERR_STATE;
}

static bool peer_diagnostic_reason_is_valid(
    ucn_cluster_wire_v4_peer_diagnostic_reason_t reason)
{
    return reason >= UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V4 &&
           reason <= UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_INPUT_INVALID;
}

static void saturating_increment_u32(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

ucn_result_t ucn_cluster_wire_v4_diagnose_peer(
    const ucn_cluster_wire_v4_peer_diagnostic_input_t *input,
    ucn_cluster_wire_v4_peer_diagnostic_view_t *output)
{
    ucn_cluster_wire_v4_peer_diagnostic_view_t diagnosed;
    const ucn_cluster_wire_v4_wire_offer_t *offer;
    uint32_t ignored_offer_word;
    ucn_result_t compatibility;

    if (input == NULL || output == NULL || !node_id_is_valid(input->peer_node_id) ||
        !mixed_version_policy_is_valid(input->policy) ||
        !peer_contract_class_is_valid(input->requested_class) ||
        !required_capabilities_are_valid(input->required_v4_capabilities) ||
        (input->peer_format != UCN_CLUSTER_WIRE_FORMAT_V3 &&
         input->peer_format != UCN_CLUSTER_WIRE_FORMAT_V4) ||
        (input->peer_format == UCN_CLUSTER_WIRE_FORMAT_V3 &&
         (input->peer_offer_present || input->required_v4_capabilities != 0U))) {
        return UCN_ERR_ARGUMENT;
    }

    (void)memset(&diagnosed, 0, sizeof(diagnosed));
    diagnosed.peer_node_id = input->peer_node_id;
    diagnosed.policy = input->policy;
    diagnosed.peer_format = input->peer_format;
    diagnosed.peer_offer_present = input->peer_offer_present;
    diagnosed.requested_class = input->requested_class;
    diagnosed.required_v4_capabilities = input->required_v4_capabilities;
    if (input->peer_offer_present) {
        diagnosed.peer_offer = input->peer_offer;
    }

    if (input->peer_format == UCN_CLUSTER_WIRE_FORMAT_V3) {
        compatibility = ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
            input->policy, input->peer_format, NULL, input->requested_class,
            input->required_v4_capabilities);
        diagnosed.compatibility = UCN_ERR_STATE;
        if (input->policy == UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4) {
            diagnosed.reason =
                UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_STRICT_V4_REQUIRES_V4;
        } else if (input->requested_class !=
                   UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER) {
            diagnosed.reason =
                UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V3_LEGACY_NON_VOTING_ONLY;
        } else if (compatibility == UCN_OK) {
            diagnosed.compatibility = UCN_OK;
            diagnosed.reason =
                UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V3_LEGACY;
        } else {
            return UCN_ERR_ARGUMENT;
        }
    } else if (!input->peer_offer_present) {
        diagnosed.compatibility = UCN_ERR_STATE;
        diagnosed.reason =
            UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_OFFER_MISSING;
    } else if (ucn_cluster_wire_v4_wire_offer_to_word(&input->peer_offer,
                                                       &ignored_offer_word) != UCN_OK) {
        diagnosed.compatibility = UCN_ERR_STATE;
        diagnosed.reason =
            UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_OFFER_INVALID;
    } else {
        offer = &input->peer_offer;
        compatibility = ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
            input->policy, input->peer_format, offer, input->requested_class,
            input->required_v4_capabilities);
        if (compatibility == UCN_OK) {
            diagnosed.compatibility = UCN_OK;
            diagnosed.reason =
                UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V4;
        } else if (compatibility == UCN_ERR_STATE) {
            diagnosed.compatibility = UCN_ERR_STATE;
            diagnosed.reason =
                UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_REQUIRED_CAPABILITY_MISSING;
        } else {
            return UCN_ERR_ARGUMENT;
        }
    }

    *output = diagnosed;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_peer_diagnostic_stats_record(
    ucn_cluster_wire_v4_peer_diagnostic_stats_t *stats,
    ucn_cluster_wire_v4_peer_diagnostic_reason_t reason)
{
    if (stats == NULL || !peer_diagnostic_reason_is_valid(reason)) {
        return UCN_ERR_ARGUMENT;
    }
    if (reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_INPUT_INVALID) {
        saturating_increment_u32(&stats->invalid_inputs);
        return UCN_OK;
    }

    saturating_increment_u32(&stats->evaluated);
    if (reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V4) {
        saturating_increment_u32(&stats->compatible_v4);
    } else if (reason ==
               UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V3_LEGACY) {
        saturating_increment_u32(&stats->compatible_v3_legacy);
    } else {
        saturating_increment_u32(&stats->rejected);
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_detect_format(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_wire_format_t *format)
{
    ucn_cluster_wire_format_t detected;

    if (input == NULL || format == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length == UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES &&
        input[V4_VERSION_OFFSET] == UCN_CLUSTER_WIRE_V3_FORMAT_VERSION) {
        detected = UCN_CLUSTER_WIRE_FORMAT_V3;
    } else if (input_length == UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES &&
               input[V4_VERSION_OFFSET] == UCN_CLUSTER_WIRE_V4_FORMAT_VERSION) {
        detected = UCN_CLUSTER_WIRE_FORMAT_V4;
    } else {
        return UCN_ERR_MALFORMED;
    }
    *format = detected;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_wire_message_t *output)
{
    ucn_cluster_wire_format_t format;
    ucn_cluster_wire_message_t decoded;
    ucn_result_t result;

    if (output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_cluster_wire_detect_format(input, input_length, &format);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.format = format;
    if (format == UCN_CLUSTER_WIRE_FORMAT_V3) {
        result = ucn_cluster_message_decode(input, input_length, &decoded.body.v3);
    } else {
        result = ucn_cluster_wire_v4_decode(input, input_length, &decoded.body.v4);
    }
    if (result != UCN_OK) {
        return result;
    }
    *output = decoded;
    return UCN_OK;
}

static bool admission_matches_takeover(
    const ucn_cluster_wire_v4_certificate_admission_t *admission,
    const ucn_cluster_wire_v4_frame_t *takeover)
{
    if (!ucn_cluster_wire_v4_certificate_admission_is_valid(admission) ||
        takeover == NULL || admission->outer_source != takeover->head_node_id) {
        return false;
    }
    if (takeover->words[4U] == V4_CERT_REQUIRED_OLD) {
        return admission->new_config_id == 0U &&
               admission->old_config_id == takeover->words[2U];
    }
    return takeover->words[4U] ==
                   (V4_CERT_REQUIRED_OLD | V4_CERT_REQUIRED_NEW) &&
           admission->new_config_id != 0U &&
           admission->new_config_id == takeover->words[2U];
}

static bool admission_matches_pending(
    const ucn_cluster_wire_v4_pending_t *pending,
    const ucn_cluster_wire_v4_certificate_admission_t *admission)
{
    const ucn_cluster_wire_v4_certificate_admission_t *stored;

    if (pending == NULL || !pending->occupied ||
        !ucn_cluster_wire_v4_certificate_admission_is_valid(admission)) {
        return false;
    }
    stored = &pending->admission;
    return stored->outer_source == admission->outer_source &&
           stored->old_config_id == admission->old_config_id &&
           stored->new_config_id == admission->new_config_id &&
           stored->source_admitted == admission->source_admitted &&
           stored->frozen_config_admitted == admission->frozen_config_admitted;
}

static bool pending_key_matches(
    const ucn_cluster_wire_v4_pending_t *pending,
    const ucn_cluster_wire_v4_frame_t *takeover,
    const ucn_cluster_wire_v4_certificate_admission_t *admission)
{
    const ucn_cluster_wire_v4_frame_t *stored;

    if (pending == NULL || takeover == NULL || !pending->occupied ||
        !admission_matches_pending(pending, admission)) {
        return false;
    }
    stored = &pending->takeover;
    return stored->cluster_id == takeover->cluster_id &&
           stored->term == takeover->term &&
           stored->head_node_id == takeover->head_node_id &&
           stored->words[0U] == takeover->words[0U] &&
           stored->words[1U] == takeover->words[1U] &&
           stored->words[2U] == takeover->words[2U] &&
           stored->words[3U] == takeover->words[3U] &&
           stored->words[4U] == takeover->words[4U] &&
           stored->words[5U] == takeover->words[5U];
}

static bool fragment_matches_pending_key(
    const ucn_cluster_wire_v4_pending_t *pending,
    const ucn_cluster_wire_v4_frame_t *fragment)
{
    const ucn_cluster_wire_v4_frame_t *takeover;

    if (pending == NULL || fragment == NULL || !pending->occupied) {
        return false;
    }
    takeover = &pending->takeover;
    return fragment->cluster_id == takeover->cluster_id &&
           fragment->term == takeover->term &&
           fragment->head_node_id == takeover->head_node_id &&
           fragment->words[0U] == takeover->words[0U] &&
           fragment->words[1U] == takeover->words[1U] &&
           fragment->words[3U] == takeover->words[3U];
}

static bool fragment_config_matches_admission(
    const ucn_cluster_wire_v4_pending_t *pending,
    const ucn_cluster_wire_v4_frame_t *fragment,
    size_t set_index)
{
    if (pending == NULL || fragment == NULL) {
        return false;
    }
    if (set_index == V4_CERT_SET_OLD_INDEX) {
        return fragment->words[2U] == pending->admission.old_config_id;
    }
    return pending->admission.new_config_id != 0U &&
           fragment->words[2U] == pending->admission.new_config_id;
}

static bool pending_required_set(const ucn_cluster_wire_v4_pending_t *pending,
                                 size_t set_index)
{
    uint32_t required_bit = set_index == V4_CERT_SET_OLD_INDEX ?
                                V4_CERT_REQUIRED_OLD : V4_CERT_REQUIRED_NEW;

    return pending != NULL && (pending->takeover.words[4U] & required_bit) != 0U;
}

void ucn_cluster_wire_v4_pending_reset(
    ucn_cluster_wire_v4_pending_t *pending)
{
    if (pending != NULL) {
        (void)memset(pending, 0, sizeof(*pending));
    }
}

void ucn_cluster_wire_v4_pending_on_active_epoch_change(
    ucn_cluster_wire_v4_pending_t *pending)
{
    ucn_cluster_wire_v4_pending_reset(pending);
}

bool ucn_cluster_wire_v4_pending_expire(
    ucn_cluster_wire_v4_pending_t *pending,
    uint32_t now_ms)
{
    if (pending == NULL || !pending->occupied ||
        !ucn_deadline_expired(now_ms, pending->deadline_ms)) {
        return false;
    }
    ucn_cluster_wire_v4_pending_reset(pending);
    return true;
}

ucn_result_t ucn_cluster_wire_v4_pending_begin(
    ucn_cluster_wire_v4_pending_t *pending,
    const ucn_cluster_wire_v4_frame_t *takeover,
    const ucn_cluster_wire_v4_certificate_admission_t *admission,
    uint32_t now_ms)
{
    if (pending == NULL || takeover == NULL || admission == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_wire_v4_frame_is_valid(takeover) ||
        takeover->type != UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER) {
        return UCN_ERR_MALFORMED;
    }
    if (!admission_matches_takeover(admission, takeover)) {
        return UCN_ERR_STATE;
    }
    (void)ucn_cluster_wire_v4_pending_expire(pending, now_ms);
    if (pending->occupied) {
        if (!pending_key_matches(pending, takeover, admission)) {
            return UCN_ERR_NO_SPACE;
        }
        return UCN_OK;
    }
    (void)memset(pending, 0, sizeof(*pending));
    pending->occupied = true;
    pending->deadline_ms = ucn_deadline_from_now(
        now_ms, UCN_CLUSTER_WIRE_V4_PENDING_TIMEOUT_MS);
    pending->takeover = *takeover;
    pending->admission = *admission;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_v4_pending_accept_fragment(
    ucn_cluster_wire_v4_pending_t *pending,
    const ucn_cluster_wire_v4_frame_t *fragment,
    const ucn_cluster_wire_v4_certificate_admission_t *admission,
    uint32_t now_ms)
{
    size_t set_index;
    uint32_t descriptor;
    uint32_t fragment_index;
    uint32_t fragment_count;
    uint16_t fragment_bit;

    if (pending == NULL || fragment == NULL || admission == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_wire_v4_frame_is_valid(fragment) ||
        fragment->type != UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE) {
        return UCN_ERR_MALFORMED;
    }
    if (!ucn_cluster_wire_v4_certificate_admission_is_valid(admission)) {
        return UCN_ERR_STATE;
    }
    if (!pending->occupied) {
        return UCN_ERR_NOT_FOUND;
    }
    if (!admission_matches_pending(pending, admission)) {
        return UCN_ERR_STATE;
    }
    if (!fragment_matches_pending_key(pending, fragment)) {
        return UCN_ERR_MALFORMED;
    }
    set_index = fragment->flags == UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD ?
                    V4_CERT_SET_OLD_INDEX : V4_CERT_SET_NEW_INDEX;
    if (!pending_required_set(pending, set_index) ||
        !fragment_config_matches_admission(pending, fragment, set_index)) {
        return UCN_ERR_STATE;
    }

    /* An unauthenticated or frozen-Config-inconsistent fragment must never
     * use the lazy-expiry path as an eviction primitive. A future RX owner
     * may call pending_expire() from its timer; this RX-side convenience
     * expiry is reached only after the non-mutating checks above pass. */
    (void)ucn_cluster_wire_v4_pending_expire(pending, now_ms);
    if (!pending->occupied) {
        return UCN_ERR_NOT_FOUND;
    }

    descriptor = fragment->words[4U];
    fragment_index = descriptor >> 16U;
    fragment_count = descriptor & UINT32_C(0xFFFF);
    fragment_bit = (uint16_t)(UINT16_C(1) << fragment_index);
    if (pending->fragment_counts[set_index] == 0U) {
        pending->fragment_counts[set_index] = (uint16_t)fragment_count;
        pending->set_config_ids[set_index] = fragment->words[2U];
    } else if (pending->fragment_counts[set_index] != fragment_count ||
               pending->set_config_ids[set_index] != fragment->words[2U]) {
        ucn_cluster_wire_v4_pending_reset(pending);
        return UCN_ERR_MALFORMED;
    }
    if ((pending->received_fragment_masks[set_index] & fragment_bit) != 0U) {
        if (pending->bitmap_words[set_index][fragment_index] != fragment->words[5U]) {
            ucn_cluster_wire_v4_pending_reset(pending);
            return UCN_ERR_MALFORMED;
        }
        return UCN_OK;
    }
    pending->bitmap_words[set_index][fragment_index] = fragment->words[5U];
    pending->received_fragment_masks[set_index] |= fragment_bit;
    return UCN_OK;
}

bool ucn_cluster_wire_v4_pending_has_all_fragments(
    const ucn_cluster_wire_v4_pending_t *pending)
{
    size_t set_index;

    if (pending == NULL || !pending->occupied) {
        return false;
    }
    for (set_index = V4_CERT_SET_OLD_INDEX;
         set_index <= V4_CERT_SET_NEW_INDEX; ++set_index) {
        uint16_t expected_mask;

        if (!pending_required_set(pending, set_index)) {
            continue;
        }
        if (pending->fragment_counts[set_index] == 0U) {
            return false;
        }
        expected_mask = (uint16_t)((UINT16_C(1)
                                     << pending->fragment_counts[set_index]) -
                                    UINT16_C(1));
        if (pending->received_fragment_masks[set_index] != expected_mask) {
            return false;
        }
    }
    return true;
}
