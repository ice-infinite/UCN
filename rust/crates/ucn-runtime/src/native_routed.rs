//! Native port over the existing routed Coordinator; no second routing/security FSM.
//! Native端口复用实际多邻居协调器，不重新实现路由或安全状态机。
use crate::{
    DatagramEndpointHandle, DatagramEndpointView, DatagramSendOptions, PeerTxDriver,
    RoutedCounterBank, RoutedNode,
};
use ucn_adapter::AdapterOwner;
use ucn_capability::CapabilityOwner;
use ucn_core::{EndpointHandler, Lifecycle, SendOptions, Target};
use ucn_identity::IdentityOwner;
use ucn_native::{
    Backend, BackendMessage, BackendScope, Channel, Destination, PAYLOAD_BYTES, SecurityMinimum,
    Source,
};
use ucn_security::{AccessDirection, CryptoProvider, SecurityOwner, SecurityPacketWorkspace};
use ucn_types::{BindingGeneration, Error, NodeAddress, OriginSecurity, Result, ServiceId};

/// Immutable product label attached to one actual Origin Endpoint registration.
#[derive(Clone, Copy)]
pub struct RoutedNativeEndpoint {
    /// Previously registered and durably activated Endpoint.
    pub endpoint: DatagramEndpointHandle,
    /// Local configuration label, never a Principal or authorization grant.
    pub principal_digest: u64,
}
/// Explicit permanent product borrows; no hidden Owner/global or duplicate counter.
pub struct NativeRoutedResources<
    'a,
    'g,
    'i,
    DR,
    CP,
    CB,
    const N: usize,
    const S: usize,
    const R: usize,
    const K: usize,
    const L: usize,
    const T: usize,
    const X: usize,
    const B: usize,
    const W: usize,
> {
    /// Current durable Identity parents.
    pub identity: &'a IdentityOwner<'g, N>,
    /// Unique Security Owner, shared by Origin and Hop.
    pub security: &'a mut SecurityOwner<'g, S, R>,
    /// Current authenticated neighbour capabilities.
    pub capabilities: &'a CapabilityOwner<K>,
    /// Single physical ingress/retirement owner.
    pub adapter: &'a mut AdapterOwner<'i, L, T, X, B>,
    /// Product cryptographic provider.
    pub crypto: &'a mut CP,
    /// Existing unique counters, never reinitialized by the facade.
    pub counters: &'a mut CB,
    /// Actual directed neighbour Driver.
    pub driver: &'a mut DR,
    /// Caller-owned fixed packet work area, not a large automatic copy.
    pub scratch: &'a mut SecurityPacketWorkspace<W>,
}
/// Multi-Peer H1 Native adapter borrowing the actual autonomous `RoutedNode`.
pub struct NativeRoutedBackend<
    'a,
    'g,
    'i,
    DR,
    CP,
    CB,
    const P: usize,
    const C: usize,
    const Q: usize,
    const A: usize,
    const E: usize,
    const B: usize,
    const D: usize,
    const V: usize,
    const RR: usize,
    const SS: usize,
    const N: usize,
    const S: usize,
    const R: usize,
    const K: usize,
    const L: usize,
    const T: usize,
    const X: usize,
    const W: usize,
> {
    node: &'a mut RoutedNode<'g, P, C, Q, A, E, B, D, V, RR, SS>,
    io: NativeRoutedResources<'a, 'g, 'i, DR, CP, CB, N, S, R, K, L, T, X, B, W>,
    endpoints: &'a [RoutedNativeEndpoint],
    local: Source,
    now: u64,
    lifecycle: Lifecycle,
}
impl<
    'a,
    'g,
    'i,
    DR: PeerTxDriver,
    CP: CryptoProvider,
    CB: RoutedCounterBank,
    const P: usize,
    const C: usize,
    const Q: usize,
    const A: usize,
    const E: usize,
    const B: usize,
    const D: usize,
    const V: usize,
    const RR: usize,
    const SS: usize,
    const N: usize,
    const S: usize,
    const R: usize,
    const K: usize,
    const L: usize,
    const T: usize,
    const X: usize,
    const W: usize,
