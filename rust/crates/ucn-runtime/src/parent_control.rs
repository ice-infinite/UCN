//! Actual authenticated Parent publication; Coordinator owns all cross-Owner calls.
//! 真实认证父发布；跨Owner调用均由Coordinator负责。
use crate::{
    ControlBody, ControlCoordinator, ControlHandle, ControlReplies, JournalPeer, ReplyHandle,
};
use ucn_adapter::{AdapterOwner, TxDriver, TxState, TxToken};
use ucn_digest::DigestWorkspace;
use ucn_durable::{DurableBase, DurableContract};
use ucn_persistence::{CodecWorkspace, PersistenceOwner};
use ucn_security::{
    AccessDirection, AccessRequest, C0TransactionOwner, CryptoProvider, OriginContext,
    OriginCounterOwner, PacketPlan, SecurityOwner, SecurityPacketWorkspace, SenderDirection,
};
use ucn_transport::{
    ParentKind, ParentOwner, ParentPublication, SecurityDomain, TransferParent,
    parent_context_digest,
};
use ucn_types::{
    DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile, InteractionRole, PayloadKind,
    ProtocolOpcode, Result, ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;

/// Online publication phase, independent of durable allocator state.
/// 线上发布阶段，独立于耐久分配器。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParentOnlinePhase {
    /// Target has no installed Parent; 目标尚无父域。
    Empty,
    /// Source PREPARE awaits authenticated Prepared; 源PREPARE待确认。
    PreparePending,
    /// Target reserved exact Parent; 目标预留精确父域。
    Prepared,
    /// Source COMMIT awaits authenticated Committed; 源COMMIT待确认。
    CommitPending,
    /// Both current local conditions hold; 本地线上条件成立，使用时仍需当前权限。
    Active,
    /// Irreversible within this instance; 本实例不可逆围栏。
    Fenced,
}
struct Outbound {
    opcode: ProtocolOpcode,
    length: usize,
    peer: JournalPeer,
    attempts: u16,
    next_try: u64,
    submitted: bool,
    token: Option<TxToken>,
}

// The private trait can only be implemented inside this module; callers cannot fabricate media.
// 私有trait只能在本模块实现，调用方不能伪造媒体验证器。
trait ParentMedia {}
impl<const D: usize, const Q: usize, const L: usize> ParentMedia for PersistenceOwner<'_, D, Q, L> {}

/// Borrowed current source evidence; holding it prevents allocator/media mutation.
/// 借用当前源证明；存活期间不能改分配器或媒体，不可Clone/自行构造。
pub struct SourceParentProof<'a> {
    owner: u32,
    parent: TransferParent,
    allocator: &'a ParentOwner,
    _media: &'a dyn ParentMedia,
}
impl SourceParentProof<'_> {
    #[cfg(feature = "transfer-sender")]
    pub(crate) const fn allocator(&self) -> &ParentOwner {
        self.allocator
    }
}

