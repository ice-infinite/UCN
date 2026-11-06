#![no_std]
#![forbid(unsafe_code)]
//! Optional static Pub/Sub over the existing trusted O0 Core, without new Wire fields.
//! 可裁剪静态业务层；此后端不冒充密码认证，不支持的安全要求明确拒绝。

use core::cell::RefCell;
use core::marker::PhantomData;
use ucn_adapter::{AdapterOwner, TxDriver};
use ucn_core::{
    CoreNode, EndpointDisposition, EndpointHandler, EndpointMessage, Lifecycle, SendOptions, Target,
};
use ucn_types::{BindingGeneration, Error, HopLimit, NodeAddress, Result, ServiceId, TrafficClass};

/// Maximum copied business payload. 最大复制业务长度。
pub const PAYLOAD_BYTES: usize = 64;
/// Explicit product trust, never an E2E authentication assertion.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SecurityMinimum {
    /// Explicit product-trusted isolated transport; no crypto claim.
    StaticTrusted,
    /// Requires Origin authentication; unsupported by this backend.
    OriginAuth,
    /// Requires encryption; unsupported by this backend.
    Encrypted,
}
/// Local application queue policy, NOT a Wire delivery guarantee.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum QueuePolicy {
    /// Preserve admission order independently for each exact source stream.
    Fifo,
    /// Coalesce only the same channel/source/locality; reject older sequences.
    KeepLatest,
}
/// Source configuration, not a cryptographic proof.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Source {
    /// Configured node address.
    pub address: NodeAddress,
    /// Exact static binding generation.
    pub binding: BindingGeneration,
    /// Configured Core identity label, not a MAC or authentication proof.
    pub principal_digest: u64,
}
/// Destination frozen by product configuration.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Destination {
    /// Static target node.
    pub address: NodeAddress,
    /// Exact target binding generation.
    pub binding: BindingGeneration,
}
/// Static channel. First N sources / N/2 targets are available for capacity N.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Channel {
    /// Process-local channel ID, never sent.
    pub id: u16,
    /// Existing C1 Service ID; the static demultiplexing key.
    pub service: ServiceId,
    /// Product-assigned business schema identity.
    pub schema_id: u32,
    /// Exact schema revision; not a protocol version.
    pub schema_revision: u16,
    /// Exact encoded business length, 1..=64.
    pub payload_bytes: u16,
    /// Q1..Q3; application code cannot claim Q0.
    pub traffic_class: TrafficClass,
    /// Local application queue policy.
    pub queue: QueuePolicy,
    /// Required protection; unsupported requirements are never downgraded.
    pub security: SecurityMinimum,
    /// Local RX-to-dispatch or admission-to-submit lifetime, not source age.
    pub max_local_age_us: u64,
    /// Unique configured sources. Entries beyond the selected capacity are empty.
    pub sources: [Option<Source>; 16],
    /// Fixed remote targets; local delivery is implicit, not a target entry.
    pub targets: [Option<Destination>; 8],
}
/// Exact process-local entity reference.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Handle {
    instance: u32,
    generation: u32,
    slot: usize,
    subscription: bool,
}
/// Owned bounded sample copied before calling the application.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Sample {
    /// Exact configured source, never a fabricated verified principal.
    pub source: Source,
    /// Original local RX/admission time, never refreshed on queue replacement.
    pub received_us: u64,
    /// C1 sequence, or a separate local-only sequence if `local` is true.
    pub origin_sequence: u32,
    /// Local channel identity.
    pub channel_id: u16,
    /// Valid prefix length in `payload`.
    pub payload_bytes: u16,
    /// True only for local, non-Wire delivery.
    pub local: bool,
    /// Bounded owned copy; only `payload_bytes` bytes are meaningful.
    pub payload: [u8; PAYLOAD_BYTES],
}
/// Read-only callback with a deferred command port; never borrows Core mutably.
pub trait SampleHandler {
    /// Called without a protocol or inbox borrow; commands are copied/deferred.
    fn receive(&self, sample: &Sample, commands: &dyn CommandPort);
}
/// The only mutation port passed to an application callback.
pub trait CommandPort {
    /// Copy one bounded publication; success means local admission only.
    /// # Errors
    /// Wrong/stale handle, source fence, length mismatch, full queue or expiry overflow.
    fn publish(&self, handle: Handle, payload: &[u8]) -> Result<()>;
    /// Fence future callbacks independently of data-queue capacity.
    /// # Errors
    /// Wrong/stale/already closed handle or a concurrent protocol step.
    fn close_subscription(&self, handle: Handle) -> Result<()>;
}

