//! Actual Identity publications, persistent predecessor and new physical Link pair.
#![allow(
    clippy::many_single_char_names,
    clippy::too_many_lines,
    clippy::too_many_arguments
)] // Host-only full real-proof fixture.
use super::*;
struct Signer {
    fail: bool,
    calls: usize,
    signed: usize,
}
impl Signer {
    fn sign(principal: [u8; 16], message: &[u8]) -> [u8; 32] {
        Crypto::mac(
            KeySelector {
                suite: SuiteId::OriginHmacSha256_128,
                key_id: KeyId::new(u16::from(principal[0])).unwrap(),
                key_generation: 1,
            },
            message,
            &[],
        )
        .unwrap()
    }
}
impl PeerReauthProvider for Signer {
    fn sign_reauth(&mut self, p: [u8; 16], m: &[u8], o: &mut [u8]) -> Result<usize> {
        self.calls += 1;
        self.signed += 1;
        if self.fail {
            return Err(Error::Security);
        }
        o.copy_from_slice(&Self::sign(p, m));
        Ok(32)
    }
    fn verify_reauth(&mut self, p: [u8; 16], m: &[u8], proof: &[u8]) -> Result<()> {
        self.calls += 1;
        if self.fail || proof != Self::sign(p, m) {
            return Err(Error::Security);
        }
        Ok(())
    }
}
pub(super) struct ReauthCrypto {
    pub(super) expected: HandshakeCandidate,
    pub(super) record: Option<PeerReauthRecord>,
}
#[cfg(feature = "native-routed")]
impl ReauthCrypto {
    pub(super) const fn for_candidate(expected: HandshakeCandidate) -> Self {
        Self {
            expected,
            record: None,
        }
    }
}
impl CryptoProvider for ReauthCrypto {
    fn verify_session_proof(&mut self, c: &HandshakeCandidate, proof: &[u8]) -> Result<()> {
        if *c != self.expected {
            return Err(Error::Security);
        }
        if let Some(r) = self.record {
            let mut bundle = Vec::new();
            for (stage, principal) in [(1, r.responder.principal), (2, r.initiator.principal)] {
                let mut b = vec![0; REAUTH_SIGN_DOMAIN.len() + REAUTH_RECORD_BYTES];
                b[..REAUTH_SIGN_DOMAIN.len()].copy_from_slice(REAUTH_SIGN_DOMAIN);
                PeerReauthRecord { stage, ..r }.encode(&mut b[REAUTH_SIGN_DOMAIN.len()..])?;
                bundle.extend_from_slice(&Signer::sign(principal, &b));
            }
            if proof != bundle {
                return Err(Error::Security);
            }
        } else if proof != b"initial" {
            return Err(Error::Security);
        }
        Ok(())
    }
    fn compute_origin_tag(
        &mut self,
        k: KeySelector,
        a: &[u8],
        p: &[u8],
        t: &mut [u8; 16],
    ) -> Result<()> {
        t.copy_from_slice(&Crypto::mac(k, a, p)?[..16]);
        Ok(())
    }
    fn verify_origin_tag(
        &mut self,
        k: KeySelector,
        a: &[u8],
        p: &[u8],
        t: &[u8; 16],
    ) -> Result<()> {
        if Crypto::mac(k, a, p)?[..16] == *t {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }
    fn seal_origin(&mut self, _: SealOriginRequest<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }
    fn open_origin(&mut self, _: OpenOriginRequest<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }
    fn compute_hop_tag(&mut self, k: KeySelector, a: &[u8], t: &mut [u8; 12]) -> Result<()> {
        t.copy_from_slice(&Crypto::mac(k, a, &[])?[..12]);
        Ok(())
    }
    fn verify_hop_tag(&mut self, k: KeySelector, a: &[u8], t: &[u8; 12]) -> Result<()> {
        if Crypto::mac(k, a, &[])?[..12] == *t {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }
}
fn identity_two<'a>(gate: &'a CallbackGate, domains: &'a [DomainKey]) -> IdentityOwner<'a, 2> {
    let lease = LeasePolicy {
        local_timer_max_slow_ppm: 0,
        local_timer_resolution_us: 1,
        local_timer_read_uncertainty_us: 0,
        timer_read_uncertainty_known: true,
        local_policy_max_lease_us: 100_000,
    };
    IdentityOwner::new(IdentityConfig {
        runtime_instance: 1,
        owner_instance: 5,
        persistence_business_owner_instance: 21,
        persistence_owner_instance: 3,
        realm: RealmId::new(1).unwrap(),
        address_width: AddressWidth::A1,
        provider_gate: gate,
        authority_domain: AUTH,
        binding_domains: domains,
        challenge_lifetime_us: 100_000,
        authority_lease_policy: lease,
        binding_lease_policy: lease,
    })
    .unwrap()
}
fn publish_parents(
    i: &mut IdentityOwner<'_, 2>,
    p: &mut MediaOwner<'_>,
    f: &mut Media,
) -> [BindingHandle; 2] {
    let t = authority_transcript(cookie_transcript_for_test());
    // Reuse the cryptographically checked Identity fixture, never construct BindingView.
    let h = i
        .begin_authority_challenge(Principal::new(t.authority_principal).unwrap(), 7, 3, 2)
        .unwrap();
    let fresh = AuthorityFreshness {
        verifier_principal: Principal::new(t.authority_principal).unwrap(),
        challenge_nonce: 7,
        transaction_id: 3,
        authority_lease_sequence: 1,
        max_remaining_lease_us: 8000,
        binding_lease_id: [0; 16],
        binding_generation: 0,
        proof_transcript_hash: t.freshness_proof_transcript_hash,
    };
    let mut v = Verifier {
        transcript: t,
        event: BootstrapEvent::AuthorityProof,
    };
    let (a, r) = i
        .prepare_authority(
            &mut v,
            h,
            AuthorityTransitionKind::Initial,
            epoch(&t),
            fresh,
            IdentityProof {
                bytes: SignedEvents::sign(BootstrapEvent::AuthorityProof, &t).bytes(),
            },
            ibase(p, AUTH, 1),
            3,
        )
        .unwrap();
    let ph = p.submit(&r.persistence_request(), 4).unwrap();
    i.bind_authority_persistence(a, ph).unwrap();
    drive_media(p, f, 5);
    i.activate_authority(a, ph, p.proof_get(ph).unwrap(), 5)
        .unwrap();
    p.proof_retire(ph).unwrap();
    let issue = |i: &mut IdentityOwner<'_, 2>,
                 p: &mut MediaOwner<'_>,
                 f: &mut Media,
                 index: usize| {
        let mut t = device_transcript(t);
        t.address_binding_generation = 1;
        t.binding_lease_duration_us = 3000;
        t.binding_mode = 2;
        t.joining_device_principal = if index == 0 { [0x11; 16] } else { [0x33; 16] };
        t.proposed_address = 7 + u32::try_from(index).unwrap();
        t.binding_lease_id = [u8::try_from(index + 1).unwrap(); 16];
        let h = i
            .begin_binding_challenge(Principal::new(t.joining_device_principal).unwrap(), 7, 3, 6)
            .unwrap();
        let c = BindingCertificate {
            binding: IdentityBinding {
                realm: RealmId::new(1).unwrap(),
                address_width: AddressWidth::A1,
                address: NodeAddress::new(t.proposed_address, AddressWidth::A1).unwrap(),
                generation: BindingGeneration::active(t.address_binding_generation).unwrap(),
                principal: Principal::new(t.joining_device_principal).unwrap(),
            },
            authority_principal: Principal::new(t.authority_principal).unwrap(),
            authority_generation: AddressAuthorityGeneration::new(t.authority_generation).unwrap(),
            lease_id: t.binding_lease_id,
            authority_lease_sequence: 1,
            lease_duration_us: t.binding_lease_duration_us,
            mode: AddressMode::Leased,
        };
        let fresh = AuthorityFreshness {
            verifier_principal: Principal::new(t.joining_device_principal).unwrap(),
            challenge_nonce: 7,
            transaction_id: 3,
            authority_lease_sequence: 1,
            max_remaining_lease_us: 3000,
            binding_lease_id: t.binding_lease_id,
            binding_generation: t.address_binding_generation,
            proof_transcript_hash: t.freshness_proof_transcript_hash,
        };
        let mut v = Verifier {
            transcript: t,
            event: BootstrapEvent::AddressOffer,
        };
        let (b, r) = i
            .prepare_binding(
                &mut v,
                h,
                c,
                fresh,
                IdentityProof {
                    bytes: SignedEvents::sign(BootstrapEvent::AddressOffer, &t).bytes(),
                },
                ibase(
                    p,
                    if index == 0 { BIND } else { BIND_REMOTE },
                    2 + u32::try_from(index).unwrap(),
                ),
                7,
            )
            .unwrap();
        let ph = p.submit(&r.persistence_request(), 8).unwrap();
        i.bind_binding_persistence(b, ph).unwrap();
        drive_media(p, f, 9);
        i.activate_binding(b, ph, p.proof_get(ph).unwrap(), 9)
            .unwrap();
        p.proof_retire(ph).unwrap();
        b
    };
    [issue(i, p, f, 0), issue(i, p, f, 1)]
}
pub(super) fn initial(local: Binding, peer: Binding, initiator: bool) -> HandshakeCandidate {
    let key = |id| KeySelector {
        suite: SuiteId::OriginHmacSha256_128,
        key_id: KeyId::new(id).unwrap(),
        key_generation: 1,
    };
    let mut c = HandshakeCandidate {
        local,
        peer,
        link_generation: LinkInstanceGeneration::new(if initiator { 3 } else { 9 }).unwrap(),
        session_generation: PeerSessionGeneration::new(1).unwrap(),
        policy_generation: 1,
        expires_at_us: 4000,
        origin_level: SecurityLevel::Authenticated,
        hop_profile: HopProfile::H1,
        origin_tx: key(if initiator { 1 } else { 2 }),
        origin_rx: key(if initiator { 2 } else { 1 }),
        hop_tx: None,
        hop_rx: None,
        origin_fingerprint: Fingerprint::new([5; 16]).unwrap(),
        hop_fingerprint: Fingerprint::new([6; 16]).unwrap(),
        transcript_digest: [7; 32],
    };
    c.hop_tx = Some(KeySelector {
        suite: SuiteId::HopHmacSha256_96,
        ..c.origin_tx
    });
    c.hop_rx = Some(KeySelector {
        suite: SuiteId::HopHmacSha256_96,
        ..c.origin_rx
    });
    c
}
fn initial_session(
    s: &mut SecurityOwner<'_, 2, 1>,
    p: &mut MediaOwner<'_>,
    f: &mut Media,
    c: HandshakeCandidate,
) -> SessionHandle {
    let mut crypto = ReauthCrypto {
        expected: c,
        record: None,
    };
    let facts = CurrentFacts {
        local: c.local,
        peer: c.peer,
        link_generation: c.link_generation,
        policy_generation: 1,
        now_us: 10,
    };
    let (h, r) = s
        .prepare_static_session(
            &mut crypto,
            c,
            HandshakeProof { bytes: b"initial" },
            sbase(p),
            facts,
        )
        .unwrap();
    let ph = p.submit(&r.persistence_request(), 10).unwrap();
    s.bind_persistence(h, ph).unwrap();
    drive_media(p, f, 11);
    s.activate(
        h,
        ph,
        p.proof_get(ph).unwrap(),
        CurrentFacts {
            now_us: 11,
            ..facts
        },
    )
    .unwrap();
    p.proof_retire(ph).unwrap();
    s.fence(h).unwrap();
    s.retire_fenced(h).unwrap();
    h
}
#[derive(Clone, Copy, PartialEq)]
#[cfg_attr(not(feature = "capability-control"), allow(dead_code))] // Only the optional Host fault variants lack constructors in this test configuration.
enum Fault {
    None,
    LostChallenge,
    LostAccept,
    BadSignature,
    WrongBinding,
    Backpressure,
    Unknown,
    Expiry,
    CorruptReload,
    Callback,
    NoPredecessor,
    PhysicalBudget,
    ScopeMutation,
    StopPending,
    BusyTx,
    EstablishedLifetime,
    InitiatorAhead,
    ResponderAhead,
    RepeatedAhead,
    Capability,
    CapabilityLostAccept,
    CapabilityDenied,
    CapabilityUnknown,
    CapabilityExpiry,
    CapabilityReopen,
    CapabilityRecover,
}
fn fixed_record(ci: HandshakeCandidate) -> PeerReauthRecord {
    PeerReauthRecord {
        stage: 2,
        width: AddressWidth::A1,
        realm: RealmId::new(1).unwrap(),
        initiator: ci.local,
        responder: ci.peer,
        previous_session: 1,
        next_session: 2,
        origin_suite: 1,
        origin_key: 1,
        hop_key: 1,
        origin_generation: 2,
        hop_generation: 2,
        initiator_link: (1, 4),
        responder_link: (2, 10),
        transaction: 301,
        initiator_nonce: 1001,
        responder_nonce: 2001,
        lifetime_us: 2000,
        previous_digest: [7; 32],
        initiator_keys: (1, 1),
        responder_history: ucn_security::PeerReauthHistory {
            session: 1,
            origin: 1,
            hop: 1,
            digest: [7; 32],
        },
    }
}
fn is_capability(f: Fault) -> bool {
    matches!(
        f,
        Fault::Capability
            | Fault::CapabilityLostAccept
            | Fault::CapabilityDenied
            | Fault::CapabilityUnknown
            | Fault::CapabilityExpiry
            | Fault::CapabilityReopen
            | Fault::CapabilityRecover
    )
}
#[cfg(feature = "capability-control")]
mod capability_tests;
fn data_roundtrip(
    ss: &mut SecurityOwner<'_, 2, 1>,
    rs: &mut SecurityOwner<'_, 2, 1>,
    sender: JournalPeer,
    receiver: JournalPeer,
) {
    struct Counter(u32);
    impl OriginSequenceOwner for Counter {
        fn preview(&self) -> Result<OriginSequence> {
            OriginSequence::new(self.0)
        }
        fn burn(&mut self, v: OriginSequence) -> Result<()> {
            assert_eq!(v.get(), self.0);
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
            assert_eq!(v.get(), self.0);
            self.0 += 1;
            Ok(())
        }
    }
    let mut prefix = [0; 13];
    ucn_wire::CommonHeader {
        contract: HeaderContract::C1,
        origin_security: OriginSecurity::O1,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: InteractionRole::OneWay,
        payload_kind: PayloadKind::Data,
        traffic_class: TrafficClass::Q1,
        hop_limit: HopLimit::new(1).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..5].copy_from_slice(&7u16.to_be_bytes());
    prefix[5..7].copy_from_slice(&8u16.to_be_bytes());
    prefix[7..9].copy_from_slice(&0x40u16.to_be_bytes());
    prefix[9..13].copy_from_slice(&1u32.to_be_bytes());
    let mut identity = [0; 46];
    identity[..16].copy_from_slice(&sender.facts.local.principal);
    identity[16..20].copy_from_slice(&sender.facts.local.generation.get().to_be_bytes());
    identity[20..36].copy_from_slice(&sender.facts.peer.principal);
    identity[36..40].copy_from_slice(&sender.facts.peer.generation.get().to_be_bytes());
    identity[40..].copy_from_slice(&prefix[7..]);
    let context = OriginContext::Sequenced {
        contract: HeaderContract::C1,
        fingerprint: sender.fingerprint,
        sequence: OriginSequence::new(1).unwrap(),
    };
    let access = |p: JournalPeer, direction| AccessRequest {
        session: p.session,
        context_fingerprint: p.fingerprint,
        service: ServiceId::new(0x40).unwrap(),
        protocol_opcode: 0,
        direction,
    };
    let mut crypto = ReauthCrypto {
        expected: initial(sender.facts.local, sender.facts.peer, true),
        record: None,
    };
    let mut packet = [0; 128];
    let n = ss
        .protect_packet(
            &mut crypto,
            OriginCounterOwner::Sequence(&mut Counter(1)),
            sender.session,
            sender.facts,
            access(sender, AccessDirection::Outbound),
            PacketPlan {
                prefix: &prefix,
                payload: b"reauth-data",
                canonical_identity: &identity,
                origin_context: context,
                hop_profile: HopProfile::H1,
            },
            Some(&mut Hop(1)),
            &mut SecurityPacketWorkspace::<192>::new(),
            &mut packet,
        )
        .unwrap();
    let plan = OpenPacketPlan {
        packet: &packet[..n],
        prefix_bytes: 13,
        canonical_identity: &identity,
        origin_context: context,
        hop_profile: HopProfile::H1,
    };
    let mut out = [0xcc; 16];
    let opened = rs
        .open_packet(
            &mut crypto,
            receiver.session,
            receiver.facts,
            access(receiver, AccessDirection::Inbound),
            plan,
            &mut SecurityPacketWorkspace::<192>::new(),
            &mut out,
        )
        .unwrap();
    rs.replay_commit(opened.replay, receiver.facts).unwrap();
    assert_eq!(&out[..11], b"reauth-data");
    assert_eq!(prefix.len(), 13);
}
fn final_record(ci: HandshakeCandidate, fault: Fault) -> PeerReauthRecord {
    let mut r = fixed_record(ci);
    if fault == Fault::CapabilityExpiry {
        r.lifetime_us = 4000;
    }
    let earlier = prior_record(ci).candidate(true, 1, 50, 3004).unwrap();
    if matches!(
        fault,
        Fault::InitiatorAhead | Fault::RepeatedAhead | Fault::CapabilityRecover
    ) {
        r.previous_session = 2;
        r.previous_digest = earlier.transcript_digest;
        r.initiator_keys = (2, 2);
        r.next_session = 3;
        r.origin_generation = 3;
        r.hop_generation = 3;
        if fault == Fault::RepeatedAhead {
            let mut second = r;
            second.transaction = 290;
            second.initiator_nonce = 901;
            second.responder_nonce = 1901;
            let c = second.candidate(true, 1, 50, 3004).unwrap();
            r.previous_session = 3;
            r.previous_digest = c.transcript_digest;
            r.initiator_keys = (3, 3);
            r.next_session = 4;
            r.origin_generation = 4;
            r.hop_generation = 4;
        }
    }
    if fault == Fault::ResponderAhead {
        r.responder_history = ucn_security::PeerReauthHistory {
            session: 2,
            origin: 2,
            hop: 2,
            digest: earlier.transcript_digest,
        };
        r.next_session = 3;
        r.origin_generation = 3;
        r.hop_generation = 3;
    }
    r
}
fn prior_record(ci: HandshakeCandidate) -> PeerReauthRecord {
    PeerReauthRecord {
        transaction: 201,
        initiator_nonce: 501,
        responder_nonce: 601,
        ..fixed_record(ci)
    }
}
fn seed_durable_reauth(
    s: &mut SecurityOwner<'_, 2, 1>,
    p: &mut MediaOwner<'_>,
    f: &mut Media,
    r: PeerReauthRecord,
    init: bool,
) {
    let c = r.candidate(init, 1, 50, 3004).unwrap();
    let mut crypto = ReauthCrypto {
        expected: c,
        record: Some(r),
    };
    let mut proof = Vec::new();
    for (stage, principal) in [(1, r.responder.principal), (2, r.initiator.principal)] {
        let mut b = Vec::from(REAUTH_SIGN_DOMAIN);
        let mut raw = [0; REAUTH_RECORD_BYTES];
        PeerReauthRecord { stage, ..r }.encode(&mut raw).unwrap();
        b.extend_from_slice(&raw);
        proof.extend_from_slice(&Signer::sign(principal, &b));
    }
    let facts = CurrentFacts {
        local: c.local,
        peer: c.peer,
        link_generation: c.link_generation,
        policy_generation: 1,
        now_us: 50,
    };
    let (h, req) = s
        .prepare_peer_reauth_session(
            &mut crypto,
            &mut Signer {
                fail: false,
                calls: 0,
                signed: 0,
            },
            PeerReauthSessionPlan {
                record: r,
                initiator: init,
                policy: 1,
                started: 50,
                parent_deadline: 3004,
            },
            HandshakeProof { bytes: &proof },
            sbase(p),
            facts,
        )
        .unwrap();
    let ph = p.submit(&req.persistence_request(), 50).unwrap();
    s.bind_persistence(h, ph).unwrap();
    drive_media(p, f, 50);
    s.activate(h, ph, p.proof_get(ph).unwrap(), facts).unwrap();
    p.proof_retire(ph).unwrap();
    s.fence(h).unwrap();
    s.retire_fenced(h).unwrap();
}
fn persist_root(
    root: &mut PeerReauthRuntime<'_, '_, 128>,
    a: &mut Adapter<'_, 128>,
    sec: &mut SecurityOwner<'_, 2, 1>,
    media: &mut MediaOwner<'_>,
    flash: &mut Media,
    init: bool,
    fault: Fault,
    now: u64,
) -> bool {
    if root.phase() != PeerReauthPhase::AwaitDurability {
        return false;
    }
    let record = root.record();
    let expected = record
        .candidate(
            init,
            1,
            100,
            if fault == Fault::CapabilityExpiry {
                3005
            } else {
                3004
            },
        )
        .unwrap();
    let mut crypto = ReauthCrypto {
        expected,
        record: Some(record),
    };
    if fault == Fault::Expiry {
        assert_eq!(
            root.persist(
                a,
                sec,
                &mut crypto,
                &mut Signer {
                    fail: false,
                    calls: 0,
                    signed: 0
                },
                media,
                1100
            ),
            Err(Error::Timeout)
        );
        assert!(root.session(a, sec, 1100).is_err());
        return true;
    }
    root.persist(
        a,
        sec,
        &mut crypto,
        &mut Signer {
            fail: false,
            calls: 0,
            signed: 0,
        },
        media,
        now,
    )
    .unwrap();
    assert!(!root.finish_durability(a, sec, media, now).unwrap());
    if fault == Fault::StopPending {
        root.stop(a, sec).unwrap();
        assert_eq!(root.retire_stopped_durability(media), Err(Error::State));
        drive_media(media, flash, now);
        root.retire_stopped_durability(media).unwrap();
        assert!(root.quiescent(a));
        assert!(root.session(a, sec, now).is_err());
        return true;
    }
    flash.corrupt = fault == Fault::CorruptReload;
    drive_media(media, flash, now);
    let result = root.finish_durability(a, sec, media, now);
    if fault == Fault::CorruptReload {
        assert!(result.is_err());
        assert!(root.session(a, sec, now).is_err());
        return true;
    }
    assert_eq!(result, Ok(true));
    false
}
fn recovery_negatives(
    s: &mut SecurityOwner<'_, 2, 1>,
    p: &mut MediaOwner<'_>,
    r: PeerReauthRecord,
) {
    let base = sbase(p);
    let c = r.candidate(false, 1, 100, 3004).unwrap();
    let facts = CurrentFacts {
        local: c.local,
        peer: c.peer,
        link_generation: c.link_generation,
        policy_generation: 1,
        now_us: 100,
    };
    let mut crypto = ReauthCrypto {
        expected: c,
        record: Some(r),
    };
    let mut proof = Vec::new();
    for (stage, principal) in [(1, r.responder.principal), (2, r.initiator.principal)] {
        let mut b = Vec::from(REAUTH_SIGN_DOMAIN);
        let mut raw = [0; REAUTH_RECORD_BYTES];
        PeerReauthRecord { stage, ..r }.encode(&mut raw).unwrap();
        b.extend_from_slice(&raw);
        proof.extend_from_slice(&Signer::sign(principal, &b));
    }
    assert!(matches!(
        s.prepare_static_session(
            &mut crypto,
            c,
            HandshakeProof { bytes: &proof },
            base,
            facts
        ),
        Err(Error::Replay)
    ));
    let plan = PeerReauthSessionPlan {
        record: r,
        initiator: false,
        policy: 1,
        started: 100,
        parent_deadline: 3004,
    };
    for offset in [0, 32] {
        let mut bad = proof.clone();
        bad[offset] ^= 1;
        let result = s.prepare_peer_reauth_session(
            &mut crypto,
            &mut Signer {
                fail: false,
                calls: 0,
                signed: 0,
            },
            plan,
            HandshakeProof { bytes: &bad },
            base,
            facts,
        );
        assert!(matches!(result, Err(Error::Security)));
        assert!(s.peer_is_fenced(c.local, c.peer));
    }
    let mut wrong = plan;
    wrong.record.responder_history = wrong.record.initiator_history();
    let mut signer = Signer {
        fail: false,
        calls: 0,
        signed: 0,
    };
    assert!(matches!(
        s.prepare_peer_reauth_session(
            &mut crypto,
            &mut signer,
            wrong,
            HandshakeProof { bytes: &proof },
            base,
            facts
        ),
        Err(Error::Replay)
    ));
    assert_eq!(signer.calls, 0);
    wrong = plan;
    wrong.started = 101;
    assert!(
        s.prepare_peer_reauth_session(
            &mut crypto,
            &mut signer,
            wrong,
            HandshakeProof { bytes: &proof },
            base,
            facts
        )
        .is_err()
    );
    assert_eq!(signer.calls, 0);
    assert_eq!(p.domain_get(SESSION).unwrap().record_generation, 1);
}
fn exercise(fault: Fault) {
    media(|sp, sf| {
        media(|rp, rf| {
            let gate = CallbackGate::new(30).unwrap();
            let domains = [BIND, BIND_REMOTE];
            let mut sid = identity_two(&gate, &domains);
            let sh = publish_parents(&mut sid, sp, sf);
            let mut rid = identity_two(&gate, &domains);
            let rh = publish_parents(&mut rid, rp, rf);
            let bind = |i: &IdentityOwner<'_, 2>, h| {
                let b = i.binding_get(h, 20).unwrap().certificate().binding;
                Binding {
                    address: b.address,
                    generation: b.generation,
                    principal: b.principal.bytes(),
                }
            };
            let sd = [SessionDomainRule {
                domain: SESSION,
                peer_principal: [0x33; 16],
            }];
            let rd = [SessionDomainRule {
                domain: SESSION,
                peer_principal: [0x11; 16],
            }];
            let ci = initial(bind(&sid, sh[0]), bind(&sid, sh[1]), true);
            let cr = initial(bind(&rid, rh[1]), bind(&rid, rh[0]), false);
            let fp = final_record(ci, fault)
                .candidate(true, 1, 100, 3004)
                .unwrap()
                .origin_fingerprint;
            let acl = |principal, direction| {
                [AclRule {
                    peer_principal: principal,
                    peer_binding_generation: BindingGeneration::active(1).unwrap(),
                    context_fingerprint: fp,
                    service: ServiceId::new(0x40).unwrap(),
                    protocol_opcode: 0,
                    direction,
                }]
            };
            let mut sacl = Vec::from(acl(ci.peer.principal, AccessDirection::Outbound));
            let mut racl = Vec::from(acl(cr.peer.principal, AccessDirection::Inbound));
            if is_capability(fault) {
                for (list, principal) in [
                    (&mut sacl, ci.peer.principal),
                    (&mut racl, cr.peer.principal),
                ] {
                    for op in [
                        ProtocolOpcode::NeighborHello,
                        ProtocolOpcode::NeighborKeepalive,
                        ProtocolOpcode::CapabilityQuery,
                        ProtocolOpcode::CapabilityAdvertise,
                        ProtocolOpcode::CapabilitySelect,
                        ProtocolOpcode::CapabilityAck,
                        ProtocolOpcode::CapabilityRevoke,
                        ProtocolOpcode::CapabilityFragment,
                    ] {
                        if fault == Fault::CapabilityDenied {
                            continue;
                        }
                        for direction in [AccessDirection::Inbound, AccessDirection::Outbound] {
                            list.push(AclRule {
                                peer_principal: principal,
                                peer_binding_generation: BindingGeneration::active(1).unwrap(),
                                context_fingerprint: fp,
                                service: ServiceId::new(0x40).unwrap(),
                                protocol_opcode: op.into(),
                                direction,
                            });
                        }
                    }
                }
            }
            let mut ss = security_acl(&gate, &sd, &sacl);
            let mut rs = security_acl(&gate, &rd, &racl);
            if fault != Fault::NoPredecessor {
                initial_session(&mut ss, sp, sf, ci);
                initial_session(&mut rs, rp, rf, cr);
            }
            let si = Ingress::<128>::new(101).unwrap();
            let mut sb = sbase(sp);
            let mut rb = sbase(rp);
            if matches!(
                fault,
                Fault::InitiatorAhead
                    | Fault::RepeatedAhead
                    | Fault::ResponderAhead
                    | Fault::CapabilityRecover
            ) {
                if fault == Fault::ResponderAhead {
                    seed_durable_reauth(&mut rs, rp, rf, prior_record(ci), false);
                } else {
                    seed_durable_reauth(&mut ss, sp, sf, prior_record(ci), true);
                }
                if fault == Fault::RepeatedAhead {
                    let mut r = final_record(ci, Fault::InitiatorAhead);
                    r.transaction = 290;
                    r.initiator_nonce = 901;
                    r.responder_nonce = 1901;
                    seed_durable_reauth(&mut ss, sp, sf, r, true);
                }
                recover(sf, |p, _| sb = sbase(p));
                recover(rf, |p, _| rb = sbase(p));
                // Media and Security restart; Identity parents must separately remain live.
                ss = security_acl(&gate, &sd, &sacl);
                rs = security_acl(&gate, &rd, &racl);
                if fault == Fault::InitiatorAhead {
                    recovery_negatives(&mut rs, rp, final_record(ci, fault));
                }
                assert_eq!(
                    sb.previous_session().unwrap().session_generation.get(),
                    if fault == Fault::ResponderAhead {
                        1
                    } else if fault == Fault::RepeatedAhead {
                        3
                    } else {
                        2
                    }
                );
                assert_eq!(
                    rb.previous_session().unwrap().session_generation.get(),
                    if fault == Fault::ResponderAhead { 2 } else { 1 }
                );
            }
            let ri = Ingress::<128>::new(102).unwrap();
            let mut sa = Adapter::<128>::new(101, &si).unwrap();
            let mut ra = Adapter::<128>::new(102, &ri).unwrap();
            let sl = sa.open_link(0, 4, 127).unwrap();
            let rl = ra.open_link(1, 10, 128).unwrap();
            let policy = |init| PeerReauthPolicy {
                initiator: init,
                nonce: if init { 1001 } else { 2001 },
                transaction: if init { 301 } else { 0 },
                peer_discriminator: if init { 90 } else { 31 },
                proof_bytes: 32,
                generation: 1,
                session_lifetime_us: if fault == Fault::CapabilityExpiry {
                    4000
                } else {
                    2000
                },
            };
            let mut sc = config(&gate, sl, 201);
            if fault == Fault::PhysicalBudget {
                sc.max_attempts = 4;
            }
            let rc = config(&gate, rl, 202);
            let held = if fault == Fault::BusyTx {
                Some(sa.tx_reserve(sl, 0).unwrap())
            } else {
                None
            };
            let start = PeerReauthRuntime::new(
                sc,
                policy(true),
                &sa,
                &sid,
                sh[0],
                if fault == Fault::WrongBinding {
                    sh[0]
                } else {
                    sh[1]
                },
                &ss,
                sb,
                100,
            );
            if fault == Fault::NoPredecessor
                || fault == Fault::WrongBinding
                || fault == Fault::BusyTx
            {
                assert!(start.is_err());
                if let Some(token) = held {
                    assert_eq!(sa.tx_view(token).unwrap().state, TxState::Reserved);
                }
                return;
            }
            let mut s = start.unwrap();
            let mut r =
                PeerReauthRuntime::new(rc, policy(false), &ra, &rid, rh[1], rh[0], &rs, rb, 100)
                    .unwrap();
            sa.try_set_rx_enabled(true).unwrap();
            ra.try_set_rx_enabled(true).unwrap();
            #[cfg(feature = "capability-control")]
            if is_capability(fault) {
                capability_tests::arm(&mut s, &mut r);
            }
            let mut signer = Signer {
                fail: false,
                calls: 0,
                signed: 0,
            };
            let mut ds = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut dr = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            if fault == Fault::Callback {
                let claim = CallbackClaim::new(300, 1, 1, 1).unwrap();
                let lease = gate.try_enter(claim).unwrap();
                assert_eq!(s.start(&sa, &mut signer, 100), Err(Error::State));
                assert_eq!(signer.calls, 0);
                gate.leave(lease).unwrap();
            }
            s.start(&sa, &mut signer, 100).unwrap();
            let mut dropped = false;
            let mut corrected = false;
            for now in 101..1099 {
                if (fault == Fault::Backpressure && !corrected) || fault == Fault::PhysicalBudget {
                    ds.submit = DriverSubmit::NotSubmitted;
                } else {
                    ds.submit = DriverSubmit::Complete(TerminalOutcome::Success);
                }
                if fault == Fault::Unknown {
                    ds.submit = DriverSubmit::Unknown;
                }
                let sent = s.drive(&mut sa, &mut signer, &mut ds, now);
                if fault == Fault::PhysicalBudget && sent == Err(Error::Exhausted) {
                    assert_eq!(ds.frames.len(), 4);
                    assert!(sa.tx_is_quiescent());
                    assert_eq!(sp.domain_get(SESSION).unwrap().record_generation, 1);
                    return;
                }
                if fault == Fault::Unknown {
                    assert!(sent.is_err() || sent == Ok(true));
                    assert!(!sa.tx_is_quiescent());
                    s.stop(&mut sa, &mut ss).unwrap();
                    let _ = s.retire(&mut sa, &mut ds, now);
                    assert!(!s.quiescent(&sa));
                    assert!(!sa.tx_is_quiescent());
                    return;
                }
                if sent.unwrap() {
                    if ds.last_submit == DriverSubmit::NotSubmitted {
                        corrected = true;
                    } else {
                        let mut bytes = ds.frames.last().unwrap().clone();
                        if fault == Fault::BadSignature && !corrected && bytes[31] == 2 {
                            let len = bytes.len();
                            bytes[len - 1] ^= 1;
                        }
                        publish(&ri, rl, &bytes, now, 31);
                        let result = r.receive(&mut ra, &mut signer, now);
                        if fault == Fault::BadSignature && result == Err(Error::Security) {
                            assert_eq!(r.phase(), PeerReauthPhase::AwaitHello);
                            corrected = true;
                        } else {
                            assert!(
                                result.is_ok() || result == Err(Error::State),
                                "response {result:?}"
                            );
                        }
                    }
                }
                s.retire(&mut sa, &mut ds, now).unwrap();
                if r.drive(&mut ra, &mut signer, &mut dr, now).unwrap() {
                    let packet = dr.frames.last().unwrap();
                    let op = u16::from_be_bytes([packet[27], packet[28]]);
                    if !dropped
                        && ((fault == Fault::LostChallenge && op == 0x11)
                            || (matches!(fault, Fault::LostAccept | Fault::CapabilityLostAccept)
                                && op == 0x14))
                    {
                        dropped = true;
                    } else {
                        publish(&si, sl, packet, now, 90);
                        let result = s.receive(&mut sa, &mut signer, now);
                        assert!(
                            result.is_ok() || result == Err(Error::State),
                            "initiator {result:?}"
                        );
                    }
                }
                r.retire(&mut ra, &mut dr, now).unwrap();
                if fault == Fault::ScopeMutation && s.phase() == PeerReauthPhase::AwaitDurability {
                    let original = fixed_record(ci);
                    for mutation in [
                        PeerReauthRecord {
                            responder_nonce: 2002,
                            ..original
                        },
                        PeerReauthRecord {
                            initiator_nonce: 1002,
                            ..original
                        },
                        PeerReauthRecord {
                            responder_link: (2, 11),
                            ..original
                        },
                        PeerReauthRecord {
                            previous_session: 2,
                            next_session: 3,
                            initiator_keys: (2, 2),
                            origin_generation: 3,
                            hop_generation: 3,
                            ..original
                        },
                        PeerReauthRecord {
                            origin_key: 3,
                            ..original
                        },
                        PeerReauthRecord {
                            previous_digest: [8; 32],
                            responder_history: ucn_security::PeerReauthHistory {
                                digest: [8; 32],
                                ..original.responder_history
                            },
                            ..original
                        },
                    ] {
                        let mut logical = [0; 241];
                        mutation.encode(&mut logical[..208]).unwrap();
                        logical[208] = 32;
                        let mut msg = Vec::from(REAUTH_SIGN_DOMAIN);
                        msg.extend_from_slice(&logical[..208]);
                        logical[209..]
                            .copy_from_slice(&Signer::sign(mutation.initiator.principal, &msg));
                        let callbacks = signer.calls;
                        for index in 0..4 {
                            let offset = index * 80;
                            let length = (241 - offset).min(80);
                            let mut body = vec![0; 18 + length];
                            body[0] = 1;
                            body[1] = 2;
                            body[2] = u8::try_from(index).unwrap();
                            body[3] = 4;
                            body[4..6].copy_from_slice(&241u16.to_be_bytes());
                            body[6..8]
                                .copy_from_slice(&u16::try_from(offset).unwrap().to_be_bytes());
                            body[8] = 80;
                            body[10..18].copy_from_slice(&301u64.to_be_bytes());
                            body[18..].copy_from_slice(&logical[offset..offset + length]);
                            let mut packet = [0; 128];
                            let n = ucn_wire::encode_c0_o0_h0(
                                &ucn_wire::C0Frame {
                                    common: ucn_wire::CommonHeader {
                                        contract: HeaderContract::C0,
                                        origin_security: OriginSecurity::O0,
                                        delivery: DeliveryGuarantee::BestEffort,
                                        interaction: InteractionRole::Request,
                                        payload_kind: PayloadKind::Control,
                                        traffic_class: TrafficClass::Q2,
                                        hop_limit: HopLimit::new(1).unwrap(),
                                    },
                                    realm_id: original.realm,
                                    source: ucn_wire::C0Address::Bound(original.initiator.address),
                                    destination: ucn_wire::C0Address::Bound(
                                        original.responder.address,
                                    ),
                                    source_binding_generation: original.initiator.generation,
                                    destination_binding_generation: original.responder.generation,
                                    transaction_id: C0TransactionId::new(301).unwrap(),
                                    opcode: ProtocolOpcode::PeerReauthResponse,
                                    payload: &body,
                                },
                                AddressWidth::A1,
                                HopProfile::H0,
                                &mut packet,
                            )
                            .unwrap();
                            publish(&ri, rl, &packet[..n], now, 31);
                            let result = r.receive(&mut ra, &mut signer, now);
                            if index == 3 {
                                assert_eq!(result, Err(Error::Security));
                            }
                        }
                        assert_eq!(signer.calls, callbacks);
                        assert_eq!(r.phase(), PeerReauthPhase::AwaitResponse);
                        assert_eq!(rp.domain_get(SESSION).unwrap().record_generation, 1);
                    }
                    return;
                }
                if persist_root(&mut s, &mut sa, &mut ss, sp, sf, true, fault, now)
                    || persist_root(&mut r, &mut ra, &mut rs, rp, rf, false, fault, now)
                {
                    return;
                }
                #[cfg(feature = "capability-control")]
                if is_capability(fault) && r.phase() == PeerReauthPhase::Established {
                    capability_tests::exchange(
                        &mut s,
                        &mut r,
                        &mut sa,
                        &mut ra,
                        &si,
                        &ri,
                        &mut ss,
                        &mut rs,
                        &mut signer,
                        &mut ds,
                        &mut dr,
                        fault,
                        now,
                        ci,
                        sl,
                        rl,
                    );
                    return;
                }
                if s.phase() == PeerReauthPhase::Established
                    && r.phase() == PeerReauthPhase::Established
                {
                    let sj = s.session(&sa, &ss, now).unwrap();
                    let rj = r.session(&ra, &rs, now).unwrap();
                    data_roundtrip(&mut ss, &mut rs, sj, rj);
                    assert_eq!(signer.signed, 4); // Retries never resign a transaction.
                    for frames in [&ds.frames, &dr.frames] {
                        let mut exact = std::collections::BTreeMap::new();
                        for packet in frames {
                            let key = (packet[28], packet[31]);
                            if let Some(previous) = exact.insert(key, packet) {
                                assert_eq!(previous, packet);
                            }
                        }
                    }
                    assert_eq!(
                        ss.session_get(sj.session, sj.facts)
                            .unwrap()
                            .session_generation
                            .get(),
                        final_record(ci, fault).next_session
                    );
                    assert_eq!(
                        rs.session_get(rj.session, rj.facts)
                            .unwrap()
                            .session_generation
                            .get(),
                        final_record(ci, fault).next_session
                    );
                    assert_eq!(sj.fingerprint, rj.fingerprint);
                    assert_eq!(sp.domain_get(BIND).unwrap().record_generation, 1);
                    assert_eq!(rp.domain_get(BIND_REMOTE).unwrap().record_generation, 1);
                    assert!(ds.frames.iter().chain(&dr.frames).all(|f| f.len() <= 127));
                    let retirement_now = if fault == Fault::EstablishedLifetime {
                        let mut exhausted = false;
                        for tick in now + 1..1100 {
                            let result = r.drive(&mut ra, &mut signer, &mut dr, tick);
                            r.retire(&mut ra, &mut dr, tick).unwrap();
                            if result == Err(Error::Exhausted) {
                                exhausted = true;
                                break;
                            }
                        }
                        assert!(exhausted);
                        assert_eq!(r.phase(), PeerReauthPhase::Established);
                        assert_eq!(
                            s.drive(&mut sa, &mut signer, &mut ds, 1100),
                            Err(Error::Timeout)
                        );
                        assert!(s.session(&sa, &ss, 1101).is_ok());
                        assert!(r.session(&ra, &rs, 1101).is_ok());
                        assert!(s.session(&sa, &ss, 2100).is_err());
                        assert!(r.session(&ra, &rs, 2100).is_err());
                        2100
                    } else {
                        now
                    };
                    s.stop(&mut sa, &mut ss).unwrap();
                    r.stop(&mut ra, &mut rs).unwrap();
                    for _ in 0..3 {
                        s.retire(&mut sa, &mut ds, retirement_now).unwrap();
                        r.retire(&mut ra, &mut dr, retirement_now).unwrap();
                    }
                    assert!(s.quiescent(&sa) && r.quiescent(&ra));
                    return;
                }
            }
            panic!("reauth did not converge");
        });
    });
}
#[test]
fn peer_reauth_actual_identity_persistence_and_new_link() {
    exercise(Fault::None);
}
#[test]
fn peer_reauth_lost_challenge_retries_frozen_group() {
    exercise(Fault::LostChallenge);
}
#[test]
fn peer_reauth_lost_durable_accept_retries() {
    exercise(Fault::LostAccept);
}
#[test]
fn peer_reauth_bad_signature_cannot_advance() {
    exercise(Fault::BadSignature);
}
#[test]
fn peer_reauth_exact_binding_required() {
    exercise(Fault::WrongBinding);
}
#[test]
fn peer_reauth_backpressure_keeps_original() {
    exercise(Fault::Backpressure);
}
#[test]
fn peer_reauth_unknown_keeps_physical_holder() {
    exercise(Fault::Unknown);
}
#[test]
fn peer_reauth_late_proof_never_renews_deadline() {
    exercise(Fault::Expiry);
}
#[test]
fn peer_reauth_corrupt_reload_never_publishes() {
    exercise(Fault::CorruptReload);
}
#[test]
fn peer_reauth_callback_gate_rejects() {
    exercise(Fault::Callback);
}
#[test]
fn peer_reauth_factory_cannot_bypass_join() {
    exercise(Fault::NoPredecessor);
}
#[test]
fn peer_reauth_total_physical_backpressure_budget() {
    exercise(Fault::PhysicalBudget);
}
#[test]
fn peer_reauth_valid_signatures_cannot_change_frozen_scope() {
    exercise(Fault::ScopeMutation);
}
#[test]
fn peer_reauth_stop_pending_durability_never_reactivates() {
    exercise(Fault::StopPending);
}
#[test]
fn peer_reauth_requires_old_tx_to_be_retired() {
    exercise(Fault::BusyTx);
}
#[test]
fn peer_reauth_established_result_outlives_handshake_and_retry_exhaustion() {
    exercise(Fault::EstablishedLifetime);
}
#[test]
fn peer_reauth_initiator_durable_ahead_recovers() {
    exercise(Fault::InitiatorAhead);
}
#[test]
fn peer_reauth_responder_durable_ahead_recovers() {
    exercise(Fault::ResponderAhead);
}
#[test]
fn peer_reauth_repeated_one_sided_commits_recover() {
    exercise(Fault::RepeatedAhead);
}
#[cfg(feature = "capability-control")]
#[test]
fn peer_reauth_automatic_capability_handoff() {
    exercise(Fault::Capability);
}
#[cfg(feature = "capability-control")]
#[test]
fn peer_reauth_accept_loss_after_capability_handoff() {
    exercise(Fault::CapabilityLostAccept);
}
#[cfg(feature = "capability-control")]
#[test]
fn peer_reauth_capability_never_inherits_acl() {
    exercise(Fault::CapabilityDenied);
}
#[cfg(feature = "capability-control")]
#[test]
fn peer_reauth_capability_unknown_holder_survives_stop() {
    exercise(Fault::CapabilityUnknown);
}
#[cfg(feature = "capability-control")]
#[test]
fn peer_reauth_capability_parent_expiry_rejected() {
    exercise(Fault::CapabilityExpiry);
}
#[cfg(feature = "capability-control")]
#[test]
fn peer_reauth_capability_reopened_link_is_rejected() {
    exercise(Fault::CapabilityReopen);
}
#[cfg(feature = "capability-control")]
#[test]
fn peer_reauth_recovered_pair_automatically_negotiates_capability() {
    exercise(Fault::CapabilityRecover);
}
