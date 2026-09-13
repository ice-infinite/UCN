#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的单写 Owner、回调租约门、公平唤醒提示和 typed Coordinator 基础。
//!
//! 状态 Owner 使用 `&mut self` 表达唯一写权限；跨任务/ISR 只发布原子提示或使用受限
//! [`CallbackGate`]，不共享可写业务对象。

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use ucn_types::{Error, Result};

const GATE_IDLE: u32 = 0;
const GATE_TRANSITION: u32 = 1;
const GATE_ACTIVE: u32 = 2;
const MAILBOX_NEXT_CURSOR: [u32; 5] = [1, 2, 3, 4, 0];

/// 一次受控 Provider/Driver 回调的不可变归属声明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CallbackClaim {
    owner_instance: u32,
    operation_id: u32,
    operation_generation: u16,
    operation_kind: u16,
}

impl CallbackClaim {
    /// 构造完整非零回调声明。
    ///
    /// # Errors
    ///
    /// 任一字段为 0 时返回 [`Error::Argument`]。
    pub const fn new(
        owner_instance: u32,
        operation_id: u32,
        operation_generation: u16,
        operation_kind: u16,
    ) -> Result<Self> {
        if owner_instance == 0
            || operation_id == 0
            || operation_generation == 0
            || operation_kind == 0
        {
            return Err(Error::Argument);
        }
        Ok(Self {
            owner_instance,
            operation_id,
            operation_generation,
            operation_kind,
        })
    }

    fn packed_meta(self) -> u32 {
        (u32::from(self.operation_generation) << 16) | u32::from(self.operation_kind)
    }
}

/// `CallbackGate` 签发的精确、不可伪造字段组合。
#[derive(Clone, Copy)]
pub struct CallbackLease<'a> {
    gate: &'a CallbackGate,
    gate_instance: u32,
    lease_generation: u32,
    claim: CallbackClaim,
}

/// 调用方持有的、任务/ISR/SMP 安全的回调域门。
pub struct CallbackGate {
    gate_instance: u32,
    state: AtomicU32,
    lease_generation: AtomicU32,
    claim_owner: AtomicU32,
    claim_operation: AtomicU32,
    claim_meta: AtomicU32,
}

impl CallbackGate {
    /// 建立空闲回调门。
    ///
    /// # Errors
    ///
    /// `gate_instance` 为 0 时返回 [`Error::Argument`]。
    pub const fn new(gate_instance: u32) -> Result<Self> {
        if gate_instance == 0 {
            return Err(Error::Argument);
        }
        Ok(Self {
            gate_instance,
            state: AtomicU32::new(GATE_IDLE),
            lease_generation: AtomicU32::new(0),
            claim_owner: AtomicU32::new(0),
            claim_operation: AtomicU32::new(0),
            claim_meta: AtomicU32::new(0),
        })
    }

    /// 尝试进入回调域并签发新租约；已有调用活动时立即失败，不阻塞。
    ///
    /// # Errors
    ///
    /// 回调域忙返回 [`Error::State`]；租约代际耗尽返回 [`Error::Exhausted`]。
    pub fn try_enter(&self, claim: CallbackClaim) -> Result<CallbackLease<'_>> {
        self.state
            .compare_exchange(
                GATE_IDLE,
                GATE_TRANSITION,
                Ordering::Acquire,
                Ordering::Relaxed,
            )
            .map_err(|_| Error::State)?;

