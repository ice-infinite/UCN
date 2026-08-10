#include <string.h>

#include "test_support.h"
#include "ucn/ucn_adapter.h"

#define HELLO_SIM_NODES ((size_t)100U)
#define HELLO_SIM_DURATION_MS UINT32_C(15000)
#define HELLO_SIM_10MS_BUCKETS ((size_t)(HELLO_SIM_DURATION_MS / 10U + 1U))
#define HELLO_SIM_100MS_BUCKETS ((size_t)(HELLO_SIM_DURATION_MS / 100U + 1U))

typedef struct hello_sim_metrics {
    uint32_t total_hellos;
    uint16_t peak_10_ms;
    uint16_t peak_100_ms;
    uint32_t admission_p50_ms;
    uint32_t admission_p95_ms;
} hello_sim_metrics_t;

static const ucn_adapter_hello_config_t HELLO_STOP_CONFIG = {
    true,
    UCN_ADAPTER_HELLO_ADMITTED_POLICY_STOP,
    2U,
    0U,
    UINT32_C(100),
    UINT32_C(100),
    UINT32_C(50),
    UINT32_C(200),
    UINT32_C(800),
    0U
};

static void sort_u32(uint32_t *values, size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        const uint32_t value = values[index];
        size_t cursor = index;

        while (cursor > 0U && values[cursor - 1U] > value) {
            values[cursor] = values[cursor - 1U];
            --cursor;
        }
        values[cursor] = value;
    }
}

static bool run_hello_simulation(bool jitter_enabled,
                                 uint32_t seed,
                                 hello_sim_metrics_t *metrics)
{
    ucn_adapter_hello_scheduler_t schedulers[HELLO_SIM_NODES];
    uint8_t hello_counts[HELLO_SIM_NODES];
    uint32_t admission_pending_ms[HELLO_SIM_NODES];
    uint32_t admission_times_ms[HELLO_SIM_NODES];
    bool admitted[HELLO_SIM_NODES];
    uint16_t buckets_10_ms[HELLO_SIM_10MS_BUCKETS];
    uint16_t buckets_100_ms[HELLO_SIM_100MS_BUCKETS];
    ucn_adapter_hello_config_t config = HELLO_STOP_CONFIG;
    size_t admitted_count = 0U;
    size_t index;
    uint32_t now_ms;

    if (metrics == NULL) {
        return false;
    }
    (void)memset(schedulers, 0, sizeof(schedulers));
    (void)memset(hello_counts, 0, sizeof(hello_counts));
    (void)memset(admission_pending_ms, 0, sizeof(admission_pending_ms));
    (void)memset(admission_times_ms, 0, sizeof(admission_times_ms));
    (void)memset(admitted, 0, sizeof(admitted));
    (void)memset(buckets_10_ms, 0, sizeof(buckets_10_ms));
    (void)memset(buckets_100_ms, 0, sizeof(buckets_100_ms));
    (void)memset(metrics, 0, sizeof(*metrics));

    if (jitter_enabled) {
        config.initial_jitter_max_ms = UINT32_C(400);
        config.fast_retry_interval_ms = UINT32_C(200);
        config.retry_jitter_permille = 150U;
        config.backoff_initial_ms = UINT32_C(400);
        config.backoff_max_ms = UINT32_C(3200);
    }
    for (index = 0U; index < HELLO_SIM_NODES; ++index) {
        if (ucn_adapter_hello_scheduler_init(
                &schedulers[index], &config, (uint32_t)index + 1U,
                seed, 0U) != UCN_OK) {
            return false;
        }
    }

    for (now_ms = 0U; now_ms <= HELLO_SIM_DURATION_MS; ++now_ms) {
        for (index = 0U; index < HELLO_SIM_NODES; ++index) {
            bool hello_due = false;

            if (!admitted[index] && admission_pending_ms[index] != 0U &&
                now_ms >= admission_pending_ms[index]) {
                admitted[index] = true;
                admission_times_ms[index] = now_ms;
                ++admitted_count;
            }
            if (ucn_adapter_hello_scheduler_step(
                    &schedulers[index], now_ms, admitted[index],
                    &hello_due) != UCN_OK) {
                return false;
            }
            if (!hello_due) {
                continue;
            }

            ++metrics->total_hellos;
            ++buckets_10_ms[now_ms / 10U];
            ++buckets_100_ms[now_ms / 100U];
            ++hello_counts[index];
            /* Deterministically drop/reject the first 0..3 HELLO attempts.
             * A bounded reply delay models the final reciprocal admission. */
            if (hello_counts[index] >= (uint8_t)(index % 4U + 1U) &&
                admission_pending_ms[index] == 0U) {
                admission_pending_ms[index] =
                    now_ms + UINT32_C(20) + (uint32_t)(index % 11U);
            }
        }
        if (admitted_count == HELLO_SIM_NODES) {
            break;
        }
    }
    if (admitted_count != HELLO_SIM_NODES) {
        return false;
    }

    for (index = 0U; index < HELLO_SIM_10MS_BUCKETS; ++index) {
        if (buckets_10_ms[index] > metrics->peak_10_ms) {
            metrics->peak_10_ms = buckets_10_ms[index];
        }
    }
    for (index = 0U; index < HELLO_SIM_100MS_BUCKETS; ++index) {
        if (buckets_100_ms[index] > metrics->peak_100_ms) {
            metrics->peak_100_ms = buckets_100_ms[index];
        }
    }
    sort_u32(admission_times_ms, HELLO_SIM_NODES);
    metrics->admission_p50_ms = admission_times_ms[49U];
    metrics->admission_p95_ms = admission_times_ms[94U];
    return true;
}

