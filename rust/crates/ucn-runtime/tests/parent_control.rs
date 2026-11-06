#![cfg(feature = "parent-control")]
//! Actual Flash -> Parent -> raw Security -> Driver/atomic RX -> cached confirmation.
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::*;
use ucn_adapter::*;
use ucn_digest::DigestWorkspace;
use ucn_persistence::CodecWorkspace;
use ucn_runtime::*;
use ucn_security::*;
use ucn_transport::{ParentOwner, ParentPublication, parent_context_digest};
use ucn_types::*;
const OPS: &[ProtocolOpcode] = &[
    ProtocolOpcode::TransportParentPrepare,
    ProtocolOpcode::TransportParentAccept,
    ProtocolOpcode::TransportParentCommit,
    ProtocolOpcode::TransportParentAbort,
    ProtocolOpcode::TransferSetup,
    ProtocolOpcode::TransferSetupAck,
    ProtocolOpcode::TransferSackCredit,
    ProtocolOpcode::TransferTerminalReceipt,
    ProtocolOpcode::TransferAbort,
];
type Ingress = AdapterEventIngress<1, 1, 1, 128>;
fn packet(bytes: &[u8]) -> Packet {
    let mut result = Packet {
        bytes: [0; 128],
        length: bytes.len(),
    };
    result.bytes[..bytes.len()].copy_from_slice(bytes);
    result
}
fn peer(session: SessionHandle, facts: CurrentFacts, link: LinkHandle) -> JournalPeer {
    JournalPeer {
        session,
        facts,
        link,
        fingerprint: Fingerprint::new([5; 16]).unwrap(),
    }
}
#[allow(clippy::too_many_arguments)]
fn deliver<const C: usize>(
    queue: &mut Queue,
    replies: &mut ControlReplies<C, 128>,
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    peer: JournalPeer,
    adapter: &mut AdapterOwner<'_, 1, 1, 1, 128>,
    ingress: &Ingress,
    frame: &Packet,
) -> Result<ControlAdmission> {
    ingress.rx_publish(
        peer.link,
        &frame.bytes[..frame.length],
        RxMeta {
            timestamp_us: peer.facts.now_us,
            sender_discriminator: 1,
        },
    )?;
    let mut raw = Packet {
        bytes: [0; 128],
        length: 0,
    };
    let claim = adapter.rx_claim(&mut raw.bytes)?;
    raw.length = claim.frame_bytes;
    let result = receive(
        queue,
        replies,
        security,
        crypto,
        peer.session,
        peer.facts,
        &raw,
    );
    adapter.rx_retire(claim.token)?;
    result
}
fn durable_parent(
    p: &mut security::flash::Owner<'_>,
    f: &mut security::flash::Flash,
) -> ParentOwner {
    let mut allocator = ParentOwner::new(contract()).unwrap();
    allocator
        .prepare_parent(
            security::flash::base(p, contract()),
            parent(),
            1,
            10000,
            1,
            &mut CodecWorkspace::new(),
        )
        .unwrap();
    let (h, proof) = security::flash::commit(p, f, &allocator.requirement().unwrap());
    allocator.bind(h).unwrap();
    allocator.confirm(h, proof).unwrap();
    p.proof_retire(h).unwrap();
    allocator
}
#[test]
#[allow(clippy::too_many_lines)]
fn real_publication_backpressure_lost_prepared_and_commit_exact_confirmation() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            security::flash::exercise(contract(), |p, f| {
                let mut allocator = durable_parent(p, f);
                let si = Ingress::new(1).unwrap();
                let ri = Ingress::new(1).unwrap();
                let mut sa = AdapterOwner::new(1, &si).unwrap();
                let mut ra = AdapterOwner::new(1, &ri).unwrap();
                let sl = sa.open_link(0, 1, 128).unwrap();
                let rl = ra.open_link(0, 1, 128).unwrap();
                sa.try_set_rx_enabled(true).unwrap();
                ra.try_set_rx_enabled(true).unwrap();
                let sp = peer(sh, sf, sl);
                let rp = peer(rh, rf, rl);
                let service = ServiceId::new(0x40).unwrap();
                let mut source =
                    ParentCoordinator::source(11, &allocator, p, sp, 1, 1000, 10, 4).unwrap();
                let mut target = ParentCoordinator::target(12, 1000, 10, 4).unwrap();
                let proof = source.source_proof(&allocator, p).unwrap();
                assert_eq!(source.online(sp, sender), Err(Error::State));
                assert_eq!(source.online_source(&proof, sp, sender), Err(Error::State));
                let mut sq = Queue::new(20).unwrap();
                let mut rq = Queue::new(21).unwrap();
                let mut sr = ControlReplies::<2, 128>::new(22, 10, 4).unwrap();
                let mut rr = ControlReplies::<2, 128>::new(23, 10, 4).unwrap();
                let mut sd = Driver::new(DriverSubmit::NotSubmitted);
                let mut rd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                let mut ct = Counter(1);
                let mut rt = Counter(1);
                source
                    .seal(
                        &proof,
                        sp,
                        service,
                        sender,
                        sc,
                        &mut ct,
                        &sa,
                        &mut SecurityPacketWorkspace::<128>::new(),
                    )
                    .unwrap();
                source
                    .submit(&proof, sp, service, sender, &mut sa, &mut sd)
                    .unwrap();
                assert!(source.retire(&mut sa, sender, &mut sd).unwrap());
                let sp31 = JournalPeer {
                    facts: CurrentFacts { now_us: 31, ..sf },
                    ..sp
                };
                sd.submit = DriverSubmit::Submitted;
                source
                    .submit(&proof, sp31, service, sender, &mut sa, &mut sd)
                    .unwrap();
                assert_eq!(sd.frames[0], sd.frames[1]);
                assert_eq!(ct.0, 2);
                let raw = packet(sd.frames.last().unwrap());
                let h = command(
                    deliver(&mut rq, &mut rr, receiver, rc, rp, &mut ra, &ri, &raw).unwrap(),
                );
                let prepared = rq
                    .dispatch_parent(h, &mut target, None, rp, receiver, &mut rr)
                    .unwrap()
                    .unwrap();
                assert_eq!(target.phase(), ParentOnlinePhase::Prepared);
                assert_eq!(target.online(rp, receiver), Err(Error::State));
                // First Prepared was sent but lost. Duplicate PREPARE reuses exact response cache.
                let first = emit(
                    &mut rr, prepared, receiver, rc, rh, rf, &mut rt, &mut ra, &mut rd, rl,
                );
                let rp41 = JournalPeer {
                    facts: CurrentFacts { now_us: 41, ..rf },
                    ..rp
                };
                assert_eq!(
                    deliver(&mut rq, &mut rr, receiver, rc, rp41, &mut ra, &ri, &raw),
                    Ok(ControlAdmission::CachedReply(prepared))
                );
                rr.submit(prepared, receiver, rh, rp41.facts, &mut ra, &mut rd, rl)
                    .unwrap();
                rr.retire_attempt(prepared, &mut ra).unwrap();
                let mut response = Packet {
                    bytes: [0; 128],
                    length: 0,
                };
                response.length = rr.copy_packet(prepared, &mut response.bytes).unwrap();
                assert_eq!(first.bytes, response.bytes);
                assert_eq!(rt.0, 2);
                let sp41 = JournalPeer {
                    facts: CurrentFacts { now_us: 41, ..sf },
                    ..sp
                };
                let h = command(
                    deliver(&mut sq, &mut sr, sender, sc, sp41, &mut sa, &si, &response).unwrap(),
                );
                sq.dispatch_parent(h, &mut source, Some(&proof), sp41, sender, &mut sr)
                    .unwrap();
                assert_eq!(source.phase(), ParentOnlinePhase::CommitPending);
                // Ready arrived before local completion: stage cannot overwrite the old holder.
                assert_eq!(
                    source.seal(
                        &proof,
                        sp41,
                        service,
                        sender,
                        sc,
                        &mut ct,
                        &sa,
                        &mut SecurityPacketWorkspace::<128>::new()
                    ),
                    Err(Error::State)
                );
                si.tx_complete(sd.tokens[1], TerminalOutcome::Success)
                    .unwrap();
                assert!(source.retire(&mut sa, sender, &mut sd).unwrap());
                source
                    .seal(
                        &proof,
                        sp41,
                        service,
                        sender,
                        sc,
                        &mut ct,
                        &sa,
                        &mut SecurityPacketWorkspace::<128>::new(),
                    )
                    .unwrap();
                source
                    .submit(&proof, sp41, service, sender, &mut sa, &mut sd)
                    .unwrap();
                assert_eq!(ct.0, 3);
                let commit = packet(sd.frames.last().unwrap());
                let h = command(
                    deliver(&mut rq, &mut rr, receiver, rc, rp41, &mut ra, &ri, &commit).unwrap(),
                );
                let committed = rq
                    .dispatch_parent(h, &mut target, None, rp41, receiver, &mut rr)
                    .unwrap()
                    .unwrap();
                assert_eq!(target.online(rp41, receiver), Ok(parent()));
                assert_eq!(
                    source.online_source(&proof, sp41, sender),
                    Err(Error::State)
                );
                let response = emit(
                    &mut rr, committed, receiver, rc, rh, rp41.facts, &mut rt, &mut ra, &mut rd, rl,
                );
                let h = command(
                    deliver(&mut sq, &mut sr, sender, sc, sp41, &mut sa, &si, &response).unwrap(),
                );
                let wrong = JournalPeer {
                    facts: CurrentFacts {
                        link_generation: LinkInstanceGeneration::new(2).unwrap(),
                        ..sp41.facts
                    },
                    ..sp41
                };
                assert!(
                    sq.dispatch_parent(h, &mut source, Some(&proof), wrong, sender, &mut sr)
                        .is_err()
                );
                assert_eq!(source.phase(), ParentOnlinePhase::CommitPending);
                sq.dispatch_parent(h, &mut source, Some(&proof), sp41, sender, &mut sr)
                    .unwrap();
                assert_eq!(source.online_source(&proof, sp41, sender), Ok(parent()));
                si.tx_complete(sd.tokens[2], TerminalOutcome::Success)
                    .unwrap();
                assert!(source.retire(&mut sa, sender, &mut sd).unwrap());
                #[cfg(feature = "transfer-sender")]
                {
                    allocator
                        .allocate(
                            security::flash::base(p, contract()),
                            41,
                            10000,
                            2,
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                    let (io, publication) =
                        security::flash::commit(p, f, &allocator.requirement().unwrap());
                    allocator.bind(io).unwrap();
                    allocator.confirm(io, publication).unwrap();
                    p.proof_retire(io).unwrap();
                    let permit = allocator.take_permit().unwrap();
                    let proof = source.source_proof(&allocator, p).unwrap();
                    let mut tx = TransferSender::<64, 4, 128>::new(30, 10, 4).unwrap();
                    tx.begin(
                        &source,
                        &proof,
                        sp41,
                        sender,
                        &sa,
                        permit,
                        setup(1),
                        b"abcdef",
                    )
                    .unwrap();
                    let (rx_parent, _) = published_parent(false);
                    let mut rx = Transfer::new(31, 500, 0xff00).unwrap();
                    let mut data = TransferBridge::<1, 128>::new(32).unwrap();
                    let mut feedbacks = ControlReplies::<3, 128>::new(33, 10, 4).unwrap();
                    let mut seq = SequenceCounter(10);
                    sd.submit = DriverSubmit::Complete(TerminalOutcome::Success);
                    tx.send_next(
                        &source,
                        &proof,
                        sp41,
                        sender,
                        sc,
                        &mut ct,
                        &mut seq,
                        &mut sa,
                        &mut sd,
                        &mut SecurityPacketWorkspace::<128>::new(),
                    )
                    .unwrap();
                    assert!(tx.retire(sender, &mut sa, &mut sd).unwrap());
                    let setup_frame = packet(sd.frames.last().unwrap());
                    let h = command(
                        deliver(
                            &mut rq,
                            &mut feedbacks,
                            receiver,
                            rc,
                            rp41,
                            &mut ra,
                            &ri,
                            &setup_frame,
                        )
                        .unwrap(),
                    );
                    let (_, reply) = rq
                        .dispatch_transfer_setup(
                            h,
                            &rx_parent,
                            receiver,
                            rh,
                            rp41.facts,
                            &mut rx,
                            &mut feedbacks,
                            &mut DigestWorkspace::new(),
                        )
                        .unwrap();
                    let ack = emit(
                        &mut feedbacks,
                        reply,
                        receiver,
                        rc,
                        rh,
                        rp41.facts,
                        &mut rt,
                        &mut ra,
                        &mut rd,
                        rl,
                    );
                    let h = command(
                        deliver(&mut sq, &mut sr, sender, sc, sp41, &mut sa, &si, &ack).unwrap(),
                    );
                    tx.feedback(&source, &proof, sp41, sender, &mut sq, h)
                        .unwrap();
                    assert_eq!(
                        tx.view().unwrap().phase,
                        ucn_transport::TransferPhase::Sending
                    );
                    let mut received = None;
                    for index in 0..2u64 {
                        let now = 51 + index * 10;
                        let spn = JournalPeer {
                            facts: CurrentFacts { now_us: now, ..sf },
                            ..sp
                        };
                        let rpn = JournalPeer {
                            facts: CurrentFacts { now_us: now, ..rf },
                            ..rp
                        };
                        assert!(
                            tx.send_next(
                                &source,
                                &proof,
                                spn,
                                sender,
                                sc,
                                &mut ct,
                                &mut seq,
                                &mut sa,
                                &mut sd,
                                &mut SecurityPacketWorkspace::<128>::new()
                            )
                            .unwrap()
                        );
                        tx.retire(sender, &mut sa, &mut sd).unwrap();
                        let mut raw = FragmentPacket {
                            bytes: [0; 128],
                            length: 0,
                            identity: [0; 46],
                            sequence: 10 + u32::try_from(index).unwrap(),
                        };
                        let wire = sd.frames.last().unwrap();
                        raw.length = wire.len();
                        raw.bytes[..raw.length].copy_from_slice(wire);
                        raw.identity[..16].fill(1);
                        raw.identity[16..20].copy_from_slice(&1u32.to_be_bytes());
                        raw.identity[20..36].fill(2);
                        raw.identity[36..40].copy_from_slice(&1u32.to_be_bytes());
                        raw.identity[40..].copy_from_slice(&raw.bytes[7..13]);
                        ri.rx_publish(
                            rl,
                            &raw.bytes[..raw.length],
                            RxMeta {
                                timestamp_us: now,
                                sender_discriminator: 1,
                            },
                        )
                        .unwrap();
                        let claim = ra.rx_claim(&mut raw.bytes).unwrap();
                        raw.length = claim.frame_bytes;
                        let admission = data
                            .receive(
                                receiver,
                                rc,
                                rh,
                                rpn.facts,
                                data_access(rh, AccessDirection::Inbound),
                                raw.plan(),
                                &mut SecurityPacketWorkspace::<128>::new(),
                                &rx_parent,
                                rl,
                                &mut rx,
                                &mut feedbacks,
                                &mut DigestWorkspace::new(),
                            )
                            .unwrap();
                        ra.rx_retire(claim.token).unwrap();
                        received = Some(admission.transfer);
                        let sack = emit(
                            &mut feedbacks,
                            admission.reply,
                            receiver,
                            rc,
                            rh,
                            rpn.facts,
                            &mut rt,
                            &mut ra,
                            &mut rd,
                            rl,
                        );
                        let h = command(
                            deliver(&mut sq, &mut sr, sender, sc, spn, &mut sa, &si, &sack)
                                .unwrap(),
                        );
                        tx.feedback(&source, &proof, spn, sender, &mut sq, h)
                            .unwrap();
                        data.retire_feedback(admission.transfer, &mut feedbacks)
                            .unwrap();
                    }
                    let mut business = [0xa5; 6];
                    let now = 71;
                    let rpn = JournalPeer {
                        facts: CurrentFacts { now_us: now, ..rf },
                        ..rp
                    };
                    let delivery = data
                        .deliver(
                            received.unwrap(),
                            &rx_parent,
                            receiver,
                            rh,
                            rpn.facts,
                            rl,
                            &mut rx,
                            &mut feedbacks,
                            &mut business,
                            &mut DigestWorkspace::new(),
                        )
                        .unwrap();
                    assert_eq!(&business, b"abcdef");
                    let terminal = emit(
                        &mut feedbacks,
                        delivery.terminal,
                        receiver,
                        rc,
                        rh,
                        rpn.facts,
                        &mut rt,
                        &mut ra,
                        &mut rd,
                        rl,
                    );
                    // First terminal was lost: all SACK bits are not completion. Solicit exact cached receipt.
                    assert_eq!(
                        tx.view().unwrap().phase,
                        ucn_transport::TransferPhase::Sending
                    );
                    let sp81 = JournalPeer {
                        facts: CurrentFacts { now_us: 81, ..sf },
                        ..sp
                    };
                    let rp81 = JournalPeer {
                        facts: CurrentFacts { now_us: 81, ..rf },
                        ..rp
                    };
                    assert!(
                        tx.send_next(
                            &source,
                            &proof,
                            sp81,
                            sender,
                            sc,
                            &mut ct,
                            &mut seq,
                            &mut sa,
                            &mut sd,
                            &mut SecurityPacketWorkspace::<128>::new()
                        )
                        .unwrap()
                    );
                    tx.retire(sender, &mut sa, &mut sd).unwrap();
                    let mut raw = FragmentPacket {
                        bytes: [0; 128],
                        length: 0,
                        identity: [0; 46],
                        sequence: 11,
                    };
                    let wire = sd.frames.last().unwrap();
                    raw.length = wire.len();
                    raw.bytes[..raw.length].copy_from_slice(wire);
                    raw.identity[..16].fill(1);
                    raw.identity[16..20].copy_from_slice(&1u32.to_be_bytes());
                    raw.identity[20..36].fill(2);
                    raw.identity[36..40].copy_from_slice(&1u32.to_be_bytes());
                    raw.identity[40..].copy_from_slice(&raw.bytes[7..13]);
                    let repeated = data
                        .receive(
                            receiver,
                            rc,
                            rh,
                            rp81.facts,
                            data_access(rh, AccessDirection::Inbound),
                            raw.plan(),
                            &mut SecurityPacketWorkspace::<128>::new(),
                            &rx_parent,
                            rl,
                            &mut rx,
                            &mut feedbacks,
                            &mut DigestWorkspace::new(),
                        )
                        .unwrap();
                    assert!(!repeated.fresh);
                    feedbacks
                        .submit(
                            repeated.reply,
                            receiver,
                            rh,
                            rp81.facts,
                            &mut ra,
                            &mut rd,
                            rl,
                        )
                        .unwrap();
                    feedbacks.retire_attempt(repeated.reply, &mut ra).unwrap();
                    let mut cached = Packet {
                        bytes: [0; 128],
                        length: 0,
                    };
                    cached.length = feedbacks
                        .copy_packet(repeated.reply, &mut cached.bytes)
                        .unwrap();
                    assert_eq!(cached.bytes, terminal.bytes);
                    let h = command(
                        deliver(&mut sq, &mut sr, sender, sc, sp81, &mut sa, &si, &cached).unwrap(),
                    );
                    tx.feedback(&source, &proof, sp81, sender, &mut sq, h)
                        .unwrap();
                    assert_eq!(tx.view().unwrap().phase, ucn_transport::TransferPhase::Done);
                    assert_eq!(seq.0, 12); // No resealing/no additional nonce per fragment.
                    tx.stop();
                    while tx.retire(sender, &mut sa, &mut sd).unwrap() {}
                    assert!(tx.quiescent());
                }
                source.stop();
                assert!(source.quiescent());
                assert_eq!(source.phase(), ParentOnlinePhase::Fenced);
                assert_eq!(target.expire(40), Err(Error::Argument));
                target.stop();
                assert!(target.quiescent());
            });
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // One adversarial authentication/lifecycle sequence.
fn authenticated_but_early_or_wrong_proposal_cannot_activate_source() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            security::flash::exercise(contract(), |p, f| {
                let allocator = durable_parent(p, f);
                let ingress = Ingress::new(1).unwrap();
                let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                let sp = peer(sh, sf, link);
                let mut source =
                    ParentCoordinator::source(11, &allocator, p, sp, 1, 1000, 10, 4).unwrap();
                let proof = source.source_proof(&allocator, p).unwrap();
                let mut queue = Queue::new(20).unwrap();
                let mut replies = ControlReplies::<2, 128>::new(21, 10, 4).unwrap();
                let mut value = ParentPublication {
                    transaction: 1,
                    generation: 1,
                    digest: parent_context_digest(parent(), &mut DigestWorkspace::new()).unwrap(),
                    disposition: 1,
                };
                let mut counter = Counter(1);
                let mut driver = Driver::new(DriverSubmit::Unknown);
                for attempt in 1..=5 {
                    if attempt == 3 {
                        source
                            .seal(
                                &proof,
                                sp,
                                ServiceId::new(0x40).unwrap(),
                                sender,
                                sc,
                                &mut counter,
                                &adapter,
                                &mut SecurityPacketWorkspace::<128>::new(),
                            )
                            .unwrap();
                        assert_eq!(
                            source.submit(
                                &proof,
                                sp,
                                ServiceId::new(0x40).unwrap(),
                                sender,
                                &mut adapter,
                                &mut driver,
                            ),
                            Err(Error::InDoubt)
                        );
                    }
                    let mut body = [0; 32];
                    value
                        .encode(ProtocolOpcode::TransportParentAccept, &mut body)
                        .unwrap();
                    let raw = protect(
                        receiver,
                        rc,
                        rh,
                        rf,
                        ProtocolOpcode::TransportParentAccept,
                        &body,
                        attempt,
                    );
                    let h = command(
                        receive(&mut queue, &mut replies, sender, sc, sh, sf, &raw).unwrap(),
                    );
                    assert!(
                        queue
                            .dispatch_parent(h, &mut source, Some(&proof), sp, sender, &mut replies)
                            .is_err()
                    );
                    assert_eq!(source.phase(), ParentOnlinePhase::PreparePending);
                    queue.ack(h).unwrap();
                    match attempt {
                        1 => value.disposition = 2,
                        2 => value.transaction = 2,
                        3 => value.generation = 2,
                        4 => value.digest[0] ^= 1,
                        _ => {}
                    }
                }
                // Too narrow for the complete response: no nonce burn and no Driver token.
                let narrow_ingress = Ingress::new(1).unwrap();
                let mut narrow_adapter = AdapterOwner::new(1, &narrow_ingress).unwrap();
                let narrow = narrow_adapter.open_link(0, 1, 64).unwrap();
                let wrong = JournalPeer {
                    link: narrow,
                    facts: CurrentFacts {
                        link_generation: LinkInstanceGeneration::new(1).unwrap(),
                        ..sf
                    },
                    ..sp
                };
                assert!(
                    source
                        .seal(
                            &proof,
                            wrong,
                            ServiceId::new(0x40).unwrap(),
                            sender,
                            sc,
                            &mut counter,
                            &narrow_adapter,
                            &mut SecurityPacketWorkspace::<128>::new()
                        )
                        .is_err()
                );
                assert_eq!(counter.0, 2);
                assert!(source.expire(sf.now_us + 1000).unwrap());
                assert_eq!(source.phase(), ParentOnlinePhase::Fenced);
                assert!(!source.quiescent());
                assert!(!source.retire(&mut adapter, sender, &mut driver).unwrap());
                adapter.reopen_link(link, 2, 128).unwrap(); // Fake-only; not a hardware action.
                assert_eq!(
                    source.retire(&mut adapter, sender, &mut driver),
                    Err(Error::InDoubt)
                );
                ingress
                    .tx_complete(driver.tokens[0], TerminalOutcome::Success)
                    .unwrap();
                assert!(source.retire(&mut adapter, sender, &mut driver).unwrap());
                assert!(source.quiescent());
            });
        });
    });
}

