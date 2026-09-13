#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的最小静态通信核心。
//!
//! 本 crate 只实现可信 O0 静态域中的 C1 单帧 Copy、Endpoint、静态 Path、Q0～Q3
//! 固定队列与有界生命周期。它不包含动态路由、可靠传输、Security 或 Persistence。

use ucn_adapter::{AdapterOwner, LinkHandle, RxView, TerminalOutcome, TxDriver, TxState, TxToken};
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_types::{
    AddressWidth, BindingGeneration, DeliveryGuarantee, Error, HeaderContract, HopLimit,
    HopProfile, InteractionRole, NodeAddress, OriginSecurity, OriginSequence, PayloadKind, Result,
    ServiceId, TrafficClass,
};
use ucn_wire::{C1Frame, CommonHeader, c1_o0_h0_encoded_size, decode_c1_o0_h0, encode_c1_o0_h0};

const CALLBACK_KIND_ENDPOINT: u16 = 1;
const TX_SCHEDULE: [TrafficClass; 12] = [
    TrafficClass::Q0,
    TrafficClass::Q1,
    TrafficClass::Q0,
    TrafficClass::Q2,
    TrafficClass::Q0,
    TrafficClass::Q1,
    TrafficClass::Q0,
    TrafficClass::Q3,
    TrafficClass::Q0,
    TrafficClass::Q1,
    TrafficClass::Q0,
    TrafficClass::Q2,
];

/// Core Runtime 的生命周期。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Lifecycle {
    /// 静态对象可配置，尚未接收 Driver RX。
    Initialized,
    /// 正常接收与发送。
    Running,
    /// 已关闭新接纳，正在对账和退休已有 Token。
    Stopping,
    /// 所有易失 obligation 已退休，可重新启动或释放对象。
    Quiescent,
    /// 无法安全继续的终态。
    Fault,
}

/// 四个业务队列的固定容量分区。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct QueueCapacities {
    depths: [u16; 4],
    offsets: [u16; 4],
    total: u16,
}

impl QueueCapacities {
    /// 建立 Q0～Q3 的固定分区。
    ///
    /// # Errors
    ///
    /// 任一深度为零、总容量溢出或不等于 `TX_SLOTS` 时返回 [`Error::Config`]。
    pub const fn new<const TX_SLOTS: usize>(q0: u16, q1: u16, q2: u16, q3: u16) -> Result<Self> {
        if q0 == 0 || q1 == 0 || q2 == 0 || q3 == 0 || TX_SLOTS > u16::MAX as usize {
            return Err(Error::Config);
        }
        let Some(offset2) = q0.checked_add(q1) else {
            return Err(Error::Config);
        };
        let Some(offset3) = offset2.checked_add(q2) else {
            return Err(Error::Config);
        };
        let Some(total) = offset3.checked_add(q3) else {
            return Err(Error::Config);
        };
        if total as usize != TX_SLOTS {
            return Err(Error::Config);
        }
        Ok(Self {
            depths: [q0, q1, q2, q3],
            offsets: [0, q0, offset2, offset3],
            total,
        })
    }

    fn range(self, traffic_class: TrafficClass) -> core::ops::Range<usize> {
        let class = traffic_class as usize;
        let begin = usize::from(self.offsets[class]);
        begin..begin + usize::from(self.depths[class])
    }
}

/// 一个节点的静态身份与运行边界。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NodeConfig {
    /// Runtime/Handle 的本地实例号。
    pub node_instance: u32,
    /// 必须与传入 Adapter Owner 一致。
    pub adapter_instance: u32,
    /// Realm 固定地址宽度。
    pub address_width: AddressWidth,
    /// 本机静态地址。
    pub local_address: NodeAddress,
    /// 本机静态 Binding 代际。
    pub local_binding_generation: BindingGeneration,
    /// O0 只允许在产品显式声明的可信隔离网络中运行。
    pub trusted_o0_network: bool,
}

/// 一个静态目标。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Target {
    /// 目标节点地址。
    pub address: NodeAddress,
    /// 目标静态 Binding 代际。
    pub binding_generation: BindingGeneration,
    /// 目标 Service/Endpoint。
    pub service_id: ServiceId,
}

/// 最小单帧发送选项。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PublishOptions {
    /// Q0～Q3 业务类别。
    pub traffic_class: TrafficClass,
    /// 当前帧的跳数上限。
    pub hop_limit: HopLimit,
    /// 可选绝对单调 Deadline；等于当前时间即到期。零表示没有 Deadline。
    pub absolute_deadline_us: u64,
}

/// 本地接纳成功后返回的最小回执。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PublishReceipt {
    /// 已经分配且不会回绕的 Origin Sequence。
    pub origin_sequence: OriginSequence,
}

/// Endpoint 的终态处理结论。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EndpointDisposition {
    /// 业务已接受 Payload。
    Accept,
    /// 业务明确丢弃 Payload。
    Drop,
}

/// 一次本地 Endpoint 投递的只读 View。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EndpointMessage<'a> {
    /// 静态 Source 地址。
    pub source: NodeAddress,
    /// 本地静态表中与 Source 对应的 Binding 代际。
    pub source_binding_generation: BindingGeneration,
    /// 目标 Service。
    pub service_id: ServiceId,
    /// 原始 Traffic Class。
    pub traffic_class: TrafficClass,
    /// 帧内剩余 Hop Limit。
    pub hop_limit: HopLimit,
    /// 借用 RX workspace 的 Payload；回调返回后失效。
    pub payload: &'a [u8],
    /// Driver 捕获的本地接收时间。
    pub receive_timestamp_us: u64,
}

