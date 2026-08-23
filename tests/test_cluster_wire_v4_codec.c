#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_wire_v4.h"
#include "ucn_cluster_wire_v4_semantic.h"

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

/* CLV2-05-10 keeps its raw-byte tests independent from the codec encoder.
 * These offsets are the frozen RFC4 layout, not production Cluster storage. */
#define V4_TEST_VERSION_OFFSET ((size_t)0U)
#define V4_TEST_TYPE_OFFSET ((size_t)1U)
#define V4_TEST_ROLE_OFFSET ((size_t)2U)
#define V4_TEST_FLAGS_OFFSET ((size_t)3U)
#define V4_TEST_CLUSTER_ID_OFFSET ((size_t)4U)
#define V4_TEST_TERM_OFFSET ((size_t)8U)
#define V4_TEST_HEAD_ID_OFFSET ((size_t)12U)
#define V4_TEST_WORDS_OFFSET ((size_t)16U)
#define V4_TEST_NO_ZERO_TAIL UINT8_MAX
#define V4_TEST_FUZZ_SEED UINT32_C(0xC15A4E5D)
#define V4_TEST_FUZZ_ITERATIONS ((size_t)4096U)

static void test_write_u32_be(uint8_t *output, uint32_t value)
{
    output[0U] = (uint8_t)(value >> 24U);
    output[1U] = (uint8_t)(value >> 16U);
    output[2U] = (uint8_t)(value >> 8U);
    output[3U] = (uint8_t)value;
}

static void raw_from_frame_for_test(
    const ucn_cluster_wire_v4_frame_t *frame,
    uint8_t output[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES])
{
    size_t index;

    (void)memset(output, 0, UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES);
    output[V4_TEST_VERSION_OFFSET] = UCN_CLUSTER_WIRE_V4_FORMAT_VERSION;
    output[V4_TEST_TYPE_OFFSET] = frame->type;
    output[V4_TEST_ROLE_OFFSET] = (uint8_t)frame->role;
    output[V4_TEST_FLAGS_OFFSET] = frame->flags;
    test_write_u32_be(output + V4_TEST_CLUSTER_ID_OFFSET, frame->cluster_id);
    test_write_u32_be(output + V4_TEST_TERM_OFFSET, frame->term);
    test_write_u32_be(output + V4_TEST_HEAD_ID_OFFSET, frame->head_node_id);
    for (index = 0U; index < UCN_CLUSTER_WIRE_V4_WORD_COUNT; ++index) {
        test_write_u32_be(output + V4_TEST_WORDS_OFFSET + index * 4U,
                          frame->words[index]);
    }
}

static void raw_set_word_for_test(
    uint8_t output[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES],
    size_t index,
    uint32_t value)
{
    test_write_u32_be(output + V4_TEST_WORDS_OFFSET + index * 4U, value);
}

static bool raw_rejection_keeps_output_unchanged(const uint8_t *input,
                                                  size_t input_length)
{
    ucn_cluster_wire_v4_frame_t frame;
    ucn_cluster_wire_v4_frame_t expected_frame;
    ucn_cluster_wire_message_t message;
    ucn_cluster_wire_message_t expected_message;

    (void)memset(&frame, 0xA5, sizeof(frame));
    expected_frame = frame;
    if (ucn_cluster_wire_v4_decode(input, input_length, &frame) == UCN_OK ||
        memcmp(&frame, &expected_frame, sizeof(frame)) != 0) {
        return false;
    }

    (void)memset(&message, 0x5A, sizeof(message));
    expected_message = message;
    return ucn_cluster_wire_decode(input, input_length, &message) != UCN_OK &&
           memcmp(&message, &expected_message, sizeof(message)) == 0;
}

static bool semantic_rejection_keeps_output_unchanged(
    const ucn_cluster_wire_v4_frame_t *frame)
{
    ucn_cluster_wire_v4_semantic_message_t semantic;
    ucn_cluster_wire_v4_semantic_message_t expected_semantic;

    (void)memset(&semantic, 0x3C, sizeof(semantic));
    expected_semantic = semantic;
    return ucn_cluster_wire_v4_semantic_from_frame(frame, &semantic) != UCN_OK &&
           memcmp(&semantic, &expected_semantic, sizeof(semantic)) == 0;
}

static bool raw_decode_contract_holds(const uint8_t *input, size_t input_length)
{
    ucn_cluster_wire_v4_frame_t frame;
    ucn_cluster_wire_v4_frame_t expected_frame;
    ucn_cluster_wire_v4_semantic_message_t semantic;
    ucn_cluster_wire_message_t message;
    ucn_cluster_wire_message_t expected_message;
    ucn_result_t result;

    (void)memset(&frame, 0xA5, sizeof(frame));
    expected_frame = frame;
    result = ucn_cluster_wire_v4_decode(input, input_length, &frame);
    if (result == UCN_OK) {
        if (!ucn_cluster_wire_v4_frame_is_valid(&frame)) {
            return false;
        }
        (void)memset(&semantic, 0x3C, sizeof(semantic));
        if (ucn_cluster_wire_v4_semantic_from_frame(&frame, &semantic) != UCN_OK ||
            semantic.header.type != frame.type) {
            return false;
        }
    } else if (memcmp(&frame, &expected_frame, sizeof(frame)) != 0) {
        return false;
    }

    (void)memset(&message, 0x5A, sizeof(message));
    expected_message = message;
    result = ucn_cluster_wire_decode(input, input_length, &message);
    if (result == UCN_OK) {
        return message.format == UCN_CLUSTER_WIRE_FORMAT_V4 &&
               ucn_cluster_wire_v4_frame_is_valid(&message.body.v4);
    }
    return memcmp(&message, &expected_message, sizeof(message)) == 0;
}

static uint8_t invalid_flags_for_type(uint8_t type)
{
    switch (type) {
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC:
        return UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN |
               UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
        return UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_ADD |
               UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_REMOVE;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        return UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD |
               UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW;
    default:
        return UINT8_C(0x01);
    }
}

