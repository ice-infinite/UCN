use crate::{DeliveryAck, ReliableKey, Submission, deadline};
use ucn_types::{Error, Result};

const ATTEMPT_HOLDERS: usize = 8;
/// Immutable terminal or retry phase; 不变终态或重试状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReliablePhase {
    /// Ready for bounded submission; 可提交。
    Ready,
    /// Waiting for final target ACK; 等最终目标ACK。
    WaitAck,
    /// Target Inbox accepted; 远端Inbox接纳。
    Done,
    /// Definitive failure; 明确失败。
    Failed,
    /// Local cancellation; 本地取消。
    Cancelled,
    /// Unproven Driver disposition; Driver状态未知。
    InDoubt,
}

/// Exact Reliable slot handle; 精确可靠槽Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReliableHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// One reserved Driver/Hop artifact holder; 单次Driver/Hop持有证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AttemptHandle {
    transfer: ReliableHandle,
    slot: usize,
    generation: u32,
}
/// Fixed Reliable scheduling policy; 固定重试策略。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReliableConfig {
    /// Unique Owner; 唯一Owner。
    pub owner: u32,
    /// Retry interval in local microseconds; 本机重试间隔。
    pub retry_us: u64,
    /// Maximum real submissions; 最大真实提交次数。
    pub max_attempts: u8,
    /// Receipt retention, covering legal retry/replay; Receipt保留期。
    pub receipt_retention_us: u64,
}

/// TX read-only view; TX只读视图。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReliableView {
    /// Original identity; 原事务身份。
    pub key: ReliableKey,
    /// Current phase; 状态。
    pub phase: ReliablePhase,
    /// Actual submissions; 实际提交次数。
    pub attempts: u8,
    /// Unretired Driver/Hop holders; 未退休持有者。
    pub holders: u8,
    /// Original deadline; 原始时限。
    pub deadline_us: u64,
    /// Earliest bounded retry; 最早重试。
    pub retry_at_us: u64,
}
#[derive(Clone, Copy)]
struct Holder {
    generation: u32,
    state: u8,
}
const EMPTY_HOLDER: Holder = Holder {
    generation: 0,
    state: 0,
};
struct Tx<const BYTES: usize> {
    key: ReliableKey,
    bytes: [u8; BYTES],
    length: usize,
    phase: ReliablePhase,
    attempts: u8,
    deadline: u64,
    retry: u64,
    holders: [Holder; ATTEMPT_HOLDERS],
    terminal_ack: Option<DeliveryAck>,
}

/// RX reservation; `RX预留Handle`。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReceiptHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Receipt {
    key: ReliableKey,
    aad: [u8; 16],
    payload: [u8; 16],
    deadline: u64,
    committed: bool,
}

/// Receiver outcome; 接收裁决。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReceiveAction {
    /// New inbox reservation; 新Inbox预留。
    Reserved(ReceiptHandle),
    /// Retained exact receipt, no second application delivery.
    /// 保留的精确Receipt，不再次业务交付。
    Duplicate(DeliveryAck),
}

/// Fixed TX/RX tables, without Route/Flow/Provider ownership.
/// 固定TX/RX表，不持有Route/Flow/Provider。
pub struct ReliableOwner<const TX: usize, const RX: usize, const BYTES: usize> {
    config: ReliableConfig,
    tx: [Option<Tx<BYTES>>; TX],
    tx_generations: [u32; TX],
    rx: [Option<Receipt>; RX],
    rx_generations: [u32; RX],
    cursor: usize,
}

impl<const TX: usize, const RX: usize, const BYTES: usize> ReliableOwner<TX, RX, BYTES> {
    /// Initialize fixed resources; 初始化固定资源。
    /// # Errors
    /// Zero capacities/identity or invalid durations are rejected.
    pub const fn new(config: ReliableConfig) -> Result<Self> {
        if config.owner == 0
            || TX == 0
            || RX == 0
            || BYTES == 0
            || config.max_attempts == 0
            || config.retry_us == 0
            || config.receipt_retention_us < config.retry_us
            || config.receipt_retention_us > i64::MAX as u64
        {
            return Err(Error::Config);
        }
        Ok(Self {
            config,
            tx: [const { None }; TX],
            tx_generations: [0; TX],
            rx: [None; RX],
            rx_generations: [0; RX],
            cursor: 0,
        })
    }

