use ucn_types::{Error, Result};

/// Capability Record 固定长度。
pub const CAPABILITY_RECORD_BYTES: usize = 68;
/// Capability Summary 固定长度。
pub const CAPABILITY_SUMMARY_BYTES: usize = 24;
/// Capability Query 固定长度。
pub const CAPABILITY_QUERY_BYTES: usize = 20;
/// Capability Digest 长度。
pub const CAPABILITY_DIGEST_BYTES: usize = 16;

/// Identity/Wire/Security/Capability 四项强制基础 Feature。
pub const REQUIRED_BASE_FEATURE_BITS: u32 = 0x0000_010B;
/// 当前已知 Feature 位。
pub const KNOWN_FEATURE_BITS: u32 = 0x0000_07FF;
/// 当前支持的 Hop Suite bit。
pub const KNOWN_HOP_SUITE_BITS: u32 = 0x0000_0002;
/// 当前支持的 E2E Suite bits。
pub const KNOWN_E2E_SUITE_BITS: u32 = 0x0000_000E;
const KNOWN_LINK_FLAGS: u16 = 0x001F;
const KNOWN_TIMESTAMP_BITS: u16 = 0x000F;
const KNOWN_REALTIME_BITS: u16 = 0x0007;
const MAX_FRAME_BYTES: u32 = 65_678;

/// 最大消息等级。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum MessageClass {
    /// 最大 32 B。
    T32 = 0,
    /// 最大 64 B。
    T64 = 1,
    /// 最大 128 B。
    T128 = 2,
    /// 最大 256 B。
    T256 = 3,
    /// 最大 512 B。
    T512 = 4,
    /// 最大 1 KiB。
    T1K = 5,
    /// 最大 2 KiB。
    T2K = 6,
    /// 最大 4 KiB。
    T4K = 7,
    /// 最大 8 KiB。
    T8K = 8,
}

impl TryFrom<u8> for MessageClass {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0 => Ok(Self::T32),
            1 => Ok(Self::T64),
            2 => Ok(Self::T128),
            3 => Ok(Self::T256),
            4 => Ok(Self::T512),
            5 => Ok(Self::T1K),
            6 => Ok(Self::T2K),
            7 => Ok(Self::T4K),
            8 => Ok(Self::T8K),
            _ => Err(Error::Malformed),
        }
    }
}

impl MessageClass {
    /// 返回该等级的最大业务字节数。
    #[must_use]
    pub const fn max_bytes(self) -> u32 {
        match self {
            Self::T32 => 32,
            Self::T64 => 64,
            Self::T128 => 128,
            Self::T256 => 256,
            Self::T512 => 512,
            Self::T1K => 1024,
            Self::T2K => 2048,
            Self::T4K => 4096,
            Self::T8K => 8192,
        }
    }
}

/// 单 Link 能力。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LinkCapability {
    /// Link Instance Generation。
    pub link_instance_generation: u32,
    /// Carrier 原始 MTU。
    pub carrier_mtu: u32,
    /// Link 可承载 UCN Frame MTU。
    pub link_frame_mtu: u32,
    /// 本机处理 Frame MTU。
    pub processing_frame_mtu: u32,
    /// Carrier 头开销。
    pub carrier_header_bytes: u16,
    /// Carrier padding 开销。
    pub carrier_padding_bytes: u16,
    /// Carrier CRC 开销。
    pub carrier_crc_bytes: u16,
    /// Carrier Tag 开销。
    pub carrier_tag_bytes: u16,
    /// Carrier 最大分片数。
    pub carrier_max_fragments: u16,
    /// Ordered/Reliable/Broadcast/Unicast/Security flags。
    pub link_flags: u16,
    /// 名义速率。
    pub nominal_rate_bps: u32,
    /// 硬件优先级数量。
    pub hardware_priority_count: u8,
    /// RX/TX 软件/硬件时间戳 bits。
    pub timestamp_capability_bits: u16,
    /// 时间戳误差上界。
    pub timestamp_uncertainty_us: u32,
}

