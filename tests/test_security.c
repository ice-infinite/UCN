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
    bool fail_rotate;
    uint32_t rotate_calls;
    struct security_provider_state *rotation_peer;
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

static ucn_result_t security_rotate(void *context,
                                    ucn_session_id_t current_session_id,
                                    ucn_session_id_t *new_session_id,
                                    ucn_sequence_t *next_sequence)
{
    security_provider_state_t *state = (security_provider_state_t *)context;
    ucn_session_id_t rotated_session;

    state->rotate_calls++;
    if (state->fail_rotate || current_session_id != state->session_id) {
        return UCN_ERR_SECURITY;
    }
    rotated_session = state->session_id + 1U;
    if (rotated_session == 0U) {
        rotated_session = 1U;
    }
    state->session_id = rotated_session;
    state->next_sequence = 1U;
    if (state->rotation_peer != NULL) {
        state->rotation_peer->session_id = rotated_session;
    }
    *new_session_id = rotated_session;
    *next_sequence = 1U;
    return UCN_OK;
}

static ucn_result_t security_seal(void *context,
                                  const ucn_frame_t *frame,
                                  const uint8_t *plaintext,
                                  uint16_t plaintext_length,
                                  uint8_t *ciphertext,
                                  uint8_t auth_tag[UCN_E2E_TAG_SIZE])
{
    (void)context;
    (void)frame;
    if (plaintext_length != 0U) {
        (void)memcpy(ciphertext, plaintext, plaintext_length);
    }
    (void)memset(auth_tag, 0xA5, UCN_E2E_TAG_SIZE);
    return UCN_OK;
}

static ucn_result_t security_open(void *context,
                                  const ucn_link_t *ingress_link,
                                  const ucn_frame_t *frame,
                                  const uint8_t *ciphertext,
                                  uint16_t ciphertext_length,
                                  const uint8_t auth_tag[UCN_E2E_TAG_SIZE],
                                  uint8_t *plaintext)
{
    (void)context;
    (void)ingress_link;
    (void)frame;
    (void)auth_tag;
    if (ciphertext_length != 0U) {
        (void)memcpy(plaintext, ciphertext, ciphertext_length);
    }
    return UCN_OK;
}

