use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_persistence::{
    CodecWorkspace, DIGEST_BYTES, DomainKey, DomainKind, DomainState, DomainView,
    PersistenceHandle, PersistenceProof, PersistenceRequest, RecordMeta, body_digest,
};
use ucn_types::{
    AddressAuthorityGeneration, AddressWidth, BindingGeneration, Error, NodeAddress, RealmId,
    Result,
};

use crate::{
    AddressMode, AuthorityEpoch, AuthorityFreshness, AuthorityTransition, AuthorityTransitionKind,
    BindingCertificate, BindingView, IdentityProof, IdentityVerifier, LeasePolicy, Principal,
    lease_deadline_build, lease_is_live,
};

/// Address Authority 持久化正文长度。
pub const AUTHORITY_RECORD_BYTES: usize = 192;
const AUTHORITY_RECORD_BYTES_U32: u32 = 192;
/// Address Binding 持久化正文长度。
pub const BINDING_RECORD_BYTES: usize = 96;
const BINDING_RECORD_BYTES_U32: u32 = 96;
/// Address Authority Schema ID。
pub const AUTHORITY_SCHEMA_ID: u16 = 0x4901;
/// Address Authority Schema Version。
pub const AUTHORITY_SCHEMA_VERSION: u16 = 1;
/// Address Authority publish operation kind。
pub const AUTHORITY_OPERATION_KIND: u16 = 0x0601;
/// Address Binding Schema ID。
pub const BINDING_SCHEMA_ID: u16 = 0x4902;
/// Address Binding Schema Version。
pub const BINDING_SCHEMA_VERSION: u16 = 1;
/// Address Binding issue operation kind。
pub const BINDING_ISSUE_OPERATION_KIND: u16 = 0x0602;
/// Address Binding retire operation kind；本阶段保留 Registry 值。
pub const BINDING_RETIRE_OPERATION_KIND: u16 = 0x0603;

const AUTHORITY_MAGIC: &[u8; 4] = b"UC6A";
const BINDING_MAGIC: &[u8; 4] = b"UC6B";
const CALLBACK_AUTHORITY_VERIFY: u16 = 0x0601;
const CALLBACK_BINDING_VERIFY: u16 = 0x0602;
const MAX_IDENTITY_PROOF_BYTES: usize = 512;
const MAX_REQUIREMENT_BODY_BYTES: usize = AUTHORITY_RECORD_BYTES;

/// 从 Persistence Domain 当前视图派生的 Identity 提交基线。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct IdentityDurabilityBase {
    /// Runtime instance。
    pub runtime_instance: u32,
    /// Identity Owner 对应的 Persistence business owner instance。
    pub caller_owner_instance: u16,
    /// 本启动 Domain Generation。
    pub domain_generation: u16,
    /// Identity durable domain。
    pub domain: DomainKey,
    /// 下一 Foundation Transaction ID。
    pub next_transaction_id: u64,
    /// 当前 Record Generation。
    pub expected_record_generation: u64,
    /// 当前 Body Digest。
    pub expected_body_digest: [u8; DIGEST_BYTES],
    /// 半开绝对持久化 Deadline。
    pub absolute_deadline_us: u64,
    /// Coordinator 易失 continuation。
    pub volatile_continuation: u32,
    prior: PriorRecord,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum PriorRecord {
    Empty,
    Authority(AuthorityEpoch),
    Binding(BindingCertificate),
}

impl IdentityDurabilityBase {
    /// 从 Ready Domain view 和当前正文构造精确提交基线。
    ///
    /// # Errors
    ///
    /// Domain、Schema、Digest、正文、Deadline 或单调 Transaction ID 不成立时失败关闭。
    pub fn from_view(
        runtime_instance: u32,
        caller_owner_instance: u16,
        view: DomainView,
        current_body: &[u8],
        absolute_deadline_us: u64,
        volatile_continuation: u32,
        workspace: &mut CodecWorkspace,
    ) -> Result<Self> {
        if runtime_instance == 0
            || caller_owner_instance == 0
            || view.domain.kind != DomainKind::IdentityBinding
            || view.state != DomainState::Ready
            || view.pending
            || view.domain_generation == 0
            || absolute_deadline_us == 0
            || volatile_continuation == 0
        {
            return Err(Error::State);
        }
        let next_transaction_id = view
            .foundation_transaction_id
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        if next_transaction_id == 0 || next_transaction_id == u64::MAX {
            return Err(Error::Exhausted);
        }
        let prior = if view.record_generation == 0 {
            if view.foundation_transaction_id != 0
                || view.body_bytes != 0
                || !current_body.is_empty()
                || view.body_digest != [0; DIGEST_BYTES]
                || view.current_operation_kind != 0
            {
                return Err(Error::State);
            }
            PriorRecord::Empty
        } else {
            if current_body.len() != view.body_bytes as usize {
                return Err(Error::State);
            }
            let meta = RecordMeta {
                domain: view.domain,
                record_generation: view.record_generation,
                transaction_id: view.foundation_transaction_id,
                body_bytes: view.body_bytes,
                schema_id: view.schema_id,
                schema_version: view.schema_version,
                operation_kind: view.current_operation_kind,
                body_digest: [0; DIGEST_BYTES],
            };
            if body_digest(&meta, current_body, workspace)? != view.body_digest {
                return Err(Error::Security);
            }
            match (
                view.schema_id,
                view.schema_version,
                view.current_operation_kind,
                current_body.len(),
            ) {
                (
                    AUTHORITY_SCHEMA_ID,
                    AUTHORITY_SCHEMA_VERSION,
                    AUTHORITY_OPERATION_KIND,
                    AUTHORITY_RECORD_BYTES,
                ) => PriorRecord::Authority(decode_authority_record(current_body)?),
                (
                    BINDING_SCHEMA_ID,
                    BINDING_SCHEMA_VERSION,
                    BINDING_ISSUE_OPERATION_KIND,
                    BINDING_RECORD_BYTES,
                ) => PriorRecord::Binding(decode_binding_record(current_body)?),
                _ => return Err(Error::State),
            }
        };
        Ok(Self {
            runtime_instance,
            caller_owner_instance,
            domain_generation: view.domain_generation,
            domain: view.domain,
            next_transaction_id,
            expected_record_generation: view.record_generation,
            expected_body_digest: view.body_digest,
            absolute_deadline_us,
            volatile_continuation,
            prior,
        })
    }
}

/// Identity Owner 交给 Coordinator 的不可变 Persistence requirement。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct IdentityDurabilityRequirement {
    base: IdentityDurabilityBase,
    body: [u8; MAX_REQUIREMENT_BODY_BYTES],
    body_bytes: u32,
    schema_id: u16,
    schema_version: u16,
    operation_kind: u16,
    expected_published_digest: [u8; DIGEST_BYTES],
    business_transition_digest: u64,
}

impl IdentityDurabilityRequirement {
    /// 转换成 Persistence Owner 的只读请求。
    #[must_use]
    pub fn persistence_request(&self) -> PersistenceRequest<'_> {
        PersistenceRequest {
            runtime_instance: self.base.runtime_instance,
            caller_owner_instance: self.base.caller_owner_instance,
            domain_generation: self.base.domain_generation,
            domain: self.base.domain,
            foundation_transaction_id: self.base.next_transaction_id,
            expected_record_generation: self.base.expected_record_generation,
            absolute_deadline_us: self.base.absolute_deadline_us,
            business_transition_digest: self.business_transition_digest,
            canonical_body: &self.body[..self.body_bytes as usize],
            schema_id: self.schema_id,
            schema_version: self.schema_version,
            operation_kind: self.operation_kind,
            expected_body_digest: self.base.expected_body_digest,
            volatile_continuation: self.base.volatile_continuation,
        }
    }
}