        let current = self.lease_generation.load(Ordering::Relaxed);
        let Some(next) = current.checked_add(1) else {
            self.state.store(GATE_IDLE, Ordering::Release);
            return Err(Error::Exhausted);
        };
        self.claim_owner
            .store(claim.owner_instance, Ordering::Relaxed);
        self.claim_operation
            .store(claim.operation_id, Ordering::Relaxed);
        self.claim_meta
            .store(claim.packed_meta(), Ordering::Relaxed);
        self.lease_generation.store(next, Ordering::Relaxed);
        self.state.store(GATE_ACTIVE, Ordering::Release);
        Ok(CallbackLease {
            gate: self,
            gate_instance: self.gate_instance,
            lease_generation: next,
            claim,
        })
    }

    /// 只允许签发该活动域的精确租约退出。
    ///
    /// # Errors
    ///
    /// 租约过期、错门或错声明返回 [`Error::State`]，活动调用保持不变。
    pub fn leave(&self, lease: CallbackLease<'_>) -> Result<()> {
        if !core::ptr::eq(self, lease.gate) || lease.gate_instance != self.gate_instance {
            return Err(Error::State);
        }
        self.state
            .compare_exchange(
                GATE_ACTIVE,
                GATE_TRANSITION,
                Ordering::Acquire,
                Ordering::Relaxed,
            )
            .map_err(|_| Error::State)?;

        let matches = self.lease_generation.load(Ordering::Relaxed) == lease.lease_generation
            && self.claim_owner.load(Ordering::Relaxed) == lease.claim.owner_instance
            && self.claim_operation.load(Ordering::Relaxed) == lease.claim.operation_id
            && self.claim_meta.load(Ordering::Relaxed) == lease.claim.packed_meta();
        if !matches {
            self.state.store(GATE_ACTIVE, Ordering::Release);
            return Err(Error::State);
        }

        self.claim_owner.store(0, Ordering::Relaxed);
        self.claim_operation.store(0, Ordering::Relaxed);
        self.claim_meta.store(0, Ordering::Relaxed);
        self.state.store(GATE_IDLE, Ordering::Release);
        Ok(())
    }

    /// 判断回调域是否活动；过渡态也保守地视为活动。
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.state.load(Ordering::Acquire) != GATE_IDLE
    }
}

/// Owner 的五类有界工作提示。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WorkClass {
    /// Driver/Provider completion。
    Completion = 0,
    /// Link/Context invalidation。
    Invalidation = 1,
    /// Cancel/retire obligation。
    CancelRetire = 2,
    /// Deadline/timer。
    Timer = 3,
    /// 新请求。
    Request = 4,
}

/// 从 Mailbox 取出的合并唤醒提示；它不是正确性事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct WorkHint {
    /// 工作类别。
    pub work_class: WorkClass,
    /// 自上次取出后合并的发布次数；在 `u32::MAX` 饱和。
    pub occurrences: u32,
}

/// 多发布者、单消费者的固定五类公平 Mailbox。
pub struct OwnerMailbox {
    occurrences: [AtomicU32; 5],
    cursor: AtomicU32,
    taker_active: AtomicBool,
}

impl OwnerMailbox {
    /// 建立空 Mailbox；可用于静态初始化。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            occurrences: [const { AtomicU32::new(0) }; 5],
            cursor: AtomicU32::new(0),
            taker_active: AtomicBool::new(false),
        }
    }

    /// 从任何受目标原子能力支持的任务/ISR 上下文发布一个提示。
    pub fn publish(&self, work_class: WorkClass) {
        let counter = &self.occurrences[work_class as usize];
        let _ = counter.fetch_update(Ordering::Release, Ordering::Relaxed, |value| {
            Some(value.saturating_add(1))
        });
    }

    /// 按跨调用持久游标取出一个类别；并发第二消费者立即失败。
    ///
    /// # Errors
    ///
    /// 没有提示返回 [`Error::NotFound`]；另一个消费者正在取提示返回 [`Error::State`]。
    pub fn take(&self) -> Result<WorkHint> {
        self.taker_active
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| Error::State)?;

        let start = self.cursor.load(Ordering::Relaxed) as usize;
        for offset in 0..self.occurrences.len() {
            let index = (start + offset) % self.occurrences.len();
            let occurrences = self.occurrences[index].swap(0, Ordering::AcqRel);
            if occurrences != 0 {
                self.cursor
                    .store(MAILBOX_NEXT_CURSOR[index], Ordering::Relaxed);
                self.taker_active.store(false, Ordering::Release);
                return Ok(WorkHint {
                    work_class: work_class_from_index(index),
                    occurrences,
                });
            }
        }
        self.taker_active.store(false, Ordering::Release);
        Err(Error::NotFound)
    }
}

