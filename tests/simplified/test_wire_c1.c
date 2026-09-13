#include "internal/ucn_wire.h"

#include <stdio.h>
#include <string.h>

typedef struct golden_case {
    uint8_t width;
    uint32_t source;
    uint32_t destination;
    uint16_t service;
    uint32_t sequence;
    uint8_t traffic_class;
    uint8_t hop_limit;
    const uint8_t *payload;
    size_t payload_bytes;
    const uint8_t *wire;
    size_t wire_bytes;
} golden_case_t;

static int failures;

#define CHECK(condition_)                                                     \
    do {                                                                      \
        if (!(condition_)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition_);    \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

/* EN: wire_a1 is the byte-for-byte authoritative C1/A1 Golden from
 * V6S-00-04.  A0/A2/A3 are independent supplemental fixtures.  None of the
 * vectors may be regenerated from the implementation.
 * 中文：wire_a1 逐字节复制自 V6S-00-04 的权威 C1/A1 Golden；A0/A2/A3
 * 是独立补充 fixture。所有向量都禁止由当前实现反向生成，以免编解码双方同错。 */
static const uint8_t payload_a0[] = {0xDEU, 0xADU};
static const uint8_t wire_a0[] = {
    0x61U, 0x80U, 0x05U, 0x01U, 0x02U, 0x12U, 0x34U,
    0x11U, 0x22U, 0x33U, 0x44U, 0xDEU, 0xADU};
static const uint8_t payload_a1[] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
static const uint8_t wire_a1[] = {
    0x61U, 0x40U, 0x03U, 0x12U, 0x34U, 0x56U, 0x78U,
    0x01U, 0x02U, 0x01U, 0x02U, 0x03U, 0x04U, 0xDEU, 0xADU,
    0xBEU, 0xEFU};
static const uint8_t payload_a2[] = {0x00U, 0xFFU};
static const uint8_t wire_a2[] = {
    0x61U, 0x40U, 0x01U, 0x01U, 0x02U, 0x03U, 0x0AU, 0x0BU, 0x0CU,
    0x01U, 0x02U, 0xA1U, 0xB2U, 0xC3U, 0xD4U, 0x00U, 0xFFU};
static const uint8_t payload_a3[] = {0x55U};
static const uint8_t wire_a3[] = {
    0x61U, 0x00U, 0x20U, 0x01U, 0x02U, 0x03U, 0x04U,
    0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xBEU, 0xEFU,
    0x89U, 0xABU, 0xCDU, 0xEFU, 0x55U};

static const golden_case_t golden[] = {
    {1U, 1U, 2U, 0x1234U, UINT32_C(0x11223344), UCN_TRAFFIC_Q2, 5U,
     payload_a0, sizeof(payload_a0), wire_a0, sizeof(wire_a0)},
    {2U, 0x1234U, 0x5678U, 0x0102U, UINT32_C(0x01020304),
     UCN_TRAFFIC_Q1, 3U, payload_a1, sizeof(payload_a1), wire_a1,
     sizeof(wire_a1)},
    {3U, UINT32_C(0x010203), UINT32_C(0x0A0B0C), 0x0102U,
     UINT32_C(0xA1B2C3D4), UCN_TRAFFIC_Q1, 1U, payload_a2,
     sizeof(payload_a2), wire_a2, sizeof(wire_a2)},
    {4U, UINT32_C(0x01020304), UINT32_C(0xA1A2A3A4), 0xBEEFU,
     UINT32_C(0x89ABCDEF), UCN_TRAFFIC_Q0, 32U, payload_a3,
     sizeof(payload_a3), wire_a3, sizeof(wire_a3)}};

static ucn_i_c1_frame_t make_frame(const golden_case_t *item)
{
    ucn_i_c1_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.payload = item->payload;
    frame.payload_bytes = item->payload_bytes;
    frame.source_address = item->source;
    frame.destination_address = item->destination;
    frame.origin_sequence = item->sequence;
    frame.service_id = item->service;
    frame.traffic_class = item->traffic_class;
    frame.hop_limit = item->hop_limit;
    return frame;
}

static void test_golden(void)
{
    size_t index;

    for (index = 0U; index < sizeof(golden) / sizeof(golden[0]); ++index) {
        const golden_case_t *item = &golden[index];
        ucn_i_c1_frame_t frame = make_frame(item);
        ucn_i_c1_frame_t decoded;
        uint8_t output[64];
        size_t output_bytes = SIZE_MAX;
        size_t encoded_bytes = SIZE_MAX;

        memset(output, 0xA5, sizeof(output));
        memset(&decoded, 0x5A, sizeof(decoded));
        CHECK(ucn_i_c1_encoded_size(item->width, item->payload_bytes,
                                    &encoded_bytes) == UCN_OK);
        CHECK(encoded_bytes == item->wire_bytes);
        CHECK(ucn_i_c1_encode(&frame, item->width, output, sizeof(output),
                              &output_bytes) == UCN_OK);
        CHECK(output_bytes == item->wire_bytes);
        CHECK(memcmp(output, item->wire, item->wire_bytes) == 0);
        CHECK(ucn_i_c1_decode(item->wire, item->wire_bytes, item->width,
                              &decoded) == UCN_OK);
        CHECK(decoded.source_address == item->source);
        CHECK(decoded.destination_address == item->destination);
        CHECK(decoded.service_id == item->service);
        CHECK(decoded.origin_sequence == item->sequence);
        CHECK(decoded.traffic_class == item->traffic_class);
        CHECK(decoded.hop_limit == item->hop_limit);
        CHECK(decoded.payload_bytes == item->payload_bytes);
        CHECK(memcmp(decoded.payload, item->payload, item->payload_bytes) == 0);
    }
}

static void test_in_place(void)
{
    ucn_i_c1_frame_t frame = make_frame(&golden[0]);
    uint8_t buffer[64];
    size_t output_bytes = SIZE_MAX;

    memset(buffer, 0xCC, sizeof(buffer));
    memcpy(buffer, payload_a0, sizeof(payload_a0));
    frame.payload = buffer;
    CHECK(ucn_i_c1_encode_in_place(&frame, 1U, buffer, sizeof(buffer),
                                   &output_bytes) == UCN_OK);
    CHECK(output_bytes == sizeof(wire_a0));
    CHECK(memcmp(buffer, wire_a0, sizeof(wire_a0)) == 0);
    CHECK(frame.payload == buffer + 11U);
}

static void expect_decode_failure(const uint8_t *wire,
                                  size_t wire_bytes,
                                  uint8_t width,
                                  ucn_result_t expected)
{
    ucn_i_c1_frame_t decoded;
    ucn_i_c1_frame_t before;

    memset(&decoded, 0xA7, sizeof(decoded));
    before = decoded;
    CHECK(ucn_i_c1_decode(wire, wire_bytes, width, &decoded) == expected);
    CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);
}

