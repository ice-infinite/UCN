//! Unauthenticated Bootstrap framing, never an admitted peer projection.
//! 未认证Bootstrap帧合同，绝不产生已准入Peer投影。
use ucn_admission::{
    BootstrapFlow, BootstrapTranscript, CookieChallenge, Hello, HelloCookie,
    decode_cookie_challenge, decode_hello, decode_hello_cookie, encode_cookie_challenge,
    encode_hello, encode_hello_cookie,
};
use ucn_digest::{DigestWorkspace, sha256};
use ucn_types::{
    AddressWidth, BindingGeneration, C0TransactionId, DeliveryGuarantee, Error, HeaderContract,
    HopLimit, HopProfile, InteractionRole, OriginSecurity, PayloadKind, ProtocolOpcode, RealmId,
    Result, TrafficClass,
};
use ucn_wire::{C0Address, C0Frame, CommonHeader, decode_c0_o0_h0, encode_c0_o0_h0};
/// Exactly three pre-authentication messages; 仅三类认证前消息。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BootstrapMessage {
    /// HELLO is not identity proof; HELLO不证明身份。
    Hello(Hello),
    /// Opaque Cookie challenge and issuer-local Link; Cookie 挑战及签发方本地 Link。
    Challenge(CookieChallenge),
    /// Echoed issuer scope; 回显签发方域。
    Echo(HelloCookie),
}
impl BootstrapMessage {
    const fn identity(self) -> (ProtocolOpcode, InteractionRole, u64) {
        match self {
            Self::Hello(v) => (
                ProtocolOpcode::BootstrapHello,
                InteractionRole::Request,
                v.transaction_id,
            ),
            Self::Challenge(v) => (
                ProtocolOpcode::BootstrapCookieChallenge,
                InteractionRole::Result,
                v.transaction_id,
            ),
            Self::Echo(v) => (
                ProtocolOpcode::BootstrapHelloCookie,
                InteractionRole::Request,
                v.transaction_id,
            ),
        }
    }
}
/// Encode exact O0/H0/Q0/one-hop framing, no legacy fallback; 精确受限帧，无旧版fallback。
/// # Errors
/// Invalid body, Realm, Link context, role or short output rejects before writes.
pub fn encode_bootstrap_frame(
    message: BootstrapMessage,
    realm: RealmId,
    width: AddressWidth,
    output: &mut [u8],
) -> Result<usize> {
    let mut body = [0; 98];
    let n = match message {
        BootstrapMessage::Hello(v) => {
            if v.flow != BootstrapFlow::Join {
                return Err(Error::Unsupported);
            }
            encode_hello(v, &mut body[..40])?;
            40
        }
        BootstrapMessage::Challenge(v) => {
            if v.flow != BootstrapFlow::Join {
                return Err(Error::Unsupported);
            }
            encode_cookie_challenge(v, &mut body[..40])?;
            40
        }
        BootstrapMessage::Echo(v) => {
            if v.flow != BootstrapFlow::Join {
                return Err(Error::Unsupported);
            }
            encode_hello_cookie(v, &mut body)?
        }
    };
    let (op, role, tx) = message.identity();
    encode_body(op, role, tx, realm, width, &body[..n], output)
}
fn encode_body(
    op: ProtocolOpcode,
    role: InteractionRole,
    tx: u64,
    realm: RealmId,
    width: AddressWidth,
    body: &[u8],
    output: &mut [u8],
) -> Result<usize> {
    encode_c0_o0_h0(
        &C0Frame {
            common: CommonHeader {
                contract: HeaderContract::C0,
                origin_security: OriginSecurity::O0,
                delivery: DeliveryGuarantee::BestEffort,
                interaction: role,
                payload_kind: PayloadKind::Control,
                traffic_class: TrafficClass::Q0,
                hop_limit: HopLimit::new(1)?,
            },
            realm_id: realm,
            source: C0Address::Unbound,
            destination: C0Address::LinkLocalAuthority,
            source_binding_generation: BindingGeneration::UNBOUND,
            destination_binding_generation: BindingGeneration::UNBOUND,
            transaction_id: C0TransactionId::new(tx)?,
            opcode: op,
            payload: body,
        },
        width,
        HopProfile::H0,
        output,
    )
}
/// Decode complete frame; its output remains unauthenticated. 完整帧解码，输出仍未认证。
/// # Errors
/// Wrong address/domain/profile/role/transaction/length/unknown opcode rejects.
pub fn decode_bootstrap_frame(
    packet: &[u8],
    realm: RealmId,
    width: AddressWidth,
) -> Result<BootstrapMessage> {
    let f = decode_body(packet, realm, width)?;
    let msg = match f.opcode {
        ProtocolOpcode::BootstrapHello => BootstrapMessage::Hello(decode_hello(f.payload)?),
        ProtocolOpcode::BootstrapCookieChallenge => {
            BootstrapMessage::Challenge(decode_cookie_challenge(f.payload)?)
        }
        ProtocolOpcode::BootstrapHelloCookie => {
            BootstrapMessage::Echo(decode_hello_cookie(f.payload)?)
        }
        _ => return Err(Error::Unsupported),
    };
    let (_, role, tx) = msg.identity();
    if f.common.interaction != role || f.transaction_id.get() != tx {
        return Err(Error::Malformed);
    }
    if match msg {
        BootstrapMessage::Hello(v) => v.flow,
        BootstrapMessage::Challenge(v) => v.flow,
        BootstrapMessage::Echo(v) => v.flow,
    } != BootstrapFlow::Join
    {
        return Err(Error::Unsupported);
    }
    Ok(msg)
}
fn decode_body(packet: &[u8], realm: RealmId, width: AddressWidth) -> Result<C0Frame<'_>> {
    let f = decode_c0_o0_h0(packet, width, HopProfile::H0)?;
    if f.realm_id != realm
        || f.source != C0Address::Unbound
        || f.destination != C0Address::LinkLocalAuthority
        || !f.source_binding_generation.is_unbound()
        || !f.destination_binding_generation.is_unbound()
        || f.common.delivery != DeliveryGuarantee::BestEffort
        || f.common.traffic_class != TrafficClass::Q0
        || f.common.payload_kind != PayloadKind::Control
        || f.common.hop_limit.get() != 1
    {
        return Err(Error::Malformed);
    }
    Ok(f)
}

