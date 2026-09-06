#include "ucn/v6/ucn_v6_transfer.h"

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

#if UCN_V6_TRANSFER_MAX_MESSAGE_BYTES >= 512U
#define TEST_SELECTIVE_BYTES 512U
#define TEST_SELECTIVE_BUDGET 100U
#define TEST_SELECTIVE_CLASS UCN_V6_MESSAGE_T512
#else
#define TEST_SELECTIVE_BYTES 256U
#define TEST_SELECTIVE_BUDGET 50U
#define TEST_SELECTIVE_CLASS UCN_V6_MESSAGE_T256
#endif
#define TEST_SELECTIVE_FINAL_OFFSET \
    ((TEST_SELECTIVE_BYTES / TEST_SELECTIVE_BUDGET) * TEST_SELECTIVE_BUDGET)
#define TEST_SELECTIVE_FINAL_BYTES \
    (TEST_SELECTIVE_BYTES - TEST_SELECTIVE_FINAL_OFFSET)

typedef struct transfer_fixture {
    ucn_v6_capability_owner_storage_t capability_storage;
    ucn_v6_capability_owner_t *capability_owner;
    ucn_v6_route_owner_storage_t route_storage;
    ucn_v6_route_owner_t *route_owner;
    ucn_v6_transfer_owner_storage_t tx_storage;
    ucn_v6_transfer_owner_storage_t rx_storage;
    ucn_v6_transfer_owner_t *tx;
    ucn_v6_transfer_owner_t *rx;
    ucn_v6_route_domain_t domain;
    ucn_v6_capability_record_t destination_capability;
    ucn_v6_path_capability_t path;
    uint8_t payload[UCN_V6_TRANSFER_MAX_MESSAGE_BYTES];
} transfer_fixture_t;

static ucn_v6_principal_t principal(uint8_t seed)
{
    ucn_v6_principal_t value;
    size_t index;
    for (index = 0U; index < sizeof(value.bytes); ++index) {
        value.bytes[index] = (uint8_t)(seed + index);
    }
    return value;
}

static ucn_v6_session_key_t session(uint8_t seed,
                                    uint32_t address,
                                    uint32_t binding_generation,
                                    uint32_t session_generation)
{
    ucn_v6_session_key_t value;
    memset(&value, 0, sizeof(value));
    value.principal = principal(seed);
    value.binding.realm_id = 1U;
    value.binding.node_address = address;
    value.binding.binding_generation = binding_generation;
    value.session_generation = session_generation;
    return value;
}

static ucn_v6_capability_record_t capability_record(
    uint32_t capability_generation,
    uint32_t link_generation)
{
    ucn_v6_capability_record_t value;
    memset(&value, 0, sizeof(value));
    value.capability_generation = capability_generation;
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

static ucn_v6_security_open_result_t capability_open(
    const transfer_fixture_t *fixture,
    const uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES])
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.hop_authenticated = true;
    opened.authenticated_principal = fixture->domain.destination_principal;
    opened.ingress_peer_session.principal =
        fixture->domain.destination_principal;
    opened.ingress_peer_session.binding =
        fixture->domain.destination_binding;
    opened.ingress_peer_session.session_generation = 7U;
    opened.ingress_link_instance_id = 3U;
    opened.ingress_link_instance_generation = 8U;
    opened.frame.frame_type = UCN_V6_FRAME_CONTROL;
    opened.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_PROTOCOL_CONTEXT;
    opened.frame.realm_id = fixture->domain.destination_binding.realm_id;
    opened.frame.source_address =
        fixture->domain.destination_binding.node_address;
    opened.frame.source_binding_generation =
        fixture->domain.destination_binding.binding_generation;
    opened.frame.session_generation = 7U;
    opened.frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE;
    opened.frame.payload = payload;
    opened.frame.payload_length = UCN_V6_CAPABILITY_RECORD_BYTES;
    return opened;
}

static ucn_v6_route_path_ref_t route_ref(
    const transfer_fixture_t *fixture,
    const ucn_v6_path_capability_t *path)
{
    ucn_v6_route_path_ref_t reference;
    memset(&reference, 0, sizeof(reference));
    reference.domain = fixture->domain;
    reference.route_generation = path->route_generation;
    reference.path_id = path->path_id;
    reference.path_generation = path->path_generation;
    return reference;
}

static ucn_v6_stack_invalidation_t session_invalidation(
    const ucn_v6_session_key_t *session_key,
    uint16_t link_id,
    uint32_t link_generation)
{
    ucn_v6_stack_invalidation_t event;
    memset(&event, 0, sizeof(event));
    event.type = UCN_V6_STACK_INVALIDATE_SESSION;
    event.link_id = link_id;
    event.link_generation = link_generation;
    event.session = *session_key;
    return event;
}

static int activate_exact_route_path(transfer_fixture_t *fixture,
                                     uint64_t now_us,
                                     uint64_t candidate_transaction_id,
                                     const ucn_v6_route_path_t *route_path)
{
    ucn_v6_route_activation_t activation;
    CHECK(route_path != NULL);
    CHECK(ucn_v6_route_candidate_begin(
              fixture->route_owner, now_us, candidate_transaction_id,
              &fixture->domain, route_path->capability.route_generation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_add_path(
              fixture->route_owner, now_us, candidate_transaction_id,
              &fixture->domain, route_path) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_probe(
              fixture->route_owner, now_us, candidate_transaction_id,
              &fixture->domain, route_path->path_id,
              route_path->path_generation) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              fixture->route_owner, now_us, candidate_transaction_id,
              &fixture->domain, &activation) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_activation_send(
              fixture->route_owner, now_us, candidate_transaction_id,
              &fixture->domain, true) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_commit_ack(
              fixture->route_owner, now_us, &activation) == UCN_V6_OK);
    return 0;
}

static int activate_fixture_path(transfer_fixture_t *fixture,
                                 uint64_t now_us,
                                 uint64_t candidate_transaction_id)
{
    ucn_v6_route_path_t route_path;
    memset(&route_path, 0, sizeof(route_path));
    route_path.path_id = fixture->path.path_id;
    route_path.path_generation = fixture->path.path_generation;
    route_path.next_hop.principal =
        fixture->domain.destination_principal;
    route_path.next_hop.binding = fixture->domain.destination_binding;
    route_path.next_hop = fixture->path.local_parent_session;
    route_path.egress_link_id = fixture->path.local_parent_link_id;
    route_path.egress_link_generation =
        fixture->path.local_parent_link_generation;
    route_path.next_hop_capability_generation =
        fixture->path.local_parent_capability_generation;
    route_path.hop_count = fixture->path.hop_count;
    route_path.priority = 1U;
    route_path.weight = 1U;
    route_path.available = true;
    route_path.capability = fixture->path;
    return activate_exact_route_path(fixture, now_us,
                                     candidate_transaction_id, &route_path);
}

