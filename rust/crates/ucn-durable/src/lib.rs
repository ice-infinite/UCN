#![no_std]
#![forbid(unsafe_code)]

//! Exact, Coordinator-routed Persistence consumer. 共同精确回读证明消费工具。
//! This module never calls a Provider or another Owner. 不调用 Provider 或业务 Owner。

use ucn_persistence::{
    CodecWorkspace, DomainKey, DomainState, DomainView, PersistenceHandle, PersistenceProof,
    PersistenceRequest, RecordMeta, body_digest,
};
use ucn_types::{Error, Result};

/// Fixed business-domain binding; 固定业务持久化域绑定。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DurableContract {
    /// Runtime instance; Runtime实例。
    pub runtime: u32,
    /// Business Owner; 业务Owner。
    pub owner: u16,
    /// Persistence Owner; 持久化Owner。
    pub persistence_owner: u16,
    /// Exact Domain; 精确域。
    pub domain: DomainKey,
    /// Record schema; 正文Schema。
    pub schema: u16,
    /// Schema version; Schema版本。
    pub version: u16,
    /// Authorized operation; 唯一业务操作种类。
    pub operation: u16,
}

/// Verified recovery baseline; 已验证的恢复基线，字段不可外部构造。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DurableBase {
    contract: DurableContract,
    view: DomainView,
}

impl DurableBase {
    /// Verify the full current body before deriving a next transaction.
    /// 推导下一事务之前验证完整当前正文。
    /// # Errors
    /// Non-Ready, wrong schema, ambiguous empty state or digest mismatch is rejected.
    /// 非Ready、错Schema、空域不一致或摘要不匹配拒绝。
    pub fn from_view(
        contract: DurableContract,
        view: DomainView,
        body: &[u8],
        scratch: &mut CodecWorkspace,
    ) -> Result<Self> {
        if contract.runtime == 0
            || contract.owner == 0
            || contract.persistence_owner == 0
            || contract.owner == contract.persistence_owner
            || contract.schema == 0
            || contract.version == 0
            || contract.operation == 0
            || view.domain != contract.domain
            || view.domain_generation == 0
            || view.state != DomainState::Ready
            || view.pending
        {
            return Err(Error::State);
        }
        if view.record_generation == 0 {
            if view.foundation_transaction_id != 0
                || view.body_bytes != 0
                || !body.is_empty()
                || view.body_digest != [0; 16]
                || view.current_operation_kind != 0
            {
                return Err(Error::Malformed);
            }
        } else {
            if body.len() != view.body_bytes as usize
                || view.schema_id != contract.schema
                || view.schema_version != contract.version
                || view.current_operation_kind == 0
            {
                return Err(Error::Malformed);
            }
            let meta = RecordMeta {
                domain: contract.domain,
                schema_id: contract.schema,
                schema_version: contract.version,
                record_generation: view.record_generation,
                transaction_id: view.foundation_transaction_id,
                operation_kind: view.current_operation_kind,
                body_bytes: view.body_bytes,
                body_digest: [0; 16],
            };
            if body_digest(&meta, body, scratch)? != view.body_digest {
                return Err(Error::Malformed);
            }
        }
        Ok(Self { contract, view })
    }
    /// Verified current generation; 已核对当前代际。
    #[must_use]
    pub const fn generation(self) -> u64 {
        self.view.record_generation
    }
    /// Confirmed Foundation Record digest, including metadata; 已确认Foundation Record摘要，含元数据。
    #[must_use]
    pub const fn record_digest(self) -> [u8; 16] {
        self.view.body_digest
    }
    /// Verified transaction high-water; 已核对事务高水位。
    #[must_use]
    pub const fn transaction(self) -> u64 {
        self.view.foundation_transaction_id
    }
    /// Exact business binding; 精确业务域。
    #[must_use]
    pub const fn contract(self) -> DurableContract {
        self.contract
    }
    /// Verify that the supplied restore bytes are exactly the previously verified body.
    /// 验证恢复正文与先前核对的正文完全一致。
    /// # Errors
    /// Another well-formed body, stale generation or modified bytes rejects.
    pub fn verify_body(self, body: &[u8], scratch: &mut CodecWorkspace) -> Result<()> {
        Self::from_view(self.contract, self.view, body, scratch).map(|_| ())
    }
    /// Select a domain-specific next operation after verifying the old record.
    /// 旧Record已验证后选择本域特定下一操作。
    /// # Errors
    /// Zero operation is never a valid writer operation.
    pub fn for_operation(mut self, operation: u16) -> Result<Self> {
        if operation == 0 {
            return Err(Error::Argument);
        }
        self.contract.operation = operation;
        Ok(self)
    }
}

