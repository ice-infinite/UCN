use ucn_types::{HeaderContract, HopProfile, OriginSecurity};

use crate::{CapabilityRecord, MessageClass, capability_digest};

/// 用户/产品冻结的最小 Origin 安全等级。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum SecurityFloor {
    /// 无端到端保护要求。
    None = 0,
    /// 必须认证。
    Authenticated = 1,
    /// 必须认证并加密。
    Confidential = 2,
}

/// Capability Profile 的硬需求。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProfileRequirements {
    /// 必需 Feature bits。
    pub required_feature_bits: u32,
    /// 必需 Hop Suite bits。
    pub required_hop_suite_bits: u32,
    /// 必需 E2E Suite bits。
    pub required_e2e_suite_bits: u32,
    /// 最小消息等级。
    pub minimum_message_class: MessageClass,
    /// 最小 RX Window。
    pub minimum_rx_window: u16,
    /// 最小并发 Transfer。
    pub minimum_concurrent_transfers: u16,
    /// 必需 Realtime bits。
    pub required_realtime_mode_bits: u16,
}

/// 双方对 Capability 交集形成的精确选择。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProfileSelect {
    /// 本机 Capability Generation。
    pub local_generation: u32,
    /// 本机 Capability Digest。
    pub local_digest: [u8; 16],
    /// 对端 Capability Generation。
    pub peer_generation: u32,
    /// 对端 Capability Digest。
    pub peer_digest: [u8; 16],
    /// 实际 Feature 交集。
    pub feature_bits: u32,
    /// 实际 Hop Suite 交集。
    pub hop_suite_bits: u32,
    /// 实际 E2E Suite 交集。
    pub e2e_suite_bits: u32,
    /// 双方最大消息等级的较小值。
    pub max_message_class: MessageClass,
    /// 双方 RX Window 较小值。
    pub max_rx_window: u16,
    /// 双方 Transfer 并发较小值。
    pub max_concurrent_transfers: u16,
    /// Realtime mode 交集。
    pub realtime_mode_bits: u16,
}

impl ProfileSelect {
    /// 对两份精确 Capability 做交集；任何 REQUIRED 不满足都拒绝而不降级。
    ///
    /// # Errors
    ///
    /// 输入记录/摘要/需求非法或任一 REQUIRED 不满足时返回错误。
    pub fn build(
        local: CapabilityRecord,
        local_digest: [u8; 16],
        peer: CapabilityRecord,
        peer_digest: [u8; 16],
        requirements: ProfileRequirements,
    ) -> core::result::Result<Self, ucn_types::Error> {
        local.validate()?;
        peer.validate()?;
        if requirements.minimum_rx_window == 0 || requirements.minimum_concurrent_transfers == 0 {
            return Err(ucn_types::Error::Argument);
        }
        if capability_digest(local)? != local_digest || capability_digest(peer)? != peer_digest {
            return Err(ucn_types::Error::Security);
        }
        let feature_bits = local.peer.feature_bits & peer.peer.feature_bits;
        let hop_suite_bits = local.peer.hop_suite_bits & peer.peer.hop_suite_bits;
        let e2e_suite_bits = local.peer.e2e_suite_bits & peer.peer.e2e_suite_bits;
        let realtime_mode_bits = local.peer.realtime_mode_bits & peer.peer.realtime_mode_bits;
        let max_message_class = local
            .peer
            .max_message_class
            .min(peer.peer.max_message_class);
        let max_rx_window = local.peer.max_rx_window.min(peer.peer.max_rx_window);
        let max_concurrent_transfers = local
            .peer
            .max_concurrent_transfers
            .min(peer.peer.max_concurrent_transfers);
        if feature_bits & requirements.required_feature_bits != requirements.required_feature_bits
            || hop_suite_bits & requirements.required_hop_suite_bits
                != requirements.required_hop_suite_bits
            || e2e_suite_bits & requirements.required_e2e_suite_bits
                != requirements.required_e2e_suite_bits
            || max_message_class < requirements.minimum_message_class
            || max_rx_window < requirements.minimum_rx_window
            || max_concurrent_transfers < requirements.minimum_concurrent_transfers
            || realtime_mode_bits & requirements.required_realtime_mode_bits
                != requirements.required_realtime_mode_bits
        {
            return Err(ucn_types::Error::Unsupported);
        }
        Ok(Self {
            local_generation: local.capability_generation,
            local_digest,
            peer_generation: peer.capability_generation,
            peer_digest,
            feature_bits,
            hop_suite_bits,
            e2e_suite_bits,
            max_message_class,
            max_rx_window,
            max_concurrent_transfers,
            realtime_mode_bits,
        })
    }
}

