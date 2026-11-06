//! Capability Wire、Profile 交集和纯 Contract Resolver 回归。

use std::vec::Vec;

use ucn_capability::{
    CAPABILITY_QUERY_BYTES, CAPABILITY_RECORD_BYTES, CAPABILITY_SUMMARY_BYTES, CapabilityQuery,
    CapabilityRecord, CapabilitySummary, ContractCandidate, ContractResolver, DependencyKind,
    EffectiveIntent, LinkCapability, MessageClass, PeerCapability, ProfileAck, ProfileRequirements,
    ProfileSelect, ResolveResult, ResourceView, SecurityFloor, SelectContext, capability_digest,
    decode_capability_query, decode_capability_record, decode_capability_summary,
    decode_profile_ack, decode_profile_select, encode_capability_query, encode_capability_record,
    encode_capability_summary, encode_profile_ack, encode_profile_select, select_transcript,
};
use ucn_types::{Error, HeaderContract, HopProfile, OriginSecurity};

const SHARED_FIXTURES: &str =
    include_str!("../../../tests/conformance/v6s_admission_capability_v1.h");

fn fixture(name: &str) -> Vec<u8> {
    let marker = std::format!("#define {name}");
    let mut collecting = false;
    let mut bytes = Vec::new();
    for line in SHARED_FIXTURES.lines() {
        if !collecting {
            if line.starts_with(&marker) {
                collecting = true;
            } else {
                continue;
            }
        }
        for token in
            line.split(|character: char| character.is_ascii_whitespace() || character == ',')
        {
            let token = token.trim_end_matches('\\');
            if let Some(hex) = token.strip_prefix("0x") {
                bytes.push(u8::from_str_radix(hex, 16).expect("fixture byte"));
            }
        }
        if collecting && !line.trim_end().ends_with('\\') {
            break;
        }
    }
    assert!(!bytes.is_empty(), "fixture {name} must exist");
    bytes
}
#[test]
#[allow(clippy::too_many_lines)] // Explicit field-by-field negative and transcript coverage, test only.
fn profile_codecs_exact_lengths_reserved_fields_zero_write_and_full_transcript() {
    let local = record(1, 1);
    let peer = record(7, 1);
    let select = ProfileSelect::build(
        local,
        capability_digest(local).unwrap(),
        peer,
        capability_digest(peer).unwrap(),
        ProfileRequirements {
            required_feature_bits: 0x010b,
            required_hop_suite_bits: 2,
            required_e2e_suite_bits: 2,
            minimum_message_class: MessageClass::T32,
            minimum_rx_window: 1,
            minimum_concurrent_transfers: 1,
            required_realtime_mode_bits: 0,
        },
    )
    .unwrap();
    let mut bytes = [0; 59];
    encode_profile_select(select, &mut bytes).unwrap();
    assert_eq!(decode_profile_select(&bytes), Ok(select));
    assert_eq!(select.reverse().reverse(), select);
    for length in [58, 60] {
        let mut out = vec![0xa5; length];
        let before = out.clone();
        assert_eq!(encode_profile_select(select, &mut out), Err(Error::NoSpace));
        assert_eq!(out, before);
        assert_eq!(decode_profile_select(&out), Err(Error::Malformed));
    }
    for offset in [0, 4, 8, 12, 16, 20, 22, 26, 27, 43] {
        let mut bad = bytes;
        match offset {
            0 | 4 | 8 | 12 | 16 => bad[offset..offset + 4].fill(0),
            20 | 22 => bad[offset..offset + 2].fill(0),
            26 => bad[26] = 255,
            27 | 43 => bad[offset..offset + 16].fill(0),
            _ => unreachable!(),
        }
        assert!(decode_profile_select(&bad).is_err());
    }
    let binding = |id| ucn_security::Binding {
        address: ucn_types::NodeAddress::new(id, ucn_types::AddressWidth::A1).unwrap(),
        generation: ucn_types::BindingGeneration::active(1).unwrap(),
        principal: [u8::try_from(id).unwrap(); 16],
    };
    let context = SelectContext {
        realm: 1,
        source: binding(1),
        destination: binding(2),
        session: 1,
        key: 1,
        policy: 1,
        origin: OriginSecurity::O1,
        fingerprint: [5; 16],
        transaction: 1,
        service: 0x40,
    };
    let base = select_transcript(context, select, &mut ucn_digest::DigestWorkspace::new()).unwrap();
    for field in 0..12 {
        let mut changed = context;
        match field {
            0 => changed.realm += 1,
            1 => changed.source = binding(3),
            2 => changed.destination = binding(4),
            3 => changed.source.generation = ucn_types::BindingGeneration::active(2).unwrap(),
            4 => changed.destination.generation = ucn_types::BindingGeneration::active(2).unwrap(),
            5 => changed.session += 1,
            6 => changed.key += 1,
            7 => changed.policy += 1,
            8 => changed.origin = OriginSecurity::O2,
            9 => changed.fingerprint[0] ^= 1,
            10 => changed.transaction += 1,
            11 => changed.service += 1,
            _ => unreachable!(),
        }
        assert_ne!(
            select_transcript(changed, select, &mut ucn_digest::DigestWorkspace::new()).unwrap(),
            base
        );
    }
    let ack = ProfileAck {
        selected: select,
        transcript_digest: base,
    };
    let mut encoded = [0; 75];
    encode_profile_ack(ack, &mut encoded).unwrap();
    assert_eq!(decode_profile_ack(&encoded), Ok(ack));
    assert!(ack.verify_exact(select.reverse(), base).is_err());
    let before = [0xa5; 75];
    let mut output = before;
    assert_eq!(
        encode_profile_ack(
            ProfileAck {
                transcript_digest: [0; 16],
                ..ack
            },
            &mut output
        ),
        Err(Error::Argument)
    );
    assert_eq!(output, before);
    encoded[59..].fill(0);
    assert!(decode_profile_ack(&encoded).is_err());
}

