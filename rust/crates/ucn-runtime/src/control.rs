//! Authenticated bounded Control admission, not a fabricated `authenticated=true` DTO.
//! 实际Security认证的有界Control接纳，不接受调用方伪造的认证布尔值。
use ucn_security::{
    AccessDirection, AccessRequest, CryptoProvider, CurrentFacts, OpenDisposition, OpenPacketPlan,
    PacketEvidence, SecurityOwner, SecurityPacketWorkspace, SessionHandle,
};
use ucn_types::{
    DeliveryGuarantee, Error, HeaderContract, InteractionRole, OriginSecurity, PayloadKind,
    ProtocolOpcode, Result, ServiceId,
};
use ucn_wire::CommonHeader;

/// Exact supported Control bodies; 未编译的功能不产生状态，也不消费Replay。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControlBody {
    /// Authenticated HELLO/Keepalive summary, not admission; 认证摘要，非入网许可。
    #[cfg(feature = "capability-control")]
    CapabilitySummary(ProtocolOpcode, ucn_capability::CapabilitySummary),
    /// Full authenticated capability record; 完整认证能力记录。
    #[cfg(feature = "capability-control")]
    CapabilityAdvertise(ucn_capability::CapabilityRecord),
    /// Exact source-oriented negotiation; 精确源视角协商。
    #[cfg(feature = "capability-control")]
    CapabilitySelect(ucn_capability::ProfileSelect),
    /// Exact selected profile and request transcript; 精确选择及请求事务摘要。
    #[cfg(feature = "capability-control")]
    CapabilityAck(ucn_capability::ProfileAck),
    /// Full-record query, never a revoke alias; 完整记录查询，非撤销别名。
    #[cfg(feature = "capability-control")]
    CapabilityQuery(ucn_capability::CapabilityQuery),
    /// Exact generation revocation, preserving high-water; 精确代际撤销，保留高水位。
    #[cfg(feature = "capability-control")]
    CapabilityRevoke(ucn_capability::CapabilityQuery),
    /// Canonical Parent publication, still requiring exact current dispatch.
    /// 规范父发布正文，仍须精确当前域分派。
    #[cfg(feature = "parent-control")]
    Parent(ProtocolOpcode, ucn_transport::ParentPublication),
    /// Single-frame delivery ACK; 单帧ACK。
    DeliveryAck(ucn_transport::DeliveryAck),
    /// Transfer Setup; 分片建立。
    #[cfg(feature = "transfer")]
    TransferSetup(ucn_transport::TransferSetup),
    /// Setup ACK; 建立确认。
    #[cfg(feature = "transfer")]
    TransferSetupAck(ucn_transport::SetupAck),
    /// Transfer Abort; 分片取消。
    #[cfg(feature = "transfer")]
    TransferAbort(ucn_transport::TransferAbort),
    /// Exact terminal receipt; 精确终态凭据。
    #[cfg(feature = "transfer")]
    TransferTerminal(ucn_transport::TransferTerminal),
    /// SACK and bounded credit; 选择确认和有界额度。
    #[cfg(feature = "transfer")]
    TransferSack(ucn_transport::Sack),
    /// Durable operation query; 耐久操作查询。
    #[cfg(feature = "service-control")]
    OperationQuery(ucn_service::OperationWireKey),
    /// Operation result receipt; 操作结果凭据。
    #[cfg(feature = "service-control")]
    OperationReceipt(ucn_service::OperationReceipt),
    /// Cancel request, not effect reversal proof; 取消请求，不证明副作用撤销。
    #[cfg(feature = "service-control")]
    OperationCancel(ucn_service::OperationCancel),
    /// Terminal-result ACK, not transport ACK; 终态结果ACK，非传输ACK。
    #[cfg(feature = "operation-control")]
    OperationResultAck(ucn_service::OperationReceipt),
    /// Reauthenticate and recover terminal result, never reexecute; 重认证恢复终态，绝不重执行。
    #[cfg(feature = "operation-control")]
    OperationResume(ucn_service::OperationWireKey),
    /// Exact terminal bytes under current authorization; 当前授权下的精确终态正文。
    #[cfg(feature = "operation-control")]
    OperationResult(ucn_service::OperationResult),
    /// First synchronization announcement; 首次同步声明。
    #[cfg(feature = "realtime-control")]
    TimeSync(ucn_realtime::TimeKey),
    /// Master T1 follow-up; Master T1后续报文。
    #[cfg(feature = "realtime-control")]
    TimeFollowUp(ucn_realtime::TimeStampMessage),
    /// Exact member Delay Request; 精确Member延迟请求。
    #[cfg(feature = "realtime-control")]
    TimeDelayRequest(ucn_realtime::TimeKey),
    /// Master T4 response; Master T4响应。
    #[cfg(feature = "realtime-control")]
    TimeDelayResponse(ucn_realtime::TimeStampMessage),
}

