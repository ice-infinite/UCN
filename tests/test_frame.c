#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"

int test_frame(void)
{
    static const uint8_t payload[] = { 0x10U, 0x20U, 0x30U, 0x40U };
    ucn_frame_t source;
    ucn_frame_t decoded;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;

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

    source.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
    TEST_ASSERT((decoded.flags & UCN_FRAME_FLAG_DIAGNOSTIC) != 0U);
    source.flags = 0U;
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    encoded[5] = UINT8_C(0x80);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_MALFORMED);
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    encoded[UCN_FRAME_HEADER_SIZE] ^= 0x01U;
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_CRC);
    encoded[UCN_FRAME_HEADER_SIZE] ^= 0x01U;
    encoded[2] = (uint8_t)(UCN_PROTOCOL_VERSION + 1U);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_VERSION);
    encoded[2] = UCN_PROTOCOL_VERSION;
    TEST_ASSERT(ucn_frame_decode(encoded, UCN_FRAME_HEADER_SIZE - 1U, &decoded) == UCN_ERR_MALFORMED);

    source.payload = NULL;
    source.payload_length = 1U;
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded), &encoded_length) == UCN_ERR_ARGUMENT);
    return 0;
}
