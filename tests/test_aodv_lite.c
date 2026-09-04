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
    ucn_result_t forced_result;
    bool route_cost_valid;
    uint16_t route_cost;
    uint32_t send_count;
    ucn_wire_profile_t last_hello_profile;
    ucn_wire_profile_t last_route_request_profile;
    ucn_wire_profile_t last_route_reply_profile;
    ucn_wire_profile_t last_route_error_profile;
    uint16_t last_route_error_payload_length;
    ucn_wire_profile_t last_data_profile;
    uint8_t last_route_request_frame[UCN_MAX_FRAME_BYTES];
    size_t last_route_request_length;
    uint8_t last_route_reply_frame[UCN_MAX_FRAME_BYTES];
    size_t last_route_reply_length;
    uint8_t last_data_frame[UCN_MAX_FRAME_BYTES];
    size_t last_data_length;
    uint32_t path_activate_count;
    uint32_t path_activate_candidate_ids[8U];
    uint16_t path_activate_epochs[8U];
    ucn_sequence_t path_activate_sequences[8U];
    const ucn_candidate_route_t *expected_frozen_candidate;
    bool observed_frozen_before_send;
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

    context->send_count++;
    if (context->expected_frozen_candidate != NULL &&
        context->expected_frozen_candidate->path_snapshot_frozen) {
        context->observed_frozen_before_send = true;
    }

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
            if (length <= sizeof(context->last_route_reply_frame)) {
                (void)memcpy(context->last_route_reply_frame, frame, length);
                context->last_route_reply_length = length;
            }
        } else if (decoded.message_type == UCN_MSG_ROUTE_ERROR) {
            context->last_route_error_profile = decoded.wire_profile;
            context->last_route_error_payload_length = decoded.payload_length;
        } else if (decoded.message_type == UCN_MSG_PATH_ACTIVATE &&
                   decoded.payload_length == 6U &&
                   context->path_activate_count < 8U) {
            const uint32_t activate_index = context->path_activate_count++;

            context->path_activate_candidate_ids[activate_index] =
                ((uint32_t)decoded.payload[0] << 24U) |
                ((uint32_t)decoded.payload[1] << 16U) |
                ((uint32_t)decoded.payload[2] << 8U) |
                (uint32_t)decoded.payload[3];
            context->path_activate_epochs[activate_index] =
                (uint16_t)(((uint16_t)decoded.payload[4] << 8U) |
                           (uint16_t)decoded.payload[5]);
            context->path_activate_sequences[activate_index] =
                decoded.sequence;
        } else if (!ucn_message_type_is_control(decoded.message_type)) {
            context->last_data_profile = decoded.wire_profile;
            if (length <= sizeof(context->last_data_frame)) {
                (void)memcpy(context->last_data_frame, frame, length);
                context->last_data_length = length;
            }
        }
    }

    if (context->forced_result != UCN_OK) {
        return context->forced_result;
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

/*
 * EN: Returns optional deterministic Link Cost for path-selection fixtures.
 * 中文：为路径选择测试返回可选的确定性 Link Cost。
 */
static ucn_result_t aodv_link_metrics(const ucn_link_t *link,
                                      ucn_link_metrics_t *metrics)
{
    const aodv_link_context_t *context =
        (const aodv_link_context_t *)link->context;

    (void)memset(metrics, 0, sizeof(*metrics));
    metrics->route_cost_valid = context->route_cost_valid;
    metrics->route_cost = context->route_cost;
    return UCN_OK;
}

static const ucn_link_ops_t AODV_LINK_OPS = {
    NULL, aodv_link_send, NULL, aodv_link_status, NULL, aodv_link_metrics
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

static const ucn_route_entry_t *aodv_find_route(
    const ucn_node_t *node,
    ucn_node_id_t route_origin,
    ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination &&
            (node->routes[index].is_static ||
             node->routes[index].route_origin == route_origin)) {
            return &node->routes[index];
        }
    }
    return NULL;
}

/*
 * EN: Proves an administrator-owned static Route remains a wildcard when an
 *     upstream dynamic route carries its Source-owned Route Epoch.
 * 中文：验证管理员静态路由在上游动态路由携带源节点 Route Epoch 时仍作为通配路由。
 */
