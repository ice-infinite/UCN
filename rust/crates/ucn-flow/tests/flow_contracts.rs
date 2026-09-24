//! Flow Codec、Candidate 不可变性和两阶段激活回归。

use ucn_capability::{
    CachedPeerCapability, CapabilityRecord, LinkCapability, MessageClass, PeerCapability,
    PeerCapabilityRef, capability_digest,
};
use ucn_flow::{
    ActivationAck, ActivationMessage, ActivationSubmit, FlowConfig, FlowCurrentFacts,
    FlowForwardRequest, FlowHopFacts, FlowOwner, FlowPhase, FlowPrefix, FlowRequirements,
    FlowTxRequest, LABEL_SETUP_BYTES, NanoFlowOwner, ProbeAck, RelayAbortAction, RelayCommitAction,
    RelayStageAction, TerminalOutcome, decode_flow_prefix, decode_label_setup, encode_flow_prefix,
    encode_label_setup,
};
use ucn_routing::{
    LinkRef, NanoRouteOwner, ReplyAction, RouteConfig, RouteUseFacts, RrepMessage, RrepPayload,
};
use ucn_security::{Binding, OriginSequenceOwner};
use ucn_types::{
    AddressWidth, BindingGeneration, ContextId, DeliveryGuarantee, Error, HeaderContract, HopLimit,
    HopProfile, InteractionRole, LinkInstanceGeneration, NodeAddress, OriginSecurity,
    OriginSequence, PayloadKind, PeerSessionGeneration, ProtocolOpcode, RealmId, ServiceId,
    TrafficClass,
};
use ucn_wire::CommonHeader;

fn binding(address: u32, principal: u8) -> Binding {
    Binding {
        address: NodeAddress::new(address, AddressWidth::A1).expect("address"),
        generation: BindingGeneration::active(1).expect("binding"),
        principal: [principal; 16],
    }
}

fn link(id: u16, peer: Binding, cost: u32) -> LinkRef {
    LinkRef::new(
        id,
        LinkInstanceGeneration::new(9).expect("link generation"),
        peer,
        200,
        cost,
        0x010B,
    )
    .expect("link")
}

fn route_owner(local: Binding) -> NanoRouteOwner {
    NanoRouteOwner::new(
        RouteConfig {
            owner_instance: 11,
            realm: RealmId::new(7).expect("realm"),
            address_width: AddressWidth::A1,
            local,
            local_session_generation: PeerSessionGeneration::new(3).expect("session"),
            discovery_lifetime_us: 10_000,
            discovery_retry_us: 100,
            discovery_max_attempts: 3,
            reverse_lifetime_us: 5_000,
            route_lifetime_us: 8_000,
            maximum_hops: HopLimit::new(8).expect("hop"),
        },
        100,
    )
    .expect("route owner")
}

fn soft_route(
    owner: &mut NanoRouteOwner,
    next_hop: Binding,
    destination: Binding,
    downstream_hops: u8,
    cost: u32,
    now_us: u64,
) -> ucn_routing::SoftRouteView {
    let discovery = owner
        .ensure_discovery(destination.address, 0x010B, 180, 0, now_us)
        .expect("discovery");
    let reply = RrepMessage {
        key: discovery.request.key,
        payload: RrepPayload {
            destination_principal: destination.principal,
            destination_binding_generation: destination.generation.get(),
            hop_count: downstream_hops,
            accumulated_cost: cost,
            path_frame_mtu: 190,
            capability_bits: 0x010B,
        },
    };
    match owner
        .on_rrep(reply, link(1, next_hop, cost), now_us + 1)
        .expect("reply")
    {
        ReplyAction::ReachedOrigin(route) => route,
        ReplyAction::Forward { .. } => panic!("origin route expected"),
    }
}

fn capability(peer: Binding, deadline_us: u64) -> CachedPeerCapability {
    let record = CapabilityRecord {
        capability_generation: 5,
        link: LinkCapability {
            link_instance_generation: 9,
            carrier_mtu: 240,
            link_frame_mtu: 200,
            processing_frame_mtu: 200,
            carrier_header_bytes: 4,
            carrier_padding_bytes: 0,
            carrier_crc_bytes: 4,
            carrier_tag_bytes: 0,
            carrier_max_fragments: 1,
            link_flags: 0x000C,
            nominal_rate_bps: 1_000_000,
            hardware_priority_count: 4,
            timestamp_capability_bits: 0,
            timestamp_uncertainty_us: 0,
        },
        peer: PeerCapability {
            feature_bits: 0x0000_010B,
            hop_suite_bits: 0x0000_0002,
            e2e_suite_bits: 0x0000_0006,
            max_message_class: MessageClass::T128,
            max_rx_window: 8,
            max_concurrent_transfers: 2,
            realtime_mode_bits: 0,
            clock_domain_id: 0,
            clock_domain_generation: 0,
        },
    };
    CachedPeerCapability {
        peer_ref: PeerCapabilityRef {
            runtime_instance: 1,
            security_owner_instance: 2,
            realm: RealmId::new(7).expect("realm"),
            principal: peer.principal,
            binding: peer,
            session_generation: 4,
            ingress_link_id: 1,
            ingress_link_generation: 9,
        },
        record,
        digest: capability_digest(record).expect("digest"),
        discovery_deadline_us: deadline_us,
        capability_deadline_us: deadline_us,
    }
}