static int install_and_activate_path(transfer_fixture_t *fixture,
                                     uint64_t now_us,
                                     const ucn_v6_path_capability_t *path)
{
    CHECK(path != NULL);
    CHECK(ucn_v6_capability_install_path(
              fixture->capability_owner, now_us, path) == UCN_V6_OK);
    fixture->path = *path;
    CHECK(activate_fixture_path(fixture, now_us,
                                path->route_generation) == 0);
    return 0;
}

static int fixture_init(transfer_fixture_t *fixture)
{
    ucn_v6_capability_record_t local;
    ucn_v6_security_open_result_t opened;
    uint8_t encoded[UCN_V6_CAPABILITY_RECORD_BYTES];
    ucn_v6_transfer_owner_t *rejected_owner = NULL;
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    fixture->domain.origin_principal = principal(0x10U);
    fixture->domain.origin_binding.realm_id = 1U;
    fixture->domain.origin_binding.node_address = 10U;
    fixture->domain.origin_binding.binding_generation = 2U;
    fixture->domain.origin_session_generation = 3U;
    fixture->domain.destination_principal = principal(0x40U);
    fixture->domain.destination_binding.realm_id = 1U;
    fixture->domain.destination_binding.node_address = 40U;
    fixture->domain.destination_binding.binding_generation = 5U;
    fixture->domain.destination_session_generation = 7U;
    local = capability_record(1U, 1U);
    fixture->destination_capability = capability_record(1U, 8U);
    CHECK(ucn_v6_capability_owner_init_in_place(
              fixture->capability_storage.bytes,
              sizeof(fixture->capability_storage),
              ucn_v6_compiled_manifest(), &local, 500000U, 500000U,
              &fixture->capability_owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_record_encode(
              &fixture->destination_capability, encoded) == UCN_V6_OK);
    opened = capability_open(fixture, encoded);
    CHECK(ucn_v6_capability_ingest_advertise(
              fixture->capability_owner, 0U, &opened,
              &fixture->destination_capability) == UCN_V6_OK);
    fixture->path.valid = true;
    fixture->path.destination_principal =
        fixture->domain.destination_principal;
    fixture->path.destination_binding = fixture->domain.destination_binding;
    fixture->path.destination_session_generation = 7U;
    fixture->path.destination_capability_generation =
        fixture->destination_capability.capability_generation;
    fixture->path.destination_realtime_mode_bits =
        fixture->destination_capability.peer.realtime_mode_bits;
    fixture->path.destination_clock_domain_id =
        fixture->destination_capability.peer.clock_domain_id;
    fixture->path.destination_clock_domain_generation =
        fixture->destination_capability.peer.clock_domain_generation;
    fixture->path.local_parent_session.principal =
        fixture->domain.destination_principal;
    fixture->path.local_parent_session.binding =
        fixture->domain.destination_binding;
    fixture->path.local_parent_session.session_generation = 7U;
    fixture->path.local_parent_link_id = 3U;
    fixture->path.local_parent_link_generation = 8U;
    fixture->path.local_parent_capability_generation =
        fixture->destination_capability.capability_generation;
    fixture->path.route_generation = 1U;
    fixture->path.path_id = 4U;
    fixture->path.path_generation = 6U;
    fixture->path.path_frame_mtu = 200U;
    fixture->path.payload_budget = 180U;
    fixture->path.fragment_data_budget = 100U;
    fixture->path.feature_bits = UCN_V6_FEATURE_TRANSFER;
    fixture->path.hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    fixture->path.e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    fixture->path.max_message_class =
        (ucn_v6_message_class_t)UCN_V6_CONFIG_TRANSFER_MAX_CLASS;
    fixture->path.max_window = UCN_V6_CONFIG_TRANSFER_WINDOW;
    fixture->path.max_concurrency = UCN_V6_CONFIG_TRANSFER_RX_SLOTS;
    fixture->path.deadline_us = 100000U;
    CHECK(ucn_v6_capability_digest(
              &fixture->destination_capability,
              fixture->path.destination_capability_digest) == UCN_V6_OK);
    memcpy(fixture->path.local_parent_capability_digest,
           fixture->path.destination_capability_digest,
           sizeof(fixture->path.local_parent_capability_digest));
    fixture->path.hop_count = 1U;
    CHECK(ucn_v6_capability_install_path(
              fixture->capability_owner, 0U, &fixture->path) == UCN_V6_OK);
    CHECK(ucn_v6_route_owner_init_in_place(
              fixture->route_storage.bytes, sizeof(fixture->route_storage),
              ucn_v6_compiled_manifest(), fixture->capability_owner,
              1000U, 100U, 4U, 100U, 100U,
              &fixture->route_owner) == UCN_V6_OK);
    CHECK(activate_fixture_path(fixture, 0U, 1U) == 0);
    for (index = 0U; index < sizeof(fixture->payload); ++index) {
        fixture->payload[index] = (uint8_t)(index * 29U + 7U);
    }
    CHECK(ucn_v6_transfer_owner_init_in_place(
              fixture->tx_storage.bytes, sizeof(fixture->tx_storage),
              ucn_v6_compiled_manifest(), NULL, 100U, 4U, 1000U, 2000U,
              &rejected_owner) == UCN_V6_OK);
    CHECK(rejected_owner != NULL);
    CHECK(ucn_v6_transfer_owner_init_in_place(
              fixture->tx_storage.bytes, sizeof(fixture->tx_storage),
              ucn_v6_compiled_manifest(), fixture->route_owner,
              100U, 4U, 1000U, 2000U,
              &fixture->tx) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_owner_init_in_place(
              fixture->rx_storage.bytes, sizeof(fixture->rx_storage),
              ucn_v6_compiled_manifest(), NULL,
              100U, 4U, 1000U, 2000U,
              &fixture->rx) == UCN_V6_OK);
    return 0;
}

static ucn_v6_transfer_send_request_t request_for(
    const transfer_fixture_t *fixture,
    uint64_t message_id,
    uint16_t payload_length,
    uint16_t budget,
    uint16_t window)
{
    ucn_v6_transfer_send_request_t request;
    memset(&request, 0, sizeof(request));
    request.route_ref = route_ref(fixture, &fixture->path);
    request.message.traffic_class = UCN_V6_TRAFFIC_Q2;
    request.message.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    request.message.interaction_role = UCN_V6_INTERACTION_REQUEST;
    request.message.source_endpoint = 10U;
    request.message.destination_endpoint = 20U;
    request.message.operation_id = message_id;
    request.message.payload_length = payload_length;
    request.message_class = payload_length <= 64U ? UCN_V6_MESSAGE_T64 :
                            payload_length <= 128U ? UCN_V6_MESSAGE_T128 :
                            payload_length <= 256U ? UCN_V6_MESSAGE_T256 :
                            payload_length <= 512U ? UCN_V6_MESSAGE_T512 :
                            UCN_V6_MESSAGE_T8K;
    request.message_id = message_id;
    request.buffer_token = message_id + 100U;
    request.payload = fixture->payload;
    request.payload_length = payload_length;
    request.fragment_data_budget = budget;
    request.window_size = window;
    return request;
}

static ucn_v6_security_open_result_t opened_fragment(
    const transfer_fixture_t *fixture,
    uint64_t operation_id,
    const uint8_t *encoded,
    size_t encoded_length)
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.authenticated_principal = fixture->domain.origin_principal;
    opened.ingress_peer_session = session(0x20U, 20U, 1U, 2U);
    opened.hop_authenticated = true;
    opened.endpoint_authorized = true;
    opened.frame.frame_type = UCN_V6_FRAME_TRANSFER;
    opened.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_E2E_CONTEXT |
                         UCN_V6_FLAG_PROTOCOL_CONTEXT |
                         UCN_V6_FLAG_MESSAGE_CONTEXT |
                         UCN_V6_FLAG_ROUTE_CONTEXT |
                         UCN_V6_FLAG_PATH_CONTEXT;
    opened.frame.traffic_class = UCN_V6_TRAFFIC_Q2;
    opened.frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    opened.frame.realm_id = fixture->domain.origin_binding.realm_id;
    opened.frame.source_address = fixture->domain.origin_binding.node_address;
    opened.frame.source_binding_generation =
        fixture->domain.origin_binding.binding_generation;
    opened.frame.session_generation =
        fixture->domain.origin_session_generation;
    opened.frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_TRANSFER_FRAGMENT;
    opened.frame.message.source_endpoint = 10U;
    opened.frame.message.destination_endpoint = 20U;
    opened.frame.message.interaction_role = UCN_V6_INTERACTION_REQUEST;
    opened.frame.message.operation_id = operation_id;
    opened.frame.route_generation = fixture->path.route_generation;
    opened.frame.path.path_id = fixture->path.path_id;
    opened.frame.path.path_generation = fixture->path.path_generation;
    opened.frame.payload = encoded;
    opened.frame.payload_length = (uint16_t)encoded_length;
    return opened;
}

