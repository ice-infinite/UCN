//! Actual atomic RX demux across the handoff; no synthesized Security evidence.
use super::*;
struct Counter(u64);
impl C0TransactionOwner for Counter {
    fn preview(&self) -> Result<C0TransactionId> {
        C0TransactionId::new(self.0)
    }
    fn burn(&mut self, v: C0TransactionId) -> Result<()> {
        assert_eq!(self.0, v.get());
        self.0 += 1;
        Ok(())
    }
}
struct Hop(u32);
impl HopSequenceOwner for Hop {
    fn preview(&self) -> Result<HopSequence> {
        HopSequence::new(self.0)
    }
    fn burn(&mut self, v: HopSequence) -> Result<()> {
        assert_eq!(self.0, v.get());
        self.0 += 1;
        Ok(())
    }
}
pub(super) fn arm<'a, 'g>(
    s: &mut PeerReauthRuntime<'a, 'g, 128>,
    r: &mut PeerReauthRuntime<'a, 'g, 128>,
) {
    for (root, link, mtu, owner) in [(s, 4, 127, 501), (r, 10, 128, 601)] {
        root.enable_capability(ReauthCapabilityConfig {
            owner,
            local: super::super::capability_join::record(link, mtu),
            requirements: super::super::capability_join::requirements(),
            service: ServiceId::new(0x40).unwrap(),
            lifetime_us: 1000,
            retry_us: 10,
            max_attempts: 20,
            capability_lease_us: 2000,
            discovery_lease_us: 1000,
        })
        .unwrap();
    }
}
pub(super) fn exchange<'g>(
    s: &mut PeerReauthRuntime<'_, 'g, 128>,
    r: &mut PeerReauthRuntime<'_, 'g, 128>,
    sa: &mut Adapter<'_, 128>,
    ra: &mut Adapter<'_, 128>,
    si: &Ingress<128>,
    ri: &Ingress<128>,
    ss: &mut SecurityOwner<'g, 2, 1>,
    rs: &mut SecurityOwner<'g, 2, 1>,
    signer: &mut Signer,
    ds: &mut Driver,
    dr: &mut Driver,
    fault: Fault,
    start: u64,
    ci: HandshakeCandidate,
    sl: ucn_adapter::LinkHandle,
    rl: ucn_adapter::LinkHandle,
) {
    let mut sc = ReauthCrypto {
        expected: ci,
        record: None,
    };
    let mut rc = ReauthCrypto {
        expected: ci,
        record: None,
    };
    let mut st = Counter(10);
    let mut rt = Counter(10);
    let mut sh = Hop(1);
    let mut rh = Hop(1);
    let mut scratch = SecurityPacketWorkspace::<192>::new();
    let mut saw_overlap = false;
    for now in start + 1..start + 850 {
        if fault == Fault::CapabilityUnknown && s.capability_phase().is_some() {
            ds.submit = DriverSubmit::Unknown;
        }
        let sent = s.drive_with_capability(
            sa,
            ss,
            signer,
            &mut sc,
            &mut st,
            &mut sh,
            ds,
            &mut scratch,
            now,
        );
        if fault == Fault::CapabilityDenied && sent == Err(Error::Access) {
            assert!(s.session(sa, ss, now).is_ok());
            assert!(s.capability_phase().is_none());
            assert!(s.selected_capability(sa, ss, now).is_err());
            s.stop(sa, ss).unwrap();
            r.stop(ra, rs).unwrap();
            return;
        }
        if fault == Fault::CapabilityUnknown
            && !sa.tx_is_quiescent()
            && ds.last_submit == DriverSubmit::Unknown
        {
            assert!(sent.is_err() || sent == Ok(true));
            s.stop(sa, ss).unwrap();
            for _ in 0..16 {
                let _ = s.retire_with_capability(sa, ds, now);
            }
            assert!(!sa.tx_is_quiescent());
            assert!(!s.quiescent(sa));
            return;
        }
        if sent.unwrap() {
            let bytes = ds.frames.last().unwrap();
            publish(ri, rl, bytes, now, 31);
            let got = r.receive_with_capability(ra, rs, signer, &mut rc, &mut scratch, now);
            assert!(
                got.is_ok() || got == Err(Error::State) || got == Err(Error::Replay),
                "responder {got:?}"
            );
        }
        s.retire_with_capability(sa, ds, now).unwrap();
        if r.capability_phase().is_some() && s.phase() != PeerReauthPhase::Established {
            saw_overlap = true;
        }
        if r.drive_with_capability(
            ra,
            rs,
            signer,
            &mut rc,
            &mut rt,
            &mut rh,
            dr,
            &mut scratch,
            now,
        )
        .unwrap()
        {
            let bytes = dr.frames.last().unwrap();
            publish(si, sl, bytes, now, 90);
            let got = s.receive_with_capability(sa, ss, signer, &mut sc, &mut scratch, now);
            assert!(
                got.is_ok() || got == Err(Error::State) || got == Err(Error::Replay),
                "initiator {got:?}"
            );
        }
        r.retire_with_capability(ra, dr, now).unwrap();
        if let (Ok(a), Ok(b)) = (
            s.selected_capability(sa, ss, now),
            r.selected_capability(ra, rs, now),
        ) {
            assert_eq!(a.reverse(), b);
            // Callback reentry cannot advance either cursor, counter or Driver.
            let claim = CallbackClaim::new(999, 1, 1, 1).unwrap();
            let gate = ss.callback_gate();
            let lease = gate.try_enter(claim).unwrap();
            let before = (ds.frames.len(), st.0, sh.0);
            assert_eq!(
                s.drive_with_capability(
                    sa,
                    ss,
                    signer,
                    &mut sc,
                    &mut st,
                    &mut sh,
                    ds,
                    &mut scratch,
                    now
                ),
                Err(Error::State)
            );
            assert_eq!((ds.frames.len(), st.0, sh.0), before);
            gate.leave(lease).unwrap();
            if fault == Fault::CapabilityLostAccept {
                assert!(saw_overlap);
            }
            assert!(ds.frames.iter().chain(&dr.frames).all(|b| b.len() <= 127));
            if fault == Fault::CapabilityExpiry {
                assert!(s.session(sa, ss, 3004).is_ok());
                assert!(r.session(ra, rs, 3004).is_ok());
                assert!(s.session(sa, ss, 3005).is_err());
                assert!(r.session(ra, rs, 3005).is_err());
                assert!(s.selected_capability(sa, ss, 3005).is_err());
                assert!(r.selected_capability(ra, rs, 3005).is_err());
            }
            if fault == Fault::CapabilityReopen {
                // Drain actual terminal holders before reopening the physical Link.
                for _ in 0..32 {
                    s.retire_with_capability(sa, ds, now).unwrap();
                    r.retire_with_capability(ra, dr, now).unwrap();
                }
                assert!(sa.tx_is_quiescent());
                sa.reopen_link(sl, 5, 127).unwrap();
                assert!(s.selected_capability(sa, ss, now).is_err());
                let phase = s.capability_phase();
                assert_eq!(s.stop(sa, ss), Err(Error::NotFound));
                assert_eq!(s.capability_phase(), phase);
                assert!(!s.quiescent(sa));
                // Product owns the new Link; the old Root cannot disable it.
                sa.try_set_rx_enabled(false).unwrap();
                r.stop(ra, rs).unwrap();
                for _ in 0..32 {
                    r.retire_with_capability(ra, dr, now).unwrap();
                }
                assert!(r.quiescent(ra));
                return;
            }
            s.stop(sa, ss).unwrap();
            r.stop(ra, rs).unwrap();
            for _ in 0..64 {
                s.retire_with_capability(sa, ds, now).unwrap();
                r.retire_with_capability(ra, dr, now).unwrap();
            }
            assert!(s.quiescent(sa) && r.quiescent(ra));
            return;
        }
    }
    panic!(
        "capability handoff failed: {:?} {:?}",
        s.capability_phase(),
        r.capability_phase()
    );
}
