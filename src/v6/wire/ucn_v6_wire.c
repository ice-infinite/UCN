#include "ucn/v6/ucn_v6_wire.h"

#include <limits.h>
#include <string.h>

enum {
    UCN_V6_PREFIX_BYTES = 8,
    UCN_V6_FIXED_AFTER_PREFIX_BYTES = 4 + 8 + 4 + 4 + 2,
    UCN_V6_CRC_BYTES = 4,
    UCN_V6_PEER_CONTEXT_BYTES = 7,
    UCN_V6_GROUP_CONTEXT_BYTES = 15,
    UCN_V6_E2E_CONTEXT_BYTES = 8,
    UCN_V6_PROTOCOL_CONTEXT_BYTES = 2,
    UCN_V6_MESSAGE_CONTEXT_BYTES = 13,
    UCN_V6_ROUTE_CONTEXT_BYTES = 4,
    UCN_V6_PATH_CONTEXT_BYTES = 6,
    UCN_V6_HOP_BUDGET_CONTEXT_BYTES = 16
};

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

static void write_u64_be(uint8_t *output, uint64_t value)
{
    write_u32_be(output, (uint32_t)(value >> 32U));
    write_u32_be(output + 4U, (uint32_t)value);
}

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

static uint64_t read_u64_be(const uint8_t *input)
{
    return ((uint64_t)read_u32_be(input) << 32U) |
           read_u32_be(input + 4U);
}

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool suite_selector_is_valid(
    uint8_t suite_id,
    uint16_t key_id,
    uint32_t key_generation)
{
    return suite_id == 1U && key_id != 0U &&
           serial_is_valid(key_generation);
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

size_t ucn_v6_address_bytes(ucn_v6_address_class_t address_class)
{
    if ((uint32_t)address_class > (uint32_t)UCN_V6_ADDRESS_CLASS_A3) {
        return 0U;
    }
    return (size_t)address_class + 1U;
}

static uint32_t address_wire_max(ucn_v6_address_class_t address_class)
{
    static const uint32_t maxima[4] = {
        UINT32_C(0xFF), UINT32_C(0xFFFF), UINT32_C(0xFFFFFF), UINT32_MAX
    };

    return ucn_v6_address_bytes(address_class) == 0U ? 0U :
                                                       maxima[address_class];
}

uint32_t ucn_v6_address_max_ordinary(
    ucn_v6_address_class_t address_class)
{
    uint32_t maximum = address_wire_max(address_class);
    return maximum == 0U ? 0U : maximum - 1U;
}

static bool frame_type_is_valid(ucn_v6_frame_type_t frame_type)
{
    return frame_type >= UCN_V6_FRAME_BOOTSTRAP &&
           frame_type <= UCN_V6_FRAME_DIAGNOSTIC;
}

static bool frame_is_group_hello(const ucn_v6_frame_t *frame)
{
    return frame->frame_type == UCN_V6_FRAME_CONTROL &&
           frame->protocol_opcode == UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO &&
           (frame->flags & UCN_V6_FLAG_GROUP_CONTEXT) != 0U;
}

static bool absent_contexts_are_canonical(const ucn_v6_frame_t *frame)
{
    if ((frame->flags & UCN_V6_FLAG_PEER_HOP_CONTEXT) == 0U &&
        (frame->peer_hop.suite_id != 0U || frame->peer_hop.key_id != 0U ||
         frame->peer_hop.key_generation != 0U)) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_GROUP_CONTEXT) == 0U &&
        (frame->group.group_id != 0U || frame->group.group_generation != 0U ||
         frame->group.suite_id != 0U || frame->group.key_id != 0U ||
         frame->group.key_generation != 0U)) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_E2E_CONTEXT) == 0U &&
        (frame->e2e.mode != UCN_V6_E2E_NONE || frame->e2e.suite_id != 0U ||
         frame->e2e.key_id != 0U || frame->e2e.key_generation != 0U ||
         !bytes_are_zero(frame->e2e_tag, sizeof(frame->e2e_tag)))) {
        return false;
    }
    if ((frame->flags & (UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_GROUP_CONTEXT)) == 0U &&
        !bytes_are_zero(frame->link_tag, sizeof(frame->link_tag))) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_PROTOCOL_CONTEXT) == 0U &&
        frame->protocol_opcode != 0U) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_MESSAGE_CONTEXT) == 0U &&
        (frame->message.source_endpoint != 0U ||
         frame->message.destination_endpoint != 0U ||
         frame->message.interaction_role != UCN_V6_INTERACTION_ONE_WAY ||
         frame->message.operation_id != 0U)) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_ROUTE_CONTEXT) == 0U &&
        frame->route_generation != 0U) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_PATH_CONTEXT) == 0U &&
        (frame->path.path_id != 0U || frame->path.path_generation != 0U)) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) == 0U &&
        (frame->hop_budget.initial_budget_us != 0U ||
         frame->hop_budget.remaining_budget_us != 0U)) {
        return false;
    }
    return true;
}

