//! Native facade exercises the actual routed Coordinator, not a fake Backend.
use super::*;
mod security {
    pub(super) type Crypto = super::ReauthCrypto;
}
#[path = "../../native_backend_support/test_cipher.rs"]
mod test_cipher;
use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use ucn_native::{
    Channel, CommandPort, Destination, Inbox, NativeRoot, QueuePolicy, Sample, SampleHandler,
    SecurityMinimum, Source,
};

#[test]
fn native_routed_api_uses_current_endpoint_and_original_receive_metadata() {
    with_mesh_candidates(256, true, |a, _, _| {
        let (routed, endpoint) = data_node::root(a);
        let endpoint = endpoint.unwrap();
        a.adapter.try_set_rx_enabled(false).unwrap();
        let config = [RoutedNativeEndpoint {
            endpoint,
            principal_digest: 33,
        }];
        let resources = NativeRoutedResources {
            identity: &a.identity,
            security: &mut a.security,
            capabilities: &a.caps,
            adapter: &mut a.adapter,
            crypto: &mut a.crypto,
            counters: &mut a.counters,
            driver: &mut a.driver,
            scratch: &mut SecurityPacketWorkspace::<320>::new(),
        };
        let mut routed = routed;
        let backend = NativeRoutedBackend::new(&mut routed, resources, &config, 11, 20).unwrap();
        assert_eq!(
            ucn_native::Backend::principal(&backend, fixed_binding(2).address),
            Some(33)
        );
    });
}

#[test]
fn native_local_channel_must_fit_an_actual_outbound_endpoint_policy() {
    with_mesh_candidates(256, true, |a, _, _| {
        let mut routed = RoutedNode::<3, 12, 6, 2, 2, 256, 2, 4, 8, 2>::new(
            route_config(480),
            a.routes.config(),
            100,
            DatagramConfig {
                lifetime_us: 1500,
                max_hops: HopLimit::new(8).unwrap(),
                required_capability_bits: 0,
            },
            a.identity.callback_gate(),
        )
        .unwrap();
        let peer = a.peers[2].unwrap();
        let endpoint = routed
            .register_endpoint(
                peer.session,
                CurrentFacts {
                    now_us: 20,
                    ..peer.facts
                },
                peer.fingerprint,
                [a.bindings[0], a.bindings[2]],
                DatagramEndpointPolicy {
                    service: ServiceId::new(0x40).unwrap(),
                    traffic_ceiling: TrafficClass::Q3,
                    max_payload_bytes: 2,
                },
                &a.identity,
                &a.security,
                20,
            )
            .unwrap();
        a.adapter.try_set_rx_enabled(false).unwrap();
        let config = [RoutedNativeEndpoint {
            endpoint,
            principal_digest: 3,
        }];
        let mut scratch = SecurityPacketWorkspace::<320>::new();
        let io = NativeRoutedResources {
            identity: &a.identity,
            security: &mut a.security,
            capabilities: &a.caps,
            adapter: &mut a.adapter,
            crypto: &mut a.crypto,
            counters: &mut a.counters,
            driver: &mut a.driver,
            scratch: &mut scratch,
        };
        let mut backend = NativeRoutedBackend::new(&mut routed, io, &config, 1, 20).unwrap();
        let mut c = channel(0);
        c.sources = [None; 16];
        c.sources[0] = Some(Source {
            address: fixed_binding(0).address,
            binding: fixed_binding(0).generation,
            principal_digest: 1,
        });
        c.targets = [None; 8];
        let inbox = Box::leak(Box::new(Inbox::<4>::new()));
        assert_eq!(
            ucn_native::Backend::install(&mut backend, &[c], inbox),
            Err(Error::Policy)
        );
        ucn_native::Backend::start(&mut backend, 21).unwrap();
        assert!(
            ucn_native::Backend::validate(
                &backend,
                ucn_native::BackendScope::LOCAL,
                c.sources[0].unwrap(),
                &c,
                22
            )
            .is_err()
        );
        c.traffic_class = TrafficClass::Q3;
        c.payload_bytes = 2;
        assert_eq!(
            ucn_native::Backend::validate(
                &backend,
                ucn_native::BackendScope::LOCAL,
                c.sources[0].unwrap(),
                &c,
                22
            ),
            Ok([0; 16])
        );
    });
}

