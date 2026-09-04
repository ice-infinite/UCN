#include "ucn/ucn_realtime.h"

#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "assertion failed: %s:%d: %s\n",          \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (0)

/* EN: Returns the canonical semantic value behind each frozen vector.
 * 中文：返回每条冻结向量对应的规范语义对象。 */
static ucn_realtime_envelope_t make_local_stamp(void)
{
    ucn_realtime_envelope_t envelope;

    (void)memset(&envelope, 0, sizeof(envelope));
    envelope.mode = UCN_REALTIME_MODE_LOCAL_STAMP;
    envelope.uncertainty_class = UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN;
    envelope.sample_capture_hardware = true;
    envelope.capture_time_us = UINT64_C(0x0102030405060708);
    return envelope;
}

static ucn_realtime_envelope_t make_synced_stamp(void)
{
    ucn_realtime_envelope_t envelope;

    (void)memset(&envelope, 0, sizeof(envelope));
    envelope.mode = UCN_REALTIME_MODE_SYNCED_STAMP;
    envelope.uncertainty_class = 10U;
    envelope.sample_capture_hardware = true;
    envelope.domain_time_valid = true;
    envelope.clock_domain_id = UINT16_C(0x1234);
    envelope.domain_generation = UINT32_C(0x01020304);
    envelope.capture_time_us = UINT64_C(0x1122334455667788);
    return envelope;
}

static ucn_realtime_envelope_t make_deadline(void)
{
    ucn_realtime_envelope_t envelope;

    (void)memset(&envelope, 0, sizeof(envelope));
    envelope.mode = UCN_REALTIME_MODE_DEADLINE;
    envelope.uncertainty_class = 11U;
    envelope.domain_time_valid = true;
    envelope.source_holdover = true;
    envelope.clock_domain_id = UINT16_C(0xFFFE);
    envelope.domain_generation = UINT32_C(0x7FFFFFFF);
    envelope.capture_time_us = UINT64_MAX;
    return envelope;
}

/* EN: Compares semantic fields without relying on host struct padding.
 * 中文：逐语义字段比较，避免依赖 Host 结构体填充。 */
static bool envelope_equals(const ucn_realtime_envelope_t *left,
                            const ucn_realtime_envelope_t *right)
{
    return left->mode == right->mode &&
           left->uncertainty_class == right->uncertainty_class &&
           left->sample_capture_hardware == right->sample_capture_hardware &&
           left->domain_time_valid == right->domain_time_valid &&
           left->source_holdover == right->source_holdover &&
           left->clock_domain_id == right->clock_domain_id &&
           left->domain_generation == right->domain_generation &&
           left->capture_time_us == right->capture_time_us;
}

static bool verify_vector(const ucn_realtime_envelope_t *semantic,
                          const uint8_t expected[16])
{
    uint8_t encoded[UCN_REALTIME_ENVELOPE_WIRE_BYTES];
    ucn_realtime_envelope_t decoded;

    (void)memset(encoded, 0xA5, sizeof(encoded));
    (void)memset(&decoded, 0x5A, sizeof(decoded));
    TEST_ASSERT(ucn_realtime_envelope_is_valid(semantic));
    TEST_ASSERT(ucn_realtime_envelope_encode(semantic, encoded) == UCN_OK);
    TEST_ASSERT(memcmp(encoded, expected, sizeof(encoded)) == 0);
    TEST_ASSERT(ucn_realtime_envelope_decode(
                    expected, sizeof(encoded), &decoded) == UCN_OK);
    TEST_ASSERT(envelope_equals(&decoded, semantic));
    return true;
}

