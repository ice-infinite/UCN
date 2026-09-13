use ucn_persistence::{CodecWorkspace, blake2s128};
use ucn_types::{
    C0TransactionId, ContextId, Error, ForwardingLabel, HeaderContract, HopProfile,
    InteractionRole, OriginSecurity, OriginSequence, PayloadKind, ProtocolOpcode, Result,
    SenderSlot,
};
use ucn_wire::CommonHeader;

use crate::crypto::{CryptoProvider, OpenOriginRequest, SealOriginRequest};
use crate::replay::ReplayMutationHandle;
use crate::session::{
    AccessDirection, AccessRequest, C0TransactionOwner, CALLBACK_CRYPTO, CurrentFacts,
    HopSequenceOwner, OriginCounterOwner, OriginSequenceOwner, SecurityOwner, SessionHandle,
};

/// Origin Tag 固定长度。
pub const ORIGIN_TAG_BYTES: usize = 16;
/// Hop Tag 固定长度。
pub const HOP_TAG_BYTES: usize = 12;
/// Hop Sequence + Hop Tag 固定长度。
pub const HOP_TRAILER_BYTES: usize = 16;
/// O2 Nonce 固定长度。
pub const NONCE_BYTES: usize = 12;
/// 当前 canonical identity 的固定上限。
pub const MAX_CANONICAL_IDENTITY_BYTES: usize = 96;
/// Nonce input 的最大长度。
pub(crate) const NONCE_INPUT_BYTES: usize = 45;
/// Hop AAD 在原报文之外的固定开销。
pub(crate) const HOP_AAD_FIXED_BYTES: usize = 35;

const ORIGIN_DOMAIN: &[u8] = b"UCN6-ORIGIN-V1";
const HOP_DOMAIN: &[u8] = b"UCN6-HOP-V1";
const SEQUENCE_NONCE_DOMAIN: &[u8] = b"UCN6-NONCE-SEQ-V1";
const C0_NONCE_DOMAIN: &[u8] = b"UCN6-NONCE-C0-V1";

/// 128-bit canonical context fingerprint。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Fingerprint([u8; 16]);

impl Fingerprint {
    /// 构造非零 fingerprint。
    ///
    /// # Errors
    ///
    /// 全零值返回 [`Error::Argument`]。
    pub const fn new(bytes: [u8; 16]) -> Result<Self> {
        let mut index = 0;
        while index < bytes.len() {
            if bytes[index] != 0 {
                return Ok(Self(bytes));
            }
            index += 1;
        }
        Err(Error::Argument)
    }

    /// 返回固定字节。
    #[must_use]
    pub const fn bytes(self) -> [u8; 16] {
        self.0
    }
}

/// C0 事务的发送方向。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SenderDirection {
    /// Initiator 到 Responder。
    InitiatorToResponder = 0,
    /// Responder 到 Initiator。
    ResponderToInitiator = 1,
}

/// 端到端保护所需的不可变 Context 事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OriginContext {
    /// C3 等只做逐跳认证、不携带 Origin 序号的报文。
    None {
        /// 必须匹配 Packet Header Contract。
        contract: HeaderContract,
        /// 供完整 Context 绑定使用，不参与 Nonce。
        fingerprint: Fingerprint,
    },
    /// C0 使用 Transaction/Opcode/方向派生 Nonce。
    C0 {
        /// canonical C0 上下文摘要。
        fingerprint: Fingerprint,
        /// 不回绕事务 ID。
        transaction_id: C0TransactionId,
        /// Header 中的唯一 Opcode。
        opcode: ProtocolOpcode,
        /// 方向。
        direction: SenderDirection,
    },
    /// C1/C2/C4/C5 使用 Origin Sequence 派生 Nonce。
    Sequenced {
        /// 必须匹配 Packet Header Contract。
        contract: HeaderContract,
        /// canonical Origin Context 摘要。
        fingerprint: Fingerprint,
        /// 不回绕 Origin Sequence。
        sequence: OriginSequence,
    },
}

/// 一次受保护 Packet 的只读输入计划。
///
/// `prefix` 是 Common Header 与 Contract fields，`canonical_identity` 是该 Contract 在 Origin
/// AAD 中的完整、不含长度的 canonical identity。Security Owner 会在 Provider 调用前验证
/// Header、Profile、序号和 Payload code。
#[derive(Clone, Copy)]
pub struct PacketPlan<'a> {
    /// Header + Contract fields，不含 Payload/Tag。
    pub prefix: &'a [u8],
    /// 明文 Payload；O2 时由 Provider 加密。
    pub payload: &'a [u8],
    /// AAD 中的完整 canonical identity。
    pub canonical_identity: &'a [u8],
    /// Origin Context。
    pub origin_context: OriginContext,
    /// 由已认证 Context 选择的 Hop Profile。
    pub hop_profile: HopProfile,
}

/// 接收路径对完整受保护 Packet 的解析计划。
#[derive(Clone, Copy)]
pub struct OpenPacketPlan<'a> {
    /// 完整 Packet，包括 Origin/Hop trailer。
    pub packet: &'a [u8],
    /// Header + Contract fields 的精确长度。
    pub prefix_bytes: usize,
    /// Origin AAD 的完整 canonical identity。
    pub canonical_identity: &'a [u8],
    /// Origin Context。
    pub origin_context: OriginContext,
    /// 由已认证 Context 选择的 Hop Profile。
    pub hop_profile: HopProfile,
}

/// 调用方静态持有的受保护 Packet scratch。
pub struct SecurityPacketWorkspace<const BYTES: usize> {
    packet: [u8; BYTES],
    aad: [u8; BYTES],
    plaintext: [u8; BYTES],
    codec: CodecWorkspace,
}

