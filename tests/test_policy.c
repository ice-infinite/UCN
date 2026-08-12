#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct policy_link_context {
    bool is_up;
    ucn_link_metrics_t metrics;
    uint32_t send_count;
} policy_link_context_t;

static ucn_result_t policy_link_send(ucn_link_t *link,
                                     const uint8_t *frame,
                                     size_t length)
{
    policy_link_context_t *context = (policy_link_context_t *)link->context;

    (void)frame;
    (void)length;
    context->send_count++;
    return UCN_OK;
}

static ucn_result_t policy_link_status(const ucn_link_t *link,
                                       ucn_link_status_t *status)
{
    const policy_link_context_t *context =
        (const policy_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static ucn_result_t policy_link_metrics(const ucn_link_t *link,
                                        ucn_link_metrics_t *metrics)
{
    const policy_link_context_t *context =
        (const policy_link_context_t *)link->context;

    *metrics = context->metrics;
    return UCN_OK;
}

static const ucn_link_ops_t POLICY_LINK_OPS = {
    NULL, policy_link_send, NULL, policy_link_status, NULL, policy_link_metrics
};

static int policy_init_node(ucn_node_t *node)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x22AABBCC);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 4U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void policy_setup_link(ucn_link_t *link,
                              policy_link_context_t *context,
                              uint8_t link_id,
                              ucn_node_id_t peer_node_id)
{
    link->ops = &POLICY_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
}

static int policy_step(ucn_node_t *node, uint32_t now_ms)
{
    const ucn_result_t result = ucn_node_step(node, now_ms);

    return result == UCN_OK || result == UCN_ERR_NOT_FOUND ? 0 : 1;
}

int test_policy(void)
{
    ucn_node_t node;
    ucn_link_t link, unregistered_link;
    policy_link_context_t link_context, unregistered_context;
    ucn_route_policy_config_t policy;
    ucn_policy_path_config_t path;
    const ucn_route_policy_entry_t *matched;
    const ucn_policy_path_entry_t *path_entry;
    const ucn_policy_flow_binding_t *flow;
    const ucn_policy_link_quality_snapshot_t *quality;
    const ucn_policy_stats_t *stats;
    uint8_t payload = 0x5AU;
    size_t index;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    (void)memset(&unregistered_link, 0, sizeof(unregistered_link));
    (void)memset(&link_context, 0, sizeof(link_context));
    (void)memset(&unregistered_context, 0, sizeof(unregistered_context));
    TEST_ASSERT(policy_init_node(&node) == 0);
    TEST_ASSERT(ucn_node_find_route_policy(&node, UINT32_C(9), 0x40U,
                                           UCN_TRAFFIC_Q1_REALTIME) == NULL);

    link_context.is_up = true;
    link_context.metrics.route_cost_valid = true;
    link_context.metrics.route_cost = 80U;
    link_context.metrics.rtt_valid = true;
    link_context.metrics.rtt_ms = 20U;
    link_context.metrics.tx_failure_rate_valid = true;
    link_context.metrics.tx_failure_per_mille = 40U;
    link_context.metrics.queue_pressure_valid = true;
    link_context.metrics.queue_pressure_per_mille = 100U;
    link_context.metrics.rx_failure_rate_valid = true;
    link_context.metrics.rx_failure_per_mille = 50U;
    link_context.metrics.medium_busy_valid = true;
    link_context.metrics.medium_busy_per_mille = 750U;
    link_context.metrics.medium_quality_valid = true;
    link_context.metrics.medium_quality_per_mille = 700U;
    link_context.metrics.rtt_reference_valid = true;
    link_context.metrics.rtt_reference_ms = 5U;
    link_context.metrics.administrative_bias_valid = true;
    link_context.metrics.administrative_bias = -10;
    link_context.metrics.metrics_timestamp_valid = true;
    link_context.metrics.metrics_timestamp_ms = 0U;
    policy_setup_link(&link, &link_context, 1U, UINT32_C(9));
    policy_setup_link(&unregistered_link, &unregistered_context, 2U, UINT32_C(9));
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);
    TEST_ASSERT(policy_step(&node, 0U) == 0);
    quality = ucn_node_get_link_quality(&node, &link);
    TEST_ASSERT(quality != NULL && quality->is_up && quality->route_cost_valid);
    TEST_ASSERT(quality->route_cost == 80U && quality->rtt_ewma_ms == 20U);
    TEST_ASSERT(quality->tx_failure_ewma_per_mille == 40U &&
                quality->queue_pressure_ewma_per_mille == 100U);
    TEST_ASSERT(quality->rx_failure_ewma_per_mille == 50U &&
                quality->medium_busy_ewma_per_mille == 750U &&
                quality->medium_quality_ewma_per_mille == 700U);
    TEST_ASSERT(quality->cost.selectable && quality->cost.base_cost_known &&
                quality->cost.effective_select_cost == 158U);

    link_context.metrics.route_cost = 40U;
    link_context.metrics.rtt_ms = 60U;
    link_context.metrics.tx_failure_per_mille = 140U;
    link_context.metrics.queue_pressure_per_mille = 300U;
    link_context.metrics.rx_failure_per_mille = 100U;
    link_context.metrics.medium_busy_per_mille = 500U;
    link_context.metrics.medium_quality_per_mille = 900U;
    link_context.metrics.metrics_timestamp_ms =
        UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS;
    TEST_ASSERT(policy_step(&node, UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS - 1U) == 0);
    quality = ucn_node_get_link_quality(&node, &link);
    TEST_ASSERT(quality != NULL && quality->route_cost == 80U);
    TEST_ASSERT(policy_step(&node, UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS) == 0);
    quality = ucn_node_get_link_quality(&node, &link);
    TEST_ASSERT(quality != NULL && quality->route_cost == 40U);
    TEST_ASSERT(quality->rtt_ewma_ms == 30U);
    TEST_ASSERT(quality->tx_failure_ewma_per_mille == 65U);
    TEST_ASSERT(quality->queue_pressure_ewma_per_mille == 150U);
    TEST_ASSERT(quality->rx_failure_ewma_per_mille == 62U);
    TEST_ASSERT(quality->medium_busy_ewma_per_mille == 687U);
    TEST_ASSERT(quality->medium_quality_ewma_per_mille == 750U);

    /* Link metrics use independent valid bits.  Out-of-range per-mille input
     * is rejected as unknown instead of being silently folded into Policy
     * quality, so an Adapter cannot turn a unit/configuration error into an
     * exaggerated congestion decision. */
    link_context.metrics.route_cost_valid = false;
    link_context.metrics.rtt_valid = false;
    link_context.metrics.tx_failure_rate_valid = true;
    link_context.metrics.tx_failure_per_mille =
        UCN_LINK_METRIC_PER_MILLE_MAX + 1U;
    link_context.metrics.queue_pressure_valid = true;
    link_context.metrics.queue_pressure_per_mille =
        UCN_LINK_METRIC_PER_MILLE_MAX + 1U;
    TEST_ASSERT(policy_step(&node,
                            UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS * 2U) == 0);
    quality = ucn_node_get_link_quality(&node, &link);
    TEST_ASSERT(quality != NULL && !quality->route_cost_valid &&
                !quality->rtt_valid && !quality->tx_failure_rate_valid &&
                !quality->queue_pressure_valid);
    TEST_ASSERT(quality->rejected_metric_count == 2U);

    (void)memset(&policy, 0, sizeof(policy));
    policy.key.destination = UINT32_C(9);
    policy.key.endpoint = 0x40U;
    policy.key.traffic_class = UCN_POLICY_ANY_TRAFFIC_CLASS;
    policy.mode = UCN_ROUTE_POLICY_PINNED_FAILOVER;
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_OK);
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q1_REALTIME;
    policy.mode = UCN_ROUTE_POLICY_PINNED_STRICT;
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_OK);
    matched = ucn_node_find_route_policy(&node, UINT32_C(9), 0x40U,
                                         UCN_TRAFFIC_Q1_REALTIME);
    TEST_ASSERT(matched != NULL &&
                matched->config.mode == UCN_ROUTE_POLICY_PINNED_STRICT);
    matched = ucn_node_find_route_policy(&node, UINT32_C(9), 0x40U,
                                         UCN_TRAFFIC_Q0_CRITICAL);
    TEST_ASSERT(matched != NULL &&
                matched->config.mode == UCN_ROUTE_POLICY_PINNED_FAILOVER);
    policy.mode = UCN_ROUTE_POLICY_AUTO_BEST;
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_OK);
    matched = ucn_node_find_route_policy(&node, UINT32_C(9), 0x40U,
                                         UCN_TRAFFIC_Q1_REALTIME);
    TEST_ASSERT(matched != NULL && matched->config.mode == UCN_ROUTE_POLICY_AUTO_BEST);

    policy.key.endpoint = 0x20U;
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_ERR_ARGUMENT);
    policy.key.endpoint = 0x41U;
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q0_CRITICAL;
    policy.mode = UCN_ROUTE_POLICY_AUTO_BALANCE;
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_ERR_UNSUPPORTED);
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q1_REALTIME;
    policy.primary_local_path_id = 0U;
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_ERR_ARGUMENT);
    policy.primary_local_path_id = 1U;
    policy.allow_discovery_on_hard_failure = true;
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_ERR_ARGUMENT);
    policy.allow_discovery_on_hard_failure = false;
    policy.balance_flow_lease_ms = UINT32_C(0x80000000);
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_ERR_ARGUMENT);
    policy.balance_flow_lease_ms = 0U;

    for (index = 2U; index < UCN_MAX_ROUTE_POLICIES; ++index) {
        policy.key.destination = (ucn_node_id_t)(UINT32_C(100) + index);
        policy.key.endpoint = (ucn_endpoint_t)(0x40U + index);
        policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q1_REALTIME;
        policy.mode = UCN_ROUTE_POLICY_AUTO_BEST;
        TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_OK);
    }
    policy.key.destination = UINT32_C(999);
    policy.key.endpoint = 0x60U;
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_ERR_NO_SPACE);

    (void)memset(&path, 0, sizeof(path));
    path.local_path_id = 1U;
    path.destination = UINT32_C(9);
    path.egress_link = &link;
    path.verified = true;
    TEST_ASSERT(ucn_node_set_policy_path(&node, &path) == UCN_OK);
    path_entry = ucn_node_find_policy_path(&node, 1U);
    TEST_ASSERT(path_entry != NULL && path_entry->state == UCN_POLICY_PATH_VERIFIED);
    path.egress_link = &unregistered_link;
    TEST_ASSERT(ucn_node_set_policy_path(&node, &path) == UCN_ERR_ARGUMENT);
    path.egress_link = &link;
    for (index = 2U; index <= UCN_MAX_POLICY_PATHS; ++index) {
        path.local_path_id = (uint16_t)index;
        TEST_ASSERT(ucn_node_set_policy_path(&node, &path) == UCN_OK);
    }
    path.local_path_id = UINT16_C(99);
    TEST_ASSERT(ucn_node_set_policy_path(&node, &path) == UCN_ERR_NO_SPACE);

    TEST_ASSERT(ucn_node_bind_q1_flow(&node, UINT32_C(9), 0x40U, 1U, 200U) == UCN_OK);
    flow = ucn_node_find_q1_flow(&node, UINT32_C(9), 0x40U);
    TEST_ASSERT(flow != NULL && flow->local_path_id == 1U);
    TEST_ASSERT(ucn_node_bind_q1_flow(&node, UINT32_C(10), 0x40U, 1U, 200U) ==
                UCN_ERR_CONFIG);
    for (index = 1U; index < UCN_MAX_POLICY_FLOWS; ++index) {
        TEST_ASSERT(ucn_node_bind_q1_flow(&node, UINT32_C(9),
                                           (ucn_endpoint_t)(0x40U + index),
                                           1U, 200U) == UCN_OK);
    }
    TEST_ASSERT(ucn_node_bind_q1_flow(&node, UINT32_C(9), 0x60U, 1U, 200U) ==
                UCN_ERR_NO_SPACE);

    /* The exact Q1 rule above is AUTO_BEST, so it must retain the old send
     * API even after T22.3 adds enforcement for only PINNED modes. */
    TEST_ASSERT(ucn_node_send_endpoint(&node, UINT32_C(9), 0x40U,
                                       UCN_TRAFFIC_Q1_REALTIME,
                                       &payload, 1U) == UCN_OK);
    TEST_ASSERT(link_context.send_count == 1U);
    stats = ucn_node_get_policy_stats(&node);
    TEST_ASSERT(stats != NULL && stats->policy_match_hits == 1U);

    TEST_ASSERT(policy_step(&node,
                            UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS * 2U + 200U) ==
                0);
    TEST_ASSERT(ucn_node_find_q1_flow(&node, UINT32_C(9), 0x40U) == NULL);
    stats = ucn_node_get_policy_stats(&node);
    TEST_ASSERT(stats != NULL && stats->flow_bindings_expired == UCN_MAX_POLICY_FLOWS);

    link_context.is_up = false;
    TEST_ASSERT(policy_step(&node, UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS * 3U) == 0);
    quality = ucn_node_get_link_quality(&node, &link);
    path_entry = ucn_node_find_policy_path(&node, 1U);
    TEST_ASSERT(quality != NULL && !quality->is_up);
    TEST_ASSERT(path_entry != NULL && path_entry->state == UCN_POLICY_PATH_DOWN);
    /* T22.1 only records the local hard-down state.  It does not select a
     * backup or alter forwarding until the later Path-ID stages. */
    TEST_ASSERT(ucn_node_clear_policy_path(&node, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_find_policy_path(&node, 1U) == NULL);
    return 0;
}
