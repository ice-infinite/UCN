use ucn_types::{Error, Result};
/// Canonical committed member; 规范已提交成员。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ClusterMember {
    /// Principal; 身份。
    pub principal: [u8; 16],
    /// Binding generation; 绑定代际。
    pub binding: u32,
    /// Committed voter; 已提交Voter。
    pub voter: bool,
    /// Designated Backup; 指定Backup。
    pub backup: bool,
}
impl ClusterMember {
    /// Unused fixed slot; 未使用固定槽。
    pub const EMPTY: Self = Self {
        principal: [0; 16],
        binding: 0,
        voter: false,
        backup: false,
    };
    pub(crate) fn validate(self) -> Result<()> {
        if self.principal == [0; 16] || self.binding == 0 || self.backup && !self.voter {
            return Err(Error::Argument);
        }
        Ok(())
    }
}
/// Complete Epoch identity; 完整Epoch身份。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ClusterEpoch {
    /// Cluster ID; 簇ID。
    pub cluster: u32,
    /// Term, comparable only within same Cluster; Term仅同簇可比。
    pub term: u32,
    /// Head Principal; Head身份。
    pub head: [u8; 16],
    /// Head binding generation; Head绑定代际。
    pub binding: u32,
}
impl ClusterEpoch {
    pub(crate) fn validate(self) -> Result<()> {
        if self.cluster == 0
            || self.term == 0
            || self.term > 0x7fff_ffff
            || self.head == [0; 16]
            || self.binding == 0
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}
/// Canonical Stable/Joint voter-set component; 规范Stable/Joint投票集合分量。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ClusterConfig<const VOTERS: usize> {
    /// Config identity; Config身份。
    pub id: u32,
    /// Config generation; Config代际。
    pub generation: u32,
    /// Frozen member count; 冻结成员数。
    pub count: usize,
    /// Fixed canonical ordered members; 固定规范有序成员。
    pub members: [ClusterMember; VOTERS],
}
impl<const V: usize> ClusterConfig<V> {
    /// Validate canonical configured members and unique Backup; 验证规范成员及唯一Backup。
    /// # Errors
    /// Invalid IDs, ordering, counts, missing voters or duplicate Backup rejects.
    pub fn validate(self) -> Result<()> {
        if V == 0
            || V > 32
            || self.id == 0
            || self.id > 0x7fff_ffff
            || self.generation == 0
            || self.generation > 0x7fff_ffff
            || self.count == 0
            || self.count > V
        {
            return Err(Error::Argument);
        }
        let mut previous = None;
        let mut voters = 0;
        let mut backups = 0;
        for member in &self.members[..self.count] {
            member.validate()?;
            if previous.is_some_and(|principal| principal >= member.principal) {
                return Err(Error::Argument);
            }
            previous = Some(member.principal);
            voters += usize::from(member.voter);
            backups += usize::from(member.backup);
        }
        if voters == 0
            || backups > 1
            || self.members[self.count..]
                .iter()
                .any(|member| *member != ClusterMember::EMPTY)
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
    pub(crate) fn find(self, principal: [u8; 16]) -> Option<ClusterMember> {
        self.members[..self.count]
            .iter()
            .find(|member| member.principal == principal)
            .copied()
    }
    pub(crate) fn quorum(self) -> usize {
        self.members[..self.count]
            .iter()
            .filter(|member| member.voter)
            .count()
            / 2
            + 1
    }
}
/// Current authenticated member evidence, freshly supplied by Coordinator.
/// Coordinator新鲜提供的当前认证成员证据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MemberFact {
    /// Configured member identity; 配置成员身份。
    pub member: ClusterMember,
    /// Session generation; 会话代际。
    pub session: u32,
    /// Capability generation; 能力代际。
    pub capability: u32,
    /// Exact capability digest; 精确能力摘要。
    pub capability_digest: [u8; 16],
    /// Local member lease; 本地成员租约。
    pub lease_until: u64,
    /// Local capability lease; 本地能力租约。
    pub capability_until: u64,
    /// Current activated C4 Flow generation; 当前激活C4 Flow代际。
    pub flow_generation: u32,
    /// Current physical Link generation; 当前物理Link代际。
    pub link_generation: u32,
}
impl MemberFact {
    pub(crate) fn live(self, now: u64) -> bool {
        now != 0
            && now < self.lease_until
            && now < self.capability_until
            && self.session != 0
            && self.capability != 0
            && self.capability_digest != [0; 16]
            && self.flow_generation != 0
            && self.link_generation != 0
    }
    pub(crate) fn same_parent(self, other: Self) -> bool {
        self.member == other.member
            && self.session == other.session
            && self.capability == other.capability
            && self.capability_digest == other.capability_digest
            && self.flow_generation == other.flow_generation
            && self.link_generation == other.link_generation
    }
}
/// Durable transaction phase; 耐久事务阶段。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ClusterPhase {
    /// Committed Stable; 已提交Stable。
    Stable = 1,
    /// Durable PREPARED only; 仅耐久PREPARED。
    Prepared = 2,
    /// Durable Joint; 耐久Joint。
    Joint = 3,
    /// Collecting precise votes; 收集精确票。
    Collecting = 4,
    /// Live exact quorum; live精确Quorum。
    Quorum = 5,
    /// One-way terminal durable Epoch; 单向终态耐久Epoch。
    EpochDurable = 6,
    /// Aborted before Joint/Commit; Joint/Commit前取消。
    Aborted = 7,
}
/// Transition semantic kind; 转换语义种类。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TransitionKind {
    /// Config; 配置。
    Config = 1,
    /// Designated Backup takeover; 指定Backup接管。
    Takeover = 2,
    /// Explicit recovery; 显式恢复。
    Recovery = 3,
    /// Same-Cluster planned handover; 同簇计划移交。
    Handover = 4,
    /// Cross-Cluster merge; 跨簇合并。
    Merge = 5,
}
/// Immutable current Authority proof issued only by Cluster Owner preflight.
/// `仅ClusterOwner` preflight签发的当前Authority证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ClusterAuthority {
    pub(crate) owner: u32,
    pub(crate) epoch: ClusterEpoch,
    pub(crate) config_id: u32,
    pub(crate) config_generation: u32,
    pub(crate) checked_at: u64,
    pub(crate) deadline: u64,
}
impl ClusterAuthority {
    /// Exact authority Epoch; `精确AuthorityEpoch`。
    #[must_use]
    pub const fn epoch(self) -> ClusterEpoch {
        self.epoch
    }
    /// Current preflight time; 当前预检时刻。
    #[must_use]
    pub const fn checked_at(self) -> u64 {
        self.checked_at
    }
}
/// Complete durable vote evidence; 完整耐久票证据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct VoteEvidence {
    /// Exact voter; 精确投票者。
    pub voter: ClusterMember,
    /// Session at vote; 投票时会话。
    pub session: u32,
    /// Capability at vote; 投票时能力。
    pub capability: u32,
    /// Vote ID; 投票ID。
    pub vote_id: u32,
    /// Complete canonical vote proof digest; 完整规范票证明摘要。
    pub digest: [u8; 16],
}
/// Frozen owner status; 冻结Owner状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ClusterView {
    /// Epoch; Epoch。
    pub epoch: ClusterEpoch,
    /// Config ID; `ConfigID`。
    pub config_id: u32,
    /// Phase; 阶段。
    pub phase: ClusterPhase,
    /// Permanent authority fence; `永久AuthorityFence`。
    pub fenced: bool,
    /// Live quorum at last preflight; `上次预检LiveQuorum`。
    pub quorum: bool,
}

/// Sealed readiness of the exact proposed successor; 封闭的精确后继就绪证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HandoverReady {
    pub(crate) epoch: ClusterEpoch,
    pub(crate) target: ClusterMember,
    pub(crate) config: u32,
    pub(crate) generation: u32,
    pub(crate) session: u32,
    pub(crate) capability: u32,
    pub(crate) capability_digest: [u8; 16],
    pub(crate) flow_generation: u32,
    pub(crate) link_generation: u32,
    pub(crate) snapshot: [u8; 16],
    pub(crate) checked_at: u64,
    pub(crate) until: u64,
}
/// Irreversible old-Head commit, issued only after reload of its Fence.
/// `仅旧Head的Fence回读后签发的不可逆Commit`。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HandoverCommit {
    pub(crate) old: ClusterEpoch,
    pub(crate) next: ClusterEpoch,
    pub(crate) config: u32,
    pub(crate) config_generation: u32,
    pub(crate) transaction: u64,
}