fn permanent_nodes(encrypted: bool) -> [&'static mut Node<'static>; 3] {
    let mut nodes = Vec::new();
    for id in 0..3 {
        with_store(|store, media| {
            let gate = Box::leak(Box::new(
                CallbackGate::new(101 + u32::try_from(id).unwrap()).unwrap(),
            ));
            let ingress = Box::leak(Box::new(
                MeshIngress::new(201 + u32::try_from(id).unwrap()).unwrap(),
            ));
            let domains = Box::leak(Box::new([DOMAINS[1], DOMAINS[2], DOMAINS[3]]));
            let mut cs = candidates(id);
            if id != 1 {
                let mut end = c1_path::endpoint_candidate(id, 2 - id);
                if encrypted {
                    end.origin_level = SecurityLevel::Confidential;
                    end.origin_tx.suite = SuiteId::OriginAes128Gcm;
                    end.origin_rx.suite = SuiteId::OriginAes128Gcm;
                }
                cs.push(end);
            }
            let ds = Box::leak(rules(&cs).into_boxed_slice());
            let acl = Box::leak(acl(&cs).into_boxed_slice());
            nodes.push(Box::leak(Box::new(node(
                id, 256, gate, ingress, domains, &cs, ds, acl, store, media,
            ))));
        });
    }
    let [a, b, c]: [_; 3] = nodes.try_into().ok().unwrap();
    advertise(a, b);
    advertise(b, a);
    advertise(b, c);
    advertise(c, b);
    [a, b, c]
}