/// Identity Owner 静态配置。
pub struct IdentityConfig<'a> {
    /// Runtime instance。
    pub runtime_instance: u32,
    /// Identity Owner instance。
    pub owner_instance: u32,
    /// Persistence Domain Binding 使用的 16-bit Owner ID。
    pub persistence_business_owner_instance: u16,
    /// 产生 durable proof 的 Persistence Owner instance。
    pub persistence_owner_instance: u16,
    /// Realm。
    pub realm: RealmId,
    /// Realm 固定地址宽度。
    pub address_width: AddressWidth,
    /// 所有 Identity Provider 共用的任务/ISR/SMP 安全门。
    pub provider_gate: &'a CallbackGate,
    /// 唯一 Authority durable domain。
    pub authority_domain: DomainKey,
    /// Manifest 固定的 Binding durable domains。
    pub binding_domains: &'a [DomainKey],
    /// 认证 challenge 的固定最大寿命。
    pub challenge_lifetime_us: u64,
    /// Authority 租约验证策略。
    pub authority_lease_policy: LeasePolicy,
    /// Binding 租约验证策略。
    pub binding_lease_policy: LeasePolicy,
}

/// Authority runtime 生命周期。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum AuthorityPhase {
    Empty,
    AwaitingDurability,
    Active,
    Fenced,
}

/// Binding runtime 生命周期。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum BindingPhase {
    Empty,
    AwaitingDurability,
    Active,
    Retired,
    Fenced,
}

#[derive(Clone, Copy)]
struct Challenge {
    valid: bool,
    generation: u32,
    verifier: Principal,
    nonce: u64,
    transaction_id: u64,
    started_at_us: u64,
    deadline_us: u64,
}

impl Challenge {
    const EMPTY: Self = Self {
        valid: false,
        generation: 0,
        verifier: Principal::INVALID,
        nonce: 0,
        transaction_id: 0,
        started_at_us: 0,
        deadline_us: 0,
    };
}

#[derive(Clone, Copy)]
struct AuthoritySlot {
    generation: u32,
    phase: AuthorityPhase,
    epoch: Option<AuthorityEpoch>,
    local_deadline_us: u64,
    requirement: Option<IdentityDurabilityRequirement>,
    persistence_handle: Option<PersistenceHandle>,
    transition_kind: Option<AuthorityTransitionKind>,
    previous_epoch: Option<AuthorityEpoch>,
    previous_deadline_us: u64,
}

impl AuthoritySlot {
    const EMPTY: Self = Self {
        generation: 0,
        phase: AuthorityPhase::Empty,
        epoch: None,
        local_deadline_us: 0,
        requirement: None,
        persistence_handle: None,
        transition_kind: None,
        previous_epoch: None,
        previous_deadline_us: 0,
    };
}

#[derive(Clone, Copy)]
struct BindingSlot {
    occupied: bool,
    generation: u32,
    phase: BindingPhase,
    certificate: Option<BindingCertificate>,
    local_deadline_us: u64,
    requirement: Option<IdentityDurabilityRequirement>,
    persistence_handle: Option<PersistenceHandle>,
    previous_certificate: Option<BindingCertificate>,
    previous_deadline_us: u64,
    challenge_slot: u16,
    challenge_generation: u32,
}

impl BindingSlot {
    const EMPTY: Self = Self {
        occupied: false,
        generation: 0,
        phase: BindingPhase::Empty,
        certificate: None,
        local_deadline_us: 0,
        requirement: None,
        persistence_handle: None,
        previous_certificate: None,
        previous_deadline_us: 0,
        challenge_slot: 0,
        challenge_generation: 0,
    };
}

/// Authority challenge 的不可伪造本地 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AuthorityChallengeHandle {
    owner_instance: u32,
    generation: u32,
    nonce: u64,
    transaction_id: u64,
}

/// Binding challenge 的不可伪造本地 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BindingChallengeHandle {
    owner_instance: u32,
    slot: u16,
    generation: u32,
    nonce: u64,
    transaction_id: u64,
}

/// Authority transition 的精确 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AuthorityHandle {
    owner_instance: u32,
    generation: u32,
    authority_generation: AddressAuthorityGeneration,
    lease_sequence: u64,
}

/// Binding transition 的精确 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BindingHandle {
    owner_instance: u32,
    slot: u16,
    generation: u32,
    binding_generation: BindingGeneration,
}

/// 固定容量 Identity Owner。
pub struct IdentityOwner<'a, const BINDINGS: usize> {
    config: IdentityConfig<'a>,
    next_operation_id: u32,
    codec: CodecWorkspace,
    authority_challenge: Challenge,
    binding_challenges: [Challenge; BINDINGS],
    authority: AuthoritySlot,
    bindings: [BindingSlot; BINDINGS],
}

/// Nano Profile：2 个 durable Binding slot。
pub type NanoIdentityOwner<'a> = IdentityOwner<'a, 2>;
/// Lite Profile：8 个 durable Binding slot。
pub type LiteIdentityOwner<'a> = IdentityOwner<'a, 8>;
/// Full Profile：16 个 durable Binding slot。
pub type FullIdentityOwner<'a> = IdentityOwner<'a, 16>;

impl<'a, const BINDINGS: usize> IdentityOwner<'a, BINDINGS> {
    /// 建立空 Identity Owner。
    ///
    /// # Errors
    ///
    /// Instance、容量、Domain、租约或地址配置不完整时返回配置错误。
    pub fn new(config: IdentityConfig<'a>) -> Result<Self> {
        if config.runtime_instance == 0
            || config.owner_instance == 0
            || config.persistence_business_owner_instance == 0
            || config.persistence_owner_instance == 0
            || BINDINGS == 0
            || BINDINGS > u16::MAX as usize
            || config.binding_domains.len() != BINDINGS
            || config.challenge_lifetime_us == 0
            || config.authority_domain.kind != DomainKind::IdentityBinding
            || config.binding_domains.iter().any(|domain| {
                domain.kind != DomainKind::IdentityBinding || *domain == config.authority_domain
            })
            || config
                .binding_domains
                .iter()
                .enumerate()
                .any(|(index, left)| config.binding_domains[index + 1..].contains(left))
            || config.authority_lease_policy.local_policy_max_lease_us == 0
            || config.binding_lease_policy.local_policy_max_lease_us == 0
        {
            return Err(Error::Config);
        }
        validate_lease_policy(config.authority_lease_policy)?;
        validate_lease_policy(config.binding_lease_policy)?;
        Ok(Self {
            config,
            next_operation_id: 1,
            codec: CodecWorkspace::new(),
            authority_challenge: Challenge::EMPTY,
            binding_challenges: [Challenge::EMPTY; BINDINGS],
            authority: AuthoritySlot::EMPTY,
            bindings: [BindingSlot::EMPTY; BINDINGS],
        })
    }

    /// 锁存 Authority challenge 的本地单调起点；后续 API 不能重写该起点。
    ///
    /// # Errors
    ///
    /// 已有 challenge、零标识或 Deadline 溢出时失败且不改写原 challenge。
    pub fn begin_authority_challenge(
        &mut self,
        verifier: Principal,
        nonce: u64,
        transaction_id: u64,
        now_us: u64,
    ) -> Result<AuthorityChallengeHandle> {
        if self.authority_challenge.valid || nonce == 0 || transaction_id == 0 {
            return Err(Error::State);
        }
        let deadline = now_us
            .checked_add(self.config.challenge_lifetime_us)
            .ok_or(Error::Exhausted)?;
        let generation = self
            .authority_challenge
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.authority_challenge = Challenge {
            valid: true,
            generation,
            verifier,
            nonce,
            transaction_id,
            started_at_us: now_us,
            deadline_us: deadline,
        };
        Ok(AuthorityChallengeHandle {
            owner_instance: self.config.owner_instance,
            generation,
            nonce,
            transaction_id,
        })
    }

