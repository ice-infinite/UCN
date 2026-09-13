//! Shared C/Rust Golden, Negative and fixed-seed conformance tests.

use std::vec::Vec;

use ucn_types::{
    AddressWidth, BindingGeneration, C0TransactionId, DeliveryGuarantee, Error, HeaderContract,
    HopLimit, HopProfile, InteractionRole, NodeAddress, OriginSecurity, OriginSequence,
    PayloadKind, ProtocolOpcode, RealmId, ServiceId, TrafficClass,
};
use ucn_wire::{
    C0Address, C0Frame, C1Frame, CommonHeader, c0_o0_h0_encoded_size, c1_o0_h0_encoded_size,
    decode_c0_o0_h0, decode_c1_o0_h0, encode_c0_o0_h0, encode_c1_o0_h0,
};

const SHARED_FIXTURES: &str = include_str!("../../../tests/conformance/v6s_wire_core_v1.h");

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
                bytes.push(u8::from_str_radix(hex, 16).expect("fixture byte must be hexadecimal"));
            }
        }

        if collecting && !line.trim_end().ends_with('\\') {
            break;
        }
    }

    assert!(!bytes.is_empty(), "fixture {name} must exist");
    bytes
}

fn common(
    contract: HeaderContract,
    traffic_class: TrafficClass,
    delivery: DeliveryGuarantee,
    payload_kind: PayloadKind,
    hop_limit: u8,
) -> CommonHeader {
    CommonHeader {
        contract,
        traffic_class,
        delivery,
        interaction: InteractionRole::OneWay,
        payload_kind,
        origin_security: OriginSecurity::O0,
        hop_limit: HopLimit::new(hop_limit).expect("valid hop limit"),
    }
}

fn c0_bootstrap(payload: &[u8]) -> C0Frame<'_> {
    C0Frame {
        common: common(
            HeaderContract::C0,
            TrafficClass::Q0,
            DeliveryGuarantee::BestEffort,
            PayloadKind::Control,
            1,
        ),
        realm_id: RealmId::new(0x0102_0304).expect("valid realm"),
        source: C0Address::Unbound,
        destination: C0Address::LinkLocalAuthority,
        source_binding_generation: BindingGeneration::UNBOUND,
        destination_binding_generation: BindingGeneration::UNBOUND,
        transaction_id: C0TransactionId::new(0x0102_0304_0506_0708).expect("valid transaction"),
        opcode: ProtocolOpcode::BootstrapHello,
        payload,
    }
}

#[allow(clippy::too_many_arguments)]
fn c1_data(
    width: AddressWidth,
    source: u32,
    destination: u32,
    service: u16,
    sequence: u32,
    traffic_class: TrafficClass,
    hop_limit: u8,
    payload: &[u8],
) -> C1Frame<'_> {
    C1Frame {
        common: common(
            HeaderContract::C1,
            traffic_class,
            DeliveryGuarantee::BestEffort,
            PayloadKind::Data,
            hop_limit,
        ),
        source: NodeAddress::new(source, width).expect("valid source"),
        destination: NodeAddress::new(destination, width).expect("valid destination"),
        service_id: ServiceId::new(service).expect("valid service"),
        origin_sequence: OriginSequence::new(sequence).expect("valid sequence"),
        payload,
    }
}

#[test]
fn c0_matches_the_frozen_a1_bootstrap_golden() {
    let expected = fixture("UCN_V6S_WIRE_C0_A1_O0_H0_BOOTSTRAP_BYTES");
    let frame = c0_bootstrap(&[0xAA, 0x55]);
    let mut output = [0xA5; 64];

    let bytes = encode_c0_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output)
        .expect("C0 Golden must encode");
    assert_eq!(&output[..bytes], expected);

    let decoded = decode_c0_o0_h0(&expected, AddressWidth::A1, HopProfile::H0)
        .expect("C0 Golden must decode");
    assert_eq!(decoded, frame);
}

#[test]
fn c0_and_c1_base_lengths_follow_all_address_width_formulas() {
    for (width, c0_bytes, c1_bytes) in [
        (AddressWidth::A0, 27, 11),
        (AddressWidth::A1, 29, 13),
        (AddressWidth::A2, 31, 15),
        (AddressWidth::A3, 33, 17),
    ] {
        assert_eq!(c0_o0_h0_encoded_size(width, 0), Ok(c0_bytes));
        assert_eq!(c1_o0_h0_encoded_size(width, 0), Ok(c1_bytes));
    }
}