fn decode(op: ProtocolOpcode, bytes: &[u8]) -> Result<ControlBody> {
    Ok(match op {
        #[cfg(feature = "capability-control")]
        ProtocolOpcode::NeighborHello | ProtocolOpcode::NeighborKeepalive => {
            ControlBody::CapabilitySummary(op, ucn_capability::decode_capability_summary(bytes)?)
        }
        #[cfg(feature = "capability-control")]
        ProtocolOpcode::CapabilityAdvertise => {
            ControlBody::CapabilityAdvertise(ucn_capability::decode_capability_record(bytes)?)
        }
        #[cfg(feature = "capability-control")]
        ProtocolOpcode::CapabilitySelect => {
            ControlBody::CapabilitySelect(ucn_capability::decode_profile_select(bytes)?)
        }
        #[cfg(feature = "capability-control")]
        ProtocolOpcode::CapabilityAck => {
            ControlBody::CapabilityAck(ucn_capability::decode_profile_ack(bytes)?)
        }
        #[cfg(feature = "capability-control")]
        ProtocolOpcode::CapabilityQuery => {
            ControlBody::CapabilityQuery(ucn_capability::decode_capability_query(bytes)?)
        }
        #[cfg(feature = "capability-control")]
        ProtocolOpcode::CapabilityRevoke => {
            ControlBody::CapabilityRevoke(ucn_capability::decode_capability_query(bytes)?)
        }
        #[cfg(feature = "parent-control")]
        ProtocolOpcode::TransportParentPrepare
        | ProtocolOpcode::TransportParentAccept
        | ProtocolOpcode::TransportParentCommit
        | ProtocolOpcode::TransportParentAbort => {
            ControlBody::Parent(op, ucn_transport::ParentPublication::decode(op, bytes)?)
        }
        ProtocolOpcode::DeliveryAck => ControlBody::DeliveryAck(ucn_transport::decode_ack(bytes)?),
        #[cfg(feature = "transfer")]
        ProtocolOpcode::TransferSetup => {
            ControlBody::TransferSetup(ucn_transport::decode_setup(bytes)?)
        }
        #[cfg(feature = "transfer")]
        ProtocolOpcode::TransferSetupAck => {
            ControlBody::TransferSetupAck(ucn_transport::decode_setup_ack(bytes)?)
        }
        #[cfg(feature = "transfer")]
        ProtocolOpcode::TransferAbort => {
            ControlBody::TransferAbort(ucn_transport::decode_abort(bytes)?)
        }
        #[cfg(feature = "transfer")]
        ProtocolOpcode::TransferTerminalReceipt => {
            ControlBody::TransferTerminal(ucn_transport::decode_terminal(bytes)?)
        }
        #[cfg(feature = "transfer")]
        ProtocolOpcode::TransferSackCredit => {
            ControlBody::TransferSack(ucn_transport::decode_sack(
                // Flow SACK's Parent is implicit in the paired reverse Flow Header.
                // A C0 envelope cannot supply that missing identity.
                if bytes.len() == 20 {
                    ucn_transport::ParentKind::C1
                } else {
                    return Err(Error::Malformed);
                },
                bytes,
            )?)
        }
        #[cfg(feature = "service-control")]
        ProtocolOpcode::OperationQuery => {
            ControlBody::OperationQuery(ucn_service::decode_operation_key(bytes)?)
        }
        #[cfg(feature = "service-control")]
        ProtocolOpcode::OperationReceipt => {
            ControlBody::OperationReceipt(ucn_service::decode_operation_receipt(bytes)?)
        }
        #[cfg(feature = "service-control")]
        ProtocolOpcode::OperationCancel => {
            ControlBody::OperationCancel(ucn_service::decode_operation_cancel(bytes)?)
        }
        #[cfg(feature = "operation-control")]
        ProtocolOpcode::OperationResultAck => {
            ControlBody::OperationResultAck(ucn_service::decode_operation_ack(bytes)?)
        }
        #[cfg(feature = "operation-control")]
        ProtocolOpcode::OperationResume => {
            ControlBody::OperationResume(ucn_service::decode_operation_key(bytes)?)
        }
        #[cfg(feature = "operation-control")]
        ProtocolOpcode::OperationResult => {
            ControlBody::OperationResult(ucn_service::decode_operation_result(bytes)?)
        }
        #[cfg(feature = "realtime-control")]
        ProtocolOpcode::TimeSync => ControlBody::TimeSync(ucn_realtime::decode_time_key(bytes)?),
        #[cfg(feature = "realtime-control")]
        ProtocolOpcode::TimeFollowUp => {
            ControlBody::TimeFollowUp(ucn_realtime::decode_time_stamp(bytes)?)
        }
        #[cfg(feature = "realtime-control")]
        ProtocolOpcode::TimeDelayRequest => {
            ControlBody::TimeDelayRequest(ucn_realtime::decode_time_key(bytes)?)
        }
        #[cfg(feature = "realtime-control")]
        ProtocolOpcode::TimeDelayResponse => {
            ControlBody::TimeDelayResponse(ucn_realtime::decode_time_stamp(bytes)?)
        }
        _ => return Err(Error::Unsupported),
    })
}
fn role(op: ProtocolOpcode) -> InteractionRole {
    match op {
        ProtocolOpcode::TransferSetup
        | ProtocolOpcode::TransportParentPrepare
        | ProtocolOpcode::TransportParentCommit
        | ProtocolOpcode::OperationQuery
        | ProtocolOpcode::OperationCancel
        | ProtocolOpcode::OperationResume
        | ProtocolOpcode::TimeDelayRequest => InteractionRole::Request,
        ProtocolOpcode::CapabilitySelect | ProtocolOpcode::CapabilityQuery => {
            InteractionRole::Request
        }
        ProtocolOpcode::TransferSetupAck
        | ProtocolOpcode::TransportParentAccept
        | ProtocolOpcode::OperationReceipt
        | ProtocolOpcode::OperationResult
        | ProtocolOpcode::TimeDelayResponse
        | ProtocolOpcode::CapabilityAck => InteractionRole::Result,
        _ => InteractionRole::OneWay,
    }
}

