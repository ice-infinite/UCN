#ifndef UCN_CLUSTER_WIRE_V4_H
#define UCN_CLUSTER_WIRE_V4_H

/* CLV2-05-02: frozen RFC4 codec boundary.
 *
 * This header deliberately does not alter the existing v3 Cluster message
 * type or its 32-byte production path.  The v4 frame is an isolated raw
 * wire container; CLV2-05-03 adds private type-specific semantic builders
 * without changing this public raw boundary. Later milestones will decide
 * whether any decoded frame may affect the FSM. */

#include "ucn/ucn_cluster.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_CLUSTER_WIRE_V3_FORMAT_VERSION ((uint8_t)3U)
#define UCN_CLUSTER_WIRE_V4_FORMAT_VERSION ((uint8_t)4U)
/* M14 recommendation only.  This does not bypass M05's production encoder,
 * RX, FSM or Authority hold. */
#define UCN_CLUSTER_RECOMMENDED_WIRE_FORMAT \
    UCN_CLUSTER_WIRE_V4_FORMAT_VERSION
#define UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES ((size_t)40U)
/* Capacity-only upper bound for code that deliberately carries either frozen
 * Cluster wire format.  It does not alter UCN_CLUSTER_MESSAGE_BYTES and does
 * not enable v4 production TX/RX. */
#define UCN_CLUSTER_WIRE_MAX_MESSAGE_BYTES UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES
#define UCN_CLUSTER_WIRE_V4_WORD_COUNT ((size_t)6U)

typedef char ucn_cluster_wire_v3_current_alias_must_match[
    UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES == UCN_CLUSTER_MESSAGE_BYTES ? 1 : -1];
typedef char ucn_cluster_wire_max_must_cover_v3_and_v4[
    UCN_CLUSTER_WIRE_MAX_MESSAGE_BYTES >= UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES &&
            UCN_CLUSTER_WIRE_MAX_MESSAGE_BYTES >=
                UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES ?
        1 :
        -1];

/* RFC4 §6.2.1: one bounded reassembly slot and at most two 32-bit words per
 * voter set for the current MAX_MEMBERS <= 32 contract. */
#define UCN_CLUSTER_WIRE_V4_PENDING_MAX ((size_t)1U)
#define UCN_CLUSTER_WIRE_V4_MAX_CERTIFICATE_FRAGMENTS ((size_t)2U)
#define UCN_CLUSTER_WIRE_V4_PENDING_TIMEOUT_MS UINT32_C(1000)

/* A product may explicitly enable v4 encoding in a non-production test or
 * staging target.  The normal library default is fail-closed: decode works,
 * but encode returns UCN_ERR_CONFIG and no Cluster FSM call site uses it. */
#ifndef UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED
#define UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED 0
#endif

typedef char ucn_cluster_wire_v4_encoder_enabled_must_be_bool[
    UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED == 0 ||
            UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED == 1 ? 1 : -1];

typedef enum ucn_cluster_wire_v4_type {
    UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE = 1,
    UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST = 2,
    UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT = 3,
    UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT = 4,
    UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE = 5,
    UCN_CLUSTER_WIRE_V4_MSG_LEAVE = 6,
    UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE = 7,
    UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER = 8,
    UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN = 9,
    UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN = 10,
    UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY = 11,
    UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC = 12,
    UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT = 13,
    UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE = 14,
    UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK = 15,
    UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE = 16,
    UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK = 17,
    UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ = 18,
    UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT = 19,
    UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN = 20,
    UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER = 21,
    UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE = 22,
    UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK = 23,
    UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT = 24,
    UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT = 25,
    UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE = 26,
    UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY = 27,
    UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT = 28,
    UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW = 29,
    UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE = 30,
    UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK = 31,
    UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT = 32,
    UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE = 33
} ucn_cluster_wire_v4_type_t;

#define UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN ((uint8_t)0x01U)
#define UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END ((uint8_t)0x02U)
#define UCN_CLUSTER_WIRE_V4_FLAG_SYNC_DELTA ((uint8_t)0x04U)
#define UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_ADD ((uint8_t)0x10U)
#define UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_REMOVE ((uint8_t)0x20U)
#define UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD ((uint8_t)0x40U)
#define UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW ((uint8_t)0x80U)

/* Raw, decoded RFC4 frame.  words[0] is P0 and words[5] is P5.  This is not
 * a replacement for ucn_cluster_message_t and is intentionally not fed into
 * the current v3 Cluster receive/send path. */
typedef struct ucn_cluster_wire_v4_frame {
    uint8_t type;
    ucn_cluster_role_t role;
    uint8_t flags;
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
    uint32_t words[UCN_CLUSTER_WIRE_V4_WORD_COUNT];
} ucn_cluster_wire_v4_frame_t;