impl<const BYTES: usize> SecurityPacketWorkspace<BYTES> {
    /// 建立零化 scratch；它不保存长期密钥或协议状态。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            packet: [0; BYTES],
            aad: [0; BYTES],
            plaintext: [0; BYTES],
            codec: CodecWorkspace::new(),
        }
    }
}

impl<const BYTES: usize> Default for SecurityPacketWorkspace<BYTES> {
    fn default() -> Self {
        Self::new()
    }
}

/// 已认证输入相对 Origin Replay Window 的语义分类。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OpenDisposition {
    /// 首次认证输入，持有待提交 mutation。
    FreshAuthenticated,
    /// 已提交 Origin Sequence 的认证重放候选；只允许查询 Reliable receipt。
    AuthenticatedReplayCandidate,
}

/// Security Owner 签发的组合 Replay mutation；字段不向其他 Owner 开放。
#[derive(Debug, Eq, PartialEq)]
pub struct SecurityReplayHandle {
    owner_instance: u32,
    session: SessionHandle,
    origin: Option<ReplayMutationHandle>,
    hop: Option<ReplayMutationHandle>,
    disposition: OpenDisposition,
}

/// 接收路径认证、ACL 与 Replay reservation 成功后的结果。
#[derive(Debug, Eq, PartialEq)]
pub struct OpenedPacket {
    /// Fresh 时复制到调用方预检缓冲的明文长度；Replay candidate 固定为零。
    pub payload_bytes: usize,
    /// 本次认证结果的语义分类。
    pub disposition: OpenDisposition,
    /// 已提交 Origin Sequence 的只读重放证据；Fresh 固定为 `None`。
    pub duplicate_evidence: Option<crate::ReplayEvidence>,
    /// 必须由 Coordinator 在业务预检后精确 commit/abort 的组合 Handle。
    pub replay: SecurityReplayHandle,
}

/// 组合 Replay mutation 成功提交后的不可变证据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReplayCommitEvidence {
    /// 本次提交对应的语义分类。
    pub disposition: OpenDisposition,
    /// Fresh Origin mutation 的提交证据；无 Origin Replay 域时为 `None`。
    pub origin: Option<crate::ReplayEvidence>,
    /// Fresh Hop mutation 的提交证据；H0/H2 时为 `None`。
    pub hop: Option<crate::ReplayEvidence>,
}

