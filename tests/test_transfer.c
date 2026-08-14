#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_transfer.h"

#define TRANSFER_TEST_ENDPOINT ((ucn_endpoint_t)0x80U)

typedef struct transfer_test_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool is_up;
    bool drop_first_ack;
    bool drop_all_acks;
    bool drop_first_fragment;
    bool drop_first_nonzero_fragment;
    uint16_t drop_ack_offset;
    uint32_t dropped_acks;
    uint32_t dropped_fragments;
} transfer_test_link_context_t;

typedef struct transfer_test_receive_state {
    ucn_transfer_t *owner;
    const uint8_t *expected;
    uint16_t expected_length;
    ucn_transfer_class_t expected_class;
    uint32_t count;
    ucn_transfer_rx_handle_t last_handle;
    ucn_result_t release_result;
    bool auto_release;
    bool mismatch;
} transfer_test_receive_state_t;

typedef struct transfer_test_completion_state {
    uint32_t count;
    ucn_transfer_completion_status_t status;
    ucn_node_id_t destination;
    ucn_endpoint_t endpoint;
    uint16_t transfer_id;
} transfer_test_completion_state_t;

typedef struct transfer_test_pair {
    ucn_node_t a;
    ucn_node_t b;
    ucn_transfer_t transfer_a;
    ucn_transfer_t transfer_b;
    ucn_link_t ab;
    ucn_link_t ba;
    transfer_test_link_context_t context_ab;
    transfer_test_link_context_t context_ba;
    transfer_test_receive_state_t received;
    uint32_t now_ms;
} transfer_test_pair_t;

static transfer_test_pair_t DIRECT_PAIR;
static transfer_test_pair_t MTU_PAIR;
static transfer_test_pair_t WINDOW_PAIR;
static uint8_t TRANSFER_TEST_DATA[UCN_TRANSFER_MAX_MESSAGE_BYTES];

static uint32_t transfer_test_now_ms(void *context)
{
    return *(const uint32_t *)context;
}

static ucn_result_t transfer_test_step_at(ucn_transfer_t *transfer,
                                          uint32_t now_ms)
{
    uint32_t *clock = (uint32_t *)transfer->config.now_context;

    *clock = now_ms;
    return ucn_transfer_step(transfer);
}

static ucn_transfer_class_t transfer_test_local_max_class(void)
{
    ucn_transfer_class_t transfer_class = UCN_TRANSFER_CLASS_T32;

    while (transfer_class + 1 < UCN_TRANSFER_CLASS_COUNT &&
           ucn_transfer_class_max_bytes(
               (ucn_transfer_class_t)(transfer_class + 1)) <=
               UCN_TRANSFER_MAX_MESSAGE_BYTES) {
        transfer_class = (ucn_transfer_class_t)(transfer_class + 1);
    }
    return transfer_class;
}

static ucn_result_t transfer_test_link_send(ucn_link_t *link,
                                            const uint8_t *bytes,
                                            size_t length)
{
    transfer_test_link_context_t *context =
        (transfer_test_link_context_t *)link->context;
    ucn_frame_t frame;
    ucn_result_t decode_result = ucn_frame_decode(bytes, length, &frame);

    if (decode_result == UCN_OK &&
        frame.message_type == UCN_MSG_TRANSFER_FRAGMENT &&
        context->drop_first_fragment) {
        context->drop_first_fragment = false;
        context->dropped_fragments++;
        return UCN_OK;
    }
    if (decode_result == UCN_OK &&
        frame.message_type == UCN_MSG_TRANSFER_FRAGMENT &&
        context->drop_first_nonzero_fragment) {
        ucn_transfer_fragment_t fragment;

        if (ucn_transfer_decode_fragment(frame.payload, frame.payload_length,
                                         &fragment) == UCN_OK &&
            fragment.fragment_offset != 0U) {
            context->drop_first_nonzero_fragment = false;
            context->dropped_fragments++;
            return UCN_OK;
        }
    }
    if (decode_result == UCN_OK &&
        frame.message_type == UCN_MSG_TRANSFER_ACK) {
        ucn_transfer_ack_t ack;
        bool drop = context->drop_first_ack;

        if (context->drop_all_acks &&
            ucn_transfer_decode_ack(frame.payload, frame.payload_length,
                                    &ack) == UCN_OK &&
            (context->drop_ack_offset == 0U ||
             ack.next_expected_offset == context->drop_ack_offset)) {
            drop = true;
        }
        if (drop) {
            context->dropped_acks++;
            context->drop_first_ack = false;
            return UCN_OK;
        }
    }
    return ucn_node_receive(context->peer, context->peer_ingress, bytes,
                            length);
}

