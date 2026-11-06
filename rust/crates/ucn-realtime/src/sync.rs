use crate::{DomainConfig, EventDirection, EventKey, TimePath, TimeSample, Timestamp};
use ucn_types::{Error, Result};

/// Exact Wire transaction identity; 精确Wire事务身份。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimeKey {
    /// Clock domain; 时间域。
    pub domain: u16,
    /// Domain generation; 域代际。
    pub generation: u32,
    /// Non-wrapping Sync sequence; 不回绕Sync序号。
    pub sequence: u32,
}
/// Opaque local transaction; 不可伪造本地事务。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimeTxnHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// Master response only after real T1/T4; 真实T1/T4之后才有Master回复。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DelayResponse {
    /// Exact Wire key; 精确Wire键。
    pub key: TimeKey,
    /// Master real TX; `Master真实TX`。
    pub t1: u64,
    /// Master real RX; `Master真实RX`。
    pub t4: u64,
    /// T1 capture bound; T1捕获界。
    pub t1_bound: u32,
    /// T4 capture bound; T4捕获界。
    pub t4_bound: u32,
}
/// Typed Sync completion verdict; 类型化Sync完成裁决。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SyncOutcome {
    /// Valid sample for Domain; 有效Domain样本。
    Sample(TimeSample),
    /// No asymmetry proof: diagnostic only; 无非对称证明仅诊断。
    Diagnostic {
        /// Reported offset; 报告Offset。
        offset_us: i64,
    },
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Role {
    Master,
    Member,
}
#[derive(Clone, Copy)]
struct Pending {
    role: Role,
    key: TimeKey,
    config: DomainConfig,
    peer: [u8; 16],
    tx: EventKey,
    rx: Option<EventKey>,
    first: Option<Timestamp>,
    second: Option<Timestamp>,
    follow_up: Option<(u64, u32)>,
    deadline: u64,
    started: u64,
}
/// Sync Owner with independent bounded local release obligations.
/// `SyncOwner与独立有界本地释放义务`。
pub struct SyncOwner<const PENDING: usize, const RELEASES: usize> {
    owner: u32,
    forward_instance: u32,
    reverse_instance: u32,
    slots: [Option<Pending>; PENDING],
    generations: [u32; PENDING],
    releases: [Option<EventKey>; RELEASES],
    cursor: usize,
    history: [Option<([u8; 16], TimeKey)>; PENDING],
}
impl<const N: usize, const R: usize> SyncOwner<N, R> {
    /// Reserve two local release slots per transaction; 每事务预留两个本地释放槽。
    /// # Errors
    /// Invalid owner or undersized release capacity rejects.
    pub fn new(owner: u32) -> Result<Self> {
        Self::with_link_instances(owner, owner, owner)
    }
    /// Bind the two local event namespaces, distinct from remote Wire keys.
    /// 绑定两个本地事件命名空间，与远端Wire键分离。
    /// # Errors
    /// Zero identities or insufficient release capacity rejects.
    pub fn with_link_instances(
        owner: u32,
        forward_instance: u32,
        reverse_instance: u32,
    ) -> Result<Self> {
        if owner == 0
            || forward_instance == 0
            || reverse_instance == 0
            || N == 0
            || N.checked_mul(2).is_none_or(|needed| R < needed)
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            forward_instance,
            reverse_instance,
            slots: [None; N],
            generations: [0; N],
            releases: [None; R],
            cursor: 0,
            history: [None; N],
        })
    }
    #[allow(clippy::large_types_passed_by_value)] // Private bounded transaction, copied once into its fixed slot.
    fn reserve(&mut self, pending: Pending) -> Result<TimeTxnHandle> {
        pending.config.path.validate()?;
        if pending.config.path.asymmetry.is_some() {
            pending.config.validate()?;
        }
        if pending.key.domain == 0
            || pending.key.domain == u16::MAX
            || pending.key.generation == 0
            || pending.key.generation > 0x7fff_ffff
            || pending.key.domain != pending.config.domain
            || pending.key.generation != pending.config.generation
            || pending.key.sequence == 0
            || pending.peer == [0; 16]
            || pending.tx.direction != EventDirection::Tx
            || pending.tx.instance
                != if pending.role == Role::Master {
                    self.forward_instance
                } else {
                    self.reverse_instance
                }
            || pending
                .rx
                .is_some_and(|key| key.instance != self.forward_instance)
        {
            return Err(Error::Argument);
        }
        if self.slots.iter().flatten().any(|slot| {
            slot.key == pending.key && slot.peer == pending.peer
                || slot.tx == pending.tx
                || slot.rx.is_some_and(|rx| Some(rx) == pending.rx)
        }) {
            return Err(Error::Replay);
        }
        let history_slot = self
            .history
            .iter()
            .position(|old| old.is_some_and(|(peer, _)| peer == pending.peer));
        if let Some(index) = history_slot {
            let (_, previous) = self.history[index].ok_or(Error::State)?;
            if previous.domain != pending.key.domain
                || pending.key.generation < previous.generation
                || pending.key.generation == previous.generation
                    && pending.key.sequence <= previous.sequence
            {
                return Err(Error::Replay);
            }
        }
        let history_index = history_slot
            .or_else(|| self.history.iter().position(Option::is_none))
            .ok_or(Error::NoSpace)?;
        let used = self.slots.iter().flatten().count() * 2 + self.releases.iter().flatten().count();
        if used + 2 > R {
            return Err(Error::NoSpace);
        }
        let slot = self
            .slots
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.slots[slot] = Some(pending);
        self.generations[slot] = generation;
        self.history[history_index] = Some((pending.peer, pending.key));
        Ok(TimeTxnHandle {
            owner: self.owner,
            slot,
            generation,
        })
    }
    fn index(&self, h: TimeTxnHandle) -> Result<usize> {
        if h.owner != self.owner
            || h.slot >= N
            || self.generations[h.slot] != h.generation
            || self.slots[h.slot].is_none()
        {
            Err(Error::State)
        } else {
            Ok(h.slot)
        }
    }
    /// Master freezes key/path and a genuine reserved T1 TX token.
    /// Master冻结键/Path及真实已预留T1 TX Token。
    /// # Errors
    /// Wrong token/link/generation/time or full capacity rejects.
    pub fn master_begin(
        &mut self,
        config: DomainConfig,
        peer: [u8; 16],
        sequence: u32,
        t1: EventKey,
        now: u64,
        duration: u64,
    ) -> Result<TimeTxnHandle> {
        if now == 0
            || duration == 0
            || t1.link != config.path.forward_link
            || t1.generation != config.path.forward_generation
        {
            return Err(Error::Argument);
        }
        let deadline = now.checked_add(duration).ok_or(Error::Argument)?;
        self.reserve(Pending {
            role: Role::Master,
            key: TimeKey {
                domain: config.domain,
                generation: config.generation,
                sequence,
            },
            config,
            peer,
            tx: t1,
            rx: None,
            first: None,
            second: None,
            follow_up: None,
            deadline,
            started: now,
        })
    }
    /// Member binds actual atomic SYNC RX and a reserved exact `DELAY_REQ` TX.
    /// `Member绑定实际原子SYNC` `RX及精确DELAY_REQ` TX。
    /// # Errors
    /// Cross-path, old generation or fictitious T2 cannot be bound.
    #[allow(clippy::too_many_arguments)] // Four independent event/path/peer identities must be explicit.
    pub fn member_begin(
        &mut self,
        config: DomainConfig,
        peer: [u8; 16],
        key: TimeKey,
        t2: Timestamp,
        t3: EventKey,
        now: u64,
        duration: u64,
    ) -> Result<TimeTxnHandle> {
        if now == 0
            || duration == 0
            || t2.time_us() > now
            || t2.key().direction != EventDirection::Rx
            || t2.key().link != config.path.forward_link
            || t2.key().generation != config.path.forward_generation
            || t3.link != config.path.reverse_link
            || t3.generation != config.path.reverse_generation
        {
            return Err(Error::Argument);
        }
        let deadline = now.checked_add(duration).ok_or(Error::Argument)?;
        self.reserve(Pending {
            role: Role::Member,
            key,
            config,
            peer,
            tx: t3,
            rx: Some(t2.key()),
            first: Some(t2),
            second: None,
            follow_up: None,
            deadline,
            started: now,
        })
    }
    /// Bind actual T1/T3 completion, never a successful submit alone.
    /// 绑定真实T1/T3 completion，submit成功本身不算。
    /// # Errors
    /// Wrong key/phase, backwards local event or expired transaction rejects.
    pub fn tx_completed(&mut self, h: TimeTxnHandle, timestamp: Timestamp, now: u64) -> Result<()> {
        let index = self.index(h)?;
        let pending = self.slots[index].as_mut().ok_or(Error::State)?;
        if timestamp.key() != pending.tx
            || now == 0
            || timestamp.time_us() > now
            || now >= pending.deadline
            || pending.role == Role::Master && timestamp.time_us() < pending.started
        {
            return Err(Error::State);
        }
        let prior = if pending.role == Role::Master {
            &mut pending.first
        } else {
            if pending
                .first
                .is_none_or(|t2| timestamp.time_us() < t2.time_us())
            {
                return Err(Error::Argument);
            }
            &mut pending.second
        };
        if prior.is_some_and(|old| old != timestamp) {
            return Err(Error::Replay);
        }
        *prior = Some(timestamp);
        Ok(())
    }
    /// Master emits response only with current Path and actual T4 RX.
    /// Exact `SYNC/DELAY_REQ` payload from the frozen transaction, never arbitrary bytes.
    /// `冻结事务的精确SYNC/DELAY_REQ载荷，不能用任意字节`。
    /// # Errors
    /// Stale handle rejects with output unchanged.
    pub fn request_payload(&self, h: TimeTxnHandle, out: &mut [u8; 12]) -> Result<()> {
        let p = self.slots[self.index(h)?].ok_or(Error::State)?;
        crate::encode_time_key(p.key, out)
    }
    /// `FOLLOW_UP` is constructible only after actual completed T1.
    /// 仅真实T1 `completion后才能构造FOLLOW_UP`。
    /// # Errors
    /// No actual T1, wrong role or expired transaction rejects.
    pub fn follow_up_payload(&self, h: TimeTxnHandle, now: u64, out: &mut [u8; 24]) -> Result<()> {
        let p = self.slots[self.index(h)?].ok_or(Error::State)?;
        let t1 = p.first.ok_or(Error::State)?;
        if p.role != Role::Master || now == 0 || now >= p.deadline {
            return Err(Error::State);
        }
        crate::encode_time_stamp(
            crate::TimeStampMessage {
                key: p.key,
                time: t1.time_us(),
                bound: t1.bound_us(),
            },
            out,
        )
    }
    /// Ingest authenticated raw `FOLLOW_UP` under exact frozen peer/path/key.
    /// 在精确冻结peer/path/key下接纳认证raw `FOLLOW_UP`。
    /// # Errors
    /// Wrong key, role, deadline or conflicting duplicate rejects atomically.
    pub fn accept_follow_up(
        &mut self,
        h: TimeTxnHandle,
        peer: [u8; 16],
        path: TimePath,
        bytes: &[u8],
        now: u64,
    ) -> Result<()> {
        let message = crate::decode_time_stamp(bytes)?;
        let index = self.index(h)?;
        let p = self.slots[index].as_mut().ok_or(Error::State)?;
        if p.role != Role::Member
            || p.peer != peer
            || p.config.path != path
            || p.key != message.key
            || now == 0
            || now >= p.deadline
            || p.follow_up
                .is_some_and(|old| old != (message.time, message.bound))
        {
            return Err(Error::State);
        }
        p.follow_up = Some((message.time, message.bound));
        Ok(())
    }
    /// Complete using separate actual `FOLLOW_UP` and `DELAY_RESP` messages.
    /// `通过分离的实际FOLLOW_UP和DELAY_RESP消息完成`。
    /// # Errors
    /// Missing `FOLLOW_UP` or key/peer/path mismatch rejects.
    pub fn delay_response_payload(
        &mut self,
        h: TimeTxnHandle,
        peer: [u8; 16],
        path: TimePath,
        bytes: &[u8],
        now: u64,
    ) -> Result<SyncOutcome> {
        let message = crate::decode_time_stamp(bytes)?;
        let p = self.slots[self.index(h)?].ok_or(Error::State)?;
        let (t1, t1_bound) = p.follow_up.ok_or(Error::State)?;
        self.member_response(
            h,
            peer,
            path,
            DelayResponse {
                key: message.key,
                t1,
                t4: message.time,
                t1_bound,
                t4_bound: message.bound,
            },
            now,
        )
    }
    /// Master emits response only with current Path and actual T4 RX.
    /// `Master仅在当前Path及真实T4` RX下产生回复。
    /// # Errors
    /// Cross-member/key/path, no T1 or expired transaction rejects.
    pub fn master_response(
        &mut self,
        h: TimeTxnHandle,
        key: TimeKey,
        peer: [u8; 16],
        path: TimePath,
        t4: Timestamp,
        now: u64,
    ) -> Result<DelayResponse> {
        let index = self.index(h)?;
        let pending = self.slots[index].ok_or(Error::State)?;
        let t1 = pending.first.ok_or(Error::State)?;
        if pending.role != Role::Master
            || pending.key != key
            || pending.peer != peer
            || pending.config.path != path
            || now == 0
            || now >= pending.deadline
            || t4.time_us() > now
            || t4.time_us() < t1.time_us()
            || t4.key().direction != EventDirection::Rx
            || t4.key().instance != self.reverse_instance
            || t4.key().link != path.reverse_link
            || t4.key().generation != path.reverse_generation
        {
            return Err(Error::State);
        }
        self.slots[index].as_mut().ok_or(Error::State)?.rx = Some(t4.key());
        let response = DelayResponse {
            key,
            t1: t1.time_us(),
            t4: t4.time_us(),
            t1_bound: t1.bound_us(),
            t4_bound: t4.bound_us(),
        };
        self.finish_slot(index)?;
        Ok(response)
    }
    /// Member combines only exact authenticated response and real T2/T3 events.
    /// Member仅合并精确认证回复及真实T2/T3事件。
    /// # Errors
    /// Missing T3, cross-peer/path/domain, impossible delay or unknown bounds rejects.
    pub fn member_response(
        &mut self,
        h: TimeTxnHandle,
        peer: [u8; 16],
        path: TimePath,
        response: DelayResponse,
        now: u64,
    ) -> Result<SyncOutcome> {
        let index = self.index(h)?;
        let pending = self.slots[index].ok_or(Error::State)?;
        let t2 = pending.first.ok_or(Error::State)?;
        let t3 = pending.second.ok_or(Error::State)?;
        if pending.role != Role::Member
            || pending.peer != peer
            || pending.key != response.key
            || pending.config.path != path
            || now == 0
            || now >= pending.deadline
            || response.t1 == 0
            || response.t4 < response.t1
            || response.t1_bound == 0
            || response.t4_bound == 0
        {
            return Err(Error::State);
        }
        let master_elapsed = response.t4 - response.t1;
        let member_elapsed = t3
            .time_us()
            .checked_sub(t2.time_us())
            .ok_or(Error::Argument)?;
        if master_elapsed < member_elapsed {
            return Err(Error::Policy);
        }
        let difference = (i128::from(response.t1) - i128::from(t2.time_us()))
            + (i128::from(response.t4) - i128::from(t3.time_us()));
        let offset = i64::try_from(difference / 2).map_err(|_| Error::Policy)?;
        let outcome = if path.asymmetry.is_none() {
            SyncOutcome::Diagnostic { offset_us: offset }
        } else {
            let base = pending.config.uncertainty.aggregate()?;
            // Account for both clocks' bounded drift during exchange and processing.
            // 同时覆盖两个时钟在交换和处理期间的有界漂移，不能把旧Offset当刚采样。
            let elapsed = now
                .checked_sub(t2.time_us())
                .ok_or(Error::Policy)?
                .max(master_elapsed);
            let drift = (u128::from(elapsed) * u128::from(pending.config.oscillator_ppb) * 2)
                .div_ceil(1_000_000_000);
            let drift = u32::try_from(drift).map_err(|_| Error::Policy)?;
            let bound = [
                response.t1_bound,
                response.t4_bound,
                t2.bound_us(),
                t3.bound_us(),
                1,
            ]
            .into_iter()
            .try_fold(
                base.checked_add(drift).ok_or(Error::Policy)?,
                |sum, value| sum.checked_add(value).ok_or(Error::Argument),
            )?;
            SyncOutcome::Sample(TimeSample {
                config: pending.config,
                local: now,
                sequence: pending.key.sequence,
                offset,
                uncertainty: bound,
            })
        };
        self.finish_slot(index)?;
        Ok(outcome)
    }
    fn finish_slot(&mut self, index: usize) -> Result<()> {
        let pending = self.slots[index].ok_or(Error::State)?;
        let count = 1 + usize::from(pending.rx.is_some());
        if self.releases.iter().filter(|item| item.is_none()).count() < count {
            return Err(Error::State);
        }
        for key in [Some(pending.tx), pending.rx].into_iter().flatten() {
            let slot = self
                .releases
                .iter()
                .position(Option::is_none)
                .ok_or(Error::State)?;
            self.releases[slot] = Some(key);
        }
        self.slots[index] = None;
        Ok(())
    }
    /// Peek, never destructive pop, the next local Driver release.
    /// peek下一本地Driver释放，绝不提前出队。
    #[must_use]
    pub fn release_peek(&self) -> Option<EventKey> {
        self.releases.iter().flatten().next().copied()
    }
    /// ACK only after actual Timed Link retirement succeeded.
    /// `TimedLink真实退休成功后才ACK`。
    /// # Errors
    /// Wrong/stale key has zero state writes.
    pub fn release_ack(&mut self, proof: crate::RetirementProof) -> Result<()> {
        let key = proof.key();
        let slot = self
            .releases
            .iter()
            .position(|item| *item == Some(key))
            .ok_or(Error::State)?;
        self.releases[slot] = None;
        Ok(())
    }
    /// Explicit local cancellation preserves both exact release obligations.
    /// 显式本地取消，保留两个精确退休义务，不伪造未来时刻。
    /// # Errors
    /// Wrong handle rejects without clearing tokens.
    pub fn cancel(&mut self, h: TimeTxnHandle) -> Result<()> {
        self.finish_slot(self.index(h)?)
    }
    /// Bounded timeout/path invalidation retains local keys for retirement.
    /// 有界超时/Path失效保留本地Key供退休。
    /// # Errors
    /// Unknown time/zero budget rejects.
    pub fn step(&mut self, now: u64, current_path: TimePath, budget: usize) -> Result<usize> {
        if now == 0 || budget == 0 {
            return Err(Error::Argument);
        }
        let count = budget.min(N);
        for _ in 0..count {
            let index = self.cursor;
            self.cursor = (self.cursor + 1) % N;
            if self.slots[index].is_some_and(|pending| {
                now >= pending.deadline || pending.config.path != current_path
            }) {
                self.finish_slot(index)?;
            }
        }
        Ok(count)
    }
}
