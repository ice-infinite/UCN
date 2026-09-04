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
    uint16_t opcode,
    const uint8_t *payload,
    uint16_t payload_length)
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.hop_authenticated = true;
    opened.authenticated_principal = *peer;
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
    opened = peer_open(&remote_principal, 7U, 3U, 5U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 100U, 2U, 9U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(owner, 101U, &remote_principal,
                                      &remote_binding, 5U, 9U,
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
    opened = peer_open(&remote_principal, 7U, 3U, 5U,
                       UCN_V6_PROTOCOL_OPCODE_PEER_HELLO,
                       payload, UCN_V6_CAPABILITY_HELLO_BYTES);
    CHECK(ucn_v6_capability_ingest_peer_hello(
              owner, 200U, 2U, 9U, &opened, &summary,
              &disposition) == UCN_V6_OK);
    CHECK(disposition == UCN_V6_HELLO_MATCHED);
    ++summary.capability_generation;
    CHECK(ucn_v6_capability_summary_encode(&summary, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_peer_hello(
              owner, 201U, 2U, 9U, &opened, &summary,
              &disposition) == UCN_V6_OK);
    CHECK(disposition == UCN_V6_HELLO_QUERY_REQUIRED);

    remote.link.nominal_rate_bps += 1U;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened = peer_open(&remote_principal, 7U, 3U, 5U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 210U, 2U, 9U, &opened, &remote) == UCN_V6_ERR_REPLAY);
    ++remote.capability_generation;
    CHECK(ucn_v6_capability_record_encode(&remote, payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 211U, 2U, 9U, &opened, &remote) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.cached_peers == 1U);
    CHECK(ucn_v6_capability_copy_peer(owner, 512U, &remote_principal,
                                      &remote_binding, 5U, 9U,
                                      &cached) == UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_capability_expire(owner, 512U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.cached_peers == 0U);
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
    frame.packet_sequence = 1U;
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
    ucn_v6_principal_t destination = principal(0x30U);
    ucn_v6_binding_key_t binding = { 1U, 7U, 3U };
    ucn_v6_security_open_result_t opened;
    ucn_v6_path_budget_request_t request;
    ucn_v6_path_capability_t path;
    ucn_v6_path_capability_t copied;
    ucn_v6_frame_t frame = budget_frame();
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    size_t overhead = 0U;

    hops[0] = record(4U, 11U, 220U);
    hops[1] = record(9U, 12U, 180U);
    hops[1].peer.max_message_class = UCN_V6_MESSAGE_T2K;
    hops[1].peer.max_rx_window = 8U;
    hops[1].peer.max_concurrent_transfers = 2U;
    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 5000U, 5000U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(&hops[1], payload) == UCN_V6_OK);
    opened = peer_open(&destination, 7U, 3U, 5U,
                       UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                       payload, sizeof(payload));
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 10U, 2U, 12U, &opened, &hops[1]) == UCN_V6_OK);
    CHECK(ucn_v6_capability_digest(&hops[1], digest) == UCN_V6_OK);
    memset(&request, 0, sizeof(request));
    request.hops = hops;
    request.hop_count = 2U;
    request.destination_principal = destination;
    request.destination_binding = binding;
    request.session_generation = 5U;
    request.destination_link_instance_generation = 12U;
    memcpy(request.destination_capability_digest, digest, sizeof(digest));
    request.route_generation = 2U;
    request.path_id = 4U;
    request.path_generation = 6U;
    request.deadline_us = 1000U;
    request.path_policy_frame_mtu = 200U;
    request.required_feature_bits = UCN_V6_FEATURE_TRANSFER;
    request.required_hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    request.required_e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    request.policy_max_message_class = UCN_V6_MESSAGE_T8K;
    request.policy_max_window = 32U;
    request.policy_max_concurrency = 8U;
    request.frame_contract = &frame;
    request.fragment_header_bytes = 12U;
    CHECK(ucn_v6_wire_encoded_size(&frame, &overhead) == UCN_V6_OK);
    CHECK(ucn_v6_capability_derive_path(&request, &path) == UCN_V6_OK);
    CHECK(path.path_frame_mtu == 172U);
    CHECK(path.payload_budget == 172U - overhead);
    CHECK(path.fragment_data_budget == path.payload_budget - 12U);
    CHECK(path.max_message_class == UCN_V6_MESSAGE_T2K);
    CHECK(path.max_window == 8U && path.max_concurrency == 2U);
    CHECK(path.timestamp_uncertainty_us == 8U);
    CHECK(ucn_v6_capability_install_path(owner, 20U, &path) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_path(
              owner, 20U, &destination, &binding, 5U, 2U, 4U, 6U,
              &copied) == UCN_V6_OK);

    ++hops[1].capability_generation;
    hops[1].link.nominal_rate_bps += 10U;
    CHECK(ucn_v6_capability_record_encode(&hops[1], payload) == UCN_V6_OK);
    opened.frame.payload = payload;
    CHECK(ucn_v6_capability_ingest_advertise(
              owner, 30U, 2U, 12U, &opened, &hops[1]) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_path(
              owner, 31U, &destination, &binding, 5U, 2U, 4U, 6U,
              &copied) == UCN_V6_ERR_NOT_FOUND);
    request.path_policy_frame_mtu = 1U;
    memset(&copied, 0xA5, sizeof(copied));
    {
        ucn_v6_path_capability_t before = copied;
        CHECK(ucn_v6_capability_derive_path(&request, &copied) ==
              UCN_V6_ERR_NO_SPACE);
        CHECK(memcmp(&copied, &before, sizeof(copied)) == 0);
    }
    return 0;
}

