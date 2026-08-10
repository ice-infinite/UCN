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

static inline bool ucn_duration_is_valid(uint32_t duration_ms)
{
    return duration_ms != 0U && duration_ms <= UCN_MAX_SAFE_DURATION_MS;
}

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

static inline bool ucn_deadline_expired(uint32_t now_ms,
                                        uint32_t deadline_ms)
{
    return deadline_ms != 0U && (int32_t)(now_ms - deadline_ms) >= 0;
}

static inline bool ucn_deadline_due_within(uint32_t now_ms,
                                            uint32_t deadline_ms,
                                            uint32_t window_ms)
{
    return deadline_ms != 0U && ucn_duration_is_valid(window_ms) &&
           !ucn_deadline_expired(now_ms, deadline_ms) &&
           (uint32_t)(deadline_ms - now_ms) <= window_ms;
}

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