static ucn_result_t transfer_test_link_status(const ucn_link_t *link,
                                              ucn_link_status_t *status)
{
    const transfer_test_link_context_t *context =
        (const transfer_test_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t TRANSFER_TEST_LINK_OPS = {
    NULL,
    transfer_test_link_send,
    NULL,
    transfer_test_link_status,
    NULL,
    NULL
};

static void transfer_test_receive(void *context,
                                  ucn_node_id_t source,
                                  ucn_session_id_t source_session_id,
                                  ucn_endpoint_t endpoint,
                                  ucn_transfer_class_t transfer_class,
                                  const uint8_t *data,
                                  uint16_t length,
                                  ucn_transfer_rx_handle_t handle)
{
    transfer_test_receive_state_t *state =
        (transfer_test_receive_state_t *)context;

    (void)source_session_id;
    state->count++;
    state->last_handle = handle;
    if (source == 0U || endpoint != TRANSFER_TEST_ENDPOINT ||
        transfer_class != state->expected_class ||
        length != state->expected_length || state->expected == NULL ||
        memcmp(data, state->expected, length) != 0) {
        state->mismatch = true;
    }
    if (state->auto_release && handle != UCN_TRANSFER_RX_HANDLE_DIRECT) {
        state->release_result =
            ucn_transfer_release_received(state->owner, handle);
    }
}

static void transfer_test_complete(void *context,
                                   ucn_node_id_t destination,
                                   ucn_endpoint_t endpoint,
                                   uint16_t transfer_id,
                                   ucn_transfer_completion_status_t status)
{
    transfer_test_completion_state_t *state =
        (transfer_test_completion_state_t *)context;

    state->count++;
    state->status = status;
    state->destination = destination;
    state->endpoint = endpoint;
    state->transfer_id = transfer_id;
}

static int transfer_test_init_node(ucn_node_t *node, ucn_node_id_t node_id)
{
    const ucn_config_t config = {
        UINT32_C(0x5452414E),
        node_id,
        8U
    };

    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void transfer_test_init_link(ucn_link_t *link,
                                    transfer_test_link_context_t *context,
                                    uint8_t link_id,
                                    size_t mtu,
                                    ucn_node_id_t peer_node_id,
                                    ucn_node_t *peer,
                                    ucn_link_t *peer_ingress)
{
    (void)memset(link, 0, sizeof(*link));
    (void)memset(context, 0, sizeof(*context));
    link->ops = &TRANSFER_TEST_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = mtu;
    link->peer_node_id = peer_node_id;
    context->peer = peer;
    context->peer_ingress = peer_ingress;
    context->is_up = true;
}

static int transfer_test_init_pair_window(transfer_test_pair_t *pair,
                                          size_t mtu,
                                          uint8_t tx_window_size)
{
    ucn_transfer_config_t config;

    (void)memset(pair, 0, sizeof(*pair));
    if (transfer_test_init_node(&pair->a, UINT32_C(1)) != 0 ||
        transfer_test_init_node(&pair->b, UINT32_C(2)) != 0) {
        return 1;
    }
    transfer_test_init_link(&pair->ab, &pair->context_ab, 1U, mtu,
                            UINT32_C(2), &pair->b, &pair->ba);
    transfer_test_init_link(&pair->ba, &pair->context_ba, 2U, mtu,
                            UINT32_C(1), &pair->a, &pair->ab);
    if (ucn_node_register_link(&pair->a, &pair->ab) != UCN_OK ||
        ucn_node_register_link(&pair->b, &pair->ba) != UCN_OK) {
        return 1;
    }

    (void)memset(&config, 0, sizeof(config));
    config.node = &pair->a;
    config.now_ms = transfer_test_now_ms;
    config.now_context = &pair->now_ms;
    config.ack_timeout_ms = 10U;
    if (ucn_transfer_init(&pair->transfer_a, &config) != UCN_OK) {
        return 1;
    }
    if (ucn_transfer_set_tx_window_size(
            &pair->transfer_a, tx_window_size) != UCN_OK) {
        return 1;
    }
    config.node = &pair->b;
    if (ucn_transfer_init(&pair->transfer_b, &config) != UCN_OK) {
        return 1;
    }
    pair->received.owner = &pair->transfer_b;
    pair->received.release_result = UCN_ERR_NOT_FOUND;
    if (ucn_transfer_bind_endpoint(
            &pair->transfer_b, TRANSFER_TEST_ENDPOINT,
            transfer_test_local_max_class(), false, transfer_test_receive,
            &pair->received) != UCN_OK ||
        ucn_transfer_set_peer_capability(
            &pair->transfer_a, UINT32_C(2),
            transfer_test_local_max_class()) != UCN_OK) {
        return 1;
    }
    if (ucn_transfer_set_peer_window_capability(
            &pair->transfer_a, UINT32_C(2), tx_window_size) != UCN_OK) {
        return 1;
    }
    return 0;
}

static int transfer_test_init_pair(transfer_test_pair_t *pair, size_t mtu)
{
    return transfer_test_init_pair_window(pair, mtu, 1U);
}

static void transfer_test_prepare_receive(transfer_test_receive_state_t *state,
                                          const uint8_t *expected,
                                          uint16_t length,
                                          ucn_transfer_class_t transfer_class,
                                          bool auto_release)
{
    ucn_transfer_t *owner = state->owner;

    (void)memset(state, 0, sizeof(*state));
    state->owner = owner;
    state->expected = expected;
    state->expected_length = length;
    state->expected_class = transfer_class;
    state->auto_release = auto_release;
    state->release_result = UCN_ERR_NOT_FOUND;
}

static int transfer_test_run_until_complete(
    ucn_transfer_t *sender,
    ucn_transfer_t *receiver,
    transfer_test_completion_state_t *completion,
    uint32_t *now_ms,
    uint32_t max_steps)
{
    uint32_t step;

    for (step = 0U; step < max_steps && completion->count == 0U; ++step) {
        ucn_result_t result;

        (*now_ms)++;
        result = transfer_test_step_at(sender, *now_ms);
        if (result != UCN_OK && result != UCN_ERR_NOT_FOUND &&
            result != UCN_ERR_TOO_LARGE && result != UCN_ERR_NO_SPACE &&
            result != UCN_ERR_LINK_DOWN) {
            return 1;
        }
        result = transfer_test_step_at(receiver, *now_ms);
        if (result != UCN_OK && result != UCN_ERR_NOT_FOUND) {
            return 1;
        }
    }
    return completion->count == 1U ? 0 : 1;
}

static int transfer_test_codec(void)
{
    static const size_t limits[UCN_TRANSFER_CLASS_COUNT] = {
        32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U, 8192U
    };
    static const uint8_t crc_vector[] = "123456789";
    uint8_t data[16];
    uint8_t encoded[UCN_MAX_PAYLOAD_BYTES];
    uint8_t ack_bytes[UCN_TRANSFER_ACK_BYTES];
    ucn_transfer_fragment_t fragment;
    ucn_transfer_fragment_t decoded;
    ucn_transfer_ack_t ack;
    ucn_transfer_ack_t decoded_ack;
    size_t encoded_length;
    size_t index;

    (void)memset(data, 0xA5, sizeof(data));
    for (index = 0U; index < UCN_TRANSFER_CLASS_COUNT; ++index) {
        TEST_ASSERT(ucn_transfer_class_max_bytes(
                        (ucn_transfer_class_t)index) == limits[index]);
        TEST_ASSERT(ucn_transfer_smallest_class_for_length(limits[index]) ==
                    (ucn_transfer_class_t)index);
    }
    TEST_ASSERT(ucn_transfer_smallest_class_for_length(0U) ==
                UCN_TRANSFER_CLASS_COUNT);
    TEST_ASSERT(ucn_transfer_smallest_class_for_length(8193U) ==
                UCN_TRANSFER_CLASS_COUNT);
    TEST_ASSERT(ucn_transfer_crc32(crc_vector,
                                   sizeof(crc_vector) - 1U) ==
                UINT32_C(0xCBF43926));

    (void)memset(&fragment, 0, sizeof(fragment));
    fragment.target_endpoint = TRANSFER_TEST_ENDPOINT;
    fragment.transfer_class = UCN_TRANSFER_CLASS_T128;
    fragment.flags = UCN_TRANSFER_FLAG_START | UCN_TRANSFER_FLAG_END;
    fragment.transfer_id = 7U;
    fragment.total_length = (uint16_t)sizeof(data);
    fragment.message_crc32 = ucn_transfer_crc32(data, sizeof(data));
    fragment.data = data;
    fragment.data_length = (uint16_t)sizeof(data);
    TEST_ASSERT(ucn_transfer_encode_fragment(
                    &fragment, encoded, sizeof(encoded), &encoded_length) ==
                UCN_OK);
    TEST_ASSERT(encoded_length ==
                UCN_TRANSFER_FRAGMENT_HEADER_BYTES + sizeof(data));
    TEST_ASSERT(ucn_transfer_decode_fragment(encoded, encoded_length,
                                              &decoded) == UCN_OK);
    TEST_ASSERT(decoded.transfer_id == fragment.transfer_id);
    TEST_ASSERT(decoded.total_length == fragment.total_length);
    TEST_ASSERT(decoded.message_crc32 == fragment.message_crc32);
    TEST_ASSERT(memcmp(decoded.data, data, sizeof(data)) == 0);
    encoded[3] = 0x80U;
    TEST_ASSERT(ucn_transfer_decode_fragment(encoded, encoded_length,
                                              &decoded) ==
                UCN_ERR_MALFORMED);
    encoded[3] = UCN_TRANSFER_FLAG_START | UCN_TRANSFER_FLAG_END;
    encoded[2] = (uint8_t)UCN_TRANSFER_CLASS_T64;
    TEST_ASSERT(ucn_transfer_decode_fragment(encoded, encoded_length,
                                              &decoded) ==
                UCN_ERR_MALFORMED);

    ack.target_endpoint = TRANSFER_TEST_ENDPOINT;
    ack.transfer_id = 7U;
    ack.next_expected_offset = 16U;
    ack.status = UCN_TRANSFER_ACK_OK;
    TEST_ASSERT(ucn_transfer_encode_ack(&ack, ack_bytes) == UCN_OK);
    TEST_ASSERT(ucn_transfer_decode_ack(ack_bytes, sizeof(ack_bytes),
                                       &decoded_ack) == UCN_OK);
    TEST_ASSERT(decoded_ack.transfer_id == ack.transfer_id);
    TEST_ASSERT(decoded_ack.next_expected_offset == 16U);
    ack_bytes[7] = 1U;
    TEST_ASSERT(ucn_transfer_decode_ack(ack_bytes, sizeof(ack_bytes),
                                       &decoded_ack) == UCN_ERR_MALFORMED);
    return 0;
}

static int transfer_test_all_classes(void)
{
    transfer_test_pair_t *pair = &DIRECT_PAIR;
    uint32_t now_ms = 0U;
    size_t index;
    ucn_transfer_class_t transfer_class;
    uint32_t expected_direct = 0U;
    uint32_t expected_fragmented = 0U;

    TEST_ASSERT(transfer_test_init_pair(pair, 128U) == 0);
    for (index = 0U; index < sizeof(TRANSFER_TEST_DATA); ++index) {
        TRANSFER_TEST_DATA[index] =
            (uint8_t)((index * 17U + index / 251U) & 0xFFU);
    }
    if (UCN_TRANSFER_MAX_MESSAGE_BYTES >= 128U) {
        TEST_ASSERT(ucn_transfer_set_peer_capability(
                        &pair->transfer_a, UINT32_C(2),
                        UCN_TRANSFER_CLASS_T64) == UCN_OK);
        TEST_ASSERT(ucn_transfer_send(
                        &pair->transfer_a, UINT32_C(2),
                        TRANSFER_TEST_ENDPOINT, UCN_TRANSFER_CLASS_T128,
                        TRANSFER_TEST_DATA, 128U, NULL, NULL) ==
                    UCN_ERR_ACCESS);
        TEST_ASSERT(ucn_transfer_set_peer_capability(
                        &pair->transfer_a, UINT32_C(2),
                        transfer_test_local_max_class()) == UCN_OK);
    }
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T32, TRANSFER_TEST_DATA, 33U,
                    NULL, NULL) == UCN_ERR_TOO_LARGE);
    for (transfer_class = UCN_TRANSFER_CLASS_T32;
         transfer_class < UCN_TRANSFER_CLASS_COUNT;
         transfer_class = (ucn_transfer_class_t)(transfer_class + 1)) {
        const uint16_t length = (uint16_t)ucn_transfer_class_max_bytes(
            transfer_class);
        transfer_test_completion_state_t completion;

        if (length > UCN_TRANSFER_MAX_MESSAGE_BYTES ||
            (transfer_class <= UCN_TRANSFER_CLASS_T64 &&
             length > UCN_MAX_PAYLOAD_BYTES)) {
            continue;
        }

        (void)memset(&completion, 0, sizeof(completion));
        transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA,
                                      length, transfer_class, true);
        TEST_ASSERT(ucn_transfer_send(
                        &pair->transfer_a, UINT32_C(2),
                        TRANSFER_TEST_ENDPOINT, transfer_class,
                        TRANSFER_TEST_DATA, length, transfer_test_complete,
                        &completion) == UCN_OK);
        if (transfer_class <= UCN_TRANSFER_CLASS_T64) {
            expected_direct++;
            TEST_ASSERT(completion.count == 1U);
            TEST_ASSERT(completion.status == UCN_TRANSFER_COMPLETION_SENT);
        } else {
            expected_fragmented++;
            TEST_ASSERT(transfer_test_run_until_complete(
                            &pair->transfer_a, &pair->transfer_b, &completion,
                            &now_ms, 2000U) == 0);
            TEST_ASSERT(completion.status ==
                        UCN_TRANSFER_COMPLETION_DELIVERED);
            TEST_ASSERT(completion.transfer_id != 0U);
            TEST_ASSERT(pair->received.release_result == UCN_OK);
        }
        TEST_ASSERT(pair->received.count == 1U);
        TEST_ASSERT(!pair->received.mismatch);
        TEST_ASSERT(completion.destination == UINT32_C(2));
        TEST_ASSERT(completion.endpoint == TRANSFER_TEST_ENDPOINT);
    }
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_a)->direct_sent ==
                expected_direct);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_a)->messages_delivered ==
                expected_fragmented);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_b)->messages_reassembled ==
                expected_fragmented);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_a)->fragments_sent >=
                expected_fragmented);
    return 0;
}

