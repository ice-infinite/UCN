#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct hello_link_context {
    ucn_node_t *peer_node;
    ucn_link_t *peer_ingress_link;
    bool is_up;
    ucn_result_t last_receive_result;
} hello_link_context_t;

typedef struct hello_receive_state {
    uint32_t count;
    ucn_node_id_t source;
    uint8_t message_type;
    uint8_t payload[8];
    uint16_t payload_length;
} hello_receive_state_t;

static ucn_result_t hello_link_send(ucn_link_t *link,
                                    const uint8_t *frame,
                                    size_t length)
{
    hello_link_context_t *context = (hello_link_context_t *)link->context;

    if (context->peer_node == NULL || context->peer_ingress_link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    context->last_receive_result = ucn_node_receive(context->peer_node,
                                                    context->peer_ingress_link,
                                                    frame, length);
    return context->last_receive_result;
}

static ucn_result_t hello_link_status(const ucn_link_t *link,
                                      ucn_link_status_t *status)
{
    const hello_link_context_t *context = (const hello_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t HELLO_LINK_OPS = {
    NULL, hello_link_send, NULL, hello_link_status, NULL, NULL
};

static void hello_init_config(ucn_config_t *config, ucn_node_id_t node_id)
{
    config->network_id = UINT32_C(42);
    config->node_id = node_id;
    config->default_hop_limit = 3U;
}

static void hello_setup_link(ucn_link_t *link,
                             hello_link_context_t *context,
                             uint8_t link_id,
                             ucn_node_id_t peer_node_id)
{
    link->ops = &HELLO_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
    context->is_up = true;
}

static void hello_receive_callback(void *context, const ucn_frame_t *frame)
{
    hello_receive_state_t *state = (hello_receive_state_t *)context;

    state->count++;
    state->source = frame->source;
    state->message_type = frame->message_type;
    state->payload_length = frame->payload_length;
    if (frame->payload_length != 0U) {
        (void)memcpy(state->payload, frame->payload, frame->payload_length);
    }
}

static ucn_result_t encode_hello(ucn_network_id_t network_id,
                                 ucn_node_id_t source,
                                 ucn_node_id_t destination,
                                 const uint8_t *payload,
                                 uint16_t payload_length,
                                 ucn_sequence_t sequence,
                                 uint8_t *encoded,
                                 size_t encoded_capacity,
                                 size_t *encoded_length)
{
    ucn_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_HELLO;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = 1U;
    frame.network_id = network_id;
    frame.source = source;
    frame.destination = destination;
    frame.sequence = sequence;
    frame.payload = payload;
    frame.payload_length = payload_length;
    return ucn_frame_encode(&frame, encoded, encoded_capacity, encoded_length);
}

static ucn_result_t encode_data(ucn_network_id_t network_id,
                                ucn_node_id_t source,
                                ucn_node_id_t destination,
                                ucn_sequence_t sequence,
                                uint8_t *encoded,
                                size_t encoded_capacity,
                                size_t *encoded_length)
{
    ucn_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_DATA_Q1;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.hop_limit = 1U;
    frame.network_id = network_id;
    frame.source = source;
    frame.destination = destination;
    frame.sequence = sequence;
    return ucn_frame_encode(&frame, encoded, encoded_capacity, encoded_length);
}

int test_hello_join(void)
{
    static const uint8_t payload[] = { 0x5AU, 0xA5U };
    static const uint8_t hello_w3[] = { UCN_WIRE_PROFILE_W3_BACKBONE };
    static const uint8_t bad_hello_payload[] = { 0U, 0U, 0U, 2U };
    static const uint8_t bad_hello_profile[] = { UINT8_C(99) };
    ucn_node_t node_a, node_b, node_c;
    ucn_config_t config_a, config_b, config_c;
    ucn_link_t link_a_to_b, link_b_to_a, candidate_ingress;
    hello_link_context_t context_a_to_b, context_b_to_a, candidate_context;
    hello_receive_state_t receive_a, receive_b;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;

    (void)memset(&node_a, 0, sizeof(node_a));
    (void)memset(&node_b, 0, sizeof(node_b));
    (void)memset(&node_c, 0, sizeof(node_c));
    (void)memset(&link_a_to_b, 0, sizeof(link_a_to_b));
    (void)memset(&link_b_to_a, 0, sizeof(link_b_to_a));
    (void)memset(&candidate_ingress, 0, sizeof(candidate_ingress));
    (void)memset(&context_a_to_b, 0, sizeof(context_a_to_b));
    (void)memset(&context_b_to_a, 0, sizeof(context_b_to_a));
    (void)memset(&candidate_context, 0, sizeof(candidate_context));
    (void)memset(&receive_a, 0, sizeof(receive_a));
    (void)memset(&receive_b, 0, sizeof(receive_b));
    hello_init_config(&config_a, UINT32_C(1));
    hello_init_config(&config_b, UINT32_C(2));
    hello_init_config(&config_c, UINT32_C(3));

    TEST_ASSERT(ucn_node_init(&node_a, &config_a) == UCN_OK);
    TEST_ASSERT(ucn_node_init(&node_b, &config_b) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node_a, UCN_WIRE_PROFILE_W0_LOCAL,
                    UCN_WIRE_PROFILE_W3_BACKBONE) == UCN_OK);
    TEST_ASSERT(ucn_node_set_join_policy(&node_a, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_node_set_join_policy(&node_b, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
    hello_setup_link(&link_a_to_b, &context_a_to_b, 1U, 0U);
    hello_setup_link(&link_b_to_a, &context_b_to_a, 2U, 0U);
    context_a_to_b.peer_node = &node_b;
    context_a_to_b.peer_ingress_link = &link_b_to_a;
    context_b_to_a.peer_node = &node_a;
    context_b_to_a.peer_ingress_link = &link_a_to_b;

    /* Neither side knows the remote Node ID.  Each Adapter broadcasts from
     * an unknown physical Link, then Core binds it only after HELLO. */
    TEST_ASSERT(ucn_node_broadcast_hello(&node_a, &link_a_to_b, 100U) == UCN_OK);
    TEST_ASSERT(context_a_to_b.last_receive_result == UCN_OK);
    TEST_ASSERT(node_b.link_count == 1U);
    TEST_ASSERT(link_b_to_a.peer_node_id == config_a.node_id);
    TEST_ASSERT(link_b_to_a.peer_wire_profile ==
                UCN_WIRE_PROFILE_W3_BACKBONE);
    TEST_ASSERT(ucn_node_neighbor_count(&node_b, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(ucn_node_broadcast_hello(&node_b, &link_b_to_a, 101U) == UCN_OK);
    TEST_ASSERT(context_b_to_a.last_receive_result == UCN_OK);
    TEST_ASSERT(node_a.link_count == 1U);
    TEST_ASSERT(link_a_to_b.peer_node_id == config_b.node_id);
    TEST_ASSERT(link_a_to_b.peer_wire_profile ==
                UCN_WIRE_PROFILE_W3_BACKBONE);
    TEST_ASSERT(ucn_node_neighbor_count(&node_a, UCN_NEIGHBOR_ADMITTED) == 1U);

    ucn_node_set_rx_handler(&node_b, hello_receive_callback, &receive_b);
    TEST_ASSERT(ucn_node_send(&node_a, config_b.node_id, UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, payload,
                              (uint16_t)sizeof(payload)) == UCN_OK);
    TEST_ASSERT(receive_b.count == 1U);
    TEST_ASSERT(receive_b.source == config_a.node_id);
    TEST_ASSERT(receive_b.message_type == UCN_MSG_DATA_Q1);
    TEST_ASSERT(receive_b.payload_length == sizeof(payload));
    TEST_ASSERT(memcmp(receive_b.payload, payload, sizeof(payload)) == 0);
    ucn_node_set_rx_handler(&node_a, hello_receive_callback, &receive_a);
    TEST_ASSERT(ucn_node_send(&node_b, config_a.node_id, UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, payload,
                              (uint16_t)sizeof(payload)) == UCN_OK);
    TEST_ASSERT(receive_a.count == 1U);
    TEST_ASSERT(receive_a.source == config_b.node_id);
    TEST_ASSERT(receive_a.payload_length == sizeof(payload));

    TEST_ASSERT(ucn_node_init(&node_c, &config_c) == UCN_OK);
    TEST_ASSERT(ucn_node_set_join_policy(&node_c, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
    hello_setup_link(&candidate_ingress, &candidate_context, 3U, 0U);
    TEST_ASSERT(encode_data(config_c.network_id, config_a.node_id, config_c.node_id,
                            1U, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&node_c, &candidate_ingress, encoded,
                                 encoded_length) == UCN_ERR_ACCESS);
    TEST_ASSERT(ucn_node_neighbor_count(&node_c, UCN_NEIGHBOR_CANDIDATE) == 0U);

    TEST_ASSERT(encode_hello(config_c.network_id, config_a.node_id,
                             config_c.node_id, bad_hello_payload,
                             (uint16_t)sizeof(bad_hello_payload), 2U,
                             encoded, sizeof(encoded),
                             &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&node_c, &candidate_ingress, encoded,
                                 encoded_length) == UCN_ERR_MALFORMED);
    TEST_ASSERT(candidate_ingress.peer_node_id == 0U);
    TEST_ASSERT(ucn_node_neighbor_count(&node_c, UCN_NEIGHBOR_CANDIDATE) == 0U);

    TEST_ASSERT(encode_hello(config_c.network_id + 1U, config_a.node_id,
                             config_c.node_id, hello_w3,
                             (uint16_t)sizeof(hello_w3), 3U, encoded,
                             sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&node_c, &candidate_ingress, encoded,
                                 encoded_length) == UCN_ERR_NETWORK);

    TEST_ASSERT(encode_hello(config_c.network_id, config_a.node_id,
                             config_c.node_id, bad_hello_profile,
                             (uint16_t)sizeof(bad_hello_profile), 4U,
                             encoded, sizeof(encoded),
                             &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&node_c, &candidate_ingress, encoded,
                                 encoded_length) == UCN_ERR_MALFORMED);
    TEST_ASSERT(candidate_ingress.peer_node_id == 0U);

    TEST_ASSERT(encode_hello(config_c.network_id, config_a.node_id,
                             config_c.node_id, hello_w3,
                             (uint16_t)sizeof(hello_w3), 5U,
                             encoded, sizeof(encoded),
                             &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&node_c, &candidate_ingress, encoded,
                                 encoded_length) == UCN_OK);
    TEST_ASSERT(candidate_ingress.peer_node_id == config_a.node_id);
    TEST_ASSERT(node_c.link_count == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&node_c, UCN_NEIGHBOR_ADMITTED) == 1U);
    return 0;
}
