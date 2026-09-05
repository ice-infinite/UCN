#include "ucn/v6/ucn_v6_route.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static ucn_v6_principal_t principal(uint8_t seed)
{
    ucn_v6_principal_t value;
    size_t index;
    for (index = 0U; index < sizeof(value.bytes); ++index) {
        value.bytes[index] = (uint8_t)(seed + index);
    }
    return value;
}

static ucn_v6_route_domain_t domain(uint8_t origin_seed,
                                    uint8_t destination_seed,
                                    uint32_t origin_generation,
                                    uint32_t destination_generation)
{
    ucn_v6_route_domain_t value;
    memset(&value, 0, sizeof(value));
    value.origin_principal = principal(origin_seed);
    value.origin_binding.realm_id = 1U;
    value.origin_binding.node_address = (uint32_t)origin_seed;
    value.origin_binding.binding_generation = origin_generation;
    value.origin_session_generation = 3U;
    value.destination_principal = principal(destination_seed);
    value.destination_binding.realm_id = 1U;
    value.destination_binding.node_address = (uint32_t)destination_seed;
    value.destination_binding.binding_generation = destination_generation;
    value.destination_session_generation = 5U;
    return value;
}

static ucn_v6_capability_record_t capability_record(
    uint32_t generation,
    uint32_t link_generation)
{
    ucn_v6_capability_record_t value;
    memset(&value, 0, sizeof(value));
    value.capability_generation = generation;
    value.link.link_instance_generation = link_generation;
    value.link.carrier_mtu = 512U;
    value.link.link_frame_mtu = 256U;
    value.link.processing_frame_mtu = 220U;
    value.link.carrier_header_bytes = 2U;
    value.link.carrier_padding_bytes = 1U;
    value.link.carrier_crc_bytes = 2U;
    value.link.carrier_tag_bytes = 8U;
    value.link.carrier_max_fragments = 32U;
    value.link.link_flags = UCN_V6_LINK_ORDERED | UCN_V6_LINK_RELIABLE |
                            UCN_V6_LINK_UNICAST | UCN_V6_LINK_SECURITY;
    value.link.nominal_rate_bps = 3000000U;
    value.link.hardware_priority_count = 8U;
    value.link.timestamp_capability_bits = UCN_V6_TIMESTAMP_RX_HARDWARE |
                                           UCN_V6_TIMESTAMP_TX_HARDWARE;
    value.link.timestamp_uncertainty_us = 4U;
    value.peer.feature_bits = UCN_V6_COMPILED_FEATURE_BITS |
                              UCN_V6_FEATURE_TRANSFER;
    value.peer.hop_suite_bits = UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    value.peer.e2e_suite_bits = UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    value.peer.max_message_class =
        (ucn_v6_message_class_t)UCN_V6_CONFIG_TRANSFER_MAX_CLASS;
    value.peer.max_rx_window = UCN_V6_CONFIG_TRANSFER_WINDOW;
    value.peer.max_concurrent_transfers = UCN_V6_CONFIG_TRANSFER_RX_SLOTS;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    value.peer.realtime_mode_bits = UCN_V6_REALTIME_MODE_SYNCED |
                                    UCN_V6_REALTIME_MODE_DEADLINE;
    value.peer.clock_domain_id = 1U;
    value.peer.clock_domain_generation = 1U;
#endif
    return value;
}

static ucn_v6_security_open_result_t peer_open(
    const ucn_v6_principal_t *peer,
    const ucn_v6_binding_key_t *binding,
    uint32_t session_generation,
    uint16_t link_id,
    uint32_t link_generation,
    const uint8_t *payload)
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.hop_authenticated = true;
    opened.authenticated_principal = *peer;
    opened.ingress_peer_session.principal = *peer;
    opened.ingress_peer_session.binding = *binding;
    opened.ingress_peer_session.session_generation = session_generation;
    opened.ingress_link_instance_id = link_id;
    opened.ingress_link_instance_generation = link_generation;
    opened.frame.frame_type = UCN_V6_FRAME_CONTROL;
    opened.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_PROTOCOL_CONTEXT;
    opened.frame.realm_id = binding->realm_id;
    opened.frame.source_address = binding->node_address;
    opened.frame.source_binding_generation = binding->binding_generation;
    opened.frame.session_generation = session_generation;
    opened.frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE;
    opened.frame.payload = payload;
    opened.frame.payload_length = UCN_V6_CAPABILITY_RECORD_BYTES;
    return opened;
}

