#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct security_provider_state {
    ucn_sequence_t next_sequence;
    ucn_session_id_t session_id;
    bool fail_load;
    bool fail_store;
    bool deny_tx;
    bool deny_rx;
} security_provider_state_t;

typedef struct security_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
} security_link_context_t;

typedef struct security_receive_state {
    uint32_t count;
    ucn_sequence_t last_sequence;
    ucn_session_id_t last_session;
} security_receive_state_t;

static ucn_result_t security_load(void *context, ucn_sequence_t *next_sequence)
{
    security_provider_state_t *state = (security_provider_state_t *)context;

    if (state->fail_load) {
        return UCN_ERR_SECURITY;
    }
    *next_sequence = state->next_sequence;
    return UCN_OK;
}

static ucn_result_t security_store(void *context, ucn_sequence_t next_sequence)
{
    security_provider_state_t *state = (security_provider_state_t *)context;

    if (state->fail_store) {
        return UCN_ERR_SECURITY;
    }
    state->next_sequence = next_sequence;
    return UCN_OK;
}

static ucn_result_t security_session(void *context, ucn_session_id_t *session_id)
{
    security_provider_state_t *state = (security_provider_state_t *)context;

    *session_id = state->session_id;
    return UCN_OK;
}

static ucn_result_t security_authorize_tx(void *context, const ucn_frame_t *frame)
{
    security_provider_state_t *state = (security_provider_state_t *)context;

    if (state->deny_tx || frame->session_id != state->session_id) {
        return UCN_ERR_ACCESS;
    }
    return UCN_OK;
}

static ucn_result_t security_authorize_rx(void *context,
                                          const ucn_link_t *ingress_link,
                                          const ucn_frame_t *frame)
{
    security_provider_state_t *state = (security_provider_state_t *)context;

    (void)ingress_link;
    if (state->deny_rx || frame->session_id != state->session_id) {
        return UCN_ERR_ACCESS;
    }
    return UCN_OK;
}

static const ucn_security_ops_t TEST_SECURITY_OPS = {
    security_load, security_store, security_session, security_authorize_tx, security_authorize_rx,
    NULL, NULL, NULL
};

static ucn_result_t security_link_send(ucn_link_t *link,
                                       const uint8_t *frame,
                                       size_t length)
{
    security_link_context_t *context = (security_link_context_t *)link->context;
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t security_link_status(const ucn_link_t *link, ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t SECURITY_LINK_OPS = {
    NULL, security_link_send, NULL, security_link_status, NULL, NULL
};

static int security_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x55667788);
    config.node_id = id;
    config.default_hop_limit = 2U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void security_rx(void *context, const ucn_frame_t *frame)
{
    security_receive_state_t *state = (security_receive_state_t *)context;

    state->count++;
    state->last_sequence = frame->sequence;
    state->last_session = frame->session_id;
}

int test_security(void)
{
    uint8_t payload = 0x42U;
    ucn_node_t a, a_after_reboot, b;
    ucn_link_t ab, ba, reboot_ab;
    security_link_context_t cab, cba, creboot_ab;
    security_provider_state_t a_security = { 40U, 0x77U, false, false, false, false };
    security_provider_state_t b_security = { 70U, 0x77U, false, false, false, false };
    security_provider_state_t bad_security = { 1U, 0x77U, true, false, false, false };
    security_receive_state_t received;
    ucn_security_ops_t incomplete_ops = TEST_SECURITY_OPS;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&a_after_reboot, 0, sizeof(a_after_reboot));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&reboot_ab, 0, sizeof(reboot_ab));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&creboot_ab, 0, sizeof(creboot_ab));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(security_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(security_init_node(&b, UINT32_C(2)) == 0);
    incomplete_ops.authorize_rx = NULL;
    TEST_ASSERT(ucn_node_set_security(&a, &incomplete_ops, &a_security) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_set_security(&a, &TEST_SECURITY_OPS, &bad_security) == UCN_ERR_SECURITY);
    TEST_ASSERT(ucn_node_set_security(&a, &TEST_SECURITY_OPS, &a_security) == UCN_OK);
    TEST_ASSERT(ucn_node_set_security(&b, &TEST_SECURITY_OPS, &b_security) == UCN_OK);

    ab.ops = &SECURITY_LINK_OPS; ab.context = &cab; ab.link_id = 1U;
    ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
    ba.ops = &SECURITY_LINK_OPS; ba.context = &cba; ba.link_id = 2U;
    ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    ucn_node_set_rx_handler(&b, security_rx, &received);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(a_security.next_sequence == 41U);
    TEST_ASSERT(received.count == 1U);
    TEST_ASSERT(received.last_sequence == 40U);
    TEST_ASSERT(received.last_session == 0x77U);

    TEST_ASSERT(security_init_node(&a_after_reboot, UINT32_C(1)) == 0);
    TEST_ASSERT(ucn_node_set_security(&a_after_reboot, &TEST_SECURITY_OPS, &a_security) == UCN_OK);
    reboot_ab.ops = &SECURITY_LINK_OPS; reboot_ab.context = &creboot_ab;
    reboot_ab.link_id = 3U; reboot_ab.mtu = UCN_MAX_FRAME_BYTES;
    reboot_ab.peer_node_id = UINT32_C(2);
    creboot_ab.peer = &b; creboot_ab.peer_ingress = &ba;
    TEST_ASSERT(ucn_node_register_link(&a_after_reboot, &reboot_ab) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&a_after_reboot, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(a_security.next_sequence == 42U);
    TEST_ASSERT(received.count == 2U);
    TEST_ASSERT(received.last_sequence == 41U);

    b_security.deny_rx = true;
    TEST_ASSERT(ucn_node_send(&a_after_reboot, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_ERR_ACCESS);
    TEST_ASSERT(received.count == 2U);
    b_security.deny_rx = false;
    a_security.deny_tx = true;
    TEST_ASSERT(ucn_node_send(&a_after_reboot, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_ERR_ACCESS);
    a_security.deny_tx = false;
    a_security.fail_store = true;
    TEST_ASSERT(ucn_node_send(&a_after_reboot, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_ERR_SECURITY);
    TEST_ASSERT(a_security.next_sequence == 44U);
    return 0;
}
