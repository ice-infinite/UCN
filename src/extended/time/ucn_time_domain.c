/* Optional fixed-memory Time Domain FSM and conversion.
 * 可选的固定内存时间域状态机与换算。 */

#include "ucn/ucn_time_domain.h"

#include <limits.h>
#include <string.h>

/* EN: Saturating-increments a diagnostic counter.
 * 中文：对诊断计数器执行饱和递增。 */
static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
}

/* EN: Returns the absolute magnitude of a signed 64-bit value safely.
 * 中文：安全返回有符号 64 位值的绝对幅度。 */
static uint64_t signed_magnitude(int64_t value)
{
    if (value >= 0) {
        return (uint64_t)value;
    }
    return (uint64_t)(-(value + 1)) + 1U;
}

/* EN: Subtracts two signed offsets without undefined overflow.
 * 中文：在不产生未定义溢出的情况下计算两个有符号 offset 之差。 */
static bool signed_difference(int64_t left, int64_t right, int64_t *result)
{
    if ((right > 0 && left < INT64_MIN + right) ||
        (right < 0 && left > INT64_MAX + right)) {
        return false;
    }
    *result = left - right;
    return true;
}

/* EN: Applies a signed correction to an unsigned timestamp with checks.
 * 中文：经过检查后把有符号修正量应用到无符号时间戳。 */
static bool apply_signed(uint64_t base, int64_t correction, uint64_t *result)
{
    const uint64_t magnitude = signed_magnitude(correction);

    if (correction >= 0) {
        if (UINT64_MAX - base < magnitude) {
            return false;
        }
        *result = base + magnitude;
        return true;
    }
    if (base < magnitude) {
        return false;
    }
    *result = base - magnitude;
    return true;
}

/* EN: Computes delta*ppb/1e9 without a wide integer extension.
 * 中文：无需宽整数扩展即可计算 delta*ppb/1e9。 */
static bool scale_ppb(uint64_t delta_us, int32_t ppb, int64_t *result)
{
    const uint64_t magnitude = ppb < 0 ? (uint64_t)(-(int64_t)ppb) :
                                        (uint64_t)ppb;
    const uint64_t quotient = delta_us / UINT64_C(1000000000);
    const uint64_t remainder = delta_us % UINT64_C(1000000000);
    uint64_t scaled;

    if (magnitude != 0U && quotient > (uint64_t)INT64_MAX / magnitude) {
        return false;
    }
    scaled = quotient * magnitude;
    if (UINT64_MAX - scaled <
        (remainder * magnitude) / UINT64_C(1000000000)) {
        return false;
    }
    scaled += (remainder * magnitude) / UINT64_C(1000000000);
    if (scaled > (uint64_t)INT64_MAX) {
        return false;
    }
    *result = ppb < 0 ? -(int64_t)scaled : (int64_t)scaled;
    return true;
}

/* EN: Computes ceil(delta*ppb/1e9) with saturation detection.
 * 中文：计算 ceil(delta*ppb/1e9) 并检测饱和。 */
static bool uncertainty_growth(uint64_t delta_us,
                               uint32_t ppb,
                               uint32_t *growth_us)
{
    const uint64_t quotient = delta_us / UINT64_C(1000000000);
    const uint64_t remainder = delta_us % UINT64_C(1000000000);
    uint64_t growth;
    uint64_t tail;

    if (ppb != 0U && quotient > UINT64_MAX / ppb) {
        return false;
    }
    growth = quotient * ppb;
    tail = remainder * (uint64_t)ppb;
    if (tail != 0U) {
        tail = tail / UINT64_C(1000000000) +
               (tail % UINT64_C(1000000000) != 0U ? 1U : 0U);
    }
    if (UINT64_MAX - growth < tail || growth + tail > UINT32_MAX) {
        return false;
    }
    *growth_us = (uint32_t)(growth + tail);
    return true;
}

/* EN: Sorts the tiny fixed sample window and returns its median.
 * 中文：排序很小的固定样本窗口并返回中位数。 */
static int64_t median_offset(const ucn_time_domain_t *domain)
{
    int64_t values[UCN_TIME_DOMAIN_SAMPLE_WINDOW];
    size_t index;
    size_t inner;

    for (index = 0U; index < domain->sample_count; ++index) {
        values[index] = domain->offset_samples[index];
    }
    for (index = 1U; index < domain->sample_count; ++index) {
        const int64_t value = values[index];
        inner = index;
        while (inner != 0U && values[inner - 1U] > value) {
            values[inner] = values[inner - 1U];
            --inner;
        }
        values[inner] = value;
    }
    return values[domain->sample_count / 2U];
}

