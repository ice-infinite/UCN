use crate::ServiceKey;
use ucn_durable::{DurableBase, DurableContract, DurableUpdate};
use ucn_persistence::{
    CodecWorkspace, DomainKind, PersistenceHandle, PersistenceProof, PersistenceRequest,
};
use ucn_types::{Error, OriginSecurity, Result};

const BODY_BYTES: usize = 256;

/// Product's bounded executor reconciliation boundary; 产品有界执行器对账接口。
pub trait ExecutorQuery {
    /// Query the exact Operation, never execute it again; 查询精确Operation，绝不再执行。
    /// # Errors
    /// Unknown results return `InDoubt`; output cannot exceed the supplied fixed slice.
    fn query(&mut self, key: OperationKey, result: &mut [u8]) -> Result<(i32, usize)>;
}
/// Durable Operation phases; 耐久Operation阶段。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OperationPhase {
    /// No effect, durable prepare; 无副作用耐久准备。
    Prepared = 1,
    /// Executing record durable before handoff; 交接前执行Record耐久。
    Executing = 2,
    /// Reconciled durable result; 已对账耐久结果。
    CommittedResult = 3,
    /// Proven no executor observation; 证明执行器未观察。
    AbortedNoEffect = 4,
    /// Unknown external result, do not reexecute; 结果未知，禁止重执行。
    InDoubt = 5,
    /// Durable replay tombstone; 耐久重放墓碑。
    Tombstoned = 6,
}
/// One Operation key with exact arguments; 含精确参数摘要的Operation键。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationKey {
    /// Full RPC key; 完整RPC键。
    pub service: ServiceKey,
    /// Exact request digest; 精确请求摘要。
    pub request_digest: [u8; 16],
}
/// Unique executor permission, issued once after exact EXECUTING reload.
/// 精确EXECUTING回读后签发一次的执行器权限。
#[derive(Debug, Eq, PartialEq)]
pub struct ExecutionPermit {
    key: OperationKey,
    generation: u32,
}
impl ExecutionPermit {
    /// Immutable executor key; 不变执行器键。
    #[must_use]
    pub const fn key(&self) -> OperationKey {
        self.key
    }
}
/// Fixed retained Operation and pre-reserved inline result.
/// 固定保留Operation及预留内联结果。
#[allow(clippy::struct_excessive_bools)] // Distinct durable observation, local handoff, ACK and fault facts.
pub struct OperationOwner<const RESULT_BYTES: usize> {
    contract: DurableContract,
    key: Option<OperationKey>,
    phase: Option<OperationPhase>,
    observed: bool,
    handed_off: bool,
    result: [u8; RESULT_BYTES],
    length: usize,
    status: i32,
    pending: Option<(DurableUpdate<BODY_BYTES>, OperationPhase)>,
    generation: u32,
    durable_generation: u64,
    durable_digest: [u8; 16],
    retention: u64,
    result_ack: bool,
    fault: bool,
    reconciled: bool,
    result_ready: bool,
}
impl<const B: usize> OperationOwner<B> {
    /// Exact installed Journal identity, never chosen by a short Operation ID.
    /// 已安装Journal精确身份，不以短Operation ID代替。
    #[must_use]
    pub const fn key(&self) -> Option<OperationKey> {
        self.key
    }
    /// Exact immutable Journal binding; 精确不变Journal绑定。
    #[must_use]
    pub const fn contract(&self) -> DurableContract {
        self.contract
    }
    /// Last actually confirmed Record generation; 最后实际确认的Record代际。
    #[must_use]
    pub const fn record_generation(&self) -> u64 {
        self.durable_generation
    }
    /// Digest of the last confirmed Record, not the mutable executor result.
    /// 最后确认Record摘要，不是可变执行器结果摘要。
    #[must_use]
    pub const fn record_digest(&self) -> [u8; 16] {
        self.durable_digest
    }

    /// Publish only confirmed durable terminal observation, without exposing execution permission.
    /// 仅发布已确认耐久终态观察，不签发执行权限。
    /// # Errors
    /// Pending/nonterminal/faulted Journal or invalid business key rejects.
    pub fn receipt(
        &self,
        scratch: &mut ucn_digest::DigestWorkspace,
    ) -> Result<crate::OperationReceipt> {
        if self.pending.is_some() || self.fault || self.reconciled {
            return Err(Error::State);
        }
        let key = self.key.ok_or(Error::State)?;
        let phase = self.phase.ok_or(Error::State)? as u8;
        let wire = crate::OperationWireKey {
            operation: key.service.operation,
            service: key.service.service,
            opcode: key.service.opcode,
            request_digest: key.request_digest,
        };
        Ok(crate::OperationReceipt {
            key: wire,
            result_digest: crate::operation_result_digest(
                wire,
                phase,
                self.status,
                &self.result[..self.length],
                scratch,
            )?,
            phase,
        })
    }

