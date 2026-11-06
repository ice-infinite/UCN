use crate::{
    FragmentPrefix, ParentKind, Sack, TransferParent, TransferPermit, TransferSetup, deadline,
};
use ucn_digest::{DigestWorkspace, canonical_begin, canonical_digest};
use ucn_types::{DeliveryGuarantee, Error, InteractionRole, Result};

/// Transfer phases, with application buffer ownership distinct from protocol completion.
/// Transfer状态，应用Buffer所有权独立于协议完成。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TransferPhase {
    /// Awaiting Setup ACK; `等SetupACK`。
    SetupPending,
    /// TX sending; TX发送。
    Sending,
    /// RX reassembly; RX重组。
    Receiving,
    /// Complete validated RX; 完整校验RX。
    Complete,
    /// Application holds payload; 应用持有Buffer。
    AppHeld,
    /// Delivered payload, retained protocol receipt; 已交付并保留Receipt。
    Delivered,
    /// TX terminal target receipt; `TX目标终态Receipt`。
    Done,
    /// Cancelled before delivery; 未交付取消。
    Aborted,
    /// Definitive failure; 明确失败。
    Failed,
}
/// Exact Transfer handle; `精确TransferHandle`。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferHandle {
    owner: u32,
    slot: usize,
    generation: u32,
    rx: bool,
}
/// Application buffer claim; 应用Buffer领取凭据。
#[derive(Debug, Eq, PartialEq)]
pub struct PayloadClaim {
    handle: TransferHandle,
}
/// Per-fragment immutable authentication evidence; 每片不可变认证证据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FragmentEvidence {
    /// Immutable Origin Sequence; 不变Origin序号。
    pub sequence: u32,
    /// Canonical AAD digest; AAD摘要。
    pub aad: [u8; 16],
    /// Exact plaintext payload digest; 精确明文载荷摘要。
    pub payload: [u8; 16],
}

/// Linear preflight, exclusively borrowing the RX Owner until commit or drop.
/// 线性预检：提交或丢弃前独占借用RX Owner；丢弃不改写重组状态。
#[must_use]
pub struct FragmentAdmission<'a, 'p, const F: usize> {
    record: &'a mut Record<F>,
    bytes: &'a mut [u8],
    saved_evidence: &'a mut Option<FragmentEvidence>,
    handle: TransferHandle,
    index: u16,
    evidence: FragmentEvidence,
    payload: &'p [u8],
    duplicate: bool,
    complete: bool,
}
impl<const F: usize> FragmentAdmission<'_, '_, F> {
    /// Prospective cumulative SACK, not publishable before commit.
    /// 预计累计SACK，提交前不能发送。
    #[must_use]
    pub fn sack(&self) -> Sack {
        let record = &self.record;
        let base = self.index / 32 * 32;
        let mut bitmap = 0;
        for bit in 0..32 {
            let i = usize::from(base) + bit;
            if i < usize::from(record.setup.fragments)
                && (record.bits[i] || i == usize::from(self.index))
            {
                bitmap |= 1 << bit;
            }
        }
        Sack {
            kind: record.parent.kind,
            transfer_id: record.setup.transfer_id,
            parent_generation: if record.parent.kind == ParentKind::C1 {
                record.parent.generation
            } else {
                0
            },
            original_service: if record.parent.kind == ParentKind::C1 {
                record.setup.service
            } else {
                0
            },
            base,
            bitmap,
            credit: 32,
        }
    }
    /// Infallible commit after Replay succeeds; no callbacks or allocation.
    /// Replay成功后无失败提交：不调用回调、不分配内存。
    #[must_use]
    pub fn commit(self) -> bool {
        if self.duplicate {
            return false;
        }
        self.bytes.copy_from_slice(self.payload);
        self.record.bits[usize::from(self.index)] = true;
        *self.saved_evidence = Some(self.evidence);
        if self.complete {
            self.record.phase = TransferPhase::Complete;
        }
        // Complete reassembly is not application delivery; do not publish terminal success.
        // 重组完成不是业务交付，尚不能发布成功终态。
        true
    }
    /// Exact borrowed receiver; 精确借用的接收实例。
    #[must_use]
    pub const fn handle(&self) -> TransferHandle {
        self.handle
    }
}
/// Frozen Setup and local deadline; 冻结Setup和本地时限。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferView {
    /// Setup; Setup。
    pub setup: TransferSetup,
    /// Current phase; 当前阶段。
    pub phase: TransferPhase,
    /// Original deadline; 原始时限。
    pub deadline_us: u64,
    /// Safely received/acknowledged fragments; 已确认片数。
    pub completed_fragments: usize,
}
struct Record<const FRAGMENTS: usize> {
    parent: TransferParent,
    setup: TransferSetup,
    bits: [bool; FRAGMENTS],
    sent: [bool; FRAGMENTS],
    prepared: [bool; FRAGMENTS],
    phase: TransferPhase,
    deadline: u64,
    receipt: usize,
    credit: u16,
    setup_credit: Option<u16>,
    terminal: Option<bool>,
    // Full identity is already frozen in Parent + Setup; retain only nonredundant result fields.
    // 完整身份已冻结于Parent与Setup，仅保留不重复的结果字段，避免增加每槽冗余。
    wire_terminal: Option<(u8, u16)>,
    wire_abort: Option<u8>,
}
#[derive(Clone, Copy)]
struct Receipt {
    parent: TransferParent,
    setup: TransferSetup,
    expires: u64,
    terminal: Option<bool>,
}

