use ucn_capability::{CachedPeerCapability, PeerCapabilityRef};
use ucn_persistence::{CodecWorkspace, blake2s128};
use ucn_routing::{LinkRef, RouteDomain, SoftRouteView};
use ucn_security::{
    AccessDirection, Binding, Fingerprint, FlowReplayClaim, FlowSecurityRequest,
    OriginSequenceOwner, SessionHandle,
};
use ucn_types::{
    C0TransactionId, CandidateId, ContextId, DeliveryGuarantee, Error, FlowGeneration,
    ForwardingLabel, HeaderContract, HopProfile, InteractionRole, OriginSecurity, OriginSequence,
    PayloadKind, ProtocolOpcode, RealmId, Result, RouteGeneration, ServiceId, TrafficClass,
};

use crate::LabelSetup;

const CANONICAL_PROPOSAL_BYTES: usize = 224;

/// Flow Owner 固定配置。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowConfig {
    /// 唯一 Owner 实例。
    pub owner_instance: u32,
    /// 本机 Realm。
    pub realm: RealmId,
    /// 当前不可变本机 Binding。
    pub local: Binding,
    /// 当前 Policy Generation。
    pub policy_generation: u32,
    /// Probe 半开 Deadline 时长。
    pub probe_lifetime_us: u64,
    /// Stage 半开 Deadline 时长。
    pub stage_lifetime_us: u64,
    /// Commit 半开 Deadline 时长。
    pub commit_lifetime_us: u64,
    /// Active Flow 最大易失租期。
    pub flow_lifetime_us: u64,
    /// Terminal receipt 保留时长。
    pub receipt_lifetime_us: u64,
}

/// 一条 Flow 允许的完整、不可变业务要求。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowRequirements {
    /// C2、C3 或 C4。
    pub contract: HeaderContract,
    /// 唯一目标 Service。
    pub service: ServiceId,
    /// 最高可用流量类别；单帧不能临时提权。
    pub traffic_ceiling: TrafficClass,
    /// 交付保证。
    pub delivery: DeliveryGuarantee,
    /// 交互角色。
    pub interaction: InteractionRole,
    /// Payload 类别。
    pub payload_kind: PayloadKind,
    /// Data 固定为 0；Control 为精确 Opcode。
    pub protocol_opcode: u16,
    /// Origin 保护。
    pub origin_security: OriginSecurity,
    /// 当前路径逐跳保护。
    pub hop_profile: HopProfile,
    /// 必须由认证 Capability 满足的 Feature bits。
    pub required_feature_bits: u32,
    /// 至少需要的单帧业务 Payload。
    pub minimum_payload_bytes: u16,
    /// 创建时绑定的 Policy Generation。
    pub policy_generation: u32,
    /// Manifest Path Profile ID。
    pub path_profile_id: u16,
    /// 调用方允许的绝对半开租期。
    pub expires_at_us: u64,
}

impl FlowRequirements {
    fn validate(self, hop_count: u8, now_us: u64) -> Result<()> {
        let contract_ok = match self.contract {
            HeaderContract::C2 => hop_count == 1,
            HeaderContract::C3 => {
                hop_count > 1
                    && self.delivery == DeliveryGuarantee::BestEffort
                    && self.interaction == InteractionRole::OneWay
                    && matches!(
                        self.payload_kind,
                        PayloadKind::Data | PayloadKind::Diagnostic
                    )
                    && self.origin_security == OriginSecurity::O0
                    && matches!(self.hop_profile, HopProfile::H0 | HopProfile::H1)
            }
            HeaderContract::C4 => {
                hop_count > 1 && matches!(self.hop_profile, HopProfile::H0 | HopProfile::H1)
            }
            _ => false,
        };
        let opcode_ok = match self.payload_kind {
            PayloadKind::Control => {
                self.protocol_opcode != 0
                    && ProtocolOpcode::try_from(self.protocol_opcode).is_ok()
                    && self.contract != HeaderContract::C3
            }
            _ => self.protocol_opcode == 0,
        };
        if !contract_ok
            || !opcode_ok
            || (self.interaction != InteractionRole::OneWay
                && self.payload_kind == PayloadKind::Data)
            || self.minimum_payload_bytes == 0
            || self.policy_generation == 0
            || self.path_profile_id == 0
            || self.path_profile_id == u16::MAX
            || now_us >= self.expires_at_us
            || (self.origin_security != OriginSecurity::O0 && self.contract == HeaderContract::C3)
            || (self.hop_profile == HopProfile::H2
                && (self.contract != HeaderContract::C2
                    || self.origin_security == OriginSecurity::O0))
            || self.hop_profile == HopProfile::H3
        {
            return Err(Error::Policy);
        }
        Ok(())
    }
}

/// Flow 生命周期。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FlowPhase {
    /// 只拥有候选路径事实。
    Candidate,
    /// `PATH_PROBE` 已产生，等待精确 ACK。
    Probing,
    /// Probe 已精确完成，可进入 Stage。
    ReadyToStage,
    /// 本地资源已预留，Stage 输出尚未确认提交。
    StagePending,
    /// 下游已精确 Staged。
    Staged,
    /// Commit 已可能产生外部副作用。
    Committing,
    /// 可供 C2/C3/C4 使用。
    Active,
    /// Commit 结果无法证明；保留键和对账义务。
    InDoubt,
    /// Commit 前的取消正在等待收敛。
    Aborting,
    /// 已被新 Active 替换，只处理旧引用。
    Draining,
    /// 硬依赖失效，不允许继续使用。
    Fenced,
    /// 已退休；ID 在父代际内仍不复用。
    Retired,
}

/// 本节点在激活链中的角色。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FlowRole {
    /// 发起者，最后发布 Active。
    Origin,
    /// 等下游 ACK 后才向上游 ACK。
    Relay,
    /// 链路末端。
    Target,
}

/// Flow Owner 签发的 Candidate Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CandidateHandle {
    owner_instance: u32,
    slot: u16,
    slot_generation: u32,
    candidate_id: CandidateId,
}

/// Flow Owner 签发的 Activation Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ActivationHandle {
    owner_instance: u32,
    slot: u16,
    slot_generation: u32,
    transaction_id: C0TransactionId,
}

/// Flow Owner 签发的 Active/terminal Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowHandle {
    owner_instance: u32,
    slot: u16,
    slot_generation: u32,
    flow_generation: FlowGeneration,
}

/// 完整激活事务键。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ActivationKey {
    route_domain: RouteDomain,
    transaction_id: C0TransactionId,
    candidate_id: CandidateId,
    route_generation: RouteGeneration,
    reverse_label: ForwardingLabel,
    forward_label: ForwardingLabel,
    path_profile_id: u16,
    proposal_digest: [u8; 16],
}

impl ActivationKey {
    /// Route Domain。
    #[must_use]
    pub const fn route_domain(self) -> RouteDomain {
        self.route_domain
    }

    /// C0 Transaction ID。
    #[must_use]
    pub const fn transaction_id(self) -> C0TransactionId {
        self.transaction_id
    }

    /// Candidate ID。
    #[must_use]
    pub const fn candidate_id(self) -> CandidateId {
        self.candidate_id
    }

    /// Route Generation。
    #[must_use]
    pub const fn route_generation(self) -> RouteGeneration {
        self.route_generation
    }

    /// 完整 128-bit Proposal Digest。
    #[must_use]
    pub const fn proposal_digest(self) -> [u8; 16] {
        self.proposal_digest
    }

    /// 生成 Wire 16 B Setup；32-bit digest 只作预筛选。
    #[must_use]
    pub fn label_setup(self) -> LabelSetup {
        LabelSetup {
            candidate_id: self.candidate_id,
            route_generation: self.route_generation,
            reverse_label: self.reverse_label,
            forward_label: self.forward_label,
            path_profile_id: self.path_profile_id,
            context_digest: u32::from_be_bytes([
                self.proposal_digest[0],
                self.proposal_digest[1],
                self.proposal_digest[2],
                self.proposal_digest[3],
            ]),
        }
    }
}

/// Probe/Stage 时冻结的完整 Proposal。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowProposal {
    key: ActivationKey,
    route_owner_instance: u32,
    next_hop: LinkRef,
    hop_count: u8,
    cost: u32,
    path_frame_mtu: u16,
    payload_budget: u16,
    capability_bits: u16,
    capability_runtime_instance: u32,
    capability_security_owner_instance: u32,
    capability_session_generation: u32,
    capability_generation: u32,
    capability_digest: [u8; 16],
    capability_deadline_us: u64,
    context_id: ContextId,
    requirements: FlowRequirements,
    probe_deadline_us: u64,
    flow_expires_at_us: u64,
}

impl FlowProposal {
    /// 完整激活键。
    #[must_use]
    pub const fn key(self) -> ActivationKey {
        self.key
    }

    /// 冻结下一跳。
    #[must_use]
    pub const fn next_hop(self) -> LinkRef {
        self.next_hop
    }

    /// 稳态 Flow Context ID。
    #[must_use]
    pub const fn context_id(self) -> ContextId {
        self.context_id
    }

    /// 冻结 Contract/业务要求。
    #[must_use]
    pub const fn requirements(self) -> FlowRequirements {
        self.requirements
    }

    /// 可供业务使用的保守 Payload Budget。
    #[must_use]
    pub const fn payload_budget(self) -> u16 {
        self.payload_budget
    }

    /// Flow 半开绝对 Deadline。
    #[must_use]
    pub const fn expires_at_us(self) -> u64 {
        self.flow_expires_at_us
    }

    /// 由完整 canonical Proposal 得到的安全 fingerprint。
    ///
    /// # Errors
    ///
    /// 全零摘要不会成为合法 fingerprint。
    pub const fn fingerprint(self) -> Result<Fingerprint> {
        Fingerprint::new(self.key.proposal_digest)
    }
}

/// Origin 发出的 Probe。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProbeRequest {
    /// 冻结 Proposal。
    pub proposal: FlowProposal,
}

/// Target/Relay 返回的 Probe ACK。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProbeAck {
    /// 必须 exact-match。
    pub key: ActivationKey,
    /// ACK 观察到的路径 MTU。
    pub path_frame_mtu: u16,
    /// ACK 观察到的能力交集。
    pub capability_bits: u16,
    /// 有界 RTT 观测；0 非法。
    pub measured_rtt_us: u32,
}

/// Stage/Commit/Abort 线上语义对象。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ActivationMessage {
    /// 精确 Opcode。
    pub opcode: ProtocolOpcode,
    /// 完整激活键。
    pub key: ActivationKey,
    /// 16 B Wire Setup 的强类型值。
    pub setup: LabelSetup,
    /// 本地事务保存的完整 Proposal；Wire 解码后必须由 typed context 精确恢复并比较。
    pub proposal: FlowProposal,
}

/// Stage/Commit/Terminal ACK。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ActivationAck {
    /// `PathStageAck`、`PathCommitAck` 或 `PathTerminalReceipt`。
    pub opcode: ProtocolOpcode,
    /// 完整激活键。
    pub key: ActivationKey,
}

/// Caller 完成 Adapter submit 后报告的精确结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ActivationSubmit {
    /// Driver 明确未观察报文；可保持相同事务等待有界重试。
    NotSubmitted,
    /// Driver 已接受报文。
    Submitted,
    /// 是否产生外部副作用无法证明。
    InDoubt,
}

/// Relay/Target 接收 Stage 后的动作。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RelayStageAction {
    /// Target 已 Staged，可向上游 ACK。
    Ack {
        /// 精确 ACK。
        ack: ActivationAck,
        /// 冻结上游 Link。
        upstream: LinkRef,
    },
    /// Relay 必须先向下游转发。
    Forward {
        /// 下一跳。
        downstream: LinkRef,
        /// 本地精确事务。
        activation: ActivationHandle,
    },
}

/// Relay/Target 接收 Commit 后的动作。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RelayCommitAction {
    /// Target 已 Active，可向上游 ACK。
    Ack {
        /// 精确 ACK。
        ack: ActivationAck,
        /// 冻结上游 Link。
        upstream: LinkRef,
        /// Target 本地 Active Flow Handle；重复 Commit 返回完全相同的 Handle。
        flow: FlowHandle,
    },
    /// Relay 必须先向下游转发，尚不可发布 Active。
    Forward {
        /// 下一跳。
        downstream: LinkRef,
        /// 本地精确事务。
        activation: ActivationHandle,
    },
}

