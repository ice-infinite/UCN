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
      UCN_SERVICE_SOURCE_MASK(BRIDGE_SERVICE_SENSOR), true, true, false },
    { 0x60U, BRIDGE_SERVICE_ACTUATOR, 16U, BRIDGE_Q0_MASK,
      UCN_SERVICE_DELIVERY_Q0_FIFO,
      UCN_SERVICE_SOURCE_MASK(BRIDGE_SERVICE_CONTROL), true, true, false }
};

static const ucn_service_binding_t VALIDATED_BINDINGS[] = {
    { 0x40U, BRIDGE_SERVICE_CONTROL, 24U, BRIDGE_Q1_MASK,
      UCN_SERVICE_DELIVERY_Q1_LATEST,
      UCN_SERVICE_SOURCE_MASK(BRIDGE_SERVICE_SENSOR), true, true, false },
    { 0x60U, BRIDGE_SERVICE_ACTUATOR, 16U, BRIDGE_Q0_MASK,
      UCN_SERVICE_DELIVERY_Q0_FIFO,
      UCN_SERVICE_SOURCE_MASK(BRIDGE_SERVICE_CONTROL), true, true, true },
    { 0x61U, BRIDGE_SERVICE_ACTUATOR, 16U, BRIDGE_Q0_MASK,
      UCN_SERVICE_DELIVERY_Q0_FIFO,
      UCN_SERVICE_SOURCE_MASK(BRIDGE_SERVICE_CONTROL), true, true, true }
};

typedef struct bridge_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    ucn_result_t forced_result;
    uint32_t send_calls;
    uint8_t no_space_remaining;
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

typedef struct bridge_outbound_state {
    uint32_t count;
    uint32_t event_count;
    uint8_t payloads[8];
    ucn_result_t results[8];
    ucn_service_async_stage_t stages[8];
    ucn_service_bridge_outbound_outcome_t outcomes[8];
    ucn_result_t event_results[8];
} bridge_outbound_state_t;

typedef struct bridge_validator_state {
    ucn_service_bridge_replay_state_t replay;
    uint32_t calls;
    ucn_node_id_t last_source;
    ucn_session_id_t last_session;
    ucn_endpoint_t last_endpoint;
    uint32_t last_now_ms;
} bridge_validator_state_t;

