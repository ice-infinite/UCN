use ucn_types::{Error, Result};

/// 固定摘要长度。
pub const DIGEST_BYTES: usize = 16;
/// Record Envelope 固定长度。
pub const ENVELOPE_BYTES: usize = 96;
/// Commit Marker 固定长度。
pub const MARKER_BYTES: usize = 16;
/// 每个持久化域的固定槽数。
pub const SLOT_COUNT: usize = 2;
/// 当前 Record Envelope 版本。
pub const RECORD_ENVELOPE_VERSION: u16 = 1;
/// BLAKE2s-128 Digest Suite。
pub const DIGEST_BLAKE2S_128: u16 = 1;
/// 独立单调 Witness Policy。
pub const WITNESS_INDEPENDENT_MONOTONIC: u8 = 1;
/// 16 B 原子 Commit Marker Provider 类别。
pub const ATOMIC_COMMIT_MARKER_16: u8 = 1;

const RECORD_MAGIC: &[u8; 4] = b"UC6R";
const MARKER_MAGIC: &[u8; 4] = b"UC6C";
const MANIFEST_DOMAIN: &[u8] = b"UCN6-DURABLE-MANIFEST-V1";
const BODY_DOMAIN: &[u8] = b"UCN6-PERSIST-BODY-V1";

const BLAKE2S_IV: [u32; 8] = [
    0x6A09_E667,
    0xBB67_AE85,
    0x3C6E_F372,
    0xA54F_F53A,
    0x510E_527F,
    0x9B05_688C,
    0x1F83_D9AB,
    0x5BE0_CD19,
];

const BLAKE2S_SIGMA: [[usize; 16]; 10] = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
    [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4],
    [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8],
    [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13],
    [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9],
    [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11],
    [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10],
    [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5],
    [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0],
];

/// 持久化域类别。
#[repr(u16)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum DomainKind {
    /// Identity/Address Binding。
    IdentityBinding = 1,
    /// Security high-water。
    SecurityHighWater = 2,
    /// Transport high-water。
    TransportHighWater = 3,
    /// Durable Operation Journal。
    OperationJournal = 4,
    /// Group policy/key。
    GroupPolicyKey = 5,
    /// Time Authority。
    TimeAuthority = 6,
    /// Cluster。
    Cluster = 7,
    /// Product configuration。
    ProductConfig = 8,
}

impl TryFrom<u16> for DomainKind {
    type Error = Error;

    fn try_from(value: u16) -> Result<Self> {
        match value {
            1 => Ok(Self::IdentityBinding),
            2 => Ok(Self::SecurityHighWater),
            3 => Ok(Self::TransportHighWater),
            4 => Ok(Self::OperationJournal),
            5 => Ok(Self::GroupPolicyKey),
            6 => Ok(Self::TimeAuthority),
            7 => Ok(Self::Cluster),
            8 => Ok(Self::ProductConfig),
            _ => Err(Error::Malformed),
        }
    }
}

/// 一个持久化域的稳定键。
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct DomainKey {
    /// 域类别。
    pub kind: DomainKind,
    /// 类别内非零、非全一标识。
    pub id: u64,
}

impl DomainKey {
    /// 构造合法持久化域键。
    ///
    /// # Errors
    ///
    /// `id` 为 0 或 `u64::MAX` 时返回 [`Error::Argument`]。
    pub const fn new(kind: DomainKind, id: u64) -> Result<Self> {
        if id == 0 || id == u64::MAX {
            return Err(Error::Argument);
        }
        Ok(Self { kind, id })
    }
}

/// Durable Manifest 中的一个有序域条目。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ManifestEntry {
    /// 域键。
    pub domain: DomainKey,
    /// 业务正文固定上限。
    pub body_capacity_bytes: u32,
    /// Provider 原始槽固定长度。
    pub slot_capacity_bytes: u32,
    /// 业务 Record Schema ID。
    pub schema_id: u16,
    /// 业务 Record Schema Version。
    pub schema_version: u16,
    /// 摘要 Suite。
    pub digest_suite: u16,
    /// Witness Policy。
    pub witness_policy: u8,
    /// Provider 原子类别。
    pub provider_atomicity_class: u8,
}

