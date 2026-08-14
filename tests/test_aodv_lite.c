#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct aodv_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool *is_up;
    bool *drop_frames;
    bool async_submit;
    ucn_wire_profile_t last_hello_profile;
    ucn_wire_profile_t last_route_request_profile;
    ucn_wire_profile_t last_route_reply_profile;
    ucn_wire_profile_t last_route_error_profile;
    uint16_t last_route_error_payload_length;
    ucn_wire_profile_t last_data_profile;
    uint8_t last_route_request_frame[UCN_MAX_FRAME_BYTES];
    size_t last_route_request_length;
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
    ucn_frame_t decoded;

    if (ucn_frame_decode(frame, length, &decoded) == UCN_OK) {
        if (decoded.message_type == UCN_MSG_HELLO) {
            context->last_hello_profile = decoded.wire_profile;
        } else if (decoded.message_type == UCN_MSG_ROUTE_REQ) {
            context->last_route_request_profile = decoded.wire_profile;
            if (length <= sizeof(context->last_route_request_frame)) {
                (void)memcpy(context->last_route_request_frame, frame, length);
                context->last_route_request_length = length;
            }
        } else if (decoded.message_type == UCN_MSG_ROUTE_REPLY) {
            context->last_route_reply_profile = decoded.wire_profile;
        } else if (decoded.message_type == UCN_MSG_ROUTE_ERROR) {
            context->last_route_error_profile = decoded.wire_profile;
            context->last_route_error_payload_length = decoded.payload_length;
        } else if (!ucn_message_type_is_control(decoded.message_type)) {
            context->last_data_profile = decoded.wire_profile;
        }
    }

    if (*context->drop_frames) {
        return UCN_OK;
    }
    {
        const ucn_result_t peer_result =
            ucn_node_receive(context->peer, context->peer_ingress, frame, length);

        return context->async_submit ? UCN_OK : peer_result;
    }
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
    ucn_result_t result;

    config.network_id = UINT32_C(42);
    config.node_id = id;
    config.default_hop_limit = 4U;
    result = ucn_node_init(node, &config);
    if (result != UCN_OK) {
        return 1;
    }
    return ucn_node_set_wire_profiles(node, UCN_WIRE_PROFILE_W0_LOCAL,
                                      UCN_WIRE_PROFILE_W0_LOCAL) == UCN_OK ?
               0 : 1;
}

static void aodv_rx(void *context, const ucn_frame_t *frame)
{
    aodv_receive_state_t *state = (aodv_receive_state_t *)context;

    state->count++;
    state->last_payload = frame->payload[0];
}

