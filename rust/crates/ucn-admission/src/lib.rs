#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版 Dynamic Admission：认证前 Cookie、固定 JOIN pending 与严格 FSM。
//!
//! 本 crate 只处理 `UNBOUND -> ADMITTED`。已绑定 Peer 的 REAUTH 继续由 Security Owner
//! 负责；Admission 不直接调用 Identity、Security、Persistence 或 Adapter Owner。

mod codec;
mod fragment;
mod owner;

pub use codec::{
    BootstrapEvent, BootstrapFlow, BootstrapPhase, BootstrapTranscript, COOKIE_CHALLENGE_BYTES,
    COOKIE_MAX_BYTES, CookieChallenge, Evidence, HELLO_BYTES, HELLO_COOKIE_FIXED_BYTES, Hello,
    HelloCookie, LOGICAL_MAX_BYTES, MAX_EVIDENCE_BYTES, OPCODE_ABORT, OPCODE_ADDRESS_OFFER,
    OPCODE_DEVICE_COMMIT, OPCODE_FINAL_COMMIT, OPCODE_IDENTITY_CHALLENGE, OPCODE_IDENTITY_RESPONSE,
    TRANSCRIPT_BYTES, decode_cookie_challenge, decode_hello, decode_hello_cookie,
    decode_logical_event, decode_transcript, encode_cookie_challenge, encode_hello,
    encode_hello_cookie, encode_logical_event, encode_transcript,
};
pub use fragment::{
    BootstrapFragment, FRAGMENT_DATA_BYTES, FRAGMENT_HEADER_BYTES, MAX_FRAGMENTS, Reassembly,
    decode_fragment, encode_fragment,
};
pub use owner::{
    AdmissionConfig, AdmissionHandle, AdmissionKey, AdmissionOwner, AdmittedView, CookieProvider,
    FullAdmissionOwner, InitialHelloContext, LinkIdentity, LiteAdmissionOwner, NanoAdmissionOwner,
    PendingView,
};
