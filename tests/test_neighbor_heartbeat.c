#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct heartbeat_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool is_up;
    bool deliver;
    bool backpressure_q0;
    uint32_t close_count;
    uint32_t send_count;
    uint32_t heartbeat_send_count;
} heartbeat_link_context_t;

static ucn_result_t heartbeat_link_send(ucn_link_t *link,
                                        const uint8_t *frame,
                                        size_t length)
{
    heartbeat_link_context_t *context = (heartbeat_link_context_t *)link->context;
    ucn_frame_t decoded;
    const ucn_result_t decode_result = ucn_frame_decode(frame, length, &decoded);

    context->send_count++;
    if (decode_result == UCN_OK && decoded.message_type == UCN_MSG_HEARTBEAT) {
        context->heartbeat_send_count++;
    }
    if (context->backpressure_q0 && decode_result == UCN_OK &&
        decoded.message_type == UCN_MSG_DATA_Q0) {
        return UCN_ERR_NO_SPACE;
    }

    if (!context->deliver) {
        return UCN_OK;
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t heartbeat_link_status(const ucn_link_t *link,
                                          ucn_link_status_t *status)
{
    const heartbeat_link_context_t *context =
        (const heartbeat_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static void heartbeat_link_close(ucn_link_t *link)
{
    heartbeat_link_context_t *context = (heartbeat_link_context_t *)link->context;

    context->close_count++;
}

static const ucn_link_ops_t HEARTBEAT_LINK_OPS = {
    NULL, heartbeat_link_send, NULL, heartbeat_link_status, heartbeat_link_close, NULL
};

static int heartbeat_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x9A8B7C6D);
    config.node_id = id;
    config.default_hop_limit = 4U;
    if (ucn_node_init(node, &config) != UCN_OK) {
        return 1;
    }
    return ucn_node_set_join_policy(node, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK ? 0 : 1;
}

static void heartbeat_setup_link(ucn_link_t *link,
                                 heartbeat_link_context_t *context,
                                 uint8_t link_id)
{
    link->ops = &HEARTBEAT_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = 0U;
    context->is_up = true;
    context->deliver = true;
}

static ucn_neighbor_bearer_t *heartbeat_find_bearer(ucn_node_t *node,
                                                     const ucn_link_t *link)
{
    size_t neighbor_index;

    for (neighbor_index = 0U; neighbor_index < UCN_MAX_NEIGHBORS;
         ++neighbor_index) {
        size_t bearer_index;
        ucn_neighbor_entry_t *entry = &node->neighbors[neighbor_index];

        for (bearer_index = 0U; bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            if (entry->bearers[bearer_index].link == link) {
                return &entry->bearers[bearer_index];
            }
        }
    }
    return NULL;
}

static int heartbeat_run_mixed_liveness_profiles(void)
{
    ucn_node_t a, b;
    ucn_link_t ab_fast, ab_default, ba_fast, ba_default;
    heartbeat_link_context_t cab_fast, cab_default, cba_fast, cba_default;
    ucn_neighbor_bearer_t *fast_bearer;
    ucn_neighbor_bearer_t *default_bearer;

    if (UCN_MAX_BEARERS_PER_NEIGHBOR < 2U) {
        return 0;
    }
    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab_fast, 0, sizeof(ab_fast));
    (void)memset(&ab_default, 0, sizeof(ab_default));
    (void)memset(&ba_fast, 0, sizeof(ba_fast));
    (void)memset(&ba_default, 0, sizeof(ba_default));
    (void)memset(&cab_fast, 0, sizeof(cab_fast));
    (void)memset(&cab_default, 0, sizeof(cab_default));
    (void)memset(&cba_fast, 0, sizeof(cba_fast));
    (void)memset(&cba_default, 0, sizeof(cba_default));
    TEST_ASSERT(heartbeat_init_node(&a, UINT32_C(51)) == 0);
    TEST_ASSERT(heartbeat_init_node(&b, UINT32_C(52)) == 0);
    heartbeat_setup_link(&ab_fast, &cab_fast, 51U);
    heartbeat_setup_link(&ab_default, &cab_default, 52U);
    heartbeat_setup_link(&ba_fast, &cba_fast, 53U);
    heartbeat_setup_link(&ba_default, &cba_default, 54U);
    ab_fast.liveness_profile = (uint8_t)UCN_LINK_LIVENESS_FAST;
    ba_fast.liveness_profile = (uint8_t)UCN_LINK_LIVENESS_FAST;
    cab_fast.peer = &b; cab_fast.peer_ingress = &ba_fast;
    cab_default.peer = &b; cab_default.peer_ingress = &ba_default;
    cba_fast.peer = &a; cba_fast.peer_ingress = &ab_fast;
    cba_default.peer = &a; cba_default.peer_ingress = &ab_default;

    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab_fast, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab_default, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_fast, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_default, 0U) == UCN_OK);
    TEST_ASSERT(a.neighbors[0].bearer_count == 2U);
    fast_bearer = heartbeat_find_bearer(&a, &ab_fast);
    default_bearer = heartbeat_find_bearer(&a, &ab_default);
    TEST_ASSERT(fast_bearer != NULL && default_bearer != NULL);

    b.now_ms = 0U;
    TEST_ASSERT(ucn_node_step(&a, 0U) == UCN_OK);
    b.now_ms = 1U;
    TEST_ASSERT(ucn_node_step(&a, 1U) == UCN_OK);
    (void)ucn_node_step(&a, UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS - 1U);
    TEST_ASSERT(cab_fast.heartbeat_send_count == 1U &&
                cab_default.heartbeat_send_count == 1U);
    /* Scheduled liveness is independently bounded per admitted Bearer and
     * must not be starved by the generic RREQ/Probe control-token bucket. */
    a.control_tokens = 0U;
    a.control_last_refill_ms = UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS;
    b.now_ms = UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS;
    TEST_ASSERT(ucn_node_step(&a,
                              UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS) ==
                UCN_OK);
    TEST_ASSERT(cab_fast.heartbeat_send_count == 2U &&
                cab_default.heartbeat_send_count == 1U);
    TEST_ASSERT(a.control_tokens == 0U &&
                a.stats.control_budget_dropped == 0U);

    /* UART-like FAST Bearer becomes physically silent while the DEFAULT
     * Backup continues to ACK. Local send still reports success. */
    cab_fast.deliver = false;
    cba_fast.deliver = false;
    b.now_ms = UINT32_C(1000);
    (void)ucn_node_step(&a, UINT32_C(1000));
    b.now_ms = UINT32_C(1001);
    (void)ucn_node_step(&a, UINT32_C(1001));
    (void)ucn_node_step(
        &a, UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS +
                UCN_LINK_LIVENESS_FAST_SUSPECT_TIMEOUT_MS - 1U);
    TEST_ASSERT(fast_bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED);
    (void)ucn_node_step(
        &a, UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS +
                UCN_LINK_LIVENESS_FAST_SUSPECT_TIMEOUT_MS);
    TEST_ASSERT(fast_bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT &&
                default_bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED);
    TEST_ASSERT(a.neighbors[0].bearers[a.neighbors[0].primary_bearer_index].link ==
                &ab_default);

    b.now_ms = UINT32_C(2000);
    (void)ucn_node_step(&a, UINT32_C(2000));
    b.now_ms = UINT32_C(2001);
    (void)ucn_node_step(&a, UINT32_C(2001));
    (void)ucn_node_step(
        &a, UCN_LINK_LIVENESS_FAST_HEARTBEAT_INTERVAL_MS +
                UCN_LINK_LIVENESS_FAST_REMOVE_TIMEOUT_MS);
    TEST_ASSERT(fast_bearer->state == UCN_NEIGHBOR_BEARER_DOWN);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);

    /* A valid HELLO on the physical Bearer is the recovery/admission path.
     * Quality/Cost hysteresis, not this liveness test, decides when it becomes
     * Primary again. */
    cab_fast.deliver = true;
    cba_fast.deliver = true;
    a.now_ms = UINT32_C(2300);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba_fast, UINT32_C(2300)) == UCN_OK);
    fast_bearer = heartbeat_find_bearer(&a, &ab_fast);
    TEST_ASSERT(fast_bearer != NULL &&
                fast_bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED);
    TEST_ASSERT(cab_fast.heartbeat_send_count > cab_default.heartbeat_send_count);
    return 0;
}