static ucn_result_t bridge_link_send(ucn_link_t *link,
                                     const uint8_t *frame,
                                     size_t length)
{
    bridge_link_context_t *context = (bridge_link_context_t *)link->context;

    context->send_calls++;
    if (context->no_space_remaining != 0U) {
        context->no_space_remaining--;
        return UCN_ERR_NO_SPACE;
    }
    if (context->forced_result != UCN_OK) {
        return context->forced_result;
    }
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

static int bridge_init_router_with_bindings(
    ucn_service_router_t *router,
    ucn_node_id_t local_node_id,
    const ucn_service_binding_t *bindings,
    uint8_t binding_count)
{
    const ucn_service_router_config_t config = {
        local_node_id, bindings, binding_count
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

static void bridge_outbound_observer(void *context,
                                     const ucn_service_message_t *message,
                                     ucn_result_t result)
{
    bridge_outbound_state_t *state = (bridge_outbound_state_t *)context;
    const uint32_t index = state->count;

    if (index < 8U) {
        state->payloads[index] =
            message->payload_length == 0U ? 0U : message->payload[0];
        state->results[index] = result;
    }
    state->count++;
}

static void bridge_outbound_event_observer(
    void *context,
    const ucn_service_message_t *message,
    const ucn_service_bridge_outbound_event_t *event)
{
    bridge_outbound_state_t *state = (bridge_outbound_state_t *)context;
    uint32_t index;

    if (state == NULL || message == NULL || event == NULL) {
        return;
    }
    index = state->event_count;
    if (index < 8U) {
        state->stages[index] = event->stage;
        state->outcomes[index] = event->outcome;
        state->event_results[index] = event->result;
    }
    state->event_count++;
}

static ucn_result_t bridge_guard_validator(
    void *context,
    const ucn_frame_t *frame,
    ucn_node_id_t source_node_id,
    ucn_session_id_t source_session_id,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length,
    uint32_t now_ms)
{
    bridge_validator_state_t *state = (bridge_validator_state_t *)context;
    ucn_service_command_guard_t guard;
    ucn_result_t result;

    if (state == NULL || frame == NULL || payload == NULL ||
        source_node_id != frame->source || source_session_id != frame->session_id ||
        endpoint != (ucn_endpoint_t)frame->message_type || endpoint != 0x60U ||
        payload != frame->payload || payload_length != 13U) {
        return UCN_ERR_MALFORMED;
    }
    state->calls++;
    state->last_source = source_node_id;
    state->last_session = source_session_id;
    state->last_endpoint = endpoint;
    state->last_now_ms = now_ms;
    result = ucn_service_command_guard_decode(payload, payload_length, &guard);
    if (result != UCN_OK || guard.result_endpoint != 0x40U) {
        return result == UCN_OK ? UCN_ERR_MALFORMED : result;
    }
    result = ucn_service_command_guard_validate(&guard, now_ms, false, 0U);
    if (result != UCN_OK) {
        return result;
    }
    return ucn_service_bridge_replay_accept_command(
        &state->replay, source_node_id, source_session_id, endpoint,
        guard.command_id);
}

static ucn_result_t bridge_compact_validator(
    void *context,
    const ucn_frame_t *frame,
    ucn_node_id_t source_node_id,
    ucn_session_id_t source_session_id,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length,
    uint32_t now_ms)
{
    bridge_validator_state_t *state = (bridge_validator_state_t *)context;

    if (state == NULL || frame == NULL || payload == NULL || endpoint != 0x61U ||
        source_node_id != frame->source || source_session_id != frame->session_id ||
        payload != frame->payload || payload_length != 1U || payload[0] != 0xA5U) {
        return UCN_ERR_MALFORMED;
    }
    state->calls++;
    state->last_source = source_node_id;
    state->last_session = source_session_id;
    state->last_endpoint = endpoint;
    state->last_now_ms = now_ms;
    return ucn_service_bridge_replay_accept_command(
        &state->replay, source_node_id, source_session_id, endpoint,
        (uint32_t)payload[0]);
}

static int bridge_build_guard_payload(uint8_t payload[13],
                                      uint32_t command_id,
                                      uint32_t issued_at_ms,
                                      uint16_t valid_for_ms)
{
    const ucn_service_command_guard_t guard = {
        command_id, issued_at_ms, valid_for_ms, 0x40U, 0U
    };

    payload[12] = 0x5AU;
    return ucn_service_command_guard_encode(&guard, payload) == UCN_OK ? 0 : 1;
}

static int test_service_bridge_validation(void)
{
    ucn_node_t node_a, node_b;
    ucn_service_router_t router_a, router_b;
    ucn_service_protocol_bridge_t bridge;
    uint8_t payload = 1U;
    uint8_t processed = 9U;
    ucn_service_bridge_inbound_hooks_t hooks;
    ucn_service_bridge_q0_backpressure_policy_t policy;
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
    (void)memset(&policy, 0, sizeof(policy));
    TEST_ASSERT(ucn_service_protocol_bridge_set_q0_backpressure_policy(
                    &bridge, &policy) == UCN_ERR_CONFIG);
    policy.max_retries = 1U;
    policy.retry_interval_ms = 10U;
    policy.timeout_ms = 10U;
    TEST_ASSERT(ucn_service_protocol_bridge_set_q0_backpressure_policy(
                    &bridge, &policy) == UCN_ERR_CONFIG);
    policy.timeout_ms = 20U;
    TEST_ASSERT(ucn_service_protocol_bridge_set_q0_backpressure_policy(
                    &bridge, &policy) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_q0_backpressure_policy(
                    &bridge, NULL) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_observer(
                    &bridge, bridge_outbound_observer, NULL) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_observer(
                    &bridge, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_event_observer(
                    &bridge, bridge_outbound_event_observer, NULL) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_event_observer(
                    &bridge, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_event_observer(
                    NULL, NULL, NULL) == UCN_ERR_ARGUMENT);
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

static int test_service_bridge_q0_backpressure(void)
{
    uint8_t payload;
    uint8_t processed;
    ucn_node_t node_a, node_c;
    ucn_link_t ac, ca;
    bridge_link_context_t cac, cca;
    ucn_service_router_t router_a, router_c;
    ucn_service_protocol_bridge_t bridge_a, bridge_c;
    ucn_service_bridge_q0_backpressure_policy_t policy;
    bridge_outbound_state_t outbound;
    ucn_service_message_t message;
    const ucn_service_bridge_stats_t *stats;

    TEST_ASSERT(bridge_init_node(&node_a, UINT32_C(1)) == 0);
    TEST_ASSERT(bridge_init_node(&node_c, UINT32_C(3)) == 0);
    TEST_ASSERT(bridge_init_router(&router_a, UINT32_C(1)) == 0);
    TEST_ASSERT(bridge_init_router(&router_c, UINT32_C(3)) == 0);
    (void)memset(&ac, 0, sizeof(ac));
    (void)memset(&ca, 0, sizeof(ca));
    (void)memset(&cac, 0, sizeof(cac));
    (void)memset(&cca, 0, sizeof(cca));
    (void)memset(&bridge_a, 0, sizeof(bridge_a));
    (void)memset(&bridge_c, 0, sizeof(bridge_c));
    (void)memset(&outbound, 0, sizeof(outbound));

    ac.ops = &BRIDGE_LINK_OPS; ac.context = &cac; ac.link_id = 1U;
    ac.mtu = UCN_MAX_FRAME_BYTES; ac.peer_node_id = UINT32_C(3);
    ca.ops = &BRIDGE_LINK_OPS; ca.context = &cca; ca.link_id = 2U;
    ca.mtu = UCN_MAX_FRAME_BYTES; ca.peer_node_id = UINT32_C(1);
    cac.peer = &node_c; cac.peer_ingress = &ca;
    cca.peer = &node_a; cca.peer_ingress = &ac;
    TEST_ASSERT(ucn_node_register_link(&node_a, &ac) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_c, &ca) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge_a, &router_a, &node_a) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge_c, &router_c, &node_c) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge_a) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge_c) == UCN_OK);
    policy.max_retries = 2U;
    policy.retry_interval_ms = 5U;
    policy.timeout_ms = 50U;
    TEST_ASSERT(ucn_service_protocol_bridge_set_q0_backpressure_policy(
                    &bridge_a, &policy) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_observer(
                    &bridge_a, bridge_outbound_observer, &outbound) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_event_observer(
                    &bridge_a, bridge_outbound_event_observer, &outbound) == UCN_OK);

    /* Two transient failures retain the first Q0 in the fixed Pending slot.
     * The second Router Q0 cannot overtake it. */
    payload = 0xA1U;
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL,
                                 0x60U, UCN_TRAFFIC_Q0_CRITICAL,
                                 &payload, 1U) == UCN_OK);
    payload = 0xA2U;
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL,
                                 0x60U, UCN_TRAFFIC_Q0_CRITICAL,
                                 &payload, 1U) == UCN_OK);
    cac.no_space_remaining = 2U;
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 10U, 1U, &processed) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(processed == 1U && bridge_a.q0_pending.occupied &&
                router_a.remote_q0.count == 1U && outbound.count == 0U &&
                node_a.stats.tx_error_dropped == 0U);
    TEST_ASSERT(ucn_service_protocol_bridge_set_q0_backpressure_policy(
                    &bridge_a, NULL) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 12U, 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 0U && cac.send_calls == 1U &&
                router_a.remote_q0.count == 1U);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 15U, 1U, &processed) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(processed == 1U && outbound.count == 0U);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 20U, 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U && !bridge_a.q0_pending.occupied &&
                outbound.count == 1U && outbound.payloads[0] == 0xA1U &&
                outbound.results[0] == UCN_OK && outbound.event_count == 1U &&
                outbound.stages[0] == UCN_SERVICE_STAGE_LINK_QUEUE_ACCEPTED &&
                outbound.outcomes[0] ==
                    UCN_SERVICE_OUTBOUND_LINK_QUEUE_ACCEPTED &&
                outbound.event_results[0] == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 20U, 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U && outbound.count == 2U &&
                outbound.payloads[1] == 0xA2U && outbound.results[1] == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(&router_c, BRIDGE_SERVICE_ACTUATOR,
                                       0x60U, &message) == UCN_OK);
    TEST_ASSERT(message.payload[0] == 0xA1U);
    TEST_ASSERT(ucn_service_inbox_take(&router_c, BRIDGE_SERVICE_ACTUATOR,
                                       0x60U, &message) == UCN_OK);
    TEST_ASSERT(message.payload[0] == 0xA2U);

    /* Exhaustion is final once, despite three failed Link attempts. */
    payload = 0xA3U;
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL,
                                 0x60U, UCN_TRAFFIC_Q0_CRITICAL,
                                 &payload, 1U) == UCN_OK);
    cac.no_space_remaining = 3U;
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 30U, 1U, &processed) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 35U, 1U, &processed) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 40U, 1U, &processed) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(!bridge_a.q0_pending.occupied && outbound.count == 3U &&
                outbound.payloads[2] == 0xA3U &&
                outbound.results[2] == UCN_ERR_NO_SPACE &&
                outbound.event_count == 3U &&
                outbound.stages[2] == UCN_SERVICE_STAGE_NONE &&
                outbound.outcomes[2] ==
                    UCN_SERVICE_OUTBOUND_BACKPRESSURE_EXHAUSTED &&
                node_a.stats.tx_error_dropped == 1U);

    /* LINK_DOWN is terminal and never consumes the retry budget. */
    payload = 0xA4U;
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL,
                                 0x60U, UCN_TRAFFIC_Q0_CRITICAL,
                                 &payload, 1U) == UCN_OK);
    cac.forced_result = UCN_ERR_LINK_DOWN;
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 50U, 1U, &processed) == UCN_ERR_LINK_DOWN);
    TEST_ASSERT(!bridge_a.q0_pending.occupied && outbound.count == 4U &&
                outbound.results[3] == UCN_ERR_LINK_DOWN &&
                outbound.event_count == 4U &&
                outbound.outcomes[3] ==
                    UCN_SERVICE_OUTBOUND_TERMINAL_FAILED);
    cac.forced_result = UCN_OK;

    /* Expiry completes locally with TTL and never retries late. */
    policy.retry_interval_ms = 10U;
    policy.timeout_ms = 15U;
    TEST_ASSERT(ucn_service_protocol_bridge_set_q0_backpressure_policy(
                    &bridge_a, &policy) == UCN_OK);
    payload = 0xA5U;
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL,
                                 0x60U, UCN_TRAFFIC_Q0_CRITICAL,
                                 &payload, 1U) == UCN_OK);
    cac.no_space_remaining = 1U;
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 60U, 1U, &processed) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 76U, 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U && !bridge_a.q0_pending.occupied &&
                outbound.count == 5U && outbound.payloads[4] == 0xA5U &&
                outbound.results[4] == UCN_ERR_TTL &&
                outbound.event_count == 5U &&
                outbound.outcomes[4] == UCN_SERVICE_OUTBOUND_EXPIRED);

    /* With retry disabled, immediate local queue rejection is distinguishable
     * from a configured retry budget being exhausted. */
    TEST_ASSERT(ucn_service_protocol_bridge_set_q0_backpressure_policy(
                    &bridge_a, NULL) == UCN_OK);
    payload = 0xA6U;
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL,
                                 0x60U, UCN_TRAFFIC_Q0_CRITICAL,
                                 &payload, 1U) == UCN_OK);
    cac.no_space_remaining = 1U;
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, 80U, 1U, &processed) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(processed == 1U && outbound.count == 6U &&
                outbound.event_count == 6U &&
                outbound.outcomes[5] ==
                    UCN_SERVICE_OUTBOUND_BACKPRESSURE_REJECTED &&
                outbound.event_results[5] == UCN_ERR_NO_SPACE);

    stats = ucn_service_protocol_bridge_get_stats(&bridge_a);
    TEST_ASSERT(stats != NULL && stats->remote_tx_attempted == 10U &&
                stats->remote_tx_accepted == 2U && stats->remote_tx_failed == 4U &&
                stats->q0_backpressure_retries == 5U &&
                stats->q0_backpressure_exhausted == 1U &&
                stats->q0_backpressure_terminal_failed == 1U &&
                stats->q0_backpressure_expired == 1U);
    TEST_ASSERT(cac.send_calls == 10U && node_a.stats.tx_sent == 2U &&
                node_a.stats.tx_error_dropped == 3U);
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

