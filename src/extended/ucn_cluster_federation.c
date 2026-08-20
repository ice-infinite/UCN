#include "ucn/ucn_cluster_federation.h"

#include <string.h>

#include "ucn/ucn_time.h"

#define FED_VERSION_OFFSET ((size_t)0U)
#define FED_KIND_OFFSET ((size_t)1U)
#define FED_FLAGS_OFFSET ((size_t)2U)
#define FED_HOP_LIMIT_OFFSET ((size_t)3U)
#define FED_TRANSACTION_OFFSET ((size_t)4U)
#define FED_BODY_OFFSET UCN_CLUSTER_FEDERATION_COMMON_HEADER_BYTES

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

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

static bool node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool kind_is_valid(ucn_cluster_federation_kind_t kind)
{
    return (kind >= UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER &&
            kind <= UCN_CLUSTER_FED_KIND_TUNNEL_ERROR) ||
           kind == UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER;
}

static bool kind_is_reserved(ucn_cluster_federation_kind_t kind)
{
    return kind == UCN_CLUSTER_FED_KIND_NEXT_CLUSTER_ANNOUNCE ||
           kind == UCN_CLUSTER_FED_KIND_NEXT_CLUSTER_WITHDRAW;
}

static bool error_is_valid(ucn_cluster_federation_error_t error)
{
    return error >= UCN_CLUSTER_FED_ERROR_DIRECTORY_NOT_FOUND &&
           error <= UCN_CLUSTER_FED_ERROR_TIMEOUT;
}

static bool traffic_class_is_valid(ucn_traffic_class_t traffic_class)
{
    return traffic_class == UCN_TRAFFIC_Q0_CRITICAL ||
           traffic_class == UCN_TRAFFIC_Q1_REALTIME;
}

static bool tunnel_payload_is_valid(const uint8_t *payload,
                                    uint16_t length,
                                    size_t fixed_header_bytes)
{
    return fixed_header_bytes <= UCN_MAX_PAYLOAD_BYTES &&
           (length == 0U || payload != NULL) &&
           (size_t)length <= UCN_MAX_PAYLOAD_BYTES - fixed_header_bytes;
}

static bool locator_is_valid(const ucn_cluster_locator_t *locator,
                             bool withdrawal)
{
    if (locator == NULL || !node_id_is_valid(locator->node_id) ||
        locator->cluster_id == 0U || !node_id_is_valid(locator->head_node_id) ||
        locator->term == 0U || locator->record_nonce == 0U) {
        return false;
    }
    return withdrawal ? locator->lease_ms == 0U :
                        ucn_duration_is_valid(locator->lease_ms);
}

static bool handover_is_valid(const ucn_cluster_federation_handover_t *handover)
{
    return handover != NULL && handover->cluster_id != 0U &&
           node_id_is_valid(handover->new_head_node_id) &&
           handover->new_term != 0U;
}

static bool message_is_valid(const ucn_cluster_federation_message_t *message)
{
    if (message == NULL || !kind_is_valid(message->kind) || message->flags != 0U ||
        message->hop_limit == 0U || message->hop_limit > UCN_MAX_HOPS ||
        message->transaction_id == 0U) {
        return false;
    }
    switch (message->kind) {
        case UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER:
        case UCN_CLUSTER_FED_KIND_LOCATOR_REPLY:
            return locator_is_valid(&message->body.locator, false);
        case UCN_CLUSTER_FED_KIND_LOCATOR_WITHDRAW:
            return locator_is_valid(&message->body.locator, true);
        case UCN_CLUSTER_FED_KIND_LOCATOR_QUERY:
            return node_id_is_valid(message->body.query.target_node_id) &&
                   message->body.query.requester_cluster_id != 0U &&
                   node_id_is_valid(message->body.query.requester_head_node_id);
        case UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT:
            return node_id_is_valid(message->body.submit.final_node_id) &&
                   ucn_endpoint_is_static(message->body.submit.endpoint) &&
                   traffic_class_is_valid(message->body.submit.traffic_class) &&
                   tunnel_payload_is_valid(
                       message->body.submit.inner_payload,
                       message->body.submit.inner_length,
                       UCN_CLUSTER_FEDERATION_SUBMIT_HEADER_BYTES);
        case UCN_CLUSTER_FED_KIND_TUNNEL_DATA:
            return node_id_is_valid(message->body.tunnel.origin_node_id) &&
                   node_id_is_valid(message->body.tunnel.final_node_id) &&
                   message->body.tunnel.origin_cluster_id != 0U &&
                   message->body.tunnel.destination_cluster_id != 0U &&
                   ucn_endpoint_is_static(message->body.tunnel.endpoint) &&
                   traffic_class_is_valid(message->body.tunnel.traffic_class) &&
                   tunnel_payload_is_valid(
                       message->body.tunnel.inner_payload,
                       message->body.tunnel.inner_length,
                       UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES);
        case UCN_CLUSTER_FED_KIND_TUNNEL_DELIVER:
            return node_id_is_valid(message->body.delivery.origin_node_id) &&
                   node_id_is_valid(message->body.delivery.final_node_id) &&
                   message->body.delivery.origin_cluster_id != 0U &&
                   message->body.delivery.destination_cluster_id != 0U &&
                   ucn_endpoint_is_static(message->body.delivery.endpoint) &&
                   traffic_class_is_valid(message->body.delivery.traffic_class) &&
                   tunnel_payload_is_valid(
                       message->body.delivery.inner_payload,
                       message->body.delivery.inner_length,
                       UCN_CLUSTER_FEDERATION_DELIVER_HEADER_BYTES);
        case UCN_CLUSTER_FED_KIND_TUNNEL_ERROR:
            return error_is_valid(message->body.error.error) &&
                   node_id_is_valid(message->body.error.origin_node_id) &&
                   node_id_is_valid(message->body.error.final_node_id);
        case UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER:
            return handover_is_valid(&message->body.handover);
        case UCN_CLUSTER_FED_KIND_NEXT_CLUSTER_ANNOUNCE:
        case UCN_CLUSTER_FED_KIND_NEXT_CLUSTER_WITHDRAW:
        default:
            return false;
    }
}

size_t ucn_cluster_federation_message_encoded_size(
    const ucn_cluster_federation_message_t *message)
{
    if (!message_is_valid(message)) {
        return 0U;
    }
    switch (message->kind) {
        case UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER:
        case UCN_CLUSTER_FED_KIND_LOCATOR_WITHDRAW:
        case UCN_CLUSTER_FED_KIND_LOCATOR_REPLY:
            return UCN_CLUSTER_FEDERATION_LOCATOR_BYTES;
        case UCN_CLUSTER_FED_KIND_LOCATOR_QUERY:
            return UCN_CLUSTER_FEDERATION_QUERY_BYTES;
        case UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT:
            return UCN_CLUSTER_FEDERATION_SUBMIT_HEADER_BYTES +
                   message->body.submit.inner_length;
        case UCN_CLUSTER_FED_KIND_TUNNEL_DATA:
            return UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES +
                   message->body.tunnel.inner_length;
        case UCN_CLUSTER_FED_KIND_TUNNEL_DELIVER:
            return UCN_CLUSTER_FEDERATION_DELIVER_HEADER_BYTES +
                   message->body.delivery.inner_length;
        case UCN_CLUSTER_FED_KIND_TUNNEL_ERROR:
            return UCN_CLUSTER_FEDERATION_ERROR_BYTES;
        case UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER:
            return UCN_CLUSTER_FEDERATION_HANDOVER_BYTES;
        default:
            return 0U;
    }
}

static void encode_common(const ucn_cluster_federation_message_t *message,
                          uint8_t *output)
{
    output[FED_VERSION_OFFSET] = UCN_CLUSTER_FEDERATION_FORMAT_VERSION;
    output[FED_KIND_OFFSET] = (uint8_t)message->kind;
    output[FED_FLAGS_OFFSET] = message->flags;
    output[FED_HOP_LIMIT_OFFSET] = message->hop_limit;
    write_u32_be(output + FED_TRANSACTION_OFFSET, message->transaction_id);
}

static void encode_locator(const ucn_cluster_locator_t *locator, uint8_t *output)
{
    write_u32_be(output + FED_BODY_OFFSET, locator->node_id);
    write_u32_be(output + FED_BODY_OFFSET + 4U, locator->cluster_id);
    write_u32_be(output + FED_BODY_OFFSET + 8U, locator->head_node_id);
    write_u32_be(output + FED_BODY_OFFSET + 12U, locator->term);
    write_u32_be(output + FED_BODY_OFFSET + 16U, locator->lease_ms);
    write_u32_be(output + FED_BODY_OFFSET + 20U, locator->record_nonce);
}

