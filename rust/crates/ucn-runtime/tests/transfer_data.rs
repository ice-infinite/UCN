#![cfg(feature = "transfer")]
#![cfg_attr(not(feature = "transfer"), allow(missing_docs))]
//! Actual authenticated fragments, atomic reassembly and once-only application handoff.
//! 实际认证分片、原子重组和一次业务交付；所有Driver均为Host Fake。
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::*;
use ucn_adapter::*;
use ucn_digest::DigestWorkspace;
use ucn_runtime::*;
use ucn_security::*;
use ucn_transport::{FragmentPrefix, ParentKind, ParentOwner, TransferHandle, TransferPhase};
use ucn_types::*;

type Bridge = TransferBridge<1, 128>;
type Feedback = ControlReplies<3, 128>;
type Ingress = AdapterEventIngress<1, 1, 1, 128>;
type Adapter<'a> = AdapterOwner<'a, 1, 1, 1, 128>;

#[allow(clippy::too_many_arguments)]
fn open_fragment<const C: usize>(
    bridge: &mut Bridge,
    receiver: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    parent: &ParentOwner,
    link: LinkHandle,
    transfer: &mut Transfer,
    replies: &mut ControlReplies<C, 128>,
    packet: &FragmentPacket,
) -> Result<FragmentReceived> {
    bridge.receive(
        receiver,
        crypto,
        session,
        facts,
        data_access(session, AccessDirection::Inbound),
        packet.plan(),
        &mut SecurityPacketWorkspace::<128>::new(),
        parent,
        link,
        transfer,
        replies,
        &mut DigestWorkspace::new(),
    )
}
fn raw_fragment(index: u16, bytes: &[u8]) -> Vec<u8> {
    let mut raw = [0; 32];
    let n = ucn_transport::encode_fragment(
        FragmentPrefix {
            kind: ParentKind::C1,
            transfer_id: 1,
            parent_generation: 1,
            index,
            count: 2,
        },
        &mut raw,
    )
    .unwrap();
    raw[n..n + bytes.len()].copy_from_slice(bytes);
    raw[..n + bytes.len()].to_vec()
}
fn transmit(
    adapter: &mut Adapter<'_>,
    link: LinkHandle,
    driver: &mut Driver,
    bytes: &[u8],
) -> Vec<u8> {
    let token = adapter.tx_reserve(link, 0).unwrap();
    let view = adapter.tx_submit(token, bytes, driver).unwrap();
    assert_eq!(view.terminal, Some(TerminalOutcome::Success));
    adapter.tx_retire(token).unwrap();
    driver.frames.last().unwrap().clone()
}
#[allow(clippy::too_many_arguments)]
fn send_reply<const C: usize>(
    replies: &mut ControlReplies<C, 128>,
    h: ReplyHandle,
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    counter: &mut Counter,
    adapter: &mut Adapter<'_>,
    driver: &mut Driver,
    link: LinkHandle,
) -> Packet {
    if replies.view(h).unwrap().phase == ReplyPhase::AwaitSeal {
        replies
            .seal(
                h,
                security,
                crypto,
                counter,
                session,
                facts,
                link,
                &mut SecurityPacketWorkspace::<128>::new(),
            )
            .unwrap();
    }
    replies
        .submit(h, security, session, facts, adapter, driver, link)
        .unwrap();
    replies.retire_attempt(h, adapter).unwrap();
    let mut packet = Packet {
        bytes: [0; 128],
        length: 0,
    };
    packet.length = replies.copy_packet(h, &mut packet.bytes).unwrap();
    assert_eq!(
        &packet.bytes[..packet.length],
        driver.frames.last().unwrap()
    );
    packet
}
#[allow(clippy::too_many_arguments)]
fn admit_setup<const C: usize>(
    sender: &mut SecurityOwner<'_, 2, 2>,
    sc: &mut security::Crypto,
    sh: SessionHandle,
    sf: CurrentFacts,
    receiver: &mut SecurityOwner<'_, 2, 2>,
    rc: &mut security::Crypto,
    rh: SessionHandle,
    rf: CurrentFacts,
    parent: &ParentOwner,
    rx: &mut Transfer,
    replies: &mut ControlReplies<C, 128>,
) -> (TransferHandle, ReplyHandle) {
    let mut bytes = [0; 54];
    let n = ucn_transport::encode_setup(setup(1), &mut bytes).unwrap();
    let request = protect(
        sender,
        sc,
        sh,
        sf,
        ProtocolOpcode::TransferSetup,
        &bytes[..n],
        1,
    );
    let mut queue = Queue::new(10).unwrap();
    let h = command(receive(&mut queue, replies, receiver, rc, rh, rf, &request).unwrap());
    queue
        .dispatch_transfer_setup(
            h,
            parent,
            receiver,
            rh,
            rf,
            rx,
            replies,
            &mut DigestWorkspace::new(),
        )
        .unwrap()
}

