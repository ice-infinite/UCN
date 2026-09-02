#include <string.h>

#include "ucn/ucn_frame.h"

enum {
    UCN_OFFSET_MAGIC_0 = 0,
    UCN_OFFSET_MAGIC_1 = 1,
    UCN_OFFSET_VERSION_PROFILE = 2,
    UCN_OFFSET_MESSAGE_TYPE = 3,
    UCN_OFFSET_TRAFFIC_FLAGS = 4,
    UCN_OFFSET_HOP_LIMIT = 5,
    UCN_OFFSET_VARIABLE = 6
};

#define UCN_VERSION_MASK ((uint8_t)0x3FU)
#define UCN_WIRE_PROFILE_SHIFT ((uint8_t)6U)
#define UCN_WIRE_FLAGS_MASK ((uint8_t)0x3FU)
#define UCN_E2E_AAD_BYTES ((size_t)30U)

static const ucn_wire_profile_descriptor_t UCN_WIRE_PROFILES[] = {
    { UCN_WIRE_PROFILE_W0_LOCAL, 0U, 1U, 1U, 1U, 1U, 3U, 4U,
      UINT32_C(0x000000FF), UINT32_C(0x000000FE) },
    { UCN_WIRE_PROFILE_W1_EDGE, 1U, 2U, 1U, 2U, 2U, 3U, 16U,
      UINT32_C(0x0000FFFF), UINT32_C(0x0000FFFE) },
    { UCN_WIRE_PROFILE_W2_MESH, 2U, 3U, 2U, 2U, 3U, 3U, 64U,
      UINT32_C(0x00FFFFFF), UINT32_C(0x00FFFFFE) },
    { UCN_WIRE_PROFILE_W3_BACKBONE, 3U, 4U, 2U, 2U, 4U, 4U, 254U,
      UINT32_MAX, UINT32_MAX - UINT32_C(1) }
};

static bool frame_is_protected(const ucn_frame_t *frame);
static ucn_result_t validate_frame_fields(
    const ucn_frame_t *frame,
    bool require_auth_tag,
    const ucn_wire_profile_descriptor_t **descriptor_out);

/*
 * EN: Selects or resolves `resolve_profile` using deterministic Wire frame codec rules.
 * 中文：按照确定性的 Wire 帧编解码 规则选择或解析 `resolve_profile`。
 */
static const ucn_wire_profile_descriptor_t *resolve_profile(
    ucn_wire_profile_t profile)
{
    if (profile == UCN_WIRE_PROFILE_UNSPECIFIED) {
        profile = UCN_WIRE_PROFILE_W3_BACKBONE;
    }
    return ucn_wire_profile_get_descriptor(profile);
}

/*
 * EN: Returns the current `wire_profile_get_descriptor` view from Wire frame codec state.
 * 中文：从 Wire 帧编解码 状态返回当前 `wire_profile_get_descriptor` 视图。
 */
const ucn_wire_profile_descriptor_t *ucn_wire_profile_get_descriptor(
    ucn_wire_profile_t profile)
{
    size_t index;

    for (index = 0U;
         index < sizeof(UCN_WIRE_PROFILES) / sizeof(UCN_WIRE_PROFILES[0]);
         ++index) {
        if (UCN_WIRE_PROFILES[index].profile == profile) {
            return &UCN_WIRE_PROFILES[index];
        }
    }
    return NULL;
}

/*
 * EN: Derives `profile_from_wire_code` with the canonical Wire frame codec conversion rules.
 * 中文：按照规范的 Wire 帧编解码 转换规则推导 `profile_from_wire_code`。
 */
static ucn_wire_profile_t profile_from_wire_code(uint8_t wire_code)
{
    return (ucn_wire_profile_t)(wire_code +
                                (uint8_t)UCN_WIRE_PROFILE_W0_LOCAL);
}

/*
 * EN: Builds the packed protocol-version and Wire-Profile header byte.
 * 中文：构造协议版本与 Wire Profile 组合后的头部字节。
 */
static uint8_t version_profile_byte(
    const ucn_wire_profile_descriptor_t *descriptor)
{
    return (uint8_t)((descriptor->wire_code << UCN_WIRE_PROFILE_SHIFT) |
                     (UCN_PROTOCOL_VERSION & UCN_VERSION_MASK));
}

