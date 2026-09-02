#include <string.h>

#include "ucn/ucn_link_cost.h"

/*
 * EN: Calculates `queue_penalty` with bounded, deterministic Link Cost arithmetic.
 * 中文：使用有界且确定性的 Link Cost 算术计算 `queue_penalty`。
 */
static uint16_t queue_penalty(uint16_t value)
{
    if (value < 250U) {
        return 0U;
    }
    if (value < 500U) {
        return 10U;
    }
    if (value < 700U) {
        return 25U;
    }
    if (value < 850U) {
        return 50U;
    }
    return 80U;
}

/*
 * EN: Calculates `tx_failure_penalty` with bounded, deterministic Link Cost arithmetic.
 * 中文：使用有界且确定性的 Link Cost 算术计算 `tx_failure_penalty`。
 */
static uint16_t tx_failure_penalty(uint16_t value)
{
    if (value < 5U) {
        return 0U;
    }
    if (value < 20U) {
        return 8U;
    }
    if (value < 50U) {
        return 20U;
    }
    if (value < 100U) {
        return 40U;
    }
    if (value < 200U) {
        return 80U;
    }
    return 160U;
}

/*
 * EN: Calculates `rx_failure_penalty` with bounded, deterministic Link Cost arithmetic.
 * 中文：使用有界且确定性的 Link Cost 算术计算 `rx_failure_penalty`。
 */
static uint16_t rx_failure_penalty(uint16_t value)
{
    if (value < 5U) {
        return 0U;
    }
    if (value < 20U) {
        return 4U;
    }
    if (value < 50U) {
        return 10U;
    }
    if (value < 100U) {
        return 20U;
    }
    if (value < 200U) {
        return 40U;
    }
    return 80U;
}

/*
 * EN: Calculates `medium_busy_penalty` with bounded, deterministic Link Cost arithmetic.
 * 中文：使用有界且确定性的 Link Cost 算术计算 `medium_busy_penalty`。
 */
static uint16_t medium_busy_penalty(uint16_t value)
{
    if (value < 250U) {
        return 0U;
    }
    if (value < 500U) {
        return 5U;
    }
    if (value < 700U) {
        return 15U;
    }
    if (value < 850U) {
        return 35U;
    }
    return 70U;
}

/*
 * EN: Calculates `medium_quality_penalty` with bounded, deterministic Link Cost arithmetic.
 * 中文：使用有界且确定性的 Link Cost 算术计算 `medium_quality_penalty`。
 */
static uint16_t medium_quality_penalty(uint16_t value)
{
    if (value >= 850U) {
        return 0U;
    }
    if (value >= 700U) {
        return 5U;
    }
    if (value >= 500U) {
        return 15U;
    }
    if (value >= 300U) {
        return 35U;
    }
    return 70U;
}

/*
 * EN: Checks whether `per_mille` satisfies the Link Cost module's validity rules.
 * 中文：检查 `per_mille` 是否满足 Link Cost 模块的合法性规则。
 */
static bool per_mille_is_valid(bool valid, uint16_t value)
{
    return valid && value <= UCN_LINK_METRIC_PER_MILLE_MAX;
}

/*
 * EN: Updates a Link Cost sample with the fixed-point EWMA rule.
 * 中文：使用定点 EWMA 规则更新 Link Cost 样本。
 */
uint16_t ucn_link_cost_ewma_update(uint16_t previous, uint16_t sample)
{
    return (uint16_t)(((uint32_t)previous * 3U + (uint32_t)sample) / 4U);
}

/*
 * EN: Selects or resolves `resolve` using deterministic Link Cost rules.
 * 中文：按照确定性的 Link Cost 规则选择或解析 `resolve`。
 */
