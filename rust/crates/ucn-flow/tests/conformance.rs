//! Independent byte fixtures and fixed-seed properties for Flow codecs.

use std::vec::Vec;

use ucn_flow::{
    C2_PREFIX_BYTES, C3_PREFIX_BYTES, C4_PREFIX_BYTES, FlowPrefix, LABEL_SETUP_BYTES, LabelSetup,
    decode_flow_prefix, decode_label_setup, encode_flow_prefix, encode_label_setup,
};
use ucn_types::{
    CandidateId, ContextId, DeliveryGuarantee, Error, ForwardingLabel, HeaderContract, HopLimit,
    InteractionRole, OriginSecurity, OriginSequence, PayloadKind, RouteGeneration, TrafficClass,
};
use ucn_wire::CommonHeader;

const SHARED_FIXTURES: &str = include_str!("../../../tests/conformance/v6s_routing_flow_v1.h");

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

fn header(contract: HeaderContract) -> CommonHeader {
    CommonHeader {
        contract,
        traffic_class: TrafficClass::Q1,
        delivery: if contract == HeaderContract::C3 {
            DeliveryGuarantee::BestEffort
        } else {
            DeliveryGuarantee::Reliable
        },
        interaction: if contract == HeaderContract::C3 {
            InteractionRole::OneWay
        } else {
            InteractionRole::Request
        },
        payload_kind: if contract == HeaderContract::C3 {
            PayloadKind::Data
        } else {
            PayloadKind::Control
        },
        origin_security: if contract == HeaderContract::C3 {
            OriginSecurity::O0
        } else {
            OriginSecurity::O1
        },
        hop_limit: HopLimit::new(7).expect("hop"),
    }
}

fn setup() -> LabelSetup {
    LabelSetup {
        candidate_id: CandidateId::new(0x1122).expect("candidate"),
        route_generation: RouteGeneration::new(0x3344_5566).expect("route"),
        reverse_label: ForwardingLabel::new(0x7788).expect("label"),
        forward_label: ForwardingLabel::new(0x99AA).expect("label"),
        path_profile_id: 0xBBCC,
        context_digest: 0xDDEE_F001,
    }
}

fn prefixes() -> [FlowPrefix; 3] {
    [
        FlowPrefix::C2 {
            header: header(HeaderContract::C2),
            context_id: ContextId::new(0x1122).expect("context"),
            sequence: OriginSequence::new(0x3344_5566).expect("sequence"),
        },
        FlowPrefix::C3 {
            header: header(HeaderContract::C3),
            label: ForwardingLabel::new(0x1122).expect("label"),
            context_id: ContextId::new(0x3344).expect("context"),
        },
        FlowPrefix::C4 {
            header: header(HeaderContract::C4),
            label: ForwardingLabel::new(0x1122).expect("label"),
            context_id: ContextId::new(0x3344).expect("context"),
            sequence: OriginSequence::new(0x5566_7788).expect("sequence"),
        },
    ]
}

#[test]
fn flow_codecs_match_language_neutral_big_endian_fixtures() {
    let mut setup_bytes = [0xA5; LABEL_SETUP_BYTES];
    encode_label_setup(setup(), &mut setup_bytes).expect("setup encode");
    assert_eq!(
        setup_bytes.as_slice(),
        fixture("UCN_V6S_FLOW_LABEL_SETUP_BYTES")
    );
    assert_eq!(decode_label_setup(&setup_bytes), Ok(setup()));

    for (value, name) in prefixes().into_iter().zip([
        "UCN_V6S_FLOW_C2_PREFIX_BYTES",
        "UCN_V6S_FLOW_C3_PREFIX_BYTES",
        "UCN_V6S_FLOW_C4_PREFIX_BYTES",
    ]) {
        let mut bytes = [0xA5; C4_PREFIX_BYTES];
        let size = value.encoded_size();
        encode_flow_prefix(value, &mut bytes[..size]).expect("prefix encode");
        assert_eq!(&bytes[..size], fixture(name));
        assert_eq!(decode_flow_prefix(&bytes[..size]), Ok(value));
    }
}

