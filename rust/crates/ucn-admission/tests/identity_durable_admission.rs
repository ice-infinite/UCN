//! Identity 持久化发布与 Admission 最终准入的端到端 Host 回归。

use ucn_admission::{
    AdmissionConfig, AdmissionKey, BootstrapEvent, BootstrapFlow, BootstrapPhase,
    BootstrapTranscript, COOKIE_CHALLENGE_BYTES, COOKIE_MAX_BYTES, CookieProvider, Evidence,
    HELLO_BYTES, Hello, HelloCookie, InitialHelloContext, LinkIdentity, NanoAdmissionOwner,
};
use ucn_identity::{
    AUTHORITY_SCHEMA_ID, AUTHORITY_SCHEMA_VERSION, AddressMode, AuthorityEpoch, AuthorityFreshness,
    AuthorityTransition, AuthorityTransitionKind, BINDING_SCHEMA_ID, BINDING_SCHEMA_VERSION,
    BindingCertificate, IdentityBinding, IdentityConfig, IdentityDurabilityBase, IdentityOwner,
    IdentityProof, IdentityVerifier, LeasePolicy, Principal,
};
use ucn_owner::CallbackGate;
use ucn_persistence::{
    ATOMIC_COMMIT_MARKER_16, BlobState, Completion, DIGEST_BLAKE2S_128, DomainBinding, DomainKey,
    DomainKind, ENVELOPE_BYTES, IoPhase, IoStart, Lifecycle, MARKER_BYTES, Manifest, ManifestEntry,
    PersistenceConfig, PersistenceOwner, PersistenceProvider, ProviderGate, ProviderGeometry,
    RequestState, WITNESS_INDEPENDENT_MONOTONIC, WitnessView,
};
use ucn_types::{
    AddressAuthorityGeneration, AddressWidth, BindingGeneration, Error, NodeAddress,
    PROTOCOL_MAJOR, RealmId, Result,
};

const BODY: usize = 256;
const BODY_U32: u32 = 256;
const SLOT: usize = ENVELOPE_BYTES + BODY + MARKER_BYTES;
const SLOT_U32: u32 = 368;
const AUTHORITY_DOMAIN: DomainKey = DomainKey {
    kind: DomainKind::IdentityBinding,
    id: 1,
};
const BINDING_DOMAIN: DomainKey = DomainKey {
    kind: DomainKind::IdentityBinding,
    id: 2,
};
const POLICY: LeasePolicy = LeasePolicy {
    local_timer_max_slow_ppm: 0,
    local_timer_resolution_us: 1,
    local_timer_read_uncertainty_us: 0,
    timer_read_uncertainty_known: true,
    local_policy_max_lease_us: 100_000,
};

struct MemoryProvider {
    slots: [[[u8; SLOT]; 2]; 2],
    witnesses: [u64; 2],
}

impl MemoryProvider {
    fn new() -> Self {
        Self {
            slots: [[[0xFF; SLOT]; 2]; 2],
            witnesses: [0; 2],
        }
    }

