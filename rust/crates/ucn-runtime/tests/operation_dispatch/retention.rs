//! Real C0 authentication, fake Adapter, raw Foundation reload and new-session recovery.
use super::*;

#[test]
fn stale_ram_journal_never_authorizes_ack_or_recovery_after_media_advances() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = committed(p, f);
                let receipt = op.receipt(&mut DigestWorkspace::new()).unwrap();
                let mut body = [0; 256];
                let n = p.copy_body(contract().domain, &mut body).unwrap();
                let mut newer = Journal::new(contract()).unwrap();
                newer
                    .restore(
                        flash::base(p, contract()),
                        &body[..n],
                        [5; 16],
                        &mut CodecWorkspace::new(),
                    )
                    .unwrap();
                newer.acknowledge_result(key(), 17, 1).unwrap();
                newer
                    .transition(
                        OperationPhase::Tombstoned,
                        flash::base(p, contract()),
                        0,
                        &[],
                        18,
                        100,
                        8,
                        &mut CodecWorkspace::new(),
                    )
                    .unwrap();
                persist(&mut newer, p, f, 19);
                assert_eq!(
                    p.domain_get(contract().domain).unwrap().record_generation,
                    4
                );
                assert_eq!(op.record_generation(), 3);
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let mut ack = [0; 48];
                encode_operation_ack(receipt, &mut ack).unwrap();
                let raw = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationResultAck,
                    &ack,
                    1,
                );
                let h =
                    fixture::command(receive(&mut q, &mut r, server, sc, sh, sf, &raw).unwrap());
                let calls = f.calls;
                assert_eq!(
                    q.dispatch_result_ack(
                        h,
                        &mut op,
                        p,
                        server,
                        sh,
                        sf,
                        10,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new()
                    ),
                    Err(Error::State)
                );
                assert_eq!(op.gc_after(), None);
                assert_eq!(f.calls, calls);
                q.ack(h).unwrap();
                let mut request = [0; 28];
                encode_operation_key(wire(key()), &mut request).unwrap();
                let raw = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationResume,
                    &request,
                    2,
                );
                let h =
                    fixture::command(receive(&mut q, &mut r, server, sc, sh, sf, &raw).unwrap());
                assert_eq!(
                    q.dispatch_operation_resume(
                        h,
                        &op,
                        p,
                        server,
                        sh,
                        sf,
                        100,
                        &mut r,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new()
                    ),
                    Err(Error::State)
                );
                assert_eq!(f.calls, calls);
            });
        });
    });
}

#[test]
fn missing_ack_outbound_acl_prevents_result_delivery_and_ack_reservation() {
    security::exercise_opcodes(
        1,
        2,
        &[ProtocolOpcode::OperationResult],
        |client, ch, cf, cc| {
            security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
                let mut result = OperationResult {
                    receipt: OperationReceipt {
                        key: wire(key()),
                        result_digest: operation_result_digest(
                            wire(key()),
                            3,
                            7,
                            b"ok",
                            &mut DigestWorkspace::new(),
                        )
                        .unwrap(),
                        phase: 3,
                    },
                    status: 7,
                    length: 2,
                    payload: [0; 146],
                };
                result.payload[..2].copy_from_slice(b"ok");
                let mut bytes = [0; 56];
                encode_operation_result(result, &mut bytes).unwrap();
                let packet = protect(
                    server,
                    sc,
                    sh,
                    sf,
                    ProtocolOpcode::OperationResult,
                    &bytes,
                    1,
                );
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let h =
                    fixture::command(receive(&mut q, &mut r, client, cc, ch, cf, &packet).unwrap());
                let mut recovery = OperationRecovery::new(key(), 21, 100).unwrap();
                let mut output = [0xa5; 8];
                assert_eq!(
                    q.dispatch_recovered_result(
                        h,
                        &mut recovery,
                        client,
                        ch,
                        cf,
                        &mut r,
                        &mut output,
                        &mut DigestWorkspace::new()
                    ),
                    Err(Error::Access)
                );
                assert_eq!(output, [0xa5; 8]);
                assert_eq!(q.peek_next().unwrap(), h);
            });
        },
    );
}