#[test]
fn c1_matches_the_shared_c_and_rust_goldens_for_all_address_widths() {
    let cases = [
        (
            "UCN_V6S_WIRE_C1_A0_O0_H0_DATA_BYTES",
            AddressWidth::A0,
            c1_data(
                AddressWidth::A0,
                1,
                2,
                0x1234,
                0x1122_3344,
                TrafficClass::Q2,
                5,
                &[0xDE, 0xAD],
            ),
        ),
        (
            "UCN_V6S_WIRE_C1_A1_O0_H0_DATA_BYTES",
            AddressWidth::A1,
            c1_data(
                AddressWidth::A1,
                0x1234,
                0x5678,
                0x0102,
                0x0102_0304,
                TrafficClass::Q1,
                3,
                &[0xDE, 0xAD, 0xBE, 0xEF],
            ),
        ),
        (
            "UCN_V6S_WIRE_C1_A2_O0_H0_DATA_BYTES",
            AddressWidth::A2,
            c1_data(
                AddressWidth::A2,
                0x01_0203,
                0x0A_0B0C,
                0x0102,
                0xA1B2_C3D4,
                TrafficClass::Q1,
                1,
                &[0x00, 0xFF],
            ),
        ),
        (
            "UCN_V6S_WIRE_C1_A3_O0_H0_DATA_BYTES",
            AddressWidth::A3,
            c1_data(
                AddressWidth::A3,
                0x0102_0304,
                0xA1A2_A3A4,
                0xBEEF,
                0x89AB_CDEF,
                TrafficClass::Q0,
                32,
                &[0x55],
            ),
        ),
    ];

    for (name, width, frame) in cases {
        let expected = fixture(name);
        let mut output = [0xA5; 64];
        let bytes = encode_c1_o0_h0(&frame, width, HopProfile::H0, &mut output)
            .expect("C1 Golden must encode");
        assert_eq!(&output[..bytes], expected, "{name}");
        assert_eq!(
            decode_c1_o0_h0(&expected, width, HopProfile::H0),
            Ok(frame),
            "{name}"
        );
    }
}

