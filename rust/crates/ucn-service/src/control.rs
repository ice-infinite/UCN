//! Shared 28/48/32-byte Operation bodies and conditional application Envelope.
//! `共同Operation控制正文与条件业务Envelope`。
use ucn_digest::{DigestWorkspace, canonical_begin};
use ucn_types::{Error, InteractionRole, Result, operation_flags_valid};

/// Original operation identity carried by C0; `C0承载的原业务Operation身份`。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationWireKey {
    /// Nonzero unique operation; 非零唯一Operation。
    pub operation: u64,
    /// Original service; 原Service。
    pub service: u16,
    /// Original business opcode, not C0 Query opcode; 原业务Opcode。
    pub opcode: u16,
    /// Exact request digest; 精确请求摘要。
    pub request_digest: [u8; 16],
}
impl OperationWireKey {
    /// Validate exact non-reserved key; 校验精确非保留键。
    /// # Errors
    /// Missing identity/digest or reserved service/opcode rejects.
    pub fn validate(self) -> Result<()> {
        if self.operation == 0
            || self.service == 0
            || self.service == u16::MAX
            || self.opcode == u16::MAX
            || self.request_digest == [0; 16]
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}
/// Exact retained result observation; 精确保留结果观察。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationReceipt {
    /// Original complete key; 原完整键。
    pub key: OperationWireKey,
    /// Exact result digest; 精确结果摘要。
    pub result_digest: [u8; 16],
    /// 3=committed,4=no effect,5=in doubt,6=tombstone; 规范耐久观察阶段。
    pub phase: u8,
}

