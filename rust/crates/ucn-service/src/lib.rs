#![no_std]
#![forbid(unsafe_code)]

//! Independent fixed-capacity Service Owners. 独立固定容量 Service 模块。

mod control;
pub use control::*;

#[cfg(feature = "operation")]
mod ids;
#[cfg(feature = "operation")]
mod operation;
#[cfg(feature = "qos")]
mod qos;
#[cfg(feature = "rpc")]
mod rpc;
#[cfg(feature = "operation")]
pub use ids::*;
#[cfg(feature = "operation")]
pub use operation::*;
#[cfg(feature = "qos")]
pub use qos::*;
#[cfg(feature = "rpc")]
pub use rpc::*;

use ucn_types::{Error, Result};

/// Complete Request/Operation correlation domain; 完整请求/Operation关联域。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ServiceKey {
    /// Realm; Realm域。
    pub realm: u32,
    /// Original client Principal; 原始Client身份。
    pub client: [u8; 16],
    /// Final server Principal; 最终Server身份。
    pub server: [u8; 16],
    /// Client address binding generation; Client绑定代际。
    pub client_generation: u32,
    /// Client address; Client地址。
    pub client_address: u32,
    /// Server address binding generation; Server绑定代际。
    pub server_generation: u32,
    /// Server address; Server地址。
    pub server_address: u32,
    /// Nonzero Operation ID; `非零OperationID`。
    pub operation: u64,
    /// Exact service; 精确Service。
    pub service: u16,
    /// Data=0, Control=exact opcode; Data为0，Control为精确Opcode。
    pub opcode: u16,
    /// Security Session generation; 安全会话代际。
    pub session: u32,
    /// Key generation; 密钥代际。
    pub key_generation: u32,
    /// Origin O1/O2; Origin保护。
    pub origin: ucn_types::OriginSecurity,
    /// Policy generation; 策略代际。
    pub policy: u32,
    /// Canonical Security context; 规范安全域。
    pub security: [u8; 16],
}
impl ServiceKey {
    /// Validate a complete nonzero correlation key; 验证完整关联键。
    /// # Errors
    /// Invalid identities or missing generations reject.
    pub fn validate(self) -> Result<()> {
        if self.realm == 0
            || self.client == [0; 16]
            || self.server == [0; 16]
            || self.client == self.server
            || self.client_generation == 0
            || self.server_generation == 0
            || self.client_address == 0
            || self.server_address == 0
            || self.client_address == u32::MAX
            || self.server_address == u32::MAX
            || self.operation == 0
            || self.service == 0
            || self.service == u16::MAX
            || self.opcode == u16::MAX
            || self.session == 0
            || self.key_generation == 0
            || self.origin == ucn_types::OriginSecurity::O0
            || self.policy == 0
            || self.security == [0; 16]
        {
            Err(Error::Argument)
        } else {
            Ok(())
        }
    }
}
