//! Adapter Token、同步早到、并发 completion 与原子 RX 的公开合同回归。

use std::sync::{Arc, Barrier};
use std::thread;

use ucn_adapter::{
    AdapterEventIngress, AdapterOwner, DriverCancel, DriverSubmit, LinkHandle, RxMeta,
    TerminalOutcome, TxDriver, TxState, TxToken,
};
use ucn_types::Error;

type Ingress = AdapterEventIngress<2, 2, 2, 64>;

#[test]
fn owner_storage_never_copies_caller_owned_frame_arrays() {
    type SmallIngress = AdapterEventIngress<1, 1, 1, 8>;
    type LargeIngress = AdapterEventIngress<1, 1, 1, 1024>;
    type SmallOwner = AdapterOwner<'static, 1, 1, 1, 8>;
    type LargeOwner = AdapterOwner<'static, 1, 1, 1, 1024>;

    assert_eq!(
        std::mem::size_of::<SmallOwner>(),
        std::mem::size_of::<LargeOwner>()
    );
    assert!(std::mem::size_of::<LargeIngress>() > std::mem::size_of::<SmallIngress>());
    assert!(!std::mem::needs_drop::<SmallIngress>());
    assert!(!std::mem::needs_drop::<SmallOwner>());
}

struct ScriptDriver<'a> {
    ingress: &'a Ingress,
    submit_result: DriverSubmit,
    submit_completion: Option<TerminalOutcome>,
    cancel_result: DriverCancel,
    cancel_completion: Option<TerminalOutcome>,
    submits: usize,
    cancels: usize,
}

impl<'a> ScriptDriver<'a> {
    fn new(ingress: &'a Ingress, submit_result: DriverSubmit) -> Self {
        Self {
            ingress,
            submit_result,
            submit_completion: None,
            cancel_result: DriverCancel::NotCancelled,
            cancel_completion: None,
            submits: 0,
            cancels: 0,
        }
    }
}

impl TxDriver for ScriptDriver<'_> {
    fn submit(&mut self, _link: LinkHandle, _frame: &[u8], token: TxToken) -> DriverSubmit {
        self.submits += 1;
        if let Some(outcome) = self.submit_completion {
            self.ingress
                .tx_complete(token, outcome)
                .expect("synchronous completion must publish");
        }
        self.submit_result
    }

    fn cancel(&mut self, token: TxToken) -> DriverCancel {
        self.cancels += 1;
        if let Some(outcome) = self.cancel_completion {
            self.ingress
                .tx_complete(token, outcome)
                .expect("cancel-time completion must publish");
        }
        self.cancel_result
    }
}

struct RetryDriver<'a> {
    ingress: &'a AdapterEventIngress<1, 1, 1, 16>,
    attempts: usize,
}

impl TxDriver for RetryDriver<'_> {
    fn submit(&mut self, _link: LinkHandle, _frame: &[u8], token: TxToken) -> DriverSubmit {
        self.attempts += 1;
        if self.attempts < 3 {
            DriverSubmit::NotSubmitted
        } else {
            self.ingress
                .tx_complete(token, TerminalOutcome::Success)
                .expect("early completion");
            DriverSubmit::Submitted
        }
    }

    fn cancel(&mut self, _token: TxToken) -> DriverCancel {
        DriverCancel::Cancelled
    }
}

#[test]
fn synchronous_early_completion_is_merged_before_submit_returns() {
    let ingress = Ingress::new(11).expect("ingress");
    let mut owner = AdapterOwner::new(11, &ingress).expect("owner");
    let link = owner.open_link(0, 7, 32).expect("link");
    let token = owner.tx_reserve(link, 9).expect("token");
    let mut driver = ScriptDriver::new(&ingress, DriverSubmit::Submitted);
    driver.submit_completion = Some(TerminalOutcome::Success);

    let view = owner
        .tx_submit(token, &[1, 2, 3], &mut driver)
        .expect("submit");
    assert_eq!(view.state, TxState::Completed);
    assert_eq!(view.terminal, Some(TerminalOutcome::Success));
    assert_eq!(view.core_tx_slot, 9);
    assert_eq!(driver.submits, 1);

    assert_eq!(ingress.tx_complete(token, TerminalOutcome::Success), Ok(()));
    assert_eq!(owner.tx_view(token), Ok(view));
    assert_eq!(owner.tx_retire(token), Ok(()));
}

