#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct bearer_link_context {
    ucn_node_t *peer_node;
    ucn_link_t *peer_ingress;
    bool is_up;
    bool deliver;
    uint16_t route_cost;
    ucn_link_metrics_t metrics;
    uint32_t send_count;
    uint32_t close_count;
    bool fail_send_link_down_once;
    bool propagate_peer_result;
    bool last_has_route_extension;
    uint16_t last_route_epoch;
} bearer_link_context_t;

typedef struct bearer_receive_state {
    uint32_t count;
    uint8_t last_payload;
} bearer_receive_state_t;

typedef struct bearer_provider_context {
    uint8_t denied_link_id;
    uint32_t calls;
} bearer_provider_context_t;

static ucn_result_t bearer_link_send(ucn_link_t *link,
                                     const uint8_t *frame,
                                     size_t length)
{
    bearer_link_context_t *context = (bearer_link_context_t *)link->context;
    ucn_frame_t decoded;

    context->send_count++;
    if (ucn_frame_decode(frame, length, &decoded) == UCN_OK) {
        context->last_has_route_extension = decoded.has_route_extension;
        context->last_route_epoch = decoded.route_epoch;
    }
    if (context->fail_send_link_down_once) {
        context->fail_send_link_down_once = false;
        return UCN_ERR_LINK_DOWN;
    }
    if (context->peer_node == NULL || context->peer_ingress == NULL) {
        return UCN_OK;
    }
    if (!context->deliver) {
        return UCN_OK;
    }
    {
        const ucn_result_t peer_result =
            ucn_node_receive(context->peer_node, context->peer_ingress, frame, length);

        return context->propagate_peer_result ? peer_result : UCN_OK;
    }
}

static ucn_result_t bearer_link_status(const ucn_link_t *link,
                                       ucn_link_status_t *status)
{
    const bearer_link_context_t *context =
        (const bearer_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static void bearer_link_close(ucn_link_t *link)
{
    bearer_link_context_t *context = (bearer_link_context_t *)link->context;

    context->close_count++;
}

static ucn_result_t bearer_link_metrics(const ucn_link_t *link,
                                        ucn_link_metrics_t *metrics)
{
    const bearer_link_context_t *context =
        (const bearer_link_context_t *)link->context;

    *metrics = context->metrics;
    metrics->route_cost_valid = true;
    metrics->route_cost = context->route_cost;
    return UCN_OK;
}

static const ucn_link_ops_t BEARER_LINK_OPS = {
    NULL, bearer_link_send, NULL, bearer_link_status, bearer_link_close,
    bearer_link_metrics
};

static ucn_result_t bearer_step_with_peer_clock(ucn_node_t *node,
                                                ucn_node_t *peer,
                                                uint32_t now_ms)
{
    peer->now_ms = now_ms;
    return ucn_node_step(node, now_ms);
}

static int bearer_init_node(ucn_node_t *node, ucn_node_id_t node_id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x4E424D21);
    config.node_id = node_id;
    config.default_hop_limit = 3U;
    if (ucn_node_init(node, &config) != UCN_OK) {
        return 1;
    }
    return ucn_node_set_join_policy(node, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK ?
           0 : 1;
}

static void bearer_setup_link(ucn_link_t *link,
                              bearer_link_context_t *context,
                              uint8_t link_id,
                              uint16_t route_cost)
{
    link->ops = &BEARER_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = 0U;
    context->is_up = true;
    context->deliver = true;
    context->propagate_peer_result = true;
    context->route_cost = route_cost;
}

static void bearer_receive(void *context, const ucn_frame_t *frame)
{
    bearer_receive_state_t *state = (bearer_receive_state_t *)context;

    state->count++;
    state->last_payload = frame->payload_length == 0U ? 0U : frame->payload[0];
}

static ucn_result_t bearer_authorize(void *context,
                                     ucn_node_id_t local_node_id,
                                     ucn_node_id_t peer_node_id,
                                     const ucn_link_t *link)
{
    bearer_provider_context_t *provider = (bearer_provider_context_t *)context;

    (void)local_node_id;
    (void)peer_node_id;
    provider->calls++;
    return link->link_id == provider->denied_link_id ? UCN_ERR_ACCESS : UCN_OK;
}

#if UCN_FEATURE_CANDIDATE_ROUTING
static ucn_route_entry_t *bearer_find_dynamic_route(ucn_node_t *node,
                                                     ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && !node->routes[index].is_static &&
            node->routes[index].destination == destination) {
            return &node->routes[index];
        }
    }
    return NULL;
}

