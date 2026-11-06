//! Opaque A-B-C C1 with independent AC Origin and AB/BC Hop sessions.
//! 独立 AC Origin 和 AB/BC Hop 上下文的真实三节点 C1。
use super::*;
use ucn_wire::CommonHeader;

pub(super) fn endpoint_candidate(local: usize, remote: usize) -> HandshakeCandidate {
    let mut c = initial(fixed_binding(local), fixed_binding(remote), local < remote);
    c.hop_profile = HopProfile::H0;
    c.hop_tx = None;
    c.hop_rx = None;
    c.origin_fingerprint = Fingerprint::new([20; 16]).unwrap();
    c.hop_fingerprint = Fingerprint::new([21; 16]).unwrap();
    c
}
fn access(
    peer: JournalPeer,
    hop: bool,
    security: &SecurityOwner<'_, 4, 8>,
    direction: AccessDirection,
) -> AccessRequest {
    AccessRequest {
        session: peer.session,
        context_fingerprint: if hop {
            security
                .context_fingerprints(peer.session, peer.facts)
                .unwrap()
                .1
        } else {
            peer.fingerprint
        },
        service: ServiceId::new(0x40).unwrap(),
        protocol_opcode: 0,
        direction,
    }
}
fn at(mut peer: JournalPeer, now: u64) -> JournalPeer {
    peer.facts.now_us = now;
    peer
}

/// Genuine Driver TX + atomic RX, never a manually constructed `OpenedPacket`.
fn transmit(from: &mut Node<'_>, to: &mut Node<'_>, packet: &[u8], now: u64) -> Vec<u8> {
    let token = from.adapter.tx_reserve(from.link, 0).unwrap();
    let view = from
        .adapter
        .tx_submit(token, packet, &mut from.driver)
        .unwrap();
    assert_eq!(view.terminal, Some(TerminalOutcome::Success));
    from.adapter.tx_retire(token).unwrap();
    to.ingress
        .rx_publish(
            to.link,
            packet,
            RxMeta {
                timestamp_us: now,
                sender_discriminator: 100 + u32::try_from(from.id).unwrap(),
            },
        )
        .unwrap();
    let mut bytes = [0; 256];
    let rx = to.adapter.rx_claim(&mut bytes).unwrap();
    assert_eq!(
        rx.meta.sender_discriminator,
        100 + u32::try_from(from.id).unwrap()
    );
    to.adapter.rx_retire(rx.token).unwrap();
    bytes[..rx.frame_bytes].to_vec()
}

