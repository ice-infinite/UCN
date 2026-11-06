#![cfg(feature = "operation-control")]
#![cfg_attr(not(feature = "operation-control"), allow(missing_docs))]
//! Real authenticated control and Foundation reload; no production hardware.
//! 实际认证控制与Foundation回读，所有介质/Driver均为Host测试实现。
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::{security, *};
use security::flash;
use ucn_adapter::*;
use ucn_digest::DigestWorkspace;
use ucn_durable::DurableContract;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_runtime::*;
use ucn_security::*;
use ucn_service::*;
use ucn_types::*;

const OPS: &[ProtocolOpcode] = &[
    ProtocolOpcode::OperationQuery,
    ProtocolOpcode::OperationCancel,
    ProtocolOpcode::OperationReceipt,
    ProtocolOpcode::OperationResultAck,
    ProtocolOpcode::OperationResume,
    ProtocolOpcode::OperationResult,
];
#[path = "operation_dispatch/retention.rs"]
mod retention;
#[test]
fn pending_reply_cannot_seal_retire_or_escape_through_premature_queue_ack() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = prepared(p, f, key());
                let request = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationCancel,
                    &encode_cancel(key()),
                    1,
                );
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let command = fixture::command(
                    receive(&mut q, &mut r, server, sc, sh, sf, &request).unwrap(),
                );
                let mut c = Coordinator::new(30, 1000).unwrap();
                let h = pending(
                    dispatch(&mut c, &mut q, command, server, sh, sf, &mut op, p, &mut r).unwrap(),
                );
                let blocked = c.pending_reply(h).unwrap();
                assert_eq!(r.view(blocked).unwrap().phase, ReplyPhase::AwaitCommit);
                let ingress = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                let mut counter = Counter(10);
                assert_eq!(
                    r.seal(
                        blocked,
                        server,
                        sc,
                        &mut counter,
                        sh,
                        sf,
                        link,
                        &mut SecurityPacketWorkspace::<128>::new()
                    ),
                    Err(Error::State)
                );
                assert_eq!(counter.0, 10);
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                assert_eq!(
                    r.submit(blocked, server, sh, sf, &mut adapter, &mut driver, link),
                    Err(Error::State)
                );
                assert!(driver.frames.is_empty());
                assert_eq!(r.retire_cache(blocked, 1021), Err(Error::State));
                // Even out-of-contract premature acknowledgement cannot publish a pending reply.
                q.ack(command).unwrap();
                assert_eq!(
                    receive(
                        &mut q,
                        &mut r,
                        server,
                        sc,
                        sh,
                        CurrentFacts { now_us: 1021, ..sf },
                        &request
                    ),
                    Err(Error::State)
                );
                assert_eq!(r.view(blocked).unwrap().phase, ReplyPhase::AwaitCommit);
                assert_eq!(c.reject_confirmed(h, &mut q, &mut r), Err(Error::State));
                assert_eq!(op.view(), (Some(OperationPhase::Prepared), true, false));
            });
        });
    });
}
#[test]
fn failed_readback_retires_exact_obligations_but_journal_remains_fenced() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = prepared(p, f, key());
                let request = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationCancel,
                    &encode_cancel(key()),
                    1,
                );
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let command = fixture::command(
                    receive(&mut q, &mut r, server, sc, sh, sf, &request).unwrap(),
                );
                let mut c = Coordinator::new(30, 1000).unwrap();
                let h = pending(
                    dispatch(&mut c, &mut q, command, server, sh, sf, &mut op, p, &mut r).unwrap(),
                );
                let blocked = c.pending_reply(h).unwrap();
                let io = c.submit(h, &mut op, p, 22).unwrap();
                assert_eq!(
                    c.retire_failed(h, &mut q, &mut op, p, &mut r),
                    Err(Error::State)
                );
                let before = f.slots;
                let mut corrupted = false;
                for _ in 0..8 {
                    p.step(f, 23, 1).unwrap();
                    if let Some(slot) = f
                        .slots
                        .iter()
                        .zip(before.iter())
                        .position(|(new, old)| new != old)
                    {
                        f.slots[slot][0] ^= 1; // Corrupt just-written raw bytes before readback.
                        corrupted = true;
                        break;
                    }
                }
                assert!(corrupted);
                assert_eq!(p.step(f, 23, 1).unwrap().domains_faulted, 1);
                assert_eq!(
                    p.request_get(io).unwrap().state,
                    ucn_persistence::RequestState::Failed
                );
                assert_eq!(
                    p.request_get(io).unwrap().terminal_error,
                    Some(Error::Malformed)
                );
                assert_eq!(f.witness, 1);
                assert_eq!(
                    finish(
                        &mut c,
                        h,
                        &mut q,
                        &mut op,
                        p,
                        server,
                        sh,
                        CurrentFacts { now_us: 24, ..sf },
                        &mut r
                    ),
                    Err(Error::State)
                );
                assert_eq!(r.view(blocked).unwrap().phase, ReplyPhase::AwaitCommit);
                assert_eq!(
                    c.retire_failed(h, &mut q, &mut op, p, &mut r).unwrap(),
                    Error::Malformed
                );
                assert_eq!(c.view(h), Err(Error::State));
                assert_eq!(r.view(blocked), Err(Error::State));
                assert!(p.request_get(io).is_err());
                assert!(op.handoff(key()).is_err());
                assert!(op.receipt(&mut DigestWorkspace::new()).is_err());
                assert_eq!(q.peek_next(), Err(Error::NotFound));
                assert_eq!(op.view(), (Some(OperationPhase::Prepared), false, false));
            });
        });
    });
}
type Journal = OperationOwner<32>;
type Coordinator = OperationCoordinator<1>;
type Feedback = ControlReplies<2, 128>;
fn contract() -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::OperationJournal,
            id: 5,
        },
        schema: 1,
        version: 1,
        operation: 0x0701,
    }
}
fn key() -> OperationKey {
    OperationKey {
        service: ServiceKey {
            realm: 1,
            client: [1; 16],
            server: [2; 16],
            client_generation: 1,
            server_generation: 1,
            client_address: 1,
            server_address: 2,
            operation: 1,
            service: 0x40,
            opcode: 9,
            session: 1,
            key_generation: 1,
            origin: OriginSecurity::O1,
            policy: 1,
            security: [5; 16],
        },
        request_digest: [3; 16],
    }
}
fn wire(k: OperationKey) -> OperationWireKey {
    OperationWireKey {
        operation: k.service.operation,
        service: k.service.service,
        opcode: k.service.opcode,
        request_digest: k.request_digest,
    }
}
fn persist(
    owner: &mut Journal,
    persistence: &mut flash::Owner<'_>,
    provider: &mut flash::Flash,
    now: u64,
) {
    let h = persistence
        .submit(&owner.requirement().unwrap(), now)
        .unwrap();
    owner.bind(h).unwrap();
    drive_proof(persistence, provider, h, now);
    owner.confirm(h, persistence.proof_get(h).unwrap()).unwrap();
    persistence.proof_retire(h).unwrap();
}
fn drive_proof(
    p: &mut flash::Owner<'_>,
    f: &mut flash::Flash,
    h: ucn_persistence::PersistenceHandle,
    now: u64,
) {
    for _ in 0..128 {
        p.step(f, now, 1).unwrap();
        if p.proof_get(h).is_ok() {
            return;
        }
    }
    panic!("actual reload proof did not converge");
}
fn prepared(p: &mut flash::Owner<'_>, f: &mut flash::Flash, k: OperationKey) -> Journal {
    let mut op = Journal::new(contract()).unwrap();
    op.begin(
        k,
        flash::base(p, contract()),
        11,
        1000,
        1,
        &mut CodecWorkspace::new(),
    )
    .unwrap();
    persist(&mut op, p, f, 12);
    op
}
fn committed(p: &mut flash::Owner<'_>, f: &mut flash::Flash) -> Journal {
    let mut op = prepared(p, f, key());
    op.transition(
        OperationPhase::Executing,
        flash::base(p, contract()),
        0,
        &[],
        13,
        1000,
        2,
        &mut CodecWorkspace::new(),
    )
    .unwrap();
    persist(&mut op, p, f, 14);
    let permit = op.handoff(key()).unwrap();
    op.executor_result(permit, 7, b"ok").unwrap();
    op.transition(
        OperationPhase::CommittedResult,
        flash::base(p, contract()),
        7,
        b"ok",
        15,
        1000,
        3,
        &mut CodecWorkspace::new(),
    )
    .unwrap();
    persist(&mut op, p, f, 16);
    op
}
fn encode_cancel(k: OperationKey) -> [u8; 32] {
    let mut body = [0; 32];
    encode_operation_cancel(
        OperationCancel {
            key: wire(k),
            reason: 1,
        },
        &mut body,
    )
    .unwrap();
    body
}
#[allow(clippy::too_many_arguments)]
fn dispatch<const C: usize>(
    coordinator: &mut Coordinator,
    queue: &mut Queue,
    handle: ControlHandle,
    security: &SecurityOwner<'_, 2, 2>,
    sh: SessionHandle,
    facts: CurrentFacts,
    op: &mut Journal,
    persistence: &flash::Owner<'_>,
    replies: &mut ControlReplies<C, 128>,
) -> Result<OperationDispatch> {
    coordinator.dispatch(
        queue,
        handle,
        security,
        sh,
        facts,
        op,
        flash::base(persistence, contract()),
        replies,
        &mut CodecWorkspace::new(),
        &mut DigestWorkspace::new(),
    )
}
fn pending(v: OperationDispatch) -> OperationCancelHandle {
    let OperationDispatch::Pending(h) = v else {
        panic!("expected retained cancellation")
    };
    h
}
fn reply(v: OperationDispatch) -> ReplyHandle {
    let OperationDispatch::Reply(h) = v else {
        panic!("expected durable observation")
    };
    h
}
#[allow(clippy::too_many_arguments)]
fn finish<const C: usize>(
    coordinator: &mut Coordinator,
    handle: OperationCancelHandle,
    queue: &mut Queue,
    op: &mut Journal,
    persistence: &mut flash::Owner<'_>,
    security: &SecurityOwner<'_, 2, 2>,
    sh: SessionHandle,
    facts: CurrentFacts,
    replies: &mut ControlReplies<C, 128>,
) -> Result<ReplyHandle> {
    coordinator.finish(
        handle,
        queue,
        op,
        persistence,
        security,
        sh,
        facts,
        replies,
        &mut DigestWorkspace::new(),
    )
}
#[allow(clippy::too_many_arguments)]
fn emit(
    r: &mut Feedback,
    h: ReplyHandle,
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    sh: SessionHandle,
    facts: CurrentFacts,
    counter: &mut Counter,
    adapter: &mut AdapterOwner<'_, 1, 1, 1, 128>,
    driver: &mut Driver,
    link: LinkHandle,
) -> Packet {
    if r.view(h).unwrap().phase == ReplyPhase::AwaitSeal {
        r.seal(
            h,
            security,
            crypto,
            counter,
            sh,
            facts,
            link,
            &mut SecurityPacketWorkspace::<128>::new(),
        )
        .unwrap();
    }
    r.submit(h, security, sh, facts, adapter, driver, link)
        .unwrap();
    r.retire_attempt(h, adapter).unwrap();
    let mut packet = Packet {
        bytes: [0; 128],
        length: 0,
    };
    packet.length = r.copy_packet(h, &mut packet.bytes).unwrap();
    assert_eq!(
        &packet.bytes[..packet.length],
        driver.frames.last().unwrap()
    );
    packet
}