static ucn_v6_frame_t budget_frame(void)
{
    ucn_v6_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.address_class = UCN_V6_ADDRESS_CLASS_A0;
    frame.frame_type = UCN_V6_FRAME_TRANSFER;
    frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                  UCN_V6_FLAG_E2E_CONTEXT |
                  UCN_V6_FLAG_PROTOCOL_CONTEXT |
                  UCN_V6_FLAG_MESSAGE_CONTEXT |
                  UCN_V6_FLAG_PATH_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q3;
    frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    frame.hop_limit = 8U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = 1U;
    frame.source_address = 2U;
    frame.destination_address = 7U;
    frame.source_binding_generation = 2U;
    frame.destination_binding_generation = 3U;
    frame.session_generation = 5U;
    frame.origin_sequence = 1U;
    frame.hop_sequence = 1U;
    frame.peer_hop.suite_id = UCN_V6_SUITE_HMAC_SHA256_128;
    frame.peer_hop.key_id = 1U;
    frame.peer_hop.key_generation = 1U;
    frame.e2e.mode = UCN_V6_E2E_AEAD;
    frame.e2e.suite_id = UCN_V6_SUITE_AES_GCM_128;
    frame.e2e.key_id = 2U;
    frame.e2e.key_generation = 1U;
    frame.protocol_opcode = 40U;
    frame.message.source_endpoint = 10U;
    frame.message.destination_endpoint = 20U;
    frame.message.interaction_role = UCN_V6_INTERACTION_ONE_WAY;
    frame.path.path_id = 4U;
    frame.path.path_generation = 6U;
    return frame;
}

typedef struct fixture {
    ucn_v6_capability_owner_storage_t capability_storage;
    ucn_v6_capability_owner_t *capability_owner;
    ucn_v6_route_owner_storage_t route_storage;
    ucn_v6_route_owner_t *route_owner;
    ucn_v6_route_domain_t route_domain;
    ucn_v6_capability_record_t destination_capability;
    ucn_v6_capability_record_t next_hop_capability[2];
    ucn_v6_principal_t next_hop_principal[2];
    ucn_v6_binding_key_t next_hop_binding[2];
} fixture_t;