ucn_result_t ucn_cluster_federation_message_encode(
    const ucn_cluster_federation_message_t *message,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    size_t encoded_size = ucn_cluster_federation_message_encoded_size(message);

    if (output_length != NULL) {
        *output_length = 0U;
    }
    if (encoded_size == 0U || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (output_capacity < encoded_size) {
        return UCN_ERR_TOO_LARGE;
    }
    (void)memset(output, 0, encoded_size);
    encode_common(message, output);
    switch (message->kind) {
        case UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER:
        case UCN_CLUSTER_FED_KIND_LOCATOR_WITHDRAW:
        case UCN_CLUSTER_FED_KIND_LOCATOR_REPLY:
            encode_locator(&message->body.locator, output);
            break;
        case UCN_CLUSTER_FED_KIND_LOCATOR_QUERY:
            write_u32_be(output + FED_BODY_OFFSET,
                         message->body.query.target_node_id);
            write_u32_be(output + FED_BODY_OFFSET + 4U,
                         message->body.query.requester_cluster_id);
            write_u32_be(output + FED_BODY_OFFSET + 8U,
                         message->body.query.requester_head_node_id);
            break;
        case UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT:
            write_u32_be(output + FED_BODY_OFFSET,
                         message->body.submit.final_node_id);
            output[FED_BODY_OFFSET + 4U] = message->body.submit.endpoint;
            output[FED_BODY_OFFSET + 5U] =
                (uint8_t)message->body.submit.traffic_class;
            write_u16_be(output + FED_BODY_OFFSET + 6U,
                         message->body.submit.inner_length);
            if (message->body.submit.inner_length != 0U) {
                (void)memcpy(output + UCN_CLUSTER_FEDERATION_SUBMIT_HEADER_BYTES,
                             message->body.submit.inner_payload,
                             message->body.submit.inner_length);
            }
            break;
        case UCN_CLUSTER_FED_KIND_TUNNEL_DATA:
            write_u32_be(output + FED_BODY_OFFSET,
                         message->body.tunnel.origin_node_id);
            write_u32_be(output + FED_BODY_OFFSET + 4U,
                         message->body.tunnel.final_node_id);
            write_u32_be(output + FED_BODY_OFFSET + 8U,
                         message->body.tunnel.origin_cluster_id);
            write_u32_be(output + FED_BODY_OFFSET + 12U,
                         message->body.tunnel.destination_cluster_id);
            output[FED_BODY_OFFSET + 16U] = message->body.tunnel.endpoint;
            output[FED_BODY_OFFSET + 17U] =
                (uint8_t)message->body.tunnel.traffic_class;
            write_u16_be(output + FED_BODY_OFFSET + 18U,
                         message->body.tunnel.inner_length);
            if (message->body.tunnel.inner_length != 0U) {
                (void)memcpy(output + UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES,
                             message->body.tunnel.inner_payload,
                             message->body.tunnel.inner_length);
            }
            break;
        case UCN_CLUSTER_FED_KIND_TUNNEL_DELIVER:
            write_u32_be(output + FED_BODY_OFFSET,
                         message->body.delivery.origin_node_id);
            write_u32_be(output + FED_BODY_OFFSET + 4U,
                         message->body.delivery.final_node_id);
            write_u32_be(output + FED_BODY_OFFSET + 8U,
                         message->body.delivery.origin_cluster_id);
            write_u32_be(output + FED_BODY_OFFSET + 12U,
                         message->body.delivery.destination_cluster_id);
            output[FED_BODY_OFFSET + 16U] = message->body.delivery.endpoint;
            output[FED_BODY_OFFSET + 17U] =
                (uint8_t)message->body.delivery.traffic_class;
            write_u16_be(output + FED_BODY_OFFSET + 18U,
                         message->body.delivery.inner_length);
            if (message->body.delivery.inner_length != 0U) {
                (void)memcpy(output + UCN_CLUSTER_FEDERATION_DELIVER_HEADER_BYTES,
                             message->body.delivery.inner_payload,
                             message->body.delivery.inner_length);
            }
            break;
        case UCN_CLUSTER_FED_KIND_TUNNEL_ERROR:
            write_u16_be(output + FED_BODY_OFFSET,
                         (uint16_t)message->body.error.error);
            write_u32_be(output + FED_BODY_OFFSET + 4U,
                         message->body.error.origin_node_id);
            write_u32_be(output + FED_BODY_OFFSET + 8U,
                         message->body.error.final_node_id);
            break;
        case UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER:
            write_u32_be(output + FED_BODY_OFFSET,
                         message->body.handover.cluster_id);
            write_u32_be(output + FED_BODY_OFFSET + 4U,
                         message->body.handover.new_head_node_id);
            write_u32_be(output + FED_BODY_OFFSET + 8U,
                         message->body.handover.new_term);
            write_u32_be(output + FED_BODY_OFFSET + 12U,
                         message->body.handover.backup_generation);
            (void)memcpy(output + FED_BODY_OFFSET + 16U,
                         message->body.handover.proof,
                         UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES);
            break;
        default:
            return UCN_ERR_ARGUMENT;
    }
    if (output_length != NULL) {
        *output_length = encoded_size;
    }
    return UCN_OK;
}

static ucn_result_t decode_common(const uint8_t *input,
                                  size_t input_length,
                                  ucn_cluster_federation_message_t *message)
{
    if (input_length < UCN_CLUSTER_FEDERATION_COMMON_HEADER_BYTES ||
        input[FED_VERSION_OFFSET] != UCN_CLUSTER_FEDERATION_FORMAT_VERSION ||
        input[FED_FLAGS_OFFSET] != 0U) {
        return UCN_ERR_MALFORMED;
    }
    message->kind = (ucn_cluster_federation_kind_t)input[FED_KIND_OFFSET];
    message->flags = input[FED_FLAGS_OFFSET];
    message->hop_limit = input[FED_HOP_LIMIT_OFFSET];
    message->transaction_id = read_u32_be(input + FED_TRANSACTION_OFFSET);
    return (kind_is_valid(message->kind) || kind_is_reserved(message->kind)) ?
               UCN_OK :
               UCN_ERR_MALFORMED;
}

static void decode_locator(const uint8_t *input, ucn_cluster_locator_t *locator)
{
    locator->node_id = read_u32_be(input + FED_BODY_OFFSET);
    locator->cluster_id = read_u32_be(input + FED_BODY_OFFSET + 4U);
    locator->head_node_id = read_u32_be(input + FED_BODY_OFFSET + 8U);
    locator->term = read_u32_be(input + FED_BODY_OFFSET + 12U);
    locator->lease_ms = read_u32_be(input + FED_BODY_OFFSET + 16U);
    locator->record_nonce = read_u32_be(input + FED_BODY_OFFSET + 20U);
}

ucn_result_t ucn_cluster_federation_message_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_federation_message_t *message)
{
    ucn_result_t result;
    uint16_t inner_length;

    if (input == NULL || message == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(message, 0, sizeof(*message));
    result = decode_common(input, input_length, message);
    if (result != UCN_OK) {
        return result;
    }
    switch (message->kind) {
        case UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER:
        case UCN_CLUSTER_FED_KIND_LOCATOR_WITHDRAW:
        case UCN_CLUSTER_FED_KIND_LOCATOR_REPLY:
            if (input_length != UCN_CLUSTER_FEDERATION_LOCATOR_BYTES) {
                return UCN_ERR_MALFORMED;
            }
            decode_locator(input, &message->body.locator);
            break;
        case UCN_CLUSTER_FED_KIND_LOCATOR_QUERY:
            if (input_length != UCN_CLUSTER_FEDERATION_QUERY_BYTES) {
                return UCN_ERR_MALFORMED;
            }
            message->body.query.target_node_id =
                read_u32_be(input + FED_BODY_OFFSET);
            message->body.query.requester_cluster_id =
                read_u32_be(input + FED_BODY_OFFSET + 4U);
            message->body.query.requester_head_node_id =
                read_u32_be(input + FED_BODY_OFFSET + 8U);
            break;
        case UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT:
            if (input_length < UCN_CLUSTER_FEDERATION_SUBMIT_HEADER_BYTES) {
                return UCN_ERR_MALFORMED;
            }
            message->body.submit.final_node_id =
                read_u32_be(input + FED_BODY_OFFSET);
            message->body.submit.endpoint = input[FED_BODY_OFFSET + 4U];
            message->body.submit.traffic_class =
                (ucn_traffic_class_t)input[FED_BODY_OFFSET + 5U];
            inner_length = read_u16_be(input + FED_BODY_OFFSET + 6U);
            if (input_length !=
                UCN_CLUSTER_FEDERATION_SUBMIT_HEADER_BYTES + inner_length) {
                return UCN_ERR_MALFORMED;
            }
            message->body.submit.inner_length = inner_length;
            message->body.submit.inner_payload =
                input + UCN_CLUSTER_FEDERATION_SUBMIT_HEADER_BYTES;
            break;
        case UCN_CLUSTER_FED_KIND_TUNNEL_DATA:
            if (input_length < UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES) {
                return UCN_ERR_MALFORMED;
            }
            message->body.tunnel.origin_node_id =
                read_u32_be(input + FED_BODY_OFFSET);
            message->body.tunnel.final_node_id =
                read_u32_be(input + FED_BODY_OFFSET + 4U);
            message->body.tunnel.origin_cluster_id =
                read_u32_be(input + FED_BODY_OFFSET + 8U);
            message->body.tunnel.destination_cluster_id =
                read_u32_be(input + FED_BODY_OFFSET + 12U);
            message->body.tunnel.endpoint = input[FED_BODY_OFFSET + 16U];
            message->body.tunnel.traffic_class =
                (ucn_traffic_class_t)input[FED_BODY_OFFSET + 17U];
            inner_length = read_u16_be(input + FED_BODY_OFFSET + 18U);
            if (input_length !=
                UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES + inner_length) {
                return UCN_ERR_MALFORMED;
            }
            message->body.tunnel.inner_length = inner_length;
            message->body.tunnel.inner_payload =
                input + UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES;
            break;
        case UCN_CLUSTER_FED_KIND_TUNNEL_DELIVER:
            if (input_length < UCN_CLUSTER_FEDERATION_DELIVER_HEADER_BYTES) {
                return UCN_ERR_MALFORMED;
            }
            message->body.delivery.origin_node_id =
                read_u32_be(input + FED_BODY_OFFSET);
            message->body.delivery.final_node_id =
                read_u32_be(input + FED_BODY_OFFSET + 4U);
            message->body.delivery.origin_cluster_id =
                read_u32_be(input + FED_BODY_OFFSET + 8U);
            message->body.delivery.destination_cluster_id =
                read_u32_be(input + FED_BODY_OFFSET + 12U);
            message->body.delivery.endpoint = input[FED_BODY_OFFSET + 16U];
            message->body.delivery.traffic_class =
                (ucn_traffic_class_t)input[FED_BODY_OFFSET + 17U];
            inner_length = read_u16_be(input + FED_BODY_OFFSET + 18U);
            if (input_length !=
                UCN_CLUSTER_FEDERATION_DELIVER_HEADER_BYTES + inner_length) {
                return UCN_ERR_MALFORMED;
            }
            message->body.delivery.inner_length = inner_length;
            message->body.delivery.inner_payload =
                input + UCN_CLUSTER_FEDERATION_DELIVER_HEADER_BYTES;
            break;
        case UCN_CLUSTER_FED_KIND_TUNNEL_ERROR:
            if (input_length != UCN_CLUSTER_FEDERATION_ERROR_BYTES ||
                input[FED_BODY_OFFSET + 2U] != 0U ||
                input[FED_BODY_OFFSET + 3U] != 0U) {
                return UCN_ERR_MALFORMED;
            }
            message->body.error.error =
                (ucn_cluster_federation_error_t)read_u16_be(input + FED_BODY_OFFSET);
            message->body.error.origin_node_id =
                read_u32_be(input + FED_BODY_OFFSET + 4U);
            message->body.error.final_node_id =
                read_u32_be(input + FED_BODY_OFFSET + 8U);
            break;
        case UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER:
            if (input_length != UCN_CLUSTER_FEDERATION_HANDOVER_BYTES) {
                return UCN_ERR_MALFORMED;
            }
            message->body.handover.cluster_id =
                read_u32_be(input + FED_BODY_OFFSET);
            message->body.handover.new_head_node_id =
                read_u32_be(input + FED_BODY_OFFSET + 4U);
            message->body.handover.new_term =
                read_u32_be(input + FED_BODY_OFFSET + 8U);
            message->body.handover.backup_generation =
                read_u32_be(input + FED_BODY_OFFSET + 12U);
            (void)memcpy(message->body.handover.proof,
                         input + FED_BODY_OFFSET + 16U,
                         UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES);
            break;
        default:
            return UCN_ERR_UNSUPPORTED;
    }
    return message_is_valid(message) ? UCN_OK : UCN_ERR_MALFORMED;
}

static uint32_t federation_now(const ucn_cluster_federation_t *federation)
{
    return federation->config.now_ms(federation->config.now_context);
}

static uint32_t next_nonzero_counter(uint32_t *counter)
{
    uint32_t value = *counter;

    if (value == 0U || value == UINT32_MAX) {
        value = 1U;
    }
    *counter = value + 1U;
    return value;
}

static void federation_apply_defaults(ucn_cluster_federation_config_t *config)
{
    if (config->default_hop_limit == 0U) {
        config->default_hop_limit = UCN_MAX_HOPS;
    }
    if (config->directory_lease_ms == 0U) {
        config->directory_lease_ms = UCN_CLUSTER_FED_DIRECTORY_LEASE_MS;
    }
    if (config->locator_refresh_ms == 0U) {
        config->locator_refresh_ms = UCN_CLUSTER_FED_LOCATOR_REFRESH_MS;
    }
    if (config->query_timeout_ms == 0U) {
        config->query_timeout_ms = UCN_CLUSTER_FED_QUERY_TIMEOUT_MS;
    }
}

static bool federation_config_is_valid(
    const ucn_cluster_federation_config_t *config)
{
    size_t index;
    bool local_is_authority = false;

    if (config == NULL || !node_id_is_valid(config->local_node_id)) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    if (config->cluster == NULL || config->now_ms == NULL || config->send == NULL ||
        config->directory_authorities == NULL ||
        config->directory_authority_count == 0U ||
        config->directory_authority_count >
            UCN_CLUSTER_FED_MAX_DIRECTORY_AUTHORITIES ||
        config->default_hop_limit == 0U ||
        config->default_hop_limit > UCN_MAX_HOPS ||
        !ucn_duration_is_valid(config->directory_lease_ms) ||
        !ucn_duration_is_valid(config->locator_refresh_ms) ||
        !ucn_duration_is_valid(config->query_timeout_ms) ||
        config->locator_refresh_ms > config->directory_lease_ms / 3U ||
        (config->directory_authority && config->authorize_head == NULL) ||
        (config->require_protected_control &&
         (config->authorize_handover == NULL ||
          config->build_handover_proof == NULL)) ||
        (config->enable_tunnel &&
         (config->authorize_head == NULL || config->deliver == NULL ||
          (config->inner_security_mode == UCN_CLUSTER_FED_INNER_SECURITY_REQUIRED &&
           (config->seal_inner == NULL || config->open_inner == NULL)) ||
          config->inner_security_mode >
              UCN_CLUSTER_FED_INNER_SECURITY_PROTECTED_OUTER_ONLY))) {
        return false;
    }
    for (index = 0U; index < config->directory_authority_count; ++index) {
        size_t prior;

        if (!node_id_is_valid(config->directory_authorities[index])) {
            return false;
        }
        if (config->directory_authorities[index] == config->local_node_id) {
            local_is_authority = true;
        }
        for (prior = 0U; prior < index; ++prior) {
            if (config->directory_authorities[prior] ==
                config->directory_authorities[index]) {
                return false;
            }
        }
    }
    return !config->directory_authority || local_is_authority;
}

static bool federation_is_authority(const ucn_cluster_federation_t *federation,
                                    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < federation->config.directory_authority_count;
         ++index) {
        if (federation->directory_authorities[index] == node_id) {
            return true;
        }
    }
    return false;
}