/// Bounded terminal recovery response; 有界终态恢复响应，不签发执行权限。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationResult {
    /// Original identity and authenticated digest; 原身份及认证摘要。
    pub receipt: OperationReceipt,
    /// Exact signed executor status; 精确有符号执行器状态。
    pub status: i32,
    /// Exact result length; 精确结果长度。
    pub length: u16,
    /// Fixed inline result storage, unused tail must be zero; 固定正文，未使用尾部须为零。
    pub payload: [u8; 146],
}
impl OperationResult {
    /// Validate canonical terminal storage; 验证规范终态存储。
    /// # Errors
    /// Nonterminal, oversized, nonzero tail or non-result payload rejects.
    pub fn validate(&self) -> Result<()> {
        self.receipt.key.validate()?;
        let n = usize::from(self.length);
        if !(3..=5).contains(&self.receipt.phase)
            || self.receipt.result_digest == [0; 16]
            || n > self.payload.len()
            || (self.receipt.phase != 3 && n != 0)
            || self.payload[n..].iter().any(|v| *v != 0)
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}
/// Encode terminal recovery: Receipt48 + status4 + length2 + exact payload.
/// 编码终态恢复：48B回执、4B状态、2B长度和精确正文。
/// # Errors
/// Invalid value/short output rejects without writes.
pub fn encode_operation_result(v: OperationResult, out: &mut [u8]) -> Result<usize> {
    v.validate()?;
    let n = usize::from(v.length);
    if out.len() < 54 + n {
        return Err(Error::NoSpace);
    }
    encode_operation_receipt(v.receipt, out)?;
    out[48..52].copy_from_slice(&v.status.to_be_bytes());
    out[52..54].copy_from_slice(&v.length.to_be_bytes());
    out[54..54 + n].copy_from_slice(&v.payload[..n]);
    Ok(54 + n)
}
/// Decode exact bounded terminal recovery; 解码精确有界终态恢复。
/// # Errors
/// Length, phase and reserved-byte violations reject.
pub fn decode_operation_result(b: &[u8]) -> Result<OperationResult> {
    if b.len() < 54 {
        return Err(Error::Malformed);
    }
    let length = u16::from_be_bytes([b[52], b[53]]);
    let n = usize::from(length);
    if n > 146 || b.len() != 54 + n {
        return Err(Error::Malformed);
    }
    let mut v = OperationResult {
        receipt: decode_operation_receipt(&b[..48])?,
        status: i32::from_be_bytes(b[48..52].try_into().map_err(|_| Error::Malformed)?),
        length,
        payload: [0; 146],
    };
    v.payload[..n].copy_from_slice(&b[54..]);
    v.validate().map_err(|_| Error::Malformed)?;
    Ok(v)
}

/// Exact result ACK, never an ACK of `IN_DOUBT` or a bare received frame.
/// 精确结果ACK，不确认`IN_DOUBT`，也不等于裸帧已收到。
/// # Errors
/// Only committed/no-effect receipts may authorize result retention.
pub fn encode_operation_ack(v: OperationReceipt, out: &mut [u8]) -> Result<usize> {
    if !matches!(v.phase, 3 | 4) {
        return Err(Error::Argument);
    }
    encode_operation_receipt(v, out)
}
/// Decode exact result ACK; 解码精确结果ACK。
/// # Errors
/// Nonfinal/uncertain phases reject.
pub fn decode_operation_ack(b: &[u8]) -> Result<OperationReceipt> {
    let v = decode_operation_receipt(b)?;
    if !matches!(v.phase, 3 | 4) {
        return Err(Error::Malformed);
    }
    Ok(v)
}

/// Shared result observation digest, not a Foundation Record digest.
/// 共同结果观察摘要，不是Foundation Record摘要，不含易失Handle。
/// # Errors
/// Invalid key/phase, nonterminal payload, or oversized result rejects.
pub fn operation_result_digest(
    key: OperationWireKey,
    phase: u8,
    status: i32,
    payload: &[u8],
    scratch: &mut DigestWorkspace,
) -> Result<[u8; 16]> {
    key.validate()?;
    if !(3..=6).contains(&phase)
        || payload.len() > u16::MAX as usize
        || (phase != 3 && !payload.is_empty())
    {
        return Err(Error::Argument);
    }
    let mut identity = [0; 28];
    write_key(key, &mut identity);
    let length = u16::try_from(payload.len()).map_err(|_| Error::Argument)?;
    let mut digest = canonical_begin(b"UCN6-OPERATION-RESULT-V1", 35 + u32::from(length), scratch)?;
    digest.append(&identity)?;
    digest.append(&[phase])?;
    digest.append(&status.to_be_bytes())?;
    digest.append(&length.to_be_bytes())?;
    digest.append(payload)?;
    digest.finish()
}
/// Cancel is a request, never proof of no external effects; Cancel仅是请求，不证明外部无副作用。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationCancel {
    /// Original complete key; 原完整键。
    pub key: OperationWireKey,
    /// 1=caller,2=deadline,3=policy,4=shutdown; 规范原因。
    pub reason: u8,
}
/// Conditional application correlation, zero bytes for One-Way.
/// 条件业务关联，One-Way不携带字节。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationEnvelope {
    /// Nonzero ID; 非零ID。
    pub operation: u64,
    /// Shared mode/final bits; 共同模式/终态位。
    pub flags: u8,
    /// Only Result/Error carries code; 仅Result/Error含结果码。
    pub result: u16,
}

