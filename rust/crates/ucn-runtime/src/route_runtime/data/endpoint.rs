//! Monotonic local registry fence, exact physical drain and durable successor handoff.
//! 单向本地登记围栏、精确物理排空与真实耐久后继交接；不是线上握手。
use super::{DatagramEndpointHandle, RoutedNode};
use ucn_identity::{BindingHandle, IdentityOwner};
use ucn_security::{CurrentFacts, Fingerprint, SecurityOwner, SessionHandle};
use ucn_types::{Error, Result};

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
    /// Fence the actual Origin Session and every local Service sharing it.
    /// 围栏实际Origin Session及共享它的所有本地Service；不清除任何物理义务。
    /// # Errors
    /// Invalid handle, wrong Security gate/owner or callback/time reentry rejects.
    pub fn fence_origin<const S: usize, const R: usize>(
        &mut self,
        handle: DatagramEndpointHandle,
        security: &mut SecurityOwner<'_, S, R>,
        now: u64,
    ) -> Result<usize> {
        self.clock(now, true)?;
        if !core::ptr::eq(security.callback_gate(), self.control.gate) {
            return Err(Error::State);
        }
        let session = self.endpoints[self.endpoint_index(handle)?]
            .ok_or(Error::NotFound)?
            .session;
        self.fence_session(session, security)
    }

    pub(super) fn fence_session<const S: usize, const R: usize>(
        &mut self,
        session: SessionHandle,
        security: &mut SecurityOwner<'_, S, R>,
    ) -> Result<usize> {
        security.fence(session)?;
        // The same Session may also be the neighbour. Fence both roles without
        // erasing any Control/Data token or its retirement obligation.
        // 同Session可能也是邻居；同时围栏两种角色，不抹除Control/Data Token。
        for i in 0..P {
            if let Some(mut peer) = self.control.peers[i] {
                if peer.peer.session == session {
                    peer.fenced = true;
                    self.control.peers[i] = Some(peer);
                    self.control.assemblies[i].active = false;
                    let handle = self.control.handle(i);
                    for job in &mut self.control.jobs {
                        if let Some(job) = job.value.as_mut() {
                            if job.peer == handle {
                                job.retire = true;
                            }
                        }
                    }
                    let _ = self.routes.invalidate_peer(peer.link);
                    for receipt in self.control.receipts.iter_mut().flatten() {
                        if receipt.peer == handle || receipt.upstream == Some(handle) {
                            receipt.failed = true;
                        }
                    }
                }
            }
        }
        let mut count = 0;
        for endpoint in self
            .endpoints
            .iter_mut()
            .flatten()
            .filter(|e| e.session == session)
        {
            endpoint.fenced = true;
            count += 1;
        }
        Ok(count)
    }

    pub(super) fn session_drained(&self, session: SessionHandle) -> bool {
        let belongs = |handle: DatagramEndpointHandle| {
            self.endpoints.get(handle.slot).is_some_and(|e| {
                e.is_some_and(|e| e.session == session)
                    && self.endpoint_generations[handle.slot] == handle.generation
            })
        };
        let hop_belongs = |handle: super::RoutePeerHandle| {
            self.control.peers.get(handle.slot).is_some_and(|p| {
                p.is_some_and(|p| p.peer.session == session)
                    && self.control.handle(handle.slot) == handle
            })
        };
        !self.pending_rx.is_some_and(|rx| {
            self.control.peers.iter().flatten().any(|p| {
                p.peer.session == session
                    && p.peer.link == rx.link
                    && p.discriminator == rx.meta.sender_discriminator
            })
        }) && !self
            .control
            .jobs
            .iter()
            .any(|slot| slot.value.is_some_and(|j| hop_belongs(j.peer)))
            && !self.jobs.iter().any(|slot| {
                slot.value.is_some_and(|j| {
                    (j.endpoint.is_some_and(belongs) || j.peer.is_some_and(hop_belongs))
                        && (j.outcome.is_none() || j.token.is_some())
                })
            })
            && !self
                .app
                .iter()
                .any(|slot| slot.value.is_some_and(|message| belongs(message.endpoint)))
    }

    /// Local barrier includes Control/relay holders if Origin shares a Hop Session.
    /// 本地屏障包含共享Hop Session的Control/中继义务，不授予新建Session权力。
    /// # Errors
    /// Stale handle or callback reentry rejects. Active registrations return false.
    pub fn origin_is_drained(&self, handle: DatagramEndpointHandle) -> Result<bool> {
        if self.control.gate.is_active() {
            return Err(Error::State);
        }
        let endpoint = self.endpoints[self.endpoint_index(handle)?].ok_or(Error::NotFound)?;
        Ok(endpoint.fenced && self.session_drained(endpoint.session))
    }

    /// Atomically replace one fenced registration with an actual durable successor.
    /// 用真实耐久后继原子替换一个已围栏登记；同Session其他Service仍须分别交接。
    /// # Errors
    /// Undrained work, stale/unpublished Session, different Binding/policy/protection,
    /// nonincreasing generation or any parent mismatch rejects without replacing the slot.
    #[allow(clippy::too_many_arguments)]
    pub fn rebind_endpoint<const N: usize, const S: usize, const R: usize>(
        &mut self,
        handle: DatagramEndpointHandle,
        session: SessionHandle,
        facts: CurrentFacts,
        fingerprint: Fingerprint,
        parents: [BindingHandle; 2],
        identity: &IdentityOwner<'_, N>,
        security: &SecurityOwner<'_, S, R>,
        now: u64,
    ) -> Result<DatagramEndpointHandle> {
        self.clock(now, false)?;
        let slot = self.endpoint_index(handle)?;
        let old = self.endpoints[slot].ok_or(Error::NotFound)?;
        if !old.fenced || !self.session_drained(old.session) {
            return Err(Error::State);
        }
        if session == old.session
            || facts.local != old.facts.local
            || facts.peer != old.facts.peer
            || facts.policy_generation != old.facts.policy_generation
        {
            return Err(Error::State);
        }
        // Another Service must not carry a newer undrained lineage behind this
        // older tombstone. The barrier is peer-wide, not just one Endpoint.
        // 禁止借另一Service的旧墓碑绕过较新Session尚未排空的义务。
        if self.endpoints.iter().flatten().any(|e| {
            (e.facts.peer.principal == facts.peer.principal
                || e.facts.peer.address == facts.peer.address)
                && e.session != session
                && (!e.fenced || !self.session_drained(e.session))
        }) {
            return Err(Error::State);
        }
        let next = self.make_endpoint(
            session,
            facts,
            fingerprint,
            parents,
            old.policy,
            identity,
            security,
            now,
        )?;
        if next.session_generation.get() <= old.session_generation.get()
            || next.protection != old.protection
            || self.endpoints.iter().flatten().any(|e| {
                e.facts.peer == facts.peer
                    && e.session != session
                    && next.session_generation.get() <= e.session_generation.get()
            })
        {
            return Err(Error::Replay);
        }
        let generation = self.endpoint_generations[slot]
            .checked_add(1)
            .ok_or(Error::Exhausted)?;
        self.endpoints[slot] = Some(next);
        self.endpoint_generations[slot] = generation;
        Ok(DatagramEndpointHandle {
            owner: self.control.config.owner,
            slot,
            generation,
        })
    }
}
