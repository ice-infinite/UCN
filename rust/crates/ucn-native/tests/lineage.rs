//! Exact authenticated lineage and cancellation barrier regressions.
use std::cell::Cell;
use ucn_core::{EndpointHandler, Lifecycle, SendOptions, Target};
use ucn_native::{
    Backend, BackendMessage, BackendScope, Channel, CommandPort, Destination, Inbox,
    LifecycleStorage, NativeRoot, QueuePolicy, Sample, SampleHandler, SecurityMinimum, Source,
};
use ucn_types::{
    AddressWidth, BindingGeneration, Error, NodeAddress, Result, ServiceId, TrafficClass,
};

fn source(a: u32) -> Source {
    Source {
        address: NodeAddress::new(a, AddressWidth::A0).unwrap(),
        binding: BindingGeneration::active(a).unwrap(),
        principal_digest: u64::from(a),
    }
}
fn scope(generation: u32) -> BackendScope {
    BackendScope {
        instance: 7,
        generation,
        slot: 1,
    }
}
struct Control {
    pending: Cell<bool>,
    sequence: Cell<u32>,
    scope: Cell<BackendScope>,
    live: Cell<BackendScope>,
    sent: Cell<u32>,
    received: Cell<u64>,
    source: Cell<Source>,
}
struct Port(&'static Control);
impl Backend for Port {
    fn supports(&self, s: SecurityMinimum) -> bool {
        s == SecurityMinimum::OriginAuth
    }
    fn local(&self) -> Destination {
        Destination {
            address: source(2).address,
            binding: source(2).binding,
        }
    }
    fn binding(&self, a: NodeAddress) -> Option<BindingGeneration> {
        Some(source(a.get()).binding)
    }
    fn principal(&self, a: NodeAddress) -> Option<u64> {
        Some(u64::from(a.get()))
    }
    fn lifecycle(&self) -> Lifecycle {
        Lifecycle::Initialized
    }
    fn trusted(&self) -> bool {
        false
    }
    fn install(&mut self, _: &[Channel], _: &'static dyn EndpointHandler) -> Result<()> {
        Ok(())
    }
    fn start(&mut self, _: u64) -> Result<()> {
        Ok(())
    }
    fn stop(&mut self) -> Result<()> {
        Ok(())
    }
    fn send(&mut self, t: Target, _: &[u8], _: SendOptions, _: u64) -> Result<()> {
        self.0.sent.set(self.0.sent.get() | (1 << t.address.get()));
        Ok(())
    }
    fn step(&mut self, _: u64, _: u16) -> Result<()> {
        Ok(())
    }
    fn poll(&mut self) -> Result<BackendMessage> {
        if !self.0.pending.replace(false) {
            return Err(Error::NotFound);
        }
        Ok(BackendMessage {
            source: self.0.source.get(),
            scope: self.0.scope.get(),
            receive_time_us: self.0.received.get(),
            origin_sequence: self.0.sequence.get(),
            service: ServiceId::new(0x40)?,
            traffic_class: TrafficClass::Q1,
            protection: SecurityMinimum::OriginAuth,
            payload_bytes: 1,
            payload: [0xA7; 64],
        })
    }
    fn validate(&self, sc: BackendScope, s: Source, _: &Channel, _: u64) -> Result<[u8; 16]> {
        if (sc == self.0.live.get() && (s == source(1) || s == source(3)))
            || (sc == BackendScope::LOCAL && s == source(2))
        {
            Ok([0x51; 16])
        } else {
            Err(Error::Access)
        }
    }
}
struct Handler(Cell<u32>);
impl SampleHandler for Handler {
    fn receive(&self, _: &Sample, _: &dyn CommandPort) {
        self.0.set(self.0.get() + 1);
    }
}
fn channel() -> Channel {
    let mut c = Channel {
        id: 1,
        service: ServiceId::new(0x40).unwrap(),
        schema_id: 1,
        schema_revision: 1,
        payload_bytes: 1,
        traffic_class: TrafficClass::Q1,
        queue: QueuePolicy::KeepLatest,
        security: SecurityMinimum::OriginAuth,
        max_local_age_us: 100,
        sources: [None; 16],
        targets: [None; 8],
    };
    c.sources[0] = Some(source(1));
    c.sources[1] = Some(source(2));
    c.targets[0] = Some(Destination {
        address: source(1).address,
        binding: source(1).binding,
    });
    c.targets[1] = Some(Destination {
        address: source(3).address,
        binding: source(3).binding,
    });
    c
}
fn control() -> &'static Control {
    Box::leak(Box::new(Control {
        pending: Cell::new(true),
        sequence: Cell::new(100),
        scope: Cell::new(scope(3)),
        live: Cell::new(scope(3)),
        sent: Cell::new(0),
        received: Cell::new(10),
        source: Cell::new(source(1)),
    }))
}

