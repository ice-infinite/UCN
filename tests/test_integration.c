#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct integration_security_state {
    ucn_sequence_t next_sequence;
    ucn_session_id_t session_id;
} integration_security_state_t;

typedef struct integration_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool *is_up;
} integration_link_context_t;

typedef struct integration_receive_state {
    uint32_t count;
    ucn_node_id_t sources[8];
    uint8_t payloads[8];
} integration_receive_state_t;

static ucn_result_t integration_load_sequence(void *context,
                                              ucn_sequence_t *next_sequence)
{
    integration_security_state_t *state = (integration_security_state_t *)context;
    *next_sequence = state->next_sequence;
    return UCN_OK;
}

static ucn_result_t integration_store_sequence(void *context,
                                               ucn_sequence_t next_sequence)
{
    integration_security_state_t *state = (integration_security_state_t *)context;
    state->next_sequence = next_sequence;
    return UCN_OK;
}

static ucn_result_t integration_get_session(void *context, ucn_session_id_t *session_id)
{
    const integration_security_state_t *state = (const integration_security_state_t *)context;
    *session_id = state->session_id;
    return UCN_OK;
}

static ucn_result_t integration_authorize_tx(void *context, const ucn_frame_t *frame)
{
    const integration_security_state_t *state = (const integration_security_state_t *)context;
    return frame->session_id == state->session_id ? UCN_OK : UCN_ERR_ACCESS;
}

static ucn_result_t integration_authorize_rx(void *context,
                                              const ucn_link_t *ingress_link,
                                              const ucn_frame_t *frame)
{
    const integration_security_state_t *state = (const integration_security_state_t *)context;
    (void)ingress_link;
    return frame->session_id == state->session_id ? UCN_OK : UCN_ERR_ACCESS;
}

static const ucn_security_ops_t INTEGRATION_SECURITY_OPS = {
    integration_load_sequence,
    integration_store_sequence,
    integration_get_session,
    integration_authorize_tx,
    integration_authorize_rx,
    NULL, NULL, NULL, NULL
};

