use ucn_types::{Error, Result};

/// RREQ 在 C0 Header 之后的冻结 Payload 长度。
pub const RREQ_PAYLOAD_BYTES: usize = 9;
/// RREP 在 C0 Header 之后的冻结 Payload 长度。
pub const RREP_PAYLOAD_BYTES: usize = 29;
/// RERR 显式携带完整因果域的冻结 Payload 长度。
pub const RERR_PAYLOAD_BYTES: usize = 99;

/// RREQ 的 9 B 基础发现 Payload；Origin、Target 和 Transaction ID 由 C0 Header 承载。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RreqPayload {
    /// 已累计的保守路径成本。
    pub accumulated_cost: u32,
    /// 已知路径上的最小业务 Payload Budget。
    pub minimum_payload_budget: u16,
    /// 基础发现所需能力位。
    pub required_capability_bits: u16,
    /// 冻结发现标志；当前只允许低两位。
    pub flags: u8,
}

/// RREP 的基础路径事实；它不携带 Label、Flow 或远端绝对租期。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RrepPayload {
    /// 目标设备 Principal。
    pub destination_principal: [u8; 16],
    /// 目标当前 Binding Generation。
    pub destination_binding_generation: u32,
    /// 从目标开始累计的 Hop Count。
    pub hop_count: u8,
    /// 从目标开始累计的路径成本。
    pub accumulated_cost: u32,
    /// 路径完整 Frame MTU。
    pub path_frame_mtu: u16,
    /// 路径能力交集位。
    pub capability_bits: u16,
}

/// RERR 的精确原因。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RerrReason {
    /// 精确 Link 已失效。
    LinkInvalid = 1,
    /// `SoftRoute` 本地租期已到期。
    RouteExpired = 2,
    /// 下一跳明确拒绝该路径。
    NextHopRejected = 3,
    /// MTU/能力变化使旧路径不可用。
    PathContractChanged = 4,
}

impl TryFrom<u8> for RerrReason {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            1 => Ok(Self::LinkInvalid),
            2 => Ok(Self::RouteExpired),
            3 => Ok(Self::NextHopRejected),
            4 => Ok(Self::PathContractChanged),
            _ => Err(Error::Malformed),
        }
    }
}

/// RERR 的完整显式因果 Payload。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RerrPayload {
    /// Realm。
    pub realm: u32,
    /// 原始业务 Origin Principal。
    pub origin_principal: [u8; 16],
    /// 原始业务 Origin Address。
    pub origin_address: u32,
    /// Origin Binding Generation。
    pub origin_binding_generation: u32,
    /// Origin Session Generation。
    pub origin_session_generation: u32,
    /// 目标 Principal。
    pub destination_principal: [u8; 16],
    /// 目标 Address。
    pub destination_address: u32,
    /// 目标 Binding Generation。
    pub destination_binding_generation: u32,
    /// 被报告的本地 Route Generation。
    pub route_generation: u32,
    /// 建立该 Route 的发现因果 ID。
    pub route_causal_id: u64,
    /// 报告者 Principal。
    pub reporter_principal: [u8; 16],
    /// 报告者 Address。
    pub reporter_address: u32,
    /// 报告者 Binding Generation。
    pub reporter_binding_generation: u32,
    /// 失败 Link ID。
    pub failed_link_id: u16,
    /// 失败 Link Instance Generation。
    pub failed_link_generation: u32,
    /// 原因。
    pub reason: RerrReason,
}

/// 编码冻结 RREQ Payload。
///
/// # Errors
///
/// 字段非法或输出不是精确长度时返回错误，输出保持不变。
pub fn encode_rreq_payload(value: RreqPayload, output: &mut [u8]) -> Result<()> {
    validate_rreq(value)?;
    if output.len() != RREQ_PAYLOAD_BYTES {
        return Err(Error::NoSpace);
    }
    let mut encoded = [0_u8; RREQ_PAYLOAD_BYTES];
    encoded[0..4].copy_from_slice(&value.accumulated_cost.to_be_bytes());
    encoded[4..6].copy_from_slice(&value.minimum_payload_budget.to_be_bytes());
    encoded[6..8].copy_from_slice(&value.required_capability_bits.to_be_bytes());
    encoded[8] = value.flags;
    output.copy_from_slice(&encoded);
    Ok(())
}

