//! One private Coordinator demultiplexes real RX and retains final-response obligations.
//! 唯一私有协调器分派真实RX并保留最后确认义务；不合成认证凭据。
use super::{PeerReauthPhase, PeerReauthRuntime};
use crate::{CapabilityPhase, CapabilityRole, CapabilityRuntime};
use ucn_adapter::{AdapterOwner, TxDriver};
use ucn_capability::{CapabilityRecord, ProfileRequirements, ProfileSelect};
use ucn_security::{C0TransactionOwner, HopSequenceOwner, SecurityPacketWorkspace};
use ucn_security::{CryptoProvider, PeerReauthProvider, SecurityOwner};
use ucn_types::ServiceId;
use ucn_types::{Error, Result};

/// Immutable explicit policy; no inherited or widened ACL.
/// 不可变显式策略，不继承或扩大旧ACL。
#[derive(Clone, Copy)]
pub struct ReauthCapabilityConfig {
    /// Unique capability Coordinator owner; 能力协调器唯一Owner。
    pub owner: u32,
    /// Exact local capability; 精确本端能力。
    pub local: CapabilityRecord,
    /// Product requirements; 产品要求。
    pub requirements: ProfileRequirements,
    /// ACL service; ACL服务域。
    pub service: ServiceId,
    /// Negotiation timeout; 协商超时。
    pub lifetime_us: u64,
    /// Retry interval; 重试间隔。
    pub retry_us: u64,
    /// Fixed attempts; 固定次数。
    pub max_attempts: u16,
    /// Capability lease; 能力租期。
    pub capability_lease_us: u64,
    /// Discovery lease; 发现租期。
    pub discovery_lease_us: u64,
}
impl<'gate, const B: usize> PeerReauthRuntime<'_, 'gate, B> {
    /// Arm before network start; actual handoff waits for reload and physical retirement.
    /// 网络开始前配置，真正交接等待回读成功及物理退休。
    /// # Errors
    /// Duplicate/started/stopped or malformed policy rejects without changes.
    pub fn enable_capability(&mut self, c: ReauthCapabilityConfig) -> Result<()> {
        if self.io.config.gate.is_active()
            || self.capability_config.is_some()
            || !matches!(
                self.phase,
                PeerReauthPhase::Idle | PeerReauthPhase::AwaitHello
            )
            || self.assembly_next != 0
            || c.owner == 0
            || c.owner.checked_add(2).is_none()
            || c.lifetime_us == 0
            || c.lifetime_us > i64::MAX as u64
            || c.retry_us == 0
            || c.retry_us > c.lifetime_us
            || c.max_attempts < 2
        {
            return Err(Error::Config);
        }
        self.capability_config = Some(c);
        Ok(())
    }
    fn advance_capability<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &mut self,
        a: &AdapterOwner<'_, L, T, X, B>,
        s: &SecurityOwner<'gate, S, R>,
        now: u64,
    ) -> Result<()> {
        let c = self.capability_config.ok_or(Error::State)?;
        if self.capability.is_some()
            || self.phase != PeerReauthPhase::Established
            || self.io.tx.as_ref().is_some_and(|t| t.token.is_some())
        {
            return Ok(());
        }
        let peer = self.session(a, s, now)?;
        let role = if self.policy.initiator {
            CapabilityRole::Initiator
        } else {
            CapabilityRole::Responder
        };
        let mut cap = CapabilityRuntime::new(
            c.owner,
            role,
            peer,
            s,
            a,
            c.local,
            c.requirements,
            c.service,
            c.lifetime_us,
            c.retry_us,
            c.max_attempts,
            c.capability_lease_us,
            c.discovery_lease_us,
        )?;
        if self.policy.initiator {
            cap.start(peer, s, a)?;
        }
        self.capability = Some(cap);
        Ok(())
    }
    /// One real RX claim -> exact demux -> identity signature or Session/ACL authentication.
    /// 一条真实RX领取、精确分派，再做身份签名或Session/ACL认证。
    /// # Errors
    /// Wrong parent, Link, raw scope, signature, ACL or phase rejects; token is retired.
    #[allow(clippy::too_many_arguments)] // Independent caller-owned owners/providers.
    pub fn receive_with_capability<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const W: usize,
    >(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
        s: &mut SecurityOwner<'gate, S, R>,
        signer: &mut impl PeerReauthProvider,
        crypto: &mut impl CryptoProvider,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
    ) -> Result<bool> {
        self.advance_capability(a, s, now)?;
        if self.capability.is_none() {
            return self.receive(a, signer, now);
        }
        let peer = self.session(a, s, now)?;
        let claim = a.rx_claim(&mut self.io.raw)?;
        let valid = claim.link == self.io.config.link
            && claim.meta.timestamp_us != 0
            && claim.meta.timestamp_us <= now
            && claim.meta.sender_discriminator == self.policy.peer_discriminator;
        a.rx_retire(claim.token)?;
        if !valid {
            return Err(Error::Access);
        }
        self.io.raw_length = claim.frame_bytes;
        self.io.now = now;
        let n = 25 + 2 * self.io.config.width.bytes();
        if claim.frame_bytes < n {
            return Err(Error::Malformed);
        }
        let op = u16::from_be_bytes([self.io.raw[n - 2], self.io.raw[n - 1]]);
        if matches!(op, 16 | 17 | 18 | 20) {
            if self.peer_ready {
                return Err(Error::Replay);
            }
            self.current(a, now)?;
            return self.receive_staged(claim.meta.sender_discriminator, signer, now);
        }
        self.capability
            .as_mut()
            .ok_or(Error::State)?
            .receive_routed(
                peer,
                s,
                crypto,
                a,
                scratch,
                &self.io.raw[..claim.frame_bytes],
            )?;
        self.peer_ready = true; // Only actual successful Security/ACL admission can close Accept retry.
        Ok(true)
    }
    /// One alternating Reauth/Capability visit; at most one physical submission.
    /// Reauth/Capability持续轮转访问，一次最多一次物理提交。
    /// # Errors
    /// Every lane keeps its physical holder on error; no false automatic rollback.
    #[allow(clippy::too_many_arguments)] // Explicit product owners and counters, no hidden allocation.
    pub fn drive_with_capability<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
        const W: usize,
    >(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
        s: &mut SecurityOwner<'gate, S, R>,
        signer: &mut impl PeerReauthProvider,
        crypto: &mut impl CryptoProvider,
        counter: &mut impl C0TransactionOwner,
        hop: &mut impl HopSequenceOwner,
        driver: &mut impl TxDriver,
        scratch: &mut SecurityPacketWorkspace<W>,
        now: u64,
    ) -> Result<bool> {
        self.advance_capability(a, s, now)?;
        if self.capability.is_none() {
            return self.drive(a, signer, driver, now);
        }
        let peer = self.session(a, s, now)?;
        let lane = self.capability_cursor;
        self.capability_cursor = !lane;
        // The minimal Adapter has one physical TX slot shared by both lanes.
        // 最小Adapter仅一个物理TX槽，两条通道必须等待其真正退休。
        if !a.tx_is_quiescent() {
            return Ok(false);
        }
        if lane {
            return self
                .capability
                .as_mut()
                .ok_or(Error::State)?
                .drive_with_hop(peer, s, crypto, counter, Some(hop), a, driver, scratch);
        }
        if self.peer_ready || self.policy.initiator || now >= self.deadline || self.io.failed {
            return Ok(false);
        }
        match self.drive(a, signer, driver, now) {
            Err(Error::Exhausted) => Ok(false),
            v => v,
        }
    }
    /// One rotating retirement lane; Unknown remains owned.
    /// 单次轮转退休通道；Unknown始终保留Holder。
    /// # Errors
    /// Failed cancellation never discards its token.
    pub fn retire_with_capability<const L: usize, const T: usize, const X: usize>(
        &mut self,
        a: &mut AdapterOwner<'_, L, T, X, B>,
        d: &mut impl TxDriver,
        now: u64,
    ) -> Result<()> {
        if self.io.config.gate.is_active() || now < self.io.now {
            return Err(Error::State);
        }
        let lane = self.capability_retire_cursor;
        self.capability_retire_cursor = !lane;
        if lane {
            if let Some(c) = self.capability.as_mut() {
                c.retire(a, d, 1)?;
                return Ok(());
            }
        }
        self.retire(a, d, now)
    }
    /// Current confirmed capability, rechecking both Identity parents every time.
    /// 当前确认能力；每次重新校验双Identity父域。
    /// # Errors
    /// No confirmation, expired/replaced parents or Session rejects.
    pub fn selected_capability<
        const S: usize,
        const R: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &self,
        a: &AdapterOwner<'_, L, T, X, B>,
        s: &SecurityOwner<'gate, S, R>,
        now: u64,
    ) -> Result<ProfileSelect> {
        let peer = self.session(a, s, now)?;
        self.capability
            .as_ref()
            .ok_or(Error::State)?
            .selected(peer, s, a)
    }
    /// Logical progress only, not current-use authority. 仅逻辑进度，不表示当前可用权。
    #[must_use]
    pub fn capability_phase(&self) -> Option<CapabilityPhase> {
        self.capability.as_ref().map(CapabilityRuntime::phase)
    }
}
