use crate::{Binding, ParentKind, SecurityDomain};
use ucn_durable::{DurableBase, DurableContract, DurableUpdate};
use ucn_persistence::{
    CodecWorkspace, DomainKind, PersistenceHandle, PersistenceProof, PersistenceRequest,
};
use ucn_types::{Error, OriginSecurity, Result};

/// Frozen independent C1 or Flow Parent; `独立C1或Flow父域`。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferParent {
    /// Parent kind; 类型。
    pub kind: ParentKind,
    /// C1=0, Flow nonzero; 父短ID。
    pub id: u16,
    /// Parent generation; 父代际。
    pub generation: u32,
    /// Realm; Realm域。
    pub realm: u32,
    /// Source identity; 原始来源。
    pub source: Binding,
    /// Final destination; 最终目标。
    pub destination: Binding,
    /// Current security domain; 安全域。
    pub security: SecurityDomain,
    /// Paired reverse feedback Flow fingerprint; 成对反向反馈Flow指纹。
    pub feedback_flow: Option<[u8; 16]>,
}
impl TransferParent {
    /// Validate exact parent including feedback obligations; 验证父域和反馈义务。
    /// # Errors
    /// Missing identity, generation or Flow feedback fails closed.
    pub fn validate(self) -> Result<()> {
        self.source.validate()?;
        self.destination.validate()?;
        self.security.validate()?;
        if self.generation == 0
            || self.realm == 0
            || self.source == self.destination
            || (self.kind == ParentKind::C1 && (self.id != 0 || self.feedback_flow.is_some()))
            || (self.kind == ParentKind::Flow
                && (self.id == 0 || self.feedback_flow.is_none_or(|value| value == [0; 16])))
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// One linear, non-forgeable allocated ID; 一次线性、不可伪造分配ID。
pub struct TransferPermit {
    pub(crate) parent: TransferParent,
    pub(crate) transfer_id: u32,
}
impl TransferPermit {
    /// Allocated ID; 已分配ID。
    #[must_use]
    pub const fn id(&self) -> u32 {
        self.transfer_id
    }
    /// Frozen Parent; 冻结父域。
    #[must_use]
    pub const fn parent(&self) -> TransferParent {
        self.parent
    }
}

/// Fixed C1 Parent record bytes; `C1父域Record固定长度`。
pub const PARENT_RECORD_BYTES: usize = 88;
struct ParentPending {
    update: DurableUpdate<PARENT_RECORD_BYTES>,
    parent: TransferParent,
    high_water: u32,
    allocate: bool,
}

/// Sole C1 Transfer-ID allocator; 唯一C1 Transfer-ID分配者。
pub struct ParentOwner {
    contract: DurableContract,
    durable_generation: u64,
    durable_digest: [u8; 16],
    parent: Option<TransferParent>,
    high_water: u32,
    pending: Option<ParentPending>,
    permit: Option<TransferPermit>,
}

impl ParentOwner {
    /// Bind one durable high-water domain; 绑定一个耐久高水位域。
    /// # Errors
    /// Wrong domain/schema rejects.
    pub fn new(contract: DurableContract) -> Result<Self> {
        if contract.domain.kind != DomainKind::TransportHighWater
            || contract.schema != 0x5401
            || contract.version != 1
            || contract.operation != 0x0701
        {
            return Err(Error::Config);
        }
        Ok(Self {
            contract,
            durable_generation: 0,
            durable_digest: [0; 16],
            parent: None,
            high_water: 0,
            pending: None,
            permit: None,
        })
    }

    /// Import only a verified Ready record; 仅导入已验证Ready正文。
    /// # Errors
    /// Wrong record/key/high-water cannot publish a parent.
    pub fn restore(
        &mut self,
        base: DurableBase,
        body: &[u8],
        current_security: SecurityDomain,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if base.contract() != self.contract || self.pending.is_some() || self.parent.is_some() {
            return Err(Error::State);
        }
        base.verify_body(body, scratch)?;
        if base.generation() == 0 {
            if !body.is_empty() {
                return Err(Error::Malformed);
            }
            return Ok(());
        }
        let (parent, high_water, transaction) = decode_parent_record(body, current_security)?;
        if transaction != base.transaction() {
            return Err(Error::Malformed);
        }
        self.parent = Some(parent);
        self.durable_generation = base.generation();
        self.durable_digest = base.record_digest();
        self.high_water = high_water;
        Ok(())
    }

    /// Persist a new checked-next Parent before using it; 新父代际先持久化。
    /// # Errors
    /// Existing pending/permit, wrong generation or deadline reject.
    pub fn prepare_parent(
        &mut self,
        base: DurableBase,
        parent: TransferParent,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        parent.validate()?;
        if base.contract() != self.contract
            || base.generation() != self.durable_generation
            || base.record_digest() != self.durable_digest
            || parent.kind != ParentKind::C1
            || self.pending.is_some()
            || self.permit.is_some()
            || parent.generation
                != self.parent.map_or(Ok(1), |old| {
                    old.generation.checked_add(1).ok_or(Error::Exhausted)
                })?
        {
            return Err(Error::State);
        }
        let body = encode_parent_record(
            parent,
            0,
            base.transaction().checked_add(1).ok_or(Error::Exhausted)?,
        )?;
        let update = DurableUpdate::prepare(base, &body, now, until, continuation, scratch)?;
        self.pending = Some(ParentPending {
            update,
            parent,
            high_water: 0,
            allocate: false,
        });
        Ok(())
    }

    /// Persist one new Transfer-ID high-water; `持久化下一TransferID`。
    /// # Errors
    /// No published Parent, pending update or exhausted high-water rejects.
    pub fn allocate(
        &mut self,
        base: DurableBase,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if base.contract() != self.contract
            || base.generation() != self.durable_generation
            || base.record_digest() != self.durable_digest
            || self.pending.is_some()
            || self.permit.is_some()
        {
            return Err(Error::State);
        }
        let parent = self.parent.ok_or(Error::State)?;
        let high_water = self.high_water.checked_add(1).ok_or(Error::Exhausted)?;
        let body = encode_parent_record(
            parent,
            high_water,
            base.transaction().checked_add(1).ok_or(Error::Exhausted)?,
        )?;
        let update = DurableUpdate::prepare(base, &body, now, until, continuation, scratch)?;
        self.pending = Some(ParentPending {
            update,
            parent,
            high_water,
            allocate: true,
        });
        Ok(())
    }

    /// Read the Coordinator persistence requirement; 读取Coordinator持久化要求。
    /// # Errors
    /// No transition returns State.
    pub fn requirement(&self) -> Result<PersistenceRequest<'_>> {
        Ok(self.pending.as_ref().ok_or(Error::State)?.update.request())
    }
    /// Bind accepted Foundation handle; `绑定Foundation接纳Handle`。
    /// # Errors
    /// Wrong phase or second different handle rejects.
    pub fn bind(&mut self, h: PersistenceHandle) -> Result<()> {
        self.pending.as_mut().ok_or(Error::State)?.update.bind(h)
    }
    /// Publish only after exact reload proof; 精确回读后发布。
    /// # Errors
    /// Wrong proof leaves pending and allocator unchanged.
    pub fn confirm(&mut self, h: PersistenceHandle, proof: PersistenceProof) -> Result<()> {
        self.pending
            .as_mut()
            .ok_or(Error::State)?
            .update
            .confirm(h, proof)?;
        let pending = self.pending.take().ok_or(Error::State)?;
        self.durable_generation = proof.record_generation();
        self.durable_digest = proof.body_digest();
        self.parent = Some(pending.parent);
        self.high_water = pending.high_water;
        if pending.allocate {
            self.permit = Some(TransferPermit {
                parent: pending.parent,
                transfer_id: pending.high_water,
            });
        }
        Ok(())
    }
    /// Take the single-use published ID; 领取一次性已发布ID。
    /// # Errors
    /// Proof not confirmed or already taken rejects.
    pub fn take_permit(&mut self) -> Result<TransferPermit> {
        self.permit.take().ok_or(Error::State)
    }
    /// Current Parent and durable high-water; 当前父域和耐久高水位。
    #[must_use]
    pub const fn view(&self) -> (Option<TransferParent>, u32) {
        (self.parent, self.high_water)
    }

    /// Exact current confirmed record identity; 当前确认的完整Record身份。
    #[must_use]
    pub const fn record_binding(&self) -> (DurableContract, u64, [u8; 16]) {
        (self.contract, self.durable_generation, self.durable_digest)
    }
    /// Pending local durable update; 本地耐久更新尚未确认。
    #[must_use]
    pub const fn is_pending(&self) -> bool {
        self.pending.is_some()
    }
}

/// Independent volatile Flow-ID allocator; `独立易失Flow传输ID分配器`。
pub struct FlowTransferAllocator {
    parent: TransferParent,
    high_water: u32,
}
impl FlowTransferAllocator {
    /// Initialize a fresh Flow with proven feedback binding.
    /// 初始化全新Flow和成对反馈绑定。
    /// # Errors
    /// Non-Flow or incomplete parent rejects.
    pub fn new(parent: TransferParent) -> Result<Self> {
        parent.validate()?;
        if parent.kind != ParentKind::Flow {
            return Err(Error::Argument);
        }
        Ok(Self {
            parent,
            high_water: 0,
        })
    }
    /// Checked-next ID; ID checked-next。
    /// # Errors
    /// Exhaustion never wraps.
    pub fn allocate(&mut self) -> Result<TransferPermit> {
        let next = self.high_water.checked_add(1).ok_or(Error::Exhausted)?;
        self.high_water = next;
        Ok(TransferPermit {
            parent: self.parent,
            transfer_id: next,
        })
    }
}

/// Encode C/Rust-shared 88B Parent record; `编码共享88B父Record`。
/// # Errors
/// Invalid C1 context or transaction rejects.
pub fn encode_parent_record(
    parent: TransferParent,
    high_water: u32,
    transaction: u64,
) -> Result<[u8; 88]> {
    parent.validate()?;
    if parent.kind != ParentKind::C1 || transaction == 0 || transaction == u64::MAX {
        return Err(Error::Argument);
    }
    let mut b = [0; 88];
    b[..4].copy_from_slice(b"UCPT");
    b[4..6].copy_from_slice(&1u16.to_be_bytes());
    b[6] = parent.security.origin as u8;
    b[8..12].copy_from_slice(&parent.realm.to_be_bytes());
    b[12..16].copy_from_slice(&parent.source.address.to_be_bytes());
    b[16..20].copy_from_slice(&parent.source.generation.to_be_bytes());
    b[20..36].copy_from_slice(&parent.source.principal);
    b[36..40].copy_from_slice(&parent.destination.address.to_be_bytes());
    b[40..44].copy_from_slice(&parent.destination.generation.to_be_bytes());
    b[44..60].copy_from_slice(&parent.destination.principal);
    b[60..64].copy_from_slice(&parent.security.policy.to_be_bytes());
    b[64..68].copy_from_slice(&parent.security.session.to_be_bytes());
    b[68..72].copy_from_slice(&parent.security.key.to_be_bytes());
    b[72..76].copy_from_slice(&parent.generation.to_be_bytes());
    b[76..80].copy_from_slice(&high_water.to_be_bytes());
    b[80..88].copy_from_slice(&transaction.to_be_bytes());
    Ok(b)
}
/// Decode canonical C1 Parent record; `解码C1父Record`。
/// # Errors
/// Wrong length/magic/schema/domain is malformed.
pub fn decode_parent_record(
    b: &[u8],
    current_security: SecurityDomain,
) -> Result<(TransferParent, u32, u64)> {
    if b.len() != 88 || &b[..4] != b"UCPT" || crate::codec::be16(b, 4) != 1 || b[7] != 0 {
        return Err(Error::Malformed);
    }
    let origin = OriginSecurity::try_from(b[6]).map_err(|_| Error::Malformed)?;
    current_security.validate()?;
    if current_security.origin != origin
        || current_security.session != crate::codec::be32(b, 64)
        || current_security.key != crate::codec::be32(b, 68)
        || current_security.policy != crate::codec::be32(b, 60)
    {
        return Err(Error::State);
    }
    let mut source = [0; 16];
    source.copy_from_slice(&b[20..36]);
    let mut destination = [0; 16];
    destination.copy_from_slice(&b[44..60]);
    // Parent security fingerprint is reconstructed by the Session/Context Owner at use time.
    // 父安全指纹在实际使用时由Session/Context Owner重建，Record不自报认证。
    let parent = TransferParent {
        kind: ParentKind::C1,
        id: 0,
        generation: crate::codec::be32(b, 72),
        realm: crate::codec::be32(b, 8),
        source: Binding {
            principal: source,
            address: crate::codec::be32(b, 12),
            generation: crate::codec::be32(b, 16),
        },
        destination: Binding {
            principal: destination,
            address: crate::codec::be32(b, 36),
            generation: crate::codec::be32(b, 40),
        },
        security: current_security,
        feedback_flow: None,
    };
    parent.validate().map_err(|_| Error::Malformed)?;
    let tx = crate::codec::be64(b, 80);
    if tx == 0 || tx == u64::MAX {
        return Err(Error::Malformed);
    }
    Ok((parent, crate::codec::be32(b, 76), tx))
}