#[cfg_attr(
    not(any(
        feature = "transfer",
        feature = "realtime-control",
        feature = "capability-control"
    )),
    allow(clippy::unnecessary_wraps)
)] // Time/Flow bodies have no explicit Service in other builds.
fn body_service(body: ControlBody) -> Option<u16> {
    match body {
        #[cfg(feature = "capability-control")]
        ControlBody::CapabilitySummary(_, _)
        | ControlBody::CapabilityAdvertise(_)
        | ControlBody::CapabilitySelect(_)
        | ControlBody::CapabilityAck(_)
        | ControlBody::CapabilityQuery(_)
        | ControlBody::CapabilityRevoke(_) => None,
        #[cfg(feature = "parent-control")]
        ControlBody::Parent(_, _) => None,
        ControlBody::DeliveryAck(v) => Some(v.service),
        #[cfg(feature = "transfer")]
        ControlBody::TransferSetup(v) => Some(v.service),
        #[cfg(feature = "transfer")]
        ControlBody::TransferSetupAck(v) => Some(v.key.service),
        #[cfg(feature = "transfer")]
        ControlBody::TransferAbort(v) => Some(v.key.service),
        #[cfg(feature = "transfer")]
        ControlBody::TransferTerminal(v) => Some(v.key.service),
        #[cfg(feature = "transfer")]
        ControlBody::TransferSack(v) => {
            if v.kind == ucn_transport::ParentKind::C1 {
                Some(v.original_service)
            } else {
                None
            }
        }
        #[cfg(feature = "service-control")]
        ControlBody::OperationQuery(v) => Some(v.service),
        #[cfg(feature = "service-control")]
        ControlBody::OperationReceipt(v) => Some(v.key.service),
        #[cfg(feature = "service-control")]
        ControlBody::OperationCancel(v) => Some(v.key.service),
        #[cfg(feature = "operation-control")]
        ControlBody::OperationResultAck(v) => Some(v.key.service),
        #[cfg(feature = "operation-control")]
        ControlBody::OperationResume(v) => Some(v.service),
        #[cfg(feature = "operation-control")]
        ControlBody::OperationResult(v) => Some(v.receipt.key.service),
        #[cfg(feature = "realtime-control")]
        ControlBody::TimeSync(_)
        | ControlBody::TimeFollowUp(_)
        | ControlBody::TimeDelayRequest(_)
        | ControlBody::TimeDelayResponse(_) => None,
    }
}