static int fixture_init(fixture_t *fixture)
{
    ucn_v6_capability_record_t local = capability_record(1U, 1U);
    ucn_v6_security_open_result_t opened;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    fixture->route_domain = domain(2U, 7U, 2U, 3U);
    fixture->destination_capability = capability_record(1U, 9U);
    CHECK(ucn_v6_capability_owner_init_in_place(
              fixture->capability_storage.bytes,
              sizeof(fixture->capability_storage),
              ucn_v6_compiled_manifest(), &local, 100000U, 100000U,
              &fixture->capability_owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(
              &fixture->destination_capability, payload) == UCN_V6_OK);
    opened = peer_open(&fixture->route_domain.destination_principal,
                       &fixture->route_domain.destination_binding, 5U, 4U, 6U,
                       payload);
    CHECK(ucn_v6_capability_ingest_advertise(
              fixture->capability_owner, 1U, &opened,
              &fixture->destination_capability) == UCN_V6_OK);
    for (index = 0U; index < 2U; ++index) {
        fixture->next_hop_principal[index] =
            principal((uint8_t)(0x40U + index * 0x10U));
        fixture->next_hop_binding[index].realm_id = 1U;
        fixture->next_hop_binding[index].node_address =
            (uint32_t)(20U + index);
        fixture->next_hop_binding[index].binding_generation = 1U;
        fixture->next_hop_capability[index] =
            capability_record(1U, (uint32_t)(21U + index));
        CHECK(ucn_v6_capability_record_encode(
                  &fixture->next_hop_capability[index], payload) == UCN_V6_OK);
        opened = peer_open(&fixture->next_hop_principal[index],
                           &fixture->next_hop_binding[index], 1U,
                           (uint16_t)(10U + index), 1U, payload);
        CHECK(ucn_v6_capability_ingest_advertise(
                  fixture->capability_owner, 1U, &opened,
                  &fixture->next_hop_capability[index]) == UCN_V6_OK);
    }
    CHECK(ucn_v6_route_owner_init_in_place(
              fixture->route_storage.bytes, sizeof(fixture->route_storage),
              ucn_v6_compiled_manifest(), NULL, 1000U, 100U, 3U, 200U,
              500U, &fixture->route_owner) == UCN_V6_ERR_CONFIG);
    CHECK(fixture->route_owner == NULL);
    CHECK(ucn_v6_route_owner_init_in_place(
              fixture->route_storage.bytes, sizeof(fixture->route_storage),
              ucn_v6_compiled_manifest(), fixture->capability_owner,
              1000U, 100U, 3U, 200U, 500U, &fixture->route_owner) ==
          UCN_V6_OK);
    return 0;
}

static ucn_v6_route_path_t route_path(fixture_t *fixture,
                                      uint16_t path_id,
                                      uint32_t path_generation,
                                      uint32_t route_generation,
                                      size_t next_hop_index,
                                      uint16_t priority,
                                      uint16_t weight)
{
    ucn_v6_path_budget_request_t request;
    ucn_v6_path_budget_request_t downstream_request;
    ucn_v6_path_budget_accumulator_t accumulator;
    ucn_v6_path_budget_accumulator_t downstream_accumulator;
    ucn_v6_path_capability_t downstream_path;
    ucn_v6_route_path_t path;
    ucn_v6_frame_t frame = budget_frame();
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    memset(&request, 0, sizeof(request));
    memset(&path, 0, sizeof(path));
    (void)ucn_v6_capability_digest(&fixture->destination_capability, digest);
    request.destination_principal =
        fixture->route_domain.destination_principal;
    request.destination_binding = fixture->route_domain.destination_binding;
    request.destination_session_generation = 5U;
    request.destination_capability_generation =
        fixture->destination_capability.capability_generation;
    memcpy(request.destination_capability_digest, digest, sizeof(digest));
    request.destination_realtime_mode_bits =
        fixture->destination_capability.peer.realtime_mode_bits;
    request.destination_clock_domain_id =
        fixture->destination_capability.peer.clock_domain_id;
    request.destination_clock_domain_generation =
        fixture->destination_capability.peer.clock_domain_generation;
    request.route_generation = route_generation;
    request.path_id = path_id;
    request.path_generation = path_generation;
    request.deadline_us = 50000U;
    request.fixed_path = true;
    request.path_policy_frame_mtu = 200U;
    request.required_feature_bits = UCN_V6_FEATURE_ROUTE;
    request.required_hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    request.required_e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    request.policy_max_message_class = UCN_V6_MESSAGE_T8K;
    request.policy_max_window = 8U;
    request.policy_max_concurrency = 2U;
    request.frame_contract = &frame;
    request.fragment_header_bytes = 16U;
    downstream_request = request;
    downstream_request.route_generation = 1U;
    downstream_request.path_id = (uint16_t)(100U + path_id);
    downstream_request.path_generation = 1U;
    (void)ucn_v6_capability_path_reduce_begin(
        &downstream_request, &downstream_accumulator);
    {
        ucn_v6_capability_peer_ref_t destination_ref;
        memset(&destination_ref, 0, sizeof(destination_ref));
        destination_ref.principal = fixture->route_domain.destination_principal;
        destination_ref.binding = fixture->route_domain.destination_binding;
        destination_ref.session_generation = 5U;
        destination_ref.ingress_link_id = 4U;
        destination_ref.ingress_link_generation = 6U;
        (void)ucn_v6_capability_path_reduce_hop(
            fixture->capability_owner, 2U, &downstream_accumulator,
            &destination_ref);
    }
    (void)ucn_v6_capability_path_reduce_finalize(
        &downstream_accumulator, &downstream_path);
    (void)ucn_v6_capability_path_reduce_begin(&request, &accumulator);
    {
        ucn_v6_capability_peer_ref_t next_hop_ref;
        memset(&next_hop_ref, 0, sizeof(next_hop_ref));
        next_hop_ref.principal = fixture->next_hop_principal[next_hop_index];
        next_hop_ref.binding = fixture->next_hop_binding[next_hop_index];
        next_hop_ref.session_generation = 1U;
        next_hop_ref.ingress_link_id = (uint16_t)(10U + next_hop_index);
        next_hop_ref.ingress_link_generation = 1U;
        (void)ucn_v6_capability_path_reduce_downstream(
            &accumulator, &downstream_path, 50000U);
        (void)ucn_v6_capability_path_reduce_hop(
            fixture->capability_owner, 2U, &accumulator, &next_hop_ref);
    }
    (void)ucn_v6_capability_path_reduce_finalize(
        &accumulator, &path.capability);
    (void)ucn_v6_capability_install_path(fixture->capability_owner, 2U,
                                         &path.capability);
    path.path_id = path_id;
    path.path_generation = path_generation;
    path.next_hop.principal = fixture->next_hop_principal[next_hop_index];
    path.next_hop.binding = fixture->next_hop_binding[next_hop_index];
    path.next_hop.session_generation = 1U;
    path.egress_link_id = (uint16_t)(10U + next_hop_index);
    path.egress_link_generation = 1U;
    path.next_hop_capability_generation =
        fixture->next_hop_capability[next_hop_index].capability_generation;
    path.hop_count = path.capability.hop_count;
    path.priority = priority;
    path.weight = weight;
    path.available = true;
    return path;
}

static int activate_candidate(fixture_t *fixture,
                              uint64_t candidate_id,
                              uint32_t route_generation,
                              bool two_paths,
                              ucn_v6_route_activation_t *activation)
{
    ucn_v6_route_path_t first = route_path(
        fixture, 1U, route_generation, route_generation, 0U, 1U, 1U);
    ucn_v6_route_path_t second;
    CHECK(ucn_v6_route_candidate_begin(
              fixture->route_owner, 10U, candidate_id,
              &fixture->route_domain, route_generation) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_add_path(
              fixture->route_owner, 11U, candidate_id,
              &fixture->route_domain, &first) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_probe(
              fixture->route_owner, 12U, candidate_id,
              &fixture->route_domain, first.path_id,
              UCN_V6_SERIAL_ROTATION_THRESHOLD + UINT32_C(1)) ==
          UCN_V6_ERR_ARGUMENT);
    if (two_paths) {
        second = route_path(fixture, 2U, route_generation,
                            route_generation, 1U, 2U, 3U);
        CHECK(ucn_v6_route_candidate_add_path(
                  fixture->route_owner, 12U, candidate_id,
                  &fixture->route_domain,
                  &second) == UCN_V6_OK);
    }
    CHECK(ucn_v6_route_candidate_record_probe(
              fixture->route_owner, 13U, candidate_id,
              &fixture->route_domain, 1U,
              route_generation) == UCN_V6_OK);
    if (two_paths) {
        CHECK(ucn_v6_route_candidate_record_probe(
                  fixture->route_owner, 14U, candidate_id,
                  &fixture->route_domain, 2U,
                  route_generation) == UCN_V6_OK);
    }
    CHECK(ucn_v6_route_candidate_prepare_activation(
              fixture->route_owner, 15U, candidate_id,
              &fixture->route_domain, activation) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_activation_send(
              fixture->route_owner, 15U, candidate_id,
              &fixture->route_domain, true) == UCN_V6_OK);
    return 0;
}

static int test_atomic_activation_and_frozen_candidate(void)
{
    fixture_t fixture;
    ucn_v6_route_activation_t activation;
    ucn_v6_route_activation_t bad;
    ucn_v6_route_candidate_view_t before;
    ucn_v6_route_candidate_view_t after;
    ucn_v6_route_path_t changed;
    ucn_v6_route_view_t view;
    CHECK(fixture_init(&fixture) == 0);
    CHECK(activate_candidate(&fixture, 101U, 1U, true, &activation) == 0);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 101U, &fixture.route_domain,
              &before) == UCN_V6_OK);
    changed = before.proposal.paths[0];
    changed.egress_link_id = 99U;
    CHECK(ucn_v6_route_candidate_add_path(
              fixture.route_owner, 16U, 101U, &fixture.route_domain,
              &changed) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 101U, &fixture.route_domain,
              &after) == UCN_V6_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);

    bad = activation;
    --bad.route_generation;
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture.route_owner, 17U, &bad) == UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 101U, &fixture.route_domain,
              &after) == UCN_V6_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture.route_owner, 18U, &activation) == UCN_V6_OK);
    CHECK(ucn_v6_route_copy_view(fixture.route_owner, &view) == UCN_V6_OK);
    CHECK(view.route_sets == 1U && view.candidates == 0U &&
          view.activations == 1U);
    return 0;
}