#[test]
fn authenticated_successor_resets_sequence_but_invalid_scope_does_not() {
    eprintln!(
        "host Rust Inbox N=4/8/16: {}/{}/{}; optional LifecycleStorage: {}/{}/{} bytes",
        core::mem::size_of::<Inbox<4>>(),
        core::mem::size_of::<Inbox<8>>(),
        core::mem::size_of::<Inbox<16>>(),
        core::mem::size_of::<LifecycleStorage<4>>(),
        core::mem::size_of::<LifecycleStorage<8>>(),
        core::mem::size_of::<LifecycleStorage<16>>()
    );
    let ctl = control();
    let inbox = Box::leak(Box::new(Inbox::<4>::new()));
    let mut root = NativeRoot::new(Box::leak(Box::new(Port(ctl))), inbox, 1, &[channel()]).unwrap();
    root.enable_lifecycle(Box::leak(Box::new(LifecycleStorage::new())))
        .unwrap();
    let handler = Box::leak(Box::new(Handler(Cell::new(0))));
    let sub = inbox.subscribe(1, handler).unwrap();
    root.start(0).unwrap();
    root.step(11, 1).unwrap();
    assert_eq!(root.dispatch(12, 2), Ok(1));
    ctl.sequence.set(1);
    ctl.scope.set(scope(4));
    ctl.live.set(scope(4));
    ctl.pending.set(true);
    root.step(13, 1).unwrap();
    assert_eq!(root.dispatch(14, 2), Ok(1));
    ctl.sequence.set(1000);
    ctl.scope.set(scope(5));
    ctl.pending.set(true);
    root.step(15, 1).unwrap();
    assert_eq!(root.dispatch(16, 2), Ok(0));
    ctl.sequence.set(2);
    ctl.scope.set(scope(4));
    ctl.pending.set(true);
    root.step(17, 1).unwrap();
    assert_eq!(root.dispatch(18, 2), Ok(1));
    inbox.close_subscription(sub).unwrap();
    let _sub = inbox.subscribe(1, handler).unwrap();
    ctl.pending.set(true);
    root.step(19, 1).unwrap();
    assert_eq!(root.dispatch(20, 2), Ok(0));
    ctl.scope.set(scope(3));
    ctl.live.set(scope(3));
    ctl.sequence.set(1000);
    ctl.pending.set(true);
    root.step(21, 1).unwrap();
    assert_eq!(root.dispatch(22, 2), Ok(0));
    let foreign = BackendScope {
        instance: 8,
        generation: 99,
        slot: 1,
    };
    ctl.scope.set(foreign);
    ctl.live.set(foreign);
    ctl.pending.set(true);
    root.step(23, 1).unwrap();
    assert_eq!(root.dispatch(24, 2), Ok(0));
    ctl.scope.set(scope(4));
    ctl.live.set(scope(4));
    ctl.sequence.set(3);
    ctl.pending.set(true);
    root.step(25, 1).unwrap();
    assert_eq!(root.dispatch(110, 2), Ok(0)); // TTL removes event only.
    ctl.received.set(111);
    ctl.pending.set(true);
    root.step(111, 1).unwrap();
    assert_eq!(root.dispatch(112, 2), Ok(0));
    inbox.source_fence(1, source(1)).unwrap();
    ctl.scope.set(scope(5));
    ctl.live.set(scope(5));
    ctl.sequence.set(1);
    ctl.pending.set(true);
    root.step(113, 1).unwrap();
    assert_eq!(root.dispatch(114, 2), Ok(0));
}