/// Relay/Target 接收 Abort 后的动作。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RelayAbortAction {
    /// 本节点与所有已知下游均未接受 Commit，可向上游返回终态 receipt。
    Ack {
        /// 精确 terminal receipt。
        ack: ActivationAck,
        /// 冻结上游 Link。
        upstream: LinkRef,
    },
    /// Relay 已向下游提交过 Stage，必须先转发相同 Abort 并等待 receipt。
    Forward {
        /// 冻结下游 Link。
        downstream: LinkRef,
        /// 本地精确事务。
        activation: ActivationHandle,
    },
}

/// `InDoubt` 对账结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TerminalOutcome {
    /// 认证 receipt 证明整条链已提交。
    Committed,
    /// 认证 proof 证明远端不再可用。
    Excluded,
}

/// 对外只读 terminal receipt。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowReceipt {
    /// 完整事务键。
    pub key: ActivationKey,
    /// 最后确定的状态。
    pub phase: FlowPhase,
    /// Receipt 半开保留 Deadline。
    pub expires_at_us: u64,
}

/// 每次 Flow 使用时由 Coordinator 重新取得的当前事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowCurrentFacts {
    /// 可信单调时间。
    pub now_us: u64,
    /// 当前本机 Binding。
    pub local: Binding,
    /// 当前目标 Binding。
    pub destination: Binding,
    /// 当前 Origin Session Generation 数值。
    pub origin_session_generation: u32,
    /// 当前下一跳 Link。
    pub next_hop: LinkRef,
    /// 当前 Capability 父引用。
    pub capability_ref: PeerCapabilityRef,
    /// 当前 Capability Generation。
    pub capability_generation: u32,
    /// 当前 Capability Digest。
    pub capability_digest: [u8; 16],
    /// 当前 Capability Deadline。
    pub capability_deadline_us: u64,
    /// 当前 Policy Generation。
    pub policy_generation: u32,
}

/// Relay/Target 在 Stage 与 Commit 两次复核的本地逐跳事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowHopFacts {
    /// 可信单调时间。
    pub now_us: u64,
    /// 必须等于当前 Owner 本机 Binding。
    pub local: Binding,
    /// 当前入站 Link；必须与 Stage 冻结值精确相同。
    pub upstream: LinkRef,
    /// Relay 的当前出站 Link；Target 固定为 `None`。
    pub downstream: Option<LinkRef>,
    /// 当前本地 Policy Generation。
    pub policy_generation: u32,
    /// 当前 Hop 可承载的完整 Frame MTU。
    pub path_frame_mtu: u16,
    /// 当前 Hop 已认证的能力位。
    pub capability_bits: u16,
    /// 当前 Hop Security Profile。
    pub hop_profile: HopProfile,
    /// 本地路径/Capability 事实的半开 Deadline。
    pub capability_deadline_us: u64,
    /// 当前本地 Security Owner instance。
    pub security_owner_instance: u32,
    /// 当前上游 Peer Session Generation。
    pub peer_session_generation: u32,
    /// 当前入站 Origin Key Generation。
    pub origin_key_generation: u32,
}

/// Flow Replay 预留 Handle；字段不可由调用方构造。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowReplayHandle {
    owner_instance: u32,
    flow_slot: u16,
    flow_slot_generation: u32,
    reservation_generation: u32,
    sequence: OriginSequence,
    reservation_deadline_us: u64,
    aad_digest: [u8; 16],
    payload_digest: [u8; 16],
}

/// 已认证 Flow 输入相对本 Flow Replay Window 的分类。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FlowReplayAdmission {
    /// 首次输入；业务提交前必须 commit/abort 此 Handle。
    Fresh(FlowReplayHandle),
    /// 已提交序号；不得再次执行业务，只能查询上层 receipt。
    Duplicate,
}

/// 一次稳态发送请求。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowTxRequest {
    /// 必须等于 Flow Contract。
    pub contract: HeaderContract,
    /// 不高于 Flow ceiling。
    pub traffic_class: TrafficClass,
    /// 必须精确匹配。
    pub delivery: DeliveryGuarantee,
    /// 必须精确匹配。
    pub interaction: InteractionRole,
    /// 必须精确匹配。
    pub payload_kind: PayloadKind,
    /// Data 为 0；Control 为精确值。
    pub protocol_opcode: u16,
    /// 必须精确匹配。
    pub origin_security: OriginSecurity,
    /// 单帧业务字节数。
    pub payload_bytes: u16,
}

/// Flow Use-Preflight 返回的不可变编码/转发计划。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowTxPlan {
    /// Contract。
    pub contract: HeaderContract,
    /// Context ID。
    pub context_id: ContextId,
    /// C3/C4 当前正向 Label；C2 仍返回冻结值但不编码它。
    pub forward_label: ForwardingLabel,
    /// Route Generation。
    pub route_generation: RouteGeneration,
    /// 下一跳。
    pub next_hop: LinkRef,
    /// 完整 Flow fingerprint，供 Security 绑定。
    pub flow_fingerprint: Fingerprint,
    /// Payload budget。
    pub payload_budget: u16,
    /// Hop Profile。
    pub hop_profile: HopProfile,
    /// O1/O2 的下一 Origin Sequence；C3/O0 为 `None`。仅 Security Provider 首次观察时消耗。
    pub origin_sequence: Option<OriginSequence>,
}

/// Relay Label 快路径的一次输入。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowForwardRequest {
    /// 当前入站 Link。
    pub ingress: LinkRef,
    /// Wire Label。
    pub label: ForwardingLabel,
    /// Wire Flow Context ID。
    pub context_id: ContextId,
    /// 已由本地 Label 表绑定的 Route Generation。
    pub route_generation: RouteGeneration,
    /// 当前 Traffic Class。
    pub traffic_class: TrafficClass,
    /// 完整 Core Packet 字节数。
    pub packet_bytes: u16,
}

/// Relay 预检后的不可变 O(1) 转发计划。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FlowForwardPlan {
    /// 冻结出站 Link。
    pub egress: LinkRef,
    /// 下游 Label。
    pub egress_label: ForwardingLabel,
    /// 下游 Context ID。
    pub egress_context_id: ContextId,
    /// Hop Profile。
    pub hop_profile: HopProfile,
}

#[derive(Clone, Copy)]
struct CandidateRecord {
    candidate_id: CandidateId,
    source_route: SoftRouteView,
    capability: CachedPeerCapability,
    requirements: FlowRequirements,
    phase: FlowPhase,
    proposal: Option<FlowProposal>,
}

#[derive(Clone, Copy)]
struct CandidateSlot {
    generation: u32,
    value: Option<CandidateRecord>,
}

impl CandidateSlot {
    const EMPTY: Self = Self {
        generation: 0,
        value: None,
    };
}

#[derive(Clone, Copy)]
struct ActivationRecord {
    role: FlowRole,
    proposal: FlowProposal,
    phase: FlowPhase,
    stage_submitted: bool,
    commit_submitted: bool,
    abort_submitted: bool,
    upstream: Option<LinkRef>,
    downstream: Option<LinkRef>,
    local_capability_deadline_us: u64,
    local_security_owner_instance: u32,
    local_peer_session_generation: u32,
    local_origin_key_generation: u32,
    flow_slot: usize,
    receipt_slot: usize,
    stage_deadline_us: u64,
    commit_deadline_us: u64,
}

#[derive(Clone, Copy)]
struct ActivationSlot {
    generation: u32,
    value: Option<ActivationRecord>,
}

impl ActivationSlot {
    const EMPTY: Self = Self {
        generation: 0,
        value: None,
    };
}

#[derive(Clone, Copy)]
struct FlowRecord {
    role: FlowRole,
    proposal: FlowProposal,
    phase: FlowPhase,
    flow_generation: FlowGeneration,
    upstream: Option<LinkRef>,
    downstream: Option<LinkRef>,
    local_capability_deadline_us: u64,
    local_security_owner_instance: u32,
    local_peer_session_generation: u32,
    local_origin_key_generation: u32,
    replay_initialized: bool,
    replay_highest: u32,
    replay_bitmap: u64,
    replay_reservation_generation: u32,
    replay_reservation: Option<FlowReplayReservation>,
}

#[derive(Clone, Copy)]
struct FlowReplayReservation {
    sequence: OriginSequence,
    deadline_us: u64,
    aad_digest: [u8; 16],
    payload_digest: [u8; 16],
}

#[derive(Clone, Copy)]
struct FlowSlot {
    generation: u32,
    value: Option<FlowRecord>,
}

impl FlowSlot {
    const EMPTY: Self = Self {
        generation: 0,
        value: None,
    };
}

#[derive(Clone, Copy)]
struct ReceiptSlot {
    value: Option<FlowReceipt>,
}

impl ReceiptSlot {
    const EMPTY: Self = Self { value: None };
}

/// 固定容量高级 Flow Owner。
pub struct FlowOwner<
    const CANDIDATES: usize,
    const ACTIVATIONS: usize,
    const FLOWS: usize,
    const RECEIPTS: usize,
> {
    config: FlowConfig,
    next_candidate_id: Option<u32>,
    next_transaction_id: Option<u64>,
    next_route_generation: Option<u32>,
    next_context_id: Option<u16>,
    next_label: Option<u16>,
    next_flow_generation: Option<u32>,
    next_origin_sequence: Option<u32>,
    candidates: [CandidateSlot; CANDIDATES],
    activations: [ActivationSlot; ACTIVATIONS],
    flows: [FlowSlot; FLOWS],
    receipts: [ReceiptSlot; RECEIPTS],
    cleanup_cursor: usize,
    codec: CodecWorkspace,
}

/// Nano：2 Candidate、2 Activation、2 Flow、2 Receipt。
pub type NanoFlowOwner = FlowOwner<2, 2, 2, 2>;
/// Lite：8 Candidate、8 Activation、8 Flow、8 Receipt。
pub type LiteFlowOwner = FlowOwner<8, 8, 8, 8>;
/// Full：24 Candidate、24 Activation、32 Flow、32 Receipt。
pub type FullFlowOwner = FlowOwner<24, 24, 32, 32>;

