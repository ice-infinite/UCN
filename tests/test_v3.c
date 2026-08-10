#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

/* The historical filename is retained so old failure logs remain searchable.
 * This suite exercises the current v5 wire/security behavior and the explicit
 * rejection of older protocol versions; "v3" prefixes below are test-local
 * fixture names, not production API or current-version claims. */

typedef struct v3_provider_state {
    ucn_sequence_t next_sequence;
    ucn_session_id_t session_id;
    uint8_t test_key;
    uint32_t seal_calls;
    uint32_t open_calls;
} v3_provider_state_t;

typedef struct v3_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool deliver;
    bool tamper_next;
} v3_link_context_t;

typedef struct v3_receive_state {
    uint32_t count;
    uint8_t payload;
    bool protected_frame;
} v3_receive_state_t;

static ucn_result_t v3_load_sequence(void *context, ucn_sequence_t *next_sequence)
{
    *next_sequence = ((v3_provider_state_t *)context)->next_sequence;
    return UCN_OK;
}

static ucn_result_t v3_store_sequence(void *context, ucn_sequence_t next_sequence)
{
    ((v3_provider_state_t *)context)->next_sequence = next_sequence;
    return UCN_OK;
}

static ucn_result_t v3_session(void *context, ucn_session_id_t *session_id)
{
    *session_id = ((v3_provider_state_t *)context)->session_id;
    return UCN_OK;
}

static ucn_result_t v3_authorize_tx(void *context, const ucn_frame_t *frame)
{
    return frame->session_id == ((v3_provider_state_t *)context)->session_id ?
           UCN_OK : UCN_ERR_ACCESS;
}

static ucn_result_t v3_authorize_rx(void *context,
                                    const ucn_link_t *ingress_link,
                                    const ucn_frame_t *frame)
{
    (void)ingress_link;
    return frame->session_id == ((v3_provider_state_t *)context)->session_id ?
           UCN_OK : UCN_ERR_ACCESS;
}

/* This is an explicitly non-production reversible test provider.  It proves
 * the Core seal/open, AAD and transparent-forwarding contracts only. */
static uint8_t v3_test_tag_byte(const ucn_frame_t *frame,
                                const uint8_t *ciphertext,
                                uint16_t ciphertext_length,
                                uint8_t key,
                                size_t tag_index)
{
    uint8_t aad[32];
    size_t aad_length = 0U;
    uint8_t value = (uint8_t)(key + (uint8_t)tag_index);
    size_t index;

    (void)ucn_frame_write_e2e_aad(frame, aad, sizeof(aad), &aad_length);
    for (index = 0U; index < aad_length; ++index) {
        value = (uint8_t)(value + aad[index]);
    }
    for (index = 0U; index < ciphertext_length; ++index) {
        value = (uint8_t)(value + ciphertext[index]);
    }
    return value;
}

static ucn_result_t v3_seal(void *context,
                            const ucn_frame_t *frame,
                            const uint8_t *plaintext,
                            uint16_t plaintext_length,
                            uint8_t *ciphertext,
                            uint8_t auth_tag[UCN_E2E_TAG_SIZE])
{
    v3_provider_state_t *state = (v3_provider_state_t *)context;
    size_t index;

    for (index = 0U; index < plaintext_length; ++index) {
        ciphertext[index] = (uint8_t)(plaintext[index] ^ state->test_key);
    }
    for (index = 0U; index < UCN_E2E_TAG_SIZE; ++index) {
        auth_tag[index] = v3_test_tag_byte(frame, ciphertext, plaintext_length,
                                           state->test_key, index);
    }
    state->seal_calls++;
    return UCN_OK;
}

static ucn_result_t v3_open(void *context,
                            const ucn_link_t *ingress_link,
                            const ucn_frame_t *frame,
                            const uint8_t *ciphertext,
                            uint16_t ciphertext_length,
                            const uint8_t auth_tag[UCN_E2E_TAG_SIZE],
                            uint8_t *plaintext)
{
    v3_provider_state_t *state = (v3_provider_state_t *)context;
    size_t index;

    (void)ingress_link;
    for (index = 0U; index < UCN_E2E_TAG_SIZE; ++index) {
        if (auth_tag[index] != v3_test_tag_byte(frame, ciphertext, ciphertext_length,
                                                state->test_key, index)) {
            return UCN_ERR_SECURITY;
        }
    }
    for (index = 0U; index < ciphertext_length; ++index) {
        plaintext[index] = (uint8_t)(ciphertext[index] ^ state->test_key);
    }
    state->open_calls++;
    return UCN_OK;
}