/// Product Codec: no string/topic metadata is added to ordinary frames.
pub trait Message: Sized {
    /// Exact product schema identity.
    const SCHEMA_ID: u32;
    /// Exact schema revision.
    const REVISION: u16;
    /// Exact encoded business length, at most 64 bytes.
    const WIRE_BYTES: u16;
    /// Encode into the caller's exact-size buffer.
    /// # Errors
    /// Reject an incorrect buffer length or invalid business value.
    fn encode(&self, output: &mut [u8]) -> Result<()>;
}

/// Typed allocation-free publisher borrowing a permanent inbox.
pub struct Publisher<M: Message, const N: usize = 4> {
    inbox: &'static Inbox<N>,
    handle: Handle,
    closed: bool,
    marker: PhantomData<M>,
}
impl<M: Message, const N: usize> Publisher<M, N> {
    /// Verify schema before consuming a publisher slot.
    /// # Errors
    /// Schema mismatch or the same admission errors as `Inbox::publisher`.
    pub fn new(inbox: &'static Inbox<N>, channel: u16) -> Result<Self> {
        {
            let s = inbox.state.try_borrow().map_err(|_| Error::State)?;
            let c = s.channels[s.channel(channel)?].ok_or(Error::State)?;
            if c.schema_id != M::SCHEMA_ID
                || c.schema_revision != M::REVISION
                || c.payload_bytes != M::WIRE_BYTES
            {
                return Err(Error::Config);
            }
        }
        let handle = inbox.publisher(channel)?;
        Ok(Self {
            inbox,
            handle,
            closed: false,
            marker: PhantomData,
        })
    }
    /// Encode and copy; success is local admission, not remote delivery.
    /// # Errors
    /// Invalid Codec output, closed publisher, full queue or source fence.
    pub fn publish(&self, value: &M) -> Result<()> {
        if self.closed || usize::from(M::WIRE_BYTES) > PAYLOAD_BYTES {
            return Err(Error::State);
        }
        let mut bytes = [0; PAYLOAD_BYTES];
        let p = &mut bytes[..usize::from(M::WIRE_BYTES)];
        value.encode(p)?;
        self.inbox.publish(self.handle, p)
    }
    /// Close without waiting for already Core-admitted physical work.
    /// # Errors
    /// Stale handle or active protocol step.
    pub fn close(&mut self) -> Result<()> {
        self.inbox.close_publisher(self.handle)?;
        self.closed = true;
        Ok(())
    }
}
impl<M: Message, const N: usize> Drop for Publisher<M, N> {
    fn drop(&mut self) {
        if !self.closed {
            let _ = self.inbox.close_publisher(self.handle);
        }
    }
}

