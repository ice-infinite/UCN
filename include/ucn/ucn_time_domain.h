#ifndef UCN_TIME_DOMAIN_H
#define UCN_TIME_DOMAIN_H

/* Optional fixed-memory UCN Time Domain model.
 * 可选的固定内存 UCN 时间域模型。 */

#include "ucn/ucn_realtime_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UCN_TIME_DOMAIN_SAMPLE_WINDOW
#define UCN_TIME_DOMAIN_SAMPLE_WINDOW ((size_t)5U)
#endif

typedef char ucn_time_domain_sample_window_must_be_1_to_8[
    UCN_TIME_DOMAIN_SAMPLE_WINDOW >= 1U &&
    UCN_TIME_DOMAIN_SAMPLE_WINDOW <= 8U ? 1 : -1];

typedef uint8_t ucn_time_domain_phase_t;
enum {
    UCN_TIME_DOMAIN_UNSYNCED = 0U,
    UCN_TIME_DOMAIN_ACQUIRING = 1U,
    UCN_TIME_DOMAIN_LOCKED = 2U,
    UCN_TIME_DOMAIN_HOLDOVER = 3U,
    UCN_TIME_DOMAIN_FAULT = 4U
};

typedef uint8_t ucn_time_sample_kind_t;
enum {
    UCN_TIME_SAMPLE_DIAGNOSTIC = 0U,
    UCN_TIME_SAMPLE_VALID_SYNC = 1U
};

typedef struct ucn_time_domain_config {
    uint16_t clock_domain_id;
    ucn_node_id_t master_node_id;
    ucn_session_id_t master_session_id;
    uint32_t domain_generation;
    uint8_t lock_sample_count;
    uint64_t sync_timeout_us;
    uint64_t max_holdover_us;
    uint32_t max_offset_jump_us;
    uint32_t max_slew_per_sample_us;
    uint32_t max_rate_ppb;
    uint32_t oscillator_uncertainty_ppb;
    bool oscillator_uncertainty_known;
} ucn_time_domain_config_t;

typedef struct ucn_time_sync_sample {
    ucn_time_sample_kind_t kind;
    uint16_t clock_domain_id;
    ucn_node_id_t master_node_id;
    ucn_session_id_t master_session_id;
    uint32_t domain_generation;
    uint64_t local_sample_us;
    int64_t offset_us;
    uint64_t mean_path_delay_us;
    uint32_t uncertainty_us;
    bool uncertainty_known;
} ucn_time_sync_sample_t;

typedef struct ucn_time_domain_stats {
    uint32_t valid_samples;
    uint32_t diagnostic_samples;
    uint32_t rejected_samples;
    uint32_t lock_transitions;
    uint32_t holdover_transitions;
    uint32_t unsynced_transitions;
    uint32_t faults;
} ucn_time_domain_stats_t;

typedef struct ucn_time_domain {
    ucn_time_domain_config_t config;
    ucn_time_domain_stats_t stats;
    int64_t offset_samples[UCN_TIME_DOMAIN_SAMPLE_WINDOW];
    int64_t offset_us;
    int32_t rate_ppb;
    uint64_t rate_reference_local_us;
    uint64_t last_sample_local_us;
    uint64_t last_output_local_us;
    uint64_t last_output_domain_us;
    uint32_t base_uncertainty_us;
    uint32_t current_uncertainty_us;
    uint8_t sample_count;
    uint8_t sample_cursor;
    uint8_t consecutive_valid_samples;
    ucn_time_domain_phase_t phase;
    bool initialized;
    bool has_valid_sample;
    /* EN: Remains set across UNSYNCED acquisition resets in the same
     * generation so an older local sample cannot be accepted again.
     * 中文：同 generation 进入 UNSYNCED 并清理采集状态后仍保持，用于拒绝
     * 再次接受更旧的本地样本。 */
    bool has_sample_high_water;
    bool has_output;
} ucn_time_domain_t;

/* EN: Validates a bounded Time Domain configuration.
 * 中文：校验有界的时间域配置。 */
bool ucn_time_domain_config_is_valid(const ucn_time_domain_config_t *config);

/* EN: Initializes one caller-owned Domain in ACQUIRING state.
 * 中文：在 ACQUIRING 状态初始化一个调用者拥有的时间域。 */
ucn_result_t ucn_time_domain_init(ucn_time_domain_t *domain,
                                  const ucn_time_domain_config_t *config);

/* EN: Rebinds a restarted authenticated Master with a strictly newer
 * generation and clears all old samples/replay state.
 * 中文：使用严格更新的 generation 绑定重启后的认证 Master，并清除旧样本。 */
ucn_result_t ucn_time_domain_rebind_master(
    ucn_time_domain_t *domain,
    ucn_session_id_t master_session_id,
    uint32_t domain_generation);

/* EN: Adds either an effective sync sample or a diagnostic-only sample.
 * 中文：加入有效同步样本或仅诊断样本。 */
ucn_result_t ucn_time_domain_ingest_sample(
    ucn_time_domain_t *domain,
    const ucn_time_sync_sample_t *sample);

/* EN: Advances timeout/HOLDOVER state without reading any global clock.
 * 中文：在不读取全局时钟的前提下推进超时与 HOLDOVER 状态。 */
ucn_result_t ucn_time_domain_step(ucn_time_domain_t *domain,
                                  uint64_t local_monotonic_us);

/* EN: Produces a monotonic Domain time view for Endpoint policy.
 * Failure leaves output unchanged.
 * 中文：为 Endpoint 策略生成单调时间域视图；失败时输出不写回。 */
ucn_result_t ucn_time_domain_get_clock_view(
    ucn_time_domain_t *domain,
    uint64_t local_monotonic_us,
    ucn_realtime_clock_view_t *view);

/* EN: Returns the strictly next no-wrap generation.
 * 中文：返回严格递增且不回绕的下一 generation。 */
ucn_result_t ucn_time_domain_generation_next(uint32_t current_generation,
                                             uint32_t *next_generation);

#ifdef __cplusplus
}
#endif

#endif