/*
 * EN: Writes `uint_be` in the canonical Wire frame codec byte order.
 * 中文：按规范的 Wire 帧编解码 字节序写入 `uint_be`。
 */
static void write_uint_be(uint8_t *output, uint8_t width, uint32_t value)
{
    uint8_t index;

    for (index = 0U; index < width; ++index) {
        const uint8_t shift = (uint8_t)((width - index - 1U) * 8U);
        output[index] = (uint8_t)(value >> shift);
    }
}

/*
 * EN: Reads `uint_be` from the canonical Wire frame codec byte order.
 * 中文：按规范的 Wire 帧编解码 字节序读取 `uint_be`。
 */
static uint32_t read_uint_be(const uint8_t *input, uint8_t width)
{
    uint8_t index;
    uint32_t value = 0U;

    for (index = 0U; index < width; ++index) {
        value = (value << 8U) | (uint32_t)input[index];
    }
    return value;
}

/*
 * EN: Calculates `max_for_width` with bounded, deterministic Wire frame codec arithmetic.
 * 中文：使用有界且确定性的 Wire 帧编解码 算术计算 `max_for_width`。
 */
static uint32_t max_for_width(uint8_t width)
{
    if (width >= 4U) {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << ((uint32_t)width * 8U)) - UINT32_C(1);
}

/*
 * EN: Checks whether `flags` satisfies the Wire frame codec module's validity rules.
 * 中文：检查 `flags` 是否满足 Wire 帧编解码 模块的合法性规则。
 */
static bool flags_are_valid(uint8_t flags)
{
    return (flags & (uint8_t)~UCN_FRAME_KNOWN_FLAGS) == 0U &&
           !(((flags & UCN_FRAME_FLAG_PATH_ID) != 0U) &&
             ((flags & UCN_FRAME_FLAG_ROUTE_EXTENSION) == 0U));
}

/*
 * EN: Calculates `header_size_for_profile` with bounded, deterministic Wire frame codec arithmetic.
 * 中文：使用有界且确定性的 Wire 帧编解码 算术计算 `header_size_for_profile`。
 */
size_t ucn_frame_header_size_for_profile(ucn_wire_profile_t profile,
                                         uint8_t flags)
{
    const ucn_wire_profile_descriptor_t *descriptor = resolve_profile(profile);
    size_t header_size;

    if (descriptor == NULL || !flags_are_valid(flags)) {
        return 0U;
    }

    header_size = (size_t)12U +
                  (size_t)4U * (size_t)descriptor->address_bytes +
                  (size_t)descriptor->payload_length_bytes;
    if ((flags & UCN_FRAME_FLAG_ROUTE_EXTENSION) != 0U) {
        header_size += descriptor->route_epoch_bytes;
    }
    if ((flags & UCN_FRAME_FLAG_PATH_ID) != 0U) {
        header_size += descriptor->path_id_bytes;
    }
    return header_size;
}

/*
 * EN: Calculates `max_payload_for_profile` with bounded, deterministic Wire frame codec arithmetic.
 * 中文：使用有界且确定性的 Wire 帧编解码 算术计算 `max_payload_for_profile`。
 */
