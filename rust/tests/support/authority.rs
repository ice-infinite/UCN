//! Test-only REAL Identity -> REAL Foundation -> sealed `RealmAuthorityView` fixture.
use crate::flash;
use ucn_durable::DurableContract;
use ucn_identity::*;
use ucn_owner::CallbackGate;
use ucn_persistence::*;
use ucn_types::*;
struct Verifier;
impl IdentityVerifier for Verifier {
    fn verify_authority_transition(&mut self, t: &AuthorityTransition, p: &[u8]) -> Result<()> {
        if p == b"test-proof" && t.proposed.realm.get() == 1 {
            Ok(())
        } else {
            Err(Error::Security)
        }
    }
    fn verify_binding_issue(
        &mut self,
        _: &AuthorityEpoch,
        _: &BindingCertificate,
        _: &AuthorityFreshness,
        _: u64,
        _: u64,
        _: &[u8],
    ) -> Result<()> {
        Err(Error::Unsupported)
    }
}
pub fn exercise(callback: impl FnOnce(&IdentityOwner<'_, 1>)) {
    let contract = DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::IdentityBinding,
            id: 1,
        },
        schema: AUTHORITY_SCHEMA_ID,
        version: AUTHORITY_SCHEMA_VERSION,
        operation: AUTHORITY_OPERATION_KIND,
    };
    flash::exercise(contract, |p, f| {
        let gate = CallbackGate::new(1).unwrap();
        let domains = [DomainKey {
            kind: DomainKind::IdentityBinding,
            id: 2,
        }];
        let policy = LeasePolicy {
            local_timer_max_slow_ppm: 0,
            local_timer_resolution_us: 1,
            local_timer_read_uncertainty_us: 0,
            timer_read_uncertainty_known: true,
            local_policy_max_lease_us: 100_000,
        };
        let mut owner = IdentityOwner::<1>::new(IdentityConfig {
            runtime_instance: 1,
            owner_instance: 4,
            persistence_business_owner_instance: 2,
            persistence_owner_instance: 3,
            realm: RealmId::new(1).unwrap(),
            address_width: AddressWidth::A1,
            provider_gate: &gate,
            authority_domain: contract.domain,
            binding_domains: &domains,
            challenge_lifetime_us: 10000,
            authority_lease_policy: policy,
            binding_lease_policy: policy,
        })
        .unwrap();
        let principal = Principal::new([1; 16]).unwrap();
        let epoch = AuthorityEpoch {
            realm: RealmId::new(1).unwrap(),
            authority_principal: principal,
            authority_generation: AddressAuthorityGeneration::new(1).unwrap(),
            durable_fence_token: [2; 16],
            lease_sequence: 1,
            lease_duration_us: 50000,
            allocation_high_water_digest: [3; 16],
            quorum_config_digest: [4; 32],
            signer_set_digest: [5; 32],
            threshold_proof_digest: [6; 32],
            signer_count: 3,
            quorum_threshold: 2,
        };
        let freshness = AuthorityFreshness {
            verifier_principal: principal,
            challenge_nonce: 1,
            transaction_id: 1,
            authority_lease_sequence: 1,
            max_remaining_lease_us: 40000,
            binding_lease_id: [0; 16],
            binding_generation: 0,
            proof_transcript_hash: [7; 32],
        };
        let challenge = owner.begin_authority_challenge(principal, 1, 1, 1).unwrap();
        let base = IdentityDurabilityBase::from_view(
            1,
            2,
            p.domain_get(contract.domain).unwrap(),
            &[],
            10000,
            1,
            &mut CodecWorkspace::new(),
        )
        .unwrap();
        let (h, requirement) = owner
            .prepare_authority(
                &mut Verifier,
                challenge,
                AuthorityTransitionKind::Initial,
                epoch,
                freshness,
                IdentityProof {
                    bytes: b"test-proof",
                },
                base,
                2,
            )
            .unwrap();
        let (ph, proof) = flash::commit(p, f, &requirement.persistence_request());
        owner.bind_authority_persistence(h, ph).unwrap();
        owner.activate_authority(h, ph, proof, 21).unwrap();
        p.proof_retire(ph).unwrap();
        callback(&owner);
    });
}
