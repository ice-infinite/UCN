#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版 Identity、Address Authority、Lease 与 Binding Owner。
//!
//! 本 crate 不实现 Bootstrap/JOIN 消息状态机，也不直接调用 Persistence Owner。它只产生不可变
//! persistence requirement，并在 Coordinator 回送 exact reload proof 后发布 Authority/Binding。

mod lease;
mod model;
mod owner;

pub use lease::{LeasePolicy, lease_deadline_build, lease_is_live};
pub use model::{
    AddressMode, AuthorityEpoch, AuthorityFreshness, AuthorityTransition, AuthorityTransitionKind,
    BindingCertificate, BindingView, IdentityBinding, IdentityProof, IdentityVerifier, Principal,
};
pub use owner::{
    AUTHORITY_OPERATION_KIND, AUTHORITY_RECORD_BYTES, AUTHORITY_SCHEMA_ID,
    AUTHORITY_SCHEMA_VERSION, AuthorityChallengeHandle, AuthorityHandle,
    BINDING_ISSUE_OPERATION_KIND, BINDING_RECORD_BYTES, BINDING_RETIRE_OPERATION_KIND,
    BINDING_SCHEMA_ID, BINDING_SCHEMA_VERSION, BindingChallengeHandle, BindingHandle,
    FullIdentityOwner, IdentityConfig, IdentityDurabilityBase, IdentityDurabilityRequirement,
    IdentityOwner, LiteIdentityOwner, NanoIdentityOwner, decode_authority_record,
    decode_binding_record,
};
