//! 真实 Security Session 父域与 Capability 固定缓存的跨 crate 集成回归。

use std::boxed::Box;

use ucn_capability::{
    CapabilityConfig, CapabilityRecord, CapabilitySummary, HelloDisposition, LinkCapability,
    MessageClass, NanoCapabilityOwner, PeerCapability, capability_digest,
};
use ucn_owner::CallbackGate;
use ucn_persistence::{
    ATOMIC_COMMIT_MARKER_16, BlobState, Completion, DIGEST_BLAKE2S_128, DomainBinding, DomainKey,
    DomainKind, ENVELOPE_BYTES, IoPhase, IoStart, Lifecycle, MARKER_BYTES, Manifest, ManifestEntry,
    PersistenceConfig, PersistenceOwner, PersistenceProvider, ProviderGate, ProviderGeometry,
    RequestState, WITNESS_INDEPENDENT_MONOTONIC, WitnessView,
};
use ucn_security::{
    Binding, CryptoProvider, CurrentFacts, DurabilityBase, Fingerprint, HandshakeCandidate,
    HandshakeProof, KeySelector, OpenOriginRequest, SealOriginRequest, SecurityConfig,
    SecurityLevel, SecurityOwner, SessionDomainRule,
};
use ucn_types::{
    AddressWidth, BindingGeneration, Error, HopProfile, KeyId, LinkInstanceGeneration, NodeAddress,
    PeerSessionGeneration, RealmId, Result, SuiteId,
};

const BODY: usize = 256;
const SLOT: usize = ENVELOPE_BYTES + BODY + MARKER_BYTES;
const BODY_U32: u32 = 256;
const SLOT_U32: u32 = 368;
const MARKER_U32: u32 = 16;
const DOMAIN: DomainKey = DomainKey {
    kind: DomainKind::SecurityHighWater,
    id: 1,
};

struct MemoryProvider {
    slots: [[u8; SLOT]; 2],
    witness: u64,
}

impl MemoryProvider {
    fn new() -> Self {
        Self {
            slots: [[0xFF; SLOT]; 2],
            witness: 0,
        }
    }

    fn complete(token: u64, phase: IoPhase, bytes: u32, slot: u8) -> IoStart {
        IoStart::Completed(Completion {
            io_token: token,
            result: Ok(()),
            exact_bytes: bytes,
            phase,
            blob_state: BlobState::Present,
            slot_index: slot,
        })
    }
}

impl PersistenceProvider for MemoryProvider {
    fn geometry(&self) -> ProviderGeometry {
        ProviderGeometry {
            minimum_write_alignment: 8,
            minimum_erase_alignment: 8,
            maximum_slot_bytes: u32::try_from(SLOT).expect("slot size"),
            atomic_marker_bytes: u16::try_from(MARKER_BYTES).expect("marker size"),
            erased_value: 0xFF,
        }
    }

    fn begin_load_slot(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        output: &mut [u8],
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN || usize::from(slot_index) >= 2 || output.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        output.copy_from_slice(&self.slots[usize::from(slot_index)]);
        Self::complete(token, IoPhase::LoadSlot, SLOT_U32, slot_index)
    }

    fn begin_write_inactive(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        input: &[u8],
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN || usize::from(slot_index) >= 2 || input.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        self.slots[usize::from(slot_index)].copy_from_slice(input);
        Self::complete(token, IoPhase::WriteInactive, SLOT_U32, slot_index)
    }

    fn begin_readback(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        output: &mut [u8],
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN || usize::from(slot_index) >= 2 || output.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        output.copy_from_slice(&self.slots[usize::from(slot_index)]);
        Self::complete(token, IoPhase::Readback, SLOT_U32, slot_index)
    }

    fn begin_publish_marker(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        marker: &[u8; MARKER_BYTES],
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN || usize::from(slot_index) >= 2 {
            return IoStart::Failed(Error::Argument);
        }
        self.slots[usize::from(slot_index)][SLOT - MARKER_BYTES..].copy_from_slice(marker);
        Self::complete(token, IoPhase::PublishMarker, MARKER_U32, slot_index)
    }

    fn begin_load_witness(
        &mut self,
        domain: DomainKey,
        output: &mut WitnessView,
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN {
            return IoStart::Failed(Error::Argument);
        }
        *output = WitnessView {
            domain,
            highest_maybe_published_generation: self.witness,
            state: BlobState::Present,
        };
        Self::complete(token, IoPhase::LoadWitness, 40, u8::MAX)
    }