static const ucn_security_ops_t V3_SECURITY_OPS = {
    v3_load_sequence, v3_store_sequence, v3_session, v3_authorize_tx,
    v3_authorize_rx, NULL, v3_seal, v3_open, NULL
};

static const ucn_security_ops_t V3_FORWARD_ONLY_SECURITY_OPS = {
    v3_load_sequence, v3_store_sequence, v3_session, v3_authorize_tx,
    v3_authorize_rx, NULL, NULL, NULL, NULL
};

static ucn_result_t v3_link_send(ucn_link_t *link,
                                 const uint8_t *frame,
                                 size_t length)
{
    v3_link_context_t *context = (v3_link_context_t *)link->context;

    if (!context->deliver) {
        return UCN_OK;
    }

    if (context->tamper_next) {
        uint8_t modified[UCN_MAX_FRAME_BYTES];
        uint8_t reencoded[UCN_MAX_FRAME_BYTES];
        ucn_frame_t decoded;
        size_t encoded_length = 0U;
        size_t header_size;

        context->tamper_next = false;
        (void)memcpy(modified, frame, length);
        if (ucn_frame_decode(modified, length, &decoded) != UCN_OK) {
            return UCN_ERR_MALFORMED;
        }
        header_size = ucn_frame_header_size_for_profile(decoded.wire_profile,
                                                        decoded.flags);
        modified[header_size] ^= 0x01U;
        if (ucn_frame_encode(&decoded, reencoded, sizeof(reencoded),
                             &encoded_length) != UCN_OK) {
            return UCN_ERR_MALFORMED;
        }
        return ucn_node_receive(context->peer, context->peer_ingress, reencoded,
                                encoded_length);
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t v3_link_status(const ucn_link_t *link, ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t V3_LINK_OPS = {
    NULL, v3_link_send, NULL, v3_link_status, NULL, NULL
};

static int v3_init_node(ucn_node_t *node, ucn_node_id_t node_id)
{
    ucn_config_t config;
    ucn_result_t result;

    config.network_id = UINT32_C(42);
    config.node_id = node_id;
    config.default_hop_limit = 4U;
    result = ucn_node_init(node, &config);
    if (result != UCN_OK) {
        return 1;
    }
    return ucn_node_set_wire_profiles(node, UCN_WIRE_PROFILE_W0_LOCAL,
                                      UCN_WIRE_PROFILE_W0_LOCAL) == UCN_OK ?
               0 : 1;
}

static void v3_receive(void *context, const ucn_frame_t *frame)
{
    v3_receive_state_t *state = (v3_receive_state_t *)context;

    state->count++;
    state->payload = frame->payload[0];
    state->protected_frame =
        (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U;
}

static int v3_test_frame_format(void)
{
    uint8_t payload[] = { 0x11U, 0x22U };
    uint8_t tag[UCN_E2E_TAG_SIZE];
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    uint8_t aad_a[32], aad_b[32];
    ucn_frame_t frame;
    ucn_frame_t decoded;
    size_t encoded_length = 0U;
    size_t aad_length_a = 0U;
    size_t aad_length_b = 0U;
    size_t index;
    uint8_t plaintext[] = { 0x31U, 0x32U };
    uint8_t ciphertext[sizeof(plaintext)];
    uint8_t bound_tag[UCN_E2E_TAG_SIZE];
    uint8_t opened[sizeof(plaintext)];
    v3_provider_state_t provider = { 1U, 10U, 0x5CU, 0U, 0U };
    ucn_frame_t bound;
    ucn_frame_t tampered;

    for (index = 0U; index < sizeof(tag); ++index) {
        tag[index] = (uint8_t)index;
    }
    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_DATA_Q1;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_ROUTE_EXTENSION;
    frame.hop_limit = 3U;
    frame.network_id = UINT32_C(0x11223344);
    frame.source = UINT32_C(1);
    frame.destination = UINT32_C(3);
    frame.sequence = UINT32_C(9);
    frame.session_id = UINT32_C(10);
    frame.has_route_extension = true;
    frame.route_epoch = UINT16_C(0x1234);
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);

    TEST_ASSERT(ucn_frame_encoded_size(&frame) ==
                UCN_FRAME_ROUTE_HEADER_SIZE + sizeof(payload));
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(encoded_length == UCN_FRAME_ROUTE_HEADER_SIZE + sizeof(payload));
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
    TEST_ASSERT(decoded.has_route_extension && decoded.route_epoch == UINT16_C(0x1234));
    TEST_ASSERT(ucn_frame_write_e2e_aad(&frame, aad_a, sizeof(aad_a),
                                         &aad_length_a) == UCN_OK);
    decoded.hop_limit = 1U;
    decoded.route_epoch = UINT16_C(0xABCD);
    TEST_ASSERT(ucn_frame_write_e2e_aad(&decoded, aad_b, sizeof(aad_b),
                                         &aad_length_b) == UCN_OK);
    TEST_ASSERT(aad_length_a == ucn_frame_e2e_aad_size() &&
                aad_length_a == aad_length_b &&
                memcmp(aad_a, aad_b, aad_length_a) == 0);

    frame.flags |= UCN_FRAME_FLAG_E2E_PROTECTED;
    frame.auth_tag = tag;
    TEST_ASSERT(ucn_frame_encoded_size(&frame) == UCN_FRAME_ROUTE_HEADER_SIZE +
                sizeof(payload) + UCN_E2E_TAG_SIZE);
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
    TEST_ASSERT(decoded.auth_tag != NULL &&
                memcmp(decoded.auth_tag, tag, sizeof(tag)) == 0);
    encoded[UCN_FRAME_ROUTE_HEADER_SIZE + sizeof(payload)] ^= 0x01U;
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_CRC);
    encoded[2] = 4U;
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_VERSION);

    (void)memset(&bound, 0, sizeof(bound));
    bound.message_type = UCN_MSG_DATA_Q1;
    bound.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    bound.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    bound.flags = UCN_FRAME_FLAG_E2E_PROTECTED;
    bound.hop_limit = 4U;
    bound.network_id = 42U;
    bound.source = 1U;
    bound.destination = 3U;
    bound.sequence = 9U;
    bound.session_id = 10U;
    bound.payload = plaintext;
    bound.payload_length = (uint16_t)sizeof(plaintext);
    TEST_ASSERT(v3_seal(&provider, &bound, plaintext,
                        (uint16_t)sizeof(plaintext), ciphertext,
                        bound_tag) == UCN_OK);
    TEST_ASSERT(v3_open(&provider, NULL, &bound, ciphertext,
                        (uint16_t)sizeof(ciphertext), bound_tag, opened) == UCN_OK);
    TEST_ASSERT(memcmp(opened, plaintext, sizeof(plaintext)) == 0);

    tampered = bound;
    tampered.wire_profile = UCN_WIRE_PROFILE_W1_EDGE;
    TEST_ASSERT(v3_open(&provider, NULL, &tampered, ciphertext,
                        (uint16_t)sizeof(ciphertext), bound_tag, opened) ==
                UCN_ERR_SECURITY);
    tampered = bound;
    tampered.destination = 2U;
    TEST_ASSERT(v3_open(&provider, NULL, &tampered, ciphertext,
                        (uint16_t)sizeof(ciphertext), bound_tag, opened) ==
                UCN_ERR_SECURITY);
    tampered = bound;
    tampered.payload_length = 1U;
    TEST_ASSERT(v3_open(&provider, NULL, &tampered, ciphertext,
                        (uint16_t)sizeof(ciphertext), bound_tag, opened) ==
                UCN_ERR_SECURITY);
    return 0;
}

static int v3_test_transparent_security(void)
{
    const ucn_endpoint_t endpoint = UCN_STATIC_ENDPOINT_FIRST;
    uint8_t payload = 0x5AU;
    ucn_node_t a, b, c;
    ucn_link_t ab, ba, bc, cb;
    v3_link_context_t cab, cba, cbc, ccb;
    v3_provider_state_t a_state = { 1U, 0x88U, 0x5CU, 0U, 0U };
    v3_provider_state_t b_state = { 1U, 0x88U, 0U, 0U, 0U };
    v3_provider_state_t c_state = { 1U, 0x88U, 0x5CU, 0U, 0U };
    v3_receive_state_t received;
    ucn_security_policy_t encrypted = {
        UCN_SECURITY_TX_E2E_PROTECTED,
        UCN_SECURITY_RX_ENCRYPTED_ONLY,
        UCN_SECURITY_FORWARD_OPAQUE_E2E_ONLY
    };
    ucn_security_policy_t plain = {
        UCN_SECURITY_TX_PLAIN,
        UCN_SECURITY_RX_BOTH,
        UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E
    };

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c)); (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba)); (void)memset(&bc, 0, sizeof(bc));
    (void)memset(&cb, 0, sizeof(cb)); (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba)); (void)memset(&cbc, 0, sizeof(cbc));
    (void)memset(&ccb, 0, sizeof(ccb)); (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(v3_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(v3_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(v3_init_node(&c, UINT32_C(3)) == 0);
    TEST_ASSERT(ucn_node_set_security(&a, &V3_SECURITY_OPS, &a_state) == UCN_OK);
    TEST_ASSERT(ucn_node_set_security(&b, &V3_FORWARD_ONLY_SECURITY_OPS, &b_state) == UCN_OK);
    TEST_ASSERT(ucn_node_set_security(&c, &V3_SECURITY_OPS, &c_state) == UCN_OK);
    TEST_ASSERT(ucn_node_set_endpoint_security_policy(&a, endpoint, &encrypted) == UCN_OK);
    TEST_ASSERT(ucn_node_set_endpoint_security_policy(&b, endpoint, &encrypted) == UCN_OK);
    TEST_ASSERT(ucn_node_set_endpoint_security_policy(&c, endpoint, &encrypted) == UCN_OK);

    ab.ops = &V3_LINK_OPS; ab.context = &cab; ab.link_id = 1U; ab.mtu = UCN_MAX_FRAME_BYTES;
    ab.peer_node_id = UINT32_C(2);
    ba.ops = &V3_LINK_OPS; ba.context = &cba; ba.link_id = 2U; ba.mtu = UCN_MAX_FRAME_BYTES;
    ba.peer_node_id = UINT32_C(1);
    bc.ops = &V3_LINK_OPS; bc.context = &cbc; bc.link_id = 3U; bc.mtu = UCN_MAX_FRAME_BYTES;
    bc.peer_node_id = UINT32_C(3);
    cb.ops = &V3_LINK_OPS; cb.context = &ccb; cb.link_id = 4U; cb.mtu = UCN_MAX_FRAME_BYTES;
    cb.peer_node_id = UINT32_C(2);
    cab.peer = &b; cab.peer_ingress = &ba; cba.peer = &a; cba.peer_ingress = &ab;
    cbc.peer = &c; cbc.peer_ingress = &cb; ccb.peer = &b; ccb.peer_ingress = &bc;
    cba.deliver = true; cbc.deliver = true; ccb.deliver = true;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    ucn_node_set_rx_handler(&c, v3_receive, &received);

    /* Endpoint Q1 在未知多跳路径上只保留固定待发项；Q0 绝不等待。 */
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(3), endpoint,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(a.stats.q1_route_wait_queued == 1U && received.count == 0U);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(3), endpoint,
                                       UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) == UCN_ERR_NOT_FOUND);
    cab.deliver = true;
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 200U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 201U) == UCN_OK);
    TEST_ASSERT(received.count == 1U && received.payload == payload &&
                received.protected_frame);

    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(3), endpoint,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 2U && received.payload == payload &&
                received.protected_frame);
    TEST_ASSERT(a_state.seal_calls == 2U && c_state.open_calls == 2U &&
                b_state.open_calls == 0U &&
                b.stats.e2e_protected_forwarded == 2U);

    cab.tamper_next = true;
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(3), endpoint,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_ERR_SECURITY);
    TEST_ASSERT(received.count == 2U);

    TEST_ASSERT(ucn_node_set_endpoint_security_policy(&a, endpoint, &plain) == UCN_OK);
    TEST_ASSERT(ucn_node_set_endpoint_security_policy(&b, endpoint, &plain) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(3), endpoint,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_ERR_ACCESS);
    TEST_ASSERT(received.count == 2U);

    TEST_ASSERT(ucn_node_set_endpoint_security_policy(&c, endpoint, &plain) == UCN_OK);
    TEST_ASSERT(ucn_node_set_endpoint_security_policy(&b, endpoint, &encrypted) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(3), endpoint,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_ERR_ACCESS);
    return 0;
}

