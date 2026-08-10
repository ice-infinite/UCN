#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct policy_diagnostic_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool is_up;
    ucn_link_metrics_t metrics;
    uint32_t send_count;
    uint8_t last_message_type;
    uint8_t last_flags;
    ucn_traffic_class_t last_traffic_class;
    size_t last_length;
} policy_diagnostic_link_context_t;

typedef struct policy_diagnostic_callback_state {
    uint32_t count;
    ucn_policy_diagnostic_result_t result;
} policy_diagnostic_callback_state_t;

static ucn_result_t policy_diagnostic_link_send(ucn_link_t *link,
                                                 const uint8_t *frame,
                                                 size_t length)
{
    policy_diagnostic_link_context_t *context =
        (policy_diagnostic_link_context_t *)link->context;
    ucn_frame_t decoded;

    context->send_count++;
    context->last_length = length;
    if (ucn_frame_decode(frame, length, &decoded) == UCN_OK) {
        context->last_message_type = decoded.message_type;
        context->last_flags = decoded.flags;
        context->last_traffic_class = decoded.traffic_class;
    }
    if (!context->is_up || context->peer == NULL || context->peer_ingress == NULL) {
        return UCN_ERR_LINK_DOWN;
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t policy_diagnostic_link_status(const ucn_link_t *link,
                                                   ucn_link_status_t *status)
{
    const policy_diagnostic_link_context_t *context =
        (const policy_diagnostic_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static ucn_result_t policy_diagnostic_link_metrics(const ucn_link_t *link,
                                                    ucn_link_metrics_t *metrics)
{
    const policy_diagnostic_link_context_t *context =
        (const policy_diagnostic_link_context_t *)link->context;

    *metrics = context->metrics;
    return UCN_OK;
}

static const ucn_link_ops_t POLICY_DIAGNOSTIC_LINK_OPS = {
    NULL, policy_diagnostic_link_send, NULL, policy_diagnostic_link_status,
    NULL, policy_diagnostic_link_metrics
};

static int policy_diagnostic_init_node(ucn_node_t *node, ucn_node_id_t node_id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0xD1A60001);
    config.node_id = node_id;
    config.default_hop_limit = 4U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void policy_diagnostic_setup_link(
    ucn_link_t *link,
    policy_diagnostic_link_context_t *context,
    uint8_t link_id,
    ucn_node_id_t peer_node_id)
{
    link->ops = &POLICY_DIAGNOSTIC_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
    context->is_up = true;
}

static bool policy_diagnostic_allow_origin(void *context,
                                           ucn_node_id_t requester)
{
    return requester == *(const ucn_node_id_t *)context;
}

static void policy_diagnostic_callback(
    void *context,
    const ucn_policy_diagnostic_result_t *result)
{
    policy_diagnostic_callback_state_t *state =
        (policy_diagnostic_callback_state_t *)context;

    state->count++;
    state->result = *result;
}

static int policy_diagnostic_step(ucn_node_t *node, uint32_t now_ms)
{
    const ucn_result_t result = ucn_node_step(node, now_ms);

    return result == UCN_OK || result == UCN_ERR_NOT_FOUND ? 0 : 1;
}

static int policy_diagnostic_query(
    ucn_node_t *origin,
    ucn_node_t *target,
    ucn_policy_diagnostic_section_t section,
    uint8_t index,
    policy_diagnostic_callback_state_t *callback)
{
    origin->policy_diagnostic_tokens = UCN_POLICY_DIAGNOSTIC_TOKEN_BURST;
    target->policy_diagnostic_tokens = UCN_POLICY_DIAGNOSTIC_TOKEN_BURST;
    if (ucn_node_request_policy_diagnostic(origin, target->config.node_id,
                                           section, index,
                                           policy_diagnostic_callback,
                                           callback) != UCN_OK) {
        return 1;
    }
    if (policy_diagnostic_step(origin, origin->now_ms) != 0 ||
        policy_diagnostic_step(target, target->now_ms) != 0) {
        return 1;
    }
    return 0;
}

int test_policy_diagnostic(void)
{
    const ucn_node_id_t node_a_id = UINT32_C(1);
    const ucn_node_id_t node_b_id = UINT32_C(2);
    ucn_node_t a, b;
    ucn_link_t ab, ba;
    policy_diagnostic_link_context_t cab, cba;
    policy_diagnostic_callback_state_t callback;
    ucn_route_policy_config_t policy;
    ucn_policy_path_config_t path;
    ucn_send_request_t q0;
    ucn_result_t result;
    uint8_t payload = 0x5AU;
    size_t index;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&cab, 0, sizeof(cab)); (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&callback, 0, sizeof(callback));
    TEST_ASSERT(policy_diagnostic_init_node(&a, node_a_id) == 0);
    TEST_ASSERT(policy_diagnostic_init_node(&b, node_b_id) == 0);
    policy_diagnostic_setup_link(&ab, &cab, 1U, node_b_id);
    policy_diagnostic_setup_link(&ba, &cba, 2U, node_a_id);
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    cba.metrics.route_cost_valid = true;
    cba.metrics.route_cost = 37U;
    cba.metrics.rtt_valid = true;
    cba.metrics.rtt_ms = 9U;
    cba.metrics.tx_failure_rate_valid = true;
    cba.metrics.tx_failure_per_mille = 13U;
    cba.metrics.queue_pressure_valid = true;
    cba.metrics.queue_pressure_per_mille = 21U;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_set_policy_diagnostic_authorizer(
                    &b, policy_diagnostic_allow_origin, (void *)&node_a_id) == UCN_OK);
    TEST_ASSERT(policy_diagnostic_step(&b, 0U) == 0);

    (void)memset(&policy, 0, sizeof(policy));
    policy.key.destination = node_a_id;
    policy.key.endpoint = 0x40U;
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q1_REALTIME;
    policy.mode = UCN_ROUTE_POLICY_AUTO_BEST;
    TEST_ASSERT(ucn_node_set_route_policy(&b, &policy) == UCN_OK);
    (void)memset(&path, 0, sizeof(path));
    path.local_path_id = 7U;
    path.destination = node_a_id;
    path.egress_link = &ba;
    path.verified = true;
    TEST_ASSERT(ucn_node_set_policy_path(&b, &path) == UCN_OK);
    TEST_ASSERT(ucn_node_bind_q1_flow(&b, node_a_id, 0x40U, 7U, 5000U) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&b, node_a_id, 0x40U,
                                       UCN_TRAFFIC_Q1_REALTIME,
                                       &payload, 1U) == UCN_OK);
    TEST_ASSERT(b.policy_state.policies[0].match_hits == 1U);

    /* The first request shares neither the Q0 queue nor the Q0 budget.  A
     * queued Q0 business frame must leave first; only the following step may
     * emit the diagnostics request. */
    TEST_ASSERT(ucn_node_request_policy_diagnostic(
                    &a, node_b_id, UCN_POLICY_DIAGNOSTIC_POLICY, 0U,
                    policy_diagnostic_callback, &callback) == UCN_OK);
    TEST_ASSERT(ucn_node_request_policy_diagnostic(
                    &a, node_b_id, UCN_POLICY_DIAGNOSTIC_POLICY, 1U,
                    policy_diagnostic_callback, &callback) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(a.stats.policy_diagnostic_rate_dropped == 1U);
    (void)memset(&q0, 0, sizeof(q0));
    q0.destination = node_b_id;
    q0.message_type = 0x40U;
    q0.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    q0.delivery = UCN_DELIVERY_BEST_EFFORT;
    q0.deadline_ms = 500U;
    q0.payload = &payload;
    q0.payload_length = 1U;
    TEST_ASSERT(ucn_node_enqueue(&a, &q0) == UCN_OK);
    TEST_ASSERT(policy_diagnostic_step(&a, 0U) == 0);
    TEST_ASSERT(cab.last_message_type == 0x40U && callback.count == 0U);
    TEST_ASSERT(policy_diagnostic_step(&a, 0U) == 0);
    TEST_ASSERT(cab.last_message_type == UCN_MSG_POLICY_DIAGNOSTIC_REQ &&
                cab.last_flags == UCN_FRAME_FLAG_DIAGNOSTIC &&
                cab.last_traffic_class == UCN_TRAFFIC_Q1_REALTIME &&
                cab.last_length == UCN_FRAME_HEADER_SIZE +
                                   UCN_POLICY_DIAGNOSTIC_REQUEST_PAYLOAD_BYTES);
    TEST_ASSERT(policy_diagnostic_step(&b, 0U) == 0);
    TEST_ASSERT(callback.count == 1U &&
                callback.result.status == UCN_POLICY_DIAGNOSTIC_STATUS_OK &&
                callback.result.section == UCN_POLICY_DIAGNOSTIC_POLICY &&
                callback.result.record.policy.key.destination == node_a_id &&
                callback.result.record.policy.key.endpoint == 0x40U &&
                callback.result.record.policy.match_hits == 1U);
    TEST_ASSERT(cba.last_message_type == UCN_MSG_POLICY_DIAGNOSTIC_REPLY &&
                cba.last_length == UCN_FRAME_HEADER_SIZE +
                                   UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES);
    TEST_ASSERT(b.stats.policy_diagnostic_requests_received == 1U &&
                b.stats.policy_diagnostic_replies_sent == 1U &&
                a.stats.policy_diagnostic_completed == 1U);

    (void)memset(&callback, 0, sizeof(callback));
    TEST_ASSERT(policy_diagnostic_query(&a, &b, UCN_POLICY_DIAGNOSTIC_PATH,
                                        0U, &callback) == 0);
    TEST_ASSERT(callback.count == 1U &&
                callback.result.status == UCN_POLICY_DIAGNOSTIC_STATUS_OK &&
                callback.result.record.path.local_path_id == 7U &&
                callback.result.record.path.active_bearer_link_id == ba.link_id &&
                callback.result.record.path.route_cost_valid &&
                callback.result.record.path.route_cost == 37U &&
                callback.result.record.path.rtt_ewma_ms == 9U &&
                callback.result.record.path.tx_failure_ewma_per_mille == 13U &&
                callback.result.record.path.queue_pressure_ewma_per_mille == 21U);

    (void)memset(&callback, 0, sizeof(callback));
    TEST_ASSERT(policy_diagnostic_query(&a, &b, UCN_POLICY_DIAGNOSTIC_FLOW,
                                        0U, &callback) == 0);
    TEST_ASSERT(callback.count == 1U &&
                callback.result.status == UCN_POLICY_DIAGNOSTIC_STATUS_OK &&
                callback.result.record.flow.key.destination == node_a_id &&
                callback.result.record.flow.local_path_id == 7U &&
                callback.result.record.flow.remaining_ms == 5000U &&
                callback.result.record.flow.active_bearer_link_id == ba.link_id);

    (void)memset(&callback, 0, sizeof(callback));
    TEST_ASSERT(policy_diagnostic_query(&a, &b,
                                        UCN_POLICY_DIAGNOSTIC_LINK_QUALITY,
                                        0U, &callback) == 0);
    TEST_ASSERT(callback.count == 1U &&
                callback.result.status == UCN_POLICY_DIAGNOSTIC_STATUS_OK &&
                callback.result.record.link_quality.link_id == ba.link_id &&
                callback.result.record.link_quality.is_up &&
                callback.result.record.link_quality.route_cost == 37U);

    (void)memset(&callback, 0, sizeof(callback));
    TEST_ASSERT(policy_diagnostic_query(&a, &b, UCN_POLICY_DIAGNOSTIC_SUMMARY,
                                        0U, &callback) == 0);
    TEST_ASSERT(callback.count == 1U &&
                callback.result.status == UCN_POLICY_DIAGNOSTIC_STATUS_OK &&
                callback.result.record.summary.counters[0] == 1U);

    (void)memset(&callback, 0, sizeof(callback));
    TEST_ASSERT(policy_diagnostic_query(&a, &b, UCN_POLICY_DIAGNOSTIC_POLICY,
                                        1U, &callback) == 0);
    TEST_ASSERT(callback.count == 1U &&
                callback.result.status == UCN_POLICY_DIAGNOSTIC_STATUS_EMPTY);
    TEST_ASSERT(ucn_node_request_policy_diagnostic(
                    &a, node_b_id, UCN_POLICY_DIAGNOSTIC_SUMMARY, 3U,
                    policy_diagnostic_callback, &callback) == UCN_ERR_ARGUMENT);

    /* The request table is fixed.  A product may choose a larger token burst
     * than the default one, so capacity remains an independent hard bound. */
    (void)memset(&callback, 0, sizeof(callback));
    a.policy_diagnostic_tokens = (uint8_t)UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH;
    TEST_ASSERT(ucn_node_request_policy_diagnostic(
                    &a, node_b_id, UCN_POLICY_DIAGNOSTIC_SUMMARY, 0U,
                    policy_diagnostic_callback, &callback) == UCN_OK);
    TEST_ASSERT(ucn_node_request_policy_diagnostic(
                    &a, node_b_id, UCN_POLICY_DIAGNOSTIC_SUMMARY, 1U,
                    policy_diagnostic_callback, &callback) == UCN_OK);
    TEST_ASSERT(ucn_node_request_policy_diagnostic(
                    &a, node_b_id, UCN_POLICY_DIAGNOSTIC_SUMMARY, 2U,
                    policy_diagnostic_callback, &callback) == UCN_ERR_NO_SPACE);
    result = ucn_node_step(&a, UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS);
    TEST_ASSERT((result == UCN_OK || result == UCN_ERR_NOT_FOUND) &&
                callback.count == UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH);

    /* A target that has not opted in reveals nothing.  The source callback is
     * bounded by its local timeout instead of silently retaining a request. */
    (void)memset(&callback, 0, sizeof(callback));
    TEST_ASSERT(ucn_node_set_policy_diagnostic_authorizer(&b, NULL, NULL) == UCN_OK);
    a.policy_diagnostic_tokens = UCN_POLICY_DIAGNOSTIC_TOKEN_BURST;
    TEST_ASSERT(ucn_node_request_policy_diagnostic(
                    &a, node_b_id, UCN_POLICY_DIAGNOSTIC_SUMMARY, 0U,
                    policy_diagnostic_callback, &callback) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, a.now_ms) == UCN_ERR_ACCESS);
    result = ucn_node_step(&a, a.now_ms + UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS);
    TEST_ASSERT(result == UCN_OK || result == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(callback.count == 1U &&
                callback.result.status == UCN_POLICY_DIAGNOSTIC_STATUS_TIMEOUT &&
                b.stats.policy_diagnostic_rejected != 0U &&
                a.stats.policy_diagnostic_timeouts >= 1U);

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        TEST_ASSERT(!a.policy_diagnostic_pending[index].occupied);
    }
    return 0;
}
