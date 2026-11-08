//! Host-only permanent storage for actual Foundation-published Sessions.
#![cfg(feature = "native-backend")]
use ucn_durable::DurableContract;
use ucn_owner::CallbackGate;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_security::{
    AccessDirection, AclRule, CurrentFacts, DurabilityBase, Fingerprint, HandshakeCandidate,
    HandshakeProof, KeySelector, SESSION_OPERATION_KIND, SESSION_SCHEMA_ID, SESSION_SCHEMA_VERSION,
    SecurityConfig, SecurityLevel, SecurityOwner, SessionDomainRule, SessionHandle,
};
use ucn_types::{
    AddressWidth, HopProfile, KeyId, LinkInstanceGeneration, PeerSessionGeneration, RealmId,
    ServiceId, SuiteId,
};
#[path = "../../../../tests/support/security.rs"]
pub mod security;
mod test_cipher;
pub struct Fixture {
    pub owner: &'static mut SecurityOwner<'static, 2, 2>,
    pub handle: SessionHandle,
    pub facts: CurrentFacts,
    pub crypto: &'static mut test_cipher::TestCrypto,
}
fn candidate(local: u8, peer: u8, encrypted: bool) -> HandshakeCandidate {
    let key = |id| KeySelector {
        suite: if encrypted {
            SuiteId::OriginAes128Gcm
        } else {
            SuiteId::OriginHmacSha256_128
        },
        key_id: KeyId::new(id).unwrap(),
        key_generation: 1,
    };
    HandshakeCandidate {
        local: security::binding(local),
        peer: security::binding(peer),
        link_generation: LinkInstanceGeneration::new(1).unwrap(),
        session_generation: PeerSessionGeneration::new(1).unwrap(),
        policy_generation: 1,
        expires_at_us: 10000,
        origin_level: if encrypted {
            SecurityLevel::Confidential
        } else {
            SecurityLevel::Authenticated
        },
        hop_profile: HopProfile::H0,
        origin_tx: key(u16::from(local)),
        origin_rx: key(u16::from(peer)),
        hop_tx: None,
        hop_rx: None,
        origin_fingerprint: Fingerprint::new([5; 16]).unwrap(),
        hop_fingerprint: Fingerprint::new([5; 16]).unwrap(),
        transcript_digest: [1; 32],
    }
}
pub fn active(local: u8, peer: u8, encrypted: bool) -> Fixture {
    let c = candidate(local, peer, encrypted);
    let facts = CurrentFacts {
        local: c.local,
        peer: c.peer,
        link_generation: c.link_generation,
        policy_generation: 1,
        now_us: 21,
    };
    let contract = DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::SecurityHighWater,
            id: 1,
        },
        schema: SESSION_SCHEMA_ID,
        version: SESSION_SCHEMA_VERSION,
        operation: SESSION_OPERATION_KIND,
    };
    let gate = Box::leak(Box::new(CallbackGate::new(1).unwrap()));
    let rule = AclRule {
        peer_principal: c.peer.principal,
        peer_binding_generation: c.peer.generation,
        context_fingerprint: c.origin_fingerprint,
        service: ServiceId::new(0x40).unwrap(),
        protocol_opcode: 0,
        direction: AccessDirection::Inbound,
    };
    let rules = Box::leak(Box::new([
        rule,
        AclRule {
            direction: AccessDirection::Outbound,
            ..rule
        },
    ]));
    let domains = Box::leak(Box::new([SessionDomainRule {
        domain: contract.domain,
        peer_principal: c.peer.principal,
    }]));
    let owner = Box::leak(Box::new(
        SecurityOwner::new(SecurityConfig {
            runtime_instance: 1,
            owner_instance: 4,
            persistence_business_owner_instance: 2,
            persistence_owner_instance: 3,
            realm: RealmId::new(1).unwrap(),
            address_width: AddressWidth::A1,
            provider_gate: gate,
            session_domains: domains,
            replay_reservation_lifetime_us: 100,
            acl: rules,
        })
        .unwrap(),
    ));
    let crypto = Box::leak(Box::new(test_cipher::TestCrypto::new(c)));
    let mut handle = None;
    security::flash::exercise(contract, |p, flash| {
        let mut body = [0; 1024];
        let n = p.copy_body(contract.domain, &mut body).unwrap();
        let base = DurabilityBase::from_view(
            1,
            2,
            p.domain_get(contract.domain).unwrap(),
            &body[..n],
            10000,
            1,
            &mut CodecWorkspace::new(),
        )
        .unwrap();
        let (h, request) = owner
            .prepare_static_session(
                crypto,
                c,
                HandshakeProof {
                    bytes: b"exact-test-proof",
                },
                base,
                facts,
            )
            .unwrap();
        let (ph, proof) = security::flash::commit(p, flash, &request.persistence_request());
        owner.bind_persistence(h, ph).unwrap();
        owner.activate(h, ph, proof, facts).unwrap();
        p.proof_retire(ph).unwrap();
        handle = Some(h);
    });
    Fixture {
        owner,
        handle: handle.unwrap(),
        facts,
        crypto,
    }
}
