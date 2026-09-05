#include "ucn/v6/ucn_v6_capability.h"

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

static ucn_v6_capability_record_t record(uint32_t generation,
                                         uint32_t link_generation,
                                         uint32_t frame_mtu)
{
    ucn_v6_capability_record_t value;
    memset(&value, 0, sizeof(value));
    value.capability_generation = generation;
    value.link.link_instance_generation = link_generation;
    value.link.carrier_mtu = 64U;
    value.link.link_frame_mtu = frame_mtu;
    value.link.processing_frame_mtu = frame_mtu - 8U;
    value.link.carrier_header_bytes = 2U;
    value.link.carrier_padding_bytes = 1U;
    value.link.carrier_crc_bytes = 2U;
    value.link.carrier_tag_bytes = 8U;
    value.link.carrier_max_fragments = 32U;
    value.link.link_flags = UCN_V6_LINK_ORDERED | UCN_V6_LINK_RELIABLE |
                            UCN_V6_LINK_UNICAST | UCN_V6_LINK_SECURITY;
    value.link.nominal_rate_bps = 3000000U;
    value.link.hardware_priority_count = 8U;
    value.link.timestamp_capability_bits =
        UCN_V6_TIMESTAMP_RX_HARDWARE | UCN_V6_TIMESTAMP_TX_HARDWARE;
    value.link.timestamp_uncertainty_us = 4U;
    value.peer.feature_bits = UCN_V6_FEATURE_IDENTITY |
                              UCN_V6_FEATURE_WIRE |
                              UCN_V6_FEATURE_MESSAGE |
                              UCN_V6_FEATURE_SECURITY |
                              UCN_V6_FEATURE_CAPABILITY |
                              UCN_V6_FEATURE_ROUTE |
                              UCN_V6_FEATURE_TRANSFER;
    value.peer.hop_suite_bits = UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    value.peer.e2e_suite_bits =
        (UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128) |
        (UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128);
    value.peer.max_message_class = UCN_V6_MESSAGE_T8K;
    value.peer.max_rx_window = 16U;
    value.peer.max_concurrent_transfers = 4U;
    return value;
}

static ucn_v6_security_open_result_t peer_open(
    const ucn_v6_principal_t *peer,
    uint32_t source,
    uint32_t binding_generation,
    uint32_t session_generation,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    uint16_t opcode,
    const uint8_t *payload,
    uint16_t payload_length)
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.hop_authenticated = true;
    opened.authenticated_principal = *peer;
    opened.ingress_peer_session.principal = *peer;
    opened.ingress_peer_session.binding.realm_id = 1U;
    opened.ingress_peer_session.binding.node_address = source;
    opened.ingress_peer_session.binding.binding_generation =
        binding_generation;
    opened.ingress_peer_session.session_generation = session_generation;
    opened.ingress_link_instance_id = ingress_link_id;
    opened.ingress_link_instance_generation = ingress_link_generation;
    opened.frame.frame_type = UCN_V6_FRAME_CONTROL;
    opened.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_PROTOCOL_CONTEXT;
    opened.frame.realm_id = 1U;
    opened.frame.source_address = source;
    opened.frame.source_binding_generation = binding_generation;
    opened.frame.session_generation = session_generation;
    opened.frame.protocol_opcode = opcode;
    opened.frame.payload = payload;
    opened.frame.payload_length = payload_length;
    return opened;
}

static bool session_event_matches(
    const ucn_v6_stack_invalidation_t *event,
    ucn_v6_stack_invalidation_type_t type,
    uint16_t link_id,
    uint32_t link_generation,
    const ucn_v6_principal_t *peer,
    const ucn_v6_binding_key_t *binding,
    uint32_t session_generation)
{
    return event->type == type && event->link_id == link_id &&
           event->link_generation == link_generation &&
           memcmp(event->session.principal.bytes, peer->bytes,
                  sizeof(peer->bytes)) == 0 &&
           ucn_v6_binding_key_equal(&event->session.binding, binding) &&
           event->session.session_generation == session_generation;
}