/// One static Peer publication, with fixed control holder and no fabricated proof input.
/// 一个静态Peer发布，固定控制Holder，不接受调用方伪造Proof。
pub struct ParentCoordinator {
    owner: u32,
    source: bool,
    runtime: u32,
    source_contract: Option<DurableContract>,
    lifetime: u64,
    retry: u64,
    max_attempts: u16,
    phase: ParentOnlinePhase,
    parent: Option<TransferParent>,
    proposal: Option<ParentPublication>,
    bound_peer: Option<JournalPeer>,
    bound_service: Option<ServiceId>,
    deadline: u64,
    last_now: u64,
    output: [u8; 96],
    outbound: Option<Outbound>,
}
impl ParentCoordinator {
    /// Empty target; authentication and exact identity arrive only through `ControlCoordinator`.
    /// 空目标，身份只从真实Control认证进入。
    /// # Errors
    /// Invalid fixed local timeout/retry policy rejects.
    pub fn target(owner: u32, lifetime: u64, retry: u64, max_attempts: u16) -> Result<Self> {
        if owner == 0
            || lifetime == 0
            || lifetime > i64::MAX as u64
            || retry == 0
            || retry > lifetime
            || max_attempts == 0
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            source: false,
            runtime: 0,
            source_contract: None,
            lifetime,
            retry,
            max_attempts,
            phase: ParentOnlinePhase::Empty,
            parent: None,
            proposal: None,
            bound_peer: None,
            bound_service: None,
            deadline: 0,
            last_now: 0,
            output: [0; 96],
            outbound: None,
        })
    }
    /// Start source only from the actual current Foundation and confirmed allocator.
    /// 源仅由实际当前Foundation与确认分配器开始。
    /// # Errors
    /// Wrong current record, pending update, stale composition or invalid time rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn source<const D: usize, const Q: usize, const L: usize>(
        owner: u32,
        parent: &ParentOwner,
        persistence: &PersistenceOwner<'_, D, Q, L>,
        peer: JournalPeer,
        transaction: u64,
        lifetime: u64,
        retry: u64,
        max_attempts: u16,
    ) -> Result<Self> {
        let mut result = Self::target(owner, lifetime, retry, max_attempts)?;
        let (contract, generation, digest) = parent.record_binding();
        let now = peer.facts.now_us;
        persistence.validate_consumer(
            contract.runtime,
            contract.persistence_owner,
            contract.domain,
            contract.owner,
        )?;
        if now == 0 || transaction == 0 || generation == 0 || parent.is_pending() {
            return Err(Error::State);
        }
        let view = persistence.domain_get(contract.domain)?;
        if view.pending {
            return Err(Error::State);
        }
        let mut body = [0; 88];
        let n = persistence.copy_body(contract.domain, &mut body)?;
        let base = DurableBase::from_view(contract, view, &body[..n], &mut CodecWorkspace::new())?;
        let p = parent.view().0.ok_or(Error::State)?;
        let (decoded, water, tx) = ucn_transport::decode_parent_record(&body[..n], p.security)?;
        if base.generation() != generation
            || base.record_digest() != digest
            || decoded != p
            || water != parent.view().1
            || tx != base.transaction()
            || p.kind != ParentKind::C1
        {
            return Err(Error::State);
        }
        result.deadline = now.checked_add(lifetime).ok_or(Error::Exhausted)?;
        result.last_now = now;
        result.phase = ParentOnlinePhase::PreparePending;
        result.source = true;
        result.runtime = contract.runtime;
        result.source_contract = Some(contract);
        result.parent = Some(p);
        result.proposal = Some(ParentPublication {
            transaction,
            generation: p.generation,
            digest: parent_context_digest(p, &mut DigestWorkspace::new())?,
            disposition: 0,
        });
        result.bound_peer = Some(peer);
        Ok(result)
    }
    /// Read phase, not authentication or physical completion proof; 只读阶段，非认证/物理证明。
    #[must_use]
    pub const fn phase(&self) -> ParentOnlinePhase {
        self.phase
    }
    /// Revalidate actual current source media and freeze a borrow for one control action.
    /// 复验实际当前源媒体，以借用冻结一次控制动作。
    /// # Errors
    /// Changed Parent, pending I/O, stale digest/schema or crossed composition rejects.
    pub fn source_proof<'a, const D: usize, const Q: usize, const L: usize>(
        &self,
        allocator: &'a ParentOwner,
        media: &'a PersistenceOwner<'_, D, Q, L>,
    ) -> Result<SourceParentProof<'a>> {
        if !self.source || self.phase == ParentOnlinePhase::Fenced || allocator.is_pending() {
            return Err(Error::State);
        }
        let p = self.parent.ok_or(Error::State)?;
        let (contract, generation, digest) = allocator.record_binding();
        if self.source_contract != Some(contract) {
            return Err(Error::Config);
        }
        media.validate_consumer(
            contract.runtime,
            contract.persistence_owner,
            contract.domain,
            contract.owner,
        )?;
        let view = media.domain_get(contract.domain)?;
        let mut body = [0; 88];
        let n = media.copy_body(contract.domain, &mut body)?;
        let base = DurableBase::from_view(contract, view, &body[..n], &mut CodecWorkspace::new())?;
        let (decoded, water, transaction) =
            ucn_transport::decode_parent_record(&body[..n], p.security)?;
        if view.pending
            || base.generation() != generation
            || base.record_digest() != digest
            || decoded != p
            || allocator.view().0 != Some(p)
            || water != allocator.view().1
            || transaction != base.transaction()
        {
            return Err(Error::State);
        }
        Ok(SourceParentProof {
            owner: self.owner,
            parent: p,
            allocator,
            _media: media,
        })
    }
    fn match_proof(&self, proof: &SourceParentProof<'_>) -> Result<()> {
        if !self.source || self.owner != proof.owner || self.parent != Some(proof.parent) {
            Err(Error::State)
        } else {
            if proof.allocator.view().0 != self.parent {
                return Err(Error::State);
            }
            Ok(())
        }
    }
    /// Irreversible local stop, retains physical holder; 本地不可逆停止，保留物理义务。
    pub fn stop(&mut self) {
        self.phase = ParentOnlinePhase::Fenced;
    }
    /// One handshake deadline check; Active is governed by current Session/Parent authority.
    /// 检查握手截止，Active使用时按当前Session/Parent门禁。
    /// # Errors
    /// Zero or regressing clock cannot advance the object.
    pub fn expire(&mut self, now: u64) -> Result<bool> {
        self.clock(now)?;
        self.last_now = now;
        if !matches!(
            self.phase,
            ParentOnlinePhase::Empty | ParentOnlinePhase::Active | ParentOnlinePhase::Fenced
        ) && now >= self.deadline
        {
            self.stop();
            return Ok(true);
        }
        Ok(false)
    }
    fn clock(&self, now: u64) -> Result<()> {
        if now == 0 || now < self.last_now {
            Err(Error::Argument)
        } else {
            Ok(())
        }
    }
    fn current<const S: usize, const R: usize>(
        &self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
    ) -> Result<TransferParent> {
        self.clock(peer.facts.now_us)?;
        let p = self.parent.ok_or(Error::State)?;
        if self.runtime != security.runtime_instance() {
            return Err(Error::Config);
        }
        let view = security.session_get(peer.session, peer.facts)?;
        let expected = if self.source {
            (p.source, p.destination)
        } else {
            (p.destination, p.source)
        };
        let key = if self.source {
            security.outbound_key_generation(peer.session, peer.facts)?
        } else {
            security.inbound_key_generation(peer.session, peer.facts)?
        };
        let local = peer.facts.local;
        let remote = peer.facts.peer;
        if p.security.session != view.session_generation.get()
            || view.hop_profile != HopProfile::H0
            || view.origin_security != p.security.origin
            || key != p.security.key
            || peer.fingerprint.bytes() != p.security.fingerprint
            || security.realm().get() != p.realm
            || peer.facts.policy_generation != p.security.policy
            || local.principal != expected.0.principal
            || local.address.get() != expected.0.address
            || local.generation.get() != expected.0.generation
            || remote.principal != expected.1.principal
            || remote.address.get() != expected.1.address
            || remote.generation.get() != expected.1.generation
            || peer.facts.link_generation.get() != peer.link.instance_generation()
        {
            return Err(Error::Access);
        }
        if let Some(old) = self.bound_peer {
            if old.link != peer.link
                || old.session != peer.session
                || old.fingerprint != peer.fingerprint
                || old.facts.local != local
                || old.facts.peer != remote
                || old.facts.policy_generation != peer.facts.policy_generation
                || old.facts.link_generation != peer.facts.link_generation
            {
                return Err(Error::Access);
            }
        }
        Ok(p)
    }
    /// Current online permission, not just disk presence; 当前线上许可，不是磁盘有Record。
    /// # Errors
    /// Not active, changed current authorization or stale clock rejects.
    pub fn online<const S: usize, const R: usize>(
        &self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
    ) -> Result<TransferParent> {
        if self.source || self.phase != ParentOnlinePhase::Active {
            return Err(Error::State);
        }
        self.current(peer, security)
    }
    /// Source permission includes the borrowed current durable proof.
    /// 源许可还包含当前耐久借用证明。
    /// # Errors
    /// Wrong proof or unavailable current permission rejects.
    pub fn online_source<const S: usize, const R: usize>(
        &self,
        proof: &SourceParentProof<'_>,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
    ) -> Result<TransferParent> {
        self.match_proof(proof)?;
        if self.phase != ParentOnlinePhase::Active {
            return Err(Error::State);
        }
        self.current(peer, security)
    }
    fn desired(&self) -> Result<ProtocolOpcode> {
        match self.phase {
            ParentOnlinePhase::PreparePending => Ok(ProtocolOpcode::TransportParentPrepare),
            ParentOnlinePhase::CommitPending => Ok(ProtocolOpcode::TransportParentCommit),
            _ => Err(Error::State),
        }
    }
    /// Seal source's exact stage once; retry reuses the same bytes and nonce.
    /// 源阶段仅保护一次，重试复用原字节和nonce。
    /// # Errors
    /// Active old token, current ACL/context/MTU, deadline or counter errors reject.
    #[allow(clippy::too_many_arguments)]
    pub fn seal<
        const S: usize,
        const R: usize,
        const W: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &mut self,
        proof: &SourceParentProof<'_>,
        peer: JournalPeer,
        service: ServiceId,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<()> {
        self.match_proof(proof)?;
        if self.bound_service.is_some_and(|old| old != service) {
            return Err(Error::Access);
        }
        let op = self.desired()?;
        self.current(peer, security)?;
        let now = peer.facts.now_us;
        if !self.source
            || now >= self.deadline
            || self.outbound.as_ref().is_some_and(|v| v.token.is_some())
        {
            return Err(Error::State);
        }
        if self.outbound.as_ref().is_some_and(|v| v.opcode == op) {
            return Ok(());
        }
        let transaction = counter.preview()?;
        let proposal = self.proposal.ok_or(Error::State)?;
        if op == ProtocolOpcode::TransportParentPrepare && proposal.transaction != transaction.get()
        {
            return Err(Error::State);
        }
        let width = security.address_width().bytes();
        let n = 25 + 2 * width;
        if n + 32 + 16 > adapter.link_mtu(peer.link)? {
            return Err(Error::NoSpace);
        }
        let mut prefix = [0; 33];
        CommonHeader {
            contract: HeaderContract::C0,
            origin_security: self.parent.ok_or(Error::State)?.security.origin,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::Request,
            payload_kind: PayloadKind::Control,
            traffic_class: TrafficClass::Q1,
            hop_limit: HopLimit::new(1)?,
        }
        .encode(&mut prefix);
        prefix[3..7].copy_from_slice(&security.realm().get().to_be_bytes());
        prefix[7..7 + width]
            .copy_from_slice(&peer.facts.local.address.get().to_be_bytes()[4 - width..]);
        prefix[7 + width..7 + 2 * width]
            .copy_from_slice(&peer.facts.peer.address.get().to_be_bytes()[4 - width..]);
        prefix[7 + 2 * width..11 + 2 * width]
            .copy_from_slice(&peer.facts.local.generation.get().to_be_bytes());
        prefix[11 + 2 * width..15 + 2 * width]
            .copy_from_slice(&peer.facts.peer.generation.get().to_be_bytes());
        prefix[15 + 2 * width..23 + 2 * width].copy_from_slice(&transaction.get().to_be_bytes());
        prefix[23 + 2 * width..n].copy_from_slice(&u16::from(op).to_be_bytes());
        let mut body = [0; 32];
        proposal.encode(op, &mut body)?;
        let length = security.protect_packet(
            crypto,
            OriginCounterOwner::Transaction(counter),
            peer.session,
            peer.facts,
            AccessRequest {
                session: peer.session,
                context_fingerprint: peer.fingerprint,
                service,
                protocol_opcode: u16::from(op),
                direction: AccessDirection::Outbound,
            },
            PacketPlan {
                prefix: &prefix[..n],
                payload: &body,
                canonical_identity: &prefix[3..n],
                origin_context: OriginContext::C0 {
                    fingerprint: peer.fingerprint,
                    transaction_id: transaction,
                    opcode: op,
                    direction: if peer.facts.local.principal < peer.facts.peer.principal {
                        SenderDirection::AscendingPrincipal
                    } else {
                        SenderDirection::DescendingPrincipal
                    },
                },
                hop_profile: HopProfile::H0,
            },
            None,
            scratch,
            &mut self.output,
        )?;
        self.outbound = Some(Outbound {
            opcode: op,
            length,
            peer,
            attempts: 0,
            next_try: now,
            submitted: false,
            token: None,
        });
        self.last_now = now;
        self.bound_service = Some(service);
        Ok(())
    }
    /// Actual Adapter submission under the real Security gate; 真实门控Adapter提交。
    /// # Errors
    /// Retry/deadline/current ACL, changed Link or still-held token rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn submit<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &mut self,
        proof: &SourceParentProof<'_>,
        peer: JournalPeer,
        service: ServiceId,
        security: &SecurityOwner<'_, S, R>,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<()> {
        self.match_proof(proof)?;
        if self.bound_service != Some(service) {
            return Err(Error::Access);
        }
        let desired = self.desired()?;
        self.current(peer, security)?;
        let now = peer.facts.now_us;
        let out = self.outbound.as_ref().ok_or(Error::State)?;
        if out.opcode != desired
            || out.token.is_some()
            || out.peer.link != peer.link
            || now < out.next_try
            || now >= self.deadline
            || out.attempts >= self.max_attempts
        {
            return Err(Error::State);
        }
        security.authorize(
            AccessRequest {
                session: peer.session,
                context_fingerprint: peer.fingerprint,
                service,
                protocol_opcode: u16::from(desired),
                direction: AccessDirection::Outbound,
            },
            peer.facts,
        )?;
        let next = now.checked_add(self.retry).ok_or(Error::Exhausted)?;
        if security.callback_gate().is_active() {
            return Err(Error::State);
        }
        let token = adapter.tx_reserve(peer.link, 2)?;
        let out = self.outbound.as_mut().ok_or(Error::State)?;
        out.token = Some(token);
        out.attempts += 1;
        out.next_try = next;
        let mut gated = crate::runtime_common::GatedDriver {
            driver,
            gate: security.callback_gate(),
            owner: self.owner,
        };
        let submitted = adapter.tx_submit(token, &self.output[..out.length], &mut gated);
        out.submitted = out.submitted
            || matches!(
                adapter.tx_view(token)?.state,
                TxState::Submitted | TxState::Completed | TxState::InDoubt | TxState::Cancelled
            );
        self.last_now = now;
        submitted?;
        Ok(())
    }
    /// Retire an exact completed or definitely unsubmitted attempt; 退休精确完成/未提交Attempt。
    /// # Errors
    /// UNKNOWN or pending Driver state retains token and frame.
    pub fn retire<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
        security: &SecurityOwner<'_, S, R>,
        driver: &mut impl TxDriver,
    ) -> Result<bool> {
        if self.runtime != security.runtime_instance() {
            return Err(Error::Config);
        }
        let Some(out) = self.outbound.as_mut() else {
            return Ok(false);
        };
        let Some(token) = out.token else {
            return Ok(false);
        };
        if self.phase == ParentOnlinePhase::Fenced
            && matches!(
                adapter.tx_view(token)?.state,
                TxState::Submitted | TxState::Reserved
            )
        {
            let mut gated = crate::runtime_common::GatedDriver {
                driver,
                gate: security.callback_gate(),
                owner: self.owner,
            };
            adapter.tx_cancel(token, &mut gated)?;
        }
        if matches!(
            adapter.tx_view(token)?.state,
            TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
        ) {
            adapter.tx_retire(token)?;
            out.token = None;
            return Ok(true);
        }
        Ok(false)
    }
    /// Local holder quiescence only; 本地Holder静止，不表示对端取消。
    #[must_use]
    pub fn quiescent(&self) -> bool {
        self.phase == ParentOnlinePhase::Fenced
            && self.outbound.as_ref().is_none_or(|v| v.token.is_none())
    }
}