    /// 锁存设备 Binding challenge；容量满时不驱逐其他 challenge。
    ///
    /// # Errors
    ///
    /// 标识非法、重复、容量满或 Deadline 溢出时失败且不驱逐其他槽。
    pub fn begin_binding_challenge(
        &mut self,
        verifier: Principal,
        nonce: u64,
        transaction_id: u64,
        now_us: u64,
    ) -> Result<BindingChallengeHandle> {
        if nonce == 0
            || transaction_id == 0
            || self.binding_challenges.iter().any(|challenge| {
                challenge.valid
                    && (challenge.verifier == verifier
                        || challenge.transaction_id == transaction_id)
            })
        {
            return Err(Error::State);
        }
        let index = self
            .binding_challenges
            .iter()
            .position(|challenge| !challenge.valid)
            .ok_or(Error::NoSpace)?;
        let deadline = now_us
            .checked_add(self.config.challenge_lifetime_us)
            .ok_or(Error::Exhausted)?;
        let generation = self.binding_challenges[index]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.binding_challenges[index] = Challenge {
            valid: true,
            generation,
            verifier,
            nonce,
            transaction_id,
            started_at_us: now_us,
            deadline_us: deadline,
        };
        Ok(BindingChallengeHandle {
            owner_instance: self.config.owner_instance,
            slot: u16::try_from(index + 1).map_err(|_| Error::NoSpace)?,
            generation,
            nonce,
            transaction_id,
        })
    }

    /// 验证 Authority Epoch 并形成 persist-before-publish requirement。
    ///
    /// # Errors
    ///
    /// Challenge、证明、转换、租约、历史或持久化基线不精确时失败关闭。
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn prepare_authority<V: IdentityVerifier>(
        &mut self,
        verifier: &mut V,
        challenge_handle: AuthorityChallengeHandle,
        kind: AuthorityTransitionKind,
        proposed: AuthorityEpoch,
        freshness: AuthorityFreshness,
        proof: IdentityProof<'_>,
        base: IdentityDurabilityBase,
        now_us: u64,
    ) -> Result<(AuthorityHandle, IdentityDurabilityRequirement)> {
        proposed.validate(self.config.realm)?;
        if proof.bytes.is_empty()
            || proof.bytes.len() > MAX_IDENTITY_PROOF_BYTES
            || base.runtime_instance != self.config.runtime_instance
            || base.caller_owner_instance != self.config.persistence_business_owner_instance
            || base.domain != self.config.authority_domain
            || now_us >= base.absolute_deadline_us
            || self.authority.phase == AuthorityPhase::AwaitingDurability
        {
            return Err(Error::State);
        }
        let challenge = self.match_authority_challenge(challenge_handle)?;
        if now_us < challenge.started_at_us
            || now_us >= challenge.deadline_us
            || freshness.verifier_principal != proposed.authority_principal
            || challenge.verifier != freshness.verifier_principal
            || freshness.challenge_nonce != challenge.nonce
            || freshness.transaction_id != challenge.transaction_id
            || freshness.authority_lease_sequence != proposed.lease_sequence
            || freshness.max_remaining_lease_us == 0
            || freshness.max_remaining_lease_us > proposed.lease_duration_us
            || freshness.binding_lease_id != [0; 16]
            || freshness.binding_generation != 0
            || crate::model::trivial(&freshness.proof_transcript_hash)
        {
            return Err(Error::Security);
        }
        let committed = match base.prior {
            PriorRecord::Empty => None,
            PriorRecord::Authority(value) => Some(value),
            PriorRecord::Binding(_) => return Err(Error::State),
        };
        validate_authority_transition(kind, committed, proposed)?;
        let local_deadline = lease_deadline_build(
            challenge.started_at_us,
            freshness.max_remaining_lease_us,
            self.config.authority_lease_policy,
        )?;
        if !lease_is_live(now_us, local_deadline) {
            return Err(Error::Timeout);
        }
        let transition = AuthorityTransition {
            kind,
            committed,
            proposed,
            freshness,
            challenge_started_local_us: challenge.started_at_us,
            lease_policy: self.config.authority_lease_policy,
            derived_local_deadline_us: local_deadline,
        };
        self.call_verify_authority(verifier, &transition, proof.bytes)?;

        let body = encode_authority_record(proposed);
        let requirement = self.build_requirement(
            base,
            &body,
            AUTHORITY_SCHEMA_ID,
            AUTHORITY_SCHEMA_VERSION,
            AUTHORITY_OPERATION_KIND,
        )?;
        let generation = self
            .authority
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let previous_epoch = if self.authority.phase == AuthorityPhase::Active {
            self.authority.epoch
        } else {
            None
        };
        let previous_deadline_us = if previous_epoch.is_some() {
            self.authority.local_deadline_us
        } else {
            0
        };
        self.authority = AuthoritySlot {
            generation,
            phase: AuthorityPhase::AwaitingDurability,
            epoch: Some(proposed),
            local_deadline_us: local_deadline,
            requirement: Some(requirement),
            persistence_handle: None,
            transition_kind: Some(kind),
            previous_epoch,
            previous_deadline_us,
        };
        let handle = AuthorityHandle {
            owner_instance: self.config.owner_instance,
            generation,
            authority_generation: proposed.authority_generation,
            lease_sequence: proposed.lease_sequence,
        };
        Ok((handle, requirement))
    }

    /// 绑定 Authority requirement 实际提交得到的 Persistence Handle。
    ///
    /// # Errors
    ///
    /// Handle、阶段或重复绑定不匹配时返回错误。
    pub fn bind_authority_persistence(
        &mut self,
        authority: AuthorityHandle,
        persistence: PersistenceHandle,
    ) -> Result<()> {
        self.match_authority(authority)?;
        if self.authority.phase != AuthorityPhase::AwaitingDurability
            || self.authority.persistence_handle.is_some()
        {
            return Err(Error::State);
        }
        self.authority.persistence_handle = Some(persistence);
        Ok(())
    }

    /// 使用 exact reload proof 发布 Authority；未来起点、到期租约和错 proof 均零发布拒绝。
    ///
    /// # Errors
    ///
    /// Handle、Proof、Challenge、Deadline 或候选代际不精确时返回错误。
    pub fn activate_authority(
        &mut self,
        authority: AuthorityHandle,
        persistence: PersistenceHandle,
        proof: PersistenceProof,
        now_us: u64,
    ) -> Result<()> {
        self.match_authority(authority)?;
        let requirement = self.authority.requirement.ok_or(Error::State)?;
        let epoch = self.authority.epoch.ok_or(Error::State)?;
        let challenge = self.authority_challenge;
        if self.authority.phase != AuthorityPhase::AwaitingDurability
            || self.authority.persistence_handle != Some(persistence)
            || !challenge.valid
            || now_us < challenge.started_at_us
            || now_us >= challenge.deadline_us
            || !lease_is_live(now_us, self.authority.local_deadline_us)
            || proof_mismatch(
                &self.config,
                &requirement,
                &proof,
                AUTHORITY_RECORD_BYTES_U32,
            )?
            || authority.authority_generation != epoch.authority_generation
            || authority.lease_sequence != epoch.lease_sequence
        {
            return Err(Error::State);
        }
        let transferred = self.authority.transition_kind == Some(AuthorityTransitionKind::Transfer);
        self.authority.phase = AuthorityPhase::Active;
        self.authority.requirement = None;
        self.authority.persistence_handle = None;
        self.authority.transition_kind = None;
        self.authority.previous_epoch = None;
        self.authority.previous_deadline_us = 0;
        self.authority_challenge.valid = false;
        if transferred {
            for slot in &mut self.bindings {
                if slot.occupied {
                    slot.phase = BindingPhase::Fenced;
                    slot.requirement = None;
                    slot.persistence_handle = None;
                }
            }
        }
        Ok(())
    }

