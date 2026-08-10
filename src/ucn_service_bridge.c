#include <string.h>

#include "ucn/ucn_service_bridge.h"

static void ucn_service_protocol_bridge_endpoint_rx(void *context,
                                                    const ucn_frame_t *frame)
{
    ucn_service_protocol_bridge_t *bridge = (ucn_service_protocol_bridge_t *)context;
    ucn_result_t result;

    if (bridge == NULL) {
        return;
    }
    if (bridge->inbound_hooks.lock != NULL) {
        bridge->inbound_hooks.lock(bridge->inbound_hooks.context);
    }
    result = ucn_service_deliver_remote(bridge->router, frame);
    if (bridge->inbound_hooks.unlock != NULL) {
        bridge->inbound_hooks.unlock(bridge->inbound_hooks.context);
    }
    bridge->stats.last_inbound_result = result;
    if (result == UCN_OK) {
        bridge->stats.inbound_delivered++;
    } else {
        bridge->stats.inbound_rejected++;
    }
    if (bridge->inbound_hooks.observer != NULL) {
        bridge->inbound_hooks.observer(bridge->inbound_hooks.context, frame, result);
    }
}

static bool ucn_service_protocol_bridge_binding_has_foreign_handler(
    const ucn_service_protocol_bridge_t *bridge,
    ucn_endpoint_t endpoint,
    bool *already_owned)
{
    size_t index;

    *already_owned = false;
    for (index = 0U; index < UCN_MAX_ENDPOINT_HANDLERS; ++index) {
        const ucn_endpoint_handler_entry_t *entry = &bridge->node->endpoint_handlers[index];

        if (!entry->occupied || entry->endpoint != endpoint) {
            continue;
        }
        if (entry->handler == ucn_service_protocol_bridge_endpoint_rx &&
            entry->context == bridge) {
            *already_owned = true;
            return false;
        }
        return true;
    }
    return false;
}

ucn_result_t ucn_service_protocol_bridge_init(
    ucn_service_protocol_bridge_t *bridge,
    ucn_service_router_t *router,
    ucn_node_t *node)
{
    if (bridge == NULL || router == NULL || node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (router->config.bindings == NULL || router->config.binding_count == 0U ||
        router->config.local_node_id != node->config.node_id) {
        return UCN_ERR_CONFIG;
    }

    (void)memset(bridge, 0, sizeof(*bridge));
    bridge->router = router;
    bridge->node = node;
    bridge->stats.last_tx_result = UCN_OK;
    bridge->stats.last_inbound_result = UCN_OK;
    return UCN_OK;
}

ucn_result_t ucn_service_protocol_bridge_install_endpoint_handlers(
    ucn_service_protocol_bridge_t *bridge)
{
    uint8_t binding_index;
    size_t free_entries = 0U;
    size_t required_entries = 0U;

    if (bridge == NULL || bridge->router == NULL || bridge->node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    for (binding_index = 0U; binding_index < bridge->router->config.binding_count;
         ++binding_index) {
        bool already_owned;

        if (ucn_service_protocol_bridge_binding_has_foreign_handler(
                bridge, bridge->router->config.bindings[binding_index].endpoint, &already_owned)) {
            return UCN_ERR_CONFIG;
        }
        if (!already_owned) {
            required_entries++;
        }
    }
    for (size_t index = 0U; index < UCN_MAX_ENDPOINT_HANDLERS; ++index) {
        if (!bridge->node->endpoint_handlers[index].occupied) {
            free_entries++;
        }
    }
    if (free_entries < required_entries) {
        return UCN_ERR_NO_SPACE;
    }

    for (binding_index = 0U; binding_index < bridge->router->config.binding_count;
         ++binding_index) {
        const ucn_result_t result = ucn_node_set_endpoint_handler(
            bridge->node, bridge->router->config.bindings[binding_index].endpoint,
            ucn_service_protocol_bridge_endpoint_rx, bridge);

        if (result != UCN_OK) {
            return result;
        }
    }
    bridge->endpoint_handlers_installed = true;
    bridge->stats.endpoint_handlers_installed = bridge->router->config.binding_count;
    return UCN_OK;
}

ucn_result_t ucn_service_protocol_bridge_set_inbound_hooks(
    ucn_service_protocol_bridge_t *bridge,
    const ucn_service_bridge_inbound_hooks_t *hooks)
{
    if (bridge == NULL || bridge->router == NULL || bridge->node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (hooks != NULL && ((hooks->lock == NULL) != (hooks->unlock == NULL))) {
        return UCN_ERR_CONFIG;
    }
    if (hooks == NULL) {
        (void)memset(&bridge->inbound_hooks, 0, sizeof(bridge->inbound_hooks));
    } else {
        bridge->inbound_hooks = *hooks;
    }
    return UCN_OK;
}

ucn_result_t ucn_service_protocol_bridge_step(
    ucn_service_protocol_bridge_t *bridge,
    uint8_t max_requests,
    uint8_t *processed)
{
    uint8_t count = 0U;

    if (processed != NULL) {
        *processed = 0U;
    }
    if (bridge == NULL || bridge->router == NULL || bridge->node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!bridge->endpoint_handlers_installed) {
        return UCN_ERR_CONFIG;
    }

    while (count < max_requests) {
        ucn_service_message_t message;
        ucn_result_t result;

        if (bridge->inbound_hooks.lock != NULL) {
            bridge->inbound_hooks.lock(bridge->inbound_hooks.context);
        }
        result = ucn_service_remote_tx_take(bridge->router, &message);
        if (bridge->inbound_hooks.unlock != NULL) {
            bridge->inbound_hooks.unlock(bridge->inbound_hooks.context);
        }

        if (result == UCN_ERR_NOT_FOUND) {
            break;
        }
        if (result != UCN_OK) {
            bridge->stats.last_tx_result = result;
            return result;
        }

        bridge->stats.remote_tx_attempted++;
        result = ucn_node_send_endpoint(bridge->node, message.destination_node_id,
                                        message.endpoint, message.traffic_class,
                                        message.payload, message.payload_length);
        bridge->stats.last_tx_result = result;
        count++;
        if (result != UCN_OK) {
            bridge->stats.remote_tx_failed++;
            if (processed != NULL) {
                *processed = count;
            }
            return result;
        }
        bridge->stats.remote_tx_accepted++;
    }

    if (processed != NULL) {
        *processed = count;
    }
    return UCN_OK;
}

const ucn_service_bridge_stats_t *ucn_service_protocol_bridge_get_stats(
    const ucn_service_protocol_bridge_t *bridge)
{
    return bridge == NULL ? NULL : &bridge->stats;
}