#[test]
#[allow(clippy::too_many_lines)]
fn authenticated_cancel_waits_for_reload_then_exact_wire_receipt_retries_without_execution() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = prepared(p, f, key());
                let request = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationCancel,
                    &encode_cancel(key()),
                    1,
                );
                let ingress = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                adapter.try_set_rx_enabled(true).unwrap();
                // Actual atomic RX carries the authenticated raw request to the Coordinator.
                ingress
                    .rx_publish(
                        link,
                        &request.bytes[..request.length],
                        RxMeta {
                            timestamp_us: 21,
                            sender_discriminator: 1,
                        },
                    )
                    .unwrap();
                let mut received = Packet {
                    bytes: [0; 128],
                    length: 0,
                };
                let rx = adapter.rx_claim(&mut received.bytes).unwrap();
                received.length = rx.frame_bytes;
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 2, 8).unwrap();
                let command = fixture::command(
                    receive(&mut q, &mut r, server, sc, sh, sf, &received).unwrap(),
                );
                adapter.rx_retire(rx.token).unwrap();
                let mut c = Coordinator::new(30, 1000).unwrap();
                let h = pending(
                    dispatch(&mut c, &mut q, command, server, sh, sf, &mut op, p, &mut r).unwrap(),
                );
                assert_eq!(op.view(), (Some(OperationPhase::Prepared), true, false));
                assert!(op.receipt(&mut DigestWorkspace::new()).is_err());
                assert_eq!(
                    finish(&mut c, h, &mut q, &mut op, p, server, sh, sf, &mut r),
                    Err(Error::State)
                );
                assert_eq!(
                    receive(&mut q, &mut r, server, sc, sh, sf, &request).unwrap(),
                    ControlAdmission::Pending(command)
                );
                assert_eq!(
                    pending(
                        dispatch(&mut c, &mut q, command, server, sh, sf, &mut op, p, &mut r)
                            .unwrap()
                    ),
                    h
                );
                let other = DurableContract {
                    domain: DomainKey {
                        kind: DomainKind::OperationJournal,
                        id: 6,
                    },
                    ..contract()
                };
                flash::exercise(other, |other_p, other_f| {
                    let mut wrong_owner = Journal::new(other).unwrap();
                    wrong_owner
                        .begin(
                            key(),
                            flash::base(other_p, other),
                            11,
                            1000,
                            1,
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                    let calls = other_f.calls;
                    assert_eq!(
                        c.submit(h, &mut wrong_owner, other_p, 22),
                        Err(Error::State)
                    );
                    assert!(!other_p.domain_get(other.domain).unwrap().pending);
                    assert_eq!(other_f.calls, calls);
                    assert_eq!(c.view(h).unwrap(), (false, None));
                });
                assert_eq!(c.submit(h, &mut op, p, 20), Err(Error::State));
                let io = c.submit(h, &mut op, p, 22).unwrap();
                assert_eq!(
                    finish(
                        &mut c,
                        h,
                        &mut q,
                        &mut op,
                        p,
                        server,
                        sh,
                        CurrentFacts { now_us: 22, ..sf },
                        &mut r
                    ),
                    Err(Error::State)
                );
                f.pending_once = true;
                drive_proof(p, f, io, 23);
                let response = finish(
                    &mut c,
                    h,
                    &mut q,
                    &mut op,
                    p,
                    server,
                    sh,
                    CurrentFacts { now_us: 24, ..sf },
                    &mut r,
                )
                .unwrap();
                assert_eq!(c.view(h), Err(Error::State));
                assert_eq!(
                    op.view(),
                    (Some(OperationPhase::AbortedNoEffect), false, false)
                );
                assert_eq!(q.peek_next(), Err(Error::NotFound));
                assert!(op.handoff(key()).is_err());
                let mut counter = Counter(10);
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                let first = emit(
                    &mut r,
                    response,
                    server,
                    sc,
                    sh,
                    CurrentFacts { now_us: 25, ..sf },
                    &mut counter,
                    &mut adapter,
                    &mut driver,
                    link,
                );
                // First response is lost; repeated cancel queries its retained identical reply.
                assert_eq!(
                    receive(
                        &mut q,
                        &mut r,
                        server,
                        sc,
                        sh,
                        CurrentFacts { now_us: 27, ..sf },
                        &request
                    )
                    .unwrap(),
                    ControlAdmission::CachedReply(response)
                );
                let repeated = emit(
                    &mut r,
                    response,
                    server,
                    sc,
                    sh,
                    CurrentFacts { now_us: 27, ..sf },
                    &mut counter,
                    &mut adapter,
                    &mut driver,
                    link,
                );
                assert_eq!(first.length, repeated.length);
                assert_eq!(first.bytes, repeated.bytes);
                assert_eq!(counter.0, 11);
                let mut client_queue = Queue::new(50).unwrap();
                let mut unused = Feedback::new(51, 1, 2).unwrap();
                let rh = fixture::command(
                    receive(
                        &mut client_queue,
                        &mut unused,
                        client,
                        cc,
                        ch,
                        CurrentFacts { now_us: 28, ..cf },
                        &repeated,
                    )
                    .unwrap(),
                );
                let mut watch = OperationWatch::new(key(), 21, 1000).unwrap();
                client_queue
                    .dispatch_operation_receipt(
                        rh,
                        &mut watch,
                        client,
                        ch,
                        CurrentFacts { now_us: 28, ..cf },
                    )
                    .unwrap();
                assert_eq!(watch.receipt().unwrap().phase, 4);
                watch
                    .verify_result(Error::Cancelled.code(), &[], &mut DigestWorkspace::new())
                    .unwrap();
                assert_eq!(
                    watch.verify_result(0, &[], &mut DigestWorkspace::new()),
                    Err(Error::Security)
                );
            });
        });
    });
}