int test_adapter_hello(void)
{
    ucn_adapter_hello_scheduler_t scheduler;
    ucn_adapter_hello_scheduler_t same_token;
    ucn_adapter_hello_scheduler_t other_token;
    ucn_adapter_hello_config_t slow_config = HELLO_STOP_CONFIG;
    ucn_adapter_hello_config_t token_config;
    ucn_adapter_hello_config_t disabled_config;
    ucn_adapter_hello_config_t invalid_config = HELLO_STOP_CONFIG;
    hello_sim_metrics_t plain_metrics;
    hello_sim_metrics_t jitter_metrics;
    hello_sim_metrics_t replay_metrics;
    bool hello_due = false;

    (void)memset(&scheduler, 0, sizeof(scheduler));
    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &scheduler, &HELLO_STOP_CONFIG, UINT32_C(0x1001),
                    UINT32_C(0x12345678), UINT32_C(1000)) == UCN_OK);
    TEST_ASSERT(scheduler.state == UCN_ADAPTER_HELLO_INITIAL_JITTER);
    TEST_ASSERT(scheduler.next_hello_ms == UINT32_C(1100));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(1099), false, &hello_due) == UCN_OK);
    TEST_ASSERT(!hello_due);
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(1100), false, &hello_due) == UCN_OK);
    TEST_ASSERT(hello_due);
    TEST_ASSERT(scheduler.state == UCN_ADAPTER_HELLO_FAST_RETRY);
    TEST_ASSERT(scheduler.next_hello_ms == UINT32_C(1150));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(1150), false, &hello_due) == UCN_OK);
    TEST_ASSERT(hello_due && scheduler.fast_retries_sent == 1U);
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(1200), false, &hello_due) == UCN_OK);
    TEST_ASSERT(hello_due);
    TEST_ASSERT(scheduler.state == UCN_ADAPTER_HELLO_BACKOFF);
    TEST_ASSERT(scheduler.next_hello_ms == UINT32_C(1400));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(1400), false, &hello_due) == UCN_OK);
    TEST_ASSERT(hello_due && scheduler.next_hello_ms == UINT32_C(1800));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(1800), false, &hello_due) == UCN_OK);
    TEST_ASSERT(hello_due && scheduler.backoff_interval_ms == UINT32_C(800) &&
                scheduler.next_hello_ms == UINT32_C(2600));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(2600), false, &hello_due) == UCN_OK);
    TEST_ASSERT(hello_due && scheduler.backoff_interval_ms == UINT32_C(800) &&
                scheduler.next_hello_ms == UINT32_C(3400));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(2601), true, &hello_due) == UCN_OK);
    TEST_ASSERT(!hello_due &&
                scheduler.state == UCN_ADAPTER_HELLO_ADMITTED_STOP &&
                scheduler.next_hello_ms == 0U);
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(2700), false, &hello_due) == UCN_OK);
    TEST_ASSERT(!hello_due &&
                scheduler.state == UCN_ADAPTER_HELLO_INITIAL_JITTER &&
                scheduler.next_hello_ms == UINT32_C(2800));
    TEST_ASSERT(ucn_adapter_hello_scheduler_get_stats(&scheduler)->hellos_due == 6U);
    TEST_ASSERT(ucn_adapter_hello_scheduler_get_stats(&scheduler)->
                    discovery_restarts == 1U);

    slow_config.admitted_policy = UCN_ADAPTER_HELLO_ADMITTED_POLICY_SLOW;
    slow_config.admitted_slow_interval_ms = UINT32_C(1000);
    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &scheduler, &slow_config, UINT32_C(0x2001),
                    UINT32_C(7), 0U) == UCN_OK);
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(10), true, &hello_due) == UCN_OK);
    TEST_ASSERT(!hello_due &&
                scheduler.state == UCN_ADAPTER_HELLO_ADMITTED_SLOW &&
                scheduler.next_hello_ms == UINT32_C(60));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(60), true, &hello_due) == UCN_OK);
    TEST_ASSERT(hello_due && scheduler.next_hello_ms == UINT32_C(1060));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(70), false, &hello_due) == UCN_OK);
    TEST_ASSERT(!hello_due &&
                scheduler.state == UCN_ADAPTER_HELLO_INITIAL_JITTER &&
                scheduler.next_hello_ms == UINT32_C(170));

    (void)memset(&disabled_config, 0, sizeof(disabled_config));
    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &scheduler, &disabled_config, UINT32_C(0x3001), 0U,
                    0U) == UCN_OK);
    TEST_ASSERT(scheduler.state == UCN_ADAPTER_HELLO_DISABLED);
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_MAX, false, &hello_due) == UCN_OK);
    TEST_ASSERT(!hello_due);
    TEST_ASSERT(ucn_adapter_hello_scheduler_restart(
                    &scheduler, UINT32_C(1), UINT32_C(100)) == UCN_OK);
    TEST_ASSERT(scheduler.state == UCN_ADAPTER_HELLO_DISABLED);

    invalid_config.initial_jitter_min_ms = 0U;
    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &scheduler, &invalid_config, UINT32_C(1), 0U, 0U) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &scheduler, &HELLO_STOP_CONFIG, 0U, 0U, 0U) ==
                UCN_ERR_ARGUMENT);

    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &scheduler, &HELLO_STOP_CONFIG, UINT32_C(0x4001),
                    UINT32_C(0x42), UINT32_MAX - UINT32_C(50)) == UCN_OK);
    TEST_ASSERT(scheduler.next_hello_ms == UINT32_C(49));
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(48), false, &hello_due) == UCN_OK);
    TEST_ASSERT(!hello_due);
    TEST_ASSERT(ucn_adapter_hello_scheduler_step(
                    &scheduler, UINT32_C(49), false, &hello_due) == UCN_OK);
    TEST_ASSERT(hello_due);

    token_config = slow_config;
    token_config.initial_jitter_max_ms = UINT32_C(400);
    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &scheduler, &token_config, UINT32_C(0x5001),
                    UINT32_C(0xCAFEBABE), 0U) == UCN_OK);
    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &same_token, &token_config, UINT32_C(0x5001),
                    UINT32_C(0xCAFEBABE), 0U) == UCN_OK);
    TEST_ASSERT(ucn_adapter_hello_scheduler_init(
                    &other_token, &token_config, UINT32_C(0x5002),
                    UINT32_C(0xCAFEBABE), 0U) == UCN_OK);
    TEST_ASSERT(scheduler.random_state == same_token.random_state);
    TEST_ASSERT(scheduler.next_hello_ms == same_token.next_hello_ms);
    TEST_ASSERT(scheduler.random_state != other_token.random_state);
    TEST_ASSERT(scheduler.next_hello_ms != other_token.next_hello_ms);
    TEST_ASSERT(ucn_adapter_hello_scheduler_restart(
                    &scheduler, UINT32_C(0x13579BDF), UINT32_C(500)) == UCN_OK);
    TEST_ASSERT(scheduler.state == UCN_ADAPTER_HELLO_INITIAL_JITTER);
    TEST_ASSERT(ucn_adapter_hello_scheduler_get_stats(&scheduler)->
                    discovery_restarts == 1U);

    TEST_ASSERT(run_hello_simulation(false, UINT32_C(0x5EED1234),
                                     &plain_metrics));
    TEST_ASSERT(run_hello_simulation(true, UINT32_C(0x5EED1234),
                                     &jitter_metrics));
    TEST_ASSERT(run_hello_simulation(true, UINT32_C(0x5EED1234),
                                     &replay_metrics));
    TEST_ASSERT(memcmp(&jitter_metrics, &replay_metrics,
                       sizeof(jitter_metrics)) == 0);
    TEST_ASSERT(plain_metrics.total_hellos == UINT32_C(250));
    TEST_ASSERT(jitter_metrics.total_hellos == UINT32_C(250));
    TEST_ASSERT(jitter_metrics.peak_10_ms < plain_metrics.peak_10_ms);
    TEST_ASSERT(jitter_metrics.peak_100_ms < plain_metrics.peak_100_ms);
    TEST_ASSERT(jitter_metrics.admission_p50_ms > 0U);
    TEST_ASSERT(jitter_metrics.admission_p95_ms >=
                jitter_metrics.admission_p50_ms);
    printf("HELLO sim scheduler_B=%u seed=0x5EED1234 plain peak10=%u peak100=%u "
           "jitter peak10=%u peak100=%u total=%lu p50=%lums p95=%lums\n",
           (unsigned)sizeof(ucn_adapter_hello_scheduler_t),
           (unsigned)plain_metrics.peak_10_ms,
           (unsigned)plain_metrics.peak_100_ms,
           (unsigned)jitter_metrics.peak_10_ms,
           (unsigned)jitter_metrics.peak_100_ms,
           (unsigned long)jitter_metrics.total_hellos,
           (unsigned long)jitter_metrics.admission_p50_ms,
           (unsigned long)jitter_metrics.admission_p95_ms);
    return 0;
}