enum PreparedOriginCounter<'a> {
    None,
    Transaction(&'a mut dyn C0TransactionOwner, C0TransactionId),
    Sequence(&'a mut dyn OriginSequenceOwner, OriginSequence),
}

impl<const SESSIONS: usize, const REPLAY_SLOTS: usize> SecurityOwner<'_, SESSIONS, REPLAY_SLOTS> {
    /// 生成 O1/O2/H1/H2/H3 受保护 Packet；失败时输出完全不写。
    ///
    /// # Errors
    ///
    /// Session、ACL、Wire、计数器、Provider 或输出容量任一检查失败时返回错误；调用方
    /// 输出保持不变，已交给 Provider 的计数器值不会复用。
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn protect_packet<P: CryptoProvider, const BYTES: usize>(
        &mut self,
        provider: &mut P,
        origin_counter: OriginCounterOwner<'_>,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: PacketPlan<'_>,
        hop_sequence_owner: Option<&mut dyn HopSequenceOwner>,
        workspace: &mut SecurityPacketWorkspace<BYTES>,
        output: &mut [u8],
    ) -> Result<usize> {
        if access.session != session || access.direction != AccessDirection::Outbound {
            return Err(Error::Argument);
        }
        let candidate = self.candidate(session, facts)?;
        let header = self.validate_packet_plan(&plan)?;
        if matches!(header.contract, HeaderContract::C4 | HeaderContract::C5) {
            return Err(Error::Unsupported);
        }
        validate_context(candidate.origin_fingerprint, plan.origin_context)?;
        validate_wire_bindings(
            candidate,
            self.realm(),
            self.address_width().bytes(),
            header,
            plan.prefix,
            plan.canonical_identity,
            AccessDirection::Outbound,
        )?;
        if header.origin_security != OriginSecurity::O0
            && header.origin_security != candidate.origin_level.wire()
        {
            return Err(Error::Security);
        }
        if header.origin_security == OriginSecurity::O0 && plan.hop_profile == HopProfile::H0 {
            return Err(Error::Security);
        }
        if plan.hop_profile != candidate.hop_profile {
            return Err(Error::Security);
        }
        validate_access_context(
            access,
            header,
            plan.origin_context,
            candidate.hop_fingerprint,
        )?;
        self.authorize(access, facts)?;
        let origin_bytes =
            usize::from(header.origin_security != OriginSecurity::O0) * ORIGIN_TAG_BYTES;
        let hop_bytes = usize::from(matches!(plan.hop_profile, HopProfile::H1 | HopProfile::H3))
            * HOP_TRAILER_BYTES;
        let packet_bytes = plan
            .prefix
            .len()
            .checked_add(plan.payload.len())
            .and_then(|value| value.checked_add(origin_bytes + hop_bytes))
            .ok_or(Error::NoSpace)?;
        if output.len() < packet_bytes
            || workspace.packet.len() < packet_bytes
            || workspace.plaintext.len() < plan.payload.len()
        {
            return Err(Error::NoSpace);
        }
        validate_payload_code(header, plan.prefix, plan.payload)?;
        let canonical_opcode = canonical_protocol_opcode(header, plan.prefix, plan.payload)?;
        if access.protocol_opcode != canonical_opcode {
            return Err(Error::Access);
        }
        validate_c1_service(self.address_width().bytes(), header, plan.prefix, access)?;
        workspace.packet[..plan.prefix.len()].copy_from_slice(plan.prefix);
        let payload_start = plan.prefix.len();
        let payload_end = payload_start + plan.payload.len();
        let aad_bytes = if header.origin_security == OriginSecurity::O0 {
            0
        } else {
            build_origin_aad(&plan, header, &mut workspace.aad)?
        };
        let mut nonce_input = [0_u8; NONCE_INPUT_BYTES];
        let mut derived_nonce = [0_u8; NONCE_BYTES];
        let nonce_bytes = if header.origin_security == OriginSecurity::O2 {
            build_nonce_input(plan.origin_context, header.interaction, &mut nonce_input)?
        } else {
            0
        };
        if aad_bytes > workspace.aad.len()
            || (hop_bytes != 0
                && payload_end + origin_bytes + HOP_AAD_FIXED_BYTES > workspace.aad.len())
        {
            return Err(Error::NoSpace);
        }
        let prepared_origin = match (plan.origin_context, origin_counter) {
            (OriginContext::Sequenced { sequence, .. }, OriginCounterOwner::Sequence(owner)) => {
                if owner.preview()? != sequence {
                    return Err(Error::State);
                }
                PreparedOriginCounter::Sequence(owner, sequence)
            }
            (OriginContext::C0 { transaction_id, .. }, OriginCounterOwner::Transaction(owner)) => {
                if owner.preview()? != transaction_id {
                    return Err(Error::State);
                }
                PreparedOriginCounter::Transaction(owner, transaction_id)
            }
            (OriginContext::None { .. }, OriginCounterOwner::None) => PreparedOriginCounter::None,
            _ => return Err(Error::Argument),
        };
        let prepared_hop = match (plan.hop_profile, hop_sequence_owner) {
            (HopProfile::H1 | HopProfile::H3, Some(owner)) => {
                let sequence = owner.preview()?;
                Some((owner, sequence))
            }
            (HopProfile::H0 | HopProfile::H2, None) => None,
            _ => return Err(Error::Argument),
        };
        let hop_sequence = prepared_hop.as_ref().map(|(_, sequence)| *sequence);
        let claim = self.next_claim(CALLBACK_CRYPTO)?;
        let gate = self.gate();
        let lease = gate.try_enter(claim)?;
        let crypto_result = (|| {
            match prepared_origin {
                PreparedOriginCounter::None => {}
                PreparedOriginCounter::Transaction(owner, transaction) => {
                    owner.burn(transaction)?;
                }
                PreparedOriginCounter::Sequence(owner, sequence) => owner.burn(sequence)?,
            }
            if let Some((owner, sequence)) = prepared_hop {
                owner.burn(sequence)?;
            }
            match header.origin_security {
                OriginSecurity::O0 => {
                    workspace.packet[payload_start..payload_end].copy_from_slice(plan.payload);
                }
                OriginSecurity::O1 => {
                    workspace.packet[payload_start..payload_end].copy_from_slice(plan.payload);
                    let mut tag = [0_u8; ORIGIN_TAG_BYTES];
                    provider.compute_origin_tag(
                        candidate.origin_tx,
                        &workspace.aad[..aad_bytes],
                        plan.payload,
                        &mut tag,
                    )?;
                    workspace.packet[payload_end..payload_end + ORIGIN_TAG_BYTES]
                        .copy_from_slice(&tag);
                }
                OriginSecurity::O2 => {
                    let mut tag = [0_u8; ORIGIN_TAG_BYTES];
                    provider.seal_origin(SealOriginRequest {
                        selector: candidate.origin_tx,
                        nonce_input: &nonce_input[..nonce_bytes],
                        aad: &workspace.aad[..aad_bytes],
                        plaintext: plan.payload,
                        ciphertext: &mut workspace.packet[payload_start..payload_end],
                        tag: &mut tag,
                        derived_nonce: &mut derived_nonce,
                    })?;
                    workspace.packet[payload_end..payload_end + ORIGIN_TAG_BYTES]
                        .copy_from_slice(&tag);
                }
            }
            if matches!(plan.hop_profile, HopProfile::H1 | HopProfile::H3) {
                let sequence = hop_sequence.ok_or(Error::State)?.get();
                let without_hop = payload_end + origin_bytes;
                let hop_aad_bytes = build_hop_aad(
                    &workspace.packet[..without_hop],
                    sequence,
                    candidate.hop_fingerprint,
                    &mut workspace.aad,
                )?;
                let mut tag = [0_u8; HOP_TAG_BYTES];
                provider.compute_hop_tag(
                    candidate.hop_tx.ok_or(Error::State)?,
                    &workspace.aad[..hop_aad_bytes],
                    &mut tag,
                )?;
                workspace.packet[without_hop..without_hop + 4]
                    .copy_from_slice(&sequence.to_be_bytes());
                workspace.packet[without_hop + 4..without_hop + HOP_TRAILER_BYTES]
                    .copy_from_slice(&tag);
            }
            Ok(())
        })();
        let leave_result = gate.leave(lease);
        crypto_result?;
        leave_result?;
        output[..packet_bytes].copy_from_slice(&workspace.packet[..packet_bytes]);
        Ok(packet_bytes)
    }

    /// 完成 Hop/Origin 验证、精确 ACL、两阶段 Replay 后才发布明文。
    ///
    /// # Errors
    ///
    /// Session、ACL、Wire、认证、解密、Replay 或输出容量任一检查失败时返回错误；明文
    /// 输出保持不变，失败的 Replay reservation 被回滚。
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn open_packet<P: CryptoProvider, const BYTES: usize>(
        &mut self,
        provider: &mut P,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        workspace: &mut SecurityPacketWorkspace<BYTES>,
        plaintext_output: &mut [u8],
    ) -> Result<OpenedPacket> {
        if access.session != session || access.direction != AccessDirection::Inbound {
            return Err(Error::Argument);
        }
        let candidate = self.candidate(session, facts)?;
        if plan.packet.len() > workspace.packet.len() || plan.prefix_bytes > plan.packet.len() {
            return Err(Error::NoSpace);
        }
        let header = CommonHeader::decode(plan.packet)?;
        if matches!(header.contract, HeaderContract::C4 | HeaderContract::C5) {
            return Err(Error::Unsupported);
        }
        let origin_bytes =
            usize::from(header.origin_security != OriginSecurity::O0) * ORIGIN_TAG_BYTES;
        let hop_bytes = usize::from(matches!(plan.hop_profile, HopProfile::H1 | HopProfile::H3))
            * HOP_TRAILER_BYTES;
        let payload_end = plan
            .packet
            .len()
            .checked_sub(origin_bytes + hop_bytes)
            .ok_or(Error::Malformed)?;
        if payload_end < plan.prefix_bytes {
            return Err(Error::Malformed);
        }
        let payload = &plan.packet[plan.prefix_bytes..payload_end];
        let structural = PacketPlan {
            prefix: &plan.packet[..plan.prefix_bytes],
            payload,
            canonical_identity: plan.canonical_identity,
            origin_context: plan.origin_context,
            hop_profile: plan.hop_profile,
        };
        let checked_header = self.validate_packet_plan(&structural)?;
        if checked_header != header
            || plan.hop_profile != candidate.hop_profile
            || (header.origin_security != OriginSecurity::O0
                && header.origin_security != candidate.origin_level.wire())
            || (header.origin_security == OriginSecurity::O0 && plan.hop_profile == HopProfile::H0)
        {
            return Err(Error::Security);
        }
        validate_context(candidate.origin_fingerprint, plan.origin_context)?;
        validate_wire_bindings(
            candidate,
            self.realm(),
            self.address_width().bytes(),
            header,
            structural.prefix,
            structural.canonical_identity,
            AccessDirection::Inbound,
        )?;
        validate_access_context(
            access,
            header,
            plan.origin_context,
            candidate.hop_fingerprint,
        )?;
        if plaintext_output.len() < payload.len() || workspace.plaintext.len() < payload.len() {
            return Err(Error::NoSpace);
        }

        let aad_bytes = if header.origin_security == OriginSecurity::O0 {
            0
        } else {
            build_origin_aad(&structural, header, &mut workspace.aad)?
        };
        let hop_sequence = if hop_bytes == 0 {
            None
        } else {
            Some(u32::from_be_bytes(
                plan.packet[payload_end + origin_bytes..payload_end + origin_bytes + 4]
                    .try_into()
                    .map_err(|_| Error::Malformed)?,
            ))
        };
        let hop_aad_bytes = if let Some(sequence) = hop_sequence {
            build_hop_aad(
                &plan.packet[..payload_end + origin_bytes],
                sequence,
                candidate.hop_fingerprint,
                &mut workspace.packet,
            )?
        } else {
            0
        };
        let mut nonce_input = [0_u8; NONCE_INPUT_BYTES];
        let nonce_bytes = if header.origin_security == OriginSecurity::O2 {
            build_nonce_input(plan.origin_context, header.interaction, &mut nonce_input)?
        } else {
            0
        };
        let mut derived_nonce = [0_u8; NONCE_BYTES];
        let claim = self.next_claim(CALLBACK_CRYPTO)?;
        let gate = self.gate();
        let lease = gate.try_enter(claim)?;
        let crypto_result = (|| {
            if let Some(_sequence) = hop_sequence {
                let tag: &[u8; HOP_TAG_BYTES] = plan.packet[plan.packet.len() - HOP_TAG_BYTES..]
                    .try_into()
                    .map_err(|_| Error::Malformed)?;
                provider.verify_hop_tag(
                    candidate.hop_rx.ok_or(Error::State)?,
                    &workspace.packet[..hop_aad_bytes],
                    tag,
                )?;
            }
            match header.origin_security {
                OriginSecurity::O0 => {
                    workspace.plaintext[..payload.len()].copy_from_slice(payload);
                }
                OriginSecurity::O1 => {
                    let tag: &[u8; ORIGIN_TAG_BYTES] = plan.packet
                        [payload_end..payload_end + ORIGIN_TAG_BYTES]
                        .try_into()
                        .map_err(|_| Error::Malformed)?;
                    provider.verify_origin_tag(
                        candidate.origin_rx,
                        &workspace.aad[..aad_bytes],
                        payload,
                        tag,
                    )?;
                    workspace.plaintext[..payload.len()].copy_from_slice(payload);
                }
                OriginSecurity::O2 => {
                    let tag: &[u8; ORIGIN_TAG_BYTES] = plan.packet
                        [payload_end..payload_end + ORIGIN_TAG_BYTES]
                        .try_into()
                        .map_err(|_| Error::Malformed)?;
                    provider.open_origin(OpenOriginRequest {
                        selector: candidate.origin_rx,
                        nonce_input: &nonce_input[..nonce_bytes],
                        aad: &workspace.aad[..aad_bytes],
                        ciphertext: payload,
                        tag,
                        plaintext: &mut workspace.plaintext[..payload.len()],
                        derived_nonce: &mut derived_nonce,
                    })?;
                }
            }
            Ok(())
        })();
        let leave_result = gate.leave(lease);
        crypto_result?;
        leave_result?;
        validate_payload_code(
            header,
            structural.prefix,
            &workspace.plaintext[..payload.len()],
        )?;
        let canonical_opcode = canonical_protocol_opcode(
            header,
            structural.prefix,
            &workspace.plaintext[..payload.len()],
        )?;
        if access.protocol_opcode != canonical_opcode {
            return Err(Error::Access);
        }
        validate_c1_service(
            self.address_width().bytes(),
            header,
            structural.prefix,
            access,
        )?;

        let aad_digest = blake2s128(&workspace.aad[..aad_bytes], &mut workspace.codec);
        let payload_digest =
            blake2s128(&workspace.plaintext[..payload.len()], &mut workspace.codec);
        let hop_aad_digest = if hop_aad_bytes == 0 {
            aad_digest
        } else {
            blake2s128(&workspace.packet[..hop_aad_bytes], &mut workspace.codec)
        };
        let origin_sequence = origin_replay_sequence(plan.origin_context, header.origin_security)?;
        let mut origin_handle = None;
        let mut hop_handle = None;
        let mut disposition = OpenDisposition::FreshAuthenticated;
        let mut duplicate_evidence = None;
        if let Some(sequence) = origin_sequence {
            match self.classify_origin_replay(session, facts, sequence)? {
                crate::ReplayClassification::Fresh => {
                    origin_handle = Some(self.reserve_origin_replay(
                        session,
                        facts,
                        sequence,
                        aad_digest,
                        payload_digest,
                    )?);
                }
                crate::ReplayClassification::Duplicate => {
                    disposition = OpenDisposition::AuthenticatedReplayCandidate;
                    duplicate_evidence = Some(self.origin_replay_evidence(
                        session,
                        facts,
                        sequence,
                        aad_digest,
                        payload_digest,
                    )?);
                }
                crate::ReplayClassification::Stale => return Err(Error::Replay),
                crate::ReplayClassification::InFlight => return Err(Error::State),
            }
        }
        if let Some(sequence) = hop_sequence {
            match self.reserve_hop_replay(
                session,
                facts,
                u64::from(sequence),
                hop_aad_digest,
                payload_digest,
            ) {
                Ok(handle) => hop_handle = Some(handle),
                Err(error) => {
                    if let Some(handle) = origin_handle {
                        let _ = self.abort_replay(session, handle, false);
                    }
                    return Err(error);
                }
            }
        }
        if let Err(error) = self.authorize(access, facts) {
            if let Some(handle) = origin_handle {
                let _ = self.abort_replay(session, handle, false);
            }
            if let Some(handle) = hop_handle {
                let _ = self.abort_replay(session, handle, true);
            }
            return Err(error);
        }
        let payload_bytes = if disposition == OpenDisposition::FreshAuthenticated {
            plaintext_output[..payload.len()]
                .copy_from_slice(&workspace.plaintext[..payload.len()]);
            payload.len()
        } else {
            0
        };
        Ok(OpenedPacket {
            payload_bytes,
            disposition,
            duplicate_evidence,
            replay: SecurityReplayHandle {
                owner_instance: self.owner_instance(),
                session,
                origin: origin_handle,
                hop: hop_handle,
                disposition,
            },
        })
    }

    /// 在所有业务资源预检成功后原子提交组合 Replay mutation。
    ///
    /// # Errors
    ///
    /// Session/Key/Handle 代际、当前事实或 reservation Deadline 不再匹配时，释放本组合仍
    /// 存在的 reservation 并返回错误；不得执行业务副作用。
    #[allow(clippy::needless_pass_by_value)] // Handle 必须以所有权语义一次性消费。
    pub fn replay_commit(
        &mut self,
        handle: SecurityReplayHandle,
        facts: CurrentFacts,
    ) -> Result<ReplayCommitEvidence> {
        let SecurityReplayHandle {
            owner_instance,
            session,
            origin,
            hop,
            disposition,
        } = handle;
        if owner_instance != self.owner_instance() {
            return Err(Error::NotFound);
        }
        let session_check = self.candidate(session, facts).map(|_| ());
        let origin_check = origin.map_or(Ok(()), |value| {
            self.preflight_replay(session, facts, value, false)
        });
        let hop_check = hop.map_or(Ok(()), |value| {
            self.preflight_replay(session, facts, value, true)
        });
        if let Err(error) = session_check.and(origin_check).and(hop_check) {
            if let Some(value) = origin {
                let _ = self.abort_replay(session, value, false);
            }
            if let Some(value) = hop {
                let _ = self.abort_replay(session, value, true);
            }
            return Err(error);
        }
        let origin_evidence = match origin {
            Some(value) => Some(self.commit_replay(session, facts, value, false)?),
            None => None,
        };
        let hop_evidence = match hop {
            Some(value) => Some(self.commit_replay(session, facts, value, true)?),
            None => None,
        };
        Ok(ReplayCommitEvidence {
            disposition,
            origin: origin_evidence,
            hop: hop_evidence,
        })
    }

    /// 在业务预检失败时释放组合 Replay mutation，不把序号标记为已见。
    ///
    /// # Errors
    ///
    /// Handle 已提交、已释放、所属 Owner/Session 不匹配或 Session 已清理时返回错误。
    #[allow(clippy::needless_pass_by_value)] // Handle 必须以所有权语义一次性消费。
    pub fn replay_abort(&mut self, handle: SecurityReplayHandle) -> Result<()> {
        let SecurityReplayHandle {
            owner_instance,
            session,
            origin,
            hop,
            disposition: _,
        } = handle;
        if owner_instance != self.owner_instance() {
            return Err(Error::NotFound);
        }
        let mut result = Ok(());
        if let Some(value) = origin {
            if let Err(error) = self.abort_replay(session, value, false) {
                result = Err(error);
            }
        }
        if let Some(value) = hop {
            if let Err(error) = self.abort_replay(session, value, true) {
                result = Err(error);
            }
        }
        result
    }

    fn validate_packet_plan(&self, plan: &PacketPlan<'_>) -> Result<CommonHeader> {
        match CommonHeader::decode(plan.prefix)?.contract {
            HeaderContract::C0 | HeaderContract::C1 => {
                validate_addressed_prefix(plan, self.address_width().bytes())
            }
            _ => validate_plan(plan),
        }
    }
}

