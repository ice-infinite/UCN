#include <string.h>

#include "ucn/ucn_service.h"

static bool ucn_service_id_is_valid(ucn_service_id_t service_id)
{
    return service_id != UCN_SERVICE_ID_NONE && service_id <= UCN_SERVICE_ID_MAX;
}

static bool ucn_service_traffic_is_supported(ucn_traffic_class_t traffic_class)
{
    return traffic_class == UCN_TRAFFIC_Q0_CRITICAL ||
           traffic_class == UCN_TRAFFIC_Q1_REALTIME;
}

static uint8_t ucn_service_traffic_mask(ucn_traffic_class_t traffic_class)
{
    return UCN_SERVICE_TRAFFIC_MASK(traffic_class);
}

static int ucn_service_find_binding(const ucn_service_router_t *router,
                                    ucn_endpoint_t endpoint)
{
    uint8_t index;

    for (index = 0U; index < router->config.binding_count; ++index) {
        if (router->config.bindings[index].endpoint == endpoint) {
            return (int)index;
        }
    }
    return -1;
}

static bool ucn_service_binding_is_valid(const ucn_service_binding_t *binding)
{
    const uint8_t q0_mask = ucn_service_traffic_mask(UCN_TRAFFIC_Q0_CRITICAL);
    const uint8_t q1_mask = ucn_service_traffic_mask(UCN_TRAFFIC_Q1_REALTIME);

    if (!ucn_endpoint_is_static(binding->endpoint) ||
        !ucn_service_id_is_valid(binding->owner_service_id) ||
        binding->max_payload_length == 0U ||
        binding->max_payload_length > UCN_SERVICE_MAX_PAYLOAD_BYTES ||
        binding->allowed_local_source_mask == 0U) {
        return false;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
        return binding->allowed_traffic_mask == q0_mask;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q1_LATEST) {
        return binding->allowed_traffic_mask == q1_mask;
    }
    return false;
}

static ucn_result_t ucn_service_validate_target(ucn_service_router_t *router,
                                                uint8_t binding_index,
                                                ucn_traffic_class_t traffic_class,
                                                uint16_t payload_length)
{
    const ucn_service_binding_t *binding = &router->config.bindings[binding_index];

    if (!router->binding_states[binding_index].ready) {
        router->stats.not_ready++;
        return UCN_ERR_NOT_FOUND;
    }
    if (!ucn_service_traffic_is_supported(traffic_class) ||
        (binding->allowed_traffic_mask & ucn_service_traffic_mask(traffic_class)) == 0U) {
        router->stats.traffic_rejected++;
        return UCN_ERR_ARGUMENT;
    }
    if (payload_length > binding->max_payload_length) {
        router->stats.length_rejected++;
        return UCN_ERR_TOO_LARGE;
    }
    return UCN_OK;
}

static void ucn_service_build_message(ucn_service_message_t *message,
                                      ucn_node_id_t source_node_id,
                                      ucn_node_id_t destination_node_id,
                                      ucn_service_id_t source_service_id,
                                      ucn_endpoint_t endpoint,
                                      ucn_traffic_class_t traffic_class,
                                      const uint8_t *payload,
                                      uint16_t payload_length)
{
    (void)memset(message, 0, sizeof(*message));
    message->source_node_id = source_node_id;
    message->destination_node_id = destination_node_id;
    message->source_service_id = source_service_id;
    message->endpoint = endpoint;
    message->traffic_class = traffic_class;
    message->payload_length = payload_length;
    if (payload_length > 0U) {
        (void)memcpy(message->payload, payload, payload_length);
    }
}

static ucn_result_t ucn_service_q0_push(ucn_service_q0_inbox_t *inbox,
                                        const ucn_service_message_t *message)
{
    if (inbox->count >= UCN_SERVICE_Q0_INBOX_DEPTH) {
        return UCN_ERR_NO_SPACE;
    }
    inbox->messages[inbox->tail] = *message;
    inbox->tail = (uint8_t)((inbox->tail + 1U) % UCN_SERVICE_Q0_INBOX_DEPTH);
    inbox->count++;
    return UCN_OK;
}

static ucn_result_t ucn_service_q0_take(ucn_service_q0_inbox_t *inbox,
                                        ucn_service_message_t *message)
{
    if (inbox->count == 0U) {
        return UCN_ERR_NOT_FOUND;
    }
    *message = inbox->messages[inbox->head];
    inbox->head = (uint8_t)((inbox->head + 1U) % UCN_SERVICE_Q0_INBOX_DEPTH);
    inbox->count--;
    return UCN_OK;
}

