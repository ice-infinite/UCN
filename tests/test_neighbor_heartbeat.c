#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct heartbeat_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool is_up;
    bool deliver;
    uint32_t close_count;
} heartbeat_link_context_t;

static ucn_result_t heartbeat_link_send(ucn_link_t *link,
                                        const uint8_t *frame,
                                        size_t length)
{
    heartbeat_link_context_t *context = (heartbeat_link_context_t *)link->context;

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
    return 0;
}
