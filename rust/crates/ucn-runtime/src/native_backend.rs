//! Direct immutable Session Coordinator; no new Wire, Session or Replay FSM.
//! 固定直连Session协调器，复用已有安全与Adapter，不属于动态多跳后端。
use crate::runtime_common::GatedDriver;
use ucn_adapter::{AdapterOwner, RxView, TxDriver, TxState, TxToken};
use ucn_core::{EndpointHandler, Lifecycle, SendOptions, Target};
use ucn_native::{
    Backend, BackendMessage, BackendScope, Channel, Destination, PAYLOAD_BYTES, SecurityMinimum,
    Source,
};
use ucn_security::{
    AccessDirection, AccessRequest, Binding, CryptoProvider, CurrentFacts, Fingerprint,
    OpenDisposition, OpenPacketPlan, OriginContext, OriginCounterOwner, OriginSequenceOwner,
    PacketPlan, SecurityOwner, SecurityPacketWorkspace, SessionHandle,
};
use ucn_types::{
    AddressWidth, BindingGeneration, DeliveryGuarantee, Error, HeaderContract, HopLimit,
    HopProfile, InteractionRole, NodeAddress, OriginSecurity, OriginSequence, PayloadKind, Result,
    ServiceId,
};
use ucn_wire::CommonHeader;

/// Trusted product adapter for one durably ACTIVE, direct H0 Session/Service.
/// Owners/counter/Driver remain borrowed; constructing this never authenticates a peer.
pub struct NativeSessionBackend<
    'a,
    'g,
    'i,
    D,
    P,
    O,
    const S: usize,
    const R: usize,
    const T: usize,
    const X: usize,
    const B: usize,
> {
    security: &'a mut SecurityOwner<'g, S, R>,
    adapter: &'a mut AdapterOwner<'i, 1, T, X, B>,
    driver: &'a mut D,
    crypto: &'a mut P,
    counter: &'a mut O,
    session: SessionHandle,
    facts: CurrentFacts,
    fingerprint: Fingerprint,
    width: AddressWidth,
    service: ServiceId,
    local: Source,
    peer: Source,
    scope: BackendScope,
    lifecycle: Lifecycle,
    now: u64,
    tx: Option<TxToken>,
    deadline: u64,
    frame: [u8; B],
    frame_bytes: usize,
    raw: [u8; B],
    rx: Option<RxView>,
    ready: Option<BackendMessage>,
    workspace: SecurityPacketWorkspace<B>,
}
impl<
    'a,
    'g,
    'i,
    D: TxDriver,
    P: CryptoProvider,
    O: OriginSequenceOwner,
    const S: usize,
    const R: usize,
    const T: usize,
    const X: usize,
    const B: usize,
