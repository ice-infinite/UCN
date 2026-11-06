//! Actual Foundation successors + authenticated raw Advertise + existing Root hot adoption.
//! 真实Foundation后继、原始认证Advertise与原Root热接纳，不伪造ACTIVE/Capability事实。
use super::*;

fn setup<'g>(
    node: &mut Node<'g>,
    remote: usize,
) -> (Root<'g>, RoutePeerHandle, DatagramEndpointHandle) {
    let mut root = Root::new(
        route_config(80 + u32::try_from(node.id).unwrap()),
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
    let mut selected = None;
    for other in 0usize..3 {
        if other.abs_diff(node.id) != 1 {
            continue;
        }
        let (peer, capability) = peer_reference(node, other, 20);
        let h = root
            .register_neighbor(
                peer,
                [node.bindings[node.id], node.bindings[other]],
                &node.identity,
                &node.security,
                &node.caps,
                capability,
                &node.adapter,
                1,
                100 + u32::try_from(other).unwrap(),
                20,
            )
            .unwrap();
        if other == remote {
            selected = Some(h);
        }
    }
    let endpoint = contexts::register(&mut root, node, remote);
    logs(node);
    (root, selected.unwrap(), endpoint)
}
fn logs(node: &mut Node<'_>) {
    node.driver.frames.clear();
    node.driver.tokens.clear();
    node.driver.peer_locators.clear();
}
fn candidate(local: usize, remote: usize, generation: u32) -> HandshakeCandidate {
    let mut c = candidates(local)
        .into_iter()
        .find(|c| c.peer == fixed_binding(remote))
        .unwrap();
    c.session_generation = PeerSessionGeneration::new(generation).unwrap();
    let increment = 10 * u16::try_from(generation).unwrap();
    for key in [&mut c.origin_tx, &mut c.origin_rx] {
        key.key_id = KeyId::new(key.key_id.get() + increment).unwrap();
        key.key_generation = generation;
    }
    for key in [&mut c.hop_tx, &mut c.hop_rx].into_iter().flatten() {
        key.key_id = KeyId::new(key.key_id.get() + increment).unwrap();
        key.key_generation = generation;
    }
    c.transcript_digest = [u8::try_from(generation + 20).unwrap(); 32];
    c
}
fn renew(
    node: &mut Node<'_>,
    remote: usize,
    generation: u32,
    p: &mut Store<'_>,
    m: &mut MeshMedia,
    now: u64,
) -> JournalPeer {
    let mut c = candidate(node.id, remote, generation);
    c.link_generation = LinkInstanceGeneration::new(node.link.instance_generation()).unwrap();
    let session = activate_at(&mut node.security, p, m, c, DOMAINS[4 + remote], now);
    let peer = JournalPeer {
        session,
        facts: CurrentFacts {
            now_us: now + 1,
            local: c.local,
            peer: c.peer,
            link_generation: c.link_generation,
            policy_generation: 1,
        },
        fingerprint: c.origin_fingerprint,
        link: node.link,
    };
    node.peers[remote] = Some(peer);
    node.counters
        .0
        .push((session, Counter(1), Hop(1), OriginCounter(1)));
    peer
}
fn adopt(
    root: &mut Root<'_>,
    old: RoutePeerHandle,
    node: &Node<'_>,
    remote: usize,
    reference: PeerCapabilityRef,
    now: u64,
) -> Result<RoutePeerHandle> {
    let mut peer = node.peers[remote].unwrap();
    peer.facts.now_us = now;
    root.rebind_neighbor(
        old,
        peer,
        [node.bindings[node.id], node.bindings[remote]],
        &node.identity,
        &node.security,
        &node.caps,
        reference,
        &node.adapter,
        1,
        100 + u32::try_from(remote).unwrap(),
        now,
    )
}
fn endpoint(
    root: &mut Root<'_>,
    old: DatagramEndpointHandle,
    node: &Node<'_>,
    remote: usize,
    now: u64,
) -> DatagramEndpointHandle {
    let mut peer = node.peers[remote].unwrap();
    peer.facts.now_us = now;
    root.rebind_endpoint(
        old,
        peer.session,
        peer.facts,
        peer.fingerprint,
        [node.bindings[node.id], node.bindings[remote]],
        &node.identity,
        &node.security,
        now,
    )
    .unwrap()
}
fn send_now(
    root: &mut Root<'_>,
    node: &Node<'_>,
    ep: DatagramEndpointHandle,
    now: u64,
) -> DatagramTxHandle {
    root.send(
        ep,
        b"hot",
        DatagramSendOptions {
            traffic_class: TrafficClass::Q2,
            deadline_us: now + 1000,
            tracked: true,
        },
        &node.identity,
        &node.security,
        now,
    )
    .unwrap()
    .unwrap()
}

#[test]
fn durable_h1_successors_and_authenticated_capabilities_resume_direct_on_the_same_roots() {
    with_mesh_all_stores(256, false, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, ah, ae) = setup(a, 1);
        let (mut br, bh, be) = setup(b, 0);
        let ac = peer_reference(a, 1, 20).1;
        let bc = peer_reference(b, 0, 20).1;
        assert!(!ar.neighbor_is_drained(ah).unwrap());
        ar.fence_neighbor(ah, &mut a.security, 21).unwrap();
        br.fence_neighbor(bh, &mut b.security, 21).unwrap();
        assert!(ar.neighbor_is_drained(ah).unwrap());
        assert!(br.neighbor_is_drained(bh).unwrap());
        assert!(ar.origin_is_drained(ae).unwrap());
        renew(a, 1, 2, ap, am, 22);
        renew(b, 0, 2, bp, bm, 22);
        assert_eq!(adopt(&mut ar, ah, a, 1, ac, 24), Err(Error::State));
        let ac2 = advertise_at(b, a, 25, Some(ac));
        let bc2 = advertise_at(a, b, 25, Some(bc));
        assert_eq!(a.caps.peer_get(ac, 25), Err(Error::NotFound));
        let mut peer = a.peers[1].unwrap();
        peer.facts.now_us = 26;
        assert_eq!(
            ar.register_neighbor(
                peer,
                [a.bindings[0], a.bindings[1]],
                &a.identity,
                &a.security,
                &a.caps,
                ac2,
                &a.adapter,
                1,
                101,
                26
            ),
            Err(Error::State),
            "valid fresh parents still cannot cold-register around the fenced slot"
        );
        let ah2 = adopt(&mut ar, ah, a, 1, ac2, 26).unwrap();
        let bh2 = adopt(&mut br, bh, b, 0, bc2, 26).unwrap();
        assert_ne!(ah2, ah);
        assert_ne!(bh2, bh);
        assert_eq!(ar.neighbor_is_drained(ah), Err(Error::NotFound));
        assert!(
            send_now_result(&mut ar, a, ae, 27).is_err(),
            "neighbour adoption does not silently rebind a Service"
        );
        let ae2 = endpoint(&mut ar, ae, a, 1, 28);
        let _be2 = endpoint(&mut br, be, b, 0, 28);
        logs(a);
        logs(b);
        let tx = send_now(&mut ar, a, ae2, 29);
        for now in 30..80 {
            tick(&mut ar, a, now);
            for (to, packet) in sent(a) {
                assert_eq!(to, 101);
                deliver(b, 0, &packet, now);
                tick(&mut br, b, now);
            }
        }
        let mut out = [0xa5; 64];
        let message = br
            .receive_datagram(&b.identity, &b.security, &mut out, 80)
            .unwrap();
        assert_eq!(&out[..message.bytes], b"hot");
        assert_eq!(message.origin_sequence, 1);
        assert_eq!(
            ar.query(tx).unwrap().outcome,
            Some(DatagramOutcome::LocalSuccess)
        );
    });
}
fn send_now_result(
    root: &mut Root<'_>,
    node: &Node<'_>,
    ep: DatagramEndpointHandle,
    now: u64,
) -> Result<Option<DatagramTxHandle>> {
    root.send(
        ep,
        b"old",
        DatagramSendOptions {
            traffic_class: TrafficClass::Q2,
            deadline_us: now + 1000,
            tracked: true,
        },
        &node.identity,
        &node.security,
        now,
    )
}

