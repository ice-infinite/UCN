//! Static durable client: actual ID reload, Data result, terminal Resume/ACK and exact retirement.
//! 静态耐久客户端：实际ID回读、Data结果、终态Resume/ACK与精确退休。
use crate::runtime_common::{CLASSES, GatedDriver};
use crate::{
    ControlAdmission, ControlBody, ControlCoordinator, ControlReplies, JournalPeer,
    JournalRuntimeStats, JournalStepBudget, JournalStepReport, JournalWorkClass, OperationRecovery,
    ReplyPhase, RpcBridge, RpcEndpoint, RpcHandle,
};
use ucn_adapter::{AdapterOwner, LinkHandle, RxToken, TxDriver, TxState, TxToken};
use ucn_digest::DigestWorkspace;
use ucn_durable::DurableBase;
use ucn_owner::CallbackClaim;
use ucn_persistence::{
    CodecWorkspace, PersistenceHandle, PersistenceOwner, PersistenceProvider, RequestState,
};
use ucn_security::{
    AccessDirection, AccessRequest, C0TransactionOwner, CryptoProvider, OpenPacketPlan,
    OriginContext, OriginCounterOwner, OriginSequenceOwner, PacketPlan, SecurityOwner,
    SecurityPacketWorkspace, SenderDirection,
};
use ucn_service::{
    OperationIds, OperationKey, OperationWireKey, RequestPhase, ServiceOwner, encode_operation_key,
};
use ucn_types::{
    C0TransactionId, DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile,
    InteractionRole, OriginSecurity, OriginSequence, PayloadKind, ProtocolOpcode, Result,
    ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;

/// Product timing/interval policy, not a remote lease; 产品计时/区间策略，非远端租约。
#[derive(Clone, Copy)]
pub struct SourceRuntimeConfig {
    /// Unique Coordinator ID with four derived IDs; `唯一Coordinator及四个派生ID`。
    pub owner: u32,
    /// ID interval size, reserved before use; 先确认后使用的ID区间大小。
    pub id_interval: u64,
    /// Provider work lifetime; Provider工作时限。
    pub io_timeout_us: u64,
    /// Separate terminal query/ACK lifetime; 独立终态查询/ACK生命周期。
    pub recovery_lifetime_us: u64,
    /// Immutable-byte retry spacing; 不变字节重试间隔。
    pub retry_us: u64,
    /// Maximum physical submit calls; 最大物理提交次数。
    pub max_attempts: u16,
}
/// Local observation, never a remote execution permission; 本地观察，绝非远端执行权限。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SourcePhase {
    /// No request; 无请求。
    Idle,
    /// Arguments retained, ID/crypto still pending; 已保留参数，ID/保护未完成。
    Queued,
    /// Original Data waiting; 原Data待结果。
    WaitResult,
    /// Independent terminal retrieval; 独立终态检索。
    Recovering,
    /// Exact authenticated terminal observation retained; 精确认证终态观察已保留。
    Observed,
    /// No provable result, never safe to reexecute automatically; 无可证明结果，不可自动重执行。
    UnknownOutcome,
    /// Local stop, not remote cancellation; 本地停止，不等于远端取消。
    Stopped,
}
/// Durable observation status, copied only after authentication and ACK reservation.
/// 耐久观察状态，认证并预留ACK后才复制。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SourceResult {
    /// OPS1 terminal phase 3/4/5; OPS1终态3/4/5。
    pub phase: u8,
    /// Original signed business status; 原有符号业务状态。
    pub status: i32,
    /// Retained exact result bytes; 保留的精确结果长度。
    pub bytes: usize,
}
struct Resume {
    peer: JournalPeer,
    key_generation: u32,
    length: usize,
    attempts: u16,
    next_try: u64,
    token: Option<TxToken>,
}
/// One direct durable client and a dedicated single-Link Adapter; no heap or fabricated proof.
/// 单个直达耐久客户端、专属单Link适配器，无堆或伪造证明。
pub struct SourceRuntime<const FRAME: usize, const BUSINESS: usize> {
    config: SourceRuntimeConfig,
    service_id: ServiceId,
    expected_peer: [u8; 16],
    minimum: OriginSecurity,
    service: ServiceOwner<1, 1, BUSINESS>,
    rpc: RpcBridge<1, FRAME>,
    control: ControlCoordinator<2, FRAME>,
    replies: ControlReplies<2, FRAME>,
    ids: OperationIds,
    id_io: Option<PersistenceHandle>,
    id_confirmed: bool,
    continuation: u32,
    rpc_handle: Option<RpcHandle>,
    key: Option<OperationKey>,
    recovery: Option<OperationRecovery>,
    recovery_deadline: u64,
    resume: Option<Resume>,
    resume_bytes: [u8; FRAME],
    result: Option<SourceResult>,
    result_bytes: [u8; BUSINESS],
    arguments: [u8; BUSINESS],
    argument_bytes: usize,
    deadline: u64,
    phase: SourcePhase,
    raw: [u8; FRAME],
    rx: Option<RxToken>,
    crypto_workspace: SecurityPacketWorkspace<FRAME>,
    codec: CodecWorkspace,
    digest: DigestWorkspace,
    cursor: usize,
    tx_cursor: usize,
    retire_cursor: usize,
    reply_cursor: usize,
    timer_cursor: bool,
    last_now: u64,
    stats: JournalRuntimeStats,
    stopped: bool,
}
impl<const F: usize, const B: usize> SourceRuntime<F, B> {
    /// Assemble a client; restored ID ranges are burned, new ranges require actual Foundation.
    /// 装配客户端，恢复ID区间烧掉，新区间必须走真实Foundation。
    /// # Errors
    /// Invalid policy/capacities or pending ID update rejects without I/O.
    pub fn new(
        config: SourceRuntimeConfig,
        endpoint: RpcEndpoint,
        ids: OperationIds,
    ) -> Result<Self> {
        if config.owner == 0
            || config.owner > u32::MAX - 4
            || config.id_interval == 0
            || config.max_attempts == 0
            || !(96..=4096).contains(&F)
            || B == 0
            || B > 146
            || B + 103 > F
            || ids.is_pending()
            || ids.is_fenced()
        {
            return Err(Error::Config);
        }
        for duration in [
            config.io_timeout_us,
            config.recovery_lifetime_us,
            config.retry_us,
        ] {
            if duration == 0 || duration > i64::MAX as u64 {
                return Err(Error::Config);
            }
        }
        let (service_id, expected_peer, minimum) = endpoint.source_policy(B)?;
        Ok(Self {
            service_id,
            expected_peer,
            minimum,
            rpc: RpcBridge::new(
                config.owner + 1,
                endpoint,
                config.retry_us,
                config.max_attempts,
            )?,
            service: ServiceOwner::new(config.owner + 2, config.recovery_lifetime_us)?,
            control: ControlCoordinator::new(config.owner + 3)?,
            replies: ControlReplies::new(config.owner + 4, config.retry_us, config.max_attempts)?,
            config,
            ids,
            id_io: None,
            id_confirmed: false,
            continuation: 0,
            rpc_handle: None,
            key: None,
            recovery: None,
            recovery_deadline: 0,
            resume: None,
            resume_bytes: [0; F],
            result: None,
            result_bytes: [0; B],
            arguments: [0; B],
            argument_bytes: 0,
            deadline: 0,
            phase: SourcePhase::Idle,
            raw: [0; F],
            rx: None,
            crypto_workspace: SecurityPacketWorkspace::new(),
            codec: CodecWorkspace::new(),
            digest: DigestWorkspace::new(),
            cursor: 0,
            tx_cursor: 0,
            retire_cursor: 0,
            reply_cursor: 0,
            timer_cursor: false,
            last_now: 0,
            stats: JournalRuntimeStats::default(),
            stopped: false,
        })
    }
    /// Queue arguments only, no ID/nonce/Driver side effect; 只排队参数，无ID/nonce/Driver副作用。
    /// # Errors
    /// Existing work, bad clock/lifetime/capacity rejects atomically.
    pub fn request(&mut self, business: &[u8], now: u64, deadline: u64) -> Result<()> {
        if self.stopped
            || self.phase != SourcePhase::Idle
            || now == 0
            || now < self.last_now
            || deadline <= now
            || deadline - now > i64::MAX as u64
        {
            return Err(Error::State);
        }
        if business.len() > B {
            return Err(Error::NoSpace);
        }
        self.arguments[..business.len()].copy_from_slice(business);
        self.argument_bytes = business.len();
        self.deadline = deadline;
        self.last_now = now;
        self.phase = SourcePhase::Queued;
        Ok(())
    }
    /// Recover only the supplied original correlation, never a new Operation/execution.
    /// 只恢复原关联，不新建Operation或执行。
    /// # Errors
    /// Active work/invalid identity/lifetime rejects without publishing a query.
    pub fn recover(&mut self, key: OperationKey, now: u64, deadline: u64) -> Result<()> {
        if self.stopped
            || self.phase != SourcePhase::Idle
            || now < self.last_now
            || key.service.service != self.service_id.get()
            || key.service.opcode != 0
            || key.service.server != self.expected_peer
        {
            return Err(Error::State);
        }
        if deadline <= now || deadline - now > i64::MAX as u64 {
            return Err(Error::Argument);
        }
        let recovery = OperationRecovery::new(key, now, deadline)?;
        self.key = Some(key);
        self.recovery = Some(recovery);
        self.recovery_deadline = deadline;
        self.last_now = now;
        self.phase = SourcePhase::Recovering;
        Ok(())
    }
    /// Exact original key, useful for product-owned crash recovery; 精确原键供产品保存/恢复。
    #[must_use]
    pub const fn operation_key(&self) -> Option<OperationKey> {
        self.key
    }
    /// Logical phase, not physical quiescence; 逻辑阶段，非物理静止。
    #[must_use]
    pub const fn phase(&self) -> SourcePhase {
        self.phase
    }
    /// Fixed diagnostics; 固定诊断。
    #[must_use]
    pub const fn stats(&self) -> JournalRuntimeStats {
        self.stats
    }
    /// Copy retained authenticated observation; 复制保留的认证观察。
    /// # Errors
    /// No terminal observation/short output leaves output unchanged.
    pub fn copy_result(&self, output: &mut [u8]) -> Result<SourceResult> {
        let result = self.result.ok_or(Error::State)?;
        if output.len() < result.bytes {
            return Err(Error::NoSpace);
        }
        output[..result.bytes].copy_from_slice(&self.result_bytes[..result.bytes]);
        Ok(result)
    }
    /// Stop local work without declaring remote `NoEffect` or erasing a Holder.
    /// 本地停止，不宣称远端NoEffect、不抹Holder。
    pub fn stop(&mut self) {
        self.stopped = true;
        self.phase = SourcePhase::Stopped;
        self.rpc.stop();
        self.control.stop();
        self.replies.stop();
        self.arguments.fill(0);
    }
    #[cfg(feature = "duplex-runtime")]
    pub(crate) fn duplex_binding(&self) -> Result<(u32, u32, ucn_persistence::DomainKey)> {
        if self.stopped
            || self.rx.is_some()
            || self.id_io.is_some()
            || self.ids.is_pending()
            || self.rpc_handle.is_some()
            || self.resume.is_some()
            || !self.control.is_empty()
            || (0..2).any(|i| self.replies.handle_at(i).is_some())
        {
            return Err(Error::State);
        }
        Ok((
            self.config.owner,
            self.config.owner + 4,
            self.ids.contract().domain,
        ))
    }
    #[cfg(feature = "duplex-runtime")]
    pub(crate) fn receive_shared<const S: usize, const Q: usize>(
        &mut self,
        raw: &[u8],
        link: LinkHandle,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
    ) -> Result<()> {
        if raw.len() > F || self.rx.is_some() {
            return Err(Error::NoSpace);
        }
        self.raw[..raw.len()].copy_from_slice(raw);
        self.stats.rx = self.stats.rx.saturating_add(1);
        let result = self.route_rx(raw.len(), link, peer, security, crypto);
        if result.is_err() {
            self.stats.rejected = self.stats.rejected.saturating_add(1);
        }
        result
    }
    pub(crate) fn quiescent(&self) -> bool {
        self.stopped
            && self.rx.is_none()
            && self.id_io.is_none()
            && !self.ids.is_pending()
            && self.rpc_handle.is_none()
            && self.resume.is_none()
            && self.control.is_empty()
            && (0..2).all(|i| self.replies.handle_at(i).is_none())
    }
    pub(crate) fn environment<
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
    >(
        &self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, Q>,
        p: &PersistenceOwner<'_, D, P, L>,
    ) -> Result<()> {
        let c = self.ids.contract();
        p.validate_consumer(c.runtime, c.persistence_owner, c.domain, c.owner)?;
        if security.runtime_instance() != c.runtime {
            return Err(Error::Config);
        }
        let v = p.domain_get(c.domain)?;
        if v.schema_id != c.schema || v.schema_version != c.version {
            return Err(Error::Config);
        }
        if self.id_io.is_none() && (v.record_generation, v.body_digest) != self.ids.record_binding()
        {
            return Err(Error::State);
        }
        if peer.facts.now_us == 0
            || peer.facts.now_us < self.last_now
            || security.callback_gate().is_active()
        {
            return Err(Error::State);
        }
        Ok(())
    }
    /// Seven hard-capped rotating classes, actual Provider/Driver and counters supplied by product.
    /// 七类硬预算轮转，产品提供实际Provider/Driver和唯一计数器。
    /// # Errors
    /// Invalid environment rejects before progress; class errors are returned in report.
    #[allow(clippy::too_many_arguments)]
    pub fn step<
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        budget: JournalStepBudget,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
        provider: &mut impl PersistenceProvider,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
        sequence: &mut impl OriginSequenceOwner,
        transaction: &mut impl C0TransactionOwner,
    ) -> Result<JournalStepReport> {
        self.environment(peer, security, persistence)?;
        self.last_now = peer.facts.now_us;
        let gate = security.callback_gate();
        let mut report = JournalStepReport {
            visits: [0; 7],
            progress: 0,
            error: None,
            quiescent: false,
        };
        for _ in 0..budget.total {
            for _ in 0..7 {
                if report.visits[self.cursor] < budget.caps[self.cursor] {
                    break;
                }
                self.cursor = (self.cursor + 1) % 7;
            }
            let i = self.cursor;
            self.cursor = (i + 1) % 7;
            report.visits[i] += 1;
            self.stats.visits[i] = self.stats.visits[i].saturating_add(1);
            let advance = match CLASSES[i] {
                JournalWorkClass::Ingress => self.ingress(peer, security, crypto, adapter),
                JournalWorkClass::Foundation => {
                    match gate.try_enter(CallbackClaim::new(self.config.owner, 1, 1, 0x0714)?) {
                        Ok(lease) => {
                            let r = persistence
                                .step(provider, peer.facts.now_us, 1)
                                .map(|r| r.made_progress);
                            gate.leave(lease)?;
                            r
                        }
                        Err(e) => Err(e),
                    }
                }
                JournalWorkClass::Control => self.control_step(peer, security),
                JournalWorkClass::Request => self.request_step(
                    peer,
                    security,
                    crypto,
                    persistence,
                    adapter,
                    sequence,
                    transaction,
                ),
                JournalWorkClass::Timer => {
                    self.timer_cursor = !self.timer_cursor;
                    if self.timer_cursor {
                        self.service.step(peer.facts.now_us, 1).map(|_| false)
                    } else {
                        Ok(security
                            .maintain_replay_reservations(peer.facts.now_us, 1)
                            .expired
                            > 0)
                    }
                }
                JournalWorkClass::Transmit => self.transmit(
                    peer,
                    security,
                    crypto,
                    adapter,
                    &mut GatedDriver {
                        driver,
                        gate,
                        owner: self.config.owner,
                    },
                    transaction,
                ),
                JournalWorkClass::Retire => self.retire(
                    peer.facts.now_us,
                    persistence,
                    adapter,
                    &mut GatedDriver {
                        driver,
                        gate,
                        owner: self.config.owner,
                    },
                ),
            };
            match advance {
                Ok(true) => report.progress += 1,
                Ok(false) => {}
                Err(e) => {
                    if report.error.is_none() {
                        report.error = Some(e);
                    }
                }
            }
        }
        report.quiescent = self.quiescent() && adapter.rx_is_quiescent();
        Ok(report)
    }
    fn access(&self, peer: JournalPeer, opcode: u16, direction: AccessDirection) -> AccessRequest {
        AccessRequest {
            session: peer.session,
            context_fingerprint: peer.fingerprint,
            service: self.service_id,
            protocol_opcode: opcode,
            direction,
        }
    }
    fn ingress<const S: usize, const Q: usize, const T: usize, const RX: usize>(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
    ) -> Result<bool> {
        if self.stopped {
            adapter.try_set_rx_enabled(false)?;
        }
        if self.rx.is_some() {
            return Ok(false);
        }
        let item = match adapter.rx_claim(&mut self.raw) {
            Ok(v) => v,
            Err(Error::NotFound) => return Ok(false),
            Err(e) => return Err(e),
        };
        self.rx = Some(item.token);
        self.stats.rx = self.stats.rx.saturating_add(1);
        let r = if self.stopped {
            Ok(())
        } else {
            self.route_rx(item.frame_bytes, item.link, peer, security, crypto)
        };
        if r.is_err() {
            self.stats.rejected = self.stats.rejected.saturating_add(1);
        }
        adapter.rx_retire(item.token)?;
        self.rx = None;
        self.stats.retired = self.stats.retired.saturating_add(1);
        r.map(|()| true)
    }
    #[allow(clippy::too_many_lines)] // Two explicit authenticated raw contracts, no arbitrary callback dispatch.
    fn route_rx<const S: usize, const Q: usize>(
        &mut self,
        n: usize,
        link: LinkHandle,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
    ) -> Result<()> {
        if link != peer.link {
            return Err(Error::Access);
        }
        let raw = &self.raw[..n];
        let header = CommonHeader::decode(raw)?;
        let w = security.address_width().bytes();
        if header.hop_limit.get() != 1 || (header.origin_security as u8) < self.minimum as u8 {
            return Err(Error::Unsupported);
        }
        match (header.contract, header.payload_kind) {
            (HeaderContract::C1, PayloadKind::Data) => {
                let h = self.rpc_handle.ok_or(Error::State)?;
                if self.phase != SourcePhase::WaitResult {
                    return Err(Error::State);
                }
                let prefix = 9 + 2 * w;
                if n < prefix + 16 {
                    return Err(Error::Malformed);
                }
                let service = u16::from_be_bytes(
                    raw[prefix - 6..prefix - 4]
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                );
                if service != self.service_id.get() {
                    return Err(Error::Access);
                }
                let seq = OriginSequence::new(u32::from_be_bytes(
                    raw[prefix - 4..prefix]
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                ))?;
                let mut identity = [0; 46];
                identity[..16].copy_from_slice(&peer.facts.peer.principal);
                identity[16..20].copy_from_slice(&peer.facts.peer.generation.get().to_be_bytes());
                identity[20..36].copy_from_slice(&peer.facts.local.principal);
                identity[36..40].copy_from_slice(&peer.facts.local.generation.get().to_be_bytes());
                identity[40..42].copy_from_slice(&service.to_be_bytes());
                identity[42..].copy_from_slice(&seq.get().to_be_bytes());
                self.rpc.receive_result(
                    h,
                    &mut self.service,
                    security,
                    crypto,
                    peer.session,
                    peer.facts,
                    OpenPacketPlan {
                        packet: raw,
                        prefix_bytes: prefix,
                        canonical_identity: &identity,
                        origin_context: OriginContext::Sequenced {
                            contract: HeaderContract::C1,
                            fingerprint: peer.fingerprint,
                            sequence: seq,
                        },
                        hop_profile: HopProfile::H0,
                    },
                    link,
                    &mut self.crypto_workspace,
                )?;
            }
            (HeaderContract::C0, PayloadKind::Control) => {
                if self.phase != SourcePhase::Recovering && self.phase != SourcePhase::Observed {
                    return Err(Error::State);
                }
                let prefix = 25 + 2 * w;
                if n < prefix + 16 {
                    return Err(Error::Malformed);
                }
                let op = ProtocolOpcode::try_from(u16::from_be_bytes(
                    raw[prefix - 2..prefix]
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                ))?;
                if op != ProtocolOpcode::OperationResult {
                    return Err(Error::Unsupported);
                }
                let plan = OpenPacketPlan {
                    packet: raw,
                    prefix_bytes: prefix,
                    canonical_identity: &raw[3..prefix],
                    origin_context: OriginContext::C0 {
                        fingerprint: peer.fingerprint,
                        transaction_id: C0TransactionId::new(u64::from_be_bytes(
                            raw[prefix - 10..prefix - 2]
                                .try_into()
                                .map_err(|_| Error::Malformed)?,
                        ))?,
                        opcode: op,
                        direction: if peer.facts.peer.principal < peer.facts.local.principal {
                            SenderDirection::AscendingPrincipal
                        } else {
                            SenderDirection::DescendingPrincipal
                        },
                    },
                    hop_profile: HopProfile::H0,
                };
                let access = self.access(peer, u16::from(op), AccessDirection::Inbound);
                match self.control.receive_cached(
                    security,
                    crypto,
                    peer.session,
                    peer.facts,
                    access,
                    plan,
                    &mut self.crypto_workspace,
                    &mut self.replies,
                )? {
                    ControlAdmission::Command(_)
                    | ControlAdmission::Pending(_)
                    | ControlAdmission::CachedReply(_) => {}
                }
            }
            _ => return Err(Error::Unsupported),
        }
        Ok(())
    }
    fn control_step<const S: usize, const Q: usize>(
        &mut self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, Q>,
    ) -> Result<bool> {
        let h = match self.control.peek_next() {
            Ok(h) => h,
            Err(Error::NotFound) => return Ok(false),
            Err(e) => return Err(e),
        };
        if self.stopped || peer.facts.now_us >= self.recovery_deadline {
            self.control.ack(h)?;
            return Ok(true);
        }
        if !matches!(
            self.control.view(h)?.body(),
            ControlBody::OperationResult(_)
        ) {
            self.control.ack(h)?;
            return Err(Error::Unsupported);
        }
        let recovered = self.control.dispatch_recovered_result(
            h,
            self.recovery.as_mut().ok_or(Error::State)?,
            security,
            peer.session,
            peer.facts,
            &mut self.replies,
            &mut self.result_bytes,
            &mut self.digest,
        );
        match recovered {
            Ok(r) => {
                if self.result.is_none() {
                    self.result = Some(SourceResult {
                        phase: r.phase,
                        status: r.status,
                        bytes: r.bytes,
                    });
                }
                self.phase = SourcePhase::Observed;
                Ok(true)
            }
            Err(Error::NoSpace) => Ok(false),
            Err(e) => {
                self.control.ack(h)?;
                self.stats.rejected = self.stats.rejected.saturating_add(1);
                Err(e)
            }
        }
    }
    #[allow(clippy::too_many_arguments)]
    fn request_step<
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        p: &mut PersistenceOwner<'_, D, P, L>,
        adapter: &AdapterOwner<'_, 1, T, RX, F>,
        sequence: &mut impl OriginSequenceOwner,
        transaction: &mut impl C0TransactionOwner,
    ) -> Result<bool> {
        let now = peer.facts.now_us;
        if self.id_io.is_some() || self.ids.is_pending() {
            return self.interval_step(now, p);
        }
        if self.stopped {
            return Ok(false);
        }
        match self.phase {
            SourcePhase::Queued => {
                if now >= self.deadline {
                    self.phase = SourcePhase::UnknownOutcome;
                    self.arguments.fill(0);
                    return Ok(true);
                }
                let view = security.session_get(peer.session, peer.facts)?;
                if peer.facts.peer.principal != self.expected_peer
                    || view.hop_profile != HopProfile::H0
                    || (view.origin_security as u8) < self.minimum as u8
                {
                    return Err(Error::Access);
                }
                security.authorize(self.access(peer, 0, AccessDirection::Outbound), peer.facts)?;
                if 95 + 2 * security.address_width().bytes() + B > adapter.link_mtu(peer.link)? {
                    return Err(Error::NoSpace);
                }
                if self.ids.reserve_id().is_err() {
                    let c = self.ids.contract();
                    let mut body = [0; 20];
                    let n = p.copy_body(c.domain, &mut body)?;
                    let base = DurableBase::from_view(
                        c,
                        p.domain_get(c.domain)?,
                        &body[..n],
                        &mut self.codec,
                    )?;
                    let continuation = self.continuation.checked_add(1).ok_or(Error::Exhausted)?;
                    self.ids.reserve(
                        base,
                        self.config.id_interval,
                        now,
                        now.checked_add(self.config.io_timeout_us)
                            .ok_or(Error::Exhausted)?,
                        continuation,
                        &mut self.codec,
                    )?;
                    self.continuation = continuation;
                    return Ok(true);
                }
                let h = self.rpc.begin_durable_request(
                    &mut self.service,
                    security,
                    crypto,
                    sequence,
                    &mut self.ids,
                    peer.session,
                    peer.facts,
                    peer.fingerprint,
                    peer.link,
                    adapter,
                    &self.arguments[..self.argument_bytes],
                    self.deadline,
                    &mut self.crypto_workspace,
                )?;
                self.key = Some(self.rpc.original_operation(h)?);
                self.rpc_handle = Some(h);
                self.arguments.fill(0);
                self.phase = SourcePhase::WaitResult;
                Ok(true)
            }
            SourcePhase::WaitResult => {
                let h = self.rpc_handle.ok_or(Error::State)?;
                if now >= self.deadline
                    || self.service.view(self.rpc.view(h)?.service)?.phase == RequestPhase::Complete
                {
                    self.recovery_deadline = now
                        .checked_add(self.config.recovery_lifetime_us)
                        .ok_or(Error::Exhausted)?;
                    self.recovery = Some(OperationRecovery::new(
                        self.key.ok_or(Error::State)?,
                        now,
                        self.recovery_deadline,
                    )?);
                    self.phase = SourcePhase::Recovering;
                    return Ok(true);
                }
                Ok(false)
            }
            SourcePhase::Recovering => {
                if now >= self.recovery_deadline {
                    self.phase = SourcePhase::UnknownOutcome;
                    return Ok(true);
                }
                if self.resume.is_none() {
                    self.seal_resume(peer, security, crypto, transaction, adapter)?;
                    return Ok(true);
                }
                Ok(false)
            }
            _ => Ok(false),
        }
    }
    fn interval_step<const D: usize, const P: usize, const L: usize>(
        &mut self,
        now: u64,
        p: &mut PersistenceOwner<'_, D, P, L>,
    ) -> Result<bool> {
        if let Some(io) = self.id_io {
            if p.request_get(io)?.state == RequestState::Failed {
                return Ok(false);
            }
            if !self.id_confirmed {
                let proof = match p.proof_get(io) {
                    Ok(v) => v,
                    Err(Error::State) => return Ok(false),
                    Err(e) => return Err(e),
                };
                self.ids.confirm(io, proof)?;
                self.id_confirmed = true;
                // Common confirmation tail is bounded and has no Provider callback.
                // Do not wait an extra Retire rotation just to release this in-memory proof.
                // 确认共同尾部有界且无Provider回调，不额外等待退休轮转来释放内存Proof。
                p.proof_retire(io)?;
                self.id_io = None;
                self.id_confirmed = false;
                return Ok(true);
            }
            Ok(false)
        } else {
            if self.stopped || now >= self.ids.requirement()?.absolute_deadline_us {
                self.ids.fence();
                return Ok(true);
            }
            match p.submit(&self.ids.requirement()?, now) {
                Ok(h) => {
                    self.ids.bind(h)?;
                    self.id_io = Some(h);
                    self.id_confirmed = false;
                    Ok(true)
                }
                Err(Error::NoSpace) => Ok(false),
                Err(e) => Err(e),
            }
        }
    }
    fn recovery_access<const S: usize, const Q: usize>(
        &self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, Q>,
    ) -> Result<()> {
        let k = self.key.ok_or(Error::State)?.service;
        let f = peer.facts;
        let view = security.session_get(peer.session, f)?;
        let key_gen = security.outbound_key_generation(peer.session, f)?;
        if (view.origin_security as u8) < self.minimum as u8
            || f.peer.principal != self.expected_peer
            || k.realm != security.realm().get()
            || k.client != f.local.principal
            || k.server != f.peer.principal
            || k.client_address != f.local.address.get()
            || k.server_address != f.peer.address.get()
            || k.client_generation != f.local.generation.get()
            || k.server_generation != f.peer.generation.get()
            || view.session_generation.get() < k.session
            || key_gen < k.key_generation
            || f.policy_generation < k.policy
            || view.hop_profile != HopProfile::H0
            || view.origin_security == OriginSecurity::O0
            || (k.origin == OriginSecurity::O2 && view.origin_security != OriginSecurity::O2)
            || (view.session_generation.get() == k.session
                && key_gen == k.key_generation
                && f.policy_generation == k.policy
                && peer.fingerprint.bytes() != k.security)
        {
            return Err(Error::Access);
        }
        security.authorize(
            self.access(
                peer,
                u16::from(ProtocolOpcode::OperationResume),
                AccessDirection::Outbound,
            ),
            f,
        )
    }
    #[allow(clippy::too_many_arguments)]
    fn seal_resume<const S: usize, const Q: usize, const T: usize, const RX: usize>(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        adapter: &AdapterOwner<'_, 1, T, RX, F>,
    ) -> Result<()> {
        self.recovery_access(peer, security)?;
        let w = security.address_width().bytes();
        let n = 25 + 2 * w;
        if n + 28 + 16 > F.min(adapter.link_mtu(peer.link)?) {
            return Err(Error::NoSpace);
        }
        let tx = counter.preview()?;
        let key = self.key.ok_or(Error::State)?;
        let mut body = [0; 28];
        encode_operation_key(
            OperationWireKey {
                operation: key.service.operation,
                service: key.service.service,
                opcode: key.service.opcode,
                request_digest: key.request_digest,
            },
            &mut body,
        )?;
        let mut prefix = [0; 33];
        let view = security.session_get(peer.session, peer.facts)?;
        CommonHeader {
            contract: HeaderContract::C0,
            origin_security: view.origin_security,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::Request,
            payload_kind: PayloadKind::Control,
            traffic_class: TrafficClass::Q1,
            hop_limit: HopLimit::new(1)?,
        }
        .encode(&mut prefix);
        prefix[3..7].copy_from_slice(&security.realm().get().to_be_bytes());
        prefix[7..7 + w].copy_from_slice(&peer.facts.local.address.get().to_be_bytes()[4 - w..]);
        prefix[7 + w..7 + 2 * w]
            .copy_from_slice(&peer.facts.peer.address.get().to_be_bytes()[4 - w..]);
        prefix[7 + 2 * w..11 + 2 * w]
            .copy_from_slice(&peer.facts.local.generation.get().to_be_bytes());
        prefix[11 + 2 * w..15 + 2 * w]
            .copy_from_slice(&peer.facts.peer.generation.get().to_be_bytes());
        prefix[15 + 2 * w..n - 2].copy_from_slice(&tx.get().to_be_bytes());
        prefix[n - 2..n].copy_from_slice(&u16::from(ProtocolOpcode::OperationResume).to_be_bytes());
        let length = security.protect_packet(
            crypto,
            OriginCounterOwner::Transaction(counter),
            peer.session,
            peer.facts,
            self.access(
                peer,
                u16::from(ProtocolOpcode::OperationResume),
                AccessDirection::Outbound,
            ),
            PacketPlan {
                prefix: &prefix[..n],
                payload: &body,
                canonical_identity: &prefix[3..n],
                origin_context: OriginContext::C0 {
                    fingerprint: peer.fingerprint,
                    transaction_id: tx,
                    opcode: ProtocolOpcode::OperationResume,
                    direction: if peer.facts.local.principal < peer.facts.peer.principal {
                        SenderDirection::AscendingPrincipal
                    } else {
                        SenderDirection::DescendingPrincipal
                    },
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut self.crypto_workspace,
            &mut self.resume_bytes,
        )?;
        self.resume = Some(Resume {
            peer,
            key_generation: security.outbound_key_generation(peer.session, peer.facts)?,
            length,
            attempts: 0,
            next_try: peer.facts.now_us,
            token: None,
        });
        Ok(())
    }
    #[allow(clippy::too_many_arguments)]
    fn transmit<const S: usize, const Q: usize, const T: usize, const RX: usize>(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
        transaction: &mut impl C0TransactionOwner,
    ) -> Result<bool> {
        let branch = self.tx_cursor;
        self.tx_cursor = (branch + 1) % 4;
        if self.stopped {
            return Ok(false);
        }
        match branch {
            0 => {
                let Some(h) = self.rpc_handle else {
                    return Ok(false);
                };
                if self.phase == SourcePhase::WaitResult
                    && self.rpc.client_retry_due(h, peer.facts.now_us)?
                    && matches!(
                        self.service.view(self.rpc.view(h)?.service)?.phase,
                        RequestPhase::WaitSend | RequestPhase::WaitResult
                    )
                {
                    self.rpc.requeue(h, peer.facts.now_us)?;
                    return Ok(true);
                }
                if self.phase != SourcePhase::WaitResult
                    || !self.rpc.scheduled_ready(h, peer.facts.now_us)?
                {
                    return Ok(false);
                }
                self.rpc
                    .submit(
                        h,
                        &mut self.service,
                        security,
                        peer.session,
                        peer.facts,
                        adapter,
                        driver,
                    )
                    .map(|()| true)
            }
            1 => self.submit_resume(peer, security, adapter, driver),
            slot => {
                let Some(h) = self.replies.handle_at(slot - 2) else {
                    return Ok(false);
                };
                let v = self.replies.view(h)?;
                if peer.facts.now_us >= v.expires_at {
                    return Ok(false);
                }
                match v.phase {
                    ReplyPhase::AwaitSeal => self
                        .replies
                        .seal(
                            h,
                            security,
                            crypto,
                            transaction,
                            peer.session,
                            peer.facts,
                            peer.link,
                            &mut self.crypto_workspace,
                        )
                        .map(|()| true),
                    ReplyPhase::Ready if self.replies.scheduled_ready(h, peer.facts.now_us)? => {
                        self.replies
                            .submit(
                                h,
                                security,
                                peer.session,
                                peer.facts,
                                adapter,
                                driver,
                                peer.link,
                            )
                            .map(|()| true)
                    }
                    _ => Ok(false),
                }
            }
        }
    }
    fn submit_resume<const S: usize, const Q: usize, const T: usize, const RX: usize>(
        &mut self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, Q>,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<bool> {
        if self.phase != SourcePhase::Recovering {
            return Ok(false);
        }
        let Some(r) = self.resume.as_ref() else {
            return Ok(false);
        };
        if r.token.is_some()
            || peer.facts.now_us < r.next_try
            || r.attempts >= self.config.max_attempts
        {
            return Ok(false);
        }
        self.recovery_access(peer, security)?;
        let f = r.peer.facts;
        let now = peer.facts.now_us;
        if r.peer.session != peer.session
            || r.peer.link != peer.link
            || r.peer.fingerprint != peer.fingerprint
            || f.local != peer.facts.local
            || f.peer != peer.facts.peer
            || f.policy_generation != peer.facts.policy_generation
            || f.link_generation != peer.facts.link_generation
            || now < f.now_us
            || security.outbound_key_generation(peer.session, peer.facts)? != r.key_generation
        {
            return Err(Error::State);
        }
        let next = now
            .checked_add(self.config.retry_us)
            .ok_or(Error::Exhausted)?;
        let token = adapter.tx_reserve(peer.link, 2)?;
        let r = self.resume.as_mut().ok_or(Error::State)?;
        r.token = Some(token);
        r.attempts += 1;
        r.next_try = next;
        adapter.tx_submit(token, &self.resume_bytes[..r.length], driver)?;
        Ok(true)
    }
    fn retire<const D: usize, const P: usize, const L: usize, const T: usize, const RX: usize>(
        &mut self,
        now: u64,
        p: &mut PersistenceOwner<'_, D, P, L>,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<bool> {
        let branch = self.retire_cursor;
        self.retire_cursor = (branch + 1) % 5;
        match branch {
            0 => {
                if let Some(io) = self.id_io {
                    if p.request_get(io)?.state == RequestState::Failed {
                        p.failed_retire(io)?;
                        self.id_io = None;
                        self.ids.fence();
                        self.stop();
                        return Ok(true);
                    }
                    if self.id_confirmed {
                        p.proof_retire(io)?;
                        self.id_io = None;
                        self.id_confirmed = false;
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            1 => {
                if let Some(token) = self.rx {
                    adapter.rx_retire(token)?;
                    self.rx = None;
                    return Ok(true);
                }
                Ok(false)
            }
            2 => self.retire_rpc(now, adapter, driver),
            3 => self.retire_resume(now, adapter, driver),
            _ => {
                let slot = self.reply_cursor;
                self.reply_cursor = (slot + 1) % 2;
                let Some(h) = self.replies.handle_at(slot) else {
                    return Ok(false);
                };
                let view = self.replies.view(h)?;
                if view.physical_pending {
                    let retired = if self.stopped || now >= view.expires_at {
                        self.replies.cancel_expired_attempt(h, now, adapter, driver)
                    } else {
                        self.replies.retire_attempt(h, adapter)
                    };
                    return match retired {
                        Ok(()) => Ok(true),
                        Err(Error::State) => Ok(false),
                        Err(e) => Err(e),
                    };
                }
                if now >= view.expires_at {
                    self.replies.retire_cache(h, now)?;
                    return Ok(true);
                }
                Ok(false)
            }
        }
    }
    fn retire_rpc<const T: usize, const RX: usize>(
        &mut self,
        now: u64,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<bool> {
        let Some(h) = self.rpc_handle else {
            return Ok(false);
        };
        let v = self.rpc.view(h)?;
        if v.physical_pending {
            let r = if self.stopped || now >= v.deadline {
                self.rpc.cancel_expired_attempt(h, now, adapter, driver)
            } else {
                self.rpc.retire_attempt(h, adapter)
            };
            return match r {
                Ok(()) => Ok(true),
                Err(Error::State) => Ok(false),
                Err(e) => Err(e),
            };
        }
        let state = self.service.view(v.service)?.phase;
        if self.stopped && matches!(state, RequestPhase::WaitSend | RequestPhase::WaitResult) {
            self.service.cancel(v.service)?;
            return Ok(true);
        }
        if self.phase != SourcePhase::WaitResult
            && matches!(
                state,
                RequestPhase::Complete | RequestPhase::Failed | RequestPhase::Cancelled
            )
        {
            self.rpc.retire(h, &mut self.service, now)?;
            self.rpc_handle = None;
            return Ok(true);
        }
        Ok(false)
    }
    fn retire_resume<const T: usize, const RX: usize>(
        &mut self,
        now: u64,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<bool> {
        let Some(r) = self.resume.as_ref() else {
            return Ok(false);
        };
        if let Some(token) = r.token {
            let v = adapter.tx_view(token)?;
            if !matches!(
                v.state,
                TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
            ) {
                if self.stopped
                    || self.phase != SourcePhase::Recovering
                    || now >= self.recovery_deadline
                {
                    adapter.tx_cancel(token, driver)?;
                } else {
                    return Ok(false);
                }
            }
            let v = adapter.tx_view(token)?;
            if !matches!(
                v.state,
                TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
            ) {
                return Ok(false);
            }
            adapter.tx_retire(token)?;
            let r = self.resume.as_mut().ok_or(Error::State)?;
            r.token = None;
            // Even a successful local send is not a result: retry identical query until observation.
            // 本地发送成功仍非结果，观察前按不变查询重试。
            return Ok(true);
        }
        if self.stopped || self.phase != SourcePhase::Recovering || now >= self.recovery_deadline {
            self.resume = None;
            self.resume_bytes.fill(0);
            return Ok(true);
        }
        Ok(false)
    }
}
