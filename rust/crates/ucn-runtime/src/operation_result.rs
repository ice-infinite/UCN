//! Terminal recovery/ACK and durable tombstones, without resurrecting execution permission.
//! 终态恢复、结果ACK和耐久墓碑，绝不恢复执行权限。
use crate::{
    ControlBody, ControlCommand, ControlCoordinator, ControlHandle, ControlReplies, ReplyHandle,
};
use ucn_digest::DigestWorkspace;
use ucn_durable::{DurableBase, DurableContract};
use ucn_persistence::{CodecWorkspace, PersistenceHandle, PersistenceOwner, RequestState};
use ucn_security::{
    AccessDirection, AccessRequest, CurrentFacts, Fingerprint, SecurityOwner, SessionHandle,
};
use ucn_service::{
    OperationKey, OperationOwner, OperationPhase, OperationReceipt, OperationResult,
    OperationWireKey, operation_result_digest,
};
use ucn_types::{Error, HopProfile, OriginSecurity, ProtocolOpcode, Result};

fn wire(key: OperationKey) -> OperationWireKey {
    OperationWireKey {
        operation: key.service.operation,
        service: key.service.service,
        opcode: key.service.opcode,
        request_digest: key.request_digest,
    }
}
fn opcode(command: &ControlCommand, key: OperationKey) -> Result<ProtocolOpcode> {
    match command.body() {
        ControlBody::OperationResume(v) if v == wire(key) => Ok(ProtocolOpcode::OperationResume),
        ControlBody::OperationResultAck(v) if v.key == wire(key) => {
            Ok(ProtocolOpcode::OperationResultAck)
        }
        ControlBody::OperationResult(v) if v.receipt.key == wire(key) => {
            Ok(ProtocolOpcode::OperationResult)
        }
        _ => Err(Error::Access),
    }
}
/// Stable original identities remain exact; only authenticated non-regressing security may change.
/// 原稳定身份保持精确，仅允许实际认证且不回退的安全域变化。
fn current<const S: usize, const R: usize>(
    command: &ControlCommand,
    key: OperationKey,
    security: &SecurityOwner<'_, S, R>,
    session: SessionHandle,
    facts: CurrentFacts,
    reverse: bool,
) -> Result<()> {
    key.service.validate()?;
    wire(key).validate()?;
    let (realm, source, destination) = command.evidence().identities();
    let (generation, received_key, policy, origin, fingerprint) = command.evidence().security();
    let k = key.service;
    let view = security.session_get(session, facts)?;
    let (client, server) = if reverse {
        (destination, source)
    } else {
        (source, destination)
    };
    let key_generation = if reverse {
        security.outbound_key_generation(session, facts)?
    } else {
        received_key
    };
    if command.header().hop_limit.get() != 1
        || facts.now_us < command.accepted_at()
        || facts.peer != source
        || facts.local != destination
        || realm != k.realm
        || realm != security.realm().get()
        || client.principal != k.client
        || server.principal != k.server
        || client.address.get() != k.client_address
        || client.generation.get() != k.client_generation
        || server.address.get() != k.server_address
        || server.generation.get() != k.server_generation
        || view.session_generation.get() != generation
        || generation < k.session
        || key_generation < k.key_generation
        || policy < k.policy
        || origin == OriginSecurity::O0
        || (k.origin == OriginSecurity::O2 && origin != OriginSecurity::O2)
        || (generation == k.session
            && key_generation == k.key_generation
            && policy == k.policy
            && fingerprint != k.security)
        || view.origin_security != origin
        || view.hop_profile != HopProfile::H0
        || command.service().get() != k.service
    {
        return Err(Error::Access);
    }
    security.authorize(
        AccessRequest {
            session,
            context_fingerprint: Fingerprint::new(fingerprint)?,
            service: command.service(),
            protocol_opcode: u16::from(opcode(command, key)?),
            direction: AccessDirection::Inbound,
        },
        facts,
    )?;
    Ok(())
}
fn base<const O: usize, const D: usize, const P: usize, const L: usize>(
    operation: &OperationOwner<O>,
    persistence: &PersistenceOwner<'_, D, P, L>,
    codec: &mut CodecWorkspace,
) -> Result<DurableBase> {
    let contract = operation.contract();
    let view = persistence.domain_get(contract.domain)?;
    if view.record_generation != operation.record_generation()
        || view.body_digest != operation.record_digest()
        || view.schema_id != contract.schema
        || view.schema_version != contract.version
    {
        return Err(Error::State);
    }
    let mut bytes = [0; 256];
    let n = persistence.copy_body(contract.domain, &mut bytes)?;
    DurableBase::from_view(contract, view, &bytes[..n], codec)
}

