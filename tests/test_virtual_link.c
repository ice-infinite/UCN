#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct virtual_link_context {
    ucn_node_t *peer_node;
    ucn_link_t *peer_ingress_link;
    bool is_up;
    ucn_result_t last_receive_result;
} virtual_link_context_t;

typedef struct receive_state {
    uint32_t count;
    ucn_node_id_t source;
    uint8_t message_type;
    uint8_t payload[16];
    uint16_t payload_length;
} receive_state_t;

static ucn_result_t virtual_link_send(ucn_link_t *link,
                                      const uint8_t *frame,
                                      size_t length)
{
    virtual_link_context_t *context = (virtual_link_context_t *)link->context;

    context->last_receive_result = ucn_node_receive(context->peer_node,
                                                    context->peer_ingress_link,
                                                    frame,
                                                    length);
    return context->last_receive_result;
}

static ucn_result_t virtual_link_status(const ucn_link_t *link,
                                        ucn_link_status_t *status)
{
    const virtual_link_context_t *context = (const virtual_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t VIRTUAL_LINK_OPS = {
    NULL,
    virtual_link_send,
    NULL,
    virtual_link_status,
    NULL,
    NULL
};

static void receive_callback(void *context, const ucn_frame_t *frame)
{
    receive_state_t *state = (receive_state_t *)context;

    state->count++;
    state->source = frame->source;
    state->message_type = frame->message_type;
    state->payload_length = frame->payload_length;
    if (frame->payload_length != 0U) {
        (void)memcpy(state->payload, frame->payload, frame->payload_length);
    }
}

static void init_config(ucn_config_t *config, ucn_node_id_t node_id)
{
    config->network_id = UINT32_C(0xAABBCCDD);
    config->node_id = node_id;
    config->default_hop_limit = 3U;
}

int test_virtual_link(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xB2U, 0xC3U };
    ucn_node_t node_a;
    ucn_node_t node_b;
    ucn_config_t config_a;
    ucn_config_t config_b;
    ucn_link_t link_a_to_b;
    ucn_link_t link_b_to_a;
    virtual_link_context_t context_a_to_b;
    virtual_link_context_t context_b_to_a;
    receive_state_t receive_b;

    (void)memset(&node_a, 0, sizeof(node_a));
    (void)memset(&node_b, 0, sizeof(node_b));
    (void)memset(&link_a_to_b, 0, sizeof(link_a_to_b));
    (void)memset(&link_b_to_a, 0, sizeof(link_b_to_a));
    (void)memset(&context_a_to_b, 0, sizeof(context_a_to_b));
    (void)memset(&context_b_to_a, 0, sizeof(context_b_to_a));
    (void)memset(&receive_b, 0, sizeof(receive_b));
    init_config(&config_a, UINT32_C(1));
    init_config(&config_b, UINT32_C(2));

    TEST_ASSERT(ucn_node_init(&node_a, &config_a) == UCN_OK);
    TEST_ASSERT(ucn_node_init(&node_b, &config_b) == UCN_OK);

    link_a_to_b.ops = &VIRTUAL_LINK_OPS;
    link_a_to_b.context = &context_a_to_b;
    link_a_to_b.link_id = 1U;
    link_a_to_b.mtu = UCN_MAX_FRAME_BYTES;
    link_a_to_b.peer_node_id = config_b.node_id;

    link_b_to_a.ops = &VIRTUAL_LINK_OPS;
    link_b_to_a.context = &context_b_to_a;
    link_b_to_a.link_id = 2U;
    link_b_to_a.mtu = UCN_MAX_FRAME_BYTES;
    link_b_to_a.peer_node_id = config_a.node_id;

    context_a_to_b.peer_node = &node_b;
    context_a_to_b.peer_ingress_link = &link_b_to_a;
    context_a_to_b.is_up = true;
    context_b_to_a.peer_node = &node_a;
    context_b_to_a.peer_ingress_link = &link_a_to_b;
    context_b_to_a.is_up = true;

    TEST_ASSERT(ucn_node_register_link(&node_a, &link_a_to_b) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_b, &link_b_to_a) == UCN_OK);
    ucn_node_set_rx_handler(&node_b, receive_callback, &receive_b);

    TEST_ASSERT(ucn_node_send(&node_a, config_b.node_id, UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME,
                              payload, (uint16_t)sizeof(payload)) == UCN_OK);
    TEST_ASSERT(context_a_to_b.last_receive_result == UCN_OK);
    TEST_ASSERT(receive_b.count == 1U);
    TEST_ASSERT(receive_b.source == config_a.node_id);
    TEST_ASSERT(receive_b.message_type == UCN_MSG_DATA_Q1);
    TEST_ASSERT(receive_b.payload_length == sizeof(payload));
    TEST_ASSERT(memcmp(receive_b.payload, payload, sizeof(payload)) == 0);
    return 0;
}