/* EN: Validates the static Domain configuration and all duration bounds.
 * 中文：校验静态时间域配置及全部时长边界。 */
bool ucn_time_domain_config_is_valid(const ucn_time_domain_config_t *config)
{
    return config != NULL && config->clock_domain_id != 0U &&
           config->clock_domain_id <= UCN_REALTIME_CLOCK_DOMAIN_ID_MAX &&
           config->master_node_id != 0U &&
           config->master_node_id != UCN_NODE_BROADCAST &&
           config->master_session_id != 0U &&
           config->domain_generation != 0U &&
           config->domain_generation <=
               UCN_REALTIME_DOMAIN_GENERATION_MAX &&
           config->lock_sample_count != 0U &&
           config->lock_sample_count <= UCN_TIME_DOMAIN_SAMPLE_WINDOW &&
           config->sync_timeout_us != 0U && config->max_holdover_us != 0U &&
           UINT64_MAX - config->sync_timeout_us >= config->max_holdover_us &&
           config->max_offset_jump_us != 0U &&
           config->max_slew_per_sample_us != 0U &&
           config->max_slew_per_sample_us <= config->max_offset_jump_us &&
           config->max_rate_ppb != 0U &&
           config->max_rate_ppb <= (uint32_t)INT32_MAX &&
           config->oscillator_uncertainty_known &&
           config->oscillator_uncertainty_ppb != 0U;
}

/* EN: Validates all caller-visible mutable Domain state before indexing the
 * fixed sample window or publishing time.
 * 中文：在索引固定样本窗口或发布时间前校验全部调用者可见的可变状态。 */
static bool domain_state_is_valid(const ucn_time_domain_t *domain)
{
    if (domain == NULL || !domain->initialized ||
        !ucn_time_domain_config_is_valid(&domain->config) ||
        domain->phase > UCN_TIME_DOMAIN_FAULT ||
        domain->sample_count > UCN_TIME_DOMAIN_SAMPLE_WINDOW ||
        domain->sample_cursor >= UCN_TIME_DOMAIN_SAMPLE_WINDOW ||
        signed_magnitude(domain->rate_ppb) > domain->config.max_rate_ppb ||
        domain->current_uncertainty_us < domain->base_uncertainty_us) {
        return false;
    }
    if (!domain->has_valid_sample) {
        return domain->sample_count == 0U &&
               domain->consecutive_valid_samples == 0U &&
               (!domain->has_output || domain->has_sample_high_water) &&
               (domain->phase == UCN_TIME_DOMAIN_ACQUIRING ||
                 domain->phase == UCN_TIME_DOMAIN_UNSYNCED);
    }
    if (!domain->has_sample_high_water || domain->sample_count == 0U) {
        return false;
    }
    if ((domain->phase == UCN_TIME_DOMAIN_LOCKED ||
         domain->phase == UCN_TIME_DOMAIN_HOLDOVER) &&
        domain->consecutive_valid_samples < domain->config.lock_sample_count) {
        return false;
    }
    return true;
}

/* EN: Initializes one Domain with no inherited sample or replay history.
 * 中文：初始化一个不继承任何样本或重放历史的时间域。 */
ucn_result_t ucn_time_domain_init(ucn_time_domain_t *domain,
                                  const ucn_time_domain_config_t *config)
{
    ucn_time_domain_t initialized;

    if (domain == NULL || !ucn_time_domain_config_is_valid(config)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&initialized, 0, sizeof(initialized));
    initialized.config = *config;
    initialized.phase = UCN_TIME_DOMAIN_ACQUIRING;
    initialized.initialized = true;
    *domain = initialized;
    return UCN_OK;
}

/* EN: Rebinds an authenticated restarted Master and destroys old samples.
 * 中文：重新绑定认证的重启 Master，并销毁全部旧样本。 */