static const ucn_security_ops_t TEST_SECURITY_OPS = {
    security_load, security_store, security_session, security_authorize_tx, security_authorize_rx,
    NULL, security_seal, security_open, security_rotate
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
    ucn_node_t a, a_after_reboot, b, gated;
    ucn_link_t ab, ba, reboot_ab;
    security_link_context_t cab, cba, creboot_ab;
    security_provider_state_t a_security = {
        .next_sequence = 1U,
        .session_id = 0x77U
    };
    security_provider_state_t b_security = {
        .next_sequence = 70U,
        .session_id = 0x77U
    };
    security_provider_state_t bad_security = {
        .next_sequence = 1U,
        .session_id = 0x77U,
        .fail_load = true
    };
    security_provider_state_t gated_security = {
        .next_sequence = 1U,
        .session_id = 0x88U
    };
    security_receive_state_t received;
    ucn_security_ops_t incomplete_ops = TEST_SECURITY_OPS;
    ucn_security_ops_t no_rotation_ops = TEST_SECURITY_OPS;
    ucn_security_ops_t authorization_only_ops = TEST_SECURITY_OPS;
    const ucn_security_policy_t production_policy = {
        UCN_SECURITY_TX_E2E_PROTECTED,
        UCN_SECURITY_RX_ENCRYPTED_ONLY,
        UCN_SECURITY_FORWARD_OPAQUE_E2E_ONLY
    };
    const ucn_security_policy_t insecure_endpoint_policy = {
        UCN_SECURITY_TX_PLAIN,
        UCN_SECURITY_RX_BOTH,
        UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E
    };

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&a_after_reboot, 0, sizeof(a_after_reboot));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&gated, 0, sizeof(gated));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&reboot_ab, 0, sizeof(reboot_ab));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&creboot_ab, 0, sizeof(creboot_ab));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(security_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(security_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(security_init_node(&gated, UINT32_C(3)) == 0);
    TEST_ASSERT(ucn_node_security_ready(&gated));
    TEST_ASSERT(ucn_node_set_security_required(&gated, true) == UCN_OK);
    TEST_ASSERT(!ucn_node_security_ready(&gated));
    TEST_ASSERT(ucn_node_step(&gated, 1U) == UCN_ERR_SECURITY);
    TEST_ASSERT(ucn_node_send(&gated, UINT32_C(4), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_ERR_SECURITY);
    authorization_only_ops.seal = NULL;
    authorization_only_ops.open = NULL;
    TEST_ASSERT(ucn_node_set_security(&gated, &authorization_only_ops,
                                      &gated_security) == UCN_ERR_SECURITY);
    TEST_ASSERT(ucn_node_set_security(&gated, &TEST_SECURITY_OPS,
                                      &gated_security) == UCN_OK);
    TEST_ASSERT(!ucn_node_security_ready(&gated));
    TEST_ASSERT(ucn_node_step(&gated, 2U) == UCN_ERR_SECURITY);
    TEST_ASSERT(ucn_node_set_security_policy(&gated, &production_policy) == UCN_OK);
    TEST_ASSERT(ucn_node_security_ready(&gated));
    TEST_ASSERT(ucn_node_set_endpoint_security_policy(
                    &gated, (ucn_endpoint_t)0x40U,
                    &insecure_endpoint_policy) == UCN_OK);
    TEST_ASSERT(!ucn_node_security_ready(&gated));
    TEST_ASSERT(ucn_node_set_endpoint_security_policy(
                    &gated, (ucn_endpoint_t)0x40U, NULL) == UCN_OK);
    TEST_ASSERT(ucn_node_security_ready(&gated));
    TEST_ASSERT(ucn_node_step(&gated, 3U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_set_security(&gated, NULL, NULL) == UCN_OK);
    TEST_ASSERT(!ucn_node_security_ready(&gated));
    incomplete_ops.authorize_rx = NULL;
    TEST_ASSERT(ucn_node_set_security(&a, &incomplete_ops, &a_security) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_set_security(&a, &TEST_SECURITY_OPS, &bad_security) == UCN_ERR_SECURITY);
    TEST_ASSERT(ucn_node_set_security(&a, &TEST_SECURITY_OPS, &a_security) == UCN_OK);
    TEST_ASSERT(ucn_node_set_plain_session_id(&a, UINT32_C(9)) == UCN_ERR_CONFIG);
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
    TEST_ASSERT(a_security.next_sequence == 2U);
    TEST_ASSERT(received.count == 1U);
    TEST_ASSERT(received.last_sequence == 1U);
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
    TEST_ASSERT(a_security.next_sequence == 3U);
    TEST_ASSERT(received.count == 2U);
    TEST_ASSERT(received.last_sequence == 2U);

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
    TEST_ASSERT(a_security.next_sequence == 5U);

    /* A Core never wraps Sequence inside the same Session.  Without an
     * explicit Provider rollover it fails closed; Provider failure also keeps
     * the old state.  A successful atomic rollover starts the new Session at
     * Sequence 1, which the receiver accepts even though old Session/1 is in
     * its duplicate source window. */
    a_security.fail_store = false;
    a_security.next_sequence = UCN_SEQUENCE_ROTATION_THRESHOLD;
    a_after_reboot.next_sequence = UCN_SEQUENCE_ROTATION_THRESHOLD;
    no_rotation_ops.rotate_session = NULL;
    TEST_ASSERT(ucn_node_set_security(&a_after_reboot, &no_rotation_ops,
                                      &a_security) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&a_after_reboot, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_ERR_SECURITY);
    TEST_ASSERT(a_security.session_id == 0x77U && a_security.rotate_calls == 0U);

    a_security.fail_rotate = true;
    TEST_ASSERT(ucn_node_set_security(&a_after_reboot, &TEST_SECURITY_OPS,
                                      &a_security) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&a_after_reboot, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_ERR_SECURITY);
    TEST_ASSERT(a_security.session_id == 0x77U && a_security.rotate_calls == 1U);

    a_security.fail_rotate = false;
    a_security.rotation_peer = &b_security;
    TEST_ASSERT(ucn_node_send(&a_after_reboot, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(a_security.session_id == 0x78U && a_security.next_sequence == 2U &&
                a_security.rotate_calls == 2U &&
                a_after_reboot.stats.session_rotations == 1U);
    TEST_ASSERT(received.count == 3U && received.last_sequence == 1U &&
                received.last_session == 0x78U);
    return 0;
}