#[test]
fn executing_and_restored_executing_cancel_never_claim_unobserved_external_effects() {
    for restored in [false, true] {
        security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
            security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
                flash::exercise(contract(), |p, f| {
                    let mut op = prepared(p, f, key());
                    op.transition(
                        OperationPhase::Executing,
                        flash::base(p, contract()),
                        0,
                        &[],
                        13,
                        1000,
                        2,
                        &mut CodecWorkspace::new(),
                    )
                    .unwrap();
                    persist(&mut op, p, f, 14);
                    if restored {
                        let mut body = [0; 256];
                        let n = p.copy_body(contract().domain, &mut body).unwrap();
                        let mut recovered = Journal::new(contract()).unwrap();
                        recovered
                            .restore(
                                flash::base(p, contract()),
                                &body[..n],
                                key().service.security,
                                &mut CodecWorkspace::new(),
                            )
                            .unwrap();
                        op = recovered;
                    } else {
                        let _permit = op.handoff(key()).unwrap();
                    }
                    assert!(op.handoff(key()).is_err());
                    let request = protect(
                        client,
                        cc,
                        ch,
                        cf,
                        ProtocolOpcode::OperationCancel,
                        &encode_cancel(key()),
                        1,
                    );
                    let mut q = Queue::new(20).unwrap();
                    let mut r = Feedback::new(21, 1, 4).unwrap();
                    let command = fixture::command(
                        receive(&mut q, &mut r, server, sc, sh, sf, &request).unwrap(),
                    );
                    let mut c = Coordinator::new(30, 1000).unwrap();
                    let h = pending(
                        dispatch(&mut c, &mut q, command, server, sh, sf, &mut op, p, &mut r)
                            .unwrap(),
                    );
                    let io = c.submit(h, &mut op, p, 22).unwrap();
                    drive_proof(p, f, io, 23);
                    finish(
                        &mut c,
                        h,
                        &mut q,
                        &mut op,
                        p,
                        server,
                        sh,
                        CurrentFacts { now_us: 24, ..sf },
                        &mut r,
                    )
                    .unwrap();
                    assert_eq!(op.view(), (Some(OperationPhase::InDoubt), false, true));
                    assert_eq!(op.receipt(&mut DigestWorkspace::new()).unwrap().phase, 5);
                    assert!(op.handoff(key()).is_err());
                });
            });
        });
    }
}

