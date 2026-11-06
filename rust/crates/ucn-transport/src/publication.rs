//! Exact Parent publication body, not authentication; 精确父发布正文，不代替认证。
use crate::{ParentKind, TransferParent, encode_parent_record};
use ucn_digest::{DigestWorkspace, canonical_digest};
use ucn_types::{Error, ProtocolOpcode, Result};

/// All Parent control bodies are fixed 32 bytes; 四类正文固定32字节。
pub const PARENT_PUBLICATION_BYTES: usize = 32;

/// Pure phase policy, never authority or authentication proof.
/// 纯阶段规则，不是Authority或认证证明。
/// # Errors
/// Wrong local role/order/submission rejects without a transition.
pub fn parent_phase_transition(
    source: bool,
    phase: u8,
    opcode: ProtocolOpcode,
    disposition: u8,
    sent: u8,
) -> Result<(u8, Option<u8>)> {
    use ProtocolOpcode::{
        TransportParentAbort, TransportParentAccept, TransportParentCommit, TransportParentPrepare,
    };
    if phase > 4
        || sent > 2
        || (source && matches!(phase, 0 | 2))
        || (!source && matches!(phase, 1 | 3))
    {
        return Err(Error::State);
    }
    match (source, phase, opcode, disposition) {
        (false, 0 | 2, TransportParentPrepare, 0) => Ok((2, Some(1))),
        (false, 4, TransportParentPrepare | TransportParentCommit, 0)
        | (false, 2, TransportParentCommit, 0) => Ok((4, Some(2))),
        (true, 1, TransportParentAccept, 1) if sent == 1 => Ok((3, None)),
        (true, 3, TransportParentAccept, 2) if sent == 2 => Ok((4, None)),
        (_, _, TransportParentAbort, 1..=255) => Ok((5, None)),
        _ => Err(Error::State),
    }
}

/// Immutable proposal identity plus disposition; 不变提案身份与阶段。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ParentPublication {
    /// Original PREPARE transaction, not this frame nonce; 原PREPARE事务，非本帧nonce。
    pub transaction: u64,
    /// Durable Parent generation; 耐久父代际。
    pub generation: u32,
    /// Canonical full context digest; 规范完整域摘要。
    pub digest: [u8; 16],
    /// Prepare/Commit=0, Accept=1/2, Abort=reason; 阶段或原因。
    pub disposition: u8,
}
impl ParentPublication {
    /// Validate exact Opcode/disposition combination; 验证精确组合。
    /// # Errors
    /// Reserved identity or impossible stage rejects.
    pub fn validate(self, opcode: ProtocolOpcode) -> Result<()> {
        let legal = match opcode {
            ProtocolOpcode::TransportParentPrepare | ProtocolOpcode::TransportParentCommit => {
                self.disposition == 0
            }
            ProtocolOpcode::TransportParentAccept => matches!(self.disposition, 1 | 2),
            ProtocolOpcode::TransportParentAbort => self.disposition != 0,
            _ => false,
        };
        if self.transaction == 0 || self.generation == 0 || self.digest == [0; 16] || !legal {
            return Err(Error::Argument);
        }
        Ok(())
    }
    /// Encode without any failure writes; 失败不写回。
    /// # Errors
    /// Invalid combination or small output rejects.
    pub fn encode(self, opcode: ProtocolOpcode, output: &mut [u8]) -> Result<usize> {
        self.validate(opcode)?;
        if output.len() < PARENT_PUBLICATION_BYTES {
            return Err(Error::NoSpace);
        }
        let mut bytes = [0; PARENT_PUBLICATION_BYTES];
        bytes[..8].copy_from_slice(&self.transaction.to_be_bytes());
        bytes[8..12].copy_from_slice(&self.generation.to_be_bytes());
        bytes[12..28].copy_from_slice(&self.digest);
        bytes[28] = self.disposition;
        output[..PARENT_PUBLICATION_BYTES].copy_from_slice(&bytes);
        Ok(PARENT_PUBLICATION_BYTES)
    }
    /// Decode independent raw input; 解码独立原字节。
    /// # Errors
    /// Length/reserved/Opcode/stage errors reject.
    pub fn decode(opcode: ProtocolOpcode, bytes: &[u8]) -> Result<Self> {
        if bytes.len() != PARENT_PUBLICATION_BYTES || bytes[29..] != [0; 3] {
            return Err(Error::Malformed);
        }
        let mut digest = [0; 16];
        digest.copy_from_slice(&bytes[12..28]);
        let value = Self {
            transaction: crate::codec::be64(bytes, 0),
            generation: crate::codec::be32(bytes, 8),
            digest,
            disposition: bytes[28],
        };
        value.validate(opcode).map_err(|_| Error::Malformed)?;
        Ok(value)
    }
}

/// Hash canonical Parent identity, excluding allocator progress; 摘要不包含分配进度。
/// # Errors
/// Invalid C1 context or digest failure rejects.
pub fn parent_context_digest(
    parent: TransferParent,
    scratch: &mut DigestWorkspace,
) -> Result<[u8; 16]> {
    if parent.kind != ParentKind::C1 {
        return Err(Error::Argument);
    }
    let record = encode_parent_record(parent, 0, 1)?;
    canonical_digest(b"UCN6-C1-PARENT-V1", &record[..76], scratch)
}
