#include <string.h>

#include "test_support.h"
#include "ucn/ucn_service_bridge.h"

enum {
    BRIDGE_SERVICE_SENSOR = 1,
    BRIDGE_SERVICE_CONTROL = 3,
    BRIDGE_SERVICE_ACTUATOR = 4
};

#define BRIDGE_Q0_MASK UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q0_CRITICAL)
#define BRIDGE_Q1_MASK UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q1_REALTIME)

static const ucn_service_binding_t BRIDGE_BINDINGS[] = {
    { 0x40U, BRIDGE_SERVICE_CONTROL, 24U, BRIDGE_Q1_MASK,
      UCN_SERVICE_DELIVERY_Q1_LATEST,
      UCN_SERVICE_SOURCE_MASK(BRIDGE_SERVICE_SENSOR), true, true },
    { 0x60U, BRIDGE_SERVICE_ACTUATOR, 16U, BRIDGE_Q0_MASK,
      UCN_SERVICE_DELIVERY_Q0_FIFO,
      UCN_SERVICE_SOURCE_MASK(BRIDGE_SERVICE_CONTROL), true, true }
};

typedef struct bridge_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
} bridge_link_context_t;

typedef struct bridge_generic_rx_state {
    uint32_t count;
} bridge_generic_rx_state_t;

typedef struct bridge_hook_state {
    uint32_t lock_count;
    uint32_t unlock_count;
    uint32_t observer_count;
    ucn_endpoint_t last_endpoint;
    ucn_result_t last_result;
} bridge_hook_state_t;

static ucn_result_t bridge_link_send(ucn_link_t *link,
                                     const uint8_t *frame,
                                     size_t length)
{
    bridge_link_context_t *context = (bridge_link_context_t *)link->context;

    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t bridge_link_status(const ucn_link_t *link,
                                       ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t BRIDGE_LINK_OPS = {
    NULL, bridge_link_send, NULL, bridge_link_status, NULL, NULL
};

static int bridge_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    const ucn_config_t config = {
        UINT32_C(0x4E435425), id, 4U
    };

    (void)memset(node, 0, sizeof(*node));
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static int bridge_init_router(ucn_service_router_t *router, ucn_node_id_t local_node_id)
{
    const ucn_service_router_config_t config = {
        local_node_id, BRIDGE_BINDINGS,
        (uint8_t)(sizeof(BRIDGE_BINDINGS) / sizeof(BRIDGE_BINDINGS[0]))
    };

    (void)memset(router, 0, sizeof(*router));
    return ucn_service_router_init(router, &config) == UCN_OK ? 0 : 1;
}

static void bridge_generic_rx(void *context, const ucn_frame_t *frame)
{
    bridge_generic_rx_state_t *state = (bridge_generic_rx_state_t *)context;

    (void)frame;
    state->count++;
}

static void bridge_foreign_endpoint_rx(void *context, const ucn_frame_t *frame)
{
    (void)context;
    (void)frame;
}

static void bridge_hook_lock(void *context)
{
    bridge_hook_state_t *state = (bridge_hook_state_t *)context;

    state->lock_count++;
}

static void bridge_hook_unlock(void *context)
{
    bridge_hook_state_t *state = (bridge_hook_state_t *)context;

    state->unlock_count++;
}

static void bridge_hook_observer(void *context,
                                 const ucn_frame_t *frame,
                                 ucn_result_t result)
{
    bridge_hook_state_t *state = (bridge_hook_state_t *)context;

    state->observer_count++;
    state->last_endpoint = (ucn_endpoint_t)frame->message_type;
    state->last_result = result;
}

static int test_service_bridge_validation(void)
{
    ucn_node_t node_a, node_b;
    ucn_service_router_t router_a, router_b;
    ucn_service_protocol_bridge_t bridge;
    uint8_t payload = 1U;
    uint8_t processed = 9U;
    ucn_service_bridge_inbound_hooks_t hooks;
    const ucn_service_bridge_stats_t *stats;

    TEST_ASSERT(bridge_init_node(&node_a, UINT32_C(1)) == 0);
    TEST_ASSERT(bridge_init_node(&node_b, UINT32_C(2)) == 0);
    TEST_ASSERT(bridge_init_router(&router_a, UINT32_C(1)) == 0);
    TEST_ASSERT(bridge_init_router(&router_b, UINT32_C(2)) == 0);
    (void)memset(&bridge, 0, sizeof(bridge));
    TEST_ASSERT(ucn_service_protocol_bridge_init(NULL, &router_a, &node_a) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge, &router_a, &node_b) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge, &router_a, &node_a) == UCN_OK);
    (void)memset(&hooks, 0, sizeof(hooks));
    hooks.lock = bridge_hook_lock;
    TEST_ASSERT(ucn_service_protocol_bridge_set_inbound_hooks(&bridge, &hooks) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_service_protocol_bridge_set_inbound_hooks(&bridge, NULL) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step(&bridge, 1U, &processed) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(processed == 0U);

    TEST_ASSERT(ucn_node_set_endpoint_handler(&node_a, 0x40U,
                                              bridge_foreign_endpoint_rx, NULL) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge) ==
                UCN_ERR_CONFIG);

    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge, &router_b, &node_b) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router_b, UINT32_C(99), BRIDGE_SERVICE_CONTROL, 0x60U,
                                 UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step(&bridge, 1U, &processed) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(processed == 1U);
    stats = ucn_service_protocol_bridge_get_stats(&bridge);
    TEST_ASSERT(stats != NULL && stats->remote_tx_attempted == 1U &&
                stats->remote_tx_accepted == 0U && stats->remote_tx_failed == 1U &&
                stats->last_tx_result == UCN_ERR_NOT_FOUND);
    return 0;
}