impl<const CANDIDATES: usize, const ACTIVATIONS: usize, const FLOWS: usize, const RECEIPTS: usize>
    FlowOwner<CANDIDATES, ACTIVATIONS, FLOWS, RECEIPTS>
{
    /// 建立空 Owner；计数器最大值本身可被分配一次，但不得回绕。
    ///
    /// # Errors
    ///
    /// 配置、容量或计数器初值非法时返回配置错误。
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        config: FlowConfig,
        first_candidate_id: u32,
        first_transaction_id: u64,
        first_route_generation: u32,
        first_context_id: u16,
        first_label: u16,
        first_flow_generation: u32,
    ) -> Result<Self> {
        if config.owner_instance == 0
            || config.local.principal == [0; 16]
            || config.local.generation.is_unbound()
            || config.policy_generation == 0
            || config.probe_lifetime_us == 0
            || config.stage_lifetime_us == 0
            || config.commit_lifetime_us == 0
            || config.flow_lifetime_us == 0
            || config.receipt_lifetime_us < config.commit_lifetime_us
            || CANDIDATES == 0
            || ACTIVATIONS == 0
            || FLOWS == 0
            || RECEIPTS == 0
            || CANDIDATES > usize::from(u16::MAX)
            || ACTIVATIONS > usize::from(u16::MAX)
            || FLOWS > usize::from(u16::MAX)
            || first_candidate_id == 0
            || first_candidate_id >= u32::from(u16::MAX)
            || first_transaction_id == 0
            || first_route_generation == 0
            || first_context_id == 0
            || first_context_id == u16::MAX
            || first_label == 0
            || first_label >= u16::MAX - 1
            || first_flow_generation == 0
        {
            return Err(Error::Config);
        }
        Ok(Self {
            config,
            next_candidate_id: Some(first_candidate_id),
            next_transaction_id: Some(first_transaction_id),
            next_route_generation: Some(first_route_generation),
            next_context_id: Some(first_context_id),
            next_label: Some(first_label),
            next_flow_generation: Some(first_flow_generation),
            next_origin_sequence: Some(1),
            candidates: [CandidateSlot::EMPTY; CANDIDATES],
            activations: [ActivationSlot::EMPTY; ACTIVATIONS],
            flows: [FlowSlot::EMPTY; FLOWS],
            receipts: [ReceiptSlot::EMPTY; RECEIPTS],
            cleanup_cursor: 0,
            codec: CodecWorkspace::new(),
        })
    }

    /// 从一个易失 `SoftRoute` 和当前认证 Capability 创建新的候选。
    ///
    /// 每次调用都分配新 Candidate；不会原地改写已经 Probe 的对象。
    ///
    /// # Errors
    ///
    /// Route、Capability、Requirements、Deadline 或容量不匹配时失败且不占槽。
    pub fn import_candidate(
        &mut self,
        route: SoftRouteView,
        capability: CachedPeerCapability,
        requirements: FlowRequirements,
        now_us: u64,
    ) -> Result<CandidateHandle> {
        self.validate_candidate_inputs(route, capability, requirements, now_us)?;
        let index = self
            .candidates
            .iter()
            .position(|slot| slot.value.is_none())
            .ok_or(Error::NoSpace)?;
        let candidate_raw = self.next_candidate_id.ok_or(Error::Exhausted)?;
        let candidate_id = CandidateId::new(candidate_raw)?;
        let next_candidate = next_u32_with_limit(candidate_raw, u32::from(u16::MAX - 1));
        let generation = self.candidates[index]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let slot = u16::try_from(index + 1).map_err(|_| Error::NoSpace)?;
        self.candidates[index] = CandidateSlot {
            generation,
            value: Some(CandidateRecord {
                candidate_id,
                source_route: route,
                capability,
                requirements,
                phase: FlowPhase::Candidate,
                proposal: None,
            }),
        };
        self.next_candidate_id = next_candidate;
        Ok(CandidateHandle {
            owner_instance: self.config.owner_instance,
            slot,
            slot_generation: generation,
            candidate_id,
        })
    }

    /// 冻结 Candidate 并产生 `PATH_PROBE`；此后路径字段不可改变。
    ///
    /// # Errors
    ///
    /// Handle、计数器、事实或 Deadline 不成立时失败且 Candidate 不变。
    pub fn begin_probe(&mut self, candidate: CandidateHandle, now_us: u64) -> Result<ProbeRequest> {
        let index = self.candidate_index(candidate)?;
        let record = self.candidates[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Candidate {
            return Err(Error::State);
        }
        self.validate_candidate_inputs(
            record.source_route,
            record.capability,
            record.requirements,
            now_us,
        )?;
        let transaction_raw = self.next_transaction_id.ok_or(Error::Exhausted)?;
        let route_generation_raw = self.next_route_generation.ok_or(Error::Exhausted)?;
        let context_raw = self.next_context_id.ok_or(Error::Exhausted)?;
        let reverse_label_raw = self.next_label.ok_or(Error::Exhausted)?;
        let transaction_id = C0TransactionId::new(transaction_raw)?;
        let route_generation = RouteGeneration::new(route_generation_raw)?;
        let context_id = ContextId::new(context_raw)?;
        let reverse_label = ForwardingLabel::new(reverse_label_raw)?;
        let forward_label_raw = reverse_label_raw
            .checked_add(1)
            .filter(|value| *value != u16::MAX)
            .ok_or(Error::Exhausted)?;
        let forward_label = ForwardingLabel::new(forward_label_raw)?;
        let probe_deadline_us = now_us
            .checked_add(self.config.probe_lifetime_us)
            .ok_or(Error::Exhausted)?
            .min(record.source_route.expires_at_us())
            .min(record.capability.capability_deadline_us)
            .min(record.requirements.expires_at_us);
        if now_us >= probe_deadline_us {
            return Err(Error::Timeout);
        }
        let flow_expires_at_us = now_us
            .checked_add(self.config.flow_lifetime_us)
            .ok_or(Error::Exhausted)?
            .min(record.capability.capability_deadline_us)
            .min(record.requirements.expires_at_us);
        if now_us >= flow_expires_at_us {
            return Err(Error::Timeout);
        }
        let payload_budget =
            payload_budget(record.requirements, record.source_route.path_frame_mtu())?;
        let mut proposal = FlowProposal {
            key: ActivationKey {
                route_domain: record.source_route.domain(),
                transaction_id,
                candidate_id: candidate.candidate_id,
                route_generation,
                reverse_label,
                forward_label,
                path_profile_id: record.requirements.path_profile_id,
                proposal_digest: [0; 16],
            },
            route_owner_instance: record.source_route.owner_instance(),
            next_hop: record.source_route.next_hop(),
            hop_count: record.source_route.hop_count(),
            cost: record.source_route.cost(),
            path_frame_mtu: record.source_route.path_frame_mtu(),
            payload_budget,
            capability_bits: record.source_route.capability_bits(),
            capability_runtime_instance: record.capability.peer_ref.runtime_instance,
            capability_security_owner_instance: record.capability.peer_ref.security_owner_instance,
            capability_session_generation: record.capability.peer_ref.session_generation,
            capability_generation: record.capability.record.capability_generation,
            capability_digest: record.capability.digest,
            capability_deadline_us: record.capability.capability_deadline_us,
            context_id,
            requirements: record.requirements,
            probe_deadline_us,
            flow_expires_at_us,
        };
        let digest = proposal_digest(&proposal, &mut self.codec)?;
        Fingerprint::new(digest).map_err(|_| Error::Security)?;
        proposal.key.proposal_digest = digest;
        let next_transaction = transaction_raw.checked_add(1);
        let next_route = route_generation_raw.checked_add(1);
        let next_context = next_u16_with_limit(context_raw, u16::MAX - 1);
        let next_label = forward_label_raw
            .checked_add(1)
            .filter(|value| *value < u16::MAX - 1);
        self.candidates[index].value = Some(CandidateRecord {
            phase: FlowPhase::Probing,
            proposal: Some(proposal),
            ..record
        });
        self.next_transaction_id = next_transaction;
        self.next_route_generation = next_route;
        self.next_context_id = next_context;
        self.next_label = next_label;
        Ok(ProbeRequest { proposal })
    }

    /// 只接受完整键、MTU 和 Capability 都与冻结 Proposal 相同的 Probe ACK。
    ///
    /// # Errors
    ///
    /// ACK 错绑、超时或状态不匹配时失败且 Candidate 不变。
    pub fn on_probe_ack(
        &mut self,
        candidate: CandidateHandle,
        ack: ProbeAck,
        now_us: u64,
    ) -> Result<()> {
        let index = self.candidate_index(candidate)?;
        let record = self.candidates[index].value.ok_or(Error::NotFound)?;
        let proposal = record.proposal.ok_or(Error::State)?;
        if record.phase != FlowPhase::Probing
            || ack.key != proposal.key
            || ack.path_frame_mtu != proposal.path_frame_mtu
            || ack.capability_bits != proposal.capability_bits
            || ack.measured_rtt_us == 0
        {
            return Err(Error::State);
        }
        if now_us >= proposal.probe_deadline_us {
            return Err(Error::Timeout);
        }
        self.candidates[index].value = Some(CandidateRecord {
            phase: FlowPhase::ReadyToStage,
            ..record
        });
        Ok(())
    }

    /// 在一次零写入预检后预留 Activation、Flow、Receipt 并产生 Stage。
    ///
    /// # Errors
    ///
    /// 任一固定容量、Deadline 或状态不成立时失败且全部表不变。
    pub fn begin_stage(
        &mut self,
        candidate: CandidateHandle,
        facts: FlowCurrentFacts,
    ) -> Result<(ActivationHandle, ActivationMessage)> {
        let candidate_index = self.candidate_index(candidate)?;
        let candidate_record = self.candidates[candidate_index]
            .value
            .ok_or(Error::NotFound)?;
        let proposal = candidate_record.proposal.ok_or(Error::State)?;
        if candidate_record.phase != FlowPhase::ReadyToStage {
            return Err(Error::State);
        }
        if facts.now_us >= proposal.probe_deadline_us {
            return Err(Error::Timeout);
        }
        self.validate_use_facts(&proposal, facts)?;
        let now_us = facts.now_us;
        let activation_index = self.free_activation_slot()?;
        let flow_index = self.free_flow_slot()?;
        let receipt_index = self.free_receipt_slot()?;
        let stage_deadline_us = now_us
            .checked_add(self.config.stage_lifetime_us)
            .ok_or(Error::Exhausted)?
            .min(proposal.flow_expires_at_us);
        let commit_deadline_us = stage_deadline_us
            .checked_add(self.config.commit_lifetime_us)
            .ok_or(Error::Exhausted)?
            .min(proposal.flow_expires_at_us);
        if now_us >= stage_deadline_us || stage_deadline_us >= commit_deadline_us {
            return Err(Error::Timeout);
        }
        let generation = self.activations[activation_index]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let slot = u16::try_from(activation_index + 1).map_err(|_| Error::NoSpace)?;
        let message = activation_message(ProtocolOpcode::PathActivateStage, proposal);
        self.activations[activation_index] = ActivationSlot {
            generation,
            value: Some(ActivationRecord {
                role: FlowRole::Origin,
                proposal,
                phase: FlowPhase::StagePending,
                stage_submitted: false,
                commit_submitted: false,
                abort_submitted: false,
                upstream: None,
                downstream: Some(proposal.next_hop),
                local_capability_deadline_us: proposal.capability_deadline_us,
                local_security_owner_instance: proposal.capability_security_owner_instance,
                local_peer_session_generation: proposal.capability_session_generation,
                local_origin_key_generation: 0,
                flow_slot: flow_index,
                receipt_slot: receipt_index,
                stage_deadline_us,
                commit_deadline_us,
            }),
        };
        self.candidates[candidate_index].value = Some(CandidateRecord {
            phase: FlowPhase::StagePending,
            ..candidate_record
        });
        Ok((
            ActivationHandle {
                owner_instance: self.config.owner_instance,
                slot,
                slot_generation: generation,
                transaction_id: proposal.key.transaction_id,
            },
            message,
        ))
    }

    /// 报告 Stage 发送结果；明确未发送时保留同键重试，未知副作用进入 `InDoubt`。
    ///
    /// # Errors
    ///
    /// Handle 或状态不匹配时失败且事务不变。
    pub fn complete_stage_submit(
        &mut self,
        activation: ActivationHandle,
        outcome: ActivationSubmit,
    ) -> Result<()> {
        let index = self.activation_index(activation)?;
        let mut record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::StagePending {
            return Err(Error::State);
        }
        match outcome {
            ActivationSubmit::NotSubmitted => {}
            ActivationSubmit::Submitted => record.stage_submitted = true,
            ActivationSubmit::InDoubt => record.phase = FlowPhase::InDoubt,
        }
        self.activations[index].value = Some(record);
        Ok(())
    }

    /// 返回完全相同的 Stage 事务，供明确未提交或 ACK 丢失后的有界重试使用。
    ///
    /// # Errors
    ///
    /// Handle 或阶段不匹配时返回错误；不会分配新 Candidate、ID、Label 或 Deadline。
    pub fn stage_message(&self, activation: ActivationHandle) -> Result<ActivationMessage> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::StagePending {
            return Err(Error::State);
        }
        Ok(activation_message(
            ProtocolOpcode::PathActivateStage,
            record.proposal,
        ))
    }

    /// Origin 处理完整 Stage ACK，并产生 Commit；ACK 前必须实际提交过 Stage。
    ///
    /// # Errors
    ///
    /// ACK 错绑、状态或 Deadline 不满足时失败且不改状态。
    pub fn on_stage_ack(
        &mut self,
        activation: ActivationHandle,
        ack: ActivationAck,
        facts: FlowCurrentFacts,
    ) -> Result<ActivationMessage> {
        let index = self.activation_index(activation)?;
        let mut record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.role != FlowRole::Origin
            || record.phase != FlowPhase::StagePending
            || !record.stage_submitted
            || ack.opcode != ProtocolOpcode::PathStageAck
            || ack.key != record.proposal.key
        {
            return Err(Error::State);
        }
        let now_us = facts.now_us;
        if now_us >= record.stage_deadline_us {
            return Err(Error::Timeout);
        }
        self.validate_use_facts(&record.proposal, facts)?;
        record.phase = FlowPhase::Committing;
        self.activations[index].value = Some(record);
        Ok(activation_message(
            ProtocolOpcode::PathActivateCommit,
            record.proposal,
        ))
    }

    /// 报告 Commit 发送结果；可能发送时不可回滚为 Stage。
    ///
    /// # Errors
    ///
    /// Handle 或状态不匹配时失败且不改状态。
    pub fn complete_commit_submit(
        &mut self,
        activation: ActivationHandle,
        outcome: ActivationSubmit,
    ) -> Result<()> {
        let index = self.activation_index(activation)?;
        let mut record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Committing {
            return Err(Error::State);
        }
        match outcome {
            ActivationSubmit::NotSubmitted => {}
            ActivationSubmit::Submitted => record.commit_submitted = true,
            ActivationSubmit::InDoubt => record.phase = FlowPhase::InDoubt,
        }
        self.activations[index].value = Some(record);
        Ok(())
    }

    /// 返回完全相同的 Commit 事务，供明确未提交或 ACK 丢失后的有界重试使用。
    ///
    /// # Errors
    ///
    /// Handle 或阶段不匹配时返回错误；不会分配新事务身份。
    pub fn commit_message(&self, activation: ActivationHandle) -> Result<ActivationMessage> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Committing {
            return Err(Error::State);
        }
        Ok(activation_message(
            ProtocolOpcode::PathActivateCommit,
            record.proposal,
        ))
    }

    /// Origin 在第一跳精确 Commit ACK 后最后发布新 Active，并将同 Domain 旧 Flow 置为 Draining。
    ///
    /// # Errors
    ///
    /// ACK、依赖、Deadline 或预留槽不匹配时失败，旧 Active 保持不变。
    pub fn on_commit_ack(
        &mut self,
        activation: ActivationHandle,
        ack: ActivationAck,
        facts: FlowCurrentFacts,
    ) -> Result<FlowHandle> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.role != FlowRole::Origin
            || ack.opcode != ProtocolOpcode::PathCommitAck
            || ack.key != record.proposal.key
        {
            return Err(Error::State);
        }
        if record.phase == FlowPhase::Active {
            self.validate_use_facts(&record.proposal, facts)?;
            return self.active_flow_handle(&record);
        }
        if record.phase != FlowPhase::Committing || !record.commit_submitted {
            return Err(Error::State);
        }
        if facts.now_us >= record.commit_deadline_us {
            return Err(Error::Timeout);
        }
        self.validate_use_facts(&record.proposal, facts)?;
        self.publish_activation(index, FlowPhase::Active, facts.now_us)
    }

    /// Relay/Target 接纳 Stage；Relay 必须等待下游 ACK，Target 可立即返回 ACK。
    ///
    /// # Errors
    ///
    /// 消息、角色、路径、容量或 Deadline 不成立时零写入拒绝。
    pub fn accept_stage(
        &mut self,
        message: &ActivationMessage,
        facts: FlowHopFacts,
    ) -> Result<RelayStageAction> {
        validate_activation_message(message, ProtocolOpcode::PathActivateStage)?;
        let role = self.validate_stage_admission(&message.proposal, facts)?;
        if let Some(action) = self.existing_stage_action(message, facts)? {
            return Ok(action);
        }
        let activation_index = self.free_activation_slot()?;
        let flow_index = self.free_flow_slot()?;
        let receipt_index = self.free_receipt_slot()?;
        let stage_deadline_us = facts
            .now_us
            .checked_add(self.config.stage_lifetime_us)
            .ok_or(Error::Exhausted)?
            .min(message.proposal.flow_expires_at_us)
            .min(facts.capability_deadline_us);
        let commit_deadline_us = stage_deadline_us
            .checked_add(self.config.commit_lifetime_us)
            .ok_or(Error::Exhausted)?
            .min(message.proposal.flow_expires_at_us);
        if facts.now_us >= stage_deadline_us || stage_deadline_us >= commit_deadline_us {
            return Err(Error::Timeout);
        }
        let generation = self.activations[activation_index]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let phase = if role == FlowRole::Target {
            FlowPhase::Staged
        } else {
            FlowPhase::StagePending
        };
        self.activations[activation_index] = ActivationSlot {
            generation,
            value: Some(ActivationRecord {
                role,
                proposal: message.proposal,
                phase,
                stage_submitted: role == FlowRole::Target,
                commit_submitted: false,
                abort_submitted: false,
                upstream: Some(facts.upstream),
                downstream: facts.downstream,
                local_capability_deadline_us: facts.capability_deadline_us,
                local_security_owner_instance: facts.security_owner_instance,
                local_peer_session_generation: facts.peer_session_generation,
                local_origin_key_generation: facts.origin_key_generation,
                flow_slot: flow_index,
                receipt_slot: receipt_index,
                stage_deadline_us,
                commit_deadline_us,
            }),
        };
        let activation = self.activation_handle(activation_index, message.key)?;
        if role == FlowRole::Target {
            Ok(RelayStageAction::Ack {
                ack: activation_ack(ProtocolOpcode::PathStageAck, &message.proposal),
                upstream: facts.upstream,
            })
        } else {
            Ok(RelayStageAction::Forward {
                downstream: facts.downstream.ok_or(Error::State)?,
                activation,
            })
        }
    }

    /// Relay 收到下游 Stage ACK 后才进入 Staged 并向上游返回 ACK。
    ///
    /// # Errors
    ///
    /// 非 Relay、错键、未提交或过期时不改状态。
    pub fn relay_on_stage_ack(
        &mut self,
        activation: ActivationHandle,
        ack: ActivationAck,
        facts: FlowHopFacts,
    ) -> Result<ActivationAck> {
        let index = self.activation_index(activation)?;
        let mut record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.role != FlowRole::Relay
            || record.phase != FlowPhase::StagePending
            || !record.stage_submitted
            || ack.opcode != ProtocolOpcode::PathStageAck
            || ack.key != record.proposal.key
        {
            return Err(Error::State);
        }
        self.validate_hop_facts(&record, facts)?;
        if facts.now_us >= record.stage_deadline_us {
            return Err(Error::Timeout);
        }
        record.phase = FlowPhase::Staged;
        self.activations[index].value = Some(record);
        Ok(activation_ack(
            ProtocolOpcode::PathStageAck,
            &record.proposal,
        ))
    }

    /// Relay 报告向下游的 Stage submit。
    ///
    /// # Errors
    ///
    /// Handle、角色或状态不匹配时失败且事务不变。
    pub fn relay_complete_stage_submit(
        &mut self,
        activation: ActivationHandle,
        outcome: ActivationSubmit,
    ) -> Result<()> {
        let index = self.activation_index(activation)?;
        if self.activations[index].value.ok_or(Error::NotFound)?.role != FlowRole::Relay {
            return Err(Error::State);
        }
        self.complete_stage_submit(activation, outcome)
    }

    /// Relay/Target 接纳 Commit。Target 先发布再 ACK；Relay 先转发且仍不可 Active。
    ///
    /// # Errors
    ///
    /// 未 Staged、错键或到期时不写状态。
    pub fn accept_commit(
        &mut self,
        message: &ActivationMessage,
        facts: FlowHopFacts,
    ) -> Result<RelayCommitAction> {
        validate_activation_message(message, ProtocolOpcode::PathActivateCommit)?;
        let (index, existing) = self.find_activation(message.key).ok_or(Error::NotFound)?;
        if existing.proposal != message.proposal {
            return Err(Error::Security);
        }
        self.validate_hop_facts(&existing, facts)?;
        if existing.phase == FlowPhase::Active {
            if !matches!(existing.role, FlowRole::Target | FlowRole::Relay) {
                return Err(Error::State);
            }
            return Ok(RelayCommitAction::Ack {
                ack: activation_ack(ProtocolOpcode::PathCommitAck, &existing.proposal),
                upstream: existing.upstream.ok_or(Error::State)?,
                flow: self.active_flow_handle(&existing)?,
            });
        }
        if existing.role == FlowRole::Relay && existing.phase == FlowPhase::Committing {
            return Ok(RelayCommitAction::Forward {
                downstream: existing.downstream.ok_or(Error::State)?,
                activation: self.activation_handle(index, existing.proposal.key)?,
            });
        }
        if existing.phase != FlowPhase::Staged {
            return Err(Error::State);
        }
        if facts.now_us >= existing.commit_deadline_us {
            return Err(Error::Timeout);
        }
        match existing.role {
            FlowRole::Target => {
                if existing.proposal.key.route_domain.destination != self.config.local {
                    return Err(Error::Access);
                }
                let flow = self.publish_activation(index, FlowPhase::Active, facts.now_us)?;
                Ok(RelayCommitAction::Ack {
                    ack: activation_ack(ProtocolOpcode::PathCommitAck, &existing.proposal),
                    upstream: existing.upstream.ok_or(Error::State)?,
                    flow,
                })
            }
            FlowRole::Relay => {
                let downstream = existing.downstream.ok_or(Error::State)?;
                let mut record = existing;
                record.phase = FlowPhase::Committing;
                self.activations[index].value = Some(record);
                Ok(RelayCommitAction::Forward {
                    downstream,
                    activation: self.activation_handle(index, existing.proposal.key)?,
                })
            }
            FlowRole::Origin => Err(Error::State),
        }
    }

    /// Relay 收到下游 Commit ACK 后才发布本地 Active，再向上游 ACK。
    ///
    /// # Errors
    ///
    /// 错键、未提交、依赖失效或到期时不发布 Active。
    pub fn relay_on_commit_ack(
        &mut self,
        activation: ActivationHandle,
        ack: ActivationAck,
        facts: FlowHopFacts,
    ) -> Result<(FlowHandle, ActivationAck)> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.role != FlowRole::Relay
            || ack.opcode != ProtocolOpcode::PathCommitAck
            || ack.key != record.proposal.key
        {
            return Err(Error::State);
        }
        self.validate_hop_facts(&record, facts)?;
        if record.phase == FlowPhase::Active {
            return Ok((
                self.active_flow_handle(&record)?,
                activation_ack(ProtocolOpcode::PathCommitAck, &record.proposal),
            ));
        }
        if record.phase != FlowPhase::Committing || !record.commit_submitted {
            return Err(Error::State);
        }
        if facts.now_us >= record.commit_deadline_us {
            return Err(Error::Timeout);
        }
        let handle = self.publish_activation(index, FlowPhase::Active, facts.now_us)?;
        Ok((
            handle,
            activation_ack(ProtocolOpcode::PathCommitAck, &record.proposal),
        ))
    }

    /// Origin 在 Commit 尚未可能被接受前启动精确 Abort。
    ///
    /// Stage 从未提交时只完成本地终止并返回 `None`；否则返回必须沿冻结路径发送的
    /// `PathActivateAbort`。重复调用返回相同消息且不刷新事务 Deadline。
    ///
    /// # Errors
    ///
    /// 非 Origin、已 Active/InDoubt，或 Commit 已提交时失败且事务不变。
    pub fn begin_abort(
        &mut self,
        activation: ActivationHandle,
        now_us: u64,
    ) -> Result<Option<ActivationMessage>> {
        let index = self.activation_index(activation)?;
        let mut record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.role != FlowRole::Origin {
            return Err(Error::State);
        }
        if record.phase == FlowPhase::Fenced {
            return Ok(None);
        }
        if record.phase == FlowPhase::Aborting {
            return Ok(Some(activation_message(
                ProtocolOpcode::PathActivateAbort,
                record.proposal,
            )));
        }
        let may_abort = matches!(record.phase, FlowPhase::StagePending | FlowPhase::Staged)
            || (record.phase == FlowPhase::Committing && !record.commit_submitted);
        if !may_abort || record.commit_submitted {
            return Err(Error::State);
        }
        if !record.stage_submitted {
            self.finish_precommit_abort(index, now_us)?;
            return Ok(None);
        }
        record.phase = FlowPhase::Aborting;
        record.abort_submitted = false;
        self.activations[index].value = Some(record);
        Ok(Some(activation_message(
            ProtocolOpcode::PathActivateAbort,
            record.proposal,
        )))
    }

    /// 返回同一 Abort 事务，供明确未提交或 receipt 丢失后的有界重试使用。
    ///
    /// # Errors
    ///
    /// Handle 或阶段不匹配时返回错误，不刷新任何 Deadline。
    pub fn abort_message(&self, activation: ActivationHandle) -> Result<ActivationMessage> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Aborting {
            return Err(Error::State);
        }
        Ok(activation_message(
            ProtocolOpcode::PathActivateAbort,
            record.proposal,
        ))
    }

    /// 报告 Abort submit 结果。
    ///
    /// `InDoubt` 仅表示 Abort 是否到达未知，不表示 Commit 可回滚；事务继续保持
    /// `Aborting`，直到精确 receipt 或原 Stage lease 收敛。
    ///
    /// # Errors
    ///
    /// 非 Origin/Relay 或状态不匹配时失败且事务不变。
    pub fn complete_abort_submit(
        &mut self,
        activation: ActivationHandle,
        outcome: ActivationSubmit,
    ) -> Result<()> {
        let index = self.activation_index(activation)?;
        let mut record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Aborting || record.role == FlowRole::Target {
            return Err(Error::State);
        }
        match outcome {
            ActivationSubmit::NotSubmitted => record.abort_submitted = false,
            ActivationSubmit::Submitted | ActivationSubmit::InDoubt => {
                record.abort_submitted = true;
            }
        }
        self.activations[index].value = Some(record);
        Ok(())
    }

    /// Relay/Target 接纳精确 Abort。
    ///
    /// Relay 只有在从未向下游提交 Stage 时才能本地终止；否则必须先转发并等待下游
    /// terminal receipt。任何已提交 Commit、Active 或 `InDoubt` 状态都拒绝回滚。
    ///
    /// # Errors
    ///
    /// 消息、父事实、阶段或 Deadline 不匹配时失败且不改变事务。
    pub fn accept_abort(
        &mut self,
        message: &ActivationMessage,
        facts: FlowHopFacts,
    ) -> Result<RelayAbortAction> {
        validate_activation_message(message, ProtocolOpcode::PathActivateAbort)?;
        let (index, existing) = self.find_activation(message.key).ok_or(Error::NotFound)?;
        if existing.proposal != message.proposal {
            return Err(Error::Security);
        }
        self.validate_hop_facts(&existing, facts)?;
        if existing.role == FlowRole::Origin
            || existing.commit_submitted
            || matches!(existing.phase, FlowPhase::Active | FlowPhase::InDoubt)
        {
            return Err(Error::State);
        }
        if existing.phase == FlowPhase::Fenced {
            let receipt = self.receipts[existing.receipt_slot]
                .value
                .ok_or(Error::State)?;
            if receipt.key != existing.proposal.key
                || receipt.phase != FlowPhase::Fenced
                || facts.now_us >= receipt.expires_at_us
            {
                return Err(Error::State);
            }
            return Ok(RelayAbortAction::Ack {
                ack: activation_ack(ProtocolOpcode::PathTerminalReceipt, &existing.proposal),
                upstream: existing.upstream.ok_or(Error::State)?,
            });
        }
        if !matches!(
            existing.phase,
            FlowPhase::StagePending
                | FlowPhase::Staged
                | FlowPhase::Committing
                | FlowPhase::Aborting
        ) || (existing.phase == FlowPhase::Committing && existing.commit_submitted)
        {
            return Err(Error::State);
        }
        if existing.role == FlowRole::Relay && existing.stage_submitted {
            let mut aborting = existing;
            aborting.phase = FlowPhase::Aborting;
            self.activations[index].value = Some(aborting);
            return Ok(RelayAbortAction::Forward {
                downstream: existing.downstream.ok_or(Error::State)?,
                activation: self.activation_handle(index, existing.proposal.key)?,
            });
        }
        self.finish_precommit_abort(index, facts.now_us)?;
        Ok(RelayAbortAction::Ack {
            ack: activation_ack(ProtocolOpcode::PathTerminalReceipt, &existing.proposal),
            upstream: existing.upstream.ok_or(Error::State)?,
        })
    }

    /// Relay 收到下游 terminal receipt 后终止本地 Stage，并向上游返回相同 receipt。
    ///
    /// # Errors
    ///
    /// 非 Relay、Abort 未提交、错键或父事实失效时失败且不改变事务。
    pub fn relay_on_abort_receipt(
        &mut self,
        activation: ActivationHandle,
        ack: ActivationAck,
        facts: FlowHopFacts,
    ) -> Result<ActivationAck> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.role != FlowRole::Relay
            || record.phase != FlowPhase::Aborting
            || !record.abort_submitted
            || ack.opcode != ProtocolOpcode::PathTerminalReceipt
            || ack.key != record.proposal.key
        {
            return Err(Error::State);
        }
        self.validate_hop_facts(&record, facts)?;
        self.finish_precommit_abort(index, facts.now_us)?;
        Ok(activation_ack(
            ProtocolOpcode::PathTerminalReceipt,
            &record.proposal,
        ))
    }

    /// Origin 收到第一跳 terminal receipt 后完成本地 Abort。
    ///
    /// # Errors
    ///
    /// 非 Origin、Abort 未提交或 receipt 错绑时失败且不改变事务。
    pub fn on_abort_receipt(
        &mut self,
        activation: ActivationHandle,
        ack: ActivationAck,
        now_us: u64,
    ) -> Result<()> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.role != FlowRole::Origin
            || record.phase != FlowPhase::Aborting
            || !record.abort_submitted
            || ack.opcode != ProtocolOpcode::PathTerminalReceipt
            || ack.key != record.proposal.key
        {
            return Err(Error::State);
        }
        self.finish_precommit_abort(index, now_us)
    }

    /// Commit 后超时或未知发送结果进入 `IN_DOUBT`。
    ///
    /// Commit 前到期时，Target 或从未向下游提交 Stage 的节点可证明没有待撤销的
    /// 远端副作用，因此直接进入 `FENCED` 并保留 terminal receipt；已经提交 Stage
    /// 的 Origin/Relay 才进入 `ABORTING`，等待精确的下游撤销链收敛。
    ///
    /// # Errors
    ///
    /// Handle 不存在或尚未到期时返回错误。
    pub fn expire_activation(
        &mut self,
        activation: ActivationHandle,
        now_us: u64,
    ) -> Result<FlowPhase> {
        let index = self.activation_index(activation)?;
        let mut record = self.activations[index].value.ok_or(Error::NotFound)?;
        match record.phase {
            FlowPhase::StagePending | FlowPhase::Staged if now_us >= record.stage_deadline_us => {
                if record.role == FlowRole::Target || !record.stage_submitted {
                    self.finish_precommit_abort(index, now_us)?;
                    Ok(FlowPhase::Fenced)
                } else {
                    record.phase = FlowPhase::Aborting;
                    record.abort_submitted = false;
                    self.activations[index].value = Some(record);
                    Ok(FlowPhase::Aborting)
                }
            }
            FlowPhase::Committing if now_us >= record.commit_deadline_us => {
                record.phase = FlowPhase::InDoubt;
                self.activations[index].value = Some(record);
                Ok(FlowPhase::InDoubt)
            }
            _ => Err(Error::State),
        }
    }

    /// 认证 terminal receipt 使 `InDoubt` 单向收敛。
    ///
    /// # Errors
    ///
    /// 事务不处于 InDoubt、键不匹配或 receipt 过期时不写状态。
    pub fn reconcile_terminal(
        &mut self,
        activation: ActivationHandle,
        ack: ActivationAck,
        outcome: TerminalOutcome,
        facts: FlowCurrentFacts,
    ) -> Result<Option<FlowHandle>> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::InDoubt
            || ack.opcode != ProtocolOpcode::PathTerminalReceipt
            || ack.key != record.proposal.key
        {
            return Err(Error::State);
        }
        match outcome {
            TerminalOutcome::Committed => {
                self.validate_use_facts(&record.proposal, facts)?;
                self.publish_activation(index, FlowPhase::Active, facts.now_us)
                    .map(Some)
            }
            TerminalOutcome::Excluded => {
                let expires_at_us = facts
                    .now_us
                    .checked_add(self.config.receipt_lifetime_us)
                    .ok_or(Error::Exhausted)?;
                self.receipts[record.receipt_slot].value = Some(FlowReceipt {
                    key: record.proposal.key,
                    phase: FlowPhase::Fenced,
                    expires_at_us,
                });
                let mut fenced = record;
                fenced.phase = FlowPhase::Fenced;
                self.activations[index].value = Some(fenced);
                Ok(None)
            }
        }
    }

    /// 在每次业务发送前复核全部冻结依赖；硬失配会立即 Fence。
    ///
    /// # Errors
    ///
    /// Handle、状态、当前事实或本次业务要求不满足时失败，不返回可编码计划。
    pub fn flow_use_preflight(
        &mut self,
        flow: FlowHandle,
        facts: FlowCurrentFacts,
        request: FlowTxRequest,
    ) -> Result<FlowTxPlan> {
        let index = self.flow_index(flow)?;
        let mut record = self.flows[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Active {
            return Err(Error::State);
        }
        if let Err(error) = self.validate_use_facts(&record.proposal, facts) {
            record.phase = FlowPhase::Fenced;
            self.flows[index].value = Some(record);
            return Err(error);
        }
        let required = record.proposal.requirements;
        if request.contract != required.contract
            || u8::from(request.traffic_class) < u8::from(required.traffic_ceiling)
            || request.delivery != required.delivery
            || request.interaction != required.interaction
            || request.payload_kind != required.payload_kind
            || request.protocol_opcode != required.protocol_opcode
            || request.origin_security != required.origin_security
            || request.payload_bytes > record.proposal.payload_budget
        {
            return Err(Error::Policy);
        }
        Ok(FlowTxPlan {
            contract: required.contract,
            context_id: record.proposal.context_id,
            forward_label: record.proposal.key.forward_label,
            route_generation: record.proposal.key.route_generation,
            next_hop: record.proposal.next_hop,
            flow_fingerprint: record.proposal.fingerprint()?,
            payload_budget: record.proposal.payload_budget,
            hop_profile: required.hop_profile,
            origin_sequence: if required.origin_security == OriginSecurity::O0 {
                None
            } else {
                Some(OriginSequence::new(
                    self.next_origin_sequence.ok_or(Error::Exhausted)?,
                )?)
            },
        })
    }

    /// Origin 在使用前重验成功后导出完整 Security Requirement。
    ///
    /// 返回值只是由 Coordinator 路由给 Security Owner 的不可变 Requirement，不是授权；
    /// 真正不可伪造的 `FlowSecurityBinding` 仍由 Security Owner 在当前 Session、ACL、Policy
    /// 和 Deadline 全部通过后签发。
    ///
    /// # Errors
    ///
    /// Flow 不是 Origin Active、当前父事实或业务要求失配时失败；硬父事实失配会 Fence Flow。
    pub fn origin_security_request(
        &mut self,
        flow: FlowHandle,
        facts: FlowCurrentFacts,
        request: FlowTxRequest,
        session: SessionHandle,
    ) -> Result<(FlowTxPlan, FlowSecurityRequest)> {
        let plan = self.flow_use_preflight(flow, facts, request)?;
        let index = self.flow_index(flow)?;
        let record = self.flows[index].value.ok_or(Error::NotFound)?;
        if record.role != FlowRole::Origin {
            return Err(Error::State);
        }
        let security = make_flow_security_request(&record, session, AccessDirection::Outbound)?;
        Ok((plan, security))
    }

    /// Relay/Target 以当前逐跳事实导出完整 Security Requirement。
    ///
    /// 正向 Flow 的入站与出站都绑定 forward label；反向业务应使用独立反向 Flow，或由
    /// RUST-08 的 receipt/ACK 合同拥有，不能偷用本 Flow 的 reverse label。
    ///
    /// # Errors
    ///
    /// Flow 不是 Relay/Target Active、方向没有对应 Peer 或父事实失配时失败；硬失配会 Fence。
    pub fn hop_security_request(
        &mut self,
        flow: FlowHandle,
        facts: FlowHopFacts,
        session: SessionHandle,
        direction: AccessDirection,
    ) -> Result<FlowSecurityRequest> {
        let index = self.flow_index(flow)?;
        let mut record = self.flows[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Active || record.role == FlowRole::Origin {
            return Err(Error::State);
        }
        if !flow_parent_is_current(&record, self.config, facts) {
            record.phase = FlowPhase::Fenced;
            self.flows[index].value = Some(record);
            return Err(Error::State);
        }
        let _peer = match direction {
            AccessDirection::Inbound => record.upstream.ok_or(Error::State)?,
            AccessDirection::Outbound => record.downstream.ok_or(Error::State)?,
        };
        make_flow_security_request(&record, session, direction)
    }

    /// Relay 对 C3/C4 执行 Label 快路径预检；不解析 E2E Payload。
    ///
    /// # Errors
    ///
    /// Flow、逐跳父事实、Label、Context、代际、Traffic 或 MTU 不成立时返回错误；父事实硬
    /// 失配会把该 Flow 立即 Fence。
    pub fn forward_preflight(
        &mut self,
        flow: FlowHandle,
        facts: FlowHopFacts,
        request: FlowForwardRequest,
    ) -> Result<FlowForwardPlan> {
        let index = self.flow_index(flow)?;
        let mut record = self.flows[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Active {
            return Err(Error::State);
        }
        if !flow_parent_is_current(&record, self.config, facts) {
            record.phase = FlowPhase::Fenced;
            self.flows[index].value = Some(record);
            return Err(Error::State);
        }
        let requirements = record.proposal.requirements;
        let egress = record.downstream.ok_or(Error::State)?;
        if !matches!(
            requirements.contract,
            HeaderContract::C3 | HeaderContract::C4
        ) || request.ingress != facts.upstream
            || request.label != record.proposal.key.forward_label
            || request.context_id != record.proposal.context_id
            || request.route_generation != record.proposal.key.route_generation
            || u8::from(request.traffic_class) < u8::from(requirements.traffic_ceiling)
            || request.packet_bytes > record.proposal.path_frame_mtu
        {
            return Err(Error::Policy);
        }
        Ok(FlowForwardPlan {
            egress,
            egress_label: record.proposal.key.forward_label,
            egress_context_id: record.proposal.context_id,
            hop_profile: requirements.hop_profile,
        })
    }

    /// Target 用 Security 签发的不可伪造 Claim 预留本 Flow 的 Origin Replay mutation。
    ///
    /// # Errors
    ///
    /// Flow、逐跳父事实、Security Owner/Session/Key、fingerprint、Deadline 或固定单槽
    /// reservation 不成立时失败。Duplicate 只返回分类，不再次开放业务副作用。
    pub fn reserve_authenticated_rx(
        &mut self,
        flow: FlowHandle,
        facts: FlowHopFacts,
        claim: FlowReplayClaim,
    ) -> Result<FlowReplayAdmission> {
        let index = self.flow_index(flow)?;
        let mut record = self.flows[index].value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Active || record.role != FlowRole::Target {
            return Err(Error::State);
        }
        if !flow_parent_is_current(&record, self.config, facts) {
            record.phase = FlowPhase::Fenced;
            self.flows[index].value = Some(record);
            return Err(Error::State);
        }
        if claim.security_owner_instance() != record.local_security_owner_instance
            || claim.session_generation() != record.local_peer_session_generation
            || claim.key_generation() != record.local_origin_key_generation
            || claim.flow_fingerprint() != record.proposal.fingerprint()?
            || facts.now_us >= claim.reservation_deadline_us()
            || claim.reservation_deadline_us() > record.proposal.flow_expires_at_us
            || claim.reservation_deadline_us() > record.local_capability_deadline_us
            || claim.aad_digest() == [0; 16]
            || claim.payload_digest() == [0; 16]
        {
            return Err(Error::Security);
        }
        if record.replay_reservation.is_some() {
            return Err(Error::NoSpace);
        }
        let sequence = claim.sequence().get();
        if record.replay_initialized && sequence <= record.replay_highest {
            let distance = record.replay_highest - sequence;
            if distance >= 64 {
                return Err(Error::Replay);
            }
            if record.replay_bitmap & (1_u64 << distance) != 0 {
                return Ok(FlowReplayAdmission::Duplicate);
            }
        }
        let generation = record
            .replay_reservation_generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        record.replay_reservation_generation = generation;
        record.replay_reservation = Some(FlowReplayReservation {
            sequence: claim.sequence(),
            deadline_us: claim.reservation_deadline_us(),
            aad_digest: claim.aad_digest(),
            payload_digest: claim.payload_digest(),
        });
        self.flows[index].value = Some(record);
        Ok(FlowReplayAdmission::Fresh(FlowReplayHandle {
            owner_instance: self.config.owner_instance,
            flow_slot: flow.slot,
            flow_slot_generation: flow.slot_generation,
            reservation_generation: generation,
            sequence: claim.sequence(),
            reservation_deadline_us: claim.reservation_deadline_us(),
            aad_digest: claim.aad_digest(),
            payload_digest: claim.payload_digest(),
        }))
    }

    /// 在业务资源预检成功后提交精确 Flow Replay reservation。
    ///
    /// # Errors
    ///
    /// Handle、Deadline 或 reservation 字节不匹配时失败且不推进窗口。
    pub fn commit_authenticated_rx(&mut self, handle: FlowReplayHandle, now_us: u64) -> Result<()> {
        let (index, mut record, reservation) = self.flow_replay_reservation(handle)?;
        if now_us >= handle.reservation_deadline_us {
            return Err(Error::Timeout);
        }
        let sequence = reservation.sequence.get();
        if !record.replay_initialized {
            record.replay_initialized = true;
            record.replay_highest = sequence;
            record.replay_bitmap = 1;
        } else if sequence > record.replay_highest {
            let shift = sequence - record.replay_highest;
            record.replay_bitmap = if shift >= 64 {
                1
            } else {
                (record.replay_bitmap << shift) | 1
            };
            record.replay_highest = sequence;
        } else {
            let distance = record.replay_highest - sequence;
            if distance >= 64 {
                return Err(Error::Replay);
            }
            record.replay_bitmap |= 1_u64 << distance;
        }
        record.replay_reservation = None;
        self.flows[index].value = Some(record);
        Ok(())
    }

    /// 放弃未产生业务副作用的 Flow Replay reservation。
    ///
    /// # Errors
    ///
    /// Handle 已失效或不精确时返回错误。
    pub fn abort_authenticated_rx(&mut self, handle: FlowReplayHandle) -> Result<()> {
        let (index, mut record, _) = self.flow_replay_reservation(handle)?;
        record.replay_reservation = None;
        self.flows[index].value = Some(record);
        Ok(())
    }

    /// 显式围栏一个仍存在的 Flow，并取消尚未提交的 Replay reservation。
    ///
    /// 已围栏 Flow 只能完成上层 obligation 后再退休，不能恢复为 Active。
    ///
    /// # Errors
    ///
    /// Handle 不匹配或 Flow 已不在可围栏阶段时返回错误。
    pub fn fence_flow(&mut self, flow: FlowHandle) -> Result<()> {
        let index = self.flow_index(flow)?;
        let mut record = self.flows[index].value.ok_or(Error::NotFound)?;
        if !matches!(
            record.phase,
            FlowPhase::Active | FlowPhase::Draining | FlowPhase::Fenced
        ) {
            return Err(Error::State);
        }
        record.phase = FlowPhase::Fenced;
        record.replay_reservation = None;
        self.flows[index].value = Some(record);
        Ok(())
    }

    /// 退休已经 Draining/Fenced 且没有 Replay obligation 的 Flow。
    ///
    /// 槽可被后续 Flow 复用，但槽代际会递增，因此旧 Handle 永远不能命中新对象。
    ///
    /// # Errors
    ///
    /// Active、存在 Replay reservation 或 Handle 不匹配时返回错误且不清槽。
    pub fn retire_flow(&mut self, flow: FlowHandle) -> Result<()> {
        let index = self.flow_index(flow)?;
        let record = self.flows[index].value.ok_or(Error::NotFound)?;
        if !matches!(record.phase, FlowPhase::Draining | FlowPhase::Fenced)
            || record.replay_reservation.is_some()
        {
            return Err(Error::State);
        }
        self.flows[index].value = None;
        Ok(())
    }

    /// 退休已终止的 Activation transaction。
    ///
    /// Active transaction 只有在对应 Flow 已退休且 terminal receipt 已过期/清除后才能释放；
    /// `IN_DOUBT` 永远不能由该入口丢弃。
    ///
    /// # Errors
    ///
    /// 仍有 Flow/Receipt obligation 或阶段不允许时返回错误。
    pub fn retire_activation(&mut self, activation: ActivationHandle) -> Result<()> {
        let index = self.activation_index(activation)?;
        let record = self.activations[index].value.ok_or(Error::NotFound)?;
        let terminal = matches!(record.phase, FlowPhase::Aborting | FlowPhase::Fenced)
            || (record.phase == FlowPhase::Active
                && self.flows[record.flow_slot].value.is_none()
                && self.receipts[record.receipt_slot].value.is_none());
        if !terminal {
            return Err(Error::State);
        }
        self.activations[index].value = None;
        Ok(())
    }

    /// 退休尚未形成 Activation，或其 Activation 已明确退休的 Candidate。
    ///
    /// # Errors
    ///
    /// 仍有同 Proposal Activation obligation 时返回错误。
    pub fn retire_candidate(&mut self, candidate: CandidateHandle) -> Result<()> {
        let index = self.candidate_index(candidate)?;
        let record = self.candidates[index].value.ok_or(Error::NotFound)?;
        if record.proposal.is_some_and(|proposal| {
            self.activations.iter().any(|slot| {
                slot.value
                    .is_some_and(|activation| activation.proposal == proposal)
            })
        }) {
            return Err(Error::State);
        }
        self.candidates[index].value = None;
        Ok(())
    }

    /// 返回 Flow 当前阶段。
    ///
    /// # Errors
    ///
    /// Handle 不匹配时返回错误。
    pub fn flow_phase(&self, flow: FlowHandle) -> Result<FlowPhase> {
        Ok(self.flows[self.flow_index(flow)?]
            .value
            .ok_or(Error::NotFound)?
            .phase)
    }

    /// 返回 Activation 当前阶段。
    ///
    /// # Errors
    ///
    /// Handle 不属于当前 Owner 或已退休时返回错误。
    pub fn activation_phase(&self, activation: ActivationHandle) -> Result<FlowPhase> {
        Ok(self.activations[self.activation_index(activation)?]
            .value
            .ok_or(Error::NotFound)?
            .phase)
    }

    /// 返回 Candidate 当前阶段和已冻结 Proposal。
    ///
    /// # Errors
    ///
    /// Handle 不匹配时返回错误。
    pub fn candidate_view(
        &self,
        candidate: CandidateHandle,
    ) -> Result<(FlowPhase, Option<FlowProposal>)> {
        let record = self.candidates[self.candidate_index(candidate)?]
            .value
            .ok_or(Error::NotFound)?;
        Ok((record.phase, record.proposal))
    }

    /// 返回固定表占用数。
    #[must_use]
    pub fn counts(&self) -> (usize, usize, usize, usize) {
        (
            self.candidates
                .iter()
                .filter(|slot| slot.value.is_some())
                .count(),
            self.activations
                .iter()
                .filter(|slot| slot.value.is_some())
                .count(),
            self.flows
                .iter()
                .filter(|slot| slot.value.is_some())
                .count(),
            self.receipts
                .iter()
                .filter(|slot| slot.value.is_some())
                .count(),
        )
    }

    /// 持久游标每次只推进一个 Candidate、Activation、Flow 或 Receipt 槽。
    ///
    /// 维护只把过期状态推进到可审计的终态：过期 Active 变为 Fenced，Commit 不确定变为
    /// InDoubt，绝不隐式驱逐 Active/Draining/Fenced/InDoubt 或未到期 receipt。
    #[must_use]
    pub fn maintain_one(&mut self, now_us: u64) -> bool {
        let total = CANDIDATES + ACTIVATIONS + FLOWS + RECEIPTS;
        let index = self.cleanup_cursor;
        self.cleanup_cursor = (self.cleanup_cursor + 1) % total;
        if index < CANDIDATES {
            let expired = self.candidates[index].value.is_some_and(|record| {
                match (record.phase, record.proposal) {
                    (FlowPhase::Candidate, None) => {
                        now_us >= record.source_route.expires_at_us()
                            || now_us >= record.capability.capability_deadline_us
                            || now_us >= record.capability.discovery_deadline_us
                            || now_us >= record.requirements.expires_at_us
                    }
                    (FlowPhase::Probing | FlowPhase::ReadyToStage, Some(proposal)) => {
                        now_us >= proposal.probe_deadline_us
                    }
                    _ => false,
                }
            });
            if expired {
                self.candidates[index].value = None;
            }
            return expired;
        }
        if index < CANDIDATES + ACTIVATIONS {
            let activation_index = index - CANDIDATES;
            let Some(mut record) = self.activations[activation_index].value else {
                return false;
            };
            let next = match record.phase {
                FlowPhase::StagePending | FlowPhase::Staged
                    if now_us >= record.stage_deadline_us =>
                {
                    if record.role == FlowRole::Target || !record.stage_submitted {
                        return self
                            .finish_precommit_abort(activation_index, now_us)
                            .is_ok();
                    }
                    record.abort_submitted = false;
                    Some(FlowPhase::Aborting)
                }
                FlowPhase::Committing if now_us >= record.commit_deadline_us => {
                    Some(FlowPhase::InDoubt)
                }
                _ => None,
            };
            if let Some(phase) = next {
                record.phase = phase;
                self.activations[activation_index].value = Some(record);
                return true;
            }
            return false;
        }
        if index < CANDIDATES + ACTIVATIONS + FLOWS {
            let flow_index = index - CANDIDATES - ACTIVATIONS;
            let Some(mut record) = self.flows[flow_index].value else {
                return false;
            };
            if record
                .replay_reservation
                .is_some_and(|reservation| now_us >= reservation.deadline_us)
            {
                record.replay_reservation = None;
                self.flows[flow_index].value = Some(record);
                return true;
            }
            if record.phase == FlowPhase::Active && now_us >= record.proposal.flow_expires_at_us {
                record.phase = FlowPhase::Fenced;
                self.flows[flow_index].value = Some(record);
                return true;
            }
            return false;
        }
        let receipt_index = index - CANDIDATES - ACTIVATIONS - FLOWS;
        if self.receipts[receipt_index]
            .value
            .is_some_and(|receipt| now_us >= receipt.expires_at_us)
        {
            self.receipts[receipt_index].value = None;
            return true;
        }
        false
    }

    fn validate_candidate_inputs(
        &self,
        route: SoftRouteView,
        capability: CachedPeerCapability,
        requirements: FlowRequirements,
        now_us: u64,
    ) -> Result<()> {
        requirements.validate(route.hop_count(), now_us)?;
        let required_route_bits =
            u16::try_from(requirements.required_feature_bits).map_err(|_| Error::State)?;
        if route.domain().realm != self.config.realm
            || route.domain().origin != self.config.local
            || route.next_hop().peer() != capability.peer_ref.binding
            || route.next_hop().link_id() != capability.peer_ref.ingress_link_id
            || route.next_hop().link_generation().get()
                != capability.peer_ref.ingress_link_generation
            || capability.record.link.link_instance_generation
                != capability.peer_ref.ingress_link_generation
            || now_us >= route.expires_at_us()
            || now_us >= capability.capability_deadline_us
            || now_us >= capability.discovery_deadline_us
            || requirements.policy_generation != self.config.policy_generation
            || requirements.required_feature_bits & !capability.record.peer.feature_bits != 0
            || required_route_bits & !route.capability_bits() != 0
            || u32::from(requirements.minimum_payload_bytes) > capability.record.link.link_frame_mtu
            || requirements.minimum_payload_bytes > route.path_frame_mtu()
        {
            return Err(Error::State);
        }
        Ok(())
    }

    fn validate_use_facts(&self, proposal: &FlowProposal, facts: FlowCurrentFacts) -> Result<()> {
        if facts.now_us >= proposal.flow_expires_at_us
            || facts.now_us >= facts.capability_deadline_us
            || facts.local != self.config.local
            || facts.destination != proposal.key.route_domain.destination
            || facts.origin_session_generation
                != proposal.key.route_domain.origin_session_generation.get()
            || facts.next_hop != proposal.next_hop
            || facts.capability_ref.runtime_instance != proposal.capability_runtime_instance
            || facts.capability_ref.security_owner_instance
                != proposal.capability_security_owner_instance
            || facts.capability_ref.binding != proposal.next_hop.peer()
            || facts.capability_ref.session_generation != proposal.capability_session_generation
            || facts.capability_ref.ingress_link_id != proposal.next_hop.link_id()
            || facts.capability_ref.ingress_link_generation
                != proposal.next_hop.link_generation().get()
            || facts.capability_generation != proposal.capability_generation
            || facts.capability_digest != proposal.capability_digest
            || facts.capability_deadline_us != proposal.capability_deadline_us
            || facts.policy_generation != proposal.requirements.policy_generation
        {
            return Err(Error::State);
        }
        Ok(())
    }

    fn validate_stage_admission(
        &self,
        proposal: &FlowProposal,
        facts: FlowHopFacts,
    ) -> Result<FlowRole> {
        let target = proposal.key.route_domain.destination == self.config.local;
        let relay = proposal.key.route_domain.origin != self.config.local && !target;
        if facts.local != self.config.local
            || facts.policy_generation != self.config.policy_generation
            || facts.path_frame_mtu < proposal.path_frame_mtu
            || proposal.requirements.required_feature_bits & !u32::from(facts.capability_bits) != 0
            || facts.hop_profile != proposal.requirements.hop_profile
            || facts.now_us >= facts.capability_deadline_us
            || facts.now_us >= proposal.flow_expires_at_us
            || facts.upstream.peer() == self.config.local
            || facts
                .downstream
                .is_some_and(|link| link.peer() == self.config.local)
            || (target && facts.downstream.is_some())
            || (relay && facts.downstream.is_none())
            || (!target && !relay)
        {
            return Err(Error::State);
        }
        if target {
            Ok(FlowRole::Target)
        } else {
            Ok(FlowRole::Relay)
        }
    }

    fn existing_stage_action(
        &self,
        message: &ActivationMessage,
        facts: FlowHopFacts,
    ) -> Result<Option<RelayStageAction>> {
        let Some((index, existing)) = self.find_activation(message.key) else {
            return Ok(None);
        };
        if existing.proposal != message.proposal {
            return Err(Error::Security);
        }
        self.validate_hop_facts(&existing, facts)?;
        let action = match (existing.role, existing.phase, existing.downstream) {
            (FlowRole::Target | FlowRole::Relay, FlowPhase::Staged | FlowPhase::Active, _) => {
                RelayStageAction::Ack {
                    ack: activation_ack(ProtocolOpcode::PathStageAck, &existing.proposal),
                    upstream: existing.upstream.ok_or(Error::State)?,
                }
            }
            (FlowRole::Relay, FlowPhase::StagePending, Some(link)) => RelayStageAction::Forward {
                downstream: link,
                activation: self.activation_handle(index, existing.proposal.key)?,
            },
            _ => return Err(Error::State),
        };
        Ok(Some(action))
    }

    fn validate_hop_facts(&self, record: &ActivationRecord, facts: FlowHopFacts) -> Result<()> {
        if facts.local != self.config.local
            || facts.policy_generation != self.config.policy_generation
            || facts.path_frame_mtu < record.proposal.path_frame_mtu
            || record.proposal.requirements.required_feature_bits
                & !u32::from(facts.capability_bits)
                != 0
            || facts.hop_profile != record.proposal.requirements.hop_profile
            || facts.upstream != record.upstream.ok_or(Error::State)?
            || facts.downstream != record.downstream
            || facts.capability_deadline_us != record.local_capability_deadline_us
            || facts.security_owner_instance != record.local_security_owner_instance
            || facts.peer_session_generation != record.local_peer_session_generation
            || facts.origin_key_generation != record.local_origin_key_generation
            || facts.now_us >= record.local_capability_deadline_us
            || facts.now_us >= record.proposal.flow_expires_at_us
        {
            return Err(Error::State);
        }
        Ok(())
    }

    fn publish_activation(
        &mut self,
        activation_index: usize,
        terminal: FlowPhase,
        now_us: u64,
    ) -> Result<FlowHandle> {
        let activation = self.activations[activation_index]
            .value
            .ok_or(Error::NotFound)?;
        let flow_generation_raw = self.next_flow_generation.ok_or(Error::Exhausted)?;
        let flow_generation = FlowGeneration::new(flow_generation_raw)?;
        let next_flow_generation = flow_generation_raw.checked_add(1);
        let flow_slot_generation = self.flows[activation.flow_slot]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let expires_at_us = now_us
            .checked_add(self.config.receipt_lifetime_us)
            .ok_or(Error::Exhausted)?;
        let flow_slot = u16::try_from(activation.flow_slot + 1).map_err(|_| Error::NoSpace)?;
        for slot in &mut self.flows {
            if slot.value.is_some_and(|existing| {
                existing.phase == FlowPhase::Active
                    && existing.proposal.key.route_domain == activation.proposal.key.route_domain
            }) {
                let mut draining = slot.value.ok_or(Error::State)?;
                draining.phase = FlowPhase::Draining;
                slot.value = Some(draining);
            }
        }
        self.flows[activation.flow_slot] = FlowSlot {
            generation: flow_slot_generation,
            value: Some(FlowRecord {
                role: activation.role,
                proposal: activation.proposal,
                phase: terminal,
                flow_generation,
                upstream: activation.upstream,
                downstream: activation.downstream,
                local_capability_deadline_us: activation.local_capability_deadline_us,
                local_security_owner_instance: activation.local_security_owner_instance,
                local_peer_session_generation: activation.local_peer_session_generation,
                local_origin_key_generation: activation.local_origin_key_generation,
                replay_initialized: false,
                replay_highest: 0,
                replay_bitmap: 0,
                replay_reservation_generation: 0,
                replay_reservation: None,
            }),
        };
        self.receipts[activation.receipt_slot].value = Some(FlowReceipt {
            key: activation.proposal.key,
            phase: terminal,
            expires_at_us,
        });
        let mut done = activation;
        done.phase = terminal;
        self.activations[activation_index].value = Some(done);
        self.next_flow_generation = next_flow_generation;
        Ok(FlowHandle {
            owner_instance: self.config.owner_instance,
            slot: flow_slot,
            slot_generation: flow_slot_generation,
            flow_generation,
        })
    }

    fn finish_precommit_abort(&mut self, activation_index: usize, now_us: u64) -> Result<()> {
        let record = self.activations[activation_index]
            .value
            .ok_or(Error::NotFound)?;
        if record.commit_submitted
            || matches!(record.phase, FlowPhase::Active | FlowPhase::InDoubt)
            || self.flows[record.flow_slot].value.is_some()
        {
            return Err(Error::State);
        }
        let expires_at_us = now_us
            .checked_add(self.config.receipt_lifetime_us)
            .ok_or(Error::Exhausted)?;
        self.receipts[record.receipt_slot].value = Some(FlowReceipt {
            key: record.proposal.key,
            phase: FlowPhase::Fenced,
            expires_at_us,
        });
        let mut fenced = record;
        fenced.phase = FlowPhase::Fenced;
        self.activations[activation_index].value = Some(fenced);
        Ok(())
    }

    fn candidate_index(&self, handle: CandidateHandle) -> Result<usize> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let slot = self.candidates.get(index).ok_or(Error::NotFound)?;
        if slot.generation != handle.slot_generation
            || !slot
                .value
                .is_some_and(|record| record.candidate_id == handle.candidate_id)
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn flow_replay_reservation(
        &self,
        handle: FlowReplayHandle,
    ) -> Result<(usize, FlowRecord, FlowReplayReservation)> {
        if handle.owner_instance != self.config.owner_instance || handle.flow_slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.flow_slot - 1);
        let slot = self.flows.get(index).ok_or(Error::NotFound)?;
        let record = slot.value.ok_or(Error::NotFound)?;
        let reservation = record.replay_reservation.ok_or(Error::NotFound)?;
        if slot.generation != handle.flow_slot_generation
            || record.replay_reservation_generation != handle.reservation_generation
            || reservation.sequence != handle.sequence
            || reservation.deadline_us != handle.reservation_deadline_us
            || reservation.aad_digest != handle.aad_digest
            || reservation.payload_digest != handle.payload_digest
        {
            return Err(Error::NotFound);
        }
        Ok((index, record, reservation))
    }

    fn activation_index(&self, handle: ActivationHandle) -> Result<usize> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let slot = self.activations.get(index).ok_or(Error::NotFound)?;
        if slot.generation != handle.slot_generation
            || !slot
                .value
                .is_some_and(|record| record.proposal.key.transaction_id == handle.transaction_id)
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn flow_index(&self, handle: FlowHandle) -> Result<usize> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let slot = self.flows.get(index).ok_or(Error::NotFound)?;
        if slot.generation != handle.slot_generation
            || !slot
                .value
                .is_some_and(|record| record.flow_generation == handle.flow_generation)
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn free_activation_slot(&self) -> Result<usize> {
        self.activations
            .iter()
            .position(|slot| slot.value.is_none())
            .ok_or(Error::NoSpace)
    }

    fn free_flow_slot(&self) -> Result<usize> {
        self.flows
            .iter()
            .enumerate()
            .find(|(index, slot)| {
                slot.value.is_none()
                    && !self
                        .activations
                        .iter()
                        .filter_map(|item| item.value)
                        .any(|activation| activation.flow_slot == *index)
            })
            .map(|(index, _)| index)
            .ok_or(Error::NoSpace)
    }

    fn free_receipt_slot(&self) -> Result<usize> {
        self.receipts
            .iter()
            .enumerate()
            .find(|(index, slot)| {
                slot.value.is_none()
                    && !self
                        .activations
                        .iter()
                        .filter_map(|item| item.value)
                        .any(|activation| activation.receipt_slot == *index)
            })
            .map(|(index, _)| index)
            .ok_or(Error::NoSpace)
    }

    fn find_activation(&self, key: ActivationKey) -> Option<(usize, ActivationRecord)> {
        self.activations
            .iter()
            .enumerate()
            .find_map(|(index, slot)| {
                slot.value
                    .filter(|record| record.proposal.key == key)
                    .map(|record| (index, record))
            })
    }

    fn activation_handle(&self, index: usize, key: ActivationKey) -> Result<ActivationHandle> {
        Ok(ActivationHandle {
            owner_instance: self.config.owner_instance,
            slot: u16::try_from(index + 1).map_err(|_| Error::NoSpace)?,
            slot_generation: self.activations[index].generation,
            transaction_id: key.transaction_id,
        })
    }

    fn active_flow_handle(&self, activation: &ActivationRecord) -> Result<FlowHandle> {
        let slot = self
            .flows
            .get(activation.flow_slot)
            .ok_or(Error::NotFound)?;
        let record = slot.value.ok_or(Error::NotFound)?;
        if record.phase != FlowPhase::Active || record.proposal != activation.proposal {
            return Err(Error::State);
        }
        Ok(FlowHandle {
            owner_instance: self.config.owner_instance,
            slot: u16::try_from(activation.flow_slot + 1).map_err(|_| Error::NoSpace)?,
            slot_generation: slot.generation,
            flow_generation: record.flow_generation,
        })
    }
}

fn flow_parent_is_current(record: &FlowRecord, config: FlowConfig, facts: FlowHopFacts) -> bool {
    facts.local == config.local
        && facts.policy_generation == config.policy_generation
        && facts.path_frame_mtu >= record.proposal.path_frame_mtu
        && record.proposal.requirements.required_feature_bits & !u32::from(facts.capability_bits)
            == 0
        && facts.hop_profile == record.proposal.requirements.hop_profile
        && record.upstream == Some(facts.upstream)
        && facts.downstream == record.downstream
        && facts.capability_deadline_us == record.local_capability_deadline_us
        && facts.security_owner_instance == record.local_security_owner_instance
        && facts.peer_session_generation == record.local_peer_session_generation
        && facts.origin_key_generation == record.local_origin_key_generation
        && facts.now_us < record.local_capability_deadline_us
        && facts.now_us < record.proposal.flow_expires_at_us
}

fn make_flow_security_request(
    record: &FlowRecord,
    session: SessionHandle,
    direction: AccessDirection,
) -> Result<FlowSecurityRequest> {
    if record.phase != FlowPhase::Active {
        return Err(Error::State);
    }
    let requirements = record.proposal.requirements;
    Ok(FlowSecurityRequest {
        session,
        contract: requirements.contract,
        flow_fingerprint: record.proposal.fingerprint()?,
        context_id: record.proposal.context_id,
        label: if requirements.contract == HeaderContract::C2 {
            None
        } else {
            Some(record.proposal.key.forward_label)
        },
        route_generation: record.proposal.key.route_generation,
        service: requirements.service,
        protocol_opcode: requirements.protocol_opcode,
        traffic_ceiling: requirements.traffic_ceiling,
        delivery: requirements.delivery,
        interaction: requirements.interaction,
        payload_kind: requirements.payload_kind,
        origin_security: requirements.origin_security,
        hop_profile: requirements.hop_profile,
        direction,
        policy_generation: requirements.policy_generation,
        expires_at_us: record
            .proposal
            .flow_expires_at_us
            .min(record.local_capability_deadline_us),
    })
}

impl<const CANDIDATES: usize, const ACTIVATIONS: usize, const FLOWS: usize, const RECEIPTS: usize>
    OriginSequenceOwner for FlowOwner<CANDIDATES, ACTIVATIONS, FLOWS, RECEIPTS>
{
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.next_origin_sequence.ok_or(Error::Exhausted)?)
    }

    fn burn(&mut self, expected: OriginSequence) -> Result<()> {
        let current = self.next_origin_sequence.ok_or(Error::Exhausted)?;
        if expected.get() != current {
            return Err(Error::State);
        }
        self.next_origin_sequence = current.checked_add(1);
        Ok(())
    }
}

