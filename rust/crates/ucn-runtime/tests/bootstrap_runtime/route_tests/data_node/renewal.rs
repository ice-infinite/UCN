//! Actual Foundation successor publications; no fabricated active-session proof.
//! 后继Session必须经过实际Foundation发布，不伪造Active事实。
use super::*;

fn successor(local: usize, remote: usize, generation: u32) -> HandshakeCandidate {
    let mut candidate = c1_path::endpoint_candidate(local, remote);
    candidate.session_generation = PeerSessionGeneration::new(generation).unwrap();
    candidate.origin_tx.key_generation = generation;
    candidate.origin_rx.key_generation = generation;
    // Host test uses genuinely different MAC material, not an ignored generation label.
    candidate.origin_tx.key_id =
        KeyId::new(candidate.origin_tx.key_id.get() + 10 * u16::try_from(generation).unwrap())
            .unwrap();
    candidate.origin_rx.key_id =
        KeyId::new(candidate.origin_rx.key_id.get() + 10 * u16::try_from(generation).unwrap())
            .unwrap();
    candidate.transcript_digest = [u8::try_from(7 + generation).unwrap(); 32];
    candidate
}
fn renew(
    node: &mut Node<'_>,
    store: &mut Store<'_>,
    media: &mut MeshMedia,
    now: u64,
) -> JournalPeer {
    renew_generation(node, store, media, 2, now)
}
fn renew_generation(
    node: &mut Node<'_>,
    store: &mut Store<'_>,
    media: &mut MeshMedia,
    generation: u32,
    now: u64,
) -> JournalPeer {
    let remote = 2 - node.id;
    let candidate = successor(node.id, remote, generation);
    let session = activate_at(
        &mut node.security,
        store,
        media,
        candidate,
        DOMAINS[4 + remote],
        now,
    );
    let peer = JournalPeer {
        session,
        facts: CurrentFacts {
            now_us: now + 1,
            ..node.peers[remote].unwrap().facts
        },
        fingerprint: candidate.origin_fingerprint,
        link: node.link,
    };
    node.counters
        .0
        .push((session, Counter(1), Hop(1), OriginCounter(1)));
    node.peers[remote] = Some(peer);
    peer
}
fn rebind(
    root: &mut Root<'_>,
    node: &Node<'_>,
    old: DatagramEndpointHandle,
    peer: JournalPeer,
    now: u64,
) -> Result<DatagramEndpointHandle> {
    root.rebind_endpoint(
        old,
        peer.session,
        CurrentFacts {
            now_us: now,
            ..peer.facts
        },
        peer.fingerprint,
        [node.bindings[node.id], node.bindings[2 - node.id]],
        &node.identity,
        &node.security,
        now,
    )
}
fn send_at(
    root: &mut Root<'_>,
    node: &Node<'_>,
    endpoint: DatagramEndpointHandle,
    bytes: &[u8],
    now: u64,
) -> Result<Option<DatagramTxHandle>> {
    root.send(
        endpoint,
        bytes,
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
fn queued_old_work_is_fenced_and_cannot_reactivate_through_registration() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        let (mut root, endpoint) = root(a);
        let endpoint = endpoint.unwrap();
        let old_session = a.peers[2].unwrap().session;
        let tx = request(&mut root, a, endpoint, b"queued", 1000);
        assert!(!root.origin_is_drained(endpoint).unwrap());
        assert_eq!(root.fence_origin(endpoint, &mut a.security, 22), Ok(1));
        assert_eq!(
            send_at(&mut root, a, endpoint, b"old", 23),
            Err(Error::State)
        );
        assert!(!root.origin_is_drained(endpoint).unwrap());
        let peer = renew(a, ap, am, 24);
        assert_eq!(rebind(&mut root, a, endpoint, peer, 25), Err(Error::State));
        for now in 26..80 {
            tick(&mut root, a, now);
        }
        assert!(root.origin_is_drained(endpoint).unwrap());
        assert_eq!(
            root.query(tx).unwrap().outcome,
            Some(DatagramOutcome::Failed(Error::State))
        );
        assert_eq!(
            rebind(
                &mut root,
                a,
                endpoint,
                JournalPeer {
                    session: old_session,
                    ..peer
                },
                80
            ),
            Err(Error::State)
        );
        let next = rebind(&mut root, a, endpoint, peer, 81).unwrap();
        assert_ne!(next, endpoint);
        assert_eq!(
            send_at(&mut root, a, endpoint, b"stale", 82),
            Err(Error::NotFound)
        );
        assert_eq!(
            root.fence_origin(endpoint, &mut a.security, 82),
            Err(Error::NotFound)
        );
        assert!(
            a.security
                .session_get(
                    peer.session,
                    CurrentFacts {
                        now_us: 82,
                        ..peer.facts
                    }
                )
                .is_ok()
        );
        assert!(send_at(&mut root, a, next, b"new", 83).is_ok());
        assert_eq!(
            root.query(tx).unwrap().outcome,
            Some(DatagramOutcome::Failed(Error::State))
        );
    });
}

