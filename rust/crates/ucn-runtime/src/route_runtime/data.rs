//! One Coordinator owns route control, datagrams, atomic RX and physical retirement.
//! 唯一协调器持有路由控制、数据报、原子RX与物理退休，不建立业务Owner直连。
#![allow(clippy::too_many_arguments, clippy::too_many_lines)] // Explicit bounded Owner composition, not hidden cross-module calls.
use super::{
    JournalPeer, PeerCounterBank, RegisteredNeighbor, RoutePeerHandle, RouteRuntime,
    RouteRuntimeConfig, binding,
};
use crate::runtime_common::GatedDriver;
use ucn_adapter::{
    AdapterOwner, DriverCancel, DriverSubmit, LinkHandle, RxView, TerminalOutcome, TxDriver,
    TxState, TxToken,
};
use ucn_capability::{CapabilityOwner, PeerCapabilityRef};
use ucn_identity::{AuthorityEpoch, BindingHandle, BindingView, IdentityOwner};
use ucn_owner::CallbackGate;
use ucn_routing::{
    ResolvedRoute, RouteConfig, RouteDomain, RouteOwner, RouteUseFacts, SoftRouteView,
};
use ucn_security::{
    AccessDirection, AccessRequest, Binding, CryptoProvider, CurrentFacts, Fingerprint,
    OpenDisposition, OpenPacketPlan, OriginContext, OriginSequenceOwner, PacketPlan, SecurityOwner,
    SecurityPacketWorkspace, SessionHandle,
};
use ucn_types::{
    DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile, InteractionRole, NodeAddress,
    OriginSecurity, OriginSequence, PayloadKind, Result, ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;
mod endpoint;
mod neighbor;

const SCHEDULE: [usize; 12] = [0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3];

/// Driver-local peer locator, never a new Wire field or an authentication claim.
/// Driver本地邻居定位，不是新增Wire字段，也不是认证凭据。
pub trait PeerTxDriver: TxDriver {
    /// Address the exact registered neighbour on a shared physical Link.
    /// 在共享物理Link上发送给精确登记邻居。
    fn submit_peer(
        &mut self,
        link: LinkHandle,
        peer_locator: u32,
        frame: &[u8],
        token: TxToken,
    ) -> DriverSubmit;
}
struct Directed<'a, D> {
    driver: &'a mut D,
    locator: u32,
}
impl<D: PeerTxDriver> TxDriver for Directed<'_, D> {
    fn submit(&mut self, l: LinkHandle, b: &[u8], t: TxToken) -> DriverSubmit {
        self.driver.submit_peer(l, self.locator, b, t)
    }
    fn cancel(&mut self, t: TxToken) -> DriverCancel {
        self.driver.cancel(t)
    }
}
/// Shared Origin counters as well as existing per-neighbour C0/Hop counters.
/// 共享Origin计数器，以及已有逐邻居C0/Hop计数器。
pub trait RoutedCounterBank: PeerCounterBank {
    /// One ordinary Origin sequence space per exact Session, not per Service.
    /// 每个精确Session一个普通Origin序列域，不按Service各自从1开始。
    /// # Errors
    /// Missing/retired/exhausted Session rejects.
    fn origin(&mut self, session: SessionHandle) -> Result<&mut dyn OriginSequenceOwner>;
}
/// Fixed product limits; no frame-format choices exposed to an ordinary send.
/// 固定产品上限，普通发送不需要选择报文格式。
#[derive(Clone, Copy)]
pub struct DatagramConfig {
    /// Local nonrenewing queue/receive lifetime; 本地不续期队列/接收寿命。
    pub lifetime_us: u64,
    /// Initial TTL; 初始TTL。
    pub max_hops: HopLimit,
    /// Authenticated path capabilities; 路径必须具备的认证能力。
    pub required_capability_bits: u16,
}
/// Exact local Endpoint policy; not a remotely provided priority request.
/// 精确本地Endpoint策略，不接受远端提升优先级。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DatagramEndpointPolicy {
    /// Service; 服务。
    pub service: ServiceId,
    /// Highest admitted class; Q0 is reserved for control. 最高准入类别，Q0保留控制。
    pub traffic_ceiling: TrafficClass,
    /// Maximum application bytes; 最大业务字节数。
    pub max_payload_bytes: u16,
}
/// Opaque Endpoint registration; 不透明Endpoint登记。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DatagramEndpointHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
impl DatagramEndpointHandle {
    /// Process-local owner; not an on-wire authorization credential.
    #[must_use]
    pub const fn owner_instance(self) -> u32 {
        self.owner
    }
    /// Exact immutable registration generation.
    #[must_use]
    pub const fn generation(self) -> u32 {
        self.generation
    }
    /// Bounded local registry index.
    #[must_use]
    pub const fn slot(self) -> usize {
        self.slot
    }
}
/// Current validated Endpoint metadata, never a caller-issued authentication fact.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DatagramEndpointView {
    /// Exact local Binding.
    pub local: Binding,
    /// Authenticated remote Origin, not the adjacent forwarding Peer.
    pub remote: Binding,
    /// Fixed local Service policy.
    pub policy: DatagramEndpointPolicy,
    /// Actual current Origin protection.
    pub protection: OriginSecurity,
}
#[derive(Clone, Copy)]
struct Endpoint {
    session: SessionHandle,
    facts: CurrentFacts,
    fingerprint: Fingerprint,
    parents: [BindingHandle; 2],
    views: [BindingView; 2],
    authority: AuthorityEpoch,
    policy: DatagramEndpointPolicy,
    session_generation: ucn_types::PeerSessionGeneration,
    protection: OriginSecurity,
    fenced: bool,
}
/// Ordinary best-effort copy request. 普通尽力而为复制请求。
#[derive(Clone, Copy)]
pub struct DatagramSendOptions {
    /// User class subject to local policy; 受本地策略约束的类别。
    pub traffic_class: TrafficClass,
    /// Absolute local deadline, never renewed by discovery; 本地绝对期限，寻路不续期。
    pub deadline_us: u64,
    /// Retain a queryable local result until explicit forget; 保留本地可查询结果至显式forget。
    pub tracked: bool,
}
/// Exact local TX receipt; 精确本地TX回执。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DatagramTxHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// Local outcome only, never an end-to-end receipt. 仅本地结果，不冒充端到端回执。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DatagramOutcome {
    /// Local Driver reported success; 本地Driver报告成功。
    LocalSuccess,
    /// Local rejection/failure; 本地拒绝或失败。
    Failed(Error),
    /// Stop/cancel; 停止或取消。
    Cancelled,
    /// Submit may have happened; 保留可能发生提交的不确定结果。
    InDoubt,
}
/// Query snapshot separates logical result and physical holder.
/// 查询快照分离逻辑结果与物理义务。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DatagramTxView {
    /// Still waiting for a current route; 等待当前路径。
    pub waiting_route: bool,
    /// Local outcome, if known; 已知的本地结果。
    pub outcome: Option<DatagramOutcome>,
    /// Actual Adapter token still owned; 仍持有实际Adapter Token。
    pub physical_pending: bool,
}
#[derive(Clone, Copy)]
struct DataJob {
    endpoint: Option<DatagramEndpointHandle>,
    tracked: bool,
    traffic: TrafficClass,
    route: Option<SoftRouteView>,
    peer: Option<RoutePeerHandle>,
    payload_bytes: usize,
    frame_bytes: usize,
    deadline: u64,
    due: u64,
    attempts: u16,
    token: Option<TxToken>,
    outcome: Option<DatagramOutcome>,
}
struct DataSlot<const B: usize> {
    generation: u32,
    value: Option<DataJob>,
    bytes: [u8; B],
}
impl<const B: usize> DataSlot<B> {
    const EMPTY: Self = Self {
        generation: 0,
        value: None,
        bytes: [0; B],
    };
}
#[derive(Clone, Copy)]
struct AppMessage {
    endpoint: DatagramEndpointHandle,
    origin: Binding,
    service: ServiceId,
    traffic: TrafficClass,
    sequence: u32,
    received: u64,
    bytes: usize,
    deadline: u64,
    order: u64,
}
struct AppSlot<const B: usize> {
    value: Option<AppMessage>,
    bytes: [u8; B],
}
impl<const B: usize> AppSlot<B> {
    const EMPTY: Self = Self {
        value: None,
        bytes: [0; B],
    };
}
/// A single successfully pulled application message. 一次成功取出的业务消息。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DatagramReceived {
    /// Exact Origin Endpoint registration used for authentication.
    pub endpoint: DatagramEndpointHandle,
    /// Original physical RX capture, not the later pull time.
    pub receive_time_us: u64,
    /// Actual Origin protection, independent of the forwarding Hop.
    pub protection: OriginSecurity,
    /// Authenticated original Binding, not the neighbour; 原始认证Binding，不是邻居。
    pub origin: Binding,
    /// Exact Service; 精确服务。
    pub service: ServiceId,
    /// Policy-checked class; 经过策略检查的类别。
    pub traffic_class: TrafficClass,
    /// Original sequence; 原始序号。
    pub origin_sequence: u32,
    /// Copied bytes; 已复制字节数。
    pub bytes: usize,
}
/// Counters deliberately describe distinct funnels. 统计明确区分不同数据路径。
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RoutedStats {
    /// Locally accepted source datagrams; 本地接纳源数据报。
    pub source_accepted: u64,
    /// Locally accepted relay datagrams; 本地接纳转发数据报。
    pub relay_accepted: u64,
    /// Actual Driver success, not remote application; 实际Driver成功，不是远端业务交付。
    pub local_tx_success: u64,
    /// Rejected/expired/cancelled TX jobs; 拒绝、到期或取消的TX工作。
    pub tx_failed: u64,
    /// Authenticated into application queue; 认证后进入业务队列。
    pub rx_accepted: u64,
    /// Copied out once by the application; 业务实际取出一次。
    pub app_pulled: u64,
    /// Rejected raw inputs; 拒绝的原始输入。
    pub rx_rejected: u64,
    /// Expired/discarded application queue entries; 到期/停止丢弃的业务队列项。
    pub app_discarded: u64,
}
/// Bounded visit report, independent of the number of successful sends.
/// 有界访问报告，不用成功发送次数冒充预算。
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RoutedStep {
    /// Consumed visits; 消耗的访问预算。
    pub visits: usize,
    /// Last rejected operation, without stopping other classes; 最近拒绝，不阻塞其他类。
    pub last_error: Option<Error>,
}