/// Peer 协议能力。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PeerCapability {
    /// 协议 Feature bits。
    pub feature_bits: u32,
    /// Hop Suite bits。
    pub hop_suite_bits: u32,
    /// E2E Suite bits。
    pub e2e_suite_bits: u32,
    /// 最大消息等级。
    pub max_message_class: MessageClass,
    /// 最大 RX Window。
    pub max_rx_window: u16,
    /// 最大并发 Transfer。
    pub max_concurrent_transfers: u16,
    /// LOCAL/SYNCED/DEADLINE bits。
    pub realtime_mode_bits: u16,
    /// 当前 Clock Domain ID。
    pub clock_domain_id: u16,
    /// 当前 Clock Domain Generation。
    pub clock_domain_generation: u32,
}

/// 完整 Capability Record。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CapabilityRecord {
    /// 同一认证父域下严格递增的 Generation。
    pub capability_generation: u32,
    /// Link 能力。
    pub link: LinkCapability,
    /// Peer 协议能力。
    pub peer: PeerCapability,
}

impl CapabilityRecord {
    /// 校验完整记录。
    ///
    /// # Errors
    ///
    /// Generation、MTU、Feature、Suite、Realtime 或开销字段不成立时返回错误。
    pub fn validate(self) -> Result<()> {
        let carrier_overhead = u32::from(self.link.carrier_header_bytes)
            .checked_add(u32::from(self.link.carrier_padding_bytes))
            .and_then(|value| value.checked_add(u32::from(self.link.carrier_crc_bytes)))
            .and_then(|value| value.checked_add(u32::from(self.link.carrier_tag_bytes)))
            .ok_or(Error::Exhausted)?;
        if self.capability_generation == 0
            || self.link.link_instance_generation == 0
            || self.link.carrier_mtu == 0
            || self.link.link_frame_mtu == 0
            || self.link.processing_frame_mtu == 0
            || self.link.link_frame_mtu > MAX_FRAME_BYTES
            || self.link.processing_frame_mtu > MAX_FRAME_BYTES
            || self.link.carrier_max_fragments == 0
            || self.link.nominal_rate_bps == 0
            || self.link.hardware_priority_count == 0
            || self.link.link_flags & !KNOWN_LINK_FLAGS != 0
            || self.link.link_flags & 0x000C == 0
            || self.link.timestamp_capability_bits & !KNOWN_TIMESTAMP_BITS != 0
            || (self.link.timestamp_capability_bits == 0)
                != (self.link.timestamp_uncertainty_us == 0)
            || carrier_overhead >= self.link.carrier_mtu
            || self.peer.feature_bits & !KNOWN_FEATURE_BITS != 0
            || self.peer.feature_bits & REQUIRED_BASE_FEATURE_BITS != REQUIRED_BASE_FEATURE_BITS
            || self.peer.hop_suite_bits == 0
            || self.peer.hop_suite_bits & !KNOWN_HOP_SUITE_BITS != 0
            || self.peer.e2e_suite_bits == 0
            || self.peer.e2e_suite_bits & !KNOWN_E2E_SUITE_BITS != 0
            || self.peer.max_rx_window == 0
            || self.peer.max_concurrent_transfers == 0
            || self.peer.realtime_mode_bits & !KNOWN_REALTIME_BITS != 0
            || ((self.peer.feature_bits & (1 << 6) == 0) != (self.peer.realtime_mode_bits == 0))
            || (self.peer.realtime_mode_bits & 0x0006 != 0
                && (self.peer.clock_domain_id == 0 || self.peer.clock_domain_generation == 0))
            || (self.peer.realtime_mode_bits & 0x0006 == 0
                && (self.peer.clock_domain_id != 0 || self.peer.clock_domain_generation != 0))
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// Capability HELLO 摘要。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CapabilitySummary {
    /// Capability Generation。
    pub capability_generation: u32,
    /// Link Instance Generation。
    pub link_instance_generation: u32,
    /// 完整 Record 摘要。
    pub digest: [u8; CAPABILITY_DIGEST_BYTES],
}

/// Capability Query。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CapabilityQuery {
    /// 请求的 Generation；0 表示未知。
    pub requested_generation: u32,
    /// 已知摘要；Generation 0 时必须全零。
    pub known_digest: [u8; CAPABILITY_DIGEST_BYTES],
}