#[test]
fn higher_durable_session_does_not_bypass_explicit_registry_fence() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        let (mut root, endpoint) = root(a);
        let endpoint = endpoint.unwrap();
        let next = renew(a, ap, am, 22);
        assert_eq!(rebind(&mut root, a, endpoint, next, 23), Err(Error::State));
        root.fence_origin(endpoint, &mut a.security, 24).unwrap();
        assert!(root.origin_is_drained(endpoint).unwrap());
        let mut wrong = next;
        wrong.facts.peer = fixed_binding(1);
        assert_eq!(rebind(&mut root, a, endpoint, wrong, 25), Err(Error::State));
        assert!(root.origin_is_drained(endpoint).unwrap());
        let new = rebind(&mut root, a, endpoint, next, 26).unwrap();
        assert!(!root.origin_is_drained(new).unwrap());
    });
}

#[test]
fn unknown_old_holder_blocks_even_a_real_durable_successor() {
    with_mesh_stores(256, true, |a, b, c, ap, am, _, _| {
        let (mut ar, endpoint) = root(a);
        let endpoint = endpoint.unwrap();
        let (mut br, _) = root(b);
        let (mut cr, _) = root(c);
        a.driver.data_submit = Some(DriverSubmit::Unknown);
        let tx = request(&mut ar, a, endpoint, b"held", 1400);
        let mut token = None;
        for now in 22..300 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            if ar.query(tx).unwrap().physical_pending
                && a.driver.last_submit == DriverSubmit::Unknown
            {
                token = a.driver.tokens.last().copied();
                break;
            }
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        let token = token.expect("actual unknown data holder");
        ar.fence_origin(endpoint, &mut a.security, 300).unwrap();
        let next = renew(a, ap, am, 301);
        assert_eq!(
            ar.register_endpoint(
                next.session,
                CurrentFacts {
                    now_us: 302,
                    ..next.facts
                },
                next.fingerprint,
                [a.bindings[0], a.bindings[2]],
                DatagramEndpointPolicy {
                    service: ServiceId::new(0x42).unwrap(),
                    traffic_ceiling: TrafficClass::Q2,
                    max_payload_bytes: 32
                },
                &a.identity,
                &a.security,
                302
            ),
            Err(Error::State),
            "a fresh Service in a free slot cannot bypass the same peer's old physical holder"
        );
        for now in 303..340 {
            tick(&mut ar, a, now);
        }
        assert!(!ar.origin_is_drained(endpoint).unwrap());
        assert_eq!(rebind(&mut ar, a, endpoint, next, 340), Err(Error::State));
        assert_eq!(ar.forget(tx), Err(Error::State));
        a.adapter.reopen_link(a.link, 4, 256).unwrap();
        for now in 341..360 {
            tick(&mut ar, a, now);
        }
        assert!(
            !ar.origin_is_drained(endpoint).unwrap(),
            "reopen is not physical retirement"
        );
        assert_eq!(rebind(&mut ar, a, endpoint, next, 360), Err(Error::State));
        a.ingress
            .tx_complete(token, TerminalOutcome::Failure(Error::Access))
            .unwrap();
        for now in 361..400 {
            tick(&mut ar, a, now);
        }
        assert!(ar.origin_is_drained(endpoint).unwrap());
        let next_handle = rebind(&mut ar, a, endpoint, next, 400).unwrap();
        assert_ne!(next_handle, endpoint);
        assert_eq!(
            ar.query(tx).unwrap().outcome,
            Some(DatagramOutcome::InDoubt)
        );
        assert!(!ar.query(tx).unwrap().physical_pending);
    });
}

