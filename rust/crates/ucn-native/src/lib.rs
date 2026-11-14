#![no_std]
#![forbid(unsafe_code)]
//! Optional Pub/Sub over a trusted Core or an authenticated Coordinator backend.
//! 可裁剪业务接口；保护事实只能来自可信后端，不增加普通Wire字段。

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
    /// Actual remote protection; local delivery never impersonates a crypto proof.
    pub protection: Option<SecurityMinimum>,
    /// Full verified Principal, never the configured 64-bit label.
    pub principal: Option<[u8; 16]>,
    /// Bounded owned copy; only `payload_bytes` bytes are meaningful.
    pub payload: [u8; PAYLOAD_BYTES],
}
/// Opaque registry reference issued by the trusted backend; never sent on Wire.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BackendScope {
    /// Nonzero Root instance for remote receipts.
    pub instance: u32,
    /// Immutable source/session registration generation.
    pub generation: u32,
    /// Exact source registry slot.
    pub slot: u16,
}
impl BackendScope {
    /// Local-only sentinel, not a remote authentication grant.
    pub const LOCAL: Self = Self {
        instance: 0,
        generation: 0,
        slot: 0,
    };
}
struct LifecycleState<const N: usize> {
    source: [[BackendScope; 16]; N],
    retired_source: [[bool; 16]; N],
    retired_target: [[BackendScope; 8]; N],
    cancelled: [u32; N],
    attached: bool,
}
fn target_peer(target: Destination, source: Source) -> bool {
    target.address == source.address && target.binding == source.binding
}
fn scope_namespace(a: BackendScope, b: BackendScope) -> bool {
    a.instance == b.instance && a.slot == b.slot
}
impl<const N: usize> LifecycleState<N> {
    fn check(&self, state: &State<N>, source: Source, scope: BackendScope) -> Result<()> {
        if scope.instance == 0 || scope.generation == 0 {
            return Err(Error::Argument);
        }
        if state.stepping || state.dispatching {
            return Err(Error::State);
        }
        let (mut address, mut exact, mut target) = (false, false, false);
        for (ch, channel) in state.channels.iter().enumerate() {
            let Some(c) = channel else {
                continue;
            };
            for (i, configured) in c.sources.iter().enumerate() {
                let Some(configured) = configured else {
                    continue;
                };
                address |=
                    configured.address == source.address && configured.binding == source.binding;
                if *configured == source {
                    exact = true;
                    let prior = self.source[ch][i];
                    if prior.instance != 0 && !scope_namespace(prior, scope) {
                        return Err(Error::Access);
                    }
                }
            }
            for (i, configured) in c.targets.iter().flatten().enumerate() {
                if target_peer(*configured, source) {
                    target = true;
                    let prior = self.retired_target[ch][i];
                    if prior.instance != 0 && !scope_namespace(prior, scope) {
                        return Err(Error::Access);
                    }
                }
            }
        }
        if exact || (!address && target) {
            Ok(())
        } else {
            Err(Error::NotFound)
        }
    }
    fn newer(&self, state: &State<N>, source: Source, scope: BackendScope) -> bool {
        state.channels.iter().enumerate().any(|(ch, c)| {
            c.is_some_and(|c| {
                c.sources.iter().enumerate().any(|(i, s)| {
                    *s == Some(source) && self.source[ch][i].generation > scope.generation
                })
            })
        })
    }
}
/// Optional permanent hot-lifecycle tables. Ordinary roots do not reserve these arrays.
/// Only the trusted Root can attach or mutate this single-task, non-Sync storage.
///
/// ```compile_fail
/// fn require_sync<T: Sync>() {}
/// require_sync::<ucn_native::LifecycleStorage<4>>();
/// ```
pub struct LifecycleStorage<const N: usize = 4> {
    state: RefCell<LifecycleState<N>>,
}
impl<const N: usize> LifecycleStorage<N> {
    /// Empty, statically allocated lifecycle storage; attachment is one-way.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            state: RefCell::new(LifecycleState {
                source: [[BackendScope::LOCAL; 16]; N],
                retired_source: [[false; 16]; N],
                retired_target: [[BackendScope::LOCAL; 8]; N],
                cancelled: [0; N],
                attached: false,
            }),
        }
    }
}
impl<const N: usize> Default for LifecycleStorage<N> {
    fn default() -> Self {
        Self::new()
    }
}
/// Trusted Coordinator output after actual byte authentication and replay commit.
pub struct BackendMessage {
    /// Configured label; authorization uses the full backend Binding.
    pub source: Source,
    /// Immutable backend registration.
    pub scope: BackendScope,
    /// Original local RX capture.
    pub receive_time_us: u64,
    /// Existing Security-owned sequence.
    pub origin_sequence: u32,
    /// Existing Wire Service.
    pub service: ServiceId,
    /// Actual authenticated class.
    pub traffic_class: TrafficClass,
    /// Actual protection, not merely configured minimum.
    pub protection: SecurityMinimum,
    /// Copied prefix length.
    pub payload_bytes: u16,
    /// Bounded owned payload.
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
    scope: Option<BackendScope>,
}
// Independent latches: stop is irreversible, dispatch and step are separately gated.
#[expect(
    clippy::struct_excessive_bools,
    reason = "independent lifecycle and execution latches"
)]
struct State<const N: usize> {
    instance: u32,
    lifecycle: Option<&'static LifecycleStorage<N>>,
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
            lifecycle: None,
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
        if let Some(lifecycle) = self.lifecycle {
            lifecycle.state.borrow_mut().cancelled[slot] = 0;
        }
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
                    protection: None,
                    principal: None,
                    payload,
                },
                source: src,
                channel: ch,
                recipients: self.recipients(ch),
                order: self.event_order,
                age_anchor: self.now,
                scope: None,
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
        if self
            .state
            .borrow()
            .channels
            .iter()
            .flatten()
            .any(|c| c.security != SecurityMinimum::StaticTrusted)
        {
            return Err(Error::Unsupported);
        }
        self.dispatch_validated(now, budget, |_, _, _, _| Err(Error::Unsupported))
    }
    fn dispatch_validated<F>(&self, now: u64, budget: u16, validate: F) -> Result<u16>
    where
        F: Fn(BackendScope, Source, &Channel, u64) -> Result<[u8; 16]>,
    {
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
                    let stale_scope = !ev.sample.local
                        && s.lifecycle.is_some_and(|l| {
                            let l = l.state.borrow();
                            l.retired_source[ev.channel][ev.source]
                                || Some(l.source[ev.channel][ev.source]) != ev.scope
                        });
                    if s.stopping
                        || stale_scope
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
                            found = Some((
                                ev.sample,
                                sub,
                                s.subs[sub].handler.ok_or(Error::State)?,
                                ev.scope,
                            ));
                            break;
                        }
                    }
                    if found.is_some() {
                        break;
                    }
                }
                found
            };
            let Some((mut sample, sub, handler, scope)) = next else {
                break;
            };
            let channel = {
                let s = self.state.borrow();
                s.channels
                    .iter()
                    .flatten()
                    .find(|c| c.id == sample.channel_id)
                    .copied()
                    .ok_or(Error::State)?
            };
            if channel.security != SecurityMinimum::StaticTrusted {
                if let Ok(principal) = validate(
                    scope.unwrap_or(BackendScope::LOCAL),
                    sample.source,
                    &channel,
                    now,
                ) {
                    sample.principal = if sample.local { None } else { Some(principal) };
                } else {
                    self.state.borrow_mut().subs[sub].active = false;
                    continue;
                }
            }
            handler.receive(&sample, self);
            self.state.borrow_mut().subs[sub].active = false;
            count += 1;
        }
        self.state.borrow_mut().dispatching = false;
        Ok(count)
    }
}
impl<const N: usize> Inbox<N> {
    fn ingest_authenticated(&self, m: &BackendMessage, principal: [u8; 16]) {
        let Ok(mut s) = self.state.try_borrow_mut() else {
            return;
        };
        if !s.running
            || s.stopping
            || m.origin_sequence == 0
            || m.scope.instance == 0
            || m.scope.generation == 0
        {
            return;
        }
        let Some(ch) = s
            .channels
            .iter()
            .position(|c| c.is_some_and(|c| c.service == m.service))
        else {
            return;
        };
        let Some(c) = s.channels[ch] else { return };
        if c.security == SecurityMinimum::StaticTrusted
            || m.protection == SecurityMinimum::StaticTrusted
            || (c.security == SecurityMinimum::Encrypted
                && m.protection != SecurityMinimum::Encrypted)
            || c.traffic_class != m.traffic_class
            || c.payload_bytes != m.payload_bytes
            || usize::from(m.payload_bytes) > PAYLOAD_BYTES
        {
            return;
        }
        let Some(src) = c
            .sources
            .iter()
            .position(|source| *source == Some(m.source))
        else {
            return;
        };
        if s.fenced[ch][src] {
            return;
        }
        if let Some(lifecycle) = s.lifecycle {
            let mut l = lifecycle.state.borrow_mut();
            let prior = l.source[ch][src];
            if prior.instance != 0
                && (prior.instance != m.scope.instance
                    || prior.slot != m.scope.slot
                    || m.scope.generation < prior.generation
                    || (prior == m.scope && l.retired_source[ch][src]))
            {
                return;
            }
            if prior != m.scope {
                l.source[ch][src] = m.scope;
                l.retired_source[ch][src] = false;
                s.high[ch][src] = 0;
            }
        }
        if c.queue == QueuePolicy::KeepLatest && m.origin_sequence <= s.high[ch][src] {
            return;
        }
        let Some(at) = s.event_slot(ch, src, false) else {
            return;
        };
        let Some(order) = s.event_order.checked_add(1) else {
            return;
        };
        s.high[ch][src] = s.high[ch][src].max(m.origin_sequence);
        let recipients = s.recipients(ch);
        if recipients != 0 {
            s.event_order = order;
            s.events[at] = Some(Event {
                sample: Sample {
                    source: m.source,
                    received_us: m.receive_time_us,
                    origin_sequence: m.origin_sequence,
                    channel_id: c.id,
                    payload_bytes: m.payload_bytes,
                    local: false,
                    protection: Some(m.protection),
                    principal: Some(principal),
                    payload: m.payload,
                },
                source: src,
                channel: ch,
                recipients,
                order,
                age_anchor: m.receive_time_us.min(s.now),
                scope: Some(m.scope),
            });
        }
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
            if c.security != SecurityMinimum::StaticTrusted
                || c.service != m.service_id
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
                            protection: Some(SecurityMinimum::StaticTrusted),
                            principal: None,
                            payload,
                        },
                        source: src,
                        channel: ch,
                        recipients,
                        order,
                        age_anchor: m.receive_timestamp_us.min(s.now),
                        scope: None,
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
    /// Trusted assembly capability, not a per-message authentication flag.
    fn supports(&self, minimum: SecurityMinimum) -> bool {
        minimum == SecurityMinimum::StaticTrusted && self.trusted()
    }
    /// Pull copied, authenticated bytes from the same real protocol ingress.
    /// # Errors
    /// Empty queue returns `NotFound`; no application can inject through Inbox.
    fn poll(&mut self) -> Result<BackendMessage> {
        Err(Error::NotFound)
    }
    /// Recheck exact immutable scope and current ACL immediately before callback.
    /// # Errors
    /// Expired, fenced, mismatched or insufficiently protected context rejects.
    fn validate(
        &self,
        _scope: BackendScope,
        _source: Source,
        _channel: &Channel,
        _now: u64,
    ) -> Result<[u8; 16]> {
        Err(Error::Unsupported)
    }
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
    fn send(
        &mut self,
        target: Target,
        payload: &[u8],
        options: SendOptions,
        now: u64,
    ) -> Result<()>;
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
    fn send(&mut self, t: Target, p: &[u8], o: SendOptions, _now: u64) -> Result<()> {
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
        if backend.lifecycle() != Lifecycle::Initialized || inbox.state.borrow().instance != 0 {
            return Err(Error::State);
        }
        for (i, c) in channels.iter().enumerate() {
            if !backend.supports(c.security) {
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
    /// Attach optional permanent lifecycle storage before start. Never grants authentication.
    /// # Errors
    /// Running/stopping/reentrant Root, reused storage, or a static-trusted backend.
    pub fn enable_lifecycle(&mut self, storage: &'static LifecycleStorage<N>) -> Result<()> {
        let mut s = self
            .inbox
            .state
            .try_borrow_mut()
            .map_err(|_| Error::State)?;
        let mut l = storage.state.try_borrow_mut().map_err(|_| Error::State)?;
        if self.backend.trusted()
            || s.running
            || s.stopping
            || s.stepping
            || s.dispatching
            || s.lifecycle.is_some()
            || l.attached
        {
            return Err(Error::State);
        }
        l.attached = true;
        s.lifecycle = Some(storage);
        Ok(())
    }
    /// Retire queued obligations, not physical Driver work.
    /// # Errors
    /// Invalid scope or inactive lifecycle management.
    pub fn retire_peer(&mut self, source: Source, scope: BackendScope) -> Result<()> {
        let mut s = self
            .inbox
            .state
            .try_borrow_mut()
            .map_err(|_| Error::State)?;
        let storage = s.lifecycle.ok_or(Error::State)?;
        let mut l = storage.state.try_borrow_mut().map_err(|_| Error::State)?;
        l.check(&s, source, scope)?;
        let newer = l.newer(&s, source, scope);
        for ch in 0..N {
            let Some(c) = s.channels[ch] else {
                continue;
            };
            for (i, configured) in c.sources.iter().enumerate() {
                if *configured == Some(source) {
                    if l.source[ch][i].generation <= scope.generation {
                        l.source[ch][i] = scope;
                        l.retired_source[ch][i] = true;
                    }
                    for event in &mut s.events {
                        if event.is_some_and(|e| {
                            e.channel == ch && e.source == i && e.scope == Some(scope)
                        }) {
                            *event = None;
                        }
                    }
                }
            }
            for (i, configured) in c.targets.iter().flatten().enumerate() {
                if !target_peer(*configured, source)
                    || newer
                    || l.retired_target[ch][i].generation >= scope.generation
                {
                    continue;
                }
                l.retired_target[ch][i] = scope;
                for (j, job) in s.jobs.iter().enumerate() {
                    if let Some(job) = job {
                        let bit = if job.target.is_some() { 0 } else { i };
                        if job.channel == ch
                            && job.next_target <= bit
                            && job.target.is_none_or(|t| target_peer(t, source))
                        {
                            l.cancelled[j] |= 1 << bit;
                        }
                    }
                }
            }
        }
        Ok(())
    }
    /// Query application obligations, not physical Driver work.
    /// # Errors
    /// Invalid scope or inactive lifecycle management.
    pub fn peer_quiescent(&self, source: Source, scope: BackendScope) -> Result<bool> {
        let s = self.inbox.state.try_borrow().map_err(|_| Error::State)?;
        let storage = s.lifecycle.ok_or(Error::State)?;
        let l = storage.state.try_borrow().map_err(|_| Error::State)?;
        l.check(&s, source, scope)?;
        if s.events
            .iter()
            .flatten()
            .any(|e| e.sample.source == source && e.scope == Some(scope))
        {
            return Ok(false);
        }
        if l.newer(&s, source, scope) {
            return Ok(true);
        }
        for (j, job) in s.jobs.iter().enumerate() {
            let Some(job) = job else {
                continue;
            };
            let c = s.channels[job.channel].ok_or(Error::State)?;
            for (i, configured) in c.targets.iter().flatten().enumerate() {
                let bit = if job.target.is_some() { 0 } else { i };
                if target_peer(*configured, source)
                    && l.retired_target[job.channel][i].generation < scope.generation
                    && job.next_target <= bit
                    && job.target.is_none_or(|t| target_peer(t, source))
                    && l.cancelled[j] & (1 << bit) == 0
                {
                    return Ok(false);
                }
            }
        }
        Ok(true)
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
            let cancelled = {
                let s = self.inbox.state.borrow();
                s.lifecycle.map_or(0, |l| l.state.borrow().cancelled[i])
            };
            while job.next_target < 8 && cancelled & (1 << job.next_target) != 0 {
                job.next_target += 1;
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
                now,
            );
            if !matches!(admission, Err(Error::NoSpace | Error::State)) {
                job.next_target += 1;
                self.inbox.state.borrow_mut().jobs[i] = Some(job);
            }
        }
        if self.backend.lifecycle() != Lifecycle::Quiescent {
            self.backend.step(now, budget)?;
        }
        for _ in 0..budget {
            match self.backend.poll() {
                Ok(message) => {
                    let channel = self
                        .inbox
                        .state
                        .borrow()
                        .channels
                        .iter()
                        .flatten()
                        .find(|c| c.service == message.service)
                        .copied();
                    if let Some(c) = channel {
                        if let Ok(principal) =
                            self.backend
                                .validate(message.scope, message.source, &c, now)
                        {
                            self.inbox.ingest_authenticated(&message, principal);
                        }
                    }
                }
                Err(Error::NotFound) => break,
                Err(_) => {}
            }
        }
        Ok(())
    }
    /// Run the application executor outside the protocol runtime.
    /// # Errors
    /// Same as `Inbox::dispatch`.
    pub fn dispatch(&self, now: u64, budget: u16) -> Result<u16> {
        self.inbox
            .dispatch_validated(now, budget, |scope, source, channel, now| {
                self.backend.validate(scope, source, channel, now)
            })
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
