//! Authenticated C0 + atomic Timed RX + actual T1/T2/T3/T4, one static direct Peer.
//! 认证C0、原子Timed RX及真实四事件；单个静态直连Peer，不冒充动态Path Root。
use crate::{ControlBody, ControlCoordinator};
use ucn_durable::{DurableBase, DurableContract};
use ucn_persistence::{CodecWorkspace, PersistenceOwner};
use ucn_realtime::{
    DomainConfig, EventDirection, EventKey, RetirementProof, SyncOutcome, SyncOwner,
    SyncSequencePermit, TimeDomain, TimeKey, TimeStampMessage, TimeTxnHandle, TimedDriver,
    TimedEventState, TimedLink, TimedRxClaim, TimedRxQueue, decode_domain, encode_time_key,
    encode_time_stamp,
};
use ucn_security::{
    AccessDirection, AccessRequest, C0TransactionOwner, CryptoProvider, CurrentFacts, Fingerprint,
    OpenPacketPlan, OriginContext, OriginCounterOwner, PacketPlan, SecurityOwner,
    SecurityPacketWorkspace, SenderDirection, SessionHandle,
};
use ucn_types::{
    C0TransactionId, DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile,
    InteractionRole, OriginSecurity, PayloadKind, ProtocolOpcode, Result, ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;

/// Product-selected static authority role; 产品选择的静态Authority角色。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TimeRuntimeRole {
    /// Configured single Master; 配置的单Master。
    Master,
    /// Configured Member; 配置的Member。
    Member,
}
/// Exact current Peer, rechecked by Security at every effect.
/// 精确当前Peer，每次副作用由Security复验。
#[derive(Clone, Copy)]
pub struct TimePeer {
    /// Actual Session handle; 实际会话Handle。
    pub session: SessionHandle,
    /// Trusted local now and bindings; 可信本地时刻与绑定。
    pub facts: CurrentFacts,
    /// Canonical security context; 规范安全上下文。
    pub fingerprint: Fingerprint,
}
trait TimeMedia {}
impl<const D: usize, const Q: usize, const L: usize> TimeMedia for PersistenceOwner<'_, D, Q, L> {}
/// Borrowed exact Foundation/Domain proof; private fields prevent fabricated evidence.
/// 借用精确Foundation/Domain证明，私有字段禁止伪造；不支持Clone。
pub struct TimeDomainProof<'a, const W: usize> {
    config: DomainConfig,
    runtime: u32,
    binding: (DurableContract, u64, [u8; 16]),
    _domain: &'a TimeDomain<W>,
    _media: &'a dyn TimeMedia,
}
/// Validate the current actual record before any network authority is used.
/// 使用网络Authority前验证实际当前Record，不接受调用方View或布尔值。
/// # Errors
/// Wrong schema/digest/config, pending I/O or unretired proof rejects.
pub fn time_domain_proof<'a, const W: usize, const D: usize, const Q: usize, const L: usize>(
    domain: &'a TimeDomain<W>,
    media: &'a PersistenceOwner<'_, D, Q, L>,
) -> Result<TimeDomainProof<'a, W>> {
    let (contract, config, generation, digest) = domain.record_binding()?;
    media.validate_consumer(
        contract.runtime,
        contract.persistence_owner,
        contract.domain,
        contract.owner,
    )?;
    let view = media.domain_get(contract.domain)?;
    if view.pending {
        return Err(Error::State);
    }
    let mut body = [0; 128];
    let n = media.copy_body(contract.domain, &mut body)?;
    let base = DurableBase::from_view(contract, view, &body[..n], &mut CodecWorkspace::new())?;
    if base.generation() != generation
        || base.record_digest() != digest
        || decode_domain(&body[..n])? != config
    {
        return Err(Error::State);
    }
    Ok(TimeDomainProof {
        config,
        runtime: contract.runtime,
        binding: (contract, generation, digest),
        _domain: domain,
        _media: media,
    })
}
#[derive(Clone, Copy)]
struct Holder {
    key: EventKey,
    claim: Option<TimedRxClaim>,
    proof: Option<RetirementProof>,
    sync: bool,
    ready: bool,
}
struct Outbound {
    opcode: ProtocolOpcode,
    body: [u8; 24],
    bytes: usize,
    sealed: usize,
    holder: Option<usize>,
    submitted_at: Option<u64>,
}
/// Fixed one-Peer four-message Coordinator. No raw token/Timestamp/Opened DTO entry.
/// 固定单Peer四报文Coordinator，不提供任意Token/Timestamp/Opened DTO入口。
pub struct TimeRuntime<'a, const BYTES: usize> {
    config: DomainConfig,
    role: TimeRuntimeRole,
    peer: TimePeer,
    runtime: u32,
    binding: (DurableContract, u64, [u8; 16]),
    gate: &'a ucn_owner::CallbackGate,
    link_instance: u32,
    key_generations: (u32, u32),
    origin: OriginSecurity,
    queue_id: u32,
    service: ServiceId,
    lifetime: u64,
    deadline: u64,
    last_now: u64,
    key: Option<TimeKey>,
    txn: Option<TimeTxnHandle>,
    sync: SyncOwner<1, 2>,
    control: ControlCoordinator<1, BYTES>,
    holders: [Option<Holder>; 6],
    retire_cursor: usize,
    outbound: Option<Outbound>,
    frame: [u8; BYTES],
    raw: [u8; BYTES],
    follow_up_sent: bool,
    outcome: Option<SyncOutcome>,
    stopped: bool,
}
impl<'a, const B: usize> TimeRuntime<'a, B> {
    /// Freeze one dedicated direct Link/Queue and the configured Master Principal.
    /// 冻结专属直连Link/Queue与配置的Master Principal；动态Route不在此入口。
    /// # Errors
    /// Unauthenticated/static identity, wrong shared gate/path/capacity or bounds rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn new<const W: usize, const S: usize, const R: usize, const N: usize>(
        owner: u32,
        role: TimeRuntimeRole,
        master: [u8; 16],
        proof: &TimeDomainProof<'_, W>,
        peer: TimePeer,
        security: &SecurityOwner<'a, S, R>,
        link: &TimedLink<'_, N>,
        queue_id: u32,
        service: ServiceId,
        lifetime: u64,
    ) -> Result<Self> {
        let config = proof.config;
        ucn_realtime::encode_domain(config)?;
        if proof.runtime != security.runtime_instance() {
            return Err(Error::Config);
        }
        if B < 96
            || B > 4096
            || queue_id == 0
            || lifetime == 0
            || lifetime > i64::MAX as u64
            || master == [0; 16]
            || master
                != if role == TimeRuntimeRole::Master {
                    peer.facts.local.principal
                } else {
                    peer.facts.peer.principal
                }
        {
            return Err(Error::Config);
        }
        let (_, instance, _) = link.identity();
        let result = Self {
            config,
            role,
            peer,
            runtime: security.runtime_instance(),
            binding: proof.binding,
            gate: security.callback_gate(),
            link_instance: instance,
            key_generations: (
                security.outbound_key_generation(peer.session, peer.facts)?,
                security.inbound_key_generation(peer.session, peer.facts)?,
            ),
            origin: security
                .session_get(peer.session, peer.facts)?
                .origin_security,
            queue_id,
            service,
            lifetime,
            deadline: 0,
            last_now: peer.facts.now_us,
            key: None,
            txn: None,
            sync: SyncOwner::with_link_instances(owner, instance, instance)?,
            control: ControlCoordinator::new(owner)?,
            holders: [None; 6],
            retire_cursor: 0,
            outbound: None,
            frame: [0; B],
            raw: [0; B],
            follow_up_sent: false,
            outcome: None,
            stopped: false,
        };
        result.current(peer, security, link)?;
        Ok(result)
    }
    fn current<const S: usize, const R: usize, const N: usize>(
        &self,
        peer: TimePeer,
        security: &SecurityOwner<'_, S, R>,
        link: &TimedLink<'_, N>,
    ) -> Result<()> {
        let path = self.config.path;
        let (id, instance, generation) = link.identity();
        if self.gate.is_active() {
            return Err(Error::State);
        }
        let view = security.session_get(peer.session, peer.facts)?;
        if self.runtime != security.runtime_instance()
            || !core::ptr::eq(self.gate, security.callback_gate())
            || instance != self.link_instance
            || !link.uses_gate(security.callback_gate())
            || peer.facts.now_us == 0
            || peer.facts.now_us < self.last_now
            || peer.session != self.peer.session
            || peer.fingerprint != self.peer.fingerprint
            || peer.facts.local != self.peer.facts.local
            || peer.facts.peer != self.peer.facts.peer
            || peer.facts.policy_generation != self.peer.facts.policy_generation
            || peer.facts.link_generation != self.peer.facts.link_generation
            || id != path.forward_link
            || id != path.reverse_link
            || generation != path.forward_generation
            || generation != path.reverse_generation
            || generation != peer.facts.link_generation.get()
            || view.session_generation.get() != path.session
            || view.hop_profile != HopProfile::H0
            || view.origin_security == OriginSecurity::O0
            || view.origin_security != self.origin
            || (
                security.outbound_key_generation(peer.session, peer.facts)?,
                security.inbound_key_generation(peer.session, peer.facts)?,
            ) != self.key_generations
        {
            return Err(Error::Access);
        }
        Ok(())
    }
    fn proof<const W: usize>(&self, proof: &TimeDomainProof<'_, W>) -> Result<()> {
        if proof.config != self.config
            || proof.runtime != self.runtime
            || proof.binding != self.binding
        {
            return Err(Error::State);
        }
        Ok(())
    }
    fn access(
        &self,
        peer: TimePeer,
        op: ProtocolOpcode,
        direction: AccessDirection,
    ) -> AccessRequest {
        AccessRequest {
            session: peer.session,
            context_fingerprint: peer.fingerprint,
            service: self.service,
            protocol_opcode: u16::from(op),
            direction,
        }
    }
    fn prepare(&mut self, op: ProtocolOpcode, body: &[u8]) -> Result<()> {
        if self.outbound.is_some() || body.len() > 24 {
            return Err(Error::State);
        }
        let mut exact = [0; 24];
        exact[..body.len()].copy_from_slice(body);
        self.outbound = Some(Outbound {
            opcode: op,
            body: exact,
            bytes: body.len(),
            sealed: 0,
            holder: None,
            submitted_at: None,
        });
        Ok(())
    }
    /// Begin only using a linear sequence permit and current durable generation proof.
    /// 只用线性序号许可与当前耐久代际证明启动，不支持重置旧序号。
    /// # Errors
    /// Busy/expired/current-domain mismatch rejects; consumed permits never return.
    pub fn begin_master<const W: usize, const S: usize, const R: usize, const N: usize>(
        &mut self,
        proof: &TimeDomainProof<'_, W>,
        permit: SyncSequencePermit,
        peer: TimePeer,
        security: &SecurityOwner<'_, S, R>,
        link: &TimedLink<'_, N>,
    ) -> Result<()> {
        self.proof(proof)?;
        self.current(peer, security, link)?;
        if self.stopped
            || self.role != TimeRuntimeRole::Master
            || !self.quiescent()
            || self.outcome.is_some()
        {
            return Err(Error::State);
        }
        security.authorize(
            self.access(peer, ProtocolOpcode::TimeSync, AccessDirection::Outbound),
            peer.facts,
        )?;
        let deadline = peer
            .facts
            .now_us
            .checked_add(self.lifetime)
            .ok_or(Error::Exhausted)?;
        let key = TimeKey {
            domain: self.config.domain,
            generation: self.config.generation,
            sequence: permit.consume(self.config)?,
        };
        let mut body = [0; 12];
        encode_time_key(key, &mut body)?;
        self.prepare(ProtocolOpcode::TimeSync, &body)?;
        self.deadline = deadline;
        self.last_now = peer.facts.now_us;
        self.key = Some(key);
        self.follow_up_sent = false;
        Ok(())
    }
    fn clock(&mut self, now: u64) -> Result<()> {
        if self.gate.is_active() {
            return Err(Error::State);
        }
        if now == 0 || now < self.last_now {
            return Err(Error::State);
        }
        self.last_now = now;
        if self.key.is_some() && now >= self.deadline {
            self.abort()?;
            return Err(Error::Timeout);
        }
        Ok(())
    }
    fn abort(&mut self) -> Result<()> {
        if let Some(h) = self.txn {
            self.sync.cancel(h)?;
            self.txn = None;
        }
        for holder in self.holders.iter_mut().flatten() {
            holder.ready = true;
        }
        self.outbound = None;
        self.key = None;
        Ok(())
    }
    fn holder_slot(&self) -> Result<usize> {
        self.holders
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)
    }
    /// Advance at most one actual TX/completion edge; best-effort loss drops this sample.
    /// 最多推进一个实际TX/完成边；尽力同步丢失只放弃本次样本，不伪造成功。
    /// # Errors
    /// ACL/clock/MTU, unknown physical state or callback reentry rejects with holders retained.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn drive<const W: usize, const S: usize, const R: usize, const N: usize, const P: usize>(
        &mut self,
        proof: &TimeDomainProof<'_, W>,
        peer: TimePeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        scratch: &mut SecurityPacketWorkspace<P>,
        link: &mut TimedLink<'_, N>,
        driver: &mut impl TimedDriver<N>,
        frame_mtu: usize,
    ) -> Result<bool> {
        self.proof(proof)?;
        self.current(peer, security, link)?;
        self.clock(peer.facts.now_us)?;
        if self.stopped {
            return Err(Error::State);
        }
        let Some(out) = self.outbound.as_ref() else {
            return Ok(false);
        };
        security.authorize(
            self.access(peer, out.opcode, AccessDirection::Outbound),
            peer.facts,
        )?;
        if let Some(index) = out.holder.filter(|_| out.sealed != 0) {
            let holder = self.holders[index].ok_or(Error::State)?;
            if link.event_state(holder.key)? == TimedEventState::RetireOnly {
                self.abort()?;
                return Err(Error::InDoubt);
            }
            let stamp = match link.timestamp(holder.key) {
                Ok(stamp) => stamp,
                Err(Error::State) => return Ok(false),
                Err(e) => return Err(e),
            };
            let op = out.opcode;
            if stamp.time_us() > peer.facts.now_us
                || out.submitted_at.is_none_or(|start| stamp.time_us() < start)
            {
                self.abort()?;
                return Err(Error::State);
            }
            if matches!(
                op,
                ProtocolOpcode::TimeSync | ProtocolOpcode::TimeDelayRequest
            ) {
                self.sync
                    .tx_completed(self.txn.ok_or(Error::State)?, stamp, peer.facts.now_us)?;
            } else {
                self.holders[index].as_mut().ok_or(Error::State)?.ready = true;
            }
            self.outbound = None;
            if op == ProtocolOpcode::TimeSync {
                let mut body = [0; 24];
                self.sync.follow_up_payload(
                    self.txn.ok_or(Error::State)?,
                    peer.facts.now_us,
                    &mut body,
                )?;
                self.prepare(ProtocolOpcode::TimeFollowUp, &body)?;
            } else if op == ProtocolOpcode::TimeFollowUp {
                self.follow_up_sent = true;
            } else if op == ProtocolOpcode::TimeDelayResponse {
                self.key = None;
            }
            return Ok(true);
        }
        let existing = out.holder;
        let index = if let Some(index) = existing {
            index
        } else {
            self.holder_slot()?
        };
        let op = out.opcode;
        let body_bytes = out.bytes;
        let width = security.address_width().bytes();
        let prefix_bytes = 25 + 2 * width;
        if prefix_bytes + body_bytes + 16 > frame_mtu.min(B) {
            return Err(Error::NoSpace);
        }
        if out.sealed == 0 {
            let transaction = counter.preview()?;
            let mut prefix = [0; 33];
            CommonHeader {
                contract: HeaderContract::C0,
                origin_security: security
                    .session_get(peer.session, peer.facts)?
                    .origin_security,
                delivery: DeliveryGuarantee::BestEffort,
                interaction: match op {
                    ProtocolOpcode::TimeDelayRequest => InteractionRole::Request,
                    ProtocolOpcode::TimeDelayResponse => InteractionRole::Result,
                    _ => InteractionRole::OneWay,
                },
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
            prefix[15 + 2 * width..23 + 2 * width]
                .copy_from_slice(&transaction.get().to_be_bytes());
            prefix[23 + 2 * width..prefix_bytes].copy_from_slice(&u16::from(op).to_be_bytes());
            let bytes = security.protect_packet(
                crypto,
                OriginCounterOwner::Transaction(counter),
                peer.session,
                peer.facts,
                self.access(peer, op, AccessDirection::Outbound),
                PacketPlan {
                    prefix: &prefix[..prefix_bytes],
                    payload: &out.body[..body_bytes],
                    canonical_identity: &prefix[3..prefix_bytes],
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
                &mut self.frame,
            )?;
            self.outbound.as_mut().ok_or(Error::State)?.sealed = bytes;
        }
        let key = if existing.is_some() {
            self.holders[index].ok_or(Error::State)?.key
        } else {
            link.reserve(EventDirection::Tx)?
        };
        let is_sync = matches!(
            op,
            ProtocolOpcode::TimeSync | ProtocolOpcode::TimeDelayRequest
        );
        if existing.is_none() {
            self.holders[index] = Some(Holder {
                key,
                claim: None,
                proof: None,
                sync: is_sync,
                ready: false,
            });
        }
        if op == ProtocolOpcode::TimeSync {
            let result = self.sync.master_begin(
                self.config,
                peer.facts.peer.principal,
                self.key.ok_or(Error::State)?.sequence,
                key,
                peer.facts.now_us,
                self.deadline - peer.facts.now_us,
            );
            match result {
                Ok(h) => self.txn = Some(h),
                Err(e) => {
                    self.holders[index].as_mut().ok_or(Error::State)?.sync = false;
                    self.abort()?;
                    return Err(e);
                }
            }
        }
        self.outbound.as_mut().ok_or(Error::State)?.holder = Some(index);
        let bytes = self.outbound.as_ref().ok_or(Error::State)?.sealed;
        self.outbound.as_mut().ok_or(Error::State)?.submitted_at = Some(peer.facts.now_us);
        let result = link.submit(key, &self.frame[..bytes], driver);
        let state = link.event_state(key)?;
        if result.is_err()
            || matches!(
                state,
                TimedEventState::Reserved | TimedEventState::RetireOnly
            )
        {
            self.abort()?;
            return Err(result
                .err()
                .unwrap_or(if state == TimedEventState::Reserved {
                    Error::State
                } else {
                    Error::InDoubt
                }));
        }
        Ok(true)
    }
    /// Claim and authenticate the same atomic complete-frame/stamp record internally.
    /// 内部领取并认证同一个完整帧/打点记录，不接受外部opened或任意T2/T4。
    /// # Errors
    /// Wrong role/key/source/path, ACL, replay or missing actual completion rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn receive<
        const W: usize,
        const S: usize,
        const R: usize,
        const N: usize,
        const Q: usize,
        const P: usize,
    >(
        &mut self,
        proof: &TimeDomainProof<'_, W>,
        peer: TimePeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        scratch: &mut SecurityPacketWorkspace<P>,
        link: &mut TimedLink<'_, N>,
        queue: &TimedRxQueue<Q, B>,
    ) -> Result<()> {
        self.proof(proof)?;
        self.current(peer, security, link)?;
        self.clock(peer.facts.now_us)?;
        if self.stopped || queue.identity() != self.queue_id {
            return Err(Error::State);
        }
        let index = self.holder_slot()?;
        let claim = queue.claim(&mut self.raw)?;
        self.holders[index] = Some(Holder {
            key: claim.stamp().key(),
            claim: Some(claim),
            proof: None,
            sync: false,
            ready: true,
        });
        self.receive_inner(peer, security, crypto, scratch, link, index, claim)
    }
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    fn receive_inner<const S: usize, const R: usize, const N: usize, const P: usize>(
        &mut self,
        peer: TimePeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        scratch: &mut SecurityPacketWorkspace<P>,
        link: &mut TimedLink<'_, N>,
        index: usize,
        claim: TimedRxClaim,
    ) -> Result<()> {
        if link.timestamp(claim.stamp().key())? != claim.stamp()
            || claim.stamp().time_us() > peer.facts.now_us
        {
            return Err(Error::State);
        }
        let packet = &self.raw[..claim.bytes()];
        let n = 25 + 2 * security.address_width().bytes();
        if packet.len() < n + 16 {
            return Err(Error::Malformed);
        }
        let op = ProtocolOpcode::try_from(u16::from_be_bytes([packet[n - 2], packet[n - 1]]))?;
        if !matches!(
            op,
            ProtocolOpcode::TimeSync
                | ProtocolOpcode::TimeFollowUp
                | ProtocolOpcode::TimeDelayRequest
                | ProtocolOpcode::TimeDelayResponse
        ) {
            return Err(Error::Unsupported);
        }
        let tx = C0TransactionId::new(u64::from_be_bytes(
            packet[n - 10..n - 2]
                .try_into()
                .map_err(|_| Error::Malformed)?,
        ))?;
        let access = self.access(peer, op, AccessDirection::Inbound);
        let handle = self.control.receive(
            security,
            crypto,
            peer.session,
            peer.facts,
            access,
            OpenPacketPlan {
                packet,
                prefix_bytes: n,
                canonical_identity: &packet[3..n],
                origin_context: OriginContext::C0 {
                    fingerprint: peer.fingerprint,
                    transaction_id: tx,
                    opcode: op,
                    direction: if peer.facts.peer.principal < peer.facts.local.principal {
                        SenderDirection::AscendingPrincipal
                    } else {
                        SenderDirection::DescendingPrincipal
                    },
                },
                hop_profile: HopProfile::H0,
            },
            scratch,
        )?;
        let command = self.control.view(handle)?;
        let result = self.dispatch(command.body(), peer, link, index, claim);
        self.control.ack(handle)?;
        result
    }
    #[allow(clippy::too_many_lines)] // Four exact independent role transitions, no provider/driver callbacks.
    fn dispatch<const N: usize>(
        &mut self,
        body: ControlBody,
        peer: TimePeer,
        link: &mut TimedLink<'_, N>,
        index: usize,
        claim: TimedRxClaim,
    ) -> Result<()> {
        let now = peer.facts.now_us;
        let remote = peer.facts.peer.principal;
        match body {
            ControlBody::TimeSync(key) => {
                if self.role != TimeRuntimeRole::Member
                    || self.txn.is_some()
                    || self.key.is_some()
                    || self.outbound.is_some()
                    || self.sync.release_peek().is_some()
                    || self.outcome.is_some()
                    || key.domain != self.config.domain
                    || key.generation != self.config.generation
                {
                    return Err(Error::State);
                }
                let deadline = now.checked_add(self.lifetime).ok_or(Error::Exhausted)?;
                let tx_index = self.holder_slot()?;
                let t3 = link.reserve(EventDirection::Tx)?;
                self.holders[tx_index] = Some(Holder {
                    key: t3,
                    claim: None,
                    proof: None,
                    sync: false,
                    ready: true,
                });
                let h = self.sync.member_begin(
                    self.config,
                    remote,
                    key,
                    claim.stamp(),
                    t3,
                    now,
                    self.lifetime,
                )?;
                self.holders[tx_index].as_mut().ok_or(Error::State)?.sync = true;
                self.holders[tx_index].as_mut().ok_or(Error::State)?.ready = false;
                self.holders[index].as_mut().ok_or(Error::State)?.sync = true;
                self.holders[index].as_mut().ok_or(Error::State)?.ready = false;
                self.txn = Some(h);
                self.key = Some(key);
                self.deadline = deadline;
                let mut bytes = [0; 12];
                encode_time_key(key, &mut bytes)?;
                self.prepare(ProtocolOpcode::TimeDelayRequest, &bytes)?;
                // The reserved T3 is used only for this exact internally-built Delay Request.
                self.outbound.as_mut().ok_or(Error::State)?.holder = Some(tx_index);
            }
            ControlBody::TimeFollowUp(message) => {
                if self.role != TimeRuntimeRole::Member || self.key != Some(message.key) {
                    return Err(Error::State);
                }
                let mut bytes = [0; 24];
                encode_time_stamp(message, &mut bytes)?;
                self.sync.accept_follow_up(
                    self.txn.ok_or(Error::State)?,
                    remote,
                    self.config.path,
                    &bytes,
                    now,
                )?;
            }
            ControlBody::TimeDelayRequest(key) => {
                if self.role != TimeRuntimeRole::Master
                    || self.key != Some(key)
                    || !self.follow_up_sent
                    || self.outbound.is_some()
                {
                    return Err(Error::State);
                }
                let h = self.txn.ok_or(Error::State)?;
                let response = self.sync.master_response(
                    h,
                    key,
                    remote,
                    self.config.path,
                    claim.stamp(),
                    now,
                )?;
                self.txn = None;
                self.holders[index].as_mut().ok_or(Error::State)?.sync = true;
                let mut bytes = [0; 24];
                encode_time_stamp(
                    TimeStampMessage {
                        key,
                        time: response.t4,
                        bound: response.t4_bound,
                    },
                    &mut bytes,
                )?;
                self.prepare(ProtocolOpcode::TimeDelayResponse, &bytes)?;
            }
            ControlBody::TimeDelayResponse(message) => {
                if self.role != TimeRuntimeRole::Member
                    || self.key != Some(message.key)
                    || self.outbound.is_some()
                {
                    return Err(Error::State);
                }
                let mut bytes = [0; 24];
                encode_time_stamp(message, &mut bytes)?;
                let result = self.sync.delay_response_payload(
                    self.txn.ok_or(Error::State)?,
                    remote,
                    self.config.path,
                    &bytes,
                    now,
                )?;
                self.txn = None;
                self.key = None;
                self.outcome = Some(result);
            }
            ControlBody::DeliveryAck(_) => return Err(Error::Unsupported),
            #[cfg(any(
                feature = "transfer",
                feature = "service-control",
                feature = "capability-control"
            ))]
            _ => return Err(Error::Unsupported),
        }
        Ok(())
    }
    /// Bounded original-deadline expiration, also before another frame arrives.
    /// 有界原截止到期处理，不依赖下一帧到达。
    /// # Errors
    /// Time rollback rejects; expiry cancels the sample but preserves all physical holders.
    pub fn step(&mut self, now: u64) -> Result<()> {
        self.clock(now)
    }
    /// Stop is irreversible for this instance; Driver reentry cannot stop another Root.
    /// 本实例不可逆停止；Driver重入不得停止其他Root。
    /// # Errors
    /// Busy shared callback gate rejects before state changes.
    pub fn stop(&mut self) -> Result<()> {
        if self.gate.is_active() {
            return Err(Error::State);
        }
        self.abort()?;
        self.stopped = true;
        self.control.stop();
        Ok(())
    }
    /// Peek -> physical retirement -> retained receipt -> Queue/Sync ACK, never destructive pop.
    /// 查看、真实退休、保留凭据、Queue/Sync确认，失败义务不得提前出队。
    /// # Errors
    /// Driver/Queue failure keeps the exact key and successful receipt for retry.
    pub fn retire<const N: usize, const Q: usize>(
        &mut self,
        link: &mut TimedLink<'_, N>,
        driver: &mut impl TimedDriver<N>,
        queue: &TimedRxQueue<Q, B>,
        budget: usize,
    ) -> Result<usize> {
        if self.gate.is_active() {
            return Err(Error::State);
        }
        if queue.identity() != self.queue_id
            || budget == 0
            || !link.uses_gate(self.gate)
            || link.identity().0 != self.config.path.forward_link
            || link.identity().1 != self.link_instance
        {
            return Err(Error::Argument);
        }
        let mut used = 0;
        for _ in 0..budget.min(6) {
            let index = self.retire_cursor;
            self.retire_cursor = (index + 1) % 6;
            let Some(mut holder) = self.holders[index] else {
                continue;
            };
            if holder.sync {
                if self.sync.release_peek() != Some(holder.key) {
                    continue;
                }
            } else if !holder.ready {
                continue;
            }
            used += 1;
            if holder.proof.is_none() {
                let proof = link.retire_receipt(holder.key, driver)?;
                holder.proof = Some(proof);
                self.holders[index] = Some(holder);
            }
            let proof = holder.proof.ok_or(Error::State)?;
            if let Some(claim) = holder.claim {
                queue.release(claim, proof)?;
                holder.claim = None;
                self.holders[index] = Some(holder);
            }
            if holder.sync {
                self.sync.release_ack(proof)?;
            }
            self.holders[index] = None;
        }
        Ok(used)
    }
    /// Move the non-forgeable sample into the real Domain; 取出不可伪造样本供实际Domain消费。
    /// # Errors
    /// Driver/Provider reentry rejects before consuming the fact.
    pub fn take_outcome(&mut self) -> Result<Option<SyncOutcome>> {
        if self.gate.is_active() {
            return Err(Error::State);
        }
        Ok(self.outcome.take())
    }
    /// This component's local obligations only, not whole-node/remote quiescence.
    /// 仅本组件本地义务，不代表全节点或远端静默。
    #[must_use]
    pub fn quiescent(&self) -> bool {
        self.key.is_none()
            && self.txn.is_none()
            && self.outbound.is_none()
            && self.holders.iter().all(Option::is_none)
            && self.sync.release_peek().is_none()
    }
}