impl ManifestEntry {
    pub(crate) fn validate<const BODY: usize, const SLOT: usize>(&self) -> Result<()> {
        if self.schema_id == 0
            || self.schema_version == 0
            || self.digest_suite != DIGEST_BLAKE2S_128
            || self.witness_policy != WITNESS_INDEPENDENT_MONOTONIC
            || self.provider_atomicity_class != ATOMIC_COMMIT_MARKER_16
            || BODY > u32::MAX as usize
            || SLOT > u32::MAX as usize
            || self.body_capacity_bytes as usize > BODY
            || self.slot_capacity_bytes as usize > SLOT
            || self.slot_capacity_bytes as usize != SLOT
            || SLOT < ENVELOPE_BYTES + MARKER_BYTES
            || self.body_capacity_bytes as usize > SLOT - ENVELOPE_BYTES - MARKER_BYTES
        {
            return Err(Error::Config);
        }
        Ok(())
    }
}

/// 一个编译配置对应的完整 Durable Manifest。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Manifest<'a> {
    /// Manifest 协议版本，当前固定为 1。
    pub protocol_manifest_version: u32,
    /// Persistence Storage Layout，当前固定为 1。
    pub storage_layout_version: u32,
    /// 完整产品 Feature Bitset。
    pub composition_feature_bits: u64,
    /// Nano/Lite/Full Profile ID，分别为 1/2/3。
    pub profile_id: u8,
    /// 按 `(domain kind, domain id)` 严格递增的条目。
    pub entries: &'a [ManifestEntry],
}

/// Record Envelope 中参与持久化身份的字段。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RecordMeta {
    /// Domain 键。
    pub domain: DomainKey,
    /// 非零、非全一 Record Generation。
    pub record_generation: u64,
    /// 非零、非全一 Foundation Transaction ID。
    pub transaction_id: u64,
    /// Body 字节数。
    pub body_bytes: u32,
    /// Schema ID。
    pub schema_id: u16,
    /// Schema Version。
    pub schema_version: u16,
    /// 业务 Operation 类别。
    pub operation_kind: u16,
    /// 规范 Body Digest；Decoder 输出，Encoder 输入时会重新计算。
    pub body_digest: [u8; DIGEST_BYTES],
}

impl RecordMeta {
    fn validate(&self, body_len: usize) -> Result<()> {
        if self.record_generation == 0
            || self.record_generation == u64::MAX
            || self.transaction_id == 0
            || self.transaction_id == u64::MAX
            || self.schema_id == 0
            || self.schema_version == 0
            || self.operation_kind == 0
            || self.body_bytes as usize != body_len
        {
            return Err(Error::Argument);
        }
        Ok(())
    }
}

/// 槽尾 Marker 的三态分类。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MarkerState {
    /// Marker 完全等于 Provider 擦除值，Record 尚未发布。
    Erased,
    /// Marker 合法并绑定该 Generation。
    Committed(u64),
    /// Marker 非擦除但格式、CRC 或 Generation 非法。
    Torn,
}

/// 调用方持有的固定 Hash/Codec scratch；避免在 MCU 小栈中创建 Record 大副本。
pub struct CodecWorkspace {
    hash: Blake2s128,
    canonical: [u8; 36],
}

impl CodecWorkspace {
    /// 建立可静态初始化的空 scratch。
    #[must_use]
    pub const fn new() -> Self {
        Self {
            hash: Blake2s128::new(),
            canonical: [0; 36],
        }
    }
}

impl Default for CodecWorkspace {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Clone, Copy)]
struct Blake2s128 {
    h: [u32; 8],
    t: [u32; 2],
    v: [u32; 16],
    buffer: [u8; 64],
    buffered: usize,
}

impl Blake2s128 {
    const fn new() -> Self {
        Self {
            h: [0; 8],
            t: [0; 2],
            v: [0; 16],
            buffer: [0; 64],
            buffered: 0,
        }
    }

    fn init(&mut self) {
        *self = Self::new();
        self.h = BLAKE2S_IV;
        self.h[0] ^= 0x0101_0010;
    }

    fn add_count(&mut self, add: u32) {
        let old = self.t[0];
        self.t[0] = self.t[0].wrapping_add(add);
        if self.t[0] < old {
            self.t[1] = self.t[1].wrapping_add(1);
        }
    }

