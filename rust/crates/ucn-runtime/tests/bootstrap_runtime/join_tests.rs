//! Actual Cookie -> five events -> independent Foundation reloads -> two Sessions.
use super::*;
use ucn_identity::*;
use ucn_persistence::*;
use ucn_security::{self as security, *};
#[path = "../../../../tests/support/flash.rs"]
#[allow(dead_code)]
mod flash;
const AUTH: DomainKey = DomainKey {
    kind: DomainKind::IdentityBinding,
    id: 1,
};
const BIND: DomainKey = DomainKey {
    kind: DomainKind::IdentityBinding,
    id: 2,
};
const SESSION: DomainKey = DomainKey {
    kind: DomainKind::SecurityHighWater,
    id: 1,
};
const BIND_REMOTE: DomainKey = DomainKey {
    kind: DomainKind::IdentityBinding,
    id: 3,
};
#[cfg(feature = "peer-reauth")]
mod reauth_tests;
#[cfg(all(feature = "route-runtime", feature = "peer-reauth"))]
mod route_tests;
type MediaOwner<'a> = PersistenceOwner<'a, 4, { flash::BODY }, { flash::SLOT }>;
type Ingress<const B: usize = 144> = AdapterEventIngress<2, 1, 1, B>;
type Adapter<'a, const B: usize = 144> = AdapterOwner<'a, 2, 1, 1, B>;
struct Media {
    slots: [flash::Flash; 4],
    pending: usize,
    corrupt: bool,
}
impl Media {
    fn index(d: DomainKey) -> usize {
        [AUTH, BIND, SESSION, BIND_REMOTE]
            .iter()
            .position(|v| *v == d)
            .unwrap()
    }
}
impl PersistenceProvider for Media {
    fn geometry(&self) -> ProviderGeometry {
        self.slots[0].geometry()
    }
    fn begin_load_slot(&mut self, d: DomainKey, s: u8, out: &mut [u8], t: u64) -> IoStart {
        self.pending = Self::index(d);
        self.slots[self.pending].begin_load_slot(d, s, out, t)
    }
    fn begin_write_inactive(&mut self, d: DomainKey, s: u8, input: &[u8], t: u64) -> IoStart {
        self.pending = Self::index(d);
        self.slots[self.pending].begin_write_inactive(d, s, input, t)
    }
    fn begin_readback(&mut self, d: DomainKey, s: u8, out: &mut [u8], t: u64) -> IoStart {
        self.pending = Self::index(d);
        let result = self.slots[self.pending].begin_readback(d, s, out, t);
        if self.corrupt {
            out[100] ^= 1;
        }
        result
    }
    fn begin_publish_marker(
        &mut self,
        d: DomainKey,
        s: u8,
        marker: &[u8; MARKER_BYTES],
        t: u64,
    ) -> IoStart {
        self.pending = Self::index(d);
        self.slots[self.pending].begin_publish_marker(d, s, marker, t)
    }
    fn begin_load_witness(&mut self, d: DomainKey, out: &mut WitnessView, t: u64) -> IoStart {
        self.pending = Self::index(d);
        self.slots[self.pending].begin_load_witness(d, out, t)
    }
    fn begin_advance_witness(&mut self, d: DomainKey, old: u64, new: u64, t: u64) -> IoStart {
        self.pending = Self::index(d);
        self.slots[self.pending].begin_advance_witness(d, old, new, t)
    }
    fn poll(
        &mut self,
        t: u64,
        phase: IoPhase,
        out: Option<&mut [u8]>,
        w: Option<&mut WitnessView>,
    ) -> IoStart {
        self.slots[self.pending].poll(t, phase, out, w)
    }
}
fn media(callback: impl FnOnce(&mut MediaOwner<'_>, &mut Media)) {
    let mut f = Media {
        slots: [AUTH, BIND, SESSION, BIND_REMOTE].map(flash::Flash::new),
        pending: 0,
        corrupt: false,
    };
    recover(&mut f, callback);
}
fn recover(f: &mut Media, callback: impl FnOnce(&mut MediaOwner<'_>, &mut Media)) {
    let gate = ProviderGate::new();
    let entries = [
        (AUTH, AUTHORITY_SCHEMA_ID),
        (BIND, BINDING_SCHEMA_ID),
        (BIND_REMOTE, BINDING_SCHEMA_ID),
        (SESSION, SESSION_SCHEMA_ID),
    ]
    .map(|(domain, schema)| ManifestEntry {
        domain,
        body_capacity_bytes: 1024,
        slot_capacity_bytes: 1136,
        schema_id: schema,
        schema_version: 1,
        digest_suite: DIGEST_BLAKE2S_128,
        witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
        provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
    });
    let bindings = [AUTH, BIND, BIND_REMOTE, SESSION].map(|domain| DomainBinding {
        domain,
        business_owner_instance: if domain == SESSION { 22 } else { 21 },
        domain_generation: 1,
    });
    let mut p = MediaOwner::uninit();
    p.init(
        &PersistenceConfig {
            runtime_instance: 1,
            owner_instance: 3,
            required_domain_mask: 15,
            manifest: Manifest {
                protocol_manifest_version: 1,
                storage_layout_version: 1,
                composition_feature_bits: 0,
                profile_id: 1,
                entries: &entries,
            },
            bindings: &bindings,
            provider_gate: &gate,
        },
        f,
    )
    .unwrap();
    p.start_recovery().unwrap();
    for _ in 0..128 {
        p.step(f, 1, 1).unwrap();
        if p.lifecycle() == Lifecycle::Ready {
            break;
        }
    }
    assert_eq!(p.lifecycle(), Lifecycle::Ready);
    callback(&mut p, f);
}
fn identity<'a>(gate: &'a CallbackGate, domains: &'a [DomainKey]) -> IdentityOwner<'a, 1> {
    let policy = LeasePolicy {
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
        authority_lease_policy: policy,
        binding_lease_policy: policy,
    })
    .unwrap()
}
fn ibase(p: &MediaOwner<'_>, d: DomainKey, n: u32) -> IdentityDurabilityBase {
    let mut out = [0; flash::BODY];
    let len = p.copy_body(d, &mut out).unwrap();
    IdentityDurabilityBase::from_view(
        1,
        21,
        p.domain_get(d).unwrap(),
        &out[..len],
        100_000,
        n,
        &mut CodecWorkspace::new(),
    )
    .unwrap()
}
fn sbase(p: &MediaOwner<'_>) -> DurabilityBase {
    let mut out = [0; flash::BODY];
    let len = p.copy_body(SESSION, &mut out).unwrap();
    DurabilityBase::from_view(
        1,
        22,
        p.domain_get(SESSION).unwrap(),
        &out[..len],
        100_000,
        3,
        &mut CodecWorkspace::new(),
    )
    .unwrap()
}
struct Verifier {
    transcript: BootstrapTranscript,
    event: BootstrapEvent,
}
impl IdentityVerifier for Verifier {
    fn verify_authority_transition(&mut self, t: &AuthorityTransition, proof: &[u8]) -> Result<()> {
        let v = self.transcript;
        if proof != SignedEvents::sign(self.event, &v).bytes()
            || t.proposed.authority_principal.bytes() != v.authority_principal
            || t.freshness.challenge_nonce != v.lease_freshness_challenge_nonce
            || t.freshness.transaction_id != v.transaction_id
            || t.challenge_started_local_us > 21
            || t.derived_local_deadline_us > 8021
        {
            return Err(Error::Security);
        }
        Ok(())
    }
    fn verify_binding_issue(
        &mut self,
        a: &AuthorityEpoch,
        b: &BindingCertificate,
        f: &AuthorityFreshness,
        start: u64,
        end: u64,
        proof: &[u8],
    ) -> Result<()> {
        let t = self.transcript;
        if proof != SignedEvents::sign(self.event, &t).bytes()
            || a.authority_principal.bytes() != t.authority_principal
            || b.binding.address.get() != t.proposed_address
            || b.binding.generation.get() != t.address_binding_generation
            || b.binding.principal.bytes() != t.joining_device_principal
            || b.lease_id != t.binding_lease_id
            || f.challenge_nonce != t.lease_freshness_challenge_nonce
            || f.transaction_id != t.transaction_id
            || start > 21
            || end > 5021
        {
            return Err(Error::Security);
        }
        Ok(())
    }
}
fn epoch(t: &BootstrapTranscript) -> AuthorityEpoch {
    AuthorityEpoch {
        realm: RealmId::new(1).unwrap(),
        authority_principal: Principal::new(t.authority_principal).unwrap(),
        authority_generation: AddressAuthorityGeneration::new(t.authority_generation).unwrap(),
        durable_fence_token: t.durable_fence_token,
        lease_sequence: t.authority_lease_sequence,
        lease_duration_us: t.authority_lease_duration_us,
        allocation_high_water_digest: t.allocation_high_water_digest,
        quorum_config_digest: t.quorum_config_digest,
        signer_set_digest: t.signer_set_digest,
        threshold_proof_digest: t.threshold_proof_digest,
        signer_count: t.authority_signer_count,
        quorum_threshold: t.authority_quorum_threshold,
    }
}
fn drive_media(p: &mut MediaOwner<'_>, f: &mut Media, now: u64) {
    for _ in 0..64 {
        if p.step(f, now, 1).is_err() {
            break;
        }
    }
}
#[allow(clippy::many_single_char_names)] // Short local handles in the Host publication fixture only.
fn install_authority(
    i: &mut IdentityOwner<'_, 1>,
    p: &mut MediaOwner<'_>,
    f: &mut Media,
    t: &BootstrapTranscript,
) {
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
        transcript: *t,
        event: BootstrapEvent::AuthorityProof,
    };
    let (a, r) = i
        .prepare_authority(
            &mut v,
            h,
            AuthorityTransitionKind::Initial,
            epoch(t),
            fresh,
            IdentityProof {
                bytes: SignedEvents::sign(BootstrapEvent::AuthorityProof, t).bytes(),
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
}
struct Crypto {
    transcript: BootstrapTranscript,
    device: bool,
    expected_deadline: u64,
    calls: usize,
    fail_hop: bool,
    fail_fragment_origin_once: bool,
}
impl Crypto {
    fn mac(key: KeySelector, aad: &[u8], payload: &[u8]) -> Result<[u8; 32]> {
        if aad.len() + payload.len() > 960 {
            return Err(Error::NoSpace);
        }
        let k = u8::try_from(key.key_id.get()).map_err(|_| Error::Security)?;
        let mut bytes = [0; 1024];
        bytes[..64].fill(k ^ 0x36);
        bytes[64..64 + aad.len()].copy_from_slice(aad);
        bytes[64 + aad.len()..64 + aad.len() + payload.len()].copy_from_slice(payload);
        let inner = sha256(
            &bytes[..64 + aad.len() + payload.len()],
            &mut DigestWorkspace::new(),
        );
        bytes[..64].fill(k ^ 0x5c);
        bytes[64..96].copy_from_slice(&inner);
        Ok(sha256(&bytes[..96], &mut DigestWorkspace::new()))
    }
}
impl CryptoProvider for Crypto {
    fn verify_session_proof(&mut self, c: &HandshakeCandidate, proof: &[u8]) -> Result<()> {
        self.calls += 1;
        let t = self.transcript;
        let mut canonical = [0; 403];
        canonical[..20].copy_from_slice(b"UCN6-JOIN-SESSION-V1");
        canonical[20..24].copy_from_slice(&379_u32.to_be_bytes());
        encode_transcript(&t, BootstrapPhase::FinalDurable, &mut canonical[24..])?;
        let digest = sha256(&canonical, &mut DigestWorkspace::new());
        let (local, peer, tx, rx, link) = if self.device {
            (7, 1, 1, 2, 3)
        } else {
            (1, 7, 2, 1, 9)
        };
        if proof != SignedEvents::sign(BootstrapEvent::FinalDurable, &t).bytes()
            || c.transcript_digest != digest
            || c.origin_fingerprint.bytes() != digest[..16]
            || c.hop_fingerprint.bytes() != digest[16..]
            || c.local.address.get() != local
            || c.peer.address.get() != peer
            || c.origin_tx.key_id.get() != tx
            || c.origin_rx.key_id.get() != rx
            || c.hop_tx.unwrap().key_id.get() != tx
            || c.hop_rx.unwrap().key_id.get() != rx
            || c.link_generation.get() != link
            || c.session_generation.get() != 1
            || c.policy_generation != 1
            || c.expires_at_us != self.expected_deadline
        {
            return Err(Error::Security);
        }
        Ok(())
    }
    fn compute_origin_tag(
        &mut self,
        key: KeySelector,
        aad: &[u8],
        payload: &[u8],
        tag: &mut [u8; 16],
    ) -> Result<()> {
        if self.fail_fragment_origin_once
            && payload.len() >= 6
            && payload[0..2] == [1, 3]
            && payload[4..6] == [1, 2]
        {
            self.fail_fragment_origin_once = false;
            return Err(Error::Security);
        }
        tag.copy_from_slice(&Self::mac(key, aad, payload)?[..16]);
        Ok(())
    }
    fn verify_origin_tag(
        &mut self,
        key: KeySelector,
        aad: &[u8],
        payload: &[u8],
        tag: &[u8; 16],
    ) -> Result<()> {
        if Self::mac(key, aad, payload)?[..16] == *tag {
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
    fn compute_hop_tag(&mut self, key: KeySelector, aad: &[u8], tag: &mut [u8; 12]) -> Result<()> {
        if self.fail_hop {
            return Err(Error::Security);
        }
        tag.copy_from_slice(&Self::mac(key, aad, &[])?[..12]);
        Ok(())
    }
    fn verify_hop_tag(&mut self, key: KeySelector, aad: &[u8], tag: &[u8; 12]) -> Result<()> {
        if Self::mac(key, aad, &[])?[..12] == *tag {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }
}
fn security<'a>(
    gate: &'a CallbackGate,
    domains: &'a [SessionDomainRule],
) -> SecurityOwner<'a, 2, 1> {
    security_acl(gate, domains, &[])
}
fn security_acl<'a>(
    gate: &'a CallbackGate,
    domains: &'a [SessionDomainRule],
    acl: &'a [AclRule],
) -> SecurityOwner<'a, 2, 1> {
    SecurityOwner::new(SecurityConfig {
        runtime_instance: 1,
        owner_instance: 8,
        persistence_business_owner_instance: 22,
        persistence_owner_instance: 3,
        realm: RealmId::new(1).unwrap(),
        address_width: AddressWidth::A1,
        provider_gate: gate,
        session_domains: domains,
        replay_reservation_lifetime_us: 1000,
        acl,
    })
    .unwrap()
}
#[allow(clippy::too_many_arguments)]
fn deliver<const B: usize>(
    from: &mut BootstrapJoin<'_, '_, B>,
    a: &mut Adapter<'_, B>,
    d: &mut Driver,
    to: &mut BootstrapJoin<'_, '_, B>,
    other: &mut Adapter<'_, B>,
    ingress: &Ingress<B>,
    link: LinkHandle,
    disc: u32,
    provider: &mut SignedEvents,
    now: &mut u64,
    target: BootstrapPhase,
    drop_one: bool,
) {
    let mut dropped = false;
    while to.phase() != target {
        assert!(*now < 900, "handshake did not converge");
        if from.drive(a, d, *now).unwrap() {
            let packet = d.frames.last().unwrap();
            if drop_one && !dropped {
                dropped = true;
            } else {
                publish(ingress, link, packet, *now, disc);
                let result = to.receive(other, provider, *now);
                assert!(
                    result.is_ok() || drop_one && result == Err(Error::State),
                    "receive {:?} at {} from {:?} to {:?} target {:?}",
                    result,
                    *now,
                    from.phase(),
                    to.phase(),
                    target
                );
            }
        }
        from.retire(a, d, *now).unwrap();
        *now += 1;
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Failure {
    None,
    OwnerGate,
    SessionGate,
    LateFreshness,
    Authority,
    Reservation,
    Binding,
    Session,
    Reopen,
    StopPending,
    DelayedFinal,
    FinalUnknown,
    Reboot,
    #[cfg(feature = "capability-control")]
    Capability,
    #[cfg(feature = "capability-control")]
    CapabilitySmall,
    #[cfg(feature = "capability-control")]
    CapabilityDenied,
    #[cfg(feature = "capability-control")]
    Capability127,
    #[cfg(feature = "capability-control")]
    Capability128,
    #[cfg(feature = "capability-control")]
    FragmentDenied,
    #[cfg(feature = "capability-control")]
    FragmentUnknown,
    #[cfg(feature = "capability-control")]
    FragmentOriginalDenied,
    #[cfg(feature = "capability-control")]
    FragmentTimeout,
    #[cfg(feature = "capability-control")]
    FragmentSealFault,
    #[cfg(feature = "capability-control")]
    CapabilityMixed,
}
#[allow(clippy::too_many_lines)]
fn exercise_join(failure: Failure) {
    exercise_join_buffer::<144>(failure);
}
#[allow(clippy::too_many_lines)]
fn exercise_join_buffer<const B: usize>(failure: Failure) {
    media(|sp, sf| {
        media(|ap, af| {
            let gate = CallbackGate::new(1).unwrap();
            let other_gate = CallbackGate::new(2).unwrap();
            let domains = [BIND];
            let mut sidentity = identity(
                if failure == Failure::OwnerGate {
                    &other_gate
                } else {
                    &gate
                },
                &domains,
            );
            let mut aidentity = identity(&gate, &domains);
            let sd = [SessionDomainRule {
                domain: SESSION,
                peer_principal: [0x41; 16],
            }];
            let ad = [SessionDomainRule {
                domain: SESSION,
                peer_principal: [0x11; 16],
            }];
            let si = Ingress::<B>::new(1).unwrap();
            let ai = Ingress::<B>::new(2).unwrap();
            let mut sa = Adapter::<B>::new(1, &si).unwrap();
            let mut aa = Adapter::<B>::new(2, &ai).unwrap();
            #[cfg(feature = "capability-control")]
            let mtu = match failure {
                Failure::Capability | Failure::CapabilityDenied => 136,
                Failure::Capability127 | Failure::FragmentUnknown | Failure::CapabilityMixed => 127,
                _ => 128,
            };
            #[cfg(not(feature = "capability-control"))]
            let mtu = 128;
            let sl = sa.open_link(0, 3, mtu).unwrap();
            #[cfg(feature = "capability-control")]
            let authority_mtu = if failure == Failure::CapabilityMixed {
                136
            } else {
                mtu
            };
            #[cfg(not(feature = "capability-control"))]
            let authority_mtu = mtu;
            let al = aa.open_link(1, 9, authority_mtu).unwrap();
            sa.try_set_rx_enabled(true).unwrap();
            aa.try_set_rx_enabled(true).unwrap();
            let s_fresh = JoinFreshness::begin(
                &mut sidentity,
                Principal::new([0x41; 16]).unwrap(),
                Principal::new([0x11; 16]).unwrap(),
                7,
                3,
                if failure == Failure::LateFreshness {
                    22
                } else {
                    21
                },
            )
            .unwrap();
            let mut client_config = config(&gate, sl, 10);
            if failure == Failure::DelayedFinal {
                client_config.lifetime_us = 20_000;
            }
            let mut client = BootstrapClient::new(client_config, &sa, hello(), 7, 90, 21).unwrap();
            let mut listener =
                BootstrapListener::new(config(&gate, al, 20), admission(&gate), &aa, 1_000_000, 21)
                    .unwrap();
            let mut provider = SignedEvents(Cookies::new());
            let mut sdriver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            let mut adriver = Driver::new(DriverSubmit::Complete(TerminalOutcome::Success));
            client.start(&sa, 21).unwrap();
            client.drive(&mut sa, &mut sdriver, 21).unwrap();
            publish(&ai, al, &sdriver.frames[0], 21, 31);
            listener.receive(&mut aa, &mut provider, 21).unwrap();
            listener.drive(&mut aa, &mut adriver, 22).unwrap();
            publish(&si, sl, &adriver.frames[0], 22, 90);
            client.receive(&mut sa, 22).unwrap();
            client.retire(&mut sa, &mut sdriver, 22).unwrap();
            client.drive(&mut sa, &mut sdriver, 23).unwrap();
            publish(&ai, al, &sdriver.frames[1], 23, 31);
            listener.receive(&mut aa, &mut provider, 23).unwrap();
            client.retire(&mut sa, &mut sdriver, 24).unwrap();
            listener.retire(&mut aa, &mut adriver, 24).unwrap();
            let c = client.auth_exchange(&sa, [0x41; 16], 25).unwrap();
            let a = listener.auth_exchange(&aa, [0x41; 16], 25).unwrap();
            let authority = authority_transcript(c.transcript());
            install_authority(&mut aidentity, ap, af, &authority);
            let a_fresh = JoinFreshness::begin(
                &mut aidentity,
                Principal::new([0x41; 16]).unwrap(),
                Principal::new([0x11; 16]).unwrap(),
                7,
                3,
                21,
            )
            .unwrap();
            if failure == Failure::LateFreshness {
                assert!(matches!(
                    BootstrapJoin::new(c, s_fresh),
                    Err(Error::Security)
                ));
                return;
            }
            if failure == Failure::OwnerGate {
                assert!(matches!(
                    BootstrapJoin::new(c, s_fresh),
                    Err(Error::Security)
                ));
                return;
            }
            let mut c = BootstrapJoin::new(c, s_fresh).unwrap();
            let mut a = BootstrapJoin::new(a, a_fresh).unwrap();
            #[cfg(feature = "capability-control")]
            let (sacl, aacl) = capability_join::acl(&c.transcript(), failure);
            #[cfg(not(feature = "capability-control"))]
            let (sacl, aacl) = ([], []);
            let mut ssecurity = security_acl(&gate, &sd, &sacl);
            let mut asecurity = security_acl(&gate, &ad, &aacl);
            let mut n = 26;
            a.queue_auth(
                &aa,
                &mut provider,
                BootstrapEvent::AuthorityProof,
                authority,
                SignedEvents::sign(BootstrapEvent::AuthorityProof, &authority),
                n,
            )
            .unwrap();
            deliver(
                &mut a,
                &mut aa,
                &mut adriver,
                &mut c,
                &mut sa,
                &si,
                sl,
                90,
                &mut provider,
                &mut n,
                BootstrapPhase::AuthorityVerified,
                false,
            );
            let mut verifier = Verifier {
                transcript: authority,
                event: BootstrapEvent::AuthorityProof,
            };
            c.persist_authority(
                &sa,
                &mut sidentity,
                &mut verifier,
                sp,
                ibase(sp, AUTH, 1),
                AuthorityTransitionKind::Initial,
                SignedEvents::sign(BootstrapEvent::AuthorityProof, &authority),
                n,
            )
            .unwrap();
            assert!(
                !c.finish_durability(&sa, &mut sidentity, &mut ssecurity, sp, n)
                    .unwrap()
            );
            sf.slots[0].pending_once = failure != Failure::Authority;
            sf.corrupt = failure == Failure::Authority;
            drive_media(sp, sf, n);
            if failure == Failure::Authority {
                assert!(
                    c.finish_durability(&sa, &mut sidentity, &mut ssecurity, sp, n)
                        .is_err()
                );
                assert!(sidentity.authority_get(n).is_err());
                assert_eq!(sf.slots[0].witness, 0);
                return;
            }
            c.finish_durability(&sa, &mut sidentity, &mut ssecurity, sp, n)
                .unwrap();
            let mut device = device_transcript(c.transcript());
            device.selected_hop_suite = 16;
            c.queue_auth(
                &sa,
                &mut provider,
                BootstrapEvent::DeviceProof,
                device,
                SignedEvents::sign(BootstrapEvent::DeviceProof, &device),
                n,
            )
            .unwrap();
            deliver(
                &mut c,
                &mut sa,
                &mut sdriver,
                &mut a,
                &mut aa,
                &ai,
                al,
                31,
                &mut provider,
                &mut n,
                BootstrapPhase::DeviceVerified,
                false,
            );
            let mut offer = device;
            offer.proposed_address = 7;
            offer.address_binding_generation = 1;
            offer.binding_lease_id = [0x61; 16];
            offer.binding_lease_duration_us = 5000;
            offer.binding_mode = 2;
            let op = SignedEvents::sign(BootstrapEvent::AddressOffer, &offer);
            assert_eq!(
                a.publish(&aa, &aidentity, &mut provider, op, n),
                Err(Error::State)
            );
            verifier.transcript = offer;
            verifier.event = BootstrapEvent::AddressOffer;
            a.persist_binding(
                &aa,
                &mut aidentity,
                &mut verifier,
                ap,
                ibase(ap, BIND, 2),
                offer,
                op,
                n,
            )
            .unwrap();
            assert_eq!(
                a.publish(&aa, &aidentity, &mut provider, op, n),
                Err(Error::State)
            );
            af.slots[1].pending_once = failure != Failure::Reservation;
            af.corrupt = failure == Failure::Reservation;
            drive_media(ap, af, n);
            if failure == Failure::Reservation {
                assert!(
                    a.finish_durability(&aa, &mut aidentity, &mut asecurity, ap, n)
                        .is_err()
                );
                assert_eq!(
                    a.publish(&aa, &aidentity, &mut provider, op, n),
                    Err(Error::State)
                );
                assert_eq!(af.slots[1].witness, 0);
                return;
            }
            a.finish_durability(&aa, &mut aidentity, &mut asecurity, ap, n)
                .unwrap();
            a.publish(&aa, &aidentity, &mut provider, op, n).unwrap();
            deliver(
                &mut a,
                &mut aa,
                &mut adriver,
                &mut c,
                &mut sa,
                &si,
                sl,
                90,
                &mut provider,
                &mut n,
                BootstrapPhase::AddressOffered,
                true,
            );
            let cp = SignedEvents::sign(BootstrapEvent::DeviceCommit, &offer);
            assert_eq!(
                c.publish(&sa, &sidentity, &mut provider, cp, n),
                Err(Error::State)
            );
            c.persist_binding(
                &sa,
                &mut sidentity,
                &mut verifier,
                sp,
                ibase(sp, BIND, 2),
                offer,
                op,
                n,
            )
            .unwrap();
            sf.slots[1].pending_once = failure != Failure::Binding;
            sf.corrupt = failure == Failure::Binding;
            drive_media(sp, sf, n);
            if failure == Failure::Binding {
                assert!(
                    c.finish_durability(&sa, &mut sidentity, &mut ssecurity, sp, n)
                        .is_err()
                );
                assert_eq!(
                    c.publish(&sa, &sidentity, &mut provider, cp, n),
                    Err(Error::State)
                );
                assert_eq!(sf.slots[1].witness, 0);
                assert_eq!(af.slots[1].witness, 1);
                return;
            }
            c.finish_durability(&sa, &mut sidentity, &mut ssecurity, sp, n)
                .unwrap();
            c.publish(&sa, &sidentity, &mut provider, cp, n).unwrap();
            deliver(
                &mut c,
                &mut sa,
                &mut sdriver,
                &mut a,
                &mut aa,
                &ai,
                al,
                31,
                &mut provider,
                &mut n,
                BootstrapPhase::DeviceCommitted,
                true,
            );
            let fp = SignedEvents::sign(BootstrapEvent::FinalDurable, &offer);
            assert_eq!(
                a.publish_final(
                    &aa,
                    &aidentity,
                    &mut provider,
                    Evidence::new(&[0xCC; 32]).unwrap(),
                    n
                ),
                Err(Error::Security)
            );
            assert_eq!(a.phase(), BootstrapPhase::DeviceCommitted);
            a.publish_final(&aa, &aidentity, &mut provider, fp, n)
                .unwrap();
            assert!(c.session(&sa, &sidentity, &ssecurity, n).is_err());
            if failure == Failure::FinalUnknown {
                adriver.submit = DriverSubmit::Unknown;
                assert_eq!(a.drive(&mut aa, &mut adriver, n), Err(Error::InDoubt));
                let token = *adriver.tokens.last().unwrap();
                assert_eq!(aa.tx_view(token).unwrap().state, TxState::InDoubt);
                a.stop(&mut aa, &mut asecurity).unwrap();
                adriver.cancel = DriverCancel::Unknown;
                assert!(a.retire(&mut aa, &mut adriver, n).is_err());
                assert!(aa.tx_view(token).is_ok());
                ai.tx_complete(token, TerminalOutcome::Success).unwrap();
                a.retire(&mut aa, &mut adriver, n).unwrap();
                assert_eq!(aa.tx_view(token), Err(Error::NotFound));
                assert_eq!(af.slots[2].witness, 0);
                assert!(a.session(&aa, &aidentity, &asecurity, n).is_err());
                return;
            }
            deliver(
                &mut a,
                &mut aa,
                &mut adriver,
                &mut c,
                &mut sa,
                &si,
                sl,
                90,
                &mut provider,
                &mut n,
                BootstrapPhase::FinalDurable,
                true,
            );
            let mut sc = Crypto {
                transcript: offer,
                device: true,
                expected_deadline: 5020,
                calls: 0,
                fail_hop: false,
                fail_fragment_origin_once: false,
            };
            let mut ac = Crypto {
                transcript: offer,
                device: false,
                expected_deadline: 5020,
                calls: 0,
                fail_hop: false,
                fail_fragment_origin_once: false,
            };
            let policy = JoinSessionPolicy {
                generation: 1,
                lifetime_us: 10_000,
            };
            if failure == Failure::SessionGate {
                let mut wrong_security = security(&other_gate, &sd);
                assert_eq!(
                    c.persist_session(
                        &sa,
                        &sidentity,
                        &mut wrong_security,
                        &mut sc,
                        sp,
                        sbase(sp),
                        policy,
                        fp,
                        n
                    ),
                    Err(Error::State)
                );
                assert_eq!(sc.calls, 0);
                assert_eq!(sf.slots[2].witness, 0);
                return;
            }
            assert_eq!(
                c.persist_session(
                    &sa,
                    &sidentity,
                    &mut ssecurity,
                    &mut sc,
                    sp,
                    sbase(sp),
                    policy,
                    Evidence::new(&[0xCC; 32]).unwrap(),
                    n
                ),
                Err(Error::State)
            );
            assert_eq!(sc.calls, 0);
            if failure == Failure::DelayedFinal {
                assert_eq!(
                    c.persist_session(
                        &sa,
                        &sidentity,
                        &mut ssecurity,
                        &mut sc,
                        sp,
                        sbase(sp),
                        policy,
                        fp,
                        5020
                    ),
                    Err(Error::Access)
                );
                assert_eq!(sc.calls, 0);
                assert_eq!(sf.slots[2].witness, 0);
                return;
            }
            let sh = c
                .persist_session(
                    &sa,
                    &sidentity,
                    &mut ssecurity,
                    &mut sc,
                    sp,
                    sbase(sp),
                    policy,
                    fp,
                    n,
                )
                .unwrap();
            let _ah = a
                .persist_session(
                    &aa,
                    &aidentity,
                    &mut asecurity,
                    &mut ac,
                    ap,
                    sbase(ap),
                    policy,
                    fp,
                    n,
                )
                .unwrap();
            assert!(c.session(&sa, &sidentity, &ssecurity, n).is_err());
            assert!(a.session(&aa, &aidentity, &asecurity, n).is_err());
            if failure == Failure::StopPending {
                c.stop(&mut sa, &mut ssecurity).unwrap();
                assert_eq!(c.retire_stopped_durability(sp), Err(Error::State));
                drive_media(sp, sf, n);
                c.retire_stopped_durability(sp).unwrap();
                assert!(c.session(&sa, &sidentity, &ssecurity, n).is_err());
                assert_eq!(sf.slots[2].witness, 1);
                return;
            }
            sf.slots[2].pending_once = failure != Failure::Session;
            sf.corrupt = failure == Failure::Session;
            drive_media(sp, sf, n);
            if failure == Failure::Session {
                assert!(
                    c.finish_durability(&sa, &mut sidentity, &mut ssecurity, sp, n)
                        .is_err()
                );
                assert!(c.session(&sa, &sidentity, &ssecurity, n).is_err());
                assert_eq!(sf.slots[2].witness, 0);
                return;
            }
            if failure == Failure::Reopen {
                sa.reopen_link(sl, 4, 128).unwrap();
                assert!(
                    c.finish_durability(&sa, &mut sidentity, &mut ssecurity, sp, n)
                        .is_err()
                );
                assert!(c.session(&sa, &sidentity, &ssecurity, n).is_err());
                return;
            }
            c.finish_durability(&sa, &mut sidentity, &mut ssecurity, sp, n)
                .unwrap();
            assert_eq!(
                c.session(&sa, &sidentity, &ssecurity, n).unwrap().phase,
                security::SessionPhase::Active
            );
            assert!(a.session(&aa, &aidentity, &asecurity, n).is_err());
            drive_media(ap, af, n);
            a.finish_durability(&aa, &mut aidentity, &mut asecurity, ap, n)
                .unwrap();
            assert_eq!(
                a.session(&aa, &aidentity, &asecurity, n).unwrap().phase,
                security::SessionPhase::Active
            );
            assert_eq!((sc.calls, ac.calls), (1, 1));
            assert!(sf.slots[..3].iter().all(|s| s.witness == 1));
            assert!(af.slots[..3].iter().all(|s| s.witness == 1));
            assert_eq!(sf.slots[3].witness, 0); // Reauth's second Binding is unused by JOIN.
            assert_eq!(af.slots[3].witness, 0);
            #[cfg(feature = "capability-control")]
            if matches!(
                failure,
                Failure::Capability
                    | Failure::CapabilitySmall
                    | Failure::CapabilityDenied
                    | Failure::Capability127
                    | Failure::Capability128
                    | Failure::FragmentDenied
                    | Failure::FragmentUnknown
                    | Failure::FragmentOriginalDenied
                    | Failure::FragmentTimeout
                    | Failure::FragmentSealFault
                    | Failure::CapabilityMixed
            ) {
                capability_join::exercise(
                    &mut c,
                    &mut a,
                    &sidentity,
                    &aidentity,
                    &mut ssecurity,
                    &mut asecurity,
                    &mut sc,
                    &mut ac,
                    &mut sa,
                    &mut aa,
                    &si,
                    &ai,
                    &mut sdriver,
                    &mut adriver,
                    failure,
                    n,
                );
                return;
            }
            assert!(c.session(&sa, &sidentity, &ssecurity, 5019).is_ok());
            assert!(c.session(&sa, &sidentity, &ssecurity, 5020).is_err());
            if failure == Failure::Reboot {
                recover(sf, |recovered, _| {
                    assert_eq!(recovered.domain_get(SESSION).unwrap().record_generation, 1);
                    let fresh_identity = identity(&gate, &domains);
                    let mut fresh_security = security(&gate, &sd);
                    assert!(c.session(&sa, &fresh_identity, &fresh_security, n).is_err());
                    let candidate =
                        join_session_candidate(offer, AddressWidth::A1, true, 3, policy, 21, 5020)
                            .unwrap();
                    let facts = CurrentFacts {
                        local: candidate.local,
                        peer: candidate.peer,
                        link_generation: candidate.link_generation,
                        policy_generation: 1,
                        now_us: n,
                    };
                    assert_eq!(fresh_security.session_get(sh, facts), Err(Error::NotFound));
                    assert!(matches!(
                        fresh_security.prepare_static_session(
                            &mut sc,
                            candidate,
                            HandshakeProof { bytes: fp.bytes() },
                            sbase(recovered),
                            facts
                        ),
                        Err(Error::Replay)
                    ));
                });
            }
            c.stop(&mut sa, &mut ssecurity).unwrap();
            a.stop(&mut aa, &mut asecurity).unwrap();
            assert!(c.session(&sa, &sidentity, &ssecurity, n).is_err());
        });
    });
}
#[cfg(feature = "capability-control")]
#[path = "capability_join.rs"]
mod capability_join;
#[test]
fn real_offer_commit_final_reload_and_independent_sessions() {
    exercise_join(Failure::None);
}
#[test]
fn late_freshness_cannot_rebase_the_pre_hello_lease() {
    exercise_join(Failure::LateFreshness);
}
#[test]
fn corrupt_authority_reload_never_publishes_epoch() {
    exercise_join(Failure::Authority);
}
#[test]
fn corrupt_reservation_reload_never_publishes_offer() {
    exercise_join(Failure::Reservation);
}
#[test]
fn corrupt_device_reload_never_publishes_commit() {
    exercise_join(Failure::Binding);
}
#[test]
fn corrupt_session_reload_never_activates_session() {
    exercise_join(Failure::Session);
}
#[test]
fn reopened_link_rejects_a_delayed_reload_proof() {
    exercise_join(Failure::Reopen);
}
#[test]
fn stop_pending_durability_drains_without_reactivation() {
    exercise_join(Failure::StopPending);
}
#[test]
fn delayed_final_never_renews_local_lease() {
    exercise_join(Failure::DelayedFinal);
}
#[test]
fn final_unknown_completion_keeps_its_physical_holder() {
    exercise_join(Failure::FinalUnknown);
}
#[test]
fn reboot_retains_records_not_authority_or_active_session() {
    exercise_join(Failure::Reboot);
}
#[test]
fn foreign_identity_gate_rejects_before_exchange_is_enabled() {
    exercise_join(Failure::OwnerGate);
}
#[test]
fn foreign_security_gate_cannot_publish_a_join_session() {
    exercise_join(Failure::SessionGate);
}
#[test]
fn session_plan_rejects_key_pair_exhaustion_profile_mismatch_and_deadline_wrap() {
    let mut t = device_transcript(authority_transcript(cookie_transcript_for_test()));
    t.selected_hop_suite = 16;
    t.proposed_address = 7;
    t.address_binding_generation = 1;
    t.binding_lease_id = [0x61; 16];
    t.binding_lease_duration_us = 5000;
    t.binding_mode = 2;
    let policy = JoinSessionPolicy {
        generation: 1,
        lifetime_us: 1000,
    };
    for id in [65534, 65535] {
        let mut bad = t;
        bad.selected_e2e_key_id = id;
        assert!(join_session_candidate(bad, AddressWidth::A1, true, 3, policy, 21, 5020).is_err());
        bad = t;
        bad.selected_hop_key_id = id;
        assert!(join_session_candidate(bad, AddressWidth::A1, true, 3, policy, 21, 5020).is_err());
    }
    let mut bad = t;
    bad.selected_e2e_suite = 2;
    assert!(join_session_candidate(bad, AddressWidth::A1, true, 3, policy, 21, 5020).is_err());
    assert!(
        join_session_candidate(t, AddressWidth::A1, true, 3, policy, u64::MAX - 1, u64::MAX)
            .is_err()
    );
    assert!(join_session_candidate(t, AddressWidth::A1, true, 3, policy, 21, 21).is_err());
}
fn cookie_transcript_for_test() -> BootstrapTranscript {
    // The native C/Python oracle independently checks the exact canonical bytes.
    BootstrapTranscript {
        protocol_version: 6,
        bootstrap_header_contract: 1,
        flow: BootstrapFlow::Join,
        joining_device_principal: [0; 16],
        joining_device_identity_digest: Principal::new([1; 16]).unwrap(),
        authority_principal: [0; 16],
        authority_generation: 0,
        device_nonce: 2,
        authority_nonce: 0,
        transaction_id: 3,
        lease_freshness_challenge_nonce: 7,
        realm_id: 0,
        proposed_address: 0,
        address_binding_generation: 0,
        authority_address: 0,
        authority_binding_generation: 0,
        selected_link_instance_id: 2,
        binding_lease_id: [0; 16],
        binding_lease_duration_us: 0,
        authority_lease_sequence: 0,
        authority_lease_duration_us: 0,
        freshness_max_remaining_lease_us: 0,
        durable_fence_token: [0; 16],
        allocation_high_water_digest: [0; 16],
        quorum_config_digest: [0; 32],
        signer_set_digest: [0; 32],
        threshold_proof_digest: [0; 32],
        freshness_proof_transcript_hash: [0; 32],
        authority_signer_count: 0,
        authority_quorum_threshold: 0,
        binding_mode: 0,
        selected_hop_suite: 0,
        selected_hop_key_id: 0,
        selected_hop_key_generation: 0,
        selected_e2e_mode: 0,
        selected_e2e_suite: 0,
        selected_e2e_key_id: 0,
        selected_e2e_key_generation: 0,
        selected_session_generation: 0,
        selected_link_instance_generation: 9,
        prior_messages_hash: [0x31; 32],
    }
}