pub(crate) fn validate_plan(plan: &PacketPlan<'_>) -> Result<CommonHeader> {
    if plan.canonical_identity.is_empty()
        || plan.canonical_identity.len() > MAX_CANONICAL_IDENTITY_BYTES
    {
        return Err(Error::Argument);
    }
    let header = CommonHeader::decode(plan.prefix)?;
    let base = base_header_bytes(header.contract)?;
    if plan.prefix.len() != base {
        return Err(Error::Malformed);
    }
    validate_combination(header, plan.hop_profile)?;
    validate_contract_aliases(header.contract, plan.prefix)?;
    if header.origin_security != OriginSecurity::O2 {
        validate_payload_code(header, plan.prefix, plan.payload)?;
    }
    match plan.origin_context {
        OriginContext::None { contract, .. } => {
            if header.contract != contract || header.origin_security != OriginSecurity::O0 {
                return Err(Error::State);
            }
        }
        OriginContext::C0 {
            transaction_id,
            opcode,
            ..
        } => {
            if header.contract != HeaderContract::C0
                || transaction_id.get() != read_u64_be(plan.prefix, base - 10)
                || u16::from(opcode) != read_u16_be(plan.prefix, base - 2)
            {
                return Err(Error::State);
            }
        }
        OriginContext::Sequenced {
            contract, sequence, ..
        } => {
            let offset = sequence_offset(contract).ok_or(Error::Argument)?;
            if header.contract != contract || sequence.get() != read_u32_be(plan.prefix, offset) {
                return Err(Error::State);
            }
        }
    }
    Ok(header)
}

