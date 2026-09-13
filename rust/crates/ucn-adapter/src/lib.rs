#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的固定容量 Adapter Token Owner 与 Driver/ISR 事实入口。
//!
//! [`AdapterOwner`] 只能通过 `&mut self` 改写协议状态。Driver 只持有共享的
//! [`AdapterEventIngress`]，因此可以同步或异步发布完成事实，却无法递归调用 Owner 控制 API。

use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU8, AtomicU32, Ordering};

use ucn_types::{Error, Result};

const LINK_UNUSED: u8 = 0;
const LINK_ACTIVE: u8 = 1;
const LINK_FENCED: u8 = 2;

const TX_LATCH_EMPTY: i32 = i32::MAX;
const TX_LATCH_ARMED: i32 = i32::MAX - 1;
const TX_LATCH_CONFLICT: i32 = i32::MAX - 2;

const RX_FREE: u8 = 0;
const RX_WRITING: u8 = 1;
const RX_READY: u8 = 2;
const RX_CLAIMED: u8 = 3;
const RX_EXHAUSTED: u8 = 4;

/// 一个已经打开的物理 Link 的精确代际 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LinkHandle {
    adapter_instance: u32,
    slot: u16,
    handle_generation: u16,
    link_instance_generation: u32,
}

impl LinkHandle {
    /// 返回所属 Adapter 实例。
    #[must_use]
    pub const fn adapter_instance(self) -> u32 {
        self.adapter_instance
    }

    /// 返回 Link 槽位。
    #[must_use]
    pub const fn slot(self) -> u16 {
        self.slot
    }

    /// 返回物理 Link 实例代际。
    #[must_use]
    pub const fn instance_generation(self) -> u32 {
        self.link_instance_generation
    }
}

/// 一次 TX reservation 的精确 Token。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TxToken {
    adapter_instance: u32,
    slot: u16,
    generation: u32,
    link_slot: u16,
    link_handle_generation: u16,
    link_instance_generation: u32,
}

impl TxToken {
    /// 返回固定 TX 槽位。
    #[must_use]
    pub const fn slot(self) -> u16 {
        self.slot
    }

    /// 返回不会回绕的 Token 代际。
    #[must_use]
    pub const fn generation(self) -> u32 {
        self.generation
    }
}

/// 一次 RX publication 的精确 Token。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RxToken {
    adapter_instance: u32,
    slot: u16,
    generation: u32,
    link_slot: u16,
    link_handle_generation: u16,
    link_instance_generation: u32,
}

impl RxToken {
    /// 返回固定 RX 槽位。
    #[must_use]
    pub const fn slot(self) -> u16 {
        self.slot
    }

    /// 返回不会回绕的 Token 代际。
    #[must_use]
    pub const fn generation(self) -> u32 {
        self.generation
    }
}

/// Driver 发布的精确 TX 终态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TerminalOutcome {
    /// Frame 已被 Driver 确认发送完成。
    Success,
    /// Driver 已确认失败且不会再产生发送副作用。
    Failure(Error),
}

impl TerminalOutcome {
    const fn code(self) -> Result<i32> {
        match self {
            Self::Success => Ok(0),
            Self::Failure(Error::InDoubt) => Err(Error::Argument),
            Self::Failure(error) => Ok(error.code()),
        }
    }

    const fn from_code(code: i32) -> Result<Self> {
        if code == 0 {
            return Ok(Self::Success);
        }
        match Error::from_code(code) {
            Ok(error) => Ok(Self::Failure(error)),
            Err(_) => Err(Error::Malformed),
        }
    }
}

/// Driver 的同步提交结论。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DriverSubmit {
    /// 明确没有把 Frame 提交给硬件；同一 Attempt 可稍后重试。
    NotSubmitted,
    /// 已提交，终态将通过 completion ingress 到达。
    Submitted,
    /// 在 `submit()` 返回前已经形成终态。
    Complete(TerminalOutcome),
    /// 无法证明硬件是否接受 Frame。
    Unknown,
}

/// Driver 的取消结论。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DriverCancel {
    /// Driver 证明 Frame 未发送并已取消。
    Cancelled,
    /// Driver 明确无法取消，原提交仍然有效。
    NotCancelled,
    /// 取消调用期间观察到精确终态。
    Complete(TerminalOutcome),
    /// 无法证明取消或发送副作用。
    Unknown,
}

/// TX Driver 最小 SPI。实现者不会获得 `&mut AdapterOwner`。
pub trait TxDriver {
    /// 尝试向指定 Link 提交一帧。
    fn submit(&mut self, link: LinkHandle, frame: &[u8], token: TxToken) -> DriverSubmit;

    /// 尝试取消一个已提交 Token。
    fn cancel(&mut self, token: TxToken) -> DriverCancel;
}

/// Adapter Owner 公开的 TX 生命周期状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TxState {
    /// 槽位未占用。
    Free,
    /// 已保留但尚未调用 Driver。
    Reserved,
    /// Driver 调用动态范围内的内部状态。
    Submitting,
    /// Driver 已接受，等待终态。
    Submitted,
    /// 已得到确定终态。
    Completed,
    /// Driver 明确没有接受；可使用同一 Token 重试。
    NotSubmitted,
    /// Driver 副作用无法证明；不得退休或复用。
    InDoubt,
    /// 已证明取消。
    Cancelled,
}

/// 一个 TX Token 的不可变 View。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TxView {
    /// 当前生命周期状态。
    pub state: TxState,
    /// 终态；仅 [`TxState::Completed`] 时为 `Some`。
    pub terminal: Option<TerminalOutcome>,
    /// Runtime/Core 传入的固定槽位关联。
    pub core_tx_slot: u16,
}

/// Driver RX 时间戳与来源事实。它们尚未经过 Wire/Security 认证。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RxMeta {
    /// Link Driver 捕获的本地单调时间。
    pub timestamp_us: u64,
    /// Driver/承载层提供的发送端判别值。
    pub sender_discriminator: u32,
}

