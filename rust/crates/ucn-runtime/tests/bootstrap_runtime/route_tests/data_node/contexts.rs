//! One real H1 Session for direct Origin/Hop, independent contexts for routed Data.
//! 直连复用一个真实H1 Session，多跳独立选择Origin/Hop，不伪造第二个活动Session。
use super::*;
use ucn_wire::CommonHeader;

pub(super) fn register(
    root: &mut Root<'_>,
    node: &Node<'_>,
    remote: usize,
) -> DatagramEndpointHandle {
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
}

#[test]
fn direct_both_directions_use_one_actual_h1_session() {
    with_mesh(|a, b, _| {
        let (mut ar, ae) = root_for(a, 8, Some(1));
        let (mut br, be) = root_for(b, 8, Some(0));
        let atx = request(&mut ar, a, ae.unwrap(), b"direct AB", 1000);
        let btx = request(&mut br, b, be.unwrap(), b"direct BA", 1000);
        let mut packets = Vec::new();
        let mut received = [false; 2];
        for now in 22..500 {
            tick(&mut ar, a, now);
            for (to, packet) in sent(a) {
                assert_eq!(to, 101);
                if CommonHeader::decode(&packet).unwrap().contract == HeaderContract::C1 {
                    packets.push(packet.clone());
                }
                deliver(b, 0, &packet, now);
                tick(&mut br, b, now);
            }
            tick(&mut br, b, now);
            for (to, packet) in sent(b) {
                if to == 100 {
                    if CommonHeader::decode(&packet).unwrap().contract == HeaderContract::C1 {
                        packets.push(packet.clone());
                    }
                    deliver(a, 1, &packet, now);
                    tick(&mut ar, a, now);
                } else {
                    assert_eq!(to, 102); // Unanswered discovery branch, not a delivery shortcut.
                }
            }
            for (index, (root, node, expected)) in [
                (&mut ar, &*a, &b"direct BA"[..]),
                (&mut br, &*b, &b"direct AB"[..]),
            ]
            .into_iter()
            .enumerate()
            {
                let mut out = [0xa5; 64];
                if let Ok(message) =
                    root.receive_datagram(&node.identity, &node.security, &mut out, now)
                {
                    assert!(!received[index]);
                    assert_eq!(&out[..message.bytes], expected);
                    assert_eq!(message.origin_sequence, 1);
                    assert_eq!(message.origin, fixed_binding(1 - index));
                    received[index] = true;
                }
            }
            if received == [true; 2]
                && ar.query(atx).unwrap().outcome.is_some()
                && br.query(btx).unwrap().outcome.is_some()
            {
                break;
            }
        }
        assert_eq!(
            received,
            [true; 2],
            "a={:?}, b={:?}, atx={:?}, btx={:?}",
            ar.stats(),
            br.stats(),
            ar.query(atx),
            br.query(btx)
        );
        assert_eq!(packets.len(), 2);
        for packet in &packets {
            assert_eq!(packet.len(), 13 + 9 + 16 + 16);
            let hop = u32::from_be_bytes(
                packet[packet.len() - 16..packet.len() - 12]
                    .try_into()
                    .unwrap(),
            );
            assert!(hop > 1, "shares the actual C0/control Hop sequence space");
        }
        assert_eq!(
            ar.query(atx).unwrap().outcome,
            Some(DatagramOutcome::LocalSuccess)
        );
        assert_eq!(
            br.query(btx).unwrap().outcome,
            Some(DatagramOutcome::LocalSuccess)
        );
        assert_eq!(ar.stats().relay_accepted + br.stats().relay_accepted, 0);
        for (node, remote) in [(&*a, 1), (&*b, 0)] {
            let peer = node.peers[remote].unwrap();
            assert_eq!(
                node.security
                    .session_get(
                        peer.session,
                        CurrentFacts {
                            now_us: 500,
                            ..peer.facts
                        }
                    )
                    .unwrap()
                    .hop_profile,
                HopProfile::H1
            );
            let bank = node
                .counters
                .0
                .iter()
                .find(|v| v.0 == peer.session)
                .unwrap();
            assert_eq!(bank.3.0, 2, "one Origin counter, not one per layer");
        }
    });
}

