#![cfg(feature = "journal-runtime")]
//! Actual protected raw RX, Foundation media and scheduler-driven execution/retirement; Host only.
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::{Counter, Driver, SequenceCounter, security};
use security::flash;
use ucn_adapter::*;
use ucn_digest::DigestWorkspace;
use ucn_durable::{DurableBase, DurableContract};
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind, RecordMeta, body_digest};
use ucn_runtime::*;
use ucn_security::*;
use ucn_service::*;
use ucn_types::*;
use ucn_wire::CommonHeader;

type Runtime = JournalRuntime<2, 128, 16, 16>;
const OPS: &[ProtocolOpcode] = &[
    ProtocolOpcode::OperationQuery,
    ProtocolOpcode::OperationReceipt,
    ProtocolOpcode::OperationCancel,
    ProtocolOpcode::OperationResume,
    ProtocolOpcode::OperationResult,
    ProtocolOpcode::OperationResultAck,
];
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
fn config() -> JournalRuntimeConfig {
    JournalRuntimeConfig {
        owner: 10,
        request_lifetime_us: 1000,
        control_lifetime_us: 1000,
        io_timeout_us: 1000,
        retry_us: 2,
        max_attempts: 4,
        retention_us: 2,
    }
}
fn runtime() -> Runtime {
    Runtime::new(
        config(),
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
        OperationOwner::new(contract()).unwrap(),
    )
    .unwrap()
}
struct Executor<'a> {
    calls: usize,
    gate: &'a CallbackGate,
}
struct Gated<'a> {
    driver: Driver,
    gate: &'a CallbackGate,
}
impl TxDriver for Gated<'_> {
    fn submit(&mut self, link: LinkHandle, bytes: &[u8], token: TxToken) -> DriverSubmit {
        assert!(self.gate.is_active());
        self.driver.submit(link, bytes, token)
    }
    fn cancel(&mut self, token: TxToken) -> DriverCancel {
        assert!(self.gate.is_active());
        self.driver.cancel(token)
    }
}
impl DurableExecutor for Executor<'_> {
    fn execute(&mut self, key: OperationKey, args: &[u8], out: &mut [u8]) -> Result<(i32, usize)> {
        assert_eq!(key.service.operation, 100);
        assert_eq!(key.service.opcode, 0);
        assert_eq!(args, b"request");
        assert!(self.gate.is_active());
        assert!(
            self.gate
                .try_enter(CallbackClaim::new(99, 1, 1, 1).unwrap())
                .is_err()
        );
        self.calls += 1;
        out[..4].copy_from_slice(b"done");
        Ok((7, 4))
    }
}
fn request(
    client: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    seq: u32,
) -> Vec<u8> {
    let mut prefix = [0; 13];
    CommonHeader {
        contract: HeaderContract::C1,
        origin_security: OriginSecurity::O1,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: InteractionRole::Request,
        payload_kind: PayloadKind::Data,
        traffic_class: TrafficClass::Q2,
        hop_limit: HopLimit::new(1).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..5].copy_from_slice(&1u16.to_be_bytes());
    prefix[5..7].copy_from_slice(&2u16.to_be_bytes());
    prefix[7..9].copy_from_slice(&0x40u16.to_be_bytes());
    prefix[9..].copy_from_slice(&seq.to_be_bytes());
    let mut identity = [0; 46];
    identity[..16].copy_from_slice(&facts.local.principal);
    identity[16..20].copy_from_slice(&facts.local.generation.get().to_be_bytes());
    identity[20..36].copy_from_slice(&facts.peer.principal);
    identity[36..40].copy_from_slice(&facts.peer.generation.get().to_be_bytes());
    identity[40..42].copy_from_slice(&0x40u16.to_be_bytes());
    identity[42..].copy_from_slice(&seq.to_be_bytes());
    let mut payload = [0; 16];
    encode_operation_envelope(
        InteractionRole::Request,
        OperationEnvelope {
            operation: 100,
            flags: 2,
            result: 0,
        },
        &mut payload,
    )
    .unwrap();
    payload[9..].copy_from_slice(b"request");
    let mut bytes = [0; 128];
    let n = client
        .protect_packet(
            crypto,
            OriginCounterOwner::Sequence(&mut SequenceCounter(seq)),
            session,
            facts,
            fixture::data_access(session, AccessDirection::Outbound),
            PacketPlan {
                prefix: &prefix,
                payload: &payload,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: Fingerprint::new([5; 16]).unwrap(),
                    sequence: OriginSequence::new(seq).unwrap(),
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut SecurityPacketWorkspace::<128>::new(),
            &mut bytes,
        )
        .unwrap();
    bytes[..n].to_vec()
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
#[allow(clippy::too_many_arguments)]
fn step(
    runtime: &mut Runtime,
    now: u64,
    budget: JournalStepBudget,
    server: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    persistence: &mut flash::Owner<'_>,
    flash: &mut flash::Flash,
    adapter: &mut AdapterOwner<'_, 1, 2, 2, 128>,
    driver: &mut impl TxDriver,
    link: LinkHandle,
    executor: &mut Executor<'_>,
    sequence: &mut SequenceCounter,
    transaction: &mut Counter,
) -> JournalStepReport {
    let calls = flash.calls;
    let report = runtime
        .step(
            budget,
            peer(session, facts, link, now),
            server,
            crypto,
            persistence,
            flash,
            adapter,
            driver,
            sequence,
            transaction,
            executor,
        )
        .unwrap();
    assert!(
        flash.calls - calls <= usize::from(report.visits[JournalWorkClass::Foundation as usize])
    );
    report
}
#[test]
#[allow(clippy::too_many_lines)]
fn raw_data_and_control_journal_ack_gc_then_stop_are_driven_only_by_bounded_step() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                adapter.try_set_rx_enabled(true).unwrap();
                let mut runtime = runtime();
                let gate = server.callback_gate();
                let mut executor = Executor { calls: 0, gate };
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                let mut sequence = SequenceCounter(20);
                let mut transaction = Counter(1);
                let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
                f.pending_once = true; // Real asynchronous Foundation poll, not a fake durable flag.
                let raw = request(client, cc, ch, cf, 10);
                ingress
                    .rx_publish(
                        link,
                        &raw,
                        RxMeta {
                            timestamp_us: 21,
                            sender_discriminator: 1,
                        },
                    )
                    .unwrap();
                for now in 21..130 {
                    let result = step(
                        &mut runtime,
                        now,
                        budget,
                        server,
                        sc,
                        sh,
                        sf,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        link,
                        &mut executor,
                        &mut sequence,
                        &mut transaction,
                    );
                    assert_eq!(result.visits, [1; 7]);
                    if driver
                        .frames
                        .iter()
                        .any(|b| CommonHeader::decode(b).unwrap().contract == HeaderContract::C1)
                    {
                        break;
                    }
                }
                assert_eq!(executor.calls, 1);
                assert_eq!(
                    runtime.operation().view().0,
                    Some(OperationPhase::CommittedResult)
                );
                assert_eq!(f.witness, 3);
                let now = runtime.stats().visits[0] + 30; // Advances trusted time, not a lease reset.
                let key = runtime.operation().key().unwrap();
                let wire = OperationWireKey {
                    operation: key.service.operation,
                    service: key.service.service,
                    opcode: 0,
                    request_digest: key.request_digest,
                };
                let mut body = [0; 28];
                encode_operation_key(wire, &mut body).unwrap();
                let resume = fixture::protect(
                    client,
                    cc,
                    ch,
                    CurrentFacts { now_us: now, ..cf },
                    ProtocolOpcode::OperationResume,
                    &body,
                    1,
                );
                ingress
                    .rx_publish(
                        link,
                        &resume.bytes[..resume.length],
                        RxMeta {
                            timestamp_us: now,
                            sender_discriminator: 1,
                        },
                    )
                    .unwrap();
                for tick in now..now + 100 {
                    step(
                        &mut runtime,
                        tick,
                        budget,
                        server,
                        sc,
                        sh,
                        sf,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        link,
                        &mut executor,
                        &mut sequence,
                        &mut transaction,
                    );
                    if driver
                        .frames
                        .iter()
                        .any(|b| CommonHeader::decode(b).unwrap().contract == HeaderContract::C0)
                    {
                        break;
                    }
                }
                assert!(
                    driver
                        .frames
                        .iter()
                        .any(|b| CommonHeader::decode(b).unwrap().contract == HeaderContract::C0)
                );
                let response = driver
                    .frames
                    .iter()
                    .find(|b| CommonHeader::decode(b).unwrap().contract == HeaderContract::C0)
                    .unwrap();
                let mut packet = fixture::Packet {
                    bytes: [0; 128],
                    length: response.len(),
                };
                packet.bytes[..packet.length].copy_from_slice(response);
                let mut source_queue = fixture::Queue::new(80).unwrap();
                let mut source_replies = fixture::Replies::new(81, 2, 4).unwrap();
                let source_facts = CurrentFacts {
                    now_us: now + 101,
                    ..cf
                };
                let response_handle = fixture::command(
                    fixture::receive(
                        &mut source_queue,
                        &mut source_replies,
                        client,
                        cc,
                        ch,
                        source_facts,
                        &packet,
                    )
                    .unwrap(),
                );
                assert_eq!(
                    key.request_digest,
                    ucn_digest::canonical_digest(
                        b"UCN6-PAYLOAD-V1",
                        &raw[13..raw.len() - 16],
                        &mut DigestWorkspace::new()
                    )
                    .unwrap()
                );
                let mut recovery = OperationRecovery::new(key, now, now + 1000).unwrap();
                let mut result = [0; 16];
                let recovered = source_queue
                    .dispatch_recovered_result(
                        response_handle,
                        &mut recovery,
                        client,
                        ch,
                        source_facts,
                        &mut source_replies,
                        &mut result,
                        &mut DigestWorkspace::new(),
                    )
                    .unwrap();
                assert_eq!(&result[..recovered.bytes], b"done");
                assert_eq!(recovered.status, 7);
                let source_ingress = AdapterEventIngress::<1, 1, 1, 128>::new(3).unwrap();
                let mut source_adapter = AdapterOwner::new(3, &source_ingress).unwrap();
                let source_link = source_adapter.open_link(0, 1, 128).unwrap();
                let raw_ack = fixture::emit(
                    &mut source_replies,
                    recovered.ack.unwrap(),
                    client,
                    cc,
                    ch,
                    source_facts,
                    &mut Counter(2),
                    &mut source_adapter,
                    &mut driver,
                    source_link,
                );
                ingress
                    .rx_publish(
                        link,
                        &raw_ack.bytes[..raw_ack.length],
                        RxMeta {
                            timestamp_us: now + 101,
                            sender_discriminator: 1,
                        },
                    )
                    .unwrap();
                for tick in now + 101..now + 250 {
                    step(
                        &mut runtime,
                        tick,
                        budget,
                        server,
                        sc,
                        sh,
                        sf,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        link,
                        &mut executor,
                        &mut sequence,
                        &mut transaction,
                    );
                    if runtime.operation().view().0 == Some(OperationPhase::Tombstoned) {
                        break;
                    }
                }
                assert_eq!(
                    runtime.operation().view().0,
                    Some(OperationPhase::Tombstoned)
                );
                assert_eq!(f.witness, 4);
                assert_eq!(executor.calls, 1);
                runtime.stop();
                let mut quiescent = false;
                for tick in 1200..1300 {
                    if step(
                        &mut runtime,
                        tick,
                        budget,
                        server,
                        sc,
                        sh,
                        sf,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        link,
                        &mut executor,
                        &mut sequence,
                        &mut transaction,
                    )
                    .quiescent
                    {
                        quiescent = true;
                        break;
                    }
                }
                assert!(quiescent);
                assert_eq!(executor.calls, 1);
                assert_eq!(runtime.operation().key(), Some(key));
            });
        });
    });
}