static int test_selection_rerr_and_generation_grace(void)
{
    fixture_t fixture;
    ucn_v6_route_activation_t activation;
    ucn_v6_route_select_request_t request;
    ucn_v6_route_selection_t first;
    ucn_v6_route_selection_t second;
    ucn_v6_route_path_ref_t reference;
    ucn_v6_route_resolution_t resolved;
    ucn_v6_route_resolution_t sentinel;
    CHECK(fixture_init(&fixture) == 0);
    CHECK(activate_candidate(&fixture, 101U, 1U, true, &activation) == 0);
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture.route_owner, 18U, &activation) == UCN_V6_OK);
    memset(&request, 0, sizeof(request));
    request.domain = fixture.route_domain;
    request.flow_id = 77U;
    request.policy = UCN_V6_ROUTE_POLICY_PER_FLOW_HASH;
    CHECK(ucn_v6_route_select(fixture.route_owner, 20U, &request,
                              &first) == UCN_V6_OK);
    CHECK(ucn_v6_route_select(fixture.route_owner, 21U, &request,
                              &second) == UCN_V6_OK);
    CHECK(second.reused_flow_pin);
    CHECK(first.path.path_id == second.path.path_id);
    request.policy = UCN_V6_ROUTE_POLICY_PINNED;
    request.pinned_path_id = 2U;
    request.pinned_path_generation = 1U;
    CHECK(ucn_v6_route_select(fixture.route_owner, 21U, &request,
                              &second) == UCN_V6_OK);
    CHECK(second.path.path_id == 2U);
    request.policy = UCN_V6_ROUTE_POLICY_WEIGHTED_MULTIPATH;
    request.allow_reordering = false;
    CHECK(ucn_v6_route_select(fixture.route_owner, 21U, &request,
                              &second) == UCN_V6_ERR_ACCESS);
    request.allow_reordering = true;
    request.packet_sequence = 1U;
    CHECK(ucn_v6_route_select(fixture.route_owner, 21U, &request,
                              &second) == UCN_V6_OK);
    request.policy = UCN_V6_ROUTE_POLICY_PER_FLOW_HASH;
    request.allow_reordering = false;
    CHECK(ucn_v6_route_mark_error(
              fixture.route_owner, &fixture.route_domain, 1U,
              first.path.path_id, first.path.path_generation) == UCN_V6_OK);
    CHECK(ucn_v6_route_select(fixture.route_owner, 22U, &request,
                              &second) == UCN_V6_OK);
    CHECK(second.path.path_id != first.path.path_id);
    CHECK(ucn_v6_route_mark_error(
              fixture.route_owner, &fixture.route_domain, 2U,
              second.path.path_id, second.path.path_generation) ==
          UCN_V6_ERR_REPLAY);

    CHECK(activate_candidate(&fixture, 102U, 2U, false, &activation) == 0);
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture.route_owner, 30U, &activation) == UCN_V6_OK);
    CHECK(ucn_v6_route_accept_generation(
              fixture.route_owner, 31U, &fixture.route_domain,
              1U) == UCN_V6_OK);
    memset(&reference, 0, sizeof(reference));
    reference.domain = fixture.route_domain;
    reference.route_generation = 1U;
    reference.path_id = 2U;
    reference.path_generation = 1U;
    CHECK(ucn_v6_route_resolve_ref(
              fixture.route_owner, 31U, &reference,
              &resolved) == UCN_V6_OK);
    CHECK(resolved.path.path_id == 2U &&
          resolved.path.path_generation == 1U);
    CHECK(ucn_v6_stack_invalidation_is_valid(&resolved.dependency));
    memset(&resolved, 0xA5, sizeof(resolved));
    sentinel = resolved;
    reference.path_id = 99U;
    CHECK(ucn_v6_route_resolve_ref(
              fixture.route_owner, 31U, &reference,
              &resolved) == UCN_V6_ERR_NOT_FOUND);
    CHECK(memcmp(&resolved, &sentinel, sizeof(resolved)) == 0);
    CHECK(ucn_v6_route_accept_generation(
              fixture.route_owner, 230U, &fixture.route_domain,
              1U) == UCN_V6_ERR_REPLAY);
    reference.path_id = 2U;
    CHECK(ucn_v6_route_resolve_ref(
              fixture.route_owner, 230U, &reference,
              &resolved) == UCN_V6_ERR_REPLAY);
    CHECK(memcmp(&resolved, &sentinel, sizeof(resolved)) == 0);
    CHECK(ucn_v6_route_expire(fixture.route_owner, 230U) == UCN_V6_OK);
    return 0;
}