pub(crate) fn build_origin_aad(
    plan: &PacketPlan<'_>,
    header: CommonHeader,
    output: &mut [u8],
) -> Result<usize> {
    let length = ORIGIN_DOMAIN.len() + 4 + 3 + 1 + plan.canonical_identity.len() + 4;
    if output.len() < length {
        return Err(Error::NoSpace);
    }
    let after_length = 3_usize
        .checked_add(1)
        .and_then(|value| value.checked_add(plan.canonical_identity.len()))
        .and_then(|value| value.checked_add(4))
        .ok_or(Error::NoSpace)?;
    let after_length = u32::try_from(after_length).map_err(|_| Error::NoSpace)?;
    let mut cursor = 0;
    output[cursor..cursor + ORIGIN_DOMAIN.len()].copy_from_slice(ORIGIN_DOMAIN);
    cursor += ORIGIN_DOMAIN.len();
    output[cursor..cursor + 4].copy_from_slice(&after_length.to_be_bytes());
    cursor += 4;
    output[cursor..cursor + 3].copy_from_slice(&plan.prefix[..3]);
    output[cursor + 2] &= 0xC0;
    cursor += 3;
    output[cursor] = u8::from(header.contract);
    cursor += 1;
    output[cursor..cursor + plan.canonical_identity.len()].copy_from_slice(plan.canonical_identity);
    cursor += plan.canonical_identity.len();
    let payload_len = u32::try_from(plan.payload.len()).map_err(|_| Error::NoSpace)?;
    output[cursor..cursor + 4].copy_from_slice(&payload_len.to_be_bytes());
    Ok(cursor + 4)
}