ucn_result_t ucn_time_domain_rebind_master(
    ucn_time_domain_t *domain,
    ucn_session_id_t master_session_id,
    uint32_t domain_generation)
{
    ucn_time_domain_config_t config;

    if (domain == NULL || master_session_id == 0U ||
        domain_generation == 0U ||
        domain_generation > UCN_REALTIME_DOMAIN_GENERATION_MAX) {
        return UCN_ERR_ARGUMENT;
    }
    if (!domain_state_is_valid(domain)) {
        return UCN_ERR_STATE;
    }
    if (domain_generation <= domain->config.domain_generation) {
        return UCN_ERR_REPLAY;
    }
    config = domain->config;
    config.master_session_id = master_session_id;
    config.domain_generation = domain_generation;
    return ucn_time_domain_init(domain, &config);
}

/* EN: Checks that a sample belongs to the exact authenticated Domain.
 * 中文：检查样本是否属于精确的认证时间域。 */
static bool sample_identity_matches(const ucn_time_domain_t *domain,
                                    const ucn_time_sync_sample_t *sample)
{
    return sample->clock_domain_id == domain->config.clock_domain_id &&
           sample->master_node_id == domain->config.master_node_id &&
           sample->master_session_id == domain->config.master_session_id &&
           sample->domain_generation == domain->config.domain_generation;
}

/* EN: Moves one Domain to terminal FAULT without publishing a time view.
 * 中文：把时间域转入终止 FAULT，且不再发布时间视图。 */
static ucn_result_t domain_fault(ucn_time_domain_t *domain)
{
    domain->phase = UCN_TIME_DOMAIN_FAULT;
    increment_saturated(&domain->stats.faults);
    return UCN_ERR_STATE;
}

/* EN: Converts local monotonic time using checked offset and drift.
 * 中文：使用经过检查的 offset 和 drift 换算本地单调时间。 */
static bool domain_time_compute(const ucn_time_domain_t *domain,
                                uint64_t local_us,
                                uint64_t *domain_us);

/* EN: Proves that a candidate lock cannot republish time below the retained
 * high-water mark of the same Domain generation.
 * 中文：证明候选锁定不会在同一 Domain generation 内发布低于已保留高水位的
 * 时间。 */
static bool lock_candidate_is_monotonic(const ucn_time_domain_t *domain,
                                        uint64_t local_us)
{
    uint64_t candidate_domain_us;

    return domain_time_compute(domain, local_us, &candidate_domain_us) &&
           (!domain->has_output ||
            candidate_domain_us >= domain->last_output_domain_us);
}

/* EN: Destroys the stale acquisition/filter window while preserving the
 * same-generation sample and published-time high-water marks.
 * 中文：销毁失效的采集/滤波窗口，但保留同 generation 的样本与发布时间高水位。 */
static void domain_clear_acquisition(ucn_time_domain_t *domain)
{
    (void)memset(domain->offset_samples, 0, sizeof(domain->offset_samples));
    domain->offset_us = 0;
    domain->rate_ppb = 0;
    domain->rate_reference_local_us = 0U;
    domain->base_uncertainty_us = 0U;
    domain->current_uncertainty_us = 0U;
    domain->sample_count = 0U;
    domain->sample_cursor = 0U;
    domain->consecutive_valid_samples = 0U;
    domain->has_valid_sample = false;
}

/* EN: Adds a diagnostic or effective sync sample to bounded state.
 * 中文：把诊断或有效同步样本加入有界状态。 */
