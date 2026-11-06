use core::sync::atomic::{AtomicU32, Ordering};
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_types::{Error, Result};

/// Physical event direction; 物理事件方向。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EventDirection {
    /// Actual TX; 真实TX。
    Tx,
    /// Actual RX; 真实RX。
    Rx,
}
/// Non-forgeable Link-owned event identity; Link持有的不可伪造事件身份。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EventKey {
    pub(crate) link: u16,
    pub(crate) instance: u32,
    pub(crate) generation: u32,
    pub(crate) slot: usize,
    pub(crate) token: u32,
    pub(crate) direction: EventDirection,
}
/// Physical timestamp proof, issued only by Timed Link after actual completion.
/// `仅TimedLink在实际completion后签发的物理时间戳证明`。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Timestamp {
    pub(crate) key: EventKey,
    pub(crate) time: u64,
    pub(crate) bound: u32,
}
/// Non-forgeable successful Driver retirement receipt; 不可伪造Driver成功退休凭据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RetirementProof {
    key: EventKey,
}
impl RetirementProof {
    /// Exact retired key; 精确退休键。
    #[must_use]
    pub const fn key(self) -> EventKey {
        self.key
    }
}
impl Timestamp {
    /// Exact event; 精确事件。
    #[must_use]
    pub const fn key(self) -> EventKey {
        self.key
    }
    /// Local monotonic physical time; 本地单调物理时刻。
    #[must_use]
    pub const fn time_us(self) -> u64 {
        self.time
    }
    /// Credible hardware/Driver capture bound; 可信硬件/Driver锁存界。
    #[must_use]
    pub const fn bound_us(self) -> u32 {
        self.bound
    }
}
struct AtomicEvent {
    state: AtomicU32,
    token: AtomicU32,
    generation: AtomicU32,
    instance: AtomicU32,
    link_direction: AtomicU32,
    low: AtomicU32,
    high: AtomicU32,
    bound: AtomicU32,
}
impl AtomicEvent {
    const fn new() -> Self {
        Self {
            state: AtomicU32::new(0),
            token: AtomicU32::new(0),
            generation: AtomicU32::new(0),
            instance: AtomicU32::new(0),
            link_direction: AtomicU32::new(0),
            low: AtomicU32::new(0),
            high: AtomicU32::new(0),
            bound: AtomicU32::new(0),
        }
    }
}
/// Caller-owned atomic ISR/SMP timestamp ingress; 调用方持有ISR/SMP原子时间戳入口。
pub struct TimestampIngress<const SLOTS: usize> {
    events: [AtomicEvent; SLOTS],
}
impl<const N: usize> Default for TimestampIngress<N> {
    fn default() -> Self {
        Self::new()
    }
}
impl<const N: usize> TimestampIngress<N> {
    /// Static zero allocation; 静态零分配。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            events: [const { AtomicEvent::new() }; N],
        }
    }
    fn arm(&self, key: EventKey) -> Result<()> {
        let event = self.events.get(key.slot).ok_or(Error::State)?;
        event
            .state
            .compare_exchange(0, 4, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| Error::State)?;
        event.token.store(key.token, Ordering::Relaxed);
        event.generation.store(key.generation, Ordering::Relaxed);
        event.instance.store(key.instance, Ordering::Relaxed);
        event.link_direction.store(
            u32::from(key.link) * 2 + u32::from(key.direction == EventDirection::Rx),
            Ordering::Relaxed,
        );
        event.state.store(1, Ordering::Release);
        Ok(())
    }
    /// Sole allowed callback/ISR exception: publish exact physical completion.
    /// 唯一允许的回调/ISR例外：发布精确物理completion。
    /// # Errors
    /// Stale token, invalid bound/time or conflicting duplicate rejects with no torn timestamp.
    pub fn publish(&self, key: EventKey, time: u64, bound: u32) -> Result<()> {
        if time == 0 || bound == 0 {
            return Err(Error::Argument);
        }
        let event = self.events.get(key.slot).ok_or(Error::State)?;
        let state = event.state.load(Ordering::Acquire);
        if state == 3 {
            if event.token.load(Ordering::Relaxed) == key.token
                && event.generation.load(Ordering::Relaxed) == key.generation
                && event.instance.load(Ordering::Relaxed) == key.instance
                && event.link_direction.load(Ordering::Relaxed)
                    == u32::from(key.link) * 2 + u32::from(key.direction == EventDirection::Rx)
                && self.read(key)?.time == time
                && event.bound.load(Ordering::Relaxed) == bound
            {
                return Ok(());
            }
            return Err(Error::State);
        }
        event
            .state
            .compare_exchange(1, 2, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| Error::State)?;
        if event.token.load(Ordering::Relaxed) != key.token
            || event.generation.load(Ordering::Relaxed) != key.generation
            || event.instance.load(Ordering::Relaxed) != key.instance
            || event.link_direction.load(Ordering::Relaxed)
                != u32::from(key.link) * 2 + u32::from(key.direction == EventDirection::Rx)
        {
            event.state.store(1, Ordering::Release);
            return Err(Error::State);
        }
        let bytes = time.to_le_bytes();
        event.low.store(
            u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            Ordering::Relaxed,
        );
        event.high.store(
            u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]),
            Ordering::Relaxed,
        );
        event.bound.store(bound, Ordering::Relaxed);
        event.state.store(3, Ordering::Release);
        Ok(())
    }
    fn read(&self, key: EventKey) -> Result<Timestamp> {
        let event = self.events.get(key.slot).ok_or(Error::State)?;
        if event.state.load(Ordering::Acquire) != 3
            || event.token.load(Ordering::Relaxed) != key.token
            || event.generation.load(Ordering::Relaxed) != key.generation
            || event.instance.load(Ordering::Relaxed) != key.instance
            || event.link_direction.load(Ordering::Relaxed)
                != u32::from(key.link) * 2 + u32::from(key.direction == EventDirection::Rx)
        {
            return Err(Error::State);
        }
        let time = u64::from(event.low.load(Ordering::Relaxed))
            | (u64::from(event.high.load(Ordering::Relaxed)) << 32);
        Ok(Timestamp {
            key,
            time,
            bound: event.bound.load(Ordering::Relaxed),
        })
    }
    fn clear(&self, key: EventKey) -> Result<()> {
        let event = self.events.get(key.slot).ok_or(Error::State)?;
        if event.token.load(Ordering::Acquire) != key.token
            || event.generation.load(Ordering::Relaxed) != key.generation
            || event.instance.load(Ordering::Relaxed) != key.instance
            || event.link_direction.load(Ordering::Relaxed)
                != u32::from(key.link) * 2 + u32::from(key.direction == EventDirection::Rx)
        {
            return Err(Error::State);
        }
        let state = event.state.load(Ordering::Acquire);
        if !matches!(state, 1 | 3) {
            return Err(Error::State);
        }
        event
            .state
            .compare_exchange(state, 0, Ordering::AcqRel, Ordering::Relaxed)
            .map_err(|_| Error::State)?;
        Ok(())
    }
}
/// Driver submit disposition; Driver提交结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TimedSubmit {
    /// No physical submission; 未物理提交。
    NotSubmitted,
    /// Completion will arrive through ingress; completion异步到达。
    Pending,
    /// Actual hardware capture completed; 真实硬件打点完成。
    Captured {
        /// Physical time; 物理时刻。
        time: u64,
        /// Conservative bound; 保守界。
        bound: u32,
    },
    /// Uncertain hardware state; 硬件状态未知。
    Unknown,
}
/// Bounded nonblocking timestamp Driver boundary; 有界非阻塞打点Driver边界。
pub trait TimedDriver<const SLOTS: usize> {
    /// Submit exact frame and token; 提交精确帧和Token。
    /// The slice is borrowed only for this call; an asynchronous Driver must copy it
    /// into its own bounded storage before returning. A capture does not itself retire
    /// that Driver reservation. 本slice只借用于本次调用；异步Driver返回前必须复制到
    /// 自己的固定存储，打点完成本身不代表Driver预留已经退休。
    fn submit(
        &mut self,
        key: EventKey,
        frame: &[u8],
        ingress: &TimestampIngress<SLOTS>,
    ) -> TimedSubmit;
    /// Retire/cancel exact hardware reservation; 退休/取消精确硬件预留。
    /// # Errors
    /// Failure retains the Owner's release obligation.
    fn retire(&mut self, key: EventKey) -> Result<()>;
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum EventPhase {
    Reserved,
    Submitted,
    Completed,
    ReleasePending,
    RetireOnly,
}
/// Exact read-only physical state, not submission or completion evidence.
/// 精确只读物理状态，不代替提交/完成证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TimedEventState {
    /// Token allocated but not submitted; 已分配但未提交。
    Reserved,
    /// Awaiting actual completion; 等待真实完成。
    Submitted,
    /// Actual timestamp captured; 实际时间戳已锁存。
    Completed,
    /// Retire-only ambiguous hardware state; 硬件不确定，仅可退休。
    RetireOnly,
}
#[derive(Clone, Copy)]
struct Event {
    key: EventKey,
    phase: EventPhase,
}
/// One Link with shared callback gate and separate atomic completion ingress.
/// 单Link，共享回调门及独立原子completion入口。
pub struct TimedLink<'a, const N: usize> {
    id: u16,
    instance: u32,
    generation: u32,
    next_token: u32,
    operation: u32,
    gate: &'a CallbackGate,
    ingress: &'a TimestampIngress<N>,
    events: [Option<Event>; N],
}
impl<'a, const N: usize> TimedLink<'a, N> {
    /// Initialize caller-owned shared gates; 初始化调用方共享门。
    /// # Errors
    /// Invalid Link/instance/capacity rejects.
    pub fn new(
        id: u16,
        instance: u32,
        gate: &'a CallbackGate,
        ingress: &'a TimestampIngress<N>,
    ) -> Result<Self> {
        if id == 0 || instance == 0 || N == 0 {
            return Err(Error::Config);
        }
        Ok(Self {
            id,
            instance,
            generation: 1,
            next_token: 0,
            operation: 0,
            gate,
            ingress,
            events: [None; N],
        })
    }
    fn enter(&mut self) -> Result<ucn_owner::CallbackLease<'a>> {
        let next = self.operation.checked_add(1).ok_or(Error::Exhausted)?;
        let claim = CallbackClaim::new(self.instance, next, 1, 0x0801)?;
        let lease = self.gate.try_enter(claim)?;
        self.operation = next;
        Ok(lease)
    }
    fn index(&self, key: EventKey) -> Result<usize> {
        if key.link != self.id
            || key.instance != self.instance
            || key.slot >= N
            || self.events[key.slot].is_none_or(|event| event.key != key)
        {
            return Err(Error::State);
        }
        Ok(key.slot)
    }
    /// Reserve a token; driver callbacks cannot allocate TX or RX on any shared Link.
    /// 预留Token，共享域Driver回调不能分配任意Link的TX/RX。
    /// # Errors
    /// Busy gate, capacity or counter exhaustion has no event writes.
    pub fn reserve(&mut self, direction: EventDirection) -> Result<EventKey> {
        let lease = self.enter()?;
        let result = (|| {
            let slot = self
                .events
                .iter()
                .position(Option::is_none)
                .ok_or(Error::NoSpace)?;
            let token = self.next_token.checked_add(1).ok_or(Error::Exhausted)?;
            let key = EventKey {
                link: self.id,
                instance: self.instance,
                generation: self.generation,
                slot,
                token,
                direction,
            };
            self.ingress.arm(key)?;
            self.events[slot] = Some(Event {
                key,
                phase: EventPhase::Reserved,
            });
            self.next_token = token;
            Ok(key)
        })();
        self.gate.leave(lease)?;
        result
    }
    /// Submit once, merging synchronous early physical completion; 提交一次并合并早到completion。
    /// # Errors
    /// Reused/wrong-direction token or contradictory Driver outcome fails closed.
    pub fn submit(
        &mut self,
        key: EventKey,
        frame: &[u8],
        driver: &mut impl TimedDriver<N>,
    ) -> Result<()> {
        let slot = self.index(key)?;
        if key.direction != EventDirection::Tx
            || frame.is_empty()
            || self.events[slot].is_none_or(|event| event.phase != EventPhase::Reserved)
        {
            return Err(Error::State);
        }
        let lease = self.enter()?;
        self.events[slot].as_mut().ok_or(Error::State)?.phase = EventPhase::Submitted;
        let returned = driver.submit(key, frame, self.ingress);
        let latched = self.ingress.read(key).ok();
        let mut contradiction = false;
        match returned {
            TimedSubmit::NotSubmitted => {
                if latched.is_some() {
                    contradiction = true;
                    self.events[slot].as_mut().ok_or(Error::State)?.phase =
                        EventPhase::ReleasePending;
                } else {
                    self.events[slot].as_mut().ok_or(Error::State)?.phase = EventPhase::Reserved;
                }
            }
            TimedSubmit::Pending => {
                if latched.is_some() {
                    self.events[slot].as_mut().ok_or(Error::State)?.phase = EventPhase::Completed;
                }
            }
            TimedSubmit::Captured { time, bound } => {
                if self.ingress.publish(key, time, bound).is_err() {
                    contradiction = true;
                    self.events[slot].as_mut().ok_or(Error::State)?.phase =
                        EventPhase::ReleasePending;
                } else {
                    self.events[slot].as_mut().ok_or(Error::State)?.phase = EventPhase::Completed;
                }
            }
            TimedSubmit::Unknown => {
                self.events[slot].as_mut().ok_or(Error::State)?.phase = EventPhase::ReleasePending;
            }
        }
        self.gate.leave(lease)?;
        if contradiction {
            return Err(Error::InDoubt);
        }
        Ok(())
    }
    /// Return only a real completed timestamp; 仅返回真实已完成打点。
    /// # Errors
    /// Reserved, invalidated or retirement-only events cannot become samples.
    pub fn timestamp(&mut self, key: EventKey) -> Result<Timestamp> {
        let lease = self.enter()?;
        let result = self.read_timestamp(key);
        self.gate.leave(lease)?;
        result
    }
    fn read_timestamp(&mut self, key: EventKey) -> Result<Timestamp> {
        let slot = self.index(key)?;
        let stamp = self.ingress.read(key)?;
        let event = self.events[slot].as_mut().ok_or(Error::State)?;
        match event.phase {
            EventPhase::Submitted | EventPhase::Completed => {
                event.phase = EventPhase::Completed;
                Ok(stamp)
            }
            EventPhase::ReleasePending => {
                event.phase = EventPhase::RetireOnly;
                Err(Error::State)
            }
            _ => Err(Error::State),
        }
    }
    /// Capture actual RX before publishing an atomic frame+timestamp item.
    /// 原子帧/时间戳项发布前锁存真实RX。
    /// # Errors
    /// Only exact reserved RX token is accepted.
    pub fn receive_capture(&mut self, key: EventKey, time: u64, bound: u32) -> Result<Timestamp> {
        let slot = self.index(key)?;
        if key.direction != EventDirection::Rx
            || self.events[slot].is_none_or(|event| event.phase != EventPhase::Reserved)
        {
            return Err(Error::State);
        }
        let lease = self.enter()?;
        let result = self.ingress.publish(key, time, bound);
        if result.is_ok() {
            self.events[slot].as_mut().ok_or(Error::State)?.phase = EventPhase::Completed;
        }
        self.gate.leave(lease)?;
        result?;
        self.ingress.read(key)
    }
    /// Mark a local release obligation, preserving it for Driver retries.
    /// 标记本地释放义务，保留用于Driver重试。
    /// # Errors
    /// Old event or callback reentry rejects.
    pub fn release_pending(&mut self, key: EventKey) -> Result<()> {
        let lease = self.enter()?;
        let result = (|| {
            let slot = self.index(key)?;
            self.events[slot].as_mut().ok_or(Error::State)?.phase = EventPhase::ReleasePending;
            Ok(())
        })();
        self.gate.leave(lease)?;
        result
    }
    /// Retire succeeds only after Driver proves retirement; `Driver证明退休后才清Token`。
    /// # Errors
    /// Driver failure leaves exact key/reservation owned for retry.
    pub fn retire(&mut self, key: EventKey, driver: &mut impl TimedDriver<N>) -> Result<()> {
        self.retire_receipt(key, driver).map(|_| ())
    }
    /// Successful retirement emits a sealed receipt for Coordinator obligation ACK.
    /// 成功退休签发封闭凭据，供Coordinator确认义务。
    /// # Errors
    /// Driver failure preserves the exact event and emits no receipt.
    pub fn retire_receipt(
        &mut self,
        key: EventKey,
        driver: &mut impl TimedDriver<N>,
    ) -> Result<RetirementProof> {
        let slot = self.index(key)?;
        let lease = self.enter()?;
        let result = driver.retire(key);
        self.gate.leave(lease)?;
        result?;
        self.ingress.clear(key)?;
        self.events[slot] = None;
        Ok(RetirementProof { key })
    }
    /// Reopen fences all local old events but never loses obligations.
    /// reopen Fence全部本地旧事件但不丢释放义务。
    /// # Errors
    /// Busy gate or generation exhaustion rejects before mutation.
    pub fn reopen(&mut self) -> Result<()> {
        let lease = self.enter()?;
        let result = (|| {
            let next = self.generation.checked_add(1).ok_or(Error::Exhausted)?;
            for event in self.events.iter_mut().flatten() {
                event.phase = EventPhase::ReleasePending;
            }
            self.generation = next;
            Ok(())
        })();
        self.gate.leave(lease)?;
        result
    }
    /// Unretired events; 未退休事件数。
    #[must_use]
    pub fn outstanding(&self) -> usize {
        self.events.iter().flatten().count()
    }
    /// Immutable Link identity and generation; 不变Link身份与当前代际。
    #[must_use]
    pub const fn identity(&self) -> (u16, u32, u32) {
        (self.id, self.instance, self.generation)
    }
    /// Require the actual Security callback gate, not an unrelated equal-number gate.
    /// 要求实际Security回调门，不能用编号相同的另一门冒充。
    #[must_use]
    pub fn uses_gate(&self, gate: &CallbackGate) -> bool {
        core::ptr::eq(self.gate, gate)
    }
    /// Inspect exact owned token without entering a Driver callback; 查看精确Token状态。
    /// # Errors
    /// Cross-Link or stale token rejects.
    pub fn event_state(&self, key: EventKey) -> Result<TimedEventState> {
        Ok(
            match self.events[self.index(key)?].ok_or(Error::State)?.phase {
                EventPhase::Reserved => TimedEventState::Reserved,
                EventPhase::Submitted => TimedEventState::Submitted,
                EventPhase::Completed => TimedEventState::Completed,
                EventPhase::ReleasePending | EventPhase::RetireOnly => TimedEventState::RetireOnly,
            },
        )
    }
}