#[test]
fn every_service_of_one_session_shares_the_fence_and_drain_barrier() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        let (mut ar, first) = root(a);
        let first = first.unwrap();
        let peer = a.peers[2].unwrap();
        let second = ar
            .register_endpoint(
                peer.session,
                CurrentFacts {
                    now_us: 20,
                    ..peer.facts
                },
                peer.fingerprint,
                [a.bindings[0], a.bindings[2]],
                DatagramEndpointPolicy {
                    service: ServiceId::new(0x41).unwrap(),
                    traffic_ceiling: TrafficClass::Q2,
                    max_payload_bytes: 32,
                },
                &a.identity,
                &a.security,
                20,
            )
            .unwrap();
        let tx = request(&mut ar, a, first, b"old", 1000);
        assert_eq!(ar.fence_origin(second, &mut a.security, 22), Ok(2));
        assert!(!ar.origin_is_drained(first).unwrap());
        assert!(!ar.origin_is_drained(second).unwrap());
        for now in 23..80 {
            tick(&mut ar, a, now);
        }
        assert!(ar.origin_is_drained(first).unwrap());
        assert!(ar.origin_is_drained(second).unwrap());
        let next = renew(a, ap, am, 80);
        let first_new = rebind(&mut ar, a, first, next, 82).unwrap();
        assert!(
            ar.origin_is_drained(second).unwrap(),
            "other service must not silently reactivate"
        );
        assert_eq!(send_at(&mut ar, a, second, b"old", 83), Err(Error::State));
        let second_new = rebind(&mut ar, a, second, next, 84).unwrap();
        assert_eq!(
            send_at(&mut ar, a, second_new, b"no ACL", 85),
            Err(Error::Access)
        );
        assert!(send_at(&mut ar, a, first_new, b"new", 86).is_ok());
        assert_eq!(
            ar.query(tx).unwrap().outcome,
            Some(DatagramOutcome::Failed(Error::State))
        );
    });
}

#[test]
fn unread_old_message_is_not_relabelled_as_a_new_session_message() {
    with_mesh_stores(256, true, |a, b, c, _, _, cp, cm| {
        let (mut ar, source) = root(a);
        let (mut br, _) = root(b);
        let (mut cr, target) = root(c);
        let target = target.unwrap();
        request(&mut ar, a, source.unwrap(), b"unread old", 1000);
        for now in 22..100 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        assert_eq!(cr.counts().2, 1);
        cr.fence_origin(target, &mut c.security, 100).unwrap();
        let next = renew(c, cp, cm, 101);
        let mut sentinel = [0xa5; 64];
        assert_eq!(
            cr.receive_datagram(&c.identity, &c.security, &mut sentinel, 103),
            Err(Error::State)
        );
        assert_eq!(sentinel, [0xa5; 64]);
        assert_eq!(rebind(&mut cr, c, target, next, 103), Err(Error::State));
        for now in 104..150 {
            tick(&mut cr, c, now);
        }
        assert_eq!(cr.stats().app_discarded, 1);
        assert_eq!(cr.counts().2, 0);
        let _ = rebind(&mut cr, c, target, next, 150).unwrap();
        assert_eq!(
            cr.receive_datagram(&c.identity, &c.security, &mut sentinel, 151),
            Err(Error::NotFound)
        );
        assert_eq!(sentinel, [0xa5; 64]);
    });
}

