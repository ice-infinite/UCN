use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_persistence::{
    CodecWorkspace, DIGEST_BYTES, DomainKey, DomainKind, DomainState, DomainView,
    PersistenceHandle, PersistenceProof, PersistenceRequest, RecordMeta, body_digest,
};
use ucn_types::{
    AddressWidth, BindingGeneration, C0TransactionId, Error, HopProfile, HopSequence, KeyId,
    LinkInstanceGeneration, NodeAddress, OriginSecurity, OriginSequence, PeerSessionGeneration,
    ProtocolOpcode, RealmId, Result, ServiceId, SuiteId,
};

use crate::crypto::{CryptoProvider, KeySelector};
use crate::packet::Fingerprint;
use crate::replay::{ReplayEvidence, ReplayMutationHandle, ReplayWindow};

/// Security Session 持久化正文长度。
pub const SESSION_RECORD_BYTES: usize = 192;
const SESSION_RECORD_BYTES_U32: u32 = 192;
/// Security Session Record Schema。
pub const SESSION_SCHEMA_ID: u16 = 0x5301;
/// Security Session Record Version。
pub const SESSION_SCHEMA_VERSION: u16 = 1;
/// Session publish 对应的 Persistence operation kind。
pub const SESSION_OPERATION_KIND: u16 = 0x0501;

const SESSION_MAGIC: &[u8; 4] = b"UC6S";
const CALLBACK_VERIFY_PROOF: u16 = 0x0501;
pub(crate) const CALLBACK_CRYPTO: u16 = 0x0502;

/// 一个地址与不可复用 Binding Generation 的完整绑定。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Binding {
    /// Realm 内节点地址。
    pub address: NodeAddress,
    /// 地址绑定代际。
    pub generation: BindingGeneration,
    /// 认证设备 Principal 摘要。
    pub principal: [u8; 16],
}

impl Binding {
    fn validate(self) -> Result<()> {
        if self.generation.is_unbound() || self.principal == [0; 16] {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// Session 提供的 Origin 保护等级。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SecurityLevel {
    /// O1 认证，不加密。
    Authenticated,
    /// O2 认证并加密。
    Confidential,
}

impl SecurityLevel {
    pub(crate) const fn wire(self) -> OriginSecurity {
        match self {
            Self::Authenticated => OriginSecurity::O1,
            Self::Confidential => OriginSecurity::O2,
        }
    }
}

/// 已经由身份层和产品 Policy 形成的静态握手候选。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HandshakeCandidate {
    /// 本地 Binding。
    pub local: Binding,
    /// 对端 Binding。
    pub peer: Binding,
    /// 当前 Link Instance Generation。
    pub link_generation: LinkInstanceGeneration,
    /// 新 Peer Session Generation。
    pub session_generation: PeerSessionGeneration,
    /// Endpoint/安全 Policy Generation。
    pub policy_generation: u32,
    /// 半开 Session 绝对到期时间。
    pub expires_at_us: u64,
    /// Origin 保护等级。
    pub origin_level: SecurityLevel,
    /// Hop 保护档。
    pub hop_profile: HopProfile,
    /// 本地方向 Origin Key。
    pub origin_tx: KeySelector,
    /// 对端方向 Origin Key。
    pub origin_rx: KeySelector,
    /// 本地方向 Hop Key；H0/H2 时必须为空。
    pub hop_tx: Option<KeySelector>,
    /// 对端方向 Hop Key；H0/H2 时必须为空。
    pub hop_rx: Option<KeySelector>,
    /// canonical Origin Context 摘要。
    pub origin_fingerprint: Fingerprint,
    /// canonical Hop/Direct Context 摘要。
    pub hop_fingerprint: Fingerprint,
    /// 双向握手 transcript 摘要。
    pub transcript_digest: [u8; 32],
}

impl HandshakeCandidate {
    fn validate(self, width: AddressWidth, now_us: u64) -> Result<()> {
        self.local.validate()?;
        self.peer.validate()?;
        NodeAddress::new(self.local.address.get(), width)?;
        NodeAddress::new(self.peer.address.get(), width)?;
        if self.local.address == self.peer.address
            || self.local.principal == self.peer.principal
            || self.policy_generation == 0
            || self.expires_at_us == 0
            || now_us >= self.expires_at_us
            || self.transcript_digest == [0; 32]
            || self.origin_tx == self.origin_rx
        {
            return Err(Error::Argument);
        }
        let confidential = self.origin_level == SecurityLevel::Confidential;
        self.origin_tx.validate_origin(confidential)?;
        self.origin_rx.validate_origin(confidential)?;
        match self.hop_profile {
            HopProfile::H0 | HopProfile::H2 => {
                if self.hop_tx.is_some() || self.hop_rx.is_some() {
                    return Err(Error::Argument);
                }
            }
            HopProfile::H1 => {
                let tx = self.hop_tx.ok_or(Error::Argument)?;
                let rx = self.hop_rx.ok_or(Error::Argument)?;
                tx.validate_hop()?;
                rx.validate_hop()?;
                if tx == rx {
                    return Err(Error::Argument);
                }
            }
            HopProfile::H3 => return Err(Error::Unsupported),
        }
        Ok(())
    }
}

/// Provider 验证的握手证明字节。
#[derive(Clone, Copy)]
pub struct HandshakeProof<'a> {
    /// 认证握手的外部证明；格式由产品 Provider 唯一解释。
    pub bytes: &'a [u8],
}

/// 从 Persistence Domain 当前视图派生的提交基线。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DurabilityBase {
    /// Persistence Runtime。
    pub runtime_instance: u32,
    /// Security Owner instance，必须匹配 Domain Binding。
    pub caller_owner_instance: u16,
    /// 本启动 Domain Generation。
    pub domain_generation: u16,
    /// Security high-water Domain。
    pub domain: DomainKey,
    /// 下一 Foundation Transaction ID。
    pub next_transaction_id: u64,
    /// 当前 Record Generation。
    pub expected_record_generation: u64,
    /// 当前 Body Digest。
    pub expected_body_digest: [u8; DIGEST_BYTES],
    /// 半开绝对提交 Deadline。
    pub absolute_deadline_us: u64,
    /// Coordinator 易失 continuation。
    pub volatile_continuation: u32,
    prior_realm: Option<RealmId>,
    prior_width: Option<AddressWidth>,
    prior_session: Option<HandshakeCandidate>,
    prior_outbound_hop_key_generation: u32,
    prior_inbound_hop_key_generation: u32,
}

