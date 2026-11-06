//! Bounded authenticated-control body layout; no authentication claim from a codec.
//! 有界认证控制正文布局；Codec本身不声称认证。
use ucn_types::{Error, ProtocolOpcode, Result};
/// Exact local metadata bytes; 精确局部元数据长度。
pub const CAPABILITY_FRAGMENT_PREFIX: usize = 6;
/// Exactly two parts; 固定两片，不分配动态位图。
pub const CAPABILITY_FRAGMENT_COUNT: u8 = 2;
/// Borrowed structural body, never an authenticated receipt; 借用结构正文，不是认证凭据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CapabilityFragment<'a> {
    /// Original logical opcode; 原逻辑Opcode。
    pub opcode: ProtocolOpcode,
    /// Exact original length (68 or 75); 精确原长度。
    pub total: u16,
    /// Zero-based index; 从零计数的片号。
    pub index: u8,
    /// Canonical equalized partition; 规范均分正文。
    pub chunk: &'a [u8],
}
/// Exact body length for fragmentable opcodes; 可分片Opcode的精确原长度。
/// # Errors
/// Other opcodes cannot use this bounded envelope.
pub fn capability_fragment_length(op: ProtocolOpcode) -> Result<usize> {
    match op {
        ProtocolOpcode::CapabilityAdvertise => Ok(68),
        ProtocolOpcode::CapabilityAck => Ok(75),
        _ => Err(Error::Unsupported),
    }
}
/// Checked canonical half-open chunk range; 校验后的规范半开区间。
/// # Errors
/// Wrong original length/opcode/index rejects.
pub fn capability_fragment_range(
    op: ProtocolOpcode,
    total: usize,
    index: u8,
) -> Result<core::ops::Range<usize>> {
    if total != capability_fragment_length(op)? || index >= 2 {
        return Err(Error::Malformed);
    }
    let half = total.div_ceil(2);
    let start = usize::from(index) * half;
    Ok(start..(start + half).min(total))
}
/// Encode six metadata bytes and the exact chunk; 编码六字节元数据及精确分块。
/// # Errors
/// Invalid shape/short output leaves output unchanged.
pub fn encode_capability_fragment(v: CapabilityFragment<'_>, out: &mut [u8]) -> Result<usize> {
    let range = capability_fragment_range(v.opcode, usize::from(v.total), v.index)?;
    if range.len() != v.chunk.len() {
        return Err(Error::Malformed);
    }
    let n = 6 + v.chunk.len();
    if out.len() < n {
        return Err(Error::NoSpace);
    }
    out[..2].copy_from_slice(&u16::from(v.opcode).to_be_bytes());
    out[2..4].copy_from_slice(&v.total.to_be_bytes());
    out[4] = v.index;
    out[5] = CAPABILITY_FRAGMENT_COUNT;
    out[6..n].copy_from_slice(v.chunk);
    Ok(n)
}
/// Strict structural decode; 严格结构解码。
/// # Errors
/// Noncanonical length/index/count/opcode rejects before publishing a view.
pub fn decode_capability_fragment(input: &[u8]) -> Result<CapabilityFragment<'_>> {
    if input.len() < 6 || input[5] != CAPABILITY_FRAGMENT_COUNT {
        return Err(Error::Malformed);
    }
    let op = ProtocolOpcode::try_from(u16::from_be_bytes([input[0], input[1]]))?;
    let total = u16::from_be_bytes([input[2], input[3]]);
    let range = capability_fragment_range(op, usize::from(total), input[4])?;
    if input.len() != 6 + range.len() {
        return Err(Error::Malformed);
    }
    Ok(CapabilityFragment {
        opcode: op,
        total,
        index: input[4],
        chunk: &input[6..],
    })
}
