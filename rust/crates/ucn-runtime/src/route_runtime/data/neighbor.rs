//! Exact neighbour fence, physical drain and authenticated successor adoption.
//! 精确邻居围栏、真实排空与认证后继接纳；不重建Root、不继承旧路由或Profile。
use super::{
    AdapterOwner, BindingHandle, CapabilityOwner, Error, IdentityOwner, JournalPeer,
    PeerCapabilityRef, Result, RoutePeerHandle, RoutedNode, SecurityOwner,
};

impl<
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
> RoutedNode<'_, P, C, Q, A, E, B, D, V, RR, SS>
{
    /// Fence the actual Session and all local Origin/Hop roles without dropping holders.
    /// 围栏实际Session和本地全部Origin/Hop角色，不释放未知物理义务。
    /// # Errors
    /// Wrong handle/gate, callback reentry or a backwards clock rejects.
    pub fn fence_neighbor<const S: usize, const R: usize>(
        &mut self,
        handle: RoutePeerHandle,
        security: &mut SecurityOwner<'_, S, R>,
        now: u64,
    ) -> Result<()> {
        self.clock(now, true)?;
        if !core::ptr::eq(security.callback_gate(), self.control.gate) {
            return Err(Error::State);
        }
        let peer = self.control.peers[self.control.index(handle)?].ok_or(Error::NotFound)?;
        self.fence_session(peer.peer.session, security)?;
        Ok(())
    }

    /// Query this Root's actual Control/Data/App/RX-holder union, not global quiescence.
    /// 查询本Root的Control/Data/App/RX真实义务并集，不冒充全局静止。
    /// # Errors
    /// Foreign/stale handle or callback reentry rejects; an active neighbour is not drained.
    pub fn neighbor_is_drained(&self, handle: RoutePeerHandle) -> Result<bool> {
        if self.control.gate.is_active() {
            return Err(Error::State);
        }
        let peer = self.control.peers[self.control.index(handle)?].ok_or(Error::NotFound)?;
        Ok(peer.fenced && self.session_drained(peer.peer.session))
    }

    /// Adopt a real durable ACTIVE successor and its newly authenticated live Capability.
    /// 接纳真实耐久 ACTIVE 后继及其重新认证的 live Capability；最后一次性替换原槽。
    /// # Errors
    /// Missing fence/drain, old generation, changed Binding/policy, unconfirmed or stale
    /// Capability, profile/locator/MTU mismatch rejects without replacing the registration.
    #[allow(clippy::too_many_arguments)]
    pub fn rebind_neighbor<
        const N: usize,
        const S: usize,
        const R: usize,
        const K: usize,
        const L: usize,
        const T: usize,
        const X: usize,
    >(
        &mut self,
        old: RoutePeerHandle,
        peer: JournalPeer,
        parents: [BindingHandle; 2],
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        caps: &CapabilityOwner<K>,
        capability: PeerCapabilityRef,
        adapter: &AdapterOwner<'_, L, T, X, B>,
        cost: u32,
        locator: u32,
        now: u64,
    ) -> Result<RoutePeerHandle> {
        self.clock(now, false)?;
        let slot = self.control.index(old)?;
        let previous = self.control.peers[slot].ok_or(Error::NotFound)?;
        if !previous.fenced
            || !self.session_drained(previous.peer.session)
            || peer.session == previous.peer.session
            || peer.facts.local != previous.peer.facts.local
            || peer.facts.peer != previous.peer.facts.peer
            || peer.facts.policy_generation != previous.peer.facts.policy_generation
            || capability.session_generation <= previous.capability.peer_ref.session_generation
        {
            return Err(Error::State);
        }
        // A Service tombstone must not hide an undrained newer Origin lineage.
        // 防止同Peer另一Service墓碑掩盖较新Origin的未决义务。
        if self.endpoints.iter().flatten().any(|e| {
            e.facts.peer.principal == peer.facts.peer.principal
                && e.session != peer.session
                && (!e.fenced
                    || !self.session_drained(e.session)
                    || capability.session_generation <= e.session_generation.get())
        }) {
            return Err(Error::State);
        }
        let next = self.control.prepare_neighbor(
            peer,
            parents,
            identity,
            security,
            caps,
            capability,
            adapter,
            cost,
            locator,
            now,
            Some(slot),
        )?;
        let generation = self.control.generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        // All old physical obligations have retired. Old routes/receipts are not copied.
        // 旧物理义务全部退休；新登记不继承旧Route/Receipt/分片组装。
        self.routes.retire_peer_history(previous.link)?;
        self.control.assemblies[slot] = super::super::Assembly::EMPTY;
        for receipt in &mut self.control.receipts {
            if receipt
                .is_some_and(|r| r.upstream == Some(old) || r.peer == old && r.upstream.is_none())
            {
                *receipt = None; // Metadata only; no physical holder is stored here.
            }
        }
        self.control.peers[slot] = Some(next);
        self.control.generations[slot] = generation;
        self.control.last_now = now;
        Ok(self.control.handle(slot))
    }
}
