//! Persistence Foundation 的同步、异步、掉电窗口和恢复对抗测试。

use ucn_persistence::{
    ATOMIC_COMMIT_MARKER_16, BlobState, CodecWorkspace, Completion, DIGEST_BLAKE2S_128,
    DIGEST_BYTES, DomainBinding, DomainKey, DomainKind, DomainState, ENVELOPE_BYTES, IoPhase,
    IoStart, Lifecycle, MARKER_BYTES, Manifest, ManifestEntry, PersistenceConfig, PersistenceOwner,
    PersistenceProvider, PersistenceRequest, ProviderGate, ProviderGeometry, RequestState,
    WITNESS_INDEPENDENT_MONOTONIC, WitnessView, encode_marker, encode_record, manifest_digest,
};
use ucn_types::Error;

const BODY: usize = 64;
const SLOT: usize = ENVELOPE_BYTES + BODY + MARKER_BYTES;
const DOMAIN: DomainKey = DomainKey {
    kind: DomainKind::ProductConfig,
    id: 1,
};
const SECONDARY_DOMAIN: DomainKey = DomainKey {
    kind: DomainKind::ProductConfig,
    id: 2,
};

type Owner<'a> = PersistenceOwner<'a, 1, BODY, SLOT>;

#[derive(Clone, Copy)]
struct PendingCompletion {
    completion: Completion,
}

struct FakeProvider {
    slots: [[u8; SLOT]; 2],
    secondary_slots: [[u8; SLOT]; 2],
    witness: u64,
    secondary_witness: u64,
    pending_mask: u8,
    pending_seen: u8,
    pending: Option<PendingCompletion>,
    calls: u32,
    corrupt_readback: bool,
    completion_fault: Option<CompletionFault>,
    fail_phase: Option<IoPhase>,
    fail_on_occurrence: u8,
    phase_occurrences: [u8; 7],
    allow_secondary_domain: bool,
    geometry_override: Option<ProviderGeometry>,
}

#[derive(Clone, Copy)]
enum CompletionFault {
    Token,
    Phase,
    Bytes,
    Blob,
    Slot,
}

impl FakeProvider {
    fn new() -> Self {
        Self {
            slots: [[0xFF; SLOT]; 2],
            secondary_slots: [[0xFF; SLOT]; 2],
            witness: 0,
            secondary_witness: 0,
            pending_mask: 0,
            pending_seen: 0,
            pending: None,
            calls: 0,
            corrupt_readback: false,
            completion_fault: None,
            fail_phase: None,
            fail_on_occurrence: 0,
            phase_occurrences: [0; 7],
            allow_secondary_domain: false,
            geometry_override: None,
        }
    }

    fn all_async() -> Self {
        Self {
            pending_mask: 0x7E,
            ..Self::new()
        }
    }

    fn complete(
        &mut self,
        token: u64,
        phase: IoPhase,
        exact_bytes: u32,
        blob_state: BlobState,
        slot_index: u8,
    ) -> IoStart {
        self.calls += 1;
        let bit = 1_u8 << phase as u8;
        let mut completion = Completion {
            io_token: token,
            result: Ok(()),
            exact_bytes,
            phase,
            blob_state,
            slot_index,
        };
        match self.completion_fault {
            Some(CompletionFault::Token) => completion.io_token = token.saturating_add(1),
            Some(CompletionFault::Phase) => {
                completion.phase = if phase == IoPhase::LoadSlot {
                    IoPhase::WriteInactive
                } else {
                    IoPhase::LoadSlot
                };
            }
            Some(CompletionFault::Bytes) => completion.exact_bytes = exact_bytes.saturating_sub(1),
            Some(CompletionFault::Blob) => completion.blob_state = BlobState::Fault,
            Some(CompletionFault::Slot) => completion.slot_index ^= 1,
            None => {}
        }
        if self.pending_mask & bit != 0 && self.pending_seen & bit == 0 {
            self.pending_seen |= bit;
            self.pending = Some(PendingCompletion { completion });
            IoStart::Pending
        } else {
            IoStart::Completed(completion)
        }
    }

    fn accepts_domain(&self, domain: DomainKey) -> bool {
        domain == DOMAIN || (self.allow_secondary_domain && domain == SECONDARY_DOMAIN)
    }