static ucn_result_t integration_link_send(ucn_link_t *link,
                                          const uint8_t *frame,
                                          size_t length)
{
    integration_link_context_t *context = (integration_link_context_t *)link->context;
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t integration_link_status(const ucn_link_t *link,
                                            ucn_link_status_t *status)
{
    const integration_link_context_t *context = (const integration_link_context_t *)link->context;
    status->is_up = *context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t INTEGRATION_LINK_OPS = {
    NULL, integration_link_send, NULL, integration_link_status, NULL, NULL
};

static int integration_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x98765432);
    config.node_id = id;
    config.default_hop_limit = 4U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void integration_rx(void *context, const ucn_frame_t *frame)
{
    integration_receive_state_t *state = (integration_receive_state_t *)context;

    if (state->count < 8U) {
        state->sources[state->count] = frame->source;
        state->payloads[state->count] = frame->payload[0];
    }
    state->count++;
}

int test_integration(void)
{
    uint8_t q1_payload = 0xA1U;
    uint8_t q0_payload = 0xA0U;
    uint8_t host_payload = 0xB0U;
    uint8_t repaired_payload = 0xA2U;
    uint8_t post_host_payload = 0xA3U;
    bool ab_up = true;
    bool bc_up = true;
    bool bh_up = true;
    ucn_node_t a, b, c, host;
    ucn_link_t ab, ba, bc, cb, bh, hb;
    integration_link_context_t cab, cba, cbc, ccb, cbh, chb;
    integration_security_state_t a_security = { 1U, UINT32_C(0xABCDEF01) };
    integration_security_state_t b_security = { 10U, UINT32_C(0xABCDEF01) };
    integration_security_state_t c_security = { 20U, UINT32_C(0xABCDEF01) };
    integration_security_state_t host_security = { 30U, UINT32_C(0xABCDEF01) };
    integration_receive_state_t received;
    ucn_send_request_t q1_request;
    ucn_send_request_t q0_request;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c)); (void)memset(&host, 0, sizeof(host));
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&bc, 0, sizeof(bc)); (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&bh, 0, sizeof(bh)); (void)memset(&hb, 0, sizeof(hb));
    (void)memset(&cab, 0, sizeof(cab)); (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&cbc, 0, sizeof(cbc)); (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&cbh, 0, sizeof(cbh)); (void)memset(&chb, 0, sizeof(chb));
    (void)memset(&received, 0, sizeof(received));
    (void)memset(&q1_request, 0, sizeof(q1_request));
    (void)memset(&q0_request, 0, sizeof(q0_request));
    TEST_ASSERT(integration_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(integration_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(integration_init_node(&c, UINT32_C(3)) == 0);
    TEST_ASSERT(integration_init_node(&host, UINT32_C(100)) == 0);
    TEST_ASSERT(ucn_node_set_security(&a, &INTEGRATION_SECURITY_OPS, &a_security) == UCN_OK);
    TEST_ASSERT(ucn_node_set_security(&b, &INTEGRATION_SECURITY_OPS, &b_security) == UCN_OK);
    TEST_ASSERT(ucn_node_set_security(&c, &INTEGRATION_SECURITY_OPS, &c_security) == UCN_OK);
    TEST_ASSERT(ucn_node_set_security(&host, &INTEGRATION_SECURITY_OPS, &host_security) == UCN_OK);

    ab.ops = &INTEGRATION_LINK_OPS; ab.context = &cab; ab.link_id = 1U;
    ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
    ba.ops = &INTEGRATION_LINK_OPS; ba.context = &cba; ba.link_id = 2U;
    ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
    bc.ops = &INTEGRATION_LINK_OPS; bc.context = &cbc; bc.link_id = 3U;
    bc.mtu = UCN_MAX_FRAME_BYTES; bc.peer_node_id = UINT32_C(3);
    cb.ops = &INTEGRATION_LINK_OPS; cb.context = &ccb; cb.link_id = 4U;
    cb.mtu = UCN_MAX_FRAME_BYTES; cb.peer_node_id = UINT32_C(2);
    bh.ops = &INTEGRATION_LINK_OPS; bh.context = &cbh; bh.link_id = 5U;
    bh.mtu = UCN_MAX_FRAME_BYTES; bh.peer_node_id = UINT32_C(100);
    hb.ops = &INTEGRATION_LINK_OPS; hb.context = &chb; hb.link_id = 6U;
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
    TEST_ASSERT(ucn_node_register_link(&b, &bh) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&host, &hb) == UCN_OK);
    ucn_node_set_rx_handler(&c, integration_rx, &received);

    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 100U) == UCN_OK);
    q1_request.destination = UINT32_C(3); q1_request.message_type = UCN_MSG_DATA_Q1;
    q1_request.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    q1_request.delivery = UCN_DELIVERY_BEST_EFFORT;
    q1_request.payload = &q1_payload; q1_request.payload_length = 1U;
    q0_request.destination = UINT32_C(3); q0_request.message_type = UCN_MSG_DATA_Q0;
    q0_request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    q0_request.delivery = UCN_DELIVERY_BEST_EFFORT;
    q0_request.payload = &q0_payload; q0_request.payload_length = 1U;
    TEST_ASSERT(ucn_node_enqueue(&a, &q1_request) == UCN_OK);
    TEST_ASSERT(ucn_node_enqueue(&a, &q0_request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 110U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 111U) == UCN_OK);
    TEST_ASSERT(received.count == 2U);
    TEST_ASSERT(received.payloads[0] == q0_payload);
    TEST_ASSERT(received.payloads[1] == q1_payload);

    TEST_ASSERT(ucn_node_discover_route(&host, UINT32_C(3), 120U) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&host, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &host_payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 3U);
    TEST_ASSERT(received.sources[2] == UINT32_C(100));

    bc_up = false;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &repaired_payload, 1U) == UCN_ERR_LINK_DOWN);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &repaired_payload, 1U) == UCN_ERR_NOT_FOUND);
    bc_up = true;
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 200U) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &repaired_payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 4U);
    TEST_ASSERT(received.payloads[3] == repaired_payload);

    bh_up = false;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &post_host_payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 5U);
    TEST_ASSERT(received.payloads[4] == post_host_payload);
    TEST_ASSERT(a_security.next_sequence == 8U);
    return 0;
}
