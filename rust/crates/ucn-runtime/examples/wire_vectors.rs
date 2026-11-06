//! Standalone interoperability output; 独立互操作输出，不链接C库。
#[path = "../../../tests/support/security.rs"]
mod security;
use ucn_security::{
    AccessDirection, AccessRequest, Fingerprint, OriginContext, OriginCounterOwner,
    OriginSequenceOwner, PacketPlan, SecurityPacketWorkspace,
};
use ucn_types::{
    HeaderContract, HopLimit, OriginSecurity, OriginSequence, PayloadKind, Result, ServiceId,
    TrafficClass,
};
struct Counter(u32);
impl OriginSequenceOwner for Counter {
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.0)
    }
    fn burn(&mut self, v: OriginSequence) -> Result<()> {
        assert_eq!(v.get(), self.0);
        self.0 += 1;
        Ok(())
    }
}
use ucn_realtime::{
    Envelope, TimeKey, TimeStampMessage, TimedMode, encode_envelope, encode_time_key,
    encode_time_stamp,
};
use ucn_transport::{
    DeliveryAck, FragmentPrefix, ParentKind, Sack, SetupAck, TransferAbort, TransferKey,
    TransferSetup, TransferTerminal, encode_abort, encode_ack, encode_fragment, encode_sack,
    encode_setup, encode_setup_ack, encode_terminal,
};
use ucn_types::{DeliveryGuarantee, InteractionRole};
#[cfg(feature = "bootstrap-runtime")]
fn bootstrap_authority_fixture() -> ucn_admission::BootstrapTranscript {
    ucn_admission::BootstrapTranscript {
        flow: ucn_admission::BootstrapFlow::Join,
        protocol_version: 6,
        bootstrap_header_contract: 1,
        joining_device_principal: [0; 16],
        joining_device_identity_digest: ucn_identity::Principal::new([0x11; 16]).unwrap(),
        authority_principal: [34; 16],
        authority_generation: 3,
        device_nonce: 0x0102_0304_0506_0708,
        authority_nonce: 0x3132_3334_3536_3738,
        transaction_id: 0x1112_1314_1516_1718,
        lease_freshness_challenge_nonce: 0x2122_2324_2526_2728,
        realm_id: 42,
        proposed_address: 0,
        address_binding_generation: 0,
        authority_address: 2,
        authority_binding_generation: 5,
        selected_link_instance_id: 7,
        binding_lease_id: [0; 16],
        binding_lease_duration_us: 0,
        authority_lease_sequence: 6,
        authority_lease_duration_us: 10000,
        freshness_max_remaining_lease_us: 9000,
        durable_fence_token: [1; 16],
        allocation_high_water_digest: [2; 16],
        quorum_config_digest: [3; 32],
        signer_set_digest: [4; 32],
        threshold_proof_digest: [5; 32],
        freshness_proof_transcript_hash: [6; 32],
        authority_signer_count: 3,
        authority_quorum_threshold: 2,
        binding_mode: 0,
        selected_hop_suite: 0,
        selected_hop_key_id: 0,
        selected_hop_key_generation: 0,
        selected_e2e_mode: 0,
        selected_e2e_suite: 0,
        selected_e2e_key_id: 0,
        selected_e2e_key_generation: 0,
        selected_session_generation: 0,
        selected_link_instance_generation: 9,
        prior_messages_hash: [49; 32],
    }
}
fn emit(name: &str, b: &[u8]) {
    print!("{name}=");
    for byte in b {
        print!("{byte:02X}");
    }
    println!();
}
#[allow(clippy::too_many_lines)] // Explicit independent vectors, not a production function.
fn main() {
    #[cfg(feature = "route-runtime")]
    {
        use ucn_routing::{
            DiscoveryKey, RouteControl, RouteFragment, RrepMessage, RrepPayload, RreqMessage,
            RreqPayload,
        };
        use ucn_security::Binding;
        use ucn_types::{
            AddressWidth, BindingGeneration, C0TransactionId, NodeAddress, PeerSessionGeneration,
            ProtocolOpcode, RealmId,
        };
        let key = DiscoveryKey {
            realm: RealmId::new(7).unwrap(),
            origin: Binding {
                address: NodeAddress::new(1, AddressWidth::A1).unwrap(),
                generation: BindingGeneration::active(2).unwrap(),
                principal: [0xa1; 16],
            },
            origin_session_generation: PeerSessionGeneration::new(3).unwrap(),
            target_address: NodeAddress::new(9, AddressWidth::A1).unwrap(),
            transaction_id: C0TransactionId::new(0x0102_0304_0506_0708).unwrap(),
        };
        let mut raw = [0; 99];
        let n = RouteControl::Request(RreqMessage {
            key,
            remaining_hops: HopLimit::new(8).unwrap(),
            payload: RreqPayload {
                accumulated_cost: 0x0102_0304,
                minimum_frame_mtu: 100,
                required_capability_bits: 3,
                flags: 0,
            },
        })
        .encode(AddressWidth::A1, &mut raw)
        .unwrap();
        emit("route_request_full", &raw[..n]);
        let n = RouteControl::Reply(
            RrepMessage {
                key,
                payload: RrepPayload {
                    destination_principal: [0xc3; 16],
                    destination_binding_generation: 4,
                    hop_count: 1,
                    accumulated_cost: 12,
                    path_frame_mtu: 127,
                    capability_bits: 3,
                },
            },
            0x1122_3344,
        )
        .encode(AddressWidth::A1, &mut raw)
        .unwrap();
        emit("route_reply_full", &raw[..n]);
        for (total, count) in [(77, 2), (99, 2), (99, 3)] {
            let body: Vec<u8> = (0..total).collect();
            let op = if total == 77 {
                ProtocolOpcode::RouteRrep
            } else {
                ProtocolOpcode::RouteRerr
            };
            for index in 0..count {
                let value = RouteFragment::from_body(
                    op,
                    C0TransactionId::new(0x0102_0304_0506_0708).unwrap(),
                    count,
                    index,
                    &body,
                )
                .unwrap();
                let n = value.encode(&mut raw).unwrap();
                emit(
                    &format!("route_fragment_{total}_{count}_{index}"),
                    &raw[..n],
                );
            }
        }
    }
    {
        use ucn_security::{Binding, PeerReauthRecord, REAUTH_RECORD_BYTES};
        use ucn_types::{AddressWidth, BindingGeneration, NodeAddress, RealmId};
        let binding = |address, p| Binding {
            address: NodeAddress::new(address, AddressWidth::A1).unwrap(),
            generation: BindingGeneration::active(1).unwrap(),
            principal: [p; 16],
        };
        for stage in 0..4 {
            let record = PeerReauthRecord {
                stage,
                width: AddressWidth::A1,
                realm: RealmId::new(1).unwrap(),
                initiator: binding(1, 0x11),
                responder: binding(2, 0x22),
                previous_session: 1,
                next_session: 2,
                origin_suite: 1,
                origin_key: 1,
                hop_key: 3,
                origin_generation: 2,
                hop_generation: 2,
                initiator_link: (7, 9),
                responder_link: if stage == 0 { (0, 0) } else { (8, 10) },
                transaction: 3,
                initiator_nonce: 4,
                responder_nonce: if stage == 0 { 0 } else { 5 },
                lifetime_us: 1000,
                previous_digest: [0x77; 32],
                initiator_keys: (1, 1),
                responder_history: if stage == 0 {
                    ucn_security::PeerReauthHistory::EMPTY
                } else {
                    ucn_security::PeerReauthHistory {
                        session: 1,
                        origin: 1,
                        hop: 1,
                        digest: [0x77; 32],
                    }
                },
            };
            let mut raw = [0; REAUTH_RECORD_BYTES];
            record.encode(&mut raw).unwrap();
            assert_eq!(PeerReauthRecord::decode(&raw).unwrap(), record);
            emit(&format!("peer_reauth_{stage}"), &raw);
            if stage == 2 {
                let asymmetric = PeerReauthRecord {
                    next_session: 3,
                    origin_generation: 3,
                    hop_generation: 3,
                    responder_history: ucn_security::PeerReauthHistory {
                        session: 2,
                        origin: 2,
                        hop: 2,
                        digest: [0x88; 32],
                    },
                    ..record
                };
                let mut a = [0; REAUTH_RECORD_BYTES];
                asymmetric.encode(&mut a).unwrap();
                assert_eq!(PeerReauthRecord::decode(&a).unwrap(), asymmetric);
                emit("peer_reauth_asymmetric", &a);
            }
            if stage == 2 {
                let mut logical = [0; 241];
                logical[..208].copy_from_slice(&raw);
                logical[208] = 32;
                logical[209..].fill(0x44);
                for index in 0..4 {
                    let start = usize::from(index) * 80;
                    let end = (start + 80).min(logical.len());
                    let mut part = [0; 98];
                    let n = ucn_security::encode_peer_reauth_fragment(
                        ucn_security::PeerReauthFragment {
                            stage,
                            index,
                            total_bytes: 241,
                            transaction: 3,
                            data: &logical[start..end],
                        },
                        &mut part,
                    )
                    .unwrap();
                    emit(&format!("peer_reauth_fragment_{index}"), &part[..n]);
                }
            }
        }
    }
    #[cfg(feature = "bootstrap-runtime")]
    {
        use ucn_admission::{BootstrapFlow, Cookie, CookieChallenge, Hello, HelloCookie};
        let hello = Hello {
            flow: BootstrapFlow::Join,
            identity_digest: ucn_identity::Principal::new([0x11; 16]).unwrap(),
            device_nonce: 0x0102_0304_0506_0708,
            transaction_id: 0x1112_1314_1516_1718,
        };
        let challenge = CookieChallenge {
            flow: BootstrapFlow::Join,
            transaction_id: hello.transaction_id,
            cookie_time_bucket: 0x0102_0304,
            issuer_link_id: 7,
            issuer_link_generation: 9,
            cookie_length: 4,
            cookie: [1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
        };
        let realm = ucn_types::RealmId::new(1).unwrap();
        let width = ucn_types::AddressWidth::A1;
        let mut bytes = [0; 128];
        let hash = ucn_runtime::bootstrap_prior_hash(
            hello,
            challenge,
            realm,
            width,
            &mut ucn_digest::DigestWorkspace::new(),
        )
        .unwrap();
        let echo = HelloCookie {
            flow: BootstrapFlow::Join,
            identity_digest: hello.identity_digest,
            device_nonce: hello.device_nonce,
            transaction_id: hello.transaction_id,
            lease_freshness_challenge_nonce: 0x2122_2324_2526_2728,
            selected_link_instance_id: 7,
            selected_link_instance_generation: 9,
            prior_messages_hash: hash,
            cookie_evidence: Cookie::new(&challenge.cookie[..4]).unwrap(),
        };
        for (name, message) in [
            (
                "bootstrap_hello",
                ucn_runtime::BootstrapMessage::Hello(hello),
            ),
            (
                "bootstrap_challenge",
                ucn_runtime::BootstrapMessage::Challenge(challenge),
            ),
            ("bootstrap_echo", ucn_runtime::BootstrapMessage::Echo(echo)),
        ] {
            let n = ucn_runtime::encode_bootstrap_frame(message, realm, width, &mut bytes).unwrap();
            emit(name, &bytes[..n]);
        }
        emit("bootstrap_prior_hash", &hash);
        let mut transcript = bootstrap_authority_fixture();
        for phase in 2..=6 {
            let phase = ucn_admission::BootstrapPhase::try_from(phase).unwrap();
            if phase == ucn_admission::BootstrapPhase::DeviceVerified {
                transcript.joining_device_principal = [0x44; 16];
                transcript.selected_hop_suite = 16;
                transcript.selected_hop_key_id = 1;
                transcript.selected_hop_key_generation = 2;
                transcript.selected_e2e_mode = 1;
                transcript.selected_e2e_suite = 1;
                transcript.selected_e2e_key_id = 3;
                transcript.selected_e2e_key_generation = 4;
                transcript.selected_session_generation = 5;
            }
            if phase == ucn_admission::BootstrapPhase::AddressOffered {
                transcript.proposed_address = 9;
                transcript.address_binding_generation = 11;
                transcript.binding_lease_id = [0x55; 16];
                transcript.binding_lease_duration_us = 2000;
                transcript.binding_mode = 1;
            }
            let event = match phase {
                ucn_admission::BootstrapPhase::AuthorityVerified => {
                    ucn_admission::BootstrapEvent::AuthorityProof
                }
                ucn_admission::BootstrapPhase::DeviceVerified => {
                    ucn_admission::BootstrapEvent::DeviceProof
                }
                ucn_admission::BootstrapPhase::AddressOffered => {
                    ucn_admission::BootstrapEvent::AddressOffer
                }
                ucn_admission::BootstrapPhase::DeviceCommitted => {
                    ucn_admission::BootstrapEvent::DeviceCommit
                }
                ucn_admission::BootstrapPhase::FinalDurable => {
                    ucn_admission::BootstrapEvent::FinalDurable
                }
                _ => panic!(),
            };
            let mut logical = [0; 508];
            let n = ucn_admission::encode_logical_event(
                event,
                phase,
                &transcript,
                ucn_admission::Evidence::new(&[0x77; 128]).unwrap(),
                &mut logical,
            )
            .unwrap();
            emit(&format!("bootstrap_event_{}", phase as u8), &logical[..n]);
            #[cfg(feature = "bootstrap-join")]
            if phase == ucn_admission::BootstrapPhase::FinalDurable {
                let c = ucn_runtime::join_session_candidate(
                    transcript,
                    width,
                    true,
                    3,
                    ucn_runtime::JoinSessionPolicy {
                        generation: 1,
                        lifetime_us: 5000,
                    },
                    100,
                    2000,
                )
                .unwrap();
                let mut seed = [0; 48];
                seed[..8].copy_from_slice(&c.expires_at_us.to_be_bytes());
                seed[8..10].copy_from_slice(&c.origin_tx.key_id.get().to_be_bytes());
                seed[10..12].copy_from_slice(&c.origin_rx.key_id.get().to_be_bytes());
                seed[12..14].copy_from_slice(&c.hop_tx.unwrap().key_id.get().to_be_bytes());
                seed[14..16].copy_from_slice(&c.hop_rx.unwrap().key_id.get().to_be_bytes());
                seed[16..].copy_from_slice(&c.transcript_digest);
                emit("bootstrap_session_seed", &seed);
            }
            if phase == ucn_admission::BootstrapPhase::AuthorityVerified {
                let key = ucn_admission::AdmissionKey {
                    link: ucn_admission::LinkIdentity {
                        link_id: 7,
                        link_generation: 9,
                    },
                    local_peer_discriminator: 1,
                    identity_digest: hello.identity_digest,
                    transaction_id: hello.transaction_id,
                };
                for index in 0..9 {
                    let f = ucn_admission::BootstrapFragment::from_logical_chunk(
                        key,
                        BootstrapFlow::Join,
                        event,
                        phase,
                        index,
                        63,
                        &logical[..n],
                    )
                    .unwrap();
                    let mut raw = [0; 164];
                    let size = ucn_admission::encode_fragment(f, &mut raw).unwrap();
                    emit(&format!("bootstrap_auth_fragment_{index}"), &raw[..size]);
                    if index == 0 || index == 8 {
                        let size = ucn_runtime::encode_bootstrap_event_frame(
                            event,
                            f,
                            ucn_types::RealmId::new(42).unwrap(),
                            width,
                            &mut bytes,
                        )
                        .unwrap();
                        emit(&format!("bootstrap_auth_frame_{index}"), &bytes[..size]);
                    }
                }
            }
        }
    }
    #[cfg(feature = "capability-control")]
    {
        use ucn_capability::{
            CapabilityFragment, CapabilityQuery, CapabilityRecord, CapabilitySummary,
            LinkCapability, MessageClass, PeerCapability, ProfileAck, ProfileRequirements,
            ProfileSelect, SelectContext, capability_digest, capability_fragment_range,
            encode_capability_fragment, encode_capability_query, encode_capability_record,
            encode_capability_summary, encode_profile_ack, encode_profile_select,
            select_transcript,
        };
        let local = CapabilityRecord {
            capability_generation: 1,
            link: LinkCapability {
                link_instance_generation: 1,
                carrier_mtu: 140,
                link_frame_mtu: 128,
                processing_frame_mtu: 128,
                carrier_header_bytes: 4,
                carrier_padding_bytes: 0,
                carrier_crc_bytes: 4,
                carrier_tag_bytes: 0,
                carrier_max_fragments: 1,
                link_flags: 8,
                nominal_rate_bps: 1_000_000,
                hardware_priority_count: 1,
                timestamp_capability_bits: 0,
                timestamp_uncertainty_us: 0,
            },
            peer: PeerCapability {
                feature_bits: 0x010b,
                hop_suite_bits: 2,
                e2e_suite_bits: 14,
                max_message_class: MessageClass::T128,
                max_rx_window: 4,
                max_concurrent_transfers: 2,
                realtime_mode_bits: 0,
                clock_domain_id: 0,
                clock_domain_generation: 0,
            },
        };
        let mut peer = local;
        peer.capability_generation = 7;
        peer.peer.max_rx_window = 2;
        let requirements = ProfileRequirements {
            required_feature_bits: 0x010b,
            required_hop_suite_bits: 2,
            required_e2e_suite_bits: 14,
            minimum_message_class: MessageClass::T128,
            minimum_rx_window: 1,
            minimum_concurrent_transfers: 1,
            required_realtime_mode_bits: 0,
        };
        let digest = capability_digest(local).unwrap();
        let select = ProfileSelect::build(
            local,
            digest,
            peer,
            capability_digest(peer).unwrap(),
            requirements,
        )
        .unwrap();
        let binding = |id| ucn_security::Binding {
            address: ucn_types::NodeAddress::new(id, ucn_types::AddressWidth::A1).unwrap(),
            generation: ucn_types::BindingGeneration::active(1).unwrap(),
            principal: [u8::try_from(id).unwrap(); 16],
        };
        let transcript = select_transcript(
            SelectContext {
                realm: 1,
                source: binding(1),
                destination: binding(2),
                session: 1,
                key: 1,
                policy: 1,
                origin: OriginSecurity::O1,
                fingerprint: [5; 16],
                transaction: 0x0102_0304_0506_0708,
                service: 0x40,
            },
            select,
            &mut ucn_digest::DigestWorkspace::new(),
        )
        .unwrap();
        let mut bytes = [0; 75];
        encode_capability_record(local, &mut bytes[..68]).unwrap();
        emit("capability_record", &bytes[..68]);
        emit("capability_digest", &digest);
        encode_capability_summary(
            CapabilitySummary {
                capability_generation: 1,
                link_instance_generation: 1,
                digest,
            },
            &mut bytes[..24],
        )
        .unwrap();
        emit("capability_summary", &bytes[..24]);
        encode_capability_query(
            CapabilityQuery {
                requested_generation: 1,
                known_digest: digest,
            },
            &mut bytes[..20],
        )
        .unwrap();
        emit("capability_query", &bytes[..20]);
        encode_profile_select(select, &mut bytes[..59]).unwrap();
        emit("capability_select", &bytes[..59]);
        emit("capability_select_transcript", &transcript);
        encode_profile_ack(
            ProfileAck {
                selected: select,
                transcript_digest: transcript,
            },
            &mut bytes,
        )
        .unwrap();
        emit("capability_ack", &bytes);
        for (op, total) in [
            (ucn_types::ProtocolOpcode::CapabilityAdvertise, 68_u16),
            (ucn_types::ProtocolOpcode::CapabilityAck, 75),
        ] {
            let data: Vec<u8> = (0..u8::try_from(total).unwrap()).collect();
            for index in 0..2 {
                let range = capability_fragment_range(op, usize::from(total), index).unwrap();
                let mut raw = [0; 44];
                let n = encode_capability_fragment(
                    CapabilityFragment {
                        opcode: op,
                        total,
                        index,
                        chunk: &data[range],
                    },
                    &mut raw,
                )
                .unwrap();
                emit(&format!("cap_fragment_{total}_{index}"), &raw[..n]);
            }
        }
    }
    let parent = ucn_transport::TransferParent {
        kind: ParentKind::C1,
        id: 0,
        generation: 7,
        realm: 1,
        source: ucn_transport::Binding {
            principal: [1; 16],
            address: 1,
            generation: 1,
        },
        destination: ucn_transport::Binding {
            principal: [2; 16],
            address: 2,
            generation: 1,
        },
        security: ucn_transport::SecurityDomain {
            session: 1,
            key: 1,
            policy: 1,
            origin: OriginSecurity::O1,
            fingerprint: [5; 16],
            public_trusted_o0: false,
        },
        feedback_flow: None,
    };
    emit(
        "parent_record",
        &ucn_transport::encode_parent_record(parent, 0x0506_0708, 0x0102_0304_0506_0708).unwrap(),
    );
    emit(
        "parent_context_digest",
        &ucn_transport::parent_context_digest(parent, &mut ucn_digest::DigestWorkspace::new())
            .unwrap(),
    );
    print!("parent_phase_matrix=");
    for source in [false, true] {
        for phase in 0..6 {
            for opcode in [
                ucn_types::ProtocolOpcode::TransportParentPrepare,
                ucn_types::ProtocolOpcode::TransportParentAccept,
                ucn_types::ProtocolOpcode::TransportParentCommit,
                ucn_types::ProtocolOpcode::TransportParentAbort,
                ucn_types::ProtocolOpcode::DiagnosticPing,
            ] {
                for disposition in 0..4 {
                    for sent in 0..4 {
                        let (next, reply) = ucn_transport::parent_phase_transition(
                            source,
                            phase,
                            opcode,
                            disposition,
                            sent,
                        )
                        .map_or((255, 255), |(next, reply)| (next, reply.unwrap_or(0)));
                        print!("{next:02X}{reply:02X}");
                    }
                }
            }
        }
    }
    println!();
    let mut publication_bytes = [0; 32];
    for (name, op, disposition) in [
        (
            "parent_prepare",
            ucn_types::ProtocolOpcode::TransportParentPrepare,
            0,
        ),
        (
            "parent_prepared",
            ucn_types::ProtocolOpcode::TransportParentAccept,
            1,
        ),
        (
            "parent_committed",
            ucn_types::ProtocolOpcode::TransportParentAccept,
            2,
        ),
        (
            "parent_commit",
            ucn_types::ProtocolOpcode::TransportParentCommit,
            0,
        ),
        (
            "parent_abort",
            ucn_types::ProtocolOpcode::TransportParentAbort,
            7,
        ),
    ] {
        ucn_transport::ParentPublication {
            transaction: 0x0102_0304_0506_0708,
            generation: 0x1122_3344,
            digest: core::array::from_fn(|i| u8::try_from(i).unwrap()),
            disposition,
        }
        .encode(op, &mut publication_bytes)
        .unwrap();
        emit(name, &publication_bytes);
    }
    let mut allocator_body = [0; 16];
    ucn_group::encode_allocator_record(1, 10, 3, &mut allocator_body).unwrap();
    emit("group_allocator", &allocator_body);
    security::exercise(1, 2, |sender, session, facts, crypto| {
        let mut prefix = [0; 13];
        ucn_wire::CommonHeader {
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
        identity[..16].fill(1);
        identity[16..20].copy_from_slice(&1u32.to_be_bytes());
        identity[20..36].fill(2);
        identity[36..40].copy_from_slice(&1u32.to_be_bytes());
        identity[40..].copy_from_slice(&prefix[7..]);
        let fp = Fingerprint::new([5; 16]).unwrap();
        let mut out = [0; 128];
        let n = sender
            .protect_packet(
                crypto,
                OriginCounterOwner::Sequence(&mut Counter(1)),
                session,
                facts,
                AccessRequest {
                    session,
                    context_fingerprint: fp,
                    service: ServiceId::new(0x40).unwrap(),
                    protocol_opcode: 0,
                    direction: AccessDirection::Outbound,
                },
                PacketPlan {
                    prefix: &prefix,
                    payload: b"abc",
                    canonical_identity: &identity,
                    origin_context: OriginContext::Sequenced {
                        contract: HeaderContract::C1,
                        fingerprint: fp,
                        sequence: OriginSequence::new(1).unwrap(),
                    },
                    hop_profile: ucn_types::HopProfile::H0,
                },
                None,
                &mut SecurityPacketWorkspace::<128>::new(),
                &mut out,
            )
            .unwrap();
        emit("protected_c1", &out[..n]);
    });
    {
        let mut members = [ucn_group::Member::EMPTY; 16];
        members[0] = ucn_group::Member {
            principal: [2; 16],
            address: 2,
            binding_generation: 1,
            sender_slot: 1,
            weight: 1,
        };
        let r = ucn_group::GroupRecord {
            definition: ucn_group::GroupDefinition {
                realm: 1,
                group: 1,
                generation: 1,
                key_generation: 1,
                tree_generation: 0,
                service: 0x40,
                scope_mask: 31,
                fanout_limit: 1,
                count: 1,
                members,
                exact_principal: true,
                public_static: false,
                quorum_weight: 1,
            },
            retired: false,
            policy_generation: 1,
            member_generation: 1,
            opcode: 9,
            allocated_high_water: 1,
            security_digest: [5; 16],
        };
        let mut out = [0; 512];
        let n = ucn_group::encode_group_record(r, &mut out).unwrap();
        emit("group_context", &out[..n]);
    }
    let mut flags = [0; 1024];
    for (role_index, role) in [
        InteractionRole::OneWay,
        InteractionRole::Request,
        InteractionRole::Result,
        InteractionRole::Error,
    ]
    .iter()
    .enumerate()
    {
        for value in 0..=u8::MAX {
            flags[role_index * 256 + usize::from(value)] =
                u8::from(ucn_types::operation_flags_valid(*role, value));
        }
    }
    emit("operation_flags_matrix", &flags);
    let mut b = [0u8; 200];
    for (name, role, flags, result) in [
        ("rpc_request", InteractionRole::Request, 1, 0),
        ("rpc_result", InteractionRole::Result, 5, 7),
        ("rpc_error", InteractionRole::Error, 5, 65535),
    ] {
        let n = ucn_service::encode_operation_envelope(
            role,
            ucn_service::OperationEnvelope {
                operation: 0x0102_0304_0506_0708,
                flags,
                result,
            },
            &mut b,
        )
        .unwrap();
        b[n..n + 3].copy_from_slice(b"abc");
        emit(name, &b[..n + 3]);
    }
    let n = encode_ack(
        DeliveryAck {
            service: 0x1234,
            sequence: 0x0102_0304,
            status: 0,
            credit: 7,
        },
        &mut b,
    )
    .unwrap();
    emit("ack", &b[..n]);
    let mut setup = TransferSetup {
        kind: ParentKind::C1,
        parent_id: 0,
        parent_generation: 2,
        transfer_id: 3,
        service: 0x1234,
        total_bytes: 5,
        digest: [0xa5; 16],
        delivery: DeliveryGuarantee::Reliable,
        interaction: InteractionRole::OneWay,
        fragments: 2,
        fragment_bytes: 3,
        lifetime_ms: 1000,
        operation_id: 0,
        operation_flags: 0,
        result_code: 0,
    };
    let n = encode_setup(setup, &mut b).unwrap();
    emit("setup", &b[..n]);
    setup.interaction = InteractionRole::Request;
    setup.operation_id = 0x0102_0304_0506_0708;
    setup.operation_flags = 1;
    let n = encode_setup(setup, &mut b).unwrap();
    emit("request_setup", &b[..n]);
    let key = TransferKey {
        kind: ParentKind::C1,
        parent_id: 0,
        parent_generation: 0x0102_0304,
        transfer_id: 0x0506_0708,
        service: 0x1234,
        digest: core::array::from_fn(|i| u8::try_from(i).unwrap()),
    };
    let n = encode_setup_ack(
        SetupAck {
            key,
            fragment_bytes: 0x0102,
            credit: 0x0304,
        },
        &mut b,
    )
    .unwrap();
    emit("setup_ack", &b[..n]);
    let n = encode_abort(TransferAbort { key, reason: 7 }, &mut b).unwrap();
    emit("transfer_abort", &b[..n]);
    let n = encode_terminal(
        TransferTerminal {
            key,
            state: 2,
            result: 0x5678,
        },
        &mut b,
    )
    .unwrap();
    emit("transfer_terminal", &b[..n]);
    let op = ucn_service::OperationWireKey {
        operation: 0x0102_0304_0506_0708,
        service: 0x1234,
        opcode: 0x2345,
        request_digest: core::array::from_fn(|i| u8::try_from(i).unwrap()),
    };
    let n = ucn_service::encode_operation_key(op, &mut b).unwrap();
    emit("operation_key", &b[..n]);
    let data_op = ucn_service::OperationWireKey { opcode: 0, ..op };
    let n = ucn_service::encode_operation_key(data_op, &mut b).unwrap();
    emit("operation_data_key", &b[..n]);
    b[..2].copy_from_slice(&ucn_types::DURABLE_RPC_NO_EFFECT.to_be_bytes());
    b[2..4].copy_from_slice(&ucn_types::DURABLE_RPC_IN_DOUBT.to_be_bytes());
    emit("durable_result_codes", &b[..4]);
    let digest = ucn_service::operation_result_digest(
        op,
        3,
        -12,
        b"ok",
        &mut ucn_digest::DigestWorkspace::new(),
    )
    .unwrap();
    emit("operation_result_digest", &digest);
    let n = ucn_service::encode_operation_receipt(
        ucn_service::OperationReceipt {
            key: op,
            result_digest: [0xa5; 16],
            phase: 3,
        },
        &mut b,
    )
    .unwrap();
    emit("operation_receipt", &b[..n]);
    let receipt = ucn_service::OperationReceipt {
        key: op,
        result_digest: [0xa5; 16],
        phase: 3,
    };
    let n = ucn_service::encode_operation_ack(receipt, &mut b).unwrap();
    emit("operation_result_ack", &b[..n]);
    let n = ucn_service::encode_operation_key(op, &mut b).unwrap();
    emit("operation_resume", &b[..n]);
    let mut response = ucn_service::OperationResult {
        receipt,
        status: -12,
        length: 2,
        payload: [0; 146],
    };
    response.payload[..2].copy_from_slice(b"ok");
    let n = ucn_service::encode_operation_result(response, &mut b).unwrap();
    emit("operation_recovered_result", &b[..n]);
    let n = ucn_service::encode_operation_cancel(
        ucn_service::OperationCancel { key: op, reason: 1 },
        &mut b,
    )
    .unwrap();
    emit("operation_cancel", &b[..n]);
    let key = TransferKey::from_setup(setup, &mut ucn_digest::DigestWorkspace::new()).unwrap();
    emit("setup_digest", &key.digest);
    for (name, domain) in [
        ("aad_digest", b"UCN6-AAD-V1".as_slice()),
        ("payload_digest", b"UCN6-PAYLOAD-V1".as_slice()),
        ("message_digest", b"UCN6-TRANSFER-MESSAGE-V1".as_slice()),
    ] {
        let d =
            ucn_digest::canonical_digest(domain, b"abc", &mut ucn_digest::DigestWorkspace::new())
                .unwrap();
        emit(name, &d);
    }
    for kind in [ParentKind::C1, ParentKind::Flow] {
        let n = encode_fragment(
            FragmentPrefix {
                kind,
                transfer_id: 3,
                parent_generation: if kind == ParentKind::C1 { 2 } else { 0 },
                index: 1,
                count: 2,
            },
            &mut b,
        )
        .unwrap();
        emit(
            if kind == ParentKind::C1 {
                "c1_fragment"
            } else {
                "flow_fragment"
            },
            &b[..n],
        );
        let n = encode_sack(
            Sack {
                kind,
                transfer_id: 3,
                parent_generation: if kind == ParentKind::C1 { 2 } else { 0 },
                original_service: if kind == ParentKind::C1 { 0x1234 } else { 0 },
                base: 0,
                bitmap: 3,
                credit: 4,
            },
            &mut b,
        )
        .unwrap();
        emit(
            if kind == ParentKind::C1 {
                "c1_sack"
            } else {
                "flow_sack"
            },
            &b[..n],
        );
    }
    let key = TimeKey {
        domain: 1,
        generation: 2,
        sequence: 3,
    };
    let mut k = [0; 12];
    encode_time_key(key, &mut k).unwrap();
    emit("time_key", &k);
    let mut stamp = [0; 24];
    encode_time_stamp(
        TimeStampMessage {
            key,
            time: 0x0102_0304_0506_0708,
            bound: 7,
        },
        &mut stamp,
    )
    .unwrap();
    emit("time_stamp", &stamp);
    let mut envelope = [0; 16];
    encode_envelope(
        Envelope {
            mode: TimedMode::Deadline,
            uncertainty_class: 11,
            hardware_capture: true,
            domain_valid: true,
            holdover: false,
            domain: 1,
            generation: 2,
            capture_us: 0x0102_0304_0506_0708,
        },
        &mut envelope,
    )
    .unwrap();
    emit("envelope", &envelope);
}