static ucn_v6_security_open_result_t opened_sack(
    const transfer_fixture_t *fixture,
    uint64_t operation_id,
    const uint8_t encoded[UCN_V6_TRANSFER_SACK_BYTES])
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.authenticated_principal = fixture->domain.destination_principal;
    opened.ingress_peer_session = session(0x40U, 40U, 5U, 7U);
    opened.hop_authenticated = true;
    opened.endpoint_authorized = true;
    opened.frame.frame_type = UCN_V6_FRAME_TRANSFER;
    opened.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_E2E_CONTEXT |
                         UCN_V6_FLAG_PROTOCOL_CONTEXT |
                         UCN_V6_FLAG_MESSAGE_CONTEXT |
                         UCN_V6_FLAG_ROUTE_CONTEXT |
                         UCN_V6_FLAG_PATH_CONTEXT;
    opened.frame.realm_id = fixture->domain.destination_binding.realm_id;
    opened.frame.source_address =
        fixture->domain.destination_binding.node_address;
    opened.frame.source_binding_generation =
        fixture->domain.destination_binding.binding_generation;
    opened.frame.session_generation =
        fixture->path.destination_session_generation;
    opened.frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TRANSFER_SACK;
    opened.frame.message.operation_id = operation_id;
    opened.frame.route_generation = fixture->path.route_generation;
    opened.frame.path.path_id = fixture->path.path_id;
    opened.frame.path.path_generation = fixture->path.path_generation;
    opened.frame.payload = encoded;
    opened.frame.payload_length = UCN_V6_TRANSFER_SACK_BYTES;
    return opened;
}

static int test_codec_and_message_classes(void)
{
    static const uint16_t sizes[9] = {
        32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U, 8192U
    };
    uint8_t data[UCN_V6_TRANSFER_MAX_MESSAGE_BYTES];
    uint8_t encoded[UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES +
                    UCN_V6_TRANSFER_MAX_MESSAGE_BYTES];
    uint8_t before[sizeof(encoded)];
    size_t index;
    for (index = 0U; index < sizeof(data); ++index) {
        data[index] = (uint8_t)(index ^ 0xA5U);
    }
    for (index = 0U; index <= UCN_V6_CONFIG_TRANSFER_MAX_CLASS; ++index) {
        ucn_v6_transfer_fragment_t fragment;
        ucn_v6_transfer_fragment_t decoded;
        size_t encoded_length = 0U;
        memset(&fragment, 0, sizeof(fragment));
        fragment.message_class = (ucn_v6_message_class_t)index;
        fragment.message_id = index + 1U;
        fragment.total_length = sizes[index];
        fragment.fragment_count = 1U;
        fragment.fragment_data_budget = sizes[index];
        fragment.data_length = sizes[index];
        fragment.message_crc32c = ucn_v6_crc32c(data, sizes[index]);
        fragment.data = data;
        CHECK(ucn_v6_message_class_bytes(fragment.message_class) ==
              sizes[index]);
        CHECK(ucn_v6_transfer_fragment_encode(
                  &fragment, encoded, sizeof(encoded), &encoded_length) ==
              UCN_V6_OK);
        CHECK(encoded_length == UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES +
                                    sizes[index]);
        CHECK(ucn_v6_transfer_fragment_decode(encoded, encoded_length,
                                               &decoded) == UCN_V6_OK);
        CHECK(decoded.message_class == fragment.message_class &&
              decoded.message_id == fragment.message_id &&
              decoded.total_length == fragment.total_length &&
              memcmp(decoded.data, data, sizes[index]) == 0);
    }
    memset(encoded, 0x6A, sizeof(encoded));
    memcpy(before, encoded, sizeof(encoded));
    {
        ucn_v6_transfer_fragment_t invalid;
        size_t output_length = 77U;
        memset(&invalid, 0, sizeof(invalid));
        invalid.message_class = UCN_V6_MESSAGE_T32;
        invalid.message_id = 1U;
        invalid.total_length = 33U;
        invalid.fragment_count = 1U;
        invalid.fragment_data_budget = 33U;
        invalid.data_length = 33U;
        invalid.data = data;
        CHECK(ucn_v6_transfer_fragment_encode(
                  &invalid, encoded, sizeof(encoded), &output_length) ==
              UCN_V6_ERR_ARGUMENT);
        CHECK(output_length == 77U &&
              memcmp(encoded, before, sizeof(encoded)) == 0);
    }
    {
        ucn_v6_transfer_result_t result;
        ucn_v6_transfer_result_t decoded;
        uint8_t result_wire[UCN_V6_TRANSFER_RESULT_BYTES];
        memset(&result, 0, sizeof(result));
        result.message_id = 9U;
        result.operation_id = 9U;
        result.result_code = -17;
        CHECK(ucn_v6_transfer_result_encode(&result, result_wire) ==
              UCN_V6_OK);
        CHECK(ucn_v6_transfer_result_decode(
                  result_wire, sizeof(result_wire), &decoded) == UCN_V6_OK);
        CHECK(decoded.message_id == 9U && decoded.operation_id == 9U &&
              decoded.result_code == -17);
        result.operation_id = 10U;
        CHECK(ucn_v6_transfer_result_encode(&result, result_wire) ==
              UCN_V6_ERR_ARGUMENT);
    }
    return 0;
}

