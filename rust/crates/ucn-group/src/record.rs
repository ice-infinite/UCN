//! One bounded common Context record per Manifest slot; no runtime handles on disk.
//! 每个固定Manifest槽一个共同正文，不写入易失Handle。
use crate::{GroupDefinition, Member};
use ucn_types::{Error, Result};
/// Shared Group context schema; 共同群组正文格式。
pub const GROUP_CONTEXT_SCHEMA: u16 = 0x0802;
/// New logical-only schema version; 不含易失引用的新版本。
pub const GROUP_CONTEXT_VERSION: u16 = 2;
/// Logical header bytes; 逻辑头长度。
pub const GROUP_CONTEXT_HEADER: usize = 64;
/// Fixed member stride; 固定成员步长。
pub const GROUP_MEMBER_BYTES: usize = 28;
/// Shared logical Context body; 共同逻辑Context正文。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GroupRecord<const M: usize> {
    /// Frozen definition; 冻结定义。
    pub definition: GroupDefinition<M>,
    /// Durable retirement state; 耐久退休状态。
    pub retired: bool,
    /// Independent policy generation; 独立策略代际。
    pub policy_generation: u32,
    /// Independent membership generation; 独立成员代际。
    pub member_generation: u32,
    /// Application opcode; 应用操作码。
    pub opcode: u16,
    /// Issued allocator high-water, not the allocator itself; 已签发高水位，不替代分配器。
    pub allocated_high_water: u32,
    /// Logical installed security context digest; 逻辑安全上下文摘要。
    pub security_digest: [u8; 16],
}
impl<const M: usize> GroupRecord<M> {
    /// Validate exact logical body; 验证完整逻辑正文。
    /// # Errors
    /// Invalid policy, unused members, scope, generation or digest rejects.
    pub fn validate(&self) -> Result<()> {
        self.definition.validate()?;
        if self.definition.public_static
            || !self.definition.exact_principal
            || self.definition.quorum_weight > u16::from(u8::MAX)
            || self.definition.scope_mask & 1 == 0
            || self.policy_generation == 0
            || self.member_generation == 0
            || self.opcode == 0
            || self.opcode == u16::MAX
            || self.allocated_high_water < self.definition.group
            || self.security_digest == [0; 16]
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}
/// Canonical big-endian Context encoder; 规范大端正文编码。
/// # Errors
/// Invalid fields/capacity rejects with unchanged output.
pub fn encode_group_record<const M: usize>(r: GroupRecord<M>, out: &mut [u8]) -> Result<usize> {
    r.validate()?;
    let n = GROUP_CONTEXT_HEADER + M * GROUP_MEMBER_BYTES;
    if out.len() < n {
        return Err(Error::NoSpace);
    }
    let d = r.definition;
    out[..n].fill(0);
    out[..2].copy_from_slice(&GROUP_CONTEXT_VERSION.to_be_bytes());
    out[2] = if r.retired { 4 } else { 2 };
    out[3] = 2;
    for (i, v) in [
        (4, d.realm),
        (8, d.group),
        (12, d.generation),
        (16, r.policy_generation),
        (20, r.member_generation),
        (36, d.key_generation),
        (40, d.tree_generation),
        (44, r.allocated_high_water),
    ] {
        out[i..i + 4].copy_from_slice(&v.to_be_bytes());
    }
    out[24..26].copy_from_slice(&d.service.to_be_bytes());
    out[26..28].copy_from_slice(&r.opcode.to_be_bytes());
    out[28] = u8::try_from(d.count).map_err(|_| Error::Argument)?;
    out[29] = u8::try_from(d.fanout_limit).map_err(|_| Error::Argument)?;
    out[30] = u8::try_from(d.quorum_weight).map_err(|_| Error::Argument)?;
    out[31] = d.scope_mask;
    out[32] = 1;
    out[34] = 1;
    out[35] = u8::from(d.tree_generation != 0);
    out[48..64].copy_from_slice(&r.security_digest);
    for (i, m) in d.members.iter().enumerate() {
        let b = &mut out[64 + i * 28..92 + i * 28];
        b[..4].copy_from_slice(&m.address.to_be_bytes());
        b[4..8].copy_from_slice(&m.binding_generation.to_be_bytes());
        b[8..24].copy_from_slice(&m.principal);
        b[24..26].copy_from_slice(&m.sender_slot.to_be_bytes());
        b[26] = m.weight;
    }
    Ok(n)
}
/// Exact logical decoder; no permissions restored from disk. 精确逻辑解码，不恢复权限。
/// # Errors
/// Wrong length/schema/reserved bytes, inactive padding or invalid identity rejects.
pub fn decode_group_record<const M: usize>(b: &[u8]) -> Result<GroupRecord<M>> {
    if M == 0
        || M > 32
        || b.len() != 64 + M * 28
        || b[..2] != GROUP_CONTEXT_VERSION.to_be_bytes()
        || !matches!(b[2], 2 | 4)
        || b[3] != 2
        || b[32] != 1
        || b[33] != 0
        || b[34] != 1
        || b[35] > 1
    {
        return Err(Error::Malformed);
    }
    let r32 = |i| u32::from_be_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]]);
    if (b[35] == 1) != (r32(40) != 0) {
        return Err(Error::Malformed);
    }
    let mut members = [Member::EMPTY; M];
    for (i, m) in members.iter_mut().enumerate() {
        let v = &b[64 + i * 28..92 + i * 28];
        if v[27] != 0 {
            return Err(Error::Malformed);
        }
        m.address = u32::from_be_bytes([v[0], v[1], v[2], v[3]]);
        m.binding_generation = u32::from_be_bytes([v[4], v[5], v[6], v[7]]);
        m.principal.copy_from_slice(&v[8..24]);
        m.sender_slot = u16::from_be_bytes([v[24], v[25]]);
        m.weight = v[26];
    }
    let r = GroupRecord {
        definition: GroupDefinition {
            realm: r32(4),
            group: r32(8),
            generation: r32(12),
            key_generation: r32(36),
            tree_generation: r32(40),
            service: u16::from_be_bytes([b[24], b[25]]),
            scope_mask: b[31],
            fanout_limit: usize::from(b[29]),
            count: usize::from(b[28]),
            members,
            exact_principal: true,
            public_static: false,
            quorum_weight: u16::from(b[30]),
        },
        retired: b[2] == 4,
        policy_generation: r32(16),
        member_generation: r32(20),
        opcode: u16::from_be_bytes([b[26], b[27]]),
        allocated_high_water: r32(44),
        security_digest: b[48..64].try_into().map_err(|_| Error::Malformed)?,
    };
    r.validate().map_err(|_| Error::Malformed)?;
    Ok(r)
}
