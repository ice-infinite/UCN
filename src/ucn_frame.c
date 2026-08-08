#include <string.h>

#include "ucn/ucn_frame.h"

enum {
    UCN_OFFSET_MAGIC_0 = 0,
    UCN_OFFSET_MAGIC_1 = 1,
    UCN_OFFSET_VERSION = 2,
    UCN_OFFSET_MESSAGE_TYPE = 3,
    UCN_OFFSET_TRAFFIC_CLASS = 4,
    UCN_OFFSET_FLAGS = 5,
    UCN_OFFSET_HOP_LIMIT = 6,
    UCN_OFFSET_HEADER_SIZE = 7,
    UCN_OFFSET_NETWORK_ID = 8,
    UCN_OFFSET_SOURCE = 12,
    UCN_OFFSET_DESTINATION = 16,
    UCN_OFFSET_SEQUENCE = 20,
    UCN_OFFSET_SESSION_ID = 24,
    UCN_OFFSET_PAYLOAD_LENGTH = 28,
    UCN_OFFSET_CRC = 30
};

#define UCN_E2E_AAD_BYTES ((size_t)26U)

static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) |
           (uint32_t)input[3];
}

size_t ucn_frame_e2e_aad_size(void)
{
    return UCN_E2E_AAD_BYTES;
}

ucn_result_t ucn_frame_write_e2e_aad(const ucn_frame_t *frame,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     size_t *output_length)
{
    if (frame == NULL || output == NULL || output_length == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (output_capacity < UCN_E2E_AAD_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }

    output[0] = UCN_PROTOCOL_VERSION;
    output[1] = frame->message_type;
    output[2] = (uint8_t)frame->traffic_class;
    output[3] = frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED;
    write_u32_be(&output[4], frame->network_id);
    write_u32_be(&output[8], frame->source);
    write_u32_be(&output[12], frame->destination);
    write_u32_be(&output[16], frame->sequence);
    write_u32_be(&output[20], frame->session_id);
    write_u16_be(&output[24], frame->payload_length);
    *output_length = UCN_E2E_AAD_BYTES;
    return UCN_OK;
}

uint16_t ucn_crc16_ccitt(const uint8_t *data, size_t length, uint16_t seed)
{
    size_t index;
    uint16_t crc = seed;

    if (data == NULL && length != 0U) {
        return 0U;
    }

    for (index = 0U; index < length; ++index) {
        uint8_t bit;

        crc ^= (uint16_t)data[index] << 8U;
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc <<= 1U;
            }
        }
    }

    return crc;
}

static size_t frame_header_size(const ucn_frame_t *frame)
{
    return frame->has_route_extension ? UCN_FRAME_ROUTE_HEADER_SIZE :
                                        UCN_FRAME_HEADER_SIZE;
}

static size_t frame_crc_offset(size_t header_size)
{
    return header_size - sizeof(uint16_t);
}

