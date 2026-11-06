#![cfg(feature = "duplex-runtime")]
//! Two full-duplex static nodes, each with one Adapter and one two-Domain Foundation.
#[path = "../../../tests/support/transfer.rs"]
mod fixture;
#[path = "../../../tests/support/dual_flash.rs"]
mod media;
use fixture::{Counter, Driver, SequenceCounter, security};
use ucn_adapter::*;
use ucn_durable::DurableContract;
use ucn_persistence::{DomainKey, DomainKind};
use ucn_runtime::*;
use ucn_security::*;
use ucn_service::*;
use ucn_types::*;
type Node = DuplexRuntime<2, 128, 16, 16, 16>;
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
fn node(peer: u8) -> Node {
    let endpoint = |request, result| {
        RpcEndpoint::new(
            ServiceId::new(0x40).unwrap(),
            [peer; 16],
            request,
            result,
            16,
            OriginSecurity::O1,
        )
        .unwrap()
        .durable()
    };
    let source = SourceRuntime::new(
        SourceRuntimeConfig {
            owner: 20,
            id_interval: 4,
            io_timeout_us: 1000,
            recovery_lifetime_us: 1000,
            retry_us: 10,
            max_attempts: 4,
        },
        endpoint(false, true),
        OperationIds::new(contract(true), 1).unwrap(),
    )
    .unwrap();
    let server = JournalRuntime::new(
        JournalRuntimeConfig {
            owner: 10,
            request_lifetime_us: 3000,
            control_lifetime_us: 1000,
            io_timeout_us: 1000,
            retry_us: 10,
            max_attempts: 4,
            retention_us: 2,
        },
        endpoint(true, false),
        OperationOwner::new(contract(false)).unwrap(),
    )
    .unwrap();
    DuplexRuntime::new(30, source, server).unwrap()
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
struct Execute {
    calls: usize,
    expected: &'static [u8],
}
impl DurableExecutor for Execute {
    fn execute(&mut self, key: OperationKey, args: &[u8], out: &mut [u8]) -> Result<(i32, usize)> {
        assert_eq!(key.service.operation, 1);
        assert_eq!(args, self.expected);
        self.calls += 1;
        out[..self.expected.len()].copy_from_slice(self.expected);
        Ok((7, self.expected.len()))
    }
}
fn publish(ingress: &AdapterEventIngress<1, 2, 4, 128>, link: LinkHandle, bytes: &[u8], now: u64) {
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
#[test]
#[allow(clippy::too_many_lines)] // Concurrent real bidirectional bytes/IDs/proofs/ACKs on shared links.
fn simultaneous_bidirectional_requests_share_ingress_budget_and_media_without_cross_binding() {
    security::exercise_opcodes(1, 2, OPS, |left_sec, lh, lf, lc| {
        security::exercise_opcodes(2, 1, OPS, |right_sec, rh, rf, rc| {
            media::exercise(contract(true), contract(false), |lp, lm| {
                media::exercise(contract(true), contract(false), |rp, rm| {
                    let li = AdapterEventIngress::<1, 2, 4, 128>::new(1).unwrap();
                    let ri = AdapterEventIngress::<1, 2, 4, 128>::new(2).unwrap();
                    let mut la = AdapterOwner::new(1, &li).unwrap();
                    let ll = la.open_link(0, 1, 128).unwrap();
                    la.try_set_rx_enabled(true).unwrap();
                    let mut ra = AdapterOwner::new(2, &ri).unwrap();
                    let rl = ra.open_link(0, 1, 128).unwrap();
                    ra.try_set_rx_enabled(true).unwrap();
                    let mut left = node(2);
                    let mut right = node(1);
                    left.request(b"from-one", 21, 2000).unwrap();
                    right.request(b"from-two", 21, 2000).unwrap();
                    let mut ld = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                    let mut rd = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
                    let mut ls = SequenceCounter(1);
                    let mut rs = SequenceCounter(1);
                    let mut lt = Counter(1);
                    let mut rt = Counter(1);
                    let mut le = Execute {
                        calls: 0,
                        expected: b"from-two",
                    };
                    let mut re = Execute {
                        calls: 0,
                        expected: b"from-one",
                    };
                    let budget = JournalStepBudget::new(7, [1; 7]).unwrap();
                    let mut sent_l = 0;
                    let mut sent_r = 0;
                    lm.domains[0].pending_once = true;
                    rm.domains[1].pending_once = true;
                    for now in 21..1600 {
                        let before: usize = lm.domains.iter().map(|f| f.calls).sum();
                        let report = left
                            .step(
                                budget,
                                peer(lh, lf, ll, now),
                                left_sec,
                                lc,
                                lp,
                                lm,
                                &mut la,
                                &mut ld,
                                &mut ls,
                                &mut lt,
                                &mut le,
                            )
                            .unwrap();
                        assert_eq!(report.visits, [1; 7]);
                        let after: usize = lm.domains.iter().map(|f| f.calls).sum();
                        assert!(after - before <= 1); // One GLOBAL Foundation visit, not one per half.
                        for bytes in &ld.frames[sent_l..] {
                            publish(&ri, rl, bytes, now);
                        }
                        sent_l = ld.frames.len();
                        right
                            .step(
                                budget,
                                peer(rh, rf, rl, now),
                                right_sec,
                                rc,
                                rp,
                                rm,
                                &mut ra,
                                &mut rd,
                                &mut rs,
                                &mut rt,
                                &mut re,
                            )
                            .unwrap();
                        for bytes in &rd.frames[sent_r..] {
                            publish(&li, ll, bytes, now);
                        }
                        sent_r = rd.frames.len();
                        if left.server_operation().view().0 == Some(OperationPhase::Tombstoned)
                            && right.server_operation().view().0 == Some(OperationPhase::Tombstoned)
                        {
                            break;
                        }
                    }
                    let mut lo = [0; 16];
                    let mut ro = [0; 16];
                    assert_eq!(left.copy_result(&mut lo).unwrap().bytes, 8);
                    assert_eq!(&lo[..8], b"from-one");
                    assert_eq!(right.copy_result(&mut ro).unwrap().bytes, 8);
                    assert_eq!(&ro[..8], b"from-two");
                    assert_eq!(le.calls, 1);
                    assert_eq!(re.calls, 1);
                    assert_eq!(lm.domains[0].witness, 1);
                    assert_eq!(rm.domains[0].witness, 1);
                    assert_eq!(lm.domains[1].witness, 4);
                    assert_eq!(rm.domains[1].witness, 4);
                    assert_eq!(left.operation_key().unwrap().service.client, [1; 16]);
                    assert_eq!(
                        left.server_operation().key().unwrap().service.client,
                        [2; 16]
                    );
                    // Queued unclaimed frames also prevent a false stop/quiescence claim.
                    publish(&li, ll, &[0; 3], 3500);
                    publish(&li, ll, &[0; 3], 3500);
                    left.stop();
                    right.stop();
                    let mut quiet_l = false;
                    let mut quiet_r = false;
                    for now in 3500..3800 {
                        let lr = left
                            .step(
                                budget,
                                peer(lh, lf, ll, now),
                                left_sec,
                                lc,
                                lp,
                                lm,
                                &mut la,
                                &mut ld,
                                &mut ls,
                                &mut lt,
                                &mut le,
                            )
                            .unwrap();
                        if now == 3500 {
                            assert!(!lr.quiescent);
                        }
                        quiet_l |= lr.quiescent;
                        quiet_r |= right
                            .step(
                                budget,
                                peer(rh, rf, rl, now),
                                right_sec,
                                rc,
                                rp,
                                rm,
                                &mut ra,
                                &mut rd,
                                &mut rs,
                                &mut rt,
                                &mut re,
                            )
                            .unwrap()
                            .quiescent;
                        if quiet_l && quiet_r {
                            break;
                        }
                    }
                    assert!(quiet_l && quiet_r);
                    assert!(la.rx_is_quiescent());
                    assert!(
                        li.rx_publish(
                            ll,
                            &[0; 3],
                            RxMeta {
                                timestamp_us: 3800,
                                sender_discriminator: 1
                            }
                        )
                        .is_err()
                    );
                    assert_eq!(le.calls, 1);
                    assert_eq!(re.calls, 1);
                });
            });
        });
    });
}
#[test]
#[allow(clippy::too_many_lines)] // Budget-one global clock remains independent of child visit order.
fn shared_ingress_only_steps_still_enforce_global_clock_and_fairness() {
    security::exercise_opcodes(1, 2, OPS, |sec, sh, sf, crypto| {
        media::exercise(contract(true), contract(false), |p, m| {
            let ingress = AdapterEventIngress::<1, 2, 4, 128>::new(1).unwrap();
            let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
            let link = adapter.open_link(0, 1, 128).unwrap();
            adapter.try_set_rx_enabled(true).unwrap();
            let mut node = node(2);
            let mut driver = Driver::new(DriverSubmit::NotSubmitted);
            let mut sequence = SequenceCounter(1);
            let mut tx = Counter(1);
            let mut executor = Execute {
                calls: 0,
                expected: b"never",
            };
            let budget = JournalStepBudget::new(1, [1; 7]).unwrap();
            for now in 100..240 {
                if now % 7 == 2 {
                    publish(&ingress, link, &[0; 3], now);
                }
                let r = node
                    .step(
                        budget,
                        peer(sh, sf, link, now),
                        sec,
                        crypto,
                        p,
                        m,
                        &mut adapter,
                        &mut driver,
                        &mut sequence,
                        &mut tx,
                        &mut executor,
                    )
                    .unwrap();
                assert_eq!(r.visits.iter().sum::<u16>(), 1);
            }
            assert_eq!(node.stats().visits, [20; 7]);
            let before = node.stats();
            assert_eq!(
                node.step(
                    budget,
                    peer(sh, sf, link, 99),
                    sec,
                    crypto,
                    p,
                    m,
                    &mut adapter,
                    &mut driver,
                    &mut sequence,
                    &mut tx,
                    &mut executor
                ),
                Err(Error::State)
            );
            assert_eq!(node.stats(), before);
            assert_eq!(executor.calls, 0);
            assert!(driver.frames.is_empty());
        });
    });
}