    fn update(&mut self, mut input: &[u8]) {
        while !input.is_empty() {
            let take = core::cmp::min(64 - self.buffered, input.len());
            self.buffer[self.buffered..self.buffered + take].copy_from_slice(&input[..take]);
            self.buffered += take;
            input = &input[take..];
            if self.buffered == 64 && !input.is_empty() {
                self.add_count(64);
                self.compress(false);
                self.buffered = 0;
            }
        }
    }

    fn update_byte(&mut self, byte: u8) {
        if self.buffered == 64 {
            self.add_count(64);
            self.compress(false);
            self.buffered = 0;
        }
        self.buffer[self.buffered] = byte;
        self.buffered += 1;
    }

    fn finish(&mut self, output: &mut [u8; DIGEST_BYTES]) {
        self.add_count(u32::try_from(self.buffered).unwrap_or(u32::MAX));
        self.buffer[self.buffered..].fill(0);
        self.compress(true);
        for (chunk, word) in output.chunks_exact_mut(4).zip(self.h.iter()) {
            chunk.copy_from_slice(&word.to_le_bytes());
        }
        *self = Self::new();
    }

    fn compress(&mut self, final_block: bool) {
        self.v[..8].copy_from_slice(&self.h);
        self.v[8..].copy_from_slice(&BLAKE2S_IV);
        self.v[12] ^= self.t[0];
        self.v[13] ^= self.t[1];
        if final_block {
            self.v[14] = !self.v[14];
        }
        let mut words = [0_u32; 16];
        for (word, bytes) in words.iter_mut().zip(self.buffer.chunks_exact(4)) {
            *word = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
        }
        for schedule in BLAKE2S_SIGMA {
            self.mix(0, 4, 8, 12, words[schedule[0]], words[schedule[1]]);
            self.mix(1, 5, 9, 13, words[schedule[2]], words[schedule[3]]);
            self.mix(2, 6, 10, 14, words[schedule[4]], words[schedule[5]]);
            self.mix(3, 7, 11, 15, words[schedule[6]], words[schedule[7]]);
            self.mix(0, 5, 10, 15, words[schedule[8]], words[schedule[9]]);
            self.mix(1, 6, 11, 12, words[schedule[10]], words[schedule[11]]);
            self.mix(2, 7, 8, 13, words[schedule[12]], words[schedule[13]]);
            self.mix(3, 4, 9, 14, words[schedule[14]], words[schedule[15]]);
        }
        for index in 0..8 {
            self.h[index] ^= self.v[index] ^ self.v[index + 8];
        }
    }

    fn mix(
        &mut self,
        a_index: usize,
        b_index: usize,
        c_index: usize,
        d_index: usize,
        first_word: u32,
        second_word: u32,
    ) {
        self.v[a_index] = self.v[a_index]
            .wrapping_add(self.v[b_index])
            .wrapping_add(first_word);
        self.v[d_index] = (self.v[d_index] ^ self.v[a_index]).rotate_right(16);
        self.v[c_index] = self.v[c_index].wrapping_add(self.v[d_index]);
        self.v[b_index] = (self.v[b_index] ^ self.v[c_index]).rotate_right(12);
        self.v[a_index] = self.v[a_index]
            .wrapping_add(self.v[b_index])
            .wrapping_add(second_word);
        self.v[d_index] = (self.v[d_index] ^ self.v[a_index]).rotate_right(8);
        self.v[c_index] = self.v[c_index].wrapping_add(self.v[d_index]);
        self.v[b_index] = (self.v[b_index] ^ self.v[c_index]).rotate_right(7);
    }
}

/// 计算标准 CRC32C。
#[must_use]
pub fn crc32c(bytes: &[u8]) -> u32 {
    let mut crc = u32::MAX;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0_u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0x82F6_3B78 & mask);
        }
    }
    !crc
}

/// 计算 BLAKE2s-128；scratch 由调用方持有。
pub fn blake2s128(bytes: &[u8], workspace: &mut CodecWorkspace) -> [u8; DIGEST_BYTES] {
    workspace.hash.init();
    workspace.hash.update(bytes);
    let mut digest = [0; DIGEST_BYTES];
    workspace.hash.finish(&mut digest);
    digest
}

