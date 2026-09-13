#![no_std]
#![forbid(unsafe_code)]

//! UCN v6 简化版的协议可见基础类型。
//!
//! Protocol-visible scalar types and registries for UCN v6 simplified.
//! 所有构造函数都在产生线上值之前执行合法域检查；最大值本身合法，但单调分配器不得回绕。

use core::num::{NonZeroU16, NonZeroU32, NonZeroU64};

/// UCN v6 的主版本号。
pub const PROTOCOL_MAJOR: u8 = 6;

/// Rust 实现内部使用的、不会泄漏到 Wire ABI 的错误分类。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    /// 调用参数或强类型构造参数非法。
    Argument,
    /// 输入字节不符合冻结 Wire 合同。
    Malformed,
    /// 输入合法但当前实施阶段尚未提供该能力。
    Unsupported,
    /// 调用方提供的固定缓冲区不足。
    NoSpace,
    /// 不回绕计数器已经到达合法终值。
    Exhausted,
}

/// UCN 结果别名。
pub type Result<T> = core::result::Result<T, Error>;

macro_rules! nonzero_u32_scalar {
    ($(#[$meta:meta])* $name:ident) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, Eq, PartialEq)]
        pub struct $name(NonZeroU32);

        impl $name {
            /// 从非零线上值构造该强类型。
            ///
            /// # Errors
            ///
            /// 值为 0 时返回 [`Error::Argument`]。
            pub const fn new(value: u32) -> Result<Self> {
                match NonZeroU32::new(value) {
                    Some(value) => Ok(Self(value)),
                    None => Err(Error::Argument),
                }
            }

            /// 返回线上整数值。
            #[must_use]
            pub const fn get(self) -> u32 {
                self.0.get()
            }

            /// 计算不回绕的后继值。
            ///
            /// # Errors
            ///
            /// 当前值已经是 `u32::MAX` 时返回 [`Error::Exhausted`]。
            pub const fn checked_next(self) -> Result<Self> {
                match self.get().checked_add(1) {
                    Some(value) => Self::new(value),
                    None => Err(Error::Exhausted),
                }
            }
        }
    };
}

macro_rules! bounded_u16_scalar {
    ($(#[$meta:meta])* $name:ident) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, Eq, PartialEq)]
        pub struct $name(NonZeroU16);

        impl $name {
            /// 从 `1..=0xFFFE` 构造该强类型。
            ///
            /// # Errors
            ///
            /// 值为 0 或 `u16::MAX` 时返回 [`Error::Argument`]。
            pub const fn new(value: u16) -> Result<Self> {
                if value == u16::MAX {
                    return Err(Error::Argument);
                }
                match NonZeroU16::new(value) {
                    Some(value) => Ok(Self(value)),
                    None => Err(Error::Argument),
                }
            }

            /// 返回线上整数值。
            #[must_use]
            pub const fn get(self) -> u16 {
                self.0.get()
            }
        }
    };
}

macro_rules! nonzero_u64_scalar {
    ($(#[$meta:meta])* $name:ident) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, Eq, PartialEq)]
        pub struct $name(NonZeroU64);

        impl $name {
            /// 从非零线上值构造该强类型。
            ///
            /// # Errors
            ///
            /// 值为 0 时返回 [`Error::Argument`]。
            pub const fn new(value: u64) -> Result<Self> {
                match NonZeroU64::new(value) {
                    Some(value) => Ok(Self(value)),
                    None => Err(Error::Argument),
                }
            }

            /// 返回线上整数值。
            #[must_use]
            pub const fn get(self) -> u64 {
                self.0.get()
            }

            /// 计算不回绕的后继值。
            ///
            /// # Errors
            ///
            /// 当前值已经是 `u64::MAX` 时返回 [`Error::Exhausted`]。
            pub const fn checked_next(self) -> Result<Self> {
                match self.get().checked_add(1) {
                    Some(value) => Self::new(value),
                    None => Err(Error::Exhausted),
                }
            }
        }
    };
}

macro_rules! wire_enum_u8 {
    ($(#[$meta:meta])* $name:ident {
        $($(#[$variant_meta:meta])* $variant:ident = $value:expr),+ $(,)?
    }) => {
        $(#[$meta])*
        #[repr(u8)]
        #[derive(Clone, Copy, Debug, Eq, PartialEq)]
        pub enum $name {
            $($(#[$variant_meta])* $variant = $value),+
        }

        impl TryFrom<u8> for $name {
            type Error = Error;

            fn try_from(value: u8) -> Result<Self> {
                match value {
                    $($value => Ok(Self::$variant),)+
                    _ => Err(Error::Malformed),
                }
            }
        }

        impl From<$name> for u8 {
            fn from(value: $name) -> Self {
                value as Self
            }
        }

        impl $name {
            /// Registry 中全部已登记值，按线上数值升序排列。
            pub const ALL: &'static [Self] = &[$(Self::$variant),+];
        }
    };
}

wire_enum_u8! {
    /// Core Header Contract。
    HeaderContract {
        /// C0 ABSOLUTE 控制帧。
        C0 = 0,
        /// C1 STATELESS 单帧。
        C1 = 1,
        /// C2 DIRECT 固定路径帧。
        C2 = 2,
        /// C3 LABEL 转发帧。
        C3 = 3,
        /// C4 ROUTED 动态路由帧。
        C4 = 4,
        /// C5 GROUP 组帧。
        C5 = 5,
    }
}

wire_enum_u8! {
    /// 四级业务流量类别。
    TrafficClass {
        /// 最高优先级控制与安全流量。
        Q0 = 0,
        /// 高频最新值或低延时业务流量。
        Q1 = 1,
        /// 常规可靠业务流量。
        Q2 = 2,
        /// 后台、分片或大吞吐流量。
        Q3 = 3,
    }
}

wire_enum_u8! {
    /// 交付保证维度。
    DeliveryGuarantee {
        /// 尽力而为。
        BestEffort = 0,
        /// 只保留最新值。
        Latest = 1,
        /// 可靠交付。
        Reliable = 2,
    }
}

wire_enum_u8! {
    /// 交互角色维度。
    InteractionRole {
        /// 单向消息。
        OneWay = 0,
        /// 请求消息。
        Request = 1,
        /// 成功结果。
        Result = 2,
        /// 错误结果。
        Error = 3,
    }
}

wire_enum_u8! {
    /// Payload 的协议语义类别。
    PayloadKind {
        /// 普通业务数据。
        Data = 0,
        /// 有状态或有副作用的控制消息。
        Control = 1,
        /// 受 Transport Parent 约束的传输片段。
        Transfer = 2,
        /// 只读诊断消息。
        Diagnostic = 3,
    }
}

wire_enum_u8! {
    /// 端到端 Origin 保护等级。
    OriginSecurity {
        /// 无 Origin Tag。
        O0 = 0,
        /// 认证但不加密。
        O1 = 1,
        /// 认证并加密。
        O2 = 2,
    }
}

wire_enum_u8! {
    /// 由已认证上下文选择、不会自声明到 Core Header 的逐跳保护等级。
    HopProfile {
        /// 无逐跳 Tag。
        H0 = 0,
        /// Peer Hop Tag。
        H1 = 1,
        /// Direct Context。
        H2 = 2,
        /// Group Hop Tag。
        H3 = 3,
    }
}

/// Realm 固定的地址字节宽度。
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AddressWidth {
    /// 8-bit 地址。
    A0 = 1,
    /// 16-bit 地址。
    A1 = 2,
    /// 24-bit 地址。
    A2 = 3,
    /// 32-bit 地址。
    A3 = 4,
}

impl AddressWidth {
    /// 返回线上地址占用的字节数。
    #[must_use]
    pub const fn bytes(self) -> usize {
        self as usize
    }

    /// 返回该宽度保留的全 1 地址。
    #[must_use]
    pub const fn reserved_max(self) -> u32 {
        match self {
            Self::A0 => 0xFF,
            Self::A1 => 0xFFFF,
            Self::A2 => 0x00FF_FFFF,
            Self::A3 => u32::MAX,
        }
    }
}

impl TryFrom<u8> for AddressWidth {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            1 => Ok(Self::A0),
            2 => Ok(Self::A1),
            3 => Ok(Self::A2),
            4 => Ok(Self::A3),
            _ => Err(Error::Argument),
        }
    }
}

/// 1..=63 的逐帧剩余跳数。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HopLimit(u8);

impl HopLimit {
    /// 构造 Hop Limit。
    ///
    /// # Errors
    ///
    /// 值为 0 或大于 63 时返回 [`Error::Argument`]。
    pub const fn new(value: u8) -> Result<Self> {
        if value == 0 || value > 63 {
            return Err(Error::Argument);
        }
        Ok(Self(value))
    }

    /// 返回 6-bit 线上值。
    #[must_use]
    pub const fn get(self) -> u8 {
        self.0
    }
}

/// 普通、已绑定节点地址；0 和该宽度全 1 无法构造。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NodeAddress(u32);

impl NodeAddress {
    /// 在指定 Realm 地址宽度下构造普通节点地址。
    ///
    /// # Errors
    ///
    /// 值为 0、该宽度全 1 或超出该宽度时返回 [`Error::Argument`]。
    pub const fn new(value: u32, width: AddressWidth) -> Result<Self> {
        if value == 0 || value >= width.reserved_max() {
            return Err(Error::Argument);
        }
        Ok(Self(value))
    }

    /// 返回线上整数值。
    #[must_use]
    pub const fn get(self) -> u32 {
        self.0
    }
}

/// 非零、非全 1 的 32-bit Realm ID。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RealmId(NonZeroU32);

impl RealmId {
    /// 构造 Realm ID。
    ///
    /// # Errors
    ///
    /// 值为 0 或 `u32::MAX` 时返回 [`Error::Argument`]。
    pub const fn new(value: u32) -> Result<Self> {
        if value == u32::MAX {
            return Err(Error::Argument);
        }
        match NonZeroU32::new(value) {
            Some(value) => Ok(Self(value)),
            None => Err(Error::Argument),
        }
    }

    /// 返回线上整数值。
    #[must_use]
    pub const fn get(self) -> u32 {
        self.0.get()
    }
}

/// Address Binding Generation；0 仅表示未绑定 Bootstrap。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BindingGeneration(u32);

impl BindingGeneration {
    /// 未绑定 Bootstrap 的固定零值。
    pub const UNBOUND: Self = Self(0);

    /// 构造已经激活的非零 Binding Generation。
    ///
    /// # Errors
    ///
    /// 值为 0 时返回 [`Error::Argument`]。
    pub const fn active(value: u32) -> Result<Self> {
        if value == 0 {
            return Err(Error::Argument);
        }
        Ok(Self(value))
    }

    /// 返回线上整数值。
    #[must_use]
    pub const fn get(self) -> u32 {
        self.0
    }

    /// 判断它是否属于未绑定 Bootstrap。
    #[must_use]
    pub const fn is_unbound(self) -> bool {
        self.0 == 0
    }

    /// 计算已激活 Binding 的不回绕后继值。
    ///
    /// `UNBOUND(0)` 不能靠本函数自行变成 1；首次分配必须由持久化证明后的 Address Authority
    /// 完成。
    ///
    /// # Errors
    ///
    /// 未绑定状态返回 [`Error::Argument`]；合法终值 `u32::MAX` 返回 [`Error::Exhausted`]。
    pub const fn checked_next(self) -> Result<Self> {
        if self.is_unbound() {
            return Err(Error::Argument);
        }
        match self.get().checked_add(1) {
            Some(value) => Self::active(value),
            None => Err(Error::Exhausted),
        }
    }
}

/// C0 控制事务 ID；0 无法构造，`u64::MAX` 仍是合法终值。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct C0TransactionId(NonZeroU64);

impl C0TransactionId {
    /// 构造 C0 Transaction ID。
    ///
    /// # Errors
    ///
    /// 值为 0 时返回 [`Error::Argument`]。
    pub const fn new(value: u64) -> Result<Self> {
        match NonZeroU64::new(value) {
            Some(value) => Ok(Self(value)),
            None => Err(Error::Argument),
        }
    }

    /// 返回线上整数值。
    #[must_use]
    pub const fn get(self) -> u64 {
        self.0.get()
    }

    /// 计算不回绕的后继值。
    ///
    /// # Errors
    ///
    /// 当前值已经是 `u64::MAX` 时返回 [`Error::Exhausted`]。
    pub const fn checked_next(self) -> Result<Self> {
        match self.get().checked_add(1) {
            Some(value) => Self::new(value),
            None => Err(Error::Exhausted),
        }
    }
}

/// C1 Origin Sequence；0 无法构造，`u32::MAX` 仍是合法终值。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OriginSequence(NonZeroU32);

impl OriginSequence {
    /// 构造 Origin Sequence。
    ///
    /// # Errors
    ///
    /// 值为 0 时返回 [`Error::Argument`]。
    pub const fn new(value: u32) -> Result<Self> {
        match NonZeroU32::new(value) {
            Some(value) => Ok(Self(value)),
            None => Err(Error::Argument),
        }
    }

    /// 返回线上整数值。
    #[must_use]
    pub const fn get(self) -> u32 {
        self.0.get()
    }

    /// 计算不回绕的后继值。
    ///
    /// # Errors
    ///
    /// 当前值已经是 `u32::MAX` 时返回 [`Error::Exhausted`]。
    pub const fn checked_next(self) -> Result<Self> {
        match self.get().checked_add(1) {
            Some(value) => Self::new(value),
            None => Err(Error::Exhausted),
        }
    }
}

/// C1 Service ID；0 和 `u16::MAX` 无法构造。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ServiceId(NonZeroU16);

impl ServiceId {
    /// 构造 Service ID。
    ///
    /// # Errors
    ///
    /// 值为 0 或 `u16::MAX` 时返回 [`Error::Argument`]。
    pub const fn new(value: u16) -> Result<Self> {
        if value == u16::MAX {
            return Err(Error::Argument);
        }
        match NonZeroU16::new(value) {
            Some(value) => Ok(Self(value)),
            None => Err(Error::Argument),
        }
    }

    /// 返回线上整数值。
    #[must_use]
    pub const fn get(self) -> u16 {
        self.0.get()
    }
}

/// Operation Allocator 分配的 64-bit Operation ID。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct OperationId(NonZeroU64);

impl OperationId {
    /// 构造 Operation ID。
    ///
    /// # Errors
    ///
    /// 值为 0 时返回 [`Error::Argument`]。
    pub const fn new(value: u64) -> Result<Self> {
        match NonZeroU64::new(value) {
            Some(value) => Ok(Self(value)),
            None => Err(Error::Argument),
        }
    }

    /// 返回线上整数值。
    #[must_use]
    pub const fn get(self) -> u64 {
        self.0.get()
    }

    /// 计算不回绕的后继值。
    ///
    /// # Errors
    ///
    /// 当前值已经是 `u64::MAX` 时返回 [`Error::Exhausted`]。
    pub const fn checked_next(self) -> Result<Self> {
        match self.get().checked_add(1) {
            Some(value) => Self::new(value),
            None => Err(Error::Exhausted),
        }
    }
}

nonzero_u32_scalar! { /// Peer/Group Hop Security Owner 分配的 Hop Sequence。
HopSequence }
nonzero_u32_scalar! { /// Transport Parent/Flow Owner 分配的 Transfer ID。
TransferId }
nonzero_u32_scalar! { /// Route/Flow Candidate Owner 分配的 Candidate ID。
CandidateId }
nonzero_u32_scalar! { /// Path Installer 分配的 Path ID。
PathId }
nonzero_u32_scalar! { /// Platform/Runtime 分配的 Boot Incarnation。
BootIncarnation }
nonzero_u32_scalar! { /// Link Owner 分配的 Link Instance Generation。
LinkInstanceGeneration }
nonzero_u32_scalar! { /// Realm Authority 分配的 Address Authority Generation。
AddressAuthorityGeneration }
nonzero_u32_scalar! { /// Security Owner 分配的 Peer Session Generation。
PeerSessionGeneration }
nonzero_u32_scalar! { /// Capability Owner 分配的 Capability Generation。
CapabilityGeneration }
nonzero_u32_scalar! { /// Traffic Origin Route Owner 分配的 Route Generation。
RouteGeneration }
nonzero_u32_scalar! { /// Path Installer 分配的 Path Generation。
PathGeneration }
nonzero_u32_scalar! { /// Flow Owner 分配的 Flow Generation。
FlowGeneration }
nonzero_u32_scalar! { /// Transport Parent Owner 分配的 Transport Parent Generation。
TransportParentGeneration }
nonzero_u32_scalar! { /// Group Policy Owner 分配的 Group Policy Generation。
GroupPolicyGeneration }
nonzero_u32_scalar! { /// Group Key Rotation Owner 分配的 Group Key Generation。
GroupKeyGeneration }
nonzero_u32_scalar! { /// Group Tree Owner 分配的 Group Tree Generation。
GroupTreeGeneration }
nonzero_u32_scalar! { /// Cluster Authority 分配的 Cluster Config Generation。
ClusterConfigGeneration }
nonzero_u32_scalar! { /// Cluster Authority/Quorum 分配的 Cluster Epoch。
ClusterEpoch }
nonzero_u32_scalar! { /// Time Authority 分配的 Network Time Domain Generation。
TimeDomainGeneration }
nonzero_u32_scalar! { /// Product build authority 固定的 Manifest/Layout Generation。
ManifestLayoutGeneration }

nonzero_u64_scalar! { /// Persistence Owner 分配的 64-bit Record Generation。
PersistenceRecordGeneration }

bounded_u16_scalar! { /// Flow/Group Context Owner 分配的 Context ID。
ContextId }
bounded_u16_scalar! { /// 当前入站 Link 的 Label Owner 分配的 Forwarding Label。
ForwardingLabel }
bounded_u16_scalar! { /// Group Policy Owner 分配的 Sender Slot。
SenderSlot }
bounded_u16_scalar! { /// Security/Group Manifest 固定的 Key ID。
KeyId }

/// Manifest 固定或动态高水位分配的 Group ID。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GroupId(NonZeroU32);

impl GroupId {
    /// 从 `1..=0xFFFF_FFFE` 构造 Group ID。
    ///
    /// # Errors
    ///
    /// 值为 0 或 `u32::MAX` 时返回 [`Error::Argument`]。
    pub const fn new(value: u32) -> Result<Self> {
        if value == u32::MAX {
            return Err(Error::Argument);
        }
        match NonZeroU32::new(value) {
            Some(value) => Ok(Self(value)),
            None => Err(Error::Argument),
        }
    }

    /// 返回线上整数值。
    #[must_use]
    pub const fn get(self) -> u32 {
        self.0.get()
    }

    /// 计算不进入保留全 1 值的后继 ID。
    ///
    /// # Errors
    ///
    /// 当前值已经是 `0xFFFF_FFFE` 时返回 [`Error::Exhausted`]。
    pub const fn checked_next(self) -> Result<Self> {
        match self.get().checked_add(1) {
            Some(value) if value != u32::MAX => Self::new(value),
            _ => Err(Error::Exhausted),
        }
    }
}

wire_enum_u8! {
    /// V6S-00-03 冻结的 Security Suite Registry。
    SuiteId {
        /// O1 HMAC-SHA-256-128 Auth-only。
        OriginHmacSha256_128 = 1,
        /// O2 AES-128-GCM。
        OriginAes128Gcm = 2,
        /// O2 ChaCha20-Poly1305。
        OriginChaCha20Poly1305 = 3,
        /// H1/H3 HMAC-SHA-256-96。
        HopHmacSha256_96 = 16,
    }
}

/// 标准 Opcode 所属的 Registry 域。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum OpcodeDomain {
    /// Identity、Admission 与 Security。
    IdentitySecurity,
    /// Capability 与 Neighbor。
    CapabilityNeighbor,
    /// Route。
    Route,
    /// Flow 与 Path。
    FlowPath,
    /// Transport。
    Transport,
    /// Service 与 Operation。
    ServiceOperation,
    /// Time 与 Realtime。
    Time,
    /// Group。
    Group,
    /// Cluster。
    Cluster,
    /// 只读 Diagnostics。
    Diagnostics,
}

macro_rules! protocol_opcodes {
    ($($variant:ident = $value:expr),+ $(,)?) => {
        /// V6S-00-03 冻结的标准 Protocol Opcode Registry v1。
        #[allow(missing_docs)]
        #[repr(u16)]
        #[derive(Clone, Copy, Debug, Eq, PartialEq)]
        pub enum ProtocolOpcode {
            $($variant = $value),+
        }

        impl TryFrom<u16> for ProtocolOpcode {
            type Error = Error;

            fn try_from(value: u16) -> Result<Self> {
                match value {
                    $($value => Ok(Self::$variant),)+
                    _ => Err(Error::Malformed),
                }
            }
        }

        impl From<ProtocolOpcode> for u16 {
            fn from(value: ProtocolOpcode) -> Self {
                value as Self
            }
        }

        impl ProtocolOpcode {
            /// Registry v1 中全部已登记标准 Opcode，按线上数值升序排列。
            pub const ALL: &'static [Self] = &[$(Self::$variant),+];
        }
    };
}

protocol_opcodes! {
    BootstrapHello = 0x0001,
    BootstrapCookieChallenge = 0x0002,
    BootstrapHelloCookie = 0x0003,
    IdentityChallenge = 0x0004,
    IdentityResponse = 0x0005,
    AddressOffer = 0x0006,
    DeviceCommit = 0x0007,
    FinalCommit = 0x0008,
    AddressRenew = 0x0009,
    AddressRevoke = 0x000A,
    PeerReauthHello = 0x0010,
    PeerReauthChallenge = 0x0011,
    PeerReauthResponse = 0x0012,
    PeerSessionPrepare = 0x0013,
    PeerSessionAccept = 0x0014,
    PeerSessionCommit = 0x0015,
    PeerSessionAbort = 0x0016,
    NeighborHello = 0x0101,
    NeighborKeepalive = 0x0102,
    CapabilityAdvertise = 0x0103,
    CapabilitySelect = 0x0104,
    CapabilityAck = 0x0105,
    CapabilityRevoke = 0x0106,
    RouteRreq = 0x0201,
    RouteRrep = 0x0202,
    RouteRerr = 0x0203,
    RouteProbe = 0x0204,
    RouteProbeAck = 0x0205,
    FlowPrepare = 0x0301,
    FlowAccept = 0x0302,
    FlowCommit = 0x0303,
    FlowAbort = 0x0304,
    FlowRetire = 0x0305,
    PathActivateStage = 0x0310,
    PathStageAck = 0x0311,
    PathActivateCommit = 0x0312,
    PathCommitAck = 0x0313,
    PathActivateAbort = 0x0314,
    PathTerminalReceipt = 0x0315,
    PathProbe = 0x0316,
    PathProbeAck = 0x0317,
    DeliveryAck = 0x0401,
    TransportParentPrepare = 0x0410,
    TransportParentAccept = 0x0411,
    TransportParentCommit = 0x0412,
    TransportParentAbort = 0x0413,
    TransferSetup = 0x0420,
    TransferSetupAck = 0x0421,
    TransferAbort = 0x0422,
    TransferTerminalReceipt = 0x0423,
    TransferSackCredit = 0x0424,
    OperationQuery = 0x0501,
    OperationReceipt = 0x0502,
    OperationCancel = 0x0503,
    TimeSync = 0x0601,
    TimeFollowUp = 0x0602,
    TimeDelayRequest = 0x0603,
    TimeDelayResponse = 0x0604,
    TimeSyncAbort = 0x0605,
    GroupPrepare = 0x0701,
    GroupAccept = 0x0702,
    GroupCommit = 0x0703,
    GroupAbort = 0x0704,
    GroupRetire = 0x0705,
    GroupKeyPrepare = 0x0710,
    GroupKeyAck = 0x0711,
    GroupKeyCommit = 0x0712,
    GroupTreeStage = 0x0720,
    GroupTreeAck = 0x0721,
    GroupTreeCommit = 0x0722,
    GroupTreeAbort = 0x0723,
    GroupMembershipSnapshot = 0x0730,
    ClusterAdvertise = 0x0801,
    ClusterJoin = 0x0802,
    ClusterConfigPrepare = 0x0803,
    ClusterConfigAck = 0x0804,
    ClusterConfigCommit = 0x0805,
    ClusterBackupAssign = 0x0806,
    ClusterBackupReady = 0x0807,
    ClusterTakeoverVote = 0x0808,
    ClusterTakeoverCommit = 0x0809,
    ClusterHandoverPrepare = 0x080A,
    ClusterHandoverReady = 0x080B,
    ClusterHandoverCommit = 0x080C,
    ClusterRecoveryVote = 0x080D,
    ClusterRecoveryCommit = 0x080E,
    ClusterRekeyCommit = 0x080F,
    ClusterDirectory = 0x0810,
    ClusterTunnel = 0x0811,
    DiagnosticPing = 0x0901,
    DiagnosticPong = 0x0902,
    DiagnosticStatusQuery = 0x0903,
    DiagnosticStatusResult = 0x0904,
    DiagnosticErrorReport = 0x0905,
}

impl ProtocolOpcode {
    /// 返回 Opcode Registry 域。
    #[must_use]
    pub const fn domain(self) -> OpcodeDomain {
        match (self as u16) >> 8 {
            0x00 => OpcodeDomain::IdentitySecurity,
            0x01 => OpcodeDomain::CapabilityNeighbor,
            0x02 => OpcodeDomain::Route,
            0x03 => OpcodeDomain::FlowPath,
            0x04 => OpcodeDomain::Transport,
            0x05 => OpcodeDomain::ServiceOperation,
            0x06 => OpcodeDomain::Time,
            0x07 => OpcodeDomain::Group,
            0x08 => OpcodeDomain::Cluster,
            0x09 => OpcodeDomain::Diagnostics,
            _ => unreachable!(),
        }
    }

    /// 判断它是否属于只读诊断域。
    #[must_use]
    pub const fn is_diagnostic(self) -> bool {
        matches!(self.domain(), OpcodeDomain::Diagnostics)
    }
}

#[cfg(test)]
mod tests {
    use super::{
        AddressWidth, BindingGeneration, C0TransactionId, Error, GroupId, NodeAddress,
        OriginSequence, PersistenceRecordGeneration, ProtocolOpcode, RealmId, ServiceId, SuiteId,
    };

    #[test]
    fn scalar_boundaries_follow_the_frozen_contract() {
        assert_eq!(RealmId::new(0), Err(Error::Argument));
        assert_eq!(RealmId::new(u32::MAX), Err(Error::Argument));
        assert_eq!(
            RealmId::new(u32::MAX - 1).map(RealmId::get),
            Ok(u32::MAX - 1)
        );

        assert_eq!(NodeAddress::new(0, AddressWidth::A1), Err(Error::Argument));
        assert_eq!(
            NodeAddress::new(0xFFFF, AddressWidth::A1),
            Err(Error::Argument)
        );
        assert_eq!(
            NodeAddress::new(0xFFFE, AddressWidth::A1).map(NodeAddress::get),
            Ok(0xFFFE)
        );

        assert_eq!(ServiceId::new(0), Err(Error::Argument));
        assert_eq!(ServiceId::new(u16::MAX), Err(Error::Argument));
        assert_eq!(
            ServiceId::new(u16::MAX - 1).map(ServiceId::get),
            Ok(u16::MAX - 1)
        );
    }

    #[test]
    fn legal_terminal_values_do_not_wrap() {
        let transaction = C0TransactionId::new(u64::MAX).expect("terminal value is legal");
        let sequence = OriginSequence::new(u32::MAX).expect("terminal value is legal");
        let record =
            PersistenceRecordGeneration::new(u64::MAX).expect("record terminal value is legal");
        let group = GroupId::new(u32::MAX - 1).expect("group terminal value is legal");

        assert_eq!(transaction.checked_next(), Err(Error::Exhausted));
        assert_eq!(sequence.checked_next(), Err(Error::Exhausted));
        assert_eq!(record.checked_next(), Err(Error::Exhausted));
        assert_eq!(group.checked_next(), Err(Error::Exhausted));
        assert_eq!(
            BindingGeneration::UNBOUND.checked_next(),
            Err(Error::Argument)
        );
        assert_eq!(
            BindingGeneration::active(u32::MAX)
                .expect("binding terminal value is legal")
                .checked_next(),
            Err(Error::Exhausted)
        );
    }

    #[test]
    fn registry_rejects_unassigned_and_reserved_opcodes() {
        assert_eq!(ProtocolOpcode::try_from(0), Err(Error::Malformed));
        assert_eq!(ProtocolOpcode::try_from(0x000B), Err(Error::Malformed));
        assert_eq!(ProtocolOpcode::try_from(0x0A00), Err(Error::Malformed));
        assert_eq!(ProtocolOpcode::try_from(0xFFFF), Err(Error::Malformed));
        assert_eq!(SuiteId::try_from(0), Err(Error::Malformed));
        assert_eq!(SuiteId::try_from(4), Err(Error::Malformed));
        assert_eq!(SuiteId::try_from(16), Ok(SuiteId::HopHmacSha256_96));
        assert_eq!(
            ProtocolOpcode::try_from(0x0203),
            Ok(ProtocolOpcode::RouteRerr)
        );
        assert!(ProtocolOpcode::DiagnosticPing.is_diagnostic());
        assert!(!ProtocolOpcode::BootstrapHello.is_diagnostic());
        for opcode in ProtocolOpcode::ALL {
            assert_eq!(ProtocolOpcode::try_from(u16::from(*opcode)), Ok(*opcode));
        }
        assert_eq!(ProtocolOpcode::ALL.len(), 94);
    }
}
