use ucn_types::{
    AddressWidth, BindingGeneration, C0TransactionId, DeliveryGuarantee, Error, HeaderContract,
    HopProfile, NodeAddress, OpcodeDomain, OriginSecurity, PayloadKind, ProtocolOpcode, RealmId,
    Result,
};

use crate::common::{
    COMMON_HEADER_BYTES, CommonHeader, read_address, read_u16_be, read_u32_be, read_u64_be,
    write_address,
};

const C0_FIXED_BYTES_EXCLUDING_ADDRESSES: usize = 25;

/// C0 可见的地址类别。保留地址只允许出现在受限的一跳 Bootstrap 上下文。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum C0Address {
    /// 未绑定节点使用的全 0 地址。
    Unbound,
    /// 一跳 Address Authority 使用的该宽度全 1 地址。
    LinkLocalAuthority,
    /// 普通已绑定节点地址。
    Bound(NodeAddress),
}

impl C0Address {
    fn decode(value: u32, width: AddressWidth) -> Result<Self> {
        if value == 0 {
            return Ok(Self::Unbound);
        }
        if value == width.reserved_max() {
            return Ok(Self::LinkLocalAuthority);
        }
        NodeAddress::new(value, width)
            .map(Self::Bound)
            .map_err(|_| Error::Malformed)
    }

    const fn raw(self, width: AddressWidth) -> Result<u32> {
        match self {
            Self::Unbound => Ok(0),
            Self::LinkLocalAuthority => Ok(width.reserved_max()),
            Self::Bound(address) => {
                if address.get() >= width.reserved_max() {
                    Err(Error::Argument)
                } else {
                    Ok(address.get())
                }
            }
        }
    }

    const fn is_bound(self) -> bool {
        matches!(self, Self::Bound(_))
    }
}

/// 借用 Payload 的 C0 ABSOLUTE 帧。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct C0Frame<'a> {
    /// Common Header。
    pub common: CommonHeader,
    /// Realm ID。
    pub realm_id: RealmId,
    /// Source Address。
    pub source: C0Address,
    /// Destination Address。
    pub destination: C0Address,
    /// Source Address Binding Generation。
    pub source_binding_generation: BindingGeneration,
    /// Destination Address Binding Generation。
    pub destination_binding_generation: BindingGeneration,
    /// 64-bit C0 Transaction ID。
    pub transaction_id: C0TransactionId,
    /// 精确 Protocol Opcode。
    pub opcode: ProtocolOpcode,
    /// Opcode Body 或 Operation Envelope，直接借用调用方内存。
    pub payload: &'a [u8],
}

/// 计算 C0 `O0/H0` 帧的精确编码长度。
///
/// # Errors
///
/// 长度计算溢出时返回 [`Error::Argument`]。
pub fn c0_o0_h0_encoded_size(width: AddressWidth, payload_bytes: usize) -> Result<usize> {
    C0_FIXED_BYTES_EXCLUDING_ADDRESSES
        .checked_add(width.bytes() * 2)
        .and_then(|base| base.checked_add(payload_bytes))
        .ok_or(Error::Argument)
}

fn validate_opcode_kind(kind: PayloadKind, opcode: ProtocolOpcode) -> Result<()> {
    match kind {
        PayloadKind::Control if !opcode.is_diagnostic() => Ok(()),
        PayloadKind::Diagnostic if opcode.is_diagnostic() => Ok(()),
        PayloadKind::Control
        | PayloadKind::Diagnostic
        | PayloadKind::Data
        | PayloadKind::Transfer => Err(Error::Malformed),
    }
}

fn validate_frame(frame: &C0Frame<'_>, width: AddressWidth, profile: HopProfile) -> Result<()> {
    if frame.common.contract != HeaderContract::C0 {
        return Err(Error::Argument);
    }
    if frame.common.delivery == DeliveryGuarantee::Latest {
        return Err(Error::Argument);
    }
    if profile != HopProfile::H0 || frame.common.origin_security != OriginSecurity::O0 {
        return Err(Error::Unsupported);
    }
    validate_opcode_kind(frame.common.payload_kind, frame.opcode).map_err(|_| Error::Argument)?;

    let source = frame.source.raw(width)?;
    let destination = frame.destination.raw(width)?;
    let source_unbound = frame.source_binding_generation.is_unbound();
    let destination_unbound = frame.destination_binding_generation.is_unbound();
    if source_unbound != destination_unbound {
        return Err(Error::Argument);
    }

    if source_unbound {
        if frame.common.hop_limit.get() != 1
            || frame.opcode.domain() != OpcodeDomain::IdentitySecurity
            || frame.source != C0Address::Unbound
            || frame.destination != C0Address::LinkLocalAuthority
        {
            return Err(Error::Argument);
        }
    } else if !frame.source.is_bound() || !frame.destination.is_bound() {
        return Err(Error::Argument);
    }

    if source > width.reserved_max() || destination > width.reserved_max() {
        return Err(Error::Argument);
    }
    Ok(())
}

