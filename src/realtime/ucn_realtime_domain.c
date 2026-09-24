#include "internal/ucn_realtime.h"

#include "internal/ucn_checked.h"
#include "internal/ucn_digest.h"

#include <limits.h>
#include <string.h>

#define DOMAIN_RECORD_VERSION UINT16_C(1)
#define DOMAIN_HANDLE_KIND UCN_OBJECT_KIND_TIME_DOMAIN

static void write_be16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void write_be32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void write_be64(uint8_t *output, uint64_t value)
{
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static uint16_t read_be16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static uint32_t read_be32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | input[3];
}

static uint64_t read_be64(const uint8_t *input)
{
    uint64_t value = 0U;
    uint8_t index;
    for (index = 0U; index < 8U; ++index) value = (value << 8) | input[index];
    return value;
}

static bool nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) if (bytes[index] != 0U) return true;
    return false;
}

static bool path_valid(const ucn_i_realtime_path_facts_t *path)
{
    return path != NULL && path->route_causal_id != 0U &&
           path->route_generation != 0U && path->session_generation != 0U &&
           path->capability_generation != 0U &&
           path->forward_link_generation != 0U &&
           path->reverse_link_generation != 0U && path->forward_link_id != 0U &&
           path->reverse_link_id != 0U && nonzero(path->capability_digest, 16U) &&
           path->authenticated == 1U && path->frozen == 1U &&
           path->directional == 1U && path->asymmetry_known <= 1U &&
           (path->asymmetry_known == 0U || path->max_asymmetry_us != 0U);
}

