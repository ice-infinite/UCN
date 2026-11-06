use ucn_identity::Principal;
use ucn_types::{Error, Result};

use crate::codec::{
    BootstrapEvent, BootstrapFlow, BootstrapPhase, LOGICAL_MAX_BYTES, TRANSCRIPT_BYTES,
    event_matches_phase,
};
use crate::owner::AdmissionKey;

/// Bootstrap 分片固定 Header 长度。
pub const FRAGMENT_HEADER_BYTES: usize = 36;
/// 单片最大数据长度。
pub const FRAGMENT_DATA_BYTES: usize = 128;
/// 一个逻辑事件的最大分片数。
pub const MAX_FRAGMENTS: usize = 16;

/// 一个严格连续的 Bootstrap 分片。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BootstrapFragment {
    /// Bootstrap 流。
    pub flow: BootstrapFlow,
    /// 该事件完成后的阶段。
    pub phase: BootstrapPhase,
    /// 从零开始的分片索引。
    pub fragment_index: u8,
    /// 逻辑事件的精确分片数。
    pub fragment_count: u8,
    /// Frozen per-event chunk size (32..128); 冻结的每事件分块长度。
    pub chunk_bytes: u8,
    /// 逻辑事件总长度。
    pub total_length: u16,
    /// 本片在逻辑事件中的偏移。
    pub fragment_offset: u16,
    /// 本片数据长度。
    pub fragment_length: u16,
    /// JOIN 事务 ID。
    pub transaction_id: u64,
    /// HELLO 身份摘要。
    pub identity_digest: Principal,
    /// 固定容量数据；有效长度之后必须为零。
    pub data: [u8; FRAGMENT_DATA_BYTES],
}

impl BootstrapFragment {
    /// 从完整逻辑事件构造指定索引的唯一 canonical 分片。
    ///
    /// # Errors
    ///
    /// 主键、阶段、逻辑长度或分片索引不成立时返回错误。
    pub fn from_logical(
        key: AdmissionKey,
        flow: BootstrapFlow,
        event: BootstrapEvent,
        phase: BootstrapPhase,
        fragment_index: u8,
        logical: &[u8],
    ) -> Result<Self> {
        Self::from_logical_chunk(key, flow, event, phase, fragment_index, 128, logical)
    }

    /// Freeze a path-MTU-derived chunk for the whole event. 按冻结MTU构造整事件分块。
    /// # Errors
    /// Invalid chunk, key, phase, length or index rejects before publication.
    #[allow(clippy::too_many_arguments)]
    pub fn from_logical_chunk(
        key: AdmissionKey,
        flow: BootstrapFlow,
        event: BootstrapEvent,
        phase: BootstrapPhase,
        fragment_index: u8,
        chunk_bytes: u8,
        logical: &[u8],
    ) -> Result<Self> {
        if key.transaction_id == 0
            || event.opcode().is_err()
            || !(32..=128).contains(&chunk_bytes)
            || !event_matches_phase(event, flow, phase)
            || logical.len() < TRANSCRIPT_BYTES + 2
            || logical.len() > LOGICAL_MAX_BYTES
        {
            return Err(Error::Argument);
        }
        let chunk = usize::from(chunk_bytes);
        let count = logical.len().div_ceil(chunk);
        if count == 0 || count > MAX_FRAGMENTS || usize::from(fragment_index) >= count {
            return Err(Error::Argument);
        }
        let offset = usize::from(fragment_index) * chunk;
        let length = (logical.len() - offset).min(chunk);
        let mut data = [0; FRAGMENT_DATA_BYTES];
        data[..length].copy_from_slice(&logical[offset..offset + length]);
        let fragment = Self {
            flow,
            phase,
            fragment_index,
            fragment_count: u8::try_from(count).map_err(|_| Error::NoSpace)?,
            chunk_bytes,
            total_length: u16::try_from(logical.len()).map_err(|_| Error::NoSpace)?,
            fragment_offset: u16::try_from(offset).map_err(|_| Error::NoSpace)?,
            fragment_length: u16::try_from(length).map_err(|_| Error::NoSpace)?,
            transaction_id: key.transaction_id,
            identity_digest: key.identity_digest,
            data,
        };
        validate_fragment(fragment)?;
        Ok(fragment)
    }
}

