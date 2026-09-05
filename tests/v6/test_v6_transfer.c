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
    ucn_v6_transfer_owner_storage_t tx_storage;
    ucn_v6_transfer_owner_storage_t rx_storage;
    ucn_v6_transfer_owner_t *tx;
    ucn_v6_transfer_owner_t *rx;
    ucn_v6_route_domain_t domain;
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

static int fixture_init(transfer_fixture_t *fixture)
{
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
    fixture->path.valid = true;
    fixture->path.destination_principal =
        fixture->domain.destination_principal;
    fixture->path.destination_binding = fixture->domain.destination_binding;
    fixture->path.session_generation = 7U;
    fixture->path.destination_link_instance_generation = 8U;
    fixture->path.route_generation = 9U;
    fixture->path.path_id = 4U;
    fixture->path.path_generation = 6U;
    fixture->path.path_frame_mtu = 256U;
    fixture->path.payload_budget = 180U;
    fixture->path.fragment_data_budget = 100U;
    fixture->path.feature_bits = UCN_V6_FEATURE_TRANSFER;
    fixture->path.max_message_class = UCN_V6_MESSAGE_T8K;
    fixture->path.max_window = UCN_V6_CONFIG_TRANSFER_WINDOW;
    fixture->path.max_concurrency = UCN_V6_CONFIG_TRANSFER_TX_SLOTS;
    fixture->path.deadline_us = 100000U;
    for (index = 0U; index < sizeof(fixture->payload); ++index) {
        fixture->payload[index] = (uint8_t)(index * 29U + 7U);
    }
    CHECK(ucn_v6_transfer_owner_init_in_place(
              fixture->tx_storage.bytes, sizeof(fixture->tx_storage),
              ucn_v6_compiled_manifest(), 100U, 4U, 1000U, 2000U,
              &fixture->tx) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_owner_init_in_place(
              fixture->rx_storage.bytes, sizeof(fixture->rx_storage),
              ucn_v6_compiled_manifest(), 100U, 4U, 1000U, 2000U,
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
    request.route_domain = fixture->domain;
    request.path = fixture->path;
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
    opened.frame.session_generation = fixture->path.session_generation;
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
    CHECK(ucn_v6_transfer_apply_sack(fixture->tx, &opened) == UCN_V6_OK);
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
    CHECK(ucn_v6_transfer_copy_completed(
              fixture.rx, &origin, 1U, 1U, output, sizeof(output),
              &completed) == UCN_V6_OK);
    CHECK(completed.payload_length == TEST_SELECTIVE_BYTES &&
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
        CHECK(ucn_v6_transfer_receive_fragment(
                  fixture.rx, 10U, &opened, &received[5]) == UCN_V6_OK);
        CHECK(received[5].complete && received[5].recent_replay);
        CHECK(ucn_v6_transfer_retire_completed(
                  fixture.rx, 11U, &origin, 1U, 1U) == UCN_V6_OK);
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
    ucn_v6_transfer_invalidation_result_t invalidation;
    uint8_t encoded[UCN_V6_TRANSFER_CREDIT_BYTES];
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

    CHECK(ucn_v6_transfer_invalidate_session(
              fixture.tx, &peer, retired, 0U, &retired_count,
              &invalidation) == UCN_V6_OK);
    CHECK(retired_count == 0U && invalidation.credit_slots_retired == 1U &&
          invalidation.credit_reservations_retired == 1U);
    CHECK(ucn_v6_transfer_finish_credit(
              fixture.tx, reservation.reservation_id, true) ==
          UCN_V6_ERR_NOT_FOUND);
    return 0;
}

static int test_timeout_rebind_and_atomic_invalidation(void)
{
    transfer_fixture_t fixture;
    ucn_v6_transfer_send_request_t request;
    ucn_v6_path_capability_t new_path;
    ucn_v6_transfer_tx_view_t view;
    ucn_v6_transfer_invalidation_result_t invalidation;
    ucn_v6_session_key_t destination;
    uint64_t retired[2];
    size_t retired_count = 91U;
    CHECK(fixture_init(&fixture) == 0);
    fixture.path.max_concurrency = 1U;
    request = request_for(&fixture, 1U, 64U, 32U, 2U);
    request.path = fixture.path;
    {
        ucn_v6_transfer_send_request_t invalid = request;
        invalid.path.session_generation =
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
    new_path.path_generation = 7U;
    new_path.deadline_us = 200000U;
    CHECK(ucn_v6_transfer_rebind_path(
              fixture.tx, 1U, 1U, &new_path) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_expire(fixture.tx, 200000U) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &view) == UCN_V6_OK);
    CHECK(view.phase == UCN_V6_TRANSFER_TX_FAILED);

    destination.principal = fixture.domain.destination_principal;
    destination.binding = fixture.domain.destination_binding;
    destination.session_generation = fixture.path.session_generation;
    CHECK(ucn_v6_transfer_invalidate_session(
              fixture.tx, &destination, retired, 0U, &retired_count,
              &invalidation) == UCN_V6_ERR_NO_SPACE);
    CHECK(retired_count == 91U);
    CHECK(ucn_v6_transfer_copy_tx(fixture.tx, 1U, &view) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_invalidate_session(
              fixture.tx, &destination, retired, 2U, &retired_count,
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

int main(void)
{
    CHECK(test_codec_and_message_classes() == 0);
    CHECK(test_selective_repeat_and_recent() == 0);
    CHECK(test_credit_and_session_fence() == 0);
    CHECK(test_timeout_rebind_and_atomic_invalidation() == 0);
    CHECK(test_operation_id_gaps_and_restart_independence() == 0);
    puts("v6 transfer tests passed");
    return 0;
}