fn requirements(contract: HeaderContract, expires_at_us: u64) -> FlowRequirements {
    FlowRequirements {
        contract,
        service: ServiceId::new(0x1101).expect("service"),
        traffic_ceiling: TrafficClass::Q1,
        delivery: if contract == HeaderContract::C3 {
            DeliveryGuarantee::BestEffort
        } else {
            DeliveryGuarantee::Reliable
        },
        interaction: if contract == HeaderContract::C3 {
            InteractionRole::OneWay
        } else {
            InteractionRole::Request
        },
        payload_kind: if contract == HeaderContract::C3 {
            PayloadKind::Data
        } else {
            PayloadKind::Control
        },
        protocol_opcode: if contract == HeaderContract::C3 {
            0
        } else {
            u16::from(ProtocolOpcode::OperationQuery)
        },
        origin_security: if contract == HeaderContract::C3 {
            OriginSecurity::O0
        } else {
            OriginSecurity::O1
        },
        hop_profile: if contract == HeaderContract::C2 {
            HopProfile::H2
        } else {
            HopProfile::H1
        },
        required_feature_bits: 0x0000_010B,
        minimum_payload_bytes: 32,
        policy_generation: 7,
        path_profile_id: 3,
        expires_at_us,
    }
}

fn flow_owner(instance: u32, local: Binding) -> NanoFlowOwner {
    NanoFlowOwner::new(
        FlowConfig {
            owner_instance: instance,
            realm: RealmId::new(7).expect("realm"),
            local,
            policy_generation: 7,
            probe_lifetime_us: 500,
            stage_lifetime_us: 500,
            commit_lifetime_us: 500,
            flow_lifetime_us: 5_000,
            receipt_lifetime_us: 1_000,
        },
        1,
        10,
        1,
        1,
        1,
        1,
    )
    .expect("flow owner")
}

fn facts(
    local: Binding,
    destination: Binding,
    next_hop: Binding,
    cached: CachedPeerCapability,
    now_us: u64,
) -> FlowCurrentFacts {
    FlowCurrentFacts {
        now_us,
        local,
        destination,
        origin_session_generation: 3,
        next_hop: link(1, next_hop, 10),
        capability_ref: cached.peer_ref,
        capability_generation: cached.record.capability_generation,
        capability_digest: cached.digest,
        capability_deadline_us: cached.capability_deadline_us,
        policy_generation: 7,
    }
}

fn hop_facts(
    local: Binding,
    upstream: LinkRef,
    downstream: Option<LinkRef>,
    now_us: u64,
) -> FlowHopFacts {
    FlowHopFacts {
        now_us,
        local,
        upstream,
        downstream,
        policy_generation: 7,
        path_frame_mtu: 200,
        capability_bits: 0x010B,
        hop_profile: HopProfile::H1,
        capability_deadline_us: 9_000,
        security_owner_instance: 2,
        peer_session_generation: 4,
        origin_key_generation: 1,
    }
}

fn header(contract: HeaderContract) -> CommonHeader {
    CommonHeader {
        contract,
        traffic_class: TrafficClass::Q1,
        delivery: if contract == HeaderContract::C3 {
            DeliveryGuarantee::BestEffort
        } else {
            DeliveryGuarantee::Reliable
        },
        interaction: if contract == HeaderContract::C3 {
            InteractionRole::OneWay
        } else {
            InteractionRole::Request
        },
        payload_kind: if contract == HeaderContract::C3 {
            PayloadKind::Data
        } else {
            PayloadKind::Control
        },
        origin_security: if contract == HeaderContract::C3 {
            OriginSecurity::O0
        } else {
            OriginSecurity::O1
        },
        hop_limit: HopLimit::new(7).expect("hop"),
    }
}

#[test]
fn label_setup_and_flow_prefixes_are_exact_big_endian() {
    let setup = ucn_flow::LabelSetup {
        candidate_id: ucn_types::CandidateId::new(0x1122).expect("candidate"),
        route_generation: ucn_types::RouteGeneration::new(0x3344_5566).expect("route"),
        reverse_label: ucn_types::ForwardingLabel::new(0x7788).expect("label"),
        forward_label: ucn_types::ForwardingLabel::new(0x99AA).expect("label"),
        path_profile_id: 0xBBCC,
        context_digest: 0xDDEE_F001,
    };
    let mut bytes = [0_u8; LABEL_SETUP_BYTES];
    encode_label_setup(setup, &mut bytes).expect("encode setup");
    assert_eq!(
        bytes,
        [
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE,
            0xF0, 0x01,
        ]
    );
    assert_eq!(decode_label_setup(&bytes), Ok(setup));

    let prefixes = [
        FlowPrefix::C2 {
            header: header(HeaderContract::C2),
            context_id: ContextId::new(0x1122).expect("context"),
            sequence: OriginSequence::new(0x3344_5566).expect("sequence"),
        },
        FlowPrefix::C3 {
            header: header(HeaderContract::C3),
            label: ucn_types::ForwardingLabel::new(0x1122).expect("label"),
            context_id: ContextId::new(0x3344).expect("context"),
        },
        FlowPrefix::C4 {
            header: header(HeaderContract::C4),
            label: ucn_types::ForwardingLabel::new(0x1122).expect("label"),
            context_id: ContextId::new(0x3344).expect("context"),
            sequence: OriginSequence::new(0x5566_7788).expect("sequence"),
        },
    ];
    for value in prefixes {
        let mut output = [0xCC_u8; 11];
        let size = value.encoded_size();
        encode_flow_prefix(value, &mut output[..size]).expect("prefix encode");
        assert_eq!(decode_flow_prefix(&output[..size]), Ok(value));
    }
    let mut output = [0xCC_u8; 10];
    let before = output;
    assert_eq!(
        encode_flow_prefix(prefixes[2], &mut output),
        Err(Error::NoSpace)
    );
    assert_eq!(output, before);
}