pub(crate) fn build_nonce_input(
    context: OriginContext,
    interaction: InteractionRole,
    output: &mut [u8; NONCE_INPUT_BYTES],
) -> Result<usize> {
    match context {
        OriginContext::None { .. } => Err(Error::Argument),
        OriginContext::Sequenced {
            fingerprint,
            sequence,
            ..
        } => {
            let length = SEQUENCE_NONCE_DOMAIN.len() + 16 + 4;
            output[..SEQUENCE_NONCE_DOMAIN.len()].copy_from_slice(SEQUENCE_NONCE_DOMAIN);
            let mut cursor = SEQUENCE_NONCE_DOMAIN.len();
            output[cursor..cursor + 16].copy_from_slice(&fingerprint.bytes());
            cursor += 16;
            output[cursor..cursor + 4].copy_from_slice(&sequence.get().to_be_bytes());
            Ok(length)
        }
        OriginContext::C0 {
            fingerprint,
            transaction_id,
            opcode,
            direction,
        } => {
            let length = C0_NONCE_DOMAIN.len() + 16 + 8 + 2 + 1 + 1;
            output[..C0_NONCE_DOMAIN.len()].copy_from_slice(C0_NONCE_DOMAIN);
            let mut cursor = C0_NONCE_DOMAIN.len();
            output[cursor..cursor + 16].copy_from_slice(&fingerprint.bytes());
            cursor += 16;
            output[cursor..cursor + 8].copy_from_slice(&transaction_id.get().to_be_bytes());
            cursor += 8;
            output[cursor..cursor + 2].copy_from_slice(&u16::from(opcode).to_be_bytes());
            cursor += 2;
            output[cursor] = direction as u8;
            output[cursor + 1] = u8::from(interaction);
            Ok(length)
        }
    }
}

pub(crate) fn build_hop_aad(
    packet_without_hop_trailer: &[u8],
    hop_sequence: u32,
    hop_fingerprint: Fingerprint,
    output: &mut [u8],
) -> Result<usize> {
    if hop_sequence == 0 {
        return Err(Error::Argument);
    }
    let after_length = packet_without_hop_trailer
        .len()
        .checked_add(4 + 16)
        .ok_or(Error::NoSpace)?;
    let length = HOP_DOMAIN
        .len()
        .checked_add(4)
        .and_then(|value| value.checked_add(after_length))
        .ok_or(Error::NoSpace)?;
    if output.len() < length {
        return Err(Error::NoSpace);
    }
    let mut cursor = 0;
    output[cursor..cursor + HOP_DOMAIN.len()].copy_from_slice(HOP_DOMAIN);
    cursor += HOP_DOMAIN.len();
    output[cursor..cursor + 4].copy_from_slice(
        &u32::try_from(after_length)
            .map_err(|_| Error::NoSpace)?
            .to_be_bytes(),
    );
    cursor += 4;
    output[cursor..cursor + packet_without_hop_trailer.len()]
        .copy_from_slice(packet_without_hop_trailer);
    cursor += packet_without_hop_trailer.len();
    output[cursor..cursor + 4].copy_from_slice(&hop_sequence.to_be_bytes());
    cursor += 4;
    output[cursor..cursor + 16].copy_from_slice(&hop_fingerprint.bytes());
    Ok(cursor + 16)
}

