//! Bounded cross-Cluster Directory and lineage. `有界跨簇Directory与Lineage`。
use crate::{ClusterAuthority, ClusterEpoch};
use ucn_types::{Error, Result};
/// Bounded Directory fact, authenticated by Coordinator before ingestion.
/// `Coordinator认证后接纳的有界Directory事实`。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DirectoryEntry {
    /// Realm namespace; Realm命名空间。
    pub realm: u32,
    /// Complete current cluster Epoch; 完整当前簇Epoch。
    pub epoch: ClusterEpoch,
    /// Local Directory version; 本地Directory版本。
    pub generation: u32,
    /// Config generation; Config代际。
    pub config_generation: u32,
    /// Lease in receiver local clock; 接收端本地时钟租约。
    pub until: u64,
    /// Exact authenticated state digest; 精确认证状态摘要。
    pub digest: [u8; 16],
}
/// Independent cross-cluster Directory with no global Term comparison.
/// 独立跨簇Directory，不跨簇比较Term。
pub struct DirectoryOwner<const ENTRIES: usize> {
    entries: [Option<DirectoryEntry>; ENTRIES],
}
impl<const N: usize> Default for DirectoryOwner<N> {
    fn default() -> Self {
        Self::new()
    }
}
impl<const N: usize> DirectoryOwner<N> {
    /// Empty fixed table; 空固定表。
    #[must_use]
    pub const fn new() -> Self {
        Self { entries: [None; N] }
    }
    /// Publish a local advertisement with a fresh sealed Authority preflight.
    /// 用新鲜封闭Authority预检发布本地公告。
    /// # Errors
    /// Mismatched Epoch/time/config or expired authority rejects.
    pub fn publish(
        &mut self,
        entry: DirectoryEntry,
        authority: ClusterAuthority,
        now: u64,
    ) -> Result<()> {
        if entry.epoch != authority.epoch
            || entry.config_generation != authority.config_generation
            || now != authority.checked_at
            || now >= authority.deadline
            || entry.until > authority.deadline
        {
            return Err(Error::Access);
        }
        self.ingest_authenticated(entry, now)
    }
    /// Low-level authenticated Coordinator SPI; 低层认证Coordinator SPI。
    /// # Errors
    /// Expired/old/conflicting entries and full capacity reject; no implicit eviction.
    pub fn ingest_authenticated(&mut self, entry: DirectoryEntry, now: u64) -> Result<()> {
        entry.epoch.validate()?;
        if entry.realm == 0
            || entry.generation == 0
            || entry.generation > 0x7fff_ffff
            || entry.config_generation == 0
            || entry.digest == [0; 16]
            || now == 0
            || now >= entry.until
        {
            return Err(Error::Argument);
        }
        let old = self.entries.iter().position(|old| {
            old.is_some_and(|v| v.realm == entry.realm && v.epoch.cluster == entry.epoch.cluster)
        });
        if let Some(index) = old {
            let previous = self.entries[index].ok_or(Error::State)?;
            if previous.generation == entry.generation {
                return if previous == entry {
                    Ok(())
                } else {
                    Err(Error::Replay)
                };
            }
            if previous.generation > entry.generation || entry.epoch.term < previous.epoch.term {
                return Err(Error::Replay);
            }
        }
        let index = old
            .or_else(|| self.entries.iter().position(Option::is_none))
            .ok_or(Error::NoSpace)?;
        self.entries[index] = Some(entry);
        Ok(())
    }
    /// Resolve only live exact Realm/Cluster; 仅解析live精确Realm/Cluster。
    /// # Errors
    /// Missing or expired fact rejects.
    pub fn get(&self, realm: u32, cluster: u32, now: u64) -> Result<DirectoryEntry> {
        self.entries
            .iter()
            .flatten()
            .find(|entry| {
                entry.realm == realm
                    && entry.epoch.cluster == cluster
                    && now != 0
                    && now < entry.until
            })
            .copied()
            .ok_or(Error::Timeout)
    }
    /// Explicit bounded expiry collection; 显式有界过期回收。
    pub fn step(&mut self, now: u64, budget: usize) -> usize {
        let mut used = 0;
        for entry in &mut self.entries {
            if used == budget {
                break;
            }
            if entry.is_some_and(|value| now >= value.until) {
                *entry = None;
                used += 1;
            }
        }
        used
    }
}
/// Immutable detached lineage; 不变detach谱系。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Lineage {
    /// Original identity domain; 原始身份域。
    pub origin: ClusterEpoch,
    /// Explicit detached epoch; 显式detach纪元。
    pub detached: ClusterEpoch,
    /// Persisted detach transaction; 持久化detach事务。
    pub transaction: u64,
}
impl Lineage {
    /// Recovery never silently combines unrelated lineages; 恢复不静默合并无关谱系。
    #[must_use]
    pub fn same_recovery_domain(self, other: Self) -> bool {
        self.origin == other.origin
            && self.detached.cluster == other.detached.cluster
            && self.transaction == other.transaction
    }
}
