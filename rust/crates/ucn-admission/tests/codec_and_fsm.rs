//! Admission Codec、分片和认证前资源门禁的组合回归。

use std::vec::Vec;

use ucn_admission::{
    AdmissionConfig, AdmissionKey, BootstrapEvent, BootstrapFlow, BootstrapFragment,
    BootstrapPhase, BootstrapTranscript, COOKIE_CHALLENGE_BYTES, COOKIE_MAX_BYTES, CookieProvider,
    Evidence, FRAGMENT_HEADER_BYTES, HELLO_BYTES, Hello, HelloCookie, InitialHelloContext,
    LinkIdentity, NanoAdmissionOwner, OPCODE_DEVICE_COMMIT, OPCODE_IDENTITY_CHALLENGE, Reassembly,
    TRANSCRIPT_BYTES, decode_cookie_challenge, decode_fragment, decode_hello, decode_hello_cookie,
    decode_logical_event, decode_transcript, encode_cookie_challenge, encode_fragment,
    encode_hello, encode_hello_cookie, encode_logical_event, encode_transcript,
};
use ucn_identity::Principal;
use ucn_owner::CallbackGate;
use ucn_types::{Error, PROTOCOL_MAJOR, Result};

const SHARED_FIXTURES: &str =
    include_str!("../../../tests/conformance/v6s_admission_capability_v1.h");

fn fixture(name: &str) -> Vec<u8> {
    let marker = std::format!("#define {name}");
    let mut collecting = false;
    let mut bytes = Vec::new();
    for line in SHARED_FIXTURES.lines() {
        if !collecting {
            if line.starts_with(&marker) {
                collecting = true;
            } else {
                continue;
            }
        }
        for token in
            line.split(|character: char| character.is_ascii_whitespace() || character == ',')
        {
            let token = token.trim_end_matches('\\');
            if let Some(hex) = token.strip_prefix("0x") {
                bytes.push(u8::from_str_radix(hex, 16).expect("fixture byte"));
            }
        }
        if collecting && !line.trim_end().ends_with('\\') {
            break;
        }
    }
    assert!(!bytes.is_empty(), "fixture {name} must exist");
    bytes
}

struct Provider {
    issued: usize,
    verified: usize,
    authorized: usize,
}

impl CookieProvider for Provider {
    fn issue_cookie(
        &mut self,
        _hello: &Hello,
        _link: LinkIdentity,
        _cookie_time_bucket: u32,
        output: &mut [u8; COOKIE_MAX_BYTES],
    ) -> Result<u8> {
        self.issued += 1;
        output[..4].copy_from_slice(&[0xC1, 0xC2, 0xC3, 0xC4]);
        Ok(4)
    }

    fn verify_cookie(&mut self, _hello_cookie: &HelloCookie, _key: &AdmissionKey) -> Result<()> {
        self.verified += 1;
        Ok(())
    }

    fn authorize_event(
        &mut self,
        _event: BootstrapEvent,
        _key: &AdmissionKey,
        _transcript: &BootstrapTranscript,
        _now_us: u64,
        _evidence: &Evidence,
    ) -> Result<()> {
        self.authorized += 1;
        Ok(())
    }
}

fn principal(byte: u8) -> Principal {
    Principal::new([byte; 16]).expect("nontrivial principal")
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
        device_nonce: 0x0102_0304_0506_0708,
        authority_nonce: 0,
        transaction_id: 0x1112_1314_1516_1718,
        lease_freshness_challenge_nonce: 0x2122_2324_2526_2728,
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
        value.authority_nonce = 2;
        value.realm_id = 3;
        value.authority_address = 4;
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
        value.proposed_address = 10;
        value.address_binding_generation = 1;
        value.binding_lease_id = [0x61; 16];
        value.binding_lease_duration_us = 30_000;
        value.binding_mode = 2;
    }
    value
}