    /// Freeze exact Origin-sealed bytes once; 一次冻结Origin封装字节。
    /// # Errors
    /// Full capacity, duplicate identity, deadline or invalid input has no state writes.
    pub fn begin(
        &mut self,
        key: ReliableKey,
        sealed: &[u8],
        now: u64,
        deadline_us: u64,
    ) -> Result<ReliableHandle> {
        key.validate()?;
        if sealed.is_empty() || sealed.len() > BYTES || now == 0 || now >= deadline_us {
            return Err(Error::Argument);
        }
        if self.tx.iter().flatten().any(|tx| tx.key == key) {
            return Err(Error::State);
        }
        let slot = self
            .tx
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.tx_generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let mut bytes = [0; BYTES];
        bytes[..sealed.len()].copy_from_slice(sealed);
        self.tx[slot] = Some(Tx {
            key,
            bytes,
            length: sealed.len(),
            phase: ReliablePhase::Ready,
            attempts: 0,
            deadline: deadline_us,
            retry: now,
            holders: [EMPTY_HOLDER; ATTEMPT_HOLDERS],
            terminal_ack: None,
        });
        self.tx_generations[slot] = generation;
        Ok(ReliableHandle {
            owner: self.config.owner,
            slot,
            generation,
        })
    }
    fn index(&self, h: ReliableHandle) -> Result<usize> {
        if h.owner != self.config.owner
            || h.slot >= TX
            || h.generation != self.tx_generations[h.slot]
            || self.tx[h.slot].is_none()
        {
            Err(Error::State)
        } else {
            Ok(h.slot)
        }
    }

    /// Copy immutable Origin artifact; 复制不可变Origin封装。
    /// # Errors
    /// Wrong handle/capacity leaves output unchanged.
    pub fn copy_frame(&self, h: ReliableHandle, out: &mut [u8]) -> Result<usize> {
        let tx = self.tx[self.index(h)?].as_ref().ok_or(Error::State)?;
        if out.len() < tx.length {
            return Err(Error::NoSpace);
        }
        out[..tx.length].copy_from_slice(&tx.bytes[..tx.length]);
        Ok(tx.length)
    }

    /// Reserve one Hop/Driver holder before submitting; 提交前预留持有者。
    /// # Errors
    /// Stale/terminal/not-due/full/exhausted input has no state writes.
    pub fn attempt_begin(&mut self, h: ReliableHandle, now: u64) -> Result<AttemptHandle> {
        let index = self.index(h)?;
        let tx = self.tx[index].as_mut().ok_or(Error::State)?;
        if now == 0 || now >= tx.deadline {
            return Err(Error::Timeout);
        }
        if !matches!(tx.phase, ReliablePhase::Ready | ReliablePhase::WaitAck)
            || now < tx.retry
            || tx.attempts >= self.config.max_attempts
            || tx.holders.iter().any(|holder| holder.state == 1)
        {
            return Err(Error::State);
        }
        let slot = tx
            .holders
            .iter()
            .position(|holder| holder.state == 0)
            .ok_or(Error::NoSpace)?;
        let generation = tx.holders[slot]
            .generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        tx.holders[slot] = Holder {
            generation,
            state: 1,
        };
        Ok(AttemptHandle {
            transfer: h,
            slot,
            generation,
        })
    }
    fn holder_index(&self, h: AttemptHandle) -> Result<usize> {
        let index = self.index(h.transfer)?;
        let tx = self.tx[index].as_ref().ok_or(Error::State)?;
        if h.slot >= ATTEMPT_HOLDERS
            || tx.holders[h.slot].generation != h.generation
            || tx.holders[h.slot].state == 0
        {
            Err(Error::State)
        } else {
            Ok(index)
        }
    }

    /// Record the real Driver disposition; 记录真实Driver接纳结果。
    /// # Errors
    /// Duplicate disposition, bad time or arithmetic overflow does not consume the holder.
    pub fn submitted(&mut self, h: AttemptHandle, disposition: Submission, now: u64) -> Result<()> {
        let index = self.holder_index(h)?;
        let tx = self.tx[index].as_mut().ok_or(Error::State)?;
        if tx.holders[h.slot].state != 1 || now == 0 {
            return Err(Error::State);
        }
        let retry = now.saturating_add(self.config.retry_us);
        match disposition {
            Submission::NotSubmitted => {
                tx.holders[h.slot].state = 0;
                tx.retry = retry;
            }
            Submission::Submitted => {
                if tx.attempts == self.config.max_attempts {
                    return Err(Error::Exhausted);
                }
                tx.holders[h.slot].state = 2;
                tx.attempts += 1;
                tx.retry = retry;
                if !matches!(tx.phase, ReliablePhase::Done | ReliablePhase::Cancelled) {
                    tx.phase = ReliablePhase::WaitAck;
                }
            }
            Submission::InDoubt => {
                tx.holders[h.slot].state = 3;
                tx.phase = ReliablePhase::InDoubt;
            }
        }
        if now >= tx.deadline && !matches!(tx.phase, ReliablePhase::Done | ReliablePhase::Cancelled)
        {
            tx.phase = ReliablePhase::Failed;
        }
        Ok(())
    }