#[test]
fn renewed_endpoints_deliver_and_reject_a_delayed_old_origin_with_fresh_hop_proof() {
    with_mesh_stores(256, true, |a, b, c, ap, am, cp, cm| {
        let (mut ar, old_a) = root(a);
        let old_a = old_a.unwrap();
        let (mut br, _) = root(b);
        let (mut cr, old_c) = root(c);
        let old_c = old_c.unwrap();
        let old_tx = request(&mut ar, a, old_a, b"delayed old", 1000);
        let mut delayed = None;
        for now in 22..130 {
            tick(&mut ar, a, now);
            for (target, packet) in sent(a) {
                assert_eq!(target, 101);
                deliver(b, 0, &packet, now);
                tick(&mut br, b, now);
            }
            tick(&mut br, b, now);
            for (target, packet) in sent(b) {
                if target == 100 {
                    deliver(a, 1, &packet, now);
                    tick(&mut ar, a, now);
                } else if ucn_wire::CommonHeader::decode(&packet).unwrap().contract
                    == HeaderContract::C1
                {
                    delayed = Some(packet);
                } else {
                    assert_eq!(target, 102);
                    deliver(c, 1, &packet, now);
                    tick(&mut cr, c, now);
                }
            }
            tick(&mut cr, c, now);
            for (target, packet) in sent(c) {
                assert_eq!(target, 101);
                deliver(b, 2, &packet, now);
                tick(&mut br, b, now);
            }
            if delayed.is_some() {
                break;
            }
        }
        let delayed =
            delayed.expect("actual old frame held on simulated bearer, not delivered yet");
        ar.fence_origin(old_a, &mut a.security, 150).unwrap();
        cr.fence_origin(old_c, &mut c.security, 150).unwrap();
        for now in 151..180 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
        }
        assert!(ar.origin_is_drained(old_a).unwrap());
        assert!(cr.origin_is_drained(old_c).unwrap());
        let ac = renew(a, ap, am, 200);
        let ca = renew(c, cp, cm, 200);
        let fresh_a = rebind(&mut ar, a, old_a, ac, 202).unwrap();
        let _fresh_c = rebind(&mut cr, c, old_c, ca, 202).unwrap();
        // Prove rejection cannot be explained by Hop replay: this actual neighbour proof is fresh.
        let mut neighbor = c.peers[1].unwrap();
        neighbor.facts.now_us = 203;
        let access = AccessRequest {
            session: neighbor.session,
            context_fingerprint: c
                .security
                .context_fingerprints(neighbor.session, neighbor.facts)
                .unwrap()
                .1,
            service: ServiceId::new(0x40).unwrap(),
            protocol_opcode: 0,
            direction: AccessDirection::Inbound,
        };
        let hop = c
            .security
            .open_c1_hop(
                &mut c.crypto,
                neighbor.session,
                neighbor.facts,
                access,
                &delayed,
                &mut SecurityPacketWorkspace::<320>::new(),
            )
            .unwrap();
        c.security.abort_c1_hop(hop).unwrap();
        let rejected = cr.stats().rx_rejected;
        deliver(c, 1, &delayed, 203);
        let result = tick(&mut cr, c, 203);
        assert_eq!(result.last_error, Some(Error::Security));
        assert_eq!(cr.stats().rx_rejected, rejected + 1);
        let mut out = [0xa5; 64];
        assert_eq!(
            cr.receive_datagram(&c.identity, &c.security, &mut out, 203),
            Err(Error::NotFound)
        );
        assert_eq!(out, [0xa5; 64]);
        let _new_tx = send_at(&mut ar, a, fresh_a, b"fresh new", 204).unwrap();
        let mut received = false;
        for now in 205..600 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
            if let Ok(message) = cr.receive_datagram(&c.identity, &c.security, &mut out, now) {
                assert_eq!(&out[..message.bytes], b"fresh new");
                assert_eq!(message.origin_sequence, 1);
                received = true;
                break;
            }
        }
        assert!(
            received,
            "new generation counter=1 must work under fresh durable keys"
        );
        assert!(ar.query(old_tx).unwrap().outcome.is_some());
        assert_eq!(
            send_at(&mut ar, a, old_a, b"resurrect", 600),
            Err(Error::NotFound)
        );
    });
}

#[test]
fn prepared_or_only_foundation_durable_is_not_an_active_successor() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        let (mut ar, old) = root(a);
        let old = old.unwrap();
        ar.fence_origin(old, &mut a.security, 22).unwrap();
        let domain = DOMAINS[6];
        let candidate = successor(0, 2, 2);
        let mut body = [0; flash::BODY];
        let length = ap.copy_body(domain, &mut body).unwrap();
        let base = DurabilityBase::from_view(
            1,
            22,
            ap.domain_get(domain).unwrap(),
            &body[..length],
            1000,
            90,
            &mut CodecWorkspace::new(),
        )
        .unwrap();
        let facts = CurrentFacts {
            now_us: 23,
            local: candidate.local,
            peer: candidate.peer,
            link_generation: candidate.link_generation,
            policy_generation: 1,
        };
        let (session, requirement) = a
            .security
            .prepare_static_session(
                &mut ReauthCrypto {
                    expected: candidate,
                    record: None,
                },
                candidate,
                HandshakeProof { bytes: b"initial" },
                base,
                facts,
            )
            .unwrap();
        let peer = JournalPeer {
            session,
            facts,
            fingerprint: candidate.origin_fingerprint,
            link: a.link,
        };
        assert_eq!(rebind(&mut ar, a, old, peer, 24), Err(Error::State));
        let ph = ap.submit(&requirement.persistence_request(), 25).unwrap();
        a.security.bind_persistence(session, ph).unwrap();
        pump(ap, am, 26);
        assert_eq!(
            rebind(&mut ar, a, old, peer, 27),
            Err(Error::State),
            "durable record alone must not publish a registry entry"
        );
        a.security
            .activate(
                session,
                ph,
                ap.proof_get(ph).unwrap(),
                CurrentFacts {
                    now_us: 28,
                    ..facts
                },
            )
            .unwrap();
        ap.proof_retire(ph).unwrap();
        let mut wrong = peer;
        wrong.fingerprint = Fingerprint::new([99; 16]).unwrap();
        assert_eq!(rebind(&mut ar, a, old, wrong, 29), Err(Error::Security));
        assert!(ar.origin_is_drained(old).unwrap());
        assert!(rebind(&mut ar, a, old, peer, 30).is_ok());
    });
}

