//! Test HMAC provider and real Session publication through Foundation.
use ucn_digest::{DigestWorkspace, sha256};
use ucn_durable::DurableContract;
use ucn_owner::CallbackGate;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_security::{
    AccessDirection, AclRule, Binding, CryptoProvider, CurrentFacts, DurabilityBase, Fingerprint,
    HandshakeCandidate, HandshakeProof, KeySelector, OpenOriginRequest, SESSION_OPERATION_KIND,
    SESSION_SCHEMA_ID, SESSION_SCHEMA_VERSION, SealOriginRequest, SecurityConfig, SecurityLevel,
    SecurityOwner, SessionDomainRule, SessionHandle,
};
use ucn_types::{
    AddressWidth, BindingGeneration, Error, HopProfile, KeyId, LinkInstanceGeneration, NodeAddress,
    PeerSessionGeneration, ProtocolOpcode, RealmId, Result, ServiceId, SuiteId,
};
#[allow(dead_code)]
#[path = "flash.rs"]
pub mod flash;
pub struct Crypto {
    expected: HandshakeCandidate,
}
impl Crypto {
    /// Host-only constructor for independently allocated integration fixtures.
    #[allow(dead_code)]
    pub const fn for_candidate(expected: HandshakeCandidate) -> Self {
        Self { expected }
    }
    // Hostile-peer fixture only: deliberately bypass an honest Sender counter to
    // test receiver replay rejection. Never part of any production API.
    // 仅恶意Peer测试：故意绕过诚实发送端计数器，接收端仍须真实验签并拒绝重放。
    #[allow(dead_code)] // Shared Host fixture is included by consumers without hostile-key tests.
    pub fn adversarial_o1_tag(&mut self, aad: &[u8], payload: &[u8], tag: &mut [u8; 16]) {
        self.compute_origin_tag(self.expected.origin_tx, aad, payload, tag)
            .unwrap();
    }
    #[allow(clippy::unused_self)] // Test provider method mirrors its stateful production counterpart.
    fn mac(&self, key: KeySelector, aad: &[u8], payload: &[u8]) -> Result<[u8; 32]> {
        if aad.len() + payload.len() > 960 {
            return Err(Error::NoSpace);
        }
        let mut bytes = [0; 1024];
        let key_byte = u8::try_from(key.key_id.get()).map_err(|_| Error::Security)?;
        bytes[..64].fill(key_byte ^ 0x36);
        bytes[64..64 + aad.len()].copy_from_slice(aad);
        bytes[64 + aad.len()..64 + aad.len() + payload.len()].copy_from_slice(payload);
        let inner = sha256(
            &bytes[..64 + aad.len() + payload.len()],
            &mut DigestWorkspace::new(),
        );
        bytes[..64].fill(key_byte ^ 0x5c);
        bytes[64..96].copy_from_slice(&inner);
        Ok(sha256(&bytes[..96], &mut DigestWorkspace::new()))
    }
}
impl CryptoProvider for Crypto {
    fn verify_session_proof(&mut self, c: &HandshakeCandidate, p: &[u8]) -> Result<()> {
        if c == &self.expected && p == b"exact-test-proof" {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }
    fn compute_origin_tag(
        &mut self,
        k: KeySelector,
        a: &[u8],
        p: &[u8],
        t: &mut [u8; 16],
    ) -> Result<()> {
        t.copy_from_slice(&self.mac(k, a, p)?[..16]);
        Ok(())
    }
    fn verify_origin_tag(
        &mut self,
        k: KeySelector,
        a: &[u8],
        p: &[u8],
        t: &[u8; 16],
    ) -> Result<()> {
        if self.mac(k, a, p)?[..16] == *t {
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
        t.copy_from_slice(&self.mac(k, a, &[])?[..12]);
        Ok(())
    }
    fn verify_hop_tag(&mut self, k: KeySelector, a: &[u8], t: &[u8; 12]) -> Result<()> {
        if self.mac(k, a, &[])?[..12] == *t {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }
}
pub fn binding(id: u8) -> Binding {
    Binding {
        address: NodeAddress::new(u32::from(id), AddressWidth::A1).unwrap(),
        generation: BindingGeneration::active(1).unwrap(),
        principal: [id; 16],
    }
}
#[allow(clippy::too_many_lines)] // One explicit proof/reload fixture, no production code.
#[allow(dead_code)] // Shared fixture also provides the opcode-specific entry point.
pub fn exercise(
    local: u8,
    peer: u8,
    callback: impl FnOnce(&mut SecurityOwner<'_, 2, 2>, SessionHandle, CurrentFacts, &mut Crypto),
) {
    exercise_opcodes(local, peer, &[], callback);
}

#[allow(clippy::too_many_lines)] // Real proof fixture; dynamic storage is Host-test-only.
pub fn exercise_opcodes(
    local: u8,
    peer: u8,
    opcodes: &[ProtocolOpcode],
    callback: impl FnOnce(&mut SecurityOwner<'_, 2, 2>, SessionHandle, CurrentFacts, &mut Crypto),
) {
    exercise_generations(local, peer, opcodes, 1, 1, 1, callback);
}

/// Actual reload-published session with selectable generations; TEST ONLY.
#[allow(clippy::too_many_arguments, clippy::too_many_lines)]
pub fn exercise_generations(
    local: u8,
    peer: u8,
    opcodes: &[ProtocolOpcode],
    session_generation: u32,
    key_generation: u32,
    policy_generation: u32,
    callback: impl FnOnce(&mut SecurityOwner<'_, 2, 2>, SessionHandle, CurrentFacts, &mut Crypto),
) {
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
    flash::exercise(contract, |p, f| {
        let gate = CallbackGate::new(1).unwrap();
        let fp = Fingerprint::new([5; 16]).unwrap();
        let mut rules = vec![
            AclRule {
                peer_principal: [peer; 16],
                peer_binding_generation: BindingGeneration::active(1).unwrap(),
                context_fingerprint: fp,
                service: ServiceId::new(0x40).unwrap(),
                protocol_opcode: 0,
                direction: AccessDirection::Inbound,
            },
            AclRule {
                direction: AccessDirection::Outbound,
                ..AclRule {
                    peer_principal: [peer; 16],
                    peer_binding_generation: BindingGeneration::active(1).unwrap(),
                    context_fingerprint: fp,
                    service: ServiceId::new(0x40).unwrap(),
                    protocol_opcode: 0,
                    direction: AccessDirection::Inbound,
                }
            },
            AclRule {
                peer_principal: [peer; 16],
                peer_binding_generation: BindingGeneration::active(1).unwrap(),
                context_fingerprint: fp,
                service: ServiceId::new(0x40).unwrap(),
                protocol_opcode: u16::from(ProtocolOpcode::DeliveryAck),
                direction: AccessDirection::Inbound,
            },
            AclRule {
                peer_principal: [peer; 16],
                peer_binding_generation: BindingGeneration::active(1).unwrap(),
                context_fingerprint: fp,
                service: ServiceId::new(0x40).unwrap(),
                protocol_opcode: u16::from(ProtocolOpcode::DeliveryAck),
                direction: AccessDirection::Outbound,
            },
        ];
        for opcode in opcodes {
            for direction in [AccessDirection::Inbound, AccessDirection::Outbound] {
                rules.push(AclRule {
                    peer_principal: [peer; 16],
                    peer_binding_generation: BindingGeneration::active(1).unwrap(),
                    context_fingerprint: fp,
                    service: ServiceId::new(0x40).unwrap(),
                    protocol_opcode: u16::from(*opcode),
                    direction,
                });
            }
        }
        let domains = [SessionDomainRule {
            domain: contract.domain,
            peer_principal: [peer; 16],
        }];
        let mut security = SecurityOwner::<2, 2>::new(SecurityConfig {
            runtime_instance: 1,
            owner_instance: 4,
            persistence_business_owner_instance: 2,
            persistence_owner_instance: 3,
            realm: RealmId::new(1).unwrap(),
            address_width: AddressWidth::A1,
            provider_gate: &gate,
            session_domains: &domains,
            replay_reservation_lifetime_us: 100,
            acl: &rules,
        })
        .unwrap();
        let select = |id| KeySelector {
            suite: SuiteId::OriginHmacSha256_128,
            key_id: KeyId::new(id).unwrap(),
            key_generation,
        };
        let candidate = HandshakeCandidate {
            local: binding(local),
            peer: binding(peer),
            link_generation: LinkInstanceGeneration::new(1).unwrap(),
            session_generation: PeerSessionGeneration::new(session_generation).unwrap(),
            policy_generation,
            expires_at_us: 10000,
            origin_level: SecurityLevel::Authenticated,
            hop_profile: HopProfile::H0,
            origin_tx: select(u16::from(local)),
            origin_rx: select(u16::from(peer)),
            hop_tx: None,
            hop_rx: None,
            origin_fingerprint: fp,
            hop_fingerprint: fp,
            transcript_digest: [3; 32],
        };
        let facts = CurrentFacts {
            local: candidate.local,
            peer: candidate.peer,
            link_generation: candidate.link_generation,
            policy_generation,
            now_us: 21,
        };
        let mut crypto = Crypto {
            expected: candidate,
        };
        let mut final_handle = None;
        for generation in 1..=session_generation {
            let mut c = candidate;
            c.session_generation = PeerSessionGeneration::new(generation).unwrap();
            c.transcript_digest = [u8::try_from(generation).unwrap(); 32];
            if generation != session_generation {
                c.origin_tx.key_generation = generation;
                c.origin_rx.key_generation = generation;
                c.policy_generation = generation;
            }
            let active_facts = CurrentFacts {
                policy_generation: c.policy_generation,
                ..facts
            };
            crypto.expected = c;
            let mut body = [0; 1024];
            let n = p.copy_body(contract.domain, &mut body).unwrap();
            let base = DurabilityBase::from_view(
                1,
                2,
                p.domain_get(contract.domain).unwrap(),
                &body[..n],
                10000,
                generation,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
            let (h, requirement) = security
                .prepare_static_session(
                    &mut crypto,
                    c,
                    HandshakeProof {
                        bytes: b"exact-test-proof",
                    },
                    base,
                    active_facts,
                )
                .unwrap();
            let (ph, proof) = flash::commit(p, f, &requirement.persistence_request());
            security.bind_persistence(h, ph).unwrap();
            security.activate(h, ph, proof, active_facts).unwrap();
            p.proof_retire(ph).unwrap();
            if generation != session_generation {
                security.fence(h).unwrap();
                security.retire_fenced(h).unwrap();
            }
            final_handle = Some(h);
        }
        let h = final_handle.unwrap();
        callback(&mut security, h, facts, &mut crypto);
    });
}
