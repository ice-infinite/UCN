//! Direct C1 Data RPC; Coordinator owns authentication, publication and physical obligations.
//! 直达C1 Data RPC；Coordinator持有认证、发布和物理义务，业务Owner不直连。
use ucn_adapter::{AdapterOwner, LinkHandle, TerminalOutcome, TxDriver, TxState, TxToken};
use ucn_digest::{DigestWorkspace, canonical_digest};
use ucn_security::{
    AccessDirection, AccessRequest, CryptoProvider, CurrentFacts, Fingerprint, OpenDisposition,
    OpenPacketPlan, OriginContext, OriginCounterOwner, OriginSequenceOwner, PacketEvidence,
    PacketPlan, SecurityOwner, SecurityPacketWorkspace, SecurityReplayHandle, SessionHandle,
};
use ucn_service::{
    OperationEnvelope, RequestAction, RequestHandle, RequestPhase, ServiceKey, ServiceOwner,
    decode_operation_envelope, encode_operation_envelope,
};
use ucn_types::{
    DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile, InteractionRole,
    OPERATION_DURABLE_AT_MOST_ONCE, OPERATION_FINAL, OPERATION_VOLATILE_DEDUP, OriginSecurity,
    PayloadKind, Result, ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;

/// Explicit endpoint/peer RPC policy, additional to the Security Data ACL.
/// 显式Endpoint/Peer RPC策略，不能把Data ACL自动扩大成请求权限。
#[derive(Clone, Copy)]
pub struct RpcEndpoint {
    service: ServiceId,
    peer: [u8; 16],
    receive_request: bool,
    receive_result: bool,
    max_bytes: usize,
    minimum: OriginSecurity,
    mode: u8,
}
impl RpcEndpoint {
    #[cfg(feature = "source-runtime")]
    pub(crate) fn source_policy(
        self,
        bytes: usize,
    ) -> Result<(ServiceId, [u8; 16], OriginSecurity)> {
        if self.mode != OPERATION_DURABLE_AT_MOST_ONCE
            || self.receive_request
            || !self.receive_result
            || self.max_bytes > bytes
        {
            return Err(Error::Config);
        }
        Ok((self.service, self.peer, self.minimum))
    }
    #[cfg(feature = "journal-runtime")]
    pub(crate) fn journal_service(self, arguments: usize, result: usize) -> Result<ServiceId> {
        if self.mode != OPERATION_DURABLE_AT_MOST_ONCE
            || !self.receive_request
            || self.receive_result
            || self.max_bytes > arguments
            || self.max_bytes < result
        {
            return Err(Error::Config);
        }
        Ok(self.service)
    }
    /// Product-installed bounded permission, not a peer-supplied boolean.
    /// 产品安装的有界权限，不来自远端布尔声明。
    /// # Errors
    /// Empty identity/capacity or an unauthenticated minimum rejects.
    pub fn new(
        service: ServiceId,
        peer: [u8; 16],
        receive_request: bool,
        receive_result: bool,
        max_bytes: usize,
        minimum: OriginSecurity,
    ) -> Result<Self> {
        if peer == [0; 16] || max_bytes == 0 || minimum == OriginSecurity::O0 {
            return Err(Error::Config);
        }
        Ok(Self {
            service,
            peer,
            receive_request,
            receive_result,
            max_bytes,
            minimum,
            mode: OPERATION_VOLATILE_DEDUP,
        })
    }
    /// Require durable Journal execution; never downgrades to volatile invocation.
    /// 要求耐久Journal执行，绝不降级为易失调用。
    #[cfg(feature = "durable-rpc")]
    #[must_use]
    pub const fn durable(mut self) -> Self {
        self.mode = OPERATION_DURABLE_AT_MOST_ONCE;
        self
    }
}
/// Opaque RPC packet/context handle; 不透明RPC报文/上下文Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RpcHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// RPC packet phase, not application completion; RPC报文阶段，不等于业务完成。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpcPacketPhase {
    /// Server result reserved before invocation; 调用业务前预留Server结果资源。
    AwaitResponse,
    /// Semantic result frozen, awaiting protection; 语义结果已冻结，待保护。
    AwaitSeal,
    /// Protected immutable packet ready; 保护后的不变报文待发送。
    Ready,
    /// Driver holds an exact token; Driver 持有精确 Token。
    InFlight,
    /// Definite local send, retained for bounded replay; 确定本地发送，保留供有界重发。
    Cached,
}
/// Bounded packet inspection; 有界报文只读视图。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RpcPacketView {
    /// Separate semantic correlation; 独立语义关联。
    pub service: RequestHandle,
    /// Packet lifecycle; 报文生命周期。
    pub phase: RpcPacketPhase,
    /// Original half-open deadline; 原半开时限。
    pub deadline: u64,
    /// Driver calls; Driver调用次数。
    pub attempts: u16,
    /// Physical obligation still held; 尚有物理义务。
    pub physical_pending: bool,
    /// Authenticated result classification, not inferred from a local error enum.
    /// 认证结果分类，不能从本地错误enum猜测。
    pub result_kind: Option<RpcResultKind>,
}
/// Application result versus durable no-effect/uncertain observation.
/// 业务结果与耐久无副作用/不确定观察的区别。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpcResultKind {
    /// Known application success or failure; 已知业务成功或失败。
    Business = 1,
    /// Proven no handoff; 已证明未交接。
    NoEffect = 2,
    /// Unknown external outcome; 外部结果不确定。
    InDoubt = 3,
}
/// Server admission verdict; Server接纳裁决。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpcAction {
    /// Exactly one new application handoff; 一次新业务交接。
    Invoke,
    /// Authenticated arguments only; execution still requires Journal proofs.
    /// 仅认证参数，执行仍需Journal证明。
    Persist,
    /// Original invocation still pending; 原调用尚未完成。
    Pending,
    /// Frozen response, never a second invocation; 冻结回复，不重新调用。
    Replay,
}
/// Authenticated RPC receive result; 认证RPC接收结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RpcReceived {
    /// Exact context/response reservation; 精确上下文/回复预留。
    pub handle: RpcHandle,
    /// Invocation or retained duplicate; 新调用或保留重复。
    pub action: RpcAction,
    /// Application request bytes, zero for duplicates; 业务请求字节数，重复为零。
    pub bytes: usize,
}
struct Packet {
    key: ServiceKey,
    request_digest: [u8; 16],
    service: RequestHandle,
    session: SessionHandle,
    link: LinkHandle,
    server: bool,
    last_now: u64,
    deadline: u64,
    next_try: u64,
    phase: RpcPacketPhase,
    role: InteractionRole,
    attempts: u16,
    token: Option<TxToken>,
    outbound_key: u32,
    length: usize,
    result_digest: Option<(InteractionRole, [u8; 16])>,
    result_kind: Option<RpcResultKind>,
    #[cfg(feature = "durable-rpc")]
    max_result: usize,
}
#[derive(Clone, Copy)]
struct History {
    key: ServiceKey,
    high: u64,
    bits: u64,
}
impl History {
    fn plan(old: Option<Self>, key: ServiceKey) -> Result<(Self, bool)> {
        let mut next = match old {
            None => Self {
                key,
                high: 0,
                bits: 0,
            },
            Some(old) => {
                if !same_parent(old.key, key) {
                    return Err(Error::Unsupported);
                }
                if key.session < old.key.session {
                    return Err(Error::Replay);
                }
                if key.session == old.key.session {
                    old
                } else {
                    Self {
                        key,
                        high: 0,
                        bits: 0,
                    }
                }
            }
        };
        let seen = if key.operation > next.high {
            let shift = key.operation - next.high;
            next.bits = if shift >= 64 {
                1
            } else {
                (next.bits << shift) | 1
            };
            next.high = key.operation;
            false
        } else {
            let delta = next.high - key.operation;
            if delta >= 64 {
                return Err(Error::Replay);
            }
            let bit = 1 << delta;
            let seen = next.bits & bit != 0;
            next.bits |= bit;
            seen
        };
        Ok((next, seen))
    }
}
fn same_parent(a: ServiceKey, b: ServiceKey) -> bool {
    a.realm == b.realm
        && a.client == b.client
        && a.server == b.server
        && a.client_address == b.client_address
        && a.client_generation == b.client_generation
        && a.server_address == b.server_address
        && a.server_generation == b.server_generation
}
fn status(role: InteractionRole, code: u16) -> i32 {
    if role == InteractionRole::Error {
        -1 - i32::from(code)
    } else {
        i32::from(code)
    }
}
fn classify_result(mode: u8, role: InteractionRole, code: u16) -> RpcResultKind {
    if mode == OPERATION_DURABLE_AT_MOST_ONCE && role == InteractionRole::Error {
        match code {
            ucn_types::DURABLE_RPC_NO_EFFECT => RpcResultKind::NoEffect,
            ucn_types::DURABLE_RPC_IN_DOUBT => RpcResultKind::InDoubt,
            _ => RpcResultKind::Business,
        }
    } else {
        RpcResultKind::Business
    }
}
fn access(
    key: ServiceKey,
    session: SessionHandle,
    direction: AccessDirection,
) -> Result<AccessRequest> {
    Ok(AccessRequest {
        session,
        context_fingerprint: Fingerprint::new(key.security)?,
        service: ServiceId::new(key.service)?,
        protocol_opcode: 0,
        direction,
    })
}
fn evidence_key(e: PacketEvidence, service: ServiceId, operation: u64) -> ServiceKey {
    let (realm, client, server) = e.identities();
    let (session, key_generation, policy, origin, security) = e.security();
    ServiceKey {
        realm,
        client: client.principal,
        server: server.principal,
        client_address: client.address.get(),
        client_generation: client.generation.get(),
        server_address: server.address.get(),
        server_generation: server.generation.get(),
        operation,
        service: service.get(),
        opcode: 0,
        session,
        key_generation,
        origin,
        policy,
        security,
    }
}