static int test_codec_and_authenticated_cache(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_record_t remote = record(7U, 9U, 256U);
    ucn_v6_capability_record_t decoded;
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    ucn_v6_principal_t remote_principal = principal(0x20U);
    ucn_v6_binding_key_t remote_binding = { 1U, 7U, 3U };
    ucn_v6_security_open_result_t opened;
    ucn_v6_cached_peer_capability_t cached;
    ucn_v6_capability_summary_t summary;
    ucn_v6_capability_summary_t summary_decoded;
    ucn_v6_capability_query_t query;
    ucn_v6_capability_query_t query_decoded;
    ucn_v6_hello_disposition_t disposition;
    ucn_v6_capability_view_t view;
    ucn_v6_stack_invalidation_t invalidation;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint8_t bad[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];

    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    memset(&decoded, 0xA5, sizeof(decoded));
    CHECK(ucn_v6_capability_record_decode(payload, sizeof(payload),
                                          &decoded) == UCN_V6_OK);
    CHECK(memcmp(&decoded, &remote, sizeof(remote)) == 0);
    memcpy(bad, payload, sizeof(bad));
    bad[63] = 1U;
    memset(&decoded, 0x5A, sizeof(decoded));
    {
        ucn_v6_capability_record_t before = decoded;
        CHECK(ucn_v6_capability_record_decode(bad, sizeof(bad), &decoded) ==
              UCN_V6_ERR_MALFORMED);
        CHECK(memcmp(&decoded, &before, sizeof(decoded)) == 0);
    }
    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 1000U, 300U, &owner) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 7U, 3U, 5U, 2U, 19U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 100U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(owner, 101U, &remote_principal,
                                      &remote_binding, 5U, 19U,
                                      &cached) == UCN_V6_OK);
    CHECK(ucn_v6_capability_digest(&remote, digest) == UCN_V6_OK);
    CHECK(memcmp(cached.digest, digest, sizeof(digest)) == 0);
    summary.capability_generation = remote.capability_generation;
    summary.link_instance_generation = remote.link.link_instance_generation;
    memcpy(summary.digest, digest, sizeof(digest));
    CHECK(ucn_v6_capability_summary_encode(
              &summary, payload) == UCN_V6_OK);
    CHECK(ucn_v6_capability_summary_decode(
              payload, UCN_V6_CAPABILITY_HELLO_BYTES,
              &summary_decoded) == UCN_V6_OK);
    CHECK(memcmp(&summary, &summary_decoded, sizeof(summary)) == 0);
    memset(&query, 0, sizeof(query));
    query.requested_generation = summary.capability_generation;
    memcpy(query.known_digest, summary.digest, sizeof(query.known_digest));
    CHECK(ucn_v6_capability_query_encode(
              &query, payload) == UCN_V6_OK);
    CHECK(ucn_v6_capability_query_decode(
              payload, UCN_V6_CAPABILITY_QUERY_BYTES,
              &query_decoded) == UCN_V6_OK);
    CHECK(memcmp(&query, &query_decoded, sizeof(query)) == 0);
    CHECK(ucn_v6_capability_summary_encode(
              &summary, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 7U, 3U, 5U, 2U, 19U,
                       UCN_V6_PROTOCOL_OPCODE_PEER_HELLO,
                       payload, UCN_V6_CAPABILITY_HELLO_BYTES);
    CHECK(ucn_v6_capability_ingest_peer_hello(
              owner, 200U, &opened, &summary,
              &disposition) == UCN_V6_OK);
    CHECK(disposition == UCN_V6_HELLO_MATCHED);
    opened.ingress_link_instance_id = 3U;
    CHECK(ucn_v6_capability_ingest_peer_hello(
              owner, 200U, &opened, &summary,
              &disposition) == UCN_V6_OK);
    CHECK(disposition == UCN_V6_HELLO_QUERY_REQUIRED);
    opened.ingress_link_instance_id = 2U;
    CHECK(ucn_v6_capability_copy_peer(owner, 200U, &remote_principal,
                                      &remote_binding, 5U, 19U,
                                      &cached) == UCN_V6_OK);
    CHECK(cached.ingress_link_id == 2U);
    {
        ucn_v6_cached_peer_capability_t before = cached;
        ucn_v6_principal_t forged = principal(0xF0U);
        ucn_v6_hello_disposition_t unchanged =
            (ucn_v6_hello_disposition_t)UINT32_C(0x5A);
        opened.ingress_peer_session.principal = forged;
        CHECK(ucn_v6_capability_ingest_peer_hello(
                  owner, 200U, &opened, &summary,
                  &unchanged) == UCN_V6_ERR_SECURITY);
        CHECK(unchanged == (ucn_v6_hello_disposition_t)UINT32_C(0x5A));
        CHECK(ucn_v6_capability_copy_peer(
                  owner, 200U, &remote_principal, &remote_binding,
                  5U, 19U, &cached) == UCN_V6_OK);
        CHECK(memcmp(&cached, &before, sizeof(cached)) == 0);
        opened.ingress_peer_session.principal = remote_principal;
    }
    ++summary.capability_generation;
    CHECK(ucn_v6_capability_summary_encode(&summary, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_peer_hello(
              owner, 201U, &opened, &summary,
              &disposition) == UCN_V6_OK);
    CHECK(disposition == UCN_V6_HELLO_QUERY_REQUIRED);

    remote.link.nominal_rate_bps += 1U;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 7U, 3U, 5U, 2U, 19U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 210U, &opened, &remote) == UCN_V6_ERR_REPLAY);
    ++remote.capability_generation;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 211U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(session_event_matches(
              &invalidation, UCN_V6_STACK_INVALIDATE_CAPABILITY,
              2U, 19U, &remote_principal, &remote_binding, 5U));
    CHECK(invalidation.capability_generation == 7U);
    {
        ucn_v6_stack_invalidation_t wrong = invalidation;
        ++wrong.capability_generation;
        CHECK(ucn_v6_capability_invalidation_ack(owner, &wrong) ==
              UCN_V6_ERR_STATE);
        CHECK(ucn_v6_capability_invalidation_peek(owner, &wrong) ==
              UCN_V6_OK);
        CHECK(wrong.capability_generation == 7U);
    }
    CHECK(ucn_v6_capability_invalidation_ack(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, 211U, &view) == UCN_V6_OK);
    CHECK(view.occupied_peer_slots == 1U && view.live_peers == 1U &&
          view.pending_invalidations == 0U);
    CHECK(ucn_v6_capability_copy_peer(owner, 512U, &remote_principal,
                                      &remote_binding, 5U, 19U,
                                      &cached) == UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_capability_expire(owner, 512U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(invalidation.type == UCN_V6_STACK_INVALIDATE_CAPABILITY &&
          invalidation.capability_generation == 8U);
    CHECK(ucn_v6_capability_invalidation_ack(owner, &invalidation) ==
          UCN_V6_OK);
    memset(&invalidation, 0xA5, sizeof(invalidation));
    {
        ucn_v6_stack_invalidation_t before = invalidation;
        CHECK(ucn_v6_capability_invalidation_peek(owner, &invalidation) ==
              UCN_V6_ERR_NOT_FOUND);
        CHECK(memcmp(&invalidation, &before, sizeof(before)) == 0);
    }
    CHECK(ucn_v6_capability_copy_view(owner, 512U, &view) == UCN_V6_OK);
    CHECK(view.occupied_peer_slots == 1U && view.live_peers == 0U);

    /* Expiry revokes liveness but must retain the same-Session generation
     * history.  A stale generation cannot reopen the cache even when carried
     * by a freshly authenticated frame sequence. */
    --remote.capability_generation;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 7U, 3U, 5U, 2U, 19U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 513U, &opened, &remote) ==
          UCN_V6_ERR_REPLAY);
    ++remote.capability_generation;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 514U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(owner, 515U, &remote_principal,
                                      &remote_binding, 5U, 19U,
                                      &cached) == UCN_V6_OK);

    /* The advertised remote Link generation is 9, while Security bound this
     * cache entry to local ingress Link generation 19.  Only retirement of
     * that exact local parent releases the retained generation slot. */
    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = UCN_V6_STACK_INVALIDATE_LINK;
    invalidation.link_id = 2U;
    invalidation.link_generation = 9U;
    CHECK(ucn_v6_capability_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(owner, 515U, &remote_principal,
                                      &remote_binding, 5U, 19U,
                                      &cached) == UCN_V6_OK);
    invalidation.link_generation = 19U;
    CHECK(ucn_v6_capability_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, 514U, &view) == UCN_V6_OK);
    CHECK(view.occupied_peer_slots == 0U && view.live_peers == 0U);
    return 0;
}

