//! Independent raw two-part fixtures and fail-zero-write codec boundaries.
use ucn_capability::*;
use ucn_types::{Error, ProtocolOpcode};
#[test]
fn record_and_ack_parts_match_independent_big_endian_fixtures() {
    for (op, total) in [
        (ProtocolOpcode::CapabilityAdvertise, 68_u16),
        (ProtocolOpcode::CapabilityAck, 75),
    ] {
        let body: Vec<u8> = (0..u8::try_from(total).unwrap()).collect();
        let half = usize::from(total).div_ceil(2);
        for index in 0..2_u8 {
            let start = usize::from(index) * half;
            let chunk = &body[start..(start + half).min(body.len())];
            let mut raw = vec![
                1,
                if total == 68 { 3 } else { 5 },
                0,
                u8::try_from(total).unwrap(),
                index,
                2,
            ];
            raw.extend_from_slice(chunk);
            let v = decode_capability_fragment(&raw).unwrap();
            assert_eq!(
                v,
                CapabilityFragment {
                    opcode: op,
                    total,
                    index,
                    chunk
                }
            );
            let mut output = [0xA5; 44];
            let n = encode_capability_fragment(v, &mut output).unwrap();
            assert_eq!(&output[..n], raw);
            assert!(output[n..].iter().all(|b| *b == 0xA5));
            assert!(decode_capability_fragment(&raw[..raw.len() - 1]).is_err());
            raw.push(0);
            assert!(decode_capability_fragment(&raw).is_err());
        }
    }
}
#[test]
fn all_metadata_and_short_output_mutations_reject_without_writes() {
    let chunk = [0; 38];
    let value = CapabilityFragment {
        opcode: ProtocolOpcode::CapabilityAck,
        total: 75,
        index: 0,
        chunk: &chunk,
    };
    let mut output = [0xA5; 44];
    assert_eq!(
        encode_capability_fragment(value, &mut output[..43]),
        Err(Error::NoSpace)
    );
    assert_eq!(output, [0xA5; 44]);
    for index in [2, 255] {
        assert!(
            encode_capability_fragment(CapabilityFragment { index, ..value }, &mut output).is_err()
        );
        assert_eq!(output, [0xA5; 44]);
    }
    let mut raw = [0; 44];
    encode_capability_fragment(value, &mut raw).unwrap();
    for count in [0, 1, 3, 255] {
        let mut bad = raw;
        bad[5] = count;
        assert!(decode_capability_fragment(&bad).is_err());
    }
    for length in [0_u16, 74, 76, u16::MAX] {
        let mut bad = raw;
        bad[2..4].copy_from_slice(&length.to_be_bytes());
        assert!(decode_capability_fragment(&bad).is_err());
    }
    for op in [0_u16, 0x0104, 0x0108, 0xffff] {
        let mut bad = raw;
        bad[..2].copy_from_slice(&op.to_be_bytes());
        assert!(decode_capability_fragment(&bad).is_err());
    }
}