pub(crate) const fn base_header_bytes(contract: HeaderContract) -> Result<usize> {
    match contract {
        HeaderContract::C0 | HeaderContract::C1 => Err(Error::Unsupported),
        HeaderContract::C2 => Ok(9),
        HeaderContract::C3 => Ok(7),
        HeaderContract::C4 => Ok(11),
        HeaderContract::C5 => Ok(13),
    }
}

/// Validate C0/C1 prefixes whose address width is not self-described.
pub(crate) fn validate_addressed_prefix(
    plan: &PacketPlan<'_>,
    address_bytes: usize,
) -> Result<CommonHeader> {
    let header = CommonHeader::decode(plan.prefix)?;
    let expected = match header.contract {
        HeaderContract::C0 => 25 + 2 * address_bytes,
        HeaderContract::C1 => 9 + 2 * address_bytes,
        _ => return validate_plan(plan),
    };
    if !(1..=4).contains(&address_bytes) || plan.prefix.len() != expected {
        return Err(Error::Malformed);
    }
    validate_combination(header, plan.hop_profile)?;
    if header.origin_security != OriginSecurity::O2 {
        validate_payload_code(header, plan.prefix, plan.payload)?;
    }
    match plan.origin_context {
        OriginContext::None { contract, .. } => {
            if header.contract != contract || header.origin_security != OriginSecurity::O0 {
                return Err(Error::State);
            }
        }
        OriginContext::C0 {
            transaction_id,
            opcode,
            ..
        } if header.contract == HeaderContract::C0 => {
            if transaction_id.get() != read_u64_be(plan.prefix, expected - 10)
                || u16::from(opcode) != read_u16_be(plan.prefix, expected - 2)
            {
                return Err(Error::State);
            }
        }
        OriginContext::Sequenced {
            contract: HeaderContract::C1,
            sequence,
            ..
        } if header.contract == HeaderContract::C1 => {
            if sequence.get() != read_u32_be(plan.prefix, 5 + 2 * address_bytes) {
                return Err(Error::State);
            }
        }
        _ => return Err(Error::State),
    }
    Ok(header)
}

fn validate_combination(header: CommonHeader, hop: HopProfile) -> Result<()> {
    use ucn_types::{DeliveryGuarantee as D, InteractionRole as I, PayloadKind as K};
    let valid = match header.contract {
        HeaderContract::C0 => {
            matches!(header.delivery, D::BestEffort | D::Reliable)
                && matches!(header.payload_kind, K::Control | K::Diagnostic)
                && matches!(hop, HopProfile::H0 | HopProfile::H1)
        }
        HeaderContract::C1 | HeaderContract::C4 => {
            matches!(hop, HopProfile::H0 | HopProfile::H1)
        }
        HeaderContract::C2 => {
            matches!(hop, HopProfile::H0 | HopProfile::H1)
                || (hop == HopProfile::H2 && header.origin_security != OriginSecurity::O0)
        }
        HeaderContract::C3 => {
            header.delivery == D::BestEffort
                && header.interaction == I::OneWay
                && matches!(header.payload_kind, K::Data | K::Diagnostic)
                && header.origin_security == OriginSecurity::O0
                && matches!(hop, HopProfile::H0 | HopProfile::H1)
        }
        HeaderContract::C5 => {
            header.interaction != I::Request
                && header.payload_kind != K::Transfer
                && header.origin_security != OriginSecurity::O0
                && matches!(hop, HopProfile::H0 | HopProfile::H3)
        }
    };
    if !valid {
        return Err(Error::Malformed);
    }
    if header.interaction != I::OneWay && header.payload_kind == K::Data {
        return Err(Error::Malformed);
    }
    Ok(())
}

fn validate_contract_aliases(contract: HeaderContract, prefix: &[u8]) -> Result<()> {
    match contract {
        HeaderContract::C0 | HeaderContract::C1 => Ok(()),
        HeaderContract::C2 => ContextId::new(read_u16_be(prefix, 3)).map(|_| ()),
        HeaderContract::C3 => {
            ForwardingLabel::new(read_u16_be(prefix, 3))?;
            ContextId::new(read_u16_be(prefix, 5)).map(|_| ())
        }
        HeaderContract::C4 => {
            ForwardingLabel::new(read_u16_be(prefix, 3))?;
            ContextId::new(read_u16_be(prefix, 5)).map(|_| ())
        }
        HeaderContract::C5 => {
            ForwardingLabel::new(read_u16_be(prefix, 3))?;
            ContextId::new(read_u16_be(prefix, 5))?;
            SenderSlot::new(read_u16_be(prefix, 7)).map(|_| ())
        }
    }
}

pub(crate) fn validate_payload_code(
    header: CommonHeader,
    prefix: &[u8],
    payload: &[u8],
) -> Result<()> {
    match header.payload_kind {
        PayloadKind::Control => {
            let raw = if header.contract == HeaderContract::C0 {
                read_u16_be(prefix, prefix.len() - 2)
            } else {
                if payload.len() < 2 {
                    return Err(Error::Malformed);
                }
                read_u16_be(payload, 0)
            };
            ProtocolOpcode::try_from(raw).map(|_| ())
        }
        PayloadKind::Diagnostic | PayloadKind::Transfer => {
            if header.contract == HeaderContract::C0 || payload.len() >= 2 {
                Ok(())
            } else {
                Err(Error::Malformed)
            }
        }
        PayloadKind::Data => Ok(()),
    }
}