/// 解码冻结 RREQ Payload。
///
/// # Errors
///
/// 长度或字段非法时返回错误。
pub fn decode_rreq_payload(input: &[u8]) -> Result<RreqPayload> {
    if input.len() != RREQ_PAYLOAD_BYTES {
        return Err(Error::Malformed);
    }
    let value = RreqPayload {
        accumulated_cost: read_u32(input, 0),
        minimum_payload_budget: read_u16(input, 4),
        required_capability_bits: read_u16(input, 6),
        flags: input[8],
    };
    validate_rreq(value).map_err(|_| Error::Malformed)?;
    Ok(value)
}

/// 编码冻结 RREP Payload。
///
/// # Errors
///
/// 字段非法或输出不是精确长度时返回错误，输出保持不变。
pub fn encode_rrep_payload(value: RrepPayload, output: &mut [u8]) -> Result<()> {
    validate_rrep(value)?;
    if output.len() != RREP_PAYLOAD_BYTES {
        return Err(Error::NoSpace);
    }
    let mut encoded = [0_u8; RREP_PAYLOAD_BYTES];
    encoded[0..16].copy_from_slice(&value.destination_principal);
    encoded[16..20].copy_from_slice(&value.destination_binding_generation.to_be_bytes());
    encoded[20] = value.hop_count;
    encoded[21..25].copy_from_slice(&value.accumulated_cost.to_be_bytes());
    encoded[25..27].copy_from_slice(&value.path_frame_mtu.to_be_bytes());
    encoded[27..29].copy_from_slice(&value.capability_bits.to_be_bytes());
    output.copy_from_slice(&encoded);
    Ok(())
}

/// 解码冻结 RREP Payload。
///
/// # Errors
///
/// 长度或字段非法时返回错误。
pub fn decode_rrep_payload(input: &[u8]) -> Result<RrepPayload> {
    if input.len() != RREP_PAYLOAD_BYTES {
        return Err(Error::Malformed);
    }
    let mut destination_principal = [0_u8; 16];
    destination_principal.copy_from_slice(&input[0..16]);
    let value = RrepPayload {
        destination_principal,
        destination_binding_generation: read_u32(input, 16),
        hop_count: input[20],
        accumulated_cost: read_u32(input, 21),
        path_frame_mtu: read_u16(input, 25),
        capability_bits: read_u16(input, 27),
    };
    validate_rrep(value).map_err(|_| Error::Malformed)?;
    Ok(value)
}

/// 编码完整、显式 RERR Payload。
///
/// # Errors
///
/// 任一身份、代际、因果或 Link 字段非法时失败，输出保持不变。
pub fn encode_rerr_payload(value: RerrPayload, output: &mut [u8]) -> Result<()> {
    validate_rerr(value)?;
    if output.len() != RERR_PAYLOAD_BYTES {
        return Err(Error::NoSpace);
    }
    let mut encoded = [0_u8; RERR_PAYLOAD_BYTES];
    encoded[0..4].copy_from_slice(&value.realm.to_be_bytes());
    encoded[4..20].copy_from_slice(&value.origin_principal);
    encoded[20..24].copy_from_slice(&value.origin_address.to_be_bytes());
    encoded[24..28].copy_from_slice(&value.origin_binding_generation.to_be_bytes());
    encoded[28..32].copy_from_slice(&value.origin_session_generation.to_be_bytes());
    encoded[32..48].copy_from_slice(&value.destination_principal);
    encoded[48..52].copy_from_slice(&value.destination_address.to_be_bytes());
    encoded[52..56].copy_from_slice(&value.destination_binding_generation.to_be_bytes());
    encoded[56..60].copy_from_slice(&value.route_generation.to_be_bytes());
    encoded[60..68].copy_from_slice(&value.route_causal_id.to_be_bytes());
    encoded[68..84].copy_from_slice(&value.reporter_principal);
    encoded[84..88].copy_from_slice(&value.reporter_address.to_be_bytes());
    encoded[88..92].copy_from_slice(&value.reporter_binding_generation.to_be_bytes());
    encoded[92..94].copy_from_slice(&value.failed_link_id.to_be_bytes());
    encoded[94..98].copy_from_slice(&value.failed_link_generation.to_be_bytes());
    encoded[98] = value.reason as u8;
    output.copy_from_slice(&encoded);
    Ok(())
}