static int test_realtime_capability_contract(void)
{
    ucn_v6_capability_record_t value = record(1U, 2U, 256U);
    ucn_v6_capability_record_t decoded;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    value.peer.feature_bits |= UCN_V6_FEATURE_REALTIME;
    value.peer.realtime_mode_bits = UCN_V6_REALTIME_MODE_SYNCED |
                                    UCN_V6_REALTIME_MODE_DEADLINE;
    value.peer.clock_domain_id = 0x1234U;
    value.peer.clock_domain_generation = UINT32_C(0x01020304);
    CHECK(ucn_v6_capability_record_encode(&value, payload) == UCN_V6_OK);
    CHECK(payload[60] == 0U && payload[61] == 6U &&
          payload[62] == 0x12U && payload[63] == 0x34U &&
          payload[64] == 1U && payload[65] == 2U &&
          payload[66] == 3U && payload[67] == 4U);
    CHECK(ucn_v6_capability_record_decode(payload, sizeof(payload),
                                          &decoded) == UCN_V6_OK);
    CHECK(decoded.peer.realtime_mode_bits == value.peer.realtime_mode_bits &&
          decoded.peer.clock_domain_id == value.peer.clock_domain_id &&
          decoded.peer.clock_domain_generation ==
              value.peer.clock_domain_generation);
    value.peer.clock_domain_generation = 0U;
    CHECK(ucn_v6_capability_record_encode(&value, payload) ==
          UCN_V6_ERR_ARGUMENT);
    return 0;
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

static int test_path_budget_and_invalidation(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_record_t hops[2];
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    ucn_v6_principal_t relay = principal(0x31U);
    ucn_v6_principal_t destination = principal(0x30U);
    ucn_v6_binding_key_t relay_binding = { 1U, 6U, 2U };
    ucn_v6_binding_key_t binding = { 1U, 7U, 3U };
    ucn_v6_security_open_result_t opened;
    ucn_v6_capability_peer_ref_t hop_refs[2];
    ucn_v6_path_budget_request_t request;
    ucn_v6_path_budget_request_t downstream_request;
    ucn_v6_path_budget_accumulator_t accumulator;
    ucn_v6_path_budget_accumulator_t downstream_accumulator;
    ucn_v6_path_capability_t path;
    ucn_v6_path_capability_t downstream_path;
    ucn_v6_path_capability_t copied;
    ucn_v6_cached_peer_capability_t cached;
    ucn_v6_stack_invalidation_t invalidation;
    ucn_v6_frame_t frame = budget_frame();
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    size_t overhead = 0U;

    hops[0] = record(4U, 11U, 220U);
    hops[1] = record(9U, 12U, 180U);
    hops[1].peer.max_message_class = UCN_V6_MESSAGE_T2K;
    hops[1].peer.max_rx_window = 8U;
    hops[1].peer.max_concurrent_transfers = 2U;
    hops[0].peer.feature_bits |= UCN_V6_FEATURE_REALTIME;
    hops[0].peer.realtime_mode_bits = UCN_V6_REALTIME_MODE_SYNCED;
    hops[0].peer.clock_domain_id = 7U;
    hops[0].peer.clock_domain_generation = 1U;
    hops[1].peer.feature_bits |= UCN_V6_FEATURE_REALTIME;
    hops[1].peer.realtime_mode_bits = UCN_V6_REALTIME_MODE_SYNCED;
    hops[1].peer.clock_domain_id = 7U;
    hops[1].peer.clock_domain_generation = 1U;
    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 5000U, 5000U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(&hops[0], payload) == UCN_V6_OK);
    opened = peer_open(&relay, 6U, 2U, 4U, 3U, 21U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 9U, &opened, &hops[0]) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(&hops[1], payload) == UCN_V6_OK);
    opened = peer_open(&destination, 7U, 3U, 5U, 2U, 22U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 10U, &opened, &hops[1]) == UCN_V6_OK);
    CHECK(ucn_v6_capability_digest(&hops[1], digest) == UCN_V6_OK);
    memset(&request, 0, sizeof(request));
    request.destination_principal = destination;
    request.destination_binding = binding;
    request.destination_session_generation = 5U;
    request.destination_capability_generation =
        hops[1].capability_generation;
    memcpy(request.destination_capability_digest, digest, sizeof(digest));
    request.destination_realtime_mode_bits =
        hops[1].peer.realtime_mode_bits;
    request.destination_clock_domain_id = hops[1].peer.clock_domain_id;
    request.destination_clock_domain_generation =
        hops[1].peer.clock_domain_generation;
    request.route_generation = 2U;
    request.path_id = 4U;
    request.path_generation = 6U;
    request.deadline_us = 1000U;
    request.fixed_path = true;
    request.path_policy_frame_mtu = 200U;
    request.required_feature_bits = UCN_V6_FEATURE_TRANSFER |
                                    UCN_V6_FEATURE_REALTIME;
    request.required_hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    request.required_e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    request.policy_max_message_class = UCN_V6_MESSAGE_T8K;
    request.policy_max_window = 32U;
    request.policy_max_concurrency = 8U;
    request.frame_contract = &frame;
    request.fragment_header_bytes = 12U;
    memset(hop_refs, 0, sizeof(hop_refs));
    hop_refs[0].principal = relay;
    hop_refs[0].binding = relay_binding;
    hop_refs[0].session_generation = 4U;
    hop_refs[0].ingress_link_id = 3U;
    hop_refs[0].ingress_link_generation = 21U;
    hop_refs[1].principal = destination;
    hop_refs[1].binding = binding;
    hop_refs[1].session_generation = 5U;
    hop_refs[1].ingress_link_id = 2U;
    hop_refs[1].ingress_link_generation = 22U;
    CHECK(ucn_v6_wire_encoded_size(&frame, &overhead) == UCN_V6_OK);
    downstream_request = request;
    downstream_request.route_generation = 1U;
    downstream_request.path_id = 9U;
    downstream_request.path_generation = 1U;
    CHECK(ucn_v6_capability_path_reduce_begin(
              &downstream_request, &downstream_accumulator) == UCN_V6_OK);
    CHECK(ucn_v6_capability_path_reduce_hop(
              owner, 10U, &downstream_accumulator,
              &hop_refs[1]) == UCN_V6_OK);
    CHECK(ucn_v6_capability_path_reduce_finalize(
              &downstream_accumulator, &downstream_path) == UCN_V6_OK);
    CHECK(ucn_v6_capability_path_reduce_begin(
              &request, &accumulator) == UCN_V6_OK);
    CHECK(ucn_v6_capability_path_reduce_downstream(
              &accumulator, &downstream_path, 1000U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_path_reduce_hop(
              owner, 10U, &accumulator, &hop_refs[0]) == UCN_V6_OK);
    CHECK(ucn_v6_capability_path_reduce_finalize(
              &accumulator, &path) == UCN_V6_OK);
    CHECK(path.path_frame_mtu == 172U);
    CHECK(path.payload_budget == 172U - overhead);
    CHECK(path.fragment_data_budget == path.payload_budget - 12U);
    CHECK(path.max_message_class == UCN_V6_MESSAGE_T2K);
    CHECK(path.max_window == 8U && path.max_concurrency == 2U);
    CHECK(path.immutable_for_realtime);
    CHECK(path.timestamp_uncertainty_us == 8U);
    CHECK(path.local_parent_link_id == 3U &&
          path.local_parent_link_generation == 21U &&
          path.local_parent_capability_generation ==
              hops[0].capability_generation &&
          path.hop_count == 2U &&
          path.destination_capability_generation ==
              hops[1].capability_generation &&
          memcmp(path.destination_capability_digest, digest,
                 sizeof(digest)) == 0);
    CHECK(ucn_v6_capability_install_path(owner, 20U, &path) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(
              owner, 20U, &relay, &relay_binding, 4U, 21U,
              &cached) == UCN_V6_OK);
    CHECK(ucn_v6_capability_cached_peer_is_live(&cached, 20U));
    CHECK(ucn_v6_capability_path_is_live(&path, &cached, 20U));
    {
        ucn_v6_cached_peer_capability_t malformed = cached;
        malformed.ingress_link_id = 0U;
        CHECK(!ucn_v6_capability_cached_peer_is_live(&malformed, 20U));
        malformed = cached;
        malformed.digest[0] ^= 1U;
        CHECK(!ucn_v6_capability_cached_peer_is_live(&malformed, 20U));
        CHECK(!ucn_v6_capability_path_is_live(&path, &malformed, 20U));
    }
    {
        ucn_v6_path_capability_t forged = path;
        forged.route_generation =
            UCN_V6_SERIAL_ROTATION_THRESHOLD + UINT32_C(1);
        CHECK(ucn_v6_capability_install_path(owner, 20U, &forged) ==
              UCN_V6_ERR_ARGUMENT);
        forged = path;
        forged.feature_bits |= UINT32_C(0x80000000);
        CHECK(ucn_v6_capability_install_path(owner, 20U, &forged) ==
              UCN_V6_ERR_ARGUMENT);
        forged = path;
        forged.max_window = (uint16_t)(hops[0].peer.max_rx_window + 1U);
        CHECK(ucn_v6_capability_install_path(owner, 20U, &forged) ==
              UCN_V6_ERR_ACCESS);
    }
    CHECK(ucn_v6_capability_copy_path(
              owner, 20U, &destination, &binding, 5U, 2U, 4U, 6U,
              &copied) == UCN_V6_OK);
    CHECK(memcmp(&path, &copied, sizeof(path)) == 0);

    ++hops[0].capability_generation;
    hops[0].link.nominal_rate_bps += 10U;
    CHECK(ucn_v6_capability_record_encode(&hops[0], payload) == UCN_V6_OK);
    opened = peer_open(&relay, 6U, 2U, 4U, 3U, 21U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 30U, &opened, &hops[0]) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(session_event_matches(
              &invalidation, UCN_V6_STACK_INVALIDATE_CAPABILITY,
              3U, 21U, &relay, &relay_binding, 4U));
    CHECK(invalidation.capability_generation == 4U);
    CHECK(ucn_v6_capability_invalidation_ack(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_path(
              owner, 31U, &destination, &binding, 5U, 2U, 4U, 6U,
              &copied) == UCN_V6_ERR_NOT_FOUND);
    request.path_policy_frame_mtu = 1U;
    memset(&copied, 0xA5, sizeof(copied));
    {
        ucn_v6_path_capability_t before = copied;
        CHECK(ucn_v6_capability_path_reduce_begin(
                  &request, &accumulator) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_hop(
                  owner, 31U, &accumulator,
                  &hop_refs[0]) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_finalize(
                  &accumulator, &copied) ==
              UCN_V6_ERR_NO_SPACE);
        CHECK(memcmp(&copied, &before, sizeof(copied)) == 0);
    }
    request.path_policy_frame_mtu = 200U;
    CHECK(ucn_v6_capability_path_reduce_begin(
              &request, &accumulator) == UCN_V6_OK);
    accumulator.hop_count = UCN_V6_PATH_HOP_LIMIT;
    {
        ucn_v6_path_budget_accumulator_t before = accumulator;
        CHECK(ucn_v6_capability_path_reduce_hop(
                  owner, 31U, &accumulator,
                  &hop_refs[0]) == UCN_V6_ERR_EXHAUSTED);
        CHECK(memcmp(&accumulator, &before, sizeof(before)) == 0);
    }
    {
        ucn_v6_capability_peer_ref_t wrong_generation = hop_refs[1];
        ucn_v6_path_budget_accumulator_t before;
        ++wrong_generation.ingress_link_generation;
        CHECK(ucn_v6_capability_path_reduce_begin(
                  &request, &accumulator) == UCN_V6_OK);
        before = accumulator;
        CHECK(ucn_v6_capability_path_reduce_hop(
                  owner, 31U, &accumulator,
                  &wrong_generation) == UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(&accumulator, &before, sizeof(before)) == 0);

        CHECK(ucn_v6_capability_path_reduce_begin(
                  &request, &accumulator) == UCN_V6_OK);
        before = accumulator;
        CHECK(ucn_v6_capability_path_reduce_hop(
                  owner, 5030U, &accumulator,
                  &hop_refs[1]) == UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(&accumulator, &before, sizeof(before)) == 0);
    }
    return 0;
}

static ucn_v6_path_capability_t path_for_peer(
    const ucn_v6_principal_t *principal_value,
    const ucn_v6_binding_key_t *binding,
    uint32_t session_generation,
    const ucn_v6_capability_record_t *peer_record,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    uint16_t path_id,
    uint32_t path_generation,
    uint64_t deadline_us)
{
    ucn_v6_path_capability_t path;
    memset(&path, 0, sizeof(path));
    path.valid = true;
    path.destination_principal = *principal_value;
    path.destination_binding = *binding;
    path.destination_session_generation = session_generation;
    path.destination_capability_generation =
        peer_record->capability_generation;
    (void)ucn_v6_capability_digest(
        peer_record, path.destination_capability_digest);
    path.destination_realtime_mode_bits =
        peer_record->peer.realtime_mode_bits;
    path.destination_clock_domain_id = peer_record->peer.clock_domain_id;
    path.destination_clock_domain_generation =
        peer_record->peer.clock_domain_generation;
    path.local_parent_session.principal = *principal_value;
    path.local_parent_session.binding = *binding;
    path.local_parent_session.session_generation = session_generation;
    path.local_parent_link_id = ingress_link_id;
    path.local_parent_link_generation = ingress_link_generation;
    path.local_parent_capability_generation =
        peer_record->capability_generation;
    (void)ucn_v6_capability_digest(
        peer_record, path.local_parent_capability_digest);
    path.route_generation = 1U;
    path.path_id = path_id;
    path.path_generation = path_generation;
    path.hop_count = 1U;
    path.path_frame_mtu = 120U;
    path.payload_budget = 80U;
    path.fragment_data_budget = 64U;
    path.feature_bits = UCN_V6_FEATURE_IDENTITY | UCN_V6_FEATURE_WIRE |
                        UCN_V6_FEATURE_SECURITY |
                        UCN_V6_FEATURE_CAPABILITY;
    path.hop_suite_bits = peer_record->peer.hop_suite_bits;
    path.e2e_suite_bits = peer_record->peer.e2e_suite_bits;
    path.max_message_class = UCN_V6_MESSAGE_T128;
    path.max_window = 2U;
    path.max_concurrency = 1U;
    path.timestamp_capability_bits =
        peer_record->link.timestamp_capability_bits;
    path.timestamp_uncertainty_us =
        peer_record->link.timestamp_uncertainty_us;
    path.deadline_us = deadline_us;
    return path;
}

static int test_invalidation_domains_expiry_and_capacity(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_record_t remote = record(1U, 2U, 256U);
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    ucn_v6_principal_t remote_principal = principal(0x61U);
    ucn_v6_binding_key_t binding = { 1U, 10U, 1U };
    ucn_v6_security_open_result_t opened;
    ucn_v6_path_capability_t path;
    ucn_v6_cached_peer_capability_t cached;
    ucn_v6_stack_invalidation_t event;
    ucn_v6_capability_view_t view;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    size_t index;

    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 5000U, 5000U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 10U, 1U, 1U, 5U, 2U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 1U, &opened, &remote) == UCN_V6_OK);

    path = path_for_peer(&remote_principal, &binding, 1U, &remote, 5U, 2U,
                         17U, 1U, 100U);
    CHECK(ucn_v6_capability_install_path(owner, 2U, &path) == UCN_V6_OK);
    ++path.path_generation;
    CHECK(ucn_v6_capability_install_path(owner, 3U, &path) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(owner, &event) == UCN_V6_OK);
    CHECK(event.type == UCN_V6_STACK_INVALIDATE_PATH &&
          event.path_id == 17U && event.path_generation == 1U);
    CHECK(ucn_v6_capability_invalidation_ack(owner, &event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_expire(owner, 100U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(owner, &event) == UCN_V6_OK);
    CHECK(session_event_matches(&event, UCN_V6_STACK_INVALIDATE_PATH,
                                5U, 2U, &remote_principal, &binding, 1U));
    CHECK(event.capability_generation == 1U && event.path_id == 17U &&
          event.path_generation == 2U);
    {
        ucn_v6_stack_invalidation_t wrong = event;
        ++wrong.path_generation;
        CHECK(ucn_v6_capability_invalidation_ack(owner, &wrong) ==
              UCN_V6_ERR_STATE);
        CHECK(ucn_v6_capability_invalidation_peek(owner, &wrong) ==
              UCN_V6_OK);
        CHECK(wrong.path_generation == 2U);
    }
    CHECK(ucn_v6_capability_invalidation_ack(owner, &event) == UCN_V6_OK);

    /* A physical Link/Session replacement must be ordered by its owner.  A
     * delayed ADVERTISE cannot infer and overwrite a different parent. */
    opened.ingress_link_instance_id = 6U;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 105U, &opened, &remote) == UCN_V6_ERR_REPLAY);
    memset(&event, 0, sizeof(event));
    event.type = UCN_V6_STACK_INVALIDATE_LINK;
    event.link_id = 5U;
    event.link_generation = 2U;
    CHECK(ucn_v6_capability_apply_invalidation(owner, &event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 105U, &opened, &remote) == UCN_V6_OK);

    remote.link.link_instance_generation = 3U;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    opened.ingress_link_instance_generation = 3U;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 110U, &opened, &remote) == UCN_V6_ERR_REPLAY);
    memset(&event, 0, sizeof(event));
    event.type = UCN_V6_STACK_INVALIDATE_LINK;
    event.link_id = 6U;
    event.link_generation = 2U;
    CHECK(ucn_v6_capability_apply_invalidation(owner, &event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 110U, &opened, &remote) == UCN_V6_OK);

    binding.binding_generation = 2U;
    opened = peer_open(&remote_principal, 10U, 2U, 2U, 6U, 3U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 120U, &opened, &remote) == UCN_V6_ERR_REPLAY);
    memset(&event, 0, sizeof(event));
    event.type = UCN_V6_STACK_INVALIDATE_SESSION;
    event.link_id = 6U;
    event.link_generation = 3U;
    {
        ucn_v6_binding_key_t old_binding = { 1U, 10U, 1U };
        event.session.binding = old_binding;
    }
    event.session.principal = remote_principal;
    event.session.session_generation = 1U;
    CHECK(ucn_v6_capability_apply_invalidation(owner, &event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 120U, &opened, &remote) == UCN_V6_OK);

    for (index = 0U; index < UCN_V6_CAPABILITY_INVALIDATION_DEPTH;
         ++index) {
        remote.capability_generation = (uint32_t)(2U + index);
        CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
        opened.frame.payload = payload;
        CHECK(ucn_v6_capability_ingest_advertise(
                  owner, (uint64_t)(130U + index), &opened,
                  &remote) == UCN_V6_OK);
    }
    CHECK(ucn_v6_capability_copy_view(owner, 200U, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations ==
          UCN_V6_CAPABILITY_INVALIDATION_DEPTH && !view.faulted);
    CHECK(ucn_v6_capability_copy_peer(
              owner, 200U, &remote_principal, &binding, 2U, 3U,
              &cached) == UCN_V6_OK);
    {
        uint32_t retained_generation = cached.record.capability_generation;
        ++remote.capability_generation;
        CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
        opened.frame.payload = payload;
        CHECK(ucn_v6_capability_ingest_advertise(
                  owner, 201U, &opened, &remote) ==
              UCN_V6_ERR_NO_SPACE);
        CHECK(ucn_v6_capability_copy_peer(
                  owner, 202U, &remote_principal, &binding, 2U, 3U,
                  &cached) == UCN_V6_OK);
        CHECK(cached.record.capability_generation == retained_generation);
    }
    CHECK(ucn_v6_capability_copy_view(owner, 202U, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations ==
          UCN_V6_CAPABILITY_INVALIDATION_DEPTH && !view.faulted);
    return 0;
}

static int test_parent_generation_and_late_invalidation(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_record_t remote = record(5U, 2U, 256U);
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    ucn_v6_principal_t remote_principal = principal(0x71U);
    ucn_v6_binding_key_t binding = { 1U, 12U, 1U };
    ucn_v6_security_open_result_t opened;
    ucn_v6_cached_peer_capability_t cached;
    ucn_v6_path_capability_t path;
    ucn_v6_path_capability_t copied_path;
    ucn_v6_stack_invalidation_t expired_event;
    ucn_v6_stack_invalidation_t old_path_event;
    ucn_v6_stack_invalidation_t old_capability_event;
    ucn_v6_stack_invalidation_t old_session_event;
    ucn_v6_capability_view_t view;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];

    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 10U, 10U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 12U, 1U, 1U, 7U, 2U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 1U, &opened, &remote) == UCN_V6_OK);

    CHECK(ucn_v6_capability_expire(owner, 11U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(
              owner, &expired_event) == UCN_V6_OK);
    CHECK(expired_event.type == UCN_V6_STACK_INVALIDATE_CAPABILITY &&
          expired_event.capability_generation == 5U);
    CHECK(ucn_v6_capability_expire(owner, 12U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, 12U, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == 1U &&
          view.occupied_peer_slots == 1U && view.live_peers == 0U);

    /* The retained parent history rejects rollback.  Exact refresh is also
     * held until the already-issued revocation is consumed, so that a late
     * local event cannot revoke newly restored liveness. */
    --remote.capability_generation;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 12U, &opened, &remote) == UCN_V6_ERR_REPLAY);
    ++remote.capability_generation;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 12U, &opened, &remote) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_capability_copy_peer(
              owner, 12U, &remote_principal, &binding, 1U, 2U,
              &cached) == UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_capability_invalidation_ack(
              owner, &expired_event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 13U, &opened, &remote) == UCN_V6_OK);

    /* An exact-next statement has a distinct identity.  A queued invalidation
     * for generation 5 cannot revoke generation 6, even when applied late. */
    ++remote.capability_generation;
    ++remote.link.nominal_rate_bps;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 14U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(
              owner, &old_capability_event) == UCN_V6_OK);
    CHECK(old_capability_event.capability_generation == 5U);
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 15U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_apply_invalidation(
              owner, &old_capability_event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(
              owner, 15U, &remote_principal, &binding, 1U, 2U,
              &cached) == UCN_V6_OK);
    CHECK(cached.record.capability_generation == 6U);
    CHECK(ucn_v6_capability_invalidation_peek(
              owner, &expired_event) == UCN_V6_ERR_NOT_FOUND);

    /* A late path-generation invalidation likewise cannot clear its exact-next
     * replacement. */
    path = path_for_peer(&remote_principal, &binding, 1U, &remote, 7U, 2U,
                         19U, 1U, 24U);
    CHECK(ucn_v6_capability_install_path(owner, 16U, &path) == UCN_V6_OK);
    ++path.path_generation;
    CHECK(ucn_v6_capability_install_path(owner, 17U, &path) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(
              owner, &old_path_event) == UCN_V6_OK);
    CHECK(old_path_event.type == UCN_V6_STACK_INVALIDATE_PATH &&
          old_path_event.path_generation == 1U);
    CHECK(ucn_v6_capability_apply_invalidation(
              owner, &old_path_event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_path(
              owner, 18U, &remote_principal, &binding, 1U, 1U, 19U, 2U,
              &copied_path) == UCN_V6_OK);

    /* A broader Session retirement coalesces queued child events, frees the
     * retained history slot, and remains harmless after a new Session has
     * opened for the same Principal. */
    ++remote.capability_generation;
    ++remote.link.nominal_rate_bps;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 19U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(
              owner, &old_capability_event) == UCN_V6_OK);
    CHECK(old_capability_event.type == UCN_V6_STACK_INVALIDATE_CAPABILITY &&
          old_capability_event.capability_generation == 6U);

    remote.capability_generation = 1U;
    binding.binding_generation = 2U;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 12U, 2U, 2U, 7U, 2U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 20U, &opened, &remote) == UCN_V6_ERR_REPLAY);
    memset(&old_session_event, 0, sizeof(old_session_event));
    old_session_event.type = UCN_V6_STACK_INVALIDATE_SESSION;
    old_session_event.link_id = 7U;
    old_session_event.link_generation = 2U;
    old_session_event.session.binding.realm_id = 1U;
    old_session_event.session.binding.node_address = 12U;
    old_session_event.session.binding.binding_generation = 1U;
    old_session_event.session.principal = remote_principal;
    old_session_event.session.session_generation = 1U;
    CHECK(ucn_v6_capability_apply_invalidation(
              owner, &old_session_event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(
              owner, &expired_event) == UCN_V6_ERR_NOT_FOUND);
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 20U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, 20U, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == 0U &&
          view.occupied_peer_slots == 1U && view.live_peers == 1U);
    CHECK(ucn_v6_capability_apply_invalidation(
              owner, &old_capability_event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(
              owner, 20U, &remote_principal, &binding, 2U, 2U,
              &cached) == UCN_V6_OK);
    CHECK(ucn_v6_capability_apply_invalidation(
              owner, &old_session_event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(
              owner, 20U, &remote_principal, &binding, 2U, 2U,
              &cached) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, 20U, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == 0U &&
          view.occupied_peer_slots == 1U && view.live_peers == 1U);
    return 0;
}

static int test_path_expiry_retains_generation_until_parent_retirement(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_record_t remote = record(3U, 2U, 256U);
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    ucn_v6_principal_t remote_principal = principal(0x81U);
    ucn_v6_binding_key_t binding = { 1U, 15U, 1U };
    ucn_v6_security_open_result_t opened;
    ucn_v6_path_capability_t path;
    ucn_v6_path_capability_t replacement;
    ucn_v6_path_capability_t copied;
    ucn_v6_stack_invalidation_t path_event;
    ucn_v6_stack_invalidation_t session_event;
    ucn_v6_capability_view_t view;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];

    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 1000U, 1000U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 15U, 1U, 1U, 4U, 2U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 1U, &opened, &remote) == UCN_V6_OK);

    path = path_for_peer(&remote_principal, &binding, 1U, &remote, 4U, 2U,
                         23U, 5U, 10U);
    CHECK(ucn_v6_capability_install_path(owner, 2U, &path) == UCN_V6_OK);
    CHECK(ucn_v6_capability_expire(owner, 10U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(owner, &path_event) ==
          UCN_V6_OK);
    CHECK(session_event_matches(&path_event, UCN_V6_STACK_INVALIDATE_PATH,
                                4U, 2U, &remote_principal, &binding, 1U));
    CHECK(path_event.capability_generation == 3U &&
          path_event.path_id == 23U && path_event.path_generation == 5U);
    CHECK(ucn_v6_capability_copy_view(owner, 10U, &view) == UCN_V6_OK);
    CHECK(view.occupied_peer_slots == 1U && view.live_peers == 1U &&
          view.occupied_path_slots == 1U && view.live_paths == 0U);
    CHECK(ucn_v6_capability_copy_path(
              owner, 10U, &remote_principal, &binding, 1U, 1U, 23U, 5U,
              &copied) == UCN_V6_ERR_TIMEOUT);

    replacement = path;
    replacement.path_generation = 4U;
    replacement.deadline_us = 20U;
    CHECK(ucn_v6_capability_install_path(owner, 11U, &replacement) ==
          UCN_V6_ERR_REPLAY);
    replacement.path_generation = 5U;
    CHECK(ucn_v6_capability_install_path(owner, 11U, &replacement) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_capability_invalidation_ack(owner, &path_event) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_install_path(owner, 11U, &replacement) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_path(
              owner, 12U, &remote_principal, &binding, 1U, 1U, 23U, 5U,
              &copied) == UCN_V6_OK);

    ++replacement.path_generation;
    replacement.deadline_us = 30U;
    CHECK(ucn_v6_capability_install_path(owner, 12U, &replacement) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(owner, &path_event) ==
          UCN_V6_OK);
    CHECK(path_event.path_generation == 5U);
    CHECK(ucn_v6_capability_apply_invalidation(owner, &path_event) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_path(
              owner, 13U, &remote_principal, &binding, 1U, 1U, 23U, 6U,
              &copied) == UCN_V6_OK);

    memset(&session_event, 0, sizeof(session_event));
    session_event.type = UCN_V6_STACK_INVALIDATE_SESSION;
    session_event.link_id = 4U;
    session_event.link_generation = 2U;
    session_event.session.binding = binding;
    session_event.session.principal = remote_principal;
    session_event.session.session_generation = 1U;
    CHECK(ucn_v6_capability_apply_invalidation(owner, &session_event) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, 13U, &view) == UCN_V6_OK);
    CHECK(view.occupied_peer_slots == 0U && view.live_peers == 0U &&
          view.occupied_path_slots == 0U && view.live_paths == 0U);
    return 0;
}