static int transfer_test_retry_and_slot_lifetime(void)
{
    transfer_test_pair_t *pair = &DIRECT_PAIR;
    transfer_test_completion_state_t completion;
    ucn_transfer_rx_handle_t handles[UCN_TRANSFER_RX_SLOTS];
    ucn_transfer_rx_handle_t second_handle;
    uint32_t now_ms = 1000U;
    uint32_t retried_before;
    size_t slot_index;

    if (UCN_TRANSFER_MAX_MESSAGE_BYTES < 128U) {
        return 0;
    }
    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 128U,
                                  UCN_TRANSFER_CLASS_T128, true);
    (void)memset(&completion, 0, sizeof(completion));
    pair->context_ba.drop_first_ack = true;
    retried_before =
        ucn_transfer_get_stats(&pair->transfer_a)->fragments_retried;
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T128, TRANSFER_TEST_DATA, 128U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(ucn_transfer_set_tx_window_size(
                    &pair->transfer_a, 2U) == UCN_ERR_ACCESS);
    while (pair->context_ba.dropped_acks == 0U) {
        ucn_result_t result;

        now_ms++;
        result = transfer_test_step_at(&pair->transfer_a, now_ms);
        TEST_ASSERT(result == UCN_OK || result == UCN_ERR_TOO_LARGE);
    }
    TEST_ASSERT(completion.count == 0U);
    TEST_ASSERT(transfer_test_step_at(&pair->transfer_a, now_ms + 9U) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(transfer_test_step_at(&pair->transfer_a, now_ms + 10U) == UCN_OK);
    now_ms += 10U;
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 100U) == 0);
    TEST_ASSERT(completion.status == UCN_TRANSFER_COMPLETION_DELIVERED);
    TEST_ASSERT(pair->context_ba.dropped_acks == 1U);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_a)->fragments_retried >
                retried_before);
    TEST_ASSERT(pair->received.count == 1U);

    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 128U,
                                  UCN_TRANSFER_CLASS_T128, false);
    (void)memset(&completion, 0, sizeof(completion));
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T128, TRANSFER_TEST_DATA, 128U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 100U) == 0);
    handles[0] = pair->received.last_handle;
    TEST_ASSERT(handles[0] != UCN_TRANSFER_RX_HANDLE_DIRECT);

    for (slot_index = 1U; slot_index < UCN_TRANSFER_RX_SLOTS; ++slot_index) {
        (void)memset(&completion, 0, sizeof(completion));
        TEST_ASSERT(ucn_transfer_send(
                        &pair->transfer_a, UINT32_C(2),
                        TRANSFER_TEST_ENDPOINT, UCN_TRANSFER_CLASS_T128,
                        TRANSFER_TEST_DATA, 128U, transfer_test_complete,
                        &completion) == UCN_OK);
        TEST_ASSERT(transfer_test_run_until_complete(
                        &pair->transfer_a, &pair->transfer_b, &completion,
                        &now_ms, 100U) == 0);
        TEST_ASSERT(completion.status ==
                    UCN_TRANSFER_COMPLETION_DELIVERED);
        handles[slot_index] = pair->received.last_handle;
        TEST_ASSERT(handles[slot_index] != UCN_TRANSFER_RX_HANDLE_DIRECT);
    }

    (void)memset(&completion, 0, sizeof(completion));
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T128, TRANSFER_TEST_DATA, 128U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 100U) == 0);
    TEST_ASSERT(completion.status ==
                UCN_TRANSFER_COMPLETION_REMOTE_REJECTED);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_b)->rx_slot_full > 0U);
    for (slot_index = 0U; slot_index < UCN_TRANSFER_RX_SLOTS; ++slot_index) {
        TEST_ASSERT(ucn_transfer_release_received(
                        &pair->transfer_b, handles[slot_index]) == UCN_OK);
    }

    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 128U,
                                  UCN_TRANSFER_CLASS_T128, false);
    (void)memset(&completion, 0, sizeof(completion));
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T128, TRANSFER_TEST_DATA, 128U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 100U) == 0);
    second_handle = pair->received.last_handle;
    TEST_ASSERT(second_handle != handles[0]);
    TEST_ASSERT(ucn_transfer_release_received(
                    &pair->transfer_b, handles[0]) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_transfer_release_received(
                    &pair->transfer_b, second_handle) == UCN_OK);

    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 128U,
                                  UCN_TRANSFER_CLASS_T128, true);
    (void)memset(&completion, 0, sizeof(completion));
    pair->context_ba.drop_all_acks = true;
    pair->context_ba.drop_ack_offset = 128U;
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T128, TRANSFER_TEST_DATA, 128U,
                    transfer_test_complete, &completion) == UCN_OK);
    while (completion.count == 0U) {
        now_ms += 10U;
        {
            ucn_result_t result =
                transfer_test_step_at(&pair->transfer_a, now_ms);

            TEST_ASSERT(result == UCN_OK || result == UCN_ERR_TOO_LARGE ||
                        completion.count == 1U);
        }
        TEST_ASSERT(transfer_test_step_at(&pair->transfer_b, now_ms) ==
                    UCN_ERR_NOT_FOUND);
    }
    pair->context_ba.drop_all_acks = false;
    pair->context_ba.drop_ack_offset = 0U;
    TEST_ASSERT(completion.status ==
                UCN_TRANSFER_COMPLETION_RETRY_EXHAUSTED);
    TEST_ASSERT(pair->received.count == 1U);
    return 0;
}