/// 编码一个 canonical Bootstrap 分片。
///
/// # Errors
///
/// 分片字段非 canonical 或输出容量不足时返回错误且不写输出。
pub fn encode_fragment(fragment: BootstrapFragment, output: &mut [u8]) -> Result<usize> {
    validate_fragment(fragment)?;
    let data_length = usize::from(fragment.fragment_length);
    let encoded_length = FRAGMENT_HEADER_BYTES + data_length;
    if output.len() < encoded_length {
        return Err(Error::NoSpace);
    }
    let mut encoded = [0; FRAGMENT_HEADER_BYTES + FRAGMENT_DATA_BYTES];
    encoded[0] = 1;
    encoded[1] = fragment.flow as u8;
    encoded[2] = fragment.phase as u8;
    encoded[3] = fragment.fragment_index;
    encoded[4] = fragment.fragment_count;
    encoded[5] = fragment.chunk_bytes;
    encoded[6..8].copy_from_slice(&fragment.total_length.to_be_bytes());
    encoded[8..10].copy_from_slice(&fragment.fragment_offset.to_be_bytes());
    encoded[10..12].copy_from_slice(&fragment.fragment_length.to_be_bytes());
    encoded[12..20].copy_from_slice(&fragment.transaction_id.to_be_bytes());
    encoded[20..36].copy_from_slice(&fragment.identity_digest.bytes());
    encoded[36..encoded_length].copy_from_slice(&fragment.data[..data_length]);
    output[..encoded_length].copy_from_slice(&encoded[..encoded_length]);
    Ok(encoded_length)
}

/// 解码一个 canonical Bootstrap 分片。
///
/// # Errors
///
/// 长度、版本、保留位或分片几何关系不成立时返回格式错误。
pub fn decode_fragment(input: &[u8]) -> Result<BootstrapFragment> {
    if input.len() < FRAGMENT_HEADER_BYTES + 1
        || input.len() > FRAGMENT_HEADER_BYTES + FRAGMENT_DATA_BYTES
        || input[0] != 1
    {
        return Err(Error::Malformed);
    }
    let fragment_length =
        u16::from_be_bytes(input[10..12].try_into().map_err(|_| Error::Malformed)?);
    if fragment_length == 0 || input.len() != FRAGMENT_HEADER_BYTES + usize::from(fragment_length) {
        return Err(Error::Malformed);
    }
    let mut data = [0; FRAGMENT_DATA_BYTES];
    data[..usize::from(fragment_length)].copy_from_slice(&input[FRAGMENT_HEADER_BYTES..]);
    let fragment = BootstrapFragment {
        flow: BootstrapFlow::try_from(input[1])?,
        phase: BootstrapPhase::try_from(input[2])?,
        fragment_index: input[3],
        fragment_count: input[4],
        chunk_bytes: input[5],
        total_length: u16::from_be_bytes(input[6..8].try_into().map_err(|_| Error::Malformed)?),
        fragment_offset: u16::from_be_bytes(input[8..10].try_into().map_err(|_| Error::Malformed)?),
        fragment_length,
        transaction_id: u64::from_be_bytes(input[12..20].try_into().map_err(|_| Error::Malformed)?),
        identity_digest: Principal::new(input[20..36].try_into().map_err(|_| Error::Malformed)?)
            .map_err(|_| Error::Malformed)?,
        data,
    };
    validate_fragment(fragment).map_err(|_| Error::Malformed)?;
    Ok(fragment)
}

/// 一个固定容量的单事务重组器。
pub struct Reassembly {
    occupied: bool,
    protocol_opcode: u16,
    phase: BootstrapPhase,
    key: Option<AdmissionKey>,
    total_length: u16,
    fragment_count: u8,
    received_mask: u16,
    chunk_bytes: u8,
    deadline_us: u64,
    logical: [u8; LOGICAL_MAX_BYTES],
}