#[test]
fn an_older_service_tombstone_cannot_bypass_a_newer_unknown_holder() {
    with_mesh_stores(256, true, |a, b, c, ap, am, _, _| {
        let (mut ar, first) = root(a);
        let first = first.unwrap();
        let peer = a.peers[2].unwrap();
        let second = ar
            .register_endpoint(
                peer.session,
                CurrentFacts {
                    now_us: 20,
                    ..peer.facts
                },
                peer.fingerprint,
                [a.bindings[0], a.bindings[2]],
                DatagramEndpointPolicy {
                    service: ServiceId::new(0x41).unwrap(),
                    traffic_ceiling: TrafficClass::Q2,
                    max_payload_bytes: 32,
                },
                &a.identity,
                &a.security,
                20,
            )
            .unwrap();
        ar.fence_origin(first, &mut a.security, 21).unwrap();
        let second_generation = renew(a, ap, am, 22);
        let first_new = rebind(&mut ar, a, first, second_generation, 24).unwrap();
        let (mut br, _) = root(b);
        let (mut cr, _) = root(c);
        a.driver.data_submit = Some(DriverSubmit::Unknown);
        let tx = send_at(&mut ar, a, first_new, b"second-gen held", 25)
            .unwrap()
            .unwrap();
        let mut token = None;
        for now in 26..250 {
            tick(&mut ar, a, now);
            tick(&mut br, b, now);
            tick(&mut cr, c, now);
            if ar.query(tx).unwrap().physical_pending
                && a.driver.last_submit == DriverSubmit::Unknown
            {
                token = a.driver.tokens.last().copied();
                break;
            }
            network(a, b, c, &mut ar, &mut br, &mut cr, now);
        }
        assert!(token.is_some());
        ar.fence_origin(first_new, &mut a.security, 250).unwrap();
        let third_generation = renew_generation(a, ap, am, 3, 251);
        assert!(
            ar.origin_is_drained(second).unwrap(),
            "generation-1 tombstone itself is drained"
        );
        assert!(!ar.origin_is_drained(first_new).unwrap());
        assert_eq!(
            rebind(&mut ar, a, second, third_generation, 253),
            Err(Error::State),
            "must also inspect generation-2 holder on the other Service"
        );
        assert_eq!(
            ar.query(tx).unwrap().outcome,
            Some(DatagramOutcome::InDoubt)
        );
    });
}

#[test]
fn repeated_real_successors_reuse_bounded_registry_and_security_slots() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        let (mut ar, endpoint) = root(a);
        let mut endpoint = endpoint.unwrap();
        for generation in 2..7 {
            let now = 20 + u64::from(generation) * 10;
            let old = a.peers[2].unwrap().session;
            ar.fence_origin(endpoint, &mut a.security, now).unwrap();
            assert!(ar.origin_is_drained(endpoint).unwrap());
            a.security.retire_fenced(old).unwrap();
            let peer = renew_generation(a, ap, am, generation, now + 1);
            let stale = endpoint;
            endpoint = rebind(&mut ar, a, endpoint, peer, now + 3).unwrap();
            assert_ne!(endpoint, stale);
            assert_eq!(ar.origin_is_drained(stale), Err(Error::NotFound));
            assert_eq!(
                ar.fence_origin(stale, &mut a.security, now + 4),
                Err(Error::NotFound)
            );
            assert!(
                a.security
                    .session_get(
                        peer.session,
                        CurrentFacts {
                            now_us: now + 4,
                            ..peer.facts
                        }
                    )
                    .is_ok()
            );
            a.counters.0.retain(|entry| entry.0 != old);
        }
        assert_eq!(
            a.counters.0.len(),
            2,
            "one actual neighbour and one Origin, no history table required"
        );
        assert!(!ar.origin_is_drained(endpoint).unwrap());
    });
}