    /// 在尚未提交 Persistence 前取消 Authority 候选并恢复先前 live 快照。
    ///
    /// # Errors
    ///
    /// Handle 不匹配、不在等待持久化阶段，或已绑定 Persistence Handle 时返回状态错误。
    pub fn abort_authority(&mut self, authority: AuthorityHandle) -> Result<()> {
        self.match_authority(authority)?;
        if self.authority.phase != AuthorityPhase::AwaitingDurability
            || self.authority.persistence_handle.is_some()
        {
            return Err(Error::State);
        }
        let generation = self.authority.generation;
        let previous = self.authority.previous_epoch;
        let previous_deadline_us = self.authority.previous_deadline_us;
        self.authority = AuthoritySlot::EMPTY;
        self.authority.generation = generation;
        if let Some(epoch) = previous {
            self.authority.phase = AuthorityPhase::Active;
            self.authority.epoch = Some(epoch);
            self.authority.local_deadline_us = previous_deadline_us;
        }
        self.authority_challenge.valid = false;
        Ok(())
    }

    /// 返回当前 live Authority Epoch。
    ///
    /// # Errors
    ///
    /// Authority 未发布、已 Fence 或租约到期时返回访问错误。
    pub fn authority_get(&self, now_us: u64) -> Result<AuthorityEpoch> {
        if self.authority.phase != AuthorityPhase::Active
            || !lease_is_live(now_us, self.authority.local_deadline_us)
        {
            return Err(Error::Access);
        }
        self.authority.epoch.ok_or(Error::State)
    }

    /// 验证 Binding Certificate 并形成 persist-before-publish requirement。
    ///
    /// # Errors
    ///
    /// Authority、Challenge、Proof、地址历史、租约或持久化基线不成立时失败关闭。
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn prepare_binding<V: IdentityVerifier>(
        &mut self,
        verifier: &mut V,
        challenge_handle: BindingChallengeHandle,
        certificate: BindingCertificate,
        freshness: AuthorityFreshness,
        proof: IdentityProof<'_>,
        base: IdentityDurabilityBase,
        now_us: u64,
    ) -> Result<(BindingHandle, IdentityDurabilityRequirement)> {
        certificate.validate(self.config.realm, self.config.address_width)?;
        let authority = self.authority_get(now_us)?;
        let challenge_index = self.binding_challenge_index(challenge_handle)?;
        let challenge = self.binding_challenges[challenge_index];
        if proof.bytes.is_empty()
            || proof.bytes.len() > MAX_IDENTITY_PROOF_BYTES
            || base.runtime_instance != self.config.runtime_instance
            || base.caller_owner_instance != self.config.persistence_business_owner_instance
            || now_us >= base.absolute_deadline_us
            || !self.config.binding_domains.contains(&base.domain)
            || now_us < challenge.started_at_us
            || now_us >= challenge.deadline_us
            || challenge.verifier != certificate.binding.principal
            || freshness.verifier_principal != certificate.binding.principal
            || freshness.challenge_nonce != challenge.nonce
            || freshness.transaction_id != challenge.transaction_id
            || freshness.authority_lease_sequence != authority.lease_sequence
            || freshness.authority_lease_sequence != certificate.authority_lease_sequence
            || freshness.binding_lease_id != certificate.lease_id
            || freshness.binding_generation != certificate.binding.generation.get()
            || freshness.max_remaining_lease_us == 0
            || freshness.max_remaining_lease_us > authority.lease_duration_us
            || freshness.max_remaining_lease_us > certificate.lease_duration_us
            || crate::model::trivial(&freshness.proof_transcript_hash)
            || certificate.authority_principal != authority.authority_principal
            || certificate.authority_generation != authority.authority_generation
        {
            return Err(Error::Security);
        }
        let domain_index = self
            .config
            .binding_domains
            .iter()
            .position(|domain| *domain == base.domain)
            .ok_or(Error::Access)?;
        let prior = match base.prior {
            PriorRecord::Empty => None,
            PriorRecord::Binding(value) => Some(value),
            PriorRecord::Authority(_) => return Err(Error::State),
        };
        match prior {
            None => {
                if certificate.binding.generation.get() != 1 {
                    return Err(Error::Replay);
                }
            }
            Some(previous) => {
                if previous.binding.realm != certificate.binding.realm
                    || previous.binding.address != certificate.binding.address
                    || previous.binding.address_width != certificate.binding.address_width
                    || previous.binding.generation.checked_next()? != certificate.binding.generation
                    || previous.lease_id == certificate.lease_id
                {
                    return Err(Error::Replay);
                }
            }
        }
        if self.bindings.iter().enumerate().any(|(index, slot)| {
            index != domain_index
                && slot.occupied
                && slot.certificate.is_some_and(|existing| {
                    existing.binding.address == certificate.binding.address
                        || (slot.phase == BindingPhase::Active
                            && existing.binding.principal == certificate.binding.principal)
                })
        }) {
            return Err(Error::Access);
        }
        let local_deadline = lease_deadline_build(
            challenge.started_at_us,
            freshness.max_remaining_lease_us,
            self.config.binding_lease_policy,
        )?;
        if !lease_is_live(now_us, local_deadline) {
            return Err(Error::Timeout);
        }
        self.call_verify_binding(
            verifier,
            &authority,
            &certificate,
            &freshness,
            challenge.started_at_us,
            local_deadline,
            proof.bytes,
        )?;
        let body = encode_binding_record(certificate);
        let requirement = self.build_requirement(
            base,
            &body,
            BINDING_SCHEMA_ID,
            BINDING_SCHEMA_VERSION,
            BINDING_ISSUE_OPERATION_KIND,
        )?;
        let slot_generation = self.bindings[domain_index]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let previous_certificate = if self.bindings[domain_index].phase == BindingPhase::Active {
            self.bindings[domain_index].certificate
        } else {
            None
        };
        let previous_deadline_us = if previous_certificate.is_some() {
            self.bindings[domain_index].local_deadline_us
        } else {
            0
        };
        self.bindings[domain_index] = BindingSlot {
            occupied: true,
            generation: slot_generation,
            phase: BindingPhase::AwaitingDurability,
            certificate: Some(certificate),
            local_deadline_us: local_deadline,
            requirement: Some(requirement),
            persistence_handle: None,
            previous_certificate,
            previous_deadline_us,
            challenge_slot: challenge_handle.slot,
            challenge_generation: challenge_handle.generation,
        };
        let handle = BindingHandle {
            owner_instance: self.config.owner_instance,
            slot: u16::try_from(domain_index + 1).map_err(|_| Error::NoSpace)?,
            generation: slot_generation,
            binding_generation: certificate.binding.generation,
        };
        Ok((handle, requirement))
    }

    /// 绑定 Binding requirement 实际提交得到的 Persistence Handle。
    ///
    /// # Errors
    ///
    /// Handle、阶段或重复绑定不匹配时返回错误。
    pub fn bind_binding_persistence(
        &mut self,
        binding: BindingHandle,
        persistence: PersistenceHandle,
    ) -> Result<()> {
        let index = self.binding_index(binding)?;
        let slot = &mut self.bindings[index];
        if slot.phase != BindingPhase::AwaitingDurability || slot.persistence_handle.is_some() {
            return Err(Error::State);
        }
        slot.persistence_handle = Some(persistence);
        Ok(())
    }

