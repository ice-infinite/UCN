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
           owner->initialized && owner->canary == UCN_V6_REALTIME_OWNER_CANARY;
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
    if (value == NULL || value->mode <= UCN_V6_REALTIME_NONE ||
        value->mode > UCN_V6_REALTIME_DEADLINE ||
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

static bool policy_is_valid(const ucn_v6_realtime_endpoint_policy_t *policy)
{
    if (policy == NULL || policy->destination_endpoint == 0U ||
        policy->mode > UCN_V6_REALTIME_DEADLINE ||
        policy->requirement > UCN_V6_REALTIME_REQUIRED) {
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
           config->route_generation != 0U && config->path_id != 0U &&
           config->path_generation != 0U &&
           config->lock_sample_count != 0U &&
           config->lock_sample_count <= UCN_V6_REALTIME_SAMPLE_WINDOW &&
           config->sync_timeout_us != 0U && config->max_holdover_us != 0U &&
           UINT64_MAX - config->sync_timeout_us >= config->max_holdover_us &&
           config->max_offset_jump_us != 0U &&
           config->oscillator_uncertainty_ppb != 0U;
}

ucn_v6_result_t ucn_v6_realtime_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_realtime_generation_store_ops_t *generation_store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_realtime_owner_t **owner)
{
    ucn_v6_realtime_owner_t initialized;
    if (owner == NULL || generation_store == NULL ||
        generation_store->load_high_water == NULL ||
        generation_store->reserve_high_water == NULL ||
        callback_gate == NULL ||
        ucn_v6_callback_gate_is_active(callback_gate) ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_REALTIME_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK) {
        return UCN_V6_ERR_CONFIG;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_V6_REALTIME_OWNER_MAGIC;
    initialized.schema = UCN_V6_REALTIME_OWNER_SCHEMA;
    initialized.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
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
    if (!owner_is_valid(owner) || owner->io_active ||
        !policy_is_valid(policy)) {
        return UCN_V6_ERR_ARGUMENT;
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

ucn_v6_result_t ucn_v6_realtime_bind_domain(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_time_domain_config_t *config,
    const ucn_v6_path_capability_t *fixed_path,
    const ucn_v6_cached_peer_capability_t *master_capability)
{
    ucn_v6_time_domain_slot_t replacement;
    ucn_v6_time_domain_slot_t *slot;
    uint32_t high_water = 0U;
    ucn_v6_result_t result;
    size_t index;
    if (!owner_is_valid(owner) || owner->io_active ||
        !domain_config_is_valid(config) || fixed_path == NULL ||
        master_capability == NULL || !fixed_path->valid ||
        !fixed_path->immutable_for_realtime ||
        (fixed_path->feature_bits & UCN_V6_FEATURE_REALTIME) == 0U ||
        (fixed_path->timestamp_capability_bits &
         (UCN_V6_TIMESTAMP_RX_HARDWARE | UCN_V6_TIMESTAMP_TX_HARDWARE)) !=
            (UCN_V6_TIMESTAMP_RX_HARDWARE | UCN_V6_TIMESTAMP_TX_HARDWARE) ||
        fixed_path->timestamp_uncertainty_us == 0U ||
        !master_capability->valid ||
        !principal_equal(&master_capability->principal,
                         &config->master_principal) ||
        !ucn_v6_binding_key_equal(&master_capability->binding,
                                  &config->master_binding) ||
        master_capability->session_generation !=
            config->master_session_generation ||
        (master_capability->record.peer.feature_bits &
         UCN_V6_FEATURE_REALTIME) == 0U ||
        (master_capability->record.peer.realtime_mode_bits &
         UCN_V6_REALTIME_MODE_SYNCED) == 0U ||
        master_capability->record.peer.clock_domain_id !=
            config->clock_domain_id ||
        master_capability->record.peer.clock_domain_generation !=
            config->domain_generation ||
        !principal_equal(&fixed_path->destination_principal,
                         &config->master_principal) ||
        !ucn_v6_binding_key_equal(&fixed_path->destination_binding,
                                  &config->master_binding) ||
        fixed_path->session_generation != config->master_session_generation ||
        fixed_path->route_generation != config->route_generation ||
        fixed_path->path_id != config->path_id ||
        fixed_path->path_generation != config->path_generation) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (ucn_v6_callback_gate_try_enter(owner->callback_gate, owner) !=
        UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    owner->io_active = true;
    result = owner->generation_store.load_high_water(
        owner->generation_store.context, &config->master_principal,
        config->clock_domain_id, &high_water);
    if (result != UCN_V6_OK && result != UCN_V6_ERR_NOT_FOUND) {
        owner->io_active = false;
        (void)ucn_v6_callback_gate_leave(owner->callback_gate, owner);
        return UCN_V6_ERR_STATE;
    }
    if (result == UCN_V6_OK && high_water > config->domain_generation) {
        owner->io_active = false;
        (void)ucn_v6_callback_gate_leave(owner->callback_gate, owner);
        return UCN_V6_ERR_REPLAY;
    }
    if (result == UCN_V6_ERR_NOT_FOUND ||
        high_water < config->domain_generation) {
        result = owner->generation_store.reserve_high_water(
            owner->generation_store.context, &config->master_principal,
            config->clock_domain_id, config->domain_generation);
        if (result != UCN_V6_OK) {
            owner->io_active = false;
            (void)ucn_v6_callback_gate_leave(owner->callback_gate, owner);
            return UCN_V6_ERR_STATE;
        }
        high_water = 0U;
        result = owner->generation_store.load_high_water(
            owner->generation_store.context, &config->master_principal,
            config->clock_domain_id, &high_water);
        if (result != UCN_V6_OK ||
            high_water != config->domain_generation) {
            owner->io_active = false;
            (void)ucn_v6_callback_gate_leave(owner->callback_gate, owner);
            return UCN_V6_ERR_STATE;
        }
    }
    owner->io_active = false;
    if (ucn_v6_callback_gate_leave(owner->callback_gate, owner) !=
        UCN_V6_OK) {
        owner->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    slot = find_domain(owner, config->clock_domain_id);
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
    if (slot->occupied &&
        config->domain_generation <= slot->config.domain_generation) {
        return UCN_V6_ERR_REPLAY;
    }
    memset(&replacement, 0, sizeof(replacement));
    replacement.occupied = true;
    replacement.config = *config;
    replacement.phase = UCN_V6_TIME_ACQUIRING;
    if (!slot->occupied) {
        ++owner->stats.domains;
    } else if (slot->phase == UCN_V6_TIME_LOCKED &&
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
    const ucn_v6_time_sync_sample_t *sample)
{
    ucn_v6_time_domain_slot_t *slot;
    uint32_t uncertainty;
    int64_t next_offset;
    int64_t offset_delta;
    uint64_t candidate_time;
    if (!owner_is_valid(owner) || owner->io_active || opened == NULL ||
        sample == NULL || sample->clock_domain_id == 0U ||
        sample->domain_generation == 0U || sample->local_sample_us == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_domain(owner, sample->clock_domain_id);
    if (slot == NULL || slot->phase == UCN_V6_TIME_FAULT ||
        sample->domain_generation != slot->config.domain_generation ||
        !opened->hop_authenticated || !opened->endpoint_authorized ||
        (opened->frame.flags & UCN_V6_FLAG_E2E_CONTEXT) == 0U ||
        !principal_equal(&opened->authenticated_principal,
                         &slot->config.master_principal) ||
        opened->frame.source_address != slot->config.master_binding.node_address ||
        opened->frame.source_binding_generation !=
            slot->config.master_binding.binding_generation ||
        opened->frame.session_generation !=
            slot->config.master_session_generation ||
        (opened->frame.flags & (UCN_V6_FLAG_ROUTE_CONTEXT |
                                UCN_V6_FLAG_PATH_CONTEXT)) !=
            (UCN_V6_FLAG_ROUTE_CONTEXT | UCN_V6_FLAG_PATH_CONTEXT) ||
        opened->frame.route_generation != slot->config.route_generation ||
        opened->frame.path.path_id != slot->config.path_id ||
        opened->frame.path.path_generation != slot->config.path_generation ||
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
        slot->phase = UCN_V6_TIME_FAULT;
        owner->stats.faulted = true;
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
        slot->phase = UCN_V6_TIME_FAULT;
        owner->stats.faulted = true;
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
    if (!owner_is_valid(owner) || owner->io_active || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_domain(owner, clock_domain_id);
    if (slot == NULL || !slot->has_sample_high_water ||
        local_now_us < slot->last_sample_local_us ||
        slot->phase == UCN_V6_TIME_FAULT) {
        return UCN_V6_ERR_STATE;
    }
    elapsed = local_now_us - slot->last_sample_local_us;
    if (elapsed >= slot->config.sync_timeout_us +
                   slot->config.max_holdover_us) {
        if ((slot->phase == UCN_V6_TIME_LOCKED ||
             slot->phase == UCN_V6_TIME_HOLDOVER) &&
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
        slot->phase = UCN_V6_TIME_FAULT;
        owner->stats.faulted = true;
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
        slot->phase = UCN_V6_TIME_FAULT;
        owner->stats.faulted = true;
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
    if (!owner_is_valid(owner) || owner->io_active ||
        destination_endpoint == 0U || local_capture_us == 0U ||
        business_payload == NULL || business_length == 0U ||
        output == NULL || result == NULL) {
        return UCN_V6_ERR_ARGUMENT;
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

ucn_v6_result_t ucn_v6_realtime_copy_view(
    const ucn_v6_realtime_owner_t *owner,
    ucn_v6_realtime_view_t *view)
{
    if (!owner_is_valid(owner) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *view = owner->stats;
    return UCN_V6_OK;
}
