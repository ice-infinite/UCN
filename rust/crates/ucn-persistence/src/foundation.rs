use ucn_types::{Error, Result};

use crate::codec::{
    CodecWorkspace, DIGEST_BYTES, DomainKey, Manifest, ManifestEntry, MarkerState, RecordMeta,
    classify_marker, decode_record, encode_marker, encode_record, manifest_digest,
};
use crate::provider::{
    BlobState, Completion, IoPhase, IoStart, PersistenceProvider, ProviderGate, ProviderGeometry,
    WitnessView,
};

const INVALID_INDEX: u8 = u8::MAX;
const WITNESS_COMPLETION_BYTES: u32 = 40;
const ADVANCE_WITNESS_COMPLETION_BYTES: u32 = 8;

/// Persistence Owner 生命周期。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Lifecycle {
    /// 尚未装配 Manifest/Provider。
    Uninitialized,
    /// 已装配，尚未启动恢复。
    Initialized,
    /// 正在按域恢复。
    Recovering,
    /// 所有 required 域 Ready，可接收提交。
    Ready,
    /// required 域无法证明，整体失败关闭。
    Fault,
}

/// 单个持久化域状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DomainState {
    /// 尚未完成介质恢复。
    Recovering,
    /// 当前正文由 Witness 精确证明。
    Ready,
    /// 无法证明安全历史。
    Fault,
}

/// 单个请求状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RequestState {
    /// 已接纳但尚未开始 Provider I/O。
    Queued,
    /// Provider/验证状态机正在推进。
    InProgress,
    /// reload 后的 durable proof 已就绪。
    ProofReady,
    /// 请求以明确错误终止；调用方仍须退休 Handle。
    Failed,
}

/// Composition 为每个 Durable Domain 指定的唯一业务 Owner。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DomainBinding {
    /// 必须与同索引 Manifest Entry 完全相同。
    pub domain: DomainKey,
    /// 本次启动中唯一可以提交该域的业务 Owner。
    pub business_owner_instance: u16,
    /// 本次启动的非零、非全一易失代际。
    pub domain_generation: u16,
}

/// 初始化 Persistence Owner 所需的完整静态合同。
pub struct PersistenceConfig<'a> {
    /// Runtime 实例。
    pub runtime_instance: u32,
    /// Persistence Owner 实例。
    pub owner_instance: u16,
    /// Manifest 条目索引对应的 required 位图。
    pub required_domain_mask: u32,
    /// 完整 Durable Manifest。
    pub manifest: Manifest<'a>,
    /// 与 Manifest 同序的一对一业务绑定。
    pub bindings: &'a [DomainBinding],
    /// 所有共享同一 Provider 回调域的 Owner 共用该 Gate。
    pub provider_gate: &'a ProviderGate,
}

/// Coordinator 路由给 Persistence Owner 的规范提交请求。
pub struct PersistenceRequest<'a> {
    /// 请求所属 Runtime。
    pub runtime_instance: u32,
    /// 必须匹配 Domain Binding。
    pub caller_owner_instance: u16,
    /// 必须匹配本次启动的 Domain Generation。
    pub domain_generation: u16,
    /// Durable Domain。
    pub domain: DomainKey,
    /// 单调 Foundation Transaction ID。
    pub foundation_transaction_id: u64,
    /// 调用者观察到的当前 Record Generation。
    pub expected_record_generation: u64,
    /// 半开绝对 Deadline；0 表示无 Deadline。
    pub absolute_deadline_us: u64,
    /// 非零业务 transition 摘要，只用于易失绑定，不写 Record。
    pub business_transition_digest: u64,
    /// 完整下一状态正文。
    pub canonical_body: &'a [u8],
    /// 业务 Schema ID。
    pub schema_id: u16,
    /// 业务 Schema Version。
    pub schema_version: u16,
    /// 业务 Operation Kind。
    pub operation_kind: u16,
    /// 当前正文的精确 Digest；空域为全零。
    pub expected_body_digest: [u8; DIGEST_BYTES],
    /// Coordinator 的易失 continuation；不进入 Durable Record。
    pub volatile_continuation: u32,
}

/// Persistence Owner 签发的精确易失 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PersistenceHandle {
    runtime_instance: u32,
    owner_instance: u16,
    slot: u16,
    generation: u32,
}

/// reload 精确证明后的不可变持久化事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PersistenceProof {
    /// Runtime 实例。
    pub runtime_instance: u32,
    /// Persistence Owner 实例。
    pub persistence_owner_instance: u16,
    /// 原始业务 Owner。
    pub caller_owner_instance: u16,
    /// 本次启动的 Domain Generation。
    pub domain_generation: u16,
    /// Operation Kind。
    pub operation_kind: u16,
    /// Durable Domain。
    pub domain: DomainKey,
    /// 已发布 Record Generation。
    pub record_generation: u64,
    /// 已发布 Transaction ID。
    pub foundation_transaction_id: u64,
    /// reload 后精确匹配的 Witness Generation。
    pub witness_generation: u64,
    /// Body 长度。
    pub body_bytes: u32,
    /// 活动槽。
    pub active_slot: u8,
    /// Body Digest。
    pub body_digest: [u8; DIGEST_BYTES],
    /// Coordinator 易失 continuation。
    pub volatile_continuation: u32,
}

impl PersistenceProof {
    const EMPTY: Self = Self {
        runtime_instance: 0,
        persistence_owner_instance: 0,
        caller_owner_instance: 0,
        domain_generation: 0,
        operation_kind: 0,
        domain: DomainKey {
            kind: crate::codec::DomainKind::IdentityBinding,
            id: 1,
        },
        record_generation: 0,
        foundation_transaction_id: 0,
        witness_generation: 0,
        body_bytes: 0,
        active_slot: INVALID_INDEX,
        body_digest: [0; DIGEST_BYTES],
        volatile_continuation: 0,
    };
}

