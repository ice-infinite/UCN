#[cfg(feature = "envelope")]
use crate::ClockView;
use crate::Uncertainty;
use ucn_durable::{DurableBase, DurableContract, DurableUpdate};
use ucn_persistence::{
    CodecWorkspace, DomainKind, PersistenceHandle, PersistenceProof, PersistenceRequest,
};
use ucn_types::{Error, Result};

/// Immutable authenticated directional path, never ordinary dynamic Route.
/// 不可变认证定向Path，不能是普通动态Route。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimePath {
    /// Route causal identity; `Route因果ID`。
    pub causal: u64,
    /// Route generation; Route代际。
    pub route: u32,
    /// Peer Session generation; 会话代际。
    pub session: u32,
    /// Capability generation; 能力代际。
    pub capability: u32,
    /// Forward physical Link generation; 正向物理Link代际。
    pub forward_generation: u32,
    /// Reverse physical Link generation; 反向物理Link代际。
    pub reverse_generation: u32,
    /// Forward Link ID; `正向LinkID`。
    pub forward_link: u16,
    /// Reverse Link ID; `反向LinkID`。
    pub reverse_link: u16,
    /// Exact capability digest; 精确能力摘要。
    pub capability_digest: [u8; 16],
    /// Explicit frozen/directional path; 冻结定向Path。
    pub frozen: bool,
    /// Proven asymmetry bound, None=diagnostics only.
    /// 可信非对称界，None仅诊断。
    pub asymmetry: Option<u32>,
}
impl TimePath {
    /// Validate exact fixed path; 验证精确固定Path。
    /// # Errors
    /// Dynamic route or incomplete generations reject before pending allocation.
    pub fn validate(self) -> Result<()> {
        if !self.frozen
            || self.causal == 0
            || self.route == 0
            || self.session == 0
            || self.capability == 0
            || self.forward_generation == 0
            || self.reverse_generation == 0
            || self.forward_link == 0
            || self.reverse_link == 0
            || self.capability_digest == [0; 16]
            || self.asymmetry == Some(0)
        {
            return Err(Error::Policy);
        }
        Ok(())
    }
}
/// Domain policy persisted with its generation; 随代际持久化的Domain策略。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DomainConfig {
    /// Clock domain; 时间域。
    pub domain: u16,
    /// Published no-wrap generation; 发布代际。
    pub generation: u32,
    /// Consecutive admissible sample threshold; 连续有效样本门限。
    pub lock_samples: u8,
    /// Local sync timeout; 本地同步超时。
    pub sync_timeout: u64,
    /// Maximum local Holdover; 最大本地保持。
    pub max_holdover: u64,
    /// Credible oscillator drift ppb; 可信振荡器漂移ppb。
    pub oscillator_ppb: u32,
    /// Maximum safe offset jump; 最大安全Offset跳变。
    pub max_jump: u32,
    /// Complete base uncertainty; 完整基础误差。
    pub uncertainty: Uncertainty,
    /// Frozen path; 冻结Path。
    pub path: TimePath,
}
impl DomainConfig {
    pub(crate) fn validate(self) -> Result<()> {
        self.path.validate()?;
        self.uncertainty.aggregate()?;
        if self.domain == 0
            || self.domain == u16::MAX
            || self.generation == 0
            || self.generation > 0x7fff_ffff
            || self.lock_samples == 0
            || self.sync_timeout == 0
            || self.max_holdover == 0
            || self.oscillator_ppb == 0
            || self.max_jump == 0
            || self.path.asymmetry.is_none()
            || self.uncertainty.asymmetry < self.path.asymmetry
        {
            return Err(Error::Config);
        }
        Ok(())
    }
}
/// Domain phases; Domain状态。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DomainPhase {
    /// Reloaded authority must rotate after reboot; 重启Authority需换代。
    Loading,
    /// Awaiting exact reload; 待精确回读。
    Pending,
    /// Fresh generation, no clock; 新代际尚无时钟。
    Unsynced,
    /// Acquiring consecutive samples; 采集连续样本。
    Acquiring,
    /// Credible clock; 可信时钟。
    Locked,
    /// Bounded oscillator drift; 有界振荡器保持。
    Holdover,
    /// Scoped network-time fence; 局部网络时间Fence。
    Fault,
}
/// Private sample fact issued only by the Sync transaction owner.
/// `仅Sync事务Owner签发的私有样本事实`。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TimeSample {
    pub(crate) config: DomainConfig,
    pub(crate) local: u64,
    pub(crate) sequence: u32,
    pub(crate) offset: i64,
    pub(crate) uncertainty: u32,
}
impl TimeSample {
    /// Domain offset for diagnostics; 用于诊断的Domain Offset。
    #[must_use]
    pub const fn offset_us(self) -> i64 {
        self.offset
    }
    /// Conservative bound at the sample's local reference; 样本本地参考时刻的保守上界。
    #[must_use]
    pub const fn uncertainty_us(self) -> u32 {
        self.uncertainty
    }
}

