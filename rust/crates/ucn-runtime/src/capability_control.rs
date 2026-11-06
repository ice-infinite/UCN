//! One current admitted Peer -> HELLO/QUERY/ADVERTISE/SELECT/ACK, actual Security/Adapter.
//! 一个当前已准入Peer的实际能力协商；不冒充完整JOIN/多跳Route/Flow Root。
use crate::runtime_common::GatedDriver;
use crate::{
    ControlAdmission, ControlBody, ControlCommand, ControlCoordinator, ControlReplies, JournalPeer,
    ReplyHandle, ReplyRequest,
};
use ucn_adapter::{AdapterOwner, TxDriver, TxState, TxToken};
use ucn_capability::{
    CapabilityConfig, CapabilityOwner, CapabilityQuery, CapabilityRecord, CapabilitySummary,
    PeerCapabilityRef, ProfileAck, ProfileRequirements, ProfileSelect, SelectContext,
    capability_digest, encode_capability_query, encode_capability_summary, encode_profile_select,
    select_transcript,
};
use ucn_digest::DigestWorkspace;
use ucn_owner::CallbackGate;
use ucn_security::{
    AccessDirection, AccessRequest, C0TransactionOwner, CryptoProvider, HopSequenceOwner,
    OpenPacketPlan, OriginContext, OriginCounterOwner, PacketPlan, SecurityOwner,
    SecurityPacketWorkspace, SenderDirection,
};
use ucn_types::{
    C0TransactionId, DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile,
    InteractionRole, OriginSecurity, PayloadKind, ProtocolOpcode, Result, ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;
mod fragment_rx;

// Private Coordinator-side read adapter, not a forgeable public parent-proof DTO.
// Coordinator私有只读适配，不是可由调用方伪造的父域证明DTO。
#[cfg(feature = "bootstrap-join")]
pub(crate) trait CapabilityIdentity {
    fn current(
        &self,
        binding: ucn_identity::BindingHandle,
        expected: ucn_security::Binding,
        now: u64,
    ) -> Result<()>;
}
#[cfg(feature = "bootstrap-join")]
impl<const N: usize> CapabilityIdentity for ucn_identity::IdentityOwner<'_, N> {
    fn current(
        &self,
        binding: ucn_identity::BindingHandle,
        expected: ucn_security::Binding,
        now: u64,
    ) -> Result<()> {
        let authority = self.authority_get(now)?;
        let view = self.binding_get(binding, now)?;
        let cert = view.certificate();
        if cert.binding.address != expected.address
            || cert.binding.generation != expected.generation
            || cert.binding.principal.bytes() != expected.principal
            || cert.authority_generation != authority.authority_generation
            || cert.authority_principal != authority.authority_principal
            || cert.binding.realm != authority.realm
        {
            return Err(Error::Security);
        }
        Ok(())
    }
}