#[test]
fn single_visit_steps_rotate_under_rx_flood_and_invalid_time_or_gate_is_zero_write() {
    security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
        flash::exercise(contract(), |p, f| {
            let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(2).unwrap();
            let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            adapter.try_set_rx_enabled(true).unwrap();
            let mut runtime = runtime();
            let gate = server.callback_gate();
            let mut executor = Executor { calls: 0, gate };
            let mut driver = Driver::new(DriverSubmit::NotSubmitted);
            let mut sequence = SequenceCounter(20);
            let mut transaction = Counter(1);
            let budget = JournalStepBudget::new(1, [1; 7]).unwrap();
            for now in 21..161 {
                let _rx = ingress.rx_publish(
                    link,
                    &[0xff],
                    RxMeta {
                        timestamp_us: now,
                        sender_discriminator: 1,
                    },
                );
                let report = step(
                    &mut runtime,
                    now,
                    budget,
                    server,
                    sc,
                    sh,
                    sf,
                    p,
                    f,
                    &mut adapter,
                    &mut driver,
                    link,
                    &mut executor,
                    &mut sequence,
                    &mut transaction,
                );
                assert_eq!(report.visits.iter().sum::<u16>(), 1);
            }
            assert_eq!(runtime.stats().visits, [20; 7]);
            assert_eq!(executor.calls, 0);
            assert!(runtime.stats().rejected > 0);
            assert!(runtime.stats().retired > 0);
            let snapshot = runtime.stats();
            let calls = f.calls;
            let held = gate
                .try_enter(CallbackClaim::new(99, 1, 1, 1).unwrap())
                .unwrap();
            assert_eq!(
                runtime.step(
                    budget,
                    peer(sh, sf, link, 161),
                    server,
                    sc,
                    p,
                    f,
                    &mut adapter,
                    &mut driver,
                    &mut sequence,
                    &mut transaction,
                    &mut executor
                ),
                Err(Error::State)
            );
            gate.leave(held).unwrap();
            assert_eq!(runtime.stats(), snapshot);
            assert_eq!(f.calls, calls);
            assert_eq!(
                runtime.step(
                    budget,
                    peer(sh, sf, link, 159),
                    server,
                    sc,
                    p,
                    f,
                    &mut adapter,
                    &mut driver,
                    &mut sequence,
                    &mut transaction,
                    &mut executor
                ),
                Err(Error::State)
            );
            assert_eq!(runtime.stats(), snapshot);
            assert!(JournalStepBudget::new(0, [1; 7]).is_err());
            assert!(JournalStepBudget::new(65, [16; 7]).is_err());
            assert!(JournalStepBudget::new(1, [1, 1, 1, 1, 1, 1, 0]).is_err());
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Explicit cancellation acceptance before executor publication.
fn authenticated_cancel_during_journal_write_is_serialized_and_never_executes() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                adapter.try_set_rx_enabled(true).unwrap();
                let mut runtime = runtime();
                let gate = server.callback_gate();
                let mut executor = Executor { calls: 0, gate };
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                let mut sequence = SequenceCounter(20);
                let mut transaction = Counter(1);
                let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
                let raw = request(client, cc, ch, cf, 10);
                ingress
                    .rx_publish(
                        link,
                        &raw,
                        RxMeta {
                            timestamp_us: 21,
                            sender_discriminator: 1,
                        },
                    )
                    .unwrap();
                step(
                    &mut runtime,
                    21,
                    budget,
                    server,
                    sc,
                    sh,
                    sf,
                    p,
                    f,
                    &mut adapter,
                    &mut driver,
                    link,
                    &mut executor,
                    &mut sequence,
                    &mut transaction,
                );
                assert_eq!(
                    runtime.continuation_phase().unwrap(),
                    Some(DurableRpcPhase::PreparedPending)
                );
                assert_eq!(f.witness, 0);
                let key = runtime.operation().key().unwrap();
                let mut body = [0; 32];
                encode_operation_cancel(
                    OperationCancel {
                        key: OperationWireKey {
                            operation: 100,
                            service: 0x40,
                            opcode: 0,
                            request_digest: key.request_digest,
                        },
                        reason: 1,
                    },
                    &mut body,
                )
                .unwrap();
                let cancel = fixture::protect(
                    client,
                    cc,
                    ch,
                    CurrentFacts { now_us: 22, ..cf },
                    ProtocolOpcode::OperationCancel,
                    &body,
                    1,
                );
                ingress
                    .rx_publish(
                        link,
                        &cancel.bytes[..cancel.length],
                        RxMeta {
                            timestamp_us: 22,
                            sender_discriminator: 1,
                        },
                    )
                    .unwrap();
                for now in 22..120 {
                    step(
                        &mut runtime,
                        now,
                        budget,
                        server,
                        sc,
                        sh,
                        sf,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        link,
                        &mut executor,
                        &mut sequence,
                        &mut transaction,
                    );
                    if runtime.operation().view().0 == Some(OperationPhase::AbortedNoEffect)
                        && driver.frames.iter().any(|b| {
                            CommonHeader::decode(b).unwrap().contract == HeaderContract::C0
                        })
                    {
                        break;
                    }
                }
                assert_eq!(executor.calls, 0);
                assert_eq!(
                    runtime.operation().view().0,
                    Some(OperationPhase::AbortedNoEffect)
                );
                assert_eq!(f.witness, 2);
                assert!(
                    driver
                        .frames
                        .iter()
                        .any(|b| CommonHeader::decode(b).unwrap().contract == HeaderContract::C0)
                );
            });
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Stop at both distinct durable pre-execution edges.
fn stop_finishes_accepted_prepared_or_executing_io_then_records_no_effect() {
    for stopped_at in [
        DurableRpcPhase::PreparedPending,
        DurableRpcPhase::ExecutingPending,
    ] {
        security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
            security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
                flash::exercise(contract(), |p, f| {
                    let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(2).unwrap();
                    let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                    let link = adapter.open_link(0, 1, 128).unwrap();
                    adapter.try_set_rx_enabled(true).unwrap();
                    let mut runtime = runtime();
                    let gate = server.callback_gate();
                    let mut executor = Executor { calls: 0, gate };
                    let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                    let mut sequence = SequenceCounter(20);
                    let mut transaction = Counter(1);
                    let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
                    let raw = request(client, cc, ch, cf, 10);
                    ingress
                        .rx_publish(
                            link,
                            &raw,
                            RxMeta {
                                timestamp_us: 21,
                                sender_discriminator: 1,
                            },
                        )
                        .unwrap();
                    let mut stop_time = 0;
                    for now in 21..120 {
                        step(
                            &mut runtime,
                            now,
                            budget,
                            server,
                            sc,
                            sh,
                            sf,
                            p,
                            f,
                            &mut adapter,
                            &mut driver,
                            link,
                            &mut executor,
                            &mut sequence,
                            &mut transaction,
                        );
                        if runtime.continuation_phase().unwrap() == Some(stopped_at) {
                            stop_time = now;
                            break;
                        }
                    }
                    assert!(stop_time > 0);
                    runtime.stop();
                    for now in stop_time + 1..stop_time + 150 {
                        step(
                            &mut runtime,
                            now,
                            budget,
                            server,
                            sc,
                            sh,
                            sf,
                            p,
                            f,
                            &mut adapter,
                            &mut driver,
                            link,
                            &mut executor,
                            &mut sequence,
                            &mut transaction,
                        );
                        if runtime.operation().view().0 == Some(OperationPhase::AbortedNoEffect)
                            && !runtime.operation().view().1
                        {
                            break;
                        }
                    }
                    assert_eq!(executor.calls, 0);
                    assert!(driver.frames.is_empty());
                    assert_eq!(
                        runtime.operation().view(),
                        (Some(OperationPhase::AbortedNoEffect), false, false)
                    );
                    let mut quiet = false;
                    for now in 1200..1300 {
                        if step(
                            &mut runtime,
                            now,
                            budget,
                            server,
                            sc,
                            sh,
                            sf,
                            p,
                            f,
                            &mut adapter,
                            &mut driver,
                            link,
                            &mut executor,
                            &mut sequence,
                            &mut transaction,
                        )
                        .quiescent
                        {
                            quiet = true;
                            break;
                        }
                    }
                    assert!(quiet);
                    assert!(!p.domain_get(contract().domain).unwrap().pending);
                });
            });
        });
    }
}

