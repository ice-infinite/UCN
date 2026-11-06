//! Source-oriented Profile codecs and exact SELECT transcript; 源视角Profile编解码与精确事务摘要。
use crate::{MessageClass, ProfileAck, ProfileSelect};
use ucn_digest::{DigestWorkspace, canonical_digest};
use ucn_security::Binding;
use ucn_types::{Error, OriginSecurity, Result};
/// Exact SELECT Body length; 精确SELECT正文长度。
pub const PROFILE_SELECT_BYTES: usize = 59;
/// Exact ACK Body length; 精确ACK正文长度。
pub const PROFILE_ACK_BYTES: usize = 75;
impl ProfileSelect {
    /// Match the canonical C structural contract, not permission; 对齐C结构合同，不代表许可。
    /// # Errors
    /// Reserved features, zero identifiers/digests/windows or illegal suites reject.
    pub fn validate(self) -> Result<()> {
        if self.local_generation == 0
            || self.peer_generation == 0
            || self.local_digest == [0; 16]
            || self.peer_digest == [0; 16]
            || self.feature_bits & !0x07ff != 0
            || self.feature_bits & 0x010b != 0x010b
            || self.hop_suite_bits == 0
            || self.hop_suite_bits & !2 != 0
            || self.e2e_suite_bits == 0
            || self.e2e_suite_bits & !0x000e != 0
            || self.max_rx_window == 0
            || self.max_concurrent_transfers == 0
            || self.realtime_mode_bits & !7 != 0
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
    /// Reverse only the two party identities, not symmetric intersection fields.
    /// 仅反转双方身份，不改变交集；目标重建后转回源视角再比较。
    #[must_use]
    pub const fn reverse(self) -> Self {
        Self {
            local_generation: self.peer_generation,
            peer_generation: self.local_generation,
            local_digest: self.peer_digest,
            peer_digest: self.local_digest,
            ..self
        }
    }
}
/// Encode independent canonical59 bytes; 编码独立规范59字节。
/// # Errors
/// Invalid fields or non-exact capacity rejects without writing output.
pub fn encode_profile_select(v: ProfileSelect, output: &mut [u8]) -> Result<()> {
    v.validate()?;
    if output.len() != PROFILE_SELECT_BYTES {
        return Err(Error::NoSpace);
    }
    let mut b = [0; PROFILE_SELECT_BYTES];
    for (i, n) in [
        v.local_generation,
        v.peer_generation,
        v.feature_bits,
        v.hop_suite_bits,
        v.e2e_suite_bits,
    ]
    .into_iter()
    .enumerate()
    {
        b[i * 4..i * 4 + 4].copy_from_slice(&n.to_be_bytes());
    }
    b[20..22].copy_from_slice(&v.max_rx_window.to_be_bytes());
    b[22..24].copy_from_slice(&v.max_concurrent_transfers.to_be_bytes());
    b[24..26].copy_from_slice(&v.realtime_mode_bits.to_be_bytes());
    b[26] = v.max_message_class as u8;
    b[27..43].copy_from_slice(&v.local_digest);
    b[43..59].copy_from_slice(&v.peer_digest);
    output.copy_from_slice(&b);
    Ok(())
}
/// Decode exact59 bytes; 解码精确59字节。
/// # Errors
/// Length or canonical field violations reject.
pub fn decode_profile_select(b: &[u8]) -> Result<ProfileSelect> {
    if b.len() != PROFILE_SELECT_BYTES {
        return Err(Error::Malformed);
    }
    let read = |n| u32::from_be_bytes([b[n], b[n + 1], b[n + 2], b[n + 3]]);
    let mut local_digest = [0; 16];
    let mut peer_digest = [0; 16];
    local_digest.copy_from_slice(&b[27..43]);
    peer_digest.copy_from_slice(&b[43..59]);
    let v = ProfileSelect {
        local_generation: read(0),
        peer_generation: read(4),
        feature_bits: read(8),
        hop_suite_bits: read(12),
        e2e_suite_bits: read(16),
        max_rx_window: u16::from_be_bytes([b[20], b[21]]),
        max_concurrent_transfers: u16::from_be_bytes([b[22], b[23]]),
        realtime_mode_bits: u16::from_be_bytes([b[24], b[25]]),
        max_message_class: MessageClass::try_from(b[26])?,
        local_digest,
        peer_digest,
    };
    v.validate().map_err(|_| Error::Malformed)?;
    Ok(v)
}
/// Encode exact source SELECT plus nonzero transcript; 编码精确源SELECT及非零事务摘要。
/// # Errors
/// Invalid selection/transcript or capacity has no output writes.
pub fn encode_profile_ack(v: ProfileAck, output: &mut [u8]) -> Result<()> {
    v.selected.validate()?;
    if v.transcript_digest == [0; 16] {
        return Err(Error::Argument);
    }
    if output.len() != PROFILE_ACK_BYTES {
        return Err(Error::NoSpace);
    }
    let mut b = [0; PROFILE_ACK_BYTES];
    encode_profile_select(v.selected, &mut b[..59])?;
    b[59..].copy_from_slice(&v.transcript_digest);
    output.copy_from_slice(&b);
    Ok(())
}
/// Decode exact75 bytes; 解码精确75字节。
/// # Errors
/// Invalid length/selection/zero transcript rejects.
pub fn decode_profile_ack(b: &[u8]) -> Result<ProfileAck> {
    if b.len() != PROFILE_ACK_BYTES {
        return Err(Error::Malformed);
    }
    let selected = decode_profile_select(&b[..59])?;
    let mut digest = [0; 16];
    digest.copy_from_slice(&b[59..]);
    if digest == [0; 16] {
        return Err(Error::Malformed);
    }
    Ok(ProfileAck {
        selected,
        transcript_digest: digest,
    })
}
/// Canonical SELECT context. DTO/codec does not claim authentication.
/// 规范SELECT上下文；DTO/Codec自身不宣称认证。
#[derive(Clone, Copy)]
pub struct SelectContext {
    /// Realm; 域。
    pub realm: u32,
    /// Original SELECT source; 原SELECT源。
    pub source: Binding,
    /// Original SELECT destination; 原SELECT目的。
    pub destination: Binding,
    /// Current Session generation; 当前会话代际。
    pub session: u32,
    /// SELECT sender Origin Key generation; `SELECT`发送者Key代际。
    pub key: u32,
    /// Current Policy; 当前策略。
    pub policy: u32,
    /// Exact protection floor; 精确保护等级。
    pub origin: OriginSecurity,
    /// Exact origin fingerprint; 精确Origin域。
    pub fingerprint: [u8; 16],
    /// Actual SELECT C0 counter; 实际SELECT C0计数。
    pub transaction: u64,
    /// Fixed endpoint; 固定端点。
    pub service: u16,
}
/// SHA256 domain-separated transcript over full context and source-oriented selection.
/// 对完整上下文及源视角选择计算域分离摘要；不能仅凭摘要替代精确比较。
/// # Errors
/// Zero/reserved identity/security context or invalid selection rejects.
pub fn select_transcript(
    context: SelectContext,
    selected: ProfileSelect,
    work: &mut DigestWorkspace,
) -> Result<[u8; 16]> {
    if context.realm == 0
        || context.realm == u32::MAX
        || context.source == context.destination
        || context.source.principal == context.destination.principal
        || context.source.principal == [0; 16]
        || context.destination.principal == [0; 16]
        || context.source.generation.is_unbound()
        || context.destination.generation.is_unbound()
        || context.session == 0
        || context.key == 0
        || context.policy == 0
        || context.origin == OriginSecurity::O0
        || context.fingerprint == [0; 16]
        || context.transaction == 0
        || context.service == 0
        || context.service == u16::MAX
    {
        return Err(Error::Argument);
    }
    let mut b = [0; 152];
    let mut p = 0;
    b[..4].copy_from_slice(&context.realm.to_be_bytes());
    p += 4;
    for party in [context.source, context.destination] {
        b[p..p + 4].copy_from_slice(&party.address.get().to_be_bytes());
        p += 4;
        b[p..p + 4].copy_from_slice(&party.generation.get().to_be_bytes());
        p += 4;
        b[p..p + 16].copy_from_slice(&party.principal);
        p += 16;
    }
    for n in [context.session, context.key, context.policy] {
        b[p..p + 4].copy_from_slice(&n.to_be_bytes());
        p += 4;
    }
    b[p] = context.origin as u8;
    p += 1;
    b[p..p + 16].copy_from_slice(&context.fingerprint);
    p += 16;
    b[p..p + 8].copy_from_slice(&context.transaction.to_be_bytes());
    p += 8;
    b[p..p + 2].copy_from_slice(&context.service.to_be_bytes());
    p += 2;
    b[p..p + 2].copy_from_slice(&0x0104u16.to_be_bytes());
    p += 2;
    encode_profile_select(selected, &mut b[p..p + 59])?;
    p += 59;
    canonical_digest(b"UCN6-CAPABILITY-SELECT-V1", &b[..p], work)
}