    fn fail_now(&mut self, phase: IoPhase) -> bool {
        let index = phase as usize;
        self.phase_occurrences[index] = self.phase_occurrences[index].saturating_add(1);
        self.fail_phase == Some(phase) && self.phase_occurrences[index] == self.fail_on_occurrence
    }

    fn failed_start(&mut self) -> IoStart {
        self.calls = self.calls.saturating_add(1);
        IoStart::Failed(Error::InDoubt)
    }
}

impl PersistenceProvider for FakeProvider {
    fn geometry(&self) -> ProviderGeometry {
        self.geometry_override.unwrap_or(ProviderGeometry {
            minimum_write_alignment: 8,
            minimum_erase_alignment: 8,
            maximum_slot_bytes: 176,
            atomic_marker_bytes: 16,
            erased_value: 0xFF,
        })
    }

    fn begin_load_slot(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        output: &mut [u8],
        io_token: u64,
    ) -> IoStart {
        if !self.accepts_domain(domain) || slot_index > 1 || output.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        if self.fail_now(IoPhase::LoadSlot) {
            return self.failed_start();
        }
        if domain == DOMAIN {
            output.copy_from_slice(&self.slots[slot_index as usize]);
        } else {
            output.copy_from_slice(&self.secondary_slots[slot_index as usize]);
        }
        self.complete(
            io_token,
            IoPhase::LoadSlot,
            176,
            BlobState::Present,
            slot_index,
        )
    }

    fn begin_write_inactive(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        input: &[u8],
        io_token: u64,
    ) -> IoStart {
        if !self.accepts_domain(domain) || slot_index > 1 || input.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        if self.fail_now(IoPhase::WriteInactive) {
            return self.failed_start();
        }
        if domain == DOMAIN {
            self.slots[slot_index as usize].copy_from_slice(input);
        } else {
            self.secondary_slots[slot_index as usize].copy_from_slice(input);
        }
        self.complete(
            io_token,
            IoPhase::WriteInactive,
            176,
            BlobState::Present,
            slot_index,
        )
    }

    fn begin_readback(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        output: &mut [u8],
        io_token: u64,
    ) -> IoStart {
        if !self.accepts_domain(domain) || slot_index > 1 || output.len() != SLOT {
            return IoStart::Failed(Error::Argument);
        }
        if self.fail_now(IoPhase::Readback) {
            return self.failed_start();
        }
        if domain == DOMAIN {
            output.copy_from_slice(&self.slots[slot_index as usize]);
        } else {
            output.copy_from_slice(&self.secondary_slots[slot_index as usize]);
        }
        if self.corrupt_readback {
            output[0] ^= 1;
        }
        self.complete(
            io_token,
            IoPhase::Readback,
            176,
            BlobState::Present,
            slot_index,
        )
    }

    fn begin_publish_marker(
        &mut self,
        domain: DomainKey,
        slot_index: u8,
        marker: &[u8; MARKER_BYTES],
        io_token: u64,
    ) -> IoStart {
        if !self.accepts_domain(domain) || slot_index > 1 {
            return IoStart::Failed(Error::Argument);
        }
        if self.fail_now(IoPhase::PublishMarker) {
            return self.failed_start();
        }
        if domain == DOMAIN {
            self.slots[slot_index as usize][SLOT - MARKER_BYTES..].copy_from_slice(marker);
        } else {
            self.secondary_slots[slot_index as usize][SLOT - MARKER_BYTES..]
                .copy_from_slice(marker);
        }
        self.complete(
            io_token,
            IoPhase::PublishMarker,
            16,
            BlobState::Present,
            slot_index,
        )
    }

    fn begin_load_witness(
        &mut self,
        domain: DomainKey,
        output: &mut WitnessView,
        io_token: u64,
    ) -> IoStart {
        if !self.accepts_domain(domain) {
            return IoStart::Failed(Error::Argument);
        }
        if self.fail_now(IoPhase::LoadWitness) {
            return self.failed_start();
        }
        *output = WitnessView {
            domain,
            highest_maybe_published_generation: if domain == DOMAIN {
                self.witness
            } else {
                self.secondary_witness
            },
            state: BlobState::Present,
        };
        self.complete(
            io_token,
            IoPhase::LoadWitness,
            40,
            BlobState::Present,
            u8::MAX,
        )
    }

