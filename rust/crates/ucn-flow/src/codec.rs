use ucn_types::{CandidateId, Error, ForwardingLabel, Result, RouteGeneration};

/// Stage/Commit 共用的 Label Setup Payload 固定为 16 B。
pub const LABEL_SETUP_BYTES: usize = 16;

/// 线上 Label Setup 的六字段强类型对象。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct LabelSetup {
    /// Candidate ID；线上只使用低 16-bit 非保留域。
    pub candidate_id: CandidateId,
    /// Route Generation。
    pub route_generation: RouteGeneration,
    /// 反向 Label。
    pub reverse_label: ForwardingLabel,
    /// 正向 Label。
    pub forward_label: ForwardingLabel,
    /// Manifest Path Profile ID。
    pub path_profile_id: u16,
    /// 完整 Proposal Digest 的 32-bit 查找前缀；不能代替精确比较。
    pub context_digest: u32,
}

fn validate(value: LabelSetup) -> Result<()> {
    if value.candidate_id.get() >= u32::from(u16::MAX)
        || value.path_profile_id == 0
        || value.path_profile_id == u16::MAX
        || value.context_digest == 0
    {
        return Err(Error::Argument);
    }
    Ok(())
}

/// 以 big-endian 编码固定 16 B Label Setup；失败不写输出。
///
/// # Errors
///
/// 字段非法或输出长度不精确时返回错误。
pub fn encode_label_setup(value: LabelSetup, output: &mut [u8]) -> Result<()> {
    validate(value)?;
    if output.len() != LABEL_SETUP_BYTES {
        return Err(Error::NoSpace);
    }
    let mut bytes = [0_u8; LABEL_SETUP_BYTES];
    let candidate_id = u16::try_from(value.candidate_id.get()).map_err(|_| Error::Argument)?;
    bytes[0..2].copy_from_slice(&candidate_id.to_be_bytes());
    bytes[2..6].copy_from_slice(&value.route_generation.get().to_be_bytes());
    bytes[6..8].copy_from_slice(&value.reverse_label.get().to_be_bytes());
    bytes[8..10].copy_from_slice(&value.forward_label.get().to_be_bytes());
    bytes[10..12].copy_from_slice(&value.path_profile_id.to_be_bytes());
    bytes[12..16].copy_from_slice(&value.context_digest.to_be_bytes());
    output.copy_from_slice(&bytes);
    Ok(())
}

/// 从精确 16 B 输入解码 Label Setup。
///
/// # Errors
///
/// 长度或任一字段非法时返回格式错误。
pub fn decode_label_setup(input: &[u8]) -> Result<LabelSetup> {
    if input.len() != LABEL_SETUP_BYTES {
        return Err(Error::Malformed);
    }
    let value = LabelSetup {
        candidate_id: CandidateId::new(u32::from(u16::from_be_bytes([input[0], input[1]])))
            .map_err(|_| Error::Malformed)?,
        route_generation: RouteGeneration::new(u32::from_be_bytes([
            input[2], input[3], input[4], input[5],
        ]))
        .map_err(|_| Error::Malformed)?,
        reverse_label: ForwardingLabel::new(u16::from_be_bytes([input[6], input[7]]))
            .map_err(|_| Error::Malformed)?,
        forward_label: ForwardingLabel::new(u16::from_be_bytes([input[8], input[9]]))
            .map_err(|_| Error::Malformed)?,
        path_profile_id: u16::from_be_bytes([input[10], input[11]]),
        context_digest: u32::from_be_bytes([input[12], input[13], input[14], input[15]]),
    };
    validate(value).map_err(|_| Error::Malformed)?;
    Ok(value)
}