fn record(generation: u32, link_generation: u32) -> CapabilityRecord {
    CapabilityRecord {
        capability_generation: generation,
        link: LinkCapability {
            link_instance_generation: link_generation,
            carrier_mtu: 1_500,
            link_frame_mtu: 1_400,
            processing_frame_mtu: 1_300,
            carrier_header_bytes: 14,
            carrier_padding_bytes: 2,
            carrier_crc_bytes: 4,
            carrier_tag_bytes: 12,
            carrier_max_fragments: 8,
            link_flags: 0x000C,
            nominal_rate_bps: 1_000_000,
            hardware_priority_count: 4,
            timestamp_capability_bits: 0,
            timestamp_uncertainty_us: 0,
        },
        peer: PeerCapability {
            feature_bits: 0x0000_010B,
            hop_suite_bits: 0x0000_0002,
            e2e_suite_bits: 0x0000_0006,
            max_message_class: MessageClass::T512,
            max_rx_window: 16,
            max_concurrent_transfers: 2,
            realtime_mode_bits: 0,
            clock_domain_id: 0,
            clock_domain_generation: 0,
        },
    }
}

fn candidate(
    contract: HeaderContract,
    bytes: u32,
    dependency: Option<DependencyKind>,
    stable_order: u16,
) -> ContractCandidate {
    ContractCandidate {
        contract,
        origin_security: OriginSecurity::O1,
        hop_profile: HopProfile::H1,
        feature_bits: 0x0000_010B,
        reliable: true,
        realtime: false,
        pinned_path: false,
        payload_budget: 512,
        exact_frame_bytes: bytes,
        setup_cost_bytes: 60,
        expected_reuse_count: 3,
        missing_dependency: dependency,
        stable_order,
    }
}

#[test]
fn record_summary_query_are_big_endian_and_exact() {
    let value = record(0x0102_0304, 0x1112_1314);
    let mut bytes = [0xA5; CAPABILITY_RECORD_BYTES];
    encode_capability_record(value, &mut bytes).expect("record encode");
    let expected = fixture("UCN_V6S_CAPABILITY_RECORD_BYTES");
    assert_eq!(bytes.as_slice(), expected);
    assert_eq!(decode_capability_record(&bytes), Ok(value));
    let digest = capability_digest(value).expect("digest");
    assert_eq!(
        digest.as_slice(),
        fixture("UCN_V6S_CAPABILITY_DIGEST_BYTES")
    );

    let summary = CapabilitySummary {
        capability_generation: value.capability_generation,
        link_instance_generation: value.link.link_instance_generation,
        digest,
    };
    let mut summary_bytes = [0; CAPABILITY_SUMMARY_BYTES];
    encode_capability_summary(summary, &mut summary_bytes).expect("summary encode");
    assert_eq!(
        summary_bytes.as_slice(),
        fixture("UCN_V6S_CAPABILITY_SUMMARY_BYTES")
    );
    assert_eq!(decode_capability_summary(&summary_bytes), Ok(summary));

    let query = CapabilityQuery {
        requested_generation: value.capability_generation,
        known_digest: digest,
    };
    let mut query_bytes = [0; CAPABILITY_QUERY_BYTES];
    encode_capability_query(query, &mut query_bytes).expect("query encode");
    assert_eq!(
        query_bytes.as_slice(),
        fixture("UCN_V6S_CAPABILITY_QUERY_BYTES")
    );
    assert_eq!(decode_capability_query(&query_bytes), Ok(query));
    let before = query_bytes;
    assert_eq!(
        encode_capability_query(
            CapabilityQuery {
                requested_generation: 0,
                known_digest: digest,
            },
            &mut query_bytes
        ),
        Err(Error::Argument)
    );
    assert_eq!(query_bytes, before);
}