/// 已复制到调用方缓冲区的一次 RX claim。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RxView {
    /// 精确退休 Token。
    pub token: RxToken,
    /// 实际复制的字节数。
    pub frame_bytes: usize,
    /// 尚未经过密码认证的 Driver 事实。
    pub meta: RxMeta,
    /// 接收所在 Link。
    pub link: LinkHandle,
}

struct LinkIngress {
    locked: AtomicBool,
    state: AtomicU8,
    handle_generation: AtomicU32,
    instance_generation: AtomicU32,
    mtu: AtomicU32,
}

impl LinkIngress {
    const fn new() -> Self {
        Self {
            locked: AtomicBool::new(false),
            state: AtomicU8::new(LINK_UNUSED),
            handle_generation: AtomicU32::new(0),
            instance_generation: AtomicU32::new(0),
            mtu: AtomicU32::new(0),
        }
    }

    fn lock(&self) -> Result<AtomicGuard<'_>> {
        AtomicGuard::acquire(&self.locked)
    }

    fn matches(&self, link: LinkHandle) -> bool {
        self.state.load(Ordering::Acquire) == LINK_ACTIVE
            && self.handle_generation.load(Ordering::Relaxed) == u32::from(link.handle_generation)
            && self.instance_generation.load(Ordering::Relaxed) == link.link_instance_generation
    }
}

struct TxLatch {
    locked: AtomicBool,
    generation: AtomicU32,
    link_slot: AtomicU32,
    link_handle_generation: AtomicU32,
    link_instance_generation: AtomicU32,
    outcome: AtomicI32,
}

impl TxLatch {
    const fn new() -> Self {
        Self {
            locked: AtomicBool::new(false),
            generation: AtomicU32::new(0),
            link_slot: AtomicU32::new(0),
            link_handle_generation: AtomicU32::new(0),
            link_instance_generation: AtomicU32::new(0),
            outcome: AtomicI32::new(TX_LATCH_EMPTY),
        }
    }

    fn lock(&self) -> Result<AtomicGuard<'_>> {
        AtomicGuard::acquire(&self.locked)
    }

    fn matches(&self, token: TxToken) -> bool {
        self.generation.load(Ordering::Relaxed) == token.generation
            && self.link_slot.load(Ordering::Relaxed) == u32::from(token.link_slot)
            && self.link_handle_generation.load(Ordering::Relaxed)
                == u32::from(token.link_handle_generation)
            && self.link_instance_generation.load(Ordering::Relaxed)
                == token.link_instance_generation
    }
}

struct RxIngress<const FRAME_BYTES: usize> {
    state: AtomicU8,
    generation: AtomicU32,
    link_slot: AtomicU32,
    link_handle_generation: AtomicU32,
    link_instance_generation: AtomicU32,
    frame_bytes: AtomicU32,
    timestamp_low: AtomicU32,
    timestamp_high: AtomicU32,
    sender_discriminator: AtomicU32,
    frame: [AtomicU8; FRAME_BYTES],
}

impl<const FRAME_BYTES: usize> RxIngress<FRAME_BYTES> {
    const fn new() -> Self {
        Self {
            state: AtomicU8::new(RX_FREE),
            generation: AtomicU32::new(0),
            link_slot: AtomicU32::new(0),
            link_handle_generation: AtomicU32::new(0),
            link_instance_generation: AtomicU32::new(0),
            frame_bytes: AtomicU32::new(0),
            timestamp_low: AtomicU32::new(0),
            timestamp_high: AtomicU32::new(0),
            sender_discriminator: AtomicU32::new(0),
            frame: [const { AtomicU8::new(0) }; FRAME_BYTES],
        }
    }
}

struct AtomicGuard<'a> {
    gate: &'a AtomicBool,
}

impl<'a> AtomicGuard<'a> {
    fn acquire(gate: &'a AtomicBool) -> Result<Self> {
        gate.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| Error::State)?;
        Ok(Self { gate })
    }
}

impl Drop for AtomicGuard<'_> {
    fn drop(&mut self) {
        self.gate.store(false, Ordering::Release);
    }
}

/// Driver/ISR 可共享的固定容量事实入口。
///
/// 它不拥有协议状态，不能调用 Adapter 控制 API。所有 publication 都是非阻塞的：并发门忙时
/// 立即返回 [`Error::State`]，容量满时返回 [`Error::NoSpace`]。
pub struct AdapterEventIngress<
    const LINKS: usize,
    const TX_SLOTS: usize,
    const RX_SLOTS: usize,
    const FRAME_BYTES: usize,
> {
    adapter_instance: u32,
    rx_enabled: AtomicBool,
    rx_allocating: AtomicBool,
    rx_cursor: AtomicU32,
    links: [LinkIngress; LINKS],
    tx: [TxLatch; TX_SLOTS],
    rx: [RxIngress<FRAME_BYTES>; RX_SLOTS],
}