    fn domain_index(domain: DomainKey) -> Option<usize> {
        match domain {
            AUTHORITY_DOMAIN => Some(0),
            BINDING_DOMAIN => Some(1),
            _ => None,
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
            maximum_slot_bytes: SLOT_U32,
            atomic_marker_bytes: 16,
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
        let Some(domain_index) = Self::domain_index(domain) else {
            return IoStart::Failed(Error::Argument);
        };
        if usize::from(slot_index) >= 2 || output.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        output.copy_from_slice(&self.slots[domain_index][usize::from(slot_index)]);
        Self::complete(token, IoPhase::LoadSlot, SLOT_U32, slot_index)
    }

    fn begin_write_inactive(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        input: &[u8],
        token: u64,
    ) -> IoStart {
        let Some(domain_index) = Self::domain_index(domain) else {
            return IoStart::Failed(Error::Argument);
        };
        if usize::from(slot_index) >= 2 || input.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        self.slots[domain_index][usize::from(slot_index)].copy_from_slice(input);
        Self::complete(token, IoPhase::WriteInactive, SLOT_U32, slot_index)
    }

    fn begin_readback(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        output: &mut [u8],
        token: u64,
    ) -> IoStart {
        self.begin_load_slot(domain, slot_index, output, token)
            .map_phase(IoPhase::Readback)
    }

    fn begin_publish_marker(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        marker: &[u8; MARKER_BYTES],
        token: u64,
    ) -> IoStart {
        let Some(domain_index) = Self::domain_index(domain) else {
            return IoStart::Failed(Error::Argument);
        };
        if usize::from(slot_index) >= 2 {
            return IoStart::Failed(Error::Argument);
        }
        self.slots[domain_index][usize::from(slot_index)][SLOT - MARKER_BYTES..]
            .copy_from_slice(marker);
        Self::complete(token, IoPhase::PublishMarker, 16, slot_index)
    }

    fn begin_load_witness(
        &mut self,
        domain: DomainKey,
        output: &mut WitnessView,
        token: u64,
    ) -> IoStart {
        let Some(index) = Self::domain_index(domain) else {
            return IoStart::Failed(Error::Argument);
        };
        *output = WitnessView {
            domain,
            highest_maybe_published_generation: self.witnesses[index],
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
        let Some(index) = Self::domain_index(domain) else {
            return IoStart::Failed(Error::Argument);
        };
        if self.witnesses[index] != expected_old || exact_new != expected_old + 1 {
            return IoStart::Failed(Error::State);
        }
        self.witnesses[index] = exact_new;
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

trait IoStartPhase {
    fn map_phase(self, phase: IoPhase) -> Self;
}

impl IoStartPhase for IoStart {
    fn map_phase(self, phase: IoPhase) -> Self {
        match self {
            IoStart::Completed(mut completion) => {
                completion.phase = phase;
                IoStart::Completed(completion)
            }
            other => other,
        }
    }
}

struct Oracle;

impl IdentityVerifier for Oracle {
    fn verify_authority_transition(
        &mut self,
        _transition: &AuthorityTransition,
        proof: &[u8],
    ) -> Result<()> {
        (proof == b"proof").then_some(()).ok_or(Error::Security)
    }

    fn verify_binding_issue(
        &mut self,
        _authority: &AuthorityEpoch,
        _certificate: &BindingCertificate,
        _freshness: &AuthorityFreshness,
        _challenge_started_local_us: u64,
        _local_deadline_us: u64,
        proof: &[u8],
    ) -> Result<()> {
        (proof == b"proof").then_some(()).ok_or(Error::Security)
    }
}

impl CookieProvider for Oracle {
    fn issue_cookie(
        &mut self,
        _hello: &Hello,
        _link: LinkIdentity,
        _cookie_time_bucket: u32,
        output: &mut [u8; COOKIE_MAX_BYTES],
    ) -> Result<u8> {
        output[..4].copy_from_slice(&[1, 2, 3, 4]);
        Ok(4)
    }

    fn verify_cookie(&mut self, hello_cookie: &HelloCookie, _key: &AdmissionKey) -> Result<()> {
        if hello_cookie.cookie_evidence.bytes() == [1, 2, 3, 4] {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }

    fn authorize_event(
        &mut self,
        _event: BootstrapEvent,
        _key: &AdmissionKey,
        _transcript: &BootstrapTranscript,
        _now_us: u64,
        evidence: &Evidence,
    ) -> Result<()> {
        if evidence.bytes() == [0xA5] {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }
}

fn principal(value: u8) -> Principal {
    Principal::new([value; 16]).expect("principal")
}

fn authority() -> AuthorityEpoch {
    AuthorityEpoch {
        realm: RealmId::new(1).expect("realm"),
        authority_principal: principal(0x41),
        authority_generation: AddressAuthorityGeneration::new(1).expect("generation"),
        durable_fence_token: [0x42; 16],
        lease_sequence: 1,
        lease_duration_us: 50_000,
        allocation_high_water_digest: [0x43; 16],
        quorum_config_digest: [0x44; 32],
        signer_set_digest: [0x45; 32],
        threshold_proof_digest: [0x46; 32],
        signer_count: 3,
        quorum_threshold: 2,
    }
}

fn transcript(phase: BootstrapPhase) -> BootstrapTranscript {
    let mut value = BootstrapTranscript {
        protocol_version: PROTOCOL_MAJOR,
        bootstrap_header_contract: 1,
        flow: BootstrapFlow::Join,
        joining_device_principal: [0; 16],
        joining_device_identity_digest: principal(0x11),
        authority_principal: [0; 16],
        authority_generation: 0,
        device_nonce: 2,
        authority_nonce: 0,
        transaction_id: 3,
        lease_freshness_challenge_nonce: 4,
        realm_id: 0,
        proposed_address: 0,
        address_binding_generation: 0,
        authority_address: 0,
        authority_binding_generation: 0,
        selected_link_instance_id: 7,
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
    };
    if phase >= BootstrapPhase::AuthorityVerified {
        value.authority_principal = [0x41; 16];
        value.authority_generation = 1;
        value.authority_nonce = 5;
        value.realm_id = 1;
        value.authority_address = 1;
        value.authority_binding_generation = 1;
        value.authority_lease_sequence = 1;
        value.authority_lease_duration_us = 50_000;
        value.freshness_max_remaining_lease_us = 40_000;
        value.durable_fence_token = [0x42; 16];
        value.allocation_high_water_digest = [0x43; 16];
        value.quorum_config_digest = [0x44; 32];
        value.signer_set_digest = [0x45; 32];
        value.threshold_proof_digest = [0x46; 32];
        value.freshness_proof_transcript_hash = [0x47; 32];
        value.authority_signer_count = 3;
        value.authority_quorum_threshold = 2;
    }
    if phase >= BootstrapPhase::DeviceVerified {
        value.joining_device_principal = [0x51; 16];
        value.selected_hop_suite = 1;
        value.selected_hop_key_id = 2;
        value.selected_hop_key_generation = 3;
        value.selected_e2e_mode = 1;
        value.selected_e2e_suite = 2;
        value.selected_e2e_key_id = 4;
        value.selected_e2e_key_generation = 5;
        value.selected_session_generation = 6;
    }
    if phase >= BootstrapPhase::AddressOffered {
        value.proposed_address = 7;
        value.address_binding_generation = 1;
        value.binding_lease_id = [0x61; 16];
        value.binding_lease_duration_us = 30_000;
        value.binding_mode = 2;
    }
    value
}

fn base(
    persistence: &PersistenceOwner<2, BODY, SLOT>,
    domain: DomainKey,
    continuation: u32,
) -> IdentityDurabilityBase {
    let view = persistence.domain_get(domain).expect("domain view");
    let mut body = [0; BODY];
    let bytes = persistence.copy_body(domain, &mut body).expect("copy body");
    let mut workspace = ucn_persistence::CodecWorkspace::new();
    IdentityDurabilityBase::from_view(
        11,
        21,
        view,
        &body[..bytes],
        100_000,
        continuation,
        &mut workspace,
    )
    .expect("durability base")
}

fn drive_proof(
    persistence: &mut PersistenceOwner<2, BODY, SLOT>,
    provider: &mut MemoryProvider,
    handle: ucn_persistence::PersistenceHandle,
) -> ucn_persistence::PersistenceProof {
    for _ in 0..64 {
        if persistence.request_get(handle).expect("request").state == RequestState::ProofReady {
            return persistence.proof_get(handle).expect("proof");
        }
        persistence.step(provider, 1_000, 1).expect("step");
    }
    panic!("proof did not become ready")
}

#[test]
#[allow(clippy::too_many_lines)]
fn final_admission_requires_identity_reload_proof() {
    let callback_gate = CallbackGate::new(1).expect("callback gate");
    let provider_gate = ProviderGate::new();
    let entries = [
        ManifestEntry {
            domain: AUTHORITY_DOMAIN,
            body_capacity_bytes: BODY_U32,
            slot_capacity_bytes: SLOT_U32,
            schema_id: AUTHORITY_SCHEMA_ID,
            schema_version: AUTHORITY_SCHEMA_VERSION,
            digest_suite: DIGEST_BLAKE2S_128,
            witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
            provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
        },
        ManifestEntry {
            domain: BINDING_DOMAIN,
            body_capacity_bytes: BODY_U32,
            slot_capacity_bytes: SLOT_U32,
            schema_id: BINDING_SCHEMA_ID,
            schema_version: BINDING_SCHEMA_VERSION,
            digest_suite: DIGEST_BLAKE2S_128,
            witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
            provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
        },
    ];
    let bindings = [
        DomainBinding {
            domain: AUTHORITY_DOMAIN,
            business_owner_instance: 21,
            domain_generation: 31,
        },
        DomainBinding {
            domain: BINDING_DOMAIN,
            business_owner_instance: 21,
            domain_generation: 32,
        },
    ];
    let mut persistence = PersistenceOwner::<2, BODY, SLOT>::uninit();
    let mut storage = MemoryProvider::new();
    persistence
        .init(
            &PersistenceConfig {
                runtime_instance: 11,
                owner_instance: 12,
                required_domain_mask: 3,
                manifest: Manifest {
                    protocol_manifest_version: 1,
                    storage_layout_version: 1,
                    composition_feature_bits: 1,
                    profile_id: 1,
                    entries: &entries,
                },
                bindings: &bindings,
                provider_gate: &provider_gate,
            },
            &storage,
        )
        .expect("persistence init");
    persistence.start_recovery().expect("start recovery");
    for _ in 0..64 {
        if persistence.lifecycle() == Lifecycle::Ready {
            break;
        }
        persistence.step(&mut storage, 10, 1).expect("recover");
    }
    assert_eq!(persistence.lifecycle(), Lifecycle::Ready);

    let binding_domains = [BINDING_DOMAIN];
    let mut identity = IdentityOwner::<1>::new(IdentityConfig {
        runtime_instance: 11,
        owner_instance: 41,
        persistence_business_owner_instance: 21,
        persistence_owner_instance: 12,
        realm: RealmId::new(1).expect("realm"),
        address_width: AddressWidth::A1,
        provider_gate: &callback_gate,
        authority_domain: AUTHORITY_DOMAIN,
        binding_domains: &binding_domains,
        challenge_lifetime_us: 10_000,
        authority_lease_policy: POLICY,
        binding_lease_policy: POLICY,
    })
    .expect("identity owner");
    let mut oracle = Oracle;
    let authority = authority();
    let challenge = identity
        .begin_authority_challenge(authority.authority_principal, 10, 11, 100)
        .expect("authority challenge");
    let authority_freshness = AuthorityFreshness {
        verifier_principal: authority.authority_principal,
        challenge_nonce: 10,
        transaction_id: 11,
        authority_lease_sequence: 1,
        max_remaining_lease_us: 40_000,
        binding_lease_id: [0; 16],
        binding_generation: 0,
        proof_transcript_hash: [0x47; 32],
    };
    let (authority_handle, requirement) = identity
        .prepare_authority(
            &mut oracle,
            challenge,
            AuthorityTransitionKind::Initial,
            authority,
            authority_freshness,
            IdentityProof { bytes: b"proof" },
            base(&persistence, AUTHORITY_DOMAIN, 1),
            110,
        )
        .expect("prepare authority");
    let persistence_handle = persistence
        .submit(&requirement.persistence_request(), 120)
        .expect("submit authority");
    identity
        .bind_authority_persistence(authority_handle, persistence_handle)
        .expect("bind authority persistence");
    let proof = drive_proof(&mut persistence, &mut storage, persistence_handle);
    identity
        .activate_authority(authority_handle, persistence_handle, proof, 150)
        .expect("activate authority");
    persistence
        .proof_retire(persistence_handle)
        .expect("retire authority proof");

    let device = principal(0x51);
    let binding_challenge = identity
        .begin_binding_challenge(device, 20, 21, 200)
        .expect("binding challenge");
    let certificate = BindingCertificate {
        binding: IdentityBinding {
            realm: RealmId::new(1).expect("realm"),
            address_width: AddressWidth::A1,
            address: NodeAddress::new(7, AddressWidth::A1).expect("address"),
            generation: BindingGeneration::active(1).expect("binding generation"),
            principal: device,
        },
        authority_principal: authority.authority_principal,
        authority_generation: authority.authority_generation,
        lease_id: [0x61; 16],
        lease_duration_us: 30_000,
        authority_lease_sequence: 1,
        mode: AddressMode::Leased,
    };
    let binding_freshness = AuthorityFreshness {
        verifier_principal: device,
        challenge_nonce: 20,
        transaction_id: 21,
        authority_lease_sequence: 1,
        max_remaining_lease_us: 20_000,
        binding_lease_id: certificate.lease_id,
        binding_generation: 1,
        proof_transcript_hash: [0x62; 32],
    };
    let (binding_handle, requirement) = identity
        .prepare_binding(
            &mut oracle,
            binding_challenge,
            certificate,
            binding_freshness,
            IdentityProof { bytes: b"proof" },
            base(&persistence, BINDING_DOMAIN, 2),
            210,
        )
        .expect("prepare binding");
    let persistence_handle = persistence
        .submit(&requirement.persistence_request(), 220)
        .expect("submit binding");
    identity
        .bind_binding_persistence(binding_handle, persistence_handle)
        .expect("bind binding persistence");
    let proof = drive_proof(&mut persistence, &mut storage, persistence_handle);
    let binding_view = identity
        .activate_binding(binding_handle, persistence_handle, proof, 250)
        .expect("activate binding");

    let mut admission = NanoAdmissionOwner::new(AdmissionConfig {
        runtime_instance: 11,
        owner_instance: 51,
        identity_owner_instance: 41,
        provider_gate: &callback_gate,
        max_pending_per_link: 1,
        token_burst: 1,
        tokens_per_second: 1,
        pending_timeout_us: 10_000,
    })
    .expect("admission owner");
    let link = LinkIdentity {
        link_id: 7,
        link_generation: 9,
    };
    let hello = Hello {
        flow: BootstrapFlow::Join,
        identity_digest: principal(0x11),
        device_nonce: 2,
        transaction_id: 3,
    };
    admission
        .issue_cookie(
            &mut oracle,
            hello,
            InitialHelloContext {
                link,
                now_us: 300,
                request_bytes: HELLO_BYTES,
                response_bytes: COOKIE_CHALLENGE_BYTES,
                cookie_time_bucket: 1,
            },
        )
        .expect("cookie");
    let key = AdmissionKey {
        link,
        local_peer_discriminator: 1,
        identity_digest: hello.identity_digest,
        transaction_id: hello.transaction_id,
    };
    let hello_cookie = HelloCookie {
        flow: BootstrapFlow::Join,
        identity_digest: hello.identity_digest,
        device_nonce: hello.device_nonce,
        transaction_id: hello.transaction_id,
        lease_freshness_challenge_nonce: 4,
        selected_link_instance_id: 7,
        selected_link_instance_generation: 9,
        prior_messages_hash: [0x31; 32],
        cookie_evidence: Evidence::new(&[1, 2, 3, 4]).expect("cookie evidence"),
    };
    let handle = admission
        .open_after_cookie(
            &mut oracle,
            key,
            hello_cookie,
            transcript(BootstrapPhase::CookieVerified),
            310,
        )
        .expect("open admission");
    let evidence = Evidence::new(&[0xA5]).expect("evidence");
    for (event, phase, now) in [
        (
            BootstrapEvent::AuthorityProof,
            BootstrapPhase::AuthorityVerified,
            320,
        ),
        (
            BootstrapEvent::DeviceProof,
            BootstrapPhase::DeviceVerified,
            330,
        ),
        (
            BootstrapEvent::AddressOffer,
            BootstrapPhase::AddressOffered,
            340,
        ),
        (
            BootstrapEvent::DeviceCommit,
            BootstrapPhase::DeviceCommitted,
            350,
        ),
    ] {
        admission
            .advance(&mut oracle, handle, event, transcript(phase), evidence, now)
            .expect("advance admission");
    }
    let mut wrong_transcript = transcript(BootstrapPhase::FinalDurable);
    wrong_transcript.binding_lease_id[0] ^= 1;
    assert_eq!(
        admission.admit_durable(
            &mut oracle,
            handle,
            binding_view,
            wrong_transcript,
            evidence,
            360
        ),
        Err(Error::Security)
    );
    let admitted = admission
        .admit_durable(
            &mut oracle,
            handle,
            binding_view,
            transcript(BootstrapPhase::FinalDurable),
            evidence,
            360,
        )
        .expect("final durable admission");
    assert_eq!(admitted.binding, binding_view);
    assert_eq!(admission.admitted_get(handle, 361), Ok(admitted));
}