static bool test_frozen_vectors(void)
{
    static const uint8_t local_vector[16] = {
        0x11U, 0xFCU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U};
    static const uint8_t synced_vector[16] = {
        0x12U, 0x56U, 0x12U, 0x34U, 0x01U, 0x02U, 0x03U, 0x04U,
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U};
    static const uint8_t deadline_vector[16] = {
        0x13U, 0x5BU, 0xFFU, 0xFEU, 0x7FU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    ucn_realtime_envelope_t local = make_local_stamp();
    ucn_realtime_envelope_t synced = make_synced_stamp();
    ucn_realtime_envelope_t deadline = make_deadline();

    TEST_ASSERT(verify_vector(&local, local_vector));
    TEST_ASSERT(verify_vector(&synced, synced_vector));
    TEST_ASSERT(verify_vector(&deadline, deadline_vector));
    return true;
}

static bool encode_rejects_without_write(
    const ucn_realtime_envelope_t *envelope)
{
    uint8_t output[UCN_REALTIME_ENVELOPE_WIRE_BYTES];
    uint8_t before[UCN_REALTIME_ENVELOPE_WIRE_BYTES];

    (void)memset(output, 0xA5, sizeof(output));
    (void)memcpy(before, output, sizeof(before));
    TEST_ASSERT(ucn_realtime_envelope_encode(envelope, output) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(memcmp(output, before, sizeof(output)) == 0);
    return true;
}

static bool test_semantic_validation_and_encode_no_write(void)
{
    ucn_realtime_envelope_t value = make_synced_stamp();
    uint8_t output[UCN_REALTIME_ENVELOPE_WIRE_BYTES];

    TEST_ASSERT(!ucn_realtime_envelope_is_valid(NULL));
    TEST_ASSERT(ucn_realtime_envelope_encode(NULL, output) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_realtime_envelope_encode(&value, NULL) ==
                UCN_ERR_ARGUMENT);

    value.mode = UCN_REALTIME_MODE_NONE;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_synced_stamp();
    value.mode = UINT8_MAX;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_synced_stamp();
    value.uncertainty_class = 32U;
    TEST_ASSERT(encode_rejects_without_write(&value));

    value = make_local_stamp();
    value.uncertainty_class = 30U;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_local_stamp();
    value.domain_time_valid = true;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_local_stamp();
    value.source_holdover = true;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_local_stamp();
    value.clock_domain_id = 1U;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_local_stamp();
    value.domain_generation = 1U;
    TEST_ASSERT(encode_rejects_without_write(&value));

    value = make_synced_stamp();
    value.uncertainty_class = UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_synced_stamp();
    value.domain_time_valid = false;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_synced_stamp();
    value.clock_domain_id = 0U;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_synced_stamp();
    value.clock_domain_id = UINT16_MAX;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_synced_stamp();
    value.domain_generation = 0U;
    TEST_ASSERT(encode_rejects_without_write(&value));
    value = make_synced_stamp();
    value.domain_generation = UCN_REALTIME_DOMAIN_GENERATION_MAX + 1U;
    TEST_ASSERT(encode_rejects_without_write(&value));
    return true;
}

static bool decode_rejects_without_write(const uint8_t input[16],
                                         size_t input_length,
                                         ucn_result_t expected_result)
{
    ucn_realtime_envelope_t output;
    ucn_realtime_envelope_t before;

    (void)memset(&output, 0xA5, sizeof(output));
    (void)memcpy(&before, &output, sizeof(before));
    TEST_ASSERT(ucn_realtime_envelope_decode(input, input_length, &output) ==
                expected_result);
    TEST_ASSERT(memcmp(&output, &before, sizeof(output)) == 0);
    return true;
}

static bool test_decode_negative_matrix(void)
{
    static const uint8_t local_vector[16] = {
        0x11U, 0xFCU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U};
    static const uint8_t synced_vector[16] = {
        0x12U, 0x56U, 0x12U, 0x34U, 0x01U, 0x02U, 0x03U, 0x04U,
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U};
    uint8_t malformed[16];
    ucn_realtime_envelope_t output;

    TEST_ASSERT(ucn_realtime_envelope_decode(NULL, 16U, &output) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_realtime_envelope_decode(synced_vector, 16U, NULL) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(decode_rejects_without_write(synced_vector, 15U,
                                             UCN_ERR_MALFORMED));
    TEST_ASSERT(decode_rejects_without_write(synced_vector, 17U,
                                             UCN_ERR_MALFORMED));

    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[0] = 0x22U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_VERSION));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[0] |= 0x04U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[0] |= 0x08U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[0] = 0x10U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[1] = 0xFAU;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[1] &= (uint8_t)~0x02U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[2] = 0U;
    malformed[3] = 0U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[2] = 0xFFU;
    malformed[3] = 0xFFU;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    (void)memset(&malformed[4], 0, 4U);
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, synced_vector, sizeof(malformed));
    malformed[4] = 0x80U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));

    /* LOCAL_STAMP has its own canonical zero-domain contract. */
    (void)memcpy(malformed, local_vector, sizeof(malformed));
    malformed[1] = 0xF4U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, local_vector, sizeof(malformed));
    malformed[1] |= 0x02U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, local_vector, sizeof(malformed));
    malformed[1] |= 0x01U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, local_vector, sizeof(malformed));
    malformed[3] = 1U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    (void)memcpy(malformed, local_vector, sizeof(malformed));
    malformed[7] = 1U;
    TEST_ASSERT(decode_rejects_without_write(malformed, sizeof(malformed),
                                             UCN_ERR_MALFORMED));
    return true;
}