static int test_route_error_profile_payloads(void)
{
    static const ucn_wire_profile_t profiles[] = {
        UCN_WIRE_PROFILE_W0_LOCAL,
        UCN_WIRE_PROFILE_W1_EDGE,
        UCN_WIRE_PROFILE_W2_MESH,
        UCN_WIRE_PROFILE_W3_BACKBONE
    };
    static const uint16_t payload_lengths[] = { 1U, 2U, 3U, 4U };
    const uint8_t payload = 0xA5U;
    size_t index;

    for (index = 0U; index < sizeof(profiles) / sizeof(profiles[0]); ++index) {
        bool ab_up = true;
        bool bc_up = true;
        bool no_drop = false;
        ucn_node_t a, b, c;
        ucn_link_t ab, ba, bc, cb;
        aodv_link_context_t cab, cba, cbc, ccb;
        uint8_t malformed_payload[13U] = { 0U };
        uint8_t encoded[UCN_MAX_FRAME_BYTES];
        size_t encoded_length = 0U;
        ucn_frame_t malformed;

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
        TEST_ASSERT(aodv_init_node(&a, UINT32_C(1)) == 0);
        TEST_ASSERT(aodv_init_node(&b, UINT32_C(2)) == 0);
        TEST_ASSERT(aodv_init_node(&c, UINT32_C(3)) == 0);
        TEST_ASSERT(ucn_node_set_wire_profiles(
                        &a, profiles[index], UCN_WIRE_PROFILE_W3_BACKBONE) ==
                    UCN_OK);
        TEST_ASSERT(ucn_node_set_wire_profiles(
                        &b, profiles[index], UCN_WIRE_PROFILE_W3_BACKBONE) ==
                    UCN_OK);
        TEST_ASSERT(ucn_node_set_wire_profiles(
                        &c, profiles[index], UCN_WIRE_PROFILE_W3_BACKBONE) ==
                    UCN_OK);
        ab.ops = &AODV_LINK_OPS; ab.context = &cab;
        ab.link_id = (uint8_t)(40U + index * 4U);
        ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
        ba.ops = &AODV_LINK_OPS; ba.context = &cba;
        ba.link_id = (uint8_t)(41U + index * 4U);
        ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
        bc.ops = &AODV_LINK_OPS; bc.context = &cbc;
        bc.link_id = (uint8_t)(42U + index * 4U);
        bc.mtu = UCN_MAX_FRAME_BYTES; bc.peer_node_id = UINT32_C(3);
        cb.ops = &AODV_LINK_OPS; cb.context = &ccb;
        cb.link_id = (uint8_t)(43U + index * 4U);
        cb.mtu = UCN_MAX_FRAME_BYTES; cb.peer_node_id = UINT32_C(2);
        cab.peer = &b; cab.peer_ingress = &ba;
        cab.is_up = &ab_up; cab.drop_frames = &no_drop;
        cba.peer = &a; cba.peer_ingress = &ab;
        cba.is_up = &ab_up; cba.drop_frames = &no_drop;
        cbc.peer = &c; cbc.peer_ingress = &cb;
        cbc.is_up = &bc_up; cbc.drop_frames = &no_drop;
        ccb.peer = &b; ccb.peer_ingress = &bc;
        ccb.is_up = &bc_up; ccb.drop_frames = &no_drop;
        TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
        TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
        TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
        TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
        TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 1U) == UCN_OK);
        bc_up = false;
        TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                                  UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                    UCN_ERR_LINK_DOWN);
        TEST_ASSERT(cba.last_route_error_profile == profiles[index] &&
                    cba.last_route_error_payload_length ==
                        payload_lengths[index]);

        (void)memset(&malformed, 0, sizeof(malformed));
        malformed.message_type = UCN_MSG_ROUTE_ERROR;
        malformed.wire_profile = profiles[index];
        malformed.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
        malformed.hop_limit = 4U;
        malformed.network_id = UINT32_C(42);
        malformed.source = UINT32_C(2);
        malformed.destination = UINT32_C(1);
        malformed.sequence = (uint32_t)(100U + index);
        malformed.payload = malformed_payload;
        malformed.payload_length = (uint16_t)(payload_lengths[index] + 1U);
        TEST_ASSERT(ucn_frame_encode(&malformed, encoded, sizeof(encoded),
                                     &encoded_length) == UCN_OK);
        TEST_ASSERT(ucn_node_receive(&a, &ab, encoded, encoded_length) ==
                    UCN_ERR_MALFORMED);
    }
    return 0;
}

