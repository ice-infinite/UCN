//! Three actual durable Identity/Security owners, not synthesized authenticated DTOs.
#![allow(
    clippy::many_single_char_names,
    clippy::too_many_lines,
    clippy::too_many_arguments
)]
use super::reauth_tests::{ReauthCrypto, initial};
use super::*;
use ucn_capability::{
    CapabilityConfig, CapabilityOwner, PeerCapabilityRef, decode_capability_record,
    encode_capability_record,
};
use ucn_routing::{ResolvedRoute, RouteConfig, RouteDomain, RouteOwner, RouteUseFacts};
mod c1_path;
#[cfg(feature = "route-data-runtime")]
mod data_node;
#[cfg(feature = "native-routed")]
mod native;

const DOMAINS: [DomainKey; 7] = [
    AUTH,
    BIND,
    BIND_REMOTE,
    DomainKey {
        kind: DomainKind::IdentityBinding,
        id: 4,
    },
    SESSION,
    DomainKey {
        kind: DomainKind::SecurityHighWater,
        id: 2,
    },
    DomainKey {
        kind: DomainKind::SecurityHighWater,
        id: 3,
    },
];
type Store<'a> = PersistenceOwner<'a, 7, { flash::BODY }, { flash::SLOT }>;
struct MeshMedia {
    slots: [flash::Flash; 7],
    pending: usize,
}
impl MeshMedia {
    fn select(&mut self, d: DomainKey) -> &mut flash::Flash {
        self.pending = DOMAINS.iter().position(|v| *v == d).unwrap();
        &mut self.slots[self.pending]
    }
}
impl PersistenceProvider for MeshMedia {
    fn geometry(&self) -> ProviderGeometry {
        self.slots[0].geometry()
    }
    fn begin_load_slot(&mut self, d: DomainKey, s: u8, o: &mut [u8], t: u64) -> IoStart {
        self.select(d).begin_load_slot(d, s, o, t)
    }
    fn begin_write_inactive(&mut self, d: DomainKey, s: u8, b: &[u8], t: u64) -> IoStart {
        self.select(d).begin_write_inactive(d, s, b, t)
    }
    fn begin_readback(&mut self, d: DomainKey, s: u8, o: &mut [u8], t: u64) -> IoStart {
        self.select(d).begin_readback(d, s, o, t)
    }
    fn begin_publish_marker(
        &mut self,
        d: DomainKey,
        s: u8,
        m: &[u8; MARKER_BYTES],
        t: u64,
    ) -> IoStart {
        self.select(d).begin_publish_marker(d, s, m, t)
    }
    fn begin_load_witness(&mut self, d: DomainKey, w: &mut WitnessView, t: u64) -> IoStart {
        self.select(d).begin_load_witness(d, w, t)
    }
    fn begin_advance_witness(&mut self, d: DomainKey, a: u64, b: u64, t: u64) -> IoStart {
        self.select(d).begin_advance_witness(d, a, b, t)
    }
    fn poll(
        &mut self,
        t: u64,
        p: IoPhase,
        o: Option<&mut [u8]>,
        w: Option<&mut WitnessView>,
    ) -> IoStart {
        self.slots[self.pending].poll(t, p, o, w)
    }
}
fn with_store(f: impl FnOnce(&mut Store<'_>, &mut MeshMedia)) {
    let mut m = MeshMedia {
        slots: DOMAINS.map(flash::Flash::new),
        pending: 0,
    };
    let gate = ProviderGate::new();
    let entries = DOMAINS.map(|d| ManifestEntry {
        domain: d,
        body_capacity_bytes: 1024,
        slot_capacity_bytes: 1136,
        schema_id: if d == AUTH {
            AUTHORITY_SCHEMA_ID
        } else if d.kind == DomainKind::IdentityBinding {
            BINDING_SCHEMA_ID
        } else {
            SESSION_SCHEMA_ID
        },
        schema_version: 1,
        digest_suite: DIGEST_BLAKE2S_128,
        witness_policy: WITNESS_INDEPENDENT_MONOTONIC,
        provider_atomicity_class: ATOMIC_COMMIT_MARKER_16,
    });
    let bindings = DOMAINS.map(|d| DomainBinding {
        domain: d,
        business_owner_instance: if d.kind == DomainKind::IdentityBinding {
            21
        } else {
            22
        },
        domain_generation: 1,
    });
    let mut p = Store::uninit();
    p.init(
        &PersistenceConfig {
            runtime_instance: 1,
            owner_instance: 3,
            required_domain_mask: 127,
            manifest: Manifest {
                protocol_manifest_version: 1,
                storage_layout_version: 1,
                composition_feature_bits: 0,
                profile_id: 1,
                entries: &entries,
            },
            bindings: &bindings,
            provider_gate: &gate,
        },
        &m,
    )
    .unwrap();
    p.start_recovery().unwrap();
    pump(&mut p, &mut m, 1);
    assert_eq!(p.lifecycle(), Lifecycle::Ready);
    f(&mut p, &mut m);
}
fn pump(p: &mut Store<'_>, m: &mut MeshMedia, t: u64) {
    for _ in 0..128 {
        p.step(m, t, 1).unwrap();
    }
}
fn identity_base(p: &Store<'_>, d: DomainKey, n: u32) -> IdentityDurabilityBase {
    let mut body = [0; flash::BODY];
    let length = p.copy_body(d, &mut body).unwrap();
    IdentityDurabilityBase::from_view(
        1,
        21,
        p.domain_get(d).unwrap(),
        &body[..length],
        100_000,
        n,
        &mut CodecWorkspace::new(),
    )
    .unwrap()
}
fn bindings<'g>(
    g: &'g CallbackGate,
    domains: &'g [DomainKey],
    p: &mut Store<'_>,
    m: &mut MeshMedia,
) -> (IdentityOwner<'g, 3>, [BindingHandle; 3]) {
    let lease = LeasePolicy {
        local_timer_max_slow_ppm: 0,
        local_timer_resolution_us: 1,
        local_timer_read_uncertainty_us: 0,
        timer_read_uncertainty_known: true,
        local_policy_max_lease_us: 100_000,
    };
    let mut i = IdentityOwner::new(IdentityConfig {
        runtime_instance: 1,
        owner_instance: 5,
        persistence_business_owner_instance: 21,
        persistence_owner_instance: 3,
        realm: RealmId::new(1).unwrap(),
        address_width: AddressWidth::A1,
        provider_gate: g,
        authority_domain: AUTH,
        binding_domains: domains,
        challenge_lifetime_us: 100_000,
        authority_lease_policy: lease,
        binding_lease_policy: lease,
    })
    .unwrap();
    let t = authority_transcript(cookie_transcript_for_test());
    let h = i
        .begin_authority_challenge(Principal::new(t.authority_principal).unwrap(), 7, 3, 2)
        .unwrap();
    let fresh = AuthorityFreshness {
        verifier_principal: Principal::new(t.authority_principal).unwrap(),
        challenge_nonce: 7,
        transaction_id: 3,
        authority_lease_sequence: 1,
        max_remaining_lease_us: 8000,
        binding_lease_id: [0; 16],
        binding_generation: 0,
        proof_transcript_hash: t.freshness_proof_transcript_hash,
    };
    let (h, r) = i
        .prepare_authority(
            &mut Verifier {
                transcript: t,
                event: BootstrapEvent::AuthorityProof,
            },
            h,
            AuthorityTransitionKind::Initial,
            epoch(&t),
            fresh,
            IdentityProof {
                bytes: SignedEvents::sign(BootstrapEvent::AuthorityProof, &t).bytes(),
            },
            identity_base(p, AUTH, 1),
            3,
        )
        .unwrap();
    let ph = p.submit(&r.persistence_request(), 4).unwrap();
    i.bind_authority_persistence(h, ph).unwrap();
    pump(p, m, 5);
    i.activate_authority(h, ph, p.proof_get(ph).unwrap(), 5)
        .unwrap();
    p.proof_retire(ph).unwrap();
    let hs = core::array::from_fn(|index| {
        let mut t = device_transcript(t);
        t.address_binding_generation = 1;
        t.binding_lease_duration_us = 3000;
        t.binding_mode = 2;
        t.joining_device_principal = [u8::try_from(0x11 + index * 0x11).unwrap(); 16];
        t.proposed_address = 7 + u32::try_from(index).unwrap();
        t.binding_lease_id = [u8::try_from(index + 1).unwrap(); 16];
        let h = i
            .begin_binding_challenge(Principal::new(t.joining_device_principal).unwrap(), 7, 3, 6)
            .unwrap();
        let c = BindingCertificate {
            binding: IdentityBinding {
                realm: RealmId::new(1).unwrap(),
                address_width: AddressWidth::A1,
                address: NodeAddress::new(t.proposed_address, AddressWidth::A1).unwrap(),
                generation: BindingGeneration::active(1).unwrap(),
                principal: Principal::new(t.joining_device_principal).unwrap(),
            },
            authority_principal: Principal::new(t.authority_principal).unwrap(),
            authority_generation: AddressAuthorityGeneration::new(t.authority_generation).unwrap(),
            lease_id: t.binding_lease_id,
            authority_lease_sequence: 1,
            lease_duration_us: 3000,
            mode: AddressMode::Leased,
        };
        let fresh = AuthorityFreshness {
            verifier_principal: c.binding.principal,
            challenge_nonce: 7,
            transaction_id: 3,
            authority_lease_sequence: 1,
            max_remaining_lease_us: 3000,
            binding_lease_id: c.lease_id,
            binding_generation: 1,
            proof_transcript_hash: t.freshness_proof_transcript_hash,
        };
        let (h, r) = i
            .prepare_binding(
                &mut Verifier {
                    transcript: t,
                    event: BootstrapEvent::AddressOffer,
                },
                h,
                c,
                fresh,
                IdentityProof {
                    bytes: SignedEvents::sign(BootstrapEvent::AddressOffer, &t).bytes(),
                },
                identity_base(p, DOMAINS[1 + index], u32::try_from(index + 2).unwrap()),
                7,
            )
            .unwrap();
        let ph = p.submit(&r.persistence_request(), 8).unwrap();
        i.bind_binding_persistence(h, ph).unwrap();
        pump(p, m, 9);
        i.activate_binding(h, ph, p.proof_get(ph).unwrap(), 9)
            .unwrap();
        p.proof_retire(ph).unwrap();
        h
    });
    (i, hs)
}
fn local_binding(i: &IdentityOwner<'_, 3>, h: BindingHandle) -> Binding {
    let b = i.binding_get(h, 12).unwrap().certificate().binding;
    Binding {
        address: b.address,
        generation: b.generation,
        principal: b.principal.bytes(),
    }
}
fn activate(
    s: &mut SecurityOwner<'_, 4, 8>,
    p: &mut Store<'_>,
    m: &mut MeshMedia,
    c: HandshakeCandidate,
    domain: DomainKey,
) -> SessionHandle {
    activate_at(s, p, m, c, domain, 10)
}
fn activate_at(
    s: &mut SecurityOwner<'_, 4, 8>,
    p: &mut Store<'_>,
    m: &mut MeshMedia,
    c: HandshakeCandidate,
    domain: DomainKey,
    now: u64,
) -> SessionHandle {
    let mut crypto = ReauthCrypto {
        expected: c,
        record: None,
    };
    let facts = CurrentFacts {
        local: c.local,
        peer: c.peer,
        link_generation: c.link_generation,
        policy_generation: 1,
        now_us: now,
    };
    let mut body = [0; flash::BODY];
    let len = p.copy_body(domain, &mut body).unwrap();
    let base = DurabilityBase::from_view(
        1,
        22,
        p.domain_get(domain).unwrap(),
        &body[..len],
        100_000,
        u32::try_from(domain.id).unwrap(),
        &mut CodecWorkspace::new(),
    )
    .unwrap();
    let (h, r) = s
        .prepare_static_session(
            &mut crypto,
            c,
            HandshakeProof { bytes: b"initial" },
            base,
            facts,
        )
        .unwrap();
    let ph = p.submit(&r.persistence_request(), now).unwrap();
    s.bind_persistence(h, ph).unwrap();
    pump(p, m, now + 1);
    s.activate(
        h,
        ph,
        p.proof_get(ph).unwrap(),
        CurrentFacts {
            now_us: now + 1,
            ..facts
        },
    )
    .unwrap();
    p.proof_retire(ph).unwrap();
    h
}
struct Counter(u64);
impl C0TransactionOwner for Counter {
    fn preview(&self) -> Result<C0TransactionId> {
        C0TransactionId::new(self.0)
    }
    fn burn(&mut self, v: C0TransactionId) -> Result<()> {
        assert_eq!(v.get(), self.0);
        self.0 += 1;
        Ok(())
    }
}
struct Hop(u32);
impl HopSequenceOwner for Hop {
    fn preview(&self) -> Result<HopSequence> {
        HopSequence::new(self.0)
    }
    fn burn(&mut self, v: HopSequence) -> Result<()> {
        assert_eq!(v.get(), self.0);
        self.0 += 1;
        Ok(())
    }
}
struct OriginCounter(u32);
impl OriginSequenceOwner for OriginCounter {
    fn preview(&self) -> Result<OriginSequence> {
        OriginSequence::new(self.0)
    }
    fn burn(&mut self, value: OriginSequence) -> Result<()> {
        assert_eq!(value.get(), self.0);
        self.0 += 1;
        Ok(())
    }
}
struct Counters(Vec<(SessionHandle, Counter, Hop, OriginCounter)>);
#[cfg(feature = "route-data-runtime")]
impl RoutedCounterBank for Counters {
    fn origin(&mut self, session: SessionHandle) -> Result<&mut dyn OriginSequenceOwner> {
        Ok(&mut self
            .0
            .iter_mut()
            .find(|v| v.0 == session)
            .ok_or(Error::NotFound)?
            .3)
    }
}
impl PeerCounterBank for Counters {
    fn counters(
        &mut self,
        s: SessionHandle,
    ) -> Result<(&mut dyn C0TransactionOwner, &mut dyn HopSequenceOwner)> {
        let v = self
            .0
            .iter_mut()
            .find(|v| v.0 == s)
            .ok_or(Error::NotFound)?;
        Ok((&mut v.1, &mut v.2))
    }
}
fn acl(peers: &[HandshakeCandidate]) -> Vec<AclRule> {
    peers
        .iter()
        .flat_map(|c| {
            [
                ProtocolOpcode::CapabilityAdvertise,
                ProtocolOpcode::CapabilityFragment,
                ProtocolOpcode::RouteRreq,
                ProtocolOpcode::RouteRrep,
                ProtocolOpcode::RouteRerr,
                ProtocolOpcode::RouteFragment,
            ]
            .into_iter()
            .flat_map(move |op| {
                [AccessDirection::Inbound, AccessDirection::Outbound].map(move |direction| {
                    AclRule {
                        peer_principal: c.peer.principal,
                        peer_binding_generation: c.peer.generation,
                        context_fingerprint: c.origin_fingerprint,
                        service: ServiceId::new(0x40).unwrap(),
                        protocol_opcode: op.into(),
                        direction,
                    }
                })
            })
        })
        .chain(peers.iter().flat_map(|c| {
            [c.origin_fingerprint, c.hop_fingerprint]
                .into_iter()
                .flat_map(move |fp| {
                    [AccessDirection::Inbound, AccessDirection::Outbound].map(move |direction| {
                        AclRule {
                            peer_principal: c.peer.principal,
                            peer_binding_generation: c.peer.generation,
                            context_fingerprint: fp,
                            service: ServiceId::new(0x40).unwrap(),
                            protocol_opcode: 0,
                            direction,
                        }
                    })
                })
        }))
        .collect()
}
fn security_mesh<'g>(
    g: &'g CallbackGate,
    d: &'g [SessionDomainRule],
    acl: &'g [AclRule],
) -> SecurityOwner<'g, 4, 8> {
    SecurityOwner::new(SecurityConfig {
        runtime_instance: 1,
        owner_instance: 8,
        persistence_business_owner_instance: 22,
        persistence_owner_instance: 3,
        realm: RealmId::new(1).unwrap(),
        address_width: AddressWidth::A1,
        provider_gate: g,
        session_domains: d,
        replay_reservation_lifetime_us: 1000,
        acl,
    })
    .unwrap()
}
fn route_owner(local: Binding, id: u32) -> RouteOwner<2, 4, 8, 2> {
    RouteOwner::new(
        RouteConfig {
            owner_instance: id,
            realm: RealmId::new(1).unwrap(),
            address_width: AddressWidth::A1,
            local,
            local_session_generation: PeerSessionGeneration::new(1).unwrap(),
            discovery_lifetime_us: 1000,
            discovery_retry_us: 100,
            discovery_max_attempts: 5,
            reverse_lifetime_us: 1000,
            route_lifetime_us: 2000,
            maximum_hops: HopLimit::new(8).unwrap(),
        },
        10,
    )
    .unwrap()
}
fn route_config(owner: u32) -> RouteRuntimeConfig {
    RouteRuntimeConfig {
        owner,
        realm: RealmId::new(1).unwrap(),
        width: AddressWidth::A1,
        service: ServiceId::new(0x40).unwrap(),
        lifetime_us: 1000,
        retry_us: 100,
        max_attempts: 5,
    }
}

