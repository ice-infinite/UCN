use ucn_identity::Principal;
use ucn_types::{Error, PROTOCOL_MAJOR, Result};

/// 固定 HELLO 长度。
pub const HELLO_BYTES: usize = 40;
/// 固定 Cookie Challenge 长度，与 HELLO 相同以禁止放大。
pub const COOKIE_CHALLENGE_BYTES: usize = 40;
/// `HELLO_COOKIE` 固定前缀长度。
pub const HELLO_COOKIE_FIXED_BYTES: usize = 82;
/// 最大 Cookie 字节数。
pub const COOKIE_MAX_BYTES: usize = 16;
/// 单事件最大验证 Evidence。
pub const MAX_EVIDENCE_BYTES: usize = 128;
/// 完整 canonical Bootstrap transcript 长度。
pub const TRANSCRIPT_BYTES: usize = 379;
/// Transcript、1 B Evidence 长度和最大 Evidence 组成的逻辑事件上限。
pub const LOGICAL_MAX_BYTES: usize = TRANSCRIPT_BYTES + 1 + MAX_EVIDENCE_BYTES;

/// Bootstrap Authority proof Opcode。
pub const OPCODE_IDENTITY_CHALLENGE: u16 = 131;
/// Bootstrap Device proof Opcode。
pub const OPCODE_IDENTITY_RESPONSE: u16 = 132;
/// Bootstrap Address offer Opcode。
pub const OPCODE_ADDRESS_OFFER: u16 = 133;
/// Bootstrap Device commit Opcode。
pub const OPCODE_DEVICE_COMMIT: u16 = 134;
/// Bootstrap Final durable Opcode。
pub const OPCODE_FINAL_COMMIT: u16 = 135;
/// Bootstrap Abort Opcode。
pub const OPCODE_ABORT: u16 = 136;

/// Bootstrap 流；Rust Dynamic Admission 只接受 JOIN。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BootstrapFlow {
    /// 未绑定设备首次准入。
    Join = 1,
    /// 已绑定设备重新认证；保留 Wire Registry，但由 Security Owner 实现。
    Reauth = 2,
}

impl TryFrom<u8> for BootstrapFlow {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            1 => Ok(Self::Join),
            2 => Ok(Self::Reauth),
            _ => Err(Error::Malformed),
        }
    }
}

/// Bootstrap 严格状态阶段。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum BootstrapPhase {
    /// Cookie 已验证并占用 pending。
    CookieVerified = 1,
    /// Authority 身份已验证。
    AuthorityVerified = 2,
    /// Device 身份已验证。
    DeviceVerified = 3,
    /// 地址和安全 Profile 已由 Authority 提议。
    AddressOffered = 4,
    /// Device 已接受完整 offer。
    DeviceCommitted = 5,
    /// Identity reload proof 已形成，最终准入。
    FinalDurable = 6,
    /// 终止。
    Aborted = 7,
}

impl TryFrom<u8> for BootstrapPhase {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            1 => Ok(Self::CookieVerified),
            2 => Ok(Self::AuthorityVerified),
            3 => Ok(Self::DeviceVerified),
            4 => Ok(Self::AddressOffered),
            5 => Ok(Self::DeviceCommitted),
            6 => Ok(Self::FinalDurable),
            7 => Ok(Self::Aborted),
            _ => Err(Error::Malformed),
        }
    }
}

/// 一个状态机事件。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BootstrapEvent {
    /// Cookie 验证。
    Cookie = 1,
    /// Authority proof。
    AuthorityProof = 2,
    /// Device proof。
    DeviceProof = 3,
    /// Address offer。
    AddressOffer = 4,
    /// Device commit。
    DeviceCommit = 5,
    /// Final durable proof。
    FinalDurable = 6,
    /// Abort。
    Abort = 7,
}

impl BootstrapEvent {
    /// 返回 Cookie 之后事件的冻结 Protocol Opcode。
    ///
    /// # Errors
    ///
    /// Cookie 是认证前单帧而不是分片逻辑事件，因此返回状态错误。
    pub const fn opcode(self) -> Result<u16> {
        match self {
            Self::AuthorityProof => Ok(OPCODE_IDENTITY_CHALLENGE),
            Self::DeviceProof => Ok(OPCODE_IDENTITY_RESPONSE),
            Self::AddressOffer => Ok(OPCODE_ADDRESS_OFFER),
            Self::DeviceCommit => Ok(OPCODE_DEVICE_COMMIT),
            Self::FinalDurable => Ok(OPCODE_FINAL_COMMIT),
            Self::Abort => Ok(OPCODE_ABORT),
            Self::Cookie => Err(Error::State),
        }
    }

    /// 从冻结 Protocol Opcode 恢复 Cookie 之后事件。
    ///
    /// # Errors
    ///
    /// Opcode 未登记为 Bootstrap 逻辑事件时返回格式错误。
    pub const fn from_opcode(opcode: u16) -> Result<Self> {
        match opcode {
            OPCODE_IDENTITY_CHALLENGE => Ok(Self::AuthorityProof),
            OPCODE_IDENTITY_RESPONSE => Ok(Self::DeviceProof),
            OPCODE_ADDRESS_OFFER => Ok(Self::AddressOffer),
            OPCODE_DEVICE_COMMIT => Ok(Self::DeviceCommit),
            OPCODE_FINAL_COMMIT => Ok(Self::FinalDurable),
            OPCODE_ABORT => Ok(Self::Abort),
            _ => Err(Error::Malformed),
        }
    }
}