#[test]
fn not_submitted_keeps_the_same_attempt_retryable() {
    let ingress = AdapterEventIngress::<1, 1, 1, 16>::new(12).expect("ingress");
    let mut owner = AdapterOwner::new(12, &ingress).expect("owner");
    let link = owner.open_link(0, 1, 16).expect("link");
    let token = owner.tx_reserve(link, 3).expect("token");

    let mut driver = RetryDriver {
        ingress: &ingress,
        attempts: 0,
    };
    for _ in 0..2 {
        let view = owner
            .tx_submit(token, &[0xA5], &mut driver)
            .expect("retryable backpressure");
        assert_eq!(view.state, TxState::NotSubmitted);
        assert_eq!(view.terminal, None);
    }
    let view = owner
        .tx_submit(token, &[0xA5], &mut driver)
        .expect("third submit");
    assert_eq!(view.state, TxState::Completed);
    assert_eq!(driver.attempts, 3);
    owner.tx_retire(token).expect("retire");

    let replacement = owner.tx_reserve(link, 4).expect("replacement");
    assert_eq!(replacement.slot(), token.slot());
    assert!(replacement.generation() > token.generation());
    assert_eq!(
        ingress.tx_complete(token, TerminalOutcome::Success),
        Err(Error::NotFound)
    );
}

#[test]
fn contradictory_completion_fences_the_link_and_cannot_be_retired() {
    let ingress = Ingress::new(13).expect("ingress");
    let mut owner = AdapterOwner::new(13, &ingress).expect("owner");
    let link = owner.open_link(0, 4, 32).expect("link");
    let token = owner.tx_reserve(link, 1).expect("token");
    let mut driver = ScriptDriver::new(&ingress, DriverSubmit::Submitted);
    assert_eq!(
        owner
            .tx_submit(token, &[1], &mut driver)
            .expect("submitted")
            .state,
        TxState::Submitted
    );
    assert_eq!(ingress.tx_complete(token, TerminalOutcome::Success), Ok(()));
    assert_eq!(
        ingress.tx_complete(token, TerminalOutcome::Failure(Error::Timeout)),
        Err(Error::InDoubt)
    );
    assert_eq!(owner.tx_view(token), Err(Error::InDoubt));
    assert_eq!(owner.tx_retire(token), Err(Error::InDoubt));
    assert_eq!(owner.tx_reserve(link, 2), Err(Error::State));

    let reopened = owner.reopen_link(link, 5, 32).expect("recovery reopen");
    assert_ne!(reopened, link);
    assert_eq!(
        owner.tx_view(token).expect("old outcome sealed"),
        ucn_adapter::TxView {
            state: TxState::Completed,
            terminal: Some(TerminalOutcome::Failure(Error::InDoubt)),
            core_tx_slot: 1,
        }
    );
    owner.tx_retire(token).expect("retire sealed old token");
    assert!(owner.tx_reserve(reopened, 2).is_ok());
}

#[test]
fn cancel_distinguishes_local_cancel_driver_refusal_and_early_completion() {
    let ingress = Ingress::new(14).expect("ingress");
    let mut owner = AdapterOwner::new(14, &ingress).expect("owner");
    let link = owner.open_link(0, 1, 32).expect("link");

    let local = owner.tx_reserve(link, 1).expect("local token");
    let mut driver = ScriptDriver::new(&ingress, DriverSubmit::Submitted);
    assert_eq!(
        owner
            .tx_cancel(local, &mut driver)
            .expect("local cancel")
            .state,
        TxState::Cancelled
    );
    assert_eq!(driver.cancels, 0);
    owner.tx_retire(local).expect("retire local");

    let submitted = owner.tx_reserve(link, 2).expect("submitted token");
    owner
        .tx_submit(submitted, &[2], &mut driver)
        .expect("submit");
    driver.cancel_result = DriverCancel::NotCancelled;
    assert_eq!(
        owner
            .tx_cancel(submitted, &mut driver)
            .expect("not cancelled")
            .state,
        TxState::Submitted
    );
    driver.cancel_completion = Some(TerminalOutcome::Failure(Error::Cancelled));
    assert_eq!(
        owner
            .tx_cancel(submitted, &mut driver)
            .expect("completion wins")
            .state,
        TxState::Completed
    );
    owner.tx_retire(submitted).expect("retire submitted");
}