#[test]
fn a_real_new_session_does_not_bypass_unknown_or_cold_registration() {
    with_mesh_all_stores(256, false, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, ah, ae) = setup(a, 1);
        let (mut br, bh, _) = setup(b, 0);
        let ac = peer_reference(a, 1, 20).1;
        a.driver.data_submit = Some(DriverSubmit::Unknown);
        let tx = send_now(&mut ar, a, ae, 21);
        tick(&mut ar, a, 22);
        assert!(ar.query(tx).unwrap().physical_pending);
        let token = a.driver.tokens[0];
        ar.fence_neighbor(ah, &mut a.security, 23).unwrap();
        br.fence_neighbor(bh, &mut b.security, 23).unwrap();
        assert!(!ar.neighbor_is_drained(ah).unwrap());
        a.link = a.adapter.reopen_link(a.link, 4, 256).unwrap();
        renew(a, 1, 2, ap, am, 24);
        renew(b, 0, 2, bp, bm, 24);
        assert!(adopt(&mut ar, ah, a, 1, ac, 26).is_err());
        // RX can authenticate B's new Advertise while A's old TX is still Unknown.
        // A旧TX仍Unknown时可以接收B真实发送的新Advertise，不假装退休A的Token。
        let ac2 = advertise_at(b, a, 27, Some(ac));
        assert_eq!(adopt(&mut ar, ah, a, 1, ac2, 28), Err(Error::State));
        let mut peer = a.peers[1].unwrap();
        peer.facts.now_us = 28;
        assert_eq!(
            ar.register_neighbor(
                peer,
                [a.bindings[0], a.bindings[1]],
                &a.identity,
                &a.security,
                &a.caps,
                ac2,
                &a.adapter,
                1,
                101,
                28
            ),
            Err(Error::Config)
        );
        ar.stop(&mut a.adapter).unwrap();
        for now in 29..70 {
            tick(&mut ar, a, now);
        }
        assert!(!ar.neighbor_is_drained(ah).unwrap());
        assert!(ar.query(tx).unwrap().physical_pending);
        a.ingress
            .tx_complete(token, TerminalOutcome::Success)
            .unwrap();
        for now in 70..100 {
            tick(&mut ar, a, now);
        }
        assert!(!ar.query(tx).unwrap().physical_pending);
        assert!(ar.neighbor_is_drained(ah).unwrap());
        assert_eq!(
            adopt(&mut ar, ah, a, 1, ac2, 101),
            Err(Error::State),
            "stopped Root cannot be resurrected"
        );
    });
}

