//! Ordinary send -> automatic authenticated discovery -> opaque relay -> application pull.
//! 普通发送经自动认证寻路、不透明中继到业务读取；成功路径不手工调用Security接口。
use super::*;
mod contexts;
mod neighbors;
mod renewal;

type Root<'g> = RoutedNode<'g, 3, 12, 6, 2, 2, 256, 2, 4, 8, 2>;
pub(super) fn root<'g>(node: &mut Node<'g>) -> (Root<'g>, Option<DatagramEndpointHandle>) {
    root_hops(node, 8)
}
fn root_hops<'g>(node: &mut Node<'g>, hops: u8) -> (Root<'g>, Option<DatagramEndpointHandle>) {
    let remote = if node.id == 1 {
        None
    } else {
        Some(2 - node.id)
    };
    root_for(node, hops, remote)
}
fn root_for<'g>(
    node: &mut Node<'g>,
    hops: u8,
    remote: Option<usize>,
) -> (Root<'g>, Option<DatagramEndpointHandle>) {
    node.driver.frames.clear();
    node.driver.tokens.clear();
    node.driver.peer_locators.clear();
    let mut root = Root::new(
        route_config(80 + u32::try_from(node.id).unwrap()),
        node.routes.config(),
        100,
        DatagramConfig {
            lifetime_us: 1500,
            max_hops: HopLimit::new(hops).unwrap(),
            required_capability_bits: 0,
        },
        node.identity.callback_gate(),
    )
    .unwrap();
    for remote in 0usize..3 {
        if remote.abs_diff(node.id) != 1 {
            continue;
        }
        let (peer, cap) = peer_reference(node, remote, 20);
        root.register_neighbor(
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
    let endpoint = remote.map(|remote| {
        let peer = node.peers[remote].unwrap();
        root.register_endpoint(
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
        .unwrap()
    });
    (root, endpoint)
}
fn tick(root: &mut Root<'_>, node: &mut Node<'_>, now: u64) -> RoutedStep {
    root.step(
        &node.identity,
        &mut node.security,
        &node.caps,
        &mut node.adapter,
        &mut node.crypto,
        &mut node.counters,
        &mut node.driver,
        &mut SecurityPacketWorkspace::<320>::new(),
        now,
        24,
    )
    .unwrap()
}
fn sent(node: &mut Node<'_>) -> Vec<(u32, Vec<u8>)> {
    assert_eq!(node.driver.frames.len(), node.driver.peer_locators.len());
    node.driver.tokens.clear();
    core::mem::take(&mut node.driver.peer_locators)
        .into_iter()
        .zip(core::mem::take(&mut node.driver.frames))
        .collect()
}
fn deliver(to: &Node<'_>, source: usize, packet: &[u8], now: u64) {
    to.ingress
        .rx_publish(
            to.link,
            packet,
            RxMeta {
                timestamp_us: now,
                sender_discriminator: 100 + u32::try_from(source).unwrap(),
            },
        )
        .unwrap();
}
fn request(
    root: &mut Root<'_>,
    node: &Node<'_>,
    endpoint: DatagramEndpointHandle,
    bytes: &[u8],
    deadline: u64,
) -> DatagramTxHandle {
    root.send(
        endpoint,
        bytes,
        DatagramSendOptions {
            traffic_class: TrafficClass::Q2,
            deadline_us: deadline,
            tracked: true,
        },
        &node.identity,
        &node.security,
        21,
    )
    .unwrap()
    .unwrap()
}
fn exchange(mtu: usize, backpressure: usize) {
    with_mesh_candidates(mtu, true, |a, b, c| {
        let (mut ar, endpoint) = root(a);
        let (mut br, _) = root(b);
        let (mut cr, reverse_endpoint) = root(c);
        let receipt = request(&mut ar, a, endpoint.unwrap(), b"automatic mesh", 1400);
        let mut rejected = 0;
        let mut origin_packets = Vec::new();
        let mut rejected_packets = Vec::new();
        let mut relay_packets = Vec::new();
        let mut final_capture = None;
        let mut delivered = false;
        for now in 22..1300 {
            a.driver.data_submit = Some(if rejected < backpressure {
                DriverSubmit::NotSubmitted
            } else {
                DriverSubmit::Complete(TerminalOutcome::Success)
            });
            tick(&mut ar, a, now);
            for (target, packet) in sent(a) {
                assert_eq!(target, 101, "must use B locator, not final C address");
                if a.driver.last_submit == DriverSubmit::NotSubmitted {
                    rejected += 1;
                    rejected_packets.push(packet);
                    continue;
                }
                if ucn_wire::CommonHeader::decode(&packet).unwrap().contract == HeaderContract::C1 {
                    origin_packets.push(packet.clone());
                }
                deliver(b, 0, &packet, now);
                tick(&mut br, b, now);
            }
            tick(&mut br, b, now);
            for (target, packet) in sent(b) {
                if target == 100 {
                    deliver(a, 1, &packet, now);
                    tick(&mut ar, a, now);
                } else {
                    assert_eq!(target, 102);
                    if ucn_wire::CommonHeader::decode(&packet).unwrap().contract
                        == HeaderContract::C1
                    {
                        relay_packets.push(packet.clone());
                        final_capture = Some(now - 1);
                    }
                    deliver(c, 1, &packet, now - 1);
                    tick(&mut cr, c, now);
                }
            }
            tick(&mut cr, c, now);
            for (target, packet) in sent(c) {
                assert_eq!(target, 101);
                deliver(b, 2, &packet, now);
                tick(&mut br, b, now);
            }
            let mut out = [0xa5; 64];
            let mut short = [0xa5; 1];
            match cr.receive_datagram(&c.identity, &c.security, &mut short, now) {
                Err(Error::NotFound | Error::NoSpace) => assert_eq!(short, [0xa5; 1]),
                other => panic!("short output unexpectedly changed message {other:?}"),
            }
            match cr.receive_datagram(&c.identity, &c.security, &mut out, now) {
                Ok(message) => {
                    assert_eq!(&out[..message.bytes], b"automatic mesh");
                    assert_eq!(message.origin, fixed_binding(0));
                    assert_eq!(message.origin_sequence, 1);
                    assert_eq!(Some(message.receive_time_us), final_capture);
                    delivered = true;
                    break;
                }
                Err(Error::NotFound) => {}
                other => panic!("unexpected pull {other:?}"),
            }
        }
        assert!(
            delivered,
            "source={:?}, a={:?}, b={:?}, c={:?}",
            ar.query(receipt),
            ar.stats(),
            br.stats(),
            cr.stats()
        );
        assert_eq!(origin_packets.len(), 1);
        assert_eq!(relay_packets.len(), 1);
        let incoming = &origin_packets[0];
        let outgoing = &relay_packets[0];
        for packet in rejected_packets {
            assert_eq!(
                &packet, incoming,
                "NOT_SUBMITTED retries exact same immutable data"
            );
        }
        assert_eq!(
            &incoming[3..incoming.len() - 16],
            &outgoing[3..outgoing.len() - 16],
            "origin bytes/ciphertext unchanged"
        );
        assert_eq!(incoming[2], outgoing[2] + 1, "only TTL and H1 change");
        let mut duplicate = [0xa5; 64];
        assert_eq!(
            cr.receive_datagram(&c.identity, &c.security, &mut duplicate, 1300),
            Err(Error::NotFound)
        );
        assert_eq!(duplicate, [0xa5; 64]);
        let rejected_before = cr.stats().rx_rejected;
        deliver(c, 1, outgoing, 1301);
        tick(&mut cr, c, 1301);
        assert_eq!(
            cr.stats().rx_rejected,
            rejected_before + 1,
            "exact physical replay rejected"
        );
        assert_eq!(
            cr.receive_datagram(&c.identity, &c.security, &mut duplicate, 1301),
            Err(Error::NotFound)
        );
        assert_eq!(duplicate, [0xa5; 64]);
        assert_eq!(br.stats().relay_accepted, 1);
        assert_eq!(cr.stats().app_pulled, 1);
        assert_eq!(rejected, backpressure);
        let reverse_receipt = cr
            .send(
                reverse_endpoint.unwrap(),
                b"reverse",
                DatagramSendOptions {
                    traffic_class: TrafficClass::Q3,
                    deadline_us: 2000,
                    tracked: true,
                },
                &c.identity,
                &c.security,
                1302,
            )
            .unwrap()
            .unwrap();
        let mut reverse = false;
        for now in 1303..1900 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
            let mut out = [0xa5; 64];
            if let Ok(message) = ar.receive_datagram(&a.identity, &a.security, &mut out, now) {
                assert_eq!(&out[..message.bytes], b"reverse");
                assert_eq!(message.origin, fixed_binding(2));
                reverse = true;
                break;
            }
        }
        assert!(
            reverse,
            "reverse state={:?}, A={:?}, B={:?}, C={:?}",
            cr.query(reverse_receipt),
            ar.stats(),
            br.stats(),
            cr.stats()
        );
    });
}

/// Wire routing in the Host harness only; each received frame still enters real atomic RX.
fn network(
    a: &mut Node<'_>,
    b: &mut Node<'_>,
    c: &mut Node<'_>,
    ar: &mut Root<'_>,
    br: &mut Root<'_>,
    cr: &mut Root<'_>,
    now: u64,
) {
    for (target, packet) in sent(a) {
        assert_eq!(target, 101);
        deliver(b, 0, &packet, now);
        tick(br, b, now);
    }
    for (target, packet) in sent(b) {
        if target == 100 {
            deliver(a, 1, &packet, now);
            tick(ar, a, now);
        } else {
            assert_eq!(target, 102);
            deliver(c, 1, &packet, now);
            tick(cr, c, now);
        }
    }
    for (target, packet) in sent(c) {
        assert_eq!(target, 101);
        deliver(b, 2, &packet, now);
        tick(br, b, now);
    }
}

#[test]
fn unknown_data_submission_stays_owned_after_stop_until_exact_late_completion() {
    with_mesh_candidates(256, true, |a, b, c| {
        let (mut ar, endpoint) = root(a);
        let (mut br, _) = root(b);
        let (mut cr, _) = root(c);
        a.driver.data_submit = Some(DriverSubmit::Unknown);
        let receipt = request(&mut ar, a, endpoint.unwrap(), b"unknown", 1400);
        let mut token = None;
        for now in 22..1200 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            if ar.query(receipt).unwrap().physical_pending
                && a.driver.last_submit == DriverSubmit::Unknown
            {
                token = a.driver.tokens.last().copied();
                break;
            }
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        let token = token.expect("real Unknown data submit");
        assert_eq!(
            ar.query(receipt).unwrap().outcome,
            Some(DatagramOutcome::InDoubt)
        );
        assert_eq!(ar.forget(receipt), Err(Error::State));
        ar.stop(&mut a.adapter).unwrap();
        let count = a.driver.frames.len();
        for now in 1500..1600 {
            tick(&mut ar, a, now);
        }
        assert!(!ar.quiescent(&a.adapter));
        assert_eq!(a.driver.frames.len(), count);
        a.adapter.reopen_link(a.link, 4, 256).unwrap();
        assert_eq!(
            a.adapter.tx_retire(token),
            Err(Error::InDoubt),
            "reopen is not physical completion"
        );
        a.ingress
            .tx_complete(token, TerminalOutcome::Success)
            .unwrap();
        for now in 1600..1800 {
            tick(&mut ar, a, now);
        }
        assert!(
            ar.quiescent(&a.adapter),
            "counts={:?} view={:?} adapter={:?} rx={}",
            ar.counts(),
            ar.query(receipt),
            a.adapter.tx_view(token),
            a.adapter.rx_is_quiescent()
        );
        assert_eq!(
            ar.query(receipt).unwrap().outcome,
            Some(DatagramOutcome::InDoubt)
        );
        assert!(!ar.query(receipt).unwrap().physical_pending);
        ar.forget(receipt).unwrap();
    });
}

#[test]
fn ttl_exhaustion_does_not_forward_or_deliver() {
    with_mesh_candidates(256, true, |a, b, c| {
        let (mut ar, endpoint) = root_hops(a, 1);
        let (mut br, _) = root(b);
        let (mut cr, _) = root(c);
        request(&mut ar, a, endpoint.unwrap(), b"ttl", 1400);
        for now in 22..1100 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        assert_eq!(br.stats().relay_accepted, 0);
        assert!(br.stats().rx_rejected > 0);
        let mut out = [0xa5; 64];
        assert_eq!(
            cr.receive_datagram(&c.identity, &c.security, &mut out, 1100),
            Err(Error::NotFound)
        );
        assert_eq!(out, [0xa5; 64]);
    });
}
#[test]
fn automatic_three_nodes() {
    exchange(256, 0);
}
#[test]
fn automatic_three_nodes_with_fragmented_route_control() {
    exchange(127, 0);
}
#[test]
fn temporary_backpressure_preserves_send() {
    exchange(256, 2);
}

#[test]
fn unreachable_deadline_and_policy_rejection_do_not_burn_origin_counter() {
    with_mesh_candidates(256, true, |a, _, _| {
        let (mut ar, endpoint) = root(a);
        let endpoint = endpoint.unwrap();
        let peer = a.peers[2].unwrap();
        let before = a.counters.origin(peer.session).unwrap().preview().unwrap();
        assert_eq!(
            ar.send(
                endpoint,
                b"x",
                DatagramSendOptions {
                    traffic_class: TrafficClass::Q0,
                    deadline_us: 100,
                    tracked: true
                },
                &a.identity,
                &a.security,
                21
            ),
            Err(Error::Policy)
        );
        let receipt = request(&mut ar, a, endpoint, b"no route", 100);
        for now in 22..160 {
            tick(&mut ar, a, now);
            sent(a);
        }
        assert_eq!(
            ar.query(receipt).unwrap().outcome,
            Some(DatagramOutcome::Failed(Error::Timeout))
        );
        assert_eq!(
            a.counters.origin(peer.session).unwrap().preview().unwrap(),
            before
        );
        ar.forget(receipt).unwrap();
        ar.stop(&mut a.adapter).unwrap();
        for now in 160..300 {
            tick(&mut ar, a, now);
        }
        assert!(ar.quiescent(&a.adapter));
    });
}

#[test]
fn foreign_adapter_rejects_before_claiming_its_raw_rx() {
    with_mesh_candidates(256, true, |a, _, _| {
        let (mut ar, _) = root(a);
        let ingress = MeshIngress::new(999).unwrap();
        let mut adapter = MeshAdapter::new(999, &ingress).unwrap();
        let link = adapter.open_link(0, 1, 256).unwrap();
        adapter.try_set_rx_enabled(true).unwrap();
        ingress
            .rx_publish(
                link,
                &[1, 2, 3],
                RxMeta {
                    timestamp_us: 21,
                    sender_discriminator: 101,
                },
            )
            .unwrap();
        assert!(matches!(
            ar.step(
                &a.identity,
                &mut a.security,
                &a.caps,
                &mut adapter,
                &mut a.crypto,
                &mut a.counters,
                &mut a.driver,
                &mut SecurityPacketWorkspace::<320>::new(),
                21,
                1
            ),
            Err(Error::State)
        ));
        let mut raw = [0xa5; 256];
        let claim = adapter.rx_claim(&mut raw).unwrap();
        assert_eq!(&raw[..claim.frame_bytes], &[1, 2, 3]);
        adapter.rx_retire(claim.token).unwrap();
    });
}

#[test]
fn bounded_data_backpressure_never_reseals_or_reports_delivery() {
    with_mesh_candidates(256, true, |a, b, c| {
        let (mut ar, endpoint) = root(a);
        let (mut br, _) = root(b);
        let (mut cr, _) = root(c);
        a.driver.data_submit = Some(DriverSubmit::NotSubmitted);
        let receipt = request(&mut ar, a, endpoint.unwrap(), b"busy", 900);
        let mut copies = Vec::new();
        for now in 22..1000 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            for (target, packet) in sent(a) {
                assert_eq!(target, 101);
                if ucn_wire::CommonHeader::decode(&packet).unwrap().contract == HeaderContract::C1 {
                    copies.push(packet);
                } else {
                    deliver(b, 0, &packet, now);
                    tick(&mut br, b, now);
                }
            }
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        assert_eq!(copies.len(), 5, "fixed physical attempt limit");
        assert!(copies.iter().all(|v| v == &copies[0]));
        assert_eq!(
            ar.query(receipt).unwrap().outcome,
            Some(DatagramOutcome::Failed(Error::Exhausted))
        );
        assert!(!ar.query(receipt).unwrap().physical_pending);
        assert_eq!(
            c.counters
                .origin(c.peers[0].unwrap().session)
                .unwrap()
                .preview()
                .unwrap()
                .get(),
            1
        );
        assert_eq!(
            a.counters
                .origin(a.peers[2].unwrap().session)
                .unwrap()
                .preview()
                .unwrap()
                .get(),
            2
        );
        assert_eq!(cr.stats().app_pulled, 0);
        assert_eq!(br.stats().relay_accepted, 0);
    });
}

#[test]
fn unread_application_slots_are_never_silently_evicted() {
    with_mesh_candidates(256, true, |a, b, c| {
        let (mut ar, endpoint) = root(a);
        let (mut br, _) = root(b);
        let (mut cr, _) = root(c);
        for body in [b"one", b"two", b"tri"] {
            request(&mut ar, a, endpoint.unwrap(), body, 1400);
        }
        for now in 22..800 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        assert_eq!(cr.counts().2, 2);
        assert_eq!(cr.stats().rx_accepted, 2);
        assert!(cr.stats().rx_rejected > 0);
        let mut messages = Vec::new();
        for now in [800, 801] {
            let mut out = [0xa5; 64];
            let message = cr
                .receive_datagram(&c.identity, &c.security, &mut out, now)
                .unwrap();
            messages.push(out[..message.bytes].to_vec());
        }
        assert_eq!(messages, vec![b"one".to_vec(), b"two".to_vec()]);
        assert_eq!(cr.stats().app_pulled, 2);
        assert_eq!(
            ar.stats().local_tx_success,
            3,
            "local success is not remote delivery"
        );
    });
}

#[test]
fn source_queue_capacity_rejects_before_counters_or_driver() {
    with_mesh_candidates(256, true, |a, _, _| {
        let (mut ar, endpoint) = root(a);
        let endpoint = endpoint.unwrap();
        for _ in 0..6 {
            request(&mut ar, a, endpoint, b"queued", 1000);
        }
        let counts = ar.counts();
        let counter = a
            .counters
            .origin(a.peers[2].unwrap().session)
            .unwrap()
            .preview()
            .unwrap();
        assert_eq!(
            ar.send(
                endpoint,
                b"overflow",
                DatagramSendOptions {
                    traffic_class: TrafficClass::Q2,
                    deadline_us: 1000,
                    tracked: true
                },
                &a.identity,
                &a.security,
                21
            ),
            Err(Error::NoSpace)
        );
        assert_eq!(ar.counts(), counts);
        assert!(a.driver.frames.is_empty());
        assert_eq!(ar.stats().source_accepted, 6);
        assert_eq!(
            a.counters
                .origin(a.peers[2].unwrap().session)
                .unwrap()
                .preview()
                .unwrap(),
            counter
        );
    });
}
