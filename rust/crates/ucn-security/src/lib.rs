#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的固定容量 Security Session、密码 Provider 和 Replay Owner。
//!
//! 本 crate 只消费 Coordinator 路由的 Persistence proof，不直接调用 Persistence Owner。
//! 密钥材料和密码算法由产品 [`CryptoProvider`] 持有；协议状态只保存精确 selector、代际和
//! 不可变上下文摘要。Dynamic Admission 不属于 RUST-05。

mod crypto;
mod packet;
mod replay;
mod session;

pub use crypto::{CryptoProvider, KeySelector, OpenOriginRequest, SealOriginRequest};
pub use packet::{
    Fingerprint, HOP_TAG_BYTES, HOP_TRAILER_BYTES, NONCE_BYTES, ORIGIN_TAG_BYTES, OpenDisposition,
    OpenPacketPlan, OpenedPacket, OriginContext, PacketPlan, ReplayCommitEvidence,
    SecurityPacketWorkspace, SecurityReplayHandle, SenderDirection,
};
pub use replay::{ReplayClassification, ReplayEvidence};
pub use session::{
    AccessDirection, AccessRequest, AclRule, AuthenticatedPeerView, Binding, C0TransactionOwner,
    CurrentFacts, DurabilityBase, FullSecurityOwner, HandshakeCandidate, HandshakeProof,
    HopSequenceOwner, LiteSecurityOwner, NanoSecurityOwner, OriginCounterOwner,
    OriginSequenceOwner, ReplayMaintenance, SESSION_OPERATION_KIND, SESSION_RECORD_BYTES,
    SESSION_SCHEMA_ID, SESSION_SCHEMA_VERSION, SecurityConfig, SecurityContextHandle,
    SecurityLevel, SecurityOwner, SessionDomainRule, SessionDurabilityRequirement, SessionHandle,
    SessionPhase, SessionView, decode_session_record,
};