#[test]
fn unread_old_application_message_blocks_adoption_until_fenced_cleanup() {
    with_mesh_all_stores(256, false, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, ah, ae) = setup(a, 1);
        let (mut br, bh, _) = setup(b, 0);
        let bc = peer_reference(b, 0, 20).1;
        send_now(&mut ar, a, ae, 21);
        tick(&mut ar, a, 22);
        for (_, packet) in sent(a) {
            deliver(b, 0, &packet, 22);
        }
        tick(&mut br, b, 22);
        assert_eq!(br.stats().rx_accepted, 1);
        ar.fence_neighbor(ah, &mut a.security, 23).unwrap();
        br.fence_neighbor(bh, &mut b.security, 23).unwrap();
        assert!(!br.neighbor_is_drained(bh).unwrap());
        renew(a, 1, 2, ap, am, 24);
        renew(b, 0, 2, bp, bm, 24);
        tick(&mut ar, a, 25); // Retire A's completed physical TX before its Advertise.
        let bc2 = advertise_at(a, b, 26, Some(bc));
        assert_eq!(adopt(&mut br, bh, b, 0, bc2, 27), Err(Error::State));
        let mut out = [0xa5; 64];
        assert!(
            br.receive_datagram(&b.identity, &b.security, &mut out, 27)
                .is_err()
        );
        assert_eq!(out, [0xa5; 64]);
        for now in 28..40 {
            tick(&mut br, b, now);
        }
        assert!(br.neighbor_is_drained(bh).unwrap());
        adopt(&mut br, bh, b, 0, bc2, 40).unwrap();
        assert_eq!(br.stats().app_pulled, 0);
        assert_eq!(br.stats().app_discarded, 1);
    });
}