/// 认证前固定 HELLO。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Hello {
    /// 流类别；Rust Owner 仅接纳 JOIN。
    pub flow: BootstrapFlow,
    /// 设备稳定身份摘要。
    pub identity_digest: Principal,
    /// 设备非零 nonce。
    pub device_nonce: u64,
    /// 设备非零事务 ID。
    pub transaction_id: u64,
}

/// 无状态 Cookie Challenge。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CookieChallenge {
    /// 流类别。
    pub flow: BootstrapFlow,
    /// 精确事务 ID。
    pub transaction_id: u64,
    /// Cookie 时间桶。
    pub cookie_time_bucket: u32,
    /// 有效 Cookie 字节数。
    pub cookie_length: u8,
    /// 固定容量 Cookie；尾部必须为零。
    pub cookie: [u8; COOKIE_MAX_BYTES],
}

/// 有界证明内容。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Evidence {
    bytes: [u8; MAX_EVIDENCE_BYTES],
    length: u8,
}

impl Evidence {
    /// 从非空、最多 128 B 的证明构造。
    ///
    /// # Errors
    ///
    /// 输入为空或超过固定容量时返回参数错误。
    pub fn new(input: &[u8]) -> Result<Self> {
        if input.is_empty() || input.len() > MAX_EVIDENCE_BYTES {
            return Err(Error::Argument);
        }
        let mut bytes = [0; MAX_EVIDENCE_BYTES];
        bytes[..input.len()].copy_from_slice(input);
        Ok(Self {
            bytes,
            length: u8::try_from(input.len()).map_err(|_| Error::NoSpace)?,
        })
    }

    /// 借用有效证明字节。
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes[..usize::from(self.length)]
    }
}

/// 携回 Cookie 的固定前缀 JOIN 请求。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HelloCookie {
    /// 流类别。
    pub flow: BootstrapFlow,
    /// 设备身份摘要。
    pub identity_digest: Principal,
    /// 设备 nonce。
    pub device_nonce: u64,
    /// 事务 ID。
    pub transaction_id: u64,
    /// 后续 Binding 新鲜度 challenge nonce。
    pub lease_freshness_challenge_nonce: u64,
    /// 入站 Link ID。
    pub selected_link_instance_id: u16,
    /// 入站 Link Generation。
    pub selected_link_instance_generation: u32,
    /// HELLO 与 Challenge 的 canonical hash。
    pub prior_messages_hash: [u8; 32],
    /// Cookie proof。
    pub cookie_evidence: Evidence,
}

/// 379 B canonical JOIN transcript。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BootstrapTranscript {
    /// 协议主版本。
    pub protocol_version: u8,
    /// Bootstrap Header Contract Registry 值。
    pub bootstrap_header_contract: u16,
    /// Dynamic Admission 固定 JOIN。
    pub flow: BootstrapFlow,
    /// 完整设备 Principal。
    pub joining_device_principal: [u8; 16],
    /// HELLO 身份摘要。
    pub joining_device_identity_digest: Principal,
    /// Authority Principal。
    pub authority_principal: [u8; 16],
    /// Authority Generation。
    pub authority_generation: u32,
    /// 设备 nonce。
    pub device_nonce: u64,
    /// Authority nonce。
    pub authority_nonce: u64,
    /// 事务 ID。
    pub transaction_id: u64,
    /// Binding lease freshness challenge。
    pub lease_freshness_challenge_nonce: u64,
    /// Realm ID。
    pub realm_id: u32,
    /// 候选地址。
    pub proposed_address: u32,
    /// 候选 Binding Generation。
    pub address_binding_generation: u32,
    /// Authority 地址。
    pub authority_address: u32,
    /// Authority Binding Generation。
    pub authority_binding_generation: u32,
    /// 精确 Link ID。
    pub selected_link_instance_id: u16,
    /// Binding Lease ID。
    pub binding_lease_id: [u8; 16],
    /// Binding 租期。
    pub binding_lease_duration_us: u64,
    /// Authority Lease Sequence。
    pub authority_lease_sequence: u64,
    /// Authority 租期。
    pub authority_lease_duration_us: u64,
    /// 新鲜度证明最大剩余租期。
    pub freshness_max_remaining_lease_us: u64,
    /// Authority durable fence。
    pub durable_fence_token: [u8; 16],
    /// 地址分配高水位摘要。
    pub allocation_high_water_digest: [u8; 16],
    /// Quorum config 摘要。
    pub quorum_config_digest: [u8; 32],
    /// Signer set 摘要。
    pub signer_set_digest: [u8; 32],
    /// Threshold proof 摘要。
    pub threshold_proof_digest: [u8; 32],
    /// Freshness proof transcript 摘要。
    pub freshness_proof_transcript_hash: [u8; 32],
    /// Signer 数量。
    pub authority_signer_count: u16,
    /// Quorum 门限。
    pub authority_quorum_threshold: u16,
    /// Binding 地址模式。
    pub binding_mode: u8,
    /// Hop Suite。
    pub selected_hop_suite: u8,
    /// Hop Key ID。
    pub selected_hop_key_id: u16,
    /// Hop Key Generation。
    pub selected_hop_key_generation: u32,
    /// E2E mode。
    pub selected_e2e_mode: u8,
    /// E2E Suite。
    pub selected_e2e_suite: u8,
    /// E2E Key ID。
    pub selected_e2e_key_id: u16,
    /// E2E Key Generation。
    pub selected_e2e_key_generation: u32,
    /// Session Generation。
    pub selected_session_generation: u32,
    /// Link Generation。
    pub selected_link_instance_generation: u32,
    /// 前序消息摘要。
    pub prior_messages_hash: [u8; 32],
}