static int deliver_selected(transfer_fixture_t *fixture,
                            uint64_t now_us,
                            uint64_t message_id,
                            uint16_t expected_index,
                            bool submit,
                            bool deliver,
                            ucn_v6_transfer_rx_result_t *rx_result)
{
    ucn_v6_transfer_fragment_t fragment;
    uint8_t encoded[256];
    size_t encoded_length = 0U;
    ucn_v6_security_open_result_t opened;
    CHECK(ucn_v6_transfer_next_fragment(fixture->tx, now_us, message_id,
                                        &fragment) == UCN_V6_OK);
    CHECK(fragment.fragment_index == expected_index);
    CHECK(ucn_v6_transfer_fragment_encode(
              &fragment, encoded, sizeof(encoded), &encoded_length) ==
          UCN_V6_OK);
    CHECK(ucn_v6_transfer_record_fragment_submit(
              fixture->tx, now_us, message_id, fragment.fragment_index,
              submit) == UCN_V6_OK);
    if (!submit || !deliver) {
        return 0;
    }
    opened = opened_fragment(fixture, message_id, encoded, encoded_length);
    CHECK(ucn_v6_transfer_receive_fragment(
              fixture->rx, now_us, &opened, rx_result) == UCN_V6_OK);
    return 0;
}

static int apply_sack(transfer_fixture_t *fixture,
                      uint64_t operation_id,
                      const ucn_v6_transfer_sack_t *sack)
{
    uint8_t encoded[UCN_V6_TRANSFER_SACK_BYTES];
    ucn_v6_security_open_result_t opened;
    CHECK(ucn_v6_transfer_sack_encode(sack, encoded) == UCN_V6_OK);
    opened = opened_sack(fixture, operation_id, encoded);
    CHECK(ucn_v6_transfer_apply_sack(fixture->tx, 0U, &opened) == UCN_V6_OK);
    return 0;
}

static int test_selective_repeat_and_recent(void)
{
    transfer_fixture_t fixture;
    ucn_v6_transfer_send_request_t request;
    ucn_v6_transfer_rx_result_t received[6];
    ucn_v6_transfer_tx_view_t tx_view;
    ucn_v6_transfer_completed_t completed;
    ucn_v6_transfer_stats_t stats;
    ucn_v6_session_key_t origin;
    uint8_t output[UCN_V6_TRANSFER_MAX_MESSAGE_BYTES];
    uint64_t token = 0U;
    CHECK(fixture_init(&fixture) == 0);
    request = request_for(&fixture, 1U, TEST_SELECTIVE_BYTES,
                          TEST_SELECTIVE_BUDGET, 4U);
    CHECK(ucn_v6_transfer_send_begin(fixture.rx, 0U, &request) ==
          UCN_V6_ERR_CONFIG);
    CHECK(ucn_v6_transfer_copy_tx(fixture.rx, 1U, &tx_view) ==
          UCN_V6_ERR_NOT_FOUND);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 0U, &request) == UCN_V6_OK);

    /* Four fragments enter the pipe before any SACK: this is not stop/wait. */
    CHECK(deliver_selected(&fixture, 0U, 1U, 0U, true, true,
                           &received[0]) == 0);
    CHECK(deliver_selected(&fixture, 1U, 1U, 1U, true, false,
                           &received[1]) == 0);
    CHECK(deliver_selected(&fixture, 2U, 1U, 2U, true, true,
                           &received[2]) == 0);
    CHECK(deliver_selected(&fixture, 3U, 1U, 3U, true, true,
                           &received[3]) == 0);
    CHECK(apply_sack(&fixture, 1U, &received[3].sack) == 0);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &tx_view) == UCN_V6_OK);
    CHECK(tx_view.cumulative_base == 1U);

    CHECK(deliver_selected(&fixture, 4U, 1U, 4U, true, true,
                           &received[4]) == 0);
    CHECK(apply_sack(&fixture, 1U, &received[4].sack) == 0);
    CHECK(deliver_selected(&fixture, 101U, 1U, 1U, true, true,
                           &received[1]) == 0);
    CHECK(apply_sack(&fixture, 1U, &received[1].sack) == 0);
    CHECK(deliver_selected(&fixture, 102U, 1U, 5U, true, true,
                           &received[5]) == 0);
    CHECK(received[5].complete);
    CHECK(apply_sack(&fixture, 1U, &received[5].sack) == 0);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &tx_view) == UCN_V6_OK);
    CHECK(tx_view.phase == UCN_V6_TRANSFER_TX_REASSEMBLED &&
          tx_view.cumulative_base == 6U);
    origin = session(0x10U, 10U, 2U, 3U);
    /* Reassembly timeout only owns incomplete slots.  Once complete, the
     * buffer and its replay identity remain owned by the application until
     * explicit retirement; a timer must not silently discard either. */
    CHECK(ucn_v6_transfer_expire(fixture.rx, UINT64_C(10000)) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_copy_completed(
              fixture.rx, &origin, 1U, 1U, output, sizeof(output),
              &completed) == UCN_V6_OK);
    CHECK(completed.payload_length == TEST_SELECTIVE_BYTES &&
          completed.message.traffic_class == UCN_V6_TRAFFIC_Q2 &&
          completed.message.delivery_guarantee ==
              UCN_V6_DELIVERY_RELIABLE &&
          completed.message.interaction_role == UCN_V6_INTERACTION_REQUEST &&
          completed.message.source_endpoint == 10U &&
          completed.message.destination_endpoint == 20U &&
          completed.message.operation_id == 1U &&
          completed.message.payload_length == TEST_SELECTIVE_BYTES &&
          memcmp(output, fixture.payload, TEST_SELECTIVE_BYTES) == 0);

    /* Final SACK loss is recoverable both before and after application retire. */
    {
        ucn_v6_transfer_fragment_t final_fragment;
        uint8_t encoded[256];
        size_t encoded_length = 0U;
        ucn_v6_security_open_result_t opened;
        memset(&final_fragment, 0, sizeof(final_fragment));
        final_fragment.message_class = TEST_SELECTIVE_CLASS;
        final_fragment.message_id = 1U;
        final_fragment.total_length = TEST_SELECTIVE_BYTES;
        final_fragment.fragment_index = 5U;
        final_fragment.fragment_count = 6U;
        final_fragment.fragment_data_budget = TEST_SELECTIVE_BUDGET;
        final_fragment.data_length = TEST_SELECTIVE_FINAL_BYTES;
        final_fragment.message_crc32c = completed.message_crc32c;
        final_fragment.data = &fixture.payload[TEST_SELECTIVE_FINAL_OFFSET];
        CHECK(ucn_v6_transfer_fragment_encode(
                  &final_fragment, encoded, sizeof(encoded),
                  &encoded_length) == UCN_V6_OK);
        opened = opened_fragment(&fixture, 1U, encoded, encoded_length);
        {
            ucn_v6_security_open_result_t changed = opened;
            ucn_v6_transfer_rx_result_t sentinel;
            ucn_v6_transfer_rx_result_t before;
            memset(&sentinel, 0xA7, sizeof(sentinel));
            before = sentinel;
            changed.frame.message.destination_endpoint = 21U;
            CHECK(ucn_v6_transfer_receive_fragment(
                      fixture.rx, 9U, &changed, &sentinel) ==
                  UCN_V6_ERR_REPLAY);
            CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
            changed = opened;
            changed.frame.message.source_endpoint = 11U;
            CHECK(ucn_v6_transfer_receive_fragment(
                      fixture.rx, 9U, &changed, &sentinel) ==
                  UCN_V6_ERR_REPLAY);
            CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
            changed = opened;
            changed.frame.message.interaction_role = UCN_V6_INTERACTION_RESULT;
            CHECK(ucn_v6_transfer_receive_fragment(
                      fixture.rx, 9U, &changed, &sentinel) ==
                  UCN_V6_ERR_REPLAY);
            CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
            changed = opened;
            changed.frame.traffic_class = UCN_V6_TRAFFIC_Q3;
            CHECK(ucn_v6_transfer_receive_fragment(
                      fixture.rx, 9U, &changed, &sentinel) ==
                  UCN_V6_ERR_REPLAY);
            CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
        }
        CHECK(ucn_v6_transfer_receive_fragment(
                  fixture.rx, 10U, &opened, &received[5]) == UCN_V6_OK);
        CHECK(received[5].complete && received[5].recent_replay);
        CHECK(ucn_v6_transfer_retire_completed(
                  fixture.rx, 11U, &origin, 1U, 1U) == UCN_V6_OK);
        {
            ucn_v6_security_open_result_t changed = opened;
            ucn_v6_transfer_rx_result_t sentinel;
            ucn_v6_transfer_rx_result_t before;
            memset(&sentinel, 0x5C, sizeof(sentinel));
            before = sentinel;
            changed.frame.message.destination_endpoint = 21U;
            CHECK(ucn_v6_transfer_receive_fragment(
                      fixture.rx, 12U, &changed, &sentinel) ==
                  UCN_V6_ERR_REPLAY);
            CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
        }
        CHECK(ucn_v6_transfer_receive_fragment(
                  fixture.rx, 12U, &opened, &received[5]) == UCN_V6_OK);
        CHECK(received[5].complete && received[5].recent_replay);
    }
    CHECK(ucn_v6_transfer_retire_tx(fixture.tx, 1U, &token) == UCN_V6_OK);
    CHECK(token == 101U);
    CHECK(ucn_v6_transfer_copy_stats(fixture.tx, &stats) == UCN_V6_OK);
    CHECK(stats.fragments_submitted == 7U &&
          stats.fragments_retransmitted == 1U);
    return 0;
}

