#![cfg(feature = "transfer")]
//! Independent fixed byte vectors, not encode/decode same-error agreement.
use ucn_transport::ParentPublication;
use ucn_types::{Error, ProtocolOpcode};

fn golden(disposition: u8) -> [u8; 32] {
    let mut bytes = [
        1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x22, 0x33, 0x44, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        13, 14, 15, 0, 0, 0, 0,
    ];
    bytes[28] = disposition;
    bytes
}
#[test]
fn independent_parent_golden_and_negatives() {
    use ProtocolOpcode::{
        TransportParentAbort, TransportParentAccept, TransportParentCommit, TransportParentPrepare,
    };
    for (op, disposition) in [
        (TransportParentPrepare, 0),
        (TransportParentAccept, 1),
        (TransportParentAccept, 2),
        (TransportParentCommit, 0),
        (TransportParentAbort, 7),
    ] {
        let expected = golden(disposition);
        let value = ParentPublication {
            transaction: 0x0102_0304_0506_0708,
            generation: 0x1122_3344,
            digest: core::array::from_fn(|i| u8::try_from(i).unwrap()),
            disposition,
        };
        let mut output = [0xa5; 33];
        assert_eq!(value.encode(op, &mut output), Ok(32));
        assert_eq!(&output[..32], expected);
        assert_eq!(output[32], 0xa5);
        assert_eq!(ParentPublication::decode(op, &expected), Ok(value));
        for length in [31, 33] {
            assert_eq!(
                ParentPublication::decode(op, &output[..length]),
                Err(Error::Malformed)
            );
        }
        for offset in 29..32 {
            let mut bad = expected;
            bad[offset] = 1;
            assert_eq!(ParentPublication::decode(op, &bad), Err(Error::Malformed));
        }
        for field in [0..8, 8..12, 12..28] {
            let mut bad = expected;
            bad[field].fill(0);
            assert_eq!(ParentPublication::decode(op, &bad), Err(Error::Malformed));
        }
        assert_eq!(
            value.encode(ProtocolOpcode::DiagnosticPing, &mut output),
            Err(Error::Argument)
        );
        assert_eq!(&output[..32], expected);
        let mut small = [0xa5; 31];
        assert_eq!(value.encode(op, &mut small), Err(Error::NoSpace));
        assert_eq!(small, [0xa5; 31]);
    }
    for (op, disposition) in [
        (TransportParentPrepare, 1),
        (TransportParentCommit, 2),
        (TransportParentAccept, 0),
        (TransportParentAccept, 3),
        (TransportParentAbort, 0),
    ] {
        assert_eq!(
            ParentPublication::decode(op, &golden(disposition)),
            Err(Error::Malformed)
        );
    }
    let mut terminal = golden(0);
    terminal[..12].fill(0xff);
    assert!(ParentPublication::decode(TransportParentPrepare, &terminal).is_ok());
}

#[test]
fn directional_transfer_capacity_has_no_hidden_receiver_requirement() {
    assert!(ucn_transport::TransferOwner::<1, 0, 64, 4, 0>::new(1, 1000, 0xff00).is_ok());
    assert!(ucn_transport::TransferOwner::<0, 1, 64, 4, 1>::new(1, 1000, 0xff00).is_ok());
    assert!(matches!(
        ucn_transport::TransferOwner::<0, 0, 64, 4, 0>::new(1, 1000, 0xff00),
        Err(Error::Config)
    ));
    assert!(matches!(
        ucn_transport::TransferOwner::<0, 1, 64, 4, 0>::new(1, 1000, 0xff00),
        Err(Error::Config)
    ));
}