impl<const LINKS: usize, const TX_SLOTS: usize, const RX_SLOTS: usize, const FRAME_BYTES: usize>
    AdapterEventIngress<LINKS, TX_SLOTS, RX_SLOTS, FRAME_BYTES>
{
    /// 构造调用方持有的静态 ingress。
    ///
    /// # Errors
    ///
    /// 实例为零、任一容量为零或容量超过 Token 索引宽度时返回 [`Error::Config`]。
    pub const fn new(adapter_instance: u32) -> Result<Self> {
        if adapter_instance == 0
            || LINKS == 0
            || TX_SLOTS == 0
            || RX_SLOTS == 0
            || FRAME_BYTES == 0
            || LINKS > u16::MAX as usize
            || TX_SLOTS > u16::MAX as usize
            || RX_SLOTS > u16::MAX as usize
            || FRAME_BYTES > u16::MAX as usize
        {
            return Err(Error::Config);
        }
        Ok(Self {
            adapter_instance,
            rx_enabled: AtomicBool::new(false),
            rx_allocating: AtomicBool::new(false),
            rx_cursor: AtomicU32::new(0),
            links: [const { LinkIngress::new() }; LINKS],
            tx: [const { TxLatch::new() }; TX_SLOTS],
            rx: [const { RxIngress::new() }; RX_SLOTS],
        })
    }

    /// Driver/ISR 发布一个精确 TX terminal completion。
    ///
    /// 完全相同的重复 completion 幂等成功；不同结果的重复 completion 将槽位标为冲突，Owner
    /// 随后进入 [`TxState::InDoubt`]。
    ///
    /// # Errors
    ///
    /// Token 不匹配、槽位未 armed、入口正忙或终态冲突时返回错误且不伪造成功。
    pub fn tx_complete(&self, token: TxToken, outcome: TerminalOutcome) -> Result<()> {
        self.validate_tx_token_shape(token)?;
        let latch = self
            .tx
            .get(usize::from(token.slot))
            .ok_or(Error::Argument)?;
        let _guard = latch.lock()?;
        if !latch.matches(token) {
            return Err(Error::NotFound);
        }
        let code = outcome.code()?;
        let current = latch.outcome.load(Ordering::Acquire);
        match current {
            TX_LATCH_ARMED => {
                latch.outcome.store(code, Ordering::Release);
                Ok(())
            }
            TX_LATCH_EMPTY => Err(Error::NotFound),
            TX_LATCH_CONFLICT => Err(Error::InDoubt),
            existing if existing == code => Ok(()),
            existing if existing == Error::InDoubt.code() => {
                latch.outcome.store(code, Ordering::Release);
                Ok(())
            }
            _ => {
                latch.outcome.store(TX_LATCH_CONFLICT, Ordering::Release);
                Err(Error::InDoubt)
            }
        }
    }

    /// Driver/ISR 原子发布一帧与其本地 Link Facts。
    ///
    /// # Errors
    ///
    /// Link 过期、RX 未启用、帧为空/超 MTU、入口并发忙、槽位满或代际耗尽时返回错误。
    pub fn rx_publish(&self, link: LinkHandle, frame: &[u8], meta: RxMeta) -> Result<RxToken> {
        self.validate_link_shape(link)?;
        if !self.rx_enabled.load(Ordering::Acquire) {
            return Err(Error::State);
        }
        let link_ingress = self
            .links
            .get(usize::from(link.slot))
            .ok_or(Error::Argument)?;
        if !link_ingress.matches(link) {
            return Err(Error::NotFound);
        }
        let mtu =
            usize::try_from(link_ingress.mtu.load(Ordering::Relaxed)).map_err(|_| Error::Config)?;
        if frame.is_empty() || frame.len() > mtu || frame.len() > FRAME_BYTES {
            return Err(Error::Argument);
        }

        let _allocation = AtomicGuard::acquire(&self.rx_allocating)?;
        let start = usize::try_from(self.rx_cursor.load(Ordering::Relaxed))
            .map_err(|_| Error::Config)?
            % RX_SLOTS;
        let mut generation_exhausted = false;
        for offset in 0..RX_SLOTS {
            let index = (start + offset) % RX_SLOTS;
            let slot = &self.rx[index];
            if slot
                .state
                .compare_exchange(RX_FREE, RX_WRITING, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
            {
                continue;
            }
            let current = slot.generation.load(Ordering::Relaxed);
            let Some(generation) = current.checked_add(1) else {
                slot.state.store(RX_EXHAUSTED, Ordering::Release);
                generation_exhausted = true;
                continue;
            };
            let Ok(index_u16) = u16::try_from(index) else {
                slot.state.store(RX_FREE, Ordering::Release);
                return Err(Error::Config);
            };
            slot.generation.store(generation, Ordering::Relaxed);
            slot.link_slot
                .store(u32::from(link.slot), Ordering::Relaxed);
            slot.link_handle_generation
                .store(u32::from(link.handle_generation), Ordering::Relaxed);
            slot.link_instance_generation
                .store(link.link_instance_generation, Ordering::Relaxed);
            slot.frame_bytes.store(
                u32::try_from(frame.len()).map_err(|_| Error::Config)?,
                Ordering::Relaxed,
            );
            let timestamp = meta.timestamp_us.to_le_bytes();
            slot.timestamp_low.store(
                u32::from_le_bytes([timestamp[0], timestamp[1], timestamp[2], timestamp[3]]),
                Ordering::Relaxed,
            );
            slot.timestamp_high.store(
                u32::from_le_bytes([timestamp[4], timestamp[5], timestamp[6], timestamp[7]]),
                Ordering::Relaxed,
            );
            slot.sender_discriminator
                .store(meta.sender_discriminator, Ordering::Relaxed);
            for (destination, source) in slot.frame.iter().zip(frame.iter().copied()) {
                destination.store(source, Ordering::Relaxed);
            }
            slot.state.store(RX_READY, Ordering::Release);
            self.rx_cursor.store(
                u32::try_from((index + 1) % RX_SLOTS).map_err(|_| Error::Config)?,
                Ordering::Relaxed,
            );
            return Ok(RxToken {
                adapter_instance: self.adapter_instance,
                slot: index_u16,
                generation,
                link_slot: link.slot,
                link_handle_generation: link.handle_generation,
                link_instance_generation: link.link_instance_generation,
            });
        }
        if generation_exhausted {
            Err(Error::Exhausted)
        } else {
            Err(Error::NoSpace)
        }
    }

    fn validate_link_shape(&self, link: LinkHandle) -> Result<()> {
        if link.adapter_instance != self.adapter_instance
            || link.handle_generation == 0
            || link.link_instance_generation == 0
            || usize::from(link.slot) >= LINKS
        {
            return Err(Error::Argument);
        }
        Ok(())
    }

    fn validate_tx_token_shape(&self, token: TxToken) -> Result<()> {
        if token.adapter_instance != self.adapter_instance
            || token.generation == 0
            || token.link_handle_generation == 0
            || token.link_instance_generation == 0
            || usize::from(token.slot) >= TX_SLOTS
            || usize::from(token.link_slot) >= LINKS
        {
            return Err(Error::Argument);
        }
        Ok(())
    }

    fn configure_link(&self, link: LinkHandle, mtu: usize, state: u8) -> Result<()> {
        self.validate_link_shape(link)?;
        let ingress = &self.links[usize::from(link.slot)];
        let _guard = ingress.lock()?;
        ingress.state.store(LINK_UNUSED, Ordering::Release);
        ingress
            .handle_generation
            .store(u32::from(link.handle_generation), Ordering::Relaxed);
        ingress
            .instance_generation
            .store(link.link_instance_generation, Ordering::Relaxed);
        ingress.mtu.store(
            u32::try_from(mtu).map_err(|_| Error::Config)?,
            Ordering::Relaxed,
        );
        ingress.state.store(state, Ordering::Release);
        Ok(())
    }

    fn arm_tx(&self, token: TxToken) -> Result<()> {
        self.validate_tx_token_shape(token)?;
        let latch = &self.tx[usize::from(token.slot)];
        let _guard = latch.lock()?;
        if latch.outcome.load(Ordering::Acquire) != TX_LATCH_EMPTY {
            return Err(Error::State);
        }
        latch.generation.store(token.generation, Ordering::Relaxed);
        latch
            .link_slot
            .store(u32::from(token.link_slot), Ordering::Relaxed);
        latch
            .link_handle_generation
            .store(u32::from(token.link_handle_generation), Ordering::Relaxed);
        latch
            .link_instance_generation
            .store(token.link_instance_generation, Ordering::Relaxed);
        latch.outcome.store(TX_LATCH_ARMED, Ordering::Release);
        Ok(())
    }

    fn tx_latched(&self, token: TxToken) -> Result<Option<TerminalOutcome>> {
        self.validate_tx_token_shape(token)?;
        let latch = &self.tx[usize::from(token.slot)];
        if !latch.matches(token) {
            return Err(Error::NotFound);
        }
        match latch.outcome.load(Ordering::Acquire) {
            TX_LATCH_ARMED => Ok(None),
            TX_LATCH_EMPTY => Err(Error::NotFound),
            TX_LATCH_CONFLICT => Err(Error::InDoubt),
            code => TerminalOutcome::from_code(code).map(Some),
        }
    }

    fn disarm_tx(&self, token: TxToken) -> Result<()> {
        self.validate_tx_token_shape(token)?;
        let latch = &self.tx[usize::from(token.slot)];
        let _guard = latch.lock()?;
        if !latch.matches(token) {
            return Err(Error::NotFound);
        }
        latch.outcome.store(TX_LATCH_EMPTY, Ordering::Release);
        Ok(())
    }

    fn seal_tx_after_link_invalidation(&self, token: TxToken) -> Result<TerminalOutcome> {
        self.validate_tx_token_shape(token)?;
        let latch = &self.tx[usize::from(token.slot)];
        if !latch.matches(token) {
            return Err(Error::NotFound);
        }
        loop {
            let current = latch.outcome.load(Ordering::Acquire);
            match current {
                TX_LATCH_EMPTY => return Err(Error::NotFound),
                TX_LATCH_ARMED | TX_LATCH_CONFLICT => {
                    if latch
                        .outcome
                        .compare_exchange(
                            current,
                            Error::InDoubt.code(),
                            Ordering::AcqRel,
                            Ordering::Acquire,
                        )
                        .is_ok()
                    {
                        return Ok(TerminalOutcome::Failure(Error::InDoubt));
                    }
                }
                code => return TerminalOutcome::from_code(code),
            }
        }
    }
}

#[derive(Clone, Copy)]
struct LinkSlot {
    handle_generation: u16,
    instance_generation: u32,
    mtu: usize,
    state: u8,
}

impl LinkSlot {
    const EMPTY: Self = Self {
        handle_generation: 0,
        instance_generation: 0,
        mtu: 0,
        state: LINK_UNUSED,
    };
}

#[derive(Clone, Copy)]
struct TxSlot {
    generation: u32,
    state: TxState,
    link: LinkHandle,
    request_slot: u16,
    terminal: Option<TerminalOutcome>,
}

impl TxSlot {
    const EMPTY: Self = Self {
        generation: 0,
        state: TxState::Free,
        link: LinkHandle {
            adapter_instance: 0,
            slot: 0,
            handle_generation: 0,
            link_instance_generation: 0,
        },
        request_slot: 0,
        terminal: None,
    };
}

/// 单写 Adapter Owner。大帧数据只存在调用方 ingress 中，本对象本身保持小型固定存储。
pub struct AdapterOwner<
    'a,
    const LINKS: usize,
    const TX_SLOTS: usize,
    const RX_SLOTS: usize,
    const FRAME_BYTES: usize,
> {
    adapter_instance: u32,
    ingress: &'a AdapterEventIngress<LINKS, TX_SLOTS, RX_SLOTS, FRAME_BYTES>,
    links: [LinkSlot; LINKS],
    tx: [TxSlot; TX_SLOTS],
    tx_cursor: usize,
    rx_claim_cursor: usize,
}

impl<'a, const LINKS: usize, const TX_SLOTS: usize, const RX_SLOTS: usize, const FRAME_BYTES: usize>
    AdapterOwner<'a, LINKS, TX_SLOTS, RX_SLOTS, FRAME_BYTES>
{
    /// 返回本 Owner 精确绑定的 Adapter 实例。
    #[must_use]
    pub const fn adapter_instance(&self) -> u32 {
        self.adapter_instance
    }

    /// 绑定一个调用方持有的 ingress；不会复制大帧存储。
    ///
    /// # Errors
    ///
    /// ingress 的固定容量或实例不合法时返回 [`Error::Config`]。
    pub const fn new(
        adapter_instance: u32,
        ingress: &'a AdapterEventIngress<LINKS, TX_SLOTS, RX_SLOTS, FRAME_BYTES>,
    ) -> Result<Self> {
        if adapter_instance == 0
            || adapter_instance != ingress.adapter_instance
            || LINKS == 0
            || TX_SLOTS == 0
            || RX_SLOTS == 0
            || FRAME_BYTES == 0
        {
            return Err(Error::Config);
        }
        Ok(Self {
            adapter_instance,
            ingress,
            links: [LinkSlot::EMPTY; LINKS],
            tx: [TxSlot::EMPTY; TX_SLOTS],
            tx_cursor: 0,
            rx_claim_cursor: 0,
        })
    }

    /// 首次打开一个 Link。
    ///
    /// # Errors
    ///
    /// 槽位非法/已占用、代际为零或 MTU 超出静态帧容量时返回错误。
    pub fn open_link(
        &mut self,
        link_slot: usize,
        instance_generation: u32,
        mtu: usize,
    ) -> Result<LinkHandle> {
        let slot = self.links.get(link_slot).ok_or(Error::Argument)?;
        if slot.state != LINK_UNUSED || instance_generation == 0 || mtu == 0 || mtu > FRAME_BYTES {
            return Err(Error::Argument);
        }
        let handle = LinkHandle {
            adapter_instance: self.adapter_instance,
            slot: u16::try_from(link_slot).map_err(|_| Error::Config)?,
            handle_generation: 1,
            link_instance_generation: instance_generation,
        };
        self.ingress.configure_link(handle, mtu, LINK_ACTIVE)?;
        self.links[link_slot] = LinkSlot {
            handle_generation: 1,
            instance_generation,
            mtu,
            state: LINK_ACTIVE,
        };
        Ok(handle)
    }

    /// 取得当前 Link Handle。
    ///
    /// # Errors
    ///
    /// 槽位不存在或尚未打开时返回错误。
    pub fn link_handle(&self, link_slot: usize) -> Result<LinkHandle> {
        let slot = self.links.get(link_slot).ok_or(Error::Argument)?;
        if slot.state == LINK_UNUSED {
            return Err(Error::NotFound);
        }
        Ok(LinkHandle {
            adapter_instance: self.adapter_instance,
            slot: u16::try_from(link_slot).map_err(|_| Error::Config)?,
            handle_generation: slot.handle_generation,
            link_instance_generation: slot.instance_generation,
        })
    }

    /// 读取精确 Link Handle 当前的 Frame MTU。
    ///
    /// # Errors
    ///
    /// Handle 过期、槽位非法或 Link 已被围栏时返回错误。
    pub fn link_mtu(&self, link: LinkHandle) -> Result<usize> {
        self.validate_live_link(link)?;
        Ok(self.links[usize::from(link.slot)].mtu)
    }

    /// 在与 RX publication 互斥的短临界区内切换 RX 接纳状态。
    ///
    /// 停止路径使用本入口可证明：成功返回后，没有更早观察到 `enabled=true` 的 publication
    /// 仍处于写入中。并发 publication 已先取得门时，本调用立即返回而不等待。
    ///
    /// # Errors
    ///
    /// RX publication 正在分配槽位时返回 [`Error::State`]。
    pub fn try_set_rx_enabled(&mut self, enabled: bool) -> Result<()> {
        let _allocation = AtomicGuard::acquire(&self.ingress.rx_allocating)?;
        self.ingress.rx_enabled.store(enabled, Ordering::Release);
        Ok(())
    }

    /// 预留一个精确 TX Token；只写一个空槽。
    ///
    /// # Errors
    ///
    /// Link Handle 过期/被围栏、容量满或槽位代际耗尽时返回错误。
    pub fn tx_reserve(&mut self, link: LinkHandle, core_tx_slot: u16) -> Result<TxToken> {
        self.validate_live_link(link)?;
        for offset in 0..TX_SLOTS {
            let index = (self.tx_cursor + offset) % TX_SLOTS;
            let slot = self.tx[index];
            if slot.state != TxState::Free {
                continue;
            }
            let Some(generation) = slot.generation.checked_add(1) else {
                continue;
            };
            let token = TxToken {
                adapter_instance: self.adapter_instance,
                slot: u16::try_from(index).map_err(|_| Error::Config)?,
                generation,
                link_slot: link.slot,
                link_handle_generation: link.handle_generation,
                link_instance_generation: link.link_instance_generation,
            };
            self.ingress.arm_tx(token)?;
            self.tx[index] = TxSlot {
                generation,
                state: TxState::Reserved,
                link,
                request_slot: core_tx_slot,
                terminal: None,
            };
            self.tx_cursor = (index + 1) % TX_SLOTS;
            return Ok(token);
        }
        if self
            .tx
            .iter()
            .any(|slot| slot.state == TxState::Free && slot.generation == u32::MAX)
        {
            return Err(Error::Exhausted);
        }
        Err(Error::NoSpace)
    }

    /// 提交或重试一个 `RESERVED/NOT_SUBMITTED` Token。
    ///
    /// 同步早到 completion 会和 Driver 返回值合并；矛盾事实一律进入 `IN_DOUBT` 并围栏 Link。
    ///
    /// # Errors
    ///
    /// Token/状态/帧长度非法，或结果进入不确定态时返回错误。
    pub fn tx_submit<D: TxDriver>(
        &mut self,
        token: TxToken,
        frame: &[u8],
        driver: &mut D,
    ) -> Result<TxView> {
        let index = self.validate_tx_token(token)?;
        let slot = self.tx[index];
        if !matches!(slot.state, TxState::Reserved | TxState::NotSubmitted) {
            return Err(Error::State);
        }
        self.validate_live_link(slot.link)?;
        if frame.is_empty() || frame.len() > self.links[usize::from(token.link_slot)].mtu {
            return Err(Error::Argument);
        }
        self.tx[index].state = TxState::Submitting;
        let result = driver.submit(slot.link, frame, token);
        self.merge_submit_result(index, token, result)?;
        self.tx_view(token)
    }

    /// 合并异步 completion 并取得 TX View。
    ///
    /// # Errors
    ///
    /// Token 过期或 completion 冲突时返回错误。
    pub fn tx_view(&mut self, token: TxToken) -> Result<TxView> {
        let index = self.validate_tx_token(token)?;
        self.reconcile_latch(index, token)?;
        Ok(self.view_at(index))
    }

    /// 取消尚未形成终态的 TX Token。
    ///
    /// # Errors
    ///
    /// 状态非法、Driver 事实冲突或副作用不确定时返回错误。
    pub fn tx_cancel<D: TxDriver>(&mut self, token: TxToken, driver: &mut D) -> Result<TxView> {
        let index = self.validate_tx_token(token)?;
        self.reconcile_latch(index, token)?;
        match self.tx[index].state {
            TxState::Reserved | TxState::NotSubmitted => {
                self.tx[index].state = TxState::Cancelled;
                Ok(self.view_at(index))
            }
            TxState::Submitted => {
                let result = driver.cancel(token);
                self.merge_cancel_result(index, token, result)?;
                Ok(self.view_at(index))
            }
            TxState::Cancelled => Ok(self.view_at(index)),
            TxState::Completed | TxState::Free | TxState::Submitting | TxState::InDoubt => {
                Err(Error::State)
            }
        }
    }

    /// 退休已经确定完成、确定取消或确定未提交的 Token，并保留代际高水位。
    ///
    /// # Errors
    ///
    /// Token 过期、仍在飞行或处于不确定态时返回错误。
    pub fn tx_retire(&mut self, token: TxToken) -> Result<()> {
        let index = self.validate_tx_token(token)?;
        self.reconcile_latch(index, token)?;
        if !matches!(
            self.tx[index].state,
            TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
        ) {
            return Err(Error::State);
        }
        self.ingress.disarm_tx(token)?;
        let generation = self.tx[index].generation;
        self.tx[index] = TxSlot {
            generation,
            ..TxSlot::EMPTY
        };
        Ok(())
    }

    /// 领取下一帧 Ready RX，并原子复制到调用方缓冲区。
    ///
    /// # Errors
    ///
    /// 无 Ready 帧返回 [`Error::NotFound`]；输出不足返回 [`Error::NoSpace`] 且输出与槽位均不变；
    /// 过期 Link 事实被安全丢弃并返回 [`Error::NotFound`]。
    pub fn rx_claim(&mut self, output: &mut [u8]) -> Result<RxView> {
        for offset in 0..RX_SLOTS {
            let index = (self.rx_claim_cursor + offset) % RX_SLOTS;
            let slot = &self.ingress.rx[index];
            if slot.state.load(Ordering::Acquire) != RX_READY {
                continue;
            }
            let frame_bytes = usize::try_from(slot.frame_bytes.load(Ordering::Relaxed))
                .map_err(|_| Error::Malformed)?;
            if frame_bytes == 0 || frame_bytes > FRAME_BYTES {
                slot.state.store(RX_EXHAUSTED, Ordering::Release);
                return Err(Error::Malformed);
            }
            if output.len() < frame_bytes {
                return Err(Error::NoSpace);
            }
            if slot
                .state
                .compare_exchange(RX_READY, RX_CLAIMED, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
            {
                continue;
            }
            let token = self.rx_token_at(index)?;
            let link = LinkHandle {
                adapter_instance: self.adapter_instance,
                slot: token.link_slot,
                handle_generation: token.link_handle_generation,
                link_instance_generation: token.link_instance_generation,
            };
            if self.validate_live_link(link).is_err() {
                slot.state.store(RX_FREE, Ordering::Release);
                return Err(Error::NotFound);
            }
            for (destination, source) in output[..frame_bytes].iter_mut().zip(slot.frame.iter()) {
                *destination = source.load(Ordering::Relaxed);
            }
            let low = slot.timestamp_low.load(Ordering::Relaxed).to_le_bytes();
            let high = slot.timestamp_high.load(Ordering::Relaxed).to_le_bytes();
            let timestamp_us = u64::from_le_bytes([
                low[0], low[1], low[2], low[3], high[0], high[1], high[2], high[3],
            ]);
            self.rx_claim_cursor = (index + 1) % RX_SLOTS;
            return Ok(RxView {
                token,
                frame_bytes,
                meta: RxMeta {
                    timestamp_us,
                    sender_discriminator: slot.sender_discriminator.load(Ordering::Relaxed),
                },
                link,
            });
        }
        Err(Error::NotFound)
    }

    /// 精确退休一个已经 claim 的 RX Token。
    ///
    /// # Errors
    ///
    /// Token 过期、槽位不匹配或尚未 claim 时返回错误。
    pub fn rx_retire(&mut self, token: RxToken) -> Result<()> {
        self.validate_rx_token(token)?;
        let slot = &self.ingress.rx[usize::from(token.slot)];
        slot.state
            .compare_exchange(RX_CLAIMED, RX_FREE, Ordering::Release, Ordering::Relaxed)
            .map_err(|_| Error::State)?;
        Ok(())
    }

    /// 用严格更大的物理实例代际重新打开 Link。
    ///
    /// 旧的已提交 TX 进入 `IN_DOUBT`，未提交 TX 进入 `CANCELLED`；旧 Ready RX 不再交付。
    ///
    /// # Errors
    ///
    /// Handle 过期、代际未增加、Handle 代际耗尽或仍有已 claim RX 时返回错误且不改状态。
    pub fn reopen_link(
        &mut self,
        current: LinkHandle,
        new_instance_generation: u32,
        mtu: usize,
    ) -> Result<LinkHandle> {
        self.validate_current_link(current)?;
        if new_instance_generation <= current.link_instance_generation
            || mtu == 0
            || mtu > FRAME_BYTES
        {
            return Err(Error::Argument);
        }
        let Some(new_handle_generation) = current.handle_generation.checked_add(1) else {
            return Err(Error::Exhausted);
        };
        for slot in &self.ingress.rx {
            if slot.state.load(Ordering::Acquire) == RX_CLAIMED
                && slot.link_slot.load(Ordering::Relaxed) == u32::from(current.slot)
                && slot.link_handle_generation.load(Ordering::Relaxed)
                    == u32::from(current.handle_generation)
            {
                return Err(Error::State);
            }
        }
        let next = LinkHandle {
            adapter_instance: self.adapter_instance,
            slot: current.slot,
            handle_generation: new_handle_generation,
            link_instance_generation: new_instance_generation,
        };
        self.ingress.configure_link(next, mtu, LINK_ACTIVE)?;

        for index in 0..TX_SLOTS {
            let slot = self.tx[index];
            if slot.state == TxState::Free || slot.link != current {
                continue;
            }
            match slot.state {
                TxState::Reserved | TxState::NotSubmitted => {
                    self.tx[index].state = TxState::Cancelled;
                }
                TxState::Submitting | TxState::Submitted | TxState::InDoubt => {
                    let outcome = self.ingress.seal_tx_after_link_invalidation(TxToken {
                        adapter_instance: self.adapter_instance,
                        slot: u16::try_from(index).map_err(|_| Error::Config)?,
                        generation: slot.generation,
                        link_slot: current.slot,
                        link_handle_generation: current.handle_generation,
                        link_instance_generation: current.link_instance_generation,
                    })?;
                    self.set_completed(index, outcome);
                }
                _ => {}
            }
        }
        for slot in &self.ingress.rx {
            if slot.state.load(Ordering::Acquire) == RX_READY
                && slot.link_slot.load(Ordering::Relaxed) == u32::from(current.slot)
                && slot.link_handle_generation.load(Ordering::Relaxed)
                    == u32::from(current.handle_generation)
            {
                let _ = slot.state.compare_exchange(
                    RX_READY,
                    RX_FREE,
                    Ordering::Release,
                    Ordering::Relaxed,
                );
            }
        }
        self.links[usize::from(current.slot)] = LinkSlot {
            handle_generation: new_handle_generation,
            instance_generation: new_instance_generation,
            mtu,
            state: LINK_ACTIVE,
        };
        Ok(next)
    }

    fn validate_live_link(&self, link: LinkHandle) -> Result<()> {
        self.validate_current_link(link)?;
        if self.links[usize::from(link.slot)].state != LINK_ACTIVE {
            return Err(Error::State);
        }
        Ok(())
    }

    fn validate_current_link(&self, link: LinkHandle) -> Result<()> {
        if link.adapter_instance != self.adapter_instance {
            return Err(Error::Argument);
        }
        let slot = self
            .links
            .get(usize::from(link.slot))
            .ok_or(Error::Argument)?;
        if slot.state == LINK_UNUSED
            || slot.handle_generation != link.handle_generation
            || slot.instance_generation != link.link_instance_generation
        {
            return Err(Error::NotFound);
        }
        Ok(())
    }

    fn validate_tx_token(&self, token: TxToken) -> Result<usize> {
        self.ingress.validate_tx_token_shape(token)?;
        let index = usize::from(token.slot);
        let slot = self.tx[index];
        if slot.state == TxState::Free
            || slot.generation != token.generation
            || slot.link.slot != token.link_slot
            || slot.link.handle_generation != token.link_handle_generation
            || slot.link.link_instance_generation != token.link_instance_generation
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn validate_rx_token(&self, token: RxToken) -> Result<()> {
        if token.adapter_instance != self.adapter_instance
            || token.generation == 0
            || usize::from(token.slot) >= RX_SLOTS
            || usize::from(token.link_slot) >= LINKS
        {
            return Err(Error::Argument);
        }
        let slot = &self.ingress.rx[usize::from(token.slot)];
        if slot.generation.load(Ordering::Relaxed) != token.generation
            || slot.link_slot.load(Ordering::Relaxed) != u32::from(token.link_slot)
            || slot.link_handle_generation.load(Ordering::Relaxed)
                != u32::from(token.link_handle_generation)
            || slot.link_instance_generation.load(Ordering::Relaxed)
                != token.link_instance_generation
        {
            return Err(Error::NotFound);
        }
        Ok(())
    }

    fn rx_token_at(&self, index: usize) -> Result<RxToken> {
        let slot = &self.ingress.rx[index];
        Ok(RxToken {
            adapter_instance: self.adapter_instance,
            slot: u16::try_from(index).map_err(|_| Error::Config)?,
            generation: slot.generation.load(Ordering::Relaxed),
            link_slot: u16::try_from(slot.link_slot.load(Ordering::Relaxed))
                .map_err(|_| Error::Malformed)?,
            link_handle_generation: u16::try_from(
                slot.link_handle_generation.load(Ordering::Relaxed),
            )
            .map_err(|_| Error::Malformed)?,
            link_instance_generation: slot.link_instance_generation.load(Ordering::Relaxed),
        })
    }

    fn merge_submit_result(
        &mut self,
        index: usize,
        token: TxToken,
        result: DriverSubmit,
    ) -> Result<()> {
        if let DriverSubmit::Complete(outcome) = result {
            if self.ingress.tx_complete(token, outcome).is_err() {
                return self.mark_in_doubt(index);
            }
        }
        let latched = match self.ingress.tx_latched(token) {
            Ok(value) => value,
            Err(Error::InDoubt) => return self.mark_in_doubt(index),
            Err(error) => return Err(error),
        };
        match (result, latched) {
            (DriverSubmit::NotSubmitted, None) => {
                self.tx[index].state = TxState::NotSubmitted;
                Ok(())
            }
            (DriverSubmit::Submitted, None) => {
                self.tx[index].state = TxState::Submitted;
                Ok(())
            }
            (DriverSubmit::Complete(expected), Some(actual)) if expected == actual => {
                self.set_completed(index, actual);
                Ok(())
            }
            (DriverSubmit::Submitted, Some(actual)) => {
                self.set_completed(index, actual);
                Ok(())
            }
            (DriverSubmit::Unknown, _) | (DriverSubmit::NotSubmitted, Some(_)) => {
                self.mark_in_doubt(index)
            }
            (DriverSubmit::Complete(_), None | Some(_)) => self.mark_in_doubt(index),
        }
    }

    fn merge_cancel_result(
        &mut self,
        index: usize,
        token: TxToken,
        result: DriverCancel,
    ) -> Result<()> {
        if let DriverCancel::Complete(outcome) = result {
            if self.ingress.tx_complete(token, outcome).is_err() {
                return self.mark_in_doubt(index);
            }
        }
        let latched = match self.ingress.tx_latched(token) {
            Ok(value) => value,
            Err(Error::InDoubt) => return self.mark_in_doubt(index),
            Err(error) => return Err(error),
        };
        match (result, latched) {
            (DriverCancel::Cancelled, None) => {
                self.tx[index].state = TxState::Cancelled;
                Ok(())
            }
            (DriverCancel::NotCancelled, None) => {
                self.tx[index].state = TxState::Submitted;
                Ok(())
            }
            (DriverCancel::Complete(expected), Some(actual)) if expected == actual => {
                self.set_completed(index, actual);
                Ok(())
            }
            (DriverCancel::NotCancelled, Some(actual)) => {
                self.set_completed(index, actual);
                Ok(())
            }
            (DriverCancel::Unknown, _) | (DriverCancel::Cancelled, Some(_)) => {
                self.mark_in_doubt(index)
            }
            (DriverCancel::Complete(_), None | Some(_)) => self.mark_in_doubt(index),
        }
    }

    fn reconcile_latch(&mut self, index: usize, token: TxToken) -> Result<()> {
        match self.ingress.tx_latched(token) {
            Ok(Some(outcome)) => {
                if matches!(
                    self.tx[index].state,
                    TxState::Submitting | TxState::Submitted | TxState::Completed
                ) && (self.tx[index].terminal.is_none()
                    || self.tx[index].terminal == Some(outcome)
                    || self.tx[index].terminal == Some(TerminalOutcome::Failure(Error::InDoubt)))
                {
                    self.set_completed(index, outcome);
                    return Ok(());
                }
                self.mark_in_doubt(index)
            }
            Ok(None) => Ok(()),
            Err(Error::InDoubt) if !self.tx_link_is_current(index) => {
                let outcome = self.ingress.seal_tx_after_link_invalidation(token)?;
                self.set_completed(index, outcome);
                Ok(())
            }
            Err(Error::InDoubt) => self.mark_in_doubt(index),
            Err(error) => Err(error),
        }
    }

    fn set_completed(&mut self, index: usize, outcome: TerminalOutcome) {
        self.tx[index].state = TxState::Completed;
        self.tx[index].terminal = Some(outcome);
    }

    fn mark_in_doubt(&mut self, index: usize) -> Result<()> {
        let link_slot = usize::from(self.tx[index].link.slot);
        self.tx[index].state = TxState::InDoubt;
        self.tx[index].terminal = None;
        if self.tx_link_is_current(index) {
            self.links[link_slot].state = LINK_FENCED;
            let link = self.tx[index].link;
            self.ingress
                .configure_link(link, self.links[link_slot].mtu, LINK_FENCED)?;
        }
        Err(Error::InDoubt)
    }

    fn tx_link_is_current(&self, index: usize) -> bool {
        let tx_link = self.tx[index].link;
        self.links
            .get(usize::from(tx_link.slot))
            .is_some_and(|current| {
                current.state != LINK_UNUSED
                    && current.handle_generation == tx_link.handle_generation
                    && current.instance_generation == tx_link.link_instance_generation
            })
    }

    fn view_at(&self, index: usize) -> TxView {
        let slot = self.tx[index];
        TxView {
            state: slot.state,
            terminal: slot.terminal,
            core_tx_slot: slot.request_slot,
        }
    }
}

#[cfg(test)]
mod tests {
    use core::sync::atomic::Ordering;

    use super::{
        AdapterEventIngress, AdapterOwner, Error, LINK_ACTIVE, LinkHandle, RX_FREE, TxState,
    };

    #[test]
    fn capacities_and_instances_are_checked() {
        assert!(matches!(
            AdapterEventIngress::<1, 1, 1, 8>::new(0),
            Err(Error::Config)
        ));
        assert!(matches!(
            AdapterEventIngress::<0, 1, 1, 8>::new(1),
            Err(Error::Config)
        ));
        let ingress = AdapterEventIngress::<1, 1, 1, 8>::new(7).expect("valid ingress");
        assert!(matches!(AdapterOwner::new(8, &ingress), Err(Error::Config)));
    }

    #[test]
    fn tx_rx_and_link_generations_never_wrap() {
        let ingress = AdapterEventIngress::<1, 1, 1, 8>::new(9).expect("ingress");
        let mut owner = AdapterOwner::new(9, &ingress).expect("owner");
        let opened = owner.open_link(0, 1, 8).expect("link");

        owner.tx[0].generation = u32::MAX;
        assert_eq!(owner.tx_reserve(opened, 1), Err(Error::Exhausted));
        assert_eq!(owner.tx[0].state, TxState::Free);

        owner
            .try_set_rx_enabled(true)
            .expect("enable RX publication");
        ingress.rx[0].generation.store(u32::MAX, Ordering::Relaxed);
        ingress.rx[0].state.store(RX_FREE, Ordering::Relaxed);
        assert_eq!(
            ingress.rx_publish(
                opened,
                &[1],
                super::RxMeta {
                    timestamp_us: 1,
                    sender_discriminator: 1,
                },
            ),
            Err(Error::Exhausted)
        );
        assert_eq!(ingress.rx[0].generation.load(Ordering::Relaxed), u32::MAX);

        let terminal = LinkHandle {
            adapter_instance: 9,
            slot: 0,
            handle_generation: u16::MAX,
            link_instance_generation: 7,
        };
        owner.links[0].handle_generation = u16::MAX;
        owner.links[0].instance_generation = 7;
        owner.links[0].state = LINK_ACTIVE;
        ingress
            .configure_link(terminal, 8, LINK_ACTIVE)
            .expect("configure terminal handle");
        assert_eq!(owner.reopen_link(terminal, 8, 8), Err(Error::Exhausted));
        assert_eq!(owner.link_handle(0), Ok(terminal));
    }

    #[test]
    fn owner_observation_does_not_stall_on_completion_writer_gate() {
        let ingress = AdapterEventIngress::<1, 1, 1, 8>::new(10).expect("ingress");
        let mut owner = AdapterOwner::new(10, &ingress).expect("owner");
        let link = owner.open_link(0, 1, 8).expect("link");
        let token = owner.tx_reserve(link, 4).expect("token");
        owner.tx[0].state = TxState::Submitted;
        ingress.tx[0].outcome.store(0, Ordering::Release);
        ingress.tx[0].locked.store(true, Ordering::Release);

        let view = owner
            .tx_view(token)
            .expect("atomic observation must not borrow writer gate");
        assert_eq!(view.state, TxState::Completed);
        assert_eq!(view.terminal, Some(super::TerminalOutcome::Success));
        ingress.tx[0].locked.store(false, Ordering::Release);
    }
}