static uint32_t next_fuzz_word(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void init_frame(ucn_cluster_wire_v4_frame_t *frame, uint8_t type)
{
    (void)memset(frame, 0, sizeof(*frame));
    frame->type = type;
    frame->role = UCN_CLUSTER_ROLE_HEAD;
    frame->cluster_id = UINT32_C(0x01020304);
    frame->term = 2U;
    frame->head_node_id = UINT32_C(0x0A0B0C0D);
}

static bool make_valid_frame(uint8_t type, ucn_cluster_wire_v4_frame_t *frame)
{
    init_frame(frame, type);

    switch (type) {
    case UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE:
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE:
        frame->words[0U] = UINT32_C(0x00640020);
        frame->words[1U] = 1000U;
        frame->words[2U] = 1U;
        frame->words[3U] = UINT32_C(0x04040001);
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST:
        frame->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
        frame->words[0U] = 1U;
        frame->words[1U] = UINT32_C(0x04040001);
        frame->words[3U] = 1U;
        frame->words[4U] = UINT32_C(0x00640020);
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1000U;
        frame->words[4U] = UINT32_C(0x00040001);
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1000U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE:
        frame->role = UCN_CLUSTER_ROLE_MEMBER;
        frame->words[0U] = 1000U;
        frame->words[1U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_LEAVE:
        frame->role = UCN_CLUSTER_ROLE_MEMBER;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN:
        frame->words[0U] = 1U;
        frame->words[1U] = UINT32_C(0x11223344);
        frame->words[2U] = 3U;
        frame->words[3U] = UINT32_C(0x55667788);
        frame->words[4U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN:
        frame->words[0U] = 1U;
        frame->words[1U] = UINT32_C(0x55667788);
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY:
        frame->role = UCN_CLUSTER_ROLE_BACKUP;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = UINT32_C(0x55667788);
        frame->words[4U] = 1U;
        frame->words[5U] = 1000U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1000U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE:
        frame->role = UCN_CLUSTER_ROLE_BACKUP;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK:
        frame->role = UCN_CLUSTER_ROLE_MEMBER;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE:
        frame->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
        frame->words[0U] = UINT32_C(0x11223344);
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1000U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK:
        frame->role = UCN_CLUSTER_ROLE_MEMBER;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ:
        frame->role = UCN_CLUSTER_ROLE_BACKUP;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT:
        frame->role = UCN_CLUSTER_ROLE_BACKUP;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 2U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
        frame->flags = UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_ADD;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = UINT32_C(0x55667788);
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = UINT32_C(0x00010001);
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK:
        frame->role = UCN_CLUSTER_ROLE_MEMBER;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT:
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 2U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT:
        frame->words[0U] = 1U;
        frame->words[1U] = UINT32_C(0x11223344);
        frame->words[2U] = 3U;
        frame->words[3U] = UINT32_C(0x55667788);
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY:
        frame->words[0U] = 1U;
        frame->words[1U] = UINT32_C(0x11223344);
        frame->words[2U] = 3U;
        frame->words[3U] = UINT32_C(0x55667788);
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW:
        frame->words[0U] = 1U;
        frame->words[1U] = UINT32_C(0x11223344);
        frame->words[2U] = 3U;
        frame->words[3U] = UINT32_C(0x55667788);
        frame->words[4U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE:
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT:
        frame->words[0U] = UINT32_C(0x11223344);
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 2U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK:
        frame->role = UCN_CLUSTER_ROLE_MEMBER;
        frame->words[0U] = UINT32_C(0x11223344);
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        frame->flags = UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD;
        frame->words[0U] = 1U;
        frame->words[1U] = 1U;
        frame->words[2U] = 1U;
        frame->words[3U] = 1U;
        frame->words[4U] = 1U;
        frame->words[5U] = 1U;
        break;
    default:
        return false;
    }
    return true;
}

typedef struct extended_type_fixture {
    uint8_t type;
    ucn_cluster_role_t role;
    uint8_t flags;
    size_t payload_words;
    bool has_zero_tail;
    uint32_t words[UCN_CLUSTER_WIRE_V4_WORD_COUNT];
} extended_type_fixture_t;

static bool extended_type_semantic_fields_match(
    const ucn_cluster_wire_v4_semantic_message_t *semantic,
    const uint32_t words[UCN_CLUSTER_WIRE_V4_WORD_COUNT])
{
    if (semantic == NULL || words == NULL) {
        return false;
    }
    switch (semantic->header.type) {
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN:
        return semantic->payload.config_begin.config_txid == words[0U] &&
               semantic->payload.config_begin.old_config_id == words[1U] &&
               semantic->payload.config_begin.proposed_config_id == words[2U] &&
               semantic->payload.config_begin.old_config_hash == words[3U] &&
               semantic->payload.config_begin.proposed_config_hash == words[4U] &&
               semantic->payload.config_begin.proposal_nonce == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER:
        return semantic->payload.config_member.config_txid == words[0U] &&
               semantic->payload.config_member.target_config_id == words[1U] &&
               semantic->payload.config_member.member_node_id == words[2U] &&
               semantic->payload.config_member.member_nonce == words[3U] &&
               semantic->payload.config_member.member_capabilities == words[4U] &&
               semantic->payload.config_member.ordinal_count == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE:
        return semantic->payload.config_prepare.proposed_config_id == words[0U] &&
               semantic->payload.config_prepare.old_config_hash == words[1U] &&
               semantic->payload.config_prepare.proposed_config_hash == words[2U] &&
               semantic->payload.config_prepare.old_new_voter_count == words[3U] &&
               semantic->payload.config_prepare.config_txid == words[4U] &&
               semantic->payload.config_prepare.prepare_nonce == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK:
        return semantic->payload.config_ack.proposed_config_id == words[0U] &&
               semantic->payload.config_ack.config_txid == words[1U] &&
               semantic->payload.config_ack.voter_slot == words[2U] &&
               semantic->payload.config_ack.config_phase == words[3U] &&
               semantic->payload.config_ack.persistence_generation == words[4U] &&
               semantic->payload.config_ack.ack_nonce == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT:
        return semantic->payload.config_commit.committed_config_id == words[0U] &&
               semantic->payload.config_commit.config_txid == words[1U] &&
               semantic->payload.config_commit.committed_config_hash == words[2U] &&
               semantic->payload.config_commit.committed_voter_count == words[3U] &&
               semantic->payload.config_commit.commit_nonce == words[4U];
    case UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT:
        return semantic->payload.config_abort.config_txid == words[0U] &&
               semantic->payload.config_abort.old_config_id == words[1U] &&
               semantic->payload.config_abort.aborted_config_id == words[2U] &&
               semantic->payload.config_abort.reason == words[3U] &&
               semantic->payload.config_abort.abort_nonce == words[4U];
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE:
        return semantic->payload.handover_prepare.handover_txid == words[0U] &&
               semantic->payload.handover_prepare.target_cluster_id == words[1U] &&
               semantic->payload.handover_prepare.target_term == words[2U] &&
               semantic->payload.handover_prepare.target_head_node_id == words[3U] &&
               semantic->payload.handover_prepare.target_config_id == words[4U] &&
               semantic->payload.handover_prepare.target_config_hash == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY:
        return semantic->payload.handover_ready.handover_txid == words[0U] &&
               semantic->payload.handover_ready.target_cluster_id == words[1U] &&
               semantic->payload.handover_ready.target_term == words[2U] &&
               semantic->payload.handover_ready.target_head_node_id == words[3U] &&
               semantic->payload.handover_ready.target_config_id == words[4U] &&
               semantic->payload.handover_ready.target_config_hash == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT:
        return semantic->payload.handover_commit.handover_txid == words[0U] &&
               semantic->payload.handover_commit.target_cluster_id == words[1U] &&
               semantic->payload.handover_commit.target_term == words[2U] &&
               semantic->payload.handover_commit.target_head_node_id == words[3U] &&
               semantic->payload.handover_commit.target_config_id == words[4U] &&
               semantic->payload.handover_commit.target_config_hash == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW:
        return semantic->payload.head_withdraw.handover_txid == words[0U] &&
               semantic->payload.head_withdraw.target_cluster_id == words[1U] &&
               semantic->payload.head_withdraw.target_term == words[2U] &&
               semantic->payload.head_withdraw.target_head_node_id == words[3U] &&
               semantic->payload.head_withdraw.withdraw_nonce == words[4U];
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE:
        return semantic->payload.rekey_prepare.successor_cluster_id == words[0U] &&
               semantic->payload.rekey_prepare.successor_term == words[1U] &&
               semantic->payload.rekey_prepare.rekey_txid == words[2U] &&
               semantic->payload.rekey_prepare.old_config_id == words[3U] &&
               semantic->payload.rekey_prepare.successor_config_id == words[4U] &&
               semantic->payload.rekey_prepare.nonce == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK:
        return semantic->payload.rekey_ack.successor_cluster_id == words[0U] &&
               semantic->payload.rekey_ack.successor_term == words[1U] &&
               semantic->payload.rekey_ack.rekey_txid == words[2U] &&
               semantic->payload.rekey_ack.successor_config_id == words[3U] &&
               semantic->payload.rekey_ack.persistence_generation == words[4U] &&
               semantic->payload.rekey_ack.member_nonce == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT:
        return semantic->payload.rekey_commit.successor_cluster_id == words[0U] &&
               semantic->payload.rekey_commit.successor_term == words[1U] &&
               semantic->payload.rekey_commit.rekey_txid == words[2U] &&
               semantic->payload.rekey_commit.old_config_id == words[3U] &&
               semantic->payload.rekey_commit.successor_config_id == words[4U] &&
               semantic->payload.rekey_commit.nonce == words[5U];
    case UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE:
        return semantic->payload.takeover_certificate.backup_generation == words[0U] &&
               semantic->payload.takeover_certificate.snapshot_id == words[1U] &&
               semantic->payload.takeover_certificate.config_id == words[2U] &&
               semantic->payload.takeover_certificate.takeover_txid == words[3U] &&
               semantic->payload.takeover_certificate.fragment_descriptor == words[4U] &&
               semantic->payload.takeover_certificate.vote_bitmap_word == words[5U];
    default:
        return false;
    }
}

static int test_extended_type_registry_contract(void)
{
    static const extended_type_fixture_t fixtures[] = {
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN, UCN_CLUSTER_ROLE_HEAD, 0U, 6U, false,
          { UINT32_C(0x2001), UINT32_C(0x2002), UINT32_C(0x2003),
            UINT32_C(0x2004), UINT32_C(0x2005), UINT32_C(0x2006) } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER, UCN_CLUSTER_ROLE_HEAD,
          UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_ADD, 6U, false,
          { UINT32_C(0x2101), UINT32_C(0x2102), UINT32_C(0x2103),
            UINT32_C(0x2104), UINT32_C(0x002D), UINT32_C(0x00010003) } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE, UCN_CLUSTER_ROLE_HEAD, 0U, 6U, false,
          { UINT32_C(0x2201), UINT32_C(0x2202), UINT32_C(0x2203),
            UINT32_C(0x00020003), UINT32_C(0x2205), UINT32_C(0x2206) } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK, UCN_CLUSTER_ROLE_MEMBER, 0U, 6U, false,
          { UINT32_C(0x2301), UINT32_C(0x2302), UINT32_C(0x00000003),
            UINT32_C(0x00000002), UINT32_C(0x2305), UINT32_C(0x2306) } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT, UCN_CLUSTER_ROLE_HEAD, 0U, 5U, true,
          { UINT32_C(0x2401), UINT32_C(0x2402), UINT32_C(0x2403),
            UINT32_C(0x00000004), UINT32_C(0x2405), 0U } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT, UCN_CLUSTER_ROLE_HEAD, 0U, 5U, true,
          { UINT32_C(0x2501), UINT32_C(0x2502), UINT32_C(0x2503),
            UINT32_C(0x00000007), UINT32_C(0x2505), 0U } },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE, UCN_CLUSTER_ROLE_HEAD, 0U, 6U, false,
          { UINT32_C(0x2601), UINT32_C(0x2602), UINT32_C(0x2603),
            UINT32_C(0x2604), UINT32_C(0x2605), UINT32_C(0x2606) } },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY, UCN_CLUSTER_ROLE_HEAD, 0U, 6U, false,
          { UINT32_C(0x2701), UINT32_C(0x2702), UINT32_C(0x2703),
            UINT32_C(0x2704), UINT32_C(0x2705), UINT32_C(0x2706) } },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT, UCN_CLUSTER_ROLE_HEAD, 0U, 6U, false,
          { UINT32_C(0x2801), UINT32_C(0x2802), UINT32_C(0x2803),
            UINT32_C(0x2804), UINT32_C(0x2805), UINT32_C(0x2806) } },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW, UCN_CLUSTER_ROLE_HEAD, 0U, 5U, true,
          { UINT32_C(0x2901), UINT32_C(0x2902), UINT32_C(0x2903),
            UINT32_C(0x2904), UINT32_C(0x2905), 0U } },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE, UCN_CLUSTER_ROLE_HEAD, 0U, 6U, false,
          { UINT32_C(0x3001), UINT32_C(0x00000001), UINT32_C(0x3003),
            UINT32_C(0x3004), UINT32_C(0x3005), UINT32_C(0x3006) } },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK, UCN_CLUSTER_ROLE_MEMBER, 0U, 6U, false,
          { UINT32_C(0x3101), UINT32_C(0x00000001), UINT32_C(0x3103),
            UINT32_C(0x3104), UINT32_C(0x3105), UINT32_C(0x3106) } },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT, UCN_CLUSTER_ROLE_HEAD, 0U, 6U, false,
          { UINT32_C(0x3201), UINT32_C(0x00000001), UINT32_C(0x3203),
            UINT32_C(0x3204), UINT32_C(0x3205), UINT32_C(0x3206) } },
        { UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE, UCN_CLUSTER_ROLE_HEAD,
          UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD, 6U, false,
          { UINT32_C(0x3301), UINT32_C(0x3302), UINT32_C(0x3303),
            UINT32_C(0x3304), UINT32_C(0x00010002), UINT32_C(0x3306) } }
    };
    size_t index;

    /* RFC4 Type 20..33 are frozen numeric identifiers.  Keep this explicit
     * rather than relying on a sequential loop, so an insertion/renumbering
     * cannot silently preserve a locally consistent parser. */
    ASSERT_TRUE(UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN == 20U &&
                UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER == 21U &&
                UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE == 22U &&
                UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK == 23U &&
                UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT == 24U &&
                UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT == 25U &&
                UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE == 26U &&
                UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY == 27U &&
                UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT == 28U &&
                UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW == 29U &&
                UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE == 30U &&
                UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK == 31U &&
                UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT == 32U &&
                UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE == 33U);

    for (index = 0U; index < sizeof(fixtures) / sizeof(fixtures[0U]); ++index) {
        const extended_type_fixture_t *fixture = &fixtures[index];
        ucn_cluster_wire_v4_frame_t frame;
        ucn_cluster_wire_v4_frame_t rebuilt;
        ucn_cluster_wire_v4_semantic_message_t semantic;
        ucn_cluster_wire_v4_semantic_message_t untouched;

        (void)memset(&frame, 0, sizeof(frame));
        frame.type = fixture->type;
        frame.role = fixture->role;
        frame.flags = fixture->flags;
        frame.cluster_id = UINT32_C(0xF001);
        frame.term = UINT32_C(0xF002);
        frame.head_node_id = UINT32_C(0xF003);
        (void)memcpy(frame.words, fixture->words, sizeof(frame.words));
        ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&frame));
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_payload_size(fixture->type) ==
                    fixture->payload_words * sizeof(uint32_t));
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_from_frame(&frame, &semantic) ==
                    UCN_OK);
        ASSERT_TRUE(semantic.header.type == fixture->type &&
                    semantic.header.role == fixture->role &&
                    semantic.header.flags == fixture->flags &&
                    semantic.header.cluster_id == frame.cluster_id &&
                    semantic.header.term == frame.term &&
                    semantic.header.head_node_id == frame.head_node_id);
        ASSERT_TRUE(extended_type_semantic_fields_match(&semantic, fixture->words));
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_to_frame(&semantic, &rebuilt) ==
                    UCN_OK);
        ASSERT_TRUE(rebuilt.type == fixture->type &&
                    rebuilt.role == fixture->role && rebuilt.flags == fixture->flags &&
                    rebuilt.cluster_id == frame.cluster_id && rebuilt.term == frame.term &&
                    rebuilt.head_node_id == frame.head_node_id &&
                    rebuilt.words[0U] == fixture->words[0U] &&
                    rebuilt.words[1U] == fixture->words[1U] &&
                    rebuilt.words[2U] == fixture->words[2U] &&
                    rebuilt.words[3U] == fixture->words[3U] &&
                    rebuilt.words[4U] == fixture->words[4U] &&
                    rebuilt.words[5U] == fixture->words[5U]);
        if (fixture->has_zero_tail) {
            frame.words[5U] = 1U;
            (void)memset(&untouched, 0x5A, sizeof(untouched));
            semantic = untouched;
            ASSERT_TRUE(ucn_cluster_wire_v4_semantic_from_frame(&frame,
                                                                 &semantic) ==
                        UCN_ERR_MALFORMED);
            ASSERT_TRUE(memcmp(&semantic, &untouched, sizeof(semantic)) == 0);
        }
    }

    /* Type 21 and 33 are the only Type 20..33 messages with non-zero flags. */
    {
        ucn_cluster_wire_v4_frame_t frame;

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER,
                                     &frame));
        frame.flags = UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_REMOVE;
        ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&frame));
        frame.flags = 0U;
        ASSERT_TRUE(!ucn_cluster_wire_v4_frame_is_valid(&frame));

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE,
                                     &frame));
        frame.flags = UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW;
        ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&frame));
        frame.flags = UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD |
                      UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW;
        ASSERT_TRUE(!ucn_cluster_wire_v4_frame_is_valid(&frame));
    }

    /* Type 23/31 have two legal non-Head senders. Type 27 derives its legal
     * sender from the frozen same-/cross-Cluster target identity. */
    {
        ucn_cluster_wire_v4_frame_t frame;

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK, &frame));
        frame.role = UCN_CLUSTER_ROLE_BACKUP;
        ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&frame));
        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK, &frame));
        frame.role = UCN_CLUSTER_ROLE_BACKUP;
        ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&frame));
        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY,
                                     &frame));
        frame.words[1U] = frame.cluster_id;
        frame.role = UCN_CLUSTER_ROLE_BACKUP;
        ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&frame));
        frame.role = UCN_CLUSTER_ROLE_HEAD;
        ASSERT_TRUE(!ucn_cluster_wire_v4_frame_is_valid(&frame));
    }
    return 0;
}

