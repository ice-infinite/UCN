use ucn_types::{
    AddressAuthorityGeneration, AddressWidth, BindingGeneration, NodeAddress, RealmId, Result,
};

use crate::LeasePolicy;

/// 128-bit 稳定设备 Principal。
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct Principal([u8; 16]);

impl Principal {
    pub(crate) const INVALID: Self = Self([0; 16]);

    /// 构造非全零、非全一 Principal。
    ///
    /// # Errors
    ///
    /// 全零或全一输入返回参数错误。
    pub fn new(bytes: [u8; 16]) -> Result<Self> {
        if bytes.iter().all(|byte| *byte == 0) || bytes.iter().all(|byte| *byte == u8::MAX) {
            return Err(ucn_types::Error::Argument);
        }
        Ok(Self(bytes))
    }

    /// 返回规范 16 B 表示。
    #[must_use]
    pub const fn bytes(self) -> [u8; 16] {
        self.0
    }
}

/// Identity Owner 发布的完整地址绑定。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct IdentityBinding {
    /// Realm。
    pub realm: RealmId,
    /// 地址宽度。
    pub address_width: AddressWidth,
    /// 当前地址。
    pub address: NodeAddress,
    /// 同一 `{realm,address}` 下不可回绕的 Binding Generation。
    pub generation: BindingGeneration,
    /// 当前设备 Principal。
    pub principal: Principal,
}

/// 地址来源模式；三种模式都必须由当前 Authority 最终签发。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AddressMode {
    /// 签名 Manifest 预留地址。
    Static = 1,
    /// Authority 租约分配。
    Leased = 2,
    /// 设备自提议、Authority 最终签发。
    SelfProposed = 3,
}

/// 一个 Realm 的持久化 Address Authority Epoch。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AuthorityEpoch {
    /// Realm。
    pub realm: RealmId,
    /// Authority Principal。
    pub authority_principal: Principal,
    /// Authority Generation。
    pub authority_generation: AddressAuthorityGeneration,
    /// 不可平凡的 durable Fence token。
    pub durable_fence_token: [u8; 16],
    /// Realm 内不可回绕的 Authority Lease Sequence。
    pub lease_sequence: u64,
    /// 本 Epoch 最大租期，不是跨节点绝对截止期。
    pub lease_duration_us: u64,
    /// 已持久化分配高水位摘要。
    pub allocation_high_water_digest: [u8; 16],
    /// Quorum Config 摘要。
    pub quorum_config_digest: [u8; 32],
    /// Signer set 摘要。
    pub signer_set_digest: [u8; 32],
    /// Threshold proof 摘要。
    pub threshold_proof_digest: [u8; 32],
    /// 实际 signer 数量。
    pub signer_count: u16,
    /// Quorum 门限。
    pub quorum_threshold: u16,
}

impl AuthorityEpoch {
    pub(crate) fn validate(self, realm: RealmId) -> Result<()> {
        if self.realm != realm
            || trivial(&self.durable_fence_token)
            || self.lease_sequence == 0
            || self.lease_sequence == u64::MAX
            || self.lease_duration_us == 0
            || trivial(&self.allocation_high_water_digest)
            || trivial(&self.quorum_config_digest)
            || trivial(&self.signer_set_digest)
            || trivial(&self.threshold_proof_digest)
            || self.signer_count == 0
            || self.quorum_threshold == 0
            || self.quorum_threshold > self.signer_count
        {
            return Err(ucn_types::Error::Argument);
        }
        Ok(())
    }
}

/// 面向一个验证者的新鲜度声明；不携带远端绝对 Deadline。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AuthorityFreshness {
    /// 本证明的验证者。
    pub verifier_principal: Principal,
    /// 验证者锁存的非零 challenge nonce。
    pub challenge_nonce: u64,
    /// 精确 Bootstrap/Authority 事务 ID。
    pub transaction_id: u64,
    /// 当前 Authority Lease Sequence。
    pub authority_lease_sequence: u64,
    /// Authority 可安全承诺的最大剩余时长。
    pub max_remaining_lease_us: u64,
    /// Binding 验证时的 Lease ID；纯 Authority 验证为全零。
    pub binding_lease_id: [u8; 16],
    /// Binding 验证时的 Generation；纯 Authority 验证为零。
    pub binding_generation: u32,
    /// 完整证明 transcript 摘要。
    pub proof_transcript_hash: [u8; 32],
}

/// Authority Epoch 转换类别。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AuthorityTransitionKind {
    /// Factory 首次建立。
    Initial = 1,
    /// 同一 Epoch 的新鲜度复核，不改变 durable Record。
    Freshness = 2,
    /// 同一 Authority/Fence 的 checked-next Lease Sequence 续期。
    Renewal = 3,
    /// 新 Authority、checked-next Generation 与新 Fence。
    Transfer = 4,
}