/// 编码固定 HELLO；输出长度错误时不写回。
///
/// # Errors
///
/// HELLO 字段或精确输出长度不成立时返回错误。
pub fn encode_hello(hello: Hello, output: &mut [u8]) -> Result<()> {
    validate_hello(hello).map_err(|_| Error::Argument)?;
    if output.len() != HELLO_BYTES {
        return Err(Error::NoSpace);
    }
    let mut encoded = [0; HELLO_BYTES];
    encoded[0] = 1;
    encoded[1] = hello.flow as u8;
    encoded[4..20].copy_from_slice(&hello.identity_digest.bytes());
    encoded[20..28].copy_from_slice(&hello.device_nonce.to_be_bytes());
    encoded[28..36].copy_from_slice(&hello.transaction_id.to_be_bytes());
    output.copy_from_slice(&encoded);
    Ok(())
}

/// 解码固定 HELLO。
///
/// # Errors
///
/// 长度、版本、保留位或 typed 字段非法时返回格式错误。
pub fn decode_hello(input: &[u8]) -> Result<Hello> {
    if input.len() != HELLO_BYTES
        || input[0] != 1
        || input[2..4].iter().any(|byte| *byte != 0)
        || input[36..].iter().any(|byte| *byte != 0)
    {
        return Err(Error::Malformed);
    }
    let hello = Hello {
        flow: BootstrapFlow::try_from(input[1])?,
        identity_digest: Principal::new(input[4..20].try_into().map_err(|_| Error::Malformed)?)
            .map_err(|_| Error::Malformed)?,
        device_nonce: u64::from_be_bytes(input[20..28].try_into().map_err(|_| Error::Malformed)?),
        transaction_id: u64::from_be_bytes(input[28..36].try_into().map_err(|_| Error::Malformed)?),
    };
    validate_hello(hello).map_err(|_| Error::Malformed)?;
    Ok(hello)
}

/// 编码 40 B Cookie Challenge。
///
/// # Errors
///
/// Challenge 字段或精确输出长度不成立时返回错误。
pub fn encode_cookie_challenge(challenge: CookieChallenge, output: &mut [u8]) -> Result<()> {
    validate_cookie_challenge(challenge).map_err(|_| Error::Argument)?;
    if output.len() != COOKIE_CHALLENGE_BYTES {
        return Err(Error::NoSpace);
    }
    let mut encoded = [0; COOKIE_CHALLENGE_BYTES];
    encoded[0] = 1;
    encoded[1] = challenge.flow as u8;
    encoded[2] = challenge.cookie_length;
    encoded[4..12].copy_from_slice(&challenge.transaction_id.to_be_bytes());
    encoded[12..16].copy_from_slice(&challenge.cookie_time_bucket.to_be_bytes());
    encoded[16..32].copy_from_slice(&challenge.cookie);
    output.copy_from_slice(&encoded);
    Ok(())
}

/// 解码 40 B Cookie Challenge。
///
/// # Errors
///
/// 长度、保留位或 typed 字段非法时返回格式错误。
pub fn decode_cookie_challenge(input: &[u8]) -> Result<CookieChallenge> {
    if input.len() != COOKIE_CHALLENGE_BYTES
        || input[0] != 1
        || input[3] != 0
        || input[32..].iter().any(|byte| *byte != 0)
    {
        return Err(Error::Malformed);
    }
    let mut cookie = [0; COOKIE_MAX_BYTES];
    cookie.copy_from_slice(&input[16..32]);
    let challenge = CookieChallenge {
        flow: BootstrapFlow::try_from(input[1])?,
        transaction_id: u64::from_be_bytes(input[4..12].try_into().map_err(|_| Error::Malformed)?),
        cookie_time_bucket: u32::from_be_bytes(
            input[12..16].try_into().map_err(|_| Error::Malformed)?,
        ),
        cookie_length: input[2],
        cookie,
    };
    validate_cookie_challenge(challenge).map_err(|_| Error::Malformed)?;
    Ok(challenge)
}