impl DurabilityBase {
    /// 从 Ready Domain view 构造下一提交基线。
    ///
    /// # Errors
    ///
    /// Domain 未 Ready、当前正文无法验证、代际耗尽或调用方绑定不完整时返回错误。
    pub fn from_view(
        runtime_instance: u32,
        caller_owner_instance: u16,
        view: DomainView,
        current_body: &[u8],
        absolute_deadline_us: u64,
        volatile_continuation: u32,
        workspace: &mut CodecWorkspace,
    ) -> Result<Self> {
        if runtime_instance == 0
            || caller_owner_instance == 0
            || view.domain.kind != DomainKind::SecurityHighWater
            || view.state != DomainState::Ready
            || view.pending
            || view.domain_generation == 0
            || absolute_deadline_us == 0
            || volatile_continuation == 0
        {
            return Err(Error::State);
        }
        let next_transaction_id = view
            .foundation_transaction_id
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        if next_transaction_id == 0 || next_transaction_id == u64::MAX {
            return Err(Error::Exhausted);
        }
        let (
            prior_realm,
            prior_width,
            prior_session,
            prior_outbound_hop_key_generation,
            prior_inbound_hop_key_generation,
        ) = if view.record_generation == 0 {
            if view.foundation_transaction_id != 0
                || view.body_bytes != 0
                || !current_body.is_empty()
                || view.body_digest != [0; DIGEST_BYTES]
                || view.current_operation_kind != 0
            {
                return Err(Error::State);
            }
            (None, None, None, 0, 0)
        } else {
            if current_body.len() != view.body_bytes as usize
                || view.schema_id != SESSION_SCHEMA_ID
                || view.schema_version != SESSION_SCHEMA_VERSION
                || view.current_operation_kind != SESSION_OPERATION_KIND
            {
                return Err(Error::State);
            }
            let meta = RecordMeta {
                domain: view.domain,
                record_generation: view.record_generation,
                transaction_id: view.foundation_transaction_id,
                body_bytes: view.body_bytes,
                schema_id: view.schema_id,
                schema_version: view.schema_version,
                operation_kind: view.current_operation_kind,
                body_digest: [0; DIGEST_BYTES],
            };
            if body_digest(&meta, current_body, workspace)? != view.body_digest {
                return Err(Error::Security);
            }
            let decoded = decode_session_record_full(current_body)?;
            (
                Some(decoded.realm),
                Some(decoded.width),
                Some(decoded.candidate),
                decoded.outbound_hop_key_generation,
                decoded.inbound_hop_key_generation,
            )
        };
        Ok(Self {
            runtime_instance,
            caller_owner_instance,
            domain_generation: view.domain_generation,
            domain: view.domain,
            next_transaction_id,
            expected_record_generation: view.record_generation,
            expected_body_digest: view.body_digest,
            absolute_deadline_us,
            volatile_continuation,
            prior_realm,
            prior_width,
            prior_session,
            prior_outbound_hop_key_generation,
            prior_inbound_hop_key_generation,
        })
    }
}

/// Security Owner 交给 Coordinator 的完整持久化 Requirement。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SessionDurabilityRequirement {
    base: DurabilityBase,
    body: [u8; SESSION_RECORD_BYTES],
    expected_published_digest: [u8; DIGEST_BYTES],
    business_transition_digest: u64,
}

impl SessionDurabilityRequirement {
    /// 转换成 Persistence Owner 的只读请求。
    #[must_use]
    pub fn persistence_request(&self) -> PersistenceRequest<'_> {
        PersistenceRequest {
            runtime_instance: self.base.runtime_instance,
            caller_owner_instance: self.base.caller_owner_instance,
            domain_generation: self.base.domain_generation,
            domain: self.base.domain,
            foundation_transaction_id: self.base.next_transaction_id,
            expected_record_generation: self.base.expected_record_generation,
            absolute_deadline_us: self.base.absolute_deadline_us,
            business_transition_digest: self.business_transition_digest,
            canonical_body: &self.body,
            schema_id: SESSION_SCHEMA_ID,
            schema_version: SESSION_SCHEMA_VERSION,
            operation_kind: SESSION_OPERATION_KIND,
            expected_body_digest: self.base.expected_body_digest,
            volatile_continuation: self.base.volatile_continuation,
        }
    }
}

/// Session 生命周期。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SessionPhase {
    /// 空槽。
    Empty,
    /// 握手已验证，等待 durable proof。
    AwaitingDurability,
    /// 可用于收发。
    Active,
    /// 不可逆易失 Fence。
    Fenced,
}

/// Session Owner 签发的精确 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SessionHandle {
    owner_instance: u32,
    slot: u16,
    slot_generation: u32,
    session_generation: PeerSessionGeneration,
}

/// 受保护 Packet 使用的精确 Security Context Handle。
pub type SecurityContextHandle = SessionHandle;

/// Session 的只读视图。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SessionView {
    /// 生命周期。
    pub phase: SessionPhase,
    /// 对端 Binding。
    pub peer: Binding,
    /// Session Generation。
    pub session_generation: PeerSessionGeneration,
    /// 到期时间。
    pub expires_at_us: u64,
    /// Origin 保护档。
    pub origin_security: OriginSecurity,
    /// Hop 保护档。
    pub hop_profile: HopProfile,
}

/// 一次有界 Replay reservation 维护的结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReplayMaintenance {
    /// 本轮实际检查的固定槽数。
    pub inspected: usize,
    /// 本轮释放的到期 reservation 数。
    pub expired: usize,
}

/// 每次使用 Session 时重新核对的当前事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CurrentFacts {
    /// 当前本地 Binding。
    pub local: Binding,
    /// 当前对端 Binding；地址、Generation 或 Principal 变化都会使旧 Session 失效。
    pub peer: Binding,
    /// 当前 Link Generation。
    pub link_generation: LinkInstanceGeneration,
    /// 当前安全 Policy Generation。
    pub policy_generation: u32,
    /// 可信单调时间。
    pub now_us: u64,
}

/// ACL 方向。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AccessDirection {
    /// 入站业务副作用。
    Inbound = 1,
    /// 出站发送。
    Outbound = 2,
}

