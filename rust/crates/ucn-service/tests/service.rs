//! RPC and fair queue semantics, independent of Reliable/Persistence.
use ucn_service::*;
use ucn_types::{Error, OriginSecurity, TrafficClass};
fn key(operation: u64) -> ServiceKey {
    ServiceKey {
        realm: 1,
        client: [1; 16],
        server: [2; 16],
        client_generation: 1,
        server_generation: 1,
        client_address: 1,
        server_address: 2,
        operation,
        service: 0x40,
        opcode: 0,
        session: 1,
        key_generation: 1,
        origin: OriginSecurity::O1,
        policy: 1,
        security: [3; 16],
    }
}
#[test]
#[cfg(feature = "rpc")]
fn exact_result_identity_and_retained_server_receipt() {
    let mut client = ServiceOwner::<2, 2, 32>::new(1, 100).unwrap();
    let mut server = ServiceOwner::<2, 2, 32>::new(2, 100).unwrap();
    let h = client.request(key(1), [1; 16], 1, 100).unwrap();
    client.sent(h, 2).unwrap();
    let RequestAction::Invoke(receipt) = server.on_request(key(1), [1; 16], 2).unwrap() else {
        panic!()
    };
    assert_eq!(
        server.on_request(key(1), [1; 16], 3).unwrap(),
        RequestAction::Pending(receipt)
    );
    server.respond(receipt, 7, b"result", 3).unwrap();
    assert_eq!(
        server.on_request(key(1), [1; 16], 4).unwrap(),
        RequestAction::Replay(receipt)
    );
    assert_eq!(server.on_request(key(1), [2; 16], 4), Err(Error::Replay));
    let mut wrong = key(1);
    wrong.server_generation = 2;
    assert_eq!(
        client.on_result(h, wrong, 7, b"result", 4),
        Err(Error::Access)
    );
    client.on_result(h, key(1), 7, b"result", 4).unwrap();
    client.on_result(h, key(1), 7, b"result", 5).unwrap();
    assert_eq!(
        client.on_result(h, key(1), 7, b"wrong", 5),
        Err(Error::Replay)
    );
    let mut out = [0; 32];
    assert_eq!(client.copy_result(h, &mut out).unwrap(), 6);
    assert_eq!(&out[..6], b"result");
    client.retire(h).unwrap();
    assert_eq!(client.view(h), Err(Error::State));
}

#[test]
#[cfg(feature = "rpc")]
fn admission_guards_drop_without_publishing_and_exact_commit_is_one_way() {
    let mut server = ServiceOwner::<1, 1, 8>::new(2, 100).unwrap();
    let first = server.prepare_request(key(1), [1; 16], 2).unwrap();
    assert!(matches!(first.action(), RequestAction::Invoke(_)));
    drop(first);
    // Dropping preflight has not consumed the single slot or generation.
    let pending = server.prepare_request(key(2), [2; 16], 2).unwrap();
    let RequestAction::Invoke(receipt) = pending.commit() else {
        panic!()
    };
    assert_eq!(server.on_request(key(1), [1; 16], 2), Err(Error::NoSpace));
    assert_eq!(server.retire_server(receipt, 101), Err(Error::State));
    server.retire_server(receipt, 102).unwrap();
    server.retire_server(receipt, 102).unwrap();
    let RequestAction::Invoke(next) = server.on_request(key(3), [3; 16], 103).unwrap() else {
        panic!()
    };
    server.retire_server(receipt, 200).unwrap();
    assert_eq!(server.view(next).unwrap().key, key(3));
    let mut wrong_owner = ServiceOwner::<1, 1, 8>::new(9, 100).unwrap();
    assert_eq!(wrong_owner.retire_server(receipt, 200), Err(Error::State));

    let mut client = ServiceOwner::<1, 1, 8>::new(1, 100).unwrap();
    let h = client.request(key(1), [1; 16], 1, 100).unwrap();
    client.sent(h, 2).unwrap();
    let pending = client.prepare_result(h, key(1), 7, b"result", 3).unwrap();
    assert!(pending.fresh());
    drop(pending);
    assert_eq!(client.view(h).unwrap().phase, RequestPhase::WaitResult);
    client
        .prepare_result(h, key(1), 7, b"result", 3)
        .unwrap()
        .commit();
    let duplicate = client.prepare_result(h, key(1), 7, b"result", 4).unwrap();
    assert!(!duplicate.fresh());
    duplicate.commit();
    assert_eq!(client.view(h).unwrap().phase, RequestPhase::Complete);
}
#[test]
#[cfg(feature = "qos")]
fn two_q1_hot_latest_streams_have_bounded_wait_and_no_payload_queue() {
    let mut qos = QosOwner::<8>::new([2, 2, 2, 2], 2, 2).unwrap();
    let item = |slot, stream| QueueItem {
        sendable: SendableRef {
            runtime: 1,
            slot,
            generation: 1,
        },
        source: 1,
        flow: stream,
        class: TrafficClass::Q1,
        deadline: 0,
        latest: Some([stream.to_le_bytes()[0]; 16]),
        control_authorized: false,
    };
    qos.enqueue(item(1, 1), 1).unwrap();
    qos.enqueue(item(2, 2), 1).unwrap();
    let mut seen = [0; 2];
    for cycle in 0..1000 {
        let (h, picked) = qos.pick(2).unwrap().unwrap();
        seen[(picked.flow - 1) as usize] += 1;
        qos.submitted(h).unwrap();
        qos.remove(h).unwrap();
        let mut next = item(picked.sendable.slot, picked.flow);
        next.sendable.generation = cycle + 2;
        qos.enqueue(next, 2).unwrap();
    }
    assert_eq!(seen, [500, 500]);
    let mut urgent = item(3, 3);
    urgent.class = TrafficClass::Q0;
    assert!(qos.enqueue(urgent, 2).is_err());
}
#[test]
#[cfg(feature = "qos")]
fn latest_replaces_only_unsent_exact_domain_and_invalidates_old_handle() {
    let mut qos = QosOwner::<4>::new([1, 1, 1, 1], 1, 1).unwrap();
    let item = QueueItem {
        sendable: SendableRef {
            runtime: 1,
            slot: 1,
            generation: 1,
        },
        source: 1,
        flow: 1,
        class: TrafficClass::Q1,
        deadline: 0,
        latest: Some([1; 16]),
        control_authorized: false,
    };
    let (h, old) = qos.enqueue(item, 1).unwrap();
    assert_eq!(old, None);
    let next = QueueItem {
        sendable: SendableRef {
            generation: 2,
            ..item.sendable
        },
        ..item
    };
    let (new, old) = qos.enqueue(next, 1).unwrap();
    assert_eq!(old, Some(item.sendable));
    assert_eq!(qos.remove(h), Err(Error::State));
    let (selected, _) = qos.pick(2).unwrap().unwrap();
    assert_eq!(selected, new);
    assert!(
        qos.enqueue(
            QueueItem {
                sendable: SendableRef {
                    generation: 3,
                    ..next.sendable
                },
                ..next
            },
            2
        )
        .is_err()
    );
    qos.release_pick(new).unwrap();
}
