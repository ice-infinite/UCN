use ucn_security::Binding;
use ucn_types::{
    AddressWidth, C0TransactionId, Error, HopLimit, LinkInstanceGeneration, NodeAddress,
    PeerSessionGeneration, RealmId, Result, RouteGeneration,
};

use crate::{RerrPayload, RerrReason, RrepPayload, RreqPayload};

/// 一条业务流量的完整路由身份域。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RouteDomain {
    /// Realm。
    pub realm: RealmId,
    /// 原始业务发送者 Binding。
    pub origin: Binding,
    /// 原始业务发送者 Session Generation。
    pub origin_session_generation: PeerSessionGeneration,
    /// 目标 Binding。
    pub destination: Binding,
}

/// Coordinator 从当前 Adapter/Security/Capability 事实构造的 Link 引用。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LinkRef {
    link_id: u16,
    link_generation: LinkInstanceGeneration,
    peer: Binding,
    frame_mtu: u16,
    cost: u32,
    capability_bits: u16,
}

impl LinkRef {
    /// 构造完整 Link 引用。
    ///
    /// # Errors
    ///
    /// Link ID、代际、Peer、MTU 或成本非法时返回错误。
    pub fn new(
        link_id: u16,
        link_generation: LinkInstanceGeneration,
        peer: Binding,
        frame_mtu: u16,
        cost: u32,
        capability_bits: u16,
    ) -> Result<Self> {
        if link_id == 0
            || link_id == u16::MAX
            || peer.generation.is_unbound()
            || peer.principal == [0; 16]
            || frame_mtu == 0
            || cost == 0
        {
            return Err(Error::Argument);
        }
        Ok(Self {
            link_id,
            link_generation,
            peer,
            frame_mtu,
            cost,
            capability_bits,
        })
    }

    /// 返回 Link ID。
    #[must_use]
    pub const fn link_id(self) -> u16 {
        self.link_id
    }

    /// 返回 Link Instance Generation。
    #[must_use]
    pub const fn link_generation(self) -> LinkInstanceGeneration {
        self.link_generation
    }

    /// 返回当前认证 Peer Binding。
    #[must_use]
    pub const fn peer(self) -> Binding {
        self.peer
    }

    /// 返回完整 Frame MTU。
    #[must_use]
    pub const fn frame_mtu(self) -> u16 {
        self.frame_mtu
    }

    /// 返回保守单跳成本。
    #[must_use]
    pub const fn cost(self) -> u32 {
        self.cost
    }

    /// 返回该 Link 当前认证能力位。
    #[must_use]
    pub const fn capability_bits(self) -> u16 {
        self.capability_bits
    }
}

/// 基础发现的 canonical key。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DiscoveryKey {
    /// Realm。
    pub realm: RealmId,
    /// 发现发起者 Binding。
    pub origin: Binding,
    /// 发起者 Session Generation。
    pub origin_session_generation: PeerSessionGeneration,
    /// 请求的目标 Address。
    pub target_address: NodeAddress,
    /// C0 Transaction ID，同时作为发现因果 ID。
    pub transaction_id: C0TransactionId,
}

/// 完整 RREQ 语义对象；外层 C0 Codec 负责地址和 Transaction ID。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RreqMessage {
    /// Canonical discovery key。
    pub key: DiscoveryKey,
    /// 剩余 Hop Limit。
    pub remaining_hops: HopLimit,
    /// 冻结 Payload。
    pub payload: RreqPayload,
}

/// 完整 RREP 语义对象。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RrepMessage {
    /// 必须与 Reverse Slot 精确匹配的 key。
    pub key: DiscoveryKey,
    /// 返回的路径事实。
    pub payload: RrepPayload,
}

/// Route Owner 固定配置。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RouteConfig {
    /// Route Owner 实例。
    pub owner_instance: u32,
    /// 本机 Realm。
    pub realm: RealmId,
    /// Realm 地址宽度。
    pub address_width: AddressWidth,
    /// 本机当前 Binding。
    pub local: Binding,
    /// 本机当前 Session Generation。
    pub local_session_generation: PeerSessionGeneration,
    /// 本地发现总 Deadline。
    pub discovery_lifetime_us: u64,
    /// RREQ 重试间隔。
    pub discovery_retry_us: u64,
    /// 最大发送次数。
    pub discovery_max_attempts: u8,
    /// Relay Reverse Slot 生命周期。
    pub reverse_lifetime_us: u64,
    /// 本地 Dynamic `SoftRoute` 生命周期。
    pub route_lifetime_us: u64,
    /// 基础发现最大 Hop Limit。
    pub maximum_hops: HopLimit,
}

/// 精确 Discovery Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DiscoveryHandle {
    owner_instance: u32,
    slot: u16,
    generation: u32,
    transaction_id: C0TransactionId,
}