fn hash_be16(hash: &mut Blake2s128, value: u16) {
    hash.update(&value.to_be_bytes());
}

fn hash_be32(hash: &mut Blake2s128, value: u32) {
    hash.update(&value.to_be_bytes());
}

fn hash_be64(hash: &mut Blake2s128, value: u64) {
    hash.update(&value.to_be_bytes());
}

/// 校验并计算完整 Durable Manifest Digest。
///
/// # Errors
///
/// 版本、Profile、容量或条目排序不符合冻结合同时返回 [`Error::Config`]。
pub fn manifest_digest<const BODY: usize, const SLOT: usize>(
    manifest: &Manifest<'_>,
    workspace: &mut CodecWorkspace,
) -> Result<[u8; DIGEST_BYTES]> {
    if manifest.protocol_manifest_version != 1
        || manifest.storage_layout_version != 1
        || !(1..=3).contains(&manifest.profile_id)
        || manifest.entries.is_empty()
    {
        return Err(Error::Config);
    }
    let mut previous = None;
    for entry in manifest.entries {
        entry.validate::<BODY, SLOT>()?;
        if previous.is_some_and(|key: DomainKey| key >= entry.domain) {
            return Err(Error::Config);
        }
        previous = Some(entry.domain);
    }
    workspace.hash.init();
    workspace.hash.update(MANIFEST_DOMAIN);
    hash_be32(&mut workspace.hash, manifest.protocol_manifest_version);
    hash_be32(&mut workspace.hash, manifest.storage_layout_version);
    hash_be64(&mut workspace.hash, manifest.composition_feature_bits);
    workspace.hash.update_byte(manifest.profile_id);
    let count = u16::try_from(manifest.entries.len()).map_err(|_| Error::Config)?;
    hash_be16(&mut workspace.hash, count);
    for entry in manifest.entries {
        hash_be16(&mut workspace.hash, entry.domain.kind as u16);
        hash_be64(&mut workspace.hash, entry.domain.id);
        hash_be16(&mut workspace.hash, entry.schema_id);
        hash_be16(&mut workspace.hash, entry.schema_version);
        hash_be32(&mut workspace.hash, entry.body_capacity_bytes);
        hash_be32(&mut workspace.hash, entry.slot_capacity_bytes);
        workspace.hash.update_byte(entry.witness_policy);
        workspace.hash.update_byte(entry.provider_atomicity_class);
        hash_be16(&mut workspace.hash, entry.digest_suite);
    }
    let mut digest = [0; DIGEST_BYTES];
    workspace.hash.finish(&mut digest);
    Ok(digest)
}

/// 计算完整规范 Body Digest。
///
/// # Errors
///
/// Meta 与正文长度或非零身份字段不一致时返回 [`Error::Argument`]。
pub fn body_digest(
    meta: &RecordMeta,
    body: &[u8],
    workspace: &mut CodecWorkspace,
) -> Result<[u8; DIGEST_BYTES]> {
    meta.validate(body.len())?;
    workspace.hash.init();
    workspace.hash.update(BODY_DOMAIN);
    workspace.canonical[0..2].copy_from_slice(&(meta.domain.kind as u16).to_be_bytes());
    workspace.canonical[2..10].copy_from_slice(&meta.domain.id.to_be_bytes());
    workspace.canonical[10..12].copy_from_slice(&meta.schema_id.to_be_bytes());
    workspace.canonical[12..14].copy_from_slice(&meta.schema_version.to_be_bytes());
    workspace.canonical[14..22].copy_from_slice(&meta.record_generation.to_be_bytes());
    workspace.canonical[22..30].copy_from_slice(&meta.transaction_id.to_be_bytes());
    workspace.canonical[30..32].copy_from_slice(&meta.operation_kind.to_be_bytes());
    workspace.canonical[32..36].copy_from_slice(&meta.body_bytes.to_be_bytes());
    workspace.hash.update(&workspace.canonical);
    workspace.hash.update(body);
    let mut digest = [0; DIGEST_BYTES];
    workspace.hash.finish(&mut digest);
    Ok(digest)
}

