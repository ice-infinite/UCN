#![cfg(feature = "transfer")]
//! Transfer fixed-window, replay, Setup and application ownership regressions.
use ucn_digest::{DigestWorkspace, canonical_digest};
use ucn_transport::*;
use ucn_types::{DeliveryGuarantee, Error, InteractionRole, OriginSecurity};
type Owner = TransferOwner<1, 1, 64, 8, 1>;
fn parent() -> TransferParent {
    TransferParent {
        kind: ParentKind::Flow,
        id: 3,
        generation: 9,
        realm: 1,
        source: Binding {
            principal: [1; 16],
            address: 1,
            generation: 1,
        },
        destination: Binding {
            principal: [2; 16],
            address: 2,
            generation: 2,
        },
        security: SecurityDomain {
            session: 3,
            key: 4,
            policy: 5,
            origin: OriginSecurity::O1,
            fingerprint: [6; 16],
            public_trusted_o0: false,
        },
        feedback_flow: Some([7; 16]),
    }
}
fn setup() -> TransferSetup {
    TransferSetup {
        kind: ParentKind::Flow,
        parent_id: 3,
        parent_generation: 9,
        transfer_id: 1,
        service: 0x40,
        total_bytes: 6,
        digest: canonical_digest(
            b"UCN6-TRANSFER-MESSAGE-V1",
            b"abcdef",
            &mut DigestWorkspace::new(),
        )
        .unwrap(),
        delivery: DeliveryGuarantee::Reliable,
        interaction: InteractionRole::OneWay,
        fragments: 2,
        fragment_bytes: 3,
        lifetime_ms: 1,
        operation_id: 0,
        operation_flags: 0,
        result_code: 0,
    }
}
fn evidence(index: u16) -> FragmentEvidence {
    FragmentEvidence {
        sequence: u32::from(index) + 1,
        aad: [1; 16],
        payload: [u8::try_from(index).unwrap() + 2; 16],
    }
}

