#![cfg(feature = "operation")]
//! Durable interval allocation and reboot burn semantics.
#[path = "../../../tests/support/flash.rs"]
mod flash;
use ucn_durable::DurableContract;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_service::OperationIds;
fn contract() -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::OperationJournal,
            id: 6,
        },
        schema: 2,
        version: 1,
        operation: 0x0702,
    }
}
#[test]
fn reserved_operation_ids_publish_after_reload_and_reboot_burns_whole_interval() {
    flash::exercise(contract(), |p, f| {
        let mut ids = OperationIds::new(contract(), 1).unwrap();
        assert!(ids.allocate().is_err());
        ids.reserve(
            flash::base(p, contract()),
            3,
            1,
            10000,
            1,
            &mut CodecWorkspace::new(),
        )
        .unwrap();
        assert!(ids.allocate().is_err());
        let (h, proof) = flash::commit(p, f, &ids.requirement().unwrap());
        ids.bind(h).unwrap();
        ids.confirm(h, proof).unwrap();
        p.proof_retire(h).unwrap();
        assert_eq!(ids.allocate().unwrap(), 1);
        let mut body = [0; 20];
        p.copy_body(contract().domain, &mut body).unwrap();
        let mut restored = OperationIds::new(contract(), 1).unwrap();
        assert!(
            restored
                .reserve(
                    flash::base(p, contract()),
                    3,
                    21,
                    10000,
                    2,
                    &mut CodecWorkspace::new()
                )
                .is_err()
        );
        assert!(restored.requirement().is_err());
        let mut changed = body;
        changed[19] = 1; // Still a syntactically valid OID1 record, but wrong high-water.
        assert!(
            restored
                .restore(
                    flash::base(p, contract()),
                    &changed,
                    &mut CodecWorkspace::new()
                )
                .is_err()
        );
        assert!(restored.allocate().is_err());
        restored
            .restore(
                flash::base(p, contract()),
                &body,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
        assert!(restored.allocate().is_err());
        restored
            .reserve(
                flash::base(p, contract()),
                2,
                21,
                10000,
                2,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
        let (h, proof) = flash::commit(p, f, &restored.requirement().unwrap());
        restored.bind(h).unwrap();
        restored.confirm(h, proof).unwrap();
        p.proof_retire(h).unwrap();
        assert_eq!(restored.allocate().unwrap(), 4);
        assert_eq!(restored.allocate().unwrap(), 5);
        assert!(restored.allocate().is_err());
    });
}