#[test]
#[allow(clippy::too_many_lines)] // Explicit independent Foundation/security capacity fixtures.
fn full_identity_capacity_and_nonterminal_query_are_zero_io_and_preserve_commands() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = prepared(p, f, key());
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let mut c = Coordinator::new(30, 1000).unwrap();
                let mut bytes = [0; 28];
                encode_operation_key(wire(key()), &mut bytes).unwrap();
                let query = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationQuery,
                    &bytes,
                    1,
                );
                let command =
                    fixture::command(receive(&mut q, &mut r, server, sc, sh, sf, &query).unwrap());
                let calls = f.calls;
                assert!(
                    dispatch(&mut c, &mut q, command, server, sh, sf, &mut op, p, &mut r).is_err()
                );
                assert_eq!(f.calls, calls);
                assert_eq!(op.view(), (Some(OperationPhase::Prepared), false, false));
                q.ack(command).unwrap();
                for (i, wrong) in [
                    OperationKey {
                        request_digest: [4; 16],
                        ..key()
                    },
                    OperationKey {
                        service: ServiceKey {
                            opcode: 10,
                            ..key().service
                        },
                        ..key()
                    },
                ]
                .into_iter()
                .enumerate()
                {
                    let packet = protect(
                        client,
                        cc,
                        ch,
                        cf,
                        ProtocolOpcode::OperationCancel,
                        &encode_cancel(wrong),
                        u64::try_from(i + 2).unwrap(),
                    );
                    let h = fixture::command(
                        receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap(),
                    );
                    assert_eq!(
                        dispatch(&mut c, &mut q, h, server, sh, sf, &mut op, p, &mut r),
                        Err(Error::Access)
                    );
                    assert_eq!(op.view(), (Some(OperationPhase::Prepared), false, false));
                    assert_eq!(
                        q.view(h).unwrap().body(),
                        ControlBody::OperationCancel(OperationCancel {
                            key: wire(wrong),
                            reason: 1
                        })
                    );
                    q.ack(h).unwrap();
                }
                let request = protect(
                    client,
                    cc,
                    ch,
                    cf,
                    ProtocolOpcode::OperationCancel,
                    &encode_cancel(key()),
                    4,
                );
                let h = fixture::command(
                    receive(&mut q, &mut r, server, sc, sh, sf, &request).unwrap(),
                );
                assert_eq!(
                    dispatch(
                        &mut c,
                        &mut q,
                        h,
                        server,
                        sh,
                        CurrentFacts {
                            policy_generation: 2,
                            ..sf
                        },
                        &mut op,
                        p,
                        &mut r
                    ),
                    Err(Error::State)
                );
                assert_eq!(op.view(), (Some(OperationPhase::Prepared), false, false));
                let mut full = ControlReplies::<1, 128>::new(55, 1, 3).unwrap();
                // Separate terminal Journal fills the shared reply capacity, not a fake occupied flag.
                flash::exercise(contract(), |p2, f2| {
                    let mut terminal = committed(p2, f2);
                    q.ack(h).unwrap();
                    let mut unsolicited = [0; 48];
                    encode_operation_receipt(
                        terminal.receipt(&mut DigestWorkspace::new()).unwrap(),
                        &mut unsolicited,
                    )
                    .unwrap();
                    let packet = protect(
                        client,
                        cc,
                        ch,
                        cf,
                        ProtocolOpcode::OperationReceipt,
                        &unsolicited,
                        5,
                    );
                    let unsolicited_handle = fixture::command(
                        receive(&mut q, &mut full, server, sc, sh, sf, &packet).unwrap(),
                    );
                    assert_eq!(
                        dispatch(
                            &mut c,
                            &mut q,
                            unsolicited_handle,
                            server,
                            sh,
                            sf,
                            &mut terminal,
                            p2,
                            &mut full
                        ),
                        Err(Error::Unsupported)
                    );
                    assert_eq!(
                        terminal.view(),
                        (Some(OperationPhase::CommittedResult), false, true)
                    );
                    q.ack(unsolicited_handle).unwrap();
                    let h2 = fixture::command(
                        receive(
                            &mut q,
                            &mut full,
                            server,
                            sc,
                            sh,
                            sf,
                            &protect(
                                client,
                                cc,
                                ch,
                                cf,
                                ProtocolOpcode::OperationQuery,
                                &bytes,
                                6,
                            ),
                        )
                        .unwrap(),
                    );
                    let _retained = reply(
                        dispatch(
                            &mut c,
                            &mut q,
                            h2,
                            server,
                            sh,
                            sf,
                            &mut terminal,
                            p2,
                            &mut full,
                        )
                        .unwrap(),
                    );
                    let h3 = fixture::command(
                        receive(
                            &mut q,
                            &mut full,
                            server,
                            sc,
                            sh,
                            sf,
                            &protect(
                                client,
                                cc,
                                ch,
                                cf,
                                ProtocolOpcode::OperationCancel,
                                &encode_cancel(key()),
                                7,
                            ),
                        )
                        .unwrap(),
                    );
                    assert_eq!(
                        dispatch(&mut c, &mut q, h3, server, sh, sf, &mut op, p, &mut full),
                        Err(Error::NoSpace)
                    );
                    assert_eq!(op.view(), (Some(OperationPhase::Prepared), false, false));
                    assert_eq!(f.calls, calls);
                });
            });
        });
    });
}

