//! Real Foundation proof and adversarial membership/coverage regression tests.
#[path = "../../../tests/support/flash.rs"]
mod flash;
use ucn_cluster::*;
use ucn_digest::DigestWorkspace;
use ucn_durable::{DurableContract, DurableUpdate};
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_types::Error;
fn member(id: u8) -> ClusterMember {
    ClusterMember {
        principal: [id; 16],
        binding: 1,
        voter: true,
        backup: id == 2,
    }
}
fn config() -> ClusterConfig<3> {
    ClusterConfig {
        id: 1,
        generation: 1,
        count: 3,
        members: [member(1), member(2), member(3)],
    }
}
fn epoch() -> ClusterEpoch {
    ClusterEpoch {
        cluster: 1,
        term: 1,
        head: [1; 16],
        binding: 1,
    }
}
fn contract() -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::Cluster,
            id: 7,
        },
        schema: 0x0803,
        version: 2,
        operation: 0x0830,
    }
}
fn fact(id: u8, until: u64) -> MemberFact {
    MemberFact {
        member: member(id),
        session: 1,
        capability: 1,
        capability_digest: [1; 16],
        lease_until: until,
        capability_until: until,
        flow_generation: 1,
        link_generation: 1,
    }
}
fn snapshot(rows: &[SnapshotRow], joint: Option<ClusterConfig<3>>) -> SnapshotOwner<4, 3> {
    let mut owner = SnapshotOwner::new(epoch(), config(), joint, member(2), 1).unwrap();
    owner.begin(1, rows.len()).unwrap();
    for (index, row) in rows.iter().enumerate() {
        owner.member(index + 1, *row).unwrap();
    }
    let digest = owner.staging_digest(&mut DigestWorkspace::new()).unwrap();
    owner
        .end(rows.len() + 1, digest, &mut DigestWorkspace::new())
        .unwrap();
    owner
}
fn row(id: u8, state: CoverageState) -> SnapshotRow {
    SnapshotRow {
        member: member(id),
        state,
        state_digest: [id; 16],
    }
}
fn apply(
    owner: &mut ClusterOwner<3, 4, 512>,
    p: &mut flash::Owner<'_>,
    f: &mut flash::Flash,
    now: u64,
) {
    let (h, proof) = flash::commit(p, f, &owner.requirement().unwrap());
    owner.bind(h).unwrap();
    owner.confirm(h, proof, now).unwrap();
    p.proof_retire(h).unwrap();
}
#[test]
fn snapshot_missing_removed_and_joint_voter_are_permanently_ineligible() {
    let mut s = snapshot(
        &[
            row(1, CoverageState::Admitted),
            row(2, CoverageState::Admitted),
        ],
        None,
    );
    assert!(s.ready(1, 10).is_err());
    assert!(s.ineligible());
    s.begin(2, 3).unwrap();
    for i in 1..=3 {
        s.member(usize::from(i), row(i, CoverageState::Admitted))
            .unwrap();
    }
    let digest = s.staging_digest(&mut DigestWorkspace::new()).unwrap();
    s.end(4, digest, &mut DigestWorkspace::new()).unwrap();
    assert!(s.ready(2, 10).is_err());
    let mut target = config();
    target.id = 2;
    target.generation = 2;
    target.members[2] = member(4);
    let mut joint = snapshot(
        &[
            row(1, CoverageState::Admitted),
            row(2, CoverageState::Admitted),
            row(3, CoverageState::Admitted),
        ],
        Some(target),
    );
    assert!(joint.ready(1, 10).is_err());
    let mut suspect = snapshot(
        &[
            row(1, CoverageState::Admitted),
            row(2, CoverageState::Admitted),
            row(3, CoverageState::Suspect),
        ],
        None,
    );
    assert!(suspect.ready(1, 10).is_ok());
    assert!(suspect.ready(10, 10).is_ok());
    assert!(suspect.ready(11, 10).is_err());
    let mut removed = snapshot(
        &[
            row(1, CoverageState::Admitted),
            row(2, CoverageState::Admitted),
            row(3, CoverageState::Removed),
        ],
        None,
    );
    assert!(removed.ready(1, 10).is_err());
}
#[test]
#[allow(clippy::too_many_lines)] // End-to-end durable transition with adversarial admission order.
fn replica_takeover_requires_new_votes_after_readmission_and_epoch_is_terminal() {
    flash::exercise(contract(), |p, f| {
        let mut head = ClusterOwner::<3, 4, 512>::new(2, contract(), member(1)).unwrap();
        let mut scratch = CodecWorkspace::new();
        head.create(
            flash::base(p, contract()),
            epoch(),
            config(),
            1,
            1,
            10000,
            1,
            &mut scratch,
        )
        .unwrap();
        apply(&mut head, p, f, 21);
        let mut body = [0; 1024];
        let n = p.copy_body(contract().domain, &mut body).unwrap();
        let mut backup = ClusterOwner::<3, 4, 512>::new(2, contract(), member(2)).unwrap();
        backup
            .restore(flash::base(p, contract()), &body[..n], &mut scratch)
            .unwrap();
        for i in 1..=3 {
            backup.observe_member(fact(i, 500), 21, false).unwrap();
        }
        let mut s = snapshot(
            &[
                row(1, CoverageState::Admitted),
                row(2, CoverageState::Admitted),
                row(3, CoverageState::Admitted),
            ],
            None,
        );
        backup
            .backup_ready(
                flash::base(p, contract()),
                s.ready(22, 10).unwrap(),
                22,
                10000,
                2,
                &mut scratch,
            )
            .unwrap();
        apply(&mut backup, p, f, 23);
        let next = ClusterEpoch {
            term: 2,
            head: [2; 16],
            ..epoch()
        };
        backup
            .transition_begin(
                flash::base(p, contract()),
                TransitionKind::Takeover,
                next,
                2,
                24,
                400,
                3,
                &mut scratch,
            )
            .unwrap();
        apply(&mut backup, p, f, 25);
        for id in [1, 2] {
            let vote = VoteEvidence {
                voter: member(id),
                session: 1,
                capability: 1,
                vote_id: 1,
                digest: [id; 16],
            };
            backup
                .vote(
                    flash::base(p, contract()),
                    2,
                    vote,
                    26,
                    10000,
                    4,
                    &mut scratch,
                )
                .unwrap();
            apply(&mut backup, p, f, 27);
        }
        assert_eq!(backup.view().unwrap().phase, ClusterPhase::Quorum);
        backup.observe_member(fact(1, 600), 28, true).unwrap();
        assert_eq!(
            backup.transition_commit(flash::base(p, contract()), 2, 29, 10000, 5, &mut scratch),
            Err(Error::Access)
        );
        backup
            .vote(
                flash::base(p, contract()),
                2,
                VoteEvidence {
                    voter: member(1),
                    session: 1,
                    capability: 1,
                    vote_id: 2,
                    digest: [4; 16],
                },
                30,
                10000,
                6,
                &mut scratch,
            )
            .unwrap();
        // Re-admission while the exact immutable vote body is in flight also invalidates it.
        backup.observe_member(fact(1, 700), 31, true).unwrap();
        apply(&mut backup, p, f, 32);
        assert!(
            backup
                .transition_commit(flash::base(p, contract()), 2, 33, 10000, 7, &mut scratch)
                .is_err()
        );
        backup
            .vote(
                flash::base(p, contract()),
                2,
                VoteEvidence {
                    voter: member(1),
                    session: 1,
                    capability: 1,
                    vote_id: 3,
                    digest: [5; 16],
                },
                34,
                10000,
                8,
                &mut scratch,
            )
            .unwrap();
        apply(&mut backup, p, f, 35);
        backup
            .transition_commit(flash::base(p, contract()), 2, 36, 10000, 9, &mut scratch)
            .unwrap();
        apply(&mut backup, p, f, 37);
        let view = backup.view().unwrap();
        backup.step(1000).unwrap();
        assert_eq!(backup.view().unwrap().phase, ClusterPhase::EpochDurable);
        assert_eq!(backup.view().unwrap().epoch, view.epoch);
        assert!(
            backup
                .vote(
                    flash::base(p, contract()),
                    2,
                    VoteEvidence {
                        voter: member(3),
                        session: 1,
                        capability: 1,
                        vote_id: 4,
                        digest: [6; 16]
                    },
                    1001,
                    10000,
                    10,
                    &mut scratch
                )
                .is_err()
        );
    });
}
#[test]
fn joint_install_revokes_authority_immediately_and_cannot_abort() {
    flash::exercise(contract(), |p, f| {
        let mut owner = ClusterOwner::<3, 4, 512>::new(2, contract(), member(1)).unwrap();
        let mut scratch = CodecWorkspace::new();
        owner
            .create(
                flash::base(p, contract()),
                epoch(),
                config(),
                1,
                1,
                10000,
                1,
                &mut scratch,
            )
            .unwrap();
        apply(&mut owner, p, f, 21);
        owner.observe_member(fact(1, 1000), 22, false).unwrap();
        owner.observe_member(fact(2, 1000), 22, false).unwrap();
        owner.renew_authority(22, 900).unwrap();
        let mut target = config();
        target.id = 2;
        target.generation = 2;
        target.members = [member(1), member(3), member(4)];
        owner
            .config_prepare(
                flash::base(p, contract()),
                target,
                2,
                23,
                10000,
                2,
                &mut scratch,
            )
            .unwrap();
        apply(&mut owner, p, f, 24);
        owner
            .config_joint(flash::base(p, contract()), 2, 25, 10000, 3, &mut scratch)
            .unwrap();
        apply(&mut owner, p, f, 26);
        assert!(owner.authority_preflight(27).is_err());
        assert!(!owner.view().unwrap().quorum);
        assert!(
            owner
                .abort_prepared(flash::base(p, contract()), 2, 28, 10000, 4, &mut scratch)
                .is_err()
        );
    });
}
#[test]
fn malformed_record_and_foreign_directory_never_compare_terms() {
    let mut d = DirectoryOwner::<2>::new();
    let e = DirectoryEntry {
        realm: 1,
        epoch: epoch(),
        generation: 1,
        config_generation: 1,
        until: 100,
        digest: [1; 16],
    };
    d.ingest_authenticated(e, 1).unwrap();
    d.ingest_authenticated(
        DirectoryEntry {
            epoch: ClusterEpoch {
                cluster: 2,
                term: 999,
                ..epoch()
            },
            ..e
        },
        1,
    )
    .unwrap();
    assert_eq!(d.get(1, 1, 2).unwrap().epoch.term, 1);
    assert!(
        d.ingest_authenticated(
            DirectoryEntry {
                digest: [2; 16],
                ..e
            },
            3
        )
        .is_err()
    );
    assert_eq!(d.step(100, 1), 1);
    assert!(d.get(1, 1, 100).is_err());
    flash::exercise(contract(), |p, f| {
        let mut raw = [0u8; 480];
        raw[..4].copy_from_slice(b"CLS6");
        let mut update = DurableUpdate::<512>::prepare(
            flash::base(p, contract()),
            &raw,
            1,
            10000,
            1,
            &mut CodecWorkspace::new(),
        )
        .unwrap();
        let (h, proof) = flash::commit(p, f, &update.request());
        update.bind(h).unwrap();
        update.confirm(h, proof).unwrap();
        p.proof_retire(h).unwrap();
        let mut c = ClusterOwner::<3, 4, 512>::new(2, contract(), member(1)).unwrap();
        assert!(
            c.restore(flash::base(p, contract()), &raw, &mut CodecWorkspace::new())
                .is_err()
        );
    });
}