#[cfg(all(test, feature = "transfer"))]
mod tests {
    use super::{ControlBody, decode};
    use ucn_types::{Error, ProtocolOpcode};
    #[test]
    fn c0_never_guesses_the_implicit_flow_sack_parent() {
        let sack = ucn_transport::Sack {
            kind: ucn_transport::ParentKind::Flow,
            transfer_id: 1,
            parent_generation: 0,
            original_service: 0,
            base: 0,
            bitmap: 1,
            credit: 1,
        };
        let mut bytes = [0; 20];
        let n = ucn_transport::encode_sack(sack, &mut bytes).unwrap();
        assert_eq!(n, 14);
        assert_eq!(
            decode(ProtocolOpcode::TransferSackCredit, &bytes[..n]),
            Err(Error::Malformed)
        );
        let sack = ucn_transport::Sack {
            kind: ucn_transport::ParentKind::C1,
            parent_generation: 2,
            original_service: 0x40,
            ..sack
        };
        let n = ucn_transport::encode_sack(sack, &mut bytes).unwrap();
        assert_eq!(n, 20);
        assert_eq!(
            decode(ProtocolOpcode::TransferSackCredit, &bytes[..n]),
            Ok(ControlBody::TransferSack(sack))
        );
    }
}

#[cfg(all(test, feature = "service-control", not(feature = "operation-control")))]
mod recovery_off {
    #[test]
    fn recovery_opcodes_never_materialize_state_when_owner_is_disabled() {
        for op in [
            ucn_types::ProtocolOpcode::OperationResultAck,
            ucn_types::ProtocolOpcode::OperationResume,
            ucn_types::ProtocolOpcode::OperationResult,
        ] {
            assert_eq!(super::decode(op, &[]), Err(ucn_types::Error::Unsupported));
        }
        assert!(core::mem::size_of::<super::ControlBody>() < 128);
    }
}

