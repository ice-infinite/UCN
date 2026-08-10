#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"

static void init_wire_frame(ucn_frame_t *frame, ucn_wire_profile_t profile,
                            const uint8_t *payload)
{
    (void)memset(frame, 0, sizeof(*frame));
    frame->message_type = UCN_MSG_DATA_Q1;
    frame->wire_profile = profile;
    frame->traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame->hop_limit = 4U;
    frame->network_id = UINT32_C(0x11);
    frame->source = UINT32_C(0x22);
    frame->destination = UINT32_C(0x33);
    frame->sequence = UINT32_C(0x44556677);
    frame->session_id = UINT32_C(0x44);
    frame->payload = payload;
    frame->payload_length = 1U;
}

static int test_profile_descriptors(void)
{
    static const ucn_wire_profile_t profiles[] = {
        UCN_WIRE_PROFILE_W0_LOCAL,
        UCN_WIRE_PROFILE_W1_EDGE,
        UCN_WIRE_PROFILE_W2_MESH,
        UCN_WIRE_PROFILE_W3_BACKBONE
    };
    static const uint8_t address_bytes[] = { 1U, 2U, 3U, 4U };
    static const uint8_t length_bytes[] = { 1U, 1U, 2U, 2U };
    static const uint8_t max_hops[] = { 4U, 16U, 64U, 254U };
    static const size_t base_sizes[] = { 17U, 21U, 26U, 30U };
    static const size_t route_sizes[] = { 18U, 23U, 28U, 32U };
    static const size_t path_sizes[] = { 19U, 25U, 31U, 36U };
    size_t index;

    for (index = 0U; index < 4U; ++index) {
        const ucn_wire_profile_descriptor_t *descriptor =
            ucn_wire_profile_get_descriptor(profiles[index]);

        TEST_ASSERT(descriptor != NULL);
        TEST_ASSERT(descriptor->wire_code == index);
        TEST_ASSERT(descriptor->address_bytes == address_bytes[index]);
        TEST_ASSERT(descriptor->payload_length_bytes == length_bytes[index]);
        TEST_ASSERT(descriptor->max_hops == max_hops[index]);
        TEST_ASSERT(descriptor->max_node_id + UINT32_C(1) ==
                    descriptor->max_wire_value);
        TEST_ASSERT(ucn_frame_header_size_for_profile(profiles[index], 0U) ==
                    base_sizes[index]);
        TEST_ASSERT(ucn_frame_header_size_for_profile(
                        profiles[index], UCN_FRAME_FLAG_ROUTE_EXTENSION) ==
                    route_sizes[index]);
        TEST_ASSERT(ucn_frame_header_size_for_profile(
                        profiles[index], UCN_FRAME_FLAG_ROUTE_EXTENSION |
                                             UCN_FRAME_FLAG_PATH_ID) ==
                    path_sizes[index]);
        TEST_ASSERT(ucn_frame_max_payload_for_profile(profiles[index], 0U) ==
                    UCN_MAX_PAYLOAD_BYTES);
    }
    TEST_ASSERT(ucn_wire_profile_get_descriptor(
                    UCN_WIRE_PROFILE_UNSPECIFIED) == NULL);
    TEST_ASSERT(ucn_wire_profile_get_descriptor(UINT8_C(99)) == NULL);
    TEST_ASSERT(ucn_frame_header_size_for_profile(
                    UINT8_C(99), 0U) == 0U);
    TEST_ASSERT(ucn_frame_header_size_for_profile(
                    UCN_WIRE_PROFILE_W0_LOCAL,
                    UCN_FRAME_FLAG_PATH_ID) == 0U);
    TEST_ASSERT(ucn_frame_header_size_for_profile(
                    UCN_WIRE_PROFILE_UNSPECIFIED, 0U) ==
                UCN_FRAME_W3_HEADER_SIZE);
    return 0;
}