/// Fixed-window Domain Owner; `固定窗口DomainOwner`。
pub struct TimeDomain<const WINDOW: usize> {
    contract: DurableContract,
    durable_generation: u64,
    durable_digest: [u8; 16],
    sync_sequence: u32,
    config: Option<DomainConfig>,
    pending: Option<(DurableUpdate<128>, DomainConfig)>,
    phase: DomainPhase,
    samples: [i64; WINDOW],
    sample_bounds: [u32; WINDOW],
    sample_times: [u64; WINDOW],
    count: usize,
    cursor: usize,
    consecutive: u8,
    last_sample: u64,
    last_sequence: u32,
    offset: i64,
    bound: u32,
    output_high_water: Option<(u64, u64)>,
}
impl<const W: usize> TimeDomain<W> {
    /// Bind one Time Authority durable domain; `绑定TimeAuthority耐久域`。
    /// # Errors
    /// Invalid fixed window/domain/schema rejects.
    pub fn new(contract: DurableContract) -> Result<Self> {
        if W == 0
            || W > 9
            || contract.domain.kind != DomainKind::TimeAuthority
            || contract.schema != 0x0801
            || contract.version != 1
            || contract.operation != 0x0801
        {
            return Err(Error::Config);
        }
        Ok(Self {
            contract,
            durable_generation: 0,
            durable_digest: [0; 16],
            sync_sequence: 0,
            config: None,
            pending: None,
            phase: DomainPhase::Loading,
            samples: [0; W],
            sample_bounds: [0; W],
            sample_times: [0; W],
            count: 0,
            cursor: 0,
            consecutive: 0,
            last_sample: 0,
            last_sequence: 0,
            offset: 0,
            bound: 0,
            output_high_water: None,
        })
    }
    /// Import prior generation only, remaining Loading until a durable rotation.
    /// 仅导入上一代际，在耐久换代前保持Loading。
    /// # Errors
    /// Wrong baseline/record rejects.
    pub fn restore(
        &mut self,
        base: DurableBase,
        body: &[u8],
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        if base.contract() != self.contract
            || self.config.is_some()
            || self.pending.is_some()
            || base.generation() == 0
        {
            return Err(Error::State);
        }
        base.verify_body(body, scratch)?;
        self.config = Some(decode_domain(body)?);
        self.durable_generation = base.generation();
        self.durable_digest = base.record_digest();
        self.phase = DomainPhase::Loading;
        Ok(())
    }
    /// Fence old time and persist exact next generation; Fence旧时间并持久化下一代。
    /// # Errors
    /// Non-next generation, invalid bounds or pending update rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn rotate(
        &mut self,
        base: DurableBase,
        config: DomainConfig,
        now: u64,
        until: u64,
        continuation: u32,
        scratch: &mut CodecWorkspace,
    ) -> Result<()> {
        config.validate()?;
        if usize::from(config.lock_samples) > W
            || base.contract() != self.contract
            || base.generation() != self.durable_generation
            || self.pending.is_some()
            || config.generation
                != self.config.map_or(Ok(1), |prior| {
                    prior.generation.checked_add(1).ok_or(Error::Exhausted)
                })?
        {
            return Err(Error::State);
        }
        let body = encode_domain(config)?;
        let update = DurableUpdate::prepare(base, &body, now, until, continuation, scratch)?;
        self.phase = DomainPhase::Pending;
        self.pending = Some((update, config));
        Ok(())
    }
    /// Coordinator requirement; Coordinator要求。
    /// # Errors
    /// No pending rotation rejects.
    pub fn requirement(&self) -> Result<PersistenceRequest<'_>> {
        Ok(self.pending.as_ref().ok_or(Error::State)?.0.request())
    }
    /// Bind accepted Foundation handle; `绑定FoundationHandle`。
    /// # Errors
    /// Wrong pending state rejects.
    pub fn bind(&mut self, h: PersistenceHandle) -> Result<()> {
        self.pending.as_mut().ok_or(Error::State)?.0.bind(h)
    }
    /// Publish a fresh generation only after exact witness/reload.
    /// witness/回读精确证明后才发布全新代际。
    /// # Errors
    /// Wrong proof leaves old time fenced.
    pub fn confirm(&mut self, h: PersistenceHandle, proof: PersistenceProof) -> Result<()> {
        self.pending
            .as_mut()
            .ok_or(Error::State)?
            .0
            .confirm(h, proof)?;
        let (_, config) = self.pending.take().ok_or(Error::State)?;
        self.config = Some(config);
        self.durable_generation = proof.record_generation();
        self.durable_digest = proof.body_digest();
        self.sync_sequence = 0;
        self.clear_filter();
        self.last_sample = 0;
        self.last_sequence = 0;
        self.output_high_water = None;
        self.phase = DomainPhase::Unsynced;
        Ok(())
    }
    fn clear_filter(&mut self) {
        self.samples.fill(0);
        self.sample_bounds.fill(0);
        self.sample_times.fill(0);
        self.count = 0;
        self.cursor = 0;
        self.consecutive = 0;
        self.offset = 0;
        self.bound = 0;
    }
    /// Consume a real Sync sample, rejecting rollback before LOCKED is visible.
    /// 消费真实Sync样本，LOCKED可见前就拒绝倒退。
    /// # Errors
    /// Wrong path/generation/sequence, jump, unknown bound or time regression rejects.
    pub fn ingest(&mut self, sample: TimeSample) -> Result<()> {
        let config = self.config.ok_or(Error::State)?;
        if matches!(
            self.phase,
            DomainPhase::Loading | DomainPhase::Pending | DomainPhase::Fault
        ) || sample.config != config
            || sample.local == 0
            || sample.local <= self.last_sample
            || sample.sequence <= self.last_sequence
            || sample.uncertainty == 0
        {
            return Err(Error::State);
        }
        if self.count != 0 && sample.offset.abs_diff(self.offset) > u64::from(config.max_jump) {
            return Err(Error::Policy);
        }
        let raw_time = apply_offset(sample.local, sample.offset)?;
        if self
            .output_high_water
            .is_some_and(|(local, domain)| sample.local < local || raw_time < domain)
        {
            self.phase = DomainPhase::Fault;
            return Err(Error::State);
        }
        let mut candidates = self.samples;
        candidates[self.cursor] = sample.offset;
        let count = (self.count + 1).min(W);
        let mut sorted = candidates;
        sorted[..count].sort_unstable();
        let filtered = sorted[count / 2];
        let filtered_time = apply_offset(sample.local, filtered)?;
        if self
            .output_high_water
            .is_some_and(|(_, domain)| filtered_time < domain)
        {
            self.phase = DomainPhase::Fault;
            return Err(Error::State);
        }
        let mut bounds = self.sample_bounds;
        let mut times = self.sample_times;
        bounds[self.cursor] = sample.uncertainty;
        times[self.cursor] = sample.local;
        let mut bound = 0u64;
        for index in 0..count {
            let age = sample
                .local
                .checked_sub(times[index])
                .ok_or(Error::Policy)?;
            let drift =
                (u128::from(age) * u128::from(config.oscillator_ppb)).div_ceil(1_000_000_000);
            let estimate = u64::from(bounds[index])
                .checked_add(candidates[index].abs_diff(filtered))
                .and_then(|sum| sum.checked_add(u64::try_from(drift).ok()?))
                .ok_or(Error::Policy)?;
            bound = bound.max(estimate);
        }
        let bound = u32::try_from(bound).map_err(|_| Error::Policy)?;
        self.samples = candidates;
        self.sample_bounds = bounds;
        self.sample_times = times;
        self.count = count;
        self.cursor = (self.cursor + 1) % W;
        self.offset = filtered;
        self.bound = bound;
        self.last_sample = sample.local;
        self.last_sequence = sample.sequence;
        self.consecutive = self.consecutive.saturating_add(1).min(config.lock_samples);
        self.phase = if self.consecutive >= config.lock_samples {
            DomainPhase::Locked
        } else {
            DomainPhase::Acquiring
        };
        Ok(())
    }
    /// Local timeout clears only filter, never same-generation replay/output watermarks.
    /// 本地超时仅清滤波，不清同代际重放/输出高水位。
    /// # Errors
    /// Regressing/unknown local clock rejects.
    pub fn step(&mut self, now: u64) -> Result<()> {
        let config = self.config.ok_or(Error::State)?;
        if now == 0 || now < self.last_sample {
            return Err(Error::Argument);
        }
        let age = now - self.last_sample;
        if matches!(self.phase, DomainPhase::Locked | DomainPhase::Holdover) {
            if age >= config.max_holdover {
                self.clear_filter();
                self.phase = DomainPhase::Unsynced;
            } else if age >= config.sync_timeout {
                self.phase = DomainPhase::Holdover;
            }
        } else if self.phase == DomainPhase::Acquiring && age >= config.sync_timeout {
            self.clear_filter();
            self.phase = DomainPhase::Unsynced;
        }
        Ok(())
    }
    /// Read a conservative monotonic Domain clock; 读取保守单调Domain时钟。
    /// # Errors
    /// Not Locked/allowed-Holdover, regression or arithmetic overflow rejects.
    #[cfg(feature = "envelope")]
    pub fn clock(&mut self, now: u64, allow_holdover: bool) -> Result<ClockView> {
        self.step(now)?;
        let config = self.config.ok_or(Error::State)?;
        if self.phase != DomainPhase::Locked
            && !(allow_holdover && self.phase == DomainPhase::Holdover)
        {
            return Err(Error::State);
        }
        let time = apply_offset(now, self.offset)?;
        if self
            .output_high_water
            .is_some_and(|(local, domain)| now < local || time < domain)
        {
            self.phase = DomainPhase::Fault;
            return Err(Error::State);
        }
        let drift = (u128::from(now - self.last_sample) * u128::from(config.oscillator_ppb))
            .div_ceil(1_000_000_000);
        let drift = u32::try_from(drift).map_err(|_| Error::Policy)?;
        let uncertainty = self.bound.checked_add(drift).ok_or(Error::Policy)?;
        self.output_high_water = Some((now, time));
        Ok(ClockView {
            domain: config.domain,
            generation: config.generation,
            now: time,
            uncertainty,
            holdover: self.phase == DomainPhase::Holdover,
        })
    }
    /// Current phase and accepted sample count; 当前阶段及有效样本数。
    /// Build a source stamp from current confirmed clock plus explicit sample capture bound.
    /// 用当前确认时钟及显式采样捕获界构造来源时间戳。
    /// # Errors
    /// Unknown capture error, pre-acquisition capture, future time or untrusted clock rejects.
    #[cfg(feature = "envelope")]
    pub fn make_stamp(
        &mut self,
        capture_local: u64,
        now: u64,
        capture_bound: u32,
        mode: crate::TimedMode,
        allow_holdover: bool,
    ) -> Result<crate::Envelope> {
        if capture_bound == 0
            || capture_local < self.last_sample
            || capture_local > now
            || mode == crate::TimedMode::Local
        {
            return Err(Error::Argument);
        }
        let clock = self.clock(now, allow_holdover)?;
        let capture = clock
            .now
            .checked_sub(now - capture_local)
            .ok_or(Error::Argument)?;
        let bound = clock
            .uncertainty
            .checked_add(capture_bound)
            .ok_or(Error::Policy)?;
        Ok(crate::Envelope {
            mode,
            uncertainty_class: crate::uncertainty_class(bound)?,
            hardware_capture: false,
            domain_valid: true,
            holdover: clock.holdover,
            domain: clock.domain,
            generation: clock.generation,
            capture_us: capture,
        })
    }
    /// Current phase and accepted sample count; 当前阶段及有效样本数。
    #[must_use]
    pub const fn view(&self) -> (DomainPhase, usize, u32) {
        (self.phase, self.count, self.last_sequence)
    }
    /// Exact durable binding, not a fabricated current-media proof.
    /// 精确耐久绑定，本身不是伪造的当前媒体验证证明。
    /// # Errors
    /// Loading, pending or fenced generations cannot issue network permission.
    pub fn record_binding(&self) -> Result<(DurableContract, DomainConfig, u64, [u8; 16])> {
        if matches!(
            self.phase,
            DomainPhase::Loading | DomainPhase::Pending | DomainPhase::Fault
        ) {
            return Err(Error::State);
        }
        Ok((
            self.contract,
            self.config.ok_or(Error::State)?,
            self.durable_generation,
            self.durable_digest,
        ))
    }
    /// Linear sequence permit, never reset by acquisition/holdover cleanup.
    /// 线性同步序号许可，采集/保持清理不会重置；仅耐久换代重置。
    /// # Errors
    /// Inactive generation or no-wrap exhaustion rejects.
    pub fn allocate_sync_sequence(&mut self) -> Result<SyncSequencePermit> {
        let (_, config, _, _) = self.record_binding()?;
        let sequence = self.sync_sequence.checked_add(1).ok_or(Error::Exhausted)?;
        self.sync_sequence = sequence;
        Ok(SyncSequencePermit { config, sequence })
    }
}
/// Non-Clone sequence permission from the actual Domain owner.
/// 实际Domain `Owner签发的不可Clone序号许可`。
pub struct SyncSequencePermit {
    config: DomainConfig,
    sequence: u32,
}
impl SyncSequencePermit {
    /// Consume only in the exact frozen generation/path policy; 只在精确冻结域内消费。
    /// # Errors
    /// Wrong generation/config burns the permission and rejects.
    pub fn consume(self, config: DomainConfig) -> Result<u32> {
        if self.config != config {
            return Err(Error::State);
        }
        Ok(self.sequence)
    }
}
fn apply_offset(local: u64, offset: i64) -> Result<u64> {
    if offset >= 0 {
        local.checked_add(offset.unsigned_abs())
    } else {
        local.checked_sub(offset.unsigned_abs())
    }
    .ok_or(Error::Argument)
}