static ucn_v6_security_open_result_t group_open(uint32_t source,
                                                 uint32_t binding_generation,
                                                 uint32_t session_generation)
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.group_discovery_only = true;
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
    ucn_v6_security_open_result_t first = group_open(10U, 1U, 1U);
    ucn_v6_security_open_result_t second = group_open(11U, 1U, 1U);
    ucn_v6_security_open_result_t third = group_open(12U, 1U, 1U);
    ucn_v6_capability_view_t view;
    CHECK(ucn_v6_capability_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &local, 5000U, 5000U, &owner) == UCN_V6_OK);
    second.frame.group.group_id = 42U;
    third.frame.group.group_id = 43U;
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, 0U, 3U, 4U, &first) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, 1U, 3U, 4U, &first) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, 2U, 3U, 4U, &second) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, 3U, 3U, 4U, &third) == UCN_V6_ERR_NO_SPACE);
    CHECK(ucn_v6_capability_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.group_hints == 2U && view.cached_peers == 0U);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, UCN_V6_GROUP_HINT_TIMEOUT_US, 3U, 4U,
              &first) == UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_capability_expire(
              owner, UCN_V6_GROUP_HINT_TIMEOUT_US) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_group_hello_hint(
              owner, UCN_V6_GROUP_HINT_TIMEOUT_US, 3U, 4U,
              &third) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.group_hints == 2U && view.cached_peers == 0U);
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
        opened = peer_open(&peer, (uint32_t)(20U + index), 1U, 1U,
                           UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                           payload, sizeof(payload));
        CHECK(ucn_v6_capability_ingest_advertise(
                  owner, 10U, 1U, 2U, &opened, &remote) == UCN_V6_OK);
    }
    CHECK(ucn_v6_capability_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.cached_peers == UCN_V6_CONFIG_CAPABILITY_PEERS);
    {
        ucn_v6_principal_t overflow = principal(0xE0U);
        opened = peer_open(&overflow, 200U, 1U, 1U,
                           UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                           payload, sizeof(payload));
        CHECK(ucn_v6_capability_ingest_advertise(
                  owner, 11U, 1U, 2U, &opened, &remote) ==
              UCN_V6_ERR_NO_SPACE);
    }
    CHECK(ucn_v6_capability_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.cached_peers == UCN_V6_CONFIG_CAPABILITY_PEERS);
    CHECK(view.rejected_capacity == 1U);
    return 0;
}

int main(void)
{
    CHECK(test_codec_and_authenticated_cache() == 0);
    CHECK(test_path_budget_and_invalidation() == 0);
    CHECK(test_group_hint_is_bounded() == 0);
    CHECK(test_peer_table_full_never_evicts() == 0);
    puts("ucn v6 capability tests passed");
    return 0;
}