    /// 使用 exact reload proof 发布 Binding。
    ///
    /// # Errors
    ///
    /// Authority、Handle、Proof、Challenge 或 Deadline 不精确时返回错误。
    pub fn activate_binding(
        &mut self,
        binding: BindingHandle,
        persistence: PersistenceHandle,
        proof: PersistenceProof,
        now_us: u64,
    ) -> Result<BindingView> {
        let authority = self.authority_get(now_us)?;
        let index = self.binding_index(binding)?;
        let slot = &self.bindings[index];
        let certificate = slot.certificate.ok_or(Error::State)?;
        let requirement = slot.requirement.ok_or(Error::State)?;
        let challenge_index = usize::from(slot.challenge_slot.checked_sub(1).ok_or(Error::State)?);
        let challenge = self.binding_challenges.get(challenge_index);
        if slot.phase != BindingPhase::AwaitingDurability
            || slot.persistence_handle != Some(persistence)
            || challenge.is_none_or(|value| {
                !value.valid
                    || value.generation != slot.challenge_generation
                    || value.verifier != certificate.binding.principal
                    || now_us < value.started_at_us
                    || now_us >= value.deadline_us
            })
            || !lease_is_live(now_us, slot.local_deadline_us)
            || certificate.authority_principal != authority.authority_principal
            || certificate.authority_generation != authority.authority_generation
            || certificate.authority_lease_sequence != authority.lease_sequence
            || proof_mismatch(&self.config, &requirement, &proof, BINDING_RECORD_BYTES_U32)?
        {
            return Err(Error::State);
        }
        let slot = &mut self.bindings[index];
        slot.phase = BindingPhase::Active;
        slot.requirement = None;
        slot.persistence_handle = None;
        slot.previous_certificate = None;
        slot.previous_deadline_us = 0;
        if let Some(challenge) = self.binding_challenges.get_mut(challenge_index) {
            challenge.valid = false;
        }
        Ok(BindingView {
            runtime_instance: self.config.runtime_instance,
            identity_owner_instance: self.config.owner_instance,
            certificate,
            local_deadline_us: slot.local_deadline_us,
            slot_generation: slot.generation,
        })
    }

    /// 在尚未提交 Persistence 前取消 Binding 候选并恢复先前 live 快照。
    ///
    /// # Errors
    ///
    /// Handle 不匹配、不在等待持久化阶段，或已绑定 Persistence Handle 时返回状态错误。
    pub fn abort_binding(&mut self, binding: BindingHandle) -> Result<()> {
        let index = self.binding_index(binding)?;
        let slot = self.bindings[index];
        if slot.phase != BindingPhase::AwaitingDurability || slot.persistence_handle.is_some() {
            return Err(Error::State);
        }
        let generation = slot.generation;
        let challenge_index = usize::from(slot.challenge_slot.checked_sub(1).ok_or(Error::State)?);
        let challenge = self
            .binding_challenges
            .get(challenge_index)
            .ok_or(Error::State)?;
        if challenge.generation != slot.challenge_generation {
            return Err(Error::State);
        }
        self.bindings[index] = BindingSlot::EMPTY;
        self.bindings[index].generation = generation;
        if let Some(certificate) = slot.previous_certificate {
            self.bindings[index].occupied = true;
            self.bindings[index].phase = BindingPhase::Active;
            self.bindings[index].certificate = Some(certificate);
            self.bindings[index].local_deadline_us = slot.previous_deadline_us;
        }
        self.binding_challenges[challenge_index].valid = false;
        Ok(())
    }

    /// 显式过期未消费的 Authority/Binding challenge；不改写已发布权限。
    #[must_use]
    pub fn expire_challenges(&mut self, now_us: u64) -> usize {
        let mut expired = 0;
        if self.authority_challenge.valid && now_us >= self.authority_challenge.deadline_us {
            self.authority_challenge.valid = false;
            expired += 1;
        }
        for challenge in &mut self.binding_challenges {
            if challenge.valid && now_us >= challenge.deadline_us {
                challenge.valid = false;
                expired += 1;
            }
        }
        expired
    }

    /// 查询当前 live Binding。
    ///
    /// # Errors
    ///
    /// Handle 无效、Binding 未发布或租约到期时返回错误。
    pub fn binding_get(&self, binding: BindingHandle, now_us: u64) -> Result<BindingView> {
        let index = self.binding_index(binding)?;
        let slot = &self.bindings[index];
        if slot.phase != BindingPhase::Active || !lease_is_live(now_us, slot.local_deadline_us) {
            return Err(Error::Access);
        }
        Ok(BindingView {
            runtime_instance: self.config.runtime_instance,
            identity_owner_instance: self.config.owner_instance,
            certificate: slot.certificate.ok_or(Error::State)?,
            local_deadline_us: slot.local_deadline_us,
            slot_generation: slot.generation,
        })
    }

    /// 立即 Fence Authority 及全部活动 Binding；易失权限不可通过 reset 恢复。
    pub fn fence_authority(&mut self) {
        self.authority.phase = AuthorityPhase::Fenced;
        self.authority.local_deadline_us = 0;
        self.authority.requirement = None;
        self.authority.persistence_handle = None;
        self.authority_challenge.valid = false;
        for slot in &mut self.bindings {
            if slot.occupied {
                slot.phase = BindingPhase::Fenced;
                slot.local_deadline_us = 0;
                slot.requirement = None;
                slot.persistence_handle = None;
            }
        }
    }

    /// 按可信时间使 Authority/Binding 易失权限到期；历史高水位保留。
    pub fn expire(&mut self, now_us: u64) -> usize {
        let mut expired = 0;
        if self.authority.phase == AuthorityPhase::Active
            && !lease_is_live(now_us, self.authority.local_deadline_us)
        {
            self.fence_authority();
            expired += 1;
        }
        for slot in &mut self.bindings {
            if slot.phase == BindingPhase::Active && !lease_is_live(now_us, slot.local_deadline_us)
            {
                slot.phase = BindingPhase::Retired;
                slot.local_deadline_us = 0;
                expired += 1;
            }
        }
        expired
    }

    fn build_requirement<const N: usize>(
        &mut self,
        base: IdentityDurabilityBase,
        body: &[u8; N],
        schema_id: u16,
        schema_version: u16,
        operation_kind: u16,
    ) -> Result<IdentityDurabilityRequirement> {
        if N > MAX_REQUIREMENT_BODY_BYTES {
            return Err(Error::NoSpace);
        }
        let next_record_generation = base
            .expected_record_generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        if next_record_generation == u64::MAX {
            return Err(Error::Exhausted);
        }
        let body_bytes = u32::try_from(N).map_err(|_| Error::NoSpace)?;
        let meta = RecordMeta {
            domain: base.domain,
            record_generation: next_record_generation,
            transaction_id: base.next_transaction_id,
            body_bytes,
            schema_id,
            schema_version,
            operation_kind,
            body_digest: [0; DIGEST_BYTES],
        };
        let expected_published_digest = body_digest(&meta, body, &mut self.codec)?;
        let business_transition_digest = u64::from_be_bytes(
            expected_published_digest[..8]
                .try_into()
                .map_err(|_| Error::State)?,
        );
        if business_transition_digest == 0 {
            return Err(Error::State);
        }
        let mut stored_body = [0; MAX_REQUIREMENT_BODY_BYTES];
        stored_body[..N].copy_from_slice(body);
        Ok(IdentityDurabilityRequirement {
            base,
            body: stored_body,
            body_bytes,
            schema_id,
            schema_version,
            operation_kind,
            expected_published_digest,
            business_transition_digest,
        })
    }

    fn match_authority_challenge(&self, handle: AuthorityChallengeHandle) -> Result<Challenge> {
        let challenge = self.authority_challenge;
        if !challenge.valid
            || handle.owner_instance != self.config.owner_instance
            || handle.generation != challenge.generation
            || handle.nonce != challenge.nonce
            || handle.transaction_id != challenge.transaction_id
        {
            return Err(Error::NotFound);
        }
        Ok(challenge)
    }

