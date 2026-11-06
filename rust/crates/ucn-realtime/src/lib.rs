#![no_std]
#![forbid(unsafe_code)]
//! Optional time semantics with no dependency on Group or Cluster.
//! 可选时间语义，不依赖Group或Cluster。
#[cfg(feature = "network")]
mod codec;
#[cfg(feature = "network")]
pub use codec::*;
#[cfg(feature = "network")]
mod domain;
#[cfg(feature = "envelope")]
mod envelope;
#[cfg(feature = "network")]
mod rx;
#[cfg(feature = "network")]
mod sync;
#[cfg(feature = "network")]
mod timed_link;
#[cfg(feature = "network")]
pub use domain::*;
#[cfg(feature = "envelope")]
pub use envelope::*;
#[cfg(feature = "network")]
pub use rx::*;
#[cfg(feature = "network")]
pub use sync::*;
#[cfg(feature = "network")]
pub use timed_link::*;

use ucn_types::{Error, Result};

/// Complete conservative uncertainty components in microseconds.
/// 完整保守误差分量，微秒。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Uncertainty {
    /// Timer quantization/read bound; 时钟量化/读取界。
    pub timer: Option<u32>,
    /// Physical link capture bound; 物理链路打点界。
    pub link: Option<u32>,
    /// Filter residual bound; 滤波残差界。
    pub filter: Option<u32>,
    /// Integer rounding bound; 整数舍入界。
    pub rounding: Option<u32>,
    /// Sensor capture bound; 传感器锁存界。
    pub capture: Option<u32>,
    /// Proven path asymmetry bound; 已证明路径非对称界。
    pub asymmetry: Option<u32>,
}
impl Uncertainty {
    /// Sum all known positive bounds with checked arithmetic.
    /// checked求和全部已知正误差界。
    /// # Errors
    /// Unknown, invalid zero or overflow fails closed.
    pub fn aggregate(self) -> Result<u32> {
        [
            self.timer,
            self.link,
            self.filter,
            self.rounding,
            self.capture,
            self.asymmetry,
        ]
        .into_iter()
        .try_fold(0u32, |sum, value| {
            let value = value.ok_or(Error::Policy)?;
            if value == 0 {
                return Err(Error::Argument);
            }
            sum.checked_add(value).ok_or(Error::Argument)
        })
    }
}
/// Local sample capture without network-domain state; 无网络域状态本地采样。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LocalStamp {
    /// Monotonic local time; 本地单调时刻。
    pub time_us: u64,
    /// Credible capture bound; 可信锁存误差。
    pub bound_us: u32,
    /// Sensor generation; 传感器代际。
    pub generation: u32,
}
/// Capture product's bounded local timestamp; 记录产品有界本地时间戳。
/// # Errors
/// Unknown/zero clock, bound or generation rejects.
#[cfg(feature = "local")]
pub fn local_stamp(time_us: u64, bound_us: u32, generation: u32) -> Result<LocalStamp> {
    if time_us == 0 || bound_us == 0 || generation == 0 {
        return Err(Error::Argument);
    }
    Ok(LocalStamp {
        time_us,
        bound_us,
        generation,
    })
}
