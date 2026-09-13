use ucn_types::{
    AddressWidth, Error, HeaderContract, HopProfile, NodeAddress, OriginSecurity, PayloadKind,
    ProtocolOpcode, Result, ServiceId,
};

use crate::common::{CommonHeader, read_address, read_u16_be, read_u32_be, write_address};

const C1_FIXED_BYTES_EXCLUDING_ADDRESSES: usize = 9;

/// 借用 Payload 的 C1 STATELESS 帧。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct C1Frame<'a> {
    /// Common Header。
    pub common: CommonHeader,
    /// 普通 Source Address。
    pub source: NodeAddress,
    /// 普通 Destination Address。
    pub destination: NodeAddress,
    /// Service ID。
    pub service_id: ServiceId,
    /// Origin Sequence。
    pub origin_sequence: ucn_types::OriginSequence,
    /// Data、Opcode Body 或其他上层 Envelope，直接借用调用方内存。
    pub payload: &'a [u8],
}

/// 计算 C1 `O0/H0` 帧的精确编码长度。
///
/// # Errors
///
/// 长度计算溢出时返回 [`Error::Argument`]。
pub fn c1_o0_h0_encoded_size(width: AddressWidth, payload_bytes: usize) -> Result<usize> {
    C1_FIXED_BYTES_EXCLUDING_ADDRESSES
        .checked_add(width.bytes() * 2)
        .and_then(|base| base.checked_add(payload_bytes))
        .ok_or(Error::Argument)
}

fn validate_payload(kind: PayloadKind, payload: &[u8]) -> Result<()> {
    match kind {
        PayloadKind::Data => Ok(()),
        PayloadKind::Transfer => {
            if payload.len() < 2 {
                return Err(Error::Malformed);
            }
            let subtype = read_u16_be(payload, 0);
            if subtype == 0 || (subtype & 0x8000 != 0 && subtype != 0x8000) {
                return Err(Error::Malformed);
            }
            Ok(())
        }
        PayloadKind::Control | PayloadKind::Diagnostic => {
            if payload.len() < 2 {
                return Err(Error::Malformed);
            }
            let opcode = ProtocolOpcode::try_from(read_u16_be(payload, 0))?;
            if (kind == PayloadKind::Diagnostic) != opcode.is_diagnostic() {
                return Err(Error::Malformed);
            }
            Ok(())
        }
    }
}

fn validate_frame(frame: &C1Frame<'_>, width: AddressWidth, profile: HopProfile) -> Result<()> {
    if frame.common.contract != HeaderContract::C1 {
        return Err(Error::Argument);
    }
    if profile != HopProfile::H0 || frame.common.origin_security != OriginSecurity::O0 {
        return Err(Error::Unsupported);
    }
    NodeAddress::new(frame.source.get(), width)?;
    NodeAddress::new(frame.destination.get(), width)?;
    validate_payload(frame.common.payload_kind, frame.payload).map_err(|_| Error::Argument)
}

/// 将 C1 `O0/H0` 帧编码到调用方固定缓冲区。
///
/// 所有检查都在首次写入前完成；返回错误时 `output` 保持逐字节不变。
///
/// # Errors
///
/// 字段组合非法返回 [`Error::Argument`]，尚未实施的 Security Profile 返回
/// [`Error::Unsupported`]，缓冲区不足返回 [`Error::NoSpace`]。
pub fn encode_c1_o0_h0(
    frame: &C1Frame<'_>,
    width: AddressWidth,
    profile: HopProfile,
    output: &mut [u8],
) -> Result<usize> {
    validate_frame(frame, width, profile)?;
    let encoded_size = c1_o0_h0_encoded_size(width, frame.payload.len())?;
    if output.len() < encoded_size {
        return Err(Error::NoSpace);
    }

    let width_bytes = width.bytes();
    let source_offset = 3;
    let destination_offset = source_offset + width_bytes;
    let service_offset = destination_offset + width_bytes;
    let sequence_offset = service_offset + 2;
    let payload_offset = sequence_offset + 4;

    frame.common.encode(output);
    write_address(output, source_offset, width_bytes, frame.source.get());
    write_address(
        output,
        destination_offset,
        width_bytes,
        frame.destination.get(),
    );
    output[service_offset..sequence_offset].copy_from_slice(&frame.service_id.get().to_be_bytes());
    output[sequence_offset..payload_offset]
        .copy_from_slice(&frame.origin_sequence.get().to_be_bytes());
    output[payload_offset..encoded_size].copy_from_slice(frame.payload);
    Ok(encoded_size)
}

/// 从可信 Carrier 边界内解码一个 C1 `O0/H0` 帧。
///
/// # Errors
///
/// 非法字节返回 [`Error::Malformed`]；合法但尚未实施的 Security Profile 返回
/// [`Error::Unsupported`]。
pub fn decode_c1_o0_h0(
    input: &[u8],
    width: AddressWidth,
    profile: HopProfile,
) -> Result<C1Frame<'_>> {
    let base_size = c1_o0_h0_encoded_size(width, 0)?;
    if input.len() < base_size {
        return Err(Error::Malformed);
    }

    let common = CommonHeader::decode(input)?;
    if common.contract != HeaderContract::C1 {
        return Err(Error::Unsupported);
    }
    if profile != HopProfile::H0 || common.origin_security != OriginSecurity::O0 {
        return Err(Error::Unsupported);
    }

    let width_bytes = width.bytes();
    let source_offset = 3;
    let destination_offset = source_offset + width_bytes;
    let service_offset = destination_offset + width_bytes;
    let sequence_offset = service_offset + 2;
    let payload_offset = sequence_offset + 4;

    let source = NodeAddress::new(read_address(input, source_offset, width_bytes), width)
        .map_err(|_| Error::Malformed)?;
    let destination = NodeAddress::new(read_address(input, destination_offset, width_bytes), width)
        .map_err(|_| Error::Malformed)?;
    let service_id =
        ServiceId::new(read_u16_be(input, service_offset)).map_err(|_| Error::Malformed)?;
    let origin_sequence = ucn_types::OriginSequence::new(read_u32_be(input, sequence_offset))
        .map_err(|_| Error::Malformed)?;
    let payload = &input[payload_offset..];
    validate_payload(common.payload_kind, payload)?;

    Ok(C1Frame {
        common,
        source,
        destination,
        service_id,
        origin_sequence,
        payload,
    })
}