type MeshIngress = AdapterEventIngress<2, 1, 1, 256>;
type MeshAdapter<'g> = AdapterOwner<'g, 2, 1, 1, 256>;
struct Node<'g> {
    id: usize,
    identity: IdentityOwner<'g, 3>,
    bindings: [BindingHandle; 3],
    security: SecurityOwner<'g, 4, 8>,
    caps: CapabilityOwner<3>,
    peers: [Option<JournalPeer>; 3],
    adapter: MeshAdapter<'g>,
    ingress: &'g MeshIngress,
    link: LinkHandle,
    root: RouteRuntime<'g, 3, 12, 256>,
    routes: RouteOwner<2, 4, 8, 2>,
    crypto: ReauthCrypto,
    counters: Counters,
    driver: Driver,
}
fn fixed_binding(index: usize) -> Binding {
    Binding {
        address: NodeAddress::new(7 + u32::try_from(index).unwrap(), AddressWidth::A1).unwrap(),
        generation: BindingGeneration::active(1).unwrap(),
        principal: [u8::try_from(0x11 + index * 0x11).unwrap(); 16],
    }
}
fn candidates(id: usize) -> Vec<HandshakeCandidate> {
    let targets: &[usize] = match id {
        0 | 2 => &[1],
        _ => &[0, 2],
    };
    targets
        .iter()
        .map(|&peer| {
            let mut c = initial(fixed_binding(id), fixed_binding(peer), id < peer);
            c.link_generation = LinkInstanceGeneration::new([3, 9, 17][id]).unwrap();
            c.origin_fingerprint =
                Fingerprint::new([if id.min(peer) == 0 { 5 } else { 8 }; 16]).unwrap();
            c.hop_fingerprint =
                Fingerprint::new([if id.min(peer) == 0 { 6 } else { 9 }; 16]).unwrap();
            c
        })
        .collect()
}
fn rules(cs: &[HandshakeCandidate]) -> Vec<SessionDomainRule> {
    cs.iter()
        .map(|c| SessionDomainRule {
            peer_principal: c.peer.principal,
            domain: DOMAINS[4 + usize::try_from(c.peer.address.get() - 7).unwrap()],
        })
        .collect()
}
fn node<'g>(
    id: usize,
    mtu: usize,
    g: &'g CallbackGate,
    ingress: &'g MeshIngress,
    domains: &'g [DomainKey],
    cs: &[HandshakeCandidate],
    ds: &'g [SessionDomainRule],
    acl: &'g [AclRule],
    p: &mut Store<'_>,
    m: &mut MeshMedia,
) -> Node<'g> {
    let (identity, hs) = bindings(g, domains, p, m);
    let local = local_binding(&identity, hs[id]);
    assert_eq!(local, fixed_binding(id));
    let mut security = security_mesh(g, ds, acl);
    let mut adapter = MeshAdapter::new(201 + u32::try_from(id).unwrap(), ingress).unwrap();
    let link = adapter
        .open_link(usize::from(id == 2), [3, 9, 17][id], mtu)
        .unwrap();
    adapter.try_set_rx_enabled(true).unwrap();
    let mut peers = [None; 3];
    let mut counters = Counters(vec![]);
    for (c, d) in cs.iter().zip(ds) {
        let session = activate(&mut security, p, m, *c, d.domain);
        let idx = usize::try_from(c.peer.address.get() - 7).unwrap();
        peers[idx] = Some(JournalPeer {
            session,
            facts: CurrentFacts {
                local: c.local,
                peer: c.peer,
                link_generation: c.link_generation,
                policy_generation: 1,
                now_us: 12,
            },
            fingerprint: c.origin_fingerprint,
            link,
        });
        counters
            .0
            .push((session, Counter(1), Hop(1), OriginCounter(1)));
    }
    let caps = CapabilityOwner::new(CapabilityConfig {
        runtime_instance: 1,
        security_owner_instance: 8,
        realm: RealmId::new(1).unwrap(),
        local_record: super::capability_join::record([3, 9, 17][id], u32::try_from(mtu).unwrap()),
        capability_lease_us: 2500,
        discovery_lease_us: 2500,
    })
    .unwrap();
    Node {
        id,
        identity,
        bindings: hs,
        security,
        caps,
        peers,
        adapter,
        ingress,
        link,
        root: RouteRuntime::new(route_config(50 + u32::try_from(id).unwrap()), g).unwrap(),
        routes: route_owner(local, 70 + u32::try_from(id).unwrap()),
        crypto: ReauthCrypto {
            expected: cs[0],
            record: None,
        },
        counters,
        driver: Driver::new(DriverSubmit::Complete(TerminalOutcome::Success)),
    }
}
fn with_mesh(f: impl FnOnce(&mut Node<'_>, &mut Node<'_>, &mut Node<'_>)) {
    with_mesh_mtu(256, f);
}
fn with_mesh_mtu(mtu: usize, f: impl FnOnce(&mut Node<'_>, &mut Node<'_>, &mut Node<'_>)) {
    with_mesh_candidates(mtu, false, f);
}
fn with_mesh_candidates(
    mtu: usize,
    endpoints: bool,
    f: impl FnOnce(&mut Node<'_>, &mut Node<'_>, &mut Node<'_>),
) {
    with_mesh_stores(mtu, endpoints, |a, b, c, _, _, _, _| f(a, b, c));
}
fn with_mesh_stores(
    mtu: usize,
    endpoints: bool,
    f: impl FnOnce(
        &mut Node<'_>,
        &mut Node<'_>,
        &mut Node<'_>,
        &mut Store<'_>,
        &mut MeshMedia,
        &mut Store<'_>,
        &mut MeshMedia,
    ),
) {
    with_mesh_all_stores(mtu, endpoints, |a, b, c, ap, am, _, _, cp, cm| {
        f(a, b, c, ap, am, cp, cm);
    });
}
fn with_mesh_all_stores(
    mtu: usize,
    endpoints: bool,
    f: impl FnOnce(
        &mut Node<'_>,
        &mut Node<'_>,
        &mut Node<'_>,
        &mut Store<'_>,
        &mut MeshMedia,
        &mut Store<'_>,
        &mut MeshMedia,
        &mut Store<'_>,
        &mut MeshMedia,
    ),
) {
    with_store(|ap, am| {
        with_store(|bp, bm| {
            with_store(|cp, cm| {
                let ag = CallbackGate::new(101).unwrap();
                let bg = CallbackGate::new(102).unwrap();
                let cg = CallbackGate::new(103).unwrap();
                let ai = MeshIngress::new(201).unwrap();
                let bi = MeshIngress::new(202).unwrap();
                let ci = MeshIngress::new(203).unwrap();
                let domains = [DOMAINS[1], DOMAINS[2], DOMAINS[3]];
                let mut ac = candidates(0);
                let bc = candidates(1);
                let mut cc = candidates(2);
                if endpoints {
                    ac.push(c1_path::endpoint_candidate(0, 2));
                    cc.push(c1_path::endpoint_candidate(2, 0));
                }
                let ad = rules(&ac);
                let bd = rules(&bc);
                let cd = rules(&cc);
                let aa = acl(&ac);
                let ba = acl(&bc);
                let ca = acl(&cc);
                let mut a = node(0, mtu, &ag, &ai, &domains, &ac, &ad, &aa, ap, am);
                let mut b = node(1, mtu, &bg, &bi, &domains, &bc, &bd, &ba, bp, bm);
                let mut c = node(2, mtu, &cg, &ci, &domains, &cc, &cd, &ca, cp, cm);
                advertise(&mut a, &mut b);
                advertise(&mut b, &mut a);
                advertise(&mut b, &mut c);
                advertise(&mut c, &mut b);
                f(&mut a, &mut b, &mut c, ap, am, bp, bm, cp, cm);
            });
        });
    });
}
fn packet_context(
    peer: JournalPeer,
    tx: C0TransactionId,
    op: ProtocolOpcode,
    outbound: bool,
) -> OriginContext {
    OriginContext::C0 {
        fingerprint: peer.fingerprint,
        transaction_id: tx,
        opcode: op,
        direction: if (peer.facts.local.principal < peer.facts.peer.principal) == outbound {
            SenderDirection::AscendingPrincipal
        } else {
            SenderDirection::DescendingPrincipal
        },
    }
}
fn advertise(from: &mut Node<'_>, to: &mut Node<'_>) {
    advertise_at(from, to, 12, None);
}
fn advertise_at(
    from: &mut Node<'_>,
    to: &mut Node<'_>,
    now: u64,
    previous: Option<PeerCapabilityRef>,
) -> PeerCapabilityRef {
    advertise_record_at(from, to, now, previous, None)
}
fn advertise_record_at(
    from: &mut Node<'_>,
    to: &mut Node<'_>,
    now: u64,
    previous: Option<PeerCapabilityRef>,
    record: Option<ucn_capability::CapabilityRecord>,
) -> PeerCapabilityRef {
    let mut peer = from.peers[to.id].unwrap();
    let mut receive = to.peers[from.id].unwrap();
    peer.facts.now_us = now;
    receive.facts.now_us = now;
    let mut published = None;
    let mut body = [0; 68];
    encode_capability_record(record.unwrap_or(from.caps.local().0), &mut body).unwrap();
    let parts = if from.adapter.link_mtu(from.link).unwrap() < 129 {
        2
    } else {
        1
    };
    let mut whole = [0; 68];
    for index in 0..parts {
        let mut fragment = [0; 44];
        let (op, body) = if parts == 2 {
            let range = ucn_capability::capability_fragment_range(
                ProtocolOpcode::CapabilityAdvertise,
                68,
                index,
            )
            .unwrap();
            let length = ucn_capability::encode_capability_fragment(
                ucn_capability::CapabilityFragment {
                    opcode: ProtocolOpcode::CapabilityAdvertise,
                    total: 68,
                    index,
                    chunk: &body[range],
                },
                &mut fragment,
            )
            .unwrap();
            (ProtocolOpcode::CapabilityFragment, &fragment[..length])
        } else {
            (ProtocolOpcode::CapabilityAdvertise, &body[..])
        };
        let (counter, hop) = from.counters.counters(peer.session).unwrap();
        let tx = counter.preview().unwrap();
        let mut prefix = [0; 29];
        ucn_wire::CommonHeader {
            contract: HeaderContract::C0,
            origin_security: OriginSecurity::O1,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::OneWay,
            payload_kind: PayloadKind::Control,
            traffic_class: TrafficClass::Q2,
            hop_limit: HopLimit::new(1).unwrap(),
        }
        .encode(&mut prefix);
        prefix[3..7].copy_from_slice(&1u32.to_be_bytes());
        prefix[7..9].copy_from_slice(
            &u16::try_from(peer.facts.local.address.get())
                .unwrap()
                .to_be_bytes(),
        );
        prefix[9..11].copy_from_slice(
            &u16::try_from(peer.facts.peer.address.get())
                .unwrap()
                .to_be_bytes(),
        );
        prefix[11..15].copy_from_slice(&1u32.to_be_bytes());
        prefix[15..19].copy_from_slice(&1u32.to_be_bytes());
        prefix[19..27].copy_from_slice(&tx.get().to_be_bytes());
        prefix[27..29].copy_from_slice(&u16::from(op).to_be_bytes());
        let access = |p: JournalPeer, direction| AccessRequest {
            session: p.session,
            context_fingerprint: p.fingerprint,
            service: ServiceId::new(0x40).unwrap(),
            protocol_opcode: op.into(),
            direction,
        };
        let mut packet = [0; 256];
        let length = from
            .security
            .protect_packet(
                &mut from.crypto,
                OriginCounterOwner::Transaction(counter),
                peer.session,
                peer.facts,
                access(peer, AccessDirection::Outbound),
                PacketPlan {
                    prefix: &prefix,
                    payload: body,
                    canonical_identity: &prefix[3..],
                    origin_context: packet_context(peer, tx, op, true),
                    hop_profile: HopProfile::H1,
                },
                Some(hop),
                &mut SecurityPacketWorkspace::<320>::new(),
                &mut packet,
            )
            .unwrap();
        let token = from.adapter.tx_reserve(from.link, 0).unwrap();
        from.adapter
            .tx_submit(token, &packet[..length], &mut from.driver)
            .unwrap();
        from.adapter.tx_retire(token).unwrap();
        to.ingress
            .rx_publish(
                to.link,
                &packet[..length],
                RxMeta {
                    sender_discriminator: 100 + u32::try_from(from.id).unwrap(),
                    timestamp_us: now,
                },
            )
            .unwrap();
        let mut raw = [0; 256];
        let claim = to.adapter.rx_claim(&mut raw).unwrap();
        let mut out = [0; 68];
        let opened = to
            .security
            .open_packet(
                &mut to.crypto,
                receive.session,
                receive.facts,
                access(receive, AccessDirection::Inbound),
                OpenPacketPlan {
                    packet: &raw[..claim.frame_bytes],
                    prefix_bytes: 29,
                    canonical_identity: &raw[3..29],
                    origin_context: packet_context(receive, tx, op, false),
                    hop_profile: HopProfile::H1,
                },
                &mut SecurityPacketWorkspace::<320>::new(),
                &mut out,
            )
            .unwrap();
        if parts == 2 {
            let piece =
                ucn_capability::decode_capability_fragment(&out[..opened.payload_bytes]).unwrap();
            let range =
                ucn_capability::capability_fragment_range(piece.opcode, 68, piece.index).unwrap();
            whole[range].copy_from_slice(piece.chunk);
        } else {
            whole.copy_from_slice(&out[..opened.payload_bytes]);
        }
        let auth = to
            .security
            .authenticated_peer_view(receive.session, receive.facts, to.link.slot() + 1)
            .unwrap();
        to.security
            .replay_preflight(&opened.replay, receive.facts)
            .unwrap();
        if index + 1 == parts {
            let record = decode_capability_record(&whole).unwrap();
            published = Some(if let Some(old) = previous {
                to.caps.replace_session(old, auth, record, now).unwrap()
            } else {
                to.caps.ingest_advertise(auth, record, now).unwrap()
            });
        }
        to.security
            .replay_commit(opened.replay, receive.facts)
            .unwrap();
        to.adapter.rx_retire(claim.token).unwrap();
    }
    published.unwrap()
}
fn register(node: &mut Node<'_>) {
    for remote in 0..3 {
        if let Some(mut peer) = node.peers[remote] {
            peer.facts.now_us = 20;
            let auth = node
                .security
                .authenticated_peer_view(peer.session, peer.facts, node.link.slot() + 1)
                .unwrap();
            let cap = PeerCapabilityRef {
                runtime_instance: auth.runtime_instance(),
                security_owner_instance: auth.security_owner_instance(),
                realm: RealmId::new(1).unwrap(),
                principal: peer.facts.peer.principal,
                binding: peer.facts.peer,
                session_generation: auth.session_generation().get(),
                ingress_link_id: node.link.slot() + 1,
                ingress_link_generation: node.link.instance_generation(),
            };
            node.root
                .register(
                    peer,
                    [node.bindings[node.id], node.bindings[remote]],
                    &node.identity,
                    &node.security,
                    &node.caps,
                    cap,
                    &node.adapter,
                    10,
                    100 + u32::try_from(remote).unwrap(),
                    20,
                )
                .unwrap();
        }
    }
}
fn peer_reference(node: &Node<'_>, remote: usize, now: u64) -> (JournalPeer, PeerCapabilityRef) {
    let mut peer = node.peers[remote].unwrap();
    peer.facts.now_us = now;
    let auth = node
        .security
        .authenticated_peer_view(peer.session, peer.facts, node.link.slot() + 1)
        .unwrap();
    (
        peer,
        PeerCapabilityRef {
            runtime_instance: auth.runtime_instance(),
            security_owner_instance: auth.security_owner_instance(),
            realm: auth.realm(),
            principal: peer.facts.peer.principal,
            binding: peer.facts.peer,
            session_generation: auth.session_generation().get(),
            ingress_link_id: node.link.slot() + 1,
            ingress_link_generation: node.link.instance_generation(),
        },
    )
}

#[test]
fn one_slot_origin_accepts_reply_while_original_request_holder_still_occupies_queue() {
    with_mesh(|a, b, _c| {
        let mut small =
            RouteRuntime::<1, 1, 256>::new(route_config(99), a.identity.callback_gate()).unwrap();
        let (peer, cap) = peer_reference(a, 1, 20);
        let count = small.counts();
        let mut bad = peer;
        bad.fingerprint = Fingerprint::new([0x51; 16]).unwrap();
        assert_eq!(
            small.register(
                bad,
                [a.bindings[0], a.bindings[1]],
                &a.identity,
                &a.security,
                &a.caps,
                cap,
                &a.adapter,
                10,
                101,
                20
            ),
            Err(Error::Security)
        );
        assert_eq!(small.counts(), count);
        assert_eq!(
            small.register(
                peer,
                [a.bindings[1], a.bindings[0]],
                &a.identity,
                &a.security,
                &a.caps,
                cap,
                &a.adapter,
                10,
                101,
                20
            ),
            Err(Error::Access)
        );
        assert_eq!(small.counts(), count);
        small
            .register(
                peer,
                [a.bindings[0], a.bindings[1]],
                &a.identity,
                &a.security,
                &a.caps,
                cap,
                &a.adapter,
                10,
                101,
                20,
            )
            .unwrap();
        register(b);
        small
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(1).address,
                1,
                100,
                21,
            )
            .unwrap();
        small
            .drive(
                &a.identity,
                &mut a.security,
                &a.caps,
                &mut a.adapter,
                &mut a.crypto,
                &mut a.counters,
                &mut a.driver,
                &mut SecurityPacketWorkspace::<320>::new(),
                22,
            )
            .unwrap();
        assert_eq!(small.counts().1, 1);
        assert_eq!(small.counts().2, 1);
        let packet = a.driver.frames.last().unwrap().clone();
        deliver_route(0, b, &packet, 23).unwrap();
        let reply = drive_node(b, 24).unwrap();
        a.ingress
            .rx_publish(
                a.link,
                &reply,
                RxMeta {
                    timestamp_us: 25,
                    sender_discriminator: 101,
                },
            )
            .unwrap();
        small
            .receive(
                &mut a.routes,
                &a.identity,
                &mut a.security,
                &a.caps,
                &mut a.adapter,
                &mut a.crypto,
                &mut SecurityPacketWorkspace::<320>::new(),
                25,
            )
            .unwrap();
        assert_eq!(a.routes.counts().0, 0);
        assert_eq!(a.routes.counts().2, 1);
        small
            .retire(&mut a.adapter, &mut a.driver, &mut a.routes, 26)
            .unwrap();
        assert_eq!(small.counts().1, 0);
        assert!(a.adapter.tx_is_quiescent());
    });
}

#[test]
fn full_frame_requirement_honors_local_processing_ceiling_before_discovery() {
    with_mesh(|a, b, _c| {
        // Actual replacement setup is before any Root registration. Publish the new
        // local capability as checked-next generation through real H1 frames.
        let mut local = a.caps.local().0;
        local.capability_generation = 2;
        local.link.processing_frame_mtu = 127;
        a.caps = CapabilityOwner::new(CapabilityConfig {
            runtime_instance: 1,
            security_owner_instance: 8,
            realm: RealmId::new(1).unwrap(),
            local_record: local,
            capability_lease_us: 2500,
            discovery_lease_us: 2500,
        })
        .unwrap();
        advertise(b, a);
        advertise(a, b);
        register(a);
        let before = a.routes.counts();
        let jobs = a.root.counts();
        // 100 business bytes + C1(A1)13 + O1(16) + H1(16) = 145 frame bytes.
        assert_eq!(
            a.root.discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(1).address,
                1,
                145,
                21
            ),
            Err(Error::NotFound)
        );
        assert_eq!(a.routes.counts(), before);
        assert_eq!(a.root.counts(), jobs);
        assert!(
            a.root
                .discover(
                    &mut a.routes,
                    &a.identity,
                    &a.security,
                    &a.caps,
                    &a.adapter,
                    fixed_binding(1).address,
                    1,
                    127,
                    22
                )
                .is_ok()
        );
    });
}

#[test]
fn idle_visits_keep_clock_highwater_and_invalid_requirements_never_create_work() {
    with_mesh(|a, b, _c| {
        register(a);
        register(b);
        assert!(drive_node(a, 100).is_none());
        let before = a.routes.counts();
        assert_eq!(
            a.root.discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(1).address,
                1,
                100,
                99
            ),
            Err(Error::State)
        );
        assert_eq!(
            a.root.discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(1).address,
                1,
                0,
                101
            ),
            Err(Error::Argument)
        );
        assert_eq!(a.routes.counts(), before);
        assert_eq!(a.root.counts().1, 0);
    });
}
fn drive_node(node: &mut Node<'_>, now: u64) -> Option<Vec<u8>> {
    let before = node.driver.frames.len();
    for _ in 0..12 {
        let result = node.root.drive(
            &node.identity,
            &mut node.security,
            &node.caps,
            &mut node.adapter,
            &mut node.crypto,
            &mut node.counters,
            &mut node.driver,
            &mut SecurityPacketWorkspace::<320>::new(),
            now,
        );
        assert!(
            result.is_ok() || result == Err(Error::InDoubt),
            "drive failed: {result:?}"
        );
        if node.driver.frames.len() != before {
            return node.driver.frames.last().cloned();
        }
    }
    None
}
fn retired(node: &mut Node<'_>, now: u64) {
    for _ in 0..12 {
        node.root
            .retire(&mut node.adapter, &mut node.driver, &mut node.routes, now)
            .unwrap();
    }
}
fn deliver_route(from_id: usize, to: &mut Node<'_>, bytes: &[u8], now: u64) -> Result<()> {
    to.ingress.rx_publish(
        to.link,
        bytes,
        RxMeta {
            sender_discriminator: 100 + u32::try_from(from_id).unwrap(),
            timestamp_us: now,
        },
    )?;
    to.root.receive(
        &mut to.routes,
        &to.identity,
        &mut to.security,
        &to.caps,
        &mut to.adapter,
        &mut to.crypto,
        &mut SecurityPacketWorkspace::<320>::new(),
        now,
    )
}

