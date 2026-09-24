//! Independent byte fixtures for RREQ/RREP/RERR.

use std::vec::Vec;

use ucn_routing::{
    RERR_PAYLOAD_BYTES, RREP_PAYLOAD_BYTES, RREQ_PAYLOAD_BYTES, RerrPayload, RerrReason,
    RrepPayload, RreqPayload, decode_rerr_payload, decode_rrep_payload, decode_rreq_payload,
    encode_rerr_payload, encode_rrep_payload, encode_rreq_payload,
};
use ucn_types::Error;

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

fn rreq() -> RreqPayload {
    RreqPayload {
        accumulated_cost: 0x0102_0304,
        minimum_payload_budget: 0x1122,
        required_capability_bits: 0x3344,
        flags: 0x03,
    }
}

fn rrep() -> RrepPayload {
    RrepPayload {
        destination_principal: [0xA5; 16],
        destination_binding_generation: 0x0102_0304,
        hop_count: 0,
        accumulated_cost: 0x1122_3344,
        path_frame_mtu: 0x5566,
        capability_bits: 0x7788,
    }
}

fn rerr() -> RerrPayload {
    RerrPayload {
        realm: 0x0102_0304,
        origin_principal: [0x11; 16],
        origin_address: 0x1122_3344,
        origin_binding_generation: 0x0102_0304,
        origin_session_generation: 0x0506_0708,
        destination_principal: [0x22; 16],
        destination_address: 0x5566_7788,
        destination_binding_generation: 0x1112_1314,
        route_generation: 0x2122_2324,
        route_causal_id: 0x3132_3334_3536_3738,
        reporter_principal: [0x33; 16],
        reporter_address: 0x99AA_BBCC,
        reporter_binding_generation: 0x4142_4344,
        failed_link_id: 0x5152,
        failed_link_generation: 0x6162_6364,
        reason: RerrReason::PathContractChanged,
    }
}

#[test]
fn routing_payloads_match_language_neutral_big_endian_fixtures() {
    let mut request_bytes = [0xA5; RREQ_PAYLOAD_BYTES];
    encode_rreq_payload(rreq(), &mut request_bytes).expect("RREQ encode");
    assert_eq!(
        request_bytes.as_slice(),
        fixture("UCN_V6S_ROUTE_RREQ_BYTES")
    );
    assert_eq!(decode_rreq_payload(&request_bytes), Ok(rreq()));

    let mut reply_bytes = [0xA5; RREP_PAYLOAD_BYTES];
    encode_rrep_payload(rrep(), &mut reply_bytes).expect("RREP encode");
    assert_eq!(reply_bytes.as_slice(), fixture("UCN_V6S_ROUTE_RREP_BYTES"));
    assert_eq!(decode_rrep_payload(&reply_bytes), Ok(rrep()));

    let mut rerr_bytes = [0xA5; RERR_PAYLOAD_BYTES];
    encode_rerr_payload(rerr(), &mut rerr_bytes).expect("RERR encode");
    assert_eq!(rerr_bytes.as_slice(), fixture("UCN_V6S_ROUTE_RERR_BYTES"));
    assert_eq!(decode_rerr_payload(&rerr_bytes), Ok(rerr()));
}

#[test]
fn routing_codecs_reject_exact_length_and_reserved_field_mutations() {
    for name in [
        "UCN_V6S_ROUTE_RREQ_BYTES",
        "UCN_V6S_ROUTE_RREP_BYTES",
        "UCN_V6S_ROUTE_RERR_BYTES",
    ] {
        let bytes = fixture(name);
        let short = &bytes[..bytes.len() - 1];
        assert!(
            matches!(
                name,
                "UCN_V6S_ROUTE_RREQ_BYTES"
                    if decode_rreq_payload(short) == Err(Error::Malformed)
            ) || matches!(
                name,
                "UCN_V6S_ROUTE_RREP_BYTES"
                    if decode_rrep_payload(short) == Err(Error::Malformed)
            ) || matches!(
                name,
                "UCN_V6S_ROUTE_RERR_BYTES"
                    if decode_rerr_payload(short) == Err(Error::Malformed)
            )
        );
    }

    let mut bad_request = fixture("UCN_V6S_ROUTE_RREQ_BYTES");
    bad_request[8] = 0x80;
    assert_eq!(decode_rreq_payload(&bad_request), Err(Error::Malformed));

    let mut bad_reply = fixture("UCN_V6S_ROUTE_RREP_BYTES");
    bad_reply[0..16].fill(0);
    assert_eq!(decode_rrep_payload(&bad_reply), Err(Error::Malformed));

    let mut rerr_bad = fixture("UCN_V6S_ROUTE_RERR_BYTES");
    rerr_bad[98] = 0xFF;
    assert_eq!(decode_rerr_payload(&rerr_bad), Err(Error::Malformed));
}

#[test]
fn routing_encode_failures_do_not_write_outputs() {
    let mut output = [0xA5; RERR_PAYLOAD_BYTES];
    let before = output;
    assert_eq!(
        encode_rreq_payload(rreq(), &mut output[..RREQ_PAYLOAD_BYTES - 1]),
        Err(Error::NoSpace)
    );
    assert_eq!(output, before);

    let mut invalid = rerr();
    invalid.failed_link_id = 0;
    assert_eq!(
        encode_rerr_payload(invalid, &mut output),
        Err(Error::Argument)
    );
    assert_eq!(output, before);
}