fn activation_message(opcode: ProtocolOpcode, proposal: FlowProposal) -> ActivationMessage {
    ActivationMessage {
        opcode,
        key: proposal.key,
        setup: proposal.key.label_setup(),
        proposal,
    }
}

fn activation_ack(opcode: ProtocolOpcode, proposal: &FlowProposal) -> ActivationAck {
    ActivationAck {
        opcode,
        key: proposal.key,
    }
}

fn validate_activation_message(
    message: &ActivationMessage,
    expected: ProtocolOpcode,
) -> Result<()> {
    if message.opcode != expected
        || message.key != message.proposal.key
        || message.setup != message.key.label_setup()
    {
        return Err(Error::Security);
    }
    Ok(())
}

fn payload_budget(requirements: FlowRequirements, frame_mtu: u16) -> Result<u16> {
    let prefix = match requirements.contract {
        HeaderContract::C2 => 9_u16,
        HeaderContract::C3 => 7_u16,
        HeaderContract::C4 => 11_u16,
        _ => return Err(Error::Unsupported),
    };
    let origin = if requirements.origin_security == OriginSecurity::O0 {
        0
    } else {
        16
    };
    let hop = if requirements.hop_profile == HopProfile::H1 {
        16
    } else {
        0
    };
    let overhead = prefix
        .checked_add(origin)
        .and_then(|value| value.checked_add(hop))
        .ok_or(Error::Exhausted)?;
    let budget = frame_mtu.checked_sub(overhead).ok_or(Error::NoSpace)?;
    if budget < requirements.minimum_payload_bytes {
        return Err(Error::NoSpace);
    }
    Ok(budget)
}

