//! Security Session、Persistence proof、受保护 Golden 与 Replay 集成测试。

use std::boxed::Box;

use ucn_owner::CallbackGate;
use ucn_persistence::{
    ATOMIC_COMMIT_MARKER_16, BlobState, Completion, DIGEST_BLAKE2S_128, DomainBinding, DomainKey,
    DomainKind, DomainState, DomainView, ENVELOPE_BYTES, IoPhase, IoStart, Lifecycle, MARKER_BYTES,
    Manifest, ManifestEntry, PersistenceConfig, PersistenceOwner, PersistenceProvider,
    ProviderGate, ProviderGeometry, RecordMeta, RequestState, WITNESS_INDEPENDENT_MONOTONIC,
    WitnessView, body_digest,
};
use ucn_security::{
    AccessDirection, AccessRequest, AclRule, Binding, C0TransactionOwner, CryptoProvider,
    CurrentFacts, DurabilityBase, Fingerprint, HandshakeCandidate, HandshakeProof,
    HopSequenceOwner, KeySelector, OpenDisposition, OpenOriginRequest, OpenPacketPlan,
    OriginContext, OriginCounterOwner, OriginSequenceOwner, PacketPlan, SealOriginRequest,
    SecurityConfig, SecurityLevel, SecurityOwner, SecurityPacketWorkspace, SessionDomainRule,
    SessionHandle, SessionPhase, decode_session_record,
};
use ucn_types::{
    AddressWidth, BindingGeneration, C0TransactionId, Error, HeaderContract, HopProfile,
    HopSequence, KeyId, LinkInstanceGeneration, NodeAddress, OriginSequence, PeerSessionGeneration,
    ProtocolOpcode, RealmId, Result, ServiceId, SuiteId,
};