static int test_static_route_accepts_dynamic_epoch(void)
{
    bool links_up = true;
    bool no_drop = false;
    const uint8_t payload = 0x5AU;
    ucn_node_t origin, relay, target;
    ucn_link_t origin_to_relay, relay_from_origin;
    ucn_link_t relay_to_target, target_from_relay;
    aodv_link_context_t contexts[4];
    aodv_receive_state_t received;
    ucn_frame_t forwarded;

    (void)memset(&origin, 0, sizeof(origin));
    (void)memset(&relay, 0, sizeof(relay));
    (void)memset(&target, 0, sizeof(target));
    (void)memset(&origin_to_relay, 0, sizeof(origin_to_relay));
    (void)memset(&relay_from_origin, 0, sizeof(relay_from_origin));
    (void)memset(&relay_to_target, 0, sizeof(relay_to_target));
    (void)memset(&target_from_relay, 0, sizeof(target_from_relay));
    (void)memset(contexts, 0, sizeof(contexts));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(aodv_init_node(&origin, UINT32_C(1)) == 0);
    TEST_ASSERT(aodv_init_node(&relay, UINT32_C(2)) == 0);
    TEST_ASSERT(aodv_init_node(&target, UINT32_C(3)) == 0);

#define AODV_STATIC_LINK(link_, context_, id_, peer_) \
    do { \
        (link_).ops = &AODV_LINK_OPS; \
        (link_).context = &(context_); \
        (link_).link_id = (id_); \
        (link_).mtu = UCN_MAX_FRAME_BYTES; \
        (link_).peer_node_id = (peer_); \
        (context_).is_up = &links_up; \
        (context_).drop_frames = &no_drop; \
    } while (0)
    AODV_STATIC_LINK(origin_to_relay, contexts[0], 131U, UINT32_C(2));
    AODV_STATIC_LINK(relay_from_origin, contexts[1], 132U, UINT32_C(1));
    AODV_STATIC_LINK(relay_to_target, contexts[2], 133U, UINT32_C(3));
    AODV_STATIC_LINK(target_from_relay, contexts[3], 134U, UINT32_C(2));
#undef AODV_STATIC_LINK
    contexts[0].peer = &relay;
    contexts[0].peer_ingress = &relay_from_origin;
    contexts[1].peer = &origin;
    contexts[1].peer_ingress = &origin_to_relay;
    contexts[2].peer = &target;
    contexts[2].peer_ingress = &target_from_relay;
    contexts[3].peer = &relay;
    contexts[3].peer_ingress = &relay_to_target;
    TEST_ASSERT(ucn_node_register_link(&origin, &origin_to_relay) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&relay, &relay_from_origin) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&relay, &relay_to_target) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&target, &target_from_relay) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&relay, UINT32_C(3),
                                   &relay_to_target) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&target, UINT32_C(1),
                                   &target_from_relay) == UCN_OK);
    origin.routes[0].valid = true;
    origin.routes[0].route_origin = UINT32_C(1);
    origin.routes[0].destination = UINT32_C(3);
    origin.routes[0].egress_link = &origin_to_relay;
    origin.routes[0].expires_at_ms = 1000U;
    origin.routes[0].route_cost = 2U;
    origin.routes[0].hop_count = 2U;
    origin.routes[0].route_epoch = 7U;
    ucn_node_set_rx_handler(&target, aodv_rx, &received);

    TEST_ASSERT(ucn_node_send(&origin, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_OK);
    TEST_ASSERT(received.count == 1U && received.last_payload == payload);
    TEST_ASSERT(contexts[2].last_data_length != 0U);
    TEST_ASSERT(ucn_frame_decode(contexts[2].last_data_frame,
                                 contexts[2].last_data_length,
                                 &forwarded) == UCN_OK);
    TEST_ASSERT(forwarded.has_route_extension && forwarded.route_epoch == 7U);
    TEST_ASSERT(relay.stats.route_errors_sent == 0U &&
                relay.stats.route_epoch_rejected == 0U &&
                target.stats.route_epoch_rejected == 0U);
    return 0;
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

    a_to_d = aodv_find_route(&a, UINT32_C(1), UINT32_C(4));
    b_to_a = aodv_find_route(&b, UINT32_C(4), UINT32_C(1));
    b_to_d = aodv_find_route(&b, UINT32_C(1), UINT32_C(4));
    c_to_a = aodv_find_route(&c, UINT32_C(4), UINT32_C(1));
    c_to_d = aodv_find_route(&c, UINT32_C(1), UINT32_C(4));
    d_to_a = aodv_find_route(&d, UINT32_C(4), UINT32_C(1));
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

/* Two independent Sources deliberately use different Request-ID/Epoch
 * ranges while sharing the R-X-T relay corridor.  A destination-only Route
 * table overwrites A's forward epoch when B discovers T; the directional
 * (Origin, Destination) instance model must keep both flows live. */
static int test_multi_origin_route_epoch_isolation(void)
{
    bool links_up = true;
    bool tail_up = true;
    bool no_drop = false;
    bool drop_ra = false;
    bool drop_rb = false;
    const uint8_t payload_a = 0xA1U;
    const uint8_t payload_b = 0xB2U;
    ucn_node_t a, b, r, x, t;
    ucn_link_t ar, ra, br, rb, rx, xr, xt, tx;
    aodv_link_context_t contexts[8];
    aodv_receive_state_t received;
    const ucn_route_entry_t *r_a_t;
    const ucn_route_entry_t *r_b_t;
    ucn_route_summary_t summaries[UCN_MAX_ROUTES];
    ucn_frame_t crossed_epoch_frame;
    uint8_t crossed_epoch_encoded[UCN_MAX_FRAME_BYTES];
    size_t crossed_epoch_length = 0U;
    uint32_t epoch_rejected_before;
    size_t summary_count;
    size_t index;
    bool saw_a_t = false;
    bool saw_b_t = false;
#if UCN_FEATURE_CANDIDATE_ROUTING
    bool saw_candidate_a_t = false;
    bool saw_candidate_b_t = false;
#endif

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&r, 0, sizeof(r));
    (void)memset(&x, 0, sizeof(x));
    (void)memset(&t, 0, sizeof(t));
    (void)memset(&ar, 0, sizeof(ar));
    (void)memset(&ra, 0, sizeof(ra));
    (void)memset(&br, 0, sizeof(br));
    (void)memset(&rb, 0, sizeof(rb));
    (void)memset(&rx, 0, sizeof(rx));
    (void)memset(&xr, 0, sizeof(xr));
    (void)memset(&xt, 0, sizeof(xt));
    (void)memset(&tx, 0, sizeof(tx));
    (void)memset(contexts, 0, sizeof(contexts));
    (void)memset(&received, 0, sizeof(received));
    (void)memset(summaries, 0, sizeof(summaries));
    TEST_ASSERT(aodv_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(aodv_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(aodv_init_node(&r, UINT32_C(3)) == 0);
    TEST_ASSERT(aodv_init_node(&x, UINT32_C(4)) == 0);
    TEST_ASSERT(aodv_init_node(&t, UINT32_C(5)) == 0);
    /* A expands 2->4 hops while B expands 2->3 hops.  Both discoveries are
     * active together and their replies are deliberately delivered B then A. */
    b.config.default_hop_limit = 3U;

#define AODV_INIT_LINK(link_, context_, id_, peer_) \
    do { \
        (link_).ops = &AODV_LINK_OPS; \
        (link_).context = &(context_); \
        (link_).link_id = (id_); \
        (link_).mtu = UCN_MAX_FRAME_BYTES; \
        (link_).peer_node_id = (peer_); \
        (context_).is_up = &links_up; \
        (context_).drop_frames = &no_drop; \
        (context_).async_submit = true; \
    } while (0)
    AODV_INIT_LINK(ar, contexts[0], 111U, UINT32_C(3));
    AODV_INIT_LINK(ra, contexts[1], 112U, UINT32_C(1));
    AODV_INIT_LINK(br, contexts[2], 113U, UINT32_C(3));
    AODV_INIT_LINK(rb, contexts[3], 114U, UINT32_C(2));
    AODV_INIT_LINK(rx, contexts[4], 115U, UINT32_C(4));
    AODV_INIT_LINK(xr, contexts[5], 116U, UINT32_C(3));
    AODV_INIT_LINK(xt, contexts[6], 117U, UINT32_C(5));
    AODV_INIT_LINK(tx, contexts[7], 118U, UINT32_C(4));
#undef AODV_INIT_LINK
    contexts[0].peer = &r; contexts[0].peer_ingress = &ra;
    contexts[1].peer = &a; contexts[1].peer_ingress = &ar;
    contexts[2].peer = &r; contexts[2].peer_ingress = &rb;
    contexts[3].peer = &b; contexts[3].peer_ingress = &br;
    contexts[4].peer = &x; contexts[4].peer_ingress = &xr;
    contexts[5].peer = &r; contexts[5].peer_ingress = &rx;
    contexts[6].peer = &t; contexts[6].peer_ingress = &tx;
    contexts[7].peer = &x; contexts[7].peer_ingress = &xt;
    contexts[1].drop_frames = &drop_ra;
    contexts[3].drop_frames = &drop_rb;
    contexts[6].is_up = &tail_up;
    contexts[7].is_up = &tail_up;
    TEST_ASSERT(ucn_node_register_link(&a, &ar) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&r, &ra) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &br) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&r, &rb) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&r, &rx) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&x, &xr) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&x, &xt) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&t, &tx) == UCN_OK);
    ucn_node_set_rx_handler(&t, aodv_rx, &received);

    b.next_route_request_id = UINT32_C(100);
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(5), 10U) == UCN_OK);
    TEST_ASSERT(ucn_node_route_pending(&a, UINT32_C(5)));
    TEST_ASSERT(ucn_node_discover_route(&b, UINT32_C(5), 20U) == UCN_OK);
    TEST_ASSERT(ucn_node_route_pending(&b, UINT32_C(5)));
    drop_ra = true;
    drop_rb = true;
    r.now_ms = x.now_ms = t.now_ms =
        10U + UCN_ROUTE_RING_TIMEOUT_MS + 1U;
    TEST_ASSERT(ucn_node_step(&a, 10U + UCN_ROUTE_RING_TIMEOUT_MS + 1U) ==
                UCN_OK);
    r.now_ms = x.now_ms = t.now_ms =
        20U + UCN_ROUTE_RING_TIMEOUT_MS + 1U;
    TEST_ASSERT(ucn_node_step(&b, 20U + UCN_ROUTE_RING_TIMEOUT_MS + 1U) ==
                UCN_OK);
    TEST_ASSERT(ucn_node_route_pending(&a, UINT32_C(5)));
    TEST_ASSERT(ucn_node_route_pending(&b, UINT32_C(5)));
    TEST_ASSERT(contexts[1].last_route_reply_length != 0U);
    TEST_ASSERT(contexts[3].last_route_reply_length != 0U);
    drop_ra = false;
    drop_rb = false;
    TEST_ASSERT(ucn_node_receive(&b, &br, contexts[3].last_route_reply_frame,
                                 contexts[3].last_route_reply_length) == UCN_OK);
    TEST_ASSERT(!ucn_node_route_pending(&b, UINT32_C(5)));
    TEST_ASSERT(ucn_node_route_pending(&a, UINT32_C(5)));
    TEST_ASSERT(ucn_node_receive(&a, &ar, contexts[1].last_route_reply_frame,
                                 contexts[1].last_route_reply_length) == UCN_OK);
    TEST_ASSERT(!ucn_node_route_pending(&a, UINT32_C(5)));

    r_a_t = aodv_find_route(&r, UINT32_C(1), UINT32_C(5));
    r_b_t = aodv_find_route(&r, UINT32_C(2), UINT32_C(5));
    TEST_ASSERT(r_a_t != NULL && r_b_t != NULL && r_a_t != r_b_t);
    TEST_ASSERT(r_a_t->route_epoch != 0U && r_b_t->route_epoch != 0U &&
                r_a_t->route_epoch != r_b_t->route_epoch);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(5), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload_a, 1U) ==
                UCN_OK);
    TEST_ASSERT(ucn_node_send(&b, UINT32_C(5), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload_b, 1U) ==
                UCN_OK);
    TEST_ASSERT(received.count == 2U && received.last_payload == payload_b);

    /* A frame in A's Source domain must not borrow B's numerically valid
     * Route Epoch.  Inject at the target to isolate the destination-side
     * Current/Previous check from forwarding and RERR side effects. */
    TEST_ASSERT(contexts[0].last_data_length != 0U);
    TEST_ASSERT(ucn_frame_decode(contexts[0].last_data_frame,
                                 contexts[0].last_data_length,
                                 &crossed_epoch_frame) == UCN_OK);
    crossed_epoch_frame.sequence += UINT32_C(1000);
    crossed_epoch_frame.route_epoch = r_b_t->route_epoch;
    TEST_ASSERT(ucn_frame_encode(&crossed_epoch_frame, crossed_epoch_encoded,
                                 sizeof(crossed_epoch_encoded),
                                 &crossed_epoch_length) == UCN_OK);
    epoch_rejected_before = t.stats.route_epoch_rejected;
    TEST_ASSERT(ucn_node_receive(&t, &tx, crossed_epoch_encoded,
                                 crossed_epoch_length) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(received.count == 2U);
    TEST_ASSERT(t.stats.route_epoch_rejected == epoch_rejected_before + 1U);

    summary_count = ucn_node_copy_route_summaries(
        &r, summaries, UCN_MAX_ROUTES);
    TEST_ASSERT(summary_count >= 4U);
    for (index = 0U; index < summary_count; ++index) {
        if (!summaries[index].is_static &&
            summaries[index].route_origin == UINT32_C(1) &&
            summaries[index].destination == UINT32_C(5)) {
            saw_a_t = true;
        }
        if (!summaries[index].is_static &&
            summaries[index].route_origin == UINT32_C(2) &&
            summaries[index].destination == UINT32_C(5)) {
            saw_b_t = true;
        }
    }
    TEST_ASSERT(saw_a_t && saw_b_t);

#if UCN_FEATURE_CANDIDATE_ROUTING
    /* Request IDs are local to each Origin.  Reuse the exact same ID range
     * and prove Candidate slots do not alias at the common relay. */
    a.next_route_request_id = UINT32_C(200);
    b.next_route_request_id = UINT32_C(200);
    r.now_ms = x.now_ms = t.now_ms = 600U;
    TEST_ASSERT(ucn_node_refresh_route(&a, UINT32_C(5), 600U) == UCN_OK);
    r.now_ms = x.now_ms = t.now_ms = 900U;
    TEST_ASSERT(ucn_node_refresh_route(&b, UINT32_C(5), 900U) == UCN_OK);
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        const ucn_candidate_route_t *candidate = &r.candidates[index];

        if (!candidate->valid || candidate->candidate_id != UINT32_C(200) ||
            candidate->destination != UINT32_C(5)) {
            continue;
        }
        saw_candidate_a_t = saw_candidate_a_t ||
                            candidate->route_origin == UINT32_C(1);
        saw_candidate_b_t = saw_candidate_b_t ||
                            candidate->route_origin == UINT32_C(2);
    }
    TEST_ASSERT(saw_candidate_a_t && saw_candidate_b_t);
