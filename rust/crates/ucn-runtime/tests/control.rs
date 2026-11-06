#![cfg(feature = "service-control")]
#![cfg_attr(not(feature = "service-control"), allow(missing_docs))]
//! Real authenticated Control admission: no fabricated evidence or bypassed Replay.
#[path = "../../../tests/support/security.rs"]
mod security;
use ucn_runtime::*;
use ucn_security::*;
use ucn_types::*;
use ucn_wire::CommonHeader;
struct Counter(u64);
impl C0TransactionOwner for Counter {
    fn preview(&self) -> Result<C0TransactionId> {
        C0TransactionId::new(self.0)
    }
    fn burn(&mut self, v: C0TransactionId) -> Result<()> {
        if v.get() != self.0 {
            return Err(Error::State);
        }
        self.0 += 1;
        Ok(())
    }
}
#[test]
#[allow(clippy::too_many_lines)]
fn real_control_auth_acl_capacity_negative_replay_and_stop() {
    let op = ProtocolOpcode::OperationQuery;
    security::exercise_opcodes(1, 2, &[op], |sender, sh, sf, sc| {
        security::exercise_opcodes(2, 1, &[op], |receiver, rh, rf, rc| {
            let key = ucn_service::OperationWireKey {
                operation: 7,
                service: 0x40,
                opcode: 9,
                request_digest: [3; 16],
            };
            let mut body = [0; 28];
            ucn_service::encode_operation_key(key, &mut body).unwrap();
            let mut prefix = [0; 29];
            CommonHeader {
                contract: HeaderContract::C0,
                origin_security: OriginSecurity::O1,
                delivery: DeliveryGuarantee::BestEffort,
                interaction: InteractionRole::Request,
                payload_kind: PayloadKind::Control,
                traffic_class: TrafficClass::Q1,
                hop_limit: HopLimit::new(1).unwrap(),
            }
            .encode(&mut prefix);
            prefix[3..7].copy_from_slice(&1u32.to_be_bytes());
            prefix[7..9].copy_from_slice(&1u16.to_be_bytes());
            prefix[9..11].copy_from_slice(&2u16.to_be_bytes());
            prefix[11..15].copy_from_slice(&1u32.to_be_bytes());
            prefix[15..19].copy_from_slice(&1u32.to_be_bytes());
            prefix[19..27].copy_from_slice(&1u64.to_be_bytes());
            prefix[27..29].copy_from_slice(&u16::from(op).to_be_bytes());
            let fp = Fingerprint::new([5; 16]).unwrap();
            let context = OriginContext::C0 {
                fingerprint: fp,
                transaction_id: C0TransactionId::new(1).unwrap(),
                opcode: op,
                direction: SenderDirection::AscendingPrincipal,
            };
            let sa = AccessRequest {
                session: sh,
                context_fingerprint: fp,
                service: ServiceId::new(0x40).unwrap(),
                protocol_opcode: u16::from(op),
                direction: AccessDirection::Outbound,
            };
            let ra = AccessRequest {
                session: rh,
                direction: AccessDirection::Inbound,
                ..sa
            };
            let mut bytes = [0; 128];
            let n = sender
                .protect_packet(
                    sc,
                    OriginCounterOwner::Transaction(&mut Counter(1)),
                    sh,
                    sf,
                    sa,
                    PacketPlan {
                        prefix: &prefix,
                        payload: &body,
                        canonical_identity: &prefix[3..],
                        origin_context: context,
                        hop_profile: HopProfile::H0,
                    },
                    None,
                    &mut SecurityPacketWorkspace::<128>::new(),
                    &mut bytes,
                )
                .unwrap();
            let plan = |p| OpenPacketPlan {
                packet: p,
                prefix_bytes: 29,
                canonical_identity: &prefix[3..],
                origin_context: context,
                hop_profile: HopProfile::H0,
            };
            let mut queue = ControlCoordinator::<1, 128>::new(10).unwrap();
            let mut work = SecurityPacketWorkspace::<128>::new();
            let mut bad = bytes;
            bad[29] ^= 1;
            assert!(
                queue
                    .receive(receiver, rc, rh, rf, ra, plan(&bad[..n]), &mut work)
                    .is_err()
            );
            assert_eq!(queue.peek_next(), Err(Error::NotFound));
            // Exact ACL mismatch never reserves or consumes the legitimate request.
            let wrong = AccessRequest {
                service: ServiceId::new(0x41).unwrap(),
                ..ra
            };
            assert!(
                queue
                    .receive(receiver, rc, rh, rf, wrong, plan(&bytes[..n]), &mut work)
                    .is_err()
            );
            let h = queue
                .receive(receiver, rc, rh, rf, ra, plan(&bytes[..n]), &mut work)
                .unwrap();
            let command = queue.view(h).unwrap();
            assert_eq!(command.body(), ControlBody::OperationQuery(key));
            assert_eq!(command.evidence().identities().1.principal, [1; 16]);
            assert_eq!(
                queue.receive(receiver, rc, rh, rf, ra, plan(&bytes[..n]), &mut work),
                Err(Error::NoSpace)
            );
            assert_eq!(queue.view(h).unwrap(), command);
            queue.ack(h).unwrap();
            assert_eq!(
                queue.receive(receiver, rc, rh, rf, ra, plan(&bytes[..n]), &mut work),
                Err(Error::Replay)
            );
            assert_eq!(queue.view(h), Err(Error::State));
            queue.stop();
            assert_eq!(
                queue.receive(receiver, rc, rh, rf, ra, plan(&bytes[..n]), &mut work),
                Err(Error::State)
            );
            // An authorized peer may MAC a different Body Service: that does not grant ACL.
            let mut other_queue = ControlCoordinator::<1, 128>::new(11).unwrap();
            let other_key = ucn_service::OperationWireKey {
                service: 0x41,
                ..key
            };
            ucn_service::encode_operation_key(other_key, &mut body).unwrap();
            prefix[19..27].copy_from_slice(&2u64.to_be_bytes());
            let other_context = OriginContext::C0 {
                transaction_id: C0TransactionId::new(2).unwrap(),
                fingerprint: fp,
                opcode: op,
                direction: SenderDirection::AscendingPrincipal,
            };
            let count = sender
                .protect_packet(
                    sc,
                    OriginCounterOwner::Transaction(&mut Counter(2)),
                    sh,
                    sf,
                    sa,
                    PacketPlan {
                        prefix: &prefix,
                        payload: &body,
                        canonical_identity: &prefix[3..],
                        origin_context: other_context,
                        hop_profile: HopProfile::H0,
                    },
                    None,
                    &mut work,
                    &mut bytes,
                )
                .unwrap();
            assert_eq!(
                other_queue.receive(
                    receiver,
                    rc,
                    rh,
                    rf,
                    ra,
                    OpenPacketPlan {
                        packet: &bytes[..count],
                        prefix_bytes: 29,
                        canonical_identity: &prefix[3..],
                        origin_context: other_context,
                        hop_profile: HopProfile::H0
                    },
                    &mut work
                ),
                Err(Error::Access)
            );
            assert_eq!(other_queue.peek_next(), Err(Error::NotFound));
        });
    });
}