#[test]
fn frozen_candidate_is_not_mutated_by_a_better_rrep() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let mut routes = route_owner(a);
    let first = soft_route(&mut routes, b, c, 1, 20, 1);
    let cached = capability(b, 10_000);
    let mut flows = flow_owner(20, a);
    let first_handle = flows
        .import_candidate(first, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("first candidate");
    let frozen = flows.begin_probe(first_handle, 4).expect("probe").proposal;

    let improved = soft_route(&mut routes, b, c, 1, 5, 10);
    let second_handle = flows
        .import_candidate(
            improved,
            cached,
            requirements(HeaderContract::C4, 9_000),
            12,
        )
        .expect("second candidate");
    assert_ne!(first_handle, second_handle);
    assert_eq!(
        flows.candidate_view(first_handle),
        Ok((FlowPhase::Probing, Some(frozen)))
    );
    assert_eq!(
        flows.candidate_view(second_handle),
        Ok((FlowPhase::Candidate, None))
    );
}

#[test]
#[allow(clippy::too_many_lines)]
fn three_node_stage_commit_publishes_target_then_relay_then_origin() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);
    let mut origin = flow_owner(20, a);
    let mut relay = flow_owner(21, b);
    let mut target = flow_owner(22, c);

    let candidate = origin
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("candidate");
    let probe = origin.begin_probe(candidate, 4).expect("probe");
    origin
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 20,
            },
            10,
        )
        .expect("probe ack");
    let ready_counts = origin.counts();
    let mut revoked = facts(a, c, b, cached, 11);
    revoked.capability_generation += 1;
    assert_eq!(origin.begin_stage(candidate, revoked), Err(Error::State));
    revoked = facts(a, c, b, cached, 11);
    revoked.next_hop = link(9, b, 10);
    assert_eq!(origin.begin_stage(candidate, revoked), Err(Error::State));
    revoked = facts(a, c, b, cached, 11);
    revoked.policy_generation += 1;
    assert_eq!(origin.begin_stage(candidate, revoked), Err(Error::State));
    assert_eq!(origin.counts(), ready_counts);
    assert_eq!(
        origin.candidate_view(candidate),
        Ok((FlowPhase::ReadyToStage, Some(probe.proposal)))
    );
    let (origin_activation, stage) = origin
        .begin_stage(candidate, facts(a, c, b, cached, 11))
        .expect("stage");
    origin
        .complete_stage_submit(origin_activation, ActivationSubmit::Submitted)
        .expect("origin stage submit");
    assert_eq!(origin.stage_message(origin_activation), Ok(stage));
    origin
        .complete_stage_submit(origin_activation, ActivationSubmit::Submitted)
        .expect("idempotent stage submit report");

    let relay_before = relay.counts();
    let mut stale_hop = hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 12);
    stale_hop.path_frame_mtu = 189;
    assert_eq!(relay.accept_stage(&stage, stale_hop), Err(Error::State));
    stale_hop = hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 12);
    stale_hop.capability_bits = 0x000B;
    assert_eq!(relay.accept_stage(&stage, stale_hop), Err(Error::State));
    stale_hop = hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 12);
    stale_hop.hop_profile = HopProfile::H0;
    assert_eq!(relay.accept_stage(&stage, stale_hop), Err(Error::State));
    assert_eq!(relay.counts(), relay_before);

    let relay_activation = match relay
        .accept_stage(
            &stage,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 12),
        )
        .expect("relay stage")
    {
        RelayStageAction::Forward { activation, .. } => activation,
        RelayStageAction::Ack { .. } => panic!("relay must wait"),
    };
    relay
        .relay_complete_stage_submit(relay_activation, ActivationSubmit::Submitted)
        .expect("relay stage submit");
    let target_stage_ack = match target
        .accept_stage(&stage, hop_facts(c, link(8, b, 10), None, 13))
        .expect("target stage")
    {
        RelayStageAction::Ack { ack, upstream } => {
            assert_eq!(upstream.peer(), b);
            ack
        }
        RelayStageAction::Forward { .. } => panic!("target must ack"),
    };
    let relay_stage_ack = relay
        .relay_on_stage_ack(
            relay_activation,
            target_stage_ack,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 14),
        )
        .expect("relay stage ack");
    let mut stale_ack_facts = facts(a, c, b, cached, 15);
    stale_ack_facts.capability_digest[0] ^= 0x80;
    assert_eq!(
        origin.on_stage_ack(origin_activation, relay_stage_ack, stale_ack_facts),
        Err(Error::State)
    );
    assert_eq!(
        origin.activation_phase(origin_activation),
        Ok(FlowPhase::StagePending)
    );
    let commit = origin
        .on_stage_ack(
            origin_activation,
            relay_stage_ack,
            facts(a, c, b, cached, 15),
        )
        .expect("origin commit");
    origin
        .complete_commit_submit(origin_activation, ActivationSubmit::Submitted)
        .expect("origin commit submit");
    assert_eq!(origin.commit_message(origin_activation), Ok(commit));
    origin
        .complete_commit_submit(origin_activation, ActivationSubmit::Submitted)
        .expect("idempotent commit submit report");

    let relay_commit_activation = match relay
        .accept_commit(
            &commit,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 16),
        )
        .expect("relay commit")
    {
        RelayCommitAction::Forward { activation, .. } => activation,
        RelayCommitAction::Ack { .. } => panic!("relay must wait"),
    };
    assert_eq!(relay_commit_activation, relay_activation);
    relay
        .complete_commit_submit(relay_activation, ActivationSubmit::Submitted)
        .expect("relay commit submit");
    let target_commit_ack = match target
        .accept_commit(&commit, hop_facts(c, link(8, b, 10), None, 17))
        .expect("target commit")
    {
        RelayCommitAction::Ack {
            ack,
            upstream,
            flow,
        } => {
            assert_eq!(upstream.peer(), b);
            assert_eq!(target.flow_phase(flow), Ok(FlowPhase::Active));
            ack
        }
        RelayCommitAction::Forward { .. } => panic!("target must ack"),
    };
    assert!(matches!(
        target
            .accept_stage(&stage, hop_facts(c, link(8, b, 10), None, 18))
            .expect("duplicate stage after active"),
        RelayStageAction::Ack { .. }
    ));
    assert!(matches!(
        target
            .accept_commit(&commit, hop_facts(c, link(8, b, 10), None, 18))
            .expect("duplicate commit after active"),
        RelayCommitAction::Ack { .. }
    ));
    let (relay_flow, relay_commit_ack) = relay
        .relay_on_commit_ack(
            relay_activation,
            target_commit_ack,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 18),
        )
        .expect("relay commit ack");
    assert_eq!(relay.flow_phase(relay_flow), Ok(FlowPhase::Active));
    let (duplicate_relay_flow, _) = relay
        .relay_on_commit_ack(
            relay_activation,
            target_commit_ack,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 19),
        )
        .expect("duplicate relay commit ack");
    assert_eq!(duplicate_relay_flow, relay_flow);
    let origin_flow = origin
        .on_commit_ack(
            origin_activation,
            relay_commit_ack,
            facts(a, c, b, cached, 19),
        )
        .expect("origin commit ack");
    assert_eq!(origin.flow_phase(origin_flow), Ok(FlowPhase::Active));
    let duplicate_origin_flow = origin
        .on_commit_ack(
            origin_activation,
            relay_commit_ack,
            facts(a, c, b, cached, 20),
        )
        .expect("duplicate origin commit ack");
    assert_eq!(duplicate_origin_flow, origin_flow);
    let tx = origin
        .flow_use_preflight(
            origin_flow,
            facts(a, c, b, cached, 20),
            FlowTxRequest {
                contract: HeaderContract::C4,
                traffic_class: TrafficClass::Q1,
                delivery: DeliveryGuarantee::Reliable,
                interaction: InteractionRole::Request,
                payload_kind: PayloadKind::Control,
                protocol_opcode: u16::from(ProtocolOpcode::OperationQuery),
                origin_security: OriginSecurity::O1,
                payload_bytes: 32,
            },
        )
        .expect("tx preflight");
    assert_eq!(tx.contract, HeaderContract::C4);
    assert_eq!(tx.next_hop.peer(), b);
    assert_eq!(tx.forward_label, stage.setup.forward_label);
    assert_eq!(tx.origin_sequence.expect("O1 sequence").get(), 1);
    let relay_plan = relay
        .forward_preflight(
            relay_flow,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 20),
            FlowForwardRequest {
                ingress: link(7, a, 10),
                label: tx.forward_label,
                context_id: tx.context_id,
                route_generation: tx.route_generation,
                traffic_class: TrafficClass::Q1,
                packet_bytes: 100,
            },
        )
        .expect("relay fast path accepts the origin plan");
    assert_eq!(relay_plan.egress.peer(), c);
    assert_eq!(relay_plan.egress_label, tx.forward_label);
    let repeated = origin
        .flow_use_preflight(
            origin_flow,
            facts(a, c, b, cached, 21),
            FlowTxRequest {
                contract: HeaderContract::C4,
                traffic_class: TrafficClass::Q1,
                delivery: DeliveryGuarantee::Reliable,
                interaction: InteractionRole::Request,
                payload_kind: PayloadKind::Control,
                protocol_opcode: u16::from(ProtocolOpcode::OperationQuery),
                origin_security: OriginSecurity::O1,
                payload_bytes: 32,
            },
        )
        .expect("repeated preflight");
    assert_eq!(repeated.origin_sequence, tx.origin_sequence);
    OriginSequenceOwner::burn(&mut origin, tx.origin_sequence.expect("sequence")).expect("burn");
    let next = origin
        .flow_use_preflight(
            origin_flow,
            facts(a, c, b, cached, 22),
            FlowTxRequest {
                contract: HeaderContract::C4,
                traffic_class: TrafficClass::Q1,
                delivery: DeliveryGuarantee::Reliable,
                interaction: InteractionRole::Request,
                payload_kind: PayloadKind::Control,
                protocol_opcode: u16::from(ProtocolOpcode::OperationQuery),
                origin_security: OriginSecurity::O1,
                payload_bytes: 32,
            },
        )
        .expect("next preflight");
    assert_eq!(next.origin_sequence.expect("next sequence").get(), 2);
}

