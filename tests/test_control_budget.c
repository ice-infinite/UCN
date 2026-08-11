#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct budget_link_context {
    bool is_up;
    uint32_t send_count;
} budget_link_context_t;

static void budget_write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static ucn_result_t budget_link_send(ucn_link_t *link,
                                     const uint8_t *frame,
                                     size_t length)
{
    budget_link_context_t *context = (budget_link_context_t *)link->context;

    (void)frame;
    (void)length;
    context->send_count++;
    return UCN_OK;
}

static ucn_result_t budget_link_status(const ucn_link_t *link,
                                       ucn_link_status_t *status)
{
    const budget_link_context_t *context =
        (const budget_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t BUDGET_LINK_OPS = {
    NULL, budget_link_send, NULL, budget_link_status, NULL, NULL
};

#if UCN_FEATURE_DIAGNOSTICS
static bool budget_trace_authorize(void *context, ucn_node_id_t requester)
{
    const bool *allow = (const bool *)context;

    return *allow && requester == UINT32_C(2);
}
#endif

static ucn_result_t budget_inject(ucn_node_t *node,
                                  ucn_link_t *ingress,
                                  ucn_frame_t *frame)
{
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_result_t result;

    result = ucn_frame_encode(frame, encoded, sizeof(encoded), &encoded_length);
    if (result != UCN_OK) {
        return result;
    }
    return ucn_node_receive(node, ingress, encoded, encoded_length);
}

int test_control_budget(void)
{
    ucn_config_t config;
    ucn_node_t node;
    ucn_link_t ingress, egress;
    budget_link_context_t ingress_context, egress_context;
    ucn_frame_t frame;
    uint8_t heartbeat[8] = { 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    uint8_t route_request[14] = { 0U };
#if UCN_FEATURE_DIAGNOSTICS
    uint8_t trace[UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES + sizeof(ucn_node_id_t)] = { 0U };
    bool allow_trace = false;
#endif
    uint32_t index;
    uint32_t duplicate_frames_before_rreq;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&ingress, 0, sizeof(ingress));
    (void)memset(&egress, 0, sizeof(egress));
    (void)memset(&ingress_context, 0, sizeof(ingress_context));
    (void)memset(&egress_context, 0, sizeof(egress_context));
    config.network_id = UINT32_C(0x4354524C);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 4U;
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    ingress_context.is_up = true;
    egress_context.is_up = true;
    ingress.ops = &BUDGET_LINK_OPS;
    ingress.context = &ingress_context;
    ingress.link_id = 1U;
    ingress.mtu = UCN_MAX_FRAME_BYTES;
    ingress.peer_node_id = UINT32_C(2);
    egress.ops = &BUDGET_LINK_OPS;
    egress.context = &egress_context;
    egress.link_id = 2U;
    egress.mtu = UCN_MAX_FRAME_BYTES;
    egress.peer_node_id = UINT32_C(3);
    TEST_ASSERT(ucn_node_register_link(&node, &ingress) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node, &egress) == UCN_OK);
    TEST_ASSERT(ucn_node_observe_neighbor(&node, &ingress, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_observe_neighbor(&node, &egress, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_admit_neighbor(&node, UINT32_C(2)) == UCN_OK);
    TEST_ASSERT(ucn_node_admit_neighbor(&node, UINT32_C(3)) == UCN_OK);

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_HEARTBEAT;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = 1U;
    frame.network_id = config.network_id;
    frame.source = UINT32_C(2);
    frame.destination = UINT32_C(1);
    frame.payload = heartbeat;
    frame.payload_length = (uint16_t)sizeof(heartbeat);
    for (index = 0U; index < UCN_HEARTBEAT_RX_TOKEN_BURST; ++index) {
        frame.sequence = UINT32_C(10) + index;
        budget_write_u32(heartbeat + 4U, index + 1U);
        TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_OK);
    }
    frame.sequence++;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.heartbeat_rx_rate_dropped == 1U);
    frame.source = UINT32_C(3);
    frame.sequence = UINT32_C(90);
    TEST_ASSERT(budget_inject(&node, &egress, &frame) == UCN_OK);
    frame.source = UINT32_C(2);
    node.now_ms = UCN_HEARTBEAT_RX_TOKEN_REFILL_MS;
    frame.sequence++;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_OK);
    duplicate_frames_before_rreq = node.stats.duplicate_frames_dropped;

    (void)memset(&frame, 0, sizeof(frame));
    budget_write_u32(route_request, UINT32_C(99));
    frame.message_type = UCN_MSG_ROUTE_REQ;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = 4U;
    frame.network_id = config.network_id;
    frame.source = UINT32_C(2);
    frame.destination = UCN_NODE_BROADCAST;
    frame.payload = route_request;
    frame.payload_length = (uint16_t)sizeof(route_request);
    budget_write_u32(route_request + 8U, 100U);
    budget_write_u32(route_request + 4U, UINT32_C(1));
    route_request[12] = 1U;
    frame.sequence = UINT32_C(100);
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_OK);
    for (index = 0U; index < 3U; ++index) {
        frame.sequence++;
        TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_ERR_REPLAY);
    }
    TEST_ASSERT(node.stats.route_request_replayed == 3U);
    TEST_ASSERT(node.stats.duplicate_frames_dropped ==
                duplicate_frames_before_rreq);
    for (index = 1U; index < UCN_ROUTE_REQUEST_RX_TOKEN_BURST; ++index) {
        budget_write_u32(route_request + 4U, index + 1U);
        frame.sequence++;
        TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_OK);
    }
    budget_write_u32(route_request + 4U, UINT32_C(100));
    frame.sequence++;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.route_request_rx_rate_dropped == 1U);

    /* A better copy is classified before the Token but committed only after
     * the Token succeeds.  Exhaustion must not poison the Best Cost state. */
    budget_write_u32(route_request + 4U, UINT32_C(1));
    budget_write_u32(route_request + 8U, 50U);
    frame.sequence++;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.route_request_rx_rate_dropped == 2U);
    node.now_ms += UCN_ROUTE_REQUEST_RX_TOKEN_REFILL_MS;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_OK);

    budget_write_u32(route_request + 8U, 100U);
    budget_write_u32(route_request + 4U, UINT32_C(101));
    frame.source = UINT32_C(3);
    frame.sequence = UINT32_C(190);
    TEST_ASSERT(budget_inject(&node, &egress, &frame) == UCN_OK);
    frame.source = UINT32_C(2);
    node.now_ms += UCN_ROUTE_REQUEST_RX_TOKEN_REFILL_MS;
    frame.sequence++;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_OK);

