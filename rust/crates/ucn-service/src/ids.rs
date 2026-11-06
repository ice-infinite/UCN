use ucn_durable::{DurableBase, DurableContract, DurableUpdate};
use ucn_persistence::{
    CodecWorkspace, DomainKind, PersistenceHandle, PersistenceProof, PersistenceRequest,
};
use ucn_types::{Error, Result};

/// Exclusive reservation from a confirmed interval; drop never allocates an ID.
/// 已确认区间的独占预留，丢弃不会分配ID。
#[must_use]
pub struct OperationIdReservation<'a> {
    owner: &'a mut OperationIds,
    id: u64,
}
impl OperationIdReservation<'_> {
    /// Exact next ID, not yet published; 精确下一ID，尚未发布。
    #[must_use]
    pub const fn id(&self) -> u64 {
        self.id
    }
    /// Publish once after all caller preflight; 预检后发布一次。
    #[must_use]
    pub fn commit(self) -> u64 {
        if self.id == self.owner.through {
            self.owner.exhausted = true;
        } else {
            self.owner.next += 1;
        }
        self.id
    }
}

/// Persist-before-use Operation-ID intervals; Operation-ID区间先持久化后使用。
pub struct OperationIds {
    contract: DurableContract,
    durable_generation: u64,
    durable_digest: [u8; 16],
    parent: u32,
    through: u64,
    next: u64,
    pending: Option<(DurableUpdate<20>, u64)>,
    exhausted: bool,
    fenced: bool,
}
impl OperationIds {
    /// Bind an independent ID high-water domain; 绑定独立ID高水位域。
    /// # Errors
    /// Invalid parent or non-Operation Journal domain rejects.
    pub fn new(contract: DurableContract, parent: u32) -> Result<Self> {
        if contract.domain.kind != DomainKind::OperationJournal
            || contract.operation != 0x0702
            || parent == 0
        {
            return Err(Error::Config);
        }
        Ok(Self {
            contract,
            durable_generation: 0,
            durable_digest: [0; 16],
            parent,
            through: 0,
            next: 1,
            pending: None,
            exhausted: false,
            fenced: false,
        })
    }
    /// Restore the whole reserved range as burned; 恢复时烧掉全部已预留区间。
    /// # Errors
    /// Bad OID1, parent or baseline rejects.
    pub fn restore(
        &mut self,
        base: DurableBase,
        body: &[u8],
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if self.fenced
            || base.contract() != self.contract
            || self.through != 0
            || self.pending.is_some()
        {
            return Err(Error::State);
        }
        base.verify_body(body, scratch)?;
        if base.generation() == 0 {
            if !body.is_empty() {
                return Err(Error::Malformed);
            }
            return Ok(());
        }
        if body.len() != 20
            || &body[..8] != b"OID1\0\x01\0\0"
            || u32::from_be_bytes(body[8..12].try_into().map_err(|_| Error::Malformed)?)
                != self.parent
        {
            return Err(Error::Malformed);
        }
        let through = u64::from_be_bytes(body[12..20].try_into().map_err(|_| Error::Malformed)?);
        if through == 0 {
            return Err(Error::Malformed);
        }
        self.through = through;
        self.durable_generation = base.generation();
        self.durable_digest = base.record_digest();
        self.next = through;
        self.exhausted = true;
        Ok(())
    }
    /// Reserve a durable interval before allocating any ID; 分配前预留耐久区间。
    /// # Errors
    /// Unspent range/pending/overflow rejects atomically.
    #[allow(clippy::too_many_arguments)]
    pub fn reserve(
        &mut self,
        base: DurableBase,
        count: u64,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if self.fenced
            || base.contract() != self.contract
            || base.generation() != self.durable_generation
            || base.record_digest() != self.durable_digest
            || count == 0
            || self.pending.is_some()
            || (!self.exhausted && self.next <= self.through)
        {
            return Err(Error::State);
        }
        let through = self.through.checked_add(count).ok_or(Error::Exhausted)?;
        let mut body = [0; 20];
        body[..8].copy_from_slice(b"OID1\0\x01\0\0");
        body[8..12].copy_from_slice(&self.parent.to_be_bytes());
        body[12..].copy_from_slice(&through.to_be_bytes());
        let update = DurableUpdate::prepare(base, &body, now, until, continuation, scratch)?;
        self.pending = Some((update, through));
        Ok(())
    }
    /// Coordinator requirement; Coordinator要求。
    /// # Errors
    /// No pending interval rejects.
    pub fn requirement(&self) -> Result<PersistenceRequest<'_>> {
        Ok(self.pending.as_ref().ok_or(Error::State)?.0.request())
    }
    /// Bind Foundation handle; `绑定FoundationHandle`。
    /// # Errors
    /// Wrong pending state rejects.
    pub fn bind(&mut self, h: PersistenceHandle) -> Result<()> {
        self.pending.as_mut().ok_or(Error::State)?.0.bind(h)
    }
    /// Publish interval only after reload proof; 回读后发布区间。
    /// # Errors
    /// Wrong proof cannot enable allocation.
    pub fn confirm(&mut self, h: PersistenceHandle, proof: PersistenceProof) -> Result<()> {
        self.pending
            .as_mut()
            .ok_or(Error::State)?
            .0
            .confirm(h, proof)?;
        let (_, through) = self.pending.take().ok_or(Error::State)?;
        self.durable_generation = proof.record_generation();
        self.durable_digest = proof.body_digest();
        self.next = self.through.checked_add(1).ok_or(Error::Exhausted)?;
        self.through = through;
        self.exhausted = false;
        Ok(())
    }
    /// Allocate one non-reusable checked-next ID; 分配一次不可复用ID。
    /// # Errors
    /// Before durability or after interval exhaustion rejects.
    pub fn allocate(&mut self) -> Result<u64> {
        Ok(self.reserve_id()?.commit())
    }
    /// Linear preflight without consuming the confirmed interval; 不消耗已确认区间的线性预检。
    /// # Errors
    /// Unconfirmed or exhausted range rejects.
    pub fn reserve_id(&mut self) -> Result<OperationIdReservation<'_>> {
        if self.fenced || self.pending.is_some() || self.exhausted || self.next > self.through {
            return Err(Error::Exhausted);
        }
        let id = self.next;
        Ok(OperationIdReservation { owner: self, id })
    }
    /// Exact confirmed allocator binding; 已确认分配器的精确绑定。
    #[must_use]
    pub const fn contract(&self) -> DurableContract {
        self.contract
    }
    /// Confirmed media generation and digest; 确认媒体代际与摘要。
    #[must_use]
    pub const fn record_binding(&self) -> (u64, [u8; 16]) {
        (self.durable_generation, self.durable_digest)
    }
    /// Pending interval, never permission to allocate; 待确认区间，不授予分配权限。
    #[must_use]
    pub const fn is_pending(&self) -> bool {
        self.pending.is_some()
    }
    /// Local allocator failure fence; 分配器本地失败围栏。
    #[must_use]
    pub const fn is_fenced(&self) -> bool {
        self.fenced
    }
    /// Irreversible local failure fence, not retirement of a Provider obligation.
    /// 不可逆本地失败围栏，不代表Provider义务已退休。
    pub fn fence(&mut self) {
        self.fenced = true;
        self.exhausted = true;
        self.pending = None;
    }
}