/// Manifest 固定的一条精确 ACL。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AclRule {
    /// 对端 Principal。
    pub peer_principal: [u8; 16],
    /// 对端 Binding Generation。
    pub peer_binding_generation: BindingGeneration,
    /// 此 ACL 精确授权的 canonical Security Context fingerprint。
    pub context_fingerprint: Fingerprint,
    /// Service ID。
    pub service: ServiceId,
    /// Data 为 0；Control 为精确 Protocol Opcode。
    pub protocol_opcode: u16,
    /// 方向。
    pub direction: AccessDirection,
}

/// 一次 ACL 查询的完整 canonical key。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AccessRequest {
    /// Session Handle。
    pub session: SessionHandle,
    /// 由 Context Owner 冻结并与 Packet 计划精确匹配的 fingerprint。
    pub context_fingerprint: Fingerprint,
    /// Service ID。
    pub service: ServiceId,
    /// Data 为 0；Control 为精确 Protocol Opcode。
    pub protocol_opcode: u16,
    /// 方向。
    pub direction: AccessDirection,
}

/// 每个 Origin Sequence 发送域必须实现的不可回绕所有权接口。
pub trait OriginSequenceOwner {
    /// 只读查看下一值，不消耗。
    ///
    /// # Errors
    ///
    /// 域未就绪或计数器已耗尽时返回错误。
    fn preview(&self) -> Result<OriginSequence>;
    /// 在密码 Provider 首次观察 Key/Nonce 前不可逆消耗精确值。
    ///
    /// # Errors
    ///
    /// `expected` 不是当前下一值或计数器已耗尽时返回错误。
    fn burn(&mut self, expected: OriginSequence) -> Result<()>;
}

/// C0 Transaction ID 的不可回绕所有权接口。
pub trait C0TransactionOwner {
    /// 只读查看下一值，不消耗。
    ///
    /// # Errors
    ///
    /// 域未就绪或计数器已耗尽时返回错误。
    fn preview(&self) -> Result<C0TransactionId>;
    /// 在密码 Provider 首次观察 Key/Nonce 前不可逆消耗精确值。
    ///
    /// # Errors
    ///
    /// `expected` 不是当前下一值或计数器已耗尽时返回错误。
    fn burn(&mut self, expected: C0TransactionId) -> Result<()>;
}

/// Hop Sequence 的不可回绕所有权接口。
pub trait HopSequenceOwner {
    /// 只读查看下一值，不消耗。
    ///
    /// # Errors
    ///
    /// 域未就绪或计数器已耗尽时返回错误。
    fn preview(&self) -> Result<HopSequence>;
    /// 在密码 Provider 首次观察 Hop Key/Sequence 前不可逆消耗精确值。
    ///
    /// # Errors
    ///
    /// `expected` 不是当前下一值或计数器已耗尽时返回错误。
    fn burn(&mut self, expected: HopSequence) -> Result<()>;
}

/// 不同 Header Contract 对应的唯一 Origin 计数器 Owner。
pub enum OriginCounterOwner<'a> {
    /// O0 且无 Origin Replay 域。
    None,
    /// C0 Transaction ID Owner。
    Transaction(&'a mut dyn C0TransactionOwner),
    /// C1/C2/C4/C5 Origin Sequence Owner。
    Sequence(&'a mut dyn OriginSequenceOwner),
}

/// Manifest 中一个 Security high-water Domain 与唯一 Peer Principal 的静态绑定。
///
/// 该绑定阻止首次会话把合法握手写入另一个 Peer 的空持久化 Domain；同一 Principal 与同一
/// Domain 都只能出现一次。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SessionDomainRule {
    /// Security high-water Domain。
    pub domain: DomainKey,
    /// 此 Domain 唯一允许的对端 Principal。
    pub peer_principal: [u8; 16],
}

/// Security Owner 静态配置。
pub struct SecurityConfig<'a> {
    /// Runtime instance。
    pub runtime_instance: u32,
    /// Security Owner instance。
    pub owner_instance: u32,
    /// Persistence Domain Binding 使用的 16-bit Owner ID。
    pub persistence_business_owner_instance: u16,
    /// 产生 durable proof 的 Persistence Owner instance。
    pub persistence_owner_instance: u16,
    /// Realm。
    pub realm: RealmId,
    /// 固定地址宽度。
    pub address_width: AddressWidth,
    /// Provider 共享回调门。
    pub provider_gate: &'a CallbackGate,
    /// Manifest 固定的 Session Domain/Peer 映射。
    pub session_domains: &'a [SessionDomainRule],
    /// Replay mutation reservation 的非零最大生命周期。
    pub replay_reservation_lifetime_us: u64,
    /// Manifest 固定 ACL。
    pub acl: &'a [AclRule],
}

struct SessionSlot<const REPLAY_SLOTS: usize> {
    occupied: bool,
    slot_generation: u32,
    phase: SessionPhase,
    candidate: Option<HandshakeCandidate>,
    requirement: Option<SessionDurabilityRequirement>,
    persistence_handle: Option<PersistenceHandle>,
    origin_replay: ReplayWindow<REPLAY_SLOTS>,
    hop_replay: ReplayWindow<REPLAY_SLOTS>,
}

impl<const REPLAY_SLOTS: usize> SessionSlot<REPLAY_SLOTS> {
    const EMPTY: Self = Self {
        occupied: false,
        slot_generation: 0,
        phase: SessionPhase::Empty,
        candidate: None,
        requirement: None,
        persistence_handle: None,
        origin_replay: ReplayWindow::new(),
        hop_replay: ReplayWindow::new(),
    };
}

/// 固定容量 Security Session Owner。
pub struct SecurityOwner<'a, const SESSIONS: usize, const REPLAY_SLOTS: usize> {
    config: SecurityConfig<'a>,
    next_operation_id: u32,
    replay_maintenance_cursor: usize,
    codec: CodecWorkspace,
    sessions: [SessionSlot<REPLAY_SLOTS>; SESSIONS],
}

/// Nano Profile：2 个 Peer Session，每 Session 2 个并发 Replay reservation。
pub type NanoSecurityOwner<'a> = SecurityOwner<'a, 2, 2>;
/// Lite Profile：8 个 Peer Session，每 Session 4 个并发 Replay reservation。
pub type LiteSecurityOwner<'a> = SecurityOwner<'a, 8, 4>;
/// Full Profile：16 个 Peer Session，每 Session 8 个并发 Replay reservation。
pub type FullSecurityOwner<'a> = SecurityOwner<'a, 16, 8>;

