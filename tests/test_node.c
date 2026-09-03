#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct test_link_context {
    bool is_up;
    size_t send_count;
} test_link_context_t;

static ucn_result_t test_link_send(ucn_link_t *link,
                                   const uint8_t *frame,
                                   size_t length)
{
    test_link_context_t *context = (test_link_context_t *)link->context;

    (void)frame;
    (void)length;
    context->send_count++;
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
    static const int invalid_traffic_classes[] = {
        -1,
        -255,
        -256,
        (int)UCN_TRAFFIC_CLASS_COUNT
    };
    ucn_config_t config;
    ucn_node_t node;
    ucn_link_t link;
    test_link_context_t context;
    size_t index;

    (void)memset(&config, 0, sizeof(config));
    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    config.network_id = UINT32_C(0xAABBCCDD);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 3U;
    context.is_up = true;
    context.send_count = 0U;

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
                              UCN_TRAFFIC_Q2_NORMAL, NULL, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&node, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q3_BULK, NULL, 0U) == UCN_OK);
    TEST_ASSERT(context.send_count == 2U);
    for (index = 0U;
         index < sizeof(invalid_traffic_classes) /
                     sizeof(invalid_traffic_classes[0]);
         ++index) {
        const ucn_traffic_class_t invalid_class =
            (ucn_traffic_class_t)invalid_traffic_classes[index];

        TEST_ASSERT(ucn_node_send(
                        &node, UINT32_C(2), UCN_MSG_DATA_Q1,
                        invalid_class, NULL, 0U) == UCN_ERR_UNSUPPORTED);
        TEST_ASSERT(ucn_node_send_endpoint(
                        &node, UINT32_C(2), 0x40U, invalid_class,
                        NULL, 0U) == UCN_ERR_UNSUPPORTED);
        TEST_ASSERT(context.send_count == 2U);
    }
    TEST_ASSERT(node.stats.tx_sent_by_class[UCN_TRAFFIC_Q2_NORMAL] == 1U &&
                node.stats.tx_sent_by_class[UCN_TRAFFIC_Q3_BULK] == 1U);
    TEST_ASSERT(node.stats.tx_queue_sent_by_class[UCN_TRAFFIC_Q2_NORMAL] == 0U &&
                node.stats.tx_queue_sent_by_class[UCN_TRAFFIC_Q3_BULK] == 0U);

    context.is_up = false;
    TEST_ASSERT(ucn_node_send(&node, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, NULL, 0U) == UCN_ERR_LINK_DOWN);
    return 0;
}