/// Registered post-Cookie event frame; no E2E Session is assumed.
/// 已登记的Cookie后事件帧；不假定已有E2E Session。
/// # Errors
/// Wrong opcode/phase/chunk or short output rejects before writes.
pub fn encode_bootstrap_event_frame(
    event: ucn_admission::BootstrapEvent,
    fragment: ucn_admission::BootstrapFragment,
    realm: RealmId,
    width: AddressWidth,
    output: &mut [u8],
) -> Result<usize> {
    let mut body = [0; 164];
    let opcode = ProtocolOpcode::try_from(event.opcode()?)?;
    let role = event_role(event)?;
    // Logical decoder independently checks the event/phase pair.
    if event_phase(event)? != fragment.phase || fragment.flow != BootstrapFlow::Join {
        return Err(Error::Argument);
    }
    let n = ucn_admission::encode_fragment(fragment, &mut body)?;
    encode_body(
        opcode,
        role,
        fragment.transaction_id,
        realm,
        width,
        &body[..n],
        output,
    )
}
/// Structural fragment decode; authentication occurs only after full reassembly.
/// 分片结构解码；只有完整重组后才进行身份认证。
/// # Errors
/// Invalid framing/role/transaction/phase rejects.
pub fn decode_bootstrap_event_frame(
    packet: &[u8],
    realm: RealmId,
    width: AddressWidth,
) -> Result<(
    ucn_admission::BootstrapEvent,
    ucn_admission::BootstrapFragment,
)> {
    let frame = decode_body(packet, realm, width)?;
    let event = ucn_admission::BootstrapEvent::from_opcode(frame.opcode.into())?;
    let fragment = ucn_admission::decode_fragment(frame.payload)?;
    if fragment.flow != BootstrapFlow::Join
        || fragment.phase != event_phase(event)?
        || frame.common.interaction != event_role(event)?
        || frame.transaction_id.get() != fragment.transaction_id
    {
        return Err(Error::Malformed);
    }
    Ok((event, fragment))
}
pub(crate) fn event_phase(
    event: ucn_admission::BootstrapEvent,
) -> Result<ucn_admission::BootstrapPhase> {
    use ucn_admission::{BootstrapEvent as E, BootstrapPhase as P};
    match event {
        E::AuthorityProof => Ok(P::AuthorityVerified),
        E::DeviceProof => Ok(P::DeviceVerified),
        E::AddressOffer => Ok(P::AddressOffered),
        E::DeviceCommit => Ok(P::DeviceCommitted),
        E::FinalDurable => Ok(P::FinalDurable),
        _ => Err(Error::Unsupported),
    }
}
fn event_role(event: ucn_admission::BootstrapEvent) -> Result<InteractionRole> {
    use ucn_admission::BootstrapEvent as E;
    match event {
        E::AuthorityProof | E::AddressOffer => Ok(InteractionRole::Request),
        E::DeviceProof | E::DeviceCommit | E::FinalDurable => Ok(InteractionRole::Result),
        _ => Err(Error::Unsupported),
    }
}
/// Full prior-message hash, including Realm/address width and both exact outer frames.
/// 完整前序摘要，包含Realm、地址宽度编码及两份精确外层帧。
/// # Errors
/// A challenge for a different transaction or illegal body rejects.
pub fn bootstrap_prior_hash(
    hello: Hello,
    challenge: CookieChallenge,
    realm: RealmId,
    width: AddressWidth,
    work: &mut DigestWorkspace,
) -> Result<[u8; 32]> {
    const DOMAIN: &[u8] = b"UCN6-BOOTSTRAP-COOKIE-V1";
    if hello.transaction_id != challenge.transaction_id || hello.flow != challenge.flow {
        return Err(Error::Security);
    }
    let mut bytes = [0; 174];
    let start = DOMAIN.len() + 4;
    bytes[..DOMAIN.len()].copy_from_slice(DOMAIN);
    let a = encode_bootstrap_frame(
        BootstrapMessage::Hello(hello),
        realm,
        width,
        &mut bytes[start..start + 73],
    )?;
    let b = encode_bootstrap_frame(
        BootstrapMessage::Challenge(challenge),
        realm,
        width,
        &mut bytes[start + a..],
    )?;
    bytes[DOMAIN.len()..start].copy_from_slice(
        &u32::try_from(a + b)
            .map_err(|_| Error::NoSpace)?
            .to_be_bytes(),
    );
    Ok(sha256(&bytes[..start + a + b], work))
}
pub(crate) fn cookie_transcript(v: HelloCookie) -> BootstrapTranscript {
    BootstrapTranscript {
        protocol_version: 6,
        bootstrap_header_contract: 1,
        flow: BootstrapFlow::Join,
        joining_device_principal: [0; 16],
        joining_device_identity_digest: v.identity_digest,
        authority_principal: [0; 16],
        authority_generation: 0,
        device_nonce: v.device_nonce,
        authority_nonce: 0,
        transaction_id: v.transaction_id,
        lease_freshness_challenge_nonce: v.lease_freshness_challenge_nonce,
        realm_id: 0,
        proposed_address: 0,
        address_binding_generation: 0,
        authority_address: 0,
        authority_binding_generation: 0,
        selected_link_instance_id: v.selected_link_instance_id,
        binding_lease_id: [0; 16],
        binding_lease_duration_us: 0,
        authority_lease_sequence: 0,
        authority_lease_duration_us: 0,
        freshness_max_remaining_lease_us: 0,
        durable_fence_token: [0; 16],
        allocation_high_water_digest: [0; 16],
        quorum_config_digest: [0; 32],
        signer_set_digest: [0; 32],
        threshold_proof_digest: [0; 32],
        freshness_proof_transcript_hash: [0; 32],
        authority_signer_count: 0,
        authority_quorum_threshold: 0,
        binding_mode: 0,
        selected_hop_suite: 0,
        selected_hop_key_id: 0,
        selected_hop_key_generation: 0,
        selected_e2e_mode: 0,
        selected_e2e_suite: 0,
        selected_e2e_key_id: 0,
        selected_e2e_key_generation: 0,
        selected_session_generation: 0,
        selected_link_instance_generation: v.selected_link_instance_generation,
        prior_messages_hash: v.prior_messages_hash,
    }
}