#[test]
fn encode_failures_leave_output_unchanged() {
    let mut output = [0xA5; 64];
    let before = output;
    let frame = c1_data(
        AddressWidth::A1,
        0x1234,
        0x5678,
        1,
        1,
        TrafficClass::Q1,
        1,
        &[1, 2, 3],
    );

    assert_eq!(
        encode_c1_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output[..12]),
        Err(Error::NoSpace)
    );
    assert_eq!(output, before);

    let mut invalid = frame;
    invalid.common.contract = HeaderContract::C0;
    assert_eq!(
        encode_c1_o0_h0(&invalid, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);

    invalid = frame;
    invalid.common.payload_kind = PayloadKind::Transfer;
    invalid.payload = &[0x80, 0x01];
    assert_eq!(
        encode_c1_o0_h0(&invalid, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);
}

#[test]
fn raw_decoder_rejects_reserved_and_cross_contract_values() {
    let golden = fixture("UCN_V6S_WIRE_C1_A1_O0_H0_DATA_BYTES");

    for (offset, value, expected) in [
        (
            0,
            fixture("UCN_V6S_NEG_BAD_VERSION_BYTE0")[0],
            Error::Malformed,
        ),
        (
            0,
            fixture("UCN_V6S_NEG_RESERVED_CONTRACT_BYTE0")[0],
            Error::Malformed,
        ),
        (
            0,
            fixture("UCN_V6S_NEG_OTHER_CONTRACT_BYTE0")[0],
            Error::Unsupported,
        ),
        (
            1,
            golden[1] | fixture("UCN_V6S_NEG_RESERVED_DELIVERY_MASK")[0],
            Error::Malformed,
        ),
        (
            2,
            fixture("UCN_V6S_NEG_ZERO_HOP_BYTE2")[0],
            Error::Malformed,
        ),
        (
            2,
            fixture("UCN_V6S_NEG_RESERVED_ORIGIN_BYTE2")[0],
            Error::Malformed,
        ),
    ] {
        let mut bad = golden.clone();
        bad[offset] = value;
        assert_eq!(
            decode_c1_o0_h0(&bad, AddressWidth::A1, HopProfile::H0),
            Err(expected)
        );
    }

    for range in [3..5, 5..7, 7..9, 9..13] {
        let mut bad = golden.clone();
        bad[range].fill(0);
        assert_eq!(
            decode_c1_o0_h0(&bad, AddressWidth::A1, HopProfile::H0),
            Err(Error::Malformed)
        );
    }
}

#[test]
fn c0_rejects_wrong_domain_binding_and_opcode_before_writing() {
    let mut output = [0xA5; 64];
    let before = output;
    let mut frame = c0_bootstrap(&[]);

    frame.common.hop_limit = HopLimit::new(2).expect("valid non-bootstrap hop");
    assert_eq!(
        encode_c0_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);

    frame = c0_bootstrap(&[]);
    frame.destination_binding_generation = BindingGeneration::active(1).expect("active binding");
    assert_eq!(
        encode_c0_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);

    frame = c0_bootstrap(&[]);
    frame.opcode = ProtocolOpcode::DiagnosticPing;
    assert_eq!(
        encode_c0_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);

    frame = c0_bootstrap(&[]);
    frame.source =
        C0Address::Bound(NodeAddress::new(1, AddressWidth::A1).expect("valid ordinary address"));
    assert_eq!(
        encode_c0_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);

    frame = c0_bootstrap(&[]);
    frame.destination =
        C0Address::Bound(NodeAddress::new(2, AddressWidth::A1).expect("valid ordinary address"));
    assert_eq!(
        encode_c0_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);
}

#[test]
fn c0_decoder_rejects_broken_bootstrap_identity() {
    let golden = fixture("UCN_V6S_WIRE_C0_A1_O0_H0_BOOTSTRAP_BYTES");
    let mutations = [(3..7, 0_u8), (11..15, 1_u8), (19..27, 0_u8), (27..29, 0_u8)];

    for (range, value) in mutations {
        let mut bad = golden.clone();
        bad[range].fill(value);
        assert_eq!(
            decode_c0_o0_h0(&bad, AddressWidth::A1, HopProfile::H0),
            Err(Error::Malformed)
        );
    }

    let mut wrong_source = golden.clone();
    wrong_source[8] = 1;
    assert_eq!(
        decode_c0_o0_h0(&wrong_source, AddressWidth::A1, HopProfile::H0),
        Err(Error::Malformed)
    );

    let mut wrong_destination = golden.clone();
    wrong_destination[10] = 0xFE;
    assert_eq!(
        decode_c0_o0_h0(&wrong_destination, AddressWidth::A1, HopProfile::H0),
        Err(Error::Malformed)
    );

    let mut late = golden;
    late[2] = 2;
    assert_eq!(
        decode_c0_o0_h0(&late, AddressWidth::A1, HopProfile::H0),
        Err(Error::Malformed)
    );
}

#[test]
fn c1_control_diagnostic_and_terminal_sequence_are_exact() {
    let mut output = [0_u8; 64];
    let mut control = c1_data(
        AddressWidth::A1,
        1,
        2,
        1,
        u32::MAX,
        TrafficClass::Q0,
        1,
        &[0x02, 0x01, 0xAA],
    );
    control.common.payload_kind = PayloadKind::Control;
    let bytes = encode_c1_o0_h0(&control, AddressWidth::A1, HopProfile::H0, &mut output)
        .expect("registered control and terminal sequence are legal");
    assert_eq!(
        decode_c1_o0_h0(&output[..bytes], AddressWidth::A1, HopProfile::H0),
        Ok(control)
    );

    let mut diagnostic = control;
    diagnostic.common.payload_kind = PayloadKind::Diagnostic;
    diagnostic.payload = &[0x09, 0x01];
    assert!(encode_c1_o0_h0(&diagnostic, AddressWidth::A1, HopProfile::H0, &mut output).is_ok());

    diagnostic.payload = &[0x02, 0x01];
    assert_eq!(
        encode_c1_o0_h0(&diagnostic, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Argument)
    );

    let mut transfer = control;
    transfer.common.payload_kind = PayloadKind::Transfer;
    for payload in [&[0x00, 0x01, 0xAA][..], &[0x80, 0x00, 0x55][..]] {
        transfer.payload = payload;
        assert!(encode_c1_o0_h0(&transfer, AddressWidth::A1, HopProfile::H0, &mut output).is_ok());
    }
}

#[test]
fn protected_profiles_are_not_falsely_exposed_before_security_owner() {
    let mut output = [0xA5; 64];
    let before = output;
    let mut frame = c1_data(AddressWidth::A1, 1, 2, 1, 1, TrafficClass::Q1, 1, &[]);
    frame.common.origin_security = OriginSecurity::O1;

    assert_eq!(
        encode_c1_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output),
        Err(Error::Unsupported)
    );
    assert_eq!(output, before);
    assert_eq!(
        decode_c1_o0_h0(
            &fixture("UCN_V6S_WIRE_C1_A1_O0_H0_DATA_BYTES"),
            AddressWidth::A1,
            HopProfile::H1,
        ),
        Err(Error::Unsupported)
    );

    let mut protected = fixture("UCN_V6S_WIRE_C1_A1_O0_H0_DATA_BYTES");
    protected[2] |= 0x40;
    assert_eq!(
        decode_c1_o0_h0(&protected, AddressWidth::A1, HopProfile::H0),
        Err(Error::Unsupported)
    );
}

#[test]
fn fixed_seed_c1_roundtrip_covers_4096_valid_frames() {
    let mut state = 0xC0DE_CAFE_u32;
    let mut payload = [0_u8; 17];
    let mut output = [0_u8; 64];

    for index in 0..4096_u32 {
        state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        for byte in &mut payload {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            *byte = state.to_be_bytes()[0];
        }
        let payload_bytes = usize::try_from(index % 18).expect("bounded payload length");
        let source = 1 + (state & 0x7FFE);
        let destination = 1 + ((state >> 1) & 0x7FFE);
        let sequence = if index == 4095 { u32::MAX } else { index + 1 };
        let frame = c1_data(
            AddressWidth::A1,
            source,
            destination,
            1 + u16::try_from(index % 0xFFFE).expect("bounded service"),
            sequence,
            TrafficClass::try_from(u8::try_from(index % 4).expect("bounded class"))
                .expect("valid class"),
            1 + u8::try_from(index % 63).expect("bounded hop"),
            &payload[..payload_bytes],
        );
        let bytes = encode_c1_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut output)
            .expect("valid generated frame");
        assert_eq!(
            decode_c1_o0_h0(&output[..bytes], AddressWidth::A1, HopProfile::H0),
            Ok(frame)
        );
    }
}