#[test]
#[allow(clippy::too_many_lines)]
fn actual_setup_fragment_wire_sack_reorder_application_and_terminal_complete_the_transaction() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (source_parent, permit) = published_parent(true);
            let (target_parent, _) = published_parent(false);
            let mut tx = Transfer::new(1, 200, 0xff00).unwrap();
            let txh = tx
                .begin(
                    permit.unwrap(),
                    setup(1),
                    b"abcdef",
                    21,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            let mut rx = Transfer::new(2, 200, 0xff00).unwrap();
            let si = Ingress::new(1).unwrap();
            let ri = Ingress::new(2).unwrap();
            let mut sa = Adapter::new(1, &si).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            let mut ra = Adapter::new(2, &ri).unwrap();
            let rl = ra.open_link(0, 1, 128).unwrap();
            sa.try_set_rx_enabled(true).unwrap();
            ra.try_set_rx_enabled(true).unwrap();
            let mut sd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut rd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut replies = Feedback::new(20, 1, 8).unwrap();
            // Setup uses the real raw Adapter RX -> Security -> command path.
            let mut body = [0; 54];
            let n = ucn_transport::encode_setup(setup(1), &mut body).unwrap();
            let request = protect(
                sender,
                sc,
                sh,
                sf,
                ProtocolOpcode::TransferSetup,
                &body[..n],
                1,
            );
            let frame = transmit(&mut sa, sl, &mut sd, &request.bytes[..request.length]);
            ri.rx_publish(
                rl,
                &frame,
                RxMeta {
                    timestamp_us: 21,
                    sender_discriminator: 1,
                },
            )
            .unwrap();
            let mut packet = Packet {
                bytes: [0; 128],
                length: 0,
            };
            let claim = ra.rx_claim(&mut packet.bytes).unwrap();
            packet.length = claim.frame_bytes;
            let mut queue = Queue::new(21).unwrap();
            let h =
                command(receive(&mut queue, &mut replies, receiver, rc, rh, rf, &packet).unwrap());
            ra.rx_retire(claim.token).unwrap();
            let (rxh, setup_reply) = queue
                .dispatch_transfer_setup(
                    h,
                    &target_parent,
                    receiver,
                    rh,
                    rf,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            let mut counter = Counter(1);
            let ack = send_reply(
                &mut replies,
                setup_reply,
                receiver,
                rc,
                rh,
                rf,
                &mut counter,
                &mut ra,
                &mut rd,
                rl,
            );
            let mut feedback_queue = Queue::new(30).unwrap();
            let mut source_replies = Replies::new(31, 1, 8).unwrap();
            let h = command(
                receive(
                    &mut feedback_queue,
                    &mut source_replies,
                    sender,
                    sc,
                    sh,
                    sf,
                    &ack,
                )
                .unwrap(),
            );
            feedback_queue
                .dispatch_transfer_feedback(
                    h,
                    &source_parent,
                    sender,
                    sh,
                    sf,
                    &mut tx,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            let mut bridge = Bridge::new(40).unwrap();
            let mut received = None;
            let mut packets = Vec::new();
            // Send fragment 1 before 0; every TX uses actual immutable protected bytes.
            for (index, sequence, now) in [(1u16, 1u32, 22u64), (0, 2, 23)] {
                let mut raw = [0; 32];
                let n = tx.fragment(txh, index, now, &mut raw).unwrap();
                let outgoing = protect_fragment(
                    sender,
                    sc,
                    sh,
                    CurrentFacts { now_us: now, ..sf },
                    &raw[..n],
                    sequence,
                );
                let frame = transmit(&mut sa, sl, &mut sd, &outgoing.bytes[..outgoing.length]);
                tx.fragment_disposition(txh, index, ucn_transport::Submission::Submitted)
                    .unwrap();
                ri.rx_publish(
                    rl,
                    &frame,
                    RxMeta {
                        timestamp_us: now,
                        sender_discriminator: 1,
                    },
                )
                .unwrap();
                let mut incoming = outgoing;
                let claim = ra.rx_claim(&mut incoming.bytes).unwrap();
                incoming.length = claim.frame_bytes;
                let facts = CurrentFacts { now_us: now, ..rf };
                let result = open_fragment(
                    &mut bridge,
                    receiver,
                    rc,
                    rh,
                    facts,
                    &target_parent,
                    rl,
                    &mut rx,
                    &mut replies,
                    &incoming,
                )
                .unwrap();
                ra.rx_retire(claim.token).unwrap();
                assert!(result.fresh);
                assert_eq!(result.complete, index == 0);
                let sack = send_reply(
                    &mut replies,
                    result.reply,
                    receiver,
                    rc,
                    rh,
                    facts,
                    &mut counter,
                    &mut ra,
                    &mut rd,
                    rl,
                );
                // SACK is not final delivery; sender stays Sending even with every bit acknowledged.
                si.rx_publish(
                    sl,
                    &sack.bytes[..sack.length],
                    RxMeta {
                        timestamp_us: now,
                        sender_discriminator: 2,
                    },
                )
                .unwrap();
                let mut sack = sack;
                let claim = sa.rx_claim(&mut sack.bytes).unwrap();
                sack.length = claim.frame_bytes;
                let h = command(
                    receive(
                        &mut feedback_queue,
                        &mut source_replies,
                        sender,
                        sc,
                        sh,
                        CurrentFacts { now_us: now, ..sf },
                        &sack,
                    )
                    .unwrap(),
                );
                sa.rx_retire(claim.token).unwrap();
                feedback_queue
                    .dispatch_transfer_feedback(
                        h,
                        &source_parent,
                        sender,
                        sh,
                        CurrentFacts { now_us: now, ..sf },
                        &mut tx,
                        &mut DigestWorkspace::new(),
                    )
                    .unwrap();
                bridge
                    .retire_feedback(result.transfer, &mut replies)
                    .unwrap();
                assert_eq!(tx.view(txh).unwrap().phase, TransferPhase::Sending);
                received = Some(result.transfer);
                packets.push(incoming);
            }
            let h = received.unwrap();
            assert_eq!(rx.view(rxh).unwrap().phase, TransferPhase::Complete);
            let duplicate = open_fragment(
                &mut bridge,
                receiver,
                rc,
                rh,
                CurrentFacts { now_us: 24, ..rf },
                &target_parent,
                rl,
                &mut rx,
                &mut replies,
                &packets[0],
            )
            .unwrap();
            assert!(!duplicate.fresh);
            assert_eq!(duplicate.transfer, h);
            let duplicate_sack = send_reply(
                &mut replies,
                duplicate.reply,
                receiver,
                rc,
                rh,
                CurrentFacts { now_us: 24, ..rf },
                &mut counter,
                &mut ra,
                &mut rd,
                rl,
            );
            assert_eq!(
                ucn_types::ProtocolOpcode::try_from(u16::from_be_bytes(
                    duplicate_sack.bytes[27..29].try_into().unwrap()
                ))
                .unwrap(),
                ProtocolOpcode::TransferSackCredit
            );
            bridge.retire_feedback(h, &mut replies).unwrap();
            let mut short = [0xa5; 5];
            assert_eq!(
                bridge.deliver(
                    h,
                    &target_parent,
                    receiver,
                    rh,
                    CurrentFacts { now_us: 25, ..rf },
                    rl,
                    &mut rx,
                    &mut replies,
                    &mut short,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(short, [0xa5; 5]);
            assert_eq!(rx.view(rxh).unwrap().phase, TransferPhase::Complete);
            let mut application = [0xa5; 8];
            let delivered = bridge
                .deliver(
                    h,
                    &target_parent,
                    receiver,
                    rh,
                    CurrentFacts { now_us: 25, ..rf },
                    rl,
                    &mut rx,
                    &mut replies,
                    &mut application,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            assert_eq!(delivered.bytes, 6);
            assert_eq!(&application[..6], b"abcdef");
            assert_eq!(&application[6..], &[0xa5; 2]);
            assert_eq!(rx.view(rxh).unwrap().phase, TransferPhase::Delivered);
            application.fill(0xa5);
            assert_eq!(
                bridge.deliver(
                    h,
                    &target_parent,
                    receiver,
                    rh,
                    CurrentFacts { now_us: 26, ..rf },
                    rl,
                    &mut rx,
                    &mut replies,
                    &mut application,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::State)
            );
            assert_eq!(application, [0xa5; 8]);
            let terminal = send_reply(
                &mut replies,
                delivered.terminal,
                receiver,
                rc,
                rh,
                CurrentFacts { now_us: 26, ..rf },
                &mut counter,
                &mut ra,
                &mut rd,
                rl,
            );
            // Lose terminal on first attempt; original authenticated fragment retrieves same sealed reply.
            let old_counter = counter.0;
            let repeat = open_fragment(
                &mut bridge,
                receiver,
                rc,
                rh,
                CurrentFacts { now_us: 27, ..rf },
                &target_parent,
                rl,
                &mut rx,
                &mut replies,
                &packets[1],
            )
            .unwrap();
            assert_eq!(repeat.reply, delivered.terminal);
            assert!(!repeat.fresh);
            let retry = send_reply(
                &mut replies,
                repeat.reply,
                receiver,
                rc,
                rh,
                CurrentFacts { now_us: 27, ..rf },
                &mut counter,
                &mut ra,
                &mut rd,
                rl,
            );
            assert_eq!(counter.0, old_counter);
            assert_eq!(
                &terminal.bytes[..terminal.length],
                &retry.bytes[..retry.length]
            );
            let hterm = command(
                receive(
                    &mut feedback_queue,
                    &mut source_replies,
                    sender,
                    sc,
                    sh,
                    CurrentFacts { now_us: 27, ..sf },
                    &retry,
                )
                .unwrap(),
            );
            feedback_queue
                .dispatch_transfer_feedback(
                    hterm,
                    &source_parent,
                    sender,
                    sh,
                    CurrentFacts { now_us: 27, ..sf },
                    &mut tx,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            assert_eq!(tx.view(txh).unwrap().phase, TransferPhase::Done);
            assert_eq!(bridge.retire(h, &mut rx, 1021), Err(Error::State));
            bridge.retire(h, &mut rx, 1221).unwrap();
            assert_eq!(bridge.view(h, &rx), Err(Error::State));
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)]
fn auth_geometry_digest_acl_and_capacity_failures_never_publish_or_consume_replay() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, _) = published_parent(false);
            let mut rx = Transfer::new(2, 200, 0xff00).unwrap();
            let mut full_replies = ControlReplies::<1, 128>::new(20, 1, 8).unwrap();
            let (rxh, setup_reply) = admit_setup(
                sender,
                sc,
                sh,
                sf,
                receiver,
                rc,
                rh,
                rf,
                &parent,
                &mut rx,
                &mut full_replies,
            );
            let ingress = Ingress::new(2).unwrap();
            let mut adapter = Adapter::new(2, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut bridge = Bridge::new(40).unwrap();
            let good = protect_fragment(sender, sc, sh, sf, &raw_fragment(0, b"abc"), 1);
            let mut bad = FragmentPacket {
                bytes: good.bytes,
                length: good.length,
                identity: good.identity,
                sequence: good.sequence,
            };
            bad.bytes[13] ^= 1;
            let old = rx.view(rxh).unwrap();
            assert!(
                open_fragment(
                    &mut bridge,
                    receiver,
                    rc,
                    rh,
                    rf,
                    &parent,
                    link,
                    &mut rx,
                    &mut full_replies,
                    &bad
                )
                .is_err()
            );
            assert_eq!(rx.view(rxh).unwrap(), old);
            assert_eq!(
                open_fragment(
                    &mut bridge,
                    receiver,
                    rc,
                    rh,
                    rf,
                    &parent,
                    link,
                    &mut rx,
                    &mut full_replies,
                    &good
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(rx.view(rxh).unwrap(), old);
            // Distinct explicitly configured feedback pool becomes available; preserve the full
            // setup pool and its obligation. No clock rewind or implicit eviction.
            assert_eq!(
                full_replies.view(setup_reply).unwrap().phase,
                ReplyPhase::AwaitSeal
            );
            let mut replies = ControlReplies::<1, 128>::new(21, 1, 8).unwrap();
            let work = &mut SecurityPacketWorkspace::<128>::new();
            let wrong_access = AccessRequest {
                service: ServiceId::new(0x41).unwrap(),
                ..data_access(rh, AccessDirection::Inbound)
            };
            assert!(
                bridge
                    .receive(
                        receiver,
                        rc,
                        rh,
                        rf,
                        wrong_access,
                        good.plan(),
                        work,
                        &parent,
                        link,
                        &mut rx,
                        &mut replies,
                        &mut DigestWorkspace::new()
                    )
                    .is_err()
            );
            assert_eq!(rx.view(rxh).unwrap(), old);
            let first = open_fragment(
                &mut bridge,
                receiver,
                rc,
                rh,
                rf,
                &parent,
                link,
                &mut rx,
                &mut replies,
                &good,
            )
            .unwrap();
            assert!(first.fresh);
            assert_eq!(rx.view(rxh).unwrap().completed_fragments, 1);
            send_reply(
                &mut replies,
                first.reply,
                receiver,
                rc,
                rh,
                rf,
                &mut Counter(1),
                &mut adapter,
                &mut driver,
                link,
            );
            bridge
                .retire_feedback(first.transfer, &mut replies)
                .unwrap();
            let old = rx.view(rxh).unwrap();
            for (ordinal, raw) in [&raw_fragment(1, b"BAD")[..], &raw_fragment(1, b"de")[..]]
                .into_iter()
                .enumerate()
            {
                let invalid =
                    protect_fragment(sender, sc, sh, sf, raw, 2 + u32::try_from(ordinal).unwrap());
                assert_eq!(
                    open_fragment(
                        &mut bridge,
                        receiver,
                        rc,
                        rh,
                        rf,
                        &parent,
                        link,
                        &mut rx,
                        &mut replies,
                        &invalid
                    ),
                    Err(Error::Malformed)
                );
                assert_eq!(rx.view(rxh).unwrap(), old);
            }
            let mut geometry = raw_fragment(1, b"def");
            geometry[4..8].copy_from_slice(&2u32.to_be_bytes());
            let invalid = protect_fragment(sender, sc, sh, sf, &geometry, 4);
            assert_eq!(
                open_fragment(
                    &mut bridge,
                    receiver,
                    rc,
                    rh,
                    rf,
                    &parent,
                    link,
                    &mut rx,
                    &mut replies,
                    &invalid
                ),
                Err(Error::Access)
            );
            assert_eq!(rx.view(rxh).unwrap(), old);
            let good = protect_fragment(sender, sc, sh, sf, &raw_fragment(1, b"def"), 5);
            let last = open_fragment(
                &mut bridge,
                receiver,
                rc,
                rh,
                rf,
                &parent,
                link,
                &mut rx,
                &mut replies,
                &good,
            )
            .unwrap();
            assert!(last.fresh && last.complete);
            let mut app = [0xa5; 6];
            let complete = rx.view(rxh).unwrap();
            assert_eq!(
                bridge.deliver(
                    last.transfer,
                    &parent,
                    receiver,
                    rh,
                    rf,
                    link,
                    &mut rx,
                    &mut replies,
                    &mut app,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(app, [0xa5; 6]);
            assert_eq!(rx.view(rxh).unwrap(), complete);
            send_reply(
                &mut replies,
                last.reply,
                receiver,
                rc,
                rh,
                rf,
                &mut Counter(2),
                &mut adapter,
                &mut driver,
                link,
            );
            bridge.retire_feedback(last.transfer, &mut replies).unwrap();
            assert!(
                bridge
                    .deliver(
                        last.transfer,
                        &parent,
                        receiver,
                        rh,
                        CurrentFacts {
                            policy_generation: 2,
                            ..rf
                        },
                        link,
                        &mut rx,
                        &mut replies,
                        &mut app,
                        &mut DigestWorkspace::new()
                    )
                    .is_err()
            );
            assert_eq!(app, [0xa5; 6]);
            assert_eq!(rx.view(rxh).unwrap(), complete);
            let success = bridge
                .deliver(
                    last.transfer,
                    &parent,
                    receiver,
                    rh,
                    rf,
                    link,
                    &mut rx,
                    &mut replies,
                    &mut app,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            assert_eq!(success.bytes, 6);
            assert_eq!(&app, b"abcdef");
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)]
fn conflicting_replay_stale_link_and_expired_delivery_preserve_verified_message() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, _) = published_parent(false);
            let mut rx = Transfer::new(2, 200, 0xff00).unwrap();
            let mut replies = Feedback::new(20, 1, 8).unwrap();
            let (rxh, _) = admit_setup(
                sender,
                sc,
                sh,
                sf,
                receiver,
                rc,
                rh,
                rf,
                &parent,
                &mut rx,
                &mut replies,
            );
            let ingress = AdapterEventIngress::<2, 1, 1, 128>::new(2).unwrap();
            let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let other_link = adapter.open_link(1, 1, 128).unwrap();
            let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut bridge = Bridge::new(40).unwrap();
            let mut counter = Counter(1);
            let mut last = None;
            for (index, bytes, sequence) in
                [(0u16, b"abc".as_slice(), 1u32), (1, b"def".as_slice(), 2)]
            {
                let packet =
                    protect_fragment(sender, sc, sh, sf, &raw_fragment(index, bytes), sequence);
                let result = open_fragment(
                    &mut bridge,
                    receiver,
                    rc,
                    rh,
                    rf,
                    &parent,
                    link,
                    &mut rx,
                    &mut replies,
                    &packet,
                )
                .unwrap();
                // Same Crypto-valid Origin sequence with different exact bytes cannot be replayed.
                let conflict =
                    adversarial_conflicting_fragment(sc, &packet, &raw_fragment(index, b"BAD"));
                let old = rx.view(rxh).unwrap();
                assert_eq!(
                    open_fragment(
                        &mut bridge,
                        receiver,
                        rc,
                        rh,
                        rf,
                        &parent,
                        link,
                        &mut rx,
                        &mut replies,
                        &conflict
                    ),
                    Err(Error::Replay)
                );
                assert_eq!(rx.view(rxh).unwrap(), old);
                // A different physical Link with the same generation must not borrow old evidence.
                assert_eq!(
                    open_fragment(
                        &mut bridge,
                        receiver,
                        rc,
                        rh,
                        rf,
                        &parent,
                        other_link,
                        &mut rx,
                        &mut replies,
                        &packet
                    ),
                    Err(Error::Access)
                );
                replies
                    .seal(
                        result.reply,
                        receiver,
                        rc,
                        &mut counter,
                        rh,
                        rf,
                        link,
                        &mut SecurityPacketWorkspace::<128>::new(),
                    )
                    .unwrap();
                replies
                    .submit(
                        result.reply,
                        receiver,
                        rh,
                        rf,
                        &mut adapter,
                        &mut driver,
                        link,
                    )
                    .unwrap();
                replies.retire_attempt(result.reply, &mut adapter).unwrap();
                bridge
                    .retire_feedback(result.transfer, &mut replies)
                    .unwrap();
                last = Some((result.transfer, packet));
            }
            let (h, packet) = last.unwrap();
            let old = rx.view(rxh).unwrap();
            let mut output = [0xa5; 6];
            assert_eq!(
                bridge.deliver(
                    h,
                    &parent,
                    receiver,
                    rh,
                    CurrentFacts { now_us: 1021, ..rf },
                    link,
                    &mut rx,
                    &mut replies,
                    &mut output,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::Timeout)
            );
            assert_eq!(output, [0xa5; 6]);
            assert_eq!(rx.view(rxh).unwrap(), old);
            assert!(
                open_fragment(
                    &mut bridge,
                    receiver,
                    rc,
                    rh,
                    CurrentFacts { now_us: 1221, ..rf },
                    &parent,
                    link,
                    &mut rx,
                    &mut replies,
                    &packet
                )
                .is_err()
            );
            assert_eq!(rx.view(rxh).unwrap(), old);
            // Timed-out but already complete data remains owned until explicit local rejection.
            bridge.discard(h, &mut rx).unwrap();
            assert_eq!(rx.view(rxh).unwrap().phase, TransferPhase::Aborted);
            bridge.retire(h, &mut rx, 1221).unwrap();
            assert_eq!(bridge.view(h, &rx), Err(Error::State));
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)]
fn stop_discard_and_logical_retirement_never_erase_pending_feedback_token() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, _) = published_parent(false);
            let mut rx = Transfer::new(2, 200, 0xff00).unwrap();
            let mut replies = Feedback::new(20, 1, 8).unwrap();
            let (rxh, _) = admit_setup(
                sender,
                sc,
                sh,
                sf,
                receiver,
                rc,
                rh,
                rf,
                &parent,
                &mut rx,
                &mut replies,
            );
            let ingress = Ingress::new(2).unwrap();
            let mut adapter = Adapter::new(2, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut bridge = Bridge::new(40).unwrap();
            let mut counter = Counter(1);
            let first = protect_fragment(sender, sc, sh, sf, &raw_fragment(0, b"abc"), 1);
            let result = open_fragment(
                &mut bridge,
                receiver,
                rc,
                rh,
                rf,
                &parent,
                link,
                &mut rx,
                &mut replies,
                &first,
            )
            .unwrap();
            send_reply(
                &mut replies,
                result.reply,
                receiver,
                rc,
                rh,
                rf,
                &mut counter,
                &mut adapter,
                &mut driver,
                link,
            );
            bridge
                .retire_feedback(result.transfer, &mut replies)
                .unwrap();
            let last = protect_fragment(sender, sc, sh, sf, &raw_fragment(1, b"def"), 2);
            let result = open_fragment(
                &mut bridge,
                receiver,
                rc,
                rh,
                rf,
                &parent,
                link,
                &mut rx,
                &mut replies,
                &last,
            )
            .unwrap();
            replies
                .seal(
                    result.reply,
                    receiver,
                    rc,
                    &mut counter,
                    rh,
                    rf,
                    link,
                    &mut SecurityPacketWorkspace::<128>::new(),
                )
                .unwrap();
            driver.submit = DriverSubmit::Submitted;
            replies
                .submit(
                    result.reply,
                    receiver,
                    rh,
                    rf,
                    &mut adapter,
                    &mut driver,
                    link,
                )
                .unwrap();
            let old = replies.view(result.reply).unwrap();
            assert_eq!(
                bridge.retire_feedback(result.transfer, &mut replies),
                Err(Error::State)
            );
            assert_eq!(replies.view(result.reply).unwrap(), old);
            bridge.stop();
            replies.stop();
            let mut output = [0xa5; 6];
            assert_eq!(
                bridge.deliver(
                    result.transfer,
                    &parent,
                    receiver,
                    rh,
                    rf,
                    link,
                    &mut rx,
                    &mut replies,
                    &mut output,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::State)
            );
            assert_eq!(output, [0xa5; 6]);
            assert_eq!(
                open_fragment(
                    &mut bridge,
                    receiver,
                    rc,
                    rh,
                    rf,
                    &parent,
                    link,
                    &mut rx,
                    &mut replies,
                    &last
                ),
                Err(Error::State)
            );
            // Complete data can be rejected after stop; its sealed feedback has no RX-buffer borrow.
            bridge.discard(result.transfer, &mut rx).unwrap();
            assert_eq!(rx.view(rxh).unwrap().phase, TransferPhase::Aborted);
            bridge.retire(result.transfer, &mut rx, 1221).unwrap();
            assert_eq!(replies.retire_cache(result.reply, 1221), Err(Error::State));
            assert_eq!(
                replies.cancel_attempt(result.reply, &mut adapter, &mut driver),
                Err(Error::State)
            );
            assert_eq!(replies.view(result.reply).unwrap(), old);
            driver.cancel = DriverCancel::Cancelled;
            replies
                .cancel_attempt(result.reply, &mut adapter, &mut driver)
                .unwrap();
            replies.retire_cache(result.reply, 1221).unwrap();
            assert_eq!(replies.view(result.reply), Err(Error::State));
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)]
fn duplicate_cannot_substitute_another_sack_window() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, _) = published_parent(false);
            let mut rx =
                ucn_transport::TransferOwner::<1, 1, 96, 64, 1>::new(2, 200, 0xff00).unwrap();
            let setup = ucn_transport::TransferSetup {
                total_bytes: 33,
                fragments: 33,
                fragment_bytes: 1,
                digest: ucn_digest::canonical_digest(
                    b"UCN6-TRANSFER-MESSAGE-V1",
                    &[7; 33],
                    &mut DigestWorkspace::new(),
                )
                .unwrap(),
                ..fixture::setup(1)
            };
            rx.receive_setup(fixture::parent(), setup, 21).unwrap();
            let ingress = Ingress::new(2).unwrap();
            let mut adapter = Adapter::new(2, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut replies = Feedback::new(20, 1, 8).unwrap();
            let mut bridge = Bridge::new(40).unwrap();
            let mut packets = Vec::new();
            let mut received = None;
            for (index, sequence) in [(0u16, 1u32), (32, 2)] {
                let mut bytes = [0; 13];
                ucn_transport::encode_fragment(
                    FragmentPrefix {
                        kind: ParentKind::C1,
                        transfer_id: 1,
                        parent_generation: 1,
                        index,
                        count: 33,
                    },
                    &mut bytes,
                )
                .unwrap();
                bytes[12] = 7;
                let packet = protect_fragment(sender, sc, sh, sf, &bytes, sequence);
                let result = bridge
                    .receive(
                        receiver,
                        rc,
                        rh,
                        rf,
                        data_access(rh, AccessDirection::Inbound),
                        packet.plan(),
                        &mut SecurityPacketWorkspace::<128>::new(),
                        &parent,
                        link,
                        &mut rx,
                        &mut replies,
                        &mut DigestWorkspace::new(),
                    )
                    .unwrap();
                if index == 0 {
                    send_reply(
                        &mut replies,
                        result.reply,
                        receiver,
                        rc,
                        rh,
                        rf,
                        &mut Counter(1),
                        &mut adapter,
                        &mut driver,
                        link,
                    );
                    bridge
                        .retire_feedback(result.transfer, &mut replies)
                        .unwrap();
                }
                received = Some(result);
                packets.push(packet);
            }
            let result = received.unwrap();
            let old = replies.view(result.reply).unwrap();
            assert_eq!(
                bridge.receive(
                    receiver,
                    rc,
                    rh,
                    rf,
                    data_access(rh, AccessDirection::Inbound),
                    packets[0].plan(),
                    &mut SecurityPacketWorkspace::<128>::new(),
                    &parent,
                    link,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(replies.view(result.reply).unwrap(), old);
            let response = send_reply(
                &mut replies,
                result.reply,
                receiver,
                rc,
                rh,
                rf,
                &mut Counter(2),
                &mut adapter,
                &mut driver,
                link,
            );
            let sack = ucn_transport::decode_sack(ParentKind::C1, &response.bytes[29..49]).unwrap();
            assert_eq!(sack.base, 32);
            assert_eq!(sack.bitmap, 1);
            bridge
                .retire_feedback(result.transfer, &mut replies)
                .unwrap();
            let duplicate = bridge
                .receive(
                    receiver,
                    rc,
                    rh,
                    rf,
                    data_access(rh, AccessDirection::Inbound),
                    packets[0].plan(),
                    &mut SecurityPacketWorkspace::<128>::new(),
                    &parent,
                    link,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            assert!(!duplicate.fresh);
            let response = send_reply(
                &mut replies,
                duplicate.reply,
                receiver,
                rc,
                rh,
                rf,
                &mut Counter(3),
                &mut adapter,
                &mut driver,
                link,
            );
            let sack = ucn_transport::decode_sack(ParentKind::C1, &response.bytes[29..49]).unwrap();
            assert_eq!(sack.base, 0);
            assert_eq!(sack.bitmap, 1);
        });
    });
}
