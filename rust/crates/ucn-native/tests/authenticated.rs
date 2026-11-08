//! Facade contract; Runtime integration separately exercises real Security/Adapter.
use std::cell::Cell;
use ucn_core::{EndpointHandler, Lifecycle, SendOptions, Target};
use ucn_native::{
    Backend, BackendMessage, BackendScope, Channel, CommandPort, Destination, Inbox, NativeRoot,
    QueuePolicy, Sample, SampleHandler, SecurityMinimum, Source,
};
use ucn_types::{
    AddressWidth, BindingGeneration, Error, NodeAddress, Result, ServiceId, TrafficClass,
};
fn source(a: u32) -> Source {
    Source {
        address: NodeAddress::new(a, AddressWidth::A0).unwrap(),
        binding: BindingGeneration::active(a).unwrap(),
        principal_digest: u64::from(a),
    }
}
struct Port {
    live: &'static Cell<bool>,
    pending: bool,
}
impl Backend for Port {
    fn supports(&self, s: SecurityMinimum) -> bool {
        s == SecurityMinimum::OriginAuth
    }
    fn local(&self) -> Destination {
        Destination {
            address: source(2).address,
            binding: source(2).binding,
        }
    }
    fn binding(&self, a: NodeAddress) -> Option<BindingGeneration> {
        Some(source(a.get()).binding)
    }
    fn principal(&self, a: NodeAddress) -> Option<u64> {
        Some(u64::from(a.get()))
    }
    fn lifecycle(&self) -> Lifecycle {
        Lifecycle::Initialized
    }
    fn trusted(&self) -> bool {
        false
    }
    fn install(&mut self, _: &[Channel], _: &'static dyn EndpointHandler) -> Result<()> {
        Ok(())
    }
    fn start(&mut self, _: u64) -> Result<()> {
        Ok(())
    }
    fn stop(&mut self) -> Result<()> {
        Ok(())
    }
    fn send(&mut self, _: Target, _: &[u8], _: SendOptions, _: u64) -> Result<()> {
        Err(Error::Unsupported)
    }
    fn step(&mut self, _: u64, _: u16) -> Result<()> {
        Ok(())
    }
    fn poll(&mut self) -> Result<BackendMessage> {
        if !self.pending {
            return Err(Error::NotFound);
        }
        self.pending = false;
        let mut payload = [0; 64];
        payload[0] = 0xA7;
        Ok(BackendMessage {
            source: source(1),
            scope: BackendScope {
                instance: 7,
                generation: 3,
                slot: 1,
            },
            receive_time_us: 10,
            origin_sequence: 9,
            service: ServiceId::new(0x40)?,
            traffic_class: TrafficClass::Q1,
            protection: SecurityMinimum::OriginAuth,
            payload_bytes: 1,
            payload,
        })
    }
    fn validate(&self, scope: BackendScope, s: Source, c: &Channel, now: u64) -> Result<[u8; 16]> {
        if self.live.get()
            && scope
                == (BackendScope {
                    instance: 7,
                    generation: 3,
                    slot: 1,
                })
            && s == source(1)
            && c.service == ServiceId::new(0x40)?
            && now < 100
        {
            Ok([0x51; 16])
        } else {
            Err(Error::Access)
        }
    }
}
struct Handler {
    live: &'static Cell<bool>,
    calls: Cell<u32>,
}
impl SampleHandler for Handler {
    fn receive(&self, s: &Sample, _: &dyn CommandPort) {
        assert_eq!(s.principal, Some([0x51; 16]));
        assert_eq!(s.received_us, 10);
        assert_eq!(s.payload[0], 0xA7);
        self.calls.set(self.calls.get() + 1);
        self.live.set(false);
    }
}
#[test]
fn each_callback_revalidates_and_inbox_cannot_bypass_authority() {
    let live = Box::leak(Box::new(Cell::new(true)));
    let port = Box::leak(Box::new(Port {
        live,
        pending: true,
    }));
    let inbox = Box::leak(Box::new(Inbox::<4>::new()));
    let handler = Box::leak(Box::new(Handler {
        live,
        calls: Cell::new(0),
    }));
    let mut c = Channel {
        id: 1,
        service: ServiceId::new(0x40).unwrap(),
        schema_id: 1,
        schema_revision: 1,
        payload_bytes: 1,
        traffic_class: TrafficClass::Q1,
        queue: QueuePolicy::KeepLatest,
        security: SecurityMinimum::OriginAuth,
        max_local_age_us: 100,
        sources: [None; 16],
        targets: [None; 8],
    };
    c.sources[0] = Some(source(1));
    let mut root = NativeRoot::new(port, inbox, 1, &[c]).unwrap();
    let first = inbox.subscribe(1, handler).unwrap();
    let second = inbox.subscribe(1, handler).unwrap();
    root.start(0).unwrap();
    root.step(11, 2).unwrap();
    assert_eq!(inbox.dispatch(12, 2), Err(Error::Unsupported));
    assert_eq!(root.dispatch(12, 2), Ok(1));
    assert_eq!(handler.calls.get(), 1);
    let _ = (first, second);
}
