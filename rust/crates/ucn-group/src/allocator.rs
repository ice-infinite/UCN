//! Realm-wide durable allocator, separate from bounded Context records.
//! Realm级耐久分配器，独立于各固定Context正文。
use ucn_durable::{DurableBase, DurableContract, DurableUpdate};
use ucn_identity::RealmAuthorityView;
use ucn_persistence::{
    CodecWorkspace, DomainKind, PersistenceHandle, PersistenceProof, PersistenceRequest,
};
use ucn_types::{Error, Result};
/// Shared allocator schema; 共同分配器Schema。
pub const GROUP_ALLOCATOR_SCHEMA: u16 = 0x0804;
/// Unique Realm allocator domain, distinct from fixed context slot domains.
/// 唯一Realm分配器域，区别于固定Context槽域。
#[must_use]
pub const fn group_allocator_domain_id(realm: u32) -> u64 {
    (realm as u64) << 32 | 0xffff_fffe
}
/// Shared 16-byte allocator body encoder; 共同16字节分配器正文编码。
/// # Errors
/// Unknown Realm/authority or zero high-water rejects without output writes.
pub fn encode_allocator_record(
    realm: u32,
    high_water: u32,
    authority: u32,
    out: &mut [u8; 16],
) -> Result<()> {
    if realm == 0 || high_water == 0 || authority == 0 {
        return Err(Error::Argument);
    }
    out[..4].copy_from_slice(b"GID1");
    out[4..8].copy_from_slice(&realm.to_be_bytes());
    out[8..12].copy_from_slice(&high_water.to_be_bytes());
    out[12..].copy_from_slice(&authority.to_be_bytes());
    Ok(())
}
/// A linear issued Group ID, never cloned or reconstructed from disk.
/// 线性签发Group ID，不可克隆或从旧正文重建。
pub struct GroupIdPermit {
    pub(crate) realm: u32,
    pub(crate) id: u32,
    pub(crate) authority: u32,
    pub(crate) runtime: u32,
    pub(crate) allocator_domain: u64,
}
impl GroupIdPermit {
    /// Non-reusable issued ID; 不可复用签发ID。
    #[must_use]
    pub const fn id(&self) -> u32 {
        self.id
    }
}
/// Caller-owned 16-byte durable high-water consumer; 调用方持有的16字节高水位消费者。
pub struct GroupAllocator {
    contract: DurableContract,
    realm: u32,
    generation: u64,
    high_water: u32,
    authority_high_water: u32,
    pending: Option<(DurableUpdate<16>, u32, u32)>,
    permit: Option<GroupIdPermit>,
}
impl GroupAllocator {
    /// Bind one unique Realm allocator domain; 绑定一个唯一Realm分配器域。
    /// # Errors
    /// Wrong schema/kind/capacity rejects.
    pub fn new(contract: DurableContract, realm: u32) -> Result<Self> {
        if realm == 0
            || contract.domain.kind != DomainKind::GroupPolicyKey
            || contract.schema != GROUP_ALLOCATOR_SCHEMA
            || contract.version != 1
            || contract.operation != GROUP_ALLOCATOR_SCHEMA
            || contract.domain.id != group_allocator_domain_id(realm)
        {
            return Err(Error::Config);
        }
        Ok(Self {
            contract,
            realm,
            generation: 0,
            high_water: 0,
            authority_high_water: 0,
            pending: None,
            permit: None,
        })
    }
    /// Restore verified high-water, never an outstanding permit; 恢复已验证高水位，不恢复旧许可。
    /// # Errors
    /// Missing restore, different exact bytes, malformed or rollback rejects.
    pub fn restore(
        &mut self,
        base: DurableBase,
        b: &[u8],
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if base.contract() != self.contract
            || self.generation != 0
            || self.pending.is_some()
            || self.permit.is_some()
        {
            return Err(Error::State);
        }
        base.verify_body(b, scratch)?;
        if base.generation() == 0 {
            return if b.is_empty() {
                Ok(())
            } else {
                Err(Error::Malformed)
            };
        }
        if b.len() != 16
            || &b[..4] != b"GID1"
            || read32(b, 4) != self.realm
            || read32(b, 8) == 0
            || read32(b, 12) == 0
        {
            return Err(Error::Malformed);
        }
        self.high_water = read32(b, 8);
        self.authority_high_water = read32(b, 12);
        self.generation = base.generation();
        Ok(())
    }
    /// Persist next ID before any Context or announcement exists; 下一ID先持久化再创建上下文。
    /// # Errors
    /// Expired authority, non-restored base, capacity or exhaustion rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn allocate(
        &mut self,
        base: DurableBase,
        authority: RealmAuthorityView,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        let parent = check_authority(self.contract, self.realm, authority, now)?;
        if self.pending.is_some()
            || self.permit.is_some()
            || base.contract() != self.contract
            || base.generation() != self.generation
            || parent < self.authority_high_water
            || (base.generation() == 0 && parent != 1)
        {
            return Err(Error::State);
        }
        let id = self.high_water.checked_add(1).ok_or(Error::Exhausted)?;
        let mut body = [0; 16];
        encode_allocator_record(self.realm, id, parent, &mut body)?;
        self.pending = Some((
            DurableUpdate::prepare(base, &body, now, until, continuation, scratch)?,
            id,
            parent,
        ));
        Ok(())
    }
    /// Route through Coordinator; 经Coordinator路由。
    /// # Errors
    /// No pending update rejects.
    pub fn requirement(&self) -> Result<PersistenceRequest<'_>> {
        Ok(self.pending.as_ref().ok_or(Error::State)?.0.request())
    }
    /// Bind Foundation handle; 绑定Foundation凭据。
    /// # Errors
    /// Rebinding or wrong phase rejects.
    pub fn bind(&mut self, h: PersistenceHandle) -> Result<()> {
        self.pending.as_mut().ok_or(Error::State)?.0.bind(h)
    }
    /// Consume real reload proof, irrevocably burning the next ID; 消费真实回读证明，不可逆消耗ID。
    /// # Errors
    /// Wrong proof cannot release an ID.
    pub fn confirm(&mut self, h: PersistenceHandle, proof: PersistenceProof) -> Result<()> {
        self.pending
            .as_mut()
            .ok_or(Error::State)?
            .0
            .confirm(h, proof)?;
        let (_, id, parent) = self.pending.take().ok_or(Error::State)?;
        self.high_water = id;
        self.authority_high_water = parent;
        self.generation = proof.record_generation();
        self.permit = Some(GroupIdPermit {
            realm: self.realm,
            id,
            authority: parent,
            runtime: self.contract.runtime,
            allocator_domain: self.contract.domain.id,
        });
        Ok(())
    }
    /// Take one linear permission under the same currently-live Authority.
    /// 在同一个当前有效Authority下领取一次线性许可。
    /// # Errors
    /// Authority change/expiry rejects; high-water never moves back.
    pub fn take_permit(
        &mut self,
        authority: RealmAuthorityView,
        now: u64,
    ) -> Result<GroupIdPermit> {
        let parent = check_authority(self.contract, self.realm, authority, now)?;
        if self.permit.as_ref().ok_or(Error::State)?.authority != parent {
            return Err(Error::Access);
        }
        self.permit.take().ok_or(Error::State)
    }
    /// Explicitly discard unused permission, preserving high-water; 丢弃未用许可但保留高水位。
    pub fn burn_unused(&mut self) {
        self.permit = None;
    }
    /// Durable allocated high-water; 耐久分配高水位。
    #[must_use]
    pub const fn high_water(&self) -> u32 {
        self.high_water
    }
}
pub(crate) fn check_authority(
    c: DurableContract,
    realm: u32,
    v: RealmAuthorityView,
    now: u64,
) -> Result<u32> {
    if v.runtime() != c.runtime
        || v.epoch().realm.get() != realm
        || v.checked_at() != now
        || now == 0
        || now >= v.deadline()
    {
        return Err(Error::Access);
    }
    Ok(v.epoch().authority_generation.get())
}
fn read32(b: &[u8], i: usize) -> u32 {
    u32::from_be_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}
