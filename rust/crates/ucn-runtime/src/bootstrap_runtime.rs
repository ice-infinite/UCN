//! Dedicated one-link pre-authentication transport. `CookieVerified` is not ADMITTED.
//! 专属单Link认证前传输；CookieVerified不等于已准入。
use crate::bootstrap_wire::cookie_transcript;
use crate::runtime_common::GatedDriver;
use crate::{
    BootstrapMessage, bootstrap_prior_hash, decode_bootstrap_frame, encode_bootstrap_frame,
};
use ucn_adapter::{AdapterOwner, LinkHandle, TerminalOutcome, TxDriver, TxState, TxToken};
use ucn_admission::{
    AdmissionConfig, AdmissionHandle, AdmissionKey, AdmissionOwner, BootstrapFlow, Cookie,
    CookieProvider, Hello, HelloCookie, InitialHelloContext, LinkIdentity, PendingView,
};
use ucn_digest::DigestWorkspace;
use ucn_owner::CallbackGate;
use ucn_types::{AddressWidth, Error, RealmId, Result};
mod auth;
pub use auth::*;
#[cfg(feature = "peer-reauth")]
mod reauth;
#[cfg(feature = "peer-reauth")]
pub use reauth::*;

/// Fixed dedicated transport configuration; 固定专属传输配置。
#[derive(Clone, Copy)]
pub struct BootstrapConfig<'a> {
    /// Unique Coordinator instance; 唯一Coordinator实例。
    pub owner: u32,
    /// Fixed Realm, never taken from an incoming HELLO; 固定Realm。
    pub realm: RealmId,
    /// Realm address width; Realm地址宽度。
    pub width: AddressWidth,
    /// Exact local Adapter Link, not the peer's Link ID; 精确本地Link。
    pub link: LinkHandle,
    /// Shared Provider/Driver callback gate; 共用回调门。
    pub gate: &'a CallbackGate,
    /// Original half-open request/reply lifetime; 原始半开事务/回复寿命。
    pub lifetime_us: u64,
    /// Bounded retry interval; 有界重试间隔。
    pub retry_us: u64,
    /// Maximum physical submit attempts per phase; 每阶段最大提交次数。
    pub max_attempts: u16,
}
#[derive(Clone, Copy, Eq, PartialEq)]
enum Retry {
    UntilReply,
    OnDemand,
}
struct Tx {
    length: usize,
    token: Option<TxToken>,
    attempts: u16,
    due: u64,
    deadline: u64,
    ready: bool,
    retry: Retry,
    sent: bool,
    requeue: bool,
    completion: Option<TerminalOutcome>,
}
struct Io<'a, const B: usize> {
    config: BootstrapConfig<'a>,
    #[cfg(feature = "bootstrap-join")]
    started: u64,
    now: u64,
    stopped: bool,
    failed: bool,
    frame: [u8; B],
    raw: [u8; B],
    raw_length: usize,
    tx: Option<Tx>,
}
impl<'a, const B: usize> Io<'a, B> {
    fn new<const L: usize, const T: usize, const R: usize>(
        config: BootstrapConfig<'a>,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        now: u64,
    ) -> Result<Self> {
        if config.owner == 0
            || config.gate.is_active()
            || now == 0
            || config.lifetime_us == 0
            || config.lifetime_us > i64::MAX as u64
            || config.retry_us == 0
            || config.retry_us > config.lifetime_us
            || config.max_attempts == 0
            || B > 512
            || adapter.link_mtu(config.link)? < 25 + 2 * config.width.bytes() + 98
            || B < 25 + 2 * config.width.bytes() + 98
        {
            return Err(Error::Config);
        }
        Ok(Self {
            config,
            #[cfg(feature = "bootstrap-join")]
            started: now,
            now,
            stopped: false,
            failed: false,
            frame: [0; B],
            raw: [0; B],
            raw_length: 0,
            tx: None,
        })
    }
    fn current<const L: usize, const T: usize, const R: usize>(
        &self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        now: u64,
    ) -> Result<()> {
        if self.config.gate.is_active() || self.stopped || self.failed || now == 0 || now < self.now
        {
            return Err(Error::State);
        }
        adapter.link_mtu(self.config.link)?;
        Ok(())
    }
    fn local_link(&self) -> Result<LinkIdentity> {
        Ok(LinkIdentity {
            link_id: self
                .config
                .link
                .slot()
                .checked_add(1)
                .ok_or(Error::Config)?,
            link_generation: self.config.link.instance_generation(),
        })
    }
    fn claim<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        now: u64,
    ) -> Result<(BootstrapMessage, u32)> {
        let peer = self.claim_raw(adapter, now)?;
        decode_bootstrap_frame(
            &self.raw[..self.raw_length],
            self.config.realm,
            self.config.width,
        )
        .map(|message| (message, peer))
    }
    fn claim_raw<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        now: u64,
    ) -> Result<u32> {
        self.current(adapter, now)?;
        self.now = now;
        let claim = adapter.rx_claim(&mut self.raw)?;
        let result = if claim.link != self.config.link
            || claim.meta.timestamp_us == 0
            || claim.meta.timestamp_us > now
            || claim.meta.sender_discriminator == 0
        {
            Err(Error::Access)
        } else {
            self.raw_length = claim.frame_bytes;
            Ok(claim.meta.sender_discriminator)
        };
        adapter.rx_retire(claim.token)?;
        result
    }
    fn stage(
        &mut self,
        message: BootstrapMessage,
        now: u64,
        deadline: u64,
        auto_retry: bool,
    ) -> Result<()> {
        if self.tx.as_ref().is_some_and(|t| t.token.is_some()) || now >= deadline {
            return Err(Error::State);
        }
        let length = encode_bootstrap_frame(
            message,
            self.config.realm,
            self.config.width,
            &mut self.frame,
        )?;
        self.tx = Some(Tx {
            length,
            token: None,
            attempts: 0,
            due: now,
            deadline,
            ready: true,
            retry: if auto_retry {
                Retry::UntilReply
            } else {
                Retry::OnDemand
            },
            sent: false,
            requeue: false,
            completion: None,
        });
        Ok(())
    }
    fn drive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<bool> {
        self.current(adapter, now)?;
        self.now = now;
        let Some(t) = self.tx.as_mut() else {
            return Ok(false);
        };
        if now >= t.deadline {
            return Err(Error::Timeout);
        }
        if !t.ready || t.token.is_some() || now < t.due {
            return Ok(false);
        }
        if t.attempts >= self.config.max_attempts {
            self.failed = true;
            return Err(Error::Exhausted);
        }
        let due = now
            .checked_add(self.config.retry_us)
            .ok_or(Error::Exhausted)?;
        let token = adapter.tx_reserve(self.config.link, 0)?;
        t.token = Some(token);
        t.attempts += 1;
        t.due = due;
        let mut gated = GatedDriver {
            driver,
            gate: self.config.gate,
            owner: self.config.owner,
        };
        let result = adapter.tx_submit(token, &self.frame[..t.length], &mut gated);
        let state = adapter.tx_view(token)?.state;
        t.sent |= matches!(
            state,
            TxState::Submitted | TxState::Completed | TxState::InDoubt
        );
        if state == TxState::NotSubmitted {
            adapter.tx_retire(token)?;
            t.token = None;
        }
        result?;
        Ok(true)
    }
    fn retire<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<()> {
        if self.config.gate.is_active()
            || now == 0
            || now < self.now
            || adapter.adapter_instance() != self.config.link.adapter_instance()
            || adapter.link_handle(usize::from(self.config.link.slot()))? != self.config.link
        {
            return Err(Error::State);
        }
        self.now = now;
        if let Some(t) = self.tx.as_mut() {
            if let Some(token) = t.token {
                let mut view = adapter.tx_view(token)?;
                let terminal = |s| {
                    matches!(
                        s,
                        TxState::Completed | TxState::Cancelled | TxState::NotSubmitted
                    )
                };
                if !terminal(view.state) && (self.stopped || self.failed || now >= t.deadline) {
                    let mut gated = GatedDriver {
                        driver,
                        gate: self.config.gate,
                        owner: self.config.owner,
                    };
                    view = adapter.tx_cancel(token, &mut gated)?;
                }
                if terminal(view.state) {
                    t.completion = view.terminal;
                    adapter.tx_retire(token)?;
                    t.token = None;
                    if t.retry == Retry::OnDemand && view.terminal == Some(TerminalOutcome::Success)
                    {
                        t.ready = t.requeue;
                        t.requeue = false;
                    }
                }
            }
            if (self.stopped || self.failed || now >= t.deadline) && t.token.is_none() {
                self.tx = None;
            }
        }
        if self.stopped {
            match adapter.rx_claim(&mut self.raw) {
                Ok(c) => adapter.rx_retire(c.token)?,
                Err(Error::NotFound) => {}
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }
    fn stop<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
    ) -> Result<()> {
        if self.config.gate.is_active()
            || adapter.adapter_instance() != self.config.link.adapter_instance()
        {
            return Err(Error::State);
        }
        if adapter.link_handle(usize::from(self.config.link.slot()))? != self.config.link {
            return Err(Error::NotFound);
        }
        adapter.try_set_rx_enabled(false)?;
        self.stopped = true;
        Ok(())
    }
    fn quiet<const L: usize, const T: usize, const R: usize>(
        &self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
    ) -> bool {
        self.stopped
            && adapter.adapter_instance() == self.config.link.adapter_instance()
            && adapter.rx_is_quiescent()
            && self.tx.is_none()
    }
}
/// Client's unauthenticated transport phase; 客户端未认证传输阶段。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BootstrapClientPhase {
    /// No HELLO submitted; 尚未开始。
    Idle,
    /// Original HELLO waits for a Cookie; 原 HELLO 待 Cookie。
    AwaitCookie,
    /// Cookie echo scheduled; no identity/address/session is granted.
    /// 已排队回显Cookie，不授予身份、地址或会话。
    Echoing,
    /// Transport borrowed by authentication events; no return to initial HELLO.
    /// 传输已交给认证事件，不得返回初始HELLO。
    AuthEvents,
    /// No new work; 不接新工作。
    Stopped,
}
/// One new device, one immutable HELLO/nonce, actual Adapter RX/TX; no JOIN success API.
/// 单设备、不可变HELLO/nonce、真实收发；不提供入网成功API。
pub struct BootstrapClient<'a, const B: usize> {
    io: Io<'a, B>,
    hello: Hello,
    freshness: u64,
    deadline: u64,
    peer_discriminator: u32,
    phase: BootstrapClientPhase,
    echo: Option<HelloCookie>,
}
impl<'a, const B: usize> BootstrapClient<'a, B> {
    /// Bind product-issued fresh nonces and a dedicated physical peer discriminator.
    /// 绑定产品签发的新鲜nonce及专属物理Peer区分值。
    /// # Errors
    /// Invalid nonce, transport, capacity or gate rejects without any Driver call.
    #[allow(clippy::too_many_arguments)]
    pub fn new<const L: usize, const T: usize, const R: usize>(
        config: BootstrapConfig<'a>,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        hello: Hello,
        freshness: u64,
        peer_discriminator: u32,
        now: u64,
    ) -> Result<Self> {
        if hello.flow != BootstrapFlow::Join || freshness == 0 || peer_discriminator == 0 {
            return Err(Error::Config);
        }
        let mut body = [0; 40];
        ucn_admission::encode_hello(hello, &mut body)?;
        Ok(Self {
            io: Io::new(config, adapter, now)?,
            hello,
            freshness,
            deadline: 0,
            peer_discriminator,
            phase: BootstrapClientPhase::Idle,
            echo: None,
        })
    }
    /// Start exactly once; 只启动一次。
    /// # Errors
    /// Non-idle, changed Link or deadline overflow rejects.
    pub fn start<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        now: u64,
    ) -> Result<()> {
        self.io.current(adapter, now)?;
        if self.phase != BootstrapClientPhase::Idle {
            return Err(Error::State);
        }
        let deadline = now
            .checked_add(self.io.config.lifetime_us)
            .ok_or(Error::Exhausted)?;
        self.io
            .stage(BootstrapMessage::Hello(self.hello), now, deadline, true)?;
        self.deadline = deadline;
        self.io.now = now;
        self.phase = BootstrapClientPhase::AwaitCookie;
        Ok(())
    }
    /// Read actual O0 challenge, freeze the issuer's remote Link and prior frames.
    /// 接实际O0挑战，冻结签发方远端Link和前序报文；绝不认为已认证。
    /// # Errors
    /// Wrong peer/tx/flow/time or a changed challenge rejects; claimed raw item retires.
    pub fn receive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        now: u64,
    ) -> Result<()> {
        self.io.current(adapter, now)?;
        if self.phase == BootstrapClientPhase::Idle
            || self.phase == BootstrapClientPhase::Stopped
            || self.phase == BootstrapClientPhase::AuthEvents
            || now >= self.deadline
        {
            return Err(Error::State);
        }
        if self.phase == BootstrapClientPhase::AwaitCookie
            && self.io.tx.as_ref().is_none_or(|t| !t.sent)
        {
            return Err(Error::State);
        }
        let (message, peer) = self.io.claim(adapter, now)?;
        let BootstrapMessage::Challenge(c) = message else {
            return Err(Error::Unsupported);
        };
        if c.cookie_length != 16 {
            return Err(Error::Security);
        }
        if peer != self.peer_discriminator || c.transaction_id != self.hello.transaction_id {
            return Err(Error::Security);
        }
        let echo = HelloCookie {
            flow: BootstrapFlow::Join,
            identity_digest: self.hello.identity_digest,
            device_nonce: self.hello.device_nonce,
            transaction_id: self.hello.transaction_id,
            lease_freshness_challenge_nonce: self.freshness,
            selected_link_instance_id: c.issuer_link_id,
            selected_link_instance_generation: c.issuer_link_generation,
            prior_messages_hash: bootstrap_prior_hash(
                self.hello,
                c,
                self.io.config.realm,
                self.io.config.width,
                &mut DigestWorkspace::new(),
            )?,
            cookie_evidence: Cookie::new(&c.cookie[..usize::from(c.cookie_length)])?,
        };
        if self.echo.is_some_and(|old| old != echo) {
            return Err(Error::Replay);
        }
        self.echo = Some(echo);
        // Receipt of a challenge cannot erase the still-active HELLO token.
        if let Some(t) = self.io.tx.as_mut() {
            t.ready = false;
        }
        Ok(())
    }
    /// One bounded send visit; `NOT_SUBMITTED` reuses original bytes/deadline.
    /// 一次有界发送；背压重用原字节和截止期。
    /// # Errors
    /// Original deadline, Link, callback or Driver uncertainty rejects without erasing holders.
    pub fn drive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<bool> {
        self.io.current(adapter, now)?;
        if matches!(
            self.phase,
            BootstrapClientPhase::Idle | BootstrapClientPhase::AuthEvents
        ) || now >= self.deadline
        {
            return Err(Error::Timeout);
        }
        if self.phase == BootstrapClientPhase::AwaitCookie
            && self.echo.is_some()
            && self.io.tx.as_ref().is_none_or(|t| t.token.is_none())
        {
            self.io.stage(
                BootstrapMessage::Echo(self.echo.ok_or(Error::State)?),
                now,
                self.deadline,
                true,
            )?;
            self.phase = BootstrapClientPhase::Echoing;
        }
        self.io.drive(adapter, driver, now)
    }
    /// Retire one physical obligation and at most one stopped RX; 退休一个物理义务及至多一个停止RX。
    /// # Errors
    /// Unknown cancellation/completion retains exact token and blocks quiescence.
    pub fn retire<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<()> {
        self.io.retire(adapter, driver, now)
    }
    /// Stop cannot release an in-flight token; 停止不释放在途Token。
    /// # Errors
    /// Busy callback or wrong Adapter rejects.
    pub fn stop<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
    ) -> Result<()> {
        self.io.stop(adapter)?;
        self.phase = BootstrapClientPhase::Stopped;
        Ok(())
    }
    /// Transport phase only, never ADMITTED; 仅传输阶段，绝非ADMITTED。
    #[must_use]
    pub const fn phase(&self) -> BootstrapClientPhase {
        self.phase
    }
    /// Local stopped transport proof only; 仅本地已停止传输证明。
    #[must_use]
    pub fn quiescent<const L: usize, const T: usize, const R: usize>(
        &self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
    ) -> bool {
        self.io.quiet(adapter)
    }
}
/// Dedicated ingress -> Cookie verifier -> one bounded pending, not an Authority/Session.
/// 专属入站到Cookie验证和一个固定Pending，不是Authority或Session。
pub struct BootstrapListener<'a, const B: usize> {
    io: Io<'a, B>,
    admission: AdmissionOwner<'a, 1, 1>,
    pending: Option<(AdmissionHandle, AdmissionKey)>,
    cause: Option<(Hello, u32)>,
    bucket_us: u64,
    verify_tokens: u8,
    verify_burst: u8,
    verify_rate: u8,
    verify_refill: u64,
    auth_started: bool,
}
impl<'a, const B: usize> BootstrapListener<'a, B> {
    /// The exact same callback gate owns Cookie Provider and Driver scopes.
    /// Cookie Provider 和 Driver 必须共享同一精确回调门。
    /// # Errors
    /// Config, capacity, zero bucket or mismatched gate rejects before external effects.
    pub fn new<const L: usize, const T: usize, const R: usize>(
        config: BootstrapConfig<'a>,
        admission: AdmissionConfig<'a>,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        bucket_us: u64,
        now: u64,
    ) -> Result<Self> {
        if bucket_us == 0
            || !core::ptr::eq(config.gate, admission.provider_gate)
            || config.owner == admission.owner_instance
        {
            return Err(Error::Config);
        }
        let burst = admission.token_burst;
        let rate = admission.tokens_per_second;
        Ok(Self {
            io: Io::new(config, adapter, now)?,
            admission: AdmissionOwner::new(admission)?,
            pending: None,
            cause: None,
            bucket_us,
            verify_tokens: burst,
            verify_burst: burst,
            verify_rate: rate,
            verify_refill: now,
            auth_started: false,
        })
    }
    fn verify_budget(&mut self, now: u64) -> Result<()> {
        if now < self.verify_refill {
            return Err(Error::State);
        }
        let seconds = (now - self.verify_refill) / 1_000_000;
        if seconds != 0 {
            self.verify_tokens = u8::try_from(
                u64::from(self.verify_tokens)
                    .saturating_add(seconds.saturating_mul(u64::from(self.verify_rate)))
                    .min(u64::from(self.verify_burst)),
            )
            .map_err(|_| Error::Exhausted)?;
            self.verify_refill = self
                .verify_refill
                .checked_add(seconds * 1_000_000)
                .ok_or(Error::Exhausted)?;
        }
        if self.verify_tokens == 0 {
            return Err(Error::Access);
        }
        self.verify_tokens -= 1;
        Ok(())
    }
    /// One atomic raw RX, at most one bounded Cookie callback, no trust in Driver identity.
    /// 一次原子原始RX、至多一次有界Cookie回调；Driver区分值不是可信Principal。
    /// # Errors
    /// Bad frame/cookie/parent, original deadline or fixed capacity rejects without admission.
    pub fn receive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        provider: &mut impl CookieProvider,
        now: u64,
    ) -> Result<()> {
        if self.auth_started {
            return Err(Error::State);
        }
        let (message, peer) = self.io.claim(adapter, now)?;
        match message {
            BootstrapMessage::Hello(hello) => {
                if self.pending.is_some() {
                    return Err(Error::NoSpace);
                }
                if self.io.tx.as_ref().is_some_and(|t| now < t.deadline) {
                    if self.cause == Some((hello, peer)) {
                        let t = self.io.tx.as_mut().ok_or(Error::State)?;
                        if t.token.is_some() {
                            t.requeue = true;
                        } else {
                            t.ready = true;
                        }
                        return Ok(());
                    }
                    return Err(Error::NoSpace);
                }
                if self.io.tx.as_ref().is_some_and(|t| t.token.is_some()) {
                    return Err(Error::NoSpace);
                }
                let bucket = u32::try_from(now / self.bucket_us)
                    .ok()
                    .and_then(|b| b.checked_add(1))
                    .ok_or(Error::Exhausted)?;
                let challenge = self.admission.issue_cookie(
                    provider,
                    hello,
                    InitialHelloContext {
                        link: self.io.local_link()?,
                        now_us: now,
                        request_bytes: 25 + 2 * self.io.config.width.bytes() + 40,
                        response_bytes: 25 + 2 * self.io.config.width.bytes() + 40,
                        cookie_time_bucket: bucket,
                    },
                )?;
                if challenge.cookie_length != 16 {
                    return Err(Error::Security);
                }
                let deadline = now
                    .checked_add(self.io.config.lifetime_us)
                    .ok_or(Error::Exhausted)?;
                self.io
                    .stage(BootstrapMessage::Challenge(challenge), now, deadline, false)?;
                self.cause = Some((hello, peer));
            }
            BootstrapMessage::Echo(echo) => {
                let local = self.io.local_link()?;
                if echo.selected_link_instance_id != local.link_id
                    || echo.selected_link_instance_generation != local.link_generation
                {
                    return Err(Error::Security);
                }
                let key = AdmissionKey {
                    link: local,
                    local_peer_discriminator: peer,
                    identity_digest: echo.identity_digest,
                    transaction_id: echo.transaction_id,
                };
                if self.pending.is_some_and(|(_, old)| old != key) {
                    return Err(Error::NoSpace);
                }
                self.verify_budget(now)?;
                let handle = self.admission.open_after_cookie(
                    provider,
                    key,
                    echo,
                    cookie_transcript(echo),
                    now,
                )?;
                self.pending = Some((handle, key));
                if self
                    .cause
                    .is_some_and(|(h, p)| h.transaction_id == echo.transaction_id && p == peer)
                {
                    if let Some(t) = self.io.tx.as_mut() {
                        t.ready = false;
                    }
                }
            }
            BootstrapMessage::Challenge(_) => return Err(Error::Unsupported),
        }
        Ok(())
    }
    /// One scheduled Cookie reply; no amplification or automatic JOIN continuation.
    /// 一次Cookie回复调度；无放大，也不自动执行后续JOIN。
    /// # Errors
    /// Driver/Link/timeout keeps existing physical holder.
    pub fn drive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<bool> {
        if self.auth_started {
            return Err(Error::State);
        }
        self.io.drive(adapter, driver, now)
    }
    /// Explicit pending expiry, not lazy eviction on attacker input; 显式过期，不在坏输入上隐式驱逐。
    /// # Errors
    /// Callback/time rollback rejects before mutation.
    pub fn step(&mut self, now: u64) -> Result<()> {
        if self.io.config.gate.is_active() || now == 0 || now < self.io.now {
            return Err(Error::State);
        }
        self.io.now = now;
        self.admission.expire(now);
        Ok(())
    }
    /// Only CookieVerified/Aborted metadata; does not publish an admitted view.
    /// 只给出待审/终止元数据，不发布已准入View。
    /// # Errors
    /// Missing/expired pending rejects; lookup cannot refresh it.
    pub fn pending(&self, now: u64) -> Result<PendingView> {
        if self.io.config.gate.is_active() || now < self.io.now {
            return Err(Error::State);
        }
        let (h, _) = self.pending.ok_or(Error::NotFound)?;
        let view = self.admission.pending_get(h)?;
        if now >= view.deadline_us {
            return Err(Error::Timeout);
        }
        Ok(view)
    }
    /// One physical holder, one terminal pending and one stopped RX at most.
    /// 至多一个物理Holder、一个终态Pending及一个停止RX。
    /// # Errors
    /// Failed physical retirement retains its token; pending retirement is explicit.
    pub fn retire<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<()> {
        self.io.retire(adapter, driver, now)?;
        if let Some((h, _)) = self.pending {
            if self.admission.pending_get(h)?.phase == ucn_admission::BootstrapPhase::Aborted {
                self.admission.retire(h)?;
                self.pending = None;
            }
        }
        Ok(())
    }
    /// Stop new RX and expire private pre-auth pending, preserving physical obligations.
    /// 停止新RX并终止私有认证前Pending，保留物理义务。
    /// # Errors
    /// Callback/Adapter mismatch rejects before changes.
    pub fn stop<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
    ) -> Result<()> {
        self.io.stop(adapter)?;
        self.admission.expire(u64::MAX);
        Ok(())
    }
    /// Local transport/pending quiescence only; 仅本地传输及Pending收口。
    #[must_use]
    pub fn quiescent<const L: usize, const T: usize, const R: usize>(
        &self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
    ) -> bool {
        self.pending.is_none() && self.io.quiet(adapter)
    }
}