/// 将 C0 `O0/H0` 帧编码到调用方固定缓冲区。
///
/// 所有检查都在首次写入前完成；返回错误时 `output` 保持逐字节不变。安全 Rust 无法同时构造与
/// `output` 重叠的 Payload 借用，因此本 API 不需要运行期别名探测。
///
/// # Errors
///
/// 字段组合非法返回 [`Error::Argument`]，受保护 Profile 返回 [`Error::Unsupported`]，缓冲区不足
/// 返回 [`Error::NoSpace`]。
pub fn encode_c0_o0_h0(
    frame: &C0Frame<'_>,
    width: AddressWidth,
    profile: HopProfile,
    output: &mut [u8],
) -> Result<usize> {
    validate_frame(frame, width, profile)?;
    let encoded_size = c0_o0_h0_encoded_size(width, frame.payload.len())?;
    if output.len() < encoded_size {
        return Err(Error::NoSpace);
    }

    let width_bytes = width.bytes();
    let source_offset = 7;
    let destination_offset = source_offset + width_bytes;
    let source_generation_offset = destination_offset + width_bytes;
    let destination_generation_offset = source_generation_offset + 4;
    let transaction_offset = destination_generation_offset + 4;
    let opcode_offset = transaction_offset + 8;
    let payload_offset = opcode_offset + 2;
    let source_raw = frame.source.raw(width)?;
    let destination_raw = frame.destination.raw(width)?;

    frame.common.encode(output);
    output[COMMON_HEADER_BYTES..7].copy_from_slice(&frame.realm_id.get().to_be_bytes());
    write_address(output, source_offset, width_bytes, source_raw);
    write_address(output, destination_offset, width_bytes, destination_raw);
    output[source_generation_offset..destination_generation_offset]
        .copy_from_slice(&frame.source_binding_generation.get().to_be_bytes());
    output[destination_generation_offset..transaction_offset]
        .copy_from_slice(&frame.destination_binding_generation.get().to_be_bytes());
    output[transaction_offset..opcode_offset]
        .copy_from_slice(&frame.transaction_id.get().to_be_bytes());
    output[opcode_offset..payload_offset].copy_from_slice(&u16::from(frame.opcode).to_be_bytes());
    output[payload_offset..encoded_size].copy_from_slice(frame.payload);
    Ok(encoded_size)
}

/// 从可信 Carrier 边界内解码一个 C0 `O0/H0` 帧。
///
/// # Errors
///
/// 非法字节返回 [`Error::Malformed`]；合法但尚未实施的 Security/Hop Profile 返回
/// [`Error::Unsupported`]。
pub fn decode_c0_o0_h0(
    input: &[u8],
    width: AddressWidth,
    profile: HopProfile,
) -> Result<C0Frame<'_>> {
    let base_size = c0_o0_h0_encoded_size(width, 0)?;
    if input.len() < base_size {
        return Err(Error::Malformed);
    }

    let common = CommonHeader::decode(input)?;
    if common.contract != HeaderContract::C0 {
        return if u8::from(common.contract) <= u8::from(HeaderContract::C5) {
            Err(Error::Unsupported)
        } else {
            Err(Error::Malformed)
        };
    }
    if common.delivery == DeliveryGuarantee::Latest {
        return Err(Error::Malformed);
    }
    if profile != HopProfile::H0 || common.origin_security != OriginSecurity::O0 {
        return Err(Error::Unsupported);
    }

    let width_bytes = width.bytes();
    let source_offset = 7;
    let destination_offset = source_offset + width_bytes;
    let source_generation_offset = destination_offset + width_bytes;
    let destination_generation_offset = source_generation_offset + 4;
    let transaction_offset = destination_generation_offset + 4;
    let opcode_offset = transaction_offset + 8;
    let payload_offset = opcode_offset + 2;

    let realm_id =
        RealmId::new(read_u32_be(input, COMMON_HEADER_BYTES)).map_err(|_| Error::Malformed)?;
    let source = C0Address::decode(read_address(input, source_offset, width_bytes), width)?;
    let destination =
        C0Address::decode(read_address(input, destination_offset, width_bytes), width)?;
    let source_generation_raw = read_u32_be(input, source_generation_offset);
    let destination_generation_raw = read_u32_be(input, destination_generation_offset);
    let source_binding_generation = if source_generation_raw == 0 {
        BindingGeneration::UNBOUND
    } else {
        BindingGeneration::active(source_generation_raw).map_err(|_| Error::Malformed)?
    };
    let destination_binding_generation = if destination_generation_raw == 0 {
        BindingGeneration::UNBOUND
    } else {
        BindingGeneration::active(destination_generation_raw).map_err(|_| Error::Malformed)?
    };
    let transaction_id = C0TransactionId::new(read_u64_be(input, transaction_offset))
        .map_err(|_| Error::Malformed)?;
    let opcode = ProtocolOpcode::try_from(read_u16_be(input, opcode_offset))?;
    validate_opcode_kind(common.payload_kind, opcode)?;

    let frame = C0Frame {
        common,
        realm_id,
        source,
        destination,
        source_binding_generation,
        destination_binding_generation,
        transaction_id,
        opcode,
        payload: &input[payload_offset..],
    };
    validate_frame(&frame, width, profile).map_err(|error| match error {
        Error::Unsupported => Error::Unsupported,
        _ => Error::Malformed,
    })?;
    Ok(frame)
}
