//! Dedicated bound-pair reauthentication; no Admission or address allocation.
//! 已绑定身份对专属重新认证；不调用Admission、不分配地址。
use super::{BootstrapConfig, Io, Retry, Tx};
use crate::JournalPeer;
use ucn_adapter::{AdapterOwner, TerminalOutcome, TxDriver};
use ucn_identity::{AuthorityEpoch, BindingHandle, BindingView, IdentityOwner};
use ucn_owner::CallbackClaim;
use ucn_persistence::{PersistenceHandle, PersistenceOwner, RequestState};
use ucn_security::{
    Binding, CryptoProvider, CurrentFacts, DurabilityBase, HandshakeCandidate, HandshakeProof,
    PeerReauthHistory, PeerReauthProvider, PeerReauthRecord, PeerReauthSessionPlan,
    REAUTH_RECORD_BYTES, REAUTH_SIGN_DOMAIN, SecurityOwner, SessionHandle,
};
use ucn_types::{
    C0TransactionId, DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile,
    InteractionRole, OriginSecurity, PayloadKind, ProtocolOpcode, Result, TrafficClass,
};
use ucn_wire::{C0Address, C0Frame, CommonHeader, decode_c0_o0_h0, encode_c0_o0_h0};
const PREFIX: usize = 18;
const MAX_PROOF: usize = 64;
const LOGICAL: usize = REAUTH_RECORD_BYTES + 1 + MAX_PROOF;
#[cfg(feature = "capability-control")]
mod capability;
#[cfg(feature = "capability-control")]
pub use capability::ReauthCapabilityConfig;
trait Parents {
    fn check(
        &self,
        a: BindingHandle,
        b: BindingHandle,
        expected: [BindingView; 2],
        authority: AuthorityEpoch,
        now: u64,
    ) -> Result<()>;
}
impl<const N: usize> Parents for IdentityOwner<'_, N> {
    fn check(
        &self,
        a: BindingHandle,
        b: BindingHandle,
        expected: [BindingView; 2],
        authority: AuthorityEpoch,
        now: u64,
    ) -> Result<()> {
        if self.authority_get(now)? != authority
            || [self.binding_get(a, now)?, self.binding_get(b, now)?] != expected
        {
            return Err(Error::Security);
        }
        Ok(())
    }
}
/// Reauth lifecycle, not a remote readiness assertion. 重新认证阶段，不声称远端已就绪。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PeerReauthPhase {
    /// Initiator has not sent Hello; 发起方尚未发Hello。
    Idle,
    /// Responder awaits authenticated Hello; 等待认证Hello。
    AwaitHello,
    /// Initiator awaits challenge; 等待挑战。
    AwaitChallenge,
    /// Responder awaits response; 等待响应。
    AwaitResponse,
    /// Exact local Foundation proof pending; 等待本端Foundation精确证明。
    AwaitDurability,
    /// Initiator keeps Response until signed durable Accept; 保留响应直到收到耐久确认。
    AwaitAccept,
    /// Responder is durable, but Accept has not completed physical publication.
    /// 响应方已耐久，但ACCEPT物理发布尚未完成。
    PublishingAccept,
    /// Local durable Session and handshake condition hold; 本端耐久会话成立。
    Established,
    /// No new work; 停止新工作。
    Stopped,
}
/// Fixed known peer policy; no discovery table or implicit trust bootstrap.
/// 固定已知邻居策略；不产生发现表或隐式信任。
#[derive(Clone, Copy)]
pub struct PeerReauthPolicy {
    /// Initiator role; 是否发起方。
    pub initiator: bool,
    /// Fresh product nonce, captured before any proof; 证明前捕获的产品新鲜nonce。
    pub nonce: u64,
    /// Initiator transaction, responder must pass zero; 发起事务，响应方填零。
    pub transaction: u64,
    /// Physical peer hint only, never identity; 物理邻居提示，不是身份。
    pub peer_discriminator: u32,
    /// Fixed signature size 16..64; 固定签名长度。
    pub proof_bytes: usize,
    /// Current local policy generation; 当前本端策略代际。
    pub generation: u32,
    /// Maximum new Session duration, independent of handshake timeout.
    /// 新会话最长时长，独立于握手超时。
    pub session_lifetime_us: u64,
}
/// Fixed one-peer Coordinator, owns signatures, reassembly and physical obligations.
/// 固定单邻居协调器，持有签名、重组及物理义务。
pub struct PeerReauthRuntime<'a, 'gate, const B: usize> {
    io: Io<'gate, B>,
    identity: &'a dyn Parents,
    handles: [BindingHandle; 2],
    parents: [BindingView; 2],
    authority: AuthorityEpoch,
    policy: PeerReauthPolicy,
    base: DurabilityBase,
    record: PeerReauthRecord,
    phase: PeerReauthPhase,
    started: u64,
    deadline: u64,
    parent_deadline: u64,
    outgoing: [u8; LOGICAL],
    outgoing_stage: Option<u8>,
    part: u8,
    rounds: u16,
    due: u64,
    assembly: [u8; LOGICAL],
    assembly_stage: Option<u8>,
    assembly_next: u8,
    assembly_transaction: u64,
    proofs: [u8; 2 * MAX_PROOF],
    physical_attempts: u16,
    callback_generation: u32,
    callbacks: u16,
    work: Option<(SessionHandle, PersistenceHandle)>,
    session: Option<(SessionHandle, HandshakeCandidate)>,
    #[cfg(feature = "capability-control")]
    capability_config: Option<ReauthCapabilityConfig>,
    #[cfg(feature = "capability-control")]
    capability: Option<crate::CapabilityRuntime<'gate, B>>,
    #[cfg(feature = "capability-control")]
    capability_cursor: bool,
    #[cfg(feature = "capability-control")]
    capability_retire_cursor: bool,
    #[cfg(feature = "capability-control")]
    peer_ready: bool,
}
impl<'a, 'gate, const B: usize> PeerReauthRuntime<'a, 'gate, B> {
    /// Both Binding handles must be exact live Identity publications; old sessions/TX fenced/retired.
    /// 双Binding必须是实际Identity发布；旧会话及TX须先围栏、退休。
    /// # Errors
    /// Factory/no previous session, wrong parent, gate, profile, key pair or busy Adapter rejects.
    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub fn new<
        const N: usize,
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        config: BootstrapConfig<'gate>,
        policy: PeerReauthPolicy,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        identity: &'a IdentityOwner<'_, N>,
        local: BindingHandle,
        remote: BindingHandle,
        security: &SecurityOwner<'_, S, R>,
        base: DurabilityBase,
        now: u64,
    ) -> Result<Self> {
        if policy.nonce == 0
            || policy.peer_discriminator == 0
            || policy.generation == 0
            || policy.session_lifetime_us == 0
            || policy.session_lifetime_us > i64::MAX as u64
            || !(16..=MAX_PROOF).contains(&policy.proof_bytes)
            || policy.initiator != (policy.transaction != 0)
            || !core::ptr::eq(identity.callback_gate(), config.gate)
            || !core::ptr::eq(security.callback_gate(), config.gate)
            || !adapter.tx_is_quiescent()
            || !adapter.rx_is_quiescent()
            || config.realm != security.realm()
            || config.width != security.address_width()
            || config.max_attempts < 4
        {
            return Err(Error::Config);
        }
        let prior = base.previous_session().ok_or(Error::State)?;
        // Directional key slots are frozen by the existing identity-pair policy.
        // 定向密钥槽由既有身份对策略冻结，不能在重新认证时交换角色。
        if (prior.origin_tx.key_id.get() < prior.origin_rx.key_id.get()) != policy.initiator
            || (prior.hop_tx.ok_or(Error::Config)?.key_id.get()
                < prior.hop_rx.ok_or(Error::Config)?.key_id.get())
                != policy.initiator
        {
            return Err(Error::Config);
        }
        if !security.peer_is_fenced(prior.local, prior.peer) {
            return Err(Error::State);
        }
        let parents = [
            identity.binding_get(local, now)?,
            identity.binding_get(remote, now)?,
        ];
        let bind = |v: BindingView| {
            let c = v.certificate().binding;
            Binding {
                address: c.address,
                generation: c.generation,
                principal: c.principal.bytes(),
            }
        };
        if bind(parents[0]) != prior.local || bind(parents[1]) != prior.peer {
            return Err(Error::Security);
        }
        let authority = identity.authority_get(now)?;
        if parents.iter().any(|v| {
            let c = v.certificate();
            c.binding.realm != config.realm
                || c.authority_generation != authority.authority_generation
                || c.authority_principal != authority.authority_principal
        }) {
            return Err(Error::Security);
        }
        let parent_deadline = parents[0]
            .local_deadline_us()
            .min(parents[1].local_deadline_us())
            .min(identity.authority_view(now)?.deadline());
        let deadline = now
            .checked_add(config.lifetime_us)
            .ok_or(Error::Exhausted)?
            .min(parent_deadline)
            .min(base.absolute_deadline_us);
        if deadline <= now {
            return Err(Error::Timeout);
        }
        let (a, b) = if policy.initiator {
            (prior.local, prior.peer)
        } else {
            (prior.peer, prior.local)
        };
        let pair =
            |x: ucn_security::KeySelector, y: ucn_security::KeySelector| -> Result<(u16, u32)> {
                let id = x.key_id.get().min(y.key_id.get());
                if x.suite != y.suite
                    || x.key_generation != y.key_generation
                    || x.key_id.get().max(y.key_id.get())
                        != id.checked_add(1).ok_or(Error::Exhausted)?
                {
                    return Err(Error::Config);
                }
                Ok((id, x.key_generation.checked_add(1).ok_or(Error::Exhausted)?))
            };
        let (origin_key, origin_generation) = pair(prior.origin_tx, prior.origin_rx)?;
        let (hop_key, hop_generation) = pair(
            prior.hop_tx.ok_or(Error::Config)?,
            prior.hop_rx.ok_or(Error::Config)?,
        )?;
        let local_link = (
            config.link.slot().checked_add(1).ok_or(Error::Exhausted)?,
            config.link.instance_generation(),
        );
        let record = PeerReauthRecord {
            stage: 0,
            width: config.width,
            realm: config.realm,
            initiator: a,
            responder: b,
            previous_session: prior.session_generation.get(),
            next_session: prior
                .session_generation
                .get()
                .checked_add(1)
                .ok_or(Error::Exhausted)?,
            origin_suite: prior.origin_tx.suite as u8,
            origin_key,
            hop_key,
            origin_generation,
            hop_generation,
            initiator_link: if policy.initiator { local_link } else { (0, 0) },
            responder_link: (0, 0),
            transaction: policy.transaction,
            initiator_nonce: if policy.initiator { policy.nonce } else { 0 },
            responder_nonce: 0,
            lifetime_us: policy.session_lifetime_us,
            previous_digest: prior.transcript_digest,
            initiator_keys: (
                prior.origin_tx.key_generation,
                prior.hop_tx.ok_or(Error::Config)?.key_generation,
            ),
            responder_history: PeerReauthHistory::EMPTY,
        };
        let callbacks = config.max_attempts.checked_mul(4).ok_or(Error::Config)?;
        let io = Io::new(config, adapter, now)?;
        Ok(Self {
            io,
            identity,
            handles: [local, remote],
            parents,
            authority,
            policy,
            base,
            record,
            phase: if policy.initiator {
                PeerReauthPhase::Idle
            } else {
                PeerReauthPhase::AwaitHello
            },
            started: now,
            deadline,
            parent_deadline,
            outgoing: [0; LOGICAL],
            outgoing_stage: None,
            part: 0,
            rounds: 0,
            due: now,
            assembly: [0; LOGICAL],
            assembly_stage: None,
            assembly_next: 0,
            assembly_transaction: 0,
            proofs: [0; 2 * MAX_PROOF],
            physical_attempts: 0,
            callback_generation: 1,
            callbacks,
            work: None,
            session: None,
            #[cfg(feature = "capability-control")]
            capability_config: None,
            #[cfg(feature = "capability-control")]
            capability: None,
            #[cfg(feature = "capability-control")]
            capability_cursor: false,
            #[cfg(feature = "capability-control")]
            capability_retire_cursor: false,
            #[cfg(feature = "capability-control")]
            peer_ready: false,
        })
    }
    fn current<const L: usize, const T: usize, const X: usize>(
        &self,
        a: &AdapterOwner<'_, L, T, X, B>,
        now: u64,
    ) -> Result<()> {
        self.io.current(a, now)?;
        if now >= self.deadline {
            return Err(Error::Timeout);
        }
        self.identity.check(
            self.handles[0],
            self.handles[1],
            self.parents,
            self.authority,
            now,
        )
    }
    fn sign_message(
        record: PeerReauthRecord,
    ) -> Result<[u8; REAUTH_RECORD_BYTES + REAUTH_SIGN_DOMAIN.len()]> {
        let mut bytes = [0; REAUTH_RECORD_BYTES + REAUTH_SIGN_DOMAIN.len()];
        bytes[..REAUTH_SIGN_DOMAIN.len()].copy_from_slice(REAUTH_SIGN_DOMAIN);
        record.encode(&mut bytes[REAUTH_SIGN_DOMAIN.len()..])?;
        Ok(bytes)
    }
    fn callback(&mut self) -> Result<ucn_owner::CallbackLease<'gate>> {
        if self.callbacks == 0 {
            return Err(Error::Exhausted);
        }
        let next = self
            .callback_generation
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        let claim = CallbackClaim::new(self.io.config.owner, self.callback_generation, 1, 0x0618)?;
        let lease = self.io.config.gate.try_enter(claim)?;
        self.callback_generation = next;
        self.callbacks -= 1;
        Ok(lease)
    }
    fn queue(&mut self, provider: &mut impl PeerReauthProvider, stage: u8, now: u64) -> Result<()> {
        if self.io.tx.as_ref().is_some_and(|t| t.token.is_some()) {
            return Err(Error::State);
        }
        let r = PeerReauthRecord {
            stage,
            ..self.record
        };
        let message = Self::sign_message(r)?;
        let principal = if stage == 0 || stage == 2 {
            r.initiator.principal
        } else {
            r.responder.principal
        };
        let mut proof = [0; MAX_PROOF];
        let lease = self.callback()?;
        let result =
            provider.sign_reauth(principal, &message, &mut proof[..self.policy.proof_bytes]);
        self.io.config.gate.leave(lease)?;
        if result? != self.policy.proof_bytes {
            return Err(Error::Security);
        }
        r.encode(&mut self.outgoing[..REAUTH_RECORD_BYTES])?;
        self.outgoing[REAUTH_RECORD_BYTES] =
            u8::try_from(self.policy.proof_bytes).map_err(|_| Error::Config)?;
        self.outgoing[REAUTH_RECORD_BYTES + 1..REAUTH_RECORD_BYTES + 1 + self.policy.proof_bytes]
            .copy_from_slice(&proof[..self.policy.proof_bytes]);
        if stage == 1 || stage == 2 {
            self.store_proof(stage, &proof[..self.policy.proof_bytes]);
        }
        self.outgoing_stage = Some(stage);
        self.record = r;
        self.physical_attempts = 0;
        self.part = 0;
        self.rounds = 0;
        self.due = now;
        self.io.tx = None;
        Ok(())
    }
    fn store_proof(&mut self, stage: u8, proof: &[u8]) {
        let offset = if stage == 1 {
            0
        } else {
            self.policy.proof_bytes
        };
        self.proofs[offset..offset + self.policy.proof_bytes].copy_from_slice(proof);
    }
    /// Start one immutable Hello; 开始唯一不可变Hello。
    /// # Errors
    /// Non-idle, stale parents or signature failure rejects.
    pub fn start<const L: usize, const T: usize, const X: usize>(
        &mut self,
        a: &AdapterOwner<'_, L, T, X, B>,
        p: &mut impl PeerReauthProvider,
        now: u64,
    ) -> Result<()> {
        self.current(a, now)?;
        if self.phase != PeerReauthPhase::Idle {
            return Err(Error::State);
        }
        self.io.now = now;
        self.queue(p, 0, now)?;
        self.phase = PeerReauthPhase::AwaitChallenge;
        self.io.now = now;
        Ok(())
    }
    fn expected(&self, mut r: PeerReauthRecord) -> bool {
        let mut expected = self.record;
        // First Hello supplies only the initiator history. Challenge adds the
        // responder history; later messages must match both histories exactly.
        // Hello只提供发起方历史；Challenge添加响应方历史，之后逐字段冻结。
        if self.phase == PeerReauthPhase::AwaitHello {
            return r.stage == 0
                && r.width == expected.width
                && r.realm == expected.realm
                && r.initiator == expected.initiator
                && r.responder == expected.responder
                && r.origin_suite == expected.origin_suite
                && r.origin_key == expected.origin_key
                && r.hop_key == expected.hop_key
                && r.lifetime_us == expected.lifetime_us;
        }
        if self.phase == PeerReauthPhase::AwaitChallenge || r.stage == 0 {
            for v in [&mut r, &mut expected] {
                v.next_session = v.previous_session.saturating_add(1);
                v.origin_generation = v.initiator_keys.0.saturating_add(1);
                v.hop_generation = v.initiator_keys.1.saturating_add(1);
                v.responder_history = PeerReauthHistory::EMPTY;
            }
        }
        r.stage = 0;
        expected.stage = 0;
        r.responder_link = (0, 0);
        r.responder_nonce = 0;
        expected.responder_link = (0, 0);
        expected.responder_nonce = 0;
        if self.phase == PeerReauthPhase::AwaitHello {
            expected.initiator_link = r.initiator_link;
            expected.initiator_nonce = r.initiator_nonce;
            expected.transaction = r.transaction;
        }
        r == expected
    }
    /// One actual atomic frame, full reassembly and identity signature before state advance.
    /// 单次实际原子帧，完整重组及身份签名后推进。
    /// # Errors
    /// Wrong role/binding/link/sequence/nonce/proof or deadline rejects; no lease renewal.
    #[allow(clippy::too_many_lines, clippy::many_single_char_names)] // Canonical frame parser, bounded local fields.
    pub fn receive<const L: usize, const T: usize, const X: usize>(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
        p: &mut impl PeerReauthProvider,
        now: u64,
    ) -> Result<bool> {
        self.current(a, now)?;
        let peer = self.io.claim_raw(a, now)?;
        self.receive_staged(peer, p, now)
    }
    #[allow(clippy::too_many_lines)] // One bounded canonical decode and authenticated FSM transition.
    fn receive_staged(
        &mut self,
        peer: u32,
        p: &mut impl PeerReauthProvider,
        now: u64,
    ) -> Result<bool> {
        let f = decode_c0_o0_h0(
            &self.io.raw[..self.io.raw_length],
            self.io.config.width,
            HopProfile::H0,
        )?;
        let stage = stage(f.opcode)?;
        let (source, dest) = if stage == 0 || stage == 2 {
            (self.record.initiator, self.record.responder)
        } else {
            (self.record.responder, self.record.initiator)
        };
        if peer != self.policy.peer_discriminator
            || f.realm_id != self.record.realm
            || f.source != C0Address::Bound(source.address)
            || f.destination != C0Address::Bound(dest.address)
            || f.source_binding_generation != source.generation
            || f.destination_binding_generation != dest.generation
            || f.common.traffic_class != TrafficClass::Q2
            || f.common.hop_limit.get() != 1
            || f.common.delivery != DeliveryGuarantee::BestEffort
            || f.common.payload_kind != PayloadKind::Control
            || f.common.interaction != role(stage)
            || self.policy.initiator == (stage == 0 || stage == 2)
        {
            return Err(Error::Security);
        }
        let fragment = ucn_security::decode_peer_reauth_fragment(f.payload)?;
        let n = REAUTH_RECORD_BYTES + 1 + self.policy.proof_bytes;
        let chunk = chunk();
        if fragment.stage != stage
            || usize::from(fragment.total_bytes) != n
            || fragment.transaction != f.transaction_id.get()
            || (self.phase != PeerReauthPhase::AwaitHello
                && f.transaction_id.get() != self.record.transaction)
        {
            return Err(Error::Malformed);
        }
        let index = usize::from(fragment.index);
        let offset = index * chunk;
        if index == 0 {
            self.assembly_next = 0;
            self.assembly_stage = Some(stage);
            self.assembly_transaction = f.transaction_id.get();
        }
        if self.assembly_stage != Some(stage)
            || index != usize::from(self.assembly_next)
            || self.assembly_transaction != f.transaction_id.get()
        {
            return Err(Error::State);
        }
        self.assembly[offset..offset + fragment.data.len()].copy_from_slice(fragment.data);
        self.assembly_next += 1;
        if index + 1 != n.div_ceil(chunk) {
            return Ok(false);
        }
        self.assembly_stage = None;
        let r = PeerReauthRecord::decode(&self.assembly[..REAUTH_RECORD_BYTES])?;
        if r.stage != stage
            || !self.expected(r)
            || r.transaction != f.transaction_id.get()
            || self.assembly[REAUTH_RECORD_BYTES]
                != u8::try_from(self.policy.proof_bytes).map_err(|_| Error::Config)?
        {
            return Err(Error::Security);
        }
        if stage != 0
            && !self.policy.initiator
            && (r.responder_nonce != self.policy.nonce
                || r.responder_link
                    != (
                        self.io.config.link.slot() + 1,
                        self.io.config.link.instance_generation(),
                    ))
        {
            return Err(Error::Security);
        }
        if stage == 3
            && (r.responder_nonce != self.record.responder_nonce
                || r.responder_link != self.record.responder_link)
        {
            return Err(Error::Security);
        }
        let message = Self::sign_message(r)?;
        let mut proof = [0; MAX_PROOF];
        proof[..self.policy.proof_bytes]
            .copy_from_slice(&self.assembly[REAUTH_RECORD_BYTES + 1..n]);
        let lease = self.callback()?;
        let result = p.verify_reauth(
            source.principal,
            &message,
            &proof[..self.policy.proof_bytes],
        );
        self.io.config.gate.leave(lease)?;
        result?;
        // Do not discard a real TX just because the peer already answered.
        // 不能因对端提前回复便丢弃真实TX。
        if self.io.tx.as_ref().is_some_and(|t| t.token.is_some()) {
            return Err(Error::State);
        }
        match (self.phase, stage) {
            (PeerReauthPhase::AwaitHello, 0) => {
                let original = self.record;
                let mut next = r;
                let prior = self.base.previous_session().ok_or(Error::State)?;
                next.responder_history = PeerReauthHistory {
                    session: prior.session_generation.get(),
                    origin: prior.origin_tx.key_generation,
                    hop: prior.hop_tx.ok_or(Error::Config)?.key_generation,
                    digest: prior.transcript_digest,
                };
                next.next_session = r
                    .previous_session
                    .max(prior.session_generation.get())
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?;
                next.origin_generation = r
                    .initiator_keys
                    .0
                    .max(prior.origin_tx.key_generation)
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?;
                next.hop_generation = r
                    .initiator_keys
                    .1
                    .max(prior.hop_tx.ok_or(Error::Config)?.key_generation)
                    .checked_add(1)
                    .ok_or(Error::Exhausted)?;
                next.responder_nonce = self.policy.nonce;
                next.responder_link = (
                    self.io.config.link.slot() + 1,
                    self.io.config.link.instance_generation(),
                );
                next.stage = 1;
                next.validate()?;
                self.record = next;
                if let Err(e) = self.queue(p, 1, now) {
                    self.record = original;
                    return Err(e);
                }
                self.phase = PeerReauthPhase::AwaitResponse;
            }
            (PeerReauthPhase::AwaitChallenge, 1) => {
                self.record = r;
                self.store_proof(1, &proof[..self.policy.proof_bytes]);
                self.queue(p, 2, now)?;
                self.phase = PeerReauthPhase::AwaitDurability;
            }
            (PeerReauthPhase::AwaitResponse, 2) => {
                self.store_proof(2, &proof[..self.policy.proof_bytes]);
                self.record = r;
                self.outgoing_stage = None;
                self.io.tx = None;
                self.phase = PeerReauthPhase::AwaitDurability;
            }
            (PeerReauthPhase::AwaitAccept, 3) => {
                self.record = r;
                self.outgoing_stage = None;
                self.io.tx = None;
                self.phase = PeerReauthPhase::Established;
            }
            (PeerReauthPhase::AwaitResponse, 0)
            | (PeerReauthPhase::PublishingAccept | PeerReauthPhase::Established, 2) => {
                // Exact signed duplicates only re-arm the frozen cached response.
                self.part = 0;
                self.due = now;
            }
            _ => return Err(Error::State),
        }
        self.io.now = now;
        Ok(true)
    }
    /// Prepare exact successor Session and submit its immutable durability requirement.
    /// 准备精确后继Session并提交不可变耐久要求。
    /// # Errors
    /// Missing mutual proof, stale parent/baseline, crypto rejection or no capacity rejects.
    #[allow(clippy::too_many_arguments, clippy::many_single_char_names)] // Independently owned durability participants.
    pub fn persist<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const Q: usize,
        const Z: usize,
    >(
        &mut self,
        a: &AdapterOwner<'_, L, T, X, B>,
        security: &mut SecurityOwner<'_, S, R>,
        crypto: &mut impl CryptoProvider,
        signer: &mut impl PeerReauthProvider,
        media: &mut PersistenceOwner<'_, D, Q, Z>,
        now: u64,
    ) -> Result<()> {
        self.current(a, now)?;
        if self.phase != PeerReauthPhase::AwaitDurability
            || self.work.is_some()
            || self.session.is_some()
            || !core::ptr::eq(security.callback_gate(), self.io.config.gate)
        {
            return Err(Error::State);
        }
        let c = self.record.candidate(
            self.policy.initiator,
            self.policy.generation,
            self.started,
            self.parent_deadline,
        )?;
        self.io.now = now;
        let n = 2 * self.policy.proof_bytes;
        let (h, req) = security.prepare_peer_reauth_session(
            crypto,
            signer,
            PeerReauthSessionPlan {
                record: self.record,
                initiator: self.policy.initiator,
                policy: self.policy.generation,
                started: self.started,
                parent_deadline: self.parent_deadline,
            },
            HandshakeProof {
                bytes: &self.proofs[..n],
            },
            self.base,
            facts(c, now),
        )?;
        let ph = match media.submit(&req.persistence_request(), now) {
            Ok(v) => v,
            Err(e) => {
                security.fence(h)?;
                security.retire_fenced(h)?;
                return Err(e);
            }
        };
        self.session = Some((h, c));
        self.work = Some((h, ph));
        security.bind_persistence(h, ph)?;
        self.io.now = now;
        Ok(())
    }
    /// Activate only exact actual reload proof; Initiator still waits for durable Accept.
    /// 只使用实际精确回读证明激活；发起方仍须等待耐久Accept。
    /// # Errors
    /// Pending returns false; expired parents or failed proof publish nothing.
    #[allow(clippy::many_single_char_names)] // Exact opaque handles and candidate are local only.
    pub fn finish_durability<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const D: usize,
        const Q: usize,
        const Z: usize,
    >(
        &mut self,
        a: &AdapterOwner<'_, L, T, X, B>,
        security: &mut SecurityOwner<'_, S, R>,
        media: &mut PersistenceOwner<'_, D, Q, Z>,
        now: u64,
    ) -> Result<bool> {
        self.current(a, now)?;
        if !core::ptr::eq(security.callback_gate(), self.io.config.gate) {
            return Err(Error::State);
        }
        let (h, p) = self.work.ok_or(Error::State)?;
        self.io.now = now;
        let v = media.request_get(p)?;
        if v.state == RequestState::Failed {
            return Err(v.terminal_error.unwrap_or(Error::State));
        }
        if v.state != RequestState::ProofReady {
            return Ok(false);
        }
        let c = self.session.ok_or(Error::State)?.1;
        security.activate(h, p, media.proof_get(p)?, facts(c, now))?;
        media.proof_retire(p)?;
        self.work = None;
        self.phase = if self.policy.initiator {
            PeerReauthPhase::AwaitAccept
        } else {
            PeerReauthPhase::PublishingAccept
        };
        self.io.now = now;
        Ok(true)
    }
    /// One bounded physical submission; signatures and fragments are frozen across retries.
    /// 单次有界物理提交；重试冻结签名及分片。
    /// # Errors
    /// Deadline, budget, signature or Driver uncertainty keeps every holder.
    #[allow(clippy::too_many_lines)] // One preflight-to-physical-submit transition; no hidden loops.
    pub fn drive<const L: usize, const T: usize, const X: usize>(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
        p: &mut impl PeerReauthProvider,
        d: &mut impl TxDriver,
        now: u64,
    ) -> Result<bool> {
        self.current(a, now)?;
        self.io.now = now;
        if self.phase == PeerReauthPhase::AwaitDurability {
            return Ok(false);
        }
        if !self.policy.initiator
            && self.phase == PeerReauthPhase::PublishingAccept
            && self.outgoing_stage != Some(3)
        {
            self.queue(p, 3, now)?;
        }
        let Some(stage) = self.outgoing_stage else {
            return Ok(false);
        };
        if self.io.tx.as_ref().is_some_and(|t| t.token.is_some()) {
            return Ok(false);
        }
        let n = REAUTH_RECORD_BYTES + 1 + self.policy.proof_bytes;
        let chunk = chunk();
        let count = n.div_ceil(chunk);
        if self
            .io
            .tx
            .as_ref()
            .is_some_and(|t| t.completion == Some(TerminalOutcome::Success) && !t.ready)
        {
            self.io.tx = None;
            self.part += 1;
            if usize::from(self.part) == count {
                self.part = 0;
                self.rounds += 1;
                if self.phase == PeerReauthPhase::PublishingAccept {
                    self.phase = PeerReauthPhase::Established;
                }
                self.due = now
                    .checked_add(self.io.config.retry_us)
                    .ok_or(Error::Exhausted)?;
            }
        }
        if self.rounds >= self.io.config.max_attempts {
            self.io.failed = true;
            return Err(Error::Exhausted);
        }
        if now < self.due {
            return Ok(false);
        }
        if self.io.tx.is_none() {
            let offset = usize::from(self.part) * chunk;
            let len = (n - offset).min(chunk);
            let mut body = [0; 98];
            let body_bytes = ucn_security::encode_peer_reauth_fragment(
                ucn_security::PeerReauthFragment {
                    stage,
                    index: self.part,
                    total_bytes: u16::try_from(n).map_err(|_| Error::Config)?,
                    transaction: self.record.transaction,
                    data: &self.outgoing[offset..offset + len],
                },
                &mut body,
            )?;
            let (source, destination) = if stage == 0 || stage == 2 {
                (self.record.initiator, self.record.responder)
            } else {
                (self.record.responder, self.record.initiator)
            };
            let length = encode_c0_o0_h0(
                &C0Frame {
                    common: CommonHeader {
                        contract: HeaderContract::C0,
                        origin_security: OriginSecurity::O0,
                        delivery: DeliveryGuarantee::BestEffort,
                        interaction: role(stage),
                        payload_kind: PayloadKind::Control,
                        traffic_class: TrafficClass::Q2,
                        hop_limit: HopLimit::new(1)?,
                    },
                    realm_id: self.record.realm,
                    source: C0Address::Bound(source.address),
                    destination: C0Address::Bound(destination.address),
                    source_binding_generation: source.generation,
                    destination_binding_generation: destination.generation,
                    transaction_id: C0TransactionId::new(self.record.transaction)?,
                    opcode: opcode(stage)?,
                    payload: &body[..body_bytes],
                },
                self.io.config.width,
                HopProfile::H0,
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
        if self
            .io
            .tx
            .as_ref()
            .is_some_and(|tx| tx.ready && tx.token.is_none() && now >= tx.due)
        {
            if self.physical_attempts >= self.io.config.max_attempts {
                self.io.failed = true;
                return Err(Error::Exhausted);
            }
            self.physical_attempts += 1;
        }
        self.io.drive(a, d, now)
    }
    /// Current local established Session, never a forged DTO or a remote readiness promise.
    /// 当前本端已建立会话，不伪造DTO、不保证对端已启用。
    /// # Errors
    /// Handshake, durability, parent, Link or Security state mismatch rejects.
    pub fn session<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &self,
        a: &AdapterOwner<'_, L, T, X, B>,
        s: &SecurityOwner<'_, S, R>,
        now: u64,
    ) -> Result<JournalPeer> {
        if self.phase != PeerReauthPhase::Established
            || self.work.is_some()
            || !core::ptr::eq(s.callback_gate(), self.io.config.gate)
            || (!self.policy.initiator && self.outgoing_stage != Some(3))
            || self.io.config.gate.is_active()
            || self.io.stopped
            || now == 0
            || now < self.io.now
        {
            return Err(Error::State);
        }
        // Transport retry exhaustion/handshake timeout cannot undo durable publication.
        // 传输重试耗尽或握手到期不能撤销已发布的耐久结果；当前父域/会话仍必须存活。
        a.link_mtu(self.io.config.link)?;
        self.identity.check(
            self.handles[0],
            self.handles[1],
            self.parents,
            self.authority,
            now,
        )?;
        let (session, c) = self.session.ok_or(Error::State)?;
        s.session_get(session, facts(c, now))?;
        Ok(JournalPeer {
            session,
            facts: facts(c, now),
            link: self.io.config.link,
            fingerprint: c.origin_fingerprint,
        })
    }
    /// Retire one real physical obligation. 退休一次实际物理义务。
    /// # Errors
    /// Unknown/cancellation failures retain exact Token.
    pub fn retire<const L: usize, const T: usize, const X: usize>(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
        d: &mut impl TxDriver,
        now: u64,
    ) -> Result<()> {
        self.io.retire(a, d, now)
    }
    /// Stop and fence the new Session, never cancel by dropping a holder.
    /// 停止并围栏新会话，不能通过丢弃Holder取消。
    /// # Errors
    /// Wrong gate or Adapter rejects before changes.
    pub fn stop<const S: usize, const R: usize, const L: usize, const T: usize, const X: usize>(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
        s: &mut SecurityOwner<'_, S, R>,
    ) -> Result<()> {
        if !core::ptr::eq(s.callback_gate(), self.io.config.gate) {
            return Err(Error::State);
        }
        // Validate the physical generation before changing either lane.
        // 修改任何通道前，先核对实际物理代际；旧Root不能关闭新Link。
        if a.link_handle(usize::from(self.io.config.link.slot()))? != self.io.config.link {
            return Err(Error::NotFound);
        }
        #[cfg(feature = "capability-control")]
        if let Some(cap) = self.capability.as_mut() {
            cap.stop(a)?;
        }
        self.io.stop(a)?;
        self.phase = PeerReauthPhase::Stopped;
        if let Some((h, _)) = self.session {
            s.fence(h)?;
        }
        Ok(())
    }
    /// Retire stopped durability only after a true terminal Provider result.
    /// 仅在真实Provider终态后退休停止的持久化义务。
    /// # Errors
    /// Non-stopped/pending work retains its holder.
    pub fn retire_stopped_durability<const D: usize, const Q: usize, const Z: usize>(
        &mut self,
        p: &mut PersistenceOwner<'_, D, Q, Z>,
    ) -> Result<()> {
        if self.phase != PeerReauthPhase::Stopped || self.io.config.gate.is_active() {
            return Err(Error::State);
        }
        if let Some((_, h)) = self.work {
            match p.request_get(h)?.state {
                RequestState::ProofReady => p.proof_retire(h)?,
                RequestState::Failed => p.failed_retire(h)?,
                _ => return Err(Error::State),
            }
            self.work = None;
        }
        Ok(())
    }
    /// Local phase only; 仅本端阶段。
    #[must_use]
    pub const fn phase(&self) -> PeerReauthPhase {
        self.phase
    }
    /// Exact frozen record for product signing/auditing, not authentication evidence.
    /// 产品签名/审计用精确只读正文，不是认证凭据。
    #[must_use]
    pub const fn record(&self) -> PeerReauthRecord {
        self.record
    }
    /// Physical and durability quiescence; 物理与耐久义务收口。
    #[must_use]
    pub fn quiescent<const L: usize, const T: usize, const X: usize>(
        &self,
        a: &AdapterOwner<'_, L, T, X, B>,
    ) -> bool {
        let quiet =
            self.phase == PeerReauthPhase::Stopped && self.work.is_none() && self.io.quiet(a);
        #[cfg(feature = "capability-control")]
        if let Some(cap) = &self.capability {
            return quiet && cap.quiescent(a);
        }
        quiet
    }
}
fn chunk() -> usize {
    98 - PREFIX
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
fn opcode(stage: u8) -> Result<ProtocolOpcode> {
    match stage {
        0 => Ok(ProtocolOpcode::PeerReauthHello),
        1 => Ok(ProtocolOpcode::PeerReauthChallenge),
        2 => Ok(ProtocolOpcode::PeerReauthResponse),
        3 => Ok(ProtocolOpcode::PeerSessionAccept),
        _ => Err(Error::Malformed),
    }
}
fn stage(op: ProtocolOpcode) -> Result<u8> {
    match op {
        ProtocolOpcode::PeerReauthHello => Ok(0),
        ProtocolOpcode::PeerReauthChallenge => Ok(1),
        ProtocolOpcode::PeerReauthResponse => Ok(2),
        ProtocolOpcode::PeerSessionAccept => Ok(3),
        _ => Err(Error::Unsupported),
    }
}
fn role(stage: u8) -> InteractionRole {
    if stage == 0 || stage == 2 {
        InteractionRole::Request
    } else {
        InteractionRole::Result
    }
}
