#![cfg(feature = "transfer")]
#![cfg_attr(not(feature = "transfer"), allow(missing_docs))]
//! Actual authenticated Control and Adapter lifecycle regressions.
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::*;
use ucn_adapter::*;
use ucn_digest::DigestWorkspace;
use ucn_runtime::*;
use ucn_security::*;
use ucn_transport::{ParentOwner, TransferPhase};
use ucn_types::*;

#[test]
#[allow(clippy::too_many_lines)] // Readable end-to-end multi-Owner fault sequence.
fn authenticated_setup_lost_ack_cached_retry_actual_adapter_and_exact_tx_dispatch() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (tx_parent, permit) = published_parent(true);
            let (rx_parent, _) = published_parent(false);
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
            let mut queue = Queue::new(1).unwrap();
            let mut replies = Replies::new(2, 10, 4).unwrap();
            let ingress = AdapterEventIngress::<2, 2, 1, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let other_link = adapter.open_link(1, 1, 128).unwrap();
            adapter.try_set_rx_enabled(true).unwrap();
            ingress
                .rx_publish(
                    link,
                    &request.bytes[..request.length],
                    RxMeta {
                        timestamp_us: rf.now_us,
                        sender_discriminator: 1,
                    },
                )
                .unwrap();
            let mut input = Packet {
                bytes: [0; 128],
                length: 0,
            };
            let claim = adapter.rx_claim(&mut input.bytes).unwrap();
            input.length = claim.frame_bytes;
            assert_eq!(claim.link, link);
            assert_eq!(claim.meta.timestamp_us, rf.now_us);
            let h =
                command(receive(&mut queue, &mut replies, receiver, rc, rh, rf, &input).unwrap());
            // Immutable command has copied its facts; only now may RX storage be retired.
            adapter.rx_retire(claim.token).unwrap();
            assert_eq!(
                receive(&mut queue, &mut replies, receiver, rc, rh, rf, &request),
                Ok(ControlAdmission::Pending(h))
            );
            let (rxh, reply) = queue
                .dispatch_transfer_setup(
                    h,
                    &rx_parent,
                    receiver,
                    rh,
                    rf,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            assert_eq!(queue.view(h), Err(Error::State));
            let old_rx = rx.view(rxh).unwrap();
            assert_eq!(old_rx.deadline_us, 1021);
            let mut counter = Counter(1);
            replies
                .seal(
                    reply,
                    receiver,
                    rc,
                    &mut counter,
                    rh,
                    rf,
                    link,
                    &mut SecurityPacketWorkspace::<128>::new(),
                )
                .unwrap();
            assert_eq!(counter.0, 2);
            let mut driver = Driver::new(DriverSubmit::NotSubmitted);
            assert_eq!(
                replies.submit(
                    reply,
                    receiver,
                    rh,
                    rf,
                    &mut adapter,
                    &mut driver,
                    other_link
                ),
                Err(Error::State)
            );
            assert!(driver.frames.is_empty());
            replies
                .submit(reply, receiver, rh, rf, &mut adapter, &mut driver, link)
                .unwrap();
            assert_eq!(replies.view(reply).unwrap().phase, ReplyPhase::Ready);
            assert!(!replies.view(reply).unwrap().physical_pending);
            assert_eq!(
                replies.submit(reply, receiver, rh, rf, &mut adapter, &mut driver, link),
                Err(Error::State)
            );
            let rf31 = CurrentFacts { now_us: 31, ..rf };
            driver.submit = DriverSubmit::Submitted;
            replies
                .submit(reply, receiver, rh, rf31, &mut adapter, &mut driver, link)
                .unwrap();
            assert_eq!(
                replies.retire_attempt(reply, &mut adapter),
                Err(Error::State)
            );
            ingress
                .tx_complete(driver.tokens[1], TerminalOutcome::Success)
                .unwrap();
            replies.retire_attempt(reply, &mut adapter).unwrap();
            assert_eq!(replies.view(reply).unwrap().phase, ReplyPhase::Cached);
            // First ACK was sent but lost in the network: TX still awaits Setup ACK.
            assert_eq!(tx.view(txh).unwrap().phase, TransferPhase::SetupPending);
            let mut bad = Packet {
                bytes: request.bytes,
                length: request.length,
            };
            bad.bytes[29] ^= 1;
            let before = replies.view(reply).unwrap();
            assert!(receive(&mut queue, &mut replies, receiver, rc, rh, rf31, &bad).is_err());
            assert_eq!(replies.view(reply).unwrap(), before);
            let rf41 = CurrentFacts { now_us: 41, ..rf };
            assert_eq!(
                receive(&mut queue, &mut replies, receiver, rc, rh, rf41, &request),
                Ok(ControlAdmission::CachedReply(reply))
            );
            assert_eq!(rx.view(rxh).unwrap(), old_rx); // No lease renewal/reassembly reset.
            replies
                .submit(reply, receiver, rh, rf41, &mut adapter, &mut driver, link)
                .unwrap();
            ingress
                .tx_complete(driver.tokens[2], TerminalOutcome::Success)
                .unwrap();
            replies.retire_attempt(reply, &mut adapter).unwrap();
            assert_eq!(driver.frames[0], driver.frames[1]);
            assert_eq!(driver.frames[1], driver.frames[2]);
            assert_eq!(counter.0, 2); // Exact retry burned no new Origin transaction.
            let mut ack = Packet {
                bytes: [0; 128],
                length: 0,
            };
            ack.length = replies.copy_packet(reply, &mut ack.bytes).unwrap();
            let origin_ingress = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
            let mut origin_adapter = AdapterOwner::new(2, &origin_ingress).unwrap();
            let origin_link = origin_adapter.open_link(0, 1, 128).unwrap();
            origin_adapter.try_set_rx_enabled(true).unwrap();
            origin_ingress
                .rx_publish(
                    origin_link,
                    &driver.frames[2],
                    RxMeta {
                        timestamp_us: 41,
                        sender_discriminator: 2,
                    },
                )
                .unwrap();
            let claim = origin_adapter.rx_claim(&mut ack.bytes).unwrap();
            ack.length = claim.frame_bytes;
            let mut txq = Queue::new(3).unwrap();
            let mut tx_replies = Replies::new(4, 10, 4).unwrap();
            let sf41 = CurrentFacts { now_us: 41, ..sf };
            let ah =
                command(receive(&mut txq, &mut tx_replies, sender, sc, sh, sf41, &ack).unwrap());
            origin_adapter.rx_retire(claim.token).unwrap();
            assert_eq!(
                txq.dispatch_transfer_feedback(
                    ah,
                    &tx_parent,
                    sender,
                    sh,
                    sf41,
                    &mut tx,
                    &mut DigestWorkspace::new()
                ),
                Ok(txh)
            );
            assert_eq!(tx.view(txh).unwrap().phase, TransferPhase::Sending);
            let mut short = [0xa5; 2];
            assert_eq!(replies.copy_packet(reply, &mut short), Err(Error::NoSpace));
            assert_eq!(short, [0xa5; 2]);
            replies.retire_cache(reply, 1021).unwrap();
            assert_eq!(
                receive(
                    &mut queue,
                    &mut replies,
                    receiver,
                    rc,
                    rh,
                    CurrentFacts { now_us: 1021, ..rf },
                    &request
                ),
                Err(Error::Replay)
            );
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)]
fn cache_capacity_wrong_parent_expired_session_and_delayed_dispatch_do_not_commit_rx() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, _) = published_parent(false);
            let mut queue = Queue::new(1).unwrap();
            let mut replies = Replies::new(2, 10, 4).unwrap();
            let mut rx = Transfer::new(2, 200, 0xff00).unwrap();
            let mut bytes = [0; 54];
            let n = ucn_transport::encode_setup(setup(1), &mut bytes).unwrap();
            let packet = protect(
                sender,
                sc,
                sh,
                sf,
                ProtocolOpcode::TransferSetup,
                &bytes[..n],
                1,
            );
            let h =
                command(receive(&mut queue, &mut replies, receiver, rc, rh, rf, &packet).unwrap());
            let pending = queue.view(h).unwrap();
            let missing = ParentOwner::new(contract()).unwrap();
            assert_eq!(
                queue.dispatch_transfer_setup(
                    h,
                    &missing,
                    receiver,
                    rh,
                    rf,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::State)
            );
            assert_eq!(queue.view(h).unwrap(), pending);
            // Queued at 21, dispatched at 40: original expiry is 1021, not 1040.
            let rf40 = CurrentFacts { now_us: 40, ..rf };
            let (rhx, reply) = queue
                .dispatch_transfer_setup(
                    h,
                    &parent,
                    receiver,
                    rh,
                    rf40,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            assert_eq!(rx.view(rhx).unwrap().deadline_us, 1021);
            let n = ucn_transport::encode_setup(setup(2), &mut bytes).unwrap();
            let second = protect(
                sender,
                sc,
                sh,
                sf,
                ProtocolOpcode::TransferSetup,
                &bytes[..n],
                2,
            );
            let h2 = command(
                receive(&mut queue, &mut replies, receiver, rc, rh, rf40, &second).unwrap(),
            );
            let old = queue.view(h2).unwrap();
            let old_reply = replies.view(reply).unwrap();
            assert_eq!(
                queue.dispatch_transfer_setup(
                    h2,
                    &parent,
                    receiver,
                    rh,
                    rf40,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(queue.view(h2).unwrap(), old);
            assert_eq!(replies.view(reply).unwrap(), old_reply);
            // Cache lookup must work even though the only fresh command slot is full.
            assert_eq!(
                receive(&mut queue, &mut replies, receiver, rc, rh, rf40, &packet),
                Ok(ControlAdmission::CachedReply(reply))
            );
            assert_eq!(queue.view(h2).unwrap(), old);
            let expired = CurrentFacts {
                now_us: 10000,
                ..rf
            };
            assert!(
                queue
                    .dispatch_transfer_setup(
                        h2,
                        &parent,
                        receiver,
                        rh,
                        expired,
                        &mut rx,
                        &mut replies,
                        &mut DigestWorkspace::new()
                    )
                    .is_err()
            );
            assert_eq!(queue.view(h2).unwrap(), old);
            assert_eq!(rx.find_rx(crate::parent(), 2), Err(Error::NotFound));
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)]
fn stop_and_failed_driver_cancel_retain_reply_until_explicit_completion_and_retirement() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, _) = published_parent(false);
            let mut queue = Queue::new(1).unwrap();
            let mut replies = Replies::new(2, 10, 4).unwrap();
            let mut rx = Transfer::new(2, 200, 0xff00).unwrap();
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
            let h =
                command(receive(&mut queue, &mut replies, receiver, rc, rh, rf, &request).unwrap());
            let (_, reply) = queue
                .dispatch_transfer_setup(
                    h,
                    &parent,
                    receiver,
                    rh,
                    rf,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            let ingress = AdapterEventIngress::<1, 1, 1, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            replies
                .seal(
                    reply,
                    receiver,
                    rc,
                    &mut Counter(1),
                    rh,
                    rf,
                    link,
                    &mut SecurityPacketWorkspace::<128>::new(),
                )
                .unwrap();
            let mut driver = Driver::new(DriverSubmit::Submitted);
            replies
                .submit(reply, receiver, rh, rf, &mut adapter, &mut driver, link)
                .unwrap();
            replies.stop();
            queue.stop();
            let old = replies.view(reply).unwrap();
            assert_eq!(
                replies.cancel_attempt(reply, &mut adapter, &mut driver),
                Err(Error::State)
            );
            assert_eq!(replies.view(reply).unwrap(), old);
            assert_eq!(driver.cancels, 1);
            assert_eq!(replies.retire_cache(reply, 1021), Err(Error::State));
            driver.cancel = DriverCancel::Cancelled;
            replies
                .cancel_attempt(reply, &mut adapter, &mut driver)
                .unwrap();
            assert!(!replies.view(reply).unwrap().physical_pending);
            assert_eq!(
                replies.submit(reply, receiver, rh, rf, &mut adapter, &mut driver, link),
                Err(Error::State)
            );
            replies.retire_cache(reply, 1021).unwrap();
            assert_eq!(replies.view(reply), Err(Error::State));
            assert_eq!(
                replies.retire_attempt(reply, &mut adapter),
                Err(Error::State)
            );
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)]
fn raw_feedback_cannot_change_setup_generation_budget_or_final_result() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, permit) = published_parent(true);
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
            let key = ucn_transport::TransferKey::from_setup(setup(1), &mut DigestWorkspace::new())
                .unwrap();
            let mut queue = Queue::new(1).unwrap();
            let mut replies = Replies::new(2, 10, 4).unwrap();
            let ack = ucn_transport::SetupAck {
                key,
                fragment_bytes: 3,
                credit: 2,
            };
            let mut bytes = [0; 33];
            for (i, bad) in [
                ucn_transport::SetupAck {
                    fragment_bytes: 4,
                    ..ack
                },
                ucn_transport::SetupAck {
                    key: ucn_transport::TransferKey {
                        parent_generation: 2,
                        ..key
                    },
                    ..ack
                },
                ucn_transport::SetupAck {
                    key: ucn_transport::TransferKey {
                        digest: [9; 16],
                        ..key
                    },
                    ..ack
                },
            ]
            .into_iter()
            .enumerate()
            {
                let n = ucn_transport::encode_setup_ack(bad, &mut bytes).unwrap();
                let packet = protect(
                    receiver,
                    rc,
                    rh,
                    rf,
                    ProtocolOpcode::TransferSetupAck,
                    &bytes[..n],
                    u64::try_from(i).unwrap() + 1,
                );
                let h = command(
                    receive(&mut queue, &mut replies, sender, sc, sh, sf, &packet).unwrap(),
                );
                let old = tx.view(txh).unwrap();
                assert_eq!(
                    queue.dispatch_transfer_feedback(
                        h,
                        &parent,
                        sender,
                        sh,
                        sf,
                        &mut tx,
                        &mut DigestWorkspace::new()
                    ),
                    Err(Error::Access)
                );
                assert_eq!(tx.view(txh).unwrap(), old);
                assert!(queue.view(h).is_ok());
                queue.ack(h).unwrap();
            }
            let n = ucn_transport::encode_setup_ack(ack, &mut bytes).unwrap();
            let packet = protect(
                receiver,
                rc,
                rh,
                rf,
                ProtocolOpcode::TransferSetupAck,
                &bytes[..n],
                4,
            );
            let h =
                command(receive(&mut queue, &mut replies, sender, sc, sh, sf, &packet).unwrap());
            queue
                .dispatch_transfer_feedback(
                    h,
                    &parent,
                    sender,
                    sh,
                    sf,
                    &mut tx,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            for i in 0..2 {
                tx.fragment(txh, i, 22, &mut [0; 32]).unwrap();
                tx.fragment_disposition(txh, i, ucn_transport::Submission::Submitted)
                    .unwrap();
            }
            let terminal = ucn_transport::TransferTerminal {
                key,
                state: 1,
                result: 0,
            };
            let n = ucn_transport::encode_terminal(terminal, &mut bytes).unwrap();
            let packet = protect(
                receiver,
                rc,
                rh,
                rf,
                ProtocolOpcode::TransferTerminalReceipt,
                &bytes[..n],
                5,
            );
            let h =
                command(receive(&mut queue, &mut replies, sender, sc, sh, sf, &packet).unwrap());
            queue
                .dispatch_transfer_feedback(
                    h,
                    &parent,
                    sender,
                    sh,
                    sf,
                    &mut tx,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            assert_eq!(tx.view(txh).unwrap().phase, TransferPhase::Done);
            let n = ucn_transport::encode_terminal(
                ucn_transport::TransferTerminal {
                    result: 7,
                    ..terminal
                },
                &mut bytes,
            )
            .unwrap();
            let packet = protect(
                receiver,
                rc,
                rh,
                rf,
                ProtocolOpcode::TransferTerminalReceipt,
                &bytes[..n],
                6,
            );
            let h =
                command(receive(&mut queue, &mut replies, sender, sc, sh, sf, &packet).unwrap());
            let old = tx.view(txh).unwrap();
            assert_eq!(
                queue.dispatch_transfer_feedback(
                    h,
                    &parent,
                    sender,
                    sh,
                    sf,
                    &mut tx,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::Replay)
            );
            assert_eq!(tx.view(txh).unwrap(), old);
            assert!(queue.view(h).is_ok());
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)]
fn persistent_backpressure_is_bounded_and_unknown_reopen_never_drops_a_holder() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, _) = published_parent(false);
            let mut queue = Queue::new(1).unwrap();
            let mut replies = Replies::new(2, 10, 2).unwrap();
            let mut rx = Transfer::new(2, 200, 0xff00).unwrap();
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
            let h =
                command(receive(&mut queue, &mut replies, receiver, rc, rh, rf, &request).unwrap());
            let (_, reply) = queue
                .dispatch_transfer_setup(
                    h,
                    &parent,
                    receiver,
                    rh,
                    rf,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            let ingress = AdapterEventIngress::<1, 1, 1, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            replies
                .seal(
                    reply,
                    receiver,
                    rc,
                    &mut Counter(1),
                    rh,
                    rf,
                    link,
                    &mut SecurityPacketWorkspace::<128>::new(),
                )
                .unwrap();
            let mut driver = Driver::new(DriverSubmit::NotSubmitted);
            for now in [21, 31] {
                replies
                    .submit(
                        reply,
                        receiver,
                        rh,
                        CurrentFacts { now_us: now, ..rf },
                        &mut adapter,
                        &mut driver,
                        link,
                    )
                    .unwrap();
            }
            assert_eq!(
                replies.submit(
                    reply,
                    receiver,
                    rh,
                    CurrentFacts { now_us: 41, ..rf },
                    &mut adapter,
                    &mut driver,
                    link
                ),
                Err(Error::Exhausted)
            );
            let old = replies.view(reply).unwrap();
            assert_eq!(
                receive(
                    &mut queue,
                    &mut replies,
                    receiver,
                    rc,
                    rh,
                    CurrentFacts { now_us: 1021, ..rf },
                    &request
                ),
                Err(Error::Timeout)
            );
            assert_eq!(replies.view(reply).unwrap(), old);
            assert_eq!(driver.frames.len(), 2);
            replies.retire_cache(reply, 1021).unwrap();

            // New request after old receipt expiry, with a genuinely fresh Transfer ID.
            rx.step(1021, 2).unwrap();
            let old_rx = rx.find_rx(crate::parent(), 1).unwrap();
            rx.retire_rx(old_rx, 1221).unwrap();
            let n = ucn_transport::encode_setup(setup(2), &mut bytes).unwrap();
            let request = protect(
                sender,
                sc,
                sh,
                CurrentFacts { now_us: 1300, ..sf },
                ProtocolOpcode::TransferSetup,
                &bytes[..n],
                2,
            );
            let rf1300 = CurrentFacts { now_us: 1300, ..rf };
            let h = command(
                receive(&mut queue, &mut replies, receiver, rc, rh, rf1300, &request).unwrap(),
            );
            let (_, next) = queue
                .dispatch_transfer_setup(
                    h,
                    &parent,
                    receiver,
                    rh,
                    rf1300,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new(),
                )
                .unwrap();
            assert_ne!(next, reply);
            assert_eq!(replies.view(reply), Err(Error::State));
            replies
                .seal(
                    next,
                    receiver,
                    rc,
                    &mut Counter(2),
                    rh,
                    rf1300,
                    link,
                    &mut SecurityPacketWorkspace::<128>::new(),
                )
                .unwrap();
            driver.submit = DriverSubmit::Unknown;
            assert_eq!(
                replies.submit(next, receiver, rh, rf1300, &mut adapter, &mut driver, link),
                Err(Error::InDoubt)
            );
            let holder = replies.view(next).unwrap();
            assert!(holder.physical_pending);
            assert_eq!(
                replies.retire_attempt(next, &mut adapter),
                Err(Error::State)
            );
            adapter.reopen_link(link, 2, 128).unwrap();
            assert_eq!(
                replies.retire_attempt(next, &mut adapter),
                Err(Error::InDoubt)
            );
            assert_eq!(replies.view(next).unwrap(), holder);
            ingress
                .tx_complete(
                    *driver.tokens.last().unwrap(),
                    TerminalOutcome::Failure(Error::Timeout),
                )
                .unwrap();
            replies.retire_attempt(next, &mut adapter).unwrap();
            assert!(!replies.view(next).unwrap().physical_pending);
            // Still pinned to old exact Link; reopening cannot silently authorize cached bytes.
            let new_link = adapter.link_handle(0).unwrap();
            assert_eq!(
                replies.submit(
                    next,
                    receiver,
                    rh,
                    CurrentFacts { now_us: 1310, ..rf },
                    &mut adapter,
                    &mut driver,
                    new_link
                ),
                Err(Error::State)
            );
        });
    });
}

