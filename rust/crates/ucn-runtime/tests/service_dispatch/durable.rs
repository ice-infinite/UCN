//! Actual authenticated durable RPC and Foundation/executor failures; software only.
use super::*;
use fixture::security::flash;
use ucn_digest::DigestWorkspace;
use ucn_durable::DurableContract;
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
type Coordinator = DurableRpcCoordinator<16, 16>;
fn contract(id: bool) -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::OperationJournal,
            id: if id { 6 } else { 5 },
        },
        schema: if id { 2 } else { 1 },
        version: 1,
        operation: if id { 0x0702 } else { 0x0701 },
    }
}
fn durable_bridge(peer: u8, server: bool) -> Rpc {
    Rpc::new(
        u32::from(peer) + 10,
        endpoint(peer, server, !server).durable(),
        10,
        4,
    )
    .unwrap()
}
struct Executor<'a> {
    calls: usize,
    gate: &'a CallbackGate,
    status: i32,
    outcome: Result<usize>,
    nested_blocked: bool,
}
impl DurableExecutor for Executor<'_> {
    fn execute(&mut self, key: OperationKey, args: &[u8], out: &mut [u8]) -> Result<(i32, usize)> {
        assert_eq!(key.service.opcode, 0);
        assert_eq!(args, b"request");
        self.calls += 1;
        self.nested_blocked = self
            .gate
            .try_enter(CallbackClaim::new(99, 1, 1, 1).unwrap())
            .is_err();
        out[..4].copy_from_slice(b"done");
        self.outcome.map(|n| (self.status, n))
    }
}
fn finish(
    c: &mut Coordinator,
    h: DurableRpcHandle,
    op: &mut OperationOwner<16>,
    p: &mut flash::Owner<'_>,
    f: &mut flash::Flash,
    now: u64,
) {
    c.submit(h, op, p, now).unwrap();
    assert!(c.finish_write(h, op, p, now).is_err());
    for _ in 0..128 {
        p.step(f, now, 1).unwrap();
        if c.finish_write(h, op, p, now).is_ok() {
            return;
        }
    }
    panic!("Journal did not converge");
}
#[allow(clippy::too_many_arguments)]
fn prepare(
    c: &mut Coordinator,
    rpc: &Rpc,
    rh: RpcHandle,
    arguments: &[u8],
    op: &mut OperationOwner<16>,
    p: &flash::Owner<'_>,
    sec: &SecurityOwner<'_, 2, 2>,
    sh: SessionHandle,
    sf: CurrentFacts,
) -> Result<DurableRpcHandle> {
    c.prepare(
        rpc,
        rh,
        arguments,
        op,
        p,
        sec,
        sh,
        sf,
        &mut CodecWorkspace::new(),
        &mut DigestWorkspace::new(),
    )
}
#[allow(clippy::too_many_arguments)]
fn arm(
    c: &mut Coordinator,
    h: DurableRpcHandle,
    rpc: &Rpc,
    op: &mut OperationOwner<16>,
    p: &flash::Owner<'_>,
    sec: &SecurityOwner<'_, 2, 2>,
    sh: SessionHandle,
    sf: CurrentFacts,
) -> Result<()> {
    c.arm(h, rpc, op, p, sec, sh, sf, &mut CodecWorkspace::new())
}
#[test]
#[allow(clippy::too_many_lines)] // Full actual Source-ID -> raw Data -> three reloads -> result path.
fn durable_id_raw_request_two_reload_gates_one_executor_and_final_raw_result() {
    let recovery_ops = &[
        ProtocolOpcode::OperationResume,
        ProtocolOpcode::OperationResult,
        ProtocolOpcode::OperationResultAck,
    ];
    security::exercise_opcodes(1, 2, recovery_ops, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, recovery_ops, |server, sh, sf, sc| {
            flash::exercise(contract(true), |ip, iff| {
                let mut ids = OperationIds::new(contract(true), 1).unwrap();
                assert!(ids.reserve_id().is_err());
                ids.reserve(
                    flash::base(ip, contract(true)),
                    4,
                    1,
                    1000,
                    1,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
                let (io, proof) = flash::commit(ip, iff, &ids.requirement().unwrap());
                ids.bind(io).unwrap();
                ids.confirm(io, proof).unwrap();
                ip.proof_retire(io).unwrap();
                assert_eq!(ids.allocate().unwrap(), 1);
                let reservation = ids.reserve_id().unwrap();
                assert_eq!(reservation.id(), 2);
                drop(reservation);
                let ci = AdapterEventIngress::<1, 1, 1, 128>::new(1).unwrap();
                let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                let mut ca = AdapterOwner::new(1, &ci).unwrap();
                let mut sa = AdapterOwner::new(2, &si).unwrap();
                let cl = ca.open_link(0, 1, 128).unwrap();
                let sl = sa.open_link(0, 1, 128).unwrap();
                sa.try_set_rx_enabled(true).unwrap();
                let mut client_rpc = durable_bridge(2, false);
                let mut server_rpc = durable_bridge(1, true);
                let mut cs = Service::new(1, 100).unwrap();
                let mut ss = Service::new(2, 100).unwrap();
                let mut seq = SequenceCounter(10);
                assert_eq!(
                    client_rpc.begin_durable_request(
                        &mut cs,
                        client,
                        cc,
                        &mut seq,
                        &mut ids,
                        ch,
                        cf,
                        Fingerprint::new([5; 16]).unwrap(),
                        cl,
                        &ca,
                        &[0; 17],
                        121,
                        &mut SecurityPacketWorkspace::<128>::new()
                    ),
                    Err(Error::Argument)
                );
                assert_eq!(ids.reserve_id().unwrap().id(), 2);
                assert_eq!(seq.0, 10);
                let source = client_rpc
                    .begin_durable_request(
                        &mut cs,
                        client,
                        cc,
                        &mut seq,
                        &mut ids,
                        ch,
                        cf,
                        Fingerprint::new([5; 16]).unwrap(),
                        cl,
                        &ca,
                        b"request",
                        121,
                        &mut SecurityPacketWorkspace::<128>::new(),
                    )
                    .unwrap();
                assert_eq!(ids.allocate().unwrap(), 3);
                assert_eq!(seq.0, 11);
                assert_eq!(
                    cs.view(client_rpc.view(source).unwrap().service)
                        .unwrap()
                        .key
                        .operation,
                    2
                );
                let p = packet(&client_rpc, source, cf);
                assert_eq!(p.bytes[21], 2);
                assert_eq!(p.sequence, 10);
                let mut d = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                client_rpc
                    .submit(source, &mut cs, client, ch, cf, &mut ca, &mut d)
                    .unwrap();
                client_rpc.retire_attempt(source, &mut ca).unwrap();
                si.rx_publish(
                    sl,
                    &d.frames[0],
                    RxMeta {
                        timestamp_us: sf.now_us,
                        sender_discriminator: 1,
                    },
                )
                .unwrap();
                let mut raw = [0; 128];
                let rx = sa.rx_claim(&mut raw).unwrap();
                let incoming = FragmentPacket {
                    bytes: raw,
                    length: rx.frame_bytes,
                    identity: p.identity,
                    sequence: p.sequence,
                };
                let mut args = [0xa5; 16];
                let accepted = request(
                    &mut server_rpc,
                    &mut ss,
                    server,
                    sc,
                    sh,
                    sf,
                    &incoming,
                    sl,
                    &sa,
                    &mut args,
                )
                .unwrap();
                sa.rx_retire(rx.token).unwrap();
                assert_eq!(accepted.action, RpcAction::Persist);
                assert_eq!(
                    server_rpc.respond(
                        accepted.handle,
                        &mut ss,
                        server,
                        sh,
                        sf,
                        InteractionRole::Result,
                        7,
                        b"fake"
                    ),
                    Err(Error::Unsupported)
                );
                flash::exercise(contract(false), |jp, jf| {
                    let mut op = OperationOwner::<16>::new(contract(false)).unwrap();
                    let mut c = Coordinator::new(50, 1000).unwrap();
                    let gate = CallbackGate::new(7).unwrap();
                    let mut executor = Executor {
                        calls: 0,
                        gate: &gate,
                        status: 7,
                        outcome: Ok(4),
                        nested_blocked: false,
                    };
                    assert_eq!(
                        prepare(
                            &mut c,
                            &server_rpc,
                            accepted.handle,
                            b"changed",
                            &mut op,
                            jp,
                            server,
                            sh,
                            sf
                        ),
                        Err(Error::Security)
                    );
                    assert!(op.key().is_none());
                    assert_eq!(jf.witness, 0);
                    let h = prepare(
                        &mut c,
                        &server_rpc,
                        accepted.handle,
                        &args[..accepted.bytes],
                        &mut op,
                        jp,
                        server,
                        sh,
                        sf,
                    )
                    .unwrap();
                    assert_eq!(c.view(h).unwrap(), DurableRpcPhase::PreparedPending);
                    assert!(
                        c.execute(
                            h,
                            &server_rpc,
                            &mut op,
                            jp,
                            server,
                            sh,
                            sf,
                            &gate,
                            &mut executor
                        )
                        .is_err()
                    );
                    assert_eq!(executor.calls, 0);
                    assert!(
                        c.publish(h, &mut server_rpc, &op, jp, &mut ss, server, sh, sf)
                            .is_err()
                    );
                    finish(&mut c, h, &mut op, jp, jf, 22);
                    assert_eq!(jf.witness, 1);
                    let sf23 = CurrentFacts { now_us: 23, ..sf };
                    arm(&mut c, h, &server_rpc, &mut op, jp, server, sh, sf23).unwrap();
                    assert!(
                        c.execute(
                            h,
                            &server_rpc,
                            &mut op,
                            jp,
                            server,
                            sh,
                            sf23,
                            &gate,
                            &mut executor
                        )
                        .is_err()
                    );
                    finish(&mut c, h, &mut op, jp, jf, 24);
                    assert_eq!(jf.witness, 2);
                    let sf25 = CurrentFacts { now_us: 25, ..sf };
                    let lease = gate
                        .try_enter(CallbackClaim::new(9, 1, 1, 1).unwrap())
                        .unwrap();
                    assert_eq!(
                        c.execute(
                            h,
                            &server_rpc,
                            &mut op,
                            jp,
                            server,
                            sh,
                            sf25,
                            &gate,
                            &mut executor
                        ),
                        Err(Error::State)
                    );
                    assert_eq!(executor.calls, 0);
                    assert!(!op.view().2);
                    gate.leave(lease).unwrap();
                    c.execute(
                        h,
                        &server_rpc,
                        &mut op,
                        jp,
                        server,
                        sh,
                        sf25,
                        &gate,
                        &mut executor,
                    )
                    .unwrap();
                    assert_eq!(executor.calls, 1);
                    assert!(executor.nested_blocked);
                    assert_eq!(
                        c.execute(
                            h,
                            &server_rpc,
                            &mut op,
                            jp,
                            server,
                            sh,
                            sf25,
                            &gate,
                            &mut executor
                        ),
                        Err(Error::State)
                    );
                    assert!(
                        c.publish(h, &mut server_rpc, &op, jp, &mut ss, server, sh, sf25)
                            .is_err()
                    );
                    c.persist_outcome(h, &mut op, jp, 26, &mut CodecWorkspace::new())
                        .unwrap();
                    assert!(op.copy_result(&mut args).is_err());
                    finish(&mut c, h, &mut op, jp, jf, 27);
                    assert_eq!(jf.witness, 3);
                    let sf28 = CurrentFacts { now_us: 28, ..sf };
                    c.publish(h, &mut server_rpc, &op, jp, &mut ss, server, sh, sf28)
                        .unwrap();
                    let mut response_seq = SequenceCounter(20);
                    seal(
                        &mut server_rpc,
                        accepted.handle,
                        &ss,
                        server,
                        sc,
                        &mut response_seq,
                        sh,
                        sf28,
                    )
                    .unwrap();
                    let reply = packet(&server_rpc, accepted.handle, sf28);
                    assert_eq!(reply.bytes[21], 6);
                    let mut sd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                    server_rpc
                        .submit(accepted.handle, &mut ss, server, sh, sf28, &mut sa, &mut sd)
                        .unwrap();
                    server_rpc.retire_attempt(accepted.handle, &mut sa).unwrap();
                    assert!(
                        result(
                            &mut client_rpc,
                            source,
                            &mut cs,
                            client,
                            cc,
                            ch,
                            CurrentFacts { now_us: 29, ..cf },
                            &reply,
                            cl
                        )
                        .unwrap()
                    );
                    let mut body = [0; 16];
                    assert_eq!(
                        cs.copy_result(client_rpc.view(source).unwrap().service, &mut body)
                            .unwrap(),
                        4
                    );
                    assert_eq!(&body[..4], b"done");
                    assert_eq!(
                        client_rpc.view(source).unwrap().result_kind,
                        Some(RpcResultKind::Business)
                    );
                    assert_eq!(
                        request(
                            &mut server_rpc,
                            &mut ss,
                            server,
                            sc,
                            sh,
                            sf28,
                            &p,
                            sl,
                            &sa,
                            &mut args
                        )
                        .unwrap()
                        .action,
                        RpcAction::Replay
                    );
                    assert_eq!(executor.calls, 1);
                    c.retire(h, &op, 29).unwrap();
                    assert_eq!(op.view().0, Some(OperationPhase::CommittedResult)); // No Journal GC on send success.
                    #[cfg(feature = "operation-control")]
                    {
                        recover_ack_gc(&mut op, jp, jf, client, ch, cf, cc, server, sh, sf, sc);
                        assert_eq!(executor.calls, 1);
                    }
                });
            });
        });
    });
}