    fn begin_advance_witness(
        &mut self,
        domain: DomainKey,
        expected_old: u64,
        exact_new: u64,
        io_token: u64,
    ) -> IoStart {
        if !self.accepts_domain(domain)
            || (domain == DOMAIN && self.witness != expected_old)
            || (domain == SECONDARY_DOMAIN && self.secondary_witness != expected_old)
            || exact_new != expected_old + 1
        {
            return IoStart::Failed(Error::State);
        }
        if self.fail_now(IoPhase::AdvanceWitness) {
            return self.failed_start();
        }
        if domain == DOMAIN {
            self.witness = exact_new;
        } else {
            self.secondary_witness = exact_new;
        }
        self.complete(
            io_token,
            IoPhase::AdvanceWitness,
            8,
            BlobState::Present,
            u8::MAX,
        )
    }

    fn poll(
        &mut self,
        io_token: u64,
        expected_phase: IoPhase,
        slot_output: Option<&mut [u8]>,
        witness_output: Option<&mut WitnessView>,
    ) -> IoStart {
        self.calls += 1;
        let Some(pending) = self.pending else {
            return IoStart::Failed(Error::NotFound);
        };
        if pending.completion.io_token != io_token || pending.completion.phase != expected_phase {
            return IoStart::Failed(Error::State);
        }
        match expected_phase {
            IoPhase::LoadSlot => {
                let Some(output) = slot_output else {
                    return IoStart::Failed(Error::Argument);
                };
                output.copy_from_slice(&self.slots[pending.completion.slot_index as usize]);
            }
            IoPhase::Readback => {
                let Some(output) = slot_output else {
                    return IoStart::Failed(Error::Argument);
                };
                output.copy_from_slice(&self.slots[pending.completion.slot_index as usize]);
                if self.corrupt_readback {
                    output[0] ^= 1;
                }
            }
            IoPhase::LoadWitness => {
                let Some(output) = witness_output else {
                    return IoStart::Failed(Error::Argument);
                };
                *output = WitnessView {
                    domain: DOMAIN,
                    highest_maybe_published_generation: self.witness,
                    state: BlobState::Present,
                };
            }
            _ => {}
        }
        self.pending = None;
        IoStart::Completed(pending.completion)
    }
}

fn entry() -> ManifestEntry {
    ManifestEntry {
        domain: DOMAIN,
        body_capacity_bytes: 64,
        slot_capacity_bytes: 176,
        schema_id: 1,
        schema_version: 1,
        digest_suite: DIGEST_BLAKE2S_128,
        witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
        provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
    }
}

fn initialize<'a>(
    owner: &mut Owner<'a>,
    provider: &FakeProvider,
    gate: &'a ProviderGate,
    manifest_entry: &'a ManifestEntry,
    bindings: &'a [DomainBinding; 1],
) {
    let config = PersistenceConfig {
        runtime_instance: 11,
        owner_instance: 12,
        required_domain_mask: 1,
        manifest: Manifest {
            protocol_manifest_version: 1,
            storage_layout_version: 1,
            composition_feature_bits: 0,
            profile_id: 3,
            entries: core::slice::from_ref(manifest_entry),
        },
        bindings,
        provider_gate: gate,
    };
    owner.init(&config, provider).unwrap();
    owner.start_recovery().unwrap();
}

fn drive_ready(owner: &mut Owner<'_>, provider: &mut FakeProvider) {
    for _ in 0..64 {
        if owner.lifecycle() == Lifecycle::Ready {
            return;
        }
        owner.step(provider, 10, 1).unwrap();
    }
    panic!("recovery did not converge");
}

fn request(
    body: &[u8],
    view_digest: [u8; DIGEST_BYTES],
    generation: u64,
    tx: u64,
) -> PersistenceRequest<'_> {
    PersistenceRequest {
        runtime_instance: 11,
        caller_owner_instance: 21,
        domain_generation: 31,
        domain: DOMAIN,
        foundation_transaction_id: tx,
        expected_record_generation: generation,
        absolute_deadline_us: 1_000,
        business_transition_digest: 0x1234,
        canonical_body: body,
        schema_id: 1,
        schema_version: 1,
        operation_kind: 2,
        expected_body_digest: view_digest,
        volatile_continuation: 41,
    }
}