#[test]
fn expired_queued_setup_and_authenticated_wrong_direction_never_start_a_transfer() {
    security::exercise_opcodes(1, 2, OPS, |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, OPS, |receiver, rh, rf, rc| {
            let (parent, _) = published_parent(false);
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
            let mut queue = Queue::new(1).unwrap();
            let mut replies = Replies::new(2, 10, 2).unwrap();
            let mut rx = Transfer::new(2, 200, 0xff00).unwrap();
            let h =
                command(receive(&mut queue, &mut replies, receiver, rc, rh, rf, &request).unwrap());
            assert!(
                queue
                    .dispatch_transfer_setup(
                        h,
                        &parent,
                        receiver,
                        rh,
                        CurrentFacts { now_us: 1021, ..rf },
                        &mut rx,
                        &mut replies,
                        &mut DigestWorkspace::new()
                    )
                    .is_err()
            );
            assert!(queue.view(h).is_ok());
            assert_eq!(rx.find_rx(crate::parent(), 1), Err(Error::NotFound));
            let reversed = protect(
                receiver,
                rc,
                rh,
                rf,
                ProtocolOpcode::TransferSetup,
                &bytes[..n],
                1,
            );
            let mut wrong_queue = Queue::new(3).unwrap();
            let h = command(
                receive(
                    &mut wrong_queue,
                    &mut replies,
                    sender,
                    sc,
                    sh,
                    sf,
                    &reversed,
                )
                .unwrap(),
            );
            assert_eq!(
                wrong_queue.dispatch_transfer_setup(
                    h,
                    &parent,
                    sender,
                    sh,
                    sf,
                    &mut rx,
                    &mut replies,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::Access)
            );
            assert!(wrong_queue.view(h).is_ok());
            assert_eq!(rx.find_rx(crate::parent(), 1), Err(Error::NotFound));
        });
    });
}
