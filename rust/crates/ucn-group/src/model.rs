use ucn_types::{Error, Result};
/// Canonical group member; 规范Group成员。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Member {
    /// Exact device; 精确设备身份。
    pub principal: [u8; 16],
    /// Current address; 当前地址。
    pub address: u32,
    /// Address binding generation; 地址绑定代际。
    pub binding_generation: u32,
    /// Group Sender slot; Group发送者槽。
    pub sender_slot: u16,
    /// Quorum weight; Quorum权重。
    pub weight: u8,
}
impl Member {
    /// Inactive fixed slot; 固定未使用槽。
    pub const EMPTY: Self = Self {
        principal: [0; 16],
        address: 0,
        binding_generation: 0,
        sender_slot: 0,
        weight: 0,
    };
    pub(crate) fn validate(self) -> Result<()> {
        if self.principal == [0; 16]
            || self.address == 0
            || self.address == u32::MAX
            || self.binding_generation == 0
            || self.sender_slot == 0
            || self.weight == 0
        {
            Err(Error::Argument)
        } else {
            Ok(())
        }
    }
}
/// Frozen group completion scope; 冻结群组完成范围。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SuccessScope {
    /// Local bundle accepted, no remote evidence; 本地bundle接纳。
    LocalOnly = 0,
    /// One authenticated frozen member; 一个认证冻结成员。
    Any = 1,
    /// Every frozen member; 全部冻结成员。
    All = 2,
    /// Frozen weighted quorum; 冻结权重Quorum。
    Quorum = 3,
    /// Exact required subset; 精确要求子集。
    Subset = 4,
}
/// Immutable Group context; 不变Group上下文。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GroupDefinition<const MEMBERS: usize> {
    /// Owner Realm; `OwnerRealm`。
    pub realm: u32,
    /// Non-reusable Group ID; `不复用GroupID`。
    pub group: u32,
    /// Policy generation; 策略代际。
    pub generation: u32,
    /// Key generation; 密钥代际。
    pub key_generation: u32,
    /// Tree generation; Tree代际。
    pub tree_generation: u32,
    /// Exact Endpoint; 精确Endpoint。
    pub service: u16,
    /// Authoritative allowed scopes bitset; 允许Scope位图。
    pub scope_mask: u8,
    /// Maximum bounded unicast expansion; 有界单播展开上限。
    pub fanout_limit: usize,
    /// Frozen member count; 冻结成员数。
    pub count: usize,
    /// Fixed canonical members; 固定规范成员。
    pub members: [Member; MEMBERS],
    /// Exact Principal security, not a shared-key assertion.
    /// 精确身份安全，不能只凭共享Key。
    pub exact_principal: bool,
    /// Product allows public non-authoritative static use.
    /// 产品允许公开无Authority静态业务。
    pub public_static: bool,
    /// Frozen quorum weight; 冻结Quorum权重。
    pub quorum_weight: u16,
}
impl<const M: usize> GroupDefinition<M> {
    /// Validate canonical members, fixed limits and exact source security.
    /// 验证规范成员、固定限制及精确来源安全。
    /// # Errors
    /// Invalid generation/scope/member order/capacity rejects.
    pub fn validate(&self) -> Result<()> {
        if M == 0
            || M > 32
            || self.realm == 0
            || self.group == 0
            || self.generation == 0
            || self.service == 0
            || self.service == u16::MAX
            || self.count == 0
            || self.count > M
            || self.fanout_limit == 0
            || self.fanout_limit > M
            || self.scope_mask & !0x1f != 0
            || self.scope_mask == 0
            || (!self.public_static && !self.exact_principal)
            || self.exact_principal && self.key_generation == 0
        {
            return Err(Error::Argument);
        }
        let mut previous = None;
        let mut total = 0u16;
        for member in &self.members[..self.count] {
            member.validate()?;
            if previous.is_some_and(|prior| prior >= member.principal) {
                return Err(Error::Argument);
            }
            if self.members[..self.count]
                .iter()
                .filter(|other| {
                    other.address == member.address || other.sender_slot == member.sender_slot
                })
                .count()
                != 1
            {
                return Err(Error::Argument);
            }
            previous = Some(member.principal);
            total = total
                .checked_add(u16::from(member.weight))
                .ok_or(Error::Argument)?;
        }
        if self.quorum_weight == 0
            || self.quorum_weight > total
            || self.members[self.count..]
                .iter()
                .any(|member| *member != Member::EMPTY)
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}