fn write_key(v: OperationWireKey, b: &mut [u8]) {
    b[..8].copy_from_slice(&v.operation.to_be_bytes());
    b[8..10].copy_from_slice(&v.service.to_be_bytes());
    b[10..12].copy_from_slice(&v.opcode.to_be_bytes());
    b[12..28].copy_from_slice(&v.request_digest);
}
fn read_key(b: &[u8]) -> Result<OperationWireKey> {
    let v = OperationWireKey {
        operation: u64::from_be_bytes(b[..8].try_into().map_err(|_| Error::Malformed)?),
        service: u16::from_be_bytes([b[8], b[9]]),
        opcode: u16::from_be_bytes([b[10], b[11]]),
        request_digest: b[12..28].try_into().map_err(|_| Error::Malformed)?,
    };
    v.validate().map_err(|_| Error::Malformed)?;
    Ok(v)
}
/// Encode Query identity; 编码Query身份。
/// # Errors
/// Invalid key/short buffer leaves output unchanged.
pub fn encode_operation_key(v: OperationWireKey, out: &mut [u8]) -> Result<usize> {
    v.validate()?;
    if out.len() < 28 {
        return Err(Error::NoSpace);
    }
    write_key(v, &mut out[..28]);
    Ok(28)
}
/// Decode exact Query identity; 解码精确Query身份。
/// # Errors
/// Wrong length/key rejects.
pub fn decode_operation_key(b: &[u8]) -> Result<OperationWireKey> {
    if b.len() != 28 {
        return Err(Error::Malformed);
    }
    read_key(b)
}
/// Encode retained observation; 编码保留观察。
/// # Errors
/// Invalid key/digest/phase or short output has zero writes.
pub fn encode_operation_receipt(v: OperationReceipt, out: &mut [u8]) -> Result<usize> {
    v.key.validate()?;
    if v.result_digest == [0; 16] || !(3..=6).contains(&v.phase) {
        return Err(Error::Argument);
    }
    if out.len() < 48 {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 48];
    write_key(v.key, &mut b);
    b[28..44].copy_from_slice(&v.result_digest);
    b[44] = v.phase;
    out[..48].copy_from_slice(&b);
    Ok(48)
}
/// Decode retained observation; 解码保留观察。
/// # Errors
/// Bad phase/digest/reserved bytes rejects.
pub fn decode_operation_receipt(b: &[u8]) -> Result<OperationReceipt> {
    if b.len() != 48 || !(3..=6).contains(&b[44]) || b[45..48] != [0; 3] || b[28..44] == [0; 16] {
        return Err(Error::Malformed);
    }
    Ok(OperationReceipt {
        key: read_key(b)?,
        result_digest: b[28..44].try_into().map_err(|_| Error::Malformed)?,
        phase: b[44],
    })
}
/// Encode bounded cancel request; 编码有界取消请求。
/// # Errors
/// Invalid key/reason or short output has zero writes.
pub fn encode_operation_cancel(v: OperationCancel, out: &mut [u8]) -> Result<usize> {
    v.key.validate()?;
    if !(1..=4).contains(&v.reason) {
        return Err(Error::Argument);
    }
    if out.len() < 32 {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 32];
    write_key(v.key, &mut b);
    b[28] = v.reason;
    out[..32].copy_from_slice(&b);
    Ok(32)
}
/// Decode cancel, never execute a duplicate operation; 解码取消，不重执行Operation。
/// # Errors
/// Invalid reason/key/reserved bytes rejects.
pub fn decode_operation_cancel(b: &[u8]) -> Result<OperationCancel> {
    if b.len() != 32 || !(1..=4).contains(&b[28]) || b[29..32] != [0; 3] {
        return Err(Error::Malformed);
    }
    Ok(OperationCancel {
        key: read_key(b)?,
        reason: b[28],
    })
}
/// Encode only the conditional correlation prefix; 只编码条件关联前缀。
/// # Errors
/// OneWay/mode mismatch/short output rejects before writing.
pub fn encode_operation_envelope(
    role: InteractionRole,
    v: OperationEnvelope,
    out: &mut [u8],
) -> Result<usize> {
    if role == InteractionRole::OneWay
        || v.operation == 0
        || !operation_flags_valid(role, v.flags)
        || (role == InteractionRole::Request && v.result != 0)
    {
        return Err(Error::Argument);
    }
    let n = if role == InteractionRole::Request {
        9
    } else {
        11
    };
    if out.len() < n {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 11];
    b[..8].copy_from_slice(&v.operation.to_be_bytes());
    b[8] = v.flags;
    if n == 11 {
        b[9..11].copy_from_slice(&v.result.to_be_bytes());
    }
    out[..n].copy_from_slice(&b[..n]);
    Ok(n)
}
/// Decode prefix while retaining business bytes; 解码前缀并保留后续业务字节。
/// # Errors
/// Short/malformed/mode mismatch rejects.
pub fn decode_operation_envelope(
    role: InteractionRole,
    b: &[u8],
) -> Result<(OperationEnvelope, usize)> {
    let n = if role == InteractionRole::Request {
        9
    } else {
        11
    };
    if role == InteractionRole::OneWay || b.len() < n {
        return Err(Error::Malformed);
    }
    let v = OperationEnvelope {
        operation: u64::from_be_bytes(b[..8].try_into().map_err(|_| Error::Malformed)?),
        flags: b[8],
        result: if n == 11 {
            u16::from_be_bytes([b[9], b[10]])
        } else {
            0
        },
    };
    if v.operation == 0 || !operation_flags_valid(role, v.flags) {
        return Err(Error::Malformed);
    }
    Ok((v, n))
}