#[test]
#[allow(clippy::too_many_lines)] // Two real durable Owners and irreversible handover lifecycle.
fn handover_checks_current_target_dependencies_and_reload_fence_is_irreversible() {
    flash::exercise(contract(), |hp, hf| {
        let mut scratch = CodecWorkspace::new();
        let mut head = ClusterOwner::<3, 4, 512>::new(1, contract(), member(1)).unwrap();
        head.create(
            flash::base(hp, contract()),
            epoch(),
            config(),
            1,
            1,
            10000,
            1,
            &mut scratch,
        )
        .unwrap();
        apply(&mut head, hp, hf, 21);
        for id in 1..=3 {
            head.observe_member(fact(id, 500), 21, false).unwrap();
        }
        head.renew_authority(21, 450).unwrap();
        flash::exercise(contract(), |bp, bf| {
            let mut seed = ClusterOwner::<3, 4, 512>::new(1, contract(), member(1)).unwrap();
            seed.create(
                flash::base(bp, contract()),
                epoch(),
                config(),
                1,
                1,
                10000,
                1,
                &mut scratch,
            )
            .unwrap();
            apply(&mut seed, bp, bf, 21);
            let mut body = [0; 512];
            let n = bp.copy_body(contract().domain, &mut body).unwrap();
            let mut target = ClusterOwner::<3, 4, 512>::new(2, contract(), member(2)).unwrap();
            target
                .restore(flash::base(bp, contract()), &body[..n], &mut scratch)
                .unwrap();
            for id in 1..=3 {
                target.observe_member(fact(id, 500), 21, false).unwrap();
            }
            assert!(target.handover_target_ready(22).is_err());
            let mut s = snapshot(
                &[
                    row(1, CoverageState::Admitted),
                    row(2, CoverageState::Admitted),
                    row(3, CoverageState::Admitted),
                ],
                None,
            );
            target
                .backup_ready(
                    flash::base(bp, contract()),
                    s.ready(22, 10).unwrap(),
                    22,
                    10000,
                    2,
                    &mut scratch,
                )
                .unwrap();
            assert!(target.handover_target_ready(22).is_err());
            apply(&mut target, bp, bf, 23);
            let ready = target.handover_target_ready(24).unwrap();
            let mut changed = fact(2, 500);
            changed.capability_digest = [2; 16];
            head.observe_member(changed, 24, false).unwrap();
            let before = head.view().unwrap();
            assert_eq!(
                head.handover_commit(
                    flash::base(hp, contract()),
                    ready,
                    2,
                    24,
                    10000,
                    3,
                    &mut scratch
                ),
                Err(Error::Access)
            );
            assert_eq!(head.view().unwrap(), before);
            head.observe_member(fact(2, 500), 24, false).unwrap();
            head.handover_commit(
                flash::base(hp, contract()),
                ready,
                2,
                24,
                10000,
                4,
                &mut scratch,
            )
            .unwrap();
            assert!(head.handover_proof().is_err());
            assert!(head.authority_preflight(24).is_err());
            apply(&mut head, hp, hf, 25);
            let commit = head.handover_proof().unwrap();
            assert!(head.view().unwrap().fenced);
            assert!(head.renew_authority(26, 450).is_err());
            target
                .accept_handover(
                    flash::base(bp, contract()),
                    commit,
                    26,
                    10000,
                    5,
                    &mut scratch,
                )
                .unwrap();
            apply(&mut target, bp, bf, 27);
            assert_eq!(target.view().unwrap().epoch.term, 2);
            assert_eq!(target.view().unwrap().epoch.head, [2; 16]);
            let mut durable = [0; 512];
            let n = hp.copy_body(contract().domain, &mut durable).unwrap();
            let mut rebooted = ClusterOwner::<3, 4, 512>::new(1, contract(), member(1)).unwrap();
            rebooted
                .restore(flash::base(hp, contract()), &durable[..n], &mut scratch)
                .unwrap();
            for id in 1..=3 {
                rebooted.observe_member(fact(id, 500), 28, false).unwrap();
            }
            assert!(rebooted.view().unwrap().fenced);
            assert!(rebooted.renew_authority(28, 450).is_err());
        });
    });
}