#[test]
fn one_root_mixes_direct_and_multihop_without_wire_or_send_mode_switches() {
    with_mesh_candidates(256, true, |a, b, c| {
        let (mut ar, ac) = root(a);
        let ab = register(&mut ar, a, 1);
        let (mut br, _) = root_for(b, 8, Some(0));
        let (mut cr, _) = root(c);
        let direct = request(&mut ar, a, ab, b"B", 1000);
        let routed = request(&mut ar, a, ac.unwrap(), b"C", 1000);
        let mut got = [false; 2];
        for now in 22..500 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
            for (index, (root, node, expected)) in
                [(&mut br, &*b, &b"B"[..]), (&mut cr, &*c, &b"C"[..])]
                    .into_iter()
                    .enumerate()
            {
                let mut out = [0xa5; 8];
                if let Ok(message) =
                    root.receive_datagram(&node.identity, &node.security, &mut out, now)
                {
                    assert!(!got[index]);
                    assert_eq!(&out[..message.bytes], expected);
                    assert_eq!(message.origin, fixed_binding(0));
                    assert_eq!(message.origin_sequence, 1, "independent end-to-end domains");
                    got[index] = true;
                }
            }
            if got == [true; 2]
                && ar.query(direct).unwrap().outcome.is_some()
                && ar.query(routed).unwrap().outcome.is_some()
            {
                break;
            }
        }
        assert_eq!(
            got,
            [true; 2],
            "a={:?}, b={:?}, c={:?}",
            ar.stats(),
            br.stats(),
            cr.stats()
        );
        assert_eq!(br.stats().relay_accepted, 1);
        assert_eq!(br.stats().app_pulled, 1);
        assert_eq!(cr.stats().app_pulled, 1);
        assert_eq!(
            ar.query(direct).unwrap().outcome,
            Some(DatagramOutcome::LocalSuccess)
        );
        assert_eq!(
            ar.query(routed).unwrap().outcome,
            Some(DatagramOutcome::LocalSuccess)
        );
    });
}

