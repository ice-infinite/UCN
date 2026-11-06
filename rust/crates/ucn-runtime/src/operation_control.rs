//! Authenticated Operation control routed by Coordinator, never by a business Owner.
//! Coordinator路由认证Operation控制，业务Owner不直调Persistence/Security。
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
    OperationKey, OperationOwner, OperationPhase, OperationReceipt, OperationWireKey,
    operation_result_digest,
};
use ucn_types::{Error, HopProfile, ProtocolOpcode, Result};

fn wire(key: OperationKey) -> OperationWireKey {
    OperationWireKey {
        operation: key.service.operation,
        service: key.service.service,
        opcode: key.service.opcode,
        request_digest: key.request_digest,
    }
}
fn require_receipt_storage<const BYTES: usize>(address_bytes: usize) -> Result<()> {
    // C0 prefix + fixed Receipt + O1/O2 Tag, direct H0 has no Hop tail.
    if BYTES < 25 + 2 * address_bytes + 48 + 16 {
        Err(Error::NoSpace)
    } else {
        Ok(())
    }
}
fn terminal(phase: Option<OperationPhase>) -> bool {
    matches!(
        phase,
        Some(
            OperationPhase::CommittedResult
                | OperationPhase::AbortedNoEffect
                | OperationPhase::InDoubt
                | OperationPhase::Tombstoned
        )
    )
}
fn cancel_target(phase: Option<OperationPhase>, handed_off: bool) -> Result<(OperationPhase, i32)> {
    match phase {
        Some(OperationPhase::Prepared) => {
            Ok((OperationPhase::AbortedNoEffect, Error::Cancelled.code()))
        }
        Some(OperationPhase::Executing) if !handed_off => {
            Ok((OperationPhase::AbortedNoEffect, Error::Cancelled.code()))
        }
        Some(OperationPhase::Executing) => Ok((OperationPhase::InDoubt, Error::InDoubt.code())),
        _ => Err(Error::State),
    }
}
pub(crate) fn current<const S: usize, const R: usize>(
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
        || generation != k.session
        || view.session_generation.get() != generation
        || key_generation != k.key_generation
        || policy != k.policy
        || origin != k.origin
        || view.origin_security != origin
        || view.hop_profile != HopProfile::H0
        || fingerprint != k.security
        || command.service().get() != k.service
    {
        return Err(Error::Access);
    }
    let opcode = match command.body() {
        ControlBody::OperationQuery(v) if v == wire(key) => ProtocolOpcode::OperationQuery,
        ControlBody::OperationCancel(v) if v.key == wire(key) => ProtocolOpcode::OperationCancel,
        ControlBody::OperationReceipt(v) if v.key == wire(key) => ProtocolOpcode::OperationReceipt,
        _ => return Err(Error::Access),
    };
    security.authorize(
        AccessRequest {
            session,
            context_fingerprint: Fingerprint::new(fingerprint)?,
            service: command.service(),
            protocol_opcode: u16::from(opcode),
            direction: AccessDirection::Inbound,
        },
        facts,
    )?;
    Ok(())
}