/// 解码完整 RERR Payload。
///
/// # Errors
///
/// 长度或字段非法时返回错误。
pub fn decode_rerr_payload(input: &[u8]) -> Result<RerrPayload> {
    if input.len() != RERR_PAYLOAD_BYTES {
        return Err(Error::Malformed);
    }
    let mut origin_principal = [0_u8; 16];
    origin_principal.copy_from_slice(&input[4..20]);
    let mut destination_principal = [0_u8; 16];
    destination_principal.copy_from_slice(&input[32..48]);
    let mut reporter_principal = [0_u8; 16];
    reporter_principal.copy_from_slice(&input[68..84]);
    let value = RerrPayload {
        realm: read_u32(input, 0),
        origin_principal,
        origin_address: read_u32(input, 20),
        origin_binding_generation: read_u32(input, 24),
        origin_session_generation: read_u32(input, 28),
        destination_principal,
        destination_address: read_u32(input, 48),
        destination_binding_generation: read_u32(input, 52),
        route_generation: read_u32(input, 56),
        route_causal_id: read_u64(input, 60),
        reporter_principal,
        reporter_address: read_u32(input, 84),
        reporter_binding_generation: read_u32(input, 88),
        failed_link_id: read_u16(input, 92),
        failed_link_generation: read_u32(input, 94),
        reason: RerrReason::try_from(input[98]).map_err(|_| Error::Malformed)?,
    };
    validate_rerr(value).map_err(|_| Error::Malformed)?;
    Ok(value)
}

fn validate_rreq(value: RreqPayload) -> Result<()> {
    if value.minimum_payload_budget == 0 || value.flags & !0x03 != 0 {
        return Err(Error::Argument);
    }
    Ok(())
}

fn validate_rrep(value: RrepPayload) -> Result<()> {
    if value.destination_principal == [0; 16]
        || value.destination_binding_generation == 0
        || value.path_frame_mtu == 0
    {
        return Err(Error::Argument);
    }
    Ok(())
}

fn validate_rerr(value: RerrPayload) -> Result<()> {
    if value.realm == 0
        || value.realm == u32::MAX
        || value.origin_principal == [0; 16]
        || value.origin_address == 0
        || value.origin_binding_generation == 0
        || value.origin_session_generation == 0
        || value.destination_principal == [0; 16]
        || value.destination_address == 0
        || value.destination_binding_generation == 0
        || value.route_generation == 0
        || value.route_causal_id == 0
        || value.reporter_principal == [0; 16]
        || value.reporter_address == 0
        || value.reporter_binding_generation == 0
        || value.failed_link_id == 0
        || value.failed_link_id == u16::MAX
        || value.failed_link_generation == 0
    {
        return Err(Error::Argument);
    }
    Ok(())
}

fn read_u16(input: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes([input[offset], input[offset + 1]])
}

fn read_u32(input: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes(input[offset..offset + 4].try_into().unwrap_or([0; 4]))
}

fn read_u64(input: &[u8], offset: usize) -> u64 {
    u64::from_be_bytes(input[offset..offset + 8].try_into().unwrap_or([0; 8]))
}