struct MediaDriver {
    sent: &'static Outbox,
    backpressure: &'static Cell<usize>,
}
type Outbox = RefCell<Vec<(u32, Vec<u8>)>>;
type NativeFixture = (
    NativeRoot,
    &'static Received,
    &'static Outbox,
    Option<&'static Received>,
);
impl TxDriver for MediaDriver {
    fn submit(&mut self, _: LinkHandle, _: &[u8], _: TxToken) -> DriverSubmit {
        panic!("routed TX must select a precise neighbour locator")
    }
    fn cancel(&mut self, _: TxToken) -> DriverCancel {
        DriverCancel::Cancelled
    }
}
impl PeerTxDriver for MediaDriver {
    fn submit_peer(&mut self, _: LinkHandle, to: u32, frame: &[u8], _: TxToken) -> DriverSubmit {
        assert!((100..103).contains(&to));
        if frame[0] & 15 == 1 && self.backpressure.get() > 0 {
            self.backpressure.set(self.backpressure.get() - 1);
            return DriverSubmit::NotSubmitted;
        }
        self.sent.borrow_mut().push((to, frame.to_vec()));
        DriverSubmit::Complete(TerminalOutcome::Success)
    }
}
struct Received(RefCell<Vec<Sample>>);
impl SampleHandler for Received {
    fn receive(&self, s: &Sample, _: &dyn CommandPort) {
        if !s.local {
            self.0.borrow_mut().push(*s);
        }
    }
}
fn channel(id: usize) -> Channel {
    let mut c = Channel {
        id: 1,
        service: ServiceId::new(0x40).unwrap(),
        schema_id: 1,
        schema_revision: 1,
        payload_bytes: 3,
        traffic_class: TrafficClass::Q2,
        queue: QueuePolicy::KeepLatest,
        security: SecurityMinimum::OriginAuth,
        max_local_age_us: 10000,
        sources: [None; 16],
        targets: [None; 8],
    };
    for n in 0..3 {
        let b = fixed_binding(n);
        c.sources[n] = Some(Source {
            address: b.address,
            binding: b.generation,
            principal_digest: u64::try_from(n + 1).unwrap(),
        });
    }
    for (i, n) in (0..3).filter(|n| *n != id).enumerate() {
        let b = fixed_binding(n);
        c.targets[i] = Some(Destination {
            address: b.address,
            binding: b.generation,
        });
    }
    c
}
fn native_root(node: &'static mut Node<'static>, delayed: bool) -> NativeFixture {
    // Every Peer has one Origin Endpoint for this Service. Direct peers reuse
    // the actual H1 Session; AC uses a different end-to-end H0 Session.
    let mut handles = Vec::new();
    let mut routed = RoutedNode::<3, 12, 6, 2, 2, 256, 2, 4, 8, 2>::new(
        route_config(180 + u32::try_from(node.id).unwrap()),
        node.routes.config(),
        100,
        DatagramConfig {
            lifetime_us: 1500,
            max_hops: HopLimit::new(8).unwrap(),
            required_capability_bits: 0,
        },
        node.identity.callback_gate(),
    )
    .unwrap();
    for remote in 0usize..3 {
        if remote.abs_diff(node.id) == 1 {
            let (peer, cap) = peer_reference(node, remote, 20);
            routed
                .register_neighbor(
                    peer,
                    [node.bindings[node.id], node.bindings[remote]],
                    &node.identity,
                    &node.security,
                    &node.caps,
                    cap,
                    &node.adapter,
                    1,
                    100 + u32::try_from(remote).unwrap(),
                    20,
                )
                .unwrap();
        }
        if remote != node.id {
            let peer = node.peers[remote].unwrap();
            let h = routed
                .register_endpoint(
                    peer.session,
                    CurrentFacts {
                        now_us: 20,
                        ..peer.facts
                    },
                    peer.fingerprint,
                    [node.bindings[node.id], node.bindings[remote]],
                    DatagramEndpointPolicy {
                        service: ServiceId::new(0x40).unwrap(),
                        traffic_ceiling: TrafficClass::Q1,
                        max_payload_bytes: 64,
                    },
                    &node.identity,
                    &node.security,
                    20,
                )
                .unwrap();
            handles.push(RoutedNativeEndpoint {
                endpoint: h,
                principal_digest: u64::try_from(remote + 1).unwrap(),
            });
        }
    }
    node.adapter.try_set_rx_enabled(false).unwrap();
    let sent = Box::leak(Box::new(RefCell::new(Vec::new())));
    let driver = Box::leak(Box::new(MediaDriver {
        sent,
        backpressure: Box::leak(Box::new(Cell::new(2))),
    }));
    let id = node.id;
    let resources = NativeRoutedResources {
        identity: &node.identity,
        security: &mut node.security,
        capabilities: &node.caps,
        adapter: &mut node.adapter,
        crypto: Box::leak(Box::new(test_cipher::TestCrypto::new(initial(
            fixed_binding(id),
            fixed_binding((id + 1) % 3),
            id < (id + 1) % 3,
        )))),
        counters: &mut node.counters,
        driver,
        scratch: Box::leak(Box::new(SecurityPacketWorkspace::<320>::new())),
    };
    let backend = Box::leak(Box::new(
        NativeRoutedBackend::new(
            Box::leak(Box::new(routed)),
            resources,
            Box::leak(handles.into_boxed_slice()),
            u64::try_from(id + 1).unwrap(),
            20,
        )
        .unwrap(),
    ));
    let inbox = Box::leak(Box::new(Inbox::<4>::new()));
    let mut root = NativeRoot::new(
        backend,
        inbox,
        700 + u32::try_from(id).unwrap(),
        &[channel(id)],
    )
    .unwrap();
    let receiver = Box::leak(Box::new(Received(RefCell::new(Vec::new()))));
    inbox.subscribe(1, receiver).unwrap();
    let retained: Option<&'static Received> = if delayed {
        let next = Box::leak(Box::new(Received(RefCell::new(Vec::new()))));
        inbox.subscribe(1, next).unwrap();
        Some(next)
    } else {
        None
    };
    root.start(30).unwrap();
    (root, receiver, sent, retained)
}