/// 对端对精确 `ProfileSelect` 的确认；任何字段变化都必须重新 Select。
///
/// 该 DTO 本身不证明消息已经通过密码认证。调用方必须先由 Security Owner
/// 验证承载该 DTO 的帧，再把该认证事务对应的预期摘要传给 [`Self::verify_exact`]。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProfileAck {
    /// 被确认的完整选择。
    pub selected: ProfileSelect,
    /// 非零、已认证的选择 transcript 摘要。
    pub transcript_digest: [u8; 16],
}

impl ProfileAck {
    /// 验证已认证 Ack 精确确认本地选择和 transcript。
    ///
    /// # Errors
    ///
    /// 选择任一字段或 transcript 摘要不匹配时返回安全错误。
    /// 本函数只做 typed payload 的精确比较，不替代 Security Owner 的认证。
    pub fn verify_exact(
        self,
        expected: ProfileSelect,
        expected_transcript_digest: [u8; 16],
    ) -> core::result::Result<(), ucn_types::Error> {
        if expected_transcript_digest == [0; 16]
            || self.selected != expected
            || self.transcript_digest != expected_transcript_digest
        {
            return Err(ucn_types::Error::Security);
        }
        Ok(())
    }
}

/// Resolver 输入的已合并硬意图。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EffectiveIntent {
    /// 必需 Feature bits。
    pub required_feature_bits: u32,
    /// 禁止 Feature bits。
    pub forbidden_feature_bits: u32,
    /// 最小 Origin 安全等级。
    pub security_floor: SecurityFloor,
    /// 是否必须可靠交付。
    pub reliable_required: bool,
    /// 是否必须实时语义。
    pub realtime_required: bool,
    /// 是否固定路径。
    pub pinned_path_required: bool,
    /// 实际业务字节数。
    pub payload_bytes: u32,
}

/// 请求时固定资源快照；它不是 reservation。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResourceView {
    /// TX/队列是否有可尝试资源。
    pub tx_available: bool,
    /// Reliable receipt 是否有资源。
    pub reliable_available: bool,
    /// Transfer 是否有资源。
    pub transfer_available: bool,
}

/// 一个完整、不可执行的 Contract 候选。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ContractCandidate {
    /// Core Header Contract。
    pub contract: HeaderContract,
    /// Origin Security。
    pub origin_security: OriginSecurity,
    /// Hop Profile。
    pub hop_profile: HopProfile,
    /// 可用 Feature bits。
    pub feature_bits: u32,
    /// 是否提供可靠交付。
    pub reliable: bool,
    /// 是否提供 Timed 语义。
    pub realtime: bool,
    /// 是否固定 Path/Flow。
    pub pinned_path: bool,
    /// 精确 Payload budget。
    pub payload_budget: u32,
    /// 完整 Frame bytes。
    pub exact_frame_bytes: u32,
    /// Setup 成本。
    pub setup_cost_bytes: u32,
    /// 预计复用次数，必须非零。
    pub expected_reuse_count: u16,
    /// 缺失依赖；None 表示现有依赖已满足。
    pub missing_dependency: Option<DependencyKind>,
    /// Fixture 稳定顺序；不得使用指针地址作为 tie-break。
    pub stable_order: u16,
}