#if UCN_FEATURE_DIAGNOSTICS
    (void)memset(&frame, 0, sizeof(frame));
    budget_write_u32(trace, UINT32_C(1));
    trace[4] = 1U;
    trace[5] = 3U;
    trace[6] = (uint8_t)UCN_PATH_TRACE_STATUS_OK;
    budget_write_u32(trace + UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES, UINT32_C(2));
    frame.message_type = UCN_MSG_PATH_TRACE_REQ;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = 4U;
    frame.network_id = config.network_id;
    frame.source = UINT32_C(2);
    frame.destination = UINT32_C(1);
    frame.sequence = UINT32_C(200);
    frame.payload = trace;
    frame.payload_length = (uint16_t)sizeof(trace);
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_ERR_ACCESS);
    TEST_ASSERT(node.stats.path_trace_rejected == 1U &&
                node.stats.path_trace_rx_rate_dropped == 0U);
    allow_trace = true;
    TEST_ASSERT(ucn_node_set_path_trace_authorizer(&node, budget_trace_authorize,
                                                    &allow_trace) == UCN_OK);
    frame.sequence++;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_OK);
    frame.sequence++;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.path_trace_rx_rate_dropped == 1U);
    node.now_ms += UCN_PATH_TRACE_RX_TOKEN_REFILL_MS;
    frame.sequence++;
    TEST_ASSERT(budget_inject(&node, &ingress, &frame) == UCN_OK);
#endif
    return 0;
}
