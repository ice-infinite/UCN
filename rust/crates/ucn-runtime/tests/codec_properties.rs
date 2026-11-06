//! Deterministic independent raw-negative/property corpus for optional payload codecs.
use ucn_realtime::{
    decode_envelope, decode_time_key, decode_time_stamp, encode_envelope, encode_time_key,
    encode_time_stamp,
};
use ucn_transport::{
    ParentKind, decode_ack, decode_fragment, decode_sack, decode_setup, encode_ack,
    encode_fragment, encode_sack, encode_setup,
};
#[test]
fn fixed_seed_untrusted_bytes_never_panic_and_every_accepted_value_is_canonical() {
    let mut seed = 0x7abc_23deu32;
    let mut input = [0u8; 64];
    let mut out = [0u8; 64];
    for iteration in 0..8192usize {
        for byte in &mut input {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            *byte = seed.to_le_bytes()[0];
        }
        let n = iteration % 65;
        let raw = &input[..n];
        if let Ok(value) = decode_ack(raw) {
            let n = encode_ack(value, &mut out).unwrap();
            assert_eq!(&out[..n], raw);
        }
        if let Ok(value) = decode_setup(raw) {
            let n = encode_setup(value, &mut out).unwrap();
            assert_eq!(&out[..n], raw);
        }
        for kind in [ParentKind::C1, ParentKind::Flow] {
            if let Ok((value, prefix)) = decode_fragment(kind, raw) {
                let n = encode_fragment(value, &mut out).unwrap();
                assert_eq!(n, prefix);
                assert_eq!(&out[..n], &raw[..prefix]);
            }
            if let Ok(value) = decode_sack(kind, raw) {
                let n = encode_sack(value, &mut out).unwrap();
                assert_eq!(&out[..n], raw);
            }
        }
        if let Ok(value) = decode_time_key(raw) {
            let mut b = [0; 12];
            encode_time_key(value, &mut b).unwrap();
            assert_eq!(&b, raw);
        }
        if let Ok(value) = decode_time_stamp(raw) {
            let mut b = [0; 24];
            encode_time_stamp(value, &mut b).unwrap();
            assert_eq!(&b, raw);
        }
        if let Ok(value) = decode_envelope(raw) {
            let mut b = [0; 16];
            encode_envelope(value, &mut b).unwrap();
            assert_eq!(&b, raw);
        }
    }
}
