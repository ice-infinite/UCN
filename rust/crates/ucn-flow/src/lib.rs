#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的不可变高级 Flow、逐跳 Stage/Commit 与 Label 快路径。
//!
//! 本 crate 只消费 Route、Capability 与 Security Owner 已签发的不可变事实，不直接调用其他
//! Owner。基础 C1 `SoftRoute` 与这里的 C2/C3/C4 `FlowPath` 是两种互不冒充的状态。

mod codec;
mod owner;
mod prefix;

pub use codec::{LABEL_SETUP_BYTES, LabelSetup, decode_label_setup, encode_label_setup};
pub use owner::{
    ActivationAck, ActivationHandle, ActivationKey, ActivationMessage, ActivationSubmit,
    CandidateHandle, FlowConfig, FlowCurrentFacts, FlowForwardPlan, FlowForwardRequest, FlowHandle,
    FlowHopFacts, FlowOwner, FlowPhase, FlowProposal, FlowReceipt, FlowReplayAdmission,
    FlowReplayHandle, FlowRequirements, FlowRole, FlowTxPlan, FlowTxRequest, FullFlowOwner,
    LiteFlowOwner, NanoFlowOwner, ProbeAck, ProbeRequest, RelayAbortAction, RelayCommitAction,
    RelayStageAction, TerminalOutcome,
};
pub use prefix::{
    C2_PREFIX_BYTES, C3_PREFIX_BYTES, C4_PREFIX_BYTES, FlowPrefix, decode_flow_prefix,
    encode_flow_prefix,
};