#[test]
#[allow(clippy::too_many_lines)] // NOT_SUBMITTED keeps bytes; UNKNOWN waits for real completion even after stop.
fn backpressure_retries_identical_bytes_and_unknown_preserves_holder_until_actual_completion() {
    for outcome in [DriverSubmit::NotSubmitted, DriverSubmit::Unknown] {
        security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
            security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
                flash::exercise(contract(), |p, f| {
                    let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(2).unwrap();
                    let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                    let link = adapter.open_link(0, 1, 128).unwrap();
                    adapter.try_set_rx_enabled(true).unwrap();
                    let mut runtime = runtime();
                    let gate = server.callback_gate();
                    let mut executor = Executor { calls: 0, gate };
                    let mut driver = Gated {
                        driver: Driver::new(outcome),
                        gate,
                    };
                    let mut sequence = SequenceCounter(20);
                    let mut transaction = Counter(1);
                    let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
                    let raw = request(client, cc, ch, cf, 10);
                    ingress
                        .rx_publish(
                            link,
                            &raw,
                            RxMeta {
                                timestamp_us: 21,
                                sender_discriminator: 1,
                            },
                        )
                        .unwrap();
                    let mut first = 0;
                    for now in 21..200 {
                        step(
                            &mut runtime,
                            now,
                            budget,
                            server,
                            sc,
                            sh,
                            sf,
                            p,
                            f,
                            &mut adapter,
                            &mut driver,
                            link,
                            &mut executor,
                            &mut sequence,
                            &mut transaction,
                        );
                        if !driver.driver.frames.is_empty() {
                            first = now;
                            break;
                        }
                    }
                    assert!(first > 0);
                    assert_eq!(executor.calls, 1);
                    let sealed = driver.driver.frames[0].clone();
                    if outcome == DriverSubmit::NotSubmitted {
                        for now in first + 1..first + 80 {
                            step(
                                &mut runtime,
                                now,
                                budget,
                                server,
                                sc,
                                sh,
                                sf,
                                p,
                                f,
                                &mut adapter,
                                &mut driver,
                                link,
                                &mut executor,
                                &mut sequence,
                                &mut transaction,
                            );
                            if driver.driver.frames.len() == 3 {
                                break;
                            }
                        }
                        assert_eq!(driver.driver.frames.len(), 3);
                        driver.driver.submit = DriverSubmit::Complete(TerminalOutcome::Success);
                        for now in first + 81..first + 160 {
                            step(
                                &mut runtime,
                                now,
                                budget,
                                server,
                                sc,
                                sh,
                                sf,
                                p,
                                f,
                                &mut adapter,
                                &mut driver,
                                link,
                                &mut executor,
                                &mut sequence,
                                &mut transaction,
                            );
                            if driver.driver.frames.len() == 4 {
                                break;
                            }
                        }
                        assert_eq!(driver.driver.frames.len(), 4);
                        assert!(driver.driver.frames.iter().all(|b| b == &sealed));
                        assert_eq!(sequence.0, 21);
                    } else {
                        let token = driver.driver.tokens[0];
                        runtime.stop();
                        for now in 1200..1250 {
                            assert!(
                                !step(
                                    &mut runtime,
                                    now,
                                    budget,
                                    server,
                                    sc,
                                    sh,
                                    sf,
                                    p,
                                    f,
                                    &mut adapter,
                                    &mut driver,
                                    link,
                                    &mut executor,
                                    &mut sequence,
                                    &mut transaction
                                )
                                .quiescent
                            );
                        }
                        assert_eq!(driver.driver.frames.len(), 1);
                        assert_eq!(adapter.tx_view(token).unwrap().state, TxState::InDoubt);
                        // UNKNOWN is sticky within the old instance; only modeled reopen plus
                        // exact late completion permits its old physical holder to retire.
                        adapter.reopen_link(link, 2, 128).unwrap();
                        ingress
                            .tx_complete(token, TerminalOutcome::Success)
                            .unwrap();
                        let mut quiet = false;
                        for now in 1250..1350 {
                            if step(
                                &mut runtime,
                                now,
                                budget,
                                server,
                                sc,
                                sh,
                                sf,
                                p,
                                f,
                                &mut adapter,
                                &mut driver,
                                link,
                                &mut executor,
                                &mut sequence,
                                &mut transaction,
                            )
                            .quiescent
                            {
                                quiet = true;
                                break;
                            }
                        }
                        assert!(quiet);
                        assert_eq!(adapter.tx_view(token), Err(Error::NotFound));
                    }
                    assert_eq!(executor.calls, 1);
                    assert_eq!(
                        runtime.operation().view().0,
                        Some(OperationPhase::CommittedResult)
                    );
                });
            });
        });
    }
}