static int heartbeat_run_sustained_backlog(bool include_q0, bool include_q1)
{
    uint8_t payload = 0U;
    uint32_t now_ms;
    ucn_node_t a, b;
    ucn_link_t ab, ba;
    heartbeat_link_context_t cab, cba;
    ucn_send_request_t request;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    TEST_ASSERT(heartbeat_init_node(&a, UINT32_C(11)) == 0);
    TEST_ASSERT(heartbeat_init_node(&b, UINT32_C(12)) == 0);
    heartbeat_setup_link(&ab, &cab, 11U);
    heartbeat_setup_link(&ba, &cba, 12U);
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba, 0U) == UCN_OK);

    /* Establish the first heartbeat schedule.  The following loop keeps one
     * or both business queues non-empty for a 30 s virtual-time equivalent. */
    TEST_ASSERT(ucn_node_step(&a, 0U) == UCN_OK);
    for (now_ms = 1U; now_ms <= UINT32_C(30000); ++now_ms) {
        payload = (uint8_t)now_ms;
        /* Synchronous virtual delivery stands in for a peer Protocol Task;
         * keep the receiver's monotonic clock aligned for RX token refill. */
        b.now_ms = now_ms;
        if (include_q0) {
            ucn_result_t enqueue_result;

            (void)memset(&request, 0, sizeof(request));
            request.destination = UINT32_C(12);
            request.message_type = UCN_MSG_DATA_Q0;
            request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
            request.delivery = UCN_DELIVERY_BEST_EFFORT;
            request.deadline_ms = now_ms + UINT32_C(100);
            request.payload = &payload;
            request.payload_length = 1U;
            enqueue_result = ucn_node_enqueue(&a, &request);
            /* A source offering one Q0 item on every scheduler turn is
             * intentionally faster than a Link that also carries liveness
             * control.  Fixed Q0 backpressure is therefore expected at a
             * maintenance slot; it must not prevent the following service. */
            TEST_ASSERT(enqueue_result == UCN_OK ||
                        enqueue_result == UCN_ERR_NO_SPACE);
        }
        if (include_q1) {
            (void)memset(&request, 0, sizeof(request));
            request.destination = UINT32_C(12);
            request.message_type = UCN_MSG_DATA_Q1;
            request.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
            request.delivery = UCN_DELIVERY_LATEST_VALUE;
            request.deadline_ms = now_ms + UINT32_C(100);
            request.payload = &payload;
            request.payload_length = 1U;
            TEST_ASSERT(ucn_node_enqueue(&a, &request) == UCN_OK);
        }
        TEST_ASSERT(ucn_node_step(&a, now_ms) == UCN_OK);
    }

    TEST_ASSERT(a.stats.heartbeat_requests_sent >= 30U);
    TEST_ASSERT(a.stats.maintenance_preemptions >= 30U);
    TEST_ASSERT(a.stats.neighbor_suspected == 0U);
    TEST_ASSERT(a.stats.neighbor_removed == 0U);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_ADMITTED) == 1U);
    return 0;
}