ucn_result_t ucn_link_cost_resolve(const ucn_link_cost_input_t *input,
                                   ucn_link_cost_result_t *result)
{
    uint32_t total;
    int32_t biased_base;

    if (input == NULL || result == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(result, 0, sizeof(*result));
    if (!input->link_up) {
        result->exclusion = UCN_LINK_COST_EXCLUSION_LINK_DOWN;
        return UCN_OK;
    }
    if (!input->mtu_sufficient) {
        result->exclusion = UCN_LINK_COST_EXCLUSION_MTU;
        return UCN_OK;
    }
    if (!input->capability_allowed) {
        result->exclusion = UCN_LINK_COST_EXCLUSION_CAPABILITY;
        return UCN_OK;
    }
    if (input->administrative_bias < UCN_LINK_COST_ADMINISTRATIVE_BIAS_MIN ||
        input->administrative_bias > UCN_LINK_COST_ADMINISTRATIVE_BIAS_MAX) {
        result->exclusion = UCN_LINK_COST_EXCLUSION_CONFIG;
        return UCN_ERR_CONFIG;
    }

    result->metrics_age_ms = input->metrics_timestamp_valid ?
        (uint32_t)(input->now_ms - input->metrics_timestamp_ms) : 0U;
    if (result->metrics_age_ms > UCN_LINK_COST_METRICS_STALE_LIMIT_MS) {
        result->exclusion = UCN_LINK_COST_EXCLUSION_METRICS_STALE;
        return UCN_OK;
    }
    if (result->metrics_age_ms > UCN_LINK_COST_METRICS_LIGHT_STALE_MS) {
        result->freshness_penalty = 60U;
    } else if (result->metrics_age_ms > UCN_LINK_COST_METRICS_FRESH_MS) {
        result->freshness_penalty = 20U;
    }

    result->selectable = true;
    result->base_cost_known = input->base_cost_valid && input->base_cost != 0U &&
                              input->base_cost != UCN_LINK_ROUTE_COST_UNKNOWN;
    if (input->base_cost_valid && !result->base_cost_known) {
        result->invalid_metric_mask |= UCN_LINK_COST_INVALID_BASE;
    }
    if (!result->base_cost_known) {
        result->effective_select_cost = UCN_LINK_ROUTE_COST_UNKNOWN;
        return UCN_OK;
    }

    if (per_mille_is_valid(input->queue_pressure_valid,
                           input->queue_pressure_per_mille)) {
        result->queue_penalty = queue_penalty(input->queue_pressure_per_mille);
    } else if (input->queue_pressure_valid) {
        result->invalid_metric_mask |= UCN_LINK_COST_INVALID_QUEUE;
    }
    if (per_mille_is_valid(input->tx_failure_rate_valid,
                           input->tx_failure_per_mille)) {
        result->tx_failure_penalty =
            tx_failure_penalty(input->tx_failure_per_mille);
    } else if (input->tx_failure_rate_valid) {
        result->invalid_metric_mask |= UCN_LINK_COST_INVALID_TX_FAILURE;
    }
    if (per_mille_is_valid(input->rx_failure_rate_valid,
                           input->rx_failure_per_mille)) {
        result->rx_failure_penalty =
            rx_failure_penalty(input->rx_failure_per_mille);
    } else if (input->rx_failure_rate_valid) {
        result->invalid_metric_mask |= UCN_LINK_COST_INVALID_RX_FAILURE;
    }
    if (input->rtt_valid) {
        if (!input->rtt_reference_valid || input->rtt_reference_ms == 0U) {
            result->invalid_metric_mask |= UCN_LINK_COST_INVALID_RTT_REFERENCE;
        } else if (input->rtt_ms > input->rtt_reference_ms) {
            const uint32_t excess =
                (uint32_t)input->rtt_ms - input->rtt_reference_ms;
            const uint32_t penalty = (excess + 1U) / 2U;

            result->rtt_penalty = (uint16_t)(penalty > 80U ? 80U : penalty);
        }
    }
    if (per_mille_is_valid(input->medium_busy_valid,
                           input->medium_busy_per_mille)) {
        result->medium_busy_penalty =
            medium_busy_penalty(input->medium_busy_per_mille);
    } else if (input->medium_busy_valid) {
        result->invalid_metric_mask |= UCN_LINK_COST_INVALID_MEDIUM_BUSY;
    }
    if (input->medium_busy_valid && input->medium_quality_valid &&
        input->medium_metrics_share_source) {
        result->invalid_metric_mask |= UCN_LINK_COST_DUPLICATE_MEDIUM_SOURCE;
    } else if (per_mille_is_valid(input->medium_quality_valid,
                                  input->medium_quality_per_mille)) {
        result->medium_quality_penalty =
            medium_quality_penalty(input->medium_quality_per_mille);
    } else if (input->medium_quality_valid) {
        result->invalid_metric_mask |= UCN_LINK_COST_INVALID_MEDIUM_QUALITY;
    }

    biased_base = (int32_t)input->base_cost + input->administrative_bias;
    total = (uint32_t)(biased_base < 1 ? 1 : biased_base);
    total += result->queue_penalty;
    total += result->tx_failure_penalty;
    total += result->rx_failure_penalty;
    total += result->rtt_penalty;
    total += result->medium_busy_penalty;
    total += result->medium_quality_penalty;
    total += result->freshness_penalty;
    result->effective_select_cost =
        (uint16_t)(total > UCN_LINK_ROUTE_COST_MAX ?
                       UCN_LINK_ROUTE_COST_MAX : total);
    return UCN_OK;
}

/*
 * EN: Checks the `is_sufficiently_better` predicate against current Link Cost state.
 * 中文：根据当前 Link Cost 状态检查 `is_sufficiently_better` 条件。
 */
bool ucn_link_cost_is_sufficiently_better(uint16_t active_cost,
                                          uint16_t candidate_cost)
{
    return (uint32_t)candidate_cost * 100U <=
           (uint32_t)active_cost * 80U;
}