static bool path_equal(const ucn_i_realtime_path_facts_t *left,
                       const ucn_i_realtime_path_facts_t *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static bool config_valid(const ucn_i_realtime_domain_config_t *config)
{
    uint32_t uncertainty;
    return config != NULL && config->clock_domain_id != 0U &&
           config->clock_domain_id <= UCN_I_REALTIME_DOMAIN_ID_MAX &&
           config->domain_generation != 0U &&
           config->domain_generation <= UCN_I_REALTIME_DOMAIN_GENERATION_MAX &&
           config->lock_sample_count != 0U &&
           config->lock_sample_count <= UCN_I_REALTIME_SAMPLE_WINDOW &&
           config->sync_timeout_us != 0U && config->max_holdover_us != 0U &&
           config->oscillator_uncertainty_ppb != 0U &&
           config->max_offset_jump_us != 0U && config->reserved_zero == 0U &&
           path_valid(&config->path) && config->path.asymmetry_known == 1U &&
           ucn_i_realtime_uncertainty_aggregate(&config->base_uncertainty,
                                                &uncertainty) == UCN_OK;
}

static ucn_result_t owner_lock(ucn_i_realtime_owner_t *owner)
{
    if (owner == NULL || owner->magic != UCN_I_REALTIME_MAGIC ||
        owner->schema != UCN_I_REALTIME_SCHEMA) return UCN_ERR_STATE;
    return owner->state_lock.enter(owner->state_lock.context);
}

static void owner_unlock(ucn_i_realtime_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static ucn_i_realtime_domain_slot_t *find_domain(
    ucn_i_realtime_owner_t *owner, uint16_t domain_id, uint16_t *index_out)
{
    uint16_t index;
    for (index = 0U; index < UCN_I_REALTIME_DOMAIN_COUNT; ++index) {
        if (owner->domains[index].occupied != 0U &&
            owner->domains[index].config.clock_domain_id == domain_id) {
            if (index_out != NULL) *index_out = index;
            return &owner->domains[index];
        }
    }
    return NULL;
}

static ucn_handle_t domain_handle(const ucn_i_realtime_owner_t *owner,
                                  uint16_t index,
                                  uint16_t generation)
{
    ucn_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = index;
    handle.generation = generation;
    handle.object_kind = DOMAIN_HANDLE_KIND;
    return handle;
}

static ucn_i_realtime_domain_slot_t *domain_from_handle(
    ucn_i_realtime_owner_t *owner, ucn_handle_t handle)
{
    ucn_i_realtime_domain_slot_t *slot;
    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_REALTIME_DOMAIN_COUNT ||
        handle.generation == 0U || handle.object_kind != DOMAIN_HANDLE_KIND) {
        return NULL;
    }
    slot = &owner->domains[handle.slot];
    if (slot->occupied == 0U ||
        slot->handle_generation != handle.generation) {
        return NULL;
    }
    return slot;
}

ucn_result_t ucn_i_realtime_domain_record_encode(
    const ucn_i_realtime_domain_config_t *config,
    uint8_t body_out[UCN_I_REALTIME_DOMAIN_RECORD_BYTES])
{
    uint8_t body[UCN_I_REALTIME_DOMAIN_RECORD_BYTES];
    const ucn_i_realtime_uncertainty_t *u;
    const ucn_i_realtime_path_facts_t *p;
    uint8_t *cursor;

    if (!config_valid(config) || body_out == NULL ||
        ucn_i_ranges_overlap(config, sizeof(*config), body_out,
                             UCN_I_REALTIME_DOMAIN_RECORD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(body, 0, sizeof(body));
    write_be16(&body[0], DOMAIN_RECORD_VERSION);
    write_be16(&body[2], config->clock_domain_id);
    write_be32(&body[4], config->domain_generation);
    body[8] = config->lock_sample_count;
    write_be64(&body[12], config->sync_timeout_us);
    write_be64(&body[20], config->max_holdover_us);
    write_be32(&body[28], config->oscillator_uncertainty_ppb);
    write_be32(&body[32], config->max_offset_jump_us);
    u = &config->base_uncertainty;
    cursor = &body[36];
    write_be32(cursor, u->timer_resolution_bound_us); cursor += 4;
    write_be32(cursor, u->link_timestamp_capture_bound_us); cursor += 4;
    write_be32(cursor, u->filter_residual_bound_us); cursor += 4;
    write_be32(cursor, u->arithmetic_rounding_bound_us); cursor += 4;
    write_be32(cursor, u->sample_capture_bound_us); cursor += 4;
    write_be32(cursor, u->path_asymmetry_bound_us);
    body[60] = u->known_mask;
    p = &config->path;
    write_be64(&body[64], p->route_causal_id);
    write_be32(&body[72], p->route_generation);
    write_be32(&body[76], p->session_generation);
    write_be32(&body[80], p->capability_generation);
    write_be32(&body[84], p->forward_link_generation);
    write_be32(&body[88], p->reverse_link_generation);
    write_be16(&body[92], p->forward_link_id);
    write_be16(&body[94], p->reverse_link_id);
    body[96] = p->authenticated;
    body[97] = p->frozen;
    body[98] = p->directional;
    body[99] = p->asymmetry_known;
    write_be32(&body[100], p->max_asymmetry_us);
    memcpy(&body[104], p->capability_digest, 16U);
    memcpy(body_out, body, sizeof(body));
    return UCN_OK;
}

static ucn_result_t record_decode(const uint8_t *body, size_t bytes,
                                  ucn_i_realtime_domain_config_t *config_out)
{
    ucn_i_realtime_domain_config_t config;
    ucn_i_realtime_uncertainty_t *u;
    ucn_i_realtime_path_facts_t *p;
    size_t index;
    const uint8_t *cursor;
    if (body == NULL || config_out == NULL ||
        bytes != UCN_I_REALTIME_DOMAIN_RECORD_BYTES ||
        read_be16(&body[0]) != DOMAIN_RECORD_VERSION) return UCN_ERR_MALFORMED;
    for (index = 9U; index < 12U; ++index) if (body[index] != 0U) return UCN_ERR_MALFORMED;
    for (index = 61U; index < 64U; ++index) if (body[index] != 0U) return UCN_ERR_MALFORMED;
    for (index = 120U; index < bytes; ++index) if (body[index] != 0U) return UCN_ERR_MALFORMED;
    memset(&config, 0, sizeof(config));
    config.clock_domain_id = read_be16(&body[2]);
    config.domain_generation = read_be32(&body[4]);
    config.lock_sample_count = body[8];
    config.sync_timeout_us = read_be64(&body[12]);
    config.max_holdover_us = read_be64(&body[20]);
    config.oscillator_uncertainty_ppb = read_be32(&body[28]);
    config.max_offset_jump_us = read_be32(&body[32]);
    u = &config.base_uncertainty;
    cursor = &body[36];
    u->timer_resolution_bound_us = read_be32(cursor); cursor += 4;
    u->link_timestamp_capture_bound_us = read_be32(cursor); cursor += 4;
    u->filter_residual_bound_us = read_be32(cursor); cursor += 4;
    u->arithmetic_rounding_bound_us = read_be32(cursor); cursor += 4;
    u->sample_capture_bound_us = read_be32(cursor); cursor += 4;
    u->path_asymmetry_bound_us = read_be32(cursor);
    u->known_mask = body[60];
    p = &config.path;
    p->route_causal_id = read_be64(&body[64]);
    p->route_generation = read_be32(&body[72]);
    p->session_generation = read_be32(&body[76]);
    p->capability_generation = read_be32(&body[80]);
    p->forward_link_generation = read_be32(&body[84]);
    p->reverse_link_generation = read_be32(&body[88]);
    p->forward_link_id = read_be16(&body[92]);
    p->reverse_link_id = read_be16(&body[94]);
    p->authenticated = body[96]; p->frozen = body[97];
    p->directional = body[98]; p->asymmetry_known = body[99];
    p->max_asymmetry_us = read_be32(&body[100]);
    memcpy(p->capability_digest, &body[104], 16U);
    if (!config_valid(&config)) return UCN_ERR_MALFORMED;
    *config_out = config;
    return UCN_OK;
}

static ucn_result_t body_digest(const uint8_t *body, uint8_t digest[16])
{
    ucn_i_sha256_workspace_t workspace;
    memset(&workspace, 0, sizeof(workspace));
    return ucn_i_sha256_128(body, UCN_I_REALTIME_DOMAIN_RECORD_BYTES,
                            digest, &workspace);
}

static bool durability_valid(const ucn_i_realtime_durability_t *value)
{
    return value != NULL && value->domain_id != 0U &&
           value->foundation_transaction_id != 0U &&
           value->expected_record_generation != UINT64_MAX &&
           value->absolute_deadline_us != 0U &&
           value->persistence_domain_generation != 0U &&
           value->schema_id == UCN_I_REALTIME_DOMAIN_SCHEMA_ID &&
           value->schema_version == UCN_I_REALTIME_DOMAIN_RECORD_SCHEMA &&
           value->volatile_continuation.runtime_instance != 0U &&
           value->volatile_continuation.owner_instance != 0U &&
           value->volatile_continuation.generation != 0U &&
           value->volatile_continuation.object_kind ==
               UCN_OBJECT_KIND_TIME_DOMAIN &&
           value->volatile_continuation.reserved_zero == 0U;
}

ucn_result_t ucn_i_realtime_domain_prepare(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_domain_config_t *config,
    const ucn_i_realtime_durability_t *durability,
    ucn_handle_t *domain_out,
    ucn_i_realtime_requirement_t *requirement_out)
{
    ucn_i_realtime_requirement_t requirement;
    ucn_i_realtime_domain_slot_t *slot;
    ucn_handle_t handle;
    uint16_t index;
    ucn_result_t result;

    if (owner == NULL || !config_valid(config) ||
        !durability_valid(durability) ||
        domain_out == NULL || requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), domain_out,
                             sizeof(*domain_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), domain_out,
                             sizeof(*domain_out)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability), domain_out,
                             sizeof(*domain_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(domain_out, sizeof(*domain_out), requirement_out,
                             sizeof(*requirement_out))) return UCN_ERR_ARGUMENT;
    result = owner_lock(owner); if (result != UCN_OK) return result;
    if (durability->volatile_continuation.runtime_instance !=
            owner->runtime_instance ||
        durability->volatile_continuation.owner_instance !=
            owner->owner_instance) {
        owner_unlock(owner);
        return UCN_ERR_ARGUMENT;
    }
    slot = find_domain(owner, config->clock_domain_id, &index);
    if (slot != NULL) {
        if (slot->phase == UCN_I_REALTIME_GENERATION_PENDING ||
            slot->phase == UCN_I_REALTIME_FAULT ||
            slot->config.domain_generation == UINT32_MAX ||
            config->domain_generation != slot->config.domain_generation + 1U) {
            owner_unlock(owner); return UCN_ERR_STATE;
        }
    } else {
        for (index = 0U; index < UCN_I_REALTIME_DOMAIN_COUNT; ++index)
            if (owner->domains[index].occupied == 0U) break;
        if (index == UCN_I_REALTIME_DOMAIN_COUNT) {
            owner_unlock(owner); return UCN_ERR_NO_SPACE;
        }
        slot = &owner->domains[index];
        memset(slot, 0, sizeof(*slot));
        slot->occupied = 1U;
    }
    if (slot->handle_generation == UINT16_MAX) {
        owner_unlock(owner); return UCN_ERR_EXHAUSTED;
    }
    memset(&requirement, 0, sizeof(requirement));
    requirement.durability = *durability;
    requirement.runtime_instance = owner->runtime_instance;
    requirement.body_bytes = UCN_I_REALTIME_DOMAIN_RECORD_BYTES;
    requirement.caller_owner_instance = owner->owner_instance;
    requirement.operation_kind = UCN_I_REALTIME_DOMAIN_PERSIST_KIND;
    result = ucn_i_realtime_domain_record_encode(config, requirement.body);
    if (result == UCN_OK) result = body_digest(requirement.body,
                                               requirement.canonical_body_digest);
    if (result != UCN_OK) { owner_unlock(owner); return result; }
    memcpy(requirement.expected_body_digest, slot->current_body_digest, 16U);
    slot->pending_config = *config;
    ++slot->handle_generation;
    slot->pending_domain_id = durability->domain_id;
    slot->pending_foundation_transaction_id =
        durability->foundation_transaction_id;
    slot->pending_expected_record_generation =
        durability->expected_record_generation;
    slot->pending_absolute_deadline_us = durability->absolute_deadline_us;
    slot->pending_persistence_domain_generation =
        durability->persistence_domain_generation;
    slot->phase = UCN_I_REALTIME_GENERATION_PENDING;
    slot->persistence_bound = 0U;
    memset(&slot->persistence_handle, 0, sizeof(slot->persistence_handle));
    memset(slot->pending_body_digest, 0, 16U);
    handle = domain_handle(owner, index, slot->handle_generation);
    *domain_out = handle;
    *requirement_out = requirement;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_domain_bind_persistence(
    ucn_i_realtime_owner_t *owner, ucn_handle_t domain,
    ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_REALTIME_DIGEST_BYTES])
{
    ucn_i_realtime_domain_slot_t *slot;
    ucn_result_t result = owner_lock(owner); if (result != UCN_OK) return result;
    slot = domain_from_handle(owner, domain);
    if (slot == NULL || slot->phase != UCN_I_REALTIME_GENERATION_PENDING ||
        slot->persistence_bound != 0U || persistence_handle.object_kind !=
            UCN_OBJECT_KIND_PERSISTENCE || persistence_handle.reserved_zero != 0U ||
        published_body_digest == NULL ||
        !nonzero(published_body_digest, UCN_I_REALTIME_DIGEST_BYTES)) {
        owner_unlock(owner); return UCN_ERR_STATE;
    }
    slot->persistence_handle = persistence_handle;
    memcpy(slot->pending_body_digest, published_body_digest, 16U);
    slot->persistence_bound = 1U;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_domain_accept_proof(
    ucn_i_realtime_owner_t *owner, ucn_handle_t domain,
    const ucn_i_realtime_proof_t *proof, uint64_t now_us)
{
    ucn_i_realtime_domain_slot_t *slot;
    ucn_result_t result = owner_lock(owner); if (result != UCN_OK) return result;
    slot = domain_from_handle(owner, domain);
    if (slot == NULL || proof == NULL || slot->persistence_bound == 0U ||
        slot->phase != UCN_I_REALTIME_GENERATION_PENDING ||
        memcmp(&proof->persistence_handle, &slot->persistence_handle,
               sizeof(proof->persistence_handle)) != 0 ||
        proof->runtime_instance != owner->runtime_instance ||
        proof->caller_owner_instance != owner->owner_instance ||
        proof->persistence_domain_generation !=
            slot->pending_persistence_domain_generation ||
        proof->persistence_owner_instance !=
            slot->persistence_handle.owner_instance ||
        proof->domain_id != slot->pending_domain_id ||
        proof->foundation_transaction_id !=
            slot->pending_foundation_transaction_id ||
        slot->pending_expected_record_generation == UINT64_MAX ||
        proof->record_generation !=
            slot->pending_expected_record_generation + 1U ||
        proof->witness_generation != proof->record_generation ||
        proof->body_bytes != UCN_I_REALTIME_DOMAIN_RECORD_BYTES ||
        proof->schema_id != UCN_I_REALTIME_DOMAIN_SCHEMA_ID ||
        proof->schema_version != UCN_I_REALTIME_DOMAIN_RECORD_SCHEMA ||
        proof->operation_kind != UCN_I_REALTIME_DOMAIN_PERSIST_KIND ||
        proof->reserved_zero != 0U ||
        now_us >= slot->pending_absolute_deadline_us ||
        memcmp(proof->body_digest, slot->pending_body_digest, 16U) != 0) {
        owner_unlock(owner); return UCN_ERR_STATE;
    }
    slot->config = slot->pending_config;
    memset(&slot->pending_config, 0, sizeof(slot->pending_config));
    memset(slot->samples, 0, sizeof(slot->samples));
    slot->sample_count = 0U; slot->sample_cursor = 0U;
    slot->consecutive_samples = 0U; slot->offset_us = 0;
    slot->last_sample_local_us = 0U;
    slot->last_output_local_us = 0U; slot->last_output_domain_us = 0U;
    slot->has_output_high_water = 0U;
    slot->uncertainty_us = 0U;
    slot->phase = UCN_I_REALTIME_UNSYNCED;
    slot->persistence_bound = 0U;
    slot->pending_domain_id = 0U;
    slot->pending_foundation_transaction_id = 0U;
    slot->pending_expected_record_generation = 0U;
    slot->pending_absolute_deadline_us = 0U;
    slot->pending_persistence_domain_generation = 0U;
    memcpy(slot->current_body_digest, proof->body_digest, 16U);
    memset(slot->pending_body_digest, 0, 16U);
    memset(&slot->persistence_handle, 0, sizeof(slot->persistence_handle));
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_domain_import(
    ucn_i_realtime_owner_t *owner, const uint8_t *body,
    size_t body_bytes, const ucn_i_realtime_durability_t *durability,
    const uint8_t published_body_digest[UCN_I_REALTIME_DIGEST_BYTES],
    ucn_handle_t *domain_out)
{
    ucn_i_realtime_domain_config_t config;
    ucn_i_realtime_domain_slot_t *slot;
    uint8_t digest[16];
    uint16_t index;
    ucn_result_t result;
    if (!durability_valid(durability) || published_body_digest == NULL ||
        domain_out == NULL || record_decode(body, body_bytes, &config) != UCN_OK ||
        body_digest(body, digest) != UCN_OK ||
        memcmp(digest, published_body_digest, 16U) != 0) return UCN_ERR_MALFORMED;
    result = owner_lock(owner); if (result != UCN_OK) return result;
    if (find_domain(owner, config.clock_domain_id, NULL) != NULL) {
        owner_unlock(owner); return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_REALTIME_DOMAIN_COUNT; ++index)
        if (owner->domains[index].occupied == 0U) break;
    if (index == UCN_I_REALTIME_DOMAIN_COUNT) {
        owner_unlock(owner); return UCN_ERR_NO_SPACE;
    }
    slot = &owner->domains[index]; memset(slot, 0, sizeof(*slot));
    slot->occupied = 1U; slot->config = config;
    slot->phase = UCN_I_REALTIME_UNSYNCED;
    memcpy(slot->current_body_digest, digest, 16U);
    slot->handle_generation = 1U;
    *domain_out = domain_handle(owner, index, slot->handle_generation);
    owner_unlock(owner);
    return UCN_OK;
}

static int64_t median(const int64_t *values, uint8_t count)
{
    int64_t sorted[UCN_I_REALTIME_SAMPLE_WINDOW];
    uint8_t left, right;
    memcpy(sorted, values, (size_t)count * sizeof(values[0]));
    for (left = 1U; left < count; ++left) {
        int64_t value = sorted[left];
        right = left;
        while (right > 0U && sorted[right - 1U] > value) {
            sorted[right] = sorted[right - 1U]; --right;
        }
        sorted[right] = value;
    }
    return sorted[count / 2U];
}

static bool apply_offset(uint64_t local, int64_t offset, uint64_t *domain_out)
{
    if (offset >= 0) {
        uint64_t positive = (uint64_t)offset;
        if (UINT64_MAX - local < positive) return false;
        *domain_out = local + positive;
    } else {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1U;
        if (local < magnitude) return false;
        *domain_out = local - magnitude;
    }
    return true;
}

ucn_result_t ucn_i_realtime_p_accept_sample_locked(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_sync_sample_t *sample)
{
    ucn_i_realtime_domain_slot_t *slot;
    int64_t candidate_offset;
    uint64_t candidate_time;
    if (sample == NULL || sample->clock_domain_id == 0U ||
        sample->sample_local_us == 0U || sample->uncertainty_us == 0U ||
        sample->reserved_zero != 0U || sample->valid_sync_sample > 1U ||
        !path_valid(&sample->path)) return UCN_ERR_ARGUMENT;
    slot = find_domain(owner, sample->clock_domain_id, NULL);
    if (slot == NULL || slot->phase == UCN_I_REALTIME_FAULT ||
        slot->phase == UCN_I_REALTIME_GENERATION_PENDING ||
        !path_equal(&slot->config.path, &sample->path)) {
        return UCN_ERR_STATE;
    }
    if (sample->valid_sync_sample == 0U || sample->path.asymmetry_known == 0U) {
        return UCN_ERR_POLICY;
    }
    if (slot->last_sample_local_us != 0U &&
        sample->sample_local_us <= slot->last_sample_local_us) {
        return UCN_ERR_REPLAY;
    }
    if (slot->sample_count != 0U) {
        uint64_t magnitude;
        if ((sample->offset_us >= 0) == (slot->offset_us >= 0)) {
            magnitude = sample->offset_us >= slot->offset_us ?
                (uint64_t)sample->offset_us - (uint64_t)slot->offset_us :
                (uint64_t)slot->offset_us - (uint64_t)sample->offset_us;
        } else {
            uint64_t left = sample->offset_us >= 0 ?
                (uint64_t)sample->offset_us :
                (uint64_t)(-(sample->offset_us + 1)) + 1U;
            uint64_t right = slot->offset_us >= 0 ?
                (uint64_t)slot->offset_us :
                (uint64_t)(-(slot->offset_us + 1)) + 1U;
            magnitude = UINT64_MAX - left < right ? UINT64_MAX : left + right;
        }
        if (magnitude > slot->config.max_offset_jump_us) {
            slot->phase = UCN_I_REALTIME_FAULT;
            return UCN_ERR_STATE;
        }
    }
    slot->samples[slot->sample_cursor] = sample->offset_us;
    slot->sample_cursor = (uint8_t)((slot->sample_cursor + 1U) %
                                    UCN_I_REALTIME_SAMPLE_WINDOW);
    if (slot->sample_count < UCN_I_REALTIME_SAMPLE_WINDOW) ++slot->sample_count;
    candidate_offset = median(slot->samples, slot->sample_count);
    if (!apply_offset(sample->sample_local_us, candidate_offset, &candidate_time) ||
        (slot->has_output_high_water != 0U &&
         candidate_time < slot->last_output_domain_us)) {
        slot->phase = UCN_I_REALTIME_FAULT;
        return UCN_ERR_STATE;
    }
    slot->offset_us = candidate_offset;
    slot->last_sample_local_us = sample->sample_local_us;
    slot->uncertainty_us = sample->uncertainty_us;
    if (slot->consecutive_samples < UINT8_MAX) ++slot->consecutive_samples;
    slot->phase = slot->consecutive_samples >= slot->config.lock_sample_count ?
                      UCN_I_REALTIME_LOCKED : UCN_I_REALTIME_ACQUIRING;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_domain_accept_sample(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_sync_sample_t *sample)
{
    if (owner == NULL || sample == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), sample, sizeof(*sample))) {
        return UCN_ERR_ARGUMENT;
    }
    ucn_result_t result = owner_lock(owner);
    if (result != UCN_OK) return result;
    result = ucn_i_realtime_p_accept_sample_locked(owner, sample);
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_realtime_step(ucn_i_realtime_owner_t *owner,
                                  uint64_t now_us)
{
    uint16_t index;
    ucn_result_t result = owner_lock(owner); if (result != UCN_OK) return result;
    result = ucn_i_realtime_p_sync_step_locked(owner, now_us);
    if (result != UCN_OK) { owner_unlock(owner); return result; }
    for (index = 0U; index < UCN_I_REALTIME_DOMAIN_COUNT; ++index) {
        ucn_i_realtime_domain_slot_t *slot = &owner->domains[index];
        uint64_t age;
        if (slot->occupied == 0U || slot->last_sample_local_us == 0U) continue;
        if (now_us < slot->last_sample_local_us) {
            slot->phase = UCN_I_REALTIME_FAULT;
            continue;
        }
        age = now_us - slot->last_sample_local_us;
        if (slot->phase == UCN_I_REALTIME_LOCKED &&
            age >= slot->config.sync_timeout_us) slot->phase = UCN_I_REALTIME_HOLDOVER;
        if ((slot->phase == UCN_I_REALTIME_HOLDOVER ||
             slot->phase == UCN_I_REALTIME_ACQUIRING) &&
            age >= slot->config.max_holdover_us) {
            memset(slot->samples, 0, sizeof(slot->samples));
            slot->sample_count = 0U; slot->sample_cursor = 0U;
            slot->consecutive_samples = 0U; slot->offset_us = 0;
            slot->uncertainty_us = 0U; slot->last_sample_local_us = 0U;
            slot->phase = UCN_I_REALTIME_UNSYNCED;
            /* Same-generation output high-water is intentionally retained. */
        }
    }
    owner_unlock(owner); return UCN_OK;
}

ucn_result_t ucn_i_realtime_domain_get_clock(
    ucn_i_realtime_owner_t *owner, uint16_t clock_domain_id,
    uint64_t local_now_us, ucn_i_realtime_clock_view_t *view_out)
{
    ucn_i_realtime_clock_view_t view;
    ucn_i_realtime_domain_slot_t *slot;
    uint64_t domain_time;
    uint64_t holdover_age = 0U;
    uint64_t extra;
    ucn_result_t result;
    if (owner == NULL || view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) return UCN_ERR_ARGUMENT;
    result = owner_lock(owner); if (result != UCN_OK) return result;
    slot = find_domain(owner, clock_domain_id, NULL);
    if (slot == NULL || (slot->phase != UCN_I_REALTIME_LOCKED &&
                         slot->phase != UCN_I_REALTIME_HOLDOVER) ||
        local_now_us < slot->last_sample_local_us ||
        !apply_offset(local_now_us, slot->offset_us, &domain_time)) {
        owner_unlock(owner); return UCN_ERR_STATE;
    }
    if (slot->has_output_high_water != 0U &&
        domain_time < slot->last_output_domain_us) {
        slot->phase = UCN_I_REALTIME_FAULT;
        owner_unlock(owner); return UCN_ERR_STATE;
    }
    memset(&view, 0, sizeof(view));
    view.available = 1U; view.phase = slot->phase;
    view.clock_domain_id = slot->config.clock_domain_id;
    view.domain_generation = slot->config.domain_generation;
    view.domain_time_us = domain_time; view.uncertainty_us = slot->uncertainty_us;
    if (slot->phase == UCN_I_REALTIME_HOLDOVER) {
        holdover_age = local_now_us - slot->last_sample_local_us;
        if ((holdover_age / UINT64_C(1000000000)) >
            UINT64_MAX / slot->config.oscillator_uncertainty_ppb) {
            owner_unlock(owner); return UCN_ERR_EXHAUSTED;
        }
        extra = (holdover_age / UINT64_C(1000000000)) *
                    slot->config.oscillator_uncertainty_ppb;
        if (holdover_age % UINT64_C(1000000000) != 0U) {
            uint64_t remainder = holdover_age % UINT64_C(1000000000);
            uint64_t fractional =
                (remainder * slot->config.oscillator_uncertainty_ppb +
                 UINT64_C(999999999)) / UINT64_C(1000000000);
            if (UINT64_MAX - extra < fractional) {
                owner_unlock(owner); return UCN_ERR_EXHAUSTED;
            }
            extra += fractional;
        }
        if (extra > UINT32_MAX || UINT32_MAX - view.uncertainty_us < extra) {
            owner_unlock(owner); return UCN_ERR_EXHAUSTED;
        }
        view.uncertainty_us += (uint32_t)extra;
        view.holdover = 1U; view.holdover_age_us = holdover_age;
    }
    slot->has_output_high_water = 1U;
    slot->last_output_local_us = local_now_us;
    slot->last_output_domain_us = domain_time;
    *view_out = view;
    owner_unlock(owner); return UCN_OK;
}