#endif

    /* A Route Error is sourced by the relay that observes the failed Link,
     * while its reverse Route Instance is owned by the unreachable target.
     * Prove the RERR reaches A through the (T,A) reverse domain and invalidates
     * only A's (A,T) forward instance, leaving B's independent route intact. */
    tail_up = false;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(5), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload_a, 1U) ==
                UCN_OK);
    TEST_ASSERT(aodv_find_route(&a, UINT32_C(1), UINT32_C(5)) == NULL);
    TEST_ASSERT(aodv_find_route(&b, UINT32_C(2), UINT32_C(5)) != NULL);
    TEST_ASSERT(aodv_find_route(&r, UINT32_C(1), UINT32_C(5)) == NULL);
    TEST_ASSERT(aodv_find_route(&r, UINT32_C(2), UINT32_C(5)) != NULL);
    tail_up = true;
    TEST_ASSERT(ucn_node_send(&b, UINT32_C(5), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload_b, 1U) ==
                UCN_OK);
    TEST_ASSERT(received.count == 3U && received.last_payload == payload_b);
    return 0;
}

#if UCN_FEATURE_CANDIDATE_ROUTING
/*
 * EN: Encodes one deterministic PATH_ACTIVATE control frame for atomicity
 *     and retry tests.
 * 中文：为原子性与重试测试编码一个确定性的 PATH_ACTIVATE 控制帧。
 */
static int aodv_encode_path_activation(uint8_t message_type,
                                       ucn_node_id_t source,
                                       ucn_node_id_t destination,
                                       uint32_t sequence,
                                       uint32_t session_id,
                                       uint32_t candidate_id,
                                       uint16_t route_epoch,
                                       uint8_t *encoded,
                                       size_t *encoded_length)
{
    uint8_t payload[6U];
    ucn_frame_t frame;

    payload[0] = (uint8_t)(candidate_id >> 24U);
    payload[1] = (uint8_t)(candidate_id >> 16U);
    payload[2] = (uint8_t)(candidate_id >> 8U);
    payload[3] = (uint8_t)candidate_id;
    payload[4] = (uint8_t)(route_epoch >> 8U);
    payload[5] = (uint8_t)route_epoch;
    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = 4U;
    frame.network_id = UINT32_C(42);
    frame.source = source;
    frame.destination = destination;
    frame.sequence = sequence;
    frame.session_id = session_id;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    return ucn_frame_encode(&frame, encoded, UCN_MAX_FRAME_BYTES,
                            encoded_length) == UCN_OK ? 0 : 1;
}

/*
 * EN: Encodes one deterministic PATH_PROBE or PATH_PROBE_ACK frame.
 * 中文：编码一个确定性的 PATH_PROBE 或 PATH_PROBE_ACK 帧。
 */
static int aodv_encode_path_probe(uint8_t message_type,
                                  ucn_node_id_t source,
                                  ucn_node_id_t destination,
                                  uint32_t sequence,
                                  uint32_t session_id,
                                  uint32_t candidate_id,
                                  uint32_t probe_index,
                                  uint32_t sent_at_ms,
                                  uint8_t *encoded,
                                  size_t *encoded_length)
{
    uint8_t payload[12U];
    ucn_frame_t frame;

    payload[0] = (uint8_t)(candidate_id >> 24U);
    payload[1] = (uint8_t)(candidate_id >> 16U);
    payload[2] = (uint8_t)(candidate_id >> 8U);
    payload[3] = (uint8_t)candidate_id;
    payload[4] = (uint8_t)(probe_index >> 24U);
    payload[5] = (uint8_t)(probe_index >> 16U);
    payload[6] = (uint8_t)(probe_index >> 8U);
    payload[7] = (uint8_t)probe_index;
    payload[8] = (uint8_t)(sent_at_ms >> 24U);
    payload[9] = (uint8_t)(sent_at_ms >> 16U);
    payload[10] = (uint8_t)(sent_at_ms >> 8U);
    payload[11] = (uint8_t)sent_at_ms;
    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = 4U;
    frame.network_id = UINT32_C(42);
    frame.source = source;
    frame.destination = destination;
    frame.sequence = sequence;
    frame.session_id = session_id;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    return ucn_frame_encode(&frame, encoded, UCN_MAX_FRAME_BYTES,
                            encoded_length) == UCN_OK ? 0 : 1;
}

/*
 * EN: Encodes one deterministic PATH_ACTIVATE control frame.
 * 中文：编码一个确定性的 PATH_ACTIVATE 控制帧。
 */
static int aodv_encode_path_activate(ucn_node_id_t source,
                                     ucn_node_id_t destination,
                                     uint32_t sequence,
                                     uint32_t session_id,
                                     uint32_t candidate_id,
                                     uint16_t route_epoch,
                                     uint8_t *encoded,
                                     size_t *encoded_length)
{
    return aodv_encode_path_activation(
        UCN_MSG_PATH_ACTIVATE, source, destination, sequence, session_id,
        candidate_id, route_epoch, encoded, encoded_length);
}

/*
 * EN: Encodes one deterministic PATH_ACTIVATE_ACK control frame.
 * 中文：编码一个确定性的 PATH_ACTIVATE_ACK 控制帧。
 */
static int aodv_encode_path_activate_ack(ucn_node_id_t source,
                                         ucn_node_id_t destination,
                                         uint32_t sequence,
                                         uint32_t session_id,
                                         uint32_t candidate_id,
                                         uint16_t route_epoch,
                                         uint8_t *encoded,
                                         size_t *encoded_length)
{
    return aodv_encode_path_activation(
        UCN_MSG_PATH_ACTIVATE_ACK, source, destination, sequence, session_id,
        candidate_id, route_epoch, encoded, encoded_length);
}

/*
 * EN: Encodes one W0 Candidate RREP with independently selected path fields.
 * 中文：编码一个路径字段可独立指定的 W0 Candidate RREP。
 */
static int aodv_encode_candidate_route_reply(
    ucn_node_id_t source,
    ucn_node_id_t destination,
    uint32_t sequence,
    uint32_t session_id,
    uint32_t candidate_id,
    uint32_t route_cost,
    uint8_t hop_count,
    uint8_t *encoded,
    size_t *encoded_length)
{
    uint8_t payload[10U];
    ucn_frame_t frame;

    payload[0] = (uint8_t)(candidate_id >> 24U);
    payload[1] = (uint8_t)(candidate_id >> 16U);
    payload[2] = (uint8_t)(candidate_id >> 8U);
    payload[3] = (uint8_t)candidate_id;
    payload[4] = (uint8_t)(route_cost >> 16U);
    payload[5] = (uint8_t)(route_cost >> 8U);
    payload[6] = (uint8_t)route_cost;
    payload[7] = hop_count;
    payload[8] = 1U;
    payload[9] = 0U;
    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_ROUTE_REPLY;
    frame.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = 4U;
    frame.network_id = UINT32_C(42);
    frame.source = source;
    frame.destination = destination;
    frame.sequence = sequence;
    frame.session_id = session_id;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    return ucn_frame_encode(&frame, encoded, UCN_MAX_FRAME_BYTES,
                            encoded_length) == UCN_OK ? 0 : 1;
}