#[allow(clippy::too_many_lines)]
fn proposal_digest(proposal: &FlowProposal, workspace: &mut CodecWorkspace) -> Result<[u8; 16]> {
    let mut bytes = [0_u8; CANONICAL_PROPOSAL_BYTES];
    let mut cursor = 0;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.key.route_domain.realm.get(),
    )?;
    put_bytes(
        &mut bytes,
        &mut cursor,
        &proposal.key.route_domain.origin.principal,
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.key.route_domain.origin.address.get(),
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.key.route_domain.origin.generation.get(),
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.key.route_domain.origin_session_generation.get(),
    )?;
    put_bytes(
        &mut bytes,
        &mut cursor,
        &proposal.key.route_domain.destination.principal,
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.key.route_domain.destination.address.get(),
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.key.route_domain.destination.generation.get(),
    )?;
    put_u64(&mut bytes, &mut cursor, proposal.key.transaction_id.get())?;
    put_u32(&mut bytes, &mut cursor, proposal.key.candidate_id.get())?;
    put_u32(&mut bytes, &mut cursor, proposal.key.route_generation.get())?;
    put_u16(&mut bytes, &mut cursor, proposal.key.reverse_label.get())?;
    put_u16(&mut bytes, &mut cursor, proposal.key.forward_label.get())?;
    put_u16(&mut bytes, &mut cursor, proposal.key.path_profile_id)?;
    put_u32(&mut bytes, &mut cursor, proposal.route_owner_instance)?;
    put_bytes(&mut bytes, &mut cursor, &proposal.next_hop.peer().principal)?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.next_hop.peer().address.get(),
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.next_hop.peer().generation.get(),
    )?;
    put_u16(&mut bytes, &mut cursor, proposal.next_hop.link_id())?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.next_hop.link_generation().get(),
    )?;
    put_u32(&mut bytes, &mut cursor, proposal.cost)?;
    put_u8(&mut bytes, &mut cursor, proposal.hop_count)?;
    put_u16(&mut bytes, &mut cursor, proposal.path_frame_mtu)?;
    put_u16(&mut bytes, &mut cursor, proposal.payload_budget)?;
    put_u16(&mut bytes, &mut cursor, proposal.capability_bits)?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.capability_runtime_instance,
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.capability_security_owner_instance,
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.capability_session_generation,
    )?;
    put_u32(&mut bytes, &mut cursor, proposal.capability_generation)?;
    put_bytes(&mut bytes, &mut cursor, &proposal.capability_digest)?;
    put_u64(&mut bytes, &mut cursor, proposal.capability_deadline_us)?;
    put_u16(&mut bytes, &mut cursor, proposal.context_id.get())?;
    put_u8(
        &mut bytes,
        &mut cursor,
        u8::from(proposal.requirements.contract),
    )?;
    put_u16(&mut bytes, &mut cursor, proposal.requirements.service.get())?;
    put_u8(
        &mut bytes,
        &mut cursor,
        u8::from(proposal.requirements.traffic_ceiling),
    )?;
    put_u8(
        &mut bytes,
        &mut cursor,
        u8::from(proposal.requirements.delivery),
    )?;
    put_u8(
        &mut bytes,
        &mut cursor,
        u8::from(proposal.requirements.interaction),
    )?;
    put_u8(
        &mut bytes,
        &mut cursor,
        u8::from(proposal.requirements.payload_kind),
    )?;
    put_u16(
        &mut bytes,
        &mut cursor,
        proposal.requirements.protocol_opcode,
    )?;
    put_u8(
        &mut bytes,
        &mut cursor,
        u8::from(proposal.requirements.origin_security),
    )?;
    put_u8(
        &mut bytes,
        &mut cursor,
        u8::from(proposal.requirements.hop_profile),
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.requirements.required_feature_bits,
    )?;
    put_u16(
        &mut bytes,
        &mut cursor,
        proposal.requirements.minimum_payload_bytes,
    )?;
    put_u32(
        &mut bytes,
        &mut cursor,
        proposal.requirements.policy_generation,
    )?;
    put_u64(&mut bytes, &mut cursor, proposal.requirements.expires_at_us)?;
    put_u64(&mut bytes, &mut cursor, proposal.probe_deadline_us)?;
    put_u64(&mut bytes, &mut cursor, proposal.flow_expires_at_us)?;
    Ok(blake2s128(&bytes[..cursor], workspace))
}