static int heartbeat_run_wraparound_lifecycle(void)
{
    const uint32_t base_ms = UINT32_MAX - UINT32_C(1000);
    ucn_node_t a, b;
    ucn_link_t ab, ba;
    heartbeat_link_context_t cab, cba;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    TEST_ASSERT(heartbeat_init_node(&a, UINT32_C(21)) == 0);
    TEST_ASSERT(heartbeat_init_node(&b, UINT32_C(22)) == 0);
    heartbeat_setup_link(&ab, &cab, 21U);
    heartbeat_setup_link(&ba, &cba, 22U);
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab, base_ms) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba, base_ms) == UCN_OK);

    cab.deliver = false;
    cba.deliver = false;
    (void)ucn_node_step(&a, base_ms + UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS - 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);
    (void)ucn_node_step(&a, base_ms + UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_SUSPECT) == 1U);
    (void)ucn_node_step(&a, base_ms + UCN_NEIGHBOR_REMOVE_TIMEOUT_MS);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_REMOVED) == 1U);
    TEST_ASSERT(a.link_count == 0U && cab.close_count == 1U);
    return 0;
}

static int heartbeat_run_fast_wraparound_lifecycle(void)
{
    const uint32_t base_ms = UINT32_MAX - UINT32_C(1000);
    ucn_node_t a, b;
    ucn_link_t ab, ba;
    heartbeat_link_context_t cab, cba;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    TEST_ASSERT(heartbeat_init_node(&a, UINT32_C(23)) == 0);
    TEST_ASSERT(heartbeat_init_node(&b, UINT32_C(24)) == 0);
    heartbeat_setup_link(&ab, &cab, 23U);
    heartbeat_setup_link(&ba, &cba, 24U);
    ab.liveness_profile = (uint8_t)UCN_LINK_LIVENESS_FAST;
    ba.liveness_profile = (uint8_t)UCN_LINK_LIVENESS_FAST;
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab, base_ms) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba, base_ms) == UCN_OK);

    cab.deliver = false;
    cba.deliver = false;
    (void)ucn_node_step(
        &a, base_ms + UCN_LINK_LIVENESS_FAST_SUSPECT_TIMEOUT_MS - 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);
    (void)ucn_node_step(
        &a, base_ms + UCN_LINK_LIVENESS_FAST_SUSPECT_TIMEOUT_MS);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_SUSPECT) == 1U);
    (void)ucn_node_step(
        &a, base_ms + UCN_LINK_LIVENESS_FAST_REMOVE_TIMEOUT_MS);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_REMOVED) == 1U);
    TEST_ASSERT(a.link_count == 0U && cab.close_count == 1U);
    return 0;
}

