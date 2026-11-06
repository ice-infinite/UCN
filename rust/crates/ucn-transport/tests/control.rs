#![cfg(feature = "transfer")]
//! Independent response layouts, wrong identity/reserved bytes and output atomicity.
use ucn_transport::*;
use ucn_types::Error;
fn key() -> TransferKey {
    TransferKey {
        kind: ParentKind::C1,
        parent_id: 0,
        parent_generation: 0x0102_0304,
        transfer_id: 0x0506_0708,
        service: 0x1234,
        digest: core::array::from_fn(|i| u8::try_from(i).unwrap()),
    }
}
#[test]
fn complete_transfer_responses_and_zero_write_negatives() {
    let mut b = [0xa5; 64];
    let ack = SetupAck {
        key: key(),
        fragment_bytes: 0x0102,
        credit: 0x0304,
    };
    let n = encode_setup_ack(ack, &mut b).unwrap();
    assert_eq!(n, 33);
    assert_eq!(decode_setup_ack(&b[..n]).unwrap(), ack);
    assert_eq!(&b[..13], &[1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x12, 0x34]);
    assert_eq!(&b[29..33], &[1, 2, 3, 4]);
    for length in [32, 34] {
        assert!(decode_setup_ack(&b[..length]).is_err());
    }
    let before = b;
    assert_eq!(
        encode_setup_ack(
            SetupAck {
                fragment_bytes: 0,
                ..ack
            },
            &mut b
        ),
        Err(Error::Argument)
    );
    assert_eq!(b, before);
    let mut changed = key();
    changed.parent_id = 1;
    assert_eq!(
        encode_setup_ack(
            SetupAck {
                key: changed,
                ..ack
            },
            &mut b
        ),
        Err(Error::Argument)
    );
    assert_eq!(b, before);
    let abort = TransferAbort {
        key: key(),
        reason: 7,
    };
    encode_abort(abort, &mut b).unwrap();
    assert_eq!(decode_abort(&b[..33]).unwrap(), abort);
    b[32] = 1;
    assert_eq!(decode_abort(&b[..33]), Err(Error::Malformed));
    let terminal = TransferTerminal {
        key: key(),
        state: 2,
        result: 0x5678,
    };
    encode_terminal(terminal, &mut b).unwrap();
    assert_eq!(decode_terminal(&b[..32]).unwrap(), terminal);
    b[29] = 4;
    assert_eq!(decode_terminal(&b[..32]), Err(Error::Malformed));
}
