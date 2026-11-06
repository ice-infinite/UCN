#![cfg(feature = "capability-control")]
//! Real Session reload, raw authenticated frames, atomic RX and Driver obligations.
//! 实际会话回读、认证原始帧、原子RX和Driver义务，不伪造认证DTO。
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::{Counter, Driver, security};
use ucn_adapter::*;
use ucn_capability::*;
use ucn_runtime::*;
use ucn_security::*;
use ucn_types::*;
use ucn_wire::CommonHeader;
const OPS: &[ProtocolOpcode] = &[
    ProtocolOpcode::NeighborHello,
    ProtocolOpcode::NeighborKeepalive,
    ProtocolOpcode::CapabilityQuery,
    ProtocolOpcode::CapabilityAdvertise,
    ProtocolOpcode::CapabilitySelect,
    ProtocolOpcode::CapabilityAck,
    ProtocolOpcode::CapabilityRevoke,
];
type Ingress = AdapterEventIngress<1, 1, 1, 128>;
type Adapter<'a> = AdapterOwner<'a, 1, 1, 1, 128>;
fn record(generation: u32, window: u16) -> CapabilityRecord {
    CapabilityRecord {
        capability_generation: generation,
        link: LinkCapability {
            link_instance_generation: 1,
            carrier_mtu: 140,
            link_frame_mtu: 128,
            processing_frame_mtu: 128,
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
            e2e_suite_bits: 14,
            max_message_class: MessageClass::T128,
            max_rx_window: window,
            max_concurrent_transfers: 2,
            realtime_mode_bits: 0,
            clock_domain_id: 0,
            clock_domain_generation: 0,
        },
    }
}
fn requirements() -> ProfileRequirements {
    ProfileRequirements {
        required_feature_bits: 0x010b,
        required_hop_suite_bits: 2,
        required_e2e_suite_bits: 14,
        minimum_message_class: MessageClass::T128,
        minimum_rx_window: 1,
        minimum_concurrent_transfers: 1,
        required_realtime_mode_bits: 0,
    }
}
fn peer(session: SessionHandle, facts: CurrentFacts, link: LinkHandle, now: u64) -> JournalPeer {
    JournalPeer {
        session,
        facts: CurrentFacts {
            now_us: now,
            ..facts
        },
        link,
        fingerprint: Fingerprint::new([5; 16]).unwrap(),
    }
}
fn receive(
    root: &mut CapabilityRuntime<'_, 128>,
    p: JournalPeer,
    owner: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    adapter: &mut Adapter<'_>,
    ingress: &Ingress,
    bytes: &[u8],
) -> Result<()> {
    ingress.rx_publish(
        p.link,
        bytes,
        RxMeta {
            timestamp_us: p.facts.now_us,
            sender_discriminator: 1,
        },
    )?;
    root.receive(
        p,
        owner,
        crypto,
        adapter,
        &mut SecurityPacketWorkspace::<128>::new(),
    )
}
fn protect(
    owner: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    p: JournalPeer,
    op: ProtocolOpcode,
    body: &[u8],
    tx: u64,
) -> Vec<u8> {
    let mut prefix = [0; 29];
    let role = if op == ProtocolOpcode::CapabilityAck {
        InteractionRole::Result
    } else if matches!(
        op,
        ProtocolOpcode::CapabilityQuery | ProtocolOpcode::CapabilitySelect
    ) {
        InteractionRole::Request
    } else {
        InteractionRole::OneWay
    };
    CommonHeader {
        contract: HeaderContract::C0,
        origin_security: OriginSecurity::O1,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: role,
        payload_kind: PayloadKind::Control,
        traffic_class: TrafficClass::Q2,
        hop_limit: HopLimit::new(1).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..7].copy_from_slice(&1u32.to_be_bytes());
    prefix[7..9].copy_from_slice(
        &u16::try_from(p.facts.local.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[9..11].copy_from_slice(
        &u16::try_from(p.facts.peer.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[11..15].copy_from_slice(&p.facts.local.generation.get().to_be_bytes());
    prefix[15..19].copy_from_slice(&p.facts.peer.generation.get().to_be_bytes());
    prefix[19..27].copy_from_slice(&tx.to_be_bytes());
    prefix[27..29].copy_from_slice(&u16::from(op).to_be_bytes());
    let mut output = [0; 128];
    let n = owner
        .protect_packet(
            crypto,
            OriginCounterOwner::Transaction(&mut Counter(tx)),
            p.session,
            p.facts,
            AccessRequest {
                session: p.session,
                context_fingerprint: p.fingerprint,
                service: ServiceId::new(0x40).unwrap(),
                protocol_opcode: u16::from(op),
                direction: AccessDirection::Outbound,
            },
            PacketPlan {
                prefix: &prefix,
                payload: body,
                canonical_identity: &prefix[3..],
                origin_context: OriginContext::C0 {
                    fingerprint: p.fingerprint,
                    transaction_id: C0TransactionId::new(tx).unwrap(),
                    opcode: op,
                    direction: if p.facts.local.principal < p.facts.peer.principal {
                        SenderDirection::AscendingPrincipal
                    } else {
                        SenderDirection::DescendingPrincipal
                    },
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut SecurityPacketWorkspace::<128>::new(),
            &mut output,
        )
        .unwrap();
    output[..n].to_vec()
}
#[test]
#[allow(clippy::too_many_lines)]
fn actual_handshake_losses_backpressure_exact_retry_and_live_use_gate() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let si = Ingress::new(1).unwrap();
            let ri = Ingress::new(1).unwrap();
            let mut sa = Adapter::new(1, &si).unwrap();
            let mut ra = Adapter::new(1, &ri).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            let rl = ra.open_link(0, 1, 128).unwrap();
            sa.try_set_rx_enabled(true).unwrap();
            ra.try_set_rx_enabled(true).unwrap();
            let sp = peer(sh, sf, sl, 21);
            let rp = peer(rh, rf, rl, 21);
            let mut source = CapabilityRuntime::<128>::new(
                10,
                CapabilityRole::Initiator,
                sp,
                sender,
                &sa,
                record(1, 4),
                requirements(),
                ServiceId::new(0x40).unwrap(),
                2000,
                50,
                20,
                5000,
                1000,
            )
            .unwrap();
            let mut target = CapabilityRuntime::<128>::new(
                20,
                CapabilityRole::Responder,
                rp,
                receiver,
                &ra,
                record(7, 2),
                requirements(),
                ServiceId::new(0x40).unwrap(),
                2000,
                50,
                20,
                5000,
                1000,
            )
            .unwrap();
            source.start(sp, sender, &sa).unwrap();
            let mut sd = Driver::new(DriverSubmit::NotSubmitted);
            let mut rd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut sct = Counter(1);
            let mut rct = Counter(1);
            let mut sx = 0;
            let mut rx = 0;
            let mut lost = [false; 4];
            let mut confirmed_at = 0;
            for now in 21..1800 {
                let sp = peer(sh, sf, sl, now);
                let rp = peer(rh, rf, rl, now);
                source
                    .drive(
                        sp,
                        sender,
                        sc,
                        &mut sct,
                        &mut sa,
                        &mut sd,
                        &mut SecurityPacketWorkspace::<128>::new(),
                    )
                    .unwrap();
                while sx < sd.frames.len() {
                    let bytes = sd.frames[sx].clone();
                    sx += 1;
                    if sd.submit == DriverSubmit::NotSubmitted {
                        sd.submit = DriverSubmit::Complete(TerminalOutcome::Success);
                        continue;
                    }
                    let op = u16::from_be_bytes([bytes[27], bytes[28]]);
                    if op == u16::from(ProtocolOpcode::CapabilityAdvertise) && !lost[0] {
                        lost[0] = true;
                        continue;
                    }
                    if op == u16::from(ProtocolOpcode::NeighborHello)
                        && target.phase() == CapabilityPhase::Idle
                    {
                        let mut corrupt = bytes.clone();
                        let end = corrupt.len() - 1;
                        corrupt[end] ^= 1;
                        assert_eq!(
                            receive(&mut target, rp, receiver, rc, &mut ra, &ri, &corrupt),
                            Err(Error::Security)
                        );
                        assert_eq!(target.phase(), CapabilityPhase::Idle);
                    }
                    let r = receive(&mut target, rp, receiver, rc, &mut ra, &ri, &bytes);
                    assert!(
                        matches!(r, Ok(()) | Err(Error::Replay)),
                        "target {op:x} {r:?}"
                    );
                }
                target
                    .drive(
                        rp,
                        receiver,
                        rc,
                        &mut rct,
                        &mut ra,
                        &mut rd,
                        &mut SecurityPacketWorkspace::<128>::new(),
                    )
                    .unwrap();
                while rx < rd.frames.len() {
                    let bytes = rd.frames[rx].clone();
                    rx += 1;
                    let op = u16::from_be_bytes([bytes[27], bytes[28]]);
                    let index = match ProtocolOpcode::try_from(op).unwrap() {
                        ProtocolOpcode::CapabilityQuery => 1,
                        ProtocolOpcode::CapabilityAdvertise => 2,
                        ProtocolOpcode::CapabilityAck => 3,
                        _ => panic!("unexpected reply"),
                    };
                    if !lost[index] {
                        lost[index] = true;
                        if index == 3 {
                            let mut ack = decode_profile_ack(&bytes[29..104]).unwrap();
                            ack.transcript_digest[0] ^= 1;
                            let mut body = [0; 75];
                            encode_profile_ack(ack, &mut body).unwrap();
                            let wrong = protect(
                                receiver,
                                rc,
                                rp,
                                ProtocolOpcode::CapabilityAck,
                                &body,
                                rct.0,
                            );
                            rct.0 += 1;
                            assert_eq!(
                                receive(&mut source, sp, sender, sc, &mut sa, &si, &wrong),
                                Err(Error::Security)
                            );
                            assert_eq!(source.phase(), CapabilityPhase::Selecting);
                        }
                        continue;
                    }
                    let r = receive(&mut source, sp, sender, sc, &mut sa, &si, &bytes);
                    assert!(
                        matches!(r, Ok(()) | Err(Error::Replay)),
                        "source {op:x} {r:?}"
                    );
                }
                // Independent cursors must converge even with budget 1 in both paths.
                source.retire(&mut sa, &mut sd, 1).unwrap();
                target.retire(&mut ra, &mut rd, 1).unwrap();
                if source.phase() == CapabilityPhase::Confirmed
                    && target.phase() == CapabilityPhase::Confirmed
                {
                    confirmed_at = now;
                    break;
                }
            }
            assert!(confirmed_at > 0 && lost.iter().all(|v| *v));
            assert_eq!(
                source
                    .selected(peer(sh, sf, sl, confirmed_at), sender, &sa)
                    .unwrap()
                    .peer_generation,
                7
            );
            assert_eq!(
                target
                    .selected(peer(rh, rf, rl, confirmed_at), receiver, &ra)
                    .unwrap()
                    .peer_generation,
                1
            );
            for frames in [&sd.frames, &rd.frames] {
                for a in frames {
                    for b in frames {
                        if a[19..29] == b[19..29] {
                            assert_eq!(a, b, "same tx/op must replay exact protected bytes");
                        }
                    }
                }
            }
            assert_eq!(
                source.selected(peer(sh, sf, sl, 9000), sender, &sa),
                Err(Error::Timeout)
            );
            let old = record(7, 2);
            let digest = capability_digest(old).unwrap();
            let mut body = [0; 20];
            encode_capability_query(
                CapabilityQuery {
                    requested_generation: 7,
                    known_digest: digest,
                },
                &mut body,
            )
            .unwrap();
            let revoke = protect(
                receiver,
                rc,
                peer(rh, rf, rl, confirmed_at + 1),
                ProtocolOpcode::CapabilityRevoke,
                &body,
                100,
            );
            receive(
                &mut source,
                peer(sh, sf, sl, confirmed_at + 1),
                sender,
                sc,
                &mut sa,
                &si,
                &revoke,
            )
            .unwrap();
            assert_eq!(source.phase(), CapabilityPhase::Fenced);
            assert_eq!(
                source.selected(peer(sh, sf, sl, confirmed_at + 1), sender, &sa),
                Err(Error::State)
            );
            source.stop(&mut sa).unwrap();
            target.stop(&mut ra).unwrap();
            for _ in 0..10 {
                source.retire(&mut sa, &mut sd, 1).unwrap();
                target.retire(&mut ra, &mut rd, 1).unwrap();
            }
            assert!(source.quiescent(&sa) && target.quiescent(&ra));
        });
    });
}
#[test]
fn stopped_unknown_token_is_quarantined_without_false_quiescence() {
    security::exercise_opcodes(1, 2, OPS, |s, h, f, c| {
        let ingress = Ingress::new(1).unwrap();
        let mut adapter = Adapter::new(1, &ingress).unwrap();
        let link = adapter.open_link(0, 1, 128).unwrap();
        adapter.try_set_rx_enabled(true).unwrap();
        let p = peer(h, f, link, 21);
        let mut root = CapabilityRuntime::<128>::new(
            10,
            CapabilityRole::Initiator,
            p,
            s,
            &adapter,
            record(1, 4),
            requirements(),
            ServiceId::new(0x40).unwrap(),
            1000,
            50,
            4,
            5000,
            1000,
        )
        .unwrap();
        root.start(p, s, &adapter).unwrap();
        let mut driver = Driver::new(DriverSubmit::Unknown);
        assert!(
            root.drive(
                p,
                s,
                c,
                &mut Counter(1),
                &mut adapter,
                &mut driver,
                &mut SecurityPacketWorkspace::<128>::new()
            )
            .is_err()
        );
        let token = driver.tokens[0];
        root.stop(&mut adapter).unwrap();
        assert!(root.retire(&mut adapter, &mut driver, 5).is_err());
        assert_eq!(adapter.tx_view(token).unwrap().state, TxState::InDoubt);
        assert!(!root.quiescent(&adapter));
        // Non-conflicting Unknown retains storage until an exact physical proof.
        // Retirement must not un-fence the Link or reactivate Capability exchange.
        ingress
            .tx_complete(token, TerminalOutcome::Success)
            .unwrap();
        root.retire(&mut adapter, &mut driver, 5).unwrap();
        assert_eq!(adapter.tx_view(token), Err(Error::NotFound));
        assert_eq!(adapter.tx_reserve(link, 0), Err(Error::State));
        assert!(root.quiescent(&adapter));
        assert_eq!(root.start(p, s, &adapter), Err(Error::State));
    });
}
#[test]
fn stopped_submitted_token_cancel_failure_retains_holder_until_retry() {
    security::exercise_opcodes(1, 2, OPS, |s, h, f, c| {
        let ingress = Ingress::new(1).unwrap();
        let mut adapter = Adapter::new(1, &ingress).unwrap();
        let link = adapter.open_link(0, 1, 128).unwrap();
        let p = peer(h, f, link, 21);
        let mut root = CapabilityRuntime::<128>::new(
            10,
            CapabilityRole::Initiator,
            p,
            s,
            &adapter,
            record(1, 4),
            requirements(),
            ServiceId::new(0x40).unwrap(),
            1000,
            50,
            4,
            5000,
            1000,
        )
        .unwrap();
        root.start(p, s, &adapter).unwrap();
        let mut driver = Driver::new(DriverSubmit::Submitted);
        root.drive(
            p,
            s,
            c,
            &mut Counter(1),
            &mut adapter,
            &mut driver,
            &mut SecurityPacketWorkspace::<128>::new(),
        )
        .unwrap();
        let token = driver.tokens[0];
        root.stop(&mut adapter).unwrap();
        root.retire(&mut adapter, &mut driver, 5).unwrap();
        assert_eq!(adapter.tx_view(token).unwrap().state, TxState::Submitted);
        assert!(!root.quiescent(&adapter));
        driver.cancel = DriverCancel::Cancelled;
        for _ in 0..10 {
            root.retire(&mut adapter, &mut driver, 1).unwrap();
        }
        assert!(root.quiescent(&adapter));
        assert!(adapter.tx_view(token).is_err());
    });
}
#[test]
fn authenticated_early_ack_and_wrong_role_cannot_confirm() {
    security::exercise_opcodes(1, 2, OPS, |s, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |r, rh, rf, rc| {
            let i = Ingress::new(1).unwrap();
            let mut a = Adapter::new(1, &i).unwrap();
            let l = a.open_link(0, 1, 128).unwrap();
            a.try_set_rx_enabled(true).unwrap();
            let p = peer(sh, sf, l, 21);
            let mut root = CapabilityRuntime::<128>::new(
                10,
                CapabilityRole::Initiator,
                p,
                s,
                &a,
                record(1, 4),
                requirements(),
                ServiceId::new(0x40).unwrap(),
                1000,
                50,
                4,
                5000,
                1000,
            )
            .unwrap();
            root.start(p, s, &a).unwrap();
            let select = ProfileSelect::build(
                record(1, 4),
                capability_digest(record(1, 4)).unwrap(),
                record(7, 2),
                capability_digest(record(7, 2)).unwrap(),
                requirements(),
            )
            .unwrap();
            let mut body = [0; 75];
            encode_profile_ack(
                ProfileAck {
                    selected: select,
                    transcript_digest: [9; 16],
                },
                &mut body,
            )
            .unwrap();
            let packet = protect(
                r,
                rc,
                peer(rh, rf, l, 21),
                ProtocolOpcode::CapabilityAck,
                &body,
                1,
            );
            assert_eq!(
                receive(&mut root, p, s, sc, &mut a, &i, &packet),
                Err(Error::State)
            );
            assert_eq!(root.phase(), CapabilityPhase::Discovering);
            assert_eq!(root.selected(p, s, &a), Err(Error::State));
            let mut summary = [0; 24];
            encode_capability_summary(
                CapabilitySummary {
                    capability_generation: 7,
                    link_instance_generation: 1,
                    digest: capability_digest(record(7, 2)).unwrap(),
                },
                &mut summary,
            )
            .unwrap();
            let packet = protect(
                r,
                rc,
                peer(rh, rf, l, 21),
                ProtocolOpcode::NeighborHello,
                &summary,
                2,
            );
            assert_eq!(
                receive(&mut root, p, s, sc, &mut a, &i, &packet),
                Err(Error::State)
            );
            assert_eq!(root.phase(), CapabilityPhase::Discovering);
        });
    });
}
#[test]
fn exact_opcode_acl_and_original_timeout_fence_are_required() {
    security::exercise_opcodes(1, 2, &[], |s, h, f, _| {
        let i = Ingress::new(1).unwrap();
        let mut a = Adapter::new(1, &i).unwrap();
        let l = a.open_link(0, 1, 128).unwrap();
        let p = peer(h, f, l, 21);
        let mut root = CapabilityRuntime::<128>::new(
            10,
            CapabilityRole::Initiator,
            p,
            s,
            &a,
            record(1, 4),
            requirements(),
            ServiceId::new(0x40).unwrap(),
            1000,
            50,
            4,
            5000,
            1000,
        )
        .unwrap();
        assert_eq!(root.start(p, s, &a), Err(Error::Access));
        assert_eq!(root.phase(), CapabilityPhase::Idle);
    });
    security::exercise_opcodes(1, 2, OPS, |s, h, f, _| {
        let i = Ingress::new(1).unwrap();
        let mut a = Adapter::new(1, &i).unwrap();
        let l = a.open_link(0, 1, 128).unwrap();
        let p = peer(h, f, l, 21);
        let mut root = CapabilityRuntime::<128>::new(
            10,
            CapabilityRole::Initiator,
            p,
            s,
            &a,
            record(1, 4),
            requirements(),
            ServiceId::new(0x40).unwrap(),
            1000,
            50,
            4,
            5000,
            1000,
        )
        .unwrap();
        root.start(p, s, &a).unwrap();
        assert_eq!(root.step(1020), Ok(()));
        assert_eq!(root.step(1021), Err(Error::Timeout));
        assert_eq!(root.phase(), CapabilityPhase::Fenced);
        assert_eq!(root.start(peer(h, f, l, 1021), s, &a), Err(Error::State));
        assert_eq!(root.step(1020), Err(Error::State));
    });
}