static int test_wire_offer_capability_semantics(void)
{
    ucn_cluster_wire_v4_wire_offer_t local_offer;
    ucn_cluster_wire_v4_wire_offer_t peer_offer;
    ucn_cluster_wire_v4_wire_offer_t decoded_offer;
    ucn_cluster_wire_v4_wire_offer_t untouched_offer;
    ucn_cluster_wire_v4_selected_wire_offer_t selected_offer;
    ucn_cluster_wire_v4_selected_wire_offer_t decoded_selected;
    ucn_cluster_wire_v4_selected_wire_offer_t expected_selected;
    ucn_cluster_wire_v4_selected_wire_offer_t untouched_selected;
    ucn_cluster_wire_v4_frame_t frame;
    uint32_t word;
    uint32_t untouched_word;

    (void)memset(&local_offer, 0, sizeof(local_offer));
    (void)memset(&peer_offer, 0, sizeof(peer_offer));
    (void)memset(&decoded_offer, 0xA5, sizeof(decoded_offer));
    (void)memset(&selected_offer, 0xA5, sizeof(selected_offer));
    (void)memset(&decoded_selected, 0xA5, sizeof(decoded_selected));
    (void)memset(&expected_selected, 0, sizeof(expected_selected));
    local_offer.minimum_format = 4U;
    local_offer.maximum_format = 6U;
    local_offer.capabilities = (uint16_t)(
        UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP |
        UCN_CLUSTER_WIRE_V4_CAPABILITY_TAKEOVER);
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_to_word(&local_offer, &word) ==
                UCN_OK);
    ASSERT_TRUE(word == UINT32_C(0x04060003));
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_from_word(word, &decoded_offer) ==
                UCN_OK);
    ASSERT_TRUE(memcmp(&decoded_offer, &local_offer, sizeof(local_offer)) == 0);
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_supports(
        &local_offer, UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP));
    ASSERT_TRUE(!ucn_cluster_wire_v4_wire_offer_supports(
        &local_offer, UCN_CLUSTER_WIRE_V4_CAPABILITY_PERSISTENCE));

    peer_offer.minimum_format = 4U;
    peer_offer.maximum_format = 5U;
    peer_offer.capabilities = (uint16_t)(
        UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP |
        UCN_CLUSTER_WIRE_V4_CAPABILITY_PERSISTENCE);
    expected_selected.format = 5U;
    expected_selected.capabilities = UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP;
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_negotiate(
                    &local_offer, &peer_offer,
                    UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP,
                    &selected_offer) == UCN_OK);
    ASSERT_TRUE(selected_offer.format == 5U &&
                selected_offer.capabilities == UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP);
    /* Success must canonically overwrite the complete object, including ABI
     * padding, even if the caller supplied nonzero bytes. */
    ASSERT_TRUE(memcmp(&selected_offer, &expected_selected,
                       sizeof(selected_offer)) == 0);
    ASSERT_TRUE(ucn_cluster_wire_v4_selected_wire_offer_to_word(
                    &selected_offer, &word) == UCN_OK);
    ASSERT_TRUE(word == UINT32_C(0x00050001));
    ASSERT_TRUE(ucn_cluster_wire_v4_selected_wire_offer_from_word(
                    word, &decoded_selected) == UCN_OK);
    ASSERT_TRUE(memcmp(&decoded_selected, &expected_selected,
                       sizeof(selected_offer)) == 0);

    /* Selection is a pure common-set operation. Missing required capability
     * or a non-RFC4 range rejects without touching the selected output. */
    (void)memset(&untouched_selected, 0x5A, sizeof(untouched_selected));
    selected_offer = untouched_selected;
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_negotiate(
                    &local_offer, &peer_offer,
                    UCN_CLUSTER_WIRE_V4_CAPABILITY_TAKEOVER,
                    &selected_offer) == UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&selected_offer, &untouched_selected,
                       sizeof(selected_offer)) == 0);
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_negotiate(
                    &local_offer, &peer_offer, UINT16_C(0x0040),
                    &selected_offer) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&selected_offer, &untouched_selected,
                       sizeof(selected_offer)) == 0);

    (void)memset(&untouched_word, 0x5A, sizeof(untouched_word));
    word = untouched_word;
    decoded_offer = local_offer;
    decoded_offer.minimum_format = 5U;
    decoded_offer.maximum_format = 6U;
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_to_word(&decoded_offer, &word) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(word == untouched_word);
    selected_offer = untouched_selected;
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_negotiate(
                    &decoded_offer, &peer_offer,
                    UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP,
                    &selected_offer) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&selected_offer, &untouched_selected,
                       sizeof(selected_offer)) == 0);
    (void)memset(&untouched_offer, 0x5A, sizeof(untouched_offer));
    decoded_offer = untouched_offer;
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_from_word(UINT32_C(0x04040040),
                                                          &decoded_offer) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&decoded_offer, &untouched_offer, sizeof(decoded_offer)) ==
                0);
    decoded_offer = local_offer;
    decoded_offer.capabilities = UINT16_C(0x0040);
    word = untouched_word;
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_to_word(&decoded_offer, &word) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(word == untouched_word);

    (void)memset(&untouched_selected, 0x5A, sizeof(untouched_selected));
    decoded_selected = untouched_selected;
    ASSERT_TRUE(ucn_cluster_wire_v4_selected_wire_offer_from_word(
                    UINT32_C(0x01040001), &decoded_selected) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&decoded_selected, &untouched_selected,
                       sizeof(decoded_selected)) == 0);
    decoded_selected.format = 0U;
    decoded_selected.capabilities = UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP;
    word = untouched_word;
    ASSERT_TRUE(ucn_cluster_wire_v4_selected_wire_offer_to_word(
                    &decoded_selected, &word) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(word == untouched_word);

    /* All RFC4 field owners retain their exact existing word layout. */
    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE, &frame));
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_from_word(frame.words[3U],
                                                          &decoded_offer) == UCN_OK);
    ASSERT_TRUE(decoded_offer.minimum_format == 4U &&
                decoded_offer.maximum_format == 4U &&
                decoded_offer.capabilities == UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP);
    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST, &frame));
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_from_word(frame.words[1U],
                                                          &decoded_offer) == UCN_OK);
    ASSERT_TRUE(decoded_offer.minimum_format == 4U &&
                decoded_offer.maximum_format == 4U &&
                decoded_offer.capabilities == UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP);
    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE, &frame));
    ASSERT_TRUE(ucn_cluster_wire_v4_wire_offer_from_word(frame.words[3U],
                                                          &decoded_offer) == UCN_OK);
    ASSERT_TRUE(decoded_offer.minimum_format == 4U &&
                decoded_offer.maximum_format == 4U &&
                decoded_offer.capabilities == UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP);
    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT, &frame));
    ASSERT_TRUE(ucn_cluster_wire_v4_selected_wire_offer_from_word(
                    frame.words[4U], &decoded_selected) == UCN_OK);
    ASSERT_TRUE(decoded_selected.format == 4U &&
                decoded_selected.capabilities == UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP);
    return 0;
}

static int test_mixed_version_policy(void)
{
    ucn_cluster_wire_v4_wire_offer_t v4_offer;
    ucn_cluster_wire_v4_wire_offer_t invalid_offer;
    static const ucn_cluster_wire_v4_peer_contract_class_t protected_classes[] = {
        UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_VOTER,
        UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_BACKUP,
        UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_HEAD
    };
    size_t index;

    v4_offer.minimum_format = 4U;
    v4_offer.maximum_format = 4U;
    v4_offer.capabilities = (uint16_t)(
        UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP |
        UCN_CLUSTER_WIRE_V4_CAPABILITY_TAKEOVER);

    /* Strict v4 must reject every v3 request, including a non-voting one. */
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_V3, NULL,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    0U) == UCN_ERR_STATE);
    for (index = 0U;
         index < sizeof(protected_classes) / sizeof(protected_classes[0U]);
         ++index) {
        ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                        UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                        UCN_CLUSTER_WIRE_FORMAT_V3, NULL,
                        protected_classes[index], 0U) == UCN_ERR_STATE);
    }

    /* Legacy compatibility is explicit and permits precisely one v3 class. */
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY,
                    UCN_CLUSTER_WIRE_FORMAT_V3, NULL,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    0U) == UCN_OK);
    for (index = 0U;
         index < sizeof(protected_classes) / sizeof(protected_classes[0U]);
         ++index) {
        ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                        UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY,
                        UCN_CLUSTER_WIRE_FORMAT_V3, NULL,
                        protected_classes[index], 0U) == UCN_ERR_STATE);
    }

    /* A v4 peer remains a pure required-bit check; this function does not
     * grant any role.  Future owners must supply their own exact requirements. */
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_V4, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_V4, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_BACKUP,
                    UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY,
                    UCN_CLUSTER_WIRE_FORMAT_V4, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_HEAD,
                    UCN_CLUSTER_WIRE_V4_CAPABILITY_TAKEOVER) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_V4, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_VOTER,
                    UCN_CLUSTER_WIRE_V4_CAPABILITY_PERSISTENCE) == UCN_ERR_STATE);

    /* v3 has no wire_offer: manufactured v4 bits, malformed offers and all
     * unknown policy/class/format inputs fail closed. */
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY,
                    UCN_CLUSTER_WIRE_FORMAT_V3, NULL,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY,
                    UCN_CLUSTER_WIRE_FORMAT_V3, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    0U) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_INVALID,
                    UCN_CLUSTER_WIRE_FORMAT_V4, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    0U) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_INVALID, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    0U) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_V4, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_INVALID,
                    0U) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_V4, &v4_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    UINT16_C(0x0040)) == UCN_ERR_ARGUMENT);
    invalid_offer = v4_offer;
    invalid_offer.minimum_format = 5U;
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_V4, &invalid_offer,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    0U) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(ucn_cluster_wire_v4_mixed_version_peer_is_compatible(
                    UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4,
                    UCN_CLUSTER_WIRE_FORMAT_V4, NULL,
                    UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER,
                    0U) == UCN_ERR_ARGUMENT);
    return 0;
}

static int test_versioned_diagnostics(void)
{
    ucn_cluster_wire_v4_peer_diagnostic_input_t input;
    ucn_cluster_wire_v4_peer_diagnostic_view_t view;
    ucn_cluster_wire_v4_peer_diagnostic_view_t untouched;
    ucn_cluster_wire_v4_peer_diagnostic_stats_t stats;
    uint8_t view_before[sizeof(view)];
    static const ucn_cluster_wire_v4_peer_diagnostic_reason_t all_reasons[] = {
        UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V4,
        UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V3_LEGACY,
        UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_STRICT_V4_REQUIRES_V4,
        UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V3_LEGACY_NON_VOTING_ONLY,
        UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_OFFER_MISSING,
        UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_OFFER_INVALID,
        UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_REQUIRED_CAPABILITY_MISSING,
        UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_INPUT_INVALID
    };
    size_t index;

    (void)memset(&input, 0, sizeof(input));
    (void)memset(&stats, 0, sizeof(stats));
    input.peer_node_id = UINT32_C(0x01020304);
    input.policy = UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4;
    input.peer_format = UCN_CLUSTER_WIRE_FORMAT_V3;
    input.requested_class =
        UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER;

    /* Strict v4 must explain the rejection without claiming a real member or
     * role decision. */
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) == UCN_OK);
    ASSERT_TRUE(view.peer_node_id == input.peer_node_id &&
                view.peer_format == UCN_CLUSTER_WIRE_FORMAT_V3 &&
                !view.peer_offer_present && view.compatibility == UCN_ERR_STATE &&
                view.reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_STRICT_V4_REQUIRES_V4);
    ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                    &stats, view.reason) == UCN_OK);
    ASSERT_TRUE(stats.evaluated == 1U && stats.rejected == 1U);

    input.policy =
        UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_ALLOW_V3_NON_VOTING_LEGACY;
    input.requested_class = UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_BACKUP;
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) == UCN_OK);
    ASSERT_TRUE(view.compatibility == UCN_ERR_STATE &&
                view.reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V3_LEGACY_NON_VOTING_ONLY);

    input.requested_class =
        UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_NON_VOTING_MEMBER;
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) == UCN_OK);
    ASSERT_TRUE(view.compatibility == UCN_OK &&
                view.reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V3_LEGACY);
    ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                    &stats, view.reason) == UCN_OK);
    ASSERT_TRUE(stats.evaluated == 2U && stats.compatible_v3_legacy == 1U);

    input.peer_format = UCN_CLUSTER_WIRE_FORMAT_V4;
    input.policy = UCN_CLUSTER_WIRE_V4_MIXED_VERSION_POLICY_STRICT_V4;
    input.requested_class = UCN_CLUSTER_WIRE_V4_PEER_CONTRACT_CLASS_BACKUP;
    input.required_v4_capabilities = UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP;
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) == UCN_OK);
    ASSERT_TRUE(view.compatibility == UCN_ERR_STATE &&
                view.reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_OFFER_MISSING);

    input.peer_offer_present = true;
    input.peer_offer.minimum_format = 5U;
    input.peer_offer.maximum_format = 5U;
    input.peer_offer.capabilities = UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP;
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) == UCN_OK);
    ASSERT_TRUE(view.compatibility == UCN_ERR_STATE &&
                view.reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_OFFER_INVALID);

    input.peer_offer.minimum_format = 4U;
    input.peer_offer.maximum_format = 4U;
    input.peer_offer.capabilities = UCN_CLUSTER_WIRE_V4_CAPABILITY_TAKEOVER;
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) == UCN_OK);
    ASSERT_TRUE(view.compatibility == UCN_ERR_STATE &&
                view.reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_V4_REQUIRED_CAPABILITY_MISSING);
    ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                    &stats, view.reason) == UCN_OK);
    ASSERT_TRUE(stats.evaluated == 3U && stats.rejected == 2U);

    input.peer_offer.capabilities = (uint16_t)(
        UCN_CLUSTER_WIRE_V4_CAPABILITY_BACKUP |
        UCN_CLUSTER_WIRE_V4_CAPABILITY_TAKEOVER);
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) == UCN_OK);
    ASSERT_TRUE(view.compatibility == UCN_OK &&
                view.reason == UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V4 &&
                view.peer_offer.capabilities == input.peer_offer.capabilities);
    ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                    &stats, view.reason) == UCN_OK);
    ASSERT_TRUE(stats.evaluated == 4U && stats.compatible_v4 == 1U);

    /* Invalid observations do not overwrite a caller's last valid view. */
    untouched = view;
    (void)memcpy(view_before, &view, sizeof(view));
    input.peer_node_id = 0U;
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&view, view_before, sizeof(view)) == 0);
    ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                    &stats,
                    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_INPUT_INVALID) ==
                UCN_OK);
    ASSERT_TRUE(stats.invalid_inputs == 1U);

    /* Every reason has a deterministic caller-owned stats bucket. */
    (void)memset(&stats, 0, sizeof(stats));
    for (index = 0U; index < sizeof(all_reasons) / sizeof(all_reasons[0U]); ++index) {
        ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                        &stats, all_reasons[index]) == UCN_OK);
    }
    ASSERT_TRUE(stats.evaluated == 7U && stats.compatible_v4 == 1U &&
                stats.compatible_v3_legacy == 1U && stats.rejected == 5U &&
                stats.invalid_inputs == 1U);
    ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                    NULL, untouched.reason) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                    &stats,
                    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_INVALID) ==
                UCN_ERR_ARGUMENT);

    input.peer_node_id = UINT32_C(0x01020304);
    input.peer_format = UCN_CLUSTER_WIRE_FORMAT_INVALID;
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&view, view_before, sizeof(view)) == 0);
    input.peer_format = UCN_CLUSTER_WIRE_FORMAT_V3;
    input.peer_offer_present = true;
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&view, view_before, sizeof(view)) == 0);
    input.peer_offer_present = false;
    input.required_v4_capabilities = UINT16_C(0x0040);
    ASSERT_TRUE(ucn_cluster_wire_v4_diagnose_peer(&input, &view) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&view, view_before, sizeof(view)) == 0);

    stats.evaluated = UINT32_MAX;
    stats.compatible_v4 = UINT32_MAX;
    ASSERT_TRUE(ucn_cluster_wire_v4_peer_diagnostic_stats_record(
                    &stats,
                    UCN_CLUSTER_WIRE_V4_PEER_DIAGNOSTIC_REASON_COMPATIBLE_V4) ==
                UCN_OK);
    ASSERT_TRUE(stats.evaluated == UINT32_MAX && stats.compatible_v4 == UINT32_MAX);
    return 0;
}

