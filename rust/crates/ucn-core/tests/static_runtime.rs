//! RUST-03 最小静态通信、四级调度、Replay 与停止对账回归。

use std::cell::{Cell, RefCell};
use std::vec::Vec;

use ucn_adapter::{
    AdapterEventIngress, AdapterOwner, DriverCancel, DriverSubmit, LinkHandle, RxMeta,
    TerminalOutcome, TxDriver, TxToken,
};
use ucn_core::{
    CoreNode, EndpointDisposition, EndpointHandler, EndpointMessage, Lifecycle, NodeConfig,
    PublishOptions, QueueCapacities, StaticPath, Target,
};
use ucn_types::{
    AddressWidth, BindingGeneration, DeliveryGuarantee, Error, HeaderContract, HopLimit,
    HopProfile, InteractionRole, NodeAddress, OriginSecurity, OriginSequence, PayloadKind,
    ServiceId, TrafficClass,
};
use ucn_wire::{C1Frame, CommonHeader, encode_c1_o0_h0};

type Ingress<const TX: usize, const RX: usize> = AdapterEventIngress<1, TX, RX, 64>;
type Adapter<'a, const TX: usize, const RX: usize> = AdapterOwner<'a, 1, TX, RX, 64>;
type Node<'a, const TX: usize> = CoreNode<'a, 2, 2, 2, TX, 64>;

const SERVICE: u16 = 0x1201;

fn address(value: u32) -> NodeAddress {
    NodeAddress::new(value, AddressWidth::A0).expect("valid A0 address")
}

fn generation(value: u32) -> BindingGeneration {
    BindingGeneration::active(value).expect("active generation")
}

fn service() -> ServiceId {
    ServiceId::new(SERVICE).expect("service")
}

fn options(class: TrafficClass) -> PublishOptions {
    PublishOptions {
        traffic_class: class,
        hop_limit: HopLimit::new(1).expect("hop"),
        absolute_deadline_us: 0,
    }
}

fn config(node_instance: u32, adapter_instance: u32, local: u32, binding: u32) -> NodeConfig {
    NodeConfig {
        node_instance,
        adapter_instance,
        address_width: AddressWidth::A0,
        local_address: address(local),
        local_binding_generation: generation(binding),
        trusted_o0_network: true,
    }
}

fn prepare_node<'a, const CORE_TX: usize, const ADAPTER_TX: usize, const ADAPTER_RX: usize>(
    node_config: NodeConfig,
    peer: u32,
    peer_binding: u32,
    queues: QueueCapacities,
    adapter: &Adapter<'_, ADAPTER_TX, ADAPTER_RX>,
    link: LinkHandle,
) -> Node<'a, CORE_TX> {
    let mut node = Node::new(node_config, queues).expect("node");
    node.binding_add(address(peer), generation(peer_binding))
        .expect("peer binding");
    node.path_add(
        StaticPath {
            destination: address(peer),
            destination_binding_generation: generation(peer_binding),
            link,
            path_frame_mtu: 64,
        },
        adapter,
    )
    .expect("static path");
    node
}

#[derive(Default)]
struct Receiver {
    calls: Cell<u32>,
    last_source: Cell<u32>,
    last_binding: Cell<u32>,
    last_timestamp: Cell<u64>,
    payloads: RefCell<Vec<Vec<u8>>>,
    disposition: Cell<Option<EndpointDisposition>>,
}

impl EndpointHandler for Receiver {
    fn receive(&self, message: EndpointMessage<'_>) -> EndpointDisposition {
        self.calls.set(self.calls.get() + 1);
        self.last_source.set(message.source.get());
        self.last_binding
            .set(message.source_binding_generation.get());
        self.last_timestamp.set(message.receive_timestamp_us);
        self.payloads.borrow_mut().push(message.payload.to_vec());
        self.disposition
            .get()
            .unwrap_or(EndpointDisposition::Accept)
    }
}

struct LoopDriver<
    'a,
    const LOCAL_TX: usize,
    const LOCAL_RX: usize,
    const PEER_TX: usize,
    const PEER_RX: usize,
