//! Independent business golden, not a second Core codec.
#[path = "../examples/support/health_schema.rs"]
mod health_schema;
use health_schema::Health;
use ucn_native::PAYLOAD_BYTES;

#[test]
fn independent_business_golden_and_signed_boundaries() {
    let h = Health {
        voltage_mv: 24000,
        current_ma: -1200,
        temperature_centi_c: 2534,
        mode: 2,
        flags: 1,
        fault_bits: 0x8000_0001,
    };
    let expected = [
        0x00, 0x00, 0x5d, 0xc0, 0xff, 0xff, 0xfb, 0x50, 0x09, 0xe6, 2, 1, 0x80, 0, 0, 1,
    ];
    assert_eq!(h.encode(), expected);
    assert_eq!(Health::decode(&expected), Ok(h));
    assert!(Health::decode(&expected[..15]).is_err());
    assert_eq!(PAYLOAD_BYTES, 64);
}
