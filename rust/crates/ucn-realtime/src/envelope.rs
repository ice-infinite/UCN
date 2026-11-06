use ucn_types::{Error, Result};
/// Exact Timed Payload bytes; 精确Timed载荷字节数。
pub const ENVELOPE_BYTES: usize = 16;
/// Timed mode; 时间模式。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TimedMode {
    /// Local diagnostics only; 仅本地诊断。
    Local = 1,
    /// Verified Domain stamp; 可验证Domain采样。
    Synced = 2,
    /// Half-open age limit; 半开年龄限制。
    Deadline = 3,
}
/// C/Rust exact 16B Envelope; C/Rust精确16BEnvelope。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Envelope {
    /// Payload mode; 模式。
    pub mode: TimedMode,
    /// Sender uncertainty class; 发送端误差Class。
    pub uncertainty_class: u8,
    /// Hardware sensor capture; 硬件传感器打点。
    pub hardware_capture: bool,
    /// Network time valid; 网络时间有效。
    pub domain_valid: bool,
    /// Sender reports Holdover; 发送端Holdover。
    pub holdover: bool,
    /// Exact clock domain; 精确时间域。
    pub domain: u16,
    /// No-wrap generation; 不回绕代际。
    pub generation: u32,
    /// Sensor Domain time; 传感器Domain时刻。
    pub capture_us: u64,
}
impl Envelope {
    /// Validate the complete layout contract; 验证完整布局。
    /// # Errors
    /// Local must not claim network time; unknown synced bounds reject.
    pub fn validate(self) -> Result<()> {
        if self.mode == TimedMode::Local {
            if self.domain != 0
                || self.generation != 0
                || self.domain_valid
                || self.holdover
                || self.uncertainty_class != 31
            {
                return Err(Error::Argument);
            }
        } else if self.domain == 0
            || self.domain == u16::MAX
            || self.generation == 0
            || self.generation > 0x7fff_ffff
            || !self.domain_valid
            || self.uncertainty_class >= 31
            || self.capture_us == 0
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}
/// Conservative ceil-log2 encoding; 保守ceil-log2编码。
/// # Errors
/// Zero or unrepresentable bound rejects; unknown is class31.
pub fn uncertainty_class(bound: u32) -> Result<u8> {
    if bound == 0 {
        return Err(Error::Argument);
    }
    let power = 32 - (bound - 1).leading_zeros();
    let class = u8::try_from(power).map_err(|_| Error::Argument)?;
    if class >= 31 {
        return Err(Error::Argument);
    }
    Ok(class)
}
/// Decode source bound; 解码发送端误差界。
/// # Errors
/// Reserved/unknown class rejects.
pub fn uncertainty_bound(class: u8) -> Result<u32> {
    if class >= 31 {
        return Err(Error::Policy);
    }
    Ok(1u32 << class)
}
/// Encode only after all preflight checks; 全部预检后编码。
/// # Errors
/// Invalid Envelope or short buffer leaves output unchanged.
pub fn encode_envelope(value: Envelope, out: &mut [u8]) -> Result<usize> {
    value.validate()?;
    if out.len() < 16 {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 16];
    b[0] = 0x10 | value.mode as u8;
    b[1] = (value.uncertainty_class << 3)
        | (u8::from(value.hardware_capture) * 4)
        | (u8::from(value.domain_valid) * 2)
        | u8::from(value.holdover);
    b[2..4].copy_from_slice(&value.domain.to_be_bytes());
    b[4..8].copy_from_slice(&value.generation.to_be_bytes());
    b[8..16].copy_from_slice(&value.capture_us.to_be_bytes());
    out[..16].copy_from_slice(&b);
    Ok(16)
}
/// Decode exact length/version/reserved bits; 精确解码长度/版本/保留位。
/// # Errors
/// Unknown layout rejects.
pub fn decode_envelope(b: &[u8]) -> Result<Envelope> {
    if b.len() != 16 || b[0] & 0xfc != 0x10 {
        return Err(Error::Malformed);
    }
    let mode = match b[0] & 3 {
        1 => TimedMode::Local,
        2 => TimedMode::Synced,
        3 => TimedMode::Deadline,
        _ => return Err(Error::Malformed),
    };
    let value = Envelope {
        mode,
        uncertainty_class: b[1] >> 3,
        hardware_capture: b[1] & 4 != 0,
        domain_valid: b[1] & 2 != 0,
        holdover: b[1] & 1 != 0,
        domain: u16::from_be_bytes([b[2], b[3]]),
        generation: u32::from_be_bytes(b[4..8].try_into().map_err(|_| Error::Malformed)?),
        capture_us: u64::from_be_bytes(b[8..16].try_into().map_err(|_| Error::Malformed)?),
    };
    value.validate().map_err(|_| Error::Malformed)?;
    Ok(value)
}
/// REQUIRED/PREFERRED endpoint gate; REQUIRED/PREFERRED端点门。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimedPolicy {
    /// Required trusted network time; 必须可信网络时间。
    pub required: bool,
    /// Combined sender+receiver uncertainty maximum; 组合误差上限。
    pub max_uncertainty: u32,
    /// Local endpoint age maximum; 本地端点最大年龄。
    pub max_age: u64,
    /// Explicit trust of remote self-checked Holdover; 显式信任远端Holdover自检。
    pub allow_remote_holdover: bool,
}
/// Trusted local clock at actual acceptance/execution; 实际准入/执行时可信本地Clock。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ClockView {
    /// Domain ID; `DomainID`。
    pub domain: u16,
    /// Generation; 代际。
    pub generation: u32,
    /// Domain time; Domain时刻。
    pub now: u64,
    /// Receiver conservative uncertainty; 接收端保守误差。
    pub uncertainty: u32,
    /// Local Holdover; 本地Holdover。
    pub holdover: bool,
}
/// Immutable admission, to be rechecked before side effects; 副作用前必须复验的准入。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimedAdmission {
    envelope: Envelope,
    policy: TimedPolicy,
    guard_max_age: Option<u64>,
}
/// Validate E2E+ACL, future-skew, combined uncertainty and strict Deadline.
/// 验证E2E+ACL、未来偏差、组合误差及严格Deadline。
/// # Errors
/// Failure has no admission result and cannot be bypassed by Guard uptime.
pub fn prequeue(
    envelope: Envelope,
    clock: ClockView,
    policy: TimedPolicy,
    e2e: bool,
    acl: bool,
    guard_max_age: Option<u64>,
) -> Result<TimedAdmission> {
    check(envelope, clock, policy, e2e, acl, guard_max_age)?;
    Ok(TimedAdmission {
        envelope,
        policy,
        guard_max_age,
    })
}
/// Recheck at the actual application/executor boundary; 实际业务/执行器边界复验。
/// # Errors
/// Old domain, expired deadline or missing E2E/ACL rejects.
pub fn preexecute(admission: TimedAdmission, clock: ClockView, e2e: bool, acl: bool) -> Result<()> {
    check(
        admission.envelope,
        clock,
        admission.policy,
        e2e,
        acl,
        admission.guard_max_age,
    )
}
fn check(
    e: Envelope,
    c: ClockView,
    p: TimedPolicy,
    e2e: bool,
    acl: bool,
    guard: Option<u64>,
) -> Result<()> {
    e.validate()?;
    if !e2e || !acl {
        return Err(Error::Access);
    }
    if c.domain != e.domain
        || c.generation != e.generation
        || c.now == 0
        || c.uncertainty == 0
        || e.mode == TimedMode::Local
    {
        return Err(Error::State);
    }
    if e.holdover && (p.required || !p.allow_remote_holdover) || p.required && c.holdover {
        return Err(Error::Policy);
    }
    let combined = uncertainty_bound(e.uncertainty_class)?
        .checked_add(c.uncertainty)
        .ok_or(Error::Argument)?;
    if combined > p.max_uncertainty {
        return Err(Error::Policy);
    }
    if e.capture_us > c.now && e.capture_us - c.now > u64::from(combined) {
        return Err(Error::Argument);
    }
    let age = c
        .now
        .saturating_sub(e.capture_us)
        .checked_add(u64::from(combined))
        .ok_or(Error::Argument)?;
    let max_age = guard.map_or(p.max_age, |value| value.min(p.max_age));
    if e.mode == TimedMode::Deadline && age >= max_age {
        return Err(Error::Timeout);
    }
    Ok(())
}