static int v3_test_q1_wait_bounds(void)
{
    ucn_node_t node;
    ucn_link_t link;
    v3_link_context_t context;
    uint8_t first_payload = 0x10U;
    uint8_t latest_payload = 0x11U;
    size_t index;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    (void)memset(&context, 0, sizeof(context));
    TEST_ASSERT(v3_init_node(&node, UINT32_C(10)) == 0);
    link.ops = &V3_LINK_OPS;
    link.context = &context;
    link.link_id = 9U;
    link.mtu = UCN_MAX_FRAME_BYTES;
    link.peer_node_id = UINT32_C(11);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);

    TEST_ASSERT(ucn_node_send_endpoint(&node, UINT32_C(30),
                                       UCN_STATIC_ENDPOINT_FIRST,
                                       UCN_TRAFFIC_Q1_REALTIME,
                                       &first_payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&node, UINT32_C(30),
                                       UCN_STATIC_ENDPOINT_FIRST,
                                       UCN_TRAFFIC_Q1_REALTIME,
                                       &latest_payload, 1U) == UCN_OK);
    TEST_ASSERT(node.pending_q1[0].occupied && node.pending_q1[0].payload[0] == latest_payload);
    for (index = 1U; index < UCN_PENDING_Q1_DEPTH; ++index) {
        TEST_ASSERT(ucn_node_send_endpoint(&node, UINT32_C(30),
                                           (ucn_endpoint_t)(UCN_STATIC_ENDPOINT_FIRST + index),
                                           UCN_TRAFFIC_Q1_REALTIME,
                                           &first_payload, 1U) == UCN_OK);
    }
    TEST_ASSERT(ucn_node_send_endpoint(&node, UINT32_C(30),
                                       (ucn_endpoint_t)(UCN_STATIC_ENDPOINT_FIRST +
                                                        UCN_PENDING_Q1_DEPTH),
                                       UCN_TRAFFIC_Q1_REALTIME,
                                       &first_payload, 1U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_node_send_endpoint(&node, UINT32_C(30),
                                       UCN_STATIC_ENDPOINT_FIRST,
                                       UCN_TRAFFIC_Q0_CRITICAL,
                                       &first_payload, 1U) == UCN_ERR_NOT_FOUND);
    for (index = 0U; index < UCN_PENDING_Q1_DEPTH; ++index) {
        TEST_ASSERT(ucn_node_step(&node, UCN_PENDING_Q1_TIMEOUT_MS + 1U) == UCN_ERR_TTL);
    }
    TEST_ASSERT(node.stats.q1_route_wait_expired == UCN_PENDING_Q1_DEPTH);

    node.control_tokens = 0U;
    node.control_last_refill_ms = UCN_PENDING_Q1_TIMEOUT_MS + 1U;
    TEST_ASSERT(ucn_node_send_endpoint(&node, UINT32_C(40),
                                       UCN_STATIC_ENDPOINT_FIRST,
                                       UCN_TRAFFIC_Q1_REALTIME,
                                       &first_payload, 1U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.control_budget_dropped == 1U);
    return 0;
}

int test_protocol_version(void)
{
    int result = 0;

    result |= v3_test_frame_format();
    result |= v3_test_transparent_security();
    result |= v3_test_q1_wait_bounds();
    return result;
}