static bool federation_authorize_head(const ucn_cluster_federation_t *federation,
                                      ucn_node_id_t source)
{
    return federation->config.authorize_head != NULL &&
           federation->config.authorize_head(
               federation->config.authorize_context, source);
}

static bool federation_authorize_handover(
    const ucn_cluster_federation_t *federation,
    const ucn_cluster_federation_handover_t *handover)
{
    return (!federation->config.require_protected_control &&
            federation->config.authorize_handover == NULL) ||
           (federation->config.authorize_handover != NULL &&
            federation->config.authorize_handover(
                federation->config.authorize_handover_context, handover));
}

static bool federation_get_local_head_view(
    const ucn_cluster_federation_t *federation,
    ucn_cluster_view_t *view)
{
    return federation != NULL && view != NULL &&
           ucn_cluster_get_view(federation->config.cluster, view) == UCN_OK &&
           view->enabled && view->role == UCN_CLUSTER_ROLE_HEAD &&
           view->head_node_id == federation->config.local_node_id &&
           view->cluster_id != 0U && view->term != 0U;
}

static ucn_cluster_federation_directory_record_t *find_directory_record(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS; ++index) {
        ucn_cluster_federation_directory_record_t *record =
            &federation->directory_records[index];

        if (record->occupied && record->locator.node_id == node_id) {
            return record;
        }
    }
    return NULL;
}

static ucn_cluster_federation_directory_record_t *allocate_directory_record(
    ucn_cluster_federation_t *federation)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS; ++index) {
        if (!federation->directory_records[index].occupied) {
            return &federation->directory_records[index];
        }
    }
    return NULL;
}

static ucn_cluster_federation_cluster_head_lease_t *find_head_lease(
    ucn_cluster_federation_t *federation,
    uint32_t cluster_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_CLUSTER_HEAD_LEASES; ++index) {
        ucn_cluster_federation_cluster_head_lease_t *lease =
            &federation->cluster_head_leases[index];

        if (lease->occupied && lease->cluster_id == cluster_id) {
            return lease;
        }
    }
    return NULL;
}

static ucn_cluster_federation_cluster_head_lease_t *allocate_head_lease(
    ucn_cluster_federation_t *federation)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_CLUSTER_HEAD_LEASES; ++index) {
        if (!federation->cluster_head_leases[index].occupied) {
            return &federation->cluster_head_leases[index];
        }
    }
    return NULL;
}

static ucn_cluster_federation_locator_cache_entry_t *find_locator_cache(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCATOR_CACHE; ++index) {
        ucn_cluster_federation_locator_cache_entry_t *entry =
            &federation->locator_cache[index];

        if (entry->occupied && entry->locator.node_id == node_id) {
            return entry;
        }
    }
    return NULL;
}

static ucn_cluster_federation_locator_cache_entry_t *allocate_locator_cache(
    ucn_cluster_federation_t *federation)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCATOR_CACHE; ++index) {
        if (!federation->locator_cache[index].occupied) {
            return &federation->locator_cache[index];
        }
    }
    return NULL;
}

static ucn_cluster_federation_next_cluster_entry_t *find_next_cluster_mutable(
    ucn_cluster_federation_t *federation,
    uint32_t cluster_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS; ++index) {
        ucn_cluster_federation_next_cluster_entry_t *entry =
            &federation->next_clusters[index];

        if (entry->occupied && entry->cluster_id == cluster_id) {
            return entry;
        }
    }
    return NULL;
}

static ucn_cluster_federation_next_cluster_entry_t *allocate_next_cluster(
    ucn_cluster_federation_t *federation)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS; ++index) {
        if (!federation->next_clusters[index].occupied) {
            return &federation->next_clusters[index];
        }
    }
    return NULL;
}

static ucn_cluster_federation_pending_query_t *find_pending_query(
    ucn_cluster_federation_t *federation,
    uint32_t transaction_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_PENDING; ++index) {
        ucn_cluster_federation_pending_query_t *pending =
            &federation->pending[index];

        if (pending->occupied && pending->transaction_id == transaction_id) {
            return pending;
        }
    }
    return NULL;
}

static ucn_cluster_federation_pending_query_t *find_pending_target(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t target_node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_PENDING; ++index) {
        ucn_cluster_federation_pending_query_t *pending =
            &federation->pending[index];

        if (pending->occupied && pending->target_node_id == target_node_id) {
            return pending;
        }
    }
    return NULL;
}

static ucn_cluster_federation_pending_query_t *allocate_pending_query(
    ucn_cluster_federation_t *federation)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_PENDING; ++index) {
        if (!federation->pending[index].occupied) {
            return &federation->pending[index];
        }
    }
    return NULL;
}

static void expire_federation_state(ucn_cluster_federation_t *federation)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS; ++index) {
        ucn_cluster_federation_directory_record_t *record =
            &federation->directory_records[index];

        if (record->occupied &&
            ucn_deadline_expired(federation->now_ms, record->expires_at_ms)) {
            (void)memset(record, 0, sizeof(*record));
            federation->stats.records_expired++;
        }
    }
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_CLUSTER_HEAD_LEASES;
         ++index) {
        ucn_cluster_federation_cluster_head_lease_t *lease =
            &federation->cluster_head_leases[index];

        if (lease->occupied &&
            ucn_deadline_expired(federation->now_ms,
                                 lease->lease_expires_at_ms)) {
            (void)memset(lease, 0, sizeof(*lease));
        }
    }
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCATOR_CACHE; ++index) {
        ucn_cluster_federation_locator_cache_entry_t *entry =
            &federation->locator_cache[index];

        if (entry->occupied &&
            ucn_deadline_expired(federation->now_ms, entry->expires_at_ms)) {
            (void)memset(entry, 0, sizeof(*entry));
            federation->stats.cache_expired++;
        }
    }
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS; ++index) {
        ucn_cluster_federation_next_cluster_entry_t *entry =
            &federation->next_clusters[index];

        if (entry->occupied &&
            ucn_deadline_expired(federation->now_ms, entry->expires_at_ms)) {
            (void)memset(entry, 0, sizeof(*entry));
        }
    }
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS; ++index) {
        ucn_cluster_federation_seen_transaction_t *entry =
            &federation->seen[index];

        if (entry->occupied &&
            ucn_deadline_expired(federation->now_ms, entry->expires_at_ms)) {
            (void)memset(entry, 0, sizeof(*entry));
        }
    }
}

static ucn_result_t federation_send_message(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t destination,
    ucn_traffic_class_t traffic_class,
    const ucn_cluster_federation_message_t *message)
{
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
    size_t payload_length = 0U;
    ucn_result_t result = ucn_cluster_federation_message_encode(
        message, payload, sizeof(payload), &payload_length);

    if (result != UCN_OK) {
        return result;
    }
    result = federation->config.send(federation->config.send_context, destination,
                                     UCN_CLUSTER_FEDERATION_ENDPOINT,
                                     traffic_class, payload,
                                     (uint16_t)payload_length);
    if (result == UCN_OK) {
        federation->stats.messages_sent++;
    } else {
        federation->stats.send_failures++;
    }
    return result;
}

