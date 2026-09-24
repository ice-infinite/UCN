#include "internal/ucn_flow.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_FLOW_MAGIC UINT32_C(0x5543464C)
#define UCN_I_FLOW_CANDIDATE_KIND UINT8_C(0x43)
#define UCN_I_FLOW_ACTIVATION_KIND UINT8_C(0x44)
#define UCN_I_FLOW_ACTIVE_KIND UINT8_C(0x45)

static void clear_candidate(ucn_i_flow_candidate_record_t *record);

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
    return !bytes_nonzero(bytes, length);
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

static ucn_result_t owner_lock(ucn_i_flow_owner_t *owner)
{
    if (owner == NULL || owner->magic != UCN_I_FLOW_MAGIC ||
        owner->schema != UCN_I_FLOW_SCHEMA ||
        !lock_valid(&owner->state_lock)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

static void owner_unlock(ucn_i_flow_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
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

static bool binding_equal(const ucn_i_route_binding_t *left,
                          const ucn_i_route_binding_t *right)
{
    return left->address == right->address &&
           left->generation == right->generation &&
           memcmp(left->principal, right->principal, 16U) == 0;
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

static bool domain_equal(const ucn_i_route_domain_t *left,
                         const ucn_i_route_domain_t *right)
{
    return left->realm == right->realm &&
           left->origin_session_generation ==
               right->origin_session_generation &&
           binding_equal(&left->origin, &right->origin) &&
           binding_equal(&left->destination, &right->destination);
}

static bool activation_key_equal(const ucn_i_flow_activation_key_t *left,
                                 const ucn_i_flow_activation_key_t *right)
{
    return domain_equal(&left->route_domain, &right->route_domain) &&
           memcmp(left->proposal_digest, right->proposal_digest, 16U) == 0 &&
           left->transaction_id == right->transaction_id &&
           left->candidate_id == right->candidate_id &&
           left->route_generation == right->route_generation &&
           left->reverse_label == right->reverse_label &&
           left->forward_label == right->forward_label &&
           left->path_profile_id == right->path_profile_id &&
           left->reserved_zero == 0U && right->reserved_zero == 0U;
}

static bool label_setup_valid(const ucn_i_flow_label_setup_t *value)
{
    return value != NULL && value->candidate_id != 0U &&
           value->candidate_id < UINT16_MAX &&
           value->route_generation != 0U &&
           value->context_digest != 0U &&
           value->reverse_label != 0U &&
           value->reverse_label != UINT16_MAX &&
           value->forward_label != 0U &&
           value->forward_label != UINT16_MAX &&
           value->path_profile_id != 0U &&
           value->path_profile_id != UINT16_MAX &&
           value->reserved_zero == 0U;
}

ucn_result_t ucn_i_flow_label_setup_encode(
    const ucn_i_flow_label_setup_t *value,
    uint8_t output[UCN_I_FLOW_LABEL_SETUP_BYTES])
{
    uint8_t bytes[UCN_I_FLOW_LABEL_SETUP_BYTES];

    if (!label_setup_valid(value) || output == NULL ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, sizeof(bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    put16(bytes, 0U, (uint16_t)value->candidate_id);
    put32(bytes, 2U, value->route_generation);
    put16(bytes, 6U, value->reverse_label);
    put16(bytes, 8U, value->forward_label);
    put16(bytes, 10U, value->path_profile_id);
    put32(bytes, 12U, value->context_digest);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_flow_label_setup_decode(
    const uint8_t input[UCN_I_FLOW_LABEL_SETUP_BYTES],
    ucn_i_flow_label_setup_t *value_out)
{
    ucn_i_flow_label_setup_t value;

    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_FLOW_LABEL_SETUP_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    value.candidate_id = get16(input, 0U);
    value.route_generation = get32(input, 2U);
    value.reverse_label = get16(input, 6U);
    value.forward_label = get16(input, 8U);
    value.path_profile_id = get16(input, 10U);
    value.context_digest = get32(input, 12U);
    if (!label_setup_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

static bool common_header_valid(const ucn_i_flow_common_header_t *header)
{
    if (header == NULL || header->contract < 2U || header->contract > 4U ||
        header->traffic_class > 3U || header->delivery > 2U ||
        header->interaction > 3U || header->payload_kind > 3U ||
        header->origin_security > 2U || header->hop_limit == 0U ||
        header->hop_limit > 63U || header->reserved_zero != 0U) {
        return false;
    }
    if (header->payload_kind == 0U && header->interaction != 0U) {
        return false;
    }
    if (header->contract == 3U &&
        (header->delivery != 0U || header->interaction != 0U ||
         (header->payload_kind != 0U && header->payload_kind != 3U) ||
         header->origin_security != 0U)) {
        return false;
    }
    return true;
}

static size_t prefix_size(uint8_t contract)
{
    if (contract == 2U) {
        return UCN_I_FLOW_C2_PREFIX_BYTES;
    }
    if (contract == 3U) {
        return UCN_I_FLOW_C3_PREFIX_BYTES;
    }
    if (contract == 4U) {
        return UCN_I_FLOW_C4_PREFIX_BYTES;
    }
    return 0U;
}

static bool prefix_valid(const ucn_i_flow_prefix_t *value)
{
    if (value == NULL || !common_header_valid(&value->header) ||
        value->context_id == 0U || value->context_id == UINT16_MAX) {
        return false;
    }
    if (value->header.contract == 2U) {
        return value->label == 0U && value->sequence != 0U;
    }
    if (value->header.contract == 3U) {
        return value->label != 0U && value->label != UINT16_MAX &&
               value->sequence == 0U;
    }
    return value->label != 0U && value->label != UINT16_MAX &&
           value->sequence != 0U;
}

static void encode_common(const ucn_i_flow_common_header_t *header,
                          uint8_t bytes[3])
{
    bytes[0] = (uint8_t)(UINT8_C(0x60) | header->contract);
    bytes[1] = (uint8_t)((header->traffic_class << 6U) |
                         (header->delivery << 4U) |
                         (header->interaction << 2U) |
                         header->payload_kind);
    bytes[2] = (uint8_t)((header->origin_security << 6U) |
                         header->hop_limit);
}

static bool decode_common(const uint8_t input[3],
                          ucn_i_flow_common_header_t *header)
{
    memset(header, 0, sizeof(*header));
    if ((input[0] >> 4U) != 6U) {
        return false;
    }
    header->contract = (uint8_t)(input[0] & 0x0FU);
    header->traffic_class = (uint8_t)(input[1] >> 6U);
    header->delivery = (uint8_t)((input[1] >> 4U) & 0x03U);
    header->interaction = (uint8_t)((input[1] >> 2U) & 0x03U);
    header->payload_kind = (uint8_t)(input[1] & 0x03U);
    header->origin_security = (uint8_t)(input[2] >> 6U);
    header->hop_limit = (uint8_t)(input[2] & 0x3FU);
    return common_header_valid(header);
}

ucn_result_t ucn_i_flow_prefix_encode(const ucn_i_flow_prefix_t *value,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      size_t *output_bytes)
{
    uint8_t bytes[UCN_I_FLOW_C4_PREFIX_BYTES];
    size_t length;

    if (!prefix_valid(value) || output == NULL || output_bytes == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    length = prefix_size(value->header.contract);
    if (output_capacity != length) {
        return UCN_ERR_NO_SPACE;
    }
    if (ucn_i_ranges_overlap(value, sizeof(*value), output, length) ||
        ucn_i_ranges_overlap(value, sizeof(*value), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output, length, output_bytes,
                             sizeof(*output_bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(bytes, 0, sizeof(bytes));
    encode_common(&value->header, bytes);
    if (value->header.contract == 2U) {
        put16(bytes, 3U, value->context_id);
        put32(bytes, 5U, value->sequence);
    } else if (value->header.contract == 3U) {
        put16(bytes, 3U, value->label);
        put16(bytes, 5U, value->context_id);
    } else {
        put16(bytes, 3U, value->label);
        put16(bytes, 5U, value->context_id);
        put32(bytes, 7U, value->sequence);
    }
    memcpy(output, bytes, length);
    *output_bytes = length;
    return UCN_OK;
}

ucn_result_t ucn_i_flow_prefix_decode(const uint8_t *input,
                                      size_t input_bytes,
                                      ucn_i_flow_prefix_t *value_out)
{
    ucn_i_flow_prefix_t value;

    if (input == NULL || value_out == NULL || input_bytes < 3U ||
        ucn_i_ranges_overlap(input, input_bytes, value_out,
                             sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    if (!decode_common(input, &value.header) ||
        input_bytes != prefix_size(value.header.contract)) {
        return UCN_ERR_MALFORMED;
    }
    if (value.header.contract == 2U) {
        value.context_id = get16(input, 3U);
        value.sequence = get32(input, 5U);
    } else if (value.header.contract == 3U) {
        value.label = get16(input, 3U);
        value.context_id = get16(input, 5U);
    } else {
        value.label = get16(input, 3U);
        value.context_id = get16(input, 5U);
        value.sequence = get32(input, 7U);
    }
    if (!prefix_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

static bool config_valid(const ucn_i_flow_config_t *config)
{
    return config != NULL && config->runtime_instance != 0U &&
           config->realm != 0U && config->realm != UINT32_MAX &&
           config->owner_instance != 0U && config->local.address != 0U &&
           config->local.generation != 0U &&
           bytes_nonzero(config->local.principal, 16U) &&
           config->policy_generation != 0U &&
           config->probe_lifetime_us != 0U &&
           config->stage_lifetime_us != 0U &&
           config->commit_lifetime_us != 0U &&
           config->flow_lifetime_us != 0U &&
           config->receipt_lifetime_us >= config->commit_lifetime_us &&
           config->first_transaction_id != 0U &&
           config->first_candidate_id != 0U &&
           config->first_candidate_id < UINT16_MAX &&
           config->first_route_generation != 0U &&
           config->first_flow_generation != 0U &&
           config->first_context_id != 0U &&
           config->first_context_id != UINT16_MAX &&
           config->first_label != 0U &&
           config->first_label < UINT16_MAX - 1U &&
           config->reserved_zero == 0U;
}

ucn_result_t ucn_i_flow_owner_init(ucn_i_flow_owner_t *owner,
                                   const ucn_i_flow_config_t *config,
                                   const ucn_i_lock_ops_t *state_lock)
{
    if (owner == NULL || !object_zero(owner, sizeof(*owner)) ||
        !config_valid(config) || !lock_valid(state_lock) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), state_lock,
                             sizeof(*state_lock))) {
        return UCN_ERR_ARGUMENT;
    }
    owner->magic = UCN_I_FLOW_MAGIC;
    owner->runtime_instance = config->runtime_instance;
    owner->realm = config->realm;
    owner->policy_generation = config->policy_generation;
    owner->probe_lifetime_us = config->probe_lifetime_us;
    owner->stage_lifetime_us = config->stage_lifetime_us;
    owner->commit_lifetime_us = config->commit_lifetime_us;
    owner->flow_lifetime_us = config->flow_lifetime_us;
    owner->receipt_lifetime_us = config->receipt_lifetime_us;
    owner->next_transaction_id = config->first_transaction_id;
    owner->local = config->local;
    owner->state_lock = *state_lock;
    owner->next_candidate_id = config->first_candidate_id;
    owner->next_route_generation = config->first_route_generation;
    owner->next_flow_generation = config->first_flow_generation;
    owner->next_context_id = config->first_context_id;
    owner->next_label = config->first_label;
    owner->schema = UCN_I_FLOW_SCHEMA;
    owner->owner_instance = config->owner_instance;
    return UCN_OK;
}

ucn_result_t ucn_i_flow_owner_destroy(ucn_i_flow_owner_t *owner)
{
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    owner_unlock(owner);
    memset(owner, 0, sizeof(*owner));
    return UCN_OK;
}

static ucn_handle_t make_handle(const ucn_i_flow_owner_t *owner,
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

static bool handle_matches(const ucn_handle_t *handle,
                           const ucn_i_flow_owner_t *owner,
                           uint16_t limit,
                           uint8_t kind)
{
    return handle != NULL && handle->runtime_instance == owner->runtime_instance &&
           handle->owner_instance == owner->owner_instance &&
           handle->slot != 0U && handle->slot <= limit &&
           handle->generation != 0U && handle->object_kind == kind &&
           handle->reserved_zero == 0U;
}

static bool requirements_valid(const ucn_i_flow_owner_t *owner,
                               const ucn_i_route_soft_view_t *route,
                               const ucn_i_flow_requirements_t *requirements,
                               uint64_t now_us)
{
    bool contract_valid;

    if (requirements == NULL || requirements->contract < 2U ||
        requirements->contract > 4U || requirements->traffic_ceiling > 3U ||
        requirements->delivery > 2U || requirements->interaction > 3U ||
        requirements->payload_kind > 3U ||
        requirements->origin_security > 2U || requirements->hop_profile > 2U ||
        requirements->reserved_zero != 0U ||
        requirements->service_id == 0U ||
        requirements->service_id == UINT16_MAX ||
        requirements->minimum_payload_bytes == 0U ||
        requirements->path_profile_id == 0U ||
        requirements->path_profile_id == UINT16_MAX ||
        requirements->policy_generation != owner->policy_generation ||
        ucn_i_deadline_expired_us(now_us, requirements->expires_at_us) ||
        (requirements->payload_kind == 0U &&
         (requirements->protocol_opcode != 0U ||
          requirements->interaction != 0U)) ||
        (requirements->payload_kind != 0U &&
         requirements->protocol_opcode == 0U)) {
        return false;
    }
    contract_valid =
        (requirements->contract == 2U && route->hop_count == 1U) ||
        (requirements->contract == 3U && route->hop_count > 1U &&
         requirements->delivery == 0U && requirements->interaction == 0U &&
         (requirements->payload_kind == 0U ||
          requirements->payload_kind == 3U) &&
         requirements->origin_security == 0U) ||
        (requirements->contract == 4U && route->hop_count > 1U);
    return contract_valid;
}

static uint16_t payload_budget(const ucn_i_flow_requirements_t *requirements,
                               uint16_t path_mtu)
{
    uint16_t overhead = requirements->contract == 2U ?
                            UCN_I_FLOW_C2_PREFIX_BYTES :
                        requirements->contract == 3U ?
                            UCN_I_FLOW_C3_PREFIX_BYTES :
                            UCN_I_FLOW_C4_PREFIX_BYTES;

    if (requirements->origin_security != 0U) {
        overhead = (uint16_t)(overhead + 16U);
    }
    if (requirements->hop_profile != 0U) {
        overhead = (uint16_t)(overhead + 16U);
    }
    return path_mtu > overhead ? (uint16_t)(path_mtu - overhead) : 0U;
}

static bool capability_valid(const ucn_i_flow_capability_facts_t *capability,
                             uint64_t now_us)
{
    return capability != NULL && capability->runtime_instance != 0U &&
           capability->session_generation != 0U &&
           capability->capability_generation != 0U &&
           capability->security_owner_instance != 0U &&
           capability->reserved_zero == 0U &&
           bytes_nonzero(capability->digest, 16U) &&
           !ucn_i_deadline_expired_us(now_us, capability->expires_at_us);
}

static size_t append16(uint8_t *bytes, size_t offset, uint16_t value)
{
    put16(bytes, offset, value);
    return offset + 2U;
}

static size_t append32(uint8_t *bytes, size_t offset, uint32_t value)
{
    put32(bytes, offset, value);
    return offset + 4U;
}

static size_t append64(uint8_t *bytes, size_t offset, uint64_t value)
{
    put64(bytes, offset, value);
    return offset + 8U;
}

static size_t append_binding(uint8_t *bytes,
                             size_t offset,
                             const ucn_i_route_binding_t *binding)
{
    offset = append32(bytes, offset, binding->address);
    offset = append32(bytes, offset, binding->generation);
    memcpy(&bytes[offset], binding->principal, 16U);
    return offset + 16U;
}

static ucn_result_t proposal_digest(ucn_i_flow_owner_t *owner,
                                    const ucn_i_flow_proposal_t *proposal,
                                    uint8_t digest_out[16])
{
    uint8_t bytes[256];
    size_t offset = 0U;
    const ucn_i_route_soft_view_t *route = &proposal->source_route;
    const ucn_i_flow_requirements_t *requirements = &proposal->requirements;

    memset(bytes, 0, sizeof(bytes));
    offset = append32(bytes, offset, proposal->key.route_domain.realm);
    offset = append_binding(bytes, offset, &proposal->key.route_domain.origin);
    offset = append32(bytes, offset,
                      proposal->key.route_domain.origin_session_generation);
    offset = append_binding(bytes, offset,
                            &proposal->key.route_domain.destination);
    offset = append64(bytes, offset, proposal->key.transaction_id);
    offset = append32(bytes, offset, proposal->key.candidate_id);
    offset = append32(bytes, offset, proposal->key.route_generation);
    offset = append16(bytes, offset, proposal->key.reverse_label);
    offset = append16(bytes, offset, proposal->key.forward_label);
    offset = append16(bytes, offset, proposal->key.path_profile_id);
    offset = append32(bytes, offset, route->runtime_instance);
    offset = append16(bytes, offset, route->owner_instance);
    offset = append32(bytes, offset, route->route_generation);
    offset = append64(bytes, offset, route->route_causal_id);
    offset = append64(bytes, offset, route->expires_at_us);
    offset = append16(bytes, offset, route->next_hop.link_id);
    offset = append32(bytes, offset, route->next_hop.link_generation);
    offset = append_binding(bytes, offset, &route->next_hop.peer);
    offset = append32(bytes, offset, route->cost);
    offset = append16(bytes, offset, route->next_hop.frame_mtu);
    offset = append16(bytes, offset, route->path_frame_mtu);
    offset = append16(bytes, offset, route->capability_bits);
    bytes[offset++] = route->hop_count;
    offset = append32(bytes, offset, proposal->capability.runtime_instance);
    offset = append16(bytes, offset,
                      proposal->capability.security_owner_instance);
    offset = append32(bytes, offset,
                      proposal->capability.session_generation);
    offset = append32(bytes, offset,
                      proposal->capability.capability_generation);
    offset = append32(bytes, offset, proposal->capability.feature_bits);
    offset = append64(bytes, offset, proposal->capability.expires_at_us);
    memcpy(&bytes[offset], proposal->capability.digest, 16U);
    offset += 16U;
    offset = append32(bytes, offset, requirements->required_feature_bits);
    offset = append32(bytes, offset, requirements->policy_generation);
    offset = append16(bytes, offset, requirements->service_id);
    offset = append16(bytes, offset, requirements->protocol_opcode);
    offset = append16(bytes, offset, requirements->minimum_payload_bytes);
    offset = append16(bytes, offset, requirements->path_profile_id);
    offset = append64(bytes, offset, requirements->expires_at_us);
    bytes[offset++] = requirements->contract;
    bytes[offset++] = requirements->traffic_ceiling;
    bytes[offset++] = requirements->delivery;
    bytes[offset++] = requirements->interaction;
    bytes[offset++] = requirements->payload_kind;
    bytes[offset++] = requirements->origin_security;
    bytes[offset++] = requirements->hop_profile;
    offset = append16(bytes, offset, proposal->payload_budget);
    offset = append32(bytes, offset, proposal->context_id);
    offset = append64(bytes, offset, proposal->probe_deadline_us);
    offset = append64(bytes, offset, proposal->flow_expires_at_us);
    return ucn_i_sha256_128(bytes, offset, digest_out,
                            &owner->digest_workspace);
}

ucn_result_t ucn_i_flow_import_candidate(
    ucn_i_flow_owner_t *owner,
    const ucn_i_route_soft_view_t *route,
    const ucn_i_flow_capability_facts_t *capability,
    const ucn_i_flow_requirements_t *requirements,
    uint64_t now_us,
    ucn_handle_t *candidate_out)
{
    size_t index;
    uint16_t generation;
    uint32_t candidate_id;
    ucn_i_flow_candidate_record_t record;
    ucn_result_t result;

    if (route == NULL || capability == NULL || requirements == NULL ||
        candidate_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), route, sizeof(*route)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), capability,
                             sizeof(*capability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirements,
                             sizeof(*requirements)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), candidate_out,
                             sizeof(*candidate_out)) ||
        ucn_i_ranges_overlap(route, sizeof(*route), candidate_out,
                             sizeof(*candidate_out)) ||
        ucn_i_ranges_overlap(capability, sizeof(*capability), candidate_out,
                             sizeof(*candidate_out)) ||
        ucn_i_ranges_overlap(requirements, sizeof(*requirements),
                             candidate_out, sizeof(*candidate_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (route->runtime_instance != owner->runtime_instance ||
        route->owner_instance == 0U || route->domain.realm != owner->realm ||
        !binding_equal(&route->domain.origin, &owner->local) ||
        route->route_generation == 0U || route->route_causal_id == 0U ||
        route->next_hop.link_generation == 0U ||
        route->path_frame_mtu == 0U || route->hop_count == 0U ||
        ucn_i_deadline_expired_us(now_us, route->expires_at_us) ||
        !capability_valid(capability, now_us) ||
        capability->runtime_instance != owner->runtime_instance ||
        !requirements_valid(owner, route, requirements, now_us) ||
        (requirements->required_feature_bits &
         ~capability->feature_bits) != 0U ||
        (requirements->required_feature_bits &
         (uint32_t)~route->capability_bits) != 0U ||
        payload_budget(requirements, route->path_frame_mtu) <
            requirements->minimum_payload_bytes) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    for (index = 0U; index < UCN_I_FLOW_CANDIDATE_COUNT; ++index) {
        if (owner->candidates[index].occupied == 0U) {
            break;
        }
    }
    if (index == UCN_I_FLOW_CANDIDATE_COUNT) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    candidate_id = owner->next_candidate_id;
    if (candidate_id == 0U || candidate_id >= UINT16_MAX ||
        owner->candidates[index].generation == UINT16_MAX) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    generation = (uint16_t)(owner->candidates[index].generation + 1U);
    memset(&record, 0, sizeof(record));
    record.proposal.key.route_domain = route->domain;
    record.proposal.key.candidate_id = candidate_id;
    record.proposal.source_route = *route;
    record.proposal.capability = *capability;
    record.proposal.requirements = *requirements;
    record.generation = generation;
    record.phase = UCN_I_FLOW_CANDIDATE;
    record.occupied = 1U;
    owner->candidates[index] = record;
    owner->next_candidate_id = candidate_id == UINT16_MAX - 1U ?
                                   0U : candidate_id + 1U;
    *candidate_out = make_handle(owner, index, generation,
                                 UCN_I_FLOW_CANDIDATE_KIND);
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

static uint64_t minimum_deadline(uint64_t left, uint64_t right)
{
    return left < right ? left : right;
}

ucn_result_t ucn_i_flow_begin_probe(ucn_i_flow_owner_t *owner,
                                    ucn_handle_t candidate,
                                    uint64_t now_us,
                                    ucn_i_flow_proposal_t *proposal_out)
{
    size_t index;
    ucn_i_flow_candidate_record_t record;
    ucn_i_flow_proposal_t proposal;
    uint64_t local_deadline;
    uint64_t flow_deadline;
    uint32_t forward_label;
    ucn_result_t result;

    if (proposal_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), proposal_out,
                             sizeof(*proposal_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(&candidate, owner, UCN_I_FLOW_CANDIDATE_COUNT,
                        UCN_I_FLOW_CANDIDATE_KIND)) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    index = (size_t)candidate.slot - 1U;
    record = owner->candidates[index];
    if (record.occupied == 0U || record.generation != candidate.generation ||
        record.phase != UCN_I_FLOW_CANDIDATE) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (owner->next_transaction_id == 0U ||
        owner->next_route_generation == 0U ||
        owner->next_context_id == 0U ||
        owner->next_context_id == UINT16_MAX || owner->next_label == 0U ||
        owner->next_label >= UINT16_MAX - 1U) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    result = ucn_i_deadline_from_duration_us(now_us,
                                             owner->probe_lifetime_us,
                                             &local_deadline);
    if (result != UCN_OK) {
        goto done;
    }
    local_deadline = minimum_deadline(
        local_deadline, record.proposal.source_route.expires_at_us);
    local_deadline = minimum_deadline(
        local_deadline, record.proposal.capability.expires_at_us);
    local_deadline = minimum_deadline(
        local_deadline, record.proposal.requirements.expires_at_us);
    result = ucn_i_deadline_from_duration_us(now_us,
                                             owner->flow_lifetime_us,
                                             &flow_deadline);
    if (result != UCN_OK) {
        goto done;
    }
    flow_deadline = minimum_deadline(
        flow_deadline, record.proposal.source_route.expires_at_us);
    flow_deadline = minimum_deadline(
        flow_deadline, record.proposal.capability.expires_at_us);
    flow_deadline = minimum_deadline(
        flow_deadline, record.proposal.requirements.expires_at_us);
    if (ucn_i_deadline_expired_us(now_us, local_deadline) ||
        ucn_i_deadline_expired_us(now_us, flow_deadline)) {
        result = UCN_ERR_TIMEOUT;
        goto done;
    }
    proposal = record.proposal;
    proposal.key.transaction_id = owner->next_transaction_id;
    proposal.key.route_generation = owner->next_route_generation;
    proposal.key.reverse_label = owner->next_label;
    forward_label = (uint32_t)owner->next_label + 1U;
    proposal.key.forward_label = (uint16_t)forward_label;
    proposal.key.path_profile_id = proposal.requirements.path_profile_id;
    proposal.context_id = owner->next_context_id;
    proposal.payload_budget = payload_budget(
        &proposal.requirements, proposal.source_route.path_frame_mtu);
    proposal.probe_deadline_us = local_deadline;
    proposal.flow_expires_at_us = flow_deadline;
    result = proposal_digest(owner, &proposal,
                             proposal.key.proposal_digest);
    if (result != UCN_OK ||
        !bytes_nonzero(proposal.key.proposal_digest, 16U)) {
        result = result == UCN_OK ? UCN_ERR_SECURITY : result;
        goto done;
    }
    owner->next_transaction_id =
        owner->next_transaction_id == UINT64_MAX ?
            0U : owner->next_transaction_id + 1U;
    owner->next_route_generation =
        owner->next_route_generation == UINT32_MAX ?
            0U : owner->next_route_generation + 1U;
    owner->next_context_id =
        owner->next_context_id == UINT16_MAX - 1U ?
            0U : (uint16_t)(owner->next_context_id + 1U);
    owner->next_label = forward_label >= UINT16_MAX - 1U ?
                            0U : (uint16_t)(forward_label + 1U);
    record.proposal = proposal;
    record.phase = UCN_I_FLOW_PROBING;
    owner->candidates[index] = record;
    *proposal_out = proposal;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_accept_probe_ack(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t candidate,
    const ucn_i_flow_probe_ack_t *ack,
    uint64_t now_us)
{
    size_t index;
    ucn_i_flow_candidate_record_t *record;
    ucn_result_t result;

    if (ack == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), ack, sizeof(*ack))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(&candidate, owner, UCN_I_FLOW_CANDIDATE_COUNT,
                        UCN_I_FLOW_CANDIDATE_KIND)) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    index = (size_t)candidate.slot - 1U;
    record = &owner->candidates[index];
    if (record->occupied == 0U || record->generation != candidate.generation ||
        record->phase != UCN_I_FLOW_PROBING || ack->measured_rtt_us == 0U ||
        ack->candidate_id != record->proposal.key.candidate_id ||
        ack->route_generation != record->proposal.key.route_generation ||
        ack->path_frame_mtu != record->proposal.source_route.path_frame_mtu ||
        ack->capability_bits != record->proposal.source_route.capability_bits ||
        memcmp(ack->proposal_digest,
               record->proposal.key.proposal_digest, 16U) != 0) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (ucn_i_deadline_expired_us(now_us,
                                  record->proposal.probe_deadline_us)) {
        result = UCN_ERR_TIMEOUT;
        goto done;
    }
    record->phase = UCN_I_FLOW_READY_TO_STAGE;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

static bool facts_match(const ucn_i_flow_owner_t *owner,
                        const ucn_i_flow_proposal_t *proposal,
                        const ucn_i_flow_current_facts_t *facts)
{
    return facts != NULL &&
           !ucn_i_deadline_expired_us(facts->now_us,
                                      proposal->flow_expires_at_us) &&
           !ucn_i_deadline_expired_us(facts->now_us,
                                      facts->route_deadline_us) &&
           !ucn_i_deadline_expired_us(facts->now_us,
                                      facts->capability_deadline_us) &&
           binding_equal(&facts->local, &owner->local) &&
           binding_equal(&facts->destination,
                         &proposal->key.route_domain.destination) &&
           facts->origin_session_generation ==
               proposal->key.route_domain.origin_session_generation &&
           facts->route_runtime_instance ==
               proposal->source_route.runtime_instance &&
           facts->route_owner_instance ==
               proposal->source_route.owner_instance &&
           facts->route_generation ==
               proposal->source_route.route_generation &&
           facts->route_causal_id ==
               proposal->source_route.route_causal_id &&
           facts->route_deadline_us ==
               proposal->source_route.expires_at_us &&
           facts->capability_session_generation ==
               proposal->capability.session_generation &&
           link_equal(&facts->next_hop, &proposal->source_route.next_hop) &&
           facts->capability_generation ==
               proposal->capability.capability_generation &&
           facts->capability_security_owner_instance ==
               proposal->capability.security_owner_instance &&
           bytes_zero((const uint8_t *)facts->reserved_zero,
                      sizeof(facts->reserved_zero)) &&
           facts->capability_deadline_us ==
               proposal->capability.expires_at_us &&
           memcmp(facts->capability_digest,
                  proposal->capability.digest, 16U) == 0 &&
           facts->policy_generation == proposal->requirements.policy_generation;
}

static int free_activation(const ucn_i_flow_owner_t *owner)
{
    size_t index;

    for (index = 0U; index < UCN_I_FLOW_ACTIVATION_COUNT; ++index) {
        if (owner->activations[index].occupied == 0U) {
            return (int)index;
        }
    }
    return -1;
}

static bool slot_reserved(const ucn_i_flow_owner_t *owner,
                          uint16_t slot,
                          bool receipt)
{
    size_t index;

    for (index = 0U; index < UCN_I_FLOW_ACTIVATION_COUNT; ++index) {
        if (owner->activations[index].occupied != 0U &&
            (receipt ? owner->activations[index].receipt_slot :
                       owner->activations[index].flow_slot) == slot) {
            return true;
        }
    }
    return false;
}

static int free_flow(const ucn_i_flow_owner_t *owner)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_FLOW_ACTIVE_COUNT; ++index) {
        if (owner->flows[index].occupied == 0U &&
            !slot_reserved(owner, index, false)) {
            return (int)index;
        }
    }
    return -1;
}

static int free_receipt(const ucn_i_flow_owner_t *owner)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_FLOW_RECEIPT_COUNT; ++index) {
        if (owner->receipts[index].occupied == 0U &&
            !slot_reserved(owner, index, true)) {
            return (int)index;
        }
    }
    return -1;
}

ucn_result_t ucn_i_flow_begin_stage(ucn_i_flow_owner_t *owner,
                                    ucn_handle_t candidate,
                                    const ucn_i_flow_current_facts_t *facts,
                                    ucn_handle_t *activation_out,
                                    ucn_i_flow_label_setup_t *setup_out)
{
    size_t candidate_index;
    int activation_index;
    int flow_index;
    int receipt_index;
    uint16_t generation;
    uint64_t stage_deadline;
    uint64_t commit_deadline;
    ucn_i_flow_candidate_record_t *candidate_record;
    ucn_i_flow_activation_record_t activation;
    ucn_i_flow_label_setup_t setup;
    ucn_result_t result;

    if (facts == NULL || activation_out == NULL || setup_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), activation_out,
                             sizeof(*activation_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), setup_out,
                             sizeof(*setup_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), activation_out,
                             sizeof(*activation_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), setup_out,
                             sizeof(*setup_out)) ||
        ucn_i_ranges_overlap(activation_out, sizeof(*activation_out),
                             setup_out, sizeof(*setup_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(&candidate, owner, UCN_I_FLOW_CANDIDATE_COUNT,
                        UCN_I_FLOW_CANDIDATE_KIND)) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    candidate_index = (size_t)candidate.slot - 1U;
    candidate_record = &owner->candidates[candidate_index];
    if (candidate_record->occupied == 0U ||
        candidate_record->generation != candidate.generation ||
        candidate_record->phase != UCN_I_FLOW_READY_TO_STAGE ||
        !facts_match(owner, &candidate_record->proposal, facts) ||
        ucn_i_deadline_expired_us(
            facts->now_us, candidate_record->proposal.probe_deadline_us)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    activation_index = free_activation(owner);
    flow_index = free_flow(owner);
    receipt_index = free_receipt(owner);
    if (activation_index < 0 || flow_index < 0 || receipt_index < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    if (owner->activations[activation_index].generation == UINT16_MAX) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    result = ucn_i_deadline_from_duration_us(facts->now_us,
                                             owner->stage_lifetime_us,
                                             &stage_deadline);
    if (result != UCN_OK) {
        goto done;
    }
    stage_deadline = minimum_deadline(
        stage_deadline, candidate_record->proposal.flow_expires_at_us);
    result = ucn_i_deadline_from_duration_us(stage_deadline,
                                             owner->commit_lifetime_us,
                                             &commit_deadline);
    if (result != UCN_OK) {
        goto done;
    }
    commit_deadline = minimum_deadline(
        commit_deadline, candidate_record->proposal.flow_expires_at_us);
    if (ucn_i_deadline_expired_us(facts->now_us, stage_deadline) ||
        stage_deadline >= commit_deadline) {
        result = UCN_ERR_TIMEOUT;
        goto done;
    }
    generation =
        (uint16_t)(owner->activations[activation_index].generation + 1U);
    memset(&activation, 0, sizeof(activation));
    activation.proposal = candidate_record->proposal;
    activation.stage_deadline_us = stage_deadline;
    activation.commit_deadline_us = commit_deadline;
    activation.generation = generation;
    activation.flow_slot = (uint16_t)flow_index;
    activation.receipt_slot = (uint16_t)receipt_index;
    activation.phase = UCN_I_FLOW_STAGE_PENDING;
    activation.occupied = 1U;
    memset(&setup, 0, sizeof(setup));
    setup.candidate_id = activation.proposal.key.candidate_id;
    setup.route_generation = activation.proposal.key.route_generation;
    setup.reverse_label = activation.proposal.key.reverse_label;
    setup.forward_label = activation.proposal.key.forward_label;
    setup.path_profile_id = activation.proposal.key.path_profile_id;
    setup.context_digest = get32(
        activation.proposal.key.proposal_digest, 0U);
    if (!label_setup_valid(&setup)) {
        result = UCN_ERR_SECURITY;
        goto done;
    }
    owner->activations[activation_index] = activation;
    candidate_record->phase = UCN_I_FLOW_STAGE_PENDING;
    *activation_out = make_handle(owner, (size_t)activation_index,
                                  generation, UCN_I_FLOW_ACTIVATION_KIND);
    *setup_out = setup;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

static ucn_i_flow_activation_record_t *activation_from_handle(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_flow_activation_record_t *record;

    if (!handle_matches(&handle, owner, UCN_I_FLOW_ACTIVATION_COUNT,
                        UCN_I_FLOW_ACTIVATION_KIND)) {
        return NULL;
    }
    record = &owner->activations[(size_t)handle.slot - 1U];
    if (record->occupied == 0U || record->generation != handle.generation) {
        return NULL;
    }
    return record;
}

ucn_result_t ucn_i_flow_stage_submit(ucn_i_flow_owner_t *owner,
                                     ucn_handle_t activation,
                                     ucn_i_flow_submit_result_t submit_result,
                                     uint64_t now_us)
{
    ucn_i_flow_activation_record_t *record;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = activation_from_handle(owner, activation);
    if (record == NULL || record->phase != UCN_I_FLOW_STAGE_PENDING ||
        submit_result < UCN_I_FLOW_NOT_SUBMITTED ||
        submit_result > UCN_I_FLOW_SUBMIT_IN_DOUBT) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (ucn_i_deadline_expired_us(now_us, record->stage_deadline_us)) {
        record->phase = submit_result == UCN_I_FLOW_NOT_SUBMITTED ?
                            UCN_I_FLOW_ABORTING : UCN_I_FLOW_IN_DOUBT;
    } else if (submit_result == UCN_I_FLOW_SUBMITTED) {
        record->stage_submitted = 1U;
    } else if (submit_result == UCN_I_FLOW_SUBMIT_IN_DOUBT) {
        record->phase = UCN_I_FLOW_IN_DOUBT;
    }
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_accept_stage_ack(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t activation,
    const ucn_i_flow_activation_key_t *key,
    const ucn_i_flow_current_facts_t *facts)
{
    ucn_i_flow_activation_record_t *record;
    ucn_result_t result;

    if (key == NULL || facts == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), key, sizeof(*key)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = activation_from_handle(owner, activation);
    if (record == NULL || record->phase != UCN_I_FLOW_STAGE_PENDING ||
        record->stage_submitted == 0U ||
        !activation_key_equal(key, &record->proposal.key) ||
        !facts_match(owner, &record->proposal, facts)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (ucn_i_deadline_expired_us(facts->now_us,
                                  record->stage_deadline_us)) {
        result = UCN_ERR_TIMEOUT;
        goto done;
    }
    record->phase = UCN_I_FLOW_STAGED;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_begin_commit(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t activation,
    const ucn_i_flow_current_facts_t *facts,
    ucn_i_flow_activation_key_t *key_out)
{
    ucn_i_flow_activation_record_t *record;
    ucn_result_t result;

    if (facts == NULL || key_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), key_out,
                             sizeof(*key_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), key_out,
                             sizeof(*key_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = activation_from_handle(owner, activation);
    if (record == NULL || record->phase != UCN_I_FLOW_STAGED ||
        !facts_match(owner, &record->proposal, facts)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (ucn_i_deadline_expired_us(facts->now_us,
                                  record->commit_deadline_us)) {
        result = UCN_ERR_TIMEOUT;
        goto done;
    }
    *key_out = record->proposal.key;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_commit_submit(ucn_i_flow_owner_t *owner,
                                      ucn_handle_t activation,
                                      ucn_i_flow_submit_result_t submit_result,
                                      uint64_t now_us)
{
    ucn_i_flow_activation_record_t *record;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = activation_from_handle(owner, activation);
    if (record == NULL || record->phase != UCN_I_FLOW_STAGED ||
        submit_result < UCN_I_FLOW_NOT_SUBMITTED ||
        submit_result > UCN_I_FLOW_SUBMIT_IN_DOUBT) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (ucn_i_deadline_expired_us(now_us, record->commit_deadline_us)) {
        record->phase = submit_result == UCN_I_FLOW_NOT_SUBMITTED ?
                            UCN_I_FLOW_ABORTING : UCN_I_FLOW_IN_DOUBT;
    } else if (submit_result == UCN_I_FLOW_SUBMITTED) {
        record->commit_submitted = 1U;
        record->phase = UCN_I_FLOW_COMMITTING;
    } else if (submit_result == UCN_I_FLOW_SUBMIT_IN_DOUBT) {
        record->phase = UCN_I_FLOW_IN_DOUBT;
    }
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_accept_commit_ack(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t activation,
    const ucn_i_flow_activation_key_t *key,
    const ucn_i_flow_current_facts_t *facts,
    ucn_handle_t *flow_out)
{
    ucn_i_flow_activation_record_t *record;
    ucn_i_flow_active_record_t flow;
    ucn_i_flow_receipt_record_t receipt;
    uint16_t slot_generation;
    size_t index;
    ucn_result_t result;

    if (key == NULL || facts == NULL || flow_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), key, sizeof(*key)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), flow_out,
                             sizeof(*flow_out)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), flow_out,
                             sizeof(*flow_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), flow_out,
                             sizeof(*flow_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = activation_from_handle(owner, activation);
    if (record == NULL || record->phase != UCN_I_FLOW_COMMITTING ||
        record->commit_submitted == 0U ||
        !activation_key_equal(key, &record->proposal.key) ||
        !facts_match(owner, &record->proposal, facts)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (ucn_i_deadline_expired_us(facts->now_us,
                                  record->commit_deadline_us)) {
        record->phase = UCN_I_FLOW_IN_DOUBT;
        result = UCN_ERR_IN_DOUBT;
        goto done;
    }
    if (record->flow_slot >= UCN_I_FLOW_ACTIVE_COUNT ||
        record->receipt_slot >= UCN_I_FLOW_RECEIPT_COUNT ||
        owner->flows[record->flow_slot].occupied != 0U ||
        owner->receipts[record->receipt_slot].occupied != 0U ||
        owner->flows[record->flow_slot].generation == UINT16_MAX ||
        owner->next_flow_generation == 0U) {
        result = UCN_ERR_STATE;
        goto done;
    }
    slot_generation =
        (uint16_t)(owner->flows[record->flow_slot].generation + 1U);
    memset(&flow, 0, sizeof(flow));
    flow.proposal = record->proposal;
    memcpy(flow.fingerprint, record->proposal.key.proposal_digest, 16U);
    flow.flow_generation = owner->next_flow_generation;
    flow.next_sequence = 1U;
    flow.generation = slot_generation;
    flow.phase = UCN_I_FLOW_ACTIVE;
    flow.occupied = 1U;
    memset(&receipt, 0, sizeof(receipt));
    receipt.key = record->proposal.key;
    result = ucn_i_deadline_from_duration_us(facts->now_us,
                                             owner->receipt_lifetime_us,
                                             &receipt.expires_at_us);
    if (result != UCN_OK) {
        goto done;
    }
    receipt.phase = UCN_I_FLOW_ACTIVE;
    receipt.occupied = 1U;
    for (index = 0U; index < UCN_I_FLOW_ACTIVE_COUNT; ++index) {
        if (owner->flows[index].occupied != 0U &&
            owner->flows[index].phase == UCN_I_FLOW_ACTIVE &&
            domain_equal(&owner->flows[index].proposal.key.route_domain,
                         &record->proposal.key.route_domain)) {
            owner->flows[index].phase = UCN_I_FLOW_DRAINING;
        }
    }
    owner->flows[record->flow_slot] = flow;
    owner->receipts[record->receipt_slot] = receipt;
    owner->next_flow_generation =
        owner->next_flow_generation == UINT32_MAX ?
            0U : owner->next_flow_generation + 1U;
    record->phase = UCN_I_FLOW_ACTIVE;
    for (index = 0U; index < UCN_I_FLOW_CANDIDATE_COUNT; ++index) {
        if (owner->candidates[index].occupied != 0U &&
            owner->candidates[index].proposal.key.candidate_id ==
                record->proposal.key.candidate_id) {
            clear_candidate(&owner->candidates[index]);
            break;
        }
    }
    *flow_out = make_handle(owner, record->flow_slot, slot_generation,
                            UCN_I_FLOW_ACTIVE_KIND);
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_expire_activation(ucn_i_flow_owner_t *owner,
                                          ucn_handle_t activation,
                                          uint64_t now_us,
                                          ucn_i_flow_phase_t *phase_out)
{
    ucn_i_flow_activation_record_t *record;
    ucn_result_t result;

    if (phase_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), phase_out,
                             sizeof(*phase_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = activation_from_handle(owner, activation);
    if (record == NULL) {
        result = UCN_ERR_NOT_FOUND;
        goto done;
    }
    if ((record->phase == UCN_I_FLOW_STAGE_PENDING ||
         record->phase == UCN_I_FLOW_STAGED) &&
        ucn_i_deadline_expired_us(now_us, record->stage_deadline_us)) {
        record->phase = UCN_I_FLOW_ABORTING;
    } else if (record->phase == UCN_I_FLOW_COMMITTING &&
               ucn_i_deadline_expired_us(now_us,
                                         record->commit_deadline_us)) {
        record->phase = UCN_I_FLOW_IN_DOUBT;
    }
    *phase_out = record->phase;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

static ucn_i_flow_active_record_t *flow_from_handle(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_flow_active_record_t *record;

    if (!handle_matches(&handle, owner, UCN_I_FLOW_ACTIVE_COUNT,
                        UCN_I_FLOW_ACTIVE_KIND)) {
        return NULL;
    }
    record = &owner->flows[(size_t)handle.slot - 1U];
    if (record->occupied == 0U || record->generation != handle.generation) {
        return NULL;
    }
    return record;
}

static bool request_matches(const ucn_i_flow_requirements_t *requirements,
                            const ucn_i_flow_tx_request_t *request)
{
    return request != NULL && request->contract == requirements->contract &&
           request->traffic_class <= requirements->traffic_ceiling &&
           request->delivery == requirements->delivery &&
           request->interaction == requirements->interaction &&
           request->payload_kind == requirements->payload_kind &&
           request->protocol_opcode == requirements->protocol_opcode &&
           request->origin_security == requirements->origin_security &&
           request->payload_bytes != 0U && request->reserved_zero[0] == 0U &&
           request->reserved_zero[1] == 0U;
}

ucn_result_t ucn_i_flow_use_preflight(
    ucn_i_flow_owner_t *owner,
    ucn_handle_t flow_handle,
    const ucn_i_flow_current_facts_t *facts,
    const ucn_i_flow_tx_request_t *request,
    ucn_i_flow_tx_plan_t *plan_out)
{
    ucn_i_flow_active_record_t *flow;
    ucn_i_flow_tx_plan_t plan;
    uint32_t sequence;
    ucn_result_t result;

    if (facts == NULL || request == NULL || plan_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), plan_out,
                             sizeof(*plan_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    flow = flow_from_handle(owner, flow_handle);
    if (flow == NULL || flow->phase != UCN_I_FLOW_ACTIVE ||
        !facts_match(owner, &flow->proposal, facts) ||
        !request_matches(&flow->proposal.requirements, request) ||
        request->payload_bytes > flow->proposal.payload_budget) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    sequence = 0U;
    if (request->origin_security != 0U || request->contract != 3U) {
        if (flow->next_sequence == 0U || flow->sequence_reserved != 0U) {
            result = UCN_ERR_EXHAUSTED;
            goto done;
        }
        sequence = flow->next_sequence;
    }
    memset(&plan, 0, sizeof(plan));
    plan.next_hop = flow->proposal.source_route.next_hop;
    memcpy(plan.flow_fingerprint, flow->fingerprint, 16U);
    plan.route_generation = flow->proposal.key.route_generation;
    plan.sequence = sequence;
    plan.context_id = (uint16_t)flow->proposal.context_id;
    plan.forward_label = flow->proposal.key.forward_label;
    plan.payload_budget = flow->proposal.payload_budget;
    plan.contract = request->contract;
    plan.hop_profile = flow->proposal.requirements.hop_profile;
    if (sequence != 0U) {
        flow->reserved_sequence = sequence;
        flow->sequence_reserved = 1U;
    }
    *plan_out = plan;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_sequence_commit(ucn_i_flow_owner_t *owner,
                                        ucn_handle_t flow_handle,
                                        uint32_t sequence)
{
    ucn_i_flow_active_record_t *flow;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    flow = flow_from_handle(owner, flow_handle);
    if (flow == NULL || flow->phase != UCN_I_FLOW_ACTIVE ||
        flow->sequence_reserved == 0U || sequence == 0U ||
        flow->reserved_sequence != sequence ||
        flow->next_sequence != sequence) {
        result = UCN_ERR_STATE;
        goto done;
    }
    flow->next_sequence =
        sequence == UINT32_MAX ? 0U : sequence + 1U;
    flow->reserved_sequence = 0U;
    flow->sequence_reserved = 0U;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_sequence_abort(ucn_i_flow_owner_t *owner,
                                       ucn_handle_t flow_handle,
                                       uint32_t sequence)
{
    ucn_i_flow_active_record_t *flow;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    flow = flow_from_handle(owner, flow_handle);
    if (flow == NULL || flow->phase != UCN_I_FLOW_ACTIVE ||
        flow->sequence_reserved == 0U || sequence == 0U ||
        flow->reserved_sequence != sequence) {
        result = UCN_ERR_STATE;
        goto done;
    }
    flow->reserved_sequence = 0U;
    flow->sequence_reserved = 0U;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_fence(ucn_i_flow_owner_t *owner,
                              ucn_handle_t flow_handle)
{
    ucn_i_flow_active_record_t *flow;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    flow = flow_from_handle(owner, flow_handle);
    if (flow == NULL || (flow->phase != UCN_I_FLOW_ACTIVE &&
                         flow->phase != UCN_I_FLOW_DRAINING)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    flow->phase = UCN_I_FLOW_FENCED;
    result = UCN_OK;
done:
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_flow_phase(ucn_i_flow_owner_t *owner,
                              ucn_handle_t handle,
                              ucn_i_flow_phase_t *phase_out)
{
    ucn_result_t result;

    if (phase_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), phase_out,
                             sizeof(*phase_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (handle.object_kind == UCN_I_FLOW_CANDIDATE_KIND &&
        handle_matches(&handle, owner, UCN_I_FLOW_CANDIDATE_COUNT,
                       UCN_I_FLOW_CANDIDATE_KIND) &&
        owner->candidates[(size_t)handle.slot - 1U].occupied != 0U &&
        owner->candidates[(size_t)handle.slot - 1U].generation ==
            handle.generation) {
        *phase_out = owner->candidates[(size_t)handle.slot - 1U].phase;
        result = UCN_OK;
    } else if (handle.object_kind == UCN_I_FLOW_ACTIVATION_KIND &&
               activation_from_handle(owner, handle) != NULL) {
        *phase_out = activation_from_handle(owner, handle)->phase;
        result = UCN_OK;
    } else if (handle.object_kind == UCN_I_FLOW_ACTIVE_KIND &&
               flow_from_handle(owner, handle) != NULL) {
        *phase_out = flow_from_handle(owner, handle)->phase;
        result = UCN_OK;
    } else {
        result = UCN_ERR_NOT_FOUND;
    }
    owner_unlock(owner);
    return result;
}

static void clear_candidate(ucn_i_flow_candidate_record_t *record)
{
    uint16_t generation = record->generation;
    memset(record, 0, sizeof(*record));
    record->generation = generation;
}

static void clear_candidate_by_id(ucn_i_flow_owner_t *owner,
                                  uint32_t candidate_id)
{
    size_t index;

    for (index = 0U; index < UCN_I_FLOW_CANDIDATE_COUNT; ++index) {
        if (owner->candidates[index].occupied != 0U &&
            owner->candidates[index].proposal.key.candidate_id == candidate_id) {
            clear_candidate(&owner->candidates[index]);
            return;
        }
    }
}

static void clear_activation(ucn_i_flow_activation_record_t *record)
{
    uint16_t generation = record->generation;
    memset(record, 0, sizeof(*record));
    record->generation = generation;
}

static void clear_flow(ucn_i_flow_active_record_t *record)
{
    uint16_t generation = record->generation;
    memset(record, 0, sizeof(*record));
    record->generation = generation;
}

ucn_result_t ucn_i_flow_maintain(ucn_i_flow_owner_t *owner,
                                 uint64_t now_us,
                                 uint16_t budget,
                                 uint16_t *inspected_out,
                                 uint16_t *retired_out)
{
    const uint16_t total = (uint16_t)(UCN_I_FLOW_CANDIDATE_COUNT +
                                      UCN_I_FLOW_ACTIVATION_COUNT +
                                      UCN_I_FLOW_ACTIVE_COUNT +
                                      UCN_I_FLOW_RECEIPT_COUNT);
    uint16_t inspected = 0U;
    uint16_t retired = 0U;
    ucn_result_t result;

    if (budget == 0U || inspected_out == NULL || retired_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), retired_out,
                             sizeof(*retired_out)) ||
        ucn_i_ranges_overlap(inspected_out, sizeof(*inspected_out),
                             retired_out, sizeof(*retired_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    while (inspected < budget && inspected < total) {
        uint16_t cursor = owner->maintenance_cursor;
        owner->maintenance_cursor = (uint16_t)((cursor + 1U) % total);
        if (cursor < UCN_I_FLOW_CANDIDATE_COUNT) {
            ucn_i_flow_candidate_record_t *record =
                &owner->candidates[cursor];
            if (record->occupied != 0U &&
                record->phase != UCN_I_FLOW_STAGE_PENDING &&
                record->phase != UCN_I_FLOW_ACTIVE &&
                ucn_i_deadline_expired_us(
                    now_us, record->proposal.flow_expires_at_us)) {
                clear_candidate(record);
                retired++;
            }
        } else if (cursor < UCN_I_FLOW_CANDIDATE_COUNT +
                              UCN_I_FLOW_ACTIVATION_COUNT) {
            uint16_t index =
                (uint16_t)(cursor - UCN_I_FLOW_CANDIDATE_COUNT);
            ucn_i_flow_activation_record_t *record =
                &owner->activations[index];
            if (record->occupied != 0U &&
                (record->phase == UCN_I_FLOW_ABORTING ||
                 record->phase == UCN_I_FLOW_ACTIVE) &&
                ucn_i_deadline_expired_us(
                    now_us, record->commit_deadline_us)) {
                clear_candidate_by_id(
                    owner, record->proposal.key.candidate_id);
                clear_activation(record);
                retired++;
            }
        } else if (cursor < UCN_I_FLOW_CANDIDATE_COUNT +
                              UCN_I_FLOW_ACTIVATION_COUNT +
                              UCN_I_FLOW_ACTIVE_COUNT) {
            uint16_t index = (uint16_t)(
                cursor - UCN_I_FLOW_CANDIDATE_COUNT -
                UCN_I_FLOW_ACTIVATION_COUNT);
            ucn_i_flow_active_record_t *record = &owner->flows[index];
            if (record->occupied != 0U &&
                record->phase == UCN_I_FLOW_ACTIVE &&
                ucn_i_deadline_expired_us(
                    now_us, record->proposal.flow_expires_at_us)) {
                record->phase = UCN_I_FLOW_FENCED;
            } else if (record->occupied != 0U &&
                (record->phase == UCN_I_FLOW_FENCED ||
                 record->phase == UCN_I_FLOW_DRAINING) &&
                ucn_i_deadline_expired_us(
                    now_us, record->proposal.flow_expires_at_us)) {
                clear_flow(record);
                retired++;
            }
        } else {
            uint16_t index = (uint16_t)(
                cursor - UCN_I_FLOW_CANDIDATE_COUNT -
                UCN_I_FLOW_ACTIVATION_COUNT - UCN_I_FLOW_ACTIVE_COUNT);
            if (owner->receipts[index].occupied != 0U &&
                ucn_i_deadline_expired_us(
                    now_us, owner->receipts[index].expires_at_us)) {
                memset(&owner->receipts[index], 0,
                       sizeof(owner->receipts[index]));
                retired++;
            }
        }
        inspected++;
    }
    *inspected_out = inspected;
    *retired_out = retired;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_flow_counts(ucn_i_flow_owner_t *owner,
                               uint16_t *candidates_out,
                               uint16_t *activations_out,
                               uint16_t *flows_out,
                               uint16_t *receipts_out)
{
    size_t index;
    uint16_t candidates = 0U;
    uint16_t activations = 0U;
    uint16_t flows = 0U;
    uint16_t receipts = 0U;
    ucn_result_t result;

    if (candidates_out == NULL || activations_out == NULL ||
        flows_out == NULL || receipts_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), candidates_out,
                             sizeof(*candidates_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), activations_out,
                             sizeof(*activations_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), flows_out,
                             sizeof(*flows_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), receipts_out,
                             sizeof(*receipts_out)) ||
        ucn_i_ranges_overlap(candidates_out, sizeof(*candidates_out),
                             activations_out, sizeof(*activations_out)) ||
        ucn_i_ranges_overlap(candidates_out, sizeof(*candidates_out),
                             flows_out, sizeof(*flows_out)) ||
        ucn_i_ranges_overlap(candidates_out, sizeof(*candidates_out),
                             receipts_out, sizeof(*receipts_out)) ||
        ucn_i_ranges_overlap(activations_out, sizeof(*activations_out),
                             flows_out, sizeof(*flows_out)) ||
        ucn_i_ranges_overlap(activations_out, sizeof(*activations_out),
                             receipts_out, sizeof(*receipts_out)) ||
        ucn_i_ranges_overlap(flows_out, sizeof(*flows_out),
                             receipts_out, sizeof(*receipts_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_FLOW_CANDIDATE_COUNT; ++index) {
        candidates += owner->candidates[index].occupied != 0U ? 1U : 0U;
    }
    for (index = 0U; index < UCN_I_FLOW_ACTIVATION_COUNT; ++index) {
        activations += owner->activations[index].occupied != 0U ? 1U : 0U;
    }
    for (index = 0U; index < UCN_I_FLOW_ACTIVE_COUNT; ++index) {
        flows += owner->flows[index].occupied != 0U ? 1U : 0U;
    }
    for (index = 0U; index < UCN_I_FLOW_RECEIPT_COUNT; ++index) {
        receipts += owner->receipts[index].occupied != 0U ? 1U : 0U;
    }
    *candidates_out = candidates;
    *activations_out = activations;
    *flows_out = flows;
    *receipts_out = receipts;
    owner_unlock(owner);
    return UCN_OK;
}