fn hello_and_cookie() -> (Hello, HelloCookie, AdmissionKey) {
    let hello = Hello {
        flow: BootstrapFlow::Join,
        identity_digest: principal(0x11),
        device_nonce: 0x0102_0304_0506_0708,
        transaction_id: 0x1112_1314_1516_1718,
    };
    let link = LinkIdentity {
        link_id: 7,
        link_generation: 9,
    };
    let key = AdmissionKey {
        link,
        local_peer_discriminator: 12,
        identity_digest: hello.identity_digest,
        transaction_id: hello.transaction_id,
    };
    let hello_cookie = HelloCookie {
        flow: BootstrapFlow::Join,
        identity_digest: hello.identity_digest,
        device_nonce: hello.device_nonce,
        transaction_id: hello.transaction_id,
        lease_freshness_challenge_nonce: 0x2122_2324_2526_2728,
        selected_link_instance_id: link.link_id,
        selected_link_instance_generation: link.link_generation,
        prior_messages_hash: [0x31; 32],
        cookie_evidence: Evidence::new(&[0xC1, 0xC2, 0xC3, 0xC4]).expect("evidence"),
    };
    (hello, hello_cookie, key)
}

#[test]
fn hello_and_cookie_use_frozen_big_endian_layout() {
    let (hello, hello_cookie, _) = hello_and_cookie();
    let mut encoded = [0xA5; HELLO_BYTES];
    encode_hello(hello, &mut encoded).expect("encode hello");
    let expected = fixture("UCN_V6S_ADMISSION_HELLO_JOIN_BYTES");
    assert_eq!(encoded.as_slice(), expected);
    assert_eq!(decode_hello(&expected), Ok(hello));

    let challenge = ucn_admission::CookieChallenge {
        flow: BootstrapFlow::Join,
        transaction_id: hello.transaction_id,
        cookie_time_bucket: 0x0102_0304,
        cookie_length: 4,
        cookie: [1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    };
    let mut challenge_bytes = [0; COOKIE_CHALLENGE_BYTES];
    encode_cookie_challenge(challenge, &mut challenge_bytes).expect("encode challenge");
    let expected_challenge = fixture("UCN_V6S_ADMISSION_COOKIE_CHALLENGE_BYTES");
    assert_eq!(challenge_bytes.as_slice(), expected_challenge);
    assert_eq!(decode_cookie_challenge(&expected_challenge), Ok(challenge));

    let mut hello_cookie_bytes = [0xA5; 98];
    let length =
        encode_hello_cookie(hello_cookie, &mut hello_cookie_bytes).expect("encode hello cookie");
    let expected_hello_cookie = fixture("UCN_V6S_ADMISSION_HELLO_COOKIE_BYTES");
    assert_eq!(&hello_cookie_bytes[..length], expected_hello_cookie);
    assert_eq!(
        decode_hello_cookie(&expected_hello_cookie),
        Ok(hello_cookie)
    );
}

#[test]
fn transcript_and_logical_codec_are_phase_bound() {
    let authority = transcript(BootstrapPhase::AuthorityVerified);
    let mut bytes = [0xA5; TRANSCRIPT_BYTES];
    encode_transcript(&authority, BootstrapPhase::AuthorityVerified, &mut bytes)
        .expect("encode authority phase");
    assert_eq!(
        decode_transcript(&bytes, BootstrapPhase::AuthorityVerified),
        Ok(authority)
    );
    let before = bytes;
    assert_eq!(
        encode_transcript(&authority, BootstrapPhase::CookieVerified, &mut bytes),
        Err(Error::Argument)
    );
    assert_eq!(bytes, before);
    assert_eq!(
        decode_transcript(&before, BootstrapPhase::DeviceVerified),
        Err(Error::Malformed)
    );
    let mut wrong_contract = authority;
    wrong_contract.bootstrap_header_contract = 2;
    assert_eq!(
        encode_transcript(
            &wrong_contract,
            BootstrapPhase::AuthorityVerified,
            &mut bytes
        ),
        Err(Error::Argument)
    );
    assert_eq!(bytes, before);

    let evidence = Evidence::new(&[9, 8, 7]).expect("evidence");
    let mut logical = [0x5A; 508];
    let length = encode_logical_event(
        BootstrapEvent::AuthorityProof,
        BootstrapPhase::AuthorityVerified,
        &authority,
        evidence,
        &mut logical,
    )
    .expect("logical encode");
    assert_eq!(length, TRANSCRIPT_BYTES + 4);
    assert_eq!(
        decode_logical_event(
            OPCODE_IDENTITY_CHALLENGE,
            BootstrapPhase::AuthorityVerified,
            &logical[..length]
        ),
        Ok((authority, evidence))
    );
    assert_eq!(
        decode_logical_event(
            OPCODE_DEVICE_COMMIT,
            BootstrapPhase::AuthorityVerified,
            &logical[..length]
        ),
        Err(Error::Malformed)
    );
}

#[test]
fn fragments_are_exact_and_reassembly_is_key_bound() {
    let (_, _, key) = hello_and_cookie();
    let value = transcript(BootstrapPhase::DeviceCommitted);
    let evidence = Evidence::new(&[0x77; 128]).expect("evidence");
    assert_eq!(Evidence::new(&[0x77; 129]), Err(Error::Argument));
    let mut logical = [0; 508];
    let logical_length = encode_logical_event(
        BootstrapEvent::DeviceCommit,
        BootstrapPhase::DeviceCommitted,
        &value,
        evidence,
        &mut logical,
    )
    .expect("logical encode");
    let mut reassembly = Reassembly::new();
    for index in 0..4 {
        let fragment = BootstrapFragment::from_logical(
            key,
            BootstrapFlow::Join,
            BootstrapEvent::DeviceCommit,
            BootstrapPhase::DeviceCommitted,
            index,
            &logical[..logical_length],
        )
        .expect("fragment");
        let mut encoded = [0xA5; FRAGMENT_HEADER_BYTES + 128];
        let length = encode_fragment(fragment, &mut encoded).expect("fragment encode");
        let decoded = decode_fragment(&encoded[..length]).expect("fragment decode");
        assert_eq!(decoded, fragment);
        let complete = reassembly
            .accept(OPCODE_DEVICE_COMMIT, key, 1_000, 100, decoded)
            .expect("accept fragment");
        assert_eq!(complete, index == 3);
    }
    assert_eq!(reassembly.borrow(), Ok(&logical[..logical_length]));
    let wrong_key = AdmissionKey {
        transaction_id: key.transaction_id + 1,
        ..key
    };
    let fragment = BootstrapFragment::from_logical(
        key,
        BootstrapFlow::Join,
        BootstrapEvent::DeviceCommit,
        BootstrapPhase::DeviceCommitted,
        0,
        &logical[..logical_length],
    )
    .expect("fragment");
    assert_eq!(
        reassembly.accept(OPCODE_DEVICE_COMMIT, wrong_key, 1_000, 100, fragment),
        Err(Error::Argument)
    );
    assert_eq!(reassembly.borrow(), Ok(&logical[..logical_length]));
}

#[test]
fn cookie_rate_limit_cannot_be_bypassed_and_duplicates_do_not_refresh() {
    let gate = CallbackGate::new(1).expect("gate");
    let mut owner = NanoAdmissionOwner::new(AdmissionConfig {
        runtime_instance: 1,
        owner_instance: 2,
        identity_owner_instance: 3,
        provider_gate: &gate,
        max_pending_per_link: 1,
        token_burst: 1,
        tokens_per_second: 1,
        pending_timeout_us: 1_000,
    })
    .expect("owner");
    let mut provider = Provider {
        issued: 0,
        verified: 0,
        authorized: 0,
    };
    let (hello, hello_cookie, key) = hello_and_cookie();
    let context = InitialHelloContext {
        link: key.link,
        now_us: 100,
        request_bytes: HELLO_BYTES,
        response_bytes: COOKIE_CHALLENGE_BYTES,
        cookie_time_bucket: 1,
    };
    owner
        .issue_cookie(&mut provider, hello, context)
        .expect("first cookie");
    assert_eq!(
        owner.issue_cookie(&mut provider, hello, context),
        Err(Error::Access)
    );
    assert_eq!(provider.issued, 1);

    let initial = transcript(BootstrapPhase::CookieVerified);
    let handle = owner
        .open_after_cookie(&mut provider, key, hello_cookie, initial, 200)
        .expect("open pending");
    let before = owner.pending_get(handle).expect("view");
    let duplicate = owner
        .open_after_cookie(&mut provider, key, hello_cookie, initial, 900)
        .expect("exact duplicate");
    assert_eq!(duplicate, handle);
    assert_eq!(owner.pending_get(handle), Ok(before));
    assert_eq!(provider.verified, 2);

    let out_of_order = transcript(BootstrapPhase::DeviceVerified);
    let proof = Evidence::new(&[1]).expect("evidence");
    assert_eq!(
        owner.advance(
            &mut provider,
            handle,
            BootstrapEvent::DeviceProof,
            out_of_order,
            proof,
            300
        ),
        Err(Error::State)
    );
    assert_eq!(owner.pending_get(handle), Ok(before));
    assert_eq!(provider.authorized, 0);

    let mut changed_history = transcript(BootstrapPhase::AuthorityVerified);
    changed_history.prior_messages_hash[0] ^= 1;
    assert_eq!(
        owner.advance(
            &mut provider,
            handle,
            BootstrapEvent::AuthorityProof,
            changed_history,
            proof,
            300
        ),
        Err(Error::Replay)
    );
    assert_eq!(owner.pending_get(handle), Ok(before));
    assert_eq!(provider.authorized, 0);

    assert_eq!(owner.expire(1_199), 0);
    assert_eq!(owner.expire(1_200), 1);
    assert_eq!(owner.expire(1_201), 0);
    assert_eq!(
        owner.advance(
            &mut provider,
            handle,
            BootstrapEvent::Abort,
            initial,
            proof,
            1_201
        ),
        Err(Error::State)
    );
    owner.retire(handle).expect("retire expired pending");
}

#[test]
fn expired_pending_is_not_lazily_evicted_when_capacity_is_full() {
    let gate = CallbackGate::new(1).expect("gate");
    let mut owner = NanoAdmissionOwner::new(AdmissionConfig {
        runtime_instance: 1,
        owner_instance: 2,
        identity_owner_instance: 3,
        provider_gate: &gate,
        max_pending_per_link: 1,
        token_burst: 1,
        tokens_per_second: 1,
        pending_timeout_us: 100,
    })
    .expect("owner");
    let mut provider = Provider {
        issued: 0,
        verified: 0,
        authorized: 0,
    };
    let (_, base_cookie, base_key) = hello_and_cookie();
    let mut handles = [None; 2];
    for index in 0..2_u32 {
        let identity = principal(0x21 + u8::try_from(index).expect("index"));
        let link = LinkIdentity {
            link_id: u16::try_from(index + 1).expect("link"),
            link_generation: 9,
        };
        let key = AdmissionKey {
            link,
            local_peer_discriminator: index + 1,
            identity_digest: identity,
            transaction_id: base_key.transaction_id + u64::from(index),
        };
        let mut cookie = base_cookie;
        cookie.identity_digest = identity;
        cookie.transaction_id = key.transaction_id;
        cookie.selected_link_instance_id = link.link_id;
        let mut initial = transcript(BootstrapPhase::CookieVerified);
        initial.joining_device_identity_digest = identity;
        initial.transaction_id = key.transaction_id;
        initial.selected_link_instance_id = link.link_id;
        handles[usize::try_from(index).expect("index")] = Some(
            owner
                .open_after_cookie(&mut provider, key, cookie, initial, 10)
                .expect("fill pending"),
        );
    }
    assert_eq!(owner.expire(110), 2);

    let identity = principal(0x31);
    let link = LinkIdentity {
        link_id: 3,
        link_generation: 9,
    };
    let key = AdmissionKey {
        link,
        local_peer_discriminator: 3,
        identity_digest: identity,
        transaction_id: base_key.transaction_id + 2,
    };
    let mut cookie = base_cookie;
    cookie.identity_digest = identity;
    cookie.transaction_id = key.transaction_id;
    cookie.selected_link_instance_id = link.link_id;
    let mut initial = transcript(BootstrapPhase::CookieVerified);
    initial.joining_device_identity_digest = identity;
    initial.transaction_id = key.transaction_id;
    initial.selected_link_instance_id = link.link_id;
    assert_eq!(
        owner.open_after_cookie(&mut provider, key, cookie, initial, 111),
        Err(Error::NoSpace)
    );
    for handle in handles.into_iter().flatten() {
        assert_eq!(
            owner.pending_get(handle).expect("old slot").phase,
            BootstrapPhase::Aborted
        );
    }
}
