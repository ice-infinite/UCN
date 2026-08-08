#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct route_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
} route_link_context_t;

typedef struct route_receive_state {
    uint32_t count;
    ucn_node_id_t source;
    uint8_t payload;
} route_receive_state_t;

static ucn_result_t route_link_send(ucn_link_t *link, const uint8_t *frame, size_t length)
{
    route_link_context_t *context = (route_link_context_t *)link->context;
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t route_link_status(const ucn_link_t *link, ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t ROUTE_LINK_OPS = {
    NULL, route_link_send, NULL, route_link_status, NULL, NULL
};

static void route_rx(void *context, const ucn_frame_t *frame)
{
    route_receive_state_t *state = (route_receive_state_t *)context;
    state->count++;
    state->source = frame->source;
    state->payload = frame->payload[0];
}

static int init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;
    config.network_id = UINT32_C(0xAABBCCDD);
    config.node_id = id;
    config.default_hop_limit = 3U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

int test_route(void)
{
    uint8_t payload = 0x5AU;
    ucn_node_t a, b, c;
    ucn_link_t ab, ba, bc, cb;
    route_link_context_t cab, cba, cbc, ccb;
    route_receive_state_t received;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b)); (void)memset(&c, 0, sizeof(c));
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&bc, 0, sizeof(bc)); (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&cab, 0, sizeof(cab)); (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&cbc, 0, sizeof(cbc)); (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(init_node(&a, UINT32_C(1)) == 0); TEST_ASSERT(init_node(&b, UINT32_C(2)) == 0); TEST_ASSERT(init_node(&c, UINT32_C(3)) == 0);

    ab.ops=&ROUTE_LINK_OPS; ab.context=&cab; ab.link_id=1U; ab.mtu=UCN_MAX_FRAME_BYTES; ab.peer_node_id=2U;
    ba.ops=&ROUTE_LINK_OPS; ba.context=&cba; ba.link_id=2U; ba.mtu=UCN_MAX_FRAME_BYTES; ba.peer_node_id=1U;
    bc.ops=&ROUTE_LINK_OPS; bc.context=&cbc; bc.link_id=3U; bc.mtu=UCN_MAX_FRAME_BYTES; bc.peer_node_id=3U;
    cb.ops=&ROUTE_LINK_OPS; cb.context=&ccb; cb.link_id=4U; cb.mtu=UCN_MAX_FRAME_BYTES; cb.peer_node_id=2U;
    cab.peer=&b; cab.peer_ingress=&ba; cba.peer=&a; cba.peer_ingress=&ab;
    cbc.peer=&c; cbc.peer_ingress=&cb; ccb.peer=&b; ccb.peer_ingress=&bc;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(3), &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&b, UINT32_C(3), &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(4), &bc) == UCN_ERR_ARGUMENT);
    ucn_node_set_rx_handler(&c, route_rx, &received);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1, UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U);
    TEST_ASSERT(received.source == UINT32_C(1));
    TEST_ASSERT(received.payload == payload);
    return 0;
}