/*
 * EN: Installs one valid bounded Candidate fixture without invoking discovery.
 * 中文：不启动路由发现，直接安装一个合法的有界 Candidate 测试项。
 */
static void aodv_set_candidate(ucn_candidate_route_t *candidate,
                               ucn_node_id_t route_origin,
                               ucn_node_id_t destination,
                               uint32_t candidate_id,
                               ucn_link_t *egress_link)
{
    (void)memset(candidate, 0, sizeof(*candidate));
    candidate->valid = true;
    candidate->wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    candidate->route_origin = route_origin;
    candidate->destination = destination;
    candidate->candidate_id = candidate_id;
    candidate->egress_link = egress_link;
    candidate->expires_at_ms = 1000U;
    candidate->route_cost = 1U;
    candidate->hop_count = 1U;
}

/*
 * EN: Proves PATH_ACTIVATE preflights both Route directions, rolls back local
 *     state on downstream/ACK send failure, and retries idempotently.
 * 中文：验证 PATH_ACTIVATE 会预检双向 Route，在下游或 ACK 发送失败时回滚，
 *       且后续重试保持幂等。
 */
static int test_candidate_activation_is_atomic_and_idempotent(void)
{
    bool link_up = true;
    bool drop_frames = true;
    const uint32_t candidate_id = UINT32_C(0x01020304);
    ucn_node_t relay, target;
    ucn_link_t relay_ingress, relay_egress, target_ingress;
    aodv_link_context_t relay_in_context, relay_out_context, target_context;
    ucn_route_entry_t route_before[UCN_MAX_ROUTES];
    ucn_candidate_route_t candidate_before[UCN_MAX_CANDIDATE_ROUTES];
    ucn_node_stats_t stats_before;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    size_t index;
    const ucn_route_entry_t *forward;
    const ucn_route_entry_t *reverse;
    uint32_t switches_before;

    (void)memset(&relay, 0, sizeof(relay));
    (void)memset(&target, 0, sizeof(target));
    (void)memset(&relay_ingress, 0, sizeof(relay_ingress));
    (void)memset(&relay_egress, 0, sizeof(relay_egress));
    (void)memset(&target_ingress, 0, sizeof(target_ingress));
    (void)memset(&relay_in_context, 0, sizeof(relay_in_context));
    (void)memset(&relay_out_context, 0, sizeof(relay_out_context));
    (void)memset(&target_context, 0, sizeof(target_context));
    TEST_ASSERT(aodv_init_node(&relay, UINT32_C(2)) == 0);
    TEST_ASSERT(aodv_init_node(&target, UINT32_C(3)) == 0);

#define AODV_ATOMIC_LINK(link_, context_, id_, peer_) \
    do { \
        (link_).ops = &AODV_LINK_OPS; \
        (link_).context = &(context_); \
        (link_).link_id = (id_); \
        (link_).mtu = UCN_MAX_FRAME_BYTES; \
        (link_).peer_node_id = (peer_); \
        (context_).is_up = &link_up; \
        (context_).drop_frames = &drop_frames; \
    } while (0)
    AODV_ATOMIC_LINK(relay_ingress, relay_in_context, 141U, UINT32_C(1));
    AODV_ATOMIC_LINK(relay_egress, relay_out_context, 142U, UINT32_C(3));
    AODV_ATOMIC_LINK(target_ingress, target_context, 143U, UINT32_C(2));
#undef AODV_ATOMIC_LINK
    TEST_ASSERT(ucn_node_register_link(&relay, &relay_ingress) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&relay, &relay_egress) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&target, &target_ingress) == UCN_OK);

    /* Leave exactly one Route slot. A relay activation needs two distinct
     * slots and must reject before writing either direction or sending. */
    for (index = 0U; index + 1U < UCN_MAX_ROUTES; ++index) {
        TEST_ASSERT(ucn_node_add_route(&relay,
                                      (ucn_node_id_t)(20U + index),
                                      &relay_ingress) == UCN_OK);
    }
    aodv_set_candidate(&relay.candidates[0], UINT32_C(1), UINT32_C(3),
                       candidate_id, &relay_egress);
    aodv_set_candidate(&relay.candidates[1], UINT32_C(3), UINT32_C(1),
                       candidate_id, &relay_ingress);
    relay.candidates[0].path_snapshot_frozen = true;
    relay.candidates[1].path_snapshot_frozen = true;
    (void)memcpy(route_before, relay.routes, sizeof(route_before));
    (void)memcpy(candidate_before, relay.candidates, sizeof(candidate_before));
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(3), 1U, 77U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_ingress, encoded,
                                 encoded_length) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(relay_out_context.send_count == 0U);
    TEST_ASSERT(memcmp(route_before, relay.routes, sizeof(route_before)) == 0);
    TEST_ASSERT(memcmp(candidate_before, relay.candidates,
                       sizeof(candidate_before)) == 0);

    /* Free the second slot. A downstream failure occurs after local commit,
     * so both Route writes must roll back and Candidate evidence must remain. */
    (void)memset(&relay.routes[UCN_MAX_ROUTES - 2U], 0,
                 sizeof(relay.routes[0]));
    relay_out_context.forced_result = UCN_ERR_LINK_DOWN;
    (void)memcpy(route_before, relay.routes, sizeof(route_before));
    (void)memcpy(candidate_before, relay.candidates, sizeof(candidate_before));
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(3), 2U, 77U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_ingress, encoded,
                                 encoded_length) == UCN_ERR_LINK_DOWN);
    TEST_ASSERT(relay_out_context.send_count == 1U);
    TEST_ASSERT(memcmp(route_before, relay.routes, sizeof(route_before)) == 0);
    TEST_ASSERT(memcmp(candidate_before, relay.candidates,
                       sizeof(candidate_before)) == 0);

    relay_out_context.forced_result = UCN_OK;
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(3), 3U, 77U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_ingress, encoded,
                                 encoded_length) == UCN_OK);
    forward = aodv_find_route(&relay, UINT32_C(1), UINT32_C(3));
    reverse = aodv_find_route(&relay, UINT32_C(3), UINT32_C(1));
    TEST_ASSERT(forward != NULL && reverse != NULL &&
                forward->route_epoch == 10U && reverse->route_epoch == 10U);
    TEST_ASSERT(relay.candidates[0].valid && relay.candidates[1].valid);
    (void)memcpy(route_before, relay.routes, sizeof(route_before));
    switches_before = relay.stats.route_switches;
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(3), 4U, 77U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_ingress, encoded,
                                 encoded_length) == UCN_OK);
    TEST_ASSERT(memcmp(route_before, relay.routes, sizeof(route_before)) == 0);
    TEST_ASSERT(relay.stats.route_switches == switches_before);

    /* Once either relay Candidate is bound to Epoch 10, the same Candidate
     * ID cannot be rebound to Epoch 11.  The rejection is fully zero-write. */
    (void)memcpy(route_before, relay.routes, sizeof(route_before));
    (void)memcpy(candidate_before, relay.candidates, sizeof(candidate_before));
    stats_before = relay.stats;
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(3), 5U, 77U, candidate_id, 11U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_ingress, encoded,
                                 encoded_length) == UCN_ERR_STATE);
    TEST_ASSERT(memcmp(route_before, relay.routes, sizeof(route_before)) == 0);
    TEST_ASSERT(memcmp(candidate_before, relay.candidates,
                       sizeof(candidate_before)) == 0);
    TEST_ASSERT(memcmp(&stats_before, &relay.stats, sizeof(stats_before)) == 0);

    /* At the target, ACK backpressure must also roll back the reverse Route.
     * A later retry succeeds and an exact repeat remains byte-idempotent. */
    aodv_set_candidate(&target.candidates[0], UINT32_C(3), UINT32_C(1),
                       candidate_id, &target_ingress);
    target.candidates[0].path_snapshot_frozen = true;
    target_context.forced_result = UCN_ERR_NO_SPACE;
    (void)memcpy(route_before, target.routes, sizeof(route_before));
    (void)memcpy(candidate_before, target.candidates, sizeof(candidate_before));
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(3), 10U, 88U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&target, &target_ingress, encoded,
                                 encoded_length) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(target_context.send_count == 1U);
    TEST_ASSERT(memcmp(route_before, target.routes, sizeof(route_before)) == 0);
    TEST_ASSERT(memcmp(candidate_before, target.candidates,
                       sizeof(candidate_before)) == 0);
    target_context.forced_result = UCN_OK;
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(3), 11U, 88U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&target, &target_ingress, encoded,
                                 encoded_length) == UCN_OK);
    reverse = aodv_find_route(&target, UINT32_C(3), UINT32_C(1));
    TEST_ASSERT(reverse != NULL && reverse->route_epoch == 10U &&
                target.candidates[0].valid);
    (void)memcpy(route_before, target.routes, sizeof(route_before));
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(3), 12U, 88U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&target, &target_ingress, encoded,
                                 encoded_length) == UCN_OK);
    TEST_ASSERT(memcmp(route_before, target.routes, sizeof(route_before)) == 0);
    return 0;
}