static int transfer_test_authoritative_clock(void)
{
    transfer_test_pair_t *pair = &MTU_PAIR;
    transfer_test_completion_state_t completion;
    ucn_transfer_config_t invalid_config;
    ucn_transfer_t invalid_transfer;
    uint32_t now_ms = UINT32_C(100000);

    if (UCN_TRANSFER_MAX_MESSAGE_BYTES < 128U) {
        return 0;
    }
    TEST_ASSERT(transfer_test_init_pair(pair, 128U) == 0);
    pair->now_ms = now_ms;
    (void)memset(&invalid_config, 0, sizeof(invalid_config));
    (void)memset(&invalid_transfer, 0, sizeof(invalid_transfer));
    invalid_config.node = &pair->a;
    TEST_ASSERT(ucn_transfer_init(&invalid_transfer, &invalid_config) ==
                UCN_ERR_ARGUMENT);

    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 128U,
                                  UCN_TRANSFER_CLASS_T128, true);
    (void)memset(&completion, 0, sizeof(completion));
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T128, TRANSFER_TEST_DATA, 128U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 100U) == 0);
    TEST_ASSERT(completion.status == UCN_TRANSFER_COMPLETION_DELIVERED);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_a)->fragments_sent > 0U);

    /* A previously valid cached time must not be reused after a long idle
     * interval.  Send samples the same authoritative callback again. */
    now_ms = UINT32_C(500000);
    pair->now_ms = now_ms;
    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 128U,
                                  UCN_TRANSFER_CLASS_T128, true);
    (void)memset(&completion, 0, sizeof(completion));
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T128, TRANSFER_TEST_DATA, 128U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 100U) == 0);
    TEST_ASSERT(completion.status == UCN_TRANSFER_COMPLETION_DELIVERED);
    TEST_ASSERT(pair->received.count == 1U && !pair->received.mismatch);
    return 0;
}