impl<const Q: usize, const B: usize> ControlCoordinator<Q, B> {
    /// Actual authenticated client ACK -> exact current Journal digest -> retention watermark.
    /// 实际认证Client ACK、当前Journal精确摘要、保留水位；重复ACK不续期。
    /// # Errors
    /// Stale media, wrong digest/identity/ACL or nonterminal state has no Journal writes.
    #[allow(clippy::too_many_arguments)]
    pub fn dispatch_result_ack<
        const O: usize,
        const D: usize,
        const P: usize,
        const L: usize,
        const S: usize,
        const R: usize,
    >(
        &mut self,
        h: ControlHandle,
        operation: &mut OperationOwner<O>,
        persistence: &PersistenceOwner<'_, D, P, L>,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        retention_us: u64,
        digest: &mut DigestWorkspace,
        codec: &mut CodecWorkspace,
    ) -> Result<()> {
        let command = self.view(h)?;
        let key = operation.key().ok_or(Error::State)?;
        current(&command, key, security, session, facts, false)?;
        base(operation, persistence, codec)?;
        let ControlBody::OperationResultAck(ack) = command.body() else {
            return Err(Error::Unsupported);
        };
        if operation.receipt(digest)? != ack {
            return Err(Error::Security);
        }
        operation.acknowledge_result(key, facts.now_us, retention_us)?;
        self.ack(h)
    }
    /// Recover confirmed terminal bytes under a fresh current session, not a fresh execution lease.
    /// 在新当前会话恢复确认终态正文，不签发新执行租约。
    /// # Errors
    /// PREPARED/EXECUTING/tombstone, stale media, capacity or authorization rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn dispatch_operation_resume<
        const O: usize,
        const D: usize,
        const P: usize,
        const L: usize,
        const S: usize,
        const R: usize,
        const C: usize,
        const F: usize,
    >(
        &mut self,
        h: ControlHandle,
        operation: &OperationOwner<O>,
        persistence: &PersistenceOwner<'_, D, P, L>,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        timeout_us: u64,
        replies: &mut ControlReplies<C, F>,
        digest: &mut DigestWorkspace,
        codec: &mut CodecWorkspace,
    ) -> Result<ReplyHandle> {
        let command = self.view(h)?;
        if !matches!(command.body(), ControlBody::OperationResume(_)) {
            return Err(Error::Unsupported);
        }
        let key = operation.key().ok_or(Error::State)?;
        current(&command, key, security, session, facts, false)?;
        base(operation, persistence, codec)?;
        let mut result = OperationResult {
            receipt: operation.receipt(digest)?,
            status: 0,
            length: 0,
            payload: [0; 146],
        };
        let (_, status, length) = operation.copy_result(&mut result.payload)?;
        result.status = status;
        result.length = u16::try_from(length).map_err(|_| Error::NoSpace)?;
        result.validate()?;
        if F < 25 + 2 * security.address_width().bytes() + 54 + length + 16 {
            return Err(Error::NoSpace);
        }
        if timeout_us == 0 || timeout_us > i64::MAX as u64 {
            return Err(Error::Argument);
        }
        let expires = command
            .accepted_at()
            .checked_add(timeout_us)
            .ok_or(Error::Exhausted)?;
        if facts.now_us >= expires {
            return Err(Error::Timeout);
        }
        let reply = replies.reserve(
            command.into(),
            ControlBody::OperationResult(result),
            facts.now_us,
            expires,
        )?;
        self.ack(h)?;
        Ok(reply)
    }
}