fn submit_and_drive(
    owner: &mut Owner<'_>,
    provider: &mut FakeProvider,
    body: &[u8],
    tx: u64,
) -> ucn_persistence::PersistenceHandle {
    let view = owner.domain_get(DOMAIN).unwrap();
    let handle = owner
        .submit(
            &request(body, view.body_digest, view.record_generation, tx),
            20,
        )
        .unwrap();
    for _ in 0..128 {
        if owner.request_get(handle).unwrap().state == RequestState::ProofReady {
            return handle;
        }
        owner.step(provider, 30, 1).unwrap();
    }
    panic!("submit did not converge");
}

fn encode_committed(
    provider: &mut FakeProvider,
    manifest_entry: &ManifestEntry,
    slot_index: usize,
    generation: u64,
    transaction_id: u64,
    body: &[u8],
) {
    let manifest = Manifest {
        protocol_manifest_version: 1,
        storage_layout_version: 1,
        composition_feature_bits: 0,
        profile_id: 3,
        entries: core::slice::from_ref(manifest_entry),
    };
    let mut workspace = CodecWorkspace::new();
    let digest = manifest_digest::<BODY, SLOT>(&manifest, &mut workspace).unwrap();
    let meta = ucn_persistence::RecordMeta {
        domain: DOMAIN,
        record_generation: generation,
        transaction_id,
        body_bytes: u32::try_from(body.len()).unwrap_or(u32::MAX),
        schema_id: 1,
        schema_version: 1,
        operation_kind: 2,
        body_digest: [0; DIGEST_BYTES],
    };
    encode_record::<BODY, SLOT>(
        &meta,
        &digest,
        body,
        manifest_entry,
        0xFF,
        &mut provider.slots[slot_index],
        &mut workspace,
    )
    .unwrap();
    let mut marker = [0; MARKER_BYTES];
    encode_marker(generation, &mut marker).unwrap();
    provider.slots[slot_index][SLOT - MARKER_BYTES..].copy_from_slice(&marker);
}

#[test]
fn synchronous_commit_reloads_and_exact_replay_uses_no_provider_io() {
    let gate = ProviderGate::new();
    let manifest_entry = entry();
    let bindings = [DomainBinding {
        domain: DOMAIN,
        business_owner_instance: 21,
        domain_generation: 31,
    }];
    let mut provider = FakeProvider::new();
    let mut owner = Owner::uninit();
    initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
    drive_ready(&mut owner, &mut provider);
    let handle = submit_and_drive(&mut owner, &mut provider, &[7, 8, 9], 1);
    let proof = owner.proof_get(handle).unwrap();
    assert_eq!(proof.record_generation, 1);
    assert_eq!(proof.witness_generation, 1);
    assert_eq!(provider.witness, 1);
    owner.proof_retire(handle).unwrap();

    let reboot_gate = ProviderGate::new();
    let mut rebooted = Owner::uninit();
    initialize(
        &mut rebooted,
        &provider,
        &reboot_gate,
        &manifest_entry,
        &bindings,
    );
    drive_ready(&mut rebooted, &mut provider);
    let view = rebooted.domain_get(DOMAIN).unwrap();
    assert_eq!(view.record_generation, 1);
    let calls_before = provider.calls;
    let replay = rebooted
        .submit(&request(&[7, 8, 9], view.body_digest, 1, 1), 40)
        .unwrap();
    assert_eq!(
        rebooted.request_get(replay).unwrap().state,
        RequestState::ProofReady
    );
    assert_eq!(provider.calls, calls_before);
}

#[test]
fn every_provider_phase_can_complete_asynchronously_with_exact_token() {
    let gate = ProviderGate::new();
    let manifest_entry = entry();
    let bindings = [DomainBinding {
        domain: DOMAIN,
        business_owner_instance: 21,
        domain_generation: 31,
    }];
    let mut provider = FakeProvider::all_async();
    let mut owner = Owner::uninit();
    initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
    drive_ready(&mut owner, &mut provider);
    let handle = submit_and_drive(&mut owner, &mut provider, &[1, 2], 1);
    assert_eq!(owner.proof_get(handle).unwrap().body_bytes, 2);
    assert_eq!(provider.pending_seen & 0x7E, 0x7E);
}