    /// Bind one Operation Journal domain; `绑定一个OperationJournal域`。
    /// # Errors
    /// Wrong domain or result capacity above the Foundation bound rejects.
    pub fn new(contract: DurableContract) -> Result<Self> {
        if contract.domain.kind != DomainKind::OperationJournal
            || B == 0
            || B > 146
            || contract.operation != 0x0701
        {
            return Err(Error::Config);
        }
        Ok(Self {
            contract,
            key: None,
            phase: None,
            observed: false,
            handed_off: false,
            result: [0; B],
            length: 0,
            status: 0,
            pending: None,
            generation: 0,
            durable_generation: 0,
            durable_digest: [0; 16],
            retention: 0,
            result_ack: false,
            fault: false,
            reconciled: false,
            result_ready: false,
        })
    }

    /// Restore durable PREPARED/EXECUTING/terminal without invoking an executor.
    /// 恢复耐久状态，绝不调用执行器。
    /// # Errors
    /// Wrong key/schema/status/body rejects before publication.
    pub fn restore(
        &mut self,
        base: DurableBase,
        body: &[u8],
        security_fingerprint: [u8; 16],
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if base.contract() != self.contract
            || self.key.is_some()
            || self.pending.is_some()
            || base.generation() == 0
        {
            return Err(Error::State);
        }
        base.verify_body(body, scratch)?;
        let (key, phase, observed, status, payload) = decode_operation(body, security_fingerprint)?;
        if payload.len() > B {
            return Err(Error::Malformed);
        }
        self.key = Some(key);
        self.durable_digest = base.record_digest();
        self.phase = Some(phase);
        self.observed = observed;
        self.handed_off = phase == OperationPhase::Executing || observed;
        self.status = status;
        self.length = payload.len();
        self.result[..payload.len()].copy_from_slice(payload);
        self.generation = 1;
        self.durable_generation = base.generation();
        self.result_ready = phase == OperationPhase::CommittedResult;
        Ok(())
    }