#[test]
fn corrupt_readback_faults_and_retires_without_executor_or_success_reply() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                adapter.try_set_rx_enabled(true).unwrap();
                let mut runtime = runtime();
                let gate = server.callback_gate();
                let mut executor = Executor { calls: 0, gate };
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                let mut sequence = SequenceCounter(20);
                let mut transaction = Counter(1);
                let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
                let before = f.slots;
                let raw = request(client, cc, ch, cf, 10);
                ingress
                    .rx_publish(
                        link,
                        &raw,
                        RxMeta {
                            timestamp_us: 21,
                            sender_discriminator: 1,
                        },
                    )
                    .unwrap();
                let mut corrupted = false;
                for now in 21..100 {
                    step(
                        &mut runtime,
                        now,
                        budget,
                        server,
                        sc,
                        sh,
                        sf,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        link,
                        &mut executor,
                        &mut sequence,
                        &mut transaction,
                    );
                    if !corrupted {
                        if let Some(slot) =
                            f.slots.iter().zip(before.iter()).position(|(n, o)| n != o)
                        {
                            f.slots[slot][0] ^= 1;
                            corrupted = true;
                        }
                    }
                }
                assert!(corrupted);
                assert_eq!(executor.calls, 0);
                assert!(driver.frames.is_empty());
                assert_eq!(
                    p.domain_get(contract().domain).unwrap().state,
                    ucn_persistence::DomainState::Fault
                );
                assert!(!p.domain_get(contract().domain).unwrap().pending);
                let mut quiet = false;
                for now in 1200..1300 {
                    if step(
                        &mut runtime,
                        now,
                        budget,
                        server,
                        sc,
                        sh,
                        sf,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        link,
                        &mut executor,
                        &mut sequence,
                        &mut transaction,
                    )
                    .quiescent
                    {
                        quiet = true;
                        break;
                    }
                }
                assert!(quiet);
                assert!(
                    runtime
                        .operation()
                        .receipt(&mut DigestWorkspace::new())
                        .is_err()
                );
            });
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Same-generation fabricated restore is independently checked against real media.
fn same_generation_different_restore_digest_is_rejected_before_any_work() {
    security::exercise_opcodes(1, 2, OPS, |client, ch, cf, cc| {
        security::exercise_opcodes(2, 1, OPS, |server, sh, sf, sc| {
            flash::exercise(contract(), |p, f| {
                let ingress = AdapterEventIngress::<1, 2, 2, 128>::new(2).unwrap();
                let mut adapter = AdapterOwner::new(2, &ingress).unwrap();
                let link = adapter.open_link(0, 1, 128).unwrap();
                adapter.try_set_rx_enabled(true).unwrap();
                let mut runtime = runtime();
                let gate = server.callback_gate();
                let mut executor = Executor { calls: 0, gate };
                let mut driver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                let mut sequence = SequenceCounter(20);
                let mut transaction = Counter(1);
                let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
                let raw = request(client, cc, ch, cf, 10);
                ingress
                    .rx_publish(
                        link,
                        &raw,
                        RxMeta {
                            timestamp_us: 21,
                            sender_discriminator: 1,
                        },
                    )
                    .unwrap();
                for now in 21..130 {
                    step(
                        &mut runtime,
                        now,
                        budget,
                        server,
                        sc,
                        sh,
                        sf,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        link,
                        &mut executor,
                        &mut sequence,
                        &mut transaction,
                    );
                    if runtime.operation().view().0 == Some(OperationPhase::CommittedResult)
                        && !runtime.operation().view().1
                    {
                        break;
                    }
                }
                assert_eq!(executor.calls, 1);
                let key = runtime.operation().key().unwrap();
                let mut fake_bytes = [0; 256];
                let n = encode_operation(
                    key,
                    OperationPhase::Prepared,
                    false,
                    0,
                    &[],
                    &mut fake_bytes,
                )
                .unwrap();
                let mut fake_view = p.domain_get(contract().domain).unwrap();
                fake_view.body_bytes = u32::try_from(n).unwrap();
                let meta = RecordMeta {
                    domain: contract().domain,
                    schema_id: 1,
                    schema_version: 1,
                    record_generation: fake_view.record_generation,
                    transaction_id: fake_view.foundation_transaction_id,
                    operation_kind: 0x0701,
                    body_bytes: fake_view.body_bytes,
                    body_digest: [0; 16],
                };
                fake_view.body_digest =
                    body_digest(&meta, &fake_bytes[..n], &mut CodecWorkspace::new()).unwrap();
                let base = DurableBase::from_view(
                    contract(),
                    fake_view,
                    &fake_bytes[..n],
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
                let mut restored = OperationOwner::new(contract()).unwrap();
                restored
                    .restore(base, &fake_bytes[..n], [5; 16], &mut CodecWorkspace::new())
                    .unwrap();
                assert_eq!(
                    restored.record_generation(),
                    runtime.operation().record_generation()
                );
                assert_ne!(
                    restored.record_digest(),
                    runtime.operation().record_digest()
                );
                let mut invalid = Runtime::new(
                    config(),
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
                    restored,
                )
                .unwrap();
                let calls = f.calls;
                let sends = driver.frames.len();
                assert_eq!(
                    invalid.step(
                        budget,
                        peer(sh, sf, link, 200),
                        server,
                        sc,
                        p,
                        f,
                        &mut adapter,
                        &mut driver,
                        &mut sequence,
                        &mut transaction,
                        &mut executor
                    ),
                    Err(Error::State)
                );
                assert_eq!(invalid.stats(), JournalRuntimeStats::default());
                assert_eq!(f.calls, calls);
                assert_eq!(driver.frames.len(), sends);
                assert_eq!(executor.calls, 1);
            });
        });
    });
}
