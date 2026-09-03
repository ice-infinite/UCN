#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"

static size_t expected_max_payload(uint8_t flags)
{
    const size_t header_size = ucn_frame_header_size_for_profile(
        UCN_WIRE_PROFILE_W3_BACKBONE, flags);
    const size_t tag_size = (flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U ?
                                UCN_E2E_TAG_SIZE : 0U;
    const size_t frame_limited = header_size + tag_size > UCN_MAX_FRAME_BYTES ?
                                     0U : UCN_MAX_FRAME_BYTES - header_size - tag_size;

    return frame_limited < UCN_MAX_PAYLOAD_BYTES ? frame_limited :
                                                   UCN_MAX_PAYLOAD_BYTES;
}

int test_frame(void)
{
    static const uint8_t payload[] = { 0x10U, 0x20U, 0x30U, 0x40U };
    ucn_frame_t source;
    ucn_frame_t decoded;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    uint8_t rejected_before[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_wire_profile_t peeked_profile = UCN_WIRE_PROFILE_UNSPECIFIED;

    (void)memset(&source, 0, sizeof(source));
    source.message_type = UCN_MSG_DATA_Q1;
    source.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    source.hop_limit = 3U;
    source.network_id = UINT32_C(0xAABBCCDD);
    source.source = UINT32_C(0x00000001);
    source.destination = UINT32_C(0x00000003);
    source.sequence = UINT32_C(17);
    source.session_id = UINT32_C(99);
    source.payload = payload;
    source.payload_length = (uint16_t)sizeof(payload);

    TEST_ASSERT(ucn_frame_encoded_size(&source) == UCN_FRAME_HEADER_SIZE + sizeof(payload));
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_frame_peek_wire_profile(encoded, encoded_length,
                                             &peeked_profile) == UCN_OK);
    TEST_ASSERT(peeked_profile == UCN_WIRE_PROFILE_W3_BACKBONE);
    TEST_ASSERT(ucn_frame_peek_wire_profile(encoded, 2U, &peeked_profile) ==
                UCN_ERR_MALFORMED);
    TEST_ASSERT(ucn_frame_peek_wire_profile(NULL, encoded_length,
                                             &peeked_profile) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(encoded_length == UCN_FRAME_HEADER_SIZE + sizeof(payload));
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
    TEST_ASSERT(decoded.message_type == source.message_type);
    TEST_ASSERT(decoded.traffic_class == source.traffic_class);
    TEST_ASSERT(decoded.hop_limit == source.hop_limit);
    TEST_ASSERT(decoded.network_id == source.network_id);
    TEST_ASSERT(decoded.source == source.source);
    TEST_ASSERT(decoded.destination == source.destination);
    TEST_ASSERT(decoded.sequence == source.sequence);
    TEST_ASSERT(decoded.session_id == source.session_id);
    TEST_ASSERT(decoded.payload_length == source.payload_length);
    TEST_ASSERT(memcmp(decoded.payload, payload, sizeof(payload)) == 0);

    /* A negative enum value must fail identically on compilers that choose
     * different enum comparison/packing behaviour.  Rejection happens before
     * either output buffer or output length is changed. */
    (void)memset(encoded, 0xA5, sizeof(encoded));
    (void)memcpy(rejected_before, encoded, sizeof(encoded));
    encoded_length = SIZE_MAX;
    source.traffic_class = (ucn_traffic_class_t)-1;
    TEST_ASSERT(ucn_frame_encoded_size(&source) == 0U);
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(encoded_length == SIZE_MAX);
    TEST_ASSERT(memcmp(encoded, rejected_before, sizeof(encoded)) == 0);
    source.traffic_class = UCN_TRAFFIC_Q1_REALTIME;

    source.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
    TEST_ASSERT((decoded.flags & UCN_FRAME_FLAG_DIAGNOSTIC) != 0U);
    source.flags = 0U;
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    encoded[4] = (uint8_t)(encoded[4] | UINT8_C(0x20));
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_MALFORMED);
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    encoded[UCN_FRAME_HEADER_SIZE] ^= 0x01U;
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_CRC);
    encoded[UCN_FRAME_HEADER_SIZE] ^= 0x01U;
    encoded[2] = (uint8_t)((UINT8_C(3) << 6U) |
                           (uint8_t)(UCN_PROTOCOL_VERSION + 1U));
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_VERSION);
    encoded[2] = (uint8_t)((UINT8_C(3) << 6U) | UCN_PROTOCOL_VERSION);
    TEST_ASSERT(ucn_frame_decode(encoded, UCN_FRAME_HEADER_SIZE - 1U, &decoded) == UCN_ERR_MALFORMED);

    source.payload = NULL;
    source.payload_length = 1U;
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_ERR_ARGUMENT);

    TEST_ASSERT(ucn_frame_max_payload(0U) == expected_max_payload(0U));
    TEST_ASSERT(ucn_frame_max_payload(UCN_FRAME_FLAG_ROUTE_EXTENSION) ==
                expected_max_payload(UCN_FRAME_FLAG_ROUTE_EXTENSION));
    TEST_ASSERT(ucn_frame_max_payload(UCN_FRAME_FLAG_PATH_ID) == 0U);
    TEST_ASSERT(ucn_frame_max_payload(UCN_FRAME_FLAG_ROUTE_EXTENSION |
                                      UCN_FRAME_FLAG_PATH_ID) ==
                expected_max_payload(UCN_FRAME_FLAG_ROUTE_EXTENSION |
                                     UCN_FRAME_FLAG_PATH_ID));
    TEST_ASSERT(ucn_frame_max_payload(UCN_FRAME_FLAG_E2E_PROTECTED) ==
                expected_max_payload(UCN_FRAME_FLAG_E2E_PROTECTED));
    TEST_ASSERT(ucn_frame_max_payload(UCN_FRAME_FLAG_ROUTE_EXTENSION |
                                      UCN_FRAME_FLAG_PATH_ID |
                                      UCN_FRAME_FLAG_E2E_PROTECTED) ==
                expected_max_payload(UCN_FRAME_FLAG_ROUTE_EXTENSION |
                                     UCN_FRAME_FLAG_PATH_ID |
                                     UCN_FRAME_FLAG_E2E_PROTECTED));
    TEST_ASSERT(ucn_frame_max_payload(UINT8_C(0x80)) == 0U);
    return 0;
}
