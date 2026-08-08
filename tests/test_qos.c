#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct qos_virtual_link_context {
    ucn_node_t *peer_node;
    ucn_link_t *peer_ingress_link;
} qos_virtual_link_context_t;

typedef struct qos_receive_state {
    uint32_t count;
    uint8_t message_types[4];
    uint8_t payloads[4];
} qos_receive_state_t;

static ucn_result_t qos_link_send(ucn_link_t *link,
                                  const uint8_t *frame,
                                  size_t length)
{
    qos_virtual_link_context_t *context = (qos_virtual_link_context_t *)link->context;

    return ucn_node_receive(context->peer_node, context->peer_ingress_link, frame, length);
}

static ucn_result_t qos_link_status(const ucn_link_t *link,
                                    ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t QOS_LINK_OPS = {
    NULL,
    qos_link_send,
    NULL,
    qos_link_status,
    NULL,
    NULL
};

static void qos_receive_callback(void *context, const ucn_frame_t *frame)
{
    qos_receive_state_t *state = (qos_receive_state_t *)context;
    const uint32_t index = state->count;

    if (index < 4U) {
        state->message_types[index] = frame->message_type;
        state->payloads[index] = frame->payload_length == 0U ? 0U : frame->payload[0];
    }
    state->count++;
}

static void init_node_config(ucn_config_t *config, ucn_node_id_t node_id)
{
    config->network_id = UINT32_C(0xAABBCCDD);
    config->node_id = node_id;
    config->default_hop_limit = 3U;
}

int test_qos(void)
{
    uint8_t q1_old = 0x11U;
    uint8_t q1_new = 0x22U;
    uint8_t q0 = 0x33U;
    ucn_node_t node_a;
    ucn_node_t node_b;
    ucn_config_t config_a;
    ucn_config_t config_b;
    ucn_link_t link_a_to_b;
    ucn_link_t link_b_to_a;
    qos_virtual_link_context_t context_a_to_b;
    qos_virtual_link_context_t context_b_to_a;
    qos_receive_state_t received;
    ucn_send_request_t request;

    (void)memset(&node_a, 0, sizeof(node_a));
    (void)memset(&node_b, 0, sizeof(node_b));
    (void)memset(&link_a_to_b, 0, sizeof(link_a_to_b));
    (void)memset(&link_b_to_a, 0, sizeof(link_b_to_a));
    (void)memset(&context_a_to_b, 0, sizeof(context_a_to_b));
    (void)memset(&context_b_to_a, 0, sizeof(context_b_to_a));
    (void)memset(&received, 0, sizeof(received));
    init_node_config(&config_a, UINT32_C(1));
    init_node_config(&config_b, UINT32_C(2));
    TEST_ASSERT(ucn_node_init(&node_a, &config_a) == UCN_OK);
    TEST_ASSERT(ucn_node_init(&node_b, &config_b) == UCN_OK);

    link_a_to_b.ops = &QOS_LINK_OPS;
    link_a_to_b.context = &context_a_to_b;
    link_a_to_b.link_id = 1U;
    link_a_to_b.mtu = UCN_MAX_FRAME_BYTES;
    link_a_to_b.peer_node_id = config_b.node_id;
    link_b_to_a.ops = &QOS_LINK_OPS;
    link_b_to_a.context = &context_b_to_a;
    link_b_to_a.link_id = 2U;
    link_b_to_a.mtu = UCN_MAX_FRAME_BYTES;
    link_b_to_a.peer_node_id = config_a.node_id;
    context_a_to_b.peer_node = &node_b;
    context_a_to_b.peer_ingress_link = &link_b_to_a;
    context_b_to_a.peer_node = &node_a;
    context_b_to_a.peer_ingress_link = &link_a_to_b;
    TEST_ASSERT(ucn_node_register_link(&node_a, &link_a_to_b) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_b, &link_b_to_a) == UCN_OK);
    ucn_node_set_rx_handler(&node_b, qos_receive_callback, &received);

    (void)memset(&request, 0, sizeof(request));
    request.destination = config_b.node_id;
    request.message_type = UCN_MSG_DATA_Q1;
    request.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    request.delivery = UCN_DELIVERY_LATEST_VALUE;
    request.payload = &q1_old;
    request.payload_length = 1U;
    TEST_ASSERT(ucn_node_enqueue(&node_a, &request) == UCN_OK);

    request.message_type = UCN_MSG_DATA_Q0;
    request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    request.delivery = UCN_DELIVERY_BEST_EFFORT;
    request.payload = &q0;
    TEST_ASSERT(ucn_node_enqueue(&node_a, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node_a, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node_a, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 2U);
    TEST_ASSERT(received.message_types[0] == UCN_MSG_DATA_Q0);
    TEST_ASSERT(received.payloads[0] == q0);
    TEST_ASSERT(received.message_types[1] == UCN_MSG_DATA_Q1);
    TEST_ASSERT(received.payloads[1] == q1_old);

    request.message_type = UCN_MSG_DATA_Q1;
    request.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    request.delivery = UCN_DELIVERY_LATEST_VALUE;
    request.payload = &q1_old;
    TEST_ASSERT(ucn_node_enqueue(&node_a, &request) == UCN_OK);
    request.payload = &q1_new;
    TEST_ASSERT(ucn_node_enqueue(&node_a, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node_a, 2U) == UCN_OK);
    TEST_ASSERT(received.count == 3U);
    TEST_ASSERT(received.payloads[2] == q1_new);

    request.delivery = UCN_DELIVERY_BEST_EFFORT;
    request.deadline_ms = 3U;
    TEST_ASSERT(ucn_node_enqueue(&node_a, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node_a, 3U) == UCN_ERR_TTL);
    TEST_ASSERT(received.count == 3U);
    TEST_ASSERT(ucn_node_get_stats(&node_a)->tx_expired_dropped == 1U);
    return 0;
}
