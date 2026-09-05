#include "../internal/ucn_v6_qos_private.h"

#include <limits.h>
#include <string.h>

#define UCN_V6_METRIC_SCHEMA UINT16_C(1)

typedef char ucn_v6_metric_owner_storage_must_fit[
    sizeof(struct ucn_v6_metric_owner) <= UCN_V6_METRIC_OWNER_STORAGE_BYTES ?
        1 : -1];

static void saturating_increment(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
}

static bool principal_equal(const ucn_v6_principal_t *left,
                            const ucn_v6_principal_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool domain_is_valid(const ucn_v6_route_domain_t *domain)
{
    return domain != NULL &&
           ucn_v6_principal_is_valid(&domain->origin_principal) &&
           ucn_v6_principal_is_valid(&domain->destination_principal) &&
           ucn_v6_binding_key_is_valid(&domain->origin_binding) &&
           ucn_v6_binding_key_is_valid(&domain->destination_binding) &&
           domain->origin_binding.realm_id ==
               domain->destination_binding.realm_id &&
           domain->origin_session_generation != 0U &&
           domain->origin_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           domain->destination_session_generation != 0U &&
           domain->destination_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool domain_equal(const ucn_v6_route_domain_t *left,
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

static bool key_is_valid(const ucn_v6_metric_key_t *key)
{
    return key != NULL && domain_is_valid(&key->domain) &&
           key->route_generation != 0U &&
           key->route_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           key->path_id != 0U && key->path_generation != 0U &&
           key->path_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool key_equal(const ucn_v6_metric_key_t *left,
                      const ucn_v6_metric_key_t *right)
{
    return left != NULL && right != NULL &&
           domain_equal(&left->domain, &right->domain) &&
           left->route_generation == right->route_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation;
}

static bool value_matches_known(uint16_t mask,
                                uint16_t bit,
                                uint64_t value,
                                bool zero_is_valid)
{
    if ((mask & bit) == 0U) {
        return value == 0U;
    }
    return zero_is_valid || value != 0U;
}

static bool sample_is_valid(const ucn_v6_metric_sample_t *sample)
{
    if (sample == NULL || sample->known_mask == 0U ||
        (sample->known_mask & (uint16_t)~UCN_V6_METRIC_ALL_KNOWN) != 0U ||
        sample->sample_window_us == 0U ||
        sample->loss_ppm > UINT32_C(1000000) ||
        sample->queue_occupancy_permille > 1000U ||
        sample->stability_score_permille > 1000U) {
        return false;
    }
    return value_matches_known(sample->known_mask,
                               UCN_V6_METRIC_ADMIN_COST_KNOWN,
                               sample->administrative_cost, true) &&
           value_matches_known(sample->known_mask,
                               UCN_V6_METRIC_LATENCY_KNOWN,
                               sample->latency_us, true) &&
           value_matches_known(sample->known_mask,
                               UCN_V6_METRIC_JITTER_KNOWN,
                               sample->jitter_us, true) &&
           value_matches_known(sample->known_mask,
                               UCN_V6_METRIC_LOSS_KNOWN,
                               sample->loss_ppm, true) &&
           value_matches_known(sample->known_mask,
                               UCN_V6_METRIC_BITRATE_KNOWN,
                               sample->available_bitrate_bps, false) &&
           value_matches_known(sample->known_mask,
                               UCN_V6_METRIC_QUEUE_KNOWN,
                               sample->queue_occupancy_permille, true) &&
           value_matches_known(sample->known_mask,
                               UCN_V6_METRIC_ENERGY_KNOWN,
                               sample->energy_cost, true) &&
           value_matches_known(sample->known_mask,
                               UCN_V6_METRIC_STABILITY_KNOWN,
                               sample->stability_score_permille, true);
}

static bool weights_nonzero(const ucn_v6_metric_weights_t *weights)
{
    return weights->administrative != 0U || weights->latency != 0U ||
           weights->jitter != 0U || weights->loss != 0U ||
           weights->inverse_bitrate != 0U || weights->queue != 0U ||
           weights->energy != 0U || weights->instability != 0U;
}

static bool policy_is_valid(const ucn_v6_metric_policy_t *policy)
{
    size_t index;
    if (policy == NULL || policy->algorithm_id == 0U ||
        policy->algorithm_id > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        policy->ewma_alpha_permille == 0U ||
        policy->ewma_alpha_permille > 1000U ||
        policy->stale_after_us == 0U) {
        return false;
    }
    for (index = 0U; index < 4U; ++index) {
        if (!weights_nonzero(&policy->class_weights[index])) {
            return false;
        }
    }
    return true;
}

static bool owner_is_valid(const ucn_v6_metric_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_METRIC_OWNER_MAGIC &&
           owner->schema == UCN_V6_METRIC_SCHEMA && owner->initialized &&
           !owner->faulted && owner->canary == UCN_V6_METRIC_OWNER_CANARY &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH;
}

static ucn_v6_metric_slot_t *find_slot(ucn_v6_metric_owner_t *owner,
                                      const ucn_v6_metric_key_t *key)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_METRIC_PATHS; ++index) {
        if (owner->slots[index].occupied &&
            key_equal(&owner->slots[index].key, key)) {
            return &owner->slots[index];
        }
    }
    return NULL;
}

static ucn_v6_metric_slot_t *find_free_slot(ucn_v6_metric_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_METRIC_PATHS; ++index) {
        if (!owner->slots[index].occupied) {
            return &owner->slots[index];
        }
    }
    return NULL;
}

static uint32_t ewma_u32(uint32_t previous,
                         uint32_t sample,
                         uint16_t alpha)
{
    uint64_t value = (uint64_t)previous * (uint64_t)(1000U - alpha) +
                     (uint64_t)sample * alpha + 999U;
    return (uint32_t)(value / 1000U);
}

static uint32_t ewma_u32_floor(uint32_t previous,
                               uint32_t sample,
                               uint16_t alpha)
{
    uint64_t value = (uint64_t)previous * (uint64_t)(1000U - alpha) +
                     (uint64_t)sample * alpha;
    return (uint32_t)(value / 1000U);
}

static uint16_t ewma_u16(uint16_t previous,
                         uint16_t sample,
                         uint16_t alpha)
{
    return (uint16_t)ewma_u32(previous, sample, alpha);
}

static uint16_t ewma_u16_floor(uint16_t previous,
                               uint16_t sample,
                               uint16_t alpha)
{
    return (uint16_t)ewma_u32_floor(previous, sample, alpha);
}

static bool sample_equal(const ucn_v6_metric_sample_t *left,
                         const ucn_v6_metric_sample_t *right)
{
    return left->known_mask == right->known_mask &&
           left->administrative_cost == right->administrative_cost &&
           left->latency_us == right->latency_us &&
           left->jitter_us == right->jitter_us &&
           left->loss_ppm == right->loss_ppm &&
           left->available_bitrate_bps == right->available_bitrate_bps &&
           left->queue_occupancy_permille ==
               right->queue_occupancy_permille &&
           left->energy_cost == right->energy_cost &&
           left->stability_score_permille ==
               right->stability_score_permille &&
           left->measured_at_us == right->measured_at_us &&
           left->sample_window_us == right->sample_window_us;
}

static uint32_t filtered_u32(uint16_t old_mask,
                             uint16_t new_mask,
                             uint16_t bit,
                             uint32_t old_value,
                             uint32_t new_value,
                             uint16_t alpha)
{
    if ((new_mask & bit) == 0U) {
        return 0U;
    }
    return (old_mask & bit) != 0U ?
               ewma_u32(old_value, new_value, alpha) : new_value;
}

static uint16_t filtered_u16(uint16_t old_mask,
                             uint16_t new_mask,
                             uint16_t bit,
                             uint16_t old_value,
                             uint16_t new_value,
                             uint16_t alpha)
{
    if ((new_mask & bit) == 0U) {
        return 0U;
    }
    return (old_mask & bit) != 0U ?
               ewma_u16(old_value, new_value, alpha) : new_value;
}

static uint32_t filtered_benefit_u32(uint16_t old_mask,
                                     uint16_t new_mask,
                                     uint16_t bit,
                                     uint32_t old_value,
                                     uint32_t new_value,
                                     uint16_t alpha)
{
    if ((new_mask & bit) == 0U) {
        return 0U;
    }
    return (old_mask & bit) != 0U ?
               ewma_u32_floor(old_value, new_value, alpha) : new_value;
}

static uint16_t filtered_benefit_u16(uint16_t old_mask,
                                     uint16_t new_mask,
                                     uint16_t bit,
                                     uint16_t old_value,
                                     uint16_t new_value,
                                     uint16_t alpha)
{
    if ((new_mask & bit) == 0U) {
        return 0U;
    }
    return (old_mask & bit) != 0U ?
               ewma_u16_floor(old_value, new_value, alpha) : new_value;
}

void ucn_v6_metric_default_policy(ucn_v6_metric_policy_t *policy)
{
    static const ucn_v6_metric_weights_t defaults[4] = {
        { 8U, 12U, 10U, 8U, 1U, 2U, 1U, 8U },
        { 6U, 10U, 10U, 8U, 1U, 4U, 1U, 7U },
        { 7U, 5U, 4U, 7U, 6U, 7U, 2U, 5U },
        { 5U, 2U, 2U, 6U, 12U, 8U, 3U, 4U }
    };
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
        policy->algorithm_id = UCN_V6_METRIC_ALGORITHM_DEFAULT;
        policy->ewma_alpha_permille = 250U;
        policy->stale_after_us = UINT64_C(1000000);
        memcpy(policy->class_weights, defaults, sizeof(defaults));
    }
}