/*
 * EN: Proves an Activate ACK is admitted only for the exact locally-originated,
 *     probe-complete, sent Candidate/Epoch transaction.
 * 中文：验证 Activate ACK 只能匹配本地发起、探测完成、已发送且 Epoch 精确
 *       一致的 Candidate 事务。
 */
static int test_path_activate_ack_is_exactly_bound(void)
{
    bool link_up = true;
    bool drop_frames = true;
    const uint32_t candidate_id = UINT32_C(0x10203040);
    ucn_node_t origin;
    ucn_link_t ingress;
    aodv_link_context_t context;
    ucn_route_entry_t routes_before[UCN_MAX_ROUTES];
    ucn_candidate_route_t candidates_before[UCN_MAX_CANDIDATE_ROUTES];
    ucn_node_stats_t stats_before;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    const ucn_route_entry_t *installed;

    (void)memset(&origin, 0, sizeof(origin));
    (void)memset(&ingress, 0, sizeof(ingress));
    (void)memset(&context, 0, sizeof(context));
    TEST_ASSERT(aodv_init_node(&origin, UINT32_C(1)) == 0);
    origin.now_ms = 100U;
    ingress.ops = &AODV_LINK_OPS;
    ingress.context = &context;
    ingress.link_id = 151U;
    ingress.mtu = UCN_MAX_FRAME_BYTES;
    ingress.peer_node_id = UINT32_C(9);
    context.is_up = &link_up;
    context.drop_frames = &drop_frames;
    TEST_ASSERT(ucn_node_register_link(&origin, &ingress) == UCN_OK);
    aodv_set_candidate(&origin.candidates[0], UINT32_C(1), UINT32_C(9),
                       candidate_id, &ingress);
    origin.candidates[0].originated_here = true;
    origin.candidates[0].path_snapshot_frozen = true;
    origin.candidates[0].probes_sent = UCN_PATH_PROBE_REQUIRED_ACKS;
    origin.candidates[0].probes_acked = UCN_PATH_PROBE_REQUIRED_ACKS;
    origin.candidates[0].activation_sent = true;
    origin.candidates[0].activation_attempts = 1U;
    origin.candidates[0].activation_ack_deadline_ms = 1000U;
    origin.candidates[0].route_epoch = 10U;

#define ASSERT_ACK_REJECTED_WITHOUT_STATE_CHANGE(sequence_, epoch_) \
    do { \
        (void)memcpy(routes_before, origin.routes, sizeof(routes_before)); \
        (void)memcpy(candidates_before, origin.candidates, \
                     sizeof(candidates_before)); \
        stats_before = origin.stats; \
        TEST_ASSERT(aodv_encode_path_activate_ack( \
                        UINT32_C(9), UINT32_C(1), (sequence_), 91U, \
                        candidate_id, (epoch_), encoded, &encoded_length) == 0); \
        TEST_ASSERT(ucn_node_receive(&origin, &ingress, encoded, \
                                     encoded_length) == UCN_ERR_STATE); \
        TEST_ASSERT(memcmp(routes_before, origin.routes, \
                           sizeof(routes_before)) == 0); \
        TEST_ASSERT(memcmp(candidates_before, origin.candidates, \
                           sizeof(candidates_before)) == 0); \
        TEST_ASSERT(memcmp(&stats_before, &origin.stats, \
                           sizeof(stats_before)) == 0); \
    } while (0)

    ASSERT_ACK_REJECTED_WITHOUT_STATE_CHANGE(1U, 9U);
    origin.candidates[0].probes_sent =
        (uint8_t)(UCN_PATH_PROBE_REQUIRED_ACKS - 1U);
    ASSERT_ACK_REJECTED_WITHOUT_STATE_CHANGE(2U, 10U);
    origin.candidates[0].probes_sent = UCN_PATH_PROBE_REQUIRED_ACKS;
    origin.candidates[0].activation_sent = false;
    ASSERT_ACK_REJECTED_WITHOUT_STATE_CHANGE(3U, 10U);
    origin.candidates[0].activation_sent = true;
    origin.candidates[0].probes_acked =
        (uint8_t)(UCN_PATH_PROBE_REQUIRED_ACKS - 1U);
    ASSERT_ACK_REJECTED_WITHOUT_STATE_CHANGE(4U, 10U);
    origin.candidates[0].probes_acked = UCN_PATH_PROBE_REQUIRED_ACKS;
    origin.candidates[0].originated_here = false;
    ASSERT_ACK_REJECTED_WITHOUT_STATE_CHANGE(5U, 10U);
    origin.candidates[0].originated_here = true;

    TEST_ASSERT(aodv_encode_path_activate_ack(
                    UINT32_C(9), UINT32_C(1), 6U, 91U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&origin, &ingress, encoded,
                                 encoded_length) == UCN_OK);
    installed = aodv_find_route(&origin, UINT32_C(1), UINT32_C(9));
    TEST_ASSERT(installed != NULL && installed->route_epoch == 10U);
    TEST_ASSERT(origin.candidates[0].activation_acknowledged);
    TEST_ASSERT(origin.stats.path_activate_acks_received == 1U &&
                origin.stats.route_switches == 1U);

    (void)memcpy(routes_before, origin.routes, sizeof(routes_before));
    (void)memcpy(candidates_before, origin.candidates,
                 sizeof(candidates_before));
    stats_before = origin.stats;
    TEST_ASSERT(aodv_encode_path_activate_ack(
                    UINT32_C(9), UINT32_C(1), 7U, 91U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&origin, &ingress, encoded,
                                 encoded_length) == UCN_OK);
    TEST_ASSERT(memcmp(routes_before, origin.routes, sizeof(routes_before)) == 0);
    TEST_ASSERT(memcmp(candidates_before, origin.candidates,
                       sizeof(candidates_before)) == 0);
    TEST_ASSERT(memcmp(&stats_before, &origin.stats, sizeof(stats_before)) == 0);
#undef ASSERT_ACK_REJECTED_WITHOUT_STATE_CHANGE
    return 0;
}

/*
 * EN: Proves lost PATH_ACTIVATE ACKs trigger bounded exact-identity retries,
 *     later converge with fresh outer Sequences, and exhaust without replacing
 *     the old active Route.
 * 中文：验证 PATH_ACTIVATE ACK 丢失会触发有界同身份重试，以新的外层 Sequence
 *       收敛，并在耗尽时保留旧 Active Route。
 */
