#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的无分配 Wire Codec 边界。
//!
//! Allocation-free Wire codec boundary for UCN v6 simplified.
//!
//! `RUST-00` 不实现任何线上格式。`RUST-01` 必须先导入共享 Golden/Negative Oracle，再增加
//! Encoder 或 Decoder，禁止用 Rust round-trip 自证正确。

/// 返回本实现所针对的协议主版本。
///
/// Returns the major protocol version targeted by this implementation.
#[must_use]
pub const fn protocol_major() -> u8 {
    ucn_types::PROTOCOL_MAJOR
}

#[cfg(test)]
mod tests {
    use super::protocol_major;

    #[test]
    fn wire_targets_v6() {
        assert_eq!(protocol_major(), 6);
    }
}