static int test_cross_origin_retry_and_identity_aba(void)
{
    fixture_t fixture;
    ucn_v6_route_activation_t activation;
    ucn_v6_route_activation_t retry;
    ucn_v6_route_domain_t other;
    ucn_v6_route_domain_t missing;
    ucn_v6_route_candidate_view_t before;
    ucn_v6_route_candidate_view_t after;
    ucn_v6_route_candidate_view_t other_candidate;
    ucn_v6_route_candidate_view_t untouched;
    ucn_v6_route_candidate_view_t sentinel;
    ucn_v6_route_path_t path;
    CHECK(fixture_init(&fixture) == 0);
    other = fixture.route_domain;
    other.origin_principal = principal(0x70U);
    other.origin_binding.node_address = 2U;
    other.origin_binding.binding_generation = 99U;
    CHECK(ucn_v6_route_candidate_begin(
              fixture.route_owner, 10U, 101U, &fixture.route_domain,
              1U) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_begin(
              fixture.route_owner, 10U, 101U, &other,
              1U) == UCN_V6_OK);
    path = route_path(&fixture, 1U, 1U, 1U, 0U, 1U, 1U);
    CHECK(ucn_v6_route_candidate_add_path(
              fixture.route_owner, 11U, 101U, &other,
              &path) == UCN_V6_OK);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 101U, &fixture.route_domain,
              &before) == UCN_V6_OK);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 101U, &other,
              &other_candidate) == UCN_V6_OK);
    CHECK(before.proposal.path_count == 0U);
    CHECK(other_candidate.proposal.path_count == 1U);
    missing = other;
    ++missing.origin_binding.binding_generation;
    memset(&untouched, 0xA5, sizeof(untouched));
    sentinel = untouched;
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 101U, &missing,
              &untouched) == UCN_V6_ERR_NOT_FOUND);
    CHECK(memcmp(&untouched, &sentinel, sizeof(untouched)) == 0);
    CHECK(ucn_v6_route_candidate_record_probe(
              fixture.route_owner, 12U, 101U, &missing,
              1U, 1U) == UCN_V6_ERR_NOT_FOUND);
    CHECK(ucn_v6_route_candidate_add_path(
              fixture.route_owner, 11U, 101U, &fixture.route_domain,
              &path) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_probe(
              fixture.route_owner, 12U, 101U, &fixture.route_domain,
              1U, 1U) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              fixture.route_owner, 13U, 101U, &fixture.route_domain,
              &activation) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_activation_send(
              fixture.route_owner, 13U, 101U, &fixture.route_domain,
              false) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              fixture.route_owner, 14U, 101U, &fixture.route_domain,
              &retry) == UCN_V6_OK);
    CHECK(memcmp(&activation, &retry, sizeof(activation)) == 0);
    CHECK(ucn_v6_route_candidate_record_activation_send(
              fixture.route_owner, 14U, 101U, &fixture.route_domain,
              true) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              fixture.route_owner, 113U, 101U, &fixture.route_domain,
              &retry) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              fixture.route_owner, 114U, 101U, &fixture.route_domain,
              &retry) == UCN_V6_OK);
    CHECK(memcmp(&activation, &retry, sizeof(activation)) == 0);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 101U, &fixture.route_domain,
              &after) == UCN_V6_OK);
    CHECK(after.activation_attempts == 1U);
    CHECK(before.proposal.path_count == 0U);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 101U, &other,
              &other_candidate) == UCN_V6_OK);
    CHECK(other_candidate.proposal.path_count == 1U &&
          other_candidate.probed_mask == 0U &&
          other_candidate.activation_attempts == 0U);
    CHECK(ucn_v6_route_expire(fixture.route_owner, 1010U) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_begin(
              fixture.route_owner, 1011U, 101U, &fixture.route_domain,
              1U) == UCN_V6_ERR_REPLAY);
    return 0;
}