#[test]
fn terminal_counter_values_are_allocated_once_then_exhausted() {
    type TerminalOwner = FlowOwner<2, 1, 1, 1>;
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);
    let mut owner = TerminalOwner::new(
        FlowConfig {
            owner_instance: 31,
            realm: RealmId::new(7).expect("realm"),
            local: a,
            policy_generation: 7,
            probe_lifetime_us: 500,
            stage_lifetime_us: 500,
            commit_lifetime_us: 500,
            flow_lifetime_us: 5_000,
            receipt_lifetime_us: 1_000,
        },
        u32::from(u16::MAX - 1),
        u64::MAX,
        u32::MAX,
        u16::MAX - 1,
        u16::MAX - 2,
        u32::MAX,
    )
    .expect("terminal owner");
    let candidate = owner
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("terminal candidate");
    let probe = owner.begin_probe(candidate, 4).expect("terminal probe");
    assert_eq!(probe.proposal.key().candidate_id().get(), 65_534);
    assert_eq!(probe.proposal.key().transaction_id().get(), u64::MAX);
    assert_eq!(probe.proposal.key().route_generation().get(), u32::MAX);
    assert_eq!(probe.proposal.context_id().get(), 65_534);
    assert_eq!(
        probe.proposal.key().label_setup().forward_label.get(),
        65_534
    );
    assert_eq!(
        owner.import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 5),
        Err(Error::Exhausted)
    );
    owner
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 1,
            },
            5,
        )
        .expect("probe ack");
    let (activation, stage) = owner
        .begin_stage(candidate, facts(a, c, b, cached, 6))
        .expect("stage");
    owner
        .complete_stage_submit(activation, ActivationSubmit::Submitted)
        .expect("stage submit");
    owner
        .on_stage_ack(
            activation,
            ActivationAck {
                opcode: ProtocolOpcode::PathStageAck,
                key: stage.key,
            },
            facts(a, c, b, cached, 7),
        )
        .expect("commit");
    owner
        .complete_commit_submit(activation, ActivationSubmit::Submitted)
        .expect("commit submit");
    let flow = owner
        .on_commit_ack(
            activation,
            ActivationAck {
                opcode: ProtocolOpcode::PathCommitAck,
                key: stage.key,
            },
            facts(a, c, b, cached, 8),
        )
        .expect("terminal flow generation");
    assert_eq!(owner.flow_phase(flow), Ok(FlowPhase::Active));
}