static bool test_uncertainty_classes(void)
{
    static const struct {
        bool known;
        uint64_t upper_bound_us;
        uint8_t expected_class;
    } encode_cases[] = {
        {false, 0U, 31U},
        {false, UINT64_MAX, 31U},
        {true, 0U, 0U},
        {true, 1U, 0U},
        {true, 2U, 1U},
        {true, 1024U, 10U},
        {true, 1025U, 11U},
        {true, UINT64_C(1) << 30U, 30U},
        {true, (UINT64_C(1) << 30U) + 1U, 31U}};
    size_t index;
    uint8_t encoded = 0xA5U;
    bool known;
    uint32_t upper_bound;

    for (index = 0U; index < sizeof(encode_cases) / sizeof(encode_cases[0]);
         ++index) {
        encoded = 0xA5U;
        TEST_ASSERT(ucn_realtime_uncertainty_class_encode(
                        encode_cases[index].known,
                        encode_cases[index].upper_bound_us,
                        &encoded) == UCN_OK);
        TEST_ASSERT(encoded == encode_cases[index].expected_class);
    }
    TEST_ASSERT(ucn_realtime_uncertainty_class_encode(true, 1U, NULL) ==
                UCN_ERR_ARGUMENT);

    known = false;
    upper_bound = 0U;
    TEST_ASSERT(ucn_realtime_uncertainty_class_decode(0U, &known,
                                                       &upper_bound) == UCN_OK);
    TEST_ASSERT(known && upper_bound == 1U);
    TEST_ASSERT(ucn_realtime_uncertainty_class_decode(10U, &known,
                                                       &upper_bound) == UCN_OK);
    TEST_ASSERT(known && upper_bound == 1024U);
    TEST_ASSERT(ucn_realtime_uncertainty_class_decode(30U, &known,
                                                       &upper_bound) == UCN_OK);
    TEST_ASSERT(known && upper_bound == (UINT32_C(1) << 30U));
    TEST_ASSERT(ucn_realtime_uncertainty_class_decode(31U, &known,
                                                       &upper_bound) == UCN_OK);
    TEST_ASSERT(!known && upper_bound == 0U);

    known = true;
    upper_bound = UINT32_C(0xA5A5A5A5);
    TEST_ASSERT(ucn_realtime_uncertainty_class_decode(32U, &known,
                                                       &upper_bound) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(known && upper_bound == UINT32_C(0xA5A5A5A5));
    TEST_ASSERT(ucn_realtime_uncertainty_class_decode(0U, NULL,
                                                       &upper_bound) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_realtime_uncertainty_class_decode(0U, &known, NULL) ==
                UCN_ERR_ARGUMENT);
    return true;
}

/* EN: Verifies complete known-component aggregation and overflow rejection.
 * 中文：验证完整已知分量聚合以及溢出拒绝。 */
static bool test_uncertainty_aggregate(void)
{
    ucn_realtime_uncertainty_components_t components;
    uint32_t output = UINT32_C(0xA5A5A5A5);

    (void)memset(&components, 0, sizeof(components));
    components.timer_resolution_bound_us = 1U;
    components.link_timestamp_capture_bound_us = 2U;
    components.filter_residual_bound_us = 3U;
    components.arithmetic_rounding_bound_us = 4U;
    components.known_mask = UCN_REALTIME_UNCERTAINTY_SYNC_REQUIRED_MASK;
    TEST_ASSERT(ucn_realtime_uncertainty_aggregate(
                    &components, true, 5U, &output) == UCN_OK);
    TEST_ASSERT(output == 15U);

    output = UINT32_C(0xA5A5A5A5);
    components.known_mask &=
        (uint8_t)~UCN_REALTIME_UNCERTAINTY_FILTER_RESIDUAL;
    TEST_ASSERT(ucn_realtime_uncertainty_aggregate(
                    &components, true, 5U, &output) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(output == UINT32_C(0xA5A5A5A5));
    components.known_mask = UCN_REALTIME_UNCERTAINTY_SYNC_REQUIRED_MASK;
    TEST_ASSERT(ucn_realtime_uncertainty_aggregate(
                    &components, false, 5U, &output) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(output == UINT32_C(0xA5A5A5A5));

    components.timer_resolution_bound_us = UINT32_MAX;
    TEST_ASSERT(ucn_realtime_uncertainty_aggregate(
                    &components, true, 5U, &output) == UCN_ERR_TOO_LARGE);
    TEST_ASSERT(output == UINT32_C(0xA5A5A5A5));
    return true;
}

int main(void)
{
    if (UCN_REALTIME_ENVELOPE_WIRE_BYTES != 16U ||
        !test_frozen_vectors() ||
        !test_semantic_validation_and_encode_no_write() ||
        !test_decode_negative_matrix() ||
        !test_uncertainty_classes() || !test_uncertainty_aggregate()) {
        return 1;
    }
    (void)puts("UCN Realtime Metadata v1 codec tests passed.");
    return 0;
}