ucn_v6_result_t ucn_v6_metric_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_metric_policy_t *policy,
    ucn_v6_metric_owner_t **owner_out)
{
    ucn_v6_metric_owner_t *owner;
    if (owner_out == NULL || !policy_is_valid(policy) ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_METRIC_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK) {
        return UCN_V6_ERR_CONFIG;
    }
    owner = (ucn_v6_metric_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_METRIC_OWNER_MAGIC;
    owner->schema = UCN_V6_METRIC_SCHEMA;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->policy = *policy;
    owner->initialized = true;
    owner->canary = UCN_V6_METRIC_OWNER_CANARY;
    *owner_out = owner;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_metric_ingest(
    ucn_v6_metric_owner_t *owner,
    const ucn_v6_metric_key_t *key,
    const ucn_v6_metric_sample_t *sample)
{
    ucn_v6_metric_slot_t *slot;
    ucn_v6_metric_sample_t next;
    uint16_t alpha;
    if (!owner_is_valid(owner) || !key_is_valid(key) ||
        !sample_is_valid(sample)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_slot(owner, key);
    if (slot != NULL && sample->measured_at_us < slot->filtered.measured_at_us) {
        return UCN_V6_ERR_REPLAY;
    }
    if (slot != NULL && sample->measured_at_us ==
                            slot->filtered.measured_at_us) {
        return sample_equal(sample, &slot->last_input) ?
                   UCN_V6_OK : UCN_V6_ERR_REPLAY;
    }
    if (slot == NULL) {
        slot = find_free_slot(owner);
        if (slot == NULL) {
            return UCN_V6_ERR_NO_SPACE;
        }
        memset(slot, 0, sizeof(*slot));
        slot->occupied = true;
        slot->key = *key;
        slot->last_input = *sample;
        slot->filtered = *sample;
        ++owner->stats.active_paths;
        saturating_increment(&owner->stats.samples_ingested);
        return UCN_V6_OK;
    }
    alpha = owner->policy.ewma_alpha_permille;
    memset(&next, 0, sizeof(next));
    next.known_mask = sample->known_mask;
    next.administrative_cost = filtered_u16(
        slot->filtered.known_mask, sample->known_mask,
        UCN_V6_METRIC_ADMIN_COST_KNOWN,
        slot->filtered.administrative_cost, sample->administrative_cost,
        alpha);
    next.latency_us = filtered_u32(
        slot->filtered.known_mask, sample->known_mask,
        UCN_V6_METRIC_LATENCY_KNOWN, slot->filtered.latency_us,
        sample->latency_us, alpha);
    next.jitter_us = filtered_u32(
        slot->filtered.known_mask, sample->known_mask,
        UCN_V6_METRIC_JITTER_KNOWN, slot->filtered.jitter_us,
        sample->jitter_us, alpha);
    next.loss_ppm = filtered_u32(
        slot->filtered.known_mask, sample->known_mask,
        UCN_V6_METRIC_LOSS_KNOWN, slot->filtered.loss_ppm,
        sample->loss_ppm, alpha);
    next.available_bitrate_bps = filtered_benefit_u32(
        slot->filtered.known_mask, sample->known_mask,
        UCN_V6_METRIC_BITRATE_KNOWN,
        slot->filtered.available_bitrate_bps,
        sample->available_bitrate_bps, alpha);
    next.queue_occupancy_permille = filtered_u16(
        slot->filtered.known_mask, sample->known_mask,
        UCN_V6_METRIC_QUEUE_KNOWN,
        slot->filtered.queue_occupancy_permille,
        sample->queue_occupancy_permille, alpha);
    next.energy_cost = filtered_u16(
        slot->filtered.known_mask, sample->known_mask,
        UCN_V6_METRIC_ENERGY_KNOWN, slot->filtered.energy_cost,
        sample->energy_cost, alpha);
    next.stability_score_permille = filtered_benefit_u16(
        slot->filtered.known_mask, sample->known_mask,
        UCN_V6_METRIC_STABILITY_KNOWN,
        slot->filtered.stability_score_permille,
        sample->stability_score_permille, alpha);
    next.measured_at_us = sample->measured_at_us;
    next.sample_window_us = sample->sample_window_us;
    slot->last_input = *sample;
    slot->filtered = next;
    saturating_increment(&owner->stats.samples_ingested);
    return UCN_V6_OK;
}

static bool add_weighted(uint64_t *total, uint64_t value, uint16_t weight)
{
    uint64_t contribution;
    if (weight == 0U) {
        return true;
    }
    if (value > UINT64_MAX / weight) {
        return false;
    }
    contribution = value * weight;
    if (*total > UINT64_MAX - contribution) {
        return false;
    }
    *total += contribution;
    return true;
}

static uint16_t required_mask(const ucn_v6_metric_weights_t *weights)
{
    uint16_t mask = 0U;
    if (weights->administrative != 0U) mask |= UCN_V6_METRIC_ADMIN_COST_KNOWN;
    if (weights->latency != 0U) mask |= UCN_V6_METRIC_LATENCY_KNOWN;
    if (weights->jitter != 0U) mask |= UCN_V6_METRIC_JITTER_KNOWN;
    if (weights->loss != 0U) mask |= UCN_V6_METRIC_LOSS_KNOWN;
    if (weights->inverse_bitrate != 0U) mask |= UCN_V6_METRIC_BITRATE_KNOWN;
    if (weights->queue != 0U) mask |= UCN_V6_METRIC_QUEUE_KNOWN;
    if (weights->energy != 0U) mask |= UCN_V6_METRIC_ENERGY_KNOWN;
    if (weights->instability != 0U) mask |= UCN_V6_METRIC_STABILITY_KNOWN;
    return mask;
}

ucn_v6_result_t ucn_v6_metric_score(
    ucn_v6_metric_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_metric_key_t *key,
    ucn_v6_traffic_class_t traffic_class,
    ucn_v6_metric_cost_t *cost)
{
    ucn_v6_metric_slot_t *slot;
    const ucn_v6_metric_weights_t *weights;
    uint16_t required;
    uint64_t total = 0U;
    ucn_v6_metric_cost_t next;
    if (!owner_is_valid(owner) || !key_is_valid(key) || cost == NULL ||
        (uint32_t)traffic_class >= 4U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_slot(owner, key);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (now_us < slot->filtered.measured_at_us ||
        now_us - slot->filtered.measured_at_us >=
            owner->policy.stale_after_us) {
        saturating_increment(&owner->stats.stale_reads);
        return UCN_V6_ERR_TIMEOUT;
    }
    weights = &owner->policy.class_weights[(size_t)traffic_class];
    required = required_mask(weights);
    if ((slot->filtered.known_mask & required) != required) {
        saturating_increment(&owner->stats.rejected_unknown);
        return UCN_V6_ERR_STATE;
    }
    if (!add_weighted(&total, slot->filtered.administrative_cost,
                      weights->administrative) ||
        !add_weighted(&total,
                      ((uint64_t)slot->filtered.latency_us + 9U) / 10U,
                      weights->latency) ||
        !add_weighted(&total,
                      ((uint64_t)slot->filtered.jitter_us + 9U) / 10U,
                      weights->jitter) ||
        !add_weighted(&total,
                      ((uint64_t)slot->filtered.loss_ppm + 99U) / 100U,
                      weights->loss) ||
        !add_weighted(&total,
                      (UINT64_C(1000000000) +
                       slot->filtered.available_bitrate_bps - 1U) /
                          slot->filtered.available_bitrate_bps,
                      weights->inverse_bitrate) ||
        !add_weighted(&total, slot->filtered.queue_occupancy_permille,
                      weights->queue) ||
        !add_weighted(&total, slot->filtered.energy_cost,
                      weights->energy) ||
        !add_weighted(&total,
                      1000U - slot->filtered.stability_score_permille,
                      weights->instability)) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    memset(&next, 0, sizeof(next));
    next.algorithm_id = owner->policy.algorithm_id;
    next.total_cost = total;
    next.hop_count = 1U;
    *cost = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_metric_cost_accumulate(
    const ucn_v6_metric_cost_t *prefix,
    const ucn_v6_metric_cost_t *hop,
    ucn_v6_metric_cost_t *total)
{
    ucn_v6_metric_cost_t next;
    if (prefix == NULL || hop == NULL || total == NULL ||
        prefix->algorithm_id == 0U ||
        prefix->algorithm_id > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        hop->algorithm_id > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        prefix->algorithm_id != hop->algorithm_id ||
        prefix->hop_count == 0U ||
        prefix->hop_count > UCN_V6_HOP_COUNT_MAX ||
        hop->hop_count == 0U || hop->hop_count > UCN_V6_HOP_COUNT_MAX ||
        UCN_V6_HOP_COUNT_MAX - prefix->hop_count < hop->hop_count ||
        UINT64_MAX - prefix->total_cost < hop->total_cost) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&next, 0, sizeof(next));
    next.algorithm_id = prefix->algorithm_id;
    next.total_cost = prefix->total_cost + hop->total_cost;
    next.hop_count = (uint16_t)(prefix->hop_count + hop->hop_count);
    *total = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_metric_expire(
    ucn_v6_metric_owner_t *owner,
    uint64_t now_us)
{
    size_t index;
    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_METRIC_PATHS; ++index) {
        if (owner->slots[index].occupied &&
            (now_us < owner->slots[index].filtered.measured_at_us ||
             now_us - owner->slots[index].filtered.measured_at_us >=
                 owner->policy.stale_after_us)) {
            memset(&owner->slots[index], 0, sizeof(owner->slots[index]));
            if (owner->stats.active_paths != 0U) {
                --owner->stats.active_paths;
            }
        }
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_metric_copy_view(
    const ucn_v6_metric_owner_t *owner,
    ucn_v6_metric_view_t *view)
{
    ucn_v6_metric_view_t next;
    if (!owner_is_valid(owner) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    next = owner->stats;
    next.faulted = owner->faulted;
    *view = next;
    return UCN_V6_OK;
}