static int transfer_test_window_pipeline_and_gap_recovery(void)
{
    transfer_test_pair_t *pair = &WINDOW_PAIR;
    transfer_test_completion_state_t completion;
    const ucn_transfer_stats_t *sender_stats;
    const ucn_transfer_stats_t *receiver_stats;
    uint32_t now_ms = 0U;

    if (UCN_TRANSFER_MAX_WINDOW < 4U ||
        UCN_TRANSFER_MAX_MESSAGE_BYTES < 512U) {
        return 0;
    }

    /* A window-capable sender must fall back to stop-and-wait until the peer
     * explicitly advertises a larger receive window.  This preserves the v5
     * behavior of existing peers without changing the wire format. */
    TEST_ASSERT(transfer_test_init_pair_window(pair, 256U, 4U) == 0);
    TEST_ASSERT(ucn_transfer_set_peer_window_capability(
                    &pair->transfer_a, UINT32_C(2), 1U) == UCN_OK);
    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 512U,
                                  UCN_TRANSFER_CLASS_T512, true);
    (void)memset(&completion, 0, sizeof(completion));
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T512, TRANSFER_TEST_DATA, 512U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 200U) == 0);
    TEST_ASSERT(completion.status == UCN_TRANSFER_COMPLETION_DELIVERED);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_a)
                    ->fragments_in_flight_peak == 1U);

    /* Losing the START fragment exercises the receiver's no-slot cumulative
     * ACK and the sender's bounded Go-Back-N recovery. */
    TEST_ASSERT(transfer_test_init_pair_window(pair, 256U, 4U) == 0);
    TEST_ASSERT(ucn_transfer_set_peer_window_capability(
                    &pair->transfer_a, UINT32_C(99), 4U) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_transfer_set_peer_window_capability(
                    &pair->transfer_a, UINT32_C(2), 0U) ==
                UCN_ERR_ARGUMENT);
    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 512U,
                                  UCN_TRANSFER_CLASS_T512, true);
    (void)memset(&completion, 0, sizeof(completion));
    pair->context_ab.drop_first_fragment = true;
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T512, TRANSFER_TEST_DATA, 512U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 200U) == 0);
    TEST_ASSERT(completion.status == UCN_TRANSFER_COMPLETION_DELIVERED);
    TEST_ASSERT(pair->context_ab.dropped_fragments == 1U);
    TEST_ASSERT(pair->received.count == 1U);
    TEST_ASSERT(!pair->received.mismatch);
    sender_stats = ucn_transfer_get_stats(&pair->transfer_a);
    receiver_stats = ucn_transfer_get_stats(&pair->transfer_b);
    TEST_ASSERT(sender_stats->fragments_in_flight_peak >= 2U);
    TEST_ASSERT(sender_stats->window_recovery_rounds >= 1U);
    TEST_ASSERT(sender_stats->fragments_retried >= 1U);
    TEST_ASSERT(receiver_stats->fragments_duplicate_or_out_of_order >= 1U);

    /* Losing a middle fragment exercises the same repair after a receiver
     * slot already exists and later fragments arrive out of order. */
    now_ms = 0U;
    TEST_ASSERT(transfer_test_init_pair_window(pair, 256U, 4U) == 0);
    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 512U,
                                  UCN_TRANSFER_CLASS_T512, true);
    (void)memset(&completion, 0, sizeof(completion));
    pair->context_ab.drop_first_nonzero_fragment = true;
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T512, TRANSFER_TEST_DATA, 512U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 200U) == 0);
    TEST_ASSERT(completion.status == UCN_TRANSFER_COMPLETION_DELIVERED);
    TEST_ASSERT(pair->context_ab.dropped_fragments == 1U);
    TEST_ASSERT(pair->received.count == 1U);
    TEST_ASSERT(!pair->received.mismatch);
    sender_stats = ucn_transfer_get_stats(&pair->transfer_a);
    receiver_stats = ucn_transfer_get_stats(&pair->transfer_b);
    TEST_ASSERT(sender_stats->fragments_in_flight_peak >= 2U);
    TEST_ASSERT(sender_stats->window_recovery_rounds >= 1U);
    TEST_ASSERT(sender_stats->fragments_retried >= 1U);
    TEST_ASSERT(receiver_stats->fragments_duplicate_or_out_of_order >= 1U);
    return 0;
}