    /// Pre-reserve and freeze PREPARED; 预留并冻结PREPARED。
    /// # Errors
    /// Conflicting key/full journal/pending/deadline rejects before I/O.
    pub fn begin(
        &mut self,
        key: OperationKey,
        base: DurableBase,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        key.service.validate()?;
        if key.request_digest == [0; 16]
            || base.contract() != self.contract
            || self.pending.is_some()
            || self.fault
        {
            return Err(Error::State);
        }
        if let Some(old) = self.key {
            return if old == key {
                Ok(())
            } else {
                Err(Error::Replay)
            };
        }
        if base.generation() != 0 {
            return Err(Error::State);
        }
        let generation = self.generation.checked_add(1).ok_or(Error::Exhausted)?;
        let mut body = [0; BODY_BYTES];
        let length = encode_operation(key, OperationPhase::Prepared, false, 0, &[], &mut body)?;
        let update =
            DurableUpdate::prepare(base, &body[..length], now, until, continuation, scratch)?;
        self.key = Some(key);
        self.pending = Some((update, OperationPhase::Prepared));
        self.generation = generation;
        Ok(())
    }
    /// Freeze an allowed transition; 冻结合规转换。
    /// # Errors
    /// Unsafe abort, second execution, no-effect lie, bad result or stale baseline rejects.
    #[allow(clippy::too_many_arguments)] // Explicit transition identity, result and persistence deadline.
    pub fn transition(
        &mut self,
        next: OperationPhase,
        base: DurableBase,
        status: i32,
        payload: &[u8],
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if self.pending.is_some()
            || base.contract() != self.contract
            || base.generation() != self.durable_generation
            || (self.fault && next != OperationPhase::InDoubt)
        {
            return Err(Error::State);
        }
        let current = self.phase.ok_or(Error::State)?;
        let legal = match (current, next) {
            (
                OperationPhase::Prepared,
                OperationPhase::Executing | OperationPhase::AbortedNoEffect,
            )
            | (OperationPhase::Executing, OperationPhase::InDoubt) => true,
            (OperationPhase::Executing, OperationPhase::AbortedNoEffect) => !self.handed_off,
            (OperationPhase::Executing, OperationPhase::CommittedResult) => {
                self.observed
                    && self.result_ready
                    && self.status == status
                    && self.result[..self.length] == *payload
            }
            (OperationPhase::InDoubt, OperationPhase::CommittedResult) => {
                self.reconciled && self.status == status && self.result[..self.length] == *payload
            }
            (
                OperationPhase::CommittedResult | OperationPhase::AbortedNoEffect,
                OperationPhase::Tombstoned,
            ) => self.result_ack && now >= self.retention,
            _ => false,
        };
        if !legal
            || payload.len() > B
            || (next != OperationPhase::CommittedResult && !payload.is_empty())
        {
            return Err(Error::State);
        }
        let key = self.key.ok_or(Error::State)?;
        let mut body = [0; BODY_BYTES];
        let length = encode_operation(key, next, self.observed, status, payload, &mut body)?;
        let update =
            DurableUpdate::prepare(base, &body[..length], now, until, continuation, scratch)?;
        self.pending = Some((update, next));
        Ok(())
    }
    /// Coordinator-routed exact requirement; Coordinator路由精确要求。
    /// # Errors
    /// No pending transition rejects.
    pub fn requirement(&self) -> Result<PersistenceRequest<'_>> {
        Ok(self.pending.as_ref().ok_or(Error::State)?.0.request())
    }
    /// Bind the exact accepted handle; 绑定精确接纳Handle。
    /// # Errors
    /// Wrong phase/rebinding rejects.
    pub fn bind(&mut self, h: PersistenceHandle) -> Result<()> {
        self.pending.as_mut().ok_or(Error::State)?.0.bind(h)
    }
    /// Fence an exact failed continuation; old durable state cannot regain execution permission.
    /// 围栏精确失败Continuation，旧耐久状态不能恢复执行权限。
    /// # Errors
    /// Wrong/unbound or already published update rejects without clearing obligations.
    pub fn fence_pending(&mut self, h: PersistenceHandle) -> Result<()> {
        let update = &self.pending.as_ref().ok_or(Error::State)?.0;
        if update.bound_handle() != Some(h) || update.is_confirmed() {
            return Err(Error::State);
        }
        self.fault = true;
        self.pending = None;
        Ok(())
    }
    /// Consume exact durable reload and publish no earlier than this edge.
    /// 消费精确耐久回读，此边之前不发布。
    /// # Errors
    /// Any proof or body mismatch leaves the transition pending.
    pub fn confirm(&mut self, h: PersistenceHandle, proof: PersistenceProof) -> Result<()> {
        let pending = self.pending.as_mut().ok_or(Error::State)?;
        pending.0.confirm(h, proof)?;
        let (_, phase, _, status, payload) = decode_operation(
            pending.0.body(),
            self.key.ok_or(Error::State)?.service.security,
        )?;
        self.status = status;
        self.length = payload.len();
        self.result.fill(0);
        self.result[..payload.len()].copy_from_slice(payload);
        self.result_ready = phase == OperationPhase::CommittedResult;
        self.phase = Some(phase);
        self.reconciled = false;
        self.durable_generation = proof.record_generation();
        self.durable_digest = proof.body_digest();
        self.pending = None;
        Ok(())
    }
    /// Issue a linear executor handoff once; 签发一次线性执行器交接权限。
    /// # Errors
    /// No EXECUTING proof, pending write or previous handoff rejects.
    pub fn handoff(&mut self, current_key: OperationKey) -> Result<ExecutionPermit> {
        if self.key != Some(current_key)
            || self.phase != Some(OperationPhase::Executing)
            || self.pending.is_some()
            || self.handed_off
            || self.fault
        {
            return Err(Error::State);
        }
        self.handed_off = true;
        self.observed = true;
        Ok(ExecutionPermit {
            key: current_key,
            generation: self.generation,
        })
    }
    /// Finish a proven executor observation; 完成已证明执行器观察。
    /// # Errors
    /// Wrong permit or repeated completion rejects; oversized result fences execution.
    #[allow(clippy::needless_pass_by_value)] // Executor permission is linear and consumed exactly once.
    pub fn executor_result(
        &mut self,
        permit: ExecutionPermit,
        status: i32,
        payload: &[u8],
    ) -> Result<()> {
        if permit.key != self.key.ok_or(Error::State)?
            || permit.generation != self.generation
            || !self.handed_off
            || self.phase != Some(OperationPhase::Executing)
            || self.pending.is_some()
        {
            return Err(Error::State);
        }
        if payload.len() > B {
            self.fault = true;
            return Err(Error::InDoubt);
        }
        self.status = status;
        self.length = payload.len();
        self.result[..payload.len()].copy_from_slice(payload);
        self.result_ready = true;
        Ok(())
    }
    /// Read only confirmed terminal result; 仅读取确认终态结果。
    /// # Errors
    /// Pending, PREPARED or EXECUTING never appears as a final result.
    pub fn copy_result(&self, out: &mut [u8]) -> Result<(OperationPhase, i32, usize)> {
        if self.pending.is_some() || self.fault || self.reconciled {
            return Err(Error::State);
        }
        let phase = self.phase.ok_or(Error::State)?;
        if !matches!(
            phase,
            OperationPhase::CommittedResult
                | OperationPhase::AbortedNoEffect
                | OperationPhase::InDoubt
        ) {
            return Err(Error::State);
        }
        if out.len() < self.length {
            return Err(Error::NoSpace);
        }
        out[..self.length].copy_from_slice(&self.result[..self.length]);
        Ok((phase, self.status, self.length))
    }
    /// Current confirmed phase plus pending marker; 当前确认状态及pending标志。
    #[must_use]
    pub const fn view(&self) -> (Option<OperationPhase>, bool, bool) {
        (self.phase, self.pending.is_some(), self.handed_off)
    }

    /// Query an executor under a shared callback gate; 共享回调门下查询执行器。
    /// # Errors
    /// Wrong key/phase, pending write, Provider failure or oversized output fails closed.
    pub fn reconcile(
        &mut self,
        key: OperationKey,
        provider: &mut impl ExecutorQuery,
        gate: &ucn_owner::CallbackGate,
    ) -> Result<()> {
        if self.key != Some(key)
            || self.phase != Some(OperationPhase::InDoubt)
            || self.pending.is_some()
        {
            return Err(Error::State);
        }
        let next = self.generation.checked_add(1).ok_or(Error::Exhausted)?;
        let claim = ucn_owner::CallbackClaim::new(u32::from(self.contract.owner), next, 1, 0x0701)?;
        let lease = gate.try_enter(claim)?;
        self.generation = next;
        let outcome = provider.query(key, &mut self.result);
        gate.leave(lease)?;
        let (status, length) = outcome?;
        if length > B {
            self.fault = true;
            return Err(Error::InDoubt);
        }
        self.status = status;
        self.length = length;
        self.reconciled = true;
        self.result_ready = true;
        Ok(())
    }
    /// Require authenticated result ACK and minimum retention before Tombstone.
    /// 墓碑前要求认证结果ACK及最小保留。
    /// # Errors
    /// Wrong key/phase or invalid retention rejects.
    pub fn acknowledge_result(
        &mut self,
        key: OperationKey,
        now: u64,
        retention: u64,
    ) -> Result<()> {
        if self.key != Some(key)
            || self.pending.is_some()
            || self.fault
            || self.reconciled
            || !matches!(
                self.phase,
                Some(OperationPhase::CommittedResult | OperationPhase::AbortedNoEffect)
            )
            || now == 0
            || retention == 0
            || retention > i64::MAX as u64
        {
            return Err(Error::State);
        }
        let until = now.checked_add(retention).ok_or(Error::Argument)?;
        if !self.result_ack {
            self.retention = until;
            self.result_ack = true;
        }
        Ok(())
    }

    /// Retention watermark from an accepted result ACK; 结果ACK产生的保留水位。
    /// Crash loses this volatile permission and therefore delays, never accelerates, GC.
    /// 掉电丢失此易失权限，只会延迟而不会提前GC。
    #[must_use]
    pub const fn gc_after(&self) -> Option<u64> {
        if self.result_ack {
            Some(self.retention)
        } else {
            None
        }
    }
}