/// Resolver 返回的可尝试计划；仍须 Coordinator 原子预留并复验。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ContractPlan {
    /// 选中的完整候选。
    pub candidate: ContractCandidate,
}

/// Typed dependency 类别。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum DependencyKind {
    /// Identity Binding。
    IdentityBinding = 1,
    /// Security Session。
    SecuritySession = 2,
    /// Capability 刷新。
    CapabilityRefresh = 3,
    /// 基础软路由。
    SoftRoute = 4,
    /// 固定 Flow/Path。
    Flow = 5,
    /// 时间域。
    TimeDomain = 6,
    /// 大消息 Transfer。
    Transfer = 7,
}

/// Resolver 唯一结论。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResolveResult {
    /// 当前依赖可用，可尝试原子预留。
    Ready(ContractPlan),
    /// 首个固定优先级依赖尚未满足。
    NeedDependency(DependencyKind),
    /// 编译/对端能力不支持。
    RejectUnsupported,
    /// 硬策略冲突。
    RejectPolicy,
    /// 固定资源当前不足。
    RejectResource,
}

/// 纯函数、零 Provider/Driver、零写 Owner 的 Resolver。
pub struct ContractResolver;

impl ContractResolver {
    /// 有界扫描候选并执行“先硬准入、后确定性评分”。
    #[must_use]
    pub fn resolve(
        intent: EffectiveIntent,
        resources: ResourceView,
        candidates: &[ContractCandidate],
    ) -> ResolveResult {
        if intent.required_feature_bits & intent.forbidden_feature_bits != 0 {
            return ResolveResult::RejectPolicy;
        }
        let mut best: Option<ContractCandidate> = None;
        let mut first_dependency: Option<DependencyKind> = None;
        let mut resource_blocked = false;
        for candidate in candidates {
            if candidate.expected_reuse_count == 0
                || candidate.feature_bits & intent.required_feature_bits
                    != intent.required_feature_bits
                || candidate.feature_bits & intent.forbidden_feature_bits != 0
                || candidate.payload_budget < intent.payload_bytes
                || (intent.reliable_required && !candidate.reliable)
                || (intent.realtime_required && !candidate.realtime)
                || (intent.pinned_path_required && !candidate.pinned_path)
                || security_level(candidate.origin_security) < intent.security_floor
            {
                continue;
            }
            if let Some(dependency) = candidate.missing_dependency {
                if dependency == DependencyKind::Transfer && !resources.transfer_available {
                    resource_blocked = true;
                    continue;
                }
                first_dependency = Some(match first_dependency {
                    Some(existing) => existing.min(dependency),
                    None => dependency,
                });
                continue;
            }
            if !resources.tx_available
                || (intent.reliable_required && !resources.reliable_available)
            {
                resource_blocked = true;
                continue;
            }
            if best.is_none_or(|current| candidate_key(*candidate) < candidate_key(current)) {
                best = Some(*candidate);
            }
        }
        if let Some(candidate) = best {
            ResolveResult::Ready(ContractPlan { candidate })
        } else if let Some(dependency) = first_dependency {
            ResolveResult::NeedDependency(dependency)
        } else if resource_blocked {
            ResolveResult::RejectResource
        } else {
            ResolveResult::RejectUnsupported
        }
    }
}

fn security_level(value: OriginSecurity) -> SecurityFloor {
    match value {
        OriginSecurity::O0 => SecurityFloor::None,
        OriginSecurity::O1 => SecurityFloor::Authenticated,
        OriginSecurity::O2 => SecurityFloor::Confidential,
    }
}

fn candidate_key(candidate: ContractCandidate) -> (bool, u32, u32, u8, u16) {
    (
        candidate.missing_dependency.is_some(),
        candidate.exact_frame_bytes,
        candidate.setup_cost_bytes / u32::from(candidate.expected_reuse_count),
        candidate.contract as u8,
        candidate.stable_order,
    )
}
