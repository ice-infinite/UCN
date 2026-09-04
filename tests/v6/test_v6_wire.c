#include <stdio.h>
#include <string.h>

#include "ucn/v6/ucn_v6_wire.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const uint8_t golden_a0[] = {
    0x55U, 0x43U, 0x06U, 0x01U, 0x08U, 0x00U, 0x01U, 0x01U,
    0x11U, 0x22U, 0x33U, 0x44U, 0x00U, 0xFFU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x01U, 0x02U, 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x93U, 0xC9U,
    0xD2U, 0xDBU
};
static const uint8_t golden_a1[] = {
    0x55U, 0x43U, 0x46U, 0x01U, 0x08U, 0x00U, 0x01U, 0x01U,
    0x11U, 0x22U, 0x33U, 0x44U, 0x00U, 0x00U, 0xFFU, 0xFFU,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x04U, 0x01U, 0x02U, 0xDEU, 0xADU, 0xBEU, 0xEFU,
    0x78U, 0xD6U, 0x8DU, 0x5CU
};
static const uint8_t golden_a2[] = {
    0x55U, 0x43U, 0x86U, 0x01U, 0x08U, 0x00U, 0x01U, 0x01U,
    0x11U, 0x22U, 0x33U, 0x44U, 0x00U, 0x00U, 0x00U, 0xFFU,
    0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x04U, 0x01U, 0x02U, 0xDEU, 0xADU,
    0xBEU, 0xEFU, 0xABU, 0x41U, 0x95U, 0x69U
};
static const uint8_t golden_a3[] = {
    0x55U, 0x43U, 0xC6U, 0x01U, 0x08U, 0x00U, 0x01U, 0x01U,
    0x11U, 0x22U, 0x33U, 0x44U, 0x00U, 0x00U, 0x00U, 0x00U,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x01U, 0x02U,
    0xDEU, 0xADU, 0xBEU, 0xEFU, 0x03U, 0x94U, 0x15U, 0xBEU
};

static void fill_tag(uint8_t tag[UCN_V6_SECURITY_TAG_BYTES], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < UCN_V6_SECURITY_TAG_BYTES; ++index) {
        tag[index] = (uint8_t)(seed + index);
    }
}

static ucn_v6_frame_t bootstrap_frame(ucn_v6_address_class_t address_class)
{
    static const uint8_t payload[] = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    ucn_v6_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.address_class = address_class;
    frame.frame_type = UCN_V6_FRAME_BOOTSTRAP;
    frame.flags = UCN_V6_FLAG_PROTOCOL_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_V6_DELIVERY_BEST_EFFORT;
    frame.hop_limit = 1U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = UINT32_C(0x11223344);
    frame.destination_address =
        ucn_v6_address_max_ordinary(address_class) + 1U;
    frame.protocol_opcode = UINT16_C(0x0102);
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    return frame;
}

static ucn_v6_frame_t rich_data_frame(void)
{
    static const uint8_t payload[] = { 1U, 3U, 5U, 7U, 9U };
    ucn_v6_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.address_class = UCN_V6_ADDRESS_CLASS_A2;
    frame.frame_type = UCN_V6_FRAME_DATA;
    frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                  UCN_V6_FLAG_E2E_CONTEXT |
                  UCN_V6_FLAG_MESSAGE_CONTEXT |
                  UCN_V6_FLAG_PATH_CONTEXT |
                  UCN_V6_FLAG_HOP_BUDGET_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q2;
    frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    frame.hop_limit = 17U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = UINT32_C(0x10203040);
    frame.source_address = UINT32_C(0x010203);
    frame.destination_address = UINT32_C(0x040506);
    frame.source_binding_generation = UINT32_C(0x11121314);
    frame.destination_binding_generation = UINT32_C(0x21222324);
    frame.session_generation = UINT32_C(0x31323334);
    frame.packet_sequence = UINT32_C(0x41424344);
    frame.peer_hop.suite_id = 1U;
    frame.peer_hop.key_id = UINT16_C(0x5152);
    frame.peer_hop.key_generation = UINT32_C(0x53545556);
    frame.e2e.mode = UCN_V6_E2E_AUTH_ONLY;
    frame.e2e.suite_id = 1U;
    frame.e2e.key_id = UINT16_C(0x6162);
    frame.e2e.key_generation = UINT32_C(0x63646566);
    frame.message.source_endpoint = UINT16_C(0x7172);
    frame.message.destination_endpoint = UINT16_C(0x7374);
    frame.message.interaction_role = UCN_V6_INTERACTION_REQUEST;
    frame.message.operation_id = UINT64_C(0x0102030405060708);
    frame.path.path_id = UINT16_C(0x7576);
    frame.path.path_generation = UINT32_C(0x7778797A);
    frame.hop_budget.initial_budget_us = UINT64_C(90000);
    frame.hop_budget.remaining_budget_us = UINT64_C(70000);
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    fill_tag(frame.e2e_tag, 0x80U);
    fill_tag(frame.link_tag, 0xA0U);
    return frame;
}