/// Opaque retained cancellation, not a proof or execution permission.
/// 不透明保留取消实例，不是Proof或执行权限。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationCancelHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// Query/terminal cancel reply or a Journal transition still requiring a proof.
/// 查询/终态取消回复，或仍需Journal证明的转换。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OperationDispatch {
    /// Confirmed observation reserved for protection; 已确认观察，待保护回复。
    Reply(ReplyHandle),
    /// Cancellation not yet durable; 取消尚未耐久。
    Pending(OperationCancelHandle),
}
struct Cancellation {
    command: ControlCommand,
    handle: ControlHandle,
    key: OperationKey,
    contract: DurableContract,
    last_now: u64,
    reply: ReplyHandle,
    expected: OperationReceipt,
    io: Option<PersistenceHandle>,
    confirmed: bool,
}
/// Fixed cancellation continuations; callback/Driver work is never hidden inside this Owner.
/// 固定取消Continuation，不隐藏回调或Driver工作。
pub struct OperationCoordinator<const PENDING: usize> {
    owner: u32,
    timeout: u64,
    slots: [Option<Cancellation>; PENDING],
    generations: [u32; PENDING],
    stopped: bool,
}
impl<const N: usize> OperationCoordinator<N> {
    /// Static composition with a non-renewing admission-relative timeout.
    /// 静态装配，超时从认证接纳计时，不续期。
    /// # Errors
    /// Zero/oversized capacity or duration rejects.
    pub const fn new(owner: u32, timeout_us: u64) -> Result<Self> {
        if owner == 0
            || N == 0
            || N > u16::MAX as usize
            || timeout_us == 0
            || timeout_us > i64::MAX as u64
        {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            timeout: timeout_us,
            slots: [const { None }; N],
            generations: [0; N],
            stopped: false,
        })
    }
    fn index(&self, h: OperationCancelHandle) -> Result<usize> {
        if h.owner != self.owner
            || h.slot >= N
            || h.generation != self.generations[h.slot]
            || self.slots[h.slot].is_none()
        {
            return Err(Error::State);
        }
        Ok(h.slot)
    }
    /// Current confirmation and exact outstanding Foundation handle; no caller-set durable flag.
    /// 当前确认及精确Foundation Handle，不接受调用方设置durable布尔值。
    /// # Errors
    /// Stale/cross-owner handle rejects.
    pub fn view(&self, h: OperationCancelHandle) -> Result<(bool, Option<PersistenceHandle>)> {
        let c = self.slots[self.index(h)?].as_ref().ok_or(Error::State)?;
        Ok((c.confirmed, c.io))
    }
    /// Inspect the blocked holder; possession does not permit early sealing/submission.
    /// 查看阻塞Holder，持有Handle不允许提前保护/提交。
    /// # Errors
    /// Stale/cross-owner handle rejects.
    pub fn pending_reply(&self, h: OperationCancelHandle) -> Result<ReplyHandle> {
        Ok(self.slots[self.index(h)?]
            .as_ref()
            .ok_or(Error::State)?
            .reply)
    }
    /// Reserve reply before changing Journal; retain the command until reload and response publish.
    /// 改Journal前预留回复，回读和回复发布前保留命令。
    /// # Errors
    /// Wrong full identity/current ACL, nonterminal Query, capacity or deadline rejects atomically.
    #[allow(clippy::too_many_arguments)]
    pub fn dispatch<
        const Q: usize,
        const B: usize,
        const S: usize,
        const R: usize,
        const O: usize,
        const C: usize,
        const F: usize,
    >(
        &mut self,
        queue: &mut ControlCoordinator<Q, B>,
        h: ControlHandle,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        operation: &mut OperationOwner<O>,
        base: DurableBase,
        replies: &mut ControlReplies<C, F>,
        codec: &mut CodecWorkspace,
        digest: &mut DigestWorkspace,
    ) -> Result<OperationDispatch> {
        if self.stopped {
            return Err(Error::State);
        }
        let command = queue.view(h)?;
        if !matches!(
            command.body(),
            ControlBody::OperationQuery(_) | ControlBody::OperationCancel(_)
        ) {
            return Err(Error::Unsupported);
        }
        let key = operation.key().ok_or(Error::State)?;
        current(&command, key, security, session, facts, false)?;
        require_receipt_storage::<F>(security.address_width().bytes())?;
        let expires = command
            .accepted_at()
            .checked_add(self.timeout)
            .ok_or(Error::Exhausted)?;
        if facts.now_us >= expires {
            return Err(Error::Timeout);
        }
        if let Some(slot) = self.slots.iter().position(|c| {
            c.as_ref()
                .is_some_and(|c| c.handle == h && c.command == command)
        }) {
            return Ok(OperationDispatch::Pending(OperationCancelHandle {
                owner: self.owner,
                slot,
                generation: self.generations[slot],
            }));
        }
        let (phase, pending, handed_off) = operation.view();
        if pending {
            return Err(Error::State);
        }
        if matches!(command.body(), ControlBody::OperationQuery(_)) || terminal(phase) {
            let receipt = operation.receipt(digest)?;
            let reply = replies.reserve(
                command.into(),
                ControlBody::OperationReceipt(receipt),
                facts.now_us,
                expires,
            )?;
            queue.ack(h)?;
            return Ok(OperationDispatch::Reply(reply));
        }
        if !matches!(command.body(), ControlBody::OperationCancel(_)) {
            return Err(Error::Unsupported);
        }
        let (next, status) = cancel_target(phase, handed_off)?;
        let slot = self
            .slots
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let expected = OperationReceipt {
            key: wire(key),
            phase: next as u8,
            result_digest: operation_result_digest(wire(key), next as u8, status, &[], digest)?,
        };
        let reply = replies.reserve_pending(
            command.into(),
            ControlBody::OperationReceipt(expected),
            facts.now_us,
            expires,
        )?;
        if let Err(error) = operation.transition(
            next,
            base,
            status,
            &[],
            facts.now_us,
            expires,
            generation,
            codec,
        ) {
            replies.abandon_unpublished(reply)?;
            return Err(error);
        }
        self.slots[slot] = Some(Cancellation {
            command,
            handle: h,
            key,
            contract: operation.contract(),
            last_now: facts.now_us,
            reply,
            expected,
            io: None,
            confirmed: false,
        });
        self.generations[slot] = generation;
        Ok(OperationDispatch::Pending(OperationCancelHandle {
            owner: self.owner,
            slot,
            generation,
        }))
    }
    /// Route the exact immutable requirement; submit has no Provider callback.
    /// 路由精确不变Requirement；submit不执行Provider回调。
    /// # Errors
    /// Wrong Owner, duplicate submit, Foundation admission or bind failure retains obligations.
    pub fn submit<const O: usize, const D: usize, const P: usize, const S: usize>(
        &mut self,
        h: OperationCancelHandle,
        operation: &mut OperationOwner<O>,
        persistence: &mut PersistenceOwner<'_, D, P, S>,
        now: u64,
    ) -> Result<PersistenceHandle> {
        let slot = self.index(h)?;
        let c = self.slots[slot].as_mut().ok_or(Error::State)?;
        if operation.key() != Some(c.key)
            || operation.contract() != c.contract
            || c.io.is_some()
            || c.confirmed
            || now < c.last_now
        {
            return Err(Error::State);
        }
        let io = persistence.submit(&operation.requirement()?, now)?;
        c.io = Some(io); // Retain the accepted Foundation obligation even if bind detects corruption.
        operation.bind(io)?;
        c.last_now = now;
        Ok(io)
    }
    /// Confirm actual reload, retire Proof, reauthorize, then unblock the reserved response.
    /// 确认实际回读、退休Proof、再授权，最后解锁已预留回复。
    /// # Errors
    /// Missing/wrong proof, timeout or changed security retains the command and blocked reply.
    #[allow(clippy::too_many_arguments)]
    pub fn finish<
        const Q: usize,
        const B: usize,
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
        h: OperationCancelHandle,
        queue: &mut ControlCoordinator<Q, B>,
        operation: &mut OperationOwner<O>,
        persistence: &mut PersistenceOwner<'_, D, P, L>,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        replies: &mut ControlReplies<C, F>,
        digest: &mut DigestWorkspace,
    ) -> Result<ReplyHandle> {
        let slot = self.index(h)?;
        let c = self.slots[slot].as_mut().ok_or(Error::State)?;
        if operation.key() != Some(c.key)
            || operation.contract() != c.contract
            || queue.view(c.handle)? != c.command
            || facts.now_us < c.last_now
        {
            return Err(Error::State);
        }
        c.last_now = facts.now_us;
        if let Some(io) = c.io {
            if !c.confirmed {
                operation.confirm(io, persistence.proof_get(io)?)?;
                c.confirmed = true;
            }
            persistence.proof_retire(io)?;
            c.io = None;
        }
        if !c.confirmed {
            return Err(Error::State);
        }
        current(&c.command, c.key, security, session, facts, false)?;
        if facts.now_us >= replies.view(c.reply)?.expires_at {
            return Err(Error::Timeout);
        }
        let receipt = operation.receipt(digest)?;
        if receipt != c.expected {
            return Err(Error::State);
        }
        replies.publish_committed(c.reply, ControlBody::OperationReceipt(receipt))?;
        queue.ack(c.handle)?;
        let reply = c.reply;
        self.slots[slot] = None;
        Ok(reply)
    }
    /// Explicitly reject a confirmed response after timeout/revocation; cannot forget pending I/O.
    /// 超时/撤权后显式拒绝已确认回复，不能遗忘未决I/O。
    /// # Errors
    /// Unconfirmed/held proof or changed command keeps all obligations.
    pub fn reject_confirmed<const Q: usize, const B: usize, const C: usize, const F: usize>(
        &mut self,
        h: OperationCancelHandle,
        queue: &mut ControlCoordinator<Q, B>,
        replies: &mut ControlReplies<C, F>,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let c = self.slots[slot].as_ref().ok_or(Error::State)?;
        if !c.confirmed || c.io.is_some() || queue.view(c.handle)? != c.command {
            return Err(Error::State);
        }
        replies.abandon_unpublished(c.reply)?;
        queue.ack(c.handle)?;
        self.slots[slot] = None;
        Ok(())
    }
    /// Stop new dispatch; accepted safe Journal retirement remains possible.
    /// 停止新分派，已接纳安全Journal退休仍可继续。
    pub fn stop(&mut self) {
        self.stopped = true;
    }
    /// Retire a verified Foundation failure without publishing success or un-fencing the Journal.
    /// 退休已核对Foundation失败，不发布成功，也不解除Journal围栏。
    /// # Errors
    /// Pending/proven-success I/O, wrong full Owner/command or changed blocked reply rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn retire_failed<
        const Q: usize,
        const B: usize,
        const O: usize,
        const D: usize,
        const P: usize,
        const S: usize,
        const C: usize,
        const F: usize,
    >(
        &mut self,
        h: OperationCancelHandle,
        queue: &mut ControlCoordinator<Q, B>,
        operation: &mut OperationOwner<O>,
        persistence: &mut PersistenceOwner<'_, D, P, S>,
        replies: &mut ControlReplies<C, F>,
    ) -> Result<Error> {
        let slot = self.index(h)?;
        let c = self.slots[slot].as_ref().ok_or(Error::State)?;
        if c.confirmed
            || operation.key() != Some(c.key)
            || operation.contract() != c.contract
            || queue.view(c.handle)? != c.command
            || replies.view(c.reply)?.phase != crate::ReplyPhase::AwaitCommit
        {
            return Err(Error::State);
        }
        let io = c.io.ok_or(Error::State)?;
        let failed = persistence.request_get(io)?;
        if failed.state != RequestState::Failed {
            return Err(Error::State);
        }
        let error = failed.terminal_error.ok_or(Error::State)?;
        // Exclusive borrows; no callback or intervening foreign mutation after preflight.
        operation.fence_pending(io)?;
        persistence.failed_retire(io)?;
        replies.abandon_unpublished(c.reply)?;
        queue.ack(c.handle)?;
        self.slots[slot] = None;
        Ok(error)
    }
}