/// 一次发现启动或合并的结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DiscoveryStart {
    /// 精确 Handle。
    pub handle: DiscoveryHandle,
    /// 首次创建时需要发送；合并时用于核对同一消息。
    pub request: RreqMessage,
    /// `true` 表示新建；`false` 表示合并到现有目标发现。
    pub created: bool,
}

/// Relay 接收 RREQ 后的唯一动作。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RequestAction {
    /// 精确重复；不刷新 Reverse Slot 或 Route 租期。
    Duplicate,
    /// 本机是目标，应生成 RREP。
    LocalTarget,
    /// 向下一跳广播一次更新后的 RREQ。
    Forward(RreqMessage),
}

/// 上游 RREP 发送完成前持有的精确 obligation。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReplyForwardHandle {
    owner_instance: u32,
    slot: u16,
    generation: u32,
    key: DiscoveryKey,
}

/// RREP 处理后的动作。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReplyAction {
    /// 已到达本地 Origin，匹配 Discovery 完成。
    ReachedOrigin(SoftRouteView),
    /// 沿 Reverse Slot 返回上游；完成后必须 ACK obligation。
    Forward {
        /// 不改写的同一 RREP。
        message: RrepMessage,
        /// 上游精确 Link。
        upstream: LinkRef,
        /// 成功/失败后都必须显式完成的 obligation。
        completion: ReplyForwardHandle,
    },
}

/// 静态 fallback；不拥有 Route Generation，也不被 RERR 删除。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StaticRoute {
    /// 目标 Binding。
    pub destination: Binding,
    /// 下一跳 Link。
    pub next_hop: LinkRef,
    /// 路径 Frame MTU。
    pub path_frame_mtu: u16,
}

/// 由 Route Owner 签发的易失 `SoftRoute` 快照。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SoftRouteView {
    owner_instance: u32,
    domain: RouteDomain,
    route_generation: RouteGeneration,
    route_causal_id: u64,
    next_hop: LinkRef,
    hop_count: u8,
    cost: u32,
    path_frame_mtu: u16,
    capability_bits: u16,
    expires_at_us: u64,
}

impl SoftRouteView {
    /// 返回签发该 View 的 Route Owner 实例。
    #[must_use]
    pub const fn owner_instance(self) -> u32 {
        self.owner_instance
    }

    /// 返回完整 Route Domain。
    #[must_use]
    pub const fn domain(self) -> RouteDomain {
        self.domain
    }

    /// 返回本地 Route Generation。
    #[must_use]
    pub const fn route_generation(self) -> RouteGeneration {
        self.route_generation
    }

    /// 返回原发现因果 ID。
    #[must_use]
    pub const fn route_causal_id(self) -> u64 {
        self.route_causal_id
    }

    /// 返回下一跳。
    #[must_use]
    pub const fn next_hop(self) -> LinkRef {
        self.next_hop
    }

    /// 返回 Hop Count。
    #[must_use]
    pub const fn hop_count(self) -> u8 {
        self.hop_count
    }

    /// 返回累计成本。
    #[must_use]
    pub const fn cost(self) -> u32 {
        self.cost
    }

    /// 返回路径 Frame MTU。
    #[must_use]
    pub const fn path_frame_mtu(self) -> u16 {
        self.path_frame_mtu
    }

    /// 返回能力交集。
    #[must_use]
    pub const fn capability_bits(self) -> u16 {
        self.capability_bits
    }

    /// 返回本地半开租期。
    #[must_use]
    pub const fn expires_at_us(self) -> u64 {
        self.expires_at_us
    }
}

/// 每次使用 `SoftRoute` 时由 Coordinator 提供的当前事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RouteUseFacts {
    /// 可信本地单调时间。
    pub now_us: u64,
    /// 当前 Origin Session Generation。
    pub origin_session_generation: PeerSessionGeneration,
    /// 当前目标 Binding Generation。
    pub destination_binding_generation: u32,
    /// 当前 Link Instance Generation。
    pub link_generation: LinkInstanceGeneration,
}

#[derive(Clone, Copy)]
struct DiscoveryRecord {
    target_address: NodeAddress,
    request: RreqMessage,
    deadline_us: u64,
    next_retry_us: u64,
    attempts: u8,
}

#[derive(Clone, Copy)]
struct DiscoverySlot {
    generation: u32,
    value: Option<DiscoveryRecord>,
}

impl DiscoverySlot {
    const EMPTY: Self = Self {
        generation: 0,
        value: None,
    };
}

#[derive(Clone, Copy)]
struct ReverseRecord {
    key: DiscoveryKey,
    accepted_request: RreqMessage,
    upstream: LinkRef,
    expires_at_us: u64,
    last_request_action_us: u64,
    reply_pending: bool,
    reply_completed: bool,
}

#[derive(Clone, Copy)]
struct ReverseSlot {
    generation: u32,
    value: Option<ReverseRecord>,
}

impl ReverseSlot {
    const EMPTY: Self = Self {
        generation: 0,
        value: None,
    };
}

