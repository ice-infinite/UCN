#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct metrics_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool is_up;
    bool metrics_valid;
    uint16_t route_cost;
    uint32_t send_count;
    uint8_t last_frame[UCN_MAX_FRAME_BYTES];
    size_t last_frame_length;
} metrics_link_context_t;

typedef struct metrics_receive_state {
    uint32_t count;
    uint8_t payload;
} metrics_receive_state_t;

static ucn_result_t metrics_link_send(ucn_link_t *link,
                                      const uint8_t *frame,
                                      size_t length)
{
    metrics_link_context_t *context = (metrics_link_context_t *)link->context;

    context->send_count++;
    if (length <= sizeof(context->last_frame)) {
        (void)memcpy(context->last_frame, frame, length);
        context->last_frame_length = length;
    }
    if (context->peer == NULL) {
        return UCN_OK;
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t metrics_link_status(const ucn_link_t *link, ucn_link_status_t *status)
{
    const metrics_link_context_t *context = (const metrics_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static ucn_result_t metrics_link_get_metrics(const ucn_link_t *link,
                                             ucn_link_metrics_t *metrics)
{
    const metrics_link_context_t *context = (const metrics_link_context_t *)link->context;

    metrics->route_cost_valid = context->metrics_valid;
    metrics->route_cost = context->route_cost;
    return UCN_OK;
}

static const ucn_link_ops_t METRICS_LINK_OPS = {
    NULL, metrics_link_send, NULL, metrics_link_status, NULL, metrics_link_get_metrics
};

static int metrics_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x31415926);
    config.node_id = id;
    config.default_hop_limit = 4U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void metrics_rx(void *context, const ucn_frame_t *frame)
{
    metrics_receive_state_t *state = (metrics_receive_state_t *)context;

    state->count++;
    state->payload = frame->payload[0];
}

static void metrics_setup_link(ucn_link_t *link,
                               metrics_link_context_t *context,
                               uint8_t link_id,
                               ucn_node_id_t peer_node_id)
{
    link->ops = &METRICS_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
}

static uint32_t metrics_wire_maximum(uint8_t width)
{
    return width >= 4U ? UINT32_MAX :
           (UINT32_C(1) << ((uint32_t)width * 8U)) - UINT32_C(1);
}

static void metrics_write_uint_be(uint8_t *output, uint8_t width,
                                  uint32_t value)
{
    uint8_t index;

    for (index = 0U; index < width; ++index) {
        output[index] = (uint8_t)(value >>
            ((uint32_t)(width - index - 1U) * 8U));
    }
}

static uint32_t metrics_read_uint_be(const uint8_t *input, uint8_t width)
{
    uint32_t value = 0U;
    uint8_t index;

    for (index = 0U; index < width; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

static ucn_result_t metrics_inject_route_request(
    ucn_node_t *relay,
    ucn_link_t *ingress,
    ucn_wire_profile_t profile,
    uint32_t request_id,
    uint32_t route_cost,
    uint8_t hop_count,
    uint8_t frame_hop_limit)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);
    uint8_t payload[14] = { 0U };
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    size_t cost_offset;
    size_t payload_length;
    ucn_frame_t frame;
    ucn_result_t result;

    if (descriptor == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    cost_offset = (size_t)descriptor->address_bytes + 4U;
    payload_length = cost_offset + descriptor->route_cost_bytes + 2U;
    metrics_write_uint_be(payload, descriptor->address_bytes, UINT32_C(3));
    metrics_write_uint_be(payload + descriptor->address_bytes, 4U, request_id);
    metrics_write_uint_be(payload + cost_offset, descriptor->route_cost_bytes,
                          route_cost);
    payload[cost_offset + descriptor->route_cost_bytes] = hop_count;
    payload[cost_offset + descriptor->route_cost_bytes + 1U] = 0U;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_ROUTE_REQ;
    frame.wire_profile = profile;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = frame_hop_limit;
    frame.network_id = UINT32_C(0x2A);
    frame.source = UINT32_C(1);
    frame.destination = UCN_NODE_BROADCAST;
    frame.sequence = request_id;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;
    result = ucn_frame_encode(&frame, encoded, sizeof(encoded), &encoded_length);
    return result == UCN_OK ?
               ucn_node_receive(relay, ingress, encoded, encoded_length) :
               result;
}

static int test_route_cost_profile_boundaries(void)
{
    const ucn_wire_profile_t profiles[] = {
        UCN_WIRE_PROFILE_W0_LOCAL,
        UCN_WIRE_PROFILE_W1_EDGE,
        UCN_WIRE_PROFILE_W2_MESH,
        UCN_WIRE_PROFILE_W3_BACKBONE
    };
    size_t profile_index;

    for (profile_index = 0U;
         profile_index < sizeof(profiles) / sizeof(profiles[0]);
         ++profile_index) {
        const ucn_wire_profile_t profile = profiles[profile_index];
        const ucn_wire_profile_descriptor_t *descriptor =
            ucn_wire_profile_get_descriptor(profile);
        const uint32_t wire_maximum =
            metrics_wire_maximum(descriptor->route_cost_bytes);
        const uint32_t maximum_legal_path_cost =
            (uint32_t)descriptor->max_hops *
            (uint32_t)UCN_LINK_ROUTE_COST_MAX;
        const size_t cost_offset = (size_t)descriptor->address_bytes + 4U;
        const size_t expected_payload_length =
            cost_offset + descriptor->route_cost_bytes + 2U;
        ucn_node_t relay;
        ucn_link_t ingress, egress;
        metrics_link_context_t ingress_context, egress_context;
        ucn_config_t config;
        ucn_frame_t forwarded;
        uint32_t sends_before_overflow;

        (void)memset(&relay, 0, sizeof(relay));
        (void)memset(&ingress, 0, sizeof(ingress));
        (void)memset(&egress, 0, sizeof(egress));
        (void)memset(&ingress_context, 0, sizeof(ingress_context));
        (void)memset(&egress_context, 0, sizeof(egress_context));
        config.network_id = UINT32_C(42);
        config.node_id = UINT32_C(2);
        config.default_hop_limit = 4U;
        TEST_ASSERT(ucn_node_init(&relay, &config) == UCN_OK);
        TEST_ASSERT(ucn_node_set_wire_profiles(
                        &relay, profile, UCN_WIRE_PROFILE_W3_BACKBONE) == UCN_OK);
        ingress_context.is_up = true;
        ingress_context.metrics_valid = true;
        ingress_context.route_cost = 1U;
        egress_context.is_up = true;
        egress_context.metrics_valid = true;
        egress_context.route_cost = 1U;
        metrics_setup_link(&ingress, &ingress_context, 20U, UINT32_C(1));
        metrics_setup_link(&egress, &egress_context, 21U, UINT32_C(3));
        ingress.peer_wire_profile = profile;
        egress.peer_wire_profile = profile;
        TEST_ASSERT(ucn_node_register_link(&relay, &ingress) == UCN_OK);
        TEST_ASSERT(ucn_node_register_link(&relay, &egress) == UCN_OK);
        TEST_ASSERT(maximum_legal_path_cost < wire_maximum);

        TEST_ASSERT(metrics_inject_route_request(
                        &relay, &ingress, profile, UINT32_C(9), 0U, 2U, 4U) ==
                    UCN_ERR_TTL);
        TEST_ASSERT(relay.stats.hop_scope_rejected == 1U &&
                    !relay.routes[0].valid && !relay.rreq_cache[0].valid);
        if (profile == UCN_WIRE_PROFILE_W3_BACKBONE) {
            TEST_ASSERT(metrics_inject_route_request(
                            &relay, &ingress, profile, UINT32_C(8), 0U, 1U,
                            64U) == UCN_ERR_TTL);
            TEST_ASSERT(relay.stats.hop_scope_rejected == 2U &&
                        !relay.routes[0].valid && !relay.rreq_cache[0].valid);
        }

        egress_context.route_cost = UCN_LINK_ROUTE_COST_MAX;
        TEST_ASSERT(metrics_inject_route_request(
                        &relay, &ingress, profile, UINT32_C(10),
                        maximum_legal_path_cost -
                            (uint32_t)UCN_LINK_ROUTE_COST_MAX, 1U, 4U) == UCN_OK);
        TEST_ASSERT(ucn_frame_decode(egress_context.last_frame,
                                     egress_context.last_frame_length,
                                     &forwarded) == UCN_OK);
        TEST_ASSERT(metrics_read_uint_be(
                        forwarded.payload + cost_offset,
                        descriptor->route_cost_bytes) ==
                    maximum_legal_path_cost);
        egress_context.route_cost = 1U;

        TEST_ASSERT(metrics_inject_route_request(
                        &relay, &ingress, profile, UINT32_C(1),
                        wire_maximum - UINT32_C(2), 1U, 4U) == UCN_OK);
        TEST_ASSERT(ucn_frame_decode(egress_context.last_frame,
                                     egress_context.last_frame_length,
                                     &forwarded) == UCN_OK);
        TEST_ASSERT(forwarded.wire_profile == profile);
        TEST_ASSERT(forwarded.payload_length == expected_payload_length);
        TEST_ASSERT(metrics_read_uint_be(
                        forwarded.payload + cost_offset,
                        descriptor->route_cost_bytes) ==
                    wire_maximum - UINT32_C(1));

        TEST_ASSERT(metrics_inject_route_request(
                        &relay, &ingress, profile, UINT32_C(2),
                        wire_maximum, 1U, 4U) == UCN_OK);
        TEST_ASSERT(ucn_frame_decode(egress_context.last_frame,
                                     egress_context.last_frame_length,
                                     &forwarded) == UCN_OK);
        TEST_ASSERT(metrics_read_uint_be(
                        forwarded.payload + cost_offset,
                        descriptor->route_cost_bytes) == wire_maximum);

        sends_before_overflow = egress_context.send_count;
        TEST_ASSERT(metrics_inject_route_request(
                        &relay, &ingress, profile, UINT32_C(3),
                        wire_maximum - UINT32_C(1), 1U, 4U) ==
                    UCN_ERR_TOO_LARGE);
        TEST_ASSERT(egress_context.send_count == sends_before_overflow);
    }
    return 0;
}

int test_link_metrics(void)
{
    uint8_t payload = 0x5AU;
    ucn_node_t direct_node;
    ucn_link_t default_link, high_cost_link, low_cost_link;
    metrics_link_context_t default_context, high_context, low_context;
    ucn_node_t a, b, c, d;
    ucn_link_t ab, ba, ad, da, bc, cb, dc, cd;
    metrics_link_context_t cab, cba, cad, cda, cbc, ccb, cdc, ccd;
    metrics_receive_state_t received;
    const ucn_route_entry_t *a_to_c = NULL;
    const ucn_route_entry_t *d_to_c = NULL;
    size_t route_index;

    TEST_ASSERT(test_route_cost_profile_boundaries() == 0);

    (void)memset(&direct_node, 0, sizeof(direct_node));
    (void)memset(&default_link, 0, sizeof(default_link));
    (void)memset(&high_cost_link, 0, sizeof(high_cost_link));
    (void)memset(&low_cost_link, 0, sizeof(low_cost_link));
    (void)memset(&default_context, 0, sizeof(default_context));
    (void)memset(&high_context, 0, sizeof(high_context));
    (void)memset(&low_context, 0, sizeof(low_context));
    TEST_ASSERT(metrics_init_node(&direct_node, UINT32_C(10)) == 0);
    default_context.is_up = true;
    high_context.is_up = true; high_context.metrics_valid = true; high_context.route_cost = 2000U;
    low_context.is_up = true; low_context.metrics_valid = true; low_context.route_cost = 2U;
    metrics_setup_link(&default_link, &default_context, 1U, UINT32_C(11));
    metrics_setup_link(&high_cost_link, &high_context, 2U, UINT32_C(11));
    metrics_setup_link(&low_cost_link, &low_context, 3U, UINT32_C(11));
    TEST_ASSERT(ucn_node_register_link(&direct_node, &default_link) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&direct_node, &high_cost_link) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&direct_node, UINT32_C(11), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(default_context.send_count == 0U);
    TEST_ASSERT(high_context.send_count == 1U);
    TEST_ASSERT(low_context.send_count == 0U);
    TEST_ASSERT(ucn_node_register_link(&direct_node, &low_cost_link) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&direct_node, UINT32_C(11), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(low_context.send_count == 1U);
    default_context.metrics_valid = true;
    default_context.route_cost = 10U;
    TEST_ASSERT(ucn_node_send(&direct_node, UINT32_C(11), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(low_context.send_count == 2U);

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c)); (void)memset(&d, 0, sizeof(d));
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&ad, 0, sizeof(ad)); (void)memset(&da, 0, sizeof(da));
    (void)memset(&bc, 0, sizeof(bc)); (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&dc, 0, sizeof(dc)); (void)memset(&cd, 0, sizeof(cd));
    (void)memset(&cab, 0, sizeof(cab)); (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&cad, 0, sizeof(cad)); (void)memset(&cda, 0, sizeof(cda));
    (void)memset(&cbc, 0, sizeof(cbc)); (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&cdc, 0, sizeof(cdc)); (void)memset(&ccd, 0, sizeof(ccd));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(metrics_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(metrics_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(metrics_init_node(&c, UINT32_C(3)) == 0);
    TEST_ASSERT(metrics_init_node(&d, UINT32_C(4)) == 0);
    /* Both alternatives deliberately exceed the old 16-bit aggregate ceiling:
     * A-B-C costs 100000 while A-D-C costs 80000. */
    cab.is_up = true; cab.metrics_valid = true; cab.route_cost = 50000U;
    cba.is_up = true; cba.metrics_valid = true; cba.route_cost = 50000U;
    cad.is_up = true; cad.metrics_valid = true; cad.route_cost = 40000U;
    cda.is_up = true; cda.metrics_valid = true; cda.route_cost = 40000U;
    cbc.is_up = true; cbc.metrics_valid = true; cbc.route_cost = 50000U;
    ccb.is_up = true; ccb.metrics_valid = true; ccb.route_cost = 50000U;
    cdc.is_up = true; cdc.metrics_valid = true; cdc.route_cost = 40000U;
    ccd.is_up = true; ccd.metrics_valid = true; ccd.route_cost = 40000U;
    metrics_setup_link(&ab, &cab, 10U, UINT32_C(2));
    metrics_setup_link(&ba, &cba, 11U, UINT32_C(1));
    metrics_setup_link(&ad, &cad, 12U, UINT32_C(4));
    metrics_setup_link(&da, &cda, 13U, UINT32_C(1));
    metrics_setup_link(&bc, &cbc, 14U, UINT32_C(3));
    metrics_setup_link(&cb, &ccb, 15U, UINT32_C(2));
    metrics_setup_link(&dc, &cdc, 16U, UINT32_C(3));
    metrics_setup_link(&cd, &ccd, 17U, UINT32_C(4));
    cab.peer = &b; cab.peer_ingress = &ba; cba.peer = &a; cba.peer_ingress = &ab;
    cad.peer = &d; cad.peer_ingress = &da; cda.peer = &a; cda.peer_ingress = &ad;
    cbc.peer = &c; cbc.peer_ingress = &cb; ccb.peer = &b; ccb.peer_ingress = &bc;
    cdc.peer = &c; cdc.peer_ingress = &cd; ccd.peer = &d; ccd.peer_ingress = &dc;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&a, &ad) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cd) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&d, &da) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&d, &dc) == UCN_OK);
    ucn_node_set_rx_handler(&c, metrics_rx, &received);
    TEST_ASSERT(ucn_node_discover_route(&a, UINT32_C(3), 100U) == UCN_OK);
    TEST_ASSERT(!ucn_node_route_pending(&a, UINT32_C(3)));
    for (route_index = 0U; route_index < UCN_MAX_ROUTES; ++route_index) {
        if (a.routes[route_index].valid &&
            a.routes[route_index].destination == UINT32_C(3)) {
            a_to_c = &a.routes[route_index];
        }
        if (d.routes[route_index].valid &&
            d.routes[route_index].destination == UINT32_C(3)) {
            d_to_c = &d.routes[route_index];
        }
    }
    /* RREP Cost/Hop is target-rooted: every return hop extends the metric
     * advertised by its downstream neighbor.  This keeps a same-Epoch route
     * strictly decreasing toward the target instead of copying one total
     * origin-to-target value into every relay. */
    TEST_ASSERT(a_to_c != NULL &&
                a_to_c->route_cost == UINT32_C(80000) &&
                a_to_c->hop_count == 2U);
    TEST_ASSERT(d_to_c != NULL &&
                d_to_c->route_cost == UINT32_C(40000) &&
                d_to_c->hop_count == 1U);
    cab.send_count = 0U; cad.send_count = 0U;
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(3), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(cab.send_count == 0U);
    TEST_ASSERT(cad.send_count == 1U);
    TEST_ASSERT(received.count == 1U);
    TEST_ASSERT(received.payload == payload);
    return 0;
}