#[test]
fn expired_successor_capability_and_foreign_handle_cannot_replace_the_slot() {
    with_mesh_all_stores(256, false, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, ah, _) = setup(a, 1);
        let (mut br, bh, _) = setup(b, 0);
        assert_eq!(ar.neighbor_is_drained(bh), Err(Error::NotFound));
        let old = peer_reference(a, 1, 20).1;
        ar.fence_neighbor(ah, &mut a.security, 21).unwrap();
        br.fence_neighbor(bh, &mut b.security, 21).unwrap();
        renew(a, 1, 2, ap, am, 22);
        renew(b, 0, 2, bp, bm, 22);
        let new = advertise_at(b, a, 25, Some(old));
        let deadline = a.caps.peer_get(new, 25).unwrap().capability_deadline_us;
        assert_eq!(adopt(&mut ar, ah, a, 1, new, deadline), Err(Error::Timeout));
        assert!(ar.neighbor_is_drained(ah).unwrap());
    });
}

#[test]
fn callback_gate_rejects_neighbor_lifecycle_without_reactivating_or_fencing() {
    with_mesh(|a, _, _| {
        let (mut root, h, _) = setup(a, 1);
        let gate = a.security.callback_gate();
        let lease = gate
            .try_enter(CallbackClaim::new(99, 1, 1, 1).unwrap())
            .unwrap();
        assert_eq!(root.neighbor_is_drained(h), Err(Error::State));
        assert_eq!(
            root.fence_neighbor(h, &mut a.security, 21),
            Err(Error::State)
        );
        gate.leave(lease).unwrap();
        assert!(!root.neighbor_is_drained(h).unwrap());
        assert!(
            a.security
                .session_get(
                    a.peers[1].unwrap().session,
                    CurrentFacts {
                        now_us: 21,
                        ..a.peers[1].unwrap().facts
                    }
                )
                .is_ok()
        );
    });
}

#[test]
fn repeated_durable_handoffs_reuse_fixed_neighbor_and_capability_slots() {
    with_mesh_all_stores(256, false, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, mut ah, mut ae) = setup(a, 1);
        let (mut br, mut bh, mut be) = setup(b, 0);
        let mut ac = peer_reference(a, 1, 20).1;
        let mut bc = peer_reference(b, 0, 20).1;
        for generation in 2..=6 {
            let now = u64::from(generation) * 50;
            let old_a = a.peers[1].unwrap().session;
            let old_b = b.peers[0].unwrap().session;
            ar.fence_neighbor(ah, &mut a.security, now).unwrap();
            br.fence_neighbor(bh, &mut b.security, now).unwrap();
            assert!(ar.neighbor_is_drained(ah).unwrap());
            assert!(br.neighbor_is_drained(bh).unwrap());
            a.security.retire_fenced(old_a).unwrap();
            b.security.retire_fenced(old_b).unwrap();
            renew(a, 1, generation, ap, am, now + 1);
            renew(b, 0, generation, bp, bm, now + 1);
            ac = advertise_at(b, a, now + 3, Some(ac));
            bc = advertise_at(a, b, now + 3, Some(bc));
            let ah2 = adopt(&mut ar, ah, a, 1, ac, now + 4).unwrap();
            let bh2 = adopt(&mut br, bh, b, 0, bc, now + 4).unwrap();
            assert_ne!(ah, ah2);
            assert_ne!(bh, bh2);
            ah = ah2;
            bh = bh2;
            ae = endpoint(&mut ar, ae, a, 1, now + 5);
            be = endpoint(&mut br, be, b, 0, now + 5);
            logs(a);
            logs(b);
            let tx = send_now(&mut ar, a, ae, now + 6);
            for t in now + 7..now + 20 {
                tick(&mut ar, a, t);
                for (_, packet) in sent(a) {
                    deliver(b, 0, &packet, t);
                    tick(&mut br, b, t);
                }
            }
            let mut out = [0; 64];
            let msg = br
                .receive_datagram(&b.identity, &b.security, &mut out, now + 20)
                .unwrap();
            assert_eq!(msg.origin_sequence, 1);
            assert_eq!(&out[..msg.bytes], b"hot");
            assert_eq!(
                ar.query(tx).unwrap().outcome,
                Some(DatagramOutcome::LocalSuccess)
            );
            ar.forget(tx).unwrap();
            assert_eq!(a.caps.counts(now + 20).0, 1);
            assert_eq!(b.caps.counts(now + 20).0, 2);
        }
    });
}

