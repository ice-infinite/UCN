#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的无分配 Wire Codec。
//!
//! Allocation-free Wire codecs for UCN v6 simplified. `RUST-01` 只开放 C0/C1 的
//! `O0/H0` 原始 Codec；受保护的 `O1/O2/H1` 必须等待 Security Owner 提供经过认证的
//! sealed context，不能由调用方伪造标签后直接编码。

mod c0;
mod c1;
mod common;

pub use c0::{C0Address, C0Frame, c0_o0_h0_encoded_size, decode_c0_o0_h0, encode_c0_o0_h0};
pub use c1::{C1Frame, c1_o0_h0_encoded_size, decode_c1_o0_h0, encode_c1_o0_h0};
pub use common::CommonHeader;

/// 返回本实现所针对的协议主版本。
#[must_use]
pub const fn protocol_major() -> u8 {
    ucn_types::PROTOCOL_MAJOR
}
