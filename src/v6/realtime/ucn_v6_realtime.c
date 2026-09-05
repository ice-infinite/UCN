#include "../internal/ucn_v6_realtime_private.h"

#include <limits.h>
#include <string.h>

#define VERSION_SHIFT 4U
#define MODE_MASK 0x03U
#define QUALITY_UNCERTAINTY_SHIFT 3U
#define QUALITY_HARDWARE 0x04U
#define QUALITY_VALID 0x02U
#define QUALITY_HOLDOVER 0x01U

typedef char realtime_owner_storage_must_fit[
    sizeof(struct ucn_v6_realtime_owner) <=
            UCN_V6_REALTIME_OWNER_STORAGE_BYTES ? 1 : -1];

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
        output[7U - index] = (uint8_t)value;
        value >>= 8U;
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

static void write_i64(uint8_t *output, int64_t value)
{
    write_u64(output, (uint64_t)value);
}

static bool read_i64(const uint8_t *input, int64_t *value)
{
    const uint64_t raw = read_u64(input);
    const uint64_t magnitude = (~raw) + 1U;

    if (value == NULL) {
        return false;
    }
    if (raw <= (uint64_t)INT64_MAX) {
        *value = (int64_t)raw;
    } else if (magnitude == (UINT64_C(1) << 63U)) {
        *value = INT64_MIN;
    } else {
        *value = -(int64_t)magnitude;
    }
    return true;
}