#[derive(Clone, Copy)]
struct Entity {
    generation: u32,
    channel: usize,
    open: bool,
    active: bool,
    handler: Option<&'static dyn SampleHandler>,
}
impl Entity {
    const EMPTY: Self = Self {
        generation: 0,
        channel: 0,
        open: false,
        active: false,
        handler: None,
    };
}
#[derive(Clone, Copy)]
struct Job {
    channel: usize,
    next_target: usize,
    deadline: u64,
    order: u32,
    target: Option<Destination>,
    payload: [u8; PAYLOAD_BYTES],
}
#[derive(Clone, Copy)]
struct Event {
    sample: Sample,
    source: usize,
    channel: usize,
    recipients: u32,
    order: u32,
    age_anchor: u64,
}
// Independent latches: stop is irreversible, dispatch and step are separately gated.
#[expect(
    clippy::struct_excessive_bools,
    reason = "independent lifecycle and execution latches"
)]
struct State<const N: usize> {
    instance: u32,
    channels: [Option<Channel>; N],
    pubs: [Entity; N],
    subs: [Entity; N],
    jobs: [Option<Job>; N],
    events: [Option<Event>; N],
    high: [[u32; 16]; N],
    fenced: [[bool; 16]; N],
    now: u64,
    local: Option<Destination>,
    order: u32,
    local_sequence: u32,
    event_order: u32,
    running: bool,
    stopping: bool,
    stepping: bool,
    dispatching: bool,
    job_cursor: usize,
    event_cursor: usize,
    sub_cursor: usize,
}
impl<const N: usize> State<N> {
    const fn new() -> Self {
        Self {
            instance: 0,
            channels: [None; N],
            pubs: [Entity::EMPTY; N],
            subs: [Entity::EMPTY; N],
            jobs: [None; N],
            events: [None; N],
            high: [[0; 16]; N],
            fenced: [[false; 16]; N],
            now: 0,
            local: None,
            order: 0,
            local_sequence: 0,
            event_order: 0,
            running: false,
            stopping: false,
            stepping: false,
            dispatching: false,
            job_cursor: 0,
            event_cursor: 0,
            sub_cursor: 0,
        }
    }
    fn channel(&self, id: u16) -> Result<usize> {
        self.channels
            .iter()
            .position(|c| c.is_some_and(|c| c.id == id))
            .ok_or(Error::Argument)
    }
    fn entity(&self, h: Handle, sub: bool) -> Result<Entity> {
        if h.instance != self.instance || h.subscription != sub || h.slot >= N || h.generation == 0
        {
            return Err(Error::State);
        }
        let e = if sub {
            self.subs[h.slot]
        } else {
            self.pubs[h.slot]
        };
        if e.generation != h.generation {
            return Err(Error::State);
        }
        Ok(e)
    }
    fn recipients(&self, ch: usize) -> u32 {
        self.subs
            .iter()
            .enumerate()
            .filter(|(_, s)| s.open && s.channel == ch)
            .fold(0, |m, (i, _)| m | (1 << i))
    }
    fn event_slot(&self, ch: usize, src: usize, local: bool) -> Option<usize> {
        if self.channels[ch]?.queue == QueuePolicy::KeepLatest {
            if let Some(i) = self.events.iter().position(|e| {
                e.is_some_and(|e| e.channel == ch && e.source == src && e.sample.local == local)
            }) {
                return Some(i);
            }
        }
        self.events.iter().position(Option::is_none)
    }
    fn enqueue(&mut self, ch: usize, p: &[u8], target: Option<Destination>) -> Result<()> {
        if !self.running || self.stopping || self.stepping {
            return Err(Error::State);
        }
        let c = self.channels[ch].ok_or(Error::State)?;
        if p.len() != usize::from(c.payload_bytes) {
            return Err(Error::Argument);
        }
        let local = self.local.ok_or(Error::State)?;
        let src = c
            .sources
            .iter()
            .position(|s| {
                s.is_some_and(|s| s.address == local.address && s.binding == local.binding)
            })
            .ok_or(Error::Access)?;
        if self.fenced[ch][src] {
            return Err(Error::Access);
        }
        let deadline = self
            .now
            .checked_add(c.max_local_age_us)
            .ok_or(Error::Exhausted)?;
        let slot = self
            .jobs
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let order = self.order.checked_add(1).ok_or(Error::Exhausted)?;
        let mut payload = [0; PAYLOAD_BYTES];
        payload[..p.len()].copy_from_slice(p);
        let event = if target.is_none() && self.recipients(ch) != 0 {
            let at = self.event_slot(ch, src, true).ok_or(Error::NoSpace)?;
            let seq = self.local_sequence.checked_add(1).ok_or(Error::Exhausted)?;
            self.event_order.checked_add(1).ok_or(Error::Exhausted)?;
            Some((at, seq))
        } else {
            None
        };
        self.jobs[slot] = Some(Job {
            channel: ch,
            next_target: 0,
            deadline,
            order,
            target,
            payload,
        });
        self.order = order;
        if let Some((at, seq)) = event {
            self.local_sequence = seq;
            self.event_order += 1;
            self.events[at] = Some(Event {
                sample: Sample {
                    source: c.sources[src].ok_or(Error::State)?,
                    received_us: self.now,
                    origin_sequence: seq,
                    channel_id: c.id,
                    payload_bytes: c.payload_bytes,
                    local: true,
                    payload,
                },
                source: src,
                channel: ch,
                recipients: self.recipients(ch),
                order: self.event_order,
                age_anchor: self.now,
            });
        }
        Ok(())
    }
}

