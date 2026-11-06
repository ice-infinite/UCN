//! C1 opaque hop protection; it never asserts end-to-end authentication.
//! C1不透明逐跳保护，绝不声称完成端到端认证。
use crate::packet::{HOP_TRAILER_BYTES, build_hop_aad};
use crate::replay::ReplayMutationHandle;
use crate::session::CALLBACK_CRYPTO;
use crate::{
    AccessDirection, AccessRequest, CryptoProvider, CurrentFacts, HopSequenceOwner, SecurityOwner,
    SecurityPacketWorkspace, SessionHandle,
};
use ucn_digest::canonical_digest;
use ucn_types::{
    Error, HeaderContract, HopProfile, NodeAddress, OriginSecurity, Result, ServiceId,
};
use ucn_wire::CommonHeader;

const PACKET_DOMAIN: &[u8] = b"UCN6-C1-HOP-PACKET-V1";

/// Pending exact-neighbour proof; not an Origin/Endpoint authorization.
/// 待提交的精确邻居证明；不是Origin或Endpoint授权。
#[derive(Debug)]
pub struct C1HopOpened {
    owner: u32,
    session: SessionHandle,
    replay: ReplayMutationHandle,
    digest: [u8; 16],
    bytes: usize,
    header: CommonHeader,
}
impl C1HopOpened {
    /// Authenticated visible header only; 仅已逐跳认证的可见头。
    #[must_use]
    pub const fn header(&self) -> CommonHeader {
        self.header
    }
}

#[derive(Clone, Copy)]
struct Shape {
    header: CommonHeader,
    source: NodeAddress,
    service: ServiceId,
}

fn shape(raw: &[u8], address_width: ucn_types::AddressWidth) -> Result<Shape> {
    let width = address_width.bytes();
    let header = CommonHeader::decode(raw)?;
    let prefix = 9 + 2 * width;
    if header.contract != HeaderContract::C1
        || header.origin_security == OriginSecurity::O0
        || raw.len() < prefix + 16
    {
        return Err(Error::Malformed);
    }
    // No payload inspection: an encrypted Control opcode is unavailable here.
    // 不检查载荷：加密的Control Opcode在中继不可见，因此当前只放行Data。
    if header.payload_kind != ucn_types::PayloadKind::Data {
        return Err(Error::Unsupported);
    }
    let address = |offset: usize| {
        raw[offset..offset + width]
            .iter()
            .fold(0u32, |v, b| (v << 8) | u32::from(*b))
    };
    let source = NodeAddress::new(address(3), address_width)?;
    NodeAddress::new(address(3 + width), address_width)?;
    let service = ServiceId::new(u16::from_be_bytes([raw[3 + 2 * width], raw[4 + 2 * width]]))?;
    ucn_types::OriginSequence::new(u32::from_be_bytes(
        raw[prefix - 4..prefix]
            .try_into()
            .map_err(|_| Error::Malformed)?,
    ))?;
    Ok(Shape {
        header,
        source,
        service,
    })
}

