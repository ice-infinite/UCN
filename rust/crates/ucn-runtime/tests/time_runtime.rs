#![cfg(feature = "time-runtime")]
//! Actual durable domains -> authenticated four C0 frames -> physical events -> sample.
//! 实际耐久域、四个认证C0报文、物理事件、真实样本及退休失败重试。
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
use fixture::{Counter, security};
use ucn_durable::DurableContract;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_realtime::*;
use ucn_runtime::*;
use ucn_security::*;
use ucn_types::*;
const OPS: &[ProtocolOpcode] = &[
    ProtocolOpcode::TimeSync,
    ProtocolOpcode::TimeFollowUp,
    ProtocolOpcode::TimeDelayRequest,
    ProtocolOpcode::TimeDelayResponse,
];
fn contract() -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::TimeAuthority,
            id: 6,
        },
        schema: 0x0801,
        version: 1,
        operation: 0x0801,
    }
}
fn config() -> DomainConfig {
    DomainConfig {
        domain: 1,
        generation: 1,
        lock_samples: 2,
        sync_timeout: 1000,
        max_holdover: 5000,
        oscillator_ppb: 100,
        max_jump: 10000,
        uncertainty: Uncertainty {
            timer: Some(1),
            link: Some(1),
            filter: Some(1),
            rounding: Some(1),
            capture: Some(1),
            asymmetry: Some(1),
        },
        path: TimePath {
            causal: 1,
            route: 1,
            session: 1,
            capability: 1,
            forward_generation: 1,
            reverse_generation: 1,
            forward_link: 1,
            reverse_link: 1,
            capability_digest: [1; 16],
            frozen: true,
            asymmetry: Some(1),
        },
    }
}
fn domain(p: &mut security::flash::Owner<'_>, flash: &mut security::flash::Flash) -> TimeDomain<5> {
    let mut d = TimeDomain::new(contract()).unwrap();
    d.rotate(
        security::flash::base(p, contract()),
        config(),
        1,
        10000,
        1,
        &mut CodecWorkspace::new(),
    )
    .unwrap();
    let (h, proof) = security::flash::commit(p, flash, &d.requirement().unwrap());
    d.bind(h).unwrap();
    d.confirm(h, proof).unwrap();
    assert!(time_domain_proof(&d, p).is_err());
    p.proof_retire(h).unwrap();
    d
}
fn peer(h: SessionHandle, facts: CurrentFacts, now: u64) -> TimePeer {
    TimePeer {
        session: h,
        facts: CurrentFacts {
            now_us: now,
            ..facts
        },
        fingerprint: Fingerprint::new([5; 16]).unwrap(),
    }
}
struct Driver {
    time: u64,
    fail_retire: bool,
    disposition: u8,
    frames: Vec<Vec<u8>>,
    retires: usize,
    key: Option<EventKey>,
}
impl Driver {
    fn new() -> Self {
        Self {
            time: 1,
            fail_retire: false,
            disposition: 0,
            frames: vec![],
            retires: 0,
            key: None,
        }
    }
}
impl TimedDriver<8> for Driver {
    fn submit(
        &mut self,
        key: EventKey,
        bytes: &[u8],
        ingress: &TimestampIngress<8>,
    ) -> TimedSubmit {
        self.frames.push(bytes.to_vec());
        self.key = Some(key);
        match self.disposition {
            1 => TimedSubmit::NotSubmitted,
            2 => TimedSubmit::Unknown,
            3 => TimedSubmit::Pending,
            _ => {
                ingress.publish(key, self.time, 2).unwrap();
                TimedSubmit::Pending
            }
        }
    }
    fn retire(&mut self, _: EventKey) -> Result<()> {
        self.retires += 1;
        if self.fail_retire {
            self.fail_retire = false;
            Err(Error::State)
        } else {
            Ok(())
        }
    }
}
#[allow(clippy::too_many_arguments)]
fn receive(
    runtime: &mut TimeRuntime<128>,
    proof: &TimeDomainProof<'_, 5>,
    peer: TimePeer,
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    link: &mut TimedLink<'_, 8>,
    queue: &TimedRxQueue<4, 128>,
    raw: &[u8],
    capture: u64,
) -> Result<()> {
    let key = link.reserve(EventDirection::Rx)?;
    let stamp = link.receive_capture(key, capture, 2)?;
    queue.publish(stamp, raw)?;
    runtime.receive(
        proof,
        peer,
        security,
        crypto,
        &mut SecurityPacketWorkspace::<128>::new(),
        link,
        queue,
    )
}
#[allow(clippy::too_many_arguments)]
fn drive(
    runtime: &mut TimeRuntime<128>,
    proof: &TimeDomainProof<'_, 5>,
    peer: TimePeer,
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    counter: &mut Counter,
    link: &mut TimedLink<'_, 8>,
    driver: &mut Driver,
) -> Result<bool> {
    runtime.drive(
        proof,
        peer,
        security,
        crypto,
        counter,
        &mut SecurityPacketWorkspace::<128>::new(),
        link,
        driver,
        128,
    )
}
#[test]
#[allow(clippy::too_many_lines)] // Two complete independently authenticated physical exchanges.
fn actual_authenticated_four_message_pipeline_locks_and_retires_without_loss() {
    security::exercise_opcodes(1, 2, OPS, |ms, mh, mf, mc| {
        security::exercise_opcodes(2, 1, OPS, |ns, nh, nf, nc| {
            security::flash::exercise(contract(), |mp, mflash| {
                security::flash::exercise(contract(), |np, nflash| {
                    let mut md = domain(mp, mflash);
                    let mut nd = domain(np, nflash);
                    let mi = TimestampIngress::<8>::new();
                    let ni = TimestampIngress::<8>::new();
                    let mut ml = TimedLink::new(1, 10, ms.callback_gate(), &mi).unwrap();
                    let mut nl = TimedLink::new(1, 11, ns.callback_gate(), &ni).unwrap();
                    let mq = TimedRxQueue::<4, 128>::new(10).unwrap();
                    let nq = TimedRxQueue::<4, 128>::new(11).unwrap();
                    let service = ServiceId::new(0x40).unwrap();
                    let mut m = TimeRuntime::<128>::new(
                        30,
                        TimeRuntimeRole::Master,
                        [1; 16],
                        &time_domain_proof(&md, mp).unwrap(),
                        peer(mh, mf, 21),
                        ms,
                        &ml,
                        10,
                        service,
                        500,
                    )
                    .unwrap();
                    let mut n = TimeRuntime::<128>::new(
                        31,
                        TimeRuntimeRole::Member,
                        [1; 16],
                        &time_domain_proof(&nd, np).unwrap(),
                        peer(nh, nf, 21),
                        ns,
                        &nl,
                        11,
                        service,
                        500,
                    )
                    .unwrap();
                    let mut mt = Counter(1);
                    let mut nt = Counter(1);
                    let mut mdriver = Driver::new();
                    let mut ndriver = Driver::new();
                    for offset in [0, 100] {
                        let permit = md.allocate_sync_sequence().unwrap();
                        let mproof = time_domain_proof(&md, mp).unwrap();
                        let nproof = time_domain_proof(&nd, np).unwrap();
                        m.begin_master(&mproof, permit, peer(mh, mf, 21 + offset), ms, &ml)
                            .unwrap();
                        mdriver.time = 30 + offset;
                        drive(
                            &mut m,
                            &mproof,
                            peer(mh, mf, 30 + offset),
                            ms,
                            mc,
                            &mut mt,
                            &mut ml,
                            &mut mdriver,
                        )
                        .unwrap();
                        let sync = mdriver.frames.last().unwrap().clone();
                        assert_eq!(sync.len(), 57); // Existing C0 A1=29 + TimeKey12 + OriginTag16.
                        receive(
                            &mut n,
                            &nproof,
                            peer(nh, nf, 45 + offset),
                            ns,
                            nc,
                            &mut nl,
                            &nq,
                            &sync,
                            40 + offset,
                        )
                        .unwrap();
                        drive(
                            &mut m,
                            &mproof,
                            peer(mh, mf, 31 + offset),
                            ms,
                            mc,
                            &mut mt,
                            &mut ml,
                            &mut mdriver,
                        )
                        .unwrap();
                        mdriver.time = 50 + offset;
                        drive(
                            &mut m,
                            &mproof,
                            peer(mh, mf, 50 + offset),
                            ms,
                            mc,
                            &mut mt,
                            &mut ml,
                            &mut mdriver,
                        )
                        .unwrap();
                        let fu = mdriver.frames.last().unwrap().clone();
                        assert_eq!(fu.len(), 69);
                        drive(
                            &mut m,
                            &mproof,
                            peer(mh, mf, 52 + offset),
                            ms,
                            mc,
                            &mut mt,
                            &mut ml,
                            &mut mdriver,
                        )
                        .unwrap();
                        receive(
                            &mut n,
                            &nproof,
                            peer(nh, nf, 55 + offset),
                            ns,
                            nc,
                            &mut nl,
                            &nq,
                            &fu,
                            54 + offset,
                        )
                        .unwrap();
                        ndriver.time = 60 + offset;
                        drive(
                            &mut n,
                            &nproof,
                            peer(nh, nf, 60 + offset),
                            ns,
                            nc,
                            &mut nt,
                            &mut nl,
                            &mut ndriver,
                        )
                        .unwrap();
                        let request = ndriver.frames.last().unwrap().clone();
                        drive(
                            &mut n,
                            &nproof,
                            peer(nh, nf, 61 + offset),
                            ns,
                            nc,
                            &mut nt,
                            &mut nl,
                            &mut ndriver,
                        )
                        .unwrap();
                        receive(
                            &mut m,
                            &mproof,
                            peer(mh, mf, 71 + offset),
                            ms,
                            mc,
                            &mut ml,
                            &mq,
                            &request,
                            70 + offset,
                        )
                        .unwrap();
                        mdriver.time = 72 + offset;
                        drive(
                            &mut m,
                            &mproof,
                            peer(mh, mf, 72 + offset),
                            ms,
                            mc,
                            &mut mt,
                            &mut ml,
                            &mut mdriver,
                        )
                        .unwrap();
                        let response = mdriver.frames.last().unwrap().clone();
                        drive(
                            &mut m,
                            &mproof,
                            peer(mh, mf, 73 + offset),
                            ms,
                            mc,
                            &mut mt,
                            &mut ml,
                            &mut mdriver,
                        )
                        .unwrap();
                        receive(
                            &mut n,
                            &nproof,
                            peer(nh, nf, 85 + offset),
                            ns,
                            nc,
                            &mut nl,
                            &nq,
                            &response,
                            82 + offset,
                        )
                        .unwrap();
                        let Some(SyncOutcome::Sample(sample)) = n.take_outcome().unwrap() else {
                            panic!("missing genuine sample");
                        };
                        assert_eq!(sample.offset_us(), 0);
                        nd.ingest(sample).unwrap();
                        ndriver.fail_retire = true;
                        assert_eq!(n.retire(&mut nl, &mut ndriver, &nq, 6), Err(Error::State));
                        assert!(!n.quiescent());
                        for _ in 0..3 {
                            n.retire(&mut nl, &mut ndriver, &nq, 6).unwrap();
                            m.retire(&mut ml, &mut mdriver, &mq, 6).unwrap();
                        }
                        assert!(m.quiescent());
                        assert!(n.quiescent());
                        assert_eq!(ml.outstanding(), 0);
                        assert_eq!(nl.outstanding(), 0);
                    }
                    assert_eq!(md.view().1, 0);
                    assert_eq!(nd.view(), (DomainPhase::Locked, 2, 2));
                    assert_eq!(nd.clock(186, false).unwrap().now, 186);
                    assert_eq!(mt.0, 7);
                    assert_eq!(nt.0, 3);
                });
            });
        });
    });
}
#[test]
#[allow(clippy::too_many_lines)] // Failed hardware, elapsed deadline, generation and scope rejection.
fn backpressure_unknown_timeout_reopen_and_shared_gate_are_not_samples() {
    security::exercise_opcodes(1, 2, OPS, |s, h, f, c| {
        security::flash::exercise(contract(), |p, flash| {
            let mut d = domain(p, flash);
            let ingress = TimestampIngress::<8>::new();
            let mut link = TimedLink::new(1, 10, s.callback_gate(), &ingress).unwrap();
            let q = TimedRxQueue::<4, 128>::new(10).unwrap();
            let service = ServiceId::new(0x40).unwrap();
            let mut r = TimeRuntime::<128>::new(
                30,
                TimeRuntimeRole::Master,
                [1; 16],
                &time_domain_proof(&d, p).unwrap(),
                peer(h, f, 21),
                s,
                &link,
                10,
                service,
                50,
            )
            .unwrap();
            let mut driver = Driver::new();
            let mut counter = Counter(1);
            for (offset, disposition) in [(0, 1), (100, 2)] {
                let permit = d.allocate_sync_sequence().unwrap();
                let proof = time_domain_proof(&d, p).unwrap();
                r.begin_master(&proof, permit, peer(h, f, 21 + offset), s, &link)
                    .unwrap();
                driver.disposition = disposition;
                assert!(
                    drive(
                        &mut r,
                        &proof,
                        peer(h, f, 22 + offset),
                        s,
                        c,
                        &mut counter,
                        &mut link,
                        &mut driver
                    )
                    .is_err()
                );
                assert!(!r.quiescent());
                assert!(r.take_outcome().unwrap().is_none());
                driver.fail_retire = true;
                assert_eq!(r.retire(&mut link, &mut driver, &q, 6), Err(Error::State));
                for _ in 0..2 {
                    r.retire(&mut link, &mut driver, &q, 6).unwrap();
                }
                assert!(r.quiescent());
            }
            let permit = d.allocate_sync_sequence().unwrap();
            let proof = time_domain_proof(&d, p).unwrap();
            r.begin_master(&proof, permit, peer(h, f, 221), s, &link)
                .unwrap();
            assert_eq!(r.step(271), Err(Error::Timeout));
            assert!(r.quiescent());
            let claim = ucn_owner::CallbackClaim::new(99, 1, 1, 1).unwrap();
            let lease = s.callback_gate().try_enter(claim).unwrap();
            assert_eq!(r.stop(), Err(Error::State));
            assert_eq!(r.step(271), Err(Error::State));
            assert_eq!(r.take_outcome(), Err(Error::State));
            s.callback_gate().leave(lease).unwrap();
            link.reopen().unwrap();
            assert!(
                drive(
                    &mut r,
                    &proof,
                    peer(h, f, 272),
                    s,
                    c,
                    &mut counter,
                    &mut link,
                    &mut driver
                )
                .is_err()
            );
            assert_eq!(driver.frames.len(), 2);
            assert!(r.take_outcome().unwrap().is_none());
            r.stop().unwrap();
            assert!(r.quiescent());
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Independent tampering and policy mutations, every RX obligation is drained.
#[allow(clippy::many_single_char_names)] // Test-only endpoint/domain/Link aliases, not production API names.
fn wrong_domain_sequence_tag_queue_and_acl_never_become_valid_samples() {
    security::exercise_opcodes(1, 2, OPS, |ms, mh, mf, mc| {
        security::exercise_opcodes(2, 1, OPS, |ns, nh, nf, nc| {
            security::flash::exercise(contract(), |p, flash| {
                let d = domain(p, flash);
                let proof = time_domain_proof(&d, p).unwrap();
                let ingress = TimestampIngress::<8>::new();
                let mut link = TimedLink::new(1, 10, ns.callback_gate(), &ingress).unwrap();
                let queue = TimedRxQueue::<4, 128>::new(10).unwrap();
                let service = ServiceId::new(0x40).unwrap();
                let mut r = TimeRuntime::<128>::new(
                    30,
                    TimeRuntimeRole::Member,
                    [1; 16],
                    &proof,
                    peer(nh, nf, 21),
                    ns,
                    &link,
                    10,
                    service,
                    500,
                )
                .unwrap();
                let mut driver = Driver::new();
                for (tx, key) in [
                    (
                        1,
                        TimeKey {
                            domain: 2,
                            generation: 1,
                            sequence: 1,
                        },
                    ),
                    (
                        2,
                        TimeKey {
                            domain: 1,
                            generation: 2,
                            sequence: 1,
                        },
                    ),
                ] {
                    let mut body = [0; 12];
                    encode_time_key(key, &mut body).unwrap();
                    let packet =
                        fixture::protect(ms, mc, mh, mf, ProtocolOpcode::TimeSync, &body, tx);
                    assert_eq!(
                        receive(
                            &mut r,
                            &proof,
                            peer(nh, nf, 30 + tx),
                            ns,
                            nc,
                            &mut link,
                            &queue,
                            &packet.bytes[..packet.length],
                            30
                        ),
                        Err(Error::State)
                    );
                    for _ in 0..2 {
                        r.retire(&mut link, &mut driver, &queue, 6).unwrap();
                    }
                    assert!(r.quiescent());
                }
                let mut body = [0; 12];
                encode_time_key(
                    TimeKey {
                        domain: 1,
                        generation: 1,
                        sequence: 3,
                    },
                    &mut body,
                )
                .unwrap();
                let mut forged =
                    fixture::protect(ms, mc, mh, mf, ProtocolOpcode::TimeSync, &body, 3);
                forged.bytes[forged.length - 1] ^= 1;
                assert_eq!(
                    receive(
                        &mut r,
                        &proof,
                        peer(nh, nf, 40),
                        ns,
                        nc,
                        &mut link,
                        &queue,
                        &forged.bytes[..forged.length],
                        39
                    ),
                    Err(Error::Security)
                );
                for _ in 0..2 {
                    r.retire(&mut link, &mut driver, &queue, 6).unwrap();
                }
                assert!(r.quiescent());
                assert!(r.take_outcome().unwrap().is_none());
                let wrong_queue = TimedRxQueue::<4, 128>::new(11).unwrap();
                assert_eq!(
                    r.receive(
                        &proof,
                        peer(nh, nf, 41),
                        ns,
                        nc,
                        &mut SecurityPacketWorkspace::<128>::new(),
                        &mut link,
                        &wrong_queue
                    ),
                    Err(Error::State)
                );
                assert_eq!(link.outstanding(), 0);
                let good = fixture::protect(ms, mc, mh, mf, ProtocolOpcode::TimeSync, &body, 4);
                receive(
                    &mut r,
                    &proof,
                    peer(nh, nf, 45),
                    ns,
                    nc,
                    &mut link,
                    &queue,
                    &good.bytes[..good.length],
                    44,
                )
                .unwrap();
                let wrong = TimeStampMessage {
                    key: TimeKey {
                        domain: 1,
                        generation: 1,
                        sequence: 4,
                    },
                    time: 30,
                    bound: 2,
                };
                let mut stamp = [0; 24];
                encode_time_stamp(wrong, &mut stamp).unwrap();
                let fu = fixture::protect(ms, mc, mh, mf, ProtocolOpcode::TimeFollowUp, &stamp, 5);
                assert_eq!(
                    receive(
                        &mut r,
                        &proof,
                        peer(nh, nf, 46),
                        ns,
                        nc,
                        &mut link,
                        &queue,
                        &fu.bytes[..fu.length],
                        45
                    ),
                    Err(Error::State)
                );
                assert!(r.take_outcome().unwrap().is_none());
                assert_eq!(r.step(545), Err(Error::Timeout));
                for _ in 0..3 {
                    r.retire(&mut link, &mut driver, &queue, 6).unwrap();
                }
                assert!(r.quiescent());
                assert_eq!(link.outstanding(), 0);
                assert_eq!(d.view().1, 0);
            });
        });
        // Actual receiver session with no Time ACL, not a caller-provided ACL boolean.
        security::exercise_opcodes(2, 1, &[], |ns, nh, nf, nc| {
            security::flash::exercise(contract(), |p, flash| {
                let d = domain(p, flash);
                let proof = time_domain_proof(&d, p).unwrap();
                let i = TimestampIngress::<8>::new();
                let mut l = TimedLink::new(1, 10, ns.callback_gate(), &i).unwrap();
                let q = TimedRxQueue::<4, 128>::new(10).unwrap();
                let mut r = TimeRuntime::<128>::new(
                    30,
                    TimeRuntimeRole::Member,
                    [1; 16],
                    &proof,
                    peer(nh, nf, 21),
                    ns,
                    &l,
                    10,
                    ServiceId::new(0x40).unwrap(),
                    500,
                )
                .unwrap();
                let mut body = [0; 12];
                encode_time_key(
                    TimeKey {
                        domain: 1,
                        generation: 1,
                        sequence: 1,
                    },
                    &mut body,
                )
                .unwrap();
                let packet = fixture::protect(ms, mc, mh, mf, ProtocolOpcode::TimeSync, &body, 6);
                assert_eq!(
                    receive(
                        &mut r,
                        &proof,
                        peer(nh, nf, 30),
                        ns,
                        nc,
                        &mut l,
                        &q,
                        &packet.bytes[..packet.length],
                        29
                    ),
                    Err(Error::Access)
                );
                let mut driver = Driver::new();
                for _ in 0..2 {
                    r.retire(&mut l, &mut driver, &q, 6).unwrap();
                }
                assert!(r.quiescent());
                assert!(r.take_outcome().unwrap().is_none());
            });
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // No T1, exact cached Prefix, use-time MTU and callback fence.
#[allow(clippy::many_single_char_names)] // Test-only endpoint/domain/Link aliases, not production API names.
fn pending_real_t1_blocks_follow_up_and_small_mtu_does_not_burn_counter() {
    security::exercise_opcodes(1, 2, OPS, |s, h, f, c| {
        security::flash::exercise(contract(), |p, flash| {
            let mut d = domain(p, flash);
            let permit = d.allocate_sync_sequence().unwrap();
            let proof = time_domain_proof(&d, p).unwrap();
            let i = TimestampIngress::<8>::new();
            let mut l = TimedLink::new(1, 10, s.callback_gate(), &i).unwrap();
            let q = TimedRxQueue::<4, 128>::new(10).unwrap();
            let mut r = TimeRuntime::<128>::new(
                30,
                TimeRuntimeRole::Master,
                [1; 16],
                &proof,
                peer(h, f, 21),
                s,
                &l,
                10,
                ServiceId::new(0x40).unwrap(),
                500,
            )
            .unwrap();
            r.begin_master(&proof, permit, peer(h, f, 21), s, &l)
                .unwrap();
            let mut driver = Driver::new();
            driver.disposition = 3;
            let mut ct = Counter(1);
            assert_eq!(
                r.drive(
                    &proof,
                    peer(h, f, 22),
                    s,
                    c,
                    &mut ct,
                    &mut SecurityPacketWorkspace::<128>::new(),
                    &mut l,
                    &mut driver,
                    56
                ),
                Err(Error::NoSpace)
            );
            assert_eq!(ct.0, 1);
            assert_eq!(l.outstanding(), 0);
            assert!(driver.frames.is_empty());
            drive(
                &mut r,
                &proof,
                peer(h, f, 30),
                s,
                c,
                &mut ct,
                &mut l,
                &mut driver,
            )
            .unwrap();
            assert!(
                !drive(
                    &mut r,
                    &proof,
                    peer(h, f, 31),
                    s,
                    c,
                    &mut ct,
                    &mut l,
                    &mut driver
                )
                .unwrap()
            );
            assert_eq!(driver.frames.len(), 1);
            assert_eq!(ct.0, 2);
            assert!(r.take_outcome().unwrap().is_none());
            i.publish(driver.key.unwrap(), 30, 2).unwrap();
            drive(
                &mut r,
                &proof,
                peer(h, f, 32),
                s,
                c,
                &mut ct,
                &mut l,
                &mut driver,
            )
            .unwrap();
            driver.disposition = 0;
            driver.time = 33;
            drive(
                &mut r,
                &proof,
                peer(h, f, 33),
                s,
                c,
                &mut ct,
                &mut l,
                &mut driver,
            )
            .unwrap();
            assert_eq!(driver.frames.len(), 2);
            r.stop().unwrap();
            for _ in 0..3 {
                r.retire(&mut l, &mut driver, &q, 6).unwrap();
            }
            assert!(r.quiescent());
            assert_eq!(l.outstanding(), 0);
        });
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Actual media mismatch and a physically completed pre-submit timestamp.
fn different_durable_domain_and_pre_submit_timestamp_are_rejected() {
    security::exercise_opcodes(1, 2, OPS, |security, session, facts, crypto| {
        security::flash::exercise(contract(), |media, flash| {
            let mut clock = domain(media, flash);
            let ingress = TimestampIngress::<8>::new();
            let mut link = TimedLink::new(1, 10, security.callback_gate(), &ingress).unwrap();
            let queue = TimedRxQueue::<4, 128>::new(10).unwrap();
            let mut root = TimeRuntime::<128>::new(
                30,
                TimeRuntimeRole::Master,
                [1; 16],
                &time_domain_proof(&clock, media).unwrap(),
                peer(session, facts, 21),
                security,
                &link,
                10,
                ServiceId::new(0x40).unwrap(),
                500,
            )
            .unwrap();
            let other_contract = DurableContract {
                domain: DomainKey {
                    kind: DomainKind::TimeAuthority,
                    id: 7,
                },
                ..contract()
            };
            let permit = clock.allocate_sync_sequence().unwrap();
            security::flash::exercise(other_contract, |other_media, other_flash| {
                let mut other = TimeDomain::<5>::new(other_contract).unwrap();
                other
                    .rotate(
                        security::flash::base(other_media, other_contract),
                        config(),
                        1,
                        10000,
                        1,
                        &mut CodecWorkspace::new(),
                    )
                    .unwrap();
                let (h, p) = security::flash::commit(
                    other_media,
                    other_flash,
                    &other.requirement().unwrap(),
                );
                other.bind(h).unwrap();
                other.confirm(h, p).unwrap();
                other_media.proof_retire(h).unwrap();
                let proof = time_domain_proof(&other, other_media).unwrap();
                assert_eq!(
                    root.begin_master(&proof, permit, peer(session, facts, 21), security, &link),
                    Err(Error::State)
                );
                assert!(root.quiescent());
                assert_eq!(link.outstanding(), 0);
            });
            let permit = clock.allocate_sync_sequence().unwrap();
            let proof = time_domain_proof(&clock, media).unwrap();
            root.begin_master(&proof, permit, peer(session, facts, 21), security, &link)
                .unwrap();
            let mut driver = Driver::new();
            driver.time = 20;
            let mut counter = Counter(1);
            drive(
                &mut root,
                &proof,
                peer(session, facts, 30),
                security,
                crypto,
                &mut counter,
                &mut link,
                &mut driver,
            )
            .unwrap();
            assert_eq!(
                drive(
                    &mut root,
                    &proof,
                    peer(session, facts, 31),
                    security,
                    crypto,
                    &mut counter,
                    &mut link,
                    &mut driver
                ),
                Err(Error::State)
            );
            assert_eq!(driver.frames.len(), 1);
            assert!(root.take_outcome().unwrap().is_none());
            for _ in 0..3 {
                root.retire(&mut link, &mut driver, &queue, 6).unwrap();
            }
            assert!(root.quiescent());
            assert_eq!(link.outstanding(), 0);
        });
    });
}
