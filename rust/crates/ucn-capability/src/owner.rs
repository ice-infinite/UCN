use ucn_security::{AuthenticatedPeerView, Binding};
use ucn_types::{Error, RealmId, Result};

use crate::{CapabilityRecord, CapabilitySummary, capability_digest, codec::validate_summary};

/// Capability Owner 静态配置。
#[derive(Clone, Copy)]
pub struct CapabilityConfig {
    /// Runtime 实例。
    pub runtime_instance: u32,
    /// 唯一允许签发认证 Peer View 的 Security Owner 实例。
    pub security_owner_instance: u32,
    /// Capability 所属 Realm。
    pub realm: RealmId,
    /// 本机不可变 Capability Record。
    pub local_record: CapabilityRecord,
    /// Peer record 的本地半开租期。
    pub capability_lease_us: u64,
    /// Summary discovery 的本地半开租期。
    pub discovery_lease_us: u64,
}

/// 稳定 Peer Capability 查找键；不携带可变能力事实。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PeerCapabilityRef {
    /// Runtime 实例。
    pub runtime_instance: u32,
    /// Security Owner 实例。
    pub security_owner_instance: u32,
    /// Realm。
    pub realm: RealmId,
    /// 对端 Principal。
    pub principal: [u8; 16],
    /// 对端 Binding。
    pub binding: Binding,
    /// 对端 Session Generation。
    pub session_generation: u32,
    /// 当前入站 Link ID。
    pub ingress_link_id: u16,
    /// 当前入站 Link Generation。
    pub ingress_link_generation: u32,
}

/// 固定缓存内的一个认证 Peer Capability 快照。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CachedPeerCapability {
    /// 稳定父域引用。
    pub peer_ref: PeerCapabilityRef,
    /// 完整 Record。
    pub record: CapabilityRecord,
    /// Canonical Record digest。
    pub digest: [u8; 16],
    /// Summary discovery 的半开 Deadline。
    pub discovery_deadline_us: u64,
    /// Record 的半开 Deadline。
    pub capability_deadline_us: u64,
}

/// 收到 Capability Summary 后的处理结论。
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HelloDisposition {
    /// 当前缓存精确匹配且仍 live。
    Matched,
    /// 需要在同一已认证 Session 上查询完整 Record。
    QueryRequired,
}

#[derive(Clone, Copy)]
struct PeerSlot {
    occupied: bool,
    value: Option<CachedPeerCapability>,
}

impl PeerSlot {
    const EMPTY: Self = Self {
        occupied: false,
        value: None,
    };
}

/// 固定容量 Capability Owner。
pub struct CapabilityOwner<const PEERS: usize> {
    runtime_instance: u32,
    security_owner_instance: u32,
    realm: RealmId,
    local_record: CapabilityRecord,
    local_digest: [u8; 16],
    capability_lease_us: u64,
    discovery_lease_us: u64,
    peers: [PeerSlot; PEERS],
}

/// Nano Profile：2 个认证 Peer Capability。
pub type NanoCapabilityOwner = CapabilityOwner<2>;
/// Lite Profile：8 个认证 Peer Capability。
pub type LiteCapabilityOwner = CapabilityOwner<8>;
/// Full Profile：16 个认证 Peer Capability。
pub type FullCapabilityOwner = CapabilityOwner<16>;

impl<const PEERS: usize> CapabilityOwner<PEERS> {
    /// 建立固定容量 Owner。
    ///
    /// # Errors
    ///
    /// 容量、租期或本机 Capability 非法时返回配置错误。
    pub fn new(config: CapabilityConfig) -> Result<Self> {
        if PEERS == 0
            || config.runtime_instance == 0
            || config.security_owner_instance == 0
            || config.capability_lease_us == 0
            || config.discovery_lease_us == 0
        {
            return Err(Error::Config);
        }
        config.local_record.validate()?;
        let local_digest = capability_digest(config.local_record)?;
        Ok(Self {
            runtime_instance: config.runtime_instance,
            security_owner_instance: config.security_owner_instance,
            realm: config.realm,
            local_record: config.local_record,
            local_digest,
            capability_lease_us: config.capability_lease_us,
            discovery_lease_us: config.discovery_lease_us,
            peers: [PeerSlot::EMPTY; PEERS],
        })
    }

    /// 复制不可变本机 Record 与摘要。
    #[must_use]
    pub const fn local(&self) -> (CapabilityRecord, [u8; 16]) {
        (self.local_record, self.local_digest)
    }

    /// 处理认证 Session 上的 Summary；它本身不授予任何权限。
    ///
    /// # Errors
    ///
    /// 认证父域到期或 Link Generation 错绑时返回错误。
    pub fn ingest_summary(
        &self,
        authenticated: AuthenticatedPeerView,
        summary: CapabilitySummary,
        now_us: u64,
    ) -> Result<HelloDisposition> {
        validate_summary(summary)?;
        let peer_ref = self.peer_ref(authenticated, now_us)?;
        if summary.link_instance_generation != peer_ref.ingress_link_generation {
            return Err(Error::Security);
        }
        let matched = self
            .peers
            .iter()
            .filter_map(|slot| slot.value)
            .any(|cached| {
                cached.peer_ref == peer_ref
                    && cached.record.capability_generation == summary.capability_generation
                    && cached.digest == summary.digest
                    && peer_is_live(cached, now_us)
            });
        Ok(if matched {
            HelloDisposition::Matched
        } else {
            HelloDisposition::QueryRequired
        })
    }