/// One configured peer/endpoint, fixed packet slots and one non-evicting operation history.
/// 一个已配置Peer/Endpoint，固定报文槽及不驱逐的Operation历史窗口。
pub struct RpcBridge<const SLOTS: usize, const FRAME: usize> {
    owner: u32,
    endpoint: RpcEndpoint,
    retry: u64,
    max_attempts: u16,
    packets: [Option<Packet>; SLOTS],
    generations: [u32; SLOTS],
    bytes: [[u8; FRAME]; SLOTS],
    staging: [u8; FRAME],
    history: Option<History>,
    stopped: bool,
}
impl<const N: usize, const F: usize> RpcBridge<N, F> {
    /// Static caller storage; zero heap, no implicit Persistence requirement.
    /// 调用方静态存储，零堆，不隐含要求Persistence。
    /// # Errors
    /// Invalid capacity/policy/retry limit rejects.
    pub const fn new(
        owner: u32,
        endpoint: RpcEndpoint,
        retry_us: u64,
        max_attempts: u16,
    ) -> Result<Self> {
        if owner == 0
            || N == 0
            || N > u16::MAX as usize
            || F < 44
            || retry_us == 0
            || retry_us > i64::MAX as u64
            || max_attempts == 0
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            endpoint,
            retry: retry_us,
            max_attempts,
            packets: [const { None }; N],
            generations: [0; N],
            bytes: [[0; F]; N],
            staging: [0; F],
            history: None,
            stopped: false,
        })
    }
    fn index(&self, h: RpcHandle) -> Result<usize> {
        if h.owner != self.owner
            || h.slot >= N
            || self.generations[h.slot] != h.generation
            || self.packets[h.slot].is_none()
        {
            return Err(Error::State);
        }
        Ok(h.slot)
    }
    fn free(&self) -> Result<(usize, u32)> {
        let slot = self
            .packets
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        Ok((
            slot,
            self.generations[slot]
                .checked_add(1)
                .ok_or(Error::Exhausted)?,
        ))
    }
    fn handle(&self, slot: usize) -> RpcHandle {
        RpcHandle {
            owner: self.owner,
            slot,
            generation: self.generations[slot],
        }
    }
    fn peer(&self, facts: CurrentFacts, origin: OriginSecurity) -> Result<()> {
        if facts.peer.principal != self.endpoint.peer
            || (origin as u8) < self.endpoint.minimum as u8
        {
            return Err(Error::Access);
        }
        Ok(())
    }
    fn current<const S: usize, const R: usize>(
        &self,
        key: ServiceKey,
        server: bool,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        link: LinkHandle,
    ) -> Result<()> {
        key.validate()?;
        self.peer(facts, key.origin)?;
        let view = security.session_get(session, facts)?;
        let (local, peer, local_address, peer_address, local_generation, peer_generation) =
            if server {
                (
                    key.server,
                    key.client,
                    key.server_address,
                    key.client_address,
                    key.server_generation,
                    key.client_generation,
                )
            } else {
                (
                    key.client,
                    key.server,
                    key.client_address,
                    key.server_address,
                    key.client_generation,
                    key.server_generation,
                )
            };
        let generation = if server {
            security.inbound_key_generation(session, facts)?
        } else {
            security.outbound_key_generation(session, facts)?
        };
        if view.hop_profile != HopProfile::H0
            || view.origin_security != key.origin
            || view.session_generation.get() != key.session
            || key.realm != security.realm().get()
            || generation != key.key_generation
            || facts.policy_generation != key.policy
            || facts.local.principal != local
            || facts.peer.principal != peer
            || facts.local.address.get() != local_address
            || facts.peer.address.get() != peer_address
            || facts.local.generation.get() != local_generation
            || facts.peer.generation.get() != peer_generation
            || link.instance_generation() != facts.link_generation.get()
            || key.service != self.endpoint.service.get()
        {
            return Err(Error::Access);
        }
        Ok(())
    }
    fn receive_header(&self, plan: OpenPacketPlan<'_>, role: InteractionRole) -> Result<()> {
        let h = CommonHeader::decode(plan.packet)?;
        if self.stopped
            || h.contract != HeaderContract::C1
            || h.payload_kind != PayloadKind::Data
            || h.interaction != role
            || h.delivery != DeliveryGuarantee::BestEffort
            || h.origin_security == OriginSecurity::O0
            || h.hop_limit.get() != 1
            || h.traffic_class != TrafficClass::Q2
            || plan.hop_profile != HopProfile::H0
        {
            return Err(Error::Unsupported);
        }
        Ok(())
    }
    /// Inspect packet lifecycle independently of Service completion.
    /// 查看独立于Service完成的报文生命周期。
    /// # Errors
    /// Retired/stale/cross-owner handles reject.
    pub fn view(&self, h: RpcHandle) -> Result<RpcPacketView> {
        let p = self.packets[self.index(h)?].as_ref().ok_or(Error::State)?;
        Ok(RpcPacketView {
            service: p.service,
            phase: p.phase,
            deadline: p.deadline,
            attempts: p.attempts,
            physical_pending: p.token.is_some(),
            result_kind: p.result_kind,
        })
    }
    #[cfg(any(feature = "journal-runtime", feature = "source-runtime"))]
    pub(crate) fn scheduled_ready(&self, h: RpcHandle, now: u64) -> Result<bool> {
        let p = self.packets[self.index(h)?].as_ref().ok_or(Error::State)?;
        Ok(p.phase == RpcPacketPhase::Ready
            && p.token.is_none()
            && now >= p.next_try
            && p.attempts < self.max_attempts)
    }
    /// Copy immutable protected bytes, never a plaintext/execution proof.
    /// 复制不变保护字节，不是明文/执行证明。
    /// # Errors
    /// Unsealed or short output remains untouched.
    pub fn copy_packet(&self, h: RpcHandle, output: &mut [u8]) -> Result<usize> {
        let slot = self.index(h)?;
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        if p.length == 0 {
            return Err(Error::State);
        }
        if output.len() < p.length {
            return Err(Error::NoSpace);
        }
        output[..p.length].copy_from_slice(&self.bytes[slot][..p.length]);
        Ok(p.length)
    }
    #[allow(clippy::too_many_arguments)]
    fn seal<const S: usize, const R: usize, const W: usize>(
        &mut self,
        slot: usize,
        key: ServiceKey,
        role: InteractionRole,
        code: u16,
        business_bytes: usize,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl OriginSequenceOwner,
        session: SessionHandle,
        facts: CurrentFacts,
        workspace: &mut SecurityPacketWorkspace<W>,
    ) -> Result<usize> {
        let sequence = counter.preview()?;
        let offset = if role == InteractionRole::Request {
            9
        } else {
            11
        };
        encode_operation_envelope(
            role,
            OperationEnvelope {
                operation: key.operation,
                flags: self.endpoint.mode
                    | if role == InteractionRole::Request {
                        0
                    } else {
                        OPERATION_FINAL
                    },
                result: code,
            },
            &mut self.staging[..offset],
        )?;
        let width = security.address_width().bytes();
        let header_bytes = 9 + 2 * width;
        let mut prefix = [0; 17];
        CommonHeader {
            contract: HeaderContract::C1,
            traffic_class: TrafficClass::Q2,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: role,
            payload_kind: PayloadKind::Data,
            origin_security: key.origin,
            hop_limit: HopLimit::new(1)?,
        }
        .encode(&mut prefix);
        prefix[3..3 + width].copy_from_slice(&facts.local.address.get().to_be_bytes()[4 - width..]);
        prefix[3 + width..3 + 2 * width]
            .copy_from_slice(&facts.peer.address.get().to_be_bytes()[4 - width..]);
        prefix[3 + 2 * width..5 + 2 * width].copy_from_slice(&key.service.to_be_bytes());
        prefix[5 + 2 * width..header_bytes].copy_from_slice(&sequence.get().to_be_bytes());
        let mut identity = [0; 46];
        identity[..16].copy_from_slice(&facts.local.principal);
        identity[16..20].copy_from_slice(&facts.local.generation.get().to_be_bytes());
        identity[20..36].copy_from_slice(&facts.peer.principal);
        identity[36..40].copy_from_slice(&facts.peer.generation.get().to_be_bytes());
        identity[40..46].copy_from_slice(&prefix[3 + 2 * width..header_bytes]);
        security.protect_packet(
            crypto,
            OriginCounterOwner::Sequence(counter),
            session,
            facts,
            access(key, session, AccessDirection::Outbound)?,
            PacketPlan {
                prefix: &prefix[..header_bytes],
                payload: &self.staging[..offset + business_bytes],
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: Fingerprint::new(key.security)?,
                    sequence,
                },
                hop_profile: HopProfile::H0,
            },
            None,
            workspace,
            &mut self.bytes[slot],
        )
    }
    /// Allocate Operation from the first unique Origin Sequence and protect once before any Driver call.
    /// 从首个唯一Origin序号分配Operation，任何Driver调用前只保护一次。
    /// # Errors
    /// Policy/identity/capacity/crypto failure leaves no accepted client correlation or packet.
    #[allow(clippy::too_many_arguments)]
    pub fn begin_request<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const W: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        service: &mut ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl OriginSequenceOwner,
        session: SessionHandle,
        facts: CurrentFacts,
        fingerprint: Fingerprint,
        link: LinkHandle,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        business: &[u8],
        deadline: u64,
        workspace: &mut SecurityPacketWorkspace<W>,
    ) -> Result<RpcHandle> {
        if self.endpoint.mode != OPERATION_VOLATILE_DEDUP {
            return Err(Error::Unsupported);
        }
        let operation = u64::from(counter.preview()?.get());
        self.begin_request_inner(
            service,
            security,
            crypto,
            counter,
            session,
            facts,
            fingerprint,
            link,
            adapter,
            business,
            deadline,
            workspace,
            operation,
        )
    }
    /// Allocate a confirmed durable ID and protect the request, without executing it.
    /// 从已确认区间分配耐久ID并保护请求，不执行业务。
    /// # Errors
    /// Failed preflight drops ID reservation and leaves no accepted correlation.
    #[cfg(feature = "durable-rpc")]
    #[allow(clippy::too_many_arguments)]
    pub fn begin_durable_request<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const W: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        service: &mut ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl OriginSequenceOwner,
        ids: &mut ucn_service::OperationIds,
        session: SessionHandle,
        facts: CurrentFacts,
        fingerprint: Fingerprint,
        link: LinkHandle,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        business: &[u8],
        deadline: u64,
        workspace: &mut SecurityPacketWorkspace<W>,
    ) -> Result<RpcHandle> {
        if self.endpoint.mode != OPERATION_DURABLE_AT_MOST_ONCE {
            return Err(Error::Unsupported);
        }
        let reservation = ids.reserve_id()?;
        let h = self.begin_request_inner(
            service,
            security,
            crypto,
            counter,
            session,
            facts,
            fingerprint,
            link,
            adapter,
            business,
            deadline,
            workspace,
            reservation.id(),
        )?;
        let _ = reservation.commit();
        Ok(h)
    }
    #[allow(clippy::too_many_arguments)]
    fn begin_request_inner<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const W: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        service: &mut ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl OriginSequenceOwner,
        session: SessionHandle,
        facts: CurrentFacts,
        fingerprint: Fingerprint,
        link: LinkHandle,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        business: &[u8],
        deadline: u64,
        workspace: &mut SecurityPacketWorkspace<W>,
        operation: u64,
    ) -> Result<RpcHandle> {
        if self.stopped || !self.endpoint.receive_result {
            return Err(Error::State);
        }
        let view = security.session_get(session, facts)?;
        let key = ServiceKey {
            realm: security.realm().get(),
            client: facts.local.principal,
            server: facts.peer.principal,
            client_address: facts.local.address.get(),
            client_generation: facts.local.generation.get(),
            server_address: facts.peer.address.get(),
            server_generation: facts.peer.generation.get(),
            operation,
            service: self.endpoint.service.get(),
            opcode: 0,
            session: view.session_generation.get(),
            key_generation: security.outbound_key_generation(session, facts)?,
            origin: view.origin_security,
            policy: facts.policy_generation,
            security: fingerprint.bytes(),
        };
        self.current(key, false, security, session, facts, link)?;
        if business.len() > O
            || business.len() > self.endpoint.max_bytes
            || deadline <= facts.now_us
        {
            return Err(Error::Argument);
        }
        if 9 + 2 * security.address_width().bytes() + 9 + business.len() + 16
            > F.min(adapter.link_mtu(link)?)
        {
            return Err(Error::NoSpace);
        }
        let (slot, generation) = self.free()?;
        encode_operation_envelope(
            InteractionRole::Request,
            OperationEnvelope {
                operation: key.operation,
                flags: self.endpoint.mode,
                result: 0,
            },
            &mut self.staging[..9],
        )?;
        self.staging[9..9 + business.len()].copy_from_slice(business);
        let digest = canonical_digest(
            b"UCN6-PAYLOAD-V1",
            &self.staging[..9 + business.len()],
            &mut DigestWorkspace::new(),
        )?;
        let handle = service.request(key, digest, facts.now_us, deadline)?;
        let sealed = self.seal(
            slot,
            key,
            InteractionRole::Request,
            0,
            business.len(),
            security,
            crypto,
            counter,
            session,
            facts,
            workspace,
        );
        let length = match sealed {
            Ok(n) => n,
            Err(e) => {
                service.cancel(handle)?;
                service.retire(handle)?;
                return Err(e);
            }
        };
        self.packets[slot] = Some(Packet {
            key,
            request_digest: digest,
            service: handle,
            session,
            link,
            server: false,
            last_now: facts.now_us,
            deadline,
            next_try: facts.now_us,
            phase: RpcPacketPhase::Ready,
            role: InteractionRole::Request,
            attempts: 0,
            token: None,
            outbound_key: key.key_generation,
            length,
            result_digest: None,
            result_kind: None,
            #[cfg(feature = "durable-rpc")]
            max_result: self.endpoint.max_bytes.min(O),
        });
        self.generations[slot] = generation;
        Ok(self.handle(slot))
    }
    /// Authenticate, preflight reply/Service/history, commit Replay, then hand off business bytes once.
    /// 认证后预检回复/Service/历史，提交Replay，最后一次交付业务字节。
    /// # Errors
    /// Any failed admission preserves output, Service and operation history.
    #[allow(clippy::too_many_arguments)]
    pub fn receive_request<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const W: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        service: &mut ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        plan: OpenPacketPlan<'_>,
        link: LinkHandle,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        output: &mut [u8],
        workspace: &mut SecurityPacketWorkspace<W>,
    ) -> Result<RpcReceived> {
        self.receive_request_inner(
            service, security, crypto, session, facts, plan, link, adapter, output, workspace, None,
        )
    }
    /// Authenticate a retry against an actual confirmed terminal Journal, even after cache retirement.
    /// 即使缓存已退休，也可用实际确认终态Journal核对认证重试。
    /// # Errors
    /// Nonterminal Journal never supplies a replay invocation or fabricated plaintext.
    #[cfg(feature = "durable-rpc")]
    #[allow(clippy::too_many_arguments)]
    pub fn receive_journaled_request<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const W: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
        const J: usize,
    >(
        &mut self,
        service: &mut ServiceOwner<A, B, O>,
        operation: &ucn_service::OperationOwner<J>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        plan: OpenPacketPlan<'_>,
        link: LinkHandle,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        output: &mut [u8],
        workspace: &mut SecurityPacketWorkspace<W>,
    ) -> Result<RpcReceived> {
        if self.endpoint.mode != OPERATION_DURABLE_AT_MOST_ONCE {
            return Err(Error::Unsupported);
        }
        let terminal = if operation.copy_result(&mut self.staging).is_ok() {
            operation.key()
        } else {
            None
        };
        self.receive_request_inner(
            service,
            security,
            crypto,
            session,
            facts,
            plan,
            link,
            adapter,
            output,
            workspace,
            terminal.map(|k| (k.service, k.request_digest)),
        )
    }
    #[allow(clippy::too_many_arguments)]
    fn receive_request_inner<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const W: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        service: &mut ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        plan: OpenPacketPlan<'_>,
        link: LinkHandle,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        output: &mut [u8],
        workspace: &mut SecurityPacketWorkspace<W>,
        terminal: Option<(ServiceKey, [u8; 16])>,
    ) -> Result<RpcReceived> {
        self.receive_header(plan, InteractionRole::Request)?;
        if !self.endpoint.receive_request {
            return Err(Error::Access);
        }
        let request = AccessRequest {
            session,
            context_fingerprint: context(plan)?,
            service: self.endpoint.service,
            protocol_opcode: 0,
            direction: AccessDirection::Inbound,
        };
        let opened = security.open_packet(
            crypto,
            session,
            facts,
            request,
            plan,
            workspace,
            &mut self.staging,
        )?;
        let evidence = opened.evidence();
        if opened.disposition == OpenDisposition::AuthenticatedReplayCandidate {
            security.replay_abort(opened.replay)?;
            if !self
                .packets
                .iter()
                .flatten()
                .any(|p| p.server && p.request_digest == evidence.digests().1)
            {
                if let Some((key, digest)) = terminal {
                    return self.restore_terminal_cache(
                        service, security, session, facts, link, adapter, evidence, key, digest,
                    );
                }
            }
            return self.replay_request(service, security, session, facts, evidence, link);
        }
        let mut replay = Some(opened.replay);
        let admitted = self.admit_request(
            service,
            security,
            session,
            facts,
            link,
            adapter,
            evidence,
            opened.payload_bytes,
            &mut replay,
            output,
        );
        if let Some(replay) = replay {
            security.replay_abort(replay)?;
        }
        admitted
    }
    #[allow(clippy::too_many_arguments)]
    fn restore_terminal_cache<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        service: &mut ServiceOwner<A, B, O>,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        link: LinkHandle,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        evidence: PacketEvidence,
        key: ServiceKey,
        digest: [u8; 16],
    ) -> Result<RpcReceived> {
        if self.endpoint.mode != OPERATION_DURABLE_AT_MOST_ONCE
            || evidence_key(evidence, self.endpoint.service, key.operation) != key
            || digest != evidence.digests().1
        {
            return Err(Error::Replay);
        }
        self.current(key, true, security, session, facts, link)?;
        if 9 + 2 * security.address_width().bytes() + 11 + self.endpoint.max_bytes.min(O) + 16
            > F.min(adapter.link_mtu(link)?)
        {
            return Err(Error::NoSpace);
        }
        let (slot, generation) = self.free()?;
        let pending = service.prepare_request(key, digest, facts.now_us)?;
        let deadline = pending.deadline();
        let RequestAction::Invoke(handle) = pending.action() else {
            return Err(Error::State);
        };
        let _ = pending.commit();
        self.packets[slot] = Some(Packet {
            key,
            request_digest: digest,
            service: handle,
            session,
            link,
            server: true,
            last_now: facts.now_us,
            deadline,
            next_try: facts.now_us,
            phase: RpcPacketPhase::AwaitResponse,
            role: InteractionRole::Result,
            attempts: 0,
            token: None,
            outbound_key: 0,
            length: 0,
            result_digest: None,
            result_kind: None,
            #[cfg(feature = "durable-rpc")]
            max_result: self.endpoint.max_bytes.min(O),
        });
        self.generations[slot] = generation;
        Ok(RpcReceived {
            handle: self.handle(slot),
            action: RpcAction::Persist,
            bytes: 0,
        })
    }
    #[allow(clippy::too_many_arguments)]
    fn admit_request<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        service: &mut ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        link: LinkHandle,
        adapter: &AdapterOwner<'_, L, T, RX, F>,
        evidence: PacketEvidence,
        payload_bytes: usize,
        replay: &mut Option<SecurityReplayHandle>,
        output: &mut [u8],
    ) -> Result<RpcReceived> {
        let (envelope, offset) =
            decode_operation_envelope(InteractionRole::Request, &self.staging[..payload_bytes])?;
        if envelope.flags != self.endpoint.mode
            || (self.endpoint.mode == OPERATION_VOLATILE_DEDUP
                && envelope.operation > evidence.sequence())
        {
            return Err(Error::Unsupported);
        }
        let business_bytes = payload_bytes - offset;
        if business_bytes > O || business_bytes > self.endpoint.max_bytes {
            return Err(Error::NoSpace);
        }
        let key = evidence_key(evidence, self.endpoint.service, envelope.operation);
        self.current(key, true, security, session, facts, link)?;
        let max_response =
            9 + 2 * security.address_width().bytes() + 11 + self.endpoint.max_bytes.min(O) + 16;
        if max_response > F.min(adapter.link_mtu(link)?) {
            return Err(Error::NoSpace);
        }
        let (history, seen) = if self.endpoint.mode == OPERATION_VOLATILE_DEDUP {
            let (history, seen) = History::plan(self.history, key)?;
            (Some(history), seen)
        } else {
            (self.history, false)
        };
        let digest = evidence.digests().1;
        let pending = service.prepare_request(key, digest, facts.now_us)?;
        let action = pending.action();
        let deadline = pending.deadline();
        let (handle, fresh) = match action {
            RequestAction::Invoke(h) => (h, true),
            RequestAction::Pending(h) | RequestAction::Replay(h) => (h, false),
        };
        let (slot, generation) = if fresh {
            if seen {
                return Err(Error::Replay);
            }
            if output.len() < business_bytes {
                return Err(Error::NoSpace);
            }
            self.free()?
        } else {
            let slot = self
                .packets
                .iter()
                .position(|p| p.as_ref().is_some_and(|p| p.server && p.service == handle))
                .ok_or(Error::State)?;
            self.binding(slot, key, session, link, facts.now_us)?;
            (slot, self.generations[slot])
        };
        security.replay_commit(replay.take().ok_or(Error::State)?, facts)?;
        let action = pending.commit();
        self.history = history;
        if fresh {
            self.packets[slot] = Some(Packet {
                key,
                request_digest: digest,
                service: handle,
                session,
                link,
                server: true,
                last_now: facts.now_us,
                deadline,
                next_try: facts.now_us,
                phase: RpcPacketPhase::AwaitResponse,
                role: InteractionRole::Result,
                attempts: 0,
                token: None,
                outbound_key: 0,
                length: 0,
                result_digest: None,
                result_kind: None,
                #[cfg(feature = "durable-rpc")]
                max_result: self.endpoint.max_bytes.min(O),
            });
            self.generations[slot] = generation;
            output[..business_bytes].copy_from_slice(&self.staging[offset..payload_bytes]);
        }
        Ok(RpcReceived {
            handle: self.handle(slot),
            action: match action {
                RequestAction::Invoke(_)
                    if self.endpoint.mode == OPERATION_DURABLE_AT_MOST_ONCE =>
                {
                    RpcAction::Persist
                }
                RequestAction::Invoke(_) => RpcAction::Invoke,
                RequestAction::Pending(_) => RpcAction::Pending,
                RequestAction::Replay(_) => RpcAction::Replay,
            },
            bytes: if fresh { business_bytes } else { 0 },
        })
    }
    fn binding(
        &self,
        slot: usize,
        key: ServiceKey,
        session: SessionHandle,
        link: LinkHandle,
        now: u64,
    ) -> Result<()> {
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        if p.key != key
            || p.session != session
            || p.link != link
            || now < p.last_now
            || now >= p.deadline
        {
            return Err(Error::State);
        }
        Ok(())
    }
    fn replay_request<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
    >(
        &self,
        service: &ServiceOwner<A, B, O>,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        evidence: PacketEvidence,
        link: LinkHandle,
    ) -> Result<RpcReceived> {
        let (slot, p) = self
            .packets
            .iter()
            .enumerate()
            .find_map(|(i, p)| {
                p.as_ref()
                    .filter(|p| p.server && p.request_digest == evidence.digests().1)
                    .map(|p| (i, p))
            })
            .ok_or(Error::Replay)?;
        self.binding(
            slot,
            evidence_key(evidence, self.endpoint.service, p.key.operation),
            session,
            link,
            facts.now_us,
        )?;
        self.current(p.key, true, security, session, facts, link)?;
        let action = if service.view(p.service)?.phase == RequestPhase::Complete {
            RpcAction::Replay
        } else {
            RpcAction::Pending
        };
        Ok(RpcReceived {
            handle: self.handle(slot),
            action,
            bytes: 0,
        })
    }
    /// Freeze a single bounded result; a crypto failure can later retry seal without invoking the app.
    /// 冻结一次有界结果；密码失败后可重试seal，不再次调用业务。
    /// # Errors
    /// Wrong phase/policy/context, oversize or expired response leaves Service unchanged.
    #[allow(clippy::too_many_arguments)]
    pub fn respond<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
    >(
        &mut self,
        h: RpcHandle,
        service: &mut ServiceOwner<A, B, O>,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        role: InteractionRole,
        code: u16,
        business: &[u8],
    ) -> Result<()> {
        if self.endpoint.mode != OPERATION_VOLATILE_DEDUP {
            return Err(Error::Unsupported);
        }
        self.respond_inner(h, service, security, session, facts, role, code, business)
    }
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn respond_inner<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
    >(
        &mut self,
        h: RpcHandle,
        service: &mut ServiceOwner<A, B, O>,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        role: InteractionRole,
        code: u16,
        business: &[u8],
    ) -> Result<()> {
        if self.stopped || !matches!(role, InteractionRole::Result | InteractionRole::Error) {
            return Err(Error::State);
        }
        let slot = self.index(h)?;
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        if !p.server
            || p.phase != RpcPacketPhase::AwaitResponse
            || business.len() > self.endpoint.max_bytes
        {
            return Err(Error::State);
        }
        self.binding(slot, p.key, session, p.link, facts.now_us)?;
        self.current(p.key, true, security, session, facts, p.link)?;
        security.authorize(access(p.key, session, AccessDirection::Inbound)?, facts)?;
        if service.view(p.service)?.key != p.key {
            return Err(Error::State);
        }
        service.respond(p.service, status(role, code), business, facts.now_us)?;
        let p = self.packets[slot].as_mut().ok_or(Error::State)?;
        p.role = role;
        p.phase = RpcPacketPhase::AwaitSeal;
        p.last_now = facts.now_us;
        Ok(())
    }
    /// Seal the frozen Service result once; immutable retries keep the same Origin Sequence.
    /// 仅保护一次冻结Service结果，不变重试保持同一Origin序号。
    /// # Errors
    /// Wrong context/phase or failed Provider retains semantic result and response obligation.
    #[allow(clippy::too_many_arguments)]
    pub fn seal_response<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const W: usize,
    >(
        &mut self,
        h: RpcHandle,
        service: &ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl OriginSequenceOwner,
        session: SessionHandle,
        facts: CurrentFacts,
        workspace: &mut SecurityPacketWorkspace<W>,
    ) -> Result<()> {
        if self.stopped {
            return Err(Error::State);
        }
        let slot = self.index(h)?;
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        if !p.server || p.phase != RpcPacketPhase::AwaitSeal {
            return Err(Error::State);
        }
        self.binding(slot, p.key, session, p.link, facts.now_us)?;
        self.current(p.key, true, security, session, facts, p.link)?;
        let view = service.view(p.service)?;
        if view.key != p.key || view.phase != RequestPhase::Complete {
            return Err(Error::State);
        }
        let code = if p.role == InteractionRole::Error {
            u16::try_from(-1 - view.result)
        } else {
            u16::try_from(view.result)
        }
        .map_err(|_| Error::State)?;
        let key = p.key;
        let role = p.role;
        let n = service.copy_result(p.service, &mut self.staging[11..])?;
        let length = self.seal(
            slot, key, role, code, n, security, crypto, counter, session, facts, workspace,
        )?;
        let outbound_key = security.outbound_key_generation(session, facts)?;
        let p = self.packets[slot].as_mut().ok_or(Error::State)?;
        p.length = length;
        p.outbound_key = outbound_key;
        p.phase = RpcPacketPhase::Ready;
        p.last_now = facts.now_us;
        Ok(())
    }
    /// Submit only current authorized immutable packet; local completion is never RPC completion.
    /// 仅提交当前授权的不变报文，本地发送完成不等于RPC完成。
    /// # Errors
    /// Deadline/attempt/backpressure/context error preserves logical/physical obligations.
    #[allow(clippy::too_many_arguments)]
    pub fn submit<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const RX: usize,
    >(
        &mut self,
        h: RpcHandle,
        service: &mut ServiceOwner<A, B, O>,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<()> {
        if self.stopped {
            return Err(Error::State);
        }
        let slot = self.index(h)?;
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        if p.phase != RpcPacketPhase::Ready || p.token.is_some() || facts.now_us < p.next_try {
            return Err(Error::State);
        }
        self.binding(slot, p.key, session, p.link, facts.now_us)?;
        self.current(p.key, p.server, security, session, facts, p.link)?;
        security.authorize(access(p.key, session, AccessDirection::Outbound)?, facts)?;
        if security.outbound_key_generation(session, facts)? != p.outbound_key {
            return Err(Error::State);
        }
        let view = service.view(p.service)?;
        if view.key != p.key
            || (!p.server
                && !matches!(
                    view.phase,
                    RequestPhase::WaitSend | RequestPhase::WaitResult
                ))
        {
            return Err(Error::State);
        }
        if p.length > adapter.link_mtu(p.link)? {
            return Err(Error::NoSpace);
        }
        if p.attempts >= self.max_attempts {
            return Err(Error::Exhausted);
        }
        let next_try = facts
            .now_us
            .checked_add(self.retry)
            .ok_or(Error::Exhausted)?;
        let token = adapter.tx_reserve(p.link, u16::try_from(slot).map_err(|_| Error::Config)?)?;
        let p = self.packets[slot].as_mut().ok_or(Error::State)?;
        p.token = Some(token);
        p.phase = RpcPacketPhase::InFlight;
        p.attempts += 1;
        p.next_try = next_try;
        p.last_now = facts.now_us;
        let transmitted = adapter.tx_submit(token, &self.bytes[slot][..p.length], driver)?;
        if transmitted.state == TxState::NotSubmitted {
            self.retire_attempt(h, adapter)?;
        } else if !p.server && view.phase == RequestPhase::WaitSend {
            service.sent(p.service, facts.now_us)?;
        }
        Ok(())
    }
    /// Retire exact token before releasing its holder; 精确 Token 退休后才释放 Holder。
    /// # Errors
    /// Pending/unknown or Driver retirement failure never frees protected bytes.
    pub fn retire_attempt<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: RpcHandle,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        let token = p.token.ok_or(Error::State)?;
        let view = adapter.tx_view(token)?;
        if !matches!(
            view.state,
            TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
        ) {
            return Err(Error::State);
        }
        adapter.tx_retire(token)?;
        let p = self.packets[slot].as_mut().ok_or(Error::State)?;
        p.token = None;
        p.phase = if view.terminal == Some(TerminalOutcome::Success) {
            RpcPacketPhase::Cached
        } else {
            RpcPacketPhase::Ready
        };
        Ok(())
    }
    /// Explicit bounded replay, never a new request/response or extended deadline.
    /// 显式有界重发，不创建新请求/回复，不续期。
    /// # Errors
    /// Active/unknown/expired/exhausted packet cannot be requeued.
    pub fn requeue(&mut self, h: RpcHandle, now: u64) -> Result<()> {
        if self.stopped {
            return Err(Error::State);
        }
        let slot = self.index(h)?;
        let p = self.packets[slot].as_mut().ok_or(Error::State)?;
        if p.phase != RpcPacketPhase::Cached
            || p.token.is_some()
            || now < p.last_now
            || now >= p.deadline
        {
            return Err(Error::State);
        }
        if p.attempts >= self.max_attempts {
            return Err(Error::Exhausted);
        }
        p.phase = RpcPacketPhase::Ready;
        p.last_now = now;
        Ok(())
    }
    #[cfg(feature = "source-runtime")]
    pub(crate) fn client_retry_due(&self, h: RpcHandle, now: u64) -> Result<bool> {
        let p = self.packets[self.index(h)?].as_ref().ok_or(Error::State)?;
        Ok(!self.stopped
            && !p.server
            && p.phase == RpcPacketPhase::Cached
            && p.token.is_none()
            && now >= p.next_try
            && now < p.deadline
            && p.attempts < self.max_attempts)
    }
    /// Verify actual final server result and commit Replay before immutable Service completion.
    /// 核对实际最终Server结果，Replay提交后才发布不变Service完成。
    /// # Errors
    /// Wrong role/mode/full key, replay conflict or deadline does not change Service.
    #[allow(clippy::too_many_arguments)]
    pub fn receive_result<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
        const W: usize,
    >(
        &mut self,
        h: RpcHandle,
        service: &mut ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        plan: OpenPacketPlan<'_>,
        link: LinkHandle,
        workspace: &mut SecurityPacketWorkspace<W>,
    ) -> Result<bool> {
        let role = CommonHeader::decode(plan.packet)?.interaction;
        if !matches!(role, InteractionRole::Result | InteractionRole::Error)
            || !self.endpoint.receive_result
        {
            return Err(Error::Unsupported);
        }
        self.receive_header(plan, role)?;
        let slot = self.index(h)?;
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        if p.server {
            return Err(Error::State);
        }
        self.binding(slot, p.key, session, link, facts.now_us)?;
        self.current(p.key, false, security, session, facts, link)?;
        let opened = security.open_packet(
            crypto,
            session,
            facts,
            access(p.key, session, AccessDirection::Inbound)?,
            plan,
            workspace,
            &mut self.staging,
        )?;
        let evidence = opened.evidence();
        let mut replay = Some(opened.replay);
        let outcome = self.admit_result(
            slot,
            service,
            security,
            facts,
            evidence,
            opened.payload_bytes,
            opened.disposition,
            &mut replay,
            role,
        );
        if let Some(replay) = replay {
            security.replay_abort(replay)?;
        }
        outcome
    }
    #[allow(clippy::too_many_arguments)]
    fn admit_result<
        const A: usize,
        const B: usize,
        const O: usize,
        const S: usize,
        const R: usize,
    >(
        &mut self,
        slot: usize,
        service: &mut ServiceOwner<A, B, O>,
        security: &mut SecurityOwner<'_, S, R>,
        facts: CurrentFacts,
        evidence: PacketEvidence,
        payload_bytes: usize,
        disposition: OpenDisposition,
        replay: &mut Option<SecurityReplayHandle>,
        role: InteractionRole,
    ) -> Result<bool> {
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        let (realm, server, client) = evidence.identities();
        let (session, _, policy, origin, fingerprint) = evidence.security();
        if realm != p.key.realm
            || server.principal != p.key.server
            || client.principal != p.key.client
            || server.address.get() != p.key.server_address
            || server.generation.get() != p.key.server_generation
            || client.address.get() != p.key.client_address
            || client.generation.get() != p.key.client_generation
            || session != p.key.session
            || policy != p.key.policy
            || origin != p.key.origin
            || fingerprint != p.key.security
        {
            return Err(Error::Access);
        }
        let digest = (role, evidence.digests().1);
        if disposition == OpenDisposition::AuthenticatedReplayCandidate {
            security.replay_abort(replay.take().ok_or(Error::State)?)?;
            if p.result_digest != Some(digest)
                || service.view(p.service)?.phase != RequestPhase::Complete
            {
                return Err(Error::Replay);
            }
            return Ok(false);
        }
        let (envelope, offset) = decode_operation_envelope(role, &self.staging[..payload_bytes])?;
        if envelope.operation != p.key.operation
            || envelope.flags != self.endpoint.mode | OPERATION_FINAL
        {
            return Err(Error::Access);
        }
        if p.result_digest.is_some_and(|old| old != digest) {
            return Err(Error::Replay);
        }
        let pending = service.prepare_result(
            p.service,
            p.key,
            status(role, envelope.result),
            &self.staging[offset..payload_bytes],
            facts.now_us,
        )?;
        let result_kind = classify_result(self.endpoint.mode, role, envelope.result);
        if result_kind != RpcResultKind::Business && payload_bytes != offset {
            return Err(Error::Malformed);
        }
        let fresh = pending.fresh();
        security.replay_commit(replay.take().ok_or(Error::State)?, facts)?;
        pending.commit();
        let p = self.packets[slot].as_mut().ok_or(Error::State)?;
        p.result_digest = Some(digest);
        p.result_kind = Some(result_kind);
        p.last_now = facts.now_us;
        Ok(fresh)
    }
    /// Stop new logical work, not outstanding physical retirement; 停止新逻辑工作，不抹物理义务。
    pub fn stop(&mut self) {
        self.stopped = true;
    }
    #[cfg(feature = "source-runtime")]
    pub(crate) fn original_operation(&self, h: RpcHandle) -> Result<ucn_service::OperationKey> {
        let p = self.packets[self.index(h)?].as_ref().ok_or(Error::State)?;
        if p.server {
            return Err(Error::State);
        }
        Ok(ucn_service::OperationKey {
            service: p.key,
            request_digest: p.request_digest,
        })
    }
    #[cfg(feature = "durable-rpc")]
    pub(crate) fn durable_key<const S: usize, const R: usize>(
        &self,
        h: RpcHandle,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
    ) -> Result<(ucn_service::OperationKey, u64, usize)> {
        let p = self.packets[self.index(h)?].as_ref().ok_or(Error::State)?;
        if self.stopped || !p.server || self.endpoint.mode != OPERATION_DURABLE_AT_MOST_ONCE {
            return Err(Error::State);
        }
        self.binding(self.index(h)?, p.key, session, p.link, facts.now_us)?;
        self.current(p.key, true, security, session, facts, p.link)?;
        security.authorize(access(p.key, session, AccessDirection::Inbound)?, facts)?;
        Ok((
            ucn_service::OperationKey {
                service: p.key,
                request_digest: p.request_digest,
            },
            p.deadline,
            p.max_result,
        ))
    }
    /// Cancel only a stopped physical attempt; logical cancellation is separate.
    /// 只取消已停止的物理Attempt，逻辑取消独立。
    /// # Errors
    /// Driver uncertainty/failure retains the exact Token for retry.
    pub fn cancel_attempt<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: RpcHandle,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<()> {
        if !self.stopped {
            return Err(Error::State);
        }
        self.cancel_physical(h, adapter, driver)
    }
    #[cfg(any(feature = "journal-runtime", feature = "source-runtime"))]
    pub(crate) fn cancel_expired_attempt<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: RpcHandle,
        now: u64,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<()> {
        if !self.stopped && now < self.view(h)?.deadline {
            return Err(Error::State);
        }
        self.cancel_physical(h, adapter, driver)
    }
    fn cancel_physical<const L: usize, const T: usize, const RX: usize>(
        &mut self,
        h: RpcHandle,
        adapter: &mut AdapterOwner<'_, L, T, RX, F>,
        driver: &mut impl TxDriver,
    ) -> Result<()> {
        let token = self.packets[self.index(h)?]
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
    /// Retire semantic correlation plus expired server packet, never a held physical buffer.
    /// 退休语义关联及过期Server报文，不能回收仍被物理持有的Buffer。
    /// # Errors
    /// Unexpired server/active client/held Token prevents reuse.
    pub fn retire<const A: usize, const B: usize, const O: usize>(
        &mut self,
        h: RpcHandle,
        service: &mut ServiceOwner<A, B, O>,
        now: u64,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let p = self.packets[slot].as_ref().ok_or(Error::State)?;
        if p.token.is_some() || now < p.last_now {
            return Err(Error::State);
        }
        if p.server {
            if now < p.deadline {
                return Err(Error::State);
            }
            service.retire_server(p.service, now)?;
        } else {
            service.retire(p.service)?;
        }
        self.packets[slot] = None;
        self.bytes[slot].fill(0);
        Ok(())
    }
}
fn context(plan: OpenPacketPlan<'_>) -> Result<Fingerprint> {
    if let OriginContext::Sequenced { fingerprint, .. } = plan.origin_context {
        Ok(fingerprint)
    } else {
        Err(Error::Unsupported)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn durable_uncertainty_is_not_a_normal_application_error_or_result_code() {
        assert_eq!(
            classify_result(2, InteractionRole::Error, ucn_types::DURABLE_RPC_IN_DOUBT),
            RpcResultKind::InDoubt
        );
        assert_eq!(
            classify_result(2, InteractionRole::Error, ucn_types::DURABLE_RPC_NO_EFFECT),
            RpcResultKind::NoEffect
        );
        assert_eq!(
            classify_result(2, InteractionRole::Error, 14),
            RpcResultKind::Business
        );
        assert_eq!(
            classify_result(2, InteractionRole::Result, ucn_types::DURABLE_RPC_IN_DOUBT),
            RpcResultKind::Business
        );
        assert_eq!(
            classify_result(1, InteractionRole::Error, ucn_types::DURABLE_RPC_IN_DOUBT),
            RpcResultKind::Business
        );
    }
    fn key(operation: u64) -> ServiceKey {
        ServiceKey {
            realm: 1,
            client: [1; 16],
            server: [2; 16],
            client_generation: 1,
            server_generation: 1,
            client_address: 1,
            server_address: 2,
            operation,
            service: 0x40,
            opcode: 0,
            session: 1,
            key_generation: 1,
            origin: OriginSecurity::O1,
            policy: 1,
            security: [5; 16],
        }
    }
    #[test]
    fn bounded_operation_history_survives_key_policy_changes_and_never_wraps() {
        let (history, seen) = History::plan(None, key(100)).unwrap();
        assert!(!seen);
        let (history, seen) = History::plan(Some(history), key(99)).unwrap();
        assert!(!seen);
        let (history, seen) = History::plan(Some(history), key(99)).unwrap();
        assert!(seen);
        assert!(matches!(
            History::plan(Some(history), key(36)),
            Err(Error::Replay)
        ));
        let mut rotated = key(99);
        rotated.policy = 2;
        rotated.key_generation = 2;
        assert!(History::plan(Some(history), rotated).unwrap().1);
        let mut newer = key(99);
        newer.session = 2;
        let (history, seen) = History::plan(Some(history), newer).unwrap();
        assert!(!seen);
        assert!(matches!(
            History::plan(Some(history), key(101)),
            Err(Error::Replay)
        ));
        newer.client_generation = 2;
        assert!(matches!(
            History::plan(Some(history), newer),
            Err(Error::Unsupported)
        ));
        let (history, _) = History::plan(
            Some(history),
            ServiceKey {
                session: 2,
                ..key(u64::MAX)
            },
        )
        .unwrap();
        assert!(
            History::plan(
                Some(history),
                ServiceKey {
                    session: 2,
                    ..key(u64::MAX)
                }
            )
            .unwrap()
            .1
        );
        assert_eq!(status(InteractionRole::Result, u16::MAX), 65535);
        assert_eq!(status(InteractionRole::Error, 0), -1);
        assert_eq!(status(InteractionRole::Error, u16::MAX), -65536);
    }
}
