#include "internal/ucn_wire.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_PROTOCOL_VERSION 6U
#define UCN_I_CONTRACT_C1 1U

static uint32_t address_limit(uint8_t address_width)
{
    if (address_width == 4U) {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << (address_width * 8U)) - UINT32_C(1);
}

static bool address_is_valid(uint32_t address, uint8_t address_width)
{
    uint32_t limit;

    if (address_width == 0U || address_width > 4U) {
        return false;
    }
    limit = address_limit(address_width);
    return address != 0U && address != limit && address < limit;
}

static bool frame_is_valid(const ucn_i_c1_frame_t *frame,
                           uint8_t address_width)
{
    return frame != NULL &&
           (frame->payload != NULL || frame->payload_bytes == 0U) &&
           address_is_valid(frame->source_address, address_width) &&
           address_is_valid(frame->destination_address, address_width) &&
           frame->origin_sequence != 0U && frame->service_id != 0U &&
           frame->service_id != UINT16_MAX &&
           frame->traffic_class < UCN_TRAFFIC_CLASS_COUNT &&
           frame->hop_limit != 0U && frame->hop_limit <= 63U;
}

static void write_be(uint8_t *output, uint32_t value, uint8_t bytes)
{
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        uint8_t shift_bytes = (uint8_t)(bytes - index - 1U);

        output[index] = (uint8_t)(value >> (shift_bytes * 8U));
    }
}

static uint32_t read_be(const uint8_t *input, uint8_t bytes)
{
    uint32_t value = 0U;
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        value = (value << 8U) | (uint32_t)input[index];
    }
    return value;
}

static void write_header_and_fields(const ucn_i_c1_frame_t *frame,
                                    uint8_t address_width,
                                    uint8_t *output)
{
    size_t offset = UCN_I_C1_COMMON_BYTES;

    output[0] = (uint8_t)((UCN_I_PROTOCOL_VERSION << 4U) | UCN_I_CONTRACT_C1);
    output[1] = (uint8_t)(frame->traffic_class << 6U);
    output[2] = frame->hop_limit;
    write_be(output + offset, frame->source_address, address_width);
    offset += address_width;
    write_be(output + offset, frame->destination_address, address_width);
    offset += address_width;
    write_be(output + offset, frame->service_id, 2U);
    offset += 2U;
    write_be(output + offset, frame->origin_sequence, 4U);
}

ucn_result_t ucn_i_c1_header_bytes(uint8_t address_width,
                                   size_t *header_bytes_out)
{
    size_t header_bytes;

    if (header_bytes_out == NULL || address_width == 0U || address_width > 4U) {
        return UCN_ERR_ARGUMENT;
    }
    header_bytes = UCN_I_C1_COMMON_BYTES + (2U * address_width) +
                   UCN_I_C1_FIXED_AFTER_ADDRESSES;
    *header_bytes_out = header_bytes;
    return UCN_OK;
}