impl Default for OwnerMailbox {
    fn default() -> Self {
        Self::new()
    }
}

const fn work_class_from_index(index: usize) -> WorkClass {
    match index {
        0 => WorkClass::Completion,
        1 => WorkClass::Invalidation,
        2 => WorkClass::CancelRetire,
        3 => WorkClass::Timer,
        _ => WorkClass::Request,
    }
}

/// Coordinator 内一次运行期依赖的精确 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DependencyHandle {
    coordinator_instance: u32,
    slot: u16,
    generation: u32,
}

/// `ensure()` 的结果；相同 canonical requirement 会复用原 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EnsureResult {
    /// 精确 Handle。
    pub handle: DependencyHandle,
    /// `true` 表示本次新建，`false` 表示精确复用。
    pub created: bool,
}

/// 一个依赖槽的只读快照。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DependencyView<R, E> {
    /// 完整 canonical requirement。
    pub requirement: R,
    /// 完成事件；`None` 表示仍在等待。
    pub event: Option<E>,
}

#[derive(Clone, Copy)]
struct DependencySlot<R, E> {
    generation: u32,
    requirement: Option<R>,
    event: Option<E>,
}

impl<R, E> DependencySlot<R, E> {
    const EMPTY: Self = Self {
        generation: 0,
        requirement: None,
        event: None,
    };
}

/// 固定容量、单写的 typed dependency Coordinator。
///
/// `R` 和 `E` 应分别是一个闭合的 Requirement/Event enum。Coordinator 不解释模块状态，只保存
/// canonical requirement、精确 Handle 与 immutable completion event。
pub struct TypedCoordinator<R: Copy + Eq, E: Copy + Eq, const SLOTS: usize> {
    instance: u32,
    slots: [DependencySlot<R, E>; SLOTS],
}

impl<R: Copy + Eq, E: Copy + Eq, const SLOTS: usize> TypedCoordinator<R, E, SLOTS> {
    /// 建立空 Coordinator。
    ///
    /// # Errors
    ///
    /// instance 为 0、容量为 0 或超过 `u16::MAX` 时返回 [`Error::Argument`]。
    pub const fn new(instance: u32) -> Result<Self> {
        if instance == 0 || SLOTS == 0 || SLOTS > u16::MAX as usize {
            return Err(Error::Argument);
        }
        Ok(Self {
            instance,
            slots: [DependencySlot::EMPTY; SLOTS],
        })
    }

    /// 确保 exact requirement 存在；只做逐值精确比较，不以 digest 代替相等性。
    ///
    /// # Errors
    ///
    /// 没有可用槽返回 [`Error::NoSpace`]；全部空槽代际耗尽返回 [`Error::Exhausted`]。
    pub fn ensure(&mut self, requirement: R) -> Result<EnsureResult> {
        for (index, slot) in self.slots.iter().enumerate() {
            if slot.requirement == Some(requirement) {
                return Ok(EnsureResult {
                    handle: self.handle(index, slot.generation)?,
                    created: false,
                });
            }
        }

        let mut exhausted = false;
        let mut busy = false;
        for index in 0..SLOTS {
            let slot = &mut self.slots[index];
            if slot.requirement.is_some() {
                busy = true;
                continue;
            }
            let Some(next) = slot.generation.checked_add(1) else {
                exhausted = true;
                continue;
            };
            slot.generation = next;
            slot.requirement = Some(requirement);
            slot.event = None;
            return Ok(EnsureResult {
                handle: DependencyHandle {
                    coordinator_instance: self.instance,
                    slot: u16::try_from(index + 1).map_err(|_| Error::Argument)?,
                    generation: next,
                },
                created: true,
            });
        }
        if exhausted && !busy {
            Err(Error::Exhausted)
        } else {
            Err(Error::NoSpace)
        }
    }