/// Fixed transfer/reassembly and independent receipt tables.
/// 固定传输/重组及独立Receipt表。
pub struct TransferOwner<
    const TX: usize,
    const RX: usize,
    const BYTES: usize,
    const FRAGMENTS: usize,
    const RECEIPTS: usize,
> {
    owner: u32,
    retention: u64,
    feedback_service: u16,
    tx: [Option<Record<FRAGMENTS>>; TX],
    rx: [Option<Record<FRAGMENTS>>; RX],
    tx_bytes: [[u8; BYTES]; TX],
    rx_bytes: [[u8; BYTES]; RX],
    rx_evidence: [[Option<FragmentEvidence>; FRAGMENTS]; RX],
    tx_gen: [u32; TX],
    rx_gen: [u32; RX],
    receipts: [Option<Receipt>; RECEIPTS],
    cursor: usize,
}
impl<const TX: usize, const RX: usize, const BYTES: usize, const F: usize, const R: usize>
    TransferOwner<TX, RX, BYTES, F, R>
{
    /// Initialize without linking ordinary Core state; 初始化独立固定状态。
    /// # Errors
    /// Invalid capacity/retention rejects.
    pub const fn new(owner: u32, retention: u64, feedback_service: u16) -> Result<Self> {
        if owner == 0
            || (TX == 0 && RX == 0)
            || BYTES == 0
            || F == 0
            || F > 128
            || R < RX
            || retention == 0
            || retention > i64::MAX as u64
            || feedback_service == 0
            || feedback_service == u16::MAX
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            retention,
            feedback_service,
            tx: [const { None }; TX],
            rx: [const { None }; RX],
            tx_bytes: [[0; BYTES]; TX],
            rx_bytes: [[0; BYTES]; RX],
            rx_evidence: [[None; F]; RX],
            tx_gen: [0; TX],
            rx_gen: [0; RX],
            receipts: [None; R],
            cursor: 0,
        })
    }

    /// Freeze Setup and entire immutable TX business payload.
    /// `冻结Setup和完整TX业务载荷`。
    /// # Errors
    /// Wrong allocated ID/digest/budget/parent rejects before state writes.
    #[allow(clippy::needless_pass_by_value)] // Linear ID permission must be consumed, never borrowed/reused.
    pub fn begin(
        &mut self,
        permit: TransferPermit,
        setup: TransferSetup,
        payload: &[u8],
        now: u64,
        scratch: &mut DigestWorkspace,
    ) -> Result<TransferHandle> {
        Self::validate_setup(permit.parent, setup)?;
        if setup.transfer_id != permit.transfer_id
            || payload.len() != setup.total_bytes as usize
            || canonical_digest(b"UCN6-TRANSFER-MESSAGE-V1", payload, scratch)? != setup.digest
        {
            return Err(Error::Argument);
        }
        if self.tx.iter().flatten().any(|record| {
            record.parent == permit.parent && record.setup.transfer_id == setup.transfer_id
        }) {
            return Err(Error::State);
        }
        let slot = self
            .tx
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.tx_gen[slot].checked_add(1).ok_or(Error::Exhausted)?;
        let expires = deadline(now, u64::from(setup.lifetime_ms) * 1000)?;
        self.tx_bytes[slot].fill(0);
        self.tx_bytes[slot][..payload.len()].copy_from_slice(payload);
        self.tx[slot] = Some(Record {
            parent: permit.parent,
            setup,
            bits: [false; F],
            sent: [false; F],
            prepared: [false; F],
            phase: TransferPhase::SetupPending,
            deadline: expires,
            receipt: 0,
            credit: 0,
            setup_credit: None,
            terminal: None,
            wire_terminal: None,
            wire_abort: None,
        });
        self.tx_gen[slot] = generation;
        Ok(TransferHandle {
            owner: self.owner,
            slot,
            generation,
            rx: false,
        })
    }
    fn validate_setup(parent: TransferParent, setup: TransferSetup) -> Result<()> {
        parent.validate()?;
        setup.validate()?;
        if setup.kind != parent.kind
            || setup.parent_id != parent.id
            || setup.parent_generation != parent.generation
            || setup.total_bytes as usize > BYTES
            || setup.fragments as usize > F
            || setup.fragment_bytes as usize > BYTES
            || setup.delivery == DeliveryGuarantee::Latest
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
    fn tx_index(&self, h: TransferHandle) -> Result<usize> {
        if h.owner != self.owner
            || h.rx
            || h.slot >= TX
            || h.generation != self.tx_gen[h.slot]
            || self.tx[h.slot].is_none()
        {
            Err(Error::State)
        } else {
            Ok(h.slot)
        }
    }

    /// Current exact TX fragment still needs feedback; 当前精确TX分片尚需确认。
    /// # Errors
    /// Stale handle, wrong phase or index rejects.
    pub fn fragment_needed(&self, h: TransferHandle, index: u16) -> Result<bool> {
        let r = self.tx[self.tx_index(h)?].as_ref().ok_or(Error::State)?;
        if r.phase != TransferPhase::Sending || index >= r.setup.fragments {
            return Err(Error::State);
        }
        Ok(!r.bits[usize::from(index)])
    }
    fn rx_index(&self, h: TransferHandle) -> Result<usize> {
        if h.owner != self.owner
            || !h.rx
            || h.slot >= RX
            || h.generation != self.rx_gen[h.slot]
            || self.rx[h.slot].is_none()
        {
            Err(Error::State)
        } else {
            Ok(h.slot)
        }
    }

    /// Exact accepted Setup ACK before the first fragment.
    /// `首片前精确SetupACK`。
    /// # Errors
    /// Any context/Setup/credit change or expiry rejects.
    pub fn setup_ack(
        &mut self,
        h: TransferHandle,
        parent: TransferParent,
        setup: TransferSetup,
        credit: u16,
        now: u64,
    ) -> Result<()> {
        let index = self.tx_index(h)?;
        let record = self.tx[index].as_mut().ok_or(Error::State)?;
        if parent != record.parent
            || setup != record.setup
            || now == 0
            || now >= record.deadline
            || credit > 32
            || !matches!(
                record.phase,
                TransferPhase::SetupPending | TransferPhase::Sending
            )
        {
            return Err(Error::Access);
        }
        if let Some(original) = record.setup_credit {
            // Exact retry does not replenish consumed credit or restart the transfer.
            return if original == credit {
                Ok(())
            } else {
                Err(Error::Replay)
            };
        }
        record.phase = TransferPhase::Sending;
        record.credit = credit;
        record.setup_credit = Some(credit);
        Ok(())
    }

    /// Copy one eligible fragment only after Setup ACK; `SetupACK后复制可发送片`。
    /// # Errors
    /// Credit/window, deadline, index or output capacity rejects without writing.
    pub fn fragment(
        &mut self,
        h: TransferHandle,
        index: u16,
        now: u64,
        out: &mut [u8],
    ) -> Result<usize> {
        let slot = self.tx_index(h)?;
        let record = self.tx[slot].as_mut().ok_or(Error::State)?;
        let i = usize::from(index);
        if record.phase != TransferPhase::Sending
            || now == 0
            || now >= record.deadline
            || index >= record.setup.fragments
            || record.bits[i]
            || record.prepared[i]
        {
            return Err(Error::State);
        }
        let base = record.bits[..usize::from(record.setup.fragments)]
            .iter()
            .position(|value| !*value)
            .unwrap_or(usize::from(record.setup.fragments));
        if i >= base + 32 || (!record.sent[i] && record.credit == 0) {
            return Err(Error::NoSpace);
        }
        let prefix = FragmentPrefix {
            kind: record.parent.kind,
            transfer_id: record.setup.transfer_id,
            parent_generation: if record.parent.kind == ParentKind::C1 {
                record.parent.generation
            } else {
                0
            },
            index,
            count: record.setup.fragments,
        };
        let prefix_bytes = if record.parent.kind == ParentKind::C1 {
            12
        } else {
            8
        };
        let start = i * usize::from(record.setup.fragment_bytes);
        let end = (start + usize::from(record.setup.fragment_bytes))
            .min(record.setup.total_bytes as usize);
        if out.len() < prefix_bytes + end - start {
            return Err(Error::NoSpace);
        }
        crate::encode_fragment(prefix, out)?;
        out[prefix_bytes..prefix_bytes + end - start]
            .copy_from_slice(&self.tx_bytes[slot][start..end]);
        if !record.sent[i] {
            record.credit -= 1;
        }
        record.prepared[i] = true;
        Ok(prefix_bytes + end - start)
    }

    /// Merge actual Driver disposition; preparing bytes alone is not send evidence.
    /// 合并真实Driver结果，准备字节本身不构成发送证据。
    /// # Errors
    /// Stale index/duplicate disposition rejects before mutation.
    pub fn fragment_disposition(
        &mut self,
        h: TransferHandle,
        index: u16,
        disposition: crate::Submission,
    ) -> Result<()> {
        let slot = self.tx_index(h)?;
        let record = self.tx[slot].as_mut().ok_or(Error::State)?;
        let i = usize::from(index);
        if index >= record.setup.fragments || !record.prepared[i] {
            return Err(Error::State);
        }
        record.prepared[i] = false;
        match disposition {
            crate::Submission::NotSubmitted => {
                if !record.sent[i] {
                    record.credit = record.credit.saturating_add(1).min(32);
                }
            }
            crate::Submission::Submitted | crate::Submission::InDoubt => {
                record.sent[i] = true;
            }
        }
        Ok(())
    }

    /// Validate received feedback before advancing the TX window.
    /// 推进TX窗口前验证反馈。
    /// # Errors
    /// Wrong reverse Flow/C1 feedback service, invalid bits or unsent ACK rejects.
    pub fn on_sack(
        &mut self,
        h: TransferHandle,
        parent: TransferParent,
        sack: Sack,
        feedback_service: u16,
        feedback_flow: Option<[u8; 16]>,
        now: u64,
    ) -> Result<()> {
        let slot = self.tx_index(h)?;
        let record = self.tx[slot].as_mut().ok_or(Error::State)?;
        if parent != record.parent
            || sack.kind != parent.kind
            || sack.transfer_id != record.setup.transfer_id
            || now == 0
            || now >= record.deadline
            || record.phase != TransferPhase::Sending
            || sack.credit > 32
            || sack.base >= record.setup.fragments
        {
            return Err(Error::Access);
        }
        if parent.kind == ParentKind::C1 {
            if sack.parent_generation != parent.generation
                || sack.original_service != record.setup.service
                || feedback_service != self.feedback_service
                || feedback_flow.is_some()
            {
                return Err(Error::Access);
            }
        } else if sack.parent_generation != 0
            || sack.original_service != 0
            || feedback_flow != parent.feedback_flow
        {
            return Err(Error::Access);
        }
        for bit in 0..32 {
            if sack.bitmap & (1 << bit) != 0 {
                let index = usize::from(sack.base) + bit;
                if index >= usize::from(record.setup.fragments) || !record.sent[index] {
                    return Err(Error::Malformed);
                }
            }
        }
        for bit in 0..32 {
            if sack.bitmap & (1 << bit) != 0 {
                record.bits[usize::from(sack.base) + bit] = true;
            }
        }
        record.credit = sack.credit;
        Ok(())
    }

    /// Accept Setup atomically with reserved terminal receipt and fixed reassembly.
    /// `原子接纳Setup并预留终态Receipt及固定重组`。
    /// # Errors
    /// Conflict, expired duplicate or insufficient resource never evicts an old message.
    pub fn receive_setup(
        &mut self,
        parent: TransferParent,
        setup: TransferSetup,
        now: u64,
    ) -> Result<TransferHandle> {
        self.receive_setup_at(parent, setup, now, now)
    }

    /// Preserve authenticated admission time across delayed Coordinator dispatch.
    /// Coordinator延迟分派时仍保留认证接纳起点，不延长租期。
    /// # Errors
    /// Future/expired admission, conflict or insufficient capacity cannot publish RX.
    pub fn receive_setup_at(
        &mut self,
        parent: TransferParent,
        setup: TransferSetup,
        admitted_at: u64,
        now: u64,
    ) -> Result<TransferHandle> {
        Self::validate_setup(parent, setup)?;
        if admitted_at == 0 || now < admitted_at {
            return Err(Error::Argument);
        }
        let expires = deadline(admitted_at, u64::from(setup.lifetime_ms) * 1000)?;
        if now >= expires {
            return Err(Error::Timeout);
        }
        for (slot, record) in self.rx.iter().enumerate() {
            if let Some(record) = record {
                if record.parent == parent && record.setup.transfer_id == setup.transfer_id {
                    if record.setup != setup {
                        return Err(Error::Replay);
                    }
                    if now >= record.deadline && record.phase == TransferPhase::Receiving {
                        return Err(Error::Timeout);
                    }
                    return Ok(TransferHandle {
                        owner: self.owner,
                        slot,
                        generation: self.rx_gen[slot],
                        rx: true,
                    });
                }
            }
        }
        if self.receipts.iter().flatten().any(|receipt| {
            receipt.parent == parent && receipt.setup.transfer_id == setup.transfer_id
        }) {
            return Err(Error::Replay);
        }
        let slot = self
            .rx
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let receipt = self
            .receipts
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.rx_gen[slot].checked_add(1).ok_or(Error::Exhausted)?;
        let receipt_expires = deadline(expires, self.retention)?;
        self.rx_bytes[slot].fill(0);
        self.rx_evidence[slot].fill(None);
        self.rx[slot] = Some(Record {
            parent,
            setup,
            bits: [false; F],
            sent: [false; F],
            prepared: [false; F],
            phase: TransferPhase::Receiving,
            deadline: expires,
            receipt,
            credit: 32,
            setup_credit: None,
            terminal: None,
            wire_terminal: None,
            wire_abort: None,
        });
        self.rx_gen[slot] = generation;
        self.receipts[receipt] = Some(Receipt {
            parent,
            setup,
            expires: receipt_expires,
            terminal: None,
        });
        Ok(TransferHandle {
            owner: self.owner,
            slot,
            generation,
            rx: true,
        })
    }

    /// Authenticated fragment admission with complete immutable evidence.
    /// 含完整不可变证据的认证分片准入。
    /// # Errors
    /// Wrong parent/role/service/count/sequence/length, conflicting duplicate or digest rejects.
    #[allow(clippy::too_many_arguments)] // Exact authenticated fragment facts stay independently explicit.
    pub fn receive_fragment(
        &mut self,
        h: TransferHandle,
        parent: TransferParent,
        prefix: FragmentPrefix,
        interaction: InteractionRole,
        service: u16,
        evidence: FragmentEvidence,
        payload: &[u8],
        now: u64,
        scratch: &mut DigestWorkspace,
    ) -> Result<bool> {
        Ok(self
            .prepare_fragment(
                h,
                parent,
                prefix,
                interaction,
                service,
                evidence,
                payload,
                now,
                scratch,
            )?
            .commit())
    }

    /// Validate a fragment and virtual complete digest with zero RX writes.
    /// 校验分片与虚拟完整消息摘要，预检过程零RX写入。
    /// # Errors
    /// Wrong identity/length/evidence/digest/deadline leaves bytes, bitmap and phase unchanged.
    #[allow(clippy::too_many_arguments)]
    pub fn prepare_fragment<'a, 'p>(
        &'a mut self,
        h: TransferHandle,
        parent: TransferParent,
        prefix: FragmentPrefix,
        interaction: InteractionRole,
        service: u16,
        evidence: FragmentEvidence,
        payload: &'p [u8],
        now: u64,
        scratch: &mut DigestWorkspace,
    ) -> Result<FragmentAdmission<'a, 'p, F>> {
        let slot = self.rx_index(h)?;
        let record = self.rx[slot].as_ref().ok_or(Error::State)?;
        let i = usize::from(prefix.index);
        if !matches!(
            record.phase,
            TransferPhase::Receiving
                | TransferPhase::Complete
                | TransferPhase::AppHeld
                | TransferPhase::Delivered
        ) {
            return Err(Error::State);
        }
        if parent != record.parent
            || prefix.kind != parent.kind
            || prefix.transfer_id != record.setup.transfer_id
            || prefix.count != record.setup.fragments
            || prefix.index >= prefix.count
            || service != record.setup.service
            || interaction != record.setup.interaction
            || evidence.sequence == 0
            || evidence.aad == [0; 16]
            || evidence.payload == [0; 16]
            || (parent.kind == ParentKind::C1 && prefix.parent_generation != parent.generation)
            || (parent.kind == ParentKind::Flow && prefix.parent_generation != 0)
            || now == 0
            || now
                < record
                    .deadline
                    .checked_sub(u64::from(record.setup.lifetime_ms) * 1000)
                    .ok_or(Error::State)?
        {
            return Err(Error::Access);
        }
        let start = i * usize::from(record.setup.fragment_bytes);
        let end = (start + usize::from(record.setup.fragment_bytes))
            .min(record.setup.total_bytes as usize);
        if payload.len() != end - start {
            return Err(Error::Malformed);
        }
        if record.bits[i] {
            if now >= self.receipts[record.receipt].ok_or(Error::State)?.expires
                || self.rx_evidence[slot][i] != Some(evidence)
                || self.rx_bytes[slot][start..end] != *payload
            {
                return Err(Error::Replay);
            }
            return Ok(FragmentAdmission {
                record: self.rx[slot].as_mut().ok_or(Error::State)?,
                bytes: &mut self.rx_bytes[slot][start..end],
                saved_evidence: &mut self.rx_evidence[slot][i],
                handle: h,
                index: prefix.index,
                evidence,
                payload,
                duplicate: true,
                complete: false,
            });
        }
        if record.phase != TransferPhase::Receiving || now >= record.deadline {
            return Err(Error::Timeout);
        }
        if self.rx_evidence[slot]
            .iter()
            .flatten()
            .any(|prior| prior.sequence == evidence.sequence)
        {
            return Err(Error::Replay);
        }
        let complete = record.bits[..usize::from(prefix.count)]
            .iter()
            .filter(|value| **value)
            .count()
            + 1
            == usize::from(prefix.count);
        if complete {
            let mut digest = canonical_begin(
                b"UCN6-TRANSFER-MESSAGE-V1",
                record.setup.total_bytes,
                scratch,
            )?;
            digest.append(&self.rx_bytes[slot][..start])?;
            digest.append(payload)?;
            digest.append(&self.rx_bytes[slot][end..record.setup.total_bytes as usize])?;
            if digest.finish()? != record.setup.digest {
                return Err(Error::Malformed);
            }
        }
        Ok(FragmentAdmission {
            record: self.rx[slot].as_mut().ok_or(Error::State)?,
            bytes: &mut self.rx_bytes[slot][start..end],
            saved_evidence: &mut self.rx_evidence[slot][i],
            handle: h,
            index: prefix.index,
            evidence,
            payload,
            duplicate: false,
            complete,
        })
    }

    /// Resolve authenticated replay without accepting caller-supplied plaintext.
    /// 解析认证重放，不接受调用方额外提供的明文。
    /// # Errors
    /// Absent/conflicting evidence, wrong domain/service/role or expired receipt rejects.
    pub fn replay_fragment(
        &self,
        parent: TransferParent,
        evidence: FragmentEvidence,
        interaction: InteractionRole,
        service: u16,
        now: u64,
    ) -> Result<(TransferHandle, u16)> {
        parent.validate()?;
        if now == 0 {
            return Err(Error::Argument);
        }
        for (slot, record) in self.rx.iter().enumerate() {
            let Some(record) = record else {
                continue;
            };
            if record.parent != parent
                || record.setup.service != service
                || record.setup.interaction != interaction
                || !matches!(
                    record.phase,
                    TransferPhase::Receiving
                        | TransferPhase::Complete
                        | TransferPhase::AppHeld
                        | TransferPhase::Delivered
                )
                || now >= self.receipts[record.receipt].ok_or(Error::State)?.expires
                || now
                    < record
                        .deadline
                        .checked_sub(u64::from(record.setup.lifetime_ms) * 1000)
                        .ok_or(Error::State)?
            {
                continue;
            }
            if let Some(index) = self.rx_evidence[slot]
                .iter()
                .position(|old| *old == Some(evidence))
            {
                return Ok((
                    TransferHandle {
                        owner: self.owner,
                        slot,
                        generation: self.rx_gen[slot],
                        rx: true,
                    },
                    u16::try_from(index).map_err(|_| Error::Config)?,
                ));
            }
        }
        Err(Error::Replay)
    }

    /// Fixed SACK for safely committed bytes; 已安全写入片的SACK。
    /// # Errors
    /// Wrong window/handle rejects.
    pub fn sack(&self, h: TransferHandle, base: u16) -> Result<Sack> {
        let record = self.rx[self.rx_index(h)?].as_ref().ok_or(Error::State)?;
        if base >= record.setup.fragments {
            return Err(Error::Argument);
        }
        let mut bits = 0;
        for bit in 0..32 {
            let index = usize::from(base) + bit;
            if index < usize::from(record.setup.fragments) && record.bits[index] {
                bits |= 1 << bit;
            }
        }
        Ok(Sack {
            kind: record.parent.kind,
            transfer_id: record.setup.transfer_id,
            parent_generation: if record.parent.kind == ParentKind::C1 {
                record.parent.generation
            } else {
                0
            },
            original_service: if record.parent.kind == ParentKind::C1 {
                record.setup.service
            } else {
                0
            },
            base,
            bitmap: bits,
            credit: 32,
        })
    }

    /// Copy then mark application-delivered; 复制后标记应用交付。
    /// # Errors
    /// Incomplete/wrong/capacity failure preserves bytes and phase.
    pub fn copy_complete(&mut self, h: TransferHandle, out: &mut [u8]) -> Result<usize> {
        let slot = self.rx_index(h)?;
        let record = self.rx[slot].as_mut().ok_or(Error::State)?;
        if record.phase != TransferPhase::Complete {
            return Err(Error::State);
        }
        let size = record.setup.total_bytes as usize;
        if out.len() < size {
            return Err(Error::NoSpace);
        }
        if self.receipts[record.receipt].is_none() {
            return Err(Error::State);
        }
        out[..size].copy_from_slice(&self.rx_bytes[slot][..size]);
        record.phase = TransferPhase::Delivered;
        self.receipts[record.receipt]
            .as_mut()
            .ok_or(Error::State)?
            .terminal = Some(true);
        Ok(size)
    }
    /// Explicitly discard unclaimed complete data, retaining a negative receipt until expiry.
    /// 显式丢弃尚未领取的完整数据，负向凭据保留至过期；不能释放应用持有的Buffer。
    /// # Errors
    /// Wrong/non-complete/held receiver leaves state and payload unchanged.
    pub fn discard_complete(&mut self, h: TransferHandle) -> Result<()> {
        let slot = self.rx_index(h)?;
        let record = self.rx[slot].as_mut().ok_or(Error::State)?;
        if record.phase != TransferPhase::Complete {
            return Err(Error::State);
        }
        let receipt = self.receipts[record.receipt].as_mut().ok_or(Error::State)?;
        self.rx_bytes[slot].fill(0);
        record.phase = TransferPhase::Aborted;
        receipt.terminal = Some(false);
        Ok(())
    }
    /// Claim validated complete bytes without copying; 无复制领取完整字节。
    /// # Errors
    /// Non-complete or repeated claim rejects.
    pub fn claim(&mut self, h: TransferHandle) -> Result<PayloadClaim> {
        let slot = self.rx_index(h)?;
        let record = self.rx[slot].as_mut().ok_or(Error::State)?;
        if record.phase != TransferPhase::Complete {
            return Err(Error::State);
        }
        record.phase = TransferPhase::AppHeld;
        Ok(PayloadClaim { handle: h })
    }
    /// Borrow only under an exact active claim; 精确活动Claim下借用。
    /// # Errors
    /// Stale/released claim rejects.
    pub fn claimed_bytes(&self, claim: &PayloadClaim) -> Result<&[u8]> {
        let slot = self.rx_index(claim.handle)?;
        let record = self.rx[slot].as_ref().ok_or(Error::State)?;
        if record.phase != TransferPhase::AppHeld {
            return Err(Error::State);
        }
        Ok(&self.rx_bytes[slot][..record.setup.total_bytes as usize])
    }
    /// Explicit application release; 应用显式释放。
    /// # Errors
    /// Wrong/retired claim cannot release a newer buffer.
    #[allow(clippy::needless_pass_by_value)] // Explicit release consumes the unique application claim.
    pub fn release(&mut self, claim: PayloadClaim) -> Result<()> {
        let slot = self.rx_index(claim.handle)?;
        let record = self.rx[slot].as_mut().ok_or(Error::State)?;
        if record.phase != TransferPhase::AppHeld {
            return Err(Error::State);
        }
        record.phase = TransferPhase::Delivered;
        self.receipts[record.receipt]
            .as_mut()
            .ok_or(Error::State)?
            .terminal = Some(true);
        Ok(())
    }

    /// Exact final-target receipt; 最终目标精确Receipt。
    /// # Errors
    /// SACK alone, wrong Setup/parent, unsent fragment or timeout cannot complete TX.
    pub fn terminal(
        &mut self,
        h: TransferHandle,
        parent: TransferParent,
        setup: TransferSetup,
        accepted: bool,
        now: u64,
    ) -> Result<()> {
        let slot = self.tx_index(h)?;
        let record = self.tx[slot].as_mut().ok_or(Error::State)?;
        if let Some(original) = record.terminal {
            if parent != record.parent || setup != record.setup || accepted != original {
                return Err(Error::Replay);
            }
            return Ok(());
        }
        if parent != record.parent
            || setup != record.setup
            || now == 0
            || now >= record.deadline
            || !matches!(record.phase, TransferPhase::Sending | TransferPhase::Done)
            || record.sent[..usize::from(setup.fragments)]
                .iter()
                .any(|sent| !*sent)
        {
            return Err(Error::Access);
        }
        record.phase = if accepted {
            TransferPhase::Done
        } else {
            TransferPhase::Failed
        };
        record.terminal = Some(accepted);
        Ok(())
    }

    /// Abort only uncompleted transfer; 仅取消尚未完成的Transfer。
    /// # Errors
    /// `APP_HELD/complete/delivered` state is never reclaimed by abort.
    pub fn abort(&mut self, h: TransferHandle) -> Result<()> {
        if h.rx {
            let slot = self.rx_index(h)?;
            let record = self.rx[slot].as_mut().ok_or(Error::State)?;
            if record.phase != TransferPhase::Receiving {
                return Err(Error::State);
            }
            record.phase = TransferPhase::Aborted;
            self.receipts[record.receipt]
                .as_mut()
                .ok_or(Error::State)?
                .terminal = Some(false);
        } else {
            let slot = self.tx_index(h)?;
            let record = self.tx[slot].as_mut().ok_or(Error::State)?;
            if !matches!(
                record.phase,
                TransferPhase::SetupPending | TransferPhase::Sending
            ) {
                return Err(Error::State);
            }
            record.phase = TransferPhase::Aborted;
        }
        Ok(())
    }
    /// Retire TX independently; TX独立退休。
    /// # Errors
    /// Active TX rejects.
    pub fn retire_tx(&mut self, h: TransferHandle) -> Result<()> {
        let slot = self.tx_index(h)?;
        if !matches!(
            self.tx[slot].as_ref().ok_or(Error::State)?.phase,
            TransferPhase::Done | TransferPhase::Aborted | TransferPhase::Failed
        ) || self.tx[slot]
            .as_ref()
            .ok_or(Error::State)?
            .prepared
            .iter()
            .any(|value| *value)
        {
            return Err(Error::State);
        }
        self.tx[slot] = None;
        Ok(())
    }
    /// RX evidence retires only after application release and both retention windows.
    /// 应用释放且保留窗口结束后才退休RX证据。
    /// # Errors
    /// Active/held/unexpired buffers stay owned.
    pub fn retire_rx(&mut self, h: TransferHandle, now: u64) -> Result<()> {
        let slot = self.rx_index(h)?;
        let record = self.rx[slot].as_ref().ok_or(Error::State)?;
        let receipt = self.receipts[record.receipt].ok_or(Error::State)?;
        if !matches!(
            record.phase,
            TransferPhase::Delivered | TransferPhase::Aborted | TransferPhase::Failed
        ) || now < receipt.expires
        {
            return Err(Error::State);
        }
        self.receipts[record.receipt] = None;
        self.rx[slot] = None;
        Ok(())
    }
    /// Read frozen phase; 读取冻结状态。
    /// # Errors
    /// Wrong handle rejects.
    pub fn view(&self, h: TransferHandle) -> Result<TransferView> {
        let record = if h.rx {
            self.rx[self.rx_index(h)?].as_ref()
        } else {
            self.tx[self.tx_index(h)?].as_ref()
        }
        .ok_or(Error::State)?;
        Ok(TransferView {
            setup: record.setup,
            phase: record.phase,
            deadline_us: record.deadline,
            completed_fragments: record.bits.iter().filter(|bit| **bit).count(),
        })
    }

    /// Find TX by the complete installed Parent and ID, never by digest alone.
    /// 按完整已安装父域及ID查找TX，不仅凭摘要匹配。
    /// # Errors
    /// Missing or different Parent returns `NotFound` without mutation.
    pub fn find_tx(&self, parent: TransferParent, id: u32) -> Result<TransferHandle> {
        parent.validate()?;
        self.tx
            .iter()
            .enumerate()
            .find_map(|(slot, record)| {
                record
                    .as_ref()
                    .filter(|r| r.parent == parent && r.setup.transfer_id == id)
                    .map(|_| TransferHandle {
                        owner: self.owner,
                        slot,
                        generation: self.tx_gen[slot],
                        rx: false,
                    })
            })
            .ok_or(Error::NotFound)
    }

    /// Find RX by the complete installed Parent and ID; 精确父域与ID查找RX。
    /// # Errors
    /// Missing or different Parent returns `NotFound` without mutation.
    pub fn find_rx(&self, parent: TransferParent, id: u32) -> Result<TransferHandle> {
        parent.validate()?;
        self.rx
            .iter()
            .enumerate()
            .find_map(|(slot, record)| {
                record
                    .as_ref()
                    .filter(|r| r.parent == parent && r.setup.transfer_id == id)
                    .map(|_| TransferHandle {
                        owner: self.owner,
                        slot,
                        generation: self.rx_gen[slot],
                        rx: true,
                    })
            })
            .ok_or(Error::NotFound)
    }

    /// Apply exact canonical feedback to one TX. Terminal state/code stay immutable.
    /// 对唯一TX应用规范反馈，终态及结果码保持不可变。
    /// # Errors
    /// Wrong Setup/Parent, changed terminal/abort, expired or unsent feedback rejects.
    pub fn control_feedback(
        &mut self,
        parent: TransferParent,
        feedback: TransferFeedback,
        now: u64,
        scratch: &mut DigestWorkspace,
    ) -> Result<TransferHandle> {
        let id = match feedback {
            TransferFeedback::SetupAck(v) => v.key.transfer_id,
            TransferFeedback::Abort(v) => v.key.transfer_id,
            TransferFeedback::Terminal(v) => v.key.transfer_id,
            TransferFeedback::Sack(v) => v.transfer_id,
        };
        let h = self.find_tx(parent, id)?;
        let slot = self.tx_index(h)?;
        let setup = self.tx[slot].as_ref().ok_or(Error::State)?.setup;
        let expected = crate::TransferKey::from_setup(setup, scratch)?;
        match feedback {
            TransferFeedback::SetupAck(v) => {
                if v.key != expected || v.fragment_bytes != setup.fragment_bytes {
                    return Err(Error::Access);
                }
                self.setup_ack(h, parent, setup, v.credit, now)?;
            }
            TransferFeedback::Sack(v) => {
                self.on_sack(
                    h,
                    parent,
                    v,
                    self.feedback_service,
                    parent.feedback_flow,
                    now,
                )?;
            }
            TransferFeedback::Abort(v) => {
                if v.key != expected || v.reason == 0 || now == 0 {
                    return Err(Error::Access);
                }
                if let Some(old) = self.tx[slot].as_ref().ok_or(Error::State)?.wire_abort {
                    return if old == v.reason {
                        Ok(h)
                    } else {
                        Err(Error::Replay)
                    };
                }
                if now >= self.view(h)?.deadline_us {
                    return Err(Error::Timeout);
                }
                self.abort(h)?;
                self.tx[slot].as_mut().ok_or(Error::State)?.wire_abort = Some(v.reason);
            }
            TransferFeedback::Terminal(v) => {
                if v.key != expected || !(1..=3).contains(&v.state) || now == 0 {
                    return Err(Error::Access);
                }
                if let Some(old) = self.tx[slot].as_ref().ok_or(Error::State)?.wire_terminal {
                    return if old == (v.state, v.result) {
                        Ok(h)
                    } else {
                        Err(Error::Replay)
                    };
                }
                self.terminal(h, parent, setup, v.state == 1, now)?;
                self.tx[slot].as_mut().ok_or(Error::State)?.wire_terminal =
                    Some((v.state, v.result));
            }
        }
        Ok(h)
    }
    /// Explicit bounded timeout; 显式有界超时。
    /// # Errors
    /// Unknown time/zero budget rejects; complete/held data remains intact.
    pub fn step(&mut self, now: u64, budget: usize) -> Result<usize> {
        if now == 0 || budget == 0 {
            return Err(Error::Argument);
        }
        let count = budget.min(TX + RX);
        for _ in 0..count {
            let slot = self.cursor;
            self.cursor = (self.cursor + 1) % (TX + RX);
            let record = if slot < TX {
                self.tx[slot].as_mut()
            } else {
                self.rx[slot - TX].as_mut()
            };
            if let Some(record) = record {
                if now >= record.deadline
                    && matches!(
                        record.phase,
                        TransferPhase::SetupPending
                            | TransferPhase::Sending
                            | TransferPhase::Receiving
                    )
                {
                    record.phase = TransferPhase::Failed;
                    if slot >= TX {
                        self.receipts[record.receipt]
                            .as_mut()
                            .ok_or(Error::State)?
                            .terminal = Some(false);
                    }
                }
            }
        }
        Ok(count)
    }
}

/// Coordinator-verified feedback, not an authentication claim.
/// Coordinator已验证的反馈，本类型本身不构成认证证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TransferFeedback {
    /// Setup acceptance; 建立确认。
    SetupAck(crate::SetupAck),
    /// Selective fragment acknowledgement; 分片选择确认。
    Sack(crate::Sack),
    /// Exact remote cancellation; 精确远端取消。
    Abort(crate::TransferAbort),
    /// Final target receipt; 最终目标凭据。
    Terminal(crate::TransferTerminal),
}

/// Nano fixed Transfer profile; Nano固定档。
pub type NanoTransferOwner = TransferOwner<1, 1, 512, 16, 1>;
/// Lite fixed Transfer profile; Lite固定档。
pub type LiteTransferOwner = TransferOwner<2, 2, 2048, 64, 2>;
/// Full fixed Transfer profile; Full固定档。
pub type FullTransferOwner = TransferOwner<4, 4, 4096, 128, 4>;