ucn_result_t ucn_i_c1_encoded_size(uint8_t address_width,
                                   size_t payload_bytes,
                                   size_t *encoded_bytes_out)
{
    size_t header_bytes;
    size_t encoded_bytes;
    ucn_result_t result;

    if (encoded_bytes_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_c1_header_bytes(address_width, &header_bytes);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_size_add(header_bytes, payload_bytes, &encoded_bytes);
    if (result != UCN_OK) {
        return result;
    }
    *encoded_bytes_out = encoded_bytes;
    return UCN_OK;
}

ucn_result_t ucn_i_c1_encode(const ucn_i_c1_frame_t *frame,
                             uint8_t address_width,
                             uint8_t *output,
                             size_t output_capacity,
                             size_t *output_bytes)
{
    size_t header_bytes;
    size_t encoded_bytes;
    ucn_result_t result;

    if (!frame_is_valid(frame, address_width) || output == NULL ||
        output_bytes == NULL ||
        ucn_i_ranges_overlap(frame, sizeof(*frame), output, output_capacity) ||
        ucn_i_ranges_overlap(frame, sizeof(*frame), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                             sizeof(*output_bytes)) ||
        (frame->payload_bytes != 0U &&
         (ucn_i_ranges_overlap(frame->payload, frame->payload_bytes,
                               output, output_capacity) ||
          ucn_i_ranges_overlap(frame->payload, frame->payload_bytes,
                               output_bytes, sizeof(*output_bytes))))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_c1_header_bytes(address_width, &header_bytes);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_c1_encoded_size(address_width, frame->payload_bytes,
                                   &encoded_bytes);
    if (result != UCN_OK || encoded_bytes > output_capacity) {
        return result == UCN_OK ? UCN_ERR_NO_SPACE : result;
    }
    write_header_and_fields(frame, address_width, output);
    if (frame->payload_bytes != 0U) {
        memcpy(output + header_bytes, frame->payload, frame->payload_bytes);
    }
    *output_bytes = encoded_bytes;
    return UCN_OK;
}

ucn_result_t ucn_i_c1_encode_in_place(ucn_i_c1_frame_t *frame,
                                      uint8_t address_width,
                                      uint8_t *payload_then_frame,
                                      size_t capacity,
                                      size_t *output_bytes)
{
    size_t header_bytes;
    size_t encoded_bytes;
    ucn_result_t result;

    if (frame == NULL || payload_then_frame == NULL || output_bytes == NULL ||
        frame->payload != payload_then_frame ||
        !frame_is_valid(frame, address_width) ||
        ucn_i_ranges_overlap(frame, sizeof(*frame), payload_then_frame,
                             capacity) ||
        ucn_i_ranges_overlap(frame, sizeof(*frame), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(payload_then_frame, capacity, output_bytes,
                             sizeof(*output_bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_c1_header_bytes(address_width, &header_bytes);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_c1_encoded_size(address_width, frame->payload_bytes,
                                   &encoded_bytes);
    if (result != UCN_OK || encoded_bytes > capacity) {
        return result == UCN_OK ? UCN_ERR_NO_SPACE : result;
    }
    if (frame->payload_bytes != 0U) {
        memmove(payload_then_frame + header_bytes, payload_then_frame,
                frame->payload_bytes);
    }
    write_header_and_fields(frame, address_width, payload_then_frame);
    frame->payload = payload_then_frame + header_bytes;
    *output_bytes = encoded_bytes;
    return UCN_OK;
}

ucn_result_t ucn_i_c1_decode(const uint8_t *input,
                             size_t input_bytes,
                             uint8_t address_width,
                             ucn_i_c1_frame_t *frame_out)
{
    ucn_i_c1_frame_t decoded;
    size_t header_bytes;
    size_t offset;
    uint8_t version;
    uint8_t contract;
    uint8_t delivery;
    uint8_t interaction;
    uint8_t payload_kind;
    uint8_t origin_security;
    ucn_result_t result;

    if (input == NULL || frame_out == NULL ||
        ucn_i_ranges_overlap(input, input_bytes, frame_out,
                             sizeof(*frame_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_c1_header_bytes(address_width, &header_bytes);
    if (result != UCN_OK) {
        return result;
    }
    if (input_bytes < header_bytes) {
        return UCN_ERR_MALFORMED;
    }
    version = (uint8_t)(input[0] >> 4U);
    contract = (uint8_t)(input[0] & 0x0FU);
    delivery = (uint8_t)((input[1] >> 4U) & 0x03U);
    interaction = (uint8_t)((input[1] >> 2U) & 0x03U);
    payload_kind = (uint8_t)(input[1] & 0x03U);
    origin_security = (uint8_t)(input[2] >> 6U);
    if (version != UCN_I_PROTOCOL_VERSION || contract > 5U ||
        delivery == 3U || origin_security == 3U ||
        (input[2] & 0x3FU) == 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (contract != UCN_I_CONTRACT_C1 || delivery != UCN_DELIVERY_BEST_EFFORT ||
        interaction != UCN_INTERACTION_ONE_WAY || payload_kind != 0U ||
        origin_security != 0U) {
        return UCN_ERR_UNSUPPORTED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.traffic_class = (uint8_t)(input[1] >> 6U);
    decoded.hop_limit = (uint8_t)(input[2] & 0x3FU);
    offset = UCN_I_C1_COMMON_BYTES;
    decoded.source_address = read_be(input + offset, address_width);
    offset += address_width;
    decoded.destination_address = read_be(input + offset, address_width);
    offset += address_width;
    decoded.service_id = (uint16_t)read_be(input + offset, 2U);
    offset += 2U;
    decoded.origin_sequence = read_be(input + offset, 4U);
    if (!address_is_valid(decoded.source_address, address_width) ||
        !address_is_valid(decoded.destination_address, address_width) ||
        decoded.service_id == 0U || decoded.service_id == UINT16_MAX ||
        decoded.origin_sequence == 0U) {
        return UCN_ERR_MALFORMED;
    }
    decoded.payload = input + header_bytes;
    decoded.payload_bytes = input_bytes - header_bytes;
    *frame_out = decoded;
    return UCN_OK;
}