static ucn_result_t send_pending_query(
    ucn_cluster_federation_t *federation,
    ucn_cluster_federation_pending_query_t *pending,
    const ucn_cluster_view_t *view)
{
    ucn_cluster_federation_message_t message;
    ucn_result_t result = UCN_ERR_NOT_FOUND;

    if (pending == NULL || view == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&message, 0, sizeof(message));
    message.kind = UCN_CLUSTER_FED_KIND_LOCATOR_QUERY;
    message.hop_limit = federation->config.default_hop_limit;
    message.transaction_id = pending->transaction_id;
    message.body.query.target_node_id = pending->target_node_id;
    message.body.query.requester_cluster_id = view->cluster_id;
    message.body.query.requester_head_node_id = federation->config.local_node_id;
    while (pending->attempted_authority_count <
           federation->config.directory_authority_count) {
        ucn_node_id_t authority = federation->directory_authorities[
            pending->next_authority_cursor];

        pending->next_authority_cursor =
            (pending->next_authority_cursor + 1U) %
            federation->config.directory_authority_count;
        pending->attempted_authority_count++;
        pending->authority_node_id = authority;
        result = federation_send_message(federation, authority,
                                         UCN_TRAFFIC_Q1_REALTIME, &message);
        if (result == UCN_OK) {
            pending->deadline_ms = ucn_deadline_from_now(
                federation->now_ms, federation->config.query_timeout_ms);
            federation->stats.queries_sent++;
            return UCN_OK;
        }
    }
    return result;
}

static void retry_expired_pending_queries(
    ucn_cluster_federation_t *federation)
{
    ucn_cluster_view_t view;
    size_t index;
    bool is_head = federation_get_local_head_view(federation, &view);

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_PENDING; ++index) {
        ucn_cluster_federation_pending_query_t *pending =
            &federation->pending[index];

        if (!pending->occupied ||
            !ucn_deadline_expired(federation->now_ms, pending->deadline_ms)) {
            continue;
        }
        federation->stats.query_timeouts++;
        if (is_head && pending->attempted_authority_count <
                           federation->config.directory_authority_count &&
            send_pending_query(federation, pending, &view) == UCN_OK) {
            continue;
        }
        (void)memset(pending, 0, sizeof(*pending));
    }
}

static ucn_result_t cache_locator(ucn_cluster_federation_t *federation,
                                  const ucn_cluster_locator_t *locator)
{
    ucn_cluster_locator_t bounded;
    ucn_cluster_federation_locator_cache_entry_t *cache;
    ucn_cluster_federation_next_cluster_entry_t *next;

    if (federation == NULL || locator == NULL ||
        !locator_is_valid(locator, false)) {
        return UCN_ERR_ARGUMENT;
    }
    cache = find_locator_cache(federation, locator->node_id);
    bounded = *locator;
    if (bounded.lease_ms > federation->config.directory_lease_ms) {
        bounded.lease_ms = federation->config.directory_lease_ms;
    }
    /* A Directory reply is authenticated, but it may still be delayed behind
     * a later reply for the same target.  While a cache lease is live, only
     * a strictly newer record in the SAME Cluster identity can replace it.
     * Terms of distinct Cluster identities are intentionally incomparable;
     * their handover is accepted after the old cache lease expires. */
    if (cache != NULL &&
        !ucn_deadline_expired(federation->now_ms, cache->expires_at_ms)) {
        bool same_identity =
            cache->locator.cluster_id == bounded.cluster_id &&
            cache->locator.head_node_id == bounded.head_node_id;

        if (!same_identity || bounded.term < cache->locator.term ||
            (bounded.term == cache->locator.term &&
             bounded.record_nonce <= cache->locator.record_nonce)) {
            federation->stats.replay_rejected++;
            return UCN_ERR_REPLAY;
        }
    }
    if (cache == NULL) {
        cache = allocate_locator_cache(federation);
    }
    next = find_next_cluster_mutable(federation, bounded.cluster_id);
    if (next == NULL) {
        next = allocate_next_cluster(federation);
    }
    if (cache == NULL || next == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    cache->occupied = true;
    cache->locator = bounded;
    cache->expires_at_ms =
        ucn_deadline_from_now(federation->now_ms, bounded.lease_ms);
    next->occupied = true;
    next->cluster_id = bounded.cluster_id;
    next->head_node_id = bounded.head_node_id;
    next->term = bounded.term;
    next->expires_at_ms = cache->expires_at_ms;
    return UCN_OK;
}

static ucn_result_t handle_locator_register(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_locator_t *locator)
{
    ucn_cluster_federation_directory_record_t *record;
    const ucn_cluster_federation_cluster_head_lease_t *lease;

    if (!federation->config.directory_authority) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (outer_source != locator->head_node_id ||
        !federation_authorize_head(federation, outer_source)) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    /* C07.4: once a ClusterHeadLease exists only its Head may publish for
     * that Cluster.  A stale former Head is rejected before any per-member
     * comparison, implementing "old Head updates are denied after handover". */
    lease = find_head_lease(federation, locator->cluster_id);
    if (lease != NULL && locator->head_node_id != lease->head_node_id) {
        federation->stats.replay_rejected++;
        return UCN_ERR_REPLAY;
    }
    record = find_directory_record(federation, locator->node_id);
    if (record != NULL) {
        if (lease == NULL &&
            (record->locator.cluster_id != locator->cluster_id ||
             record->locator.head_node_id != locator->head_node_id)) {
            /* A new Cluster identity waits for the old lease to expire; C07.4
             * hands it over atomically via LOCATOR_HANDOVER instead. */
            federation->stats.replay_rejected++;
            return UCN_ERR_REPLAY;
        }
        if (locator->term < record->locator.term ||
            (locator->term == record->locator.term &&
             locator->record_nonce <= record->locator.record_nonce)) {
            federation->stats.replay_rejected++;
            return UCN_ERR_REPLAY;
        }
    } else {
        record = allocate_directory_record(federation);
        if (record == NULL) {
            return UCN_ERR_NO_SPACE;
        }
    }
    record->occupied = true;
    record->locator = *locator;
    if (record->locator.lease_ms > federation->config.directory_lease_ms) {
        record->locator.lease_ms = federation->config.directory_lease_ms;
    }
    record->expires_at_ms = ucn_deadline_from_now(
        federation->now_ms, record->locator.lease_ms);
    federation->stats.records_registered++;
    return UCN_OK;
}

static ucn_result_t handle_locator_handover(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_federation_handover_t *handover)
{
    ucn_cluster_federation_cluster_head_lease_t *lease;

    if (!federation->config.directory_authority) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (outer_source != handover->new_head_node_id ||
        !federation_authorize_head(federation, outer_source) ||
        !federation_authorize_handover(federation, handover)) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    lease = find_head_lease(federation, handover->cluster_id);
    if (lease != NULL && handover->new_term < lease->term) {
        federation->stats.handovers_rejected++;
        return UCN_ERR_REPLAY;
    }
    if (lease != NULL && handover->new_term == lease->term) {
        if (handover->new_head_node_id != lease->head_node_id ||
            handover->backup_generation != lease->backup_generation ||
            memcmp(handover->proof, lease->handover_proof,
                   UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES) != 0) {
            federation->stats.handovers_rejected++;
            return UCN_ERR_REPLAY;
        }
        lease->lease_expires_at_ms = ucn_deadline_from_now(
            federation->now_ms, federation->config.directory_lease_ms);
        federation->stats.handovers_accepted++;
        return UCN_OK;
    }
    if (lease == NULL) {
        lease = allocate_head_lease(federation);
        if (lease == NULL) {
            return UCN_ERR_NO_SPACE;
        }
    }
    /* Atomic replace in one owner operation: Head, Term, generation, proof
     * and the Authority-clock deadline change together. */
    lease->occupied = true;
    lease->cluster_id = handover->cluster_id;
    lease->head_node_id = handover->new_head_node_id;
    lease->term = handover->new_term;
    lease->backup_generation = handover->backup_generation;
    lease->lease_expires_at_ms = ucn_deadline_from_now(
        federation->now_ms, federation->config.directory_lease_ms);
    (void)memcpy(lease->handover_proof, handover->proof,
                 UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES);
    federation->stats.handovers_accepted++;
    return UCN_OK;
}

static ucn_result_t handle_locator_withdraw(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_locator_t *locator)
{
    ucn_cluster_federation_directory_record_t *record;
    const ucn_cluster_federation_cluster_head_lease_t *lease;

    if (!federation->config.directory_authority) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (outer_source != locator->head_node_id ||
        !federation_authorize_head(federation, outer_source)) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    /* C07.4: a former Head cannot withdraw records it no longer owns. */
    lease = find_head_lease(federation, locator->cluster_id);
    if (lease != NULL && locator->head_node_id != lease->head_node_id) {
        federation->stats.replay_rejected++;
        return UCN_ERR_REPLAY;
    }
    record = find_directory_record(federation, locator->node_id);
    if (record == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (record->locator.cluster_id != locator->cluster_id ||
        record->locator.head_node_id != locator->head_node_id ||
        record->locator.term != locator->term ||
        locator->record_nonce <= record->locator.record_nonce) {
        federation->stats.replay_rejected++;
        return UCN_ERR_REPLAY;
    }
    (void)memset(record, 0, sizeof(*record));
    federation->stats.records_withdrawn++;
    return UCN_OK;
}

static ucn_result_t send_directory_error(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t destination,
    uint32_t transaction_id,
    ucn_node_id_t origin_node_id,
    ucn_node_id_t final_node_id,
    ucn_cluster_federation_error_t error)
{
    ucn_cluster_federation_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.kind = UCN_CLUSTER_FED_KIND_TUNNEL_ERROR;
    message.hop_limit = federation->config.default_hop_limit;
    message.transaction_id = transaction_id;
    message.body.error.error = error;
    message.body.error.origin_node_id = origin_node_id;
    message.body.error.final_node_id = final_node_id;
    return federation_send_message(federation, destination,
                                   UCN_TRAFFIC_Q1_REALTIME, &message);
}

static ucn_result_t handle_locator_query(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_federation_message_t *request)
{
    ucn_cluster_federation_directory_record_t *record;
    const ucn_cluster_federation_cluster_head_lease_t *lease;
    ucn_cluster_federation_message_t reply;

    if (!federation->config.directory_authority) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (outer_source != request->body.query.requester_head_node_id ||
        !federation_authorize_head(federation, outer_source)) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    record = find_directory_record(federation,
                                   request->body.query.target_node_id);
    if (record == NULL) {
        return send_directory_error(
            federation, outer_source, request->transaction_id, outer_source,
            request->body.query.target_node_id,
            UCN_CLUSTER_FED_ERROR_DIRECTORY_NOT_FOUND);
    }
    (void)memset(&reply, 0, sizeof(reply));
    reply.kind = UCN_CLUSTER_FED_KIND_LOCATOR_REPLY;
    reply.hop_limit = federation->config.default_hop_limit;
    reply.transaction_id = request->transaction_id;
    reply.body.locator = record->locator;
    /* C07.4: Directory Reply resolves the member Cluster through the
     * current ClusterHeadLease, so one atomic lease replace rewires every
     * member Locator to the promoted Head without per-member churn. */
    lease = find_head_lease(federation, record->locator.cluster_id);
    if (lease != NULL) {
        reply.body.locator.head_node_id = lease->head_node_id;
        reply.body.locator.term = lease->term;
    }
    return federation_send_message(federation, outer_source,
                                   UCN_TRAFFIC_Q1_REALTIME, &reply);
}

static ucn_result_t handle_locator_reply(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_federation_message_t *message)
{
    ucn_cluster_federation_pending_query_t *pending;
    ucn_result_t result;

    if (!federation_is_authority(federation, outer_source)) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    pending = find_pending_query(federation, message->transaction_id);
    if (pending == NULL || pending->authority_node_id != outer_source ||
        pending->target_node_id != message->body.locator.node_id) {
        return UCN_ERR_NOT_FOUND;
    }
    result = cache_locator(federation, &message->body.locator);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(pending, 0, sizeof(*pending));
    return UCN_OK;
}

static ucn_result_t handle_directory_error(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_federation_message_t *message)
{
    ucn_cluster_federation_pending_query_t *pending;

    if (!federation_is_authority(federation, outer_source)) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    pending = find_pending_query(federation, message->transaction_id);
    if (pending == NULL || pending->authority_node_id != outer_source ||
        pending->target_node_id != message->body.error.final_node_id) {
        return UCN_ERR_NOT_FOUND;
    }
    (void)memset(pending, 0, sizeof(*pending));
    federation->stats.query_errors++;
    return UCN_OK;
}

static bool federation_get_local_view(
    const ucn_cluster_federation_t *federation,
    ucn_cluster_view_t *view)
{
    return federation != NULL && view != NULL &&
           ucn_cluster_get_view(federation->config.cluster, view) == UCN_OK &&
           view->enabled && view->cluster_id != 0U && view->term != 0U &&
           node_id_is_valid(view->head_node_id);
}

static bool federation_head_has_member(
    const ucn_cluster_federation_t *federation,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        ucn_cluster_member_summary_t summary;

        if (ucn_cluster_get_member_summary_at(federation->config.cluster, index,
                                              &summary) == UCN_OK &&
            summary.node_id == node_id &&
            !ucn_deadline_expired(federation->now_ms,
                                  summary.lease_expires_at_ms)) {
            return true;
        }
    }
    return false;
}

static ucn_cluster_federation_seen_transaction_t *find_seen_transaction(
    ucn_cluster_federation_t *federation,
    uint32_t transaction_id,
    ucn_node_id_t origin_node_id,
    ucn_node_id_t final_node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS; ++index) {
        ucn_cluster_federation_seen_transaction_t *entry =
            &federation->seen[index];

        if (entry->occupied && entry->transaction_id == transaction_id &&
            entry->origin_node_id == origin_node_id &&
            entry->final_node_id == final_node_id) {
            return entry;
        }
    }
    return NULL;
}