static ucn_v6_security_open_result_t opened_credit(
    const ucn_v6_session_key_t *peer,
    const uint8_t encoded[UCN_V6_TRANSFER_CREDIT_BYTES])
{
    ucn_v6_security_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    opened.hop_authenticated = true;
    opened.ingress_peer_session = *peer;
    opened.frame.frame_type = UCN_V6_FRAME_TRANSFER;
    opened.frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TRANSFER_CREDIT;
    opened.frame.payload = encoded;
    opened.frame.payload_length = UCN_V6_TRANSFER_CREDIT_BYTES;
    return opened;
}

static int test_credit_and_session_fence(void)
{
    transfer_fixture_t fixture;
    ucn_v6_transfer_credit_update_t credit;
    ucn_v6_transfer_credit_reservation_t reservation;
    ucn_v6_security_open_result_t opened;
    ucn_v6_session_key_t peer = session(0x20U, 20U, 1U, 2U);
    ucn_v6_stack_invalidation_t event;
    ucn_v6_transfer_invalidation_result_t invalidation;
    uint8_t encoded[UCN_V6_TRANSFER_CREDIT_BYTES];
    uint8_t encoded_before[UCN_V6_TRANSFER_CREDIT_BYTES];
    uint64_t retired[2];
    size_t retired_count = 77U;
    CHECK(fixture_init(&fixture) == 0);
    memset(&credit, 0, sizeof(credit));
    credit.link_id = 3U;
    credit.link_generation = 1U;
    credit.traffic_class = UCN_V6_TRAFFIC_Q2;
    credit.credit_generation = 1U;
    credit.update_sequence = 1U;
    credit.available_credit = 2U;
    credit.maximum_credit = 2U;
    credit.lease_duration_us = 100U;
    memset(encoded, 0xA5, sizeof(encoded));
    memcpy(encoded_before, encoded, sizeof(encoded_before));
    credit.link_id = UINT16_MAX;
    CHECK(ucn_v6_transfer_credit_encode(&credit, encoded) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(encoded, encoded_before, sizeof(encoded)) == 0);
    credit.link_id = 3U;
    CHECK(ucn_v6_transfer_credit_encode(&credit, encoded) == UCN_V6_OK);
    opened = opened_credit(&peer, encoded);
    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 0U, &opened, 4U) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 50U, &opened, 4U) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_reserve_credit(
              fixture.tx, 100U, &peer, 3U, 1U, UCN_V6_TRAFFIC_Q2,
              &reservation) == UCN_V6_ERR_NO_SPACE);

    ++credit.link_generation;
    CHECK(ucn_v6_transfer_credit_encode(&credit, encoded) == UCN_V6_OK);
    opened = opened_credit(&peer, encoded);
    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 101U, &opened, 4U) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_reserve_credit(
              fixture.tx, 102U, &peer, 3U, 2U, UCN_V6_TRAFFIC_Q2,
              &reservation) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_finish_credit(
              fixture.tx, reservation.reservation_id, false) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_reserve_credit(
              fixture.tx, 103U, &peer, 3U, 2U, UCN_V6_TRAFFIC_Q2,
              &reservation) == UCN_V6_OK);

    /* A fresh absolute update cannot race an uncommitted local reservation. */
    ++credit.update_sequence;
    credit.available_credit = 1U;
    CHECK(ucn_v6_transfer_credit_encode(&credit, encoded) == UCN_V6_OK);
    opened = opened_credit(&peer, encoded);
    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 104U, &opened, 4U) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_transfer_finish_credit(
              fixture.tx, reservation.reservation_id, true) == UCN_V6_OK);

    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 104U, &opened, 4U) == UCN_V6_OK);
    credit.update_sequence = 1U;
    CHECK(ucn_v6_transfer_credit_encode(&credit, encoded) == UCN_V6_OK);
    opened = opened_credit(&peer, encoded);
    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 105U, &opened, 4U) == UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_transfer_reserve_credit(
              fixture.tx, 105U, &peer, 3U, 2U, UCN_V6_TRAFFIC_Q2,
              &reservation) == UCN_V6_OK);

    event = session_invalidation(&peer, 3U, 2U);
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &event, retired, 0U, &retired_count,
              &invalidation) == UCN_V6_OK);
    CHECK(retired_count == 0U && invalidation.credit_slots_retired == 1U &&
          invalidation.credit_reservations_retired == 1U);
    CHECK(ucn_v6_transfer_finish_credit(
              fixture.tx, reservation.reservation_id, true) ==
          UCN_V6_ERR_NOT_FOUND);
    return 0;
}

