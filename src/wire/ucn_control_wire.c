#include "internal/ucn_control_wire.h"

#include "internal/ucn_checked.h"

#include <string.h>

static const uint16_t opcode_registry[] = {
    0x0001U, 0x0002U, 0x0003U, 0x0004U, 0x0005U, 0x0006U, 0x0007U,
    0x0008U, 0x0009U, 0x000AU, 0x0010U, 0x0011U, 0x0012U, 0x0013U,
    0x0014U, 0x0015U, 0x0016U, 0x0101U, 0x0102U, 0x0103U, 0x0104U,
    0x0105U, 0x0106U, 0x0107U, 0x0108U, 0x0201U, 0x0202U, 0x0203U, 0x0204U, 0x0205U, 0x0206U,
    0x0301U, 0x0302U, 0x0303U, 0x0304U, 0x0305U, 0x0310U, 0x0311U,
    0x0312U, 0x0313U, 0x0314U, 0x0315U, 0x0316U, 0x0317U, 0x0401U,
    0x0410U, 0x0411U, 0x0412U, 0x0413U, 0x0420U, 0x0421U, 0x0422U,
    0x0423U, 0x0424U, 0x0501U, 0x0502U, 0x0503U, 0x0504U, 0x0505U,
    0x0506U, 0x0601U, 0x0602U,
    0x0603U, 0x0604U, 0x0605U, 0x0701U, 0x0702U, 0x0703U, 0x0704U,
    0x0705U, 0x0710U, 0x0711U, 0x0712U, 0x0720U, 0x0721U, 0x0722U,
    0x0723U, 0x0730U, 0x0801U, 0x0802U, 0x0803U, 0x0804U, 0x0805U,
    0x0806U, 0x0807U, 0x0808U, 0x0809U, 0x080AU, 0x080BU, 0x080CU,
    0x080DU, 0x080EU, 0x080FU, 0x0810U, 0x0811U, 0x0901U, 0x0902U,
    0x0903U, 0x0904U, 0x0905U
};

UCN_STATIC_ASSERT(sizeof(opcode_registry) / sizeof(opcode_registry[0]) ==
                      100U,
                  protocol_opcode_registry_must_have_100_entries);

static void write_be(uint8_t *output, uint64_t value, uint8_t bytes);
static uint64_t read_be(const uint8_t *input, uint8_t bytes);

static void write_be(uint8_t *output, uint64_t value, uint8_t bytes)
{
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        uint8_t shift = (uint8_t)((bytes - index - 1U) * 8U);

        output[index] = (uint8_t)(value >> shift);
    }
}