#[test]
fn tx_capacity_is_fixed_and_existing_slots_are_unchanged() {
    let ingress = Ingress::new(15).expect("ingress");
    let mut owner = AdapterOwner::new(15, &ingress).expect("owner");
    let link = owner.open_link(0, 1, 64).expect("link");
    let first = owner.tx_reserve(link, 10).expect("first");
    let second = owner.tx_reserve(link, 11).expect("second");
    assert_eq!(owner.tx_reserve(link, 12), Err(Error::NoSpace));
    assert_eq!(owner.tx_view(first).expect("first view").core_tx_slot, 10);
    assert_eq!(owner.tx_view(second).expect("second view").core_tx_slot, 11);
}

#[test]
fn rx_frame_and_metadata_are_one_atomic_owned_item() {
    let ingress = Ingress::new(16).expect("ingress");
    let mut owner = AdapterOwner::new(16, &ingress).expect("owner");
    let link = owner.open_link(0, 9, 32).expect("link");
    let meta = RxMeta {
        timestamp_us: 0x1122_3344_5566_7788,
        sender_discriminator: 0xAABB_CCDD,
    };
    assert_eq!(
        ingress.rx_publish(link, &[1, 2, 3], meta),
        Err(Error::State)
    );
    owner
        .try_set_rx_enabled(true)
        .expect("enable RX publication");
    let published = ingress.rx_publish(link, &[1, 2, 3], meta).expect("publish");

    let mut small = [0xEE; 2];
    assert_eq!(owner.rx_claim(&mut small), Err(Error::NoSpace));
    assert_eq!(small, [0xEE; 2]);

    let mut output = [0xCC; 8];
    let view = owner.rx_claim(&mut output).expect("claim");
    assert_eq!(view.token, published);
    assert_eq!(view.frame_bytes, 3);
    assert_eq!(view.meta, meta);
    assert_eq!(view.link, link);
    assert_eq!(&output[..3], &[1, 2, 3]);
    assert_eq!(&output[3..], &[0xCC; 5]);
    assert_eq!(owner.rx_retire(view.token), Ok(()));
    assert_eq!(owner.rx_retire(view.token), Err(Error::State));
}

#[test]
fn rx_capacity_and_stale_link_generation_fail_closed() {
    let ingress = Ingress::new(17).expect("ingress");
    let mut owner = AdapterOwner::new(17, &ingress).expect("owner");
    let link = owner.open_link(0, 2, 8).expect("link");
    owner
        .try_set_rx_enabled(true)
        .expect("enable RX publication");
    let meta = RxMeta {
        timestamp_us: 1,
        sender_discriminator: 2,
    };
    ingress.rx_publish(link, &[1], meta).expect("first");
    ingress.rx_publish(link, &[2], meta).expect("second");
    assert_eq!(ingress.rx_publish(link, &[3], meta), Err(Error::NoSpace));

    let reopened = owner.reopen_link(link, 3, 8).expect("reopen");
    assert_eq!(ingress.rx_publish(link, &[4], meta), Err(Error::NotFound));
    ingress
        .rx_publish(reopened, &[5], meta)
        .expect("new generation");
    let mut output = [0; 8];
    let view = owner.rx_claim(&mut output).expect("claim new only");
    assert_eq!(output[0], 5);
    assert_eq!(view.link, reopened);
}

