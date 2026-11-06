//! Backup snapshots use fixed inactive buffers and complete protected-voter coverage.
//! `BackupSnapshot使用固定非活动Buffer及完整受保护Voter覆盖`。
use crate::{ClusterConfig, ClusterEpoch, ClusterMember};
use ucn_digest::{DigestWorkspace, canonical_begin};
use ucn_types::{Error, Result};

/// Explicit coverage state; 显式覆盖状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CoverageState {
    /// Fresh admitted member; 已准入。
    Admitted,
    /// Explicit temporary suspicion; 显式暂时怀疑。
    Suspect,
    /// Permanent removal for this assignment; 本次assignment永久移除。
    Removed,
}
/// Ordered snapshot row; 有序快照行。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SnapshotRow {
    /// Exact member; 精确成员。
    pub member: ClusterMember,
    /// Admission state; 准入状态。
    pub state: CoverageState,
    /// Canonical authenticated business-state digest; 规范业务状态摘要。
    pub state_digest: [u8; 16],
}
/// Immutable snapshot proof, issued only after complete ordered coverage.
/// 仅完整有序覆盖后签发的不变快照证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SnapshotProof {
    pub(crate) transaction: u64,
    pub(crate) epoch: ClusterEpoch,
    pub(crate) config: u32,
    pub(crate) config_generation: u32,
    pub(crate) joint: Option<(u32, u32)>,
    pub(crate) backup: ClusterMember,
    pub(crate) generation: u32,
    pub(crate) digest: [u8; 16],
    pub(crate) checked_at: u64,
}
/// Two bounded snapshot buffers and a permanent assignment-local eligibility fence.
/// 双固定快照缓冲及assignment本地永久资格Fence。
pub struct SnapshotOwner<const MEMBERS: usize, const VOTERS: usize> {
    transaction: u64,
    epoch: ClusterEpoch,
    config: ClusterConfig<VOTERS>,
    joint: Option<ClusterConfig<VOTERS>>,
    backup: ClusterMember,
    buffers: [[Option<SnapshotRow>; MEMBERS]; 2],
    active: usize,
    staging_count: usize,
    expected: usize,
    building: bool,
    generation: u32,
    pending_generation: u32,
    digest: [u8; 16],
    ineligible: bool,
    suspect_until: Option<u64>,
}
impl<const M: usize, const V: usize> SnapshotOwner<M, V> {
    /// Establish a new assignment; 建立新的assignment。
    /// # Errors
    /// Invalid config, capacity or undesignated Backup rejects.
    pub fn new(
        epoch: ClusterEpoch,
        config: ClusterConfig<V>,
        joint: Option<ClusterConfig<V>>,
        backup: ClusterMember,
        transaction: u64,
    ) -> Result<Self> {
        epoch.validate()?;
        config.validate()?;
        if let Some(value) = joint {
            value.validate()?;
        }
        if transaction == 0
            || M < V
            || M > 32
            || !(config.find(backup.principal) == Some(backup)
                || joint.is_some_and(|target| target.find(backup.principal) == Some(backup)))
            || !backup.backup
        {
            return Err(Error::Config);
        }
        Ok(Self {
            transaction,
            epoch,
            config,
            joint,
            backup,
            buffers: [[None; M]; 2],
            active: 0,
            staging_count: 0,
            expected: 0,
            building: false,
            generation: 0,
            pending_generation: 0,
            digest: [0; 16],
            ineligible: false,
            suspect_until: None,
        })
    }
    /// BEGIN=0 reserves inactive staging only; `BEGIN0仅预留非活动staging`。
    /// # Errors
    /// Generation reuse, invalid count or active assembly rejects.
    pub fn begin(&mut self, generation: u32, count: usize) -> Result<()> {
        if self.building
            || generation == 0
            || generation <= self.generation
            || generation > 0x7fff_ffff
            || count == 0
            || count > M
        {
            return Err(Error::State);
        }
        self.buffers[1 - self.active].fill(None);
        self.staging_count = 0;
        self.expected = count;
        self.pending_generation = generation;
        self.building = true;
        Ok(())
    }
    /// MEMBER=1..N, exact order and unique Principal; MEMBER1..N精确顺序唯一身份。
    /// # Errors
    /// Disorder, invalid member or duplicate rejects without altering active buffer.
    pub fn member(&mut self, sequence: usize, row: SnapshotRow) -> Result<()> {
        row.member.validate()?;
        if !self.building
            || sequence != self.staging_count + 1
            || sequence > self.expected
            || row.state_digest == [0; 16]
            || self.buffers[1 - self.active][..self.staging_count]
                .iter()
                .flatten()
                .last()
                .is_some_and(|previous| previous.member.principal >= row.member.principal)
        {
            return Err(Error::State);
        }
        self.buffers[1 - self.active][self.staging_count] = Some(row);
        self.staging_count += 1;
        Ok(())
    }
    /// END=N+1 atomically swaps complete staging after canonical digest verification.
    /// ENDN+1在规范摘要验证后原子交换完整staging。
    /// # Errors
    /// Incomplete snapshot/digest mismatch rejects with active buffer intact.
    pub fn end(
        &mut self,
        sequence: usize,
        expected: [u8; 16],
        scratch: &mut DigestWorkspace,
    ) -> Result<()> {
        if !self.building || self.staging_count != self.expected || sequence != self.expected + 1 {
            return Err(Error::State);
        }
        let digest = self.staging_digest(scratch)?;
        if digest != expected {
            return Err(Error::Replay);
        }
        self.active = 1 - self.active;
        self.generation = self.pending_generation;
        self.digest = digest;
        self.building = false;
        if !self.coverage_complete() {
            self.ineligible = true;
            self.suspect_until = None;
        }
        Ok(())
    }
    /// Compute canonical inactive-buffer digest; 计算非活动缓冲规范摘要。
    /// # Errors
    /// Incomplete staging rejects.
    pub fn staging_digest(&self, scratch: &mut DigestWorkspace) -> Result<[u8; 16]> {
        if !self.building || self.expected != self.staging_count {
            return Err(Error::State);
        }
        let mut bytes = [0u8; 40];
        bytes[..4].copy_from_slice(&self.epoch.cluster.to_be_bytes());
        bytes[4..8].copy_from_slice(&self.epoch.term.to_be_bytes());
        bytes[8..24].copy_from_slice(&self.epoch.head);
        bytes[24..28].copy_from_slice(&self.epoch.binding.to_be_bytes());
        bytes[28..32].copy_from_slice(&self.config.id.to_be_bytes());
        bytes[32..36].copy_from_slice(&self.config.generation.to_be_bytes());
        bytes[36..40].copy_from_slice(&self.pending_generation.to_be_bytes());
        let length = u32::try_from(72 + self.expected * 37).map_err(|_| Error::Argument)?;
        let mut hash = canonical_begin(b"UCN6-CLUSTER-SNAPSHOT-V1", length, scratch)?;
        hash.append(&bytes)?;
        hash.append(&self.transaction.to_be_bytes())?;
        hash.append(&self.backup.principal)?;
        hash.append(&self.joint.map_or(0, |config| config.id).to_be_bytes())?;
        hash.append(
            &self
                .joint
                .map_or(0, |config| config.generation)
                .to_be_bytes(),
        )?;
        for row in self.buffers[1 - self.active][..self.expected]
            .iter()
            .flatten()
        {
            let mut bytes = [0; 37];
            bytes[..16].copy_from_slice(&row.member.principal);
            bytes[16..20].copy_from_slice(&row.member.binding.to_be_bytes());
            bytes[20] = match row.state {
                CoverageState::Admitted => 1,
                CoverageState::Suspect => 2,
                CoverageState::Removed => 3,
            };
            bytes[21..37].copy_from_slice(&row.state_digest);
            hash.append(&bytes)?;
        }
        hash.finish()
    }
    fn coverage_complete(&self) -> bool {
        [Some(self.config), self.joint]
            .into_iter()
            .flatten()
            .all(|config| {
                config.members[..config.count]
                    .iter()
                    .filter(|member| member.voter)
                    .all(|member| {
                        self.buffers[self.active]
                            .iter()
                            .flatten()
                            .any(|row| row.member == *member && row.state != CoverageState::Removed)
                    })
            })
    }
    /// Validate complete coverage before grace; only explicit SUSPECT starts grace.
    /// Grace前验证完整覆盖，只有显式SUSPECT启动Grace。
    /// # Errors
    /// Missing/removed voter or expired grace permanently invalidates this assignment.
    pub fn ready(&mut self, now: u64, grace: u64) -> Result<SnapshotProof> {
        if self.ineligible || self.generation == 0 || now == 0 || grace == 0 {
            return Err(Error::Access);
        }
        if !self.coverage_complete() {
            self.ineligible = true;
            self.suspect_until = None;
            return Err(Error::Access);
        }
        let suspect = [Some(self.config), self.joint]
            .into_iter()
            .flatten()
            .any(|config| {
                config.members[..config.count]
                    .iter()
                    .filter(|member| member.voter)
                    .any(|member| {
                        self.buffers[self.active]
                            .iter()
                            .flatten()
                            .any(|row| row.member == *member && row.state == CoverageState::Suspect)
                    })
            });
        if suspect {
            let until = if let Some(until) = self.suspect_until {
                until
            } else {
                let until = now.checked_add(grace).ok_or(Error::Argument)?;
                self.suspect_until = Some(until);
                until
            };
            if now >= until {
                self.ineligible = true;
                return Err(Error::Access);
            }
        } else {
            self.suspect_until = None;
        }
        Ok(SnapshotProof {
            transaction: self.transaction,
            epoch: self.epoch,
            config: self.config.id,
            config_generation: self.config.generation,
            joint: self.joint.map(|config| (config.id, config.generation)),
            backup: self.backup,
            generation: self.generation,
            digest: self.digest,
            checked_at: now,
        })
    }
    /// Permanent eligibility loss of the current assignment; 当前assignment永久失去资格。
    #[must_use]
    pub const fn ineligible(&self) -> bool {
        self.ineligible
    }
}
