use ucn_types::{Error, Result};

/// 验证端本地单调计时器的保守租约策略。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LeasePolicy {
    /// 本地单调时钟最大慢钟误差，单位 ppm，最大 1,000,000。
    pub local_timer_max_slow_ppm: u32,
    /// 一次量化可能漏计的完整 tick 上界，必须非零。
    pub local_timer_resolution_us: u64,
    /// 单次读取绝对误差上界；只有平台证明为零时才可填零。
    pub local_timer_read_uncertainty_us: u64,
    /// 是否已知读取误差；Unknown 不能伪装成零。
    pub timer_read_uncertainty_known: bool,
    /// 产品允许的最大本地租约长度，必须非零。
    pub local_policy_max_lease_us: u64,
}

/// 从本地锁存的 Challenge 起点构造保守半开租约截止期。
///
/// # Errors
///
/// 未知/非法策略、算术溢出、裕量耗尽或所得有效时长为零时失败关闭。
pub fn lease_deadline_build(
    challenge_started_local_us: u64,
    max_remaining_lease_us: u64,
    policy: LeasePolicy,
) -> Result<u64> {
    if max_remaining_lease_us == 0
        || policy.local_timer_resolution_us == 0
        || !policy.timer_read_uncertainty_known
        || policy.local_policy_max_lease_us == 0
        || policy.local_timer_max_slow_ppm > 1_000_000
    {
        return Err(Error::Argument);
    }

    let ppm_product = max_remaining_lease_us
        .checked_mul(u64::from(policy.local_timer_max_slow_ppm))
        .ok_or(Error::Exhausted)?;
    let mut clock_margin = ppm_product / 1_000_000;
    if ppm_product % 1_000_000 != 0 {
        clock_margin = clock_margin.checked_add(1).ok_or(Error::Exhausted)?;
    }
    let read_margin = policy
        .local_timer_read_uncertainty_us
        .checked_mul(2)
        .ok_or(Error::Exhausted)?;
    let quantization_margin = policy
        .local_timer_resolution_us
        .checked_add(read_margin)
        .ok_or(Error::Exhausted)?;
    let safe_duration = max_remaining_lease_us
        .checked_sub(clock_margin)
        .and_then(|value| value.checked_sub(quantization_margin))
        .ok_or(Error::Timeout)?;
    let effective_duration = safe_duration.min(policy.local_policy_max_lease_us);
    if effective_duration == 0 {
        return Err(Error::Timeout);
    }
    let deadline = challenge_started_local_us
        .checked_add(effective_duration)
        .ok_or(Error::Exhausted)?;
    if deadline == 0 {
        return Err(Error::Exhausted);
    }
    Ok(deadline)
}

/// 判断可信本地时间是否仍位于半开租约区间内。
#[must_use]
pub const fn lease_is_live(now_us: u64, deadline_us: u64) -> bool {
    deadline_us != 0 && now_us < deadline_us
}

#[cfg(test)]
mod tests {
    use super::{LeasePolicy, lease_deadline_build, lease_is_live};
    use ucn_types::Error;

    const POLICY: LeasePolicy = LeasePolicy {
        local_timer_max_slow_ppm: 100,
        local_timer_resolution_us: 10,
        local_timer_read_uncertainty_us: 5,
        timer_read_uncertainty_known: true,
        local_policy_max_lease_us: 1_000_000,
    };

    #[test]
    fn deadline_deducts_every_margin_and_is_half_open() {
        let deadline = lease_deadline_build(1_000, 100_000, POLICY).unwrap();
        assert_eq!(deadline, 100_970);
        assert!(lease_is_live(deadline - 1, deadline));
        assert!(!lease_is_live(deadline, deadline));
    }

    #[test]
    fn quantization_can_consume_a_short_lease() {
        let quantized = LeasePolicy {
            local_timer_max_slow_ppm: 0,
            local_timer_resolution_us: 10_000,
            local_timer_read_uncertainty_us: 0,
            timer_read_uncertainty_known: true,
            local_policy_max_lease_us: 20_000,
        };
        assert_eq!(lease_deadline_build(100, 15_000, quantized), Ok(5_100));
        assert_eq!(
            lease_deadline_build(100, 10_000, quantized),
            Err(Error::Timeout)
        );
    }

    #[test]
    fn unknown_zero_and_overflow_inputs_fail_closed() {
        let mut policy = POLICY;
        policy.timer_read_uncertainty_known = false;
        assert_eq!(lease_deadline_build(0, 10, policy), Err(Error::Argument));
        assert_eq!(lease_deadline_build(0, 0, POLICY), Err(Error::Argument));
        policy = POLICY;
        policy.local_timer_read_uncertainty_us = u64::MAX;
        assert_eq!(lease_deadline_build(0, 100, policy), Err(Error::Exhausted));
        assert_eq!(
            lease_deadline_build(u64::MAX, 100_000, POLICY),
            Err(Error::Exhausted)
        );
    }
}
