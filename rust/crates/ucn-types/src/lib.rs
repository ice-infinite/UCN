#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的协议可见基础类型。
//!
//! Protocol-visible value types for the UCN v6 simplified implementation.
//!
//! `RUST-00` 仅建立 crate 边界；正式类型必须在 `RUST-01` 中逐项映射冻结合同后加入。

/// UCN v6 的主版本号。
///
/// Major protocol version for UCN v6.
pub const PROTOCOL_MAJOR: u8 = 6;

#[cfg(test)]
mod tests {
    use super::PROTOCOL_MAJOR;

    #[test]
    fn protocol_major_is_v6() {
        assert_eq!(PROTOCOL_MAJOR, 6);
    }
}