/// 编码 68 B Capability Record。
///
/// # Errors
///
/// Record 非法或输出长度不精确时返回错误，输出不写回。
pub fn encode_capability_record(record: CapabilityRecord, output: &mut [u8]) -> Result<()> {
    record.validate()?;
    if output.len() != CAPABILITY_RECORD_BYTES {
        return Err(Error::NoSpace);
    }
    let mut bytes = [0; CAPABILITY_RECORD_BYTES];
    put32(&mut bytes, 0, record.capability_generation);
    put32(&mut bytes, 4, record.link.link_instance_generation);
    put32(&mut bytes, 8, record.link.carrier_mtu);
    put32(&mut bytes, 12, record.link.link_frame_mtu);
    put32(&mut bytes, 16, record.link.processing_frame_mtu);
    put16(&mut bytes, 20, record.link.carrier_header_bytes);
    put16(&mut bytes, 22, record.link.carrier_padding_bytes);
    put16(&mut bytes, 24, record.link.carrier_crc_bytes);
    put16(&mut bytes, 26, record.link.carrier_tag_bytes);
    put16(&mut bytes, 28, record.link.carrier_max_fragments);
    put16(&mut bytes, 30, record.link.link_flags);
    put32(&mut bytes, 32, record.link.nominal_rate_bps);
    put32(&mut bytes, 36, record.link.timestamp_uncertainty_us);
    put32(&mut bytes, 40, record.peer.feature_bits);
    put32(&mut bytes, 44, record.peer.hop_suite_bits);
    put32(&mut bytes, 48, record.peer.e2e_suite_bits);
    put16(&mut bytes, 52, record.peer.max_rx_window);
    put16(&mut bytes, 54, record.peer.max_concurrent_transfers);
    bytes[56] = record.peer.max_message_class as u8;
    bytes[57] = record.link.hardware_priority_count;
    put16(&mut bytes, 58, record.link.timestamp_capability_bits);
    put16(&mut bytes, 60, record.peer.realtime_mode_bits);
    put16(&mut bytes, 62, record.peer.clock_domain_id);
    put32(&mut bytes, 64, record.peer.clock_domain_generation);
    output.copy_from_slice(&bytes);
    Ok(())
}

/// 解码 68 B Capability Record。
///
/// # Errors
///
/// 长度、枚举、保留位或字段关系非法时返回 malformed。
pub fn decode_capability_record(input: &[u8]) -> Result<CapabilityRecord> {
    if input.len() != CAPABILITY_RECORD_BYTES {
        return Err(Error::Malformed);
    }
    let record = CapabilityRecord {
        capability_generation: get32(input, 0)?,
        link: LinkCapability {
            link_instance_generation: get32(input, 4)?,
            carrier_mtu: get32(input, 8)?,
            link_frame_mtu: get32(input, 12)?,
            processing_frame_mtu: get32(input, 16)?,
            carrier_header_bytes: get16(input, 20)?,
            carrier_padding_bytes: get16(input, 22)?,
            carrier_crc_bytes: get16(input, 24)?,
            carrier_tag_bytes: get16(input, 26)?,
            carrier_max_fragments: get16(input, 28)?,
            link_flags: get16(input, 30)?,
            nominal_rate_bps: get32(input, 32)?,
            hardware_priority_count: input[57],
            timestamp_capability_bits: get16(input, 58)?,
            timestamp_uncertainty_us: get32(input, 36)?,
        },
        peer: PeerCapability {
            feature_bits: get32(input, 40)?,
            hop_suite_bits: get32(input, 44)?,
            e2e_suite_bits: get32(input, 48)?,
            max_message_class: MessageClass::try_from(input[56])?,
            max_rx_window: get16(input, 52)?,
            max_concurrent_transfers: get16(input, 54)?,
            realtime_mode_bits: get16(input, 60)?,
            clock_domain_id: get16(input, 62)?,
            clock_domain_generation: get32(input, 64)?,
        },
    };
    record.validate().map_err(|_| Error::Malformed)?;
    Ok(record)
}

/// 计算与 C 版本一致的四路 salted CRC32C 16 B 摘要。
///
/// # Errors
///
/// Record 非法或摘要计算失败时返回错误。
pub fn capability_digest(record: CapabilityRecord) -> Result<[u8; CAPABILITY_DIGEST_BYTES]> {
    let mut encoded = [0; CAPABILITY_RECORD_BYTES];
    encode_capability_record(record, &mut encoded)?;
    let mut salted = [0; CAPABILITY_RECORD_BYTES + 1];
    salted[1..].copy_from_slice(&encoded);
    let mut digest = [0; CAPABILITY_DIGEST_BYTES];
    for index in 0..4 {
        salted[0] = 0xA5 + u8::try_from(index).map_err(|_| Error::State)? * 0x17;
        digest[index * 4..index * 4 + 4].copy_from_slice(&crc32c(&salted).to_be_bytes());
    }
    if digest.iter().all(|byte| *byte == 0) {
        return Err(Error::State);
    }
    Ok(digest)
}