impl Reassembly {
    /// 建立空重组器。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            occupied: false,
            protocol_opcode: 0,
            phase: BootstrapPhase::Aborted,
            key: None,
            total_length: 0,
            fragment_count: 0,
            received_mask: 0,
            chunk_bytes: 0,
            deadline_us: 0,
            logical: [0; LOGICAL_MAX_BYTES],
        }
    }

    /// 在精确 Admission pending 与不可刷新 Deadline 域内接纳一片。
    ///
    /// # Errors
    ///
    /// Opcode/阶段/主键/Deadline 错绑、非第零首片或冲突重放时返回错误。
    pub fn accept(
        &mut self,
        protocol_opcode: u16,
        key: AdmissionKey,
        pending_deadline_us: u64,
        now_us: u64,
        fragment: BootstrapFragment,
    ) -> Result<bool> {
        let event = BootstrapEvent::from_opcode(protocol_opcode)?;
        validate_fragment(fragment)?;
        if pending_deadline_us == 0
            || now_us == 0
            || now_us >= pending_deadline_us
            || !event_matches_phase(event, fragment.flow, fragment.phase)
            || fragment.transaction_id != key.transaction_id
            || fragment.identity_digest != key.identity_digest
        {
            return Err(Error::Argument);
        }
        if !self.occupied {
            if fragment.fragment_index != 0 {
                return Err(Error::NotFound);
            }
        } else if self.protocol_opcode != protocol_opcode
            || self.phase != fragment.phase
            || self.key != Some(key)
            || self.total_length != fragment.total_length
            || self.fragment_count != fragment.fragment_count
            || self.chunk_bytes != fragment.chunk_bytes
            || self.deadline_us != pending_deadline_us
        {
            return Err(Error::State);
        }
        let bit = 1_u16 << fragment.fragment_index;
        let offset = usize::from(fragment.fragment_offset);
        let length = usize::from(fragment.fragment_length);
        let already_received = self.occupied && self.received_mask & bit != 0;
        if already_received && self.logical[offset..offset + length] != fragment.data[..length] {
            return Err(Error::Replay);
        }
        if !self.occupied {
            *self = Self::new();
            self.occupied = true;
            self.protocol_opcode = protocol_opcode;
            self.phase = fragment.phase;
            self.key = Some(key);
            self.total_length = fragment.total_length;
            self.fragment_count = fragment.fragment_count;
            self.chunk_bytes = fragment.chunk_bytes;
            self.deadline_us = pending_deadline_us;
        }
        if !already_received {
            self.logical[offset..offset + length].copy_from_slice(&fragment.data[..length]);
            self.received_mask |= bit;
        }
        Ok(self.received_mask == complete_mask(self.fragment_count))
    }

    /// 借用已完整重组的逻辑事件。
    ///
    /// # Errors
    ///
    /// 重组器未占用或分片尚未完整时返回状态错误。
    pub fn borrow(&self) -> Result<&[u8]> {
        if !self.occupied
            || self.fragment_count == 0
            || usize::from(self.fragment_count) > MAX_FRAGMENTS
            || self.received_mask != complete_mask(self.fragment_count)
        {
            return Err(Error::State);
        }
        Ok(&self.logical[..usize::from(self.total_length)])
    }

    /// 显式清理重组状态；错误分片不会触发隐式清理。
    pub fn reset(&mut self) {
        *self = Self::new();
    }
}

impl Default for Reassembly {
    fn default() -> Self {
        Self::new()
    }
}

fn validate_fragment(fragment: BootstrapFragment) -> Result<()> {
    let total = usize::from(fragment.total_length);
    if !(BootstrapPhase::AuthorityVerified..=BootstrapPhase::FinalDurable).contains(&fragment.phase)
        || !(32..=128).contains(&fragment.chunk_bytes)
        || !(TRANSCRIPT_BYTES + 2..=LOGICAL_MAX_BYTES).contains(&total)
        || fragment.transaction_id == 0
    {
        return Err(Error::Argument);
    }
    let chunk = usize::from(fragment.chunk_bytes);
    let expected_count = total.div_ceil(chunk);
    if expected_count == 0
        || expected_count > MAX_FRAGMENTS
        || usize::from(fragment.fragment_count) != expected_count
        || usize::from(fragment.fragment_index) >= expected_count
    {
        return Err(Error::Argument);
    }
    let expected_offset = usize::from(fragment.fragment_index) * chunk;
    let expected_length = (total - expected_offset).min(chunk);
    if usize::from(fragment.fragment_offset) != expected_offset
        || usize::from(fragment.fragment_length) != expected_length
        || fragment.data[expected_length..]
            .iter()
            .any(|byte| *byte != 0)
    {
        return Err(Error::Argument);
    }
    Ok(())
}

const fn complete_mask(fragment_count: u8) -> u16 {
    u16::MAX >> (16 - fragment_count)
}