static int test_adaptive_wire_control_chain(void)
{
    const uint8_t payload = 0x5AU;
    bool ab_up = true, bc_up = true;
    bool no_drop = false;
    ucn_node_t a, b, c;
    ucn_link_t ab, ba, bc, cb;
    aodv_link_context_t cab, cba, cbc, ccb;
    aodv_receive_state_t received;

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
    TEST_ASSERT(ucn_node_set_wire_profiles(&a, UCN_WIRE_PROFILE_W3_BACKBONE,
                                             UCN_WIRE_PROFILE_W3_BACKBONE) ==
                UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profile_auto(&a, true) == UCN_OK);
    TEST_ASSERT(ucn_node_set_join_policy(&a, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_node_set_join_policy(&b, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_node_set_join_policy(&c, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);

    ab.ops = &AODV_LINK_OPS; ab.context = &cab; ab.link_id = 21U;
    ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
    ba.ops = &AODV_LINK_OPS; ba.context = &cba; ba.link_id = 22U;
    ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
    bc.ops = &AODV_LINK_OPS; bc.context = &cbc; bc.link_id = 23U;
    bc.mtu = UCN_MAX_FRAME_BYTES; bc.peer_node_id = UINT32_C(3);
    cb.ops = &AODV_LINK_OPS; cb.context = &ccb; cb.link_id = 24U;
    cb.mtu = UCN_MAX_FRAME_BYTES; cb.peer_node_id = UINT32_C(2);
    cab.peer = &b; cab.peer_ingress = &ba; cab.is_up = &ab_up; cab.drop_frames = &no_drop;
    cba.peer = &a; cba.peer_ingress = &ab; cba.is_up = &ab_up; cba.drop_frames = &no_drop;
    cbc.peer = &c; cbc.peer_ingress = &cb; cbc.is_up = &bc_up; cbc.drop_frames = &no_drop;
    ccb.peer = &b; ccb.peer_ingress = &bc; ccb.is_up = &bc_up; ccb.drop_frames = &no_drop;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    ucn_node_set_rx_handler(&c, aodv_rx, &received);

    TEST_ASSERT(ucn_node_broadcast_hello(&a, &ab, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &ba, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&b, &bc, 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_broadcast_hello(&c, &cb, 1U) == UCN_OK);
    TEST_ASSERT(cab.last_hello_profile == UCN_WIRE_PROFILE_W0_LOCAL &&
                cba.last_hello_profile == UCN_WIRE_PROFILE_W0_LOCAL &&
                cbc.last_hello_profile == UCN_WIRE_PROFILE_W0_LOCAL &&
                ccb.last_hello_profile == UCN_WIRE_PROFILE_W0_LOCAL);
    TEST_ASSERT(ucn_node_get_link_wire_profile_limit(&a, &ab) ==
                UCN_WIRE_PROFILE_W0_LOCAL);

    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 10U) == UCN_OK);
    TEST_ASSERT(!ucn_node_route_pending(&a, UINT32_C(3)));
    TEST_ASSERT(cab.last_route_request_profile == UCN_WIRE_PROFILE_W0_LOCAL &&
                cbc.last_route_request_profile == UCN_WIRE_PROFILE_W0_LOCAL &&
                ccb.last_route_reply_profile == UCN_WIRE_PROFILE_W0_LOCAL &&
                cba.last_route_reply_profile == UCN_WIRE_PROFILE_W0_LOCAL);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U && received.last_payload == payload);
    TEST_ASSERT(cab.last_data_profile == UCN_WIRE_PROFILE_W0_LOCAL &&
                cbc.last_data_profile == UCN_WIRE_PROFILE_W0_LOCAL);
    return 0;
}

static int test_expanding_ring_budget(void)
{
    ucn_config_t config = { UINT32_C(42), UINT32_C(10), UCN_MAX_HOPS };
    bool link_up = true;
    bool drop_frames = true;
    uint8_t expected_hop_limit;
    uint32_t now_ms = 1U;
    uint32_t expected_request_count = 1U;
    ucn_node_t node;
    ucn_link_t link;
    aodv_link_context_t context;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    (void)memset(&context, 0, sizeof(context));
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node, UCN_WIRE_PROFILE_W3_BACKBONE,
                    UCN_WIRE_PROFILE_W3_BACKBONE) == UCN_OK);
    link.ops = &AODV_LINK_OPS;
    link.context = &context;
    link.link_id = 31U;
    link.mtu = UCN_MAX_FRAME_BYTES;
    link.peer_node_id = UINT32_C(11);
    context.is_up = &link_up;
    context.drop_frames = &drop_frames;
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);

    TEST_ASSERT(ucn_node_discover_route(&node, UINT32_C(99), now_ms) == UCN_OK);
    expected_hop_limit = UCN_MAX_HOPS < 2U ? UCN_MAX_HOPS : 2U;
    TEST_ASSERT(node.discoveries[0].current_hop_limit == expected_hop_limit &&
                ucn_node_get_stats(&node)->route_requests_sent == 1U);

    while (expected_hop_limit < UCN_MAX_HOPS) {
        const uint16_t doubled = (uint16_t)expected_hop_limit * 2U;

        now_ms += UCN_ROUTE_RING_TIMEOUT_MS + 1U;
        TEST_ASSERT(ucn_node_step(&node, now_ms) == UCN_OK);
        expected_hop_limit = doubled >= UCN_MAX_HOPS ?
                                 UCN_MAX_HOPS : (uint8_t)doubled;
        expected_request_count++;
        TEST_ASSERT(node.discoveries[0].current_hop_limit == expected_hop_limit);
    }
    now_ms += UCN_ROUTE_RING_TIMEOUT_MS + 1U;
    TEST_ASSERT(ucn_node_step(&node, now_ms) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_get_stats(&node)->route_requests_sent ==
                expected_request_count);
    TEST_ASSERT(ucn_node_get_stats(&node)->route_request_ring_expansions ==
                expected_request_count - 1U);
    TEST_ASSERT(!ucn_node_route_pending(&node, UINT32_C(99)));
    return 0;
}

static const ucn_route_entry_t *aodv_find_route(const ucn_node_t *node,
                                                ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination) {
            return &node->routes[index];
        }
    }
    return NULL;
}