#if UCN_FEATURE_CANDIDATE_ROUTING
static ucn_candidate_route_t *bearer_find_candidate_route(
    ucn_node_t *node,
    ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].destination == destination) {
            return &node->candidates[index];
        }
    }
    return NULL;
}
#endif

static ucn_route_entry_t *bearer_find_static_route(ucn_node_t *node,
                                                    ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && node->routes[index].is_static &&
            node->routes[index].destination == destination) {
            return &node->routes[index];
        }
    }
    return NULL;
}
#endif

int test_neighbor_bearer(void)
{
    /* The existing single-Link suites are the regression proof for the
     * footprint-saving compatibility profile. */
    if (UCN_MAX_BEARERS_PER_NEIGHBOR < 2U) {
        return 0;
    }
    ucn_node_t a, b, provider_node;
    ucn_link_t ab_primary, ab_backup, ba_primary, ba_backup, ab_extra;
    ucn_link_t provider_first, provider_second;
    bearer_link_context_t cab_primary, cab_backup, cba_primary, cba_backup;
    bearer_link_context_t cab_extra, cprovider_first, cprovider_second;
    bearer_receive_state_t received;
    bearer_provider_context_t provider;
    uint8_t first_payload = 0xA1U;
    uint8_t second_payload = 0xB2U;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&provider_node, 0, sizeof(provider_node));
    (void)memset(&ab_primary, 0, sizeof(ab_primary));
    (void)memset(&ab_backup, 0, sizeof(ab_backup));
    (void)memset(&ba_primary, 0, sizeof(ba_primary));
    (void)memset(&ba_backup, 0, sizeof(ba_backup));
    (void)memset(&ab_extra, 0, sizeof(ab_extra));
    (void)memset(&provider_first, 0, sizeof(provider_first));
    (void)memset(&provider_second, 0, sizeof(provider_second));
    (void)memset(&cab_primary, 0, sizeof(cab_primary));
    (void)memset(&cab_backup, 0, sizeof(cab_backup));
    (void)memset(&cba_primary, 0, sizeof(cba_primary));
    (void)memset(&cba_backup, 0, sizeof(cba_backup));
    (void)memset(&cab_extra, 0, sizeof(cab_extra));
    (void)memset(&cprovider_first, 0, sizeof(cprovider_first));
    (void)memset(&cprovider_second, 0, sizeof(cprovider_second));
    (void)memset(&received, 0, sizeof(received));
    (void)memset(&provider, 0, sizeof(provider));

    TEST_ASSERT(bearer_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(bearer_init_node(&b, UINT32_C(2)) == 0);
    bearer_setup_link(&ab_primary, &cab_primary, 1U, 10U);
    bearer_setup_link(&ab_backup, &cab_backup, 2U, 30U);
    bearer_setup_link(&ba_primary, &cba_primary, 3U, 10U);
    bearer_setup_link(&ba_backup, &cba_backup, 4U, 30U);
    cab_primary.peer_node = &b; cab_primary.peer_ingress = &ba_primary;
    cab_backup.peer_node = &b; cab_backup.peer_ingress = &ba_backup;
    cba_primary.peer_node = &a; cba_primary.peer_ingress = &ab_primary;
    cba_backup.peer_node = &a; cba_backup.peer_ingress = &ab_backup;

    /* Two physical Bearers discover the same remote identity, but consume
     * exactly one Neighbor slot on each side. */
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab_primary, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab_backup, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_primary, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_backup, 1U) == UCN_OK);
    TEST_ASSERT(a.link_count == 2U && b.link_count == 2U);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(a.neighbors[0].bearer_count == 2U);
    TEST_ASSERT(a.neighbors[0].bearers[a.neighbors[0].primary_bearer_index].link ==
                &ab_primary);

    ucn_node_set_rx_handler(&b, bearer_receive, &received);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_OK);
    TEST_ASSERT(cab_primary.send_count >= 2U);
    TEST_ASSERT(cab_backup.send_count == 1U);
    TEST_ASSERT(received.count == 1U && received.last_payload == first_payload);

    /* The primary Driver is down.  The Neighbor and its logical routes stay
     * valid; the following application frame uses the admitted backup. */
    cab_primary.is_up = false;
    TEST_ASSERT(ucn_node_step(&a, 2U) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(a.link_count == 2U);
    TEST_ASSERT(a.neighbors[0].bearers[a.neighbors[0].primary_bearer_index].link ==
                &ab_backup);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &second_payload, 1U) == UCN_OK);
    TEST_ASSERT(cab_primary.send_count == 2U);
    TEST_ASSERT(cab_backup.send_count == 3U);
    TEST_ASSERT(received.count == 2U && received.last_payload == second_payload);

    bearer_setup_link(&ab_extra, &cab_extra, 5U, 40U);
    ab_extra.peer_node_id = UINT32_C(2);
    TEST_ASSERT(ucn_node_observe_neighbor(&a, &ab_extra, 3U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(a.neighbors[0].bearer_count == UCN_MAX_BEARERS_PER_NEIGHBOR);

    /* A Neighbor is removed only after its complete fixed Bearer set fails. */
    cab_backup.is_up = false;
    TEST_ASSERT(ucn_node_step(&a, 4U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_REMOVED) == 1U);
    TEST_ASSERT(a.link_count == 0U);
    TEST_ASSERT(ab_primary.peer_node_id == 0U && ab_backup.peer_node_id == 0U);
    TEST_ASSERT(cab_primary.close_count == 1U && cab_backup.close_count == 1U);

    TEST_ASSERT(bearer_init_node(&provider_node, UINT32_C(10)) == 0);
    provider.denied_link_id = 8U;
    TEST_ASSERT(ucn_node_set_join_policy(&provider_node, UCN_JOIN_PROVIDER,
                                         bearer_authorize, &provider) == UCN_OK);
    bearer_setup_link(&provider_first, &cprovider_first, 7U, 10U);
    bearer_setup_link(&provider_second, &cprovider_second, 8U, 20U);
    provider_first.peer_node_id = UINT32_C(11);
    provider_second.peer_node_id = UINT32_C(11);
    TEST_ASSERT(ucn_node_observe_neighbor(&provider_node, &provider_first, 10U) == UCN_OK);
    TEST_ASSERT(ucn_node_observe_neighbor(&provider_node, &provider_second, 11U) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(provider.calls == 2U);
    TEST_ASSERT(provider_node.link_count == 1U);
    TEST_ASSERT(provider_node.neighbors[0].bearer_count == 1U);
    return 0;
}

int test_neighbor_quality(void)
{
    ucn_node_t a, b;
    ucn_link_t ab_primary, ab_backup, ba_primary, ba_backup;
    bearer_link_context_t cab_primary, cab_backup, cba_primary, cba_backup;
    uint32_t switch_at_ms = 0U;

    if (UCN_MAX_BEARERS_PER_NEIGHBOR < 2U) {
        return 0;
    }
    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab_primary, 0, sizeof(ab_primary));
    (void)memset(&ab_backup, 0, sizeof(ab_backup));
    (void)memset(&ba_primary, 0, sizeof(ba_primary));
    (void)memset(&ba_backup, 0, sizeof(ba_backup));
    (void)memset(&cab_primary, 0, sizeof(cab_primary));
    (void)memset(&cab_backup, 0, sizeof(cab_backup));
    (void)memset(&cba_primary, 0, sizeof(cba_primary));
    (void)memset(&cba_backup, 0, sizeof(cba_backup));

    TEST_ASSERT(bearer_init_node(&a, UINT32_C(21)) == 0);
    TEST_ASSERT(bearer_init_node(&b, UINT32_C(22)) == 0);
    bearer_setup_link(&ab_primary, &cab_primary, 21U, 100U);
    bearer_setup_link(&ab_backup, &cab_backup, 22U, 95U);
    bearer_setup_link(&ba_primary, &cba_primary, 23U, 100U);
    bearer_setup_link(&ba_backup, &cba_backup, 24U, 95U);
    cab_primary.peer_node = &b; cab_primary.peer_ingress = &ba_primary;
    cab_backup.peer_node = &b; cab_backup.peer_ingress = &ba_backup;
    cba_primary.peer_node = &a; cba_primary.peer_ingress = &ab_primary;
    cba_backup.peer_node = &a; cba_backup.peer_ingress = &ab_backup;

    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab_primary, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab_backup, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_primary, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_backup, 0U) == UCN_OK);
    TEST_ASSERT(a.neighbors[0].bearers[a.neighbors[0].primary_bearer_index].link ==
                &ab_primary);

    /* A small 5% Cost fluctuation is below the hysteresis threshold and
     * never schedules a quality Probe. */
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 500U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 1000U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 1500U) == UCN_OK);
    TEST_ASSERT(a.neighbors[0].bearers[a.neighbors[0].primary_bearer_index].link ==
                &ab_primary);
    TEST_ASSERT(a.stats.bearer_quality_probes_sent == 0U);

    /* A substantially better candidate without ACKs must not disrupt the
     * old Primary.  Dropping only B->A keeps the candidate Link usable. */