/// 单个域的只读恢复/提交视图。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DomainView {
    /// 域键。
    pub domain: DomainKey,
    /// 当前发布 Generation；Factory Empty 为 0。
    pub record_generation: u64,
    /// 当前正文长度。
    pub body_bytes: u32,
    /// Schema ID。
    pub schema_id: u16,
    /// Schema Version。
    pub schema_version: u16,
    /// 本次启动 Domain Generation。
    pub domain_generation: u16,
    /// 当前状态。
    pub state: DomainState,
    /// 当前活动槽；Factory Empty 为 `u8::MAX`。
    pub active_slot: u8,
    /// 是否属于 required 集合。
    pub required: bool,
    /// 是否持有未退休请求。
    pub pending: bool,
    /// 当前正文 Digest。
    pub body_digest: [u8; DIGEST_BYTES],
}

/// 单个请求的只读状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RequestView {
    /// 请求状态。
    pub state: RequestState,
    /// 终态错误；成功 Proof Ready 时为 `None`。
    pub terminal_error: Option<Error>,
    /// 当前 Provider 阶段；尚未开始或已终态为 `None`。
    pub provider_phase: Option<IoPhase>,
}

/// 一次有界 step 的结果。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StepResult {
    /// 实际消耗的 work unit。
    pub operations_performed: u16,
    /// 当前未退休 Proof 数量。
    pub proofs_ready: u16,
    /// Ready 域数量。
    pub domains_ready: u16,
    /// Fault 域数量。
    pub domains_faulted: u16,
    /// required 域是否全部 Ready。
    pub owner_ready: bool,
    /// 本轮是否发生状态推进。
    pub made_progress: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum WorkPhase {
    Idle,
    RecoveryLoadWitness,
    RecoveryLoadSlot0,
    RecoveryLoadSlot1,
    RecoverySelect,
    RecoveryAdvanceWitness,
    SubmitEncode,
    SubmitWrite,
    SubmitReadback,
    SubmitMarker,
    SubmitAdvanceWitness,
    SubmitReloadWitness,
    SubmitReloadSlot0,
    SubmitReloadSlot1,
    SubmitSelect,
    SubmitFinalize,
}

#[derive(Clone, Copy)]
struct ActiveIo {
    valid: bool,
    token: u64,
    phase: IoPhase,
    slot_index: u8,
}

impl ActiveIo {
    const EMPTY: Self = Self {
        valid: false,
        token: 0,
        phase: IoPhase::LoadSlot,
        slot_index: INVALID_INDEX,
    };
}

#[derive(Clone, Copy)]
struct Pending<const BODY: usize> {
    valid: bool,
    handle_generation: u32,
    state: RequestState,
    terminal_error: Option<Error>,
    caller_owner_instance: u16,
    domain_generation: u16,
    transaction_id: u64,
    expected_record_generation: u64,
    absolute_deadline_us: u64,
    business_transition_digest: u64,
    body_bytes: u32,
    operation_kind: u16,
    next_body_digest: [u8; DIGEST_BYTES],
    volatile_continuation: u32,
    target_slot: u8,
    marker_published: bool,
    proof: PersistenceProof,
    body: [u8; BODY],
}

impl<const BODY: usize> Pending<BODY> {
    const EMPTY: Self = Self {
        valid: false,
        handle_generation: 0,
        state: RequestState::Queued,
        terminal_error: None,
        caller_owner_instance: 0,
        domain_generation: 0,
        transaction_id: 0,
        expected_record_generation: 0,
        absolute_deadline_us: 0,
        business_transition_digest: 0,
        body_bytes: 0,
        operation_kind: 0,
        next_body_digest: [0; DIGEST_BYTES],
        volatile_continuation: 0,
        target_slot: INVALID_INDEX,
        marker_published: false,
        proof: PersistenceProof::EMPTY,
        body: [0; BODY],
    };
}

#[allow(clippy::struct_field_names)]
#[derive(Clone, Copy)]
struct Domain<const BODY: usize> {
    configured: bool,
    manifest: ManifestEntry,
    business_owner_instance: u16,
    domain_generation: u16,
    required: bool,
    state: DomainState,
    record_generation: u64,
    transaction_id: u64,
    current_operation_kind: u16,
    active_slot: u8,
    body_bytes: u32,
    body_digest: [u8; DIGEST_BYTES],
    body: [u8; BODY],
    next_handle_generation: u32,
    pending: Pending<BODY>,
}

impl<const BODY: usize> Domain<BODY> {
    const EMPTY_ENTRY: ManifestEntry = ManifestEntry {
        domain: DomainKey {
            kind: crate::codec::DomainKind::IdentityBinding,
            id: 1,
        },
        body_capacity_bytes: 0,
        slot_capacity_bytes: 0,
        schema_id: 0,
        schema_version: 0,
        digest_suite: 0,
        witness_policy: 0,
        provider_atomicity_class: 0,
    };
    const EMPTY: Self = Self {
        configured: false,
        manifest: Self::EMPTY_ENTRY,
        business_owner_instance: 0,
        domain_generation: 0,
        required: false,
        state: DomainState::Recovering,
        record_generation: 0,
        transaction_id: 0,
        current_operation_kind: 0,
        active_slot: INVALID_INDEX,
        body_bytes: 0,
        body_digest: [0; DIGEST_BYTES],
        body: [0; BODY],
        next_handle_generation: 1,
        pending: Pending::EMPTY,
    };
}

/// 固定容量、无堆分配的 Persistence Owner。
///
/// `DOMAINS/BODY/SLOT` 由产品 Profile 在编译期决定。调用方应把该对象放在静态或专用
/// Storage 中；[`PersistenceOwner::uninit`] 只提供 const 初始化，不要求把完整 Owner 放在任务栈。
pub struct PersistenceOwner<'a, const DOMAINS: usize, const BODY: usize, const SLOT: usize> {
    lifecycle: Lifecycle,
    runtime_instance: u32,
    owner_instance: u16,
    domain_count: u8,
    required_domain_mask: u32,
    manifest_digest: [u8; DIGEST_BYTES],
    gate: Option<&'a ProviderGate>,
    provider_geometry: ProviderGeometry,
    erased_value: u8,
    domains: [Domain<BODY>; DOMAINS],
    recovery_cursor: u8,
    work_cursor: u8,
    active_domain: u8,
    work_phase: WorkPhase,
    io: ActiveIo,
    witness: WitnessView,
    target_witness: u64,
    slots: [[u8; SLOT]; 2],
    write_buffer: [u8; SLOT],
    readback_buffer: [u8; SLOT],
    recovered_meta: [Option<RecordMeta>; 2],
    recovered_body: [[u8; BODY]; 2],
    codec: CodecWorkspace,
}

