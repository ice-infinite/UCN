//! Fixed direct server scheduling: actual C0/Data RX, serialized Journal, bounded retirement.
//! 固定直达服务端调度：实际C0/Data接收、串行Journal与有界退休。
use crate::{
    ControlAdmission, ControlBody, ControlCoordinator, ControlHandle, ControlReplies,
    DurableExecutor, DurableRpcCoordinator, DurableRpcHandle, DurableRpcPhase,
    OperationCancelHandle, OperationCoordinator, OperationDispatch, OperationGc, ReplyPhase,
    RpcAction, RpcBridge, RpcEndpoint, RpcHandle, RpcPacketPhase,
};
use ucn_adapter::{AdapterOwner, LinkHandle, RxToken, TxDriver};
use ucn_digest::DigestWorkspace;
use ucn_durable::DurableBase;
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_persistence::{CodecWorkspace, PersistenceOwner, PersistenceProvider, RequestState};
use ucn_security::{
    AccessDirection, AccessRequest, C0TransactionOwner, CryptoProvider, OpenPacketPlan,
    OriginContext, OriginSequenceOwner, SecurityOwner, SecurityPacketWorkspace, SenderDirection,
};
use ucn_service::{OperationOwner, OperationPhase, ServiceOwner};
use ucn_types::{
    C0TransactionId, Error, HeaderContract, HopProfile, InteractionRole, OriginSequence,
    PayloadKind, ProtocolOpcode, Result, ServiceId,
};
use ucn_wire::CommonHeader;

use crate::runtime_common::{CLASSES, GatedDriver};
use crate::{
    JournalPeer, JournalRuntimeConfig, JournalRuntimeStats, JournalStepBudget, JournalStepReport,
    JournalWorkClass,
};
/// Static one-peer, one-Journal server; no automatic dynamic routing or shared-Link RX stealing.
/// 静态单Peer单Journal服务端，不冒充动态路由，不与其他RX Owner共享此适配实例。
#[allow(clippy::struct_excessive_bools)] // Separate stop/prepare/abort/publication facts and a two-way timer cursor.
pub struct JournalRuntime<
    const COMMANDS: usize,
    const FRAME: usize,
    const ARGUMENTS: usize,
    const RESULT_BYTES: usize,