    fn binding_challenge_index(&self, handle: BindingChallengeHandle) -> Result<usize> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let challenge = self.binding_challenges.get(index).ok_or(Error::NotFound)?;
        if !challenge.valid
            || handle.generation != challenge.generation
            || handle.nonce != challenge.nonce
            || handle.transaction_id != challenge.transaction_id
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn match_authority(&self, handle: AuthorityHandle) -> Result<()> {
        if handle.owner_instance != self.config.owner_instance
            || handle.generation != self.authority.generation
            || self.authority.epoch.is_none_or(|epoch| {
                epoch.authority_generation != handle.authority_generation
                    || epoch.lease_sequence != handle.lease_sequence
            })
        {
            return Err(Error::NotFound);
        }
        Ok(())
    }

    fn binding_index(&self, handle: BindingHandle) -> Result<usize> {
        if handle.owner_instance != self.config.owner_instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        let slot = self.bindings.get(index).ok_or(Error::NotFound)?;
        if !slot.occupied
            || slot.generation != handle.generation
            || slot.certificate.is_none_or(|certificate| {
                certificate.binding.generation != handle.binding_generation
            })
        {
            return Err(Error::NotFound);
        }
        Ok(index)
    }

    fn call_verify_authority<V: IdentityVerifier>(
        &mut self,
        verifier: &mut V,
        transition: &AuthorityTransition,
        proof: &[u8],
    ) -> Result<()> {
        let claim = self.next_claim(CALLBACK_AUTHORITY_VERIFY)?;
        let lease = self.config.provider_gate.try_enter(claim)?;
        let result = verifier.verify_authority_transition(transition, proof);
        let leave_result = self.config.provider_gate.leave(lease);
        result.and(leave_result)
    }

    #[allow(clippy::too_many_arguments)]
    fn call_verify_binding<V: IdentityVerifier>(
        &mut self,
        verifier: &mut V,
        authority: &AuthorityEpoch,
        certificate: &BindingCertificate,
        freshness: &AuthorityFreshness,
        challenge_started_local_us: u64,
        local_deadline_us: u64,
        proof: &[u8],
    ) -> Result<()> {
        let claim = self.next_claim(CALLBACK_BINDING_VERIFY)?;
        let lease = self.config.provider_gate.try_enter(claim)?;
        let result = verifier.verify_binding_issue(
            authority,
            certificate,
            freshness,
            challenge_started_local_us,
            local_deadline_us,
            proof,
        );
        let leave_result = self.config.provider_gate.leave(lease);
        result.and(leave_result)
    }

    fn next_claim(&mut self, kind: u16) -> Result<CallbackClaim> {
        let operation_id = self.next_operation_id;
        self.next_operation_id = self
            .next_operation_id
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        CallbackClaim::new(self.config.owner_instance, operation_id, 1, kind)
    }
}

fn validate_lease_policy(policy: LeasePolicy) -> Result<()> {
    if policy.local_timer_max_slow_ppm > 1_000_000
        || policy.local_timer_resolution_us == 0
        || !policy.timer_read_uncertainty_known
        || policy.local_policy_max_lease_us == 0
    {
        return Err(Error::Config);
    }
    Ok(())
}

fn validate_authority_transition(
    kind: AuthorityTransitionKind,
    committed: Option<AuthorityEpoch>,
    proposed: AuthorityEpoch,
) -> Result<()> {
    match (kind, committed) {
        (AuthorityTransitionKind::Initial, None) => {
            if proposed.authority_generation.get() != 1 || proposed.lease_sequence != 1 {
                return Err(Error::Replay);
            }
        }
        (AuthorityTransitionKind::Freshness, Some(previous)) => {
            if proposed != previous {
                return Err(Error::Replay);
            }
        }
        (AuthorityTransitionKind::Renewal, Some(previous)) => {
            if proposed.authority_principal != previous.authority_principal
                || proposed.authority_generation != previous.authority_generation
                || proposed.durable_fence_token != previous.durable_fence_token
                || proposed.lease_sequence
                    != previous
                        .lease_sequence
                        .checked_add(1)
                        .ok_or(Error::Exhausted)?
                || proposed.allocation_high_water_digest != previous.allocation_high_water_digest
                || proposed.quorum_config_digest != previous.quorum_config_digest
                || proposed.signer_set_digest != previous.signer_set_digest
            {
                return Err(Error::Replay);
            }
        }
        (AuthorityTransitionKind::Transfer, Some(previous)) => {
            if proposed.authority_principal == previous.authority_principal
                || proposed.authority_generation != previous.authority_generation.checked_next()?
                || proposed.durable_fence_token == previous.durable_fence_token
                || proposed.lease_sequence
                    != previous
                        .lease_sequence
                        .checked_add(1)
                        .ok_or(Error::Exhausted)?
            {
                return Err(Error::Replay);
            }
        }
        _ => return Err(Error::State),
    }
    Ok(())
}

fn proof_mismatch(
    config: &IdentityConfig<'_>,
    requirement: &IdentityDurabilityRequirement,
    proof: &PersistenceProof,
    body_bytes: u32,
) -> Result<bool> {
    Ok(proof.runtime_instance() != config.runtime_instance
        || proof.persistence_owner_instance() != config.persistence_owner_instance
        || proof.caller_owner_instance() != config.persistence_business_owner_instance
        || proof.domain_generation() != requirement.base.domain_generation
        || proof.operation_kind() != requirement.operation_kind
        || proof.domain() != requirement.base.domain
        || proof.record_generation()
            != requirement
                .base
                .expected_record_generation
                .checked_add(1)
                .ok_or(Error::Exhausted)?
        || proof.foundation_transaction_id() != requirement.base.next_transaction_id
        || proof.witness_generation() != proof.record_generation()
        || proof.body_bytes() != body_bytes
        || proof.body_digest() != requirement.expected_published_digest
        || proof.volatile_continuation() != requirement.base.volatile_continuation)
}

fn encode_authority_record(epoch: AuthorityEpoch) -> [u8; AUTHORITY_RECORD_BYTES] {
    let mut output = [0; AUTHORITY_RECORD_BYTES];
    output[..4].copy_from_slice(AUTHORITY_MAGIC);
    output[4..6].copy_from_slice(&AUTHORITY_SCHEMA_VERSION.to_be_bytes());
    output[8..12].copy_from_slice(&epoch.realm.get().to_be_bytes());
    output[12..28].copy_from_slice(&epoch.authority_principal.bytes());
    output[28..32].copy_from_slice(&epoch.authority_generation.get().to_be_bytes());
    output[32..48].copy_from_slice(&epoch.durable_fence_token);
    output[48..56].copy_from_slice(&epoch.lease_sequence.to_be_bytes());
    output[56..64].copy_from_slice(&epoch.lease_duration_us.to_be_bytes());
    output[64..80].copy_from_slice(&epoch.allocation_high_water_digest);
    output[80..112].copy_from_slice(&epoch.quorum_config_digest);
    output[112..144].copy_from_slice(&epoch.signer_set_digest);
    output[144..176].copy_from_slice(&epoch.threshold_proof_digest);
    output[176..178].copy_from_slice(&epoch.signer_count.to_be_bytes());
    output[178..180].copy_from_slice(&epoch.quorum_threshold.to_be_bytes());
    output
}