> {
    local: &'a Ingress<LOCAL_TX, LOCAL_RX>,
    peer: Option<(&'a Ingress<PEER_TX, PEER_RX>, LinkHandle)>,
    not_submitted: usize,
    submits: usize,
    cancels: usize,
    classes: Vec<u8>,
    frames: Vec<Vec<u8>>,
    tokens: Vec<TxToken>,
}

impl<'a, const LOCAL_TX: usize, const LOCAL_RX: usize, const PEER_TX: usize, const PEER_RX: usize>
    LoopDriver<'a, LOCAL_TX, LOCAL_RX, PEER_TX, PEER_RX>
{
    fn new(local: &'a Ingress<LOCAL_TX, LOCAL_RX>) -> Self {
        Self {
            local,
            peer: None,
            not_submitted: 0,
            submits: 0,
            cancels: 0,
            classes: Vec::new(),
            frames: Vec::new(),
            tokens: Vec::new(),
        }
    }
}

impl<const LOCAL_TX: usize, const LOCAL_RX: usize, const PEER_TX: usize, const PEER_RX: usize>
    TxDriver for LoopDriver<'_, LOCAL_TX, LOCAL_RX, PEER_TX, PEER_RX>
{
    fn submit(&mut self, _link: LinkHandle, frame: &[u8], token: TxToken) -> DriverSubmit {
        self.submits += 1;
        self.classes.push(frame[1] >> 6);
        self.frames.push(frame.to_vec());
        self.tokens.push(token);
        if self.not_submitted != 0 {
            self.not_submitted -= 1;
            return DriverSubmit::NotSubmitted;
        }
        if let Some((peer, peer_link)) = self.peer {
            peer.rx_publish(
                peer_link,
                frame,
                RxMeta {
                    timestamp_us: 12_345,
                    sender_discriminator: 77,
                },
            )
            .expect("peer RX publication");
        }
        self.local
            .tx_complete(token, TerminalOutcome::Success)
            .expect("synchronous completion");
        DriverSubmit::Submitted
    }

    fn cancel(&mut self, _token: TxToken) -> DriverCancel {
        self.cancels += 1;
        DriverCancel::Cancelled
    }
}

#[test]
fn two_nodes_complete_static_c1_tx_rx_and_reject_exact_replay() {
    let ingress_a = Ingress::<2, 2>::new(101).expect("ingress A");
    let ingress_b = Ingress::<2, 2>::new(102).expect("ingress B");
    let mut adapter_a = Adapter::new(101, &ingress_a).expect("adapter A");
    let mut adapter_b = Adapter::new(102, &ingress_b).expect("adapter B");
    let link_a = adapter_a.open_link(0, 1, 64).expect("link A");
    let link_b = adapter_b.open_link(0, 1, 64).expect("link B");
    let queues = QueueCapacities::new::<8>(2, 2, 2, 2).expect("queues");
    let receiver = Receiver::default();
    let mut node_a =
        prepare_node::<8, 2, 2>(config(1, 101, 1, 10), 2, 20, queues, &adapter_a, link_a);
    let mut node_b =
        prepare_node::<8, 2, 2>(config(2, 102, 2, 20), 1, 10, queues, &adapter_b, link_b);
    node_b.endpoint_add(service(), &receiver).expect("endpoint");
    node_a.start(1, &mut adapter_a).expect("start A");
    node_b.start(1, &mut adapter_b).expect("start B");

    let receipt = node_a
        .publish(
            Target {
                address: address(2),
                binding_generation: generation(20),
                service_id: service(),
            },
            &[0xCA, 0xFE],
            options(TrafficClass::Q1),
            &adapter_a,
        )
        .expect("publish");
    assert_eq!(receipt.origin_sequence.get(), 1);

    let mut driver_a = LoopDriver::<2, 2, 2, 2>::new(&ingress_a);
    driver_a.peer = Some((&ingress_b, link_b));
    assert_eq!(
        node_a
            .step(2, 1, &mut adapter_a, &mut driver_a)
            .expect("TX step")
            .work_done,
        1
    );
    let mut driver_b = LoopDriver::<2, 2, 2, 2>::new(&ingress_b);
    assert_eq!(
        node_b
            .step(2, 1, &mut adapter_b, &mut driver_b)
            .expect("RX step")
            .work_done,
        1
    );
    assert_eq!(receiver.calls.get(), 1);
    assert_eq!(&*receiver.payloads.borrow(), &[vec![0xCA, 0xFE]]);
    assert_eq!(receiver.last_source.get(), 1);
    assert_eq!(receiver.last_binding.get(), 10);
    assert_eq!(receiver.last_timestamp.get(), 12_345);
    assert_eq!(node_a.stats().tx_completed, 1);
    assert_eq!(node_b.stats().rx_delivered, 1);

    ingress_b
        .rx_publish(
            link_b,
            &driver_a.frames[0],
            RxMeta {
                timestamp_us: 13_000,
                sender_discriminator: 78,
            },
        )
        .expect("duplicate publication");
    node_b
        .step(3, 1, &mut adapter_b, &mut driver_b)
        .expect("duplicate step");
    assert_eq!(receiver.calls.get(), 1);
    assert_eq!(node_b.stats().replay_rejected, 1);
}

#[test]
fn saturated_queues_follow_the_exact_six_three_two_one_schedule() {
    let ingress = Ingress::<1, 1>::new(201).expect("ingress");
    let mut adapter = Adapter::new(201, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<12>(6, 3, 2, 1).expect("queues");
    let mut node = prepare_node::<12, 1, 1>(config(3, 201, 1, 10), 2, 20, queues, &adapter, link);
    node.start(0, &mut adapter).expect("start");
    let target = Target {
        address: address(2),
        binding_generation: generation(20),
        service_id: service(),
    };
    for (class, count) in [
        (TrafficClass::Q0, 6),
        (TrafficClass::Q1, 3),
        (TrafficClass::Q2, 2),
        (TrafficClass::Q3, 1),
    ] {
        for marker in 0..count {
            node.publish(target, &[class as u8, marker], options(class), &adapter)
                .expect("queue fill");
        }
    }
    let mut driver = LoopDriver::<1, 1, 1, 1>::new(&ingress);
    let result = node
        .step(1, 12, &mut adapter, &mut driver)
        .expect("scheduled step");
    assert_eq!(result.work_done, 12);
    assert_eq!(driver.classes, vec![0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2]);
    assert_eq!(node.stats().tx_completed, 12);
}

#[test]
fn continuously_replenished_queues_remain_fair_for_twelve_thousand_sends() {
    let ingress = Ingress::<1, 1>::new(202).expect("ingress");
    let mut adapter = Adapter::new(202, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let mut node = prepare_node::<4, 1, 1>(config(11, 202, 1, 10), 2, 20, queues, &adapter, link);
    node.start(0, &mut adapter).expect("start");
    let target = Target {
        address: address(2),
        binding_generation: generation(20),
        service_id: service(),
    };
    for class in [
        TrafficClass::Q0,
        TrafficClass::Q1,
        TrafficClass::Q2,
        TrafficClass::Q3,
    ] {
        node.publish(target, &[class as u8], options(class), &adapter)
            .expect("initial fill");
    }
    let mut driver = LoopDriver::<1, 1, 1, 1>::new(&ingress);
    let mut counts = [0_u32; 4];
    for now_us in 1..=12_000 {
        assert_eq!(
            node.step(now_us, 1, &mut adapter, &mut driver)
                .expect("fair step")
                .work_done,
            1
        );
        let class = match *driver.classes.last().expect("one class per step") {
            0 => TrafficClass::Q0,
            1 => TrafficClass::Q1,
            2 => TrafficClass::Q2,
            3 => TrafficClass::Q3,
            _ => panic!("invalid class"),
        };
        counts[class as usize] += 1;
        node.publish(target, &[class as u8], options(class), &adapter)
            .expect("continuous replenishment");
    }
    assert_eq!(counts, [6_000, 3_000, 2_000, 1_000]);
}

#[test]
fn queue_failure_never_consumes_sequence_or_evicts_an_existing_frame() {
    let ingress = Ingress::<1, 1>::new(301).expect("ingress");
    let mut adapter = Adapter::new(301, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let mut node = prepare_node::<4, 1, 1>(config(4, 301, 1, 10), 2, 20, queues, &adapter, link);
    node.start(10, &mut adapter).expect("start");
    let target = Target {
        address: address(2),
        binding_generation: generation(20),
        service_id: service(),
    };
    assert_eq!(
        node.publish(target, &[1], options(TrafficClass::Q0), &adapter)
            .expect("first")
            .origin_sequence
            .get(),
        1
    );
    assert_eq!(
        node.publish(target, &[2], options(TrafficClass::Q0), &adapter),
        Err(Error::NoSpace)
    );
    let oversized = [0xA5; 60];
    assert_eq!(
        node.publish(target, &oversized, options(TrafficClass::Q1), &adapter),
        Err(Error::NoSpace)
    );
    assert_eq!(
        node.publish(
            Target {
                address: address(3),
                binding_generation: generation(30),
                service_id: service(),
            },
            &[3],
            options(TrafficClass::Q1),
            &adapter,
        ),
        Err(Error::NotFound)
    );

    let mut driver = LoopDriver::<1, 1, 1, 1>::new(&ingress);
    node.step(11, 1, &mut adapter, &mut driver)
        .expect("drain first");
    assert_eq!(driver.frames[0].last(), Some(&1));
    assert_eq!(
        node.publish(target, &[4], options(TrafficClass::Q0), &adapter)
            .expect("after failures")
            .origin_sequence
            .get(),
        2
    );
}

#[test]
fn stale_static_link_is_rejected_before_sequence_allocation() {
    let ingress = Ingress::<1, 1>::new(302).expect("ingress");
    let mut adapter = Adapter::new(302, &ingress).expect("adapter");
    let old_link = adapter.open_link(0, 1, 64).expect("old link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let mut node = Node::<4>::new(config(12, 302, 1, 10), queues).expect("node");
    node.binding_add(address(2), generation(20))
        .expect("binding");
    let old_path = node
        .path_add(
            StaticPath {
                destination: address(2),
                destination_binding_generation: generation(20),
                link: old_link,
                path_frame_mtu: 64,
            },
            &adapter,
        )
        .expect("old path");
    node.start(0, &mut adapter).expect("start");
    let new_link = adapter.reopen_link(old_link, 2, 64).expect("reopen");
    let target = Target {
        address: address(2),
        binding_generation: generation(20),
        service_id: service(),
    };
    assert_eq!(
        node.publish(target, &[0x31], options(TrafficClass::Q0), &adapter),
        Err(Error::NotFound)
    );

    node.stop(&mut adapter).expect("stop");
    let mut driver = LoopDriver::<1, 1, 1, 1>::new(&ingress);
    assert_eq!(
        node.step(1, 1, &mut adapter, &mut driver)
            .expect("quiesce")
            .lifecycle,
        Lifecycle::Quiescent
    );
    node.path_remove(old_path).expect("remove stale path");
    node.path_add(
        StaticPath {
            destination: address(2),
            destination_binding_generation: generation(20),
            link: new_link,
            path_frame_mtu: 64,
        },
        &adapter,
    )
    .expect("replacement path");
    node.start(2, &mut adapter).expect("restart");
    assert_eq!(
        node.publish(target, &[0x32], options(TrafficClass::Q0), &adapter)
            .expect("first admitted sequence")
            .origin_sequence
            .get(),
        1,
        "stale-link rejection must not consume the sequence"
    );
}

#[test]
fn endpoint_callback_remains_live_beyond_the_old_u16_boundary() {
    let ingress = Ingress::<1, 1>::new(303).expect("ingress");
    let mut adapter = Adapter::new(303, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let receiver = Receiver::default();
    let mut node = prepare_node::<4, 1, 1>(config(13, 303, 2, 20), 1, 10, queues, &adapter, link);
    node.endpoint_add(service(), &receiver).expect("endpoint");
    node.start(0, &mut adapter).expect("start");
    let mut driver = LoopDriver::<1, 1, 1, 1>::new(&ingress);
    for sequence in 1..=65_537 {
        let frame = encode_frame(1, 2, SERVICE, sequence, &[0x5A]);
        ingress
            .rx_publish(
                link,
                &frame,
                RxMeta {
                    timestamp_us: u64::from(sequence),
                    sender_discriminator: 1,
                },
            )
            .expect("publish callback stress frame");
        node.step(u64::from(sequence), 1, &mut adapter, &mut driver)
            .expect("callback stress step");
    }
    assert_eq!(receiver.calls.get(), 65_537);
    assert_eq!(node.lifecycle(), Lifecycle::Running);
}

#[test]
fn not_submitted_retries_the_exact_token_and_deadline_cancels_it() {
    let ingress = Ingress::<1, 1>::new(401).expect("ingress");
    let mut adapter = Adapter::new(401, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let mut node = prepare_node::<4, 1, 1>(config(5, 401, 1, 10), 2, 20, queues, &adapter, link);
    node.start(0, &mut adapter).expect("start");
    let target = Target {
        address: address(2),
        binding_generation: generation(20),
        service_id: service(),
    };
    node.publish(target, &[7], options(TrafficClass::Q2), &adapter)
        .expect("publish retry");
    let mut driver = LoopDriver::<1, 1, 1, 1>::new(&ingress);
    driver.not_submitted = 2;
    for now in 1..=3 {
        node.step(now, 1, &mut adapter, &mut driver)
            .expect("retry step");
    }
    assert_eq!(driver.submits, 3);
    assert!(driver.tokens.windows(2).all(|pair| pair[0] == pair[1]));
    assert!(driver.frames.windows(2).all(|pair| pair[0] == pair[1]));
    assert_eq!(node.stats().tx_completed, 1);

    let mut deadline_options = options(TrafficClass::Q3);
    deadline_options.absolute_deadline_us = 10;
    node.publish(target, &[8], deadline_options, &adapter)
        .expect("deadline publish");
    driver.not_submitted = usize::MAX;
    node.step(4, 1, &mut adapter, &mut driver)
        .expect("first backpressure");
    node.step(10, 1, &mut adapter, &mut driver)
        .expect("deadline cancel");
    assert_eq!(driver.cancels, 0, "NOT_SUBMITTED cancels locally");
    assert_eq!(node.stats().tx_failed, 1);
}

#[test]
fn malformed_unknown_and_wrong_destination_frames_never_reach_endpoint() {
    let ingress = Ingress::<1, 1>::new(501).expect("ingress");
    let mut adapter = Adapter::new(501, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let receiver = Receiver::default();
    let mut node = prepare_node::<4, 1, 1>(config(6, 501, 2, 20), 1, 10, queues, &adapter, link);
    node.endpoint_add(service(), &receiver).expect("endpoint");
    node.start(0, &mut adapter).expect("start");
    let mut driver = LoopDriver::<1, 1, 1, 1>::new(&ingress);

    ingress
        .rx_publish(
            link,
            &[0x00],
            RxMeta {
                timestamp_us: 1,
                sender_discriminator: 1,
            },
        )
        .expect("malformed publish");
    node.step(1, 1, &mut adapter, &mut driver)
        .expect("malformed step");

    for (source, destination, service_id, sequence) in
        [(3, 2, SERVICE, 1), (1, 3, SERVICE, 2), (1, 2, 0x1202, 3)]
    {
        let bytes = encode_frame(source, destination, service_id, sequence, &[0x55]);
        ingress
            .rx_publish(
                link,
                &bytes,
                RxMeta {
                    timestamp_us: u64::from(sequence),
                    sender_discriminator: 1,
                },
            )
            .expect("invalid semantic publication");
        node.step(u64::from(sequence) + 1, 1, &mut adapter, &mut driver)
            .expect("semantic step");
    }
    assert_eq!(receiver.calls.get(), 0);
    assert_eq!(node.stats().malformed, 1);
    assert_eq!(node.stats().rx_dropped, 4);

    let valid = encode_frame(1, 2, SERVICE, 3, &[0xAA]);
    ingress
        .rx_publish(
            link,
            &valid,
            RxMeta {
                timestamp_us: 9,
                sender_discriminator: 1,
            },
        )
        .expect("slot must be reusable");
    node.step(9, 1, &mut adapter, &mut driver)
        .expect("valid step");
    assert_eq!(receiver.calls.get(), 1);
}

#[test]
fn adapter_backpressure_preserves_queued_work_until_capacity_returns() {
    let ingress = Ingress::<1, 1>::new(551).expect("ingress");
    let mut adapter = Adapter::new(551, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let mut node = prepare_node::<4, 1, 1>(config(8, 551, 1, 10), 2, 20, queues, &adapter, link);
    node.start(0, &mut adapter).expect("start");
    let target = Target {
        address: address(2),
        binding_generation: generation(20),
        service_id: service(),
    };
    node.publish(target, &[0], options(TrafficClass::Q0), &adapter)
        .expect("first");
    node.publish(target, &[1], options(TrafficClass::Q1), &adapter)
        .expect("second");
    node.publish(target, &[2], options(TrafficClass::Q2), &adapter)
        .expect("third");
    let mut driver = PendingDriver {
        submits: 0,
        last_token: None,
        classes: Vec::new(),
    };
    assert_eq!(
        node.step(1, 1, &mut adapter, &mut driver)
            .expect("submit first")
            .work_done,
        1
    );
    assert_eq!(
        node.step(2, 1, &mut adapter, &mut driver)
            .expect("adapter capacity is transient")
            .work_done,
        0
    );
    let first = driver.last_token.expect("first token");
    ingress
        .tx_complete(first, TerminalOutcome::Success)
        .expect("async completion");
    node.step(3, 1, &mut adapter, &mut driver)
        .expect("retire first");
    node.step(4, 1, &mut adapter, &mut driver)
        .expect("submit second");
    assert_eq!(driver.submits, 2);
    assert_eq!(driver.classes, vec![0, 1]);
    assert_eq!(
        node.stats().tx_completed,
        1,
        "second token is still pending"
    );
}

struct PendingDriver {
    submits: usize,
    last_token: Option<TxToken>,
    classes: Vec<u8>,
}

impl TxDriver for PendingDriver {
    fn submit(&mut self, _link: LinkHandle, frame: &[u8], token: TxToken) -> DriverSubmit {
        self.submits += 1;
        self.last_token = Some(token);
        self.classes.push(frame[1] >> 6);
        DriverSubmit::Submitted
    }

    fn cancel(&mut self, _token: TxToken) -> DriverCancel {
        DriverCancel::NotCancelled
    }
}

#[test]
fn indeterminate_driver_side_effect_faults_the_core_owner() {
    let ingress = Ingress::<1, 1>::new(552).expect("ingress");
    let mut adapter = Adapter::new(552, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let mut node = prepare_node::<4, 1, 1>(config(9, 552, 1, 10), 2, 20, queues, &adapter, link);
    node.start(0, &mut adapter).expect("start");
    node.publish(
        Target {
            address: address(2),
            binding_generation: generation(20),
            service_id: service(),
        },
        &[0xEE],
        options(TrafficClass::Q0),
        &adapter,
    )
    .expect("publish");
    let mut driver = UnknownDriver;
    assert_eq!(
        node.step(1, 1, &mut adapter, &mut driver),
        Err(Error::InDoubt)
    );
    assert_eq!(node.lifecycle(), Lifecycle::Fault);
}

struct UnknownDriver;

impl TxDriver for UnknownDriver {
    fn submit(&mut self, _link: LinkHandle, _frame: &[u8], _token: TxToken) -> DriverSubmit {
        DriverSubmit::Unknown
    }

    fn cancel(&mut self, _token: TxToken) -> DriverCancel {
        DriverCancel::Unknown
    }
}

#[test]
fn stop_is_bounded_rejects_new_work_and_reaches_quiescence() {
    let ingress = Ingress::<1, 1>::new(601).expect("ingress");
    let mut adapter = Adapter::new(601, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let mut node = prepare_node::<4, 1, 1>(config(7, 601, 1, 10), 2, 20, queues, &adapter, link);
    node.start(0, &mut adapter).expect("start");
    let target = Target {
        address: address(2),
        binding_generation: generation(20),
        service_id: service(),
    };
    node.publish(target, &[1], options(TrafficClass::Q0), &adapter)
        .expect("q0");
    node.publish(target, &[2], options(TrafficClass::Q1), &adapter)
        .expect("q1");
    node.stop(&mut adapter).expect("stop");
    assert_eq!(node.lifecycle(), Lifecycle::Stopping);
    assert_eq!(
        node.publish(target, &[3], options(TrafficClass::Q2), &adapter),
        Err(Error::State)
    );
    assert_eq!(
        ingress.rx_publish(
            link,
            &[1],
            RxMeta {
                timestamp_us: 1,
                sender_discriminator: 1,
            },
        ),
        Err(Error::State)
    );
    let mut driver = LoopDriver::<1, 1, 1, 1>::new(&ingress);
    for now in 1..=4 {
        let result = node
            .step(now, 1, &mut adapter, &mut driver)
            .expect("bounded stop step");
        assert!(result.work_done <= 1);
        if result.lifecycle == Lifecycle::Quiescent {
            break;
        }
    }
    assert_eq!(node.lifecycle(), Lifecycle::Quiescent);
    assert_eq!(node.stats().tx_failed, 2);
    node.start(5, &mut adapter).expect("restart from quiescent");
    assert_eq!(node.lifecycle(), Lifecycle::Running);
}

#[test]
fn endpoint_path_handles_are_exact_and_profile_storage_is_static() {
    let ingress = Ingress::<1, 1>::new(701).expect("ingress");
    let mut adapter = Adapter::new(701, &ingress).expect("adapter");
    let link = adapter.open_link(0, 1, 64).expect("link");
    let queues = QueueCapacities::new::<4>(1, 1, 1, 1).expect("queues");
    let receiver = Receiver::default();
    let mut node = Node::<4>::new(config(10, 701, 1, 10), queues).expect("node");
    node.binding_add(address(2), generation(20))
        .expect("binding");

    let first_endpoint = node.endpoint_add(service(), &receiver).expect("endpoint");
    node.endpoint_remove(first_endpoint)
        .expect("remove endpoint");
    assert_eq!(node.endpoint_remove(first_endpoint), Err(Error::NotFound));
    let second_endpoint = node.endpoint_add(service(), &receiver).expect("re-add");
    assert_ne!(first_endpoint, second_endpoint);

    let path = StaticPath {
        destination: address(2),
        destination_binding_generation: generation(20),
        link,
        path_frame_mtu: 64,
    };
    let first_path = node.path_add(path, &adapter).expect("path");
    node.path_remove(first_path).expect("remove path");
    assert_eq!(node.path_remove(first_path), Err(Error::NotFound));
    let second_path = node.path_add(path, &adapter).expect("re-add path");
    assert_ne!(first_path, second_path);

    assert!(!std::mem::needs_drop::<Node<'static, 4>>());
    assert!(
        std::mem::size_of::<ucn_core::FullCoreNode<'static>>()
            > std::mem::size_of::<ucn_core::NanoCoreNode<'static>>()
    );
    assert_eq!(
        ucn_core::nano_queue_capacities(),
        QueueCapacities::new::<16>(4, 4, 4, 4).unwrap()
    );
    assert_eq!(
        ucn_core::lite_queue_capacities(),
        QueueCapacities::new::<48>(8, 8, 16, 16).unwrap()
    );
    assert_eq!(
        ucn_core::full_queue_capacities(),
        QueueCapacities::new::<96>(16, 16, 32, 32).unwrap()
    );
    println!(
        "RUST03_CORE_BYTES nano={} lite={} full={}",
        std::mem::size_of::<ucn_core::NanoCoreNode<'static>>(),
        std::mem::size_of::<ucn_core::LiteCoreNode<'static>>(),
        std::mem::size_of::<ucn_core::FullCoreNode<'static>>()
    );
}

fn encode_frame(
    source: u32,
    destination: u32,
    service_id: u16,
    sequence: u32,
    payload: &[u8],
) -> Vec<u8> {
    let frame = C1Frame {
        common: CommonHeader {
            contract: HeaderContract::C1,
            traffic_class: TrafficClass::Q1,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::OneWay,
            payload_kind: PayloadKind::Data,
            origin_security: OriginSecurity::O0,
            hop_limit: HopLimit::new(1).expect("hop"),
        },
        source: address(source),
        destination: address(destination),
        service_id: ServiceId::new(service_id).expect("service"),
        origin_sequence: OriginSequence::new(sequence).expect("sequence"),
        payload,
    };
    let mut bytes = vec![0_u8; 64];
    let written =
        encode_c1_o0_h0(&frame, AddressWidth::A0, HopProfile::H0, &mut bytes).expect("encode");
    bytes.truncate(written);
    bytes
}