#[test]
#[allow(clippy::too_many_lines)] // Explicit maximum-body reload/seal/Driver boundary.
fn maximum_result_seals_before_driver_and_short_holder_rejects_without_dispatch() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = OperationOwner::<146>::new(contract()).unwrap();
                op.begin(
                    key(),
                    flash::base(p, contract()),
                    11,
                    100,
                    1,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
                let persist = |op: &mut OperationOwner<146>,
                               p: &mut flash::Owner<'_>,
                               f: &mut flash::Flash,
                               now| {
                    let h = p.submit(&op.requirement().unwrap(), now).unwrap();
                    op.bind(h).unwrap();
                    drive_proof(p, f, h, now);
                    op.confirm(h, p.proof_get(h).unwrap()).unwrap();
                    p.proof_retire(h).unwrap();
                };
                persist(&mut op, p, f, 12);
                op.transition(
                    OperationPhase::Executing,
                    flash::base(p, contract()),
                    0,
                    &[],
                    13,
                    100,
                    2,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
                persist(&mut op, p, f, 14);
                let permit = op.handoff(key()).unwrap();
                op.executor_result(permit, 7, &[0x35; 146]).unwrap();
                op.transition(
                    OperationPhase::CommittedResult,
                    flash::base(p, contract()),
                    7,
                    &[0x35; 146],
                    15,
                    100,
                    3,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
                persist(&mut op, p, f, 16);
                let mut bytes = [0; 28];
                encode_operation_key(wire(key()), &mut bytes).unwrap();
                let packet = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationResume,
                    &bytes,
                    1,
                );
                let mut q = Queue::new(20).unwrap();
                let mut small = Feedback::new(21, 1, 4).unwrap();
                let h = fixture::command(
                    receive(&mut q, &mut small, server, sc, sh, sf, &packet).unwrap(),
                );
                assert_eq!(
                    q.dispatch_operation_resume(
                        h,
                        &op,
                        p,
                        server,
                        sh,
                        sf,
                        100,
                        &mut small,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new()
                    ),
                    Err(Error::NoSpace)
                );
                assert_eq!(q.peek_next().unwrap(), h);
                let mut large = ControlReplies::<1, 256>::new(22, 1, 4).unwrap();
                let reply = q
                    .dispatch_operation_resume(
                        h,
                        &op,
                        p,
                        server,
                        sh,
                        sf,
                        100,
                        &mut large,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new(),
                    )
                    .unwrap();
                let ingress = AdapterEventIngress::<1, 1, 1, 256>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 256).unwrap();
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                large
                    .seal(
                        reply,
                        server,
                        sc,
                        &mut Counter(10),
                        sh,
                        sf,
                        link,
                        &mut SecurityPacketWorkspace::<256>::new(),
                    )
                    .unwrap();
                large
                    .submit(reply, server, sh, sf, &mut adapter, &mut driver, link)
                    .unwrap();
                assert_eq!(driver.frames[0].len(), 245);
                large.retire_attempt(reply, &mut adapter).unwrap();
                assert_eq!(large.view(reply).unwrap().phase, ReplyPhase::Cached);
            });
        });
    });
}

#[test]
fn authenticated_but_incorrect_result_digest_is_zero_delivery_and_zero_ack() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            let mut result = OperationResult {
                receipt: OperationReceipt {
                    key: wire(key()),
                    result_digest: [8; 16],
                    phase: 3,
                },
                status: 7,
                length: 2,
                payload: [0; 146],
            };
            result.payload[..2].copy_from_slice(b"ok");
            let mut body = [0; 56];
            encode_operation_result(result, &mut body).unwrap();
            let packet = protect(
                server,
                sc,
                sh,
                sf,
                ProtocolOpcode::OperationResult,
                &body,
                1,
            );
            let mut q = Queue::new(20).unwrap();
            let mut r = Feedback::new(21, 1, 4).unwrap();
            let h = fixture::command(receive(&mut q, &mut r, client, cc, ch, cf, &packet).unwrap());
            let mut recovery = OperationRecovery::new(key(), 21, 100).unwrap();
            let mut out = [0xa5; 8];
            assert_eq!(
                q.dispatch_recovered_result(
                    h,
                    &mut recovery,
                    client,
                    ch,
                    cf,
                    &mut r,
                    &mut out,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::Security)
            );
            assert_eq!(out, [0xa5; 8]);
            assert_eq!(q.peek_next().unwrap(), h);
            assert_eq!(
                q.dispatch_recovered_result(
                    h,
                    &mut recovery,
                    client,
                    ch,
                    CurrentFacts { now_us: 100, ..cf },
                    &mut r,
                    &mut out,
                    &mut DigestWorkspace::new()
                ),
                Err(Error::Timeout)
            );
            assert_eq!(out, [0xa5; 8]);
        });
    });
}