static int test_capacity_and_output_no_write(void)
{
    fixture_t fixture;
    ucn_v6_route_domain_t item_domain;
    ucn_v6_route_activation_t output;
    ucn_v6_route_activation_t before;
    size_t index;
    CHECK(fixture_init(&fixture) == 0);
    memset(&output, 0xA5, sizeof(output));
    before = output;
    CHECK(ucn_v6_route_candidate_begin(
              fixture.route_owner, 1U, 1U, &fixture.route_domain,
              1U) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              fixture.route_owner, 2U, 1U, &fixture.route_domain,
              &output) == UCN_V6_ERR_STATE);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    for (index = 1U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        item_domain = domain((uint8_t)(10U + index),
                             (uint8_t)(100U + index), 1U, 1U);
        CHECK(ucn_v6_route_candidate_begin(
                  fixture.route_owner, 1U, (uint64_t)(index + 1U),
                  &item_domain, 1U) == UCN_V6_OK);
    }
    item_domain = domain(90U, 91U, 1U, 1U);
    CHECK(ucn_v6_route_candidate_begin(
              fixture.route_owner, 1U, 1000U, &item_domain,
              1U) == UCN_V6_ERR_NO_SPACE);
    CHECK(ucn_v6_route_expire(fixture.route_owner, 1001U) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_begin(
              fixture.route_owner, 1002U, 1001U, &item_domain,
              1U) == UCN_V6_ERR_NO_SPACE);
    return 0;
}