static bool principal_equal(const ucn_v6_principal_t *left,
                            const ucn_v6_principal_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static bool owner_is_valid(const ucn_v6_realtime_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_REALTIME_OWNER_MAGIC &&
           owner->schema == UCN_V6_REALTIME_OWNER_SCHEMA &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->initialized && owner->route_owner != NULL &&
           owner->canary == UCN_V6_REALTIME_OWNER_CANARY;
}

static ucn_v6_result_t callback_scope_enter(
    ucn_v6_callback_gate_t *gate, const void *scope_owner,
    uint64_t *violations_before)
{
    if (gate == NULL || scope_owner == NULL || violations_before == NULL) {
        return UCN_V6_ERR_STATE;
    }
    *violations_before = ucn_v6_callback_gate_violation_count(gate);
    if (*violations_before == UINT64_MAX ||
        ucn_v6_callback_gate_try_enter(gate, scope_owner) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t callback_scope_finish(
    ucn_v6_callback_gate_t *gate, const void *scope_owner,
    uint64_t violations_before, ucn_v6_result_t callback_result)
{
    const uint64_t violations_after =
        ucn_v6_callback_gate_violation_count(gate);
    const ucn_v6_result_t leave_result =
        ucn_v6_callback_gate_leave(gate, scope_owner);

    if (leave_result != UCN_V6_OK || violations_before == UINT64_MAX ||
        violations_after == UINT64_MAX ||
        violations_after != violations_before) {
        return UCN_V6_ERR_STATE;
    }
    return callback_result;
}

static bool owner_gate_probe(ucn_v6_realtime_owner_t *owner)
{
    uint64_t violations_before;
    ucn_v6_result_t result;

    if (!owner_is_valid(owner) || owner->callback_gate == NULL) {
        return false;
    }
    /* A real try-enter probe records callback re-entry.  A read-only
     * is_active() check would let a provider hide the nested violation. */
    result = callback_scope_enter(owner->callback_gate, owner,
                                  &violations_before);
    if (result != UCN_V6_OK) {
        return false;
    }
    result = callback_scope_finish(owner->callback_gate, owner,
                                   violations_before, UCN_V6_OK);
    if (result != UCN_V6_OK || owner->io_active) {
        owner->io_faulted = true;
        owner->stats.faulted = true;
        return false;
    }
    return true;
}

static bool owner_api_is_available(ucn_v6_realtime_owner_t *owner)
{
    return owner_is_valid(owner) && !owner->io_faulted &&
           owner_gate_probe(owner);
}

static ucn_v6_result_t owner_io_enter(
    ucn_v6_realtime_owner_t *owner, uint64_t *violations_before)
{
    ucn_v6_result_t result;
    if (owner == NULL || owner->io_active || owner->io_faulted) {
        return UCN_V6_ERR_STATE;
    }
    result = callback_scope_enter(owner->callback_gate, owner,
                                  violations_before);
    if (result == UCN_V6_OK) {
        owner->io_active = true;
    }
    return result;
}

static ucn_v6_result_t owner_io_finish(
    ucn_v6_realtime_owner_t *owner, uint64_t violations_before,
    ucn_v6_result_t callback_result)
{
    uint64_t violations_after;
    ucn_v6_result_t leave_result;
    if (owner == NULL || !owner->io_active) {
        return UCN_V6_ERR_STATE;
    }
    violations_after = ucn_v6_callback_gate_violation_count(
        owner->callback_gate);
    leave_result = ucn_v6_callback_gate_leave(owner->callback_gate, owner);
    owner->io_active = false;
    if (leave_result != UCN_V6_OK || violations_before == UINT64_MAX ||
        violations_after == UINT64_MAX ||
        violations_after != violations_before) {
        owner->io_faulted = true;
        owner->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    return callback_result;
}

static bool ranges_overlap(const void *left, size_t left_size,
                           const void *right, size_t right_size)
{
    uintptr_t left_begin = (uintptr_t)left;
    uintptr_t right_begin = (uintptr_t)right;
    uintptr_t left_end;
    uintptr_t right_end;
    if (left_size == 0U || right_size == 0U) {
        return false;
    }
    if (left_begin > UINTPTR_MAX - left_size ||
        right_begin > UINTPTR_MAX - right_size) {
        return true;
    }
    left_end = left_begin + left_size;
    right_end = right_begin + right_size;
    return left_begin < right_end && right_begin < left_end;
}

static bool envelope_is_valid(const ucn_v6_realtime_envelope_t *value)
{
    if (value == NULL ||
        (uint32_t)value->mode <= (uint32_t)UCN_V6_REALTIME_NONE ||
        (uint32_t)value->mode > (uint32_t)UCN_V6_REALTIME_DEADLINE ||
        value->uncertainty_class > UCN_V6_REALTIME_UNCERTAINTY_UNKNOWN ||
        value->capture_time_us == 0U) {
        return false;
    }
    if (value->mode == UCN_V6_REALTIME_LOCAL_STAMP) {
        return !value->domain_time_valid && !value->source_holdover &&
               value->clock_domain_id == 0U && value->domain_generation == 0U;
    }
    return value->domain_time_valid && value->clock_domain_id != 0U &&
           value->clock_domain_id <= UCN_V6_REALTIME_DOMAIN_ID_MAX &&
           value->domain_generation != 0U &&
           value->domain_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           value->uncertainty_class != UCN_V6_REALTIME_UNCERTAINTY_UNKNOWN;
}

ucn_v6_result_t ucn_v6_realtime_envelope_encode(
    const ucn_v6_realtime_envelope_t *envelope,
    uint8_t output[UCN_V6_REALTIME_ENVELOPE_BYTES])
{
    uint8_t encoded[UCN_V6_REALTIME_ENVELOPE_BYTES];
    if (!envelope_is_valid(envelope) || output == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = (uint8_t)((UCN_V6_REALTIME_ENVELOPE_VERSION <<
                            VERSION_SHIFT) |
                           (uint8_t)envelope->mode);
    encoded[1] = (uint8_t)(envelope->uncertainty_class <<
                           QUALITY_UNCERTAINTY_SHIFT);
    if (envelope->sample_capture_hardware) {
        encoded[1] |= QUALITY_HARDWARE;
    }
    if (envelope->domain_time_valid) {
        encoded[1] |= QUALITY_VALID;
    }
    if (envelope->source_holdover) {
        encoded[1] |= QUALITY_HOLDOVER;
    }
    write_u16(&encoded[2], envelope->clock_domain_id);
    write_u32(&encoded[4], envelope->domain_generation);
    write_u64(&encoded[8], envelope->capture_time_us);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_realtime_envelope_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_realtime_envelope_t *envelope)
{
    ucn_v6_realtime_envelope_t decoded;
    if (input == NULL || envelope == NULL ||
        input_length != UCN_V6_REALTIME_ENVELOPE_BYTES ||
        (input[0] >> VERSION_SHIFT) != UCN_V6_REALTIME_ENVELOPE_VERSION ||
        (input[0] & 0x0CU) != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.mode = (ucn_v6_realtime_mode_t)(input[0] & MODE_MASK);
    decoded.uncertainty_class = input[1] >> QUALITY_UNCERTAINTY_SHIFT;
    decoded.sample_capture_hardware = (input[1] & QUALITY_HARDWARE) != 0U;
    decoded.domain_time_valid = (input[1] & QUALITY_VALID) != 0U;
    decoded.source_holdover = (input[1] & QUALITY_HOLDOVER) != 0U;
    decoded.clock_domain_id = read_u16(&input[2]);
    decoded.domain_generation = read_u32(&input[4]);
    decoded.capture_time_us = read_u64(&input[8]);
    if (!envelope_is_valid(&decoded)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *envelope = decoded;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_realtime_uncertainty_aggregate(
    const ucn_v6_realtime_uncertainty_t *components,
    uint32_t *upper_bound_us)
{
    uint64_t total;
    if (components == NULL || upper_bound_us == NULL ||
        components->known_mask != UCN_V6_REALTIME_KN_ALL ||
        components->timer_resolution_bound_us == 0U ||
        components->link_timestamp_capture_bound_us == 0U ||
        components->filter_residual_bound_us == 0U ||
        components->arithmetic_rounding_bound_us == 0U ||
        components->sample_capture_bound_us == 0U ||
        components->path_asymmetry_bound_us == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    total = (uint64_t)components->timer_resolution_bound_us +
            components->link_timestamp_capture_bound_us +
            components->filter_residual_bound_us +
            components->arithmetic_rounding_bound_us +
            components->sample_capture_bound_us +
            components->path_asymmetry_bound_us;
    if (total > UINT32_MAX) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *upper_bound_us = (uint32_t)total;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_time_sync_sample_encode(
    const ucn_v6_time_sync_sample_t *sample,
    uint8_t output[UCN_V6_TIME_SYNC_SAMPLE_BYTES])
{
    uint8_t encoded[UCN_V6_TIME_SYNC_SAMPLE_BYTES];
    uint32_t uncertainty;

    if (sample == NULL || output == NULL || sample->clock_domain_id == 0U ||
        sample->clock_domain_id > UCN_V6_REALTIME_DOMAIN_ID_MAX ||
        sample->domain_generation == 0U ||
        sample->domain_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        sample->local_sample_us == 0U ||
        ucn_v6_realtime_uncertainty_aggregate(&sample->uncertainty,
                                              &uncertainty) != UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    (void)uncertainty;
    memset(encoded, 0, sizeof(encoded));
    write_u16(&encoded[0], sample->clock_domain_id);
    write_u32(&encoded[2], sample->domain_generation);
    write_u64(&encoded[6], sample->local_sample_us);
    write_i64(&encoded[14], sample->offset_us);
    write_u32(&encoded[22], sample->uncertainty.timer_resolution_bound_us);
    write_u32(&encoded[26],
              sample->uncertainty.link_timestamp_capture_bound_us);
    write_u32(&encoded[30], sample->uncertainty.filter_residual_bound_us);
    write_u32(&encoded[34], sample->uncertainty.arithmetic_rounding_bound_us);
    write_u32(&encoded[38], sample->uncertainty.sample_capture_bound_us);
    write_u32(&encoded[42], sample->uncertainty.path_asymmetry_bound_us);
    encoded[46] = sample->uncertainty.known_mask;
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_time_sync_sample_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_time_sync_sample_t *sample)
{
    ucn_v6_time_sync_sample_t decoded;
    uint32_t uncertainty;

    if (input == NULL || sample == NULL ||
        input_length != UCN_V6_TIME_SYNC_SAMPLE_BYTES || input[47] != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.clock_domain_id = read_u16(&input[0]);
    decoded.domain_generation = read_u32(&input[2]);
    decoded.local_sample_us = read_u64(&input[6]);
    if (!read_i64(&input[14], &decoded.offset_us)) {
        return UCN_V6_ERR_MALFORMED;
    }
    decoded.uncertainty.timer_resolution_bound_us = read_u32(&input[22]);
    decoded.uncertainty.link_timestamp_capture_bound_us = read_u32(&input[26]);
    decoded.uncertainty.filter_residual_bound_us = read_u32(&input[30]);
    decoded.uncertainty.arithmetic_rounding_bound_us = read_u32(&input[34]);
    decoded.uncertainty.sample_capture_bound_us = read_u32(&input[38]);
    decoded.uncertainty.path_asymmetry_bound_us = read_u32(&input[42]);
    decoded.uncertainty.known_mask = input[46];
    if (decoded.clock_domain_id == 0U ||
        decoded.clock_domain_id > UCN_V6_REALTIME_DOMAIN_ID_MAX ||
        decoded.domain_generation == 0U ||
        decoded.domain_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        decoded.local_sample_us == 0U ||
        ucn_v6_realtime_uncertainty_aggregate(&decoded.uncertainty,
                                              &uncertainty) != UCN_V6_OK) {
        return UCN_V6_ERR_MALFORMED;
    }
    (void)uncertainty;
    *sample = decoded;
    return UCN_V6_OK;
}

static bool policy_is_valid(const ucn_v6_realtime_endpoint_policy_t *policy)
{
    if (policy == NULL || policy->destination_endpoint == 0U ||
        (uint32_t)policy->mode > (uint32_t)UCN_V6_REALTIME_DEADLINE ||
        (uint32_t)policy->requirement >
            (uint32_t)UCN_V6_REALTIME_REQUIRED) {
        return false;
    }
    if (policy->mode == UCN_V6_REALTIME_NONE) {
        return policy->requirement != UCN_V6_REALTIME_REQUIRED &&
               policy->clock_domain_id == 0U && policy->max_age_us == 0U &&
               policy->max_uncertainty_us == 0U &&
               policy->max_local_holdover_us == 0U &&
               !policy->require_hardware_capture;
    }
    if (policy->mode == UCN_V6_REALTIME_LOCAL_STAMP) {
        return policy->requirement != UCN_V6_REALTIME_REQUIRED &&
               policy->clock_domain_id == 0U && policy->max_age_us == 0U &&
               policy->max_uncertainty_us == 0U &&
               policy->max_local_holdover_us == 0U;
    }
    return policy->clock_domain_id != 0U &&
           policy->clock_domain_id <= UCN_V6_REALTIME_DOMAIN_ID_MAX &&
           policy->max_age_us != 0U && policy->max_uncertainty_us != 0U &&
           policy->max_local_holdover_us != 0U;
}

static bool domain_config_is_valid(const ucn_v6_time_domain_config_t *config)
{
    return config != NULL && config->clock_domain_id != 0U &&
           config->clock_domain_id <= UCN_V6_REALTIME_DOMAIN_ID_MAX &&
           config->domain_generation != 0U &&
           config->domain_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           ucn_v6_principal_is_valid(&config->master_principal) &&
           ucn_v6_binding_key_is_valid(&config->master_binding) &&
           config->master_session_generation != 0U &&
           config->master_session_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           config->lock_sample_count != 0U &&
           config->lock_sample_count <= UCN_V6_REALTIME_SAMPLE_WINDOW &&
           config->sync_timeout_us != 0U && config->max_holdover_us != 0U &&
           UINT64_MAX - config->sync_timeout_us >= config->max_holdover_us &&
           config->max_offset_jump_us != 0U &&
           config->oscillator_uncertainty_ppb != 0U;
}

static bool domain_config_equal(const ucn_v6_time_domain_config_t *left,
                                 const ucn_v6_time_domain_config_t *right)
{
    return left->clock_domain_id == right->clock_domain_id &&
           left->domain_generation == right->domain_generation &&
           principal_equal(&left->master_principal,
                           &right->master_principal) &&
           ucn_v6_binding_key_equal(&left->master_binding,
                                    &right->master_binding) &&
           left->master_session_generation ==
               right->master_session_generation &&
           left->lock_sample_count == right->lock_sample_count &&
           left->sync_timeout_us == right->sync_timeout_us &&
           left->max_holdover_us == right->max_holdover_us &&
           left->max_offset_jump_us == right->max_offset_jump_us &&
           left->oscillator_uncertainty_ppb ==
               right->oscillator_uncertainty_ppb;
}

static bool route_domain_equal(const ucn_v6_route_domain_t *left,
                               const ucn_v6_route_domain_t *right)
{
    return left != NULL && right != NULL &&
           principal_equal(&left->origin_principal,
                           &right->origin_principal) &&
           ucn_v6_binding_key_equal(&left->origin_binding,
                                    &right->origin_binding) &&
           left->origin_session_generation ==
               right->origin_session_generation &&
           principal_equal(&left->destination_principal,
                           &right->destination_principal) &&
           ucn_v6_binding_key_equal(&left->destination_binding,
                                    &right->destination_binding) &&
           left->destination_session_generation ==
               right->destination_session_generation;
}

static bool route_ref_equal(const ucn_v6_route_path_ref_t *left,
                            const ucn_v6_route_path_ref_t *right)
{
    return left != NULL && right != NULL &&
           route_domain_equal(&left->domain, &right->domain) &&
           left->route_generation == right->route_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation;
}

static bool realtime_session_equal(const ucn_v6_session_key_t *left,
                                   const ucn_v6_session_key_t *right)
{
    return left != NULL && right != NULL &&
           principal_equal(&left->principal, &right->principal) &&
           ucn_v6_binding_key_equal(&left->binding, &right->binding) &&
           left->session_generation == right->session_generation;
}

static bool invalidation_equal(const ucn_v6_stack_invalidation_t *left,
                               const ucn_v6_stack_invalidation_t *right)
{
    return left != NULL && right != NULL && left->type == right->type &&
           left->link_id == right->link_id &&
           left->link_generation == right->link_generation &&
           realtime_session_equal(&left->session, &right->session) &&
           left->capability_generation == right->capability_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation;
}

static bool route_ref_is_valid(const ucn_v6_route_path_ref_t *reference)
{
    return reference != NULL &&
           ucn_v6_principal_is_valid(&reference->domain.origin_principal) &&
           ucn_v6_binding_key_is_valid(&reference->domain.origin_binding) &&
           reference->domain.origin_session_generation != 0U &&
           reference->domain.origin_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           ucn_v6_principal_is_valid(
               &reference->domain.destination_principal) &&
           ucn_v6_binding_key_is_valid(
               &reference->domain.destination_binding) &&
           reference->domain.destination_session_generation != 0U &&
           reference->domain.destination_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           reference->domain.origin_binding.realm_id ==
               reference->domain.destination_binding.realm_id &&
           reference->route_generation != 0U &&
           reference->route_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           reference->path_id != 0U && reference->path_id != UINT16_MAX &&
           reference->path_generation != 0U &&
           reference->path_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
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

static bool path_proof_equal(
    const ucn_v6_realtime_path_proof_t *left,
    const ucn_v6_realtime_path_proof_t *right)
{
    return left != NULL && right != NULL &&
           left->destination_capability_generation ==
               right->destination_capability_generation &&
           memcmp(left->destination_capability_digest,
                  right->destination_capability_digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           memcmp(left->local_parent_capability_digest,
                  right->local_parent_capability_digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           left->destination_realtime_mode_bits ==
               right->destination_realtime_mode_bits &&
           left->destination_clock_domain_id ==
               right->destination_clock_domain_id &&
           left->destination_clock_domain_generation ==
               right->destination_clock_domain_generation &&
           left->feature_bits == right->feature_bits &&
           left->timestamp_capability_bits ==
               right->timestamp_capability_bits &&
           left->timestamp_uncertainty_us ==
               right->timestamp_uncertainty_us;
}

static bool path_proof_is_valid(
    const ucn_v6_realtime_path_proof_t *proof)
{
    return proof != NULL &&
           proof->destination_capability_generation != 0U &&
           proof->destination_capability_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           !bytes_are_zero(proof->destination_capability_digest,
                           UCN_V6_CAPABILITY_DIGEST_BYTES) &&
           !bytes_are_zero(proof->local_parent_capability_digest,
                           UCN_V6_CAPABILITY_DIGEST_BYTES) &&
           (proof->destination_realtime_mode_bits &
            UCN_V6_REALTIME_MODE_SYNCED) != 0U &&
           proof->destination_clock_domain_id != 0U &&
           proof->destination_clock_domain_id <=
               UCN_V6_REALTIME_DOMAIN_ID_MAX &&
           proof->destination_clock_domain_generation != 0U &&
           proof->destination_clock_domain_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           (proof->feature_bits & UCN_V6_FEATURE_REALTIME) != 0U &&
           (proof->timestamp_capability_bits &
            (UCN_V6_TIMESTAMP_RX_HARDWARE |
             UCN_V6_TIMESTAMP_TX_HARDWARE)) ==
               (UCN_V6_TIMESTAMP_RX_HARDWARE |
                UCN_V6_TIMESTAMP_TX_HARDWARE) &&
           proof->timestamp_uncertainty_us != 0U;
}

static void build_path_proof(
    const ucn_v6_path_capability_t *path,
    ucn_v6_realtime_path_proof_t *proof)
{
    memset(proof, 0, sizeof(*proof));
    proof->destination_capability_generation =
        path->destination_capability_generation;
    memcpy(proof->destination_capability_digest,
           path->destination_capability_digest,
           UCN_V6_CAPABILITY_DIGEST_BYTES);
    memcpy(proof->local_parent_capability_digest,
           path->local_parent_capability_digest,
           UCN_V6_CAPABILITY_DIGEST_BYTES);
    proof->destination_realtime_mode_bits =
        path->destination_realtime_mode_bits;
    proof->destination_clock_domain_id = path->destination_clock_domain_id;
    proof->destination_clock_domain_generation =
        path->destination_clock_domain_generation;
    proof->feature_bits = path->feature_bits;
    proof->timestamp_capability_bits = path->timestamp_capability_bits;
    proof->timestamp_uncertainty_us = path->timestamp_uncertainty_us;
}

static bool realtime_path_is_admissible(
    const ucn_v6_path_capability_t *path,
    const ucn_v6_time_domain_config_t *config)
{
    return path != NULL && config != NULL && path->valid &&
           path->immutable_for_realtime &&
           principal_equal(&path->destination_principal,
                           &config->master_principal) &&
           ucn_v6_binding_key_equal(&path->destination_binding,
                                    &config->master_binding) &&
           path->destination_session_generation ==
               config->master_session_generation &&
           (path->destination_realtime_mode_bits &
            UCN_V6_REALTIME_MODE_SYNCED) != 0U &&
           path->destination_clock_domain_id == config->clock_domain_id &&
           path->destination_clock_domain_generation ==
               config->domain_generation &&
           (path->feature_bits & UCN_V6_FEATURE_REALTIME) != 0U &&
           (path->timestamp_capability_bits &
            (UCN_V6_TIMESTAMP_RX_HARDWARE |
             UCN_V6_TIMESTAMP_TX_HARDWARE)) ==
               (UCN_V6_TIMESTAMP_RX_HARDWARE |
                UCN_V6_TIMESTAMP_TX_HARDWARE) &&
           path->timestamp_uncertainty_us != 0U;
}

static bool domain_record_is_valid(
    const ucn_v6_realtime_domain_record_t *record)
{
    return record != NULL && domain_config_is_valid(&record->config) &&
           route_ref_is_valid(&record->route_ref) &&
           ucn_v6_stack_invalidation_is_valid(
               &record->route_dependency) &&
           record->route_dependency.type == UCN_V6_STACK_INVALIDATE_PATH &&
           path_proof_is_valid(&record->path_proof) &&
           principal_equal(&record->route_ref.domain.destination_principal,
                           &record->config.master_principal) &&
           ucn_v6_binding_key_equal(
               &record->route_ref.domain.destination_binding,
               &record->config.master_binding) &&
           record->route_ref.domain.destination_session_generation ==
               record->config.master_session_generation &&
           record->route_ref.path_id ==
               record->route_dependency.path_id &&
           record->route_ref.path_generation ==
               record->route_dependency.path_generation &&
           record->path_proof.destination_clock_domain_id ==
               record->config.clock_domain_id &&
           record->path_proof.destination_clock_domain_generation ==
               record->config.domain_generation;
}

static bool domain_record_equal(
    const ucn_v6_realtime_domain_record_t *left,
    const ucn_v6_realtime_domain_record_t *right)
{
    return domain_config_equal(&left->config, &right->config) &&
           route_ref_equal(&left->route_ref, &right->route_ref) &&
           invalidation_equal(&left->route_dependency,
                              &right->route_dependency) &&
           path_proof_equal(&left->path_proof, &right->path_proof);
}

static bool serial_not_older(uint32_t previous, uint32_t next)
{
    return previous != 0U &&
           previous <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           next >= previous && next <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

/* A Time Domain generation is the outer authority generation, but it does not
 * erase the anti-rollback rules of its still-live parent domains.  Values may
 * restart only when the RFC-defined parent changes (Binding, Session, Link
 * slot, or Path ID). */
static bool domain_record_transition_is_valid(
    const ucn_v6_realtime_domain_record_t *previous,
    const ucn_v6_realtime_domain_record_t *next)
{
    uint32_t expected_domain_generation;
    uint32_t expected_session_generation;
    bool same_principal;
    bool same_binding;
    bool same_session;

    if (!domain_record_is_valid(previous) || !domain_record_is_valid(next) ||
        previous->config.clock_domain_id != next->config.clock_domain_id ||
        ucn_v6_serial_checked_next(previous->config.domain_generation,
                                   &expected_domain_generation) != UCN_V6_OK ||
        expected_domain_generation != next->config.domain_generation) {
        return false;
    }

    same_principal = principal_equal(&previous->config.master_principal,
                                     &next->config.master_principal);
    same_binding = same_principal && ucn_v6_binding_key_equal(
        &previous->config.master_binding, &next->config.master_binding);
    if (!same_binding) {
        /* A new Principal/Binding is a new parent domain.  The candidate was
         * already validated canonically, so its child generations may begin
         * from their legal non-zero values. */
        return true;
    }

    same_session = previous->config.master_session_generation ==
                   next->config.master_session_generation;
    if (!same_session) {
        if (ucn_v6_serial_checked_next(
                previous->config.master_session_generation,
                &expected_session_generation) != UCN_V6_OK ||
            expected_session_generation !=
                next->config.master_session_generation ||
            next->path_proof.destination_capability_generation !=
                1U) {
            return false;
        }
        /* Security and Route Owners already prove their own child generation
         * histories. Realtime persists only the exact resolved proof used by
         * this Domain generation. */
        return true;
    }

    return serial_not_older(
        previous->path_proof.destination_capability_generation,
        next->path_proof.destination_capability_generation);
}

/* A Member restart or a re-authenticated Peer Session does not own the
 * Master-issued Domain generation.  It may rebind that same Domain only when
 * Security has advanced the exact Master Session parent.  The old Session's
 * Capability/Route/Path child generations may then restart, while local
 * policy and the Master identity remain unchanged. */
static bool domain_record_session_recovery_is_valid(
    const ucn_v6_realtime_domain_record_t *previous,
    const ucn_v6_realtime_domain_record_t *next)
{
    uint32_t expected_session_generation;

    if (!domain_record_is_valid(previous) || !domain_record_is_valid(next) ||
        previous->config.clock_domain_id != next->config.clock_domain_id ||
        previous->config.domain_generation != next->config.domain_generation ||
        !principal_equal(&previous->config.master_principal,
                         &next->config.master_principal) ||
        !ucn_v6_binding_key_equal(&previous->config.master_binding,
                                  &next->config.master_binding) ||
        ucn_v6_serial_checked_next(
            previous->config.master_session_generation,
            &expected_session_generation) != UCN_V6_OK ||
        next->config.master_session_generation != expected_session_generation ||
        next->path_proof.destination_capability_generation != 1U ||
        previous->config.lock_sample_count != next->config.lock_sample_count ||
        previous->config.sync_timeout_us != next->config.sync_timeout_us ||
        previous->config.max_holdover_us != next->config.max_holdover_us ||
        previous->config.max_offset_jump_us !=
            next->config.max_offset_jump_us ||
        previous->config.oscillator_uncertainty_ppb !=
            next->config.oscillator_uncertainty_ppb) {
        return false;
    }
    return true;
}

static void build_domain_record(
    const ucn_v6_time_domain_config_t *config,
    const ucn_v6_route_path_ref_t *route_ref,
    const ucn_v6_route_resolution_t *resolution,
    ucn_v6_realtime_domain_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->config = *config;
    record->route_ref = *route_ref;
    record->route_dependency = resolution->dependency;
    build_path_proof(&resolution->path.capability, &record->path_proof);
}

static void build_domain_record_from_slot(
    const ucn_v6_time_domain_slot_t *slot,
    ucn_v6_realtime_domain_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->config = slot->config;
    record->route_ref = slot->route_ref;
    record->route_dependency = slot->route_dependency;
    record->path_proof = slot->path_proof;
}

ucn_v6_result_t ucn_v6_realtime_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_route_owner_t *route_owner,
    const ucn_v6_realtime_generation_store_ops_t *generation_store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_realtime_owner_t **owner)
{
    ucn_v6_realtime_owner_t initialized;
    uint64_t violations_before = 0U;
    ucn_v6_result_t gate_result;
    if (owner == NULL || route_owner == NULL ||
        generation_store == NULL ||
        generation_store->load_domain_record == NULL ||
        generation_store->reserve_domain_record == NULL ||
        callback_gate == NULL ||
        ucn_v6_callback_gate_violation_count(callback_gate) == UINT64_MAX ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_REALTIME_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK) {
        return UCN_V6_ERR_CONFIG;
    }
    /* A recursive init from a generation-store callback must be observable by
     * the outer scope, while caller storage remains untouched. */
    gate_result = callback_scope_enter(callback_gate, storage,
                                       &violations_before);
    if (gate_result != UCN_V6_OK) {
        return gate_result;
    }
    gate_result = callback_scope_finish(callback_gate, storage,
                                        violations_before, UCN_V6_OK);
    if (gate_result != UCN_V6_OK) {
        return gate_result;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_V6_REALTIME_OWNER_MAGIC;
    initialized.schema = UCN_V6_REALTIME_OWNER_SCHEMA;
    initialized.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    initialized.route_owner = route_owner;
    initialized.generation_store = *generation_store;
    initialized.callback_gate = callback_gate;
    initialized.initialized = true;
    initialized.canary = UCN_V6_REALTIME_OWNER_CANARY;
    memcpy(storage, &initialized, sizeof(initialized));
    *owner = (ucn_v6_realtime_owner_t *)storage;
    return UCN_V6_OK;
}

static ucn_v6_realtime_policy_slot_t *find_policy(
    ucn_v6_realtime_owner_t *owner, uint16_t endpoint)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_REALTIME_ENDPOINTS; ++index) {
        if (owner->policies[index].occupied &&
            owner->policies[index].policy.destination_endpoint == endpoint) {
            return &owner->policies[index];
        }
    }
    return NULL;
}

ucn_v6_result_t ucn_v6_realtime_set_endpoint_policy(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_realtime_endpoint_policy_t *policy)
{
    ucn_v6_realtime_policy_slot_t *slot;
    size_t index;
    if (!owner_is_valid(owner) || !policy_is_valid(policy)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_api_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    slot = find_policy(owner, policy->destination_endpoint);
    if (slot == NULL) {
        for (index = 0U; index < UCN_V6_CONFIG_REALTIME_ENDPOINTS; ++index) {
            if (!owner->policies[index].occupied) {
                slot = &owner->policies[index];
                break;
            }
        }
    }
    if (slot == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (!slot->occupied) {
        ++owner->stats.endpoint_policies;
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->policy = *policy;
    return UCN_V6_OK;
}

static ucn_v6_time_domain_slot_t *find_domain(
    ucn_v6_realtime_owner_t *owner, uint16_t domain_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TIME_DOMAINS; ++index) {
        if (owner->domains[index].occupied &&
            owner->domains[index].config.clock_domain_id == domain_id) {
            return &owner->domains[index];
        }
    }
    return NULL;
}

static void clear_acquisition(ucn_v6_time_domain_slot_t *slot);

static bool copy_current_domain_dependencies(
    const ucn_v6_realtime_owner_t *owner,
    const ucn_v6_time_domain_config_t *config,
    const ucn_v6_route_path_ref_t *route_ref,
    uint64_t now_us,
    ucn_v6_route_resolution_t *resolution)
{
    if (owner == NULL || owner->route_owner == NULL || config == NULL ||
        route_ref == NULL || resolution == NULL ||
        !principal_equal(&route_ref->domain.destination_principal,
                         &config->master_principal) ||
        !ucn_v6_binding_key_equal(&route_ref->domain.destination_binding,
                                  &config->master_binding) ||
        route_ref->domain.destination_session_generation !=
            config->master_session_generation ||
        ucn_v6_route_resolve_ref(owner->route_owner, now_us, route_ref,
                                 resolution) != UCN_V6_OK) {
        return false;
    }
    return realtime_path_is_admissible(&resolution->path.capability, config);
}

static bool domain_dependency_is_current(
    const ucn_v6_realtime_owner_t *owner,
    const ucn_v6_time_domain_slot_t *slot,
    uint64_t now_us)
{
    ucn_v6_route_resolution_t resolution;
    ucn_v6_realtime_path_proof_t current_proof;
    if (owner == NULL || slot == NULL || !slot->occupied) {
        return false;
    }
    if (!copy_current_domain_dependencies(
            owner, &slot->config, &slot->route_ref, now_us, &resolution)) {
        return false;
    }
    build_path_proof(&resolution.path.capability, &current_proof);
    return invalidation_equal(&slot->route_dependency,
                              &resolution.dependency) &&
           path_proof_equal(&slot->path_proof, &current_proof);
}

static bool phase_holds_lock(ucn_v6_time_domain_phase_t phase)
{
    return phase == UCN_V6_TIME_LOCKED || phase == UCN_V6_TIME_HOLDOVER;
}

static void fault_domain(ucn_v6_realtime_owner_t *owner,
                         ucn_v6_time_domain_slot_t *slot)
{
    if (phase_holds_lock(slot->phase) && owner->stats.locked_domains != 0U) {
        --owner->stats.locked_domains;
    }
    slot->phase = UCN_V6_TIME_FAULT;
    owner->stats.faulted = true;
}

static bool expire_domain_dependencies(ucn_v6_realtime_owner_t *owner,
                                       ucn_v6_time_domain_slot_t *slot,
                                       uint64_t now_us)
{
    if (owner == NULL || slot == NULL) {
        return false;
    }
    if (slot->dependency_invalidated) {
        return true;
    }
    if (domain_dependency_is_current(owner, slot, now_us) &&
        slot->dependency_deadline_us != 0U &&
        now_us < slot->dependency_deadline_us) {
        return false;
    }
    if (phase_holds_lock(slot->phase) &&
        owner->stats.locked_domains != 0U) {
        --owner->stats.locked_domains;
    }
    if (slot->phase != UCN_V6_TIME_FAULT) {
        slot->phase = UCN_V6_TIME_UNSYNCED;
        clear_acquisition(slot);
    }
    slot->dependency_invalidated = true;
    slot->dependency_deadline_us = 0U;
    return true;
}

ucn_v6_result_t ucn_v6_realtime_bind_domain(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_time_domain_config_t *config,
    const ucn_v6_route_path_ref_t *fixed_route_ref,
    uint64_t now_us)
{
    ucn_v6_time_domain_slot_t replacement;
    ucn_v6_time_domain_slot_t *slot;
    ucn_v6_realtime_domain_record_t runtime_record;
    ucn_v6_realtime_domain_record_t durable;
    ucn_v6_realtime_domain_record_t candidate;
    ucn_v6_result_t result;
    ucn_v6_route_resolution_t resolution;
    uint64_t violations_before = 0U;
    bool runtime_transition = false;
    bool runtime_session_recovery = false;
    bool reserve_required = false;
    size_t index;
    if (!owner_is_valid(owner) || !domain_config_is_valid(config) ||
        fixed_route_ref == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_api_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!copy_current_domain_dependencies(
            owner, config, fixed_route_ref, now_us, &resolution)) {
        return UCN_V6_ERR_ACCESS;
    }
    build_domain_record(config, fixed_route_ref, &resolution, &candidate);
    if (!domain_record_is_valid(&candidate)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_domain(owner, config->clock_domain_id);
    if (slot != NULL) {
        build_domain_record_from_slot(slot, &runtime_record);
        if (config->domain_generation < slot->config.domain_generation) {
            return UCN_V6_ERR_REPLAY;
        }
        if (config->domain_generation == slot->config.domain_generation) {
            runtime_session_recovery =
                domain_record_session_recovery_is_valid(&runtime_record,
                                                        &candidate);
            if (!runtime_session_recovery) {
                return UCN_V6_ERR_REPLAY;
            }
        } else {
            runtime_transition = domain_record_transition_is_valid(
                &runtime_record, &candidate);
            if (!runtime_transition) {
                return UCN_V6_ERR_REPLAY;
            }
        }
    }
    if (slot == NULL) {
        for (index = 0U; index < UCN_V6_CONFIG_TIME_DOMAINS; ++index) {
            if (!owner->domains[index].occupied) {
                slot = &owner->domains[index];
                break;
            }
        }
    }
    if (slot == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    result = owner_io_enter(owner, &violations_before);
    if (result != UCN_V6_OK) {
        return result;
    }
    memset(&durable, 0, sizeof(durable));
    result = owner->generation_store.load_domain_record(
        owner->generation_store.context, config->clock_domain_id, &durable);
    if (result != UCN_V6_OK && result != UCN_V6_ERR_NOT_FOUND) {
        result = UCN_V6_ERR_STATE;
        goto io_finished;
    }
    if (result == UCN_V6_OK &&
        (!domain_record_is_valid(&durable) ||
         durable.config.clock_domain_id != config->clock_domain_id)) {
        result = UCN_V6_ERR_STATE;
        goto io_finished;
    }
    if (result == UCN_V6_OK &&
        durable.config.domain_generation > config->domain_generation) {
        result = UCN_V6_ERR_REPLAY;
        goto io_finished;
    }
    if (result == UCN_V6_OK &&
        durable.config.domain_generation == config->domain_generation) {
        if (domain_record_equal(&durable, &candidate)) {
            /* Exact durable bytes may complete an interrupted in-process
             * transition, but a fresh Owner cannot resurrect an already-used
             * Security Session without a new authenticated challenge. */
            if (!runtime_transition && !runtime_session_recovery) {
                result = UCN_V6_ERR_REPLAY;
                goto io_finished;
            }
        } else {
            if (!domain_record_session_recovery_is_valid(&durable,
                                                         &candidate)) {
                result = UCN_V6_ERR_REPLAY;
                goto io_finished;
            }
            reserve_required = true;
        }
    } else if (result == UCN_V6_OK &&
               durable.config.domain_generation <
                   config->domain_generation) {
        if (!domain_record_transition_is_valid(&durable, &candidate)) {
            result = UCN_V6_ERR_REPLAY;
            goto io_finished;
        }
        reserve_required = true;
    } else if (result == UCN_V6_ERR_NOT_FOUND) {
        reserve_required = true;
    }
    if (reserve_required) {
        result = owner->generation_store.reserve_domain_record(
            owner->generation_store.context, &candidate);
        if (result != UCN_V6_OK) {
            result = UCN_V6_ERR_STATE;
            goto io_finished;
        }
        memset(&durable, 0, sizeof(durable));
        result = owner->generation_store.load_domain_record(
            owner->generation_store.context, config->clock_domain_id, &durable);
        if (result != UCN_V6_OK ||
            !domain_record_is_valid(&durable) ||
            !domain_record_equal(&durable, &candidate)) {
            result = UCN_V6_ERR_STATE;
            goto io_finished;
        }
    }
    result = UCN_V6_OK;

io_finished:
    result = owner_io_finish(owner, violations_before, result);
    if (result != UCN_V6_OK) {
        return result;
    }
    memset(&replacement, 0, sizeof(replacement));
    replacement.occupied = true;
    replacement.config = *config;
    replacement.phase = UCN_V6_TIME_ACQUIRING;
    replacement.route_ref = *fixed_route_ref;
    replacement.route_dependency = candidate.route_dependency;
    replacement.path_proof = candidate.path_proof;
    replacement.dependency_deadline_us =
        resolution.path.capability.deadline_us;
    if (runtime_session_recovery) {
        /* The Security Session changed, so acquisition/filter history cannot
         * cross the parent boundary.  Runtime monotonic sample/output fences,
         * however, belong to this Member and this Domain generation and must
         * survive the rebind. */
        replacement.last_sample_local_us = slot->last_sample_local_us;
        replacement.last_output_local_us = slot->last_output_local_us;
        replacement.last_output_domain_us = slot->last_output_domain_us;
        replacement.has_sample_high_water = slot->has_sample_high_water;
        replacement.has_output_high_water = slot->has_output_high_water;
    }
    if (!slot->occupied) {
        ++owner->stats.domains;
    } else if (phase_holds_lock(slot->phase) &&
               owner->stats.locked_domains != 0U) {
        --owner->stats.locked_domains;
    }
    *slot = replacement;
    return UCN_V6_OK;
}

static uint64_t magnitude_i64(int64_t value)
{
    return value < 0 ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;
}

static bool difference_i64(int64_t left, int64_t right, int64_t *result)
{
    if (result == NULL ||
        (right < 0 && left > INT64_MAX + right) ||
        (right > 0 && left < INT64_MIN + right)) {
        return false;
    }
    *result = left - right;
    return true;
}

static int64_t median_offset(const ucn_v6_time_domain_slot_t *slot)
{
    int64_t values[UCN_V6_REALTIME_SAMPLE_WINDOW];
    size_t index;
    size_t position;
    for (index = 0U; index < slot->sample_count; ++index) {
        values[index] = slot->offsets[index];
    }
    for (index = 1U; index < slot->sample_count; ++index) {
        int64_t value = values[index];
        position = index;
        while (position != 0U && values[position - 1U] > value) {
            values[position] = values[position - 1U];
            --position;
        }
        values[position] = value;
    }
    return values[slot->sample_count / 2U];
}

static bool apply_offset(uint64_t local, int64_t offset, uint64_t *domain)
{
    uint64_t magnitude = magnitude_i64(offset);
    if (offset < 0) {
        if (local < magnitude) {
            return false;
        }
        *domain = local - magnitude;
        return true;
    }
    if (UINT64_MAX - local < magnitude) {
        return false;
    }
    *domain = local + magnitude;
    return true;
}

ucn_v6_result_t ucn_v6_realtime_ingest_sample(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    uint64_t now_us)
{
    ucn_v6_time_sync_sample_t decoded;
    const ucn_v6_time_sync_sample_t *sample = &decoded;
    ucn_v6_time_domain_slot_t *slot;
    uint32_t uncertainty;
    int64_t next_offset;
    int64_t offset_delta;
    uint64_t candidate_time;
    if (!owner_is_valid(owner) || opened == NULL ||
        opened->frame.frame_type != UCN_V6_FRAME_CONTROL ||
        (opened->frame.flags & UCN_V6_FLAG_PROTOCOL_CONTEXT) == 0U ||
        opened->frame.protocol_opcode !=
            UCN_V6_PROTOCOL_OPCODE_TIME_FOLLOW_UP ||
        ucn_v6_time_sync_sample_decode(opened->frame.payload,
                                       opened->frame.payload_length,
                                       &decoded) != UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_api_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    slot = find_domain(owner, sample->clock_domain_id);
    if (slot != NULL && expire_domain_dependencies(owner, slot, now_us)) {
        increment_saturated(&owner->stats.rejected_samples);
        return UCN_V6_ERR_ACCESS;
    }
    if (slot == NULL || slot->phase == UCN_V6_TIME_FAULT ||
        sample->local_sample_us > now_us ||
        sample->domain_generation != slot->config.domain_generation ||
        !opened->hop_authenticated || !opened->endpoint_authorized ||
        (opened->frame.flags & UCN_V6_FLAG_E2E_CONTEXT) == 0U ||
        !principal_equal(&opened->authenticated_principal,
                         &slot->config.master_principal) ||
        opened->frame.realm_id != slot->config.master_binding.realm_id ||
        opened->frame.source_address != slot->config.master_binding.node_address ||
        opened->frame.source_binding_generation !=
            slot->config.master_binding.binding_generation ||
        opened->frame.session_generation !=
            slot->config.master_session_generation ||
        (opened->frame.flags & (UCN_V6_FLAG_ROUTE_CONTEXT |
                                UCN_V6_FLAG_PATH_CONTEXT)) !=
            (UCN_V6_FLAG_ROUTE_CONTEXT | UCN_V6_FLAG_PATH_CONTEXT) ||
        opened->frame.route_generation != slot->route_ref.route_generation ||
        opened->frame.path.path_id != slot->route_ref.path_id ||
        opened->frame.path.path_generation !=
            slot->route_ref.path_generation ||
        (slot->has_sample_high_water &&
         sample->local_sample_us <= slot->last_sample_local_us) ||
        ucn_v6_realtime_uncertainty_aggregate(
            &sample->uncertainty, &uncertainty) != UCN_V6_OK) {
        increment_saturated(&owner->stats.rejected_samples);
        return UCN_V6_ERR_ACCESS;
    }
    if (slot->sample_count != 0U &&
        (!difference_i64(sample->offset_us, slot->offset_us,
                         &offset_delta) ||
         magnitude_i64(offset_delta) > slot->config.max_offset_jump_us)) {
        fault_domain(owner, slot);
        return UCN_V6_ERR_STATE;
    }
    slot->offsets[slot->sample_cursor] = sample->offset_us;
    slot->sample_cursor = (uint8_t)((slot->sample_cursor + 1U) %
                                    UCN_V6_REALTIME_SAMPLE_WINDOW);
    if (slot->sample_count < UCN_V6_REALTIME_SAMPLE_WINDOW) {
        ++slot->sample_count;
    }
    if (slot->consecutive_samples < UINT8_MAX) {
        ++slot->consecutive_samples;
    }
    next_offset = median_offset(slot);
    if (!apply_offset(sample->local_sample_us, next_offset,
                      &candidate_time) ||
        (slot->has_output_high_water &&
         candidate_time < slot->last_output_domain_us)) {
        fault_domain(owner, slot);
        return UCN_V6_ERR_STATE;
    }
    slot->offset_us = next_offset;
    slot->last_sample_local_us = sample->local_sample_us;
    slot->base_uncertainty_us = uncertainty;
    slot->has_sample_high_water = true;
    if (slot->consecutive_samples >= slot->config.lock_sample_count &&
        slot->phase != UCN_V6_TIME_LOCKED) {
        if (slot->phase != UCN_V6_TIME_HOLDOVER) {
            ++owner->stats.locked_domains;
        }
        slot->phase = UCN_V6_TIME_LOCKED;
    }
    increment_saturated(&owner->stats.accepted_samples);
    return UCN_V6_OK;
}

static void clear_acquisition(ucn_v6_time_domain_slot_t *slot)
{
    memset(slot->offsets, 0, sizeof(slot->offsets));
    slot->sample_count = 0U;
    slot->sample_cursor = 0U;
    slot->consecutive_samples = 0U;
    slot->offset_us = 0;
    slot->base_uncertainty_us = 0U;
}

ucn_v6_result_t ucn_v6_realtime_step(
    ucn_v6_realtime_owner_t *owner,
    uint64_t now_us)
{
    size_t index;
    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_api_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_TIME_DOMAINS; ++index) {
        ucn_v6_time_domain_slot_t *slot = &owner->domains[index];
        uint64_t elapsed;
        uint64_t expiry_age;
        if (!slot->occupied || slot->phase == UCN_V6_TIME_FAULT ||
            expire_domain_dependencies(owner, slot, now_us) ||
            !slot->has_sample_high_water) {
            continue;
        }
        if (now_us < slot->last_sample_local_us) {
            fault_domain(owner, slot);
            continue;
        }
        elapsed = now_us - slot->last_sample_local_us;
        expiry_age = slot->config.sync_timeout_us +
                     slot->config.max_holdover_us;
        if (elapsed >= expiry_age) {
            if (phase_holds_lock(slot->phase) &&
                owner->stats.locked_domains != 0U) {
                --owner->stats.locked_domains;
            }
            slot->phase = UCN_V6_TIME_UNSYNCED;
            clear_acquisition(slot);
        } else if (elapsed >= slot->config.sync_timeout_us &&
                   slot->phase == UCN_V6_TIME_LOCKED) {
            slot->phase = UCN_V6_TIME_HOLDOVER;
        }
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_realtime_get_clock(
    ucn_v6_realtime_owner_t *owner,
    uint16_t clock_domain_id,
    uint64_t local_now_us,
    ucn_v6_realtime_clock_view_t *view)
{
    ucn_v6_time_domain_slot_t *slot;
    ucn_v6_realtime_clock_view_t produced;
    uint64_t elapsed;
    uint64_t growth;
    uint64_t seconds;
    uint64_t remainder;
    uint64_t domain_time;
    if (!owner_is_valid(owner) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_api_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    slot = find_domain(owner, clock_domain_id);
    if (slot != NULL && expire_domain_dependencies(owner, slot, local_now_us)) {
        return UCN_V6_ERR_TIMEOUT;
    }
    if (slot == NULL || !slot->has_sample_high_water ||
        local_now_us < slot->last_sample_local_us ||
        slot->phase == UCN_V6_TIME_FAULT) {
        return UCN_V6_ERR_STATE;
    }
    elapsed = local_now_us - slot->last_sample_local_us;
    if (elapsed >= slot->config.sync_timeout_us +
                   slot->config.max_holdover_us) {
        if (phase_holds_lock(slot->phase) &&
            owner->stats.locked_domains != 0U) {
            --owner->stats.locked_domains;
        }
        slot->phase = UCN_V6_TIME_UNSYNCED;
        clear_acquisition(slot);
        return UCN_V6_ERR_TIMEOUT;
    }
    if (elapsed >= slot->config.sync_timeout_us) {
        slot->phase = UCN_V6_TIME_HOLDOVER;
    }
    if (slot->phase != UCN_V6_TIME_LOCKED &&
        slot->phase != UCN_V6_TIME_HOLDOVER) {
        return UCN_V6_ERR_STATE;
    }
    seconds = elapsed / UINT64_C(1000000000);
    remainder = elapsed % UINT64_C(1000000000);
    if (seconds > UINT64_MAX /
            slot->config.oscillator_uncertainty_ppb ||
        remainder > (UINT64_MAX - UINT64_C(999999999)) /
            slot->config.oscillator_uncertainty_ppb) {
        fault_domain(owner, slot);
        return UCN_V6_ERR_STATE;
    }
    growth = seconds * slot->config.oscillator_uncertainty_ppb +
             (remainder * slot->config.oscillator_uncertainty_ppb +
              UINT64_C(999999999)) / UINT64_C(1000000000);
    if (growth > UINT32_MAX ||
        growth + slot->base_uncertainty_us > UINT32_MAX ||
        !apply_offset(local_now_us, slot->offset_us, &domain_time) ||
        (slot->has_output_high_water &&
         (local_now_us < slot->last_output_local_us ||
          domain_time < slot->last_output_domain_us))) {
        fault_domain(owner, slot);
        return UCN_V6_ERR_STATE;
    }
    memset(&produced, 0, sizeof(produced));
    produced.available = true;
    produced.uncertainty_known = true;
    produced.holdover = slot->phase == UCN_V6_TIME_HOLDOVER;
    produced.clock_domain_id = slot->config.clock_domain_id;
    produced.domain_generation = slot->config.domain_generation;
    produced.domain_time_us = domain_time;
    produced.uncertainty_us = slot->base_uncertainty_us + (uint32_t)growth;
    produced.holdover_age_us = produced.holdover ?
        elapsed - slot->config.sync_timeout_us : 0U;
    slot->last_output_local_us = local_now_us;
    slot->last_output_domain_us = domain_time;
    slot->has_output_high_water = true;
    *view = produced;
    return UCN_V6_OK;
}

static ucn_v6_result_t uncertainty_class_encode(uint32_t value,
                                                 uint8_t *result)
{
    uint8_t value_class = 0U;
    uint32_t bound = 1U;
    if (value == 0U || result == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    while (bound < value && value_class < 30U) {
        bound <<= 1U;
        ++value_class;
    }
    if (bound < value) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *result = value_class;
    return UCN_V6_OK;
}

static ucn_v6_result_t uncertainty_class_decode(uint8_t value_class,
                                                 uint32_t *result)
{
    if (result == NULL || value_class > 30U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *result = UINT32_C(1) << value_class;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_realtime_prepare_payload(
    ucn_v6_realtime_owner_t *owner,
    uint16_t destination_endpoint,
    uint64_t local_capture_us,
    uint32_t sample_capture_bound_us,
    bool hardware_capture,
    const uint8_t *business_payload,
    size_t business_length,
    uint8_t *output,
    size_t output_capacity,
    ucn_v6_realtime_send_result_t *result)
{
    ucn_v6_realtime_policy_slot_t *slot;
    ucn_v6_realtime_send_result_t produced;
    ucn_v6_realtime_envelope_t envelope;
    ucn_v6_realtime_clock_view_t clock;
    uint8_t encoded[UCN_V6_REALTIME_ENVELOPE_BYTES];
    uint64_t sender_uncertainty;
    if (!owner_is_valid(owner) ||
        destination_endpoint == 0U || local_capture_us == 0U ||
        business_payload == NULL || business_length == 0U ||
        output == NULL || result == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_api_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    slot = find_policy(owner, destination_endpoint);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    memset(&produced, 0, sizeof(produced));
    produced.mode = slot->policy.mode;
    if (business_length > SIZE_MAX - UCN_V6_REALTIME_ENVELOPE_BYTES ||
        ranges_overlap(business_payload, business_length, output,
                       slot->policy.mode == UCN_V6_REALTIME_NONE ?
                           business_length :
                           business_length + UCN_V6_REALTIME_ENVELOPE_BYTES)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (slot->policy.mode == UCN_V6_REALTIME_NONE) {
        if (output_capacity < business_length) {
            return UCN_V6_ERR_NO_SPACE;
        }
        memcpy(output, business_payload, business_length);
        produced.payload_length = business_length;
        *result = produced;
        return UCN_V6_OK;
    }
    if (sample_capture_bound_us == 0U ||
        (slot->policy.require_hardware_capture && !hardware_capture) ||
        output_capacity < business_length + UCN_V6_REALTIME_ENVELOPE_BYTES) {
        return UCN_V6_ERR_ACCESS;
    }
    memset(&envelope, 0, sizeof(envelope));
    envelope.mode = slot->policy.mode;
    envelope.sample_capture_hardware = hardware_capture;
    envelope.capture_time_us = local_capture_us;
    if (slot->policy.mode == UCN_V6_REALTIME_LOCAL_STAMP) {
        if (uncertainty_class_encode(sample_capture_bound_us,
                                     &envelope.uncertainty_class) !=
            UCN_V6_OK) {
            return UCN_V6_ERR_EXHAUSTED;
        }
    } else {
        if (ucn_v6_realtime_get_clock(owner, slot->policy.clock_domain_id,
                                      local_capture_us, &clock) != UCN_V6_OK ||
            (clock.holdover &&
             clock.holdover_age_us >=
                 slot->policy.max_local_holdover_us)) {
            return UCN_V6_ERR_STATE;
        }
        sender_uncertainty = (uint64_t)clock.uncertainty_us +
                             sample_capture_bound_us;
        if (sender_uncertainty > UINT32_MAX ||
            uncertainty_class_encode((uint32_t)sender_uncertainty,
                                     &envelope.uncertainty_class) !=
                UCN_V6_OK) {
            return UCN_V6_ERR_EXHAUSTED;
        }
        envelope.domain_time_valid = true;
        envelope.source_holdover = clock.holdover;
        envelope.clock_domain_id = clock.clock_domain_id;
        envelope.domain_generation = clock.domain_generation;
        envelope.capture_time_us = clock.domain_time_us;
    }
    if (ucn_v6_realtime_envelope_encode(&envelope, encoded) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    memcpy(output, encoded, sizeof(encoded));
    memcpy(output + sizeof(encoded), business_payload, business_length);
    produced.payload_length = sizeof(encoded) + business_length;
    produced.business_offset = sizeof(encoded);
    *result = produced;
    return UCN_V6_OK;
}

static ucn_v6_result_t evaluate_payload(
    ucn_v6_realtime_owner_t *owner,
    uint64_t local_now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_realtime_receive_view_t *view)
{
    ucn_v6_realtime_policy_slot_t *slot;
    ucn_v6_realtime_receive_view_t evaluated;
    ucn_v6_realtime_clock_view_t clock;
    uint32_t source_uncertainty;
    uint64_t combined;
    uint64_t age;
    if (!owner_is_valid(owner) || opened == NULL || view == NULL ||
        opened->frame.message.destination_endpoint == 0U ||
        opened->frame.payload == NULL || opened->frame.payload_length == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_api_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    slot = find_policy(owner, opened->frame.message.destination_endpoint);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    memset(&evaluated, 0, sizeof(evaluated));
    evaluated.business_length = opened->frame.payload_length;
    if (slot->policy.mode == UCN_V6_REALTIME_NONE) {
        evaluated.accepted = true;
        *view = evaluated;
        return UCN_V6_OK;
    }
    if (opened->frame.payload_length < UCN_V6_REALTIME_ENVELOPE_BYTES ||
        ucn_v6_realtime_envelope_decode(
            opened->frame.payload, UCN_V6_REALTIME_ENVELOPE_BYTES,
            &evaluated.envelope) != UCN_V6_OK) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_MALFORMED;
        goto rejected;
    }
    evaluated.metadata_present = true;
    evaluated.business_offset = UCN_V6_REALTIME_ENVELOPE_BYTES;
    evaluated.business_length -= UCN_V6_REALTIME_ENVELOPE_BYTES;
    if (evaluated.envelope.mode != slot->policy.mode) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_DOMAIN;
        goto rejected;
    }
    if ((slot->policy.requirement == UCN_V6_REALTIME_REQUIRED ||
         slot->policy.mode >= UCN_V6_REALTIME_SYNCED_STAMP) &&
        (!opened->hop_authenticated || !opened->endpoint_authorized ||
         (opened->frame.flags & UCN_V6_FLAG_E2E_CONTEXT) == 0U)) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_SECURITY;
        goto rejected;
    }
    if (slot->policy.require_hardware_capture &&
        !evaluated.envelope.sample_capture_hardware) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_UNCERTAINTY;
        goto rejected;
    }
    if (slot->policy.mode == UCN_V6_REALTIME_LOCAL_STAMP) {
        evaluated.accepted = true;
        *view = evaluated;
        return UCN_V6_OK;
    }
    if (evaluated.envelope.source_holdover) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_HOLDOVER;
        goto rejected;
    }
    if (ucn_v6_realtime_get_clock(owner, slot->policy.clock_domain_id,
                                  local_now_us, &clock) != UCN_V6_OK ||
        !clock.available || !clock.uncertainty_known ||
        clock.clock_domain_id != evaluated.envelope.clock_domain_id ||
        clock.domain_generation != evaluated.envelope.domain_generation) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_DOMAIN;
        goto rejected;
    }
    if (uncertainty_class_decode(evaluated.envelope.uncertainty_class,
                                 &source_uncertainty) != UCN_V6_OK) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_UNCERTAINTY;
        goto rejected;
    }
    combined = (uint64_t)source_uncertainty + clock.uncertainty_us;
    if (combined > UINT32_MAX || combined > slot->policy.max_uncertainty_us) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_UNCERTAINTY;
        goto rejected;
    }
    evaluated.combined_uncertainty_us = (uint32_t)combined;
    if (evaluated.envelope.capture_time_us > clock.domain_time_us) {
        if (evaluated.envelope.capture_time_us - clock.domain_time_us >
            combined) {
            evaluated.reason = UCN_V6_REALTIME_REJECT_FUTURE;
            goto rejected;
        }
        age = 0U;
    } else {
        age = clock.domain_time_us - evaluated.envelope.capture_time_us;
    }
    if (UINT64_MAX - age < combined) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_UNCERTAINTY;
        goto rejected;
    }
    evaluated.age_upper_us = age + combined;
    if (evaluated.age_upper_us >= slot->policy.max_age_us) {
        evaluated.reason = UCN_V6_REALTIME_REJECT_EXPIRED;
        goto rejected;
    }
    evaluated.accepted = true;
    *view = evaluated;
    return UCN_V6_OK;

rejected:
    increment_saturated(&owner->stats.rejected_messages);
    *view = evaluated;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_realtime_receive_admit(
    ucn_v6_realtime_owner_t *owner,
    uint64_t local_now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_realtime_receive_view_t *view)
{
    return evaluate_payload(owner, local_now_us, opened, view);
}

ucn_v6_result_t ucn_v6_realtime_execution_admit(
    ucn_v6_realtime_owner_t *owner,
    uint64_t local_now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_realtime_receive_view_t *view,
    const uint8_t **business_payload,
    size_t *business_length)
{
    ucn_v6_realtime_receive_view_t evaluated;
    ucn_v6_result_t result;
    if (view == NULL || business_payload == NULL || business_length == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = evaluate_payload(owner, local_now_us, opened, &evaluated);
    if (result != UCN_V6_OK || !evaluated.accepted) {
        return result != UCN_V6_OK ? result : UCN_V6_ERR_ACCESS;
    }
    *business_payload = opened->frame.payload + evaluated.business_offset;
    *business_length = evaluated.business_length;
    *view = evaluated;
    return UCN_V6_OK;
}

static bool realtime_invalidation_matches(
    const ucn_v6_time_domain_slot_t *slot,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    const ucn_v6_stack_invalidation_t *dependency =
        &slot->route_dependency;
    if (!ucn_v6_stack_invalidation_is_valid(dependency) ||
        dependency->link_id != invalidation->link_id ||
        dependency->link_generation != invalidation->link_generation) {
        return false;
    }
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_LINK) {
        return true;
    }
    if (!realtime_session_equal(&dependency->session,
                                &invalidation->session)) {
        return false;
    }
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_SESSION) {
        return true;
    }
    if (dependency->capability_generation !=
        invalidation->capability_generation) {
        return false;
    }
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_CAPABILITY) {
        return true;
    }
    return dependency->path_id == invalidation->path_id &&
           dependency->path_generation == invalidation->path_generation;
}

ucn_v6_result_t ucn_v6_realtime_apply_invalidation(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    size_t index;
    if (!owner_is_valid(owner) ||
        !ucn_v6_stack_invalidation_is_valid(invalidation)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_api_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_TIME_DOMAINS; ++index) {
        ucn_v6_time_domain_slot_t *slot = &owner->domains[index];
        if (!slot->occupied ||
            !realtime_invalidation_matches(slot, invalidation)) {
            continue;
        }
        if (phase_holds_lock(slot->phase) &&
            owner->stats.locked_domains != 0U) {
            --owner->stats.locked_domains;
        }
        slot->phase = UCN_V6_TIME_UNSYNCED;
        clear_acquisition(slot);
        slot->dependency_invalidated = true;
        slot->dependency_deadline_us = 0U;
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_realtime_copy_view(
    const ucn_v6_realtime_owner_t *owner,
    ucn_v6_realtime_view_t *view)
{
    if (!owner_is_valid(owner) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!owner_gate_probe((ucn_v6_realtime_owner_t *)owner)) {
        return UCN_V6_ERR_STATE;
    }
    *view = owner->stats;
    return UCN_V6_OK;
}