impl<const Q: usize, const B: usize> ControlCoordinator<Q, B> {
    /// Actual authenticated command -> exact Parent state and reserved response.
    /// 真实认证命令转为精确父状态与预留回复。
    /// # Errors
    /// Wrong role/context/transaction/deadline/order or full replies leaves state unchanged.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn dispatch_parent<const S: usize, const R: usize, const C: usize, const F: usize>(
        &mut self,
        handle: ControlHandle,
        parent: &mut ParentCoordinator,
        source_proof: Option<&SourceParentProof<'_>>,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
        replies: &mut ControlReplies<C, F>,
    ) -> Result<Option<ReplyHandle>> {
        if parent.source {
            parent.match_proof(source_proof.ok_or(Error::State)?)?;
        }
        let command = self.view(handle)?;
        if parent
            .bound_service
            .is_some_and(|old| old != command.service())
        {
            return Err(Error::Access);
        }
        let ControlBody::Parent(op, proposal) = command.body() else {
            return Err(Error::Unsupported);
        };
        let now = peer.facts.now_us;
        parent.clock(now)?;
        if parent.phase == ParentOnlinePhase::Fenced
            || now < command.accepted_at()
            || command.header().hop_limit.get() != 1
        {
            return Err(Error::State);
        }
        if parent.runtime != 0 && parent.runtime != security.runtime_instance() {
            return Err(Error::Config);
        }
        let reverse = parent.source;
        let p = if parent.phase == ParentOnlinePhase::Empty {
            if parent.source || op != ProtocolOpcode::TransportParentPrepare {
                return Err(Error::State);
            }
            let (realm, source, destination) = command.evidence().identities();
            let (session, key, policy, origin, fingerprint) = command.evidence().security();
            TransferParent {
                kind: ParentKind::C1,
                id: 0,
                generation: proposal.generation,
                realm,
                source: ucn_transport::Binding {
                    principal: source.principal,
                    address: source.address.get(),
                    generation: source.generation.get(),
                },
                destination: ucn_transport::Binding {
                    principal: destination.principal,
                    address: destination.address.get(),
                    generation: destination.generation.get(),
                },
                security: SecurityDomain {
                    session,
                    key,
                    policy,
                    origin,
                    fingerprint,
                    public_trusted_o0: false,
                },
                feedback_flow: None,
            }
        } else {
            parent.current(peer, security)?
        };
        p.validate()?;
        crate::transfer_control::current_parent(
            p,
            command.evidence(),
            security,
            peer.session,
            peer.facts,
            reverse,
        )?;
        let transaction = command.evidence().sequence();
        if op == ProtocolOpcode::TransportParentPrepare && transaction != proposal.transaction {
            return Err(Error::Access);
        }
        if proposal.digest != parent_context_digest(p, &mut DigestWorkspace::new())?
            || proposal.generation != p.generation
            || parent.proposal.is_some_and(|v| {
                v.transaction != proposal.transaction
                    || v.generation != proposal.generation
                    || v.digest != proposal.digest
            })
        {
            return Err(Error::Access);
        }
        security.authorize(
            AccessRequest {
                session: peer.session,
                context_fingerprint: peer.fingerprint,
                service: command.service(),
                protocol_opcode: u16::from(op),
                direction: AccessDirection::Inbound,
            },
            peer.facts,
        )?;
        let deadline = if parent.phase == ParentOnlinePhase::Empty {
            now.checked_add(parent.lifetime).ok_or(Error::Exhausted)?
        } else {
            parent.deadline
        };
        if now >= deadline {
            return Err(Error::Timeout);
        }
        let sent = parent.outbound.as_ref().map_or(0, |v| {
            if !v.submitted {
                0
            } else if v.opcode == ProtocolOpcode::TransportParentPrepare {
                1
            } else {
                2
            }
        });
        let (next, disposition) = ucn_transport::parent_phase_transition(
            parent.source,
            parent.phase as u8,
            op,
            proposal.disposition,
            sent,
        )?;
        let next = match next {
            2 => ParentOnlinePhase::Prepared,
            3 => ParentOnlinePhase::CommitPending,
            4 => ParentOnlinePhase::Active,
            5 => ParentOnlinePhase::Fenced,
            _ => return Err(Error::State),
        };
        let reply = if let Some(disposition) = disposition {
            let value = ParentPublication {
                disposition,
                ..proposal
            };
            let h = replies.reserve(
                command.into(),
                ControlBody::Parent(ProtocolOpcode::TransportParentAccept, value),
                now,
                deadline,
            )?;
            Some(h)
        } else {
            None
        };
        self.ack(handle)?;
        parent.parent = Some(p);
        parent.proposal = Some(ParentPublication {
            disposition: 0,
            ..proposal
        });
        parent.bound_peer = Some(peer);
        parent.bound_service = Some(command.service());
        parent.runtime = security.runtime_instance();
        parent.deadline = deadline;
        parent.last_now = now;
        parent.phase = next;
        Ok(reply)
    }
}