/// 编码 16 B Commit Marker。
///
/// # Errors
///
/// Generation 为 0 或全一时返回 [`Error::Argument`]，输出不写回。
pub fn encode_marker(generation: u64, output: &mut [u8; MARKER_BYTES]) -> Result<()> {
    if generation == 0 || generation == u64::MAX {
        return Err(Error::Argument);
    }
    let mut marker = [0; MARKER_BYTES];
    marker[..4].copy_from_slice(MARKER_MAGIC);
    marker[4..12].copy_from_slice(&generation.to_be_bytes());
    let marker_crc = crc32c(&marker[..12]);
    marker[12..16].copy_from_slice(&marker_crc.to_be_bytes());
    *output = marker;
    Ok(())
}

/// 按 Provider 擦除值分类 Commit Marker。
#[must_use]
pub fn classify_marker(marker: &[u8; MARKER_BYTES], erased_value: u8) -> MarkerState {
    if marker.iter().all(|byte| *byte == erased_value) {
        return MarkerState::Erased;
    }
    if &marker[..4] != MARKER_MAGIC
        || u32::from_be_bytes([marker[12], marker[13], marker[14], marker[15]])
            != crc32c(&marker[..12])
    {
        return MarkerState::Torn;
    }
    let generation = u64::from_be_bytes([
        marker[4], marker[5], marker[6], marker[7], marker[8], marker[9], marker[10], marker[11],
    ]);
    if generation == 0 || generation == u64::MAX {
        MarkerState::Torn
    } else {
        MarkerState::Committed(generation)
    }
}

/// 将未发布 Record image 编码到固定槽；Marker 保持擦除状态。
///
/// # Errors
///
/// Manifest、Meta、Body 或槽几何不合法时返回错误，输出不写回。
pub fn encode_record<const BODY: usize, const SLOT: usize>(
    meta: &RecordMeta,
    manifest_digest: &[u8; DIGEST_BYTES],
    body: &[u8],
    entry: &ManifestEntry,
    erased_value: u8,
    output: &mut [u8; SLOT],
    workspace: &mut CodecWorkspace,
) -> Result<()> {
    entry.validate::<BODY, SLOT>()?;
    meta.validate(body.len())?;
    if meta.domain != entry.domain
        || meta.schema_id != entry.schema_id
        || meta.schema_version != entry.schema_version
        || body.len() > BODY
        || body.len() > entry.body_capacity_bytes as usize
    {
        return Err(Error::Argument);
    }
    let digest = body_digest(meta, body, workspace)?;
    let mut envelope = [0_u8; ENVELOPE_BYTES];
    envelope[..4].copy_from_slice(RECORD_MAGIC);
    envelope[4..6].copy_from_slice(&RECORD_ENVELOPE_VERSION.to_be_bytes());
    envelope[6..8].copy_from_slice(&96_u16.to_be_bytes());
    envelope[8..10].copy_from_slice(&(meta.domain.kind as u16).to_be_bytes());
    envelope[10..12].copy_from_slice(&meta.schema_id.to_be_bytes());
    envelope[12..14].copy_from_slice(&meta.schema_version.to_be_bytes());
    envelope[16..32].copy_from_slice(manifest_digest);
    envelope[32..40].copy_from_slice(&meta.domain.id.to_be_bytes());
    envelope[40..48].copy_from_slice(&meta.record_generation.to_be_bytes());
    envelope[48..56].copy_from_slice(&meta.transaction_id.to_be_bytes());
    envelope[56..58].copy_from_slice(&meta.operation_kind.to_be_bytes());
    envelope[58..60].copy_from_slice(&DIGEST_BLAKE2S_128.to_be_bytes());
    envelope[60..64].copy_from_slice(&meta.body_bytes.to_be_bytes());
    envelope[64..68].copy_from_slice(&crc32c(body).to_be_bytes());
    envelope[72..88].copy_from_slice(&digest);
    let header_crc = crc32c(&envelope);
    envelope[68..72].copy_from_slice(&header_crc.to_be_bytes());

    output.fill(erased_value);
    output[..ENVELOPE_BYTES].copy_from_slice(&envelope);
    output[ENVELOPE_BYTES..ENVELOPE_BYTES + body.len()].copy_from_slice(body);
    Ok(())
}