impl<'a, const DOMAINS: usize, const BODY: usize, const SLOT: usize>
    PersistenceOwner<'a, DOMAINS, BODY, SLOT>
{
    /// 建立完整零运行态、可静态放置的 Owner Storage。
    #[must_use]
    pub const fn uninit() -> Self {
        Self {
            lifecycle: Lifecycle::Uninitialized,
            runtime_instance: 0,
            owner_instance: 0,
            domain_count: 0,
            required_domain_mask: 0,
            manifest_digest: [0; DIGEST_BYTES],
            gate: None,
            provider_geometry: ProviderGeometry {
                minimum_write_alignment: 0,
                minimum_erase_alignment: 0,
                maximum_slot_bytes: 0,
                atomic_marker_bytes: 0,
                erased_value: 0,
            },
            erased_value: 0,
            domains: [const { Domain::EMPTY }; DOMAINS],
            recovery_cursor: 0,
            work_cursor: 0,
            active_domain: INVALID_INDEX,
            work_phase: WorkPhase::Idle,
            io: ActiveIo::EMPTY,
            witness: WitnessView {
                domain: DomainKey {
                    kind: crate::codec::DomainKind::IdentityBinding,
                    id: 1,
                },
                highest_maybe_published_generation: 0,
                state: BlobState::Empty,
            },
            target_witness: 0,
            slots: [[0; SLOT]; 2],
            write_buffer: [0; SLOT],
            readback_buffer: [0; SLOT],
            recovered_meta: [None; 2],
            recovered_body: [[0; BODY]; 2],
            codec: CodecWorkspace::new(),
        }
    }

    /// 在首次 Provider I/O 和状态发布前校验完整 Manifest、Binding 与几何并装配 Owner。
    ///
    /// # Errors
    ///
    /// Owner 非零态、容量、Manifest、Binding 或 Provider 几何不成立时原子拒绝。
    pub fn init<P: PersistenceProvider>(
        &mut self,
        config: &PersistenceConfig<'a>,
        provider: &P,
    ) -> Result<()> {
        if self.lifecycle != Lifecycle::Uninitialized
            || config.runtime_instance == 0
            || config.owner_instance == 0
            || DOMAINS == 0
            || DOMAINS > 32
            || config.manifest.entries.len() > DOMAINS
            || config.manifest.entries.len() != config.bindings.len()
            || config.manifest.entries.is_empty()
            || (config.manifest.entries.len() < 32
                && config.required_domain_mask >> config.manifest.entries.len() != 0)
        {
            return Err(Error::Config);
        }
        let geometry = provider.geometry();
        geometry.validate::<SLOT>()?;
        for (entry, binding) in config.manifest.entries.iter().zip(config.bindings.iter()) {
            if binding.domain != entry.domain
                || binding.business_owner_instance == 0
                || binding.domain_generation == 0
                || binding.domain_generation == u16::MAX
            {
                return Err(Error::Config);
            }
        }
        let digest = manifest_digest::<BODY, SLOT>(&config.manifest, &mut self.codec)?;
        for (index, (entry, binding)) in config
            .manifest
            .entries
            .iter()
            .zip(config.bindings.iter())
            .enumerate()
        {
            let domain = &mut self.domains[index];
            domain.configured = true;
            domain.manifest = *entry;
            domain.business_owner_instance = binding.business_owner_instance;
            domain.domain_generation = binding.domain_generation;
            domain.required = (config.required_domain_mask & (1_u32 << index)) != 0;
            domain.state = DomainState::Recovering;
        }
        self.runtime_instance = config.runtime_instance;
        self.owner_instance = config.owner_instance;
        self.domain_count =
            u8::try_from(config.manifest.entries.len()).map_err(|_| Error::Config)?;
        self.required_domain_mask = config.required_domain_mask;
        self.manifest_digest = digest;
        self.gate = Some(config.provider_gate);
        self.provider_geometry = geometry;
        self.erased_value = geometry.erased_value;
        self.lifecycle = Lifecycle::Initialized;
        Ok(())
    }

    /// 返回 Owner 生命周期。
    #[must_use]
    pub const fn lifecycle(&self) -> Lifecycle {
        self.lifecycle
    }

    /// 开始逐域恢复；不在本调用内访问 Provider。
    ///
    /// # Errors
    ///
    /// 仅 [`Lifecycle::Initialized`] 可调用。
    pub fn start_recovery(&mut self) -> Result<()> {
        if self.lifecycle != Lifecycle::Initialized || self.io.valid {
            return Err(Error::State);
        }
        self.lifecycle = Lifecycle::Recovering;
        self.recovery_cursor = 0;
        self.active_domain = 0;
        self.work_phase = WorkPhase::RecoveryLoadWitness;
        for domain in &mut self.domains[..usize::from(self.domain_count)] {
            domain.state = DomainState::Recovering;
        }
        Ok(())
    }

    /// 提交一个完整 next snapshot；所有业务校验在首次 Provider I/O 前完成。
    ///
    /// # Errors
    ///
    /// 调用者、Domain、Expected State、Transaction、Deadline 或容量不匹配时零 I/O 拒绝。
    #[allow(clippy::too_many_lines)]
    pub fn submit(
        &mut self,
        request: &PersistenceRequest<'_>,
        now_us: u64,
    ) -> Result<PersistenceHandle> {
        if self.lifecycle != Lifecycle::Ready
            || request.runtime_instance != self.runtime_instance
            || request.business_transition_digest == 0
            || request.volatile_continuation == 0
            || (request.absolute_deadline_us != 0 && now_us >= request.absolute_deadline_us)
        {
            return Err(Error::State);
        }
        let runtime_instance = self.runtime_instance;
        let persistence_owner_instance = self.owner_instance;
        let index = self.find_domain(request.domain).ok_or(Error::NotFound)?;
        let domain = &mut self.domains[index];
        if domain.state != DomainState::Ready
            || domain.pending.valid
            || request.caller_owner_instance != domain.business_owner_instance
            || request.domain_generation != domain.domain_generation
            || request.schema_id != domain.manifest.schema_id
            || request.schema_version != domain.manifest.schema_version
            || request.operation_kind == 0
            || request.canonical_body.len() > BODY
            || request.canonical_body.len() > domain.manifest.body_capacity_bytes as usize
            || request.expected_record_generation != domain.record_generation
            || request.expected_body_digest != domain.body_digest
            || request.foundation_transaction_id == 0
            || request.foundation_transaction_id == u64::MAX
        {
            return Err(Error::State);
        }
        let exact_replay = request.foundation_transaction_id == domain.transaction_id
            && domain.record_generation != 0
            && request.canonical_body.len() == domain.body_bytes as usize
            && request.canonical_body == &domain.body[..domain.body_bytes as usize]
            && request.operation_kind == domain.current_operation_kind;
        if request.foundation_transaction_id <= domain.transaction_id && !exact_replay {
            return Err(Error::Replay);
        }
        let body_bytes = u32::try_from(request.canonical_body.len()).map_err(|_| Error::NoSpace)?;
        let next_digest = if exact_replay {
            domain.body_digest
        } else {
            let next_generation = domain
                .record_generation
                .checked_add(1)
                .ok_or(Error::Exhausted)?;
            if next_generation == u64::MAX {
                return Err(Error::Exhausted);
            }
            let meta = RecordMeta {
                domain: request.domain,
                record_generation: next_generation,
                transaction_id: request.foundation_transaction_id,
                body_bytes,
                schema_id: request.schema_id,
                schema_version: request.schema_version,
                operation_kind: request.operation_kind,
                body_digest: [0; DIGEST_BYTES],
            };
            crate::codec::body_digest(&meta, request.canonical_body, &mut self.codec)?
        };
        let handle_generation = domain.next_handle_generation;
        domain.next_handle_generation = domain
            .next_handle_generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        if exact_replay {
            let proof = PersistenceProof {
                runtime_instance,
                persistence_owner_instance,
                caller_owner_instance: request.caller_owner_instance,
                domain_generation: request.domain_generation,
                operation_kind: request.operation_kind,
                domain: request.domain,
                record_generation: domain.record_generation,
                foundation_transaction_id: request.foundation_transaction_id,
                witness_generation: domain.record_generation,
                body_bytes,
                active_slot: domain.active_slot,
                body_digest: domain.body_digest,
                volatile_continuation: request.volatile_continuation,
            };
            domain.pending = Pending::EMPTY;
            domain.pending.valid = true;
            domain.pending.handle_generation = handle_generation;
            domain.pending.state = RequestState::ProofReady;
            domain.pending.proof = proof;
        } else {
            domain.pending = Pending::EMPTY;
            domain.pending.valid = true;
            domain.pending.handle_generation = handle_generation;
            domain.pending.state = RequestState::Queued;
            domain.pending.caller_owner_instance = request.caller_owner_instance;
            domain.pending.domain_generation = request.domain_generation;
            domain.pending.transaction_id = request.foundation_transaction_id;
            domain.pending.expected_record_generation = request.expected_record_generation;
            domain.pending.absolute_deadline_us = request.absolute_deadline_us;
            domain.pending.business_transition_digest = request.business_transition_digest;
            domain.pending.body_bytes = body_bytes;
            domain.pending.operation_kind = request.operation_kind;
            domain.pending.next_body_digest = next_digest;
            domain.pending.volatile_continuation = request.volatile_continuation;
            domain.pending.target_slot = u8::from(domain.active_slot == 0);
            domain.pending.body[..request.canonical_body.len()]
                .copy_from_slice(request.canonical_body);
        }
        Ok(PersistenceHandle {
            runtime_instance: self.runtime_instance,
            owner_instance: self.owner_instance,
            slot: u16::try_from(index).map_err(|_| Error::Exhausted)? + 1,
            generation: handle_generation,
        })
    }

    /// 取消尚未开始 Provider I/O 的请求。
    ///
    /// # Errors
    ///
    /// Handle 不匹配或请求已经开始时返回状态错误。
    pub fn cancel(&mut self, handle: PersistenceHandle) -> Result<()> {
        let index = self.match_handle(handle)?;
        let pending = &mut self.domains[index].pending;
        if pending.state != RequestState::Queued {
            return Err(Error::State);
        }
        pending.state = RequestState::Failed;
        pending.terminal_error = Some(Error::Cancelled);
        Ok(())
    }

    /// 在固定预算内推进恢复、提交或异步 Provider continuation。
    ///
    /// # Errors
    ///
    /// 生命周期、预算、Provider completion 或持久化证明不合法时返回错误。
    pub fn step<P: PersistenceProvider>(
        &mut self,
        provider: &mut P,
        now_us: u64,
        operation_budget: u16,
    ) -> Result<StepResult> {
        if operation_budget == 0 || operation_budget > 32 {
            return Err(Error::Argument);
        }
        if !matches!(self.lifecycle, Lifecycle::Recovering | Lifecycle::Ready) {
            return Err(Error::State);
        }
        let current_geometry = provider.geometry();
        if current_geometry != self.provider_geometry
            || current_geometry.validate::<SLOT>().is_err()
        {
            return Err(Error::Config);
        }
        let mut operations = 0_u16;
        let mut made_progress = false;
        while operations < operation_budget {
            if self.work_phase == WorkPhase::Idle && self.lifecycle == Lifecycle::Ready {
                let Some(index) = self.next_pending_index() else {
                    break;
                };
                self.active_domain = u8::try_from(index).map_err(|_| Error::Exhausted)?;
                self.work_phase = WorkPhase::SubmitEncode;
            }
            let progress = match self.drive_one(provider, now_us) {
                Ok(progress) => progress,
                Err(Error::State) => return Err(Error::State),
                Err(error) => {
                    self.fault_active_domain(error);
                    true
                }
            };
            operations += 1;
            made_progress |= progress;
            if !progress {
                break;
            }
        }
        Ok(self.step_result(operations, made_progress))
    }

    /// 查询单个 Domain；返回的是即时拷贝，不借用 Owner 内部正文。
    ///
    /// # Errors
    ///
    /// Domain 未登记时返回 [`Error::NotFound`]。
    pub fn domain_get(&self, key: DomainKey) -> Result<DomainView> {
        let index = self.find_domain(key).ok_or(Error::NotFound)?;
        let domain = &self.domains[index];
        Ok(DomainView {
            domain: key,
            record_generation: domain.record_generation,
            body_bytes: domain.body_bytes,
            schema_id: domain.manifest.schema_id,
            schema_version: domain.manifest.schema_version,
            domain_generation: domain.domain_generation,
            state: domain.state,
            active_slot: domain.active_slot,
            required: domain.required,
            pending: domain.pending.valid,
            body_digest: domain.body_digest,
        })
    }

    /// 将当前 Ready 正文复制到调用方固定缓冲区。
    ///
    /// # Errors
    ///
    /// Domain 不存在、未 Ready 或输出容量不足时返回错误。
    pub fn copy_body(&self, key: DomainKey, output: &mut [u8]) -> Result<usize> {
        let index = self.find_domain(key).ok_or(Error::NotFound)?;
        let domain = &self.domains[index];
        if domain.state != DomainState::Ready {
            return Err(Error::State);
        }
        let length = domain.body_bytes as usize;
        if output.len() < length {
            return Err(Error::NoSpace);
        }
        output[..length].copy_from_slice(&domain.body[..length]);
        Ok(length)
    }

    /// 查询请求状态。
    ///
    /// # Errors
    ///
    /// Handle 不属于当前活动请求时返回 [`Error::NotFound`]。
    pub fn request_get(&self, handle: PersistenceHandle) -> Result<RequestView> {
        let index = self.match_handle(handle)?;
        let pending = &self.domains[index].pending;
        let provider_phase = if pending.state == RequestState::InProgress {
            self.io.valid.then_some(self.io.phase)
        } else {
            None
        };
        Ok(RequestView {
            state: pending.state,
            terminal_error: pending.terminal_error,
            provider_phase,
        })
    }

    /// 取得 reload 后的精确 Proof。
    ///
    /// # Errors
    ///
    /// Handle 不匹配或请求尚未形成 Proof 时返回错误。
    pub fn proof_get(&self, handle: PersistenceHandle) -> Result<PersistenceProof> {
        let index = self.match_handle(handle)?;
        let pending = &self.domains[index].pending;
        if pending.state != RequestState::ProofReady {
            return Err(Error::State);
        }
        Ok(pending.proof)
    }

    /// 退休成功 Proof 并释放该 Domain 的唯一 Pending 槽。
    ///
    /// # Errors
    ///
    /// Handle 不匹配或请求不是 Proof Ready 时返回错误。
    pub fn proof_retire(&mut self, handle: PersistenceHandle) -> Result<()> {
        let index = self.match_handle(handle)?;
        if self.domains[index].pending.state != RequestState::ProofReady {
            return Err(Error::State);
        }
        self.domains[index].pending = Pending::EMPTY;
        Ok(())
    }

    /// 退休明确失败/取消请求。
    ///
    /// # Errors
    ///
    /// Handle 不匹配或请求不是失败终态时返回错误。
    pub fn failed_retire(&mut self, handle: PersistenceHandle) -> Result<()> {
        let index = self.match_handle(handle)?;
        if self.domains[index].pending.state != RequestState::Failed {
            return Err(Error::State);
        }
        self.domains[index].pending = Pending::EMPTY;
        Ok(())
    }

    #[allow(clippy::too_many_lines)]
    fn drive_one<P: PersistenceProvider>(&mut self, provider: &mut P, now_us: u64) -> Result<bool> {
        let index = usize::from(self.active_domain);
        if matches!(
            self.work_phase,
            WorkPhase::SubmitWrite | WorkPhase::SubmitReadback | WorkPhase::SubmitMarker
        ) && !self.io.valid
            && !self.domains[index].pending.marker_published
            && self.domains[index].pending.absolute_deadline_us != 0
            && now_us >= self.domains[index].pending.absolute_deadline_us
        {
            self.domains[index].pending.state = RequestState::Failed;
            self.domains[index].pending.terminal_error = Some(Error::Timeout);
            self.active_domain = INVALID_INDEX;
            self.work_phase = WorkPhase::Idle;
            return Ok(true);
        }
        match self.work_phase {
            WorkPhase::Idle => Ok(false),
            WorkPhase::RecoveryLoadWitness => {
                let completed = self.io_witness(provider, IoPhase::LoadWitness, index, 0, 0)?;
                if completed {
                    self.work_phase = WorkPhase::RecoveryLoadSlot0;
                }
                Ok(completed)
            }
            WorkPhase::RecoveryLoadSlot0 => {
                let completed = self.io_slot(provider, IoPhase::LoadSlot, index, 0)?;
                if completed {
                    self.work_phase = WorkPhase::RecoveryLoadSlot1;
                }
                Ok(completed)
            }
            WorkPhase::RecoveryLoadSlot1 => {
                let completed = self.io_slot(provider, IoPhase::LoadSlot, index, 1)?;
                if completed {
                    self.work_phase = WorkPhase::RecoverySelect;
                }
                Ok(completed)
            }
            WorkPhase::RecoverySelect => {
                if let Some(target) = self.select_recovered(index, true)? {
                    self.target_witness = target;
                    self.work_phase = WorkPhase::RecoveryAdvanceWitness;
                } else {
                    self.finish_recovery_domain(index);
                }
                Ok(true)
            }
            WorkPhase::RecoveryAdvanceWitness => {
                let old = self.witness.highest_maybe_published_generation;
                let completed = self.io_witness(
                    provider,
                    IoPhase::AdvanceWitness,
                    index,
                    old,
                    self.target_witness,
                )?;
                if completed {
                    self.work_phase = WorkPhase::RecoveryLoadWitness;
                }
                Ok(completed)
            }
            WorkPhase::SubmitEncode => {
                let domain = &mut self.domains[index];
                if domain.pending.absolute_deadline_us != 0
                    && now_us >= domain.pending.absolute_deadline_us
                {
                    domain.pending.state = RequestState::Failed;
                    domain.pending.terminal_error = Some(Error::Timeout);
                    self.active_domain = INVALID_INDEX;
                    self.work_phase = WorkPhase::Idle;
                    return Ok(true);
                }
                let meta = RecordMeta {
                    domain: domain.manifest.domain,
                    record_generation: domain.record_generation + 1,
                    transaction_id: domain.pending.transaction_id,
                    body_bytes: domain.pending.body_bytes,
                    schema_id: domain.manifest.schema_id,
                    schema_version: domain.manifest.schema_version,
                    operation_kind: domain.pending.operation_kind,
                    body_digest: [0; DIGEST_BYTES],
                };
                encode_record::<BODY, SLOT>(
                    &meta,
                    &self.manifest_digest,
                    &domain.pending.body[..domain.pending.body_bytes as usize],
                    &domain.manifest,
                    self.erased_value,
                    &mut self.write_buffer,
                    &mut self.codec,
                )?;
                domain.pending.state = RequestState::InProgress;
                self.work_phase = WorkPhase::SubmitWrite;
                Ok(true)
            }
            WorkPhase::SubmitWrite => {
                let completed = self.io_slot(
                    provider,
                    IoPhase::WriteInactive,
                    index,
                    self.domains[index].pending.target_slot,
                )?;
                if completed {
                    self.work_phase = WorkPhase::SubmitReadback;
                }
                Ok(completed)
            }
            WorkPhase::SubmitReadback => {
                let completed = self.io_slot(
                    provider,
                    IoPhase::Readback,
                    index,
                    self.domains[index].pending.target_slot,
                )?;
                if completed {
                    if self.write_buffer != self.readback_buffer {
                        return Err(Error::Malformed);
                    }
                    self.work_phase = WorkPhase::SubmitMarker;
                }
                Ok(completed)
            }
            WorkPhase::SubmitMarker => {
                let completed = self.io_slot(
                    provider,
                    IoPhase::PublishMarker,
                    index,
                    self.domains[index].pending.target_slot,
                )?;
                if completed {
                    self.domains[index].pending.marker_published = true;
                    self.work_phase = WorkPhase::SubmitAdvanceWitness;
                }
                Ok(completed)
            }
            WorkPhase::SubmitAdvanceWitness => {
                let old = self.domains[index].record_generation;
                let completed =
                    self.io_witness(provider, IoPhase::AdvanceWitness, index, old, old + 1)?;
                if completed {
                    self.work_phase = WorkPhase::SubmitReloadWitness;
                }
                Ok(completed)
            }
            WorkPhase::SubmitReloadWitness => {
                let completed = self.io_witness(provider, IoPhase::LoadWitness, index, 0, 0)?;
                if completed {
                    self.work_phase = WorkPhase::SubmitReloadSlot0;
                }
                Ok(completed)
            }
            WorkPhase::SubmitReloadSlot0 => {
                let completed = self.io_slot(provider, IoPhase::LoadSlot, index, 0)?;
                if completed {
                    self.work_phase = WorkPhase::SubmitReloadSlot1;
                }
                Ok(completed)
            }
            WorkPhase::SubmitReloadSlot1 => {
                let completed = self.io_slot(provider, IoPhase::LoadSlot, index, 1)?;
                if completed {
                    self.work_phase = WorkPhase::SubmitSelect;
                }
                Ok(completed)
            }
            WorkPhase::SubmitSelect => {
                if self.select_recovered(index, false)?.is_some() {
                    return Err(Error::Malformed);
                }
                let domain = &self.domains[index];
                if domain.record_generation != domain.pending.expected_record_generation + 1
                    || domain.transaction_id != domain.pending.transaction_id
                    || domain.body_digest != domain.pending.next_body_digest
                    || domain.active_slot != domain.pending.target_slot
                {
                    return Err(Error::Malformed);
                }
                self.work_phase = WorkPhase::SubmitFinalize;
                Ok(true)
            }
            WorkPhase::SubmitFinalize => {
                let domain = &self.domains[index];
                let proof = PersistenceProof {
                    runtime_instance: self.runtime_instance,
                    persistence_owner_instance: self.owner_instance,
                    caller_owner_instance: domain.pending.caller_owner_instance,
                    domain_generation: domain.pending.domain_generation,
                    operation_kind: domain.pending.operation_kind,
                    domain: domain.manifest.domain,
                    record_generation: domain.record_generation,
                    foundation_transaction_id: domain.transaction_id,
                    witness_generation: self.witness.highest_maybe_published_generation,
                    body_bytes: domain.body_bytes,
                    active_slot: domain.active_slot,
                    body_digest: domain.body_digest,
                    volatile_continuation: domain.pending.volatile_continuation,
                };
                self.domains[index].pending.proof = proof;
                self.domains[index].pending.state = RequestState::ProofReady;
                self.active_domain = INVALID_INDEX;
                self.work_phase = WorkPhase::Idle;
                Ok(true)
            }
        }
    }

    fn io_slot<P: PersistenceProvider>(
        &mut self,
        provider: &mut P,
        phase: IoPhase,
        domain_index: usize,
        slot_index: u8,
    ) -> Result<bool> {
        if slot_index > 1 {
            return Err(Error::State);
        }
        let domain = self.domains[domain_index].manifest.domain;
        let expected_bytes = match phase {
            IoPhase::LoadSlot | IoPhase::WriteInactive | IoPhase::Readback => {
                u32::try_from(SLOT).map_err(|_| Error::Config)?
            }
            IoPhase::PublishMarker => 16,
            _ => return Err(Error::State),
        };
        let gate = self.gate.ok_or(Error::State)?;
        let (token, start) = if self.io.valid {
            if self.io.phase != phase || self.io.slot_index != slot_index {
                return Err(Error::State);
            }
            let token = self.io.token;
            if phase == IoPhase::LoadSlot {
                self.slots[slot_index as usize].fill(self.erased_value);
            } else if phase == IoPhase::Readback {
                self.readback_buffer.fill(self.erased_value);
            }
            let output = match phase {
                IoPhase::LoadSlot => Some(self.slots[slot_index as usize].as_mut_slice()),
                IoPhase::Readback => Some(self.readback_buffer.as_mut_slice()),
                _ => None,
            };
            let start = gate.call_existing(|| provider.poll(token, phase, output, None))?;
            (token, start)
        } else {
            if phase == IoPhase::LoadSlot {
                self.slots[slot_index as usize].fill(self.erased_value);
            } else if phase == IoPhase::Readback {
                self.readback_buffer.fill(self.erased_value);
            }
            gate.call_new(|token| match phase {
                IoPhase::LoadSlot => provider.begin_load_slot(
                    domain,
                    slot_index,
                    &mut self.slots[slot_index as usize],
                    token,
                ),
                IoPhase::WriteInactive => {
                    provider.begin_write_inactive(domain, slot_index, &self.write_buffer, token)
                }
                IoPhase::Readback => {
                    provider.begin_readback(domain, slot_index, &mut self.readback_buffer, token)
                }
                IoPhase::PublishMarker => {
                    let mut marker = [0; crate::codec::MARKER_BYTES];
                    let generation = self.domains[domain_index].record_generation + 1;
                    if encode_marker(generation, &mut marker).is_err() {
                        return IoStart::Failed(Error::Argument);
                    }
                    provider.begin_publish_marker(domain, slot_index, &marker, token)
                }
                _ => IoStart::Failed(Error::State),
            })?
        };
        self.merge_io(start, token, phase, slot_index, expected_bytes)
    }

    fn io_witness<P: PersistenceProvider>(
        &mut self,
        provider: &mut P,
        phase: IoPhase,
        domain_index: usize,
        expected_old: u64,
        exact_new: u64,
    ) -> Result<bool> {
        let domain = self.domains[domain_index].manifest.domain;
        let expected_bytes = match phase {
            IoPhase::LoadWitness => WITNESS_COMPLETION_BYTES,
            IoPhase::AdvanceWitness => ADVANCE_WITNESS_COMPLETION_BYTES,
            _ => return Err(Error::State),
        };
        let gate = self.gate.ok_or(Error::State)?;
        let (token, start) = if self.io.valid {
            if self.io.phase != phase || self.io.slot_index != INVALID_INDEX {
                return Err(Error::State);
            }
            let token = self.io.token;
            if phase == IoPhase::LoadWitness {
                self.witness = WitnessView {
                    domain,
                    highest_maybe_published_generation: 0,
                    state: BlobState::Empty,
                };
            }
            let witness_output = (phase == IoPhase::LoadWitness).then_some(&mut self.witness);
            let start = gate.call_existing(|| provider.poll(token, phase, None, witness_output))?;
            (token, start)
        } else {
            if phase == IoPhase::LoadWitness {
                self.witness = WitnessView {
                    domain,
                    highest_maybe_published_generation: 0,
                    state: BlobState::Empty,
                };
            }
            gate.call_new(|token| match phase {
                IoPhase::LoadWitness => {
                    provider.begin_load_witness(domain, &mut self.witness, token)
                }
                IoPhase::AdvanceWitness => {
                    provider.begin_advance_witness(domain, expected_old, exact_new, token)
                }
                _ => IoStart::Failed(Error::State),
            })?
        };
        self.merge_io(start, token, phase, INVALID_INDEX, expected_bytes)
    }

    fn merge_io(
        &mut self,
        start: IoStart,
        token: u64,
        phase: IoPhase,
        slot_index: u8,
        expected_bytes: u32,
    ) -> Result<bool> {
        match start {
            IoStart::Pending => {
                self.io = ActiveIo {
                    valid: true,
                    token,
                    phase,
                    slot_index,
                };
                Ok(false)
            }
            IoStart::Failed(error) => {
                self.io = ActiveIo::EMPTY;
                Err(error)
            }
            IoStart::Completed(completion) => {
                self.io = ActiveIo::EMPTY;
                self.validate_completion(completion, token, phase, slot_index, expected_bytes)?;
                Ok(true)
            }
        }
    }

    fn validate_completion(
        &self,
        completion: Completion,
        token: u64,
        phase: IoPhase,
        slot_index: u8,
        expected_bytes: u32,
    ) -> Result<()> {
        let state_ok = if phase == IoPhase::LoadWitness {
            matches!(completion.blob_state, BlobState::Present | BlobState::Empty)
        } else {
            completion.blob_state == BlobState::Present
        };
        if completion.io_token != token
            || completion.phase != phase
            || completion.slot_index != slot_index
            || completion.exact_bytes != expected_bytes
            || !state_ok
            || (phase == IoPhase::LoadWitness && completion.blob_state != self.witness.state)
        {
            return Err(Error::Malformed);
        }
        completion.result
    }

    #[allow(clippy::too_many_lines)]
    fn select_recovered(&mut self, index: usize, allow_repair: bool) -> Result<Option<u64>> {
        self.recovered_meta = [None; 2];
        for slot_index in 0..2 {
            let mut marker = [0; crate::codec::MARKER_BYTES];
            marker.copy_from_slice(&self.slots[slot_index][SLOT - crate::codec::MARKER_BYTES..]);
            match classify_marker(&marker, self.erased_value) {
                MarkerState::Erased => {}
                MarkerState::Torn => return Err(Error::Malformed),
                MarkerState::Committed(_) => {
                    let mut meta = RecordMeta {
                        domain: self.domains[index].manifest.domain,
                        record_generation: 0,
                        transaction_id: 0,
                        body_bytes: 0,
                        schema_id: 0,
                        schema_version: 0,
                        operation_kind: 0,
                        body_digest: [0; DIGEST_BYTES],
                    };
                    decode_record(
                        &self.slots[slot_index],
                        &self.domains[index].manifest,
                        &self.manifest_digest,
                        self.erased_value,
                        &mut meta,
                        &mut self.recovered_body[slot_index],
                        &mut self.codec,
                    )?;
                    self.recovered_meta[slot_index] = Some(meta);
                }
            }
        }
        if let (Some(left), Some(right)) = (self.recovered_meta[0], self.recovered_meta[1]) {
            let (older, newer) = match left.record_generation.cmp(&right.record_generation) {
                core::cmp::Ordering::Less => (left, right),
                core::cmp::Ordering::Greater => (right, left),
                core::cmp::Ordering::Equal => return Err(Error::Malformed),
            };
            if older.record_generation.checked_add(1) != Some(newer.record_generation)
                || newer.transaction_id <= older.transaction_id
                || older.domain != newer.domain
                || older.schema_id != newer.schema_id
                || older.schema_version != newer.schema_version
            {
                return Err(Error::Malformed);
            }
        }
        if self.witness.state != BlobState::Present
            || self.witness.domain != self.domains[index].manifest.domain
            || self.witness.highest_maybe_published_generation == u64::MAX
        {
            return Err(Error::Malformed);
        }
        let witness = self.witness.highest_maybe_published_generation;
        let current = self.find_generation(witness);
        let successor = witness
            .checked_add(1)
            .filter(|generation| *generation != u64::MAX)
            .and_then(|generation| self.find_generation(generation));
        for meta in self.recovered_meta.iter().flatten() {
            if meta.record_generation > witness.saturating_add(1) {
                return Err(Error::Malformed);
            }
        }
        if witness == 0 {
            if current.is_some() {
                return Err(Error::Malformed);
            }
            if let Some(successor_index) = successor {
                if allow_repair {
                    return Ok(Some(1));
                }
                let _ = successor_index;
                return Err(Error::Malformed);
            }
            if self.recovered_meta.iter().any(Option::is_some) {
                return Err(Error::Malformed);
            }
            self.clear_domain_ready(index);
            return Ok(None);
        }
        let Some(current_index) = current else {
            return Err(Error::Malformed);
        };
        if let Some(successor_index) = successor {
            let older = self.recovered_meta[current_index].unwrap();
            let newer = self.recovered_meta[successor_index].unwrap();
            if newer.transaction_id <= older.transaction_id {
                return Err(Error::Malformed);
            }
            if allow_repair {
                return Ok(Some(witness + 1));
            }
            let _ = successor_index;
            return Err(Error::Malformed);
        }
        self.apply_recovered(index, current_index);
        Ok(None)
    }

    fn find_generation(&self, generation: u64) -> Option<usize> {
        self.recovered_meta
            .iter()
            .position(|meta| meta.is_some_and(|value| value.record_generation == generation))
    }

    fn apply_recovered(&mut self, index: usize, slot_index: usize) {
        let meta = self.recovered_meta[slot_index].unwrap();
        let domain = &mut self.domains[index];
        domain.state = DomainState::Ready;
        domain.record_generation = meta.record_generation;
        domain.transaction_id = meta.transaction_id;
        domain.current_operation_kind = meta.operation_kind;
        domain.active_slot = u8::try_from(slot_index).unwrap_or(INVALID_INDEX);
        domain.body_bytes = meta.body_bytes;
        domain.body_digest = meta.body_digest;
        domain.body[..meta.body_bytes as usize]
            .copy_from_slice(&self.recovered_body[slot_index][..meta.body_bytes as usize]);
    }

    fn clear_domain_ready(&mut self, index: usize) {
        let domain = &mut self.domains[index];
        domain.state = DomainState::Ready;
        domain.record_generation = 0;
        domain.transaction_id = 0;
        domain.current_operation_kind = 0;
        domain.active_slot = INVALID_INDEX;
        domain.body_bytes = 0;
        domain.body_digest = [0; DIGEST_BYTES];
    }

    fn finish_recovery_domain(&mut self, index: usize) {
        self.domains[index].state = DomainState::Ready;
        self.recovery_cursor += 1;
        if self.recovery_cursor >= self.domain_count {
            self.lifecycle = Lifecycle::Ready;
            self.active_domain = INVALID_INDEX;
            self.work_phase = WorkPhase::Idle;
        } else {
            self.active_domain = self.recovery_cursor;
            self.work_phase = WorkPhase::RecoveryLoadWitness;
        }
    }

    fn fault_active_domain(&mut self, error: Error) {
        if self.active_domain == INVALID_INDEX {
            self.lifecycle = Lifecycle::Fault;
            return;
        }
        let index = usize::from(self.active_domain);
        let domain = &mut self.domains[index];
        domain.state = DomainState::Fault;
        if domain.pending.valid && domain.pending.state != RequestState::ProofReady {
            domain.pending.state = RequestState::Failed;
            domain.pending.terminal_error = Some(if domain.pending.marker_published {
                Error::InDoubt
            } else {
                error
            });
        }
        self.io = ActiveIo::EMPTY;
        self.work_phase = WorkPhase::Idle;
        self.active_domain = INVALID_INDEX;
        if domain.required {
            self.lifecycle = Lifecycle::Fault;
        } else if self.lifecycle == Lifecycle::Recovering {
            self.recovery_cursor += 1;
            if self.recovery_cursor >= self.domain_count {
                self.lifecycle = Lifecycle::Ready;
            } else {
                self.active_domain = self.recovery_cursor;
                self.work_phase = WorkPhase::RecoveryLoadWitness;
            }
        }
    }

    fn next_pending_index(&mut self) -> Option<usize> {
        let count = usize::from(self.domain_count);
        for offset in 0..count {
            let index = (usize::from(self.work_cursor) + offset) % count;
            if self.domains[index].state == DomainState::Ready
                && self.domains[index].pending.valid
                && self.domains[index].pending.state == RequestState::Queued
            {
                self.work_cursor = u8::try_from((index + 1) % count).unwrap_or(0);
                return Some(index);
            }
        }
        None
    }

    fn find_domain(&self, key: DomainKey) -> Option<usize> {
        self.domains[..usize::from(self.domain_count)]
            .iter()
            .position(|domain| domain.configured && domain.manifest.domain == key)
    }

    fn match_handle(&self, handle: PersistenceHandle) -> Result<usize> {
        if handle.runtime_instance != self.runtime_instance
            || handle.owner_instance != self.owner_instance
            || handle.slot == 0
            || usize::from(handle.slot) > usize::from(self.domain_count)
            || handle.generation == 0
        {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let pending = &self.domains[index].pending;
        if !pending.valid || pending.handle_generation != handle.generation {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn step_result(&self, operations: u16, made_progress: bool) -> StepResult {
        let mut proofs = 0_u16;
        let mut ready = 0_u16;
        let mut faulted = 0_u16;
        for domain in &self.domains[..usize::from(self.domain_count)] {
            proofs +=
                u16::from(domain.pending.valid && domain.pending.state == RequestState::ProofReady);
            ready += u16::from(domain.state == DomainState::Ready);
            faulted += u16::from(domain.state == DomainState::Fault);
        }
        StepResult {
            operations_performed: operations,
            proofs_ready: proofs,
            domains_ready: ready,
            domains_faulted: faulted,
            owner_ready: self.lifecycle == Lifecycle::Ready,
            made_progress,
        }
    }
}