#[test]
fn late_proof_and_revocation_keep_blocked_reply_until_explicit_confirmed_rejection() {
    for expired in [false, true] {
        security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
            security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
                flash::exercise(contract(), |p, f| {
                    let mut op = prepared(p, f, key());
                    let mut q = Queue::new(20).unwrap();
                    let mut r = Feedback::new(21, 1, 4).unwrap();
                    let packet = protect(
                        client,
                        cc,
                        ch,
                        cf,
                        ProtocolOpcode::OperationCancel,
                        &encode_cancel(key()),
                        1,
                    );
                    let command = fixture::command(
                        receive(&mut q, &mut r, server, sc, sh, sf, &packet).unwrap(),
                    );
                    let mut c = Coordinator::new(30, 10).unwrap();
                    let h = pending(
                        dispatch(&mut c, &mut q, command, server, sh, sf, &mut op, p, &mut r)
                            .unwrap(),
                    );
                    assert_eq!(c.reject_confirmed(h, &mut q, &mut r), Err(Error::State));
                    let io = c.submit(h, &mut op, p, 22).unwrap();
                    c.stop(); // Stop cannot erase the outstanding Journal continuation.
                    drive_proof(p, f, io, 23);
                    let facts = if expired {
                        CurrentFacts { now_us: 31, ..sf }
                    } else {
                        CurrentFacts {
                            now_us: 24,
                            policy_generation: 2,
                            ..sf
                        }
                    };
                    assert_eq!(
                        finish(&mut c, h, &mut q, &mut op, p, server, sh, facts, &mut r),
                        Err(if expired {
                            Error::Timeout
                        } else {
                            Error::State
                        })
                    );
                    assert_eq!(c.view(h).unwrap(), (true, None));
                    assert_eq!(
                        op.view(),
                        (Some(OperationPhase::AbortedNoEffect), false, false)
                    );
                    assert_eq!(
                        receive(
                            &mut q,
                            &mut r,
                            server,
                            sc,
                            sh,
                            CurrentFacts {
                                now_us: facts.now_us,
                                ..sf
                            },
                            &packet
                        )
                        .unwrap(),
                        ControlAdmission::Pending(command)
                    );
                    c.reject_confirmed(h, &mut q, &mut r).unwrap();
                    assert_eq!(c.view(h), Err(Error::State));
                    assert_eq!(q.peek_next(), Err(Error::NotFound));
                });
            });
        });
    }
}