    /// Retire one exact real completion; 退休精确真实completion。
    /// # Errors
    /// Reserved-but-unsubmitted, stale or duplicate completions reject.
    pub fn attempt_complete(&mut self, h: AttemptHandle) -> Result<()> {
        let index = self.holder_index(h)?;
        let tx = self.tx[index].as_mut().ok_or(Error::State)?;
        if !matches!(tx.holders[h.slot].state, 2 | 3) {
            return Err(Error::State);
        }
        tx.holders[h.slot].state = 0;
        if tx.phase == ReliablePhase::InDoubt && tx.holders.iter().all(|holder| holder.state == 0) {
            tx.phase = ReliablePhase::Failed;
        }
        Ok(())
    }

    /// Consume final-target ACK and exact frozen security domain.
    /// 消费最终目标ACK与精确冻结安全域。
    /// # Errors
    /// Forged/stale/wrong-domain/late ACK cannot terminalize a transaction.
    pub fn on_ack(
        &mut self,
        h: ReliableHandle,
        reverse_key: ReliableKey,
        ack: DeliveryAck,
        now: u64,
    ) -> Result<()> {
        self.ack_preflight(h, reverse_key, ack, now)?;
        let index = self.index(h)?;
        let tx = self.tx[index].as_mut().ok_or(Error::State)?;
        if tx.terminal_ack.is_some() {
            return Ok(());
        }
        tx.terminal_ack = Some(ack);
        tx.phase = if ack.status == 0 {
            ReliablePhase::Done
        } else {
            ReliablePhase::Failed
        };
        Ok(())
    }
    /// Preflight before Coordinator commits Security Replay.
    /// `Coordinator提交Security` Replay前预检。
    /// # Errors
    /// Wrong identity or conflicting terminal ACK rejects without mutation.
    pub fn ack_preflight(
        &self,
        h: ReliableHandle,
        reverse_key: ReliableKey,
        ack: DeliveryAck,
        now: u64,
    ) -> Result<()> {
        reverse_key.validate()?;
        let tx = self.tx[self.index(h)?].as_ref().ok_or(Error::State)?;
        if now == 0
            || now >= tx.deadline
            || ack.service != tx.key.service
            || ack.sequence != tx.key.sequence
            || reverse_key.realm != tx.key.realm
            || reverse_key.source != tx.key.destination
            || reverse_key.destination != tx.key.source
            || reverse_key.security != tx.key.security
            || !matches!(
                tx.phase,
                ReliablePhase::WaitAck
                    | ReliablePhase::Ready
                    | ReliablePhase::Done
                    | ReliablePhase::Failed
            )
            || tx.attempts == 0
        {
            return Err(Error::Access);
        }
        if tx.terminal_ack.is_some_and(|old| old != ack) {
            return Err(Error::Replay);
        }
        Ok(())
    }

    /// Cancel logical work while retaining Driver holders; 取消逻辑事务但保留Driver持有者。
    /// # Errors
    /// Terminal/ambiguous transactions cannot be rewritten.
    pub fn cancel(&mut self, h: ReliableHandle) -> Result<()> {
        let index = self.index(h)?;
        let tx = self.tx[index].as_mut().ok_or(Error::State)?;
        if !matches!(tx.phase, ReliablePhase::Ready | ReliablePhase::WaitAck) {
            return Err(Error::State);
        }
        tx.phase = ReliablePhase::Cancelled;
        Ok(())
    }

    /// Terminal retirement only after all holders vanish; 全持有者退休后释放终态。
    /// # Errors
    /// Active/ambiguous state or real Driver holders reject.
    pub fn retire(&mut self, h: ReliableHandle) -> Result<()> {
        let index = self.index(h)?;
        let tx = self.tx[index].as_ref().ok_or(Error::State)?;
        if !matches!(
            tx.phase,
            ReliablePhase::Done | ReliablePhase::Failed | ReliablePhase::Cancelled
        ) || tx.holders.iter().any(|holder| holder.state != 0)
        {
            return Err(Error::State);
        }
        self.tx[index] = None;
        Ok(())
    }

    /// Read immutable TX status; 读取TX状态。
    /// # Errors
    /// Old handle is rejected.
    pub fn view(&self, h: ReliableHandle) -> Result<ReliableView> {
        let tx = self.tx[self.index(h)?].as_ref().ok_or(Error::State)?;
        Ok(ReliableView {
            key: tx.key,
            phase: tx.phase,
            attempts: tx.attempts,
            holders: u8::try_from(tx.holders.iter().filter(|holder| holder.state != 0).count())
                .map_err(|_| Error::State)?,
            deadline_us: tx.deadline,
            retry_at_us: tx.retry,
        })
    }