static int test_expired_credit_preserves_replay_high_water(void)
{
    transfer_fixture_t fixture;
    ucn_v6_transfer_credit_update_t credit;
    ucn_v6_transfer_credit_reservation_t old_reservation;
    ucn_v6_transfer_credit_reservation_t new_reservation;
    ucn_v6_security_open_result_t opened;
    ucn_v6_session_key_t peer = session(0x28U, 28U, 1U, 2U);
    uint8_t encoded[UCN_V6_TRANSFER_CREDIT_BYTES];

    CHECK(fixture_init(&fixture) == 0);
    memset(&credit, 0, sizeof(credit));
    credit.link_id = 5U;
    credit.link_generation = 1U;
    credit.traffic_class = UCN_V6_TRAFFIC_Q2;
    credit.credit_generation = 1U;
    credit.update_sequence = 1U;
    credit.available_credit = 1U;
    credit.maximum_credit = 1U;
    credit.lease_duration_us = 100U;
    CHECK(ucn_v6_transfer_credit_encode(&credit, encoded) == UCN_V6_OK);
    opened = opened_credit(&peer, encoded);
    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 0U, &opened, 1U) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_reserve_credit(
              fixture.tx, 1U, &peer, 5U, 1U, UCN_V6_TRAFFIC_Q2,
              &old_reservation) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_expire(fixture.tx, 100U) == UCN_V6_OK);

    /* Expiry fences borrowing; it must not erase the replay floor.  The old
     * absolute update cannot recreate generation 1 while its reservation is
     * still outstanding. */
    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 101U, &opened, 1U) == UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_transfer_reserve_credit(
              fixture.tx, 101U, &peer, 5U, 1U, UCN_V6_TRAFFIC_Q2,
              &new_reservation) == UCN_V6_ERR_NO_SPACE);
    CHECK(ucn_v6_transfer_finish_credit(
              fixture.tx, old_reservation.reservation_id, false) ==
          UCN_V6_OK);

    ++credit.update_sequence;
    CHECK(ucn_v6_transfer_credit_encode(&credit, encoded) == UCN_V6_OK);
    opened = opened_credit(&peer, encoded);
    CHECK(ucn_v6_transfer_ingest_credit(
              fixture.tx, 101U, &opened, 1U) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_reserve_credit(
              fixture.tx, 102U, &peer, 5U, 1U, UCN_V6_TRAFFIC_Q2,
              &new_reservation) == UCN_V6_OK);
    return 0;
}

static int test_timeout_rebind_and_atomic_invalidation(void)
{
    transfer_fixture_t fixture;
    ucn_v6_transfer_send_request_t request;
    ucn_v6_path_capability_t new_path;
    ucn_v6_transfer_tx_view_t view;
    ucn_v6_transfer_invalidation_result_t invalidation;
    ucn_v6_transfer_invalidation_result_t invalidation_before;
    ucn_v6_session_key_t destination;
    ucn_v6_stack_invalidation_t event;
    uint64_t retired[2];
    size_t retired_count = 91U;
    CHECK(fixture_init(&fixture) == 0);
    ++fixture.path.path_generation;
    ++fixture.path.route_generation;
    fixture.path.max_concurrency = 1U;
    CHECK(install_and_activate_path(&fixture, 0U, &fixture.path) == 0);
    request = request_for(&fixture, 1U, 64U, 32U, 2U);
    {
        ucn_v6_transfer_send_request_t invalid = request;
        invalid.route_ref.domain.destination_session_generation =
            UCN_V6_SERIAL_ROTATION_THRESHOLD + UINT32_C(1);
        CHECK(ucn_v6_transfer_send_begin(fixture.tx, 0U, &invalid) ==
              UCN_V6_ERR_ARGUMENT);
    }
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 0U, &request) == UCN_V6_OK);
    request.message_id = 2U;
    request.message.operation_id = 2U;
    request.buffer_token = 102U;
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 0U, &request) ==
          UCN_V6_ERR_NO_SPACE);
    new_path = fixture.path;
    new_path.path_id = 8U;
    new_path.path_generation = 1U;
    ++new_path.route_generation;
    new_path.deadline_us = 200000U;
    {
        ucn_v6_route_path_ref_t missing_reference =
            route_ref(&fixture, &new_path);
        CHECK(ucn_v6_transfer_rebind_path(
                  fixture.tx, 1U, 1U, &missing_reference) ==
              UCN_V6_ERR_REPLAY);
        CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &view) == UCN_V6_OK &&
              view.phase == UCN_V6_TRANSFER_TX_SENDING);
    }
    CHECK(install_and_activate_path(&fixture, 1U, &new_path) == 0);
    {
        ucn_v6_route_path_ref_t new_reference =
            route_ref(&fixture, &new_path);
        CHECK(ucn_v6_transfer_rebind_path(
                  fixture.tx, 1U, 1U, &new_reference) == UCN_V6_OK);
    }
    CHECK(ucn_v6_transfer_expire(fixture.tx, 200000U) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &view) == UCN_V6_OK);
    CHECK(view.phase == UCN_V6_TRANSFER_TX_FAILED);

    destination = fixture.path.local_parent_session;
    event = session_invalidation(
        &destination, fixture.path.local_parent_link_id,
        fixture.path.local_parent_link_generation);
    memset(&invalidation, 0xA5, sizeof(invalidation));
    invalidation_before = invalidation;
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &event, retired, 0U, &retired_count,
              &invalidation) == UCN_V6_ERR_NO_SPACE);
    CHECK(retired_count == 91U &&
          memcmp(&invalidation, &invalidation_before,
                 sizeof(invalidation)) == 0);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &view) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &event, retired, 2U, &retired_count,
              &invalidation) == UCN_V6_OK);
    CHECK(retired_count == 1U && retired[0] == 101U &&
          invalidation.tx_retired == 1U);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &view) ==
          UCN_V6_ERR_NOT_FOUND);
    return 0;
}