static int heartbeat_run_invalid_liveness_profile(void)
{
    ucn_node_t node;
    ucn_link_t link;
    heartbeat_link_context_t context;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    (void)memset(&context, 0, sizeof(context));
    TEST_ASSERT(heartbeat_init_node(&node, UINT32_C(25)) == 0);
    heartbeat_setup_link(&link, &context, 25U);
    link.liveness_profile = (uint8_t)UCN_LINK_LIVENESS_PROFILE_COUNT;
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_ERR_CONFIG);
    TEST_ASSERT(node.link_count == 0U);
    return 0;
}

static int heartbeat_run_sustained_q0_backpressure(void)
{
    uint8_t payload = 0U;
    uint32_t now_ms;
    ucn_node_t a, b;
    ucn_link_t ab, ba;
    heartbeat_link_context_t cab, cba;
    ucn_send_request_t request;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    TEST_ASSERT(heartbeat_init_node(&a, UINT32_C(31)) == 0);
    TEST_ASSERT(heartbeat_init_node(&b, UINT32_C(32)) == 0);
    heartbeat_setup_link(&ab, &cab, 31U);
    heartbeat_setup_link(&ba, &cba, 32U);
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 0U) == UCN_OK);
    cab.backpressure_q0 = true;

    for (now_ms = 1U; now_ms <= UINT32_C(5000); ++now_ms) {
        ucn_result_t enqueue_result;
        ucn_result_t step_result;

        payload = (uint8_t)now_ms;
        b.now_ms = now_ms;
        (void)memset(&request, 0, sizeof(request));
        request.destination = UINT32_C(32);
        request.message_type = UCN_MSG_DATA_Q0;
        request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
        request.delivery = UCN_DELIVERY_RETRY_ON_BACKPRESSURE;
        request.deadline_ms = now_ms + UINT32_C(200);
        request.payload = &payload;
        request.payload_length = 1U;
        enqueue_result = ucn_node_enqueue(&a, &request);
        TEST_ASSERT(enqueue_result == UCN_OK ||
                    enqueue_result == UCN_ERR_NO_SPACE);
        step_result = ucn_node_step(&a, now_ms);
        TEST_ASSERT(step_result == UCN_OK ||
                    step_result == UCN_ERR_NO_SPACE ||
                    step_result == UCN_ERR_NOT_FOUND ||
                    step_result == UCN_ERR_TTL);
    }

    TEST_ASSERT(a.stats.q0_backpressure_retries > 0U);
    TEST_ASSERT(a.stats.q0_backpressure_exhausted > 0U);
    TEST_ASSERT(a.stats.heartbeat_requests_sent >= 5U);
    TEST_ASSERT(a.stats.neighbor_suspected == 0U &&
                a.stats.neighbor_removed == 0U);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);
    return 0;
}