ucn_result_t ucn_time_domain_ingest_sample(
    ucn_time_domain_t *domain,
    const ucn_time_sync_sample_t *sample)
{
    int64_t offset_delta;
    uint64_t offset_delta_magnitude;

    if (domain == NULL || sample == NULL ||
        (sample->kind != UCN_TIME_SAMPLE_DIAGNOSTIC &&
         sample->kind != UCN_TIME_SAMPLE_VALID_SYNC)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!domain_state_is_valid(domain)) {
        return UCN_ERR_STATE;
    }
    if (domain->phase == UCN_TIME_DOMAIN_FAULT ||
        !sample_identity_matches(domain, sample) ||
        (domain->has_sample_high_water &&
         sample->local_sample_us <= domain->last_sample_local_us)) {
        increment_saturated(&domain->stats.rejected_samples);
        return UCN_ERR_STATE;
    }
    if (sample->kind == UCN_TIME_SAMPLE_DIAGNOSTIC) {
        increment_saturated(&domain->stats.diagnostic_samples);
        return UCN_OK;
    }
    if (!sample->uncertainty_known || sample->uncertainty_us == 0U) {
        increment_saturated(&domain->stats.rejected_samples);
        return UCN_ERR_STATE;
    }

    if (domain->has_valid_sample) {
        if (!signed_difference(sample->offset_us, domain->offset_us,
                               &offset_delta)) {
            return domain_fault(domain);
        }
        offset_delta_magnitude = signed_magnitude(offset_delta);
        if (offset_delta_magnitude > domain->config.max_offset_jump_us) {
            return domain_fault(domain);
        }
        if (sample->local_sample_us > domain->last_sample_local_us) {
            const uint64_t local_delta =
                sample->local_sample_us - domain->last_sample_local_us;
            int64_t rate_candidate;
            int64_t numerator;

            if (local_delta > (uint64_t)INT64_MAX ||
                offset_delta > INT64_MAX / INT64_C(1000000000) ||
                offset_delta < INT64_MIN / INT64_C(1000000000)) {
                return domain_fault(domain);
            }
            numerator = offset_delta * INT64_C(1000000000);
            rate_candidate = numerator / (int64_t)local_delta;
            if (signed_magnitude(rate_candidate) >
                domain->config.max_rate_ppb) {
                return domain_fault(domain);
            }
            domain->rate_ppb = (int32_t)rate_candidate;
            domain->rate_reference_local_us = sample->local_sample_us;
        }
        if (offset_delta_magnitude >
            domain->config.max_slew_per_sample_us) {
            domain->offset_us += offset_delta < 0 ?
                -(int64_t)domain->config.max_slew_per_sample_us :
                (int64_t)domain->config.max_slew_per_sample_us;
        } else {
            domain->offset_us = sample->offset_us;
        }
    } else {
        domain->offset_us = sample->offset_us;
        domain->rate_reference_local_us = sample->local_sample_us;
    }

    domain->offset_samples[domain->sample_cursor] = sample->offset_us;
    domain->sample_cursor = (uint8_t)(
        (domain->sample_cursor + 1U) % UCN_TIME_DOMAIN_SAMPLE_WINDOW);
    if (domain->sample_count < UCN_TIME_DOMAIN_SAMPLE_WINDOW) {
        ++domain->sample_count;
    }
    if (domain->consecutive_valid_samples < UINT8_MAX) {
        ++domain->consecutive_valid_samples;
    }
    domain->last_sample_local_us = sample->local_sample_us;
    domain->base_uncertainty_us = sample->uncertainty_us;
    domain->current_uncertainty_us = sample->uncertainty_us;
    domain->has_valid_sample = true;
    domain->has_sample_high_water = true;
    increment_saturated(&domain->stats.valid_samples);

    if (domain->phase == UCN_TIME_DOMAIN_ACQUIRING ||
        domain->phase == UCN_TIME_DOMAIN_UNSYNCED) {
        if (domain->consecutive_valid_samples >=
            domain->config.lock_sample_count) {
            domain->offset_us = median_offset(domain);
            domain->rate_reference_local_us = sample->local_sample_us;
            if (!lock_candidate_is_monotonic(domain,
                                             sample->local_sample_us)) {
                return domain_fault(domain);
            }
            domain->phase = UCN_TIME_DOMAIN_LOCKED;
            increment_saturated(&domain->stats.lock_transitions);
        } else {
            domain->phase = UCN_TIME_DOMAIN_ACQUIRING;
        }
    } else if (domain->phase == UCN_TIME_DOMAIN_HOLDOVER) {
        if (!lock_candidate_is_monotonic(domain, sample->local_sample_us)) {
            return domain_fault(domain);
        }
        domain->phase = UCN_TIME_DOMAIN_LOCKED;
        increment_saturated(&domain->stats.lock_transitions);
    }
    return UCN_OK;
}

/* EN: Advances timeout state and grows a conservative uncertainty bound.
 * 中文：推进超时状态并扩大保守误差上界。 */
