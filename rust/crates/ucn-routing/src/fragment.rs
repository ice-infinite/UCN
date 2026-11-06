//! Only route-control bodies, not ordinary data headers. 仅路由控制分片，不扩展普通数据头。
use ucn_types::{AddressWidth, C0TransactionId, Error, ProtocolOpcode, Result};
/// Independent authenticated pieces; fixed maximum 50 B chunk.
/// 独立认证的分片，单块固定上限50字节。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RouteFragment {
    /// Original exact Opcode; 原始精确操作码。
    pub opcode: ProtocolOpcode,
    /// Exact 77/99 body bytes; 精确正文长度。
    pub total: u16,
    /// Piece index; 片序号。
    pub index: u8,
    /// Canonical balanced count 2..4; 规范均分片数。
    pub count: u8,
    /// Monotonic per-Session bundle key, not a reused nonce; 单Session单调组键，不复用nonce。
    pub bundle: C0TransactionId,
    /// Number of meaningful bytes; 有效字节数。
    pub length: u8,
    /// Fixed bytes, tail must be zero; 固定内容，尾部必须为零。
    pub chunk: [u8; 50],
}
fn size(op: ProtocolOpcode) -> Result<usize> {
    match op {
        ProtocolOpcode::RouteRrep => Ok(77),
        ProtocolOpcode::RouteRerr => Ok(99),
        _ => Err(Error::Malformed),
    }
}
/// Required fragment count with exact C0/O1/H1 overhead; 精确开销计算的片数。
/// # Errors
/// MTU cannot carry one bounded control or more than four parts rejects.
pub fn route_fragment_count(op: ProtocolOpcode, width: AddressWidth, mtu: usize) -> Result<u8> {
    let total = match op {
        ProtocolOpcode::RouteRreq => 54,
        _ => size(op)?,
    };
    let overhead = 25 + 2 * width.bytes() + 32;
    if total + overhead <= mtu {
        return Ok(1);
    }
    if op == ProtocolOpcode::RouteRreq {
        return Err(Error::NoSpace);
    }
    let capacity = mtu
        .checked_sub(overhead + 14)
        .filter(|v| *v > 0)
        .ok_or(Error::NoSpace)?;
    u8::try_from(total.div_ceil(capacity))
        .ok()
        .filter(|v| (2..=4).contains(v))
        .ok_or(Error::NoSpace)
}
impl RouteFragment {
    /// Exact logical slice; 精确逻辑切片。
    /// # Errors
    /// Unsupported opcode, inconsistent count/index or reserved tail rejects.
    pub fn span(&self) -> Result<(usize, usize)> {
        let total = size(self.opcode)?;
        if usize::from(self.total) != total
            || !(2..=4).contains(&self.count)
            || self.index >= self.count
        {
            return Err(Error::Malformed);
        }
        let piece = total.div_ceil(usize::from(self.count));
        let offset = usize::from(self.index) * piece;
        let length = (total - offset).min(piece);
        if length != usize::from(self.length) || self.chunk[length..].iter().any(|v| *v != 0) {
            return Err(Error::Malformed);
        }
        Ok((offset, length))
    }
    /// Construct one canonical piece; 构造一个规范分片。
    /// # Errors
    /// Wrong whole body or invalid fragment shape rejects.
    pub fn from_body(
        opcode: ProtocolOpcode,
        bundle: C0TransactionId,
        count: u8,
        index: u8,
        body: &[u8],
    ) -> Result<Self> {
        let total = size(opcode)?;
        if body.len() != total || !(2..=4).contains(&count) || index >= count {
            return Err(Error::Malformed);
        }
        let piece = total.div_ceil(usize::from(count));
        let offset = usize::from(index) * piece;
        let length = (total - offset).min(piece);
        let mut chunk = [0; 50];
        chunk[..length].copy_from_slice(&body[offset..offset + length]);
        Ok(Self {
            opcode,
            total: u16::try_from(total).map_err(|_| Error::Malformed)?,
            index,
            count,
            bundle,
            length: u8::try_from(length).map_err(|_| Error::Malformed)?,
            chunk,
        })
    }
    /// Zero-write canonical encoder; 失败零写编码。
    /// # Errors
    /// Invalid shape or insufficient buffer rejects.
    pub fn encode(&self, out: &mut [u8]) -> Result<usize> {
        let (_, length) = self.span()?;
        let n = 14 + length;
        if out.len() < n {
            return Err(Error::NoSpace);
        }
        out[..2].copy_from_slice(&u16::from(self.opcode).to_be_bytes());
        out[2..4].copy_from_slice(&self.total.to_be_bytes());
        out[4] = self.index;
        out[5] = self.count;
        out[6..14].copy_from_slice(&self.bundle.get().to_be_bytes());
        out[14..n].copy_from_slice(&self.chunk[..length]);
        Ok(n)
    }
    /// Strict raw decode, never an authentication claim; 严格结构解码，不表示认证。
    /// # Errors
    /// Length, fields or fragment balance is noncanonical.
    pub fn decode(raw: &[u8]) -> Result<Self> {
        if raw.len() < 15 || raw.len() > 64 {
            return Err(Error::Malformed);
        }
        let mut chunk = [0; 50];
        chunk[..raw.len() - 14].copy_from_slice(&raw[14..]);
        let v = Self {
            opcode: ProtocolOpcode::try_from(u16::from_be_bytes([raw[0], raw[1]]))?,
            total: u16::from_be_bytes([raw[2], raw[3]]),
            index: raw[4],
            count: raw[5],
            bundle: C0TransactionId::new(u64::from_be_bytes(
                raw[6..14].try_into().map_err(|_| Error::Malformed)?,
            ))?,
            length: u8::try_from(raw.len() - 14).map_err(|_| Error::Malformed)?,
            chunk,
        };
        v.span()?;
        Ok(v)
    }
}
