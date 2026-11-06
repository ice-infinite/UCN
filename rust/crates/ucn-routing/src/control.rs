//! Per-neighbor authenticated route-control body; outer C0 peers are not the
//! original discovery principal. 逐邻居认证正文，外层邻居不冒充原发现者。
use crate::{
    DiscoveryKey, RerrPayload, RrepMessage, RreqMessage, decode_rerr_payload, decode_rrep_payload,
    decode_rreq_payload, encode_rerr_payload, encode_rrep_payload, encode_rreq_payload,
};
use ucn_security::Binding;
use ucn_types::{
    AddressWidth, BindingGeneration, C0TransactionId, Error, HopLimit, NodeAddress,
    PeerSessionGeneration, ProtocolOpcode, RealmId, Result,
};
/// Explicit causal discovery identity; 显式发现因果身份。
pub const DISCOVERY_KEY_BYTES: usize = 44;
/// Full RREQ body; 完整RREQ正文。
pub const ROUTE_REQUEST_BYTES: usize = 54;
/// Full RREP with reporter-local receipt; 完整RREP及报告方本地回执。
pub const ROUTE_REPLY_BYTES: usize = 77;
/// Fixed route controls, not authority evidence. 固定路由控制，不是Authority证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RouteControl {
    /// Original causal key and bounded scope; 原始因果键及有界范围。
    Request(RreqMessage),
    /// Local downstream generation; target uses zero. 下游本地代际，目标填零。
    Reply(RrepMessage, u32),
    /// Exact reporter namespace; 报告者精确命名空间。
    Error(RerrPayload),
}
impl RouteControl {
    /// Opcode; 精确Opcode。
    #[must_use]
    pub const fn opcode(self) -> ProtocolOpcode {
        match self {
            Self::Request(_) => ProtocolOpcode::RouteRreq,
            Self::Reply(..) => ProtocolOpcode::RouteRrep,
            Self::Error(_) => ProtocolOpcode::RouteRerr,
        }
    }
    /// Exact body size; 精确正文长度。
    #[must_use]
    pub const fn encoded_size(self) -> usize {
        match self {
            Self::Request(_) => 54,
            Self::Reply(..) => 77,
            Self::Error(_) => 99,
        }
    }
    /// Canonical encoder, zero output changes on error. 规范编码，失败不写输出。
    /// # Errors
    /// Invalid field or short output rejects.
    pub fn encode(self, width: AddressWidth, out: &mut [u8]) -> Result<usize> {
        let n = self.encoded_size();
        if out.len() < n {
            return Err(Error::NoSpace);
        }
        let mut raw = [0; 99];
        match self {
            Self::Request(r) => {
                encode_key(r.key, width, &mut raw[..44])?;
                raw[44] = r.remaining_hops.get();
                encode_rreq_payload(r.payload, &mut raw[45..54])?;
            }
            Self::Reply(r, g) => {
                if (r.payload.hop_count == 0) != (g == 0) {
                    return Err(Error::Argument);
                }
                encode_key(r.key, width, &mut raw[..44])?;
                encode_rrep_payload(r.payload, &mut raw[44..73])?;
                raw[73..77].copy_from_slice(&g.to_be_bytes());
            }
            Self::Error(r) => encode_rerr_payload(r, &mut raw)?,
        }
        out[..n].copy_from_slice(&raw[..n]);
        Ok(n)
    }
    /// Strict raw decode, not authentication. 严格结构解码，不表示已认证。
    /// # Errors
    /// Noncanonical length, enum or identity rejects.
    pub fn decode(op: ProtocolOpcode, width: AddressWidth, b: &[u8]) -> Result<Self> {
        (|| match op {
            ProtocolOpcode::RouteRreq if b.len() == 54 => Ok(Self::Request(RreqMessage {
                key: decode_key(width, &b[..44])?,
                remaining_hops: HopLimit::new(b[44])?,
                payload: decode_rreq_payload(&b[45..])?,
            })),
            ProtocolOpcode::RouteRrep if b.len() == 77 => {
                let r = RrepMessage {
                    key: decode_key(width, &b[..44])?,
                    payload: decode_rrep_payload(&b[44..73])?,
                };
                let g = read32(b, 73)?;
                if (r.payload.hop_count == 0) != (g == 0) {
                    return Err(Error::Malformed);
                }
                Ok(Self::Reply(r, g))
            }
            ProtocolOpcode::RouteRerr if b.len() == 99 => Ok(Self::Error(decode_rerr_payload(b)?)),
            _ => Err(Error::Malformed),
        })()
        .map_err(|_| Error::Malformed)
    }
}
fn encode_key(k: DiscoveryKey, w: AddressWidth, b: &mut [u8]) -> Result<()> {
    NodeAddress::new(k.origin.address.get(), w)?;
    NodeAddress::new(k.target_address.get(), w)?;
    if k.origin.principal == [0; 16]
        || k.origin.principal == [255; 16]
        || k.origin.generation.is_unbound()
    {
        return Err(Error::Argument);
    }
    b[..4].copy_from_slice(&k.realm.get().to_be_bytes());
    b[4..8].copy_from_slice(&k.origin.address.get().to_be_bytes());
    b[8..12].copy_from_slice(&k.origin.generation.get().to_be_bytes());
    b[12..28].copy_from_slice(&k.origin.principal);
    b[28..32].copy_from_slice(&k.origin_session_generation.get().to_be_bytes());
    b[32..36].copy_from_slice(&k.target_address.get().to_be_bytes());
    b[36..44].copy_from_slice(&k.transaction_id.get().to_be_bytes());
    Ok(())
}
fn decode_key(w: AddressWidth, b: &[u8]) -> Result<DiscoveryKey> {
    let k = DiscoveryKey {
        realm: RealmId::new(read32(b, 0)?)?,
        origin: Binding {
            address: NodeAddress::new(read32(b, 4)?, w)?,
            generation: BindingGeneration::active(read32(b, 8)?)?,
            principal: b[12..28].try_into().map_err(|_| Error::Malformed)?,
        },
        origin_session_generation: PeerSessionGeneration::new(read32(b, 28)?)?,
        target_address: NodeAddress::new(read32(b, 32)?, w)?,
        transaction_id: C0TransactionId::new(u64::from_be_bytes(
            b[36..44].try_into().map_err(|_| Error::Malformed)?,
        ))?,
    };
    let mut check = [0; 44];
    encode_key(k, w, &mut check)?;
    Ok(k)
}
fn read32(b: &[u8], n: usize) -> Result<u32> {
    Ok(u32::from_be_bytes(
        b[n..n + 4].try_into().map_err(|_| Error::Malformed)?,
    ))
}