static int heartbeat_run_multi_bearer_service_bound(void)
{
    const uint32_t heartbeat_due_ms = UCN_HEARTBEAT_INTERVAL_MS;
    const uint32_t test_end_ms =
        heartbeat_due_ms + UCN_MAINTENANCE_SERVICE_BOUND_MS;
    uint8_t payload = 0U;
    uint32_t now_ms;
    size_t link_index = 0U;
    size_t neighbor_index;
    ucn_node_t node;
    ucn_link_t links[UCN_MAX_LINKS];
    heartbeat_link_context_t contexts[UCN_MAX_LINKS];
    ucn_send_request_t request;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(links, 0, sizeof(links));
    (void)memset(contexts, 0, sizeof(contexts));
    TEST_ASSERT(heartbeat_init_node(&node, UINT32_C(41)) == 0);
    TEST_ASSERT(UCN_MAINTENANCE_SERVICE_BOUND_MS ==
                (uint32_t)(
                    ((uint64_t)UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE + 1U) *
                    UCN_MAX_STEP_INTERVAL_MS * UCN_MAX_NEIGHBORS *
                    UCN_MAX_BEARERS_PER_NEIGHBOR));
    TEST_ASSERT((uint64_t)UCN_HEARTBEAT_INTERVAL_MS +
                    UCN_MAINTENANCE_SERVICE_BOUND_MS <
                UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS);

    /* Populate as many real Link slots as this Profile permits.  The default
     * profile becomes two Neighbors with two Bearers; Bearer=1 becomes four
     * Neighbors with one Bearer.  This tests the scheduler, not HELLO/Join. */
    for (neighbor_index = 0U;
         neighbor_index < UCN_MAX_NEIGHBORS && link_index < UCN_MAX_LINKS;
         ++neighbor_index) {
        ucn_neighbor_entry_t *entry = &node.neighbors[neighbor_index];
        size_t bearer_index;

        entry->state = UCN_NEIGHBOR_ADMITTED;
        entry->peer_node_id = (ucn_node_id_t)(UINT32_C(100) + neighbor_index);
        entry->primary_bearer_index = 0U;
        entry->bearer_quality_sampled = true;
        entry->last_bearer_quality_sample_ms = 0U;
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR &&
             link_index < UCN_MAX_LINKS;
             ++bearer_index, ++link_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];

            heartbeat_setup_link(&links[link_index], &contexts[link_index],
                                 (uint8_t)(link_index + 1U));
            contexts[link_index].deliver = false;
            links[link_index].peer_node_id = entry->peer_node_id;
            TEST_ASSERT(ucn_node_register_link(&node, &links[link_index]) ==
                        UCN_OK);
            bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
            bearer->link = &links[link_index];
            bearer->last_seen_ms = 0U;
            bearer->heartbeat_sent = true;
            bearer->last_heartbeat_sent_ms = 0U;
            entry->bearer_count++;
        }
    }
    TEST_ASSERT(link_index == UCN_MAX_LINKS);
    TEST_ASSERT(ucn_node_step(&node, 0U) == UCN_ERR_NOT_FOUND);

    for (now_ms = UCN_MAX_STEP_INTERVAL_MS;
         now_ms <= test_end_ms;
         now_ms += UCN_MAX_STEP_INTERVAL_MS) {
        ucn_result_t enqueue_result;

        payload = (uint8_t)now_ms;
        (void)memset(&request, 0, sizeof(request));
        request.destination = node.neighbors[0].peer_node_id;
        request.message_type = UCN_MSG_DATA_Q0;
        request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
        request.delivery = UCN_DELIVERY_BEST_EFFORT;
        request.deadline_ms = ucn_deadline_from_now(now_ms, UINT32_C(500));
        request.payload = &payload;
        request.payload_length = 1U;
        enqueue_result = ucn_node_enqueue(&node, &request);
        TEST_ASSERT(enqueue_result == UCN_OK ||
                    enqueue_result == UCN_ERR_NO_SPACE);
        TEST_ASSERT(ucn_node_step(&node, now_ms) == UCN_OK);
    }

    TEST_ASSERT(node.stats.heartbeat_requests_sent == link_index);
    TEST_ASSERT(node.stats.maintenance_preemptions >= link_index);
    TEST_ASSERT(node.stats.max_step_gap_ms == UCN_MAX_STEP_INTERVAL_MS);
    TEST_ASSERT(node.stats.step_interval_violations == 0U);
    TEST_ASSERT(node.stats.max_heartbeat_service_delay_ms > 0U &&
                node.stats.max_heartbeat_service_delay_ms <=
                UCN_MAINTENANCE_SERVICE_BOUND_MS);
    for (neighbor_index = 0U; neighbor_index < UCN_MAX_NEIGHBORS;
         ++neighbor_index) {
        size_t bearer_index;
        const ucn_neighbor_entry_t *entry = &node.neighbors[neighbor_index];

        for (bearer_index = 0U; bearer_index < entry->bearer_count;
             ++bearer_index) {
            const uint32_t sent_ms =
                entry->bearers[bearer_index].last_heartbeat_sent_ms;

            TEST_ASSERT(sent_ms >= heartbeat_due_ms && sent_ms <= test_end_ms);
        }
    }
    TEST_ASSERT(node.stats.neighbor_suspected == 0U &&
                node.stats.neighbor_removed == 0U);
    return 0;
}

