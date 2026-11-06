use ucn_types::{Error, Result, TrafficClass};

const SCHEDULE: [usize; 12] = [0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2];
/// Exact Core TX reference, never a second payload owner.
/// 精确Core TX引用，绝不重复拥有Payload。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SendableRef {
    /// Runtime instance; Runtime实例。
    pub runtime: u32,
    /// Physical slot; 物理槽。
    pub slot: u16,
    /// Physical slot generation; 物理槽代际。
    pub generation: u32,
}
/// Queue metadata only; 仅队列元数据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct QueueItem {
    /// Core TX ref; Core引用。
    pub sendable: SendableRef,
    /// Trusted source quota key; 受信来源配额键。
    pub source: u32,
    /// Frozen Flow quota key; 冻结Flow配额键。
    pub flow: u32,
    /// Original class; 原始Class。
    pub class: TrafficClass,
    /// Optional half-open deadline; 可选半开时限。
    pub deadline: u64,
    /// Full Latest canonical key; 完整Latest键。
    pub latest: Option<[u8; 16]>,
    /// Q0 requires internally verified control authority.
    /// Q0要求内部已验证控制权限。
    pub control_authorized: bool,
}
/// Exact scheduler reservation handle; 精确调度Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct QueueHandle {
    slot: usize,
    generation: u32,
}
#[derive(Clone, Copy)]
struct Record {
    item: QueueItem,
    selected: bool,
    submitted: bool,
}
/// A single reference scheduler with class/source/flow caps and persistent cursors.
/// 单一引用调度器，Class/Source/Flow配额及持久游标。
pub struct QosOwner<const SLOTS: usize> {
    depths: [usize; 4],
    source_cap: usize,
    flow_cap: usize,
    records: [Option<Record>; SLOTS],
    generations: [u32; SLOTS],
    schedule_cursor: usize,
    slot_cursors: [usize; 4],
}
impl<const N: usize> QosOwner<N> {
    /// Initialize independently reserved class depths; 初始化独立Class容量。
    /// # Errors
    /// Depth sum/overflow or quota error rejects.
    pub fn new(depths: [usize; 4], source_cap: usize, flow_cap: usize) -> Result<Self> {
        let total = depths
            .iter()
            .try_fold(0usize, |sum, value| sum.checked_add(*value))
            .ok_or(Error::Config)?;
        if N == 0 || total != N || depths.contains(&0) || source_cap == 0 || flow_cap == 0 {
            return Err(Error::Config);
        }
        Ok(Self {
            depths,
            source_cap,
            flow_cap,
            records: [None; N],
            generations: [0; N],
            schedule_cursor: 0,
            slot_cursors: [0; 4],
        })
    }
    /// Enqueue only metadata; Latest replacement returns old TX ref for proper retirement.
    /// 仅入队元数据，Latest替换返回旧TX引用供正确退休。
    /// # Errors
    /// Invalid/Q0-unapproved/expired/full/quota input has no writes.
    pub fn enqueue(
        &mut self,
        item: QueueItem,
        now: u64,
    ) -> Result<(QueueHandle, Option<SendableRef>)> {
        if item.sendable.runtime == 0
            || item.sendable.generation == 0
            || item.source == 0
            || item.flow == 0
            || now == 0
            || (item.deadline != 0 && now >= item.deadline)
            || (item.class == TrafficClass::Q0 && !item.control_authorized)
            || item.latest == Some([0; 16])
        {
            return Err(Error::Argument);
        }
        if self
            .records
            .iter()
            .flatten()
            .any(|record| record.item.sendable == item.sendable)
        {
            return Err(Error::State);
        }
        let replacement = if let Some(key) = item.latest {
            self.records.iter().position(|record| {
                record.is_some_and(|r| {
                    r.item.latest == Some(key)
                        && r.item.class == item.class
                        && r.item.source == item.source
                        && r.item.flow == item.flow
                        && !r.selected
                        && !r.submitted
                })
            })
        } else {
            None
        };
        let other = self
            .records
            .iter()
            .enumerate()
            .filter(|(index, record)| Some(*index) != replacement && record.is_some());
        let count = other
            .clone()
            .filter(|(_, record)| record.is_some_and(|r| r.item.class == item.class))
            .count();
        if count >= self.depths[item.class as usize]
            || other
                .clone()
                .filter(|(_, record)| {
                    record
                        .is_some_and(|r| r.item.class == item.class && r.item.source == item.source)
                })
                .count()
                >= self.source_cap
            || other
                .filter(|(_, record)| {
                    record.is_some_and(|r| r.item.class == item.class && r.item.flow == item.flow)
                })
                .count()
                >= self.flow_cap
        {
            return Err(Error::NoSpace);
        }
        let slot = replacement
            .or_else(|| self.records.iter().position(Option::is_none))
            .ok_or(Error::NoSpace)?;
        let generation = self.generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let old = self.records[slot].map(|record| record.item.sendable);
        self.records[slot] = Some(Record {
            item,
            selected: false,
            submitted: false,
        });
        self.generations[slot] = generation;
        Ok((QueueHandle { slot, generation }, old))
    }
    fn index(&self, h: QueueHandle) -> Result<usize> {
        if h.slot >= N || h.generation != self.generations[h.slot] || self.records[h.slot].is_none()
        {
            Err(Error::State)
        } else {
            Ok(h.slot)
        }
    }
    /// Pick by bounded 6:3:2:1 class schedule and rotating slots.
    /// 6:3:2:1 Class表与轮转槽位有界选择。
    /// # Errors
    /// Unknown time rejects; empty/expired queues return None without reclassification.
    pub fn pick(&mut self, now: u64) -> Result<Option<(QueueHandle, QueueItem)>> {
        if now == 0 {
            return Err(Error::Argument);
        }
        for _ in 0..12 {
            let class = SCHEDULE[self.schedule_cursor];
            self.schedule_cursor = (self.schedule_cursor + 1) % 12;
            for offset in 0..N {
                let slot = (self.slot_cursors[class] + offset) % N;
                if let Some(record) = self.records[slot].as_mut() {
                    if record.item.class as usize == class
                        && !record.selected
                        && !record.submitted
                        && (record.item.deadline == 0 || now < record.item.deadline)
                    {
                        record.selected = true;
                        self.slot_cursors[class] = (slot + 1) % N;
                        return Ok(Some((
                            QueueHandle {
                                slot,
                                generation: self.generations[slot],
                            },
                            record.item,
                        )));
                    }
                }
            }
        }
        Ok(None)
    }
    /// Release temporary backpressure, preserving exact reference.
    /// 临时背压释放选择，保留精确引用。
    /// # Errors
    /// Unselected/submitted ref rejects.
    pub fn release_pick(&mut self, h: QueueHandle) -> Result<()> {
        let slot = self.index(h)?;
        let record = self.records[slot].as_mut().ok_or(Error::State)?;
        if !record.selected || record.submitted {
            return Err(Error::State);
        }
        record.selected = false;
        Ok(())
    }
    /// Mark actual Driver submission; 标记真实Driver提交。
    /// # Errors
    /// Wrong or repeated submission rejects.
    pub fn submitted(&mut self, h: QueueHandle) -> Result<()> {
        let slot = self.index(h)?;
        let record = self.records[slot].as_mut().ok_or(Error::State)?;
        if !record.selected || record.submitted {
            return Err(Error::State);
        }
        record.selected = false;
        record.submitted = true;
        Ok(())
    }
    /// Remove only after caller retires its physical TX obligation.
    /// 调用方退休物理TX义务后移除引用。
    /// # Errors
    /// Active selection or stale ref rejects.
    pub fn remove(&mut self, h: QueueHandle) -> Result<SendableRef> {
        let slot = self.index(h)?;
        let record = self.records[slot].ok_or(Error::State)?;
        if record.selected {
            return Err(Error::State);
        }
        self.records[slot] = None;
        Ok(record.item.sendable)
    }
}