static bool frame_contract_is_valid(const ucn_v6_frame_t *frame)
{
    uint32_t wire_max;
    uint32_t ordinary_max;
    bool peer;
    bool group;
    bool protocol;
    bool message;

    if (frame == NULL || !frame_type_is_valid(frame->frame_type) ||
        (uint32_t)frame->traffic_class > (uint32_t)UCN_V6_TRAFFIC_Q3 ||
        (uint32_t)frame->delivery_guarantee >
            (uint32_t)UCN_V6_DELIVERY_RELIABLE ||
        frame->header_contract != UCN_V6_HEADER_CONTRACT_1 ||
        frame->hop_limit == 0U || frame->realm_id == 0U ||
        frame->realm_id == UINT32_MAX ||
        (frame->payload_length != 0U && frame->payload == NULL) ||
        !absent_contexts_are_canonical(frame)) {
        return false;
    }
    wire_max = address_wire_max(frame->address_class);
    ordinary_max = ucn_v6_address_max_ordinary(frame->address_class);
    if (wire_max == 0U || frame->source_address > wire_max ||
        frame->destination_address > wire_max) {
        return false;
    }
    peer = (frame->flags & UCN_V6_FLAG_PEER_HOP_CONTEXT) != 0U;
    group = (frame->flags & UCN_V6_FLAG_GROUP_CONTEXT) != 0U;
    protocol = (frame->flags & UCN_V6_FLAG_PROTOCOL_CONTEXT) != 0U;
    message = (frame->flags & UCN_V6_FLAG_MESSAGE_CONTEXT) != 0U;
    if (peer && group) {
        return false;
    }
    if (peer && !suite_selector_is_valid(
                    frame->peer_hop.suite_id, frame->peer_hop.key_id,
                    frame->peer_hop.key_generation)) {
        return false;
    }
    if (group &&
        (!serial_is_valid(frame->group.group_id) ||
         !serial_is_valid(frame->group.group_generation) ||
         !suite_selector_is_valid(frame->group.suite_id, frame->group.key_id,
                                  frame->group.key_generation))) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U &&
        (frame->e2e.mode < UCN_V6_E2E_AUTH_ONLY ||
         frame->e2e.mode > UCN_V6_E2E_AEAD ||
         !suite_selector_is_valid(frame->e2e.suite_id, frame->e2e.key_id,
                                  frame->e2e.key_generation))) {
        return false;
    }
    if (frame->frame_type == UCN_V6_FRAME_DATA ? protocol : !protocol) {
        return false;
    }
    if (protocol && frame->protocol_opcode == 0U) {
        return false;
    }
    if (message) {
        if (frame->message.source_endpoint == 0U ||
            frame->message.destination_endpoint == 0U ||
            (uint32_t)frame->message.interaction_role >
                (uint32_t)UCN_V6_INTERACTION_ERROR ||
            ((frame->message.interaction_role ==
                  UCN_V6_INTERACTION_ONE_WAY) !=
             (frame->message.operation_id == 0U)) ||
            frame->message.operation_id >
                UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
            return false;
        }
    } else if (frame->frame_type == UCN_V6_FRAME_DATA) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_ROUTE_CONTEXT) != 0U &&
        (frame->flags & UCN_V6_FLAG_PATH_CONTEXT) != 0U) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_ROUTE_CONTEXT) != 0U &&
        !serial_is_valid(frame->route_generation)) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_PATH_CONTEXT) != 0U &&
        (frame->path.path_id == 0U ||
         !serial_is_valid(frame->path.path_generation))) {
        return false;
    }
    if ((frame->flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U &&
        (frame->hop_budget.initial_budget_us == 0U ||
         frame->hop_budget.remaining_budget_us == 0U ||
         frame->hop_budget.remaining_budget_us >
             frame->hop_budget.initial_budget_us)) {
        return false;
    }
    if (frame->frame_type == UCN_V6_FRAME_BOOTSTRAP) {
        return !peer && !group &&
               (frame->flags & (UCN_V6_FLAG_E2E_CONTEXT |
                                UCN_V6_FLAG_MESSAGE_CONTEXT |
                                UCN_V6_FLAG_ROUTE_CONTEXT |
                                UCN_V6_FLAG_PATH_CONTEXT |
                                UCN_V6_FLAG_HOP_BUDGET_CONTEXT)) == 0U &&
               frame->source_address == 0U &&
               frame->destination_address == wire_max &&
               frame->source_binding_generation == 0U &&
               frame->destination_binding_generation == 0U &&
               frame->session_generation == 0U &&
               frame->packet_sequence == 0U && frame->hop_limit == 1U &&
               frame->traffic_class == UCN_V6_TRAFFIC_Q0 &&
               frame->delivery_guarantee == UCN_V6_DELIVERY_BEST_EFFORT;
    }
    if (frame_is_group_hello(frame)) {
        return !peer && group && frame->source_address != 0U &&
               frame->source_address <= ordinary_max &&
               frame->destination_address == wire_max &&
               serial_is_valid(frame->source_binding_generation) &&
               frame->destination_binding_generation == 0U &&
               serial_is_valid(frame->session_generation) &&
               serial_is_valid(frame->packet_sequence) &&
               frame->hop_limit == 1U &&
               frame->traffic_class == UCN_V6_TRAFFIC_Q1 &&
               frame->delivery_guarantee == UCN_V6_DELIVERY_LATEST &&
               !message &&
               (frame->flags & (UCN_V6_FLAG_E2E_CONTEXT |
                                UCN_V6_FLAG_ROUTE_CONTEXT |
                                UCN_V6_FLAG_PATH_CONTEXT |
                                UCN_V6_FLAG_HOP_BUDGET_CONTEXT)) == 0U;
    }
    return peer && !group && frame->source_address != 0U &&
           frame->source_address <= ordinary_max &&
           frame->destination_address != 0U &&
           frame->destination_address <= ordinary_max &&
           serial_is_valid(frame->source_binding_generation) &&
           serial_is_valid(frame->destination_binding_generation) &&
           serial_is_valid(frame->session_generation) &&
           serial_is_valid(frame->packet_sequence);
}