static int transfer_test_integrity_failure(void)
{
    transfer_test_pair_t *pair = &DIRECT_PAIR;
    ucn_transfer_fragment_t fragment;
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
    size_t payload_length;
    uint32_t failed_before =
        ucn_transfer_get_stats(&pair->transfer_b)->integrity_failed;
    uint32_t expired_before;

    if (UCN_TRANSFER_MAX_MESSAGE_BYTES < 128U) {
        return 0;
    }

    (void)memset(&fragment, 0, sizeof(fragment));
    fragment.target_endpoint = TRANSFER_TEST_ENDPOINT;
    fragment.transfer_class = UCN_TRANSFER_CLASS_T128;
    fragment.flags = UCN_TRANSFER_FLAG_START | UCN_TRANSFER_FLAG_END;
    fragment.transfer_id = UINT16_C(0x7A01);
    fragment.total_length = 16U;
    fragment.message_crc32 = UINT32_C(0x12345678);
    fragment.data = TRANSFER_TEST_DATA;
    fragment.data_length = 16U;
    TEST_ASSERT(ucn_transfer_encode_fragment(
                    &fragment, payload, sizeof(payload), &payload_length) ==
                UCN_OK);
    TEST_ASSERT(ucn_node_send(&pair->a, UINT32_C(2),
                              UCN_MSG_TRANSFER_FRAGMENT,
                              UCN_TRAFFIC_Q1_REALTIME, payload,
                              (uint16_t)payload_length) == UCN_OK);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_b)->integrity_failed ==
                failed_before + 1U);

    expired_before = ucn_transfer_get_stats(&pair->transfer_b)->rx_expired;
    fragment.flags = UCN_TRANSFER_FLAG_START;
    fragment.transfer_id++;
    fragment.total_length = 128U;
    fragment.message_crc32 =
        ucn_transfer_crc32(TRANSFER_TEST_DATA, 128U);
    fragment.data_length = 16U;
    TEST_ASSERT(ucn_transfer_encode_fragment(
                    &fragment, payload, sizeof(payload), &payload_length) ==
                UCN_OK);
    TEST_ASSERT(ucn_node_send(&pair->a, UINT32_C(2),
                              UCN_MSG_TRANSFER_FRAGMENT,
                              UCN_TRAFFIC_Q1_REALTIME, payload,
                              (uint16_t)payload_length) == UCN_OK);
    TEST_ASSERT(transfer_test_step_at(&pair->transfer_b, UINT32_C(100000)) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_transfer_get_stats(&pair->transfer_b)->rx_expired ==
                expired_before + 1U);
    return 0;
}

