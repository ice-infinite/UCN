use ucn_types::{
    DeliveryGuarantee, Error, HeaderContract, HopLimit, InteractionRole, OriginSecurity,
    PayloadKind, Result, TrafficClass,
};

pub(crate) const COMMON_HEADER_BYTES: usize = 3;

/// 三字节 Core Common Header 的强类型表示。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CommonHeader {
    /// Header Contract。
    pub contract: HeaderContract,
    /// 四级流量类别。
    pub traffic_class: TrafficClass,
    /// 交付保证。
    pub delivery: DeliveryGuarantee,
    /// 交互角色。
    pub interaction: InteractionRole,
    /// Payload 类别。
    pub payload_kind: PayloadKind,
    /// Origin 保护等级。
    pub origin_security: OriginSecurity,
    /// 当前剩余跳数，合法范围为 1..=63。
    pub hop_limit: HopLimit,
}

impl CommonHeader {
    /// 从任意 Core Packet 前缀解析 Common Header；不会修改输入。
    ///
    /// # Errors
    ///
    /// 长度、协议版本或 Registry 值非法时返回 [`Error::Malformed`]。
    pub fn decode(input: &[u8]) -> Result<Self> {
        if input.len() < COMMON_HEADER_BYTES {
            return Err(Error::Malformed);
        }

        let version = input[0] >> 4;
        if version != ucn_types::PROTOCOL_MAJOR {
            return Err(Error::Malformed);
        }

        let contract = HeaderContract::try_from(input[0] & 0x0F)?;
        let traffic_class = TrafficClass::try_from(input[1] >> 6)?;
        let delivery = DeliveryGuarantee::try_from((input[1] >> 4) & 0x03)?;
        let interaction = InteractionRole::try_from((input[1] >> 2) & 0x03)?;
        let payload_kind = PayloadKind::try_from(input[1] & 0x03)?;
        let origin_security = OriginSecurity::try_from(input[2] >> 6)?;
        let hop_limit = HopLimit::new(input[2] & 0x3F).map_err(|_| Error::Malformed)?;

        Ok(Self {
            contract,
            traffic_class,
            delivery,
            interaction,
            payload_kind,
            origin_security,
            hop_limit,
        })
    }

    /// 将已经验证的 Common Header 写入至少 3 B 的输出。
    pub fn encode(self, output: &mut [u8]) {
        output[0] = (ucn_types::PROTOCOL_MAJOR << 4) | u8::from(self.contract);
        output[1] = (u8::from(self.traffic_class) << 6)
            | (u8::from(self.delivery) << 4)
            | (u8::from(self.interaction) << 2)
            | u8::from(self.payload_kind);
        output[2] = (u8::from(self.origin_security) << 6) | self.hop_limit.get();
    }
}

pub(crate) fn read_u16_be(input: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes([input[offset], input[offset + 1]])
}

pub(crate) fn read_u32_be(input: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes([
        input[offset],
        input[offset + 1],
        input[offset + 2],
        input[offset + 3],
    ])
}

pub(crate) fn read_u64_be(input: &[u8], offset: usize) -> u64 {
    u64::from_be_bytes([
        input[offset],
        input[offset + 1],
        input[offset + 2],
        input[offset + 3],
        input[offset + 4],
        input[offset + 5],
        input[offset + 6],
        input[offset + 7],
    ])
}

pub(crate) fn read_address(input: &[u8], offset: usize, width: usize) -> u32 {
    let mut value = 0_u32;
    for byte in &input[offset..offset + width] {
        value = (value << 8) | u32::from(*byte);
    }
    value
}

pub(crate) fn write_address(output: &mut [u8], offset: usize, width: usize, value: u32) {
    let bytes = value.to_be_bytes();
    output[offset..offset + width].copy_from_slice(&bytes[4 - width..]);
}
