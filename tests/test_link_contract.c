#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct link_contract_context {
    ucn_result_t send_result;
    uint32_t send_calls;
    uint8_t no_space_remaining;
} link_contract_context_t;

static ucn_result_t contract_send(ucn_link_t *link,
                                  const uint8_t *frame,
                                  size_t length)
{
    link_contract_context_t *context = (link_contract_context_t *)link->context;

    (void)frame;
    (void)length;
    context->send_calls++;
    if (context->no_space_remaining != 0U) {
        context->no_space_remaining--;
        return UCN_ERR_NO_SPACE;
    }
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

static int contract_init_node(ucn_node_t *node,
                              ucn_link_t *link,
                              link_contract_context_t *context)
{
    ucn_config_t config;

    (void)memset(node, 0, sizeof(*node));
    (void)memset(link, 0, sizeof(*link));
    (void)memset(context, 0, sizeof(*context));
    config.network_id = UINT32_C(0x4C494E4B);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 4U;
    if (ucn_node_init(node, &config) != UCN_OK) {
        return 1;
    }
    link->ops = &CONTRACT_OPS;
    link->context = context;
    link->link_id = 1U;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = UINT32_C(2);
    return ucn_node_register_link(node, link) == UCN_OK ? 0 : 1;
}

static void contract_build_q0_request(ucn_send_request_t *request,
                                      const uint8_t *payload,
                                      ucn_delivery_semantic_t delivery,
                                      uint32_t deadline_ms)
{
    (void)memset(request, 0, sizeof(*request));
    request->destination = UINT32_C(2);
    request->message_type = UCN_MSG_DATA_Q0;
    request->traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    request->delivery = delivery;
    request->deadline_ms = deadline_ms;
    request->payload = payload;
    request->payload_length = 1U;
}

static int test_direct_link_contract(void)
{
    uint8_t payload = 0x5AU;
    ucn_node_t node;
    ucn_link_t link;
    link_contract_context_t context;

    TEST_ASSERT(contract_init_node(&node, &link, &context) == 0);

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

static int test_core_q0_backpressure_recovery(void)
{
    const uint8_t payload = 0x61U;
    ucn_node_t node;
    ucn_link_t link;
    link_contract_context_t context;
    ucn_send_request_t request;

    TEST_ASSERT(contract_init_node(&node, &link, &context) == 0);
    context.send_result = UCN_OK;
    context.no_space_remaining = 2U;
    contract_build_q0_request(&request, &payload,
                              UCN_DELIVERY_RETRY_ON_BACKPRESSURE, 100U);
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_OK);

    TEST_ASSERT(ucn_node_step(&node, 1U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(context.send_calls == 1U && node.q0[0].occupied);
    TEST_ASSERT(node.stats.q0_backpressure_retries == 1U &&
                node.stats.tx_error_dropped == 0U);
    TEST_ASSERT(ucn_node_step(&node, 4U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(context.send_calls == 1U && node.q0[0].occupied);
    TEST_ASSERT(ucn_node_step(&node, 6U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(context.send_calls == 2U && node.q0[0].occupied);
    TEST_ASSERT(ucn_node_step(&node, 11U) == UCN_OK);
    TEST_ASSERT(context.send_calls == 3U && !node.q0[0].occupied);
    TEST_ASSERT(node.stats.tx_sent == 1U && node.stats.tx_error_dropped == 0U &&
                node.stats.q0_backpressure_retries == 2U &&
                node.stats.q0_backpressure_exhausted == 0U);

    request.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    request.message_type = UCN_MSG_DATA_Q1;
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_ERR_ARGUMENT);
    request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    request.message_type = UCN_MSG_DATA_Q0;
    request.deadline_ms = 0U;
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_ERR_ARGUMENT);
    return 0;
}

static int test_core_q0_backpressure_final_paths(void)
{
    const uint8_t payload = 0x62U;
    ucn_node_t node;
    ucn_link_t link;
    link_contract_context_t context;
    ucn_send_request_t request;

    /* Default BEST_EFFORT remains one attempt and releases ownership. */
    TEST_ASSERT(contract_init_node(&node, &link, &context) == 0);
    context.send_result = UCN_ERR_NO_SPACE;
    contract_build_q0_request(&request, &payload,
                              UCN_DELIVERY_BEST_EFFORT, 100U);
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node, 1U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(!node.q0[0].occupied && context.send_calls == 1U &&
                node.stats.tx_error_dropped == 1U &&
                node.stats.q0_backpressure_retries == 0U);

    /* Initial attempt plus the fixed number of retries produces one final
     * drop, never one drop per transient attempt. */
    TEST_ASSERT(contract_init_node(&node, &link, &context) == 0);
    context.send_result = UCN_ERR_NO_SPACE;
    contract_build_q0_request(&request, &payload,
                              UCN_DELIVERY_RETRY_ON_BACKPRESSURE, 100U);
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node, 1U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_node_step(&node, 6U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_node_step(&node, 11U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_node_step(&node, 16U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(!node.q0[0].occupied && context.send_calls == 4U &&
                node.stats.q0_backpressure_retries == 3U &&
                node.stats.q0_backpressure_exhausted == 1U &&
                node.stats.tx_error_dropped == 1U);

    /* A terminal error is never retried and is counted exactly once. */
    TEST_ASSERT(contract_init_node(&node, &link, &context) == 0);
    context.send_result = UCN_ERR_LINK_DOWN;
    contract_build_q0_request(&request, &payload,
                              UCN_DELIVERY_RETRY_ON_BACKPRESSURE, 100U);
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node, 1U) == UCN_ERR_LINK_DOWN);
    TEST_ASSERT(!node.q0[0].occupied && context.send_calls == 1U &&
                node.stats.q0_backpressure_retries == 0U &&
                node.stats.q0_backpressure_terminal_failed == 1U &&
                node.stats.tx_error_dropped == 1U);

    /* An item whose retry time has not arrived remains FIFO-owned, then
     * expires without turning its earlier transient attempt into a drop. */
    TEST_ASSERT(contract_init_node(&node, &link, &context) == 0);
    context.send_result = UCN_ERR_NO_SPACE;
    contract_build_q0_request(&request, &payload,
                              UCN_DELIVERY_RETRY_ON_BACKPRESSURE, 7U);
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node, 1U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_node_step(&node, 7U) == UCN_ERR_TTL);
    TEST_ASSERT(!node.q0[0].occupied && context.send_calls == 1U &&
                node.stats.q0_backpressure_expired == 1U &&
                node.stats.tx_expired_dropped == 1U &&
                node.stats.tx_error_dropped == 0U);
    return 0;
}

int test_link_contract(void)
{
    return test_direct_link_contract() |
           test_core_q0_backpressure_recovery() |
           test_core_q0_backpressure_final_paths();
}