static ucn_result_t accept_tunnel_transaction(
    ucn_cluster_federation_t *federation,
    uint32_t transaction_id,
    ucn_node_id_t origin_node_id,
    ucn_node_id_t final_node_id,
    ucn_node_id_t remote_head_node_id,
    ucn_cluster_federation_seen_transaction_t **accepted)
{
    size_t index;
    ucn_cluster_federation_seen_transaction_t *entry;

    if (accepted != NULL) {
        *accepted = NULL;
    }
    entry = find_seen_transaction(federation, transaction_id, origin_node_id,
                                  final_node_id);
    if (entry != NULL) {
        federation->stats.tunnel_replays++;
        return UCN_ERR_REPLAY;
    }
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS; ++index) {
        if (!federation->seen[index].occupied) {
            entry = &federation->seen[index];
            (void)memset(entry, 0, sizeof(*entry));
            entry->occupied = true;
            entry->transaction_id = transaction_id;
            entry->origin_node_id = origin_node_id;
            entry->final_node_id = final_node_id;
            entry->remote_head_node_id = remote_head_node_id;
            entry->expires_at_ms = ucn_deadline_from_now(
                federation->now_ms, UCN_CLUSTER_FED_TRANSACTION_LEASE_MS);
            if (accepted != NULL) {
                *accepted = entry;
            }
            return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

static void build_inner_aad(ucn_cluster_federation_inner_aad_t *aad,
                            uint32_t transaction_id,
                            ucn_node_id_t origin_node_id,
                            ucn_node_id_t final_node_id,
                            uint32_t origin_cluster_id,
                            uint32_t destination_cluster_id,
                            ucn_endpoint_t endpoint,
                            ucn_traffic_class_t traffic_class)
{
    (void)memset(aad, 0, sizeof(*aad));
    aad->transaction_id = transaction_id;
    aad->origin_node_id = origin_node_id;
    aad->final_node_id = final_node_id;
    aad->origin_cluster_id = origin_cluster_id;
    aad->destination_cluster_id = destination_cluster_id;
    aad->endpoint = endpoint;
    aad->traffic_class = traffic_class;
}

/* At submission time the source knows its own Cluster but has not yet had the
 * target Locator resolved by its Head.  C06.3 therefore binds the stable
 * application semantics and origin Cluster; the destination Cluster remains
 * a separately validated Head-routing fact until C06.4 adds a negotiated
 * Locator-binding envelope. */
static void build_inner_security_aad(ucn_cluster_federation_inner_aad_t *aad,
                                     uint32_t transaction_id,
                                     ucn_node_id_t origin_node_id,
                                     ucn_node_id_t final_node_id,
                                     uint32_t origin_cluster_id,
                                     ucn_endpoint_t endpoint,
                                     ucn_traffic_class_t traffic_class)
{
    build_inner_aad(aad, transaction_id, origin_node_id, final_node_id,
                    origin_cluster_id, 0U, endpoint, traffic_class);
}

static void report_tunnel_error(
    ucn_cluster_federation_t *federation,
    uint32_t transaction_id,
    const ucn_cluster_federation_error_message_t *error)
{
    federation->stats.tunnel_errors++;
    if (federation->config.on_error != NULL) {
        federation->config.on_error(federation->config.error_context,
                                    transaction_id, error);
    }
}

static ucn_result_t send_tunnel_error(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t destination,
    uint32_t transaction_id,
    ucn_node_id_t origin_node_id,
    ucn_node_id_t final_node_id,
    ucn_cluster_federation_error_t error)
{
    ucn_cluster_federation_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.kind = UCN_CLUSTER_FED_KIND_TUNNEL_ERROR;
    message.hop_limit = federation->config.default_hop_limit;
    message.transaction_id = transaction_id;
    message.body.error.error = error;
    message.body.error.origin_node_id = origin_node_id;
    message.body.error.final_node_id = final_node_id;
    if (destination == federation->config.local_node_id) {
        report_tunnel_error(federation, transaction_id, &message.body.error);
        return UCN_OK;
    }
    return federation_send_message(federation, destination,
                                   UCN_TRAFFIC_Q1_REALTIME, &message);
}

static ucn_result_t deliver_inner_to_local(
    ucn_cluster_federation_t *federation,
    uint32_t transaction_id,
    ucn_node_id_t origin_node_id,
    ucn_node_id_t final_node_id,
    uint32_t origin_cluster_id,
    uint32_t destination_cluster_id,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *inner_payload,
    uint16_t inner_length)
{
    ucn_cluster_federation_inner_aad_t security_aad;
    ucn_cluster_federation_inner_aad_t delivery_aad;
    uint8_t plaintext[UCN_CLUSTER_FEDERATION_MAX_INNER_PAYLOAD_BYTES];
    const uint8_t *delivered_payload = inner_payload;
    uint16_t delivered_length = inner_length;
    ucn_result_t result;

    if (inner_length > UCN_CLUSTER_FEDERATION_MAX_INNER_PAYLOAD_BYTES ||
        (inner_length != 0U && inner_payload == NULL)) {
        return UCN_ERR_TOO_LARGE;
    }
    build_inner_aad(&delivery_aad, transaction_id, origin_node_id,
                    final_node_id, origin_cluster_id, destination_cluster_id,
                    endpoint, traffic_class);
    if (federation->config.inner_security_mode ==
        UCN_CLUSTER_FED_INNER_SECURITY_REQUIRED) {
        build_inner_security_aad(&security_aad, transaction_id, origin_node_id,
                                 final_node_id, origin_cluster_id, endpoint,
                                 traffic_class);
        result = federation->config.open_inner(
            federation->config.inner_security_context, &security_aad,
            inner_payload, inner_length, plaintext, (uint16_t)sizeof(plaintext),
            &delivered_length);
        if (result != UCN_OK) {
            return result == UCN_ERR_TOO_LARGE ? result : UCN_ERR_SECURITY;
        }
        if (delivered_length > sizeof(plaintext)) {
            return UCN_ERR_MALFORMED;
        }
        delivered_payload = plaintext;
    }
    result = federation->config.deliver(federation->config.deliver_context,
                                        &delivery_aad, delivered_payload,
                                        delivered_length);
    if (result == UCN_OK) {
        federation->stats.tunnel_deliveries++;
    }
    return result;
}

static ucn_result_t handle_tunnel_submit(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_federation_message_t *message)
{
    ucn_cluster_view_t view;
    const ucn_cluster_locator_t *locator;
    ucn_cluster_federation_message_t data;
    ucn_result_t result;

    if (!federation->config.enable_tunnel ||
        !federation_get_local_head_view(federation, &view)) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (outer_source != federation->config.local_node_id &&
        !federation_head_has_member(federation, outer_source)) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    if (message->hop_limit <= 1U) {
        federation->stats.tunnel_ttl_rejected++;
        return send_tunnel_error(federation, outer_source,
                                 message->transaction_id, outer_source,
                                 message->body.submit.final_node_id,
                                 UCN_CLUSTER_FED_ERROR_TTL);
    }
    if (message->body.submit.inner_length >
        UCN_CLUSTER_FEDERATION_MAX_INNER_PAYLOAD_BYTES) {
        return send_tunnel_error(federation, outer_source,
                                 message->transaction_id, outer_source,
                                 message->body.submit.final_node_id,
                                 UCN_CLUSTER_FED_ERROR_MTU);
    }
    result = accept_tunnel_transaction(
        federation, message->transaction_id, outer_source,
        message->body.submit.final_node_id, 0U, NULL);
    if (result != UCN_OK) {
        return result;
    }
    locator = ucn_cluster_federation_find_locator(
        federation, message->body.submit.final_node_id);
    if (locator == NULL || locator->cluster_id == view.cluster_id ||
        locator->head_node_id == federation->config.local_node_id) {
        (void)ucn_cluster_federation_query_locator(
            federation, message->body.submit.final_node_id);
        return send_tunnel_error(federation, outer_source,
                                 message->transaction_id, outer_source,
                                 message->body.submit.final_node_id,
                                 UCN_CLUSTER_FED_ERROR_DIRECTORY_NOT_FOUND);
    }
    (void)memset(&data, 0, sizeof(data));
    data.kind = UCN_CLUSTER_FED_KIND_TUNNEL_DATA;
    data.hop_limit = (uint8_t)(message->hop_limit - 1U);
    data.transaction_id = message->transaction_id;
    data.body.tunnel.origin_node_id = outer_source;
    data.body.tunnel.final_node_id = message->body.submit.final_node_id;
    data.body.tunnel.origin_cluster_id = view.cluster_id;
    data.body.tunnel.destination_cluster_id = locator->cluster_id;
    data.body.tunnel.endpoint = message->body.submit.endpoint;
    data.body.tunnel.traffic_class = message->body.submit.traffic_class;
    data.body.tunnel.inner_payload = message->body.submit.inner_payload;
    data.body.tunnel.inner_length = message->body.submit.inner_length;
    result = federation_send_message(federation, locator->head_node_id,
                                     data.body.tunnel.traffic_class, &data);
    if (result == UCN_OK) {
        ucn_cluster_federation_seen_transaction_t *seen =
            find_seen_transaction(federation, data.transaction_id,
                                  data.body.tunnel.origin_node_id,
                                  data.body.tunnel.final_node_id);

        if (seen != NULL) {
            seen->remote_head_node_id = locator->head_node_id;
        }
        federation->stats.tunnel_submits++;
        federation->stats.tunnel_data_sent++;
    } else {
        (void)send_tunnel_error(federation, outer_source,
                                message->transaction_id, outer_source,
                                message->body.submit.final_node_id,
                                result == UCN_ERR_TOO_LARGE ?
                                    UCN_CLUSTER_FED_ERROR_MTU :
                                    UCN_CLUSTER_FED_ERROR_DOWNSTREAM);
        /* The submitted frame was consumed and a bounded error was emitted;
         * returning LINK_DOWN here would make the Core retry the same outer
         * Submit and defeat the Seen-table replay gate. */
        return UCN_OK;
    }
    return result;
}

static ucn_result_t handle_tunnel_data(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_federation_message_t *message)
{
    ucn_cluster_view_t view;
    ucn_cluster_federation_message_t delivery;
    ucn_result_t result;

    if (!federation->config.enable_tunnel ||
        !federation_get_local_head_view(federation, &view)) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (!federation_authorize_head(federation, outer_source)) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    if (message->body.tunnel.destination_cluster_id != view.cluster_id ||
        message->hop_limit <= 1U) {
        federation->stats.tunnel_stale_rejected++;
        return send_tunnel_error(federation, outer_source,
                                 message->transaction_id,
                                 message->body.tunnel.origin_node_id,
                                 message->body.tunnel.final_node_id,
                                 message->hop_limit <= 1U ?
                                     UCN_CLUSTER_FED_ERROR_TTL :
                                     UCN_CLUSTER_FED_ERROR_DIRECTORY_STALE);
    }
    result = accept_tunnel_transaction(
        federation, message->transaction_id, message->body.tunnel.origin_node_id,
        message->body.tunnel.final_node_id, outer_source, NULL);
    if (result != UCN_OK) {
        return result;
    }
    if (message->body.tunnel.final_node_id == federation->config.local_node_id) {
        result = deliver_inner_to_local(
            federation, message->transaction_id,
            message->body.tunnel.origin_node_id,
            message->body.tunnel.final_node_id,
            message->body.tunnel.origin_cluster_id,
            message->body.tunnel.destination_cluster_id,
            message->body.tunnel.endpoint, message->body.tunnel.traffic_class,
            message->body.tunnel.inner_payload, message->body.tunnel.inner_length);
        if (result != UCN_OK) {
            (void)send_tunnel_error(federation, outer_source,
                                    message->transaction_id,
                                    message->body.tunnel.origin_node_id,
                                    message->body.tunnel.final_node_id,
                                    UCN_CLUSTER_FED_ERROR_UNAUTHORIZED);
            return UCN_OK;
        }
        return UCN_OK;
    }
    if (!federation_head_has_member(federation,
                                    message->body.tunnel.final_node_id)) {
        federation->stats.tunnel_stale_rejected++;
        return send_tunnel_error(federation, outer_source,
                                 message->transaction_id,
                                 message->body.tunnel.origin_node_id,
                                 message->body.tunnel.final_node_id,
                                 UCN_CLUSTER_FED_ERROR_DIRECTORY_STALE);
    }
    (void)memset(&delivery, 0, sizeof(delivery));
    delivery.kind = UCN_CLUSTER_FED_KIND_TUNNEL_DELIVER;
    delivery.hop_limit = (uint8_t)(message->hop_limit - 1U);
    delivery.transaction_id = message->transaction_id;
    delivery.body.delivery.origin_node_id = message->body.tunnel.origin_node_id;
    delivery.body.delivery.final_node_id = message->body.tunnel.final_node_id;
    delivery.body.delivery.origin_cluster_id =
        message->body.tunnel.origin_cluster_id;
    delivery.body.delivery.destination_cluster_id =
        message->body.tunnel.destination_cluster_id;
    delivery.body.delivery.endpoint = message->body.tunnel.endpoint;
    delivery.body.delivery.traffic_class = message->body.tunnel.traffic_class;
    delivery.body.delivery.inner_payload = message->body.tunnel.inner_payload;
    delivery.body.delivery.inner_length = message->body.tunnel.inner_length;
    result = federation_send_message(federation,
                                     message->body.tunnel.final_node_id,
                                     delivery.body.delivery.traffic_class,
                                     &delivery);
    if (result != UCN_OK) {
        (void)send_tunnel_error(federation, outer_source,
                                message->transaction_id,
                                message->body.tunnel.origin_node_id,
                                message->body.tunnel.final_node_id,
                                result == UCN_ERR_TOO_LARGE ?
                                    UCN_CLUSTER_FED_ERROR_MTU :
                                    UCN_CLUSTER_FED_ERROR_DOWNSTREAM);
        return UCN_OK;
    }
    return result;
}

static ucn_result_t handle_tunnel_deliver(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_federation_message_t *message)
{
    ucn_cluster_view_t view;
    ucn_result_t result;

    if (!federation->config.enable_tunnel ||
        !federation_get_local_view(federation, &view)) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (view.role != UCN_CLUSTER_ROLE_MEMBER ||
        view.head_node_id != outer_source ||
        message->body.delivery.final_node_id != federation->config.local_node_id ||
        message->body.delivery.destination_cluster_id != view.cluster_id) {
        federation->stats.authorization_rejected++;
        return UCN_ERR_ACCESS;
    }
    result = accept_tunnel_transaction(
        federation, message->transaction_id, message->body.delivery.origin_node_id,
        message->body.delivery.final_node_id, outer_source, NULL);
    if (result != UCN_OK) {
        return result;
    }
    result = deliver_inner_to_local(
        federation, message->transaction_id, message->body.delivery.origin_node_id,
        message->body.delivery.final_node_id,
        message->body.delivery.origin_cluster_id,
        message->body.delivery.destination_cluster_id,
        message->body.delivery.endpoint, message->body.delivery.traffic_class,
        message->body.delivery.inner_payload, message->body.delivery.inner_length);
    if (result != UCN_OK) {
        /* The final Member has no authority to fabricate a return path; the
         * local callback records the security failure and the Head-side Seen
         * timeout remains the bounded recovery boundary in C06.3. */
        federation->stats.security_rejected++;
        return UCN_OK;
    }
    return UCN_OK;
}

static ucn_result_t handle_tunnel_error(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    const ucn_cluster_federation_message_t *message)
{
    ucn_cluster_view_t view;
    ucn_cluster_federation_seen_transaction_t *seen;

    if (!federation->config.enable_tunnel ||
        !federation_get_local_view(federation, &view)) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (view.role == UCN_CLUSTER_ROLE_HEAD) {
        seen = find_seen_transaction(federation, message->transaction_id,
                                     message->body.error.origin_node_id,
                                     message->body.error.final_node_id);
        if (seen == NULL || seen->remote_head_node_id != outer_source) {
            return UCN_ERR_NOT_FOUND;
        }
        seen->remote_head_node_id = 0U;
        return send_tunnel_error(federation, message->body.error.origin_node_id,
                                 message->transaction_id,
                                 message->body.error.origin_node_id,
                                 message->body.error.final_node_id,
                                 message->body.error.error);
    }
    if (view.role == UCN_CLUSTER_ROLE_MEMBER &&
        message->body.error.origin_node_id == federation->config.local_node_id &&
        view.head_node_id == outer_source) {
        report_tunnel_error(federation, message->transaction_id,
                            &message->body.error);
        return UCN_OK;
    }
    federation->stats.authorization_rejected++;
    return UCN_ERR_ACCESS;
}

static ucn_cluster_federation_local_locator_entry_t *find_local_locator(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS; ++index) {
        ucn_cluster_federation_local_locator_entry_t *entry =
            &federation->local_locators[index];

        if (entry->occupied && entry->locator.node_id == node_id) {
            return entry;
        }
    }
    return NULL;
}

static ucn_cluster_federation_local_locator_entry_t *allocate_local_locator(
    ucn_cluster_federation_t *federation)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS; ++index) {
        if (!federation->local_locators[index].occupied) {
            return &federation->local_locators[index];
        }
    }
    return NULL;
}