#[test]
fn missing_predecessor_and_non_monotonic_transactions_fail_closed() {
    for previous_tx in [None, Some(20), Some(21)] {
        let gate = ProviderGate::new();
        let manifest_entry = entry();
        let bindings = [DomainBinding {
            domain: DOMAIN,
            business_owner_instance: 21,
            domain_generation: 31,
        }];
        let mut provider = FakeProvider::new();
        provider.witness = 1;
        if let Some(tx) = previous_tx {
            encode_committed(&mut provider, &manifest_entry, 0, 1, tx, &[1]);
        }
        encode_committed(&mut provider, &manifest_entry, 1, 2, 20, &[2]);
        let witness_before = provider.witness;
        let mut owner = Owner::uninit();
        initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
        for _ in 0..16 {
            if owner.lifecycle() == Lifecycle::Fault {
                break;
            }
            let _ = owner.step(&mut provider, 10, 1);
        }
        assert_eq!(owner.lifecycle(), Lifecycle::Fault);
        assert_eq!(owner.domain_get(DOMAIN).unwrap().state, DomainState::Fault);
        assert_eq!(provider.witness, witness_before);
    }
}

#[test]
fn factory_successor_and_legal_adjacent_successor_repair_witness() {
    let manifest_entry = entry();
    for with_predecessor in [false, true] {
        let gate = ProviderGate::new();
        let bindings = [DomainBinding {
            domain: DOMAIN,
            business_owner_instance: 21,
            domain_generation: 31,
        }];
        let mut provider = FakeProvider::new();
        if with_predecessor {
            provider.witness = 1;
            encode_committed(&mut provider, &manifest_entry, 0, 1, 10, &[1]);
            encode_committed(&mut provider, &manifest_entry, 1, 2, 11, &[2]);
        } else {
            encode_committed(&mut provider, &manifest_entry, 0, 1, 10, &[1]);
        }
        let mut owner = Owner::uninit();
        initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
        drive_ready(&mut owner, &mut provider);
        let expected = if with_predecessor { 2 } else { 1 };
        assert_eq!(provider.witness, expected);
        assert_eq!(
            owner.domain_get(DOMAIN).unwrap().record_generation,
            expected
        );
    }
}

#[test]
fn torn_marker_corrupt_latest_and_wrong_completion_never_publish_body() {
    let manifest_entry = entry();
    for scenario in 0..3 {
        let gate = ProviderGate::new();
        let bindings = [DomainBinding {
            domain: DOMAIN,
            business_owner_instance: 21,
            domain_generation: 31,
        }];
        let mut provider = FakeProvider::new();
        match scenario {
            0 => provider.slots[0][SLOT - MARKER_BYTES] = 0,
            1 => {
                provider.witness = 1;
                encode_committed(&mut provider, &manifest_entry, 0, 1, 10, &[1]);
                provider.slots[0][96] ^= 1;
            }
            _ => provider.completion_fault = Some(CompletionFault::Token),
        }
        let mut owner = Owner::uninit();
        initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
        for _ in 0..16 {
            if owner.lifecycle() == Lifecycle::Fault {
                break;
            }
            let _ = owner.step(&mut provider, 10, 1);
        }
        assert_eq!(owner.lifecycle(), Lifecycle::Fault);
        let mut output = [0xA5; BODY];
        assert_eq!(owner.copy_body(DOMAIN, &mut output), Err(Error::State));
        assert_eq!(output, [0xA5; BODY]);
    }
}

#[test]
fn pre_marker_deadline_and_readback_mismatch_do_not_create_proof() {
    let gate = ProviderGate::new();
    let manifest_entry = entry();
    let bindings = [DomainBinding {
        domain: DOMAIN,
        business_owner_instance: 21,
        domain_generation: 31,
    }];
    let mut provider = FakeProvider::new();
    let mut owner = Owner::uninit();
    initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
    drive_ready(&mut owner, &mut provider);
    let view = owner.domain_get(DOMAIN).unwrap();
    let mut timed = request(&[3], view.body_digest, 0, 1);
    timed.absolute_deadline_us = 21;
    let handle = owner.submit(&timed, 20).unwrap();
    owner.step(&mut provider, 20, 1).unwrap();
    let calls_before = provider.calls;
    owner.step(&mut provider, 21, 1).unwrap();
    assert_eq!(
        owner.request_get(handle).unwrap().state,
        RequestState::Failed
    );
    assert_eq!(
        owner.request_get(handle).unwrap().terminal_error,
        Some(Error::Timeout)
    );
    assert_eq!(provider.calls, calls_before);
    owner.failed_retire(handle).unwrap();

    provider.corrupt_readback = true;
    let handle = owner
        .submit(&request(&[4], view.body_digest, 0, 2), 30)
        .unwrap();
    for _ in 0..8 {
        if owner.lifecycle() == Lifecycle::Fault {
            break;
        }
        let _ = owner.step(&mut provider, 31, 1);
    }
    assert_eq!(owner.lifecycle(), Lifecycle::Fault);
    assert_eq!(
        owner.request_get(handle).unwrap().state,
        RequestState::Failed
    );
    assert!(owner.proof_get(handle).is_err());
    assert_eq!(provider.witness, 0);
}

