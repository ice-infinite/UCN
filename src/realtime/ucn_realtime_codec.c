#include "internal/ucn_realtime.h"

#include "internal/ucn_checked.h"
#include "internal/ucn_digest.h"

#include <string.h>

ucn_result_t ucn_i_realtime_domain_body_digest(const uint8_t *body,
                                               uint8_t digest[16])
{
    ucn_i_sha256_workspace_t workspace;
    return ucn_i_sha256_128(body, UCN_I_REALTIME_DOMAIN_RECORD_BYTES,
                            digest, &workspace);
}

#define MODE_MASK UINT8_C(0x03)
#define VERSION_SHIFT 4U
#define UNCERTAINTY_SHIFT 3U
#define FLAG_CAPTURE_HW UINT8_C(0x04)
#define FLAG_DOMAIN_VALID UINT8_C(0x02)
#define FLAG_SOURCE_HOLDOVER UINT8_C(0x01)

static void write_be16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void write_be32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void write_be64(uint8_t *output, uint64_t value)
{
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - (uint8_t)(index * 8U)));
    }
}

static uint16_t read_be16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static uint32_t read_be32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | (uint32_t)input[3];
}

static uint64_t read_be64(const uint8_t *input)
{
    uint64_t value = 0U;
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        value = (value << 8) | input[index];
    }
    return value;
}

bool ucn_i_realtime_envelope_valid(
    const ucn_i_realtime_envelope_t *envelope)
{
    if (envelope == NULL || envelope->reserved_zero != 0U ||
        envelope->sample_capture_hardware > 1U ||
        envelope->domain_time_valid > 1U ||
        envelope->source_holdover > 1U) {
        return false;
    }
    if (envelope->mode == UCN_I_REALTIME_LOCAL_STAMP) {
        return envelope->uncertainty_class ==
                   UCN_I_REALTIME_UNCERTAINTY_UNKNOWN &&
               envelope->clock_domain_id == 0U &&
               envelope->domain_generation == 0U &&
               envelope->domain_time_valid == 0U &&
               envelope->source_holdover == 0U;
    }
    if (envelope->mode != UCN_I_REALTIME_SYNCED_STAMP &&
        envelope->mode != UCN_I_REALTIME_DEADLINE) {
        return false;
    }
    return envelope->uncertainty_class <
               UCN_I_REALTIME_UNCERTAINTY_UNKNOWN &&
           envelope->clock_domain_id != 0U &&
           envelope->clock_domain_id <= UCN_I_REALTIME_DOMAIN_ID_MAX &&
           envelope->domain_generation != 0U &&
           envelope->domain_generation <=
               UCN_I_REALTIME_DOMAIN_GENERATION_MAX &&
           envelope->domain_time_valid == 1U;
}

