//! Real durable JOIN -> exact Session handoff -> H1 authenticated Capability exchange.
//! 真实耐久JOIN、精确Session交接及H1能力交换；不伪造OpenedPacket或认证View。
use super::*;
use ucn_capability::*;
const OPS: [ProtocolOpcode; 8] = [
    ProtocolOpcode::NeighborHello,
    ProtocolOpcode::NeighborKeepalive,
    ProtocolOpcode::CapabilityQuery,
    ProtocolOpcode::CapabilityAdvertise,
    ProtocolOpcode::CapabilitySelect,
    ProtocolOpcode::CapabilityAck,
    ProtocolOpcode::CapabilityRevoke,
    ProtocolOpcode::CapabilityFragment,
];
pub(super) fn acl(t: &BootstrapTranscript, failure: Failure) -> (Vec<AclRule>, Vec<AclRule>) {
    let mut t = device_transcript(authority_transcript(*t));
    t.selected_hop_suite = 16;
    t.proposed_address = 7;
    t.address_binding_generation = 1;
    t.binding_lease_id = [0x61; 16];
    t.binding_lease_duration_us = 5000;
    t.binding_mode = 2;
    let c = join_session_candidate(
        t,
        AddressWidth::A1,
        true,
        3,
        JoinSessionPolicy {
            generation: 1,
            lifetime_us: 10_000,
        },
        21,
        5020,
    )
    .unwrap();
    let build = |principal, denied| {
        OPS.into_iter()
            .flat_map(|op| {
                [AccessDirection::Inbound, AccessDirection::Outbound].map(move |direction| {
                    AclRule {
                        peer_principal: principal,
                        peer_binding_generation: BindingGeneration::active(1).unwrap(),
                        context_fingerprint: c.origin_fingerprint,
                        service: ServiceId::new(if denied { 0x41 } else { 0x40 }).unwrap(),
                        protocol_opcode: op.into(),
                        direction,
                    }
                })
            })
            .filter(|r| {
                !(failure == Failure::FragmentDenied
                    && principal == [0x41; 16]
                    && r.protocol_opcode == u16::from(ProtocolOpcode::CapabilityFragment))
                    && !(failure == Failure::FragmentOriginalDenied
                        && principal == [0x41; 16]
                        && r.direction == AccessDirection::Inbound
                        && r.protocol_opcode == u16::from(ProtocolOpcode::CapabilityAdvertise))
            })
            .collect()
    };
    (
        build([0x41; 16], failure == Failure::CapabilityDenied),
        build([0x11; 16], false),
    )
}
pub(super) fn record(link: u32, mtu: u32) -> CapabilityRecord {
    CapabilityRecord {
        capability_generation: 1,
        link: LinkCapability {
            link_instance_generation: link,
            carrier_mtu: mtu + 8,
            link_frame_mtu: mtu,
            processing_frame_mtu: mtu,
            carrier_header_bytes: 4,
            carrier_padding_bytes: 0,
            carrier_crc_bytes: 4,
            carrier_tag_bytes: 0,
            carrier_max_fragments: 1,
            link_flags: 8,
            nominal_rate_bps: 1_000_000,
            hardware_priority_count: 1,
            timestamp_capability_bits: 0,
            timestamp_uncertainty_us: 0,
        },
        peer: PeerCapability {
            feature_bits: 0x010b,
            hop_suite_bits: 2,
            e2e_suite_bits: 2,
            max_message_class: MessageClass::T128,
            max_rx_window: 1,
            max_concurrent_transfers: 1,
            realtime_mode_bits: 0,
            clock_domain_id: 0,
            clock_domain_generation: 0,
        },
    }
}
pub(super) fn requirements() -> ProfileRequirements {
    ProfileRequirements {
        required_feature_bits: 0x010b,
        required_hop_suite_bits: 2,
        required_e2e_suite_bits: 2,
        minimum_message_class: MessageClass::T128,
        minimum_rx_window: 1,
        minimum_concurrent_transfers: 1,
        required_realtime_mode_bits: 0,
    }
}
struct Counter(u64);
impl C0TransactionOwner for Counter {
    fn preview(&self) -> Result<C0TransactionId> {
        C0TransactionId::new(self.0)
    }
    fn burn(&mut self, v: C0TransactionId) -> Result<()> {
        if v.get() != self.0 {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}
struct Hop(u32);
impl HopSequenceOwner for Hop {
    fn preview(&self) -> Result<HopSequence> {
        HopSequence::new(self.0)
    }
    fn burn(&mut self, v: HopSequence) -> Result<()> {
        if v.get() != self.0 {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}
fn at(mut p: JournalPeer, now: u64) -> JournalPeer {
    p.facts.now_us = now;
    p
}
fn access(p: JournalPeer, service: u16) -> AccessRequest {
    AccessRequest {
        session: p.session,
        context_fingerprint: p.fingerprint,
        service: ServiceId::new(service).unwrap(),
        protocol_opcode: ProtocolOpcode::NeighborHello.into(),
        direction: AccessDirection::Outbound,
    }
}
#[allow(clippy::too_many_arguments)]
fn protect_fragment_like(
    owner: &mut SecurityOwner<'_, 2, 1>,
    crypto: &mut Crypto,
    counter: &mut Counter,
    hop: &mut Hop,
    peer: JournalPeer,
    template: &[u8],
    body: &[u8],
) -> Vec<u8> {
    let tx = counter.preview().unwrap();
    let mut prefix = [0; 29];
    prefix.copy_from_slice(&template[..29]);
    prefix[19..27].copy_from_slice(&tx.get().to_be_bytes());
    let mut out = [0; 128];
    let n = owner
        .protect_packet(
            crypto,
            OriginCounterOwner::Transaction(counter),
            peer.session,
            peer.facts,
            AccessRequest {
                protocol_opcode: ProtocolOpcode::CapabilityFragment.into(),
                ..access(peer, 0x40)
            },
            PacketPlan {
                prefix: &prefix,
                payload: body,
                canonical_identity: &prefix[3..],
                origin_context: OriginContext::C0 {
                    fingerprint: peer.fingerprint,
                    transaction_id: tx,
                    opcode: ProtocolOpcode::CapabilityFragment,
                    direction: if peer.facts.local.principal < peer.facts.peer.principal {
                        SenderDirection::AscendingPrincipal
                    } else {
                        SenderDirection::DescendingPrincipal
                    },
                },
                hop_profile: HopProfile::H1,
            },
            Some(hop),
            &mut SecurityPacketWorkspace::<192>::new(),
            &mut out,
        )
        .unwrap();
    out[..n].to_vec()
}
fn refresh_adversarial(
    cs: &mut SecurityOwner<'_, 2, 1>,
    crypto: &mut Crypto,
    p: JournalPeer,
    hop: &mut Hop,
    packet: &[u8],
) {
    let mut bytes = packet.to_vec();
    let old_counter = hop.0;
    let mut scratch = SecurityPacketWorkspace::<192>::new();
    assert_eq!(
        cs.refresh_c0_hop(
            crypto,
            p.session,
            p.facts,
            access(p, 0x41),
            hop,
            &mut scratch,
            &mut bytes
        ),
        Err(Error::Access)
    );
    assert_eq!((bytes.as_slice(), hop.0), (packet, old_counter));
    let mut old_hop = Hop(1);
    assert_eq!(
        cs.refresh_c0_hop(
            crypto,
            p.session,
            p.facts,
            access(p, 0x40),
            &mut old_hop,
            &mut scratch,
            &mut bytes
        ),
        Err(Error::State)
    );
    assert_eq!(bytes, packet);
    crypto.fail_hop = true;
    assert_eq!(
        cs.refresh_c0_hop(
            crypto,
            p.session,
            p.facts,
            access(p, 0x40),
            hop,
            &mut scratch,
            &mut bytes
        ),
        Err(Error::Security)
    );
    crypto.fail_hop = false;
    assert_eq!(bytes, packet);
    assert_eq!(hop.0, old_counter + 1); // Exposed Hop value is never reused.
    let mut short = SecurityPacketWorkspace::<32>::new();
    let before = hop.0;
    assert_eq!(
        cs.refresh_c0_hop(
            crypto,
            p.session,
            p.facts,
            access(p, 0x40),
            hop,
            &mut short,
            &mut bytes
        ),
        Err(Error::NoSpace)
    );
    assert_eq!((bytes.as_slice(), hop.0), (packet, before));
    let mut exhausted = Hop(0);
    assert!(
        cs.refresh_c0_hop(
            crypto,
            p.session,
            p.facts,
            access(p, 0x40),
            &mut exhausted,
            &mut scratch,
            &mut bytes
        )
        .is_err()
    );
    let expired = at(p, 5020);
    assert!(
        cs.refresh_c0_hop(
            crypto,
            expired.session,
            expired.facts,
            access(expired, 0x40),
            hop,
            &mut scratch,
            &mut bytes
        )
        .is_err()
    );
    assert_eq!((bytes.as_slice(), hop.0), (packet, before));
}
#[allow(clippy::too_many_arguments)]
fn receive<const B: usize>(
    root: &mut CapabilityRuntime<'_, B>,
    p: JournalPeer,
    s: &mut SecurityOwner<'_, 2, 1>,
    crypto: &mut Crypto,
    adapter: &mut Adapter<'_, B>,
    ingress: &Ingress<B>,
    bytes: &[u8],
) -> Result<()> {
    publish(ingress, p.link, bytes, p.facts.now_us, 1);
    root.receive(
        p,
        s,
        crypto,
        adapter,
        &mut SecurityPacketWorkspace::<192>::new(),
    )
}
#[allow(clippy::too_many_arguments, clippy::too_many_lines)]
pub(super) fn exercise<'gate, const B: usize>(
    client: &mut BootstrapJoin<'_, 'gate, B>,
    authority: &mut BootstrapJoin<'_, 'gate, B>,
    ci: &IdentityOwner<'_, 1>,
    ai: &IdentityOwner<'_, 1>,
    cs: &mut SecurityOwner<'gate, 2, 1>,
    as_: &mut SecurityOwner<'gate, 2, 1>,
    cc: &mut Crypto,
    ac: &mut Crypto,
    ca: &mut Adapter<'_, B>,
    aa: &mut Adapter<'_, B>,
    cx: &Ingress<B>,
    ax: &Ingress<B>,
    cd: &mut Driver,
    ad: &mut Driver,
    failure: Failure,
    start: u64,
) {
    let mtu = match failure {
        Failure::Capability | Failure::CapabilityDenied => 136,
        Failure::Capability127 | Failure::FragmentUnknown | Failure::CapabilityMixed => 127,
        _ => 128,
    };
    let old_frames = (cd.frames.len(), ad.frames.len());
    assert!(matches!(
        client.handoff_capability(
            ca,
            ci,
            cs,
            70,
            record(3, mtu),
            requirements(),
            ServiceId::new(0x40).unwrap(),
            1000,
            20,
            1,
            4000,
            2000,
            start
        ),
        Err(Error::Config)
    ));
    assert!(client.session(ca, ci, cs, start).is_ok());
    assert_eq!(old_frames, (cd.frames.len(), ad.frames.len()));
    let result = client.handoff_capability(
        ca,
        ci,
        cs,
        40,
        record(
            3,
            if failure == Failure::CapabilitySmall {
                119
            } else {
                mtu
            },
        ),
        requirements(),
        ServiceId::new(0x40).unwrap(),
        1000,
        20,
        50,
        4000,
        2000,
        start,
    );
    if failure == Failure::CapabilitySmall {
        assert!(matches!(result, Err(Error::Config)));
        assert!(client.session(ca, ci, cs, start).is_ok());
        assert_eq!(old_frames, (cd.frames.len(), ad.frames.len()));
        return;
    }
    let (mut source, cp) = result.unwrap();
    let (mut target, ap) = authority
        .handoff_capability(
            aa,
            ai,
            as_,
            50,
            record(
                9,
                if failure == Failure::CapabilityMixed {
                    136
                } else {
                    mtu
                },
            ),
            requirements(),
            ServiceId::new(0x40).unwrap(),
            1000,
            20,
            50,
            4000,
            2000,
            start,
        )
        .unwrap();
    assert!(matches!(
        client.handoff_capability(
            ca,
            ci,
            cs,
            60,
            record(3, mtu),
            requirements(),
            ServiceId::new(0x40).unwrap(),
            1000,
            20,
            50,
            4000,
            2000,
            start
        ),
        Err(Error::State)
    ));
    assert_eq!(client.drive(ca, cd, start), Err(Error::State));
    assert_eq!(
        client.receive(ca, &mut SignedEvents(Cookies::new()), start),
        Err(Error::State)
    );
    assert_eq!(client.stop(ca, cs), Err(Error::State));
    assert!(client.session(ca, ci, cs, start).is_ok());
    if failure == Failure::CapabilityDenied {
        assert_eq!(source.start(cp, cs, ca), Err(Error::Access));
        assert_eq!(source.phase(), CapabilityPhase::Idle);
        assert_eq!(old_frames, (cd.frames.len(), ad.frames.len()));
        return;
    }
    source.start(cp, cs, ca).unwrap();
    let (mut ct, mut atx) = (Counter(1), Counter(1));
    let (mut ch, mut ah) = (Hop(1), Hop(1));
    assert_eq!(
        source.drive(
            cp,
            cs,
            cc,
            &mut ct,
            ca,
            cd,
            &mut SecurityPacketWorkspace::<192>::new()
        ),
        Err(Error::Argument)
    );
    assert_eq!((ct.0, ch.0), (1, 1));
    let (mut cpos, mut apos) = old_frames;
    cd.submit = DriverSubmit::NotSubmitted;
    let (mut lost_query, mut lost_ack, mut corrupted) = (false, false, false);
    let mut reply_backpressure = false;
    let mut fragment_corrupted = false;
    let mut fragment_preflighted = false;
    let mut old_fragment_replayed = false;
    let mut seal_failed = false;
    if failure == Failure::FragmentSealFault {
        cc.fail_fragment_origin_once = true;
    }
    if mtu < 136 {
        ad.fragment_submit = Some(if failure == Failure::FragmentUnknown {
            DriverSubmit::Unknown
        } else {
            DriverSubmit::NotSubmitted
        });
    }
    let mut c_packets: Vec<Vec<u8>> = vec![];
    let mut a_packets: Vec<Vec<u8>> = vec![];
    let mut finished = None;
    for now in start..start + 950 {
        let cp = at(cp, now);
        let ap = at(ap, now);
        let sent = source.drive_with_hop(
            cp,
            cs,
            cc,
            &mut ct,
            Some(&mut ch),
            ca,
            cd,
            &mut SecurityPacketWorkspace::<192>::new(),
        );
        if failure == Failure::FragmentDenied && sent == Err(Error::Access) {
            assert!(!c_packets.iter().any(|p| p[27..29] == [1, 8]));
            assert_ne!(source.phase(), CapabilityPhase::Confirmed);
            return;
        }
        if failure == Failure::FragmentSealFault && sent == Err(Error::Security) {
            assert!(!seal_failed && !cc.fail_fragment_origin_once);
            assert!(!c_packets.iter().any(|p| p[27..29] == [1, 8]));
            assert!(ct.0 > 2); // Both attempted nonces burned, neither fragment published.
            seal_failed = true;
            continue;
        }
        sent.unwrap();
        while cpos < cd.frames.len() {
            let bytes = cd.frames[cpos].clone();
            cpos += 1;
            c_packets.push(bytes.clone());
            if cd.submit == DriverSubmit::NotSubmitted {
                refresh_adversarial(cs, cc, cp, &mut ch, &bytes);
                cd.submit = DriverSubmit::Complete(TerminalOutcome::Success);
                continue;
            }
            if !corrupted {
                let mut bad = bytes.clone();
                *bad.last_mut().unwrap() ^= 1;
                assert_eq!(
                    receive(&mut target, ap, as_, ac, aa, ax, &bad),
                    Err(Error::Security)
                );
                assert_eq!(target.phase(), CapabilityPhase::Idle);
                // Valid new Hop tag must not mask a corrupted Origin tag.
                let mut bad_origin = bytes.clone();
                let end = bad_origin.len() - 17;
                bad_origin[end] ^= 1;
                cs.refresh_c0_hop(
                    cc,
                    cp.session,
                    cp.facts,
                    access(cp, 0x40),
                    &mut ch,
                    &mut SecurityPacketWorkspace::<192>::new(),
                    &mut bad_origin,
                )
                .unwrap();
                assert_eq!(
                    receive(&mut target, ap, as_, ac, aa, ax, &bad_origin),
                    Err(Error::Security)
                );
                assert_eq!(target.phase(), CapabilityPhase::Idle);
                corrupted = true;
            }
            let r = receive(&mut target, ap, as_, ac, aa, ax, &bytes);
            assert!(matches!(r, Ok(()) | Err(Error::Replay)), "target {r:?}");
            if bytes[27..29] == [1, 8] && bytes[33] == 0 && !fragment_preflighted {
                let mut body = bytes[29..bytes.len() - 32].to_vec();
                body[5] = 1;
                let malformed = protect_fragment_like(cs, cc, &mut ct, &mut ch, cp, &bytes, &body);
                assert_eq!(
                    receive(&mut target, ap, as_, ac, aa, ax, &malformed),
                    Err(Error::Malformed)
                );
                body[5] = 2;
                let competing = protect_fragment_like(cs, cc, &mut ct, &mut ch, cp, &bytes, &body);
                assert_eq!(
                    receive(&mut target, ap, as_, ac, aa, ax, &competing),
                    Err(Error::NoSpace)
                );
                assert_eq!(target.phase(), CapabilityPhase::Querying);
                fragment_preflighted = true;
            }
        }
        let target_sent = target.drive_with_hop(
            ap,
            as_,
            ac,
            &mut atx,
            Some(&mut ah),
            aa,
            ad,
            &mut SecurityPacketWorkspace::<192>::new(),
        );
        assert!(
            target_sent.is_ok()
                || failure == Failure::FragmentUnknown && target_sent == Err(Error::InDoubt)
        );
        while apos < ad.frames.len() {
            let bytes = ad.frames[apos].clone();
            apos += 1;
            a_packets.push(bytes.clone());
            let raw_op =
                ProtocolOpcode::try_from(u16::from_be_bytes([bytes[27], bytes[28]])).unwrap();
            let op = if raw_op == ProtocolOpcode::CapabilityFragment {
                ProtocolOpcode::try_from(u16::from_be_bytes([bytes[29], bytes[30]])).unwrap()
            } else {
                raw_op
            };
            if ad.last_submit == DriverSubmit::Unknown {
                assert_eq!(raw_op, ProtocolOpcode::CapabilityFragment);
                assert_eq!(bytes[33], 1);
                assert_eq!(failure, Failure::FragmentUnknown);
                let token = *ad.tokens.last().unwrap();
                target.stop(aa).unwrap();
                assert!(target.retire(aa, ad, 5).is_err());
                assert!(aa.tx_view(token).is_ok());
                assert!(!target.quiescent(aa));
                return;
            }
            if ad.last_submit == DriverSubmit::NotSubmitted {
                assert_eq!(raw_op, ProtocolOpcode::CapabilityFragment);
                assert_eq!(bytes[33], 1);
                reply_backpressure = true;
                continue;
            }
            if op == ProtocolOpcode::CapabilityQuery && !lost_query {
                lost_query = true;
                continue;
            }
            if op == ProtocolOpcode::CapabilityAck && !lost_ack {
                lost_ack = true;
                continue;
            }
            if raw_op == ProtocolOpcode::CapabilityFragment && !fragment_corrupted {
                let mut bad = bytes.clone();
                let tag = bad.len() - 17;
                bad[tag] ^= 1;
                as_.refresh_c0_hop(
                    ac,
                    ap.session,
                    ap.facts,
                    AccessRequest {
                        protocol_opcode: ProtocolOpcode::CapabilityFragment.into(),
                        ..access(ap, 0x40)
                    },
                    &mut ah,
                    &mut SecurityPacketWorkspace::<192>::new(),
                    &mut bad,
                )
                .unwrap();
                assert_eq!(
                    receive(&mut source, cp, cs, cc, ca, cx, &bad),
                    Err(Error::Security)
                );
                assert_ne!(source.phase(), CapabilityPhase::Confirmed);
                fragment_corrupted = true;
            }
            let r = receive(&mut source, cp, cs, cc, ca, cx, &bytes);
            if failure == Failure::FragmentOriginalDenied
                && op == ProtocolOpcode::CapabilityAdvertise
            {
                assert_eq!(r, Err(Error::Access));
                assert_ne!(source.phase(), CapabilityPhase::Confirmed);
                return;
            }
            assert!(
                matches!(r, Ok(()) | Err(Error::Replay)),
                "source {op:?} {r:?}"
            );
            if raw_op == ProtocolOpcode::CapabilityFragment
                && op == ProtocolOpcode::CapabilityAck
                && source.phase() == CapabilityPhase::Selecting
                && !old_fragment_replayed
            {
                let mut old = a_packets
                    .iter()
                    .find(|p| p[27..31] == [1, 8, 1, 3])
                    .unwrap()
                    .clone();
                as_.refresh_c0_hop(
                    ac,
                    ap.session,
                    ap.facts,
                    AccessRequest {
                        protocol_opcode: ProtocolOpcode::CapabilityFragment.into(),
                        ..access(ap, 0x40)
                    },
                    &mut ah,
                    &mut SecurityPacketWorkspace::<192>::new(),
                    &mut old,
                )
                .unwrap();
                assert_eq!(
                    receive(&mut source, cp, cs, cc, ca, cx, &old),
                    Err(Error::Replay)
                );
                assert_eq!(source.phase(), CapabilityPhase::Selecting);
                old_fragment_replayed = true;
            }
            if failure == Failure::FragmentTimeout && op == ProtocolOpcode::CapabilityAck {
                assert_eq!(source.phase(), CapabilityPhase::Selecting);
                assert_eq!(source.step(start + 1000), Err(Error::Timeout));
                assert_eq!(source.phase(), CapabilityPhase::Fenced);
                assert!(
                    source
                        .receive(
                            at(cp, start + 1000),
                            cs,
                            cc,
                            ca,
                            &mut SecurityPacketWorkspace::<192>::new()
                        )
                        .is_err()
                );
                return;
            }
        }
        source.retire(ca, cd, 5).unwrap();
        target.retire(aa, ad, 5).unwrap();
        if source.phase() == CapabilityPhase::Confirmed
            && target.phase() == CapabilityPhase::Confirmed
        {
            finished = Some(now);
            break;
        }
    }
    let now = finished.expect("real H1 Capability exchange must converge");
    assert!(lost_query && lost_ack && corrupted);
    assert_eq!(
        source.selected(at(cp, now), cs, ca).unwrap(),
        target.selected(at(ap, now), as_, aa).unwrap().reverse()
    );
    for packets in [&c_packets, &a_packets] {
        for (index, packet) in packets.iter().enumerate() {
            assert!(packet.len() <= usize::try_from(mtu).unwrap());
            let hop =
                |p: &[u8]| u32::from_be_bytes(p[p.len() - 16..p.len() - 12].try_into().unwrap());
            for old in &packets[..index] {
                assert!(hop(packet) > hop(old));
                if old[19..27] == packet[19..27] {
                    assert_eq!(old[..old.len() - 16], packet[..packet.len() - 16]);
                }
            }
        }
    }
    if mtu >= 136 {
        assert!(a_packets.iter().any(|p| p.len() == 136));
    } else {
        assert!(reply_backpressure);
        assert!(fragment_corrupted);
        assert!(fragment_preflighted && old_fragment_replayed);
        assert!(a_packets.iter().any(|p| p[27..29] == [1, 8]));
    }
    if failure == Failure::FragmentSealFault {
        assert!(seal_failed);
    }
    let old = c_packets
        .iter()
        .find(|p| u16::from_be_bytes([p[27], p[28]]) == u16::from(ProtocolOpcode::CapabilitySelect))
        .unwrap();
    assert_eq!(
        receive(&mut target, at(ap, now), as_, ac, aa, ax, old),
        Err(Error::Replay)
    );
    assert_eq!(target.phase(), CapabilityPhase::Confirmed);
    ca.reopen_link(cp.link, 4, usize::try_from(mtu).unwrap())
        .unwrap();
    assert!(source.selected(at(cp, now), cs, ca).is_err());
    assert!(client.session(ca, ci, cs, now).is_err());
    assert!(target.selected(at(ap, 5020), as_, aa).is_err());
    target.stop(aa).unwrap();
    source.stop(ca).unwrap();
    for _ in 0..3 {
        target.retire(aa, ad, 5).unwrap();
        source.retire(ca, cd, 5).unwrap();
    }
    assert!(target.quiescent(aa) && source.quiescent(ca));
}
#[test]
fn durable_join_h1_capability_retries_exact_origin_with_fresh_hop() {
    exercise_join(Failure::Capability);
}
#[test]
fn h1_small_mtu_rejects_without_handoff_or_downgrade() {
    exercise_join(Failure::CapabilitySmall);
}
#[test]
fn active_join_session_does_not_bypass_capability_opcode_acl() {
    exercise_join(Failure::CapabilityDenied);
}
#[test]
fn real_127_byte_h1_fragments_reassemble_after_loss_and_backpressure() {
    exercise_join_buffer::<128>(Failure::Capability127);
}
#[test]
fn real_128_byte_h1_fragments_reassemble_after_loss_and_backpressure() {
    exercise_join_buffer::<128>(Failure::Capability128);
}
#[test]
fn fragment_opcode_requires_its_own_exact_acl() {
    exercise_join(Failure::FragmentDenied);
}
#[test]
fn second_fragment_unknown_retains_physical_holder_on_stop() {
    exercise_join(Failure::FragmentUnknown);
}
#[test]
fn authenticated_fragment_cannot_bypass_original_opcode_acl() {
    exercise_join(Failure::FragmentOriginalDenied);
}
#[test]
fn partial_ack_cannot_renew_original_handshake_deadline() {
    exercise_join(Failure::FragmentTimeout);
}
#[test]
fn second_fragment_crypto_failure_burns_pair_without_partial_publication() {
    exercise_join(Failure::FragmentSealFault);
}
#[test]
fn mixed_127_136_links_never_guess_remote_receive_mtu_from_local_mtu() {
    exercise_join(Failure::CapabilityMixed);
}
