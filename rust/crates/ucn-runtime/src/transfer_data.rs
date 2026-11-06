//! Authenticated C1 fragment -> exclusive preflight -> Replay commit -> fixed reassembly.
//! 认证C1分片、独占预检、Replay提交、固定重组；业务交付是独立的下一步。
use crate::reply::ReplyRequest;
use crate::transfer_control::current_parent;
use crate::{ControlBody, ControlReplies, ReplyHandle};
use ucn_adapter::LinkHandle;
use ucn_digest::DigestWorkspace;
use ucn_security::{
    AccessDirection, AccessRequest, CryptoProvider, CurrentFacts, Fingerprint, OpenDisposition,
    OpenPacketPlan, PacketEvidence, SecurityOwner, SecurityPacketWorkspace, SessionHandle,
};
use ucn_transport::{
    FragmentEvidence, ParentKind, ParentOwner, TransferHandle, TransferKey, TransferOwner,
    TransferParent, TransferPhase, TransferTerminal, TransferView,
};
use ucn_types::{
    Error, HeaderContract, InteractionRole, OriginSecurity, PayloadKind, Result, ServiceId,
};
use ucn_wire::CommonHeader;

/// Runtime-issued receiver, not an application-forgeable raw identity.
/// Runtime签发的接收实例，不接受应用伪造的原始身份元组。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReceivedTransfer {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// Fragment receipt only; does not imply application or durable execution.
/// 仅分片接纳结果，不代表业务或耐久执行成功。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FragmentReceived {
    /// Exact receiver; 精确接收实例。
    pub transfer: ReceivedTransfer,
    /// Reserved SACK or retained terminal reply; 预留SACK或保留终态回复。
    pub reply: ReplyHandle,
    /// New bytes committed, false for exact authenticated duplicate; 新字节提交，精确认证重复为false。
    pub fresh: bool,
    /// Complete message digest was validated; 完整消息摘要已通过。
    pub complete: bool,
}
/// One application-buffer handoff; not an execution permit or external effect proof.
/// 一次业务Buffer交付，不是执行许可或外部副作用证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ApplicationDelivery {
    /// Exact copied business bytes; 实际复制的业务字节数。
    pub bytes: usize,
    /// Retained final-target reply; 保留的最终目标回复。
    pub terminal: ReplyHandle,
}
struct Receiver {
    rx: TransferHandle,
    parent: TransferParent,
    evidence: PacketEvidence,
    service: ServiceId,
    session: SessionHandle,
    link: LinkHandle,
    last_admitted: u64,
    sack: Option<ReplyHandle>,
    terminal: Option<ReplyHandle>,
}
/// Independent fixed fragment staging and opaque receiver bindings.
/// 独立固定分片暂存及不透明接收绑定，不扩大普通单帧Core。
pub struct TransferBridge<const RECEIVERS: usize, const BYTES: usize> {
    owner: u32,
    bytes: [u8; BYTES],
    receivers: [Option<Receiver>; RECEIVERS],
    generations: [u32; RECEIVERS],
    stopped: bool,
}
impl<const N: usize, const B: usize> TransferBridge<N, B> {
    /// Static caller-owned initialization; 调用方可静态初始化。
    /// # Errors
    /// Zero/oversized capacity or too-small fragment scratch rejects.
    pub const fn new(owner: u32) -> Result<Self> {
        if owner == 0 || N == 0 || N > u16::MAX as usize || B < 13 {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            bytes: [0; B],
            receivers: [const { None }; N],
            generations: [0; N],
            stopped: false,
        })
    }
    fn index(&self, h: ReceivedTransfer) -> Result<usize> {
        if h.owner != self.owner
            || h.slot >= N
            || self.generations[h.slot] != h.generation
            || self.receivers[h.slot].is_none()
        {
            return Err(Error::State);
        }
        Ok(h.slot)
    }
    /// Read only the exact receiver's immutable state; 精确接收实例只读状态。
    /// # Errors
    /// Stale/cross-owner or retired Transfer cannot expose a reused slot.
    pub fn view<
        const TX: usize,
        const RX: usize,
        const M: usize,
        const F: usize,
        const RC: usize,
    >(
        &self,
        h: ReceivedTransfer,
        transfer: &TransferOwner<TX, RX, M, F, RC>,
    ) -> Result<TransferView> {
        transfer.view(
            self.receivers[self.index(h)?]
                .as_ref()
                .ok_or(Error::State)?
                .rx,
        )
    }
    fn binding(
        &self,
        rx: TransferHandle,
        parent: TransferParent,
        session: SessionHandle,
        link: LinkHandle,
        now: u64,
    ) -> Result<(usize, u32)> {
        if let Some(slot) = self
            .receivers
            .iter()
            .position(|r| r.as_ref().is_some_and(|r| r.rx == rx))
        {
            let r = self.receivers[slot].as_ref().ok_or(Error::State)?;
            if r.parent != parent || r.session != session || r.link != link || now < r.last_admitted
            {
                return Err(Error::Access);
            }
            return Ok((slot, self.generations[slot]));
        }
        let slot = self
            .receivers
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        Ok((
            slot,
            self.generations[slot]
                .checked_add(1)
                .ok_or(Error::Exhausted)?,
        ))
    }
    /// Actual Security/ACL and linear fragment preflight; retain a response before committing Replay.
    /// 实际Security/ACL与线性分片预检，先保留回复资源再提交Replay。
    /// # Errors
    /// Every failed admission aborts Replay and leaves reassembly/public result unchanged.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn receive<
        const S: usize,
        const R: usize,
        const W: usize,
        const TX: usize,
        const RX: usize,
        const M: usize,
        const F: usize,
        const RC: usize,
        const Q: usize,
        const FB: usize,
    >(
        &mut self,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        work: &mut SecurityPacketWorkspace<W>,
        parent_owner: &ParentOwner,
        link: LinkHandle,
        transfer: &mut TransferOwner<TX, RX, M, F, RC>,
        replies: &mut ControlReplies<Q, FB>,
        digest: &mut DigestWorkspace,
    ) -> Result<FragmentReceived> {
        if self.stopped {
            return Err(Error::State);
        }
        let header = CommonHeader::decode(plan.packet)?;
        // Routed/Flow and RPC execution dispatch are not implicitly enabled by this slice.
        if header.contract != HeaderContract::C1
            || header.payload_kind != PayloadKind::Transfer
            || header.interaction != InteractionRole::OneWay
            || header.origin_security == OriginSecurity::O0
            || header.hop_limit.get() != 1
            || access.protocol_opcode != 0
            || access.direction != AccessDirection::Inbound
        {
            return Err(Error::Unsupported);
        }
        if link.instance_generation() != facts.link_generation.get() {
            return Err(Error::State);
        }
        let parent = parent_owner.view().0.ok_or(Error::State)?;
        let opened =
            security.open_packet(crypto, session, facts, access, plan, work, &mut self.bytes)?;
        let evidence = opened.evidence();
        let convert = || -> Result<FragmentEvidence> {
            let sequence = u32::try_from(evidence.sequence()).map_err(|_| Error::Malformed)?;
            if sequence == 0 {
                return Err(Error::Malformed);
            }
            let (aad, payload) = evidence.digests();
            Ok(FragmentEvidence {
                sequence,
                aad,
                payload,
            })
        };
        let checked = current_parent(parent, evidence, security, session, facts, false)
            .and_then(|()| convert());
        let proof = match checked {
            Ok(proof) => proof,
            Err(error) => {
                security.replay_abort(opened.replay)?;
                return Err(error);
            }
        };
        if opened.disposition == OpenDisposition::AuthenticatedReplayCandidate {
            security.replay_abort(opened.replay)?;
            let (rx, index) = transfer.replay_fragment(
                parent,
                proof,
                header.interaction,
                access.service.get(),
                facts.now_us,
            )?;
            let (slot, _) = self.binding(rx, parent, session, link, facts.now_us)?;
            let r = self.receivers[slot].as_ref().ok_or(Error::Replay)?;
            let view = transfer.view(rx)?;
            let reply = if let Some(h) = r.terminal {
                replies.requeue(h, facts.now_us)?
            } else if let Some(h) = r.sack {
                if !replies.sack_matches(h, transfer.sack(rx, index / 32 * 32)?)? {
                    return Err(Error::NoSpace); // Preserve another window's accepted obligation.
                }
                replies.requeue(h, facts.now_us)?
            } else {
                replies.reserve(
                    ReplyRequest::fragment(evidence, access.service, facts.now_us),
                    ControlBody::TransferSack(transfer.sack(rx, index / 32 * 32)?),
                    facts.now_us,
                    view.deadline_us,
                )?
            };
            self.receivers[slot].as_mut().ok_or(Error::State)?.sack = if r.terminal.is_some() {
                r.sack
            } else {
                Some(reply)
            };
            return Ok(FragmentReceived {
                transfer: ReceivedTransfer {
                    owner: self.owner,
                    slot,
                    generation: self.generations[slot],
                },
                reply,
                fresh: false,
                complete: matches!(
                    view.phase,
                    TransferPhase::Complete | TransferPhase::AppHeld | TransferPhase::Delivered
                ),
            });
        }
        let decoded =
            ucn_transport::decode_fragment(ParentKind::C1, &self.bytes[..opened.payload_bytes]);
        let (prefix, offset) = match decoded {
            Ok(value) => value,
            Err(error) => {
                security.replay_abort(opened.replay)?;
                return Err(error);
            }
        };
        let resolved = transfer.find_rx(parent, prefix.transfer_id).and_then(|rx| {
            let view = transfer.view(rx)?;
            if view.setup.delivery != header.delivery {
                return Err(Error::Access);
            }
            let (slot, generation) = self.binding(rx, parent, session, link, facts.now_us)?;
            if self.receivers[slot]
                .as_ref()
                .is_some_and(|r| r.sack.is_some() || r.terminal.is_some())
            {
                return Err(Error::NoSpace);
            }
            Ok((rx, slot, generation, view.deadline_us))
        });
        let (rx, slot, generation, expires) = match resolved {
            Ok(value) => value,
            Err(error) => {
                security.replay_abort(opened.replay)?;
                return Err(error);
            }
        };
        let pending = match transfer.prepare_fragment(
            rx,
            parent,
            prefix,
            header.interaction,
            access.service.get(),
            proof,
            &self.bytes[offset..opened.payload_bytes],
            facts.now_us,
            digest,
        ) {
            Ok(pending) => pending,
            Err(error) => {
                security.replay_abort(opened.replay)?;
                return Err(error);
            }
        };
        let reply = match replies.reserve(
            ReplyRequest::fragment(evidence, access.service, facts.now_us),
            ControlBody::TransferSack(pending.sack()),
            facts.now_us,
            expires,
        ) {
            Ok(h) => h,
            Err(error) => {
                security.replay_abort(opened.replay)?;
                return Err(error);
            }
        };
        if let Err(error) = security.replay_commit(opened.replay, facts) {
            replies.abandon_unpublished(reply)?;
            return Err(error);
        }
        let fresh = pending.commit(); // Exclusive borrow makes this no-failure/no-callback.
        self.receivers[slot] = Some(Receiver {
            rx,
            parent,
            evidence,
            service: access.service,
            session,
            link,
            last_admitted: facts.now_us,
            sack: Some(reply),
            terminal: None,
        });
        self.generations[slot] = generation;
        Ok(FragmentReceived {
            transfer: ReceivedTransfer {
                owner: self.owner,
                slot,
                generation,
            },
            reply,
            fresh,
            complete: transfer.view(rx)?.phase == TransferPhase::Complete,
        })
    }
    /// Release a definite locally-sent SACK only; duplicates can regenerate cumulative feedback.
    /// 仅释放确定本地发送完的SACK；重复可重新生成累计反馈，不保存无限历史回复。
    /// # Errors
    /// Pending/uncertain Token keeps the reply and receiver binding occupied.
    pub fn retire_feedback<const Q: usize, const FB: usize>(
        &mut self,
        h: ReceivedTransfer,
        replies: &mut ControlReplies<Q, FB>,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let sack = self.receivers[slot]
            .as_ref()
            .ok_or(Error::State)?
            .sack
            .ok_or(Error::State)?;
        replies.forget_sack(sack)?;
        self.receivers[slot].as_mut().ok_or(Error::State)?.sack = None;
        Ok(())
    }
    /// Copy to the application once, reserving final-target feedback before any output write.
    /// 仅向业务复制一次，在首次写输出前预留最终目标反馈。
    /// # Errors
    /// Incomplete/duplicate/expired/unauthorized/capacity failure preserves output and phase.
    #[allow(clippy::too_many_arguments)]
    pub fn deliver<
        const S: usize,
        const R: usize,
        const TX: usize,
        const RX: usize,
        const M: usize,
        const F: usize,
        const RC: usize,
        const Q: usize,
        const FB: usize,
    >(
        &mut self,
        h: ReceivedTransfer,
        parent_owner: &ParentOwner,
        security: &SecurityOwner<'_, S, R>,
        session: SessionHandle,
        facts: CurrentFacts,
        link: LinkHandle,
        transfer: &mut TransferOwner<TX, RX, M, F, RC>,
        replies: &mut ControlReplies<Q, FB>,
        output: &mut [u8],
        digest: &mut DigestWorkspace,
    ) -> Result<ApplicationDelivery> {
        if self.stopped {
            return Err(Error::State);
        }
        let slot = self.index(h)?;
        let r = self.receivers[slot].as_ref().ok_or(Error::State)?;
        if r.terminal.is_some()
            || parent_owner.view().0 != Some(r.parent)
            || r.session != session
            || r.link != link
            || facts.now_us < r.last_admitted
            || link.instance_generation() != facts.link_generation.get()
        {
            return Err(Error::State);
        }
        current_parent(r.parent, r.evidence, security, session, facts, false)?;
        security.authorize(
            AccessRequest {
                session,
                context_fingerprint: Fingerprint::new(r.parent.security.fingerprint)?,
                service: r.service,
                protocol_opcode: 0,
                direction: AccessDirection::Inbound,
            },
            facts,
        )?;
        let view = transfer.view(r.rx)?;
        if view.phase != TransferPhase::Complete {
            return Err(Error::State);
        }
        if facts.now_us >= view.deadline_us {
            return Err(Error::Timeout);
        }
        if output.len() < view.setup.total_bytes as usize {
            return Err(Error::NoSpace);
        }
        let key = TransferKey::from_setup(view.setup, digest)?;
        let terminal = replies.reserve(
            ReplyRequest::fragment(r.evidence, r.service, r.last_admitted),
            ControlBody::TransferTerminal(TransferTerminal {
                key,
                state: 1,
                result: 0,
            }),
            facts.now_us,
            view.deadline_us,
        )?;
        let bytes = match transfer.copy_complete(r.rx, output) {
            Ok(bytes) => bytes,
            Err(error) => {
                replies.abandon_unpublished(terminal)?;
                return Err(error);
            }
        };
        self.receivers[slot].as_mut().ok_or(Error::State)?.terminal = Some(terminal);
        Ok(ApplicationDelivery { bytes, terminal })
    }
    /// Stop new admission/delivery without erasing accepted reply/Driver obligations.
    /// 停止新接纳/交付，但不抹掉已接纳回复或Driver义务。
    pub fn stop(&mut self) {
        self.stopped = true;
    }
    /// Local application rejection, also after stop; it is not a remote cancellation proof.
    /// 本地业务拒绝，停止后仍可操作；不冒充远端取消证明。
    /// # Errors
    /// Delivered/held/incomplete data cannot be discarded through this entry.
    pub fn discard<
        const TX: usize,
        const RX: usize,
        const M: usize,
        const F: usize,
        const RC: usize,
    >(
        &mut self,
        h: ReceivedTransfer,
        transfer: &mut TransferOwner<TX, RX, M, F, RC>,
    ) -> Result<()> {
        transfer.discard_complete(
            self.receivers[self.index(h)?]
                .as_ref()
                .ok_or(Error::State)?
                .rx,
        )
    }
    /// Retire RX after application release and receipt expiry; independent reply holders remain.
    /// 业务释放且凭据过期后退休RX；独立回复Holder继续由Reply Owner保留。
    /// # Errors
    /// Active/held/unexpired RX or stale receipt cannot free a reused slot.
    pub fn retire<
        const TX: usize,
        const RX: usize,
        const M: usize,
        const F: usize,
        const RC: usize,
    >(
        &mut self,
        h: ReceivedTransfer,
        transfer: &mut TransferOwner<TX, RX, M, F, RC>,
        now: u64,
    ) -> Result<()> {
        let slot = self.index(h)?;
        transfer.retire_rx(self.receivers[slot].as_ref().ok_or(Error::State)?.rx, now)?;
        self.receivers[slot] = None;
        Ok(())
    }
}