#[test]
#[allow(clippy::too_many_lines)] // Explicit raw receipt conflicts, lifetime and domain checks.
fn query_observation_does_not_deliver_result_and_conflicting_or_expired_receipts_do_not_rewrite() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let mut op = committed(p, f);
                let mut body = [0; 28];
                encode_operation_key(wire(key()), &mut body).unwrap();
                let mut q = Queue::new(20).unwrap();
                let mut r = Feedback::new(21, 1, 4).unwrap();
                let request = protect(client, cc, ch, cf, ProtocolOpcode::OperationQuery, &body, 1);
                let command = fixture::command(
                    receive(&mut q, &mut r, server, sc, sh, sf, &request).unwrap(),
                );
                let mut c = Coordinator::new(30, 1000).unwrap();
                let reply = reply(
                    dispatch(&mut c, &mut q, command, server, sh, sf, &mut op, p, &mut r).unwrap(),
                );
                let calls = f.calls;
                let ingress = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                let packet = emit(
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
                let mut client_queue = Queue::new(50).unwrap();
                let mut spare = Feedback::new(51, 1, 4).unwrap();
                let h = fixture::command(
                    receive(
                        &mut client_queue,
                        &mut spare,
                        client,
                        cc,
                        ch,
                        CurrentFacts { now_us: 22, ..cf },
                        &packet,
                    )
                    .unwrap(),
                );
                let mut watch = OperationWatch::new(key(), 21, 100).unwrap();
                client_queue
                    .dispatch_operation_receipt(
                        h,
                        &mut watch,
                        client,
                        ch,
                        CurrentFacts { now_us: 22, ..cf },
                    )
                    .unwrap();
                let observed = watch.receipt().unwrap();
                assert_eq!(observed.phase, 3);
                watch
                    .verify_result(7, b"ok", &mut DigestWorkspace::new())
                    .unwrap();
                assert_eq!(
                    watch.verify_result(7, b"bad", &mut DigestWorkspace::new()),
                    Err(Error::Security)
                );
                assert_eq!(f.calls, calls);
                let mut bytes = [0; 48];
                encode_operation_receipt(
                    OperationReceipt {
                        result_digest: [9; 16],
                        ..observed
                    },
                    &mut bytes,
                )
                .unwrap();
                let conflicting = protect(
                    server,
                    sc,
                    sh,
                    sf,
                    ProtocolOpcode::OperationReceipt,
                    &bytes,
                    11,
                );
                let h = fixture::command(
                    receive(
                        &mut client_queue,
                        &mut spare,
                        client,
                        cc,
                        ch,
                        CurrentFacts { now_us: 23, ..cf },
                        &conflicting,
                    )
                    .unwrap(),
                );
                assert_eq!(
                    client_queue.dispatch_operation_receipt(
                        h,
                        &mut watch,
                        client,
                        ch,
                        CurrentFacts { now_us: 23, ..cf }
                    ),
                    Err(Error::Replay)
                );
                assert_eq!(watch.receipt(), Some(observed));
                let mut short = OperationWatch::new(key(), 21, 23).unwrap();
                assert_eq!(
                    client_queue.dispatch_operation_receipt(
                        h,
                        &mut short,
                        client,
                        ch,
                        CurrentFacts { now_us: 23, ..cf }
                    ),
                    Err(Error::Timeout)
                );
                assert_eq!(short.receipt(), None);
                let mut wrong = OperationWatch::new(
                    OperationKey {
                        service: ServiceKey {
                            client_generation: 2,
                            ..key().service
                        },
                        ..key()
                    },
                    21,
                    100,
                )
                .unwrap();
                assert_eq!(
                    client_queue.dispatch_operation_receipt(
                        h,
                        &mut wrong,
                        client,
                        ch,
                        CurrentFacts { now_us: 23, ..cf }
                    ),
                    Err(Error::Access)
                );
                assert_eq!(wrong.receipt(), None);
            });
        });
    });
}