#[test]
fn pending_proof_or_old_allocator_cannot_start_publication() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, _| {
        security::flash::exercise(contract(), |p, f| {
            let ingress = Ingress::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let sp = peer(sh, sf, link);
            let mut allocator = ParentOwner::new(contract()).unwrap();
            allocator
                .prepare_parent(
                    security::flash::base(p, contract()),
                    parent(),
                    1,
                    10000,
                    1,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
            let (h, proof) = security::flash::commit(p, f, &allocator.requirement().unwrap());
            allocator.bind(h).unwrap();
            allocator.confirm(h, proof).unwrap();
            assert!(matches!(
                ParentCoordinator::source(11, &allocator, p, sp, 1, 1000, 10, 4),
                Err(Error::State)
            ));
            p.proof_retire(h).unwrap();
            let source = ParentCoordinator::source(11, &allocator, p, sp, 1, 1000, 10, 4).unwrap();
            assert_eq!(source.online(sp, sender), Err(Error::State));
            let old = published_parent(false).0;
            allocator
                .allocate(
                    security::flash::base(p, contract()),
                    21,
                    10000,
                    2,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
            let (h, proof) = security::flash::commit(p, f, &allocator.requirement().unwrap());
            allocator.bind(h).unwrap();
            allocator.confirm(h, proof).unwrap();
            p.proof_retire(h).unwrap();
            assert!(matches!(
                ParentCoordinator::source(11, &old, p, sp, 1, 1000, 10, 4),
                Err(Error::State)
            ));
            assert!(matches!(source.source_proof(&old, p), Err(Error::State)));
            assert!(source.source_proof(&allocator, p).is_ok());
        });
    });
}
