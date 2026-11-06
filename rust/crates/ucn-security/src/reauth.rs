//! Bound-peer transcript only; it neither allocates addresses nor grants a Session.
//! 已绑定邻居的规范正文；不分配地址、不授予Session。
use crate::{Binding, Fingerprint, HandshakeCandidate, KeySelector, SecurityLevel};
use ucn_types::{
    AddressWidth, BindingGeneration, Error, HopProfile, KeyId, LinkInstanceGeneration, NodeAddress,
    PeerSessionGeneration, RealmId, Result, SuiteId,
};
/// Fixed canonical body, excluding signature. 固定规范正文，不含签名。
pub const REAUTH_RECORD_BYTES: usize = 208;
/// Domain-separated authentication message. 域隔离认证输入。
pub const REAUTH_SIGN_DOMAIN: &[u8] = b"UCN6-PEER-REAUTH-V2";
/// Fixed local-control fragment prefix, not an ordinary Data header.
/// 固定局部控制分片前缀，不是普通Data头。
pub const REAUTH_FRAGMENT_PREFIX: usize = 18;
/// Borrowed structural fragment; no authentication claim. 借用结构分片，不表示已认证。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PeerReauthFragment<'a> {
    /// Exact message stage; 消息阶段。
    pub stage: u8,
    /// Zero-based fragment index; 零基片号。
    pub index: u8,
    /// Exact logical size; 逻辑长度。
    pub total_bytes: u16,
    /// Frozen transaction; 冻结事务。
    pub transaction: u64,
    /// Exact chunk; 精确数据块。
    pub data: &'a [u8],
}
impl PeerReauthFragment<'_> {
    fn validate(self) -> Result<()> {
        let offset = usize::from(self.index) * 80;
        let total = usize::from(self.total_bytes);
        if self.stage > 3
            || self.transaction == 0
            || !(225..=273).contains(&total)
            || offset >= total
            || self.data.len() != (total - offset).min(80)
        {
            return Err(Error::Malformed);
        }
        Ok(())
    }
}
/// Strict canonical fragment encoder; zero writes on error.
/// 严格规范分片编码，失败零写入。
/// # Errors
/// Invalid geometry or insufficient output rejects.
pub fn encode_peer_reauth_fragment(f: PeerReauthFragment<'_>, out: &mut [u8]) -> Result<usize> {
    f.validate()?;
    let n = REAUTH_FRAGMENT_PREFIX + f.data.len();
    if out.len() < n {
        return Err(Error::NoSpace);
    }
    out[0] = 1;
    out[1] = f.stage;
    out[2] = f.index;
    out[3] = u8::try_from(usize::from(f.total_bytes).div_ceil(80)).map_err(|_| Error::Malformed)?;
    out[4..6].copy_from_slice(&f.total_bytes.to_be_bytes());
    out[6..8].copy_from_slice(&(u16::from(f.index) * 80).to_be_bytes());
    out[8] = 80;
    out[9] = 0;
    out[10..18].copy_from_slice(&f.transaction.to_be_bytes());
    out[18..n].copy_from_slice(f.data);
    Ok(n)
}
/// Strict structural decode; full logical identity proof remains mandatory.
/// 严格结构解码，仍须完整逻辑身份认证。
/// # Errors
/// Any noncanonical field/length rejects.
pub fn decode_peer_reauth_fragment(b: &[u8]) -> Result<PeerReauthFragment<'_>> {
    if b.len() < 19 || b.len() > 98 || b[0] != 1 || b[8] != 80 || b[9] != 0 {
        return Err(Error::Malformed);
    }
    let f = PeerReauthFragment {
        stage: b[1],
        index: b[2],
        total_bytes: u16::from_be_bytes([b[4], b[5]]),
        transaction: u64::from_be_bytes(b[10..18].try_into().map_err(|_| Error::Malformed)?),
        data: &b[18..],
    };
    f.validate()?;
    if usize::from(b[3]) != usize::from(f.total_bytes).div_ceil(80)
        || u16::from_be_bytes([b[6], b[7]]) != u16::from(f.index) * 80
    {
        return Err(Error::Malformed);
    }
    Ok(f)
}
/// Product-bound identity signatures, not old Session tags. 身份签名，不是旧Session Tag。
pub trait PeerReauthProvider {
    /// Sign the exact domain-separated bytes with this principal's identity key.
    /// 使用精确Principal身份密钥签名全部域隔离字节。
    /// # Errors
    /// Unknown identity/key, short output or unavailable provider must reject.
    fn sign_reauth(
        &mut self,
        principal: [u8; 16],
        message: &[u8],
        output: &mut [u8],
    ) -> Result<usize>;
    /// Verify exact principal, message and product identity-key policy.
    /// 校验精确Principal、全部字节及产品身份密钥策略。
    /// # Errors
    /// Bad signature, revoked identity key or unsupported policy rejects.
    fn verify_reauth(&mut self, principal: [u8; 16], message: &[u8], proof: &[u8]) -> Result<()>;
}
/// Exact durable predecessor, never a remote reload proof.
/// 精确耐久前驱；远端声明不等于本端回读证明。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PeerReauthHistory {
    /// Session high-water; 会话高水位。
    pub session: u32,
    /// Origin key high-water; Origin密钥高水位。
    pub origin: u32,
    /// Hop key high-water; Hop密钥高水位。
    pub hop: u32,
    /// Shared predecessor transcript; 前驱共享摘要。
    pub digest: [u8; 32],
}
impl PeerReauthHistory {
    /// Only Hello has an unknown responder history. 仅Hello的响应方历史可未知。
    pub const EMPTY: Self = Self {
        session: 0,
        origin: 0,
        hop: 0,
        digest: [0; 32],
    };
}
/// Coordinator-captured local bounds for a mutually signed reauth.
/// 协调器在事务创建时捕获的本地边界；不得在重试时重起算。
#[derive(Clone, Copy)]
pub struct PeerReauthSessionPlan {
    /// Complete signed record; 完整签名正文。
    pub record: PeerReauthRecord,
    /// Local direction; 本端方向。
    pub initiator: bool,
    /// Local policy; 本端策略代际。
    pub policy: u32,
    /// Original local start; 原始本地起点。
    pub started: u64,
    /// Exact parent deadline; 精确父域截止。
    pub parent_deadline: u64,
}
/// Frozen initiator-oriented transaction; stage 0/1/2/3 = Hello/Challenge/Response/Accept.
/// 发起方定向事务；阶段0/1/2/3对应四种消息。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PeerReauthRecord {
    /// Exact stage; 精确阶段。
    pub stage: u8,
    /// Realm address class; Realm地址档。
    pub width: AddressWidth,
    /// Realm; 域。
    pub realm: RealmId,
    /// Initiator binding; 发起方Binding。
    pub initiator: Binding,
    /// Responder binding; 响应方Binding。
    pub responder: Binding,
    /// Old session high-water; 历史Session高水位。
    pub previous_session: u32,
    /// Next exact generation; 精确后继代际。
    pub next_session: u32,
    /// Origin suite; 端到端套件。
    pub origin_suite: u8,
    /// Initiator-to-responder key slot, reverse = +1; 正向槽，反向+1。
    pub origin_key: u16,
    /// Hop directional base slot; Hop正向槽。
    pub hop_key: u16,
    /// Next Origin key generation; 新Origin密钥代际。
    pub origin_generation: u32,
    /// Next Hop key generation; 新Hop密钥代际。
    pub hop_generation: u32,
    /// Initiator local Link ID/generation; 发起方本地Link域。
    pub initiator_link: (u16, u32),
    /// Responder local Link, zero only in Hello; 响应方域，Hello时为零。
    pub responder_link: (u16, u32),
    /// Unique transaction; 唯一事务。
    pub transaction: u64,
    /// Fresh initiator nonce; 发起方新鲜nonce。
    pub initiator_nonce: u64,
    /// Fresh responder nonce, zero only in Hello; 响应方nonce。
    pub responder_nonce: u64,
    /// Maximum Session duration, never a binding renewal; 最长会话时长，不续租。
    pub lifetime_us: u64,
    /// Full previous shared transcript digest; 历史完整共享摘要。
    pub previous_digest: [u8; 32],
    /// Initiator's exact durable key generations, not proposed successors.
    /// 发起方实际耐久密钥代际，不是建议后继值。
    pub initiator_keys: (u32, u32),
    /// Responder's exact durable predecessor; zero only in Hello.
    /// 响应方实际耐久前驱，仅Hello填零。
    pub responder_history: PeerReauthHistory,
}
impl PeerReauthRecord {
    /// Validate the complete shape, terminal serials are not incremented here.
    /// 校验完整形状，不在本函数分配或递增序号。
    /// # Errors
    /// Any noncanonical combination rejects.
    pub fn validate(self) -> Result<()> {
        if self.stage > 3
            || self.initiator == self.responder
            || self.initiator.principal == self.responder.principal
            || self.initiator.address == self.responder.address
            || self.previous_session == 0
            || self.next_session
                != self
                    .previous_session
                    .max(self.responder_history.session)
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?
            || self.origin_key == 0
            || self.origin_key >= u16::MAX - 1
            || self.hop_key == 0
            || self.hop_key >= u16::MAX - 1
            || self.origin_generation == 0
            || self.hop_generation == 0
            || self.initiator_keys.0 == 0
            || self.initiator_keys.1 == 0
            || self.origin_generation
                != self
                    .initiator_keys
                    .0
                    .max(self.responder_history.origin)
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?
            || self.hop_generation
                != self
                    .initiator_keys
                    .1
                    .max(self.responder_history.hop)
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?
            || self.initiator_link.0 == 0
            || self.initiator_link.1 == 0
            || self.transaction == 0
            || self.initiator_nonce == 0
            || self.lifetime_us == 0
            || self.lifetime_us > i64::MAX as u64
            || self.previous_digest == [0; 32]
            || !matches!(self.origin_suite, 1..=3)
            || (self.stage == 0
                && (self.responder_link != (0, 0)
                    || self.responder_nonce != 0
                    || self.responder_history != PeerReauthHistory::EMPTY))
            || (self.stage != 0
                && (self.responder_link.0 == 0
                    || self.responder_link.1 == 0
                    || self.responder_nonce == 0
                    || self.responder_history.session == 0
                    || self.responder_history.origin == 0
                    || self.responder_history.hop == 0
                    || self.responder_history.digest == [0; 32]))
        {
            return Err(Error::Malformed);
        }
        if self.stage != 0 {
            let a = self.initiator_history();
            let b = self.responder_history;
            if (a.session == b.session && a != b)
                || (a.session > b.session && (a.origin <= b.origin || a.hop <= b.hop))
                || (b.session > a.session && (b.origin <= a.origin || b.hop <= a.hop))
            {
                return Err(Error::Security);
            }
        }
        for b in [self.initiator, self.responder] {
            NodeAddress::new(b.address.get(), self.width)?;
            if b.generation.is_unbound() || b.principal == [0; 16] || b.principal == [255; 16] {
                return Err(Error::Malformed);
            }
        }
        Ok(())
    }
    /// Exact big-endian serialization; failure never writes output.
    /// 精确大端序列化，失败不写输出。
    /// # Errors
    /// Invalid record or wrong output size rejects.
    pub fn encode(self, out: &mut [u8]) -> Result<()> {
        self.validate()?;
        if out.len() != REAUTH_RECORD_BYTES {
            return Err(Error::NoSpace);
        }
        let mut b = [0; REAUTH_RECORD_BYTES];
        b[0] = 2;
        b[1] = self.stage;
        b[2] = self.width as u8;
        b[4..8].copy_from_slice(&self.realm.get().to_be_bytes());
        for (offset, v) in [(8, self.initiator), (32, self.responder)] {
            b[offset..offset + 4].copy_from_slice(&v.address.get().to_be_bytes());
            b[offset + 4..offset + 8].copy_from_slice(&v.generation.get().to_be_bytes());
            b[offset + 8..offset + 24].copy_from_slice(&v.principal);
        }
        b[56..60].copy_from_slice(&self.previous_session.to_be_bytes());
        b[60..64].copy_from_slice(&self.next_session.to_be_bytes());
        b[64] = self.origin_suite;
        b[65] = 16;
        b[66..68].copy_from_slice(&self.origin_key.to_be_bytes());
        b[68..70].copy_from_slice(&self.hop_key.to_be_bytes());
        b[72..76].copy_from_slice(&self.origin_generation.to_be_bytes());
        b[76..80].copy_from_slice(&self.hop_generation.to_be_bytes());
        b[80..82].copy_from_slice(&self.initiator_link.0.to_be_bytes());
        b[82..84].copy_from_slice(&self.responder_link.0.to_be_bytes());
        b[84..88].copy_from_slice(&self.initiator_link.1.to_be_bytes());
        b[88..92].copy_from_slice(&self.responder_link.1.to_be_bytes());
        for (offset, v) in [
            (92, self.transaction),
            (100, self.initiator_nonce),
            (108, self.responder_nonce),
            (116, self.lifetime_us),
        ] {
            b[offset..offset + 8].copy_from_slice(&v.to_be_bytes());
        }
        b[124..156].copy_from_slice(&self.previous_digest);
        b[156..160].copy_from_slice(&self.initiator_keys.0.to_be_bytes());
        b[160..164].copy_from_slice(&self.initiator_keys.1.to_be_bytes());
        b[164..168].copy_from_slice(&self.responder_history.session.to_be_bytes());
        b[168..172].copy_from_slice(&self.responder_history.origin.to_be_bytes());
        b[172..176].copy_from_slice(&self.responder_history.hop.to_be_bytes());
        b[176..208].copy_from_slice(&self.responder_history.digest);
        out.copy_from_slice(&b);
        Ok(())
    }
    /// Strict raw decode; 严格原始解码。
    /// # Errors
    /// Wrong size, version, reserved or malformed fields rejects.
    pub fn decode(b: &[u8]) -> Result<Self> {
        if b.len() != REAUTH_RECORD_BYTES
            || b[0] != 2
            || b[3] != 0
            || b[65] != 16
            || b[70..72] != [0, 0]
        {
            return Err(Error::Malformed);
        }
        let u16at = |o| -> Result<u16> {
            Ok(u16::from_be_bytes(
                b[o..o + 2].try_into().map_err(|_| Error::Malformed)?,
            ))
        };
        let u32at = |o| -> Result<u32> {
            Ok(u32::from_be_bytes(
                b[o..o + 4].try_into().map_err(|_| Error::Malformed)?,
            ))
        };
        let u64at = |o| -> Result<u64> {
            Ok(u64::from_be_bytes(
                b[o..o + 8].try_into().map_err(|_| Error::Malformed)?,
            ))
        };
        let width = AddressWidth::try_from(b[2])?;
        let binding = |o| -> Result<Binding> {
            Ok(Binding {
                address: NodeAddress::new(u32at(o)?, width)?,
                generation: BindingGeneration::active(u32at(o + 4)?)?,
                principal: b[o + 8..o + 24].try_into().map_err(|_| Error::Malformed)?,
            })
        };
        let v = Self {
            stage: b[1],
            width,
            realm: RealmId::new(u32at(4)?)?,
            initiator: binding(8)?,
            responder: binding(32)?,
            previous_session: u32at(56)?,
            next_session: u32at(60)?,
            origin_suite: b[64],
            origin_key: u16at(66)?,
            hop_key: u16at(68)?,
            origin_generation: u32at(72)?,
            hop_generation: u32at(76)?,
            initiator_link: (u16at(80)?, u32at(84)?),
            responder_link: (u16at(82)?, u32at(88)?),
            transaction: u64at(92)?,
            initiator_nonce: u64at(100)?,
            responder_nonce: u64at(108)?,
            lifetime_us: u64at(116)?,
            previous_digest: b[124..156].try_into().map_err(|_| Error::Malformed)?,
            initiator_keys: (u32at(156)?, u32at(160)?),
            responder_history: PeerReauthHistory {
                session: u32at(164)?,
                origin: u32at(168)?,
                hop: u32at(172)?,
                digest: b[176..208].try_into().map_err(|_| Error::Malformed)?,
            },
        };
        v.validate()?;
        Ok(v)
    }
    /// Exact initiator predecessor; 精确发起方前驱。
    #[must_use]
    pub const fn initiator_history(self) -> PeerReauthHistory {
        PeerReauthHistory {
            session: self.previous_session,
            origin: self.initiator_keys.0,
            hop: self.initiator_keys.1,
            digest: self.previous_digest,
        }
    }
    /// Derive the full immutable Session candidate, no authorization is granted.
    /// 派生完整不可变候选，不授予权限。
    /// # Errors
    /// Non-final shape, invalid policy or checked deadline arithmetic rejects.
    pub fn candidate(
        mut self,
        initiator: bool,
        policy: u32,
        started: u64,
        parent_deadline: u64,
    ) -> Result<HandshakeCandidate> {
        self.validate()?; // Reject malformed typed input before stage normalization.
        self.stage = 2;
        self.validate()?;
        if policy == 0 || started == 0 {
            return Err(Error::Config);
        }
        let mut canonical = [0; REAUTH_SIGN_DOMAIN.len() + REAUTH_RECORD_BYTES];
        canonical[..REAUTH_SIGN_DOMAIN.len()].copy_from_slice(REAUTH_SIGN_DOMAIN);
        self.encode(&mut canonical[REAUTH_SIGN_DOMAIN.len()..])?;
        let digest = ucn_digest::sha256(&canonical, &mut ucn_digest::DigestWorkspace::new());
        let selector = |suite, id| -> Result<KeySelector> {
            Ok(KeySelector {
                suite: SuiteId::try_from(suite)?,
                key_id: KeyId::new(id)?,
                key_generation: if suite == 16 {
                    self.hop_generation
                } else {
                    self.origin_generation
                },
            })
        };
        let (tx, rx) = if initiator { (0, 1) } else { (1, 0) };
        let deadline = started
            .checked_add(self.lifetime_us)
            .ok_or(Error::Exhausted)?
            .min(parent_deadline);
        if deadline <= started {
            return Err(Error::Timeout);
        }
        Ok(HandshakeCandidate {
            local: if initiator {
                self.initiator
            } else {
                self.responder
            },
            peer: if initiator {
                self.responder
            } else {
                self.initiator
            },
            link_generation: LinkInstanceGeneration::new(if initiator {
                self.initiator_link.1
            } else {
                self.responder_link.1
            })?,
            session_generation: PeerSessionGeneration::new(self.next_session)?,
            policy_generation: policy,
            expires_at_us: deadline,
            origin_level: if self.origin_suite == 1 {
                SecurityLevel::Authenticated
            } else {
                SecurityLevel::Confidential
            },
            hop_profile: HopProfile::H1,
            origin_tx: selector(self.origin_suite, self.origin_key + tx)?,
            origin_rx: selector(self.origin_suite, self.origin_key + rx)?,
            hop_tx: Some(selector(16, self.hop_key + tx)?),
            hop_rx: Some(selector(16, self.hop_key + rx)?),
            origin_fingerprint: Fingerprint::new(
                digest[..16].try_into().map_err(|_| Error::State)?,
            )?,
            hop_fingerprint: Fingerprint::new(digest[16..].try_into().map_err(|_| Error::State)?)?,
            transcript_digest: digest,
        })
    }
}