#if UCN_FEATURE_POLICY
    /* The Backup is statically worse (110 > 100), but LC-1 adds the Primary's
     * 160-point TX failure penalty.  Full therefore probes the Backup; Lite
     * retains the old base-Cost-only behavior below. */
    cab_backup.route_cost = 110U;
    cba_backup.route_cost = 110U;
    cab_primary.metrics.tx_failure_rate_valid = true;
    cab_primary.metrics.tx_failure_per_mille = 200U;
    cab_backup.metrics.tx_failure_rate_valid = true;
    cab_backup.metrics.tx_failure_per_mille = 0U;
#else
    cab_backup.route_cost = 70U;
    cba_backup.route_cost = 70U;
#endif
    cba_backup.deliver = false;
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 2000U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 2500U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 3000U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 3100U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 3200U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 3300U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 3500U) == UCN_OK);
    TEST_ASSERT(a.neighbors[0].bearers[a.neighbors[0].primary_bearer_index].link ==
                &ab_primary);
    TEST_ASSERT(a.stats.bearer_quality_probes_sent ==
                UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS);
    TEST_ASSERT(a.stats.bearer_quality_probe_acks_received == 0U);

    /* After the candidate proves stable and receives the configured ACKs,
     * the next sampling point atomically moves the logical next hop. */
    cba_backup.deliver = true;
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 4000U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 4500U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 5000U) == UCN_OK);
    TEST_ASSERT(bearer_step_with_peer_clock(&a, &b, 5500U) == UCN_OK);
    {
        uint32_t now_ms;

        /* Drive the bounded scheduler at its declared service cadence. A
         * deterministic Heartbeat phase may occupy either historical probe
         * timestamp, but cannot starve the candidate proof sequence. */
        for (now_ms = 5510U; now_ms <= 7000U; now_ms += 10U) {
            const ucn_result_t phase_result =
                bearer_step_with_peer_clock(&a, &b, now_ms);

            TEST_ASSERT(phase_result == UCN_OK ||
                        phase_result == UCN_ERR_NOT_FOUND);
            if (a.neighbors[0]
                    .bearers[a.neighbors[0].primary_bearer_index]
                    .link == &ab_backup) {
                switch_at_ms = now_ms;
                break;
            }
        }
    }
    TEST_ASSERT(switch_at_ms != 0U);
    TEST_ASSERT(a.neighbors[0].bearers[a.neighbors[0].primary_bearer_index].link ==
                &ab_backup);
    TEST_ASSERT(a.stats.bearer_quality_probes_sent ==
                (uint32_t)UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS +
                (uint32_t)UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS);
    TEST_ASSERT(a.stats.bearer_quality_probe_acks_received ==
                UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS);
    TEST_ASSERT(a.stats.max_probe_service_delay_ms > 0U &&
                a.stats.max_probe_service_delay_ms <=
                UCN_MAINTENANCE_SERVICE_BOUND_MS);
    TEST_ASSERT(a.stats.bearer_quality_switches == 1U);