/// Exact 128B Domain Record shared with C; `与C共享精确128B时间域Record`。
/// # Errors
/// Missing credible components or invalid path fails closed.
pub fn encode_domain(c: DomainConfig) -> Result<[u8; 128]> {
    c.validate()?;
    let mut b = [0; 128];
    b[..2].copy_from_slice(&1u16.to_be_bytes());
    b[2..4].copy_from_slice(&c.domain.to_be_bytes());
    b[4..8].copy_from_slice(&c.generation.to_be_bytes());
    b[8] = c.lock_samples;
    b[12..20].copy_from_slice(&c.sync_timeout.to_be_bytes());
    b[20..28].copy_from_slice(&c.max_holdover.to_be_bytes());
    b[28..32].copy_from_slice(&c.oscillator_ppb.to_be_bytes());
    b[32..36].copy_from_slice(&c.max_jump.to_be_bytes());
    let values = [
        c.uncertainty.timer,
        c.uncertainty.link,
        c.uncertainty.filter,
        c.uncertainty.rounding,
        c.uncertainty.capture,
        c.uncertainty.asymmetry,
    ];
    for (i, value) in values.into_iter().enumerate() {
        b[36 + i * 4..40 + i * 4].copy_from_slice(&value.ok_or(Error::Argument)?.to_be_bytes());
    }
    b[60] = 0x3f;
    let p = c.path;
    b[64..72].copy_from_slice(&p.causal.to_be_bytes());
    for (i, value) in [
        p.route,
        p.session,
        p.capability,
        p.forward_generation,
        p.reverse_generation,
    ]
    .into_iter()
    .enumerate()
    {
        b[72 + i * 4..76 + i * 4].copy_from_slice(&value.to_be_bytes());
    }
    b[92..94].copy_from_slice(&p.forward_link.to_be_bytes());
    b[94..96].copy_from_slice(&p.reverse_link.to_be_bytes());
    b[96..100].fill(1);
    b[100..104].copy_from_slice(&p.asymmetry.ok_or(Error::Policy)?.to_be_bytes());
    b[104..120].copy_from_slice(&p.capability_digest);
    Ok(b)
}
/// Decode exact canonical Domain Record; `解码精确规范DomainRecord`。
/// # Errors
/// Wrong reserved bits/generations/unknown uncertainty rejects.
#[allow(clippy::missing_panics_doc)] // Fixed offsets are dominated by the exact 128-byte length check.
pub fn decode_domain(b: &[u8]) -> Result<DomainConfig> {
    if b.len() != 128
        || b[..2] != [0, 1]
        || b[9..12] != [0; 3]
        || b[61..64] != [0; 3]
        || b[120..] != [0; 8]
        || b[60] != 0x3f
        || b[96..100] != [1; 4]
    {
        return Err(Error::Malformed);
    }
    let r32 = |i| u32::from_be_bytes(b[i..i + 4].try_into().expect("length checked Domain"));
    let r64 = |i| u64::from_be_bytes(b[i..i + 8].try_into().expect("length checked Domain"));
    let mut digest = [0; 16];
    digest.copy_from_slice(&b[104..120]);
    let c = DomainConfig {
        domain: u16::from_be_bytes([b[2], b[3]]),
        generation: r32(4),
        lock_samples: b[8],
        sync_timeout: r64(12),
        max_holdover: r64(20),
        oscillator_ppb: r32(28),
        max_jump: r32(32),
        uncertainty: Uncertainty {
            timer: Some(r32(36)),
            link: Some(r32(40)),
            filter: Some(r32(44)),
            rounding: Some(r32(48)),
            capture: Some(r32(52)),
            asymmetry: Some(r32(56)),
        },
        path: TimePath {
            causal: r64(64),
            route: r32(72),
            session: r32(76),
            capability: r32(80),
            forward_generation: r32(84),
            reverse_generation: r32(88),
            forward_link: u16::from_be_bytes([b[92], b[93]]),
            reverse_link: u16::from_be_bytes([b[94], b[95]]),
            capability_digest: digest,
            frozen: true,
            asymmetry: Some(r32(100)),
        },
    };
    c.validate().map_err(|_| Error::Malformed)?;
    Ok(c)
}