#[test]
fn multihop_data_rediscovers_after_neighbor_handoff_without_resetting_origin() {
    with_mesh_all_stores(256, true, |a, b, c, ap, am, bp, bm, _, _| {
        let (mut ar, ah, _) = setup(a, 1);
        let (mut br, bh, _) = setup(b, 0);
        let (mut cr, _) = root(c);
        let ac_ep = contexts::register(&mut ar, a, 2);
        let ac = peer_reference(a, 1, 20).1;
        let bc = peer_reference(b, 0, 20).1;
        logs(a);
        logs(b);
        logs(c);
        let old = send_now(&mut ar, a, ac_ep, 21);
        for now in 22..90 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        let mut out = [0; 64];
        assert_eq!(
            cr.receive_datagram(&c.identity, &c.security, &mut out, 90)
                .unwrap()
                .origin_sequence,
            1
        );
        assert_eq!(
            ar.query(old).unwrap().outcome,
            Some(DatagramOutcome::LocalSuccess)
        );
        ar.fence_neighbor(ah, &mut a.security, 100).unwrap();
        br.fence_neighbor(bh, &mut b.security, 100).unwrap();
        assert!(ar.neighbor_is_drained(ah).unwrap());
        assert!(br.neighbor_is_drained(bh).unwrap());
        renew(a, 1, 2, ap, am, 101);
        renew(b, 0, 2, bp, bm, 101);
        let ac2 = advertise_at(b, a, 104, Some(ac));
        let bc2 = advertise_at(a, b, 104, Some(bc));
        adopt(&mut ar, ah, a, 1, ac2, 105).unwrap();
        adopt(&mut br, bh, b, 0, bc2, 105).unwrap();
        logs(a);
        logs(b);
        logs(c);
        let next = send_now(&mut ar, a, ac_ep, 106);
        let mut discovered = false;
        for now in 107..250 {
            tick(&mut ar, a, now);
            discovered |=
                a.driver.frames.iter().any(|f| {
                    ucn_wire::CommonHeader::decode(f).unwrap().contract == HeaderContract::C0
                });
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        assert!(discovered, "old path was not inherited");
        assert_eq!(
            ar.query(next).unwrap().outcome,
            Some(DatagramOutcome::LocalSuccess)
        );
        let msg = cr
            .receive_datagram(&c.identity, &c.security, &mut out, 250)
            .unwrap();
        assert_eq!(
            msg.origin_sequence, 2,
            "only AB/BA Hop sessions changed, not AC Origin"
        );
        assert_eq!(&out[..msg.bytes], b"hot");
    });
}

#[test]
fn smaller_new_capability_cannot_inherit_old_mtu_and_can_retry_after_real_update() {
    with_mesh_all_stores(256, false, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, ah, _) = setup(a, 1);
        let (mut br, bh, _) = setup(b, 0);
        let old = peer_reference(a, 1, 20).1;
        ar.fence_neighbor(ah, &mut a.security, 21).unwrap();
        br.fence_neighbor(bh, &mut b.security, 21).unwrap();
        renew(a, 1, 2, ap, am, 22);
        renew(b, 0, 2, bp, bm, 22);
        let mut record = b.caps.local().0;
        record.link.link_frame_mtu = 126;
        record.link.processing_frame_mtu = 126;
        let next = advertise_record_at(b, a, 25, Some(old), Some(record));
        assert_eq!(adopt(&mut ar, ah, a, 1, next, 26), Err(Error::NoSpace));
        assert!(ar.neighbor_is_drained(ah).unwrap());
        record = b.caps.local().0;
        record.capability_generation = 2;
        let current = advertise_record_at(b, a, 27, None, Some(record));
        assert_eq!(current, next);
        adopt(&mut ar, ah, a, 1, current, 28).unwrap();
        assert_eq!(a.caps.counts(28).0, 1);
    });
}