/// Fixed handshake role; 固定握手角色。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CapabilityRole {
    /// Initiates authenticated HELLO/SELECT; 发起认证HELLO/SELECT。
    Initiator,
    /// Queries record and ACKs exact selection; 查询记录并确认精确选择。
    Responder,
}
/// Logical phase, never an ACL/Authority certificate; 逻辑阶段，不是权限证书。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CapabilityPhase {
    /// Source has not started, target awaits HELLO; 源未启动/目标待HELLO。
    Idle,
    /// Source HELLO awaits peer record; 源HELLO待对端记录。
    Discovering,
    /// Target QUERY is pending; 目标查询中。
    Querying,
    /// Source SELECT awaits exact current ACK; 源SELECT待精确当前ACK。
    Selecting,
    /// Target has committed exact selection and retains ACK; 目标已接纳精确选择并保留ACK。
    Confirmed,
    /// Scope invalidated irreversibly; 本实例域不可逆失效。
    Fenced,
    /// No new work, local retirement remains; 不接新工作，仍允许退休。
    Stopped,
}
struct Request<const B: usize> {
    op: ProtocolOpcode,
    body: [u8; 59],
    body_bytes: usize,
    frame: [u8; B],
    frame_bytes: usize,
    transaction: u64,
    token: Option<TxToken>,
    sent: bool,
    attempts: u16,
    due: u64,
}
/// Fixed current admitted Session and one dedicated Adapter; no caller-authenticated DTO.
/// 固定当前已准入会话与专属Adapter，不接受调用方伪造认证DTO。
pub struct CapabilityRuntime<'a, const BYTES: usize> {
    owner: u32,
    role: CapabilityRole,
    phase: CapabilityPhase,
    gate: &'a CallbackGate,
    peer: JournalPeer,
    runtime: u32,
    key_generations: (u32, u32),
    origin: OriginSecurity,
    hop: HopProfile,
    minimum_frame: u32,
    #[cfg(feature = "bootstrap-join")]
    identity: Option<(&'a dyn CapabilityIdentity, ucn_identity::BindingHandle)>,
    service: ServiceId,
    requirements: ProfileRequirements,
    capabilities: CapabilityOwner<1>,
    reference: PeerCapabilityRef,
    lifetime: u64,
    retry: u64,
    max_attempts: u16,
    deadline: u64,
    last_now: u64,
    summary: Option<CapabilitySummary>,
    selected: Option<ProfileSelect>,
    transcript: Option<[u8; 16]>,
    request: Option<Request<BYTES>>,
    pending_select: bool,
    request_observed: bool,
    control: ControlCoordinator<1, BYTES>,
    replies: ControlReplies<3, BYTES>,
    raw: [u8; BYTES],
    fragments: fragment_rx::Assembly,
    cursor: usize,
    retire_cursor: usize,
}
impl<'a, const B: usize> CapabilityRuntime<'a, B> {
    /// Bind actual Security and Adapter, trusted local immutable Record/policy and fixed budgets.
    /// 绑定实际Security/Adapter、本机可信不可变Record/策略与固定预算。
    /// # Errors
    /// Missing current Session, invalid local capacity, MTU or callback scope rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn new<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        owner: u32,
        role: CapabilityRole,
        peer: JournalPeer,
        security: &SecurityOwner<'a, S, R>,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        local: CapabilityRecord,
        requirements: ProfileRequirements,
        service: ServiceId,
        lifetime: u64,
        retry: u64,
        max_attempts: u16,
        capability_lease: u64,
        discovery_lease: u64,
    ) -> Result<Self> {
        if owner == 0
            || owner.checked_add(2).is_none()
            || B < 128
            || B > 4096
            || lifetime == 0
            || lifetime > i64::MAX as u64
            || retry == 0
            || retry > lifetime
            || max_attempts == 0
            || requirements.minimum_rx_window == 0
            || requirements.minimum_concurrent_transfers == 0
            || security.callback_gate().is_active()
        {
            return Err(Error::Config);
        }
        let link_id = peer.link.slot().checked_add(1).ok_or(Error::Config)?;
        let authenticated = security.authenticated_peer_view(peer.session, peer.facts, link_id)?;
        let view = security.session_get(peer.session, peer.facts)?;
        let local_mtu = adapter.link_mtu(peer.link)?;
        // Largest single request is SELECT59. Oversized Record/Ack use two authenticated parts.
        // 最大单帧请求SELECT59；放不下的Record/Ack使用两片认证正文。
        let minimum_frame = 25 + 2 * security.address_width().bytes() + 91; // ACK75+16 = SELECT59+32.
        if !matches!(view.hop_profile, HopProfile::H0 | HopProfile::H1)
            || view.hop_profile == HopProfile::H1 && max_attempts < 2
            || view.origin_security == OriginSecurity::O0
            || peer.facts.now_us == 0
            || peer.facts.link_generation.get() != peer.link.instance_generation()
            || local.link.link_instance_generation != peer.link.instance_generation()
            || usize::try_from(local.link.link_frame_mtu).map_err(|_| Error::Config)? > local_mtu
            || minimum_frame
                > local_mtu.min(B).min(F).min(
                    usize::try_from(local.link.processing_frame_mtu).map_err(|_| Error::Config)?,
                )
        {
            return Err(Error::Config);
        }
        let capabilities = CapabilityOwner::new(CapabilityConfig {
            runtime_instance: security.runtime_instance(),
            security_owner_instance: authenticated.security_owner_instance(),
            realm: security.realm(),
            local_record: local,
            capability_lease_us: capability_lease,
            discovery_lease_us: discovery_lease,
        })?;
        let reference = PeerCapabilityRef {
            runtime_instance: authenticated.runtime_instance(),
            security_owner_instance: authenticated.security_owner_instance(),
            realm: authenticated.realm(),
            principal: authenticated.peer().principal,
            binding: authenticated.peer(),
            session_generation: authenticated.session_generation().get(),
            ingress_link_id: link_id,
            ingress_link_generation: authenticated.ingress_link_generation().get(),
        };
        Ok(Self {
            owner,
            role,
            phase: CapabilityPhase::Idle,
            gate: security.callback_gate(),
            peer,
            runtime: security.runtime_instance(),
            key_generations: (
                security.outbound_key_generation(peer.session, peer.facts)?,
                security.inbound_key_generation(peer.session, peer.facts)?,
            ),
            origin: view.origin_security,
            hop: view.hop_profile,
            minimum_frame: u32::try_from(minimum_frame).map_err(|_| Error::Config)?,
            #[cfg(feature = "bootstrap-join")]
            identity: None,
            service,
            requirements,
            capabilities,
            reference,
            lifetime,
            retry,
            max_attempts,
            deadline: 0,
            last_now: peer.facts.now_us,
            summary: None,
            selected: None,
            transcript: None,
            request: None,
            pending_select: false,
            request_observed: false,
            control: ControlCoordinator::new(owner + 1)?,
            replies: ControlReplies::new(owner + 2, retry, max_attempts)?,
            raw: [0; B],
            fragments: fragment_rx::Assembly::new(),
            cursor: 0,
            retire_cursor: 0,
        })
    }
    fn current<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
    ) -> Result<()> {
        if self.gate.is_active() {
            return Err(Error::State);
        }
        #[cfg(feature = "bootstrap-join")]
        if let Some((identity, binding)) = self.identity {
            identity.current(
                binding,
                if self.role == CapabilityRole::Initiator {
                    self.peer.facts.local
                } else {
                    self.peer.facts.peer
                },
                peer.facts.now_us,
            )?;
        }
        let v = security.session_get(peer.session, peer.facts)?;
        if self.runtime != security.runtime_instance()
            || !core::ptr::eq(self.gate, security.callback_gate())
            || peer.facts.now_us == 0
            || peer.facts.now_us < self.last_now
            || peer.session != self.peer.session
            || peer.link != self.peer.link
            || peer.fingerprint != self.peer.fingerprint
            || peer.facts.local != self.peer.facts.local
            || peer.facts.peer != self.peer.facts.peer
            || peer.facts.link_generation != self.peer.facts.link_generation
            || peer.facts.policy_generation != self.peer.facts.policy_generation
            || peer.link.instance_generation() != peer.facts.link_generation.get()
            || v.session_generation.get() != self.reference.session_generation
            || v.hop_profile != self.hop
            || v.origin_security != self.origin
            || (
                security.outbound_key_generation(peer.session, peer.facts)?,
                security.inbound_key_generation(peer.session, peer.facts)?,
            ) != self.key_generations
        {
            return Err(Error::Access);
        }
        adapter.link_mtu(peer.link)?;
        Ok(())
    }
    #[cfg(feature = "bootstrap-join")]
    pub(crate) fn bind_identity<const N: usize>(
        &mut self,
        identity: &'a ucn_identity::IdentityOwner<'_, N>,
        binding: ucn_identity::BindingHandle,
    ) {
        self.identity = Some((identity, binding));
    }
    fn clock(&mut self, now: u64) -> Result<()> {
        if self.gate.is_active() || now == 0 || now < self.last_now {
            return Err(Error::State);
        }
        self.last_now = now;
        if self.deadline != 0
            && now >= self.deadline
            && !matches!(
                self.phase,
                CapabilityPhase::Confirmed | CapabilityPhase::Stopped | CapabilityPhase::Fenced
            )
        {
            self.fence();
            return Err(Error::Timeout);
        }
        Ok(())
    }
    fn fence(&mut self) {
        self.phase = CapabilityPhase::Fenced;
        self.control.stop();
        self.replies.stop();
    }
    fn access(
        &self,
        peer: JournalPeer,
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
    fn stage(&mut self, op: ProtocolOpcode, body: &[u8], now: u64) -> Result<()> {
        if body.len() > 59 || self.request.as_ref().is_some_and(|r| r.token.is_some()) {
            return Err(Error::State);
        }
        let mut payload = [0; 59];
        payload[..body.len()].copy_from_slice(body);
        self.request = Some(Request {
            op,
            body: payload,
            body_bytes: body.len(),
            frame: [0; B],
            frame_bytes: 0,
            transaction: 0,
            token: None,
            sent: false,
            attempts: 0,
            due: now,
        });
        self.request_observed = false;
        Ok(())
    }
    /// Start source once; HELLO is discovery, not a new Admission/JOIN.
    /// 源只启动一次，HELLO仅发现，不创建新入网许可。
    /// # Errors
    /// Wrong role/current ACL/time or occupied request rejects.
    pub fn start<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
    ) -> Result<()> {
        self.current(peer, security, adapter)?;
        if self.phase != CapabilityPhase::Idle || self.role != CapabilityRole::Initiator {
            return Err(Error::State);
        }
        security.authorize(
            self.access(
                peer,
                ProtocolOpcode::NeighborHello,
                AccessDirection::Outbound,
            ),
            peer.facts,
        )?;
        let deadline = peer
            .facts
            .now_us
            .checked_add(self.lifetime)
            .ok_or(Error::Exhausted)?;
        let (local, digest) = self.capabilities.local();
        let mut body = [0; 24];
        encode_capability_summary(
            CapabilitySummary {
                capability_generation: local.capability_generation,
                link_instance_generation: local.link.link_instance_generation,
                digest,
            },
            &mut body,
        )?;
        self.stage(ProtocolOpcode::NeighborHello, &body, peer.facts.now_us)?;
        self.deadline = deadline;
        self.phase = CapabilityPhase::Discovering;
        self.last_now = peer.facts.now_us;
        Ok(())
    }
    fn expected(&self, now: u64) -> Result<ProfileSelect> {
        let cached = self.capabilities.peer_get(self.reference, now)?;
        let (local, digest) = self.capabilities.local();
        if self.minimum_frame > cached.record.link.link_frame_mtu
            || self.minimum_frame > cached.record.link.processing_frame_mtu
        {
            return Err(Error::NoSpace);
        }
        ProfileSelect::build(
            local,
            digest,
            cached.record,
            cached.digest,
            self.requirements,
        )
    }
    fn context<const S: usize, const R: usize>(
        &self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
        transaction: u64,
    ) -> Result<SelectContext> {
        Ok(SelectContext {
            realm: security.realm().get(),
            source: peer.facts.local,
            destination: peer.facts.peer,
            session: self.reference.session_generation,
            key: security.outbound_key_generation(peer.session, peer.facts)?,
            policy: peer.facts.policy_generation,
            origin: self.origin,
            fingerprint: peer.fingerprint.bytes(),
            transaction,
            service: self.service.get(),
        })
    }
    /// Claim actual complete RX -> Security -> exact command dispatch, retaining resource failures.
    /// 领取实际完整RX、认证、精确分派；资源失败保留命令，不消失已接纳义务。
    /// # Errors
    /// Wrong Peer/Role/ACL/record/transcript or physical RX rejects; raw claim is retired.
    #[allow(clippy::too_many_arguments)]
    pub fn receive<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
        const W: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<()> {
        self.current(peer, security, adapter)?;
        self.clock(peer.facts.now_us)?;
        if matches!(
            self.phase,
            CapabilityPhase::Fenced | CapabilityPhase::Stopped
        ) {
            return Err(Error::State);
        }
        let claim = adapter.rx_claim(&mut self.raw)?;
        let result = if claim.link != peer.link
            || claim.meta.timestamp_us == 0
            || claim.meta.timestamp_us > peer.facts.now_us
        {
            Err(Error::Access)
        } else {
            self.admit(peer, security, crypto, scratch, claim.frame_bytes)
        };
        adapter.rx_retire(claim.token)?;
        match result? {
            Some(ControlAdmission::Command(h) | ControlAdmission::Pending(h)) => {
                self.dispatch(h, peer, security)
            }
            Some(ControlAdmission::CachedReply(_)) | None => Ok(()),
        }
    }
    fn admit<const S: usize, const R: usize, const W: usize>(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        scratch: &mut SecurityPacketWorkspace<W>,
        nbytes: usize,
    ) -> Result<Option<ControlAdmission>> {
        let packet = &self.raw[..nbytes];
        let n = 25 + 2 * security.address_width().bytes();
        if packet.len() < n + 16 {
            return Err(Error::Malformed);
        }
        let op = ProtocolOpcode::try_from(u16::from_be_bytes([packet[n - 2], packet[n - 1]]))?;
        if op == ProtocolOpcode::CapabilityFragment {
            return self.fragments.receive(
                peer,
                security,
                crypto,
                packet,
                n,
                self.service,
                self.deadline,
                scratch,
                &mut self.control,
                &mut self.replies,
            );
        }
        if !matches!(
            op,
            ProtocolOpcode::NeighborHello
                | ProtocolOpcode::NeighborKeepalive
                | ProtocolOpcode::CapabilityQuery
                | ProtocolOpcode::CapabilityAdvertise
                | ProtocolOpcode::CapabilitySelect
                | ProtocolOpcode::CapabilityAck
                | ProtocolOpcode::CapabilityRevoke
        ) {
            return Err(Error::Unsupported);
        }
        let tx = C0TransactionId::new(u64::from_be_bytes(
            packet[n - 10..n - 2]
                .try_into()
                .map_err(|_| Error::Malformed)?,
        ))?;
        let access = self.access(peer, op, AccessDirection::Inbound);
        self.control
            .receive_cached(
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
                    hop_profile: self.hop,
                },
                scratch,
                &mut self.replies,
            )
            .map(Some)
    }
    /// Private Coordinator ingress after one actual Adapter claim/retirement.
    /// 私有协调器入口；调用方已领取并退休一条实际原子RX，不签发认证证据。
    #[cfg(feature = "peer-reauth")]
    pub(crate) fn receive_routed<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const W: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        adapter: &AdapterOwner<'_, L, T, RX, B>,
        scratch: &mut SecurityPacketWorkspace<W>,
        bytes: &[u8],
    ) -> Result<()> {
        self.current(peer, security, adapter)?;
        self.clock(peer.facts.now_us)?;
        if bytes.len() > B
            || matches!(
                self.phase,
                CapabilityPhase::Fenced | CapabilityPhase::Stopped
            )
        {
            return Err(Error::State);
        }
        self.raw[..bytes.len()].copy_from_slice(bytes);
        match self.admit(peer, security, crypto, scratch, bytes.len())? {
            Some(ControlAdmission::Command(h) | ControlAdmission::Pending(h)) => {
                self.dispatch(h, peer, security)
            }
            Some(ControlAdmission::CachedReply(_)) | None => Ok(()),
        }
    }
    fn dispatch<const S: usize, const R: usize>(
        &mut self,
        h: crate::ControlHandle,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
    ) -> Result<()> {
        let command = self.control.view(h)?;
        let result = self.process(&command, peer, security);
        if !matches!(result, Err(Error::NoSpace)) {
            self.control.ack(h)?;
        }
        result
    }
    fn reserve_reply(
        &mut self,
        command: &ControlCommand,
        body: ControlBody,
        now: u64,
    ) -> Result<ReplyHandle> {
        self.replies
            .reserve(ReplyRequest::from(*command), body, now, self.deadline)
    }
    #[allow(clippy::too_many_lines)] // Fixed seven command cases; pure cross-Owner coordination, no callbacks.
    fn process<const S: usize, const R: usize>(
        &mut self,
        command: &ControlCommand,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
    ) -> Result<()> {
        if command.service() != self.service || command.accepted_at() > peer.facts.now_us {
            return Err(Error::Access);
        }
        let (realm, source, dest) = command.evidence().identities();
        let (session, key, policy, origin, fingerprint) = command.evidence().security();
        if realm != security.realm().get()
            || source != peer.facts.peer
            || dest != peer.facts.local
            || session != self.reference.session_generation
            || key != self.key_generations.1
            || policy != peer.facts.policy_generation
            || origin != self.origin
            || fingerprint != peer.fingerprint.bytes()
        {
            return Err(Error::Access);
        }
        let authenticated = security.authenticated_peer_view(
            peer.session,
            peer.facts,
            self.reference.ingress_link_id,
        )?;
        let now = peer.facts.now_us;
        let op = match command.body() {
            ControlBody::CapabilitySummary(op, _) => op,
            ControlBody::CapabilityQuery(_) => ProtocolOpcode::CapabilityQuery,
            ControlBody::CapabilityAdvertise(_) => ProtocolOpcode::CapabilityAdvertise,
            ControlBody::CapabilitySelect(_) => ProtocolOpcode::CapabilitySelect,
            ControlBody::CapabilityAck(_) => ProtocolOpcode::CapabilityAck,
            ControlBody::CapabilityRevoke(_) => ProtocolOpcode::CapabilityRevoke,
            #[cfg(not(any(
                feature = "transfer",
                feature = "service-control",
                feature = "realtime-control"
            )))]
            ControlBody::DeliveryAck(_) => return Err(Error::Unsupported),
            #[cfg(any(
                feature = "transfer",
                feature = "service-control",
                feature = "realtime-control"
            ))]
            _ => return Err(Error::Unsupported),
        };
        security.authorize(self.access(peer, op, AccessDirection::Inbound), peer.facts)?;
        match command.body() {
            ControlBody::CapabilitySummary(_, summary) => {
                self.capabilities
                    .ingest_summary(authenticated, summary, now)?;
                if self.role != CapabilityRole::Responder
                    || !matches!(
                        self.phase,
                        CapabilityPhase::Idle | CapabilityPhase::Querying
                    )
                {
                    return Err(Error::State);
                }
                if self.summary.is_some_and(|old| old != summary) {
                    return Err(Error::Security);
                }
                if self.phase == CapabilityPhase::Idle {
                    let deadline = command
                        .accepted_at()
                        .checked_add(self.lifetime)
                        .ok_or(Error::Exhausted)?;
                    let mut body = [0; 20];
                    encode_capability_query(
                        CapabilityQuery {
                            requested_generation: summary.capability_generation,
                            known_digest: summary.digest,
                        },
                        &mut body,
                    )?;
                    self.stage(ProtocolOpcode::CapabilityQuery, &body, now)?;
                    self.deadline = deadline;
                    self.summary = Some(summary);
                    self.phase = CapabilityPhase::Querying;
                }
            }
            ControlBody::CapabilityQuery(query) => {
                let (local, digest) = self.capabilities.local();
                if self.role != CapabilityRole::Initiator
                    || self.phase == CapabilityPhase::Idle
                    || query.requested_generation != local.capability_generation
                    || query.known_digest != digest
                {
                    return Err(Error::Security);
                }
                security.authorize(
                    self.access(
                        peer,
                        ProtocolOpcode::CapabilityAdvertise,
                        AccessDirection::Outbound,
                    ),
                    peer.facts,
                )?;
                self.reserve_reply(command, ControlBody::CapabilityAdvertise(local), now)?;
            }
            ControlBody::CapabilityAdvertise(record) => {
                if self.phase == CapabilityPhase::Confirmed {
                    let previous = self.capabilities.peer_get(self.reference, now)?;
                    self.capabilities
                        .ingest_advertise(authenticated, record, now)?;
                    if previous.record != record {
                        self.fence();
                    }
                    return Ok(());
                }
                if self.role == CapabilityRole::Responder {
                    let summary = self.summary.ok_or(Error::State)?;
                    if summary.capability_generation != record.capability_generation
                        || summary.digest != capability_digest(record)?
                    {
                        return Err(Error::Security);
                    }
                    security.authorize(
                        self.access(
                            peer,
                            ProtocolOpcode::CapabilityAdvertise,
                            AccessDirection::Outbound,
                        ),
                        peer.facts,
                    )?;
                    let reply = self.reserve_reply(
                        command,
                        ControlBody::CapabilityAdvertise(self.capabilities.local().0),
                        now,
                    )?;
                    if let Err(e) = self
                        .capabilities
                        .ingest_advertise(authenticated, record, now)
                    {
                        self.replies.abandon_unpublished(reply)?;
                        return Err(e);
                    }
                } else {
                    if !matches!(
                        self.phase,
                        CapabilityPhase::Discovering | CapabilityPhase::Selecting
                    ) {
                        return Err(Error::State);
                    }
                    self.capabilities
                        .ingest_advertise(authenticated, record, now)?;
                    self.selected = Some(self.expected(now)?);
                    self.pending_select = true;
                    self.request_observed = true;
                }
            }
            ControlBody::CapabilitySelect(selected) => {
                if self.role != CapabilityRole::Responder
                    || !matches!(
                        self.phase,
                        CapabilityPhase::Querying | CapabilityPhase::Confirmed
                    )
                {
                    return Err(Error::State);
                }
                if selected != self.expected(now)?.reverse() {
                    return Err(Error::Security);
                }
                let transcript = select_transcript(
                    SelectContext {
                        realm,
                        source,
                        destination: dest,
                        session,
                        key,
                        policy,
                        origin,
                        fingerprint,
                        transaction: command.evidence().sequence(),
                        service: self.service.get(),
                    },
                    selected,
                    &mut DigestWorkspace::new(),
                )?;
                security.authorize(
                    self.access(
                        peer,
                        ProtocolOpcode::CapabilityAck,
                        AccessDirection::Outbound,
                    ),
                    peer.facts,
                )?;
                self.reserve_reply(
                    command,
                    ControlBody::CapabilityAck(ProfileAck {
                        selected,
                        transcript_digest: transcript,
                    }),
                    now,
                )?;
                self.selected = Some(selected);
                self.transcript = Some(transcript);
                self.phase = CapabilityPhase::Confirmed;
                self.request_observed = true;
            }
            ControlBody::CapabilityAck(ack) => {
                if self.role != CapabilityRole::Initiator
                    || self.phase != CapabilityPhase::Selecting
                    || self
                        .request
                        .as_ref()
                        .is_none_or(|r| r.op != ProtocolOpcode::CapabilitySelect || !r.sent)
                {
                    return Err(Error::State);
                }
                let selected = self.selected.ok_or(Error::State)?;
                if selected != self.expected(now)? {
                    return Err(Error::Security);
                }
                ack.verify_exact(selected, self.transcript.ok_or(Error::State)?)?;
                self.phase = CapabilityPhase::Confirmed;
                self.request_observed = true;
            }
            ControlBody::CapabilityRevoke(query) => {
                self.capabilities.revoke(self.reference, query)?;
                self.fence();
            }
            #[cfg(not(any(
                feature = "transfer",
                feature = "service-control",
                feature = "realtime-control"
            )))]
            ControlBody::DeliveryAck(_) => return Err(Error::Unsupported),
            #[cfg(any(
                feature = "transfer",
                feature = "service-control",
                feature = "realtime-control"
            ))]
            _ => return Err(Error::Unsupported),
        }
        Ok(())
    }
    /// One bounded visit, alternating original request and cached reply work.
    /// 一次有界访问，在原请求/缓存回复间持续轮转，不严格优先级抢占。
    /// # Errors
    /// Current context/deadline/resource/Driver failure retains accepted obligations.
    #[allow(clippy::too_many_arguments)]
    pub fn drive<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const W: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
        driver: &mut impl TxDriver,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<bool> {
        self.drive_with_hop(
            peer, security, crypto, counter, None, adapter, driver, scratch,
        )
    }
    /// One bounded visit with one shared Hop counter for requests and replies.
    /// 单次有界访问，请求与回复使用同一个会话Hop计数器；H1不得隐式回退H0。
    /// # Errors
    /// Missing/wrong-profile counters reject before changing the scheduler cursor.
    #[allow(clippy::too_many_arguments)]
    pub fn drive_with_hop<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const W: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        hop: Option<&mut dyn HopSequenceOwner>,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
        driver: &mut impl TxDriver,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<bool> {
        self.current(peer, security, adapter)?;
        if (self.hop == HopProfile::H1) != hop.is_some() {
            return Err(Error::Argument);
        }
        self.clock(peer.facts.now_us)?;
        if matches!(
            self.phase,
            CapabilityPhase::Fenced | CapabilityPhase::Stopped
        ) {
            return Err(Error::State);
        }
        let class = self.cursor;
        self.cursor = (class + 1) % 5;
        if class == 1 {
            if let Ok(h) = self.control.peek_next() {
                self.dispatch(h, peer, security)?;
                return Ok(true);
            }
            if self.fragments.pending() {
                self.fragments
                    .publish(&mut self.control, &mut self.replies, peer.facts.now_us)?;
                return Ok(true);
            }
            return Ok(false);
        }
        if self.pending_select && self.request.as_ref().is_none_or(|r| r.token.is_none()) {
            let mut body = [0; 59];
            encode_profile_select(self.selected.ok_or(Error::State)?, &mut body)?;
            self.stage(ProtocolOpcode::CapabilitySelect, &body, peer.facts.now_us)?;
            self.pending_select = false;
            self.phase = CapabilityPhase::Selecting;
        }
        if class == 0 {
            return self.send_request(
                peer, security, crypto, counter, hop, adapter, driver, scratch,
            );
        }
        let Some(h) = self.replies.handle_at(class - 2) else {
            return Ok(false);
        };
        if self.replies.view(h)?.phase == crate::ReplyPhase::AwaitSeal {
            let remote_mtu = match self
                .capabilities
                .peer_get(self.reference, peer.facts.now_us)
            {
                Ok(peer) => usize::try_from(
                    peer.record
                        .link
                        .link_frame_mtu
                        .min(peer.record.link.processing_frame_mtu),
                )
                .map_err(|_| Error::Config)?,
                Err(Error::NotFound) => 25 + 2 * security.address_width().bytes() + 98,
                Err(e) => return Err(e),
            };
            self.replies.seal_capability(
                h,
                security,
                crypto,
                counter,
                peer.session,
                peer.facts,
                peer.link,
                hop,
                adapter.link_mtu(peer.link)?.min(B).min(remote_mtu),
                scratch,
            )?;
            return Ok(true);
        }
        if self.replies.scheduled_ready(h, peer.facts.now_us)? {
            if let Some(hop) = hop {
                self.replies.refresh_hop(
                    h,
                    security,
                    crypto,
                    peer.session,
                    peer.facts,
                    hop,
                    scratch,
                )?;
            }
            let mut gated = GatedDriver {
                driver,
                gate: self.gate,
                owner: self.owner,
            };
            self.replies.submit(
                h,
                security,
                peer.session,
                peer.facts,
                adapter,
                &mut gated,
                peer.link,
            )?;
            return Ok(true);
        }
        Ok(false)
    }
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)] // One exact C0 construction, cached nonce and explicit physical holder.
    fn send_request<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
        const W: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        hop: Option<&mut dyn HopSequenceOwner>,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
        driver: &mut impl TxDriver,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<bool> {
        if self.request_observed {
            return Ok(false);
        }
        let Some(request) = self.request.as_ref() else {
            return Ok(false);
        };
        let now = peer.facts.now_us;
        if request.token.is_some() || now < request.due {
            return Ok(false);
        }
        if request.attempts >= self.max_attempts {
            self.fence();
            return Err(Error::Exhausted);
        }
        security.authorize(
            self.access(peer, request.op, AccessDirection::Outbound),
            peer.facts,
        )?;
        let op = request.op;
        let width = security.address_width().bytes();
        let n = 25 + 2 * width;
        if n + request.body_bytes + 16 + usize::from(self.hop == HopProfile::H1) * 16
            > adapter.link_mtu(peer.link)?.min(B).min(F)
        {
            return Err(Error::NoSpace);
        }
        if request.frame_bytes != 0 {
            if let Some(hop) = hop {
                let access = self.access(peer, op, AccessDirection::Outbound);
                let request = self.request.as_mut().ok_or(Error::State)?;
                security.refresh_c0_hop(
                    crypto,
                    peer.session,
                    peer.facts,
                    access,
                    hop,
                    scratch,
                    &mut request.frame[..request.frame_bytes],
                )?;
            }
        } else {
            let tx = counter.preview()?;
            let mut prefix = [0; 33];
            CommonHeader {
                contract: HeaderContract::C0,
                origin_security: self.origin,
                delivery: DeliveryGuarantee::BestEffort,
                interaction: if op == ProtocolOpcode::NeighborHello {
                    InteractionRole::OneWay
                } else {
                    InteractionRole::Request
                },
                payload_kind: PayloadKind::Control,
                traffic_class: TrafficClass::Q2,
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
            prefix[15 + 2 * width..23 + 2 * width].copy_from_slice(&tx.get().to_be_bytes());
            prefix[23 + 2 * width..n].copy_from_slice(&u16::from(op).to_be_bytes());
            let transcript = if op == ProtocolOpcode::CapabilitySelect {
                Some(select_transcript(
                    self.context(peer, security, tx.get())?,
                    self.selected.ok_or(Error::State)?,
                    &mut DigestWorkspace::new(),
                )?)
            } else {
                None
            };
            let access = self.access(peer, op, AccessDirection::Outbound);
            let request = self.request.as_mut().ok_or(Error::State)?;
            request.frame_bytes = security.protect_packet(
                crypto,
                OriginCounterOwner::Transaction(counter),
                peer.session,
                peer.facts,
                access,
                PacketPlan {
                    prefix: &prefix[..n],
                    payload: &request.body[..request.body_bytes],
                    canonical_identity: &prefix[3..n],
                    origin_context: OriginContext::C0 {
                        fingerprint: peer.fingerprint,
                        transaction_id: tx,
                        opcode: op,
                        direction: if peer.facts.local.principal < peer.facts.peer.principal {
                            SenderDirection::AscendingPrincipal
                        } else {
                            SenderDirection::DescendingPrincipal
                        },
                    },
                    hop_profile: self.hop,
                },
                hop,
                scratch,
                &mut request.frame,
            )?;
            request.transaction = tx.get();
            if let Some(digest) = transcript {
                self.transcript = Some(digest);
            }
        }
        let due = now.checked_add(self.retry).ok_or(Error::Exhausted)?;
        let token = adapter.tx_reserve(peer.link, 0)?;
        let request = self.request.as_mut().ok_or(Error::State)?;
        request.token = Some(token);
        request.attempts += 1;
        request.due = due;
        let mut gated = GatedDriver {
            driver,
            gate: self.gate,
            owner: self.owner,
        };
        let result = adapter.tx_submit(token, &request.frame[..request.frame_bytes], &mut gated);
        let state = adapter.tx_view(token)?.state;
        request.sent |= matches!(
            state,
            TxState::Submitted | TxState::Completed | TxState::InDoubt
        );
        if state == TxState::NotSubmitted {
            adapter.tx_retire(token)?;
            request.token = None;
        }
        result?;
        Ok(true)
    }
    /// Revalidate live records/selection and current Security, not the phase alone.
    /// 复验live记录/选择及当前Security，不凭历史Confirmed阶段放行。
    /// # Errors
    /// Expired/revoked/changed capability or current session cannot provide a plan input.
    pub fn selected<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const F: usize,
    >(
        &self,
        peer: JournalPeer,
        security: &SecurityOwner<'_, S, R>,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
    ) -> Result<ProfileSelect> {
        self.current(peer, security, adapter)?;
        if self.phase != CapabilityPhase::Confirmed {
            return Err(Error::State);
        }
        let expected = self.expected(peer.facts.now_us)?;
        let canonical = if self.role == CapabilityRole::Responder {
            expected.reverse()
        } else {
            expected
        };
        if self.selected != Some(canonical) {
            return Err(Error::State);
        }
        Ok(expected)
    }
    /// Stop new work and disable dedicated RX; ambiguous attempts remain owned.
    /// 停止新工作并关闭专属RX，不清不确定物理Attempt。
    /// # Errors
    /// Shared callback reentry/Adapter mismatch rejects before changes.
    pub fn stop<const L: usize, const T: usize, const RX: usize, const F: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
    ) -> Result<()> {
        if self.gate.is_active() || adapter.adapter_instance() != self.peer.link.adapter_instance()
        {
            return Err(Error::State);
        }
        adapter.try_set_rx_enabled(false)?;
        self.phase = CapabilityPhase::Stopped;
        self.control.stop();
        self.replies.stop();
        Ok(())
    }
    /// Bounded retirement retains old request token even after early ACK/new stage.
    /// 有界退休，即使早到ACK/新阶段也保留旧请求Token。
    /// # Errors
    /// Unknown completion/cancel preserves the physical holder; no false quiescence.
    pub fn retire<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, RX, B>,
        driver: &mut impl TxDriver,
        budget: usize,
    ) -> Result<usize> {
        if self.gate.is_active()
            || budget == 0
            || adapter.adapter_instance() != self.peer.link.adapter_instance()
        {
            return Err(Error::State);
        }
        let mut used = 0;
        for _ in 0..budget.min(5) {
            let class = self.retire_cursor;
            self.retire_cursor = (class + 1) % 5;
            used += 1;
            if class == 4 {
                if matches!(
                    self.phase,
                    CapabilityPhase::Stopped | CapabilityPhase::Fenced
                ) {
                    if let Ok(h) = self.control.peek_next() {
                        self.control.ack(h)?;
                    }
                    self.fragments.discard();
                    match adapter.rx_claim(&mut self.raw) {
                        Ok(v) => adapter.rx_retire(v.token)?,
                        Err(Error::NotFound) => {}
                        Err(e) => return Err(e),
                    }
                }
                continue;
            }
            if class == 0 {
                if let Some(request) = self.request.as_mut() {
                    if let Some(token) = request.token {
                        let view = adapter.tx_view(token)?;
                        if matches!(
                            view.state,
                            TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
                        ) {
                            adapter.tx_retire(token)?;
                            request.token = None;
                        } else if matches!(
                            self.phase,
                            CapabilityPhase::Stopped | CapabilityPhase::Fenced
                        ) {
                            let mut gated = GatedDriver {
                                driver,
                                gate: self.gate,
                                owner: self.owner,
                            };
                            adapter.tx_cancel(token, &mut gated)?;
                            if matches!(
                                adapter.tx_view(token)?.state,
                                TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
                            ) {
                                adapter.tx_retire(token)?;
                                request.token = None;
                            }
                        }
                    }
                }
            } else if let Some(h) = self.replies.handle_at(class - 1) {
                if self.replies.view(h)?.physical_pending {
                    if matches!(
                        self.phase,
                        CapabilityPhase::Stopped | CapabilityPhase::Fenced
                    ) || self.last_now >= self.replies.view(h)?.expires_at
                    {
                        let mut gated = GatedDriver {
                            driver,
                            gate: self.gate,
                            owner: self.owner,
                        };
                        self.replies.cancel_expired_attempt(
                            h,
                            self.last_now,
                            adapter,
                            &mut gated,
                        )?;
                    } else {
                        self.replies.retire_if_terminal(h, adapter)?;
                    }
                }
                if self.last_now >= self.replies.view(h)?.expires_at
                    || matches!(
                        self.phase,
                        CapabilityPhase::Stopped | CapabilityPhase::Fenced
                    )
                {
                    self.replies.retire_cache(h, self.last_now)?;
                }
            }
        }
        Ok(used)
    }
    /// Original handshake deadline, including when no more RX arrives; 原始握手截止，不依赖后续RX。
    /// # Errors
    /// Time rollback/callback reentry/expiry rejects.
    pub fn step(&mut self, now: u64) -> Result<()> {
        self.clock(now)
    }
    /// Logical state only; current-use permission requires `selected`.
    /// 只读逻辑状态，使用许可必须重新调用selected。
    #[must_use]
    pub const fn phase(&self) -> CapabilityPhase {
        self.phase
    }
    /// Local stopped Root only, not remote/whole-node proof; 仅本地已停止Root，不代表远端或全节点。
    #[must_use]
    pub fn quiescent<const L: usize, const T: usize, const RX: usize>(
        &self,
        adapter: &AdapterOwner<'_, L, T, RX, B>,
    ) -> bool {
        self.phase == CapabilityPhase::Stopped
            && adapter.adapter_instance() == self.peer.link.adapter_instance()
            && adapter.rx_is_quiescent()
            && self.control.is_empty()
            && self.fragments.empty()
            && self.request.as_ref().is_none_or(|r| r.token.is_none())
            && (0..3).all(|i| self.replies.handle_at(i).is_none())
    }
}
