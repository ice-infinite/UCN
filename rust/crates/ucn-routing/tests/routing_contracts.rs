//! 基础发现、SoftRoute 与 RERR 的公开合同回归。

use ucn_routing::{
    LinkRef, NanoRouteOwner, ReplyAction, RequestAction, RerrReason, ResolvedRoute, RouteConfig,
    RouteDomain, RouteUseFacts, RrepMessage, RrepPayload, StaticRoute, decode_rerr_payload,
    decode_rrep_payload, decode_rreq_payload, encode_rerr_payload, encode_rrep_payload,
    encode_rreq_payload,
};
use ucn_security::Binding;
use ucn_types::{
    AddressWidth, BindingGeneration, Error, HopLimit, LinkInstanceGeneration, NodeAddress,
    PeerSessionGeneration, RealmId,
};

fn binding(address: u32, principal: u8) -> Binding {
    Binding {
        address: NodeAddress::new(address, AddressWidth::A1).expect("address"),
        generation: BindingGeneration::active(1).expect("binding"),
        principal: [principal; 16],
    }
}

fn owner(instance: u32, local: Binding) -> NanoRouteOwner {
    NanoRouteOwner::new(
        RouteConfig {
            owner_instance: instance,
            realm: RealmId::new(7).expect("realm"),
            address_width: AddressWidth::A1,
            local,
            local_session_generation: PeerSessionGeneration::new(1).expect("session"),
            discovery_lifetime_us: 1_000,
            discovery_retry_us: 100,
            discovery_max_attempts: 4,
            reverse_lifetime_us: 800,
            route_lifetime_us: 2_000,
            maximum_hops: HopLimit::new(8).expect("hops"),
        },
        10,
    )
    .expect("owner")
}

fn link(id: u16, generation: u32, peer: Binding, cost: u32) -> LinkRef {
    LinkRef::new(
        id,
        LinkInstanceGeneration::new(generation).expect("link generation"),
        peer,
        240,
        cost,
        0x0003,
    )
    .expect("link")
}

#[test]
fn payload_codecs_are_big_endian_exact_and_zero_write_on_failure() {
    let request_payload = ucn_routing::RreqPayload {
        accumulated_cost: 0x0102_0304,
        minimum_payload_budget: 0x1122,
        required_capability_bits: 0x3344,
        flags: 0x03,
    };
    let mut request_bytes = [0_u8; 9];
    encode_rreq_payload(request_payload, &mut request_bytes).expect("rreq encode");
    assert_eq!(request_bytes, [1, 2, 3, 4, 0x11, 0x22, 0x33, 0x44, 3]);
    assert_eq!(decode_rreq_payload(&request_bytes), Ok(request_payload));

    let reply_payload = ucn_routing::RrepPayload {
        destination_principal: [0xA5; 16],
        destination_binding_generation: 0x0102_0304,
        hop_count: 2,
        accumulated_cost: 0x1122_3344,
        path_frame_mtu: 0x5566,
        capability_bits: 0x7788,
    };
    let mut reply_bytes = [0_u8; 29];
    encode_rrep_payload(reply_payload, &mut reply_bytes).expect("rrep encode");
    assert_eq!(
        &reply_bytes[16..],
        &[
            1, 2, 3, 4, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
        ]
    );
    assert_eq!(decode_rrep_payload(&reply_bytes), Ok(reply_payload));
    let target_reply = RrepPayload {
        hop_count: 0,
        ..reply_payload
    };
    let mut target_reply_bytes = [0_u8; 29];
    encode_rrep_payload(target_reply, &mut target_reply_bytes).expect("target rrep encode");
    assert_eq!(decode_rrep_payload(&target_reply_bytes), Ok(target_reply));

    let before = [0xCC_u8; 9];
    let mut output = before;
    let bad = ucn_routing::RreqPayload {
        flags: 0x80,
        ..request_payload
    };
    assert_eq!(encode_rreq_payload(bad, &mut output), Err(Error::Argument));
    assert_eq!(output, before);
    assert_eq!(
        decode_rreq_payload(&request_bytes[..8]),
        Err(Error::Malformed)
    );
}

