use crate::DeliveryAck;
use ucn_types::{DeliveryGuarantee, Error, InteractionRole, Result, operation_flags_valid};

/// Delivery ACK payload bytes; ACK固定载荷。
pub const DELIVERY_ACK_BYTES: usize = 9;
/// Maximum on-wire fragment count; 线上分片上限。
pub const MAX_FRAGMENTS: u16 = 32767;
/// Parent Kind; 父上下文种类。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParentKind {
    /// Independent durable C1 Parent; 独立耐久C1父域。
    C1 = 1,
    /// Active Flow Parent; 已激活Flow父域。
    Flow = 2,
}

/// Frozen Transfer Setup; 不可变Transfer Setup。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferSetup {
    /// Parent kind; 父域类型。
    pub kind: ParentKind,
    /// Parent context ID (C1=0); 父短ID。
    pub parent_id: u16,
    /// Parent generation; 父代际。
    pub parent_generation: u32,
    /// Transfer high-water ID; `Transfer单调ID`。
    pub transfer_id: u32,
    /// Exact service; 精确Service。
    pub service: u16,
    /// Total business bytes; 纯业务长度。
    pub total_bytes: u32,
    /// Canonical message digest; 完整业务摘要。
    pub digest: [u8; 16],
    /// Delivery; 交付语义。
    pub delivery: DeliveryGuarantee,
    /// Interaction; 交互角色。
    pub interaction: InteractionRole,
    /// Frozen fragment count; 分片数。
    pub fragments: u16,
    /// Business bytes per fragment; 每片业务预算。
    pub fragment_bytes: u16,
    /// Initial non-renewable lifetime; 不续期寿命。
    pub lifetime_ms: u32,
    /// Conditional Operation ID; `条件OperationID`。
    pub operation_id: u64,
    /// Shared execution-mode/final flags; 共同执行模式/终态位。
    pub operation_flags: u8,
    /// Result/Error code only; 条件结果码。
    pub result_code: u16,
}
impl TransferSetup {
    /// Exact conditional payload length; 条件精确长度。
    #[must_use]
    pub const fn encoded_bytes(self) -> usize {
        match self.interaction {
            InteractionRole::OneWay => 43,
            InteractionRole::Request => 52,
            InteractionRole::Result | InteractionRole::Error => 54,
        }
    }
    /// Verify every immutable field; 验证所有不可变字段。
    /// # Errors
    /// Invalid parent, correlation, count or duration is rejected.
    pub fn validate(self) -> Result<()> {
        if self.parent_generation == 0
            || self.transfer_id == 0
            || self.service == 0
            || self.service == u16::MAX
            || self.total_bytes == 0
            || self.digest == [0; 16]
            || self.fragments == 0
            || self.fragments > MAX_FRAGMENTS
            || self.fragment_bytes == 0
            || self.lifetime_ms == 0
            || self.lifetime_ms > i32::MAX as u32
            || !operation_flags_valid(self.interaction, self.operation_flags)
        {
            return Err(Error::Argument);
        }
        if (self.kind == ParentKind::C1 && self.parent_id != 0)
            || (self.kind == ParentKind::Flow && self.parent_id == 0)
        {
            return Err(Error::Argument);
        }
        if self.interaction == InteractionRole::OneWay {
            if self.operation_id != 0 || self.operation_flags != 0 || self.result_code != 0 {
                return Err(Error::Argument);
            }
        } else if self.operation_id == 0
            || (self.interaction == InteractionRole::Request && self.result_code != 0)
        {
            return Err(Error::Argument);
        }
        let upper = u64::from(self.fragments) * u64::from(self.fragment_bytes);
        if upper < u64::from(self.total_bytes)
            || upper - u64::from(self.fragment_bytes) >= u64::from(self.total_bytes)
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// Fragment prefix; 分片前缀。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FragmentPrefix {
    /// Parent kind; 父域。
    pub kind: ParentKind,
    /// Transfer ID; `TransferID`。
    pub transfer_id: u32,
    /// C1 generation (Flow derives it from context); C1父代际。
    pub parent_generation: u32,
    /// Zero-based index; 从零索引。
    pub index: u16,
    /// Frozen count; 冻结总数。
    pub count: u16,
}

/// SACK/Credit fixed window; 固定SACK窗口。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Sack {
    /// Parent type; 父类型。
    pub kind: ParentKind,
    /// Transfer ID; `TransferID`。
    pub transfer_id: u32,
    /// C1 generation; C1父代际。
    pub parent_generation: u32,
    /// C1 original service; `C1原Service`。
    pub original_service: u16,
    /// Window base; 窗口起点。
    pub base: u16,
    /// Received bitmap; 接收位图。
    pub bitmap: u32,
    /// Receiver credit; 接收credit。
    pub credit: u16,
}