static int test_operation_id_gaps_and_restart_independence(void)
{
    transfer_fixture_t fixture;
    transfer_fixture_t restarted;
    ucn_v6_transfer_send_request_t request;
    ucn_v6_transfer_rx_result_t received;
    uint64_t retired_token = 0U;

    CHECK(fixture_init(&fixture) == 0);
    request = request_for(&fixture, UINT64_C(1000), 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 0U, &request) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 0U, &request) ==
          UCN_V6_ERR_REPLAY);
    CHECK(deliver_selected(&fixture, 0U, UINT64_C(1000), 0U, true, true,
                           &received) == 0);
    CHECK(received.complete);
    CHECK(apply_sack(&fixture, UINT64_C(1000), &received.sack) == 0);
    CHECK(ucn_v6_transfer_retire_tx(
              fixture.tx, UINT64_C(1000), &retired_token) == UCN_V6_OK);

    /* Transfer is not the durable operation-ID allocator. A lower, still
     * valid ID from the Message layer is legal when no active transfer owns
     * that exact ID; gaps and restart never require replaying IDs 1..N. */
    request = request_for(&fixture, UINT64_C(7), 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 1U, &request) == UCN_V6_OK);

    CHECK(fixture_init(&restarted) == 0);
    request = request_for(&restarted, UINT64_C(9000000), 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(restarted.tx, 0U, &request) ==
          UCN_V6_OK);
    return 0;
}

static int test_capability_bound_path_lifecycle(void)
{
    transfer_fixture_t fixture;
    ucn_v6_transfer_send_request_t request;
    ucn_v6_transfer_fragment_t fragment;
    ucn_v6_transfer_tx_view_t view;
    ucn_v6_transfer_stats_t stats;
    ucn_v6_transfer_sack_t sack;
    ucn_v6_security_open_result_t opened;
    ucn_v6_stack_invalidation_t old_path_event;
    ucn_v6_stack_invalidation_t capability_event;
    ucn_v6_stack_invalidation_t current_path_event;
    ucn_v6_stack_invalidation_t link_event;
    ucn_v6_transfer_invalidation_result_t invalidation_result;
    ucn_v6_path_capability_t next_path;
    uint8_t encoded_sack[UCN_V6_TRANSFER_SACK_BYTES];
    uint64_t retired[1];
    uint64_t token = 0U;
    size_t retired_count = 99U;
    ucn_v6_transfer_fragment_t fragment_before;

    CHECK(fixture_init(&fixture) == 0);
    request = request_for(&fixture, 1U, 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 1U, &request) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_next_fragment(
              fixture.tx, 2U, 1U, &fragment) == UCN_V6_OK);

    /* A selected fragment cannot record a submit after its exact Path
     * generation has been replaced.  No send attempt is accounted. */
    next_path = fixture.path;
    ++next_path.path_generation;
    ++next_path.route_generation;
    next_path.deadline_us = 200000U;
    CHECK(install_and_activate_path(&fixture, 3U, &next_path) == 0);
    CHECK(ucn_v6_capability_invalidation_peek(
              fixture.capability_owner, &old_path_event) == UCN_V6_OK);
    CHECK(old_path_event.type == UCN_V6_STACK_INVALIDATE_PATH &&
          old_path_event.path_generation + 1U ==
              fixture.path.path_generation);
    CHECK(ucn_v6_transfer_record_fragment_submit(
              fixture.tx, 3U, 1U, fragment.fragment_index, true) ==
          UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &view) == UCN_V6_OK &&
          view.phase == UCN_V6_TRANSFER_TX_FAILED &&
          view.cumulative_base == 0U);
    CHECK(ucn_v6_transfer_copy_stats(fixture.tx, &stats) == UCN_V6_OK &&
          stats.fragments_submitted == 0U && !stats.selection_pending);
    CHECK(ucn_v6_transfer_retire_tx(fixture.tx, 1U, &token) == UCN_V6_OK &&
          token == 101U);

    request = request_for(&fixture, 2U, 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 4U, &request) == UCN_V6_OK);

    /* A delayed invalidation for generation N cannot retire generation N+1. */
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &old_path_event, retired, 0U, &retired_count,
              &invalidation_result) == UCN_V6_OK);
    CHECK(retired_count == 0U && invalidation_result.tx_retired == 0U &&
          ucn_v6_transfer_copy_tx(fixture.tx, 2U, &view) == UCN_V6_OK);
    CHECK(ucn_v6_capability_apply_invalidation(
              fixture.capability_owner, &old_path_event) == UCN_V6_OK);
    CHECK(ucn_v6_capability_invalidation_peek(
              fixture.capability_owner, &old_path_event) ==
          UCN_V6_ERR_NOT_FOUND);

    CHECK(ucn_v6_transfer_next_fragment(
              fixture.tx, 5U, 2U, &fragment) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_record_fragment_submit(
              fixture.tx, 5U, 2U, fragment.fragment_index, true) == UCN_V6_OK);
    memset(&sack, 0, sizeof(sack));
    sack.message_id = 2U;
    sack.cumulative_base = 1U;
    sack.fragment_count = 1U;
    CHECK(ucn_v6_transfer_sack_encode(&sack, encoded_sack) == UCN_V6_OK);
    opened = opened_sack(&fixture, 2U, encoded_sack);

    /* SACK cannot advance a TX after Capability Owner replaces the Path.
     * expire() independently revalidates the same parent chain. */
    next_path = fixture.path;
    ++next_path.path_generation;
    ++next_path.route_generation;
    next_path.deadline_us = 300000U;
    CHECK(install_and_activate_path(&fixture, 6U, &next_path) == 0);
    CHECK(ucn_v6_transfer_apply_sack(fixture.tx, 6U, &opened) ==
          UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 2U, &view) == UCN_V6_OK &&
          view.phase == UCN_V6_TRANSFER_TX_SENDING &&
          view.cumulative_base == 0U);
    CHECK(ucn_v6_transfer_expire(fixture.tx, 6U) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 2U, &view) == UCN_V6_OK &&
          view.phase == UCN_V6_TRANSFER_TX_FAILED);
    CHECK(ucn_v6_transfer_retire_tx(fixture.tx, 2U, &token) == UCN_V6_OK &&
          token == 102U);

    request = request_for(&fixture, 3U, 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 7U, &request) == UCN_V6_OK);
    capability_event = old_path_event;
    capability_event.type = UCN_V6_STACK_INVALIDATE_CAPABILITY;
    capability_event.path_id = 0U;
    capability_event.path_generation = 0U;
    retired_count = 71U;
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &capability_event, retired, 0U, &retired_count,
              &invalidation_result) == UCN_V6_ERR_NO_SPACE);
    CHECK(retired_count == 71U &&
          ucn_v6_transfer_copy_tx(fixture.tx, 3U, &view) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &capability_event, retired, 1U, &retired_count,
              &invalidation_result) == UCN_V6_OK);
    CHECK(retired_count == 1U && retired[0] == 103U &&
          invalidation_result.tx_retired == 1U);

    request = request_for(&fixture, 4U, 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 8U, &request) == UCN_V6_OK);
    current_path_event = capability_event;
    current_path_event.type = UCN_V6_STACK_INVALIDATE_PATH;
    current_path_event.path_id = fixture.path.path_id;
    current_path_event.path_generation = fixture.path.path_generation;
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &current_path_event, retired, 1U, &retired_count,
              &invalidation_result) == UCN_V6_OK);
    CHECK(retired_count == 1U && retired[0] == 104U &&
          invalidation_result.tx_retired == 1U);

    request = request_for(&fixture, 5U, 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 9U, &request) == UCN_V6_OK);
    link_event = current_path_event;
    link_event.type = UCN_V6_STACK_INVALIDATE_LINK;
    memset(&link_event.session, 0, sizeof(link_event.session));
    link_event.capability_generation = 0U;
    link_event.path_id = 0U;
    link_event.path_generation = 0U;
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &link_event, retired, 1U, &retired_count,
              &invalidation_result) == UCN_V6_OK);
    CHECK(retired_count == 1U && retired[0] == 105U &&
          invalidation_result.tx_retired == 1U);

    request = request_for(&fixture, 6U, 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 10U, &request) == UCN_V6_OK);
    next_path = fixture.path;
    ++next_path.path_generation;
    ++next_path.route_generation;
    next_path.deadline_us = 400000U;
    CHECK(install_and_activate_path(&fixture, 11U, &next_path) == 0);
    {
        ucn_v6_route_path_ref_t next_reference =
            route_ref(&fixture, &next_path);
        ucn_v6_transfer_tx_view_t before;
        ucn_v6_transfer_tx_view_t after;
        CHECK(ucn_v6_transfer_copy_tx(
                  fixture.tx, 6U, &before) == UCN_V6_OK);
        CHECK(ucn_v6_transfer_rebind_path(
                  fixture.tx, 11U, 6U, &next_reference) ==
              UCN_V6_ERR_TIMEOUT);
        CHECK(ucn_v6_transfer_copy_tx(
                  fixture.tx, 6U, &after) == UCN_V6_OK);
        CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    }
    memset(&fragment, 0x5A, sizeof(fragment));
    fragment_before = fragment;
    CHECK(ucn_v6_transfer_next_fragment(
              fixture.tx, 11U, 6U, &fragment) == UCN_V6_ERR_TIMEOUT);
    CHECK(memcmp(&fragment, &fragment_before, sizeof(fragment)) == 0 &&
          ucn_v6_transfer_copy_tx(fixture.tx, 6U, &view) == UCN_V6_OK &&
          view.phase == UCN_V6_TRANSFER_TX_FAILED);
    return 0;
}