static int test_path_activate_ack_retry_is_bounded(void)
{
    bool link_up = true;
    bool activate_drop = true;
    bool ack_drop = false;
    bool exhaustion_drop = true;
    const uint32_t candidate_id = UINT32_C(0x55667788);
    ucn_node_t origin, target, exhausted;
    ucn_link_t origin_out, target_out, exhausted_out;
    aodv_link_context_t origin_context, target_context, exhausted_context;
    const ucn_route_entry_t *route;
    uint32_t index;

    (void)memset(&origin, 0, sizeof(origin));
    (void)memset(&target, 0, sizeof(target));
    (void)memset(&exhausted, 0, sizeof(exhausted));
    (void)memset(&origin_out, 0, sizeof(origin_out));
    (void)memset(&target_out, 0, sizeof(target_out));
    (void)memset(&exhausted_out, 0, sizeof(exhausted_out));
    (void)memset(&origin_context, 0, sizeof(origin_context));
    (void)memset(&target_context, 0, sizeof(target_context));
    (void)memset(&exhausted_context, 0, sizeof(exhausted_context));
    TEST_ASSERT(aodv_init_node(&origin, UINT32_C(1)) == 0);
    TEST_ASSERT(aodv_init_node(&target, UINT32_C(9)) == 0);
    TEST_ASSERT(aodv_init_node(&exhausted, UINT32_C(11)) == 0);

    origin_out.ops = &AODV_LINK_OPS;
    origin_out.context = &origin_context;
    origin_out.link_id = 161U;
    origin_out.mtu = UCN_MAX_FRAME_BYTES;
    origin_out.peer_node_id = UINT32_C(9);
    target_out.ops = &AODV_LINK_OPS;
    target_out.context = &target_context;
    target_out.link_id = 162U;
    target_out.mtu = UCN_MAX_FRAME_BYTES;
    target_out.peer_node_id = UINT32_C(1);
    origin_context.peer = &target;
    origin_context.peer_ingress = &target_out;
    origin_context.is_up = &link_up;
    origin_context.drop_frames = &activate_drop;
    target_context.peer = &origin;
    target_context.peer_ingress = &origin_out;
    target_context.is_up = &link_up;
    target_context.drop_frames = &ack_drop;
    TEST_ASSERT(ucn_node_register_link(&origin, &origin_out) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&target, &target_out) == UCN_OK);
    aodv_set_candidate(&origin.candidates[0], UINT32_C(1), UINT32_C(9),
                       candidate_id, &origin_out);
    origin.candidates[0].originated_here = true;
    origin.candidates[0].path_snapshot_frozen = true;
    origin.candidates[0].probes_sent = UCN_PATH_PROBE_REQUIRED_ACKS;
    origin.candidates[0].probes_acked = UCN_PATH_PROBE_REQUIRED_ACKS;
    origin.candidates[0].expires_at_ms = 5000U;
    aodv_set_candidate(&target.candidates[0], UINT32_C(9), UINT32_C(1),
                       candidate_id, &target_out);
    target.candidates[0].path_snapshot_frozen = true;
    target.candidates[0].expires_at_ms = 5000U;

    TEST_ASSERT(ucn_node_step(&origin, 0U) == UCN_OK);
    TEST_ASSERT(origin_context.path_activate_count == 1U &&
                origin.candidates[0].activation_sent &&
                !origin.candidates[0].activation_acknowledged &&
                origin.candidates[0].activation_attempts == 1U);
    TEST_ASSERT(ucn_node_step(
                    &origin, UCN_PATH_ACTIVATE_RETRY_INTERVAL_MS - 1U) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(origin_context.path_activate_count == 1U);

    activate_drop = false;
    TEST_ASSERT(ucn_node_step(&origin,
                              UCN_PATH_ACTIVATE_RETRY_INTERVAL_MS) == UCN_OK);
    TEST_ASSERT(origin_context.path_activate_count == 2U &&
                origin_context.path_activate_candidate_ids[0] == candidate_id &&
                origin_context.path_activate_candidate_ids[1] == candidate_id &&
                origin_context.path_activate_epochs[0] != 0U &&
                origin_context.path_activate_epochs[0] ==
                    origin_context.path_activate_epochs[1] &&
                origin_context.path_activate_sequences[0] !=
                    origin_context.path_activate_sequences[1]);
    TEST_ASSERT(origin.candidates[0].activation_acknowledged &&
                origin.stats.path_activate_retries_sent == 1U &&
                origin.stats.path_activate_acks_received == 1U);
    route = aodv_find_route(&origin, UINT32_C(1), UINT32_C(9));
    TEST_ASSERT(route != NULL &&
                route->route_epoch == origin_context.path_activate_epochs[0]);
    TEST_ASSERT(ucn_node_step(
                    &origin, UCN_PATH_ACTIVATE_RETRY_INTERVAL_MS * 2U) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(origin_context.path_activate_count == 2U);

    exhausted_out.ops = &AODV_LINK_OPS;
    exhausted_out.context = &exhausted_context;
    exhausted_out.link_id = 163U;
    exhausted_out.mtu = UCN_MAX_FRAME_BYTES;
    exhausted_out.peer_node_id = UINT32_C(12);
    exhausted_context.is_up = &link_up;
    exhausted_context.drop_frames = &exhaustion_drop;
    TEST_ASSERT(ucn_node_register_link(&exhausted, &exhausted_out) == UCN_OK);
    exhausted.routes[0].valid = true;
    exhausted.routes[0].route_origin = UINT32_C(11);
    exhausted.routes[0].destination = UINT32_C(12);
    exhausted.routes[0].egress_link = &exhausted_out;
    exhausted.routes[0].expires_at_ms = 5000U;
    exhausted.routes[0].route_cost = 10U;
    exhausted.routes[0].hop_count = 1U;
    exhausted.routes[0].route_epoch = 5U;
    aodv_set_candidate(&exhausted.candidates[0], UINT32_C(11),
                       UINT32_C(12), candidate_id + 1U, &exhausted_out);
    exhausted.candidates[0].originated_here = true;
    exhausted.candidates[0].path_snapshot_frozen = true;
    exhausted.candidates[0].probes_sent = UCN_PATH_PROBE_REQUIRED_ACKS;
    exhausted.candidates[0].probes_acked = UCN_PATH_PROBE_REQUIRED_ACKS;
    exhausted.candidates[0].expires_at_ms =
        UCN_PATH_ACTIVATE_ACK_TIMEOUT_MS + 2000U;

    for (index = 0U;
         index <= (uint32_t)UCN_PATH_ACTIVATE_MAX_RETRIES;
         ++index) {
        TEST_ASSERT(ucn_node_step(
                        &exhausted,
                        index * UCN_PATH_ACTIVATE_RETRY_INTERVAL_MS) == UCN_OK);
    }
    TEST_ASSERT(exhausted_context.path_activate_count ==
                    (uint32_t)UCN_PATH_ACTIVATE_MAX_RETRIES + 1U &&
                exhausted.candidates[0].valid);
    for (index = 1U; index < exhausted_context.path_activate_count; ++index) {
        TEST_ASSERT(exhausted_context.path_activate_candidate_ids[index] ==
                        exhausted_context.path_activate_candidate_ids[0] &&
                    exhausted_context.path_activate_epochs[index] ==
                        exhausted_context.path_activate_epochs[0] &&
                    exhausted_context.path_activate_sequences[index] !=
                        exhausted_context.path_activate_sequences[index - 1U]);
    }
    TEST_ASSERT(ucn_node_step(&exhausted,
                              UCN_PATH_ACTIVATE_ACK_TIMEOUT_MS) ==
                UCN_ERR_EXHAUSTED);
    TEST_ASSERT(!exhausted.candidates[0].valid &&
                exhausted.stats.path_activate_retry_exhausted == 1U &&
                exhausted.stats.path_activate_retries_sent ==
                    UCN_PATH_ACTIVATE_MAX_RETRIES);
    route = aodv_find_route(&exhausted, UINT32_C(11), UINT32_C(12));
    TEST_ASSERT(route != NULL && route->route_epoch == 5U);
    return 0;
}

/*
 * EN: Proves late same-ID RREPs cannot move a Candidate after Probe evidence,
 *     while waiting for an Activate ACK, or after a failed Activate submit.
 * 中文：验证同 ID 的迟到 RREP 不能在 Probe 证明后、等待 Activate ACK 时，
 *       或 Activate 提交失败后迁移 Candidate 路径。
 */
static int test_candidate_path_is_frozen_after_transaction_start(void)
{
    bool link_up = true;
    bool drop_frames = true;
    const uint32_t candidate_id = UINT32_C(0x30405060);
    size_t phase;

    for (phase = 0U; phase < 3U; ++phase) {
        ucn_node_t node;
        ucn_link_t old_link, new_link;
        aodv_link_context_t old_context, new_context;
        ucn_candidate_route_t candidate_before;
        ucn_route_entry_t routes_before[UCN_MAX_ROUTES];
        const ucn_route_entry_t *installed;
        uint8_t encoded[UCN_MAX_FRAME_BYTES];
        size_t encoded_length = 0U;
        uint16_t frozen_epoch;

        (void)memset(&node, 0, sizeof(node));
        (void)memset(&old_link, 0, sizeof(old_link));
        (void)memset(&new_link, 0, sizeof(new_link));
        (void)memset(&old_context, 0, sizeof(old_context));
        (void)memset(&new_context, 0, sizeof(new_context));
        TEST_ASSERT(aodv_init_node(&node, UINT32_C(1)) == 0);
        old_link.ops = &AODV_LINK_OPS;
        old_link.context = &old_context;
        old_link.link_id = 171U;
        old_link.mtu = UCN_MAX_FRAME_BYTES;
        old_link.peer_node_id = UINT32_C(9);
        new_link.ops = &AODV_LINK_OPS;
        new_link.context = &new_context;
        new_link.link_id = 172U;
        new_link.mtu = UCN_MAX_FRAME_BYTES;
        new_link.peer_node_id = UINT32_C(9);
        old_context.is_up = &link_up;
        old_context.drop_frames = &drop_frames;
        new_context.is_up = &link_up;
        new_context.drop_frames = &drop_frames;
        new_context.route_cost_valid = true;
        new_context.route_cost = 1U;
        TEST_ASSERT(ucn_node_register_link(&node, &old_link) == UCN_OK);
        TEST_ASSERT(ucn_node_register_link(&node, &new_link) == UCN_OK);

        node.routes[0].valid = true;
        node.routes[0].route_origin = UINT32_C(1);
        node.routes[0].destination = UINT32_C(9);
        node.routes[0].egress_link = &old_link;
        node.routes[0].expires_at_ms = 5000U;
        node.routes[0].route_cost = 100U;
        node.routes[0].hop_count = 1U;
        node.routes[0].route_epoch = 5U;
        aodv_set_candidate(&node.candidates[0], UINT32_C(1), UINT32_C(9),
                           candidate_id, &old_link);
        node.candidates[0].originated_here = true;
        node.candidates[0].path_snapshot_frozen = true;
        node.candidates[0].expires_at_ms = 5000U;
        node.candidates[0].route_cost = 20U;
        node.candidates[0].hop_count = 2U;
        node.candidates[0].probes_sent = UCN_PATH_PROBE_REQUIRED_ACKS;
        node.candidates[0].probes_acked = UCN_PATH_PROBE_REQUIRED_ACKS;

        if (phase == 0U) {
            node.candidates[0].activation_sent = true;
            node.candidates[0].activation_attempts = 1U;
            node.candidates[0].route_epoch = 10U;
            node.candidates[0].activation_ack_deadline_ms = 1000U;
            node.candidates[0].next_activation_retry_at_ms = 250U;
        } else if (phase == 2U) {
            old_context.forced_result = UCN_ERR_NO_SPACE;
            TEST_ASSERT(ucn_node_step(&node, 0U) == UCN_ERR_NO_SPACE);
            old_context.forced_result = UCN_OK;
            TEST_ASSERT(node.candidates[0].activation_attempts == 1U &&
                        !node.candidates[0].activation_sent &&
                        node.candidates[0].route_epoch != 0U);
        }
        frozen_epoch = node.candidates[0].route_epoch;
        candidate_before = node.candidates[0];
        (void)memcpy(routes_before, node.routes, sizeof(routes_before));

        TEST_ASSERT(aodv_encode_candidate_route_reply(
                        UINT32_C(9), UINT32_C(1),
                        (uint32_t)(20U + phase), 91U, candidate_id, 1U, 0U,
                        encoded, &encoded_length) == 0);
        TEST_ASSERT(ucn_node_receive(&node, &new_link, encoded,
                                     encoded_length) == UCN_ERR_STATE);
        TEST_ASSERT(memcmp(&candidate_before, &node.candidates[0],
                           sizeof(candidate_before)) == 0);
        TEST_ASSERT(memcmp(routes_before, node.routes,
                           sizeof(routes_before)) == 0);

        if (phase == 0U) {
            TEST_ASSERT(aodv_encode_path_activate_ack(
                            UINT32_C(9), UINT32_C(1), 30U, 91U,
                            candidate_id, frozen_epoch, encoded,
                            &encoded_length) == 0);
            TEST_ASSERT(ucn_node_receive(&node, &old_link, encoded,
                                         encoded_length) == UCN_OK);
            installed = aodv_find_route(&node, UINT32_C(1), UINT32_C(9));
            TEST_ASSERT(installed != NULL &&
                        installed->egress_link == &old_link &&
                        installed->route_epoch == frozen_epoch);
        } else if (phase == 1U) {
            TEST_ASSERT(ucn_node_step(&node, 0U) == UCN_OK);
            TEST_ASSERT(old_context.path_activate_count == 1U &&
                        new_context.path_activate_count == 0U);
        } else {
            TEST_ASSERT(ucn_node_step(
                            &node, UCN_PATH_ACTIVATE_RETRY_INTERVAL_MS) ==
                        UCN_OK);
            TEST_ASSERT(old_context.path_activate_count == 2U &&
                        new_context.path_activate_count == 0U &&
                        node.candidates[0].route_epoch == frozen_epoch);
        }
    }
    return 0;
}

/*
 * EN: Proves every hop freezes the exact forward/reverse Candidate before a
 *     Probe/ACK enters an external Link callback, and Activate cannot consume
 *     an unproved Candidate.
 * 中文：验证每一跳都会在 Probe/ACK 进入外部 Link 回调前冻结精确的正反向
 *       Candidate，且 Activate 不能消费未经证明的 Candidate。
 */
static int test_candidate_path_snapshot_is_frozen_at_every_hop(void)
{
    bool link_up = true;
    bool drop_frames = true;
    const uint32_t candidate_id = UINT32_C(0x10203040);
    ucn_node_t relay, target;
    ucn_link_t relay_ingress, relay_old, relay_new, target_ingress;
    aodv_link_context_t ingress_context, old_context, new_context;
    aodv_link_context_t target_context;
    ucn_candidate_route_t candidate_before;
    ucn_route_entry_t routes_before[UCN_MAX_ROUTES];
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    const ucn_route_entry_t *installed;

    (void)memset(&relay, 0, sizeof(relay));
    (void)memset(&target, 0, sizeof(target));
    (void)memset(&relay_ingress, 0, sizeof(relay_ingress));
    (void)memset(&relay_old, 0, sizeof(relay_old));
    (void)memset(&relay_new, 0, sizeof(relay_new));
    (void)memset(&target_ingress, 0, sizeof(target_ingress));
    (void)memset(&ingress_context, 0, sizeof(ingress_context));
    (void)memset(&old_context, 0, sizeof(old_context));
    (void)memset(&new_context, 0, sizeof(new_context));
    (void)memset(&target_context, 0, sizeof(target_context));
    TEST_ASSERT(aodv_init_node(&relay, UINT32_C(2)) == 0);
    TEST_ASSERT(aodv_init_node(&target, UINT32_C(9)) == 0);

#define AODV_PROOF_LINK(link_, context_, id_, peer_) \
    do { \
        (link_).ops = &AODV_LINK_OPS; \
        (link_).context = &(context_); \
        (link_).link_id = (id_); \
        (link_).mtu = UCN_MAX_FRAME_BYTES; \
        (link_).peer_node_id = (peer_); \
        (context_).is_up = &link_up; \
        (context_).drop_frames = &drop_frames; \
    } while (0)
    AODV_PROOF_LINK(relay_ingress, ingress_context, 181U, UINT32_C(1));
    AODV_PROOF_LINK(relay_old, old_context, 182U, UINT32_C(9));
    AODV_PROOF_LINK(relay_new, new_context, 183U, UINT32_C(9));
    AODV_PROOF_LINK(target_ingress, target_context, 184U, UINT32_C(1));
#undef AODV_PROOF_LINK
    new_context.route_cost_valid = true;
    new_context.route_cost = 1U;
    TEST_ASSERT(ucn_node_register_link(&relay, &relay_ingress) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&relay, &relay_old) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&relay, &relay_new) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&target, &target_ingress) == UCN_OK);

    aodv_set_candidate(&relay.candidates[0], UINT32_C(1), UINT32_C(9),
                       candidate_id, &relay_old);
    relay.candidates[0].route_cost = 20U;
    relay.candidates[0].hop_count = 2U;
    aodv_set_candidate(&relay.candidates[1], UINT32_C(9), UINT32_C(1),
                       candidate_id, &relay_ingress);
    old_context.expected_frozen_candidate = &relay.candidates[0];

    TEST_ASSERT(aodv_encode_path_probe(
                    UCN_MSG_PATH_PROBE, UINT32_C(1), UINT32_C(9), 1U, 91U,
                    candidate_id, 1U, 0U, encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_ingress, encoded,
                                 encoded_length) == UCN_OK);
    TEST_ASSERT(relay.candidates[0].path_snapshot_frozen &&
                old_context.observed_frozen_before_send &&
                old_context.send_count == 1U);

    candidate_before = relay.candidates[0];
    (void)memcpy(routes_before, relay.routes, sizeof(routes_before));
    TEST_ASSERT(aodv_encode_candidate_route_reply(
                    UINT32_C(9), UINT32_C(1), 2U, 91U, candidate_id, 1U, 0U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_new, encoded,
                                 encoded_length) == UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&candidate_before, &relay.candidates[0],
                       sizeof(candidate_before)) == 0);
    TEST_ASSERT(memcmp(routes_before, relay.routes, sizeof(routes_before)) == 0);

    ingress_context.expected_frozen_candidate = &relay.candidates[1];
    TEST_ASSERT(aodv_encode_path_probe(
                    UCN_MSG_PATH_PROBE_ACK, UINT32_C(9), UINT32_C(1), 3U, 91U,
                    candidate_id, 1U, 0U, encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_old, encoded,
                                 encoded_length) == UCN_OK);
    TEST_ASSERT(relay.candidates[1].path_snapshot_frozen &&
                ingress_context.observed_frozen_before_send);

    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(9), 4U, 91U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_ingress, encoded,
                                 encoded_length) == UCN_OK);
    installed = aodv_find_route(&relay, UINT32_C(1), UINT32_C(9));
    TEST_ASSERT(installed != NULL && installed->egress_link == &relay_old &&
                installed->route_epoch == 10U);

    /* A target without an observed Probe must not accept Activate. */
    aodv_set_candidate(&target.candidates[0], UINT32_C(9), UINT32_C(1),
                       candidate_id, &target_ingress);
    candidate_before = target.candidates[0];
    (void)memcpy(routes_before, target.routes, sizeof(routes_before));
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(9), 5U, 92U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&target, &target_ingress, encoded,
                                 encoded_length) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(memcmp(&candidate_before, &target.candidates[0],
                       sizeof(candidate_before)) == 0);
    TEST_ASSERT(memcmp(routes_before, target.routes, sizeof(routes_before)) == 0);

    target_context.expected_frozen_candidate = &target.candidates[0];
    TEST_ASSERT(aodv_encode_path_probe(
                    UCN_MSG_PATH_PROBE, UINT32_C(1), UINT32_C(9), 6U, 92U,
                    candidate_id, 1U, 10U, encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&target, &target_ingress, encoded,
                                 encoded_length) == UCN_OK);
    TEST_ASSERT(target.candidates[0].path_snapshot_frozen &&
                target_context.observed_frozen_before_send);
    TEST_ASSERT(aodv_encode_path_activate(
                    UINT32_C(1), UINT32_C(9), 7U, 92U, candidate_id, 10U,
                    encoded, &encoded_length) == 0);
    TEST_ASSERT(ucn_node_receive(&target, &target_ingress, encoded,
                                 encoded_length) == UCN_OK);
    installed = aodv_find_route(&target, UINT32_C(9), UINT32_C(1));
    TEST_ASSERT(installed != NULL && installed->egress_link == &target_ingress &&
                installed->route_epoch == 10U);
    return 0;
}