impl<'a, const SESSIONS: usize, const REPLAY_SLOTS: usize>
    SecurityOwner<'a, SESSIONS, REPLAY_SLOTS>
{
    /// 建立空 Security Owner。
    ///
    /// # Errors
    ///
    /// Instance、容量、Domain 映射或 ACL 不是完整 canonical 配置时返回配置错误。
    pub fn new(config: SecurityConfig<'a>) -> Result<Self> {
        if config.runtime_instance == 0
            || config.owner_instance == 0
            || config.persistence_business_owner_instance == 0
            || config.persistence_owner_instance == 0
            || SESSIONS == 0
            || SESSIONS > u16::MAX as usize
            || REPLAY_SLOTS == 0
            || REPLAY_SLOTS > u16::MAX as usize
            || SESSIONS
                .checked_mul(REPLAY_SLOTS)
                .and_then(|value| value.checked_mul(2))
                .is_none()
            || config.replay_reservation_lifetime_us == 0
            || config.session_domains.is_empty()
            || config.session_domains.len() >= SESSIONS
            || config.session_domains.iter().any(|rule| {
                rule.domain.kind != DomainKind::SecurityHighWater || rule.peer_principal == [0; 16]
            })
            || config
                .session_domains
                .iter()
                .enumerate()
                .any(|(index, left)| {
                    config.session_domains[index + 1..].iter().any(|right| {
                        left.domain == right.domain || left.peer_principal == right.peer_principal
                    })
                })
            || config.acl.iter().any(|rule| {
                rule.peer_principal == [0; 16]
                    || rule.peer_binding_generation.is_unbound()
                    || (rule.protocol_opcode != 0
                        && ProtocolOpcode::try_from(rule.protocol_opcode).is_err())
            })
        {
            return Err(Error::Config);
        }
        Ok(Self {
            config,
            next_operation_id: 1,
            replay_maintenance_cursor: 0,
            codec: CodecWorkspace::new(),
            sessions: core::array::from_fn(|_| SessionSlot::EMPTY),
        })
    }

    /// 验证静态握手并产生由 Coordinator 路由的 Persistence requirement。
    ///
    /// # Errors
    ///
    /// 候选、Domain、历史高水位、证明、当前事实、容量或计数器不满足合同时返回错误；失败
    /// 不占用 Session 槽。
    #[allow(clippy::too_many_lines)]
    pub fn prepare_static_session<P: CryptoProvider>(
        &mut self,
        provider: &mut P,
        candidate: HandshakeCandidate,
        proof: HandshakeProof<'_>,
        base: DurabilityBase,
        facts: CurrentFacts,
    ) -> Result<(SessionHandle, SessionDurabilityRequirement)> {
        candidate.validate(self.config.address_width, facts.now_us)?;
        let domain_rule = self
            .config
            .session_domains
            .iter()
            .find(|rule| rule.domain == base.domain)
            .ok_or(Error::Access)?;
        if domain_rule.peer_principal != candidate.peer.principal {
            return Err(Error::Access);
        }
        if proof.bytes.is_empty()
            || proof.bytes.len() > 256
            || base.runtime_instance != self.config.runtime_instance
            || base.caller_owner_instance != self.config.persistence_business_owner_instance
            || facts.local != candidate.local
            || facts.peer != candidate.peer
            || facts.link_generation != candidate.link_generation
            || facts.policy_generation != candidate.policy_generation
            || facts.now_us >= base.absolute_deadline_us
        {
            return Err(Error::State);
        }
        match base.prior_session {
            None => {
                if candidate.session_generation.get() != 1 {
                    return Err(Error::Replay);
                }
            }
            Some(prior) => {
                let expected = prior.session_generation.checked_next()?;
                if base.prior_realm != Some(self.config.realm)
                    || base.prior_width != Some(self.config.address_width)
                    || prior.local.principal != candidate.local.principal
                    || prior.peer.principal != candidate.peer.principal
                    || candidate.session_generation != expected
                    || candidate.origin_tx.key_generation <= prior.origin_tx.key_generation
                    || candidate.origin_rx.key_generation <= prior.origin_rx.key_generation
                    || candidate.transcript_digest == prior.transcript_digest
                {
                    return Err(Error::Replay);
                }
            }
        }
        if self.sessions.iter().any(|slot| {
            slot.occupied
                && slot.candidate.is_some_and(|existing| {
                    existing.peer.principal == candidate.peer.principal
                        && existing.session_generation == candidate.session_generation
                })
        }) {
            return Err(Error::State);
        }
        let slot_index = self
            .sessions
            .iter()
            .position(|slot| !slot.occupied)
            .ok_or(Error::NoSpace)?;

        let mut body = [0_u8; SESSION_RECORD_BYTES];
        let outbound_hop_high_water =
            next_hop_key_high_water(base.prior_outbound_hop_key_generation, candidate.hop_tx)?;
        let inbound_hop_high_water =
            next_hop_key_high_water(base.prior_inbound_hop_key_generation, candidate.hop_rx)?;
        encode_session_record(
            self.config.realm,
            self.config.address_width,
            candidate,
            outbound_hop_high_water,
            inbound_hop_high_water,
            &mut body,
        )?;
        let next_record_generation = base
            .expected_record_generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        if next_record_generation == u64::MAX {
            return Err(Error::Exhausted);
        }
        let meta = RecordMeta {
            domain: base.domain,
            record_generation: next_record_generation,
            transaction_id: base.next_transaction_id,
            body_bytes: SESSION_RECORD_BYTES_U32,
            schema_id: SESSION_SCHEMA_ID,
            schema_version: SESSION_SCHEMA_VERSION,
            operation_kind: SESSION_OPERATION_KIND,
            body_digest: [0; DIGEST_BYTES],
        };
        let expected_published_digest = body_digest(&meta, &body, &mut self.codec)?;
        let business_transition_digest = u64::from_be_bytes(
            expected_published_digest[..8]
                .try_into()
                .map_err(|_| Error::State)?,
        );
        if business_transition_digest == 0 {
            return Err(Error::State);
        }
        let requirement = SessionDurabilityRequirement {
            base,
            body,
            expected_published_digest,
            business_transition_digest,
        };
        let next_slot_generation = self.sessions[slot_index]
            .slot_generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.call_verify_session(provider, candidate, proof.bytes)?;
        self.sessions[slot_index] = SessionSlot::EMPTY;
        let slot = &mut self.sessions[slot_index];
        slot.occupied = true;
        slot.slot_generation = next_slot_generation;
        slot.phase = SessionPhase::AwaitingDurability;
        slot.candidate = Some(candidate);
        slot.requirement = Some(requirement);
        let handle = SessionHandle {
            owner_instance: self.config.owner_instance,
            slot: u16::try_from(slot_index + 1).map_err(|_| Error::NoSpace)?,
            slot_generation: next_slot_generation,
            session_generation: candidate.session_generation,
        };
        Ok((handle, requirement))
    }

    /// 绑定 Coordinator 实际提交得到的精确 Persistence Handle。
    ///
    /// # Errors
    ///
    /// Handle、阶段或重复绑定不匹配时返回错误且不改写槽。
    pub fn bind_persistence(
        &mut self,
        session: SessionHandle,
        persistence: PersistenceHandle,
    ) -> Result<()> {
        let slot = self.slot_mut(session)?;
        if slot.phase != SessionPhase::AwaitingDurability || slot.persistence_handle.is_some() {
            return Err(Error::State);
        }
        slot.persistence_handle = Some(persistence);
        Ok(())
    }

    /// 使用 reload proof 原子发布 Session。
    ///
    /// # Errors
    ///
    /// Proof、Persistence Handle、当前事实、Deadline 或代际不精确匹配时返回错误且不发布。
    pub fn activate(
        &mut self,
        session: SessionHandle,
        persistence: PersistenceHandle,
        proof: PersistenceProof,
        facts: CurrentFacts,
    ) -> Result<()> {
        let runtime = self.config.runtime_instance;
        let persistence_owner = self.config.persistence_business_owner_instance;
        let proof_owner = self.config.persistence_owner_instance;
        let target_index = self.slot_index(session)?;
        let target = self.slot(session)?;
        let candidate = target.candidate.ok_or(Error::State)?;
        let requirement = target.requirement.ok_or(Error::State)?;
        if target.phase != SessionPhase::AwaitingDurability
            || target.persistence_handle != Some(persistence)
            || facts.local != candidate.local
            || facts.peer != candidate.peer
            || facts.link_generation != candidate.link_generation
            || facts.policy_generation != candidate.policy_generation
            || facts.now_us >= candidate.expires_at_us
            || facts.now_us >= requirement.base.absolute_deadline_us
            || proof.runtime_instance != runtime
            || proof.persistence_owner_instance != proof_owner
            || proof.caller_owner_instance != persistence_owner
            || proof.domain_generation != requirement.base.domain_generation
            || proof.operation_kind != SESSION_OPERATION_KIND
            || proof.domain != requirement.base.domain
            || proof.record_generation
                != requirement
                    .base
                    .expected_record_generation
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?
            || proof.foundation_transaction_id != requirement.base.next_transaction_id
            || proof.witness_generation != proof.record_generation
            || proof.body_bytes != SESSION_RECORD_BYTES_U32
            || proof.body_digest != requirement.expected_published_digest
            || proof.volatile_continuation != requirement.base.volatile_continuation
        {
            return Err(Error::State);
        }
        for (index, existing) in self.sessions.iter().enumerate() {
            if index != target_index
                && existing.occupied
                && existing.phase == SessionPhase::Active
                && existing
                    .candidate
                    .is_some_and(|value| value.peer.principal == candidate.peer.principal)
            {
                let existing_generation = existing
                    .candidate
                    .ok_or(Error::State)?
                    .session_generation
                    .get();
                if existing_generation >= candidate.session_generation.get() {
                    return Err(Error::Replay);
                }
            }
        }
        for (index, existing) in self.sessions.iter_mut().enumerate() {
            if index != target_index
                && existing.occupied
                && existing.phase == SessionPhase::Active
                && existing
                    .candidate
                    .is_some_and(|value| value.peer.principal == candidate.peer.principal)
            {
                existing.phase = SessionPhase::Fenced;
                existing.origin_replay.clear();
                existing.hop_replay.clear();
            }
        }
        let slot = &mut self.sessions[target_index];
        slot.requirement = None;
        slot.persistence_handle = None;
        slot.phase = SessionPhase::Active;
        Ok(())
    }

    /// 当前事实失配、到期或显式撤销时立即 Fence Session 并清理易失 Replay。
    ///
    /// # Errors
    ///
    /// Handle 无效或指向空槽时返回错误。
    pub fn fence(&mut self, session: SessionHandle) -> Result<()> {
        let slot = self.slot_mut(session)?;
        if slot.phase == SessionPhase::Fenced {
            return Ok(());
        }
        if slot.phase == SessionPhase::Empty {
            return Err(Error::State);
        }
        slot.phase = SessionPhase::Fenced;
        slot.requirement = None;
        slot.persistence_handle = None;
        slot.origin_replay.clear();
        slot.hop_replay.clear();
        Ok(())
    }

    /// 在 Fence 后释放固定槽；slot generation 高水位不会回退。
    ///
    /// # Errors
    ///
    /// Handle 无效或 Session 尚未 Fence 时返回错误。
    pub fn retire_fenced(&mut self, session: SessionHandle) -> Result<()> {
        let slot = self.slot_mut(session)?;
        if slot.phase != SessionPhase::Fenced {
            return Err(Error::State);
        }
        slot.occupied = false;
        slot.phase = SessionPhase::Empty;
        slot.candidate = None;
        Ok(())
    }

    /// 按可信时间有界扫描并 Fence 到期 Session。
    pub fn expire(&mut self, now_us: u64) -> usize {
        let mut count = 0;
        for slot in &mut self.sessions {
            if slot.occupied
                && slot.phase != SessionPhase::Fenced
                && slot
                    .candidate
                    .is_some_and(|candidate| now_us >= candidate.expires_at_us)
            {
                slot.phase = SessionPhase::Fenced;
                slot.requirement = None;
                slot.persistence_handle = None;
                slot.origin_replay.clear();
                slot.hop_replay.clear();
                count += 1;
            }
        }
        count
    }

    /// 按持久游标和调用方预算清理到期 Replay reservation。
    ///
    /// 每个 work unit 只检查一个固定槽；错误输入路径不会隐式驱逐其他 reservation。
    #[must_use]
    pub fn maintain_replay_reservations(
        &mut self,
        now_us: u64,
        inspection_budget: usize,
    ) -> ReplayMaintenance {
        let total = SESSIONS * REPLAY_SLOTS * 2;
        let inspected = inspection_budget.min(total);
        let mut expired = 0;
        for _ in 0..inspected {
            let flat = self.replay_maintenance_cursor;
            let per_session = REPLAY_SLOTS * 2;
            let session_index = flat / per_session;
            let within_session = flat % per_session;
            let replay_index = within_session % REPLAY_SLOTS;
            let slot = &mut self.sessions[session_index];
            expired += usize::from(if within_session < REPLAY_SLOTS {
                slot.origin_replay
                    .expire_reservation_at(replay_index, now_us)
            } else {
                slot.hop_replay.expire_reservation_at(replay_index, now_us)
            });
            self.replay_maintenance_cursor = (flat + 1) % total;
        }
        ReplayMaintenance { inspected, expired }
    }

    /// 读取 Session 快照；每次都复核当前事实与半开 Deadline。
    ///
    /// # Errors
    ///
    /// Handle、阶段、当前事实或 Deadline 不匹配时返回错误。
    pub fn session_get(&self, session: SessionHandle, facts: CurrentFacts) -> Result<SessionView> {
        let slot = self.slot(session)?;
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        Ok(SessionView {
            phase: slot.phase,
            peer: candidate.peer,
            session_generation: candidate.session_generation,
            expires_at_us: candidate.expires_at_us,
            origin_security: candidate.origin_level.wire(),
            hop_profile: candidate.hop_profile,
        })
    }

    /// 在业务副作用前执行精确 Principal/Binding/Service/Opcode/方向 ACL。
    ///
    /// # Errors
    ///
    /// Session 不可用、请求不是 canonical key 或没有精确 ACL 条目时返回错误。
    pub fn authorize(&self, request: AccessRequest, facts: CurrentFacts) -> Result<()> {
        let slot = self.slot(request.session)?;
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        if request.protocol_opcode != 0
            && ProtocolOpcode::try_from(request.protocol_opcode).is_err()
        {
            return Err(Error::Argument);
        }
        if self.config.acl.iter().any(|rule| {
            rule.peer_principal == candidate.peer.principal
                && rule.peer_binding_generation == candidate.peer.generation
                && rule.context_fingerprint == request.context_fingerprint
                && rule.service == request.service
                && rule.protocol_opcode == request.protocol_opcode
                && rule.direction == request.direction
        }) {
            Ok(())
        } else {
            Err(Error::Access)
        }
    }

    pub(crate) fn reserve_origin_replay(
        &mut self,
        session: SessionHandle,
        facts: CurrentFacts,
        sequence: u64,
        aad_digest: [u8; 16],
        payload_digest: [u8; 16],
    ) -> Result<ReplayMutationHandle> {
        let owner_instance = self.config.owner_instance;
        let reservation_lifetime = self.config.replay_reservation_lifetime_us;
        let slot_index = self.slot_index(session)?;
        let slot_number = session.slot;
        let slot = &mut self.sessions[slot_index];
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        let deadline = facts
            .now_us
            .checked_add(reservation_lifetime)
            .ok_or(Error::Exhausted)?
            .min(candidate.expires_at_us);
        slot.origin_replay.reserve(
            owner_instance,
            slot_number,
            session.slot_generation,
            candidate.session_generation.get(),
            candidate.origin_rx.key_generation,
            candidate.origin_fingerprint.bytes(),
            sequence,
            facts.now_us,
            deadline,
            aad_digest,
            payload_digest,
        )
    }

    pub(crate) fn reserve_hop_replay(
        &mut self,
        session: SessionHandle,
        facts: CurrentFacts,
        sequence: u64,
        aad_digest: [u8; 16],
        payload_digest: [u8; 16],
    ) -> Result<ReplayMutationHandle> {
        let owner_instance = self.config.owner_instance;
        let reservation_lifetime = self.config.replay_reservation_lifetime_us;
        let slot_index = self.slot_index(session)?;
        let slot_number = session.slot;
        let slot = &mut self.sessions[slot_index];
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        let key_generation = candidate.hop_rx.ok_or(Error::State)?.key_generation;
        let deadline = facts
            .now_us
            .checked_add(reservation_lifetime)
            .ok_or(Error::Exhausted)?
            .min(candidate.expires_at_us);
        slot.hop_replay.reserve(
            owner_instance,
            slot_number,
            session.slot_generation,
            candidate.session_generation.get(),
            key_generation,
            candidate.hop_fingerprint.bytes(),
            sequence,
            facts.now_us,
            deadline,
            aad_digest,
            payload_digest,
        )
    }

    pub(crate) fn commit_replay(
        &mut self,
        session: SessionHandle,
        facts: CurrentFacts,
        handle: ReplayMutationHandle,
        hop: bool,
    ) -> Result<ReplayEvidence> {
        if handle.owner_instance != self.config.owner_instance
            || handle.session_slot != session.slot
            || handle.session_slot_generation != session.slot_generation
        {
            return Err(Error::NotFound);
        }
        let slot = self.slot_mut(session)?;
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        let expected_key = if hop {
            candidate.hop_rx.ok_or(Error::State)?.key_generation
        } else {
            candidate.origin_rx.key_generation
        };
        if handle.key_generation != expected_key {
            return Err(Error::State);
        }
        if hop {
            slot.hop_replay.commit(handle, facts.now_us)
        } else {
            slot.origin_replay.commit(handle, facts.now_us)
        }
    }

    pub(crate) fn preflight_replay(
        &self,
        session: SessionHandle,
        facts: CurrentFacts,
        handle: ReplayMutationHandle,
        hop: bool,
    ) -> Result<()> {
        if handle.owner_instance != self.config.owner_instance
            || handle.session_slot != session.slot
            || handle.session_slot_generation != session.slot_generation
        {
            return Err(Error::NotFound);
        }
        let slot = self.slot(session)?;
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        let expected_key = if hop {
            candidate.hop_rx.ok_or(Error::State)?.key_generation
        } else {
            candidate.origin_rx.key_generation
        };
        if handle.key_generation != expected_key {
            return Err(Error::State);
        }
        if hop {
            slot.hop_replay.preflight_commit(handle, facts.now_us)
        } else {
            slot.origin_replay.preflight_commit(handle, facts.now_us)
        }
    }

    pub(crate) fn abort_replay(
        &mut self,
        session: SessionHandle,
        handle: ReplayMutationHandle,
        hop: bool,
    ) -> Result<()> {
        if handle.owner_instance != self.config.owner_instance
            || handle.session_slot != session.slot
            || handle.session_slot_generation != session.slot_generation
        {
            return Err(Error::NotFound);
        }
        let slot = self.slot_mut(session)?;
        if hop {
            slot.hop_replay.abort(handle)
        } else {
            slot.origin_replay.abort(handle)
        }
    }

    pub(crate) fn classify_origin_replay(
        &self,
        session: SessionHandle,
        facts: CurrentFacts,
        sequence: u64,
    ) -> Result<crate::ReplayClassification> {
        let slot = self.slot(session)?;
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        slot.origin_replay.classify(sequence)
    }

    pub(crate) fn origin_replay_evidence(
        &self,
        session: SessionHandle,
        facts: CurrentFacts,
        sequence: u64,
        aad_digest: [u8; 16],
        payload_digest: [u8; 16],
    ) -> Result<ReplayEvidence> {
        let slot = self.slot(session)?;
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        slot.origin_replay.evidence(
            sequence,
            candidate.session_generation.get(),
            candidate.origin_rx.key_generation,
            candidate.origin_fingerprint.bytes(),
            aad_digest,
            payload_digest,
        )
    }

    pub(crate) fn candidate(
        &self,
        session: SessionHandle,
        facts: CurrentFacts,
    ) -> Result<HandshakeCandidate> {
        let slot = self.slot(session)?;
        let candidate = slot.candidate.ok_or(Error::State)?;
        validate_active_slot(slot, candidate, facts)?;
        Ok(candidate)
    }

    pub(crate) fn next_claim(&mut self, kind: u16) -> Result<CallbackClaim> {
        let operation_id = self.next_operation_id;
        self.next_operation_id = operation_id.checked_add(1).ok_or(Error::Exhausted)?;
        CallbackClaim::new(self.config.owner_instance, operation_id, 1, kind)
    }

    pub(crate) const fn gate(&self) -> &'a CallbackGate {
        self.config.provider_gate
    }

    pub(crate) const fn address_width(&self) -> AddressWidth {
        self.config.address_width
    }

    pub(crate) const fn realm(&self) -> RealmId {
        self.config.realm
    }

    pub(crate) const fn owner_instance(&self) -> u32 {
        self.config.owner_instance
    }

    fn call_verify_session<P: CryptoProvider>(
        &mut self,
        provider: &mut P,
        candidate: HandshakeCandidate,
        proof: &[u8],
    ) -> Result<()> {
        let claim = self.next_claim(CALLBACK_VERIFY_PROOF)?;
        let lease = self.config.provider_gate.try_enter(claim)?;
        let result = provider.verify_session_proof(&candidate, proof);
        let leave_result = self.config.provider_gate.leave(lease);
        result?;
        leave_result
    }

    fn slot(&self, handle: SessionHandle) -> Result<&SessionSlot<REPLAY_SLOTS>> {
        let index = self.slot_index(handle)?;
        let slot = &self.sessions[index];
        if !slot.occupied
            || slot.slot_generation != handle.slot_generation
            || slot.candidate.map(|value| value.session_generation)
                != Some(handle.session_generation)
        {
            return Err(Error::NotFound);
        }
        Ok(slot)
    }

    fn slot_mut(&mut self, handle: SessionHandle) -> Result<&mut SessionSlot<REPLAY_SLOTS>> {
        let index = self.slot_index(handle)?;
        let slot = &mut self.sessions[index];
        if !slot.occupied
            || slot.slot_generation != handle.slot_generation
            || slot.candidate.map(|value| value.session_generation)
                != Some(handle.session_generation)
        {
            return Err(Error::NotFound);
        }
        Ok(slot)
    }

    fn slot_index(&self, handle: SessionHandle) -> Result<usize> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        if index >= SESSIONS {
            return Err(Error::NotFound);
        }
        Ok(index)
    }
}