int test_neighbor_heartbeat(void)
{
    uint8_t payload = 0x5AU;
    ucn_node_t a, b, c;
    ucn_link_t ab, ba, cb, bc;
    heartbeat_link_context_t cab, cba, ccb, cbc;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&bc, 0, sizeof(bc));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&cbc, 0, sizeof(cbc));
    TEST_ASSERT(heartbeat_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(heartbeat_init_node(&b, UINT32_C(2)) == 0);
    heartbeat_setup_link(&ab, &cab, 1U);
    heartbeat_setup_link(&ba, &cba, 2U);
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;

    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba, 1U) == UCN_OK);
    TEST_ASSERT(a.link_count == 1U && b.link_count == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_ADMITTED) == 1U);

    TEST_ASSERT(ucn_node_step(&a, 1000U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&b, 1000U) == UCN_OK);
    TEST_ASSERT(a.stats.heartbeat_requests_sent == 1U);
    TEST_ASSERT(a.stats.heartbeat_received >= 1U);
    TEST_ASSERT(b.stats.heartbeat_acks_sent >= 1U);

    TEST_ASSERT(ucn_node_send(&a, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_ADMITTED) == 1U);

    cab.deliver = false;
    cba.deliver = false;
    TEST_ASSERT(ucn_node_step(&a, 2000U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&b, 2000U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 4000U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&b, 4000U) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_SUSPECT) == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_SUSPECT) == 1U);
    TEST_ASSERT(a.stats.neighbor_suspected == 1U);
    TEST_ASSERT(b.stats.neighbor_suspected == 1U);

    /* Any authenticated direct traffic during the short SUSPECT window
     * restores the neighbor without unregistering its Link. */
    cab.deliver = true;
    cba.deliver = true;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&b, UINT32_C(1), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_ADMITTED) == 1U);

    cab.deliver = false;
    cba.deliver = false;
    TEST_ASSERT(ucn_node_step(&a, 5000U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&b, 5000U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 7000U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&b, 7000U) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_SUSPECT) == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_SUSPECT) == 1U);
    TEST_ASSERT(ucn_node_step(&a, 8000U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_step(&b, 8000U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_neighbor_count(&a, UCN_NEIGHBOR_REMOVED) == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_REMOVED) == 1U);
    TEST_ASSERT(a.link_count == 0U && b.link_count == 0U);
    TEST_ASSERT(ab.peer_node_id == 0U && ba.peer_node_id == 0U);
    TEST_ASSERT(cab.close_count == 1U && cba.close_count == 1U);

    TEST_ASSERT(heartbeat_init_node(&c, UINT32_C(3)) == 0);
    heartbeat_setup_link(&cb, &ccb, 3U);
    heartbeat_setup_link(&bc, &cbc, 4U);
    ccb.peer = &b; ccb.peer_ingress = &bc;
    cbc.peer = &c; cbc.peer_ingress = &cb;
    TEST_ASSERT(ucn_node_broadcast_hello(&c, &cb, 9000U) == UCN_OK);
    TEST_ASSERT(b.link_count == 1U);
    TEST_ASSERT(bc.peer_node_id == UINT32_C(3));
    TEST_ASSERT(ucn_node_neighbor_count(&b, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(heartbeat_run_sustained_backlog(false, true) == 0);
    TEST_ASSERT(heartbeat_run_sustained_backlog(true, false) == 0);
    TEST_ASSERT(heartbeat_run_sustained_backlog(true, true) == 0);
    TEST_ASSERT(heartbeat_run_sustained_q0_backpressure() == 0);
    TEST_ASSERT(heartbeat_run_multi_bearer_service_bound() == 0);
    TEST_ASSERT(heartbeat_run_wraparound_lifecycle() == 0);
    TEST_ASSERT(heartbeat_run_fast_wraparound_lifecycle() == 0);
    TEST_ASSERT(heartbeat_run_mixed_liveness_profiles() == 0);
    TEST_ASSERT(heartbeat_run_invalid_liveness_profile() == 0);
    return 0;
}