static void rewrite_crc(uint8_t *wire, size_t length)
{
    uint32_t crc = ucn_v6_crc32c(wire, length - 4U);
    wire[length - 4U] = (uint8_t)(crc >> 24U);
    wire[length - 3U] = (uint8_t)(crc >> 16U);
    wire[length - 2U] = (uint8_t)(crc >> 8U);
    wire[length - 1U] = (uint8_t)crc;
}

static int test_base_sizes_and_golden_vectors(void)
{
    static const uint8_t *const golden[] = {
        golden_a0, golden_a1, golden_a2, golden_a3
    };
    static const size_t lengths[] = {
        sizeof(golden_a0), sizeof(golden_a1),
        sizeof(golden_a2), sizeof(golden_a3)
    };
    static const size_t base_lengths[] = {
        UCN_V6_BASE_FRAME_BYTES_A0, UCN_V6_BASE_FRAME_BYTES_A1,
        UCN_V6_BASE_FRAME_BYTES_A2, UCN_V6_BASE_FRAME_BYTES_A3
    };
    uint8_t output[64];
    ucn_v6_frame_t decoded;
    size_t output_length;
    size_t encoded_size;
    size_t index;

    for (index = 0U; index < 4U; ++index) {
        ucn_v6_frame_t frame =
            bootstrap_frame((ucn_v6_address_class_t)index);
        CHECK(lengths[index] == base_lengths[index] + 6U);
        CHECK(ucn_v6_wire_encoded_size(&frame, &encoded_size) == UCN_V6_OK);
        CHECK(encoded_size == lengths[index]);
        memset(output, 0xA5, sizeof(output));
        output_length = SIZE_MAX;
        CHECK(ucn_v6_wire_encode(&frame, output, sizeof(output),
                                 &output_length) == UCN_V6_OK);
        CHECK(output_length == lengths[index]);
        CHECK(memcmp(output, golden[index], lengths[index]) == 0);
        memset(&decoded, 0xA5, sizeof(decoded));
        CHECK(ucn_v6_wire_decode(output, output_length, &decoded) == UCN_V6_OK);
        CHECK(decoded.address_class == frame.address_class);
        CHECK(decoded.destination_address == frame.destination_address);
        CHECK(decoded.payload_length == frame.payload_length);
        CHECK(memcmp(decoded.payload, frame.payload, frame.payload_length) == 0);
    }
    return 0;
}

static int test_output_capacity_rejection_is_atomic(void)
{
    ucn_v6_frame_t frame = rich_data_frame();
    uint8_t output[256];
    uint8_t before[sizeof(output)];
    size_t required = 0U;
    size_t output_length = SIZE_MAX;

    CHECK(ucn_v6_wire_encoded_size(&frame, &required) == UCN_V6_OK);
    CHECK(required > 0U);
    memset(output, 0xA5, sizeof(output));
    memcpy(before, output, sizeof(before));
    CHECK(ucn_v6_wire_encode(&frame, output, required - 1U,
                             &output_length) == UCN_V6_ERR_NO_SPACE);
    CHECK(output_length == SIZE_MAX);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    return 0;
}