static void observe_local_locator(ucn_cluster_federation_t *federation,
                                  const ucn_cluster_view_t *view,
                                  ucn_node_id_t node_id)
{
    ucn_cluster_federation_local_locator_entry_t *entry =
        find_local_locator(federation, node_id);

    if (entry != NULL &&
        (entry->locator.cluster_id != view->cluster_id ||
         entry->locator.head_node_id != view->head_node_id ||
         entry->locator.term != view->term)) {
        /* Withdraw old ownership before registering the same Node ID under a
         * new Head.  This is intentionally lease-safe, not a fast handover. */
        entry->withdrawal_pending = true;
        return;
    }
    if (entry == NULL) {
        entry = allocate_local_locator(federation);
        if (entry == NULL) {
            federation->stats.local_locator_overflow++;
            return;
        }
        (void)memset(entry, 0, sizeof(*entry));
        entry->occupied = true;
        entry->locator.node_id = node_id;
        entry->locator.cluster_id = view->cluster_id;
        entry->locator.head_node_id = view->head_node_id;
        entry->locator.term = view->term;
    }
    entry->locator.lease_ms = federation->config.directory_lease_ms;
    entry->observed = true;
    entry->withdrawal_pending = false;
}

static void sync_local_locators(ucn_cluster_federation_t *federation)
{
    ucn_cluster_view_t view;
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS; ++index) {
        if (federation->local_locators[index].occupied) {
            federation->local_locators[index].observed = false;
        }
    }
    if (ucn_cluster_get_view(federation->config.cluster, &view) != UCN_OK ||
        !view.enabled || view.role != UCN_CLUSTER_ROLE_HEAD ||
        view.cluster_id == 0U || view.term == 0U ||
        view.head_node_id != federation->config.local_node_id) {
        return;
    }
    observe_local_locator(federation, &view, federation->config.local_node_id);
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        ucn_cluster_member_summary_t summary;

        if (ucn_cluster_get_member_summary_at(federation->config.cluster, index,
                                              &summary) == UCN_OK) {
            observe_local_locator(federation, &view, summary.node_id);
        }
    }
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS; ++index) {
        ucn_cluster_federation_local_locator_entry_t *entry =
            &federation->local_locators[index];

        if (entry->occupied && !entry->observed) {
            entry->withdrawal_pending = true;
        }
    }
}

