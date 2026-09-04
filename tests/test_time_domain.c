#include "ucn/ucn_time_domain.h"

#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "assertion failed: %s:%d: %s\n",          \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (0)

/* EN: Returns a bounded three-sample Domain configuration.
 * 中文：返回需要三个样本锁定的有界时间域配置。 */
static ucn_time_domain_config_t domain_config(void)
{
    ucn_time_domain_config_t config;

    (void)memset(&config, 0, sizeof(config));
    config.clock_domain_id = 10U;
    config.master_node_id = 1U;
    config.master_session_id = 100U;
    config.domain_generation = 7U;
    config.lock_sample_count = 3U;
    config.sync_timeout_us = UINT64_C(1000000);
    config.max_holdover_us = UINT64_C(2000000);
    config.max_offset_jump_us = 1000U;
    config.max_slew_per_sample_us = 100U;
    config.max_rate_ppb = 100000U;
    config.oscillator_uncertainty_ppb = 20000U;
    config.oscillator_uncertainty_known = true;
    return config;
}

/* EN: Returns one sample bound to the canonical Domain identity.
 * 中文：返回一条绑定规范时间域身份的样本。 */
static ucn_time_sync_sample_t sample_at(uint64_t local_us,
                                        int64_t offset_us,
                                        ucn_time_sample_kind_t kind)
{
    ucn_time_sync_sample_t sample;

    (void)memset(&sample, 0, sizeof(sample));
    sample.kind = kind;
    sample.clock_domain_id = 10U;
    sample.master_node_id = 1U;
    sample.master_session_id = 100U;
    sample.domain_generation = 7U;
    sample.local_sample_us = local_us;
    sample.offset_us = offset_us;
    sample.mean_path_delay_us = 50U;
    sample.uncertainty_us = 20U;
    sample.uncertainty_known = kind == UCN_TIME_SAMPLE_VALID_SYNC;
    return sample;
}

/* EN: Verifies diagnostic/effective sample separation and median locking.
 * 中文：验证诊断/有效样本分流以及中位数锁定。 */
static bool test_acquire_and_diagnostic_separation(void)
{
    ucn_time_domain_config_t config = domain_config();
    ucn_time_domain_t domain;
    ucn_time_sync_sample_t sample;
    ucn_realtime_clock_view_t view;

    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_OK);
    sample = sample_at(1000U, 500U, UCN_TIME_SAMPLE_DIAGNOSTIC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_ACQUIRING &&
                domain.consecutive_valid_samples == 0U &&
                domain.stats.diagnostic_samples == 1U);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, 1000U, &view) ==
                UCN_ERR_STATE);

    sample = sample_at(UINT64_C(1000000), 100U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    sample = sample_at(UINT64_C(2000000), 120U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    sample = sample_at(UINT64_C(3000000), 110U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_LOCKED &&
                domain.offset_us == 110 && domain.stats.valid_samples == 3U &&
                domain.stats.lock_transitions == 1U);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(3000000),
                                               &view) == UCN_OK);
    TEST_ASSERT(view.available && !view.holdover &&
                view.clock_domain_id == 10U &&
                view.domain_generation == 7U &&
                view.domain_time_us == UINT64_C(3000110));
    return true;
}

/* EN: Verifies HOLDOVER growth, expiry, and fresh-sample reacquisition.
 * 中文：验证 HOLDOVER 误差增长、失效和新样本重获锁定。 */
static bool test_holdover_and_unsynced(void)
{
    ucn_time_domain_config_t config = domain_config();
    ucn_time_domain_t domain;
    ucn_time_sync_sample_t sample;
    ucn_realtime_clock_view_t view;

    config.lock_sample_count = 2U;
    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_OK);
    sample = sample_at(UINT64_C(1000000), 100U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    sample = sample_at(UINT64_C(1000100), 100U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(2000100),
                                               &view) == UCN_OK);
    TEST_ASSERT(view.holdover && view.holdover_age_us == 0U &&
                view.uncertainty_us == 40U);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(3000099),
                                               &view) == UCN_OK);
    TEST_ASSERT(view.holdover && view.holdover_age_us == 999999U);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(4000100),
                                               &view) == UCN_ERR_STATE);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_UNSYNCED &&
                domain.consecutive_valid_samples == 0U &&
                domain.sample_count == 0U && domain.sample_cursor == 0U &&
                !domain.has_valid_sample && domain.has_sample_high_water &&
                domain.has_output &&
                domain.last_sample_local_us == UINT64_C(1000100) &&
                domain.last_output_local_us == UINT64_C(3000099) &&
                domain.last_output_domain_us == view.domain_time_us &&
                domain.offset_us == 0 && domain.rate_ppb == 0);

    sample = sample_at(UINT64_C(1000100), 900U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) ==
                UCN_ERR_STATE);

    sample = sample_at(UINT64_C(5000000), 900U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_ACQUIRING &&
                domain.offset_us == 900 && domain.sample_count == 1U);
    sample = sample_at(UINT64_C(5000100), 900U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_LOCKED &&
                domain.offset_us == 900 && domain.sample_count == 2U);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(5000100),
                                               &view) == UCN_OK);
    TEST_ASSERT(view.domain_time_us >= domain.last_output_domain_us);
    return true;
}

/* EN: Proves UNSYNCED cannot republish an older value in one generation.
 * 中文：证明同一 generation 经 UNSYNCED 后不能重新发布更小时间。 */