> {
    config: JournalRuntimeConfig,
    service_id: ServiceId,
    control: ControlCoordinator<COMMANDS, FRAME>,
    replies: ControlReplies<COMMANDS, FRAME>,
    rpc: RpcBridge<1, FRAME>,
    service: ServiceOwner<1, 1, ARGUMENTS>,
    durable: DurableRpcCoordinator<ARGUMENTS, RESULT_BYTES>,
    cancellation: OperationCoordinator<1>,
    operation: OperationOwner<RESULT_BYTES>,
    gc: OperationGc,
    rpc_handle: Option<RpcHandle>,
    work: Option<DurableRpcHandle>,
    needs_prepare: bool,
    cancel_command: Option<ControlHandle>,
    cancel_job: Option<OperationCancelHandle>,
    abort_work: bool,
    drop_result: bool,
    arguments: [u8; ARGUMENTS],
    argument_bytes: usize,
    raw: [u8; FRAME],
    rx: Option<RxToken>,
    crypto_workspace: SecurityPacketWorkspace<FRAME>,
    codec: CodecWorkspace,
    digest: DigestWorkspace,
    cursor: usize,
    tx_cursor: usize,
    retire_cursor: usize,
    reply_retire_cursor: usize,
    timer_cursor: bool,
    last_now: u64,
    continuation: u32,
    stats: JournalRuntimeStats,
    stopped: bool,
}
impl<const C: usize, const F: usize, const A: usize, const R: usize> JournalRuntime<C, F, A, R> {
    /// Assemble independent fixed Owners; caller restores Journal through actual Foundation first.
    /// 装配独立固定Owner，恢复Journal由调用方先走真实Foundation。
    /// # Errors
    /// Invalid capacities/endpoint/policy rejects; no Provider/Driver work.
    pub fn new(
        config: JournalRuntimeConfig,
        endpoint: RpcEndpoint,
        operation: OperationOwner<R>,
    ) -> Result<Self> {
        if config.owner == 0
            || config.owner > u32::MAX - 6
            || F < 96
            || F > 4096
            || A == 0
            || A > F
            || R == 0
            || R > 146
            || R > A
            || C == 0
            || C > 16
            || config.max_attempts == 0
            || operation.view().1
        {
            return Err(Error::Config);
        }
        for duration in [
            config.request_lifetime_us,
            config.control_lifetime_us,
            config.io_timeout_us,
            config.retry_us,
            config.retention_us,
        ] {
            if duration == 0 || duration > i64::MAX as u64 {
                return Err(Error::Config);
            }
        }
        Ok(Self {
            service_id: endpoint.journal_service(A, R)?,
            control: ControlCoordinator::new(config.owner + 1)?,
            replies: ControlReplies::new(config.owner + 2, config.retry_us, config.max_attempts)?,
            rpc: RpcBridge::new(
                config.owner + 3,
                endpoint,
                config.retry_us,
                config.max_attempts,
            )?,
            service: ServiceOwner::new(config.owner + 4, config.request_lifetime_us)?,
            durable: DurableRpcCoordinator::new(config.owner + 5, config.io_timeout_us)?,
            cancellation: OperationCoordinator::new(config.owner + 6, config.control_lifetime_us)?,
            config,
            operation,
            gc: OperationGc::new(),
            rpc_handle: None,
            work: None,
            needs_prepare: false,
            cancel_command: None,
            cancel_job: None,
            abort_work: false,
            drop_result: false,
            arguments: [0; A],
            argument_bytes: 0,
            raw: [0; F],
            rx: None,
            crypto_workspace: SecurityPacketWorkspace::new(),
            codec: CodecWorkspace::new(),
            digest: DigestWorkspace::new(),
            cursor: 0,
            tx_cursor: 0,
            retire_cursor: 0,
            reply_retire_cursor: 0,
            timer_cursor: false,
            last_now: 0,
            continuation: 0,
            stats: JournalRuntimeStats::default(),
            stopped: false,
        })
    }
    /// Immutable Journal observation, not a write/rebind capability; 只读Journal观察，非写入/重绑权限。
    #[must_use]
    pub const fn operation(&self) -> &OperationOwner<R> {
        &self.operation
    }
    /// Fixed diagnostic snapshot; 固定诊断快照。
    #[must_use]
    pub const fn stats(&self) -> JournalRuntimeStats {
        self.stats
    }
    /// Exact current continuation phase, without exporting a mutation handle.
    /// 精确当前续接阶段，不导出可变Handle。
    /// # Errors
    /// Internal stale handle fails closed.
    pub fn continuation_phase(&self) -> Result<Option<DurableRpcPhase>> {
        self.work.map(|h| self.durable.view(h)).transpose()
    }
    /// Fence new work; accepted writes and exact physical retirement still run.
    /// 围栏新工作，已接纳写入与精确物理退休仍推进。
    pub fn stop(&mut self) {
        self.stopped = true;
        self.abort_work = true;
        self.control.stop();
        self.rpc.stop();
        self.replies.stop();
        self.durable.stop();
        self.cancellation.stop();
    }
    #[cfg(feature = "duplex-runtime")]
    pub(crate) fn duplex_binding(&self) -> Result<(u32, u32, ucn_persistence::DomainKey)> {
        if self.stopped
            || self.rx.is_some()
            || self.work.is_some()
            || self.needs_prepare
            || self.rpc_handle.is_some()
            || self.cancel_job.is_some()
            || self.gc.is_active()
            || !self.control.is_empty()
            || (0..C).any(|i| self.replies.handle_at(i).is_some())
        {
            return Err(Error::State);
        }
        Ok((
            self.config.owner,
            self.config.owner + 6,
            self.operation.contract().domain,
        ))
    }
    #[cfg(feature = "duplex-runtime")]
    pub(crate) fn receive_shared<
        const S: usize,
        const Q: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        raw: &[u8],
        link: LinkHandle,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        adapter: &AdapterOwner<'_, 1, T, RX, F>,
    ) -> Result<()> {
        if raw.len() > F || self.rx.is_some() {
            return Err(Error::NoSpace);
        }
        self.raw[..raw.len()].copy_from_slice(raw);
        self.stats.rx = self.stats.rx.saturating_add(1);
        let result = self.route_rx(raw.len(), link, peer, security, crypto, adapter);
        if result.is_err() {
            self.stats.rejected = self.stats.rejected.saturating_add(1);
        }
        result
    }
    pub(crate) fn quiescent(&self) -> bool {
        self.stopped
            && self.rx.is_none()
            && self.rpc_handle.is_none()
            && self.work.is_none()
            && !self.needs_prepare
            && self.cancel_job.is_none()
            && self.cancel_command.is_none()
            && !self.gc.is_active()
            && self.control.is_empty()
            && (0..C).all(|i| self.replies.handle_at(i).is_none())
    }
    fn baseline<const D: usize, const P: usize, const L: usize>(
        &mut self,
        persistence: &PersistenceOwner<'_, D, P, L>,
    ) -> Result<DurableBase> {
        let contract = self.operation.contract();
        let mut body = [0; 256];
        let n = persistence.copy_body(contract.domain, &mut body)?;
        let view = persistence.domain_get(contract.domain)?;
        if view.record_generation != self.operation.record_generation() {
            return Err(Error::State);
        }
        if view.body_digest != self.operation.record_digest() {
            return Err(Error::State);
        }
        DurableBase::from_view(contract, view, &body[..n], &mut self.codec)
    }
    /// Persistent rotating selection, hard per-class cap and at most 64 visits.
    /// 持久轮转选择、硬性各类上限，最多64次访问。
    /// # Errors
    /// Bad time/active callback rejects before any state or Provider/Driver operation.
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
        executor: &mut impl DurableExecutor,
    ) -> Result<JournalStepReport> {
        let now = peer.facts.now_us;
        self.validate_environment(now, security, persistence)?;
        let gate = security.callback_gate();
        self.last_now = now;
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
            let index = self.cursor;
            self.cursor = (self.cursor + 1) % 7;
            report.visits[index] += 1;
            self.stats.visits[index] = self.stats.visits[index].saturating_add(1);
            let advanced = match CLASSES[index] {
                JournalWorkClass::Ingress => self.ingress(peer, security, crypto, adapter),
                JournalWorkClass::Foundation => {
                    match gate.try_enter(CallbackClaim::new(self.config.owner, 1, 1, 0x0704)?) {
                        Ok(lease) => {
                            let progress =
                                persistence.step(provider, now, 1).map(|r| r.made_progress);
                            gate.leave(lease)?;
                            progress
                        }
                        Err(e) => Err(e),
                    }
                }
                JournalWorkClass::Control => self.control_step(peer, security, persistence),
                JournalWorkClass::Request => {
                    self.request_step(peer, security, persistence, gate, executor)
                }
                JournalWorkClass::Timer => self.timer(peer, security),
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
                    sequence,
                    transaction,
                ),
                JournalWorkClass::Retire => self.retire(
                    peer,
                    persistence,
                    adapter,
                    &mut GatedDriver {
                        driver,
                        gate,
                        owner: self.config.owner,
                    },
                ),
            };
            match advanced {
                Ok(true) => report.progress += 1,
                Ok(false) => {}
                Err(error) => {
                    if report.error.is_none() {
                        report.error = Some(error);
                    }
                }
            }
        }
        report.quiescent = self.quiescent() && adapter.rx_is_quiescent();
        Ok(report)
    }
    pub(crate) fn validate_environment<
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
    >(
        &self,
        now: u64,
        security: &SecurityOwner<'_, S, Q>,
        persistence: &PersistenceOwner<'_, D, P, L>,
    ) -> Result<()> {
        let contract = self.operation.contract();
        persistence.validate_consumer(
            contract.runtime,
            contract.persistence_owner,
            contract.domain,
            contract.owner,
        )?;
        if security.runtime_instance() != contract.runtime {
            return Err(Error::Config);
        }
        let domain = persistence.domain_get(contract.domain)?;
        if domain.schema_id != contract.schema || domain.schema_version != contract.version {
            return Err(Error::Config);
        }
        let own_io = self
            .work
            .is_some_and(|h| self.durable.pending_io(h).is_ok_and(|io| io.is_some()))
            || self
                .cancel_job
                .is_some_and(|h| self.cancellation.view(h).is_ok_and(|v| v.1.is_some()))
            || self.gc.pending_io().is_some();
        if !own_io
            && (domain.record_generation != self.operation.record_generation()
                || domain.body_digest != self.operation.record_digest())
        {
            return Err(Error::State);
        }
        if now == 0 || now < self.last_now || security.callback_gate().is_active() {
            return Err(Error::State);
        }
        Ok(())
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
        let result = if self.stopped {
            Ok(())
        } else {
            self.route_rx(item.frame_bytes, item.link, peer, security, crypto, adapter)
        };
        if result.is_err() {
            self.stats.rejected = self.stats.rejected.saturating_add(1);
        }
        // Common tail has no external callback; do not add an artificial scheduler delay
        // to every successfully copied RX. Failed retirement remains explicitly retained.
        adapter.rx_retire(item.token)?;
        self.rx = None;
        self.stats.retired = self.stats.retired.saturating_add(1);
        result.map(|()| true)
    }
    #[allow(clippy::too_many_lines)] // Two explicit fixed-profile authenticated raw paths, no hidden dispatch callbacks.
    fn route_rx<const S: usize, const Q: usize, const T: usize, const RX: usize>(
        &mut self,
        length: usize,
        link: LinkHandle,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        adapter: &AdapterOwner<'_, 1, T, RX, F>,
    ) -> Result<()> {
        if link != peer.link {
            return Err(Error::Access);
        }
        let raw = &self.raw[..length];
        let common = CommonHeader::decode(raw)?;
        let width = security.address_width().bytes();
        if common.hop_limit.get() != 1 || common.origin_security == ucn_types::OriginSecurity::O0 {
            return Err(Error::Unsupported);
        }
        let direction = if peer.facts.peer.principal < peer.facts.local.principal {
            SenderDirection::AscendingPrincipal
        } else {
            SenderDirection::DescendingPrincipal
        };
        let mut identity = [0; 46];
        match (common.contract, common.payload_kind) {
            (HeaderContract::C0, PayloadKind::Control) => {
                let prefix = 25 + 2 * width;
                if length < prefix + 16 {
                    return Err(Error::Malformed);
                }
                let opcode = ProtocolOpcode::try_from(u16::from_be_bytes(
                    raw[prefix - 2..prefix]
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                ))?;
                if !matches!(
                    opcode,
                    ProtocolOpcode::OperationQuery
                        | ProtocolOpcode::OperationCancel
                        | ProtocolOpcode::OperationResume
                        | ProtocolOpcode::OperationResultAck
                ) {
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
                        opcode,
                        direction,
                    },
                    hop_profile: HopProfile::H0,
                };
                let admitted = self.control.receive_cached(
                    security,
                    crypto,
                    peer.session,
                    peer.facts,
                    AccessRequest {
                        session: peer.session,
                        context_fingerprint: peer.fingerprint,
                        service: self.service_id,
                        protocol_opcode: u16::from(opcode),
                        direction: AccessDirection::Inbound,
                    },
                    plan,
                    &mut self.crypto_workspace,
                    &mut self.replies,
                )?;
                // Latch only a fully authenticated exact cancel before any subsequent executor edge,
                // even if this step has already spent its Control cap.
                if let ControlAdmission::Command(h) = admitted {
                    let command = self.control.view(h)?;
                    if matches!(command.body(), ControlBody::OperationCancel(_))
                        && (self.work.is_some() || self.needs_prepare)
                    {
                        let key = if let Some(key) = self.operation.key() {
                            Some(key)
                        } else if let Some(h) = self.rpc_handle {
                            self.rpc
                                .durable_key(h, security, peer.session, peer.facts)
                                .ok()
                                .map(|v| v.0)
                        } else {
                            None
                        };
                        if let Some(key) = key {
                            if crate::operation_control::current(
                                &command,
                                key,
                                security,
                                peer.session,
                                peer.facts,
                                false,
                            )
                            .is_ok()
                                && self.cancel_command.is_none()
                            {
                                self.cancel_command = Some(h);
                                self.abort_work = true;
                            }
                        }
                    }
                }
            }
            (HeaderContract::C1, PayloadKind::Data)
                if common.interaction == InteractionRole::Request =>
            {
                let prefix = 9 + 2 * width;
                if length < prefix + 16 {
                    return Err(Error::Malformed);
                }
                let service = ServiceId::new(u16::from_be_bytes(
                    raw[prefix - 6..prefix - 4]
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                ))?;
                if service != self.service_id {
                    return Err(Error::Access);
                }
                let seq = OriginSequence::new(u32::from_be_bytes(
                    raw[prefix - 4..prefix]
                        .try_into()
                        .map_err(|_| Error::Malformed)?,
                ))?;
                identity[..16].copy_from_slice(&peer.facts.peer.principal);
                identity[16..20].copy_from_slice(&peer.facts.peer.generation.get().to_be_bytes());
                identity[20..36].copy_from_slice(&peer.facts.local.principal);
                identity[36..40].copy_from_slice(&peer.facts.local.generation.get().to_be_bytes());
                identity[40..42].copy_from_slice(&service.get().to_be_bytes());
                identity[42..].copy_from_slice(&seq.get().to_be_bytes());
                let plan = OpenPacketPlan {
                    packet: raw,
                    prefix_bytes: prefix,
                    canonical_identity: &identity,
                    origin_context: OriginContext::Sequenced {
                        contract: HeaderContract::C1,
                        fingerprint: peer.fingerprint,
                        sequence: seq,
                    },
                    hop_profile: HopProfile::H0,
                };
                let received = self.rpc.receive_journaled_request(
                    &mut self.service,
                    &self.operation,
                    security,
                    crypto,
                    peer.session,
                    peer.facts,
                    plan,
                    link,
                    adapter,
                    &mut self.arguments,
                    &mut self.crypto_workspace,
                )?;
                if self.rpc_handle.is_some_and(|old| old != received.handle) {
                    return Err(Error::State);
                }
                self.rpc_handle = Some(received.handle);
                match received.action {
                    RpcAction::Persist => {
                        self.abort_work = false;
                        self.drop_result = false;
                        self.argument_bytes = received.bytes;
                        self.needs_prepare = true;
                    }
                    RpcAction::Replay => {
                        if self.rpc.view(received.handle)?.phase == RpcPacketPhase::Cached {
                            self.rpc.requeue(received.handle, peer.facts.now_us)?;
                        }
                    }
                    RpcAction::Pending => {}
                    RpcAction::Invoke => return Err(Error::State),
                }
            }
            _ => return Err(Error::Unsupported),
        }
        Ok(())
    }
    #[allow(clippy::too_many_lines)] // One explicit serialized C0 command dispatch, including retained cancel intent.
    fn control_step<
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, Q>,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
    ) -> Result<bool> {
        let h = match self.control.peek_next() {
            Ok(h) => h,
            Err(Error::NotFound) => return Ok(false),
            Err(e) => return Err(e),
        };
        if self.cancel_job.is_some() && self.cancel_command == Some(h) {
            return Ok(false);
        }
        let command = self.control.view(h)?;
        let expires = command
            .accepted_at()
            .checked_add(self.config.control_lifetime_us)
            .ok_or(Error::Exhausted)?;
        if self.stopped || peer.facts.now_us >= expires {
            self.control.ack(h)?;
            if self.cancel_command == Some(h) {
                self.cancel_command = None;
            }
            self.stats.rejected = self.stats.rejected.saturating_add(1);
            return Ok(true);
        }
        if self.work.is_some() || self.needs_prepare {
            if matches!(command.body(), ControlBody::OperationCancel(_)) {
                if let Some(key) = self.operation.key() {
                    if let Err(e) = crate::operation_control::current(
                        &command,
                        key,
                        security,
                        peer.session,
                        peer.facts,
                        false,
                    ) {
                        self.control.ack(h)?;
                        return Err(e);
                    }
                    if self.cancel_command.is_none() {
                        self.cancel_command = Some(h);
                        self.abort_work = true;
                        return Ok(true);
                    }
                }
            }
            return Ok(false);
        }
        if self.cancel_job.is_some() || self.gc.is_active() || self.operation.view().1 {
            return Ok(false);
        }
        let dispatched = match command.body() {
            ControlBody::OperationResultAck(_) => self
                .control
                .dispatch_result_ack(
                    h,
                    &mut self.operation,
                    persistence,
                    security,
                    peer.session,
                    peer.facts,
                    self.config.retention_us,
                    &mut self.digest,
                    &mut self.codec,
                )
                .map(|()| None),
            ControlBody::OperationResume(_) => self
                .control
                .dispatch_operation_resume(
                    h,
                    &self.operation,
                    persistence,
                    security,
                    peer.session,
                    peer.facts,
                    self.config.control_lifetime_us,
                    &mut self.replies,
                    &mut self.digest,
                    &mut self.codec,
                )
                .map(|_| None),
            ControlBody::OperationQuery(_) | ControlBody::OperationCancel(_) => {
                let base = self.baseline(persistence)?;
                self.cancellation
                    .dispatch(
                        &mut self.control,
                        h,
                        security,
                        peer.session,
                        peer.facts,
                        &mut self.operation,
                        base,
                        &mut self.replies,
                        &mut self.codec,
                        &mut self.digest,
                    )
                    .map(|result| match result {
                        OperationDispatch::Reply(_) => None,
                        OperationDispatch::Pending(job) => Some(job),
                    })
            }
            _ => Err(Error::Unsupported),
        };
        match dispatched {
            Ok(Some(job)) => {
                self.cancel_job = Some(job);
                self.cancel_command = Some(h);
                Ok(true)
            }
            Ok(None) => {
                if self.cancel_command == Some(h) {
                    self.cancel_command = None;
                }
                Ok(true)
            }
            Err(Error::NoSpace) => Ok(false),
            Err(e) => {
                self.control.ack(h)?;
                if self.cancel_command == Some(h) {
                    self.cancel_command = None;
                }
                self.stats.rejected = self.stats.rejected.saturating_add(1);
                Err(e)
            }
        }
    }
    #[allow(clippy::too_many_lines)] // Straight-line durable FSM, one edge per budget visit.
    fn request_step<
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, Q>,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
        gate: &CallbackGate,
        executor: &mut impl DurableExecutor,
    ) -> Result<bool> {
        let now = peer.facts.now_us;
        if let Some(job) = self.cancel_job {
            let (confirmed, io) = self.cancellation.view(job)?;
            if let Some(io) = io {
                if persistence.request_get(io)?.state == RequestState::Failed {
                    return Ok(false);
                }
                if persistence.proof_get(io).is_err() {
                    return Ok(false);
                }
            } else if !confirmed {
                return self
                    .cancellation
                    .submit(job, &mut self.operation, persistence, now)
                    .map(|_| true);
            }
            match self.cancellation.finish(
                job,
                &mut self.control,
                &mut self.operation,
                persistence,
                security,
                peer.session,
                peer.facts,
                &mut self.replies,
                &mut self.digest,
            ) {
                Ok(_) => {
                    self.cancel_job = None;
                    self.cancel_command = None;
                    Ok(true)
                }
                Err(Error::Access | Error::Timeout) => Ok(false),
                Err(e) => Err(e),
            }
        } else if let Some(work) = self.work {
            let phase = self.durable.view(work)?;
            match phase {
                DurableRpcPhase::PreparedPending
                | DurableRpcPhase::ExecutingPending
                | DurableRpcPhase::ResultPending => {
                    if let Some(io) = self.durable.pending_io(work)? {
                        if persistence.request_get(io)?.state == RequestState::Failed
                            || persistence.proof_get(io).is_err()
                        {
                            return Ok(false);
                        }
                        self.durable
                            .finish_write(work, &mut self.operation, persistence, now)?;
                    } else {
                        self.durable
                            .submit(work, &mut self.operation, persistence, now)?;
                    }
                    Ok(true)
                }
                DurableRpcPhase::Prepared | DurableRpcPhase::ReadyToExecute => {
                    let rpc = self.rpc_handle.ok_or(Error::State)?;
                    if self.stopped
                        || self.abort_work
                        || now >= self.rpc.view(rpc)?.deadline
                        || security.session_get(peer.session, peer.facts).is_err()
                    {
                        self.durable
                            .abandon_before_execution(work, &self.operation, now)?;
                        return Ok(true);
                    }
                    let advanced = if phase == DurableRpcPhase::Prepared {
                        self.durable.arm(
                            work,
                            &self.rpc,
                            &mut self.operation,
                            persistence,
                            security,
                            peer.session,
                            peer.facts,
                            &mut self.codec,
                        )
                    } else {
                        self.durable.execute(
                            work,
                            &self.rpc,
                            &mut self.operation,
                            persistence,
                            security,
                            peer.session,
                            peer.facts,
                            gate,
                            executor,
                        )
                    };
                    match advanced {
                        Ok(()) => {
                            if phase == DurableRpcPhase::ReadyToExecute {
                                self.stats.executions = self.stats.executions.saturating_add(1);
                            }
                            Ok(true)
                        }
                        Err(e) => {
                            if !gate.is_active() {
                                self.abort_work = true;
                            }
                            Err(e)
                        }
                    }
                }
                DurableRpcPhase::Observed => self
                    .durable
                    .persist_outcome(work, &mut self.operation, persistence, now, &mut self.codec)
                    .map(|()| true),
                DurableRpcPhase::ResultReady => {
                    if self.stopped || self.drop_result {
                        return Ok(false);
                    }
                    let published = self
                        .durable
                        .publish(
                            work,
                            &mut self.rpc,
                            &self.operation,
                            persistence,
                            &mut self.service,
                            security,
                            peer.session,
                            peer.facts,
                        )
                        .map(|_| true);
                    if matches!(
                        published,
                        Err(Error::Access | Error::State | Error::Timeout)
                    ) {
                        self.drop_result = true;
                    }
                    published
                }
                DurableRpcPhase::Published | DurableRpcPhase::Faulted => Ok(false),
            }
        } else if self.needs_prepare && self.cancel_job.is_none() && !self.gc.is_active() {
            if self.stopped {
                self.needs_prepare = false;
                self.arguments.fill(0);
                return Ok(true);
            }
            let rpc = self.rpc_handle.ok_or(Error::State)?;
            match self.durable.prepare(
                &self.rpc,
                rpc,
                &self.arguments[..self.argument_bytes],
                &mut self.operation,
                persistence,
                security,
                peer.session,
                peer.facts,
                &mut self.codec,
                &mut self.digest,
            ) {
                Ok(work) => {
                    self.work = Some(work);
                    self.needs_prepare = false;
                    self.arguments.fill(0);
                    Ok(true)
                }
                Err(Error::NoSpace) => Ok(false),
                Err(e) => {
                    self.needs_prepare = false;
                    self.arguments.fill(0);
                    self.stats.rejected = self.stats.rejected.saturating_add(1);
                    Err(e)
                }
            }
        } else {
            Ok(false)
        }
    }
    fn timer<const S: usize, const Q: usize>(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
    ) -> Result<bool> {
        self.timer_cursor = !self.timer_cursor;
        if self.timer_cursor {
            self.service.step(peer.facts.now_us, 1)?;
            Ok(false)
        } else {
            let maintenance = security.maintain_replay_reservations(peer.facts.now_us, 1);
            Ok(maintenance.expired > 0)
        }
    }
    #[allow(clippy::too_many_arguments)]
    fn transmit<const S: usize, const Q: usize, const T: usize, const RX: usize>(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, Q>,
        crypto: &mut impl CryptoProvider,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
        sequence: &mut impl OriginSequenceOwner,
        transaction: &mut impl C0TransactionOwner,
    ) -> Result<bool> {
        let index = self.tx_cursor;
        self.tx_cursor = (index + 1) % (C + 1);
        if self.stopped {
            return Ok(false);
        }
        if index == C {
            let Some(h) = self.rpc_handle else {
                return Ok(false);
            };
            let view = self.rpc.view(h)?;
            if peer.facts.now_us >= view.deadline {
                return Ok(false);
            }
            match view.phase {
                RpcPacketPhase::AwaitSeal => self
                    .rpc
                    .seal_response(
                        h,
                        &self.service,
                        security,
                        crypto,
                        sequence,
                        peer.session,
                        peer.facts,
                        &mut self.crypto_workspace,
                    )
                    .map(|()| true),
                RpcPacketPhase::Ready if self.rpc.scheduled_ready(h, peer.facts.now_us)? => {
                    match self.rpc.submit(
                        h,
                        &mut self.service,
                        security,
                        peer.session,
                        peer.facts,
                        adapter,
                        driver,
                    ) {
                        Ok(()) => Ok(true),
                        Err(Error::NoSpace) => Ok(false),
                        Err(e) => Err(e),
                    }
                }
                _ => Ok(false),
            }
        } else {
            let Some(h) = self.replies.handle_at(index) else {
                return Ok(false);
            };
            let view = self.replies.view(h)?;
            if peer.facts.now_us >= view.expires_at {
                return Ok(false);
            }
            match view.phase {
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
                    match self.replies.submit(
                        h,
                        security,
                        peer.session,
                        peer.facts,
                        adapter,
                        driver,
                        peer.link,
                    ) {
                        Ok(()) => Ok(true),
                        Err(Error::NoSpace) => Ok(false),
                        Err(e) => Err(e),
                    }
                }
                _ => Ok(false),
            }
        }
    }
    fn retire<const D: usize, const P: usize, const L: usize, const T: usize, const RX: usize>(
        &mut self,
        peer: JournalPeer,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
        adapter: &mut AdapterOwner<'_, 1, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<bool> {
        let index = self.retire_cursor;
        self.retire_cursor = (index + 1) % 4;
        let now = peer.facts.now_us;
        match index {
            0 => {
                if let Some(token) = self.rx {
                    adapter.rx_retire(token)?;
                    self.rx = None;
                    self.stats.retired = self.stats.retired.saturating_add(1);
                    return Ok(true);
                }
                Ok(false)
            }
            1 => {
                let Some(h) = self.rpc_handle else {
                    return Ok(false);
                };
                let view = self.rpc.view(h)?;
                if view.physical_pending {
                    let result = if self.stopped || now >= view.deadline {
                        self.rpc.cancel_expired_attempt(h, now, adapter, driver)
                    } else {
                        self.rpc.retire_attempt(h, adapter)
                    };
                    return match result {
                        Ok(()) => {
                            self.stats.retired = self.stats.retired.saturating_add(1);
                            Ok(true)
                        }
                        Err(Error::State) => Ok(false),
                        Err(e) => Err(e),
                    };
                }
                if now >= view.deadline && self.work.is_none() && !self.needs_prepare {
                    self.rpc.retire(h, &mut self.service, now)?;
                    self.rpc_handle = None;
                    self.stats.retired = self.stats.retired.saturating_add(1);
                    return Ok(true);
                }
                Ok(false)
            }
            2 => {
                let slot = self.reply_retire_cursor;
                self.reply_retire_cursor = (slot + 1) % C;
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
                        Ok(()) => {
                            self.stats.retired = self.stats.retired.saturating_add(1);
                            Ok(true)
                        }
                        Err(Error::State) => Ok(false),
                        Err(e) => Err(e),
                    };
                }
                if now >= view.expires_at && view.phase != ReplyPhase::AwaitCommit {
                    self.replies.retire_cache(h, now)?;
                    self.stats.retired = self.stats.retired.saturating_add(1);
                    return Ok(true);
                }
                Ok(false)
            }
            _ => self.retire_journal(now, persistence),
        }
    }
    fn retire_journal<const D: usize, const P: usize, const L: usize>(
        &mut self,
        now: u64,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
    ) -> Result<bool> {
        if let Some(work) = self.work {
            if let Some(io) = self.durable.pending_io(work)? {
                if persistence.request_get(io)?.state == RequestState::Failed {
                    self.durable
                        .retire_failed(work, &mut self.operation, persistence, now)?;
                    self.stop();
                    return Ok(true);
                }
                return Ok(false);
            }
            let phase = self.durable.view(work)?;
            let expired = if let Some(h) = self.rpc_handle {
                now >= self.rpc.view(h)?.deadline
            } else {
                true
            };
            if phase == DurableRpcPhase::Published
                || phase == DurableRpcPhase::Faulted
                || (phase == DurableRpcPhase::ResultReady
                    && (self.stopped || expired || self.drop_result))
            {
                self.durable.retire(work, &self.operation, now)?;
                self.work = None;
                self.stats.retired = self.stats.retired.saturating_add(1);
                return Ok(true);
            }
            return Ok(false);
        }
        if let Some(job) = self.cancel_job {
            let (confirmed, io) = self.cancellation.view(job)?;
            if io.is_some_and(|io| {
                persistence
                    .request_get(io)
                    .is_ok_and(|v| v.state == RequestState::Failed)
            }) {
                self.cancellation.retire_failed(
                    job,
                    &mut self.control,
                    &mut self.operation,
                    persistence,
                    &mut self.replies,
                )?;
                self.cancel_job = None;
                self.cancel_command = None;
                self.stop();
                return Ok(true);
            }
            if confirmed && io.is_none() {
                self.cancellation
                    .reject_confirmed(job, &mut self.control, &mut self.replies)?;
                self.cancel_job = None;
                self.cancel_command = None;
                return Ok(true);
            }
            return Ok(false);
        }
        if self.gc.is_active() {
            if let Some(io) = self.gc.pending_io() {
                let state = persistence.request_get(io)?.state;
                if state == RequestState::Failed {
                    self.gc.retire_failed(&mut self.operation, persistence)?;
                    self.stop();
                    return Ok(true);
                }
                if persistence.proof_get(io).is_err() {
                    return Ok(false);
                }
                self.gc.finish(&mut self.operation, persistence)?;
                self.stats.retired = self.stats.retired.saturating_add(1);
                return Ok(true);
            }
            self.gc.submit(&mut self.operation, persistence, now)?;
            return Ok(true);
        }
        if !self.needs_prepare
            && self.operation.gc_after().is_some_and(|until| now >= until)
            && matches!(
                self.operation.view().0,
                Some(OperationPhase::CommittedResult | OperationPhase::AbortedNoEffect)
            )
        {
            let continuation = self.continuation.checked_add(1).ok_or(Error::Exhausted)?;
            self.continuation = continuation;
            self.gc.begin(
                &mut self.operation,
                persistence,
                now,
                now.checked_add(self.config.io_timeout_us)
                    .ok_or(Error::Exhausted)?,
                continuation,
                &mut self.codec,
            )?;
            return Ok(true);
        }
        Ok(false)
    }
}