#[test]
fn capability_owner_identity_is_part_of_the_proposal_digest() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);
    let first_capability = capability(b, 10_000);
    let mut second_capability = first_capability;
    second_capability.peer_ref.runtime_instance += 1;
    second_capability.peer_ref.security_owner_instance += 1;
    second_capability.peer_ref.session_generation += 1;
    let mut owner = flow_owner(32, a);
    let first = owner
        .import_candidate(
            route,
            first_capability,
            requirements(HeaderContract::C4, 9_000),
            3,
        )
        .expect("first");
    let second = owner
        .import_candidate(
            route,
            second_capability,
            requirements(HeaderContract::C4, 9_000),
            3,
        )
        .expect("second");
    let first_fingerprint = owner
        .begin_probe(first, 4)
        .expect("first probe")
        .proposal
        .fingerprint()
        .expect("fingerprint");
    let second_fingerprint = owner
        .begin_probe(second, 4)
        .expect("second probe")
        .proposal
        .fingerprint()
        .expect("fingerprint");
    assert_ne!(first_fingerprint, second_fingerprint);
}

#[test]
fn wrong_ack_is_zero_write_and_commit_uncertainty_is_not_rolled_back() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);
    let mut owner = flow_owner(20, a);
    let candidate = owner
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("candidate");
    let probe = owner.begin_probe(candidate, 4).expect("probe");
    let mut other_routes = route_owner(a);
    let other_route = soft_route(&mut other_routes, b, c, 1, 11, 1);
    let other_candidate = owner
        .import_candidate(
            other_route,
            cached,
            requirements(HeaderContract::C4, 9_000),
            5,
        )
        .expect("other candidate");
    let other_probe = owner.begin_probe(other_candidate, 6).expect("other probe");
    assert_eq!(
        owner.on_probe_ack(
            candidate,
            ProbeAck {
                key: other_probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 1,
            },
            7,
        ),
        Err(Error::State)
    );
    assert_eq!(
        owner.candidate_view(candidate),
        Ok((FlowPhase::Probing, Some(probe.proposal)))
    );
    owner
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 1,
            },
            8,
        )
        .expect("probe ack");
    let (activation, stage) = owner
        .begin_stage(candidate, facts(a, c, b, cached, 9))
        .expect("stage");
    owner
        .complete_stage_submit(activation, ActivationSubmit::Submitted)
        .expect("stage submit");
    let commit = owner
        .on_stage_ack(
            activation,
            ActivationAck {
                opcode: ProtocolOpcode::PathStageAck,
                key: stage.key,
            },
            facts(a, c, b, cached, 10),
        )
        .expect("commit");
    assert_eq!(commit.key, stage.key);
    owner
        .complete_commit_submit(activation, ActivationSubmit::InDoubt)
        .expect("unknown submit");
    assert_eq!(owner.counts(), (2, 1, 0, 0));
    let receipt = ActivationAck {
        opcode: ProtocolOpcode::PathTerminalReceipt,
        key: stage.key,
    };
    let flow = owner
        .reconcile_terminal(
            activation,
            receipt,
            TerminalOutcome::Committed,
            facts(a, c, b, cached, 11),
        )
        .expect("reconcile")
        .expect("committed flow");
    assert_eq!(owner.flow_phase(flow), Ok(FlowPhase::Active));
}