/// Encode Delivery ACK after preflight; 预检后编码ACK。
/// # Errors
/// Invalid identity or capacity leaves output unchanged.
pub fn encode_ack(value: DeliveryAck, output: &mut [u8]) -> Result<usize> {
    if value.service == 0 || value.service == u16::MAX || value.sequence == 0 {
        return Err(Error::Argument);
    }
    if output.len() < 9 {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 9];
    b[0] = 1;
    b[1..3].copy_from_slice(&value.service.to_be_bytes());
    b[3..7].copy_from_slice(&value.sequence.to_be_bytes());
    b[7] = value.status;
    b[8] = value.credit;
    output[..9].copy_from_slice(&b);
    Ok(9)
}
/// Decode exact ACK; 解码精确ACK。
/// # Errors
/// Wrong size, subtype or identity is malformed.
pub fn decode_ack(input: &[u8]) -> Result<DeliveryAck> {
    if input.len() != 9 || input[0] != 1 {
        return Err(Error::Malformed);
    }
    let value = DeliveryAck {
        service: be16(input, 1),
        sequence: be32(input, 3),
        status: input[7],
        credit: input[8],
    };
    if value.service == 0 || value.service == u16::MAX || value.sequence == 0 {
        return Err(Error::Malformed);
    }
    Ok(value)
}

/// Encode exact Setup, rejecting invalid combinations before the first write.
/// 精确编码Setup，首次写前拒绝非法组合。
/// # Errors
/// Invalid fields or short output fail with zero writes.
pub fn encode_setup(value: TransferSetup, output: &mut [u8]) -> Result<usize> {
    value.validate()?;
    let size = value.encoded_bytes();
    if output.len() < size {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 54];
    b[0] = value.kind as u8;
    b[1..3].copy_from_slice(&value.parent_id.to_be_bytes());
    b[3..7].copy_from_slice(&value.parent_generation.to_be_bytes());
    b[7..11].copy_from_slice(&value.transfer_id.to_be_bytes());
    b[11..13].copy_from_slice(&value.service.to_be_bytes());
    b[13..17].copy_from_slice(&value.total_bytes.to_be_bytes());
    b[17..33].copy_from_slice(&value.digest);
    b[33] = value.delivery as u8;
    b[34] = value.interaction as u8;
    b[35..37].copy_from_slice(&value.fragments.to_be_bytes());
    b[37..39].copy_from_slice(&value.fragment_bytes.to_be_bytes());
    b[39..43].copy_from_slice(&value.lifetime_ms.to_be_bytes());
    if size > 43 {
        b[43..51].copy_from_slice(&value.operation_id.to_be_bytes());
        b[51] = value.operation_flags;
    }
    if size == 54 {
        b[52..54].copy_from_slice(&value.result_code.to_be_bytes());
    }
    output[..size].copy_from_slice(&b[..size]);
    Ok(size)
}
/// Decode Setup using role at frozen offset34; 按固定offset34角色解码。
/// # Errors
/// Conditional length or fields mismatch is malformed.
pub fn decode_setup(input: &[u8]) -> Result<TransferSetup> {
    if input.len() < 43 {
        return Err(Error::Malformed);
    }
    let kind = match input[0] {
        1 => ParentKind::C1,
        2 => ParentKind::Flow,
        _ => return Err(Error::Malformed),
    };
    let delivery = DeliveryGuarantee::try_from(input[33]).map_err(|_| Error::Malformed)?;
    let interaction = InteractionRole::try_from(input[34]).map_err(|_| Error::Malformed)?;
    let size = match interaction {
        InteractionRole::OneWay => 43,
        InteractionRole::Request => 52,
        _ => 54,
    };
    if input.len() != size {
        return Err(Error::Malformed);
    }
    let mut digest = [0; 16];
    digest.copy_from_slice(&input[17..33]);
    let value = TransferSetup {
        kind,
        parent_id: be16(input, 1),
        parent_generation: be32(input, 3),
        transfer_id: be32(input, 7),
        service: be16(input, 11),
        total_bytes: be32(input, 13),
        digest,
        delivery,
        interaction,
        fragments: be16(input, 35),
        fragment_bytes: be16(input, 37),
        lifetime_ms: be32(input, 39),
        operation_id: if size > 43 { be64(input, 43) } else { 0 },
        operation_flags: if size > 43 { input[51] } else { 0 },
        result_code: if size == 54 { be16(input, 52) } else { 0 },
    };
    value.validate().map_err(|_| Error::Malformed)?;
    Ok(value)
}