/// Permanently borrowed static inbox. `RefCell` intentionally makes this single-task, !Sync.
/// 永久借用的静态Inbox，禁止把它包装成虚假的跨线程安全对象。
///
/// ```compile_fail
/// fn require_sync<T: Sync>() {}
/// require_sync::<ucn_native::Inbox<4>>();
/// ```
pub struct Inbox<const N: usize = 4> {
    state: RefCell<State<N>>,
}
impl<const N: usize> Default for Inbox<N> {
    fn default() -> Self {
        Self::new()
    }
}
impl<const N: usize> Inbox<N> {
    #[must_use]
    /// Construct static storage; no allocation, registration or authority is created.
    pub const fn new() -> Self {
        Self {
            state: RefCell::new(State::new()),
        }
    }
    fn create(&self, id: u16, handler: Option<&'static dyn SampleHandler>) -> Result<Handle> {
        let mut s = self.state.try_borrow_mut().map_err(|_| Error::State)?;
        if s.stopping || s.stepping {
            return Err(Error::State);
        }
        let ch = s.channel(id)?;
        let instance = s.instance;
        let table = if handler.is_some() {
            &mut s.subs
        } else {
            &mut s.pubs
        };
        let (slot, e) = table
            .iter_mut()
            .enumerate()
            .find(|(_, e)| !e.open && !e.active && e.generation < u32::MAX)
            .ok_or(Error::NoSpace)?;
        e.generation += 1;
        e.open = true;
        e.channel = ch;
        e.handler = handler;
        Ok(Handle {
            instance,
            generation: e.generation,
            slot,
            subscription: handler.is_some(),
        })
    }
    /// Open an exact local publisher.
    /// # Errors
    /// Unknown channel, stop fence, protocol step, exhausted generation or entity capacity.
    pub fn publisher(&self, id: u16) -> Result<Handle> {
        self.create(id, None)
    }
    /// Open a permanently borrowed callback; no captured heap closure is required.
    /// # Errors
    /// Same admission and capacity errors as `publisher`.
    pub fn subscribe(&self, id: u16, handler: &'static dyn SampleHandler) -> Result<Handle> {
        self.create(id, Some(handler))
    }
    /// Close a publisher; already copied jobs keep their original deadlines.
    /// # Errors
    /// Invalid/stale/closed handle or active protocol step.
    pub fn close_publisher(&self, h: Handle) -> Result<()> {
        let mut s = self.state.try_borrow_mut().map_err(|_| Error::State)?;
        let e = s.entity(h, false)?;
        if !e.open || s.stepping {
            return Err(Error::State);
        }
        s.pubs[h.slot].open = false;
        Ok(())
    }
    /// Query the exact closed generation, including an active callback latch.
    /// # Errors
    /// Invalid/stale handle or a conflicting inbox borrow.
    pub fn subscription_quiescent(&self, h: Handle) -> Result<bool> {
        let s = self.state.try_borrow().map_err(|_| Error::State)?;
        let e = s.entity(h, true)?;
        Ok(!e.open && !e.active)
    }
    /// Copy one point-to-point job within the configured channel/target policy.
    /// # Errors
    /// Unknown target/channel, invalid size, source fence, full queue or expiry overflow.
    pub fn send(&self, id: u16, target: Destination, p: &[u8]) -> Result<()> {
        let mut s = self.state.try_borrow_mut().map_err(|_| Error::State)?;
        let ch = s.channel(id)?;
        if !s.channels[ch]
            .ok_or(Error::State)?
            .targets
            .contains(&Some(target))
        {
            return Err(Error::Access);
        }
        s.enqueue(ch, p, Some(target))
    }
    /// Irreversibly fence queued receive events and not-yet-Core-admitted local jobs.
    /// Already Core-admitted physical work cannot be recalled by this application fence.
    /// # Errors
    /// Unknown channel/source or active protocol step.
    pub fn source_fence(&self, id: u16, source: Source) -> Result<()> {
        let mut s = self.state.try_borrow_mut().map_err(|_| Error::State)?;
        if s.stepping {
            return Err(Error::State);
        }
        let ch = s.channel(id)?;
        let src = s.channels[ch]
            .ok_or(Error::State)?
            .sources
            .iter()
            .position(|s| *s == Some(source))
            .ok_or(Error::NotFound)?;
        s.fenced[ch][src] = true;
        for e in &mut s.events {
            if e.is_some_and(|e| e.channel == ch && e.source == src) {
                *e = None;
            }
        }
        Ok(())
    }
    /// Dispatch at most `budget` callbacks with no protocol/inbox borrow held.
    /// # Errors
    /// Zero budget, time rollback, reentry or concurrent protocol step.
    pub fn dispatch(&self, now: u64, budget: u16) -> Result<u16> {
        {
            let mut s = self.state.try_borrow_mut().map_err(|_| Error::State)?;
            if budget == 0 || s.stepping || s.dispatching || now < s.now {
                return Err(Error::State);
            }
            s.dispatching = true;
            s.now = now;
        }
        let mut count = 0;
        for _ in 0..budget {
            let next = {
                let mut s = self.state.try_borrow_mut().map_err(|_| Error::State)?;
                let mut found = None;
                for k in 0..N {
                    let at = (s.event_cursor + k) % N;
                    let Some(mut ev) = s.events[at] else { continue };
                    let c = s.channels[ev.channel].ok_or(Error::State)?;
                    if c.queue == QueuePolicy::Fifo
                        && s.events.iter().flatten().any(|p| {
                            p.channel == ev.channel
                                && p.source == ev.source
                                && p.sample.local == ev.sample.local
                                && p.order < ev.order
                        })
                    {
                        continue;
                    }
                    if s.stopping
                        || s.fenced[ev.channel][ev.source]
                        || now < ev.age_anchor
                        || now - ev.age_anchor >= c.max_local_age_us
                    {
                        s.events[at] = None;
                        continue;
                    }
                    if now < ev.sample.received_us {
                        continue;
                    }
                    for j in 0..N {
                        let sub = (s.sub_cursor + j) % N;
                        if ev.recipients & (1 << sub) != 0 {
                            ev.recipients &= !(1 << sub);
                            s.events[at] = if ev.recipients == 0 { None } else { Some(ev) };
                            s.event_cursor = (at + 1) % N;
                            s.sub_cursor = (sub + 1) % N;
                            s.subs[sub].active = true;
                            found =
                                Some((ev.sample, sub, s.subs[sub].handler.ok_or(Error::State)?));
                            break;
                        }
                    }
                    if found.is_some() {
                        break;
                    }
                }
                found
            };
            let Some((sample, sub, handler)) = next else {
                break;
            };
            handler.receive(&sample, self);
            self.state.borrow_mut().subs[sub].active = false;
            count += 1;
        }
        self.state.borrow_mut().dispatching = false;
        Ok(count)
    }
}
impl<const N: usize> CommandPort for Inbox<N> {
    fn publish(&self, h: Handle, p: &[u8]) -> Result<()> {
        let mut s = self.state.try_borrow_mut().map_err(|_| Error::State)?;
        let e = s.entity(h, false)?;
        if !e.open {
            return Err(Error::State);
        }
        s.enqueue(e.channel, p, None)
    }
    fn close_subscription(&self, h: Handle) -> Result<()> {
        let mut s = self.state.try_borrow_mut().map_err(|_| Error::State)?;
        let e = s.entity(h, true)?;
        if !e.open || s.stepping {
            return Err(Error::State);
        }
        s.subs[h.slot].open = false;
        for at in &mut s.events {
            if let Some(mut ev) = *at {
                ev.recipients &= !(1 << h.slot);
                *at = if ev.recipients == 0 { None } else { Some(ev) };
            }
        }
        Ok(())
    }
}
impl<const N: usize> EndpointHandler for Inbox<N> {
    fn receive(&self, m: EndpointMessage<'_>) -> EndpointDisposition {
        let Ok(mut s) = self.state.try_borrow_mut() else {
            return EndpointDisposition::Drop;
        };
        if !s.running || s.stopping {
            return EndpointDisposition::Drop;
        }
        for ch in 0..N {
            let Some(c) = s.channels[ch] else { continue };
            if c.service != m.service_id
                || c.traffic_class != m.traffic_class
                || usize::from(c.payload_bytes) != m.payload.len()
            {
                continue;
            }
            for src in 0..16 {
                let Some(source) = c.sources[src] else {
                    continue;
                };
                let seq = m.origin_sequence.get();
                if source.address != m.source
                    || source.binding != m.source_binding_generation
                    || source.principal_digest != m.configured_principal_digest
                    || s.fenced[ch][src]
                    || (c.queue == QueuePolicy::KeepLatest && seq <= s.high[ch][src])
                {
                    continue;
                }
                let Some(at) = s.event_slot(ch, src, false) else {
                    return EndpointDisposition::Drop;
                };
                let Some(order) = s.event_order.checked_add(1) else {
                    return EndpointDisposition::Drop;
                };
                let mut payload = [0; PAYLOAD_BYTES];
                payload[..m.payload.len()].copy_from_slice(m.payload);
                s.high[ch][src] = s.high[ch][src].max(seq);
                let recipients = s.recipients(ch);
                if recipients != 0 {
                    s.event_order = order;
                    s.events[at] = Some(Event {
                        sample: Sample {
                            source,
                            received_us: m.receive_timestamp_us,
                            origin_sequence: seq,
                            channel_id: c.id,
                            payload_bytes: c.payload_bytes,
                            local: false,
                            payload,
                        },
                        source: src,
                        channel: ch,
                        recipients,
                        order,
                        age_anchor: m.receive_timestamp_us.min(s.now),
                    });
                }
                return EndpointDisposition::Accept;
            }
        }
        EndpointDisposition::Drop
    }
}