#[test]
fn active_flow_fences_on_exact_parent_change() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);
    assert_eq!(
        routes.resolve(
            route.domain(),
            RouteUseFacts {
                now_us: 2,
                origin_session_generation: PeerSessionGeneration::new(3).expect("session"),
                destination_binding_generation: 1,
                link_generation: LinkInstanceGeneration::new(9).expect("link"),
            }
        ),
        Ok(ucn_routing::ResolvedRoute::Dynamic(route))
    );
    let mut owner = flow_owner(20, a);
    let candidate = owner
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("candidate");
    let probe = owner.begin_probe(candidate, 4).expect("probe");
    owner
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 1,
            },
            5,
        )
        .expect("probe ack");
    let (activation, stage) = owner
        .begin_stage(candidate, facts(a, c, b, cached, 6))
        .expect("stage");
    owner
        .complete_stage_submit(activation, ActivationSubmit::Submitted)
        .expect("stage submit");
    owner
        .on_stage_ack(
            activation,
            ActivationAck {
                opcode: ProtocolOpcode::PathStageAck,
                key: stage.key,
            },
            facts(a, c, b, cached, 7),
        )
        .expect("commit");
    owner
        .complete_commit_submit(activation, ActivationSubmit::Submitted)
        .expect("commit submit");
    let flow = owner
        .on_commit_ack(
            activation,
            ActivationAck {
                opcode: ProtocolOpcode::PathCommitAck,
                key: stage.key,
            },
            facts(a, c, b, cached, 8),
        )
        .expect("active");
    let mut changed = facts(a, c, b, cached, 9);
    changed.policy_generation = 8;
    assert_eq!(
        owner.flow_use_preflight(
            flow,
            changed,
            FlowTxRequest {
                contract: HeaderContract::C4,
                traffic_class: TrafficClass::Q1,
                delivery: DeliveryGuarantee::Reliable,
                interaction: InteractionRole::Request,
                payload_kind: PayloadKind::Control,
                protocol_opcode: u16::from(ProtocolOpcode::OperationQuery),
                origin_security: OriginSecurity::O1,
                payload_bytes: 1,
            }
        ),
        Err(Error::State)
    );
    assert_eq!(owner.flow_phase(flow), Ok(FlowPhase::Fenced));

    assert_eq!(owner.retire_activation(activation), Err(Error::State));
    owner.retire_flow(flow).expect("retire fenced flow");
    for _ in 0..8 {
        let _ = owner.maintain_one(2_000);
    }
    owner
        .retire_activation(activation)
        .expect("retire terminal activation after receipt");
    owner
        .retire_candidate(candidate)
        .expect("retire source candidate");
    assert_eq!(owner.flow_phase(flow), Err(Error::NotFound));
    assert_eq!(owner.candidate_view(candidate), Err(Error::NotFound));
    assert_eq!(owner.counts(), (0, 0, 0, 0));
}

#[test]
fn bounded_maintenance_fences_expired_flow_but_never_drops_indoubt() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);
    let mut owner = flow_owner(40, a);
    let candidate = owner
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("candidate");
    let probe = owner.begin_probe(candidate, 4).expect("probe");
    owner
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 1,
            },
            5,
        )
        .expect("probe ack");
    let (activation, stage) = owner
        .begin_stage(candidate, facts(a, c, b, cached, 6))
        .expect("stage");
    owner
        .complete_stage_submit(activation, ActivationSubmit::Submitted)
        .expect("stage submitted");
    owner
        .on_stage_ack(
            activation,
            ActivationAck {
                opcode: ProtocolOpcode::PathStageAck,
                key: stage.key,
            },
            facts(a, c, b, cached, 7),
        )
        .expect("commit");
    owner
        .complete_commit_submit(activation, ActivationSubmit::InDoubt)
        .expect("commit uncertain");

    for _ in 0..32 {
        let _ = owner.maintain_one(100_000);
    }
    assert_eq!(owner.retire_activation(activation), Err(Error::State));
    assert_eq!(owner.counts(), (1, 1, 0, 0));
}

#[test]
fn c2_and_c3_contract_shape_is_fail_closed() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);

    let mut direct_routes = route_owner(a);
    let direct = soft_route(&mut direct_routes, b, b, 0, 5, 1);
    let mut direct_owner = flow_owner(20, a);
    assert!(
        direct_owner
            .import_candidate(direct, cached, requirements(HeaderContract::C2, 9_000), 3)
            .is_ok()
    );

    let mut routed_routes = route_owner(a);
    let routed = soft_route(&mut routed_routes, b, c, 1, 5, 1);
    let mut routed_owner = flow_owner(21, a);
    assert!(
        routed_owner
            .import_candidate(routed, cached, requirements(HeaderContract::C3, 9_000), 3)
            .is_ok()
    );
    assert_eq!(
        routed_owner.import_candidate(routed, cached, requirements(HeaderContract::C2, 9_000), 3),
        Err(Error::Policy)
    );
}