static bool size_add(size_t *value, size_t increment)
{
    if (*value > SIZE_MAX - increment) {
        return false;
    }
    *value += increment;
    return true;
}

ucn_v6_result_t ucn_v6_wire_encoded_size(
    const ucn_v6_frame_t *frame,
    size_t *encoded_size)
{
    size_t size;
    size_t address_bytes;

    if (encoded_size == NULL || !frame_contract_is_valid(frame)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    address_bytes = ucn_v6_address_bytes(frame->address_class);
    size = UCN_V6_PREFIX_BYTES + UCN_V6_FIXED_AFTER_PREFIX_BYTES +
           address_bytes * 2U + UCN_V6_CRC_BYTES;
    if (((frame->flags & UCN_V6_FLAG_PEER_HOP_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_PEER_CONTEXT_BYTES)) ||
        ((frame->flags & UCN_V6_FLAG_GROUP_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_GROUP_CONTEXT_BYTES)) ||
        ((frame->flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_E2E_CONTEXT_BYTES)) ||
        ((frame->flags & UCN_V6_FLAG_PROTOCOL_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_PROTOCOL_CONTEXT_BYTES)) ||
        ((frame->flags & UCN_V6_FLAG_MESSAGE_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_MESSAGE_CONTEXT_BYTES)) ||
        ((frame->flags & UCN_V6_FLAG_ROUTE_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_ROUTE_CONTEXT_BYTES)) ||
        ((frame->flags & UCN_V6_FLAG_PATH_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_PATH_CONTEXT_BYTES)) ||
        ((frame->flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_HOP_BUDGET_CONTEXT_BYTES)) ||
        !size_add(&size, frame->payload_length) ||
        ((frame->flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U &&
         !size_add(&size, UCN_V6_SECURITY_TAG_BYTES)) ||
        (((frame->flags & (UCN_V6_FLAG_PEER_HOP_CONTEXT |
                           UCN_V6_FLAG_GROUP_CONTEXT)) != 0U) &&
         !size_add(&size, UCN_V6_SECURITY_TAG_BYTES))) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    if (size > UCN_V6_WIRE_MAX_FRAME_BYTES) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *encoded_size = size;
    return UCN_V6_OK;
}

static void write_address(uint8_t *output, size_t bytes, uint32_t address)
{
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        output[index] = (uint8_t)(address >> (8U * (bytes - index - 1U)));
    }
}

static uint32_t read_address(const uint8_t *input, size_t bytes)
{
    size_t index;
    uint32_t value = 0U;
    for (index = 0U; index < bytes; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

uint32_t ucn_v6_crc32c(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t index;
    unsigned bit;

    if (data == NULL && length != 0U) {
        return 0U;
    }
    for (index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^
                  ((crc & 1U) != 0U ? UINT32_C(0x82F63B78) : 0U);
        }
    }
    return ~crc;
}

ucn_v6_result_t ucn_v6_wire_encode(
    const ucn_v6_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    size_t required;
    size_t address_bytes;
    size_t offset = 0U;
    uint32_t crc;
    ucn_v6_result_t result;

    if (output == NULL || output_length == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = ucn_v6_wire_encoded_size(frame, &required);
    if (result != UCN_V6_OK) {
        return result;
    }
    if (output_capacity < required) {
        return UCN_V6_ERR_NO_SPACE;
    }
    address_bytes = ucn_v6_address_bytes(frame->address_class);
    output[offset++] = UCN_V6_WIRE_MAGIC_0;
    output[offset++] = UCN_V6_WIRE_MAGIC_1;
    output[offset++] = (uint8_t)(((uint8_t)frame->address_class << 6U) |
                                 UCN_V6_PROTOCOL_VERSION);
    output[offset++] = (uint8_t)frame->frame_type;
    output[offset++] = frame->flags;
    output[offset++] = (uint8_t)((uint8_t)frame->traffic_class |
        ((uint8_t)frame->delivery_guarantee << 2U));
    output[offset++] = frame->hop_limit;
    output[offset++] = frame->header_contract;
    write_u32_be(output + offset, frame->realm_id);
    offset += 4U;
    write_address(output + offset, address_bytes, frame->source_address);
    offset += address_bytes;
    write_address(output + offset, address_bytes, frame->destination_address);
    offset += address_bytes;
    write_u32_be(output + offset, frame->source_binding_generation);
    offset += 4U;
    write_u32_be(output + offset, frame->destination_binding_generation);
    offset += 4U;
    write_u32_be(output + offset, frame->session_generation);
    offset += 4U;
    write_u32_be(output + offset, frame->packet_sequence);
    offset += 4U;
    write_u16_be(output + offset, frame->payload_length);
    offset += 2U;
    if ((frame->flags & UCN_V6_FLAG_PEER_HOP_CONTEXT) != 0U) {
        output[offset++] = frame->peer_hop.suite_id;
        write_u16_be(output + offset, frame->peer_hop.key_id);
        offset += 2U;
        write_u32_be(output + offset, frame->peer_hop.key_generation);
        offset += 4U;
    }
    if ((frame->flags & UCN_V6_FLAG_GROUP_CONTEXT) != 0U) {
        write_u32_be(output + offset, frame->group.group_id);
        offset += 4U;
        write_u32_be(output + offset, frame->group.group_generation);
        offset += 4U;
        output[offset++] = frame->group.suite_id;
        write_u16_be(output + offset, frame->group.key_id);
        offset += 2U;
        write_u32_be(output + offset, frame->group.key_generation);
        offset += 4U;
    }
    if ((frame->flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U) {
        output[offset++] = (uint8_t)frame->e2e.mode;
        output[offset++] = frame->e2e.suite_id;
        write_u16_be(output + offset, frame->e2e.key_id);
        offset += 2U;
        write_u32_be(output + offset, frame->e2e.key_generation);
        offset += 4U;
    }
    if ((frame->flags & UCN_V6_FLAG_PROTOCOL_CONTEXT) != 0U) {
        write_u16_be(output + offset, frame->protocol_opcode);
        offset += 2U;
    }
    if ((frame->flags & UCN_V6_FLAG_MESSAGE_CONTEXT) != 0U) {
        write_u16_be(output + offset, frame->message.source_endpoint);
        offset += 2U;
        write_u16_be(output + offset, frame->message.destination_endpoint);
        offset += 2U;
        output[offset++] = (uint8_t)frame->message.interaction_role;
        write_u64_be(output + offset, frame->message.operation_id);
        offset += 8U;
    }
    if ((frame->flags & UCN_V6_FLAG_ROUTE_CONTEXT) != 0U) {
        write_u32_be(output + offset, frame->route_generation);
        offset += 4U;
    }
    if ((frame->flags & UCN_V6_FLAG_PATH_CONTEXT) != 0U) {
        write_u16_be(output + offset, frame->path.path_id);
        offset += 2U;
        write_u32_be(output + offset, frame->path.path_generation);
        offset += 4U;
    }
    if ((frame->flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U) {
        write_u64_be(output + offset, frame->hop_budget.initial_budget_us);
        offset += 8U;
        write_u64_be(output + offset, frame->hop_budget.remaining_budget_us);
        offset += 8U;
    }
    if (frame->payload_length != 0U) {
        memcpy(output + offset, frame->payload, frame->payload_length);
        offset += frame->payload_length;
    }
    if ((frame->flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U) {
        memcpy(output + offset, frame->e2e_tag, UCN_V6_SECURITY_TAG_BYTES);
        offset += UCN_V6_SECURITY_TAG_BYTES;
    }
    if ((frame->flags & (UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_GROUP_CONTEXT)) != 0U) {
        memcpy(output + offset, frame->link_tag, UCN_V6_SECURITY_TAG_BYTES);
        offset += UCN_V6_SECURITY_TAG_BYTES;
    }
    crc = ucn_v6_crc32c(output, offset);
    write_u32_be(output + offset, crc);
    offset += 4U;
    *output_length = offset;
    return UCN_V6_OK;
}

static bool range_fits(size_t offset, size_t needed, size_t limit)
{
    return offset <= limit && needed <= limit - offset;
}

ucn_v6_result_t ucn_v6_wire_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_frame_t *frame)
{
    ucn_v6_frame_t decoded;
    size_t address_bytes;
    size_t offset = 0U;
    size_t required;
    uint8_t version_class;
    uint8_t traffic_delivery;
    uint32_t received_crc;

    if (input == NULL || frame == NULL || input_length < 36U ||
        input_length > UCN_V6_WIRE_MAX_FRAME_BYTES ||
        input[0] != UCN_V6_WIRE_MAGIC_0 ||
        input[1] != UCN_V6_WIRE_MAGIC_1) {
        return UCN_V6_ERR_MALFORMED;
    }
    version_class = input[2];
    if ((version_class & 0x3FU) != UCN_V6_PROTOCOL_VERSION) {
        return UCN_V6_ERR_MALFORMED;
    }
    received_crc = read_u32_be(input + input_length - UCN_V6_CRC_BYTES);
    if (received_crc !=
        ucn_v6_crc32c(input, input_length - UCN_V6_CRC_BYTES)) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.address_class = (ucn_v6_address_class_t)(version_class >> 6U);
    address_bytes = ucn_v6_address_bytes(decoded.address_class);
    if (address_bytes == 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    offset = 3U;
    decoded.frame_type = (ucn_v6_frame_type_t)input[offset++];
    decoded.flags = input[offset++];
    traffic_delivery = input[offset++];
    if ((traffic_delivery & 0xF0U) != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    decoded.traffic_class =
        (ucn_v6_traffic_class_t)(traffic_delivery & 0x03U);
    decoded.delivery_guarantee =
        (ucn_v6_delivery_guarantee_t)((traffic_delivery >> 2U) & 0x03U);
    decoded.hop_limit = input[offset++];
    decoded.header_contract = input[offset++];
    if (!range_fits(offset, UCN_V6_FIXED_AFTER_PREFIX_BYTES +
                                address_bytes * 2U,
                    input_length - UCN_V6_CRC_BYTES)) {
        return UCN_V6_ERR_MALFORMED;
    }
    decoded.realm_id = read_u32_be(input + offset);
    offset += 4U;
    decoded.source_address = read_address(input + offset, address_bytes);
    offset += address_bytes;
    decoded.destination_address = read_address(input + offset, address_bytes);
    offset += address_bytes;
    decoded.source_binding_generation = read_u32_be(input + offset);
    offset += 4U;
    decoded.destination_binding_generation = read_u32_be(input + offset);
    offset += 4U;
    decoded.session_generation = read_u32_be(input + offset);
    offset += 4U;
    decoded.packet_sequence = read_u32_be(input + offset);
    offset += 4U;
    decoded.payload_length = read_u16_be(input + offset);
    offset += 2U;
#define READ_CONTEXT(bytes_)                                                     \
    do {                                                                         \
        if (!range_fits(offset, (bytes_), input_length - UCN_V6_CRC_BYTES)) {    \
            return UCN_V6_ERR_MALFORMED;                                         \
        }                                                                        \
    } while (0)
    if ((decoded.flags & UCN_V6_FLAG_PEER_HOP_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_PEER_CONTEXT_BYTES);
        decoded.peer_hop.suite_id = input[offset++];
        decoded.peer_hop.key_id = read_u16_be(input + offset);
        offset += 2U;
        decoded.peer_hop.key_generation = read_u32_be(input + offset);
        offset += 4U;
    }
    if ((decoded.flags & UCN_V6_FLAG_GROUP_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_GROUP_CONTEXT_BYTES);
        decoded.group.group_id = read_u32_be(input + offset);
        offset += 4U;
        decoded.group.group_generation = read_u32_be(input + offset);
        offset += 4U;
        decoded.group.suite_id = input[offset++];
        decoded.group.key_id = read_u16_be(input + offset);
        offset += 2U;
        decoded.group.key_generation = read_u32_be(input + offset);
        offset += 4U;
    }
    if ((decoded.flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_E2E_CONTEXT_BYTES);
        decoded.e2e.mode = (ucn_v6_e2e_mode_t)input[offset++];
        decoded.e2e.suite_id = input[offset++];
        decoded.e2e.key_id = read_u16_be(input + offset);
        offset += 2U;
        decoded.e2e.key_generation = read_u32_be(input + offset);
        offset += 4U;
    }
    if ((decoded.flags & UCN_V6_FLAG_PROTOCOL_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_PROTOCOL_CONTEXT_BYTES);
        decoded.protocol_opcode = read_u16_be(input + offset);
        offset += 2U;
    }
    if ((decoded.flags & UCN_V6_FLAG_MESSAGE_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_MESSAGE_CONTEXT_BYTES);
        decoded.message.source_endpoint = read_u16_be(input + offset);
        offset += 2U;
        decoded.message.destination_endpoint = read_u16_be(input + offset);
        offset += 2U;
        decoded.message.interaction_role =
            (ucn_v6_interaction_role_t)input[offset++];
        decoded.message.operation_id = read_u64_be(input + offset);
        offset += 8U;
    }
    if ((decoded.flags & UCN_V6_FLAG_ROUTE_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_ROUTE_CONTEXT_BYTES);
        decoded.route_generation = read_u32_be(input + offset);
        offset += 4U;
    }
    if ((decoded.flags & UCN_V6_FLAG_PATH_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_PATH_CONTEXT_BYTES);
        decoded.path.path_id = read_u16_be(input + offset);
        offset += 2U;
        decoded.path.path_generation = read_u32_be(input + offset);
        offset += 4U;
    }
    if ((decoded.flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_HOP_BUDGET_CONTEXT_BYTES);
        decoded.hop_budget.initial_budget_us = read_u64_be(input + offset);
        offset += 8U;
        decoded.hop_budget.remaining_budget_us = read_u64_be(input + offset);
        offset += 8U;
    }
    READ_CONTEXT((size_t)decoded.payload_length);
    decoded.payload = decoded.payload_length == 0U ? NULL : input + offset;
    offset += decoded.payload_length;
    if ((decoded.flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U) {
        READ_CONTEXT(UCN_V6_SECURITY_TAG_BYTES);
        memcpy(decoded.e2e_tag, input + offset, UCN_V6_SECURITY_TAG_BYTES);
        offset += UCN_V6_SECURITY_TAG_BYTES;
    }
    if ((decoded.flags & (UCN_V6_FLAG_PEER_HOP_CONTEXT |
                          UCN_V6_FLAG_GROUP_CONTEXT)) != 0U) {
        READ_CONTEXT(UCN_V6_SECURITY_TAG_BYTES);
        memcpy(decoded.link_tag, input + offset, UCN_V6_SECURITY_TAG_BYTES);
        offset += UCN_V6_SECURITY_TAG_BYTES;
    }
#undef READ_CONTEXT
    if (offset + UCN_V6_CRC_BYTES != input_length ||
        !frame_contract_is_valid(&decoded) ||
        ucn_v6_wire_encoded_size(&decoded, &required) != UCN_V6_OK ||
        required != input_length) {
        return UCN_V6_ERR_MALFORMED;
    }
    *frame = decoded;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_wire_write_canonical_aad(
    const ucn_v6_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint8_t aad[UCN_V6_CANONICAL_AAD_BYTES];
    size_t offset = 0U;
    uint8_t context_kind = 0U;

    if (output == NULL || output_length == NULL ||
        !frame_contract_is_valid(frame)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (output_capacity < UCN_V6_CANONICAL_AAD_BYTES) {
        return UCN_V6_ERR_NO_SPACE;
    }
    aad[offset++] = UCN_V6_PROTOCOL_VERSION;
    aad[offset++] = (uint8_t)frame->address_class;
    aad[offset++] = frame->flags;
    aad[offset++] = frame->header_contract;
    aad[offset++] = (uint8_t)frame->frame_type;
    write_u16_be(aad + offset, frame->protocol_opcode);
    offset += 2U;
    aad[offset++] = (uint8_t)frame->traffic_class;
    aad[offset++] = (uint8_t)frame->delivery_guarantee;
    aad[offset++] = (uint8_t)frame->message.interaction_role;
    aad[offset++] = (uint8_t)frame->e2e.mode;
    aad[offset++] = frame->e2e.suite_id;
    write_u16_be(aad + offset, frame->e2e.key_id);
    offset += 2U;
    write_u32_be(aad + offset, frame->e2e.key_generation);
    offset += 4U;
    write_u32_be(aad + offset, frame->realm_id);
    offset += 4U;
    write_u32_be(aad + offset, frame->source_address);
    offset += 4U;
    write_u32_be(aad + offset, frame->source_binding_generation);
    offset += 4U;
    write_u32_be(aad + offset, frame->destination_address);
    offset += 4U;
    write_u32_be(aad + offset, frame->destination_binding_generation);
    offset += 4U;
    write_u32_be(aad + offset, frame->session_generation);
    offset += 4U;
    write_u32_be(aad + offset, frame->packet_sequence);
    offset += 4U;
    write_u16_be(aad + offset, frame->message.source_endpoint);
    offset += 2U;
    write_u16_be(aad + offset, frame->message.destination_endpoint);
    offset += 2U;
    write_u64_be(aad + offset, frame->message.operation_id);
    offset += 8U;
    if ((frame->flags & UCN_V6_FLAG_ROUTE_CONTEXT) != 0U) {
        context_kind = 1U;
    } else if ((frame->flags & UCN_V6_FLAG_PATH_CONTEXT) != 0U) {
        context_kind = 2U;
    }
    aad[offset++] = context_kind;
    write_u32_be(aad + offset, frame->route_generation);
    offset += 4U;
    write_u16_be(aad + offset, frame->path.path_id);
    offset += 2U;
    write_u32_be(aad + offset, frame->path.path_generation);
    offset += 4U;
    aad[offset++] = (frame->flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U ?
                        1U : 0U;
    write_u64_be(aad + offset, frame->hop_budget.initial_budget_us);
    offset += 8U;
    write_u16_be(aad + offset, frame->payload_length);
    offset += 2U;
    if (offset != UCN_V6_CANONICAL_AAD_BYTES) {
        return UCN_V6_ERR_STATE;
    }
    memcpy(output, aad, sizeof(aad));
    *output_length = sizeof(aad);
    return UCN_V6_OK;
}
