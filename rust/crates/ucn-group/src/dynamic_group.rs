//! One fixed Manifest slot owns one bounded common Context record.
//! 一个固定配置槽持有一个有界共同Context Record。
use crate::allocator::check_authority;
use crate::{
    GROUP_CONTEXT_SCHEMA, GROUP_CONTEXT_VERSION, GroupIdPermit, GroupRecord, decode_group_record,
    encode_group_record,
};
use ucn_durable::{DurableBase, DurableContract, DurableUpdate};
use ucn_identity::RealmAuthorityView;
use ucn_persistence::{
    CodecWorkspace, DomainKind, PersistenceHandle, PersistenceProof, PersistenceRequest,
};
use ucn_types::{Error, Result};
/// Dynamic context state; 动态上下文状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DynamicGroupPhase {
    /// Current verified authority; 当前已验证权限。
    Active,
    /// Reloaded or expired dependency; 恢复或依赖已失效。
    Fenced,
    /// Durable retired ID; 耐久退休ID。
    Retired,
}
/// One per-slot Context, separate from the Realm allocator and unrelated optional Owners.
/// 逐槽Context，独立于Realm分配器和其他可选Owner。
pub struct DynamicGroupOwner<const MEMBERS: usize, const BODY: usize> {
    contract: DurableContract,
    realm: u32,
    generation: u64,
    record: Option<GroupRecord<MEMBERS>>,
    authority_generation: u32,
    phase: DynamicGroupPhase,
    pending: Option<(DurableUpdate<BODY>, GroupRecord<MEMBERS>, u32)>,
}
impl<const M: usize, const B: usize> DynamicGroupOwner<M, B> {
    /// Bind exactly one compile-time slot domain; 绑定恰好一个编译期槽域。
    /// # Errors
    /// Wrong schema/kind or body bound rejects.
    pub fn new(c: DurableContract, realm: u32) -> Result<Self> {
        if realm == 0
            || M == 0
            || M > 32
            || B < 64 + 28 * M
            || c.domain.kind != DomainKind::GroupPolicyKey
            || c.schema != GROUP_CONTEXT_SCHEMA
            || c.version != GROUP_CONTEXT_VERSION
            || c.operation != GROUP_CONTEXT_SCHEMA
            || c.domain.id == crate::group_allocator_domain_id(realm)
        {
            return Err(Error::Config);
        }
        Ok(Self {
            contract: c,
            realm,
            generation: 0,
            record: None,
            authority_generation: 0,
            phase: DynamicGroupPhase::Fenced,
            pending: None,
        })
    }
    /// Consume a separately durable allocator permission before Context commit.
    /// Context提交前消费单独耐久分配器许可。
    /// # Errors
    /// Wrong Authority/Realm/ID, nonempty unretired slot or insufficient fixed resources rejects.
    #[allow(clippy::too_many_arguments, clippy::needless_pass_by_value)]
    pub fn create(
        &mut self,
        base: DurableBase,
        permit: GroupIdPermit,
        mut r: GroupRecord<M>,
        authority: RealmAuthorityView,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<u32> {
        let parent = check_authority(self.contract, self.realm, authority, now)?;
        if permit.realm != self.realm
            || permit.allocator_domain != crate::group_allocator_domain_id(self.realm)
            || permit.runtime != self.contract.runtime
            || permit.authority != parent
            || r.definition.realm != self.realm
            || self
                .record
                .is_some_and(|old| !old.retired || permit.id <= old.definition.group)
        {
            return Err(Error::Access);
        }
        r.definition.group = permit.id;
        r.definition.generation = 1;
        r.retired = false;
        r.allocated_high_water = permit.id;
        let id = permit.id;
        self.prepare(base, r, parent, now, until, continuation, scratch)?;
        Ok(id)
    }
    /// Change a Context only with checked-next immutable generations.
    /// 仅checked-next不变代际允许变更Context。
    /// # Errors
    /// No exact old record, decreasing/unchanged identity or lost Authority rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn update(
        &mut self,
        base: DurableBase,
        r: GroupRecord<M>,
        authority: RealmAuthorityView,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        let parent = check_authority(self.contract, self.realm, authority, now)?;
        let old = self.record.ok_or(Error::NotFound)?;
        if old.retired
            || r.retired
            || r.definition.group != old.definition.group
            || r.definition.generation
                != old
                    .definition
                    .generation
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?
            || r.policy_generation < old.policy_generation
            || r.member_generation < old.member_generation
            || r.definition.key_generation < old.definition.key_generation
            || r.definition.tree_generation < old.definition.tree_generation
            || r.allocated_high_water != old.allocated_high_water
            || ((r.definition.members != old.definition.members
                || r.definition.count != old.definition.count)
                && r.member_generation
                    != old
                        .member_generation
                        .checked_add(1)
                        .ok_or(Error::Exhausted)?)
        {
            return Err(Error::State);
        }
        self.prepare(base, r, parent, now, until, continuation, scratch)
    }
    /// Retire durably; never return the ID to the allocator. 耐久退休，ID永不返还。
    /// # Errors
    /// Missing/already retired context or stale Authority rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn retire(
        &mut self,
        base: DurableBase,
        authority: RealmAuthorityView,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        let parent = check_authority(self.contract, self.realm, authority, now)?;
        let mut r = self.record.ok_or(Error::NotFound)?;
        if r.retired {
            return Err(Error::State);
        }
        r.retired = true;
        self.prepare(base, r, parent, now, until, continuation, scratch)
    }
    #[allow(clippy::too_many_arguments)]
    fn prepare(
        &mut self,
        base: DurableBase,
        r: GroupRecord<M>,
        parent: u32,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if self.pending.is_some()
            || base.contract() != self.contract
            || base.generation() != self.generation
            || r.definition.realm != self.realm
        {
            return Err(Error::State);
        }
        let mut body = [0; B];
        let n = encode_group_record(r, &mut body)?;
        self.pending = Some((
            DurableUpdate::prepare(base, &body[..n], now, until, continuation, scratch)?,
            r,
            parent,
        ));
        Ok(())
    }
    /// Coordinator-routed fixed requirement; Coordinator路由的固定要求。
    /// # Errors
    /// Missing pending rejects.
    pub fn requirement(&self) -> Result<PersistenceRequest<'_>> {
        Ok(self.pending.as_ref().ok_or(Error::State)?.0.request())
    }
    /// Bind accepted Foundation handle; 绑定Foundation凭据。
    /// # Errors
    /// Wrong phase/rebinding rejects.
    pub fn bind(&mut self, h: PersistenceHandle) -> Result<()> {
        self.pending.as_mut().ok_or(Error::State)?.0.bind(h)
    }
    /// Consume exact reload proof; stale live dependencies fence but cannot undo durable state.
    /// 消费精确回读证明；失效依赖Fence但不撤销耐久事实。
    /// # Errors
    /// Wrong proof rejects before installing the Context.
    pub fn confirm(
        &mut self,
        h: PersistenceHandle,
        p: PersistenceProof,
        authority: Option<RealmAuthorityView>,
        now: u64,
    ) -> Result<()> {
        self.pending.as_mut().ok_or(Error::State)?.0.confirm(h, p)?;
        let (_, r, parent) = self.pending.take().ok_or(Error::State)?;
        self.record = Some(r);
        self.generation = p.record_generation();
        self.authority_generation = parent;
        self.phase = if r.retired {
            DynamicGroupPhase::Retired
        } else if authority
            .is_some_and(|v| check_authority(self.contract, self.realm, v, now) == Ok(parent))
        {
            DynamicGroupPhase::Active
        } else {
            DynamicGroupPhase::Fenced
        };
        Ok(())
    }
    /// Restore only exact verified logical bytes, always Fenced. 恢复精确逻辑正文，始终Fence。
    /// # Errors
    /// Missing restore, corrupt schema/domain/record rejects.
    pub fn restore(
        &mut self,
        base: DurableBase,
        b: &[u8],
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if base.contract() != self.contract || self.record.is_some() || self.pending.is_some() {
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
        let r = decode_group_record(b)?;
        if r.definition.realm != self.realm {
            return Err(Error::Malformed);
        }
        self.record = Some(r);
        self.generation = base.generation();
        self.phase = if r.retired {
            DynamicGroupPhase::Retired
        } else {
            DynamicGroupPhase::Fenced
        };
        Ok(())
    }
    /// Immutable logical view, never proof of authority; 不变逻辑View，不证明权限。
    /// # Errors
    /// Uncreated slot rejects.
    pub fn view(&self) -> Result<(GroupRecord<M>, DynamicGroupPhase)> {
        Ok((self.record.ok_or(Error::NotFound)?, self.phase))
    }
    /// Recheck current exact parent before any publication; 发布前复验当前精确父域。
    /// # Errors
    /// Reloaded/pending/retired/stale authority cannot publish.
    pub fn authorized(&self, v: RealmAuthorityView, now: u64) -> Result<GroupRecord<M>> {
        let parent = check_authority(self.contract, self.realm, v, now)?;
        if self.phase != DynamicGroupPhase::Active
            || self.pending.is_some()
            || parent != self.authority_generation
        {
            return Err(Error::Access);
        }
        self.record.ok_or(Error::State)
    }
}
