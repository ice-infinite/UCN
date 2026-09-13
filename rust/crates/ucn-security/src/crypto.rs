use ucn_types::{Error, KeyId, Result, SuiteId};

use crate::session::HandshakeCandidate;
use crate::{HOP_TAG_BYTES, NONCE_BYTES, ORIGIN_TAG_BYTES};

/// 一次 O2 加密调用的完整、借用式参数集合。
pub struct SealOriginRequest<'a> {
    /// 精确 Origin Key selector。
    pub selector: KeySelector,
    /// canonical Nonce 派生输入。
    pub nonce_input: &'a [u8],
    /// canonical Origin AAD。
    pub aad: &'a [u8],
    /// 待加密明文。
    pub plaintext: &'a [u8],
    /// 与明文等长的密文输出。
    pub ciphertext: &'a mut [u8],
    /// 固定长度 Origin Tag 输出。
    pub tag: &'a mut [u8; ORIGIN_TAG_BYTES],
    /// Provider 实际派生的 Nonce 输出，用于独立审计。
    pub derived_nonce: &'a mut [u8; NONCE_BYTES],
}

/// 一次 O2 解密调用的完整、借用式参数集合。
pub struct OpenOriginRequest<'a> {
    /// 精确 Origin Key selector。
    pub selector: KeySelector,
    /// canonical Nonce 派生输入。
    pub nonce_input: &'a [u8],
    /// canonical Origin AAD。
    pub aad: &'a [u8],
    /// 待认证密文。
    pub ciphertext: &'a [u8],
    /// 固定长度 Origin Tag。
    pub tag: &'a [u8; ORIGIN_TAG_BYTES],
    /// 与密文等长的明文 staging 输出。
    pub plaintext: &'a mut [u8],
    /// Provider 实际派生的 Nonce 输出，用于独立审计。
    pub derived_nonce: &'a mut [u8; NONCE_BYTES],
}

/// 密码 Provider 中一个不可伪造的 Key 选择器；实际密钥字节永不进入协议状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct KeySelector {
    /// 冻结 Suite Registry 值。
    pub suite: SuiteId,
    /// Provider 的非零、非保留 Key ID。
    pub key_id: KeyId,
    /// 该 Key 槽的非零 Generation。
    pub key_generation: u32,
}

impl KeySelector {
    /// 验证 Origin selector 是否与 O1/O2 要求一致。
    pub(crate) const fn validate_origin(self, confidential: bool) -> Result<()> {
        if self.key_generation == 0 {
            return Err(Error::Argument);
        }
        match (confidential, self.suite) {
            (false, SuiteId::OriginHmacSha256_128)
            | (true, SuiteId::OriginAes128Gcm | SuiteId::OriginChaCha20Poly1305) => Ok(()),
            _ => Err(Error::Argument),
        }
    }

    /// 验证逐跳 selector。
    pub(crate) const fn validate_hop(self) -> Result<()> {
        if self.key_generation == 0 || !matches!(self.suite, SuiteId::HopHmacSha256_96) {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// 产品提供的密码运算边界。
///
/// Provider 必须有界、同步返回并把所有正常失败编码成 [`Result`]。密钥由 selector 在 Provider
/// 内部唯一解析，禁止尝试 previous/current 多把 Key。协议 Owner 始终使用私有 staging，只有
/// Provider 成功后才复制到调用方输出。
pub trait CryptoProvider {
    /// 验证静态 Peer Session 的双向握手证明。
    ///
    /// # Errors
    ///
    /// 证明未精确绑定候选中的双方 Principal/Binding、Link、Session/Policy/Key Generation、
    /// Security Profile、Context fingerprint、到期时间或 transcript 时必须返回错误。Provider
    /// 不得只核对 Principal 或只核对 transcript 摘要。
    fn verify_session_proof(&mut self, candidate: &HandshakeCandidate, proof: &[u8]) -> Result<()>;

    /// 计算 O1 HMAC-SHA-256-128 Tag。
    ///
    /// # Errors
    ///
    /// Selector、输入或 Provider 状态无效时返回错误。
    fn compute_origin_tag(
        &mut self,
        selector: KeySelector,
        aad: &[u8],
        plaintext: &[u8],
        tag: &mut [u8; ORIGIN_TAG_BYTES],
    ) -> Result<()>;

    /// 验证 O1 Tag。
    ///
    /// # Errors
    ///
    /// Tag 或安全上下文不匹配时返回错误。
    fn verify_origin_tag(
        &mut self,
        selector: KeySelector,
        aad: &[u8],
        plaintext: &[u8],
        tag: &[u8; ORIGIN_TAG_BYTES],
    ) -> Result<()>;

    /// 使用 Provider 内与 selector 同方向绑定的 nonce key 派生 96-bit Nonce 并执行 O2 AEAD。
    ///
    /// # Errors
    ///
    /// Selector、Nonce 域、缓冲区或密码运算无效时返回错误。
    fn seal_origin(&mut self, request: SealOriginRequest<'_>) -> Result<()>;

    /// 验证并打开 O2 AEAD。
    ///
    /// # Errors
    ///
    /// Tag、Selector、Nonce 域、缓冲区或密码运算无效时返回错误。
    fn open_origin(&mut self, request: OpenOriginRequest<'_>) -> Result<()>;

    /// 计算 H1/H3 HMAC-SHA-256-96 Tag。
    ///
    /// # Errors
    ///
    /// Selector、输入或 Provider 状态无效时返回错误。
    fn compute_hop_tag(
        &mut self,
        selector: KeySelector,
        aad: &[u8],
        tag: &mut [u8; HOP_TAG_BYTES],
    ) -> Result<()>;

    /// 验证 H1/H3 Tag。
    ///
    /// # Errors
    ///
    /// Tag 或逐跳安全上下文不匹配时返回错误。
    fn verify_hop_tag(
        &mut self,
        selector: KeySelector,
        aad: &[u8],
        tag: &[u8; HOP_TAG_BYTES],
    ) -> Result<()>;
}