// An authenticated adversarial neighbor: real keys/counters and Provider, not
// a forged OpenedPacket. 认证对抗邻居：实际计数器与Provider，不伪造认证DTO。
fn signed_control(
    from: &mut Node<'_>,
    to: usize,
    opcode: ProtocolOpcode,
    body: &[u8],
    now: u64,
) -> Vec<u8> {
    let mut peer = from.peers[to].unwrap();
    peer.facts.now_us = now;
    let (counter, hop) = from.counters.counters(peer.session).unwrap();
    let tx = counter.preview().unwrap();
    let mut prefix = [0; 29];
    ucn_wire::CommonHeader {
        contract: HeaderContract::C0,
        origin_security: OriginSecurity::O1,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: InteractionRole::OneWay,
        payload_kind: PayloadKind::Control,
        traffic_class: TrafficClass::Q0,
        hop_limit: HopLimit::new(1).unwrap(),
    }
    .encode(&mut prefix);
    prefix[3..7].copy_from_slice(&1u32.to_be_bytes());
    prefix[7..9].copy_from_slice(
        &u16::try_from(peer.facts.local.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[9..11].copy_from_slice(
        &u16::try_from(peer.facts.peer.address.get())
            .unwrap()
            .to_be_bytes(),
    );
    prefix[11..15].copy_from_slice(&peer.facts.local.generation.get().to_be_bytes());
    prefix[15..19].copy_from_slice(&peer.facts.peer.generation.get().to_be_bytes());
    prefix[19..27].copy_from_slice(&tx.get().to_be_bytes());
    prefix[27..29].copy_from_slice(&u16::from(opcode).to_be_bytes());
    let mut raw = [0; 256];
    let length = from
        .security
        .protect_packet(
            &mut from.crypto,
            OriginCounterOwner::Transaction(counter),
            peer.session,
            peer.facts,
            AccessRequest {
                session: peer.session,
                context_fingerprint: peer.fingerprint,
                service: ServiceId::new(0x40).unwrap(),
                protocol_opcode: opcode.into(),
                direction: AccessDirection::Outbound,
            },
            PacketPlan {
                prefix: &prefix,
                payload: body,
                canonical_identity: &prefix[3..],
                origin_context: packet_context(peer, tx, opcode, true),
                hop_profile: HopProfile::H1,
            },
            Some(hop),
            &mut SecurityPacketWorkspace::<320>::new(),
            &mut raw,
        )
        .unwrap();
    raw[..length].to_vec()
}

#[test]
fn fragment_bundle_swap_replay_and_last_piece_without_first_are_rejected() {
    with_mesh_mtu(127, |a, b, c| {
        register(a);
        register(b);
        register(c);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        let ab = drive_node(a, 22).unwrap();
        deliver_route(0, b, &ab, 23).unwrap();
        retired(a, 24);
        let bc = drive_node(b, 25).unwrap();
        deliver_route(1, c, &bc, 26).unwrap();
        retired(b, 27);
        let first = drive_node(c, 28).unwrap();
        deliver_route(2, b, &first, 29).unwrap();
        retired(c, 30);
        let last = drive_node(c, 31).unwrap();
        let saved = b.routes.counts();
        let mut changed = ucn_routing::RouteFragment::decode(&last[29..last.len() - 32]).unwrap();
        changed.bundle = changed.bundle.checked_next().unwrap();
        let mut raw = [0; 64];
        let length = changed.encode(&mut raw).unwrap();
        let bad = signed_control(c, 1, ProtocolOpcode::RouteFragment, &raw[..length], 32);
        assert_eq!(deliver_route(2, b, &bad, 33), Err(Error::Security));
        assert_eq!(b.routes.counts(), saved);
        deliver_route(2, b, &last, 34).unwrap();
        retired(c, 35);
        assert_eq!(b.routes.counts().2, 2);
        let replay = signed_control(
            c,
            1,
            ProtocolOpcode::RouteFragment,
            &first[29..first.len() - 32],
            36,
        );
        assert_eq!(deliver_route(2, b, &replay, 37), Err(Error::Replay));
        assert_eq!(b.routes.counts().2, 2);
        // A separate registered Peer has no assembly for this authenticated bundle.
        // The wrong last piece cannot allocate pending state by itself.
        let missing = signed_control(
            b,
            0,
            ProtocolOpcode::RouteFragment,
            &last[29..last.len() - 32],
            38,
        );
        assert_eq!(deliver_route(1, a, &missing, 39), Err(Error::Replay));
        assert_eq!(a.routes.counts().2, 0);
        // A fenced C peer must not block B from discovering through live A.
        assert!(
            b.root
                .discover(
                    &mut b.routes,
                    &b.identity,
                    &b.security,
                    &b.caps,
                    &b.adapter,
                    fixed_binding(0).address,
                    1,
                    100,
                    44
                )
                .is_ok()
        );
    });
}

#[test]
fn fragment_expiry_clears_storage_but_never_resets_bundle_highwater() {
    with_mesh_mtu(127, |a, b, c| {
        register(a);
        register(b);
        register(c);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        let ab = drive_node(a, 22).unwrap();
        deliver_route(0, b, &ab, 23).unwrap();
        retired(a, 24);
        let bc = drive_node(b, 25).unwrap();
        deliver_route(1, c, &bc, 26).unwrap();
        retired(b, 27);
        let first = drive_node(c, 28).unwrap();
        deliver_route(2, b, &first, 29).unwrap();
        retired(c, 30);
        let last = drive_node(c, 31).unwrap();
        assert_eq!(deliver_route(2, b, &last, 1029), Err(Error::Timeout));
        assert_eq!(b.routes.counts().2, 1);
        for _ in 0..30 {
            b.root
                .maintain(
                    &mut b.routes,
                    &b.identity,
                    &b.security,
                    &b.caps,
                    &b.adapter,
                    1030,
                )
                .unwrap();
        }
        let replay = signed_control(
            c,
            1,
            ProtocolOpcode::RouteFragment,
            &first[29..first.len() - 32],
            1031,
        );
        assert_eq!(deliver_route(2, b, &replay, 1032), Err(Error::Replay));
        assert!(b.routes.counts().2 <= 1);
    });
}

#[test]
fn real_three_node_authenticated_discovery_preserves_origin_and_distinct_link_namespaces() {
    with_mesh(|a, b, c| {
        register(a);
        register(b);
        register(c);
        assert_eq!(b.root.counts().0, 2);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        let ab = drive_node(a, 22).unwrap();
        deliver_route(0, b, &ab, 23).unwrap();
        retired(a, 24);
        let bc = drive_node(b, 25).unwrap();
        assert_eq!(&bc[7..9], &8u16.to_be_bytes());
        assert_eq!(&bc[9..11], &9u16.to_be_bytes());
        // Outer C0 Source B must not overwrite inner discovery Source A.
        assert_eq!(&bc[33..37], &7u32.to_be_bytes());
        deliver_route(1, c, &bc, 26).unwrap();
        retired(b, 27);
        let cb = drive_node(c, 28).unwrap();
        deliver_route(2, b, &cb, 29).unwrap();
        retired(c, 30);
        let ba = drive_node(b, 31).unwrap();
        deliver_route(1, a, &ba, 32).unwrap();
        retired(b, 33);
        retired(a, 34);
        let domain = RouteDomain {
            realm: RealmId::new(1).unwrap(),
            origin: fixed_binding(0),
            origin_session_generation: PeerSessionGeneration::new(1).unwrap(),
            destination: fixed_binding(2),
        };
        let route = a
            .routes
            .resolve(
                domain,
                RouteUseFacts {
                    now_us: 35,
                    origin_session_generation: domain.origin_session_generation,
                    destination_binding_generation: 1,
                    link_generation: LinkInstanceGeneration::new(3).unwrap(),
                },
            )
            .unwrap();
        let ResolvedRoute::Dynamic(route) = route else {
            panic!("not a dynamic route")
        };
        assert_eq!(route.next_hop().peer(), fixed_binding(1));
        assert_eq!(route.hop_count(), 2);
        assert_eq!(route.next_hop().link_generation().get(), 3);
        assert_eq!(route.cost(), 20);
        assert_eq!(a.routes.counts().0, 0);
        assert_eq!(a.root.counts().1, 0);
    });
}

fn discover_path(a: &mut Node<'_>, b: &mut Node<'_>, c: &mut Node<'_>) {
    register(a);
    register(b);
    register(c);
    a.root
        .discover(
            &mut a.routes,
            &a.identity,
            &a.security,
            &a.caps,
            &a.adapter,
            fixed_binding(2).address,
            1,
            100,
            21,
        )
        .unwrap();
    let ab = drive_node(a, 22).unwrap();
    deliver_route(0, b, &ab, 23).unwrap();
    retired(a, 24);
    let bc = drive_node(b, 25).unwrap();
    deliver_route(1, c, &bc, 26).unwrap();
    retired(b, 27);
    let cb = drive_node(c, 28).unwrap();
    deliver_route(2, b, &cb, 29).unwrap();
    retired(c, 30);
    let ba = drive_node(b, 31).unwrap();
    deliver_route(1, a, &ba, 32).unwrap();
    retired(b, 33);
    retired(a, 34);
}
#[test]
fn peer_fence_on_shared_link_sends_generation_translated_authenticated_rerr() {
    with_mesh(|a, b, c| {
        discover_path(a, b, c);
        b.security.fence(b.peers[2].unwrap().session).unwrap();
        for _ in 0..30 {
            b.root
                .maintain(
                    &mut b.routes,
                    &b.identity,
                    &b.security,
                    &b.caps,
                    &b.adapter,
                    40,
                )
                .unwrap();
        }
        // Other logical neighbor shares exactly the same physical Link. Its reverse
        // route must remain; only C's route is fenced.
        assert_eq!(b.routes.counts().2, 1);
        let packet = drive_node(b, 41).unwrap();
        assert_eq!(&packet[27..29], &0x0203u16.to_be_bytes());
        let ucn_routing::RouteControl::Error(e) = ucn_routing::RouteControl::decode(
            ProtocolOpcode::RouteRerr,
            AddressWidth::A1,
            &packet[29..128],
        )
        .unwrap() else {
            panic!("not RERR")
        };
        assert_eq!(e.route_generation, 2);
        assert_eq!(e.failed_link_generation, 9);
        assert_eq!(e.reporter_principal, fixed_binding(1).principal);
        deliver_route(1, a, &packet, 42).unwrap();
        retired(b, 43);
        assert_eq!(a.routes.counts().2, 0);
    });
}
#[test]
fn lost_reply_retries_new_outer_c0_but_preserves_inner_discovery() {
    with_mesh(|a, b, c| {
        register(a);
        register(b);
        register(c);
        let h = a
            .root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        assert_eq!(
            a.root
                .discover(
                    &mut a.routes,
                    &a.identity,
                    &a.security,
                    &a.caps,
                    &a.adapter,
                    fixed_binding(2).address,
                    1,
                    100,
                    21
                )
                .unwrap(),
            h
        );
        let ab = drive_node(a, 22).unwrap();
        deliver_route(0, b, &ab, 23).unwrap();
        retired(a, 24);
        let bc = drive_node(b, 25).unwrap();
        deliver_route(1, c, &bc, 26).unwrap();
        retired(b, 27);
        let lost = drive_node(c, 28).unwrap();
        assert!(!lost.is_empty());
        retired(c, 30);
        let retry = drive_node(a, 122).unwrap();
        assert_ne!(&ab[19..27], &retry[19..27]);
        assert_eq!(&ab[29..83], &retry[29..83]);
        deliver_route(0, b, &retry, 123).unwrap();
        retired(a, 124);
        let retry = drive_node(b, 125).unwrap();
        deliver_route(1, c, &retry, 126).unwrap();
        retired(b, 127);
        let cb = drive_node(c, 128).unwrap();
        deliver_route(2, b, &cb, 129).unwrap();
        retired(c, 130);
        let ba = drive_node(b, 131).unwrap();
        deliver_route(1, a, &ba, 132).unwrap();
        retired(b, 133);
        retired(a, 134);
        assert_eq!(a.routes.counts().0, 0);
        assert_eq!(a.routes.counts().2, 1);
    });
}
#[test]
fn tag_and_physical_discriminator_mismatch_never_change_routes() {
    with_mesh(|a, b, c| {
        register(a);
        register(b);
        register(c);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        let raw = drive_node(a, 22).unwrap();
        let counts = b.routes.counts();
        let before = b.root.counts();
        let mut bad = raw.clone();
        bad[40] ^= 1;
        assert_eq!(deliver_route(0, b, &bad, 23), Err(Error::Security));
        assert_eq!(b.routes.counts(), counts);
        assert_eq!(b.root.counts(), before);
        assert!(deliver_route(2, b, &raw, 24).is_err());
        assert_eq!(b.routes.counts(), counts);
        assert_eq!(b.root.counts(), before);
        deliver_route(0, b, &raw, 25).unwrap();
        assert_eq!(b.routes.counts().1, 1);
    });
}
#[test]
fn backpressure_then_success_preserves_work_and_unknown_stop_keeps_holder() {
    with_mesh(|a, b, c| {
        register(a);
        register(b);
        register(c);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        a.driver.submit = DriverSubmit::NotSubmitted;
        let mut not_submitted = None;
        for t in [22, 122, 222] {
            let packet = drive_node(a, t).unwrap();
            if let Some(first) = &not_submitted {
                assert_eq!(
                    &packet, first,
                    "NOT_SUBMITTED must retain both Origin and Hop bytes"
                );
            } else {
                not_submitted = Some(packet);
            }
            retired(a, t + 1);
            assert_eq!(a.root.counts().1, 1);
            assert_eq!(a.root.counts().2, 0);
        }
        a.driver.submit = DriverSubmit::Complete(TerminalOutcome::Success);
        let packet = drive_node(a, 322).unwrap();
        assert_eq!(Some(&packet), not_submitted.as_ref());
        deliver_route(0, b, &packet, 323).unwrap();
        retired(a, 324);
        a.driver.submit = DriverSubmit::Unknown;
        a.driver.cancel = DriverCancel::Unknown;
        assert!(drive_node(a, 422).is_some());
        a.root.stop(&mut a.adapter).unwrap();
        for _ in 0..12 {
            let r = a
                .root
                .retire(&mut a.adapter, &mut a.driver, &mut a.routes, 423);
            assert!(r.is_ok() || r == Err(Error::InDoubt));
        }
        assert_eq!(a.root.counts().2, 1);
        assert!(!a.root.quiescent(&a.adapter));
        a.adapter.reopen_link(a.link, 4, 256).unwrap();
        assert_eq!(
            a.adapter.tx_retire(*a.driver.tokens.last().unwrap()),
            Err(Error::InDoubt)
        );
        a.ingress
            .tx_complete(
                *a.driver.tokens.last().unwrap(),
                TerminalOutcome::Failure(Error::Access),
            )
            .unwrap();
        retired(a, 424);
        assert!(a.root.quiescent(&a.adapter));
    });
}
#[test]
fn reverse_timeout_cannot_erase_outstanding_reply_driver_obligation() {
    with_mesh(|a, b, c| {
        register(a);
        register(b);
        register(c);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        let ab = drive_node(a, 22).unwrap();
        deliver_route(0, b, &ab, 23).unwrap();
        retired(a, 24);
        let bc = drive_node(b, 25).unwrap();
        deliver_route(1, c, &bc, 26).unwrap();
        retired(b, 27);
        let cb = drive_node(c, 28).unwrap();
        deliver_route(2, b, &cb, 29).unwrap();
        retired(c, 30);
        b.driver.submit = DriverSubmit::Unknown;
        b.driver.cancel = DriverCancel::Unknown;
        drive_node(b, 31).unwrap();
        for _ in 0..30 {
            let _ = b.routes.expire_one(2100);
        }
        assert_eq!(b.routes.counts().1, 1);
        b.root.stop(&mut b.adapter).unwrap();
        for _ in 0..12 {
            let r = b
                .root
                .retire(&mut b.adapter, &mut b.driver, &mut b.routes, 2101);
            assert!(r.is_ok() || r == Err(Error::InDoubt));
        }
        assert_eq!(b.root.counts().2, 1);
        assert_eq!(b.routes.counts().1, 1);
        b.adapter.reopen_link(b.link, 10, 256).unwrap();
        assert_eq!(
            b.adapter.tx_retire(*b.driver.tokens.last().unwrap()),
            Err(Error::InDoubt)
        );
        b.ingress
            .tx_complete(
                *b.driver.tokens.last().unwrap(),
                TerminalOutcome::Failure(Error::Access),
            )
            .unwrap();
        retired(b, 2102);
        assert!(b.root.quiescent(&b.adapter));
        for _ in 0..30 {
            let _ = b.routes.expire_one(2103);
        }
        assert_eq!(b.routes.counts().1, 0);
    });
}

#[test]
fn delayed_reply_cannot_extend_the_original_reverse_send_deadline() {
    with_mesh(|a, b, c| {
        register(a);
        register(b);
        register(c);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        let ab = drive_node(a, 22).unwrap();
        deliver_route(0, b, &ab, 23).unwrap(); // B's original reverse expires at 1023.
        retired(a, 24);
        let bc = drive_node(b, 25).unwrap();
        deliver_route(1, c, &bc, 26).unwrap();
        retired(b, 27);
        let cb = drive_node(c, 28).unwrap();
        retired(c, 30);
        deliver_route(2, b, &cb, 1021).unwrap();
        b.driver.submit = DriverSubmit::NotSubmitted;
        assert!(drive_node(b, 1022).is_some());
        retired(b, 1023);
        assert!(
            drive_node(b, 1122).is_none(),
            "late RREP must not renew Reverse expiry"
        );
        assert_eq!(b.root.counts().2, 0);
    });
}

#[test]
fn duplicate_rreq_cannot_renew_target_or_relay_control_jobs() {
    for destination in [1, 2] {
        with_mesh(|a, b, c| {
            register(a);
            register(b);
            register(c);
            a.root
                .discover(
                    &mut a.routes,
                    &a.identity,
                    &a.security,
                    &a.caps,
                    &a.adapter,
                    fixed_binding(destination).address,
                    1,
                    100,
                    21,
                )
                .unwrap();
            let first = drive_node(a, 22).unwrap();
            deliver_route(0, b, &first, 23).unwrap();
            retired(a, 24);
            b.driver.submit = DriverSubmit::NotSubmitted;
            assert!(drive_node(b, 25).is_some());
            retired(b, 26);
            let retry = drive_node(a, 1020).unwrap();
            deliver_route(0, b, &retry, 1021).unwrap();
            assert!(drive_node(b, 1022).is_some());
            retired(b, 1023);
            assert!(
                drive_node(b, 1122).is_none(),
                "duplicate RREQ must not renew original Reverse expiry"
            );
            assert_eq!(b.root.counts().2, 0);
        });
    }
}

#[test]
fn actual_127_byte_mesh_fragments_reply_and_rerr_without_growing_data_header() {
    with_mesh_mtu(127, |a, b, c| {
        register(a);
        register(b);
        register(c);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        let ab = drive_node(a, 22).unwrap();
        deliver_route(0, b, &ab, 23).unwrap();
        retired(a, 24);
        let bc = drive_node(b, 25).unwrap();
        deliver_route(1, c, &bc, 26).unwrap();
        retired(b, 27);
        let before = b.routes.counts();
        for t in [28, 30] {
            let packet = drive_node(c, t).unwrap();
            assert!(packet.len() <= 127);
            assert_eq!(&packet[27..29], &0x0206u16.to_be_bytes());
            deliver_route(2, b, &packet, t + 1).unwrap();
            if t == 28 {
                assert_eq!(b.routes.counts(), before);
            }
            retired(c, t + 1);
        }
        assert_eq!(b.routes.counts().2, 2);
        for t in [32, 34] {
            let packet = drive_node(b, t).unwrap();
            assert!(packet.len() <= 127);
            deliver_route(1, a, &packet, t + 1).unwrap();
            retired(b, t + 1);
        }
        retired(a, 36);
        assert_eq!(a.routes.counts().0, 0);
        assert_eq!(a.routes.counts().2, 1);
        b.security.fence(b.peers[2].unwrap().session).unwrap();
        for _ in 0..30 {
            b.root
                .maintain(
                    &mut b.routes,
                    &b.identity,
                    &b.security,
                    &b.caps,
                    &b.adapter,
                    40,
                )
                .unwrap();
        }
        for t in [41, 43] {
            let packet = drive_node(b, t).unwrap();
            assert!(packet.len() <= 127);
            deliver_route(1, a, &packet, t + 1).unwrap();
            retired(b, t + 1);
        }
        assert_eq!(a.routes.counts().2, 0);
        assert!(
            a.driver
                .frames
                .iter()
                .chain(&b.driver.frames)
                .chain(&c.driver.frames)
                .all(|f| f.len() <= 127)
        );
    });
}

#[test]
fn lost_first_or_last_fragment_on_either_return_hop_converges_without_an_extra_ack() {
    for downstream in [true, false] {
        for lost in 0..2 {
            with_mesh_mtu(127, |a, b, c| {
                register(a);
                register(b);
                register(c);
                a.root
                    .discover(
                        &mut a.routes,
                        &a.identity,
                        &a.security,
                        &a.caps,
                        &a.adapter,
                        fixed_binding(2).address,
                        1,
                        100,
                        21,
                    )
                    .unwrap();
                let ab = drive_node(a, 22).unwrap();
                deliver_route(0, b, &ab, 23).unwrap();
                retired(a, 24);
                let bc = drive_node(b, 25).unwrap();
                deliver_route(1, c, &bc, 26).unwrap();
                retired(b, 27);
                for (index, time) in [28, 30].into_iter().enumerate() {
                    let packet = drive_node(c, time).unwrap();
                    if !downstream || index != lost {
                        let r = deliver_route(2, b, &packet, time + 1);
                        assert!(r.is_ok() || r == Err(Error::Replay));
                    }
                    retired(c, time + 1);
                }
                if !downstream {
                    for (index, time) in [32, 34].into_iter().enumerate() {
                        let packet = drive_node(b, time).unwrap();
                        if index != lost {
                            let r = deliver_route(1, a, &packet, time + 1);
                            assert!(r.is_ok() || r == Err(Error::Replay));
                        }
                        retired(b, time + 1);
                    }
                }
                assert_eq!(a.routes.counts().0, 1);
                assert_eq!(a.routes.counts().2, 0);
                let ab = drive_node(a, 122).unwrap();
                deliver_route(0, b, &ab, 123).unwrap();
                retired(a, 124);
                let bc = drive_node(b, 125).unwrap();
                deliver_route(1, c, &bc, 126).unwrap();
                retired(b, 127);
                for time in [128, 130] {
                    let packet = drive_node(c, time).unwrap();
                    deliver_route(2, b, &packet, time + 1).unwrap();
                    retired(c, time + 1);
                }
                for time in [132, 134] {
                    let packet = drive_node(b, time).unwrap();
                    deliver_route(1, a, &packet, time + 1).unwrap();
                    retired(b, time + 1);
                }
                retired(a, 136);
                assert_eq!(a.routes.counts().0, 0);
                assert_eq!(a.routes.counts().2, 1);
            });
        }
    }
}

#[test]
fn authenticated_higher_first_piece_replaces_bytes_but_never_renews_original_deadline() {
    with_mesh_mtu(127, |a, b, c| {
        register(a);
        register(b);
        register(c);
        a.root
            .discover(
                &mut a.routes,
                &a.identity,
                &a.security,
                &a.caps,
                &a.adapter,
                fixed_binding(2).address,
                1,
                100,
                21,
            )
            .unwrap();
        let ab = drive_node(a, 22).unwrap();
        deliver_route(0, b, &ab, 23).unwrap();
        retired(a, 24);
        let bc = drive_node(b, 25).unwrap();
        deliver_route(1, c, &bc, 26).unwrap();
        retired(b, 27);
        let first = drive_node(c, 28).unwrap();
        deliver_route(2, b, &first, 29).unwrap();
        retired(c, 30);
        let last = drive_node(c, 31).unwrap();
        let mut start = ucn_routing::RouteFragment::decode(&first[29..first.len() - 32]).unwrap();
        let (counter, _) = c.counters.counters(c.peers[1].unwrap().session).unwrap();
        start.bundle = counter.preview().unwrap();
        let mut raw = [0; 64];
        let length = start.encode(&mut raw).unwrap();
        let newer = signed_control(c, 1, ProtocolOpcode::RouteFragment, &raw[..length], 1028);
        deliver_route(2, b, &newer, 1028).unwrap();
        let mut end = ucn_routing::RouteFragment::decode(&last[29..last.len() - 32]).unwrap();
        end.bundle = start.bundle;
        let length = end.encode(&mut raw).unwrap();
        let newer = signed_control(c, 1, ProtocolOpcode::RouteFragment, &raw[..length], 1028);
        assert_eq!(deliver_route(2, b, &newer, 1029), Err(Error::Timeout));
        assert_eq!(b.routes.counts().2, 1);
    });
}

#[test]
fn mesh_fixture_publishes_three_actual_bindings_and_two_independent_sessions() {
    with_store(|p, m| {
        let g = CallbackGate::new(100).unwrap();
        let domains = [DOMAINS[1], DOMAINS[2], DOMAINS[3]];
        let (i, h) = bindings(&g, &domains, p, m);
        let bs = h.map(|h| local_binding(&i, h));
        let cs = [initial(bs[1], bs[0], false), initial(bs[1], bs[2], true)];
        let ds = cs.map(|c| SessionDomainRule {
            peer_principal: c.peer.principal,
            domain: if c.peer == bs[0] {
                DOMAINS[4]
            } else {
                DOMAINS[5]
            },
        });
        let a = acl(&cs);
        let mut s = security_mesh(&g, &ds, &a);
        for (c, d) in cs.into_iter().zip(ds) {
            let h = activate(&mut s, p, m, c, d.domain);
            assert!(
                s.session_get(
                    h,
                    CurrentFacts {
                        local: c.local,
                        peer: c.peer,
                        link_generation: c.link_generation,
                        policy_generation: 1,
                        now_us: 12
                    }
                )
                .is_ok()
            );
        }
    });
}
