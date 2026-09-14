#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的固定容量 Capability Owner 与只读 Contract Resolver。
//!
//! Capability 只描述“能做什么”，不等于 ACL、Authority、资源 reservation 或执行许可。

mod codec;
mod owner;
mod resolver;

pub use codec::{
    CAPABILITY_DIGEST_BYTES, CAPABILITY_QUERY_BYTES, CAPABILITY_RECORD_BYTES,
    CAPABILITY_SUMMARY_BYTES, CapabilityQuery, CapabilityRecord, CapabilitySummary, LinkCapability,
    MessageClass, PeerCapability, capability_digest, decode_capability_query,
    decode_capability_record, decode_capability_summary, encode_capability_query,
    encode_capability_record, encode_capability_summary,
};
pub use owner::{
    CachedPeerCapability, CapabilityConfig, CapabilityOwner, FullCapabilityOwner, HelloDisposition,
    LiteCapabilityOwner, NanoCapabilityOwner, PeerCapabilityRef,
};
pub use resolver::{
    ContractCandidate, ContractPlan, ContractResolver, DependencyKind, EffectiveIntent, ProfileAck,
    ProfileRequirements, ProfileSelect, ResolveResult, ResourceView, SecurityFloor,
};