/// 编码 `HELLO_COOKIE`；返回实际长度。
///
/// # Errors
///
/// typed 字段非法或输出容量不足时返回错误且不写输出。
pub fn encode_hello_cookie(value: HelloCookie, output: &mut [u8]) -> Result<usize> {
    validate_hello_cookie(value).map_err(|_| Error::Argument)?;
    let length = HELLO_COOKIE_FIXED_BYTES + value.cookie_evidence.bytes().len();
    if output.len() < length {
        return Err(Error::NoSpace);
    }
    let mut encoded = [0; HELLO_COOKIE_FIXED_BYTES + MAX_EVIDENCE_BYTES];
    encoded[0] = 1;
    encoded[1] = value.flow as u8;
    encoded[2] = value.cookie_evidence.length;
    encoded[4..20].copy_from_slice(&value.identity_digest.bytes());
    encoded[20..28].copy_from_slice(&value.device_nonce.to_be_bytes());
    encoded[28..36].copy_from_slice(&value.transaction_id.to_be_bytes());
    encoded[36..44].copy_from_slice(&value.lease_freshness_challenge_nonce.to_be_bytes());
    encoded[44..46].copy_from_slice(&value.selected_link_instance_id.to_be_bytes());
    encoded[46..50].copy_from_slice(&value.selected_link_instance_generation.to_be_bytes());
    encoded[50..82].copy_from_slice(&value.prior_messages_hash);
    encoded[82..length].copy_from_slice(value.cookie_evidence.bytes());
    output[..length].copy_from_slice(&encoded[..length]);
    Ok(length)
}

/// 解码 `HELLO_COOKIE`。
///
/// # Errors
///
/// 长度、保留位或 typed 字段非法时返回格式错误。
pub fn decode_hello_cookie(input: &[u8]) -> Result<HelloCookie> {
    if input.len() < HELLO_COOKIE_FIXED_BYTES + 1
        || input.len() > HELLO_COOKIE_FIXED_BYTES + MAX_EVIDENCE_BYTES
        || input[0] != 1
        || input[3] != 0
        || input.len() != HELLO_COOKIE_FIXED_BYTES + usize::from(input[2])
    {
        return Err(Error::Malformed);
    }
    let value = HelloCookie {
        flow: BootstrapFlow::try_from(input[1])?,
        identity_digest: Principal::new(input[4..20].try_into().map_err(|_| Error::Malformed)?)
            .map_err(|_| Error::Malformed)?,
        device_nonce: u64::from_be_bytes(input[20..28].try_into().map_err(|_| Error::Malformed)?),
        transaction_id: u64::from_be_bytes(input[28..36].try_into().map_err(|_| Error::Malformed)?),
        lease_freshness_challenge_nonce: u64::from_be_bytes(
            input[36..44].try_into().map_err(|_| Error::Malformed)?,
        ),
        selected_link_instance_id: u16::from_be_bytes(
            input[44..46].try_into().map_err(|_| Error::Malformed)?,
        ),
        selected_link_instance_generation: u32::from_be_bytes(
            input[46..50].try_into().map_err(|_| Error::Malformed)?,
        ),
        prior_messages_hash: input[50..82].try_into().map_err(|_| Error::Malformed)?,
        cookie_evidence: Evidence::new(&input[82..]).map_err(|_| Error::Malformed)?,
    };
    validate_hello_cookie(value).map_err(|_| Error::Malformed)?;
    Ok(value)
}

/// 按指定阶段编码完整 379 B transcript。
///
/// # Errors
///
/// Transcript 不是该阶段的 canonical 状态，或输出长度不精确时返回错误。
pub fn encode_transcript(
    value: &BootstrapTranscript,
    expected_phase: BootstrapPhase,
    output: &mut [u8],
) -> Result<()> {
    validate_transcript_phase(value, expected_phase).map_err(|_| Error::Argument)?;
    if output.len() != TRANSCRIPT_BYTES {
        return Err(Error::NoSpace);
    }
    let mut writer = Writer::new(output);
    writer.u8(value.protocol_version)?;
    writer.u16(value.bootstrap_header_contract)?;
    writer.u8(1)?;
    writer.u8(value.flow as u8)?;
    writer.bytes(&value.joining_device_principal)?;
    writer.bytes(&value.joining_device_identity_digest.bytes())?;
    writer.bytes(&value.authority_principal)?;
    writer.u32(value.authority_generation)?;
    writer.u64(value.device_nonce)?;
    writer.u64(value.authority_nonce)?;
    writer.u64(value.transaction_id)?;
    writer.u64(value.lease_freshness_challenge_nonce)?;
    writer.u32(value.realm_id)?;
    writer.u32(value.proposed_address)?;
    writer.u32(value.address_binding_generation)?;
    writer.u32(value.authority_address)?;
    writer.u32(value.authority_binding_generation)?;
    writer.u16(value.selected_link_instance_id)?;
    writer.bytes(&value.binding_lease_id)?;
    writer.u64(value.binding_lease_duration_us)?;
    writer.u64(value.authority_lease_sequence)?;
    writer.u64(value.authority_lease_duration_us)?;
    writer.u64(value.freshness_max_remaining_lease_us)?;
    writer.bytes(&value.durable_fence_token)?;
    writer.bytes(&value.allocation_high_water_digest)?;
    writer.bytes(&value.quorum_config_digest)?;
    writer.bytes(&value.signer_set_digest)?;
    writer.bytes(&value.threshold_proof_digest)?;
    writer.bytes(&value.freshness_proof_transcript_hash)?;
    writer.u16(value.authority_signer_count)?;
    writer.u16(value.authority_quorum_threshold)?;
    writer.u8(value.binding_mode)?;
    writer.u8(value.selected_hop_suite)?;
    writer.u16(value.selected_hop_key_id)?;
    writer.u32(value.selected_hop_key_generation)?;
    writer.u8(value.selected_e2e_mode)?;
    writer.u8(value.selected_e2e_suite)?;
    writer.u16(value.selected_e2e_key_id)?;
    writer.u32(value.selected_e2e_key_generation)?;
    writer.u32(value.selected_session_generation)?;
    writer.u32(value.selected_link_instance_generation)?;
    writer.bytes(&value.prior_messages_hash)?;
    if writer.offset != TRANSCRIPT_BYTES {
        return Err(Error::State);
    }
    Ok(())
}

