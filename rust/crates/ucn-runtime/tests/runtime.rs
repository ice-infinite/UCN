//! Real Security/Replay/Receipt and Driver/Adapter/Transport integration.
#[path = "../../../tests/support/security.rs"]
mod security;
use ucn_adapter::*;
use ucn_runtime::*;
use ucn_security::*;
use ucn_transport::{ReliableConfig, ReliableKey, ReliableOwner, SecurityDomain};
use ucn_types::*;
use ucn_wire::CommonHeader;
struct Counter(u32);
struct TransactionCounter(u64);
impl C0TransactionOwner for TransactionCounter {
    fn preview(&self) -> Result<C0TransactionId> {
        C0TransactionId::new(self.0)
    }
    fn burn(&mut self, expected: C0TransactionId) -> Result<()> {
        if expected.get() != self.0 {
            return Err(Error::State);
        }
        self.0 += 1;
        Ok(())
    }
}
impl OriginSequenceOwner for Counter {
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.0)
    }
    fn burn(&mut self, expected: OriginSequence) -> Result<()> {
        if expected.get() != self.0 {
            return Err(Error::State);
        }
        self.0 += 1;
        Ok(())
    }
}
fn reliable() -> ReliableOwner<2, 2, 128> {
    ReliableOwner::new(ReliableConfig {
        owner: 1,
        retry_us: 10,
        max_attempts: 4,
        receipt_retention_us: 1000,
    })
    .unwrap()
}
#[test]
#[allow(clippy::too_many_lines, clippy::items_after_statements)] // One readable multi-owner integration sequence.
fn real_security_receipt_replay_and_tamper_have_atomic_application_publication() {
    security::exercise(1, 2, |sender, sh, sf, sc| {
        security::exercise(2, 1, |receiver, rh, rf, rc| {
            let mut prefix = [0u8; 13];
            CommonHeader {
                contract: HeaderContract::C1,
                origin_security: OriginSecurity::O1,
                delivery: DeliveryGuarantee::Reliable,
                interaction: InteractionRole::OneWay,
                payload_kind: PayloadKind::Data,
                traffic_class: TrafficClass::Q2,
                hop_limit: HopLimit::new(8).unwrap(),
            }
            .encode(&mut prefix);
            prefix[3..5].copy_from_slice(&1u16.to_be_bytes());
            prefix[5..7].copy_from_slice(&2u16.to_be_bytes());
            prefix[7..9].copy_from_slice(&0x40u16.to_be_bytes());
            prefix[9..13].copy_from_slice(&1u32.to_be_bytes());
            let mut identity = [0; 46];
            identity[..16].copy_from_slice(&sf.local.principal);
            identity[16..20].copy_from_slice(&sf.local.generation.get().to_be_bytes());
            identity[20..36].copy_from_slice(&sf.peer.principal);
            identity[36..40].copy_from_slice(&sf.peer.generation.get().to_be_bytes());
            identity[40..46].copy_from_slice(&prefix[7..13]);
            let fp = Fingerprint::new([5; 16]).unwrap();
            let context = OriginContext::Sequenced {
                contract: HeaderContract::C1,
                fingerprint: fp,
                sequence: OriginSequence::new(1).unwrap(),
            };
            let sa = AccessRequest {
                session: sh,
                context_fingerprint: fp,
                service: ServiceId::new(0x40).unwrap(),
                protocol_opcode: 0,
                direction: AccessDirection::Outbound,
            };
            let ra = AccessRequest {
                session: rh,
                direction: AccessDirection::Inbound,
                ..sa
            };
            let mut packet = [0u8; 128];
            let n = sender
                .protect_packet(
                    sc,
                    OriginCounterOwner::Sequence(&mut Counter(1)),
                    sh,
                    sf,
                    sa,
                    PacketPlan {
                        prefix: &prefix,
                        payload: b"abc",
                        canonical_identity: &identity,
                        origin_context: context,
                        hop_profile: HopProfile::H0,
                    },
                    None,
                    &mut SecurityPacketWorkspace::<128>::new(),
                    &mut packet,
                )
                .unwrap();
            fn plan<'a>(
                p: &'a [u8],
                identity: &'a [u8],
                context: OriginContext,
            ) -> OpenPacketPlan<'a> {
                OpenPacketPlan {
                    packet: p,
                    prefix_bytes: 13,
                    canonical_identity: identity,
                    origin_context: context,
                    hop_profile: HopProfile::H0,
                }
            }
            let mut bridge = RuntimeBridge::<2, 128>::new();
            let mut rx = reliable();
            let mut out = [0xa5; 16];
            let mut scratch = SecurityPacketWorkspace::<128>::new();
            let mut bad = packet;
            bad[13] ^= 1;
            assert!(
                bridge
                    .receive(
                        receiver,
                        rc,
                        rh,
                        rf,
                        ra,
                        plan(&bad[..n], &identity, context),
                        &mut scratch,
                        &mut rx,
                        &mut out
                    )
                    .is_err()
            );
            assert_eq!(out, [0xa5; 16]);
            let result = bridge
                .receive(
                    receiver,
                    rc,
                    rh,
                    rf,
                    ra,
                    plan(&packet[..n], &identity, context),
                    &mut scratch,
                    &mut rx,
                    &mut out,
                )
                .unwrap();
            assert_eq!(result.bytes, 3);
            assert!(!result.duplicate);
            assert_eq!(&out[..3], b"abc");
            out.fill(0xa5);
            let result = bridge
                .receive(
                    receiver,
                    rc,
                    rh,
                    rf,
                    ra,
                    plan(&packet[..n], &identity, context),
                    &mut scratch,
                    &mut rx,
                    &mut out,
                )
                .unwrap();
            assert!(result.duplicate);
            assert_eq!(result.bytes, 0);
            assert_eq!(out, [0xa5; 16]);
            // Same numerical 1 belongs to two independent authenticated counter namespaces.
            let convert = |b: ucn_security::Binding| ucn_transport::Binding {
                principal: b.principal,
                address: b.address.get(),
                generation: b.generation.get(),
            };
            let key = ReliableKey {
                realm: 1,
                source: convert(rf.local),
                destination: convert(rf.peer),
                service: 0x40,
                sequence: 1,
                security: SecurityDomain {
                    session: 1,
                    key: 1,
                    policy: 1,
                    origin: OriginSecurity::O1,
                    fingerprint: [5; 16],
                    public_trusted_o0: false,
                },
            };
            let outgoing = rx.begin(key, b"frozen-origin", 21, 1000).unwrap();
            let attempt = rx.attempt_begin(outgoing, 21).unwrap();
            rx.submitted(attempt, ucn_transport::Submission::Submitted, 21)
                .unwrap();
            let mut ack = [0; 9];
            ucn_transport::encode_ack(
                ucn_transport::DeliveryAck {
                    service: 0x40,
                    sequence: 1,
                    status: 0,
                    credit: 1,
                },
                &mut ack,
            )
            .unwrap();
            let mut control_prefix = [0; 29];
            CommonHeader {
                contract: HeaderContract::C0,
                payload_kind: PayloadKind::Control,
                delivery: DeliveryGuarantee::BestEffort,
                interaction: InteractionRole::OneWay,
                traffic_class: TrafficClass::Q1,
                ..CommonHeader::decode(&prefix).unwrap()
            }
            .encode(&mut control_prefix);
            control_prefix[3..7].copy_from_slice(&1u32.to_be_bytes());
            control_prefix[7..9].copy_from_slice(&1u16.to_be_bytes());
            control_prefix[9..11].copy_from_slice(&2u16.to_be_bytes());
            control_prefix[11..15].copy_from_slice(&1u32.to_be_bytes());
            control_prefix[15..19].copy_from_slice(&1u32.to_be_bytes());
            control_prefix[19..27].copy_from_slice(&1u64.to_be_bytes());
            control_prefix[27..29]
                .copy_from_slice(&u16::from(ProtocolOpcode::DeliveryAck).to_be_bytes());
            let control_context = OriginContext::C0 {
                fingerprint: fp,
                transaction_id: C0TransactionId::new(1).unwrap(),
                opcode: ProtocolOpcode::DeliveryAck,
                direction: SenderDirection::AscendingPrincipal,
            };
            let mut control_packet = [0; 128];
            let csa = AccessRequest {
                protocol_opcode: u16::from(ProtocolOpcode::DeliveryAck),
                ..sa
            };
            let cra = AccessRequest {
                session: rh,
                direction: AccessDirection::Inbound,
                ..csa
            };
            let cn = sender
                .protect_packet(
                    sc,
                    OriginCounterOwner::Transaction(&mut TransactionCounter(1)),
                    sh,
                    sf,
                    csa,
                    PacketPlan {
                        prefix: &control_prefix,
                        payload: &ack,
                        canonical_identity: &control_prefix[3..],
                        origin_context: control_context,
                        hop_profile: HopProfile::H0,
                    },
                    None,
                    &mut SecurityPacketWorkspace::<128>::new(),
                    &mut control_packet,
                )
                .unwrap();
            bridge
                .receive_ack(
                    receiver,
                    rc,
                    rh,
                    rf,
                    cra,
                    OpenPacketPlan {
                        packet: &control_packet[..cn],
                        prefix_bytes: 29,
                        canonical_identity: &control_prefix[3..],
                        origin_context: control_context,
                        hop_profile: HopProfile::H0,
                    },
                    &mut scratch,
                    &mut rx,
                    outgoing,
                )
                .unwrap();
            assert_eq!(
                rx.view(outgoing).unwrap().phase,
                ucn_transport::ReliablePhase::Done
            );
            rx.attempt_complete(attempt).unwrap();
            rx.retire(outgoing).unwrap();
            bridge.stop();
            assert!(
                bridge
                    .receive(
                        receiver,
                        rc,
                        rh,
                        rf,
                        ra,
                        plan(&packet[..n], &identity, context),
                        &mut scratch,
                        &mut rx,
                        &mut out
                    )
                    .is_err()
            );
        });
    });
}
struct Driver {
    busy: usize,
    submits: usize,
}
impl TxDriver for Driver {
    fn submit(&mut self, _: LinkHandle, _: &[u8], _: TxToken) -> DriverSubmit {
        self.submits += 1;
        if self.busy > 0 {
            self.busy -= 1;
            DriverSubmit::NotSubmitted
        } else {
            DriverSubmit::Complete(TerminalOutcome::Success)
        }
    }
    fn cancel(&mut self, _: TxToken) -> DriverCancel {
        DriverCancel::Cancelled
    }
}
#[test]
fn actual_adapter_backpressure_retries_same_origin_and_stop_still_retires() {
    let ingress = AdapterEventIngress::<1, 2, 1, 128>::new(1).unwrap();
    let mut adapter = AdapterOwner::new(1, &ingress).unwrap();
    let link = adapter.open_link(0, 1, 128).unwrap();
    let mut bridge = RuntimeBridge::<2, 128>::new();
    let mut tx = reliable();
    let binding = |id| ucn_transport::Binding {
        principal: [id; 16],
        address: u32::from(id),
        generation: 1,
    };
    let key = ReliableKey {
        realm: 1,
        source: binding(1),
        destination: binding(2),
        service: 0x40,
        sequence: 1,
        security: SecurityDomain {
            session: 1,
            key: 1,
            policy: 1,
            origin: OriginSecurity::O1,
            fingerprint: [1; 16],
            public_trusted_o0: false,
        },
    };
    let h = tx.begin(key, b"same-authenticated-origin", 1, 100).unwrap();
    let mut driver = Driver {
        busy: 2,
        submits: 0,
    };
    for now in [1, 11, 21] {
        bridge
            .submit(&mut adapter, &mut driver, link, &mut tx, h, now)
            .unwrap();
    }
    assert_eq!(driver.submits, 3);
    assert_eq!(tx.view(h).unwrap().attempts, 1);
    assert_eq!(bridge.outstanding(), 1);
    bridge.stop();
    assert_eq!(bridge.retire(&mut adapter, &mut tx, 2).unwrap(), 1);
    assert_eq!(bridge.outstanding(), 0);
    assert_eq!(tx.view(h).unwrap().holders, 0);
}