impl<const S: usize, const R: usize> SecurityOwner<'_, S, R> {
    fn c1_hop_access(
        &self,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        shape: Shape,
        direction: AccessDirection,
    ) -> Result<crate::HandshakeCandidate> {
        if self.callback_gate().is_active() {
            return Err(Error::State);
        }
        let candidate = self.candidate(session, facts)?;
        if candidate.hop_profile != HopProfile::H1
            || access.session != session
            || access.direction != direction
            || access.context_fingerprint != candidate.hop_fingerprint
            || access.service != shape.service
            || access.protocol_opcode != 0
        {
            return Err(Error::Access);
        }
        self.authorize(access, facts)?;
        Ok(candidate)
    }

    /// Verify only the current neighbour and reserve Hop replay, without decrypting.
    /// 只验证当前邻居并预留Hop Replay，不解密业务。
    /// # Errors
    /// Invalid header, peer/ACL, tag, counter, capacity or callback state rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn open_c1_hop<const B: usize>(
        &mut self,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        packet: &[u8],
        work: &mut SecurityPacketWorkspace<B>,
    ) -> Result<C1HopOpened> {
        let end = packet
            .len()
            .checked_sub(HOP_TRAILER_BYTES)
            .ok_or(Error::Malformed)?;
        let shape = shape(&packet[..end], self.address_width())?;
        let c = self.c1_hop_access(session, facts, access, shape, AccessDirection::Inbound)?;
        let sequence = u32::from_be_bytes(
            packet[end..end + 4]
                .try_into()
                .map_err(|_| Error::Malformed)?,
        );
        let n = build_hop_aad(&packet[..end], sequence, c.hop_fingerprint, &mut work.aad)?;
        let tag = packet[end + 4..].try_into().map_err(|_| Error::Malformed)?;
        let selector = c.hop_rx.ok_or(Error::State)?;
        let claim = self.next_claim(CALLBACK_CRYPTO)?;
        let gate = self.gate();
        let lease = gate.try_enter(claim)?;
        let result = provider.verify_hop_tag(selector, &work.aad[..n], tag);
        let left = gate.leave(lease);
        result?;
        left?;
        let digest = canonical_digest(PACKET_DOMAIN, packet, &mut work.codec)?;
        let aad = canonical_digest(b"UCN6-AAD-V1", &work.aad[..n], &mut work.codec)?;
        let replay = self.reserve_hop_replay(session, facts, u64::from(sequence), aad, digest)?;
        Ok(C1HopOpened {
            owner: self.owner_instance(),
            session,
            replay,
            digest,
            bytes: packet.len(),
            header: shape.header,
        })
    }

    /// Exact pending proof and packet binding before forwarding or terminal Origin open.
    /// 转发或终点Origin验证前复验精确待提交证明及原始报文。
    /// # Errors
    /// Changed bytes, stale parent/deadline, foreign Owner or consumed proof rejects.
    pub fn c1_hop_preflight<const B: usize>(
        &self,
        opened: &C1HopOpened,
        facts: CurrentFacts,
        packet: &[u8],
        work: &mut SecurityPacketWorkspace<B>,
    ) -> Result<()> {
        if self.callback_gate().is_active()
            || opened.owner != self.owner_instance()
            || opened.bytes != packet.len()
            || canonical_digest(PACKET_DOMAIN, packet, &mut work.codec)? != opened.digest
        {
            return Err(Error::State);
        }
        self.preflight_replay(opened.session, facts, opened.replay, true)
    }

    /// Source adds H1 to an existing Origin-protected C1; no new header fields.
    /// 源为已有Origin保护的C1添加H1，不增加头字段。
    /// # Errors
    /// Source address, exact ACL, sequence, buffer or Driver callback mismatch rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn wrap_c1_hop<const B: usize>(
        &mut self,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        origin_packet: &[u8],
        counter: &mut (impl HopSequenceOwner + ?Sized),
        work: &mut SecurityPacketWorkspace<B>,
        output: &mut [u8],
    ) -> Result<usize> {
        let s = shape(origin_packet, self.address_width())?;
        if s.source != facts.local.address {
            return Err(Error::Access);
        }
        self.seal_c1_hop(
            provider,
            session,
            facts,
            access,
            origin_packet,
            s.header,
            counter,
            work,
            output,
        )
    }

    /// Relay preserves opaque Origin bytes and only decrements TTL/replaces H1.
    /// 中继保留不透明Origin字节，只减少TTL并替换H1。
    /// # Errors
    /// Proof/packet mismatch, TTL exhaustion, stale peer, ACL or output failure rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn forward_c1_hop<const B: usize>(
        &mut self,
        provider: &mut impl CryptoProvider,
        opened: &C1HopOpened,
        inbound: CurrentFacts,
        packet: &[u8],
        outbound_session: SessionHandle,
        outbound: CurrentFacts,
        access: AccessRequest,
        counter: &mut (impl HopSequenceOwner + ?Sized),
        work: &mut SecurityPacketWorkspace<B>,
        output: &mut [u8],
    ) -> Result<usize> {
        self.c1_hop_preflight(opened, inbound, packet, work)?;
        if inbound.now_us != outbound.now_us || inbound.local != outbound.local {
            return Err(Error::Argument);
        }
        let mut h = opened.header;
        h.hop_limit =
            ucn_types::HopLimit::new(h.hop_limit.get().checked_sub(1).ok_or(Error::Exhausted)?)
                .map_err(|_| Error::Exhausted)?;
        self.seal_c1_hop(
            provider,
            outbound_session,
            outbound,
            access,
            &packet[..packet.len() - 16],
            h,
            counter,
            work,
            output,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn seal_c1_hop<const B: usize>(
        &mut self,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        origin: &[u8],
        header: CommonHeader,
        counter: &mut (impl HopSequenceOwner + ?Sized),
        work: &mut SecurityPacketWorkspace<B>,
        output: &mut [u8],
    ) -> Result<usize> {
        let s = shape(origin, self.address_width())?;
        let c = self.c1_hop_access(session, facts, access, s, AccessDirection::Outbound)?;
        let n = origin.len().checked_add(16).ok_or(Error::NoSpace)?;
        if n > output.len() || n > B {
            return Err(Error::NoSpace);
        }
        let sequence = counter.preview()?;
        self.preflight_transmit(session, facts, None, None, Some(sequence.get()))?;
        work.packet[..origin.len()].copy_from_slice(origin);
        header.encode(&mut work.packet);
        let aad = build_hop_aad(
            &work.packet[..origin.len()],
            sequence.get(),
            c.hop_fingerprint,
            &mut work.aad,
        )?;
        let claim = self.next_claim(CALLBACK_CRYPTO)?;
        let gate = self.gate();
        let lease = gate.try_enter(claim)?;
        let mut tag = [0; 12];
        let result = counter.burn(sequence).and_then(|()| {
            self.consume_transmit(session, None, None, Some(sequence.get()))?;
            provider.compute_hop_tag(c.hop_tx.ok_or(Error::State)?, &work.aad[..aad], &mut tag)
        });
        let left = gate.leave(lease);
        result?;
        left?;
        work.packet[origin.len()..origin.len() + 4].copy_from_slice(&sequence.get().to_be_bytes());
        work.packet[origin.len() + 4..n].copy_from_slice(&tag);
        output[..n].copy_from_slice(&work.packet[..n]);
        Ok(n)
    }

    /// Commit only neighbour replay after a local queue/delivery acceptance.
    /// 本地队列或交付接纳后只提交邻居Replay，不声称远端交付。
    /// # Errors
    /// Expired, wrong or already consumed proof rejects.
    #[allow(clippy::needless_pass_by_value)]
    pub fn commit_c1_hop(
        &mut self,
        opened: C1HopOpened,
        facts: CurrentFacts,
    ) -> Result<crate::ReplayEvidence> {
        if self.callback_gate().is_active() || opened.owner != self.owner_instance() {
            return Err(Error::State);
        }
        match self.commit_replay(opened.session, facts, opened.replay, true) {
            Ok(evidence) => Ok(evidence),
            Err(error) => {
                let _ = self.abort_replay(opened.session, opened.replay, true);
                Err(error)
            }
        }
    }
    /// Abort reservation on rejected routing/Endpoint preflight. 拒绝路由或Endpoint预检时释放预留。
    /// # Errors
    /// Foreign/consumed proof or callback reentry rejects.
    #[allow(clippy::needless_pass_by_value)]
    pub fn abort_c1_hop(&mut self, opened: C1HopOpened) -> Result<()> {
        if self.callback_gate().is_active() || opened.owner != self.owner_instance() {
            return Err(Error::State);
        }
        self.abort_replay(opened.session, opened.replay, true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn all_address_widths_and_truncated_ciphertexts_are_checked_without_payload_parsing() {
        for width in [
            ucn_types::AddressWidth::A0,
            ucn_types::AddressWidth::A1,
            ucn_types::AddressWidth::A2,
            ucn_types::AddressWidth::A3,
        ] {
            let w = width.bytes();
            let n = 9 + 2 * w;
            let mut packet = [0u8; 64];
            CommonHeader {
                contract: HeaderContract::C1,
                origin_security: OriginSecurity::O2,
                traffic_class: ucn_types::TrafficClass::Q1,
                delivery: ucn_types::DeliveryGuarantee::BestEffort,
                interaction: ucn_types::InteractionRole::OneWay,
                payload_kind: ucn_types::PayloadKind::Data,
                hop_limit: ucn_types::HopLimit::new(3).unwrap(),
            }
            .encode(&mut packet);
            packet[3 + w - 1] = 7;
            packet[3 + 2 * w - 1] = 9;
            packet[4 + 2 * w] = 0x40;
            packet[n - 1] = 1;
            assert_eq!(shape(&packet[..n + 16], width).unwrap().source.get(), 7);
            for end in 0..n + 16 {
                assert!(shape(&packet[..end], width).is_err());
            }
            packet[n - 1] = 0;
            assert!(shape(&packet[..n + 16], width).is_err());
            packet[n - 1] = 1;
            packet[1] |= 1;
            assert!(matches!(
                shape(&packet[..n + 16], width),
                Err(Error::Unsupported)
            ));
        }
    }
}