ucn_result_t ucn_i_realtime_envelope_encode(
    const ucn_i_realtime_envelope_t *envelope,
    uint8_t output[UCN_I_REALTIME_ENVELOPE_BYTES])
{
    uint8_t encoded[UCN_I_REALTIME_ENVELOPE_BYTES];

    if (!ucn_i_realtime_envelope_valid(envelope) || output == NULL ||
        ucn_i_ranges_overlap(envelope, sizeof(*envelope), output,
                             UCN_I_REALTIME_ENVELOPE_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = (uint8_t)((UCN_I_REALTIME_ENVELOPE_VERSION <<
                            VERSION_SHIFT) | envelope->mode);
    encoded[1] = (uint8_t)(envelope->uncertainty_class <<
                           UNCERTAINTY_SHIFT);
    if (envelope->sample_capture_hardware != 0U) {
        encoded[1] |= FLAG_CAPTURE_HW;
    }
    if (envelope->domain_time_valid != 0U) {
        encoded[1] |= FLAG_DOMAIN_VALID;
    }
    if (envelope->source_holdover != 0U) {
        encoded[1] |= FLAG_SOURCE_HOLDOVER;
    }
    write_be16(&encoded[2], envelope->clock_domain_id);
    write_be32(&encoded[4], envelope->domain_generation);
    write_be64(&encoded[8], envelope->capture_time_us);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_envelope_decode(
    const uint8_t *input, size_t input_length,
    ucn_i_realtime_envelope_t *envelope_out)
{
    ucn_i_realtime_envelope_t decoded;
    uint8_t version;

    if (input == NULL || envelope_out == NULL ||
        ucn_i_ranges_overlap(input, input_length, envelope_out,
                             sizeof(*envelope_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length != UCN_I_REALTIME_ENVELOPE_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    version = input[0] >> VERSION_SHIFT;
    if (version != UCN_I_REALTIME_ENVELOPE_VERSION) {
        return UCN_ERR_UNSUPPORTED;
    }
    if ((input[0] & UINT8_C(0x0C)) != 0U) {
        return UCN_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.mode = input[0] & MODE_MASK;
    decoded.uncertainty_class = input[1] >> UNCERTAINTY_SHIFT;
    decoded.sample_capture_hardware =
        (input[1] & FLAG_CAPTURE_HW) != 0U ? 1U : 0U;
    decoded.domain_time_valid =
        (input[1] & FLAG_DOMAIN_VALID) != 0U ? 1U : 0U;
    decoded.source_holdover =
        (input[1] & FLAG_SOURCE_HOLDOVER) != 0U ? 1U : 0U;
    decoded.clock_domain_id = read_be16(&input[2]);
    decoded.domain_generation = read_be32(&input[4]);
    decoded.capture_time_us = read_be64(&input[8]);
    if (!ucn_i_realtime_envelope_valid(&decoded)) {
        return UCN_ERR_MALFORMED;
    }
    *envelope_out = decoded;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_uncertainty_class_encode(
    bool known, uint64_t upper_bound_us, uint8_t *class_out)
{
    uint64_t limit = UINT64_C(1) << 30;
    uint64_t value;
    uint8_t value_class = 0U;

    if (class_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!known || upper_bound_us > limit) {
        *class_out = UCN_I_REALTIME_UNCERTAINTY_UNKNOWN;
        return UCN_OK;
    }
    value = upper_bound_us == 0U ? 1U : upper_bound_us;
    while ((UINT64_C(1) << value_class) < value) {
        ++value_class;
    }
    *class_out = value_class;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_uncertainty_class_decode(
    uint8_t uncertainty_class, bool *known_out,
    uint32_t *upper_bound_us_out)
{
    if (known_out == NULL || upper_bound_us_out == NULL ||
        uncertainty_class > UCN_I_REALTIME_UNCERTAINTY_UNKNOWN ||
        ucn_i_ranges_overlap(known_out, sizeof(*known_out),
                             upper_bound_us_out,
                             sizeof(*upper_bound_us_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (uncertainty_class == UCN_I_REALTIME_UNCERTAINTY_UNKNOWN) {
        *known_out = false;
        *upper_bound_us_out = 0U;
        return UCN_OK;
    }
    *known_out = true;
    *upper_bound_us_out = UINT32_C(1) << uncertainty_class;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_uncertainty_aggregate(
    const ucn_i_realtime_uncertainty_t *components,
    uint32_t *upper_bound_us_out)
{
    uint64_t sum;

    if (components == NULL || upper_bound_us_out == NULL ||
        components->known_mask != UCN_I_REALTIME_KNOWN_ALL ||
        components->reserved_zero[0] != 0U ||
        components->reserved_zero[1] != 0U ||
        components->reserved_zero[2] != 0U ||
        components->timer_resolution_bound_us == 0U ||
        components->link_timestamp_capture_bound_us == 0U ||
        components->filter_residual_bound_us == 0U ||
        components->arithmetic_rounding_bound_us == 0U ||
        components->sample_capture_bound_us == 0U ||
        components->path_asymmetry_bound_us == 0U ||
        ucn_i_ranges_overlap(components, sizeof(*components),
                             upper_bound_us_out,
                             sizeof(*upper_bound_us_out))) {
        return UCN_ERR_STATE;
    }
    sum = (uint64_t)components->timer_resolution_bound_us +
          components->link_timestamp_capture_bound_us +
          components->filter_residual_bound_us +
          components->arithmetic_rounding_bound_us +
          components->sample_capture_bound_us +
          components->path_asymmetry_bound_us;
    if (sum > UINT32_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *upper_bound_us_out = (uint32_t)sum;
    return UCN_OK;
}

static bool time_key_valid(const ucn_i_realtime_time_key_t *value)
{
    return value != NULL && value->clock_domain_id != 0U &&
           value->clock_domain_id <= UCN_I_REALTIME_DOMAIN_ID_MAX &&
           value->domain_generation != 0U &&
           value->domain_generation <=
               UCN_I_REALTIME_DOMAIN_GENERATION_MAX &&
           value->sync_sequence != 0U && value->reserved_zero == 0U;
}

static void time_key_write(const ucn_i_realtime_time_key_t *value,
                           uint8_t *output)
{
    write_be16(output, value->clock_domain_id);
    write_be16(output + 2U, 0U);
    write_be32(output + 4U, value->domain_generation);
    write_be32(output + 8U, value->sync_sequence);
}

static void time_key_read(const uint8_t *input,
                          ucn_i_realtime_time_key_t *value)
{
    value->clock_domain_id = read_be16(input);
    value->reserved_zero = read_be16(input + 2U);
    value->domain_generation = read_be32(input + 4U);
    value->sync_sequence = read_be32(input + 8U);
}

ucn_result_t ucn_i_realtime_time_key_encode(
    const ucn_i_realtime_time_key_t *value,
    uint8_t output[UCN_I_REALTIME_TIME_KEY_BYTES])
{
    uint8_t encoded[UCN_I_REALTIME_TIME_KEY_BYTES];
    if (!time_key_valid(value) || output == NULL ||
        ucn_i_ranges_overlap(value, sizeof(*value), output,
                             UCN_I_REALTIME_TIME_KEY_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    time_key_write(value, encoded);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_time_key_decode(
    const uint8_t input[UCN_I_REALTIME_TIME_KEY_BYTES],
    ucn_i_realtime_time_key_t *value_out)
{
    ucn_i_realtime_time_key_t value;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_REALTIME_TIME_KEY_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    time_key_read(input, &value);
    if (!time_key_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_time_follow_up_encode(
    const ucn_i_realtime_time_follow_up_t *value,
    uint8_t output[UCN_I_REALTIME_TIME_FOLLOW_UP_BYTES])
{
    uint8_t encoded[UCN_I_REALTIME_TIME_FOLLOW_UP_BYTES];
    if (value == NULL || output == NULL || !time_key_valid(&value->key) ||
        value->t1_master_tx_us == 0U ||
        value->t1_uncertainty_us == 0U ||
        ucn_i_ranges_overlap(value, sizeof(*value), output,
                             UCN_I_REALTIME_TIME_FOLLOW_UP_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    time_key_write(&value->key, encoded);
    write_be64(encoded + 12U, value->t1_master_tx_us);
    write_be32(encoded + 20U, value->t1_uncertainty_us);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_time_follow_up_decode(
    const uint8_t input[UCN_I_REALTIME_TIME_FOLLOW_UP_BYTES],
    ucn_i_realtime_time_follow_up_t *value_out)
{
    ucn_i_realtime_time_follow_up_t value;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_REALTIME_TIME_FOLLOW_UP_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    time_key_read(input, &value.key);
    value.t1_master_tx_us = read_be64(input + 12U);
    value.t1_uncertainty_us = read_be32(input + 20U);
    if (!time_key_valid(&value.key) || value.t1_master_tx_us == 0U ||
        value.t1_uncertainty_us == 0U) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_time_delay_response_encode(
    const ucn_i_realtime_time_delay_response_t *value,
    uint8_t output[UCN_I_REALTIME_TIME_DELAY_RESPONSE_BYTES])
{
    uint8_t encoded[UCN_I_REALTIME_TIME_DELAY_RESPONSE_BYTES];
    if (value == NULL || output == NULL || !time_key_valid(&value->key) ||
        value->t4_master_rx_us == 0U ||
        value->t4_uncertainty_us == 0U ||
        ucn_i_ranges_overlap(value, sizeof(*value), output,
                             UCN_I_REALTIME_TIME_DELAY_RESPONSE_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    time_key_write(&value->key, encoded);
    write_be64(encoded + 12U, value->t4_master_rx_us);
    write_be32(encoded + 20U, value->t4_uncertainty_us);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_time_delay_response_decode(
    const uint8_t input[UCN_I_REALTIME_TIME_DELAY_RESPONSE_BYTES],
    ucn_i_realtime_time_delay_response_t *value_out)
{
    ucn_i_realtime_time_delay_response_t value;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input,
                             UCN_I_REALTIME_TIME_DELAY_RESPONSE_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    time_key_read(input, &value.key);
    value.t4_master_rx_us = read_be64(input + 12U);
    value.t4_uncertainty_us = read_be32(input + 20U);
    if (!time_key_valid(&value.key) || value.t4_master_rx_us == 0U ||
        value.t4_uncertainty_us == 0U) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_time_abort_encode(
    const ucn_i_realtime_time_abort_t *value,
    uint8_t output[UCN_I_REALTIME_TIME_SYNC_ABORT_BYTES])
{
    uint8_t encoded[UCN_I_REALTIME_TIME_SYNC_ABORT_BYTES] = {0};
    if (value == NULL || output == NULL || !time_key_valid(&value->key) ||
        value->reason < UCN_I_REALTIME_ABORT_POLICY ||
        value->reason > UCN_I_REALTIME_ABORT_CONFLICT ||
        value->reserved_zero[0] != 0U || value->reserved_zero[1] != 0U ||
        value->reserved_zero[2] != 0U ||
        ucn_i_ranges_overlap(value, sizeof(*value), output,
                             UCN_I_REALTIME_TIME_SYNC_ABORT_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    time_key_write(&value->key, encoded);
    encoded[12] = value->reason;
    memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_time_abort_decode(
    const uint8_t input[UCN_I_REALTIME_TIME_SYNC_ABORT_BYTES],
    ucn_i_realtime_time_abort_t *value_out)
{
    ucn_i_realtime_time_abort_t value;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_REALTIME_TIME_SYNC_ABORT_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    time_key_read(input, &value.key);
    value.reason = input[12];
    memcpy(value.reserved_zero, input + 13U,
           sizeof(value.reserved_zero));
    if (!time_key_valid(&value.key) ||
        value.reason < UCN_I_REALTIME_ABORT_POLICY ||
        value.reason > UCN_I_REALTIME_ABORT_CONFLICT ||
        value.reserved_zero[0] != 0U || value.reserved_zero[1] != 0U ||
        value.reserved_zero[2] != 0U) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}