/// Autonomous known-Endpoint datagram Coordinator with exactly one ingress owner.
/// 已知Endpoint的自动数据报协调器，唯一入口持有者。
pub struct RoutedNode<
    'g,
    const P: usize,
    const C: usize,
    const Q: usize,
    const A: usize,
    const E: usize,
    const B: usize,
    const D: usize,
    const V: usize,
    const RR: usize,
    const SS: usize,
> {
    control: RouteRuntime<'g, P, C, B>,
    routes: RouteOwner<D, V, RR, SS>,
    config: DatagramConfig,
    endpoints: [Option<Endpoint>; E],
    endpoint_generations: [u32; E],
    jobs: [DataSlot<B>; Q],
    app: [AppSlot<B>; A],
    raw: [u8; B],
    prepared: [u8; B],
    pending_rx: Option<RxView>,
    cursor: usize,
    prepare_cursor: usize,
    retire_cursor: usize,
    maintenance_cursor: usize,
    class_cursor: usize,
    tx_cursor: [usize; 4],
    next_order: u64,
    last_now: u64,
    stopped: bool,
    stats: RoutedStats,
}
impl<
    'g,
    const P: usize,
    const C: usize,
    const Q: usize,
    const A: usize,
    const E: usize,
    const B: usize,
    const D: usize,
    const V: usize,
    const RR: usize,
    const SS: usize,