/// 解码并验证固定 192 B Authority Record。
///
/// # Errors
///
/// 长度、Magic、保留位或任一 Authority 字段非法时返回 malformed/argument。
pub fn decode_authority_record(input: &[u8]) -> Result<AuthorityEpoch> {
    if input.len() != AUTHORITY_RECORD_BYTES
        || &input[..4] != AUTHORITY_MAGIC
        || u16::from_be_bytes(input[4..6].try_into().map_err(|_| Error::Malformed)?)
            != AUTHORITY_SCHEMA_VERSION
        || input[6..8].iter().any(|byte| *byte != 0)
        || input[180..].iter().any(|byte| *byte != 0)
    {
        return Err(Error::Malformed);
    }
    let epoch = AuthorityEpoch {
        realm: RealmId::new(u32::from_be_bytes(
            input[8..12].try_into().map_err(|_| Error::Malformed)?,
        ))
        .map_err(|_| Error::Malformed)?,
        authority_principal: Principal::new(
            input[12..28].try_into().map_err(|_| Error::Malformed)?,
        )
        .map_err(|_| Error::Malformed)?,
        authority_generation: AddressAuthorityGeneration::new(u32::from_be_bytes(
            input[28..32].try_into().map_err(|_| Error::Malformed)?,
        ))
        .map_err(|_| Error::Malformed)?,
        durable_fence_token: input[32..48].try_into().map_err(|_| Error::Malformed)?,
        lease_sequence: u64::from_be_bytes(input[48..56].try_into().map_err(|_| Error::Malformed)?),
        lease_duration_us: u64::from_be_bytes(
            input[56..64].try_into().map_err(|_| Error::Malformed)?,
        ),
        allocation_high_water_digest: input[64..80].try_into().map_err(|_| Error::Malformed)?,
        quorum_config_digest: input[80..112].try_into().map_err(|_| Error::Malformed)?,
        signer_set_digest: input[112..144].try_into().map_err(|_| Error::Malformed)?,
        threshold_proof_digest: input[144..176].try_into().map_err(|_| Error::Malformed)?,
        signer_count: u16::from_be_bytes(input[176..178].try_into().map_err(|_| Error::Malformed)?),
        quorum_threshold: u16::from_be_bytes(
            input[178..180].try_into().map_err(|_| Error::Malformed)?,
        ),
    };
    epoch.validate(epoch.realm).map_err(|_| Error::Malformed)?;
    Ok(epoch)
}

fn encode_binding_record(certificate: BindingCertificate) -> [u8; BINDING_RECORD_BYTES] {
    let mut output = [0; BINDING_RECORD_BYTES];
    output[..4].copy_from_slice(BINDING_MAGIC);
    output[4..6].copy_from_slice(&BINDING_SCHEMA_VERSION.to_be_bytes());
    output[6] = 1;
    output[7] = certificate.mode as u8;
    output[8..12].copy_from_slice(&certificate.binding.realm.get().to_be_bytes());
    output[12] = certificate.binding.address_width as u8;
    output[16..20].copy_from_slice(&certificate.binding.address.get().to_be_bytes());
    output[20..24].copy_from_slice(&certificate.binding.generation.get().to_be_bytes());
    output[24..40].copy_from_slice(&certificate.binding.principal.bytes());
    output[40..56].copy_from_slice(&certificate.authority_principal.bytes());
    output[56..60].copy_from_slice(&certificate.authority_generation.get().to_be_bytes());
    output[60..76].copy_from_slice(&certificate.lease_id);
    output[76..84].copy_from_slice(&certificate.lease_duration_us.to_be_bytes());
    output[84..92].copy_from_slice(&certificate.authority_lease_sequence.to_be_bytes());
    output
}

/// 解码并验证固定 96 B Binding Record。
///
/// # Errors
///
/// 长度、Magic、保留位、地址、代际或证书字段非法时返回错误。
pub fn decode_binding_record(input: &[u8]) -> Result<BindingCertificate> {
    if input.len() != BINDING_RECORD_BYTES
        || &input[..4] != BINDING_MAGIC
        || u16::from_be_bytes(input[4..6].try_into().map_err(|_| Error::Malformed)?)
            != BINDING_SCHEMA_VERSION
        || input[6] != 1
        || input[13..16].iter().any(|byte| *byte != 0)
        || input[92..].iter().any(|byte| *byte != 0)
    {
        return Err(Error::Malformed);
    }
    let mode = match input[7] {
        1 => AddressMode::Static,
        2 => AddressMode::Leased,
        3 => AddressMode::SelfProposed,
        _ => return Err(Error::Malformed),
    };
    let width = AddressWidth::try_from(input[12]).map_err(|_| Error::Malformed)?;
    let certificate = BindingCertificate {
        binding: crate::IdentityBinding {
            realm: RealmId::new(u32::from_be_bytes(
                input[8..12].try_into().map_err(|_| Error::Malformed)?,
            ))
            .map_err(|_| Error::Malformed)?,
            address_width: width,
            address: NodeAddress::new(
                u32::from_be_bytes(input[16..20].try_into().map_err(|_| Error::Malformed)?),
                width,
            )
            .map_err(|_| Error::Malformed)?,
            generation: BindingGeneration::active(u32::from_be_bytes(
                input[20..24].try_into().map_err(|_| Error::Malformed)?,
            ))
            .map_err(|_| Error::Malformed)?,
            principal: Principal::new(input[24..40].try_into().map_err(|_| Error::Malformed)?)
                .map_err(|_| Error::Malformed)?,
        },
        authority_principal: Principal::new(
            input[40..56].try_into().map_err(|_| Error::Malformed)?,
        )
        .map_err(|_| Error::Malformed)?,
        authority_generation: AddressAuthorityGeneration::new(u32::from_be_bytes(
            input[56..60].try_into().map_err(|_| Error::Malformed)?,
        ))
        .map_err(|_| Error::Malformed)?,
        lease_id: input[60..76].try_into().map_err(|_| Error::Malformed)?,
        lease_duration_us: u64::from_be_bytes(
            input[76..84].try_into().map_err(|_| Error::Malformed)?,
        ),
        authority_lease_sequence: u64::from_be_bytes(
            input[84..92].try_into().map_err(|_| Error::Malformed)?,
        ),
        mode,
    };
    certificate
        .validate(certificate.binding.realm, width)
        .map_err(|_| Error::Malformed)?;
    Ok(certificate)
}

#[cfg(test)]
mod tests {
    use super::{
        AuthorityPhase, AuthoritySlot, BindingPhase, BindingSlot, IdentityConfig,
        IdentityDurabilityBase, IdentityOwner, PriorRecord, decode_authority_record,
        decode_binding_record, encode_authority_record, encode_binding_record,
    };
    use crate::{
        AddressMode, AuthorityEpoch, AuthorityFreshness, AuthorityTransition,
        AuthorityTransitionKind, BindingCertificate, IdentityBinding, IdentityProof,
        IdentityVerifier, LeasePolicy, Principal,
    };
    use ucn_owner::CallbackGate;
    use ucn_persistence::{DIGEST_BYTES, DomainKey, DomainKind};
    use ucn_types::{
        AddressAuthorityGeneration, AddressWidth, BindingGeneration, Error, NodeAddress, RealmId,
        Result,
    };

    const AUTHORITY_DOMAIN: DomainKey = DomainKey {
        kind: DomainKind::IdentityBinding,
        id: 1,
    };
    const BINDING_DOMAIN: DomainKey = DomainKey {
        kind: DomainKind::IdentityBinding,
        id: 2,
    };
    const POLICY: LeasePolicy = LeasePolicy {
        local_timer_max_slow_ppm: 0,
        local_timer_resolution_us: 1,
        local_timer_read_uncertainty_us: 0,
        timer_read_uncertainty_known: true,
        local_policy_max_lease_us: 100_000,
    };

    struct Verifier;

    impl IdentityVerifier for Verifier {
        fn verify_authority_transition(
            &mut self,
            _transition: &AuthorityTransition,
            proof: &[u8],
        ) -> Result<()> {
            if proof == b"proof" {
                Ok(())
            } else {
                Err(Error::Security)
            }
        }

