//! Coordinator-only durable JOIN; no Owner calls another Owner.
//! Coordinator专用耐久JOIN；各Owner不互相调用。
use super::{
    AdapterOwner, BootstrapAuthExchange, BootstrapTranscript, CookieProvider, Error, Event,
    Evidence, LOGICAL_MAX_BYTES, Phase, Result, Side, TerminalOutcome, TxDriver,
    encode_logical_event,
};
use ucn_identity::{
    AuthorityChallengeHandle, AuthorityEpoch, AuthorityFreshness, AuthorityHandle,
    AuthorityTransitionKind, BindingCertificate, BindingChallengeHandle, BindingHandle,
    BindingView, IdentityBinding, IdentityDurabilityBase, IdentityOwner, IdentityProof,
    IdentityVerifier, Principal,
};
use ucn_persistence::{PersistenceHandle, PersistenceOwner, RequestState};
use ucn_security::{
    Binding, CryptoProvider, CurrentFacts, DurabilityBase, Fingerprint, HandshakeCandidate,
    HandshakeProof, KeySelector, SecurityLevel, SecurityOwner, SessionHandle, SessionView,
};
use ucn_types::{
    AddressAuthorityGeneration, BindingGeneration, HopProfile, KeyId, LinkInstanceGeneration,
    NodeAddress, PeerSessionGeneration, SuiteId,
};

