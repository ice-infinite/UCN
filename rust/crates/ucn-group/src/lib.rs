#![no_std]
#![forbid(unsafe_code)]
//! Independent bounded Group management. 独立有界Group管理。

#[cfg(feature = "dynamic")]
mod allocator;
#[cfg(feature = "dynamic")]
mod dynamic_group;
#[cfg(feature = "dynamic")]
pub use allocator::*;
mod model;
#[cfg(feature = "dynamic")]
mod record;
#[cfg(feature = "dynamic")]
pub use record::*;
#[cfg(feature = "static")]
mod static_group;
#[cfg(feature = "dynamic")]
pub use dynamic_group::*;
pub use model::*;
#[cfg(feature = "static")]
pub use static_group::*;