static int test_profile_golden_vectors(void)
{
    static const uint8_t payload = 0xAAU;
    static const uint8_t w0[] = {
        0x55U, 0x43U, 0x05U, 0x21U, 0x40U, 0x04U, 0x11U, 0x22U,
        0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x44U, 0x01U, 0x5CU,
        0x99U, 0xAAU
    };
    static const uint8_t w1[] = {
        0x55U, 0x43U, 0x45U, 0x21U, 0x40U, 0x04U, 0x00U, 0x11U,
        0x00U, 0x22U, 0x00U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x00U, 0x44U, 0x01U, 0x14U, 0x7CU, 0xAAU
    };
    static const uint8_t w2[] = {
        0x55U, 0x43U, 0x85U, 0x21U, 0x40U, 0x04U, 0x00U, 0x00U,
        0x11U, 0x00U, 0x00U, 0x22U, 0x00U, 0x00U, 0x33U, 0x44U,
        0x55U, 0x66U, 0x77U, 0x00U, 0x00U, 0x44U, 0x00U, 0x01U,
        0x14U, 0x5DU, 0xAAU
    };
    static const uint8_t w3[] = {
        0x55U, 0x43U, 0xC5U, 0x21U, 0x40U, 0x04U, 0x00U, 0x00U,
        0x00U, 0x11U, 0x00U, 0x00U, 0x00U, 0x22U, 0x00U, 0x00U,
        0x00U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x00U, 0x00U,
        0x00U, 0x44U, 0x00U, 0x01U, 0xE9U, 0xBFU, 0xAAU
    };
    static const ucn_wire_profile_t profiles[] = {
        UCN_WIRE_PROFILE_W0_LOCAL,
        UCN_WIRE_PROFILE_W1_EDGE,
        UCN_WIRE_PROFILE_W2_MESH,
        UCN_WIRE_PROFILE_W3_BACKBONE
    };
    static const uint8_t *const vectors[] = { w0, w1, w2, w3 };
    static const size_t lengths[] = {
        sizeof(w0), sizeof(w1), sizeof(w2), sizeof(w3)
    };
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    ucn_frame_t source;
    ucn_frame_t decoded;
    size_t index;

    for (index = 0U; index < 4U; ++index) {
        size_t encoded_length = 0U;

        init_wire_frame(&source, profiles[index], &payload);
        TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded),
                                     &encoded_length) == UCN_OK);
        TEST_ASSERT(encoded_length == lengths[index]);
        TEST_ASSERT(memcmp(encoded, vectors[index], lengths[index]) == 0);
        TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
        TEST_ASSERT(decoded.wire_profile == profiles[index]);
        TEST_ASSERT(decoded.network_id == source.network_id);
        TEST_ASSERT(decoded.source == source.source);
        TEST_ASSERT(decoded.destination == source.destination);
        TEST_ASSERT(decoded.sequence == source.sequence);
        TEST_ASSERT(decoded.session_id == source.session_id);
        TEST_ASSERT(decoded.payload_length == 1U && decoded.payload[0] == payload);
    }

    init_wire_frame(&source, UCN_WIRE_PROFILE_UNSPECIFIED, &payload);
    TEST_ASSERT(ucn_frame_encode(&source, encoded, sizeof(encoded),
                                 &index) == UCN_OK);
    TEST_ASSERT(index == sizeof(w3) && memcmp(encoded, w3, sizeof(w3)) == 0);
    return 0;
}