>
    NativeRoutedBackend<
        'a,
        'g,
        'i,
        DR,
        CP,
        CB,
        P,
        C,
        Q,
        A,
        E,
        B,
        D,
        V,
        RR,
        SS,
        N,
        S,
        R,
        K,
        L,
        T,
        X,
        W,
    >
{
    /// Attach fixed Origin registrations to an idle actual Coordinator.
    /// # Errors
    /// Existing physical/application work, bad labels, parents, ACL or capacity rejects.
    pub fn new(
        node: &'a mut RoutedNode<'g, P, C, Q, A, E, B, D, V, RR, SS>,
        io: NativeRoutedResources<'a, 'g, 'i, DR, CP, CB, N, S, R, K, L, T, X, B, W>,
        endpoints: &'a [RoutedNativeEndpoint],
        local_label: u64,
        now: u64,
    ) -> Result<Self> {
        if local_label == 0
            || endpoints.is_empty()
            || endpoints.len() > E
            || !io.adapter.tx_is_quiescent()
            || !io.adapter.rx_is_quiescent()
            || node.counts() != (0, 0, 0, 0)
        {
            return Err(Error::State);
        }
        let local = node.local_binding();
        let backend = Self {
            node,
            io,
            endpoints,
            local: Source {
                address: local.address,
                binding: local.generation,
                principal_digest: local_label,
            },
            now,
            lifecycle: Lifecycle::Initialized,
        };
        for (i, item) in endpoints.iter().enumerate() {
            let view = backend.view(item.endpoint, now)?;
            if item.principal_digest == 0
                || usize::from(view.policy.max_payload_bytes) > PAYLOAD_BYTES
                || view.local != local
                || endpoints[..i]
                    .iter()
                    .any(|old| old.endpoint == item.endpoint)
            {
                return Err(Error::Config);
            }
            for old in &endpoints[..i] {
                let other = backend.view(old.endpoint, now)?;
                if other.remote.address == view.remote.address
                    && (other.remote != view.remote
                        || old.principal_digest != item.principal_digest)
                {
                    return Err(Error::Config);
                }
            }
        }
        Ok(backend)
    }
    fn authorized(
        &self,
        h: DatagramEndpointHandle,
        direction: AccessDirection,
        now: u64,
    ) -> Result<DatagramEndpointView> {
        if now < self.now {
            return Err(Error::State);
        }
        self.node
            .authorize_endpoint(h, self.io.identity, self.io.security, direction, now)
    }
    fn view(&self, h: DatagramEndpointHandle, now: u64) -> Result<DatagramEndpointView> {
        self.authorized(h, AccessDirection::Inbound, now)
            .or_else(|_| self.authorized(h, AccessDirection::Outbound, now))
    }
    fn find(
        &self,
        address: NodeAddress,
        generation: BindingGeneration,
        service: ServiceId,
        now: u64,
    ) -> Result<(RoutedNativeEndpoint, DatagramEndpointView)> {
        let mut found = None;
        for item in self.endpoints {
            if let Ok(v) = self.view(item.endpoint, now) {
                if v.remote.address == address
                    && v.remote.generation == generation
                    && v.policy.service == service
                {
                    if found.is_some() {
                        return Err(Error::Access);
                    }
                    found = Some((*item, v));
                }
            }
        }
        found.ok_or(Error::Access)
    }
    fn scope(h: DatagramEndpointHandle) -> Result<BackendScope> {
        Ok(BackendScope {
            instance: h.owner_instance(),
            generation: h.generation(),
            slot: u16::try_from(h.slot() + 1).map_err(|_| Error::Config)?,
        })
    }
    fn minimum(v: OriginSecurity, min: SecurityMinimum) -> bool {
        match min {
            SecurityMinimum::StaticTrusted => false,
            SecurityMinimum::OriginAuth => v != OriginSecurity::O0,
            SecurityMinimum::Encrypted => v == OriginSecurity::O2,
        }
    }
    fn eligible(v: DatagramEndpointView, c: &Channel) -> bool {
        v.policy.service == c.service
            && Self::minimum(v.protection, c.security)
            && c.payload_bytes <= v.policy.max_payload_bytes
            && (c.traffic_class as u8) >= (v.policy.traffic_ceiling as u8)
    }
    fn local_eligible(&self, c: &Channel, now: u64) -> bool {
        self.endpoints.iter().any(|e| {
            self.authorized(e.endpoint, AccessDirection::Outbound, now)
                .is_ok_and(|v| Self::eligible(v, c))
        })
    }
}
impl<
    DR: PeerTxDriver,
    CP: CryptoProvider,
    CB: RoutedCounterBank,
    const P: usize,
    const C: usize,
    const Q: usize,
    const A: usize,
    const E: usize,
    const B: usize,
    const D: usize,
    const V: usize,
    const RR: usize,
    const SS: usize,
    const N: usize,
    const S: usize,
    const R: usize,
    const K: usize,
    const L: usize,
    const T: usize,
    const X: usize,
    const W: usize,
