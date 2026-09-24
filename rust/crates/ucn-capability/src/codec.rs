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

const SHA256_K: [u32; 64] = [
    0x428A_2F98, 0x7137_4491, 0xB5C0_FBCF, 0xE9B5_DBA5,
    0x3956_C25B, 0x59F1_11F1, 0x923F_82A4, 0xAB1C_5ED5,
    0xD807_AA98, 0x1283_5B01, 0x2431_85BE, 0x550C_7DC3,
    0x72BE_5D74, 0x80DE_B1FE, 0x9BDC_06A7, 0xC19B_F174,
    0xE49B_69C1, 0xEFBE_4786, 0x0FC1_9DC6, 0x240C_A1CC,
    0x2DE9_2C6F, 0x4A74_84AA, 0x5CB0_A9DC, 0x76F9_88DA,
    0x983E_5152, 0xA831_C66D, 0xB003_27C8, 0xBF59_7FC7,
    0xC6E0_0BF3, 0xD5A7_9147, 0x06CA_6351, 0x1429_2967,
    0x27B7_0A85, 0x2E1B_2138, 0x4D2C_6DFC, 0x5338_0D13,
    0x650A_7354, 0x766A_0ABB, 0x81C2_C92E, 0x9272_2C85,
    0xA2BF_E8A1, 0xA81A_664B, 0xC24B_8B70, 0xC76C_51A3,
    0xD192_E819, 0xD699_0624, 0xF40E_3585, 0x106A_A070,
    0x19A4_C116, 0x1E37_6C08, 0x2748_774C, 0x34B0_BCB5,
    0x391C_0CB3, 0x4ED8_AA4A, 0x5B9C_CA4F, 0x682E_6FF3,
    0x748F_82EE, 0x78A5_636F, 0x84C8_7814, 0x8CC7_0208,
    0x90BE_FFFA, 0xA450_6CEB, 0xBEF9_A3F7, 0xC671_78F2,
];

fn sha256_compress(state: &mut [u32; 8], block: &[u8; 64]) {
    let mut words = [0_u32; 64];
    for (index, chunk) in block.chunks_exact(4).take(16).enumerate() {
        words[index] = u32::from_be_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
    }
    for index in 16..64 {
        let left = words[index - 15];
        let right = words[index - 2];
        let s0 = left.rotate_right(7) ^ left.rotate_right(18) ^ (left >> 3);
        let s1 = right.rotate_right(17) ^ right.rotate_right(19) ^ (right >> 10);
        words[index] = words[index - 16]
            .wrapping_add(s0)
            .wrapping_add(words[index - 7])
            .wrapping_add(s1);
    }
    let mut a = state[0];
    let mut b = state[1];
    let mut c = state[2];
    let mut d = state[3];
    let mut e = state[4];
    let mut f = state[5];
    let mut g = state[6];
    let mut h = state[7];
    for index in 0..64 {
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let choose = (e & f) ^ ((!e) & g);
        let temp1 = h
            .wrapping_add(s1)
            .wrapping_add(choose)
            .wrapping_add(SHA256_K[index])
            .wrapping_add(words[index]);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let majority = (a & b) ^ (a & c) ^ (b & c);
        let temp2 = s0.wrapping_add(majority);
        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(temp1);
        d = c;
        c = b;
        b = a;
        a = temp1.wrapping_add(temp2);
    }
    for (slot, value) in state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
        *slot = slot.wrapping_add(value);
    }
}

fn sha256_128(bytes: &[u8]) -> Result<[u8; CAPABILITY_DIGEST_BYTES]> {
    let bit_length = u64::try_from(bytes.len())
        .map_err(|_| Error::Exhausted)?
        .checked_mul(8)
        .ok_or(Error::Exhausted)?;
    let mut state = [
        0x6A09_E667, 0xBB67_AE85, 0x3C6E_F372, 0xA54F_F53A,
        0x510E_527F, 0x9B05_688C, 0x1F83_D9AB, 0x5BE0_CD19,
    ];
    let mut chunks = bytes.chunks_exact(64);
    for chunk in &mut chunks {
        let mut block = [0_u8; 64];
        block.copy_from_slice(chunk);
        sha256_compress(&mut state, &block);
    }
    let remainder = chunks.remainder();
    let mut tail = [0_u8; 128];
    tail[..remainder.len()].copy_from_slice(remainder);
    tail[remainder.len()] = 0x80;
    let total_tail = if remainder.len() < 56 { 64 } else { 128 };
    tail[total_tail - 8..total_tail].copy_from_slice(&bit_length.to_be_bytes());
    for chunk in tail[..total_tail].chunks_exact(64) {
        let mut block = [0_u8; 64];
        block.copy_from_slice(chunk);
        sha256_compress(&mut state, &block);
    }
    let mut digest = [0_u8; CAPABILITY_DIGEST_BYTES];
    for (chunk, value) in digest.chunks_exact_mut(4).zip(state.iter().take(4)) {
        chunk.copy_from_slice(&value.to_be_bytes());
    }
    Ok(digest)
}

/// 计算与 C 版本一致的 SHA-256 前 16 B canonical 摘要。
///
/// # Errors
///
/// Record 非法或摘要计算失败时返回错误。
pub fn capability_digest(record: CapabilityRecord) -> Result<[u8; CAPABILITY_DIGEST_BYTES]> {
    let mut encoded = [0; CAPABILITY_RECORD_BYTES];
    encode_capability_record(record, &mut encoded)?;
    let digest = sha256_128(&encoded)?;
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