static void test_negative_decode(void)
{
    uint8_t bad[sizeof(wire_a0)];
    size_t index;

    memcpy(bad, wire_a0, sizeof(bad));
    bad[0] = 0x51U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[0] = 0x66U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[0] = 0x62U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_UNSUPPORTED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[1] |= 0x30U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[1] |= 0x10U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_UNSUPPORTED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[1] |= 0x04U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_UNSUPPORTED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[1] |= 0x01U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_UNSUPPORTED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[2] = 0xC5U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[2] = 0x45U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_UNSUPPORTED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[2] = 0U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[3] = 0U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[3] = UINT8_MAX;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[4] = 0U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[4] = UINT8_MAX;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[5] = 0U;
    bad[6] = 0U;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    bad[5] = UINT8_MAX;
    bad[6] = UINT8_MAX;
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    memcpy(bad, wire_a0, sizeof(bad));
    memset(bad + 7U, 0, 4U);
    expect_decode_failure(bad, sizeof(bad), 1U, UCN_ERR_MALFORMED);
    expect_decode_failure(wire_a0, sizeof(wire_a0) - 1U, 0U,
                          UCN_ERR_ARGUMENT);
    expect_decode_failure(wire_a0, 10U, 1U, UCN_ERR_MALFORMED);

    for (index = 0U; index < sizeof(golden) / sizeof(golden[0]); ++index) {
        const golden_case_t *item = &golden[index];
        uint8_t copy[64];
        size_t address_offset = 3U;

        memcpy(copy, item->wire, item->wire_bytes);
        memset(copy + address_offset, 0, item->width);
        expect_decode_failure(copy, item->wire_bytes, item->width,
                              UCN_ERR_MALFORMED);
        memcpy(copy, item->wire, item->wire_bytes);
        memset(copy + address_offset, 0xFF, item->width);
        expect_decode_failure(copy, item->wire_bytes, item->width,
                              UCN_ERR_MALFORMED);
        memcpy(copy, item->wire, item->wire_bytes);
        memset(copy + address_offset + item->width, 0, item->width);
        expect_decode_failure(copy, item->wire_bytes, item->width,
                              UCN_ERR_MALFORMED);
        memcpy(copy, item->wire, item->wire_bytes);
        memset(copy + address_offset + item->width, 0xFF, item->width);
        expect_decode_failure(copy, item->wire_bytes, item->width,
                              UCN_ERR_MALFORMED);
    }
}