#[test]
fn three_node_discovery_installs_only_volatile_soft_routes() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let mut node_a = owner(1, a);
    let mut node_b = owner(2, b);
    let mut node_c = owner(3, c);

    let start = node_a
        .ensure_discovery(c.address, 0x0001, 200, 0, 10)
        .expect("start");
    assert!(start.created);
    let coalesced = node_a
        .ensure_discovery(c.address, 0x0001, 200, 0, 11)
        .expect("coalesce");
    assert!(!coalesced.created);
    assert_eq!(coalesced.request, start.request);

    let forwarded = match node_b
        .on_rreq(start.request, link(1, 1, a, 10), 20)
        .expect("B rreq")
    {
        RequestAction::Forward(value) => value,
        other @ (RequestAction::Duplicate | RequestAction::LocalTarget) => {
            panic!("unexpected action: {other:?}")
        }
    };
    assert_eq!(forwarded.remaining_hops.get(), 7);
    assert_eq!(forwarded.payload.accumulated_cost, 10);
    assert_eq!(forwarded.payload.minimum_payload_budget, 200);
    assert_eq!(
        node_b.on_rreq(start.request, link(1, 1, a, 10), 21),
        Ok(RequestAction::Duplicate)
    );
    assert_eq!(
        node_c.on_rreq(forwarded, link(2, 1, b, 20), 30),
        Ok(RequestAction::LocalTarget)
    );
    let reply = node_c.make_rrep(forwarded, 0x0003, 230, 31).expect("rrep");
    let (forwarded_reply, upstream, completion) = match node_b
        .on_rrep(reply, link(2, 1, c, 20), 40)
        .expect("B rrep")
    {
        ReplyAction::Forward {
            message,
            upstream,
            completion,
        } => (message, upstream, completion),
        other @ ReplyAction::ReachedOrigin(_) => panic!("unexpected action: {other:?}"),
    };
    assert_eq!(upstream.peer(), a);
    node_b
        .complete_rrep_forward(completion, true)
        .expect("complete reply");
    assert_eq!(
        node_b.on_rreq(start.request, link(1, 1, a, 10), 41),
        Ok(RequestAction::Duplicate)
    );

    let route = match node_a
        .on_rrep(forwarded_reply, link(1, 1, b, 10), 50)
        .expect("A rrep")
    {
        ReplyAction::ReachedOrigin(route) => route,
        other @ ReplyAction::Forward { .. } => panic!("unexpected action: {other:?}"),
    };
    assert_eq!(route.domain().origin, a);
    assert_eq!(route.domain().destination, c);
    assert_eq!(route.next_hop().peer(), b);
    assert_eq!(route.path_frame_mtu(), 230);
    assert_eq!(route.capability_bits(), 0x0003);
    assert_eq!(node_a.counts().0, 0);

    let resolved = node_a
        .resolve(
            route.domain(),
            RouteUseFacts {
                now_us: 51,
                origin_session_generation: PeerSessionGeneration::new(1).expect("session"),
                destination_binding_generation: 1,
                link_generation: LinkInstanceGeneration::new(1).expect("link"),
            },
        )
        .expect("resolve");
    assert_eq!(resolved, ResolvedRoute::Dynamic(route));
    assert_eq!(
        node_a.resolve(
            route.domain(),
            RouteUseFacts {
                now_us: 2_050,
                origin_session_generation: PeerSessionGeneration::new(1).expect("session"),
                destination_binding_generation: 1,
                link_generation: LinkInstanceGeneration::new(1).expect("link"),
            },
        ),
        Err(Error::NotFound)
    );
}

#[test]
fn exact_rerr_cannot_delete_a_refreshed_or_unrelated_route() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let mut route_owner = owner(1, a);
    let start = route_owner
        .ensure_discovery(c.address, 0, 100, 0, 1)
        .expect("discovery");
    let reply = ucn_routing::RrepMessage {
        key: start.request.key,
        payload: ucn_routing::RrepPayload {
            destination_principal: c.principal,
            destination_binding_generation: c.generation.get(),
            hop_count: 1,
            accumulated_cost: 5,
            path_frame_mtu: 200,
            capability_bits: 0,
        },
    };
    let ReplyAction::ReachedOrigin(route) = route_owner
        .on_rrep(reply, link(1, 7, b, 5), 10)
        .expect("rrep")
    else {
        panic!("origin expected");
    };
    let rerr = route_owner
        .make_rerr(route, RerrReason::LinkInvalid)
        .expect("rerr");
    let mut bytes = [0_u8; 99];
    encode_rerr_payload(rerr, &mut bytes).expect("encode");
    assert_eq!(decode_rerr_payload(&bytes), Ok(rerr));

    let mut stale = rerr;
    stale.route_causal_id += 1;
    assert_eq!(route_owner.on_rerr(stale), Ok(false));
    stale = rerr;
    stale.failed_link_generation += 1;
    assert_eq!(route_owner.on_rerr(stale), Ok(false));
    stale = rerr;
    stale.destination_principal = [0xD4; 16];
    assert_eq!(route_owner.on_rerr(stale), Ok(false));
    assert_eq!(route_owner.on_rerr(rerr), Ok(true));
    assert_eq!(route_owner.on_rerr(rerr), Ok(false));
}