    /// 接纳认证 Session 上的完整 Capability Advertise。
    ///
    /// 同一父域只接受 checked-next Generation；精确重复幂等但不刷新 Deadline。
    ///
    /// # Errors
    ///
    /// Record、认证父域、代际、摘要、Deadline 或容量不成立时失败关闭。
    pub fn ingest_advertise(
        &mut self,
        authenticated: AuthenticatedPeerView,
        record: CapabilityRecord,
        now_us: u64,
    ) -> Result<PeerCapabilityRef> {
        record.validate()?;
        let peer_ref = self.peer_ref(authenticated, now_us)?;
        if record.link.link_instance_generation != peer_ref.ingress_link_generation {
            return Err(Error::Security);
        }
        let digest = capability_digest(record)?;
        if let Some(index) = self
            .peers
            .iter()
            .position(|slot| slot.value.is_some_and(|cached| cached.peer_ref == peer_ref))
        {
            let previous = self.peers[index].value.ok_or(Error::State)?;
            if record.capability_generation == previous.record.capability_generation {
                return if digest == previous.digest {
                    Ok(peer_ref)
                } else {
                    Err(Error::Security)
                };
            }
            if previous.record.capability_generation.checked_add(1)
                != Some(record.capability_generation)
            {
                return Err(Error::Replay);
            }
            let value = self.make_cached(peer_ref, record, digest, authenticated, now_us)?;
            self.peers[index].value = Some(value);
            return Ok(peer_ref);
        }
        if self
            .peers
            .iter()
            .filter_map(|slot| slot.value)
            .any(|cached| {
                cached.peer_ref.principal == peer_ref.principal && cached.peer_ref != peer_ref
            })
        {
            return Err(Error::Replay);
        }
        let index = self
            .peers
            .iter()
            .position(|slot| !slot.occupied)
            .ok_or(Error::NoSpace)?;
        let value = self.make_cached(peer_ref, record, digest, authenticated, now_us)?;
        self.peers[index] = PeerSlot {
            occupied: true,
            value: Some(value),
        };
        Ok(peer_ref)
    }

    /// 从稳定引用重新解析当前 live 能力；过期历史不可读取为有效能力。
    ///
    /// # Errors
    ///
    /// 引用不存在或 Capability 已到期时返回错误。
    pub fn peer_get(
        &self,
        reference: PeerCapabilityRef,
        now_us: u64,
    ) -> Result<CachedPeerCapability> {
        let value = self
            .peers
            .iter()
            .filter_map(|slot| slot.value)
            .find(|cached| cached.peer_ref == reference)
            .ok_or(Error::NotFound)?;
        if !peer_is_live(value, now_us) {
            return Err(Error::Timeout);
        }
        Ok(value)
    }

    /// 明确退休认证 Session 父域并释放其固定槽。
    ///
    /// 只有父 Session 失效才能清除过期 Capability 高水位；单纯 lease expiry 不回收。
    ///
    /// # Errors
    ///
    /// 父域引用不存在时返回错误且不改写其他槽。
    pub fn invalidate_session(&mut self, reference: PeerCapabilityRef) -> Result<()> {
        let index = self
            .peers
            .iter()
            .position(|slot| {
                slot.value
                    .is_some_and(|cached| cached.peer_ref == reference)
            })
            .ok_or(Error::NotFound)?;
        self.peers[index] = PeerSlot::EMPTY;
        Ok(())
    }

    /// 统计占用与当前 live 槽；不触发隐式回收。
    #[must_use]
    pub fn counts(&self, now_us: u64) -> (usize, usize) {
        let occupied = self.peers.iter().filter(|slot| slot.occupied).count();
        let live = self
            .peers
            .iter()
            .filter_map(|slot| slot.value)
            .filter(|value| peer_is_live(*value, now_us))
            .count();
        (occupied, live)
    }

    fn make_cached(
        &self,
        peer_ref: PeerCapabilityRef,
        record: CapabilityRecord,
        digest: [u8; 16],
        authenticated: AuthenticatedPeerView,
        now_us: u64,
    ) -> Result<CachedPeerCapability> {
        let discovery_deadline_us = now_us
            .checked_add(self.discovery_lease_us)
            .ok_or(Error::Exhausted)?
            .min(authenticated.expires_at_us());
        let capability_deadline_us = now_us
            .checked_add(self.capability_lease_us)
            .ok_or(Error::Exhausted)?
            .min(authenticated.expires_at_us());
        if discovery_deadline_us <= now_us || capability_deadline_us <= now_us {
            return Err(Error::Timeout);
        }
        Ok(CachedPeerCapability {
            peer_ref,
            record,
            digest,
            discovery_deadline_us,
            capability_deadline_us,
        })
    }

    fn peer_ref(
        &self,
        authenticated: AuthenticatedPeerView,
        now_us: u64,
    ) -> Result<PeerCapabilityRef> {
        if authenticated.runtime_instance() != self.runtime_instance
            || authenticated.security_owner_instance() != self.security_owner_instance
            || authenticated.realm() != self.realm
        {
            return Err(Error::Security);
        }
        if now_us >= authenticated.expires_at_us() {
            return Err(Error::Timeout);
        }
        let binding = authenticated.peer();
        Ok(PeerCapabilityRef {
            runtime_instance: self.runtime_instance,
            security_owner_instance: self.security_owner_instance,
            realm: self.realm,
            principal: binding.principal,
            binding,
            session_generation: authenticated.session_generation().get(),
            ingress_link_id: authenticated.ingress_link_id(),
            ingress_link_generation: authenticated.ingress_link_generation().get(),
        })
    }
}

fn peer_is_live(value: CachedPeerCapability, now_us: u64) -> bool {
    now_us < value.discovery_deadline_us && now_us < value.capability_deadline_us
}
