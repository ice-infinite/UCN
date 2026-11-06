//! Fixed multi-peer Coordinator; no business Owner calls another Owner.
//! 固定多邻居协调器；业务Owner不直接调用其他Owner。
use crate::{JournalPeer, runtime_common::GatedDriver};
use ucn_adapter::{AdapterOwner, TerminalOutcome, TxDriver, TxState, TxToken};
use ucn_capability::{CachedPeerCapability, CapabilityOwner, CapabilityRecord, PeerCapabilityRef};
use ucn_identity::{AuthorityEpoch, BindingHandle, BindingView, IdentityOwner};
use ucn_owner::CallbackGate;
use ucn_routing::{
    DiscoveryHandle, LinkRef, ReplyAction, ReplyForwardHandle, RequestAction, ResolvedRoute,
    RouteControl, RouteDomain, RouteFragment, RouteOwner, RouteUseFacts, SoftRouteView,
    route_fragment_count,
};
use ucn_security::{
    AccessDirection, AccessRequest, Binding, C0TransactionOwner, CryptoProvider, HopSequenceOwner,
    OpenDisposition, OpenPacketPlan, OriginContext, OriginCounterOwner, PacketEvidence, PacketPlan,
    SecurityOwner, SecurityPacketWorkspace, SenderDirection,
};
use ucn_types::{
    AddressWidth, C0TransactionId, DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile,
    InteractionRole, NodeAddress, OriginSecurity, PayloadKind, ProtocolOpcode, RealmId, Result,
    ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;
#[cfg(feature = "route-data-runtime")]
mod data;
#[cfg(feature = "route-data-runtime")]
pub use data::*;

/// Caller-owned counters must remain unique across Capability and Route callers.
/// 调用方计数器必须跨Capability/Route共享，不能在模块切换时重置。
pub trait PeerCounterBank {
    /// Exact Session counter owners; 精确Session的计数器所有者。
    /// # Errors
    /// Unknown/retired Session or exhausted allocation rejects.
    fn counters(
        &mut self,
        session: ucn_security::SessionHandle,
    ) -> Result<(&mut dyn C0TransactionOwner, &mut dyn HopSequenceOwner)>;
}
/// Opaque peer registration; 不透明邻居登记。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RoutePeerHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// Product scheduling policy, no peer-supplied privilege.
/// 产品调度策略，不接受远端提供特权。
#[derive(Clone, Copy)]
pub struct RouteRuntimeConfig {
    /// Unique Coordinator; 唯一协调器。
    pub owner: u32,
    /// Realm; 域。
    pub realm: RealmId,
    /// Frozen width; 冻结地址宽度。
    pub width: AddressWidth,
    /// Exact control ACL service; 精确控制ACL服务。
    pub service: ServiceId,
    /// Nonrenewing per-message lifetime; 不续期消息寿命。
    pub lifetime_us: u64,
    /// Retry interval; 重试间隔。
    pub retry_us: u64,
    /// Total physical attempts; 物理总次数。
    pub max_attempts: u16,
}
#[derive(Clone, Copy)]
struct RegisteredNeighbor {
    peer: JournalPeer,
    parents: [BindingHandle; 2],
    views: [BindingView; 2],
    authority: AuthorityEpoch,
    capability: CachedPeerCapability,
    link: LinkRef,
    discriminator: u32,
    fenced: bool,
}
#[derive(Clone, Copy)]
struct Job {
    peer: RoutePeerHandle,
    body: RouteControl,
    deadline: u64,
    due: u64,
    attempts: u16,
    token: Option<TxToken>,
    length: usize,
    completion: Option<ReplyForwardHandle>,
    retire: bool,
    delivered: bool,
    part: u8,
    parts: u8,
    bundle: u64,
}
struct Slot<const B: usize> {
    value: Option<Job>,
    packet: [u8; B],
}
#[derive(Clone, Copy)]
struct Receipt {
    local: SoftRouteView,
    peer_generation: u32,
    peer: RoutePeerHandle,
    upstream: Option<RoutePeerHandle>,
    failed: bool,
}
#[derive(Clone, Copy)]
struct Assembly {
    high_water: u64,
    active: bool,
    opcode: ProtocolOpcode,
    total: u16,
    count: u8,
    mask: u8,
    deadline: u64,
    evidence: Option<PacketEvidence>,
    bytes: [u8; 99],
}
impl Assembly {
    const EMPTY: Self = Self {
        high_water: 0,
        active: false,
        opcode: ProtocolOpcode::RouteRrep,
        total: 0,
        count: 0,
        mask: 0,
        deadline: 0,
        evidence: None,
        bytes: [0; 99],
    };
}
/// Fixed multi-peer routing root, with separate logical and physical ownership.
/// 固定多邻居路由Root，逻辑结果与物理义务分开。
pub struct RouteRuntime<'g, const P: usize, const Q: usize, const B: usize> {
    config: RouteRuntimeConfig,
    gate: &'g CallbackGate,
    peers: [Option<RegisteredNeighbor>; P],
    generations: [u32; P],
    jobs: [Slot<B>; Q],
    receipts: [Option<Receipt>; Q],
    local_capability: Option<CapabilityRecord>,
    assemblies: [Assembly; P],
    raw: [u8; B],
    plain: [u8; 99],
    send_cursor: usize,
    retire_cursor: usize,
    maintenance_cursor: usize,
    last_now: u64,
    stopped: bool,
}
impl<'g, const P: usize, const Q: usize, const B: usize> RouteRuntime<'g, P, Q, B> {
    /// Construct only fixed buffers; no Driver/Provider work.
    /// 只构造固定缓冲，不操作Driver/Provider。
    /// # Errors
    /// Invalid capacities, lifetime or callback gate rejects.
    pub fn new(c: RouteRuntimeConfig, gate: &'g CallbackGate) -> Result<Self> {
        if c.owner == 0
            || P == 0
            || P > 16
            || Q < P
            || Q > 64
            || B < 127
            || B > 4096
            || c.lifetime_us == 0
            || c.lifetime_us > i64::MAX as u64
            || c.retry_us == 0
            || c.retry_us > c.lifetime_us
            || c.max_attempts == 0
            || gate.is_active()
        {
            return Err(Error::Config);
        }
        Ok(Self {
            config: c,
            gate,
            peers: [None; P],
            generations: [0; P],
            jobs: core::array::from_fn(|_| Slot {
                value: None,
                packet: [0; B],
            }),
            receipts: [None; Q],
            local_capability: None,
            assemblies: [Assembly::EMPTY; P],
            raw: [0; B],
            plain: [0; 99],
            send_cursor: 0,
            retire_cursor: 0,
            maintenance_cursor: 0,
            last_now: 0,
            stopped: false,
        })
    }
    fn clock(&self, now: u64) -> Result<()> {
        if self.gate.is_active() || self.stopped || now == 0 || now < self.last_now {
            Err(Error::State)
        } else {
            Ok(())
        }
    }
    fn index(&self, h: RoutePeerHandle) -> Result<usize> {
        if h.owner != self.config.owner
            || h.slot >= P
            || self.generations[h.slot] != h.generation
            || self.peers[h.slot].is_none()
        {
            Err(Error::NotFound)
        } else {
            Ok(h.slot)
        }
    }
    fn handle(&self, i: usize) -> RoutePeerHandle {
        RoutePeerHandle {
            owner: self.config.owner,
            slot: i,
            generation: self.generations[i],
        }
    }
    fn access(
        &self,
        p: &RegisteredNeighbor,
        op: ProtocolOpcode,
        direction: AccessDirection,
    ) -> AccessRequest {
        AccessRequest {
            session: p.peer.session,
            context_fingerprint: p.peer.fingerprint,
            service: self.config.service,
            protocol_opcode: op.into(),
            direction,
        }
    }
    fn peer_now<
        const N: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &self,
        handle: RoutePeerHandle,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        capabilities: &CapabilityOwner<C>,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        now: u64,
    ) -> Result<RegisteredNeighbor> {
        self.clock(now)?;
        let mut p = self.peers[self.index(handle)?].ok_or(Error::NotFound)?;
        if p.fenced
            || !core::ptr::eq(identity.callback_gate(), self.gate)
            || !core::ptr::eq(security.callback_gate(), self.gate)
            || identity.authority_get(now)? != p.authority
            || [
                identity.binding_get(p.parents[0], now)?,
                identity.binding_get(p.parents[1], now)?,
            ] != p.views
            || capabilities.peer_get(p.capability.peer_ref, now)? != p.capability
            || self.local_capability != Some(capabilities.local().0)
            || adapter.link_mtu(p.peer.link)? < usize::from(p.link.frame_mtu())
        {
            return Err(Error::State);
        }
        p.peer.facts.now_us = now;
        let v = security.session_get(p.peer.session, p.peer.facts)?;
        if v.origin_security != OriginSecurity::O1 || v.hop_profile != HopProfile::H1 {
            return Err(Error::Policy);
        }
        Ok(p)
    }
    /// Register only current actual Identity, Security and Capability publications.
    /// 仅登记当前实际Identity、Security及Capability发布，不接收认证布尔值。
    /// # Errors
    /// Table full, ambiguous physical peer, stale parent/profile or active callback rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn register<
        const N: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &mut self,
        peer: JournalPeer,
        parents: [BindingHandle; 2],
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        capabilities: &CapabilityOwner<C>,
        capability: PeerCapabilityRef,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        cost: u32,
        discriminator: u32,
        now: u64,
    ) -> Result<RoutePeerHandle> {
        self.clock(now)?;
        let value = self.prepare_neighbor(
            peer,
            parents,
            identity,
            security,
            capabilities,
            capability,
            adapter,
            cost,
            discriminator,
            now,
            None,
        )?;
        let slot = self
            .peers
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.peers[slot] = Some(value);
        self.generations[slot] = generation;
        self.local_capability = Some(capabilities.local().0);
        self.last_now = now;
        Ok(self.handle(slot))
    }

    #[allow(clippy::too_many_arguments, clippy::too_many_lines)] // Keep the complete zero-write registration preflight together.
    fn prepare_neighbor<
        const N: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &self,
        peer: JournalPeer,
        parents: [BindingHandle; 2],
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        capabilities: &CapabilityOwner<C>,
        capability: PeerCapabilityRef,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        cost: u32,
        discriminator: u32,
        now: u64,
        replacing: Option<usize>,
    ) -> Result<RegisteredNeighbor> {
        self.clock(now)?;
        if !core::ptr::eq(identity.callback_gate(), self.gate)
            || !core::ptr::eq(security.callback_gate(), self.gate)
            || security.realm() != self.config.realm
            || security.address_width() != self.config.width
            || discriminator == 0
            || peer.facts.now_us != now
            || peer.link.instance_generation() != peer.facts.link_generation.get()
            || capabilities.local().0.link.link_instance_generation
                != peer.link.instance_generation()
            || self
                .local_capability
                .is_some_and(|v| v != capabilities.local().0)
        {
            return Err(Error::Config);
        }
        let views = [
            identity.binding_get(parents[0], now)?,
            identity.binding_get(parents[1], now)?,
        ];
        let authority = identity.authority_get(now)?;
        if authority.realm != self.config.realm
            || views.iter().any(|v| {
                v.runtime_instance() != security.runtime_instance()
                    || v.certificate().binding.realm != self.config.realm
                    || v.certificate().binding.address_width != self.config.width
            })
            || binding(views[0]) != peer.facts.local
            || binding(views[1]) != peer.facts.peer
        {
            return Err(Error::Access);
        }
        let auth =
            security.authenticated_peer_view(peer.session, peer.facts, peer.link.slot() + 1)?;
        if auth.peer() != peer.facts.peer
            || capability.binding != peer.facts.peer
            || capability.realm != self.config.realm
            || capability.runtime_instance != auth.runtime_instance()
            || capability.security_owner_instance != auth.security_owner_instance()
            || capability.ingress_link_id != peer.link.slot() + 1
            || capability.ingress_link_generation != peer.link.instance_generation()
            || capability.session_generation != auth.session_generation().get()
        {
            return Err(Error::Security);
        }
        let cap = capabilities.peer_get(capability, now)?;
        let sv = security.session_get(peer.session, peer.facts)?;
        if security.context_fingerprints(peer.session, peer.facts)?.0 != peer.fingerprint {
            return Err(Error::Security);
        }
        if sv.origin_security != OriginSecurity::O1 || sv.hop_profile != HopProfile::H1 {
            return Err(Error::Policy);
        }
        if self.peers.iter().flatten().any(|p| {
            p.peer.facts.local != peer.facts.local
                || p.peer.link.adapter_instance() != peer.link.adapter_instance()
                || p.capability.peer_ref.runtime_instance != capability.runtime_instance
                || p.capability.peer_ref.security_owner_instance
                    != capability.security_owner_instance
        }) {
            return Err(Error::Config);
        }
        let mtu = adapter
            .link_mtu(peer.link)?
            .min(usize::try_from(cap.record.link.link_frame_mtu).map_err(|_| Error::Config)?)
            .min(usize::try_from(cap.record.link.processing_frame_mtu).map_err(|_| Error::Config)?)
            .min(
                usize::try_from(capabilities.local().0.link.link_frame_mtu)
                    .map_err(|_| Error::Config)?,
            )
            .min(
                usize::try_from(capabilities.local().0.link.processing_frame_mtu)
                    .map_err(|_| Error::Config)?,
            )
            .min(B);
        if mtu < 127 {
            return Err(Error::NoSpace);
        }
        if self.peers.iter().enumerate().any(|(i, p)| {
            Some(i) != replacing
                && p.is_some_and(|p| {
                    p.peer.facts.peer.principal == peer.facts.peer.principal
                        || p.peer.facts.peer.address == peer.facts.peer.address
                        || p.peer.link == peer.link && p.discriminator == discriminator
                })
        }) {
            return Err(Error::State);
        }
        let link = LinkRef::new(
            peer.link.slot() + 1,
            peer.facts.link_generation,
            peer.facts.peer,
            u16::try_from(mtu).map_err(|_| Error::Config)?,
            cost,
            u16::try_from(
                cap.record.peer.feature_bits & capabilities.local().0.peer.feature_bits & 0xffff,
            )
            .map_err(|_| Error::Config)?,
        )?;
        Ok(RegisteredNeighbor {
            peer,
            parents,
            views,
            authority,
            capability: cap,
            link,
            discriminator,
            fenced: false,
        })
    }
    fn free(&self) -> usize {
        self.jobs.iter().filter(|s| s.value.is_none()).count()
    }
    fn enqueue(
        &mut self,
        h: RoutePeerHandle,
        body: RouteControl,
        now: u64,
        deadline: u64,
        completion: Option<ReplyForwardHandle>,
    ) -> Result<()> {
        let slot = self
            .jobs
            .iter()
            .position(|s| s.value.is_none())
            .ok_or(Error::NoSpace)?;
        self.jobs[slot].value = Some(Job {
            peer: h,
            body,
            deadline,
            due: now,
            attempts: 0,
            token: None,
            length: 0,
            completion,
            retire: false,
            delivered: false,
            part: 0,
            parts: 0,
            bundle: 0,
        });
        Ok(())
    }
    fn assemble(
        &mut self,
        handle: RoutePeerHandle,
        piece: RouteFragment,
        evidence: PacketEvidence,
        now: u64,
        deadline: u64,
    ) -> Result<bool> {
        let assembly = &mut self.assemblies[handle.slot];
        let (offset, length) = piece.span()?;
        if piece.bundle.get() > evidence.sequence() {
            return Err(Error::Security);
        }
        if assembly.active {
            if now >= assembly.deadline {
                return Err(Error::Timeout);
            }
            let prior = assembly.evidence.ok_or(Error::State)?;
            if prior.identities() != evidence.identities()
                || prior.security() != evidence.security()
            {
                return Err(Error::Security);
            }
            if piece.index == 0 && piece.bundle.get() > assembly.high_water {
                // Only unactioned private bytes are replaced. No RouteOwner,
                // Driver token or pending replay belongs to this assembly.
                // Preserve the FIRST deadline, discard EVERY old piece and
                // advance the high-water mark; a stale tail cannot mix in.
                // 只替换尚无业务副作用的私有字节；原截止不刷新，旧片全部清空，
                // 高水位前进。不存在被抹除的Driver/Route/Replay义务。
                assembly.high_water = piece.bundle.get();
                assembly.opcode = piece.opcode;
                assembly.total = piece.total;
                assembly.count = piece.count;
                assembly.mask = 0;
                assembly.evidence = Some(evidence);
                assembly.bytes.fill(0);
            }
        }
        if !assembly.active {
            if piece.index != 0 || piece.bundle.get() <= assembly.high_water {
                return Err(Error::Replay);
            }
            *assembly = Assembly {
                high_water: piece.bundle.get(),
                active: true,
                opcode: piece.opcode,
                total: piece.total,
                count: piece.count,
                mask: 0,
                deadline,
                evidence: Some(evidence),
                bytes: [0; 99],
            };
        }
        if now >= assembly.deadline {
            return Err(Error::Timeout);
        }
        let prior = assembly.evidence.ok_or(Error::State)?;
        if assembly.high_water != piece.bundle.get()
            || assembly.opcode != piece.opcode
            || assembly.total != piece.total
            || assembly.count != piece.count
            || prior.identities() != evidence.identities()
            || prior.security() != evidence.security()
        {
            return Err(Error::Security);
        }
        let bit = 1u8 << piece.index;
        if assembly.mask & bit != 0
            && assembly.bytes[offset..offset + length] != piece.chunk[..length]
        {
            return Err(Error::Security);
        }
        assembly.bytes[offset..offset + length].copy_from_slice(&piece.chunk[..length]);
        assembly.mask |= bit;
        Ok(assembly.mask == (1u8 << piece.count) - 1)
    }
    fn peer_link(&self, l: LinkRef) -> Result<RoutePeerHandle> {
        self.peers
            .iter()
            .position(|p| p.is_some_and(|v| !v.fenced && v.link == l))
            .map(|i| self.handle(i))
            .ok_or(Error::NotFound)
    }
    /// Begin one bounded authenticated fanout; repeated same-target calls do not duplicate work.
    /// 开始一次有界认证扩散，同目标重复调用不重复建立工作。
    /// # Errors
    /// Any current peer/ACL/capacity failure precedes discovery mutation.
    #[allow(clippy::too_many_arguments)]
    pub fn discover<
        const N: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const V: usize,
        const RR: usize,
        const SS: usize,
    >(
        &mut self,
        routes: &mut RouteOwner<D, V, RR, SS>,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        capabilities: &CapabilityOwner<C>,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        target: NodeAddress,
        required: u16,
        minimum_frame_mtu: u16,
        now: u64,
    ) -> Result<DiscoveryHandle> {
        self.clock(now)?;
        self.last_now = now;
        if minimum_frame_mtu == 0 {
            return Err(Error::Argument);
        }
        if self.free() < P {
            return Err(Error::NoSpace);
        }
        if routes.config().realm != self.config.realm
            || routes.config().address_width != self.config.width
            || self
                .peers
                .iter()
                .flatten()
                .any(|p| p.peer.facts.local != routes.config().local)
        {
            return Err(Error::Config);
        }
        let mut chosen = [None; P];
        for (n, slot) in chosen.iter_mut().enumerate() {
            if self.peers[n].is_some_and(|p| !p.fenced) {
                let h = self.handle(n);
                let Ok(p) = self.peer_now(h, identity, security, capabilities, adapter, now) else {
                    continue;
                };
                if p.link.frame_mtu() < minimum_frame_mtu
                    || required & !p.link.capability_bits() != 0
                {
                    continue;
                }
                security.authorize(
                    self.access(&p, ProtocolOpcode::RouteRreq, AccessDirection::Outbound),
                    p.peer.facts,
                )?;
                *slot = Some(h);
            }
        }
        if chosen.iter().all(Option::is_none) {
            return Err(Error::NotFound);
        }
        let deadline = now
            .checked_add(self.config.lifetime_us)
            .ok_or(Error::Exhausted)?;
        let start = routes.ensure_discovery(target, required, minimum_frame_mtu, 0, now)?;
        if start.created {
            for h in chosen.into_iter().flatten() {
                self.enqueue(h, RouteControl::Request(start.request), now, deadline, None)?;
            }
        }
        self.last_now = now;
        Ok(start.handle)
    }
    /// One actual RX claim, authenticate, preflight resources, then change Route Owner.
    /// 一条实际RX：认证、资源预检，再修改Route Owner。
    /// # Errors
    /// Bad tag/ACL/parent, unsupported frame, exhausted queue or causal mismatch rejects.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn receive<
        const N: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const V: usize,
        const RR: usize,
        const SS: usize,
        const W: usize,
    >(
        &mut self,
        routes: &mut RouteOwner<D, V, RR, SS>,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, S, R>,
        capabilities: &CapabilityOwner<C>,
        adapter: &mut AdapterOwner<'_, L, T, X, B>,
        crypto: &mut impl CryptoProvider,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
    ) -> Result<()> {
        self.clock(now)?;
        self.check_route_owner(routes)?;
        let claim = adapter.rx_claim(&mut self.raw)?;
        adapter.rx_retire(claim.token)?;
        self.receive_loaded(
            routes,
            identity,
            security,
            capabilities,
            adapter,
            crypto,
            scratch,
            claim,
            now,
        )
    }
    fn check_route_owner<const D: usize, const V: usize, const R: usize, const S: usize>(
        &self,
        routes: &RouteOwner<D, V, R, S>,
    ) -> Result<()> {
        if routes.config().realm != self.config.realm
            || routes.config().address_width != self.config.width
            || self
                .peers
                .iter()
                .flatten()
                .any(|p| p.peer.facts.local != routes.config().local)
        {
            return Err(Error::Config);
        }
        Ok(())
    }
    // Coordinator-private dispatch for bytes from its single actual RX claim.
    // 协调器私有分派；输入只能来自其唯一实际RX领取，不对外开放原始事实注入。
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    fn receive_loaded<
        const N: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const V: usize,
        const RR: usize,
        const SS: usize,
        const W: usize,
    >(
        &mut self,
        routes: &mut RouteOwner<D, V, RR, SS>,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, S, R>,
        capabilities: &CapabilityOwner<C>,
        adapter: &mut AdapterOwner<'_, L, T, X, B>,
        crypto: &mut impl CryptoProvider,
        scratch: &mut SecurityPacketWorkspace<W>,
        claim: ucn_adapter::RxView,
        now: u64,
    ) -> Result<()> {
        self.clock(now)?;
        self.last_now = now;
        self.check_route_owner(routes)?;
        let selected = self.peers.iter().position(|p| {
            p.is_some_and(|p| {
                p.peer.link == claim.link && p.discriminator == claim.meta.sender_discriminator
            })
        });
        if claim.meta.timestamp_us == 0 || claim.meta.timestamp_us > now {
            return Err(Error::Access);
        }
        let handle = self.handle(selected.ok_or(Error::Access)?);
        let peer = self.peer_now(handle, identity, security, capabilities, adapter, now)?;
        let n = 25 + 2 * self.config.width.bytes();
        let bytes = &self.raw[..claim.frame_bytes];
        if bytes.len() < n + 32 {
            return Err(Error::Malformed);
        }
        let header = CommonHeader::decode(bytes)?;
        let op = ProtocolOpcode::try_from(u16::from_be_bytes([bytes[n - 2], bytes[n - 1]]))?;
        if header.contract != HeaderContract::C0
            || header.origin_security != OriginSecurity::O1
            || header.hop_limit.get() != 1
            || header.traffic_class != TrafficClass::Q0
            || header.delivery != DeliveryGuarantee::BestEffort
            || header.payload_kind != PayloadKind::Control
            || header.interaction != role(op)?
        {
            return Err(Error::Malformed);
        }
        let transaction = C0TransactionId::new(u64::from_be_bytes(
            bytes[n - 10..n - 2]
                .try_into()
                .map_err(|_| Error::Malformed)?,
        ))?;
        let opened = security.open_packet(
            crypto,
            peer.peer.session,
            peer.peer.facts,
            self.access(&peer, op, AccessDirection::Inbound),
            OpenPacketPlan {
                packet: bytes,
                prefix_bytes: n,
                canonical_identity: &bytes[3..n],
                origin_context: context(&peer, transaction, op, false),
                hop_profile: HopProfile::H1,
            },
            scratch,
            &mut self.plain,
        )?;
        let result = (|| {
            if opened.disposition != OpenDisposition::FreshAuthenticated {
                return Err(Error::Replay);
            }
            security.replay_preflight(&opened.replay, peer.peer.facts)?;
            let deadline = now
                .checked_add(self.config.lifetime_us)
                .ok_or(Error::Exhausted)?;
            let fragmented = op == ProtocolOpcode::RouteFragment;
            let (op, length) = if fragmented {
                let piece = RouteFragment::decode(&self.plain[..opened.payload_bytes])?;
                if piece.count
                    != route_fragment_count(
                        piece.opcode,
                        self.config.width,
                        usize::from(peer.link.frame_mtu()),
                    )?
                {
                    return Err(Error::Malformed);
                }
                security.authorize(
                    self.access(&peer, piece.opcode, AccessDirection::Inbound),
                    peer.peer.facts,
                )?;
                if !self.assemble(handle, piece, opened.evidence(), now, deadline)? {
                    return Ok(());
                }
                let assembly = &self.assemblies[handle.slot];
                let length = usize::from(assembly.total);
                self.plain[..length].copy_from_slice(&assembly.bytes[..length]);
                (piece.opcode, length)
            } else {
                (op, opened.payload_bytes)
            };
            let body = RouteControl::decode(op, self.config.width, &self.plain[..length])?;
            match body {
                RouteControl::Request(r) => {
                    if r.key.realm != self.config.realm {
                        return Err(Error::Access);
                    }
                    let mut fanout = [None; P];
                    for (slot, out) in fanout.iter_mut().enumerate() {
                        if r.key.target_address != routes.config().local.address
                            && slot != handle.slot
                            && self.peers[slot].is_some_and(|p| !p.fenced)
                        {
                            let h = self.handle(slot);
                            let Ok(p) =
                                self.peer_now(h, identity, security, capabilities, adapter, now)
                            else {
                                continue;
                            };
                            if p.link.frame_mtu() < r.payload.minimum_frame_mtu
                                || r.payload.required_capability_bits & !p.link.capability_bits()
                                    != 0
                            {
                                continue;
                            }
                            security.authorize(
                                self.access(&p, op, AccessDirection::Outbound),
                                p.peer.facts,
                            )?;
                            *out = Some(h);
                        }
                    }
                    if r.key.target_address == routes.config().local.address {
                        if self.free() == 0 {
                            return Err(Error::NoSpace);
                        }
                        security.authorize(
                            self.access(
                                &peer,
                                ProtocolOpcode::RouteRrep,
                                AccessDirection::Outbound,
                            ),
                            peer.peer.facts,
                        )?;
                    } else if self.free() < fanout.iter().flatten().count() {
                        return Err(Error::NoSpace);
                    }
                    match routes.on_rreq(r, peer.link, now)? {
                        RequestAction::Duplicate => {}
                        RequestAction::LocalTarget => {
                            let reply = routes.make_rrep(
                                r,
                                peer.link.capability_bits(),
                                peer.link.frame_mtu(),
                                now,
                            )?;
                            self.enqueue(
                                handle,
                                RouteControl::Reply(reply, 0),
                                now,
                                deadline,
                                None,
                            )?;
                        }
                        RequestAction::Forward(r) => {
                            for h in fanout.into_iter().flatten() {
                                self.enqueue(h, RouteControl::Request(r), now, deadline, None)?;
                            }
                        }
                    }
                }
                RouteControl::Reply(r, remote) => {
                    if self.receipts.iter().all(Option::is_some) {
                        return Err(Error::NoSpace);
                    }
                    if r.payload.hop_count == 0
                        && (r.key.target_address != peer.peer.facts.peer.address
                            || r.payload.destination_principal != peer.peer.facts.peer.principal
                            || r.payload.destination_binding_generation
                                != peer.peer.facts.peer.generation.get())
                    {
                        return Err(Error::Access);
                    }
                    // Source is the immediate authenticated router, not necessarily target.
                    // Source为实际认证路由邻居，不必等于最终目标Principal。
                    let upstream = routes
                        .reply_upstream(r.key, now)?
                        .map(|l| self.peer_link(l))
                        .transpose()?;
                    if let Some(h) = upstream {
                        if self.free() == 0 {
                            return Err(Error::NoSpace);
                        }
                        let p = self.peer_now(h, identity, security, capabilities, adapter, now)?;
                        security.authorize(
                            self.access(&p, op, AccessDirection::Outbound),
                            p.peer.facts,
                        )?;
                    }
                    let action = routes.on_rrep(r, peer.link, now)?;
                    for slot in &mut self.jobs {
                        if let Some(j) = slot.value.as_mut() {
                            if matches!(j.body,RouteControl::Request(q) if q.key==r.key) {
                                j.retire = true;
                            }
                        }
                    }
                    let local = match action {
                        ReplyAction::ReachedOrigin(v) => v,
                        ReplyAction::Forward {
                            message,
                            upstream,
                            completion,
                        } => {
                            let h = self.peer_link(upstream)?;
                            let p =
                                self.peer_now(h, identity, security, capabilities, adapter, now)?;
                            security.authorize(
                                self.access(&p, op, AccessDirection::Outbound),
                                p.peer.facts,
                            )?;
                            let domain = RouteDomain {
                                realm: r.key.realm,
                                origin: r.key.origin,
                                origin_session_generation: r.key.origin_session_generation,
                                destination: Binding {
                                    address: r.key.target_address,
                                    generation: ucn_types::BindingGeneration::active(
                                        r.payload.destination_binding_generation,
                                    )?,
                                    principal: r.payload.destination_principal,
                                },
                            };
                            let ResolvedRoute::Dynamic(view) = routes.resolve(
                                domain,
                                RouteUseFacts {
                                    now_us: now,
                                    origin_session_generation: domain.origin_session_generation,
                                    destination_binding_generation: domain
                                        .destination
                                        .generation
                                        .get(),
                                    link_generation: peer.link.link_generation(),
                                },
                            )?
                            else {
                                return Err(Error::State);
                            };
                            self.enqueue(
                                h,
                                RouteControl::Reply(message, view.route_generation().get()),
                                now,
                                deadline,
                                Some(completion),
                            )?;
                            view
                        }
                    };
                    let slot = self
                        .receipts
                        .iter()
                        .position(Option::is_none)
                        .ok_or(Error::NoSpace)?;
                    self.receipts[slot] = Some(Receipt {
                        local,
                        peer_generation: remote,
                        peer: handle,
                        upstream,
                        failed: false,
                    });
                }
                RouteControl::Error(mut e) => {
                    if e.reporter_principal != peer.peer.facts.peer.principal
                        || e.reporter_address != peer.peer.facts.peer.address.get()
                        || e.reporter_binding_generation != peer.peer.facts.peer.generation.get()
                    {
                        return Err(Error::Access);
                    }
                    let slot = self
                        .receipts
                        .iter()
                        .position(|r| {
                            r.is_some_and(|r| {
                                r.peer == handle
                                    && r.peer_generation != 0
                                    && r.peer_generation == e.route_generation
                                    && r.local.route_causal_id() == e.route_causal_id
                                    && r.local.domain().realm.get() == e.realm
                                    && r.local.domain().origin.principal == e.origin_principal
                                    && r.local.domain().origin.address.get() == e.origin_address
                                    && r.local.domain().origin.generation.get()
                                        == e.origin_binding_generation
                                    && r.local.domain().origin_session_generation.get()
                                        == e.origin_session_generation
                                    && r.local.domain().destination.principal
                                        == e.destination_principal
                                    && r.local.domain().destination.address.get()
                                        == e.destination_address
                                    && r.local.domain().destination.generation.get()
                                        == e.destination_binding_generation
                            })
                        })
                        .ok_or(Error::NotFound)?;
                    let receipt = self.receipts[slot].ok_or(Error::State)?;
                    if receipt.upstream.is_some() && self.free() == 0 {
                        return Err(Error::NoSpace);
                    }
                    let outbound = receipt
                        .upstream
                        .map(|h| {
                            let p =
                                self.peer_now(h, identity, security, capabilities, adapter, now)?;
                            security.authorize(
                                self.access(&p, op, AccessDirection::Outbound),
                                p.peer.facts,
                            )?;
                            Ok((h, p))
                        })
                        .transpose()?;
                    // Translate reporter-local generation via the frozen authenticated RREP
                    // receipt. Local Link IDs never cross node namespaces.
                    // 通过已认证RREP回执翻译报告方代际，本地Link ID不跨节点比较。
                    e.route_generation = receipt.local.route_generation().get();
                    e.failed_link_id = peer.link.link_id();
                    e.failed_link_generation = peer.link.link_generation().get();
                    let invalidated = routes.on_rerr(e)?;
                    if invalidated {
                        if let Some((h, p)) = outbound {
                            let mut forward = routes.make_rerr(receipt.local, e.reason)?;
                            forward.reporter_principal = p.peer.facts.local.principal;
                            forward.reporter_address = p.peer.facts.local.address.get();
                            forward.reporter_binding_generation =
                                p.peer.facts.local.generation.get();
                            self.enqueue(h, RouteControl::Error(forward), now, deadline, None)?;
                        }
                    }
                    self.receipts[slot] = None;
                }
            }
            if fragmented {
                self.assemblies[handle.slot].active = false;
            }
            Ok(())
        })();
        if let Err(e) = result {
            security.replay_abort(opened.replay)?;
            return Err(e);
        }
        security.replay_commit(opened.replay, peer.peer.facts)?;
        self.last_now = now;
        Ok(())
    }
    /// One rotating TX visit; inner discovery stays fixed, each logical retry
    /// allocates fresh per-peer C0/H1 counters.
    /// 一次轮转TX访问；内层发现键固定，逻辑重试分配新的逐邻居C0/H1计数。
    /// # Errors
    /// Current parent/ACL/MTU/Driver failure retains every accepted holder.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn drive<
        const N: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const W: usize,
    >(
        &mut self,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, S, R>,
        capabilities: &CapabilityOwner<C>,
        adapter: &mut AdapterOwner<'_, L, T, X, B>,
        crypto: &mut impl CryptoProvider,
        counters: &mut impl PeerCounterBank,
        driver: &mut impl TxDriver,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
    ) -> Result<bool> {
        self.clock(now)?;
        self.last_now = now;
        let slot = self.send_cursor;
        self.send_cursor = (slot + 1) % Q;
        let Some(mut job) = self.jobs[slot].value else {
            return Ok(false);
        };
        if job.retire || job.token.is_some() || now < job.due {
            return Ok(false);
        }
        if now >= job.deadline || job.attempts >= self.config.max_attempts {
            job.retire = true;
            self.jobs[slot].value = Some(job);
            return Err(Error::Exhausted);
        }
        if !adapter.tx_is_quiescent() {
            return Ok(false);
        }
        let due = now
            .checked_add(self.config.retry_us)
            .ok_or(Error::Exhausted)?;
        let peer = self.peer_now(job.peer, identity, security, capabilities, adapter, now)?;
        let original = job.body.opcode();
        job.parts = route_fragment_count(
            original,
            self.config.width,
            usize::from(peer.link.frame_mtu()),
        )?;
        if job.parts > 1
            && self.jobs.iter().enumerate().any(|(i, s)| {
                i != slot
                    && s.value
                        .is_some_and(|j| j.peer == job.peer && j.parts > 1 && j.bundle != 0)
            })
        {
            return Ok(false);
        }
        security.authorize(
            self.access(&peer, original, AccessDirection::Outbound),
            peer.peer.facts,
        )?;
        let op = if job.parts > 1 {
            ProtocolOpcode::RouteFragment
        } else {
            original
        };
        let access = self.access(&peer, op, AccessDirection::Outbound);
        security.authorize(access, peer.peer.facts)?;
        let (counter, hop) = counters.counters(peer.peer.session)?;
        if job.length == 0 {
            let tx = counter.preview()?;
            let n = 25 + 2 * self.config.width.bytes();
            let mut prefix = [0; 33];
            CommonHeader {
                contract: HeaderContract::C0,
                origin_security: OriginSecurity::O1,
                delivery: DeliveryGuarantee::BestEffort,
                interaction: role(op)?,
                payload_kind: PayloadKind::Control,
                traffic_class: TrafficClass::Q0,
                hop_limit: HopLimit::new(1)?,
            }
            .encode(&mut prefix);
            let w = self.config.width.bytes();
            prefix[3..7].copy_from_slice(&self.config.realm.get().to_be_bytes());
            prefix[7..7 + w]
                .copy_from_slice(&peer.peer.facts.local.address.get().to_be_bytes()[4 - w..]);
            prefix[7 + w..7 + 2 * w]
                .copy_from_slice(&peer.peer.facts.peer.address.get().to_be_bytes()[4 - w..]);
            prefix[7 + 2 * w..11 + 2 * w]
                .copy_from_slice(&peer.peer.facts.local.generation.get().to_be_bytes());
            prefix[11 + 2 * w..15 + 2 * w]
                .copy_from_slice(&peer.peer.facts.peer.generation.get().to_be_bytes());
            prefix[n - 10..n - 2].copy_from_slice(&tx.get().to_be_bytes());
            prefix[n - 2..n].copy_from_slice(&u16::from(op).to_be_bytes());
            let mut body = [0; 99];
            let mut len = job.body.encode(self.config.width, &mut body)?;
            if job.parts > 1 {
                if job.bundle == 0 {
                    job.bundle = tx.get();
                }
                let fragment = RouteFragment::from_body(
                    original,
                    C0TransactionId::new(job.bundle)?,
                    job.parts,
                    job.part,
                    &body[..len],
                )?;
                len = fragment.encode(&mut body)?;
            }
            job.length = security.protect_packet(
                crypto,
                OriginCounterOwner::Transaction(counter),
                peer.peer.session,
                peer.peer.facts,
                access,
                PacketPlan {
                    prefix: &prefix[..n],
                    payload: &body[..len],
                    canonical_identity: &prefix[3..n],
                    origin_context: context(&peer, tx, op, true),
                    hop_profile: HopProfile::H1,
                },
                Some(hop),
                scratch,
                &mut self.jobs[slot].packet,
            )?;
        } else {
            security.refresh_c0_hop(
                crypto,
                peer.peer.session,
                peer.peer.facts,
                access,
                hop,
                scratch,
                &mut self.jobs[slot].packet[..job.length],
            )?;
        }
        if job.length > usize::from(peer.link.frame_mtu()) {
            return Err(Error::NoSpace);
        }
        self.jobs[slot].value = Some(job);
        let token = adapter.tx_reserve(peer.peer.link, 0)?;
        let mut gated = GatedDriver {
            driver,
            gate: self.gate,
            owner: self.config.owner,
        };
        let result = adapter.tx_submit(token, &self.jobs[slot].packet[..job.length], &mut gated);
        job.token = Some(token);
        job.attempts += 1;
        job.due = due;
        self.jobs[slot].value = Some(job);
        self.last_now = now;
        result?;
        Ok(true)
    }
    /// One physical retirement visit, preserving `InDoubt` and reverse obligations.
    /// 单次物理退休访问，保留不确定状态及反向义务。
    /// # Errors
    /// Unknown cancellation never discards a token or completes a route reply.
    pub fn retire<
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const V: usize,
        const RR: usize,
        const SS: usize,
    >(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
        d: &mut impl TxDriver,
        routes: &mut RouteOwner<D, V, RR, SS>,
        now: u64,
    ) -> Result<()> {
        if self.gate.is_active() || now == 0 || now < self.last_now {
            return Err(Error::State);
        }
        self.last_now = now;
        let index = self.retire_cursor;
        self.retire_cursor = (index + 1) % Q;
        let Some(mut j) = self.jobs[index].value else {
            return Ok(());
        };
        let ending = self.stopped
            || j.retire
            || now >= j.deadline
            || j.token.is_none() && j.attempts >= self.config.max_attempts && now >= j.due;
        if let Some(token) = j.token {
            let mut v = a.tx_view(token)?;
            if v.state == TxState::InDoubt {
                // A cancellation API is not a proof for an unknown submit.
                // Only the exact late physical completion can resolve it.
                // 取消不是Unknown提交的证明，只接受精确迟到物理完成。
                return Err(Error::InDoubt);
            }
            if !matches!(
                v.state,
                TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
            ) {
                if !ending {
                    return Ok(());
                }
                let mut gated = GatedDriver {
                    driver: d,
                    gate: self.gate,
                    owner: self.config.owner,
                };
                v = a.tx_cancel(token, &mut gated)?;
            }
            if !matches!(
                v.state,
                TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
            ) {
                return Err(Error::InDoubt);
            }
            let success = v.terminal == Some(TerminalOutcome::Success);
            a.tx_retire(token)?;
            j.token = None;
            let last = j.parts <= 1 || j.part + 1 >= j.parts;
            j.delivered = success && last;
            if success {
                if !last {
                    j.part += 1;
                    j.due = now;
                } else if !matches!(j.body, RouteControl::Request(_)) {
                    j.retire = true;
                } else {
                    j.part = 0;
                    j.parts = 0;
                    j.bundle = 0;
                }
            }
            // A new route-control attempt is a new per-peer C0 transaction;
            // only the inner discovery key is stable. A receiver deliberately
            // does not expose plaintext for a committed Origin replay.
            // 路由重试使用新逐邻居C0事务，内层发现键不变；不绕过Origin重放门禁。
            j.length = 0;
            self.jobs[index].value = Some(j);
        }
        if ending || j.retire {
            if let Some(h) = j.completion {
                routes.complete_rrep_forward(h, j.delivered)?;
            }
            self.jobs[index].value = None;
        }
        self.last_now = now;
        Ok(())
    }
    /// Explicit peer expiry/fence work, at most one table entry per call.
    /// 显式邻居到期/围栏维护，每次最多一项。
    /// # Errors
    /// Reentry, stale clock, full error queue or unusable upstream rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn maintain<
        const N: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const V: usize,
        const RR: usize,
        const SS: usize,
    >(
        &mut self,
        routes: &mut RouteOwner<D, V, RR, SS>,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        caps: &CapabilityOwner<C>,
        a: &AdapterOwner<'_, L, T, X, B>,
        now: u64,
    ) -> Result<()> {
        self.clock(now)?;
        self.last_now = now;
        let index = self.maintenance_cursor;
        self.maintenance_cursor = (index + 1) % (P + Q);
        if index < P {
            if self.assemblies[index].active && now >= self.assemblies[index].deadline {
                self.assemblies[index].active = false;
            }
            if let Some(mut p) = self.peers[index] {
                let h = self.handle(index);
                if self.peer_now(h, identity, security, caps, a, now).is_err() {
                    self.assemblies[index].active = false;
                    p.fenced = true;
                    self.peers[index] = Some(p);
                    let _ = routes.invalidate_peer(p.link);
                    for slot in &mut self.jobs {
                        if let Some(j) = slot.value.as_mut() {
                            if j.peer == h {
                                j.retire = true;
                            }
                        }
                    }
                    for receipt in self.receipts.iter_mut().flatten() {
                        if receipt.peer == h {
                            receipt.failed = true;
                        }
                    }
                }
            }
        } else {
            let index = index - P;
            if let Some(r) = self.receipts[index] {
                if r.failed {
                    if let Some(h) = r.upstream {
                        if self.peers[h.slot].is_none_or(|p| p.fenced) {
                            // Best-effort diagnostic has no accepted physical
                            // holder yet; an unusable upstream cannot hear it.
                            // 未提交的尽力诊断没有物理义务，上游已围栏时可释放元数据。
                            self.receipts[index] = None;
                            return Ok(());
                        }
                        if self.free() == 0 {
                            return Err(Error::NoSpace);
                        }
                        let p = self.peer_now(h, identity, security, caps, a, now)?;
                        security.authorize(
                            self.access(&p, ProtocolOpcode::RouteRerr, AccessDirection::Outbound),
                            p.peer.facts,
                        )?;
                        let mut e =
                            routes.make_rerr(r.local, ucn_routing::RerrReason::LinkInvalid)?;
                        e.reporter_principal = p.peer.facts.local.principal;
                        e.reporter_address = p.peer.facts.local.address.get();
                        e.reporter_binding_generation = p.peer.facts.local.generation.get();
                        let deadline = now
                            .checked_add(self.config.lifetime_us)
                            .ok_or(Error::Exhausted)?;
                        self.enqueue(h, RouteControl::Error(e), now, deadline, None)?;
                    }
                    self.receipts[index] = None;
                } else {
                    let d = r.local.domain();
                    if routes.resolve(
                        d,
                        RouteUseFacts {
                            now_us: now,
                            origin_session_generation: d.origin_session_generation,
                            destination_binding_generation: d.destination.generation.get(),
                            link_generation: r.local.next_hop().link_generation(),
                        },
                    ) != Ok(ResolvedRoute::Dynamic(r.local))
                    {
                        self.receipts[index] = None;
                    }
                }
            }
        }
        let _ = routes.expire_one(now);
        self.last_now = now;
        Ok(())
    }
    /// Stop without erasing accepted physical work. 停止但不抹除物理工作。
    /// # Errors
    /// Wrong Adapter or callback reentry rejects.
    pub fn stop<const L: usize, const T: usize, const X: usize>(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
    ) -> Result<()> {
        if self.gate.is_active()
            || self
                .peers
                .iter()
                .flatten()
                .any(|p| p.peer.link.adapter_instance() != a.adapter_instance())
        {
            return Err(Error::State);
        }
        a.try_set_rx_enabled(false)?;
        self.stopped = true;
        for p in self.peers.iter_mut().flatten() {
            p.fenced = true;
        }
        Ok(())
    }
    /// Physical quiescence only; 仅物理静止。
    #[must_use]
    pub fn quiescent<const L: usize, const T: usize, const X: usize>(
        &self,
        a: &AdapterOwner<'_, L, T, X, B>,
    ) -> bool {
        self.stopped && a.rx_is_quiescent() && self.jobs.iter().all(|s| s.value.is_none())
    }
    /// Fixed peer/job/physical-holder/receipt counts; 固定邻居、任务、物理义务与回执数量。
    #[must_use]
    pub fn counts(&self) -> (usize, usize, usize, usize) {
        (
            self.peers.iter().flatten().count(),
            self.jobs.iter().filter(|s| s.value.is_some()).count(),
            self.jobs
                .iter()
                .filter(|s| s.value.is_some_and(|j| j.token.is_some()))
                .count(),
            self.receipts.iter().flatten().count(),
        )
    }
}
fn binding(v: BindingView) -> Binding {
    let b = v.certificate().binding;
    Binding {
        address: b.address,
        generation: b.generation,
        principal: b.principal.bytes(),
    }
}
fn role(op: ProtocolOpcode) -> Result<InteractionRole> {
    match op {
        ProtocolOpcode::RouteRreq => Ok(InteractionRole::Request),
        ProtocolOpcode::RouteRrep => Ok(InteractionRole::Result),
        ProtocolOpcode::RouteRerr => Ok(InteractionRole::Error),
        ProtocolOpcode::RouteFragment => Ok(InteractionRole::OneWay),
        _ => Err(Error::Unsupported),
    }
}
fn context(
    p: &RegisteredNeighbor,
    tx: C0TransactionId,
    op: ProtocolOpcode,
    outbound: bool,
) -> OriginContext {
    OriginContext::C0 {
        fingerprint: p.peer.fingerprint,
        transaction_id: tx,
        opcode: op,
        direction: if (p.peer.facts.local.principal < p.peer.facts.peer.principal) == outbound {
            SenderDirection::AscendingPrincipal
        } else {
            SenderDirection::DescendingPrincipal
        },
    }
}
