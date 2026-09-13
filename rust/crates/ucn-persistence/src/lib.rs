#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的统一 Persistence Foundation。
//!
//! 本 crate 独立实现冻结的 Record/Marker Codec、固定容量双槽提交、独立 Witness、
//! 异步 Provider continuation、恢复选择与 durable proof。它不链接 C archive，也不包含
//! Identity、Security、Operation、Cluster 等业务 Record Body 语义。

mod codec;
mod foundation;
mod provider;

pub use codec::{
    ATOMIC_COMMIT_MARKER_16, CodecWorkspace, DIGEST_BLAKE2S_128, DIGEST_BYTES, DomainKey,
    DomainKind, ENVELOPE_BYTES, MARKER_BYTES, Manifest, ManifestEntry, MarkerState, RecordMeta,
    SLOT_COUNT, WITNESS_INDEPENDENT_MONOTONIC, blake2s128, body_digest, classify_marker, crc32c,
    decode_record, encode_marker, encode_record, manifest_digest,
};
pub use foundation::{
    DomainBinding, DomainState, DomainView, Lifecycle, PersistenceConfig, PersistenceHandle,
    PersistenceOwner, PersistenceProof, PersistenceRequest, RequestState, RequestView, StepResult,
};
pub use provider::{
    BlobState, Completion, IoPhase, IoStart, PersistenceProvider, ProviderGate, ProviderGeometry,
    WitnessView,
};