size_t ucn_frame_max_payload_for_profile(ucn_wire_profile_t profile,
                                         uint8_t flags)
{
    const ucn_wire_profile_descriptor_t *descriptor = resolve_profile(profile);
    const size_t header_size = ucn_frame_header_size_for_profile(profile, flags);
    const size_t tag_size =
        (flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U ? UCN_E2E_TAG_SIZE : 0U;
    size_t available;
    size_t profile_limit;

    if (descriptor == NULL || header_size == 0U ||
        header_size + tag_size > UCN_MAX_FRAME_BYTES) {
        return 0U;
    }

    available = UCN_MAX_FRAME_BYTES - header_size - tag_size;
    profile_limit = (size_t)max_for_width(descriptor->payload_length_bytes);
    if (available > profile_limit) {
        available = profile_limit;
    }
    if (available > UCN_MAX_PAYLOAD_BYTES) {
        available = UCN_MAX_PAYLOAD_BYTES;
    }
    return available;
}

/*
 * EN: Calculates `max_payload` with bounded, deterministic Wire frame codec arithmetic.
 * 中文：使用有界且确定性的 Wire 帧编解码 算术计算 `max_payload`。
 */
size_t ucn_frame_max_payload(uint8_t flags)
{
    return ucn_frame_max_payload_for_profile(UCN_WIRE_PROFILE_W3_BACKBONE,
                                             flags);
}

/*
 * EN: Selects or resolves `select_min_wire_profile` using deterministic Wire frame codec rules.
 * 中文：按照确定性的 Wire 帧编解码 规则选择或解析 `select_min_wire_profile`。
 */
ucn_result_t ucn_frame_select_min_wire_profile(
    const ucn_frame_t *frame,
    ucn_wire_profile_t maximum_profile,
    size_t mtu,
    ucn_wire_profile_t *selected_profile)
{
    ucn_wire_profile_t profile;
    bool saw_size_failure = false;

    if (frame == NULL || selected_profile == NULL ||
        ucn_wire_profile_get_descriptor(maximum_profile) == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    for (profile = UCN_WIRE_PROFILE_W0_LOCAL;
         profile <= maximum_profile; ++profile) {
        const ucn_wire_profile_descriptor_t *descriptor;
        ucn_frame_t candidate = *frame;
        size_t total_size;
        ucn_result_t result;

        candidate.wire_profile = profile;
        result = validate_frame_fields(&candidate, false, &descriptor);
        if (result == UCN_ERR_TOO_LARGE) {
            saw_size_failure = true;
            continue;
        }
        if (result != UCN_OK) {
            return result;
        }
        total_size = ucn_frame_header_size_for_profile(profile,
                                                        candidate.flags) +
                     (size_t)candidate.payload_length +
                     (frame_is_protected(&candidate) ? UCN_E2E_TAG_SIZE : 0U);
        if (total_size > UCN_MAX_FRAME_BYTES ||
            (mtu != 0U && total_size > mtu)) {
            saw_size_failure = true;
            continue;
        }
        *selected_profile = profile;
        return UCN_OK;
    }
    return saw_size_failure ? UCN_ERR_TOO_LARGE : UCN_ERR_UNSUPPORTED;
}

/*
 * EN: Calculates the bounded `e2e_aad_size` value used by Wire frame codec.
 * 中文：计算 Wire 帧编解码 使用的有界 `e2e_aad_size` 值。
 */
size_t ucn_frame_e2e_aad_size(void)
{
    return UCN_E2E_AAD_BYTES;
}

/*
 * EN: Checks the `is_protected` predicate against current Wire frame codec state.
 * 中文：根据当前 Wire 帧编解码 状态检查 `is_protected` 条件。
 */
static bool frame_is_protected(const ucn_frame_t *frame)
{
    return (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U;
}

/*
 * EN: Validates `frame_fields` before Wire frame codec state is used or changed.
 * 中文：在使用或修改 Wire 帧编解码 状态前验证 `frame_fields`。
 */
static ucn_result_t validate_frame_fields(
    const ucn_frame_t *frame,
    bool require_auth_tag,
    const ucn_wire_profile_descriptor_t **descriptor_out)
{
    const ucn_wire_profile_descriptor_t *descriptor;

    if (frame == NULL || descriptor_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    descriptor = resolve_profile(frame->wire_profile);
    if (descriptor == NULL || !flags_are_valid(frame->flags) ||
        (((frame->flags & UCN_FRAME_FLAG_ROUTE_EXTENSION) != 0U) !=
         frame->has_route_extension) ||
        (((frame->flags & UCN_FRAME_FLAG_PATH_ID) != 0U) !=
         frame->has_path_id) ||
        (frame->has_path_id &&
         (!frame->has_route_extension || frame->path_id == 0U)) ||
        frame->traffic_class > UCN_TRAFFIC_Q3_BULK || frame->hop_limit == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (frame->hop_limit > descriptor->max_hops || frame->network_id == 0U ||
        frame->network_id > descriptor->max_wire_value || frame->source == 0U ||
        frame->source > descriptor->max_node_id ||
        (frame->destination != UCN_NODE_BROADCAST &&
         (frame->destination == 0U ||
          frame->destination > descriptor->max_node_id)) ||
        frame->session_id > descriptor->max_wire_value ||
        (frame->has_route_extension &&
         (uint32_t)frame->route_epoch >
             max_for_width(descriptor->route_epoch_bytes)) ||
        (frame->has_path_id &&
         frame->path_id > max_for_width(descriptor->path_id_bytes)) ||
        (size_t)frame->payload_length >
            ucn_frame_max_payload_for_profile(descriptor->profile,
                                              frame->flags)) {
        return UCN_ERR_TOO_LARGE;
    }
    if (require_auth_tag && frame_is_protected(frame) && frame->auth_tag == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *descriptor_out = descriptor;
    return UCN_OK;
}

/*
 * EN: Writes `u16_be` in the canonical Wire frame codec byte order.
 * 中文：按规范的 Wire 帧编解码 字节序写入 `u16_be`。
 */
static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

/*
 * EN: Writes `u32_be` in the canonical Wire frame codec byte order.
 * 中文：按规范的 Wire 帧编解码 字节序写入 `u32_be`。
 */
static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

/*
 * EN: Reads `u16_be` from the canonical Wire frame codec byte order.
 * 中文：按规范的 Wire 帧编解码 字节序读取 `u16_be`。
 */
static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

/*
 * EN: Writes `e2e_aad` in the canonical Wire frame codec byte order.
 * 中文：按规范的 Wire 帧编解码 字节序写入 `e2e_aad`。
 */
ucn_result_t ucn_frame_write_e2e_aad(const ucn_frame_t *frame,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     size_t *output_length)
{
    const ucn_wire_profile_descriptor_t *descriptor;
    ucn_result_t result;

    if (frame == NULL || output == NULL || output_length == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (output_capacity < UCN_E2E_AAD_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    result = validate_frame_fields(frame, false, &descriptor);
    if (result != UCN_OK) {
        return result;
    }

    output[0] = version_profile_byte(descriptor);
    output[1] = frame->message_type;
    output[2] = (uint8_t)frame->traffic_class;
    output[3] = frame->flags &
                (UCN_FRAME_FLAG_E2E_PROTECTED | UCN_FRAME_FLAG_PATH_ID);
    write_u32_be(&output[4], frame->network_id);
    write_u32_be(&output[8], frame->source);
    write_u32_be(&output[12], frame->destination);
    write_u32_be(&output[16], frame->sequence);
    write_u32_be(&output[20], frame->session_id);
    write_u16_be(&output[24], frame->payload_length);
    write_u32_be(&output[26], frame->has_path_id ? frame->path_id : 0U);
    *output_length = UCN_E2E_AAD_BYTES;
    return UCN_OK;
}

/*
 * EN: Calculates the incremental CRC-16/CCITT value used by Core frames.
 * 中文：计算 Core 帧使用的可增量 CRC-16/CCITT 值。
 */
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

/*
 * EN: Calculates the bounded `crc_offset` value used by Wire frame codec.
 * 中文：计算 Wire 帧编解码 使用的有界 `crc_offset` 值。
 */
static size_t frame_crc_offset(size_t header_size)
{
    return header_size - sizeof(uint16_t);
}

/*
 * EN: Calculates `crc` with bounded, deterministic Wire frame codec arithmetic.
 * 中文：使用有界且确定性的 Wire 帧编解码 算术计算 `crc`。
 */
static uint16_t frame_crc(const uint8_t *header, size_t header_size,
                          const uint8_t *payload, uint16_t payload_length,
                          const uint8_t *auth_tag)
{
    uint16_t crc = ucn_crc16_ccitt(header, frame_crc_offset(header_size),
                                   UINT16_C(0xFFFF));

    crc = ucn_crc16_ccitt(payload, payload_length, crc);
    if (auth_tag != NULL) {
        crc = ucn_crc16_ccitt(auth_tag, UCN_E2E_TAG_SIZE, crc);
    }
    return crc;
}

/*
 * EN: Encodes `encoded_size` into its bounded Wire frame codec wire representation.
 * 中文：把 `encoded_size` 编码为有界的 Wire 帧编解码 线格式。
 */
size_t ucn_frame_encoded_size(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor;
    size_t total_size;

    if (validate_frame_fields(frame, true, &descriptor) != UCN_OK) {
        return 0U;
    }
    total_size = ucn_frame_header_size_for_profile(descriptor->profile,
                                                   frame->flags) +
                 (size_t)frame->payload_length +
                 (frame_is_protected(frame) ? UCN_E2E_TAG_SIZE : 0U);
    return total_size <= UCN_MAX_FRAME_BYTES ? total_size : 0U;
}

/*
 * EN: Encodes `encode` into its bounded Wire frame codec wire representation.
 * 中文：把 `encode` 编码为有界的 Wire 帧编解码 线格式。
 */
ucn_result_t ucn_frame_encode(const ucn_frame_t *frame,
                              uint8_t *output,
                              size_t output_capacity,
                              size_t *output_length)
{
    const ucn_wire_profile_descriptor_t *descriptor;
    ucn_result_t result;
    size_t total_length;
    size_t header_size;
    size_t cursor = UCN_OFFSET_VARIABLE;
    size_t crc_offset;
    uint32_t destination;
    uint16_t crc;

    if (frame == NULL || output == NULL || output_length == NULL ||
        (frame->payload_length != 0U && frame->payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    result = validate_frame_fields(frame, true, &descriptor);
    if (result != UCN_OK) {
        return result;
    }
    total_length = ucn_frame_encoded_size(frame);
    if (total_length == 0U || output_capacity < total_length) {
        return UCN_ERR_TOO_LARGE;
    }

    header_size = ucn_frame_header_size_for_profile(descriptor->profile,
                                                    frame->flags);
    crc_offset = frame_crc_offset(header_size);
    destination = frame->destination == UCN_NODE_BROADCAST ?
                      descriptor->max_wire_value : frame->destination;

    output[UCN_OFFSET_MAGIC_0] = UCN_FRAME_MAGIC_0;
    output[UCN_OFFSET_MAGIC_1] = UCN_FRAME_MAGIC_1;
    output[UCN_OFFSET_VERSION_PROFILE] = version_profile_byte(descriptor);
    output[UCN_OFFSET_MESSAGE_TYPE] = frame->message_type;
    output[UCN_OFFSET_TRAFFIC_FLAGS] =
        (uint8_t)(((uint8_t)frame->traffic_class << 6U) |
                  (frame->flags & UCN_WIRE_FLAGS_MASK));
    output[UCN_OFFSET_HOP_LIMIT] = frame->hop_limit;

    write_uint_be(&output[cursor], descriptor->address_bytes, frame->network_id);
    cursor += descriptor->address_bytes;
    write_uint_be(&output[cursor], descriptor->address_bytes, frame->source);
    cursor += descriptor->address_bytes;
    write_uint_be(&output[cursor], descriptor->address_bytes, destination);
    cursor += descriptor->address_bytes;
    write_u32_be(&output[cursor], frame->sequence);
    cursor += sizeof(uint32_t);
    write_uint_be(&output[cursor], descriptor->address_bytes, frame->session_id);
    cursor += descriptor->address_bytes;
    write_uint_be(&output[cursor], descriptor->payload_length_bytes,
                  frame->payload_length);
    cursor += descriptor->payload_length_bytes;
    if (frame->has_route_extension) {
        write_uint_be(&output[cursor], descriptor->route_epoch_bytes,
                      frame->route_epoch);
        cursor += descriptor->route_epoch_bytes;
    }
    if (frame->has_path_id) {
        write_uint_be(&output[cursor], descriptor->path_id_bytes,
                      frame->path_id);
        cursor += descriptor->path_id_bytes;
    }
    if (cursor != crc_offset) {
        return UCN_ERR_CONFIG;
    }

    if (frame->payload_length != 0U) {
        (void)memcpy(&output[header_size], frame->payload,
                     frame->payload_length);
    }
    if (frame_is_protected(frame)) {
        (void)memcpy(&output[header_size + frame->payload_length],
                     frame->auth_tag, UCN_E2E_TAG_SIZE);
    }

    crc = frame_crc(output, header_size, &output[header_size],
                    frame->payload_length,
                    frame_is_protected(frame) ?
                        &output[header_size + frame->payload_length] : NULL);
    write_u16_be(&output[crc_offset], crc);
    *output_length = total_length;
    return UCN_OK;
}

/*
 * EN: Inspects `wire_profile` without completing a full Wire frame codec decode.
 * 中文：在不完成完整 Wire 帧编解码 解码的情况下检查 `wire_profile`。
 */
ucn_result_t ucn_frame_peek_wire_profile(const uint8_t *input,
                                         size_t input_length,
                                         ucn_wire_profile_t *profile)
{
    ucn_wire_profile_t decoded_profile;

    if (input == NULL || profile == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length <= UCN_OFFSET_VERSION_PROFILE) {
        return UCN_ERR_MALFORMED;
    }
    if (input[UCN_OFFSET_MAGIC_0] != UCN_FRAME_MAGIC_0 ||
        input[UCN_OFFSET_MAGIC_1] != UCN_FRAME_MAGIC_1) {
        return UCN_ERR_MALFORMED;
    }
    if ((input[UCN_OFFSET_VERSION_PROFILE] & UCN_VERSION_MASK) !=
        UCN_PROTOCOL_VERSION) {
        return UCN_ERR_VERSION;
    }
    decoded_profile = profile_from_wire_code(
        (uint8_t)(input[UCN_OFFSET_VERSION_PROFILE] >> UCN_WIRE_PROFILE_SHIFT));
    if (ucn_wire_profile_get_descriptor(decoded_profile) == NULL) {
        return UCN_ERR_MALFORMED;
    }
    *profile = decoded_profile;
    return UCN_OK;
}

/*
 * EN: Inspects `encoded_size` without completing a full Wire frame codec decode.
 * 中文：在不完成完整 Wire 帧编解码 解码的情况下检查 `encoded_size`。
 */
ucn_result_t ucn_frame_peek_encoded_size(const uint8_t *input,
                                         size_t available_length,
                                         size_t *encoded_length)
{
    const ucn_wire_profile_descriptor_t *descriptor;
    ucn_wire_profile_t profile;
    uint8_t flags;
    size_t header_size;
    size_t payload_length_offset;
    uint32_t payload_length;
    size_t expected_length;
    ucn_result_t result;

    if (input == NULL || encoded_length == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *encoded_length = 0U;
    result = ucn_frame_peek_wire_profile(input, available_length, &profile);
    if (result != UCN_OK) {
        return result;
    }
    if (available_length < UCN_FRAME_W0_HEADER_SIZE) {
        return UCN_ERR_MALFORMED;
    }
    descriptor = ucn_wire_profile_get_descriptor(profile);
    flags = input[UCN_OFFSET_TRAFFIC_FLAGS] & UCN_WIRE_FLAGS_MASK;
    if (descriptor == NULL || !flags_are_valid(flags)) {
        return UCN_ERR_MALFORMED;
    }
    header_size = ucn_frame_header_size_for_profile(profile, flags);
    if (header_size == 0U || available_length < header_size) {
        return UCN_ERR_MALFORMED;
    }

    payload_length_offset = UCN_OFFSET_VARIABLE +
                            (size_t)4U * descriptor->address_bytes +
                            sizeof(uint32_t);
    payload_length = read_uint_be(&input[payload_length_offset],
                                  descriptor->payload_length_bytes);
    if (payload_length > UINT16_MAX ||
        (size_t)payload_length >
            ucn_frame_max_payload_for_profile(profile, flags)) {
        return UCN_ERR_TOO_LARGE;
    }
    expected_length = header_size + (size_t)payload_length +
        (((flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U) ?
             UCN_E2E_TAG_SIZE : 0U);
    if (expected_length > UCN_MAX_FRAME_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    if (expected_length > available_length) {
        return UCN_ERR_MALFORMED;
    }
    *encoded_length = expected_length;
    return UCN_OK;
}

/*
 * EN: Decodes and validates `decode` from its Wire frame codec wire representation.
 * 中文：从 Wire 帧编解码 线格式解码并验证 `decode`。
 */
ucn_result_t ucn_frame_decode(const uint8_t *input,
                              size_t input_length,
                              ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor;
    ucn_wire_profile_t profile;
    uint8_t flags;
    uint8_t traffic_class;
    size_t header_size;
    size_t cursor = UCN_OFFSET_VARIABLE;
    size_t crc_offset;
    size_t expected_length;
    uint32_t destination;
    uint32_t payload_length;
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (input == NULL || frame == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    {
        const ucn_result_t peek_result =
            ucn_frame_peek_wire_profile(input, input_length, &profile);

        if (peek_result != UCN_OK) {
            return peek_result;
        }
    }
    if (input_length < UCN_FRAME_W0_HEADER_SIZE) {
        return UCN_ERR_MALFORMED;
    }
    descriptor = ucn_wire_profile_get_descriptor(profile);
    flags = input[UCN_OFFSET_TRAFFIC_FLAGS] & UCN_WIRE_FLAGS_MASK;
    traffic_class = (uint8_t)(input[UCN_OFFSET_TRAFFIC_FLAGS] >> 6U);
    if (descriptor == NULL || !flags_are_valid(flags)) {
        return UCN_ERR_MALFORMED;
    }
    header_size = ucn_frame_header_size_for_profile(profile, flags);
    if (header_size == 0U || input_length < header_size) {
        return UCN_ERR_MALFORMED;
    }

    (void)memset(frame, 0, sizeof(*frame));
    frame->message_type = input[UCN_OFFSET_MESSAGE_TYPE];
    frame->wire_profile = profile;
    frame->traffic_class = (ucn_traffic_class_t)traffic_class;
    frame->flags = flags;
    frame->hop_limit = input[UCN_OFFSET_HOP_LIMIT];
    frame->network_id = read_uint_be(&input[cursor], descriptor->address_bytes);
    cursor += descriptor->address_bytes;
    frame->source = read_uint_be(&input[cursor], descriptor->address_bytes);
    cursor += descriptor->address_bytes;
    destination = read_uint_be(&input[cursor], descriptor->address_bytes);
    cursor += descriptor->address_bytes;
    frame->destination = destination == descriptor->max_wire_value ?
                             UCN_NODE_BROADCAST : destination;
    frame->sequence = read_uint_be(&input[cursor], 4U);
    cursor += sizeof(uint32_t);
    frame->session_id = read_uint_be(&input[cursor], descriptor->address_bytes);
    cursor += descriptor->address_bytes;
    payload_length = read_uint_be(&input[cursor],
                                  descriptor->payload_length_bytes);
    cursor += descriptor->payload_length_bytes;
    frame->has_route_extension =
        (flags & UCN_FRAME_FLAG_ROUTE_EXTENSION) != 0U;
    if (frame->has_route_extension) {
        frame->route_epoch = (uint16_t)read_uint_be(
            &input[cursor], descriptor->route_epoch_bytes);
        cursor += descriptor->route_epoch_bytes;
    }
    frame->has_path_id = (flags & UCN_FRAME_FLAG_PATH_ID) != 0U;
    if (frame->has_path_id) {
        frame->path_id = read_uint_be(&input[cursor], descriptor->path_id_bytes);
        cursor += descriptor->path_id_bytes;
    }

    crc_offset = frame_crc_offset(header_size);
    if (cursor != crc_offset || payload_length > UINT16_MAX ||
        payload_length > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    frame->payload_length = (uint16_t)payload_length;
    expected_length = header_size + (size_t)frame->payload_length +
                      (frame_is_protected(frame) ? UCN_E2E_TAG_SIZE : 0U);
    if (expected_length > UCN_MAX_FRAME_BYTES || input_length != expected_length) {
        return UCN_ERR_MALFORMED;
    }

    expected_crc = read_u16_be(&input[crc_offset]);
    actual_crc = frame_crc(input, header_size, &input[header_size],
                           frame->payload_length,
                           frame_is_protected(frame) ?
                               &input[header_size + frame->payload_length] : NULL);
    if (actual_crc != expected_crc) {
        return UCN_ERR_CRC;
    }

    frame->payload = &input[header_size];
    frame->auth_tag = frame_is_protected(frame) ?
                          &input[header_size + frame->payload_length] : NULL;
    if (validate_frame_fields(frame, true, &descriptor) != UCN_OK) {
        return UCN_ERR_MALFORMED;
    }
    return UCN_OK;
}
