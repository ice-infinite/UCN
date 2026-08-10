#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct candidate_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    uint16_t route_cost;
    uint32_t send_count;
    bool deliver;
    bool last_business_has_route_extension;
    uint16_t last_business_route_epoch;
} candidate_link_context_t;

typedef struct candidate_receive_state {
    uint32_t count;
    uint8_t last_payload;
} candidate_receive_state_t;

static ucn_result_t candidate_link_send(ucn_link_t *link,
                                        const uint8_t *frame,
                                        size_t length)
{
    candidate_link_context_t *context = (candidate_link_context_t *)link->context;

    context->send_count++;
    {
        ucn_frame_t decoded;

        if (ucn_frame_decode(frame, length, &decoded) == UCN_OK &&
            decoded.message_type == UCN_MSG_DATA_Q1) {
            context->last_business_has_route_extension = decoded.has_route_extension;
            context->last_business_route_epoch = decoded.route_epoch;
        }
    }
    if (!context->deliver) {
        return UCN_OK;
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t candidate_link_status(const ucn_link_t *link,
                                          ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static ucn_result_t candidate_link_metrics(const ucn_link_t *link,
                                           ucn_link_metrics_t *metrics)
{
    const candidate_link_context_t *context =
        (const candidate_link_context_t *)link->context;

    metrics->route_cost_valid = true;
    metrics->route_cost = context->route_cost;
    return UCN_OK;
}

static const ucn_link_ops_t CANDIDATE_LINK_OPS = {
    NULL, candidate_link_send, NULL, candidate_link_status, NULL, candidate_link_metrics
};

static int candidate_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0xCA11D1DA);
    config.node_id = id;
    config.default_hop_limit = 5U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void candidate_setup_link(ucn_link_t *link,
                                 candidate_link_context_t *context,
                                 uint8_t link_id,
                                 ucn_node_id_t peer_node_id,
                                 uint16_t route_cost)
{
    link->ops = &CANDIDATE_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
    context->route_cost = route_cost;
    context->deliver = true;
}

static void candidate_rx(void *context, const ucn_frame_t *frame)
{
    candidate_receive_state_t *state = (candidate_receive_state_t *)context;

    state->count++;
    state->last_payload = frame->payload[0];
}

static void reset_send_counts(candidate_link_context_t *contexts, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        contexts[index].send_count = 0U;
        contexts[index].last_business_has_route_extension = false;
        contexts[index].last_business_route_epoch = 0U;
    }
}

int test_candidate_route(void)
{
    uint8_t first_payload = 0x51U;
    uint8_t second_payload = 0x52U;
    ucn_node_t a, b, c, d;
    ucn_link_t ab, ba, bc, cb, ad, da, dc, cd;
    candidate_link_context_t contexts[8];
    candidate_receive_state_t received;
    ucn_send_request_t q0_request;
    ucn_send_request_t q1_request;
    uint16_t initial_route_epoch;
    uint32_t now_ms;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c)); (void)memset(&d, 0, sizeof(d));
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&bc, 0, sizeof(bc)); (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&ad, 0, sizeof(ad)); (void)memset(&da, 0, sizeof(da));
    (void)memset(&dc, 0, sizeof(dc)); (void)memset(&cd, 0, sizeof(cd));
    (void)memset(contexts, 0, sizeof(contexts));
    (void)memset(&received, 0, sizeof(received));
    (void)memset(&q0_request, 0, sizeof(q0_request));
    (void)memset(&q1_request, 0, sizeof(q1_request));
    TEST_ASSERT(candidate_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(candidate_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(candidate_init_node(&c, UINT32_C(3)) == 0);
    TEST_ASSERT(candidate_init_node(&d, UINT32_C(4)) == 0);

    candidate_setup_link(&ab, &contexts[0], 1U, UINT32_C(2), 10U);
    candidate_setup_link(&ba, &contexts[1], 2U, UINT32_C(1), 10U);
    candidate_setup_link(&bc, &contexts[2], 3U, UINT32_C(3), 10U);
    candidate_setup_link(&cb, &contexts[3], 4U, UINT32_C(2), 10U);
    contexts[0].peer = &b; contexts[0].peer_ingress = &ba;
    contexts[1].peer = &a; contexts[1].peer_ingress = &ab;
    contexts[2].peer = &c; contexts[2].peer_ingress = &cb;
    contexts[3].peer = &b; contexts[3].peer_ingress = &bc;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    ucn_node_set_rx_handler(&c, candidate_rx, &received);

    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 100U) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U && received.last_payload == first_payload);
    TEST_ASSERT(contexts[0].last_business_has_route_extension);
    initial_route_epoch = contexts[0].last_business_route_epoch;
    TEST_ASSERT(initial_route_epoch != 0U);

    /* The only available refresh path has the same Cost.  It must be rejected
     * and the proven A-B-C active route must continue carrying traffic. */
    TEST_ASSERT(ucn_node_refresh_route(&a, UINT32_C(3), 150U) == UCN_OK);
    TEST_ASSERT(a.stats.candidate_rejected == 1U);
    reset_send_counts(contexts, 4U);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_OK);
    TEST_ASSERT(contexts[0].send_count == 1U && contexts[2].send_count == 1U);

    candidate_setup_link(&ad, &contexts[4], 5U, UINT32_C(4), 1U);
    candidate_setup_link(&da, &contexts[5], 6U, UINT32_C(1), 1U);
    candidate_setup_link(&dc, &contexts[6], 7U, UINT32_C(3), 1U);
    candidate_setup_link(&cd, &contexts[7], 8U, UINT32_C(4), 1U);
    contexts[4].peer = &d; contexts[4].peer_ingress = &da;
    contexts[5].peer = &a; contexts[5].peer_ingress = &ad;
    contexts[6].peer = &c; contexts[6].peer_ingress = &cd;
    contexts[7].peer = &d; contexts[7].peer_ingress = &dc;
    TEST_ASSERT(ucn_node_register_link(&a, &ad) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&d, &da) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&d, &dc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cd) == UCN_OK);

    /* A used route enters its refresh window 6 s before the 30 s validation
     * deadline.  Keep Q1 busy through the window: on the fifth scheduling
     * turn the due Route Refresh must preempt it rather than wait for idle. */
    q1_request.destination = UINT32_C(3);
    q1_request.message_type = UCN_MSG_DATA_Q1;
    q1_request.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    q1_request.delivery = UCN_DELIVERY_LATEST_VALUE;
    q1_request.deadline_ms = UINT32_C(25000);
    q1_request.payload = &first_payload;
    q1_request.payload_length = 1U;
    for (now_ms = UINT32_C(24096); now_ms < UINT32_C(24100); ++now_ms) {
        TEST_ASSERT(ucn_node_enqueue(&a, &q1_request) == UCN_OK);
        TEST_ASSERT(ucn_node_step(&a, now_ms) == UCN_OK);
    }
    TEST_ASSERT(ucn_node_enqueue(&a, &q1_request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 24100U) == UCN_OK);
    TEST_ASSERT(a.stats.route_refreshes_started >= 2U);
    TEST_ASSERT(a.stats.maintenance_preemptions >= 1U);
    TEST_ASSERT(a.stats.candidate_routes_learned >= 1U);
    TEST_ASSERT(ucn_node_step(&a, 24100U) == UCN_OK);
    reset_send_counts(contexts, 8U);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_OK);
    TEST_ASSERT(contexts[0].send_count == 1U && contexts[4].send_count == 0U);

    /* A lost Probe ACK only removes the untrusted candidate.  The active
     * A-B-C path keeps carrying traffic while a later refresh is allowed. */
    q0_request.destination = UINT32_C(3);
    q0_request.message_type = UCN_MSG_DATA_Q0;
    q0_request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    q0_request.delivery = UCN_DELIVERY_BEST_EFFORT;
    q0_request.payload = &first_payload;
    q0_request.payload_length = 1U;
    TEST_ASSERT(ucn_node_enqueue(&a, &q0_request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 24101U) == UCN_OK);
    TEST_ASSERT(a.stats.path_probes_sent == 0U);
    contexts[7].deliver = false;
    TEST_ASSERT(ucn_node_step(&a, 24201U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 24301U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 24401U) == UCN_OK);
    TEST_ASSERT(a.stats.path_probes_sent == UCN_PATH_PROBE_REQUIRED_ACKS);
    TEST_ASSERT(a.stats.path_probe_acks_received == 0U);
    TEST_ASSERT(ucn_node_step(&a, 27200U) == UCN_ERR_NOT_FOUND);
    reset_send_counts(contexts, 8U);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_OK);
    TEST_ASSERT(contexts[0].send_count == 1U && contexts[4].send_count == 0U);

    contexts[7].deliver = true;
    TEST_ASSERT(ucn_node_step(&a, 29100U) == UCN_OK);
    TEST_ASSERT(a.stats.route_refreshes_started >= 3U);
    TEST_ASSERT(ucn_node_step(&a, 29101U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 29201U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 29301U) == UCN_OK);
    TEST_ASSERT(a.stats.path_probes_sent == (uint32_t)UCN_PATH_PROBE_REQUIRED_ACKS * 2U);
    TEST_ASSERT(a.stats.path_probe_acks_received == UCN_PATH_PROBE_REQUIRED_ACKS);
    TEST_ASSERT(ucn_node_step(&a, 29401U) == UCN_OK);
    TEST_ASSERT(a.stats.route_switches == 1U);

    reset_send_counts(contexts, 8U);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &second_payload, 1U) == UCN_OK);
    TEST_ASSERT(contexts[0].send_count == 0U && contexts[4].send_count == 1U);
    TEST_ASSERT(contexts[6].send_count == 1U);
    TEST_ASSERT(contexts[4].last_business_has_route_extension &&
                contexts[4].last_business_route_epoch != 0U &&
                contexts[4].last_business_route_epoch != initial_route_epoch);
    TEST_ASSERT(received.last_payload == second_payload);
    return 0;
}