/// 编码 24 B Summary。
///
/// # Errors
///
/// Generation、Digest 或输出长度非法时返回错误。
pub fn encode_capability_summary(value: CapabilitySummary, output: &mut [u8]) -> Result<()> {
    validate_summary(value)?;
    if output.len() != CAPABILITY_SUMMARY_BYTES {
        return Err(Error::NoSpace);
    }
    let mut bytes = [0; CAPABILITY_SUMMARY_BYTES];
    put32(&mut bytes, 0, value.capability_generation);
    put32(&mut bytes, 4, value.link_instance_generation);
    bytes[8..].copy_from_slice(&value.digest);
    output.copy_from_slice(&bytes);
    Ok(())
}

/// 解码 24 B Summary。
///
/// # Errors
///
/// 长度、Generation 或 Digest 非法时返回 malformed。
pub fn decode_capability_summary(input: &[u8]) -> Result<CapabilitySummary> {
    if input.len() != CAPABILITY_SUMMARY_BYTES {
        return Err(Error::Malformed);
    }
    let value = CapabilitySummary {
        capability_generation: get32(input, 0)?,
        link_instance_generation: get32(input, 4)?,
        digest: input[8..].try_into().map_err(|_| Error::Malformed)?,
    };
    validate_summary(value).map_err(|_| Error::Malformed)?;
    Ok(value)
}

/// 编码 20 B Query。
///
/// # Errors
///
/// Generation/Digest 组合或输出长度非法时返回错误。
pub fn encode_capability_query(value: CapabilityQuery, output: &mut [u8]) -> Result<()> {
    validate_query(value)?;
    if output.len() != CAPABILITY_QUERY_BYTES {
        return Err(Error::NoSpace);
    }
    let mut bytes = [0; CAPABILITY_QUERY_BYTES];
    put32(&mut bytes, 0, value.requested_generation);
    bytes[4..].copy_from_slice(&value.known_digest);
    output.copy_from_slice(&bytes);
    Ok(())
}

/// 解码 20 B Query。
///
/// # Errors
///
/// 长度或 Generation/Digest 组合非法时返回 malformed。
pub fn decode_capability_query(input: &[u8]) -> Result<CapabilityQuery> {
    if input.len() != CAPABILITY_QUERY_BYTES {
        return Err(Error::Malformed);
    }
    let value = CapabilityQuery {
        requested_generation: get32(input, 0)?,
        known_digest: input[4..].try_into().map_err(|_| Error::Malformed)?,
    };
    validate_query(value).map_err(|_| Error::Malformed)?;
    Ok(value)
}

pub(crate) fn validate_summary(value: CapabilitySummary) -> Result<()> {
    if value.capability_generation == 0
        || value.link_instance_generation == 0
        || value.digest.iter().all(|byte| *byte == 0)
    {
        return Err(Error::Argument);
    }
    Ok(())
}

fn validate_query(value: CapabilityQuery) -> Result<()> {
    if (value.requested_generation == 0) != value.known_digest.iter().all(|byte| *byte == 0) {
        return Err(Error::Argument);
    }
    Ok(())
}

fn put16(output: &mut [u8], offset: usize, value: u16) {
    output[offset..offset + 2].copy_from_slice(&value.to_be_bytes());
}
fn put32(output: &mut [u8], offset: usize, value: u32) {
    output[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
}
fn get16(input: &[u8], offset: usize) -> Result<u16> {
    Ok(u16::from_be_bytes(
        input[offset..offset + 2]
            .try_into()
            .map_err(|_| Error::Malformed)?,
    ))
}
fn get32(input: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_be_bytes(
        input[offset..offset + 4]
            .try_into()
            .map_err(|_| Error::Malformed)?,
    ))
}

fn crc32c(bytes: &[u8]) -> u32 {
    let mut crc = !0_u32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0_u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0x82F6_3B78 & mask);
        }
    }
    !crc
}
