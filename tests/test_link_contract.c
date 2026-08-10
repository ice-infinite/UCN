#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct link_contract_context {
    ucn_result_t send_result;
    uint32_t send_calls;
} link_contract_context_t;

static ucn_result_t contract_send(ucn_link_t *link,
                                  const uint8_t *frame,
                                  size_t length)
{
    link_contract_context_t *context = (link_contract_context_t *)link->context;

    (void)frame;
    (void)length;
    context->send_calls++;
    return context->send_result;
}

static ucn_result_t contract_status(const ucn_link_t *link,
                                    ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t CONTRACT_OPS = {
    NULL, contract_send, NULL, contract_status, NULL, NULL
};

int test_link_contract(void)
{
    uint8_t payload = 0x5AU;
    ucn_config_t config;
    ucn_node_t node;
    ucn_link_t link;
    link_contract_context_t context;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    (void)memset(&context, 0, sizeof(context));
    config.network_id = UINT32_C(0x4C494E4B);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 4U;
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    link.ops = &CONTRACT_OPS;
    link.context = &context;
    link.link_id = 1U;
    link.mtu = UCN_MAX_FRAME_BYTES;
    link.peer_node_id = UINT32_C(2);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);

    context.send_result = UCN_ERR_NO_SPACE;
    TEST_ASSERT(ucn_node_send(&node, UINT32_C(2), UCN_MSG_DATA_Q0,
                              UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(context.send_calls == 1U);

    context.send_result = UCN_ERR_LINK_DOWN;
    TEST_ASSERT(ucn_node_send(&node, UINT32_C(2), UCN_MSG_DATA_Q0,
                              UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) ==
                UCN_ERR_LINK_DOWN);
    TEST_ASSERT(context.send_calls == 2U);

    context.send_result = UCN_OK;
    TEST_ASSERT(ucn_node_send(&node, UINT32_C(2), UCN_MSG_DATA_Q0,
                              UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) == UCN_OK);
    TEST_ASSERT(context.send_calls == 3U);
    return 0;
}