fn validate_active_slot<const REPLAY_SLOTS: usize>(
    slot: &SessionSlot<REPLAY_SLOTS>,
    candidate: HandshakeCandidate,
    facts: CurrentFacts,
) -> Result<()> {
    if slot.phase != SessionPhase::Active
        || facts.local != candidate.local
        || facts.peer != candidate.peer
        || facts.link_generation != candidate.link_generation
        || facts.policy_generation != candidate.policy_generation
        || facts.now_us >= candidate.expires_at_us
    {
        return Err(Error::State);
    }
    Ok(())
}

fn next_hop_key_high_water(prior: u32, next: Option<KeySelector>) -> Result<u32> {
    match next {
        Some(selector) if selector.key_generation <= prior => Err(Error::Replay),
        Some(selector) => Ok(selector.key_generation),
        None => Ok(prior),
    }
}

fn encode_session_record(
    realm: RealmId,
    width: AddressWidth,
    candidate: HandshakeCandidate,
    outbound_hop_high_water: u32,
    inbound_hop_high_water: u32,
    output: &mut [u8; SESSION_RECORD_BYTES],
) -> Result<()> {
    output.fill(0);
    output[..4].copy_from_slice(SESSION_MAGIC);
    output[4..6].copy_from_slice(&SESSION_SCHEMA_VERSION.to_be_bytes());
    output[6] = width as u8;
    output[7] = u8::from(candidate.origin_level.wire());
    output[8] = u8::from(candidate.hop_profile);
    output[12..16].copy_from_slice(&realm.get().to_be_bytes());
    encode_binding(candidate.local, &mut output[16..40]);
    encode_binding(candidate.peer, &mut output[40..64]);
    output[64..68].copy_from_slice(&candidate.link_generation.get().to_be_bytes());
    output[68..72].copy_from_slice(&candidate.session_generation.get().to_be_bytes());
    output[72..76].copy_from_slice(&candidate.policy_generation.to_be_bytes());
    output[76..84].copy_from_slice(&candidate.expires_at_us.to_be_bytes());
    encode_selector(candidate.origin_tx, &mut output[84..92]);
    encode_selector(candidate.origin_rx, &mut output[92..100]);
    encode_optional_selector(candidate.hop_tx, &mut output[100..108]);
    encode_optional_selector(candidate.hop_rx, &mut output[108..116]);
    output[116..132].copy_from_slice(&candidate.origin_fingerprint.bytes());
    output[132..148].copy_from_slice(&candidate.hop_fingerprint.bytes());
    output[148..180].copy_from_slice(&candidate.transcript_digest);
    output[180..184].copy_from_slice(&outbound_hop_high_water.to_be_bytes());
    output[184..188].copy_from_slice(&inbound_hop_high_water.to_be_bytes());
    decode_session_record(output).map(|_| ())
}