static int test_each_type_round_trip(void)
{
    uint32_t type;

    for (type = UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE;
         type <= UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE; ++type) {
        ucn_cluster_wire_v4_frame_t input;
        ucn_cluster_wire_v4_frame_t decoded;
        uint8_t encoded[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];
        ucn_cluster_wire_v4_frame_t untouched;
        uint8_t untouched_before[sizeof(untouched)];

        ASSERT_TRUE(make_valid_frame((uint8_t)type, &input));
        if (!ucn_cluster_wire_v4_frame_is_valid(&input)) {
            printf("invalid generated v4 type: %lu\n", (unsigned long)type);
            return 1;
        }
        ASSERT_TRUE(ucn_cluster_wire_v4_encode(&input, encoded) == UCN_OK);
        ASSERT_TRUE(ucn_cluster_wire_v4_decode(encoded, sizeof(encoded), &decoded) ==
                    UCN_OK);
        ASSERT_TRUE(memcmp(&input, &decoded, sizeof(input)) == 0);

        /* Every frozen type rejects an impossible role and a flag bit that
         * RFC4 assigns to no Type.  A rejected decode must not modify output. */
        (void)memset(&untouched, 0xA5, sizeof(untouched));
        (void)memcpy(untouched_before, &untouched, sizeof(untouched));
        encoded[2U] = (uint8_t)UCN_CLUSTER_ROLE_DISABLED;
        ASSERT_TRUE(ucn_cluster_wire_v4_decode(encoded, sizeof(encoded), &untouched) ==
                    UCN_ERR_MALFORMED);
        ASSERT_TRUE(memcmp(&untouched, untouched_before, sizeof(untouched)) == 0);
        encoded[2U] = (uint8_t)input.role;
        encoded[3U] = UINT8_C(0x08);
        ASSERT_TRUE(ucn_cluster_wire_v4_decode(encoded, sizeof(encoded), &untouched) ==
                    UCN_ERR_MALFORMED);
        ASSERT_TRUE(memcmp(&untouched, untouched_before, sizeof(untouched)) == 0);
    }
    return 0;
}

typedef struct v4_negative_fixture {
    uint8_t type;
    uint8_t required_nonzero_word;
    uint8_t zero_tail_word;
} v4_negative_fixture_t;

static int test_all_type_negative_matrix(void)
{
    static const v4_negative_fixture_t fixtures[] = {
        { UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE, 2U, 4U },
        { UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT, 0U, 3U },
        { UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE, 1U, 2U },
        { UCN_CLUSTER_WIRE_V4_MSG_LEAVE, 0U, 2U },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE, 2U, 4U },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN, 0U, 5U },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN, 0U, 5U },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE, 1U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK, 0U, 2U },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ, 0U, 4U },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT, 0U, 4U },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT, 0U, 5U },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT, 0U, 5U },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW, 0U, 5U },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT, 0U, V4_TEST_NO_ZERO_TAIL },
        { UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE, 0U,
          V4_TEST_NO_ZERO_TAIL }
    };
    size_t index;

    ASSERT_TRUE(sizeof(fixtures) / sizeof(fixtures[0U]) ==
                UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE);
    for (index = 0U; index < sizeof(fixtures) / sizeof(fixtures[0U]); ++index) {
        const v4_negative_fixture_t *fixture = &fixtures[index];
        ucn_cluster_wire_v4_frame_t frame;
        ucn_cluster_wire_v4_frame_t broken;
        uint8_t raw[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];

        ASSERT_TRUE(fixture->type == index + 1U);
        ASSERT_TRUE(make_valid_frame(fixture->type, &frame));
        ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&frame));
        raw_from_frame_for_test(&frame, raw);
        ASSERT_TRUE(raw_decode_contract_holds(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        raw[V4_TEST_ROLE_OFFSET] = (uint8_t)UCN_CLUSTER_ROLE_DISABLED;
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        raw[V4_TEST_FLAGS_OFFSET] = invalid_flags_for_type(frame.type);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        raw[V4_TEST_FLAGS_OFFSET] = (uint8_t)(frame.flags | UINT8_C(0x08));
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        test_write_u32_be(raw + V4_TEST_CLUSTER_ID_OFFSET, 0U);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        test_write_u32_be(raw + V4_TEST_CLUSTER_ID_OFFSET, UCN_NODE_BROADCAST);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        test_write_u32_be(raw + V4_TEST_TERM_OFFSET, 0U);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        test_write_u32_be(raw + V4_TEST_TERM_OFFSET, UINT32_MAX);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        test_write_u32_be(raw + V4_TEST_HEAD_ID_OFFSET, 0U);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        test_write_u32_be(raw + V4_TEST_HEAD_ID_OFFSET, UCN_NODE_BROADCAST);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));

        raw_from_frame_for_test(&frame, raw);
        raw_set_word_for_test(raw, fixture->required_nonzero_word, 0U);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));
        broken = frame;
        broken.words[fixture->required_nonzero_word] = 0U;
        ASSERT_TRUE(semantic_rejection_keeps_output_unchanged(&broken));

        if (fixture->zero_tail_word != V4_TEST_NO_ZERO_TAIL) {
            raw_from_frame_for_test(&frame, raw);
            raw_set_word_for_test(raw, fixture->zero_tail_word, UINT32_C(0xA5A5A5A5));
            ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));
        }
    }

    /* Type 12 has an additional BEGIN/END marker form whose unused tail must
     * stay zero.  It is not covered by the normal DELTA fixture above. */
    {
        ucn_cluster_wire_v4_frame_t marker;
        uint8_t raw[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC,
                                     &marker));
        marker.flags = UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN;
        marker.words[3U] = 0U;
        marker.words[4U] = 0U;
        marker.words[5U] = 0U;
        ASSERT_TRUE(ucn_cluster_wire_v4_frame_is_valid(&marker));
        raw_from_frame_for_test(&marker, raw);
        ASSERT_TRUE(raw_decode_contract_holds(raw, sizeof(raw)));
        raw_set_word_for_test(raw, 3U, 1U);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));
    }

    {
        ucn_cluster_wire_v4_frame_t frame;
        uint8_t raw[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE, &frame));
        raw_from_frame_for_test(&frame, raw);
        raw[V4_TEST_VERSION_OFFSET] = UCN_CLUSTER_WIRE_V3_FORMAT_VERSION;
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));
        raw_from_frame_for_test(&frame, raw);
        raw[V4_TEST_VERSION_OFFSET] = UINT8_C(0x05);
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));
        raw_from_frame_for_test(&frame, raw);
        raw[V4_TEST_TYPE_OFFSET] = 0U;
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));
        raw_from_frame_for_test(&frame, raw);
        raw[V4_TEST_TYPE_OFFSET] =
            UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE + 1U;
        ASSERT_TRUE(raw_rejection_keeps_output_unchanged(raw, sizeof(raw)));
    }
    return 0;
}

typedef enum v4_rfc4_word_rule {
    V4_RFC4_WORD_ANY = 0,
    V4_RFC4_WORD_ZERO,
    V4_RFC4_WORD_NONCE,
    V4_RFC4_WORD_SERIAL,
    V4_RFC4_WORD_OPTIONAL_SERIAL,
    V4_RFC4_WORD_NODE_ID,
    V4_RFC4_WORD_CLUSTER_ID,
    V4_RFC4_WORD_SUCCESSOR_CLUSTER_ID,
    V4_RFC4_WORD_DURATION,
    V4_RFC4_WORD_SCORE_CAPACITY,
    V4_RFC4_WORD_WIRE_OFFER,
    V4_RFC4_WORD_SELECTED_WIRE_OFFER,
    V4_RFC4_WORD_REASON,
    V4_RFC4_WORD_MEMBER_FLAGS,
    V4_RFC4_WORD_CAPABILITY_BITMAP,
    V4_RFC4_WORD_ORDINAL_COUNT,
    V4_RFC4_WORD_VOTER_COUNT_PAIR,
    V4_RFC4_WORD_VOTER_SLOT,
    V4_RFC4_WORD_VOTER_COUNT,
    V4_RFC4_WORD_CONFIG_PHASE,
    V4_RFC4_WORD_CERTIFICATE_MASK,
    V4_RFC4_WORD_EXACTLY_ONE,
    V4_RFC4_WORD_CERTIFICATE_DESCRIPTOR
} v4_rfc4_word_rule_t;

typedef enum v4_rfc4_flag_contract {
    V4_RFC4_FLAGS_ZERO = 0,
    V4_RFC4_FLAGS_SYNC,
    V4_RFC4_FLAGS_CONFIG_MEMBER,
    V4_RFC4_FLAGS_CERTIFICATE_SET
} v4_rfc4_flag_contract_t;

typedef struct v4_rfc4_fixture {
    uint8_t type;
    uint16_t role_mask;
    v4_rfc4_flag_contract_t flag_contract;
    v4_rfc4_word_rule_t words[UCN_CLUSTER_WIRE_V4_WORD_COUNT];
} v4_rfc4_fixture_t;

#define V4_RFC4_ROLE_BIT(role) ((uint16_t)(UINT16_C(1) << (uint8_t)(role)))

static bool rfc4_flags_are_allowed(v4_rfc4_flag_contract_t contract,
                                   uint8_t flags)
{
    switch (contract) {
    case V4_RFC4_FLAGS_ZERO:
        return flags == 0U;
    case V4_RFC4_FLAGS_SYNC:
        return flags == 0U || flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN ||
               flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END ||
               flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_DELTA;
    case V4_RFC4_FLAGS_CONFIG_MEMBER:
        return flags == UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_ADD ||
               flags == UCN_CLUSTER_WIRE_V4_FLAG_CONFIG_MEMBER_REMOVE;
    case V4_RFC4_FLAGS_CERTIFICATE_SET:
        return flags == UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD ||
               flags == UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW;
    default:
        return false;
    }
}

static void rfc4_prepare_flag_variant(ucn_cluster_wire_v4_frame_t *frame,
                                      uint8_t flags)
{
    frame->flags = flags;
    if (frame->type == UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC &&
        (flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN ||
         flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END)) {
        frame->words[3U] = 0U;
        frame->words[4U] = 0U;
        frame->words[5U] = 0U;
    }
}

