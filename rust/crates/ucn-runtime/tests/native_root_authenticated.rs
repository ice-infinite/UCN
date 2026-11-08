#![cfg(feature = "native-backend")]
//! Real authenticated Backend → `NativeRoot` → application, without hardware.
#[path = "native_backend_support/fixture.rs"]
mod fixture;
use std::cell::{Cell, RefCell};
use ucn_adapter::{
    AdapterEventIngress, AdapterOwner, DriverCancel, DriverSubmit, LinkHandle, RxMeta,
    TerminalOutcome, TxDriver, TxToken,
};
use ucn_native::{
    Channel, CommandPort, Destination, Inbox, NativeRoot, QueuePolicy, Sample, SampleHandler,
    SecurityMinimum, Source,
};
use ucn_runtime::NativeSessionBackend;
use ucn_security::{Fingerprint, OriginSequenceOwner};
use ucn_types::{AddressWidth, Error, OriginSequence, Result, ServiceId, TrafficClass};
struct Counter(u32);
impl OriginSequenceOwner for Counter {
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.0)
    }
    fn burn(&mut self, n: OriginSequence) -> Result<()> {
        if n.get() != self.0 {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}
struct Driver {
    bytes: &'static RefCell<Vec<u8>>,
}
impl TxDriver for Driver {
    fn submit(&mut self, _: LinkHandle, frame: &[u8], _: TxToken) -> DriverSubmit {
        *self.bytes.borrow_mut() = frame.to_vec();
        DriverSubmit::Complete(TerminalOutcome::Success)
    }
    fn cancel(&mut self, _: TxToken) -> DriverCancel {
        DriverCancel::Cancelled
    }
}
struct Handler {
    calls: Cell<usize>,
    protection: SecurityMinimum,
}
impl SampleHandler for Handler {
    fn receive(&self, sample: &Sample, _: &dyn CommandPort) {
        assert_eq!(sample.principal, Some([1; 16]));
        assert_eq!(sample.protection, Some(self.protection));
        assert_eq!(&sample.payload[..3], b"abc");
        assert_eq!(sample.received_us, 23);
        self.calls.set(self.calls.get() + 1);
    }
}
type Ingress = AdapterEventIngress<1, 2, 2, 128>;
fn backend(
    local: u8,
    peer: u8,
    ingress: &'static Ingress,
    bytes: &'static RefCell<Vec<u8>>,
    encrypted: bool,
) -> (&'static mut dyn ucn_native::Backend, LinkHandle) {
    let f = fixture::active(local, peer, encrypted);
    let mut adapter = AdapterOwner::new(u32::from(local), ingress).unwrap();
    let link = adapter.open_link(0, 1, 128).unwrap();
    let backend = Box::leak(Box::new(
        NativeSessionBackend::new(
            f.owner,
            Box::leak(Box::new(adapter)),
            Box::leak(Box::new(Driver { bytes })),
            f.crypto,
            Box::leak(Box::new(Counter(1))),
            f.handle,
            f.facts,
            Fingerprint::new([5; 16]).unwrap(),
            AddressWidth::A1,
            ServiceId::new(0x40).unwrap(),
            u64::from(local),
            u64::from(peer),
        )
        .unwrap(),
    ));
    (backend, link)
}
fn channel(encrypted: bool) -> Channel {
    let mut c = Channel {
        id: 1,
        service: ServiceId::new(0x40).unwrap(),
        schema_id: 1,
        schema_revision: 1,
        payload_bytes: 3,
        traffic_class: TrafficClass::Q1,
        queue: QueuePolicy::KeepLatest,
        security: if encrypted {
            SecurityMinimum::Encrypted
        } else {
            SecurityMinimum::OriginAuth
        },
        max_local_age_us: 20000,
        sources: [None; 16],
        targets: [None; 8],
    };
    let source = fixture::security::binding(1);
    c.sources[0] = Some(Source {
        address: source.address,
        binding: source.generation,
        principal_digest: 1,
    });
    let target = fixture::security::binding(2);
    c.targets[0] = Some(Destination {
        address: target.address,
        binding: target.generation,
    });
    c
}
#[test]
fn actual_native_publish_authenticated_sample_and_queued_session_expiry() {
    native_roundtrip(false);
}
#[test]
fn actual_native_encrypted_provider_path_and_queued_session_expiry() {
    native_roundtrip(true);
}
#[test]
fn assembly_rejects_service_without_any_acl_direction() {
    let f = fixture::active(1, 2, false);
    let ingress = Box::leak(Box::new(Ingress::new(1).unwrap()));
    let mut adapter = AdapterOwner::new(1, ingress).unwrap();
    adapter.open_link(0, 1, 128).unwrap();
    let bytes = Box::leak(Box::new(RefCell::new(Vec::new())));
    let mut driver = Driver { bytes };
    let mut counter = Counter(1);
    let result = NativeSessionBackend::new(
        f.owner,
        &mut adapter,
        &mut driver,
        f.crypto,
        &mut counter,
        f.handle,
        f.facts,
        Fingerprint::new([5; 16]).unwrap(),
        AddressWidth::A1,
        ServiceId::new(0x41).unwrap(),
        1,
        2,
    );
    assert!(matches!(result, Err(Error::State)));
    assert!(bytes.borrow().is_empty());
}
fn native_roundtrip(encrypted: bool) {
    let ia = Box::leak(Box::new(Ingress::new(1).unwrap()));
    let ib = Box::leak(Box::new(Ingress::new(2).unwrap()));
    let bytes_a = Box::leak(Box::new(RefCell::new(Vec::new())));
    let bytes_b = Box::leak(Box::new(RefCell::new(Vec::new())));
    let inbox_a = Box::leak(Box::new(Inbox::<4>::new()));
    let inbox_b = Box::leak(Box::new(Inbox::<4>::new()));
    let ca = channel(encrypted);
    let mut cb = ca;
    cb.targets = [None; 8];
    let (backend_a, _) = backend(1, 2, ia, bytes_a, encrypted);
    let (backend_b, lb) = backend(2, 1, ib, bytes_b, encrypted);
    let mut a = NativeRoot::new(backend_a, inbox_a, 1, &[ca]).unwrap();
    let mut b = NativeRoot::new(backend_b, inbox_b, 2, &[cb]).unwrap();
    let handler = Box::leak(Box::new(Handler {
        calls: Cell::new(0),
        protection: ca.security,
    }));
    let _subscription = inbox_b.subscribe(1, handler).unwrap();
    let publisher = inbox_a.publisher(1).unwrap();
    a.start(21).unwrap();
    b.start(21).unwrap();
    inbox_a.publish(publisher, b"abc").unwrap();
    a.step(22, 16).unwrap();
    assert_eq!(
        &bytes_a.borrow()[..3],
        &[0x61, 0x40, if encrypted { 0x81 } else { 0x41 }]
    );
    assert_eq!(bytes_a.borrow().len(), 32);
    ib.rx_publish(
        lb,
        &bytes_a.borrow(),
        RxMeta {
            timestamp_us: 23,
            sender_discriminator: 0,
        },
    )
    .unwrap();
    b.step(24, 16).unwrap();
    assert_eq!(b.dispatch(25, 16), Ok(1));
    assert_eq!(handler.calls.get(), 1);
    inbox_a.publish(publisher, b"abc").unwrap();
    a.step(26, 16).unwrap();
    ib.rx_publish(
        lb,
        &bytes_a.borrow(),
        RxMeta {
            timestamp_us: 27,
            sender_discriminator: 0,
        },
    )
    .unwrap();
    b.step(28, 16).unwrap();
    assert_eq!(b.dispatch(10001, 16), Ok(0));
    assert_eq!(handler.calls.get(), 1);
    b.stop().unwrap();
    b.step(10002, 16).unwrap();
    assert!(b.quiescent());
}