static void test_encode_failure_atomicity(void)
{
    ucn_i_c1_frame_t frame = make_frame(&golden[0]);
    ucn_i_c1_frame_t frame_before;
    uint8_t output[64];
    uint8_t before[64];
    size_t output_bytes = SIZE_MAX;
    size_t before_bytes = output_bytes;
    size_t encoded_size = SIZE_MAX;
    size_t encoded_size_before = encoded_size;

    memset(output, 0xA5, sizeof(output));
    memcpy(before, output, sizeof(output));
    CHECK(ucn_i_c1_encode(&frame, 1U, output, sizeof(wire_a0) - 1U,
                          &output_bytes) == UCN_ERR_NO_SPACE);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(output_bytes == before_bytes);
    CHECK(ucn_i_c1_encode(&frame, 1U, (uint8_t *)frame.payload,
                          frame.payload_bytes, &output_bytes) ==
          UCN_ERR_ARGUMENT);
    CHECK(output_bytes == before_bytes);
    frame.origin_sequence = 0U;
    CHECK(ucn_i_c1_encode(&frame, 1U, output, sizeof(output),
                          &output_bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(output_bytes == before_bytes);

    frame = make_frame(&golden[0]);
    frame.source_address = 0U;
    CHECK(ucn_i_c1_encode(&frame, 1U, output, sizeof(output),
                          &output_bytes) == UCN_ERR_ARGUMENT);
    frame = make_frame(&golden[0]);
    frame.destination_address = UINT8_MAX;
    CHECK(ucn_i_c1_encode(&frame, 1U, output, sizeof(output),
                          &output_bytes) == UCN_ERR_ARGUMENT);
    frame = make_frame(&golden[0]);
    frame.service_id = UINT16_MAX;
    CHECK(ucn_i_c1_encode(&frame, 1U, output, sizeof(output),
                          &output_bytes) == UCN_ERR_ARGUMENT);
    frame = make_frame(&golden[0]);
    frame.traffic_class = UCN_TRAFFIC_CLASS_COUNT;
    CHECK(ucn_i_c1_encode(&frame, 1U, output, sizeof(output),
                          &output_bytes) == UCN_ERR_ARGUMENT);
    frame = make_frame(&golden[0]);
    frame.hop_limit = 64U;
    CHECK(ucn_i_c1_encode(&frame, 1U, output, sizeof(output),
                          &output_bytes) == UCN_ERR_ARGUMENT);
    frame = make_frame(&golden[0]);
    frame.payload = NULL;
    CHECK(ucn_i_c1_encode(&frame, 1U, output, sizeof(output),
                          &output_bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(output_bytes == before_bytes);

    CHECK(ucn_i_c1_encoded_size(4U, SIZE_MAX, &encoded_size) ==
          UCN_ERR_EXHAUSTED);
    CHECK(encoded_size == encoded_size_before);

    frame = make_frame(&golden[0]);
    memset(output, 0xA5, sizeof(output));
    memcpy(output, frame.payload, frame.payload_bytes);
    frame.payload = output;
    frame_before = frame;
    memcpy(before, output, sizeof(output));
    CHECK(ucn_i_c1_encode_in_place(&frame, 1U, output,
                                   sizeof(wire_a0) - 1U,
                                   &output_bytes) == UCN_ERR_NO_SPACE);
    CHECK(memcmp(&frame, &frame_before, sizeof(frame)) == 0);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(output_bytes == before_bytes);
}

static uint32_t fuzz_next(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void test_fixed_seed_decode_reencode_property(void)
{
    uint32_t state = UINT32_C(0xC15A4E5D);
    uint32_t iteration;
    uint32_t successes = 0U;
    uint32_t failures_seen = 0U;

    for (iteration = 0U; iteration < 4096U; ++iteration) {
        ucn_i_c1_frame_t decoded;
        ucn_i_c1_frame_t before;
        uint8_t input[64];
        uint8_t output[64];
        uint8_t payload[32];
        uint8_t width = (uint8_t)(1U + (fuzz_next(&state) % 4U));
        size_t input_bytes = fuzz_next(&state) % (sizeof(input) + 1U);
        size_t output_bytes = SIZE_MAX;
        size_t index;
        ucn_result_t result;

        if ((iteration % 16U) == 0U) {
            ucn_i_c1_frame_t valid;
            uint32_t limit = width == 4U
                                 ? UINT32_MAX
                                 : (UINT32_C(1) << (width * 8U)) - 1U;
            size_t payload_bytes = fuzz_next(&state) % sizeof(payload);

            for (index = 0U; index < payload_bytes; ++index) {
                payload[index] = (uint8_t)fuzz_next(&state);
            }
            memset(&valid, 0, sizeof(valid));
            valid.payload = payload;
            valid.payload_bytes = payload_bytes;
            valid.source_address =
                1U + (fuzz_next(&state) % (limit - 1U));
            valid.destination_address =
                1U + (fuzz_next(&state) % (limit - 1U));
            valid.origin_sequence = fuzz_next(&state);
            if (valid.origin_sequence == 0U) {
                valid.origin_sequence = 1U;
            }
            valid.service_id =
                (uint16_t)(1U + (fuzz_next(&state) % (UINT16_MAX - 1U)));
            valid.traffic_class =
                (uint8_t)(fuzz_next(&state) % UCN_TRAFFIC_CLASS_COUNT);
            valid.hop_limit = (uint8_t)(1U + (fuzz_next(&state) % 63U));
            CHECK(ucn_i_c1_encode(&valid, width, input, sizeof(input),
                                  &input_bytes) == UCN_OK);
        } else {
            for (index = 0U; index < input_bytes; ++index) {
                input[index] = (uint8_t)fuzz_next(&state);
            }
        }
        memset(&decoded, 0xA7, sizeof(decoded));
        before = decoded;
        result = ucn_i_c1_decode(input, input_bytes, width, &decoded);
        if (result == UCN_OK) {
            ++successes;
            CHECK(ucn_i_c1_encode(&decoded, width, output, sizeof(output),
                                  &output_bytes) == UCN_OK);
            CHECK(output_bytes == input_bytes);
            CHECK(memcmp(output, input, input_bytes) == 0);
        } else {
            ++failures_seen;
            CHECK(result == UCN_ERR_MALFORMED ||
                  result == UCN_ERR_UNSUPPORTED);
            CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);
        }
    }
    CHECK(successes != 0U);
    CHECK(failures_seen != 0U);
}

int main(void)
{
    test_golden();
    test_in_place();
    test_negative_decode();
    test_encode_failure_atomicity();
    test_fixed_seed_decode_reencode_property();
    if (failures != 0) {
        printf("wire C1 failures=%d\n", failures);
        return 1;
    }
    printf("wire C1 tests passed\n");
    return 0;
}
