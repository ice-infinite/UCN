//! Actual durable Data request and executor handoff; no Owner-to-Owner callbacks.
//! 实际耐久Data请求与执行器交接；跨Owner编排只由Coordinator执行。
use crate::{RpcBridge, RpcHandle};
use ucn_digest::{DigestWorkspace, canonical_begin};
use ucn_durable::{DurableBase, DurableContract};
use ucn_owner::{CallbackClaim, CallbackGate};
use ucn_persistence::{
    CodecWorkspace, DomainState, PersistenceHandle, PersistenceOwner, RequestState,
};
use ucn_security::{CurrentFacts, SecurityOwner, SessionHandle};
use ucn_service::{OperationKey, OperationOwner, OperationPhase, ServiceOwner};
use ucn_types::{Error, InteractionRole, OPERATION_DURABLE_AT_MOST_ONCE, Result};

/// Bounded external execution, not a Journal or authorization Provider.
/// 有界外部执行接口，不是Journal或授权Provider。
pub trait DurableExecutor {
    /// Called once after EXECUTING reload. A signed business failure is still a complete result.
    /// EXECUTING回读后调用一次，负的业务错误仍是完整结果。
    /// # Errors
    /// `Err` means execution outcome cannot be proved, not a routine business failure.
    fn execute(
        &mut self,
        key: OperationKey,
        arguments: &[u8],
        result: &mut [u8],
    ) -> Result<(i32, usize)>;
}
/// Opaque Coordinator work handle; 不透明 Coordinator 工作 Handle。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DurableRpcHandle {
    owner: u32,
    generation: u32,
}
/// Durable continuation phase, separate from physical RPC transmission.
/// 耐久Continuation阶段，独立于物理RPC发送。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DurableRpcPhase {
    /// PREPARED awaiting reload; PREPARED待回读。
    PreparedPending,
    /// PREPARED confirmed, not permission to execute; PREPARED已确认，不是执行权限。
    Prepared,
    /// EXECUTING awaiting reload; EXECUTING待回读。
    ExecutingPending,
    /// EXECUTING confirmed, requires current authorization; EXECUTING已确认，仍需当前授权。
    ReadyToExecute,
    /// Execution observed or recovery uncertain; 执行已观察或恢复结果不确定。
    Observed,
    /// Terminal transition awaiting reload; 终态待回读。
    ResultPending,
    /// Terminal confirmed, not yet published; 终态已确认，尚未发布。
    ResultReady,
    /// Immutable RPC result handed to protection pipeline; 不变RPC结果已交给保护链。
    Published,
    /// Persistence failure permanently fences execution; 持久化失败围栏执行。
    Faulted,
}
struct Work {
    rpc: RpcHandle,
    key: OperationKey,
    contract: DurableContract,
    last_now: u64,
    phase: DurableRpcPhase,
    target: OperationPhase,
    io: Option<PersistenceHandle>,
    confirmed: bool,
    status: i32,
    bytes: usize,
    journal_generation: u64,
    journal_digest: [u8; 16],
}
/// One fixed Journal continuation and pre-reserved argument/result buffers.
/// 一份固定Journal Continuation和预留参数/结果Buffer，无动态槽扩张。
pub struct DurableRpcCoordinator<const ARGUMENTS: usize, const RESULT_BYTES: usize> {
    owner: u32,
    generation: u32,
    io_timeout: u64,
    work: Option<Work>,
    arguments: [u8; ARGUMENTS],
    argument_bytes: usize,
    result: [u8; RESULT_BYTES],
    stopped: bool,
}
fn base<const D: usize, const P: usize, const S: usize>(
    persistence: &PersistenceOwner<'_, D, P, S>,
    contract: DurableContract,
    scratch: &mut CodecWorkspace,
) -> Result<DurableBase> {
    let mut body = [0; 256];
    let n = persistence.copy_body(contract.domain, &mut body)?;
    DurableBase::from_view(
        contract,
        persistence.domain_get(contract.domain)?,
        &body[..n],
        scratch,
    )
}
fn arguments_digest(
    key: OperationKey,
    body: &[u8],
    scratch: &mut DigestWorkspace,
) -> Result<[u8; 16]> {
    let mut envelope = [0; 9];
    envelope[..8].copy_from_slice(&key.service.operation.to_be_bytes());
    envelope[8] = OPERATION_DURABLE_AT_MOST_ONCE;
    let mut digest = canonical_begin(
        b"UCN6-PAYLOAD-V1",
        u32::try_from(9usize.checked_add(body.len()).ok_or(Error::Argument)?)
            .map_err(|_| Error::Argument)?,
        scratch,
    )?;
    digest.append(&envelope)?;
    digest.append(body)?;
    digest.finish()
}
impl<const A: usize, const R: usize> DurableRpcCoordinator<A, R> {
    /// Static single-Journal composition; caller may install multiple independently bounded instances.
    /// 静态单Journal装配，可安装多份独立有界实例。
    /// # Errors
    /// Invalid fixed capacity or I/O timeout rejects.
    pub const fn new(owner: u32, io_timeout_us: u64) -> Result<Self> {
        if owner == 0
            || A == 0
            || R == 0
            || R > 146
            || io_timeout_us == 0
            || io_timeout_us > i64::MAX as u64
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            generation: 0,
            io_timeout: io_timeout_us,
            work: None,
            arguments: [0; A],
            argument_bytes: 0,
            result: [0; R],
            stopped: false,
        })
    }
    fn get(&self, h: DurableRpcHandle) -> Result<&Work> {
        if h.owner != self.owner || h.generation != self.generation {
            return Err(Error::State);
        }
        self.work.as_ref().ok_or(Error::State)
    }
    fn exact(&self, h: DurableRpcHandle, operation: &OperationOwner<R>, now: u64) -> Result<()> {
        let w = self.get(h)?;
        if operation.key() != Some(w.key)
            || operation.contract() != w.contract
            || now < w.last_now
            || operation.record_generation() != w.journal_generation
            || operation.record_digest() != w.journal_digest
        {
            return Err(Error::State);
        }
        Ok(())
    }
    fn current_journal<const D: usize, const P: usize, const S: usize>(
        &self,
        h: DurableRpcHandle,
        persistence: &PersistenceOwner<'_, D, P, S>,
    ) -> Result<()> {
        let w = self.get(h)?;
        let current = persistence.domain_get(w.contract.domain)?;
        if current.state != DomainState::Ready
            || current.pending
            || current.schema_id != w.contract.schema
            || current.schema_version != w.contract.version
            || current.record_generation != w.journal_generation
            || current.body_digest != w.journal_digest
        {
            return Err(Error::State);
        }
        Ok(())
    }
    /// Inspect one exact continuation; 查看一份精确Continuation。
    /// # Errors
    /// Cross-owner/stale/retired work rejects.
    pub fn view(&self, h: DurableRpcHandle) -> Result<DurableRpcPhase> {
        Ok(self.get(h)?.phase)
    }
    /// Exact retained Foundation obligation; 精确保留的Foundation义务。
    /// # Errors
    /// Stale/cross-owner handle rejects.
    pub fn pending_io(&self, h: DurableRpcHandle) -> Result<Option<PersistenceHandle>> {
        Ok(self.get(h)?.io)
    }
    /// Bind actual authenticated arguments, reserve all buffers, then freeze PREPARED.
    /// 绑定实际认证参数，预留全部Buffer，最后冻结PREPARED。
    /// # Errors
    /// Wrong arguments/key/Journal/capacity/context never invokes the executor or writes Provider.
    #[allow(clippy::too_many_arguments)]
    pub fn prepare<
        const N: usize,
        const F: usize,
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
    >(
        &mut self,
        rpc: &RpcBridge<N, F>,
        handle: RpcHandle,
        arguments: &[u8],
        operation: &mut OperationOwner<R>,
        persistence: &PersistenceOwner<'_, D, P, L>,
        security: &SecurityOwner<'_, S, Q>,
        session: SessionHandle,
        facts: CurrentFacts,
        codec: &mut CodecWorkspace,
        digest: &mut DigestWorkspace,
    ) -> Result<DurableRpcHandle> {
        if self.stopped || self.work.is_some() || arguments.len() > A {
            return Err(Error::State);
        }
        let (key, _, result_capacity) = rpc.durable_key(handle, security, session, facts)?;
        if R > result_capacity {
            return Err(Error::Config);
        }
        let terminal_lookup = arguments.is_empty()
            && operation.key() == Some(key)
            && !operation.view().1
            && matches!(
                operation.view().0,
                Some(
                    OperationPhase::CommittedResult
                        | OperationPhase::AbortedNoEffect
                        | OperationPhase::InDoubt
                )
            );
        if !terminal_lookup && arguments_digest(key, arguments, digest)? != key.request_digest {
            return Err(Error::Security);
        }
        let generation = self.generation.checked_add(1).ok_or(Error::Exhausted)?;
        let baseline = base(persistence, operation.contract(), codec)?;
        if baseline.generation() != operation.record_generation()
            || baseline.record_digest() != operation.record_digest()
        {
            return Err(Error::State);
        }
        let (phase, target, status, bytes) = if let Some(existing) = operation.key() {
            if existing != key || operation.view().1 {
                return Err(Error::Replay);
            }
            match operation.view().0 {
                Some(OperationPhase::Prepared) => {
                    (DurableRpcPhase::Prepared, OperationPhase::Prepared, 0, 0)
                }
                Some(OperationPhase::Executing) if operation.view().2 => (
                    DurableRpcPhase::Observed,
                    OperationPhase::InDoubt,
                    Error::InDoubt.code(),
                    0,
                ),
                Some(OperationPhase::Executing) => (
                    DurableRpcPhase::ReadyToExecute,
                    OperationPhase::Executing,
                    0,
                    0,
                ),
                Some(
                    OperationPhase::CommittedResult
                    | OperationPhase::AbortedNoEffect
                    | OperationPhase::InDoubt,
                ) => {
                    let (target, status, n) = operation.copy_result(&mut self.result)?;
                    (DurableRpcPhase::ResultReady, target, status, n)
                }
                _ => return Err(Error::State),
            }
        } else {
            operation.begin(
                key,
                baseline,
                facts.now_us,
                facts
                    .now_us
                    .checked_add(self.io_timeout)
                    .ok_or(Error::Exhausted)?,
                generation,
                codec,
            )?;
            (
                DurableRpcPhase::PreparedPending,
                OperationPhase::Prepared,
                0,
                0,
            )
        };
        self.arguments[..arguments.len()].copy_from_slice(arguments);
        self.argument_bytes = arguments.len();
        self.work = Some(Work {
            rpc: handle,
            key,
            contract: operation.contract(),
            last_now: facts.now_us,
            phase,
            target,
            io: None,
            confirmed: false,
            status,
            bytes,
            journal_generation: baseline.generation(),
            journal_digest: baseline.record_digest(),
        });
        self.generation = generation;
        Ok(DurableRpcHandle {
            owner: self.owner,
            generation,
        })
    }
    /// Submit the frozen Journal requirement; Provider I/O remains in Foundation step.
    /// 提交冻结Journal要求，Provider I/O仍由Foundation step推进。
    /// # Errors
    /// Bind/submit failure retains every accepted Foundation obligation.
    pub fn submit<const D: usize, const P: usize, const S: usize>(
        &mut self,
        h: DurableRpcHandle,
        operation: &mut OperationOwner<R>,
        persistence: &mut PersistenceOwner<'_, D, P, S>,
        now: u64,
    ) -> Result<PersistenceHandle> {
        self.exact(h, operation, now)?;
        let w = self.work.as_mut().ok_or(Error::State)?;
        if w.io.is_some()
            || w.confirmed
            || !matches!(
                w.phase,
                DurableRpcPhase::PreparedPending
                    | DurableRpcPhase::ExecutingPending
                    | DurableRpcPhase::ResultPending
            )
        {
            return Err(Error::State);
        }
        let io = persistence.submit(&operation.requirement()?, now)?;
        w.io = Some(io);
        operation.bind(io)?;
        w.last_now = now;
        Ok(io)
    }
    /// Confirm exact actual reload, then retire its proof before publishing the local next phase.
    /// 确认精确实际回读，退休Proof后才发布本地下一阶段。
    /// # Errors
    /// Missing/mismatched proof never grants execution or result publication.
    pub fn finish_write<const D: usize, const P: usize, const S: usize>(
        &mut self,
        h: DurableRpcHandle,
        operation: &mut OperationOwner<R>,
        persistence: &mut PersistenceOwner<'_, D, P, S>,
        now: u64,
    ) -> Result<()> {
        self.exact(h, operation, now)?;
        let w = self.work.as_mut().ok_or(Error::State)?;
        let io = w.io.ok_or(Error::State)?;
        if !w.confirmed {
            operation.confirm(io, persistence.proof_get(io)?)?;
            w.confirmed = true;
            w.journal_generation = operation.record_generation();
            w.journal_digest = operation.record_digest();
        }
        if operation.view().0 != Some(w.target) {
            return Err(Error::State);
        }
        persistence.proof_retire(io)?;
        w.io = None;
        w.confirmed = false;
        w.last_now = now;
        w.phase = match w.target {
            OperationPhase::Prepared => DurableRpcPhase::Prepared,
            OperationPhase::Executing => DurableRpcPhase::ReadyToExecute,
            _ => DurableRpcPhase::ResultReady,
        };
        Ok(())
    }
    /// Reauthorize after PREPARED, then freeze EXECUTING; no executor call here.
    /// PREPARED后重新授权，再冻结EXECUTING，此处不调用执行器。
    /// # Errors
    /// Expiry/revocation/stale request or baseline cannot arm execution.
    #[allow(clippy::too_many_arguments)]
    pub fn arm<
        const N: usize,
        const F: usize,
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
    >(
        &mut self,
        h: DurableRpcHandle,
        rpc: &RpcBridge<N, F>,
        operation: &mut OperationOwner<R>,
        persistence: &PersistenceOwner<'_, D, P, L>,
        security: &SecurityOwner<'_, S, Q>,
        session: SessionHandle,
        facts: CurrentFacts,
        codec: &mut CodecWorkspace,
    ) -> Result<()> {
        self.exact(h, operation, facts.now_us)?;
        let w = self.get(h)?;
        if self.stopped
            || w.phase != DurableRpcPhase::Prepared
            || w.io.is_some()
            || rpc.durable_key(w.rpc, security, session, facts)?.0 != w.key
        {
            return Err(Error::State);
        }
        operation.transition(
            OperationPhase::Executing,
            base(persistence, w.contract, codec)?,
            0,
            &[],
            facts.now_us,
            facts
                .now_us
                .checked_add(self.io_timeout)
                .ok_or(Error::Exhausted)?,
            h.generation,
            codec,
        )?;
        let w = self.work.as_mut().ok_or(Error::State)?;
        w.target = OperationPhase::Executing;
        w.phase = DurableRpcPhase::ExecutingPending;
        w.last_now = facts.now_us;
        Ok(())
    }
    /// Invoke once under the shared gate after EXECUTING proof and current authentication.
    /// EXECUTING证明和当前认证后，在共享回调门内调用一次。
    /// # Errors
    /// No proof/expired context/busy gate does not invoke; uncertain Provider outcome fences future work.
    #[allow(clippy::too_many_arguments)]
    pub fn execute<
        const N: usize,
        const F: usize,
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
    >(
        &mut self,
        h: DurableRpcHandle,
        rpc: &RpcBridge<N, F>,
        operation: &mut OperationOwner<R>,
        persistence: &PersistenceOwner<'_, D, P, L>,
        security: &SecurityOwner<'_, S, Q>,
        session: SessionHandle,
        facts: CurrentFacts,
        gate: &CallbackGate,
        executor: &mut impl DurableExecutor,
    ) -> Result<()> {
        self.exact(h, operation, facts.now_us)?;
        self.current_journal(h, persistence)?;
        let w = self.get(h)?;
        if self.stopped
            || w.phase != DurableRpcPhase::ReadyToExecute
            || w.io.is_some()
            || rpc.durable_key(w.rpc, security, session, facts)?.0 != w.key
        {
            return Err(Error::State);
        }
        let lease = gate.try_enter(CallbackClaim::new(self.owner, h.generation, 1, 0x0701)?)?;
        let permit = match operation.handoff(w.key) {
            Ok(permit) => permit,
            Err(error) => {
                gate.leave(lease)?;
                return Err(error);
            }
        };
        let outcome = executor.execute(
            permit.key(),
            &self.arguments[..self.argument_bytes],
            &mut self.result,
        );
        let left = gate.leave(lease);
        let (target, status, bytes) = match outcome {
            Ok((status, n))
                if left.is_ok()
                    && n <= R
                    && (-65536..=65535).contains(&status)
                    && status != -1 - i32::from(ucn_types::DURABLE_RPC_NO_EFFECT)
                    && status != -1 - i32::from(ucn_types::DURABLE_RPC_IN_DOUBT) =>
            {
                (OperationPhase::CommittedResult, status, n)
            }
            _ => {
                self.stopped = true;
                self.result.fill(0);
                (OperationPhase::InDoubt, Error::InDoubt.code(), 0)
            }
        };
        operation.executor_result(permit, status, &self.result[..bytes])?;
        let w = self.work.as_mut().ok_or(Error::State)?;
        w.target = target;
        w.status = status;
        w.bytes = bytes;
        w.phase = DurableRpcPhase::Observed;
        w.last_now = facts.now_us;
        Ok(())
    }
    /// Prove no handoff and request a durable no-effect rejection; 证明未交接，再请求耐久无副作用拒绝。
    /// # Errors
    /// Pending/handoff/terminal work cannot be called no-effect.
    pub fn abandon_before_execution(
        &mut self,
        h: DurableRpcHandle,
        operation: &OperationOwner<R>,
        now: u64,
    ) -> Result<()> {
        self.exact(h, operation, now)?;
        let w = self.work.as_mut().ok_or(Error::State)?;
        if w.io.is_some()
            || operation.view().1
            || operation.view().2
            || !matches!(
                w.phase,
                DurableRpcPhase::Prepared | DurableRpcPhase::ReadyToExecute
            )
        {
            return Err(Error::State);
        }
        w.target = OperationPhase::AbortedNoEffect;
        w.status = Error::Cancelled.code();
        w.bytes = 0;
        w.phase = DurableRpcPhase::Observed;
        w.last_now = now;
        Ok(())
    }
    /// Persist known facts even after request timeout/stop; never grants renewed authority.
    /// 请求超时/停止后仍可持久化已知事实，不签发新权限。
    /// # Errors
    /// Wrong phase/Journal/baseline preserves the observed outcome for retry.
    pub fn persist_outcome<const D: usize, const P: usize, const S: usize>(
        &mut self,
        h: DurableRpcHandle,
        operation: &mut OperationOwner<R>,
        persistence: &PersistenceOwner<'_, D, P, S>,
        now: u64,
        codec: &mut CodecWorkspace,
    ) -> Result<()> {
        self.exact(h, operation, now)?;
        let w = self.get(h)?;
        if w.phase != DurableRpcPhase::Observed || w.io.is_some() {
            return Err(Error::State);
        }
        operation.transition(
            w.target,
            base(persistence, w.contract, codec)?,
            w.status,
            &self.result[..w.bytes],
            now,
            now.checked_add(self.io_timeout).ok_or(Error::Exhausted)?,
            h.generation,
            codec,
        )?;
        let w = self.work.as_mut().ok_or(Error::State)?;
        w.phase = DurableRpcPhase::ResultPending;
        w.last_now = now;
        Ok(())
    }
    /// Publish only the confirmed terminal body, rechecking live RPC permission and original deadline.
    /// 仅发布确认后的终态正文，复验当前RPC权限和原截止。
    /// # Errors
    /// Unconfirmed/revoked/expired result is retained, not exposed or re-executed.
    #[allow(clippy::too_many_arguments)]
    pub fn publish<
        const N: usize,
        const F: usize,
        const C: usize,
        const K: usize,
        const B: usize,
        const S: usize,
        const Q: usize,
        const D: usize,
        const P: usize,
        const L: usize,
    >(
        &mut self,
        h: DurableRpcHandle,
        rpc: &mut RpcBridge<N, F>,
        operation: &OperationOwner<R>,
        persistence: &PersistenceOwner<'_, D, P, L>,
        service: &mut ServiceOwner<C, K, B>,
        security: &SecurityOwner<'_, S, Q>,
        session: SessionHandle,
        facts: CurrentFacts,
    ) -> Result<RpcHandle> {
        self.exact(h, operation, facts.now_us)?;
        self.current_journal(h, persistence)?;
        let w = self.get(h)?;
        if w.phase != DurableRpcPhase::ResultReady
            || w.io.is_some()
            || rpc.durable_key(w.rpc, security, session, facts)?.0 != w.key
        {
            return Err(Error::State);
        }
        let rpc_handle = w.rpc;
        let (phase, status, n) = operation.copy_result(&mut self.result)?;
        if !matches!(
            phase,
            OperationPhase::CommittedResult
                | OperationPhase::AbortedNoEffect
                | OperationPhase::InDoubt
        ) {
            return Err(Error::State);
        }
        let (role, code) = if phase == OperationPhase::InDoubt {
            (InteractionRole::Error, Ok(ucn_types::DURABLE_RPC_IN_DOUBT))
        } else if phase == OperationPhase::AbortedNoEffect {
            (InteractionRole::Error, Ok(ucn_types::DURABLE_RPC_NO_EFFECT))
        } else if status < 0 {
            (InteractionRole::Error, u16::try_from(-1 - status))
        } else {
            (InteractionRole::Result, u16::try_from(status))
        };
        rpc.respond_inner(
            rpc_handle,
            service,
            security,
            session,
            facts,
            role,
            code.map_err(|_| Error::State)?,
            &self.result[..n],
        )?;
        let w = self.work.as_mut().ok_or(Error::State)?;
        w.phase = DurableRpcPhase::Published;
        w.last_now = facts.now_us;
        Ok(rpc_handle)
    }
    /// Fence before retiring a failed Foundation request; 失败Foundation请求退休前先围栏。
    /// # Errors
    /// Still pending/proven-success/wrong Journal retains obligations.
    pub fn retire_failed<const D: usize, const P: usize, const S: usize>(
        &mut self,
        h: DurableRpcHandle,
        operation: &mut OperationOwner<R>,
        persistence: &mut PersistenceOwner<'_, D, P, S>,
        now: u64,
    ) -> Result<Error> {
        self.exact(h, operation, now)?;
        let w = self.get(h)?;
        let io = w.io.ok_or(Error::State)?;
        if w.confirmed {
            return Err(Error::State);
        }
        let failed = persistence.request_get(io)?;
        if failed.state != RequestState::Failed {
            return Err(Error::State);
        }
        let error = failed.terminal_error.ok_or(Error::State)?;
        operation.fence_pending(io)?;
        persistence.failed_retire(io)?;
        let w = self.work.as_mut().ok_or(Error::State)?;
        w.io = None;
        w.phase = DurableRpcPhase::Faulted;
        w.last_now = now;
        self.stopped = true;
        Ok(error)
    }
    /// Stop future execution, preserving writes/results/physical RPC obligations.
    /// 停止未来执行，保留写入、结果和独立物理RPC义务。
    pub fn stop(&mut self) {
        self.stopped = true;
    }
    /// Retire only confirmed terminal/fault continuation; never erases Journal or RPC Holder.
    /// 仅退休已确认终态/故障Continuation，不删除Journal或RPC Holder。
    /// # Errors
    /// Nonterminal or outstanding Foundation obligation cannot be forgotten.
    pub fn retire(
        &mut self,
        h: DurableRpcHandle,
        operation: &OperationOwner<R>,
        now: u64,
    ) -> Result<()> {
        self.exact(h, operation, now)?;
        let w = self.get(h)?;
        if w.io.is_some()
            || !matches!(
                w.phase,
                DurableRpcPhase::ResultReady
                    | DurableRpcPhase::Published
                    | DurableRpcPhase::Faulted
            )
        {
            return Err(Error::State);
        }
        self.work = None;
        self.arguments.fill(0);
        self.argument_bytes = 0;
        self.result.fill(0);
        Ok(())
    }
}