#[test]
fn stopped_root_cannot_be_reactivated_by_a_fresh_registry_entry() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        let (mut ar, old) = root(a);
        let old = old.unwrap();
        ar.fence_origin(old, &mut a.security, 21).unwrap();
        ar.stop(&mut a.adapter).unwrap();
        let peer = renew(a, ap, am, 22);
        assert!(ar.origin_is_drained(old).unwrap());
        assert_eq!(rebind(&mut ar, a, old, peer, 24), Err(Error::State));
        assert_eq!(send_at(&mut ar, a, old, b"restart", 24), Err(Error::State));
        assert!(ar.quiescent(&a.adapter));
    });
}

#[test]
fn current_identity_expiry_blocks_rebind_even_when_successor_session_is_live() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        let (mut ar, old) = root(a);
        let old = old.unwrap();
        ar.fence_origin(old, &mut a.security, 21).unwrap();
        let peer = renew(a, ap, am, 22);
        assert!(
            a.security
                .session_get(
                    peer.session,
                    CurrentFacts {
                        now_us: 3100,
                        ..peer.facts
                    }
                )
                .is_ok()
        );
        assert_eq!(rebind(&mut ar, a, old, peer, 3100), Err(Error::Access));
        assert!(ar.origin_is_drained(old).unwrap());
    });
}

#[test]
fn encryption_policy_cannot_downgrade_during_registry_handoff() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        a.security.fence(a.peers[2].unwrap().session).unwrap();
        let mut encrypted = successor(0, 2, 2);
        encrypted.origin_level = SecurityLevel::Confidential;
        encrypted.origin_tx.suite = SuiteId::OriginAes128Gcm;
        encrypted.origin_rx.suite = SuiteId::OriginAes128Gcm;
        let session = activate_at(&mut a.security, ap, am, encrypted, DOMAINS[6], 22);
        let facts = CurrentFacts {
            now_us: 24,
            local: encrypted.local,
            peer: encrypted.peer,
            link_generation: encrypted.link_generation,
            policy_generation: 1,
        };
        let mut ar = Root::new(
            route_config(199),
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
        let old = ar
            .register_endpoint(
                session,
                facts,
                encrypted.origin_fingerprint,
                [a.bindings[0], a.bindings[2]],
                DatagramEndpointPolicy {
                    service: ServiceId::new(0x40).unwrap(),
                    traffic_ceiling: TrafficClass::Q1,
                    max_payload_bytes: 64,
                },
                &a.identity,
                &a.security,
                24,
            )
            .unwrap();
        ar.fence_origin(old, &mut a.security, 25).unwrap();
        let plain = successor(0, 2, 3);
        let session = activate_at(&mut a.security, ap, am, plain, DOMAINS[6], 26);
        let peer = JournalPeer {
            session,
            facts: CurrentFacts {
                now_us: 27,
                ..facts
            },
            fingerprint: plain.origin_fingerprint,
            link: a.link,
        };
        assert_eq!(rebind(&mut ar, a, old, peer, 28), Err(Error::Replay));
        assert!(ar.origin_is_drained(old).unwrap());
    });
}

#[test]
fn callback_gate_prevents_both_fence_and_successor_publication() {
    with_mesh_stores(256, true, |a, _, _, ap, am, _, _| {
        let (mut ar, old) = root(a);
        let old = old.unwrap();
        let peer = a.peers[2].unwrap();
        let gate = a.identity.callback_gate();
        let lease = gate
            .try_enter(CallbackClaim::new(300, 1, 1, 1).unwrap())
            .unwrap();
        assert_eq!(ar.fence_origin(old, &mut a.security, 21), Err(Error::State));
        gate.leave(lease).unwrap();
        assert!(
            a.security
                .session_get(
                    peer.session,
                    CurrentFacts {
                        now_us: 21,
                        ..peer.facts
                    }
                )
                .is_ok()
        );
        ar.fence_origin(old, &mut a.security, 22).unwrap();
        let next = renew(a, ap, am, 23);
        let lease = gate
            .try_enter(CallbackClaim::new(300, 1, 1, 2).unwrap())
            .unwrap();
        assert_eq!(rebind(&mut ar, a, old, next, 25), Err(Error::State));
        assert_eq!(ar.origin_is_drained(old), Err(Error::State));
        gate.leave(lease).unwrap();
        assert!(ar.origin_is_drained(old).unwrap());
        assert!(rebind(&mut ar, a, old, next, 26).is_ok());
    });
}
