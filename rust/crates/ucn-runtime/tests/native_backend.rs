#![cfg(feature = "native-backend")]
//! Actual Foundation-activated Security and physical Adapter software integration.
#[path = "../../../tests/support/security.rs"]
mod security;
use std::cell::{Cell, RefCell};
use ucn_adapter::{
    AdapterEventIngress, AdapterOwner, DriverCancel, DriverSubmit, LinkHandle, RxMeta,
    TerminalOutcome, TxDriver, TxToken,
};
use ucn_core::{SendOptions, Target};
use ucn_native::{Backend, Channel, QueuePolicy, SecurityMinimum, Source};
use ucn_runtime::NativeSessionBackend;
use ucn_security::{Binding, CurrentFacts, Fingerprint, OriginSequenceOwner};
use ucn_types::{AddressWidth, Error, HopLimit, OriginSequence, Result, ServiceId, TrafficClass};
struct Driver<'a> {
    bytes: &'a RefCell<Vec<u8>>,
    calls: &'a Cell<usize>,
    pressure: usize,
}
impl TxDriver for Driver<'_> {
    fn submit(&mut self, _: LinkHandle, frame: &[u8], _: TxToken) -> DriverSubmit {
        *self.bytes.borrow_mut() = frame.to_vec();
        self.calls.set(self.calls.get() + 1);
        if self.pressure > 0 {
            self.pressure -= 1;
            DriverSubmit::NotSubmitted
        } else {
            DriverSubmit::Complete(TerminalOutcome::Success)
        }
    }
    fn cancel(&mut self, _: TxToken) -> DriverCancel {
        DriverCancel::Cancelled
    }
}
struct Counter(u32);
impl OriginSequenceOwner for Counter {
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.0)
    }
    fn burn(&mut self, n: OriginSequence) -> Result<()> {
        if self.0 != n.get() {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}
fn target(facts: CurrentFacts, service: ServiceId) -> Target {
    Target {
        address: facts.peer.address,
        binding_generation: facts.peer.generation,
        service_id: service,
    }
}
fn options(deadline: u64) -> SendOptions {
    SendOptions {
        traffic_class: TrafficClass::Q1,
        hop_limit: HopLimit::new(1).unwrap(),
        absolute_deadline_us: deadline,
    }
}
fn channel(source: Binding, service: ServiceId) -> Channel {
    let mut c = Channel {
        id: 1,
        service,
        schema_id: 1,
        schema_revision: 1,
        payload_bytes: 3,
        traffic_class: TrafficClass::Q1,
        queue: QueuePolicy::KeepLatest,
        security: SecurityMinimum::OriginAuth,
        max_local_age_us: 100,
        sources: [None; 16],
        targets: [None; 8],
    };
    c.sources[0] = Some(Source {
        address: source.address,
        binding: source.generation,
        principal_digest: 1,
    });
    c
}
fn receive(ingress: &AdapterEventIngress<1, 2, 2, 128>, link: LinkHandle, packet: &[u8], now: u64) {
    ingress
        .rx_publish(
            link,
            packet,
            RxMeta {
                timestamp_us: now,
                sender_discriminator: 0,
            },
        )
        .unwrap();
}
#[test]
fn real_security_and_adapter_roundtrip_replay_backpressure_and_expiry() {
    security::exercise(1, 2, |security_a, ha, fa, crypto_a| {
        security::exercise(2, 1, |security_b, hb, fb, crypto_b| {
            let ingress_a = AdapterEventIngress::<1, 2, 2, 128>::new(11).unwrap();
            let ingress_b = AdapterEventIngress::<1, 2, 2, 128>::new(12).unwrap();
            let mut aa = AdapterOwner::new(11, &ingress_a).unwrap();
            let mut ab = AdapterOwner::new(12, &ingress_b).unwrap();
            aa.open_link(0, 1, 128).unwrap();
            let lb = ab.open_link(0, 1, 128).unwrap();
            let bytes_a = RefCell::new(Vec::new());
            let bytes_b = RefCell::new(Vec::new());
            let calls_a = Cell::new(0);
            let calls_b = Cell::new(0);
            let mut da = Driver {
                pressure: 2,
                bytes: &bytes_a,
                calls: &calls_a,
            };
            let mut db = Driver {
                pressure: 0,
                bytes: &bytes_b,
                calls: &calls_b,
            };
            let mut ca = Counter(1);
            let mut cb = Counter(1);
            let fp = Fingerprint::new([5; 16]).unwrap();
            let service = ServiceId::new(0x40).unwrap();
            let mut a = NativeSessionBackend::new(
                security_a,
                &mut aa,
                &mut da,
                crypto_a,
                &mut ca,
                ha,
                fa,
                fp,
                AddressWidth::A1,
                service,
                1,
                2,
            )
            .unwrap();
            let mut b = NativeSessionBackend::new(
                security_b,
                &mut ab,
                &mut db,
                crypto_b,
                &mut cb,
                hb,
                fb,
                fp,
                AddressWidth::A1,
                service,
                2,
                1,
            )
            .unwrap();
            a.start(21).unwrap();
            b.start(21).unwrap();
            a.send(target(fa, service), b"abc", options(100), 21)
                .unwrap();
            a.step(22, 1).unwrap();
            a.step(23, 1).unwrap();
            let packet = bytes_a.borrow().clone();
            assert_eq!(calls_a.get(), 3);
            receive(&ingress_b, lb, &packet, 24);
            b.step(25, 1).unwrap();
            let message = b.poll().unwrap();
            assert_eq!(&message.payload[..3], b"abc");
            assert_eq!(message.receive_time_us, 24);
            let channel = channel(fa.local, service);
            assert_eq!(
                b.validate(message.scope, message.source, &channel, 26),
                Ok([1; 16])
            );
            assert!(
                b.validate(message.scope, message.source, &channel, 10001)
                    .is_err()
            );
            receive(&ingress_b, lb, &packet, 27);
            b.step(28, 1).unwrap();
            assert!(b.poll().is_err());
            a.step(28, 1).unwrap();
            assert!(
                a.send(target(fa, service), b"abc", options(30), 40)
                    .is_err()
            );
            assert_eq!(calls_a.get(), 3);
            assert!(
                a.send(target(fa, service), b"abc", options(20000), 10001)
                    .is_err()
            );
            assert_eq!(calls_a.get(), 3);
        });
    });
}