#[test]
fn stage_capacity_failure_does_not_consume_ready_candidate() {
    type Tiny = FlowOwner<2, 1, 1, 1>;
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let first_route = soft_route(&mut routes, b, c, 1, 10, 1);
    let second_route = soft_route(&mut routes, b, c, 1, 11, 10);
    let mut owner = Tiny::new(
        FlowConfig {
            owner_instance: 30,
            realm: RealmId::new(7).expect("realm"),
            local: a,
            policy_generation: 7,
            probe_lifetime_us: 500,
            stage_lifetime_us: 500,
            commit_lifetime_us: 500,
            flow_lifetime_us: 5_000,
            receipt_lifetime_us: 1_000,
        },
        1,
        1,
        1,
        1,
        1,
        1,
    )
    .expect("tiny owner");
    let first = owner
        .import_candidate(
            first_route,
            cached,
            requirements(HeaderContract::C4, 9_000),
            20,
        )
        .expect("first");
    let second = owner
        .import_candidate(
            second_route,
            cached,
            requirements(HeaderContract::C4, 9_000),
            20,
        )
        .expect("second");
    for handle in [first, second] {
        let probe = owner.begin_probe(handle, 21).expect("probe");
        owner
            .on_probe_ack(
                handle,
                ProbeAck {
                    key: probe.proposal.key(),
                    path_frame_mtu: 190,
                    capability_bits: 0x010B,
                    measured_rtt_us: 1,
                },
                22,
            )
            .expect("probe ack");
    }
    owner
        .begin_stage(first, facts(a, c, b, cached, 23))
        .expect("first stage");
    let before = owner.counts();
    let mut second_facts = facts(a, c, b, cached, 24);
    second_facts.next_hop = link(1, b, 11);
    assert!(matches!(
        owner.begin_stage(second, second_facts),
        Err(Error::NoSpace)
    ));
    assert_eq!(owner.counts(), before);
    assert_eq!(
        owner.candidate_view(second).map(|value| value.0),
        Ok(FlowPhase::ReadyToStage)
    );
}

#[test]
#[allow(clippy::too_many_lines)]
fn exact_abort_converges_target_then_relay_then_origin() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);
    let mut origin = flow_owner(60, a);
    let mut relay = flow_owner(61, b);
    let mut target = flow_owner(62, c);

    let candidate = origin
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("candidate");
    let probe = origin.begin_probe(candidate, 4).expect("probe");
    origin
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 20,
            },
            10,
        )
        .expect("probe ack");
    let (origin_activation, stage) = origin
        .begin_stage(candidate, facts(a, c, b, cached, 11))
        .expect("stage");
    origin
        .complete_stage_submit(origin_activation, ActivationSubmit::Submitted)
        .expect("origin stage submitted");
    let relay_activation = match relay
        .accept_stage(
            &stage,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 12),
        )
        .expect("relay stage")
    {
        RelayStageAction::Forward { activation, .. } => activation,
        RelayStageAction::Ack { .. } => panic!("relay must forward"),
    };
    relay
        .relay_complete_stage_submit(relay_activation, ActivationSubmit::Submitted)
        .expect("relay stage submitted");
    let target_receipt = match target
        .accept_stage(&stage, hop_facts(c, link(8, b, 10), None, 13))
        .expect("target stage")
    {
        RelayStageAction::Ack { ack, .. } => ack,
        RelayStageAction::Forward { .. } => panic!("target must ack"),
    };
    let relay_stage_ack = relay
        .relay_on_stage_ack(
            relay_activation,
            target_receipt,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 14),
        )
        .expect("relay stage ack");
    let commit = origin
        .on_stage_ack(
            origin_activation,
            relay_stage_ack,
            facts(a, c, b, cached, 15),
        )
        .expect("commit prepared but not submitted");

    let abort = origin
        .begin_abort(origin_activation, 16)
        .expect("origin abort")
        .expect("remote stage requires abort message");
    assert_eq!(abort.opcode, ProtocolOpcode::PathActivateAbort);
    assert_eq!(abort.key, commit.key);
    assert_eq!(origin.abort_message(origin_activation), Ok(abort));
    origin
        .complete_abort_submit(origin_activation, ActivationSubmit::Submitted)
        .expect("origin abort submit");

    let relay_abort = match relay
        .accept_abort(
            &abort,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 17),
        )
        .expect("relay abort")
    {
        RelayAbortAction::Forward { activation, .. } => activation,
        RelayAbortAction::Ack { .. } => panic!("relay must wait for downstream"),
    };
    assert_eq!(relay_abort, relay_activation);
    relay
        .complete_abort_submit(relay_abort, ActivationSubmit::Submitted)
        .expect("relay abort submit");
    let target_abort_receipt = match target
        .accept_abort(&abort, hop_facts(c, link(8, b, 10), None, 18))
        .expect("target abort")
    {
        RelayAbortAction::Ack { ack, .. } => ack,
        RelayAbortAction::Forward { .. } => panic!("target has no downstream"),
    };
    assert_eq!(
        target.accept_commit(&commit, hop_facts(c, link(8, b, 10), None, 19)),
        Err(Error::State)
    );
    assert!(matches!(
        target
            .accept_abort(&abort, hop_facts(c, link(8, b, 10), None, 19))
            .expect("duplicate target abort"),
        RelayAbortAction::Ack { .. }
    ));
    let relay_abort_receipt = relay
        .relay_on_abort_receipt(
            relay_abort,
            target_abort_receipt,
            hop_facts(b, link(7, a, 10), Some(link(8, c, 10)), 20),
        )
        .expect("relay abort receipt");
    origin
        .on_abort_receipt(origin_activation, relay_abort_receipt, 21)
        .expect("origin abort receipt");
    assert_eq!(
        origin.activation_phase(origin_activation),
        Ok(FlowPhase::Fenced)
    );
    assert_eq!(
        relay.activation_phase(relay_activation),
        Ok(FlowPhase::Fenced)
    );
}