#[test]
fn old_authenticated_views_and_equal_generations_cannot_restore_the_old_capability() {
    with_mesh_all_stores(256, false, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, ah, _) = setup(a, 1);
        let (mut br, bh, _) = setup(b, 0);
        let previous = peer_reference(a, 1, 20).1;
        let peer = a.peers[1].unwrap();
        let old_auth = a
            .security
            .authenticated_peer_view(
                peer.session,
                CurrentFacts {
                    now_us: 20,
                    ..peer.facts
                },
                a.link.slot() + 1,
            )
            .unwrap();
        ar.fence_neighbor(ah, &mut a.security, 21).unwrap();
        br.fence_neighbor(bh, &mut b.security, 21).unwrap();
        renew(a, 1, 2, ap, am, 22);
        renew(b, 0, 2, bp, bm, 22);
        let current = advertise_at(b, a, 25, Some(previous));
        let before = a.caps.peer_get(current, 25).unwrap();
        assert_eq!(
            a.caps
                .replace_session(current, old_auth, b.caps.local().0, 26),
            Err(Error::Replay)
        );
        assert_eq!(
            a.caps.ingest_advertise(old_auth, b.caps.local().0, 26),
            Err(Error::Replay)
        );
        let peer = a.peers[1].unwrap();
        let auth = a
            .security
            .authenticated_peer_view(
                peer.session,
                CurrentFacts {
                    now_us: 26,
                    ..peer.facts
                },
                a.link.slot() + 1,
            )
            .unwrap();
        assert_eq!(
            a.caps.replace_session(current, auth, b.caps.local().0, 26),
            Err(Error::Replay)
        );
        assert_eq!(
            a.caps.replace_session(previous, auth, b.caps.local().0, 26),
            Err(Error::NotFound)
        );
        assert_eq!(a.caps.peer_get(current, 26).unwrap(), before);
        assert_eq!(a.caps.counts(26).0, 1);
    });
}

#[test]
fn control_unknown_blocks_hot_adoption_even_without_origin_data_on_that_session() {
    with_mesh_all_stores(256, true, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, ah, _) = setup(a, 1);
        let (mut br, bh, _) = setup(b, 0);
        let ac_ep = contexts::register(&mut ar, a, 2);
        let previous = peer_reference(a, 1, 20).1;
        a.driver.submit = DriverSubmit::Unknown;
        send_now(&mut ar, a, ac_ep, 21);
        for now in 22..30 {
            tick(&mut ar, a, now);
            if !a.driver.tokens.is_empty() {
                break;
            }
        }
        let token = a.driver.tokens[0];
        assert_eq!(
            ucn_wire::CommonHeader::decode(&a.driver.frames[0])
                .unwrap()
                .contract,
            HeaderContract::C0
        );
        ar.fence_neighbor(ah, &mut a.security, 31).unwrap();
        br.fence_neighbor(bh, &mut b.security, 31).unwrap();
        a.link = a.adapter.reopen_link(a.link, 4, 256).unwrap();
        renew(a, 1, 2, ap, am, 32);
        renew(b, 0, 2, bp, bm, 32);
        let new = advertise_at(b, a, 35, Some(previous));
        assert_eq!(adopt(&mut ar, ah, a, 1, new, 36), Err(Error::State));
        ar.stop(&mut a.adapter).unwrap();
        for now in 37..50 {
            tick(&mut ar, a, now);
        }
        assert!(!ar.neighbor_is_drained(ah).unwrap());
        a.ingress
            .tx_complete(token, TerminalOutcome::Success)
            .unwrap();
        for now in 50..70 {
            tick(&mut ar, a, now);
        }
        assert!(ar.neighbor_is_drained(ah).unwrap());
    });
}

#[test]
fn reopening_a_bearer_cannot_reuse_the_old_local_capability_publication() {
    with_mesh_all_stores(256, false, |a, b, _, ap, am, bp, bm, _, _| {
        let (mut ar, ah, _) = setup(a, 1);
        let (mut br, bh, _) = setup(b, 0);
        let old = peer_reference(a, 1, 20).1;
        ar.fence_neighbor(ah, &mut a.security, 21).unwrap();
        br.fence_neighbor(bh, &mut b.security, 21).unwrap();
        a.link = a.adapter.reopen_link(a.link, 4, 256).unwrap();
        renew(a, 1, 2, ap, am, 22);
        renew(b, 0, 2, bp, bm, 22);
        let new = advertise_at(b, a, 25, Some(old));
        assert!(ar.neighbor_is_drained(ah).unwrap());
        assert_eq!(adopt(&mut ar, ah, a, 1, new, 26), Err(Error::Config));
        assert!(ar.neighbor_is_drained(ah).unwrap());
        assert_eq!(a.caps.local().0.link.link_instance_generation, 3);
    });
}