/// Caller-registered terminal recovery; no ability to replace the old Journal execution context.
/// 调用方登记的终态恢复，无权替换旧Journal执行上下文。
pub struct OperationRecovery {
    key: OperationKey,
    accepted: u64,
    deadline: u64,
    receipt: Option<OperationReceipt>,
    domain: Option<(u32, u32, u32, OriginSecurity, [u8; 16])>,
}
/// Application terminal observation plus a separately retained ACK obligation.
/// 业务终态观察和独立保留的ACK义务。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RecoveredOperation {
    /// Durable phase; 耐久阶段。
    pub phase: u8,
    /// Exact signed status; 精确有符号状态。
    pub status: i32,
    /// Fresh delivered bytes, zero for repeated observation; 新交付正文，重复为零。
    pub bytes: usize,
    /// None for `IN_DOUBT`; `IN_DOUBT`不产生可GC的ACK。
    pub ack: Option<ReplyHandle>,
}
impl OperationRecovery {
    /// Register original exact identity and a new non-renewing query lifetime.
    /// 登记原精确身份与新查询的不续期生命周期。
    /// # Errors
    /// Invalid identity/lifetime rejects.
    pub fn new(key: OperationKey, now: u64, deadline: u64) -> Result<Self> {
        key.service.validate()?;
        wire(key).validate()?;
        if now == 0 || now >= deadline {
            return Err(Error::Argument);
        }
        Ok(Self {
            key,
            accepted: now,
            deadline,
            receipt: None,
            domain: None,
        })
    }
}
impl<const Q: usize, const B: usize> ControlCoordinator<Q, B> {
    /// Authenticated result + digest verification + ACK reservation precede business delivery.
    /// 认证结果、摘要核对、ACK预留均先于业务交付。
    /// # Errors
    /// Wrong context/digest/capacity/deadline preserves recovery and output.
    #[allow(clippy::too_many_arguments)]
    pub fn dispatch_recovered_result<
        const S: usize,
        const R: usize,
        const C: usize,
        const F: usize,
    >(
        &mut self,
        h: ControlHandle,
        recovery: &mut OperationRecovery,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        replies: &mut ControlReplies<C, F>,
        output: &mut [u8],
        digest: &mut DigestWorkspace,
    ) -> Result<RecoveredOperation> {
        let command = self.view(h)?;
        current(&command, recovery.key, security, session, facts, true)?;
        if command.accepted_at() < recovery.accepted || facts.now_us >= recovery.deadline {
            return Err(Error::Timeout);
        }
        let ControlBody::OperationResult(result) = command.body() else {
            return Err(Error::Unsupported);
        };
        let n = usize::from(result.length);
        let domain = command.evidence().security();
        if recovery.domain.is_some_and(|old| old != domain)
            || recovery.receipt.is_some_and(|old| old != result.receipt)
        {
            return Err(Error::Replay);
        }
        if operation_result_digest(
            result.receipt.key,
            result.receipt.phase,
            result.status,
            &result.payload[..n],
            digest,
        )? != result.receipt.result_digest
        {
            return Err(Error::Security);
        }
        let duplicate = recovery.receipt.is_some();
        if !duplicate && output.len() < n {
            return Err(Error::NoSpace);
        }
        let ack = if matches!(result.receipt.phase, 3 | 4) {
            security.authorize(
                AccessRequest {
                    session,
                    context_fingerprint: Fingerprint::new(domain.4)?,
                    service: command.service(),
                    protocol_opcode: u16::from(ProtocolOpcode::OperationResultAck),
                    direction: AccessDirection::Outbound,
                },
                facts,
            )?;
            if F < 25 + 2 * security.address_width().bytes() + 48 + 16 {
                return Err(Error::NoSpace);
            }
            Some(replies.reserve(
                command.into(),
                ControlBody::OperationResultAck(result.receipt),
                facts.now_us,
                recovery.deadline,
            )?)
        } else {
            None
        };
        self.ack(h)?;
        recovery.receipt = Some(result.receipt);
        recovery.domain = Some(domain);
        if !duplicate {
            output[..n].copy_from_slice(&result.payload[..n]);
        }
        Ok(RecoveredOperation {
            phase: result.receipt.phase,
            status: result.status,
            bytes: if duplicate { 0 } else { n },
            ack,
        })
    }
}