static int test_service_bridge_validator_install_and_replay(void)
{
    ucn_node_t node;
    ucn_service_router_t router;
    ucn_service_protocol_bridge_t bridge;
    bridge_validator_state_t validator_state;
    ucn_service_bridge_replay_state_t replay;
    uint8_t index;

    TEST_ASSERT(bridge_init_node(&node, UINT32_C(7)) == 0);
    TEST_ASSERT(bridge_init_router_with_bindings(
                    &router, UINT32_C(7), VALIDATED_BINDINGS,
                    (uint8_t)(sizeof(VALIDATED_BINDINGS) /
                              sizeof(VALIDATED_BINDINGS[0]))) == 0);
    (void)memset(&bridge, 0, sizeof(bridge));
    (void)memset(&validator_state, 0, sizeof(validator_state));
    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge, &router, &node) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge, 0x62U, bridge_guard_validator,
                    &validator_state) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge, 0x60U, bridge_guard_validator,
                    &validator_state) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge, 0x60U, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge, 0x60U, NULL, NULL) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge, 0x60U, bridge_guard_validator,
                    &validator_state) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge, 0x61U, bridge_compact_validator,
                    &validator_state) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge, 0x40U, bridge_guard_validator,
                    &validator_state) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge, 0x60U, bridge_guard_validator,
                    &validator_state) == UCN_ERR_CONFIG);

    ucn_service_bridge_replay_init(&replay);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    NULL, UINT32_C(1), UINT32_C(10), 0x60U,
                    UINT32_C(1)) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UCN_NODE_BROADCAST, UINT32_C(10), 0x60U,
                    UINT32_C(1)) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(1), UINT32_C(10), 0x60U,
                    UINT32_C(100)) == UCN_OK);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(1), UINT32_C(10), 0x60U,
                    UINT32_C(100)) == UCN_ERR_REPLAY);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(1), UINT32_C(10), 0x60U,
                    UINT32_C(99)) == UCN_ERR_REPLAY);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(1), UINT32_C(11), 0x60U,
                    UINT32_C(1)) == UCN_ERR_SECURITY);
    TEST_ASSERT(ucn_service_bridge_replay_rotate_session(
                    &replay, UINT32_C(1), 0x60U, UINT32_C(10),
                    UINT32_C(10)) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_service_bridge_replay_rotate_session(
                    &replay, UINT32_C(1), 0x60U, UINT32_C(10),
                    UINT32_C(0)) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_service_bridge_replay_rotate_session(
                    &replay, UINT32_C(1), 0x60U, UINT32_C(10),
                    UINT32_C(11)) == UCN_OK);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(1), UINT32_C(11), 0x60U,
                    UINT32_C(1)) == UCN_OK);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(1), UINT32_C(10), 0x60U,
                    UINT32_C(101)) == UCN_ERR_SECURITY);
    for (index = 1U; index < UCN_SERVICE_BRIDGE_REPLAY_DEPTH; ++index) {
        TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                        &replay, (ucn_node_id_t)(index + 1U), UINT32_C(20),
                        0x60U, UINT32_C(1)) == UCN_OK);
    }
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(99), UINT32_C(20), 0x60U,
                    UINT32_C(1)) == UCN_ERR_NO_SPACE);

    ucn_service_bridge_replay_init(&replay);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(1), UINT32_C(1), 0x60U,
                    UINT32_C(0xFFFFFFFE)) == UCN_OK);
    TEST_ASSERT(ucn_service_bridge_replay_accept_command(
                    &replay, UINT32_C(1), UINT32_C(1), 0x60U,
                    UINT32_C(1)) == UCN_OK);
    return 0;
}

