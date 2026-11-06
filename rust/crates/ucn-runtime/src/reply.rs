//! Fixed cached Origin replies and explicit Adapter obligations.
//! 固定缓存Origin回复，显式保留Adapter义务；不是交付成功或耐久证明。
use crate::{ControlBody, ControlCommand};
use ucn_adapter::{AdapterOwner, LinkHandle, TerminalOutcome, TxDriver, TxState, TxToken};
use ucn_security::{
    AccessDirection, AccessRequest, C0TransactionOwner, CryptoProvider, CurrentFacts, Fingerprint,
    HopSequenceOwner, OriginContext, OriginCounterOwner, PacketEvidence, PacketPlan, SecurityOwner,
    SecurityPacketWorkspace, SenderDirection, SessionHandle,
};
use ucn_types::{
    DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile, InteractionRole, PayloadKind,
    ProtocolOpcode, Result, ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;
#[cfg(feature = "capability-control")]
mod capability_parts;

/// Opaque cached reply receipt; 不透明缓存回复凭据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReplyHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// Logical reply state, separate from the original business state.
/// 回复状态独立于原业务状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReplyPhase {
    /// Durable obligation not yet confirmed; cannot seal or submit.
    /// 耐久义务尚未确认，禁止保护或发送。
    AwaitCommit,
    /// Owner committed; sealing still required; 业务已接纳，仍需保护回复。
    AwaitSeal,
    /// Immutable sealed bytes ready for bounded submission; 不变保护字节待提交。
    Ready,
    /// Adapter owns a physical attempt; `Adapter持有物理Attempt`。
    InFlight,
    /// Retained for exact authenticated duplicate request; 为精确认证重复请求保留。
    Cached,
}
/// Immutable inspection, not a remote delivery certificate; 只读检查，非远端交付证书。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReplyView {
    /// Logical state; 逻辑状态。
    pub phase: ReplyPhase,
    /// Original non-renewing expiry; 原始不续期截止。
    pub expires_at: u64,
    /// Driver invocation count; Driver调用次数。
    pub attempts: u16,
    /// Physical retirement still required; 尚需物理退休。
    pub physical_pending: bool,
}
/// Internal verified cause, shared by C0 requests and authenticated C1 fragments.
/// 内部已验证原因：供C0请求和认证C1分片共用，不复制整个业务命令。
#[derive(Clone, Copy)]
pub(crate) struct ReplyRequest {
    evidence: PacketEvidence,
    accepted_at: u64,
    service: ServiceId,
}
impl ReplyRequest {
    #[cfg(feature = "transfer")]
    pub(crate) const fn fragment(
        evidence: PacketEvidence,
        service: ServiceId,
        accepted_at: u64,
    ) -> Self {
        Self {
            evidence,
            accepted_at,
            service,
        }
    }
    const fn evidence(self) -> PacketEvidence {
        self.evidence
    }
    const fn accepted_at(self) -> u64 {
        self.accepted_at
    }
    const fn service(self) -> ServiceId {
        self.service
    }
}
impl From<ControlCommand> for ReplyRequest {
    fn from(command: ControlCommand) -> Self {
        Self {
            evidence: command.evidence(),
            accepted_at: command.accepted_at(),
            service: command.service(),
        }
    }
}
struct Reply {
    request: ReplyRequest,
    body: ControlBody,
    expires: u64,
    next_try: u64,
    attempts: u16,
    phase: ReplyPhase,
    token: Option<TxToken>,
    session: Option<SessionHandle>,
    link: Option<LinkHandle>,
    facts: Option<CurrentFacts>,
    outbound_key: u32,
    length: usize,
}
/// Coordinator-owned fixed response cache; no heap, no direct business Owner calls.
/// Coordinator固定回复缓存：无堆、不直接调用业务Owner。
pub struct ControlReplies<const SLOTS: usize, const BYTES: usize> {
    owner: u32,
    retry_us: u64,
    max_attempts: u16,
    replies: [Option<Reply>; SLOTS],
    generations: [u32; SLOTS],
    bytes: [[u8; BYTES]; SLOTS],
    #[cfg(feature = "capability-control")]
    parts: [capability_parts::Parts<BYTES>; SLOTS],
    stopped: bool,
}
impl<const Q: usize, const B: usize> ControlReplies<Q, B> {
    /// Caller-owned fixed cache; 调用方固定缓存。
    /// # Errors
    /// Invalid owner/capacity/retry policy rejects before storage publication.
    pub const fn new(owner: u32, retry_us: u64, max_attempts: u16) -> Result<Self> {
        if owner == 0
            || Q == 0
            || Q > u16::MAX as usize
            || B < 96
            || retry_us == 0
            || retry_us > i64::MAX as u64
            || max_attempts == 0
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            retry_us,
            max_attempts,
            replies: [const { None }; Q],
            generations: [0; Q],
            bytes: [[0; B]; Q],
            #[cfg(feature = "capability-control")]
            parts: [const { capability_parts::Parts::new() }; Q],
            stopped: false,
        })
    }
    fn index(&self, h: ReplyHandle) -> Result<usize> {
        if h.owner != self.owner
            || h.slot >= Q
            || self.generations[h.slot] != h.generation
            || self.replies[h.slot].is_none()
        {
            return Err(Error::State);
        }
        Ok(h.slot)
    }
    #[cfg(any(
        feature = "transfer",
        feature = "operation-control",
        feature = "capability-control"
    ))]
    pub(crate) fn reserve(
        &mut self,
        request: ReplyRequest,
        body: ControlBody,
        now: u64,
        expires: u64,
    ) -> Result<ReplyHandle> {
        if self.stopped || now < request.accepted_at() || now == 0 || now >= expires {
            return Err(Error::State);
        }
        let slot = self
            .replies
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.replies[slot] = Some(Reply {
            request,
            body,
            expires,
            next_try: now,
            attempts: 0,
            phase: ReplyPhase::AwaitSeal,
            token: None,
            session: None,
            link: None,
            facts: None,
            outbound_key: 0,
            length: 0,
        });
        self.generations[slot] = generation;
        #[cfg(feature = "capability-control")]
        {
            self.parts[slot] = capability_parts::Parts::new();
        }
        Ok(ReplyHandle {
            owner: self.owner,
            slot,
            generation,
        })
    }
    #[cfg(any(
        feature = "transfer",
        feature = "operation-control",
        feature = "capability-control"
    ))]
    pub(crate) fn abandon_unpublished(&mut self, h: ReplyHandle) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        if !matches!(r.phase, ReplyPhase::AwaitSeal | ReplyPhase::AwaitCommit) {
            return Err(Error::State);
        }
        self.replies[slot] = None;
        Ok(())
    }
    #[cfg(feature = "operation-control")]
    pub(crate) fn reserve_pending(
        &mut self,
        request: ReplyRequest,
        body: ControlBody,
        now: u64,
        expires: u64,
    ) -> Result<ReplyHandle> {
        let h = self.reserve(request, body, now, expires)?;
        self.replies[h.slot].as_mut().ok_or(Error::State)?.phase = ReplyPhase::AwaitCommit;
        Ok(h)
    }
    #[cfg(feature = "operation-control")]
    pub(crate) fn publish_committed(
        &mut self,
        h: ReplyHandle,
        expected: ControlBody,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_mut().ok_or(Error::State)?;
        if r.phase != ReplyPhase::AwaitCommit || r.body != expected {
            return Err(Error::State);
        }
        r.phase = ReplyPhase::AwaitSeal;
        Ok(())
    }
    pub(crate) fn duplicate(&mut self, evidence: PacketEvidence, now: u64) -> Result<ReplyHandle> {
        if self.stopped {
            return Err(Error::State);
        }
        let slot = self
            .replies
            .iter()
            .position(|r| r.as_ref().is_some_and(|r| r.request.evidence() == evidence))
            .ok_or(Error::Replay)?;
        let h = ReplyHandle {
            owner: self.owner,
            slot,
            generation: self.generations[slot],
        };
        self.requeue(h, now)
    }
    pub(crate) fn requeue(&mut self, h: ReplyHandle, now: u64) -> Result<ReplyHandle> {
        if self.stopped {
            return Err(Error::State);
        }
        let slot = self.index(h)?;
        let r = self.replies[slot].as_mut().ok_or(Error::State)?;
        if r.phase == ReplyPhase::AwaitCommit {
            return Err(Error::State);
        }
        if now < r.request.accepted_at() || now >= r.expires {
            return Err(Error::Timeout);
        }
        if r.phase == ReplyPhase::Cached && r.attempts < self.max_attempts {
            r.phase = ReplyPhase::Ready;
            #[cfg(feature = "capability-control")]
            {
                self.parts[slot].index = 0;
            }
            // Do not bypass retry spacing or refresh lifetime on duplicate traffic.
        }
        Ok(h)
    }
    #[cfg(feature = "transfer")]
    pub(crate) fn forget_sack(&mut self, h: ReplyHandle) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        if !matches!(r.body, ControlBody::TransferSack(_))
            || r.phase != ReplyPhase::Cached
            || r.token.is_some()
        {
            return Err(Error::State);
        }
        // Notification can be regenerated from retained authenticated fragment evidence.
        // 通知可从保留的认证分片证据重新生成；旧密文永不改写或复用计数器。
        self.replies[slot] = None;
        self.bytes[slot].fill(0);
        Ok(())
    }
    #[cfg(feature = "transfer")]
    pub(crate) fn sack_matches(
        &self,
        h: ReplyHandle,
        expected: ucn_transport::Sack,
    ) -> Result<bool> {
        let r = self.replies[self.index(h)?].as_ref().ok_or(Error::State)?;
        Ok(r.body == ControlBody::TransferSack(expected))
    }
    /// Read an exact retained obligation; 读取精确保留义务。
    /// # Errors
    /// Stale or cross-owner handles cannot inspect reused slots.
    pub fn view(&self, h: ReplyHandle) -> Result<ReplyView> {
        let r = self.replies[self.index(h)?].as_ref().ok_or(Error::State)?;
        Ok(ReplyView {
            phase: r.phase,
            expires_at: r.expires,
            attempts: r.attempts,
            physical_pending: r.token.is_some(),
        })
    }
    #[cfg(any(
        feature = "journal-runtime",
        feature = "source-runtime",
        feature = "capability-control"
    ))]
    pub(crate) fn handle_at(&self, slot: usize) -> Option<ReplyHandle> {
        if slot < Q && self.replies[slot].is_some() {
            Some(ReplyHandle {
                owner: self.owner,
                slot,
                generation: self.generations[slot],
            })
        } else {
            None
        }
    }
    #[cfg(any(
        feature = "journal-runtime",
        feature = "source-runtime",
        feature = "capability-control"
    ))]
    pub(crate) fn scheduled_ready(&self, h: ReplyHandle, now: u64) -> Result<bool> {
        let r = self.replies[self.index(h)?].as_ref().ok_or(Error::State)?;
        Ok(r.phase == ReplyPhase::Ready
            && r.token.is_none()
            && now >= r.next_try
            && r.attempts < self.max_attempts)
    }
    fn current(r: &Reply, facts: CurrentFacts) -> Result<()> {
        let (_, peer, local) = r.request.evidence().identities();
        let (session, _, policy, _, _) = r.request.evidence().security();
        if facts.local != local
            || facts.peer != peer
            || facts.policy_generation != policy
            || session == 0
            || facts.now_us < r.request.accepted_at()
            || facts.now_us >= r.expires
        {
            return Err(Error::Access);
        }
        if r.facts.is_some_and(|old| {
            old.local != facts.local
                || old.peer != facts.peer
                || old.link_generation != facts.link_generation
                || old.policy_generation != facts.policy_generation
        }) {
            return Err(Error::State);
        }
        Ok(())
    }
    /// Build and seal the exact reverse C0 response once; retries reuse sealed bytes.
    /// 只构建并保护一次精确反向C0响应，重试复用原字节。
    /// # Errors
    /// Wrong identity/session/ACL/domain/lease or crypto failure retains unsealed obligation.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn seal<const S: usize, const R: usize, const W: usize>(
        &mut self,
        h: ReplyHandle,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        session: SessionHandle,
        facts: CurrentFacts,
        link: LinkHandle,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<()> {
        self.seal_with_hop(
            h, security, crypto, counter, session, facts, link, None, scratch,
        )
    }
    /// Seal a response with the caller-owned shared per-session Hop counter.
    /// 使用调用方持有的会话共享Hop计数器保护回复；H0仍使用None。
    /// # Errors
    /// A mismatched profile/counter or stale dependency publishes no packet.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn seal_with_hop<const S: usize, const R: usize, const W: usize>(
        &mut self,
        h: ReplyHandle,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        session: SessionHandle,
        facts: CurrentFacts,
        link: LinkHandle,
        hop: Option<&mut dyn HopSequenceOwner>,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        if self.stopped || r.phase != ReplyPhase::AwaitSeal {
            return Err(Error::State);
        }
        Self::current(r, facts)?;
        if link.instance_generation() != facts.link_generation.get() {
            return Err(Error::State);
        }
        let (realm, _, _) = r.request.evidence().identities();
        let (generation, _, policy, origin, fp) = r.request.evidence().security();
        let view = security.session_get(session, facts)?;
        if realm != security.realm().get()
            || view.session_generation.get() != generation
            || view.origin_security != origin
            || !matches!(view.hop_profile, HopProfile::H0 | HopProfile::H1)
        {
            return Err(Error::Access);
        }
        #[cfg(feature = "operation-control")]
        let mut body = [0; 200]; // Exact maximum: recovery prefix54 + inline result146.
        #[cfg(all(not(feature = "operation-control"), feature = "capability-control"))]
        let mut body = [0; 75];
        #[cfg(not(any(feature = "operation-control", feature = "capability-control")))]
        let mut body = [0; 64];
        let (opcode, body_bytes) = encode_reply(r.body, &mut body)?;
        let width = security.address_width().bytes();
        let mut prefix = [0; 33];
        let n = 25 + width * 2;
        CommonHeader {
            contract: HeaderContract::C0,
            origin_security: origin,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: reply_role(r.body),
            payload_kind: PayloadKind::Control,
            traffic_class: reply_class(r.body),
            hop_limit: HopLimit::new(1)?,
        }
        .encode(&mut prefix);
        prefix[3..7].copy_from_slice(&realm.to_be_bytes());
        prefix[7..7 + width].copy_from_slice(&facts.local.address.get().to_be_bytes()[4 - width..]);
        prefix[7 + width..7 + width * 2]
            .copy_from_slice(&facts.peer.address.get().to_be_bytes()[4 - width..]);
        let o = 7 + width * 2;
        prefix[o..o + 4].copy_from_slice(&facts.local.generation.get().to_be_bytes());
        prefix[o + 4..o + 8].copy_from_slice(&facts.peer.generation.get().to_be_bytes());
        let transaction = counter.preview()?;
        prefix[o + 8..o + 16].copy_from_slice(&transaction.get().to_be_bytes());
        prefix[o + 16..n].copy_from_slice(&u16::from(opcode).to_be_bytes());
        let fingerprint = Fingerprint::new(fp)?;
        let access = AccessRequest {
            session,
            context_fingerprint: fingerprint,
            service: r.request.service(),
            protocol_opcode: u16::from(opcode),
            direction: AccessDirection::Outbound,
        };
        let outbound_key = security.outbound_key_generation(session, facts)?;
        let length = security.protect_packet(
            crypto,
            OriginCounterOwner::Transaction(counter),
            session,
            facts,
            access,
            PacketPlan {
                prefix: &prefix[..n],
                payload: &body[..body_bytes],
                canonical_identity: &prefix[3..n],
                origin_context: OriginContext::C0 {
                    fingerprint,
                    transaction_id: transaction,
                    opcode,
                    direction: if facts.local.principal < facts.peer.principal {
                        SenderDirection::AscendingPrincipal
                    } else {
                        SenderDirection::DescendingPrincipal
                    },
                },
                hop_profile: view.hop_profile,
            },
            hop,
            scratch,
            &mut self.bytes[slot],
        )?;
        let r = self.replies[slot].as_mut().ok_or(Error::State)?;
        r.phase = ReplyPhase::Ready;
        r.session = Some(session);
        r.link = Some(link);
        r.facts = Some(facts);
        r.outbound_key = outbound_key;
        r.length = length;
        // Policy was validated by the Security Owner, not inferred from a reply DTO.
        debug_assert_eq!(policy, facts.policy_generation);
        Ok(())
    }
    /// Refresh only the Hop trailer before retrying an immutable cached C0 response.
    /// 重试缓存C0回复前仅刷新Hop尾部，不重用或重算Origin Nonce。
    /// # Errors
    /// In-flight/expired/stale contexts preserve the cached bytes and holder.
    #[allow(clippy::too_many_arguments)]
    pub fn refresh_hop<const S: usize, const R: usize, const W: usize>(
        &mut self,
        h: ReplyHandle,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        hop: &mut dyn HopSequenceOwner,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        Self::current(r, facts)?;
        if self.stopped
            || r.phase != ReplyPhase::Ready
            || r.token.is_some()
            || r.session != Some(session)
            || r.length == 0
        {
            return Err(Error::State);
        }
        let (_, _, _, _, fp) = r.request.evidence().security();
        let opcode = reply_opcode(&r.body)?;
        #[cfg(feature = "capability-control")]
        let opcode = if self.parts[slot].active {
            ProtocolOpcode::CapabilityFragment
        } else {
            opcode
        };
        let frame = &mut self.bytes[slot][..r.length];
        #[cfg(feature = "capability-control")]
        let frame = if self.parts[slot].active && self.parts[slot].index == 1 {
            &mut self.parts[slot].second[..self.parts[slot].length]
        } else {
            frame
        };
        security.refresh_c0_hop(
            crypto,
            session,
            facts,
            AccessRequest {
                session,
                context_fingerprint: Fingerprint::new(fp)?,
                service: r.request.service(),
                protocol_opcode: u16::from(opcode),
                direction: AccessDirection::Outbound,
            },
            hop,
            scratch,
            frame,
        )
    }
    /// Copy retained Origin bytes for diagnostics; zero-write on short output.
    /// 复制保留Origin字节，输出不足时零写。
    /// # Errors
    /// Unsealed or short output rejects.
    pub fn copy_packet(&self, h: ReplyHandle, output: &mut [u8]) -> Result<usize> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        #[cfg(feature = "capability-control")]
        if self.parts[slot].active {
            return Err(Error::Unsupported);
        }
        if r.length == 0 {
            return Err(Error::State);
        }
        if output.len() < r.length {
            return Err(Error::NoSpace);
        }
        output[..r.length].copy_from_slice(&self.bytes[slot][..r.length]);
        Ok(r.length)
    }
    /// Submit a cached direct-link reply through an actual bounded Adapter.
    /// 通过实际有界Adapter提交缓存的直达回复。
    /// # Errors
    /// Backpressure retires only its physical Token; uncertainty retains it for later reconciliation.
    #[allow(clippy::too_many_arguments)]
    pub fn submit<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        h: ReplyHandle,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
        driver: &mut impl TxDriver,
        link: LinkHandle,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        if self.stopped || r.phase != ReplyPhase::Ready || r.token.is_some() {
            return Err(Error::State);
        }
        Self::current(r, facts)?;
        if r.session != Some(session) || r.link != Some(link) || facts.now_us < r.next_try {
            return Err(Error::State);
        }
        if r.attempts >= self.max_attempts {
            return Err(Error::Exhausted);
        }
        let (_, _, _, _, fp) = r.request.evidence().security();
        let opcode = reply_opcode(&r.body)?;
        security.authorize(
            AccessRequest {
                session,
                context_fingerprint: Fingerprint::new(fp)?,
                service: r.request.service(),
                protocol_opcode: u16::from(opcode),
                direction: AccessDirection::Outbound,
            },
            facts,
        )?;
        if security.outbound_key_generation(session, facts)? != r.outbound_key
            || link.instance_generation() != facts.link_generation.get()
        {
            return Err(Error::State);
        }
        let length = r.length;
        let frame = &self.bytes[slot][..length];
        #[cfg(feature = "capability-control")]
        let (length, frame) = if self.parts[slot].active && self.parts[slot].index == 1 {
            (
                self.parts[slot].length,
                &self.parts[slot].second[..self.parts[slot].length],
            )
        } else {
            (length, frame)
        };
        #[cfg(feature = "capability-control")]
        if self.parts[slot].active {
            security.authorize(
                AccessRequest {
                    session,
                    context_fingerprint: Fingerprint::new(fp)?,
                    service: r.request.service(),
                    protocol_opcode: ProtocolOpcode::CapabilityFragment.into(),
                    direction: AccessDirection::Outbound,
                },
                facts,
            )?;
        }
        if length > adapter.link_mtu(link)? {
            return Err(Error::NoSpace);
        }
        let next_try = facts
            .now_us
            .checked_add(self.retry_us)
            .ok_or(Error::Exhausted)?;
        let token = adapter.tx_reserve(link, u16::try_from(slot).map_err(|_| Error::Config)?)?;
        let record = self.replies[slot].as_mut().ok_or(Error::State)?;
        record.token = Some(token);
        record.phase = ReplyPhase::InFlight;
        record.attempts += 1;
        record.next_try = next_try;
        let view = adapter.tx_submit(token, frame, driver)?;
        if view.state == TxState::NotSubmitted {
            self.retire_attempt(h, adapter)?;
        }
        Ok(())
    }
    /// `peek -> Adapter retire -> release holder`, never destructive pop first.
    /// 先查看、Adapter退休成功、再释放Holder，不提前丢义务。
    /// # Errors
    /// In-flight/InDoubt/retirement error retains the exact Token and cache.
    pub fn retire_attempt<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: ReplyHandle,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        let token = r.token.ok_or(Error::State)?;
        let view = adapter.tx_view(token)?;
        if !matches!(
            view.state,
            TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
        ) {
            return Err(Error::State);
        }
        adapter.tx_retire(token)?;
        let r = self.replies[slot].as_mut().ok_or(Error::State)?;
        r.token = None;
        #[cfg(feature = "capability-control")]
        if !self.stopped
            && self.parts[slot].active
            && self.parts[slot].index == 0
            && view.terminal == Some(TerminalOutcome::Success)
        {
            self.parts[slot].index = 1;
            r.phase = ReplyPhase::Ready;
            r.next_try = 0; // Next fragment, not a retransmission/lease refresh.
            return Ok(());
        }
        r.phase = if self.stopped || view.terminal == Some(TerminalOutcome::Success) {
            ReplyPhase::Cached
        } else {
            ReplyPhase::Ready
        };
        Ok(())
    }
    /// Stop rejects new work but cannot erase accepted physical obligations.
    /// 停止拒绝新工作，不能抹掉已接纳物理义务。
    pub fn stop(&mut self) {
        self.stopped = true;
    }
    #[cfg(feature = "capability-control")]
    pub(crate) fn retire_if_terminal<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: ReplyHandle,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
    ) -> Result<bool> {
        let token = self.replies[self.index(h)?]
            .as_ref()
            .ok_or(Error::State)?
            .token
            .ok_or(Error::State)?;
        let view = adapter.tx_view(token)?;
        if matches!(
            view.state,
            TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
        ) {
            self.retire_attempt(h, adapter)?;
            return Ok(true);
        }
        Ok(false)
    }
    /// Cancel only after stop; driver failure preserves exact Token for retry.
    /// 仅停止后取消，Driver失败保留精确Token供重试。
    /// # Errors
    /// Unproven cancellation or retirement leaves the reply occupied.
    pub fn cancel_attempt<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: ReplyHandle,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
        driver: &mut impl TxDriver,
    ) -> Result<()> {
        if !self.stopped {
            return Err(Error::State);
        }
        self.cancel_physical(h, adapter, driver)
    }
    #[cfg(any(
        feature = "journal-runtime",
        feature = "source-runtime",
        feature = "capability-control"
    ))]
    pub(crate) fn cancel_expired_attempt<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: ReplyHandle,
        now: u64,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
        driver: &mut impl TxDriver,
    ) -> Result<()> {
        if !self.stopped && now < self.view(h)?.expires_at {
            return Err(Error::State);
        }
        self.cancel_physical(h, adapter, driver)
    }
    fn cancel_physical<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: ReplyHandle,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
        driver: &mut impl TxDriver,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let token = self.replies[slot]
            .as_ref()
            .ok_or(Error::State)?
            .token
            .ok_or(Error::State)?;
        let view = adapter.tx_view(token)?;
        if !matches!(
            view.state,
            TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
        ) {
            adapter.tx_cancel(token, driver)?;
        }
        self.retire_attempt(h, adapter)
    }
    /// Reclaim only expired cache with no Driver holder; 仅回收过期且无Driver持有的缓存。
    /// # Errors
    /// Active deadline/Token or stale handle prevents slot reuse.
    pub fn retire_cache(&mut self, h: ReplyHandle, now: u64) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        if (!self.stopped && now < r.expires)
            || r.token.is_some()
            || r.phase == ReplyPhase::AwaitCommit
        {
            return Err(Error::State);
        }
        self.replies[slot] = None;
        self.bytes[slot].fill(0);
        #[cfg(feature = "capability-control")]
        {
            self.parts[slot] = capability_parts::Parts::new();
        }
        Ok(())
    }
}