> Backend
    for NativeRoutedBackend<
        '_,
        '_,
        '_,
        DR,
        CP,
        CB,
        P,
        C,
        Q,
        A,
        E,
        B,
        D,
        V,
        RR,
        SS,
        N,
        S,
        R,
        K,
        L,
        T,
        X,
        W,
    >
{
    fn local(&self) -> Destination {
        Destination {
            address: self.local.address,
            binding: self.local.binding,
        }
    }
    fn binding(&self, address: NodeAddress) -> Option<BindingGeneration> {
        if address == self.local.address {
            return Some(self.local.binding);
        }
        self.endpoints.iter().find_map(|e| {
            self.view(e.endpoint, self.now)
                .ok()
                .filter(|v| v.remote.address == address)
                .map(|v| v.remote.generation)
        })
    }
    fn principal(&self, address: NodeAddress) -> Option<u64> {
        if address == self.local.address {
            return Some(self.local.principal_digest);
        }
        self.endpoints.iter().find_map(|e| {
            self.view(e.endpoint, self.now)
                .ok()
                .filter(|v| v.remote.address == address)
                .map(|_| e.principal_digest)
        })
    }
    fn trusted(&self) -> bool {
        false
    }
    fn lifecycle(&self) -> Lifecycle {
        self.lifecycle
    }
    fn supports(&self, minimum: SecurityMinimum) -> bool {
        self.endpoints.iter().any(|e| {
            self.view(e.endpoint, self.now)
                .is_ok_and(|v| Self::minimum(v.protection, minimum))
        })
    }
    fn install(&mut self, channels: &[Channel], _: &'static dyn EndpointHandler) -> Result<()> {
        if self.lifecycle != Lifecycle::Initialized {
            return Err(Error::State);
        }
        for c in channels {
            if c.sources
                .iter()
                .flatten()
                .any(|s| s.address == self.local.address)
                && !self.local_eligible(c, self.now)
            {
                return Err(Error::Policy);
            }
            for src in c
                .sources
                .iter()
                .flatten()
                .filter(|s| s.address != self.local.address)
            {
                let (_, v) = self.find(src.address, src.binding, c.service, self.now)?;
                if !Self::eligible(v, c) {
                    return Err(Error::Policy);
                }
            }
            for dst in c.targets.iter().flatten() {
                let (_, v) = self.find(dst.address, dst.binding, c.service, self.now)?;
                if !Self::eligible(v, c) {
                    return Err(Error::Policy);
                }
            }
        }
        Ok(())
    }
    fn start(&mut self, now: u64) -> Result<()> {
        if self.lifecycle != Lifecycle::Initialized || now < self.now {
            return Err(Error::State);
        }
        for e in self.endpoints {
            self.view(e.endpoint, now)?;
        }
        self.io.adapter.try_set_rx_enabled(true)?;
        self.now = now;
        self.lifecycle = Lifecycle::Running;
        Ok(())
    }
    fn stop(&mut self) -> Result<()> {
        self.node.stop(self.io.adapter)?;
        self.lifecycle = Lifecycle::Stopping;
        Ok(())
    }
    fn send(
        &mut self,
        target: Target,
        payload: &[u8],
        options: SendOptions,
        now: u64,
    ) -> Result<()> {
        if self.lifecycle != Lifecycle::Running || options.absolute_deadline_us <= now {
            return Err(Error::State);
        }
        let (e, _) = self.find(
            target.address,
            target.binding_generation,
            target.service_id,
            now,
        )?;
        let cap = now
            .checked_add(self.node.maximum_local_lifetime_us())
            .ok_or(Error::Exhausted)?;
        self.node.send(
            e.endpoint,
            payload,
            DatagramSendOptions {
                traffic_class: options.traffic_class,
                deadline_us: options.absolute_deadline_us.min(cap),
                tracked: false,
            },
            self.io.identity,
            self.io.security,
            now,
        )?;
        self.now = now;
        Ok(())
    }
    fn step(&mut self, now: u64, budget: u16) -> Result<()> {
        if budget == 0 || now < self.now || self.lifecycle == Lifecycle::Quiescent {
            return Err(Error::State);
        }
        self.node.step(
            self.io.identity,
            self.io.security,
            self.io.capabilities,
            self.io.adapter,
            self.io.crypto,
            self.io.counters,
            self.io.driver,
            self.io.scratch,
            now,
            usize::from(budget.min(64)),
        )?;
        self.now = now;
        if self.lifecycle == Lifecycle::Stopping && self.node.quiescent(self.io.adapter) {
            self.lifecycle = Lifecycle::Quiescent;
        }
        Ok(())
    }
    fn poll(&mut self) -> Result<BackendMessage> {
        if self.lifecycle != Lifecycle::Running {
            return Err(Error::State);
        }
        let mut payload = [0; PAYLOAD_BYTES];
        let msg = self.node.receive_datagram(
            self.io.identity,
            self.io.security,
            &mut payload,
            self.now,
        )?;
        let entry = self
            .endpoints
            .iter()
            .find(|e| e.endpoint == msg.endpoint)
            .ok_or(Error::Access)?;
        Ok(BackendMessage {
            source: Source {
                address: msg.origin.address,
                binding: msg.origin.generation,
                principal_digest: entry.principal_digest,
            },
            scope: Self::scope(msg.endpoint)?,
            receive_time_us: msg.receive_time_us,
            origin_sequence: msg.origin_sequence,
            service: msg.service,
            traffic_class: msg.traffic_class,
            protection: if msg.protection == OriginSecurity::O2 {
                SecurityMinimum::Encrypted
            } else {
                SecurityMinimum::OriginAuth
            },
            payload_bytes: u16::try_from(msg.bytes).map_err(|_| Error::NoSpace)?,
            payload,
        })
    }
    fn validate(
        &self,
        scope: BackendScope,
        source: Source,
        c: &Channel,
        now: u64,
    ) -> Result<[u8; 16]> {
        if self.lifecycle != Lifecycle::Running {
            return Err(Error::State);
        }
        if scope == BackendScope::LOCAL {
            if source != self.local {
                return Err(Error::Access);
            }
            return if self.local_eligible(c, now) {
                Ok([0; 16])
            } else {
                Err(Error::Access)
            };
        }
        let (e, v) = self.find(source.address, source.binding, c.service, now)?;
        if e.principal_digest != source.principal_digest
            || Self::scope(e.endpoint)? != scope
            || !Self::eligible(v, c)
        {
            return Err(Error::Access);
        }
        let v = self.authorized(e.endpoint, AccessDirection::Inbound, now)?;
        Ok(v.remote.principal)
    }
}