/// Bounded GC continuation; deletion means result-body tombstone, never ID-history removal.
/// 有界GC续接；删除仅指正文墓碑化，绝不删除ID历史。
pub struct OperationGc {
    contract: Option<DurableContract>,
    key: Option<OperationKey>,
    io: Option<PersistenceHandle>,
    confirmed: bool,
}
impl Default for OperationGc {
    fn default() -> Self {
        Self::new()
    }
}
impl OperationGc {
    /// Empty static GC holder; 空静态GC Holder。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            contract: None,
            key: None,
            io: None,
            confirmed: false,
        }
    }
    /// Exact pending I/O, retained through failure/proof retirement; 保留精确I/O至失败/Proof退休。
    #[must_use]
    pub const fn pending_io(&self) -> Option<PersistenceHandle> {
        self.io
    }
    /// Includes a frozen update awaiting admission; 包括冻结后等待接纳的更新。
    #[must_use]
    pub const fn is_active(&self) -> bool {
        self.key.is_some()
    }
    /// Retention -> Tombstone freeze -> Foundation admission, without Provider callback.
    /// 保留期结束、墓碑冻结、Foundation接纳，不隐藏Provider回调。
    /// # Errors
    /// No ACK, early time, stale body or outstanding holder rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn begin<const O: usize, const D: usize, const P: usize, const L: usize>(
        &mut self,
        operation: &mut OperationOwner<O>,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
        now: u64,
        deadline: u64,
        continuation: u32,
        codec: &mut CodecWorkspace,
    ) -> Result<PersistenceHandle> {
        if self.io.is_some() || self.key.is_some() {
            return Err(Error::State);
        }
        let key = operation.key().ok_or(Error::State)?;
        if operation.gc_after().is_none_or(|until| now < until) {
            return Err(Error::State);
        }
        let baseline = base(operation, persistence, codec)?;
        operation.transition(
            OperationPhase::Tombstoned,
            baseline,
            0,
            &[],
            now,
            deadline,
            continuation,
            codec,
        )?;
        self.contract = Some(operation.contract());
        self.key = Some(key);
        self.confirmed = false;
        self.submit(operation, persistence, now)
    }
    /// Retry Foundation admission without forgetting the frozen update; 重试接纳，保留冻结更新。
    /// # Errors
    /// Wrong Owner/already submitted rejects.
    pub fn submit<const O: usize, const D: usize, const P: usize, const L: usize>(
        &mut self,
        operation: &mut OperationOwner<O>,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
        now: u64,
    ) -> Result<PersistenceHandle> {
        self.check(operation)?;
        if self.io.is_some() || self.confirmed {
            return Err(Error::State);
        }
        let io = persistence.submit(&operation.requirement()?, now)?;
        self.io = Some(io);
        operation.bind(io)?;
        Ok(io)
    }
    fn check<const O: usize>(&self, operation: &OperationOwner<O>) -> Result<()> {
        if self.key.is_none()
            || self.key != operation.key()
            || self.contract != Some(operation.contract())
        {
            Err(Error::State)
        } else {
            Ok(())
        }
    }
    /// Exact reload must succeed before clearing result bytes; Proof retirement is retryable.
    /// 精确回读成功后才清除正文，Proof退休可重试。
    /// # Errors
    /// Pending/failing/wrong proof retains the holder.
    pub fn finish<const O: usize, const D: usize, const P: usize, const L: usize>(
        &mut self,
        operation: &mut OperationOwner<O>,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
    ) -> Result<()> {
        self.check(operation)?;
        let io = self.io.ok_or(Error::State)?;
        if !self.confirmed {
            operation.confirm(io, persistence.proof_get(io)?)?;
            self.confirmed = true;
        }
        persistence.proof_retire(io)?;
        self.io = None;
        self.key = None;
        self.contract = None;
        self.confirmed = false;
        Ok(())
    }
    /// Verified failure retirement fences, never restores the old execution context.
    /// 已核对失败的退休只设围栏，绝不恢复旧执行域。
    /// # Errors
    /// Wrong/pending/successful I/O cannot be discarded.
    pub fn retire_failed<const O: usize, const D: usize, const P: usize, const L: usize>(
        &mut self,
        operation: &mut OperationOwner<O>,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
    ) -> Result<Error> {
        self.check(operation)?;
        let io = self.io.ok_or(Error::State)?;
        let view = persistence.request_get(io)?;
        if self.confirmed || view.state != RequestState::Failed {
            return Err(Error::State);
        }
        let error = view.terminal_error.ok_or(Error::State)?;
        operation.fence_pending(io)?;
        persistence.failed_retire(io)?;
        self.io = None;
        self.key = None;
        self.contract = None;
        Ok(error)
    }
}
