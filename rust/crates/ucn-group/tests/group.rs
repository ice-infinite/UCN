#![cfg(feature = "static")]
//! Frozen Group snapshot/receipt/capacity and permanent retirement gates.
use ucn_group::*;
fn definition() -> GroupDefinition<3> {
    GroupDefinition {
        realm: 1,
        group: 1,
        generation: 1,
        key_generation: 1,
        tree_generation: 1,
        service: 0x40,
        scope_mask: 0x1f,
        fanout_limit: 3,
        count: 3,
        members: [
            Member {
                principal: [1; 16],
                address: 1,
                binding_generation: 1,
                sender_slot: 1,
                weight: 1,
            },
            Member {
                principal: [2; 16],
                address: 2,
                binding_generation: 1,
                sender_slot: 2,
                weight: 1,
            },
            Member {
                principal: [3; 16],
                address: 3,
                binding_generation: 1,
                sender_slot: 3,
                weight: 1,
            },
        ],
        exact_principal: true,
        public_static: false,
        quorum_weight: 2,
    }
}
#[test]
fn scope_never_claims_local_acceptance_as_remote_success() {
    let entries = [definition()];
    let manifest = StaticManifest {
        realm: 1,
        generation: 1,
        minimum_generation: 1,
        entries: &entries,
    };
    let mut owner = StaticGroupOwner::<1, 2, 3>::new(1, manifest).unwrap();
    assert!(
        owner
            .send_begin(1, SuccessScope::All, 0, false, 1, 100)
            .is_err()
    );
    let h = owner
        .send_begin(1, SuccessScope::All, 0, true, 1, 100)
        .unwrap();
    let mut send = 0;
    for i in 0..3 {
        let (id, m) = owner.attempt(h, i, 2).unwrap();
        send = id;
        assert_eq!(m, entries[0].members[i]);
    }
    assert!(!owner.result(h).unwrap().complete);
    for m in entries[0].members {
        owner.receipt(h, send, m, 1, 1, 3).unwrap();
    }
    assert!(owner.result(h).unwrap().complete);
    owner.retire_send(h).unwrap();
    assert!(owner.result(h).is_err());
}
#[test]
fn weighted_quorum_subset_timeout_and_permanent_retire() {
    let entries = [definition()];
    let mut owner = StaticGroupOwner::<1, 2, 3>::new(
        1,
        StaticManifest {
            realm: 1,
            generation: 1,
            minimum_generation: 1,
            entries: &entries,
        },
    )
    .unwrap();
    let h = owner
        .send_begin(1, SuccessScope::Quorum, 0, true, 1, 100)
        .unwrap();
    let (id, a) = owner.attempt(h, 0, 2).unwrap();
    let (_, b) = owner.attempt(h, 1, 2).unwrap();
    let mut bad = a;
    bad.binding_generation = 2;
    assert!(owner.receipt(h, id, bad, 1, 1, 3).is_err());
    owner.receipt(h, id, a, 1, 1, 3).unwrap();
    assert!(!owner.result(h).unwrap().complete);
    owner.receipt(h, id, b, 1, 1, 3).unwrap();
    assert!(owner.result(h).unwrap().complete);
    let subset = owner
        .send_begin(1, SuccessScope::Subset, 4, true, 4, 20)
        .unwrap();
    owner.step(20, 2).unwrap();
    assert!(owner.result(subset).unwrap().failed);
    owner.retire_group(1).unwrap();
    assert!(
        owner
            .send_begin(1, SuccessScope::LocalOnly, 0, false, 21, 100)
            .is_err()
    );
}