    /// 发布一次 immutable completion；完全相同的重复事件幂等成功。
    ///
    /// # Errors
    ///
    /// Handle 不匹配返回 [`Error::NotFound`]；冲突的第二终态返回 [`Error::State`]。
    pub fn complete(&mut self, handle: DependencyHandle, event: E) -> Result<()> {
        let slot = self.slot_mut(handle)?;
        match slot.event {
            None => {
                slot.event = Some(event);
                Ok(())
            }
            Some(current) if current == event => Ok(()),
            Some(_) => Err(Error::State),
        }
    }

    /// 读取精确依赖快照。
    ///
    /// # Errors
    ///
    /// Handle 不属于当前 Coordinator、已过期或已退休时返回 [`Error::NotFound`]。
    pub fn view(&self, handle: DependencyHandle) -> Result<DependencyView<R, E>> {
        let slot = self.slot(handle)?;
        Ok(DependencyView {
            requirement: slot.requirement.ok_or(Error::NotFound)?,
            event: slot.event,
        })
    }

    /// 取消尚未完成的依赖并释放槽；已完成依赖不能伪装成取消。
    ///
    /// # Errors
    ///
    /// Handle 不匹配返回 [`Error::NotFound`]；completion 已存在返回 [`Error::State`]。
    pub fn cancel(&mut self, handle: DependencyHandle) -> Result<()> {
        let slot = self.slot_mut(handle)?;
        if slot.event.is_some() {
            return Err(Error::State);
        }
        slot.requirement = None;
        Ok(())
    }

    /// 仅在 completion 已发布后退休依赖；代际高水位保留。
    ///
    /// # Errors
    ///
    /// Handle 不匹配返回 [`Error::NotFound`]；依赖仍 Pending 返回 [`Error::State`]。
    pub fn retire(&mut self, handle: DependencyHandle) -> Result<()> {
        let slot = self.slot_mut(handle)?;
        if slot.event.is_none() {
            return Err(Error::State);
        }
        slot.requirement = None;
        slot.event = None;
        Ok(())
    }

    fn handle(&self, index: usize, generation: u32) -> Result<DependencyHandle> {
        Ok(DependencyHandle {
            coordinator_instance: self.instance,
            slot: u16::try_from(index + 1).map_err(|_| Error::Argument)?,
            generation,
        })
    }

    fn slot(&self, handle: DependencyHandle) -> Result<&DependencySlot<R, E>> {
        let index = self.handle_index(handle)?;
        let slot = &self.slots[index];
        if slot.generation != handle.generation || slot.requirement.is_none() {
            return Err(Error::NotFound);
        }
        Ok(slot)
    }

    fn slot_mut(&mut self, handle: DependencyHandle) -> Result<&mut DependencySlot<R, E>> {
        let index = self.handle_index(handle)?;
        let slot = &mut self.slots[index];
        if slot.generation != handle.generation || slot.requirement.is_none() {
            return Err(Error::NotFound);
        }
        Ok(slot)
    }

    fn handle_index(&self, handle: DependencyHandle) -> Result<usize> {
        if handle.coordinator_instance != self.instance || handle.slot == 0 {
            return Err(Error::NotFound);
        }
        let index = usize::from(handle.slot - 1);
        if index >= SLOTS {
            return Err(Error::NotFound);
        }
        Ok(index)
    }
}

#[cfg(test)]
mod tests {
    use super::{DependencySlot, TypedCoordinator};
    use ucn_types::Error;

    #[test]
    fn exhausted_slot_never_wraps() {
        let mut coordinator = TypedCoordinator::<u32, u32, 1>::new(1).expect("valid coordinator");
        coordinator.slots[0] = DependencySlot {
            generation: u32::MAX,
            requirement: None,
            event: None,
        };
        assert_eq!(coordinator.ensure(1), Err(Error::Exhausted));
        assert_eq!(coordinator.slots[0].generation, u32::MAX);
    }
}
