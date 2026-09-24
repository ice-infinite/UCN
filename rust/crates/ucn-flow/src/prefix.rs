use ucn_types::{ContextId, Error, ForwardingLabel, HeaderContract, OriginSequence, Result};
use ucn_wire::CommonHeader;

/// C2 固定前缀长度。
pub const C2_PREFIX_BYTES: usize = 9;
/// C3 固定前缀长度。
pub const C3_PREFIX_BYTES: usize = 7;
/// C4 固定前缀长度。
pub const C4_PREFIX_BYTES: usize = 11;

/// C2/C3/C4 稳态 Flow 前缀。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FlowPrefix {
    /// 一跳 Direct Flow。
    C2 {
        /// Common Header。
        header: CommonHeader,
        /// Parent Session 内 Context ID。
        context_id: ContextId,
        /// Flow 内 Origin Sequence。
        sequence: OriginSequence,
    },
    /// 多跳最短 Label Flow。
    C3 {
        /// Common Header。
        header: CommonHeader,
        /// 当前入站 Link 的 Label。
        label: ForwardingLabel,
        /// Hop-local Context ID。
        context_id: ContextId,
    },
    /// 多跳、带 Origin Sequence 的 Label Flow。
    C4 {
        /// Common Header。
        header: CommonHeader,
        /// 当前入站 Link 的 Label。
        label: ForwardingLabel,
        /// Hop-local Context ID。
        context_id: ContextId,
        /// Origin Flow 内不回绕 Sequence。
        sequence: OriginSequence,
    },
}

impl FlowPrefix {
    /// 返回精确 Wire 长度。
    #[must_use]
    pub const fn encoded_size(self) -> usize {
        match self {
            Self::C2 { .. } => C2_PREFIX_BYTES,
            Self::C3 { .. } => C3_PREFIX_BYTES,
            Self::C4 { .. } => C4_PREFIX_BYTES,
        }
    }
}

fn validate(value: FlowPrefix) -> Result<()> {
    use ucn_types::{DeliveryGuarantee, InteractionRole, OriginSecurity, PayloadKind};
    let header = match value {
        FlowPrefix::C2 { header, .. } if header.contract == HeaderContract::C2 => header,
        FlowPrefix::C3 { header, .. }
            if header.contract == HeaderContract::C3
                && header.delivery == DeliveryGuarantee::BestEffort
                && header.interaction == InteractionRole::OneWay
                && matches!(
                    header.payload_kind,
                    PayloadKind::Data | PayloadKind::Diagnostic
                )
                && header.origin_security == OriginSecurity::O0 =>
        {
            header
        }
        FlowPrefix::C4 { header, .. } if header.contract == HeaderContract::C4 => header,
        _ => return Err(Error::Argument),
    };
    if header.interaction != InteractionRole::OneWay && header.payload_kind == PayloadKind::Data {
        return Err(Error::Argument);
    }
    Ok(())
}

/// 编码精确长度的 C2/C3/C4 前缀；失败不写输出。
///
/// # Errors
///
/// Contract 组合非法或输出长度不精确时返回错误。
pub fn encode_flow_prefix(value: FlowPrefix, output: &mut [u8]) -> Result<()> {
    validate(value)?;
    if output.len() != value.encoded_size() {
        return Err(Error::NoSpace);
    }
    let mut bytes = [0_u8; C4_PREFIX_BYTES];
    match value {
        FlowPrefix::C2 {
            header,
            context_id,
            sequence,
        } => {
            header.encode(&mut bytes);
            bytes[3..5].copy_from_slice(&context_id.get().to_be_bytes());
            bytes[5..9].copy_from_slice(&sequence.get().to_be_bytes());
        }
        FlowPrefix::C3 {
            header,
            label,
            context_id,
        } => {
            header.encode(&mut bytes);
            bytes[3..5].copy_from_slice(&label.get().to_be_bytes());
            bytes[5..7].copy_from_slice(&context_id.get().to_be_bytes());
        }
        FlowPrefix::C4 {
            header,
            label,
            context_id,
            sequence,
        } => {
            header.encode(&mut bytes);
            bytes[3..5].copy_from_slice(&label.get().to_be_bytes());
            bytes[5..7].copy_from_slice(&context_id.get().to_be_bytes());
            bytes[7..11].copy_from_slice(&sequence.get().to_be_bytes());
        }
    }
    output.copy_from_slice(&bytes[..output.len()]);
    Ok(())
}

/// 从精确长度输入解码 C2/C3/C4 前缀。
///
/// # Errors
///
/// 长度、Contract 或组合非法时返回格式错误。
pub fn decode_flow_prefix(input: &[u8]) -> Result<FlowPrefix> {
    let header = CommonHeader::decode(input)?;
    let value = match header.contract {
        HeaderContract::C2 if input.len() == C2_PREFIX_BYTES => FlowPrefix::C2 {
            header,
            context_id: ContextId::new(u16::from_be_bytes([input[3], input[4]]))
                .map_err(|_| Error::Malformed)?,
            sequence: OriginSequence::new(u32::from_be_bytes([
                input[5], input[6], input[7], input[8],
            ]))
            .map_err(|_| Error::Malformed)?,
        },
        HeaderContract::C3 if input.len() == C3_PREFIX_BYTES => FlowPrefix::C3 {
            header,
            label: ForwardingLabel::new(u16::from_be_bytes([input[3], input[4]]))
                .map_err(|_| Error::Malformed)?,
            context_id: ContextId::new(u16::from_be_bytes([input[5], input[6]]))
                .map_err(|_| Error::Malformed)?,
        },
        HeaderContract::C4 if input.len() == C4_PREFIX_BYTES => FlowPrefix::C4 {
            header,
            label: ForwardingLabel::new(u16::from_be_bytes([input[3], input[4]]))
                .map_err(|_| Error::Malformed)?,
            context_id: ContextId::new(u16::from_be_bytes([input[5], input[6]]))
                .map_err(|_| Error::Malformed)?,
            sequence: OriginSequence::new(u32::from_be_bytes([
                input[7], input[8], input[9], input[10],
            ]))
            .map_err(|_| Error::Malformed)?,
        },
        _ => return Err(Error::Malformed),
    };
    validate(value).map_err(|_| Error::Malformed)?;
    Ok(value)
}