/* A 3-hop destination is first missed by the 2-hop ring, then reached by the
 * expanded 4-hop ring. Every relay has already learned the first request's
 * reverse route at the same Cost/egress. The second request must refresh that
 * route's epoch as well as its lifetime; otherwise the returned forward route
 * uses the new epoch while the first relay rejects business traffic against
 * the stale reverse epoch. */
static int test_expanding_ring_epoch_alignment(void)
{
    bool links_up = true;
    bool no_drop = false;
    const uint8_t payload = 0x6DU;
    ucn_node_t a, b, c, d;
    ucn_link_t ab, ba, bc, cb, cd, dc;
    aodv_link_context_t cab, cba, cbc, ccb, ccd, cdc;
    aodv_receive_state_t received;
    const ucn_route_entry_t *a_to_d;
    const ucn_route_entry_t *b_to_a;
    const ucn_route_entry_t *b_to_d;
    const ucn_route_entry_t *c_to_a;
    const ucn_route_entry_t *c_to_d;
    const ucn_route_entry_t *d_to_a;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c));
    (void)memset(&d, 0, sizeof(d));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&bc, 0, sizeof(bc));
    (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&cd, 0, sizeof(cd));
    (void)memset(&dc, 0, sizeof(dc));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&cbc, 0, sizeof(cbc));
    (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&ccd, 0, sizeof(ccd));
    (void)memset(&cdc, 0, sizeof(cdc));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(aodv_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(aodv_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(aodv_init_node(&c, UINT32_C(3)) == 0);
    TEST_ASSERT(aodv_init_node(&d, UINT32_C(4)) == 0);

    ab.ops = &AODV_LINK_OPS; ab.context = &cab; ab.link_id = 101U;
    ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
    ba.ops = &AODV_LINK_OPS; ba.context = &cba; ba.link_id = 102U;
    ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
    bc.ops = &AODV_LINK_OPS; bc.context = &cbc; bc.link_id = 103U;
    bc.mtu = UCN_MAX_FRAME_BYTES; bc.peer_node_id = UINT32_C(3);
    cb.ops = &AODV_LINK_OPS; cb.context = &ccb; cb.link_id = 104U;
    cb.mtu = UCN_MAX_FRAME_BYTES; cb.peer_node_id = UINT32_C(2);
    cd.ops = &AODV_LINK_OPS; cd.context = &ccd; cd.link_id = 105U;
    cd.mtu = UCN_MAX_FRAME_BYTES; cd.peer_node_id = UINT32_C(4);
    dc.ops = &AODV_LINK_OPS; dc.context = &cdc; dc.link_id = 106U;
    dc.mtu = UCN_MAX_FRAME_BYTES; dc.peer_node_id = UINT32_C(3);
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    cbc.peer = &c; cbc.peer_ingress = &cb;
    ccb.peer = &b; ccb.peer_ingress = &bc;
    ccd.peer = &d; ccd.peer_ingress = &dc;
    cdc.peer = &c; cdc.peer_ingress = &cd;
    cab.is_up = cba.is_up = cbc.is_up = ccb.is_up = &links_up;
    ccd.is_up = cdc.is_up = &links_up;
    cab.drop_frames = cba.drop_frames = cbc.drop_frames = ccb.drop_frames =
        &no_drop;
    ccd.drop_frames = cdc.drop_frames = &no_drop;
    cab.async_submit = cba.async_submit = cbc.async_submit = true;
    ccb.async_submit = ccd.async_submit = cdc.async_submit = true;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cd) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&d, &dc) == UCN_OK);
    ucn_node_set_rx_handler(&d, aodv_rx, &received);

    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(4), 10U) == UCN_OK);
    TEST_ASSERT(ucn_node_route_pending(&a, UINT32_C(4)));
    TEST_ASSERT(ucn_node_step(&a, 10U + UCN_ROUTE_RING_TIMEOUT_MS + 1U) ==
                UCN_OK);
    TEST_ASSERT(!ucn_node_route_pending(&a, UINT32_C(4)));

    a_to_d = aodv_find_route(&a, UINT32_C(4));
    b_to_a = aodv_find_route(&b, UINT32_C(1));
    b_to_d = aodv_find_route(&b, UINT32_C(4));
    c_to_a = aodv_find_route(&c, UINT32_C(1));
    c_to_d = aodv_find_route(&c, UINT32_C(4));
    d_to_a = aodv_find_route(&d, UINT32_C(1));
    TEST_ASSERT(a_to_d != NULL && b_to_a != NULL && b_to_d != NULL &&
                c_to_a != NULL && c_to_d != NULL && d_to_a != NULL);
    TEST_ASSERT(a_to_d->route_epoch != 0U &&
                b_to_a->route_epoch == a_to_d->route_epoch &&
                b_to_d->route_epoch == a_to_d->route_epoch &&
                c_to_a->route_epoch == a_to_d->route_epoch &&
                c_to_d->route_epoch == a_to_d->route_epoch &&
                d_to_a->route_epoch == a_to_d->route_epoch);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(4), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U && received.last_payload == payload);
    return 0;
}