#if UCN_FEATURE_POLICY
    {
        const uint32_t probes_after_switch =
            a.stats.bearer_quality_probes_sent;

        /* Reverse the dynamic conditions immediately.  The 3000 ms hold
         * keeps the new Primary stable and schedules no new soft Probe. */
        cab_primary.metrics.tx_failure_per_mille = 0U;
        cab_backup.metrics.tx_failure_per_mille = 200U;
        {
            uint32_t offset_ms;

            for (offset_ms = 500U; offset_ms <= 2500U; offset_ms += 500U) {
                const ucn_result_t hold_result = bearer_step_with_peer_clock(
                    &a, &b, switch_at_ms + offset_ms);

                TEST_ASSERT(hold_result == UCN_OK ||
                            hold_result == UCN_ERR_NOT_FOUND);
            }
        }
        TEST_ASSERT(a.neighbors[0]
                        .bearers[a.neighbors[0].primary_bearer_index]
                        .link == &ab_backup);
        TEST_ASSERT(a.stats.bearer_quality_probes_sent ==
                    probes_after_switch);

        /* A stale active snapshot is a hard exclusion.  It fails over at the
         * next sample without waiting for another three-sample/Probe cycle. */
        cab_primary.metrics.metrics_timestamp_valid = true;
        cab_primary.metrics.metrics_timestamp_ms = switch_at_ms + 3000U;
        cab_backup.metrics.metrics_timestamp_valid = true;
        cab_backup.metrics.metrics_timestamp_ms = 0U;
        TEST_ASSERT(bearer_step_with_peer_clock(
                        &a, &b, switch_at_ms + 3000U) == UCN_OK);
        TEST_ASSERT(a.neighbors[0]
                        .bearers[a.neighbors[0].primary_bearer_index]
                        .link == &ab_primary);
    }
