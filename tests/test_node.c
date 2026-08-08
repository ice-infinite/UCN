#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct test_link_context {
    bool is_up;
} test_link_context_t;

static ucn_result_t test_link_send(ucn_link_t *link,
                                   const uint8_t *frame,
                                   size_t length)
{
    (void)link;
    (void)frame;
    (void)length;
    return UCN_OK;
}

static ucn_result_t test_link_status(const ucn_link_t *link,
                                     ucn_link_status_t *status)
{
    const test_link_context_t *context = (const test_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t TEST_LINK_OPS = {
    NULL,
    test_link_send,
    NULL,
    test_link_status,
    NULL,
    NULL
};

int test_node(void)
{
    ucn_config_t config;
    ucn_node_t node;
    ucn_link_t link;
    test_link_context_t context;

    (void)memset(&config, 0, sizeof(config));
    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    config.network_id = UINT32_C(0xAABBCCDD);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 3U;
    context.is_up = true;

    link.ops = &TEST_LINK_OPS;
    link.context = &context;
    link.link_id = 1U;
    link.mtu = UCN_MAX_FRAME_BYTES;
    link.peer_node_id = UINT32_C(2);

    TEST_ASSERT(ucn_node_init(NULL, &config) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_node_send(&node, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, NULL, 0U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_send(&node, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q2_NORMAL, NULL, 0U) == UCN_ERR_UNSUPPORTED);

    context.is_up = false;
    TEST_ASSERT(ucn_node_send(&node, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, NULL, 0U) == UCN_ERR_LINK_DOWN);
    return 0;
}