/// 提交给可信 Verifier 的完整 Authority 转换候选。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AuthorityTransition {
    /// 转换类别。
    pub kind: AuthorityTransitionKind,
    /// 当前已提交 Epoch；Factory 为 None。
    pub committed: Option<AuthorityEpoch>,
    /// 候选 Epoch。
    pub proposed: AuthorityEpoch,
    /// 精确新鲜度证明域。
    pub freshness: AuthorityFreshness,
    /// Identity Owner 自己锁存的本地 Challenge 起点。
    pub challenge_started_local_us: u64,
    /// 本地保守租约策略。
    pub lease_policy: LeasePolicy,
    /// 根据上述输入推导的本地半开 Deadline。
    pub derived_local_deadline_us: u64,
}

/// 一个 durable Address Binding Certificate。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BindingCertificate {
    /// 设备绑定。
    pub binding: IdentityBinding,
    /// 签发 Authority Principal。
    pub authority_principal: Principal,
    /// 签发 Authority Generation。
    pub authority_generation: AddressAuthorityGeneration,
    /// 非平凡 Lease ID。
    pub lease_id: [u8; 16],
    /// 签发的最大租期。
    pub lease_duration_us: u64,
    /// 签发时 Authority Lease Sequence。
    pub authority_lease_sequence: u64,
    /// 地址来源模式。
    pub mode: AddressMode,
}

impl BindingCertificate {
    pub(crate) fn validate(self, realm: RealmId, width: AddressWidth) -> Result<()> {
        if self.binding.realm != realm
            || self.binding.address_width != width
            || trivial(&self.lease_id)
            || self.lease_duration_us == 0
            || self.authority_lease_sequence == 0
            || self.authority_lease_sequence == u64::MAX
        {
            return Err(ucn_types::Error::Argument);
        }
        Ok(())
    }
}

/// 当前可用 Binding 的只读快照。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BindingView {
    pub(crate) runtime_instance: u32,
    pub(crate) identity_owner_instance: u32,
    /// 证书。
    pub(crate) certificate: BindingCertificate,
    /// 验证端本地半开 Lease Deadline。
    pub(crate) local_deadline_us: u64,
    /// Identity Owner slot generation。
    pub(crate) slot_generation: u32,
}

impl BindingView {
    /// 返回签发该 View 的 Runtime 实例。
    #[must_use]
    pub const fn runtime_instance(self) -> u32 {
        self.runtime_instance
    }

    /// 返回签发该 View 的 Identity Owner 实例。
    #[must_use]
    pub const fn identity_owner_instance(self) -> u32 {
        self.identity_owner_instance
    }

    /// 返回 durable Binding Certificate。
    #[must_use]
    pub const fn certificate(self) -> BindingCertificate {
        self.certificate
    }

    /// 返回验证端本地半开 Lease Deadline。
    #[must_use]
    pub const fn local_deadline_us(self) -> u64 {
        self.local_deadline_us
    }

    /// 返回 Identity Owner slot generation。
    #[must_use]
    pub const fn slot_generation(self) -> u32 {
        self.slot_generation
    }
}

/// 可信 Identity Verifier 使用的有界证明字节。
#[derive(Clone, Copy)]
pub struct IdentityProof<'a> {
    /// 产品 Provider 唯一解释的证明内容。
    pub bytes: &'a [u8],
}

/// 产品提供的 Authority/Binding 身份与 quorum 证明验证器。
pub trait IdentityVerifier {
    /// 验证完整 Authority 转换。
    ///
    /// # Errors
    ///
    /// 身份、签名、Quorum、Fence 或 transcript 证明不成立时失败关闭。
    fn verify_authority_transition(
        &mut self,
        transition: &AuthorityTransition,
        proof: &[u8],
    ) -> Result<()>;

    /// 验证 Binding 签发及其新鲜度证明。
    ///
    /// # Errors
    ///
    /// 设备身份、Authority、Lease、Binding 或 transcript 证明不成立时失败关闭。
    fn verify_binding_issue(
        &mut self,
        authority: &AuthorityEpoch,
        certificate: &BindingCertificate,
        freshness: &AuthorityFreshness,
        challenge_started_local_us: u64,
        local_deadline_us: u64,
        proof: &[u8],
    ) -> Result<()>;
}

pub(crate) fn trivial<const N: usize>(bytes: &[u8; N]) -> bool {
    bytes.iter().all(|byte| *byte == 0) || bytes.iter().all(|byte| *byte == u8::MAX)
}