static int transfer_test_plain_rejection(void)
{
    transfer_test_pair_t *pair = &DIRECT_PAIR;
    transfer_test_completion_state_t completion;
    uint32_t now_ms = UINT32_C(101000);

    if (UCN_TRANSFER_MAX_MESSAGE_BYTES < 128U) {
        return 0;
    }
    TEST_ASSERT(ucn_transfer_bind_endpoint(
                    &pair->transfer_b, TRANSFER_TEST_ENDPOINT,
                    transfer_test_local_max_class(), true,
                    transfer_test_receive, &pair->received) == UCN_OK);
    TEST_ASSERT(transfer_test_step_at(&pair->transfer_a, now_ms) ==
                UCN_ERR_NOT_FOUND);
    transfer_test_prepare_receive(&pair->received, TRANSFER_TEST_DATA, 128U,
                                  UCN_TRANSFER_CLASS_T128, true);
    (void)memset(&completion, 0, sizeof(completion));
    TEST_ASSERT(ucn_transfer_send(
                    &pair->transfer_a, UINT32_C(2), TRANSFER_TEST_ENDPOINT,
                    UCN_TRANSFER_CLASS_T128, TRANSFER_TEST_DATA, 128U,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &pair->transfer_a, &pair->transfer_b, &completion,
                    &now_ms, 20U) == 0);
    TEST_ASSERT(completion.status ==
                UCN_TRANSFER_COMPLETION_REMOTE_REJECTED);
    TEST_ASSERT(pair->received.count == 0U);
    TEST_ASSERT(ucn_transfer_bind_endpoint(
                    &pair->transfer_b, TRANSFER_TEST_ENDPOINT,
                    transfer_test_local_max_class(), false,
                    transfer_test_receive, &pair->received) == UCN_OK);
    return 0;
}

static int transfer_test_multihop(void)
{
    static ucn_node_t a;
    static ucn_node_t b;
    static ucn_node_t c;
    static ucn_transfer_t transfer_a;
    static ucn_transfer_t transfer_c;
    static ucn_link_t ab;
    static ucn_link_t ba;
    static ucn_link_t bc;
    static ucn_link_t cb;
    static transfer_test_link_context_t context_ab;
    static transfer_test_link_context_t context_ba;
    static transfer_test_link_context_t context_bc;
    static transfer_test_link_context_t context_cb;
    transfer_test_receive_state_t received;
    transfer_test_completion_state_t completion;
    ucn_transfer_config_t config;
    uint32_t now_ms = 0U;
    ucn_transfer_class_t transfer_class;
    uint16_t transfer_length;

    if (UCN_TRANSFER_MAX_MESSAGE_BYTES < 128U) {
        return 0;
    }
    transfer_class = transfer_test_local_max_class();
    if (transfer_class > UCN_TRANSFER_CLASS_T512) {
        transfer_class = UCN_TRANSFER_CLASS_T512;
    }
    transfer_length = (uint16_t)ucn_transfer_class_max_bytes(transfer_class);

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c));
    (void)memset(&transfer_a, 0, sizeof(transfer_a));
    (void)memset(&transfer_c, 0, sizeof(transfer_c));
    (void)memset(&received, 0, sizeof(received));
    (void)memset(&completion, 0, sizeof(completion));
    TEST_ASSERT(transfer_test_init_node(&a, UINT32_C(11)) == 0);
    TEST_ASSERT(transfer_test_init_node(&b, UINT32_C(12)) == 0);
    TEST_ASSERT(transfer_test_init_node(&c, UINT32_C(13)) == 0);

    transfer_test_init_link(&ab, &context_ab, 11U, 128U, UINT32_C(12),
                            &b, &ba);
    transfer_test_init_link(&ba, &context_ba, 12U, 128U, UINT32_C(11),
                            &a, &ab);
    transfer_test_init_link(&bc, &context_bc, 13U, 128U, UINT32_C(13),
                            &c, &cb);
    transfer_test_init_link(&cb, &context_cb, 14U, 128U, UINT32_C(12),
                            &b, &bc);
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(13), &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&b, UINT32_C(13), &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&c, UINT32_C(11), &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&b, UINT32_C(11), &ba) == UCN_OK);

    (void)memset(&config, 0, sizeof(config));
    config.node = &a;
    config.now_ms = transfer_test_now_ms;
    config.now_context = &now_ms;
    config.ack_timeout_ms = 10U;
    TEST_ASSERT(ucn_transfer_init(&transfer_a, &config) == UCN_OK);
    config.node = &c;
    TEST_ASSERT(ucn_transfer_init(&transfer_c, &config) == UCN_OK);
    received.owner = &transfer_c;
    transfer_test_prepare_receive(&received, TRANSFER_TEST_DATA,
                                  transfer_length, transfer_class, true);
    TEST_ASSERT(ucn_transfer_bind_endpoint(
                    &transfer_c, TRANSFER_TEST_ENDPOINT,
                    transfer_test_local_max_class(), false,
                    transfer_test_receive,
                    &received) == UCN_OK);
    TEST_ASSERT(ucn_transfer_set_peer_capability(
                    &transfer_a, UINT32_C(13),
                    transfer_test_local_max_class()) == UCN_OK);
    TEST_ASSERT(ucn_transfer_send(
                    &transfer_a, UINT32_C(13), TRANSFER_TEST_ENDPOINT,
                    transfer_class, TRANSFER_TEST_DATA, transfer_length,
                    transfer_test_complete, &completion) == UCN_OK);
    TEST_ASSERT(transfer_test_run_until_complete(
                    &transfer_a, &transfer_c, &completion, &now_ms,
                    200U) == 0);
    TEST_ASSERT(completion.status == UCN_TRANSFER_COMPLETION_DELIVERED);
    TEST_ASSERT(received.count == 1U);
    TEST_ASSERT(!received.mismatch);
    TEST_ASSERT(received.release_result == UCN_OK);
    return 0;
}