#[test]
fn local_abort_is_zero_wire_and_commit_submission_is_irreversible() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);
    let mut owner = flow_owner(70, a);
    let candidate = owner
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("candidate");
    let probe = owner.begin_probe(candidate, 4).expect("probe");
    owner
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 20,
            },
            5,
        )
        .expect("probe ack");
    let (activation, _) = owner
        .begin_stage(candidate, facts(a, c, b, cached, 6))
        .expect("stage");
    assert_eq!(owner.begin_abort(activation, 7), Ok(None));
    assert_eq!(owner.activation_phase(activation), Ok(FlowPhase::Fenced));
    assert_eq!(owner.abort_message(activation), Err(Error::State));

    let mut owner = flow_owner(71, a);
    let candidate = owner
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 10)
        .expect("candidate");
    let probe = owner.begin_probe(candidate, 11).expect("probe");
    owner
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 20,
            },
            12,
        )
        .expect("probe ack");
    let (activation, _) = owner
        .begin_stage(candidate, facts(a, c, b, cached, 13))
        .expect("stage");
    owner
        .complete_stage_submit(activation, ActivationSubmit::Submitted)
        .expect("stage submitted");
    let ack = ActivationAck {
        opcode: ProtocolOpcode::PathStageAck,
        key: probe.proposal.key(),
    };
    owner
        .on_stage_ack(activation, ack, facts(a, c, b, cached, 14))
        .expect("commit");
    owner
        .complete_commit_submit(activation, ActivationSubmit::Submitted)
        .expect("commit submitted");
    let before = owner.activation_phase(activation);
    assert_eq!(owner.begin_abort(activation, 15), Err(Error::State));
    assert_eq!(owner.activation_phase(activation), before);
}

#[test]
fn stage_expiry_only_enters_abort_when_a_remote_stage_may_exist() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);

    let mut local_origin = flow_owner(74, a);
    let candidate = local_origin
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("local candidate");
    let probe = local_origin.begin_probe(candidate, 4).expect("local probe");
    local_origin
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 20,
            },
            5,
        )
        .expect("local probe ack");
    let (local_activation, stage) = local_origin
        .begin_stage(candidate, facts(a, c, b, cached, 6))
        .expect("local stage");
    assert_eq!(
        local_origin.expire_activation(local_activation, 506),
        Ok(FlowPhase::Fenced)
    );
    assert_eq!(
        local_origin.abort_message(local_activation),
        Err(Error::State)
    );

    let mut remote_origin = flow_owner(75, a);
    let candidate = remote_origin
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("remote candidate");
    let probe = remote_origin
        .begin_probe(candidate, 4)
        .expect("remote probe");
    remote_origin
        .on_probe_ack(
            candidate,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 20,
            },
            5,
        )
        .expect("remote probe ack");
    let (remote_activation, _) = remote_origin
        .begin_stage(candidate, facts(a, c, b, cached, 6))
        .expect("remote stage");
    remote_origin
        .complete_stage_submit(remote_activation, ActivationSubmit::Submitted)
        .expect("remote stage submitted");
    assert_eq!(
        remote_origin.expire_activation(remote_activation, 506),
        Ok(FlowPhase::Aborting)
    );
    let abort = remote_origin
        .abort_message(remote_activation)
        .expect("remote abort message");
    assert_eq!(abort.opcode, ProtocolOpcode::PathActivateAbort);
    assert_eq!(abort.key, stage.key);
    assert_eq!(abort.proposal, stage.proposal);

    let mut target = flow_owner(76, c);
    match target
        .accept_stage(&stage, hop_facts(c, link(8, b, 10), None, 7))
        .expect("target stage")
    {
        RelayStageAction::Ack { .. } => {}
        RelayStageAction::Forward { .. } => panic!("target must not forward"),
    }
    assert!(!target.maintain_one(507));
    assert!(!target.maintain_one(507));
    assert!(target.maintain_one(507));
    assert_eq!(
        target.accept_commit(
            &ActivationMessage {
                opcode: ProtocolOpcode::PathActivateCommit,
                ..stage
            },
            hop_facts(c, link(8, b, 10), None, 507)
        ),
        Err(Error::State)
    );
    assert_eq!(target.counts(), (0, 1, 0, 1));
}

#[test]
fn candidate_and_ready_probe_expiry_are_bounded_and_cannot_stage() {
    let a = binding(1, 0xA1);
    let b = binding(2, 0xB2);
    let c = binding(3, 0xC3);
    let cached = capability(b, 10_000);
    let mut routes = route_owner(a);
    let route = soft_route(&mut routes, b, c, 1, 10, 1);

    let mut idle_owner = flow_owner(72, a);
    let idle = idle_owner
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("idle candidate");
    assert!(idle_owner.maintain_one(route.expires_at_us()));
    assert_eq!(idle_owner.candidate_view(idle), Err(Error::NotFound));

    let mut ready_owner = flow_owner(73, a);
    let ready = ready_owner
        .import_candidate(route, cached, requirements(HeaderContract::C4, 9_000), 3)
        .expect("ready candidate");
    let probe = ready_owner.begin_probe(ready, 4).expect("probe");
    ready_owner
        .on_probe_ack(
            ready,
            ProbeAck {
                key: probe.proposal.key(),
                path_frame_mtu: 190,
                capability_bits: 0x010B,
                measured_rtt_us: 20,
            },
            5,
        )
        .expect("probe ack");
    let deadline = 504;
    let before = ready_owner.counts();
    assert_eq!(
        ready_owner.begin_stage(ready, facts(a, c, b, cached, deadline)),
        Err(Error::Timeout)
    );
    assert_eq!(ready_owner.counts(), before);
    assert_eq!(
        ready_owner.candidate_view(ready),
        Ok((FlowPhase::ReadyToStage, Some(probe.proposal)))
    );
    assert!(ready_owner.maintain_one(deadline));
    assert_eq!(ready_owner.candidate_view(ready), Err(Error::NotFound));
}