> RoutedNode<'g, P, C, Q, A, E, B, D, V, RR, SS>
{
    /// Construct empty fixed storage; never adopts or drops an already-live Root.
    /// 构造空固定存储，不接管或丢弃已有活动Root。
    /// # Errors
    /// Invalid capacity, lifetime, TTL or route/config domain rejects.
    pub fn new(
        control: RouteRuntimeConfig,
        routes: RouteConfig,
        first_discovery: u64,
        config: DatagramConfig,
        gate: &'g CallbackGate,
    ) -> Result<Self> {
        if Q == 0
            || Q > 64
            || A == 0
            || A > 64
            || E == 0
            || E > 32
            || config.lifetime_us == 0
            || config.lifetime_us > i64::MAX as u64
            || config.max_hops.get() > routes.maximum_hops.get()
            || control.realm != routes.realm
            || control.width != routes.address_width
        {
            return Err(Error::Config);
        }
        Ok(Self {
            control: RouteRuntime::new(control, gate)?,
            routes: RouteOwner::new(routes, first_discovery)?,
            config,
            endpoints: [None; E],
            endpoint_generations: [0; E],
            jobs: [const { DataSlot::EMPTY }; Q],
            app: [const { AppSlot::EMPTY }; A],
            raw: [0; B],
            prepared: [0; B],
            pending_rx: None,
            cursor: 0,
            prepare_cursor: 0,
            retire_cursor: 0,
            maintenance_cursor: 0,
            class_cursor: 0,
            tx_cursor: [0; 4],
            next_order: 1,
            last_now: 0,
            stopped: false,
            stats: RoutedStats::default(),
        })
    }
    fn clock(&mut self, now: u64, stopped_ok: bool) -> Result<()> {
        if self.control.gate.is_active()
            || now == 0
            || now < self.last_now
            || self.stopped && !stopped_ok
        {
            return Err(Error::State);
        }
        self.last_now = now;
        Ok(())
    }
    fn endpoint_index(&self, h: DatagramEndpointHandle) -> Result<usize> {
        if h.owner != self.control.config.owner
            || h.slot >= E
            || h.generation != self.endpoint_generations[h.slot]
            || self.endpoints[h.slot].is_none()
        {
            return Err(Error::NotFound);
        }
        Ok(h.slot)
    }
    fn endpoint_now<const N: usize, const S: usize, const R: usize>(
        &self,
        h: DatagramEndpointHandle,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        now: u64,
    ) -> Result<Endpoint> {
        let mut e = self.endpoints[self.endpoint_index(h)?].ok_or(Error::NotFound)?;
        if e.fenced
            || !core::ptr::eq(identity.callback_gate(), self.control.gate)
            || !core::ptr::eq(security.callback_gate(), self.control.gate)
            || identity.authority_get(now)? != e.authority
            || [
                identity.binding_get(e.parents[0], now)?,
                identity.binding_get(e.parents[1], now)?,
            ] != e.views
        {
            return Err(Error::State);
        }
        e.facts.now_us = now;
        let session = security.session_get(e.session, e.facts)?;
        if !matches!(session.hop_profile, HopProfile::H0 | HopProfile::H1)
            || session.origin_security == OriginSecurity::O0
            || security.context_fingerprints(e.session, e.facts)?.0 != e.fingerprint
        {
            return Err(Error::Security);
        }
        Ok(e)
    }
    fn endpoint_access(e: &Endpoint, direction: AccessDirection) -> AccessRequest {
        AccessRequest {
            session: e.session,
            context_fingerprint: e.fingerprint,
            service: e.policy.service,
            protocol_opcode: 0,
            direction,
        }
    }
    /// Read the fixed local Binding without manufacturing a remote grant.
    #[must_use]
    pub const fn local_binding(&self) -> Binding {
        self.routes.config().local
    }
    /// Product cap for one locally accepted datagram; a facade may only shorten deadlines.
    #[must_use]
    pub const fn maximum_local_lifetime_us(&self) -> u64 {
        self.config.lifetime_us
    }
    /// Revalidate exact Endpoint parents, Session and the chosen current ACL.
    /// # Errors
    /// Stopped, expired, fenced, wrong generation, parent or ACL rejects.
    pub fn authorize_endpoint<const N: usize, const S: usize, const R: usize>(
        &self,
        handle: DatagramEndpointHandle,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        direction: AccessDirection,
        now: u64,
    ) -> Result<DatagramEndpointView> {
        if self.stopped || self.control.gate.is_active() || now == 0 || now < self.last_now {
            return Err(Error::State);
        }
        let e = self.endpoint_now(handle, identity, security, now)?;
        security.authorize(Self::endpoint_access(&e, direction), e.facts)?;
        Ok(DatagramEndpointView {
            local: e.facts.local,
            remote: e.facts.peer,
            policy: e.policy,
            protection: e.protection,
        })
    }
    /// Register an actual authenticated adjacent peer. 登记实际认证邻居。
    /// # Errors
    /// Stale Identity/Session/Capability, ambiguous locator or fixed capacity rejects.
    pub fn register_neighbor<
        const N: usize,
        const S: usize,
        const R: usize,
        const K: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &mut self,
        peer: JournalPeer,
        parents: [BindingHandle; 2],
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        caps: &CapabilityOwner<K>,
        capability: PeerCapabilityRef,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        cost: u32,
        locator: u32,
        now: u64,
    ) -> Result<RoutePeerHandle> {
        self.clock(now, false)?;
        if peer.facts.local != self.routes.config().local {
            return Err(Error::Config);
        }
        self.control.register(
            peer, parents, identity, security, caps, capability, adapter, cost, locator, now,
        )
    }
    /// Register an exact durable Origin Session, independent of next-hop selection.
    /// 登记精确耐久Origin Session；直连可共享真实H1邻居Session，多跳独立选择下一跳。
    /// # Errors
    /// Wrong actual parents, Realm, Session, profile, policy or full table rejects.
    pub fn register_endpoint<const N: usize, const S: usize, const R: usize>(
        &mut self,
        session: SessionHandle,
        facts: CurrentFacts,
        fingerprint: Fingerprint,
        parents: [BindingHandle; 2],
        policy: DatagramEndpointPolicy,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        now: u64,
    ) -> Result<DatagramEndpointHandle> {
        self.clock(now, false)?;
        let endpoint = self.make_endpoint(
            session,
            facts,
            fingerprint,
            parents,
            policy,
            identity,
            security,
            now,
        )?;
        if self.endpoints.iter().flatten().any(|e| {
            (e.facts.peer.principal == facts.peer.principal
                || e.facts.peer.address == facts.peer.address)
                && (e.session != session || e.policy.service == policy.service)
        }) {
            return Err(Error::State);
        }
        let slot = self
            .endpoints
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.endpoint_generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.endpoints[slot] = Some(endpoint);
        self.endpoint_generations[slot] = generation;
        Ok(DatagramEndpointHandle {
            owner: self.control.config.owner,
            slot,
            generation,
        })
    }
    #[allow(clippy::too_many_arguments)]
    fn make_endpoint<const N: usize, const S: usize, const R: usize>(
        &self,
        session: SessionHandle,
        facts: CurrentFacts,
        fingerprint: Fingerprint,
        parents: [BindingHandle; 2],
        policy: DatagramEndpointPolicy,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        now: u64,
    ) -> Result<Endpoint> {
        if facts.now_us != now
            || facts.local != self.routes.config().local
            || policy.traffic_ceiling == TrafficClass::Q0
            || policy.max_payload_bytes == 0
            || usize::from(policy.max_payload_bytes)
                + 9
                + 2 * self.control.config.width.bytes()
                + 32
                > B
            || !core::ptr::eq(identity.callback_gate(), self.control.gate)
            || !core::ptr::eq(security.callback_gate(), self.control.gate)
            || security.realm() != self.control.config.realm
            || security.address_width() != self.control.config.width
        {
            return Err(Error::Config);
        }
        let views = [
            identity.binding_get(parents[0], now)?,
            identity.binding_get(parents[1], now)?,
        ];
        let authority = identity.authority_get(now)?;
        if authority.realm != self.control.config.realm
            || binding(views[0]) != facts.local
            || binding(views[1]) != facts.peer
            || views.iter().any(|v| {
                v.runtime_instance() != security.runtime_instance()
                    || v.certificate().binding.realm != self.control.config.realm
                    || v.certificate().binding.address_width != self.control.config.width
            })
        {
            return Err(Error::Access);
        }
        let view = security.session_get(session, facts)?;
        if !matches!(view.hop_profile, HopProfile::H0 | HopProfile::H1)
            || view.origin_security == OriginSecurity::O0
            || security.context_fingerprints(session, facts)?.0 != fingerprint
        {
            return Err(Error::Security);
        }
        Ok(Endpoint {
            session,
            facts,
            fingerprint,
            parents,
            views,
            authority,
            policy,
            session_generation: view.session_generation,
            protection: view.origin_security,
            fenced: false,
        })
    }
    /// Copy one best-effort `OneWay` Data request; unknown route is discovered automatically.
    /// 复制一次尽力而为单向Data请求；未知路径自动发现。
    /// # Errors
    /// All policy/parent/deadline/capacity checks precede copying or consuming counters.
    pub fn send<const N: usize, const S: usize, const R: usize>(
        &mut self,
        h: DatagramEndpointHandle,
        payload: &[u8],
        options: DatagramSendOptions,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        now: u64,
    ) -> Result<Option<DatagramTxHandle>> {
        self.clock(now, false)?;
        let e = self.endpoint_now(h, identity, security, now)?;
        let limit = now
            .checked_add(self.config.lifetime_us)
            .ok_or(Error::Exhausted)?;
        if options.deadline_us <= now
            || options.deadline_us > limit
            || options.traffic_class == TrafficClass::Q0
            || (options.traffic_class as u8) < (e.policy.traffic_ceiling as u8)
            || payload.len() > usize::from(e.policy.max_payload_bytes)
        {
            return Err(Error::Policy);
        }
        security.authorize(
            Self::endpoint_access(&e, AccessDirection::Outbound),
            e.facts,
        )?;
        let slot = self
            .jobs
            .iter()
            .position(|s| s.value.is_none())
            .ok_or(Error::NoSpace)?;
        let generation = self.jobs[slot]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.jobs[slot].bytes[..payload.len()].copy_from_slice(payload);
        self.jobs[slot].generation = generation;
        self.jobs[slot].value = Some(DataJob {
            endpoint: Some(h),
            tracked: options.tracked,
            traffic: options.traffic_class,
            route: None,
            peer: None,
            payload_bytes: payload.len(),
            frame_bytes: 0,
            deadline: options.deadline_us,
            due: now,
            attempts: 0,
            token: None,
            outcome: None,
        });
        self.stats.source_accepted = self.stats.source_accepted.saturating_add(1);
        Ok(options.tracked.then_some(DatagramTxHandle {
            owner: self.control.config.owner,
            slot,
            generation,
        }))
    }
    fn job_index(&self, h: DatagramTxHandle) -> Result<usize> {
        if h.owner != self.control.config.owner
            || h.slot >= Q
            || self.jobs[h.slot].generation != h.generation
            || !self.jobs[h.slot].value.is_some_and(|j| j.tracked)
        {
            return Err(Error::NotFound);
        }
        Ok(h.slot)
    }
    /// Local query never claims remote delivery. 本地查询不冒充远端交付。
    /// # Errors
    /// Wrong/forgotten handle or callback reentry rejects.
    pub fn query(&self, h: DatagramTxHandle) -> Result<DatagramTxView> {
        if self.control.gate.is_active() {
            return Err(Error::State);
        }
        let j = self.jobs[self.job_index(h)?].value.ok_or(Error::NotFound)?;
        Ok(DatagramTxView {
            waiting_route: j.frame_bytes == 0 && j.outcome.is_none(),
            outcome: j.outcome,
            physical_pending: j.token.is_some(),
        })
    }
    /// Forget only terminal, physically retired tracked work. 只释放终态且物理已退休的跟踪工作。
    /// # Errors
    /// Active/unknown holder, callback or stale handle rejects.
    pub fn forget(&mut self, h: DatagramTxHandle) -> Result<()> {
        if self.control.gate.is_active() {
            return Err(Error::State);
        }
        let i = self.job_index(h)?;
        let j = self.jobs[i].value.ok_or(Error::NotFound)?;
        if j.outcome.is_none() || j.token.is_some() {
            return Err(Error::State);
        }
        self.jobs[i].value = None;
        self.jobs[i].bytes.fill(0);
        Ok(())
    }
    fn finish(&mut self, i: usize, outcome: DatagramOutcome) {
        if let Some(mut j) = self.jobs[i].value {
            if j.outcome.is_none() {
                j.outcome = Some(outcome);
                if outcome == DatagramOutcome::LocalSuccess {
                    self.stats.local_tx_success = self.stats.local_tx_success.saturating_add(1);
                } else {
                    self.stats.tx_failed = self.stats.tx_failed.saturating_add(1);
                }
            }
            self.jobs[i].value = Some(j);
            if !j.tracked && j.token.is_none() {
                self.jobs[i].value = None;
                self.jobs[i].bytes.fill(0);
            }
        }
    }
    fn route_domain(&self, e: &Endpoint) -> RouteDomain {
        RouteDomain {
            realm: self.control.config.realm,
            origin: e.facts.local,
            origin_session_generation: self.routes.config().local_session_generation,
            destination: e.facts.peer,
        }
    }
    fn route(&self, domain: RouteDomain, now: u64) -> Result<SoftRouteView> {
        for p in self.control.peers.iter().flatten().filter(|p| !p.fenced) {
            if let Ok(ResolvedRoute::Dynamic(r)) = self.routes.resolve(
                domain,
                RouteUseFacts {
                    now_us: now,
                    origin_session_generation: domain.origin_session_generation,
                    destination_binding_generation: domain.destination.generation.get(),
                    link_generation: p.link.link_generation(),
                },
            ) {
                if r.next_hop() == p.link {
                    return Ok(r);
                }
            }
        }
        Err(Error::NotFound)
    }
    fn current_route(&self, r: SoftRouteView, now: u64) -> Result<RoutePeerHandle> {
        if self.route(r.domain(), now)? != r {
            return Err(Error::State);
        }
        self.control.peer_link(r.next_hop())
    }
    fn hop_request<const S: usize, const R: usize>(
        p: &RegisteredNeighbor,
        service: ServiceId,
        security: &SecurityOwner<'_, S, R>,
        direction: AccessDirection,
    ) -> Result<AccessRequest> {
        Ok(AccessRequest {
            session: p.peer.session,
            context_fingerprint: security
                .context_fingerprints(p.peer.session, p.peer.facts)?
                .1,
            service,
            protocol_opcode: 0,
            direction,
        })
    }
    fn prepare_one<
        const N: usize,
        const S: usize,
        const R: usize,
        const K: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const W: usize,
    >(
        &mut self,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, S, R>,
        caps: &CapabilityOwner<K>,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        crypto: &mut impl CryptoProvider,
        counters: &mut impl RoutedCounterBank,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
    ) -> Result<()> {
        let slot = self.prepare_cursor;
        self.prepare_cursor = (slot + 1) % Q;
        let Some(mut job) = self.jobs[slot].value else {
            return Ok(());
        };
        if job.outcome.is_some() || job.token.is_some() || job.frame_bytes != 0 || now < job.due {
            return Ok(());
        }
        if self.stopped || now >= job.deadline {
            self.finish(
                slot,
                if self.stopped {
                    DatagramOutcome::Cancelled
                } else {
                    DatagramOutcome::Failed(Error::Timeout)
                },
            );
            return Ok(());
        }
        let Some(endpoint_handle) = job.endpoint else {
            return Err(Error::State);
        };
        let endpoint = match self.endpoint_now(endpoint_handle, identity, security, now) {
            Ok(endpoint) => endpoint,
            Err(err) => {
                self.finish(slot, DatagramOutcome::Failed(err));
                return Err(err);
            }
        };
        let frame_bytes = 9 + 2 * self.control.config.width.bytes() + job.payload_bytes + 32;
        // An exact registered neighbour proves a direct path, not an RREQ hint.
        // Otherwise require the complete RREP receipt for this Origin domain.
        // 精确登记邻居证明直连路径；非邻居必须有本Origin域的完整RREP回执。
        let direct =
            self.control.peers.iter().position(|p| {
                p.is_some_and(|p| !p.fenced && p.peer.facts.peer == endpoint.facts.peer)
            });
        let (route, peer_handle) = if let Some(slot) = direct {
            (None, self.control.handle(slot))
        } else {
            match self.route(self.route_domain(&endpoint), now) {
                Ok(resolved)
                    if usize::from(resolved.path_frame_mtu()) >= frame_bytes
                        && self
                            .control
                            .receipts
                            .iter()
                            .flatten()
                            .any(|receipt| !receipt.failed && receipt.local == resolved) =>
                {
                    (Some(resolved), self.current_route(resolved, now)?)
                }
                _ => {
                    match self.control.discover(
                        &mut self.routes,
                        identity,
                        security,
                        caps,
                        adapter,
                        endpoint.facts.peer.address,
                        self.config.required_capability_bits,
                        u16::try_from(frame_bytes).map_err(|_| Error::NoSpace)?,
                        now,
                    ) {
                        Ok(_)
                        | Err(Error::NoSpace | Error::State | Error::NotFound | Error::Timeout) => {
                            return Ok(());
                        }
                        Err(err) => return Err(err),
                    }
                }
            }
        };
        let peer = self
            .control
            .peer_now(peer_handle, identity, security, caps, adapter, now)?;
        if route.is_none()
            && self.config.required_capability_bits & !peer.link.capability_bits() != 0
        {
            self.finish(slot, DatagramOutcome::Failed(Error::Unsupported));
            return Err(Error::Unsupported);
        }
        if frame_bytes > usize::from(peer.link.frame_mtu()) {
            self.finish(slot, DatagramOutcome::Failed(Error::NoSpace));
            return Err(Error::NoSpace);
        }
        let hop_access = Self::hop_request(
            &peer,
            endpoint.policy.service,
            security,
            AccessDirection::Outbound,
        )?;
        security.authorize(hop_access, peer.peer.facts)?;
        let origin = security
            .session_get(endpoint.session, endpoint.facts)?
            .origin_security;
        let sequence = counters.origin(endpoint.session)?.preview()?;
        let address_bytes = self.control.config.width.bytes();
        let prefix_bytes = 9 + 2 * address_bytes;
        let mut prefix = [0; 17];
        CommonHeader {
            contract: HeaderContract::C1,
            traffic_class: job.traffic,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::OneWay,
            payload_kind: PayloadKind::Data,
            origin_security: origin,
            hop_limit: self.config.max_hops,
        }
        .encode(&mut prefix);
        prefix[3..3 + address_bytes].copy_from_slice(
            &endpoint.facts.local.address.get().to_be_bytes()[4 - address_bytes..],
        );
        prefix[3 + address_bytes..3 + 2 * address_bytes]
            .copy_from_slice(&endpoint.facts.peer.address.get().to_be_bytes()[4 - address_bytes..]);
        prefix[prefix_bytes - 6..prefix_bytes - 4]
            .copy_from_slice(&endpoint.policy.service.get().to_be_bytes());
        prefix[prefix_bytes - 4..prefix_bytes].copy_from_slice(&sequence.get().to_be_bytes());
        let canonical = c1_identity(
            endpoint.facts.local,
            endpoint.facts.peer,
            endpoint.policy.service,
            sequence,
        );
        let result = (|| {
            security.authorize(
                Self::endpoint_access(&endpoint, AccessDirection::Outbound),
                endpoint.facts,
            )?;
            let length = security.protect_c1_origin(
                crypto,
                counters.origin(endpoint.session)?,
                endpoint.session,
                endpoint.facts,
                Self::endpoint_access(&endpoint, AccessDirection::Outbound),
                PacketPlan {
                    prefix: &prefix[..prefix_bytes],
                    payload: &self.jobs[slot].bytes[..job.payload_bytes],
                    canonical_identity: &canonical,
                    origin_context: OriginContext::Sequenced {
                        contract: HeaderContract::C1,
                        fingerprint: endpoint.fingerprint,
                        sequence,
                    },
                    hop_profile: HopProfile::H0,
                },
                scratch,
                &mut self.prepared,
            )?;
            let (_, hop) = counters.counters(peer.peer.session)?;
            security.wrap_c1_hop(
                crypto,
                peer.peer.session,
                peer.peer.facts,
                hop_access,
                &self.prepared[..length],
                hop,
                scratch,
                &mut self.jobs[slot].bytes,
            )
        })();
        match result {
            Ok(length) => {
                job.route = route;
                job.peer = Some(peer_handle);
                job.frame_bytes = length;
                self.jobs[slot].value = Some(job);
                Ok(())
            }
            Err(err) => {
                self.finish(slot, DatagramOutcome::Failed(err));
                Err(err)
            }
        }
    }
    fn forward_route(
        &self,
        source: NodeAddress,
        destination: NodeAddress,
        now: u64,
    ) -> Result<SoftRouteView> {
        let mut found = None;
        for receipt in self.control.receipts.iter().flatten().filter(|r| !r.failed) {
            let r = receipt.local;
            let d = r.domain();
            if d.realm == self.control.config.realm
                && d.origin.address == source
                && d.destination.address == destination
                && self.current_route(r, now).is_ok()
            {
                if found.is_some_and(|prior| prior != r) {
                    return Err(Error::Access);
                }
                found = Some(r);
            }
        }
        found.ok_or(Error::NotFound)
    }
    fn receive_data<
        const N: usize,
        const S: usize,
        const R: usize,
        const K: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const W: usize,
    >(
        &mut self,
        claim: RxView,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, S, R>,
        caps: &CapabilityOwner<K>,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        crypto: &mut impl CryptoProvider,
        counters: &mut impl RoutedCounterBank,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
    ) -> Result<()> {
        let bytes = &self.raw[..claim.frame_bytes];
        let header = CommonHeader::decode(bytes)?;
        let width = self.control.config.width;
        let w = width.bytes();
        let prefix = 9 + 2 * w;
        if bytes.len() < prefix + 32
            || header.contract != HeaderContract::C1
            || header.payload_kind != PayloadKind::Data
            || header.interaction != InteractionRole::OneWay
            || header.delivery != DeliveryGuarantee::BestEffort
            || header.traffic_class == TrafficClass::Q0
            || claim.meta.timestamp_us == 0
            || claim.meta.timestamp_us > now
        {
            return Err(Error::Malformed);
        }
        let address = |offset: usize| {
            bytes[offset..offset + w]
                .iter()
                .fold(0u32, |v, b| (v << 8) | u32::from(*b))
        };
        let source = NodeAddress::new(address(3), width)?;
        let destination = NodeAddress::new(address(3 + w), width)?;
        let service = ServiceId::new(u16::from_be_bytes(
            bytes[prefix - 6..prefix - 4]
                .try_into()
                .map_err(|_| Error::Malformed)?,
        ))?;
        let sequence = OriginSequence::new(u32::from_be_bytes(
            bytes[prefix - 4..prefix]
                .try_into()
                .map_err(|_| Error::Malformed)?,
        ))?;
        let pslot = self
            .control
            .peers
            .iter()
            .position(|p| {
                p.is_some_and(|p| {
                    p.peer.link == claim.link && p.discriminator == claim.meta.sender_discriminator
                })
            })
            .ok_or(Error::Access)?;
        let incoming = self.control.handle(pslot);
        let p = self
            .control
            .peer_now(incoming, identity, security, caps, adapter, now)?;
        let hop = security.open_c1_hop(
            crypto,
            p.peer.session,
            p.peer.facts,
            Self::hop_request(&p, service, security, AccessDirection::Inbound)?,
            bytes,
            scratch,
        )?;
        let mut pending = Some(hop);
        let result = (|| {
            let deadline = claim
                .meta
                .timestamp_us
                .checked_add(self.config.lifetime_us)
                .ok_or(Error::Exhausted)?;
            if destination == self.routes.config().local.address {
                let mut selected = None;
                for (i, e) in self.endpoints.iter().enumerate() {
                    if e.is_some_and(|e| {
                        e.policy.service == service
                            && e.facts.peer.address == source
                            && e.facts.local.address == destination
                    }) {
                        let h = DatagramEndpointHandle {
                            owner: self.control.config.owner,
                            slot: i,
                            generation: self.endpoint_generations[i],
                        };
                        let e = self.endpoint_now(h, identity, security, now)?;
                        if selected.is_some() {
                            return Err(Error::Access);
                        }
                        selected = Some((h, e));
                    }
                }
                let (h, e) = selected.ok_or(Error::Access)?;
                if (header.traffic_class as u8) < (e.policy.traffic_ceiling as u8) {
                    return Err(Error::Policy);
                }
                let slot = self
                    .app
                    .iter()
                    .position(|a| a.value.is_none())
                    .ok_or(Error::NoSpace)?;
                let next = self.next_order.checked_add(1).ok_or(Error::Exhausted)?;
                let canonical = c1_identity(e.facts.peer, e.facts.local, service, sequence);
                let opened = security.open_c1_origin(
                    crypto,
                    e.session,
                    e.facts,
                    Self::endpoint_access(&e, AccessDirection::Inbound),
                    OpenPacketPlan {
                        packet: &self.raw[..claim.frame_bytes - 16],
                        prefix_bytes: prefix,
                        canonical_identity: &canonical,
                        origin_context: OriginContext::Sequenced {
                            contract: HeaderContract::C1,
                            fingerprint: e.fingerprint,
                            sequence,
                        },
                        hop_profile: HopProfile::H0,
                    },
                    scratch,
                    &mut self.app[slot].bytes,
                )?;
                let checks = (|| {
                    if opened.disposition != OpenDisposition::FreshAuthenticated {
                        return Err(Error::Replay);
                    }
                    if opened.payload_bytes > usize::from(e.policy.max_payload_bytes) {
                        return Err(Error::NoSpace);
                    }
                    security.replay_preflight(&opened.replay, e.facts)?;
                    security.c1_hop_preflight(
                        pending.as_ref().ok_or(Error::State)?,
                        p.peer.facts,
                        &self.raw[..claim.frame_bytes],
                        scratch,
                    )?;
                    Ok(())
                })();
                if let Err(err) = checks {
                    security.replay_abort(opened.replay)?;
                    return Err(err);
                }
                security.replay_commit(opened.replay, e.facts)?;
                security.commit_c1_hop(pending.take().ok_or(Error::State)?, p.peer.facts)?;
                self.app[slot].value = Some(AppMessage {
                    endpoint: h,
                    origin: e.facts.peer,
                    service,
                    traffic: header.traffic_class,
                    sequence: sequence.get(),
                    received: claim.meta.timestamp_us,
                    bytes: opened.payload_bytes,
                    deadline,
                    order: self.next_order,
                });
                self.next_order = next;
                self.stats.rx_accepted = self.stats.rx_accepted.saturating_add(1);
            } else {
                let route = self.forward_route(source, destination, now)?;
                let outgoing = self.current_route(route, now)?;
                if incoming == outgoing {
                    return Err(Error::Access);
                }
                let out = self
                    .control
                    .peer_now(outgoing, identity, security, caps, adapter, now)?;
                if claim.frame_bytes > usize::from(out.link.frame_mtu())
                    || claim.frame_bytes > usize::from(route.path_frame_mtu())
                {
                    return Err(Error::NoSpace);
                }
                let i = self
                    .jobs
                    .iter()
                    .position(|s| s.value.is_none())
                    .ok_or(Error::NoSpace)?;
                let generation = self.jobs[i]
                    .generation
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?;
                let access = Self::hop_request(&out, service, security, AccessDirection::Outbound)?;
                let (_, hop_counter) = counters.counters(out.peer.session)?;
                let length = security.forward_c1_hop(
                    crypto,
                    pending.as_ref().ok_or(Error::State)?,
                    p.peer.facts,
                    &self.raw[..claim.frame_bytes],
                    out.peer.session,
                    out.peer.facts,
                    access,
                    hop_counter,
                    scratch,
                    &mut self.jobs[i].bytes,
                )?;
                security.commit_c1_hop(pending.take().ok_or(Error::State)?, p.peer.facts)?;
                self.jobs[i].generation = generation;
                self.jobs[i].value = Some(DataJob {
                    endpoint: None,
                    tracked: false,
                    traffic: header.traffic_class,
                    route: Some(route),
                    peer: Some(outgoing),
                    payload_bytes: 0,
                    frame_bytes: length,
                    deadline: deadline.min(route.expires_at_us()),
                    due: now,
                    attempts: 0,
                    token: None,
                    outcome: None,
                });
                self.stats.relay_accepted = self.stats.relay_accepted.saturating_add(1);
            }
            Ok(())
        })();
        if let Some(h) = pending {
            security.abort_c1_hop(h)?;
        }
        result
    }
    fn ingress<
        const N: usize,
        const S: usize,
        const R: usize,
        const K: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const W: usize,
    >(
        &mut self,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, S, R>,
        caps: &CapabilityOwner<K>,
        adapter: &mut AdapterOwner<'_, L, T, X, B>,
        crypto: &mut impl CryptoProvider,
        counters: &mut impl RoutedCounterBank,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
    ) -> Result<()> {
        if self.pending_rx.is_none() {
            match adapter.rx_claim(&mut self.raw) {
                Ok(v) => self.pending_rx = Some(v),
                Err(Error::NotFound) => return Ok(()),
                Err(e) => return Err(e),
            }
        }
        let claim = self.pending_rx.ok_or(Error::State)?;
        adapter.rx_retire(claim.token)?; // Retain exact claim/raw bytes on a transient retire failure.
        self.pending_rx = None;
        if self.stopped {
            return Ok(());
        }
        match CommonHeader::decode(&self.raw[..claim.frame_bytes])?.contract {
            HeaderContract::C0 => {
                self.control.raw[..claim.frame_bytes]
                    .copy_from_slice(&self.raw[..claim.frame_bytes]);
                self.control.receive_loaded(
                    &mut self.routes,
                    identity,
                    security,
                    caps,
                    adapter,
                    crypto,
                    scratch,
                    claim,
                    now,
                )
            }
            HeaderContract::C1 => self.receive_data(
                claim, identity, security, caps, adapter, crypto, counters, scratch, now,
            ),
            _ => Err(Error::Unsupported),
        }
    }
    fn transmit<
        const N: usize,
        const S: usize,
        const R: usize,
        const K: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const W: usize,
    >(
        &mut self,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, S, R>,
        caps: &CapabilityOwner<K>,
        adapter: &mut AdapterOwner<'_, L, T, X, B>,
        crypto: &mut impl CryptoProvider,
        counters: &mut impl RoutedCounterBank,
        driver: &mut impl PeerTxDriver,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
    ) -> Result<()> {
        if self.stopped || !adapter.tx_is_quiescent() {
            return Ok(());
        }
        for _ in 0..12 {
            let class = SCHEDULE[self.class_cursor];
            self.class_cursor = (self.class_cursor + 1) % 12;
            if class == 0 {
                let i = self.control.send_cursor;
                let locator = self.control.jobs[i]
                    .value
                    .and_then(|j| self.control.peers.get(j.peer.slot).copied().flatten())
                    .map_or(0, |p| p.discriminator);
                let mut directed = Directed { driver, locator };
                if self.control.drive(
                    identity,
                    security,
                    caps,
                    adapter,
                    crypto,
                    counters,
                    &mut directed,
                    scratch,
                    now,
                )? {
                    return Ok(());
                }
                continue;
            }
            let selected = (0..Q).map(|n| (self.tx_cursor[class] + n) % Q).find(|&i| {
                self.jobs[i].value.is_some_and(|j| {
                    j.traffic as usize == class
                        && j.frame_bytes > 0
                        && j.token.is_none()
                        && j.outcome.is_none()
                        && now >= j.due
                })
            });
            let Some(i) = selected else {
                continue;
            };
            self.tx_cursor[class] = (i + 1) % Q;
            let mut j = self.jobs[i].value.ok_or(Error::State)?;
            let result = (|| {
                if now >= j.deadline {
                    return Err(Error::Timeout);
                }
                if j.attempts >= self.control.config.max_attempts {
                    return Err(Error::Exhausted);
                }
                let peer = j.peer.ok_or(Error::State)?;
                if let Some(route) = j.route {
                    if self.current_route(route, now)? != peer {
                        return Err(Error::State);
                    }
                } else if j.endpoint.is_none() {
                    return Err(Error::State); // Relays never bypass route proof.
                }
                let p = self
                    .control
                    .peer_now(peer, identity, security, caps, adapter, now)?;
                if let Some(h) = j.endpoint {
                    let e = self.endpoint_now(h, identity, security, now)?;
                    if j.route.is_none() && e.facts.peer != p.peer.facts.peer {
                        return Err(Error::Access);
                    }
                    security.authorize(
                        Self::endpoint_access(&e, AccessDirection::Outbound),
                        e.facts,
                    )?;
                }
                let n = 9 + 2 * self.control.config.width.bytes();
                let service = ServiceId::new(u16::from_be_bytes(
                    self.jobs[i].bytes[n - 6..n - 4]
                        .try_into()
                        .map_err(|_| Error::State)?,
                ))?;
                security.authorize(
                    Self::hop_request(&p, service, security, AccessDirection::Outbound)?,
                    p.peer.facts,
                )?;
                if j.frame_bytes > usize::from(p.link.frame_mtu()) {
                    return Err(Error::NoSpace);
                }
                let due = now
                    .checked_add(self.control.config.retry_us)
                    .ok_or(Error::Exhausted)?;
                let token = adapter.tx_reserve(p.peer.link, 0)?;
                let mut directed = Directed {
                    driver,
                    locator: p.discriminator,
                };
                let mut gated = GatedDriver {
                    driver: &mut directed,
                    gate: self.control.gate,
                    owner: self.control.config.owner,
                };
                let result =
                    adapter.tx_submit(token, &self.jobs[i].bytes[..j.frame_bytes], &mut gated);
                j.token = Some(token);
                j.attempts += 1;
                j.due = due;
                self.jobs[i].value = Some(j);
                if result == Err(Error::InDoubt) {
                    self.finish(i, DatagramOutcome::InDoubt);
                }
                result.map(|_| ())
            })();
            if let Err(e) = result {
                if self.jobs[i].value.is_some_and(|j| j.token.is_none()) && e != Error::NoSpace {
                    self.finish(i, DatagramOutcome::Failed(e));
                }
                return Err(e);
            }
            return Ok(());
        }
        Ok(())
    }
    fn retire_data<const L: usize, const T: usize, const X: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, X, B>,
        driver: &mut impl PeerTxDriver,
        now: u64,
    ) -> Result<()> {
        let i = self.retire_cursor;
        self.retire_cursor = (i + 1) % Q;
        let Some(mut j) = self.jobs[i].value else {
            return Ok(());
        };
        let Some(token) = j.token else {
            return Ok(());
        };
        let mut view = adapter.tx_view(token)?;
        if view.state == TxState::InDoubt {
            self.finish(i, DatagramOutcome::InDoubt);
            return Err(Error::InDoubt);
        }
        if matches!(view.state, TxState::Submitted)
            && (self.stopped || now >= j.deadline || j.outcome.is_some())
        {
            let mut gated = GatedDriver {
                driver,
                gate: self.control.gate,
                owner: self.control.config.owner,
            };
            match adapter.tx_cancel(token, &mut gated) {
                Ok(v) => view = v,
                Err(e) => {
                    if e == Error::InDoubt {
                        self.finish(i, DatagramOutcome::InDoubt);
                    }
                    return Err(e);
                }
            }
        }
        if !matches!(
            view.state,
            TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
        ) {
            return Ok(());
        }
        adapter.tx_retire(token)?;
        j.token = None;
        self.jobs[i].value = Some(j);
        if j.outcome.is_some() {
            if !j.tracked {
                self.jobs[i].value = None;
                self.jobs[i].bytes.fill(0);
            }
            return Ok(());
        }
        match view.state {
            TxState::NotSubmitted
                if !self.stopped
                    && now < j.deadline
                    && j.attempts < self.control.config.max_attempts => {}
            TxState::Completed => self.finish(
                i,
                match view.terminal {
                    Some(TerminalOutcome::Success) => DatagramOutcome::LocalSuccess,
                    Some(TerminalOutcome::Failure(e)) => DatagramOutcome::Failed(e),
                    None => DatagramOutcome::InDoubt,
                },
            ),
            TxState::Cancelled => self.finish(i, DatagramOutcome::Cancelled),
            _ => self.finish(
                i,
                if self.stopped {
                    DatagramOutcome::Cancelled
                } else {
                    DatagramOutcome::Failed(if now >= j.deadline {
                        Error::Timeout
                    } else {
                        Error::Exhausted
                    })
                },
            ),
        }
        Ok(())
    }
    fn maintenance<const N: usize, const S: usize, const R: usize>(
        &mut self,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        now: u64,
    ) {
        let i = self.maintenance_cursor;
        self.maintenance_cursor = (i + 1) % (Q + A);
        if i < Q {
            if let Some(j) = self.jobs[i].value {
                if j.outcome.is_none()
                    && (self.stopped
                        || now >= j.deadline
                        || j.endpoint.is_some_and(|h| {
                            self.endpoint_now(h, identity, security, now).is_err()
                        }))
                {
                    self.finish(
                        i,
                        if self.stopped {
                            DatagramOutcome::Cancelled
                        } else {
                            DatagramOutcome::Failed(if now >= j.deadline {
                                Error::Timeout
                            } else {
                                Error::State
                            })
                        },
                    );
                }
            }
        } else {
            let i = i - Q;
            if self.app[i].value.is_some_and(|a| {
                self.stopped
                    || now >= a.deadline
                    || self
                        .endpoint_now(a.endpoint, identity, security, now)
                        .is_err()
            }) {
                self.app[i].value = None;
                self.app[i].bytes.fill(0);
                self.stats.app_discarded = self.stats.app_discarded.saturating_add(1);
            }
        }
    }
    /// Pull one authenticated message once, never write on error.
    /// 取出一次认证业务消息，失败不写输出。
    /// # Errors
    /// Empty queue, stale current Endpoint/ACL, expiry, stopped Root or short output rejects.
    pub fn receive_datagram<const N: usize, const S: usize, const R: usize>(
        &mut self,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        output: &mut [u8],
        now: u64,
    ) -> Result<DatagramReceived> {
        self.clock(now, false)?;
        let i = self
            .app
            .iter()
            .enumerate()
            .filter_map(|(i, a)| a.value.map(|v| (i, v.order)))
            .min_by_key(|v| v.1)
            .map(|v| v.0)
            .ok_or(Error::NotFound)?;
        let a = self.app[i].value.ok_or(Error::NotFound)?;
        if now >= a.deadline {
            return Err(Error::Timeout);
        }
        let e = self.endpoint_now(a.endpoint, identity, security, now)?;
        security.authorize(Self::endpoint_access(&e, AccessDirection::Inbound), e.facts)?;
        if output.len() < a.bytes {
            return Err(Error::NoSpace);
        }
        output[..a.bytes].copy_from_slice(&self.app[i].bytes[..a.bytes]);
        self.app[i].value = None;
        self.app[i].bytes.fill(0);
        self.stats.app_pulled = self.stats.app_pulled.saturating_add(1);
        Ok(DatagramReceived {
            endpoint: a.endpoint,
            receive_time_us: a.received,
            protection: e.protection,
            origin: a.origin,
            service: a.service,
            traffic_class: a.traffic,
            origin_sequence: a.sequence,
            bytes: a.bytes,
        })
    }
    /// Fair bounded steps: RX, weighted TX, both retire lanes, parent/route and data cleanup.
    /// 公平有界step：RX、加权TX、两条退休lane、父域/路由及数据清理。
    /// # Errors
    /// Invalid time/budget or reentry rejects before work. Per-input errors are reported in the result.
    pub fn step<
        const N: usize,
        const S: usize,
        const R: usize,
        const K: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const W: usize,
    >(
        &mut self,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, S, R>,
        caps: &CapabilityOwner<K>,
        adapter: &mut AdapterOwner<'_, L, T, X, B>,
        crypto: &mut impl CryptoProvider,
        counters: &mut impl RoutedCounterBank,
        driver: &mut impl PeerTxDriver,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
        budget: usize,
    ) -> Result<RoutedStep> {
        if budget == 0 || budget > 64 {
            return Err(Error::Argument);
        }
        if !core::ptr::eq(identity.callback_gate(), self.control.gate)
            || !core::ptr::eq(security.callback_gate(), self.control.gate)
            || self
                .control
                .peers
                .iter()
                .flatten()
                .any(|p| p.peer.link.adapter_instance() != adapter.adapter_instance())
        {
            return Err(Error::State);
        }
        self.clock(now, true)?;
        let mut report = RoutedStep::default();
        for _ in 0..budget {
            let c = self.cursor;
            self.cursor = (c + 1) % 6;
            let result = match c {
                0 => self.ingress(
                    identity, security, caps, adapter, crypto, counters, scratch, now,
                ),
                1 => self.transmit(
                    identity, security, caps, adapter, crypto, counters, driver, scratch, now,
                ),
                2 => self.control.retire(adapter, driver, &mut self.routes, now),
                3 => self.retire_data(adapter, driver, now),
                4 => {
                    if self.stopped {
                        self.maintenance(identity, security, now);
                        Ok(())
                    } else {
                        self.control.maintain(
                            &mut self.routes,
                            identity,
                            security,
                            caps,
                            adapter,
                            now,
                        )
                    }
                }
                _ => {
                    self.maintenance(identity, security, now);
                    if self.stopped {
                        Ok(())
                    } else {
                        self.prepare_one(
                            identity, security, caps, adapter, crypto, counters, scratch, now,
                        )
                    }
                }
            };
            report.visits += 1;
            if let Err(e) = result {
                if c == 0 {
                    self.stats.rx_rejected = self.stats.rx_rejected.saturating_add(1);
                }
                report.last_error = Some(e);
            }
        }
        Ok(report)
    }
    /// Stop admission without erasing any physical holder; bounded step drains existing work.
    /// 停止接纳但不抹除物理义务，后续有界step排空。
    /// # Errors
    /// Wrong Adapter generation or callback reentry rejects.
    pub fn stop<const L: usize, const T: usize, const X: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, X, B>,
    ) -> Result<()> {
        if self.control.gate.is_active() {
            return Err(Error::State);
        }
        self.control.stop(adapter)?;
        self.stopped = true;
        Ok(())
    }
    /// Physical quiescence; retained terminal query receipts do not own buffers.
    /// 物理静止；保留的终态查询回执不持有外部缓冲。
    #[must_use]
    pub fn quiescent<const L: usize, const T: usize, const X: usize>(
        &self,
        adapter: &AdapterOwner<'_, L, T, X, B>,
    ) -> bool {
        self.stopped
            && self.pending_rx.is_none()
            && self.control.quiescent(adapter)
            && self.jobs.iter().all(|s| {
                s.value
                    .is_none_or(|j| j.token.is_none() && j.outcome.is_some())
            })
            && self.app.iter().all(|a| a.value.is_none())
    }
    /// Read-only counters; 只读统计。
    #[must_use]
    pub const fn stats(&self) -> RoutedStats {
        self.stats
    }
    /// Current bounded counts, not proof of delivery. 当前有界数量，不是交付证明。
    #[must_use]
    pub fn counts(&self) -> (usize, usize, usize, usize) {
        (
            self.jobs.iter().filter(|s| s.value.is_some()).count(),
            self.jobs
                .iter()
                .filter(|s| s.value.is_some_and(|j| j.token.is_some()))
                .count(),
            self.app.iter().filter(|a| a.value.is_some()).count(),
            self.control.counts().1,
        )
    }
}
fn c1_identity(
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
