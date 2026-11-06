//! Shared C/Rust `TimeKey` and timestamp payload layouts. C/Rust共同时间载荷布局。
use crate::TimeKey;
use ucn_types::{Error, Result};
/// Timestamp message, distinguished by authenticated outer Opcode.
/// 时间戳消息，由认证外层Opcode区分。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimeStampMessage {
    /// Exact transaction; 精确事务。
    pub key: TimeKey,
    /// Physical master time; Master物理时刻。
    pub time: u64,
    /// Positive capture bound; 正捕获界。
    pub bound: u32,
}
/// Encode `SYNC/DELAY_REQ` key; `编码SYNC/DELAY_REQ键`。
/// # Errors
/// Invalid IDs reject before output writes.
pub fn encode_time_key(key: TimeKey, out: &mut [u8; 12]) -> Result<()> {
    if key.domain == 0
        || key.domain > 0xfffe
        || key.generation == 0
        || key.generation > 0x7fff_ffff
        || key.sequence == 0
    {
        return Err(Error::Argument);
    }
    out[..2].copy_from_slice(&key.domain.to_be_bytes());
    out[2..4].fill(0);
    out[4..8].copy_from_slice(&key.generation.to_be_bytes());
    out[8..12].copy_from_slice(&key.sequence.to_be_bytes());
    Ok(())
}
/// Decode exact `SYNC/DELAY_REQ` key; `解码精确SYNC/DELAY_REQ键`。
/// # Errors
/// Wrong length/reserved fields/generation rejects.
pub fn decode_time_key(b: &[u8]) -> Result<TimeKey> {
    if b.len() != 12 || b[2..4] != [0, 0] {
        return Err(Error::Malformed);
    }
    let key = TimeKey {
        domain: u16::from_be_bytes([b[0], b[1]]),
        generation: u32::from_be_bytes(b[4..8].try_into().map_err(|_| Error::Malformed)?),
        sequence: u32::from_be_bytes(b[8..12].try_into().map_err(|_| Error::Malformed)?),
    };
    encode_time_key(key, &mut [0; 12]).map_err(|_| Error::Malformed)?;
    Ok(key)
}
/// Encode `FOLLOW_UP/DELAY_RESP`; `编码FOLLOW_UP/DELAY_RESP`。
/// # Errors
/// Invalid key/time/bound has zero output writes.
pub fn encode_time_stamp(message: TimeStampMessage, out: &mut [u8; 24]) -> Result<()> {
    if message.time == 0 || message.bound == 0 {
        return Err(Error::Argument);
    }
    let mut b = [0; 24];
    let mut key = [0; 12];
    encode_time_key(message.key, &mut key)?;
    b[..12].copy_from_slice(&key);
    b[12..20].copy_from_slice(&message.time.to_be_bytes());
    b[20..24].copy_from_slice(&message.bound.to_be_bytes());
    *out = b;
    Ok(())
}
/// Decode exact `FOLLOW_UP/DELAY_RESP`; `解码精确FOLLOW_UP/DELAY_RESP`。
/// # Errors
/// Wrong length/time/unknown bound rejects.
pub fn decode_time_stamp(b: &[u8]) -> Result<TimeStampMessage> {
    if b.len() != 24 {
        return Err(Error::Malformed);
    }
    let key = decode_time_key(&b[..12])?;
    let time = u64::from_be_bytes(b[12..20].try_into().map_err(|_| Error::Malformed)?);
    let bound = u32::from_be_bytes(b[20..24].try_into().map_err(|_| Error::Malformed)?);
    if time == 0 || bound == 0 {
        return Err(Error::Malformed);
    }
    Ok(TimeStampMessage { key, time, bound })
}