#[test]
fn static_fallback_is_not_owned_by_dynamic_generation_or_rerr() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let mut route_owner = owner(1, a);
    let static_route = StaticRoute {
        destination: c,
        next_hop: link(1, 9, b, 10),
        path_frame_mtu: 220,
    };
    route_owner
        .install_static(static_route)
        .expect("static route");
    let domain = RouteDomain {
        realm: RealmId::new(7).expect("realm"),
        origin: a,
        origin_session_generation: PeerSessionGeneration::new(1).expect("session"),
        destination: c,
    };
    assert_eq!(
        route_owner.resolve(
            domain,
            RouteUseFacts {
                now_us: 1,
                origin_session_generation: PeerSessionGeneration::new(1).expect("session"),
                destination_binding_generation: 1,
                link_generation: LinkInstanceGeneration::new(9).expect("link"),
            },
        ),
        Ok(ResolvedRoute::Static(static_route))
    );
    assert_eq!(
        route_owner.invalidate_link(1, LinkInstanceGeneration::new(9).expect("link")),
        0
    );
    assert_eq!(route_owner.counts().3, 1);
}

#[test]
fn expired_capacity_is_not_lazily_evicted_by_a_new_request() {
    let a = binding(1, 0xA1);
    let mut route_owner = owner(1, a);
    route_owner
        .ensure_discovery(
            NodeAddress::new(2, AddressWidth::A1).expect("address"),
            0,
            1,
            0,
            1,
        )
        .expect("first");
    route_owner
        .ensure_discovery(
            NodeAddress::new(3, AddressWidth::A1).expect("address"),
            0,
            1,
            0,
            1,
        )
        .expect("second");
    assert_eq!(
        route_owner.ensure_discovery(
            NodeAddress::new(4, AddressWidth::A1).expect("address"),
            0,
            1,
            0,
            2_000,
        ),
        Err(Error::NoSpace)
    );
}

#[test]
fn late_rrep_after_local_discovery_expiry_cannot_install_a_route() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let mut route_owner = owner(1, a);
    let start = route_owner
        .ensure_discovery(c.address, 0, 100, 0, 1)
        .expect("discovery");
    let reply = ucn_routing::RrepMessage {
        key: start.request.key,
        payload: ucn_routing::RrepPayload {
            destination_principal: c.principal,
            destination_binding_generation: c.generation.get(),
            hop_count: 1,
            accumulated_cost: 5,
            path_frame_mtu: 200,
            capability_bits: 0,
        },
    };

    let mut expired = false;
    for _ in 0..10 {
        expired |= route_owner.expire_one(1_001);
    }
    assert!(expired);
    assert_eq!(route_owner.counts(), (0, 0, 0, 0));

    assert_eq!(
        route_owner.on_rrep(reply, link(1, 1, b, 5), 1_002),
        Err(Error::NotFound)
    );
    assert_eq!(route_owner.counts(), (0, 0, 0, 0));
}

