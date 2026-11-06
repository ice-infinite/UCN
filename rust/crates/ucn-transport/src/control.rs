//! Complete immutable C0 Transfer responses; 完整不可变C0 Transfer响应。
use crate::codec::{be16, be32};
use crate::{ParentKind, TransferSetup};
use ucn_types::{Error, Result};

/// Frozen Setup identity; 冻结Setup身份。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferKey {
    /// Parent kind; 父类型。
    pub kind: ParentKind,
    /// C1=0, Flow nonzero; 父短ID。
    pub parent_id: u16,
    /// Exact parent generation; 精确父代际。
    pub parent_generation: u32,
    /// Monotonic Transfer ID; 单调传输身份。
    pub transfer_id: u32,
    /// Original Service; 原Service。
    pub service: u16,
    /// Canonical complete Setup digest; 规范完整Setup摘要。
    pub digest: [u8; 16],
}
impl TransferKey {
    /// Validate the complete identity, not just Transfer ID; 验证完整身份。
    /// # Errors
    /// Reserved identity, missing generation or digest rejects.
    pub fn validate(self) -> Result<()> {
        if self.parent_generation == 0
            || self.transfer_id == 0
            || self.service == 0
            || self.service == u16::MAX
            || self.digest == [0; 16]
            || (self.kind == ParentKind::C1 && self.parent_id != 0)
            || (self.kind == ParentKind::Flow && self.parent_id == 0)
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
    /// Derive one canonical full Setup identity; 推导规范完整Setup身份。
    /// # Errors
    /// Invalid Setup or digest arithmetic rejects.
    pub fn from_setup(
        setup: TransferSetup,
        scratch: &mut ucn_digest::DigestWorkspace,
    ) -> Result<Self> {
        let mut bytes = [0; 54];
        let n = crate::encode_setup(setup, &mut bytes)?;
        Ok(Self {
            kind: setup.kind,
            parent_id: setup.parent_id,
            parent_generation: setup.parent_generation,
            transfer_id: setup.transfer_id,
            service: setup.service,
            digest: ucn_digest::canonical_digest(b"UCN6-TRANSFER-SETUP-V1", &bytes[..n], scratch)?,
        })
    }
}
/// Exact frozen receiver Setup acceptance; 精确冻结接收方Setup接纳。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SetupAck {
    /// Exact Setup; 精确Setup。
    pub key: TransferKey,
    /// Accepted immutable fragment budget; 接纳的不变分片预算。
    pub fragment_bytes: u16,
    /// Receiver credit; 接收credit。
    pub credit: u16,
}
/// Exact Transfer Abort; 精确传输取消。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferAbort {
    /// Complete key; 完整键。
    pub key: TransferKey,
    /// Nonzero reason; 非零原因。
    pub reason: u8,
}
/// Exact retained final target receipt; 精确保留的最终目标凭据。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TransferTerminal {
    /// Complete key; 完整键。
    pub key: TransferKey,
    /// Canonical terminal state, 1=delivered/2=aborted/3=failed; 规范终态。
    pub state: u8,
    /// Result code; 结果码。
    pub result: u16,
}

fn write_key(k: TransferKey, b: &mut [u8]) {
    b[0] = k.kind as u8;
    b[1..3].copy_from_slice(&k.parent_id.to_be_bytes());
    b[3..7].copy_from_slice(&k.parent_generation.to_be_bytes());
    b[7..11].copy_from_slice(&k.transfer_id.to_be_bytes());
    b[11..13].copy_from_slice(&k.service.to_be_bytes());
    b[13..29].copy_from_slice(&k.digest);
}
fn read_key(b: &[u8]) -> Result<TransferKey> {
    if b.len() < 29 {
        return Err(Error::Malformed);
    }
    let kind = match b[0] {
        1 => ParentKind::C1,
        2 => ParentKind::Flow,
        _ => return Err(Error::Malformed),
    };
    let k = TransferKey {
        kind,
        parent_id: be16(b, 1),
        parent_generation: be32(b, 3),
        transfer_id: be32(b, 7),
        service: be16(b, 11),
        digest: b[13..29].try_into().map_err(|_| Error::Malformed)?,
    };
    k.validate().map_err(|_| Error::Malformed)?;
    Ok(k)
}
/// Encode exact Setup ACK after preflight; 预检后编码精确建立确认。
/// # Errors
/// Invalid fields or short output has zero writes.
pub fn encode_setup_ack(v: SetupAck, out: &mut [u8]) -> Result<usize> {
    v.key.validate()?;
    if v.fragment_bytes == 0 {
        return Err(Error::Argument);
    }
    if out.len() < 33 {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 33];
    write_key(v.key, &mut b);
    b[29..31].copy_from_slice(&v.fragment_bytes.to_be_bytes());
    b[31..33].copy_from_slice(&v.credit.to_be_bytes());
    out[..33].copy_from_slice(&b);
    Ok(33)
}
/// Decode exact Setup ACK; 解码精确建立确认。
/// # Errors
/// Wrong length, identity or zero budget rejects.
pub fn decode_setup_ack(b: &[u8]) -> Result<SetupAck> {
    if b.len() != 33 || be16(b, 29) == 0 {
        return Err(Error::Malformed);
    }
    Ok(SetupAck {
        key: read_key(b)?,
        fragment_bytes: be16(b, 29),
        credit: be16(b, 31),
    })
}
/// Encode exact Abort with reserved zeros; 编码精确Abort及保留零位。
/// # Errors
/// Invalid key/reason or short output rejects before writing.
pub fn encode_abort(v: TransferAbort, out: &mut [u8]) -> Result<usize> {
    v.key.validate()?;
    if v.reason == 0 {
        return Err(Error::Argument);
    }
    if out.len() < 33 {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 33];
    write_key(v.key, &mut b);
    b[29] = v.reason;
    out[..33].copy_from_slice(&b);
    Ok(33)
}
/// Decode exact Abort; 解码精确Abort。
/// # Errors
/// Missing reason or nonzero reserved bytes rejects.
pub fn decode_abort(b: &[u8]) -> Result<TransferAbort> {
    if b.len() != 33 || b[29] == 0 || b[30..33] != [0; 3] {
        return Err(Error::Malformed);
    }
    Ok(TransferAbort {
        key: read_key(b)?,
        reason: b[29],
    })
}
/// Encode exact terminal result; 编码精确终态结果。
/// # Errors
/// Unknown state or short output rejects before writing.
pub fn encode_terminal(v: TransferTerminal, out: &mut [u8]) -> Result<usize> {
    v.key.validate()?;
    if !(1..=3).contains(&v.state) {
        return Err(Error::Argument);
    }
    if out.len() < 32 {
        return Err(Error::NoSpace);
    }
    let mut b = [0; 32];
    write_key(v.key, &mut b);
    b[29] = v.state;
    b[30..32].copy_from_slice(&v.result.to_be_bytes());
    out[..32].copy_from_slice(&b);
    Ok(32)
}
/// Decode exact terminal result; 解码精确终态结果。
/// # Errors
/// Wrong length/state/key rejects.
pub fn decode_terminal(b: &[u8]) -> Result<TransferTerminal> {
    if b.len() != 32 || !(1..=3).contains(&b[29]) {
        return Err(Error::Malformed);
    }
    Ok(TransferTerminal {
        key: read_key(b)?,
        state: b[29],
        result: be16(b, 30),
    })
}