#[test]
fn profile_required_fields_never_downgrade() {
    let local = record(2, 3);
    let mut peer = record(4, 5);
    peer.peer.max_message_class = MessageClass::T128;
    peer.peer.max_rx_window = 8;
    let local_digest = capability_digest(local).expect("local digest");
    let peer_digest = capability_digest(peer).expect("peer digest");
    let requirements = ProfileRequirements {
        required_feature_bits: 0x0000_010B,
        required_hop_suite_bits: 2,
        required_e2e_suite_bits: 2,
        minimum_message_class: MessageClass::T128,
        minimum_rx_window: 8,
        minimum_concurrent_transfers: 1,
        required_realtime_mode_bits: 0,
    };
    let selected = ProfileSelect::build(local, local_digest, peer, peer_digest, requirements)
        .expect("exact intersection");
    assert_eq!(selected.max_message_class, MessageClass::T128);
    assert_eq!(selected.max_rx_window, 8);
    let ack = ProfileAck {
        selected,
        transcript_digest: [0x77; 16],
    };
    assert_eq!(ack.verify_exact(selected, [0x77; 16]), Ok(()));
    let mut wrong_digest = local_digest;
    wrong_digest[0] ^= 1;
    assert_eq!(
        ProfileSelect::build(local, wrong_digest, peer, peer_digest, requirements),
        Err(Error::Security)
    );
    let mut changed = selected;
    changed.max_rx_window = 7;
    assert_eq!(ack.verify_exact(changed, [0x77; 16]), Err(Error::Security));
    assert_eq!(ack.verify_exact(selected, [0x76; 16]), Err(Error::Security));
    assert_eq!(ack.verify_exact(selected, [0; 16]), Err(Error::Security));

    let mut impossible = requirements;
    impossible.minimum_message_class = MessageClass::T256;
    assert_eq!(
        ProfileSelect::build(local, local_digest, peer, peer_digest, impossible),
        Err(Error::Unsupported)
    );
}

#[test]
fn resolver_is_deterministic_and_reports_first_typed_dependency() {
    let intent = EffectiveIntent {
        required_feature_bits: 0x0000_010B,
        forbidden_feature_bits: 0,
        security_floor: SecurityFloor::Authenticated,
        reliable_required: true,
        realtime_required: false,
        pinned_path_required: false,
        payload_bytes: 100,
    };
    let resources = ResourceView {
        tx_available: true,
        reliable_available: true,
        transfer_available: true,
    };
    let candidates = [
        candidate(HeaderContract::C4, 80, None, 3),
        candidate(HeaderContract::C2, 70, None, 2),
        candidate(
            HeaderContract::C1,
            60,
            Some(DependencyKind::SecuritySession),
            1,
        ),
    ];
    assert_eq!(
        ContractResolver::resolve(intent, resources, &candidates),
        ResolveResult::Ready(ucn_capability::ContractPlan {
            candidate: candidates[1]
        })
    );
    let pending_only = [
        candidate(HeaderContract::C4, 80, Some(DependencyKind::SoftRoute), 2),
        candidate(
            HeaderContract::C2,
            70,
            Some(DependencyKind::IdentityBinding),
            1,
        ),
    ];
    assert_eq!(
        ContractResolver::resolve(intent, resources, &pending_only),
        ResolveResult::NeedDependency(DependencyKind::IdentityBinding)
    );
    let conflict = EffectiveIntent {
        forbidden_feature_bits: 1,
        required_feature_bits: 1,
        ..intent
    };
    assert_eq!(
        ContractResolver::resolve(conflict, resources, &candidates),
        ResolveResult::RejectPolicy
    );
    assert_eq!(
        ContractResolver::resolve(
            intent,
            ResourceView {
                tx_available: false,
                ..resources
            },
            &[candidate(
                HeaderContract::C1,
                60,
                Some(DependencyKind::IdentityBinding),
                1,
            )]
        ),
        ResolveResult::NeedDependency(DependencyKind::IdentityBinding)
    );
    assert_eq!(
        ContractResolver::resolve(
            intent,
            ResourceView {
                tx_available: false,
                ..resources
            },
            &[candidate(HeaderContract::C1, 60, None, 1)]
        ),
        ResolveResult::RejectResource
    );
    let transfer_only = [candidate(
        HeaderContract::C4,
        80,
        Some(DependencyKind::Transfer),
        1,
    )];
    assert_eq!(
        ContractResolver::resolve(
            intent,
            ResourceView {
                transfer_available: false,
                ..resources
            },
            &transfer_only
        ),
        ResolveResult::RejectResource
    );
}
