use crate::{GroupDefinition, Member, SuccessScope};
use ucn_types::{Error, Result};
/// Product-owned immutable anti-rollback Manifest; 产品持有不可变防回退Manifest。
pub struct StaticManifest<'a, const M: usize> {
    /// Realm; Realm域。
    pub realm: u32,
    /// Verified product generation; 已验证产品代际。
    pub generation: u32,
    /// Compiled/verified generation minimum; 编译/验证代际下限。
    pub minimum_generation: u32,
    /// Exact immutable definitions; 精确不可变定义。
    pub entries: &'a [GroupDefinition<M>],
}
/// Exact send context handle; 精确发送上下文Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GroupSendHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
#[derive(Clone, Copy)]
#[allow(clippy::struct_field_names)] // Wire term Send ID is intentionally stable across Group attempts.
struct Send<const M: usize> {
    definition: GroupDefinition<M>,
    send_id: u64,
    scope: SuccessScope,
    subset: u32,
    receipts: u32,
    attempts: u32,
    deadline: u64,
    complete: bool,
    failed: bool,
}
/// Read-only frozen send result; 冻结发送结果视图。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GroupResult {
    /// Same Send ID across every attempt/fallback; 全部Attempt/fallback同一SendID。
    pub send_id: u64,
    /// Authenticated receipt bitmap; 认证Receipt位图。
    pub receipts: u32,
    /// Scope satisfied; Scope满足。
    pub complete: bool,
    /// Timed out or fenced; 超时或Fence。
    pub failed: bool,
}
/// Static Group Owner, independent of Persistence/Cluster/Realtime.
/// 独立于Persistence/Cluster/Realtime的StaticGroupOwner。
pub struct StaticGroupOwner<'a, const GROUPS: usize, const SENDS: usize, const M: usize> {
    owner: u32,
    manifest: StaticManifest<'a, M>,
    retired: [bool; GROUPS],
    fenced: [bool; GROUPS],
    sends: [Option<Send<M>>; SENDS],
    generations: [u32; SENDS],
    high_water: u64,
    cursor: usize,
}
impl<'a, const G: usize, const S: usize, const M: usize> StaticGroupOwner<'a, G, S, M> {
    /// Accept only product's borrowed immutable Manifest; 仅接纳产品借用不可变Manifest。
    /// # Errors
    /// Anti-rollback, duplicate Group IDs or invalid capacity rejects.
    pub fn new(owner: u32, manifest: StaticManifest<'a, M>) -> Result<Self> {
        if owner == 0
            || G == 0
            || S == 0
            || manifest.realm == 0
            || manifest.generation == 0
            || manifest.generation < manifest.minimum_generation
            || manifest.entries.len() > G
            || manifest.entries.is_empty()
        {
            return Err(Error::Config);
        }
        for (index, entry) in manifest.entries.iter().enumerate() {
            entry.validate()?;
            if entry.realm != manifest.realm
                || manifest.entries[..index]
                    .iter()
                    .any(|old| old.group == entry.group)
            {
                return Err(Error::Config);
            }
        }
        Ok(Self {
            owner,
            manifest,
            retired: [false; G],
            fenced: [false; G],
            sends: [None; S],
            generations: [0; S],
            high_water: 0,
            cursor: 0,
        })
    }
    /// Reserve the complete bounded fanout and frozen member set before first send.
    /// 首次发送前预留完整有界fanout和冻结成员集。
    /// # Errors
    /// Disabled scope, unauthenticated remote receipt, expansion or capacity rejects.
    pub fn send_begin(
        &mut self,
        group: u32,
        scope: SuccessScope,
        subset: u32,
        reliable: bool,
        now: u64,
        until: u64,
    ) -> Result<GroupSendHandle> {
        let group_slot = self
            .manifest
            .entries
            .iter()
            .position(|entry| entry.group == group)
            .ok_or(Error::NotFound)?;
        let definition = self.manifest.entries[group_slot];
        if self.retired[group_slot]
            || self.fenced[group_slot]
            || now == 0
            || now >= until
            || definition.count > definition.fanout_limit
            || definition.scope_mask & (1 << (scope as u8)) == 0
            || scope != SuccessScope::LocalOnly && (!reliable || !definition.exact_principal)
        {
            return Err(Error::Access);
        }
        let mask = if definition.count == 32 {
            u32::MAX
        } else {
            (1u32 << definition.count) - 1
        };
        if scope == SuccessScope::Subset && (subset == 0 || subset & !mask != 0)
            || scope != SuccessScope::Subset && subset != 0
        {
            return Err(Error::Argument);
        }
        let slot = self
            .sends
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let send_id = self.high_water.checked_add(1).ok_or(Error::Exhausted)?;
        self.sends[slot] = Some(Send {
            definition,
            send_id,
            scope,
            subset,
            receipts: 0,
            attempts: 0,
            deadline: until,
            complete: scope == SuccessScope::LocalOnly,
            failed: false,
        });
        self.generations[slot] = generation;
        self.high_water = send_id;
        Ok(GroupSendHandle {
            owner: self.owner,
            slot,
            generation,
        })
    }
    fn index(&self, h: GroupSendHandle) -> Result<usize> {
        if h.owner != self.owner
            || h.slot >= S
            || h.generation != self.generations[h.slot]
            || self.sends[h.slot].is_none()
        {
            Err(Error::State)
        } else {
            Ok(h.slot)
        }
    }
    /// Freeze each exact member attempt under the same Group Send ID.
    /// `每个精确成员Attempt使用同一GroupSendID`。
    /// # Errors
    /// Repeated attempt, expired or fenced send rejects.
    pub fn attempt(
        &mut self,
        h: GroupSendHandle,
        member: usize,
        now: u64,
    ) -> Result<(u64, Member)> {
        let slot = self.index(h)?;
        let send = self.sends[slot].as_mut().ok_or(Error::State)?;
        if member >= send.definition.count
            || send.failed
            || now == 0
            || now >= send.deadline
            || send.attempts & (1 << member) != 0
        {
            return Err(Error::State);
        }
        send.attempts |= 1 << member;
        Ok((send.send_id, send.definition.members[member]))
    }
    /// Count only exact authenticated frozen-member terminal receipts.
    /// 仅计入精确认证冻结成员终态Receipt。
    /// # Errors
    /// Wrong send/member/security/key/policy or unsent member rejects.
    pub fn receipt(
        &mut self,
        h: GroupSendHandle,
        send_id: u64,
        member: Member,
        key_generation: u32,
        policy_generation: u32,
        now: u64,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let send = self.sends[slot].as_mut().ok_or(Error::State)?;
        if send.scope == SuccessScope::LocalOnly
            || send.failed
            || send.send_id != send_id
            || send.definition.key_generation != key_generation
            || send.definition.generation != policy_generation
            || now == 0
            || now >= send.deadline
        {
            return Err(Error::Access);
        }
        let index = send.definition.members[..send.definition.count]
            .iter()
            .position(|value| *value == member)
            .ok_or(Error::Access)?;
        if send.attempts & (1 << index) == 0 {
            return Err(Error::Access);
        }
        send.receipts |= 1 << index;
        let total = if send.definition.count == 32 {
            u32::MAX
        } else {
            (1u32 << send.definition.count) - 1
        };
        send.complete = match send.scope {
            SuccessScope::LocalOnly => true,
            SuccessScope::Any => send.receipts != 0,
            SuccessScope::All => send.receipts == total,
            SuccessScope::Subset => send.receipts & send.subset == send.subset,
            SuccessScope::Quorum => {
                send.definition.members[..send.definition.count]
                    .iter()
                    .enumerate()
                    .filter(|(index, _)| send.receipts & (1 << index) != 0)
                    .map(|(_, member)| u16::from(member.weight))
                    .sum::<u16>()
                    >= send.definition.quorum_weight
            }
        };
        Ok(())
    }
    /// Permanently retire a static Manifest slot; 永久退休静态Manifest槽。
    /// # Errors
    /// Unknown Group rejects; IDs are never recycled.
    pub fn retire_group(&mut self, group: u32) -> Result<()> {
        let index = self
            .manifest
            .entries
            .iter()
            .position(|entry| entry.group == group)
            .ok_or(Error::NotFound)?;
        self.retired[index] = true;
        for send in self.sends.iter_mut().flatten() {
            if send.definition.group == group {
                send.failed = true;
            }
        }
        Ok(())
    }
    /// Fence changed key/member/tree/policy dependencies before new attempts.
    /// 新Attempt之前Fence发生变化的Key/Member/Tree/Policy依赖。
    /// # Errors
    /// Unknown Group rejects.
    pub fn fence(&mut self, group: u32) -> Result<()> {
        let index = self
            .manifest
            .entries
            .iter()
            .position(|entry| entry.group == group)
            .ok_or(Error::NotFound)?;
        self.fenced[index] = true;
        for send in self.sends.iter_mut().flatten() {
            if send.definition.group == group {
                send.failed = true;
            }
        }
        Ok(())
    }
    /// Read frozen result; 读取冻结结果。
    /// # Errors
    /// Stale handle rejects.
    pub fn result(&self, h: GroupSendHandle) -> Result<GroupResult> {
        let send = self.sends[self.index(h)?].ok_or(Error::State)?;
        Ok(GroupResult {
            send_id: send.send_id,
            receipts: send.receipts,
            complete: send.complete,
            failed: send.failed,
        })
    }
    /// Retire terminal logical send, after Runtime retires physical attempts.
    /// `Runtime退休物理Attempt后退休逻辑发送`。
    /// # Errors
    /// Nonterminal send rejects.
    pub fn retire_send(&mut self, h: GroupSendHandle) -> Result<()> {
        let index = self.index(h)?;
        let send = self.sends[index].ok_or(Error::State)?;
        if !send.complete && !send.failed {
            return Err(Error::State);
        }
        self.sends[index] = None;
        Ok(())
    }
    /// Bounded explicit timeout; 有界显式超时。
    /// # Errors
    /// Unknown time/zero budget rejects.
    pub fn step(&mut self, now: u64, budget: usize) -> Result<usize> {
        if now == 0 || budget == 0 {
            return Err(Error::Argument);
        }
        let count = budget.min(S);
        for _ in 0..count {
            let slot = self.cursor;
            self.cursor = (self.cursor + 1) % S;
            if let Some(send) = self.sends[slot].as_mut() {
                if now >= send.deadline && !send.complete {
                    send.failed = true;
                }
            }
        }
        Ok(count)
    }
}
