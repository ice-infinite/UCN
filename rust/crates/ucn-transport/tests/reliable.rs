#![cfg(feature = "reliable")]
//! Reliable lifecycle, forgery, retention and capacity regressions.
use ucn_transport::*;
use ucn_types::{Error, OriginSecurity};
fn key(sequence: u32) -> ReliableKey {
    ReliableKey {
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
        service: 0x40,
        sequence,
        security: SecurityDomain {
            session: 3,
            key: 4,
            policy: 5,
            origin: OriginSecurity::O1,
            fingerprint: [6; 16],
            public_trusted_o0: false,
        },
    }
}
fn owner() -> ReliableOwner<2, 2, 32> {
    ReliableOwner::new(ReliableConfig {
        owner: 1,
        retry_us: 10,
        max_attempts: 3,
        receipt_retention_us: 200,
    })
    .unwrap()
}
#[test]
fn immutable_retry_backpressure_ack_and_driver_holder_are_independent() {
    let mut owner = owner();
    let h = owner.begin(key(7), b"origin-sealed", 1, 100).unwrap();
    let attempt = owner.attempt_begin(h, 1).unwrap();
    owner
        .submitted(attempt, Submission::NotSubmitted, 1)
        .unwrap();
    assert_eq!(owner.view(h).unwrap().attempts, 0);
    assert_eq!(owner.attempt_begin(h, 5), Err(Error::State));
    let attempt = owner.attempt_begin(h, 11).unwrap();
    owner.submitted(attempt, Submission::Submitted, 11).unwrap();
    let mut frame = [0; 32];
    let length = owner.copy_frame(h, &mut frame).unwrap();
    assert_eq!(&frame[..length], b"origin-sealed");
    let mut reverse = key(8);
    core::mem::swap(&mut reverse.source, &mut reverse.destination);
    let ack = DeliveryAck {
        service: 0x40,
        sequence: 7,
        status: 0,
        credit: 1,
    };
    let mut forged = reverse;
    forged.source.generation += 1;
    assert_eq!(owner.on_ack(h, forged, ack, 12), Err(Error::Access));
    assert_eq!(owner.view(h).unwrap().phase, ReliablePhase::WaitAck);
    owner.on_ack(h, reverse, ack, 12).unwrap();
    assert_eq!(
        owner.on_ack(h, reverse, DeliveryAck { status: 1, ..ack }, 12),
        Err(Error::Replay)
    );
    assert_eq!(owner.retire(h), Err(Error::State));
    owner.attempt_complete(attempt).unwrap();
    assert_eq!(owner.attempt_complete(attempt), Err(Error::State));
    owner.retire(h).unwrap();
    assert_eq!(owner.view(h), Err(Error::State));
}
#[test]
fn retained_receipt_exact_duplicate_conflict_expiry_and_capacity() {
    let mut owner = owner();
    let ReceiveAction::Reserved(h) = owner
        .receive_prepare(key(1), [1; 16], [2; 16], false, 1)
        .unwrap()
    else {
        panic!()
    };
    assert_eq!(
        owner.receive_prepare(key(1), [1; 16], [2; 16], true, 2),
        Err(Error::State)
    );
    owner.receive_commit(h).unwrap();
    assert!(matches!(
        owner.receive_prepare(key(1), [1; 16], [2; 16], true, 2),
        Ok(ReceiveAction::Duplicate(_))
    ));
    assert_eq!(
        owner.receive_prepare(key(1), [1; 16], [3; 16], true, 2),
        Err(Error::Replay)
    );
    let ReceiveAction::Reserved(h2) = owner
        .receive_prepare(key(2), [1; 16], [2; 16], false, 1)
        .unwrap()
    else {
        panic!()
    };
    owner.receive_commit(h2).unwrap();
    assert_eq!(
        owner.receive_prepare(key(3), [1; 16], [2; 16], false, 300),
        Err(Error::NoSpace)
    );
    owner.step(300, 4).unwrap();
    assert_eq!(
        owner.receive_prepare(key(1), [1; 16], [2; 16], true, 300),
        Err(Error::Replay)
    );
    assert!(
        owner
            .receive_prepare(key(3), [1; 16], [2; 16], false, 300)
            .is_ok()
    );
}
#[test]
fn bounded_retry_deadline_cancel_and_slot_aba() {
    let mut owner = owner();
    let h = owner.begin(key(1), b"one", 1, 100).unwrap();
    for now in [1, 11, 21] {
        let a = owner.attempt_begin(h, now).unwrap();
        owner.submitted(a, Submission::Submitted, now).unwrap();
        owner.attempt_complete(a).unwrap();
    }
    owner.step(31, 4).unwrap();
    assert_eq!(owner.view(h).unwrap().phase, ReliablePhase::Failed);
    owner.retire(h).unwrap();
    let new = owner.begin(key(2), b"two", 32, 100).unwrap();
    assert_ne!(new, h);
    assert_eq!(owner.cancel(h), Err(Error::State));
    owner.cancel(new).unwrap();
    owner.retire(new).unwrap();
    let mut public = key(3);
    public.security.origin = OriginSecurity::O0;
    assert_eq!(owner.begin(public, b"public", 32, 100), Err(Error::Access));
}