/// Public Core-only backend boundary. No private Security/Persistence Owner calls.
pub trait Backend {
    /// Immutable local identity.
    fn local(&self) -> Destination;
    /// Immutable configured binding.
    fn binding(&self, address: NodeAddress) -> Option<BindingGeneration>;
    /// Immutable configured identity label, not cryptographic proof.
    fn principal(&self, address: NodeAddress) -> Option<u64>;
    /// Actual Core lifecycle, including physical retirement.
    fn lifecycle(&self) -> Lifecycle;
    /// Product explicitly opted into a trusted isolated O0 transport.
    fn trusted(&self) -> bool;
    /// Install all endpoints atomically in a pristine Core.
    /// # Errors
    /// Wrong lifecycle, previously used endpoints, capacity or duplicate services; no writes.
    fn install(
        &mut self,
        channels: &[Channel],
        handler: &'static dyn EndpointHandler,
    ) -> Result<()>;
    /// Start the actual Core.
    /// # Errors
    /// Forward the exact Core lifecycle/time/Adapter error.
    fn start(&mut self, now: u64) -> Result<()>;
    /// Copy into the actual Core, not a second fake transport.
    /// # Errors
    /// Forward Core admission/route/capacity errors without consuming a sequence on failure.
    fn send(&mut self, target: Target, payload: &[u8], options: SendOptions) -> Result<()>;
    /// Advance actual Core/Adapter/Driver obligations.
    /// # Errors
    /// Forward Core time, lifecycle and indeterminate-driver errors.
    fn step(&mut self, now: u64, budget: u16) -> Result<()>;
    /// Fence Core admission and begin actual retirement.
    /// # Errors
    /// Forward Core lifecycle and Adapter errors.
    fn stop(&mut self) -> Result<()>;
}
/// Permanent static borrows prevent `Drop`/forget from freeing live Driver storage.
pub struct StaticBackend<
    D: TxDriver + 'static,
    const B: usize,
    const E: usize,
    const P: usize,
    const T: usize,
    const F: usize,
    const L: usize,
    const AT: usize,
    const AR: usize,