static int test_capability_change_and_session_invalidation(void)
{
    fixture_t fixture;
    ucn_v6_route_activation_t activation;
    ucn_v6_route_select_request_t request;
    ucn_v6_route_selection_t selection;
    ucn_v6_route_selection_t before;
    ucn_v6_route_proposal_t route_set;
    ucn_v6_security_open_result_t opened;
    ucn_v6_session_key_t destination_session;
    ucn_v6_stack_invalidation_t invalidation;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    CHECK(fixture_init(&fixture) == 0);
    CHECK(activate_candidate(&fixture, 101U, 1U, false, &activation) == 0);
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture.route_owner, 18U, &activation) == UCN_V6_OK);
    memset(&request, 0, sizeof(request));
    request.domain = fixture.route_domain;
    request.flow_id = 9U;
    request.policy = UCN_V6_ROUTE_POLICY_ACTIVE_STANDBY;
    CHECK(ucn_v6_route_select(
              fixture.route_owner, 20U, &request, &selection) == UCN_V6_OK);

    /* The aggregate is revoked by its immediately trusted parent.  A source
     * need not cache the remote destination as a direct Peer. */
    ++fixture.next_hop_capability[0].capability_generation;
    ++fixture.next_hop_capability[0].link.nominal_rate_bps;
    CHECK(ucn_v6_capability_record_encode(
              &fixture.next_hop_capability[0], payload) == UCN_V6_OK);
    opened = peer_open(&fixture.next_hop_principal[0],
                       &fixture.next_hop_binding[0], 1U, 10U, 1U, payload);
    CHECK(ucn_v6_capability_ingest_advertise(
              fixture.capability_owner, 21U, &opened,
              &fixture.next_hop_capability[0]) == UCN_V6_OK);
    memset(&selection, 0xA5, sizeof(selection));
    before = selection;
    CHECK(ucn_v6_route_select(
              fixture.route_owner, 22U, &request,
              &selection) == UCN_V6_ERR_NOT_FOUND);
    CHECK(memcmp(&selection, &before, sizeof(selection)) == 0);

    destination_session.principal = fixture.next_hop_principal[0];
    destination_session.binding = fixture.next_hop_binding[0];
    destination_session.session_generation = 1U;
    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = UCN_V6_STACK_INVALIDATE_SESSION;
    invalidation.link_id = 10U;
    invalidation.link_generation = 1U;
    invalidation.session = destination_session;
    CHECK(ucn_v6_route_apply_invalidation(
              fixture.route_owner, &invalidation) == UCN_V6_OK);
    memset(&route_set, 0x5A, sizeof(route_set));
    CHECK(ucn_v6_route_copy_set(
              fixture.route_owner, &fixture.route_domain,
              &route_set) == UCN_V6_ERR_NOT_FOUND);

    /* A canonical Link event must not need Capability state that the Owner
     * fan-out has already removed. Route stores its own exact egress parent. */
    CHECK(fixture_init(&fixture) == 0);
    CHECK(activate_candidate(&fixture, 102U, 1U, false, &activation) == 0);
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture.route_owner, 18U, &activation) == UCN_V6_OK);
    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = UCN_V6_STACK_INVALIDATE_LINK;
    invalidation.link_id = 10U;
    invalidation.link_generation = 1U;
    CHECK(ucn_v6_route_apply_invalidation(
              fixture.route_owner, &invalidation) == UCN_V6_OK);
    CHECK(ucn_v6_route_copy_set(
              fixture.route_owner, &fixture.route_domain,
              &route_set) == UCN_V6_ERR_NOT_FOUND);
    return 0;
}