static int test_multihop_egress_parent_fences_transfer(void)
{
    transfer_fixture_t fixture;
    ucn_v6_capability_record_t next_hop_capability;
    ucn_v6_security_open_result_t opened;
    ucn_v6_path_capability_t next_path;
    ucn_v6_route_path_t route_path;
    ucn_v6_transfer_send_request_t request;
    ucn_v6_transfer_tx_view_t view;
    ucn_v6_stack_invalidation_t event;
    ucn_v6_transfer_invalidation_result_t result;
    uint8_t encoded[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint64_t retired[1];
    size_t retired_count = 0U;

    CHECK(fixture_init(&fixture) == 0);

    /* B is the immediate next hop while C remains the end-to-end
     * destination. Its local physical parent is deliberately different from
     * C's destination Capability parent. */
    next_hop_capability = capability_record(1U, 77U);
    CHECK(ucn_v6_capability_record_encode(
              &next_hop_capability, encoded) == UCN_V6_OK);
    opened = capability_open(&fixture, encoded);
    opened.authenticated_principal = principal(0x30U);
    opened.ingress_peer_session.principal = principal(0x30U);
    opened.ingress_peer_session.binding.realm_id = 1U;
    opened.ingress_peer_session.binding.node_address = 30U;
    opened.ingress_peer_session.binding.binding_generation = 4U;
    opened.ingress_peer_session.session_generation = 9U;
    opened.ingress_link_instance_id = 9U;
    opened.ingress_link_instance_generation = 11U;
    opened.frame.source_address = 30U;
    opened.frame.source_binding_generation = 4U;
    opened.frame.session_generation = 9U;
    CHECK(ucn_v6_capability_ingest_advertise(
              fixture.capability_owner, 1U, &opened,
              &next_hop_capability) == UCN_V6_OK);

    next_path = fixture.path;
    ++next_path.route_generation;
    ++next_path.path_generation;
    next_path.local_parent_session = opened.ingress_peer_session;
    next_path.local_parent_link_id = opened.ingress_link_instance_id;
    next_path.local_parent_link_generation =
        opened.ingress_link_instance_generation;
    next_path.local_parent_capability_generation =
        next_hop_capability.capability_generation;
    CHECK(ucn_v6_capability_digest(
              &next_hop_capability,
              next_path.local_parent_capability_digest) == UCN_V6_OK);
    next_path.hop_count = 2U;
    next_path.deadline_us = 200000U;
    CHECK(ucn_v6_capability_install_path(
              fixture.capability_owner, 1U, &next_path) == UCN_V6_OK);
    fixture.path = next_path;
    memset(&route_path, 0, sizeof(route_path));
    route_path.path_id = next_path.path_id;
    route_path.path_generation = next_path.path_generation;
    route_path.next_hop = opened.ingress_peer_session;
    route_path.egress_link_id = opened.ingress_link_instance_id;
    route_path.egress_link_generation =
        opened.ingress_link_instance_generation;
    route_path.next_hop_capability_generation =
        next_hop_capability.capability_generation;
    route_path.hop_count = 2U;
    route_path.priority = 1U;
    route_path.weight = 1U;
    route_path.available = true;
    route_path.capability = next_path;
    CHECK(activate_exact_route_path(&fixture, 1U, 2U, &route_path) == 0);

    request = request_for(&fixture, 50U, 32U, 32U, 1U);
    CHECK(ucn_v6_transfer_send_begin(fixture.tx, 2U, &request) == UCN_V6_OK);
    memset(&event, 0, sizeof(event));
    event.type = UCN_V6_STACK_INVALIDATE_LINK;
    event.link_id = 9U;
    event.link_generation = 11U;
    CHECK(ucn_v6_transfer_apply_invalidation(
              fixture.tx, &event, retired, 1U, &retired_count,
              &result) == UCN_V6_OK);
    CHECK(retired_count == 1U && retired[0] == 150U &&
          result.tx_retired == 1U &&
          ucn_v6_transfer_copy_tx(fixture.tx, 50U, &view) ==
              UCN_V6_ERR_NOT_FOUND);
    return 0;
}

int main(void)
{
    CHECK(test_codec_and_message_classes() == 0);
    CHECK(test_selective_repeat_and_recent() == 0);
    CHECK(test_credit_and_session_fence() == 0);
    CHECK(test_expired_credit_preserves_replay_high_water() == 0);
    CHECK(test_timeout_rebind_and_atomic_invalidation() == 0);
    CHECK(test_operation_id_gaps_and_restart_independence() == 0);
    CHECK(test_capability_bound_path_lifecycle() == 0);
    CHECK(test_multihop_egress_parent_fences_transfer() == 0);
    puts("v6 transfer tests passed");
    return 0;
}