/* Reusing an expired Route slot must not import another Origin's old epoch as
 * the new domain's Previous grace value.  That would let a stale foreign
 * epoch pass destination validation until the grace deadline. */
static int test_candidate_expired_foreign_slot_has_no_previous(void)
{
    bool link_up = true;
    bool drop_frames = true;
    ucn_node_t target;
    ucn_link_t ingress;
    aodv_link_context_t context;
    const uint8_t activate_payload[6U] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x00U, 0x0AU
    };
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_frame_t activate;
    const ucn_route_entry_t *installed;

    (void)memset(&target, 0, sizeof(target));
    (void)memset(&ingress, 0, sizeof(ingress));
    (void)memset(&context, 0, sizeof(context));
    (void)memset(&activate, 0, sizeof(activate));
    TEST_ASSERT(aodv_init_node(&target, UINT32_C(88)) == 0);
    target.now_ms = 100U;
    ingress.ops = &AODV_LINK_OPS;
    ingress.context = &context;
    ingress.link_id = 123U;
    ingress.mtu = UCN_MAX_FRAME_BYTES;
    ingress.peer_node_id = UINT32_C(77);
    context.is_up = &link_up;
    context.drop_frames = &drop_frames;
    TEST_ASSERT(ucn_node_register_link(&target, &ingress) == UCN_OK);

    target.routes[0].valid = true;
    target.routes[0].route_origin = UINT32_C(66);
    target.routes[0].destination = UINT32_C(55);
    target.routes[0].egress_link = &ingress;
    target.routes[0].route_epoch = 9U;
    target.routes[0].expires_at_ms = 99U;
    target.candidates[0].valid = true;
    target.candidates[0].wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    target.candidates[0].route_origin = UINT32_C(88);
    target.candidates[0].destination = UINT32_C(77);
    target.candidates[0].candidate_id = UINT32_C(0x01020304);
    target.candidates[0].egress_link = &ingress;
    target.candidates[0].path_snapshot_frozen = true;
    target.candidates[0].expires_at_ms = 1000U;
    target.candidates[0].route_cost = 1U;
    target.candidates[0].hop_count = 1U;

    activate.message_type = UCN_MSG_PATH_ACTIVATE;
    activate.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    activate.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    activate.hop_limit = 1U;
    activate.network_id = UINT32_C(42);
    activate.source = UINT32_C(77);
    activate.destination = UINT32_C(88);
    activate.sequence = 1U;
    activate.session_id = 1U;
    activate.payload = activate_payload;
    activate.payload_length = (uint16_t)sizeof(activate_payload);
    TEST_ASSERT(ucn_frame_encode(&activate, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&target, &ingress, encoded, encoded_length) ==
                UCN_OK);
    installed = aodv_find_route(&target, UINT32_C(88), UINT32_C(77));
    TEST_ASSERT(installed != NULL && installed->route_epoch == 10U);
    TEST_ASSERT(!installed->previous_valid &&
                installed->previous_route_epoch == 0U &&
                installed->previous_egress_link == NULL);
    return 0;
}
#endif