/// 固定 Endpoint 的同步、不可持有 Payload 回调。
pub trait EndpointHandler {
    /// 处理一次已经完成结构、目标、静态身份与易失 Replay 校验的消息。
    fn receive(&self, message: EndpointMessage<'_>) -> EndpointDisposition;
}

/// 精确 Endpoint Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EndpointHandle {
    node_instance: u32,
    slot: u16,
    generation: u32,
}

/// 精确静态 Path Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PathHandle {
    node_instance: u32,
    slot: u16,
    generation: u32,
}

/// 一个静态 Path 的配置。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StaticPath {
    /// 目标地址。
    pub destination: NodeAddress,
    /// 目标 Binding 代际。
    pub destination_binding_generation: BindingGeneration,
    /// 精确 Link Handle。
    pub link: LinkHandle,
    /// 该 Path 允许的完整 Frame MTU。
    pub path_frame_mtu: u16,
}

/// 最小静态通信统计。
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Stats {
    /// 成功进入固定队列的帧数。
    pub tx_admitted: u32,
    /// Driver 明确成功完成的帧数。
    pub tx_completed: u32,
    /// 到期、取消或 Driver 失败的帧数。
    pub tx_failed: u32,
    /// 从 Adapter 领取的原子 RX record 数。
    pub rx_claimed: u32,
    /// Endpoint 接受的消息数。
    pub rx_delivered: u32,
    /// 结构、目标、Replay 或 Endpoint 拒绝的消息数。
    pub rx_dropped: u32,
    /// Wire 结构非法的消息数。
    pub malformed: u32,
    /// 易失 Replay Window 拒绝的消息数。
    pub replay_rejected: u32,
}

/// 一轮有界推进的结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StepResult {
    /// 实际完成的有副作用工作数。
    pub work_done: u16,
    /// 当前仍有已知 TX obligation，或本轮恰好用尽预算。
    pub more_work: bool,
    /// 推进后的生命周期。
    pub lifecycle: Lifecycle,
}

/// Nano Profile 的 Core Owner：4 Binding、8 Endpoint、4 Path、16 个 Class Slot、256 B Frame。
pub type NanoCoreNode<'a> = CoreNode<'a, 4, 8, 4, 16, 256>;

/// Lite Profile 的 Core Owner：8 Binding、16 Endpoint、16 Path、48 个 Class Slot、256 B Frame。
pub type LiteCoreNode<'a> = CoreNode<'a, 8, 16, 16, 48, 256>;

/// Full Profile 的 Core Owner：16 Binding、32 Endpoint、32 Path、96 个 Class Slot、512 B Frame。
pub type FullCoreNode<'a> = CoreNode<'a, 16, 32, 32, 96, 512>;

/// 返回 Nano 的 Q0/Q1/Q2/Q3=`4/4/4/4` 固定分区。
#[must_use]
pub const fn nano_queue_capacities() -> QueueCapacities {
    QueueCapacities {
        depths: [4, 4, 4, 4],
        offsets: [0, 4, 8, 12],
        total: 16,
    }
}

/// 返回 Lite 的 Q0/Q1/Q2/Q3=`8/8/16/16` 固定分区。
#[must_use]
pub const fn lite_queue_capacities() -> QueueCapacities {
    QueueCapacities {
        depths: [8, 8, 16, 16],
        offsets: [0, 8, 16, 32],
        total: 48,
    }
}

/// 返回 Full 的 Q0/Q1/Q2/Q3=`16/16/32/32` 固定分区。
#[must_use]
pub const fn full_queue_capacities() -> QueueCapacities {
    QueueCapacities {
        depths: [16, 16, 32, 32],
        offsets: [0, 16, 32, 64],
        total: 96,
    }
}

#[derive(Clone, Copy)]
struct BindingSlot {
    address: Option<NodeAddress>,
    generation: Option<BindingGeneration>,
    replay_high: u32,
    replay_bits: u64,
}

impl BindingSlot {
    const EMPTY: Self = Self {
        address: None,
        generation: None,
        replay_high: 0,
        replay_bits: 0,
    };
}

#[derive(Clone, Copy)]
struct EndpointSlot<'a> {
    generation: u32,
    service_id: Option<ServiceId>,
    handler: Option<&'a dyn EndpointHandler>,
}

impl EndpointSlot<'_> {
    const EMPTY: Self = Self {
        generation: 0,
        service_id: None,
        handler: None,
    };
}

#[derive(Clone, Copy)]
struct PathSlot {
    generation: u32,
    path: Option<StaticPath>,
}

impl PathSlot {
    const EMPTY: Self = Self {
        generation: 0,
        path: None,
    };
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum CoreTxState {
    Free,
    Queued,
    Retry,
    Waiting,
    WaitingCancel,
}

#[derive(Clone, Copy)]
struct CoreTxSlot<const FRAME_BYTES: usize> {
    generation: u32,
    state: CoreTxState,
    traffic_class: TrafficClass,
    origin_sequence: u32,
    path: Option<PathHandle>,
    deadline_us: u64,
    adapter_token: Option<TxToken>,
    frame_bytes: u16,
    frame: [u8; FRAME_BYTES],
}

impl<const FRAME_BYTES: usize> CoreTxSlot<FRAME_BYTES> {
    const EMPTY: Self = Self {
        generation: 0,
        state: CoreTxState::Free,
        traffic_class: TrafficClass::Q3,
        origin_sequence: 0,
        path: None,
        deadline_us: 0,
        adapter_token: None,
        frame_bytes: 0,
        frame: [0; FRAME_BYTES],
    };

