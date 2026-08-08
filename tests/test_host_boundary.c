#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct host_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool *is_up;
} host_link_context_t;

typedef struct host_receive_state {
    uint32_t count;
    ucn_node_id_t last_source;
    uint8_t last_payload;
} host_receive_state_t;

static ucn_result_t host_link_send(ucn_link_t *link,
                                   const uint8_t *frame,
                                   size_t length)
{
    host_link_context_t *context = (host_link_context_t *)link->context;
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t host_link_status(const ucn_link_t *link, ucn_link_status_t *status)
{
    const host_link_context_t *context = (const host_link_context_t *)link->context;

    status->is_up = *context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t HOST_LINK_OPS = {
    NULL, host_link_send, NULL, host_link_status, NULL, NULL
};

static int host_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0xCAFE1234);
    config.node_id = id;
    config.default_hop_limit = 4U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void host_rx(void *context, const ucn_frame_t *frame)
{
    host_receive_state_t *state = (host_receive_state_t *)context;

    state->count++;
    state->last_source = frame->source;
    state->last_payload = frame->payload[0];
}

int test_host_boundary(void)
{
    uint8_t mcu_payload_before = 0x11U;
    uint8_t host_payload = 0x22U;
    uint8_t mcu_payload_after = 0x33U;
    bool ab_up = true;
    bool bc_up = true;
    bool bh_up = true;
    ucn_node_t a, b, c, host;
    ucn_link_t ab, ba, bc, cb, bh, hb;
    host_link_context_t cab, cba, cbc, ccb, cbh, chb;
    host_receive_state_t received;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c)); (void)memset(&host, 0, sizeof(host));
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&bc, 0, sizeof(bc)); (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&bh, 0, sizeof(bh)); (void)memset(&hb, 0, sizeof(hb));
    (void)memset(&cab, 0, sizeof(cab)); (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&cbc, 0, sizeof(cbc)); (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&cbh, 0, sizeof(cbh)); (void)memset(&chb, 0, sizeof(chb));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(host_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(host_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(host_init_node(&c, UINT32_C(3)) == 0);
    TEST_ASSERT(host_init_node(&host, UINT32_C(100)) == 0);

    ab.ops = &HOST_LINK_OPS; ab.context = &cab; ab.link_id = 1U;
    ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
    ba.ops = &HOST_LINK_OPS; ba.context = &cba; ba.link_id = 2U;
    ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
    bc.ops = &HOST_LINK_OPS; bc.context = &cbc; bc.link_id = 3U;
    bc.mtu = UCN_MAX_FRAME_BYTES; bc.peer_node_id = UINT32_C(3);
    cb.ops = &HOST_LINK_OPS; cb.context = &ccb; cb.link_id = 4U;
    cb.mtu = UCN_MAX_FRAME_BYTES; cb.peer_node_id = UINT32_C(2);
    bh.ops = &HOST_LINK_OPS; bh.context = &cbh; bh.link_id = 5U;
    bh.mtu = UCN_MAX_FRAME_BYTES; bh.peer_node_id = UINT32_C(100);
    hb.ops = &HOST_LINK_OPS; hb.context = &chb; hb.link_id = 6U;
    hb.mtu = UCN_MAX_FRAME_BYTES; hb.peer_node_id = UINT32_C(2);
    cab.peer = &b; cab.peer_ingress = &ba; cab.is_up = &ab_up;
    cba.peer = &a; cba.peer_ingress = &ab; cba.is_up = &ab_up;
    cbc.peer = &c; cbc.peer_ingress = &cb; cbc.is_up = &bc_up;
    ccb.peer = &b; ccb.peer_ingress = &bc; ccb.is_up = &bc_up;
    cbh.peer = &host; cbh.peer_ingress = &hb; cbh.is_up = &bh_up;
    chb.peer = &b; chb.peer_ingress = &bh; chb.is_up = &bh_up;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(3), &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&b, UINT32_C(3), &bc) == UCN_OK);
    ucn_node_set_rx_handler(&c, host_rx, &received);

    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &mcu_payload_before, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U);
    TEST_ASSERT(received.last_source == UINT32_C(1));

    TEST_ASSERT(ucn_node_register_link(&b, &bh) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&host, &hb) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&host, UINT32_C(3), &hb) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&host, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &host_payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 2U);
    TEST_ASSERT(received.last_source == UINT32_C(100));
    TEST_ASSERT(received.last_payload == host_payload);

    bh_up = false;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &mcu_payload_after, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 3U);
    TEST_ASSERT(received.last_source == UINT32_C(1));
    TEST_ASSERT(received.last_payload == mcu_payload_after);
    return 0;
}