static int test_route_instance_table_full_is_fail_closed(void)
{
    bool link_up = true;
    bool drop_frames = true;
    ucn_node_t origin;
    ucn_node_t relay;
    ucn_link_t origin_link;
    ucn_link_t relay_ingress;
    aodv_link_context_t origin_context;
    aodv_link_context_t relay_context;
    ucn_route_entry_t before[UCN_MAX_ROUTES];
    size_t index;

    (void)memset(&origin, 0, sizeof(origin));
    (void)memset(&relay, 0, sizeof(relay));
    (void)memset(&origin_link, 0, sizeof(origin_link));
    (void)memset(&relay_ingress, 0, sizeof(relay_ingress));
    (void)memset(&origin_context, 0, sizeof(origin_context));
    (void)memset(&relay_context, 0, sizeof(relay_context));
    TEST_ASSERT(aodv_init_node(&origin, UINT32_C(1)) == 0);
    TEST_ASSERT(aodv_init_node(&relay, UINT32_C(2)) == 0);
    origin_link.ops = &AODV_LINK_OPS;
    origin_link.context = &origin_context;
    origin_link.link_id = 121U;
    origin_link.mtu = UCN_MAX_FRAME_BYTES;
    origin_link.peer_node_id = UINT32_C(2);
    origin_context.is_up = &link_up;
    origin_context.drop_frames = &drop_frames;
    relay_ingress.ops = &AODV_LINK_OPS;
    relay_ingress.context = &relay_context;
    relay_ingress.link_id = 122U;
    relay_ingress.mtu = UCN_MAX_FRAME_BYTES;
    relay_ingress.peer_node_id = UINT32_C(1);
    relay_context.is_up = &link_up;
    relay_context.drop_frames = &drop_frames;
    TEST_ASSERT(ucn_node_register_link(&origin, &origin_link) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&relay, &relay_ingress) == UCN_OK);
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        TEST_ASSERT(ucn_node_add_route(&relay,
                                      (ucn_node_id_t)(20U + index),
                                      &relay_ingress) == UCN_OK);
    }
    (void)memcpy(before, relay.routes, sizeof(before));
    TEST_ASSERT(ucn_node_discover_route(&origin, UINT32_C(99), 1U) == UCN_OK);
    TEST_ASSERT(origin_context.last_route_request_length != 0U);
    TEST_ASSERT(ucn_node_receive(&relay, &relay_ingress,
                                 origin_context.last_route_request_frame,
                                 origin_context.last_route_request_length) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(memcmp(before, relay.routes, sizeof(before)) == 0);
    TEST_ASSERT(relay.stats.route_instance_table_full == 1U);
    TEST_ASSERT(ucn_node_copy_route_summaries(&relay, NULL, 0U) ==
                UCN_MAX_ROUTES);
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
    TEST_ASSERT(test_multi_origin_route_epoch_isolation() == 0);
    TEST_ASSERT(test_static_route_accepts_dynamic_epoch() == 0);
#if UCN_FEATURE_CANDIDATE_ROUTING
    TEST_ASSERT(test_candidate_activation_is_atomic_and_idempotent() == 0);
    TEST_ASSERT(test_path_activate_ack_is_exactly_bound() == 0);
    TEST_ASSERT(test_path_activate_ack_retry_is_bounded() == 0);
    TEST_ASSERT(test_candidate_path_is_frozen_after_transaction_start() == 0);
    TEST_ASSERT(test_candidate_path_snapshot_is_frozen_at_every_hop() == 0);
    TEST_ASSERT(test_candidate_expired_foreign_slot_has_no_previous() == 0);
#endif
    TEST_ASSERT(test_route_instance_table_full_is_fail_closed() == 0);

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
