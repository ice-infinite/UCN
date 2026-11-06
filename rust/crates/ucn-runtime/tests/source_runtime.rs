#![cfg(all(feature = "source-runtime", feature = "journal-runtime"))]
//! Full software client/server raw exchange, never a board test.
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::{Counter, Driver, SequenceCounter, security};
use security::flash;
use ucn_adapter::*;
use ucn_durable::DurableContract;
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_runtime::*;
use ucn_security::*;
use ucn_service::*;
use ucn_types::*;
use ucn_wire::CommonHeader;
const OPS: &[ProtocolOpcode] = &[
    ProtocolOpcode::OperationQuery,
    ProtocolOpcode::OperationReceipt,
    ProtocolOpcode::OperationCancel,
    ProtocolOpcode::OperationResume,
    ProtocolOpcode::OperationResult,
    ProtocolOpcode::OperationResultAck,
];
fn contract(ids: bool) -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::OperationJournal,
            id: if ids { 6 } else { 5 },
        },
        schema: if ids { 2 } else { 1 },
        version: 1,
        operation: if ids { 0x0702 } else { 0x0701 },
    }
}
fn config() -> SourceRuntimeConfig {
    SourceRuntimeConfig {
        owner: 20,
        id_interval: 4,
        io_timeout_us: 1000,
        recovery_lifetime_us: 500,
        retry_us: 10,
        max_attempts: 4,
    }
}
fn source(ids: OperationIds) -> SourceRuntime<128, 16> {
    SourceRuntime::new(
        config(),
        RpcEndpoint::new(
            ServiceId::new(0x40).unwrap(),
            [2; 16],
            false,
            true,
            16,
            OriginSecurity::O1,
        )
        .unwrap()
        .durable(),
        ids,
    )
    .unwrap()
}
fn server() -> JournalRuntime<2, 128, 16, 16> {
    JournalRuntime::new(
        JournalRuntimeConfig {
            owner: 10,
            request_lifetime_us: 1000,
            control_lifetime_us: 1000,
            io_timeout_us: 1000,
            retry_us: 10,
            max_attempts: 4,
            retention_us: 2,
        },
        RpcEndpoint::new(
            ServiceId::new(0x40).unwrap(),
            [1; 16],
            true,
            false,
            16,
            OriginSecurity::O1,
        )
        .unwrap()
        .durable(),
        OperationOwner::new(contract(false)).unwrap(),
    )
    .unwrap()
}
fn peer(session: SessionHandle, facts: CurrentFacts, link: LinkHandle, now: u64) -> JournalPeer {
    JournalPeer {
        session,
        facts: CurrentFacts {
            now_us: now,
            ..facts
        },
        fingerprint: Fingerprint::new([5; 16]).unwrap(),
        link,
    }
}
struct Executor<'a> {
    calls: usize,
    gate: &'a CallbackGate,
}
impl DurableExecutor for Executor<'_> {
    fn execute(&mut self, key: OperationKey, args: &[u8], out: &mut [u8]) -> Result<(i32, usize)> {
        assert_eq!(key.service.operation, 1);
        assert_eq!(args, b"request");
        assert!(self.gate.is_active());
        self.calls += 1;
        out[..4].copy_from_slice(b"done");
        Ok((7, 4))
    }
}
fn publish(ingress: &AdapterEventIngress<1, 2, 2, 128>, link: LinkHandle, bytes: &[u8], now: u64) {
    ingress
        .rx_publish(
            link,
            bytes,
            RxMeta {
                timestamp_us: now,
                sender_discriminator: 1,
            },
        )
        .unwrap();
}
#[allow(clippy::too_many_lines)] // Exact two real Coordinators/media/links, not fabricated completion.
fn exchange(drop_data: bool, corrupt_control: bool, mut lost_requests: usize) {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |remote, sh, sf, sc| {
            flash::exercise(contract(true), |ip, iff| {
                flash::exercise(contract(false), |jp, jff| {
                    let ci = AdapterEventIngress::<1, 2, 2, 128>::new(1).unwrap();
                    let si = AdapterEventIngress::<1, 2, 2, 128>::new(2).unwrap();
                    let mut ca = AdapterOwner::new(1, &ci).unwrap();
                    let cl = ca.open_link(0, 1, 128).unwrap();
                    ca.try_set_rx_enabled(true).unwrap();
                    let mut sa = AdapterOwner::new(2, &si).unwrap();
                    let sl = sa.open_link(0, 1, 128).unwrap();
                    sa.try_set_rx_enabled(true).unwrap();
                    let mut source = source(OperationIds::new(contract(true), 1).unwrap());
                    let mut server = server();
                    source.request(b"request", 21, 200).unwrap();
                    iff.pending_once = true;
                    jff.pending_once = true;
                    let mut cd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                    let mut sd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                    let mut executor = Executor {
                        calls: 0,
                        gate: remote.callback_gate(),
                    };
                    let mut cs = SequenceCounter(10);
                    let mut ss = SequenceCounter(20);
                    let mut ct = Counter(1);
                    let mut st = Counter(1);
                    let mut c_sent = 0;
                    let mut s_sent = 0;
                    let mut dropped = 0;
                    let mut corrupted = false;
                    let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
                    for now in 21..750 {
                        let calls = iff.calls;
                        let cr = source
                            .step(
                                budget,
                                peer(ch, cf, cl, now),
                                client,
                                cc,
                                ip,
                                iff,
                                &mut ca,
                                &mut cd,
                                &mut cs,
                                &mut ct,
                            )
                            .unwrap();
                        assert!(iff.calls - calls <= usize::from(cr.visits[1]));
                        assert_eq!(cr.visits, [1; 7]);
                        for bytes in &cd.frames[c_sent..] {
                            if lost_requests > 0
                                && CommonHeader::decode(bytes).unwrap().contract
                                    == HeaderContract::C1
                            {
                                lost_requests -= 1;
                                continue;
                            }
                            publish(&si, sl, bytes, now);
                        }
                        c_sent = cd.frames.len();
                        server
                            .step(
                                budget,
                                peer(sh, sf, sl, now),
                                remote,
                                sc,
                                jp,
                                jff,
                                &mut sa,
                                &mut sd,
                                &mut ss,
                                &mut st,
                                &mut executor,
                            )
                            .unwrap();
                        for bytes in &sd.frames[s_sent..] {
                            let header = CommonHeader::decode(bytes).unwrap();
                            if drop_data && header.contract == HeaderContract::C1 {
                                dropped += 1;
                                continue;
                            }
                            if corrupt_control
                                && !corrupted
                                && header.contract == HeaderContract::C0
                            {
                                let mut bad = bytes.clone();
                                *bad.last_mut().unwrap() ^= 1;
                                publish(&ci, cl, &bad, now);
                                corrupted = true;
                            } else {
                                publish(&ci, cl, bytes, now);
                            }
                        }
                        s_sent = sd.frames.len();
                        if server.operation().view().0 == Some(OperationPhase::Tombstoned) {
                            break;
                        }
                    }
                    let mut output = [0xA5; 16];
                    let result = source.copy_result(&mut output).unwrap();
                    assert_eq!(
                        result,
                        SourceResult {
                            phase: 3,
                            status: 7,
                            bytes: 4
                        }
                    );
                    assert_eq!(&output[..4], b"done");
                    assert_eq!(executor.calls, 1);
                    assert_eq!(lost_requests, 0);
                    assert_eq!(iff.witness, 1);
                    assert_eq!(jff.witness, 4);
                    assert_eq!(source.operation_key().unwrap().service.operation, 1);
                    assert_eq!(
                        server.operation().view().0,
                        Some(OperationPhase::Tombstoned)
                    );
                    let mut erased_output = [0xA5; 16];
                    assert_eq!(
                        server.operation().copy_result(&mut erased_output),
                        Err(Error::State)
                    );
                    assert_eq!(erased_output, [0xA5; 16]);
                    if drop_data {
                        assert!(dropped > 0);
                    }
                    if corrupt_control {
                        assert!(corrupted);
                        assert!(source.stats().rejected > 0);
                    }
                    let resume: Vec<_> = cd
                        .frames
                        .iter()
                        .filter(|v| {
                            CommonHeader::decode(v).unwrap().contract == HeaderContract::C0
                                && u16::from_be_bytes(v[27..29].try_into().unwrap())
                                    == u16::from(ProtocolOpcode::OperationResume)
                        })
                        .collect();
                    assert!(!resume.is_empty());
                    assert!(resume.iter().all(|v| *v == resume[0]));
                    assert_eq!(ct.0, 3); // one query + one real receipt ACK, no reseal on retry.
                    source.stop();
                    server.stop();
                    let mut cq = false;
                    let mut sq = false;
                    for now in 1100..1400 {
                        cq |= source
                            .step(
                                budget,
                                peer(ch, cf, cl, now),
                                client,
                                cc,
                                ip,
                                iff,
                                &mut ca,
                                &mut cd,
                                &mut cs,
                                &mut ct,
                            )
                            .unwrap()
                            .quiescent;
                        sq |= server
                            .step(
                                budget,
                                peer(sh, sf, sl, now),
                                remote,
                                sc,
                                jp,
                                jff,
                                &mut sa,
                                &mut sd,
                                &mut ss,
                                &mut st,
                                &mut executor,
                            )
                            .unwrap()
                            .quiescent;
                        if cq && sq {
                            break;
                        }
                    }
                    assert!(cq && sq);
                    assert_eq!(executor.calls, 1);
                });
            });
        });
    });
}
#[test]
fn actual_id_reload_client_server_data_resume_receipt_ack_gc_and_quiescence() {
    exchange(false, false, 0);
}
#[test]
fn lost_data_result_recovers_same_operation_without_reexecution() {
    exchange(true, false, 0);
}
#[test]
fn bad_terminal_mac_does_not_deliver_and_identical_query_retry_recovers() {
    exchange(false, true, 0);
}
#[test]
fn lost_first_two_request_frames_retry_exact_bytes_and_execute_once() {
    exchange(false, false, 2);
}
#[test]
#[allow(clippy::too_many_lines)] // Pending true media, stop and bounded final proof retirement.
fn stop_during_id_interval_write_retires_proof_without_request_or_id_publication() {
    security::exercise_opcodes(1, 2, OPS, |sec, sh, sf, crypto| {
        flash::exercise(contract(true), |p, f| {
            let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let mut source = source(OperationIds::new(contract(true), 1).unwrap());
            source.request(b"request", 21, 1000).unwrap();
            let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
            let mut sequence = SequenceCounter(10);
            let mut transaction = Counter(1);
            f.pending_once = true;
            for now in 21..23 {
                source
                    .step(
                        budget,
                        peer(sh, sf, link, now),
                        sec,
                        crypto,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        &mut sequence,
                        &mut transaction,
                    )
                    .unwrap();
            }
            source.stop();
            let mut quiescent = false;
            for now in 23..200 {
                if source
                    .step(
                        budget,
                        peer(sh, sf, link, now),
                        sec,
                        crypto,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        &mut sequence,
                        &mut transaction,
                    )
                    .unwrap()
                    .quiescent
                {
                    quiescent = true;
                    break;
                }
            }
            assert!(quiescent);
            assert_eq!(f.witness, 1);
            assert!(driver.frames.is_empty());
            assert_eq!(sequence.0, 10);
            assert_eq!(transaction.0, 1);
            assert!(source.operation_key().is_none());
            let mut restored = OperationIds::new(contract(true), 1).unwrap();
            let mut body = [0; 20];
            let n = p.copy_body(contract(true).domain, &mut body).unwrap();
            restored
                .restore(
                    flash::base(p, contract(true)),
                    &body[..n],
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
            assert_eq!(restored.allocate(), Err(Error::Exhausted));
        });
    });
}
#[test]
#[allow(clippy::too_many_lines)] // Budget-one flood, exact gate and output sentinels.
fn source_budget_one_fairness_and_zero_write_environment_gates() {
    security::exercise_opcodes(1, 2, OPS, |sec, sh, sf, crypto| {
        flash::exercise(contract(true), |p, f| {
            let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            adapter.try_set_rx_enabled(true).unwrap();
            let mut source = source(OperationIds::new(contract(true), 1).unwrap());
            let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let budget = JournalStepBudget::new(1, [1; 7]).unwrap();
            let mut sequence = SequenceCounter(10);
            let mut transaction = Counter(1);
            for now in 21..161 {
                if now % 7 == 0 {
                    publish(&ingress, link, &[0; 3], now);
                }
                let r = source
                    .step(
                        budget,
                        peer(sh, sf, link, now),
                        sec,
                        crypto,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        &mut sequence,
                        &mut transaction,
                    )
                    .unwrap();
                assert_eq!(r.visits.iter().sum::<u16>(), 1);
            }
            assert_eq!(source.stats().visits, [20; 7]);
            let before = source.stats();
            let calls = f.calls;
            let gate = sec.callback_gate();
            let lease = gate
                .try_enter(CallbackClaim::new(90, 1, 1, 1).unwrap())
                .unwrap();
            assert_eq!(
                source.step(
                    budget,
                    peer(sh, sf, link, 161),
                    sec,
                    crypto,
                    p,
                    f,
                    &mut adapter,
                    &mut driver,
                    &mut sequence,
                    &mut transaction
                ),
                Err(Error::State)
            );
            gate.leave(lease).unwrap();
            assert_eq!(
                source.step(
                    budget,
                    peer(sh, sf, link, 0),
                    sec,
                    crypto,
                    p,
                    f,
                    &mut adapter,
                    &mut driver,
                    &mut sequence,
                    &mut transaction
                ),
                Err(Error::State)
            );
            assert_eq!(source.stats(), before);
            assert_eq!(f.calls, calls);
            assert!(driver.frames.is_empty());
            let mut out = [0xA5; 16];
            assert_eq!(source.copy_result(&mut out), Err(Error::State));
            assert_eq!(out, [0xA5; 16]);
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // A real Driver UNKNOWN holder must survive logical stop/deadline.
fn source_unknown_attempt_is_retained_until_exact_completion_after_modeled_reopen() {
    security::exercise_opcodes(1, 2, OPS, |sec, sh, sf, crypto| {
        flash::exercise(contract(true), |p, f| {
            let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let mut source = source(OperationIds::new(contract(true), 1).unwrap());
            source.request(b"request", 21, 200).unwrap();
            let mut driver = Driver::new(DriverSubmit::Unknown);
            let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
            let mut sequence = SequenceCounter(10);
            let mut transaction = Counter(1);
            for now in 21..120 {
                source
                    .step(
                        budget,
                        peer(sh, sf, link, now),
                        sec,
                        crypto,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        &mut sequence,
                        &mut transaction,
                    )
                    .unwrap();
                if !driver.tokens.is_empty() {
                    break;
                }
            }
            let token = driver.tokens[0];
            source.stop();
            for now in 250..300 {
                assert!(
                    !source
                        .step(
                            budget,
                            peer(sh, sf, link, now),
                            sec,
                            crypto,
                            p,
                            f,
                            &mut adapter,
                            &mut driver,
                            &mut sequence,
                            &mut transaction
                        )
                        .unwrap()
                        .quiescent
                );
            }
            assert_eq!(adapter.tx_view(token).unwrap().state, TxState::InDoubt);
            assert_eq!(driver.frames.len(), 1);
            adapter.reopen_link(link, 2, 128).unwrap();
            ingress
                .tx_complete(token, TerminalOutcome::Success)
                .unwrap();
            let mut quiet = false;
            for now in 300..400 {
                quiet |= source
                    .step(
                        budget,
                        peer(sh, sf, link, now),
                        sec,
                        crypto,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        &mut sequence,
                        &mut transaction,
                    )
                    .unwrap()
                    .quiescent;
                if quiet {
                    break;
                }
            }
            assert!(quiet);
            assert!(adapter.tx_view(token).is_err());
            assert_eq!(driver.frames.len(), 1);
            assert!(source.copy_result(&mut [0; 16]).is_err());
        });
    });
}

#[test]
fn source_rejects_actual_newer_id_media_and_fenced_allocator_before_io_or_statistics() {
    security::exercise_opcodes(1, 2, OPS, |sec, sh, sf, crypto| {
        flash::exercise(contract(true), |p, f| {
            let mut stale = source(OperationIds::new(contract(true), 1).unwrap());
            let mut actual = OperationIds::new(contract(true), 1).unwrap();
            actual
                .reserve(
                    flash::base(p, contract(true)),
                    4,
                    1,
                    1000,
                    1,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
            let (io, proof) = flash::commit(p, f, &actual.requirement().unwrap());
            actual.bind(io).unwrap();
            actual.confirm(io, proof).unwrap();
            p.proof_retire(io).unwrap();
            let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let mut driver = Driver::new(DriverSubmit::NotSubmitted);
            let calls = f.calls;
            let stats = stale.stats();
            assert_eq!(
                stale.step(
                    JournalStepBudget::new(7, [1; 7]).unwrap(),
                    peer(sh, sf, link, 21),
                    sec,
                    crypto,
                    p,
                    f,
                    &mut adapter,
                    &mut driver,
                    &mut SequenceCounter(10),
                    &mut Counter(1)
                ),
                Err(Error::State)
            );
            assert_eq!(stale.stats(), stats);
            assert_eq!(f.calls, calls);
            assert!(driver.frames.is_empty());
            actual.fence();
            assert_eq!(actual.allocate(), Err(Error::Exhausted));
            assert!(
                SourceRuntime::<128, 16>::new(
                    config(),
                    RpcEndpoint::new(
                        ServiceId::new(0x40).unwrap(),
                        [2; 16],
                        false,
                        true,
                        16,
                        OriginSecurity::O1
                    )
                    .unwrap()
                    .durable(),
                    actual
                )
                .is_err()
            );
        });
    });
}

#[test]
fn recovery_cannot_bypass_current_endpoint_minimum_security() {
    security::exercise_opcodes(1, 2, OPS, |sec, sh, sf, crypto| {
        flash::exercise(contract(true), |p, f| {
            let mut source = SourceRuntime::<128, 16>::new(
                config(),
                RpcEndpoint::new(
                    ServiceId::new(0x40).unwrap(),
                    [2; 16],
                    false,
                    true,
                    16,
                    OriginSecurity::O2,
                )
                .unwrap()
                .durable(),
                OperationIds::new(contract(true), 1).unwrap(),
            )
            .unwrap();
            let original = OperationKey {
                service: ServiceKey {
                    realm: sec.realm().get(),
                    client: sf.local.principal,
                    server: sf.peer.principal,
                    client_address: sf.local.address.get(),
                    client_generation: sf.local.generation.get(),
                    server_address: sf.peer.address.get(),
                    server_generation: sf.peer.generation.get(),
                    operation: 1,
                    service: 0x40,
                    opcode: 0,
                    session: 1,
                    key_generation: 1,
                    origin: OriginSecurity::O1,
                    policy: sf.policy_generation,
                    security: [5; 16],
                },
                request_digest: [1; 16],
            };
            source.recover(original, 21, 100).unwrap();
            let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            let mut driver = Driver::new(DriverSubmit::NotSubmitted);
            let mut sequence = SequenceCounter(10);
            let mut tx = Counter(1);
            let report = source
                .step(
                    JournalStepBudget::new(7, [1; 7]).unwrap(),
                    peer(sh, sf, link, 21),
                    sec,
                    crypto,
                    p,
                    f,
                    &mut adapter,
                    &mut driver,
                    &mut sequence,
                    &mut tx,
                )
                .unwrap();
            assert_eq!(report.error, Some(Error::Access));
            assert_eq!(tx.0, 1);
            assert!(driver.frames.is_empty());
            let mut output = [0xA5; 16];
            assert_eq!(source.copy_result(&mut output), Err(Error::State));
            assert_eq!(output, [0xA5; 16]);
        });
    });
}
#[test]
fn current_policy_and_full_recovery_mtu_are_checked_before_id_provider_write() {
    for denied_policy in [true, false] {
        security::exercise_opcodes(1, 2, OPS, |sec, sh, sf, crypto| {
            flash::exercise(contract(true), |p, f| {
                let minimum = if denied_policy {
                    OriginSecurity::O2
                } else {
                    OriginSecurity::O1
                };
                let mut source = SourceRuntime::<128, 16>::new(
                    config(),
                    RpcEndpoint::new(
                        ServiceId::new(0x40).unwrap(),
                        [2; 16],
                        false,
                        true,
                        16,
                        minimum,
                    )
                    .unwrap()
                    .durable(),
                    OperationIds::new(contract(true), 1).unwrap(),
                )
                .unwrap();
                source.request(b"request", 21, 100).unwrap();
                let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(1).unwrap();
                let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
                let link = adapter
                    .open_link(0, 1, if denied_policy { 128 } else { 64 })
                    .unwrap();
                let mut driver = Driver::new(DriverSubmit::NotSubmitted);
                let mut sequence = SequenceCounter(10);
                let mut tx = Counter(1);
                let calls = f.calls;
                let report = source
                    .step(
                        JournalStepBudget::new(7, [1; 7]).unwrap(),
                        peer(sh, sf, link, 21),
                        sec,
                        crypto,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        &mut sequence,
                        &mut tx,
                    )
                    .unwrap();
                assert_eq!(
                    report.error,
                    Some(if denied_policy {
                        Error::Access
                    } else {
                        Error::NoSpace
                    })
                );
                assert_eq!(f.calls, calls);
                assert_eq!(f.witness, 0);
                assert_eq!(sequence.0, 10);
                assert_eq!(tx.0, 1);
                assert!(driver.frames.is_empty());
                assert!(source.operation_key().is_none());
            });
        });
    }
}
