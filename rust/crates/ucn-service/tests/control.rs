//! Independent conditional Envelope and immutable Operation control identity.
use ucn_service::*;
use ucn_types::{Error, InteractionRole};
fn key() -> OperationWireKey {
    OperationWireKey {
        operation: 0x0102_0304_0506_0708,
        service: 0x1234,
        opcode: 0x2345,
        request_digest: core::array::from_fn(|i| u8::try_from(i).unwrap()),
    }
}
#[test]
fn recovery_codec_exact_lengths_reserved_phases_and_zero_writes() {
    let receipt = OperationReceipt {
        key: key(),
        result_digest: [0xa5; 16],
        phase: 3,
    };
    let mut result = OperationResult {
        receipt,
        status: -12,
        length: 2,
        payload: [0; 146],
    };
    result.payload[..2].copy_from_slice(b"ok");
    let mut b = [0x5a; 201];
    let n = encode_operation_result(result, &mut b).unwrap();
    assert_eq!(n, 56);
    assert_eq!(&b[48..56], &[255, 255, 255, 244, 0, 2, b'o', b'k']);
    assert_eq!(decode_operation_result(&b[..n]).unwrap(), result);
    for n in [53, 55, 57] {
        assert_eq!(decode_operation_result(&b[..n]), Err(Error::Malformed));
    }
    let before = b;
    assert_eq!(
        encode_operation_result(result, &mut b[..55]),
        Err(Error::NoSpace)
    );
    assert_eq!(b, before);
    result.length = 147;
    assert_eq!(
        encode_operation_result(result, &mut b),
        Err(Error::Argument)
    );
    assert_eq!(b, before);
    result.length = 2;
    result.payload[145] = 1;
    assert_eq!(
        encode_operation_result(result, &mut b),
        Err(Error::Argument)
    );
    assert_eq!(b, before);
    for phase in [1, 2, 5, 6, 255] {
        assert_eq!(
            encode_operation_ack(OperationReceipt { phase, ..receipt }, &mut b),
            Err(Error::Argument)
        );
        assert_eq!(b, before);
    }
    encode_operation_ack(receipt, &mut b).unwrap();
    assert_eq!(decode_operation_ack(&b[..48]), Ok(receipt));
    b[44] = 5;
    assert_eq!(decode_operation_ack(&b[..48]), Err(Error::Malformed));
    result.payload = [0x35; 146];
    result.length = 146;
    assert_eq!(encode_operation_result(result, &mut b).unwrap(), 200);
    assert_eq!(decode_operation_result(&b[..200]).unwrap(), result);
}
#[test]
fn result_digest_binds_every_field_and_has_terminal_only_payload_contract() {
    let mut work = ucn_digest::DigestWorkspace::new();
    let original = operation_result_digest(key(), 3, -12, b"ok", &mut work).unwrap();
    for changed in [
        OperationWireKey {
            operation: key().operation + 1,
            ..key()
        },
        OperationWireKey {
            service: 0x1235,
            ..key()
        },
        OperationWireKey {
            opcode: 0x2346,
            ..key()
        },
        OperationWireKey {
            request_digest: [9; 16],
            ..key()
        },
    ] {
        assert_ne!(
            operation_result_digest(changed, 3, -12, b"ok", &mut work).unwrap(),
            original
        );
    }
    assert_ne!(
        operation_result_digest(key(), 3, -11, b"ok", &mut work).unwrap(),
        original
    );
    assert_ne!(
        operation_result_digest(key(), 3, -12, b"bad", &mut work).unwrap(),
        original
    );
    assert_eq!(
        operation_result_digest(key(), 2, 0, &[], &mut work),
        Err(Error::Argument)
    );
    assert_eq!(
        operation_result_digest(key(), 5, -15, b"fake result", &mut work),
        Err(Error::Argument)
    );
    assert_ne!(
        operation_result_digest(key(), 4, -12, &[], &mut work).unwrap(),
        operation_result_digest(key(), 5, -12, &[], &mut work).unwrap()
    );
}

#[test]
fn data_zero_business_opcode_is_not_a_zero_control_registry_opcode() {
    let data = OperationWireKey { opcode: 0, ..key() };
    let mut bytes = [0; 28];
    encode_operation_key(data, &mut bytes).unwrap();
    assert_eq!(&bytes[10..12], &[0, 0]);
    assert_eq!(decode_operation_key(&bytes).unwrap(), data);
    let before = bytes;
    assert_eq!(
        encode_operation_key(
            OperationWireKey {
                opcode: u16::MAX,
                ..data
            },
            &mut bytes
        ),
        Err(Error::Argument)
    );
    assert_eq!(bytes, before);
    assert!(ucn_types::ProtocolOpcode::try_from(0).is_err());
}
#[test]
fn conditional_flags_body_lengths_and_zero_write() {
    let mut b = [0xa5; 64];
    let request = OperationEnvelope {
        operation: 1,
        flags: 2,
        result: 0,
    };
    let n = encode_operation_envelope(InteractionRole::Request, request, &mut b).unwrap();
    assert_eq!(n, 9);
    assert_eq!(
        decode_operation_envelope(InteractionRole::Request, &b[..12]).unwrap(),
        (request, 9)
    );
    let before = b;
    assert_eq!(
        encode_operation_envelope(
            InteractionRole::Request,
            OperationEnvelope {
                flags: 6,
                ..request
            },
            &mut b
        ),
        Err(Error::Argument)
    );
    assert_eq!(b, before);
    let result = OperationEnvelope {
        flags: 6,
        result: 0x1234,
        ..request
    };
    let n = encode_operation_envelope(InteractionRole::Result, result, &mut b).unwrap();
    assert_eq!(n, 11);
    assert_eq!(
        decode_operation_envelope(InteractionRole::Result, &b[..11]).unwrap(),
        (result, 11)
    );
    let receipt = OperationReceipt {
        key: key(),
        result_digest: [0xa5; 16],
        phase: 3,
    };
    encode_operation_receipt(receipt, &mut b).unwrap();
    assert_eq!(decode_operation_receipt(&b[..48]).unwrap(), receipt);
    for offset in 45..48 {
        let mut wrong = b;
        wrong[offset] = 1;
        assert_eq!(
            decode_operation_receipt(&wrong[..48]),
            Err(Error::Malformed)
        );
    }
    let before = b;
    assert_eq!(
        encode_operation_receipt(
            OperationReceipt {
                phase: 2,
                ..receipt
            },
            &mut b
        ),
        Err(Error::Argument)
    );
    assert_eq!(before, b);
    let cancel = OperationCancel {
        key: key(),
        reason: 1,
    };
    encode_operation_cancel(cancel, &mut b).unwrap();
    assert_eq!(decode_operation_cancel(&b[..32]).unwrap(), cancel);
    b[28] = 5;
    assert_eq!(decode_operation_cancel(&b[..32]), Err(Error::Malformed));
}