static int test_origin_rreq_loop_guard(void)
{
    bool link_up = true;
    bool drop_frames = true;
    ucn_node_t node;
    ucn_link_t link;
    aodv_link_context_t context;
    size_t index;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    (void)memset(&context, 0, sizeof(context));
    TEST_ASSERT(aodv_init_node(&node, UINT32_C(1)) == 0);
    link.ops = &AODV_LINK_OPS;
    link.context = &context;
    link.link_id = 90U;
    link.mtu = UCN_MAX_FRAME_BYTES;
    link.peer_node_id = UINT32_C(2);
    context.is_up = &link_up;
    context.drop_frames = &drop_frames;
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);

    TEST_ASSERT(ucn_node_discover_route(&node, UINT32_C(99), 10U) == UCN_OK);
    TEST_ASSERT(context.last_route_request_length != 0U);
    /* Model the same flood returning over a second bearer. The origin must
     * reject it from its RREQ cache and must never learn a route to itself. */
    TEST_ASSERT(ucn_node_receive(&node, &link,
                                 context.last_route_request_frame,
                                 context.last_route_request_length) ==
                UCN_ERR_REPLAY);
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        TEST_ASSERT(!node.routes[index].valid ||
                    node.routes[index].destination != node.config.node_id);
    }
    return 0;
}