#[derive(Clone, Copy)]
enum ReplyPlan {
    Local {
        discovery_index: usize,
    },
    Relay {
        reverse_index: usize,
        reverse: ReverseRecord,
        forward_slot: u16,
    },
}

#[derive(Clone, Copy)]
struct RouteSlot {
    value: Option<SoftRouteView>,
}

impl RouteSlot {
    const EMPTY: Self = Self { value: None };
}

#[derive(Clone, Copy)]
struct StaticSlot {
    value: Option<StaticRoute>,
}

impl StaticSlot {
    const EMPTY: Self = Self { value: None };
}

/// 固定容量基础 Route Owner。
pub struct RouteOwner<
    const DISCOVERIES: usize,
    const REVERSE: usize,
    const ROUTES: usize,
    const STATIC: usize,
> {
    config: RouteConfig,
    next_transaction_id: u64,
    next_route_generation: u32,
    discoveries: [DiscoverySlot; DISCOVERIES],
    reverse: [ReverseSlot; REVERSE],
    routes: [RouteSlot; ROUTES],
    static_routes: [StaticSlot; STATIC],
    expire_cursor: usize,
}

/// Nano：2 个发现、4 个 Reverse、4 个 SoftRoute、4 个静态项。
pub type NanoRouteOwner = RouteOwner<2, 4, 4, 4>;
/// Lite：4 个发现、12 个 Reverse、16 个 SoftRoute、8 个静态项。
pub type LiteRouteOwner = RouteOwner<4, 12, 16, 8>;
/// Full：8 个发现、32 个 Reverse、48 个 SoftRoute、16 个静态项。
pub type FullRouteOwner = RouteOwner<8, 32, 48, 16>;

