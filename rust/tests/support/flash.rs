//! Test-only real Foundation bridge and fault-injectable fixed Fake Flash.
use ucn_durable::{DurableBase, DurableContract};
use ucn_persistence::{
    ATOMIC_COMMIT_MARKER_16, BlobState, CodecWorkspace, Completion, DIGEST_BLAKE2S_128,
    DomainBinding, DomainKey, DomainState, ENVELOPE_BYTES, IoPhase, IoStart, Lifecycle,
    MARKER_BYTES, Manifest, ManifestEntry, PersistenceConfig, PersistenceHandle, PersistenceOwner,
    PersistenceProof, PersistenceProvider, PersistenceRequest, ProviderGate, ProviderGeometry,
    WITNESS_INDEPENDENT_MONOTONIC, WitnessView,
};
use ucn_types::{Error, Result};
pub const BODY: usize = 1024;
pub const SLOT: usize = BODY + ENVELOPE_BYTES + MARKER_BYTES;
pub type Owner<'a> = PersistenceOwner<'a, 1, BODY, SLOT>;
pub struct Flash {
    pub slots: [[u8; SLOT]; 2],
    pub witness: u64,
    pub domain: DomainKey,
    pub pending_once: bool,
    pending: Option<Completion>,
    pub calls: usize,
}
impl Flash {
    pub fn new(domain: DomainKey) -> Self {
        Self {
            slots: [[0xff; SLOT]; 2],
            witness: 0,
            domain,
            pending_once: false,
            pending: None,
            calls: 0,
        }
    }
    fn done(&mut self, token: u64, phase: IoPhase, bytes: u32, slot: u8) -> IoStart {
        self.calls += 1;
        let completion = Completion {
            io_token: token,
            result: Ok(()),
            exact_bytes: bytes,
            phase,
            blob_state: BlobState::Present,
            slot_index: slot,
        };
        if self.pending_once {
            self.pending_once = false;
            self.pending = Some(completion);
            IoStart::Pending
        } else {
            IoStart::Completed(completion)
        }
    }
}
impl PersistenceProvider for Flash {
    fn geometry(&self) -> ProviderGeometry {
        ProviderGeometry {
            minimum_write_alignment: 1,
            minimum_erase_alignment: 1,
            maximum_slot_bytes: u32::try_from(SLOT).unwrap(),
            atomic_marker_bytes: u16::try_from(MARKER_BYTES).unwrap(),
            erased_value: 0xff,
        }
    }
    fn begin_load_slot(&mut self, d: DomainKey, s: u8, out: &mut [u8], t: u64) -> IoStart {
        assert_eq!(d, self.domain);
        out.copy_from_slice(&self.slots[s as usize]);
        self.done(t, IoPhase::LoadSlot, u32::try_from(SLOT).unwrap(), s)
    }
    fn begin_write_inactive(&mut self, d: DomainKey, s: u8, input: &[u8], t: u64) -> IoStart {
        assert_eq!(d, self.domain);
        self.slots[s as usize].copy_from_slice(input);
        self.done(t, IoPhase::WriteInactive, u32::try_from(SLOT).unwrap(), s)
    }
    fn begin_readback(&mut self, d: DomainKey, s: u8, out: &mut [u8], t: u64) -> IoStart {
        assert_eq!(d, self.domain);
        out.copy_from_slice(&self.slots[s as usize]);
        self.done(t, IoPhase::Readback, u32::try_from(SLOT).unwrap(), s)
    }
    fn begin_publish_marker(
        &mut self,
        d: DomainKey,
        s: u8,
        marker: &[u8; MARKER_BYTES],
        t: u64,
    ) -> IoStart {
        assert_eq!(d, self.domain);
        self.slots[s as usize][SLOT - MARKER_BYTES..].copy_from_slice(marker);
        self.done(
            t,
            IoPhase::PublishMarker,
            u32::try_from(MARKER_BYTES).unwrap(),
            s,
        )
    }
    fn begin_load_witness(&mut self, d: DomainKey, out: &mut WitnessView, t: u64) -> IoStart {
        assert_eq!(d, self.domain);
        *out = WitnessView {
            domain: d,
            highest_maybe_published_generation: self.witness,
            state: BlobState::Present,
        };
        self.done(t, IoPhase::LoadWitness, 40, u8::MAX)
    }
    fn begin_advance_witness(&mut self, d: DomainKey, old: u64, new: u64, t: u64) -> IoStart {
        assert_eq!(d, self.domain);
        if self.witness != old || new != old.checked_add(1).unwrap() {
            return IoStart::Failed(Error::State);
        }
        self.witness = new;
        self.done(t, IoPhase::AdvanceWitness, 8, u8::MAX)
    }
    fn poll(
        &mut self,
        t: u64,
        phase: IoPhase,
        slot_output: Option<&mut [u8]>,
        witness_output: Option<&mut WitnessView>,
    ) -> IoStart {
        let Some(completion) = self.pending.take() else {
            return IoStart::Failed(Error::State);
        };
        assert_eq!(t, completion.io_token);
        assert_eq!(phase, completion.phase);
        if matches!(phase, IoPhase::LoadSlot | IoPhase::Readback) {
            slot_output
                .unwrap()
                .copy_from_slice(&self.slots[completion.slot_index as usize]);
        }
        if phase == IoPhase::LoadWitness {
            *witness_output.unwrap() = WitnessView {
                domain: self.domain,
                highest_maybe_published_generation: self.witness,
                state: BlobState::Present,
            };
        }
        IoStart::Completed(completion)
    }
}
pub fn exercise(contract: DurableContract, callback: impl FnOnce(&mut Owner<'_>, &mut Flash)) {
    let mut flash = Flash::new(contract.domain);
    reopen(contract, &mut flash, callback);
}
#[allow(dead_code)] // Reconstruct Foundation from actual retained media in crash tests.
pub fn reopen(
    contract: DurableContract,
    flash: &mut Flash,
    callback: impl FnOnce(&mut Owner<'_>, &mut Flash),
) {
    let gate = ProviderGate::new();
    let entry = ManifestEntry {
        domain: contract.domain,
        body_capacity_bytes: u32::try_from(BODY).unwrap(),
        slot_capacity_bytes: u32::try_from(SLOT).unwrap(),
        schema_id: contract.schema,
        schema_version: contract.version,
        digest_suite: DIGEST_BLAKE2S_128,
        witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
        provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
    };
    let binding = [DomainBinding {
        domain: contract.domain,
        business_owner_instance: contract.owner,
        domain_generation: 1,
    }];
    let config = PersistenceConfig {
        runtime_instance: contract.runtime,
        owner_instance: contract.persistence_owner,
        required_domain_mask: 1,
        manifest: Manifest {
            protocol_manifest_version: 1,
            storage_layout_version: 1,
            composition_feature_bits: 0,
            profile_id: 1,
            entries: core::slice::from_ref(&entry),
        },
        bindings: &binding,
        provider_gate: &gate,
    };
    let mut owner = Owner::uninit();
    owner.init(&config, flash).unwrap();
    owner.start_recovery().unwrap();
    drive(&mut owner, flash);
    callback(&mut owner, flash);
}
pub fn drive(owner: &mut Owner<'_>, flash: &mut Flash) {
    for _ in 0..128 {
        owner.step(flash, 10, 1).unwrap();
        if owner.lifecycle() == Lifecycle::Ready
            && owner.domain_get(flash.domain).unwrap().state == DomainState::Ready
        {
            return;
        }
    }
    panic!("recovery did not converge");
}
pub fn base(owner: &Owner<'_>, contract: DurableContract) -> DurableBase {
    let view = owner.domain_get(contract.domain).unwrap();
    let mut body = [0; BODY];
    let bytes = owner.copy_body(contract.domain, &mut body).unwrap();
    DurableBase::from_view(contract, view, &body[..bytes], &mut CodecWorkspace::new()).unwrap()
}
pub fn commit(
    owner: &mut Owner<'_>,
    flash: &mut Flash,
    request: &PersistenceRequest<'_>,
) -> (PersistenceHandle, PersistenceProof) {
    let h = owner.submit(request, 20).unwrap();
    for _ in 0..128 {
        owner.step(flash, 20, 1).unwrap();
        if let Ok(proof) = owner.proof_get(h) {
            return (h, proof);
        }
    }
    panic!("commit did not converge");
}
#[allow(dead_code)]
pub fn no_error(result: Result<()>) {
    result.unwrap();
}