static bool rfc4_frame_is_accepted_by_all_layers(
    const ucn_cluster_wire_v4_frame_t *frame)
{
    ucn_cluster_wire_v4_frame_t decoded;
    ucn_cluster_wire_v4_semantic_message_t semantic;
    ucn_cluster_wire_message_t message;
    uint8_t raw[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];

    raw_from_frame_for_test(frame, raw);
    return ucn_cluster_wire_v4_decode(raw, sizeof(raw), &decoded) == UCN_OK &&
           memcmp(&decoded, frame, sizeof(decoded)) == 0 &&
           ucn_cluster_wire_v4_semantic_from_frame(&decoded, &semantic) == UCN_OK &&
           ucn_cluster_wire_decode(raw, sizeof(raw), &message) == UCN_OK &&
           message.format == UCN_CLUSTER_WIRE_FORMAT_V4 &&
           memcmp(&message.body.v4, frame, sizeof(*frame)) == 0;
}

static bool rfc4_frame_is_rejected_by_all_layers(
    const ucn_cluster_wire_v4_frame_t *frame)
{
    uint8_t raw[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];

    raw_from_frame_for_test(frame, raw);
    return raw_rejection_keeps_output_unchanged(raw, sizeof(raw)) &&
           semantic_rejection_keeps_output_unchanged(frame);
}

static size_t rfc4_invalid_values(v4_rfc4_word_rule_t rule,
                                  const ucn_cluster_wire_v4_frame_t *frame,
                                  uint32_t values[3U])
{
    switch (rule) {
    case V4_RFC4_WORD_ZERO:
        values[0U] = 1U;
        return 1U;
    case V4_RFC4_WORD_NONCE:
        values[0U] = 0U;
        return 1U;
    case V4_RFC4_WORD_SERIAL:
        values[0U] = 0U;
        values[1U] = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
        return 2U;
    case V4_RFC4_WORD_OPTIONAL_SERIAL:
        values[0U] = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
        return 1U;
    case V4_RFC4_WORD_NODE_ID:
    case V4_RFC4_WORD_CLUSTER_ID:
        values[0U] = 0U;
        values[1U] = UCN_NODE_BROADCAST;
        return 2U;
    case V4_RFC4_WORD_SUCCESSOR_CLUSTER_ID:
        values[0U] = 0U;
        values[1U] = UCN_NODE_BROADCAST;
        values[2U] = frame->cluster_id;
        return 3U;
    case V4_RFC4_WORD_DURATION:
        values[0U] = 0U;
        values[1U] = UCN_MAX_SAFE_DURATION_MS + 1U;
        return 2U;
    case V4_RFC4_WORD_SCORE_CAPACITY:
        values[0U] = UINT32_C(0xFFFF0000);
        return 1U;
    case V4_RFC4_WORD_WIRE_OFFER:
        values[0U] = UINT32_C(0x03030001);
        values[1U] = UINT32_C(0x04040040);
        return 2U;
    case V4_RFC4_WORD_SELECTED_WIRE_OFFER:
        values[0U] = UINT32_C(0x00000001);
        values[1U] = UINT32_C(0x01040001);
        values[2U] = UINT32_C(0x00040040);
        return 3U;
    case V4_RFC4_WORD_REASON:
        values[0U] = 0U;
        values[1U] = 11U;
        return 2U;
    case V4_RFC4_WORD_MEMBER_FLAGS:
        values[0U] = UINT32_C(0x02);
        return 1U;
    case V4_RFC4_WORD_CAPABILITY_BITMAP:
        values[0U] = UINT32_C(0x40);
        return 1U;
    case V4_RFC4_WORD_ORDINAL_COUNT:
        values[0U] = 0U;
        values[1U] = UINT32_C(0x00010001);
        return 2U;
    case V4_RFC4_WORD_VOTER_COUNT_PAIR:
        values[0U] = 0U;
        values[1U] = UINT32_C(0x00220001);
        return 2U;
    case V4_RFC4_WORD_VOTER_SLOT:
        values[0U] = (uint32_t)UCN_CLUSTER_MAX_MEMBERS + 1U;
        return 1U;
    case V4_RFC4_WORD_VOTER_COUNT:
        values[0U] = 0U;
        values[1U] = (uint32_t)UCN_CLUSTER_MAX_MEMBERS + 2U;
        return 2U;
    case V4_RFC4_WORD_CONFIG_PHASE:
        values[0U] = 0U;
        values[1U] = 4U;
        return 2U;
    case V4_RFC4_WORD_CERTIFICATE_MASK:
        values[0U] = 0U;
        values[1U] = UINT32_C(0x02);
        return 2U;
    case V4_RFC4_WORD_EXACTLY_ONE:
        values[0U] = 0U;
        values[1U] = 2U;
        return 2U;
    case V4_RFC4_WORD_CERTIFICATE_DESCRIPTOR:
        values[0U] = 0U;
        values[1U] = 3U;
        values[2U] = UINT32_C(0x00010001);
        return 3U;
    case V4_RFC4_WORD_ANY:
    default:
        return 0U;
    }
}

static int test_rfc4_full_field_contract(void)
{
    static const v4_rfc4_fixture_t fixtures[] = {
        { UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_CANDIDATE) |
              V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD),
          V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SCORE_CAPACITY, V4_RFC4_WORD_DURATION,
            V4_RFC4_WORD_NONCE, V4_RFC4_WORD_WIRE_OFFER,
            V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_JOIN_REQUEST,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_JOIN_PENDING),
          V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_WIRE_OFFER,
            V4_RFC4_WORD_OPTIONAL_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SCORE_CAPACITY, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_JOIN_ACCEPT,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD) |
              V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_RECOVERY_HEAD),
          V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_DURATION, V4_RFC4_WORD_MEMBER_FLAGS,
            V4_RFC4_WORD_SELECTED_WIRE_OFFER, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_JOIN_REJECT,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD) |
              V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_RECOVERY_HEAD),
          V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_NONCE, V4_RFC4_WORD_REASON, V4_RFC4_WORD_DURATION,
            V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_MEMBER), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_DURATION, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO,
            V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_LEAVE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_MEMBER), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_NONCE, V4_RFC4_WORD_REASON, V4_RFC4_WORD_ZERO,
            V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_DECLARE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SCORE_CAPACITY, V4_RFC4_WORD_DURATION,
            V4_RFC4_WORD_NONCE, V4_RFC4_WORD_WIRE_OFFER,
            V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_CERTIFICATE_MASK,
            V4_RFC4_WORD_ANY } },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_STEPDOWN,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_CLUSTER_ID, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NODE_ID, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_ASSIGN,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NODE_ID, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_READY,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_BACKUP), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_SYNC,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NODE_ID, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_DURATION } },
        { UCN_CLUSTER_WIRE_V4_MSG_PRIMARY_HEARTBEAT,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_DURATION, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_PREPARE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_BACKUP), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_ACK,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_MEMBER), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_DECLARE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_RECOVERY_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_CLUSTER_ID, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_DURATION } },
        { UCN_CLUSTER_WIRE_V4_MSG_RECOVERY_ACK,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_MEMBER), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_NONCE, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO,
            V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_RESYNC_REQ,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_BACKUP), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_BACKUP_REJECT,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_BACKUP), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_REASON,
            V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_BEGIN,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NONCE, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_MEMBER,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_CONFIG_MEMBER,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NODE_ID,
            V4_RFC4_WORD_NONCE, V4_RFC4_WORD_CAPABILITY_BITMAP,
            V4_RFC4_WORD_ORDINAL_COUNT } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_PREPARE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_NONCE,
            V4_RFC4_WORD_VOTER_COUNT_PAIR, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ACK,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_MEMBER) |
              V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_BACKUP),
          V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_VOTER_SLOT,
            V4_RFC4_WORD_CONFIG_PHASE, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_COMMIT,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE,
            V4_RFC4_WORD_VOTER_COUNT, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_CONFIG_ABORT,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_REASON, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_PREPARE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_CLUSTER_ID, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NODE_ID, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_CLUSTER_ID, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NODE_ID, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_COMMIT,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_CLUSTER_ID, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NODE_ID, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_HEAD_WITHDRAW,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_CLUSTER_ID, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NODE_ID, V4_RFC4_WORD_NONCE, V4_RFC4_WORD_ZERO } },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_PREPARE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SUCCESSOR_CLUSTER_ID, V4_RFC4_WORD_EXACTLY_ONE,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_ACK,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_MEMBER) |
              V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_BACKUP),
          V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SUCCESSOR_CLUSTER_ID, V4_RFC4_WORD_EXACTLY_ONE,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_REKEY_COMMIT,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD), V4_RFC4_FLAGS_ZERO,
          { V4_RFC4_WORD_SUCCESSOR_CLUSTER_ID, V4_RFC4_WORD_EXACTLY_ONE,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_NONCE } },
        { UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE,
          V4_RFC4_ROLE_BIT(UCN_CLUSTER_ROLE_HEAD),
          V4_RFC4_FLAGS_CERTIFICATE_SET,
          { V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_SERIAL,
            V4_RFC4_WORD_SERIAL, V4_RFC4_WORD_CERTIFICATE_DESCRIPTOR,
            V4_RFC4_WORD_ANY } }
    };
    size_t fixture_index;

    ASSERT_TRUE(sizeof(fixtures) / sizeof(fixtures[0U]) ==
                UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE);
    for (fixture_index = 0U;
         fixture_index < sizeof(fixtures) / sizeof(fixtures[0U]);
         ++fixture_index) {
        const v4_rfc4_fixture_t *fixture = &fixtures[fixture_index];
        ucn_cluster_wire_v4_frame_t frame;
        uint8_t role;
        uint16_t flags;
        size_t word_index;

        ASSERT_TRUE(fixture->type == fixture_index + 1U);
        ASSERT_TRUE(make_valid_frame(fixture->type, &frame));
        ASSERT_TRUE(rfc4_frame_is_accepted_by_all_layers(&frame));

        for (role = UCN_CLUSTER_ROLE_DISABLED;
             role <= UCN_CLUSTER_ROLE_TERM_CONFLICT; ++role) {
            ucn_cluster_wire_v4_frame_t candidate = frame;
            bool allowed = (fixture->role_mask & V4_RFC4_ROLE_BIT(role)) != 0U;

            candidate.role = (ucn_cluster_role_t)role;
            ASSERT_TRUE(allowed ? rfc4_frame_is_accepted_by_all_layers(&candidate) :
                                  rfc4_frame_is_rejected_by_all_layers(&candidate));
        }

        for (flags = 0U; flags <= UINT8_MAX; ++flags) {
            ucn_cluster_wire_v4_frame_t candidate = frame;
            bool allowed = rfc4_flags_are_allowed(fixture->flag_contract,
                                                  (uint8_t)flags);

            rfc4_prepare_flag_variant(&candidate, (uint8_t)flags);
            ASSERT_TRUE(allowed ? rfc4_frame_is_accepted_by_all_layers(&candidate) :
                                  rfc4_frame_is_rejected_by_all_layers(&candidate));
        }

        for (word_index = 0U; word_index < UCN_CLUSTER_WIRE_V4_WORD_COUNT;
             ++word_index) {
            uint32_t invalid_values[3U];
            size_t invalid_count = rfc4_invalid_values(fixture->words[word_index],
                                                        &frame, invalid_values);
            size_t invalid_index;

            for (invalid_index = 0U; invalid_index < invalid_count;
                 ++invalid_index) {
                ucn_cluster_wire_v4_frame_t candidate = frame;

                candidate.words[word_index] = invalid_values[invalid_index];
                if (!rfc4_frame_is_rejected_by_all_layers(&candidate)) {
                    printf("RFC4 field contract accepted Type %u P%u value 0x%08lX\n",
                           (unsigned int)fixture->type, (unsigned int)word_index,
                           (unsigned long)invalid_values[invalid_index]);
                    return 1;
                }
            }
        }

        {
            static const uint32_t invalid_cluster_ids[] = { 0U,
                                                              UCN_NODE_BROADCAST };
            static const uint32_t invalid_terms[] = {
                0U, UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U
            };
            static const uint32_t invalid_head_ids[] = { 0U,
                                                          UCN_NODE_BROADCAST };
            size_t invalid_index;

            for (invalid_index = 0U;
                 invalid_index < sizeof(invalid_cluster_ids) /
                                     sizeof(invalid_cluster_ids[0U]);
                 ++invalid_index) {
                ucn_cluster_wire_v4_frame_t candidate = frame;

                candidate.cluster_id = invalid_cluster_ids[invalid_index];
                ASSERT_TRUE(rfc4_frame_is_rejected_by_all_layers(&candidate));
            }
            for (invalid_index = 0U;
                 invalid_index < sizeof(invalid_terms) / sizeof(invalid_terms[0U]);
                 ++invalid_index) {
                ucn_cluster_wire_v4_frame_t candidate = frame;

                candidate.term = invalid_terms[invalid_index];
                ASSERT_TRUE(rfc4_frame_is_rejected_by_all_layers(&candidate));
            }
            for (invalid_index = 0U;
                 invalid_index < sizeof(invalid_head_ids) /
                                     sizeof(invalid_head_ids[0U]);
                 ++invalid_index) {
                ucn_cluster_wire_v4_frame_t candidate = frame;

                candidate.head_node_id = invalid_head_ids[invalid_index];
                ASSERT_TRUE(rfc4_frame_is_rejected_by_all_layers(&candidate));
            }
        }
    }

    /* Type 12 marker forms have an RFC4-specific zero P3..P5 tail, and are
     * intentionally checked one field at a time rather than as one sample. */
    {
        ucn_cluster_wire_v4_frame_t marker;
        size_t word_index;

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC,
                                     &marker));
        rfc4_prepare_flag_variant(&marker, UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN);
        ASSERT_TRUE(rfc4_frame_is_accepted_by_all_layers(&marker));
        for (word_index = 3U; word_index < UCN_CLUSTER_WIRE_V4_WORD_COUNT;
             ++word_index) {
            ucn_cluster_wire_v4_frame_t candidate = marker;

            candidate.words[word_index] = 1U;
            ASSERT_TRUE(rfc4_frame_is_rejected_by_all_layers(&candidate));
        }
    }

    /* RFC4 allows a same-Cluster READY only from the already confirmed
     * Backup. Cross-Cluster READY above is intentionally a Head-only row. */
    {
        ucn_cluster_wire_v4_frame_t ready;
        uint8_t role;

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_HANDOVER_READY,
                                     &ready));
        ready.words[1U] = ready.cluster_id;
        ready.words[2U] = ready.term + 1U;
        ready.role = UCN_CLUSTER_ROLE_BACKUP;
        ASSERT_TRUE(rfc4_frame_is_accepted_by_all_layers(&ready));
        for (role = UCN_CLUSTER_ROLE_DISABLED;
             role <= UCN_CLUSTER_ROLE_TERM_CONFLICT; ++role) {
            ucn_cluster_wire_v4_frame_t candidate = ready;

            candidate.role = (ucn_cluster_role_t)role;
            ASSERT_TRUE(role == UCN_CLUSTER_ROLE_BACKUP ?
                            rfc4_frame_is_accepted_by_all_layers(&candidate) :
                            rfc4_frame_is_rejected_by_all_layers(&candidate));
        }
    }
    return 0;
}