/// 解码已发布的完整 Record image。
///
/// # Errors
///
/// Marker、Envelope、CRC、Digest、Manifest 或擦除尾部任一不一致时返回
/// [`Error::Malformed`]；`meta_out/body_out` 在失败时完全不写回。
pub fn decode_record<const BODY: usize, const SLOT: usize>(
    slot: &[u8; SLOT],
    entry: &ManifestEntry,
    manifest_digest: &[u8; DIGEST_BYTES],
    erased_value: u8,
    meta_out: &mut RecordMeta,
    body_out: &mut [u8; BODY],
    workspace: &mut CodecWorkspace,
) -> Result<usize> {
    entry
        .validate::<BODY, SLOT>()
        .map_err(|_| Error::Argument)?;
    let marker_offset = SLOT - MARKER_BYTES;
    let mut marker = [0; MARKER_BYTES];
    marker.copy_from_slice(&slot[marker_offset..]);
    let MarkerState::Committed(marker_generation) = classify_marker(&marker, erased_value) else {
        return Err(Error::Malformed);
    };
    if &slot[..4] != RECORD_MAGIC
        || read_u16(slot, 4) != RECORD_ENVELOPE_VERSION
        || read_u16(slot, 6) != 96
        || read_u16(slot, 8) != entry.domain.kind as u16
        || read_u16(slot, 10) != entry.schema_id
        || read_u16(slot, 12) != entry.schema_version
        || read_u16(slot, 14) != 0
        || &slot[16..32] != manifest_digest
        || read_u64(slot, 32) != entry.domain.id
        || read_u16(slot, 58) != DIGEST_BLAKE2S_128
        || slot[88..96].iter().any(|byte| *byte != 0)
    {
        return Err(Error::Malformed);
    }
    let body_bytes = read_u32(slot, 60) as usize;
    let candidate = RecordMeta {
        domain: entry.domain,
        record_generation: read_u64(slot, 40),
        transaction_id: read_u64(slot, 48),
        body_bytes: read_u32(slot, 60),
        schema_id: read_u16(slot, 10),
        schema_version: read_u16(slot, 12),
        operation_kind: read_u16(slot, 56),
        body_digest: [
            slot[72], slot[73], slot[74], slot[75], slot[76], slot[77], slot[78], slot[79],
            slot[80], slot[81], slot[82], slot[83], slot[84], slot[85], slot[86], slot[87],
        ],
    };
    if candidate.validate(body_bytes).is_err()
        || candidate.record_generation != marker_generation
        || body_bytes > BODY
        || body_bytes > entry.body_capacity_bytes as usize
        || ENVELOPE_BYTES + body_bytes > marker_offset
    {
        return Err(Error::Malformed);
    }
    let mut header = [0_u8; ENVELOPE_BYTES];
    header.copy_from_slice(&slot[..ENVELOPE_BYTES]);
    let recorded_header_crc = read_u32(&header, 68);
    header[68..72].fill(0);
    let body = &slot[ENVELOPE_BYTES..ENVELOPE_BYTES + body_bytes];
    if recorded_header_crc != crc32c(&header)
        || read_u32(slot, 64) != crc32c(body)
        || body_digest(&candidate, body, workspace).map_err(|_| Error::Malformed)?
            != candidate.body_digest
        || slot[ENVELOPE_BYTES + body_bytes..marker_offset]
            .iter()
            .any(|byte| *byte != erased_value)
    {
        return Err(Error::Malformed);
    }
    body_out[..body_bytes].copy_from_slice(body);
    *meta_out = candidate;
    Ok(body_bytes)
}

fn read_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes(bytes[offset..offset + 2].try_into().unwrap())
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn read_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_be_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

#[cfg(test)]
mod tests {
    use super::*;

    const BODY: usize = 1024;
    const SLOT: usize = ENVELOPE_BYTES + BODY + MARKER_BYTES;

    fn entry() -> ManifestEntry {
        ManifestEntry {
            domain: DomainKey::new(DomainKind::ProductConfig, 1).unwrap(),
            body_capacity_bytes: 1024,
            slot_capacity_bytes: 1136,
            schema_id: 1,
            schema_version: 1,
            digest_suite: DIGEST_BLAKE2S_128,
            witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
            provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
        }
    }

