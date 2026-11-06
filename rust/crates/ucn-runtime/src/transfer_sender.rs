//! Bounded autonomous C1 Transfer sender; 有界自主C1分片发送。
//! Parent authority, semantic SACK and physical holders remain distinct.
//! 父许可、语义SACK和物理Holder严格分离。
use crate::{ControlCoordinator, ControlHandle, JournalPeer, ParentCoordinator, SourceParentProof};
use ucn_adapter::{AdapterOwner, TxDriver, TxState, TxToken};
use ucn_digest::DigestWorkspace;
use ucn_security::{
    AccessDirection, AccessRequest, C0TransactionOwner, CryptoProvider, OriginContext,
    OriginCounterOwner, OriginSequenceOwner, PacketPlan, SecurityOwner, SecurityPacketWorkspace,
    SenderDirection,
};
use ucn_transport::{
    Submission, TransferHandle, TransferOwner, TransferPermit, TransferPhase, TransferSetup,
    TransferView,
};
use ucn_types::{
    DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile, InteractionRole, PayloadKind,
    ProtocolOpcode, Result, ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;

/// One TX-only fixed window; no RX buffer or unauthenticated feedback API.
/// 一个纯TX固定窗口，无RX缓存、无未认证反馈入口。
pub struct TransferSender<const M: usize, const N: usize, const W: usize> {
    owner: u32,
    transfer: TransferOwner<1, 0, M, N, 0>,
    handle: Option<TransferHandle>,
    retry: u64,
    max_attempts: u16,
    last_now: u64,
    stopped: bool,
    cursor: usize,
    setup: [u8; W],
    setup_length: usize,
    setup_attempts: u16,
    setup_due: u64,
    fragments: [[u8; W]; N],
    lengths: [usize; N],
    attempts: [u16; N],
    due: [u64; N],
    prepared: [bool; N],
    payload: [u8; W],
    token: Option<TxToken>,
    peer: Option<JournalPeer>,
}
impl<const M: usize, const N: usize, const W: usize> TransferSender<M, N, W> {
    /// Const fixed storage; compile-time window limits ordinary memory use.
    /// const固定存储，编译期窗口限制普通内存成本。
    /// # Errors
    /// Invalid capacity/retry policy rejects.
    pub const fn new(owner: u32, retry: u64, max_attempts: u16) -> Result<Self> {
        if owner == 0
            || N == 0
            || N > 32
            || W < 128
            || W > 4096
            || retry == 0
            || retry > i64::MAX as u64
            || max_attempts == 0
        {
            return Err(Error::Config);
        }
        let transfer = match TransferOwner::new(owner, 1000, 0xff00) {
            Ok(v) => v,
            Err(e) => return Err(e),
        };
        Ok(Self {
            owner,
            transfer,
            handle: None,
            retry,
            max_attempts,
            last_now: 0,
            stopped: false,
            cursor: 0,
            setup: [0; W],
            setup_length: 0,
            setup_attempts: 0,
            setup_due: 0,
            fragments: [[0; W]; N],
            lengths: [0; N],
            attempts: [0; N],
            due: [0; N],
            prepared: [false; N],
            payload: [0; W],
            token: None,
            peer: None,
        })
    }
    fn current<const S: usize, const R: usize>(
        &self,
        parent: &ParentCoordinator,
        proof: &SourceParentProof<'_>,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
    ) -> Result<()> {
        if peer.facts.now_us == 0 || peer.facts.now_us < self.last_now {
            return Err(Error::Argument);
        }
        parent.online_source(proof, peer, security)?;
        if let Some(old) = self.peer {
            if old.link != peer.link
                || old.session != peer.session
                || old.fingerprint != peer.fingerprint
                || old.facts.local != peer.facts.local
                || old.facts.peer != peer.facts.peer
                || old.facts.policy_generation != peer.facts.policy_generation
                || old.facts.link_generation != peer.facts.link_generation
            {
                return Err(Error::Access);
            }
        }
        Ok(())
    }
    /// Consume one actually durable linear ID after online publication.
    /// 线上发布后消费实际耐久线性ID；无Provider或Driver副作用。
    /// # Errors
    /// Wrong permit/Parent, capacity or digest rejects. Rejected IDs remain burned.
    #[allow(clippy::too_many_arguments, clippy::needless_pass_by_value)]
    pub fn begin<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        parent: &ParentCoordinator,
        proof: &SourceParentProof<'_>,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
        adapter: &AdapterOwner<'_, L, T, RX, W>,
        permit: TransferPermit,
        setup: TransferSetup,
        payload: &[u8],
    ) -> Result<()> {
        if self.stopped || self.handle.is_some() {
            return Err(Error::State);
        }
        self.current(parent, proof, peer, security)?;
        let width = security.address_width().bytes();
        if permit.parent() != parent.online_source(proof, peer, security)?
            || setup.delivery != DeliveryGuarantee::Reliable
            || setup.interaction != InteractionRole::OneWay
            || setup.fragment_bytes as usize + 37 + 2 * width > adapter.link_mtu(peer.link)?
            || 95 + 2 * width > adapter.link_mtu(peer.link)?
        {
            return Err(Error::Argument);
        }
        let handle = self.transfer.begin(
            permit,
            setup,
            payload,
            peer.facts.now_us,
            &mut DigestWorkspace::new(),
        )?;
        self.handle = Some(handle);
        self.peer = Some(peer);
        self.last_now = peer.facts.now_us;
        Ok(())
    }
    /// Read semantic progress; physical retirement may still be outstanding.
    /// 只读语义进度，物理退休可能未完成。
    /// # Errors
    /// No active transfer rejects.
    pub fn view(&self) -> Result<TransferView> {
        self.transfer.view(self.handle.ok_or(Error::State)?)
    }
    /// Apply only a real opaque authenticated queue command; no DTO feedback bypass.
    /// 仅接受真实认证队列命令，无DTO反馈旁路。
    /// # Errors
    /// Current Parent/authority, exact key and deadline errors preserve sender.
    #[allow(clippy::too_many_arguments)]
    pub fn feedback<const S: usize, const R: usize, const Q: usize, const B: usize>(
        &mut self,
        parent: &ParentCoordinator,
        proof: &SourceParentProof<'_>,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
        queue: &mut ControlCoordinator<Q, B>,
        command: ControlHandle,
    ) -> Result<()> {
        if self.stopped {
            return Err(Error::State);
        }
        self.current(parent, proof, peer, security)?;
        let h = queue.dispatch_transfer_feedback(
            command,
            proof.allocator(),
            security,
            peer.session,
            peer.facts,
            &mut self.transfer,
            &mut DigestWorkspace::new(),
        )?;
        if Some(h) != self.handle {
            return Err(Error::State);
        }
        self.last_now = peer.facts.now_us;
        Ok(())
    }
    fn prefix(
        peer: JournalPeer,
        width: usize,
        sequence: u32,
        setup: TransferSetup,
        origin: ucn_types::OriginSecurity,
    ) -> Result<([u8; 17], usize, [u8; 46])> {
        let n = 9 + 2 * width;
        let mut header = [0; 17];
        let mut identity = [0; 46];
        CommonHeader {
            contract: HeaderContract::C1,
            origin_security: origin,
            delivery: DeliveryGuarantee::Reliable,
            interaction: setup.interaction,
            payload_kind: PayloadKind::Transfer,
            traffic_class: TrafficClass::Q3,
            hop_limit: HopLimit::new(1)?,
        }
        .encode(&mut header);
        header[3..3 + width]
            .copy_from_slice(&peer.facts.local.address.get().to_be_bytes()[4 - width..]);
        header[3 + width..3 + 2 * width]
            .copy_from_slice(&peer.facts.peer.address.get().to_be_bytes()[4 - width..]);
        header[n - 6..n - 4].copy_from_slice(&setup.service.to_be_bytes());
        header[n - 4..n].copy_from_slice(&sequence.to_be_bytes());
        identity[..16].copy_from_slice(&peer.facts.local.principal);
        identity[16..20].copy_from_slice(&peer.facts.local.generation.get().to_be_bytes());
        identity[20..36].copy_from_slice(&peer.facts.peer.principal);
        identity[36..40].copy_from_slice(&peer.facts.peer.generation.get().to_be_bytes());
        identity[40..].copy_from_slice(&header[n - 6..n]);
        Ok((header, n, identity))
    }
    /// One bounded send opportunity; cached Setup/fragments retry with identical bytes.
    /// 一次有界发送机会，缓存Setup/分片按不变字节重试。
    /// # Errors
    /// Current permission, RTO/credit/deadline or unknown physical disposition retains obligations.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn send_next<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        parent: &ParentCoordinator,
        proof: &SourceParentProof<'_>,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        transaction: &mut impl C0TransactionOwner,
        sequence: &mut impl OriginSequenceOwner,
        adapter: &mut AdapterOwner<'_, L, T, RX, W>,
        driver: &mut impl TxDriver,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<bool> {
        if self.stopped {
            return Err(Error::State);
        }
        self.current(parent, proof, peer, security)?;
        self.last_now = peer.facts.now_us;
        if let Some(handle) = self.handle {
            self.transfer.step(peer.facts.now_us, 1)?;
            if matches!(self.transfer.view(handle)?.phase, TransferPhase::Failed) {
                return Err(Error::Timeout);
            }
        }
        if self.token.is_some() {
            return Ok(false);
        }
        let handle = self.handle.ok_or(Error::State)?;
        let view = self.transfer.view(handle)?;
        let now = peer.facts.now_us;
        if now >= view.deadline_us {
            return Err(Error::Timeout);
        }
        if !matches!(
            view.phase,
            TransferPhase::SetupPending | TransferPhase::Sending
        ) {
            return Ok(false);
        }
        let width = security.address_width().bytes();
        let origin = parent.online_source(proof, peer, security)?.security.origin;
        let mut chosen = None;
        if view.phase == TransferPhase::SetupPending {
            if now < self.setup_due || self.setup_attempts >= self.max_attempts {
                return Ok(false);
            }
            if self.setup_length == 0 {
                let mut body = [0; 54];
                let b = ucn_transport::encode_setup(view.setup, &mut body)?;
                let tx = transaction.preview()?;
                let n = 25 + 2 * width;
                let mut header = [0; 33];
                CommonHeader {
                    contract: HeaderContract::C0,
                    origin_security: origin,
                    delivery: DeliveryGuarantee::BestEffort,
                    interaction: InteractionRole::Request,
                    payload_kind: PayloadKind::Control,
                    traffic_class: TrafficClass::Q1,
                    hop_limit: HopLimit::new(1)?,
                }
                .encode(&mut header);
                header[3..7].copy_from_slice(&security.realm().get().to_be_bytes());
                header[7..7 + width]
                    .copy_from_slice(&peer.facts.local.address.get().to_be_bytes()[4 - width..]);
                header[7 + width..7 + 2 * width]
                    .copy_from_slice(&peer.facts.peer.address.get().to_be_bytes()[4 - width..]);
                header[7 + 2 * width..11 + 2 * width]
                    .copy_from_slice(&peer.facts.local.generation.get().to_be_bytes());
                header[11 + 2 * width..15 + 2 * width]
                    .copy_from_slice(&peer.facts.peer.generation.get().to_be_bytes());
                header[n - 10..n - 2].copy_from_slice(&tx.get().to_be_bytes());
                header[n - 2..n]
                    .copy_from_slice(&u16::from(ProtocolOpcode::TransferSetup).to_be_bytes());
                self.setup_length = security.protect_packet(
                    crypto,
                    OriginCounterOwner::Transaction(transaction),
                    peer.session,
                    peer.facts,
                    AccessRequest {
                        session: peer.session,
                        context_fingerprint: peer.fingerprint,
                        service: ServiceId::new(view.setup.service)?,
                        protocol_opcode: u16::from(ProtocolOpcode::TransferSetup),
                        direction: AccessDirection::Outbound,
                    },
                    PacketPlan {
                        prefix: &header[..n],
                        payload: &body[..b],
                        canonical_identity: &header[3..n],
                        origin_context: OriginContext::C0 {
                            fingerprint: peer.fingerprint,
                            transaction_id: tx,
                            opcode: ProtocolOpcode::TransferSetup,
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
                    &mut self.setup,
                )?;
            }
        } else {
            for _ in 0..usize::from(view.setup.fragments) {
                let i = self.cursor % usize::from(view.setup.fragments);
                self.cursor = (i + 1) % usize::from(view.setup.fragments);
                let needs = self
                    .transfer
                    .fragment_needed(handle, u16::try_from(i).map_err(|_| Error::Config)?)?;
                let terminal_probe = view.completed_fragments == usize::from(view.setup.fragments)
                    && i + 1 == usize::from(view.setup.fragments);
                if (needs || terminal_probe)
                    && now >= self.due[i]
                    && self.attempts[i] < self.max_attempts
                {
                    chosen = Some(i);
                    break;
                }
            }
            let Some(i) = chosen else {
                return Ok(false);
            };
            if self.lengths[i] == 0
                || (self
                    .transfer
                    .fragment_needed(handle, u16::try_from(i).map_err(|_| Error::Config)?)?
                    && !self.prepared[i])
            {
                let next_sequence = if self.lengths[i] == 0 {
                    Some(sequence.preview()?)
                } else {
                    None
                };
                let n = self.transfer.fragment(
                    handle,
                    u16::try_from(i).map_err(|_| Error::Config)?,
                    now,
                    &mut self.payload,
                )?;
                self.prepared[i] = true;
                if self.lengths[i] == 0 {
                    let seq = next_sequence.ok_or(Error::State)?;
                    let (header, h, identity) =
                        Self::prefix(peer, width, seq.get(), view.setup, origin)?;
                    let sealed = security.protect_packet(
                        crypto,
                        OriginCounterOwner::Sequence(sequence),
                        peer.session,
                        peer.facts,
                        AccessRequest {
                            session: peer.session,
                            context_fingerprint: peer.fingerprint,
                            service: ServiceId::new(view.setup.service)?,
                            protocol_opcode: 0,
                            direction: AccessDirection::Outbound,
                        },
                        PacketPlan {
                            prefix: &header[..h],
                            payload: &self.payload[..n],
                            canonical_identity: &identity,
                            origin_context: OriginContext::Sequenced {
                                contract: HeaderContract::C1,
                                fingerprint: peer.fingerprint,
                                sequence: seq,
                            },
                            hop_profile: HopProfile::H0,
                        },
                        None,
                        scratch,
                        &mut self.fragments[i],
                    );
                    match sealed {
                        Ok(n) => self.lengths[i] = n,
                        Err(e) => {
                            self.transfer.fragment_disposition(
                                handle,
                                u16::try_from(i).map_err(|_| Error::Config)?,
                                Submission::NotSubmitted,
                            )?;
                            self.prepared[i] = false;
                            return Err(e);
                        }
                    }
                }
            }
        }
        let opcode = if chosen.is_none() {
            u16::from(ProtocolOpcode::TransferSetup)
        } else {
            0
        };
        security.authorize(
            AccessRequest {
                session: peer.session,
                context_fingerprint: peer.fingerprint,
                service: ServiceId::new(view.setup.service)?,
                protocol_opcode: opcode,
                direction: AccessDirection::Outbound,
            },
            peer.facts,
        )?;
        let due = now.checked_add(self.retry).ok_or(Error::Exhausted)?;
        if security.callback_gate().is_active() {
            return Err(Error::State);
        }
        let token = adapter.tx_reserve(peer.link, 3)?;
        self.token = Some(token);
        let (bytes, length) = if let Some(i) = chosen {
            self.attempts[i] += 1;
            self.due[i] = due;
            (&self.fragments[i], self.lengths[i])
        } else {
            self.setup_attempts += 1;
            self.setup_due = due;
            (&self.setup, self.setup_length)
        };
        let mut gated = crate::runtime_common::GatedDriver {
            driver,
            gate: security.callback_gate(),
            owner: self.owner,
        };
        let submitted = adapter.tx_submit(token, &bytes[..length], &mut gated);
        if let Some(i) = chosen {
            if self.prepared[i] {
                let disposition = match adapter.tx_view(token)?.state {
                    TxState::NotSubmitted | TxState::Reserved => Submission::NotSubmitted,
                    TxState::InDoubt => Submission::InDoubt,
                    _ => Submission::Submitted,
                };
                self.transfer.fragment_disposition(
                    handle,
                    u16::try_from(i).map_err(|_| Error::Config)?,
                    disposition,
                )?;
                self.prepared[i] = false;
            }
        }
        self.last_now = now;
        submitted?;
        Ok(true)
    }
    /// Stop rejects new work but never returns a remote no-effect certificate.
    /// 停止拒绝新工作，不返回远端无副作用证明。
    pub fn stop(&mut self) {
        self.stopped = true;
        if let Some(h) = self.handle {
            let _ = self.transfer.abort(h);
        }
    }
    /// One exact retirement; UNKNOWN is not made free by stop.
    /// 一次精确退休，UNKNOWN不因停止变为空闲。
    /// # Errors
    /// Unresolved physical facts preserve all storage.
    pub fn retire<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        security: &SecurityOwner<'_, S, R>,
        adapter: &mut AdapterOwner<'_, L, T, RX, W>,
        driver: &mut impl TxDriver,
    ) -> Result<bool> {
        if let Some(token) = self.token {
            let state = adapter.tx_view(token)?.state;
            if self.stopped && matches!(state, TxState::Submitted | TxState::Reserved) {
                let mut gated = crate::runtime_common::GatedDriver {
                    driver,
                    gate: security.callback_gate(),
                    owner: self.owner,
                };
                adapter.tx_cancel(token, &mut gated)?;
            }
            if !matches!(
                adapter.tx_view(token)?.state,
                TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
            ) {
                return Ok(false);
            }
            adapter.tx_retire(token)?;
            self.token = None;
            return Ok(true);
        }
        if self.stopped {
            if let Some(h) = self.handle {
                for i in 0..N {
                    if self.prepared[i] {
                        self.transfer.fragment_disposition(
                            h,
                            u16::try_from(i).map_err(|_| Error::Config)?,
                            Submission::NotSubmitted,
                        )?;
                        self.prepared[i] = false;
                    }
                }
                self.transfer.retire_tx(h)?;
                self.handle = None;
                self.setup.fill(0);
                for packet in &mut self.fragments {
                    packet.fill(0);
                }
                return Ok(true);
            }
        }
        Ok(false)
    }
    /// This component's quiescence, not whole Adapter/Peer quiescence.
    /// 本组件静止，非整个Adapter或Peer静止。
    #[must_use]
    pub const fn quiescent(&self) -> bool {
        self.stopped && self.token.is_none() && self.handle.is_none()
    }
}
