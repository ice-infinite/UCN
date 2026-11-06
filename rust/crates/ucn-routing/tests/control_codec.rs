//! Independent canonical inputs, not encoder/decoder self-agreement.
use ucn_routing::{RouteControl, RouteFragment, route_fragment_count};
use ucn_types::{AddressWidth, C0TransactionId, ProtocolOpcode};
fn key() -> Vec<u8> {
    // Independent BE fixture: realm7, origin1/binding2/principalA1,
    // origin-session3,target9,discovery0x0102030405060708.
    let mut raw = vec![0, 0, 0, 7, 0, 0, 0, 1, 0, 0, 0, 2];
    raw.extend([0xa1; 16]);
    raw.extend([0, 0, 0, 3, 0, 0, 0, 9, 1, 2, 3, 4, 5, 6, 7, 8]);
    raw
}
#[test]
fn independent_request_and_reply_vectors_and_terminal_generation() {
    let mut request = key();
    request.extend([8, 1, 2, 3, 4, 0, 100, 0, 3, 0]);
    let value =
        RouteControl::decode(ProtocolOpcode::RouteRreq, AddressWidth::A1, &request).unwrap();
    let mut output = [0xcc; 99];
    assert_eq!(value.encode(AddressWidth::A1, &mut output).unwrap(), 54);
    assert_eq!(&output[..54], request);
    assert_eq!(&output[54..], &[0xcc; 45]);
    let mut reply = key();
    reply.extend([0xc3; 16]);
    reply.extend([0, 0, 0, 4, 1, 0, 0, 0, 12, 0, 127, 0, 3, 255, 255, 255, 255]);
    let value = RouteControl::decode(ProtocolOpcode::RouteRrep, AddressWidth::A1, &reply).unwrap();
    assert_eq!(value.encode(AddressWidth::A1, &mut output).unwrap(), 77);
    assert_eq!(&output[..77], reply);
    // Zero-hop target must not claim a relay's route generation.
    reply[64] = 0;
    assert!(RouteControl::decode(ProtocolOpcode::RouteRrep, AddressWidth::A1, &reply).is_err());
    reply[73..77].fill(0);
    assert!(RouteControl::decode(ProtocolOpcode::RouteRrep, AddressWidth::A1, &reply).is_ok());
}
#[test]
fn field_and_length_negatives_never_touch_encoder_output() {
    let mut raw = key();
    raw.extend([8, 0, 0, 0, 0, 0, 100, 0, 3, 0]);
    for offset in [0, 4, 8, 28, 32, 36] {
        let mut bad = raw.clone();
        let length = if offset == 36 { 8 } else { 4 };
        bad[offset..offset + length].fill(0);
        assert!(RouteControl::decode(ProtocolOpcode::RouteRreq, AddressWidth::A1, &bad).is_err());
    }
    for length in [0, 9, 53, 55] {
        let mut bad = raw.clone();
        bad.resize(length, 0);
        assert!(RouteControl::decode(ProtocolOpcode::RouteRreq, AddressWidth::A1, &bad).is_err());
    }
    let good = RouteControl::decode(ProtocolOpcode::RouteRreq, AddressWidth::A1, &raw).unwrap();
    let mut short = [0xcc; 53];
    let before = short;
    assert!(good.encode(AddressWidth::A1, &mut short).is_err());
    assert_eq!(short, before);
    let RouteControl::Request(mut bad) = good else {
        panic!()
    };
    bad.key.origin.principal = [255; 16];
    let mut output = [0xcc; 99];
    assert!(
        RouteControl::Request(bad)
            .encode(AddressWidth::A1, &mut output)
            .is_err()
    );
    assert_eq!(output, [0xcc; 99]);
}
#[test]
fn fragment_balance_all_widths_mtu_limits_and_zero_write() {
    for w in [
        AddressWidth::A0,
        AddressWidth::A1,
        AddressWidth::A2,
        AddressWidth::A3,
    ] {
        for op in [ProtocolOpcode::RouteRrep, ProtocolOpcode::RouteRerr] {
            let n = if op == ProtocolOpcode::RouteRrep {
                77
            } else {
                99
            };
            let body = vec![0xa5; n];
            let count = route_fragment_count(op, w, 127).unwrap();
            let mut whole = vec![0; n];
            for index in 0..count {
                let value = RouteFragment::from_body(
                    op,
                    C0TransactionId::new(u64::MAX).unwrap(),
                    count,
                    index,
                    &body,
                )
                .unwrap();
                let mut raw = [0xcc; 64];
                let length = value.encode(&mut raw).unwrap();
                assert!(25 + 2 * w.bytes() + 32 + length <= 127);
                assert_eq!(RouteFragment::decode(&raw[..length]), Ok(value));
                let (offset, length) = value.span().unwrap();
                whole[offset..offset + length].copy_from_slice(&value.chunk[..length]);
                let mut invalid = value;
                invalid.count = 1;
                let before = raw;
                assert!(invalid.encode(&mut raw).is_err());
                assert_eq!(raw, before);
                let mut bad = raw;
                bad[5] = 0;
                assert!(RouteFragment::decode(&bad[..14 + length]).is_err());
            }
            assert_eq!(whole, body);
        }
    }
    assert!(route_fragment_count(ProtocolOpcode::RouteRreq, AddressWidth::A1, 100).is_err());
}

#[test]
fn fixed_seed_raw_mutations_are_canonical_or_rejected_without_panics() {
    let mut base = key();
    base.extend([8, 0, 0, 0, 0, 0, 100, 0, 3, 0]);
    let mut seed = 0x1357_2468u32;
    for _ in 0..4096 {
        seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        let mut raw = base.clone();
        let index = usize::try_from(seed).unwrap() % raw.len();
        raw[index] ^= u8::try_from(seed >> 24).unwrap();
        if let Ok(value) = RouteControl::decode(ProtocolOpcode::RouteRreq, AddressWidth::A1, &raw) {
            let mut output = [0xcc; 99];
            let length = value.encode(AddressWidth::A1, &mut output).unwrap();
            assert_eq!(&output[..length], raw);
        }
        let length = usize::try_from(seed % 80).unwrap();
        let fuzz = vec![u8::try_from(seed >> 16 & 255).unwrap(); length];
        let _ = RouteFragment::decode(&fuzz);
    }
}