#[test]
fn same_key_retry_replays_only_the_exact_reverse_path_without_refreshing_lease() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let mut node_a = owner(1, a);
    let mut node_b = owner(2, b);
    let mut node_c = owner(3, c);
    let start = node_a
        .ensure_discovery(c.address, 0x0001, 100, 0, 10)
        .expect("discovery");
    let forwarded = match node_b
        .on_rreq(start.request, link(1, 1, a, 10), 20)
        .expect("first relay")
    {
        RequestAction::Forward(message) => message,
        other @ (RequestAction::LocalTarget | RequestAction::Duplicate) => {
            panic!("unexpected action: {other:?}")
        }
    };
    assert_eq!(
        node_c.on_rreq(forwarded, link(2, 1, b, 20), 30),
        Ok(RequestAction::LocalTarget)
    );
    let first_reply = node_c.make_rrep(forwarded, 0x0003, 230, 31).unwrap();
    let first_completion = match node_b.on_rrep(first_reply, link(2, 1, c, 20), 40).unwrap() {
        ReplyAction::Forward { completion, .. } => completion,
        other @ ReplyAction::ReachedOrigin(_) => panic!("unexpected action: {other:?}"),
    };
    node_b
        .complete_rrep_forward(first_completion, false)
        .unwrap();

    let retry = node_a.retry_discovery(start.handle, 110).unwrap();
    assert_eq!(retry, start.request);
    let retry_forwarded = match node_b
        .on_rreq(retry, link(1, 1, a, 10), 121)
        .expect("relay retry")
    {
        RequestAction::Forward(message) => message,
        other @ (RequestAction::LocalTarget | RequestAction::Duplicate) => {
            panic!("unexpected action: {other:?}")
        }
    };
    assert_eq!(retry_forwarded, forwarded);
    assert_eq!(
        node_c.on_rreq(retry_forwarded, link(2, 1, b, 20), 131),
        Ok(RequestAction::LocalTarget)
    );
    let retry_reply = node_c.make_rrep(retry_forwarded, 0x0003, 230, 132).unwrap();
    let (upstream_reply, completion) =
        match node_b.on_rrep(retry_reply, link(2, 1, c, 20), 140).unwrap() {
            ReplyAction::Forward {
                message,
                completion,
                ..
            } => (message, completion),
            other @ ReplyAction::ReachedOrigin(_) => panic!("unexpected action: {other:?}"),
        };
    node_b.complete_rrep_forward(completion, true).unwrap();
    assert!(matches!(
        node_a.on_rrep(upstream_reply, link(1, 1, b, 10), 150),
        Ok(ReplyAction::ReachedOrigin(_))
    ));

    let mut conflicting = retry;
    conflicting.payload.flags ^= 1;
    assert_eq!(
        node_b.on_rreq(conflicting, link(1, 1, a, 10), 221),
        Err(Error::Security)
    );
    for _ in 0..16 {
        let _ = node_b.expire_one(820);
    }
    assert_eq!(node_b.counts().1, 0);
}

#[test]
fn discovery_coalescing_requires_exact_requirements_and_invalid_input_burns_no_id() {
    let a = binding(1, 0xA1);
    let c = binding(3, 0xC3);
    let mut routes = owner(1, a);
    assert_eq!(
        routes.ensure_discovery(c.address, 1, 100, 0x80, 0),
        Err(Error::Argument)
    );
    let first = routes
        .ensure_discovery(c.address, 1, 100, 1, 1)
        .expect("valid discovery");
    assert_eq!(first.request.key.transaction_id.get(), 10);
    let before = routes.counts();
    assert_eq!(
        routes.ensure_discovery(c.address, 2, 100, 1, 2),
        Err(Error::State)
    );
    assert_eq!(
        routes.ensure_discovery(c.address, 1, 101, 1, 2),
        Err(Error::State)
    );
    assert_eq!(
        routes.ensure_discovery(c.address, 1, 100, 2, 2),
        Err(Error::State)
    );
    assert_eq!(routes.counts(), before);
    let same = routes
        .ensure_discovery(c.address, 1, 100, 1, 2)
        .expect("exact coalescing");
    assert!(!same.created);
    assert_eq!(same.request, first.request);
}

#[test]
fn rrep_must_satisfy_the_original_mtu_capability_and_hop_contract() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let mut routes = owner(1, a);
    let discovery = routes
        .ensure_discovery(c.address, 0x0003, 180, 0, 1)
        .expect("discovery");
    let good = RrepMessage {
        key: discovery.request.key,
        payload: RrepPayload {
            destination_principal: c.principal,
            destination_binding_generation: c.generation.get(),
            hop_count: 1,
            accumulated_cost: 10,
            path_frame_mtu: 190,
            capability_bits: 0x0003,
        },
    };
    let ingress = link(1, 1, b, 10);
    let before = routes.counts();

    let mut bad = good;
    bad.payload.path_frame_mtu = 179;
    assert_eq!(routes.on_rrep(bad, ingress, 2), Err(Error::Access));
    assert_eq!(routes.counts(), before);

    bad = good;
    bad.payload.capability_bits = 0x0001;
    assert_eq!(routes.on_rrep(bad, ingress, 2), Err(Error::Access));
    assert_eq!(routes.counts(), before);

    bad = good;
    bad.payload.hop_count = 8;
    assert_eq!(routes.on_rrep(bad, ingress, 2), Err(Error::Access));
    assert_eq!(routes.counts(), before);

    assert!(matches!(
        routes.on_rrep(good, ingress, 2),
        Ok(ReplyAction::ReachedOrigin(_))
    ));
}