> NativeSessionBackend<'a, 'g, 'i, D, P, O, S, R, T, X, B>
{
    /// Borrow the unique existing Session/counter and Adapter; labels are not authorization.
    /// # Errors
    /// Inactive/mismatched Session, non-H0, capacity, fingerprint or Link rejects.
    #[allow(clippy::too_many_arguments)] // Explicit trusted product assembly, no hidden globals.
    pub fn new(
        security: &'a mut SecurityOwner<'g, S, R>,
        adapter: &'a mut AdapterOwner<'i, 1, T, X, B>,
        driver: &'a mut D,
        crypto: &'a mut P,
        counter: &'a mut O,
        session: SessionHandle,
        facts: CurrentFacts,
        fingerprint: Fingerprint,
        width: AddressWidth,
        service: ServiceId,
        local_label: u64,
        peer_label: u64,
    ) -> Result<Self> {
        let view = security.session_get(session, facts)?;
        if B < 96
            || width != security.address_width()
            || !adapter.rx_is_quiescent()
            || !adapter.tx_is_quiescent()
            || local_label == 0
            || peer_label == 0
            || view.hop_profile != HopProfile::H0
            || security.context_fingerprints(session, facts)?.0 != fingerprint
            || adapter.link_handle(0)?.instance_generation() != facts.link_generation.get()
        {
            return Err(Error::Config);
        }
        let inbound = AccessRequest {
            session,
            context_fingerprint: fingerprint,
            service,
            protocol_opcode: 0,
            direction: AccessDirection::Inbound,
        };
        if security.authorize(inbound, facts).is_err()
            && security
                .authorize(
                    AccessRequest {
                        direction: AccessDirection::Outbound,
                        ..inbound
                    },
                    facts,
                )
                .is_err()
        {
            return Err(Error::State);
        }
        let local = Source {
            address: facts.local.address,
            binding: facts.local.generation,
            principal_digest: local_label,
        };
        let peer = Source {
            address: facts.peer.address,
            binding: facts.peer.generation,
            principal_digest: peer_label,
        };
        Ok(Self {
            scope: BackendScope {
                instance: security.runtime_instance(),
                generation: view.session_generation.get(),
                slot: 1,
            },
            security,
            adapter,
            driver,
            crypto,
            counter,
            session,
            facts,
            fingerprint,
            width,
            service,
            local,
            peer,
            lifecycle: Lifecycle::Initialized,
            now: facts.now_us,
            tx: None,
            deadline: 0,
            frame: [0; B],
            frame_bytes: 0,
            raw: [0; B],
            rx: None,
            ready: None,
            workspace: SecurityPacketWorkspace::new(),
        })
    }
    fn current(&self, now: u64) -> Result<CurrentFacts> {
        if now < self.now
            || self.adapter.link_handle(0)?.instance_generation()
                != self.facts.link_generation.get()
        {
            return Err(Error::State);
        }
        self.adapter.link_mtu(self.adapter.link_handle(0)?)?;
        let facts = CurrentFacts {
            now_us: now,
            ..self.facts
        };
        self.security.session_get(self.session, facts)?;
        Ok(facts)
    }
    fn access(&self, direction: AccessDirection) -> AccessRequest {
        AccessRequest {
            session: self.session,
            context_fingerprint: self.fingerprint,
            service: self.service,
            protocol_opcode: 0,
            direction,
        }
    }
    fn identity(
        source: Binding,
        destination: Binding,
        service: ServiceId,
        sequence: OriginSequence,
    ) -> [u8; 46] {
        let mut b = [0; 46];
        b[..16].copy_from_slice(&source.principal);
        b[16..20].copy_from_slice(&source.generation.get().to_be_bytes());
        b[20..36].copy_from_slice(&destination.principal);
        b[36..40].copy_from_slice(&destination.generation.get().to_be_bytes());
        b[40..42].copy_from_slice(&service.get().to_be_bytes());
        b[42..].copy_from_slice(&sequence.get().to_be_bytes());
        b
    }
    fn submit_prepared(&mut self, token: TxToken) -> Result<()> {
        let mut driver = GatedDriver {
            driver: self.driver,
            gate: self.security.callback_gate(),
            owner: self.scope.instance,
        };
        match self
            .adapter
            .tx_submit(token, &self.frame[..self.frame_bytes], &mut driver)
        {
            Ok(_) => Ok(()),
            Err(e) if self.adapter.tx_view(token)?.state == TxState::InDoubt => {
                let _ = e;
                Ok(())
            }
            Err(e) => Err(e),
        }
    }
    fn decode_rx(&mut self, rx: RxView) -> Result<BackendMessage> {
        let facts = self.current(self.now)?;
        if rx.link != self.adapter.link_handle(0)? || rx.meta.timestamp_us > self.now {
            return Err(Error::State);
        }
        let n = 9 + 2 * self.width.bytes();
        if rx.frame_bytes < n + 16 {
            return Err(Error::Malformed);
        }
        let header = CommonHeader::decode(&self.raw[..rx.frame_bytes])?;
        if header.contract != HeaderContract::C1
            || header.delivery != DeliveryGuarantee::BestEffort
            || header.interaction != InteractionRole::OneWay
            || header.payload_kind != PayloadKind::Data
            || header.origin_security == OriginSecurity::O0
            || header.hop_limit.get() != 1
        {
            return Err(Error::Unsupported);
        }
        let sequence = OriginSequence::new(u32::from_be_bytes(
            self.raw[n - 4..n]
                .try_into()
                .map_err(|_| Error::Malformed)?,
        ))?;
        let identity = Self::identity(facts.peer, facts.local, self.service, sequence);
        let access = self.access(AccessDirection::Inbound);
        let mut payload = [0; PAYLOAD_BYTES];
        let opened = self.security.open_packet(
            self.crypto,
            self.session,
            facts,
            access,
            OpenPacketPlan {
                packet: &self.raw[..rx.frame_bytes],
                prefix_bytes: n,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: self.fingerprint,
                    sequence,
                },
                hop_profile: HopProfile::H0,
            },
            &mut self.workspace,
            &mut payload,
        )?;
        if opened.disposition != OpenDisposition::FreshAuthenticated {
            self.security.replay_abort(opened.replay)?;
            return Err(Error::Replay);
        }
        self.security.replay_commit(opened.replay, facts)?;
        Ok(BackendMessage {
            source: self.peer,
            scope: self.scope,
            receive_time_us: rx.meta.timestamp_us,
            origin_sequence: sequence.get(),
            service: self.service,
            traffic_class: header.traffic_class,
            protection: if header.origin_security == OriginSecurity::O2 {
                SecurityMinimum::Encrypted
            } else {
                SecurityMinimum::OriginAuth
            },
            payload_bytes: u16::try_from(opened.payload_bytes).map_err(|_| Error::NoSpace)?,
            payload,
        })
    }
}
impl<
    D: TxDriver,
    P: CryptoProvider,
    O: OriginSequenceOwner,
    const S: usize,
    const R: usize,
    const T: usize,
    const X: usize,
    const B: usize,