#[test]
fn invalid_submit_is_zero_io_and_zero_pending_state() {
    let gate = ProviderGate::new();
    let manifest_entry = entry();
    let bindings = [DomainBinding {
        domain: DOMAIN,
        business_owner_instance: 21,
        domain_generation: 31,
    }];
    let mut provider = FakeProvider::new();
    let mut owner = Owner::uninit();
    initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
    drive_ready(&mut owner, &mut provider);
    let calls_before = provider.calls;
    let view_before = owner.domain_get(DOMAIN).unwrap();
    let mut invalid = request(&[1], view_before.body_digest, 0, 1);
    invalid.caller_owner_instance = 99;
    assert_eq!(owner.submit(&invalid, 20), Err(Error::State));
    assert_eq!(provider.calls, calls_before);
    assert_eq!(owner.domain_get(DOMAIN).unwrap(), view_before);
}

#[test]
fn provider_failures_at_each_power_cut_boundary_recover_conservatively() {
    for (phase, fail_on_occurrence, expected_generation) in [
        (IoPhase::WriteInactive, 1, 0),
        (IoPhase::Readback, 1, 0),
        (IoPhase::PublishMarker, 1, 0),
        (IoPhase::AdvanceWitness, 1, 1),
        (IoPhase::LoadWitness, 2, 1),
    ] {
        let gate = ProviderGate::new();
        let manifest_entry = entry();
        let bindings = [DomainBinding {
            domain: DOMAIN,
            business_owner_instance: 21,
            domain_generation: 31,
        }];
        let mut provider = FakeProvider::new();
        provider.fail_phase = Some(phase);
        provider.fail_on_occurrence = fail_on_occurrence;
        let mut owner = Owner::uninit();
        initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
        drive_ready(&mut owner, &mut provider);
        let view = owner.domain_get(DOMAIN).unwrap();
        let handle = owner
            .submit(&request(&[0xA5], view.body_digest, 0, 1), 20)
            .unwrap();
        for _ in 0..16 {
            if owner.lifecycle() == Lifecycle::Fault {
                break;
            }
            owner.step(&mut provider, 21, 1).unwrap();
        }
        assert_eq!(owner.lifecycle(), Lifecycle::Fault);
        assert_eq!(
            owner.request_get(handle).unwrap().terminal_error,
            Some(Error::InDoubt)
        );

        provider.fail_phase = None;
        let reboot_gate = ProviderGate::new();
        let mut rebooted = Owner::uninit();
        initialize(
            &mut rebooted,
            &provider,
            &reboot_gate,
            &manifest_entry,
            &bindings,
        );
        drive_ready(&mut rebooted, &mut provider);
        let recovered = rebooted.domain_get(DOMAIN).unwrap();
        assert_eq!(recovered.record_generation, expected_generation);
        if expected_generation == 0 {
            assert_eq!(recovered.body_bytes, 0);
            assert_eq!(provider.witness, 0);
        } else {
            let mut body = [0; BODY];
            assert_eq!(rebooted.copy_body(DOMAIN, &mut body).unwrap(), 1);
            assert_eq!(body[0], 0xA5);
            assert_eq!(provider.witness, 1);
        }
    }
}