static int test_full_context_round_trip_and_aad(void)
{
    static const uint8_t expected_aad[UCN_V6_CANONICAL_AAD_BYTES] = {
        0x06U, 0x02U, 0xD5U, 0x01U, 0x03U, 0x00U, 0x00U, 0x02U,
        0x02U, 0x01U, 0x01U, 0x01U, 0x61U, 0x62U, 0x63U, 0x64U,
        0x65U, 0x66U, 0x10U, 0x20U, 0x30U, 0x40U, 0x00U, 0x01U,
        0x02U, 0x03U, 0x11U, 0x12U, 0x13U, 0x14U, 0x00U, 0x04U,
        0x05U, 0x06U, 0x21U, 0x22U, 0x23U, 0x24U, 0x31U, 0x32U,
        0x33U, 0x34U, 0x41U, 0x42U, 0x43U, 0x44U, 0x71U, 0x72U,
        0x73U, 0x74U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U,
        0x07U, 0x08U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x75U,
        0x76U, 0x77U, 0x78U, 0x79U, 0x7AU, 0x01U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x01U, 0x5FU, 0x90U, 0x00U, 0x05U
    };
    ucn_v6_frame_t frame = rich_data_frame();
    ucn_v6_frame_t decoded;
    ucn_v6_frame_t mutable_outer;
    uint8_t wire[256];
    uint8_t aad[UCN_V6_CANONICAL_AAD_BYTES];
    uint8_t changed[UCN_V6_CANONICAL_AAD_BYTES];
    size_t wire_length = 0U;
    size_t aad_length = 0U;
    size_t changed_length = 0U;

    CHECK(ucn_v6_wire_encode(&frame, wire, sizeof(wire), &wire_length) ==
          UCN_V6_OK);
    CHECK(ucn_v6_wire_decode(wire, wire_length, &decoded) == UCN_V6_OK);
    CHECK(decoded.source_address == frame.source_address);
    CHECK(decoded.destination_address == frame.destination_address);
    CHECK(decoded.message.operation_id == frame.message.operation_id);
    CHECK(decoded.path.path_generation == frame.path.path_generation);
    CHECK(decoded.hop_budget.remaining_budget_us ==
          frame.hop_budget.remaining_budget_us);
    CHECK(memcmp(decoded.e2e_tag, frame.e2e_tag, sizeof(frame.e2e_tag)) == 0);
    CHECK(memcmp(decoded.link_tag, frame.link_tag, sizeof(frame.link_tag)) == 0);
    CHECK(memcmp(decoded.payload, frame.payload, frame.payload_length) == 0);

    CHECK(ucn_v6_wire_write_canonical_aad(
              &frame, aad, sizeof(aad), &aad_length) == UCN_V6_OK);
    CHECK(aad_length == UCN_V6_CANONICAL_AAD_BYTES);
    CHECK(memcmp(aad, expected_aad, sizeof(expected_aad)) == 0);
    mutable_outer = frame;
    mutable_outer.hop_limit = 2U;
    mutable_outer.peer_hop.key_id = UINT16_C(0x1111);
    mutable_outer.peer_hop.key_generation = 7U;
    mutable_outer.hop_budget.remaining_budget_us = 1U;
    fill_tag(mutable_outer.link_tag, 0x11U);
    CHECK(ucn_v6_wire_write_canonical_aad(
              &mutable_outer, changed, sizeof(changed),
              &changed_length) == UCN_V6_OK);
    CHECK(changed_length == aad_length);
    CHECK(memcmp(aad, changed, aad_length) == 0);
    mutable_outer.hop_budget.initial_budget_us =
        frame.hop_budget.initial_budget_us - 1U;
    CHECK(ucn_v6_wire_write_canonical_aad(
              &mutable_outer, changed, sizeof(changed),
              &changed_length) == UCN_V6_OK);
    CHECK(memcmp(aad, changed, aad_length) != 0);

    memset(changed, 0xA5, sizeof(changed));
    {
        uint8_t before[sizeof(changed)];
        memcpy(before, changed, sizeof(before));
        changed_length = SIZE_MAX;
        CHECK(ucn_v6_wire_write_canonical_aad(
                  &frame, changed, sizeof(changed) - 1U,
                  &changed_length) == UCN_V6_ERR_NO_SPACE);
        CHECK(changed_length == SIZE_MAX);
        CHECK(memcmp(changed, before, sizeof(changed)) == 0);
    }
    return 0;
}

