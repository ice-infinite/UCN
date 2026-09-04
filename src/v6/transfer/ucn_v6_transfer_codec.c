#include "ucn/v6/ucn_v6_transfer.h"

#include <string.h>

static void write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void write_u64(uint8_t *output, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static uint16_t read_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | input[3];
}

static uint64_t read_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

size_t ucn_v6_message_class_bytes(ucn_v6_message_class_t message_class)
{
    static const size_t sizes[9] = {
        32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U, 8192U
    };
    return (uint32_t)message_class < 9U ? sizes[(size_t)message_class] : 0U;
}

static bool fragment_is_valid(const ucn_v6_transfer_fragment_t *fragment)
{
    size_t class_bytes;
    uint32_t expected_count;
    uint32_t expected_length;
    if (fragment == NULL || fragment->message_id == 0U ||
        fragment->message_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        fragment->total_length == 0U ||
        fragment->total_length > UCN_V6_TRANSFER_MAX_MESSAGE_BYTES ||
        fragment->fragment_data_budget == 0U ||
        fragment->fragment_count == 0U || fragment->data_length == 0U ||
        fragment->data == NULL) {
        return false;
    }
    class_bytes = ucn_v6_message_class_bytes(fragment->message_class);
    if (class_bytes == 0U ||
        (uint32_t)fragment->message_class >
            UCN_V6_CONFIG_TRANSFER_MAX_CLASS ||
        fragment->total_length > class_bytes) {
        return false;
    }
    expected_count =
        ((uint32_t)fragment->total_length +
         fragment->fragment_data_budget - 1U) /
        fragment->fragment_data_budget;
    if (expected_count == 0U || expected_count > UINT16_MAX ||
        fragment->fragment_count != expected_count ||
        fragment->fragment_index >= fragment->fragment_count) {
        return false;
    }
    expected_length = fragment->fragment_index + 1U <
                              fragment->fragment_count ?
                          fragment->fragment_data_budget :
                          (uint32_t)fragment->total_length -
                              (uint32_t)fragment->fragment_index *
                                  fragment->fragment_data_budget;
    return fragment->data_length == expected_length;
}