/// Canonical OPS1 business Record shared with C; `与C共享OPS1业务Record`。
/// # Errors
/// Invalid key/phase/result capacity leaves output unchanged.
pub fn encode_operation(
    key: OperationKey,
    phase: OperationPhase,
    observed: bool,
    status: i32,
    payload: &[u8],
    out: &mut [u8],
) -> Result<usize> {
    key.service.validate()?;
    let k = key.service;
    let size = 110usize.checked_add(payload.len()).ok_or(Error::Argument)?;
    if key.request_digest == [0; 16]
        || size > 256
        || out.len() < size
        || payload.len() > u16::MAX as usize
        || (phase != OperationPhase::CommittedResult && !payload.is_empty())
        || (phase == OperationPhase::AbortedNoEffect && observed)
    {
        return Err(Error::Argument);
    }
    let mut bytes = [0; 256];
    bytes[..4].copy_from_slice(b"OPS1");
    bytes[4..6].copy_from_slice(&1u16.to_be_bytes());
    bytes[6] = phase as u8;
    bytes[7] = u8::from(observed);
    bytes[8..16].copy_from_slice(&k.operation.to_be_bytes());
    bytes[16..20].copy_from_slice(&k.realm.to_be_bytes());
    bytes[20..22].copy_from_slice(&k.service.to_be_bytes());
    bytes[22..24].copy_from_slice(&k.opcode.to_be_bytes());
    bytes[24..28].copy_from_slice(&k.client_address.to_be_bytes());
    bytes[28..32].copy_from_slice(&k.client_generation.to_be_bytes());
    bytes[32..48].copy_from_slice(&k.client);
    bytes[48..52].copy_from_slice(&k.server_address.to_be_bytes());
    bytes[52..56].copy_from_slice(&k.server_generation.to_be_bytes());
    bytes[56..72].copy_from_slice(&k.server);
    bytes[72..76].copy_from_slice(&k.session.to_be_bytes());
    bytes[76..80].copy_from_slice(&k.key_generation.to_be_bytes());
    bytes[80..84].copy_from_slice(&k.policy.to_be_bytes());
    bytes[84] = k.origin as u8;
    bytes[85] = 1;
    bytes[88..104].copy_from_slice(&key.request_digest);
    bytes[104..108].copy_from_slice(&status.to_be_bytes());
    bytes[108..110].copy_from_slice(
        &u16::try_from(payload.len())
            .map_err(|_| Error::Argument)?
            .to_be_bytes(),
    );
    bytes[110..size].copy_from_slice(payload);
    out[..size].copy_from_slice(&bytes[..size]);
    Ok(size)
}
/// Decode exact OPS1 without trusting a Record to authenticate itself.
/// 解码精确OPS1，不让Record自行宣称认证。
/// # Errors
/// Wrong lengths/schema/flags/key or incomplete security binding rejects.
#[allow(clippy::missing_panics_doc)] // All fixed field slices are dominated by the exact 110-byte length check.
pub fn decode_operation(
    b: &[u8],
    security: [u8; 16],
) -> Result<(OperationKey, OperationPhase, bool, i32, &[u8])> {
    if b.len() < 110
        || &b[..4] != b"OPS1"
        || u16::from_be_bytes([b[4], b[5]]) != 1
        || b[7] > 1
        || b[85] != 1
        || b[86..88] != [0, 0]
    {
        return Err(Error::Malformed);
    }
    let phase = match b[6] {
        1 => OperationPhase::Prepared,
        2 => OperationPhase::Executing,
        3 => OperationPhase::CommittedResult,
        4 => OperationPhase::AbortedNoEffect,
        5 => OperationPhase::InDoubt,
        6 => OperationPhase::Tombstoned,
        _ => return Err(Error::Malformed),
    };
    let size = usize::from(u16::from_be_bytes([b[108], b[109]]));
    if b.len() != 110 + size
        || (phase != OperationPhase::CommittedResult && size != 0)
        || (phase == OperationPhase::AbortedNoEffect && b[7] != 0)
    {
        return Err(Error::Malformed);
    }
    let u32at =
        |index| u32::from_be_bytes(b[index..index + 4].try_into().expect("checked OPS1 u32"));
    let u16at =
        |index| u16::from_be_bytes(b[index..index + 2].try_into().expect("checked OPS1 u16"));
    let mut client = [0; 16];
    client.copy_from_slice(&b[32..48]);
    let mut server = [0; 16];
    server.copy_from_slice(&b[56..72]);
    let mut digest = [0; 16];
    digest.copy_from_slice(&b[88..104]);
    let key = OperationKey {
        service: ServiceKey {
            realm: u32at(16),
            client,
            server,
            client_address: u32at(24),
            client_generation: u32at(28),
            server_address: u32at(48),
            server_generation: u32at(52),
            operation: u64::from_be_bytes(b[8..16].try_into().expect("checked OPS1 id")),
            service: u16at(20),
            opcode: u16at(22),
            session: u32at(72),
            key_generation: u32at(76),
            origin: OriginSecurity::try_from(b[84]).map_err(|_| Error::Malformed)?,
            policy: u32at(80),
            security,
        },
        request_digest: digest,
    };
    key.service.validate().map_err(|_| Error::Malformed)?;
    if digest == [0; 16] {
        return Err(Error::Malformed);
    }
    Ok((
        key,
        phase,
        b[7] != 0,
        i32::from_be_bytes(b[104..108].try_into().expect("checked OPS1 status")),
        &b[110..],
    ))
}
