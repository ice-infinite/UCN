//! Fixed two-frame capability replies; each frame has its own Origin counter/tag.
//! 固定双帧能力回复，每帧都有独立Origin计数和认证Tag。
use super::{ControlReplies, ReplyHandle, ReplyPhase, encode_reply, reply_role};
use ucn_adapter::LinkHandle;
use ucn_capability::{CapabilityFragment, capability_fragment_range, encode_capability_fragment};
use ucn_security::{
    AccessDirection, AccessRequest, C0TransactionOwner, CryptoProvider, CurrentFacts, Fingerprint,
    HopSequenceOwner, OriginContext, OriginCounterOwner, PacketPlan, SecurityOwner,
    SecurityPacketWorkspace, SenderDirection, SessionHandle,
};
use ucn_types::{
    DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile, PayloadKind, ProtocolOpcode,
    Result, TrafficClass,
};
use ucn_wire::CommonHeader;
pub(super) struct Parts<const B: usize> {
    pub(super) active: bool,
    pub(super) index: u8,
    pub(super) second: [u8; B],
    pub(super) length: usize,
}
impl<const B: usize> Parts<B> {
    pub(super) const fn new() -> Self {
        Self {
            active: false,
            index: 0,
            second: [0; B],
            length: 0,
        }
    }
}
impl<const Q: usize, const B: usize> ControlReplies<Q, B> {
    /// Split only an oversized Record/Ack; otherwise keep the existing exact single frame.
    /// 仅拆分放不下的Record/Ack，否则保留原单帧。
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(crate) fn seal_capability<const S: usize, const R: usize, const W: usize>(
        &mut self,
        h: ReplyHandle,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        session: SessionHandle,
        facts: CurrentFacts,
        link: LinkHandle,
        mut hop: Option<&mut dyn HopSequenceOwner>,
        mtu: usize,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<()> {
        let slot = self.index(h)?;
        let r = self.replies[slot].as_ref().ok_or(Error::State)?;
        let view = security.session_get(session, facts)?;
        let n = 25 + 2 * security.address_width().bytes();
        let mut body = [0; 75];
        let (opcode, bytes) = encode_reply(r.body, &mut body)?;
        if n + bytes + 16 + usize::from(view.hop_profile == HopProfile::H1) * 16 <= mtu.min(B) {
            return self.seal_with_hop(
                h, security, crypto, counter, session, facts, link, hop, scratch,
            );
        }
        Self::current(r, facts)?;
        if self.max_attempts < 2 {
            return Err(Error::Config);
        }
        if self.stopped
            || r.phase != ReplyPhase::AwaitSeal
            || r.token.is_some()
            || view.hop_profile != HopProfile::H1
            || hop.is_none()
            || link.instance_generation() != facts.link_generation.get()
        {
            return Err(Error::State);
        }
        let (realm, _, _) = r.request.evidence().identities();
        let (generation, _, _, origin, fp) = r.request.evidence().security();
        if generation != view.session_generation.get()
            || origin != view.origin_security
            || realm != security.realm().get()
        {
            return Err(Error::Access);
        }
        let fp = Fingerprint::new(fp)?;
        let access = |op: ProtocolOpcode| AccessRequest {
            session,
            context_fingerprint: fp,
            service: r.request.service(),
            protocol_opcode: op.into(),
            direction: AccessDirection::Outbound,
        };
        security.authorize(access(opcode), facts)?;
        security.authorize(access(ProtocolOpcode::CapabilityFragment), facts)?;
        let range = capability_fragment_range(opcode, bytes, 0)?;
        if n + 6 + range.len() + 32 > mtu.min(B) {
            return Err(Error::NoSpace);
        }
        let base = counter.preview()?.get();
        base.checked_add(1).ok_or(Error::Exhausted)?;
        let outbound_key = security.outbound_key_generation(session, facts)?;
        let mut lengths = [0; 2];
        for index in 0..2_u8 {
            let transaction = counter.preview()?;
            if transaction.get() != base + u64::from(index) {
                return Err(Error::State);
            }
            let range = capability_fragment_range(opcode, bytes, index)?;
            let mut chunk = [0; 44];
            let payload = encode_capability_fragment(
                CapabilityFragment {
                    opcode,
                    total: u16::try_from(bytes).map_err(|_| Error::Config)?,
                    index,
                    chunk: &body[range],
                },
                &mut chunk,
            )?;
            let mut prefix = [0; 33];
            CommonHeader {
                contract: HeaderContract::C0,
                origin_security: origin,
                delivery: DeliveryGuarantee::BestEffort,
                interaction: reply_role(r.body),
                payload_kind: PayloadKind::Control,
                traffic_class: TrafficClass::Q2,
                hop_limit: HopLimit::new(1)?,
            }
            .encode(&mut prefix);
            let width = security.address_width().bytes();
            prefix[3..7].copy_from_slice(&realm.to_be_bytes());
            prefix[7..7 + width]
                .copy_from_slice(&facts.local.address.get().to_be_bytes()[4 - width..]);
            prefix[7 + width..7 + 2 * width]
                .copy_from_slice(&facts.peer.address.get().to_be_bytes()[4 - width..]);
            prefix[7 + 2 * width..11 + 2 * width]
                .copy_from_slice(&facts.local.generation.get().to_be_bytes());
            prefix[11 + 2 * width..15 + 2 * width]
                .copy_from_slice(&facts.peer.generation.get().to_be_bytes());
            prefix[n - 10..n - 2].copy_from_slice(&transaction.get().to_be_bytes());
            prefix[n - 2..n]
                .copy_from_slice(&u16::from(ProtocolOpcode::CapabilityFragment).to_be_bytes());
            let output = if index == 0 {
                &mut self.bytes[slot]
            } else {
                &mut self.parts[slot].second
            };
            lengths[usize::from(index)] = security.protect_packet(
                crypto,
                OriginCounterOwner::Transaction(counter),
                session,
                facts,
                access(ProtocolOpcode::CapabilityFragment),
                PacketPlan {
                    prefix: &prefix[..n],
                    payload: &chunk[..payload],
                    canonical_identity: &prefix[3..n],
                    origin_context: OriginContext::C0 {
                        fingerprint: fp,
                        transaction_id: transaction,
                        opcode: ProtocolOpcode::CapabilityFragment,
                        direction: if facts.local.principal < facts.peer.principal {
                            SenderDirection::AscendingPrincipal
                        } else {
                            SenderDirection::DescendingPrincipal
                        },
                    },
                    hop_profile: HopProfile::H1,
                },
                Some(&mut **hop.as_mut().ok_or(Error::State)?),
                scratch,
                output,
            )?;
        }
        let r = self.replies[slot].as_mut().ok_or(Error::State)?;
        r.length = lengths[0];
        r.session = Some(session);
        r.link = Some(link);
        r.facts = Some(facts);
        r.outbound_key = outbound_key;
        r.phase = ReplyPhase::Ready;
        self.parts[slot].active = true;
        self.parts[slot].index = 0;
        self.parts[slot].length = lengths[1];
        Ok(())
    }
}
