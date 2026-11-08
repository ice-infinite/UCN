#include "internal/ucn_route.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_ROUTE_MAGIC UINT32_C(0x55435254)
#define UCN_I_ROUTE_DISCOVERY_KIND UINT8_C(0x41)
#define UCN_I_ROUTE_REPLY_KIND UINT8_C(0x42)

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool bytes_zero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) { return false; }
    }
    return true;
}

static bool object_zero(const void *object, size_t bytes)
{
    return bytes_zero((const uint8_t *)object, bytes);
}

static bool lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

static ucn_result_t owner_lock(ucn_i_route_owner_t *owner)
{
    if (owner == NULL || owner->magic != UCN_I_ROUTE_MAGIC ||
        owner->schema != UCN_I_ROUTE_SCHEMA ||
        !lock_valid(&owner->state_lock)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

static void owner_unlock(ucn_i_route_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static bool address_valid(uint32_t address, uint8_t address_width)
{
    uint32_t limit;

    if (address_width == 0U || address_width > 4U) {
        return false;
    }
    limit = address_width == 4U ? UINT32_MAX :
        (UINT32_C(1) << (address_width * 8U)) - UINT32_C(1);
    return address != 0U && address < limit;
}

static bool binding_valid(const ucn_i_route_binding_t *binding,
                          uint8_t address_width)
{
    return binding != NULL &&
           address_valid(binding->address, address_width) &&
           binding->generation != 0U &&
           bytes_nonzero(binding->principal, sizeof(binding->principal));
}

static bool binding_equal(const ucn_i_route_binding_t *left,
                          const ucn_i_route_binding_t *right)
{
    size_t i;
    if (left->address != right->address || left->generation != right->generation) { return false; }
    for (i = 0U; i < sizeof(left->principal); ++i) {
        if (left->principal[i] != right->principal[i]) { return false; }
    }
    return true;
}

static bool link_valid(const ucn_i_route_link_ref_t *link,
                       uint8_t address_width)
{
    return link != NULL && binding_valid(&link->peer, address_width) &&
           link->link_generation != 0U && link->cost != 0U &&
           link->link_id != 0U && link->link_id != UINT16_MAX &&
           link->frame_mtu != 0U && link->reserved_zero == 0U;
}

static bool link_equal(const ucn_i_route_link_ref_t *left,
                       const ucn_i_route_link_ref_t *right)
{
    return binding_equal(&left->peer, &right->peer) &&
           left->link_generation == right->link_generation &&
           left->cost == right->cost && left->link_id == right->link_id &&
           left->frame_mtu == right->frame_mtu &&
           left->capability_bits == right->capability_bits;
}

static bool domain_valid(const ucn_i_route_owner_t *owner,
                         const ucn_i_route_domain_t *domain)
{
    return domain != NULL && domain->realm == owner->realm &&
           domain->origin_session_generation != 0U &&
           binding_valid(&domain->origin, owner->address_width) &&
           binding_valid(&domain->destination, owner->address_width) &&
           !binding_equal(&domain->origin, &domain->destination);
}

static bool domain_equal(const ucn_i_route_domain_t *left,
                         const ucn_i_route_domain_t *right)
{
    return left->realm == right->realm &&
           left->origin_session_generation ==
               right->origin_session_generation &&
           binding_equal(&left->origin, &right->origin) &&
           binding_equal(&left->destination, &right->destination);
}

static bool key_valid(const ucn_i_route_owner_t *owner,
                      const ucn_i_route_discovery_key_t *key)
{
    return key != NULL && key->realm == owner->realm &&
           binding_valid(&key->origin, owner->address_width) &&
           key->origin_session_generation != 0U &&
           address_valid(key->target_address, owner->address_width) &&
           key->transaction_id != 0U;
}

static bool key_equal(const ucn_i_route_discovery_key_t *left,
                      const ucn_i_route_discovery_key_t *right)
{
    return left->transaction_id == right->transaction_id &&
           left->realm == right->realm &&
           left->origin_session_generation ==
               right->origin_session_generation &&
           left->target_address == right->target_address &&
           binding_equal(&left->origin, &right->origin);
}

static bool rreq_payload_valid(const ucn_i_route_rreq_payload_t *value)
{
    return value != NULL && value->minimum_frame_mtu != 0U &&
           (value->flags & UINT8_C(0xFC)) == 0U &&
           bytes_zero(value->reserved_zero, sizeof(value->reserved_zero));
}

static bool rrep_payload_valid(const ucn_i_route_rrep_payload_t *value)
{
    return value != NULL &&
           bytes_nonzero(value->destination_principal,
                         sizeof(value->destination_principal)) &&
           value->destination_binding_generation != 0U &&
           value->path_frame_mtu != 0U && value->hop_count != UINT8_MAX &&
           bytes_zero(value->reserved_zero, sizeof(value->reserved_zero));
}

static bool rerr_reason_valid(uint8_t reason)
{
    return reason >= UCN_I_ROUTE_RERR_LINK_INVALID &&
           reason <= UCN_I_ROUTE_RERR_PATH_CONTRACT_CHANGED;
}

static bool rerr_payload_valid(const ucn_i_route_rerr_payload_t *value)
{
    return value != NULL && value->realm != 0U &&
           value->origin_address != 0U &&
           value->origin_binding_generation != 0U &&
           value->origin_session_generation != 0U &&
           value->destination_address != 0U &&
           value->destination_binding_generation != 0U &&
           value->route_generation != 0U && value->route_causal_id != 0U &&
           value->reporter_address != 0U &&
           value->reporter_binding_generation != 0U &&
           value->failed_link_id != 0U &&
           value->failed_link_id != UINT16_MAX &&
           value->failed_link_generation != 0U &&
           rerr_reason_valid(value->reason) && value->reserved_zero == 0U &&
           bytes_nonzero(value->origin_principal, 16U) &&
           bytes_nonzero(value->destination_principal, 16U) &&
           bytes_nonzero(value->reporter_principal, 16U);
}

static bool rreq_message_valid(const ucn_i_route_owner_t *owner,
                               const ucn_i_route_rreq_message_t *request)
{
    return request != NULL && key_valid(owner, &request->key) &&
           rreq_payload_valid(&request->payload) &&
           request->remaining_hops != 0U && request->remaining_hops <= 63U &&
           bytes_zero(request->reserved_zero, sizeof(request->reserved_zero));
}

static bool request_equal(const ucn_i_route_rreq_message_t *left,
                          const ucn_i_route_rreq_message_t *right)
{
    return key_equal(&left->key, &right->key) &&
           left->remaining_hops == right->remaining_hops &&
           left->payload.accumulated_cost ==
               right->payload.accumulated_cost &&
           left->payload.minimum_frame_mtu ==
               right->payload.minimum_frame_mtu &&
           left->payload.required_capability_bits ==
               right->payload.required_capability_bits &&
           left->payload.flags == right->payload.flags;
}

static void clear_discovery_value(ucn_i_route_discovery_record_t *record)
{
    uint16_t generation = record->generation;

    memset(record, 0, sizeof(*record));
    record->generation = generation;
}

static void clear_reverse_value(ucn_i_route_reverse_record_t *record)
{
    uint16_t generation = record->generation;

    memset(record, 0, sizeof(*record));
    record->generation = generation;
}

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

static bool control_key_valid(const ucn_i_route_discovery_key_t *key,
                              uint8_t width)
{
    size_t index;
    bool not_ff = false;
    for (index = 0U; index < sizeof(key->origin.principal); ++index) {
        not_ff = not_ff || key->origin.principal[index] != UINT8_MAX;
    }
    return key->realm != 0U && key->realm != UINT32_MAX &&
           binding_valid(&key->origin, width) &&
           not_ff && key->origin_session_generation != 0U &&
           address_valid(key->target_address, width) &&
           key->transaction_id != 0U;
}

static void control_key_encode(const ucn_i_route_discovery_key_t *key,
                               uint8_t bytes[44])
{
    put32(bytes, 0U, key->realm);
    put32(bytes, 4U, key->origin.address);
    put32(bytes, 8U, key->origin.generation);
    memcpy(&bytes[12], key->origin.principal, 16U);
    put32(bytes, 28U, key->origin_session_generation);
    put32(bytes, 32U, key->target_address);
    put64(bytes, 36U, key->transaction_id);
}

static void control_key_decode(const uint8_t bytes[44],
                               ucn_i_route_discovery_key_t *key)
{
    key->realm = get32(bytes, 0U);
    key->origin.address = get32(bytes, 4U);
    key->origin.generation = get32(bytes, 8U);
    memcpy(key->origin.principal, &bytes[12], 16U);
    key->origin_session_generation = get32(bytes, 28U);
    key->target_address = get32(bytes, 32U);
    key->transaction_id = get64(bytes, 36U);
}

ucn_result_t ucn_i_route_control_request_encode(
    const ucn_i_route_rreq_message_t *message, uint8_t address_width,
    uint8_t output[UCN_I_ROUTE_CONTROL_REQUEST_BYTES])
{
    uint8_t bytes[UCN_I_ROUTE_CONTROL_REQUEST_BYTES];
    if (message == NULL || output == NULL ||
        ucn_i_ranges_overlap(message, sizeof(*message), output, sizeof(bytes)) ||
        !control_key_valid(&message->key, address_width) ||
        message->remaining_hops == 0U || message->remaining_hops > 63U ||
        !bytes_zero(message->reserved_zero, sizeof(message->reserved_zero)) ||
        !rreq_payload_valid(&message->payload)) {
        return UCN_ERR_ARGUMENT;
    }
    control_key_encode(&message->key, bytes);
    bytes[44] = message->remaining_hops;
    (void)ucn_i_route_rreq_encode(&message->payload, &bytes[45]);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_route_control_request_decode(
    const uint8_t *input, size_t length, uint8_t address_width,
    ucn_i_route_rreq_message_t *message_out)
{
    ucn_i_route_rreq_message_t message;
    if (input == NULL || message_out == NULL ||
        ucn_i_ranges_overlap(input, length, message_out, sizeof(*message_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (length != UCN_I_ROUTE_CONTROL_REQUEST_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    memset(&message, 0, sizeof(message));
    control_key_decode(input, &message.key);
    message.remaining_hops = input[44];
    if (!control_key_valid(&message.key, address_width) ||
        message.remaining_hops == 0U || message.remaining_hops > 63U ||
        ucn_i_route_rreq_decode(&input[45], &message.payload) != UCN_OK) {
        return UCN_ERR_MALFORMED;
    }
    *message_out = message;
    return UCN_OK;
}

ucn_result_t ucn_i_route_control_reply_encode(
    const ucn_i_route_rrep_message_t *message, uint32_t reporter_generation,
    uint8_t address_width, uint8_t output[UCN_I_ROUTE_CONTROL_REPLY_BYTES])
{
    uint8_t bytes[UCN_I_ROUTE_CONTROL_REPLY_BYTES];
    if (message == NULL || output == NULL ||
        ucn_i_ranges_overlap(message, sizeof(*message), output, sizeof(bytes)) ||
        !control_key_valid(&message->key, address_width) ||
        !rrep_payload_valid(&message->payload) ||
        ((message->payload.hop_count == 0U) != (reporter_generation == 0U))) {
        return UCN_ERR_ARGUMENT;
    }
    control_key_encode(&message->key, bytes);
    (void)ucn_i_route_rrep_encode(&message->payload, &bytes[44]);
    put32(bytes, 73U, reporter_generation);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_route_control_reply_decode(
    const uint8_t *input, size_t length, uint8_t address_width,
    ucn_i_route_rrep_message_t *message_out, uint32_t *reporter_generation_out)
{
    ucn_i_route_rrep_message_t message;
    uint32_t generation;
    if (input == NULL || message_out == NULL || reporter_generation_out == NULL ||
        ucn_i_ranges_overlap(input, length, message_out, sizeof(*message_out)) ||
        ucn_i_ranges_overlap(input, length, reporter_generation_out,
                             sizeof(*reporter_generation_out)) ||
        ucn_i_ranges_overlap(message_out, sizeof(*message_out),
                             reporter_generation_out, sizeof(*reporter_generation_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (length != UCN_I_ROUTE_CONTROL_REPLY_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    memset(&message, 0, sizeof(message));
    control_key_decode(input, &message.key);
    generation = get32(input, 73U);
    if (!control_key_valid(&message.key, address_width) ||
        ucn_i_route_rrep_decode(&input[44], &message.payload) != UCN_OK ||
        ((message.payload.hop_count == 0U) != (generation == 0U))) {
        return UCN_ERR_MALFORMED;
    }
    *message_out = message;
    *reporter_generation_out = generation;
    return UCN_OK;
}

static bool route_fragment_valid(const ucn_i_route_fragment_t *v)
{
    size_t piece;
    size_t offset;
    size_t length;
    if (v == NULL || v->bundle == 0U || v->reserved_zero != 0U ||
        v->count < 2U || v->count > 4U || v->index >= v->count ||
        !((v->opcode == UINT16_C(0x0202) && v->total == 77U) ||
          (v->opcode == UINT16_C(0x0203) && v->total == 99U))) {
        return false;
    }
    piece = ((size_t)v->total + v->count - 1U) / v->count;
    offset = v->index * piece;
    length = (size_t)v->total - offset;
    if (length > piece) { length = piece; }
    return length == v->length &&
           bytes_zero(&v->chunk[length], sizeof(v->chunk) - length);
}

ucn_result_t ucn_i_route_fragment_encode(
    const ucn_i_route_fragment_t *fragment, uint8_t *output,
    size_t capacity, size_t *length_out)
{
    size_t length;
    uint8_t bytes[64];
    if (output == NULL || length_out == NULL || !route_fragment_valid(fragment) ||
        ucn_i_ranges_overlap(fragment, sizeof(*fragment), output, capacity) ||
        ucn_i_ranges_overlap(fragment, sizeof(*fragment), length_out, sizeof(*length_out)) ||
        ucn_i_ranges_overlap(output, capacity, length_out, sizeof(*length_out))) {
        return UCN_ERR_ARGUMENT;
    }
    length = UCN_I_ROUTE_FRAGMENT_PREFIX_BYTES + fragment->length;
    if (capacity < length) { return UCN_ERR_NO_SPACE; }
    put16(bytes, 0U, fragment->opcode);
    put16(bytes, 2U, fragment->total);
    bytes[4] = fragment->index;
    bytes[5] = fragment->count;
    put64(bytes, 6U, fragment->bundle);
    memcpy(&bytes[14], fragment->chunk, fragment->length);
    memcpy(output, bytes, length);
    *length_out = length;
    return UCN_OK;
}

ucn_result_t ucn_i_route_fragment_decode(
    const uint8_t *input, size_t length, ucn_i_route_fragment_t *fragment_out)
{
    ucn_i_route_fragment_t v;
    if (input == NULL || fragment_out == NULL ||
        ucn_i_ranges_overlap(input, length, fragment_out, sizeof(*fragment_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (length < 15U || length > 64U) { return UCN_ERR_MALFORMED; }
    memset(&v, 0, sizeof(v));
    v.opcode = get16(input, 0U);
    v.total = get16(input, 2U);
    v.index = input[4];
    v.count = input[5];
    v.bundle = get64(input, 6U);
    v.length = (uint8_t)(length - UCN_I_ROUTE_FRAGMENT_PREFIX_BYTES);
    memcpy(v.chunk, &input[14], v.length);
    if (!route_fragment_valid(&v)) { return UCN_ERR_MALFORMED; }
    *fragment_out = v;
    return UCN_OK;
}

ucn_result_t ucn_i_route_rreq_encode(const ucn_i_route_rreq_payload_t *value,
                                     uint8_t output[UCN_I_ROUTE_RREQ_BYTES])
{
    uint8_t bytes[UCN_I_ROUTE_RREQ_BYTES];

    if (!rreq_payload_valid(value) || output == NULL ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, sizeof(bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    put32(bytes, 0U, value->accumulated_cost);
    put16(bytes, 4U, value->minimum_frame_mtu);
    put16(bytes, 6U, value->required_capability_bits);
    bytes[8] = value->flags;
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_route_rreq_decode(const uint8_t input[UCN_I_ROUTE_RREQ_BYTES],
                                     ucn_i_route_rreq_payload_t *value_out)
{
    ucn_i_route_rreq_payload_t value;

    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_ROUTE_RREQ_BYTES, value_out,
                             sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    value.accumulated_cost = get32(input, 0U);
    value.minimum_frame_mtu = get16(input, 4U);
    value.required_capability_bits = get16(input, 6U);
    value.flags = input[8];
    if (!rreq_payload_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_route_rrep_encode(const ucn_i_route_rrep_payload_t *value,
                                     uint8_t output[UCN_I_ROUTE_RREP_BYTES])
{
    uint8_t bytes[UCN_I_ROUTE_RREP_BYTES];

    if (!rrep_payload_valid(value) || output == NULL ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, sizeof(bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    memcpy(bytes, value->destination_principal, 16U);
    put32(bytes, 16U, value->destination_binding_generation);
    bytes[20] = value->hop_count;
    put32(bytes, 21U, value->accumulated_cost);
    put16(bytes, 25U, value->path_frame_mtu);
    put16(bytes, 27U, value->capability_bits);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_route_rrep_decode(const uint8_t input[UCN_I_ROUTE_RREP_BYTES],
                                     ucn_i_route_rrep_payload_t *value_out)
{
    ucn_i_route_rrep_payload_t value;

    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_ROUTE_RREP_BYTES, value_out,
                             sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    memcpy(value.destination_principal, input, 16U);
    value.destination_binding_generation = get32(input, 16U);
    value.hop_count = input[20];
    value.accumulated_cost = get32(input, 21U);
    value.path_frame_mtu = get16(input, 25U);
    value.capability_bits = get16(input, 27U);
    if (!rrep_payload_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_route_rerr_encode(const ucn_i_route_rerr_payload_t *value,
                                     uint8_t output[UCN_I_ROUTE_RERR_BYTES])
{
    uint8_t bytes[UCN_I_ROUTE_RERR_BYTES];

    if (!rerr_payload_valid(value) || output == NULL ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, sizeof(bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    put32(bytes, 0U, value->realm);
    memcpy(&bytes[4], value->origin_principal, 16U);
    put32(bytes, 20U, value->origin_address);
    put32(bytes, 24U, value->origin_binding_generation);
    put32(bytes, 28U, value->origin_session_generation);
    memcpy(&bytes[32], value->destination_principal, 16U);
    put32(bytes, 48U, value->destination_address);
    put32(bytes, 52U, value->destination_binding_generation);
    put32(bytes, 56U, value->route_generation);
    put64(bytes, 60U, value->route_causal_id);
    memcpy(&bytes[68], value->reporter_principal, 16U);
    put32(bytes, 84U, value->reporter_address);
    put32(bytes, 88U, value->reporter_binding_generation);
    put16(bytes, 92U, value->failed_link_id);
    put32(bytes, 94U, value->failed_link_generation);
    bytes[98] = value->reason;
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_route_rerr_decode(const uint8_t input[UCN_I_ROUTE_RERR_BYTES],
                                     ucn_i_route_rerr_payload_t *value_out)
{
    ucn_i_route_rerr_payload_t value;

    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_ROUTE_RERR_BYTES, value_out,
                             sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    value.realm = get32(input, 0U);
    memcpy(value.origin_principal, &input[4], 16U);
    value.origin_address = get32(input, 20U);
    value.origin_binding_generation = get32(input, 24U);
    value.origin_session_generation = get32(input, 28U);
    memcpy(value.destination_principal, &input[32], 16U);
    value.destination_address = get32(input, 48U);
    value.destination_binding_generation = get32(input, 52U);
    value.route_generation = get32(input, 56U);
    value.route_causal_id = get64(input, 60U);
    memcpy(value.reporter_principal, &input[68], 16U);
    value.reporter_address = get32(input, 84U);
    value.reporter_binding_generation = get32(input, 88U);
    value.failed_link_id = get16(input, 92U);
    value.failed_link_generation = get32(input, 94U);
    value.reason = input[98];
    if (!rerr_payload_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

static bool config_valid(const ucn_i_route_config_t *config)
{
    return config != NULL && config->runtime_instance != 0U &&
           config->realm != 0U && config->realm != UINT32_MAX &&
           config->owner_instance != 0U &&
           binding_valid(&config->local, config->address_width) &&
           config->local_session_generation != 0U &&
           config->discovery_lifetime_us != 0U &&
           config->discovery_retry_us != 0U &&
           config->discovery_retry_us <= config->discovery_lifetime_us &&
           config->reverse_lifetime_us != 0U &&
           config->route_lifetime_us != 0U &&
           config->first_transaction_id != 0U &&
           config->discovery_max_attempts != 0U &&
           config->maximum_hops != 0U && config->maximum_hops <= 63U &&
           bytes_zero(config->reserved_zero, sizeof(config->reserved_zero));
}

ucn_result_t ucn_i_route_owner_init(ucn_i_route_owner_t *owner,
                                    const ucn_i_route_config_t *config,
                                    const ucn_i_lock_ops_t *state_lock)
{
    if (owner == NULL || !object_zero(owner, sizeof(*owner)) ||
        !config_valid(config) || !lock_valid(state_lock) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), state_lock,
                             sizeof(*state_lock))) {
        return UCN_ERR_ARGUMENT;
    }
    owner->magic = UCN_I_ROUTE_MAGIC;
    owner->runtime_instance = config->runtime_instance;
    owner->realm = config->realm;
    owner->local_session_generation = config->local_session_generation;
    owner->next_transaction_id = config->first_transaction_id;
    owner->discovery_lifetime_us = config->discovery_lifetime_us;
    owner->discovery_retry_us = config->discovery_retry_us;
    owner->reverse_lifetime_us = config->reverse_lifetime_us;
    owner->route_lifetime_us = config->route_lifetime_us;
    owner->local = config->local;
    owner->state_lock = *state_lock;
    owner->next_route_generation = 1U;
    owner->schema = UCN_I_ROUTE_SCHEMA;
    owner->owner_instance = config->owner_instance;
    owner->address_width = config->address_width;
    owner->discovery_max_attempts = config->discovery_max_attempts;
    owner->maximum_hops = config->maximum_hops;
    return UCN_OK;
}

ucn_result_t ucn_i_route_owner_destroy(ucn_i_route_owner_t *owner)
{
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    owner_unlock(owner);
    memset(owner, 0, sizeof(*owner));
    return UCN_OK;
}

static int route_slot_for(const ucn_i_route_owner_t *owner,
                          const ucn_i_route_domain_t *domain)
{
    size_t index;
    int free_index = -1;

    for (index = 0U; index < UCN_I_ROUTE_DYNAMIC_COUNT; ++index) {
        if (owner->routes[index].occupied != 0U &&
            domain_equal(&owner->routes[index].value.domain, domain)) {
            return (int)index;
        }
        if (free_index < 0 && owner->routes[index].occupied == 0U) {
            free_index = (int)index;
        }
    }
    return free_index;
}

ucn_result_t ucn_i_route_install_static(ucn_i_route_owner_t *owner,
                                        const ucn_i_route_static_entry_t *route)
{
    size_t index;
    size_t free_index = UCN_I_ROUTE_STATIC_COUNT;
    ucn_result_t result;

    if (route == NULL || ucn_i_ranges_overlap(owner, sizeof(*owner), route,
                                               sizeof(*route))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!binding_valid(&route->destination, owner->address_width) ||
        !link_valid(&route->next_hop, owner->address_width) ||
        route->path_frame_mtu == 0U ||
        route->path_frame_mtu > route->next_hop.frame_mtu ||
        route->reserved_zero != 0U ||
        binding_equal(&route->destination, &owner->local)) {
        result = UCN_ERR_ARGUMENT;
        goto done;
    }
    for (index = 0U; index < UCN_I_ROUTE_STATIC_COUNT; ++index) {
        if (owner->static_routes[index].occupied != 0U &&
            binding_equal(&owner->static_routes[index].value.destination,
                          &route->destination)) {
            owner->static_routes[index].value = *route;
            result = UCN_OK;
            goto done;
        }
        if (free_index == UCN_I_ROUTE_STATIC_COUNT &&
            owner->static_routes[index].occupied == 0U) {
            free_index = index;
        }
    }
    if (free_index == UCN_I_ROUTE_STATIC_COUNT) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    owner->static_routes[free_index].value = *route;
    owner->static_routes[free_index].occupied = 1U;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

static ucn_handle_t make_handle(const ucn_i_route_owner_t *owner,
                                size_t index,
                                uint16_t generation,
                                uint8_t kind)
{
    ucn_handle_t handle;

    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = (uint16_t)(index + 1U);
    handle.generation = generation;
    handle.object_kind = kind;
    return handle;
}

static bool route_handle_matches(const ucn_handle_t *handle,
                                 const ucn_i_route_owner_t *owner,
                                 uint16_t slot_limit,
                                 uint8_t expected_kind)
{
    return handle != NULL && handle->reserved_zero == 0U &&
           handle->runtime_instance == owner->runtime_instance &&
           handle->owner_instance == owner->owner_instance &&
           handle->slot != 0U && handle->slot <= slot_limit &&
           handle->generation != 0U &&
           handle->object_kind == expected_kind;
}

ucn_result_t ucn_i_route_ensure_discovery(ucn_i_route_owner_t *owner,
                                          uint32_t target_address,
                                          uint16_t required_capability_bits,
                                          uint16_t minimum_frame_mtu,
                                          uint8_t flags,
                                          uint64_t now_us,
                                          ucn_handle_t *handle_out,
                                          ucn_i_route_rreq_message_t *request_out,
                                          uint8_t *created_out)
{
    size_t index;
    size_t free_index = UCN_I_ROUTE_DISCOVERY_COUNT;
    ucn_i_route_discovery_record_t record;
    ucn_handle_t handle;
    uint64_t deadline;
    uint64_t retry;
    uint64_t transaction;
    uint16_t generation;
    ucn_result_t result;

    if (handle_out == NULL || request_out == NULL || created_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request_out,
                             sizeof(*request_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), created_out,
                             sizeof(*created_out)) ||
        ucn_i_ranges_overlap(handle_out, sizeof(*handle_out), request_out,
                             sizeof(*request_out)) ||
        ucn_i_ranges_overlap(handle_out, sizeof(*handle_out), created_out,
                             sizeof(*created_out)) ||
        ucn_i_ranges_overlap(request_out, sizeof(*request_out), created_out,
                             sizeof(*created_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!address_valid(target_address, owner->address_width) ||
        target_address == owner->local.address || minimum_frame_mtu == 0U ||
        (flags & UINT8_C(0xFC)) != 0U) {
        result = UCN_ERR_ARGUMENT;
        goto done;
    }
    for (index = 0U; index < UCN_I_ROUTE_DISCOVERY_COUNT; ++index) {
        if (owner->discoveries[index].occupied != 0U &&
            owner->discoveries[index].request.key.target_address ==
                target_address) {
            record = owner->discoveries[index];
            if (ucn_i_deadline_expired_us(now_us, record.deadline_us)) {
                result = UCN_ERR_TIMEOUT;
                goto done;
            }
            if (record.request.payload.required_capability_bits !=
                    required_capability_bits ||
                record.request.payload.minimum_frame_mtu !=
                    minimum_frame_mtu ||
                record.request.payload.flags != flags) {
                result = UCN_ERR_STATE;
                goto done;
            }
            handle = make_handle(owner, index, record.generation,
                                 UCN_I_ROUTE_DISCOVERY_KIND);
            *handle_out = handle;
            *request_out = record.request;
            *created_out = 0U;
            result = UCN_OK;
            goto done;
        }
        if (free_index == UCN_I_ROUTE_DISCOVERY_COUNT &&
            owner->discoveries[index].occupied == 0U) {
            free_index = index;
        }
    }
    if (free_index == UCN_I_ROUTE_DISCOVERY_COUNT) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    result = ucn_i_deadline_from_duration_us(
        now_us, owner->discovery_lifetime_us, &deadline);
    if (result != UCN_OK) {
        goto done;
    }
    result = ucn_i_deadline_from_duration_us(now_us,
                                             owner->discovery_retry_us,
                                             &retry);
    if (result != UCN_OK) {
        goto done;
    }
    transaction = owner->next_transaction_id;
    if (transaction == 0U ||
        owner->discoveries[free_index].generation == UINT16_MAX) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    generation = (uint16_t)(owner->discoveries[free_index].generation + 1U);
    memset(&record, 0, sizeof(record));
    record.request.key.origin = owner->local;
    record.request.key.transaction_id = transaction;
    record.request.key.realm = owner->realm;
    record.request.key.origin_session_generation =
        owner->local_session_generation;
    record.request.key.target_address = target_address;
    record.request.payload.minimum_frame_mtu = minimum_frame_mtu;
    record.request.payload.required_capability_bits =
        required_capability_bits;
    record.request.payload.flags = flags;
    record.request.remaining_hops = owner->maximum_hops;
    record.deadline_us = deadline;
    record.next_retry_us = retry;
    record.generation = generation;
    record.attempts = 1U;
    record.occupied = 1U;
    owner->discoveries[free_index] = record;
    owner->next_transaction_id =
        transaction == UINT64_MAX ? 0U : transaction + 1U;
    handle = make_handle(owner, free_index, generation,
                         UCN_I_ROUTE_DISCOVERY_KIND);
    *handle_out = handle;
    *request_out = record.request;
    *created_out = 1U;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_route_retry_discovery(ucn_i_route_owner_t *owner,
                                         ucn_handle_t handle,
                                         uint64_t now_us,
                                         ucn_i_route_rreq_message_t *request_out)
{
    size_t index;
    ucn_i_route_discovery_record_t record;
    uint64_t next_retry;
    ucn_result_t result;

    if (request_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request_out,
                             sizeof(*request_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!route_handle_matches(&handle, owner,
                              UCN_I_ROUTE_DISCOVERY_COUNT,
                              UCN_I_ROUTE_DISCOVERY_KIND)) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    index = (size_t)handle.slot - 1U;
    record = owner->discoveries[index];
    if (record.occupied == 0U || record.generation != handle.generation) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    if (ucn_i_deadline_expired_us(now_us, record.deadline_us)) {
        result = UCN_ERR_TIMEOUT;
        goto done;
    }
    if (now_us < record.next_retry_us) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (record.attempts >= owner->discovery_max_attempts) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    result = ucn_i_deadline_from_duration_us(record.next_retry_us,
                                             owner->discovery_retry_us,
                                             &next_retry);
    if (result != UCN_OK) {
        goto done;
    }
    record.next_retry_us = next_retry;
    record.attempts++;
    owner->discoveries[index] = record;
    *request_out = record.request;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

static int reverse_find(const ucn_i_route_owner_t *owner,
                        const ucn_i_route_discovery_key_t *key)
{
    size_t index;

    for (index = 0U; index < UCN_I_ROUTE_REVERSE_COUNT; ++index) {
        if (owner->reverse[index].occupied != 0U &&
            key_equal(&owner->reverse[index].accepted_request.key, key)) {
            return (int)index;
        }
    }
    return -1;
}

static int reverse_free(const ucn_i_route_owner_t *owner)
{
    size_t index;

    for (index = 0U; index < UCN_I_ROUTE_REVERSE_COUNT; ++index) {
        if (owner->reverse[index].occupied == 0U) {
            return (int)index;
        }
    }
    return -1;
}

ucn_result_t ucn_i_route_reverse_peek(ucn_i_route_owner_t *owner,
    const ucn_i_route_discovery_key_t *key, uint64_t now_us,
    ucn_i_route_link_ref_t *upstream_out)
{
    int index; ucn_result_t result;
    if (!key || !upstream_out ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), key, sizeof(*key)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), upstream_out, sizeof(*upstream_out)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), upstream_out, sizeof(*upstream_out))) { return UCN_ERR_ARGUMENT; }
    result = owner_lock(owner); if (result != UCN_OK) { return result; }
    index = key_valid(owner, key) ? reverse_find(owner, key) : -1;
    if (index < 0) { result = UCN_ERR_NOT_FOUND; }
    else if (now_us >= owner->reverse[index].expires_at_us ||
        owner->reverse[index].reply_pending || owner->reverse[index].reply_completed) { result = UCN_ERR_STATE; }
    else { *upstream_out = owner->reverse[index].upstream; result = UCN_OK; }
    owner_unlock(owner); return result;
}

static void commit_route(ucn_i_route_owner_t *owner,
                         size_t index,
                         const ucn_i_route_domain_t *domain,
                         const ucn_i_route_link_ref_t *next_hop,
                         uint32_t generation,
                         uint64_t causal_id,
                         uint8_t hop_count,
                         uint32_t cost,
                         uint16_t mtu,
                         uint16_t capabilities,
                         uint64_t expires_at_us)
{
    ucn_i_route_soft_view_t *value = &owner->routes[index].value;

    /* All validation precedes this locked commit. Build directly in the
     * fixed route slot, not in a full automatic SoftView copy. */
    memset(value, 0, sizeof(*value));
    value->domain = *domain;
    value->next_hop = *next_hop;
    value->route_causal_id = causal_id;
    value->expires_at_us = expires_at_us;
    value->runtime_instance = owner->runtime_instance;
    value->route_generation = generation;
    value->cost = cost;
    value->owner_instance = owner->owner_instance;
    value->path_frame_mtu = mtu;
    value->capability_bits = capabilities;
    value->hop_count = hop_count;
    owner->routes[index].reporter_generation = 0U;
    owner->routes[index].reporter_bound = 0U;
    owner->routes[index].occupied = 1U;
}

ucn_result_t ucn_i_route_on_rreq(ucn_i_route_owner_t *owner,
                                 const ucn_i_route_rreq_message_t *request,
                                 const ucn_i_route_link_ref_t *ingress,
                                 uint64_t now_us,
                                 ucn_i_route_request_action_t *action_out)
{
    ucn_i_route_reverse_record_t *reverse;
    ucn_i_route_domain_t *return_domain;
    uint64_t reverse_deadline;
    uint64_t route_deadline;
    uint64_t retry_at;
    uint32_t route_generation;
    uint32_t cost;
    int reverse_index;
    int route_index;
    ucn_result_t result;

    if (request == NULL || ingress == NULL || action_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), ingress,
                             sizeof(*ingress)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), action_out,
                             sizeof(*action_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), action_out,
                             sizeof(*action_out)) ||
        ucn_i_ranges_overlap(ingress, sizeof(*ingress), action_out,
                             sizeof(*action_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    return_domain = &owner->work_domain;
    if (!rreq_message_valid(owner, request) ||
        !link_valid(ingress, owner->address_width) ||
        binding_equal(&request->key.origin, &owner->local) ||
        binding_equal(&ingress->peer, &owner->local) ||
        request->payload.minimum_frame_mtu > ingress->frame_mtu ||
        (request->payload.required_capability_bits &
         (uint16_t)~ingress->capability_bits) != 0U) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    reverse_index = reverse_find(owner, &request->key);
    if (reverse_index >= 0) {
        reverse = &owner->reverse[reverse_index];
        if (!request_equal(&reverse->accepted_request, request) ||
            !link_equal(&reverse->upstream, ingress)) {
            result = UCN_ERR_SECURITY;
            goto done;
        }
        if (ucn_i_deadline_expired_us(now_us, reverse->expires_at_us) ||
            reverse->reply_pending != 0U) {
            memset(action_out, 0, sizeof(*action_out));
            action_out->kind = UCN_I_ROUTE_REQUEST_DUPLICATE;
            result = UCN_OK;
            goto done;
        }
        result = ucn_i_deadline_from_duration_us(
            reverse->last_action_us, owner->discovery_retry_us, &retry_at);
        if (result != UCN_OK) {
            goto done;
        }
        if (now_us < retry_at) {
            memset(action_out, 0, sizeof(*action_out));
            action_out->kind = UCN_I_ROUTE_REQUEST_DUPLICATE;
            result = UCN_OK;
            goto done;
        }
        if (request->key.target_address == owner->local.address) {
            memset(action_out, 0, sizeof(*action_out));
            action_out->kind = UCN_I_ROUTE_REQUEST_LOCAL_TARGET;
        } else if (request->remaining_hops <= 1U ||
                   UINT32_MAX - request->payload.accumulated_cost <
                       ingress->cost) {
            result = UCN_ERR_EXHAUSTED;
            goto done;
        } else {
            memset(action_out, 0, sizeof(*action_out));
            action_out->kind = UCN_I_ROUTE_REQUEST_FORWARD;
            action_out->forwarded = *request;
            action_out->forwarded.remaining_hops--;
            action_out->forwarded.payload.accumulated_cost += ingress->cost;
        }
        action_out->reverse_expires_at_us = reverse->expires_at_us;
        reverse->last_action_us = now_us;
        reverse->reply_completed = 0U;
        result = UCN_OK;
        goto done;
    }
    if (request->key.target_address != owner->local.address &&
        request->remaining_hops <= 1U) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    reverse_index = reverse_free(owner);
    if (reverse_index < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    memset(return_domain, 0, sizeof(*return_domain));
    return_domain->origin = owner->local;
    return_domain->destination = request->key.origin;
    return_domain->realm = owner->realm;
    return_domain->origin_session_generation = owner->local_session_generation;
    route_index = route_slot_for(owner, return_domain);
    if (route_index < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    if (owner->next_route_generation == 0U ||
        owner->reverse[reverse_index].generation == UINT16_MAX) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    result = ucn_i_deadline_from_duration_us(now_us,
                                             owner->reverse_lifetime_us,
                                             &reverse_deadline);
    if (result != UCN_OK) {
        goto done;
    }
    result = ucn_i_deadline_from_duration_us(now_us,
                                             owner->route_lifetime_us,
                                             &route_deadline);
    if (result != UCN_OK) {
        goto done;
    }
    if (UINT32_MAX - request->payload.accumulated_cost < ingress->cost) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    cost = request->payload.accumulated_cost + ingress->cost;
    route_generation = owner->next_route_generation;
    reverse = &owner->reverse[reverse_index];
    ++reverse->generation;
    reverse->accepted_request = *request;
    reverse->upstream = *ingress;
    reverse->expires_at_us = reverse_deadline;
    reverse->last_action_us = now_us;
    reverse->occupied = 1U;
    commit_route(owner, (size_t)route_index, return_domain, ingress,
                 route_generation, request->key.transaction_id, 1U,
                 ingress->cost, ingress->frame_mtu,
                 request->payload.required_capability_bits, route_deadline);
    owner->next_route_generation =
        route_generation == UINT32_MAX ? 0U : route_generation + 1U;
    memset(action_out, 0, sizeof(*action_out));
    action_out->reverse_expires_at_us = reverse_deadline;
    if (request->key.target_address == owner->local.address) {
        action_out->kind = UCN_I_ROUTE_REQUEST_LOCAL_TARGET;
    } else {
        action_out->kind = UCN_I_ROUTE_REQUEST_FORWARD;
        action_out->forwarded = *request;
        action_out->forwarded.remaining_hops--;
        action_out->forwarded.payload.accumulated_cost = cost;
    }
    result = UCN_OK;
done:
    memset(return_domain, 0, sizeof(*return_domain));
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_route_make_rrep(ucn_i_route_owner_t *owner,
                                   const ucn_i_route_rreq_message_t *request,
                                   uint16_t local_capability_bits,
                                   uint16_t local_frame_mtu,
                                   uint64_t now_us,
                                   ucn_i_route_rrep_message_t *reply_out)
{
    size_t index;
    ucn_i_route_rrep_message_t reply;
    ucn_result_t result;

    if (request == NULL || reply_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), reply_out,
                             sizeof(*reply_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), reply_out,
                             sizeof(*reply_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!rreq_message_valid(owner, request) ||
        request->key.target_address != owner->local.address ||
        local_frame_mtu == 0U ||
        local_frame_mtu < request->payload.minimum_frame_mtu ||
        (request->payload.required_capability_bits &
         (uint16_t)~local_capability_bits) != 0U) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    for (index = 0U; index < UCN_I_ROUTE_REVERSE_COUNT; ++index) {
        if (owner->reverse[index].occupied != 0U &&
            key_equal(&owner->reverse[index].accepted_request.key,
                      &request->key)) {
            if (!request_equal(&owner->reverse[index].accepted_request,
                               request)) {
                result = UCN_ERR_SECURITY;
                goto done;
            }
            if (ucn_i_deadline_expired_us(
                    now_us, owner->reverse[index].expires_at_us)) {
                result = UCN_ERR_TIMEOUT;
                goto done;
            }
            memset(&reply, 0, sizeof(reply));
            reply.key = request->key;
            memcpy(reply.payload.destination_principal,
                   owner->local.principal, 16U);
            reply.payload.destination_binding_generation =
                owner->local.generation;
            reply.payload.path_frame_mtu = local_frame_mtu;
            reply.payload.capability_bits = local_capability_bits;
            *reply_out = reply;
            result = UCN_OK;
            goto done;
        }
    }
    result = UCN_ERR_NOT_FOUND;
done:
    owner_unlock(owner);
    return result;
}

static int discovery_find(const ucn_i_route_owner_t *owner,
                          const ucn_i_route_discovery_key_t *key)
{
    size_t index;

    for (index = 0U; index < UCN_I_ROUTE_DISCOVERY_COUNT; ++index) {
        if (owner->discoveries[index].occupied != 0U &&
            key_equal(&owner->discoveries[index].request.key, key)) {
            return (int)index;
        }
    }
    return -1;
}

ucn_result_t ucn_i_route_apply_reply(ucn_i_route_owner_t *owner,
                                 const ucn_i_route_rrep_message_t *reply,
                                 const ucn_i_route_link_ref_t *ingress,
                                 uint64_t now_us,
                                 ucn_i_route_reply_action_t *action_out,
                                 bool bind_reporter,
                                 uint32_t reporter_generation)
{
    ucn_i_route_domain_t *domain;
    const ucn_i_route_rreq_message_t *accepted;
    uint64_t route_deadline;
    uint32_t cost;
    uint32_t route_generation;
    uint16_t path_mtu;
    uint16_t capability_bits;
    uint8_t hop_count;
    int discovery_index;
    int reverse_index;
    int route_index;
    bool local_origin;
    ucn_result_t result;

    if (reply == NULL || ingress == NULL || action_out == NULL ||
        (bind_reporter &&
         ((reply->payload.hop_count == 0U) != (reporter_generation == 0U))) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), reply,
                             sizeof(*reply)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), ingress,
                             sizeof(*ingress)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), action_out,
                             sizeof(*action_out)) ||
        ucn_i_ranges_overlap(reply, sizeof(*reply), action_out,
                             sizeof(*action_out)) ||
        ucn_i_ranges_overlap(ingress, sizeof(*ingress), action_out,
                             sizeof(*action_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    domain = &owner->work_domain;
    if (!key_valid(owner, &reply->key) ||
        !rrep_payload_valid(&reply->payload) ||
        !link_valid(ingress, owner->address_width)) {
        result = UCN_ERR_MALFORMED;
        goto done;
    }
    local_origin = binding_equal(&reply->key.origin, &owner->local) &&
                   reply->key.origin_session_generation ==
                       owner->local_session_generation;
    discovery_index = local_origin ? discovery_find(owner, &reply->key) : -1;
    reverse_index = local_origin ? -1 : reverse_find(owner, &reply->key);
    if ((local_origin && discovery_index < 0) ||
        (!local_origin && reverse_index < 0)) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    if (local_origin) {
        if (ucn_i_deadline_expired_us(
                now_us, owner->discoveries[discovery_index].deadline_us)) {
            result = UCN_ERR_TIMEOUT;
            goto done;
        }
        accepted = &owner->discoveries[discovery_index].request;
    } else {
        if (ucn_i_deadline_expired_us(
                now_us, owner->reverse[reverse_index].expires_at_us) ||
            owner->reverse[reverse_index].reply_pending != 0U ||
            owner->reverse[reverse_index].reply_completed != 0U) {
            result = UCN_ERR_STATE;
            goto done;
        }
        accepted = &owner->reverse[reverse_index].accepted_request;
    }
    memset(domain, 0, sizeof(*domain));
    domain->destination.address = reply->key.target_address;
    domain->destination.generation = reply->payload.destination_binding_generation;
    memcpy(domain->destination.principal, reply->payload.destination_principal, 16U);
    if (!binding_valid(&domain->destination, owner->address_width)) {
        result = UCN_ERR_MALFORMED;
        goto done;
    }
    domain->origin = reply->key.origin;
    domain->realm = reply->key.realm;
    domain->origin_session_generation =
        reply->key.origin_session_generation;
    route_index = route_slot_for(owner, domain);
    if (route_index < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    if (owner->next_route_generation == 0U ||
        reply->payload.hop_count >= owner->maximum_hops ||
        UINT32_MAX - reply->payload.accumulated_cost < ingress->cost) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    hop_count = (uint8_t)(reply->payload.hop_count + 1U);
    cost = reply->payload.accumulated_cost + ingress->cost;
    path_mtu = reply->payload.path_frame_mtu < ingress->frame_mtu ?
                   reply->payload.path_frame_mtu : ingress->frame_mtu;
    capability_bits =
        (uint16_t)(reply->payload.capability_bits & ingress->capability_bits);
    if (path_mtu < accepted->payload.minimum_frame_mtu ||
        (accepted->payload.required_capability_bits &
         (uint16_t)~capability_bits) != 0U) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    result = ucn_i_deadline_from_duration_us(now_us,
                                             owner->route_lifetime_us,
                                             &route_deadline);
    if (result != UCN_OK) {
        goto done;
    }
    route_generation = owner->next_route_generation;
    memset(action_out, 0, sizeof(*action_out));
    /* All rejection points have passed. The caller-owned action is the
     * commit workspace, avoiding an eleven-argument stack staging call. */
    action_out->installed.domain = *domain;
    action_out->installed.next_hop = *ingress;
    action_out->installed.route_causal_id = reply->key.transaction_id;
    action_out->installed.expires_at_us = route_deadline;
    action_out->installed.runtime_instance = owner->runtime_instance;
    action_out->installed.route_generation = route_generation;
    action_out->installed.cost = cost;
    action_out->installed.owner_instance = owner->owner_instance;
    action_out->installed.path_frame_mtu = path_mtu;
    action_out->installed.capability_bits = capability_bits;
    action_out->installed.hop_count = hop_count;
    owner->routes[route_index].value = action_out->installed;
    owner->routes[route_index].occupied = 1U;
    owner->routes[route_index].reporter_bound = bind_reporter ? 1U : 0U;
    owner->routes[route_index].reporter_generation = reporter_generation;
    owner->next_route_generation =
        route_generation == UINT32_MAX ? 0U : route_generation + 1U;
    if (local_origin) {
        clear_discovery_value(&owner->discoveries[discovery_index]);
        action_out->kind = UCN_I_ROUTE_REPLY_REACHED_ORIGIN;
    } else {
        action_out->kind = UCN_I_ROUTE_REPLY_FORWARD;
        action_out->forwarded = *reply;
        action_out->forwarded.payload.hop_count = hop_count;
        action_out->forwarded.payload.accumulated_cost = cost;
        action_out->forwarded.payload.path_frame_mtu = path_mtu;
        action_out->forwarded.payload.capability_bits = capability_bits;
        action_out->upstream = owner->reverse[reverse_index].upstream;
        action_out->reverse_expires_at_us = owner->reverse[reverse_index].expires_at_us;
        owner->reverse[reverse_index].reply_pending = 1U;
        action_out->completion = make_handle(
            owner, (size_t)reverse_index,
            owner->reverse[reverse_index].generation,
            UCN_I_ROUTE_REPLY_KIND);
    }
    result = UCN_OK;
done:
    memset(domain, 0, sizeof(*domain));
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_route_on_rrep(ucn_i_route_owner_t *owner,
    const ucn_i_route_rrep_message_t *reply,
    const ucn_i_route_link_ref_t *ingress, uint64_t now_us,
    ucn_i_route_reply_action_t *action_out)
{
    return ucn_i_route_apply_reply(owner, reply, ingress, now_us, action_out, false, 0U);
}

ucn_result_t ucn_i_route_on_control_rrep(ucn_i_route_owner_t *owner,
    const ucn_i_route_rrep_message_t *reply, uint32_t reporter_generation,
    const ucn_i_route_link_ref_t *ingress, uint64_t now_us,
    ucn_i_route_reply_action_t *action_out)
{
    return ucn_i_route_apply_reply(owner, reply, ingress, now_us, action_out,
                         true, reporter_generation);
}

ucn_result_t ucn_i_route_complete_rrep_forward(ucn_i_route_owner_t *owner,
                                               ucn_handle_t completion,
                                               uint8_t delivered)
{
    size_t index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if ((delivered != 0U && delivered != 1U) ||
        !route_handle_matches(&completion, owner,
                              UCN_I_ROUTE_REVERSE_COUNT,
                              UCN_I_ROUTE_REPLY_KIND)) {
        result = UCN_ERR_ARGUMENT;
        goto done;
    }
    index = (size_t)completion.slot - 1U;
    if (owner->reverse[index].occupied == 0U ||
        owner->reverse[index].generation != completion.generation ||
        owner->reverse[index].reply_pending == 0U) {
        result = UCN_ERR_STATE;
        goto done;
    }
    owner->reverse[index].reply_pending = 0U;
    owner->reverse[index].reply_completed = delivered;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_route_resolve(ucn_i_route_owner_t *owner,
                                 const ucn_i_route_domain_t *domain,
                                 const ucn_i_route_use_facts_t *facts,
                                 ucn_i_route_resolved_t *resolved_out)
{
    size_t index;
    ucn_i_route_resolved_t resolved;
    ucn_result_t result;

    if (domain == NULL || facts == NULL || resolved_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), domain,
                             sizeof(*domain)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), resolved_out,
                             sizeof(*resolved_out)) ||
        ucn_i_ranges_overlap(domain, sizeof(*domain), resolved_out,
                             sizeof(*resolved_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), resolved_out,
                             sizeof(*resolved_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!domain_valid(owner, domain)) {
        result = UCN_ERR_ARGUMENT;
        goto done;
    }
    if (facts->origin_session_generation !=
            domain->origin_session_generation ||
        facts->destination_binding_generation !=
            domain->destination.generation) {
        result = UCN_ERR_REPLAY;
        goto done;
    }
    memset(&resolved, 0, sizeof(resolved));
    for (index = 0U; index < UCN_I_ROUTE_DYNAMIC_COUNT; ++index) {
        if (owner->routes[index].occupied != 0U &&
            domain_equal(&owner->routes[index].value.domain, domain) &&
            !ucn_i_deadline_expired_us(
                facts->now_us, owner->routes[index].value.expires_at_us) &&
            facts->link_generation ==
                owner->routes[index].value.next_hop.link_generation) {
            resolved.kind = UCN_I_ROUTE_RESOLVED_DYNAMIC;
            resolved.dynamic = owner->routes[index].value;
            *resolved_out = resolved;
            result = UCN_OK;
            goto done;
        }
    }
    for (index = 0U; index < UCN_I_ROUTE_STATIC_COUNT; ++index) {
        if (owner->static_routes[index].occupied != 0U &&
            binding_equal(&owner->static_routes[index].value.destination,
                          &domain->destination)) {
            if (owner->static_routes[index].value.next_hop.link_generation !=
                facts->link_generation) {
                result = UCN_ERR_STATE;
                goto done;
            }
            resolved.kind = UCN_I_ROUTE_RESOLVED_STATIC;
            resolved.static_route = owner->static_routes[index].value;
            *resolved_out = resolved;
            result = UCN_OK;
            goto done;
        }
    }
    result = UCN_ERR_NOT_FOUND;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_route_forward_resolve(
    ucn_i_route_owner_t *owner,
    const ucn_i_route_forward_key_t *key,
    ucn_i_route_resolved_t *resolved_out)
{
    size_t index;
    size_t match_index = SIZE_MAX;
    ucn_result_t result;

    if (owner == NULL || key == NULL || resolved_out == NULL ||
        key->now_us == 0U || key->realm == 0U ||
        key->source_address == 0U ||
        key->source_binding_generation == 0U ||
        key->destination_address == 0U ||
        key->destination_binding_generation == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), key, sizeof(*key)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), resolved_out,
                             sizeof(*resolved_out)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), resolved_out,
                             sizeof(*resolved_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (key->realm != owner->realm ||
        !address_valid(key->source_address, owner->address_width) ||
        !address_valid(key->destination_address, owner->address_width)) {
        result = UCN_ERR_ARGUMENT;
        goto done;
    }
    for (index = 0U; index < UCN_I_ROUTE_DYNAMIC_COUNT; ++index) {
        const ucn_i_route_soft_view_t *route = &owner->routes[index].value;

        if (owner->routes[index].occupied == 0U ||
            ucn_i_deadline_expired_us(key->now_us, route->expires_at_us) ||
            route->domain.realm != key->realm ||
            route->domain.origin.address != key->source_address ||
            route->domain.origin.generation !=
                key->source_binding_generation ||
            route->domain.destination.address !=
                key->destination_address ||
            route->domain.destination.generation !=
                key->destination_binding_generation) {
            continue;
        }
        if (match_index != SIZE_MAX) {
            result = UCN_ERR_STATE;
            goto done;
        }
        match_index = index;
    }
    if (match_index != SIZE_MAX) {
        memset(resolved_out, 0, sizeof(*resolved_out));
        resolved_out->kind = UCN_I_ROUTE_RESOLVED_DYNAMIC;
        resolved_out->dynamic = owner->routes[match_index].value;
        result = UCN_OK;
        goto done;
    }
    match_index = SIZE_MAX;
    for (index = 0U; index < UCN_I_ROUTE_STATIC_COUNT; ++index) {
        const ucn_i_route_static_entry_t *route =
            &owner->static_routes[index].value;

        if (owner->static_routes[index].occupied == 0U ||
            route->destination.address != key->destination_address ||
            route->destination.generation !=
                key->destination_binding_generation) {
            continue;
        }
        if (match_index != SIZE_MAX) {
            result = UCN_ERR_STATE;
            goto done;
        }
        match_index = index;
    }
    if (match_index == SIZE_MAX) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    memset(resolved_out, 0, sizeof(*resolved_out));
    resolved_out->kind = UCN_I_ROUTE_RESOLVED_STATIC;
    resolved_out->static_route = owner->static_routes[match_index].value;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_route_invalidate_link(ucn_i_route_owner_t *owner,
                                         uint16_t link_id,
                                         uint32_t link_generation,
                                         uint16_t *invalidated_out)
{
    size_t index;
    uint16_t invalidated = 0U;
    ucn_result_t result;

    if (invalidated_out == NULL || link_id == 0U ||
        link_id == UINT16_MAX || link_generation == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_i_ranges_overlap(owner, sizeof(*owner), invalidated_out,
                             sizeof(*invalidated_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_ROUTE_DYNAMIC_COUNT; ++index) {
        if (owner->routes[index].occupied != 0U &&
            owner->routes[index].value.next_hop.link_id == link_id &&
            owner->routes[index].value.next_hop.link_generation ==
                link_generation) {
            memset(&owner->routes[index], 0, sizeof(owner->routes[index]));
            invalidated++;
        }
    }
    *invalidated_out = invalidated;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_route_retire_peer_history(
    ucn_i_route_owner_t *owner,
    const ucn_i_route_link_ref_t *peer,
    uint16_t *retired_out)
{
    size_t i;
    uint16_t count = 0U;
    ucn_result_t result;
    if (owner == NULL || peer == NULL || retired_out == NULL ||
        ucn_i_ranges_overlap(owner,sizeof(*owner),peer,sizeof(*peer)) ||
        ucn_i_ranges_overlap(owner,sizeof(*owner),retired_out,sizeof(*retired_out)) ||
        ucn_i_ranges_overlap(peer,sizeof(*peer),retired_out,sizeof(*retired_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result=owner_lock(owner); if (result != UCN_OK) { return result; }
    if (!link_valid(peer,owner->address_width)) {
        owner_unlock(owner); return UCN_ERR_ARGUMENT;
    }
    for (i=0U;i<UCN_I_ROUTE_REVERSE_COUNT;++i) {
        if (owner->reverse[i].occupied != 0U &&
            link_equal(&owner->reverse[i].upstream,peer) &&
            owner->reverse[i].reply_pending != 0U) {
            owner_unlock(owner); return UCN_ERR_STATE;
        }
    }
    for (i=0U;i<UCN_I_ROUTE_DYNAMIC_COUNT;++i) {
        if (owner->routes[i].occupied != 0U &&
            link_equal(&owner->routes[i].value.next_hop,peer)) {
            memset(&owner->routes[i],0,sizeof(owner->routes[i])); ++count;
        }
    }
    for (i=0U;i<UCN_I_ROUTE_REVERSE_COUNT;++i) {
        if (owner->reverse[i].occupied != 0U &&
            link_equal(&owner->reverse[i].upstream,peer)) {
            clear_reverse_value(&owner->reverse[i]); ++count;
        }
    }
    *retired_out=count; owner_unlock(owner); return UCN_OK;
}

ucn_result_t ucn_i_route_make_rerr(ucn_i_route_owner_t *owner,
                                   const ucn_i_route_soft_view_t *route,
                                   ucn_i_route_rerr_reason_t reason,
                                   ucn_i_route_rerr_payload_t *payload_out)
{
    ucn_i_route_rerr_payload_t payload;
    ucn_result_t result;

    if (route == NULL || payload_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), route,
                             sizeof(*route)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload_out,
                             sizeof(*payload_out)) ||
        ucn_i_ranges_overlap(route, sizeof(*route), payload_out,
                             sizeof(*payload_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (route->runtime_instance != owner->runtime_instance ||
        route->owner_instance != owner->owner_instance ||
        !domain_valid(owner, &route->domain) ||
        !link_valid(&route->next_hop, owner->address_width) ||
        route->route_generation == 0U || route->route_causal_id == 0U ||
        !rerr_reason_valid(reason)) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    memset(&payload, 0, sizeof(payload));
    payload.realm = route->domain.realm;
    memcpy(payload.origin_principal, route->domain.origin.principal, 16U);
    payload.origin_address = route->domain.origin.address;
    payload.origin_binding_generation = route->domain.origin.generation;
    payload.origin_session_generation =
        route->domain.origin_session_generation;
    memcpy(payload.destination_principal,
           route->domain.destination.principal, 16U);
    payload.destination_address = route->domain.destination.address;
    payload.destination_binding_generation =
        route->domain.destination.generation;
    payload.route_generation = route->route_generation;
    payload.route_causal_id = route->route_causal_id;
    memcpy(payload.reporter_principal, route->next_hop.peer.principal, 16U);
    payload.reporter_address = route->next_hop.peer.address;
    payload.reporter_binding_generation = route->next_hop.peer.generation;
    payload.failed_link_id = route->next_hop.link_id;
    payload.failed_link_generation = route->next_hop.link_generation;
    payload.reason = reason;
    *payload_out = payload;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

static ucn_result_t route_on_rerr(ucn_i_route_owner_t *owner,
                                 const ucn_i_route_rerr_payload_t *payload,
                                 uint8_t *invalidated_out, bool from_wire)
{
    ucn_i_route_domain_t domain;
    size_t index;
    ucn_result_t result;

    if (payload == NULL || invalidated_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload,
                             sizeof(*payload)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), invalidated_out,
                             sizeof(*invalidated_out)) ||
        ucn_i_ranges_overlap(payload, sizeof(*payload), invalidated_out,
                             sizeof(*invalidated_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!rerr_payload_valid(payload)) {
        result = UCN_ERR_MALFORMED;
        goto done;
    }
    memset(&domain, 0, sizeof(domain));
    domain.realm = payload->realm;
    domain.origin.address = payload->origin_address;
    domain.origin.generation = payload->origin_binding_generation;
    memcpy(domain.origin.principal, payload->origin_principal, 16U);
    domain.origin_session_generation = payload->origin_session_generation;
    domain.destination.address = payload->destination_address;
    domain.destination.generation = payload->destination_binding_generation;
    memcpy(domain.destination.principal, payload->destination_principal, 16U);
    if (!domain_valid(owner, &domain)) {
        result = UCN_ERR_MALFORMED;
        goto done;
    }
    for (index = 0U; index < UCN_I_ROUTE_DYNAMIC_COUNT; ++index) {
        ucn_i_route_soft_view_t *route = &owner->routes[index].value;

        if (owner->routes[index].occupied != 0U &&
            domain_equal(&route->domain, &domain) &&
            (from_wire ?
                (owner->routes[index].reporter_bound != 0U &&
                 owner->routes[index].reporter_generation != 0U &&
                 owner->routes[index].reporter_generation == payload->route_generation) :
                route->route_generation == payload->route_generation) &&
            route->route_causal_id == payload->route_causal_id &&
            (from_wire ||
             (route->next_hop.link_id == payload->failed_link_id &&
              route->next_hop.link_generation == payload->failed_link_generation)) &&
            route->next_hop.peer.address == payload->reporter_address &&
            route->next_hop.peer.generation ==
                payload->reporter_binding_generation &&
            memcmp(route->next_hop.peer.principal,
                   payload->reporter_principal, 16U) == 0) {
            memset(&owner->routes[index], 0, sizeof(owner->routes[index]));
            *invalidated_out = 1U;
            owner_unlock(owner);
            return UCN_OK;
        }
    }
    *invalidated_out = 0U;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_route_on_rerr(ucn_i_route_owner_t *owner,
    const ucn_i_route_rerr_payload_t *payload, uint8_t *invalidated_out)
{
    return route_on_rerr(owner, payload, invalidated_out, false);
}

ucn_result_t ucn_i_route_on_wire_rerr(ucn_i_route_owner_t *owner,
    const ucn_i_route_rerr_payload_t *payload, uint8_t *invalidated_out)
{
    return route_on_rerr(owner, payload, invalidated_out, true);
}

ucn_result_t ucn_i_route_maintain(ucn_i_route_owner_t *owner,
                                  uint64_t now_us,
                                  uint16_t budget,
                                  uint16_t *inspected_out,
                                  uint16_t *expired_out)
{
    const uint16_t total = (uint16_t)(UCN_I_ROUTE_DISCOVERY_COUNT +
                                      UCN_I_ROUTE_REVERSE_COUNT +
                                      UCN_I_ROUTE_DYNAMIC_COUNT);
    uint16_t inspected = 0U;
    uint16_t expired = 0U;
    ucn_result_t result;

    if (budget == 0U || inspected_out == NULL || expired_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), expired_out,
                             sizeof(*expired_out)) ||
        ucn_i_ranges_overlap(inspected_out, sizeof(*inspected_out),
                             expired_out, sizeof(*expired_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    while (inspected < budget && inspected < total) {
        uint16_t cursor = owner->maintenance_cursor;

        owner->maintenance_cursor = (uint16_t)((cursor + 1U) % total);
        if (cursor < UCN_I_ROUTE_DISCOVERY_COUNT) {
            if (owner->discoveries[cursor].occupied != 0U &&
                ucn_i_deadline_expired_us(
                    now_us, owner->discoveries[cursor].deadline_us)) {
                clear_discovery_value(&owner->discoveries[cursor]);
                expired++;
            }
        } else if (cursor < UCN_I_ROUTE_DISCOVERY_COUNT +
                              UCN_I_ROUTE_REVERSE_COUNT) {
            uint16_t index =
                (uint16_t)(cursor - UCN_I_ROUTE_DISCOVERY_COUNT);
            if (owner->reverse[index].occupied != 0U &&
                owner->reverse[index].reply_pending == 0U &&
                ucn_i_deadline_expired_us(
                    now_us, owner->reverse[index].expires_at_us)) {
                clear_reverse_value(&owner->reverse[index]);
                expired++;
            }
        } else {
            uint16_t index = (uint16_t)(
                cursor - UCN_I_ROUTE_DISCOVERY_COUNT -
                UCN_I_ROUTE_REVERSE_COUNT);
            if (owner->routes[index].occupied != 0U &&
                ucn_i_deadline_expired_us(
                    now_us, owner->routes[index].value.expires_at_us)) {
                memset(&owner->routes[index], 0,
                       sizeof(owner->routes[index]));
                expired++;
            }
        }
        inspected++;
    }
    *inspected_out = inspected;
    *expired_out = expired;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_route_counts(ucn_i_route_owner_t *owner,
                                uint16_t *discoveries_out,
                                uint16_t *reverse_out,
                                uint16_t *routes_out,
                                uint16_t *static_out)
{
    size_t index;
    uint16_t discoveries = 0U;
    uint16_t reverse = 0U;
    uint16_t routes = 0U;
    uint16_t statics = 0U;
    ucn_result_t result;

    if (discoveries_out == NULL || reverse_out == NULL ||
        routes_out == NULL || static_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), discoveries_out,
                             sizeof(*discoveries_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), reverse_out,
                             sizeof(*reverse_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), routes_out,
                             sizeof(*routes_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), static_out,
                             sizeof(*static_out)) ||
        ucn_i_ranges_overlap(discoveries_out, sizeof(*discoveries_out),
                             reverse_out, sizeof(*reverse_out)) ||
        ucn_i_ranges_overlap(discoveries_out, sizeof(*discoveries_out),
                             routes_out, sizeof(*routes_out)) ||
        ucn_i_ranges_overlap(discoveries_out, sizeof(*discoveries_out),
                             static_out, sizeof(*static_out)) ||
        ucn_i_ranges_overlap(reverse_out, sizeof(*reverse_out),
                             routes_out, sizeof(*routes_out)) ||
        ucn_i_ranges_overlap(reverse_out, sizeof(*reverse_out),
                             static_out, sizeof(*static_out)) ||
        ucn_i_ranges_overlap(routes_out, sizeof(*routes_out),
                             static_out, sizeof(*static_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_ROUTE_DISCOVERY_COUNT; ++index) {
        discoveries += owner->discoveries[index].occupied != 0U ? 1U : 0U;
    }
    for (index = 0U; index < UCN_I_ROUTE_REVERSE_COUNT; ++index) {
        reverse += owner->reverse[index].occupied != 0U ? 1U : 0U;
    }
    for (index = 0U; index < UCN_I_ROUTE_DYNAMIC_COUNT; ++index) {
        routes += owner->routes[index].occupied != 0U ? 1U : 0U;
    }
    for (index = 0U; index < UCN_I_ROUTE_STATIC_COUNT; ++index) {
        statics += owner->static_routes[index].occupied != 0U ? 1U : 0U;
    }
    *discoveries_out = discoveries;
    *reverse_out = reverse;
    *routes_out = routes;
    *static_out = statics;
    owner_unlock(owner);
    return UCN_OK;
}