static bool frame_is_protected(const ucn_frame_t *frame)
{
    return (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U;
}

static uint16_t frame_crc(const uint8_t *header, size_t header_size,
                          const uint8_t *payload, uint16_t payload_length,
                          const uint8_t *auth_tag)
{
    uint16_t crc = ucn_crc16_ccitt(header, frame_crc_offset(header_size), 0xFFFFU);

    crc = ucn_crc16_ccitt(payload, payload_length, crc);
    if (auth_tag != NULL) {
        crc = ucn_crc16_ccitt(auth_tag, UCN_E2E_TAG_SIZE, crc);
    }

    return crc;
}

size_t ucn_frame_encoded_size(const ucn_frame_t *frame)
{
    size_t header_size;
    size_t total_size;

    if (frame == NULL || frame->payload_length > UCN_MAX_PAYLOAD_BYTES ||
        (frame->flags & (uint8_t)~UCN_FRAME_KNOWN_FLAGS) != 0U ||
        (((frame->flags & UCN_FRAME_FLAG_ROUTE_EXTENSION) != 0U) !=
         frame->has_route_extension) ||
        (frame_is_protected(frame) && frame->auth_tag == NULL)) {
        return 0U;
    }

    header_size = frame_header_size(frame);
    total_size = header_size + (size_t)frame->payload_length +
                 (frame_is_protected(frame) ? UCN_E2E_TAG_SIZE : 0U);
    return total_size <= UCN_MAX_FRAME_BYTES ? total_size : 0U;
}

ucn_result_t ucn_frame_encode(const ucn_frame_t *frame,
                              uint8_t *output,
                              size_t output_capacity,
                              size_t *output_length)
{
    const size_t total_length = ucn_frame_encoded_size(frame);
    size_t header_size;
    size_t crc_offset;
    uint16_t crc;

    if (frame == NULL || output == NULL || output_length == NULL ||
        (frame->payload_length != 0U && frame->payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }

    if (total_length == 0U || output_capacity < total_length) {
        return UCN_ERR_TOO_LARGE;
    }

    if (frame->hop_limit == 0U || frame->traffic_class > UCN_TRAFFIC_Q3_BULK) {
        return UCN_ERR_ARGUMENT;
    }

    output[UCN_OFFSET_MAGIC_0] = UCN_FRAME_MAGIC_0;
    output[UCN_OFFSET_MAGIC_1] = UCN_FRAME_MAGIC_1;
    output[UCN_OFFSET_VERSION] = UCN_PROTOCOL_VERSION;
    output[UCN_OFFSET_MESSAGE_TYPE] = frame->message_type;
    output[UCN_OFFSET_TRAFFIC_CLASS] = (uint8_t)frame->traffic_class;
    output[UCN_OFFSET_FLAGS] = frame->flags;
    output[UCN_OFFSET_HOP_LIMIT] = frame->hop_limit;
    header_size = frame_header_size(frame);
    crc_offset = frame_crc_offset(header_size);
    output[UCN_OFFSET_HEADER_SIZE] = (uint8_t)header_size;
    write_u32_be(&output[UCN_OFFSET_NETWORK_ID], frame->network_id);
    write_u32_be(&output[UCN_OFFSET_SOURCE], frame->source);
    write_u32_be(&output[UCN_OFFSET_DESTINATION], frame->destination);
    write_u32_be(&output[UCN_OFFSET_SEQUENCE], frame->sequence);
    write_u32_be(&output[UCN_OFFSET_SESSION_ID], frame->session_id);
    write_u16_be(&output[UCN_OFFSET_PAYLOAD_LENGTH], frame->payload_length);

    if (frame->has_route_extension) {
        write_u16_be(&output[UCN_OFFSET_CRC], frame->route_epoch);
        output[UCN_OFFSET_CRC + 2U] = 0U;
        output[UCN_OFFSET_CRC + 3U] = 0U;
    }

    if (frame->payload_length != 0U) {
        size_t index;
        for (index = 0U; index < frame->payload_length; ++index) {
            output[header_size + index] = frame->payload[index];
        }
    }
    if (frame_is_protected(frame)) {
        (void)memcpy(&output[header_size + frame->payload_length], frame->auth_tag,
                     UCN_E2E_TAG_SIZE);
    }

    crc = frame_crc(output, header_size, &output[header_size], frame->payload_length,
                    frame_is_protected(frame) ?
                    &output[header_size + frame->payload_length] : NULL);
    write_u16_be(&output[crc_offset], crc);
    *output_length = total_length;
    return UCN_OK;
}

ucn_result_t ucn_frame_decode(const uint8_t *input,
                              size_t input_length,
                              ucn_frame_t *frame)
{
    size_t header_size;
    size_t crc_offset;
    size_t expected_length;
    uint16_t payload_length;
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (input == NULL || frame == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    if (input_length < UCN_FRAME_HEADER_SIZE) {
        return UCN_ERR_MALFORMED;
    }

    if (input[UCN_OFFSET_MAGIC_0] != UCN_FRAME_MAGIC_0 ||
        input[UCN_OFFSET_MAGIC_1] != UCN_FRAME_MAGIC_1) {
        return UCN_ERR_MALFORMED;
    }

    if (input[UCN_OFFSET_VERSION] != UCN_PROTOCOL_VERSION) {
        return UCN_ERR_VERSION;
    }

    header_size = input[UCN_OFFSET_HEADER_SIZE];
    if (header_size != UCN_FRAME_HEADER_SIZE &&
        header_size != UCN_FRAME_ROUTE_HEADER_SIZE) {
        return UCN_ERR_MALFORMED;
    }
    if ((input[UCN_OFFSET_FLAGS] & (uint8_t)~UCN_FRAME_KNOWN_FLAGS) != 0U ||
        (((input[UCN_OFFSET_FLAGS] & UCN_FRAME_FLAG_ROUTE_EXTENSION) != 0U) !=
         (header_size == UCN_FRAME_ROUTE_HEADER_SIZE))) {
        return UCN_ERR_MALFORMED;
    }

    if (input[UCN_OFFSET_TRAFFIC_CLASS] > (uint8_t)UCN_TRAFFIC_Q3_BULK ||
        input[UCN_OFFSET_HOP_LIMIT] == 0U) {
        return UCN_ERR_MALFORMED;
    }

    payload_length = read_u16_be(&input[UCN_OFFSET_PAYLOAD_LENGTH]);
    expected_length = header_size + (size_t)payload_length +
                      ((input[UCN_OFFSET_FLAGS] & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U ?
                       UCN_E2E_TAG_SIZE : 0U);
    if (payload_length > UCN_MAX_PAYLOAD_BYTES || expected_length > UCN_MAX_FRAME_BYTES ||
        input_length != expected_length) {
        return UCN_ERR_MALFORMED;
    }

    crc_offset = frame_crc_offset(header_size);
    expected_crc = read_u16_be(&input[crc_offset]);
    actual_crc = frame_crc(input, header_size, &input[header_size], payload_length,
                           (input[UCN_OFFSET_FLAGS] & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U ?
                           &input[header_size + payload_length] : NULL);
    if (actual_crc != expected_crc) {
        return UCN_ERR_CRC;
    }

    frame->message_type = input[UCN_OFFSET_MESSAGE_TYPE];
    frame->traffic_class = (ucn_traffic_class_t)input[UCN_OFFSET_TRAFFIC_CLASS];
    frame->flags = input[UCN_OFFSET_FLAGS];
    frame->hop_limit = input[UCN_OFFSET_HOP_LIMIT];
    frame->network_id = read_u32_be(&input[UCN_OFFSET_NETWORK_ID]);
    frame->source = read_u32_be(&input[UCN_OFFSET_SOURCE]);
    frame->destination = read_u32_be(&input[UCN_OFFSET_DESTINATION]);
    frame->sequence = read_u32_be(&input[UCN_OFFSET_SEQUENCE]);
    frame->session_id = read_u32_be(&input[UCN_OFFSET_SESSION_ID]);
    frame->has_route_extension = header_size == UCN_FRAME_ROUTE_HEADER_SIZE;
    frame->route_epoch = frame->has_route_extension ?
                         read_u16_be(&input[UCN_OFFSET_CRC]) : 0U;
    frame->payload_length = payload_length;
    frame->payload = &input[header_size];
    frame->auth_tag = (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U ?
                      &input[header_size + payload_length] : NULL;
    return UCN_OK;
}