static ucn_result_t ucn_service_deliver_to_binding(ucn_service_router_t *router,
                                                    uint8_t binding_index,
                                                    const ucn_service_message_t *message)
{
    const ucn_service_binding_t *binding = &router->config.bindings[binding_index];
    const ucn_service_binding_state_t *state = &router->binding_states[binding_index];

    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
        const ucn_result_t result = ucn_service_q0_push(
            &router->q0_inboxes[state->q0_inbox_index], message);

        if (result != UCN_OK) {
            router->stats.q0_inbox_full++;
        }
        return result;
    }

    if (router->q1_inboxes[state->q1_inbox_index].occupied) {
        router->stats.q1_overwrites++;
    }
    router->q1_inboxes[state->q1_inbox_index].latest = *message;
    router->q1_inboxes[state->q1_inbox_index].occupied = true;
    return UCN_OK;
}

static int ucn_service_find_remote_q1(const ucn_service_router_t *router,
                                      ucn_node_id_t destination,
                                      ucn_endpoint_t endpoint)
{
    uint8_t index;

    for (index = 0U; index < UCN_SERVICE_REMOTE_TX_Q1_DEPTH; ++index) {
        const ucn_service_remote_q1_slot_t *slot = &router->remote_q1[index];

        if (slot->occupied && slot->message.destination_node_id == destination &&
            slot->message.endpoint == endpoint) {
            return (int)index;
        }
    }
    return -1;
}

static int ucn_service_find_free_remote_q1(const ucn_service_router_t *router)
{
    uint8_t index;

    for (index = 0U; index < UCN_SERVICE_REMOTE_TX_Q1_DEPTH; ++index) {
        if (!router->remote_q1[index].occupied) {
            return (int)index;
        }
    }
    return -1;
}

ucn_result_t ucn_service_router_init(ucn_service_router_t *router,
                                     const ucn_service_router_config_t *config)
{
    uint8_t index;
    uint8_t q0_count = 0U;
    uint8_t q1_count = 0U;

    if (router == NULL || config == NULL || config->bindings == NULL ||
        config->binding_count == 0U || config->binding_count > UCN_SERVICE_MAX_BINDINGS ||
        config->local_node_id == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }

    (void)memset(router, 0, sizeof(*router));
    router->config = *config;
    for (index = 0U; index < config->binding_count; ++index) {
        const ucn_service_binding_t *binding = &config->bindings[index];
        uint8_t other_index;

        if (!ucn_service_binding_is_valid(binding)) {
            (void)memset(router, 0, sizeof(*router));
            return UCN_ERR_CONFIG;
        }
        for (other_index = 0U; other_index < index; ++other_index) {
            if (config->bindings[other_index].endpoint == binding->endpoint) {
                (void)memset(router, 0, sizeof(*router));
                return UCN_ERR_CONFIG;
            }
        }

        router->binding_states[index].ready = binding->enabled_at_boot;
        router->binding_states[index].q0_inbox_index = UCN_SERVICE_BINDING_INDEX_NONE;
        router->binding_states[index].q1_inbox_index = UCN_SERVICE_BINDING_INDEX_NONE;
        if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
            if (q0_count >= UCN_SERVICE_MAX_Q0_BINDINGS) {
                (void)memset(router, 0, sizeof(*router));
                return UCN_ERR_NO_SPACE;
            }
            router->binding_states[index].q0_inbox_index = q0_count++;
        } else {
            if (q1_count >= UCN_SERVICE_MAX_Q1_BINDINGS) {
                (void)memset(router, 0, sizeof(*router));
                return UCN_ERR_NO_SPACE;
            }
            router->binding_states[index].q1_inbox_index = q1_count++;
        }
    }
    return UCN_OK;
}