static int test_service_bridge_three_node_delivery(void)
{
    uint8_t q1_value = 0x41U;
    uint8_t q1_rejected_value = 0x42U;
    uint8_t q0_value = 0x60U;
    uint8_t processed;
    ucn_node_t node_a, node_b, node_c;
    ucn_link_t ab, ba, bc, cb;
    bridge_link_context_t cab, cba, cbc, ccb;
    bridge_generic_rx_state_t relay_rx;
    bridge_hook_state_t hook_state;
    ucn_service_router_t router_a, router_c;
    ucn_service_protocol_bridge_t bridge_a, bridge_c;
    ucn_service_message_t message;
    const ucn_service_bridge_stats_t *a_stats;
    const ucn_service_bridge_stats_t *c_stats;

    TEST_ASSERT(bridge_init_node(&node_a, UINT32_C(1)) == 0);
    TEST_ASSERT(bridge_init_node(&node_b, UINT32_C(2)) == 0);
    TEST_ASSERT(bridge_init_node(&node_c, UINT32_C(3)) == 0);
    TEST_ASSERT(bridge_init_router(&router_a, UINT32_C(1)) == 0);
    TEST_ASSERT(bridge_init_router(&router_c, UINT32_C(3)) == 0);
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&bc, 0, sizeof(bc)); (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&cab, 0, sizeof(cab)); (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&cbc, 0, sizeof(cbc)); (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&relay_rx, 0, sizeof(relay_rx));
    (void)memset(&hook_state, 0, sizeof(hook_state));
    (void)memset(&bridge_a, 0, sizeof(bridge_a));
    (void)memset(&bridge_c, 0, sizeof(bridge_c));

    ab.ops = &BRIDGE_LINK_OPS; ab.context = &cab; ab.link_id = 1U;
    ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
    ba.ops = &BRIDGE_LINK_OPS; ba.context = &cba; ba.link_id = 2U;
    ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
    bc.ops = &BRIDGE_LINK_OPS; bc.context = &cbc; bc.link_id = 3U;
    bc.mtu = UCN_MAX_FRAME_BYTES; bc.peer_node_id = UINT32_C(3);
    cb.ops = &BRIDGE_LINK_OPS; cb.context = &ccb; cb.link_id = 4U;
    cb.mtu = UCN_MAX_FRAME_BYTES; cb.peer_node_id = UINT32_C(2);
    cab.peer = &node_b; cab.peer_ingress = &ba;
    cba.peer = &node_a; cba.peer_ingress = &ab;
    cbc.peer = &node_c; cbc.peer_ingress = &cb;
    ccb.peer = &node_b; ccb.peer_ingress = &bc;
    TEST_ASSERT(ucn_node_register_link(&node_a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&node_a, UINT32_C(3), &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_add_route(&node_b, UINT32_C(3), &bc) == UCN_OK);
    ucn_node_set_rx_handler(&node_b, bridge_generic_rx, &relay_rx);

    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge_a, &router_a, &node_a) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge_c, &router_c, &node_c) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge_a) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge_c) == UCN_OK);
    {
        const ucn_service_bridge_inbound_hooks_t hooks = {
            &hook_state, bridge_hook_lock, bridge_hook_unlock, bridge_hook_observer
        };

        TEST_ASSERT(ucn_service_protocol_bridge_set_inbound_hooks(&bridge_a, &hooks) ==
                    UCN_OK);
        TEST_ASSERT(ucn_service_protocol_bridge_set_inbound_hooks(&bridge_c, &hooks) ==
                    UCN_OK);
    }
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &q1_value, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                                 UCN_TRAFFIC_Q0_CRITICAL, &q0_value, 1U) == UCN_OK);

    TEST_ASSERT(ucn_service_protocol_bridge_step(&bridge_a, 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U);
    TEST_ASSERT(ucn_service_inbox_take(&router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U,
                                       &message) == UCN_OK);
    TEST_ASSERT(message.source_node_id == UINT32_C(1) &&
                message.source_service_id == UCN_SERVICE_ID_NONE &&
                message.payload[0] == q0_value);
    TEST_ASSERT(ucn_service_inbox_take(&router_c, BRIDGE_SERVICE_CONTROL, 0x40U,
                                       &message) == UCN_ERR_NOT_FOUND);

    TEST_ASSERT(ucn_service_protocol_bridge_step(&bridge_a, 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U);
    TEST_ASSERT(ucn_service_inbox_take(&router_c, BRIDGE_SERVICE_CONTROL, 0x40U,
                                       &message) == UCN_OK);
    TEST_ASSERT(message.source_node_id == UINT32_C(1) &&
                message.source_service_id == UCN_SERVICE_ID_NONE &&
                message.payload[0] == q1_value);
    /* Endpoint handlers are void in the existing Core.  A target Service
     * rejection therefore remains local/observable in Bridge statistics and
     * is not misreported as a transport ACK to the source. */
    TEST_ASSERT(ucn_service_set_ready(&router_c, 0x40U, false) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &q1_rejected_value, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step(&bridge_a, 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U);
    TEST_ASSERT(ucn_service_inbox_take(&router_c, BRIDGE_SERVICE_CONTROL, 0x40U,
                                       &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_service_set_ready(&router_c, 0x40U, true) == UCN_OK);
    TEST_ASSERT(relay_rx.count == 0U);
    a_stats = ucn_service_protocol_bridge_get_stats(&bridge_a);
    c_stats = ucn_service_protocol_bridge_get_stats(&bridge_c);
    TEST_ASSERT(a_stats != NULL && a_stats->remote_tx_accepted == 3U &&
                a_stats->remote_tx_failed == 0U && a_stats->last_tx_result == UCN_OK);
    TEST_ASSERT(c_stats != NULL && c_stats->inbound_delivered == 2U &&
                c_stats->inbound_rejected == 1U &&
                c_stats->last_inbound_result == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(hook_state.lock_count == 6U && hook_state.unlock_count == 6U &&
                hook_state.observer_count == 3U && hook_state.last_endpoint == 0x40U &&
                hook_state.last_result == UCN_ERR_NOT_FOUND);
    return 0;
}

int test_service_bridge(void)
{
    return test_service_bridge_validation() | test_service_bridge_three_node_delivery();
}