/// 按指定阶段解码完整 379 B transcript。
///
/// # Errors
///
/// 长度、字段、保留位或阶段 canonical 状态不成立时返回格式错误。
pub fn decode_transcript(
    input: &[u8],
    expected_phase: BootstrapPhase,
) -> Result<BootstrapTranscript> {
    if input.len() != TRANSCRIPT_BYTES || input[3] != 1 {
        return Err(Error::Malformed);
    }
    let mut reader = Reader::new(input);
    let protocol_version = reader.u8()?;
    let bootstrap_header_contract = reader.u16()?;
    if reader.u8()? != 1 {
        return Err(Error::Malformed);
    }
    let value = BootstrapTranscript {
        protocol_version,
        bootstrap_header_contract,
        flow: BootstrapFlow::try_from(reader.u8()?)?,
        joining_device_principal: reader.array()?,
        joining_device_identity_digest: Principal::new(reader.array()?)
            .map_err(|_| Error::Malformed)?,
        authority_principal: reader.array()?,
        authority_generation: reader.u32()?,
        device_nonce: reader.u64()?,
        authority_nonce: reader.u64()?,
        transaction_id: reader.u64()?,
        lease_freshness_challenge_nonce: reader.u64()?,
        realm_id: reader.u32()?,
        proposed_address: reader.u32()?,
        address_binding_generation: reader.u32()?,
        authority_address: reader.u32()?,
        authority_binding_generation: reader.u32()?,
        selected_link_instance_id: reader.u16()?,
        binding_lease_id: reader.array()?,
        binding_lease_duration_us: reader.u64()?,
        authority_lease_sequence: reader.u64()?,
        authority_lease_duration_us: reader.u64()?,
        freshness_max_remaining_lease_us: reader.u64()?,
        durable_fence_token: reader.array()?,
        allocation_high_water_digest: reader.array()?,
        quorum_config_digest: reader.array()?,
        signer_set_digest: reader.array()?,
        threshold_proof_digest: reader.array()?,
        freshness_proof_transcript_hash: reader.array()?,
        authority_signer_count: reader.u16()?,
        authority_quorum_threshold: reader.u16()?,
        binding_mode: reader.u8()?,
        selected_hop_suite: reader.u8()?,
        selected_hop_key_id: reader.u16()?,
        selected_hop_key_generation: reader.u32()?,
        selected_e2e_mode: reader.u8()?,
        selected_e2e_suite: reader.u8()?,
        selected_e2e_key_id: reader.u16()?,
        selected_e2e_key_generation: reader.u32()?,
        selected_session_generation: reader.u32()?,
        selected_link_instance_generation: reader.u32()?,
        prior_messages_hash: reader.array()?,
    };
    if reader.offset != TRANSCRIPT_BYTES {
        return Err(Error::Malformed);
    }
    validate_transcript_phase(&value, expected_phase).map_err(|_| Error::Malformed)?;
    Ok(value)
}

/// 编码一个 canonical Bootstrap 逻辑事件：Transcript + Evidence length + Evidence。
///
/// # Errors
///
/// Event/阶段、Transcript、Evidence 或输出容量不成立时返回错误且不写输出。
pub fn encode_logical_event(
    event: BootstrapEvent,
    expected_phase: BootstrapPhase,
    transcript: &BootstrapTranscript,
    evidence: Evidence,
    output: &mut [u8],
) -> Result<usize> {
    if !event_matches_phase(event, transcript.flow, expected_phase) {
        return Err(Error::Argument);
    }
    let length = TRANSCRIPT_BYTES + 1 + evidence.bytes().len();
    if output.len() < length {
        return Err(Error::NoSpace);
    }
    let mut encoded = [0; LOGICAL_MAX_BYTES];
    encode_transcript(transcript, expected_phase, &mut encoded[..TRANSCRIPT_BYTES])?;
    encoded[TRANSCRIPT_BYTES] = evidence.length;
    encoded[TRANSCRIPT_BYTES + 1..length].copy_from_slice(evidence.bytes());
    output[..length].copy_from_slice(&encoded[..length]);
    Ok(length)
}