    /// Reserve retained receipt before Replay commit and application delivery.
    /// `Replay提交与业务交付前预留Receipt`。
    /// # Errors
    /// Conflict, expired replay, full capacity or missing digest reject atomically.
    pub fn receive_prepare(
        &mut self,
        key: ReliableKey,
        aad: [u8; 16],
        payload: [u8; 16],
        replay_candidate: bool,
        now: u64,
    ) -> Result<ReceiveAction> {
        key.validate()?;
        if aad == [0; 16] || payload == [0; 16] || now == 0 {
            return Err(Error::Argument);
        }
        if let Some(receipt) = self.rx.iter().flatten().find(|receipt| receipt.key == key) {
            if now >= receipt.deadline || receipt.aad != aad || receipt.payload != payload {
                return Err(Error::Replay);
            }
            if !receipt.committed {
                return Err(Error::State);
            }
            return Ok(ReceiveAction::Duplicate(DeliveryAck {
                service: key.service,
                sequence: key.sequence,
                status: 0,
                credit: 0,
            }));
        }
        if replay_candidate {
            return Err(Error::Replay);
        }
        let slot = self
            .rx
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.rx_generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let expires = deadline(now, self.config.receipt_retention_us)?;
        self.rx[slot] = Some(Receipt {
            key,
            aad,
            payload,
            deadline: expires,
            committed: false,
        });
        self.rx_generations[slot] = generation;
        Ok(ReceiveAction::Reserved(ReceiptHandle {
            owner: self.config.owner,
            slot,
            generation,
        }))
    }
    fn receipt_index(&self, h: ReceiptHandle) -> Result<usize> {
        if h.owner != self.config.owner
            || h.slot >= RX
            || h.generation != self.rx_generations[h.slot]
            || self.rx[h.slot].is_none()
        {
            Err(Error::State)
        } else {
            Ok(h.slot)
        }
    }

    /// Commit an already reserved Inbox obligation; 提交已预留Inbox。
    /// # Errors
    /// Wrong handle or duplicate commit fails; caller commits Security replay first.
    pub fn receive_commit(&mut self, h: ReceiptHandle) -> Result<DeliveryAck> {
        let index = self.receipt_index(h)?;
        let receipt = self.rx[index].as_mut().ok_or(Error::State)?;
        if receipt.committed {
            return Err(Error::State);
        }
        receipt.committed = true;
        Ok(DeliveryAck {
            service: receipt.key.service,
            sequence: receipt.key.sequence,
            status: 0,
            credit: 0,
        })
    }

    /// Roll back an unpublished reservation; 回滚未发布预留。
    /// # Errors
    /// Committed receipts cannot be revoked.
    pub fn receive_abort(&mut self, h: ReceiptHandle) -> Result<()> {
        let index = self.receipt_index(h)?;
        if self.rx[index].is_some_and(|receipt| receipt.committed) {
            return Err(Error::State);
        }
        self.rx[index] = None;
        Ok(())
    }

    /// Explicit bounded maintenance; 显式有界维护，输入路径不隐式驱逐。
    /// # Errors
    /// Unknown time or zero budget rejects.
    pub fn step(&mut self, now: u64, budget: usize) -> Result<usize> {
        if now == 0 || budget == 0 {
            return Err(Error::Argument);
        }
        let count = budget.min(TX + RX);
        for _ in 0..count {
            let index = self.cursor;
            self.cursor = (self.cursor + 1) % (TX + RX);
            if index < TX {
                if let Some(tx) = self.tx[index].as_mut() {
                    if matches!(tx.phase, ReliablePhase::Ready | ReliablePhase::WaitAck)
                        && (now >= tx.deadline
                            || (tx.attempts >= self.config.max_attempts && now >= tx.retry))
                    {
                        tx.phase = ReliablePhase::Failed;
                    }
                }
            } else {
                let index = index - TX;
                if self.rx[index]
                    .is_some_and(|receipt| receipt.committed && now >= receipt.deadline)
                {
                    self.rx[index] = None;
                }
            }
        }
        Ok(count)
    }
}

/// Nano fixed Profile; Nano固定档。
pub type NanoReliableOwner = ReliableOwner<2, 2, 128>;
/// Lite fixed Profile; Lite固定档。
pub type LiteReliableOwner = ReliableOwner<4, 8, 256>;
/// Full fixed Profile; Full固定档。
pub type FullReliableOwner = ReliableOwner<8, 24, 512>;