ucn_v6_result_t ucn_v6_transfer_fragment_encode(
    const ucn_v6_transfer_fragment_t *fragment,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    size_t required;
    if (!fragment_is_valid(fragment) || output == NULL ||
        output_length == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    required = UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES + fragment->data_length;
    if (output_capacity < required) {
        return UCN_V6_ERR_NO_SPACE;
    }
    output[0] = UCN_V6_TRANSFER_PAYLOAD_FRAGMENT;
    output[1] = (uint8_t)fragment->message_class;
    output[2] = 0U;
    output[3] = 0U;
    write_u64(&output[4], fragment->message_id);
    write_u16(&output[12], fragment->total_length);
    write_u16(&output[14], fragment->fragment_index);
    write_u16(&output[16], fragment->fragment_count);
    write_u16(&output[18], fragment->fragment_data_budget);
    write_u32(&output[20], fragment->message_crc32c);
    memcpy(&output[UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES], fragment->data,
           fragment->data_length);
    *output_length = required;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_fragment_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_transfer_fragment_t *fragment_out)
{
    ucn_v6_transfer_fragment_t fragment;
    if (input == NULL || fragment_out == NULL ||
        input_length <= UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES ||
        input_length > UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES +
                           UCN_V6_TRANSFER_MAX_MESSAGE_BYTES ||
        input[0] != UCN_V6_TRANSFER_PAYLOAD_FRAGMENT || input[2] != 0U ||
        input[3] != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&fragment, 0, sizeof(fragment));
    fragment.message_class = (ucn_v6_message_class_t)input[1];
    fragment.message_id = read_u64(&input[4]);
    fragment.total_length = read_u16(&input[12]);
    fragment.fragment_index = read_u16(&input[14]);
    fragment.fragment_count = read_u16(&input[16]);
    fragment.fragment_data_budget = read_u16(&input[18]);
    fragment.message_crc32c = read_u32(&input[20]);
    fragment.data_length =
        (uint16_t)(input_length - UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES);
    fragment.data = &input[UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES];
    if (!fragment_is_valid(&fragment)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *fragment_out = fragment;
    return UCN_V6_OK;
}

static bool sack_is_valid(const ucn_v6_transfer_sack_t *sack)
{
    uint16_t remaining;
    uint32_t allowed;
    if (sack == NULL || sack->message_id == 0U ||
        sack->message_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        sack->fragment_count == 0U ||
        sack->cumulative_base > sack->fragment_count) {
        return false;
    }
    remaining = (uint16_t)(sack->fragment_count - sack->cumulative_base);
    allowed = remaining >= 32U ? UINT32_MAX :
              remaining == 0U ? 0U :
              (UINT32_C(1) << remaining) - 1U;
    return (sack->received_bitmap & ~allowed) == 0U;
}

ucn_v6_result_t ucn_v6_transfer_sack_encode(
    const ucn_v6_transfer_sack_t *sack,
    uint8_t output[UCN_V6_TRANSFER_SACK_BYTES])
{
    uint8_t encoded[UCN_V6_TRANSFER_SACK_BYTES];
    if (!sack_is_valid(sack) || output == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = UCN_V6_TRANSFER_PAYLOAD_SACK;
    write_u64(&encoded[4], sack->message_id);
    write_u16(&encoded[12], sack->cumulative_base);
    write_u16(&encoded[14], sack->fragment_count);
    write_u32(&encoded[16], sack->received_bitmap);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_sack_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_transfer_sack_t *sack_out)
{
    ucn_v6_transfer_sack_t sack;
    if (input == NULL || sack_out == NULL ||
        input_length != UCN_V6_TRANSFER_SACK_BYTES ||
        input[0] != UCN_V6_TRANSFER_PAYLOAD_SACK || input[1] != 0U ||
        input[2] != 0U || input[3] != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&sack, 0, sizeof(sack));
    sack.message_id = read_u64(&input[4]);
    sack.cumulative_base = read_u16(&input[12]);
    sack.fragment_count = read_u16(&input[14]);
    sack.received_bitmap = read_u32(&input[16]);
    if (!sack_is_valid(&sack)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *sack_out = sack;
    return UCN_V6_OK;
}

static bool credit_is_valid(const ucn_v6_transfer_credit_update_t *credit)
{
    return credit != NULL && credit->link_id != 0U &&
           credit->link_generation != 0U &&
           credit->link_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           (credit->traffic_class == UCN_V6_TRAFFIC_Q2 ||
            credit->traffic_class == UCN_V6_TRAFFIC_Q3) &&
           credit->credit_generation != 0U &&
           credit->credit_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           credit->update_sequence != 0U &&
           credit->update_sequence <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           credit->maximum_credit != 0U &&
           credit->available_credit <= credit->maximum_credit &&
           credit->lease_duration_us != 0U;
}

ucn_v6_result_t ucn_v6_transfer_credit_encode(
    const ucn_v6_transfer_credit_update_t *credit,
    uint8_t output[UCN_V6_TRANSFER_CREDIT_BYTES])
{
    uint8_t encoded[UCN_V6_TRANSFER_CREDIT_BYTES];
    if (!credit_is_valid(credit) || output == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = UCN_V6_TRANSFER_PAYLOAD_CREDIT;
    encoded[1] = (uint8_t)credit->traffic_class;
    write_u16(&encoded[2], credit->link_id);
    write_u32(&encoded[4], credit->link_generation);
    write_u32(&encoded[8], credit->credit_generation);
    write_u32(&encoded[12], credit->update_sequence);
    write_u16(&encoded[16], credit->available_credit);
    write_u16(&encoded[18], credit->maximum_credit);
    write_u64(&encoded[20], credit->lease_duration_us);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_credit_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_transfer_credit_update_t *credit_out)
{
    ucn_v6_transfer_credit_update_t credit;
    if (input == NULL || credit_out == NULL ||
        input_length != UCN_V6_TRANSFER_CREDIT_BYTES ||
        input[0] != UCN_V6_TRANSFER_PAYLOAD_CREDIT || input[28] != 0U ||
        input[29] != 0U || input[30] != 0U || input[31] != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&credit, 0, sizeof(credit));
    credit.traffic_class = (ucn_v6_traffic_class_t)input[1];
    credit.link_id = read_u16(&input[2]);
    credit.link_generation = read_u32(&input[4]);
    credit.credit_generation = read_u32(&input[8]);
    credit.update_sequence = read_u32(&input[12]);
    credit.available_credit = read_u16(&input[16]);
    credit.maximum_credit = read_u16(&input[18]);
    credit.lease_duration_us = read_u64(&input[20]);
    if (!credit_is_valid(&credit)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *credit_out = credit;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_result_encode(
    const ucn_v6_transfer_result_t *result,
    uint8_t output[UCN_V6_TRANSFER_RESULT_BYTES])
{
    uint8_t encoded[UCN_V6_TRANSFER_RESULT_BYTES];
    if (result == NULL || output == NULL || result->message_id == 0U ||
        result->message_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        result->operation_id == 0U ||
        result->operation_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        result->message_id != result->operation_id) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = UCN_V6_TRANSFER_PAYLOAD_RESULT;
    write_u64(&encoded[4], result->message_id);
    write_u64(&encoded[12], result->operation_id);
    write_u32(&encoded[20], (uint32_t)result->result_code);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_result_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_transfer_result_t *result_out)
{
    ucn_v6_transfer_result_t result;
    if (input == NULL || result_out == NULL ||
        input_length != UCN_V6_TRANSFER_RESULT_BYTES ||
        input[0] != UCN_V6_TRANSFER_PAYLOAD_RESULT || input[1] != 0U ||
        input[2] != 0U || input[3] != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&result, 0, sizeof(result));
    result.message_id = read_u64(&input[4]);
    result.operation_id = read_u64(&input[12]);
    result.result_code = (int32_t)read_u32(&input[20]);
    if (result.message_id == 0U ||
        result.message_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        result.operation_id == 0U ||
        result.operation_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        result.message_id != result.operation_id) {
        return UCN_V6_ERR_MALFORMED;
    }
    *result_out = result;
    return UCN_V6_OK;
}