/// One bounded client observation, not an application result or durable execution certificate.
/// 一次有界Client观察，不是业务结果或耐久执行证书。
pub struct OperationWatch {
    key: OperationKey,
    accepted: u64,
    deadline: u64,
    receipt: Option<OperationReceipt>,
}
impl OperationWatch {
    /// Register exact outstanding Query/Cancel identity before sending.
    /// 发送前登记精确Query/Cancel身份。
    /// # Errors
    /// Invalid key or non-half-open lifetime rejects.
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
        })
    }
    /// Authenticated point-in-time observation only; 仅认证时点观察。
    #[must_use]
    pub const fn receipt(&self) -> Option<OperationReceipt> {
        self.receipt
    }
    /// Check separately obtained result bytes against the authenticated observation.
    /// 核对单独获得的结果字节与认证观察，不凭48B回执捏造业务正文。
    /// # Errors
    /// No observation, wrong phase/status/bytes or digest rejects.
    pub fn verify_result(
        &self,
        status: i32,
        payload: &[u8],
        digest: &mut DigestWorkspace,
    ) -> Result<()> {
        let receipt = self.receipt.ok_or(Error::State)?;
        if operation_result_digest(receipt.key, receipt.phase, status, payload, digest)?
            != receipt.result_digest
        {
            return Err(Error::Security);
        }
        Ok(())
    }
}
impl<const Q: usize, const B: usize> ControlCoordinator<Q, B> {
    /// Match current final server and full frozen request before retaining an observation.
    /// 保留观察前核对当前最终Server及完整冻结请求。
    /// # Errors
    /// Wrong full domain, timeout or conflicting receipt preserves command and Watch.
    pub fn dispatch_operation_receipt<const S: usize, const R: usize>(
        &mut self,
        h: ControlHandle,
        watch: &mut OperationWatch,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
    ) -> Result<()> {
        let command = self.view(h)?;
        current(&command, watch.key, security, session, facts, true)?;
        if command.accepted_at() < watch.accepted || facts.now_us >= watch.deadline {
            return Err(Error::Timeout);
        }
        let ControlBody::OperationReceipt(receipt) = command.body() else {
            return Err(Error::Unsupported);
        };
        if watch.receipt.is_some_and(|old| old != receipt) {
            return Err(Error::Replay);
        }
        watch.receipt = Some(receipt);
        self.ack(h)
    }
}

#[cfg(test)]
mod tests {
    use super::require_receipt_storage;
    use ucn_types::Error;
    #[test]
    fn receipt_storage_preflight_includes_address_width_and_origin_tag() {
        assert!(require_receipt_storage::<93>(2).is_ok());
        assert_eq!(require_receipt_storage::<92>(2), Err(Error::NoSpace));
        assert_eq!(require_receipt_storage::<96>(4), Err(Error::NoSpace));
        assert!(require_receipt_storage::<97>(4).is_ok());
    }
}