static int test_path_lease_refresh_and_delayed_old_event(void)
{
    fixture_t fixture;
    ucn_v6_route_activation_t activation;
    ucn_v6_route_select_request_t request;
    ucn_v6_route_selection_t selection;
    ucn_v6_route_proposal_t current;
    ucn_v6_path_capability_t refreshed;
    ucn_v6_path_capability_t malformed;
    ucn_v6_stack_invalidation_t old_path;

    CHECK(fixture_init(&fixture) == 0);
    CHECK(activate_candidate(&fixture, 101U, 1U, false, &activation) == 0);
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture.route_owner, 18U, &activation) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_path(
              fixture.capability_owner, 20U,
              &fixture.route_domain.destination_principal,
              &fixture.route_domain.destination_binding, 5U, 1U, 1U, 1U,
              &refreshed) == UCN_V6_OK);
    CHECK(refreshed.deadline_us == 50000U);
    refreshed.deadline_us = 60000U;
    CHECK(ucn_v6_capability_install_path(
              fixture.capability_owner, 21U, &refreshed) == UCN_V6_OK);

    memset(&request, 0, sizeof(request));
    request.domain = fixture.route_domain;
    request.flow_id = 501U;
    request.policy = UCN_V6_ROUTE_POLICY_ACTIVE_STANDBY;
    CHECK(ucn_v6_route_select(fixture.route_owner, 50000U, &request,
                              &selection) == UCN_V6_OK);
    CHECK(selection.path.capability.deadline_us == 60000U);
    CHECK(ucn_v6_route_select(fixture.route_owner, 60000U, &request,
                              &selection) == UCN_V6_ERR_NOT_FOUND);

    malformed = refreshed;
    --malformed.payload_budget;
    CHECK(ucn_v6_capability_install_path(
              fixture.capability_owner, 22U, &malformed) ==
          UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_capability_copy_path(
              fixture.capability_owner, 23U,
              &fixture.route_domain.destination_principal,
              &fixture.route_domain.destination_binding, 5U, 1U, 1U, 1U,
              &malformed) == UCN_V6_OK);
    CHECK(memcmp(&malformed, &refreshed, sizeof(refreshed)) == 0);

    CHECK(activate_candidate(&fixture, 102U, 2U, false, &activation) == 0);
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture.route_owner, 30U, &activation) == UCN_V6_OK);
    memset(&old_path, 0, sizeof(old_path));
    old_path.type = UCN_V6_STACK_INVALIDATE_PATH;
    old_path.link_id = 4U;
    old_path.link_generation = 6U;
    old_path.session.principal = fixture.route_domain.destination_principal;
    old_path.session.binding = fixture.route_domain.destination_binding;
    old_path.session.session_generation = 5U;
    old_path.capability_generation = 1U;
    old_path.path_id = 1U;
    old_path.path_generation = 1U;
    CHECK(ucn_v6_route_apply_invalidation(
              fixture.route_owner, &old_path) == UCN_V6_OK);
    CHECK(ucn_v6_route_copy_set(fixture.route_owner,
                                &fixture.route_domain, &current) == UCN_V6_OK);
    CHECK(current.route_generation == 2U);
    CHECK(current.paths[0].path_generation == 2U);
    CHECK(ucn_v6_route_candidate_begin(
              fixture.route_owner, 31U, 102U, &fixture.route_domain, 3U) ==
          UCN_V6_ERR_REPLAY);
    return 0;
}

static int test_route_hop_count_matches_capability_width(void)
{
    fixture_t fixture;
    ucn_v6_route_path_t path;
    ucn_v6_route_candidate_view_t candidate_before;
    ucn_v6_route_candidate_view_t candidate_after;
    CHECK(fixture_init(&fixture) == 0);
    path = route_path(&fixture, 61U, 1U, 1U, 0U, 1U, 1U);
    path.hop_count = 300U;
    CHECK(path.hop_count == 300U);
    path.hop_count = UCN_V6_PATH_HOP_LIMIT;
    CHECK(path.hop_count == UCN_V6_PATH_HOP_LIMIT);
    path.path_id = 62U;
    path.capability.path_id = 62U;
    path.capability.hop_count = UCN_V6_HOP_COUNT_MAX;
    CHECK(ucn_v6_capability_install_path(
              fixture.capability_owner, 2U, &path.capability) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_begin(
              fixture.route_owner, 10U, 901U, &fixture.route_domain, 1U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_add_path(
              fixture.route_owner, 11U, 901U, &fixture.route_domain,
              &path) == UCN_V6_OK);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 901U, &fixture.route_domain,
              &candidate_before) == UCN_V6_OK);
    path.hop_count = UINT16_MAX;
    path.capability.hop_count = UINT16_MAX;
    CHECK(ucn_v6_route_candidate_add_path(
              fixture.route_owner, 11U, 901U, &fixture.route_domain,
              &path) != UCN_V6_OK);
    CHECK(ucn_v6_route_copy_candidate(
              fixture.route_owner, 901U, &fixture.route_domain,
              &candidate_after) == UCN_V6_OK);
    CHECK(memcmp(&candidate_before, &candidate_after,
                 sizeof(candidate_before)) == 0);
    return 0;
}

int main(void)
{
    CHECK(test_atomic_activation_and_frozen_candidate() == 0);
    CHECK(test_selection_rerr_and_generation_grace() == 0);
    CHECK(test_cross_origin_retry_and_identity_aba() == 0);
    CHECK(test_capacity_and_output_no_write() == 0);
    CHECK(test_capability_change_and_session_invalidation() == 0);
    CHECK(test_path_lease_refresh_and_delayed_old_event() == 0);
    CHECK(test_route_hop_count_matches_capability_width() == 0);
    puts("ucn v6 route tests passed");
    return 0;
}
