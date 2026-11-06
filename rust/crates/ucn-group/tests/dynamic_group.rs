#![cfg(feature = "dynamic")]
//! Real independent allocator/Context domains and exact reload proofs.
#[path = "../../../tests/support/authority.rs"]
mod authority;
#[path = "../../../tests/support/flash.rs"]
mod flash;
use ucn_durable::DurableContract;
use ucn_group::*;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_types::Error;
fn contract(allocator: bool) -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::GroupPolicyKey,
            id: if allocator {
                group_allocator_domain_id(1)
            } else {
                9
            },
        },
        schema: if allocator {
            GROUP_ALLOCATOR_SCHEMA
        } else {
            GROUP_CONTEXT_SCHEMA
        },
        version: if allocator { 1 } else { GROUP_CONTEXT_VERSION },
        operation: if allocator {
            GROUP_ALLOCATOR_SCHEMA
        } else {
            GROUP_CONTEXT_SCHEMA
        },
    }
}
fn record() -> GroupRecord<1> {
    GroupRecord {
        definition: GroupDefinition {
            realm: 1,
            group: 1,
            generation: 1,
            key_generation: 1,
            tree_generation: 1,
            service: 0x40,
            scope_mask: 0x1f,
            fanout_limit: 1,
            count: 1,
            members: [Member {
                principal: [2; 16],
                address: 2,
                binding_generation: 1,
                sender_slot: 1,
                weight: 1,
            }],
            exact_principal: true,
            public_static: false,
            quorum_weight: 1,
        },
        retired: false,
        policy_generation: 1,
        member_generation: 1,
        opcode: 9,
        allocated_high_water: 1,
        security_digest: [5; 16],
    }
}
#[test]
#[allow(clippy::too_many_lines)]
fn allocator_commit_precedes_context_and_reboot_burns_unpublished_ids() {
    authority::exercise(|identity| {
        flash::exercise(contract(true), |ap, af| {
            flash::exercise(contract(false), |cp, cf| {
                let ac = contract(true);
                let cc = contract(false);
                let mut work = CodecWorkspace::new();
                let mut allocator = GroupAllocator::new(ac, 1).unwrap();
                let mut group = DynamicGroupOwner::<1, 128>::new(cc, 1).unwrap();
                assert!(
                    allocator
                        .allocate(
                            flash::base(ap, ac),
                            identity.authority_view(21).unwrap(),
                            22,
                            10000,
                            1,
                            &mut work
                        )
                        .is_err()
                );
                allocator
                    .allocate(
                        flash::base(ap, ac),
                        identity.authority_view(21).unwrap(),
                        21,
                        10000,
                        1,
                        &mut work,
                    )
                    .unwrap();
                assert!(
                    allocator
                        .take_permit(identity.authority_view(21).unwrap(), 21)
                        .is_err()
                );
                let (h, p) = flash::commit(ap, af, &allocator.requirement().unwrap());
                allocator.bind(h).unwrap();
                allocator.confirm(h, p).unwrap();
                ap.proof_retire(h).unwrap();
                let permit = allocator
                    .take_permit(identity.authority_view(22).unwrap(), 22)
                    .unwrap();
                assert_eq!(permit.id(), 1);
                assert!(
                    allocator
                        .take_permit(identity.authority_view(22).unwrap(), 22)
                        .is_err()
                );
                group
                    .create(
                        flash::base(cp, cc),
                        permit,
                        record(),
                        identity.authority_view(22).unwrap(),
                        22,
                        10000,
                        1,
                        &mut work,
                    )
                    .unwrap();
                assert!(group.view().is_err());
                let (h, p) = flash::commit(cp, cf, &group.requirement().unwrap());
                group.bind(h).unwrap();
                group
                    .confirm(h, p, Some(identity.authority_view(23).unwrap()), 23)
                    .unwrap();
                cp.proof_retire(h).unwrap();
                assert_eq!(group.view().unwrap().1, DynamicGroupPhase::Active);
                assert!(
                    group
                        .authorized(identity.authority_view(23).unwrap(), 24)
                        .is_err()
                );
                let mut restored = DynamicGroupOwner::<1, 128>::new(cc, 1).unwrap();
                let mut body = [0; 128];
                let n = cp.copy_body(cc.domain, &mut body).unwrap();
                assert_eq!(n, 92);
                restored
                    .restore(flash::base(cp, cc), &body[..n], &mut work)
                    .unwrap();
                assert_eq!(restored.view().unwrap().1, DynamicGroupPhase::Fenced);
                assert!(
                    restored
                        .authorized(identity.authority_view(24).unwrap(), 24)
                        .is_err()
                );
                group
                    .retire(
                        flash::base(cp, cc),
                        identity.authority_view(24).unwrap(),
                        24,
                        10000,
                        2,
                        &mut work,
                    )
                    .unwrap();
                let (h, p) = flash::commit(cp, cf, &group.requirement().unwrap());
                group.bind(h).unwrap();
                group.confirm(h, p, None, 25).unwrap();
                cp.proof_retire(h).unwrap();
                assert_eq!(group.view().unwrap().1, DynamicGroupPhase::Retired);
                allocator
                    .allocate(
                        flash::base(ap, ac),
                        identity.authority_view(25).unwrap(),
                        25,
                        10000,
                        2,
                        &mut work,
                    )
                    .unwrap();
                let (h, p) = flash::commit(ap, af, &allocator.requirement().unwrap());
                allocator.bind(h).unwrap();
                allocator.confirm(h, p).unwrap();
                ap.proof_retire(h).unwrap();
                // Crash after allocator commit but before Context commit: 2 is permanently burned.
                let mut reboot = GroupAllocator::new(ac, 1).unwrap();
                let n = ap.copy_body(ac.domain, &mut body).unwrap();
                reboot
                    .restore(flash::base(ap, ac), &body[..n], &mut work)
                    .unwrap();
                assert_eq!(reboot.high_water(), 2);
                assert!(
                    reboot
                        .take_permit(identity.authority_view(26).unwrap(), 26)
                        .is_err()
                );
                reboot
                    .allocate(
                        flash::base(ap, ac),
                        identity.authority_view(26).unwrap(),
                        26,
                        10000,
                        3,
                        &mut work,
                    )
                    .unwrap();
                let (h, p) = flash::commit(ap, af, &reboot.requirement().unwrap());
                reboot.bind(h).unwrap();
                reboot.confirm(h, p).unwrap();
                ap.proof_retire(h).unwrap();
                let permit = reboot
                    .take_permit(identity.authority_view(27).unwrap(), 27)
                    .unwrap();
                assert_eq!(permit.id(), 3);
                group
                    .create(
                        flash::base(cp, cc),
                        permit,
                        record(),
                        identity.authority_view(27).unwrap(),
                        27,
                        10000,
                        3,
                        &mut work,
                    )
                    .unwrap();
                let (h, p) = flash::commit(cp, cf, &group.requirement().unwrap());
                group.bind(h).unwrap();
                group.confirm(h, p, None, 28).unwrap();
                cp.proof_retire(h).unwrap();
                assert_eq!(group.view().unwrap().0.definition.group, 3);
                assert_eq!(group.view().unwrap().1, DynamicGroupPhase::Fenced);
            });
        });
    });
}
#[test]
fn logical_record_profile_bounds_exact_padding_and_no_runtime_handles() {
    let mut bad_allocator = contract(true);
    bad_allocator.domain.id = 7;
    assert!(GroupAllocator::new(bad_allocator, 1).is_err());
    let mut bad_context = contract(false);
    bad_context.domain.id = group_allocator_domain_id(1);
    assert!(DynamicGroupOwner::<4, 256>::new(bad_context, 1).is_err());
    let r = record();
    let mut bytes = [0xa5; 128];
    let n = encode_group_record(r, &mut bytes).unwrap();
    assert_eq!(n, 92);
    assert_eq!(decode_group_record::<1>(&bytes[..n]).unwrap(), r);
    bytes[33] = 1;
    assert_eq!(decode_group_record::<1>(&bytes[..n]), Err(Error::Malformed));
    for (members, bytes) in [(4, 176), (8, 288), (16, 512)] {
        assert_eq!(64 + 28 * members, bytes);
    }
    assert!(DynamicGroupOwner::<4, 256>::new(contract(false), 1).is_ok());
    assert!(DynamicGroupOwner::<16, 1024>::new(contract(false), 1).is_ok());
    assert!(DynamicGroupOwner::<16, 256>::new(contract(false), 1).is_err());
    let mut bad = r;
    bad.opcode = 0;
    let mut out = [0xa5; 128];
    assert!(encode_group_record(bad, &mut out).is_err());
    assert_eq!(out, [0xa5; 128]);
}
