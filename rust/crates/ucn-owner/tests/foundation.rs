//! Owner、Callback Gate、Mailbox 与 typed Coordinator 的公开合同回归。

use std::sync::{Arc, Barrier};
use std::thread;

use ucn_owner::{CallbackClaim, CallbackGate, OwnerMailbox, TypedCoordinator, WorkClass};
use ucn_types::Error;

#[test]
fn callback_gate_rejects_reentry_wrong_gate_and_stale_lease() {
    let gate = CallbackGate::new(1).expect("valid gate");
    let other = CallbackGate::new(2).expect("valid gate");
    let first = CallbackClaim::new(10, 20, 1, 7).expect("valid claim");
    let second = CallbackClaim::new(10, 21, 1, 7).expect("valid claim");

    let first_lease = gate.try_enter(first).expect("first enter");
    assert!(gate.is_active());
    assert!(matches!(gate.try_enter(second), Err(Error::State)));
    assert_eq!(other.leave(first_lease), Err(Error::State));
    gate.leave(first_lease).expect("exact leave");
    assert!(!gate.is_active());

    let second_lease = gate.try_enter(second).expect("second enter");
    assert_eq!(gate.leave(first_lease), Err(Error::State));
    assert!(gate.is_active());
    gate.leave(second_lease).expect("current lease leaves");
}

#[test]
fn callback_lease_cannot_cross_another_gate_with_the_same_numeric_id() {
    let first = CallbackGate::new(77).expect("first gate");
    let second = CallbackGate::new(77).expect("second gate");
    let claim = CallbackClaim::new(1, 2, 3, 4).expect("claim");
    let first_lease = first.try_enter(claim).expect("first lease");
    let second_lease = second.try_enter(claim).expect("second lease");

    assert_eq!(second.leave(first_lease), Err(Error::State));
    assert!(second.is_active());
    assert_eq!(second.leave(second_lease), Ok(()));
    assert_eq!(first.leave(first_lease), Ok(()));
}

#[test]
fn callback_gate_has_exactly_one_concurrent_winner() {
    const THREADS: usize = 8;
    let gate = Arc::new(CallbackGate::new(9).expect("valid gate"));
    let barrier = Arc::new(Barrier::new(THREADS));
    let attempted = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let winners = Arc::new(std::sync::atomic::AtomicUsize::new(0));

    thread::scope(|scope| {
        for index in 0..THREADS {
            let gate = Arc::clone(&gate);
            let barrier = Arc::clone(&barrier);
            let attempted = Arc::clone(&attempted);
            let winners = Arc::clone(&winners);
            scope.spawn(move || {
                let claim = CallbackClaim::new(
                    1,
                    u32::try_from(index + 1).expect("bounded operation"),
                    1,
                    1,
                )
                .expect("valid claim");
                barrier.wait();
                let result = gate.try_enter(claim);
                attempted.fetch_add(1, std::sync::atomic::Ordering::AcqRel);
                if let Ok(lease) = result {
                    winners.fetch_add(1, std::sync::atomic::Ordering::AcqRel);
                    while attempted.load(std::sync::atomic::Ordering::Acquire) != THREADS {
                        thread::yield_now();
                    }
                    gate.leave(lease).expect("winner leaves");
                }
            });
        }
    });

    assert_eq!(winners.load(std::sync::atomic::Ordering::Acquire), 1);
    assert!(!gate.is_active());
}

#[test]
fn mailbox_coalesces_occurrences_and_rotates_fairly() {
    let mailbox = OwnerMailbox::new();
    assert_eq!(mailbox.take(), Err(Error::NotFound));

    mailbox.publish(WorkClass::Completion);
    mailbox.publish(WorkClass::Completion);
    mailbox.publish(WorkClass::Invalidation);
    let first = mailbox.take().expect("completion hint");
    assert_eq!(first.work_class, WorkClass::Completion);
    assert_eq!(first.occurrences, 2);

    mailbox.publish(WorkClass::Completion);
    assert_eq!(
        mailbox.take().expect("older invalidation").work_class,
        WorkClass::Invalidation
    );
    assert_eq!(
        mailbox.take().expect("new completion").work_class,
        WorkClass::Completion
    );
    assert_eq!(mailbox.take(), Err(Error::NotFound));
}

#[test]
fn mailbox_accepts_concurrent_publishers_without_losing_counts() {
    const THREADS: usize = 4;
    const PER_THREAD: usize = 1_000;
    let mailbox = Arc::new(OwnerMailbox::new());

    thread::scope(|scope| {
        for _ in 0..THREADS {
            let mailbox = Arc::clone(&mailbox);
            scope.spawn(move || {
                for _ in 0..PER_THREAD {
                    mailbox.publish(WorkClass::Timer);
                }
            });
        }
    });

    let hint = mailbox.take().expect("merged timer hint");
    assert_eq!(hint.work_class, WorkClass::Timer);
    assert_eq!(
        hint.occurrences,
        u32::try_from(THREADS * PER_THREAD).expect("bounded publication count")
    );
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Requirement {
    digest: u64,
    exact_value: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Event {
    Ready(u32),
}

#[test]
fn coordinator_uses_exact_requirement_and_generation_bound_handles() {
    let mut coordinator =
        TypedCoordinator::<Requirement, Event, 2>::new(17).expect("valid coordinator");
    let first_requirement = Requirement {
        digest: 1,
        exact_value: 10,
    };
    let collision = Requirement {
        digest: 1,
        exact_value: 11,
    };

    let first = coordinator.ensure(first_requirement).expect("first slot");
    assert!(first.created);
    let duplicate = coordinator.ensure(first_requirement).expect("exact reuse");
    assert!(!duplicate.created);
    assert_eq!(duplicate.handle, first.handle);

    let second = coordinator
        .ensure(collision)
        .expect("collision is distinct");
    assert!(second.created);
    assert_ne!(second.handle, first.handle);
    assert_eq!(
        coordinator.ensure(Requirement {
            digest: 2,
            exact_value: 12,
        }),
        Err(Error::NoSpace)
    );

    coordinator
        .complete(first.handle, Event::Ready(7))
        .expect("first completion");
    coordinator
        .complete(first.handle, Event::Ready(7))
        .expect("exact duplicate is idempotent");
    assert_eq!(
        coordinator.complete(first.handle, Event::Ready(8)),
        Err(Error::State)
    );
    assert_eq!(
        coordinator
            .view(first.handle)
            .expect("completed view")
            .event,
        Some(Event::Ready(7))
    );
    coordinator.retire(first.handle).expect("retire completion");
    assert_eq!(coordinator.view(first.handle), Err(Error::NotFound));

    let replacement = coordinator.ensure(first_requirement).expect("slot reuse");
    assert_ne!(replacement.handle, first.handle);
    assert_eq!(
        coordinator.complete(first.handle, Event::Ready(7)),
        Err(Error::NotFound)
    );
    coordinator
        .cancel(replacement.handle)
        .expect("cancel pending");
    assert_eq!(coordinator.view(replacement.handle), Err(Error::NotFound));

    assert_eq!(coordinator.cancel(second.handle), Ok(()));
}