/// 解码一个与 Opcode/阶段精确绑定的 canonical Bootstrap 逻辑事件。
///
/// # Errors
///
/// Opcode、阶段、长度、Transcript 或 Evidence 非 canonical 时返回格式错误。
pub fn decode_logical_event(
    protocol_opcode: u16,
    expected_phase: BootstrapPhase,
    input: &[u8],
) -> Result<(BootstrapTranscript, Evidence)> {
    let event = BootstrapEvent::from_opcode(protocol_opcode)?;
    if input.len() < TRANSCRIPT_BYTES + 2 || input.len() > LOGICAL_MAX_BYTES {
        return Err(Error::Malformed);
    }
    let evidence_length = usize::from(input[TRANSCRIPT_BYTES]);
    if evidence_length == 0 || input.len() != TRANSCRIPT_BYTES + 1 + evidence_length {
        return Err(Error::Malformed);
    }
    let transcript = decode_transcript(&input[..TRANSCRIPT_BYTES], expected_phase)?;
    if !event_matches_phase(event, transcript.flow, expected_phase) {
        return Err(Error::Malformed);
    }
    let evidence = Evidence::new(&input[TRANSCRIPT_BYTES + 1..]).map_err(|_| Error::Malformed)?;
    Ok((transcript, evidence))
}

fn validate_hello(value: Hello) -> Result<()> {
    if value.device_nonce == 0 || value.transaction_id == 0 {
        return Err(Error::Argument);
    }
    Ok(())
}

fn validate_cookie_challenge(value: CookieChallenge) -> Result<()> {
    let length = usize::from(value.cookie_length);
    if value.transaction_id == 0
        || value.cookie_time_bucket == 0
        || length == 0
        || length > COOKIE_MAX_BYTES
        || value.cookie[..length].iter().all(|byte| *byte == 0)
        || value.cookie[length..].iter().any(|byte| *byte != 0)
    {
        return Err(Error::Argument);
    }
    Ok(())
}

fn validate_hello_cookie(value: HelloCookie) -> Result<()> {
    if value.device_nonce == 0
        || value.transaction_id == 0
        || value.lease_freshness_challenge_nonce == 0
        || value.selected_link_instance_id == 0
        || value.selected_link_instance_generation == 0
        || value.prior_messages_hash.iter().all(|byte| *byte == 0)
    {
        return Err(Error::Argument);
    }
    Ok(())
}

fn validate_transcript_base(value: &BootstrapTranscript) -> Result<()> {
    if value.protocol_version != PROTOCOL_MAJOR
        || value.bootstrap_header_contract != 1
        || value.flow != BootstrapFlow::Join
        || !valid_optional_principal(&value.joining_device_principal)
        || Principal::new(value.joining_device_identity_digest.bytes()).is_err()
        || !valid_optional_principal(&value.authority_principal)
        || value.device_nonce == 0
        || value.transaction_id == 0
        || value.lease_freshness_challenge_nonce == 0
        || value.selected_link_instance_id == 0
        || value.selected_link_instance_id == u16::MAX
        || value.selected_link_instance_generation == 0
        || value.prior_messages_hash.iter().all(|byte| *byte == 0)
    {
        return Err(Error::Argument);
    }
    Ok(())
}

pub(crate) fn validate_transcript_phase(
    value: &BootstrapTranscript,
    phase: BootstrapPhase,
) -> Result<()> {
    validate_transcript_base(value)?;
    let authority = authority_fields_valid(value);
    let device = device_fields_valid(value);
    let address = address_fields_valid(value);
    match phase {
        BootstrapPhase::CookieVerified => {
            if authority || device || address {
                return Err(Error::Malformed);
            }
        }
        BootstrapPhase::AuthorityVerified => {
            if !authority || device || address {
                return Err(Error::Malformed);
            }
        }
        BootstrapPhase::DeviceVerified => {
            if !authority || !device || address {
                return Err(Error::Malformed);
            }
        }
        BootstrapPhase::AddressOffered
        | BootstrapPhase::DeviceCommitted
        | BootstrapPhase::FinalDurable => {
            if !authority || !device || !address {
                return Err(Error::Malformed);
            }
        }
        BootstrapPhase::Aborted => return Err(Error::State),
    }
    Ok(())
}

pub(crate) fn event_matches_phase(
    event: BootstrapEvent,
    flow: BootstrapFlow,
    phase: BootstrapPhase,
) -> bool {
    match event {
        BootstrapEvent::Cookie => phase == BootstrapPhase::CookieVerified,
        BootstrapEvent::AuthorityProof => phase == BootstrapPhase::AuthorityVerified,
        BootstrapEvent::DeviceProof => phase == BootstrapPhase::DeviceVerified,
        BootstrapEvent::AddressOffer => {
            matches!(flow, BootstrapFlow::Join) && phase == BootstrapPhase::AddressOffered
        }
        BootstrapEvent::DeviceCommit => {
            matches!(flow, BootstrapFlow::Join) && phase == BootstrapPhase::DeviceCommitted
        }
        BootstrapEvent::FinalDurable => phase == BootstrapPhase::FinalDurable,
        BootstrapEvent::Abort => {
            (phase as u8) >= BootstrapPhase::CookieVerified as u8
                && (phase as u8) <= BootstrapPhase::DeviceCommitted as u8
        }
    }
}

