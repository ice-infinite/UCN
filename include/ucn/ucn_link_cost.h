#ifndef UCN_LINK_COST_H
#define UCN_LINK_COST_H

#include "ucn/ucn_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_LINK_COST_ADMINISTRATIVE_BIAS_MIN ((int16_t)-32)
#define UCN_LINK_COST_ADMINISTRATIVE_BIAS_MAX ((int16_t)64)
#define UCN_LINK_COST_METRICS_FRESH_MS UINT32_C(1500)
#define UCN_LINK_COST_METRICS_LIGHT_STALE_MS UINT32_C(3000)
#define UCN_LINK_COST_METRICS_STALE_LIMIT_MS UINT32_C(5000)

typedef enum ucn_link_cost_exclusion {
    UCN_LINK_COST_EXCLUSION_NONE = 0,
    UCN_LINK_COST_EXCLUSION_LINK_DOWN = 1,
    UCN_LINK_COST_EXCLUSION_MTU = 2,
    UCN_LINK_COST_EXCLUSION_CAPABILITY = 3,
    UCN_LINK_COST_EXCLUSION_METRICS_STALE = 4,
    UCN_LINK_COST_EXCLUSION_CONFIG = 5
} ucn_link_cost_exclusion_t;

enum {
    UCN_LINK_COST_INVALID_BASE = 1U << 0,
    UCN_LINK_COST_INVALID_QUEUE = 1U << 1,
    UCN_LINK_COST_INVALID_TX_FAILURE = 1U << 2,
    UCN_LINK_COST_INVALID_RX_FAILURE = 1U << 3,
    UCN_LINK_COST_INVALID_RTT_REFERENCE = 1U << 4,
    UCN_LINK_COST_INVALID_MEDIUM_BUSY = 1U << 5,
    UCN_LINK_COST_INVALID_MEDIUM_QUALITY = 1U << 6,
    UCN_LINK_COST_DUPLICATE_MEDIUM_SOURCE = 1U << 7
};

/* Pure LC-1 input.  Core supplies already-smoothed values; products may also
 * use this type in Host tests without constructing a Node.  A missing metric
 * contributes zero penalty.  metrics_timestamp_valid=false is the legacy
 * Adapter compatibility case and is treated as a snapshot taken now. */
typedef struct ucn_link_cost_input {
    bool link_up;
    bool mtu_sufficient;
    bool capability_allowed;
    bool base_cost_valid;
    uint16_t base_cost;
    bool rtt_reference_valid;
    uint16_t rtt_reference_ms;
    int16_t administrative_bias;
    bool queue_pressure_valid;
    uint16_t queue_pressure_per_mille;
    bool tx_failure_rate_valid;
    uint16_t tx_failure_per_mille;
    bool rx_failure_rate_valid;
    uint16_t rx_failure_per_mille;
    bool rtt_valid;
    uint16_t rtt_ms;
    bool medium_busy_valid;
    uint16_t medium_busy_per_mille;
    bool medium_quality_valid;
    uint16_t medium_quality_per_mille;
    /* Set only when both medium fields originate from the same hardware
     * counter.  LC-1 then uses busy and rejects the duplicate quality term. */
    bool medium_metrics_share_source;
    bool metrics_timestamp_valid;
    uint32_t metrics_timestamp_ms;
    uint32_t now_ms;
} ucn_link_cost_input_t;

typedef struct ucn_link_cost_result {
    bool selectable;
    bool base_cost_known;
    uint16_t effective_select_cost;
    ucn_link_cost_exclusion_t exclusion;
    uint16_t invalid_metric_mask;
    uint16_t queue_penalty;
    uint16_t tx_failure_penalty;
    uint16_t rx_failure_penalty;
    uint16_t rtt_penalty;
    uint16_t medium_busy_penalty;
    uint16_t medium_quality_penalty;
    uint16_t freshness_penalty;
    uint32_t metrics_age_ms;
} ucn_link_cost_result_t;

/* LC-1 uses floor((3 * previous + sample) / 4), never floating point. */
uint16_t ucn_link_cost_ewma_update(uint16_t previous, uint16_t sample);

/* Resolve one local Link score.  UCN_OK includes Unknown base Cost and a
 * normal state-gate exclusion; malformed static configuration returns
 * UCN_ERR_CONFIG and leaves result->selectable false. */
ucn_result_t ucn_link_cost_resolve(const ucn_link_cost_input_t *input,
                                   ucn_link_cost_result_t *result);

bool ucn_link_cost_is_sufficiently_better(uint16_t active_cost,
                                          uint16_t candidate_cost);

#ifdef __cplusplus
}
#endif

#endif