#[test]
fn actual_native_fanout_multiple_origins_and_multihop_never_relabels_the_relay() {
    run_native(false, false);
}
#[test]
fn actual_native_o2_multihop_preserves_ciphertext_with_a_test_provider() {
    run_native(true, false);
}
#[test]
fn queued_native_multihop_sample_is_rejected_after_actual_identity_parent_expiry() {
    run_native(false, true);
}
fn run_native(encrypted: bool, delayed: bool) {
    let nodes = permanent_nodes(encrypted);
    let ingresses = nodes.each_ref().map(|n| (n.ingress, n.link));
    let [
        (mut a, ah, at, aprobe),
        (mut b, bh, bt, bprobe),
        (mut c, ch, ct, cprobe),
    ] = nodes.map(|node| native_root(node, delayed));
    let pa = a.commands().publisher(1).unwrap();
    let pc = c.commands().publisher(1).unwrap();
    a.commands().publish(pa, b"abc").unwrap();
    c.commands().publish(pc, b"cba").unwrap();
    let mut pending = VecDeque::new();
    let mut packets = Vec::new();
    for now in 31..1200 {
        for (i, (root, out)) in [(&mut a, at), (&mut b, bt), (&mut c, ct)]
            .into_iter()
            .enumerate()
        {
            root.step(now, 48).unwrap();
            for (to, packet) in core::mem::take(&mut *out.borrow_mut()) {
                let to = usize::try_from(to - 100).unwrap();
                packets.push((i, to, packet.clone()));
                pending.push_back((i, to, packet));
            }
        }
        for _ in 0..pending.len() {
            let (from, to, packet) = pending.pop_front().unwrap();
            match ingresses[to].0.rx_publish(
                ingresses[to].1,
                &packet,
                RxMeta {
                    timestamp_us: now,
                    sender_discriminator: 100 + u32::try_from(from).unwrap(),
                },
            ) {
                Ok(_) => {}
                Err(Error::NoSpace) => pending.push_back((from, to, packet)),
                other => panic!("unexpected carrier RX {other:?}"),
            }
        }
        if !delayed {
            a.dispatch(now, 8).unwrap();
            b.dispatch(now, 8).unwrap();
            c.dispatch(now, 8).unwrap();
        } else if ch.0.borrow().is_empty() {
            // Deliver the exact authenticated event to its first subscriber,
            // keeping the second recipient pending across parent expiry.
            c.dispatch(now, 1).unwrap();
        }
        if delayed && ch.0.borrow().len() == 1 {
            break;
        }
        if ah.0.borrow().len() == 1 && bh.0.borrow().len() == 2 && ch.0.borrow().len() == 1 {
            break;
        }
    }
    if delayed {
        assert!(
            packets
                .iter()
                .any(|(from, to, p)| *from == 1 && *to == 2 && p[0] == 0x61)
        );
        // Local age10000 and Session expiry remain valid; the actual Identity
        // Binding lease has expired. This must not be a facade-age false positive.
        for root in [&a, &b, &c] {
            assert_eq!(root.dispatch(4000, 8).unwrap(), 0);
        }
        assert!(ah.0.borrow().is_empty() && bh.0.borrow().is_empty());
        assert_eq!(ch.0.borrow().len(), 1);
        assert_eq!(ch.0.borrow()[0].principal, Some(fixed_binding(0).principal));
        for receiver in [aprobe, bprobe, cprobe].into_iter().flatten() {
            assert!(receiver.0.borrow().is_empty());
        }
        for root in [&mut a, &mut b, &mut c] {
            root.stop().unwrap();
            root.step(4001, 64).unwrap();
            assert!(root.quiescent());
        }
        return;
    }
    assert_eq!(
        (
            ah.0.borrow().len(),
            bh.0.borrow().len(),
            ch.0.borrow().len()
        ),
        (1, 2, 1)
    );
    for (destination, handler, expected) in
        [(0, ah, vec![2]), (1, bh, vec![0, 2]), (2, ch, vec![0])]
    {
        let mut origins = handler
            .0
            .borrow()
            .iter()
            .map(|s| {
                let n = usize::try_from(s.source.address.get() - 7).unwrap();
                assert_eq!(s.principal, Some(fixed_binding(n).principal));
                assert_eq!(s.origin_sequence, 1);
                assert_eq!(
                    s.protection,
                    Some(if encrypted && destination != 1 {
                        SecurityMinimum::Encrypted
                    } else {
                        SecurityMinimum::OriginAuth
                    })
                );
                assert_eq!(&s.payload[..3], if n == 0 { b"abc" } else { b"cba" });
                n
            })
            .collect::<Vec<_>>();
        origins.sort_unstable();
        assert_eq!(origins, expected);
    }
    let ac = packets
        .iter()
        .find(|(from, _, p)| *from == 0 && p[0] == 0x61 && p[5..7] == [0, 9])
        .unwrap();
    let bc = packets
        .iter()
        .find(|(from, to, p)| *from == 1 && *to == 2 && p[0] == 0x61 && p[3..5] == [0, 7])
        .unwrap();
    assert_eq!(&ac.2[3..ac.2.len() - 16], &bc.2[3..bc.2.len() - 16]);
    assert_eq!(ac.2[2], bc.2[2] + 1);
    assert_eq!(ac.2.len(), 13 + 3 + 16 + 16);
    for root in [&mut a, &mut b, &mut c] {
        root.stop().unwrap();
        root.step(1300, 64).unwrap();
        assert!(root.quiescent());
    }
}