#[allow(clippy::trivially_copy_pass_by_ref)] // Shared borrowed signature; feature-rich Body exceeds 200 B.
fn reply_opcode(body: &ControlBody) -> Result<ProtocolOpcode> {
    match body {
        #[cfg(feature = "capability-control")]
        ControlBody::CapabilityAdvertise(_) => Ok(ProtocolOpcode::CapabilityAdvertise),
        #[cfg(feature = "capability-control")]
        ControlBody::CapabilityAck(_) => Ok(ProtocolOpcode::CapabilityAck),
        #[cfg(feature = "parent-control")]
        ControlBody::Parent(ProtocolOpcode::TransportParentAccept, _) => {
            Ok(ProtocolOpcode::TransportParentAccept)
        }
        #[cfg(feature = "operation-control")]
        ControlBody::OperationReceipt(_) => Ok(ProtocolOpcode::OperationReceipt),
        #[cfg(feature = "operation-control")]
        ControlBody::OperationResultAck(_) => Ok(ProtocolOpcode::OperationResultAck),
        #[cfg(feature = "operation-control")]
        ControlBody::OperationResult(_) => Ok(ProtocolOpcode::OperationResult),
        #[cfg(feature = "transfer")]
        ControlBody::TransferSetupAck(_) => Ok(ProtocolOpcode::TransferSetupAck),
        #[cfg(feature = "transfer")]
        ControlBody::TransferSack(_) => Ok(ProtocolOpcode::TransferSackCredit),
        #[cfg(feature = "transfer")]
        ControlBody::TransferTerminal(_) => Ok(ProtocolOpcode::TransferTerminalReceipt),
        _ => Err(Error::Unsupported),
    }
}
fn encode_reply(body: ControlBody, output: &mut [u8]) -> Result<(ProtocolOpcode, usize)> {
    #[cfg(not(any(
        feature = "transfer",
        feature = "operation-control",
        feature = "capability-control"
    )))]
    let _ = output; // No response-producing dispatcher is compiled in this configuration.
    match body {
        #[cfg(feature = "capability-control")]
        ControlBody::CapabilityAdvertise(v) => {
            if output.len() < 68 {
                return Err(Error::NoSpace);
            }
            ucn_capability::encode_capability_record(v, &mut output[..68])?;
            Ok((ProtocolOpcode::CapabilityAdvertise, 68))
        }
        #[cfg(feature = "capability-control")]
        ControlBody::CapabilityAck(v) => {
            if output.len() < 75 {
                return Err(Error::NoSpace);
            }
            ucn_capability::encode_profile_ack(v, &mut output[..75])?;
            Ok((ProtocolOpcode::CapabilityAck, 75))
        }
        #[cfg(feature = "parent-control")]
        ControlBody::Parent(op @ ProtocolOpcode::TransportParentAccept, v) => {
            Ok((op, v.encode(op, output)?))
        }
        #[cfg(feature = "operation-control")]
        ControlBody::OperationReceipt(v) => Ok((
            ProtocolOpcode::OperationReceipt,
            ucn_service::encode_operation_receipt(v, output)?,
        )),
        #[cfg(feature = "operation-control")]
        ControlBody::OperationResultAck(v) => Ok((
            ProtocolOpcode::OperationResultAck,
            ucn_service::encode_operation_ack(v, output)?,
        )),
        #[cfg(feature = "operation-control")]
        ControlBody::OperationResult(v) => Ok((
            ProtocolOpcode::OperationResult,
            ucn_service::encode_operation_result(v, output)?,
        )),
        #[cfg(feature = "transfer")]
        ControlBody::TransferSetupAck(v) => Ok((
            ProtocolOpcode::TransferSetupAck,
            ucn_transport::encode_setup_ack(v, output)?,
        )),
        #[cfg(feature = "transfer")]
        ControlBody::TransferSack(v) => Ok((
            ProtocolOpcode::TransferSackCredit,
            ucn_transport::encode_sack(v, output)?,
        )),
        #[cfg(feature = "transfer")]
        ControlBody::TransferTerminal(v) => Ok((
            ProtocolOpcode::TransferTerminalReceipt,
            ucn_transport::encode_terminal(v, output)?,
        )),
        _ => Err(Error::Unsupported),
    }
}
fn reply_role(body: ControlBody) -> InteractionRole {
    match body {
        #[cfg(feature = "capability-control")]
        ControlBody::CapabilityAck(_) => InteractionRole::Result,
        #[cfg(feature = "parent-control")]
        ControlBody::Parent(ProtocolOpcode::TransportParentAccept, _) => InteractionRole::Result,
        #[cfg(feature = "operation-control")]
        ControlBody::OperationReceipt(_) => InteractionRole::Result,
        #[cfg(feature = "operation-control")]
        ControlBody::OperationResult(_) => InteractionRole::Result,
        #[cfg(feature = "transfer")]
        ControlBody::TransferSetupAck(_) => InteractionRole::Result,
        _ => InteractionRole::OneWay,
    }
}
fn reply_class(body: ControlBody) -> TrafficClass {
    #[cfg(feature = "capability-control")]
    if matches!(
        body,
        ControlBody::CapabilityAdvertise(_) | ControlBody::CapabilityAck(_)
    ) {
        return TrafficClass::Q2;
    }
    let _ = body;
    TrafficClass::Q1
}
