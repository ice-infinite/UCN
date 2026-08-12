#include <string.h>

#include "test_support.h"
#include "ucn/ucn_link_cost.h"

static void cost_input_init(ucn_link_cost_input_t *input,
                            uint16_t base_cost,
                            uint16_t rtt_reference_ms)
{
    (void)memset(input, 0, sizeof(*input));
    input->link_up = true;
    input->mtu_sufficient = true;
    input->capability_allowed = true;
    input->base_cost_valid = true;
    input->base_cost = base_cost;
    input->rtt_reference_valid = rtt_reference_ms != 0U;
    input->rtt_reference_ms = rtt_reference_ms;
    input->metrics_timestamp_valid = true;
    input->metrics_timestamp_ms = UINT32_C(1000);
    input->now_ms = UINT32_C(1000);
}

static int expect_cost(const ucn_link_cost_input_t *input,
                       uint16_t expected)
{
    ucn_link_cost_result_t result;

    TEST_ASSERT(ucn_link_cost_resolve(input, &result) == UCN_OK);
    TEST_ASSERT(result.selectable && result.base_cost_known);
    TEST_ASSERT(result.effective_select_cost == expected);
    return 0;
}

int test_link_cost(void)
{
    ucn_link_cost_input_t input;
    ucn_link_cost_result_t result;
    const uint32_t ages[] = { 1500U, 1501U, 3000U, 3001U, 5000U, 5001U };
    const uint16_t penalties[] = { 0U, 20U, 20U, 60U, 60U, 0U };
    size_t index;

    /* C01: UART 115200. */
    cost_input_init(&input, 34U, 5U);
    input.queue_pressure_valid = true;
    input.queue_pressure_per_mille = 760U;
    input.tx_failure_rate_valid = true;
    input.tx_failure_per_mille = 25U;
    input.rtt_valid = true;
    input.rtt_ms = 13U;
    TEST_ASSERT(expect_cost(&input, 108U) == 0);

    /* C02: ESP-NOW. */
    cost_input_init(&input, 45U, 12U);
    input.queue_pressure_valid = true;
    input.queue_pressure_per_mille = 100U;
    input.tx_failure_rate_valid = true;
    input.tx_failure_per_mille = 0U;
    input.rtt_valid = true;
    input.rtt_ms = 10U;
    input.medium_quality_valid = true;
    input.medium_quality_per_mille = 700U;
    TEST_ASSERT(expect_cost(&input, 50U) == 0);

    /* C03: CAN-FD 1M/4M. */
    cost_input_init(&input, 12U, 3U);
    input.queue_pressure_valid = true;
    input.queue_pressure_per_mille = 500U;
    input.tx_failure_rate_valid = true;
    input.tx_failure_per_mille = 100U;
    input.rx_failure_rate_valid = true;
    input.rx_failure_per_mille = 50U;
    input.rtt_valid = true;
    input.rtt_ms = 10U;
    input.medium_busy_valid = true;
    input.medium_busy_per_mille = 750U;
    TEST_ASSERT(expect_cost(&input, 176U) == 0);

    /* C04/C05: the only subtraction is the static bias; addition saturates. */
    cost_input_init(&input, 14U, 4U);
    input.administrative_bias = -10;
    TEST_ASSERT(expect_cost(&input, 4U) == 0);
    cost_input_init(&input, UCN_LINK_ROUTE_COST_MAX, 4U);
    input.queue_pressure_valid = true;
    input.queue_pressure_per_mille = 250U;
    TEST_ASSERT(expect_cost(&input, UCN_LINK_ROUTE_COST_MAX) == 0);

    /* C06: freshness boundaries are inclusive exactly as LC-1 specifies. */
    for (index = 0U; index < sizeof(ages) / sizeof(ages[0]); ++index) {
        cost_input_init(&input, 100U, 5U);
        input.now_ms = input.metrics_timestamp_ms + ages[index];
        TEST_ASSERT(ucn_link_cost_resolve(&input, &result) == UCN_OK);
        if (ages[index] > UCN_LINK_COST_METRICS_STALE_LIMIT_MS) {
            TEST_ASSERT(!result.selectable &&
                        result.exclusion ==
                            UCN_LINK_COST_EXCLUSION_METRICS_STALE);
        } else {
            TEST_ASSERT(result.selectable &&
                        result.freshness_penalty == penalties[index]);
        }
    }

    /* C07: equality at 20% is accepted, 19% is not.  The Neighbor test owns
     * the three-sample/two-Probe state-machine evidence. */
    TEST_ASSERT(ucn_link_cost_is_sufficiently_better(100U, 80U));
    TEST_ASSERT(!ucn_link_cost_is_sufficiently_better(100U, 81U));

    /* C08/C09: Unknown is a separate state, never the number 65535. */
    cost_input_init(&input, 2000U, 5U);
    TEST_ASSERT(ucn_link_cost_resolve(&input, &result) == UCN_OK &&
                result.base_cost_known);
    input.base_cost_valid = false;
    TEST_ASSERT(ucn_link_cost_resolve(&input, &result) == UCN_OK &&
                result.selectable && !result.base_cost_known &&
                result.effective_select_cost == UCN_LINK_ROUTE_COST_UNKNOWN);

    /* C10: hard Down is excluded before any soft score is considered. */
    cost_input_init(&input, 10U, 5U);
    input.link_up = false;
    TEST_ASSERT(ucn_link_cost_resolve(&input, &result) == UCN_OK &&
                !result.selectable &&
                result.exclusion == UCN_LINK_COST_EXCLUSION_LINK_DOWN);

    /* One physical counter cannot be charged as both busy and quality. */
    cost_input_init(&input, 10U, 5U);
    input.medium_busy_valid = true;
    input.medium_busy_per_mille = 900U;
    input.medium_quality_valid = true;
    input.medium_quality_per_mille = 100U;
    input.medium_metrics_share_source = true;
    TEST_ASSERT(ucn_link_cost_resolve(&input, &result) == UCN_OK &&
                result.medium_busy_penalty == 70U &&
                result.medium_quality_penalty == 0U &&
                (result.invalid_metric_mask &
                 UCN_LINK_COST_DUPLICATE_MEDIUM_SOURCE) != 0U);

    TEST_ASSERT(ucn_link_cost_ewma_update(900U, 0U) == 675U);
    TEST_ASSERT(ucn_link_cost_ewma_update(675U, 0U) == 506U);
    TEST_ASSERT(ucn_link_cost_ewma_update(506U, 0U) == 379U);

    input.administrative_bias = 65;
    TEST_ASSERT(ucn_link_cost_resolve(&input, &result) == UCN_ERR_CONFIG &&
                !result.selectable &&
                result.exclusion == UCN_LINK_COST_EXCLUSION_CONFIG);
    return 0;
}
