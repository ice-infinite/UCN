//! One fixed authenticated assembly, with a non-renewing deadline and replay high-water.
//! 单个固定认证重组槽、不可续截止及不回退重放高水位。
use super::{ControlAdmission, ControlCoordinator, ControlReplies, JournalPeer};
use ucn_capability::{CapabilityFragment, decode_capability_fragment};
use ucn_security::{
    AccessDirection, AccessRequest, CryptoProvider, OpenPacketPlan, OriginContext, SecurityOwner,
    SecurityPacketWorkspace, SenderDirection,
};
use ucn_security::{OpenDisposition, PacketEvidence};
use ucn_types::{
    C0TransactionId, DeliveryGuarantee, Error, HeaderContract, HopProfile, InteractionRole,
    OriginSecurity, PayloadKind, ProtocolOpcode, Result, ServiceId, TrafficClass,
};
use ucn_wire::CommonHeader;
pub(super) struct Assembly {
    base: u64,
    high: u64,
    opcode: ProtocolOpcode,
    total: usize,
    bytes: [u8; 75],
    evidence: [Option<PacketEvidence>; 2],
    header: Option<CommonHeader>,
    service: Option<ServiceId>,
    first_at: u64,
    now: u64,
    expires: u64,
    published: bool,
}
impl Assembly {
    pub(super) const fn new() -> Self {
        Self {
            base: 0,
            high: 0,
            opcode: ProtocolOpcode::CapabilityAdvertise,
            total: 0,
            bytes: [0; 75],
            evidence: [None; 2],
            header: None,
            service: None,
            first_at: 0,
            now: 0,
            expires: 0,
            published: false,
        }
    }
    pub(super) fn pending(&self) -> bool {
        self.evidence.iter().all(Option::is_some) && !self.published
    }
    pub(super) fn discard(&mut self) {
        self.bytes.fill(0);
        self.evidence = [None; 2];
        self.published = true; // Stop retirement only; keep replay high-water.
    }
    pub(super) fn empty(&self) -> bool {
        self.evidence.iter().all(Option::is_none)
    }
    fn check(
        &self,
        v: CapabilityFragment<'_>,
        e: PacketEvidence,
        h: CommonHeader,
        service: ServiceId,
        now: u64,
        expires: u64,
    ) -> Result<u64> {
        let base = e
            .sequence()
            .checked_sub(u64::from(v.index))
            .ok_or(Error::Malformed)?;
        if base == 0 || base.checked_add(1).is_none() || now >= expires {
            return Err(Error::Timeout);
        }
        if h.interaction
            != if v.opcode == ProtocolOpcode::CapabilityAck {
                InteractionRole::Result
            } else {
                InteractionRole::OneWay
            }
        {
            return Err(Error::Malformed);
        }
        if base == self.base {
            if now >= self.expires || now < self.first_at {
                return Err(Error::Timeout);
            }
            if self.opcode != v.opcode
                || self.total != usize::from(v.total)
                || self.header != Some(h)
                || self.service != Some(service)
            {
                return Err(Error::Security);
            }
            for old in self.evidence.iter().flatten() {
                if old.identities() != e.identities() || old.security() != e.security() {
                    return Err(Error::Security);
                }
            }
            if self.evidence[usize::from(v.index)].is_some() {
                return Err(Error::Replay);
            }
        } else {
            if base <= self.high {
                return Err(Error::Replay);
            }
            if self.base != 0 && !self.published {
                return Err(Error::NoSpace);
            }
        }
        Ok(base)
    }
    #[allow(clippy::too_many_arguments)] // One preflighted atomic slot publication.
    fn commit(
        &mut self,
        v: CapabilityFragment<'_>,
        e: PacketEvidence,
        h: CommonHeader,
        service: ServiceId,
        base: u64,
        now: u64,
        expires: u64,
    ) {
        if base != self.base {
            self.base = base;
            self.high = base + 1;
            self.opcode = v.opcode;
            self.total = usize::from(v.total);
            self.bytes.fill(0);
            self.evidence = [None; 2];
            self.header = Some(h);
            self.service = Some(service);
            self.first_at = now;
            self.expires = expires;
            self.published = false;
        }
        let half = usize::from(v.total).div_ceil(2);
        let start = usize::from(v.index) * half;
        self.bytes[start..(start + half).min(usize::from(v.total))].copy_from_slice(v.chunk);
        self.evidence[usize::from(v.index)] = Some(e);
        self.now = now;
    }
    pub(super) fn publish<const Q: usize, const B: usize, const C: usize, const F: usize>(
        &mut self,
        control: &mut ControlCoordinator<Q, B>,
        replies: &mut ControlReplies<C, F>,
        now: u64,
    ) -> Result<ControlAdmission> {
        if self.evidence.iter().any(Option::is_none) || now < self.first_at || now >= self.expires {
            return Err(Error::State);
        }
        self.now = now;
        let result = control.admit_capability_parts(
            self.opcode,
            &self.bytes[..self.total],
            self.evidence[0].ok_or(Error::State)?,
            self.header.ok_or(Error::State)?,
            self.service.ok_or(Error::State)?,
            self.first_at,
            self.now,
            self.published,
            replies,
        );
        if result != Err(Error::NoSpace) {
            self.published = true;
        }
        result
    }
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(super) fn receive<
        const S: usize,
        const R: usize,
        const W: usize,
        const Q: usize,
        const B: usize,
        const C: usize,
        const F: usize,
    >(
        &mut self,
        peer: JournalPeer,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        packet: &[u8],
        n: usize,
        service: ServiceId,
        deadline: u64,
        scratch: &mut SecurityPacketWorkspace<W>,
        control: &mut ControlCoordinator<Q, B>,
        replies: &mut ControlReplies<C, F>,
    ) -> Result<Option<ControlAdmission>> {
        let header = CommonHeader::decode(packet)?;
        if header.contract != HeaderContract::C0
            || header.payload_kind != PayloadKind::Control
            || header.origin_security == OriginSecurity::O0
            || header.delivery != DeliveryGuarantee::BestEffort
            || header.traffic_class != TrafficClass::Q2
            || header.hop_limit.get() != 1
            || security.session_get(peer.session, peer.facts)?.hop_profile != HopProfile::H1
        {
            return Err(Error::Malformed);
        }
        let tx = C0TransactionId::new(u64::from_be_bytes(
            packet[n - 10..n - 2]
                .try_into()
                .map_err(|_| Error::Malformed)?,
        ))?;
        let access = |op: ProtocolOpcode| AccessRequest {
            session: peer.session,
            context_fingerprint: peer.fingerprint,
            service,
            protocol_opcode: op.into(),
            direction: AccessDirection::Inbound,
        };
        let mut bytes = [0; 44];
        let opened = security.open_packet(
            crypto,
            peer.session,
            peer.facts,
            access(ProtocolOpcode::CapabilityFragment),
            OpenPacketPlan {
                packet,
                prefix_bytes: n,
                canonical_identity: &packet[3..n],
                origin_context: OriginContext::C0 {
                    fingerprint: peer.fingerprint,
                    transaction_id: tx,
                    opcode: ProtocolOpcode::CapabilityFragment,
                    direction: if peer.facts.peer.principal < peer.facts.local.principal {
                        SenderDirection::AscendingPrincipal
                    } else {
                        SenderDirection::DescendingPrincipal
                    },
                },
                hop_profile: HopProfile::H1,
            },
            scratch,
            &mut bytes,
        )?;
        let evidence = opened.evidence();
        let now = peer.facts.now_us;
        if opened.disposition == OpenDisposition::AuthenticatedReplayCandidate {
            security.replay_abort(opened.replay)?;
            if now >= self.expires
                || now < self.first_at
                || header != self.header.ok_or(Error::Replay)?
                || !self.evidence.contains(&Some(evidence))
            {
                return Err(Error::Replay);
            }
            security.authorize(access(self.opcode), peer.facts)?;
            self.now = now;
        } else {
            let result = (|| {
                let v = decode_capability_fragment(&bytes[..opened.payload_bytes])?;
                security.authorize(access(v.opcode), peer.facts)?;
                let base = self.check(v, evidence, header, service, now, deadline)?;
                Ok((v, base))
            })();
            let (v, base) = match result {
                Ok(v) => v,
                Err(e) => {
                    security.replay_abort(opened.replay)?;
                    return Err(e);
                }
            };
            security.replay_commit(opened.replay, peer.facts)?;
            self.commit(v, evidence, header, service, base, now, deadline);
        }
        if self.evidence.iter().any(Option::is_none) {
            return Ok(None);
        }
        self.publish(control, replies, now).map(Some)
    }
}