pub(crate) fn transcript_transition_valid(
    previous: &BootstrapTranscript,
    next: &BootstrapTranscript,
    event: BootstrapEvent,
) -> bool {
    if !common_equal(previous, next) {
        return false;
    }
    match event {
        BootstrapEvent::AuthorityProof => {
            authority_fields_zero(previous)
                && authority_fields_valid(next)
                && device_fields_zero(previous)
                && device_fields_zero(next)
                && address_fields_zero(previous)
                && address_fields_zero(next)
        }
        BootstrapEvent::DeviceProof => {
            authority_equal(previous, next)
                && device_fields_zero(previous)
                && device_fields_valid(next)
                && address_fields_zero(previous)
                && address_fields_zero(next)
        }
        BootstrapEvent::AddressOffer => {
            authority_equal(previous, next)
                && device_equal(previous, next)
                && address_fields_zero(previous)
                && address_fields_valid(next)
        }
        BootstrapEvent::DeviceCommit | BootstrapEvent::FinalDurable => {
            semantic_equal(previous, next)
        }
        BootstrapEvent::Abort => true,
        BootstrapEvent::Cookie => false,
    }
}

fn valid_optional_principal(value: &[u8; 16]) -> bool {
    value.iter().all(|byte| *byte == 0) || Principal::new(*value).is_ok()
}

fn authority_fields_valid(value: &BootstrapTranscript) -> bool {
    Principal::new(value.authority_principal).is_ok()
        && value.authority_generation != 0
        && value.authority_nonce != 0
        && value.realm_id != 0
        && value.realm_id != u32::MAX
        && value.authority_address != 0
        && value.authority_address != u32::MAX
        && value.authority_binding_generation != 0
        && value.authority_lease_sequence != 0
        && value.authority_lease_duration_us != 0
        && value.freshness_max_remaining_lease_us != 0
        && value.freshness_max_remaining_lease_us <= value.authority_lease_duration_us
        && nonzero(&value.durable_fence_token)
        && nonzero(&value.allocation_high_water_digest)
        && nonzero(&value.quorum_config_digest)
        && nonzero(&value.signer_set_digest)
        && nonzero(&value.threshold_proof_digest)
        && nonzero(&value.freshness_proof_transcript_hash)
        && value.authority_signer_count != 0
        && value.authority_quorum_threshold != 0
        && value.authority_quorum_threshold <= value.authority_signer_count
}

fn device_fields_valid(value: &BootstrapTranscript) -> bool {
    Principal::new(value.joining_device_principal).is_ok()
        && value.selected_hop_suite != 0
        && value.selected_hop_key_id != 0
        && value.selected_hop_key_generation != 0
        && matches!(value.selected_e2e_mode, 1 | 2)
        && value.selected_e2e_suite != 0
        && value.selected_e2e_key_id != 0
        && value.selected_e2e_key_generation != 0
        && value.selected_session_generation != 0
}

fn address_fields_valid(value: &BootstrapTranscript) -> bool {
    value.proposed_address != 0
        && value.proposed_address != u32::MAX
        && value.address_binding_generation != 0
        && nonzero(&value.binding_lease_id)
        && value.binding_lease_duration_us != 0
        && matches!(value.binding_mode, 1..=3)
}

fn authority_fields_zero(value: &BootstrapTranscript) -> bool {
    value.authority_principal == [0; 16]
        && value.authority_generation == 0
        && value.authority_nonce == 0
        && value.realm_id == 0
        && value.authority_address == 0
        && value.authority_binding_generation == 0
        && value.authority_lease_sequence == 0
        && value.authority_lease_duration_us == 0
        && value.freshness_max_remaining_lease_us == 0
        && value.durable_fence_token == [0; 16]
        && value.allocation_high_water_digest == [0; 16]
        && value.quorum_config_digest == [0; 32]
        && value.signer_set_digest == [0; 32]
        && value.threshold_proof_digest == [0; 32]
        && value.freshness_proof_transcript_hash == [0; 32]
        && value.authority_signer_count == 0
        && value.authority_quorum_threshold == 0
}

fn device_fields_zero(value: &BootstrapTranscript) -> bool {
    value.joining_device_principal == [0; 16]
        && value.selected_hop_suite == 0
        && value.selected_hop_key_id == 0
        && value.selected_hop_key_generation == 0
        && value.selected_e2e_mode == 0
        && value.selected_e2e_suite == 0
        && value.selected_e2e_key_id == 0
        && value.selected_e2e_key_generation == 0
        && value.selected_session_generation == 0
}

fn address_fields_zero(value: &BootstrapTranscript) -> bool {
    value.proposed_address == 0
        && value.address_binding_generation == 0
        && value.binding_lease_id == [0; 16]
        && value.binding_lease_duration_us == 0
        && value.binding_mode == 0
}