static int test_expiry_preflight_accounts_for_parent_coalescing(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_record_t remote = record(1U, 2U, 256U);
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    ucn_v6_principal_t remote_principal = principal(0x91U);
    ucn_v6_binding_key_t binding = { 1U, 16U, 1U };
    ucn_v6_security_open_result_t opened;
    ucn_v6_path_capability_t path;
    ucn_v6_stack_invalidation_t event;
    ucn_v6_capability_view_t view;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    size_t index;

    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 10U, 1000U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 16U, 1U, 1U, 5U, 2U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 1U, &opened, &remote) == UCN_V6_OK);
    path = path_for_peer(&remote_principal, &binding, 1U, &remote, 5U, 2U,
                         29U, 1U, 1000U);
    CHECK(ucn_v6_capability_install_path(owner, 2U, &path) == UCN_V6_OK);
    for (index = 0U; index < UCN_V6_CAPABILITY_INVALIDATION_DEPTH; ++index) {
        ++path.path_generation;
        CHECK(ucn_v6_capability_install_path(owner, 2U, &path) == UCN_V6_OK);
    }
    CHECK(ucn_v6_capability_copy_view(owner, 2U, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations ==
          UCN_V6_CAPABILITY_INVALIDATION_DEPTH);

    /* The new Capability event replaces every queued Path descendant.  A
     * simple free-slot check would incorrectly reject this atomic expiry when
     * the queue starts full. */
    CHECK(ucn_v6_capability_expire(owner, 11U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, 11U, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == 1U &&
          view.occupied_peer_slots == 1U && view.live_peers == 0U &&
          view.occupied_path_slots == 0U && view.live_paths == 0U &&
          view.rejected_capacity == 0U && !view.faulted);
    CHECK(ucn_v6_capability_invalidation_peek(owner, &event) == UCN_V6_OK);
    CHECK(event.type == UCN_V6_STACK_INVALIDATE_CAPABILITY &&
          event.capability_generation == 1U);
    return 0;
}

static ucn_v6_security_open_result_t group_open(
    uint32_t source,
    uint32_t binding_generation,
    uint32_t session_generation,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation)
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.group_discovery_only = true;
    opened.ingress_link_instance_id = ingress_link_id;
    opened.ingress_link_instance_generation = ingress_link_generation;
    opened.frame.frame_type = UCN_V6_FRAME_CONTROL;
    opened.frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO;
    opened.frame.realm_id = 1U;
    opened.frame.source_address = source;
    opened.frame.source_binding_generation = binding_generation;
    opened.frame.session_generation = session_generation;
    opened.frame.group.group_id = 41U;
    opened.frame.group.group_generation = 1U;
    return opened;
}