    fn clear(&mut self) {
        let generation = self.generation;
        *self = Self {
            generation,
            ..Self::EMPTY
        };
    }
}

/// RUST-03 的固定容量最小静态通信 Owner。
///
/// `TX_SLOTS` 由 [`QueueCapacities`] 精确拆为四个独立分区；Frame/RX workspace 都在调用方
/// 持有的对象内，运行期不分配内存。
pub struct CoreNode<
    'a,
    const BINDINGS: usize,
    const ENDPOINTS: usize,
    const PATHS: usize,
    const TX_SLOTS: usize,
    const FRAME_BYTES: usize,
> {
    config: NodeConfig,
    lifecycle: Lifecycle,
    queues: QueueCapacities,
    bindings: [BindingSlot; BINDINGS],
    endpoints: [EndpointSlot<'a>; ENDPOINTS],
    paths: [PathSlot; PATHS],
    tx: [CoreTxSlot<FRAME_BYTES>; TX_SLOTS],
    queue_allocate_cursor: [u16; 4],
    schedule_cursor: usize,
    completion_cursor: usize,
    stop_cursor: usize,
    phase_cursor: usize,
    last_origin_sequence: u32,
    last_now_us: u64,
    callback_gate: CallbackGate,
    rx_workspace: [u8; FRAME_BYTES],
    stats: Stats,
}

impl<
    'a,
    const BINDINGS: usize,
    const ENDPOINTS: usize,
    const PATHS: usize,
    const TX_SLOTS: usize,
    const FRAME_BYTES: usize,
> CoreNode<'a, BINDINGS, ENDPOINTS, PATHS, TX_SLOTS, FRAME_BYTES>
{
    /// 建立仅含本机静态 Binding 的初始化对象。
    ///
    /// # Errors
    ///
    /// 实例、容量、地址、O0 产品边界或队列分区非法时返回 [`Error::Config`]。
    pub fn new(config: NodeConfig, queues: QueueCapacities) -> Result<Self> {
        if config.node_instance == 0
            || config.adapter_instance == 0
            || !config.trusted_o0_network
            || config.local_binding_generation.is_unbound()
            || BINDINGS == 0
            || ENDPOINTS == 0
            || PATHS == 0
            || TX_SLOTS == 0
            || FRAME_BYTES == 0
            || BINDINGS > u16::MAX as usize
            || ENDPOINTS > u16::MAX as usize
            || PATHS > u16::MAX as usize
            || FRAME_BYTES > u16::MAX as usize
            || queues.total as usize != TX_SLOTS
            || NodeAddress::new(config.local_address.get(), config.address_width).is_err()
        {
            return Err(Error::Config);
        }
        let callback_gate = CallbackGate::new(config.node_instance).map_err(|_| Error::Config)?;
        let mut bindings = [BindingSlot::EMPTY; BINDINGS];
        bindings[0] = BindingSlot {
            address: Some(config.local_address),
            generation: Some(config.local_binding_generation),
            replay_high: 0,
            replay_bits: 0,
        };
        Ok(Self {
            config,
            lifecycle: Lifecycle::Initialized,
            queues,
            bindings,
            endpoints: [EndpointSlot::EMPTY; ENDPOINTS],
            paths: [PathSlot::EMPTY; PATHS],
            tx: [CoreTxSlot::EMPTY; TX_SLOTS],
            queue_allocate_cursor: [0; 4],
            schedule_cursor: 0,
            completion_cursor: 0,
            stop_cursor: 0,
            phase_cursor: 0,
            last_origin_sequence: 0,
            last_now_us: 0,
            callback_gate,
            rx_workspace: [0; FRAME_BYTES],
            stats: Stats::default(),
        })
    }

    /// 返回当前生命周期。
    #[must_use]
    pub const fn lifecycle(&self) -> Lifecycle {
        self.lifecycle
    }

    /// 返回冻结统计快照。
    #[must_use]
    pub const fn stats(&self) -> Stats {
        self.stats
    }

    /// 增加一个不可在运行期变化的静态 Peer Binding。
    ///
    /// # Errors
    ///
    /// 生命周期非法、地址重复、Generation 为零或表满时返回错误。
    pub fn binding_add(
        &mut self,
        address: NodeAddress,
        generation: BindingGeneration,
    ) -> Result<()> {
        self.require_configurable()?;
        NodeAddress::new(address.get(), self.config.address_width)?;
        if generation.is_unbound()
            || self
                .bindings
                .iter()
                .any(|slot| slot.address == Some(address))
        {
            return Err(Error::Argument);
        }
        let slot = self
            .bindings
            .iter_mut()
            .find(|slot| slot.address.is_none())
            .ok_or(Error::NoSpace)?;
        *slot = BindingSlot {
            address: Some(address),
            generation: Some(generation),
            replay_high: 0,
            replay_bits: 0,
        };
        Ok(())
    }

    /// 注册一个唯一 Service Endpoint。
    ///
    /// # Errors
    ///
    /// 生命周期非法、Service 重复、容量满或 Handle 代际耗尽时返回错误。
    pub fn endpoint_add(
        &mut self,
        service_id: ServiceId,
        handler: &'a dyn EndpointHandler,
    ) -> Result<EndpointHandle> {
        self.require_configurable()?;
        if self
            .endpoints
            .iter()
            .any(|slot| slot.service_id == Some(service_id))
        {
            return Err(Error::State);
        }
        let mut exhausted = false;
        for (index, slot) in self.endpoints.iter_mut().enumerate() {
            if slot.service_id.is_some() {
                continue;
            }
            let Some(generation) = slot.generation.checked_add(1) else {
                exhausted = true;
                continue;
            };
            *slot = EndpointSlot {
                generation,
                service_id: Some(service_id),
                handler: Some(handler),
            };
            return Ok(EndpointHandle {
                node_instance: self.config.node_instance,
                slot: u16::try_from(index).map_err(|_| Error::Config)?,
                generation,
            });
        }
        if exhausted {
            Err(Error::Exhausted)
        } else {
            Err(Error::NoSpace)
        }
    }

    /// 移除精确 Endpoint；代际高水位保留。
    ///
    /// # Errors
    ///
    /// 生命周期或 Handle 不匹配时返回错误。
    pub fn endpoint_remove(&mut self, handle: EndpointHandle) -> Result<()> {
        self.require_configurable()?;
        let slot = self.endpoint_slot(handle)?;
        let generation = self.endpoints[slot].generation;
        self.endpoints[slot] = EndpointSlot {
            generation,
            ..EndpointSlot::EMPTY
        };
        Ok(())
    }

    /// 增加一个精确绑定目标与当前 Link 代际的静态 Path。
    ///
    /// # Errors
    ///
    /// 生命周期、Binding、Link、MTU 或容量不合法时返回错误。
    pub fn path_add<const LINKS: usize, const ADAPTER_TX: usize, const ADAPTER_RX: usize>(
        &mut self,
        path: StaticPath,
        adapter: &AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
    ) -> Result<PathHandle> {
        self.require_configurable()?;
        if adapter.adapter_instance() != self.config.adapter_instance
            || path.link.adapter_instance() != self.config.adapter_instance
        {
            return Err(Error::Argument);
        }
        let link_mtu = adapter.link_mtu(path.link)?;
        let base = c1_o0_h0_encoded_size(self.config.address_width, 0)?;
        if usize::from(path.path_frame_mtu) < base
            || usize::from(path.path_frame_mtu) > link_mtu
            || usize::from(path.path_frame_mtu) > FRAME_BYTES
            || !self.binding_matches(path.destination, path.destination_binding_generation)
            || path.destination == self.config.local_address
        {
            return Err(Error::Argument);
        }
        if self.paths.iter().any(|slot| {
            slot.path.is_some_and(|existing| {
                existing.destination == path.destination
                    && existing.destination_binding_generation
                        == path.destination_binding_generation
            })
        }) {
            return Err(Error::State);
        }
        let mut exhausted = false;
        for (index, slot) in self.paths.iter_mut().enumerate() {
            if slot.path.is_some() {
                continue;
            }
            let Some(generation) = slot.generation.checked_add(1) else {
                exhausted = true;
                continue;
            };
            *slot = PathSlot {
                generation,
                path: Some(path),
            };
            return Ok(PathHandle {
                node_instance: self.config.node_instance,
                slot: u16::try_from(index).map_err(|_| Error::Config)?,
                generation,
            });
        }
        if exhausted {
            Err(Error::Exhausted)
        } else {
            Err(Error::NoSpace)
        }
    }

    /// 移除未被 TX Slot 引用的精确静态 Path。
    ///
    /// # Errors
    ///
    /// 生命周期/Handle 不匹配或仍有活动引用时返回错误。
    pub fn path_remove(&mut self, handle: PathHandle) -> Result<()> {
        self.require_configurable()?;
        let slot = self.path_slot(handle)?;
        if self
            .tx
            .iter()
            .any(|tx| tx.state != CoreTxState::Free && tx.path == Some(handle))
        {
            return Err(Error::State);
        }
        let generation = self.paths[slot].generation;
        self.paths[slot] = PathSlot {
            generation,
            ..PathSlot::EMPTY
        };
        Ok(())
    }

    /// 打开 RX 接纳并进入运行态。
    ///
    /// # Errors
    ///
    /// 生命周期、Adapter 实例或并发 RX publication 不合法时返回错误。
    pub fn start<const LINKS: usize, const ADAPTER_TX: usize, const ADAPTER_RX: usize>(
        &mut self,
        now_us: u64,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
    ) -> Result<()> {
        if !matches!(
            self.lifecycle,
            Lifecycle::Initialized | Lifecycle::Quiescent
        ) || adapter.adapter_instance() != self.config.adapter_instance
        {
            return Err(Error::State);
        }
        if now_us < self.last_now_us {
            return Err(Error::Argument);
        }
        adapter.try_set_rx_enabled(true)?;
        self.last_now_us = now_us;
        self.lifecycle = Lifecycle::Running;
        Ok(())
    }

    /// 原子关闭新的 RX publication 并进入停止态。
    ///
    /// # Errors
    ///
    /// 生命周期非法或 RX publication 正在写入时返回错误，生命周期保持不变。
    pub fn stop<const LINKS: usize, const ADAPTER_TX: usize, const ADAPTER_RX: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
    ) -> Result<()> {
        if self.lifecycle != Lifecycle::Running
            || adapter.adapter_instance() != self.config.adapter_instance
        {
            return Err(Error::State);
        }
        adapter.try_set_rx_enabled(false)?;
        self.lifecycle = Lifecycle::Stopping;
        Ok(())
    }

    /// 将一帧 Best-Effort/One-Way/Data Copy 消息接纳到对应 Class 的固定分区。
    ///
    /// 所有检查均在分配 Sequence 前完成。容量或输入失败时，既有 Slot 和 Sequence 不变。
    ///
    /// # Errors
    ///
    /// 生命周期、静态 Binding/Path、MTU、Deadline、容量或 Sequence 非法时返回错误。
    pub fn publish<const LINKS: usize, const ADAPTER_TX: usize, const ADAPTER_RX: usize>(
        &mut self,
        target: Target,
        payload: &[u8],
        options: PublishOptions,
        adapter: &AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
    ) -> Result<PublishReceipt> {
        if self.lifecycle != Lifecycle::Running
            || self.callback_gate.is_active()
            || adapter.adapter_instance() != self.config.adapter_instance
        {
            return Err(Error::State);
        }
        if target.address == self.config.local_address
            || !self.binding_matches(target.address, target.binding_generation)
        {
            return Err(Error::NotFound);
        }
        if options.absolute_deadline_us != 0 && self.last_now_us >= options.absolute_deadline_us {
            return Err(Error::Timeout);
        }
        let (path_handle, path) = self.find_path(target)?;
        let link_mtu = adapter.link_mtu(path.link)?;
        let encoded_size = c1_o0_h0_encoded_size(self.config.address_width, payload.len())?;
        if encoded_size > FRAME_BYTES
            || encoded_size > usize::from(path.path_frame_mtu)
            || encoded_size > link_mtu
        {
            return Err(Error::NoSpace);
        }
        let sequence = self
            .last_origin_sequence
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let origin_sequence = OriginSequence::new(sequence)?;
        let (slot_index, slot_generation) = self.find_free_tx(options.traffic_class)?;
        let common = CommonHeader {
            contract: HeaderContract::C1,
            traffic_class: options.traffic_class,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::OneWay,
            payload_kind: PayloadKind::Data,
            origin_security: OriginSecurity::O0,
            hop_limit: options.hop_limit,
        };
        let wire_frame = C1Frame {
            common,
            source: self.config.local_address,
            destination: target.address,
            service_id: target.service_id,
            origin_sequence,
            payload,
        };
        let written = encode_c1_o0_h0(
            &wire_frame,
            self.config.address_width,
            HopProfile::H0,
            &mut self.tx[slot_index].frame,
        )?;
        self.tx[slot_index].generation = slot_generation;
        self.tx[slot_index].state = CoreTxState::Queued;
        self.tx[slot_index].traffic_class = options.traffic_class;
        self.tx[slot_index].origin_sequence = sequence;
        self.tx[slot_index].path = Some(path_handle);
        self.tx[slot_index].deadline_us = options.absolute_deadline_us;
        self.tx[slot_index].adapter_token = None;
        self.tx[slot_index].frame_bytes = u16::try_from(written).map_err(|_| Error::Config)?;
        let class = options.traffic_class as usize;
        let range = self.queues.range(options.traffic_class);
        self.queue_allocate_cursor[class] = u16::try_from(slot_index + 1 - range.start)
            .map_err(|_| Error::Config)?
            % self.queues.depths[class];
        self.last_origin_sequence = sequence;
        self.stats.tx_admitted = self.stats.tx_admitted.saturating_add(1);
        Ok(PublishReceipt { origin_sequence })
    }

    /// 有界推进 RX、completion/cancel 和按 `6:3:2:1` 调度的普通 TX。
    ///
    /// # Errors
    ///
    /// 时间倒退、预算为零、生命周期非法或底层 Token/Driver 事实冲突时返回错误。
    pub fn step<
        D: TxDriver,
        const LINKS: usize,
        const ADAPTER_TX: usize,
        const ADAPTER_RX: usize,
    >(
        &mut self,
        now_us: u64,
        budget: u16,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
        driver: &mut D,
    ) -> Result<StepResult> {
        if budget == 0
            || !matches!(self.lifecycle, Lifecycle::Running | Lifecycle::Stopping)
            || adapter.adapter_instance() != self.config.adapter_instance
        {
            return Err(Error::State);
        }
        if now_us < self.last_now_us {
            return Err(Error::Argument);
        }
        self.last_now_us = now_us;
        let mut work_done = 0_u16;
        while work_done < budget {
            let progressed = if self.lifecycle == Lifecycle::Stopping {
                self.advance_stopping(adapter, driver)?
            } else {
                self.advance_running(now_us, adapter, driver)?
            };
            if !progressed {
                break;
            }
            work_done += 1;
        }
        if work_done < budget
            && self.lifecycle == Lifecycle::Stopping
            && !self.tx.iter().any(|slot| slot.state != CoreTxState::Free)
        {
            match adapter.rx_claim(&mut self.rx_workspace) {
                Err(Error::NotFound) => self.lifecycle = Lifecycle::Quiescent,
                Ok(view) => {
                    adapter.rx_retire(view.token)?;
                    work_done = work_done.saturating_add(1);
                }
                Err(error) => return Err(error),
            }
        }
        Ok(StepResult {
            work_done,
            more_work: self.tx.iter().any(|slot| slot.state != CoreTxState::Free)
                || work_done == budget,
            lifecycle: self.lifecycle,
        })
    }

    fn require_configurable(&self) -> Result<()> {
        if !matches!(
            self.lifecycle,
            Lifecycle::Initialized | Lifecycle::Quiescent
        ) || self.callback_gate.is_active()
        {
            return Err(Error::State);
        }
        Ok(())
    }

    fn binding_matches(&self, address: NodeAddress, generation: BindingGeneration) -> bool {
        self.bindings
            .iter()
            .any(|slot| slot.address == Some(address) && slot.generation == Some(generation))
    }

    fn endpoint_slot(&self, handle: EndpointHandle) -> Result<usize> {
        if handle.node_instance != self.config.node_instance {
            return Err(Error::Argument);
        }
        let index = usize::from(handle.slot);
        let slot = self.endpoints.get(index).ok_or(Error::Argument)?;
        if slot.service_id.is_none() || slot.generation != handle.generation {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn path_slot(&self, handle: PathHandle) -> Result<usize> {
        if handle.node_instance != self.config.node_instance {
            return Err(Error::Argument);
        }
        let index = usize::from(handle.slot);
        let slot = self.paths.get(index).ok_or(Error::Argument)?;
        if slot.path.is_none() || slot.generation != handle.generation {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn find_path(&self, target: Target) -> Result<(PathHandle, StaticPath)> {
        for (index, slot) in self.paths.iter().enumerate() {
            let Some(path) = slot.path else {
                continue;
            };
            if path.destination == target.address
                && path.destination_binding_generation == target.binding_generation
            {
                return Ok((
                    PathHandle {
                        node_instance: self.config.node_instance,
                        slot: u16::try_from(index).map_err(|_| Error::Config)?,
                        generation: slot.generation,
                    },
                    path,
                ));
            }
        }
        Err(Error::NotFound)
    }

    fn find_free_tx(&self, traffic_class: TrafficClass) -> Result<(usize, u32)> {
        let class = traffic_class as usize;
        let range = self.queues.range(traffic_class);
        let start = usize::from(self.queue_allocate_cursor[class]);
        let mut exhausted = false;
        for offset in 0..range.len() {
            let index = range.start + ((start + offset) % range.len());
            let slot = self.tx[index];
            if slot.state != CoreTxState::Free {
                continue;
            }
            if let Some(generation) = slot.generation.checked_add(1) {
                return Ok((index, generation));
            }
            exhausted = true;
        }
        if exhausted
            && !range
                .clone()
                .any(|index| self.tx[index].state != CoreTxState::Free)
        {
            Err(Error::Exhausted)
        } else {
            Err(Error::NoSpace)
        }
    }

    fn advance_running<
        D: TxDriver,
        const LINKS: usize,
        const ADAPTER_TX: usize,
        const ADAPTER_RX: usize,
    >(
        &mut self,
        now_us: u64,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
        driver: &mut D,
    ) -> Result<bool> {
        for offset in 0..3 {
            let phase = (self.phase_cursor + offset) % 3;
            let progressed = match phase {
                0 => self.advance_completion(now_us, false, adapter, driver)?,
                1 => self.advance_rx(adapter)?,
                _ => self.advance_tx(now_us, adapter, driver)?,
            };
            if progressed {
                self.phase_cursor = (phase + 1) % 3;
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn advance_tx<
        D: TxDriver,
        const LINKS: usize,
        const ADAPTER_TX: usize,
        const ADAPTER_RX: usize,
    >(
        &mut self,
        now_us: u64,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
        driver: &mut D,
    ) -> Result<bool> {
        for schedule_offset in 0..TX_SCHEDULE.len() {
            let schedule_index = (self.schedule_cursor + schedule_offset) % TX_SCHEDULE.len();
            let class = TX_SCHEDULE[schedule_index];
            let Some(index) = self.oldest_ready(class) else {
                continue;
            };
            let next_schedule = (schedule_index + 1) % TX_SCHEDULE.len();
            if self.tx[index].deadline_us != 0 && now_us >= self.tx[index].deadline_us {
                self.schedule_cursor = next_schedule;
                self.cancel_or_drop(index, adapter, driver)?;
                return Ok(true);
            }
            let path_handle = self.tx[index].path.ok_or(Error::State)?;
            let path = self.paths[self.path_slot(path_handle)?]
                .path
                .ok_or(Error::State)?;
            if self.tx[index].state == CoreTxState::Queued {
                let token = match adapter
                    .tx_reserve(path.link, u16::try_from(index).map_err(|_| Error::Config)?)
                {
                    Ok(token) => token,
                    Err(Error::NoSpace) => return Ok(false),
                    Err(Error::NotFound | Error::State) => {
                        self.schedule_cursor = next_schedule;
                        self.stats.tx_failed = self.stats.tx_failed.saturating_add(1);
                        self.tx[index].clear();
                        return Ok(true);
                    }
                    Err(Error::Exhausted) => return self.enter_fault(Error::Exhausted),
                    Err(error) => return Err(error),
                };
                self.tx[index].adapter_token = Some(token);
            }
            let token = self.tx[index].adapter_token.ok_or(Error::State)?;
            let frame_bytes = usize::from(self.tx[index].frame_bytes);
            let view = match adapter.tx_submit(token, &self.tx[index].frame[..frame_bytes], driver)
            {
                Ok(view) => view,
                Err(Error::InDoubt) => return self.enter_fault(Error::InDoubt),
                Err(error) => return self.enter_fault(error),
            };
            self.schedule_cursor = next_schedule;
            match view.state {
                TxState::NotSubmitted => self.tx[index].state = CoreTxState::Retry,
                TxState::Submitted => self.tx[index].state = CoreTxState::Waiting,
                TxState::Completed | TxState::Cancelled => self.finish_tx(index, view, adapter)?,
                TxState::InDoubt => return self.enter_fault(Error::InDoubt),
                TxState::Free | TxState::Reserved | TxState::Submitting => {
                    return self.enter_fault(Error::State);
                }
            }
            return Ok(true);
        }
        Ok(false)
    }

    fn oldest_ready(&self, class: TrafficClass) -> Option<usize> {
        self.queues
            .range(class)
            .filter(|&index| {
                matches!(
                    self.tx[index].state,
                    CoreTxState::Queued | CoreTxState::Retry
                )
            })
            .min_by_key(|&index| self.tx[index].origin_sequence)
    }

    fn advance_completion<
        D: TxDriver,
        const LINKS: usize,
        const ADAPTER_TX: usize,
        const ADAPTER_RX: usize,
    >(
        &mut self,
        now_us: u64,
        stopping: bool,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
        driver: &mut D,
    ) -> Result<bool> {
        for offset in 0..TX_SLOTS {
            let index = (self.completion_cursor + offset) % TX_SLOTS;
            if matches!(
                self.tx[index].state,
                CoreTxState::Free | CoreTxState::Queued
            ) {
                continue;
            }
            let token = self.tx[index].adapter_token.ok_or(Error::State)?;
            let view = match adapter.tx_view(token) {
                Ok(view) => view,
                Err(Error::InDoubt) => return self.enter_fault(Error::InDoubt),
                Err(error) => return self.enter_fault(error),
            };
            if matches!(view.state, TxState::Completed | TxState::Cancelled) {
                self.completion_cursor = (index + 1) % TX_SLOTS;
                self.finish_tx(index, view, adapter)?;
                return Ok(true);
            }
            if view.state == TxState::InDoubt {
                return self.enter_fault(Error::InDoubt);
            }
            let expired = self.tx[index].deadline_us != 0 && now_us >= self.tx[index].deadline_us;
            if (stopping || expired) && self.tx[index].state != CoreTxState::WaitingCancel {
                let cancelled = match adapter.tx_cancel(token, driver) {
                    Ok(view) => view,
                    Err(Error::InDoubt) => return self.enter_fault(Error::InDoubt),
                    Err(error) => return self.enter_fault(error),
                };
                self.completion_cursor = (index + 1) % TX_SLOTS;
                if matches!(cancelled.state, TxState::Completed | TxState::Cancelled) {
                    self.finish_tx(index, cancelled, adapter)?;
                } else if cancelled.state == TxState::Submitted {
                    self.tx[index].state = CoreTxState::WaitingCancel;
                } else {
                    return self.enter_fault(Error::State);
                }
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn cancel_or_drop<
        D: TxDriver,
        const LINKS: usize,
        const ADAPTER_TX: usize,
        const ADAPTER_RX: usize,
    >(
        &mut self,
        index: usize,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
        driver: &mut D,
    ) -> Result<()> {
        let Some(token) = self.tx[index].adapter_token else {
            self.stats.tx_failed = self.stats.tx_failed.saturating_add(1);
            self.tx[index].clear();
            return Ok(());
        };
        let view = match adapter.tx_cancel(token, driver) {
            Ok(view) => view,
            Err(Error::InDoubt) => return self.enter_fault(Error::InDoubt),
            Err(error) => return self.enter_fault(error),
        };
        match view.state {
            TxState::Cancelled | TxState::Completed => self.finish_tx(index, view, adapter),
            TxState::Submitted => {
                self.tx[index].state = CoreTxState::WaitingCancel;
                Ok(())
            }
            _ => self.enter_fault(Error::State),
        }
    }

    fn finish_tx<const LINKS: usize, const ADAPTER_TX: usize, const ADAPTER_RX: usize>(
        &mut self,
        index: usize,
        view: ucn_adapter::TxView,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
    ) -> Result<()> {
        let token = self.tx[index].adapter_token.ok_or(Error::State)?;
        if let Err(error) = adapter.tx_retire(token) {
            return self.enter_fault(error);
        }
        if view.state == TxState::Completed && view.terminal == Some(TerminalOutcome::Success) {
            self.stats.tx_completed = self.stats.tx_completed.saturating_add(1);
        } else {
            self.stats.tx_failed = self.stats.tx_failed.saturating_add(1);
        }
        self.tx[index].clear();
        Ok(())
    }

    fn advance_rx<const LINKS: usize, const ADAPTER_TX: usize, const ADAPTER_RX: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
    ) -> Result<bool> {
        let view = match adapter.rx_claim(&mut self.rx_workspace) {
            Ok(view) => view,
            Err(Error::NotFound) => return Ok(false),
            Err(error) => return Err(error),
        };
        self.stats.rx_claimed = self.stats.rx_claimed.saturating_add(1);
        let result = self.deliver_claimed(view);
        if let Err(error) = adapter.rx_retire(view.token) {
            return self.enter_fault(error);
        }
        result.map(|()| true)
    }

    fn deliver_claimed(&mut self, view: RxView) -> Result<()> {
        let input = &self.rx_workspace[..view.frame_bytes];
        let frame = match decode_c1_o0_h0(input, self.config.address_width, HopProfile::H0) {
            Ok(frame) => frame,
            Err(Error::Malformed | Error::Argument | Error::Unsupported) => {
                self.stats.malformed = self.stats.malformed.saturating_add(1);
                self.stats.rx_dropped = self.stats.rx_dropped.saturating_add(1);
                return Ok(());
            }
            Err(error) => return Err(error),
        };
        if frame.destination != self.config.local_address
            || frame.common.delivery != DeliveryGuarantee::BestEffort
            || frame.common.interaction != InteractionRole::OneWay
            || frame.common.payload_kind != PayloadKind::Data
            || frame.common.origin_security != OriginSecurity::O0
        {
            self.stats.rx_dropped = self.stats.rx_dropped.saturating_add(1);
            return Ok(());
        }
        let Some(binding_index) = self
            .bindings
            .iter()
            .position(|slot| slot.address == Some(frame.source))
        else {
            self.stats.rx_dropped = self.stats.rx_dropped.saturating_add(1);
            return Ok(());
        };
        let Some(endpoint_index) = self
            .endpoints
            .iter()
            .position(|slot| slot.service_id == Some(frame.service_id))
        else {
            self.stats.rx_dropped = self.stats.rx_dropped.saturating_add(1);
            return Ok(());
        };
        match Self::consume_replay(
            &mut self.bindings[binding_index],
            frame.origin_sequence.get(),
        ) {
            Ok(()) => {}
            Err(Error::Replay) => {
                self.stats.replay_rejected = self.stats.replay_rejected.saturating_add(1);
                self.stats.rx_dropped = self.stats.rx_dropped.saturating_add(1);
                return Ok(());
            }
            Err(error) => return self.enter_fault(error),
        }
        // `operation_generation` describes the callback contract version, not the invocation
        // count. Invocation freshness is owned by CallbackGate's u32 no-wrap lease generation;
        // duplicating it here with u16 would fault a healthy long-running node after 65,535 RX.
        let claim = CallbackClaim::new(
            self.config.node_instance,
            view.token.generation(),
            1,
            CALLBACK_KIND_ENDPOINT,
        )
        .inspect_err(|_error| {
            self.lifecycle = Lifecycle::Fault;
        })?;
        let lease = self.callback_gate.try_enter(claim).inspect_err(|_error| {
            self.lifecycle = Lifecycle::Fault;
        })?;
        let Some(handler) = self.endpoints[endpoint_index].handler else {
            return self.enter_fault(Error::State);
        };
        let source_binding_generation = self.bindings[binding_index]
            .generation
            .ok_or(Error::State)
            .inspect_err(|_error| {
                self.lifecycle = Lifecycle::Fault;
            })?;
        let disposition = handler.receive(EndpointMessage {
            source: frame.source,
            source_binding_generation,
            service_id: frame.service_id,
            traffic_class: frame.common.traffic_class,
            hop_limit: frame.common.hop_limit,
            payload: frame.payload,
            receive_timestamp_us: view.meta.timestamp_us,
        });
        self.callback_gate.leave(lease).inspect_err(|_error| {
            self.lifecycle = Lifecycle::Fault;
        })?;
        match disposition {
            EndpointDisposition::Accept => {
                self.stats.rx_delivered = self.stats.rx_delivered.saturating_add(1);
            }
            EndpointDisposition::Drop => {
                self.stats.rx_dropped = self.stats.rx_dropped.saturating_add(1);
            }
        }
        Ok(())
    }

    fn consume_replay(slot: &mut BindingSlot, sequence: u32) -> Result<()> {
        if sequence == 0 {
            return Err(Error::Malformed);
        }
        if slot.replay_high == 0 {
            slot.replay_high = sequence;
            slot.replay_bits = 1;
            return Ok(());
        }
        if sequence > slot.replay_high {
            let shift = sequence - slot.replay_high;
            slot.replay_bits = if shift >= 64 {
                1
            } else {
                (slot.replay_bits << shift) | 1
            };
            slot.replay_high = sequence;
            return Ok(());
        }
        let distance = slot.replay_high - sequence;
        if distance >= 64 {
            return Err(Error::Replay);
        }
        let mask = 1_u64 << distance;
        if slot.replay_bits & mask != 0 {
            return Err(Error::Replay);
        }
        slot.replay_bits |= mask;
        Ok(())
    }

    fn advance_stopping<
        D: TxDriver,
        const LINKS: usize,
        const ADAPTER_TX: usize,
        const ADAPTER_RX: usize,
    >(
        &mut self,
        adapter: &mut AdapterOwner<'_, LINKS, ADAPTER_TX, ADAPTER_RX, FRAME_BYTES>,
        driver: &mut D,
    ) -> Result<bool> {
        if self.advance_completion(self.last_now_us, true, adapter, driver)? {
            return Ok(true);
        }
        for offset in 0..TX_SLOTS {
            let index = (self.stop_cursor + offset) % TX_SLOTS;
            if self.tx[index].state == CoreTxState::Free {
                continue;
            }
            self.stop_cursor = (index + 1) % TX_SLOTS;
            match self.tx[index].state {
                CoreTxState::Queued | CoreTxState::Retry => {
                    self.cancel_or_drop(index, adapter, driver)?;
                    return Ok(true);
                }
                CoreTxState::Waiting | CoreTxState::WaitingCancel | CoreTxState::Free => {}
            }
        }
        match adapter.rx_claim(&mut self.rx_workspace) {
            Ok(view) => {
                adapter.rx_retire(view.token)?;
                Ok(true)
            }
            Err(Error::NotFound) => Ok(false),
            Err(error) => Err(error),
        }
    }

    fn enter_fault<T>(&mut self, error: Error) -> Result<T> {
        self.lifecycle = Lifecycle::Fault;
        Err(error)
    }
}

#[cfg(test)]
mod tests {
    use super::{BindingSlot, CoreNode};
    use ucn_types::Error;

    #[test]
    fn replay_window_accepts_unseen_reordering_but_rejects_duplicates_and_stale_values() {
        let mut slot = BindingSlot::EMPTY;
        assert_eq!(
            CoreNode::<1, 1, 1, 4, 16>::consume_replay(&mut slot, 5),
            Ok(())
        );
        assert_eq!(
            CoreNode::<1, 1, 1, 4, 16>::consume_replay(&mut slot, 3),
            Ok(())
        );
        assert_eq!(
            CoreNode::<1, 1, 1, 4, 16>::consume_replay(&mut slot, 3),
            Err(Error::Replay)
        );
        assert_eq!(
            CoreNode::<1, 1, 1, 4, 16>::consume_replay(&mut slot, 70),
            Ok(())
        );
        assert_eq!(
            CoreNode::<1, 1, 1, 4, 16>::consume_replay(&mut slot, 5),
            Err(Error::Replay)
        );
    }
}