        fn verify_binding_issue(
            &mut self,
            _authority: &AuthorityEpoch,
            _certificate: &BindingCertificate,
            _freshness: &AuthorityFreshness,
            _challenge_started_local_us: u64,
            _local_deadline_us: u64,
            proof: &[u8],
        ) -> Result<()> {
            if proof == b"proof" {
                Ok(())
            } else {
                Err(Error::Security)
            }
        }
    }

    fn principal(value: u8) -> Principal {
        Principal::new([value; 16]).expect("principal")
    }

    fn epoch(principal_value: u8, generation: u32, sequence: u64) -> AuthorityEpoch {
        AuthorityEpoch {
            realm: RealmId::new(1).expect("realm"),
            authority_principal: principal(principal_value),
            authority_generation: AddressAuthorityGeneration::new(generation)
                .expect("authority generation"),
            durable_fence_token: [principal_value.wrapping_add(1); 16],
            lease_sequence: sequence,
            lease_duration_us: 50_000,
            allocation_high_water_digest: [0x22; 16],
            quorum_config_digest: [0x23; 32],
            signer_set_digest: [0x24; 32],
            threshold_proof_digest: [0x25; 32],
            signer_count: 3,
            quorum_threshold: 2,
        }
    }

    fn freshness(
        verifier: Principal,
        nonce: u64,
        transaction_id: u64,
        sequence: u64,
    ) -> AuthorityFreshness {
        AuthorityFreshness {
            verifier_principal: verifier,
            challenge_nonce: nonce,
            transaction_id,
            authority_lease_sequence: sequence,
            max_remaining_lease_us: 40_000,
            binding_lease_id: [0; 16],
            binding_generation: 0,
            proof_transcript_hash: [0x31; 32],
        }
    }

    fn certificate(generation: u32, lease: u8) -> BindingCertificate {
        BindingCertificate {
            binding: IdentityBinding {
                realm: RealmId::new(1).expect("realm"),
                address_width: AddressWidth::A1,
                address: NodeAddress::new(7, AddressWidth::A1).expect("address"),
                generation: BindingGeneration::active(generation).expect("binding generation"),
                principal: principal(0x51),
            },
            authority_principal: principal(0x41),
            authority_generation: AddressAuthorityGeneration::new(1).expect("authority generation"),
            lease_id: [lease; 16],
            lease_duration_us: 30_000,
            authority_lease_sequence: 1,
            mode: AddressMode::Leased,
        }
    }

    fn base(domain: DomainKey, prior: PriorRecord) -> IdentityDurabilityBase {
        IdentityDurabilityBase {
            runtime_instance: 11,
            caller_owner_instance: 21,
            domain_generation: 31,
            domain,
            next_transaction_id: 1,
            expected_record_generation: u64::from(!matches!(prior, PriorRecord::Empty)),
            expected_body_digest: [0; DIGEST_BYTES],
            absolute_deadline_us: 100_000,
            volatile_continuation: 77,
            prior,
        }
    }

    fn owner<'a>(gate: &'a CallbackGate, domains: &'a [DomainKey; 1]) -> IdentityOwner<'a, 1> {
        IdentityOwner::new(IdentityConfig {
            runtime_instance: 11,
            owner_instance: 41,
            persistence_business_owner_instance: 21,
            persistence_owner_instance: 12,
            realm: RealmId::new(1).expect("realm"),
            address_width: AddressWidth::A1,
            provider_gate: gate,
            authority_domain: AUTHORITY_DOMAIN,
            binding_domains: domains,
            challenge_lifetime_us: 10_000,
            authority_lease_policy: POLICY,
            binding_lease_policy: POLICY,
        })
        .expect("identity owner")
    }

    #[test]
    fn record_codecs_are_exact_and_reject_reserved_bytes() {
        let authority = epoch(0x41, 1, 1);
        let bytes = encode_authority_record(authority);
        assert_eq!(decode_authority_record(&bytes), Ok(authority));
        let mut corrupted = bytes;
        corrupted[6] = 1;
        assert_eq!(decode_authority_record(&corrupted), Err(Error::Malformed));

        let binding = certificate(1, 0x61);
        let bytes = encode_binding_record(binding);
        assert_eq!(decode_binding_record(&bytes), Ok(binding));
        let mut corrupted = bytes;
        corrupted[13] = 1;
        assert_eq!(decode_binding_record(&corrupted), Err(Error::Malformed));
    }

    #[test]
    fn authority_candidate_abort_restores_prior_live_epoch() {
        let gate = CallbackGate::new(1).expect("gate");
        let domains = [BINDING_DOMAIN];
        let mut owner = owner(&gate, &domains);
        let previous = epoch(0x41, 1, 1);
        owner.authority = AuthoritySlot {
            generation: 1,
            phase: AuthorityPhase::Active,
            epoch: Some(previous),
            local_deadline_us: 90_000,
            requirement: None,
            persistence_handle: None,
            transition_kind: None,
            previous_epoch: None,
            previous_deadline_us: 0,
        };
        let challenge = owner
            .begin_authority_challenge(previous.authority_principal, 7, 8, 100)
            .expect("challenge");
        let mut proposed = previous;
        proposed.lease_sequence = 2;
        let mut verifier = Verifier;
        let (handle, _) = owner
            .prepare_authority(
                &mut verifier,
                challenge,
                AuthorityTransitionKind::Renewal,
                proposed,
                freshness(previous.authority_principal, 7, 8, 2),
                IdentityProof { bytes: b"proof" },
                base(AUTHORITY_DOMAIN, PriorRecord::Authority(previous)),
                200,
            )
            .expect("prepare renewal");
        assert_eq!(owner.authority_get(200), Err(Error::Access));
        owner.abort_authority(handle).expect("abort candidate");
        assert_eq!(owner.authority_get(200), Ok(previous));
    }

    #[test]
    fn binding_candidate_abort_restores_prior_live_certificate() {
        let gate = CallbackGate::new(1).expect("gate");
        let domains = [BINDING_DOMAIN];
        let mut owner = owner(&gate, &domains);
        let authority = epoch(0x41, 1, 1);
        owner.authority = AuthoritySlot {
            generation: 1,
            phase: AuthorityPhase::Active,
            epoch: Some(authority),
            local_deadline_us: 90_000,
            requirement: None,
            persistence_handle: None,
            transition_kind: None,
            previous_epoch: None,
            previous_deadline_us: 0,
        };
        let previous = certificate(1, 0x61);
        owner.bindings[0] = BindingSlot {
            occupied: true,
            generation: 1,
            phase: BindingPhase::Active,
            certificate: Some(previous),
            local_deadline_us: 80_000,
            requirement: None,
            persistence_handle: None,
            previous_certificate: None,
            previous_deadline_us: 0,
            challenge_slot: 0,
            challenge_generation: 0,
        };
        let challenge = owner
            .begin_binding_challenge(previous.binding.principal, 17, 18, 100)
            .expect("challenge");
        let proposed = certificate(2, 0x62);
        let mut proof_freshness = freshness(previous.binding.principal, 17, 18, 1);
        proof_freshness.max_remaining_lease_us = 20_000;
        proof_freshness.binding_lease_id = proposed.lease_id;
        proof_freshness.binding_generation = proposed.binding.generation.get();
        let mut verifier = Verifier;
        let (handle, _) = owner
            .prepare_binding(
                &mut verifier,
                challenge,
                proposed,
                proof_freshness,
                IdentityProof { bytes: b"proof" },
                base(BINDING_DOMAIN, PriorRecord::Binding(previous)),
                200,
            )
            .expect("prepare binding");
        assert_eq!(owner.binding_get(handle, 200), Err(Error::Access));
        owner.abort_binding(handle).expect("abort binding");
        assert_eq!(owner.bindings[0].certificate, Some(previous));
        assert_eq!(owner.bindings[0].local_deadline_us, 80_000);
    }
}