/// Encode Fragment prefix; 编码分片前缀。
/// # Errors
/// Wrong kind/count/generation or capacity is rejected before writing.
pub fn encode_fragment(value: FragmentPrefix, output: &mut [u8]) -> Result<usize> {
    if value.transfer_id == 0
        || value.count == 0
        || value.count > MAX_FRAGMENTS
        || value.index >= value.count
        || (value.kind == ParentKind::C1 && value.parent_generation == 0)
        || (value.kind == ParentKind::Flow && value.parent_generation != 0)
    {
        return Err(Error::Argument);
    }
    let size = if value.kind == ParentKind::C1 { 12 } else { 8 };
    if output.len() < size {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 12];
    b[..4].copy_from_slice(&value.transfer_id.to_be_bytes());
    let offset = if size == 12 {
        b[4..8].copy_from_slice(&value.parent_generation.to_be_bytes());
        8
    } else {
        4
    };
    b[offset..offset + 2].copy_from_slice(&value.index.to_be_bytes());
    b[offset + 2..offset + 4].copy_from_slice(&value.count.to_be_bytes());
    output[..size].copy_from_slice(&b[..size]);
    Ok(size)
}
/// Decode only the fixed prefix, returning payload offset; 解码固定前缀。
/// # Errors
/// SACK subtype or invalid count/index rejects.
pub fn decode_fragment(kind: ParentKind, input: &[u8]) -> Result<(FragmentPrefix, usize)> {
    let size = if kind == ParentKind::C1 { 12 } else { 8 };
    if input.len() < size {
        return Err(Error::Malformed);
    }
    let offset = size - 4;
    let value = FragmentPrefix {
        kind,
        transfer_id: be32(input, 0),
        parent_generation: if size == 12 { be32(input, 4) } else { 0 },
        index: be16(input, offset),
        count: be16(input, offset + 2),
    };
    if value.transfer_id == 0
        || value.count == 0
        || value.count > MAX_FRAGMENTS
        || value.index >= value.count
        || (kind == ParentKind::C1 && value.parent_generation == 0)
    {
        return Err(Error::Malformed);
    }
    Ok((value, size))
}
/// Encode frozen SACK layout; 编码固定SACK。
/// # Errors
/// Incomplete identity, subtype or short output fails before writing.
pub fn encode_sack(value: Sack, output: &mut [u8]) -> Result<usize> {
    if value.transfer_id == 0
        || (value.kind == ParentKind::C1
            && (value.parent_generation == 0 || value.original_service == 0))
        || (value.kind == ParentKind::Flow
            && (value.parent_generation != 0 || value.original_service != 0))
    {
        return Err(Error::Argument);
    }
    let size = if value.kind == ParentKind::C1 { 20 } else { 14 };
    if output.len() < size {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 20];
    b[..4].copy_from_slice(&value.transfer_id.to_be_bytes());
    let mut o = 4;
    if size == 20 {
        b[o..o + 4].copy_from_slice(&value.parent_generation.to_be_bytes());
        o += 4;
    }
    b[o..o + 2].copy_from_slice(&0x8000u16.to_be_bytes());
    o += 2;
    if size == 20 {
        b[o..o + 2].copy_from_slice(&value.original_service.to_be_bytes());
        o += 2;
    }
    b[o..o + 2].copy_from_slice(&value.base.to_be_bytes());
    b[o + 2..o + 6].copy_from_slice(&value.bitmap.to_be_bytes());
    b[o + 6..o + 8].copy_from_slice(&value.credit.to_be_bytes());
    output[..size].copy_from_slice(&b[..size]);
    Ok(size)
}
/// Decode exact SACK, never infer from state/length alone; 精确解码SACK子型。
/// # Errors
/// Wrong prefix kind or incomplete identity is malformed.
pub fn decode_sack(kind: ParentKind, input: &[u8]) -> Result<Sack> {
    let size = if kind == ParentKind::C1 { 20 } else { 14 };
    if input.len() != size {
        return Err(Error::Malformed);
    }
    let mut o = if size == 20 { 8 } else { 4 };
    if be16(input, o) != 0x8000 {
        return Err(Error::Malformed);
    }
    o += 2;
    let original_service = if size == 20 {
        let value = be16(input, o);
        o += 2;
        value
    } else {
        0
    };
    let value = Sack {
        kind,
        transfer_id: be32(input, 0),
        parent_generation: if size == 20 { be32(input, 4) } else { 0 },
        original_service,
        base: be16(input, o),
        bitmap: be32(input, o + 2),
        credit: be16(input, o + 6),
    };
    if value.transfer_id == 0
        || (kind == ParentKind::C1 && (value.parent_generation == 0 || value.original_service == 0))
    {
        return Err(Error::Malformed);
    }
    Ok(value)
}

pub(crate) fn be16(b: &[u8], o: usize) -> u16 {
    u16::from_be_bytes([b[o], b[o + 1]])
}
pub(crate) fn be32(b: &[u8], o: usize) -> u32 {
    u32::from_be_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
pub(crate) fn be64(b: &[u8], o: usize) -> u64 {
    u64::from_be_bytes(b[o..o + 8].try_into().expect("checked eight byte field"))
}
