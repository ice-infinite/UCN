#![allow(dead_code)] // Shared Host-only two-Domain raw media fixture.
//! Actual fixed Foundation with independent ID and Journal domains, no durable sidecar.
use crate::fixture::security::flash;
use ucn_durable::DurableContract;
use ucn_persistence::*;
use ucn_types::Error;
pub type Owner<'a> = PersistenceOwner<'a, 2, { flash::BODY }, { flash::SLOT }>;
pub struct Media {
    pub domains: [flash::Flash; 2],
    pending: [Option<(u64, usize)>; 2],
}
impl Media {
    fn index(&self, domain: DomainKey) -> usize {
        self.domains
            .iter()
            .position(|f| f.domain == domain)
            .unwrap()
    }
    fn remember(&mut self, index: usize, token: u64, start: IoStart) -> IoStart {
        if matches!(start, IoStart::Pending) {
            self.pending[index] = Some((token, index));
        }
        start
    }
}
impl PersistenceProvider for Media {
    fn geometry(&self) -> ProviderGeometry {
        self.domains[0].geometry()
    }
    fn begin_load_slot(&mut self, d: DomainKey, s: u8, out: &mut [u8], t: u64) -> IoStart {
        let index = self.index(d);
        let start = self.domains[index].begin_load_slot(d, s, out, t);
        self.remember(index, t, start)
    }
    fn begin_write_inactive(&mut self, d: DomainKey, s: u8, input: &[u8], t: u64) -> IoStart {
        let index = self.index(d);
        let start = self.domains[index].begin_write_inactive(d, s, input, t);
        self.remember(index, t, start)
    }
    fn begin_readback(&mut self, d: DomainKey, s: u8, out: &mut [u8], t: u64) -> IoStart {
        let index = self.index(d);
        let start = self.domains[index].begin_readback(d, s, out, t);
        self.remember(index, t, start)
    }
    fn begin_publish_marker(
        &mut self,
        d: DomainKey,
        s: u8,
        marker: &[u8; MARKER_BYTES],
        t: u64,
    ) -> IoStart {
        let index = self.index(d);
        let start = self.domains[index].begin_publish_marker(d, s, marker, t);
        self.remember(index, t, start)
    }
    fn begin_load_witness(&mut self, d: DomainKey, out: &mut WitnessView, t: u64) -> IoStart {
        let index = self.index(d);
        let start = self.domains[index].begin_load_witness(d, out, t);
        self.remember(index, t, start)
    }
    fn begin_advance_witness(&mut self, d: DomainKey, old: u64, new: u64, t: u64) -> IoStart {
        let index = self.index(d);
        let start = self.domains[index].begin_advance_witness(d, old, new, t);
        self.remember(index, t, start)
    }
    fn poll(
        &mut self,
        t: u64,
        phase: IoPhase,
        slot: Option<&mut [u8]>,
        witness: Option<&mut WitnessView>,
    ) -> IoStart {
        let Some(index) = self
            .pending
            .iter()
            .position(|v| v.is_some_and(|(token, _)| token == t))
        else {
            return IoStart::Failed(Error::State);
        };
        let start = self.domains[index].poll(t, phase, slot, witness);
        if !matches!(start, IoStart::Pending) {
            self.pending[index] = None;
        }
        start
    }
}
pub fn exercise(
    ids: DurableContract,
    journal: DurableContract,
    callback: impl FnOnce(&mut Owner<'_>, &mut Media),
) {
    let gate = ProviderGate::new();
    let contracts = [journal, ids]; // Canonical DomainKey ascending order.
    let entries = contracts.map(|c| ManifestEntry {
        domain: c.domain,
        body_capacity_bytes: u32::try_from(flash::BODY).unwrap(),
        slot_capacity_bytes: u32::try_from(flash::SLOT).unwrap(),
        schema_id: c.schema,
        schema_version: c.version,
        digest_suite: DIGEST_BLAKE2S_128,
        witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
        provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
    });
    let bindings = contracts.map(|c| DomainBinding {
        domain: c.domain,
        business_owner_instance: c.owner,
        domain_generation: 1,
    });
    let config = PersistenceConfig {
        runtime_instance: ids.runtime,
        owner_instance: ids.persistence_owner,
        required_domain_mask: 3,
        manifest: Manifest {
            protocol_manifest_version: 1,
            storage_layout_version: 1,
            composition_feature_bits: 0,
            profile_id: 1,
            entries: &entries,
        },
        bindings: &bindings,
        provider_gate: &gate,
    };
    let mut media = Media {
        domains: [ids, journal].map(|c| flash::Flash::new(c.domain)),
        pending: [None; 2],
    };
    let mut owner = Owner::uninit();
    owner.init(&config, &media).unwrap();
    owner.start_recovery().unwrap();
    for _ in 0..256 {
        owner.step(&mut media, 10, 1).unwrap();
        if owner.lifecycle() == Lifecycle::Ready {
            callback(&mut owner, &mut media);
            return;
        }
    }
    panic!("two-Domain recovery failed");
}