static int expect_encode_reject_no_write(const ucn_v6_frame_t *frame)
{
    uint8_t output[256];
    uint8_t before[sizeof(output)];
    size_t output_length = SIZE_MAX;

    memset(output, 0xA5, sizeof(output));
    memcpy(before, output, sizeof(before));
    CHECK(ucn_v6_wire_encode(frame, output, sizeof(output), &output_length) !=
          UCN_V6_OK);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(output_length == SIZE_MAX);
    return 0;
}

static int test_semantic_negative_matrix(void)
{
    ucn_v6_frame_t base = rich_data_frame();
    ucn_v6_frame_t bad;

    bad = base;
    bad.address_class = (ucn_v6_address_class_t)-1;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.flags |= UCN_V6_FLAG_GROUP_CONTEXT;
    bad.group.group_id = 1U;
    bad.group.group_generation = 1U;
    bad.group.suite_id = 1U;
    bad.group.key_id = 1U;
    bad.group.key_generation = 1U;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.flags |= UCN_V6_FLAG_ROUTE_CONTEXT;
    bad.route_generation = 1U;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.e2e.key_id = 0U;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.flags |= UCN_V6_FLAG_PROTOCOL_CONTEXT;
    bad.protocol_opcode = 1U;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.message.operation_id = 0U;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.message.operation_id = UINT64_MAX;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.packet_sequence = UINT32_MAX;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.hop_budget.remaining_budget_us =
        bad.hop_budget.initial_budget_us + 1U;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = base;
    bad.flags &= (uint8_t)~UCN_V6_FLAG_E2E_CONTEXT;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    bad = bootstrap_frame(UCN_V6_ADDRESS_CLASS_A0);
    bad.traffic_class = UCN_V6_TRAFFIC_Q1;
    CHECK(expect_encode_reject_no_write(&bad) == 0);
    return 0;
}

static int expect_decode_reject_no_write(const uint8_t *wire, size_t length)
{
    ucn_v6_frame_t output;
    ucn_v6_frame_t before;
    memset(&output, 0xA5, sizeof(output));
    before = output;
    CHECK(ucn_v6_wire_decode(wire, length, &output) != UCN_V6_OK);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    return 0;
}

static int test_raw_negative_matrix(void)
{
    ucn_v6_frame_t frame = rich_data_frame();
    uint8_t wire[256];
    uint8_t changed[sizeof(wire)];
    size_t length = 0U;

    CHECK(ucn_v6_wire_encode(&frame, wire, sizeof(wire), &length) ==
          UCN_V6_OK);
    memcpy(changed, wire, length);
    changed[0] ^= 1U;
    CHECK(expect_decode_reject_no_write(changed, length) == 0);
    memcpy(changed, wire, length);
    changed[2] = (uint8_t)((changed[2] & 0xC0U) | 5U);
    rewrite_crc(changed, length);
    CHECK(expect_decode_reject_no_write(changed, length) == 0);
    memcpy(changed, wire, length);
    changed[5] |= 0xF0U;
    rewrite_crc(changed, length);
    CHECK(expect_decode_reject_no_write(changed, length) == 0);
    memcpy(changed, wire, length);
    changed[length - 1U] ^= 1U;
    CHECK(expect_decode_reject_no_write(changed, length) == 0);
    CHECK(expect_decode_reject_no_write(wire, length - 1U) == 0);
    memcpy(changed, wire, length);
    changed[length] = 0U;
    CHECK(expect_decode_reject_no_write(changed, length + 1U) == 0);
    return 0;
}

static int test_group_hello_contract(void)
{
    static const uint8_t payload[] = { 0x31U, 0x32U };
    ucn_v6_frame_t frame;
    uint8_t wire[128];
    size_t length = 0U;

    memset(&frame, 0, sizeof(frame));
    frame.address_class = UCN_V6_ADDRESS_CLASS_A1;
    frame.frame_type = UCN_V6_FRAME_CONTROL;
    frame.flags = UCN_V6_FLAG_GROUP_CONTEXT |
                  UCN_V6_FLAG_PROTOCOL_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q1;
    frame.delivery_guarantee = UCN_V6_DELIVERY_LATEST;
    frame.hop_limit = 1U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = 7U;
    frame.source_address = 1U;
    frame.destination_address = UINT16_MAX;
    frame.source_binding_generation = 1U;
    frame.session_generation = 1U;
    frame.packet_sequence = 1U;
    frame.group.group_id = 1U;
    frame.group.group_generation = 1U;
    frame.group.suite_id = 1U;
    frame.group.key_id = 1U;
    frame.group.key_generation = 1U;
    frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    fill_tag(frame.link_tag, 0x55U);
    CHECK(ucn_v6_wire_encode(&frame, wire, sizeof(wire), &length) ==
          UCN_V6_OK);
    frame.destination_binding_generation = 1U;
    CHECK(expect_encode_reject_no_write(&frame) == 0);
    frame.destination_binding_generation = 0U;
    frame.hop_limit = 2U;
    CHECK(expect_encode_reject_no_write(&frame) == 0);
    return 0;
}