static uint64_t read_be(const uint8_t *input, uint8_t bytes)
{
    uint64_t value = 0U;
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

/* EN: Validate structural proposal fields, never peer authorization.
 * 中文：只验证提案结构字段，不验证对端授权。 */
static bool parent_publication_valid(uint16_t opcode,
                                     const ucn_i_parent_publication_t *value)
{
    size_t index;
    bool digest_present = false;
    if (value == NULL || value->proposal_transaction == 0U ||
        value->parent_generation == 0U) {
        return false;
    }
    for (index = 0U; index < sizeof(value->context_digest); ++index) {
        digest_present = digest_present || value->context_digest[index] != 0U;
    }
    if (!digest_present) { return false; }
    switch (opcode) {
        case UCN_I_OPCODE_TRANSPORT_PARENT_PREPARE:
        case UCN_I_OPCODE_TRANSPORT_PARENT_COMMIT:
            return value->disposition == 0U;
        case UCN_I_OPCODE_TRANSPORT_PARENT_ACCEPT:
            return value->disposition == 1U || value->disposition == 2U;
        case UCN_I_OPCODE_TRANSPORT_PARENT_ABORT:
            return value->disposition != 0U;
        default: return false;
    }
}


/* EN: Encode the exact common 32 B body with failure-zero-write semantics.
 * 中文：编码精确共同32字节正文，失败不写回。 */
ucn_result_t ucn_i_parent_publication_encode(
    uint16_t opcode, const ucn_i_parent_publication_t *value,
    uint8_t output[UCN_I_PARENT_PUBLICATION_BYTES])
{
    uint8_t bytes[UCN_I_PARENT_PUBLICATION_BYTES] = {0};
    if (output == NULL || !parent_publication_valid(opcode, value) ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, sizeof(bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    write_be(bytes, value->proposal_transaction, 8U);
    write_be(bytes + 8U, value->parent_generation, 4U);
    memcpy(bytes + 12U, value->context_digest, 16U);
    bytes[28] = value->disposition;
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

/* EN: Reject lengths, reserved fields and overlapping input/output before publication.
 * 中文：发布前拒绝错误长度、保留位及输入输出重叠。 */
ucn_result_t ucn_i_parent_publication_decode(
    uint16_t opcode, const uint8_t *input, size_t input_bytes,
    ucn_i_parent_publication_t *value_out)
{
    ucn_i_parent_publication_t value;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, input_bytes, value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_bytes != UCN_I_PARENT_PUBLICATION_BYTES ||
        input[29] != 0U || input[30] != 0U || input[31] != 0U) {
        return UCN_ERR_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.proposal_transaction = read_be(input, 8U);
    value.parent_generation = (uint32_t)read_be(input + 8U, 4U);
    memcpy(value.context_digest, input + 12U, 16U);
    value.disposition = input[28];
    if (!parent_publication_valid(opcode, &value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

static uint32_t address_reserved_max(uint8_t width)
{
    return width == 4U ? UINT32_MAX :
                         (UINT32_C(1) << (width * 8U)) - UINT32_C(1);
}


bool ucn_i_protocol_opcode_valid(uint16_t opcode)
{
    size_t left = 0U;
    size_t right = sizeof(opcode_registry) / sizeof(opcode_registry[0]);

    while (left < right) {
        size_t middle = left + (right - left) / 2U;

        if (opcode_registry[middle] == opcode) {
            return true;
        }
        if (opcode_registry[middle] < opcode) {
            left = middle + 1U;
        } else {
            right = middle;
        }
    }
    return false;
}

ucn_result_t ucn_i_protocol_opcode_support(
    uint16_t opcode,
    ucn_i_protocol_opcode_support_t *support_out)
{
    ucn_i_protocol_opcode_support_t support;

    if (support_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_i_protocol_opcode_valid(opcode)) {
        return UCN_ERR_MALFORMED;
    }
    support = UCN_I_OPCODE_SUPPORT_REGISTERED;
    switch (opcode) {
        case UCN_I_OPCODE_BOOTSTRAP_HELLO:
        case UCN_I_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE:
        case UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE:
            support |= UCN_I_OPCODE_SUPPORT_BOOTSTRAP_TERMINAL;
            break;
        case UCN_I_OPCODE_IDENTITY_CHALLENGE:
        case UCN_I_OPCODE_IDENTITY_RESPONSE:
        case UCN_I_OPCODE_ADDRESS_OFFER:
        case UCN_I_OPCODE_DEVICE_COMMIT:
        case UCN_I_OPCODE_FINAL_COMMIT:
        case UCN_I_OPCODE_CAPABILITY_FRAGMENT:
            /* Structural codecs only; no generic relay or production JOIN handler.
             * 仅结构Codec；不开放通用转发或宣称生产JOIN分派已接通。 */
            break;
        case UCN_I_OPCODE_NEIGHBOR_HELLO:
        case UCN_I_OPCODE_NEIGHBOR_KEEPALIVE:
        case UCN_I_OPCODE_CAPABILITY_ADVERTISE:
        case UCN_I_OPCODE_CAPABILITY_SELECT:
        case UCN_I_OPCODE_CAPABILITY_ACK:
        case UCN_I_OPCODE_CAPABILITY_REVOKE:
        case UCN_I_OPCODE_CAPABILITY_QUERY:
        case UCN_I_OPCODE_ROUTE_RREQ:
        case UCN_I_OPCODE_ROUTE_RREP:
        case UCN_I_OPCODE_ROUTE_RERR:
        case UCN_I_OPCODE_DIAGNOSTIC_PING:
        case UCN_I_OPCODE_DIAGNOSTIC_PONG:
        case UCN_I_OPCODE_TRANSFER_SETUP:
        case UCN_I_OPCODE_TRANSFER_SETUP_ACK:
        case UCN_I_OPCODE_TRANSFER_ABORT:
        case UCN_I_OPCODE_TRANSFER_TERMINAL_RECEIPT:
        case UCN_I_OPCODE_OPERATION_QUERY:
        case UCN_I_OPCODE_OPERATION_RECEIPT:
        case UCN_I_OPCODE_OPERATION_CANCEL:
            support |= UCN_I_OPCODE_SUPPORT_RELAY |
                       UCN_I_OPCODE_SUPPORT_AUTH_TERMINAL;
            break;
        case UCN_I_OPCODE_ROUTE_FRAGMENT:
            /* Only the shared codec is implemented on C. No generic relay or
             * authenticated terminal dispatch before its bounded Root exists.
             * C侧仅共同Codec，不放开通用转发或终端分派。 */
            break;
        default:
            support |= UCN_I_OPCODE_SUPPORT_RELAY;
            break;
    }
    *support_out = support;
    return UCN_OK;
}

ucn_result_t ucn_i_diagnostic_echo_encode(
    const ucn_i_diagnostic_echo_t *value,
    uint8_t output[UCN_I_DIAGNOSTIC_ECHO_BYTES])
{
    if (value == NULL || output == NULL || value->nonce == 0U ||
        ucn_i_ranges_overlap(value, sizeof(*value), output,
                             UCN_I_DIAGNOSTIC_ECHO_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    write_be(output, value->nonce, UCN_I_DIAGNOSTIC_ECHO_BYTES);
    return UCN_OK;
}

ucn_result_t ucn_i_diagnostic_echo_decode(
    const uint8_t input[UCN_I_DIAGNOSTIC_ECHO_BYTES],
    ucn_i_diagnostic_echo_t *value_out)
{
    ucn_i_diagnostic_echo_t value;

    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_DIAGNOSTIC_ECHO_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    value.nonce = read_be(input, UCN_I_DIAGNOSTIC_ECHO_BYTES);
    if (value.nonce == 0U) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_protocol_opcode_domain(uint16_t opcode,
                                          uint8_t *domain_out)
{
    uint8_t domain;

    if (domain_out == NULL || !ucn_i_protocol_opcode_valid(opcode)) {
        return domain_out == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_MALFORMED;
    }
    domain = (uint8_t)(opcode >> 8U);
    if (domain > UCN_I_OPCODE_DIAGNOSTICS) {
        return UCN_ERR_MALFORMED;
    }
    *domain_out = domain;
    return UCN_OK;
}

ucn_result_t ucn_i_c0_header_bytes(uint8_t address_width,
                                   size_t *header_bytes_out)
{
    if (header_bytes_out == NULL || address_width == 0U ||
        address_width > 4U) {
        return UCN_ERR_ARGUMENT;
    }
    *header_bytes_out = UCN_I_C0_FIXED_EXCLUDING_ADDRESSES +
                        2U * address_width;
    return UCN_OK;
}

static ucn_result_t trailer_bytes(uint8_t origin_security,
                                  uint8_t hop_profile,
                                  size_t *bytes_out)
{
    size_t bytes = 0U;

    if (bytes_out == NULL || origin_security > UCN_I_ORIGIN_O2 ||
        hop_profile > UCN_I_CONTROL_H1) {
        return UCN_ERR_ARGUMENT;
    }
    if (origin_security != UCN_I_ORIGIN_O0) {
        bytes += UCN_I_C0_ORIGIN_TAG_BYTES;
    }
    if (hop_profile == UCN_I_CONTROL_H1) {
        bytes += UCN_I_C0_H1_TRAILER_BYTES;
    }
    *bytes_out = bytes;
    return UCN_OK;
}

ucn_result_t ucn_i_c0_encoded_size(uint8_t address_width,
                                   uint8_t origin_security,
                                   uint8_t hop_profile,
                                   size_t payload_bytes,
                                   size_t *encoded_bytes_out)
{
    size_t header;
    size_t trailer;
    size_t total;
    ucn_result_t result;

    if (encoded_bytes_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_c0_header_bytes(address_width, &header);
    if (result != UCN_OK) {
        return result;
    }
    result = trailer_bytes(origin_security, hop_profile, &trailer);
    if (result != UCN_OK ||
        ucn_i_size_add(header, payload_bytes, &total) != UCN_OK ||
        ucn_i_size_add(total, trailer, &total) != UCN_OK) {
        return result == UCN_OK ? UCN_ERR_EXHAUSTED : result;
    }
    *encoded_bytes_out = total;
    return UCN_OK;
}

static bool opcode_kind_matches(uint8_t kind, uint16_t opcode)
{
    bool diagnostic = (opcode >> 8U) == UCN_I_OPCODE_DIAGNOSTICS;

    return (kind == UCN_I_PAYLOAD_CONTROL && !diagnostic) ||
           (kind == UCN_I_PAYLOAD_DIAGNOSTIC && diagnostic);
}

static bool frame_shape_valid(const ucn_i_c0_frame_t *frame, uint8_t width,
                        uint8_t hop_profile)
{
    bool unbound;
    uint32_t limit;

    if (width == 0U || width > 4U || frame == NULL ||
        (frame->payload == NULL && frame->payload_bytes != 0U) ||
        frame->realm_id == 0U || frame->transaction_id == 0U ||
        frame->delivery_guarantee == UCN_DELIVERY_LATEST ||
        frame->delivery_guarantee > UCN_DELIVERY_RELIABLE ||
        frame->interaction_role > UCN_INTERACTION_ERROR ||
        frame->traffic_class >= UCN_TRAFFIC_CLASS_COUNT ||
        frame->hop_limit == 0U || frame->hop_limit > 63U ||
        frame->origin_security > UCN_I_ORIGIN_O2 ||
        hop_profile > UCN_I_CONTROL_H1 ||
        !opcode_kind_matches(frame->payload_kind, frame->opcode)) {
        return false;
    }
    if (frame->origin_security == UCN_I_ORIGIN_O0) {
        if (frame->origin_tag != NULL) {
            return false;
        }
    } else if (frame->origin_tag == NULL) {
        return false;
    }
    if (hop_profile == UCN_I_CONTROL_H0) {
        if (frame->hop_sequence != 0U || frame->hop_tag != NULL) {
            return false;
        }
    } else if (frame->hop_sequence == 0U || frame->hop_tag == NULL) {
        return false;
    }
    unbound = frame->source_binding_generation == 0U &&
              frame->destination_binding_generation == 0U;
    limit = address_reserved_max(width);
    if (unbound) {
        return frame->origin_security == UCN_I_ORIGIN_O0 &&
               hop_profile == UCN_I_CONTROL_H0 && frame->hop_limit == 1U &&
               (frame->opcode >> 8U) == UCN_I_OPCODE_IDENTITY_SECURITY &&
               frame->source_address == 0U &&
               frame->destination_address == limit;
    }
    return frame->source_binding_generation != 0U &&
           frame->destination_binding_generation != 0U &&
           frame->source_address != 0U && frame->source_address < limit &&
           frame->destination_address != 0U && frame->destination_address < limit;
}

static void encode_header(const ucn_i_c0_frame_t *frame, uint8_t width,
                          uint8_t *output)
{
    size_t offset = UCN_I_C0_COMMON_BYTES;

    output[0] = (uint8_t)(UCN_I_WIRE_PROTOCOL_VERSION << 4U);
    output[1] = (uint8_t)((frame->traffic_class << 6U) |
                          (frame->delivery_guarantee << 4U) |
                          (frame->interaction_role << 2U) |
                          frame->payload_kind);
    output[2] = (uint8_t)((frame->origin_security << 6U) |
                          frame->hop_limit);
    write_be(output + offset, frame->realm_id, 4U);
    offset += 4U;
    write_be(output + offset, frame->source_address, width);
    offset += width;
    write_be(output + offset, frame->destination_address, width);
    offset += width;
    write_be(output + offset, frame->source_binding_generation, 4U);
    offset += 4U;
    write_be(output + offset, frame->destination_binding_generation, 4U);
    offset += 4U;
    write_be(output + offset, frame->transaction_id, 8U);
    offset += 8U;
    write_be(output + offset, frame->opcode, 2U);
}

ucn_result_t ucn_i_c0_encode(const ucn_i_c0_frame_t *frame,
                             uint8_t address_width,
                             uint8_t hop_profile,
                             uint8_t *output,
                             size_t output_capacity,
                             size_t *output_bytes)
{
    size_t header;
    size_t total;
    size_t offset;
    ucn_result_t result;

    if (output == NULL || output_bytes == NULL ||
        !frame_shape_valid(frame, address_width, hop_profile) ||
        !ucn_i_protocol_opcode_valid(frame->opcode) ||
        ucn_i_ranges_overlap(frame, sizeof(*frame), output, output_capacity) ||
        ucn_i_ranges_overlap(frame, sizeof(*frame), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                             sizeof(*output_bytes)) ||
        (frame->payload_bytes != 0U &&
         (ucn_i_ranges_overlap(frame->payload, frame->payload_bytes, output,
                               output_capacity) ||
          ucn_i_ranges_overlap(frame->payload, frame->payload_bytes,
                               output_bytes, sizeof(*output_bytes)))) ||
        (frame->origin_tag != NULL &&
         (ucn_i_ranges_overlap(frame->origin_tag,
                               UCN_I_C0_ORIGIN_TAG_BYTES, output,
                               output_capacity) ||
          ucn_i_ranges_overlap(frame->origin_tag,
                               UCN_I_C0_ORIGIN_TAG_BYTES, output_bytes,
                               sizeof(*output_bytes)))) ||
        (frame->hop_tag != NULL &&
         (ucn_i_ranges_overlap(frame->hop_tag, 12U, output,
                               output_capacity) ||
          ucn_i_ranges_overlap(frame->hop_tag, 12U, output_bytes,
                               sizeof(*output_bytes))))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_c0_header_bytes(address_width, &header);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_c0_encoded_size(address_width, frame->origin_security,
                                   hop_profile, frame->payload_bytes, &total);
    if (result != UCN_OK || total > output_capacity) {
        return result == UCN_OK ? UCN_ERR_NO_SPACE : result;
    }
    encode_header(frame, address_width, output);
    offset = header;
    if (frame->payload_bytes != 0U) {
        memcpy(output + offset, frame->payload, frame->payload_bytes);
        offset += frame->payload_bytes;
    }
    if (frame->origin_security != UCN_I_ORIGIN_O0) {
        memcpy(output + offset, frame->origin_tag, UCN_I_C0_ORIGIN_TAG_BYTES);
        offset += UCN_I_C0_ORIGIN_TAG_BYTES;
    }
    if (hop_profile == UCN_I_CONTROL_H1) {
        write_be(output + offset, frame->hop_sequence, 4U);
        offset += 4U;
        memcpy(output + offset, frame->hop_tag, 12U);
    }
    *output_bytes = total;
    return UCN_OK;
}

static void decode_header(const uint8_t *input, uint8_t address_width,
                          ucn_i_c0_frame_t *decoded)
{
    size_t offset;
    decoded->traffic_class = (uint8_t)(input[1] >> 6U);
    decoded->delivery_guarantee = (uint8_t)((input[1] >> 4U) & 3U);
    decoded->interaction_role = (uint8_t)((input[1] >> 2U) & 3U);
    decoded->payload_kind = (uint8_t)(input[1] & 3U);
    decoded->origin_security = (uint8_t)(input[2] >> 6U);
    decoded->hop_limit = (uint8_t)(input[2] & 0x3FU);
    offset = UCN_I_C0_COMMON_BYTES;
    decoded->realm_id = ((uint32_t)input[offset] << 24U) |
        ((uint32_t)input[offset+1U] << 16U) |
        ((uint32_t)input[offset+2U] << 8U) | input[offset+3U];
    offset += 4U;
    decoded->source_address = (uint32_t)read_be(input + offset, address_width);
    offset += address_width;
    decoded->destination_address =
        (uint32_t)read_be(input + offset, address_width);
    offset += address_width;
    decoded->source_binding_generation = ((uint32_t)input[offset] << 24U) |
        ((uint32_t)input[offset+1U] << 16U) |
        ((uint32_t)input[offset+2U] << 8U) | input[offset+3U];
    offset += 4U;
    decoded->destination_binding_generation = ((uint32_t)input[offset] << 24U) |
        ((uint32_t)input[offset+1U] << 16U) |
        ((uint32_t)input[offset+2U] << 8U) | input[offset+3U];
    offset += 4U;
    decoded->transaction_id = ((uint64_t)input[offset] << 56U) |
        ((uint64_t)input[offset+1U] << 48U) |
        ((uint64_t)input[offset+2U] << 40U) |
        ((uint64_t)input[offset+3U] << 32U) |
        ((uint64_t)input[offset+4U] << 24U) |
        ((uint64_t)input[offset+5U] << 16U) |
        ((uint64_t)input[offset+6U] << 8U) | input[offset+7U];
    offset += 8U;
    decoded->opcode = (uint16_t)(((uint16_t)input[offset] << 8U) |
                                input[offset+1U]);
}

ucn_result_t ucn_i_c0_decode(const uint8_t *input,
                             size_t input_bytes,
                             uint8_t address_width,
                             uint8_t hop_profile,
                             ucn_i_c0_frame_t *frame_out)
{
    ucn_i_c0_frame_t decoded;
    size_t header;
    size_t trailer;
    size_t offset;
    if (input == NULL || frame_out == NULL ||
        ucn_i_ranges_overlap(input, input_bytes, frame_out,
                             sizeof(*frame_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (address_width == 0U || address_width > 4U)
        return UCN_ERR_ARGUMENT;
    header = UCN_I_C0_FIXED_EXCLUDING_ADDRESSES + 2U * address_width;
    if (input_bytes < header ||
        (input[0] >> 4U) != UCN_I_WIRE_PROTOCOL_VERSION ||
        (input[0] & 0x0FU) != UCN_I_WIRE_CONTRACT_C0) {
        return UCN_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decode_header(input, address_width, &decoded);
    if (decoded.origin_security > UCN_I_ORIGIN_O2 || hop_profile > UCN_I_CONTROL_H1)
        return UCN_ERR_UNSUPPORTED;
    trailer = (decoded.origin_security == UCN_I_ORIGIN_O0 ? 0U :
                   UCN_I_C0_ORIGIN_TAG_BYTES) +
              (hop_profile == UCN_I_CONTROL_H0 ? 0U : UCN_I_C0_H1_TRAILER_BYTES);
    if (input_bytes < header + trailer) return UCN_ERR_MALFORMED;
    decoded.payload = input + header;
    decoded.payload_bytes = input_bytes - header - trailer;
    offset = header + decoded.payload_bytes;
    if (decoded.origin_security != UCN_I_ORIGIN_O0) {
        decoded.origin_tag = input + offset;
        offset += UCN_I_C0_ORIGIN_TAG_BYTES;
    }
    if (hop_profile == UCN_I_CONTROL_H1) {
        decoded.hop_sequence = (uint32_t)read_be(input + offset, 4U);
        decoded.hop_tag = input + offset + 4U;
    }
    if (!ucn_i_protocol_opcode_valid(decoded.opcode) ||
        !frame_shape_valid(&decoded, address_width, hop_profile)) {
        return UCN_ERR_MALFORMED;
    }
    *frame_out = decoded;
    return UCN_OK;
}

ucn_result_t ucn_i_c0_origin_aad_build(
    const ucn_i_c0_frame_t *frame,
    uint8_t address_width,
    uint8_t output[UCN_I_C0_ORIGIN_AAD_MAX_BYTES],
    size_t *output_bytes)
{
    static const uint8_t domain[] = "UCN6-ORIGIN-V1";
    uint8_t identity[30U];
    size_t identity_bytes;
    size_t offset = 0U;

    if (frame == NULL || output == NULL || output_bytes == NULL ||
        address_width == 0U || address_width > 4U ||
        !frame_shape_valid(frame, address_width,
                     frame->hop_tag == NULL ? UCN_I_CONTROL_H0 :
                                               UCN_I_CONTROL_H1) ||
        !ucn_i_protocol_opcode_valid(frame->opcode) ||
        ucn_i_ranges_overlap(frame, sizeof(*frame), output,
                             UCN_I_C0_ORIGIN_AAD_MAX_BYTES) ||
        ucn_i_ranges_overlap(frame, sizeof(*frame), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output, UCN_I_C0_ORIGIN_AAD_MAX_BYTES,
                             output_bytes, sizeof(*output_bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    identity_bytes = 22U + 2U * address_width;
    write_be(identity, frame->realm_id, 4U);
    write_be(identity + 4U, frame->source_address, address_width);
    write_be(identity + 4U + address_width, frame->destination_address,
             address_width);
    write_be(identity + 4U + 2U * address_width,
             frame->source_binding_generation, 4U);
    write_be(identity + 8U + 2U * address_width,
             frame->destination_binding_generation, 4U);
    write_be(identity + 12U + 2U * address_width,
             frame->transaction_id, 8U);
    write_be(identity + 20U + 2U * address_width, frame->opcode, 2U);
    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    write_be(output + offset, 3U + 1U + identity_bytes + 4U, 4U);
    offset += 4U;
    output[offset++] = (uint8_t)(UCN_I_WIRE_PROTOCOL_VERSION << 4U);
    output[offset++] = (uint8_t)((frame->traffic_class << 6U) |
                                 (frame->delivery_guarantee << 4U) |
                                 (frame->interaction_role << 2U) |
                                 frame->payload_kind);
    output[offset++] = (uint8_t)(frame->origin_security << 6U);
    output[offset++] = UCN_I_WIRE_CONTRACT_C0;
    memcpy(output + offset, identity, identity_bytes);
    offset += identity_bytes;
    write_be(output + offset, frame->payload_bytes, 4U);
    offset += 4U;
    *output_bytes = offset;
    return UCN_OK;
}
