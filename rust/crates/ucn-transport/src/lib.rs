#![no_std]
#![forbid(unsafe_code)]

//! Bounded Reliable and Transfer state, independent of routing and Driver Owners.
//! 有界可靠传输与分片状态，不调用路由或Driver Owner。
//! Coordinator supplies verified current facts; these low-level APIs do not authenticate bytes.
//! 低层接口消费Coordinator已验证事实，本模块不自行宣称认证输入。

mod codec;
#[cfg(feature = "transfer")]
mod control;
#[cfg(feature = "transfer")]
mod parent;
#[cfg(feature = "transfer")]
mod publication;
#[cfg(feature = "reliable")]
mod reliable;
#[cfg(feature = "transfer")]
mod transfer;

pub use codec::*;
#[cfg(feature = "transfer")]
pub use control::*;
#[cfg(feature = "transfer")]
pub use parent::*;
#[cfg(feature = "transfer")]
pub use publication::*;
#[cfg(feature = "reliable")]
pub use reliable::*;
#[cfg(feature = "transfer")]
pub use transfer::*;

use ucn_types::{Error, OriginSecurity, Result};

/// Exact identity/address binding; 精确身份地址绑定。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Binding {
    /// Canonical principal; 原始身份。
    pub principal: [u8; 16],
    /// Bound address; 已绑定地址。
    pub address: u32,
    /// Binding generation; 绑定代际。
    pub generation: u32,
}
impl Binding {
    /// Validate a non-reserved binding; 验证非保留绑定。
    /// # Errors
    /// Zero/erased addresses or missing identities are rejected.
    pub fn validate(self) -> Result<()> {
        if self.address == 0
            || self.address == u32::MAX
            || self.generation == 0
            || self.principal == [0; 16]
        {
            Err(Error::Argument)
        } else {
            Ok(())
        }
    }
}

/// Frozen end-to-end security domain, supplied by Coordinator.
/// Coordinator提供的冻结端到端安全域。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SecurityDomain {
    /// Session generation; 会话代际。
    pub session: u32,
    /// Key generation; 密钥代际。
    pub key: u32,
    /// Endpoint policy generation; 策略代际。
    pub policy: u32,
    /// Origin level; Origin保护。
    pub origin: OriginSecurity,
    /// Exact context fingerprint; 精确Context指纹。
    pub fingerprint: [u8; 16],
    /// O0 permitted only for public telemetry on trusted transport.
    /// 只有公开遥测且受信承载才允许O0。
    pub public_trusted_o0: bool,
}
impl SecurityDomain {
    /// Validate the admitted security domain; 验证已准入安全域。
    /// # Errors
    /// Unauthorized O0 or incomplete protected context fails closed.
    pub fn validate(self) -> Result<()> {
        if self.policy == 0 || self.fingerprint == [0; 16] {
            return Err(Error::Access);
        }
        match self.origin {
            OriginSecurity::O0 if self.public_trusted_o0 && self.session == 0 && self.key == 0 => {
                Ok(())
            }
            OriginSecurity::O1 | OriginSecurity::O2 if self.session != 0 && self.key != 0 => Ok(()),
            _ => Err(Error::Access),
        }
    }
}

/// Canonical Reliable transaction key; 可靠事务完整键。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReliableKey {
    /// Realm; Realm域。
    pub realm: u32,
    /// Original source; 原始来源。
    pub source: Binding,
    /// Final destination; 最终目标。
    pub destination: Binding,
    /// Original service; 原Service。
    pub service: u16,
    /// Immutable Origin Sequence; 不变Origin序号。
    pub sequence: u32,
    /// End-to-end security; 端到端安全域。
    pub security: SecurityDomain,
}
impl ReliableKey {
    pub(crate) fn validate(self) -> Result<()> {
        self.source.validate()?;
        self.destination.validate()?;
        self.security.validate()?;
        if self.realm == 0
            || self.service == 0
            || self.service == u16::MAX
            || self.sequence == 0
            || self.source == self.destination
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// Exact transport acknowledgement; 精确交付ACK。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DeliveryAck {
    /// Original service; 原Service。
    pub service: u16,
    /// Original Origin Sequence; 原序号。
    pub sequence: u32,
    /// Frozen status byte; 冻结状态字节。
    pub status: u8,
    /// Remaining receive credit; 接收credit。
    pub credit: u8,
}

/// Actual submission disposition; 真实Driver提交结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Submission {
    /// No Driver ownership; Driver未持有。
    NotSubmitted,
    /// Driver accepted; Driver接纳。
    Submitted,
    /// Unknown external side effects; 外部副作用未知。
    InDoubt,
}

/// Validate known monotonic time and compute a half-open deadline.
/// 已知单调时钟与半开时限。
pub(crate) fn deadline(now: u64, duration: u64) -> Result<u64> {
    if now == 0 || duration == 0 || duration > i64::MAX as u64 {
        return Err(Error::Argument);
    }
    now.checked_add(duration).ok_or(Error::Argument)
}
