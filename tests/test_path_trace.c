#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

#define TEST_TRACE_RECORD_COUNT_OFFSET ((size_t)4U)
#define TEST_TRACE_RECORD_LIMIT_OFFSET ((size_t)5U)
#define TEST_TRACE_STATUS_OFFSET ((size_t)6U)
#define TEST_TRACE_NODE_IDS_OFFSET UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES

typedef struct trace_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool deliver;
    uint32_t send_count;
    uint8_t last_message_type;
    uint8_t last_flags;
    ucn_traffic_class_t last_traffic_class;
} trace_link_context_t;

typedef struct trace_callback_state {
    uint32_t count;
    ucn_path_trace_result_t result;
} trace_callback_state_t;

static ucn_result_t trace_link_send(ucn_link_t *link,
                                    const uint8_t *frame,
                                    size_t length)
{
    trace_link_context_t *context = (trace_link_context_t *)link->context;
    ucn_frame_t decoded;

    context->send_count++;
    if (ucn_frame_decode(frame, length, &decoded) == UCN_OK) {
        context->last_message_type = decoded.message_type;
        context->last_flags = decoded.flags;
        context->last_traffic_class = decoded.traffic_class;
    }
    if (!context->deliver) {
        return UCN_OK;
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t trace_link_status(const ucn_link_t *link,
                                      ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t TRACE_LINK_OPS = {
    NULL, trace_link_send, NULL, trace_link_status, NULL, NULL
};

static int trace_init_node(ucn_node_t *node, ucn_node_id_t node_id,
                           uint8_t default_hop_limit)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x54524345);
    config.node_id = node_id;
    config.default_hop_limit = default_hop_limit;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void trace_setup_link(ucn_link_t *link,
                             trace_link_context_t *context,
                             uint8_t link_id,
                             ucn_node_id_t peer_node_id)
{
    link->ops = &TRACE_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
    context->deliver = true;
}

static void trace_callback(void *context, const ucn_path_trace_result_t *result)
{
    trace_callback_state_t *state = (trace_callback_state_t *)context;

    state->count++;
    state->result = *result;
}

static int trace_test_complete_truncated_and_rejected(void)
{
    ucn_node_t a, b, c;
    ucn_link_t ab, ba, bc, cb, unregistered;
    trace_link_context_t cab, cba, cbc, ccb;
    trace_callback_state_t complete, truncated;
    trace_callback_state_t no_route;
    uint8_t malicious_payload[UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES +
                              sizeof(ucn_node_id_t)];
    uint8_t tag[UCN_E2E_TAG_SIZE] = { 0U };
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_frame_t malicious;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c)); (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba)); (void)memset(&bc, 0, sizeof(bc));
    (void)memset(&cb, 0, sizeof(cb)); (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba)); (void)memset(&cbc, 0, sizeof(cbc));
    (void)memset(&ccb, 0, sizeof(ccb)); (void)memset(&complete, 0, sizeof(complete));
    (void)memset(&truncated, 0, sizeof(truncated));
    (void)memset(&no_route, 0, sizeof(no_route));
    (void)memset(&unregistered, 0, sizeof(unregistered));
    TEST_ASSERT(trace_init_node(&a, UINT32_C(1), 5U) == 0);
    TEST_ASSERT(trace_init_node(&b, UINT32_C(2), 5U) == 0);
    TEST_ASSERT(trace_init_node(&c, UINT32_C(3), 5U) == 0);
    trace_setup_link(&ab, &cab, 1U, UINT32_C(2));
    trace_setup_link(&ba, &cba, 2U, UINT32_C(1));
    trace_setup_link(&bc, &cbc, 3U, UINT32_C(3));
    trace_setup_link(&cb, &ccb, 4U, UINT32_C(2));
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    cbc.peer = &c; cbc.peer_ingress = &cb;
    ccb.peer = &b; ccb.peer_ingress = &bc;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(3), &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(99), &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&b, UINT32_C(3), &bc) == UCN_OK);

    TEST_ASSERT(ucn_node_request_path_trace(&a, UINT32_C(3), 0U,
                                             trace_callback, &complete) == UCN_OK);
    TEST_ASSERT(complete.count == 1U &&
                complete.result.status == UCN_PATH_TRACE_STATUS_OK &&
                complete.result.node_count == 3U &&
                complete.result.node_ids[0] == UINT32_C(1) &&
                complete.result.node_ids[1] == UINT32_C(2) &&
                complete.result.node_ids[2] == UINT32_C(3));
    TEST_ASSERT(cab.last_message_type == UCN_MSG_PATH_TRACE_REQ &&
                cab.last_flags == UCN_FRAME_FLAG_DIAGNOSTIC &&
                cab.last_traffic_class == UCN_TRAFFIC_Q1_REALTIME);
    TEST_ASSERT(cba.last_message_type == UCN_MSG_PATH_TRACE_REPLY &&
                cba.last_flags == UCN_FRAME_FLAG_DIAGNOSTIC &&
                cba.last_traffic_class == UCN_TRAFFIC_Q1_REALTIME);
    TEST_ASSERT(a.stats.path_trace_requests_sent == 1U &&
                a.stats.path_trace_completed == 1U &&
                b.stats.path_trace_replies_sent == 0U &&
                c.stats.path_trace_replies_sent == 1U);
    TEST_ASSERT(ucn_node_request_path_trace(&a, UINT32_C(3), 0U,
                                             trace_callback, &complete) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(a.stats.path_trace_rate_dropped == 1U && complete.count == 1U);

    TEST_ASSERT(ucn_node_step(&a, UCN_PATH_TRACE_TOKEN_REFILL_MS) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_request_path_trace(&a, UINT32_C(3), 1U,
                                             trace_callback, &truncated) == UCN_OK);
    TEST_ASSERT(truncated.count == 1U &&
                truncated.result.status == UCN_PATH_TRACE_STATUS_TRUNCATED &&
                truncated.result.node_count == 1U &&
                truncated.result.node_ids[0] == UINT32_C(1));
    TEST_ASSERT(ucn_node_step(&a, UCN_PATH_TRACE_TOKEN_REFILL_MS * 2U) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_request_path_trace(&a, UINT32_C(99), 0U,
                                             trace_callback, &no_route) == UCN_OK);
    TEST_ASSERT(no_route.count == 1U &&
                no_route.result.status == UCN_PATH_TRACE_STATUS_NO_ROUTE &&
                no_route.result.node_count == 2U &&
                no_route.result.node_ids[0] == UINT32_C(1) &&
                no_route.result.node_ids[1] == UINT32_C(2));
    TEST_ASSERT(ucn_node_request_path_trace(&a, UINT32_C(100), 0U,
                                             trace_callback, &truncated) == UCN_ERR_NOT_FOUND);

    (void)memset(malicious_payload, 0, sizeof(malicious_payload));
    malicious_payload[3] = 55U;
    malicious_payload[TEST_TRACE_RECORD_COUNT_OFFSET] = 1U;
    malicious_payload[TEST_TRACE_RECORD_LIMIT_OFFSET] = 1U;
    malicious_payload[TEST_TRACE_STATUS_OFFSET] =
        (uint8_t)UCN_PATH_TRACE_STATUS_OK;
    malicious_payload[TEST_TRACE_NODE_IDS_OFFSET + 3U] = 1U;
    (void)memset(&malicious, 0, sizeof(malicious));
    malicious.message_type = UCN_MSG_PATH_TRACE_REQ;
    malicious.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    malicious.flags = UCN_FRAME_FLAG_DIAGNOSTIC | UCN_FRAME_FLAG_E2E_PROTECTED;
    malicious.hop_limit = 2U;
    malicious.network_id = UINT32_C(0x54524345);
    malicious.source = UINT32_C(1);
    malicious.destination = UINT32_C(3);
    malicious.sequence = UINT32_C(99);
    malicious.payload = malicious_payload;
    malicious.payload_length = (uint16_t)sizeof(malicious_payload);
    malicious.auth_tag = tag;
    TEST_ASSERT(ucn_frame_encode(&malicious, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&b, &ba, encoded, encoded_length) == UCN_ERR_MALFORMED);
    malicious.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    malicious.auth_tag = NULL;
    malicious.sequence = UINT32_C(100);
    TEST_ASSERT(ucn_frame_encode(&malicious, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&b, &unregistered, encoded, encoded_length) ==
                UCN_ERR_ACCESS);
    return 0;
}