ucn_result_t ucn_service_set_ready(ucn_service_router_t *router,
                                   ucn_endpoint_t endpoint,
                                   bool ready)
{
    const int binding_index = router == NULL ? -1 : ucn_service_find_binding(router, endpoint);

    if (router == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (binding_index < 0) {
        router->stats.unknown_endpoint++;
        return UCN_ERR_NOT_FOUND;
    }
    router->binding_states[binding_index].ready = ready;
    return UCN_OK;
}

ucn_result_t ucn_service_send(ucn_service_router_t *router,
                              ucn_node_id_t destination,
                              ucn_service_id_t source_service_id,
                              ucn_endpoint_t endpoint,
                              ucn_traffic_class_t traffic_class,
                              const uint8_t *payload,
                              uint16_t payload_length)
{
    const int binding_index = router == NULL ? -1 : ucn_service_find_binding(router, endpoint);
    const ucn_service_binding_t *binding;
    ucn_service_message_t message;
    ucn_result_t result;

    if (router == NULL || !ucn_service_id_is_valid(source_service_id) ||
        (payload_length > 0U && payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    if (binding_index < 0) {
        router->stats.unknown_endpoint++;
        return UCN_ERR_NOT_FOUND;
    }
    binding = &router->config.bindings[binding_index];
    result = ucn_service_validate_target(router, (uint8_t)binding_index, traffic_class,
                                         payload_length);
    if (result != UCN_OK) {
        return result;
    }
    if ((binding->allowed_local_source_mask &
         UCN_SERVICE_SOURCE_MASK(source_service_id)) == 0U) {
        router->stats.local_acl_rejected++;
        return UCN_ERR_ACCESS;
    }

    ucn_service_build_message(&message, router->config.local_node_id, destination,
                              source_service_id, endpoint, traffic_class, payload,
                              payload_length);
    if (destination == router->config.local_node_id) {
        result = ucn_service_deliver_to_binding(router, (uint8_t)binding_index, &message);
        if (result == UCN_OK) {
            router->stats.local_delivered++;
        }
        return result;
    }

    if (traffic_class == UCN_TRAFFIC_Q0_CRITICAL) {
        result = ucn_service_q0_push(&router->remote_q0, &message);
        if (result != UCN_OK) {
            router->stats.remote_q0_full++;
            return result;
        }
    } else {
        int slot_index = ucn_service_find_remote_q1(router, destination, endpoint);

        if (slot_index >= 0) {
            router->remote_q1[slot_index].message = message;
            router->stats.remote_q1_overwrites++;
        } else {
            slot_index = ucn_service_find_free_remote_q1(router);
            if (slot_index < 0) {
                router->stats.remote_q1_full++;
                return UCN_ERR_NO_SPACE;
            }
            router->remote_q1[slot_index].occupied = true;
            router->remote_q1[slot_index].message = message;
        }
    }
    router->stats.remote_enqueued++;
    return UCN_OK;
}

ucn_result_t ucn_service_deliver_remote(ucn_service_router_t *router,
                                        const ucn_frame_t *frame)
{
    const int binding_index = router == NULL || frame == NULL ?
                                  -1 : ucn_service_find_binding(router, frame->message_type);
    const ucn_service_binding_t *binding;
    ucn_service_message_t message;
    ucn_result_t result;

    if (router == NULL || frame == NULL ||
        (frame->payload_length > 0U && frame->payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    if (frame->destination != router->config.local_node_id &&
        frame->destination != UCN_NODE_BROADCAST) {
        return UCN_ERR_NETWORK;
    }
    if (binding_index < 0) {
        router->stats.unknown_endpoint++;
        return UCN_ERR_NOT_FOUND;
    }
    binding = &router->config.bindings[binding_index];
    if (!binding->accept_remote) {
        router->stats.remote_rejected++;
        return UCN_ERR_ACCESS;
    }
    result = ucn_service_validate_target(router, (uint8_t)binding_index,
                                         frame->traffic_class, frame->payload_length);
    if (result != UCN_OK) {
        return result;
    }
    ucn_service_build_message(&message, frame->source, frame->destination,
                              UCN_SERVICE_ID_NONE, frame->message_type,
                              frame->traffic_class, frame->payload,
                              frame->payload_length);
    result = ucn_service_deliver_to_binding(router, (uint8_t)binding_index, &message);
    if (result == UCN_OK) {
        router->stats.inbound_delivered++;
    }
    return result;
}

ucn_result_t ucn_service_inbox_take(ucn_service_router_t *router,
                                    ucn_service_id_t owner_service_id,
                                    ucn_endpoint_t endpoint,
                                    ucn_service_message_t *message)
{
    const int binding_index = router == NULL ? -1 : ucn_service_find_binding(router, endpoint);
    const ucn_service_binding_t *binding;
    const ucn_service_binding_state_t *state;
    ucn_result_t result;

    if (router == NULL || message == NULL || !ucn_service_id_is_valid(owner_service_id)) {
        return UCN_ERR_ARGUMENT;
    }
    if (binding_index < 0) {
        router->stats.unknown_endpoint++;
        return UCN_ERR_NOT_FOUND;
    }
    binding = &router->config.bindings[binding_index];
    if (binding->owner_service_id != owner_service_id) {
        return UCN_ERR_ACCESS;
    }
    state = &router->binding_states[binding_index];
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
        result = ucn_service_q0_take(&router->q0_inboxes[state->q0_inbox_index], message);
    } else if (!router->q1_inboxes[state->q1_inbox_index].occupied) {
        result = UCN_ERR_NOT_FOUND;
    } else {
        *message = router->q1_inboxes[state->q1_inbox_index].latest;
        router->q1_inboxes[state->q1_inbox_index].occupied = false;
        result = UCN_OK;
    }
    if (result == UCN_OK) {
        router->stats.inbox_reads++;
    }
    return result;
}

ucn_result_t ucn_service_remote_tx_take(ucn_service_router_t *router,
                                        ucn_service_message_t *message)
{
    uint8_t index;
    ucn_result_t result;

    if (router == NULL || message == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_service_q0_take(&router->remote_q0, message);
    if (result == UCN_OK) {
        router->stats.remote_tx_reads++;
        return UCN_OK;
    }
    for (index = 0U; index < UCN_SERVICE_REMOTE_TX_Q1_DEPTH; ++index) {
        if (router->remote_q1[index].occupied) {
            *message = router->remote_q1[index].message;
            router->remote_q1[index].occupied = false;
            router->stats.remote_tx_reads++;
            return UCN_OK;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

const ucn_service_stats_t *ucn_service_get_stats(const ucn_service_router_t *router)
{
    return router == NULL ? NULL : &router->stats;
}