static int test_group_hint_is_bounded(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    ucn_v6_security_open_result_t first = group_open(10U, 1U, 1U, 3U, 4U);
    ucn_v6_security_open_result_t second = group_open(11U, 1U, 1U, 3U, 4U);
    ucn_v6_security_open_result_t third = group_open(12U, 1U, 1U, 3U, 4U);
    ucn_v6_capability_view_t view;
    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 5000U, 5000U, &owner) == UCN_V6_OK);
    second.frame.group.group_id = 42U;
    third.frame.group.group_id = 43U;
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, 0U, &first) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, 1U, &first) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, 2U, &second) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, 3U, &third) == UCN_V6_ERR_NO_SPACE);
    CHECK(ucn_v6_capability_copy_view(owner, 3U, &view) == UCN_V6_OK);
    CHECK(view.group_hints == 2U && view.occupied_peer_slots == 0U &&
          view.live_peers == 0U);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, UCN_V6_GROUP_HINT_TIMEOUT_US,
              &first) == UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_capability_expire(
              owner, UCN_V6_GROUP_HINT_TIMEOUT_US) == UCN_V6_OK);
    third.frame.group.group_id = first.frame.group.group_id;
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, UCN_V6_GROUP_HINT_TIMEOUT_US,
              &third) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(
              owner, UCN_V6_GROUP_HINT_TIMEOUT_US, &view) == UCN_V6_OK);
    CHECK(view.group_hints == 2U && view.occupied_peer_slots == 0U &&
          view.live_peers == 0U);
    return 0;
}