#[test]
fn concurrent_exact_completions_coalesce_without_data_race() {
    let ingress = Arc::new(Ingress::new(18).expect("ingress"));
    let mut owner = AdapterOwner::new(18, ingress.as_ref()).expect("owner");
    let link = owner.open_link(0, 1, 32).expect("link");
    let token = owner.tx_reserve(link, 1).expect("token");
    let mut driver = ScriptDriver::new(ingress.as_ref(), DriverSubmit::Submitted);
    owner.tx_submit(token, &[9], &mut driver).expect("submit");

    let barrier = Arc::new(Barrier::new(9));
    thread::scope(|scope| {
        for _ in 0..8 {
            let shared = Arc::clone(&ingress);
            let start = Arc::clone(&barrier);
            scope.spawn(move || {
                start.wait();
                loop {
                    match shared.tx_complete(token, TerminalOutcome::Success) {
                        Ok(()) => break,
                        Err(Error::State) => thread::yield_now(),
                        other => panic!("unexpected completion result: {other:?}"),
                    }
                }
            });
        }
        barrier.wait();
    });
    assert_eq!(
        owner.tx_view(token).expect("view").terminal,
        Some(TerminalOutcome::Success)
    );
}

#[test]
fn unknown_submit_is_fenced_and_late_proof_can_resolve_after_reopen() {
    let ingress = Ingress::new(19).expect("ingress");
    let mut owner = AdapterOwner::new(19, &ingress).expect("owner");
    let old_link = owner.open_link(0, 3, 32).expect("link");
    let token = owner.tx_reserve(old_link, 6).expect("token");
    let mut driver = ScriptDriver::new(&ingress, DriverSubmit::Unknown);
    assert_eq!(
        owner.tx_submit(token, &[1], &mut driver),
        Err(Error::InDoubt)
    );
    assert_eq!(owner.tx_reserve(old_link, 7), Err(Error::State));

    let new_link = owner
        .reopen_link(old_link, 4, 32)
        .expect("invalidate old hardware instance");
    ingress
        .tx_complete(token, TerminalOutcome::Failure(Error::Timeout))
        .expect("late exact proof replaces sealed unknown outcome");
    let old_view = owner.tx_view(token).expect("resolved old view");
    assert_eq!(old_view.state, TxState::Completed);
    assert_eq!(
        old_view.terminal,
        Some(TerminalOutcome::Failure(Error::Timeout))
    );
    owner.tx_retire(token).expect("retire resolved token");
    assert!(owner.tx_reserve(new_link, 7).is_ok());
}

#[test]
fn concurrent_rx_publishers_preserve_each_atomic_frame() {
    type ConcurrentIngress = AdapterEventIngress<1, 1, 4, 8>;
    let ingress = Arc::new(ConcurrentIngress::new(20).expect("ingress"));
    let mut owner = AdapterOwner::new(20, ingress.as_ref()).expect("owner");
    let link = owner.open_link(0, 1, 8).expect("link");
    owner
        .try_set_rx_enabled(true)
        .expect("enable RX publication");
    let barrier = Arc::new(Barrier::new(5));

    thread::scope(|scope| {
        for value in 1_u8..=4 {
            let shared = Arc::clone(&ingress);
            let start = Arc::clone(&barrier);
            scope.spawn(move || {
                start.wait();
                loop {
                    match shared.rx_publish(
                        link,
                        &[value; 4],
                        RxMeta {
                            timestamp_us: u64::from(value),
                            sender_discriminator: u32::from(value),
                        },
                    ) {
                        Ok(_) => break,
                        Err(Error::State) => thread::yield_now(),
                        other => panic!("unexpected RX publish result: {other:?}"),
                    }
                }
            });
        }
        barrier.wait();
    });

    let mut seen = [false; 4];
    for _ in 0..4 {
        let mut frame = [0; 8];
        let view = owner.rx_claim(&mut frame).expect("claim");
        let value = frame[0];
        assert!((1..=4).contains(&value));
        assert_eq!(&frame[..4], &[value; 4]);
        assert_eq!(view.frame_bytes, 4);
        assert_eq!(view.meta.timestamp_us, u64::from(value));
        assert_eq!(view.meta.sender_discriminator, u32::from(value));
        seen[usize::from(value - 1)] = true;
        owner.rx_retire(view.token).expect("retire");
    }
    assert_eq!(seen, [true; 4]);
}