static int test_fixed_seed_v4_decode_fuzz(void)
{
    uint32_t state = V4_TEST_FUZZ_SEED;
    size_t iteration;

    for (iteration = 0U; iteration < V4_TEST_FUZZ_ITERATIONS; ++iteration) {
        ucn_cluster_wire_v4_frame_t frame;
        uint8_t raw[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES + 1U];
        size_t mutation;
        size_t mutation_count;
        size_t input_length = UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES;

        ASSERT_TRUE(make_valid_frame(
            (uint8_t)(UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE +
                      iteration % UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE),
            &frame));
        raw_from_frame_for_test(&frame, raw);
        raw[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES] = (uint8_t)next_fuzz_word(&state);

        if ((iteration % 7U) == 0U) {
            for (mutation = 0U; mutation < UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES;
                 ++mutation) {
                raw[mutation] = (uint8_t)next_fuzz_word(&state);
            }
            raw[V4_TEST_VERSION_OFFSET] = UCN_CLUSTER_WIRE_V4_FORMAT_VERSION;
        } else {
            mutation_count = 1U + (size_t)(next_fuzz_word(&state) % 3U);
            for (mutation = 0U; mutation < mutation_count; ++mutation) {
                size_t offset = (size_t)(next_fuzz_word(&state) %
                                         UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES);
                raw[offset] ^= (uint8_t)(next_fuzz_word(&state) >> 24U) |
                               UINT8_C(0x01);
            }
        }
        if ((iteration % 19U) == 0U) {
            input_length = UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES - 1U;
        } else if ((iteration % 23U) == 0U) {
            input_length = UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES + 1U;
        }
        ASSERT_TRUE(raw_decode_contract_holds(raw, input_length));
    }
    return 0;
}

static int test_frozen_vectors_and_strict_dispatch(void)
{
    static const uint8_t v4_vectors[][UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES] = {
        { 0x04, 0x01, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
          0x00, 0x64, 0x00, 0x20, 0x00, 0x00, 0x07, 0xD0,
          0x01, 0x02, 0x03, 0x04, 0x03, 0x04, 0x00, 0x0B,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x04, 0x0C, 0x05, 0x04, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
          0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x09,
          0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x20,
          0xAB, 0xCD, 0x00, 0x01, 0x00, 0x00, 0x07, 0xD0 },
        { 0x04, 0x20, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x07, 0x0A, 0x0B, 0x0C, 0x0D,
          0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00, 0x01,
          0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x09,
          0x00, 0x00, 0x00, 0x0A, 0xDE, 0xAD, 0xBE, 0xEF },
        { 0x04, 0x09, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
          0x00, 0x00, 0x00, 0x31, 0x11, 0x22, 0x33, 0x44,
          0x00, 0x00, 0x00, 0x05, 0x55, 0x66, 0x77, 0x88,
          0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00 },
        { 0x04, 0x21, 0x05, 0x40, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x03, 0x0A, 0x0B, 0x0C, 0x0D,
          0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x09,
          0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x21,
          0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07 },
        { 0x04, 0x08, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x03, 0x0A, 0x0B, 0x0C, 0x0D,
          0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x09,
          0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x21,
          0x00, 0x00, 0x00, 0x01, 0x12, 0xD2, 0x21, 0xF9 },
        { 0x04, 0x1A, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
          0x00, 0x00, 0x00, 0x31, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x03, 0x55, 0x66, 0x77, 0x88,
          0x00, 0x00, 0x00, 0x0F, 0xAA, 0xBB, 0xCC, 0xDD },
        { 0x04, 0x1B, 0x06, 0x00, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
          0x00, 0x00, 0x00, 0x31, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x03, 0x55, 0x66, 0x77, 0x88,
          0x00, 0x00, 0x00, 0x0F, 0xAA, 0xBB, 0xCC, 0xDD },
        { 0x04, 0x09, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
          0x00, 0x00, 0x00, 0x31, 0x01, 0x02, 0x03, 0x04,
          0x00, 0x00, 0x00, 0x03, 0x55, 0x66, 0x77, 0x88,
          0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x00 }
    };
    static const uint8_t bad_same_cluster_ready[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES] = {
        0x04, 0x1B, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04,
        0x00, 0x00, 0x00, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
        0x00, 0x00, 0x00, 0x31, 0x01, 0x02, 0x03, 0x04,
        0x00, 0x00, 0x00, 0x03, 0x55, 0x66, 0x77, 0x88,
        0x00, 0x00, 0x00, 0x0F, 0xAA, 0xBB, 0xCC, 0xDD
    };
    static const uint8_t bad_cross_cluster_ready[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES] = {
        0x04, 0x1B, 0x06, 0x00, 0x01, 0x02, 0x03, 0x04,
        0x00, 0x00, 0x00, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
        0x00, 0x00, 0x00, 0x31, 0x11, 0x22, 0x33, 0x44,
        0x00, 0x00, 0x00, 0x03, 0x55, 0x66, 0x77, 0x88,
        0x00, 0x00, 0x00, 0x0F, 0xAA, 0xBB, 0xCC, 0xDD
    };
    static const uint8_t v3_advertise[UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES] = {
        0x03, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x64, 0x00, 0x20, 0x00, 0x00, 0x03, 0xE8,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
    };
    size_t index;

    for (index = 0U; index < sizeof(v4_vectors) / sizeof(v4_vectors[0U]); ++index) {
        ucn_cluster_wire_v4_frame_t frame;
        ucn_cluster_wire_v4_frame_t semantic_rebuilt;
        ucn_cluster_wire_message_t dispatched;
        ucn_cluster_wire_v4_semantic_message_t semantic;
        uint8_t encoded[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];

        if (ucn_cluster_wire_v4_decode(v4_vectors[index], sizeof(v4_vectors[index]),
                                       &frame) != UCN_OK) {
            printf("invalid frozen v4 vector: %lu\n", (unsigned long)index);
            return 1;
        }
        /* The test-only encoder must reproduce the frozen RFC4 bytes exactly,
         * rather than merely round-trip through its own decoder. */
        ASSERT_TRUE(ucn_cluster_wire_v4_encode(&frame, encoded) == UCN_OK);
        ASSERT_TRUE(memcmp(encoded, v4_vectors[index], sizeof(encoded)) == 0);
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_from_frame(&frame, &semantic) ==
                    UCN_OK);
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_to_frame(&semantic,
                                                           &semantic_rebuilt) == UCN_OK);
        ASSERT_TRUE(memcmp(&semantic_rebuilt, &frame, sizeof(frame)) == 0);
        ASSERT_TRUE(ucn_cluster_wire_decode(v4_vectors[index],
                                            sizeof(v4_vectors[index]),
                                            &dispatched) == UCN_OK);
        ASSERT_TRUE(dispatched.format == UCN_CLUSTER_WIRE_FORMAT_V4);
    }
    {
        ucn_cluster_wire_v4_frame_t frame;
        ucn_cluster_wire_message_t dispatched;
        uint8_t malformed[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];
        uint8_t expected_frame[sizeof(frame)];

        ASSERT_TRUE(ucn_cluster_wire_v4_decode(bad_same_cluster_ready,
                                                sizeof(bad_same_cluster_ready),
                                                &frame) == UCN_ERR_MALFORMED);
        ASSERT_TRUE(ucn_cluster_wire_v4_decode(bad_cross_cluster_ready,
                                                sizeof(bad_cross_cluster_ready),
                                                &frame) == UCN_ERR_MALFORMED);
        ASSERT_TRUE(ucn_cluster_wire_decode(v3_advertise, sizeof(v3_advertise),
                                            &dispatched) == UCN_OK);
        ASSERT_TRUE(dispatched.format == UCN_CLUSTER_WIRE_FORMAT_V3);
        ASSERT_TRUE(ucn_cluster_wire_detect_format(v4_vectors[0U],
                                                   UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES,
                                                   &dispatched.format) == UCN_ERR_MALFORMED);
        ASSERT_TRUE(ucn_cluster_wire_detect_format(v3_advertise,
                                                   UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES,
                                                   &dispatched.format) == UCN_ERR_MALFORMED);
        (void)memcpy(malformed, v4_vectors[0U], sizeof(malformed));
        malformed[1U] = 34U;
        ASSERT_TRUE(ucn_cluster_wire_v4_decode(malformed, sizeof(malformed), &frame) ==
                    UCN_ERR_MALFORMED);
        (void)memset(&frame, 0xA5, sizeof(frame));
        (void)memcpy(expected_frame, &frame, sizeof(frame));
        (void)memcpy(malformed, v4_vectors[0U], sizeof(malformed));
        malformed[3U] = 1U;
        ASSERT_TRUE(ucn_cluster_wire_v4_decode(malformed, sizeof(malformed), &frame) ==
                    UCN_ERR_MALFORMED);
        ASSERT_TRUE(memcmp(&frame, expected_frame, sizeof(frame)) == 0);
        (void)memcpy(malformed, v4_vectors[0U], sizeof(malformed));
        malformed[2U] = (uint8_t)UCN_CLUSTER_ROLE_DISABLED;
        ASSERT_TRUE(ucn_cluster_wire_v4_decode(malformed, sizeof(malformed), &frame) ==
                    UCN_ERR_MALFORMED);
        ASSERT_TRUE(memcmp(&frame, expected_frame, sizeof(frame)) == 0);
        (void)memcpy(malformed, v4_vectors[0U], sizeof(malformed));
        malformed[32U] = 1U;
        ASSERT_TRUE(ucn_cluster_wire_v4_decode(malformed, sizeof(malformed), &frame) ==
                    UCN_ERR_MALFORMED);
        ASSERT_TRUE(memcmp(&frame, expected_frame, sizeof(frame)) == 0);
    }
    return 0;
}