int test_aodv_lite(void)
{
    uint8_t first_payload = 0x31U;
    uint8_t repaired_payload = 0x32U;
    uint8_t duplicate_payload[10] = { 0U };
    uint8_t old_w0_request_payload[8] = { 0U };
    uint8_t old_w1_request_payload[10] = { 0U };
    uint8_t legacy_reply_payload[18] = { 0U };
    uint8_t legacy_request_payload[16] = { 0U };
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
    TEST_ASSERT(ucn_node_set_wire_profiles(&b, UCN_WIRE_PROFILE_W0_LOCAL,
                                            UCN_WIRE_PROFILE_W3_BACKBONE) ==
                UCN_OK);
    TEST_ASSERT(test_route_error_profile_payloads() == 0);
    TEST_ASSERT(test_adaptive_wire_control_chain() == 0);
    TEST_ASSERT(test_expanding_ring_budget() == 0);
    TEST_ASSERT(test_expanding_ring_epoch_alignment() == 0);
    TEST_ASSERT(test_origin_rreq_loop_guard() == 0);

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

    /* V5-23 widened W0/W1 route Cost to three bytes.  Frames emitted by the
     * immediately preceding V5 layout must fail closed instead of being
     * decoded with shifted Hop/Flags fields. */
    old_w0_request_payload[0] = 3U;
    old_w0_request_payload[4] = 1U;
    old_w0_request_payload[6] = 1U;
    (void)memset(&duplicate_request, 0, sizeof(duplicate_request));
    duplicate_request.message_type = UCN_MSG_ROUTE_REQ;
    duplicate_request.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    duplicate_request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    duplicate_request.hop_limit = 4U;
    duplicate_request.network_id = UINT32_C(42);
    duplicate_request.source = UINT32_C(1);
    duplicate_request.destination = UCN_NODE_BROADCAST;
    duplicate_request.session_id = UINT32_C(1);
    duplicate_request.sequence = UINT32_C(0xA7);
    duplicate_request.payload = old_w0_request_payload;
    duplicate_request.payload_length =
        (uint16_t)sizeof(old_w0_request_payload);
    TEST_ASSERT(ucn_frame_encode(&duplicate_request, duplicate_encoded,
                                 sizeof(duplicate_encoded), &duplicate_length) ==
                UCN_OK);
    TEST_ASSERT(ucn_node_receive(&b, &ba, duplicate_encoded, duplicate_length) ==
                UCN_ERR_MALFORMED);

    old_w1_request_payload[1] = 3U;
    old_w1_request_payload[5] = 2U;
    old_w1_request_payload[8] = 1U;
    duplicate_request.wire_profile = UCN_WIRE_PROFILE_W1_EDGE;
    duplicate_request.sequence = UINT32_C(0xA8);
    duplicate_request.payload = old_w1_request_payload;
    duplicate_request.payload_length =
        (uint16_t)sizeof(old_w1_request_payload);
    TEST_ASSERT(ucn_frame_encode(&duplicate_request, duplicate_encoded,
                                 sizeof(duplicate_encoded), &duplicate_length) ==
                UCN_OK);
    TEST_ASSERT(ucn_node_receive(&b, &ba, duplicate_encoded, duplicate_length) ==
                UCN_ERR_MALFORMED);
    /* The pre-V5-14 18-byte RREP (including duplicate Origin/Target) is not
     * accepted as the new profile-aware compressed format. */
    legacy_reply_payload[3] = 1U;
    legacy_reply_payload[7] = 3U;
    legacy_reply_payload[11] = 1U;
    legacy_reply_payload[17] = 1U;
    (void)memset(&duplicate_request, 0, sizeof(duplicate_request));
    duplicate_request.message_type = UCN_MSG_ROUTE_REPLY;
    duplicate_request.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    duplicate_request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    duplicate_request.hop_limit = 4U;
    duplicate_request.network_id = UINT32_C(42);
    duplicate_request.source = UINT32_C(3);
    duplicate_request.destination = UINT32_C(1);
    duplicate_request.sequence = UINT32_C(0);
    duplicate_request.payload = legacy_reply_payload;
    duplicate_request.payload_length =
        (uint16_t)sizeof(legacy_reply_payload);
    TEST_ASSERT(ucn_frame_encode(&duplicate_request, duplicate_encoded,
                                 sizeof(duplicate_encoded), &duplicate_length) ==
                UCN_OK);
    TEST_ASSERT(ucn_node_receive(&a, &ab, duplicate_encoded, duplicate_length) ==
                UCN_ERR_MALFORMED);

    (void)memset(&duplicate_request, 0, sizeof(duplicate_request));
    legacy_request_payload[3] = 1U;
    legacy_request_payload[7] = 99U;
    legacy_request_payload[11] = 1U;
    duplicate_request.message_type = UCN_MSG_ROUTE_REQ;
    duplicate_request.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    duplicate_request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    duplicate_request.hop_limit = 4U;
    duplicate_request.network_id = UINT32_C(42);
    duplicate_request.source = UINT32_C(1);
    duplicate_request.destination = UCN_NODE_BROADCAST;
    duplicate_request.sequence = UINT32_C(0xA9);
    duplicate_request.payload = legacy_request_payload;
    duplicate_request.payload_length = (uint16_t)sizeof(legacy_request_payload);
    TEST_ASSERT(ucn_frame_encode(&duplicate_request, duplicate_encoded,
                                 sizeof(duplicate_encoded),
                                 &duplicate_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&b, &ba, duplicate_encoded, duplicate_length) ==
                UCN_ERR_MALFORMED);

    (void)memset(&duplicate_request, 0, sizeof(duplicate_request));
    duplicate_payload[0] = 99U;
    duplicate_payload[1] = 0U; duplicate_payload[2] = 0U;
    duplicate_payload[3] = 0U; duplicate_payload[4] = 1U;
    duplicate_payload[5] = 0U; duplicate_payload[6] = 0U;
    duplicate_payload[7] = 0U; duplicate_payload[8] = 1U;
    duplicate_payload[9] = 0U;
    duplicate_request.message_type = UCN_MSG_ROUTE_REQ;
    duplicate_request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    duplicate_request.hop_limit = 4U;
    duplicate_request.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    duplicate_request.network_id = UINT32_C(42);
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
    TEST_ASSERT(ucn_node_step(&a, 261U) == UCN_OK);
    TEST_ASSERT(a.discoveries[0].current_hop_limit == 4U &&
                ucn_node_get_stats(&a)->route_requests_sent == 2U &&
                ucn_node_get_stats(&a)->route_request_ring_expansions == 1U);
    TEST_ASSERT(ucn_node_step(&a, 512U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_step(&a, 1011U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(!ucn_node_route_pending(&a, UINT32_C(3)));

    ab_drop = false;
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 1100U) == UCN_OK);
    TEST_ASSERT(!ucn_node_route_pending(&a, UINT32_C(3)));
    TEST_ASSERT(ucn_node_get_stats(&a)->route_requests_sent == 3U);
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