#[test]
fn retirement_cancels_only_old_peer_and_preserves_new_fanout() {
    let ctl = control();
    let inbox = Box::leak(Box::new(Inbox::<4>::new()));
    let mut root = NativeRoot::new(Box::leak(Box::new(Port(ctl))), inbox, 1, &[channel()]).unwrap();
    root.enable_lifecycle(Box::leak(Box::new(LifecycleStorage::new())))
        .unwrap();
    let handler = Box::leak(Box::new(Handler(Cell::new(0))));
    let _sub = inbox.subscribe(1, handler).unwrap();
    let pubh = inbox.publisher(1).unwrap();
    root.start(0).unwrap();
    root.step(11, 1).unwrap();
    inbox.publish(pubh, &[1]).unwrap();
    inbox.send(1, channel().targets[0].unwrap(), &[1]).unwrap();
    assert_eq!(root.peer_quiescent(source(1), scope(3)), Ok(false));
    root.retire_peer(source(1), scope(3)).unwrap();
    assert_eq!(root.peer_quiescent(source(1), scope(3)), Ok(true));
    root.step(12, 16).unwrap();
    assert_eq!(ctl.sent.get(), 1 << 3);
    assert_eq!(root.dispatch(13, 4), Ok(1)); // Only the local publish remains.
    ctl.sent.set(0);
    inbox.publish(pubh, &[2]).unwrap();
    ctl.scope.set(scope(4));
    ctl.live.set(scope(4));
    ctl.sequence.set(1);
    ctl.pending.set(true);
    root.retire_peer(source(1), scope(3)).unwrap(); // Old barrier cannot eat new work.
    root.step(14, 16).unwrap();
    assert_eq!(ctl.sent.get(), (1 << 1) | (1 << 3));
    root.retire_peer(source(1), scope(3)).unwrap();
    assert_eq!(root.dispatch(15, 4), Ok(2)); // New remote lineage and local publish both survive.
    inbox.close_publisher(pubh).unwrap();
}

#[test]
fn retirement_preserves_other_source_and_target_only_peer_is_supported() {
    let ctl = control();
    let inbox = Box::leak(Box::new(Inbox::<4>::new()));
    let mut c = channel();
    c.sources[2] = Some(source(3));
    let mut root = NativeRoot::new(Box::leak(Box::new(Port(ctl))), inbox, 1, &[c]).unwrap();
    let storage = Box::leak(Box::new(LifecycleStorage::new()));
    root.enable_lifecycle(storage).unwrap();
    assert_eq!(root.enable_lifecycle(storage), Err(Error::State));
    let handler = Box::leak(Box::new(Handler(Cell::new(0))));
    let _sub = inbox.subscribe(1, handler).unwrap();
    root.start(0).unwrap();
    root.step(11, 1).unwrap();
    ctl.source.set(source(3));
    ctl.pending.set(true);
    root.step(12, 1).unwrap();
    let mut alias = source(1);
    alias.principal_digest = 999;
    assert_eq!(root.retire_peer(alias, scope(3)), Err(Error::NotFound));
    assert_eq!(root.retire_peer(source(9), scope(3)), Err(Error::NotFound));
    assert_eq!(
        root.retire_peer(source(1), BackendScope::LOCAL),
        Err(Error::Argument)
    );
    assert_eq!(
        root.retire_peer(
            source(1),
            BackendScope {
                instance: 8,
                generation: 3,
                slot: 1
            }
        ),
        Err(Error::Access)
    );
    root.retire_peer(source(1), scope(3)).unwrap();
    assert_eq!(root.dispatch(13, 4), Ok(1));
    assert_eq!(handler.0.get(), 1);

    let ctl = control();
    ctl.pending.set(false);
    let inbox = Box::leak(Box::new(Inbox::<4>::new()));
    let mut root = NativeRoot::new(Box::leak(Box::new(Port(ctl))), inbox, 2, &[channel()]).unwrap();
    assert_eq!(root.enable_lifecycle(storage), Err(Error::State));
    root.enable_lifecycle(Box::leak(Box::new(LifecycleStorage::new())))
        .unwrap();
    root.start(0).unwrap();
    let pubh = inbox.publisher(1).unwrap();
    inbox.publish(pubh, &[1]).unwrap();
    assert_eq!(root.peer_quiescent(source(3), scope(3)), Ok(false));
    root.retire_peer(source(3), scope(3)).unwrap();
    root.step(11, 16).unwrap();
    assert_eq!(ctl.sent.get(), 1 << 1);
}

#[test]
fn queued_fifo_old_scope_is_not_revived_by_stale_backend_validation() {
    let ctl = control();
    let inbox = Box::leak(Box::new(Inbox::<4>::new()));
    let mut c = channel();
    c.queue = QueuePolicy::Fifo;
    let mut root = NativeRoot::new(Box::leak(Box::new(Port(ctl))), inbox, 1, &[c]).unwrap();
    root.enable_lifecycle(Box::leak(Box::new(LifecycleStorage::new())))
        .unwrap();
    let handler = Box::leak(Box::new(Handler(Cell::new(0))));
    let _sub = inbox.subscribe(1, handler).unwrap();
    root.start(0).unwrap();
    root.step(11, 1).unwrap();
    ctl.scope.set(scope(4));
    ctl.live.set(scope(4));
    ctl.sequence.set(1);
    ctl.pending.set(true);
    root.step(12, 1).unwrap();
    ctl.live.set(scope(3));
    assert_eq!(root.dispatch(13, 4), Ok(0));
}