static int transfer_test_mtu_matrix(void)
{
    static const size_t mtus[] = { 64U, 250U, 256U };
    size_t index;

    if (UCN_TRANSFER_MAX_MESSAGE_BYTES < 128U) {
        return 0;
    }
    for (index = 0U; index < sizeof(mtus) / sizeof(mtus[0]); ++index) {
        transfer_test_completion_state_t completion;
        uint32_t now_ms = 0U;

        TEST_ASSERT(transfer_test_init_pair(&MTU_PAIR, mtus[index]) == 0);
        transfer_test_prepare_receive(&MTU_PAIR.received, TRANSFER_TEST_DATA,
                                      128U, UCN_TRANSFER_CLASS_T128, true);
        (void)memset(&completion, 0, sizeof(completion));
        TEST_ASSERT(ucn_transfer_send(
                        &MTU_PAIR.transfer_a, UINT32_C(2),
                        TRANSFER_TEST_ENDPOINT, UCN_TRANSFER_CLASS_T128,
                        TRANSFER_TEST_DATA, 128U, transfer_test_complete,
                        &completion) == UCN_OK);
        TEST_ASSERT(transfer_test_run_until_complete(
                        &MTU_PAIR.transfer_a, &MTU_PAIR.transfer_b,
                        &completion, &now_ms, 100U) == 0);
        TEST_ASSERT(completion.status ==
                    UCN_TRANSFER_COMPLETION_DELIVERED);
        TEST_ASSERT(MTU_PAIR.received.count == 1U);
        TEST_ASSERT(!MTU_PAIR.received.mismatch);
    }

    {
        transfer_test_completion_state_t completion;
        uint32_t now_ms = 0U;

        TEST_ASSERT(transfer_test_init_pair(&MTU_PAIR, 32U) == 0);
        transfer_test_prepare_receive(&MTU_PAIR.received, TRANSFER_TEST_DATA,
                                      128U, UCN_TRANSFER_CLASS_T128, true);
        (void)memset(&completion, 0, sizeof(completion));
        TEST_ASSERT(ucn_transfer_send(
                        &MTU_PAIR.transfer_a, UINT32_C(2),
                        TRANSFER_TEST_ENDPOINT, UCN_TRANSFER_CLASS_T128,
                        TRANSFER_TEST_DATA, 128U, transfer_test_complete,
                        &completion) == UCN_OK);
        TEST_ASSERT(transfer_test_run_until_complete(
                        &MTU_PAIR.transfer_a, &MTU_PAIR.transfer_b,
                        &completion, &now_ms, 20U) == 0);
        TEST_ASSERT(completion.status ==
                    UCN_TRANSFER_COMPLETION_SEND_FAILED);
        TEST_ASSERT(MTU_PAIR.received.count == 0U);
    }
    return 0;
}

int test_transfer(void)
{
    TEST_ASSERT(transfer_test_codec() == 0);
    if (UCN_MAX_PAYLOAD_BYTES <
        UCN_TRANSFER_FRAGMENT_HEADER_BYTES +
            UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES) {
        return 0;
    }
    TEST_ASSERT(transfer_test_all_classes() == 0);
    TEST_ASSERT(transfer_test_authoritative_clock() == 0);
    TEST_ASSERT(transfer_test_retry_and_slot_lifetime() == 0);
    TEST_ASSERT(transfer_test_window_pipeline_and_gap_recovery() == 0);
    TEST_ASSERT(transfer_test_integrity_failure() == 0);
    TEST_ASSERT(transfer_test_plain_rejection() == 0);
    TEST_ASSERT(transfer_test_mtu_matrix() == 0);
    TEST_ASSERT(transfer_test_multihop() == 0);
    return 0;
}