#[test]
fn flow_negative_matrix_rejects_lengths_reserved_fields_and_cross_contracts() {
    for name in [
        "UCN_V6S_FLOW_C2_PREFIX_BYTES",
        "UCN_V6S_FLOW_C3_PREFIX_BYTES",
        "UCN_V6S_FLOW_C4_PREFIX_BYTES",
    ] {
        let bytes = fixture(name);
        assert_eq!(
            decode_flow_prefix(&bytes[..bytes.len() - 1]),
            Err(Error::Malformed)
        );
        let mut wrong_contract = bytes;
        wrong_contract[0] = (wrong_contract[0] & 0xF0) | 1;
        assert_eq!(decode_flow_prefix(&wrong_contract), Err(Error::Malformed));
    }
    let mut setup_bad = fixture("UCN_V6S_FLOW_LABEL_SETUP_BYTES");
    setup_bad[0..2].fill(0);
    assert_eq!(decode_label_setup(&setup_bad), Err(Error::Malformed));
    setup_bad = fixture("UCN_V6S_FLOW_LABEL_SETUP_BYTES");
    setup_bad[10..12].fill(0);
    assert_eq!(decode_label_setup(&setup_bad), Err(Error::Malformed));
    setup_bad = fixture("UCN_V6S_FLOW_LABEL_SETUP_BYTES");
    setup_bad[12..16].fill(0);
    assert_eq!(decode_label_setup(&setup_bad), Err(Error::Malformed));
}

#[test]
fn flow_encode_failures_leave_outputs_unchanged() {
    let mut output = [0xA5; LABEL_SETUP_BYTES];
    let before = output;
    let mut invalid = setup();
    invalid.path_profile_id = 0;
    assert_eq!(
        encode_label_setup(invalid, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);
    assert_eq!(
        encode_flow_prefix(prefixes()[2], &mut output[..C4_PREFIX_BYTES - 1]),
        Err(Error::NoSpace)
    );
    assert_eq!(output, before);
}

#[test]
fn fixed_seed_flow_prefix_roundtrip_covers_all_three_contracts() {
    let mut state = 0xC407_CAFE_u32;
    let mut output = [0_u8; C4_PREFIX_BYTES];
    for index in 0..4096_u32 {
        state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        let contract = match index % 3 {
            0 => HeaderContract::C2,
            1 => HeaderContract::C3,
            _ => HeaderContract::C4,
        };
        let context_low = state % u32::from(u16::MAX - 1);
        let context =
            ContextId::new(1 + u16::try_from(context_low).expect("context fixture is bounded"))
                .expect("context");
        let label_low = (state >> 8) % u32::from(u16::MAX - 1);
        let label =
            ForwardingLabel::new(1 + u16::try_from(label_low).expect("label fixture is bounded"))
                .expect("label");
        let sequence = OriginSequence::new(if index == 4095 { u32::MAX } else { index + 1 })
            .expect("sequence");
        let value = match contract {
            HeaderContract::C2 => FlowPrefix::C2 {
                header: header(contract),
                context_id: context,
                sequence,
            },
            HeaderContract::C3 => FlowPrefix::C3 {
                header: header(contract),
                label,
                context_id: context,
            },
            HeaderContract::C4 => FlowPrefix::C4 {
                header: header(contract),
                label,
                context_id: context,
                sequence,
            },
            _ => unreachable!(),
        };
        let size = value.encoded_size();
        encode_flow_prefix(value, &mut output[..size]).expect("generated prefix");
        assert_eq!(decode_flow_prefix(&output[..size]), Ok(value));
    }
    assert_eq!(C2_PREFIX_BYTES + C3_PREFIX_BYTES + C4_PREFIX_BYTES, 27);
}
