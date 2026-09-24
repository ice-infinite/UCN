#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的基础自动发现、易失 `SoftRoute` 与精确 RERR。
//!
//! 本 crate 不实现 Label Flow、原子路径激活或 Authority。RREP 可以在返回途中逐跳安装
//! SoftRoute；这些本地提示只能用于 C1，普通业务流量不会延长其租期。

mod codec;
mod owner;

pub use codec::{
    RERR_PAYLOAD_BYTES, RREP_PAYLOAD_BYTES, RREQ_PAYLOAD_BYTES, RerrPayload, RerrReason,
    RrepPayload, RreqPayload, decode_rerr_payload, decode_rrep_payload, decode_rreq_payload,
    encode_rerr_payload, encode_rrep_payload, encode_rreq_payload,
};
pub use owner::{
    DiscoveryHandle, DiscoveryKey, DiscoveryStart, FullRouteOwner, LinkRef, LiteRouteOwner,
    NanoRouteOwner, ReplyAction, ReplyForwardHandle, RequestAction, ResolvedRoute, RouteConfig,
    RouteDomain, RouteOwner, RouteUseFacts, RrepMessage, RreqMessage, SoftRouteView, StaticRoute,
};
