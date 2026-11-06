//! Borrowed authentication transport; durable publication is isolated in `join`.
//! 借用认证传输；耐久发布单独隔离于join协调器。
use super::{
    AdapterOwner, AdmissionHandle, AdmissionKey, AdmissionOwner, BootstrapClient,
    BootstrapClientPhase, BootstrapFlow, BootstrapListener, CookieProvider, Error, Io,
    LinkIdentity, Result, Retry, TerminalOutcome, Tx, TxDriver,
};
use crate::bootstrap_wire::{cookie_transcript, event_phase};
use crate::{decode_bootstrap_event_frame, encode_bootstrap_event_frame};
use ucn_admission::{
    BootstrapEvent as Event, BootstrapFragment, BootstrapPhase as Phase, BootstrapTranscript,
    Evidence, LOGICAL_MAX_BYTES, Reassembly, decode_logical_event, encode_logical_event,
    transcript_transition_valid,
};
use ucn_owner::CallbackClaim;
#[cfg(feature = "bootstrap-join")]
mod join;
#[cfg(feature = "bootstrap-join")]
pub use join::*;

#[derive(Clone, Copy, Eq, PartialEq)]
enum Side {
    Device,
    Authority,
}
struct Outgoing {
    event: Event,
    phase: Phase,
    bytes: [u8; LOGICAL_MAX_BYTES],
    length: usize,
    chunk: u8,
    index: u8,
    count: u8,
    rounds: u16,
    due: u64,
    acknowledged: bool,
    fully_submitted: bool,
}
/// One borrowed, irreversible post-Cookie exchange; local progress is not remote admission.
/// 一次借用且不可回退的Cookie后交换；本地进展不是远端准入。
pub struct BootstrapAuthExchange<'root, 'gate, const B: usize> {
    io: &'root mut Io<'gate, B>,
    admission: Option<(&'root mut AdmissionOwner<'gate, 1, 1>, AdmissionHandle)>,
    side: Side,
    issuer_key: AdmissionKey,
    local_key: AdmissionKey,
    authority: [u8; 16],
    peer: u32,
    deadline: u64,
    chunk: u8,
    phase: Phase,
    transcript: BootstrapTranscript,
    assembly: Reassembly,
    received: Option<Event>,
    #[cfg(feature = "bootstrap-join")]
    last_evidence: Option<Evidence>,
    outgoing: Option<Outgoing>,
    callbacks_left: u16,
    next_callback: u32,
    durable: bool,
}
impl<'gate, const B: usize> BootstrapClient<'gate, B> {
    /// Borrow transport only after actual Echo completion; capture the original deadline.
    /// 只在Echo实际完成后借用传输；捕获原截止期。
    /// # Errors
    /// Busy/unsent Echo, changed Link, expiry or wrong trust anchor leaves the Client unchanged.
    pub fn auth_exchange<'root, const L: usize, const T: usize, const R: usize>(
        &'root mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        authority: [u8; 16],
        now: u64,
    ) -> Result<BootstrapAuthExchange<'root, 'gate, B>> {
        self.io.current(adapter, now)?;
        let echo = self.echo.ok_or(Error::State)?;
        if self.phase != BootstrapClientPhase::Echoing
            || now >= self.deadline
            || self
                .io
                .tx
                .as_ref()
                .is_none_or(|t| t.token.is_some() || t.completion != Some(TerminalOutcome::Success))
        {
            return Err(Error::State);
        }
        let local = self.io.local_link()?;
        let issuer = LinkIdentity {
            link_id: echo.selected_link_instance_id,
            link_generation: echo.selected_link_instance_generation,
        };
        let key = AdmissionKey {
            link: issuer,
            local_peer_discriminator: self.peer_discriminator,
            identity_digest: self.hello.identity_digest,
            transaction_id: self.hello.transaction_id,
        };
        let chunk = auth_chunk(&self.io, adapter, authority)?;
        self.phase = BootstrapClientPhase::AuthEvents;
        self.io.tx = None;
        self.io.now = now;
        Ok(BootstrapAuthExchange::new(
            &mut self.io,
            None,
            Side::Device,
            key,
            AdmissionKey { link: local, ..key },
            authority,
            self.peer_discriminator,
            self.deadline,
            chunk,
            &cookie_transcript(echo),
        ))
    }
}
impl<'gate, const B: usize> BootstrapListener<'gate, B> {
    /// Borrow exact `CookieVerified` pending without refreshing it or dropping a TX holder.
    /// 借用精确CookieVerified事务，不续期、不丢TX Holder。
    /// # Errors
    /// Missing/advanced pending, active TX, wrong anchor or changed Link rejects atomically.
    pub fn auth_exchange<'root, const L: usize, const T: usize, const R: usize>(
        &'root mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        authority: [u8; 16],
        now: u64,
    ) -> Result<BootstrapAuthExchange<'root, 'gate, B>> {
        self.io.current(adapter, now)?;
        let (handle, key) = self.pending.ok_or(Error::State)?;
        let pending = self.pending(now)?;
        if self.auth_started
            || pending.phase != Phase::CookieVerified
            || self.io.tx.as_ref().is_some_and(|t| t.token.is_some())
        {
            return Err(Error::State);
        }
        let transcript = self.admission.pending_transcript(handle)?;
        let chunk = auth_chunk(&self.io, adapter, authority)?;
        self.auth_started = true;
        self.io.tx = None;
        self.io.now = now;
        Ok(BootstrapAuthExchange::new(
            &mut self.io,
            Some((&mut self.admission, handle)),
            Side::Authority,
            key,
            key,
            authority,
            key.local_peer_discriminator,
            pending.deadline_us,
            chunk,
            &transcript,
        ))
    }
}
fn auth_chunk<const B: usize, const L: usize, const T: usize, const R: usize>(
    io: &Io<'_, B>,
    adapter: &AdapterOwner<'_, L, T, R, B>,
    authority: [u8; 16],
) -> Result<u8> {
    if io.config.max_attempts < 2
        || authority.iter().all(|b| *b == 0)
        || authority.iter().all(|b| *b == 255)
    {
        return Err(Error::Config);
    }
    let overhead = 25 + 2 * io.config.width.bytes() + 36;
    // Before authenticated Capability exchange, only the 98 B Echo body is a
    // common receive guarantee. A large local MTU cannot prove the peer's MTU.
    // 认证能力交换前只保证对端能收98字节Echo正文，不能拿本机MTU当对端能力。
    let bootstrap_frame_ceiling = 25 + 2 * io.config.width.bytes() + 98;
    let chunk = adapter
        .link_mtu(io.config.link)?
        .min(B)
        .min(bootstrap_frame_ceiling)
        .checked_sub(overhead)
        .ok_or(Error::Config)?
        .min(128);
    if chunk < 32 {
        return Err(Error::Config);
    }
    u8::try_from(chunk).map_err(|_| Error::Config)
}
impl<'root, 'gate, const B: usize> BootstrapAuthExchange<'root, 'gate, B> {
    #[allow(clippy::too_many_arguments)]
    fn new(
        io: &'root mut Io<'gate, B>,
        admission: Option<(&'root mut AdmissionOwner<'gate, 1, 1>, AdmissionHandle)>,
        side: Side,
        issuer_key: AdmissionKey,
        local_key: AdmissionKey,
        authority: [u8; 16],
        peer: u32,
        deadline: u64,
        chunk: u8,
        transcript: &BootstrapTranscript,
    ) -> Self {
        let callbacks_left = io.config.max_attempts;
        Self {
            io,
            admission,
            side,
            issuer_key,
            local_key,
            authority,
            peer,
            deadline,
            chunk,
            phase: Phase::CookieVerified,
            transcript: *transcript,
            assembly: Reassembly::new(),
            received: None,
            #[cfg(feature = "bootstrap-join")]
            last_evidence: None,
            outgoing: None,
            callbacks_left,
            next_callback: 1,
            durable: false,
        }
    }
    fn authorize(
        &mut self,
        provider: &mut impl CookieProvider,
        event: Event,
        transcript: &BootstrapTranscript,
        evidence: Evidence,
        now: u64,
    ) -> Result<()> {
        if self.callbacks_left == 0 {
            return Err(Error::Exhausted);
        }
        self.callbacks_left -= 1;
        if let Some((owner, handle)) = self.admission.as_mut() {
            owner.advance(provider, *handle, event, *transcript, evidence, now)
        } else {
            let next = self.next_callback.checked_add(1).ok_or(Error::Exhausted)?;
            let claim = CallbackClaim::new(self.io.config.owner, self.next_callback, 1, 0x0612)?;
            let lease = self.io.config.gate.try_enter(claim)?;
            self.next_callback = next;
            let result =
                provider.authorize_event(event, &self.issuer_key, transcript, now, &evidence);
            self.io.config.gate.leave(lease)?;
            result
        }
    }
    fn transition(&self, event: Event, next: &BootstrapTranscript) -> Result<Phase> {
        let phase = event_phase(event)?;
        if !(matches!(
            (self.phase, event),
            (Phase::CookieVerified, Event::AuthorityProof)
                | (Phase::AuthorityVerified, Event::DeviceProof)
        ) || self.durable
            && matches!(
                (self.phase, event),
                (Phase::DeviceVerified, Event::AddressOffer)
                    | (Phase::AddressOffered, Event::DeviceCommit)
                    | (Phase::DeviceCommitted, Event::FinalDurable)
            ))
        {
            return Err(Error::State);
        }
        if next.authority_principal != self.authority
            || next.realm_id != self.io.config.realm.get()
            || next.authority_address >= self.io.config.width.reserved_max()
            || !transcript_transition_valid(&self.transcript, next, event)
        {
            return Err(Error::Security);
        }
        Ok(phase)
    }
    /// Verify and queue one local signed event; only the two mutual-authentication events are wired.
    /// 验证并排队本地签名事件；本段仅接入双向身份认证两事件。
    /// # Errors
    /// Wrong direction/phase, forged evidence, busy TX, expiry or unsupported durable stage rejects.
    pub fn queue<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        provider: &mut impl CookieProvider,
        event: Event,
        transcript: BootstrapTranscript,
        evidence: Evidence,
        now: u64,
    ) -> Result<()> {
        if !matches!(
            (self.side, event),
            (Side::Authority, Event::AuthorityProof) | (Side::Device, Event::DeviceProof)
        ) {
            return Err(Error::State);
        }
        self.queue_event(adapter, provider, event, &transcript, evidence, now, false)
    }
    #[allow(clippy::too_many_arguments)]
    fn queue_event<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        provider: &mut impl CookieProvider,
        event: Event,
        transcript: &BootstrapTranscript,
        evidence: Evidence,
        now: u64,
        final_authorized: bool,
    ) -> Result<()> {
        self.io.current(adapter, now)?;
        if now >= self.deadline
            || self.outgoing.as_ref().is_some_and(|t| !t.acknowledged)
            || self.io.tx.as_ref().is_some_and(|t| t.token.is_some())
            || !matches!(
                (self.side, event),
                (
                    Side::Authority,
                    Event::AuthorityProof | Event::AddressOffer | Event::FinalDurable
                ) | (Side::Device, Event::DeviceProof | Event::DeviceCommit)
            )
        {
            return Err(Error::State);
        }
        let phase = self.transition(event, transcript)?;
        let mut bytes = [0; LOGICAL_MAX_BYTES];
        let length = encode_logical_event(event, phase, transcript, evidence, &mut bytes)?;
        let count =
            u8::try_from(length.div_ceil(usize::from(self.chunk))).map_err(|_| Error::Config)?;
        self.io.now = now;
        if !final_authorized {
            self.authorize(provider, event, transcript, evidence, now)?;
        } else if event != Event::FinalDurable || !self.durable {
            return Err(Error::State);
        }
        self.phase = phase;
        self.transcript = *transcript;
        #[cfg(feature = "bootstrap-join")]
        {
            self.last_evidence = Some(evidence);
        }
        self.io.now = now;
        self.io.tx = None;
        self.outgoing = Some(Outgoing {
            event,
            phase,
            bytes,
            length,
            chunk: self.chunk,
            index: 0,
            count,
            rounds: 0,
            due: now,
            acknowledged: false,
            fully_submitted: false,
        });
        Ok(())
    }
    #[cfg(feature = "bootstrap-join")]
    fn next_ready(&self) -> bool {
        self.outgoing.as_ref().is_none_or(|t| t.acknowledged)
            && self.io.tx.as_ref().is_none_or(|t| t.token.is_none())
    }
    /// One actual raw fragment; only a fully authenticated logical event advances the phase.
    /// 一份实际原始分片；只有完整且认证成功的逻辑事件才能推进阶段。
    /// # Errors
    /// Domain/peer/phase/chunk/cause mismatch or invalid evidence rejects without any admission.
    pub fn receive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        provider: &mut impl CookieProvider,
        now: u64,
    ) -> Result<bool> {
        self.io.current(adapter, now)?;
        if now >= self.deadline {
            return Err(Error::Timeout);
        }
        let peer = self.io.claim_raw(adapter, now)?;
        let (event, fragment) = decode_bootstrap_event_frame(
            &self.io.raw[..self.io.raw_length],
            self.io.config.realm,
            self.io.config.width,
        )?;
        if peer != self.peer
            || fragment.chunk_bytes > self.chunk
            || !(matches!(
                (self.side, event),
                (Side::Device, Event::AuthorityProof) | (Side::Authority, Event::DeviceProof)
            ) || self.durable
                && matches!(
                    (self.side, event),
                    (Side::Device, Event::AddressOffer | Event::FinalDurable)
                        | (Side::Authority, Event::DeviceCommit)
                ))
        {
            return Err(Error::Security);
        }
        if self.received != Some(event) {
            let expected = match event {
                Event::AuthorityProof => Phase::CookieVerified,
                Event::DeviceProof => Phase::AuthorityVerified,
                Event::AddressOffer => Phase::DeviceVerified,
                Event::DeviceCommit => Phase::AddressOffered,
                Event::FinalDurable => Phase::DeviceCommitted,
                _ => return Err(Error::State),
            };
            if self.phase != expected
                || (event != Event::AuthorityProof
                    && self.outgoing.as_ref().is_none_or(|t| !t.fully_submitted))
            {
                return Err(Error::State);
            }
            // A new role starts a distinct bounded assembly; duplicate fragments
            // of the last event still use the completed cache above.
            // 新角色使用独立有界重组；上一事件的重复分片仍使用完成缓存。
            if self.received.is_some() && fragment.fragment_index == 0 {
                self.assembly.reset();
                self.received = None;
            }
        }
        if !self.assembly.accept(
            event.opcode()?,
            self.local_key,
            self.deadline,
            now,
            fragment,
        )? {
            return Ok(false);
        }
        if self.received == Some(event) {
            return Ok(false);
        }
        let decoded = (|| {
            let (transcript, evidence) = decode_logical_event(
                event.opcode()?,
                event_phase(event)?,
                self.assembly.borrow()?,
            )?;
            Ok((self.transition(event, &transcript)?, transcript, evidence))
        })();
        let (phase, transcript, evidence) = match decoded {
            Ok(value) => value,
            Err(error) => {
                self.assembly.reset();
                return Err(error);
            }
        };
        if let Err(e) = self.authorize(provider, event, &transcript, evidence, now) {
            self.assembly.reset();
            return Err(e);
        }
        self.phase = phase;
        self.transcript = transcript;
        #[cfg(feature = "bootstrap-join")]
        {
            self.last_evidence = Some(evidence);
        }
        self.received = Some(event);
        if let Some(t) = self.outgoing.as_mut() {
            t.acknowledged = true;
        }
        Ok(true)
    }
    /// At most one physical submission per call, bounded rounds and original deadline.
    /// 每次至多一次物理提交，有界轮次且保持原截止期。
    /// # Errors
    /// Expiry/exhaustion/Driver fault preserves every in-flight holder.
    pub fn drive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<bool> {
        self.io.current(adapter, now)?;
        if now >= self.deadline {
            return Err(Error::Timeout);
        }
        let Some(t) = self.outgoing.as_mut() else {
            return Ok(false);
        };
        if self.io.tx.as_ref().is_some_and(|p| p.token.is_some()) {
            return Ok(false);
        }
        if t.acknowledged {
            self.io.tx = None;
            return Ok(false);
        }
        if self
            .io
            .tx
            .as_ref()
            .is_some_and(|p| p.completion == Some(TerminalOutcome::Success) && !p.ready)
        {
            self.io.tx = None;
            t.index += 1;
            if t.index == t.count {
                t.index = 0;
                t.rounds += 1;
                t.due = now
                    .checked_add(self.io.config.retry_us)
                    .ok_or(Error::Exhausted)?;
            }
        }
        if now < t.due {
            return Ok(false);
        }
        if t.rounds >= self.io.config.max_attempts {
            self.io.failed = true;
            return Err(Error::Exhausted);
        }
        if self.io.tx.is_none() {
            let fragment = BootstrapFragment::from_logical_chunk(
                self.local_key,
                BootstrapFlow::Join,
                t.event,
                t.phase,
                t.index,
                t.chunk,
                &t.bytes[..t.length],
            )?;
            let length = encode_bootstrap_event_frame(
                t.event,
                fragment,
                self.io.config.realm,
                self.io.config.width,
                &mut self.io.frame,
            )?;
            self.io.tx = Some(Tx {
                length,
                token: None,
                attempts: 0,
                due: now,
                deadline: self.deadline,
                ready: true,
                retry: Retry::OnDemand,
                sent: false,
                requeue: false,
                completion: None,
            });
        }
        let result = self.io.drive(adapter, driver, now);
        if t.index + 1 == t.count && self.io.tx.as_ref().is_some_and(|p| p.sent) {
            t.fully_submitted = true;
        }
        result
    }
    /// Retire real physical obligations; exact duplicate event bytes remain cached.
    /// 退休真实物理义务；精确重复事件的字节仍保留。
    /// # Errors
    /// Cancellation/completion uncertainty retains the holder.
    pub fn retire<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<()> {
        self.io.retire(adapter, driver, now)
    }
    /// Stop is local and does not fabricate an on-wire Abort or remote result.
    /// 本地停止，不伪造线上Abort或远端结果。
    /// # Errors
    /// Reentry/stale Link rejects before changes.
    pub fn stop<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
    ) -> Result<()> {
        self.io.stop(adapter)?;
        self.phase = Phase::Aborted;
        if let Some((owner, _)) = self.admission.as_mut() {
            owner.expire(u64::MAX);
        }
        Ok(())
    }
    /// Local authentication progress, never an ADMITTED view. 本地认证进展，不是ADMITTED。
    #[must_use]
    pub const fn phase(&self) -> Phase {
        self.phase
    }
    /// Exact latest authenticated transcript, not an Authority lease. 精确最新认证记录，不是Authority租约。
    #[must_use]
    pub const fn transcript(&self) -> BootstrapTranscript {
        self.transcript
    }
}