fn with_endpoints(
    f: impl FnOnce(
        &mut Node<'_>,
        &mut Node<'_>,
        &mut Node<'_>,
        &mut SecurityOwner<'_, 4, 8>,
        JournalPeer,
        &mut SecurityOwner<'_, 4, 8>,
        JournalPeer,
    ),
) {
    with_mesh(|a, b, c| {
        with_store(|ap, am| {
            with_store(|cp, cm| {
                let ac = endpoint_candidate(0, 2);
                let ca = endpoint_candidate(2, 0);
                let ad = rules(&[ac]);
                let cd = rules(&[ca]);
                let aa = acl(&[ac]);
                let cc = acl(&[ca]);
                let mut origin = security_mesh(a.identity.callback_gate(), &ad, &aa);
                let mut target = security_mesh(c.identity.callback_gate(), &cd, &cc);
                let ah = activate(&mut origin, ap, am, ac, ad[0].domain);
                let ch = activate(&mut target, cp, cm, ca, cd[0].domain);
                let peer = |session, candidate: HandshakeCandidate, link| JournalPeer {
                    session,
                    facts: CurrentFacts {
                        local: candidate.local,
                        peer: candidate.peer,
                        link_generation: candidate.link_generation,
                        policy_generation: 1,
                        now_us: 30,
                    },
                    fingerprint: candidate.origin_fingerprint,
                    link,
                };
                f(
                    a,
                    b,
                    c,
                    &mut origin,
                    peer(ah, ac, a.link),
                    &mut target,
                    peer(ch, ca, c.link),
                );
            });
        });
    });
}
struct Sequence(u32);
fn identity(source: Binding, destination: Binding) -> [u8; 46] {
    let mut raw = [0; 46];
    raw[..16].copy_from_slice(&source.principal);
    raw[16..20].copy_from_slice(&source.generation.get().to_be_bytes());
    raw[20..36].copy_from_slice(&destination.principal);
    raw[36..40].copy_from_slice(&destination.generation.get().to_be_bytes());
    raw[40..42].copy_from_slice(&0x40u16.to_be_bytes());
    raw[42..].copy_from_slice(&1u32.to_be_bytes());
    raw
}
impl OriginSequenceOwner for Sequence {
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.0)
    }
    fn burn(&mut self, s: OriginSequence) -> Result<()> {
        assert_eq!(s.get(), self.0);
        self.0 += 1;
        Ok(())
    }
}
fn origin_packet(
    s: &mut SecurityOwner<'_, 4, 8>,
    p: JournalPeer,
    crypto: &mut ReauthCrypto,
    ttl: u8,
) -> Vec<u8> {
    let mut prefix = [0; 13];
    CommonHeader {
        contract: HeaderContract::C1,
        traffic_class: TrafficClass::Q1,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: InteractionRole::OneWay,
        payload_kind: PayloadKind::Data,
        origin_security: OriginSecurity::O1,
        hop_limit: HopLimit::new(ttl).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..5].copy_from_slice(&7u16.to_be_bytes());
    prefix[5..7].copy_from_slice(&9u16.to_be_bytes());
    prefix[7..9].copy_from_slice(&0x40u16.to_be_bytes());
    prefix[9..13].copy_from_slice(&1u32.to_be_bytes());
    let mut out = [0; 256];
    let n = s
        .protect_packet(
            crypto,
            OriginCounterOwner::Sequence(&mut Sequence(1)),
            p.session,
            p.facts,
            access(p, false, s, AccessDirection::Outbound),
            PacketPlan {
                prefix: &prefix,
                payload: b"opaque-origin-payload",
                canonical_identity: &identity(p.facts.local, p.facts.peer),
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: p.fingerprint,
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut SecurityPacketWorkspace::<320>::new(),
            &mut out,
        )
        .unwrap();
    out[..n].to_vec()
}

#[test]
fn resetting_a_shared_hop_counter_cannot_reseal_a_consumed_sequence() {
    with_endpoints(|a, _b, _c, origin, ac, _target, _ca| {
        let bytes = origin_packet(origin, ac, &mut a.crypto, 3);
        let ab = at(a.peers[1].unwrap(), 30);
        let mut work = SecurityPacketWorkspace::<320>::new();
        let mut packet = [0; 256];
        a.security
            .wrap_c1_hop(
                &mut a.crypto,
                ab.session,
                ab.facts,
                access(ab, true, &a.security, AccessDirection::Outbound),
                &bytes,
                &mut Hop(100),
                &mut work,
                &mut packet,
            )
            .unwrap();
        let mut output = [0xA5; 256];
        let mut reset = Hop(100);
        assert_eq!(
            a.security.wrap_c1_hop(
                &mut a.crypto,
                ab.session,
                ab.facts,
                access(ab, true, &a.security, AccessDirection::Outbound),
                &bytes,
                &mut reset,
                &mut work,
                &mut output
            ),
            Err(Error::Replay)
        );
        assert_eq!(reset.0, 100);
        assert_eq!(output, [0xA5; 256]);
    });
}

#[test]
fn independent_end_origin_and_neighbor_hops_deliver_without_reauthenticating_plaintext_at_relay() {
    with_endpoints(|a, b, c, origin, ac, target, ca| {
        discover_path(a, b, c);
        let domain = RouteDomain {
            realm: RealmId::new(1).unwrap(),
            origin: fixed_binding(0),
            origin_session_generation: PeerSessionGeneration::new(1).unwrap(),
            destination: fixed_binding(2),
        };
        for node in [&*a, &*b] {
            let ResolvedRoute::Dynamic(route) = node
                .routes
                .resolve(
                    domain,
                    RouteUseFacts {
                        now_us: 40,
                        origin_session_generation: domain.origin_session_generation,
                        destination_binding_generation: 1,
                        link_generation: LinkInstanceGeneration::new([3, 9][node.id]).unwrap(),
                    },
                )
                .unwrap()
            else {
                panic!("expected actual discovered route");
            };
            assert_eq!(route.next_hop().peer(), fixed_binding(node.id + 1));
        }
        let origin_bytes = origin_packet(origin, at(ac, 40), &mut a.crypto, 3);
        let ab = at(a.peers[1].unwrap(), 40);
        let ba = at(b.peers[0].unwrap(), 41);
        let bc = at(b.peers[2].unwrap(), 41);
        let cb = at(c.peers[1].unwrap(), 42);
        let mut work = SecurityPacketWorkspace::<320>::new();
        let mut first = [0; 256];
        let mut second = [0; 256];
        let n = a
            .security
            .wrap_c1_hop(
                &mut a.crypto,
                ab.session,
                ab.facts,
                access(ab, true, &a.security, AccessDirection::Outbound),
                &origin_bytes,
                &mut Hop(100),
                &mut work,
                &mut first,
            )
            .unwrap();
        assert_eq!(n, 13 + 21 + 16 + 16);
        let incoming = transmit(a, b, &first[..n], 41);
        let opened = b
            .security
            .open_c1_hop(
                &mut b.crypto,
                ba.session,
                ba.facts,
                access(ba, true, &b.security, AccessDirection::Inbound),
                &incoming,
                &mut work,
            )
            .unwrap();
        assert_eq!(opened.header().hop_limit.get(), 3);
        let mut changed = incoming.clone();
        changed[14] ^= 1;
        let sentinel = second;
        assert_eq!(
            b.security.forward_c1_hop(
                &mut b.crypto,
                &opened,
                ba.facts,
                &changed,
                bc.session,
                bc.facts,
                access(bc, true, &b.security, AccessDirection::Outbound),
                &mut Hop(100),
                &mut work,
                &mut second
            ),
            Err(Error::State)
        );
        assert_eq!(second, sentinel);
        let m = b
            .security
            .forward_c1_hop(
                &mut b.crypto,
                &opened,
                ba.facts,
                &incoming,
                bc.session,
                bc.facts,
                access(bc, true, &b.security, AccessDirection::Outbound),
                &mut Hop(100),
                &mut work,
                &mut second,
            )
            .unwrap();
        // No Origin key or Origin open is performed by B. It only changes TTL/H1.
        assert_eq!(&second[..2], &origin_bytes[..2]);
        assert_eq!(&second[3..m - 16], &origin_bytes[3..]);
        assert_eq!(CommonHeader::decode(&second).unwrap().hop_limit.get(), 2);
        b.security.commit_c1_hop(opened, ba.facts).unwrap();
        assert!(
            b.security
                .open_c1_hop(
                    &mut b.crypto,
                    ba.session,
                    ba.facts,
                    access(ba, true, &b.security, AccessDirection::Inbound),
                    &incoming,
                    &mut work
                )
                .is_err()
        );
        let received = transmit(b, c, &second[..m], 42);
        let hop = c
            .security
            .open_c1_hop(
                &mut c.crypto,
                cb.session,
                cb.facts,
                access(cb, true, &c.security, AccessDirection::Inbound),
                &received,
                &mut work,
            )
            .unwrap();
        let ca = at(ca, 42);
        let mut plaintext = [0xA5; 64];
        let end = target
            .open_packet(
                &mut c.crypto,
                ca.session,
                ca.facts,
                access(ca, false, target, AccessDirection::Inbound),
                OpenPacketPlan {
                    packet: &received[..m - 16],
                    prefix_bytes: 13,
                    canonical_identity: &identity(ca.facts.peer, ca.facts.local),
                    origin_context: OriginContext::Sequenced {
                        contract: HeaderContract::C1,
                        fingerprint: ca.fingerprint,
                        sequence: OriginSequence::new(1).unwrap(),
                    },
                    hop_profile: HopProfile::H0,
                },
                &mut work,
                &mut plaintext,
            )
            .unwrap();
        assert_eq!(&plaintext[..end.payload_bytes], b"opaque-origin-payload");
        assert_eq!(end.evidence().identities().1, fixed_binding(0));
        c.security
            .c1_hop_preflight(&hop, cb.facts, &received, &mut work)
            .unwrap();
        target.replay_preflight(&end.replay, ca.facts).unwrap();
        target.replay_commit(end.replay, ca.facts).unwrap();
        c.security.commit_c1_hop(hop, cb.facts).unwrap();
    });
}

#[test]
fn expired_wrong_acl_bad_tag_and_exhausted_ttl_never_produce_forwarding_bytes() {
    with_endpoints(|a, b, _c, origin, ac, _target, _ca| {
        let bytes = origin_packet(origin, ac, &mut a.crypto, 1);
        let ab = at(a.peers[1].unwrap(), 30);
        let ba = at(b.peers[0].unwrap(), 31);
        let bc = at(b.peers[2].unwrap(), 31);
        let mut w = SecurityPacketWorkspace::<320>::new();
        let mut packet = [0; 256];
        let n = a
            .security
            .wrap_c1_hop(
                &mut a.crypto,
                ab.session,
                ab.facts,
                access(ab, true, &a.security, AccessDirection::Outbound),
                &bytes,
                &mut Hop(100),
                &mut w,
                &mut packet,
            )
            .unwrap();
        let mut bad = packet;
        bad[n - 1] ^= 1;
        assert!(
            b.security
                .open_c1_hop(
                    &mut b.crypto,
                    ba.session,
                    ba.facts,
                    access(ba, true, &b.security, AccessDirection::Inbound),
                    &bad[..n],
                    &mut w
                )
                .is_err()
        );
        let mut wrong = access(ba, true, &b.security, AccessDirection::Inbound);
        wrong.service = ServiceId::new(0x41).unwrap();
        assert!(
            b.security
                .open_c1_hop(
                    &mut b.crypto,
                    ba.session,
                    ba.facts,
                    wrong,
                    &packet[..n],
                    &mut w
                )
                .is_err()
        );
        let h = b
            .security
            .open_c1_hop(
                &mut b.crypto,
                ba.session,
                ba.facts,
                access(ba, true, &b.security, AccessDirection::Inbound),
                &packet[..n],
                &mut w,
            )
            .unwrap();
        let mut out = [0xA5; 256];
        let mut counter = Hop(100);
        assert_eq!(
            b.security.forward_c1_hop(
                &mut b.crypto,
                &h,
                ba.facts,
                &packet[..n],
                bc.session,
                bc.facts,
                access(bc, true, &b.security, AccessDirection::Outbound),
                &mut counter,
                &mut w,
                &mut out
            ),
            Err(Error::Exhausted)
        );
        assert_eq!(counter.0, 100);
        assert_eq!(out, [0xA5; 256]);
        assert!(
            b.security
                .c1_hop_preflight(&h, at(ba, 1031).facts, &packet[..n], &mut w)
                .is_err()
        );
        assert!(b.security.commit_c1_hop(h, at(ba, 1031).facts).is_err());
        // Failed commit released the reservation. The uncommitted frame can be retried.
        let h = b
            .security
            .open_c1_hop(
                &mut b.crypto,
                ba.session,
                at(ba, 1032).facts,
                access(at(ba, 1032), true, &b.security, AccessDirection::Inbound),
                &packet[..n],
                &mut w,
            )
            .unwrap();
        b.security.abort_c1_hop(h).unwrap();
    });
}

#[test]
fn valid_neighbor_tag_cannot_turn_a_corrupt_origin_into_an_endpoint_message() {
    with_endpoints(|a, b, c, origin, ac, target, ca| {
        let mut origin_bytes = origin_packet(origin, ac, &mut a.crypto, 3);
        origin_bytes[15] ^= 1;
        let ab = at(a.peers[1].unwrap(), 30);
        let ba = at(b.peers[0].unwrap(), 31);
        let bc = at(b.peers[2].unwrap(), 31);
        let cb = at(c.peers[1].unwrap(), 32);
        let mut w = SecurityPacketWorkspace::<320>::new();
        let mut first = [0; 256];
        let mut second = [0; 256];
        let n = a
            .security
            .wrap_c1_hop(
                &mut a.crypto,
                ab.session,
                ab.facts,
                access(ab, true, &a.security, AccessDirection::Outbound),
                &origin_bytes,
                &mut Hop(100),
                &mut w,
                &mut first,
            )
            .unwrap();
        let input = transmit(a, b, &first[..n], 31);
        let h = b
            .security
            .open_c1_hop(
                &mut b.crypto,
                ba.session,
                ba.facts,
                access(ba, true, &b.security, AccessDirection::Inbound),
                &input,
                &mut w,
            )
            .unwrap();
        let n = b
            .security
            .forward_c1_hop(
                &mut b.crypto,
                &h,
                ba.facts,
                &input,
                bc.session,
                bc.facts,
                access(bc, true, &b.security, AccessDirection::Outbound),
                &mut Hop(100),
                &mut w,
                &mut second,
            )
            .unwrap();
        b.security.commit_c1_hop(h, ba.facts).unwrap();
        let input = transmit(b, c, &second[..n], 32);
        let h = c
            .security
            .open_c1_hop(
                &mut c.crypto,
                cb.session,
                cb.facts,
                access(cb, true, &c.security, AccessDirection::Inbound),
                &input,
                &mut w,
            )
            .unwrap();
        let ca = at(ca, 32);
        let mut output = [0xA5; 64];
        assert!(matches!(
            target.open_packet(
                &mut c.crypto,
                ca.session,
                ca.facts,
                access(ca, false, target, AccessDirection::Inbound),
                OpenPacketPlan {
                    packet: &input[..n - 16],
                    prefix_bytes: 13,
                    canonical_identity: &identity(ca.facts.peer, ca.facts.local),
                    origin_context: OriginContext::Sequenced {
                        contract: HeaderContract::C1,
                        fingerprint: ca.fingerprint,
                        sequence: OriginSequence::new(1).unwrap()
                    },
                    hop_profile: HopProfile::H0
                },
                &mut w,
                &mut output
            ),
            Err(Error::Security)
        ));
        assert_eq!(output, [0xA5; 64]);
        c.security.abort_c1_hop(h).unwrap();
    });
}