static int test_profile_boundaries_and_rejection(void)
{
    static const uint8_t payload = 0x5AU;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    uint8_t aad_w0[32];
    uint8_t aad_w1[32];
    ucn_frame_t frame;
    ucn_frame_t decoded;
    size_t encoded_length = 0U;
    size_t aad_w0_length = 0U;
    size_t aad_w1_length = 0U;

    init_wire_frame(&frame, UCN_WIRE_PROFILE_W0_LOCAL, &payload);
    frame.network_id = UINT32_C(0xFF);
    frame.source = UINT32_C(0xFE);
    frame.destination = UCN_NODE_BROADCAST;
    frame.session_id = UINT32_C(0xFF);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
    TEST_ASSERT(decoded.destination == UCN_NODE_BROADCAST);

    frame.source = UINT32_C(0xFF);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_ERR_TOO_LARGE);
    frame.source = UINT32_C(0xFE);
    frame.destination = UINT32_C(0xFF);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_ERR_TOO_LARGE);
    frame.destination = UCN_NODE_BROADCAST;
    frame.network_id = UINT32_C(0x100);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_ERR_TOO_LARGE);
    frame.network_id = UINT32_C(0xFF);
    frame.session_id = UINT32_C(0x100);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_ERR_TOO_LARGE);
    frame.session_id = UINT32_C(0xFF);
    frame.hop_limit = 5U;
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_ERR_TOO_LARGE);

    frame.hop_limit = 4U;
    frame.flags = UCN_FRAME_FLAG_ROUTE_EXTENSION |
                  UCN_FRAME_FLAG_PATH_ID;
    frame.has_route_extension = true;
    frame.route_epoch = UINT16_C(0xFF);
    frame.has_path_id = true;
    frame.path_id = UINT32_C(0xFF);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    frame.route_epoch = UINT16_C(0x100);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_ERR_TOO_LARGE);
    frame.route_epoch = UINT16_C(0xFF);
    frame.path_id = UINT32_C(0x100);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_ERR_TOO_LARGE);

    init_wire_frame(&frame, UCN_WIRE_PROFILE_W0_LOCAL, &payload);
    TEST_ASSERT(ucn_frame_write_e2e_aad(&frame, aad_w0, sizeof(aad_w0),
                                        &aad_w0_length) == UCN_OK);
    frame.wire_profile = UCN_WIRE_PROFILE_W1_EDGE;
    TEST_ASSERT(ucn_frame_write_e2e_aad(&frame, aad_w1, sizeof(aad_w1),
                                        &aad_w1_length) == UCN_OK);
    TEST_ASSERT(aad_w0_length == aad_w1_length &&
                memcmp(aad_w0, aad_w1, aad_w0_length) != 0);

    frame.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    encoded[2] = 4U;
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) ==
                UCN_ERR_VERSION);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    encoded[4] = (uint8_t)(encoded[4] | UINT8_C(0x20));
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) ==
                UCN_ERR_MALFORMED);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    encoded[UCN_FRAME_W0_HEADER_SIZE] ^= UINT8_C(1);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) ==
                UCN_ERR_CRC);
    return 0;
}

static int test_profile_extensions(void)
{
    static const uint8_t payload = 0xC3U;
    static const ucn_wire_profile_t profiles[] = {
        UCN_WIRE_PROFILE_W0_LOCAL,
        UCN_WIRE_PROFILE_W1_EDGE,
        UCN_WIRE_PROFILE_W2_MESH,
        UCN_WIRE_PROFILE_W3_BACKBONE
    };
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    ucn_frame_t frame;
    ucn_frame_t decoded;
    size_t index;

    for (index = 0U; index < 4U; ++index) {
        const ucn_wire_profile_descriptor_t *descriptor =
            ucn_wire_profile_get_descriptor(profiles[index]);
        const uint16_t route_epoch = descriptor->route_epoch_bytes == 1U ?
                                         UINT16_C(0xFF) : UINT16_C(0xFFFF);
        const uint32_t path_id = descriptor->max_wire_value;
        size_t encoded_length = 0U;

        init_wire_frame(&frame, profiles[index], &payload);
        frame.hop_limit = descriptor->max_hops;
        frame.flags = UCN_FRAME_FLAG_ROUTE_EXTENSION |
                      UCN_FRAME_FLAG_PATH_ID;
        frame.has_route_extension = true;
        frame.route_epoch = route_epoch;
        frame.has_path_id = true;
        frame.path_id = path_id;
        TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                     &encoded_length) == UCN_OK);
        TEST_ASSERT(encoded_length ==
                    ucn_frame_header_size_for_profile(profiles[index],
                                                      frame.flags) + 1U);
        TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
        TEST_ASSERT(decoded.wire_profile == profiles[index]);
        TEST_ASSERT(decoded.route_epoch == route_epoch);
        TEST_ASSERT(decoded.path_id == path_id);
        TEST_ASSERT(decoded.hop_limit == descriptor->max_hops);
    }

    init_wire_frame(&frame, UCN_WIRE_PROFILE_W3_BACKBONE, &payload);
    frame.hop_limit = UINT8_C(255);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded), &index) ==
                UCN_ERR_TOO_LARGE);
    return 0;
}

int test_wire_profile(void)
{
    int result = 0;

    result |= test_profile_descriptors();
    result |= test_profile_golden_vectors();
    result |= test_profile_boundaries_and_rejection();
    result |= test_profile_extensions();
    return result;
}