    #[test]
    fn hash_oracles_match_the_c_foundation() {
        let mut workspace = CodecWorkspace::new();
        assert_eq!(
            blake2s128(b"", &mut workspace),
            hex("64550d6ffe2c0a01a14aba1eade0200c")
        );
        assert_eq!(
            blake2s128(b"abc", &mut workspace),
            hex("aa4938119b1dc7b87cbad0ffd200d0ae")
        );
        assert_eq!(crc32c(b"123456789"), 0xE306_9283);
    }

    #[test]
    fn full_manifest_and_body_golden_match_c() {
        let entry = entry();
        let manifest = Manifest {
            protocol_manifest_version: 1,
            storage_layout_version: 1,
            composition_feature_bits: 0,
            profile_id: 3,
            entries: core::slice::from_ref(&entry),
        };
        let mut workspace = CodecWorkspace::new();
        let digest = manifest_digest::<BODY, SLOT>(&manifest, &mut workspace).unwrap();
        // Feature mask 0 的独立 Rust fixture；C/Rust 共享 fixture 测试会覆盖实际产品 mask。
        assert_eq!(digest, hex("d8568dfd0af446d55289de734850aadc"));
        let meta = RecordMeta {
            domain: entry.domain,
            record_generation: 1,
            transaction_id: 5,
            body_bytes: 3,
            schema_id: 1,
            schema_version: 1,
            operation_kind: 2,
            body_digest: [0; DIGEST_BYTES],
        };
        assert_eq!(
            body_digest(&meta, &[1, 2, 3], &mut workspace).unwrap(),
            hex("e4816b54f028eb5121253114c18612dc")
        );
    }

    #[test]
    fn record_round_trip_and_negative_matrix_are_fail_closed() {
        let entry = entry();
        let manifest_digest = [0x31; DIGEST_BYTES];
        let body = [1, 2, 3];
        let meta = RecordMeta {
            domain: entry.domain,
            record_generation: 1,
            transaction_id: 5,
            body_bytes: 3,
            schema_id: 1,
            schema_version: 1,
            operation_kind: 2,
            body_digest: [0; DIGEST_BYTES],
        };
        let mut workspace = CodecWorkspace::new();
        let mut slot = [0xA5; SLOT];
        encode_record::<BODY, SLOT>(
            &meta,
            &manifest_digest,
            &body,
            &entry,
            0xFF,
            &mut slot,
            &mut workspace,
        )
        .unwrap();
        assert_eq!(&slot[..8], b"UC6R\0\x01\0\x60");
        let mut marker = [0; MARKER_BYTES];
        encode_marker(1, &mut marker).unwrap();
        slot[SLOT - MARKER_BYTES..].copy_from_slice(&marker);
        let mut decoded = RecordMeta {
            body_digest: [0xA5; 16],
            ..meta
        };
        let mut decoded_body = [0xA5; BODY];
        assert_eq!(
            decode_record(
                &slot,
                &entry,
                &manifest_digest,
                0xFF,
                &mut decoded,
                &mut decoded_body,
                &mut workspace
            ),
            Ok(3)
        );
        assert_eq!(decoded.transaction_id, 5);
        assert_eq!(&decoded_body[..3], &body);

        for offset in [
            0,
            4,
            6,
            8,
            10,
            12,
            14,
            16,
            32,
            40,
            48,
            56,
            58,
            60,
            64,
            68,
            72,
            88,
            96,
            99,
            SLOT - 16,
            SLOT - 12,
            SLOT - 4,
            SLOT - 1,
        ] {
            let mut malformed = slot;
            malformed[offset] ^= 1;
            let before_meta = decoded;
            let before_body = decoded_body;
            assert_eq!(
                decode_record(
                    &malformed,
                    &entry,
                    &manifest_digest,
                    0xFF,
                    &mut decoded,
                    &mut decoded_body,
                    &mut workspace
                ),
                Err(Error::Malformed)
            );
            assert_eq!(decoded, before_meta);
            assert_eq!(decoded_body, before_body);
        }
    }

    fn hex(value: &str) -> [u8; DIGEST_BYTES] {
        let mut result = [0; DIGEST_BYTES];
        for (index, byte) in result.iter_mut().enumerate() {
            *byte = u8::from_str_radix(&value[index * 2..index * 2 + 2], 16).unwrap();
        }
        result
    }
}