#[cfg(feature = "operation-control")]
#[allow(
    clippy::too_many_arguments,
    clippy::too_many_lines,
    clippy::many_single_char_names
)] // Both real contexts and explicit Data-to-GC test, not production code.
fn recover_ack_gc(
    op: &mut OperationOwner<16>,
    p: &mut flash::Owner<'_>,
    f: &mut flash::Flash,
    client: &mut SecurityOwner<'_, 2, 2>,
    ch: SessionHandle,
    cf: CurrentFacts,
    cc: &mut security::Crypto,
    server: &mut SecurityOwner<'_, 2, 2>,
    sh: SessionHandle,
    sf: CurrentFacts,
    sc: &mut security::Crypto,
) {
    let key = op.key().unwrap();
    assert_eq!(key.service.opcode, 0);
    assert_eq!(key.service.operation, 2);
    let cf = CurrentFacts { now_us: 30, ..cf };
    let sf = CurrentFacts { now_us: 30, ..sf };
    let wire = OperationWireKey {
        operation: key.service.operation,
        service: key.service.service,
        opcode: 0,
        request_digest: key.request_digest,
    };
    let mut body = [0; 28];
    encode_operation_key(wire, &mut body).unwrap();
    let raw = fixture::protect(
        client,
        cc,
        ch,
        cf,
        ProtocolOpcode::OperationResume,
        &body,
        1,
    );
    let mut q = fixture::Queue::new(70).unwrap();
    let mut r = fixture::Replies::new(71, 1, 4).unwrap();
    let h = fixture::command(fixture::receive(&mut q, &mut r, server, sc, sh, sf, &raw).unwrap());
    let reply = q
        .dispatch_operation_resume(
            h,
            op,
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
    let ingress = AdapterEventIngress::<1, 1, 1, 128>::new(3).unwrap();
    let mut adapter = AdapterOwner::new(3, &ingress).unwrap();
    let link = adapter.open_link(0, 1, 128).unwrap();
    let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
    let response = fixture::emit(
        &mut r,
        reply,
        server,
        sc,
        sh,
        sf,
        &mut fixture::Counter(10),
        &mut adapter,
        &mut driver,
        link,
    );
    let mut cq = fixture::Queue::new(72).unwrap();
    let mut cr = fixture::Replies::new(73, 1, 4).unwrap();
    let h = fixture::command(
        fixture::receive(&mut cq, &mut cr, client, cc, ch, cf, &response).unwrap(),
    );
    let mut recovery = OperationRecovery::new(key, 30, 100).unwrap();
    let mut out = [0; 16];
    let result = cq
        .dispatch_recovered_result(
            h,
            &mut recovery,
            client,
            ch,
            cf,
            &mut cr,
            &mut out,
            &mut DigestWorkspace::new(),
        )
        .unwrap();
    assert_eq!(&out[..result.bytes], b"done");
    let ack = fixture::emit(
        &mut cr,
        result.ack.unwrap(),
        client,
        cc,
        ch,
        cf,
        &mut fixture::Counter(2),
        &mut adapter,
        &mut driver,
        link,
    );
    let h = fixture::command(fixture::receive(&mut q, &mut r, server, sc, sh, sf, &ack).unwrap());
    q.dispatch_result_ack(
        h,
        op,
        p,
        server,
        sh,
        sf,
        1,
        &mut DigestWorkspace::new(),
        &mut CodecWorkspace::new(),
    )
    .unwrap();
    let mut gc = OperationGc::new();
    let io = gc
        .begin(op, p, 31, 100, 8, &mut CodecWorkspace::new())
        .unwrap();
    for _ in 0..128 {
        p.step(f, 32, 1).unwrap();
        if p.proof_get(io).is_ok() {
            break;
        }
    }
    gc.finish(op, p).unwrap();
    assert_eq!(op.view().0, Some(OperationPhase::Tombstoned));
    assert!(op.handoff(key).is_err());
}

#[test]
#[allow(clippy::too_many_lines)] // Current authorization before both handoffs; no-effect persists after timeout.
fn expired_and_revoked_work_never_executes_and_no_effect_rejection_still_needs_reload() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        security::exercise(2, 1, |server, sh, sf, sc| {
            let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
            let mut sa = AdapterOwner::new(2, &si).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            let mut rpc = durable_bridge(1, true);
            let mut service = Service::new(2, 100).unwrap();
            let p = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                10,
                1000,
                2,
                0,
                b"request",
            );
            let mut args = [0; 16];
            let got = request(
                &mut rpc,
                &mut service,
                server,
                sc,
                sh,
                sf,
                &p,
                sl,
                &sa,
                &mut args,
            )
            .unwrap();
            assert_eq!(got.action, RpcAction::Persist);
            flash::exercise(contract(false), |jp, jf| {
                let mut op = OperationOwner::<16>::new(contract(false)).unwrap();
                let mut c = Coordinator::new(50, 1000).unwrap();
                let h = prepare(
                    &mut c, &rpc, got.handle, b"request", &mut op, jp, server, sh, sf,
                )
                .unwrap();
                finish(&mut c, h, &mut op, jp, jf, 22);
                assert!(
                    arm(
                        &mut c,
                        h,
                        &rpc,
                        &mut op,
                        jp,
                        server,
                        sh,
                        CurrentFacts {
                            now_us: 23,
                            policy_generation: 2,
                            ..sf
                        }
                    )
                    .is_err()
                );
                assert_eq!(c.view(h).unwrap(), DurableRpcPhase::Prepared);
                arm(
                    &mut c,
                    h,
                    &rpc,
                    &mut op,
                    jp,
                    server,
                    sh,
                    CurrentFacts { now_us: 23, ..sf },
                )
                .unwrap();
                finish(&mut c, h, &mut op, jp, jf, 24);
                let gate = CallbackGate::new(7).unwrap();
                let mut executor = Executor {
                    calls: 0,
                    gate: &gate,
                    status: 0,
                    outcome: Ok(4),
                    nested_blocked: false,
                };
                assert!(
                    c.execute(
                        h,
                        &rpc,
                        &mut op,
                        jp,
                        server,
                        sh,
                        CurrentFacts { now_us: 121, ..sf },
                        &gate,
                        &mut executor
                    )
                    .is_err()
                );
                assert_eq!(executor.calls, 0);
                assert!(!op.view().2);
                c.stop();
                c.abandon_before_execution(h, &op, 121).unwrap();
                c.persist_outcome(h, &mut op, jp, 121, &mut CodecWorkspace::new())
                    .unwrap();
                assert!(op.copy_result(&mut args).is_err());
                finish(&mut c, h, &mut op, jp, jf, 122);
                assert_eq!(
                    op.copy_result(&mut args).unwrap().0,
                    OperationPhase::AbortedNoEffect
                );
                assert!(
                    c.publish(
                        h,
                        &mut rpc,
                        &op,
                        jp,
                        &mut service,
                        server,
                        sh,
                        CurrentFacts { now_us: 122, ..sf }
                    )
                    .is_err()
                );
                c.retire(h, &op, 122).unwrap();
                assert_eq!(jf.witness, 3);
            });
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Known business failure vs uncertain callback and broken capacity contract.
fn known_business_failure_commits_but_unknown_or_oversized_execution_only_becomes_in_doubt() {
    for (outcome, application_status) in [
        (Ok(4), -12),
        (Err(Error::Timeout), -12),
        (Ok(17), -12),
        (Ok(4), -65535),
    ] {
        security::exercise(1, 2, |client, ch, cf, cc| {
            security::exercise(2, 1, |server, sh, sf, sc| {
                let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                let mut sa = AdapterOwner::new(2, &si).unwrap();
                let sl = sa.open_link(0, 1, 128).unwrap();
                let mut rpc = durable_bridge(1, true);
                let mut service = Service::new(2, 100).unwrap();
                let p = custom(
                    client,
                    cc,
                    ch,
                    cf,
                    InteractionRole::Request,
                    10,
                    1000,
                    2,
                    0,
                    b"request",
                );
                let mut args = [0; 16];
                let got = request(
                    &mut rpc,
                    &mut service,
                    server,
                    sc,
                    sh,
                    sf,
                    &p,
                    sl,
                    &sa,
                    &mut args,
                )
                .unwrap();
                flash::exercise(contract(false), |jp, jf| {
                    let mut op = OperationOwner::<16>::new(contract(false)).unwrap();
                    let mut c = Coordinator::new(50, 1000).unwrap();
                    let h = prepare(
                        &mut c, &rpc, got.handle, b"request", &mut op, jp, server, sh, sf,
                    )
                    .unwrap();
                    finish(&mut c, h, &mut op, jp, jf, 22);
                    let sf23 = CurrentFacts { now_us: 23, ..sf };
                    arm(&mut c, h, &rpc, &mut op, jp, server, sh, sf23).unwrap();
                    finish(&mut c, h, &mut op, jp, jf, 24);
                    let gate = CallbackGate::new(7).unwrap();
                    let mut e = Executor {
                        calls: 0,
                        gate: &gate,
                        status: application_status,
                        outcome,
                        nested_blocked: false,
                    };
                    c.execute(
                        h,
                        &rpc,
                        &mut op,
                        jp,
                        server,
                        sh,
                        CurrentFacts { now_us: 25, ..sf },
                        &gate,
                        &mut e,
                    )
                    .unwrap();
                    assert_eq!(e.calls, 1);
                    c.persist_outcome(h, &mut op, jp, 26, &mut CodecWorkspace::new())
                        .unwrap();
                    finish(&mut c, h, &mut op, jp, jf, 27);
                    let confirmed = op.copy_result(&mut args).unwrap();
                    if outcome == Ok(4) && application_status == -12 {
                        assert_eq!(confirmed, (OperationPhase::CommittedResult, -12, 4));
                    } else {
                        assert_eq!(
                            confirmed,
                            (OperationPhase::InDoubt, Error::InDoubt.code(), 0)
                        );
                    }
                    assert_eq!(
                        c.execute(
                            h,
                            &rpc,
                            &mut op,
                            jp,
                            server,
                            sh,
                            CurrentFacts { now_us: 28, ..sf },
                            &gate,
                            &mut e
                        ),
                        Err(Error::State)
                    );
                    assert_eq!(e.calls, 1);
                    c.publish(
                        h,
                        &mut rpc,
                        &op,
                        jp,
                        &mut service,
                        server,
                        sh,
                        CurrentFacts { now_us: 28, ..sf },
                    )
                    .unwrap();
                    let mut seq = SequenceCounter(20);
                    seal(
                        &mut rpc,
                        got.handle,
                        &service,
                        server,
                        sc,
                        &mut seq,
                        sh,
                        CurrentFacts { now_us: 28, ..sf },
                    )
                    .unwrap();
                    let final_packet = packet(&rpc, got.handle, sf);
                    let code = u16::from_be_bytes(final_packet.bytes[22..24].try_into().unwrap());
                    if outcome == Ok(4) && application_status == -12 {
                        assert_eq!(code, 11);
                    } else {
                        assert_eq!(code, DURABLE_RPC_IN_DOUBT);
                        assert_eq!(final_packet.length, 13 + 11 + 16);
                    }
                });
            });
        });
    }
}

#[test]
#[allow(clippy::too_many_lines)] // Complete raw media recovery, never a synthetic durability boolean.
fn restored_executing_never_hands_off_again_and_restored_prepared_requires_authenticated_arguments()
{
    for executing in [false, true] {
        security::exercise(1, 2, |client, ch, cf, cc| {
            security::exercise(2, 1, |server, sh, sf, sc| {
                let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                let mut sa = AdapterOwner::new(2, &si).unwrap();
                let sl = sa.open_link(0, 1, 128).unwrap();
                let mut rpc = durable_bridge(1, true);
                let mut service = Service::new(2, 100).unwrap();
                let p = custom(
                    client,
                    cc,
                    ch,
                    cf,
                    InteractionRole::Request,
                    10,
                    1000,
                    2,
                    0,
                    b"request",
                );
                let mut args = [0; 16];
                let got = request(
                    &mut rpc,
                    &mut service,
                    server,
                    sc,
                    sh,
                    sf,
                    &p,
                    sl,
                    &sa,
                    &mut args,
                )
                .unwrap();
                flash::exercise(contract(false), |jp, jf| {
                    let mut op = OperationOwner::<16>::new(contract(false)).unwrap();
                    let mut c = Coordinator::new(50, 1000).unwrap();
                    let h = prepare(
                        &mut c, &rpc, got.handle, b"request", &mut op, jp, server, sh, sf,
                    )
                    .unwrap();
                    finish(&mut c, h, &mut op, jp, jf, 22);
                    if executing {
                        arm(
                            &mut c,
                            h,
                            &rpc,
                            &mut op,
                            jp,
                            server,
                            sh,
                            CurrentFacts { now_us: 23, ..sf },
                        )
                        .unwrap();
                        finish(&mut c, h, &mut op, jp, jf, 24);
                    }
                    // Reconstruct the actual Foundation from raw slots and Witness, not from old RAM.
                    flash::reopen(contract(false), jf, |recovered, media| {
                        let mut body = [0; 256];
                        let body_bytes = recovered
                            .copy_body(contract(false).domain, &mut body)
                            .unwrap();
                        let mut restored = OperationOwner::<16>::new(contract(false)).unwrap();
                        restored
                            .restore(
                                flash::base(recovered, contract(false)),
                                &body[..body_bytes],
                                [5; 16],
                                &mut CodecWorkspace::new(),
                            )
                            .unwrap();
                        let mut next = Coordinator::new(51, 1000).unwrap();
                        let sf25 = CurrentFacts { now_us: 25, ..sf };
                        let h = prepare(
                            &mut next,
                            &rpc,
                            got.handle,
                            b"request",
                            &mut restored,
                            recovered,
                            server,
                            sh,
                            sf25,
                        )
                        .unwrap();
                        let gate = CallbackGate::new(7).unwrap();
                        let mut e = Executor {
                            calls: 0,
                            gate: &gate,
                            status: 7,
                            outcome: Ok(4),
                            nested_blocked: false,
                        };
                        if executing {
                            assert_eq!(next.view(h).unwrap(), DurableRpcPhase::Observed);
                            assert!(
                                next.execute(
                                    h,
                                    &rpc,
                                    &mut restored,
                                    recovered,
                                    server,
                                    sh,
                                    sf25,
                                    &gate,
                                    &mut e
                                )
                                .is_err()
                            );
                            next.persist_outcome(
                                h,
                                &mut restored,
                                recovered,
                                26,
                                &mut CodecWorkspace::new(),
                            )
                            .unwrap();
                            finish(&mut next, h, &mut restored, recovered, media, 27);
                            assert_eq!(
                                restored.copy_result(&mut args).unwrap().0,
                                OperationPhase::InDoubt
                            );
                            assert_eq!(e.calls, 0);
                        } else {
                            arm(
                                &mut next,
                                h,
                                &rpc,
                                &mut restored,
                                recovered,
                                server,
                                sh,
                                sf25,
                            )
                            .unwrap();
                            finish(&mut next, h, &mut restored, recovered, media, 26);
                            next.execute(
                                h,
                                &rpc,
                                &mut restored,
                                recovered,
                                server,
                                sh,
                                CurrentFacts { now_us: 27, ..sf },
                                &gate,
                                &mut e,
                            )
                            .unwrap();
                            assert_eq!(e.calls, 1);
                        }
                    });
                });
            });
        });
    }
}

#[test]
#[allow(clippy::too_many_lines)] // Corrupt actual written bytes before Foundation readback; keep all obligations.
fn failed_prepared_readback_fences_without_executor_reply_or_lost_io() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        security::exercise(2, 1, |server, sh, sf, sc| {
            let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
            let mut sa = AdapterOwner::new(2, &si).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            let mut rpc = durable_bridge(1, true);
            let mut service = Service::new(2, 100).unwrap();
            let p = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                10,
                1000,
                2,
                0,
                b"request",
            );
            let mut args = [0; 16];
            let got = request(
                &mut rpc,
                &mut service,
                server,
                sc,
                sh,
                sf,
                &p,
                sl,
                &sa,
                &mut args,
            )
            .unwrap();
            flash::exercise(contract(false), |jp, jf| {
                let mut op = OperationOwner::<16>::new(contract(false)).unwrap();
                let mut c = Coordinator::new(50, 1000).unwrap();
                let h = prepare(
                    &mut c, &rpc, got.handle, b"request", &mut op, jp, server, sh, sf,
                )
                .unwrap();
                let io = c.submit(h, &mut op, jp, 22).unwrap();
                assert_eq!(c.retire_failed(h, &mut op, jp, 22), Err(Error::State));
                assert_eq!(c.retire(h, &op, 22), Err(Error::State));
                let before = jf.slots;
                for _ in 0..8 {
                    jp.step(jf, 22, 1).unwrap();
                    if let Some(slot) = jf.slots.iter().zip(before.iter()).position(|(a, b)| a != b)
                    {
                        jf.slots[slot][0] ^= 1;
                        break;
                    }
                }
                for _ in 0..16 {
                    jp.step(jf, 22, 1).unwrap();
                    if jp.request_get(io).unwrap().state == ucn_persistence::RequestState::Failed {
                        break;
                    }
                }
                assert_eq!(
                    c.retire_failed(h, &mut op, jp, 23).unwrap(),
                    Error::Malformed
                );
                assert_eq!(jf.witness, 0);
                assert_eq!(c.view(h).unwrap(), DurableRpcPhase::Faulted);
                assert!(
                    c.publish(
                        h,
                        &mut rpc,
                        &op,
                        jp,
                        &mut service,
                        server,
                        sh,
                        CurrentFacts { now_us: 24, ..sf }
                    )
                    .is_err()
                );
                assert!(op.copy_result(&mut args).is_err());
                c.retire(h, &op, 24).unwrap();
                assert!(
                    prepare(
                        &mut c,
                        &rpc,
                        got.handle,
                        b"request",
                        &mut op,
                        jp,
                        server,
                        sh,
                        CurrentFacts { now_us: 25, ..sf }
                    )
                    .is_err()
                );
            });
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Recreate ephemeral owners from an actual terminal Journal and authenticated replay.
fn confirmed_journal_rehydrates_expired_reply_without_plaintext_fallback_or_reexecution() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        security::exercise(2, 1, |server, sh, sf, sc| {
            let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
            let mut sa = AdapterOwner::new(2, &si).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            let mut rpc = durable_bridge(1, true);
            let mut service = Service::new(2, 100).unwrap();
            let p = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                10,
                1000,
                2,
                0,
                b"request",
            );
            let mut args = [0; 16];
            let got = request(
                &mut rpc,
                &mut service,
                server,
                sc,
                sh,
                sf,
                &p,
                sl,
                &sa,
                &mut args,
            )
            .unwrap();
            flash::exercise(contract(false), |jp, jf| {
                let mut op = OperationOwner::<16>::new(contract(false)).unwrap();
                let mut c = Coordinator::new(50, 1000).unwrap();
                let h = prepare(
                    &mut c, &rpc, got.handle, b"request", &mut op, jp, server, sh, sf,
                )
                .unwrap();
                finish(&mut c, h, &mut op, jp, jf, 22);
                let sf23 = CurrentFacts { now_us: 23, ..sf };
                arm(&mut c, h, &rpc, &mut op, jp, server, sh, sf23).unwrap();
                finish(&mut c, h, &mut op, jp, jf, 24);
                let gate = CallbackGate::new(7).unwrap();
                let mut e = Executor {
                    calls: 0,
                    gate: &gate,
                    status: 7,
                    outcome: Ok(4),
                    nested_blocked: false,
                };
                c.execute(
                    h,
                    &rpc,
                    &mut op,
                    jp,
                    server,
                    sh,
                    CurrentFacts { now_us: 25, ..sf },
                    &gate,
                    &mut e,
                )
                .unwrap();
                c.persist_outcome(h, &mut op, jp, 26, &mut CodecWorkspace::new())
                    .unwrap();
                finish(&mut c, h, &mut op, jp, jf, 27);
                // Reply was never delivered. Ephemeral receipt and packet expire independently.
                c.retire(h, &op, 121).unwrap();
                service.step(121, 1).unwrap();
                rpc.retire(got.handle, &mut service, 121).unwrap();
                flash::reopen(contract(false), jf, |recovered, media| {
                    let mut body = [0; 256];
                    let body_bytes = recovered
                        .copy_body(contract(false).domain, &mut body)
                        .unwrap();
                    let mut restored = OperationOwner::<16>::new(contract(false)).unwrap();
                    restored
                        .restore(
                            flash::base(recovered, contract(false)),
                            &body[..body_bytes],
                            [5; 16],
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                    let sf122 = CurrentFacts { now_us: 122, ..sf };
                    let mut new_rpc = durable_bridge(1, true);
                    let mut new_service = Service::new(3, 100).unwrap();
                    args.fill(0xa5);
                    let accepted = new_rpc
                        .receive_journaled_request(
                            &mut new_service,
                            &restored,
                            server,
                            sc,
                            sh,
                            sf122,
                            p.plan(),
                            sl,
                            &sa,
                            &mut args,
                            &mut SecurityPacketWorkspace::<128>::new(),
                        )
                        .unwrap();
                    assert_eq!(accepted.action, RpcAction::Persist);
                    assert_eq!(accepted.bytes, 0);
                    assert_eq!(args, [0xa5; 16]);
                    let mut next = Coordinator::new(51, 1000).unwrap();
                    let h = prepare(
                        &mut next,
                        &new_rpc,
                        accepted.handle,
                        &[],
                        &mut restored,
                        recovered,
                        server,
                        sh,
                        sf122,
                    )
                    .unwrap();
                    assert_eq!(next.view(h).unwrap(), DurableRpcPhase::ResultReady);
                    assert!(
                        next.execute(
                            h,
                            &new_rpc,
                            &mut restored,
                            recovered,
                            server,
                            sh,
                            sf122,
                            &gate,
                            &mut e
                        )
                        .is_err()
                    );
                    assert_eq!(e.calls, 1);
                    next.publish(
                        h,
                        &mut new_rpc,
                        &restored,
                        recovered,
                        &mut new_service,
                        server,
                        sh,
                        sf122,
                    )
                    .unwrap();
                    seal(
                        &mut new_rpc,
                        accepted.handle,
                        &new_service,
                        server,
                        sc,
                        &mut SequenceCounter(20),
                        sh,
                        sf122,
                    )
                    .unwrap();
                    assert_eq!(
                        new_service
                            .copy_result(new_rpc.view(accepted.handle).unwrap().service, &mut args)
                            .unwrap(),
                        4
                    );
                    assert_eq!(&args[..4], b"done");
                    assert_eq!(media.witness, 3);
                    // A fresh authenticated operation cannot borrow this old terminal Journal.
                    let wrong = custom(
                        client,
                        cc,
                        ch,
                        cf,
                        InteractionRole::Request,
                        11,
                        1001,
                        2,
                        0,
                        b"request",
                    );
                    assert!(
                        request(
                            &mut new_rpc,
                            &mut new_service,
                            server,
                            sc,
                            sh,
                            sf122,
                            &wrong,
                            sl,
                            &sa,
                            &mut args
                        )
                        .is_err()
                    );
                });
            });
        });
    });
}