static int test_semantic_builders(void)
{
    uint8_t type;

    for (type = UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE;
         type <= UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE; ++type) {
        ucn_cluster_wire_v4_frame_t frame;
        ucn_cluster_wire_v4_frame_t rebuilt;
        ucn_cluster_wire_v4_frame_t untouched_frame;
        ucn_cluster_wire_v4_semantic_message_t semantic;
        ucn_cluster_wire_v4_semantic_message_t poisoned;
        size_t active_payload_bytes;

        ASSERT_TRUE(make_valid_frame(type, &frame));
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_from_frame(&frame, &semantic) ==
                    UCN_OK);
        ASSERT_TRUE(semantic.header.type == frame.type &&
                    semantic.header.role == frame.role &&
                    semantic.header.flags == frame.flags &&
                    semantic.header.cluster_id == frame.cluster_id &&
                    semantic.header.term == frame.term &&
                    semantic.header.head_node_id == frame.head_node_id);
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_to_frame(&semantic, &rebuilt) ==
                    UCN_OK);
        ASSERT_TRUE(memcmp(&rebuilt, &frame, sizeof(frame)) == 0);

        /* The builder must not read union storage after this Type's explicit
         * payload.  Poisoning it models a caller that did not initialize
         * fields belonging to other RFC4 Types. */
        active_payload_bytes = ucn_cluster_wire_v4_semantic_payload_size(type);
        ASSERT_TRUE(active_payload_bytes != 0U &&
                    active_payload_bytes <= sizeof(poisoned.payload));
        poisoned = semantic;
        if (active_payload_bytes < sizeof(poisoned.payload)) {
            (void)memset((uint8_t *)&poisoned.payload + active_payload_bytes,
                         0xA5, sizeof(poisoned.payload) - active_payload_bytes);
        }
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_to_frame(&poisoned, &rebuilt) ==
                    UCN_OK);
        ASSERT_TRUE(memcmp(&rebuilt, &frame, sizeof(frame)) == 0);

        /* A bad semantic header/payload must not partially overwrite caller
         * output; the final raw structural gate is the single validator. */
        poisoned = semantic;
        poisoned.header.term = 0U;
        (void)memset(&untouched_frame, 0x5A, sizeof(untouched_frame));
        rebuilt = untouched_frame;
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_to_frame(&poisoned, &rebuilt) ==
                    UCN_ERR_ARGUMENT);
        ASSERT_TRUE(memcmp(&rebuilt, &untouched_frame, sizeof(rebuilt)) == 0);
    }

    /* Type 12 marker form proves that an active Type-specific payload writes
     * its mandated zero tail, rather than inheriting any generic words. */
    {
        ucn_cluster_wire_v4_frame_t marker;
        ucn_cluster_wire_v4_frame_t rebuilt;
        ucn_cluster_wire_v4_semantic_message_t semantic;

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC,
                                     &marker));
        marker.flags = UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN;
        marker.words[3U] = 0U;
        marker.words[4U] = 0U;
        marker.words[5U] = 0U;
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_from_frame(&marker, &semantic) ==
                    UCN_OK);
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_to_frame(&semantic, &rebuilt) ==
                    UCN_OK);
        ASSERT_TRUE(memcmp(&rebuilt, &marker, sizeof(marker)) == 0);
    }
    {
        ucn_cluster_wire_v4_frame_t malformed;
        ucn_cluster_wire_v4_semantic_message_t semantic;
        ucn_cluster_wire_v4_semantic_message_t expected;

        ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_LEAVE, &malformed));
        malformed.term = 0U;
        (void)memset(&semantic, 0x5A, sizeof(semantic));
        expected = semantic;
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_from_frame(&malformed, &semantic) ==
                    UCN_ERR_MALFORMED);
        ASSERT_TRUE(memcmp(&semantic, &expected, sizeof(semantic)) == 0);
        ASSERT_TRUE(ucn_cluster_wire_v4_semantic_payload_size(0U) == 0U &&
                    ucn_cluster_wire_v4_semantic_payload_size(34U) == 0U);
    }
    return 0;
}

static int test_snapshot_semantic_binding(void)
{
    ucn_cluster_wire_v4_snapshot_t snapshot;
    ucn_cluster_wire_v4_snapshot_t decoded;
    ucn_cluster_wire_v4_snapshot_t untouched_snapshot;
    ucn_cluster_wire_v4_frame_t frame;
    ucn_cluster_wire_v4_frame_t untouched_frame;

    (void)memset(&snapshot, 0, sizeof(snapshot));
    snapshot.cluster_id = UINT32_C(0xFFFFFFFE);
    snapshot.term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    snapshot.head_node_id = UINT32_C(0xFFFFFFFE);
    snapshot.backup_generation = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    snapshot.snapshot_id = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    snapshot.membership_sequence = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    snapshot.kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_MEMBER;
    snapshot.member_node_id = UINT32_C(0xFFFFFFFE);
    snapshot.member_nonce = UINT32_C(0xFFFFFFFE);
    snapshot.member_lease_ms = UINT32_C(0x7FFFFFFF);
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) == UCN_OK);
    ASSERT_TRUE(frame.type == UCN_CLUSTER_WIRE_V4_MSG_BACKUP_MEMBER_SYNC &&
                frame.role == UCN_CLUSTER_ROLE_HEAD && frame.flags == 0U &&
                frame.cluster_id == snapshot.cluster_id && frame.term == snapshot.term &&
                frame.head_node_id == snapshot.head_node_id &&
                frame.words[0U] == snapshot.backup_generation &&
                frame.words[1U] == snapshot.snapshot_id &&
                frame.words[2U] == snapshot.membership_sequence &&
                frame.words[3U] == snapshot.member_node_id &&
                frame.words[4U] == snapshot.member_nonce &&
                frame.words[5U] == snapshot.member_lease_ms);
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_from_frame(&frame, &decoded) == UCN_OK);
    ASSERT_TRUE(decoded.cluster_id == snapshot.cluster_id && decoded.term == snapshot.term &&
                decoded.head_node_id == snapshot.head_node_id &&
                decoded.backup_generation == snapshot.backup_generation &&
                decoded.snapshot_id == snapshot.snapshot_id &&
                decoded.membership_sequence == snapshot.membership_sequence &&
                decoded.kind == snapshot.kind &&
                decoded.member_node_id == snapshot.member_node_id &&
                decoded.member_nonce == snapshot.member_nonce &&
                decoded.member_lease_ms == snapshot.member_lease_ms);

    /* BEGIN and END are not partial member records: the structural gate
     * requires the marker tail to be zero and preserves full Epoch binding. */
    snapshot.kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_BEGIN;
    snapshot.member_node_id = 1U;
    snapshot.member_nonce = 1U;
    snapshot.member_lease_ms = 1U;
    (void)memset(&untouched_frame, 0x5A, sizeof(untouched_frame));
    frame = untouched_frame;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&frame, &untouched_frame, sizeof(frame)) == 0);
    snapshot.member_node_id = 0U;
    snapshot.member_nonce = 0U;
    snapshot.member_lease_ms = 0U;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) == UCN_OK);
    ASSERT_TRUE(frame.flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_BEGIN &&
                frame.words[3U] == 0U && frame.words[4U] == 0U &&
                frame.words[5U] == 0U);
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_from_frame(&frame, &decoded) == UCN_OK &&
                decoded.kind == UCN_CLUSTER_WIRE_V4_SNAPSHOT_BEGIN &&
                decoded.cluster_id == snapshot.cluster_id && decoded.term == snapshot.term &&
                decoded.head_node_id == snapshot.head_node_id);
    frame.words[3U] = 1U;
    (void)memset(&untouched_snapshot, 0x5A, sizeof(untouched_snapshot));
    decoded = untouched_snapshot;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_from_frame(&frame, &decoded) ==
                UCN_ERR_MALFORMED);
    ASSERT_TRUE(memcmp(&decoded, &untouched_snapshot, sizeof(decoded)) == 0);
    frame.words[3U] = 0U;

    snapshot.kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_END;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) == UCN_OK);
    ASSERT_TRUE(frame.flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_END);
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_from_frame(&frame, &decoded) == UCN_OK &&
                decoded.kind == UCN_CLUSTER_WIRE_V4_SNAPSHOT_END);

    snapshot.kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_DELTA;
    snapshot.member_node_id = UINT32_C(0xFFFFFFFE);
    snapshot.member_nonce = UINT32_C(0xFFFFFFFE);
    snapshot.member_lease_ms = UINT32_C(0x7FFFFFFF);
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) == UCN_OK);
    ASSERT_TRUE(frame.flags == UCN_CLUSTER_WIRE_V4_FLAG_SYNC_DELTA);

    /* The helper cannot form a Type 12 frame from a partial Epoch. */
    snapshot.cluster_id = 0U;
    (void)memset(&untouched_frame, 0x5A, sizeof(untouched_frame));
    frame = untouched_frame;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&frame, &untouched_frame, sizeof(frame)) == 0);
    snapshot.cluster_id = UINT32_C(0xFFFFFFFE);
    snapshot.term = 0U;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&frame, &untouched_frame, sizeof(frame)) == 0);
    snapshot.term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    snapshot.head_node_id = 0U;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&frame, &untouched_frame, sizeof(frame)) == 0);
    snapshot.head_node_id = UINT32_C(0xFFFFFFFE);

    /* Serial and duration fields retain their existing fail-closed domain:
     * UINT32_MAX-1 is a legal ID/nonce boundary, but not a serial/duration. */
    snapshot.snapshot_id = UINT32_C(0xFFFFFFFE);
    (void)memset(&untouched_frame, 0x5A, sizeof(untouched_frame));
    frame = untouched_frame;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&frame, &untouched_frame, sizeof(frame)) == 0);
    snapshot.snapshot_id = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    snapshot.member_lease_ms = UINT32_C(0xFFFFFFFE);
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&frame, &untouched_frame, sizeof(frame)) == 0);

    snapshot.member_lease_ms = UINT32_C(0x7FFFFFFF);
    snapshot.kind = UCN_CLUSTER_WIRE_V4_SNAPSHOT_INVALID;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_to_frame(&snapshot, &frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&frame, &untouched_frame, sizeof(frame)) == 0);

    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_KEEPALIVE, &frame));
    (void)memset(&untouched_snapshot, 0x5A, sizeof(untouched_snapshot));
    decoded = untouched_snapshot;
    ASSERT_TRUE(ucn_cluster_wire_v4_snapshot_from_frame(&frame, &decoded) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&decoded, &untouched_snapshot, sizeof(decoded)) == 0);
    return 0;
}