/* Receiver-side proof passed into the Certificate-pending helper.  These
 * fields are never decoded from the wire.  The future v4 RX gate must set
 * both admission bits only after authenticating/admitting outer_source and
 * binding the indicated Config identities to its frozen ConfigState.  The
 * codec does not yet own a ConfigState or an RX gate, so it fail-closes when
 * this explicit proof is absent rather than accepting a raw Type 8/33 frame.
 *
 * Stable Certificate: new_config_id is zero and old_config_id is Type 8 P2.
 * Joint Certificate: both IDs are valid/different and new_config_id is Type
 * 8 P2; Type 33 OLD/NEW fragments bind to the respective IDs. */
typedef struct ucn_cluster_wire_v4_certificate_admission {
    ucn_node_id_t outer_source;
    uint32_t old_config_id;
    uint32_t new_config_id;
    bool source_admitted;
    bool frozen_config_admitted;
} ucn_cluster_wire_v4_certificate_admission_t;

typedef enum ucn_cluster_wire_format {
    UCN_CLUSTER_WIRE_FORMAT_INVALID = 0,
    UCN_CLUSTER_WIRE_FORMAT_V3 = UCN_CLUSTER_WIRE_V3_FORMAT_VERSION,
    UCN_CLUSTER_WIRE_FORMAT_V4 = UCN_CLUSTER_WIRE_V4_FORMAT_VERSION
} ucn_cluster_wire_format_t;

/* Strict dispatch output. The v3 arm retains the existing semantic message;
 * the public v4 arm remains raw. CLV2-05-03's semantic builder is private to
 * the codec/test boundary and is not a production RX/FSM interface. */
typedef struct ucn_cluster_wire_message {
    ucn_cluster_wire_format_t format;
    union {
        ucn_cluster_message_t v3;
        ucn_cluster_wire_v4_frame_t v4;
    } body;
} ucn_cluster_wire_message_t;

/* Fixed, codec-only RFC4 §6.2.1 Certificate-pending storage.  It records
 * fragments but never establishes Authority or invokes the Cluster FSM. */
typedef struct ucn_cluster_wire_v4_pending {
    bool occupied;
    uint32_t deadline_ms;
    ucn_cluster_wire_v4_frame_t takeover;
    ucn_cluster_wire_v4_certificate_admission_t admission;
    uint16_t fragment_counts[2];
    uint16_t received_fragment_masks[2];
    uint32_t set_config_ids[2];
    uint32_t bitmap_words[2][UCN_CLUSTER_WIRE_V4_MAX_CERTIFICATE_FRAGMENTS];
} ucn_cluster_wire_v4_pending_t;

bool ucn_cluster_wire_v4_frame_is_valid(
    const ucn_cluster_wire_v4_frame_t *frame);
bool ucn_cluster_wire_v4_certificate_admission_is_valid(
    const ucn_cluster_wire_v4_certificate_admission_t *admission);

/* Requires exact RFC4 length/version.  On failure output is unchanged. */
ucn_result_t ucn_cluster_wire_v4_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_wire_v4_frame_t *output);

/* Default-disabled production encoder.  A test/staging target can compile
 * with UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED=1 to verify golden vectors. */
ucn_result_t ucn_cluster_wire_v4_encode(
    const ucn_cluster_wire_v4_frame_t *frame,
    uint8_t output[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES]);

/* Classifies only exact (length, version) pairs: 32/3 or 40/4.  There is no
 * fallback decoder and no downgrade interpretation. */
ucn_result_t ucn_cluster_wire_detect_format(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_wire_format_t *format);

/* Strict two-way dispatch.  It is codec-only and has no Cluster FSM effects.
 * On failure output is unchanged. */
ucn_result_t ucn_cluster_wire_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_wire_message_t *output);

void ucn_cluster_wire_v4_pending_reset(
    ucn_cluster_wire_v4_pending_t *pending);
void ucn_cluster_wire_v4_pending_on_active_epoch_change(
    ucn_cluster_wire_v4_pending_t *pending);
bool ucn_cluster_wire_v4_pending_expire(
    ucn_cluster_wire_v4_pending_t *pending,
    uint32_t now_ms);
/* Both insert operations require the receiver-side admission context above.
 * A missing, unadmitted, source-mismatched, key-mismatched, or frozen-Config-
 * mismatched request is rejected without occupying, clearing, replacing or
 * extending an existing slot/deadline, including at exactly deadline_ms.
 * A future RX owner performs time-driven collection with pending_expire();
 * accept_fragment() performs lazy expiry only after those non-mutating gates
 * have passed. */
ucn_result_t ucn_cluster_wire_v4_pending_begin(
    ucn_cluster_wire_v4_pending_t *pending,
    const ucn_cluster_wire_v4_frame_t *takeover,
    const ucn_cluster_wire_v4_certificate_admission_t *admission,
    uint32_t now_ms);
ucn_result_t ucn_cluster_wire_v4_pending_accept_fragment(
    ucn_cluster_wire_v4_pending_t *pending,
    const ucn_cluster_wire_v4_frame_t *fragment,
    const ucn_cluster_wire_v4_certificate_admission_t *admission,
    uint32_t now_ms);
bool ucn_cluster_wire_v4_pending_has_all_fragments(
    const ucn_cluster_wire_v4_pending_t *pending);

#ifdef __cplusplus
}
#endif

#endif