#[test]
fn every_completion_field_is_bound_before_state_changes() {
    for fault in [
        CompletionFault::Token,
        CompletionFault::Phase,
        CompletionFault::Bytes,
        CompletionFault::Blob,
        CompletionFault::Slot,
    ] {
        let gate = ProviderGate::new();
        let manifest_entry = entry();
        let bindings = [DomainBinding {
            domain: DOMAIN,
            business_owner_instance: 21,
            domain_generation: 31,
        }];
        let mut provider = FakeProvider::new();
        provider.completion_fault = Some(fault);
        let witness_before = provider.witness;
        let mut owner = Owner::uninit();
        initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
        owner.step(&mut provider, 10, 1).unwrap();
        assert_eq!(owner.lifecycle(), Lifecycle::Fault);
        assert_eq!(owner.domain_get(DOMAIN).unwrap().state, DomainState::Fault);
        assert_eq!(provider.witness, witness_before);
    }
}

#[test]
fn optional_domain_fault_does_not_publish_or_block_required_domain() {
    type TwoDomainOwner<'a> = PersistenceOwner<'a, 2, BODY, SLOT>;

    let gate = ProviderGate::new();
    let manifest_entries = [
        entry(),
        ManifestEntry {
            domain: SECONDARY_DOMAIN,
            ..entry()
        },
    ];
    let bindings = [
        DomainBinding {
            domain: DOMAIN,
            business_owner_instance: 21,
            domain_generation: 31,
        },
        DomainBinding {
            domain: SECONDARY_DOMAIN,
            business_owner_instance: 22,
            domain_generation: 32,
        },
    ];
    let config = PersistenceConfig {
        runtime_instance: 11,
        owner_instance: 12,
        required_domain_mask: 0b10,
        manifest: Manifest {
            protocol_manifest_version: 1,
            storage_layout_version: 1,
            composition_feature_bits: 0,
            profile_id: 3,
            entries: &manifest_entries,
        },
        bindings: &bindings,
        provider_gate: &gate,
    };
    let mut provider = FakeProvider::new();
    provider.allow_secondary_domain = true;
    provider.slots[0][SLOT - MARKER_BYTES] = 0;
    let mut owner = TwoDomainOwner::uninit();
    owner.init(&config, &provider).unwrap();
    owner.start_recovery().unwrap();
    for _ in 0..32 {
        if owner.lifecycle() == Lifecycle::Ready {
            break;
        }
        owner.step(&mut provider, 10, 1).unwrap();
    }
    assert_eq!(owner.lifecycle(), Lifecycle::Ready);
    assert_eq!(owner.domain_get(DOMAIN).unwrap().state, DomainState::Fault);
    assert_eq!(
        owner.domain_get(SECONDARY_DOMAIN).unwrap().state,
        DomainState::Ready
    );
    let mut output = [0x5A; BODY];
    assert_eq!(owner.copy_body(DOMAIN, &mut output), Err(Error::State));
    assert_eq!(output, [0x5A; BODY]);
}

#[test]
fn provider_geometry_drift_is_rejected_before_any_io() {
    let gate = ProviderGate::new();
    let manifest_entry = entry();
    let bindings = [DomainBinding {
        domain: DOMAIN,
        business_owner_instance: 21,
        domain_generation: 31,
    }];
    let mut provider = FakeProvider::new();
    let mut owner = Owner::uninit();
    initialize(&mut owner, &provider, &gate, &manifest_entry, &bindings);
    provider.geometry_override = Some(ProviderGeometry {
        maximum_slot_bytes: 184,
        ..provider.geometry()
    });
    let calls_before = provider.calls;
    assert_eq!(owner.step(&mut provider, 10, 1), Err(Error::Config));
    assert_eq!(provider.calls, calls_before);
    assert_eq!(owner.lifecycle(), Lifecycle::Recovering);
    assert_eq!(
        owner.domain_get(DOMAIN).unwrap().state,
        DomainState::Recovering
    );
}