struct DecodedSessionRecord {
    realm: RealmId,
    width: AddressWidth,
    candidate: HandshakeCandidate,
    outbound_hop_key_generation: u32,
    inbound_hop_key_generation: u32,
}

/// 解码并验证固定 Security Session Record。
///
/// # Errors
///
/// 长度、Magic、保留位、枚举、地址、Selector 或 canonical 零填充无效时返回错误。
pub fn decode_session_record(input: &[u8]) -> Result<(RealmId, AddressWidth, HandshakeCandidate)> {
    let decoded = decode_session_record_full(input)?;
    Ok((decoded.realm, decoded.width, decoded.candidate))
}

fn decode_session_record_full(input: &[u8]) -> Result<DecodedSessionRecord> {
    if input.len() != SESSION_RECORD_BYTES
        || &input[..4] != SESSION_MAGIC
        || u16::from_be_bytes([input[4], input[5]]) != SESSION_SCHEMA_VERSION
        || input[9..12] != [0; 3]
        || input[188..] != [0; 4]
    {
        return Err(Error::Malformed);
    }
    let width = AddressWidth::try_from(input[6]).map_err(|_| Error::Malformed)?;
    let realm = RealmId::new(u32::from_be_bytes(
        input[12..16].try_into().map_err(|_| Error::Malformed)?,
    ))
    .map_err(|_| Error::Malformed)?;
    let origin = match OriginSecurity::try_from(input[7])? {
        OriginSecurity::O1 => SecurityLevel::Authenticated,
        OriginSecurity::O2 => SecurityLevel::Confidential,
        OriginSecurity::O0 => return Err(Error::Malformed),
    };
    let hop = HopProfile::try_from(input[8])?;
    let candidate = HandshakeCandidate {
        local: decode_binding(&input[16..40], width)?,
        peer: decode_binding(&input[40..64], width)?,
        link_generation: LinkInstanceGeneration::new(u32::from_be_bytes(
            input[64..68].try_into().map_err(|_| Error::Malformed)?,
        ))
        .map_err(|_| Error::Malformed)?,
        session_generation: PeerSessionGeneration::new(u32::from_be_bytes(
            input[68..72].try_into().map_err(|_| Error::Malformed)?,
        ))
        .map_err(|_| Error::Malformed)?,
        policy_generation: u32::from_be_bytes(
            input[72..76].try_into().map_err(|_| Error::Malformed)?,
        ),
        expires_at_us: u64::from_be_bytes(input[76..84].try_into().map_err(|_| Error::Malformed)?),
        origin_level: origin,
        hop_profile: hop,
        origin_tx: decode_selector(&input[84..92])?.ok_or(Error::Malformed)?,
        origin_rx: decode_selector(&input[92..100])?.ok_or(Error::Malformed)?,
        hop_tx: decode_selector(&input[100..108])?,
        hop_rx: decode_selector(&input[108..116])?,
        origin_fingerprint: Fingerprint::new(
            input[116..132].try_into().map_err(|_| Error::Malformed)?,
        )
        .map_err(|_| Error::Malformed)?,
        hop_fingerprint: Fingerprint::new(
            input[132..148].try_into().map_err(|_| Error::Malformed)?,
        )
        .map_err(|_| Error::Malformed)?,
        transcript_digest: input[148..180].try_into().map_err(|_| Error::Malformed)?,
    };
    candidate.validate(width, 0).map_err(|_| Error::Malformed)?;
    let outbound_hop_key_generation =
        u32::from_be_bytes(input[180..184].try_into().map_err(|_| Error::Malformed)?);
    let inbound_hop_key_generation =
        u32::from_be_bytes(input[184..188].try_into().map_err(|_| Error::Malformed)?);
    if (outbound_hop_key_generation == 0) != (inbound_hop_key_generation == 0)
        || candidate
            .hop_tx
            .is_some_and(|selector| selector.key_generation != outbound_hop_key_generation)
        || candidate
            .hop_rx
            .is_some_and(|selector| selector.key_generation != inbound_hop_key_generation)
    {
        return Err(Error::Malformed);
    }
    Ok(DecodedSessionRecord {
        realm,
        width,
        candidate,
        outbound_hop_key_generation,
        inbound_hop_key_generation,
    })
}