static int test_takeover_certificate_semantic_binding(void)
{
    ucn_cluster_wire_v4_takeover_t takeover;
    ucn_cluster_wire_v4_takeover_t decoded_takeover;
    ucn_cluster_wire_v4_takeover_t untouched_takeover;
    ucn_cluster_wire_v4_takeover_fragment_t fragment;
    ucn_cluster_wire_v4_takeover_fragment_t decoded_fragment;
    ucn_cluster_wire_v4_takeover_fragment_t untouched_fragment;
    ucn_cluster_wire_v4_takeover_fragment_t forged_fragment;
    ucn_cluster_wire_v4_frame_t takeover_frame;
    ucn_cluster_wire_v4_frame_t fragment_frame;
    ucn_cluster_wire_v4_frame_t untouched_frame;
    ucn_cluster_wire_v4_pending_t pending;
    ucn_cluster_wire_v4_certificate_admission_t admission;

    (void)memset(&takeover, 0, sizeof(takeover));
    takeover.cluster_id = UINT32_C(0xFFFFFFFE);
    takeover.proposed_term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    takeover.proposed_head_node_id = UINT32_C(0xFFFFFFFE);
    takeover.backup_generation = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    takeover.snapshot_id = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    takeover.certificate_anchor_config_id = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    takeover.takeover_txid = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    takeover.required_set_mask = 1U;
    takeover.certificate_crc32 = UINT32_C(0x12D221F9);
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_to_frame(&takeover, &takeover_frame) ==
                UCN_OK);
    ASSERT_TRUE(takeover_frame.type == UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER &&
                takeover_frame.role == UCN_CLUSTER_ROLE_HEAD &&
                takeover_frame.cluster_id == takeover.cluster_id &&
                takeover_frame.term == takeover.proposed_term &&
                takeover_frame.head_node_id == takeover.proposed_head_node_id &&
                takeover_frame.words[0U] == takeover.backup_generation &&
                takeover_frame.words[1U] == takeover.snapshot_id &&
                takeover_frame.words[2U] == takeover.certificate_anchor_config_id &&
                takeover_frame.words[3U] == takeover.takeover_txid &&
                takeover_frame.words[4U] == takeover.required_set_mask &&
                takeover_frame.words[5U] == takeover.certificate_crc32);
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_from_frame(&takeover_frame,
                                                         &decoded_takeover) == UCN_OK);
    ASSERT_TRUE(memcmp(&decoded_takeover, &takeover, sizeof(takeover)) == 0);

    (void)memset(&fragment, 0, sizeof(fragment));
    fragment.cluster_id = takeover.cluster_id;
    fragment.proposed_term = takeover.proposed_term;
    fragment.proposed_head_node_id = takeover.proposed_head_node_id;
    fragment.backup_generation = takeover.backup_generation;
    fragment.snapshot_id = takeover.snapshot_id;
    fragment.config_id = takeover.certificate_anchor_config_id;
    fragment.takeover_txid = takeover.takeover_txid;
    fragment.certificate_set = UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_OLD;
    fragment.fragment_index = 0U;
    fragment.fragment_count = 2U;
    fragment.vote_bitmap_word = UINT32_C(0x00000007);
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(&fragment,
                                                                 &fragment_frame) ==
                UCN_OK);
    ASSERT_TRUE(fragment_frame.type ==
                    UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE &&
                fragment_frame.role == UCN_CLUSTER_ROLE_HEAD &&
                fragment_frame.flags == UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD &&
                fragment_frame.cluster_id == fragment.cluster_id &&
                fragment_frame.term == fragment.proposed_term &&
                fragment_frame.head_node_id == fragment.proposed_head_node_id &&
                fragment_frame.words[0U] == fragment.backup_generation &&
                fragment_frame.words[1U] == fragment.snapshot_id &&
                fragment_frame.words[2U] == fragment.config_id &&
                fragment_frame.words[3U] == fragment.takeover_txid &&
                fragment_frame.words[4U] == 2U &&
                fragment_frame.words[5U] == fragment.vote_bitmap_word);
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_from_frame(
                    &fragment_frame, &decoded_fragment) == UCN_OK);
    ASSERT_TRUE(memcmp(&decoded_fragment, &fragment, sizeof(fragment)) == 0);

    (void)memset(&admission, 0, sizeof(admission));
    admission.outer_source = takeover.proposed_head_node_id;
    admission.old_config_id = takeover.certificate_anchor_config_id;
    admission.source_admitted = true;
    admission.frozen_config_admitted = true;
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &fragment, &admission));

    /* A two-word Stable voter set is incomplete after one word and becomes
     * complete only after the exact second fragment. A forged key/Config/set
     * cannot change the existing pending slot. */
    ucn_cluster_wire_v4_pending_reset(&pending);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(&pending, &takeover_frame,
                                                   &admission, 0U) == UCN_OK);
    forged_fragment = fragment;
    forged_fragment.snapshot_id--;
    ASSERT_TRUE(!ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &forged_fragment, &admission));
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(
                    &forged_fragment, &fragment_frame) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment_frame, &admission, 1U) ==
                UCN_ERR_MALFORMED);
    ASSERT_TRUE(pending.occupied && pending.received_fragment_masks[0U] == 0U);

    forged_fragment = fragment;
    forged_fragment.config_id--;
    ASSERT_TRUE(!ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &forged_fragment, &admission));
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(
                    &forged_fragment, &fragment_frame) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment_frame, &admission, 1U) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.received_fragment_masks[0U] == 0U);

    forged_fragment = fragment;
    forged_fragment.certificate_set = UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_NEW;
    ASSERT_TRUE(!ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &forged_fragment, &admission));
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(
                    &forged_fragment, &fragment_frame) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment_frame, &admission, 1U) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.received_fragment_masks[0U] == 0U);

    forged_fragment = fragment;
    forged_fragment.proposed_term--;
    ASSERT_TRUE(!ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &forged_fragment, &admission));
    forged_fragment = fragment;
    forged_fragment.backup_generation--;
    ASSERT_TRUE(!ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &forged_fragment, &admission));
    forged_fragment = fragment;
    forged_fragment.takeover_txid--;
    ASSERT_TRUE(!ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &forged_fragment, &admission));

    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(&fragment,
                                                                 &fragment_frame) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment_frame, &admission, 1U) == UCN_OK);
    ASSERT_TRUE(!ucn_cluster_wire_v4_pending_has_all_fragments(&pending));
    fragment.fragment_index = 1U;
    fragment.vote_bitmap_word = UINT32_C(0x00000001);
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(&fragment,
                                                                 &fragment_frame) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment_frame, &admission, 1U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_has_all_fragments(&pending));
    ucn_cluster_wire_v4_pending_reset(&pending);

    /* Joint certificates use C_new as Type 8's anchor while each fragment
     * retains its own admitted C_old/C_new identity. */
    takeover.certificate_anchor_config_id = 9U;
    takeover.required_set_mask = 3U;
    admission.old_config_id = 7U;
    admission.new_config_id = 9U;
    fragment.fragment_index = 0U;
    fragment.fragment_count = 1U;
    fragment.certificate_set = UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_OLD;
    fragment.config_id = 7U;
    fragment.vote_bitmap_word = UINT32_C(0x00000007);
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_to_frame(&takeover, &takeover_frame) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &fragment, &admission));
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(&fragment,
                                                                 &fragment_frame) ==
                UCN_OK);
    ucn_cluster_wire_v4_pending_reset(&pending);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(&pending, &takeover_frame,
                                                   &admission, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment_frame, &admission, 1U) == UCN_OK);
    ASSERT_TRUE(!ucn_cluster_wire_v4_pending_has_all_fragments(&pending));
    fragment.certificate_set = UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_NEW;
    fragment.config_id = 9U;
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_matches_admission(
        &takeover, &fragment, &admission));
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(&fragment,
                                                                 &fragment_frame) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment_frame, &admission, 1U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_has_all_fragments(&pending));

    /* Builder failures do not modify the caller output, including an
     * out-of-range descriptor or an invalid private set enum. */
    (void)memset(&untouched_frame, 0x5A, sizeof(untouched_frame));
    fragment_frame = untouched_frame;
    fragment.fragment_count = (uint16_t)(
        UCN_CLUSTER_WIRE_V4_MAX_CERTIFICATE_FRAGMENTS + 1U);
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(&fragment,
                                                                 &fragment_frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&fragment_frame, &untouched_frame, sizeof(fragment_frame)) ==
                0);
    fragment.fragment_count = 1U;
    fragment.certificate_set = UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_INVALID;
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_to_frame(&fragment,
                                                                 &fragment_frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&fragment_frame, &untouched_frame, sizeof(fragment_frame)) ==
                0);
    fragment.certificate_set = UCN_CLUSTER_WIRE_V4_CERTIFICATE_SET_NEW;
    takeover.proposed_term = 0U;
    takeover_frame = untouched_frame;
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_to_frame(&takeover, &takeover_frame) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&takeover_frame, &untouched_frame, sizeof(takeover_frame)) ==
                0);
    takeover.proposed_term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;

    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER,
                                 &takeover_frame));
    (void)memset(&untouched_fragment, 0x5A, sizeof(untouched_fragment));
    decoded_fragment = untouched_fragment;
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_fragment_from_frame(
                    &takeover_frame, &decoded_fragment) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&decoded_fragment, &untouched_fragment,
                       sizeof(decoded_fragment)) == 0);
    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE,
                                 &fragment_frame));
    (void)memset(&untouched_takeover, 0x5A, sizeof(untouched_takeover));
    decoded_takeover = untouched_takeover;
    ASSERT_TRUE(ucn_cluster_wire_v4_takeover_from_frame(&fragment_frame,
                                                         &decoded_takeover) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&decoded_takeover, &untouched_takeover,
                       sizeof(decoded_takeover)) == 0);
    return 0;
}

static int test_pending_cache(void)
{
    ucn_cluster_wire_v4_frame_t takeover;
    ucn_cluster_wire_v4_frame_t fragment;
    ucn_cluster_wire_v4_pending_t pending;
    ucn_cluster_wire_v4_certificate_admission_t admission;
    ucn_cluster_wire_v4_certificate_admission_t rejected_admission;
    uint32_t held_deadline;

    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_HEAD_TAKEOVER, &takeover));
    ASSERT_TRUE(make_valid_frame(UCN_CLUSTER_WIRE_V4_MSG_TAKEOVER_CERTIFICATE,
                                 &fragment));
    (void)memset(&admission, 0, sizeof(admission));
    admission.outer_source = takeover.head_node_id;
    admission.old_config_id = takeover.words[2U];
    admission.source_admitted = true;
    admission.frozen_config_admitted = true;
    ASSERT_TRUE(ucn_cluster_wire_v4_certificate_admission_is_valid(&admission));

    /* All public cache operations require an explicit initial reset. */
    ucn_cluster_wire_v4_pending_reset(&pending);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission, 0U) == UCN_ERR_NOT_FOUND);
    rejected_admission = admission;
    rejected_admission.outer_source++;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(
                    &pending, &takeover, &rejected_admission, 0U) == UCN_ERR_STATE);
    ASSERT_TRUE(!pending.occupied);
    rejected_admission = admission;
    rejected_admission.frozen_config_admitted = false;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(
                    &pending, &takeover, &rejected_admission, 0U) == UCN_ERR_STATE);
    ASSERT_TRUE(!pending.occupied);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(
                    &pending, &takeover, &admission, 0U) == UCN_OK);
    ASSERT_TRUE(pending.occupied);
    held_deadline = pending.deadline_ms;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(
                    &pending, &takeover, &admission, 1U) == UCN_OK);
    takeover.words[3U] = 2U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(
                    &pending, &takeover, &admission, 1U) == UCN_ERR_NO_SPACE);
    ASSERT_TRUE(pending.takeover.words[3U] == 1U);
    takeover.words[3U] = 1U;
    rejected_admission = admission;
    rejected_admission.outer_source++;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &rejected_admission, 1U) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.deadline_ms == held_deadline &&
                pending.received_fragment_masks[0U] == 0U);
    rejected_admission = admission;
    rejected_admission.frozen_config_admitted = false;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &rejected_admission, 1U) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.deadline_ms == held_deadline &&
                pending.received_fragment_masks[0U] == 0U);
    fragment.words[2U] = 2U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission, 1U) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.deadline_ms == held_deadline &&
                pending.received_fragment_masks[0U] == 0U);
    fragment.words[2U] = 1U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission, 1U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_has_all_fragments(&pending));
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission, 1U) == UCN_OK);
    fragment.words[5U] = 2U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission, 1U) == UCN_ERR_MALFORMED);
    ASSERT_TRUE(!pending.occupied);

    /* A Joint Certificate records both frozen identities.  Its OLD fragment
     * is not the Type 8 anchor (which is C_new), so each set is checked
     * against the separately admitted C_old/C_new context. */
    takeover.words[2U] = 9U;
    takeover.words[4U] = 3U;
    admission.old_config_id = 7U;
    admission.new_config_id = 9U;
    fragment.flags = UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD;
    fragment.words[2U] = 8U;
    fragment.words[5U] = 1U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(
                    &pending, &takeover, &admission, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission, 1U) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.received_fragment_masks[0U] == 0U);
    fragment.words[2U] = 7U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission, 1U) == UCN_OK);
    fragment.flags = UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_NEW;
    fragment.words[2U] = 9U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission, 1U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_has_all_fragments(&pending));
    ucn_cluster_wire_v4_pending_reset(&pending);

    takeover.words[2U] = 1U;
    takeover.words[4U] = 1U;
    admission.old_config_id = 1U;
    admission.new_config_id = 0U;
    fragment.flags = UCN_CLUSTER_WIRE_V4_FLAG_CERT_SET_OLD;
    fragment.words[2U] = 1U;
    fragment.words[5U] = 1U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(
                    &pending, &takeover, &admission, 0U) == UCN_OK);
    held_deadline = pending.deadline_ms;
    /* At the exact deadline, an unadmitted frame cannot use the lazy-expiry
     * path to evict the slot. Only the explicit timer-owner API, or a fully
     * admitted matching fragment, may reach expiry. */
    rejected_admission = admission;
    rejected_admission.outer_source++;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &rejected_admission,
                    held_deadline) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.deadline_ms == held_deadline &&
                pending.received_fragment_masks[0U] == 0U);
    rejected_admission = admission;
    rejected_admission.frozen_config_admitted = false;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &rejected_admission,
                    held_deadline) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.deadline_ms == held_deadline &&
                pending.received_fragment_masks[0U] == 0U);
    fragment.words[2U] = 2U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_accept_fragment(
                    &pending, &fragment, &admission,
                    held_deadline) == UCN_ERR_STATE);
    ASSERT_TRUE(pending.occupied && pending.deadline_ms == held_deadline &&
                pending.received_fragment_masks[0U] == 0U);
    fragment.words[2U] = 1U;
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_expire(
                    &pending, held_deadline));
    ASSERT_TRUE(!pending.occupied);
    ASSERT_TRUE(ucn_cluster_wire_v4_pending_begin(
                    &pending, &takeover, &admission, 0U) == UCN_OK);
    ucn_cluster_wire_v4_pending_on_active_epoch_change(&pending);
    ASSERT_TRUE(!pending.occupied);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_extended_type_registry_contract();
    result |= test_wire_offer_capability_semantics();
    result |= test_mixed_version_policy();
    result |= test_versioned_diagnostics();
    result |= test_each_type_round_trip();
    result |= test_all_type_negative_matrix();
    result |= test_rfc4_full_field_contract();
    result |= test_fixed_seed_v4_decode_fuzz();
    result |= test_frozen_vectors_and_strict_dispatch();
    result |= test_semantic_builders();
    result |= test_snapshot_semantic_binding();
    result |= test_takeover_certificate_semantic_binding();
    result |= test_pending_cache();
    if (result == 0) {
        printf("Cluster Wire v4 codec tests passed.\n");
    }
    return result;
}