/// One fixed pending update; 一次固定容量待持久化转换。
pub struct DurableUpdate<const BYTES: usize> {
    base: DurableBase,
    body: [u8; BYTES],
    length: usize,
    deadline: u64,
    continuation: u32,
    digest: [u8; 16],
    bound: Option<PersistenceHandle>,
    published: bool,
}

impl<const BYTES: usize> DurableUpdate<BYTES> {
    /// Freeze one exact next body; 冻结一次精确下一正文。
    /// # Errors
    /// Capacity, deadline, overflow or malformed body is rejected atomically.
    /// 容量、时限、溢出或非法正文原子拒绝。
    pub fn prepare(
        base: DurableBase,
        body: &[u8],
        now: u64,
        deadline: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<Self> {
        if body.is_empty() || body.len() > BYTES || now == 0 || now >= deadline || continuation == 0
        {
            return Err(Error::Argument);
        }
        let meta = RecordMeta {
            domain: base.contract.domain,
            schema_id: base.contract.schema,
            schema_version: base.contract.version,
            record_generation: base.generation().checked_add(1).ok_or(Error::Exhausted)?,
            transaction_id: base.transaction().checked_add(1).ok_or(Error::Exhausted)?,
            operation_kind: base.contract.operation,
            body_bytes: u32::try_from(body.len()).map_err(|_| Error::Argument)?,
            body_digest: [0; 16],
        };
        let digest = body_digest(&meta, body, scratch)?;
        let mut frozen = [0; BYTES];
        frozen[..body.len()].copy_from_slice(body);
        Ok(Self {
            base,
            body: frozen,
            length: body.len(),
            deadline,
            continuation,
            digest,
            bound: None,
            published: false,
        })
    }

    /// Route this requirement through Coordinator; 经Coordinator路由此要求。
    #[must_use]
    pub fn request(&self) -> PersistenceRequest<'_> {
        let c = self.base.contract;
        PersistenceRequest {
            runtime_instance: c.runtime,
            caller_owner_instance: c.owner,
            domain_generation: self.base.view.domain_generation,
            domain: c.domain,
            foundation_transaction_id: self.base.transaction() + 1,
            expected_record_generation: self.base.generation(),
            absolute_deadline_us: self.deadline,
            business_transition_digest: u64::from_be_bytes([
                self.digest[0],
                self.digest[1],
                self.digest[2],
                self.digest[3],
                self.digest[4],
                self.digest[5],
                self.digest[6],
                self.digest[7],
            ])
            .max(1),
            canonical_body: &self.body[..self.length],
            schema_id: c.schema,
            schema_version: c.version,
            operation_kind: c.operation,
            expected_body_digest: self.base.view.body_digest,
            volatile_continuation: self.continuation,
        }
    }

    /// Bind the accepted Persistence handle; 绑定已接纳持久化Handle。
    /// # Errors
    /// A second different handle or a terminal update is rejected.
    /// 二次错绑定或终态拒绝。
    pub fn bind(&mut self, handle: PersistenceHandle) -> Result<()> {
        if self.published || self.bound.is_some_and(|old| old != handle) {
            return Err(Error::State);
        }
        self.bound = Some(handle);
        Ok(())
    }
    /// Exact outstanding handle, not durable publication; 精确未决Handle，不代表耐久发布。
    #[must_use]
    pub const fn bound_handle(&self) -> Option<PersistenceHandle> {
        self.bound
    }

    /// Consume an exact reload proof. Durable state remains authoritative after timeout.
    /// 消费精确回读证明；超时不撤销已经持久化的状态，业务授权另行复验。
    /// # Errors
    /// Wrong Owner, body, continuation or witness rejects without changing the update.
    /// 错Owner/正文/continuation/witness零写拒绝。
    pub fn confirm(&mut self, handle: PersistenceHandle, proof: PersistenceProof) -> Result<()> {
        let c = self.base.contract;
        if self.bound != Some(handle)
            || proof.runtime_instance() != c.runtime
            || proof.caller_owner_instance() != c.owner
            || proof.persistence_owner_instance() != c.persistence_owner
            || proof.domain() != c.domain
            || proof.domain_generation() != self.base.view.domain_generation
            || proof.operation_kind() != c.operation
            || proof.record_generation() != self.base.generation() + 1
            || proof.witness_generation() != proof.record_generation()
            || proof.foundation_transaction_id() != self.base.transaction() + 1
            || proof.body_bytes() as usize != self.length
            || proof.body_digest() != self.digest
            || proof.volatile_continuation() != self.continuation
        {
            return Err(Error::State);
        }
        self.published = true;
        Ok(())
    }
    /// Publication gate; 发布门。
    #[must_use]
    pub const fn is_confirmed(&self) -> bool {
        self.published
    }
    /// Frozen body; 冻结正文。
    #[must_use]
    pub fn body(&self) -> &[u8] {
        &self.body[..self.length]
    }
}