static int test_peer_table_full_never_evicts(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_record_t remote = record(1U, 2U, 256U);
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    ucn_v6_security_open_result_t opened;
    ucn_v6_capability_view_t view;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    size_t index;
    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 5000U, 5000U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        ucn_v6_principal_t peer = principal((uint8_t)(0x40U + index));
        opened = peer_open(&peer, (uint32_t)(20U + index), 1U, 1U, 1U, 2U,
                           UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                           payload, sizeof(payload));
        CHECK(ucn_v6_capability_ingest_advertise(
                  owner, 10U, &opened, &remote) == UCN_V6_OK);
    }
    CHECK(ucn_v6_capability_copy_view(owner, 10U, &view) == UCN_V6_OK);
    CHECK(view.occupied_peer_slots == UCN_V6_CONFIG_CAPABILITY_PEERS &&
          view.live_peers == UCN_V6_CONFIG_CAPABILITY_PEERS);
    {
        ucn_v6_principal_t overflow = principal(0xE0U);
        opened = peer_open(&overflow, 200U, 1U, 1U, 1U, 2U,
                           UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                           payload, sizeof(payload));
        CHECK(ucn_v6_capability_ingest_advertise(
                  owner, 11U, &opened, &remote) ==
              UCN_V6_ERR_NO_SPACE);
    }
    CHECK(ucn_v6_capability_copy_view(owner, 11U, &view) == UCN_V6_OK);
    CHECK(view.occupied_peer_slots == UCN_V6_CONFIG_CAPABILITY_PEERS &&
          view.live_peers == UCN_V6_CONFIG_CAPABILITY_PEERS);
    CHECK(view.rejected_capacity == 1U);
    return 0;
}