static bool test_same_generation_output_high_water(void)
{
    ucn_time_domain_config_t config = domain_config();
    ucn_time_domain_t domain;
    ucn_time_sync_sample_t sample;
    ucn_realtime_clock_view_t old_view;
    ucn_realtime_clock_view_t rejected_view;
    ucn_realtime_clock_view_t before;
    uint32_t lock_transitions;

    config.lock_sample_count = 1U;
    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_OK);
    sample = sample_at(UINT64_C(10000), 1000,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(10000),
                                               &old_view) == UCN_OK);
    TEST_ASSERT(old_view.domain_time_us == UINT64_C(11000));
    TEST_ASSERT(ucn_time_domain_step(
                    &domain, UINT64_C(10000) + config.sync_timeout_us +
                                 config.max_holdover_us) == UCN_OK);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_UNSYNCED &&
                domain.has_output && domain.has_sample_high_water);
    lock_transitions = domain.stats.lock_transitions;

    sample = sample_at(UINT64_C(5000000), -INT64_C(4998000),
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) ==
                UCN_ERR_STATE);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_FAULT &&
                domain.stats.lock_transitions == lock_transitions &&
                domain.last_output_domain_us == old_view.domain_time_us);
    (void)memset(&rejected_view, 0xA5, sizeof(rejected_view));
    before = rejected_view;
    TEST_ASSERT(ucn_time_domain_get_clock_view(
                    &domain, UINT64_C(5000000), &rejected_view) ==
                UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&rejected_view, &before, sizeof(rejected_view)) == 0 &&
                domain.phase == UCN_TIME_DOMAIN_FAULT &&
                domain.last_output_domain_us == old_view.domain_time_us);
    return true;
}

/* EN: Verifies Master restart and generation no-wrap behavior.
 * 中文：验证 Master 重启与 generation 禁止回绕。 */
static bool test_generation_and_rebind(void)
{
    ucn_time_domain_config_t config = domain_config();
    ucn_time_domain_t domain;
    uint32_t next = 0U;
    uint32_t before = 0xA5A5A5A5U;

    config.oscillator_uncertainty_known = false;
    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_ERR_ARGUMENT);
    config.oscillator_uncertainty_known = true;
    config.oscillator_uncertainty_ppb = 0U;
    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_ERR_ARGUMENT);
    config.oscillator_uncertainty_ppb = 20000U;

    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_OK);
    TEST_ASSERT(ucn_time_domain_rebind_master(&domain, 101U, 7U) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(ucn_time_domain_rebind_master(&domain, 101U, 8U) == UCN_OK);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_ACQUIRING &&
                domain.config.master_session_id == 101U &&
                domain.config.domain_generation == 8U &&
                !domain.has_valid_sample);
    TEST_ASSERT(ucn_time_domain_generation_next(0U, &next) == UCN_OK &&
                next == 1U);
    next = before;
    TEST_ASSERT(ucn_time_domain_generation_next(
                    UCN_REALTIME_DOMAIN_GENERATION_MAX, &next) ==
                UCN_ERR_EXHAUSTED);
    TEST_ASSERT(next == before);
    return true;
}

/* EN: Verifies fail-closed time reversal and offset jump behavior.
 * 中文：验证时间倒退与 offset 跳变的失败关闭行为。 */
static bool test_fault_boundaries(void)
{
    ucn_time_domain_config_t config = domain_config();
    ucn_time_domain_t domain;
    ucn_time_sync_sample_t sample;
    ucn_realtime_clock_view_t view;
    ucn_realtime_clock_view_t before;

    config.lock_sample_count = 1U;
    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_OK);
    sample = sample_at(UINT64_C(1000000), 100U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(1000100),
                                               &view) == UCN_OK);
    (void)memset(&view, 0x5A, sizeof(view));
    before = view;
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(999999),
                                               &view) == UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&view, &before, sizeof(view)) == 0 &&
                domain.phase == UCN_TIME_DOMAIN_FAULT);

    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_OK);
    sample = sample_at(UINT64_C(1000000), 100U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    sample = sample_at(UINT64_C(2000000), 1200U,
                       UCN_TIME_SAMPLE_VALID_SYNC);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) ==
                UCN_ERR_STATE);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_FAULT);
    return true;
}

/* EN: Rejects caller-visible state corruption before indexing the sample ring.
 * 中文：在索引样本环形区前拒绝调用者可见状态损坏。 */
static bool test_corrupt_state_is_fail_closed(void)
{
    ucn_time_domain_config_t config = domain_config();
    ucn_time_domain_t domain;
    ucn_time_domain_t before;
    ucn_time_sync_sample_t sample = sample_at(
        UINT64_C(1000000), 100, UCN_TIME_SAMPLE_VALID_SYNC);
    ucn_realtime_clock_view_t view;
    ucn_realtime_clock_view_t view_before;

    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_OK);
    domain.sample_cursor = (uint8_t)UCN_TIME_DOMAIN_SAMPLE_WINDOW;
    before = domain;
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) ==
                UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&domain, &before, sizeof(domain)) == 0);
    (void)memset(&view, 0xA5, sizeof(view));
    view_before = view;
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, 1U, &view) ==
                UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&view, &view_before, sizeof(view)) == 0);
    return true;
}

/* EN: Runs all RT-03 focused tests.
 * 中文：运行全部 RT-03 定向测试。 */
int main(void)
{
    if (!test_acquire_and_diagnostic_separation() ||
        !test_holdover_and_unsynced() ||
        !test_same_generation_output_high_water() ||
        !test_generation_and_rebind() ||
        !test_fault_boundaries() || !test_corrupt_state_is_fail_closed()) {
        return 1;
    }
    (void)puts("ucn time domain tests passed");
    return 0;
}