ucn_result_t ucn_time_domain_step(ucn_time_domain_t *domain,
                                  uint64_t local_monotonic_us)
{
    uint64_t elapsed;
    uint32_t growth;

    if (domain == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!domain_state_is_valid(domain)) {
        return UCN_ERR_STATE;
    }
    if (domain->phase == UCN_TIME_DOMAIN_FAULT) {
        return UCN_ERR_STATE;
    }
    if (!domain->has_valid_sample) {
        return UCN_OK;
    }
    if (local_monotonic_us < domain->last_sample_local_us) {
        return domain_fault(domain);
    }
    elapsed = local_monotonic_us - domain->last_sample_local_us;
    if (!uncertainty_growth(elapsed,
                            domain->config.oscillator_uncertainty_ppb,
                            &growth) ||
        UINT32_MAX - domain->base_uncertainty_us < growth) {
        return domain_fault(domain);
    }
    domain->current_uncertainty_us = domain->base_uncertainty_us + growth;

    if (elapsed >= domain->config.sync_timeout_us +
                   domain->config.max_holdover_us) {
        if (domain->phase != UCN_TIME_DOMAIN_UNSYNCED) {
            domain->phase = UCN_TIME_DOMAIN_UNSYNCED;
            domain_clear_acquisition(domain);
            increment_saturated(&domain->stats.unsynced_transitions);
        }
    } else if (elapsed >= domain->config.sync_timeout_us &&
               domain->phase == UCN_TIME_DOMAIN_LOCKED) {
        domain->phase = UCN_TIME_DOMAIN_HOLDOVER;
        increment_saturated(&domain->stats.holdover_transitions);
    }
    return UCN_OK;
}

/* EN: Converts local monotonic time using checked offset and drift.
 * 中文：使用经过检查的 offset 和 drift 换算本地单调时间。 */
static bool domain_time_compute(const ucn_time_domain_t *domain,
                                uint64_t local_us,
                                uint64_t *domain_us)
{
    uint64_t with_offset;
    int64_t rate_correction;

    if (!apply_signed(local_us, domain->offset_us, &with_offset)) {
        return false;
    }
    if (local_us < domain->rate_reference_local_us ||
        !scale_ppb(local_us - domain->rate_reference_local_us,
                   domain->rate_ppb, &rate_correction)) {
        return false;
    }
    return apply_signed(with_offset, rate_correction, domain_us);
}

/* EN: Produces a monotonic point-in-time view for RT-02 admission.
 * 中文：为 RT-02 准入生成单调的即时视图。 */
ucn_result_t ucn_time_domain_get_clock_view(
    ucn_time_domain_t *domain,
    uint64_t local_monotonic_us,
    ucn_realtime_clock_view_t *view)
{
    ucn_realtime_clock_view_t produced;
    uint64_t domain_us;
    ucn_result_t status;

    if (domain == NULL || view == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!domain_state_is_valid(domain)) {
        return UCN_ERR_STATE;
    }
    status = ucn_time_domain_step(domain, local_monotonic_us);
    if (status != UCN_OK ||
        (domain->phase != UCN_TIME_DOMAIN_LOCKED &&
         domain->phase != UCN_TIME_DOMAIN_HOLDOVER)) {
        return UCN_ERR_STATE;
    }
    if (domain->has_output &&
        local_monotonic_us < domain->last_output_local_us) {
        return domain_fault(domain);
    }
    if (!domain_time_compute(domain, local_monotonic_us, &domain_us)) {
        return domain_fault(domain);
    }
    if (domain->has_output && domain_us < domain->last_output_domain_us) {
        return domain_fault(domain);
    }
    (void)memset(&produced, 0, sizeof(produced));
    produced.available = true;
    produced.uncertainty_known = true;
    produced.holdover = domain->phase == UCN_TIME_DOMAIN_HOLDOVER;
    produced.clock_domain_id = domain->config.clock_domain_id;
    produced.domain_generation = domain->config.domain_generation;
    produced.domain_time_us = domain_us;
    produced.uncertainty_us = domain->current_uncertainty_us;
    if (produced.holdover) {
        produced.holdover_age_us =
            local_monotonic_us - domain->last_sample_local_us -
            domain->config.sync_timeout_us;
    }
    domain->last_output_local_us = local_monotonic_us;
    domain->last_output_domain_us = domain_us;
    domain->has_output = true;
    *view = produced;
    return UCN_OK;
}

/* EN: Advances generation without ever wrapping or reusing zero.
 * 中文：推进 generation，绝不回绕或复用零值。 */
ucn_result_t ucn_time_domain_generation_next(uint32_t current_generation,
                                             uint32_t *next_generation)
{
    if (next_generation == NULL ||
        current_generation > UCN_REALTIME_DOMAIN_GENERATION_MAX) {
        return UCN_ERR_ARGUMENT;
    }
    if (current_generation == UCN_REALTIME_DOMAIN_GENERATION_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *next_generation = current_generation + 1U;
    return UCN_OK;
}