static size_t publishable_local_locator_count(
    const ucn_cluster_federation_t *federation)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS; ++index) {
        if (federation->local_locators[index].occupied &&
            (federation->local_locators[index].observed ||
             federation->local_locators[index].withdrawal_pending)) {
            ++count;
        }
    }
    return count;
}

static ucn_result_t publish_one_locator(ucn_cluster_federation_t *federation)
{
    size_t examined;
    size_t work_count = publishable_local_locator_count(federation);
    size_t entry_index = 0U;
    size_t denominator;
    ucn_cluster_federation_local_locator_entry_t *entry = NULL;
    ucn_cluster_federation_message_t message;
    ucn_node_id_t authority;
    uint32_t slice_ms;
    ucn_result_t result;

    /* A zero publish deadline is the explicit cold-start "publish now"
     * state.  ucn_deadline_expired() deliberately treats zero as no deadline. */
    if (work_count == 0U ||
        (federation->next_publish_at_ms != 0U &&
         !ucn_deadline_expired(federation->now_ms,
                               federation->next_publish_at_ms))) {
        return UCN_OK;
    }
    for (examined = 0U; examined < UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS; ++examined) {
        size_t index = (federation->publish_locator_cursor + examined) %
                       UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS;
        ucn_cluster_federation_local_locator_entry_t *candidate =
            &federation->local_locators[index];

        if (candidate->occupied && candidate->withdrawal_pending) {
            entry = candidate;
            entry_index = index;
            break;
        }
    }
    if (entry == NULL) {
        for (examined = 0U; examined < UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS;
             ++examined) {
            size_t index = (federation->publish_locator_cursor + examined) %
                           UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS;
            ucn_cluster_federation_local_locator_entry_t *candidate =
                &federation->local_locators[index];

            if (candidate->occupied && candidate->observed) {
                entry = candidate;
                entry_index = index;
                break;
            }
        }
    }
    if (entry == NULL) {
        return UCN_OK;
    }
    authority = federation->directory_authorities[entry->authority_cursor];
    (void)memset(&message, 0, sizeof(message));
    message.kind = entry->withdrawal_pending ?
                       UCN_CLUSTER_FED_KIND_LOCATOR_WITHDRAW :
                       UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER;
    message.hop_limit = federation->config.default_hop_limit;
    message.transaction_id = next_nonzero_counter(&federation->next_transaction_id);
    message.body.locator = entry->locator;
    message.body.locator.record_nonce =
        next_nonzero_counter(&federation->next_record_nonce);
    entry->locator.record_nonce = message.body.locator.record_nonce;
    if (entry->withdrawal_pending) {
        message.body.locator.lease_ms = 0U;
    }
    if (authority == federation->config.local_node_id &&
        federation->config.directory_authority) {
        result = entry->withdrawal_pending ?
                     handle_locator_withdraw(federation, authority,
                                             &message.body.locator) :
                     handle_locator_register(federation, authority,
                                             &message.body.locator);
        if (entry->withdrawal_pending && result == UCN_ERR_NOT_FOUND) {
            result = UCN_OK;
        }
        if (result == UCN_OK) {
            federation->stats.messages_sent++;
        }
    } else {
        result = federation_send_message(federation, authority,
                                         UCN_TRAFFIC_Q1_REALTIME, &message);
    }
    /* Each refresh cycle attempts every fixed Authority exactly once.  A
     * failed replica is retried next cycle, but must not starve later Locator
     * entries or the remaining healthy replica. */
    entry->authority_cursor++;
    if (entry->authority_cursor >=
        federation->config.directory_authority_count) {
        entry->authority_cursor = 0U;
        federation->publish_locator_cursor =
            (entry_index + 1U) %
            UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS;
        if (entry->withdrawal_pending) {
            (void)memset(entry, 0, sizeof(*entry));
        }
    }
    if (work_count > UINT32_MAX /
                         federation->config.directory_authority_count) {
        denominator = (size_t)UINT32_MAX;
    } else {
        denominator = work_count * federation->config.directory_authority_count;
    }
    slice_ms = federation->config.locator_refresh_ms / (uint32_t)denominator;
    if (slice_ms == 0U) {
        slice_ms = 1U;
    }
    federation->next_publish_at_ms =
        ucn_deadline_from_now(federation->now_ms, slice_ms);
    return result;
}

static ucn_result_t send_handover_message(
    ucn_cluster_federation_t *federation,
    const ucn_cluster_federation_handover_t *handover,
    ucn_node_id_t authority)
{
    ucn_cluster_federation_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.kind = UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER;
    message.hop_limit = federation->config.default_hop_limit;
    message.transaction_id =
        next_nonzero_counter(&federation->next_transaction_id);
    message.body.handover = *handover;
    if (authority == federation->config.local_node_id &&
        federation->config.directory_authority) {
        return handle_locator_handover(federation, authority,
                                        &message.body.handover);
    }
    return federation_send_message(federation, authority,
                                   UCN_TRAFFIC_Q1_REALTIME, &message);
}

ucn_result_t ucn_cluster_federation_publish_handover(
    ucn_cluster_federation_t *federation)
{
    ucn_cluster_view_t view;
    ucn_cluster_federation_handover_t handover;
    ucn_result_t result = UCN_OK;
    size_t index;

    if (federation == NULL || !federation->config.enabled) {
        return UCN_ERR_ARGUMENT;
    }
    if (!federation_get_local_head_view(federation, &view)) {
        return UCN_ERR_ACCESS;
    }
    (void)memset(&handover, 0, sizeof(handover));
    handover.cluster_id = view.cluster_id;
    handover.new_head_node_id = federation->config.local_node_id;
    handover.new_term = view.term;
    handover.backup_generation =
        federation->config.cluster->backup_generation;
    if (federation->config.build_handover_proof != NULL) {
        result = federation->config.build_handover_proof(
            federation->config.handover_proof_context, &handover,
            handover.proof);
        if (result != UCN_OK) {
            return result;
        }
    } else if (federation->config.require_protected_control) {
        return UCN_ERR_CONFIG;
    }
    for (index = 0U; index < federation->config.directory_authority_count;
         ++index) {
        ucn_result_t send_result = send_handover_message(
            federation, &handover, federation->directory_authorities[index]);

        if (send_result != UCN_OK && result == UCN_OK) {
            result = send_result;
        }
    }
    return result;
}

static void publish_handover_if_needed(ucn_cluster_federation_t *federation)
{
    ucn_cluster_view_t view;
    bool local_is_head = federation_get_local_head_view(federation, &view);

    /* A local-Head transition (or a Term bump while staying Head) is the
     * takeover signal: publish exactly one ClusterHeadLease handover per
     * new generation. */
    if (!local_is_head) {
        federation->handover_pending = false;
        federation->handover_attempts = 0U;
        federation->was_local_head = false;
        return;
    }
    if (!federation->was_local_head ||
        federation->pending_handover_term != view.term) {
        federation->handover_pending = true;
        federation->pending_handover_term = view.term;
        federation->handover_attempts = 0U;
        federation->next_handover_retry_ms = federation->now_ms;
    }
    if (federation->handover_pending &&
        (federation->next_handover_retry_ms == 0U ||
         ucn_deadline_expired(federation->now_ms,
                              federation->next_handover_retry_ms))) {
        (void)ucn_cluster_federation_publish_handover(federation);
        federation->handover_attempts++;
        if (federation->handover_attempts >=
            UCN_CLUSTER_FED_HANDOVER_MAX_ATTEMPTS) {
            /* C06.3 has no transport ACK.  Finish the bounded immediate
             * burst, then keep the same idempotent handover leased by
             * republishing it at the normal Directory refresh cadence.
             * This survives an outage longer than one retry burst without
             * pretending that a queued send proved Authority delivery. */
            federation->handover_attempts = 0U;
            federation->next_handover_retry_ms = ucn_deadline_from_now(
                federation->now_ms, federation->config.locator_refresh_ms);
            federation->last_handover_term = view.term;
        } else {
            federation->next_handover_retry_ms = ucn_deadline_from_now(
                federation->now_ms, UCN_CLUSTER_FED_HANDOVER_RETRY_MS);
        }
    }
    federation->was_local_head = true;
}

