#include "internal/ucn_transport.h"

#include "internal/ucn_checked.h"

#include <string.h>

static void put16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (uint8_t)(value >> 8U);
    bytes[offset + 1U] = (uint8_t)value;
}

static void put32(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (uint8_t)(value >> 24U);
    bytes[offset + 1U] = (uint8_t)(value >> 16U);
    bytes[offset + 2U] = (uint8_t)(value >> 8U);
    bytes[offset + 3U] = (uint8_t)value;
}

static void put64(uint8_t *bytes, size_t offset, uint64_t value)
{
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[offset + index] =
            (uint8_t)(value >> (uint8_t)((7U - index) * 8U));
    }
}

static uint16_t get16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t)(((uint16_t)bytes[offset] << 8U) |
                      bytes[offset + 1U]);
}

static uint32_t get32(const uint8_t *bytes, size_t offset)
{
    return ((uint32_t)bytes[offset] << 24U) |
           ((uint32_t)bytes[offset + 1U] << 16U) |
           ((uint32_t)bytes[offset + 2U] << 8U) | bytes[offset + 3U];
}

static uint64_t get64(const uint8_t *bytes, size_t offset)
{
    uint64_t value = 0U;
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

static size_t setup_size(uint8_t interaction)
{
    if (interaction == UCN_INTERACTION_ONE_WAY) {
        return UCN_I_TRANSPORT_TRANSFER_SETUP_ONE_WAY_BYTES;
    }
    if (interaction == UCN_INTERACTION_REQUEST) {
        return UCN_I_TRANSPORT_TRANSFER_SETUP_OPERATION_BYTES;
    }
    if (interaction == UCN_INTERACTION_RESULT ||
        interaction == UCN_INTERACTION_ERROR) {
        return UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES;
    }
    return 0U;
}

static bool setup_valid(const ucn_i_transport_transfer_setup_t *value)
{
    size_t expected;
    uint64_t described_bytes;

    if (value == NULL ||
        (value->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         value->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_FLOW) ||
        value->parent_generation == 0U || value->transfer_id == 0U ||
        value->service_id == 0U || value->total_length == 0U ||
        value->delivery > UCN_DELIVERY_RELIABLE ||
        value->interaction > UCN_INTERACTION_ERROR ||
        value->fragment_count == 0U ||
        value->fragment_count > UCN_I_TRANSPORT_MAX_FRAGMENT_COUNT ||
        value->fragment_budget == 0U || value->lifetime_ms == 0U) {
        return false;
    }
    if ((value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         value->parent_id != 0U) ||
        (value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_FLOW &&
         value->parent_id == 0U)) {
        return false;
    }
    expected = setup_size(value->interaction);
    if (expected == 0U ||
        (value->interaction == UCN_INTERACTION_ONE_WAY &&
         (value->operation_id != 0U || value->operation_flags != 0U ||
          value->result_code != 0U)) ||
        (value->interaction != UCN_INTERACTION_ONE_WAY &&
         value->operation_id == 0U) ||
        (value->interaction == UCN_INTERACTION_REQUEST &&
         value->result_code != 0U)) {
        return false;
    }
    described_bytes = (uint64_t)value->fragment_count *
                      value->fragment_budget;
    return described_bytes >= value->total_length &&
           described_bytes - value->fragment_budget < value->total_length;
}

ucn_result_t ucn_i_transport_delivery_ack_encode(
    const ucn_i_transport_delivery_ack_t *value,
    uint8_t output[UCN_I_TRANSPORT_DELIVERY_ACK_BYTES])
{
    uint8_t bytes[UCN_I_TRANSPORT_DELIVERY_ACK_BYTES];

    if (value == NULL || output == NULL || value->service_id == 0U ||
        value->origin_sequence == 0U ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, sizeof(bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    bytes[0] = UCN_I_TRANSPORT_ACK_SUBTYPE;
    put16(bytes, 1U, value->service_id);
    put32(bytes, 3U, value->origin_sequence);
    bytes[7] = value->status;
    bytes[8] = value->receive_credit;
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_transport_delivery_ack_decode(
    const uint8_t input[UCN_I_TRANSPORT_DELIVERY_ACK_BYTES],
    ucn_i_transport_delivery_ack_t *value_out)
{
    ucn_i_transport_delivery_ack_t value;

    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_TRANSPORT_DELIVERY_ACK_BYTES,
                             value_out, sizeof(*value_out)) ||
        input[0] != UCN_I_TRANSPORT_ACK_SUBTYPE) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    value.service_id = get16(input, 1U);
    value.origin_sequence = get32(input, 3U);
    value.status = input[7];
    value.receive_credit = input[8];
    if (value.service_id == 0U || value.origin_sequence == 0U) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_setup_encode(
    const ucn_i_transport_transfer_setup_t *value,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    uint8_t bytes[UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES];
    size_t length;

    if (!setup_valid(value) || output == NULL || output_bytes == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    length = setup_size(value->interaction);
    if (output_capacity < length ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, length) ||
        ucn_i_ranges_overlap(value, sizeof(*value), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output_bytes, sizeof(*output_bytes), output,
                             length)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(bytes, 0, sizeof(bytes));
    bytes[0] = value->parent_kind;
    put16(bytes, 1U, value->parent_id);
    put32(bytes, 3U, value->parent_generation);
    put32(bytes, 7U, value->transfer_id);
    put16(bytes, 11U, value->service_id);
    put32(bytes, 13U, value->total_length);
    memcpy(&bytes[17], value->message_digest, 16U);
    bytes[33] = value->delivery;
    bytes[34] = value->interaction;
    put16(bytes, 35U, value->fragment_count);
    put16(bytes, 37U, value->fragment_budget);
    put32(bytes, 39U, value->lifetime_ms);
    if (length > UCN_I_TRANSPORT_TRANSFER_SETUP_ONE_WAY_BYTES) {
        put64(bytes, 43U, value->operation_id);
        bytes[51] = value->operation_flags;
    }
    if (length == UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES) {
        put16(bytes, 52U, value->result_code);
    }
    memcpy(output, bytes, length);
    *output_bytes = length;
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_setup_decode(
    const uint8_t *input,
    size_t input_bytes,
    ucn_i_transport_transfer_setup_t *value_out)
{
    ucn_i_transport_transfer_setup_t value;
    size_t expected;

    if (input == NULL || value_out == NULL || input_bytes < 35U ||
        ucn_i_ranges_overlap(input, input_bytes, value_out,
                             sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    expected = setup_size(input[34]);
    if (expected == 0U || input_bytes != expected) {
        return UCN_ERR_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.parent_kind = input[0];
    value.parent_id = get16(input, 1U);
    value.parent_generation = get32(input, 3U);
    value.transfer_id = get32(input, 7U);
    value.service_id = get16(input, 11U);
    value.total_length = get32(input, 13U);
    memcpy(value.message_digest, &input[17], 16U);
    value.delivery = input[33];
    value.interaction = input[34];
    value.fragment_count = get16(input, 35U);
    value.fragment_budget = get16(input, 37U);
    value.lifetime_ms = get32(input, 39U);
    if (expected > UCN_I_TRANSPORT_TRANSFER_SETUP_ONE_WAY_BYTES) {
        value.operation_id = get64(input, 43U);
        value.operation_flags = input[51];
    }
    if (expected == UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES) {
        value.result_code = get16(input, 52U);
    }
    if (!setup_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_transport_fragment_prefix_encode(
    const ucn_i_transport_fragment_prefix_t *value,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    size_t length;

    if (value == NULL || output == NULL || output_bytes == NULL ||
        value->transfer_id == 0U || value->fragment_count == 0U ||
        value->fragment_count > UCN_I_TRANSPORT_MAX_FRAGMENT_COUNT ||
        value->fragment_index >= value->fragment_count ||
        (value->fragment_index & UCN_I_TRANSPORT_FRAGMENT_KIND_MASK) != 0U ||
        (value->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         value->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_FLOW) ||
        (value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         value->parent_generation == 0U) ||
        (value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_FLOW &&
         value->parent_generation != 0U) ||
        value->reserved_zero[0] != 0U || value->reserved_zero[1] != 0U ||
        value->reserved_zero[2] != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    length = value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
                 UCN_I_TRANSPORT_C1_FRAGMENT_PREFIX_BYTES :
                 UCN_I_TRANSPORT_FLOW_FRAGMENT_PREFIX_BYTES;
    if (output_capacity < length ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, length) ||
        ucn_i_ranges_overlap(value, sizeof(*value), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output_bytes, sizeof(*output_bytes), output,
                             length)) {
        return UCN_ERR_ARGUMENT;
    }
    if (value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1) {
        uint8_t bytes[UCN_I_TRANSPORT_C1_FRAGMENT_PREFIX_BYTES] = {0};
        put32(bytes, 0U, value->transfer_id);
        put32(bytes, 4U, value->parent_generation);
        put16(bytes, 8U, value->fragment_index);
        put16(bytes, 10U, value->fragment_count);
        memcpy(output, bytes, sizeof(bytes));
    } else {
        uint8_t bytes[UCN_I_TRANSPORT_FLOW_FRAGMENT_PREFIX_BYTES] = {0};
        put32(bytes, 0U, value->transfer_id);
        put16(bytes, 4U, value->fragment_index);
        put16(bytes, 6U, value->fragment_count);
        memcpy(output, bytes, sizeof(bytes));
    }
    *output_bytes = length;
    return UCN_OK;
}

ucn_result_t ucn_i_transport_fragment_prefix_decode(
    const uint8_t *input,
    size_t input_bytes,
    uint8_t parent_kind,
    ucn_i_transport_fragment_prefix_t *value_out)
{
    ucn_i_transport_fragment_prefix_t value;
    size_t expected = parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
                          UCN_I_TRANSPORT_C1_FRAGMENT_PREFIX_BYTES :
                          UCN_I_TRANSPORT_FLOW_FRAGMENT_PREFIX_BYTES;

    if (input == NULL || value_out == NULL ||
        (parent_kind != UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         parent_kind != UCN_I_TRANSPORT_PARENT_KIND_FLOW) ||
        input_bytes != expected ||
        ucn_i_ranges_overlap(input, input_bytes, value_out,
                             sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    value.parent_kind = parent_kind;
    value.transfer_id = get32(input, 0U);
    if (parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1) {
        value.parent_generation = get32(input, 4U);
        value.fragment_index = get16(input, 8U);
        value.fragment_count = get16(input, 10U);
    } else {
        value.fragment_index = get16(input, 4U);
        value.fragment_count = get16(input, 6U);
    }
    if (value.transfer_id == 0U || value.fragment_count == 0U ||
        value.fragment_count > UCN_I_TRANSPORT_MAX_FRAGMENT_COUNT ||
        value.fragment_index >= value.fragment_count ||
        (value.fragment_index & UCN_I_TRANSPORT_FRAGMENT_KIND_MASK) != 0U ||
        (parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         value.parent_generation == 0U)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_transport_sack_encode(
    const ucn_i_transport_sack_t *value,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    size_t length;

    if (value == NULL || output == NULL || output_bytes == NULL ||
        value->transfer_id == 0U ||
        (value->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         value->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_FLOW) ||
        (value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         (value->parent_generation == 0U ||
          value->original_service_id == 0U)) ||
        (value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_FLOW &&
         (value->parent_generation != 0U ||
          value->original_service_id != 0U)) ||
        value->reserved_zero[0] != 0U || value->reserved_zero[1] != 0U ||
        value->reserved_zero[2] != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    length = value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
                 UCN_I_TRANSPORT_C1_SACK_BYTES :
                 UCN_I_TRANSPORT_FLOW_SACK_BYTES;
    if (output_capacity < length ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, length) ||
        ucn_i_ranges_overlap(value, sizeof(*value), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output_bytes, sizeof(*output_bytes), output,
                             length)) {
        return UCN_ERR_ARGUMENT;
    }
    if (value->parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1) {
        uint8_t bytes[UCN_I_TRANSPORT_C1_SACK_BYTES] = {0};
        put32(bytes, 0U, value->transfer_id);
        put32(bytes, 4U, value->parent_generation);
        put16(bytes, 8U, UCN_I_TRANSPORT_SACK_KIND);
        put16(bytes, 10U, value->original_service_id);
        put16(bytes, 12U, value->window_base);
        put32(bytes, 14U, value->bitmap);
        put16(bytes, 18U, value->receive_credit);
        memcpy(output, bytes, sizeof(bytes));
    } else {
        uint8_t bytes[UCN_I_TRANSPORT_FLOW_SACK_BYTES] = {0};
        put32(bytes, 0U, value->transfer_id);
        put16(bytes, 4U, UCN_I_TRANSPORT_SACK_KIND);
        put16(bytes, 6U, value->window_base);
        put32(bytes, 8U, value->bitmap);
        put16(bytes, 12U, value->receive_credit);
        memcpy(output, bytes, sizeof(bytes));
    }
    *output_bytes = length;
    return UCN_OK;
}

ucn_result_t ucn_i_transport_sack_decode(
    const uint8_t *input,
    size_t input_bytes,
    uint8_t parent_kind,
    ucn_i_transport_sack_t *value_out)
{
    ucn_i_transport_sack_t value;
    size_t expected = parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
                          UCN_I_TRANSPORT_C1_SACK_BYTES :
                          UCN_I_TRANSPORT_FLOW_SACK_BYTES;

    if (input == NULL || value_out == NULL ||
        (parent_kind != UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         parent_kind != UCN_I_TRANSPORT_PARENT_KIND_FLOW) ||
        input_bytes != expected ||
        ucn_i_ranges_overlap(input, input_bytes, value_out,
                             sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    value.parent_kind = parent_kind;
    value.transfer_id = get32(input, 0U);
    if (parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1) {
        value.parent_generation = get32(input, 4U);
        if (get16(input, 8U) != UCN_I_TRANSPORT_SACK_KIND) {
            return UCN_ERR_MALFORMED;
        }
        value.original_service_id = get16(input, 10U);
        value.window_base = get16(input, 12U);
        value.bitmap = get32(input, 14U);
        value.receive_credit = get16(input, 18U);
    } else {
        if (get16(input, 4U) != UCN_I_TRANSPORT_SACK_KIND) {
            return UCN_ERR_MALFORMED;
        }
        value.window_base = get16(input, 6U);
        value.bitmap = get32(input, 8U);
        value.receive_credit = get16(input, 12U);
    }
    if (value.transfer_id == 0U ||
        (parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 &&
         (value.parent_generation == 0U ||
          value.original_service_id == 0U))) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}