const BODY: usize = 256;
const SLOT: usize = ENVELOPE_BYTES + BODY + MARKER_BYTES;
const BODY_U32: u32 = 256;
const SESSION_RECORD_BYTES_U32: u32 = 192;
const SLOT_U32: u32 = 368;
const MARKER_BYTES_U16: u16 = 16;
const MARKER_BYTES_U32: u32 = 16;
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

    fn completion(
        token: u64,
        phase: IoPhase,
        exact_bytes: u32,
        state: BlobState,
        slot: u8,
    ) -> IoStart {
        IoStart::Completed(Completion {
            io_token: token,
            result: Ok(()),
            exact_bytes,
            phase,
            blob_state: state,
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
            atomic_marker_bytes: MARKER_BYTES_U16,
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
        if domain != DOMAIN || slot_index > 1 || output.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        output.copy_from_slice(&self.slots[usize::from(slot_index)]);
        Self::completion(
            token,
            IoPhase::LoadSlot,
            SLOT_U32,
            BlobState::Present,
            slot_index,
        )
    }

    fn begin_write_inactive(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        input: &[u8],
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN || slot_index > 1 || input.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        self.slots[usize::from(slot_index)].copy_from_slice(input);
        Self::completion(
            token,
            IoPhase::WriteInactive,
            SLOT_U32,
            BlobState::Present,
            slot_index,
        )
    }

    fn begin_readback(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        output: &mut [u8],
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN || slot_index > 1 || output.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        output.copy_from_slice(&self.slots[usize::from(slot_index)]);
        Self::completion(
            token,
            IoPhase::Readback,
            SLOT_U32,
            BlobState::Present,
            slot_index,
        )
    }

    fn begin_publish_marker(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        marker: &[u8; MARKER_BYTES],
        token: u64,
    ) -> IoStart {
        if domain != DOMAIN || slot_index > 1 {
            return IoStart::Failed(Error::Argument);
        }
        self.slots[usize::from(slot_index)][SLOT - MARKER_BYTES..].copy_from_slice(marker);
        Self::completion(
            token,
            IoPhase::PublishMarker,
            MARKER_BYTES_U32,
            BlobState::Present,
            slot_index,
        )
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
        Self::completion(token, IoPhase::LoadWitness, 40, output.state, u8::MAX)
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
        Self::completion(
            token,
            IoPhase::AdvanceWitness,
            8,
            BlobState::Present,
            u8::MAX,
        )
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

struct OracleCrypto {
    expected_candidate: HandshakeCandidate,
}

impl CryptoProvider for OracleCrypto {
    fn verify_session_proof(&mut self, candidate: &HandshakeCandidate, proof: &[u8]) -> Result<()> {
        if proof == b"proof" && *candidate == self.expected_candidate {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }

    fn compute_origin_tag(
        &mut self,
        _selector: KeySelector,
        aad: &[u8],
        plaintext: &[u8],
        tag: &mut [u8; 16],
    ) -> Result<()> {
        let expected = bytes(
            "55434E362D4F524947494E2D56310000002C62004002202122232425262728292A2B2C2D2E2F00000001303132333435363738393A3B3C3D3E3F00000004",
        );
        if aad == expected && plaintext == [1, 2, 3, 4] {
            tag.copy_from_slice(&bytes("8D2206580D6E913E15F14DE32AC68EF9"));
            return Ok(());
        }
        if aad.starts_with(b"UCN6-ORIGIN-V1") && plaintext == [0xDE, 0xAD, 0xBE, 0xEF] {
            tag.fill(0x5A);
            return Ok(());
        }
        Err(Error::Security)
    }

    fn verify_origin_tag(
        &mut self,
        selector: KeySelector,
        aad: &[u8],
        plaintext: &[u8],
        tag: &[u8; 16],
    ) -> Result<()> {
        let mut expected = [0_u8; 16];
        self.compute_origin_tag(selector, aad, plaintext, &mut expected)?;
        if tag == &expected {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }

    fn seal_origin(&mut self, request: SealOriginRequest<'_>) -> Result<()> {
        let SealOriginRequest {
            selector,
            nonce_input,
            aad,
            plaintext,
            ciphertext,
            tag,
            derived_nonce: nonce,
        } = request;
        match selector.suite {
            SuiteId::OriginAes128Gcm => {
                if nonce_input
                    != bytes(
                        "55434E362D4E4F4E43452D43302D5631505152535455565758595A5B5C5D5E5F010203040506070800130001",
                    )
                    || aad
                        != bytes(
                            "55434E362D4F524947494E2D56310000002260258000010203041234567800000001000000020102030405060708001300000004",
                        )
                    || plaintext != [0xDE, 0xAD, 0xBE, 0xEF]
                {
                    return Err(Error::Security);
                }
                ciphertext.copy_from_slice(&bytes("58B23779"));
                tag.copy_from_slice(&bytes("98A3E0C198106C043A0DBCC6BE150D6C"));
                nonce.copy_from_slice(&bytes("23543C789589E4D6067570A4"));
            }
            SuiteId::OriginChaCha20Poly1305 => {
                if nonce_input
                    != bytes(
                        "55434E362D4E4F4E43452D5345512D5631202122232425262728292A2B2C2D2E2F00000001",
                    )
                    || aad
                        != bytes(
                            "55434E362D4F524947494E2D56310000002C62008002202122232425262728292A2B2C2D2E2F00000001303132333435363738393A3B3C3D3E3F00000004",
                        )
                    || plaintext != [1, 2, 3, 4]
                {
                    return Err(Error::Security);
                }
                ciphertext.copy_from_slice(&bytes("81052B38"));
                tag.copy_from_slice(&bytes("E079A5ACC9BDDBE6A1E43903DC3541C2"));
                nonce.copy_from_slice(&bytes("1E606C8E60B022EAE3BC7931"));
            }
            _ => return Err(Error::Security),
        }
        Ok(())
    }

    fn open_origin(&mut self, request: OpenOriginRequest<'_>) -> Result<()> {
        let OpenOriginRequest {
            selector,
            nonce_input,
            aad,
            ciphertext,
            tag,
            plaintext,
            derived_nonce: nonce,
        } = request;
        let expected_plain = match selector.suite {
            SuiteId::OriginAes128Gcm => bytes("DEADBEEF"),
            SuiteId::OriginChaCha20Poly1305 => bytes("01020304"),
            _ => return Err(Error::Security),
        };
        let mut sealed = [0_u8; 4];
        let mut expected_tag = [0_u8; 16];
        self.seal_origin(SealOriginRequest {
            selector,
            nonce_input,
            aad,
            plaintext: &expected_plain,
            ciphertext: &mut sealed,
            tag: &mut expected_tag,
            derived_nonce: nonce,
        })?;
        if ciphertext != sealed || tag != &expected_tag {
            return Err(Error::Security);
        }
        plaintext.copy_from_slice(&expected_plain);
        Ok(())
    }

    fn compute_hop_tag(
        &mut self,
        _selector: KeySelector,
        aad: &[u8],
        tag: &mut [u8; 12],
    ) -> Result<()> {
        if aad
            != bytes(
                "55434E362D484F502D56310000001E63800211112222AABBCC00000001404142434445464748494A4B4C4D4E4F",
            )
        {
            return Err(Error::Security);
        }
        tag.copy_from_slice(&bytes("B61FCD2ED841CF792777F4F1"));
        Ok(())
    }

    fn verify_hop_tag(&mut self, selector: KeySelector, aad: &[u8], tag: &[u8; 12]) -> Result<()> {
        let mut expected = [0_u8; 12];
        self.compute_hop_tag(selector, aad, &mut expected)?;
        if tag == &expected {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }
}

struct SequenceCounter(u32);
impl OriginSequenceOwner for SequenceCounter {
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.0)
    }
    fn burn(&mut self, expected: OriginSequence) -> Result<()> {
        if expected.get() != self.0 {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}

struct TransactionCounter(u64);
impl C0TransactionOwner for TransactionCounter {
    fn preview(&self) -> Result<C0TransactionId> {
        C0TransactionId::new(self.0)
    }
    fn burn(&mut self, expected: C0TransactionId) -> Result<()> {
        if expected.get() != self.0 {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}

struct HopCounter(u32);
impl HopSequenceOwner for HopCounter {
    fn preview(&self) -> Result<HopSequence> {
        HopSequence::new(self.0)
    }
    fn burn(&mut self, expected: HopSequence) -> Result<()> {
        if expected.get() != self.0 {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}

fn binding(address: u32, generation: u32, principal: u8) -> Binding {
    Binding {
        address: NodeAddress::new(address, AddressWidth::A1).unwrap(),
        generation: BindingGeneration::active(generation).unwrap(),
        principal: [principal; 16],
    }
}

fn key(suite: SuiteId, id: u16) -> KeySelector {
    KeySelector {
        suite,
        key_id: KeyId::new(id).unwrap(),
        key_generation: 1,
    }
}

fn candidate(
    level: SecurityLevel,
    hop: HopProfile,
    suite: SuiteId,
    origin_fingerprint: [u8; 16],
    hop_fingerprint: [u8; 16],
) -> HandshakeCandidate {
    let hop_keys = matches!(hop, HopProfile::H1 | HopProfile::H3);
    HandshakeCandidate {
        local: binding(0x1234, 1, 0xA1),
        peer: binding(0x5678, 2, 0xB2),
        link_generation: LinkInstanceGeneration::new(1).unwrap(),
        session_generation: PeerSessionGeneration::new(1).unwrap(),
        policy_generation: 1,
        expires_at_us: 10_000,
        origin_level: level,
        hop_profile: hop,
        origin_tx: key(suite, 1),
        origin_rx: key(suite, 2),
        hop_tx: hop_keys.then(|| key(SuiteId::HopHmacSha256_96, 3)),
        hop_rx: hop_keys.then(|| key(SuiteId::HopHmacSha256_96, 4)),
        origin_fingerprint: Fingerprint::new(origin_fingerprint).unwrap(),
        hop_fingerprint: Fingerprint::new(hop_fingerprint).unwrap(),
        transcript_digest: [0xCC; 32],
    }
}

type Security = SecurityOwner<'static, 2, 4>;

#[allow(clippy::too_many_lines)]
fn activate_candidate(
    candidate: HandshakeCandidate,
    opcode: u16,
) -> (
    Security,
    SessionHandle,
    CurrentFacts,
    OracleCrypto,
    ucn_persistence::DomainView,
    [u8; ucn_security::SESSION_RECORD_BYTES],
) {
    let callback_gate = Box::leak(Box::new(CallbackGate::new(41).unwrap()));
    let session_domains = Box::leak(Box::new([SessionDomainRule {
        domain: DOMAIN,
        peer_principal: candidate.peer.principal,
    }]));
    let rules = Box::leak(Box::new([
        AclRule {
            peer_principal: candidate.peer.principal,
            peer_binding_generation: candidate.peer.generation,
            context_fingerprint: candidate.origin_fingerprint,
            service: ServiceId::new(1).unwrap(),
            protocol_opcode: opcode,
            direction: AccessDirection::Outbound,
        },
        AclRule {
            peer_principal: candidate.peer.principal,
            peer_binding_generation: candidate.peer.generation,
            context_fingerprint: candidate.origin_fingerprint,
            service: ServiceId::new(1).unwrap(),
            protocol_opcode: opcode,
            direction: AccessDirection::Inbound,
        },
        AclRule {
            peer_principal: candidate.peer.principal,
            peer_binding_generation: candidate.peer.generation,
            context_fingerprint: candidate.hop_fingerprint,
            service: ServiceId::new(1).unwrap(),
            protocol_opcode: opcode,
            direction: AccessDirection::Outbound,
        },
        AclRule {
            peer_principal: candidate.peer.principal,
            peer_binding_generation: candidate.peer.generation,
            context_fingerprint: candidate.hop_fingerprint,
            service: ServiceId::new(1).unwrap(),
            protocol_opcode: opcode,
            direction: AccessDirection::Inbound,
        },
    ]));
    let mut security = Security::new(SecurityConfig {
        runtime_instance: 11,
        owner_instance: 41,
        persistence_business_owner_instance: 21,
        persistence_owner_instance: 12,
        realm: RealmId::new(0x0102_0304).unwrap(),
        address_width: AddressWidth::A1,
        provider_gate: callback_gate,
        session_domains,
        replay_reservation_lifetime_us: 100,
        acl: rules,
    })
    .unwrap();
    let facts = CurrentFacts {
        local: candidate.local,
        peer: candidate.peer,
        link_generation: candidate.link_generation,
        policy_generation: candidate.policy_generation,
        now_us: 10,
    };

    let mut provider = MemoryProvider::new();
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
    let binding = DomainBinding {
        domain: DOMAIN,
        business_owner_instance: 21,
        domain_generation: 31,
    };
    let mut persistence = PersistenceOwner::<1, BODY, SLOT>::uninit();
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
                bindings: core::slice::from_ref(&binding),
                provider_gate: &provider_gate,
            },
            &provider,
        )
        .unwrap();
    persistence.start_recovery().unwrap();
    for _ in 0..32 {
        if persistence.lifecycle() == Lifecycle::Ready {
            break;
        }
        persistence.step(&mut provider, 10, 1).unwrap();
    }
    assert_eq!(persistence.lifecycle(), Lifecycle::Ready);
    let view = persistence.domain_get(DOMAIN).unwrap();
    let mut current_body = [0_u8; BODY];
    let current_bytes = persistence.copy_body(DOMAIN, &mut current_body).unwrap();
    let mut durability_codec = ucn_persistence::CodecWorkspace::new();
    let base = DurabilityBase::from_view(
        11,
        21,
        view,
        &current_body[..current_bytes],
        1_000,
        77,
        &mut durability_codec,
    )
    .unwrap();
    let mut crypto = OracleCrypto {
        expected_candidate: candidate,
    };
    let (handle, requirement) = security
        .prepare_static_session(
            &mut crypto,
            candidate,
            HandshakeProof { bytes: b"proof" },
            base,
            facts,
        )
        .unwrap();
    assert_eq!(security.session_get(handle, facts), Err(Error::State));
    let persistence_handle = persistence
        .submit(&requirement.persistence_request(), 20)
        .unwrap();
    security
        .bind_persistence(handle, persistence_handle)
        .unwrap();
    for _ in 0..64 {
        if persistence.request_get(persistence_handle).unwrap().state == RequestState::ProofReady {
            break;
        }
        persistence.step(&mut provider, 30, 1).unwrap();
    }
    let proof = persistence.proof_get(persistence_handle).unwrap();
    security
        .activate(handle, persistence_handle, proof, facts)
        .unwrap();
    persistence.proof_retire(persistence_handle).unwrap();
    let mut body = [0_u8; BODY];
    let count = persistence.copy_body(DOMAIN, &mut body).unwrap();
    let (_, _, decoded) = decode_session_record(&body[..count]).unwrap();
    assert_eq!(decoded, candidate);
    let committed_view = persistence.domain_get(DOMAIN).unwrap();
    let committed_body = body[..count].try_into().unwrap();
    (
        security,
        handle,
        facts,
        crypto,
        committed_view,
        committed_body,
    )
}

fn bytes(hex: &str) -> Vec<u8> {
    assert_eq!(hex.len() % 2, 0);
    hex.as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let text = std::str::from_utf8(pair).unwrap();
            u8::from_str_radix(text, 16).unwrap()
        })
        .collect()
}

fn access(
    session: SessionHandle,
    direction: AccessDirection,
    opcode: u16,
    context_fingerprint: Fingerprint,
) -> AccessRequest {
    AccessRequest {
        session,
        context_fingerprint,
        service: ServiceId::new(1).unwrap(),
        protocol_opcode: opcode,
        direction,
    }
}

#[test]
fn session_requires_durable_proof_and_exact_current_facts() {
    let candidate = candidate(
        SecurityLevel::Authenticated,
        HopProfile::H1,
        SuiteId::OriginHmacSha256_128,
        [0x20; 16],
        [0x40; 16],
    );
    let (mut owner, handle, facts, _, view, body) = activate_candidate(candidate, 0);
    assert_eq!(
        owner.session_get(handle, facts).unwrap().phase,
        SessionPhase::Active
    );
    let mut stale = facts;
    stale.policy_generation = 2;
    assert_eq!(owner.session_get(handle, stale), Err(Error::State));
    let mut rebound_peer = facts;
    rebound_peer.peer.generation = BindingGeneration::active(3).unwrap();
    assert_eq!(owner.session_get(handle, rebound_peer), Err(Error::State));
    assert_eq!(
        owner.session_get(
            handle,
            CurrentFacts {
                now_us: 10_000,
                ..facts
            }
        ),
        Err(Error::State)
    );
    assert_eq!(owner.expire(10_000), 1);
    assert_eq!(owner.session_get(handle, facts), Err(Error::State));
    owner.retire_fenced(handle).unwrap();
    assert_eq!(owner.retire_fenced(handle), Err(Error::NotFound));

    let mut tampered = body;
    tampered[20] ^= 1;
    let mut codec = ucn_persistence::CodecWorkspace::new();
    assert_eq!(
        DurabilityBase::from_view(11, 21, view, &tampered, 1_000, 78, &mut codec),
        Err(Error::Security)
    );
    let mut malformed = body;
    malformed[9] = 1;
    assert_eq!(decode_session_record(&malformed), Err(Error::Malformed));
    assert_eq!(
        decode_session_record(&body[..body.len() - 1]),
        Err(Error::Malformed)
    );
}

#[test]
fn recovered_session_generation_must_advance_exactly_once() {
    let first = candidate(
        SecurityLevel::Authenticated,
        HopProfile::H1,
        SuiteId::OriginHmacSha256_128,
        [0x20; 16],
        [0x40; 16],
    );
    let (mut owner, _, facts, mut crypto, view, body) = activate_candidate(first, 0);
    let mut codec = ucn_persistence::CodecWorkspace::new();
    let base = DurabilityBase::from_view(11, 21, view, &body, 1_000, 79, &mut codec).unwrap();

    let mut wrong_domain = first;
    wrong_domain.session_generation = PeerSessionGeneration::new(2).unwrap();
    wrong_domain.origin_tx.key_generation = 2;
    wrong_domain.origin_rx.key_generation = 2;
    wrong_domain.hop_tx.as_mut().unwrap().key_generation = 2;
    wrong_domain.hop_rx.as_mut().unwrap().key_generation = 2;
    wrong_domain.transcript_digest = [0xCD; 32];
    let mut wrong_base = base;
    wrong_base.domain = DomainKey {
        kind: DomainKind::SecurityHighWater,
        id: 2,
    };
    assert_eq!(
        owner
            .prepare_static_session(
                &mut crypto,
                wrong_domain,
                HandshakeProof { bytes: b"proof" },
                wrong_base,
                facts,
            )
            .map(|_| ()),
        Err(Error::Access)
    );

    let mut wrong_peer = wrong_domain;
    wrong_peer.peer.principal = [0xD4; 16];
    assert_eq!(
        owner
            .prepare_static_session(
                &mut crypto,
                wrong_peer,
                HandshakeProof { bytes: b"proof" },
                base,
                facts,
            )
            .map(|_| ()),
        Err(Error::Access)
    );

    let mut skipped = first;
    skipped.session_generation = PeerSessionGeneration::new(3).unwrap();
    skipped.origin_tx.key_generation = 2;
    skipped.origin_rx.key_generation = 2;
    skipped.hop_tx.as_mut().unwrap().key_generation = 2;
    skipped.hop_rx.as_mut().unwrap().key_generation = 2;
    skipped.transcript_digest = [0xCD; 32];
    assert_eq!(
        owner
            .prepare_static_session(
                &mut crypto,
                skipped,
                HandshakeProof { bytes: b"proof" },
                base,
                facts,
            )
            .map(|_| ()),
        Err(Error::Replay)
    );

    let mut next = skipped;
    next.session_generation = PeerSessionGeneration::new(2).unwrap();
    crypto.expected_candidate = next;
    let (handle, requirement) = owner
        .prepare_static_session(
            &mut crypto,
            next,
            HandshakeProof { bytes: b"proof" },
            base,
            facts,
        )
        .unwrap();
    assert_eq!(
        requirement.persistence_request().foundation_transaction_id,
        2
    );
    assert_eq!(owner.session_get(handle, facts), Err(Error::State));

    let mut reused_key_generation = next;
    reused_key_generation.origin_tx.key_generation = 1;
    assert_eq!(
        owner
            .prepare_static_session(
                &mut crypto,
                reused_key_generation,
                HandshakeProof { bytes: b"proof" },
                base,
                facts,
            )
            .map(|_| ()),
        Err(Error::Replay)
    );
}

#[test]
fn handshake_proof_binds_every_candidate_field() {
    let first = candidate(
        SecurityLevel::Authenticated,
        HopProfile::H1,
        SuiteId::OriginHmacSha256_128,
        [0x20; 16],
        [0x40; 16],
    );
    let (mut owner, _, facts, mut crypto, view, body) = activate_candidate(first, 0);
    let mut codec = ucn_persistence::CodecWorkspace::new();
    let base = DurabilityBase::from_view(11, 21, view, &body, 1_000, 79, &mut codec).unwrap();
    let mut expected = first;
    expected.session_generation = PeerSessionGeneration::new(2).unwrap();
    expected.origin_tx.key_generation = 2;
    expected.origin_rx.key_generation = 2;
    expected.hop_tx.as_mut().unwrap().key_generation = 2;
    expected.hop_rx.as_mut().unwrap().key_generation = 2;
    expected.transcript_digest = [0xCD; 32];
    crypto.expected_candidate = expected;
    let mut proof_mismatch = expected;
    proof_mismatch.expires_at_us += 1;
    assert_eq!(
        owner
            .prepare_static_session(
                &mut crypto,
                proof_mismatch,
                HandshakeProof { bytes: b"proof" },
                base,
                facts,
            )
            .map(|_| ()),
        Err(Error::Security)
    );
}

#[test]
#[allow(clippy::too_many_lines)]
fn disabling_hop_protection_does_not_release_its_key_high_water() {
    let first = candidate(
        SecurityLevel::Authenticated,
        HopProfile::H1,
        SuiteId::OriginHmacSha256_128,
        [0x20; 16],
        [0x40; 16],
    );
    let (mut owner, first_handle, facts, mut crypto, first_view, first_body) =
        activate_candidate(first, 0);
    let mut codec = ucn_persistence::CodecWorkspace::new();
    let first_base =
        DurabilityBase::from_view(11, 21, first_view, &first_body, 1_000, 81, &mut codec).unwrap();
    let mut disabled = first;
    disabled.session_generation = PeerSessionGeneration::new(2).unwrap();
    disabled.origin_tx.key_generation = 2;
    disabled.origin_rx.key_generation = 2;
    disabled.hop_profile = HopProfile::H0;
    disabled.hop_tx = None;
    disabled.hop_rx = None;
    disabled.transcript_digest = [0xDD; 32];
    crypto.expected_candidate = disabled;
    let (_, disabled_requirement) = owner
        .prepare_static_session(
            &mut crypto,
            disabled,
            HandshakeProof { bytes: b"proof" },
            first_base,
            facts,
        )
        .unwrap();
    let request = disabled_requirement.persistence_request();
    let record_generation = request.expected_record_generation + 1;
    let meta = RecordMeta {
        domain: request.domain,
        record_generation,
        transaction_id: request.foundation_transaction_id,
        body_bytes: SESSION_RECORD_BYTES_U32,
        schema_id: request.schema_id,
        schema_version: request.schema_version,
        operation_kind: request.operation_kind,
        body_digest: [0; 16],
    };
    let digest = body_digest(&meta, request.canonical_body, &mut codec).unwrap();
    let disabled_view = DomainView {
        domain: request.domain,
        record_generation,
        foundation_transaction_id: request.foundation_transaction_id,
        body_bytes: SESSION_RECORD_BYTES_U32,
        schema_id: request.schema_id,
        schema_version: request.schema_version,
        current_operation_kind: request.operation_kind,
        domain_generation: request.domain_generation,
        state: DomainState::Ready,
        active_slot: 1,
        required: true,
        pending: false,
        body_digest: digest,
    };
    let next_base = DurabilityBase::from_view(
        11,
        21,
        disabled_view,
        request.canonical_body,
        1_000,
        82,
        &mut codec,
    )
    .unwrap();
    owner.fence(first_handle).unwrap();
    owner.retire_fenced(first_handle).unwrap();

    let mut reused = disabled;
    reused.session_generation = PeerSessionGeneration::new(3).unwrap();
    reused.origin_tx.key_generation = 3;
    reused.origin_rx.key_generation = 3;
    reused.hop_profile = HopProfile::H1;
    reused.hop_tx = first.hop_tx;
    reused.hop_rx = first.hop_rx;
    reused.transcript_digest = [0xEE; 32];
    crypto.expected_candidate = reused;
    assert_eq!(
        owner
            .prepare_static_session(
                &mut crypto,
                reused,
                HandshakeProof { bytes: b"proof" },
                next_base,
                facts,
            )
            .map(|_| ()),
        Err(Error::Replay)
    );
    reused.hop_tx.as_mut().unwrap().key_generation = 2;
    reused.hop_rx.as_mut().unwrap().key_generation = 2;
    crypto.expected_candidate = reused;
    assert!(
        owner
            .prepare_static_session(
                &mut crypto,
                reused,
                HandshakeProof { bytes: b"proof" },
                next_base,
                facts,
            )
            .is_ok()
    );
}

#[test]
fn duplicate_session_domain_or_principal_configuration_is_rejected() {
    let gate = CallbackGate::new(41).unwrap();
    let domain_two = DomainKey {
        kind: DomainKind::SecurityHighWater,
        id: 2,
    };
    let duplicate_domain = [
        SessionDomainRule {
            domain: DOMAIN,
            peer_principal: [0xB2; 16],
        },
        SessionDomainRule {
            domain: DOMAIN,
            peer_principal: [0xD4; 16],
        },
    ];
    let duplicate_principal = [
        SessionDomainRule {
            domain: DOMAIN,
            peer_principal: [0xB2; 16],
        },
        SessionDomainRule {
            domain: domain_two,
            peer_principal: [0xB2; 16],
        },
    ];
    for session_domains in [&duplicate_domain[..], &duplicate_principal[..]] {
        let result = SecurityOwner::<3, 4>::new(SecurityConfig {
            runtime_instance: 11,
            owner_instance: 41,
            persistence_business_owner_instance: 21,
            persistence_owner_instance: 12,
            realm: RealmId::new(0x0102_0304).unwrap(),
            address_width: AddressWidth::A1,
            provider_gate: &gate,
            session_domains,
            replay_reservation_lifetime_us: 100,
            acl: &[],
        });
        assert!(matches!(result, Err(Error::Config)));
    }
}

#[test]
#[allow(clippy::too_many_lines)]
fn frozen_c2_o1_h2_and_replay_are_exact() {
    let flow: [u8; 16] = bytes("202122232425262728292A2B2C2D2E2F")
        .try_into()
        .unwrap();
    let direct: [u8; 16] = bytes("303132333435363738393A3B3C3D3E3F")
        .try_into()
        .unwrap();
    let candidate = candidate(
        SecurityLevel::Authenticated,
        HopProfile::H2,
        SuiteId::OriginHmacSha256_128,
        flow,
        direct,
    );
    let (mut owner, handle, facts, mut crypto, _, _) = activate_candidate(candidate, 0);
    let prefix = bytes("620041123400000001");
    let payload = bytes("01020304");
    let mut identity = Vec::from(flow);
    identity.extend_from_slice(&1_u32.to_be_bytes());
    identity.extend_from_slice(&direct);
    let mut sequence = SequenceCounter(1);
    let mut workspace = SecurityPacketWorkspace::<128>::new();
    let mut output = [0xA5_u8; 64];
    let count = owner
        .protect_packet(
            &mut crypto,
            OriginCounterOwner::Sequence(&mut sequence),
            handle,
            facts,
            access(
                handle,
                AccessDirection::Outbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            PacketPlan {
                prefix: &prefix,
                payload: &payload,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            None,
            &mut workspace,
            &mut output,
        )
        .unwrap();
    let golden = bytes("620041123400000001010203048D2206580D6E913E15F14DE32AC68EF9");
    assert_eq!(&output[..count], golden);
    assert_eq!(sequence.0, 2);

    let mut plaintext = [0xCC_u8; 8];
    let mut corrupt = golden.clone();
    let last = corrupt.len() - 1;
    corrupt[last] ^= 1;
    assert_eq!(
        owner.open_packet(
            &mut crypto,
            handle,
            facts,
            access(
                handle,
                AccessDirection::Inbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            OpenPacketPlan {
                packet: &corrupt,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        ),
        Err(Error::Security)
    );
    assert_eq!(plaintext, [0xCC; 8]);
    let wrong_context = AccessRequest {
        session: handle,
        context_fingerprint: Fingerprint::new([0xEE; 16]).unwrap(),
        service: ServiceId::new(1).unwrap(),
        protocol_opcode: 0,
        direction: AccessDirection::Inbound,
    };
    assert_eq!(
        owner.open_packet(
            &mut crypto,
            handle,
            facts,
            wrong_context,
            OpenPacketPlan {
                packet: &golden,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        ),
        Err(Error::Access)
    );
    assert_eq!(plaintext, [0xCC; 8]);
    let unauthorized = AccessRequest {
        session: handle,
        context_fingerprint: Fingerprint::new(flow).unwrap(),
        service: ServiceId::new(2).unwrap(),
        protocol_opcode: 0,
        direction: AccessDirection::Inbound,
    };
    assert_eq!(
        owner.open_packet(
            &mut crypto,
            handle,
            facts,
            unauthorized,
            OpenPacketPlan {
                packet: &golden,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        ),
        Err(Error::Access)
    );
    assert_eq!(plaintext, [0xCC; 8]);
    let opened = owner
        .open_packet(
            &mut crypto,
            handle,
            facts,
            access(
                handle,
                AccessDirection::Inbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            OpenPacketPlan {
                packet: &golden,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        )
        .unwrap();
    assert_eq!(opened.payload_bytes, 4);
    assert_eq!(opened.disposition, OpenDisposition::FreshAuthenticated);
    assert_eq!(&plaintext[..4], payload);
    plaintext.fill(0xCC);
    assert!(matches!(
        owner.open_packet(
            &mut crypto,
            handle,
            facts,
            access(
                handle,
                AccessDirection::Inbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            OpenPacketPlan {
                packet: &golden,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        ),
        Err(Error::State)
    ));
    assert_eq!(plaintext, [0xCC; 8]);
    let after_deadline = CurrentFacts {
        now_us: 110,
        ..facts
    };
    assert_eq!(
        owner.replay_commit(opened.replay, after_deadline),
        Err(Error::Timeout)
    );
    let aborted = owner
        .open_packet(
            &mut crypto,
            handle,
            after_deadline,
            access(
                handle,
                AccessDirection::Inbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            OpenPacketPlan {
                packet: &golden,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        )
        .unwrap();
    owner.replay_abort(aborted.replay).unwrap();
    let _orphaned = owner
        .open_packet(
            &mut crypto,
            handle,
            after_deadline,
            access(
                handle,
                AccessDirection::Inbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            OpenPacketPlan {
                packet: &golden,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        )
        .unwrap();
    assert_eq!(
        owner.maintain_replay_reservations(209, 1),
        ucn_security::ReplayMaintenance {
            inspected: 1,
            expired: 0,
        }
    );
    assert_eq!(owner.maintain_replay_reservations(210, 16).expired, 1);
    let maintenance_facts = CurrentFacts {
        now_us: 210,
        ..facts
    };
    let recommitted = owner
        .open_packet(
            &mut crypto,
            handle,
            maintenance_facts,
            access(
                handle,
                AccessDirection::Inbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            OpenPacketPlan {
                packet: &golden,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        )
        .unwrap();
    let committed = owner
        .replay_commit(recommitted.replay, maintenance_facts)
        .unwrap();
    assert_eq!(committed.disposition, OpenDisposition::FreshAuthenticated);
    let origin_evidence = committed.origin.unwrap();
    assert_eq!(origin_evidence.sequence, 1);
    assert_eq!(origin_evidence.session_generation, 1);
    assert_eq!(origin_evidence.key_generation, 1);
    assert_eq!(origin_evidence.context_fingerprint, flow);
    plaintext.fill(0xCC);
    let duplicate = owner
        .open_packet(
            &mut crypto,
            handle,
            maintenance_facts,
            access(
                handle,
                AccessDirection::Inbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            OpenPacketPlan {
                packet: &golden,
                prefix_bytes: 9,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            &mut workspace,
            &mut plaintext,
        )
        .unwrap();
    assert_eq!(
        duplicate.disposition,
        OpenDisposition::AuthenticatedReplayCandidate
    );
    assert_eq!(duplicate.payload_bytes, 0);
    let duplicate_evidence = duplicate.duplicate_evidence.unwrap();
    assert_eq!(duplicate_evidence.sequence, 1);
    assert_eq!(duplicate_evidence.session_generation, 1);
    assert_eq!(duplicate_evidence.key_generation, 1);
    assert_eq!(duplicate_evidence.context_fingerprint, flow);
    owner
        .replay_commit(duplicate.replay, maintenance_facts)
        .unwrap();
    assert_eq!(plaintext, [0xCC; 8]);

    let mut failed_prefix = prefix.clone();
    failed_prefix[8] = 2;
    let mut failed_identity = identity.clone();
    let identity_last = failed_identity.len() - 1;
    failed_identity[identity_last] = 2;
    let mut failed_sequence = SequenceCounter(2);
    let mut failed_output = [0xA5_u8; 64];
    assert_eq!(
        owner.protect_packet(
            &mut crypto,
            OriginCounterOwner::Sequence(&mut failed_sequence),
            handle,
            maintenance_facts,
            access(
                handle,
                AccessDirection::Outbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            PacketPlan {
                prefix: &failed_prefix,
                payload: &payload,
                canonical_identity: &failed_identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(2).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            None,
            &mut workspace,
            &mut failed_output,
        ),
        Err(Error::Security)
    );
    assert_eq!(failed_sequence.0, 2);
    assert_eq!(failed_output, [0xA5; 64]);
}

#[test]
#[allow(clippy::too_many_lines)]
fn frozen_c0_aes_c2_chacha_and_c3_hop_vectors_match() {
    let flow: [u8; 16] = bytes("202122232425262728292A2B2C2D2E2F")
        .try_into()
        .unwrap();
    let direct: [u8; 16] = bytes("303132333435363738393A3B3C3D3E3F")
        .try_into()
        .unwrap();
    let c0_fp: [u8; 16] = bytes("505152535455565758595A5B5C5D5E5F")
        .try_into()
        .unwrap();
    let hop_fp: [u8; 16] = bytes("404142434445464748494A4B4C4D4E4F")
        .try_into()
        .unwrap();

    let c0 = candidate(
        SecurityLevel::Confidential,
        HopProfile::H0,
        SuiteId::OriginAes128Gcm,
        c0_fp,
        [0x30; 16],
    );
    let (mut owner, handle, facts, mut crypto, _, _) =
        activate_candidate(c0, u16::from(ProtocolOpcode::PeerSessionPrepare));
    let prefix = bytes("6025810102030412345678000000010000000201020304050607080013");
    let payload = bytes("DEADBEEF");
    let mut transaction = TransactionCounter(0x0102_0304_0506_0708);
    let mut workspace = SecurityPacketWorkspace::<160>::new();
    let mut output = [0_u8; 80];
    let count = owner
        .protect_packet(
            &mut crypto,
            OriginCounterOwner::Transaction(&mut transaction),
            handle,
            facts,
            access(
                handle,
                AccessDirection::Outbound,
                u16::from(ProtocolOpcode::PeerSessionPrepare),
                Fingerprint::new(c0_fp).unwrap(),
            ),
            PacketPlan {
                prefix: &prefix,
                payload: &payload,
                canonical_identity: &prefix[3..],
                origin_context: OriginContext::C0 {
                    fingerprint: Fingerprint::new(c0_fp).unwrap(),
                    transaction_id: C0TransactionId::new(0x0102_0304_0506_0708).unwrap(),
                    opcode: ProtocolOpcode::PeerSessionPrepare,
                    direction: ucn_security::SenderDirection::InitiatorToResponder,
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut workspace,
            &mut output,
        )
        .unwrap();
    assert_eq!(
        &output[..count],
        bytes(
            "602581010203041234567800000001000000020102030405060708001358B2377998A3E0C198106C043A0DBCC6BE150D6C"
        )
    );

    let chacha = candidate(
        SecurityLevel::Confidential,
        HopProfile::H2,
        SuiteId::OriginChaCha20Poly1305,
        flow,
        direct,
    );
    let (mut owner, handle, facts, mut crypto, _, _) = activate_candidate(chacha, 0);
    let prefix = bytes("620081123400000001");
    let payload = bytes("01020304");
    let mut identity = Vec::from(flow);
    identity.extend_from_slice(&1_u32.to_be_bytes());
    identity.extend_from_slice(&direct);
    let mut sequence = SequenceCounter(1);
    let count = owner
        .protect_packet(
            &mut crypto,
            OriginCounterOwner::Sequence(&mut sequence),
            handle,
            facts,
            access(
                handle,
                AccessDirection::Outbound,
                0,
                Fingerprint::new(flow).unwrap(),
            ),
            PacketPlan {
                prefix: &prefix,
                payload: &payload,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new(flow).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H2,
            },
            None,
            &mut workspace,
            &mut output,
        )
        .unwrap();
    assert_eq!(
        &output[..count],
        bytes("62008112340000000181052B38E079A5ACC9BDDBE6A1E43903DC3541C2")
    );

    let c3 = candidate(
        SecurityLevel::Authenticated,
        HopProfile::H1,
        SuiteId::OriginHmacSha256_128,
        [0x20; 16],
        hop_fp,
    );
    let (mut owner, handle, facts, mut crypto, _, _) = activate_candidate(c3, 0);
    let prefix = bytes("63800211112222");
    let payload = bytes("AABBCC");
    let mut hop_sequence = HopCounter(1);
    let count = owner
        .protect_packet(
            &mut crypto,
            OriginCounterOwner::None,
            handle,
            facts,
            access(
                handle,
                AccessDirection::Outbound,
                0,
                Fingerprint::new(hop_fp).unwrap(),
            ),
            PacketPlan {
                prefix: &prefix,
                payload: &payload,
                canonical_identity: &[1],
                origin_context: OriginContext::None {
                    contract: HeaderContract::C3,
                    fingerprint: Fingerprint::new([0x20; 16]).unwrap(),
                },
                hop_profile: HopProfile::H1,
            },
            Some(&mut hop_sequence),
            &mut workspace,
            &mut output,
        )
        .unwrap();
    assert_eq!(
        &output[..count],
        bytes("63800211112222AABBCC00000001B61FCD2ED841CF792777F4F1")
    );
    let mut opened_payload = [0xCC_u8; 8];
    let opened = owner
        .open_packet(
            &mut crypto,
            handle,
            facts,
            access(
                handle,
                AccessDirection::Inbound,
                0,
                Fingerprint::new(hop_fp).unwrap(),
            ),
            OpenPacketPlan {
                packet: &output[..count],
                prefix_bytes: 7,
                canonical_identity: &[1],
                origin_context: OriginContext::None {
                    contract: HeaderContract::C3,
                    fingerprint: Fingerprint::new([0x20; 16]).unwrap(),
                },
                hop_profile: HopProfile::H1,
            },
            &mut workspace,
            &mut opened_payload,
        )
        .unwrap();
    assert_eq!(&opened_payload[..3], payload);
    let committed = owner.replay_commit(opened.replay, facts).unwrap();
    assert_eq!(committed.origin, None);
    let hop_evidence = committed.hop.unwrap();
    assert_eq!(hop_evidence.sequence, 1);
    assert_eq!(hop_evidence.session_generation, 1);
    assert_eq!(hop_evidence.key_generation, 1);
    assert_eq!(hop_evidence.context_fingerprint, hop_fp);
}

#[test]
#[allow(clippy::too_many_lines)]
fn c1_wrong_wire_binding_is_rejected_before_sequence_burn() {
    let candidate = candidate(
        SecurityLevel::Authenticated,
        HopProfile::H0,
        SuiteId::OriginHmacSha256_128,
        [0x20; 16],
        [0x30; 16],
    );
    let (mut owner, handle, facts, mut crypto, _, _) = activate_candidate(candidate, 0);
    let prefix = bytes("61404112355678000100000001");
    let payload = bytes("DEADBEEF");
    let mut identity = Vec::from(candidate.local.principal);
    identity.extend_from_slice(&candidate.local.generation.get().to_be_bytes());
    identity.extend_from_slice(&candidate.peer.principal);
    identity.extend_from_slice(&candidate.peer.generation.get().to_be_bytes());
    identity.extend_from_slice(&1_u16.to_be_bytes());
    identity.extend_from_slice(&1_u32.to_be_bytes());
    let mut sequence = SequenceCounter(1);
    let mut workspace = SecurityPacketWorkspace::<128>::new();
    let mut output = [0xA5_u8; 64];
    assert_eq!(
        owner.protect_packet(
            &mut crypto,
            OriginCounterOwner::Sequence(&mut sequence),
            handle,
            facts,
            access(
                handle,
                AccessDirection::Outbound,
                0,
                Fingerprint::new([0x20; 16]).unwrap(),
            ),
            PacketPlan {
                prefix: &prefix,
                payload: &payload,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: Fingerprint::new([0x20; 16]).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut workspace,
            &mut output,
        ),
        Err(Error::Security)
    );
    assert_eq!(sequence.0, 1);
    assert_eq!(output, [0xA5; 64]);

    let zero_context_prefix = bytes("620041000000000001");
    assert_eq!(
        owner.protect_packet(
            &mut crypto,
            OriginCounterOwner::Sequence(&mut sequence),
            handle,
            facts,
            access(
                handle,
                AccessDirection::Outbound,
                0,
                Fingerprint::new([0x20; 16]).unwrap(),
            ),
            PacketPlan {
                prefix: &zero_context_prefix,
                payload: &payload,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C2,
                    fingerprint: Fingerprint::new([0x20; 16]).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut workspace,
            &mut output,
        ),
        Err(Error::Argument)
    );
    assert_eq!(sequence.0, 1);
    assert_eq!(output, [0xA5; 64]);

    let plaintext_prefix = bytes("61400112345678000100000001");
    assert_eq!(
        owner.protect_packet(
            &mut crypto,
            OriginCounterOwner::Sequence(&mut sequence),
            handle,
            facts,
            access(
                handle,
                AccessDirection::Outbound,
                0,
                Fingerprint::new([0x20; 16]).unwrap(),
            ),
            PacketPlan {
                prefix: &plaintext_prefix,
                payload: &payload,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: Fingerprint::new([0x20; 16]).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut workspace,
            &mut output,
        ),
        Err(Error::Security)
    );
    assert_eq!(sequence.0, 1);
    assert_eq!(output, [0xA5; 64]);

    let correct_prefix = bytes("61404112345678000100000001");
    let count = owner
        .protect_packet(
            &mut crypto,
            OriginCounterOwner::Sequence(&mut sequence),
            handle,
            facts,
            access(
                handle,
                AccessDirection::Outbound,
                0,
                Fingerprint::new([0x20; 16]).unwrap(),
            ),
            PacketPlan {
                prefix: &correct_prefix,
                payload: &payload,
                canonical_identity: &identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: Fingerprint::new([0x20; 16]).unwrap(),
                    sequence: OriginSequence::new(1).unwrap(),
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut workspace,
            &mut output,
        )
        .unwrap();
    assert_eq!(count, correct_prefix.len() + payload.len() + 16);
    assert_eq!(&output[..correct_prefix.len()], correct_prefix);
    assert_eq!(
        &output[correct_prefix.len()..correct_prefix.len() + payload.len()],
        payload
    );
    assert_eq!(&output[count - 16..count], [0x5A; 16]);
    assert_eq!(sequence.0, 2);
}