    fn begin_advance_witness(
        &mut self,
        domain: DomainKey,
        expected_old: u64,
        exact_new: u64,
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN || self.witness != expected_old || exact_new != expected_old + 1 {
            return IoStart::Failed(Error::State);
        }
        self.witness = exact_new;
        Self::complete(token, IoPhase::AdvanceWitness, 8, u8::MAX)
    }

    fn poll(
        &mut self,
        _token: u64,
        _phase: IoPhase,
        _slot_output: Option<&mut [u8]>,
        _witness_output: Option<&mut WitnessView>,
    ) -> IoStart {
        IoStart::Failed(Error::State)
    }
}

struct Verifier;

impl CryptoProvider for Verifier {
    fn verify_session_proof(
        &mut self,
        _candidate: &HandshakeCandidate,
        proof: &[u8],
    ) -> Result<()> {
        if proof == b"proof" {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }

    fn compute_origin_tag(
        &mut self,
        _selector: KeySelector,
        _aad: &[u8],
        _plaintext: &[u8],
        _tag: &mut [u8; 16],
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn verify_origin_tag(
        &mut self,
        _selector: KeySelector,
        _aad: &[u8],
        _plaintext: &[u8],
        _tag: &[u8; 16],
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn seal_origin(&mut self, _request: SealOriginRequest<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn open_origin(&mut self, _request: OpenOriginRequest<'_>) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn compute_hop_tag(
        &mut self,
        _selector: KeySelector,
        _aad: &[u8],
        _tag: &mut [u8; 12],
    ) -> Result<()> {
        Err(Error::Unsupported)
    }

    fn verify_hop_tag(
        &mut self,
        _selector: KeySelector,
        _aad: &[u8],
        _tag: &[u8; 12],
    ) -> Result<()> {
        Err(Error::Unsupported)
    }
}

fn binding(address: u32, generation: u32, principal: u8) -> Binding {
    Binding {
        address: NodeAddress::new(address, AddressWidth::A1).expect("address"),
        generation: BindingGeneration::active(generation).expect("binding generation"),
        principal: [principal; 16],
    }
}

fn key(id: u16) -> KeySelector {
    KeySelector {
        suite: SuiteId::OriginHmacSha256_128,
        key_id: KeyId::new(id).expect("key id"),
        key_generation: 1,
    }
}

fn record(generation: u32, link_generation: u32) -> CapabilityRecord {
    CapabilityRecord {
        capability_generation: generation,
        link: LinkCapability {
            link_instance_generation: link_generation,
            carrier_mtu: 1_500,
            link_frame_mtu: 1_400,
            processing_frame_mtu: 1_300,
            carrier_header_bytes: 14,
            carrier_padding_bytes: 2,
            carrier_crc_bytes: 4,
            carrier_tag_bytes: 12,
            carrier_max_fragments: 8,
            link_flags: 0x000C,
            nominal_rate_bps: 1_000_000,
            hardware_priority_count: 4,
            timestamp_capability_bits: 0,
            timestamp_uncertainty_us: 0,
        },
        peer: PeerCapability {
            feature_bits: 0x0000_010B,
            hop_suite_bits: 2,
            e2e_suite_bits: 6,
            max_message_class: MessageClass::T512,
            max_rx_window: 16,
            max_concurrent_transfers: 2,
            realtime_mode_bits: 0,
            clock_domain_id: 0,
            clock_domain_generation: 0,
        },
    }
}

#[allow(clippy::too_many_lines)]
fn authenticated_peer(
    peer_principal: u8,
    peer_address: u32,
) -> (
    SecurityOwner<'static, 2, 1>,
    ucn_security::SessionHandle,
    CurrentFacts,
) {
    let callback_gate = Box::leak(Box::new(CallbackGate::new(41).expect("callback gate")));
    let domains = Box::leak(Box::new([SessionDomainRule {
        domain: DOMAIN,
        peer_principal: [peer_principal; 16],
    }]));
    let local = binding(0x1234, 1, 0xA1);
    let peer = binding(peer_address, 2, peer_principal);
    let candidate = HandshakeCandidate {
        local,
        peer,
        link_generation: LinkInstanceGeneration::new(3).expect("link generation"),
        session_generation: PeerSessionGeneration::new(1).expect("session generation"),
        policy_generation: 1,
        expires_at_us: 10_000,
        origin_level: SecurityLevel::Authenticated,
        hop_profile: HopProfile::H0,
        origin_tx: key(1),
        origin_rx: key(2),
        hop_tx: None,
        hop_rx: None,
        origin_fingerprint: Fingerprint::new([0x31; 16]).expect("fingerprint"),
        hop_fingerprint: Fingerprint::new([0x32; 16]).expect("fingerprint"),
        transcript_digest: [0x33; 32],
    };
    let facts = CurrentFacts {
        local,
        peer,
        link_generation: candidate.link_generation,
        policy_generation: 1,
        now_us: 10,
    };
    let mut security = SecurityOwner::<2, 1>::new(SecurityConfig {
        runtime_instance: 11,
        owner_instance: 41,
        persistence_business_owner_instance: 21,
        persistence_owner_instance: 12,
        realm: RealmId::new(1).expect("realm"),
        address_width: AddressWidth::A1,
        provider_gate: callback_gate,
        session_domains: domains,
        replay_reservation_lifetime_us: 100,
        acl: &[],
    })
    .expect("security owner");
    let provider_gate = ProviderGate::new();
    let entry = ManifestEntry {
        domain: DOMAIN,
        body_capacity_bytes: BODY_U32,
        slot_capacity_bytes: SLOT_U32,
        schema_id: ucn_security::SESSION_SCHEMA_ID,
        schema_version: ucn_security::SESSION_SCHEMA_VERSION,
        digest_suite: DIGEST_BLAKE2S_128,
        witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
        provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
    };
    let domain_binding = DomainBinding {
        domain: DOMAIN,
        business_owner_instance: 21,
        domain_generation: 31,
    };
    let mut persistence = PersistenceOwner::<1, BODY, SLOT>::uninit();
    let mut provider = MemoryProvider::new();
    persistence
        .init(
            &PersistenceConfig {
                runtime_instance: 11,
                owner_instance: 12,
                required_domain_mask: 1,
                manifest: Manifest {
                    protocol_manifest_version: 1,
                    storage_layout_version: 1,
                    composition_feature_bits: 1,
                    profile_id: 3,
                    entries: core::slice::from_ref(&entry),
                },
                bindings: core::slice::from_ref(&domain_binding),
                provider_gate: &provider_gate,
            },
            &provider,
        )
        .expect("persistence init");
    persistence.start_recovery().expect("start recovery");
    for _ in 0..32 {
        if persistence.lifecycle() == Lifecycle::Ready {
            break;
        }
        persistence.step(&mut provider, 10, 1).expect("recover");
    }
    let view = persistence.domain_get(DOMAIN).expect("domain view");
    let mut current = [0; BODY];
    let bytes = persistence
        .copy_body(DOMAIN, &mut current)
        .expect("copy body");
    let mut workspace = ucn_persistence::CodecWorkspace::new();
    let base =
        DurabilityBase::from_view(11, 21, view, &current[..bytes], 1_000, 77, &mut workspace)
            .expect("durability base");
    let mut verifier = Verifier;
    let (handle, requirement) = security
        .prepare_static_session(
            &mut verifier,
            candidate,
            HandshakeProof { bytes: b"proof" },
            base,
            facts,
        )
        .expect("prepare session");
    let persistence_handle = persistence
        .submit(&requirement.persistence_request(), 20)
        .expect("submit persistence");
    security
        .bind_persistence(handle, persistence_handle)
        .expect("bind persistence");
    for _ in 0..64 {
        if persistence
            .request_get(persistence_handle)
            .expect("request")
            .state
            == RequestState::ProofReady
        {
            break;
        }
        persistence.step(&mut provider, 30, 1).expect("commit");
    }
    let proof = persistence.proof_get(persistence_handle).expect("proof");
    security
        .activate(handle, persistence_handle, proof, facts)
        .expect("activate");
    (security, handle, facts)
}

#[test]
fn capability_cache_requires_live_authenticated_session_and_exact_generation() {
    let (security, handle, facts) = authenticated_peer(0xB2, 0x5678);
    let authenticated = security
        .authenticated_peer_view(handle, facts, 7)
        .expect("authenticated view");
    let local = record(1, 3);
    let mut owner = NanoCapabilityOwner::new(CapabilityConfig {
        runtime_instance: 11,
        security_owner_instance: 41,
        realm: RealmId::new(1).expect("realm"),
        local_record: local,
        capability_lease_us: 100,
        discovery_lease_us: 80,
    })
    .expect("capability owner");
    let peer = record(1, 3);
    let digest = capability_digest(peer).expect("digest");
    let summary = CapabilitySummary {
        capability_generation: 1,
        link_instance_generation: 3,
        digest,
    };
    let wrong_provenance = NanoCapabilityOwner::new(CapabilityConfig {
        runtime_instance: 11,
        security_owner_instance: 42,
        realm: RealmId::new(1).expect("realm"),
        local_record: local,
        capability_lease_us: 100,
        discovery_lease_us: 80,
    })
    .expect("wrong provenance owner");
    assert_eq!(
        wrong_provenance.ingest_summary(authenticated, summary, 20),
        Err(Error::Security)
    );
    let invalid_summary = CapabilitySummary {
        capability_generation: 0,
        link_instance_generation: 3,
        digest,
    };
    assert_eq!(
        owner.ingest_summary(authenticated, invalid_summary, 20),
        Err(Error::Argument)
    );
    assert_eq!(
        owner.ingest_summary(authenticated, summary, 20),
        Ok(HelloDisposition::QueryRequired)
    );
    let reference = owner
        .ingest_advertise(authenticated, peer, 20)
        .expect("advertise");
    let moved_parent = security
        .authenticated_peer_view(handle, facts, 8)
        .expect("moved parent view");
    assert_eq!(
        owner.ingest_advertise(moved_parent, peer, 21),
        Err(Error::Replay)
    );
    assert_eq!(
        owner
            .peer_get(reference, 21)
            .expect("original parent")
            .peer_ref,
        reference
    );
    assert_eq!(
        owner.ingest_summary(authenticated, summary, 30),
        Ok(HelloDisposition::Matched)
    );
    assert_eq!(owner.counts(30), (1, 1));

    let before = owner.peer_get(reference, 40).expect("cached peer");
    assert_eq!(
        owner.ingest_advertise(authenticated, peer, 70),
        Ok(reference)
    );
    assert_eq!(owner.peer_get(reference, 79), Ok(before));
    assert_eq!(owner.peer_get(reference, 100), Err(Error::Timeout));
    assert_eq!(owner.counts(100), (1, 0));

    let mut wrong_link = record(2, 4);
    wrong_link.capability_generation = 2;
    assert_eq!(
        owner.ingest_advertise(authenticated, wrong_link, 40),
        Err(Error::Security)
    );
    assert_eq!(owner.counts(40), (1, 1));
    owner
        .invalidate_session(reference)
        .expect("invalidate parent session");
    assert_eq!(owner.counts(40), (0, 0));
}

#[test]
fn expired_capability_slots_require_explicit_parent_retirement_before_reuse() {
    let (security_a, handle_a, facts_a) = authenticated_peer(0xB2, 0x5678);
    let (security_b, handle_b, facts_b) = authenticated_peer(0xB3, 0x5679);
    let (security_c, handle_c, facts_c) = authenticated_peer(0xB4, 0x567A);
    let authenticated_a = security_a
        .authenticated_peer_view(handle_a, facts_a, 7)
        .expect("peer A");
    let authenticated_b = security_b
        .authenticated_peer_view(handle_b, facts_b, 7)
        .expect("peer B");
    let authenticated_c = security_c
        .authenticated_peer_view(handle_c, facts_c, 7)
        .expect("peer C");
    let local = record(1, 3);
    let peer = record(1, 3);
    let mut owner = NanoCapabilityOwner::new(CapabilityConfig {
        runtime_instance: 11,
        security_owner_instance: 41,
        realm: RealmId::new(1).expect("realm"),
        local_record: local,
        capability_lease_us: 100,
        discovery_lease_us: 80,
    })
    .expect("capability owner");

    let reference_a = owner
        .ingest_advertise(authenticated_a, peer, 20)
        .expect("cache peer A");
    let reference_b = owner
        .ingest_advertise(authenticated_b, peer, 20)
        .expect("cache peer B");
    assert_eq!(owner.counts(20), (2, 2));
    assert_eq!(owner.counts(120), (2, 0));
    assert_eq!(
        owner.ingest_advertise(authenticated_c, peer, 121),
        Err(Error::NoSpace)
    );
    assert_eq!(owner.counts(121), (2, 0));
    assert_eq!(owner.peer_get(reference_a, 121), Err(Error::Timeout));
    assert_eq!(owner.peer_get(reference_b, 121), Err(Error::Timeout));

    owner
        .invalidate_session(reference_a)
        .expect("retire exact parent A");
    let reference_c = owner
        .ingest_advertise(authenticated_c, peer, 122)
        .expect("reuse explicitly retired slot");
    assert_eq!(owner.counts(122), (2, 1));
    assert_eq!(owner.peer_get(reference_b, 122), Err(Error::Timeout));
    assert_eq!(
        owner.peer_get(reference_c, 122).map(|value| value.peer_ref),
        Ok(reference_c)
    );
}