fn put_u8(output: &mut [u8], cursor: &mut usize, value: u8) -> Result<()> {
    put_bytes(output, cursor, &[value])
}

fn put_u16(output: &mut [u8], cursor: &mut usize, value: u16) -> Result<()> {
    put_bytes(output, cursor, &value.to_be_bytes())
}

fn put_u32(output: &mut [u8], cursor: &mut usize, value: u32) -> Result<()> {
    put_bytes(output, cursor, &value.to_be_bytes())
}

fn put_u64(output: &mut [u8], cursor: &mut usize, value: u64) -> Result<()> {
    put_bytes(output, cursor, &value.to_be_bytes())
}

fn put_bytes(output: &mut [u8], cursor: &mut usize, value: &[u8]) -> Result<()> {
    let end = cursor.checked_add(value.len()).ok_or(Error::NoSpace)?;
    let target = output.get_mut(*cursor..end).ok_or(Error::NoSpace)?;
    target.copy_from_slice(value);
    *cursor = end;
    Ok(())
}

const fn next_u16_with_limit(current: u16, maximum: u16) -> Option<u16> {
    match current.checked_add(1) {
        Some(next) if next <= maximum => Some(next),
        _ => None,
    }
}

const fn next_u32_with_limit(current: u32, maximum: u32) -> Option<u32> {
    match current.checked_add(1) {
        Some(next) if next <= maximum => Some(next),
        _ => None,
    }
}
