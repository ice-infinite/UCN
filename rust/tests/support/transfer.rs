#![allow(dead_code, missing_docs)] // Shared Host-only fixture, not a public protocol API.
//! Actual Foundation -> Parent -> Security -> dispatch -> cached response -> Adapter.
//! 实际Foundation、Parent、Security、分派、缓存回复与Adapter链路，非伪造DTO。
use security::flash;
#[path = "security.rs"]
pub mod security;
use ucn_adapter::*;
use ucn_digest::{DigestWorkspace, canonical_digest};
use ucn_durable::DurableContract;
use ucn_persistence::{CodecWorkspace, DomainKey, DomainKind};
use ucn_runtime::*;
use ucn_security::*;
use ucn_transport::{
    ParentKind, ParentOwner, TransferOwner, TransferParent, TransferPermit, TransferSetup,
};
use ucn_types::*;
use ucn_wire::CommonHeader;

pub type Transfer = TransferOwner<1, 1, 64, 8, 1>;
pub type Queue = ControlCoordinator<1, 128>;
pub type Replies = ControlReplies<1, 128>;
pub const OPS: &[ProtocolOpcode] = &[
    ProtocolOpcode::TransferSetup,
    ProtocolOpcode::TransferSetupAck,
    ProtocolOpcode::TransferTerminalReceipt,
    ProtocolOpcode::TransferAbort,
    ProtocolOpcode::TransferSackCredit,
];
pub struct Counter(pub u64);
impl C0TransactionOwner for Counter {
    fn preview(&self) -> Result<C0TransactionId> {
        C0TransactionId::new(self.0)
    }
    fn burn(&mut self, id: C0TransactionId) -> Result<()> {
        if id.get() != self.0 {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}
pub struct SequenceCounter(pub u32);
impl OriginSequenceOwner for SequenceCounter {
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.0)
    }
    fn burn(&mut self, expected: OriginSequence) -> Result<()> {
        if expected.get() != self.0 {
            return Err(Error::State);
        }
        self.0 = self.0.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(())
    }
}
pub struct FragmentPacket {
    pub bytes: [u8; 128],
    pub length: usize,
    pub identity: [u8; 46],
    pub sequence: u32,
}
impl FragmentPacket {
    pub fn plan(&self) -> OpenPacketPlan<'_> {
        OpenPacketPlan {
            packet: &self.bytes[..self.length],
            prefix_bytes: 13,
            canonical_identity: &self.identity,
            origin_context: OriginContext::Sequenced {
                contract: HeaderContract::C1,
                fingerprint: Fingerprint::new([5; 16]).unwrap(),
                sequence: OriginSequence::new(self.sequence).unwrap(),
            },
            hop_profile: HopProfile::H0,
        }
    }
}
pub fn data_access(session: SessionHandle, direction: AccessDirection) -> AccessRequest {
    AccessRequest {
        session,
        context_fingerprint: Fingerprint::new([5; 16]).unwrap(),
        service: ServiceId::new(0x40).unwrap(),
        protocol_opcode: 0,
        direction,
    }
}
pub fn protect_fragment(
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    body: &[u8],
    sequence: u32,
) -> FragmentPacket {
    let mut prefix = [0; 13];
    CommonHeader {
        contract: HeaderContract::C1,
        origin_security: OriginSecurity::O1,
        delivery: DeliveryGuarantee::Reliable,
        interaction: InteractionRole::OneWay,
        payload_kind: PayloadKind::Transfer,
        traffic_class: TrafficClass::Q3,
        hop_limit: HopLimit::new(1).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..5].copy_from_slice(
        &u16::try_from(facts.local.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[5..7].copy_from_slice(
        &u16::try_from(facts.peer.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[7..9].copy_from_slice(&0x40u16.to_be_bytes());
    prefix[9..13].copy_from_slice(&sequence.to_be_bytes());
    let mut packet = FragmentPacket {
        bytes: [0; 128],
        length: 0,
        identity: [0; 46],
        sequence,
    };
    packet.identity[..16].copy_from_slice(&facts.local.principal);
    packet.identity[16..20].copy_from_slice(&facts.local.generation.get().to_be_bytes());
    packet.identity[20..36].copy_from_slice(&facts.peer.principal);
    packet.identity[36..40].copy_from_slice(&facts.peer.generation.get().to_be_bytes());
    packet.identity[40..46].copy_from_slice(&prefix[7..13]);
    packet.length = security
        .protect_packet(
            crypto,
            OriginCounterOwner::Sequence(&mut SequenceCounter(sequence)),
            session,
            facts,
            data_access(session, AccessDirection::Outbound),
            PacketPlan {
                prefix: &prefix,
                payload: body,
                canonical_identity: &packet.identity,
                origin_context: OriginContext::Sequenced {
                    contract: HeaderContract::C1,
                    fingerprint: Fingerprint::new([5; 16]).unwrap(),
                    sequence: OriginSequence::new(sequence).unwrap(),
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut SecurityPacketWorkspace::<128>::new(),
            &mut packet.bytes,
        )
        .unwrap();
    packet
}
/// Hostile key-holder crafts a different O1 body at a reused sequence. Not a
/// production send path and never used to bypass the receiving Security Owner.
/// 恶意持钥者构造重复序号异内容，只用于验证真实接收端失败关闭。
pub fn adversarial_conflicting_fragment(
    crypto: &mut security::Crypto,
    original: &FragmentPacket,
    body: &[u8],
) -> FragmentPacket {
    assert_eq!(body.len(), original.length - 13 - 16);
    let mut packet = FragmentPacket {
        bytes: original.bytes,
        length: original.length,
        identity: original.identity,
        sequence: original.sequence,
    };
    packet.bytes[13..13 + body.len()].copy_from_slice(body);
    let mut aad = Vec::from(b"UCN6-ORIGIN-V1".as_slice());
    aad.extend_from_slice(&54u32.to_be_bytes()); // 3 + 1 + canonical identity46 + length4.
    aad.extend_from_slice(&packet.bytes[..2]);
    aad.push(packet.bytes[2] & 0xC0);
    aad.push(1);
    aad.extend_from_slice(&packet.identity);
    aad.extend_from_slice(&u32::try_from(body.len()).unwrap().to_be_bytes());
    let mut tag = [0; 16];
    crypto.adversarial_o1_tag(&aad, body, &mut tag);
    packet.bytes[13 + body.len()..packet.length].copy_from_slice(&tag);
    packet
}
pub fn parent() -> TransferParent {
    let convert = |b: ucn_security::Binding| ucn_transport::Binding {
        principal: b.principal,
        address: b.address.get(),
        generation: b.generation.get(),
    };
    TransferParent {
        kind: ParentKind::C1,
        id: 0,
        generation: 1,
        realm: 1,
        source: convert(security::binding(1)),
        destination: convert(security::binding(2)),
        security: ucn_transport::SecurityDomain {
            session: 1,
            key: 1,
            policy: 1,
            origin: OriginSecurity::O1,
            fingerprint: [5; 16],
            public_trusted_o0: false,
        },
        feedback_flow: None,
    }
}
pub fn contract() -> DurableContract {
    DurableContract {
        runtime: 1,
        owner: 2,
        persistence_owner: 3,
        domain: DomainKey {
            kind: DomainKind::TransportHighWater,
            id: 4,
        },
        schema: 0x5401,
        version: 1,
        operation: 0x0701,
    }
}
pub fn published_parent(allocate: bool) -> (ParentOwner, Option<TransferPermit>) {
    let mut result = None;
    flash::exercise(contract(), |p, f| {
        let mut owner = ParentOwner::new(contract()).unwrap();
        owner
            .prepare_parent(
                flash::base(p, contract()),
                parent(),
                1,
                10000,
                1,
                &mut CodecWorkspace::new(),
            )
            .unwrap();
        let (h, proof) = flash::commit(p, f, &owner.requirement().unwrap());
        owner.bind(h).unwrap();
        owner.confirm(h, proof).unwrap();
        p.proof_retire(h).unwrap();
        let permit = if allocate {
            owner
                .allocate(
                    flash::base(p, contract()),
                    21,
                    10000,
                    2,
                    &mut CodecWorkspace::new(),
                )
                .unwrap();
            let (h, proof) = flash::commit(p, f, &owner.requirement().unwrap());
            owner.bind(h).unwrap();
            owner.confirm(h, proof).unwrap();
            p.proof_retire(h).unwrap();
            Some(owner.take_permit().unwrap())
        } else {
            None
        };
        result = Some((owner, permit));
    });
    result.unwrap()
}
pub fn setup(id: u32) -> TransferSetup {
    TransferSetup {
        kind: ParentKind::C1,
        parent_id: 0,
        parent_generation: 1,
        transfer_id: id,
        service: 0x40,
        total_bytes: 6,
        digest: canonical_digest(
            b"UCN6-TRANSFER-MESSAGE-V1",
            b"abcdef",
            &mut DigestWorkspace::new(),
        )
        .unwrap(),
        delivery: DeliveryGuarantee::Reliable,
        interaction: InteractionRole::OneWay,
        fragments: 2,
        fragment_bytes: 3,
        lifetime_ms: 1,
        operation_id: 0,
        operation_flags: 0,
        result_code: 0,
    }
}
pub struct Packet {
    pub bytes: [u8; 128],
    pub length: usize,
}
impl Packet {
    pub fn plan(&self) -> OpenPacketPlan<'_> {
        let source = u16::from_be_bytes(self.bytes[7..9].try_into().unwrap());
        let destination = u16::from_be_bytes(self.bytes[9..11].try_into().unwrap());
        OpenPacketPlan {
            packet: &self.bytes[..self.length],
            prefix_bytes: 29,
            canonical_identity: &self.bytes[3..29],
            origin_context: OriginContext::C0 {
                fingerprint: Fingerprint::new([5; 16]).unwrap(),
                transaction_id: C0TransactionId::new(u64::from_be_bytes(
                    self.bytes[19..27].try_into().unwrap(),
                ))
                .unwrap(),
                opcode: ProtocolOpcode::try_from(u16::from_be_bytes(
                    self.bytes[27..29].try_into().unwrap(),
                ))
                .unwrap(),
                direction: if source < destination {
                    SenderDirection::AscendingPrincipal
                } else {
                    SenderDirection::DescendingPrincipal
                },
            },
            hop_profile: HopProfile::H0,
        }
    }
}
pub fn access(
    session: SessionHandle,
    opcode: ProtocolOpcode,
    direction: AccessDirection,
) -> AccessRequest {
    AccessRequest {
        session,
        context_fingerprint: Fingerprint::new([5; 16]).unwrap(),
        service: ServiceId::new(0x40).unwrap(),
        protocol_opcode: u16::from(opcode),
        direction,
    }
}
#[allow(clippy::too_many_arguments)]
pub fn protect(
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    opcode: ProtocolOpcode,
    body: &[u8],
    tx: u64,
) -> Packet {
    let mut prefix = [0; 29];
    CommonHeader {
        contract: HeaderContract::C0,
        origin_security: OriginSecurity::O1,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: if matches!(
            opcode,
            ProtocolOpcode::TransferSetup
                | ProtocolOpcode::TransportParentPrepare
                | ProtocolOpcode::TransportParentCommit
                | ProtocolOpcode::OperationQuery
                | ProtocolOpcode::OperationCancel
                | ProtocolOpcode::OperationResume
                | ProtocolOpcode::TimeDelayRequest
        ) {
            InteractionRole::Request
        } else if matches!(
            opcode,
            ProtocolOpcode::TransferSetupAck
                | ProtocolOpcode::TransportParentAccept
                | ProtocolOpcode::OperationReceipt
                | ProtocolOpcode::OperationResult
                | ProtocolOpcode::TimeDelayResponse
        ) {
            InteractionRole::Result
        } else {
            InteractionRole::OneWay
        },
        payload_kind: PayloadKind::Control,
        traffic_class: TrafficClass::Q1,
        hop_limit: HopLimit::new(1).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..7].copy_from_slice(&1u32.to_be_bytes());
    prefix[7..9].copy_from_slice(
        &u16::try_from(facts.local.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[9..11].copy_from_slice(
        &u16::try_from(facts.peer.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[11..15].copy_from_slice(&facts.local.generation.get().to_be_bytes());
    prefix[15..19].copy_from_slice(&facts.peer.generation.get().to_be_bytes());
    prefix[19..27].copy_from_slice(&tx.to_be_bytes());
    prefix[27..29].copy_from_slice(&u16::from(opcode).to_be_bytes());
    let mut packet = Packet {
        bytes: [0; 128],
        length: 0,
    };
    packet.length = security
        .protect_packet(
            crypto,
            OriginCounterOwner::Transaction(&mut Counter(tx)),
            session,
            facts,
            access(session, opcode, AccessDirection::Outbound),
            PacketPlan {
                prefix: &prefix,
                payload: body,
                canonical_identity: &prefix[3..],
                origin_context: OriginContext::C0 {
                    fingerprint: Fingerprint::new([5; 16]).unwrap(),
                    transaction_id: C0TransactionId::new(tx).unwrap(),
                    opcode,
                    direction: if facts.local.principal < facts.peer.principal {
                        SenderDirection::AscendingPrincipal
                    } else {
                        SenderDirection::DescendingPrincipal
                    },
                },
                hop_profile: HopProfile::H0,
            },
            None,
            &mut SecurityPacketWorkspace::<128>::new(),
            &mut packet.bytes,
        )
        .unwrap();
    packet
}
#[allow(clippy::too_many_arguments)]
pub fn receive<const C: usize>(
    queue: &mut Queue,
    replies: &mut ControlReplies<C, 128>,
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    packet: &Packet,
) -> Result<ControlAdmission> {
    let OriginContext::C0 { opcode, .. } = packet.plan().origin_context else {
        unreachable!()
    };
    queue.receive_cached(
        security,
        crypto,
        session,
        facts,
        access(session, opcode, AccessDirection::Inbound),
        packet.plan(),
        &mut SecurityPacketWorkspace::<128>::new(),
        replies,
    )
}
pub fn command(a: ControlAdmission) -> ControlHandle {
    match a {
        ControlAdmission::Command(h) => h,
        _ => panic!("expected fresh command"),
    }
}
#[allow(clippy::too_many_arguments, dead_code)] // Shared real protection/submission test fixture.
pub fn emit<const C: usize>(
    r: &mut ControlReplies<C, 128>,
    h: ReplyHandle,
    security: &mut SecurityOwner<'_, 2, 2>,
    crypto: &mut security::Crypto,
    session: SessionHandle,
    facts: CurrentFacts,
    counter: &mut Counter,
    adapter: &mut AdapterOwner<'_, 1, 1, 1, 128>,
    driver: &mut Driver,
    link: LinkHandle,
) -> Packet {
    r.seal(
        h,
        security,
        crypto,
        counter,
        session,
        facts,
        link,
        &mut SecurityPacketWorkspace::<128>::new(),
    )
    .unwrap();
    r.submit(h, security, session, facts, adapter, driver, link)
        .unwrap();
    r.retire_attempt(h, adapter).unwrap();
    let mut packet = Packet {
        bytes: [0; 128],
        length: 0,
    };
    packet.length = r.copy_packet(h, &mut packet.bytes).unwrap();
    assert_eq!(
        &packet.bytes[..packet.length],
        driver.frames.last().unwrap()
    );
    packet
}
pub struct Driver {
    pub submit: DriverSubmit,
    pub cancel: DriverCancel,
    pub frames: Vec<Vec<u8>>,
    pub tokens: Vec<TxToken>,
    pub cancels: usize,
}
impl Driver {
    pub fn new(submit: DriverSubmit) -> Self {
        Self {
            submit,
            cancel: DriverCancel::NotCancelled,
            frames: Vec::new(),
            tokens: Vec::new(),
            cancels: 0,
        }
    }
}
impl TxDriver for Driver {
    fn submit(&mut self, _: LinkHandle, bytes: &[u8], token: TxToken) -> DriverSubmit {
        self.frames.push(bytes.to_vec());
        self.tokens.push(token);
        self.submit
    }
    fn cancel(&mut self, _: TxToken) -> DriverCancel {
        self.cancels += 1;
        self.cancel
    }
}