#[test]
fn different_authenticated_client_cannot_resume_original_operation() {
    security::exercise_opcodes(3, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 3, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let op = committed(p, f);
                let mut body = [0; 28];
                encode_operation_key(wire(key()), &mut body).unwrap();
                let packet = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationResume,
                    &body,
                    1,
                );
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let h =
                    fixture::command(receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap());
                let before = f.calls;
                assert_eq!(
                    q.dispatch_operation_resume(
                        h,
                        &op,
                        p,
                        server,
                        sh,
                        sf,
                        100,
                        &mut r,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new()
                    ),
                    Err(Error::Access)
                );
                assert_eq!(f.calls, before);
                assert_eq!(op.key(), Some(key()));
                assert_eq!(q.peek_next().unwrap(), h);
            });
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Both distinct terminal phases use the real raw pipeline.
fn no_effect_can_ack_but_in_doubt_can_only_observe_never_gc() {
    for phase in [OperationPhase::AbortedNoEffect, OperationPhase::InDoubt] {
        security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
            security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
                flash::exercise(contract(), |p, f| {
                    let mut op = prepared(p, f, key());
                    if phase == OperationPhase::InDoubt {
                        op.transition(
                            OperationPhase::Executing,
                            flash::base(p, contract()),
                            0,
                            &[],
                            13,
                            100,
                            2,
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                        persist(&mut op, p, f, 14);
                        let _permit = op.handoff(key()).unwrap(); // Unknown executor completion, not a no-effect observation.
                    }
                    op.transition(
                        phase,
                        flash::base(p, contract()),
                        -12,
                        &[],
                        15,
                        100,
                        3,
                        &mut CodecWorkspace::new(),
                    )
                    .unwrap();
                    persist(&mut op, p, f, 16);
                    let mut body = [0; 28];
                    encode_operation_key(wire(key()), &mut body).unwrap();
                    let packet = protect(
                        client,
                        cc,
                        ch,
                        cf,
                        ProtocolOpcode::OperationResume,
                        &body,
                        1,
                    );
                    let mut q = Queue::new(20).unwrap();
                    let mut r = Feedback::new(21, 1, 4).unwrap();
                    let h = fixture::command(
                        receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap(),
                    );
                    let reply = q
                        .dispatch_operation_resume(
                            h,
                            &op,
                            p,
                            server,
                            sh,
                            sf,
                            100,
                            &mut r,
                            &mut DigestWorkspace::new(),
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                    let ingress = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                    let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                    let link = adapter.open_link(0, 1, 128).unwrap();
                    let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                    let result = emit(
                        &mut r,
                        reply,
                        server,
                        sc,
                        sh,
                        sf,
                        &mut Counter(10),
                        &mut adapter,
                        &mut driver,
                        link,
                    );
                    let mut cq = Queue::new(50).unwrap();
                    let mut cr = Feedback::new(51, 1, 4).unwrap();
                    let h = fixture::command(
                        receive(&mut cq, &mut cr, client, cc, ch, cf, &result).unwrap(),
                    );
                    let mut watch = OperationRecovery::new(key(), 21, 100).unwrap();
                    let mut out = [0xa5; 8];
                    let recovered = cq
                        .dispatch_recovered_result(
                            h,
                            &mut watch,
                            client,
                            ch,
                            cf,
                            &mut cr,
                            &mut out,
                            &mut DigestWorkspace::new(),
                        )
                        .unwrap();
                    assert_eq!(recovered.phase, phase as u8);
                    assert_eq!(recovered.bytes, 0);
                    assert_eq!(out, [0xa5; 8]);
                    assert_eq!(
                        recovered.ack.is_some(),
                        phase == OperationPhase::AbortedNoEffect
                    );
                    if phase == OperationPhase::InDoubt {
                        assert!(op.acknowledge_result(key(), 21, 1).is_err());
                        assert!(
                            OperationGc::new()
                                .begin(&mut op, p, 30, 100, 4, &mut CodecWorkspace::new())
                                .is_err()
                        );
                        assert_eq!(op.view().0, Some(phase));
                    }
                });
            });
        });
    }
}

