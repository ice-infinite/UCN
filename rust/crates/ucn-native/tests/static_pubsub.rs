//! Real Core/Adapter/software Driver integration, never fabricated verified facts.
use std::cell::{Cell, RefCell};
use ucn_adapter::{
    AdapterEventIngress, AdapterOwner, DriverCancel, DriverSubmit, LinkHandle, RxMeta,
    TerminalOutcome, TxDriver, TxToken,
};
use ucn_core::{CoreNode, NodeConfig, QueueCapacities, StaticPath};
use ucn_native::{
    Channel, CommandPort, Destination, Handle, Inbox, NativeRoot, QueuePolicy, Sample,
    SampleHandler, SecurityMinimum, Source, StaticBackend,
};
use ucn_types::{AddressWidth, BindingGeneration, Error, NodeAddress, ServiceId, TrafficClass};

type Ingress = AdapterEventIngress<1, 4, 8, 128>;
type Core = CoreNode<'static, 4, 4, 4, 8, 128>;
type Adapter = AdapterOwner<'static, 1, 4, 8, 128>;
fn addr(a: u32) -> NodeAddress {
    NodeAddress::new(a, AddressWidth::A0).unwrap()
}
fn binding(a: u32) -> BindingGeneration {
    BindingGeneration::active(a * 10).unwrap()
}
fn source() -> Source {
    Source {
        address: addr(1),
        binding: binding(1),
        principal_digest: 1,
    }
}
fn config(local: u32) -> NodeConfig {
    NodeConfig {
        node_instance: local,
        adapter_instance: 100 + local,
        address_width: AddressWidth::A0,
        local_address: addr(local),
        local_binding_generation: binding(local),
        local_principal_digest: u64::from(local),
        trusted_o0_network: true,
    }
}
fn channel(queue: QueuePolicy, targets: bool) -> Channel {
    let mut c = Channel {
        id: 1,
        service: ServiceId::new(0x40).unwrap(),
        schema_id: 0x4845_414c,
        schema_revision: 1,
        payload_bytes: 1,
        traffic_class: TrafficClass::Q1,
        queue,
        security: SecurityMinimum::StaticTrusted,
        max_local_age_us: 100,
        sources: [None; 16],
        targets: [None; 8],
    };
    c.sources[0] = Some(source());
    if targets {
        c.targets[0] = Some(Destination {
            address: addr(2),
            binding: binding(2),
        });
        c.targets[1] = Some(Destination {
            address: addr(3),
            binding: binding(3),
        });
    }
    c
}
struct Driver {
    peers: [Option<(&'static Ingress, LinkHandle)>; 3],
    now: &'static Cell<u64>,
    frames: &'static RefCell<Vec<Vec<u8>>>,
    backpressure: usize,
    unknown: bool,
    last_token: &'static Cell<Option<TxToken>>,
}
impl TxDriver for Driver {
    fn submit(&mut self, _link: LinkHandle, frame: &[u8], token: TxToken) -> DriverSubmit {
        self.last_token.set(Some(token));
        self.frames.borrow_mut().push(frame.to_vec());
        if self.unknown {
            return DriverSubmit::Unknown;
        }
        if self.backpressure > 0 {
            self.backpressure -= 1;
            return DriverSubmit::NotSubmitted;
        }
        let (peer, link) = self.peers[usize::from(frame[4] - 1)].unwrap();
        peer.rx_publish(
            link,
            frame,
            RxMeta {
                timestamp_us: self.now.get(),
                sender_discriminator: 1,
            },
        )
        .unwrap();
        DriverSubmit::Complete(TerminalOutcome::Success)
    }
    fn cancel(&mut self, _token: TxToken) -> DriverCancel {
        if self.unknown {
            DriverCancel::Unknown
        } else {
            DriverCancel::Cancelled
        }
    }
}
struct Receiver {
    values: RefCell<Vec<u8>>,
    close: Cell<Option<Handle>>,
    reply: Cell<Option<Handle>>,
    recursive: &'static Inbox<4>,
}
impl SampleHandler for Receiver {
    fn receive(&self, s: &Sample, commands: &dyn CommandPort) {
        self.values.borrow_mut().push(s.payload[0]);
        assert_eq!(s.source, source());
        assert_eq!(self.recursive.dispatch(s.received_us, 1), Err(Error::State));
        if let Some(h) = self.close.get() {
            commands.close_subscription(h).unwrap();
        }
        if let Some(h) = self.reply.get() {
            commands.publish(h, &[99]).unwrap();
        }
    }
}
struct Fixture {
    a: NativeRoot<4>,
    b: NativeRoot<4>,
    c: NativeRoot<4>,
    ia: &'static Ingress,
    ib: &'static Ingress,
    link_b: LinkHandle,
    now: &'static Cell<u64>,
    frames: &'static RefCell<Vec<Vec<u8>>>,
    last_token: &'static Cell<Option<TxToken>>,
}
fn fixture(queue: QueuePolicy, backpressure: usize) -> Fixture {
    fixture_payload(queue, backpressure, 1)
}
fn fixture_payload(queue: QueuePolicy, backpressure: usize, payload_bytes: u16) -> Fixture {
    fixture_mode(queue, backpressure, payload_bytes, false)
}
fn fixture_mode(
    queue: QueuePolicy,
    backpressure: usize,
    payload_bytes: u16,
    unknown: bool,
) -> Fixture {
    let ia = Box::leak(Box::new(Ingress::new(101).unwrap()));
    let ib = Box::leak(Box::new(Ingress::new(102).unwrap()));
    let ic = Box::leak(Box::new(Ingress::new(103).unwrap()));
    let aa = Box::leak(Box::new(Adapter::new(101, ia).unwrap()));
    let ab = Box::leak(Box::new(Adapter::new(102, ib).unwrap()));
    let ac = Box::leak(Box::new(Adapter::new(103, ic).unwrap()));
    let la = aa.open_link(0, 1, 128).unwrap();
    let lb = ab.open_link(0, 1, 128).unwrap();
    let lc = ac.open_link(0, 1, 128).unwrap();
    let now = Box::leak(Box::new(Cell::new(1)));
    let frames = Box::leak(Box::new(RefCell::new(Vec::new())));
    let last_token = Box::leak(Box::new(Cell::new(None)));
    let peers = [Some((&*ia, la)), Some((&*ib, lb)), Some((&*ic, lc))];
    let backend = |local, adapter: &'static mut Adapter, link| {
        let mut core = Core::new(
            config(local),
            QueueCapacities::new::<8>(2, 2, 2, 2).unwrap(),
        )
        .unwrap();
        for peer in 1..=3 {
            if peer != local {
                core.binding_add(addr(peer), binding(peer), u64::from(peer))
                    .unwrap();
            }
        }
        if local == 1 {
            for peer in 2..=3 {
                core.path_add(
                    StaticPath {
                        destination: addr(peer),
                        destination_binding_generation: binding(peer),
                        link,
                        path_frame_mtu: 128,
                    },
                    adapter,
                )
                .unwrap();
            }
        }
        let core = Box::leak(Box::new(core));
        let driver = Box::leak(Box::new(Driver {
            peers,
            now,
            frames,
            backpressure: if local == 1 { backpressure } else { 0 },
            unknown: local == 1 && unknown,
            last_token,
        }));
        Box::leak(Box::new(StaticBackend {
            core,
            adapter,
            driver,
        }))
    };
    let in_a = Box::leak(Box::new(Inbox::new()));
    let in_b = Box::leak(Box::new(Inbox::new()));
    let in_c = Box::leak(Box::new(Inbox::new()));
    let mut ca = channel(queue, true);
    ca.payload_bytes = payload_bytes;
    let mut cb = channel(queue, false);
    cb.payload_bytes = payload_bytes;
    let mut a = NativeRoot::new(backend(1, aa, la), in_a, 1, &[ca]).unwrap();
    let mut b = NativeRoot::new(backend(2, ab, lb), in_b, 2, &[cb]).unwrap();
    let mut c = NativeRoot::new(backend(3, ac, lc), in_c, 3, &[cb]).unwrap();
    a.start(1).unwrap();
    b.start(1).unwrap();
    c.start(1).unwrap();
    Fixture {
        a,
        b,
        c,
        ia,
        ib,
        link_b: lb,
        now,
        frames,
        last_token,
    }
}
fn receiver(inbox: &'static Inbox<4>) -> &'static Receiver {
    Box::leak(Box::new(Receiver {
        values: RefCell::new(Vec::new()),
        close: Cell::new(None),
        reply: Cell::new(None),
        recursive: inbox,
    }))
}
fn run(f: &mut Fixture, from: u64, to: u64) {
    for t in from..to {
        f.now.set(t);
        f.a.step(t, 8).unwrap();
        f.b.step(t, 8).unwrap();
        f.c.step(t, 8).unwrap();
    }
}
fn inject(f: &mut Fixture, seq: u32, value: u8, time: u64) {
    inject_at(f, seq, value, time, time);
}
fn inject_at(f: &mut Fixture, seq: u32, value: u8, time: u64, owner_now: u64) {
    let mut frame = [0x61, 0x40, 4, 1, 2, 0, 0x40, 0, 0, 0, 0, 0];
    frame[7..11].copy_from_slice(&seq.to_be_bytes());
    frame[11] = value;
    f.ib.rx_publish(
        f.link_b,
        &frame,
        RxMeta {
            timestamp_us: time,
            sender_discriminator: 1,
        },
    )
    .unwrap();
    f.b.step(owner_now, 8).unwrap();
}

#[test]
fn irq_after_step_snapshot_is_deferred_without_extending_lifetime() {
    let mut f = fixture(QueuePolicy::KeepLatest, 0);
    let r = receiver(f.b.commands());
    f.b.commands().subscribe(1, r).unwrap();
    inject_at(&mut f, 10, 1, 3, 2);
    assert_eq!(f.b.dispatch(2, 8), Ok(0));
    assert_eq!(f.b.dispatch(3, 8), Ok(1));
    inject_at(&mut f, 11, 2, 1000, 4);
    assert_eq!(f.b.dispatch(104, 8), Ok(0));
    assert_eq!(*r.values.borrow(), [1]);
}

#[test]
fn three_nodes_real_wire_copy_fanout_backpressure_and_stop() {
    let mut f = fixture(QueuePolicy::KeepLatest, 3);
    let rb = receiver(f.b.commands());
    let rc = receiver(f.c.commands());
    f.b.commands().subscribe(1, rb).unwrap();
    f.c.commands().subscribe(1, rc).unwrap();
    let p = f.a.commands().publisher(1).unwrap();
    let mut bytes = [7];
    f.a.commands().publish(p, &bytes).unwrap();
    bytes[0] = 99;
    assert_eq!(bytes, [99]);
    run(&mut f, 2, 30);
    assert_eq!(f.b.dispatch(30, 8), Ok(1));
    assert_eq!(f.c.dispatch(30, 8), Ok(1));
    assert_eq!(*rb.values.borrow(), [7]);
    assert_eq!(*rc.values.borrow(), [7]);
    let frames = f.frames.borrow();
    assert!(frames.len() >= 5);
    assert_eq!(frames[0], frames[1]);
    assert_eq!(frames[1], frames[2]);
    drop(frames);
    f.a.stop().unwrap();
    f.b.stop().unwrap();
    f.c.stop().unwrap();
    run(&mut f, 31, 40);
    assert!(f.a.quiescent() && f.b.quiescent() && f.c.quiescent());
    let _ = &f.ia;
}
#[test]
fn fifo_two_subscribers_budget_one_preserves_each_stream_order() {
    let mut f = fixture(QueuePolicy::Fifo, 0);
    let r0 = receiver(f.b.commands());
    let r1 = receiver(f.b.commands());
    f.b.commands().subscribe(1, r0).unwrap();
    f.b.commands().subscribe(1, r1).unwrap();
    inject(&mut f, 10, 1, 2);
    inject(&mut f, 11, 2, 3);
    for t in 4..8 {
        assert_eq!(f.b.dispatch(t, 1), Ok(1));
    }
    assert_eq!(*r0.values.borrow(), [1, 2]);
    assert_eq!(*r1.values.borrow(), [1, 2]);
}
#[test]
fn latest_old_event_expiry_fence_and_callback_self_close() {
    let mut f = fixture(QueuePolicy::KeepLatest, 0);
    let r = receiver(f.b.commands());
    let h = f.b.commands().subscribe(1, r).unwrap();
    inject(&mut f, 100, 10, 2);
    inject(&mut f, 99, 9, 3);
    assert_eq!(f.b.dispatch(4, 8), Ok(1));
    assert_eq!(*r.values.borrow(), [10]);
    inject(&mut f, 101, 11, 5);
    assert_eq!(f.b.dispatch(105, 8), Ok(0));
    inject(&mut f, 102, 12, 106);
    r.close.set(Some(h));
    assert_eq!(f.b.dispatch(107, 8), Ok(1));
    assert_eq!(f.b.commands().subscription_quiescent(h), Ok(true));
    let h2 = f.b.commands().subscribe(1, r).unwrap();
    assert_ne!(h, h2);
    inject(&mut f, 103, 13, 108);
    f.b.commands().source_fence(1, source()).unwrap();
    assert_eq!(f.b.dispatch(109, 8), Ok(0));
    inject(&mut f, 104, 14, 110);
    assert_eq!(f.b.dispatch(111, 8), Ok(0));
}
#[test]
fn command_pool_full_close_and_source_fence_are_independent() {
    let mut f = fixture(QueuePolicy::Fifo, 0);
    let p = f.a.commands().publisher(1).unwrap();
    for _ in 0..4 {
        f.a.commands().publish(p, &[1]).unwrap();
    }
    assert_eq!(f.a.commands().publish(p, &[1]), Err(Error::NoSpace));
    f.a.commands().close_publisher(p).unwrap();
    f.a.commands().source_fence(1, source()).unwrap();
    run(&mut f, 2, 6);
    assert!(f.frames.borrow().is_empty());
}

#[test]
fn unknown_holds_storage_until_exact_late_completion_and_never_restarts() {
    let mut f = fixture_mode(QueuePolicy::Fifo, 0, 1, true);
    let p = f.a.commands().publisher(1).unwrap();
    f.a.commands().publish(p, &[1]).unwrap();
    assert_eq!(f.a.step(2, 8), Err(Error::InDoubt));
    assert_eq!(f.a.commands().publish(p, &[2]), Err(Error::State));
    f.a.stop().unwrap();
    for t in 3..6 {
        f.a.step(t, 8).unwrap();
        assert!(!f.a.quiescent());
    }
    f.ia.tx_complete(f.last_token.get().unwrap(), TerminalOutcome::Success)
        .unwrap();
    f.a.step(6, 8).unwrap();
    assert!(f.a.quiescent());
    assert_eq!(f.a.start(7), Err(Error::State));
}

#[test]
fn callback_can_publish_deferred_local_work_without_borrowing_core() {
    let f = fixture(QueuePolicy::KeepLatest, 0);
    let r = receiver(f.a.commands());
    let h = f.a.commands().subscribe(1, r).unwrap();
    let p = f.a.commands().publisher(1).unwrap();
    r.reply.set(Some(p));
    f.a.commands().publish(p, &[1]).unwrap();
    assert_eq!(f.a.dispatch(2, 1), Ok(1));
    r.reply.set(None);
    r.close.set(Some(h));
    assert_eq!(f.a.dispatch(3, 1), Ok(1));
    assert_eq!(*r.values.borrow(), [1, 99]);
    assert_eq!(f.a.commands().subscription_quiescent(h), Ok(true));
}

#[path = "../examples/support/health_schema.rs"]
mod health_schema;
struct HealthReceiver {
    count: Cell<usize>,
}
impl SampleHandler for HealthReceiver {
    fn receive(&self, s: &Sample, _commands: &dyn CommandPort) {
        let h = health_schema::Health::decode(&s.payload[..usize::from(s.payload_bytes)]).unwrap();
        assert_eq!(h.voltage_mv, 24000);
        assert_eq!(h.current_ma, -1200);
        assert_eq!(h.fault_bits, 0x8000_0001);
        self.count.set(self.count.get() + 1);
    }
}
impl ucn_native::Message for health_schema::Health {
    const SCHEMA_ID: u32 = 0x4845_414c;
    const REVISION: u16 = 1;
    const WIRE_BYTES: u16 = 16;
    fn encode(&self, out: &mut [u8]) -> ucn_types::Result<()> {
        if out.len() != 16 {
            return Err(Error::Argument);
        }
        out.copy_from_slice(&(*self).encode());
        Ok(())
    }
}
/// Host-only software demo; Box is test wiring, never used by the `no_std` library.
/// # Panics
/// Fails immediately if the real software loopback, Codec or golden contract regresses.
pub fn run_health_demo() {
    let mut f = fixture_payload(QueuePolicy::KeepLatest, 0, 16);
    let b = Box::leak(Box::new(HealthReceiver {
        count: Cell::new(0),
    }));
    let c = Box::leak(Box::new(HealthReceiver {
        count: Cell::new(0),
    }));
    f.b.commands().subscribe(1, b).unwrap();
    f.c.commands().subscribe(1, c).unwrap();
    let p = ucn_native::Publisher::<health_schema::Health>::new(f.a.commands(), 1).unwrap();
    p.publish(&health_schema::Health {
        voltage_mv: 24000,
        current_ma: -1200,
        temperature_centi_c: 2534,
        mode: 2,
        flags: 1,
        fault_bits: 0x8000_0001,
    })
    .unwrap();
    run(&mut f, 2, 20);
    assert_eq!(f.b.dispatch(20, 8), Ok(1));
    assert_eq!(f.c.dispatch(20, 8), Ok(1));
    assert_eq!(b.count.get(), 1);
    assert_eq!(c.count.get(), 1);
    for (i, frame) in f.frames.borrow().iter().enumerate() {
        assert_eq!(frame.len(), 27);
        assert_eq!(
            &frame[..7],
            &[0x61, 0x40, 4, 1, u8::try_from(i + 2).unwrap(), 0, 0x40]
        );
        print!("NATIVE_FRAME_HEX=");
        for byte in frame {
            print!("{byte:02X}");
        }
        println!();
    }
    assert_eq!(f.frames.borrow().len(), 2);
    f.a.stop().unwrap();
    f.b.stop().unwrap();
    f.c.stop().unwrap();
    run(&mut f, 21, 25);
    assert!(f.a.quiescent() && f.b.quiescent() && f.c.quiescent());
    println!("health: B=1 C=1 header=11 payload=16");
}
#[test]
fn health_typed_three_node_golden() {
    run_health_demo();
}
