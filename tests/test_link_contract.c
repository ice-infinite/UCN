#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct link_contract_context {
    ucn_result_t send_result;
    uint32_t send_calls;
    uint32_t route_request_calls;
    uint32_t q0_data_calls;
    uint32_t q0_data_accepted;
    uint8_t no_space_remaining;
} link_contract_context_t;

static ucn_result_t contract_send(ucn_link_t *link,
                                  const uint8_t *frame,
                                  size_t length)
{
    link_contract_context_t *context = (link_contract_context_t *)link->context;
    ucn_frame_t decoded;
    ucn_result_t result;

    context->send_calls++;
    if (context->no_space_remaining != 0U) {
        context->no_space_remaining--;
        result = UCN_ERR_NO_SPACE;
    } else {
        result = context->send_result;
    }
    if (ucn_frame_decode(frame, length, &decoded) == UCN_OK) {
        if (decoded.message_type == UCN_MSG_ROUTE_REQ) {
            context->route_request_calls++;
        } else if (decoded.message_type == UCN_MSG_DATA_Q0) {
            context->q0_data_calls++;
            if (result == UCN_OK) {
                context->q0_data_accepted++;
            }
        }
    }
    return result;
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

static int contract_init_node_for_peer(ucn_node_t *node,
                                       ucn_link_t *link,
                                       link_contract_context_t *context,
                                       ucn_node_id_t peer_node_id)
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
    link->peer_node_id = peer_node_id;
    return ucn_node_register_link(node, link) == UCN_OK ? 0 : 1;
}

static int contract_init_node(ucn_node_t *node,
                              ucn_link_t *link,
                              link_contract_context_t *context)
{
    return contract_init_node_for_peer(node, link, context, UINT32_C(2));
}

#if UCN_FEATURE_DYNAMIC_MESH
static void contract_install_route(ucn_node_t *node,
                                   ucn_link_t *link,
                                   ucn_node_id_t destination,
                                   uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (!node->routes[index].valid ||
            node->routes[index].destination == destination) {
            ucn_route_entry_t *route = &node->routes[index];

            (void)memset(route, 0, sizeof(*route));
            route->valid = true;
            route->route_origin = node->config.node_id;
            route->destination = destination;
            route->egress_link = link;
            route->expires_at_ms =
                ucn_deadline_from_now(now_ms, UCN_ROUTE_ENTRY_LIFETIME_MS);
            route->last_used_at_ms = now_ms;
            route->route_cost = 2U;
            route->hop_count = 2U;
            route->route_epoch = 1U;
            return;
        }
    }
}

static size_t contract_active_discoveries(const ucn_node_t *node,
                                          ucn_node_id_t destination)
{
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination) {
            count++;
        }
    }
    return count;
}
#endif

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