ucn_result_t ucn_cluster_federation_init(
    ucn_cluster_federation_t *federation,
    const ucn_cluster_federation_config_t *config)
{
    ucn_cluster_view_t view;
    size_t index;

    if (federation == NULL || config == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(federation, 0, sizeof(*federation));
    federation->config = *config;
    federation_apply_defaults(&federation->config);
    if (!federation_config_is_valid(&federation->config)) {
        (void)memset(federation, 0, sizeof(*federation));
        return UCN_ERR_CONFIG;
    }
    for (index = 0U; index < federation->config.directory_authority_count;
         ++index) {
        federation->directory_authorities[index] =
            federation->config.directory_authorities[index];
    }
    federation->config.directory_authorities = federation->directory_authorities;
    federation->next_transaction_id = 1U;
    federation->next_record_nonce = 1U;
    if (federation->config.enabled) {
        federation->now_ms = federation_now(federation);
        federation->next_publish_at_ms = federation->now_ms;
    }
    /* C07.4: seed the takeover tracker from the current owner view so a
     * node that is already Head does not publish a spurious handover. */
    federation->was_local_head =
        federation_get_local_head_view(federation, &view);
    federation->last_handover_term =
        federation->was_local_head ? view.term : 0U;
    federation->pending_handover_term = federation->last_handover_term;
    return UCN_OK;
}

ucn_result_t ucn_cluster_federation_receive(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    bool protected_outer,
    const uint8_t *payload,
    size_t payload_length)
{
    ucn_cluster_federation_message_t message;
    ucn_result_t result;

    if (federation == NULL || !federation->config.enabled ||
        !node_id_is_valid(outer_source)) {
        return UCN_ERR_ARGUMENT;
    }
    if (federation->config.require_protected_control && !protected_outer) {
        federation->stats.security_rejected++;
        return UCN_ERR_SECURITY;
    }
    federation->now_ms = federation_now(federation);
    expire_federation_state(federation);
    result = ucn_cluster_federation_message_decode(payload, payload_length,
                                                    &message);
    if (result != UCN_OK) {
        if (result == UCN_ERR_MALFORMED) {
            federation->stats.malformed_messages++;
        }
        return result;
    }
    federation->stats.messages_received++;
    switch (message.kind) {
        case UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER:
            return handle_locator_register(federation, outer_source,
                                           &message.body.locator);
        case UCN_CLUSTER_FED_KIND_LOCATOR_WITHDRAW:
            return handle_locator_withdraw(federation, outer_source,
                                           &message.body.locator);
        case UCN_CLUSTER_FED_KIND_LOCATOR_QUERY:
            return handle_locator_query(federation, outer_source, &message);
        case UCN_CLUSTER_FED_KIND_LOCATOR_REPLY:
            return handle_locator_reply(federation, outer_source, &message);
        case UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER:
            return handle_locator_handover(federation, outer_source,
                                           &message.body.handover);
        case UCN_CLUSTER_FED_KIND_TUNNEL_ERROR:
            if (federation_is_authority(federation, outer_source) &&
                find_pending_query(federation, message.transaction_id) != NULL) {
                return handle_directory_error(federation, outer_source, &message);
            }
            return handle_tunnel_error(federation, outer_source, &message);
        case UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT:
            return handle_tunnel_submit(federation, outer_source, &message);
        case UCN_CLUSTER_FED_KIND_TUNNEL_DATA:
            return handle_tunnel_data(federation, outer_source, &message);
        case UCN_CLUSTER_FED_KIND_TUNNEL_DELIVER:
            return handle_tunnel_deliver(federation, outer_source, &message);
        default:
            return UCN_ERR_UNSUPPORTED;
    }
}

ucn_result_t ucn_cluster_federation_step(
    ucn_cluster_federation_t *federation)
{
    ucn_result_t result;

    if (federation == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!federation->config.enabled) {
        return UCN_OK;
    }
    federation->now_ms = federation_now(federation);
    expire_federation_state(federation);
    retry_expired_pending_queries(federation);
    sync_local_locators(federation);
    publish_handover_if_needed(federation);
    result = publish_one_locator(federation);
    return result == UCN_ERR_NOT_FOUND ? UCN_OK : result;
}

ucn_result_t ucn_cluster_federation_query_locator(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t target_node_id)
{
    ucn_cluster_view_t view;
    ucn_cluster_federation_pending_query_t *pending;
    ucn_result_t result;

    if (federation == NULL || !federation->config.enabled ||
        !node_id_is_valid(target_node_id)) {
        return UCN_ERR_ARGUMENT;
    }
    federation->now_ms = federation_now(federation);
    expire_federation_state(federation);
    retry_expired_pending_queries(federation);
    if (ucn_cluster_federation_find_locator(federation, target_node_id) != NULL) {
        federation->stats.cache_hits++;
        return UCN_OK;
    }
    federation->stats.cache_misses++;
    if (!federation_get_local_head_view(federation, &view)) {
        return UCN_ERR_ACCESS;
    }
    if (find_pending_target(federation, target_node_id) != NULL) {
        return UCN_OK;
    }
    pending = allocate_pending_query(federation);
    if (pending == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    (void)memset(pending, 0, sizeof(*pending));
    pending->occupied = true;
    pending->transaction_id =
        next_nonzero_counter(&federation->next_transaction_id);
    pending->target_node_id = target_node_id;
    pending->next_authority_cursor = federation->query_authority_cursor;
    federation->query_authority_cursor =
        (federation->query_authority_cursor + 1U) %
        federation->config.directory_authority_count;
    result = send_pending_query(federation, pending, &view);
    if (result != UCN_OK) {
        (void)memset(pending, 0, sizeof(*pending));
    }
    return result;
}

ucn_result_t ucn_cluster_federation_send(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t final_node_id,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length)
{
    ucn_cluster_view_t view;
    ucn_cluster_federation_message_t submit;
    ucn_cluster_federation_inner_aad_t security_aad;
    uint8_t secured_payload[UCN_CLUSTER_FEDERATION_MAX_INNER_PAYLOAD_BYTES];
    const uint8_t *inner_payload = payload;
    uint16_t inner_length = payload_length;
    ucn_result_t result;

    if (federation == NULL || !federation->config.enabled ||
        !federation->config.enable_tunnel || !node_id_is_valid(final_node_id) ||
        final_node_id == federation->config.local_node_id ||
        !ucn_endpoint_is_static(endpoint) || !traffic_class_is_valid(traffic_class) ||
        (payload_length != 0U && payload == NULL) ||
        payload_length > UCN_CLUSTER_FEDERATION_MAX_INNER_PAYLOAD_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    federation->now_ms = federation_now(federation);
    expire_federation_state(federation);
    if (!federation_get_local_view(federation, &view) ||
        (view.role != UCN_CLUSTER_ROLE_HEAD &&
         view.role != UCN_CLUSTER_ROLE_MEMBER)) {
        return UCN_ERR_ACCESS;
    }
    (void)memset(&submit, 0, sizeof(submit));
    submit.kind = UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT;
    submit.hop_limit = federation->config.default_hop_limit;
    submit.transaction_id = next_nonzero_counter(&federation->next_transaction_id);
    if (federation->config.inner_security_mode ==
        UCN_CLUSTER_FED_INNER_SECURITY_REQUIRED) {
        build_inner_security_aad(&security_aad, submit.transaction_id,
                                 federation->config.local_node_id,
                                 final_node_id, view.cluster_id, endpoint,
                                 traffic_class);
        result = federation->config.seal_inner(
            federation->config.inner_security_context, &security_aad, payload,
            payload_length, secured_payload, (uint16_t)sizeof(secured_payload),
            &inner_length);
        if (result != UCN_OK) {
            return result == UCN_ERR_TOO_LARGE ? result : UCN_ERR_SECURITY;
        }
        if (inner_length > sizeof(secured_payload)) {
            return UCN_ERR_MALFORMED;
        }
        inner_payload = secured_payload;
    }
    submit.body.submit.final_node_id = final_node_id;
    submit.body.submit.endpoint = endpoint;
    submit.body.submit.traffic_class = traffic_class;
    submit.body.submit.inner_payload = inner_payload;
    submit.body.submit.inner_length = inner_length;
    if (view.role == UCN_CLUSTER_ROLE_HEAD) {
        return handle_tunnel_submit(federation, federation->config.local_node_id,
                                    &submit);
    }
    return federation_send_message(federation, view.head_node_id, traffic_class,
                                   &submit);
}

const ucn_cluster_locator_t *ucn_cluster_federation_find_locator(
    const ucn_cluster_federation_t *federation,
    ucn_node_id_t target_node_id)
{
    size_t index;
    uint32_t now_ms;

    if (federation == NULL || !federation->config.enabled ||
        !node_id_is_valid(target_node_id)) {
        return NULL;
    }
    now_ms = federation_now(federation);
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCATOR_CACHE; ++index) {
        const ucn_cluster_federation_locator_cache_entry_t *entry =
            &federation->locator_cache[index];

        if (entry->occupied && entry->locator.node_id == target_node_id &&
            !ucn_deadline_expired(now_ms, entry->expires_at_ms)) {
            return &entry->locator;
        }
    }
    return NULL;
}

const ucn_cluster_federation_next_cluster_entry_t *
ucn_cluster_federation_find_next_cluster(
    const ucn_cluster_federation_t *federation,
    uint32_t cluster_id)
{
    size_t index;
    uint32_t now_ms;

    if (federation == NULL || !federation->config.enabled || cluster_id == 0U) {
        return NULL;
    }
    now_ms = federation_now(federation);
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS; ++index) {
        const ucn_cluster_federation_next_cluster_entry_t *entry =
            &federation->next_clusters[index];

        if (entry->occupied && entry->cluster_id == cluster_id &&
            !ucn_deadline_expired(now_ms, entry->expires_at_ms)) {
            return entry;
        }
    }
    return NULL;
}

const ucn_cluster_federation_cluster_head_lease_t *
ucn_cluster_federation_find_head_lease(
    const ucn_cluster_federation_t *federation,
    uint32_t cluster_id)
{
    size_t index;

    if (federation == NULL || !federation->config.enabled ||
        cluster_id == 0U) {
        return NULL;
    }
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_CLUSTER_HEAD_LEASES;
         ++index) {
        const ucn_cluster_federation_cluster_head_lease_t *lease =
            &federation->cluster_head_leases[index];

        if (lease->occupied && lease->cluster_id == cluster_id) {
            return lease;
        }
    }
    return NULL;
}

const ucn_cluster_federation_stats_t *ucn_cluster_federation_get_stats(
    const ucn_cluster_federation_t *federation)
{
    return federation == NULL ? NULL : &federation->stats;
}