#[test]
fn exclusive_fragment_preflight_drop_and_last_digest_failure_publish_nothing() {
    let mut owner = Owner::new(2, 200, 0xff00).unwrap();
    let h = owner.receive_setup(parent(), setup(), 1).unwrap();
    let old = owner.view(h).unwrap();
    {
        let pending = owner
            .prepare_fragment(
                h,
                parent(),
                prefix(0),
                InteractionRole::OneWay,
                0x40,
                evidence(0),
                b"abc",
                2,
                &mut DigestWorkspace::new(),
            )
            .unwrap();
        assert_eq!(pending.handle(), h);
        assert_eq!(pending.sack().bitmap, 1);
        // Scope drop models failed Replay/resource preflight: do not commit.
    }
    assert_eq!(owner.view(h).unwrap(), old);
    owner
        .receive_fragment(
            h,
            parent(),
            prefix(0),
            InteractionRole::OneWay,
            0x40,
            evidence(0),
            b"abc",
            2,
            &mut DigestWorkspace::new(),
        )
        .unwrap();
    let old = owner.view(h).unwrap();
    assert!(matches!(
        owner.prepare_fragment(
            h,
            parent(),
            prefix(1),
            InteractionRole::OneWay,
            0x40,
            evidence(1),
            b"bad",
            3,
            &mut DigestWorkspace::new()
        ),
        Err(Error::Malformed)
    ));
    assert_eq!(owner.view(h).unwrap(), old);
    let pending = owner
        .prepare_fragment(
            h,
            parent(),
            prefix(1),
            InteractionRole::OneWay,
            0x40,
            evidence(1),
            b"def",
            3,
            &mut DigestWorkspace::new(),
        )
        .unwrap();
    assert_eq!(pending.sack().bitmap, 3);
    assert!(pending.commit());
    assert_eq!(owner.view(h).unwrap().phase, TransferPhase::Complete);
    let mut output = [0xa5; 6];
    assert_eq!(owner.copy_complete(h, &mut output), Ok(6));
    assert_eq!(&output, b"abcdef");
}
fn prefix(index: u16) -> FragmentPrefix {
    FragmentPrefix {
        kind: ParentKind::Flow,
        transfer_id: 1,
        parent_generation: 0,
        index,
        count: 2,
    }
}
#[test]
fn reordered_fragments_conflicts_complete_claim_and_timeout() {
    let mut rx = Owner::new(2, 200, 0xff00).unwrap();
    let h = rx.receive_setup(parent(), setup(), 1).unwrap();
    let mut scratch = DigestWorkspace::new();
    assert!(
        rx.receive_fragment(
            h,
            parent(),
            prefix(1),
            InteractionRole::OneWay,
            0x40,
            evidence(1),
            b"def",
            2,
            &mut scratch
        )
        .unwrap()
    );
    let before = rx.view(h).unwrap();
    assert_eq!(
        rx.receive_fragment(
            h,
            parent(),
            prefix(1),
            InteractionRole::OneWay,
            0x40,
            evidence(1),
            b"bad",
            2,
            &mut scratch
        ),
        Err(Error::Replay)
    );
    assert_eq!(rx.view(h).unwrap(), before);
    assert_eq!(
        rx.receive_fragment(
            h,
            parent(),
            prefix(0),
            InteractionRole::OneWay,
            0x40,
            evidence(0),
            b"bad",
            2,
            &mut scratch
        ),
        Err(Error::Malformed)
    );
    assert_eq!(rx.view(h).unwrap(), before);
    rx.receive_fragment(
        h,
        parent(),
        prefix(0),
        InteractionRole::OneWay,
        0x40,
        evidence(0),
        b"abc",
        2,
        &mut scratch,
    )
    .unwrap();
    let claim = rx.claim(h).unwrap();
    rx.step(10_000, 2).unwrap();
    assert_eq!(rx.claimed_bytes(&claim).unwrap(), b"abcdef");
    assert_eq!(rx.abort(h), Err(Error::State));
    assert_eq!(rx.retire_rx(h, 10_000), Err(Error::State));
    let new = TransferSetup {
        transfer_id: 2,
        ..setup()
    };
    assert_eq!(rx.receive_setup(parent(), new, 10_000), Err(Error::NoSpace));
    rx.release(claim).unwrap();
    rx.retire_rx(h, 10_000).unwrap();
    assert!(rx.receive_setup(parent(), new, 10_000).is_ok());
}
#[test]
fn setup_first_credit_feedback_and_remote_receipt() {
    let mut tx = Owner::new(1, 200, 0xff00).unwrap();
    let mut allocator = FlowTransferAllocator::new(parent()).unwrap();
    let permit = allocator.allocate().unwrap();
    let mut scratch = DigestWorkspace::new();
    let h = tx
        .begin(permit, setup(), b"abcdef", 1, &mut scratch)
        .unwrap();
    let mut bytes = [0; 20];
    assert_eq!(tx.fragment(h, 0, 2, &mut bytes), Err(Error::State));
    tx.setup_ack(h, parent(), setup(), 1, 2).unwrap();
    tx.fragment(h, 0, 3, &mut bytes).unwrap();
    let early = Sack {
        kind: ParentKind::Flow,
        transfer_id: 1,
        parent_generation: 0,
        original_service: 0,
        base: 0,
        bitmap: 1,
        credit: 1,
    };
    assert_eq!(
        tx.on_sack(h, parent(), early, 0xff00, Some([7; 16]), 3),
        Err(Error::Malformed)
    );
    tx.fragment_disposition(h, 0, Submission::NotSubmitted)
        .unwrap();
    // A retry after proven backpressure consumes the refunded credit, not new ID.
    let n = tx.fragment(h, 0, 3, &mut bytes).unwrap();
    assert_eq!(&bytes[n - 3..n], b"abc");
    tx.fragment_disposition(h, 0, Submission::Submitted)
        .unwrap();
    assert_eq!(tx.fragment(h, 1, 3, &mut bytes), Err(Error::NoSpace));
    tx.setup_ack(h, parent(), setup(), 1, 3).unwrap();
    // Replayed Setup ACK must not refill a spent receiver-credit grant.
    assert_eq!(tx.fragment(h, 1, 3, &mut bytes), Err(Error::NoSpace));
    assert_eq!(tx.setup_ack(h, parent(), setup(), 2, 3), Err(Error::Replay));
    let mut sack = Sack {
        kind: ParentKind::Flow,
        transfer_id: 1,
        parent_generation: 0,
        original_service: 0,
        base: 0,
        bitmap: 1,
        credit: 1,
    };
    assert_eq!(
        tx.on_sack(h, parent(), sack, 0xff00, Some([8; 16]), 3),
        Err(Error::Access)
    );
    sack.bitmap = 2;
    assert_eq!(
        tx.on_sack(h, parent(), sack, 0xff00, Some([7; 16]), 3),
        Err(Error::Malformed)
    );
    sack.bitmap = 1;
    tx.on_sack(h, parent(), sack, 0xff00, Some([7; 16]), 3)
        .unwrap();
    tx.fragment(h, 1, 4, &mut bytes).unwrap();
    tx.fragment_disposition(h, 1, Submission::Submitted)
        .unwrap();
    sack.bitmap = 3;
    tx.on_sack(h, parent(), sack, 0xff00, Some([7; 16]), 4)
        .unwrap();
    assert_eq!(tx.view(h).unwrap().phase, TransferPhase::Sending);
    tx.terminal(h, parent(), setup(), true, 5).unwrap();
    let done = tx.view(h).unwrap();
    assert_eq!(
        tx.terminal(h, parent(), setup(), false, 6),
        Err(Error::Replay)
    );
    assert_eq!(tx.view(h).unwrap(), done);
    tx.terminal(h, parent(), setup(), true, 10000).unwrap();
    tx.retire_tx(h).unwrap();
    assert_eq!(tx.view(h), Err(Error::State));
}
#[test]
fn exact_setup_duplicate_never_renews_and_wrong_domains_do_not_mutate() {
    let mut rx = Owner::new(2, 200, 0xff00).unwrap();
    let h = rx.receive_setup(parent(), setup(), 1).unwrap();
    let before = rx.view(h).unwrap();
    assert_eq!(rx.receive_setup(parent(), setup(), 900).unwrap(), h);
    assert_eq!(rx.view(h).unwrap(), before);
    assert_eq!(
        rx.receive_setup(parent(), setup(), 1001),
        Err(Error::Timeout)
    );
    let mut wrong = parent();
    wrong.generation += 1;
    let mut scratch = DigestWorkspace::new();
    assert_eq!(
        rx.receive_fragment(
            h,
            wrong,
            prefix(0),
            InteractionRole::OneWay,
            0x40,
            evidence(0),
            b"abc",
            2,
            &mut scratch
        ),
        Err(Error::Access)
    );
    assert_eq!(rx.view(h).unwrap(), before);
}
#[test]
fn frozen_layouts_lengths_reserved_kinds_and_zero_write() {
    let mut out = [0xa5; 80];
    assert_eq!(
        encode_ack(
            DeliveryAck {
                service: 0x1234,
                sequence: 0x0102_0304,
                status: 0,
                credit: 1
            },
            &mut out
        )
        .unwrap(),
        9
    );
    assert_eq!(&out[..9], &[1, 0x12, 0x34, 1, 2, 3, 4, 0, 1]);
    let value = setup();
    let n = encode_setup(value, &mut out).unwrap();
    assert_eq!(n, 43);
    assert_eq!(
        &out[..17],
        &[2, 0, 3, 0, 0, 0, 9, 0, 0, 0, 1, 0, 0x40, 0, 0, 0, 6]
    );
    assert_eq!(decode_setup(&out[..n]).unwrap(), value);
    assert_eq!(decode_setup(&out[..=n]), Err(Error::Malformed));
    let mut invalid = value;
    invalid.operation_id = 1;
    let sentinel = out;
    assert_eq!(encode_setup(invalid, &mut out), Err(Error::Argument));
    assert_eq!(out, sentinel);
    let n = encode_fragment(prefix(1), &mut out).unwrap();
    assert_eq!(&out[..n], &[0, 0, 0, 1, 0, 1, 0, 2]);
    out[4] = 0x80;
    assert_eq!(
        decode_fragment(ParentKind::Flow, &out[..n]),
        Err(Error::Malformed)
    );
    for role in [
        InteractionRole::Request,
        InteractionRole::Result,
        InteractionRole::Error,
    ] {
        let value = TransferSetup {
            interaction: role,
            operation_id: 1,
            operation_flags: if role == InteractionRole::Request {
                1
            } else {
                5
            },
            ..value
        };
        let n = encode_setup(value, &mut out).unwrap();
        assert_eq!(
            n,
            if role == InteractionRole::Request {
                52
            } else {
                54
            }
        );
        assert_eq!(decode_setup(&out[..n]).unwrap(), value);
    }
    // Every reserved/conflicting mode byte fails before any output write.
    for flags in 0..=u8::MAX {
        let request = TransferSetup {
            interaction: InteractionRole::Request,
            operation_id: 1,
            operation_flags: flags,
            ..value
        };
        let before = out;
        if flags <= 2 {
            let n = encode_setup(request, &mut out).unwrap();
            assert_eq!(decode_setup(&out[..n]).unwrap(), request);
        } else {
            assert_eq!(encode_setup(request, &mut out), Err(Error::Argument));
            assert_eq!(out, before);
        }
    }
}