/// Opaque exact queue receipt, never a public raw tuple; 不透明队列凭据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ControlHandle {
    owner: u32,
    slot: usize,
    generation: u32,
}
/// Immutable command published only after actual verify/ACL/Replay and queue reservation.
/// 仅实际认证、ACL、Replay以及队列预留后发布的不变命令。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ControlCommand {
    body: ControlBody,
    evidence: PacketEvidence,
    accepted_at: u64,
    service: ServiceId,
    header: CommonHeader,
    #[cfg(feature = "capability-control")]
    fragmented: bool,
}
impl ControlCommand {
    /// Exact ACL service, including control without a Body service; 精确ACL Service。
    #[must_use]
    pub const fn service(self) -> ServiceId {
        self.service
    }
    /// Verified immutable header fields and observed mutable `HopLimit`.
    /// `已验证的不变Header字段及观察到的可变HopLimit`。
    #[must_use]
    pub const fn header(self) -> CommonHeader {
        self.header
    }
    /// Strictly decoded body; 严格解码的正文。
    #[must_use]
    pub const fn body(self) -> ControlBody {
        self.body
    }
    /// Non-forgeable authentication binding; 不可伪造的认证绑定。
    /// For fragmented commands this identifies the FIRST fragment, not a whole-body tag.
    /// 分片命令仅标识首片；完整聚合由私有Coordinator验证，不能当整段Payload的Tag。
    #[must_use]
    pub const fn evidence(self) -> PacketEvidence {
        self.evidence
    }
    /// Whether the receipt identifies a two-fragment aggregate; 是否是双片聚合命令。
    #[cfg(feature = "capability-control")]
    #[must_use]
    pub const fn is_fragmented(self) -> bool {
        self.fragmented
    }
    /// Local admission instant, does not renew a business lease; 接纳时刻，不续租。
    #[must_use]
    pub const fn accepted_at(self) -> u64 {
        self.accepted_at
    }
}
/// Fresh command or cached duplicate, never a second business invocation.
/// 新命令或缓存重复，不产生第二次业务调用。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControlAdmission {
    /// New admitted obligation; 新接纳义务。
    Command(ControlHandle),
    /// Exact request is already pending; 精确请求已在等待。
    Pending(ControlHandle),
    /// Exact authenticated duplicate points to retained response; 精确认证重复指向缓存回复。
    CachedReply(crate::ReplyHandle),
}
/// Fixed Coordinator queue. Admission is not business completion or durable proof.
/// 固定Coordinator队列；接纳不等于业务完成或持久化证明。
pub struct ControlCoordinator<const SLOTS: usize, const BYTES: usize> {
    owner: u32,
    bytes: [u8; BYTES],
    commands: [Option<ControlCommand>; SLOTS],
    generations: [u32; SLOTS],
    cursor: usize,
    stopped: bool,
}
impl<const Q: usize, const B: usize> ControlCoordinator<Q, B> {
    /// Caller-owned bounded storage; 调用方固定存储。
    /// # Errors
    /// Zero owner/capacity or insufficient body scratch rejects.
    pub const fn new(owner: u32) -> Result<Self> {
        if owner == 0 || Q == 0 || Q > u16::MAX as usize || B < 64 {
            return Err(Error::Config);
        }
        Ok(Self {
            owner,
            bytes: [0; B],
            commands: [None; Q],
            generations: [0; Q],
            cursor: 0,
            stopped: false,
        })
    }
    /// Stop new admissions but retain accepted obligations; 停止新接纳，保留已接纳义务。
    pub fn stop(&mut self) {
        self.stopped = true;
    }
    #[cfg(any(
        feature = "journal-runtime",
        feature = "source-runtime",
        feature = "capability-control"
    ))]
    pub(crate) fn is_empty(&self) -> bool {
        self.commands.iter().all(Option::is_none)
    }
    // Coordinator-only aggregate admission: BOTH authenticated fragment receipts remain
    // in Assembly. The public PacketEvidence here identifies the first fragment, not
    // a fabricated whole-body authentication tag.
    // Coordinator专用聚合接纳：Assembly保留双片凭据；公开Evidence只标识首片。
    #[cfg(feature = "capability-control")]
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn admit_capability_parts<const C: usize, const F: usize>(
        &mut self,
        op: ProtocolOpcode,
        bytes: &[u8],
        evidence: PacketEvidence,
        header: CommonHeader,
        service: ServiceId,
        first_at: u64,
        now: u64,
        published: bool,
        replies: &mut crate::ControlReplies<C, F>,
    ) -> Result<ControlAdmission> {
        if self.stopped {
            return Err(Error::State);
        }
        if published {
            if let Some(slot) = self
                .commands
                .iter()
                .position(|c| c.is_some_and(|v| v.evidence == evidence))
            {
                return Ok(ControlAdmission::Pending(ControlHandle {
                    owner: self.owner,
                    slot,
                    generation: self.generations[slot],
                }));
            }
            return Ok(ControlAdmission::CachedReply(
                replies.duplicate(evidence, now)?,
            ));
        }
        if !matches!(
            op,
            ProtocolOpcode::CapabilityAdvertise | ProtocolOpcode::CapabilityAck
        ) {
            return Err(Error::Unsupported);
        }
        let slot = self
            .commands
            .iter()
            .position(Option::is_none)
            .ok_or(Error::NoSpace)?;
        let generation = self.generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let body = decode(op, bytes)?;
        self.commands[slot] = Some(ControlCommand {
            body,
            evidence,
            accepted_at: first_at,
            header,
            service,
            fragmented: true,
        });
        self.generations[slot] = generation;
        Ok(ControlAdmission::Command(ControlHandle {
            owner: self.owner,
            slot,
            generation,
        }))
    }
    /// Actual authenticated raw C0 -> strict typed body -> queue reserve -> Replay commit.
    /// 实际认证raw C0到强类型Body，预留队列后才提交Replay。
    /// # Errors
    /// Unsupported/malformed/unauthorized/full/duplicate paths publish nothing.
    #[allow(clippy::too_many_arguments)]
    pub fn receive<const S: usize, const R: usize, const W: usize>(
        &mut self,
        security: &mut SecurityOwner<'_, S, R>,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        scratch: &mut SecurityPacketWorkspace<W>,
    ) -> Result<ControlHandle> {
        if self.stopped {
            return Err(Error::State);
        }
        if self.commands.iter().all(Option::is_some) {
            return Err(Error::NoSpace);
        }
        match self.receive_inner::<S, R, W, 1, 96>(
            security, provider, session, facts, access, plan, scratch, None,
        )? {
            ControlAdmission::Command(h) => Ok(h),
            _ => Err(Error::Replay),
        }
    }
    /// Authenticate duplicate before consulting cached replies; never repeat dispatch.
    /// 先认证重复，再查缓存回复，不重复分派；队列满也可回复已有事务。
    /// # Errors
    /// Invalid ACL/tag, absent/expired receipt, or full fresh queue leaves commands unchanged.
    #[allow(clippy::too_many_arguments)]
    pub fn receive_cached<
        const S: usize,
        const R: usize,
        const W: usize,
        const C: usize,
        const F: usize,
    >(
        &mut self,
        security: &mut SecurityOwner<'_, S, R>,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        scratch: &mut SecurityPacketWorkspace<W>,
        replies: &mut crate::ControlReplies<C, F>,
    ) -> Result<ControlAdmission> {
        self.receive_inner(
            security,
            provider,
            session,
            facts,
            access,
            plan,
            scratch,
            Some(replies),
        )
    }
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    fn receive_inner<
        const S: usize,
        const R: usize,
        const W: usize,
        const C: usize,
        const F: usize,
    >(
        &mut self,
        security: &mut SecurityOwner<'_, S, R>,
        provider: &mut impl CryptoProvider,
        session: SessionHandle,
        facts: CurrentFacts,
        access: AccessRequest,
        plan: OpenPacketPlan<'_>,
        scratch: &mut SecurityPacketWorkspace<W>,
        replies: Option<&mut crate::ControlReplies<C, F>>,
    ) -> Result<ControlAdmission> {
        if self.stopped {
            return Err(Error::State);
        }
        let header = CommonHeader::decode(plan.packet)?;
        let op = ProtocolOpcode::try_from(access.protocol_opcode)?;
        #[cfg(feature = "capability-control")]
        if matches!(
            op,
            ProtocolOpcode::NeighborHello
                | ProtocolOpcode::NeighborKeepalive
                | ProtocolOpcode::CapabilityAdvertise
                | ProtocolOpcode::CapabilitySelect
                | ProtocolOpcode::CapabilityAck
                | ProtocolOpcode::CapabilityQuery
                | ProtocolOpcode::CapabilityRevoke
        ) && (header.traffic_class != ucn_types::TrafficClass::Q2 || header.hop_limit.get() != 1)
        {
            return Err(Error::Malformed);
        }
        if header.contract != HeaderContract::C0
            || header.payload_kind != PayloadKind::Control
            || header.origin_security == OriginSecurity::O0
            || header.delivery != DeliveryGuarantee::BestEffort
            || header.interaction != role(op)
            || access.direction != AccessDirection::Inbound
        {
            return Err(Error::Malformed);
        }
        let opened = security.open_packet(
            provider,
            session,
            facts,
            access,
            plan,
            scratch,
            &mut self.bytes,
        )?;
        if opened.disposition == OpenDisposition::AuthenticatedReplayCandidate {
            let evidence = opened.evidence();
            security.replay_abort(opened.replay)?;
            if let Some(replies) = replies {
                if let Some(slot) = self
                    .commands
                    .iter()
                    .position(|command| command.is_some_and(|command| command.evidence == evidence))
                {
                    return Ok(ControlAdmission::Pending(ControlHandle {
                        owner: self.owner,
                        slot,
                        generation: self.generations[slot],
                    }));
                }
                return Ok(ControlAdmission::CachedReply(
                    replies.duplicate(evidence, facts.now_us)?,
                ));
            }
            return Err(Error::Replay);
        }
        let Some(index) = self.commands.iter().position(Option::is_none) else {
            security.replay_abort(opened.replay)?;
            return Err(Error::NoSpace);
        };
        let Some(generation) = self.generations[index].checked_add(1) else {
            security.replay_abort(opened.replay)?;
            return Err(Error::Exhausted);
        };
        let body = match decode(op, &self.bytes[..opened.payload_bytes]) {
            Ok(body) => body,
            Err(e) => {
                security.replay_abort(opened.replay)?;
                return Err(e);
            }
        };
        let command = ControlCommand {
            // Body target cannot broaden the exact Service authenticated by Security.
            body,
            evidence: opened.evidence(),
            accepted_at: facts.now_us,
            service: access.service,
            header,
            #[cfg(feature = "capability-control")]
            fragmented: false,
        };
        if body_service(body).is_some_and(|service| service != access.service.get()) {
            security.replay_abort(opened.replay)?;
            return Err(Error::Access);
        }
        // No callback/provider/foreign Owner between Replay commit and fixed queue publish.
        security.replay_commit(opened.replay, facts)?;
        self.commands[index] = Some(command);
        self.generations[index] = generation;
        Ok(ControlAdmission::Command(ControlHandle {
            owner: self.owner,
            slot: index,
            generation,
        }))
    }
    fn index(&self, h: ControlHandle) -> Result<usize> {
        if h.owner != self.owner
            || h.slot >= Q
            || self.generations[h.slot] != h.generation
            || self.commands[h.slot].is_none()
        {
            return Err(Error::State);
        }
        Ok(h.slot)
    }
    /// Borrow exact accepted obligation until dispatch completes; 完成分派前借用义务。
    /// # Errors
    /// Stale/cross-owner receipt rejects.
    pub fn view(&self, h: ControlHandle) -> Result<ControlCommand> {
        self.commands[self.index(h)?].ok_or(Error::State)
    }
    /// Bounded fair peek, also available after stop; 有界公平查看，stop后可用。
    /// # Errors
    /// Empty queue reports `NotFound`.
    pub fn peek_next(&mut self) -> Result<ControlHandle> {
        for _ in 0..Q {
            let i = self.cursor;
            self.cursor = (i + 1) % Q;
            if self.commands[i].is_some() {
                return Ok(ControlHandle {
                    owner: self.owner,
                    slot: i,
                    generation: self.generations[i],
                });
            }
        }
        Err(Error::NotFound)
    }
    /// Acknowledge processed or definitively rejected command, never before a required proof.
    /// 仅处理完成或明确拒绝后确认，需耐久证明时不得提前确认。
    /// # Errors
    /// Stale acknowledgement cannot remove a reused slot.
    pub fn ack(&mut self, h: ControlHandle) -> Result<()> {
        let i = self.index(h)?;
        self.commands[i] = None;
        Ok(())
    }
}