fn encode_binding(binding: Binding, output: &mut [u8]) {
    output[..4].copy_from_slice(&binding.address.get().to_be_bytes());
    output[4..8].copy_from_slice(&binding.generation.get().to_be_bytes());
    output[8..24].copy_from_slice(&binding.principal);
}

fn decode_binding(input: &[u8], width: AddressWidth) -> Result<Binding> {
    let binding = Binding {
        address: NodeAddress::new(
            u32::from_be_bytes(input[..4].try_into().map_err(|_| Error::Malformed)?),
            width,
        )
        .map_err(|_| Error::Malformed)?,
        generation: BindingGeneration::active(u32::from_be_bytes(
            input[4..8].try_into().map_err(|_| Error::Malformed)?,
        ))
        .map_err(|_| Error::Malformed)?,
        principal: input[8..24].try_into().map_err(|_| Error::Malformed)?,
    };
    binding.validate().map_err(|_| Error::Malformed)?;
    Ok(binding)
}

fn encode_selector(selector: KeySelector, output: &mut [u8]) {
    output.fill(0);
    output[0] = u8::from(selector.suite);
    output[2..4].copy_from_slice(&selector.key_id.get().to_be_bytes());
    output[4..8].copy_from_slice(&selector.key_generation.to_be_bytes());
}

fn encode_optional_selector(selector: Option<KeySelector>, output: &mut [u8]) {
    match selector {
        Some(selector) => encode_selector(selector, output),
        None => output.fill(0),
    }
}

fn decode_selector(input: &[u8]) -> Result<Option<KeySelector>> {
    if input == [0; 8] {
        return Ok(None);
    }
    if input[1] != 0 {
        return Err(Error::Malformed);
    }
    Ok(Some(KeySelector {
        suite: SuiteId::try_from(input[0])?,
        key_id: KeyId::new(u16::from_be_bytes([input[2], input[3]]))
            .map_err(|_| Error::Malformed)?,
        key_generation: u32::from_be_bytes(input[4..8].try_into().map_err(|_| Error::Malformed)?),
    }))
}