static int test_peer_control_route_contract(void)
{
    static const uint8_t payload[] = { 0x44U };
    ucn_v6_frame_t frame;
    ucn_v6_frame_t decoded;
    uint8_t wire[128];
    size_t length = 0U;

    memset(&frame, 0, sizeof(frame));
    frame.address_class = UCN_V6_ADDRESS_CLASS_A3;
    frame.frame_type = UCN_V6_FRAME_CONTROL;
    frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                  UCN_V6_FLAG_PROTOCOL_CONTEXT |
                  UCN_V6_FLAG_ROUTE_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    frame.hop_limit = 8U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = 1U;
    frame.source_address = 2U;
    frame.destination_address = 3U;
    frame.source_binding_generation = 4U;
    frame.destination_binding_generation = 5U;
    frame.session_generation = 6U;
    frame.packet_sequence = 7U;
    frame.peer_hop.suite_id = 1U;
    frame.peer_hop.key_id = 8U;
    frame.peer_hop.key_generation = 9U;
    frame.protocol_opcode = UINT16_C(0x1122);
    frame.route_generation = 10U;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    fill_tag(frame.link_tag, 0xD0U);
    CHECK(ucn_v6_wire_encode(&frame, wire, sizeof(wire), &length) ==
          UCN_V6_OK);
    CHECK(ucn_v6_wire_decode(wire, length, &decoded) == UCN_V6_OK);
    CHECK(decoded.protocol_opcode == frame.protocol_opcode);
    CHECK(decoded.route_generation == frame.route_generation);
    CHECK(decoded.path.path_id == 0U);
    return 0;
}

static uint32_t fuzz_next(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static int test_deterministic_fuzz_and_canonical_round_trip(void)
{
    uint32_t state = UINT32_C(0x56300321);
    uint8_t input[256];
    uint8_t reencoded[256];
    ucn_v6_frame_t decoded;
    ucn_v6_frame_t before;
    size_t iteration;
    size_t index;
    size_t input_length;
    size_t output_length;
    ucn_v6_result_t result;

    for (iteration = 0U; iteration < 4096U; ++iteration) {
        input_length = (size_t)(fuzz_next(&state) % sizeof(input));
        for (index = 0U; index < input_length; ++index) {
            input[index] = (uint8_t)fuzz_next(&state);
        }
        memset(&decoded, 0xA5, sizeof(decoded));
        before = decoded;
        result = ucn_v6_wire_decode(input, input_length, &decoded);
        if (result == UCN_V6_OK) {
            memset(reencoded, 0x5A, sizeof(reencoded));
            output_length = SIZE_MAX;
            CHECK(ucn_v6_wire_encode(&decoded, reencoded,
                                     sizeof(reencoded),
                                     &output_length) == UCN_V6_OK);
            CHECK(output_length == input_length);
            CHECK(memcmp(reencoded, input, input_length) == 0);
        } else {
            CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);
        }
    }
    return 0;
}

int main(void)
{
    CHECK(test_base_sizes_and_golden_vectors() == 0);
    CHECK(test_output_capacity_rejection_is_atomic() == 0);
    CHECK(test_full_context_round_trip_and_aad() == 0);
    CHECK(test_semantic_negative_matrix() == 0);
    CHECK(test_raw_negative_matrix() == 0);
    CHECK(test_group_hello_contract() == 0);
    CHECK(test_peer_control_route_contract() == 0);
    CHECK(test_deterministic_fuzz_and_canonical_round_trip() == 0);
    puts("ucn v6 wire tests passed");
    return 0;
}
