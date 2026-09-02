#ifndef UCN_TIME_H
#define UCN_TIME_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Deadlines use modulo-2^32 millisecond clocks.  Signed-difference ordering
 * is unambiguous only while every relative duration stays within half of the
 * clock range.  Value zero remains the public "no deadline" sentinel. */
#define UCN_MAX_SAFE_DURATION_MS ((uint32_t)INT32_MAX)

/*
 * EN: Checks whether `duration` satisfies the wrap-safe time module's validity rules.
 * 中文：检查 `duration` 是否满足 回绕安全时间 模块的合法性规则。
 */
static inline bool ucn_duration_is_valid(uint32_t duration_ms)
{
    return duration_ms != 0U && duration_ms <= UCN_MAX_SAFE_DURATION_MS;
}

/*
 * EN: Derives `deadline_from_now` with the canonical wrap-safe time conversion rules.
 * 中文：按照规范的 回绕安全时间 转换规则推导 `deadline_from_now`。
 */
static inline uint32_t ucn_deadline_from_now(uint32_t now_ms,
                                             uint32_t duration_ms)
{
    uint32_t deadline_ms;

    if (!ucn_duration_is_valid(duration_ms)) {
        return 0U;
    }
    deadline_ms = now_ms + duration_ms;
    /* Preserve zero as the sentinel.  At the single natural-zero instant the
     * deadline is delayed by 1 ms rather than becoming silently infinite. */
    return deadline_ms == 0U ? 1U : deadline_ms;
}

/*
 * EN: Checks or removes expired `deadline_expired` state in wrap-safe time.
 * 中文：检查或移除 回绕安全时间 中已过期的 `deadline_expired` 状态。
 */
static inline bool ucn_deadline_expired(uint32_t now_ms,
                                        uint32_t deadline_ms)
{
    return deadline_ms != 0U && (int32_t)(now_ms - deadline_ms) >= 0;
}

/*
 * EN: Calculates `deadline_due_within` with bounded, deterministic wrap-safe time arithmetic.
 * 中文：使用有界且确定性的 回绕安全时间 算术计算 `deadline_due_within`。
 */
static inline bool ucn_deadline_due_within(uint32_t now_ms,
                                            uint32_t deadline_ms,
                                            uint32_t window_ms)
{
    return deadline_ms != 0U && ucn_duration_is_valid(window_ms) &&
           !ucn_deadline_expired(now_ms, deadline_ms) &&
           (uint32_t)(deadline_ms - now_ms) <= window_ms;
}

/*
 * EN: Checks a wrap-safe elapsed interval against a validated duration.
 * 中文：使用回绕安全算法检查已过时间是否达到合法时长。
 */
static inline bool ucn_elapsed_at_least(uint32_t now_ms,
                                        uint32_t since_ms,
                                        uint32_t interval_ms)
{
    return ucn_duration_is_valid(interval_ms) &&
           (uint32_t)(now_ms - since_ms) >= interval_ms;
}

#ifdef __cplusplus
}
#endif

#endif