> {
    /// Single pre-existing Core, borrowed permanently.
    pub core: &'static mut CoreNode<'static, B, E, P, T, F>,
    /// The same Core's Adapter, not a second ingress queue.
    pub adapter: &'static mut AdapterOwner<'static, L, AT, AR, F>,
    /// Non-blocking fixed-resource physical or software driver.
    pub driver: &'static mut D,
}
impl<
    D: TxDriver + 'static,
    const B: usize,
    const E: usize,
    const P: usize,
    const T: usize,
    const F: usize,
    const L: usize,
    const AT: usize,
    const AR: usize,
> Backend for StaticBackend<D, B, E, P, T, F, L, AT, AR>
{
    fn local(&self) -> Destination {
        let c = self.core.config();
        Destination {
            address: c.local_address,
            binding: c.local_binding_generation,
        }
    }
    fn binding(&self, a: NodeAddress) -> Option<BindingGeneration> {
        if a == self.local().address {
            Some(self.local().binding)
        } else {
            self.core.binding(a)
        }
    }
    fn principal(&self, a: NodeAddress) -> Option<u64> {
        self.core.principal(a)
    }
    fn lifecycle(&self) -> Lifecycle {
        self.core.lifecycle()
    }
    fn trusted(&self) -> bool {
        self.core.config().trusted_o0_network
    }
    fn install(
        &mut self,
        channels: &[Channel],
        handler: &'static dyn EndpointHandler,
    ) -> Result<()> {
        if channels.len() > 16 {
            return Err(Error::Config);
        }
        let initial = channels.first().ok_or(Error::Config)?.service;
        let mut services = [initial; 16];
        for (i, c) in channels.iter().enumerate() {
            services[i] = c.service;
        }
        self.core
            .endpoint_install(&services[..channels.len()], handler)
    }
    fn start(&mut self, now: u64) -> Result<()> {
        self.core.start(now, self.adapter)
    }
    fn send(&mut self, t: Target, p: &[u8], o: SendOptions) -> Result<()> {
        self.core.send(t, p, o, self.adapter).map(|_| ())
    }
    fn step(&mut self, now: u64, b: u16) -> Result<()> {
        self.core
            .step(now, b, self.adapter, self.driver)
            .map(|_| ())
    }
    fn stop(&mut self) -> Result<()> {
        self.core.stop(self.adapter)
    }
}
/// Single task actor. The inbox may be called from its application callback.
/// A scoped backend cannot be used: physical work keeps permanent storage borrows.
/// ```compile_fail
/// fn temporary_backend(backend: &mut dyn ucn_native::Backend,
///                      inbox: &'static ucn_native::Inbox<4>) {
///     let _ = ucn_native::NativeRoot::new(backend, inbox, 1, &[]);
/// }
/// ```
pub struct NativeRoot<const N: usize = 4> {
    backend: &'static mut dyn Backend,
    inbox: &'static Inbox<N>,
    stopped: bool,
}
impl<const N: usize> NativeRoot<N> {
    /// Attach one pristine backend and permanently borrowed static inbox.
    /// Instance IDs must be nonzero and unique for the process lifetime.
    /// # Errors
    /// Invalid/ambiguous schema, unsupported security, wrong binding/identity, used inbox,
    /// non-pristine Core or insufficient endpoint capacity; no partial endpoint install.
    pub fn new(
        backend: &'static mut dyn Backend,
        inbox: &'static Inbox<N>,
        instance: u32,
        channels: &[Channel],
    ) -> Result<Self> {
        if !matches!(N, 4 | 8 | 16) || instance == 0 || channels.is_empty() || channels.len() > N {
            return Err(Error::Config);
        }
        if backend.lifecycle() != Lifecycle::Initialized
            || !backend.trusted()
            || inbox.state.borrow().instance != 0
        {
            return Err(Error::State);
        }
        for (i, c) in channels.iter().enumerate() {
            if c.security != SecurityMinimum::StaticTrusted {
                return Err(Error::Unsupported);
            }
            if c.id == 0
                || c.schema_id == 0
                || c.schema_revision == 0
                || c.payload_bytes == 0
                || usize::from(c.payload_bytes) > PAYLOAD_BYTES
                || c.max_local_age_us == 0
                || c.traffic_class == TrafficClass::Q0
                || channels[..i]
                    .iter()
                    .any(|other| other.id == c.id || other.service == c.service)
                || c.sources[..N].iter().all(Option::is_none)
                || c.sources[N..].iter().any(Option::is_some)
                || c.targets[N / 2..].iter().any(Option::is_some)
            {
                return Err(Error::Config);
            }
            for (j, source) in c
                .sources
                .iter()
                .enumerate()
                .filter_map(|(j, s)| s.map(|s| (j, s)))
            {
                if source.principal_digest == 0
                    || backend.principal(source.address) != Some(source.principal_digest)
                    || backend.binding(source.address) != Some(source.binding)
                    || c.sources[..j]
                        .iter()
                        .flatten()
                        .any(|s| s.address == source.address)
                {
                    return Err(Error::Config);
                }
            }
            for (j, target) in c
                .targets
                .iter()
                .enumerate()
                .filter_map(|(j, t)| t.map(|t| (j, t)))
            {
                if target.address == backend.local().address
                    || backend.binding(target.address) != Some(target.binding)
                    || c.targets[..j].contains(&Some(target))
                {
                    return Err(Error::Config);
                }
            }
        }
        // Exclusive initialization: register once, then publish the immutable table.
        backend.install(channels, inbox)?;
        {
            let mut s = inbox.state.borrow_mut();
            s.instance = instance;
            s.local = Some(backend.local());
            for (i, c) in channels.iter().enumerate() {
                s.channels[i] = Some(*c);
            }
        }
        Ok(Self {
            backend,
            inbox,
            stopped: false,
        })
    }
    #[must_use]
    /// Application command port; usable without borrowing the protocol runtime.
    pub const fn commands(&self) -> &'static Inbox<N> {
        self.inbox
    }
    /// Start once. Closed/fenced objects cannot be resurrected by restart.
    /// # Errors
    /// Already running/stopping, invalid time or actual Core start failure.
    pub fn start(&mut self, now: u64) -> Result<()> {
        {
            let s = self.inbox.state.borrow();
            if s.running || s.stopping {
                return Err(Error::State);
            }
        }
        self.backend.start(now)?;
        let mut s = self.inbox.state.borrow_mut();
        s.now = now;
        s.running = true;
        Ok(())
    }
    /// Irreversibly fence admissions and start real Core retirement.
    /// # Errors
    /// Active protocol step or actual Core stop failure.
    pub fn stop(&mut self) -> Result<()> {
        {
            let mut s = self.inbox.state.borrow_mut();
            if s.stepping {
                return Err(Error::State);
            }
            s.stopping = true;
            s.events.fill(None);
        }
        if !self.stopped || self.backend.lifecycle() == Lifecycle::Fault {
            self.backend.stop()?;
            self.stopped = true;
        }
        Ok(())
    }
    /// Bounded fair facade work followed by bounded actual Core work.
    /// # Errors
    /// Zero budget, time rollback, reentry or actual Core/Driver failure.
    pub fn step(&mut self, now: u64, budget: u16) -> Result<()> {
        {
            let mut s = self.inbox.state.borrow_mut();
            if budget == 0
                || s.stepping
                || s.dispatching
                || (!s.running && !s.stopping)
                || now < s.now
            {
                return Err(Error::State);
            }
            s.stepping = true;
            s.now = now;
        }
        let result = self.step_inner(now, budget);
        let mut s = self.inbox.state.borrow_mut();
        s.stepping = false;
        if result.is_err() && self.backend.lifecycle() == Lifecycle::Fault {
            s.stopping = true;
            s.events.fill(None);
        }
        result
    }
    fn step_inner(&mut self, now: u64, budget: u16) -> Result<()> {
        for _ in 0..budget {
            let selected = {
                let mut s = self.inbox.state.borrow_mut();
                let i = s.job_cursor;
                s.job_cursor = (i + 1) % N;
                s.jobs[i].map(|job| (i, job, s.stopping))
            };
            let Some((i, mut job, stopping)) = selected else {
                continue;
            };
            let c = self.inbox.state.borrow().channels[job.channel].ok_or(Error::State)?;
            let blocked = {
                let s = self.inbox.state.borrow();
                let local = s.local.ok_or(Error::State)?;
                c.sources.iter().enumerate().all(|(i, source)| {
                    !source.is_some_and(|source| {
                        source.address == local.address && !s.fenced[job.channel][i]
                    })
                })
            };
            if blocked || stopping || now >= job.deadline {
                self.inbox.state.borrow_mut().jobs[i] = None;
                continue;
            }
            if self
                .inbox
                .state
                .borrow()
                .jobs
                .iter()
                .flatten()
                .any(|j| j.channel == job.channel && j.order < job.order)
            {
                continue;
            }
            let destination = if let Some(t) = job.target {
                if job.next_target == 0 { Some(t) } else { None }
            } else {
                c.targets.iter().flatten().nth(job.next_target).copied()
            };
            let Some(t) = destination else {
                self.inbox.state.borrow_mut().jobs[i] = None;
                continue;
            };
            let options = SendOptions {
                traffic_class: c.traffic_class,
                hop_limit: HopLimit::new(4)?,
                absolute_deadline_us: job.deadline,
            };
            let admission = self.backend.send(
                Target {
                    address: t.address,
                    binding_generation: t.binding,
                    service_id: c.service,
                },
                &job.payload[..usize::from(c.payload_bytes)],
                options,
            );
            if !matches!(admission, Err(Error::NoSpace | Error::State)) {
                job.next_target += 1;
                self.inbox.state.borrow_mut().jobs[i] = Some(job);
            }
        }
        if self.backend.lifecycle() != Lifecycle::Quiescent {
            self.backend.step(now, budget)?;
        }
        Ok(())
    }
    /// Run the application executor outside the protocol runtime.
    /// # Errors
    /// Same as `Inbox::dispatch`.
    pub fn dispatch(&self, now: u64, budget: u16) -> Result<u16> {
        self.inbox.dispatch(now, budget)
    }
    #[must_use]
    /// True only after real Core physical retirement and all facade jobs/callbacks finish.
    pub fn quiescent(&self) -> bool {
        let s = self.inbox.state.borrow();
        self.backend.lifecycle() == Lifecycle::Quiescent
            && s.jobs.iter().all(Option::is_none)
            && !s.dispatching
            && !s.stepping
    }
}