#[test]
fn invalid_init_is_retryable_and_cancel_never_starts_io() {
    let gate = ProviderGate::new();
    let manifest_entry = entry();
    let valid_bindings = [DomainBinding {
        domain: DOMAIN,
        business_owner_instance: 21,
        domain_generation: 31,
    }];
    let invalid_bindings = [DomainBinding {
        business_owner_instance: 0,
        ..valid_bindings[0]
    }];
    let manifest = Manifest {
        protocol_manifest_version: 1,
        storage_layout_version: 1,
        composition_feature_bits: 0,
        profile_id: 3,
        entries: core::slice::from_ref(&manifest_entry),
    };
    let mut provider = FakeProvider::new();
    let mut owner = Owner::uninit();
    let invalid = PersistenceConfig {
        runtime_instance: 11,
        owner_instance: 12,
        required_domain_mask: 1,
        manifest,
        bindings: &invalid_bindings,
        provider_gate: &gate,
    };
    assert_eq!(owner.init(&invalid, &provider), Err(Error::Config));
    assert_eq!(owner.lifecycle(), Lifecycle::Uninitialized);
    initialize(
        &mut owner,
        &provider,
        &gate,
        &manifest_entry,
        &valid_bindings,
    );
    drive_ready(&mut owner, &mut provider);
    let calls_before = provider.calls;
    let view = owner.domain_get(DOMAIN).unwrap();
    let handle = owner
        .submit(&request(&[1], view.body_digest, 0, 1), 20)
        .unwrap();
    owner.cancel(handle).unwrap();
    assert_eq!(provider.calls, calls_before);
    assert_eq!(
        owner.request_get(handle).unwrap().terminal_error,
        Some(Error::Cancelled)
    );
    owner.failed_retire(handle).unwrap();
    assert_eq!(owner.request_get(handle), Err(Error::NotFound));
}

#[test]
fn two_domains_commit_with_independent_slots_witnesses_and_handles() {
    type TwoDomainOwner<'a> = PersistenceOwner<'a, 2, BODY, SLOT>;

    let gate = ProviderGate::new();
    let manifest_entries = [
        entry(),
        ManifestEntry {
            domain: SECONDARY_DOMAIN,
            ..entry()
        },
    ];
    let bindings = [
        DomainBinding {
            domain: DOMAIN,
            business_owner_instance: 21,
            domain_generation: 31,
        },
        DomainBinding {
            domain: SECONDARY_DOMAIN,
            business_owner_instance: 22,
            domain_generation: 32,
        },
    ];
    let config = PersistenceConfig {
        runtime_instance: 11,
        owner_instance: 12,
        required_domain_mask: 0b11,
        manifest: Manifest {
            protocol_manifest_version: 1,
            storage_layout_version: 1,
            composition_feature_bits: 0,
            profile_id: 3,
            entries: &manifest_entries,
        },
        bindings: &bindings,
        provider_gate: &gate,
    };
    let mut provider = FakeProvider::new();
    provider.allow_secondary_domain = true;
    let mut owner = TwoDomainOwner::uninit();
    owner.init(&config, &provider).unwrap();
    owner.start_recovery().unwrap();
    for _ in 0..32 {
        if owner.lifecycle() == Lifecycle::Ready {
            break;
        }
        owner.step(&mut provider, 10, 1).unwrap();
    }
    assert_eq!(owner.lifecycle(), Lifecycle::Ready);

    let primary_view = owner.domain_get(DOMAIN).unwrap();
    let primary = owner
        .submit(&request(&[0x11], primary_view.body_digest, 0, 1), 20)
        .unwrap();
    let mut secondary_request = request(&[0x22], [0; DIGEST_BYTES], 0, 1);
    secondary_request.domain = SECONDARY_DOMAIN;
    secondary_request.caller_owner_instance = 22;
    secondary_request.domain_generation = 32;
    let secondary = owner.submit(&secondary_request, 20).unwrap();
    for _ in 0..64 {
        if owner.request_get(primary).unwrap().state == RequestState::ProofReady
            && owner.request_get(secondary).unwrap().state == RequestState::ProofReady
        {
            break;
        }
        owner.step(&mut provider, 21, 1).unwrap();
    }
    assert_eq!(owner.proof_get(primary).unwrap().domain, DOMAIN);
    assert_eq!(owner.proof_get(secondary).unwrap().domain, SECONDARY_DOMAIN);
    assert_eq!(provider.witness, 1);
    assert_eq!(provider.secondary_witness, 1);
    let mut primary_body = [0; BODY];
    let mut secondary_body = [0; BODY];
    assert_eq!(owner.copy_body(DOMAIN, &mut primary_body).unwrap(), 1);
    assert_eq!(
        owner
            .copy_body(SECONDARY_DOMAIN, &mut secondary_body)
            .unwrap(),
        1
    );
    assert_eq!(primary_body[0], 0x11);
    assert_eq!(secondary_body[0], 0x22);
}