/// Opaque locally captured freshness; never reconstructed from a delayed Final.
/// 本机捕获的新鲜度句柄；绝不从迟到Final重建起点。
pub struct JoinFreshness<'gate> {
    gate: &'gate ucn_owner::CallbackGate,
    authority: AuthorityChallengeHandle,
    binding: BindingChallengeHandle,
    authority_principal: Principal,
    device: Principal,
    nonce: u64,
    transaction: u64,
    started: u64,
}
impl<'gate> JoinFreshness<'gate> {
    /// Device calls before HELLO; Authority calls before its first proof.
    /// 设备在HELLO前调用，Authority在首份身份证明前调用。
    /// # Errors
    /// Occupied/expired challenge capacity rejects; already allocated challenges
    /// remain bounded and must be explicitly expired if the second allocation fails.
    pub fn begin<const N: usize>(
        identity: &mut IdentityOwner<'gate, N>,
        authority: Principal,
        device: Principal,
        nonce: u64,
        transaction: u64,
        now: u64,
    ) -> Result<Self> {
        if now == 0 || authority == device {
            return Err(Error::Argument);
        }
        let a = identity.begin_authority_challenge(authority, nonce, transaction, now)?;
        let b = identity.begin_binding_challenge(device, nonce, transaction, now)?;
        Ok(Self {
            gate: identity.callback_gate(),
            authority: a,
            binding: b,
            authority_principal: authority,
            device,
            nonce,
            transaction,
            started: now,
        })
    }
}
#[derive(Clone, Copy)]
enum Work {
    Authority(AuthorityHandle),
    Binding(BindingHandle),
    Session(SessionHandle),
}
/// Product policy only; identities, keys and generations come from the signed transcript.
/// 仅包含产品策略；身份、密钥及代际必须取自认证记录。
#[derive(Clone, Copy)]
pub struct JoinSessionPolicy {
    /// Current local Policy generation; 当前本机策略代际。
    pub generation: u32,
    /// Maximum lifetime anchored at the original freshness start, not Final receipt.
    /// 最大寿命锚定最初新鲜度起点，不锚定Final到达时间。
    pub lifetime_us: u64,
}
/// One bounded durable JOIN Coordinator. Final reception alone never activates a Session.
/// 单个有界耐久JOIN协调器；仅收到Final绝不激活Session。
pub struct BootstrapJoin<'root, 'gate, const B: usize> {
    exchange: BootstrapAuthExchange<'root, 'gate, B>,
    freshness: JoinFreshness<'gate>,
    work: Option<(Work, PersistenceHandle)>,
    binding: Option<BindingHandle>,
    binding_transcript: Option<BootstrapTranscript>,
    binding_evidence: Option<Evidence>,
    session: Option<(SessionHandle, HandshakeCandidate)>,
    handed_off: bool,
}
impl<'root, 'gate, const B: usize> BootstrapJoin<'root, 'gate, B> {
    /// Consume a Cookie-stage exchange and genuine locally captured challenge handles.
    /// 消费 Cookie 阶段交换及真实本地 Challenge 句柄。
    /// # Errors
    /// Late device challenge, foreign nonce/transaction/anchor or advanced exchange rejects.
    pub fn new(
        mut exchange: BootstrapAuthExchange<'root, 'gate, B>,
        freshness: JoinFreshness<'gate>,
    ) -> Result<Self> {
        let t = exchange.transcript;
        if !core::ptr::eq(freshness.gate, exchange.io.config.gate)
            || exchange.phase != Phase::CookieVerified
            || exchange.durable
            || freshness.authority_principal.bytes() != exchange.authority
            || freshness.nonce != t.lease_freshness_challenge_nonce
            || freshness.transaction != t.transaction_id
            || freshness.started > exchange.io.now
            || (exchange.side == Side::Device && freshness.started > exchange.io.started)
        {
            return Err(Error::Security);
        }
        exchange.durable = true;
        Ok(Self {
            exchange,
            freshness,
            work: None,
            binding: None,
            binding_transcript: None,
            binding_evidence: None,
            session: None,
            handed_off: false,
        })
    }
    fn current<const L: usize, const T: usize, const R: usize>(
        &self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        now: u64,
    ) -> Result<()> {
        if self.handed_off {
            return Err(Error::State);
        }
        self.exchange.io.current(adapter, now)?;
        if now >= self.exchange.deadline {
            return Err(Error::Timeout);
        }
        Ok(())
    }
    fn check_identity<const N: usize>(
        &self,
        identity: &IdentityOwner<'_, N>,
        now: u64,
    ) -> Result<()> {
        if !core::ptr::eq(identity.callback_gate(), self.exchange.io.config.gate)
            || identity.authority_get(now)? != authority(&self.exchange.transcript)?
        {
            return Err(Error::Security);
        }
        Ok(())
    }
    fn live_binding<const N: usize>(
        &self,
        identity: &IdentityOwner<'_, N>,
        now: u64,
    ) -> Result<BindingView> {
        self.check_identity(identity, now)?;
        let v = identity.binding_get(self.binding.ok_or(Error::State)?, now)?;
        if Some(self.exchange.transcript) != self.binding_transcript
            || v.certificate()
                != certificate(&self.exchange.transcript, self.exchange.io.config.width)?
        {
            return Err(Error::Security);
        }
        Ok(v)
    }
    /// Two authentication events keep the existing bounded Provider path.
    /// 双向认证事件沿用已有有界Provider路径。
    /// # Errors
    /// Wrong identity or phase rejects before cryptographic or network effects.
    pub fn queue_auth<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        provider: &mut impl CookieProvider,
        event: Event,
        transcript: BootstrapTranscript,
        evidence: Evidence,
        now: u64,
    ) -> Result<()> {
        self.current(adapter, now)?;
        if event == Event::DeviceProof
            && transcript.joining_device_principal != self.freshness.device.bytes()
        {
            return Err(Error::Security);
        }
        self.exchange
            .queue(adapter, provider, event, transcript, evidence, now)
    }
    /// Authenticated raw receive; Offer/Final are facts, not durable publication.
    /// 认证原始接收；Offer/Final仅是事实，不是耐久发布。
    /// # Errors
    /// Invalid proof, peer, phase or deadline never grants a Session.
    pub fn receive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        provider: &mut impl CookieProvider,
        now: u64,
    ) -> Result<bool> {
        self.current(adapter, now)?;
        self.exchange.receive(adapter, provider, now)
    }
    /// Persist the authenticated Authority Epoch using the pre-HELLO challenge.
    /// 使用 HELLO 前 Challenge 持久化已认证 Authority Epoch。
    /// # Errors
    /// No authenticated Authority, active work, expired baseline or Provider rejection fails closed.
    #[allow(clippy::too_many_arguments)]
    pub fn persist_authority<
        const N: usize,
        const L: usize,
        const T: usize,
        const R: usize,
        const D: usize,
        const Q: usize,
        const S: usize,
    >(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        identity: &mut IdentityOwner<'_, N>,
        verifier: &mut impl IdentityVerifier,
        media: &mut PersistenceOwner<'_, D, Q, S>,
        base: IdentityDurabilityBase,
        kind: AuthorityTransitionKind,
        evidence: Evidence,
        now: u64,
    ) -> Result<()> {
        self.current(adapter, now)?;
        if self.work.is_some()
            || self.exchange.io.stopped
            || self.exchange.io.failed
            || self.exchange.io.config.gate.is_active()
            || !core::ptr::eq(identity.callback_gate(), self.exchange.io.config.gate)
            || self.exchange.phase != Phase::AuthorityVerified
            || self.exchange.side != Side::Device
            || self.exchange.last_evidence != Some(evidence)
            || now < self.exchange.io.now
            || now >= self.exchange.deadline
        {
            return Err(Error::State);
        }
        let t = self.exchange.transcript;
        self.exchange.io.now = now;
        let (h, req) = identity.prepare_authority(
            verifier,
            self.freshness.authority,
            kind,
            authority(&t)?,
            freshness(&t, true)?,
            IdentityProof {
                bytes: evidence.bytes(),
            },
            base,
            now,
        )?;
        let p = match media.submit(&req.persistence_request(), now) {
            Ok(p) => p,
            Err(e) => {
                identity.abort_authority(h)?;
                return Err(e);
            }
        };
        self.work = Some((Work::Authority(h), p));
        self.exchange.io.now = now;
        identity.bind_authority_persistence(h, p)
    }
    /// Authority reserves the exact proposed address before Offer; Device persists it before Commit.
    /// Authority在Offer前保留精确地址；设备在Commit前持久化同一地址。
    /// # Errors
    /// Wrong role/phase/transcript, stale Authority, nonce or real Foundation baseline rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn persist_binding<
        const N: usize,
        const L: usize,
        const T: usize,
        const R: usize,
        const D: usize,
        const Q: usize,
        const S: usize,
    >(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        identity: &mut IdentityOwner<'_, N>,
        verifier: &mut impl IdentityVerifier,
        media: &mut PersistenceOwner<'_, D, Q, S>,
        base: IdentityDurabilityBase,
        transcript: BootstrapTranscript,
        evidence: Evidence,
        now: u64,
    ) -> Result<()> {
        self.current(adapter, now)?;
        if self.work.is_some()
            || self.exchange.io.stopped
            || self.exchange.io.failed
            || self.exchange.io.config.gate.is_active()
            || self.binding.is_some()
            || now < self.exchange.io.now
            || now >= self.exchange.deadline
            || transcript.joining_device_principal != self.freshness.device.bytes()
        {
            return Err(Error::State);
        }
        match self.exchange.side {
            Side::Authority => {
                self.exchange.transition(Event::AddressOffer, &transcript)?;
            }
            Side::Device
                if self.exchange.phase == Phase::AddressOffered
                    && self.exchange.transcript == transcript
                    && self.exchange.last_evidence == Some(evidence) => {}
            Side::Device => return Err(Error::State),
        }
        self.check_identity(identity, now)?;
        self.exchange.io.now = now;
        let (h, req) = identity.prepare_binding(
            verifier,
            self.freshness.binding,
            certificate(&transcript, self.exchange.io.config.width)?,
            freshness(&transcript, false)?,
            IdentityProof {
                bytes: evidence.bytes(),
            },
            base,
            now,
        )?;
        let p = match media.submit(&req.persistence_request(), now) {
            Ok(p) => p,
            Err(e) => {
                identity.abort_binding(h)?;
                return Err(e);
            }
        };
        self.work = Some((Work::Binding(h), p));
        self.binding_transcript = Some(transcript);
        self.binding_evidence = Some(evidence);
        self.exchange.io.now = now;
        identity.bind_binding_persistence(h, p)
    }
    /// Consume only the exact live Foundation reload proof, then retire it.
    /// 仅消费精确且存活的Foundation回读证明，再退休Proof。
    /// # Errors
    /// Pending returns false. Failed/expired/stale proof retains the work holder and publishes nothing.
    pub fn finish_durability<
        const N: usize,
        const A: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const Q: usize,
        const S: usize,
    >(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        identity: &mut IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, A, R>,
        media: &mut PersistenceOwner<'_, D, Q, S>,
        now: u64,
    ) -> Result<bool> {
        self.current(adapter, now)?;
        self.exchange.io.now = now;
        if !core::ptr::eq(identity.callback_gate(), self.exchange.io.config.gate)
            || !core::ptr::eq(security.callback_gate(), self.exchange.io.config.gate)
        {
            return Err(Error::State);
        }
        let (w, p) = self.work.ok_or(Error::State)?;
        let view = media.request_get(p)?;
        if view.state == RequestState::Failed {
            return Err(view.terminal_error.unwrap_or(Error::State));
        }
        if view.state != RequestState::ProofReady {
            return Ok(false);
        }
        let proof = media.proof_get(p)?;
        match w {
            Work::Authority(h) => identity.activate_authority(h, p, proof, now)?,
            Work::Binding(h) => {
                identity.activate_binding(h, p, proof, now)?;
                self.binding = Some(h);
            }
            Work::Session(h) => {
                self.live_binding(identity, now)?;
                let c = self.session.ok_or(Error::State)?.1;
                security.activate(h, p, proof, facts(c, now))?;
            }
        }
        media.proof_retire(p)?;
        self.work = None;
        self.exchange.io.now = now;
        Ok(true)
    }
    /// Publish Offer/Commit only after this side's actual Binding reload proof.
    /// 仅在本端Binding真实回读证明完成后发布Offer/Commit。
    /// # Errors
    /// Any missing proof, changed proposal, phase, Authority or active physical holder rejects.
    pub fn publish<const N: usize, const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        identity: &IdentityOwner<'_, N>,
        provider: &mut impl CookieProvider,
        evidence: Evidence,
        now: u64,
    ) -> Result<()> {
        self.current(adapter, now)?;
        if self.work.is_some() || !self.exchange.next_ready() {
            return Err(Error::State);
        }
        let t = self.binding_transcript.ok_or(Error::State)?;
        // Before Offer the current transcript legitimately lacks the allocation group.
        self.check_identity(identity, now)?;
        let b = identity.binding_get(self.binding.ok_or(Error::State)?, now)?;
        if b.certificate() != certificate(&t, self.exchange.io.config.width)? {
            return Err(Error::Security);
        }
        let event = match self.exchange.side {
            Side::Authority
                if self.exchange.phase == Phase::DeviceVerified
                    && self.binding_evidence == Some(evidence) =>
            {
                Event::AddressOffer
            }
            Side::Device
                if self.exchange.phase == Phase::AddressOffered
                    && self.exchange.transcript == t =>
            {
                Event::DeviceCommit
            }
            _ => return Err(Error::State),
        };
        self.exchange
            .queue_event(adapter, provider, event, &t, evidence, now, false)
    }
    /// Authority sends Final only after Commit plus its live, exact durable allocation.
    /// Authority 仅在 Commit 及精确存活耐久分配成立后发送 Final。
    /// # Errors
    /// Caller booleans, remote assertions or unrelated Binding views cannot satisfy this entry.
    pub fn publish_final<const N: usize, const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, R, B>,
        identity: &IdentityOwner<'_, N>,
        provider: &mut impl CookieProvider,
        evidence: Evidence,
        now: u64,
    ) -> Result<()> {
        self.current(adapter, now)?;
        if self.exchange.side != Side::Authority
            || self.work.is_some()
            || self.exchange.phase != Phase::DeviceCommitted
            || !self.exchange.next_ready()
            || self.exchange.callbacks_left == 0
        {
            return Err(Error::State);
        }
        let binding = self.live_binding(identity, now)?;
        let t = self.exchange.transcript;
        // Validate the complete encoding before the irreversible Admission publication.
        // 不可逆Admission发布前完成完整编码预检。
        let mut bytes = [0; LOGICAL_MAX_BYTES];
        encode_logical_event(
            Event::FinalDurable,
            Phase::FinalDurable,
            &t,
            evidence,
            &mut bytes,
        )?;
        self.exchange.transition(Event::FinalDurable, &t)?;
        self.exchange.io.now = now;
        self.exchange.callbacks_left -= 1;
        let (owner, handle) = self.exchange.admission.as_mut().ok_or(Error::State)?;
        owner.admit_durable(provider, *handle, binding, t, evidence, now)?;
        self.exchange.queue_event(
            adapter,
            provider,
            Event::FinalDurable,
            &t,
            evidence,
            now,
            true,
        )
    }
    /// Derive and persist a Session from Final, never from an arbitrary caller candidate.
    /// 从Final派生并持久化Session，不接受任意调用方候选。
    /// # Errors
    /// Final not received/completed, stale Binding/Link, key-pair overflow or failed proof rejects.
    #[allow(clippy::too_many_arguments)]
    pub fn persist_session<
        const N: usize,
        const A: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const Q: usize,
        const S: usize,
    >(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        identity: &IdentityOwner<'_, N>,
        security: &mut SecurityOwner<'_, A, R>,
        provider: &mut impl CryptoProvider,
        media: &mut PersistenceOwner<'_, D, Q, S>,
        base: DurabilityBase,
        policy: JoinSessionPolicy,
        evidence: Evidence,
        now: u64,
    ) -> Result<SessionHandle> {
        self.current(adapter, now)?;
        if self.work.is_some()
            || self.session.is_some()
            || self.exchange.phase != Phase::FinalDurable
            || self.exchange.last_evidence != Some(evidence)
            || !core::ptr::eq(security.callback_gate(), self.exchange.io.config.gate)
        {
            return Err(Error::State);
        }
        if self.exchange.side == Side::Authority {
            let o = self.exchange.outgoing.as_ref().ok_or(Error::State)?;
            if o.rounds == 0
                && (o.index + 1 != o.count
                    || self.exchange.io.tx.as_ref().is_none_or(|t| {
                        t.token.is_some() || t.completion != Some(TerminalOutcome::Success)
                    }))
            {
                return Err(Error::State);
            }
        }
        let binding = self.live_binding(identity, now)?;
        self.exchange.io.now = now;
        let deadline = binding
            .local_deadline_us()
            .min(identity.authority_view(now)?.deadline());
        let candidate = join_session_candidate(
            self.exchange.transcript,
            self.exchange.io.config.width,
            self.exchange.side == Side::Device,
            self.exchange.io.config.link.instance_generation(),
            policy,
            self.freshness.started,
            deadline,
        )?;
        let (h, req) = security.prepare_static_session(
            provider,
            candidate,
            HandshakeProof {
                bytes: evidence.bytes(),
            },
            base,
            facts(candidate, now),
        )?;
        let p = match media.submit(&req.persistence_request(), now) {
            Ok(p) => p,
            Err(e) => {
                security.fence(h)?;
                security.retire_fenced(h)?;
                return Err(e);
            }
        };
        self.session = Some((h, candidate));
        self.work = Some((Work::Session(h), p));
        self.exchange.io.now = now;
        security.bind_persistence(h, p)?;
        Ok(h)
    }
    /// Local active Session only; it makes no claim about remote readiness.
    /// 仅表示本端活动Session，不声称对端已就绪。
    /// # Errors
    /// Pending durability, stopped root or any expired dependency rejects.
    pub fn session<
        const N: usize,
        const A: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &self,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, A, R>,
        now: u64,
    ) -> Result<SessionView> {
        if self.work.is_some()
            || self.exchange.io.stopped
            || self.exchange.io.failed
            || now < self.exchange.io.now
            || !core::ptr::eq(security.callback_gate(), self.exchange.io.config.gate)
        {
            return Err(Error::State);
        }
        self.exchange.io.current(adapter, now)?;
        self.live_binding(identity, now)?;
        let (h, c) = self.session.ok_or(Error::State)?;
        security.session_get(h, facts(c, now))
    }
    /// Transfer this exact live Session to the authenticated Capability Coordinator.
    /// 将本端精确存活Session交接给认证能力协商协调器；不构造调用方认证DTO。
    /// # Errors
    /// Active durability/TX, stale dependencies or insufficient MTU leaves JOIN usable;
    /// success irreversibly disables its network entry points, not the shared Session.
    #[cfg(feature = "capability-control")]
    #[allow(clippy::too_many_arguments)]
    pub fn handoff_capability<
        'handoff,
        const N: usize,
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &mut self,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        identity: &'handoff IdentityOwner<'_, N>,
        security: &SecurityOwner<'gate, S, R>,
        owner: u32,
        local: ucn_capability::CapabilityRecord,
        requirements: ucn_capability::ProfileRequirements,
        service: ucn_types::ServiceId,
        lifetime: u64,
        retry: u64,
        max_attempts: u16,
        capability_lease: u64,
        discovery_lease: u64,
        now: u64,
    ) -> Result<(crate::CapabilityRuntime<'handoff, B>, crate::JournalPeer)>
    where
        'gate: 'handoff,
    {
        if self.handed_off
            || self
                .exchange
                .io
                .tx
                .as_ref()
                .is_some_and(|v| v.token.is_some())
        {
            return Err(Error::State);
        }
        self.session(adapter, identity, security, now)?;
        let (session, c) = self.session.ok_or(Error::State)?;
        let peer = crate::JournalPeer {
            session,
            facts: facts(c, now),
            link: self.exchange.io.config.link,
            fingerprint: c.origin_fingerprint,
        };
        let mut root: crate::CapabilityRuntime<'handoff, B> = crate::CapabilityRuntime::new(
            owner,
            if self.exchange.side == Side::Device {
                crate::CapabilityRole::Initiator
            } else {
                crate::CapabilityRole::Responder
            },
            peer,
            security,
            adapter,
            local,
            requirements,
            service,
            lifetime,
            retry,
            max_attempts,
            capability_lease,
            discovery_lease,
        )?;
        root.bind_identity(identity, self.binding.ok_or(Error::State)?);
        self.handed_off = true;
        self.exchange.io.now = now;
        Ok((root, peer))
    }
    /// One bounded physical submit. 单次有界物理提交。
    /// # Errors
    /// Expiry/fault retains every physical holder.
    pub fn drive<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<bool> {
        self.current(adapter, now)?;
        self.exchange.drive(adapter, driver, now)
    }
    /// Explicit physical retirement. 显式物理退休。
    /// # Errors
    /// Uncertain completion/cancellation retains its token.
    pub fn retire<const L: usize, const T: usize, const R: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        driver: &mut impl TxDriver,
        now: u64,
    ) -> Result<()> {
        self.exchange.retire(adapter, driver, now)
    }
    /// Stop and fence the local Session; Foundation I/O remains owned until drained.
    /// 停止并Fence本端Session；Foundation I/O仍须完成退休。
    /// # Errors
    /// Stale Link/reentry refuses before stop or Fence.
    pub fn stop<const L: usize, const T: usize, const R: usize, const A: usize, const X: usize>(
        &mut self,
        adapter: &mut AdapterOwner<'_, L, T, R, B>,
        security: &mut SecurityOwner<'_, A, X>,
    ) -> Result<()> {
        if self.handed_off || !core::ptr::eq(security.callback_gate(), self.exchange.io.config.gate)
        {
            return Err(Error::State);
        }
        self.exchange.stop(adapter)?;
        if let Some((h, _)) = self.session {
            security.fence(h)?;
        }
        Ok(())
    }
    /// Retire a completed stopped request without publishing any volatile permission.
    /// 退休已完成的停止请求，不发布任何易失权限。
    /// # Errors
    /// Active root or still-running I/O preserves the holder.
    pub fn retire_stopped_durability<const D: usize, const Q: usize, const S: usize>(
        &mut self,
        media: &mut PersistenceOwner<'_, D, Q, S>,
    ) -> Result<()> {
        if !self.exchange.io.stopped || self.exchange.io.config.gate.is_active() {
            return Err(Error::State);
        }
        let (_, p) = self.work.ok_or(Error::State)?;
        match media.request_get(p)?.state {
            RequestState::ProofReady => media.proof_retire(p)?,
            RequestState::Failed => media.failed_retire(p)?,
            _ => return Err(Error::State),
        }
        self.work = None;
        Ok(())
    }
    /// Latest authenticated facts; 最新认证事实。
    #[must_use]
    pub const fn transcript(&self) -> BootstrapTranscript {
        self.exchange.transcript
    }
    /// Local phase, not remote availability; 本机阶段，不表示远端可用性。
    #[must_use]
    pub const fn phase(&self) -> Phase {
        self.exchange.phase
    }
}
fn authority(t: &BootstrapTranscript) -> Result<AuthorityEpoch> {
    Ok(AuthorityEpoch {
        realm: ucn_types::RealmId::new(t.realm_id)?,
        authority_principal: Principal::new(t.authority_principal)?,
        authority_generation: AddressAuthorityGeneration::new(t.authority_generation)?,
        durable_fence_token: t.durable_fence_token,
        lease_sequence: t.authority_lease_sequence,
        lease_duration_us: t.authority_lease_duration_us,
        allocation_high_water_digest: t.allocation_high_water_digest,
        quorum_config_digest: t.quorum_config_digest,
        signer_set_digest: t.signer_set_digest,
        threshold_proof_digest: t.threshold_proof_digest,
        signer_count: t.authority_signer_count,
        quorum_threshold: t.authority_quorum_threshold,
    })
}
fn certificate(
    t: &BootstrapTranscript,
    width: ucn_types::AddressWidth,
) -> Result<BindingCertificate> {
    Ok(BindingCertificate {
        binding: IdentityBinding {
            realm: ucn_types::RealmId::new(t.realm_id)?,
            address_width: width,
            address: NodeAddress::new(t.proposed_address, width)?,
            generation: BindingGeneration::active(t.address_binding_generation)?,
            principal: Principal::new(t.joining_device_principal)?,
        },
        authority_principal: Principal::new(t.authority_principal)?,
        authority_generation: AddressAuthorityGeneration::new(t.authority_generation)?,
        lease_id: t.binding_lease_id,
        lease_duration_us: t.binding_lease_duration_us,
        authority_lease_sequence: t.authority_lease_sequence,
        mode: match t.binding_mode {
            1 => ucn_identity::AddressMode::Static,
            2 => ucn_identity::AddressMode::Leased,
            3 => ucn_identity::AddressMode::SelfProposed,
            _ => return Err(Error::Malformed),
        },
    })
}
fn freshness(t: &BootstrapTranscript, for_authority: bool) -> Result<AuthorityFreshness> {
    Ok(AuthorityFreshness {
        verifier_principal: Principal::new(if for_authority {
            t.authority_principal
        } else {
            t.joining_device_principal
        })?,
        challenge_nonce: t.lease_freshness_challenge_nonce,
        transaction_id: t.transaction_id,
        authority_lease_sequence: t.authority_lease_sequence,
        max_remaining_lease_us: if for_authority {
            t.freshness_max_remaining_lease_us
        } else {
            t.freshness_max_remaining_lease_us
                .min(t.binding_lease_duration_us)
        },
        binding_lease_id: if for_authority {
            [0; 16]
        } else {
            t.binding_lease_id
        },
        binding_generation: if for_authority {
            0
        } else {
            t.address_binding_generation
        },
        proof_transcript_hash: t.freshness_proof_transcript_hash,
    })
}
fn facts(c: HandshakeCandidate, now: u64) -> CurrentFacts {
    CurrentFacts {
        local: c.local,
        peer: c.peer,
        link_generation: c.link_generation,
        policy_generation: c.policy_generation,
        now_us: now,
    }
}
// Key IDs name fixed directional pairs: base is Device->Authority, base+1 is reverse.
// Key ID表示固定方向槽对：基址是设备到Authority，基址+1是反方向。
/// Pure deterministic Session plan, not authorization or proof of durability.
/// 纯确定性Session计划，不授予权限，也不证明耐久性。
/// # Errors
/// Invalid canonical Final, policy, suite, exhausted directional pair or deadline rejects.
#[allow(clippy::too_many_arguments)]
pub fn join_session_candidate(
    t: BootstrapTranscript,
    width: ucn_types::AddressWidth,
    device: bool,
    local_link: u32,
    policy: JoinSessionPolicy,
    started: u64,
    dependency_deadline: u64,
) -> Result<HandshakeCandidate> {
    const DOMAIN: &[u8] = b"UCN6-JOIN-SESSION-V1";
    if policy.generation == 0
        || policy.lifetime_us == 0
        || started == 0
        || dependency_deadline <= started
        || !matches!(
            (t.selected_e2e_mode, t.selected_e2e_suite),
            (1, 1) | (2, 2 | 3)
        )
        || t.selected_hop_suite != 16
    {
        return Err(Error::Config);
    }
    let d = Binding {
        address: NodeAddress::new(t.proposed_address, width)?,
        generation: BindingGeneration::active(t.address_binding_generation)?,
        principal: t.joining_device_principal,
    };
    let a = Binding {
        address: NodeAddress::new(t.authority_address, width)?,
        generation: BindingGeneration::active(t.authority_binding_generation)?,
        principal: t.authority_principal,
    };
    let keys = |suite: u8, id: u16, generation: u32| -> Result<(KeySelector, KeySelector)> {
        let forward = KeySelector {
            suite: SuiteId::try_from(suite)?,
            key_id: KeyId::new(id)?,
            key_generation: generation,
        };
        let reverse = KeySelector {
            key_id: KeyId::new(id.checked_add(1).ok_or(Error::Exhausted)?)?,
            ..forward
        };
        Ok(if device {
            (forward, reverse)
        } else {
            (reverse, forward)
        })
    };
    let (origin_tx, origin_rx) = keys(
        t.selected_e2e_suite,
        t.selected_e2e_key_id,
        t.selected_e2e_key_generation,
    )?;
    let (tx, rx) = keys(
        t.selected_hop_suite,
        t.selected_hop_key_id,
        t.selected_hop_key_generation,
    )?;
    let (hop_profile, hop_tx, hop_rx) = (HopProfile::H1, Some(tx), Some(rx));
    let mut bytes = [0; DOMAIN.len() + 4 + ucn_admission::TRANSCRIPT_BYTES];
    bytes[..DOMAIN.len()].copy_from_slice(DOMAIN);
    bytes[DOMAIN.len()..DOMAIN.len() + 4].copy_from_slice(&379_u32.to_be_bytes());
    ucn_admission::encode_transcript(&t, Phase::FinalDurable, &mut bytes[DOMAIN.len() + 4..])?;
    let digest = ucn_digest::sha256(&bytes, &mut ucn_digest::DigestWorkspace::new());
    Ok(HandshakeCandidate {
        local: if device { d } else { a },
        peer: if device { a } else { d },
        link_generation: LinkInstanceGeneration::new(local_link)?,
        session_generation: PeerSessionGeneration::new(t.selected_session_generation)?,
        policy_generation: policy.generation,
        expires_at_us: dependency_deadline.min(
            started
                .checked_add(policy.lifetime_us)
                .ok_or(Error::Exhausted)?,
        ),
        origin_level: match t.selected_e2e_mode {
            1 => SecurityLevel::Authenticated,
            2 => SecurityLevel::Confidential,
            _ => return Err(Error::Security),
        },
        hop_profile,
        origin_tx,
        origin_rx,
        hop_tx,
        hop_rx,
        origin_fingerprint: Fingerprint::new(digest[..16].try_into().map_err(|_| Error::State)?)?,
        hop_fingerprint: Fingerprint::new(digest[16..].try_into().map_err(|_| Error::State)?)?,
        transcript_digest: digest,
    })
}