impl<const DISCOVERIES: usize, const REVERSE: usize, const ROUTES: usize, const STATIC: usize>
    RouteOwner<DISCOVERIES, REVERSE, ROUTES, STATIC>
{
    /// 建立空 Route Owner。
    ///
    /// # Errors
    ///
    /// 固定容量、身份、时限或初始高水位非法时返回配置错误。
    pub fn new(config: RouteConfig, first_transaction_id: u64) -> Result<Self> {
        if DISCOVERIES == 0
            || REVERSE == 0
            || ROUTES == 0
            || STATIC == 0
            || DISCOVERIES > usize::from(u16::MAX)
            || REVERSE > usize::from(u16::MAX)
            || config.owner_instance == 0
            || config.local.generation.is_unbound()
            || config.local.principal == [0; 16]
            || config.discovery_lifetime_us == 0
            || config.discovery_retry_us == 0
            || config.discovery_retry_us > config.discovery_lifetime_us
            || config.discovery_max_attempts == 0
            || config.reverse_lifetime_us == 0
            || config.route_lifetime_us == 0
            || first_transaction_id == 0
        {
            return Err(Error::Config);
        }
        NodeAddress::new(config.local.address.get(), config.address_width)
            .map_err(|_| Error::Config)?;
        Ok(Self {
            config,
            next_transaction_id: first_transaction_id,
            next_route_generation: 1,
            discoveries: [DiscoverySlot::EMPTY; DISCOVERIES],
            reverse: [ReverseSlot::EMPTY; REVERSE],
            routes: [RouteSlot::EMPTY; ROUTES],
            static_routes: [StaticSlot::EMPTY; STATIC],
            expire_cursor: 0,
        })
    }

    /// 安装或精确更新一个静态 fallback。
    ///
    /// # Errors
    ///
    /// 目标、Link、MTU 或固定容量非法时失败且旧表不变。
    pub fn install_static(&mut self, route: StaticRoute) -> Result<()> {
        self.validate_binding(route.destination)?;
        if route.path_frame_mtu == 0
            || route.path_frame_mtu > route.next_hop.frame_mtu()
            || route.destination == self.config.local
        {
            return Err(Error::Argument);
        }
        if let Some(index) = self.static_routes.iter().position(|slot| {
            slot.value
                .is_some_and(|value| value.destination == route.destination)
        }) {
            self.static_routes[index].value = Some(route);
            return Ok(());
        }
        let index = self
            .static_routes
            .iter()
            .position(|slot| slot.value.is_none())
            .ok_or(Error::NoSpace)?;
        self.static_routes[index].value = Some(route);
        Ok(())
    }

    /// 为本地目标启动或合并一个基础发现。
    ///
    /// # Errors
    ///
    /// 目标、时间、计数器或容量不满足时失败且不占槽。
    pub fn ensure_discovery(
        &mut self,
        target_address: NodeAddress,
        required_capability_bits: u16,
        minimum_payload_budget: u16,
        flags: u8,
        now_us: u64,
    ) -> Result<DiscoveryStart> {
        NodeAddress::new(target_address.get(), self.config.address_width)?;
        if target_address == self.config.local.address || minimum_payload_budget == 0 {
            return Err(Error::Argument);
        }
        if let Some((index, record)) =
            self.discoveries
                .iter()
                .enumerate()
                .find_map(|(index, slot)| {
                    slot.value
                        .filter(|record| record.target_address == target_address)
                        .map(|record| (index, record))
                })
        {
            if now_us >= record.deadline_us {
                return Err(Error::Timeout);
            }
            if record.request.payload.required_capability_bits != required_capability_bits
                || record.request.payload.minimum_payload_budget != minimum_payload_budget
                || record.request.payload.flags != flags
            {
                return Err(Error::State);
            }
            return Ok(DiscoveryStart {
                handle: self.discovery_handle(index, record.request.key.transaction_id)?,
                request: record.request,
                created: false,
            });
        }
        let payload = RreqPayload {
            accumulated_cost: 0,
            minimum_payload_budget,
            required_capability_bits,
            flags,
        };
        crate::encode_rreq_payload(payload, &mut [0; crate::RREQ_PAYLOAD_BYTES])?;
        let index = self
            .discoveries
            .iter()
            .position(|slot| slot.value.is_none())
            .ok_or(Error::NoSpace)?;
        let transaction_id = self.allocate_transaction_id()?;
        let deadline_us = now_us
            .checked_add(self.config.discovery_lifetime_us)
            .ok_or(Error::Exhausted)?;
        let next_retry_us = now_us
            .checked_add(self.config.discovery_retry_us)
            .ok_or(Error::Exhausted)?;
        let request = RreqMessage {
            key: DiscoveryKey {
                realm: self.config.realm,
                origin: self.config.local,
                origin_session_generation: self.config.local_session_generation,
                target_address,
                transaction_id,
            },
            remaining_hops: self.config.maximum_hops,
            payload,
        };
        let next_generation = self.discoveries[index]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let handle_slot = u16::try_from(index + 1).map_err(|_| Error::NoSpace)?;
        self.discoveries[index] = DiscoverySlot {
            generation: next_generation,
            value: Some(DiscoveryRecord {
                target_address,
                request,
                deadline_us,
                next_retry_us,
                attempts: 1,
            }),
        };
        Ok(DiscoveryStart {
            handle: DiscoveryHandle {
                owner_instance: self.config.owner_instance,
                slot: handle_slot,
                generation: next_generation,
                transaction_id,
            },
            request,
            created: true,
        })
    }

    /// 到重试时刻时返回同一 RREQ；原 Transaction ID 与 Deadline 不变。
    ///
    /// # Errors
    ///
    /// Handle、Deadline、次数或调用时刻不满足时返回错误且不改状态。
    pub fn retry_discovery(&mut self, handle: DiscoveryHandle, now_us: u64) -> Result<RreqMessage> {
        let index = self.discovery_index(handle)?;
        let record = self.discoveries[index].value.ok_or(Error::NotFound)?;
        if now_us >= record.deadline_us {
            return Err(Error::Timeout);
        }
        if now_us < record.next_retry_us {
            return Err(Error::State);
        }
        if record.attempts >= self.config.discovery_max_attempts {
            return Err(Error::Exhausted);
        }
        let next_retry_us = record
            .next_retry_us
            .checked_add(self.config.discovery_retry_us)
            .ok_or(Error::Exhausted)?;
        self.discoveries[index].value = Some(DiscoveryRecord {
            attempts: record.attempts + 1,
            next_retry_us,
            ..record
        });
        Ok(record.request)
    }

    /// 接收一个已经完成 Wire、Hop Security 与 Peer 身份验证的 RREQ。
    ///
    /// # Errors
    ///
    /// 身份、Hop、能力、算术或容量不成立时失败；预检失败时 Reverse/Route 表不变。
    pub fn on_rreq(
        &mut self,
        request: RreqMessage,
        ingress: LinkRef,
        now_us: u64,
    ) -> Result<RequestAction> {
        self.validate_discovery_key(request.key)?;
        if ingress.peer() == self.config.local
            || request.payload.minimum_payload_budget == 0
            || request.payload.minimum_payload_budget > ingress.frame_mtu()
            || request.payload.required_capability_bits & !ingress.capability_bits() != 0
        {
            return Err(Error::Access);
        }
        if let Some(index) = self
            .reverse
            .iter()
            .position(|slot| slot.value.is_some_and(|record| record.key == request.key))
        {
            let mut record = self.reverse[index].value.ok_or(Error::State)?;
            if record.accepted_request != request || record.upstream != ingress {
                return Err(Error::Security);
            }
            let retry_at = record
                .last_request_action_us
                .checked_add(self.config.discovery_retry_us)
                .ok_or(Error::Exhausted)?;
            if now_us >= record.expires_at_us || record.reply_pending || now_us < retry_at {
                return Ok(RequestAction::Duplicate);
            }
            record.last_request_action_us = now_us;
            record.reply_completed = false;
            self.reverse[index].value = Some(record);
            if request.key.target_address == self.config.local.address {
                return Ok(RequestAction::LocalTarget);
            }
            return Self::forwarded_rreq(request, ingress).map(RequestAction::Forward);
        }
        if request.remaining_hops.get() <= 1
            && request.key.target_address != self.config.local.address
        {
            return Err(Error::Exhausted);
        }
        let reverse_index = self
            .reverse
            .iter()
            .position(|slot| slot.value.is_none())
            .ok_or(Error::NoSpace)?;
        let return_domain = RouteDomain {
            realm: request.key.realm,
            origin: self.config.local,
            origin_session_generation: self.config.local_session_generation,
            destination: request.key.origin,
        };
        let route_index = self.preflight_route_slot(return_domain)?;
        let route_generation = self.preview_route_generation()?;
        let reverse_deadline = now_us
            .checked_add(self.config.reverse_lifetime_us)
            .ok_or(Error::Exhausted)?;
        let route_deadline = now_us
            .checked_add(self.config.route_lifetime_us)
            .ok_or(Error::Exhausted)?;
        let reverse_generation = self.reverse[reverse_index]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let forwarded = (request.key.target_address != self.config.local.address)
            .then(|| Self::forwarded_rreq(request, ingress))
            .transpose()?;

        self.commit_route(
            route_index,
            return_domain,
            route_generation,
            request.key.transaction_id.get(),
            ingress,
            1,
            ingress.cost(),
            ingress.frame_mtu(),
            request.payload.required_capability_bits,
            route_deadline,
        );
        self.consume_route_generation();
        self.reverse[reverse_index] = ReverseSlot {
            generation: reverse_generation,
            value: Some(ReverseRecord {
                key: request.key,
                accepted_request: request,
                upstream: ingress,
                expires_at_us: reverse_deadline,
                last_request_action_us: now_us,
                reply_pending: false,
                reply_completed: false,
            }),
        };
        match forwarded {
            Some(message) => Ok(RequestAction::Forward(message)),
            None => Ok(RequestAction::LocalTarget),
        }
    }

    /// 目标节点从精确 RREQ 生成 RREP。
    ///
    /// # Errors
    ///
    /// 目标、Reverse Slot、Deadline 或本机能力不匹配时返回错误。
    pub fn make_rrep(
        &self,
        request: RreqMessage,
        local_capability_bits: u16,
        local_frame_mtu: u16,
        now_us: u64,
    ) -> Result<RrepMessage> {
        if request.key.target_address != self.config.local.address
            || local_frame_mtu == 0
            || local_frame_mtu < request.payload.minimum_payload_budget
            || request.payload.required_capability_bits & !local_capability_bits != 0
        {
            return Err(Error::Access);
        }
        let reverse = self
            .reverse
            .iter()
            .filter_map(|slot| slot.value)
            .find(|record| record.key == request.key)
            .ok_or(Error::NotFound)?;
        if now_us >= reverse.expires_at_us {
            return Err(Error::Timeout);
        }
        Ok(RrepMessage {
            key: request.key,
            payload: RrepPayload {
                destination_principal: self.config.local.principal,
                destination_binding_generation: self.config.local.generation.get(),
                hop_count: 0,
                accumulated_cost: 0,
                path_frame_mtu: local_frame_mtu,
                capability_bits: local_capability_bits,
            },
        })
    }

    /// 接收 RREP，原子安装一个本地 SoftRoute，并返回上游 obligation 或完成本地发现。
    ///
    /// # Errors
    ///
    /// Reverse、身份、算术、Deadline 或容量不成立时失败且不安装 Route。
    #[allow(clippy::too_many_lines)]
    pub fn on_rrep(
        &mut self,
        reply: RrepMessage,
        ingress: LinkRef,
        now_us: u64,
    ) -> Result<ReplyAction> {
        self.validate_discovery_key(reply.key)?;
        if reply.payload.destination_principal == [0; 16]
            || reply.payload.destination_binding_generation == 0
            || reply.payload.hop_count == u8::MAX
            || reply.payload.path_frame_mtu == 0
            || reply.key.target_address.get() == 0
        {
            return Err(Error::Malformed);
        }
        let local_origin = reply.key.origin == self.config.local
            && reply.key.origin_session_generation == self.config.local_session_generation;
        let reply_plan = if local_origin {
            let discovery_index = self
                .discoveries
                .iter()
                .position(|slot| {
                    slot.value
                        .is_some_and(|record| record.request.key == reply.key)
                })
                .ok_or(Error::NotFound)?;
            ReplyPlan::Local { discovery_index }
        } else {
            let reverse_index = self
                .reverse
                .iter()
                .position(|slot| slot.value.is_some_and(|record| record.key == reply.key))
                .ok_or(Error::NotFound)?;
            let record = self.reverse[reverse_index].value.ok_or(Error::State)?;
            if now_us >= record.expires_at_us || record.reply_pending || record.reply_completed {
                return Err(Error::State);
            }
            ReplyPlan::Relay {
                reverse_index,
                reverse: record,
                forward_slot: u16::try_from(reverse_index.checked_add(1).ok_or(Error::NoSpace)?)
                    .map_err(|_| Error::NoSpace)?,
            }
        };
        let accepted_request = match reply_plan {
            ReplyPlan::Local { discovery_index } => {
                self.discoveries[discovery_index]
                    .value
                    .ok_or(Error::State)?
                    .request
            }
            ReplyPlan::Relay { reverse, .. } => reverse.accepted_request,
        };
        let destination = Binding {
            address: reply.key.target_address,
            generation: ucn_types::BindingGeneration::active(
                reply.payload.destination_binding_generation,
            )?,
            principal: reply.payload.destination_principal,
        };
        self.validate_binding(destination)?;
        let domain = RouteDomain {
            realm: reply.key.realm,
            origin: reply.key.origin,
            origin_session_generation: reply.key.origin_session_generation,
            destination,
        };
        let route_index = self.preflight_route_slot(domain)?;
        let route_generation = self.preview_route_generation()?;
        let cost = reply
            .payload
            .accumulated_cost
            .checked_add(ingress.cost())
            .ok_or(Error::Exhausted)?;
        let hop_count = reply
            .payload
            .hop_count
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let route_deadline = now_us
            .checked_add(self.config.route_lifetime_us)
            .ok_or(Error::Exhausted)?;
        let path_frame_mtu = reply.payload.path_frame_mtu.min(ingress.frame_mtu());
        let capability_bits = reply.payload.capability_bits & ingress.capability_bits();
        if hop_count > self.config.maximum_hops.get()
            || path_frame_mtu < accepted_request.payload.minimum_payload_budget
            || accepted_request.payload.required_capability_bits & !capability_bits != 0
        {
            return Err(Error::Access);
        }
        let view = SoftRouteView {
            owner_instance: self.config.owner_instance,
            domain,
            route_generation,
            route_causal_id: reply.key.transaction_id.get(),
            next_hop: ingress,
            hop_count,
            cost,
            path_frame_mtu,
            capability_bits,
            expires_at_us: route_deadline,
        };
        self.routes[route_index].value = Some(view);
        self.consume_route_generation();
        match reply_plan {
            ReplyPlan::Local { discovery_index } => {
                self.discoveries[discovery_index].value = None;
                Ok(ReplyAction::ReachedOrigin(view))
            }
            ReplyPlan::Relay {
                reverse_index,
                reverse,
                forward_slot,
            } => {
                let mut pending = reverse;
                pending.reply_pending = true;
                self.reverse[reverse_index].value = Some(pending);
                Ok(ReplyAction::Forward {
                    message: RrepMessage {
                        payload: RrepPayload {
                            hop_count,
                            accumulated_cost: cost,
                            path_frame_mtu,
                            capability_bits,
                            ..reply.payload
                        },
                        ..reply
                    },
                    upstream: reverse.upstream,
                    completion: ReplyForwardHandle {
                        owner_instance: self.config.owner_instance,
                        slot: forward_slot,
                        generation: self.reverse[reverse_index].generation,
                        key: reply.key,
                    },
                })
            }
        }
    }

    /// 在上游 RREP Driver 终态后退休 Reverse obligation。
    ///
    /// # Errors
    ///
    /// Handle 或 key 不匹配时返回错误且保留 obligation。
    pub fn complete_rrep_forward(
        &mut self,
        handle: ReplyForwardHandle,
        delivered: bool,
    ) -> Result<()> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let slot = self.reverse.get_mut(index).ok_or(Error::NotFound)?;
        if slot.generation != handle.generation
            || !slot
                .value
                .is_some_and(|record| record.key == handle.key && record.reply_pending)
        {
            return Err(Error::State);
        }
        let mut record = slot.value.ok_or(Error::State)?;
        record.reply_pending = false;
        record.reply_completed = delivered;
        slot.value = Some(record);
        Ok(())
    }

    /// 按完整 Domain 和当前事实解析动态 Route；找不到动态项时才尝试静态 fallback。
    ///
    /// # Errors
    ///
    /// 当前身份/代际不匹配或不存在可用路径时返回错误。
    pub fn resolve(&self, domain: RouteDomain, facts: RouteUseFacts) -> Result<ResolvedRoute> {
        self.validate_domain(domain)?;
        if facts.origin_session_generation != domain.origin_session_generation
            || facts.destination_binding_generation != domain.destination.generation.get()
        {
            return Err(Error::Replay);
        }
        if let Some(route) = self
            .routes
            .iter()
            .filter_map(|slot| slot.value)
            .find(|route| route.domain == domain)
        {
            if facts.now_us < route.expires_at_us
                && facts.link_generation == route.next_hop.link_generation()
            {
                return Ok(ResolvedRoute::Dynamic(route));
            }
        }
        let route = self
            .static_routes
            .iter()
            .filter_map(|slot| slot.value)
            .find(|route| route.destination == domain.destination)
            .ok_or(Error::NotFound)?;
        if route.next_hop.link_generation() != facts.link_generation {
            return Err(Error::State);
        }
        Ok(ResolvedRoute::Static(route))
    }

    /// 对精确 Link Generation 失效所有动态 Route；不删除静态配置。
    pub fn invalidate_link(&mut self, link_id: u16, generation: LinkInstanceGeneration) -> usize {
        let mut invalidated = 0;
        for slot in &mut self.routes {
            if slot.value.is_some_and(|route| {
                route.next_hop.link_id() == link_id
                    && route.next_hop.link_generation() == generation
            }) {
                slot.value = None;
                invalidated += 1;
            }
        }
        invalidated
    }

    /// 构造与一个已签发 `SoftRoute` 精确绑定的 RERR Payload。
    ///
    /// # Errors
    ///
    /// View 不属于本 Owner 时返回错误。
    pub fn make_rerr(&self, route: SoftRouteView, reason: RerrReason) -> Result<RerrPayload> {
        if route.owner_instance != self.config.owner_instance {
            return Err(Error::NotFound);
        }
        Ok(RerrPayload {
            realm: route.domain.realm.get(),
            origin_principal: route.domain.origin.principal,
            origin_address: route.domain.origin.address.get(),
            origin_binding_generation: route.domain.origin.generation.get(),
            origin_session_generation: route.domain.origin_session_generation.get(),
            destination_principal: route.domain.destination.principal,
            destination_address: route.domain.destination.address.get(),
            destination_binding_generation: route.domain.destination.generation.get(),
            route_generation: route.route_generation.get(),
            route_causal_id: route.route_causal_id,
            reporter_principal: route.next_hop.peer().principal,
            reporter_address: route.next_hop.peer().address.get(),
            reporter_binding_generation: route.next_hop.peer().generation.get(),
            failed_link_id: route.next_hop.link_id(),
            failed_link_generation: route.next_hop.link_generation().get(),
            reason,
        })
    }

    /// 只在 RERR 完整匹配 Route、因果 ID、报告 Peer 和 Link Generation 时失效该项。
    ///
    /// # Errors
    ///
    /// RERR 结构非法时返回错误；过期/错绑提示返回 `false` 且不改状态。
    pub fn on_rerr(&mut self, payload: RerrPayload) -> Result<bool> {
        let domain = RouteDomain {
            realm: RealmId::new(payload.realm)?,
            origin: Binding {
                address: NodeAddress::new(payload.origin_address, self.config.address_width)?,
                generation: ucn_types::BindingGeneration::active(
                    payload.origin_binding_generation,
                )?,
                principal: payload.origin_principal,
            },
            origin_session_generation: PeerSessionGeneration::new(
                payload.origin_session_generation,
            )?,
            destination: Binding {
                address: NodeAddress::new(payload.destination_address, self.config.address_width)?,
                generation: ucn_types::BindingGeneration::active(
                    payload.destination_binding_generation,
                )?,
                principal: payload.destination_principal,
            },
        };
        self.validate_domain(domain)?;
        let reporter = Binding {
            address: NodeAddress::new(payload.reporter_address, self.config.address_width)?,
            generation: ucn_types::BindingGeneration::active(payload.reporter_binding_generation)?,
            principal: payload.reporter_principal,
        };
        let Some(index) = self.routes.iter().position(|slot| {
            slot.value.is_some_and(|route| {
                route.domain == domain
                    && route.route_generation.get() == payload.route_generation
                    && route.route_causal_id == payload.route_causal_id
                    && route.next_hop.peer() == reporter
                    && route.next_hop.link_id() == payload.failed_link_id
                    && route.next_hop.link_generation().get() == payload.failed_link_generation
            })
        }) else {
            return Ok(false);
        };
        self.routes[index].value = None;
        Ok(true)
    }

    /// 按持久游标检查一个 Discovery、Reverse 或 Route 槽；不会在新请求路径隐式驱逐。
    #[must_use]
    pub fn expire_one(&mut self, now_us: u64) -> bool {
        let total = DISCOVERIES + REVERSE + ROUTES;
        let index = self.expire_cursor;
        self.expire_cursor = (self.expire_cursor + 1) % total;
        if index < DISCOVERIES {
            let expired = self.discoveries[index]
                .value
                .is_some_and(|record| now_us >= record.deadline_us);
            if expired {
                self.discoveries[index].value = None;
            }
            return expired;
        }
        if index < DISCOVERIES + REVERSE {
            let reverse_index = index - DISCOVERIES;
            let expired = self.reverse[reverse_index]
                .value
                .is_some_and(|record| now_us >= record.expires_at_us);
            if expired {
                self.reverse[reverse_index].value = None;
            }
            return expired;
        }
        let route_index = index - DISCOVERIES - REVERSE;
        let expired = self.routes[route_index]
            .value
            .is_some_and(|record| now_us >= record.expires_at_us);
        if expired {
            self.routes[route_index].value = None;
        }
        expired
    }

    /// 返回当前固定表占用计数。
    #[must_use]
    pub fn counts(&self) -> (usize, usize, usize, usize) {
        (
            self.discoveries
                .iter()
                .filter(|slot| slot.value.is_some())
                .count(),
            self.reverse
                .iter()
                .filter(|slot| slot.value.is_some())
                .count(),
            self.routes
                .iter()
                .filter(|slot| slot.value.is_some())
                .count(),
            self.static_routes
                .iter()
                .filter(|slot| slot.value.is_some())
                .count(),
        )
    }

    fn validate_binding(&self, binding: Binding) -> Result<()> {
        NodeAddress::new(binding.address.get(), self.config.address_width)?;
        if binding.generation.is_unbound() || binding.principal == [0; 16] {
            return Err(Error::Argument);
        }
        Ok(())
    }

    fn forwarded_rreq(request: RreqMessage, ingress: LinkRef) -> Result<RreqMessage> {
        if request.remaining_hops.get() <= 1 {
            return Err(Error::Exhausted);
        }
        let accumulated_cost = request
            .payload
            .accumulated_cost
            .checked_add(ingress.cost())
            .ok_or(Error::Exhausted)?;
        Ok(RreqMessage {
            remaining_hops: HopLimit::new(request.remaining_hops.get() - 1)?,
            payload: RreqPayload {
                accumulated_cost,
                ..request.payload
            },
            ..request
        })
    }

    fn validate_domain(&self, domain: RouteDomain) -> Result<()> {
        if domain.realm != self.config.realm || domain.origin == domain.destination {
            return Err(Error::Argument);
        }
        self.validate_binding(domain.origin)?;
        self.validate_binding(domain.destination)
    }

    fn validate_discovery_key(&self, key: DiscoveryKey) -> Result<()> {
        if key.realm != self.config.realm {
            return Err(Error::Access);
        }
        self.validate_binding(key.origin)?;
        NodeAddress::new(key.target_address.get(), self.config.address_width)?;
        Ok(())
    }

    fn discovery_handle(
        &self,
        index: usize,
        transaction_id: C0TransactionId,
    ) -> Result<DiscoveryHandle> {
        Ok(DiscoveryHandle {
            owner_instance: self.config.owner_instance,
            slot: u16::try_from(index + 1).map_err(|_| Error::NoSpace)?,
            generation: self.discoveries[index].generation,
            transaction_id,
        })
    }

    fn discovery_index(&self, handle: DiscoveryHandle) -> Result<usize> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let slot = self.discoveries.get(index).ok_or(Error::NotFound)?;
        if slot.generation != handle.generation
            || !slot
                .value
                .is_some_and(|record| record.request.key.transaction_id == handle.transaction_id)
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn allocate_transaction_id(&mut self) -> Result<C0TransactionId> {
        if self.next_transaction_id == 0 {
            return Err(Error::Exhausted);
        }
        let value = C0TransactionId::new(self.next_transaction_id)?;
        self.next_transaction_id = self.next_transaction_id.checked_add(1).unwrap_or(0);
        Ok(value)
    }

    fn preview_route_generation(&self) -> Result<RouteGeneration> {
        if self.next_route_generation == 0 {
            return Err(Error::Exhausted);
        }
        RouteGeneration::new(self.next_route_generation)
    }

    fn consume_route_generation(&mut self) {
        self.next_route_generation = self.next_route_generation.checked_add(1).unwrap_or(0);
    }

    fn preflight_route_slot(&self, domain: RouteDomain) -> Result<usize> {
        self.validate_domain(domain)?;
        self.routes
            .iter()
            .position(|slot| slot.value.is_some_and(|route| route.domain == domain))
            .or_else(|| self.routes.iter().position(|slot| slot.value.is_none()))
            .ok_or(Error::NoSpace)
    }

    #[allow(clippy::too_many_arguments)]
    fn commit_route(
        &mut self,
        index: usize,
        domain: RouteDomain,
        route_generation: RouteGeneration,
        route_causal_id: u64,
        next_hop: LinkRef,
        hop_count: u8,
        cost: u32,
        path_frame_mtu: u16,
        capability_bits: u16,
        expires_at_us: u64,
    ) {
        self.routes[index].value = Some(SoftRouteView {
            owner_instance: self.config.owner_instance,
            domain,
            route_generation,
            route_causal_id,
            next_hop,
            hop_count,
            cost,
            path_frame_mtu,
            capability_bits,
            expires_at_us,
        });
    }
}

/// Route 解析的动态/静态结果；两者不可互相冒充。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResolvedRoute {
    /// 易失动态 `SoftRoute`。
    Dynamic(SoftRouteView),
    /// Manifest 静态 fallback。
    Static(StaticRoute),
}