static int trace_test_ttl_and_timeout(void)
{
    ucn_node_t a, b, c, x, y;
    ucn_link_t ab, ba, bc, cb, xy, yx;
    trace_link_context_t cab, cba, cbc, ccb, cxy, cyx;
    trace_callback_state_t ttl, timeout;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c)); (void)memset(&x, 0, sizeof(x));
    (void)memset(&y, 0, sizeof(y)); (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba)); (void)memset(&bc, 0, sizeof(bc));
    (void)memset(&cb, 0, sizeof(cb)); (void)memset(&xy, 0, sizeof(xy));
    (void)memset(&yx, 0, sizeof(yx)); (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba)); (void)memset(&cbc, 0, sizeof(cbc));
    (void)memset(&ccb, 0, sizeof(ccb)); (void)memset(&cxy, 0, sizeof(cxy));
    (void)memset(&cyx, 0, sizeof(cyx)); (void)memset(&ttl, 0, sizeof(ttl));
    (void)memset(&timeout, 0, sizeof(timeout));
    TEST_ASSERT(trace_init_node(&a, UINT32_C(11), 1U) == 0);
    TEST_ASSERT(trace_init_node(&b, UINT32_C(12), 5U) == 0);
    TEST_ASSERT(trace_init_node(&c, UINT32_C(13), 5U) == 0);
    trace_setup_link(&ab, &cab, 11U, UINT32_C(12));
    trace_setup_link(&ba, &cba, 12U, UINT32_C(11));
    trace_setup_link(&bc, &cbc, 13U, UINT32_C(13));
    trace_setup_link(&cb, &ccb, 14U, UINT32_C(12));
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    cbc.peer = &c; cbc.peer_ingress = &cb;
    ccb.peer = &b; ccb.peer_ingress = &bc;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(13), &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&b, UINT32_C(13), &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_request_path_trace(&a, UINT32_C(13), 0U,
                                             trace_callback, &ttl) == UCN_OK);
    TEST_ASSERT(ttl.count == 1U &&
                ttl.result.status == UCN_PATH_TRACE_STATUS_TTL_EXCEEDED &&
                ttl.result.node_count == 2U &&
                ttl.result.node_ids[0] == UINT32_C(11) &&
                ttl.result.node_ids[1] == UINT32_C(12));

    TEST_ASSERT(trace_init_node(&x, UINT32_C(21), 5U) == 0);
    TEST_ASSERT(trace_init_node(&y, UINT32_C(22), 5U) == 0);
    trace_setup_link(&xy, &cxy, 21U, UINT32_C(22));
    trace_setup_link(&yx, &cyx, 22U, UINT32_C(21));
    cxy.peer = &y; cxy.peer_ingress = &yx;
    cyx.peer = &x; cyx.peer_ingress = &xy;
    cxy.deliver = false;
    TEST_ASSERT(ucn_node_register_link(&x, &xy) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&y, &yx) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&x, UINT32_C(23), &xy) == UCN_OK);
    TEST_ASSERT(ucn_node_request_path_trace(&x, UINT32_C(23), 0U,
                                             trace_callback, &timeout) == UCN_OK);
    TEST_ASSERT(timeout.count == 0U);
    x.path_trace_tokens = UCN_PATH_TRACE_TOKEN_BURST;
    TEST_ASSERT(ucn_node_request_path_trace(&x, UINT32_C(23), 0U,
                                             trace_callback, &timeout) == UCN_OK);
    x.path_trace_tokens = UCN_PATH_TRACE_TOKEN_BURST;
    TEST_ASSERT(ucn_node_request_path_trace(&x, UINT32_C(23), 0U,
                                             trace_callback, &timeout) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_node_step(&x, UCN_PATH_TRACE_TIMEOUT_MS) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(timeout.count == UCN_PATH_TRACE_PENDING_DEPTH &&
                timeout.result.status == UCN_PATH_TRACE_STATUS_TIMEOUT &&
                timeout.result.node_count == 0U &&
                x.stats.path_trace_timeouts == UCN_PATH_TRACE_PENDING_DEPTH);
    return 0;
}

int test_path_trace(void)
{
    int result = 0;

    result |= trace_test_complete_truncated_and_rejected();
    result |= trace_test_ttl_and_timeout();
    return result;
}