#[test]
#[allow(clippy::too_many_lines)]
fn result_resume_ack_retention_reload_tombstone_and_reboot_never_reexecute() {
    security::exercise_generations(1, 2, OPS, 2, 2, 2, |client, ch, cf, cc| {
        security::exercise_generations(2, 1, OPS, 2, 2, 2, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = committed(p, f); // Journal retains original Session/Key/Policy=1.
                let original = op.key();
                let generation = op.record_generation();
                let mut body = [0; 28];
                encode_operation_key(wire(key()), &mut body).unwrap();
                let packet = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationResume,
                    &body,
                    1,
                );
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let h =
                    fixture::command(receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap());
                let before = f.calls;
                let reply = q
                    .dispatch_operation_resume(
                        h,
                        &op,
                        p,
                        server,
                        sh,
                        sf,
                        100,
                        &mut r,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new(),
                    )
                    .unwrap();
                assert_eq!(op.key(), original);
                assert_eq!(op.record_generation(), generation);
                assert_eq!(f.calls, before);
                assert!(op.handoff(key()).is_err()); // New Session never reactivates old execution.
                let ingress = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                let result = emit(
                    &mut r,
                    reply,
                    server,
                    sc,
                    sh,
                    sf,
                    &mut Counter(10),
                    &mut adapter,
                    &mut driver,
                    link,
                );
                let mut cq = Queue::new(50).unwrap();
                let mut cr = Feedback::new(51, 1, 4).unwrap();
                let client_now = CurrentFacts { now_us: 22, ..cf };
                let rh = fixture::command(
                    receive(&mut cq, &mut cr, client, cc, ch, client_now, &result).unwrap(),
                );
                let mut recovery = OperationRecovery::new(key(), 21, 100).unwrap();
                let mut out = [0xa5; 8];
                let recovered = cq
                    .dispatch_recovered_result(
                        rh,
                        &mut recovery,
                        client,
                        ch,
                        client_now,
                        &mut cr,
                        &mut out,
                        &mut DigestWorkspace::new(),
                    )
                    .unwrap();
                assert_eq!(
                    (recovered.phase, recovered.status, recovered.bytes),
                    (3, 7, 2)
                );
                assert_eq!(&out[..2], b"ok");
                assert_eq!(&out[2..], &[0xa5; 6]);
                let ack = emit(
                    &mut cr,
                    recovered.ack.unwrap(),
                    client,
                    cc,
                    ch,
                    client_now,
                    &mut Counter(2),
                    &mut adapter,
                    &mut driver,
                    link,
                );
                let ack_now = CurrentFacts { now_us: 23, ..sf };
                let ah = fixture::command(
                    receive(&mut q, &mut r, server, sc, sh, ack_now, &ack).unwrap(),
                );
                q.dispatch_result_ack(
                    ah,
                    &mut op,
                    p,
                    server,
                    sh,
                    ack_now,
                    10,
                    &mut DigestWorkspace::new(),
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
                assert_eq!(op.gc_after(), Some(33));
                // Fresh transaction with the same precise ACK is idempotent, no renewal.
                let receipt = op.receipt(&mut DigestWorkspace::new()).unwrap();
                let mut ackbody = [0; 48];
                encode_operation_ack(receipt, &mut ackbody).unwrap();
                let repeat = protect(
                    client,
                    cc,
                    ch,
                    CurrentFacts { now_us: 24, ..cf },
                    ProtocolOpcode::OperationResultAck,
                    &ackbody,
                    3,
                );
                let repeated_at = CurrentFacts { now_us: 24, ..sf };
                let ah = fixture::command(
                    receive(&mut q, &mut r, server, sc, sh, repeated_at, &repeat).unwrap(),
                );
                q.dispatch_result_ack(
                    ah,
                    &mut op,
                    p,
                    server,
                    sh,
                    repeated_at,
                    10,
                    &mut DigestWorkspace::new(),
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
                assert_eq!(op.gc_after(), Some(33));
                // Crash before tombstone loses ACK permission, never grants early GC.
                flash::reopen(contract(), f, |recovered_p, _| {
                    let mut bytes = [0; 256];
                    let n = recovered_p
                        .copy_body(contract().domain, &mut bytes)
                        .unwrap();
                    let mut restored = Journal::new(contract()).unwrap();
                    restored
                        .restore(
                            flash::base(recovered_p, contract()),
                            &bytes[..n],
                            [5; 16],
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                    assert_eq!(restored.gc_after(), None);
                    assert!(restored.handoff(key()).is_err());
                    assert_eq!(
                        OperationGc::new().begin(
                            &mut restored,
                            recovered_p,
                            33,
                            100,
                            7,
                            &mut CodecWorkspace::new()
                        ),
                        Err(Error::State)
                    );
                });
                let mut gc = OperationGc::new();
                assert_eq!(
                    gc.begin(&mut op, p, 32, 100, 7, &mut CodecWorkspace::new()),
                    Err(Error::State)
                );
                let io = gc
                    .begin(&mut op, p, 33, 100, 7, &mut CodecWorkspace::new())
                    .unwrap();
                assert!(gc.finish(&mut op, p).is_err());
                assert_eq!(gc.pending_io(), Some(io));
                assert_eq!(op.copy_result(&mut out), Err(Error::State));
                drive_proof(p, f, io, 34);
                gc.finish(&mut op, p).unwrap();
                assert_eq!(op.view().0, Some(OperationPhase::Tombstoned));
                assert_eq!(op.key(), original);
                assert_eq!(gc.pending_io(), None);
                let mut bytes = [0; 256];
                let n = p.copy_body(contract().domain, &mut bytes).unwrap();
                assert_eq!(n, 110); // Old result body reclaimed, key retained.
                assert_eq!(
                    decode_operation(&bytes[..n], [5; 16]).unwrap().1,
                    OperationPhase::Tombstoned
                );
                flash::reopen(contract(), f, |recovered_p, _| {
                    let mut restored = Journal::new(contract()).unwrap();
                    restored
                        .restore(
                            flash::base(recovered_p, contract()),
                            &bytes[..n],
                            [5; 16],
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                    assert!(restored.handoff(key()).is_err());
                    assert!(restored.copy_result(&mut out).is_err());
                    let mut changed = key();
                    changed.service.operation = 2;
                    assert!(
                        restored
                            .begin(
                                changed,
                                flash::base(recovered_p, contract()),
                                35,
                                100,
                                8,
                                &mut CodecWorkspace::new()
                            )
                            .is_err()
                    ); // No unsafe slot reuse.
                });
            });
        });
    });
}