fn origin_fixture(peer: JournalPeer) -> ([u8; 13], [u8; 46], OriginContext) {
    let mut prefix = [0; 13];
    CommonHeader {
        contract: HeaderContract::C1,
        traffic_class: TrafficClass::Q2,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: InteractionRole::OneWay,
        payload_kind: PayloadKind::Data,
        origin_security: OriginSecurity::O1,
        hop_limit: HopLimit::new(8).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..5].copy_from_slice(
        &u16::try_from(peer.facts.local.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[5..7].copy_from_slice(
        &u16::try_from(peer.facts.peer.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[7..9].copy_from_slice(&0x40u16.to_be_bytes());
    prefix[9..13].copy_from_slice(&1u32.to_be_bytes());
    let mut canonical = [0; 46];
    canonical[..16].copy_from_slice(&peer.facts.local.principal);
    canonical[16..20].copy_from_slice(&peer.facts.local.generation.get().to_be_bytes());
    canonical[20..36].copy_from_slice(&peer.facts.peer.principal);
    canonical[36..40].copy_from_slice(&peer.facts.peer.generation.get().to_be_bytes());
    canonical[40..42].copy_from_slice(&0x40u16.to_be_bytes());
    canonical[42..].copy_from_slice(&1u32.to_be_bytes());
    (
        prefix,
        canonical,
        OriginContext::Sequenced {
            contract: HeaderContract::C1,
            fingerprint: peer.fingerprint,
            sequence: OriginSequence::new(1).unwrap(),
        },
    )
}
fn access(
    peer: JournalPeer,
    fingerprint: Fingerprint,
    direction: AccessDirection,
) -> AccessRequest {
    AccessRequest {
        session: peer.session,
        context_fingerprint: fingerprint,
        service: ServiceId::new(0x40).unwrap(),
        protocol_opcode: 0,
        direction,
    }
}

#[test]
fn origin_projection_does_not_weaken_complete_packet_profile_or_double_reserve_hop() {
    with_mesh(|a, b, _| {
        let ap = a.peers[1].unwrap();
        let bp = b.peers[0].unwrap();
        let (prefix, canonical, context) = origin_fixture(ap);
        let plan = PacketPlan {
            prefix: &prefix,
            payload: b"layer",
            canonical_identity: &canonical,
            origin_context: context,
            hop_profile: HopProfile::H0,
        };
        let mut work = SecurityPacketWorkspace::<320>::new();
        let mut origin = [0xa5; 256];
        assert_eq!(
            a.security.protect_packet(
                &mut a.crypto,
                OriginCounterOwner::Sequence(a.counters.origin(ap.session).unwrap()),
                ap.session,
                ap.facts,
                access(ap, ap.fingerprint, AccessDirection::Outbound),
                plan,
                None,
                &mut work,
                &mut origin
            ),
            Err(Error::Security)
        );
        assert_eq!(origin, [0xa5; 256]);
        assert_eq!(
            a.counters
                .origin(ap.session)
                .unwrap()
                .preview()
                .unwrap()
                .get(),
            1
        );
        let hop_before = a
            .counters
            .counters(ap.session)
            .unwrap()
            .1
            .preview()
            .unwrap();
        let n = a
            .security
            .protect_c1_origin(
                &mut a.crypto,
                a.counters.origin(ap.session).unwrap(),
                ap.session,
                ap.facts,
                access(ap, ap.fingerprint, AccessDirection::Outbound),
                plan,
                &mut work,
                &mut origin,
            )
            .unwrap();
        assert_eq!(n, 13 + 5 + 16);
        let before = origin;
        let mut reset = OriginCounter(1);
        assert_eq!(
            a.security.protect_c1_origin(
                &mut a.crypto,
                &mut reset,
                ap.session,
                ap.facts,
                access(ap, ap.fingerprint, AccessDirection::Outbound),
                plan,
                &mut work,
                &mut origin
            ),
            Err(Error::Replay)
        );
        assert_eq!(origin, before);
        assert_eq!(reset.0, 1);
        assert_eq!(
            a.counters
                .counters(ap.session)
                .unwrap()
                .1
                .preview()
                .unwrap(),
            hop_before
        );
        let open = OpenPacketPlan {
            packet: &origin[..n],
            prefix_bytes: 13,
            canonical_identity: &canonical,
            origin_context: context,
            hop_profile: HopProfile::H0,
        };
        let mut out = [0xa5; 16];
        assert!(
            b.security
                .open_packet(
                    &mut b.crypto,
                    bp.session,
                    bp.facts,
                    access(bp, bp.fingerprint, AccessDirection::Inbound),
                    open,
                    &mut work,
                    &mut out
                )
                .is_err()
        );
        assert_eq!(out, [0xa5; 16]);
        let ah = a
            .security
            .context_fingerprints(ap.session, ap.facts)
            .unwrap()
            .1;
        let bh = b
            .security
            .context_fingerprints(bp.session, bp.facts)
            .unwrap()
            .1;
        let mut packet = [0; 256];
        let bytes = a
            .security
            .wrap_c1_hop(
                &mut a.crypto,
                ap.session,
                ap.facts,
                access(ap, ah, AccessDirection::Outbound),
                &origin[..n],
                a.counters.counters(ap.session).unwrap().1,
                &mut work,
                &mut packet,
            )
            .unwrap();
        let hop = b
            .security
            .open_c1_hop(
                &mut b.crypto,
                bp.session,
                bp.facts,
                access(bp, bh, AccessDirection::Inbound),
                &packet[..bytes],
                &mut work,
            )
            .unwrap();
        let opened = b
            .security
            .open_c1_origin(
                &mut b.crypto,
                bp.session,
                bp.facts,
                access(bp, bp.fingerprint, AccessDirection::Inbound),
                open,
                &mut work,
                &mut out,
            )
            .unwrap();
        assert_eq!(&out[..opened.payload_bytes], b"layer");
        b.security
            .replay_preflight(&opened.replay, bp.facts)
            .unwrap();
        b.security
            .c1_hop_preflight(&hop, bp.facts, &packet[..bytes], &mut work)
            .unwrap();
        b.security.replay_commit(opened.replay, bp.facts).unwrap();
        b.security.commit_c1_hop(hop, bp.facts).unwrap();
        assert!(
            b.security
                .open_c1_hop(
                    &mut b.crypto,
                    bp.session,
                    bp.facts,
                    access(bp, bh, AccessDirection::Inbound),
                    &packet[..bytes],
                    &mut work
                )
                .is_err()
        );
    });
}

#[test]
fn shared_session_fence_keeps_control_unknown_in_the_origin_drain_barrier() {
    with_mesh_candidates(256, true, |a, _, _| {
        let (mut ar, ac) = root(a);
        let ab = register(&mut ar, a, 1);
        request(&mut ar, a, ac.unwrap(), b"needs B hop", 1000);
        a.driver.submit = DriverSubmit::Unknown;
        for now in 22..60 {
            tick(&mut ar, a, now);
            if !a.driver.tokens.is_empty() {
                break;
            }
        }
        assert!(!a.driver.tokens.is_empty());
        assert_eq!(
            CommonHeader::decode(&a.driver.frames[0]).unwrap().contract,
            HeaderContract::C0
        );
        assert_eq!(ar.fence_origin(ab, &mut a.security, 61), Ok(1));
        assert!(
            !ar.origin_is_drained(ab).unwrap(),
            "C0 owns the same physical Session"
        );
        ar.stop(&mut a.adapter).unwrap();
        for now in 62..150 {
            tick(&mut ar, a, now);
        }
        assert!(
            !ar.origin_is_drained(ab).unwrap(),
            "stop/cancel is not a retirement proof"
        );
        assert!(!ar.quiescent(&a.adapter));
    });
}

#[test]
fn direct_data_unknown_never_claims_a_route_wait_or_physical_drain() {
    with_mesh(|a, _, _| {
        let (mut ar, endpoint) = root_for(a, 8, Some(1));
        a.driver.data_submit = Some(DriverSubmit::Unknown);
        let tx = request(&mut ar, a, endpoint.unwrap(), b"owned", 1000);
        for now in 22..50 {
            tick(&mut ar, a, now);
            if ar.query(tx).unwrap().physical_pending {
                break;
            }
        }
        let view = ar.query(tx).unwrap();
        assert!(view.physical_pending);
        assert!(!view.waiting_route);
        assert_eq!(view.outcome, Some(DatagramOutcome::InDoubt));
        assert_eq!(
            ar.fence_origin(endpoint.unwrap(), &mut a.security, 51),
            Ok(1)
        );
        assert!(!ar.origin_is_drained(endpoint.unwrap()).unwrap());
        assert_eq!(a.driver.frames.len(), 1);
        assert_eq!(
            CommonHeader::decode(&a.driver.frames[0]).unwrap().contract,
            HeaderContract::C1,
            "no RREQ for exact live neighbour"
        );
    });
}

#[test]
fn good_hop_is_not_origin_authorization_and_rejections_leave_both_replays_fresh() {
    with_mesh(|a, b, _| {
        let (mut ar, endpoint) = root_for(a, 8, Some(1));
        let (mut br, _) = root_for(b, 8, Some(0));
        request(&mut ar, a, endpoint.unwrap(), b"auth layers", 1000);
        tick(&mut ar, a, 22);
        let packets = sent(a);
        assert_eq!(packets.len(), 1);
        let packet = &packets[0].1;
        let mut bad_hop = packet.clone();
        *bad_hop.last_mut().unwrap() ^= 1;
        deliver(b, 0, &bad_hop, 23);
        tick(&mut br, b, 23);
        assert_eq!(br.stats().rx_rejected, 1);
        assert_eq!(br.stats().rx_accepted, 0);

        // A valid neighbour can relay corrupt Origin bytes, but cannot authorize them.
        // 邻居Hop认证有效也不能为已损坏的Origin授权。
        let peer = a.peers[1].unwrap();
        let fp = a
            .security
            .context_fingerprints(peer.session, peer.facts)
            .unwrap()
            .1;
        let mut origin = packet[..packet.len() - 16].to_vec();
        *origin.last_mut().unwrap() ^= 1;
        let mut bad_origin = [0; 256];
        let length = a
            .security
            .wrap_c1_hop(
                &mut a.crypto,
                peer.session,
                CurrentFacts {
                    now_us: 24,
                    ..peer.facts
                },
                access(peer, fp, AccessDirection::Outbound),
                &origin,
                a.counters.counters(peer.session).unwrap().1,
                &mut SecurityPacketWorkspace::<320>::new(),
                &mut bad_origin,
            )
            .unwrap();
        deliver(b, 0, &bad_origin[..length], 24);
        tick(&mut br, b, 24);
        assert_eq!(br.stats().rx_rejected, 2);
        assert_eq!(br.stats().rx_accepted, 0);
        let mut out = [0xa5; 64];
        assert_eq!(
            br.receive_datagram(&b.identity, &b.security, &mut out, 24),
            Err(Error::NotFound)
        );
        assert_eq!(out, [0xa5; 64]);
        deliver(b, 0, packet, 25);
        tick(&mut br, b, 25);
        let message = br
            .receive_datagram(&b.identity, &b.security, &mut out, 25)
            .unwrap();
        assert_eq!(&out[..message.bytes], b"auth layers");
        assert_eq!(message.origin_sequence, 1);
        assert_eq!(br.stats().rx_accepted, 1);
    });
}

#[test]
fn direct_path_cannot_skip_required_capabilities_or_burn_an_origin_sequence() {
    with_mesh(|a, _, _| {
        let mut root = Root::new(
            route_config(199),
            a.routes.config(),
            100,
            DatagramConfig {
                lifetime_us: 1500,
                max_hops: HopLimit::new(8).unwrap(),
                required_capability_bits: u16::MAX,
            },
            a.identity.callback_gate(),
        )
        .unwrap();
        let (peer, cap) = peer_reference(a, 1, 20);
        root.register_neighbor(
            peer,
            [a.bindings[0], a.bindings[1]],
            &a.identity,
            &a.security,
            &a.caps,
            cap,
            &a.adapter,
            1,
            101,
            20,
        )
        .unwrap();
        let endpoint = register(&mut root, a, 1);
        let tx = request(&mut root, a, endpoint, b"unsupported", 1000);
        a.driver.frames.clear();
        for now in 22..30 {
            tick(&mut root, a, now);
        }
        assert_eq!(
            root.query(tx).unwrap().outcome,
            Some(DatagramOutcome::Failed(Error::Unsupported))
        );
        assert_eq!(
            a.counters
                .origin(peer.session)
                .unwrap()
                .preview()
                .unwrap()
                .get(),
            1
        );
        assert!(a.driver.frames.is_empty());
    });
}