#endif
    return 0;
}

#if UCN_FEATURE_CANDIDATE_ROUTING
int test_neighbor_route_bearer(void)
{
    ucn_node_t a, b, c;
    ucn_link_t ab_primary, ab_backup, ba_primary, ba_backup, bc, cb;
    bearer_link_context_t cab_primary, cab_backup, cba_primary, cba_backup;
    bearer_link_context_t cbc, ccb;
    bearer_receive_state_t received;
    ucn_route_entry_t *route;
    ucn_route_entry_t *static_route;
    ucn_candidate_route_t *candidate;
    uint8_t first_payload = 0xC1U;
    uint8_t second_payload = 0xC2U;
    uint32_t primary_send_count;
    uint32_t backup_send_count;

    if (UCN_MAX_BEARERS_PER_NEIGHBOR < 2U) {
        return 0;
    }
    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c));
    (void)memset(&ab_primary, 0, sizeof(ab_primary));
    (void)memset(&ab_backup, 0, sizeof(ab_backup));
    (void)memset(&ba_primary, 0, sizeof(ba_primary));
    (void)memset(&ba_backup, 0, sizeof(ba_backup));
    (void)memset(&bc, 0, sizeof(bc));
    (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&cab_primary, 0, sizeof(cab_primary));
    (void)memset(&cab_backup, 0, sizeof(cab_backup));
    (void)memset(&cba_primary, 0, sizeof(cba_primary));
    (void)memset(&cba_backup, 0, sizeof(cba_backup));
    (void)memset(&cbc, 0, sizeof(cbc));
    (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&received, 0, sizeof(received));

    TEST_ASSERT(bearer_init_node(&a, UINT32_C(31)) == 0);
    TEST_ASSERT(bearer_init_node(&b, UINT32_C(32)) == 0);
    TEST_ASSERT(bearer_init_node(&c, UINT32_C(33)) == 0);
    bearer_setup_link(&ab_primary, &cab_primary, 31U, 10U);
    bearer_setup_link(&ab_backup, &cab_backup, 32U, 30U);
    bearer_setup_link(&ba_primary, &cba_primary, 33U, 10U);
    bearer_setup_link(&ba_backup, &cba_backup, 34U, 30U);
    bearer_setup_link(&bc, &cbc, 35U, 10U);
    bearer_setup_link(&cb, &ccb, 36U, 10U);
    bc.peer_node_id = UINT32_C(33);
    cb.peer_node_id = UINT32_C(32);
    cab_primary.peer_node = &b; cab_primary.peer_ingress = &ba_primary;
    cab_backup.peer_node = &b; cab_backup.peer_ingress = &ba_backup;
    cba_primary.peer_node = &a; cba_primary.peer_ingress = &ab_primary;
    cba_backup.peer_node = &a; cba_backup.peer_ingress = &ab_backup;
    cbc.peer_node = &c; cbc.peer_ingress = &cb;
    ccb.peer_node = &b; ccb.peer_ingress = &bc;

    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab_primary, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab_backup, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_primary, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_backup, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    ucn_node_set_rx_handler(&c, bearer_receive, &received);

    /* The learned dynamic A->B->C Route records the primary physical Link,
     * while the logical next hop remains Neighbor B. */
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(33), 100U) == UCN_OK);
    route = bearer_find_dynamic_route(&a, UINT32_C(33));
    TEST_ASSERT(route != NULL && route->egress_link == &ab_primary &&
                route->route_cost == 20U);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(33), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U && received.last_payload == first_payload);

    /* Make the downstream segment sufficiently better so the Candidate itself
     * is learned through the current Primary before that Bearer fails.  The
     * local A-B base Cost remains immutable, matching the Link Cost contract. */
    cbc.route_cost = 1U;
    ccb.route_cost = 1U;
    TEST_ASSERT(ucn_node_refresh_route(&a, UINT32_C(33), 200U) == UCN_OK);
    candidate = bearer_find_candidate_route(&a, UINT32_C(33));
    TEST_ASSERT(candidate != NULL && candidate->originated_here &&
                candidate->egress_link == &ab_primary &&
                candidate->route_cost == 11U);

    /* The Driver reports a hard failure after status selection.  The same
     * application call must mark that Bearer down, remap every logical Route
     * reference and retry exactly once through the admitted Backup. */
    primary_send_count = cab_primary.send_count;
    backup_send_count = cab_backup.send_count;
    cab_primary.fail_send_link_down_once = true;
    cba_primary.is_up = false;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(33), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &second_payload, 1U) == UCN_OK);
    route = bearer_find_dynamic_route(&a, UINT32_C(33));
    TEST_ASSERT(route != NULL && route->egress_link == &ab_backup &&
                route->route_cost == 40U);
    candidate = bearer_find_candidate_route(&a, UINT32_C(33));
    TEST_ASSERT(candidate != NULL && candidate->egress_link == &ab_backup &&
                candidate->route_cost == 31U);
    TEST_ASSERT(a.neighbors[0].bearers[a.neighbors[0].primary_bearer_index].link ==
                &ab_backup);
    TEST_ASSERT(cab_primary.send_count == primary_send_count + 1U &&
                cab_backup.send_count == backup_send_count + 1U);
    TEST_ASSERT(cab_backup.last_has_route_extension &&
                cab_backup.last_route_epoch == route->route_epoch);
    TEST_ASSERT(received.count == 2U && received.last_payload == second_payload);

    /* A route installed against the failed physical member is also normalized
     * to the logical Neighbor's current Primary. */
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(99), &ab_primary) == UCN_OK);
    static_route = bearer_find_static_route(&a, UINT32_C(99));
    TEST_ASSERT(static_route != NULL && static_route->egress_link == &ab_primary);
    TEST_ASSERT(ucn_node_step(&a, 201U) == UCN_OK);
    TEST_ASSERT(static_route->egress_link == &ab_backup &&
                static_route->route_cost == 30U);
    backup_send_count = cab_backup.send_count;
    TEST_ASSERT(ucn_node_step(&a, 301U) == UCN_OK);
    TEST_ASSERT(a.stats.path_probes_sent == 1U &&
                a.stats.path_probe_acks_received == 1U);
    TEST_ASSERT(cab_backup.send_count > backup_send_count);
    TEST_ASSERT(ucn_node_step(&a, 401U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 501U) == UCN_OK);
    TEST_ASSERT(a.stats.path_probes_sent == UCN_PATH_PROBE_REQUIRED_ACKS &&
                a.stats.path_probe_acks_received == UCN_PATH_PROBE_REQUIRED_ACKS);
    TEST_ASSERT(ucn_node_step(&a, 601U) == UCN_OK);
    TEST_ASSERT(a.stats.route_switches == 1U);
    route = bearer_find_dynamic_route(&a, UINT32_C(33));
    TEST_ASSERT(route != NULL && route->egress_link == &ab_backup &&
                route->route_cost == 31U);

    /* A downstream B->C failure returns RERR to A over the current Backup.
     * That RERR clears both A's dynamic Route and untrusted Candidate. */
    cbc.is_up = false;
    ccb.is_up = false;
    cab_backup.propagate_peer_result = false;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(33), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &second_payload, 1U) ==
                UCN_OK);
    TEST_ASSERT(b.stats.route_errors_sent == 1U);
    TEST_ASSERT(bearer_find_dynamic_route(&a, UINT32_C(33)) == NULL);
    TEST_ASSERT(bearer_find_candidate_route(&a, UINT32_C(33)) == NULL);

    /* Relearn through the Backup, then prove that losing all A<->B Bearers
     * removes the Neighbor and invalidates the newly learned dynamic Route. */
    cbc.is_up = true;
    ccb.is_up = true;
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(33), 1000U) == UCN_OK);
    route = bearer_find_dynamic_route(&a, UINT32_C(33));
    TEST_ASSERT(route != NULL && route->egress_link == &ab_backup);
    cab_backup.is_up = false;
    cba_backup.is_up = false;
    TEST_ASSERT(ucn_node_step(&a, 1001U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_REMOVED) == 1U);
    TEST_ASSERT(a.link_count == 0U);
    TEST_ASSERT(bearer_find_dynamic_route(&a, UINT32_C(33)) == NULL);
    /* Static product routes are deliberately not auto-deleted by dynamic
     * Neighbor removal; callers must decide their own recovery policy. */
    TEST_ASSERT(bearer_find_static_route(&a, UINT32_C(99)) == static_route);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(99), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &second_payload, 1U) ==
                UCN_ERR_LINK_DOWN);
    return 0;
}
#endif