static int test_group_hint_budget_generation_churn_reclaims_idle_slots(void)
{
    ucn_v6_capability_record_t local = record(1U, 1U, 512U);
    ucn_v6_capability_owner_storage_t storage;
    ucn_v6_capability_owner_t *owner = NULL;
    size_t index;
    uint64_t now_us = 0U;

    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 5000U, 5000U, &owner) == UCN_V6_OK);
    for (index = 0U;
         index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS + 2U;
         ++index) {
        ucn_v6_security_open_result_t opened =
            group_open((uint32_t)(100U + index), 1U, 1U,
                       7U, (uint32_t)(index + 1U));
        opened.frame.group.group_id = 50U;
        CHECK(ucn_v6_capability_ingest_group_hello_hint(
                  owner, now_us, &opened) == UCN_V6_OK);
        now_us += UCN_V6_GROUP_HINT_TIMEOUT_US;
        CHECK(ucn_v6_capability_expire(owner, now_us) == UCN_V6_OK);
    }
    return 0;
}

int main(void)
{
    CHECK(test_codec_and_authenticated_cache() == 0);
    CHECK(test_realtime_capability_contract() == 0);
    CHECK(test_path_budget_and_invalidation() == 0);
    CHECK(test_invalidation_domains_expiry_and_capacity() == 0);
    CHECK(test_parent_generation_and_late_invalidation() == 0);
    CHECK(test_path_expiry_retains_generation_until_parent_retirement() == 0);
    CHECK(test_expiry_preflight_accounts_for_parent_coalescing() == 0);
    CHECK(test_group_hint_is_bounded() == 0);
    CHECK(test_peer_table_full_never_evicts() == 0);
    CHECK(test_group_hint_budget_generation_churn_reclaims_idle_slots() == 0);
    puts("ucn v6 capability tests passed");
    return 0;
}
