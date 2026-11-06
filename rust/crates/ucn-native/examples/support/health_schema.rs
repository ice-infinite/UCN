//! Product example only: this is not part of the Native protocol library.
use ucn_types::{Error, Result};
/// Fixed 16-byte read-only health schema.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Health {
    /// Millivolts, unsigned.
    pub voltage_mv: u32,
    /// Milliamps, signed two's complement.
    pub current_ma: i32,
    /// Hundredths of a degree Celsius.
    pub temperature_centi_c: i16,
    /// Product mode, not an Authority state.
    pub mode: u8,
    /// Product flags.
    pub flags: u8,
    /// Product-defined fault bitmap.
    pub fault_bits: u32,
}
impl Health {
    /// Exact big-endian business encoding; never native struct bytes.
    #[must_use]
    pub fn encode(self) -> [u8; 16] {
        let mut b = [0; 16];
        b[..4].copy_from_slice(&self.voltage_mv.to_be_bytes());
        b[4..8].copy_from_slice(&self.current_ma.to_be_bytes());
        b[8..10].copy_from_slice(&self.temperature_centi_c.to_be_bytes());
        b[10] = self.mode;
        b[11] = self.flags;
        b[12..].copy_from_slice(&self.fault_bits.to_be_bytes());
        b
    }
    /// Decode an exact-size business record.
    /// # Errors
    /// Reject every non-16-byte input, without reading it out of bounds.
    pub fn decode(b: &[u8]) -> Result<Self> {
        if b.len() != 16 {
            return Err(Error::Argument);
        }
        Ok(Self {
            voltage_mv: u32::from_be_bytes(b[..4].try_into().map_err(|_| Error::Argument)?),
            current_ma: i32::from_be_bytes(b[4..8].try_into().map_err(|_| Error::Argument)?),
            temperature_centi_c: i16::from_be_bytes([b[8], b[9]]),
            mode: b[10],
            flags: b[11],
            fault_bits: u32::from_be_bytes(b[12..].try_into().map_err(|_| Error::Argument)?),
        })
    }
}
