#![no_std]
#![forbid(unsafe_code)]
//! Coordinator owns cross-module ordering; business Owners never call one another.
//! Coordinator持有跨模块顺序，业务Owner不互相调用。
#[cfg(any(
    feature = "journal-runtime",
    feature = "source-runtime",
    feature = "parent-control",
    feature = "capability-control",
    feature = "bootstrap-runtime"
))]
mod runtime_common;
#[cfg(any(
    feature = "journal-runtime",
    feature = "source-runtime",
    feature = "parent-control",
    feature = "capability-control",
    feature = "bootstrap-runtime"
))]
pub use runtime_common::*;
#[cfg(feature = "capability-control")]
mod capability_control;
#[cfg(feature = "capability-control")]
pub use capability_control::*;
#[cfg(feature = "route-runtime")]
mod route_runtime;
#[cfg(feature = "route-runtime")]
pub use route_runtime::*;
#[cfg(feature = "bootstrap-runtime")]
mod bootstrap_wire;
#[cfg(feature = "bootstrap-runtime")]
pub use bootstrap_wire::*;
#[cfg(feature = "bootstrap-runtime")]
mod bootstrap_runtime;
#[cfg(feature = "bootstrap-runtime")]
pub use bootstrap_runtime::*;
#[cfg(feature = "parent-control")]
mod parent_control;
#[cfg(feature = "parent-control")]
pub use parent_control::*;
#[cfg(feature = "transfer-sender")]
mod transfer_sender;
#[cfg(feature = "transfer-sender")]
pub use transfer_sender::*;
mod control;
pub use control::*;
#[cfg(feature = "time-runtime")]
mod time_runtime;
#[cfg(feature = "time-runtime")]
pub use time_runtime::*;
mod reply;
#[cfg(feature = "transfer")]
mod transfer_control;
#[cfg(feature = "transfer")]
mod transfer_data;
pub use reply::*;
#[cfg(feature = "service-rpc")]
mod service_data;
#[cfg(feature = "service-rpc")]
pub use service_data::*;
#[cfg(feature = "durable-rpc")]
mod durable_service;
#[cfg(feature = "durable-rpc")]
pub use durable_service::*;
#[cfg(feature = "operation-control")]
mod operation_control;
#[cfg(feature = "operation-control")]
pub use operation_control::*;
#[cfg(feature = "operation-control")]
mod operation_result;
#[cfg(feature = "operation-control")]
pub use operation_result::*;
#[cfg(feature = "journal-runtime")]
mod journal_runtime;
#[cfg(feature = "journal-runtime")]
pub use journal_runtime::*;
#[cfg(feature = "source-runtime")]
mod source_runtime;
#[cfg(feature = "source-runtime")]
pub use source_runtime::*;
#[cfg(feature = "duplex-runtime")]
mod duplex_runtime;
#[cfg(feature = "duplex-runtime")]
pub use duplex_runtime::*;
#[cfg(feature = "transfer")]
pub use transfer_data::*;
use ucn_adapter::{AdapterOwner, LinkHandle, TxDriver, TxState, TxToken};
use ucn_security::{
    AccessDirection, AccessRequest, CryptoProvider, CurrentFacts, OpenDisposition, OpenPacketPlan,
    SecurityOwner, SecurityPacketWorkspace, SessionHandle,
};
use ucn_transport::{
    AttemptHandle, Binding, DeliveryAck, ReceiveAction, ReliableHandle, ReliableKey, ReliableOwner,
    SecurityDomain, Submission,
};
use ucn_types::{DeliveryGuarantee, Error, HeaderContract, InteractionRole, PayloadKind, Result};
use ucn_wire::CommonHeader;
/// One accepted application publication or exact duplicate ACK.
/// 一次已接纳业务发布或精确重复ACK。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReceiveResult {
    /// Retained delivery ACK; 保留的交付ACK。
    pub ack: DeliveryAck,
    /// Fresh application bytes, zero for duplicate; 新业务字节数，重复为零。
    pub bytes: usize,
    /// Authentication/replay identified a duplicate; 认证/Replay识别重复。
    pub duplicate: bool,
}
#[derive(Clone, Copy)]
struct Attempt {
    token: TxToken,
    handle: AttemptHandle,
}
/// Caller-owned fixed staging, with explicit physical obligations.
/// 调用方持有固定staging及显式物理义务。
pub struct RuntimeBridge<const HOLDERS: usize, const BYTES: usize> {
    bytes: [u8; BYTES],
    attempts: [Option<Attempt>; HOLDERS],
    running: bool,
    stopped: bool,
    retire_cursor: usize,
}
impl<const H: usize, const B: usize> Default for RuntimeBridge<H, B> {
    fn default() -> Self {
        Self::new()
    }
}
impl<const H: usize, const B: usize> RuntimeBridge<H, B> {
    /// Const storage for a static composition; 静态装配的const存储。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            bytes: [0; B],
            attempts: [None; H],
            running: false,
            stopped: false,
            retire_cursor: 0,
        }
    }
    /// Stop rejects new work, existing Driver retirement remains available.
    /// Stop拒绝新工作，仍允许既有Driver退休。
    pub fn stop(&mut self) {
        self.stopped = true;
    }
    /// Security verify -> receipt reserve -> Replay commit -> application copy.
    /// Security验证、Receipt预留、Replay提交、业务复制。
    /// # Errors
    /// Every failure aborts unpublished receipt/Replay; output is unchanged.
    #[allow(clippy::too_many_arguments)] // Explicit independently-owned modules and caller scratch.
    pub fn receive<
        const S: usize,
        const R: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
        const W: usize,
    >(
        &mut self,
        security: &mut SecurityOwner<'_, S, R>,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        scratch: &mut SecurityPacketWorkspace<W>,
        reliable: &mut ReliableOwner<T, RX, F>,
        output: &mut [u8],
    ) -> Result<ReceiveResult> {
        if self.running || self.stopped || access.direction != AccessDirection::Inbound {
            return Err(Error::State);
        }
        let header = CommonHeader::decode(plan.packet)?;
        if header.contract != HeaderContract::C1
            || header.delivery != DeliveryGuarantee::Reliable
            || header.interaction != InteractionRole::OneWay
            || header.payload_kind != PayloadKind::Data
        {
            return Err(Error::Unsupported);
        }
        self.running = true;
        let result = self.receive_inner(
            security, provider, session, facts, access, plan, scratch, reliable, output,
        );
        self.running = false;
        result
    }
    #[allow(clippy::too_many_arguments)]
    fn receive_inner<
        const S: usize,
        const R: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
        const W: usize,
    >(
        &mut self,
        security: &mut SecurityOwner<'_, S, R>,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        scratch: &mut SecurityPacketWorkspace<W>,
        reliable: &mut ReliableOwner<T, RX, F>,
        output: &mut [u8],
    ) -> Result<ReceiveResult> {
        let opened = security.open_packet(
            provider,
            session,
            facts,
            access,
            plan,
            scratch,
            &mut self.bytes,
        )?;
        let evidence = opened.evidence();
        let (realm, source, destination) = evidence.identities();
        let (sg, kg, policy, origin, fingerprint) = evidence.security();
        let (aad, payload) = evidence.digests();
        let sequence = match u32::try_from(evidence.sequence()) {
            Ok(value) if value != 0 => value,
            _ => {
                security.replay_abort(opened.replay)?;
                return Err(Error::Malformed);
            }
        };
        let convert = |binding: ucn_security::Binding| Binding {
            principal: binding.principal,
            address: binding.address.get(),
            generation: binding.generation.get(),
        };
        let key = ReliableKey {
            realm,
            source: convert(source),
            destination: convert(destination),
            service: access.service.get(),
            sequence,
            security: SecurityDomain {
                session: sg,
                key: kg,
                policy,
                origin,
                fingerprint,
                public_trusted_o0: false,
            },
        };
        if output.len() < opened.payload_bytes {
            security.replay_abort(opened.replay)?;
            return Err(Error::NoSpace);
        }
        let reservation = reliable.receive_prepare(
            key,
            aad,
            payload,
            opened.disposition == OpenDisposition::AuthenticatedReplayCandidate,
            facts.now_us,
        );
        let action = match reservation {
            Ok(value) => value,
            Err(error) => {
                security.replay_abort(opened.replay)?;
                return Err(error);
            }
        };
        if let Err(error) = security.replay_commit(opened.replay, facts) {
            if let ReceiveAction::Reserved(h) = action {
                reliable.receive_abort(h)?;
            }
            return Err(error);
        }
        match action {
            ReceiveAction::Duplicate(ack) => Ok(ReceiveResult {
                ack,
                bytes: 0,
                duplicate: true,
            }),
            ReceiveAction::Reserved(h) => {
                let ack = reliable.receive_commit(h)?;
                output[..opened.payload_bytes].copy_from_slice(&self.bytes[..opened.payload_bytes]);
                Ok(ReceiveResult {
                    ack,
                    bytes: opened.payload_bytes,
                    duplicate: false,
                })
            }
        }
    }
    /// Accept authenticated C0 `DeliveryAck`, preflighting before Replay commit.
    /// 接纳认证C0 DeliveryAck，在Replay提交前预检。
    /// # Errors
    /// Wrong Role/Opcode/source/domain/sequence or conflicting terminal receipt rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn receive_ack<
        const S: usize,
        const R: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
        const W: usize,
    >(
        &mut self,
        security: &mut SecurityOwner<'_, S, R>,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        scratch: &mut SecurityPacketWorkspace<W>,
        reliable: &mut ReliableOwner<T, RX, F>,
        h: ReliableHandle,
    ) -> Result<()> {
        if self.running || self.stopped {
            return Err(Error::State);
        }
        let header = CommonHeader::decode(plan.packet)?;
        if header.contract != HeaderContract::C0
            || header.payload_kind != PayloadKind::Control
            || header.delivery != DeliveryGuarantee::BestEffort
            || header.interaction != InteractionRole::OneWay
            || access.protocol_opcode != u16::from(ucn_types::ProtocolOpcode::DeliveryAck)
            || access.direction != AccessDirection::Inbound
        {
            return Err(Error::Unsupported);
        }
        self.running = true;
        let result = self.receive_ack_inner(
            security, provider, session, facts, access, plan, scratch, reliable, h,
        );
        self.running = false;
        result
    }
    #[allow(clippy::too_many_arguments)]
    fn receive_ack_inner<
        const S: usize,
        const R: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
        const W: usize,
    >(
        &mut self,
        security: &mut SecurityOwner<'_, S, R>,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        scratch: &mut SecurityPacketWorkspace<W>,
        reliable: &mut ReliableOwner<T, RX, F>,
        h: ReliableHandle,
    ) -> Result<()> {
        let opened = security.open_packet(
            provider,
            session,
            facts,
            access,
            plan,
            scratch,
            &mut self.bytes,
        )?;
        let evidence = opened.evidence();
        let ack = if opened.disposition == OpenDisposition::AuthenticatedReplayCandidate {
            // Transport stores the exact terminal ACK; duplicate frames never get plaintext.
            security.replay_abort(opened.replay)?;
            return Err(Error::Replay);
        } else {
            match ucn_transport::decode_ack(&self.bytes[..opened.payload_bytes]) {
                Ok(ack) => ack,
                Err(error) => {
                    security.replay_abort(opened.replay)?;
                    return Err(error);
                }
            }
        };
        let (realm, source, destination) = evidence.identities();
        let (sg, _, policy, origin, fingerprint) = evidence.security();
        let outbound_key = match security.outbound_key_generation(session, facts) {
            Ok(key) => key,
            Err(error) => {
                security.replay_abort(opened.replay)?;
                return Err(error);
            }
        };
        let convert = |binding: ucn_security::Binding| Binding {
            principal: binding.principal,
            address: binding.address.get(),
            generation: binding.generation.get(),
        };
        let reverse = ReliableKey {
            realm,
            source: convert(source),
            destination: convert(destination),
            service: ack.service,
            sequence: ack.sequence,
            security: SecurityDomain {
                session: sg,
                key: outbound_key,
                policy,
                origin,
                fingerprint,
                public_trusted_o0: false,
            },
        };
        if let Err(error) = reliable.ack_preflight(h, reverse, ack, facts.now_us) {
            security.replay_abort(opened.replay)?;
            return Err(error);
        }
        security.replay_commit(opened.replay, facts)?;
        reliable.on_ack(h, reverse, ack, facts.now_us)
    }
    /// Submit immutable Origin bytes through an actual Adapter/Driver attempt.
    /// 通过实际Adapter/Driver `Attempt提交不可变Origin字节`。
    /// # Errors
    /// Capacity/backpressure/deadline errors retain all accepted physical holders.
    #[allow(clippy::too_many_arguments)]
    pub fn submit<
        const L: usize,
        const AT: usize,
        const AR: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, AT, AR, B>,
        driver: &mut impl TxDriver,
        link: LinkHandle,
        reliable: &mut ReliableOwner<T, RX, F>,
        h: ReliableHandle,
        now: u64,
    ) -> Result<()> {
        if self.running || self.stopped {
            return Err(Error::State);
        }
        let slot = self
            .attempts
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let bytes = reliable.copy_frame(h, &mut self.bytes)?;
        if bytes > adapter.link_mtu(link)? {
            return Err(Error::NoSpace);
        }
        let token = adapter.tx_reserve(link, u16::try_from(slot).map_err(|_| Error::Config)?)?;
        let attempt = match reliable.attempt_begin(h, now) {
            Ok(h) => h,
            Err(error) => {
                adapter.tx_cancel(token, driver)?;
                adapter.tx_retire(token)?;
                return Err(error);
            }
        };
        self.attempts[slot] = Some(Attempt {
            token,
            handle: attempt,
        });
        self.running = true;
        let submit = adapter.tx_submit(token, &self.bytes[..bytes], driver);
        self.running = false;
        let view = match submit {
            Ok(value) => value,
            Err(error) => {
                reliable.submitted(attempt, Submission::InDoubt, now)?;
                return Err(error);
            }
        };
        if view.state == TxState::NotSubmitted {
            reliable.submitted(attempt, Submission::NotSubmitted, now)?;
            adapter.tx_retire(token)?;
            self.attempts[slot] = None;
        } else {
            reliable.submitted(attempt, Submission::Submitted, now)?;
        }
        Ok(())
    }
    /// Explicit bounded physical retirement, including after stop.
    /// 显式有界物理退休，包括stop后。
    /// # Errors
    /// Ambiguous Driver completion retains both matching holders.
    pub fn retire<
        const L: usize,
        const AT: usize,
        const AR: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, AT, AR, B>,
        reliable: &mut ReliableOwner<T, RX, F>,
        budget: usize,
    ) -> Result<usize> {
        if self.running {
            return Err(Error::State);
        }
        let mut used = 0;
        for _ in 0..budget.min(H) {
            let index = self.retire_cursor;
            self.retire_cursor = (self.retire_cursor + 1) % H;
            let slot = &mut self.attempts[index];
            if let Some(attempt) = *slot {
                used += 1;
                let view = adapter.tx_view(attempt.token)?;
                if matches!(view.state, TxState::Completed | TxState::Cancelled) {
                    adapter.tx_retire(attempt.token)?;
                    reliable.attempt_complete(attempt.handle)?;
                    *slot = None;
                }
            }
        }
        Ok(used)
    }
    /// Explicit stop-time Driver cancellation retains every failed obligation for retry.
    /// 停止时显式Driver取消，失败义务全部保留重试。
    /// # Errors
    /// Driver uncertainty or invalid holder rejects without losing the Token.
    pub fn cancel_pending<
        const L: usize,
        const AT: usize,
        const AR: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, AT, AR, B>,
        driver: &mut impl TxDriver,
        reliable: &mut ReliableOwner<T, RX, F>,
        budget: usize,
    ) -> Result<usize> {
        if self.running || !self.stopped {
            return Err(Error::State);
        }
        let mut used = 0;
        for _ in 0..budget.min(H) {
            let index = self.retire_cursor;
            self.retire_cursor = (self.retire_cursor + 1) % H;
            if let Some(attempt) = self.attempts[index] {
                used += 1;
                let current = adapter.tx_view(attempt.token)?;
                let view = if matches!(
                    current.state,
                    TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
                ) {
                    current
                } else {
                    adapter.tx_cancel(attempt.token, driver)?
                };
                if matches!(
                    view.state,
                    TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
                ) {
                    adapter.tx_retire(attempt.token)?;
                    reliable.attempt_complete(attempt.handle)?;
                    self.attempts[index] = None;
                }
            }
        }
        Ok(used)
    }
    /// Physical obligations, not logical delivery status; 物理义务，不代表业务投递状态。
    #[must_use]
    pub fn outstanding(&self) -> usize {
        self.attempts.iter().flatten().count()
    }
}
