#![cfg(feature = "transfer")]
//! Durable C1 Parent and ID allocation through the real Persistence state machine.
#[path = "../../../tests/support/flash.rs"]
mod flash;
use ucn_durable::DurableContract;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_transport::*;
use ucn_types::{Error, OriginSecurity};
fn contract() -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::TransportHighWater,
            id: 4,
        },
        schema: 0x5401,
        version: 1,
        operation: 0x0701,
    }
}
fn parent() -> TransferParent {
    TransferParent {
        kind: ParentKind::C1,
        id: 0,
        generation: 1,
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
        feedback_flow: None,
    }
}
#[test]
fn persist_before_parent_and_id_publish_and_reload_never_resets_high_water() {
    flash::exercise(contract(), |persistence, provider| {
        let mut owner = ParentOwner::new(contract()).unwrap();
        let mut scratch = CodecWorkspace::new();
        owner
            .prepare_parent(
                flash::base(persistence, contract()),
                parent(),
                1,
                10_000,
                1,
                &mut scratch,
            )
            .unwrap();
        assert_eq!(owner.view(), (None, 0));
        assert!(matches!(owner.take_permit(), Err(Error::State)));
        provider.pending_once = true;
        let (h, proof) = flash::commit(persistence, provider, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        persistence.proof_retire(h).unwrap();
        assert_eq!(owner.view().0, Some(parent()));
        owner
            .allocate(
                flash::base(persistence, contract()),
                21,
                10_000,
                2,
                &mut scratch,
            )
            .unwrap();
        assert!(matches!(owner.take_permit(), Err(Error::State)));
        let (h, proof) = flash::commit(persistence, provider, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        persistence.proof_retire(h).unwrap();
        assert_eq!(owner.take_permit().unwrap().id(), 1);
        assert!(owner.take_permit().is_err());
        let mut bytes = [0; 256];
        let n = persistence
            .copy_body(contract().domain, &mut bytes)
            .unwrap();
        let mut restarted = ParentOwner::new(contract()).unwrap();
        assert_eq!(
            restarted.prepare_parent(
                flash::base(persistence, contract()),
                parent(),
                21,
                10000,
                3,
                &mut scratch
            ),
            Err(Error::State)
        );
        let mut substituted = bytes;
        substituted[79] = 2; // Valid next high-water syntax, but not the recovered Flash body.
        assert_eq!(
            restarted.restore(
                flash::base(persistence, contract()),
                &substituted[..n],
                parent().security,
                &mut scratch
            ),
            Err(Error::Malformed)
        );
        assert_eq!(restarted.view(), (None, 0));
        restarted
            .restore(
                flash::base(persistence, contract()),
                &bytes[..n],
                parent().security,
                &mut scratch,
            )
            .unwrap();
        assert_eq!(restarted.view().1, 1);
        restarted
            .allocate(
                flash::base(persistence, contract()),
                21,
                10_000,
                3,
                &mut scratch,
            )
            .unwrap();
        let (h, proof) = flash::commit(persistence, provider, &restarted.requirement().unwrap());
        restarted.bind(h).unwrap();
        restarted.confirm(h, proof).unwrap();
        assert_eq!(restarted.take_permit().unwrap().id(), 2);
    });
}
#[test]
fn parent_record_matches_frozen_offsets_and_wrong_security_rejects() {
    let record = encode_parent_record(parent(), 9, 10).unwrap();
    assert_eq!(&record[..8], &[0x55, 0x43, 0x50, 0x54, 0, 1, 1, 0]);
    assert_eq!(
        &record[72..88],
        &[0, 0, 0, 1, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 10]
    );
    assert_eq!(
        decode_parent_record(&record, parent().security).unwrap(),
        (parent(), 9, 10)
    );
    let mut wrong = parent().security;
    wrong.key += 1;
    assert_eq!(decode_parent_record(&record, wrong), Err(Error::State));
}
