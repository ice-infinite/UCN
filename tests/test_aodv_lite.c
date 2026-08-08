#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct aodv_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool *is_up;
    bool *drop_frames;
} aodv_link_context_t;

typedef struct aodv_receive_state {
    uint32_t count;
    uint8_t last_payload;
} aodv_receive_state_t;

static ucn_result_t aodv_link_send(ucn_link_t *link,
                                   const uint8_t *frame,
                                   size_t length)
{
    aodv_link_context_t *context = (aodv_link_context_t *)link->context;

    if (*context->drop_frames) {
        return UCN_OK;
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t aodv_link_status(const ucn_link_t *link, ucn_link_status_t *status)
{
    const aodv_link_context_t *context = (const aodv_link_context_t *)link->context;

    status->is_up = *context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t AODV_LINK_OPS = {
    NULL, aodv_link_send, NULL, aodv_link_status, NULL, NULL
};

static int aodv_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x12345678);
    config.node_id = id;
    config.default_hop_limit = 4U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void aodv_rx(void *context, const ucn_frame_t *frame)
{
    aodv_receive_state_t *state = (aodv_receive_state_t *)context;

    state->count++;
    state->last_payload = frame->payload[0];
}

int test_aodv_lite(void)
{
    uint8_t first_payload = 0x31U;
    uint8_t repaired_payload = 0x32U;
    uint8_t duplicate_payload[16];
    uint8_t duplicate_encoded[UCN_MAX_FRAME_BYTES];
    size_t duplicate_length = 0U;
    bool ab_up = true;
    bool bc_up = true;
    bool ab_drop = true;
    bool ba_drop = false;
    bool bc_drop = false;
    bool cb_drop = false;
    ucn_node_t a, b, c;
    ucn_link_t ab, ba, bc, cb;
    aodv_link_context_t cab, cba, cbc, ccb;
    aodv_receive_state_t received;
    ucn_frame_t duplicate_request;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&bc, 0, sizeof(bc));
    (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&cbc, 0, sizeof(cbc));
    (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(aodv_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(aodv_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(aodv_init_node(&c, UINT32_C(3)) == 0);

    ab.ops = &AODV_LINK_OPS; ab.context = &cab; ab.link_id = 1U;
    ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
    ba.ops = &AODV_LINK_OPS; ba.context = &cba; ba.link_id = 2U;
    ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
    bc.ops = &AODV_LINK_OPS; bc.context = &cbc; bc.link_id = 3U;
    bc.mtu = UCN_MAX_FRAME_BYTES; bc.peer_node_id = UINT32_C(3);
    cb.ops = &AODV_LINK_OPS; cb.context = &ccb; cb.link_id = 4U;
    cb.mtu = UCN_MAX_FRAME_BYTES; cb.peer_node_id = UINT32_C(2);
    cab.peer = &b; cab.peer_ingress = &ba; cab.is_up = &ab_up; cab.drop_frames = &ab_drop;
    cba.peer = &a; cba.peer_ingress = &ab; cba.is_up = &ab_up; cba.drop_frames = &ba_drop;
    cbc.peer = &c; cbc.peer_ingress = &cb; cbc.is_up = &bc_up; cbc.drop_frames = &bc_drop;
    ccb.peer = &b; ccb.peer_ingress = &bc; ccb.is_up = &bc_up; ccb.drop_frames = &cb_drop;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    ucn_node_set_rx_handler(&c, aodv_rx, &received);

    (void)memset(&duplicate_request, 0, sizeof(duplicate_request));
    duplicate_payload[0] = 0U; duplicate_payload[1] = 0U;
    duplicate_payload[2] = 0U; duplicate_payload[3] = 1U;
    duplicate_payload[4] = 0U; duplicate_payload[5] = 0U;
    duplicate_payload[6] = 0U; duplicate_payload[7] = 99U;
    duplicate_payload[8] = 0U; duplicate_payload[9] = 0U;
    duplicate_payload[10] = 0U; duplicate_payload[11] = 1U;
    duplicate_payload[12] = 0U; duplicate_payload[13] = 0U;
    duplicate_payload[14] = 0U; duplicate_payload[15] = 0U;
    duplicate_request.message_type = UCN_MSG_ROUTE_REQ;
    duplicate_request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    duplicate_request.hop_limit = 4U;
    duplicate_request.network_id = UINT32_C(0x12345678);
    duplicate_request.source = UINT32_C(1);
    duplicate_request.destination = UCN_NODE_BROADCAST;
    duplicate_request.sequence = UINT32_C(0xAA);
    duplicate_request.payload = duplicate_payload;
    duplicate_request.payload_length = (uint16_t)sizeof(duplicate_payload);
    TEST_ASSERT(ucn_frame_encode(&duplicate_request, duplicate_encoded,
                                 sizeof(duplicate_encoded), &duplicate_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&b, &ba, duplicate_encoded, duplicate_length) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_receive(&b, &ba, duplicate_encoded, duplicate_length) == UCN_ERR_REPLAY);

    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 10U) == UCN_OK);
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 20U) == UCN_OK);
    TEST_ASSERT(ucn_node_get_stats(&a)->route_requests_sent == 1U);
    TEST_ASSERT(ucn_node_route_pending(&a, UINT32_C(3)));
    TEST_ASSERT(ucn_node_step(&a, 1011U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(!ucn_node_route_pending(&a, UINT32_C(3)));

    ab_drop = false;
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 1100U) == UCN_OK);
    TEST_ASSERT(!ucn_node_route_pending(&a, UINT32_C(3)));
    TEST_ASSERT(ucn_node_get_stats(&a)->route_requests_sent == 2U);
    TEST_ASSERT(ucn_node_get_stats(&c)->route_replies_sent == 1U);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U);
    TEST_ASSERT(received.last_payload == first_payload);

    bc_up = false;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_ERR_LINK_DOWN);
    TEST_ASSERT(ucn_node_get_stats(&b)->route_errors_sent == 1U);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &first_payload, 1U) == UCN_ERR_NOT_FOUND);

    bc_up = true;
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 1200U) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &repaired_payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 2U);
    TEST_ASSERT(received.last_payload == repaired_payload);
    TEST_ASSERT(ucn_node_step(&a, 31200U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &repaired_payload, 1U) == UCN_ERR_NOT_FOUND);
    return 0;
}