#if UCN_FEATURE_DYNAMIC_MESH
static int test_core_q0_route_wait_recovery(void)
{
    const uint8_t payload = 0x63U;
    ucn_node_t node;
    ucn_link_t link;
    link_contract_context_t context;
    ucn_send_request_t request;
    const uint32_t first_attempt_ms = 10U;
    const uint32_t retry_ms =
        first_attempt_ms + UCN_Q0_ROUTE_WAIT_RETRY_INTERVAL_MS;
    const uint32_t recovered_retry_ms =
        retry_ms + UCN_Q0_ROUTE_WAIT_RETRY_INTERVAL_MS;

    TEST_ASSERT(contract_init_node_for_peer(&node, &link, &context,
                                            UINT32_C(3)) == 0);
    context.send_result = UCN_OK;
    /* A make-before-break candidate refresh for the same destination already
     * owns the discovery domain. Q0 must join it rather than allocate or emit
     * another ordinary discovery. */
    node.discoveries[0].active = true;
    node.discoveries[0].destination = UINT32_C(2);
    node.discoveries[0].overall_started_at_ms = first_attempt_ms;
    node.discoveries[0].started_at_ms = first_attempt_ms;
    node.discoveries[0].deadline_ms = ucn_deadline_from_now(
        first_attempt_ms, UCN_ROUTE_RING_TIMEOUT_MS);
    node.discoveries[0].current_hop_limit = 1U;
    node.discoveries[0].maximum_hop_limit = 4U;
#if UCN_FEATURE_CANDIDATE_ROUTING
    node.discoveries[0].is_candidate = true;
#endif

    contract_build_q0_request(&request, &payload,
                              UCN_DELIVERY_RETRY_ON_BACKPRESSURE, 1000U);
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node, first_attempt_ms) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(node.q0[0].occupied && node.q0[0].waiting_for_route &&
                node.q0[0].deadline_ms == 1000U &&
                context.q0_data_calls == 0U &&
                context.route_request_calls == 0U &&
                contract_active_discoveries(&node, UINT32_C(2)) == 1U);
    TEST_ASSERT(node.stats.q0_route_wait_started == 1U &&
                node.stats.q0_route_wait_retried == 0U &&
                node.stats.q0_backpressure_retries == 0U &&
                node.stats.tx_error_dropped == 0U);

    TEST_ASSERT(ucn_node_step(&node, retry_ms) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(node.q0[0].occupied && node.q0[0].waiting_for_route &&
                context.q0_data_calls == 0U &&
                context.route_request_calls == 0U &&
                contract_active_discoveries(&node, UINT32_C(2)) == 1U &&
                node.stats.q0_route_wait_retried == 1U);

    contract_install_route(&node, &link, UINT32_C(2), recovered_retry_ms);
    TEST_ASSERT(ucn_node_step(&node, recovered_retry_ms - 1U) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(node.q0[0].occupied && context.q0_data_calls == 0U);
    TEST_ASSERT(ucn_node_step(&node, recovered_retry_ms) == UCN_OK);
    TEST_ASSERT(!node.q0[0].occupied && context.q0_data_calls == 1U &&
                context.q0_data_accepted == 1U &&
                node.stats.q0_route_wait_started == 1U &&
                node.stats.q0_route_wait_retried == 1U &&
                node.stats.q0_route_wait_recovered == 1U &&
                node.stats.q0_route_wait_terminal_failed == 0U &&
                node.stats.tx_error_dropped == 0U);
    return 0;
}

static int test_core_q0_route_wait_wrap_expiry(void)
{
    const uint8_t payload = 0x64U;
    const uint32_t start_ms = UINT32_MAX - UINT32_C(20);
    const uint32_t deadline_ms = ucn_deadline_from_now(
        start_ms, UCN_Q0_ROUTE_WAIT_RETRY_INTERVAL_MS + UINT32_C(1));
    ucn_node_t node;
    ucn_link_t link;
    link_contract_context_t context;
    ucn_send_request_t request;

    TEST_ASSERT(contract_init_node_for_peer(&node, &link, &context,
                                            UINT32_C(3)) == 0);
    context.send_result = UCN_OK;
    contract_build_q0_request(&request, &payload,
                              UCN_DELIVERY_RETRY_ON_BACKPRESSURE,
                              deadline_ms);
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node, start_ms) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(node.q0[0].occupied && node.q0[0].waiting_for_route &&
                context.route_request_calls == 1U &&
                contract_active_discoveries(&node, UINT32_C(2)) == 1U);
    TEST_ASSERT(ucn_node_step(&node, deadline_ms) == UCN_ERR_TTL);
    TEST_ASSERT(!node.q0[0].occupied && context.q0_data_calls == 0U &&
                node.stats.q0_route_wait_started == 1U &&
                node.stats.q0_route_wait_expired == 1U &&
                node.stats.q0_backpressure_expired == 0U &&
                node.stats.tx_expired_dropped == 1U &&
                node.stats.tx_error_dropped == 0U);
    return 0;
}

static int test_core_q0_route_wait_then_backpressure(void)
{
    const uint8_t payload = 0x65U;
    ucn_node_t node;
    ucn_link_t link;
    link_contract_context_t context;
    ucn_send_request_t request;
    const uint32_t first_attempt_ms = 20U;
    const uint32_t route_retry_ms =
        first_attempt_ms + UCN_Q0_ROUTE_WAIT_RETRY_INTERVAL_MS;
    const uint32_t backpressure_retry_ms =
        route_retry_ms + UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS;

    TEST_ASSERT(contract_init_node_for_peer(&node, &link, &context,
                                            UINT32_C(3)) == 0);
    context.send_result = UCN_OK;
    contract_build_q0_request(&request, &payload,
                              UCN_DELIVERY_RETRY_ON_BACKPRESSURE, 1000U);
    TEST_ASSERT(ucn_node_enqueue(&node, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&node, first_attempt_ms) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(node.q0[0].occupied && node.q0[0].waiting_for_route &&
                node.q0[0].backpressure_retries == 0U &&
                context.route_request_calls == 1U);

    contract_install_route(&node, &link, UINT32_C(2), route_retry_ms);
    context.no_space_remaining = 1U;
    TEST_ASSERT(ucn_node_step(&node, route_retry_ms) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.q0[0].occupied && !node.q0[0].waiting_for_route &&
                node.q0[0].backpressure_retries == 1U &&
                node.stats.q0_route_wait_recovered == 1U &&
                node.stats.q0_backpressure_retries == 1U &&
                node.stats.q0_route_wait_terminal_failed == 0U &&
                node.stats.q0_backpressure_terminal_failed == 0U);

    TEST_ASSERT(ucn_node_step(&node, backpressure_retry_ms) == UCN_OK);
    TEST_ASSERT(!node.q0[0].occupied && context.q0_data_calls == 2U &&
                context.q0_data_accepted == 1U &&
                node.stats.q0_route_wait_recovered == 1U &&
                node.stats.q0_backpressure_retries == 1U &&
                node.stats.tx_error_dropped == 0U);
    return 0;
}
#endif

int test_link_contract(void)
{
    return test_direct_link_contract() |
           test_core_q0_backpressure_recovery() |
           test_core_q0_backpressure_final_paths()
#if UCN_FEATURE_DYNAMIC_MESH
           | test_core_q0_route_wait_recovery()
           | test_core_q0_route_wait_wrap_expiry()
           | test_core_q0_route_wait_then_backpressure()
#endif
        ;
}