#[test]
fn executor_result_capacity_is_checked_before_any_journal_write() {
    security::exercise(1, 2, |client, ch, cf, cc| {
        security::exercise(2, 1, |server, sh, sf, sc| {
            let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
            let mut sa = AdapterOwner::new(2, &si).unwrap();
            let sl = sa.open_link(0, 1, 128).unwrap();
            let mut rpc = durable_bridge(1, true);
            let mut service = Service::new(2, 100).unwrap();
            let p = custom(
                client,
                cc,
                ch,
                cf,
                InteractionRole::Request,
                10,
                1000,
                2,
                0,
                b"request",
            );
            let mut args = [0; 16];
            let got = request(
                &mut rpc,
                &mut service,
                server,
                sc,
                sh,
                sf,
                &p,
                sl,
                &sa,
                &mut args,
            )
            .unwrap();
            flash::exercise(contract(false), |jp, jf| {
                let mut op = OperationOwner::<32>::new(contract(false)).unwrap();
                let mut c = DurableRpcCoordinator::<16, 32>::new(50, 1000).unwrap();
                assert_eq!(
                    c.prepare(
                        &rpc,
                        got.handle,
                        b"request",
                        &mut op,
                        jp,
                        server,
                        sh,
                        sf,
                        &mut CodecWorkspace::new(),
                        &mut DigestWorkspace::new()
                    ),
                    Err(Error::Config)
                );
                assert!(op.key().is_none());
                assert_eq!(jf.witness, 0);
            });
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Real protected incoming outcomes, distinct from a bare enum classifier.
fn client_distinguishes_no_effect_in_doubt_and_business_error_without_accepting_uncertain_payload()
{
    for (code, expected) in [
        (DURABLE_RPC_NO_EFFECT, RpcResultKind::NoEffect),
        (DURABLE_RPC_IN_DOUBT, RpcResultKind::InDoubt),
        (14, RpcResultKind::Business),
    ] {
        security::exercise(1, 2, |client, ch, cf, cc| {
            security::exercise(2, 1, |server, sh, sf, sc| {
                flash::exercise(contract(true), |ip, iff| {
                    let mut ids = OperationIds::new(contract(true), 1).unwrap();
                    ids.reserve(
                        flash::base(ip, contract(true)),
                        1,
                        1,
                        1000,
                        1,
                        &mut CodecWorkspace::new(),
                    )
                    .unwrap();
                    let (io, proof) = flash::commit(ip, iff, &ids.requirement().unwrap());
                    ids.bind(io).unwrap();
                    ids.confirm(io, proof).unwrap();
                    ip.proof_retire(io).unwrap();
                    let ci = AdapterEventIngress::<1, 1, 1, 128>::new(1).unwrap();
                    let mut ca = AdapterOwner::new(1, &ci).unwrap();
                    let cl = ca.open_link(0, 1, 128).unwrap();
                    let mut rpc = durable_bridge(2, false);
                    let mut service = Service::new(1, 100).unwrap();
                    let h = rpc
                        .begin_durable_request(
                            &mut service,
                            client,
                            cc,
                            &mut SequenceCounter(10),
                            &mut ids,
                            ch,
                            cf,
                            Fingerprint::new([5; 16]).unwrap(),
                            cl,
                            &ca,
                            b"request",
                            121,
                            &mut SecurityPacketWorkspace::<128>::new(),
                        )
                        .unwrap();
                    let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                    rpc.submit(h, &mut service, client, ch, cf, &mut ca, &mut driver)
                        .unwrap();
                    rpc.retire_attempt(h, &mut ca).unwrap();
                    if expected != RpcResultKind::Business {
                        let malformed = custom(
                            server,
                            sc,
                            sh,
                            sf,
                            InteractionRole::Error,
                            20,
                            1,
                            6,
                            code,
                            b"unproven",
                        );
                        assert_eq!(
                            result(
                                &mut rpc,
                                h,
                                &mut service,
                                client,
                                cc,
                                ch,
                                cf,
                                &malformed,
                                cl
                            ),
                            Err(Error::Malformed)
                        );
                        assert_eq!(
                            service.view(rpc.view(h).unwrap().service).unwrap().phase,
                            RequestPhase::WaitResult
                        );
                    }
                    let good = custom(
                        server,
                        sc,
                        sh,
                        sf,
                        InteractionRole::Error,
                        21,
                        1,
                        6,
                        code,
                        &[],
                    );
                    assert!(
                        result(&mut rpc, h, &mut service, client, cc, ch, cf, &good, cl).unwrap()
                    );
                    assert_eq!(rpc.view(h).unwrap().result_kind, Some(expected));
                    assert_eq!(
                        service.view(rpc.view(h).unwrap().service).unwrap().result,
                        -1 - i32::from(code)
                    );
                    assert!(
                        !result(&mut rpc, h, &mut service, client, cc, ch, cf, &good, cl).unwrap()
                    );
                });
            });
        });
    }
}

#[test]
#[allow(clippy::too_many_lines)] // Independently advance real Journal while retaining an old RAM Owner/proof.
fn current_foundation_generation_blocks_stale_execution_and_stale_result_publication() {
    for terminal in [false, true] {
        security::exercise(1, 2, |client, ch, cf, cc| {
            security::exercise(2, 1, |server, sh, sf, sc| {
                let si = AdapterEventIngress::<1, 1, 1, 128>::new(2).unwrap();
                let mut sa = AdapterOwner::new(2, &si).unwrap();
                let sl = sa.open_link(0, 1, 128).unwrap();
                let mut rpc = durable_bridge(1, true);
                let mut service = Service::new(2, 100).unwrap();
                let p = custom(
                    client,
                    cc,
                    ch,
                    cf,
                    InteractionRole::Request,
                    10,
                    1000,
                    2,
                    0,
                    b"request",
                );
                let mut args = [0; 16];
                let got = request(
                    &mut rpc,
                    &mut service,
                    server,
                    sc,
                    sh,
                    sf,
                    &p,
                    sl,
                    &sa,
                    &mut args,
                )
                .unwrap();
                flash::exercise(contract(false), |jp, jf| {
                    let mut op = OperationOwner::<16>::new(contract(false)).unwrap();
                    let mut c = Coordinator::new(50, 1000).unwrap();
                    let h = prepare(
                        &mut c, &rpc, got.handle, b"request", &mut op, jp, server, sh, sf,
                    )
                    .unwrap();
                    finish(&mut c, h, &mut op, jp, jf, 22);
                    let sf23 = CurrentFacts { now_us: 23, ..sf };
                    arm(&mut c, h, &rpc, &mut op, jp, server, sh, sf23).unwrap();
                    finish(&mut c, h, &mut op, jp, jf, 24);
                    let gate = CallbackGate::new(7).unwrap();
                    let mut e = Executor {
                        calls: 0,
                        gate: &gate,
                        status: 7,
                        outcome: Ok(4),
                        nested_blocked: false,
                    };
                    if terminal {
                        c.execute(
                            h,
                            &rpc,
                            &mut op,
                            jp,
                            server,
                            sh,
                            CurrentFacts { now_us: 25, ..sf },
                            &gate,
                            &mut e,
                        )
                        .unwrap();
                        c.persist_outcome(h, &mut op, jp, 26, &mut CodecWorkspace::new())
                            .unwrap();
                        finish(&mut c, h, &mut op, jp, jf, 27);
                    }
                    let mut body = [0; 256];
                    let body_bytes = jp.copy_body(contract(false).domain, &mut body).unwrap();
                    let mut alias = OperationOwner::<16>::new(contract(false)).unwrap();
                    alias
                        .restore(
                            flash::base(jp, contract(false)),
                            &body[..body_bytes],
                            [5; 16],
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                    let next = if terminal {
                        alias
                            .acknowledge_result(alias.key().unwrap(), 28, 1)
                            .unwrap();
                        OperationPhase::Tombstoned
                    } else {
                        OperationPhase::InDoubt
                    };
                    alias
                        .transition(
                            next,
                            flash::base(jp, contract(false)),
                            7,
                            &[],
                            30,
                            1000,
                            9,
                            &mut CodecWorkspace::new(),
                        )
                        .unwrap();
                    let io = jp.submit(&alias.requirement().unwrap(), 30).unwrap();
                    alias.bind(io).unwrap();
                    for _ in 0..128 {
                        jp.step(jf, 30, 1).unwrap();
                        if let Ok(proof) = jp.proof_get(io) {
                            alias.confirm(io, proof).unwrap();
                            jp.proof_retire(io).unwrap();
                            break;
                        }
                    }
                    assert!(op.record_generation() < alias.record_generation());
                    let current = CurrentFacts { now_us: 31, ..sf };
                    if terminal {
                        assert_eq!(
                            c.publish(h, &mut rpc, &op, jp, &mut service, server, sh, current),
                            Err(Error::State)
                        );
                        assert_eq!(e.calls, 1);
                    } else {
                        assert_eq!(
                            c.execute(h, &rpc, &mut op, jp, server, sh, current, &gate, &mut e),
                            Err(Error::State)
                        );
                        assert_eq!(e.calls, 0);
                        assert!(!op.view().2);
                    }
                    assert_eq!(
                        service
                            .view(rpc.view(got.handle).unwrap().service)
                            .unwrap()
                            .phase,
                        RequestPhase::WaitResult
                    );
                });
            });
        });
    }
}
