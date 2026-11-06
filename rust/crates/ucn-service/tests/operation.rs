#![cfg(feature = "operation")]
//! Operation crash recovery uses actual Foundation reload proof, not fake boolean durability.
#[path = "../../../tests/support/flash.rs"]
mod flash;
use ucn_durable::DurableContract;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_service::*;
use ucn_types::{Error, OriginSecurity};
fn contract() -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::OperationJournal,
            id: 5,
        },
        schema: 1,
        version: 1,
        operation: 0x0701,
    }
}
fn key() -> OperationKey {
    OperationKey {
        service: ServiceKey {
            realm: 1,
            client: [1; 16],
            server: [2; 16],
            client_generation: 1,
            server_generation: 1,
            client_address: 1,
            server_address: 2,
            operation: 1,
            service: 0x40,
            opcode: 0,
            session: 1,
            key_generation: 1,
            origin: OriginSecurity::O1,
            policy: 1,
            security: [3; 16],
        },
        request_digest: [1; 16],
    }
}
#[test]
fn operation_receipt_is_unavailable_before_exact_terminal_reload() {
    flash::exercise(contract(), |p, f| {
        let mut k = key();
        k.service.opcode = 9;
        let mut owner = OperationOwner::<32>::new(contract()).unwrap();
        owner
            .begin(
                k,
                flash::base(p, contract()),
                1,
                1000,
                1,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
        assert!(
            owner
                .receipt(&mut ucn_digest::DigestWorkspace::new())
                .is_err()
        );
        let (h, proof) = flash::commit(p, f, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        p.proof_retire(h).unwrap();
        assert!(
            owner
                .receipt(&mut ucn_digest::DigestWorkspace::new())
                .is_err()
        );
        owner
            .transition(
                OperationPhase::AbortedNoEffect,
                flash::base(p, contract()),
                Error::Cancelled.code(),
                &[],
                21,
                1000,
                2,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
        assert!(
            owner
                .receipt(&mut ucn_digest::DigestWorkspace::new())
                .is_err()
        );
        let h = p.submit(&owner.requirement().unwrap(), 21).unwrap();
        owner.bind(h).unwrap();
        for _ in 0..128 {
            p.step(f, 21, 1).unwrap();
            if let Ok(proof) = p.proof_get(h) {
                owner.confirm(h, proof).unwrap();
                break;
            }
        }
        let receipt = owner
            .receipt(&mut ucn_digest::DigestWorkspace::new())
            .unwrap();
        assert_eq!(receipt.phase, 4);
        assert_eq!(receipt.key.opcode, 9);
        assert_eq!(owner.key(), Some(k));
    });
}

#[test]
fn reconciled_ram_is_not_published_before_the_new_terminal_reload() {
    struct Query;
    impl ExecutorQuery for Query {
        fn query(&mut self, _: OperationKey, out: &mut [u8]) -> ucn_types::Result<(i32, usize)> {
            out[..9].copy_from_slice(b"recovered");
            Ok((7, 9))
        }
    }
    flash::exercise(contract(), |p, f| {
        let mut k = key();
        k.service.opcode = 9;
        let mut op = OperationOwner::<32>::new(contract()).unwrap();
        op.begin(
            k,
            flash::base(p, contract()),
            1,
            1000,
            1,
            &mut CodecWorkspace::new(),
        )
        .unwrap();
        let (h, proof) = flash::commit(p, f, &op.requirement().unwrap());
        op.bind(h).unwrap();
        op.confirm(h, proof).unwrap();
        p.proof_retire(h).unwrap();
        for phase in [OperationPhase::Executing, OperationPhase::InDoubt] {
            op.transition(
                phase,
                flash::base(p, contract()),
                Error::InDoubt.code(),
                &[],
                21,
                1000,
                2,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
            let (h, proof) = flash::commit(p, f, &op.requirement().unwrap());
            op.bind(h).unwrap();
            op.confirm(h, proof).unwrap();
            p.proof_retire(h).unwrap();
        }
        let gate = ucn_owner::CallbackGate::new(7).unwrap();
        op.reconcile(k, &mut Query, &gate).unwrap();
        let mut out = [0xa5; 32];
        assert_eq!(op.copy_result(&mut out), Err(Error::State));
        assert_eq!(out, [0xa5; 32]);
        assert!(op.receipt(&mut ucn_digest::DigestWorkspace::new()).is_err());
        op.transition(
            OperationPhase::CommittedResult,
            flash::base(p, contract()),
            7,
            b"recovered",
            21,
            1000,
            3,
            &mut CodecWorkspace::new(),
        )
        .unwrap();
        let (h, proof) = flash::commit(p, f, &op.requirement().unwrap());
        op.bind(h).unwrap();
        op.confirm(h, proof).unwrap();
        p.proof_retire(h).unwrap();
        assert_eq!(
            op.copy_result(&mut out).unwrap(),
            (OperationPhase::CommittedResult, 7, 9)
        );
        assert_eq!(&out[..9], b"recovered");
        assert!(op.receipt(&mut ucn_digest::DigestWorkspace::new()).is_ok());
    });
}
#[test]
fn executor_is_handed_off_only_after_two_exact_reload_proofs_and_result_waits() {
    flash::exercise(contract(), |persistence, provider| {
        let mut owner = OperationOwner::<32>::new(contract()).unwrap();
        let mut scratch = CodecWorkspace::new();
        owner
            .begin(
                key(),
                flash::base(persistence, contract()),
                1,
                10_000,
                1,
                &mut scratch,
            )
            .unwrap();
        assert!(owner.handoff(key()).is_err());
        let (h, proof) = flash::commit(persistence, provider, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        persistence.proof_retire(h).unwrap();
        assert!(owner.handoff(key()).is_err());
        owner
            .transition(
                OperationPhase::Executing,
                flash::base(persistence, contract()),
                0,
                &[],
                21,
                10_000,
                2,
                &mut scratch,
            )
            .unwrap();
        assert!(owner.handoff(key()).is_err());
        let (h, proof) = flash::commit(persistence, provider, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        persistence.proof_retire(h).unwrap();
        let permit = owner.handoff(key()).unwrap();
        assert!(owner.handoff(key()).is_err());
        assert!(
            owner
                .transition(
                    OperationPhase::CommittedResult,
                    flash::base(persistence, contract()),
                    0,
                    &[],
                    21,
                    10000,
                    3,
                    &mut scratch
                )
                .is_err()
        );
        owner.executor_result(permit, 5, b"ok").unwrap();
        owner
            .transition(
                OperationPhase::CommittedResult,
                flash::base(persistence, contract()),
                5,
                b"ok",
                21,
                10_000,
                3,
                &mut scratch,
            )
            .unwrap();
        assert!(owner.copy_result(&mut [0; 32]).is_err());
        provider.pending_once = true;
        let (h, proof) = flash::commit(persistence, provider, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        assert_eq!(
            owner.copy_result(&mut [0; 32]).unwrap(),
            (OperationPhase::CommittedResult, 5, 2)
        );
    });
}
#[test]
fn restored_executing_never_reissues_executor_and_canonical_record_is_exact() {
    flash::exercise(contract(), |persistence, provider| {
        let mut owner = OperationOwner::<32>::new(contract()).unwrap();
        let mut scratch = CodecWorkspace::new();
        owner
            .begin(
                key(),
                flash::base(persistence, contract()),
                1,
                10_000,
                1,
                &mut scratch,
            )
            .unwrap();
        let (h, proof) = flash::commit(persistence, provider, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        persistence.proof_retire(h).unwrap();
        owner
            .transition(
                OperationPhase::Executing,
                flash::base(persistence, contract()),
                0,
                &[],
                21,
                10_000,
                2,
                &mut scratch,
            )
            .unwrap();
        let (h, proof) = flash::commit(persistence, provider, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        persistence.proof_retire(h).unwrap();
        let mut body = [0; 256];
        let n = persistence.copy_body(contract().domain, &mut body).unwrap();
        assert_eq!(n, 110);
        assert_eq!(&body[..8], &[b'O', b'P', b'S', b'1', 0, 1, 2, 0]);
        let mut rebooted = OperationOwner::<32>::new(contract()).unwrap();
        rebooted
            .restore(
                flash::base(persistence, contract()),
                &body[..n],
                key().service.security,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
        assert_eq!(rebooted.handoff(key()), Err(Error::State));
        assert!(
            rebooted
                .transition(
                    OperationPhase::AbortedNoEffect,
                    flash::base(persistence, contract()),
                    0,
                    &[],
                    21,
                    10_000,
                    3,
                    &mut scratch
                )
                .is_err()
        );
        rebooted
            .transition(
                OperationPhase::InDoubt,
                flash::base(persistence, contract()),
                0,
                &[],
                21,
                10_000,
                3,
                &mut scratch,
            )
            .unwrap();
        let (h, proof) = flash::commit(persistence, provider, &rebooted.requirement().unwrap());
        rebooted.bind(h).unwrap();
        rebooted.confirm(h, proof).unwrap();
        assert_eq!(
            rebooted.copy_result(&mut [0; 32]).unwrap().0,
            OperationPhase::InDoubt
        );
    });
}
