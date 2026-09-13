//! 与 C99 实现共用的 Persistence 字节 Oracle。

use std::vec::Vec;

use ucn_persistence::{
    ATOMIC_COMMIT_MARKER_16, CodecWorkspace, DIGEST_BLAKE2S_128, DomainKey, DomainKind,
    ENVELOPE_BYTES, MARKER_BYTES, Manifest, ManifestEntry, WITNESS_INDEPENDENT_MONOTONIC,
    blake2s128, body_digest, encode_marker, encode_record, manifest_digest,
};

const SHARED_FIXTURES: &str = include_str!("../../../tests/conformance/v6s_persistence_v1.h");

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
                bytes.push(u8::from_str_radix(hex, 16).expect("fixture byte must be hexadecimal"));
            }
        }
        if collecting && !line.trim_end().ends_with('\\') {
            break;
        }
    }
    assert!(!bytes.is_empty(), "fixture {name} must exist");
    bytes
}

#[test]
fn hash_manifest_and_body_match_the_shared_c_oracle() {
    const BODY: usize = 1024;
    const SLOT: usize = ENVELOPE_BYTES + BODY + MARKER_BYTES;
    let mut workspace = CodecWorkspace::new();
    assert_eq!(
        blake2s128(b"", &mut workspace),
        fixture("UCN_V6S_PERSIST_BLAKE2S128_EMPTY_BYTES").as_slice()
    );
    assert_eq!(
        blake2s128(b"abc", &mut workspace),
        fixture("UCN_V6S_PERSIST_BLAKE2S128_ABC_BYTES").as_slice()
    );
    let entry = ManifestEntry {
        domain: DomainKey::new(DomainKind::ProductConfig, 1).unwrap(),
        body_capacity_bytes: 1024,
        slot_capacity_bytes: 1136,
        schema_id: 1,
        schema_version: 1,
        digest_suite: DIGEST_BLAKE2S_128,
        witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
        provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
    };
    let manifest = Manifest {
        protocol_manifest_version: 1,
        storage_layout_version: 1,
        composition_feature_bits: 0x1F,
        profile_id: 3,
        entries: core::slice::from_ref(&entry),
    };
    assert_eq!(
        manifest_digest::<BODY, SLOT>(&manifest, &mut workspace).unwrap(),
        fixture("UCN_V6S_PERSIST_MANIFEST_FULL_BYTES").as_slice()
    );
    let meta = ucn_persistence::RecordMeta {
        domain: entry.domain,
        record_generation: 1,
        transaction_id: 5,
        body_bytes: 3,
        schema_id: 1,
        schema_version: 1,
        operation_kind: 2,
        body_digest: [0; 16],
    };
    assert_eq!(
        body_digest(&meta, &[1, 2, 3], &mut workspace).unwrap(),
        fixture("UCN_V6S_PERSIST_BODY_PRODUCT_CONFIG_GEN1_TX5_BYTES").as_slice()
    );
}

#[test]
fn complete_record_image_matches_the_shared_c_oracle() {
    const BODY: usize = 3;
    const SLOT: usize = ENVELOPE_BYTES + BODY + MARKER_BYTES;
    let entry = ManifestEntry {
        domain: DomainKey::new(DomainKind::ProductConfig, 1).unwrap(),
        body_capacity_bytes: 3,
        slot_capacity_bytes: 115,
        schema_id: 1,
        schema_version: 1,
        digest_suite: DIGEST_BLAKE2S_128,
        witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
        provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
    };
    let meta = ucn_persistence::RecordMeta {
        domain: entry.domain,
        record_generation: 1,
        transaction_id: 5,
        body_bytes: 3,
        schema_id: 1,
        schema_version: 1,
        operation_kind: 2,
        body_digest: [0; 16],
    };
    let manifest_digest = core::array::from_fn(|index| u8::try_from(index).unwrap());
    let mut workspace = CodecWorkspace::new();
    let mut slot = [0; SLOT];
    encode_record::<BODY, SLOT>(
        &meta,
        &manifest_digest,
        &[1, 2, 3],
        &entry,
        0xFF,
        &mut slot,
        &mut workspace,
    )
    .unwrap();
    let mut marker = [0; MARKER_BYTES];
    encode_marker(1, &mut marker).unwrap();
    slot[SLOT - MARKER_BYTES..].copy_from_slice(&marker);
    assert_eq!(
        slot.as_slice(),
        fixture("UCN_V6S_PERSIST_RECORD_GEN1_TX5_SLOT115_BYTES").as_slice()
    );
}