#[test]
fn wrong_digest_and_identity_ack_never_arm_gc() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = committed(p, f);
                let original = op.receipt(&mut DigestWorkspace::new()).unwrap();
                let mut wrong = original;
                wrong.result_digest[0] ^= 1;
                let mut wrong_key = original;
                wrong_key.key.operation += 1;
                for (tx, value) in [(1, wrong), (2, wrong_key)] {
                    let mut bytes = [0; 48];
                    encode_operation_ack(value, &mut bytes).unwrap();
                    let packet = protect(
                        client,
                        cc,
                        ch,
                        cf,
                        ProtocolOpcode::OperationResultAck,
                        &bytes,
                        tx,
                    );
                    let mut q = Queue::new(20).unwrap();
                    let mut r = Feedback::new(21, 1, 4).unwrap();
                    let h = fixture::command(
                        receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap(),
                    );
                    let calls = f.calls;
                    assert!(
                        q.dispatch_result_ack(
                            h,
                            &mut op,
                            p,
                            server,
                            sh,
                            sf,
                            10,
                            &mut DigestWorkspace::new(),
                            &mut CodecWorkspace::new()
                        )
                        .is_err()
                    );
                    assert_eq!(op.gc_after(), None);
                    assert_eq!(f.calls, calls);
                    assert_eq!(q.peek_next().unwrap(), h);
                }
            });
        });
    });
}