fn common_equal(left: &BootstrapTranscript, right: &BootstrapTranscript) -> bool {
    left.protocol_version == right.protocol_version
        && left.bootstrap_header_contract == right.bootstrap_header_contract
        && left.flow == right.flow
        && left.joining_device_identity_digest == right.joining_device_identity_digest
        && left.device_nonce == right.device_nonce
        && left.transaction_id == right.transaction_id
        && left.lease_freshness_challenge_nonce == right.lease_freshness_challenge_nonce
        && left.selected_link_instance_id == right.selected_link_instance_id
        && left.selected_link_instance_generation == right.selected_link_instance_generation
        && left.prior_messages_hash == right.prior_messages_hash
        && nonzero(&right.prior_messages_hash)
}

fn authority_equal(left: &BootstrapTranscript, right: &BootstrapTranscript) -> bool {
    left.authority_principal == right.authority_principal
        && left.authority_generation == right.authority_generation
        && left.authority_nonce == right.authority_nonce
        && left.realm_id == right.realm_id
        && left.authority_address == right.authority_address
        && left.authority_binding_generation == right.authority_binding_generation
        && left.authority_lease_sequence == right.authority_lease_sequence
        && left.authority_lease_duration_us == right.authority_lease_duration_us
        && left.freshness_max_remaining_lease_us == right.freshness_max_remaining_lease_us
        && left.durable_fence_token == right.durable_fence_token
        && left.allocation_high_water_digest == right.allocation_high_water_digest
        && left.quorum_config_digest == right.quorum_config_digest
        && left.signer_set_digest == right.signer_set_digest
        && left.threshold_proof_digest == right.threshold_proof_digest
        && left.freshness_proof_transcript_hash == right.freshness_proof_transcript_hash
        && left.authority_signer_count == right.authority_signer_count
        && left.authority_quorum_threshold == right.authority_quorum_threshold
}

fn device_equal(left: &BootstrapTranscript, right: &BootstrapTranscript) -> bool {
    left.joining_device_principal == right.joining_device_principal
        && left.selected_hop_suite == right.selected_hop_suite
        && left.selected_hop_key_id == right.selected_hop_key_id
        && left.selected_hop_key_generation == right.selected_hop_key_generation
        && left.selected_e2e_mode == right.selected_e2e_mode
        && left.selected_e2e_suite == right.selected_e2e_suite
        && left.selected_e2e_key_id == right.selected_e2e_key_id
        && left.selected_e2e_key_generation == right.selected_e2e_key_generation
        && left.selected_session_generation == right.selected_session_generation
}

fn semantic_equal(left: &BootstrapTranscript, right: &BootstrapTranscript) -> bool {
    common_equal(left, right)
        && authority_equal(left, right)
        && device_equal(left, right)
        && left.proposed_address == right.proposed_address
        && left.address_binding_generation == right.address_binding_generation
        && left.binding_lease_id == right.binding_lease_id
        && left.binding_lease_duration_us == right.binding_lease_duration_us
        && left.binding_mode == right.binding_mode
}

fn nonzero<const N: usize>(value: &[u8; N]) -> bool {
    value.iter().any(|byte| *byte != 0)
}

struct Writer<'a> {
    output: &'a mut [u8],
    offset: usize,
}

impl<'a> Writer<'a> {
    fn new(output: &'a mut [u8]) -> Self {
        output.fill(0);
        Self { output, offset: 0 }
    }

    fn bytes(&mut self, value: &[u8]) -> Result<()> {
        let end = self
            .offset
            .checked_add(value.len())
            .ok_or(Error::Exhausted)?;
        let target = self
            .output
            .get_mut(self.offset..end)
            .ok_or(Error::NoSpace)?;
        target.copy_from_slice(value);
        self.offset = end;
        Ok(())
    }

    fn u8(&mut self, value: u8) -> Result<()> {
        self.bytes(&[value])
    }
    fn u16(&mut self, value: u16) -> Result<()> {
        self.bytes(&value.to_be_bytes())
    }
    fn u32(&mut self, value: u32) -> Result<()> {
        self.bytes(&value.to_be_bytes())
    }
    fn u64(&mut self, value: u64) -> Result<()> {
        self.bytes(&value.to_be_bytes())
    }
}

struct Reader<'a> {
    input: &'a [u8],
    offset: usize,
}

impl<'a> Reader<'a> {
    const fn new(input: &'a [u8]) -> Self {
        Self { input, offset: 0 }
    }

    fn bytes(&mut self, length: usize) -> Result<&'a [u8]> {
        let end = self.offset.checked_add(length).ok_or(Error::Exhausted)?;
        let value = self.input.get(self.offset..end).ok_or(Error::Malformed)?;
        self.offset = end;
        Ok(value)
    }

    fn array<const N: usize>(&mut self) -> Result<[u8; N]> {
        self.bytes(N)?.try_into().map_err(|_| Error::Malformed)
    }
    fn u8(&mut self) -> Result<u8> {
        Ok(self.bytes(1)?[0])
    }
    fn u16(&mut self) -> Result<u16> {
        Ok(u16::from_be_bytes(self.array()?))
    }
    fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_be_bytes(self.array()?))
    }
    fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_be_bytes(self.array()?))
    }
}