> Backend for NativeSessionBackend<'_, '_, '_, D, P, O, S, R, T, X, B>
{
    fn supports(&self, minimum: SecurityMinimum) -> bool {
        self.security
            .session_get(self.session, self.facts)
            .is_ok_and(|v| match minimum {
                SecurityMinimum::StaticTrusted => false,
                SecurityMinimum::OriginAuth => v.origin_security != OriginSecurity::O0,
                SecurityMinimum::Encrypted => v.origin_security == OriginSecurity::O2,
            })
    }
    fn local(&self) -> Destination {
        Destination {
            address: self.local.address,
            binding: self.local.binding,
        }
    }
    fn binding(&self, address: NodeAddress) -> Option<BindingGeneration> {
        if address == self.local.address {
            Some(self.local.binding)
        } else if address == self.peer.address {
            Some(self.peer.binding)
        } else {
            None
        }
    }
    fn principal(&self, address: NodeAddress) -> Option<u64> {
        if address == self.local.address {
            Some(self.local.principal_digest)
        } else if address == self.peer.address {
            Some(self.peer.principal_digest)
        } else {
            None
        }
    }
    fn trusted(&self) -> bool {
        false
    }
    fn lifecycle(&self) -> Lifecycle {
        self.lifecycle
    }
    fn install(&mut self, channels: &[Channel], _: &'static dyn EndpointHandler) -> Result<()> {
        if self.lifecycle != Lifecycle::Initialized
            || channels
                .iter()
                .any(|c| c.service != self.service || !self.supports(c.security))
        {
            return Err(Error::Unsupported);
        }
        let mtu = self.adapter.link_mtu(self.adapter.link_handle(0)?)?;
        if channels
            .iter()
            .any(|c| 9 + 2 * self.width.bytes() + 16 + usize::from(c.payload_bytes) > mtu)
        {
            return Err(Error::NoSpace);
        }
        Ok(())
    }
    fn start(&mut self, now: u64) -> Result<()> {
        if self.lifecycle != Lifecycle::Initialized {
            return Err(Error::State);
        }
        self.current(now)?;
        self.adapter.try_set_rx_enabled(true)?;
        self.now = now;
        self.lifecycle = Lifecycle::Running;
        Ok(())
    }
    fn stop(&mut self) -> Result<()> {
        self.lifecycle = Lifecycle::Stopping;
        self.ready = None;
        Ok(())
    }
    fn send(
        &mut self,
        target: Target,
        payload: &[u8],
        options: SendOptions,
        now: u64,
    ) -> Result<()> {
        if self.lifecycle != Lifecycle::Running {
            return Err(Error::State);
        }
        if self.tx.is_some() {
            return Err(Error::NoSpace);
        }
        if target.address != self.peer.address
            || target.binding_generation != self.peer.binding
            || target.service_id != self.service
            || payload.len() > PAYLOAD_BYTES
            || options.absolute_deadline_us <= now
        {
            return Err(Error::Access);
        }
        let facts = self.current(now)?;
        self.now = now;
        let access = self.access(AccessDirection::Outbound);
        self.security.authorize(access, facts)?;
        let view = self.security.session_get(self.session, facts)?;
        let frame_bytes = 9 + 2 * self.width.bytes() + payload.len() + 16;
        if frame_bytes > self.adapter.link_mtu(self.adapter.link_handle(0)?)? {
            return Err(Error::NoSpace);
        }
        let sequence = self.counter.preview()?;
        let n = 9 + 2 * self.width.bytes();
        let w = self.width.bytes();
        let mut prefix = [0; 17];
        CommonHeader {
            contract: HeaderContract::C1,
            origin_security: view.origin_security,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::OneWay,
            payload_kind: PayloadKind::Data,
            traffic_class: options.traffic_class,
            hop_limit: HopLimit::new(1)?,
        }
        .encode(&mut prefix);
        prefix[3..3 + w].copy_from_slice(&facts.local.address.get().to_be_bytes()[4 - w..]);
        prefix[3 + w..3 + 2 * w].copy_from_slice(&facts.peer.address.get().to_be_bytes()[4 - w..]);
        prefix[n - 6..n - 4].copy_from_slice(&self.service.get().to_be_bytes());
        prefix[n - 4..n].copy_from_slice(&sequence.get().to_be_bytes());
        let identity = Self::identity(facts.local, facts.peer, self.service, sequence);
        let token = self.adapter.tx_reserve(self.adapter.link_handle(0)?, 0)?;
        self.tx = Some(token);
        self.frame_bytes = 0;
        self.deadline = options.absolute_deadline_us;
        let protected = self.security.protect_packet(
            self.crypto,
            OriginCounterOwner::Sequence(self.counter),
            self.session,
            facts,
            access,
            PacketPlan {
                prefix: &prefix[..n],
                payload,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: self.fingerprint,
                    sequence,
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut self.workspace,
            &mut self.frame,
        );
        match protected {
            Ok(n) => self.frame_bytes = n,
            Err(e) => {
                let mut driver = GatedDriver {
                    driver: self.driver,
                    gate: self.security.callback_gate(),
                    owner: self.scope.instance,
                };
                if self.adapter.tx_cancel(token, &mut driver).is_ok()
                    && self.adapter.tx_retire(token).is_ok()
                {
                    self.tx = None;
                }
                return Err(e);
            }
        }
        // Once copied/protected, the actor owns the frame on transient submit
        // failure; never make the facade duplicate a locally accepted request.
        let _ = self.submit_prepared(token);
        Ok(())
    }
    fn step(&mut self, now: u64, budget: u16) -> Result<()> {
        if budget == 0 || now < self.now {
            return Err(Error::State);
        }
        self.now = now;
        if let Some(token) = self.tx {
            let state = self.adapter.tx_view(token)?.state;
            if matches!(state, TxState::Completed | TxState::Cancelled) {
                self.adapter.tx_retire(token)?;
                self.tx = None;
            } else if self.frame_bytes == 0
                || self.lifecycle == Lifecycle::Stopping
                || now >= self.deadline
                || self.current(now).is_err()
            {
                let mut driver = GatedDriver {
                    driver: self.driver,
                    gate: self.security.callback_gate(),
                    owner: self.scope.instance,
                };
                let view = self.adapter.tx_cancel(token, &mut driver)?;
                if matches!(
                    view.state,
                    TxState::Cancelled | TxState::Completed | TxState::NotSubmitted
                ) {
                    self.adapter.tx_retire(token)?;
                    self.tx = None;
                }
            } else if matches!(state, TxState::NotSubmitted | TxState::Reserved) {
                self.submit_prepared(token)?;
            }
        }
        if self.lifecycle == Lifecycle::Stopping {
            self.adapter.try_set_rx_enabled(false)?;
            for _ in 0..budget {
                let rx = match self.rx {
                    Some(rx) => rx,
                    None => match self.adapter.rx_claim(&mut self.raw) {
                        Ok(rx) => rx,
                        Err(Error::NotFound) => break,
                        Err(e) => return Err(e),
                    },
                };
                self.rx = Some(rx);
                self.adapter.rx_retire(rx.token)?;
                self.rx = None;
            }
            if self.tx.is_none() && self.adapter.tx_is_quiescent() && self.adapter.rx_is_quiescent()
            {
                self.lifecycle = Lifecycle::Quiescent;
            }
        }
        Ok(())
    }
    fn poll(&mut self) -> Result<BackendMessage> {
        if self.lifecycle != Lifecycle::Running {
            return Err(Error::State);
        }
        if self.rx.is_none() {
            let rx = self.adapter.rx_claim(&mut self.raw)?;
            self.rx = Some(rx);
            self.ready = self.decode_rx(rx).ok();
        }
        let rx = self.rx.ok_or(Error::State)?;
        self.adapter.rx_retire(rx.token)?;
        self.rx = None;
        self.ready.take().ok_or(Error::Security)
    }
    fn validate(
        &self,
        scope: BackendScope,
        source: Source,
        channel: &Channel,
        now: u64,
    ) -> Result<[u8; 16]> {
        let local = scope == BackendScope::LOCAL;
        if self.lifecycle != Lifecycle::Running
            || channel.service != self.service
            || !self.supports(channel.security)
            || (!local && scope != self.scope)
            || source != if local { self.local } else { self.peer }
        {
            return Err(Error::Access);
        }
        let facts = self.current(now)?;
        self.security.authorize(
            self.access(if local {
                AccessDirection::Outbound
            } else {
                AccessDirection::Inbound
            }),
            facts,
        )?;
        Ok(if local { [0; 16] } else { facts.peer.principal })
    }
}