#[test]
fn revoked_session_after_raw_admission_cannot_ack_or_resume() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = committed(p, f);
                let receipt = op.receipt(&mut DigestWorkspace::new()).unwrap();
                let mut body = [0; 48];
                encode_operation_ack(receipt, &mut body).unwrap();
                let packet = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationResultAck,
                    &body,
                    1,
                );
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let h =
                    fixture::command(receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap());
                server.fence(sh).unwrap();
                assert!(
                    q.dispatch_result_ack(
                        h,
                        &mut op,
                        p,
                        server,
                        sh,
                        sf,
                        10,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new()
                    )
                    .is_err()
                );
                assert_eq!(op.gc_after(), None);
                assert_eq!(q.peek_next().unwrap(), h);
                assert!(
                    OperationGc::new()
                        .begin(&mut op, p, 30, 100, 4, &mut CodecWorkspace::new())
                        .is_err()
                );
            });
        });
    });
}

#[test]
fn unfinished_and_rolled_back_session_recovery_cannot_acquire_authority() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let op = prepared(p, f, key());
                let mut body = [0; 28];
                encode_operation_key(wire(key()), &mut body).unwrap();
                let packet = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationResume,
                    &body,
                    1,
                );
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let h =
                    fixture::command(receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap());
                assert_eq!(
                    q.dispatch_operation_resume(
                        h,
                        &op,
                        p,
                        server,
                        sh,
                        sf,
                        100,
                        &mut r,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new()
                    ),
                    Err(Error::Argument)
                );
                assert_eq!(op.view(), (Some(OperationPhase::Prepared), false, false));
            });
            flash::exercise(contract(), |p, f| {
                let mut k = key();
                k.service.session = 2;
                let mut op = prepared(p, f, k);
                op.transition(
                    OperationPhase::AbortedNoEffect,
                    flash::base(p, contract()),
                    -12,
                    &[],
                    13,
                    100,
                    2,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
                persist(&mut op, p, f, 14);
                let mut body = [0; 28];
                encode_operation_key(wire(k), &mut body).unwrap();
                let packet = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationResume,
                    &body,
                    2,
                );
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let h =
                    fixture::command(receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap());
                assert_eq!(
                    q.dispatch_operation_resume(
                        h,
                        &op,
                        p,
                        server,
                        sh,
                        sf,
                        100,
                        &mut r,
                        &mut DigestWorkspace::new(),
                        &mut CodecWorkspace::new()
                    ),
                    Err(Error::Access)
                );
                assert_eq!(op.gc_after(), None);
            });
        });
    });
}

#[test]
fn gc_failure_keeps_exact_obligation_and_fences_result() {
    flash::exercise(contract(), |p, f| {
        let mut op = committed(p, f);
        op.acknowledge_result(key(), 21, 1).unwrap();
        let mut gc = OperationGc::new();
        let io = gc
            .begin(&mut op, p, 22, 100, 4, &mut CodecWorkspace::new())
            .unwrap();
        assert_eq!(gc.retire_failed(&mut op, p), Err(Error::State));
        assert_eq!(gc.pending_io(), Some(io));
        let before = f.slots;
        let mut corrupted = false;
        for _ in 0..10 {
            p.step(f, 23, 1).unwrap();
            if let Some(i) = f.slots.iter().zip(before.iter()).position(|(n, o)| n != o) {
                f.slots[i][0] ^= 1;
                corrupted = true;
                break;
            }
        }
        assert!(corrupted);
        assert_eq!(p.step(f, 23, 1).unwrap().domains_faulted, 1);
        assert_eq!(gc.pending_io(), Some(io));
        assert!(gc.finish(&mut op, p).is_err());
        assert_eq!(gc.retire_failed(&mut op, p).unwrap(), Error::Malformed);
        assert_eq!(gc.pending_io(), None);
        assert!(op.copy_result(&mut [0; 32]).is_err());
        assert!(op.handoff(key()).is_err());
    });
}
