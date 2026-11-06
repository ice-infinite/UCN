#![cfg(all(feature = "network", feature = "envelope"))]
//! Actual Timed-Link callbacks, four-event transactions and Domain gates.
#[path = "../../../tests/support/flash.rs"]
mod flash;
use ucn_durable::DurableContract;
use ucn_owner::CallbackGate;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_realtime::*;
use ucn_types::{Error, Result};

struct Driver {
    time: u64,
    fail_retire: bool,
    submits: usize,
}
impl<const N: usize> TimedDriver<N> for Driver {
    fn submit(&mut self, key: EventKey, _: &[u8], ingress: &TimestampIngress<N>) -> TimedSubmit {
        self.submits += 1;
        ingress.publish(key, self.time, 2).unwrap();
        TimedSubmit::Pending
    }
    fn retire(&mut self, _: EventKey) -> Result<()> {
        if self.fail_retire {
            self.fail_retire = false;
            Err(Error::State)
        } else {
            Ok(())
        }
    }
}
fn config() -> DomainConfig {
    DomainConfig {
        domain: 1,
        generation: 1,
        lock_samples: 2,
        sync_timeout: 100,
        max_holdover: 500,
        oscillator_ppb: 100,
        max_jump: 100_000,
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
#[allow(clippy::too_many_lines)] // Full four-message/atomic-RX/retirement integration sequence.
fn sample(c: DomainConfig, seq: u32, local: u64, offset: i64) -> SyncOutcome {
    let gate = CallbackGate::new(1).unwrap();
    let master_ingress = TimestampIngress::<4>::new();
    let member_ingress = TimestampIngress::<4>::new();
    let mut master_link = TimedLink::new(1, 1, &gate, &master_ingress).unwrap();
    let mut member_link = TimedLink::new(1, 2, &gate, &member_ingress).unwrap();
    let mut master = SyncOwner::<1, 2>::new(1).unwrap();
    let mut member = SyncOwner::<1, 2>::new(2).unwrap();
    let local_t2 = local - 100;
    let master_t1 = u64::try_from(i128::from(local_t2) + i128::from(offset) - 10).unwrap();
    let t1 = master_link.reserve(EventDirection::Tx).unwrap();
    let mh = master
        .master_begin(c, [2; 16], seq, t1, 1, local + 10_000)
        .unwrap();
    let mut driver = Driver {
        time: master_t1,
        fail_retire: false,
        submits: 0,
    };
    let mut sync = [0; 12];
    master.request_payload(mh, &mut sync).unwrap();
    master_link.submit(t1, &sync, &mut driver).unwrap();
    master
        .tx_completed(mh, master_link.timestamp(t1).unwrap(), local)
        .unwrap();
    let t2 = member_link.reserve(EventDirection::Rx).unwrap();
    let t2 = member_link.receive_capture(t2, local_t2, 2).unwrap();
    let queue = TimedRxQueue::<1, 32>::new(2).unwrap();
    queue.publish(t2, &sync).unwrap();
    let mut received = [0; 32];
    let claim = queue.claim(&mut received).unwrap();
    assert_eq!(&received[..claim.bytes()], &sync);
    let t3 = member_link.reserve(EventDirection::Tx).unwrap();
    let key = TimeKey {
        domain: c.domain,
        generation: c.generation,
        sequence: seq,
    };
    let nh = member
        .member_begin(
            c,
            [1; 16],
            decode_time_key(&received[..claim.bytes()]).unwrap(),
            claim.stamp(),
            t3,
            local,
            10_000,
        )
        .unwrap();
    driver.time = local_t2 + 20;
    let mut request = [0; 12];
    member.request_payload(nh, &mut request).unwrap();
    assert_eq!(request, sync);
    member_link.submit(t3, &request, &mut driver).unwrap();
    let mut follow_up = [0; 24];
    master.follow_up_payload(mh, local, &mut follow_up).unwrap();
    member
        .accept_follow_up(nh, [1; 16], c.path, &follow_up, local)
        .unwrap();
    assert!(
        member
            .member_response(
                nh,
                [1; 16],
                c.path,
                DelayResponse {
                    key,
                    t1: master_t1,
                    t4: master_t1 + 40,
                    t1_bound: 2,
                    t4_bound: 2
                },
                local
            )
            .is_err()
    );
    member
        .tx_completed(nh, member_link.timestamp(t3).unwrap(), local)
        .unwrap();
    let t4 = master_link.reserve(EventDirection::Rx).unwrap();
    let t4 = master_link.receive_capture(t4, master_t1 + 40, 2).unwrap();
    let response = master
        .master_response(mh, key, [2; 16], c.path, t4, local.max(master_t1 + 40))
        .unwrap();
    let mut raw_response = [0; 24];
    encode_time_stamp(
        TimeStampMessage {
            key: response.key,
            time: response.t4,
            bound: response.t4_bound,
        },
        &mut raw_response,
    )
    .unwrap();
    let outcome = member
        .delay_response_payload(nh, [1; 16], c.path, &raw_response, local)
        .unwrap();
    // Release keys are not popped until the Driver confirms retirement.
    let release = member.release_peek().unwrap();
    driver.fail_retire = true;
    member_link.release_pending(release).unwrap();
    assert_eq!(member_link.retire(release, &mut driver), Err(Error::State));
    assert_eq!(member.release_peek(), Some(release));
    let receipt = member_link.retire_receipt(release, &mut driver).unwrap();
    member.release_ack(receipt).unwrap();
    assert_ne!(member.release_peek(), Some(release));
    let release = member.release_peek().unwrap();
    assert_eq!(release, claim.stamp().key());
    let proof = member_link.retire_receipt(release, &mut driver).unwrap();
    queue.release(claim, proof).unwrap();
    member.release_ack(proof).unwrap();
    assert!(member.release_peek().is_none());
    assert_eq!(driver.submits, 2);
    outcome
}
#[test]
fn real_four_timestamp_samples_and_immediate_regression_fault() {
    flash::exercise(contract(), |persistence, provider| {
        let mut domain = TimeDomain::<5>::new(contract()).unwrap();
        let c = config();
        domain
            .rotate(
                flash::base(persistence, contract()),
                c,
                1,
                10_000,
                1,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
        let (h, proof) = flash::commit(persistence, provider, &domain.requirement().unwrap());
        domain.bind(h).unwrap();
        domain.confirm(h, proof).unwrap();
        persistence.proof_retire(h).unwrap();
        let mut unrestored = TimeDomain::<5>::new(contract()).unwrap();
        assert_eq!(
            unrestored.rotate(
                flash::base(persistence, contract()),
                c,
                21,
                10000,
                2,
                &mut CodecWorkspace::new()
            ),
            Err(Error::State)
        );
        assert!(unrestored.requirement().is_err());
        for (seq, local) in [(1, 1200), (2, 1300)] {
            let SyncOutcome::Sample(value) = sample(c, seq, local, -90) else {
                panic!()
            };
            assert_eq!(value.offset_us(), -90);
            domain.ingest(value).unwrap();
        }
        let published = domain.clock(1300, false).unwrap();
        let first = domain.allocate_sync_sequence().unwrap().consume(c).unwrap();
        assert_eq!(first, 1);
        assert_eq!(published.now, 1210);
        assert!(
            domain
                .make_stamp(1300, 1300, 0, TimedMode::Deadline, false)
                .is_err()
        );
        let stamp = domain
            .make_stamp(1300, 1300, 1025, TimedMode::Deadline, false)
            .unwrap();
        assert!(stamp.uncertainty_class >= 11);
        domain.step(2000).unwrap();
        assert_eq!(domain.view().0, DomainPhase::Unsynced);
        assert_eq!(domain.view().1, 0);
        assert_eq!(
            domain.allocate_sync_sequence().unwrap().consume(c).unwrap(),
            2
        );
        let SyncOutcome::Sample(value) = sample(c, 3, 2100, -1000) else {
            panic!()
        };
        assert_eq!(domain.ingest(value), Err(Error::State));
        assert_eq!(domain.view().0, DomainPhase::Fault);
    });
}

#[test]
fn atomic_capture_threads_and_cross_instance_ingress_never_mix_keys() {
    let gate = CallbackGate::new(1).unwrap();
    let ingress = TimestampIngress::<1>::new();
    let foreign_ingress = TimestampIngress::<1>::new();
    let mut link = TimedLink::new(1, 1, &gate, &ingress).unwrap();
    let mut foreign = TimedLink::new(1, 2, &gate, &foreign_ingress).unwrap();
    let key = link.reserve(EventDirection::Tx).unwrap();
    let other = foreign.reserve(EventDirection::Tx).unwrap();
    assert!(ingress.publish(other, 123, 2).is_err());
    let mut driver = Driver {
        time: 123,
        fail_retire: false,
        submits: 0,
    };
    link.submit(key, b"frame", &mut driver).unwrap();
    std::thread::scope(|scope| {
        for _ in 0..4 {
            scope.spawn(|| {
                for _ in 0..1000 {
                    assert!(ingress.publish(key, 123, 2).is_ok());
                }
            });
        }
    });
    assert_eq!(link.timestamp(key).unwrap().time_us(), 123);
    link.retire(key, &mut driver).unwrap();
    assert!(ingress.publish(key, 123, 2).is_err());
}
#[test]
fn uncertainty_deadline_acl_future_and_source_holdover_boundaries() {
    assert_eq!(uncertainty_class(1025).unwrap(), 11);
    let e = Envelope {
        mode: TimedMode::Deadline,
        uncertainty_class: 6,
        hardware_capture: false,
        domain_valid: true,
        holdover: false,
        domain: 1,
        generation: 1,
        capture_us: 1000,
    };
    let mut out = [0xa5; 16];
    encode_envelope(e, &mut out).unwrap();
    assert_eq!(decode_envelope(&out).unwrap(), e);
    let c = ClockView {
        domain: 1,
        generation: 1,
        now: 1000,
        uncertainty: 60,
        holdover: false,
    };
    let p = TimedPolicy {
        required: true,
        max_uncertainty: 100,
        max_age: 200,
        allow_remote_holdover: false,
    };
    assert_eq!(prequeue(e, c, p, true, true, None), Err(Error::Policy));
    let p = TimedPolicy {
        max_uncertainty: 124,
        ..p
    };
    let admission = prequeue(e, c, p, true, true, None).unwrap();
    assert_eq!(prequeue(e, c, p, true, false, None), Err(Error::Access));
    assert_eq!(
        preexecute(admission, ClockView { now: 1076, ..c }, true, true),
        Err(Error::Timeout)
    );
    assert!(preexecute(admission, ClockView { now: 1075, ..c }, true, true).is_ok());
    assert_eq!(
        prequeue(
            Envelope {
                capture_us: 1125,
                ..e
            },
            c,
            p,
            true,
            true,
            None
        ),
        Err(Error::Argument)
    );
    assert_eq!(
        prequeue(
            Envelope {
                holdover: true,
                ..e
            },
            c,
            p,
            true,
            true,
            None
        ),
        Err(Error::Policy)
    );
}
#[test]
fn unknown_asymmetry_is_diagnostic_and_dynamic_route_creates_no_pending() {
    let c = config();
    assert!(matches!(
        sample(
            DomainConfig {
                path: TimePath {
                    asymmetry: None,
                    ..c.path
                },
                ..c
            },
            1,
            1200,
            -90
        ),
        SyncOutcome::Diagnostic { .. }
    ));
    let mut sync = SyncOwner::<1, 2>::new(1).unwrap();
    let gate = CallbackGate::new(1).unwrap();
    let ingress = TimestampIngress::<2>::new();
    let mut link = TimedLink::new(1, 1, &gate, &ingress).unwrap();
    let key = link.reserve(EventDirection::Tx).unwrap();
    let dynamic = DomainConfig {
        path: TimePath {
            frozen: false,
            ..c.path
        },
        ..c
    };
    assert_eq!(
        sync.master_begin(dynamic, [2; 16], 1, key, 1, 1000),
        Err(Error::Policy)
    );
    assert_eq!(sync.release_peek(), None);
}

#[test]
fn understated_asymmetry_and_zero_oscillator_reject_before_pending() {
    let c = config();
    let gate = CallbackGate::new(1).unwrap();
    let ingress = TimestampIngress::<2>::new();
    let mut link = TimedLink::new(1, 1, &gate, &ingress).unwrap();
    let tx = link.reserve(EventDirection::Tx).unwrap();
    let mut owner = SyncOwner::<1, 2>::new(1).unwrap();
    let smaller = DomainConfig {
        path: TimePath {
            asymmetry: Some(2),
            ..c.path
        },
        ..c
    };
    assert!(encode_domain(smaller).is_err());
    assert!(
        owner
            .master_begin(smaller, [2; 16], 1, tx, 1, 1000)
            .is_err()
    );
    assert_eq!(owner.release_peek(), None);
    assert!(
        owner
            .master_begin(
                DomainConfig {
                    oscillator_ppb: 0,
                    ..c
                },
                [2; 16],
                1,
                tx,
                1,
                1000
            )
            .is_err()
    );
    assert!(owner.master_begin(c, [2; 16], 1, tx, 1, 1000).is_ok());
}
#[test]
fn exchange_and_response_processing_drift_is_not_omitted() {
    let c = DomainConfig {
        oscillator_ppb: 1_000_000,
        ..config()
    };
    let SyncOutcome::Sample(value) = sample(c, 1, 1200, -90) else {
        panic!("missing sample");
    };
    // T2=1100, sample at1200: two clocks *100us*1e6ppb => ceil(0.2)=1us.
    assert!(value.uncertainty_us() > 6 + 8 + 1);
}
#[test]
fn token_reuse_and_cross_link_callback_gate() {
    let gate = CallbackGate::new(1).unwrap();
    let i1 = TimestampIngress::<2>::new();
    let i2 = TimestampIngress::<2>::new();
    let mut one = TimedLink::new(1, 1, &gate, &i1).unwrap();
    let mut two = TimedLink::new(2, 2, &gate, &i2).unwrap();
    let key = one.reserve(EventDirection::Tx).unwrap();
    let mut driver = Driver {
        time: 1,
        fail_retire: false,
        submits: 0,
    };
    one.submit(key, b"a", &mut driver).unwrap();
    assert_eq!(one.submit(key, b"a", &mut driver), Err(Error::State));
    let lease = gate
        .try_enter(ucn_owner::CallbackClaim::new(3, 1, 1, 1).unwrap())
        .unwrap();
    assert_eq!(two.reserve(EventDirection::Rx), Err(Error::State));
    assert_eq!(two.outstanding(), 0);
    gate.leave(lease).unwrap();
    one.reopen().unwrap();
    assert!(one.timestamp(key).is_err());
    one.retire(key, &mut driver).unwrap();
    assert_eq!(one.outstanding(), 0);
}

#[test]
fn atomic_rx_full_short_duplicate_stale_and_concurrent_release() {
    let gate = CallbackGate::new(1).unwrap();
    let ingress = TimestampIngress::<2>::new();
    let mut link = TimedLink::new(1, 1, &gate, &ingress).unwrap();
    let queue = TimedRxQueue::<1, 16>::new(1).unwrap();
    let key = link.reserve(EventDirection::Rx).unwrap();
    let stamp = link.receive_capture(key, 100, 2).unwrap();
    queue.publish(stamp, b"original-frame").unwrap();
    assert_eq!(queue.publish(stamp, b"duplicate"), Err(Error::State));
    let mut short = [0xa5; 2];
    assert_eq!(queue.claim(&mut short), Err(Error::NoSpace));
    assert_eq!(short, [0xa5; 2]);
    let mut bytes = [0; 16];
    let old = queue.claim(&mut bytes).unwrap();
    assert_eq!(&bytes[..old.bytes()], b"original-frame");
    let other = link.reserve(EventDirection::Rx).unwrap();
    let next_stamp = link.receive_capture(other, 101, 2).unwrap();
    assert_eq!(queue.publish(next_stamp, b"next"), Err(Error::NoSpace));
    let mut driver = Driver {
        time: 0,
        fail_retire: true,
        submits: 0,
    };
    assert_eq!(link.retire_receipt(key, &mut driver), Err(Error::State));
    assert_eq!(queue.claim(&mut bytes), Err(Error::NoSpace));
    let proof = link.retire_receipt(key, &mut driver).unwrap();
    let successes = std::sync::atomic::AtomicUsize::new(0);
    std::thread::scope(|scope| {
        for _ in 0..4 {
            scope.spawn(|| {
                if queue.release(old, proof).is_ok() {
                    successes.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                }
            });
        }
    });
    assert_eq!(successes.load(std::sync::atomic::Ordering::Relaxed), 1);
    queue.publish(next_stamp, b"next").unwrap();
    let next = queue.claim(&mut bytes).unwrap();
    assert_eq!(queue.release(next, proof), Err(Error::State));
    assert_eq!(queue.release(old, proof), Err(Error::State));
    assert_eq!(queue.claim(&mut bytes), Err(Error::NoSpace));
    let next_proof = link.retire_receipt(other, &mut driver).unwrap();
    queue.release(next, next_proof).unwrap();
}