fn validate_context(expected: Fingerprint, context: OriginContext) -> Result<()> {
    let actual = match context {
        OriginContext::None { fingerprint, .. }
        | OriginContext::C0 { fingerprint, .. }
        | OriginContext::Sequenced { fingerprint, .. } => fingerprint,
    };
    if actual == expected {
        Ok(())
    } else {
        Err(Error::Security)
    }
}

fn validate_access_context(
    access: AccessRequest,
    header: CommonHeader,
    origin_context: OriginContext,
    hop_fingerprint: Fingerprint,
) -> Result<()> {
    let expected = if header.origin_security == OriginSecurity::O0 {
        hop_fingerprint
    } else {
        match origin_context {
            OriginContext::None { .. } => return Err(Error::State),
            OriginContext::C0 { fingerprint, .. }
            | OriginContext::Sequenced { fingerprint, .. } => fingerprint,
        }
    };
    if access.context_fingerprint == expected {
        Ok(())
    } else {
        Err(Error::Access)
    }
}

fn origin_replay_sequence(context: OriginContext, security: OriginSecurity) -> Result<Option<u64>> {
    if security == OriginSecurity::O0 {
        return Ok(None);
    }
    match context {
        OriginContext::None { .. } => Err(Error::State),
        OriginContext::C0 { transaction_id, .. } => Ok(Some(transaction_id.get())),
        OriginContext::Sequenced { sequence, .. } => Ok(Some(u64::from(sequence.get()))),
    }
}

fn canonical_protocol_opcode(header: CommonHeader, prefix: &[u8], payload: &[u8]) -> Result<u16> {
    if header.payload_kind != PayloadKind::Control {
        return Ok(0);
    }
    let raw = if header.contract == HeaderContract::C0 {
        read_u16_be(prefix, prefix.len() - 2)
    } else {
        if payload.len() < 2 {
            return Err(Error::Malformed);
        }
        read_u16_be(payload, 0)
    };
    ProtocolOpcode::try_from(raw)?;
    Ok(raw)
}

fn validate_c1_service(
    address_bytes: usize,
    header: CommonHeader,
    prefix: &[u8],
    access: AccessRequest,
) -> Result<()> {
    if header.contract == HeaderContract::C1
        && read_u16_be(prefix, 3 + 2 * address_bytes) != access.service.get()
    {
        return Err(Error::Access);
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn validate_wire_bindings(
    candidate: crate::HandshakeCandidate,
    realm: ucn_types::RealmId,
    width: usize,
    header: CommonHeader,
    prefix: &[u8],
    canonical_identity: &[u8],
    direction: AccessDirection,
) -> Result<()> {
    let (source, destination) = match direction {
        AccessDirection::Outbound => (candidate.local, candidate.peer),
        AccessDirection::Inbound => (candidate.peer, candidate.local),
    };
    match header.contract {
        HeaderContract::C0 => {
            if read_u32_be(prefix, 3) != realm.get()
                || read_address(prefix, 7, width) != source.address.get()
                || read_address(prefix, 7 + width, width) != destination.address.get()
                || read_u32_be(prefix, 7 + 2 * width) != source.generation.get()
                || read_u32_be(prefix, 11 + 2 * width) != destination.generation.get()
                || canonical_identity != &prefix[3..]
            {
                return Err(Error::Security);
            }
        }
        HeaderContract::C1 => {
            if read_address(prefix, 3, width) != source.address.get()
                || read_address(prefix, 3 + width, width) != destination.address.get()
            {
                return Err(Error::Security);
            }
            let mut expected = [0_u8; 46];
            expected[..16].copy_from_slice(&source.principal);
            expected[16..20].copy_from_slice(&source.generation.get().to_be_bytes());
            expected[20..36].copy_from_slice(&destination.principal);
            expected[36..40].copy_from_slice(&destination.generation.get().to_be_bytes());
            expected[40..42].copy_from_slice(&prefix[3 + 2 * width..5 + 2 * width]);
            expected[42..46].copy_from_slice(&prefix[5 + 2 * width..9 + 2 * width]);
            if canonical_identity != expected {
                return Err(Error::Security);
            }
        }
        HeaderContract::C2 => {
            let expected_len = if candidate.hop_profile == HopProfile::H2 {
                36
            } else {
                20
            };
            if canonical_identity.len() != expected_len
                || canonical_identity[..16] != candidate.origin_fingerprint.bytes()
                || read_u32_be(canonical_identity, 16) != read_u32_be(prefix, 5)
                || (candidate.hop_profile == HopProfile::H2
                    && canonical_identity[20..36] != candidate.hop_fingerprint.bytes())
            {
                return Err(Error::Security);
            }
        }
        HeaderContract::C3 | HeaderContract::C4 | HeaderContract::C5 => {}
    }
    Ok(())
}

fn read_address(input: &[u8], offset: usize, width: usize) -> u32 {
    let mut value = 0_u32;
    for byte in &input[offset..offset + width] {
        value = (value << 8) | u32::from(*byte);
    }
    value
}

const fn sequence_offset(contract: HeaderContract) -> Option<usize> {
    match contract {
        HeaderContract::C2 => Some(5),
        HeaderContract::C4 => Some(7),
        HeaderContract::C5 => Some(9),
        _ => None,
    }
}

fn read_u16_be(input: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes([input[offset], input[offset + 1]])
}

fn read_u32_be(input: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes([
        input[offset],
        input[offset + 1],
        input[offset + 2],
        input[offset + 3],
    ])
}

fn read_u64_be(input: &[u8], offset: usize) -> u64 {
    u64::from_be_bytes([
        input[offset],
        input[offset + 1],
        input[offset + 2],
        input[offset + 3],
        input[offset + 4],
        input[offset + 5],
        input[offset + 6],
        input[offset + 7],
    ])
}
