//! Coordinator dispatch, never Transport -> Security/Adapter calls.
//! Coordinator负责分派，Transport不调用Security或Adapter。
use crate::{ControlBody, ControlCoordinator, ControlHandle, ControlReplies, ReplyHandle};
use ucn_digest::DigestWorkspace;
use ucn_security::{
    AccessDirection, AccessRequest, CurrentFacts, Fingerprint, PacketEvidence, SecurityOwner,
    SessionHandle,
};
use ucn_transport::{
    ParentKind, ParentOwner, SetupAck, TransferFeedback, TransferHandle, TransferKey,
    TransferOwner, TransferParent,
};
use ucn_types::{Error, HopProfile, Result};

fn binding(b: ucn_security::Binding) -> ucn_transport::Binding {
    ucn_transport::Binding {
        principal: b.principal,
        address: b.address.get(),
        generation: b.generation.get(),
    }
}
pub(crate) fn current_parent<const S: usize, const R: usize>(
    p: TransferParent,
    evidence: PacketEvidence,
    security: &SecurityOwner<'_, S, R>,
    session: SessionHandle,
    facts: CurrentFacts,
    reverse: bool,
) -> Result<()> {
    p.validate()?;
    if p.kind != ParentKind::C1 {
        return Err(Error::Unsupported);
    }
    let (realm, source, destination) = evidence.identities();
    let (generation, key, policy, origin, fingerprint) = evidence.security();
    let current = security.session_get(session, facts)?;
    let expected_key = if reverse {
        security.outbound_key_generation(session, facts)?
    } else {
        key
    };
    let (expected_source, expected_destination) = if reverse {
        (p.destination, p.source)
    } else {
        (p.source, p.destination)
    };
    if facts.peer != source
        || facts.local != destination
        || realm != p.realm
        || realm != security.realm().get()
        || binding(source) != expected_source
        || binding(destination) != expected_destination
        || generation != p.security.session
        || current.session_generation.get() != generation
        || current.hop_profile != HopProfile::H0
        || current.origin_security != origin
        || policy != p.security.policy
        || origin != p.security.origin
        || fingerprint != p.security.fingerprint
        || expected_key != p.security.key
    {
        return Err(Error::Access);
    }
    Ok(())
}
impl<const Q: usize, const B: usize> ControlCoordinator<Q, B> {
    fn transfer_parent<const S: usize, const R: usize>(
        &self,
        h: ControlHandle,
        parent: &ParentOwner,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        reverse: bool,
    ) -> Result<TransferParent> {
        let command = self.view(h)?;
        // Current direct pipeline cannot impersonate a routed terminal Origin.
        if command.header().hop_limit.get() != 1 || facts.now_us < command.accepted_at() {
            return Err(Error::Unsupported);
        }
        let p = parent.view().0.ok_or(Error::State)?;
        let evidence = command.evidence();
        current_parent(p, evidence, security, session, facts, reverse)?;
        let (_, _, _, _, fingerprint) = evidence.security();
        let opcode = match command.body() {
            #[cfg(feature = "capability-control")]
            ControlBody::CapabilitySummary(_, _)
            | ControlBody::CapabilityAdvertise(_)
            | ControlBody::CapabilitySelect(_)
            | ControlBody::CapabilityAck(_)
            | ControlBody::CapabilityQuery(_)
            | ControlBody::CapabilityRevoke(_) => return Err(Error::Unsupported),
            #[cfg(feature = "parent-control")]
            ControlBody::Parent(_, _) => return Err(Error::Unsupported),
            ControlBody::TransferSetup(_) => ucn_types::ProtocolOpcode::TransferSetup,
            ControlBody::TransferSetupAck(_) => ucn_types::ProtocolOpcode::TransferSetupAck,
            ControlBody::TransferSack(_) => ucn_types::ProtocolOpcode::TransferSackCredit,
            ControlBody::TransferAbort(_) => ucn_types::ProtocolOpcode::TransferAbort,
            ControlBody::TransferTerminal(_) => ucn_types::ProtocolOpcode::TransferTerminalReceipt,
            ControlBody::DeliveryAck(_) => return Err(Error::Unsupported),
            #[cfg(feature = "service-control")]
            ControlBody::OperationQuery(_)
            | ControlBody::OperationReceipt(_)
            | ControlBody::OperationCancel(_) => return Err(Error::Unsupported),
            #[cfg(feature = "operation-control")]
            ControlBody::OperationResultAck(_)
            | ControlBody::OperationResume(_)
            | ControlBody::OperationResult(_) => return Err(Error::Unsupported),
            #[cfg(feature = "realtime-control")]
            ControlBody::TimeSync(_)
            | ControlBody::TimeFollowUp(_)
            | ControlBody::TimeDelayRequest(_)
            | ControlBody::TimeDelayResponse(_) => return Err(Error::Unsupported),
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
        Ok(p)
    }
    /// Reserve reply first, commit RX Setup once, then move command into a retained reply.
    /// 先预留回复、接纳一次RX Setup、再将命令义务移入缓存回复。
    /// # Errors
    /// Wrong/expired Parent, changed identity or full reply/reassembly capacity retains command.
    #[allow(clippy::too_many_arguments)]
    pub fn dispatch_transfer_setup<
        const S: usize,
        const R: usize,
        const TX: usize,
        const RX: usize,
        const M: usize,
        const F: usize,
        const RC: usize,
        const C: usize,
        const FB: usize,
    >(
        &mut self,
        h: ControlHandle,
        parent: &ParentOwner,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        transfer: &mut TransferOwner<TX, RX, M, F, RC>,
        replies: &mut ControlReplies<C, FB>,
        digest: &mut DigestWorkspace,
    ) -> Result<(TransferHandle, ReplyHandle)> {
        let command = self.view(h)?;
        let ControlBody::TransferSetup(setup) = command.body() else {
            return Err(Error::Unsupported);
        };
        let p = self.transfer_parent(h, parent, security, session, facts, false)?;
        let key = TransferKey::from_setup(setup, digest)?;
        let expires = match transfer.find_rx(p, setup.transfer_id) {
            Ok(existing) => transfer.view(existing)?.deadline_us,
            Err(Error::NotFound) => command
                .accepted_at()
                .checked_add(u64::from(setup.lifetime_ms) * 1000)
                .ok_or(Error::Exhausted)?,
            Err(error) => return Err(error),
        };
        // A fixed reply slot is reserved before any accepted business state is written.
        let reply = replies.reserve(
            command.into(),
            ControlBody::TransferSetupAck(SetupAck {
                key,
                fragment_bytes: setup.fragment_bytes,
                credit: 32,
            }),
            facts.now_us,
            expires,
        )?;
        let rx = match transfer.receive_setup_at(p, setup, command.accepted_at(), facts.now_us) {
            Ok(handle) => handle,
            Err(error) => {
                replies.abandon_unpublished(reply)?;
                return Err(error);
            }
        };
        // No Provider/Driver callback or foreign mutation between publish and command ack.
        self.ack(h)?;
        Ok((rx, reply))
    }
    /// Resolve complete frozen TX Parent/Setup before applying authenticated feedback.
    /// `应用认证反馈前解析完整冻结TX父域与Setup`。
    /// # Errors
    /// Wrong direction/key/digest, expired lease or conflict preserves command and TX state.
    #[allow(clippy::too_many_arguments)]
    pub fn dispatch_transfer_feedback<
        const S: usize,
        const R: usize,
        const TX: usize,
        const RX: usize,
        const M: usize,
        const F: usize,
        const RC: usize,
    >(
        &mut self,
        h: ControlHandle,
        parent: &ParentOwner,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        transfer: &mut TransferOwner<TX, RX, M, F, RC>,
        digest: &mut DigestWorkspace,
    ) -> Result<TransferHandle> {
        let feedback = match self.view(h)?.body() {
            ControlBody::TransferSetupAck(v) => TransferFeedback::SetupAck(v),
            ControlBody::TransferSack(v) => TransferFeedback::Sack(v),
            ControlBody::TransferAbort(v) => TransferFeedback::Abort(v),
            ControlBody::TransferTerminal(v) => TransferFeedback::Terminal(v),
            _ => return Err(Error::Unsupported),
        };
        let p = self.transfer_parent(h, parent, security, session, facts, true)?;
        let tx = transfer.control_feedback(p, feedback, facts.now_us, digest)?;
        self.ack(h)?;
        Ok(tx)
    }
}