static int test_service_bridge_validated_two_node_round_trip(void)
{
    ucn_node_t node_a, node_c;
    ucn_link_t ac, ca;
    bridge_link_context_t cac, cca;
    ucn_service_router_t router_a, router_c;
    ucn_service_protocol_bridge_t bridge_a, bridge_c;
    bridge_validator_state_t validator_a, validator_c;
    bridge_outbound_state_t outbound_a;
    ucn_service_message_t message;
    uint8_t guard_payload[13];
    uint8_t result_payload[UCN_SERVICE_RESULT_HEADER_BYTES];
    uint8_t compact_payload = 0xA5U;
    uint8_t telemetry_payload = 0x44U;
    uint8_t local_unchecked_payload = 0x01U;
    uint8_t processed;
    const ucn_service_command_guard_t command = {
        UINT32_C(10), UINT32_C(900), 200U, 0x40U, 0U
    };
    ucn_service_result_header_t result_header;
    ucn_service_result_header_t decoded_result;
    const ucn_service_bridge_stats_t *stats;

    TEST_ASSERT(bridge_init_node(&node_a, UINT32_C(1)) == 0);
    TEST_ASSERT(bridge_init_node(&node_c, UINT32_C(3)) == 0);
    TEST_ASSERT(bridge_init_router_with_bindings(
                    &router_a, UINT32_C(1), VALIDATED_BINDINGS,
                    (uint8_t)(sizeof(VALIDATED_BINDINGS) /
                              sizeof(VALIDATED_BINDINGS[0]))) == 0);
    TEST_ASSERT(bridge_init_router_with_bindings(
                    &router_c, UINT32_C(3), VALIDATED_BINDINGS,
                    (uint8_t)(sizeof(VALIDATED_BINDINGS) /
                              sizeof(VALIDATED_BINDINGS[0]))) == 0);
    (void)memset(&ac, 0, sizeof(ac));
    (void)memset(&ca, 0, sizeof(ca));
    (void)memset(&cac, 0, sizeof(cac));
    (void)memset(&cca, 0, sizeof(cca));
    (void)memset(&bridge_a, 0, sizeof(bridge_a));
    (void)memset(&bridge_c, 0, sizeof(bridge_c));
    (void)memset(&validator_a, 0, sizeof(validator_a));
    (void)memset(&validator_c, 0, sizeof(validator_c));
    (void)memset(&outbound_a, 0, sizeof(outbound_a));

    ac.ops = &BRIDGE_LINK_OPS; ac.context = &cac; ac.link_id = 1U;
    ac.mtu = UCN_MAX_FRAME_BYTES; ac.peer_node_id = UINT32_C(3);
    ca.ops = &BRIDGE_LINK_OPS; ca.context = &cca; ca.link_id = 2U;
    ca.mtu = UCN_MAX_FRAME_BYTES; ca.peer_node_id = UINT32_C(1);
    cac.peer = &node_c; cac.peer_ingress = &ca;
    cca.peer = &node_a; cca.peer_ingress = &ac;
    TEST_ASSERT(ucn_node_register_link(&node_a, &ac) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_c, &ca) == UCN_OK);
    node_a.session_id = UINT32_C(0x10);
    node_c.session_id = UINT32_C(0x20);
    node_a.now_ms = UINT32_C(1000);
    node_c.now_ms = UINT32_C(1000);

    TEST_ASSERT(ucn_service_protocol_bridge_init(
                    &bridge_a, &router_a, &node_a) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_init(
                    &bridge_c, &router_c, &node_c) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge_a, 0x60U, bridge_guard_validator,
                    &validator_a) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge_a, 0x61U, bridge_compact_validator,
                    &validator_a) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge_c, 0x60U, bridge_guard_validator,
                    &validator_c) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_set_validator(
                    &bridge_c, 0x61U, bridge_compact_validator,
                    &validator_c) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge_a) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge_c) == UCN_OK);

    /* Local Fast Path never invokes the remote Validator.  The execution Task
     * remains responsible for its own second validation before actuation. */
    TEST_ASSERT(ucn_service_send(
                    &router_c, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                    UCN_TRAFFIC_Q0_CRITICAL, &local_unchecked_payload, 1U) == UCN_OK);
    TEST_ASSERT(validator_c.calls == 0U);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U, &message) == UCN_OK);

    /* Ordinary Q1 telemetry does not consume a Validator slot or add a Guard. */
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_SENSOR, 0x40U,
                    UCN_TRAFFIC_Q1_REALTIME, &telemetry_payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U && validator_c.calls == 0U);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_CONTROL, 0x40U, &message) == UCN_OK);

    TEST_ASSERT(bridge_build_guard_payload(
                    guard_payload, command.command_id, command.issued_at_ms,
                    command.valid_for_ms) == 0);
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                    UCN_TRAFFIC_Q0_CRITICAL, guard_payload,
                    (uint16_t)sizeof(guard_payload)) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U && validator_c.calls == 1U &&
                validator_c.last_source == UINT32_C(1) &&
                validator_c.last_session == UINT32_C(0x10) &&
                validator_c.last_endpoint == 0x60U &&
                validator_c.last_now_ms == UINT32_C(1000));
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U, &message) == UCN_OK);

    /* The Result Endpoint remains ordinary business Q1 traffic.  The target
     * explicitly reports Inbox then Executed; neither stage is inferred from
     * the source-side UCN_OK. */
    result_header.command_id = command.command_id;
    result_header.stage = UCN_SERVICE_STAGE_REMOTE_INBOXED;
    result_header.status = UCN_SERVICE_RESULT_ACCEPTED;
    result_header.detail_code = 0U;
    TEST_ASSERT(ucn_service_result_header_encode(
                    &result_header, result_payload) == UCN_OK);
    TEST_ASSERT(ucn_service_send(
                    &router_c, UINT32_C(1), BRIDGE_SERVICE_SENSOR, 0x40U,
                    UCN_TRAFFIC_Q1_REALTIME, result_payload,
                    (uint16_t)sizeof(result_payload)) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_c, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(processed == 1U && validator_a.calls == 0U);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_a, BRIDGE_SERVICE_CONTROL, 0x40U, &message) == UCN_OK);
    TEST_ASSERT(ucn_service_result_header_decode(
                    message.payload, message.payload_length, &decoded_result) == UCN_OK &&
                decoded_result.stage == UCN_SERVICE_STAGE_REMOTE_INBOXED &&
                ucn_service_result_matches_command(
                    &command, message.endpoint, &decoded_result));

    result_header.stage = UCN_SERVICE_STAGE_REMOTE_EXECUTED;
    result_header.status = UCN_SERVICE_RESULT_SUCCEEDED;
    result_header.detail_code = UINT16_C(0x0090);
    TEST_ASSERT(ucn_service_result_header_encode(
                    &result_header, result_payload) == UCN_OK);
    TEST_ASSERT(ucn_service_send(
                    &router_c, UINT32_C(1), BRIDGE_SERVICE_SENSOR, 0x40U,
                    UCN_TRAFFIC_Q1_REALTIME, result_payload,
                    (uint16_t)sizeof(result_payload)) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_c, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_a, BRIDGE_SERVICE_CONTROL, 0x40U, &message) == UCN_OK);
    TEST_ASSERT(ucn_service_result_header_decode(
                    message.payload, message.payload_length, &decoded_result) == UCN_OK &&
                decoded_result.stage == UCN_SERVICE_STAGE_REMOTE_EXECUTED &&
                decoded_result.status == UCN_SERVICE_RESULT_SUCCEEDED &&
                decoded_result.detail_code == UINT16_C(0x0090) &&
                ucn_service_result_matches_command(
                    &command, message.endpoint, &decoded_result));

    /* Duplicate, expired and malformed remote commands never enter Inbox. */
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_event_observer(
                    &bridge_a, bridge_outbound_event_observer, &outbound_a) == UCN_OK);
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                    UCN_TRAFFIC_Q0_CRITICAL, guard_payload,
                    (uint16_t)sizeof(guard_payload)) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U,
                    &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(bridge_c.stats.last_inbound_result == UCN_ERR_REPLAY);
    TEST_ASSERT(outbound_a.event_count == 1U &&
                outbound_a.stages[0] == UCN_SERVICE_STAGE_LINK_QUEUE_ACCEPTED &&
                outbound_a.outcomes[0] ==
                    UCN_SERVICE_OUTBOUND_LINK_QUEUE_ACCEPTED);
    /* No generic remote ACK is synthesized for the target-side rejection.
     * The product-owned command timer therefore reaches its deadline with no
     * matching Result Endpoint message. */
    node_a.now_ms = UINT32_C(1200);
    TEST_ASSERT((int32_t)(node_a.now_ms - UINT32_C(1100)) >= 0 &&
                ucn_service_inbox_take(
                    &router_a, BRIDGE_SERVICE_CONTROL, 0x40U,
                    &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_service_protocol_bridge_set_outbound_event_observer(
                    &bridge_a, NULL, NULL) == UCN_OK);

    TEST_ASSERT(bridge_build_guard_payload(
                    guard_payload, UINT32_C(11), UINT32_C(800), 100U) == 0);
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                    UCN_TRAFFIC_Q0_CRITICAL, guard_payload,
                    (uint16_t)sizeof(guard_payload)) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U,
                    &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(bridge_c.stats.last_inbound_result == UCN_ERR_TTL);

    TEST_ASSERT(bridge_build_guard_payload(
                    guard_payload, UINT32_C(12), UINT32_C(950), 100U) == 0);
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                    UCN_TRAFFIC_Q0_CRITICAL, guard_payload, 12U) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U,
                    &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(bridge_c.stats.last_inbound_result == UCN_ERR_MALFORMED);

    /* A different Session is not automatically trusted.  Authenticated
     * product code rotates it explicitly, after which IDs may restart. */
    node_a.session_id = UINT32_C(0x11);
    TEST_ASSERT(bridge_build_guard_payload(
                    guard_payload, UINT32_C(1), UINT32_C(950), 100U) == 0);
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                    UCN_TRAFFIC_Q0_CRITICAL, guard_payload,
                    (uint16_t)sizeof(guard_payload)) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U,
                    &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(bridge_c.stats.last_inbound_result == UCN_ERR_SECURITY);
    TEST_ASSERT(ucn_service_bridge_replay_rotate_session(
                    &validator_c.replay, UINT32_C(1), 0x60U, UINT32_C(0x10),
                    UINT32_C(0x11)) == UCN_OK);
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                    UCN_TRAFFIC_Q0_CRITICAL, guard_payload,
                    (uint16_t)sizeof(guard_payload)) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U, &message) == UCN_OK);

    node_a.session_id = UINT32_C(0x10);
    TEST_ASSERT(bridge_build_guard_payload(
                    guard_payload, UINT32_C(13), UINT32_C(950), 100U) == 0);
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x60U,
                    UCN_TRAFFIC_Q0_CRITICAL, guard_payload,
                    (uint16_t)sizeof(guard_payload)) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x60U,
                    &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(bridge_c.stats.last_inbound_result == UCN_ERR_SECURITY);

    /* A product-specific one-byte format proves Guard is not globally forced. */
    node_a.session_id = UINT32_C(0x11);
    TEST_ASSERT(ucn_service_send(
                    &router_a, UINT32_C(3), BRIDGE_SERVICE_CONTROL, 0x61U,
                    UCN_TRAFFIC_Q0_CRITICAL, &compact_payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_step_at(
                    &bridge_a, UINT32_C(1000), 1U, &processed) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(
                    &router_c, BRIDGE_SERVICE_ACTUATOR, 0x61U, &message) == UCN_OK &&
                message.payload_length == 1U && message.payload[0] == compact_payload);

    stats = ucn_service_protocol_bridge_get_stats(&bridge_c);
    TEST_ASSERT(stats != NULL && stats->inbound_validator_checked == 8U &&
                stats->inbound_validator_accepted == 3U &&
                stats->inbound_validator_rejected == 5U &&
                stats->inbound_validator_missing == 0U &&
                stats->inbound_delivered == 4U && stats->inbound_rejected == 5U);
    return 0;
}

int test_service_bridge(void)
{
    return test_service_bridge_validation() |
           test_service_bridge_three_node_delivery() |
           test_service_bridge_q0_backpressure() |
           test_service_bridge_validator_install_and_replay() |
           test_service_bridge_validated_two_node_round_trip();
}
