#include <string.h>

#include "ucn/ucn_service.h"

/*
 * EN: Checks whether `id` satisfies the Service Router module's validity rules.
 * 中文：检查 `id` 是否满足 Service Router 模块的合法性规则。
 */
static bool ucn_service_id_is_valid(ucn_service_id_t service_id)
{
    return service_id != UCN_SERVICE_ID_NONE && service_id <= UCN_SERVICE_ID_MAX;
}

/*
 * EN: Checks the `traffic_is_supported` condition against current Service Router state.
 * 中文：根据当前 Service Router 状态检查 `traffic_is_supported` 条件。
 */
static bool ucn_service_traffic_is_supported(ucn_traffic_class_t traffic_class)
{
    return (uint32_t)traffic_class < (uint32_t)UCN_TRAFFIC_CLASS_COUNT;
}

/*
 * EN: Calculates `traffic_mask` with bounded, deterministic Service Router arithmetic.
 * 中文：使用有界且确定性的 Service Router 算术计算 `traffic_mask`。
 */
static uint8_t ucn_service_traffic_mask(ucn_traffic_class_t traffic_class)
{
    return UCN_SERVICE_TRAFFIC_MASK(traffic_class);
}

/*
 * EN: Searches bounded Service Router state for `binding`.
 * 中文：在固定容量的 Service Router 状态中查找 `binding`。
 */
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

/*
 * EN: Checks whether `binding` satisfies the Service Router module's validity rules.
 * 中文：检查 `binding` 是否满足 Service Router 模块的合法性规则。
 */
static bool ucn_service_binding_is_valid(const ucn_service_binding_t *binding)
{
    const uint8_t q0_mask = ucn_service_traffic_mask(UCN_TRAFFIC_Q0_CRITICAL);
    const uint8_t q1_mask = ucn_service_traffic_mask(UCN_TRAFFIC_Q1_REALTIME);
    const uint8_t q2_mask = ucn_service_traffic_mask(UCN_TRAFFIC_Q2_NORMAL);
    const uint8_t q3_mask = ucn_service_traffic_mask(UCN_TRAFFIC_Q3_BULK);

    if (!ucn_endpoint_is_static(binding->endpoint) ||
        !ucn_service_id_is_valid(binding->owner_service_id) ||
        binding->max_payload_length == 0U ||
        binding->max_payload_length > UCN_SERVICE_MAX_PAYLOAD_BYTES ||
        binding->allowed_local_source_mask == 0U ||
        (binding->require_remote_q0_validator && !binding->accept_remote)) {
        return false;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
        return binding->allowed_traffic_mask == q0_mask;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q1_LATEST) {
        return binding->allowed_traffic_mask == q1_mask &&
               !binding->require_remote_q0_validator;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q2_FIFO) {
        return binding->allowed_traffic_mask == q2_mask &&
               !binding->require_remote_q0_validator;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q3_FIFO) {
        return binding->allowed_traffic_mask == q3_mask &&
               !binding->require_remote_q0_validator;
    }
    return false;
}

/*
 * EN: Validates `target` before Service Router state is used or changed.
 * 中文：在使用或修改 Service Router 状态前验证 `target`。
 */
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

/*
 * EN: Builds `build_message` in caller-provided storage for Service Router.
 * 中文：在调用方存储中为 Service Router 构造 `build_message`。
 */
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

/*
 * EN: Copies one message into caller-provided bounded FIFO storage.
 * 中文：把一条消息复制到调用方提供的有界 FIFO 存储。
 */
static ucn_result_t ucn_service_fifo_push(ucn_service_message_t *messages,
                                          uint8_t capacity,
                                          uint8_t *tail,
                                          uint8_t *count,
                                          const ucn_service_message_t *message)
{
    if (*count >= capacity) {
        return UCN_ERR_NO_SPACE;
    }
    messages[*tail] = *message;
    *tail = (uint8_t)((*tail + 1U) % capacity);
    (*count)++;
    return UCN_OK;
}

/*
 * EN: Removes one message from caller-provided bounded FIFO storage.
 * 中文：从调用方提供的有界 FIFO 存储中移除一条消息。
 */
static ucn_result_t ucn_service_fifo_take(ucn_service_message_t *messages,
                                          uint8_t capacity,
                                          uint8_t *head,
                                          uint8_t *count,
                                          ucn_service_message_t *message)
{
    if (*count == 0U) {
        return UCN_ERR_NOT_FOUND;
    }
    *message = messages[*head];
    *head = (uint8_t)((*head + 1U) % capacity);
    (*count)--;
    return UCN_OK;
}

/*
 * EN: Forwards or delivers `deliver_to_binding` through the bounded Service Router path.
 * 中文：通过有界的 Service Router 路径转发或投递 `deliver_to_binding`。
 */
static ucn_result_t ucn_service_deliver_to_binding(ucn_service_router_t *router,
                                                    uint8_t binding_index,
                                                    const ucn_service_message_t *message)
{
    const ucn_service_binding_t *binding = &router->config.bindings[binding_index];
    const ucn_service_binding_state_t *state = &router->binding_states[binding_index];

    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
        ucn_service_q0_inbox_t *inbox =
            &router->q0_inboxes[state->q0_inbox_index];
        const ucn_result_t result = ucn_service_fifo_push(
            inbox->messages, UCN_SERVICE_Q0_INBOX_DEPTH, &inbox->tail,
            &inbox->count, message);

        if (result != UCN_OK) {
            router->stats.q0_inbox_full++;
        }
        return result;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q1_LATEST) {
        if (router->q1_inboxes[state->q1_inbox_index].occupied) {
            router->stats.q1_overwrites++;
        }
        router->q1_inboxes[state->q1_inbox_index].latest = *message;
        router->q1_inboxes[state->q1_inbox_index].occupied = true;
        return UCN_OK;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q2_FIFO) {
        ucn_service_q2_inbox_t *inbox =
            &router->q2_inboxes[state->q2_inbox_index];
        const ucn_result_t result = ucn_service_fifo_push(
            inbox->messages, UCN_SERVICE_Q2_INBOX_DEPTH, &inbox->tail,
            &inbox->count, message);

        if (result != UCN_OK) {
            router->stats.q2_inbox_full++;
        }
        return result;
    }
    {
        ucn_service_q3_inbox_t *inbox =
            &router->q3_inboxes[state->q3_inbox_index];
        const ucn_result_t result = ucn_service_fifo_push(
            inbox->messages, UCN_SERVICE_Q3_INBOX_DEPTH, &inbox->tail,
            &inbox->count, message);

        if (result != UCN_OK) {
            router->stats.q3_inbox_full++;
        }
        return result;
    }
}

/*
 * EN: Searches bounded Service Router state for `remote_q1`.
 * 中文：在固定容量的 Service Router 状态中查找 `remote_q1`。
 */
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

/*
 * EN: Searches bounded Service Router state for `free_remote_q1`.
 * 中文：在固定容量的 Service Router 状态中查找 `free_remote_q1`。
 */
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

/*
 * EN: Initializes `router_init` for Service Router using caller-owned fixed storage.
 * 中文：使用调用方提供的固定存储初始化 Service Router 的 `router_init`。
 */
ucn_result_t ucn_service_router_init(ucn_service_router_t *router,
                                     const ucn_service_router_config_t *config)
{
    uint8_t index;
    uint8_t q0_count = 0U;
    uint8_t q1_count = 0U;
    uint8_t q2_count = 0U;
    uint8_t q3_count = 0U;

    if (router == NULL || config == NULL || config->bindings == NULL ||
        config->binding_count == 0U || config->binding_count > UCN_SERVICE_MAX_BINDINGS ||
        config->local_node_id == 0U || config->local_node_id == UCN_NODE_BROADCAST) {
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
        router->binding_states[index].q2_inbox_index = UCN_SERVICE_BINDING_INDEX_NONE;
        router->binding_states[index].q3_inbox_index = UCN_SERVICE_BINDING_INDEX_NONE;
        if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
            if (q0_count >= UCN_SERVICE_MAX_Q0_BINDINGS) {
                (void)memset(router, 0, sizeof(*router));
                return UCN_ERR_NO_SPACE;
            }
            router->binding_states[index].q0_inbox_index = q0_count++;
        } else if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q1_LATEST) {
            if (q1_count >= UCN_SERVICE_MAX_Q1_BINDINGS) {
                (void)memset(router, 0, sizeof(*router));
                return UCN_ERR_NO_SPACE;
            }
            router->binding_states[index].q1_inbox_index = q1_count++;
        } else if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q2_FIFO) {
            if (q2_count >= UCN_SERVICE_MAX_Q2_BINDINGS) {
                (void)memset(router, 0, sizeof(*router));
                return UCN_ERR_NO_SPACE;
            }
            router->binding_states[index].q2_inbox_index = q2_count++;
        } else {
            if (q3_count >= UCN_SERVICE_MAX_Q3_BINDINGS) {
                (void)memset(router, 0, sizeof(*router));
                return UCN_ERR_NO_SPACE;
            }
            router->binding_states[index].q3_inbox_index = q3_count++;
        }
    }
    return UCN_OK;
}

/*
 * EN: Validates and sets `ready` in Service Router state.
 * 中文：验证并设置 Service Router 状态中的 `ready`。
 */
ucn_result_t ucn_service_set_ready(ucn_service_router_t *router,
                                   ucn_endpoint_t endpoint,
                                   bool ready)
{
    const int binding_index = router == NULL ? -1 : ucn_service_find_binding(router, endpoint);
    const ucn_service_binding_t *binding;
    ucn_service_binding_state_t *state;

    if (router == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (binding_index < 0) {
        router->stats.unknown_endpoint++;
        return UCN_ERR_NOT_FOUND;
    }
    binding = &router->config.bindings[binding_index];
    state = &router->binding_states[binding_index];
    if (!ready) {
        if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
            (void)memset(&router->q0_inboxes[state->q0_inbox_index], 0,
                         sizeof(router->q0_inboxes[state->q0_inbox_index]));
        } else if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q1_LATEST) {
            (void)memset(&router->q1_inboxes[state->q1_inbox_index], 0,
                         sizeof(router->q1_inboxes[state->q1_inbox_index]));
        } else if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q2_FIFO) {
            (void)memset(&router->q2_inboxes[state->q2_inbox_index], 0,
                         sizeof(router->q2_inboxes[state->q2_inbox_index]));
        } else {
            (void)memset(&router->q3_inboxes[state->q3_inbox_index], 0,
                         sizeof(router->q3_inboxes[state->q3_inbox_index]));
        }
        router->stats.binding_purges++;
    }
    state->ready = ready;
    return UCN_OK;
}

/*
 * EN: Validates and submits `send_ex` through the bounded Service Router transmit path.
 * 中文：验证 `send_ex` 并将其提交到有界的 Service Router 发送路径。
 */
ucn_result_t ucn_service_send_ex(ucn_service_router_t *router,
                                 ucn_node_id_t destination,
                                 ucn_service_id_t source_service_id,
                                 ucn_endpoint_t endpoint,
                                 ucn_traffic_class_t traffic_class,
                                 const uint8_t *payload,
                                 uint16_t payload_length,
                                 ucn_service_acceptance_t *acceptance)
{
    const int binding_index = router == NULL ? -1 : ucn_service_find_binding(router, endpoint);
    const ucn_service_binding_t *binding;
    ucn_service_message_t message;
    ucn_result_t result;

    if (acceptance != NULL) {
        *acceptance = UCN_SERVICE_ACCEPTANCE_NONE;
    }
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
            if (acceptance != NULL) {
                *acceptance = UCN_SERVICE_ACCEPTANCE_LOCAL_DELIVERED;
            }
        }
        return result;
    }

    if (traffic_class == UCN_TRAFFIC_Q0_CRITICAL) {
        result = ucn_service_fifo_push(
            router->remote_q0.messages, UCN_SERVICE_REMOTE_TX_Q0_DEPTH,
            &router->remote_q0.tail, &router->remote_q0.count, &message);
        if (result != UCN_OK) {
            router->stats.remote_q0_full++;
            return result;
        }
    } else if (traffic_class == UCN_TRAFFIC_Q1_REALTIME) {
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
    } else if (traffic_class == UCN_TRAFFIC_Q2_NORMAL) {
        result = ucn_service_fifo_push(
            router->remote_q2.messages, UCN_SERVICE_REMOTE_TX_Q2_DEPTH,
            &router->remote_q2.tail, &router->remote_q2.count, &message);
        if (result != UCN_OK) {
            router->stats.remote_q2_full++;
            return result;
        }
    } else {
        result = ucn_service_fifo_push(
            router->remote_q3.messages, UCN_SERVICE_REMOTE_TX_Q3_DEPTH,
            &router->remote_q3.tail, &router->remote_q3.count, &message);
        if (result != UCN_OK) {
            router->stats.remote_q3_full++;
            return result;
        }
    }
    router->stats.remote_enqueued++;
    if (acceptance != NULL) {
        *acceptance = UCN_SERVICE_ACCEPTANCE_REMOTE_ENQUEUED;
    }
    return UCN_OK;
}

/*
 * EN: Validates and submits `send` through the bounded Service Router transmit path.
 * 中文：验证 `send` 并将其提交到有界的 Service Router 发送路径。
 */
ucn_result_t ucn_service_send(ucn_service_router_t *router,
                              ucn_node_id_t destination,
                              ucn_service_id_t source_service_id,
                              ucn_endpoint_t endpoint,
                              ucn_traffic_class_t traffic_class,
                              const uint8_t *payload,
                              uint16_t payload_length)
{
    return ucn_service_send_ex(router, destination, source_service_id, endpoint,
                               traffic_class, payload, payload_length, NULL);
}

/*
 * EN: Forwards or delivers `deliver_remote` through the bounded Service Router path.
 * 中文：通过有界的 Service Router 路径转发或投递 `deliver_remote`。
 */
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

/*
 * EN: Removes and returns `inbox_take` from bounded Service Router storage.
 * 中文：从固定容量的 Service Router 存储中移除并返回 `inbox_take`。
 */
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
    if (!state->ready) {
        router->stats.not_ready++;
        return UCN_ERR_NOT_FOUND;
    }
    if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q0_FIFO) {
        ucn_service_q0_inbox_t *inbox =
            &router->q0_inboxes[state->q0_inbox_index];
        result = ucn_service_fifo_take(
            inbox->messages, UCN_SERVICE_Q0_INBOX_DEPTH, &inbox->head,
            &inbox->count, message);
    } else if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q1_LATEST) {
        if (!router->q1_inboxes[state->q1_inbox_index].occupied) {
            result = UCN_ERR_NOT_FOUND;
        } else {
            *message = router->q1_inboxes[state->q1_inbox_index].latest;
            router->q1_inboxes[state->q1_inbox_index].occupied = false;
            result = UCN_OK;
        }
    } else if (binding->delivery_mode == UCN_SERVICE_DELIVERY_Q2_FIFO) {
        ucn_service_q2_inbox_t *inbox =
            &router->q2_inboxes[state->q2_inbox_index];
        result = ucn_service_fifo_take(
            inbox->messages, UCN_SERVICE_Q2_INBOX_DEPTH, &inbox->head,
            &inbox->count, message);
    } else {
        ucn_service_q3_inbox_t *inbox =
            &router->q3_inboxes[state->q3_inbox_index];
        result = ucn_service_fifo_take(
            inbox->messages, UCN_SERVICE_Q3_INBOX_DEPTH, &inbox->head,
            &inbox->count, message);
    }
    if (result == UCN_OK) {
        router->stats.inbox_reads++;
    }
    return result;
}

/* Keep Service-to-Node handoff ordering identical to the Node queue weights. */
#define UCN_SERVICE_REMOTE_SCHEDULE_LENGTH ((uint8_t)12U)
static const ucn_traffic_class_t
    ucn_service_remote_schedule[UCN_SERVICE_REMOTE_SCHEDULE_LENGTH] = {
        UCN_TRAFFIC_Q0_CRITICAL,
        UCN_TRAFFIC_Q1_REALTIME,
        UCN_TRAFFIC_Q0_CRITICAL,
        UCN_TRAFFIC_Q2_NORMAL,
        UCN_TRAFFIC_Q0_CRITICAL,
        UCN_TRAFFIC_Q1_REALTIME,
        UCN_TRAFFIC_Q0_CRITICAL,
        UCN_TRAFFIC_Q3_BULK,
        UCN_TRAFFIC_Q0_CRITICAL,
        UCN_TRAFFIC_Q1_REALTIME,
        UCN_TRAFFIC_Q0_CRITICAL,
        UCN_TRAFFIC_Q2_NORMAL
    };

/*
 * EN: Takes one remote message from a specific Service traffic class.
 * 中文：从指定 Service 业务等级取出一条远端消息。
 */
static ucn_result_t ucn_service_remote_take_class(
    ucn_service_router_t *router,
    ucn_traffic_class_t traffic_class,
    ucn_service_message_t *message)
{
    uint8_t offset;

    if (traffic_class == UCN_TRAFFIC_Q0_CRITICAL) {
        return ucn_service_fifo_take(
            router->remote_q0.messages, UCN_SERVICE_REMOTE_TX_Q0_DEPTH,
            &router->remote_q0.head, &router->remote_q0.count, message);
    }
    if (traffic_class == UCN_TRAFFIC_Q1_REALTIME) {
        for (offset = 0U; offset < UCN_SERVICE_REMOTE_TX_Q1_DEPTH; ++offset) {
            const uint8_t index = (uint8_t)(
                ((uint32_t)router->remote_q1_cursor + (uint32_t)offset) %
                (uint32_t)UCN_SERVICE_REMOTE_TX_Q1_DEPTH);

            if (router->remote_q1[index].occupied) {
                *message = router->remote_q1[index].message;
                router->remote_q1[index].occupied = false;
                router->remote_q1_cursor = (uint8_t)(
                    ((uint32_t)index + UINT32_C(1)) %
                    (uint32_t)UCN_SERVICE_REMOTE_TX_Q1_DEPTH);
                return UCN_OK;
            }
        }
        return UCN_ERR_NOT_FOUND;
    }
    if (traffic_class == UCN_TRAFFIC_Q2_NORMAL) {
        return ucn_service_fifo_take(
            router->remote_q2.messages, UCN_SERVICE_REMOTE_TX_Q2_DEPTH,
            &router->remote_q2.head, &router->remote_q2.count, message);
    }
    return ucn_service_fifo_take(
        router->remote_q3.messages, UCN_SERVICE_REMOTE_TX_Q3_DEPTH,
        &router->remote_q3.head, &router->remote_q3.count, message);
}

/*
 * EN: Removes and returns `remote_tx_take` from bounded Service Router storage.
 * 中文：从固定容量的 Service Router 存储中移除并返回 `remote_tx_take`。
 */
ucn_result_t ucn_service_remote_tx_take(ucn_service_router_t *router,
                                        ucn_service_message_t *message)
{
    uint8_t offset;

    if (router == NULL || message == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    for (offset = 0U; offset < UCN_SERVICE_REMOTE_SCHEDULE_LENGTH; ++offset) {
        const uint8_t schedule_index = (uint8_t)(
            (router->remote_schedule_cursor + offset) %
            UCN_SERVICE_REMOTE_SCHEDULE_LENGTH);
        const ucn_traffic_class_t traffic_class =
            ucn_service_remote_schedule[schedule_index];
        const ucn_result_t result = ucn_service_remote_take_class(
            router, traffic_class, message);

        if (result == UCN_OK) {
            router->remote_schedule_cursor = (uint8_t)(
                (schedule_index + 1U) % UCN_SERVICE_REMOTE_SCHEDULE_LENGTH);
            router->stats.remote_tx_reads++;
            router->stats.remote_tx_reads_by_class[(uint8_t)traffic_class]++;
            return UCN_OK;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Returns the current `stats` view from Service Router state.
 * 中文：从 Service Router 状态返回当前 `stats` 视图。
 */
const ucn_service_stats_t *ucn_service_get_stats(const ucn_service_router_t *router)
{
    return router == NULL ? NULL : &router->stats;
}

/*
 * EN: Maps a synchronous Service acceptance result to its asynchronous stage.
 * 中文：把同步 Service 接受结果映射为对应的异步阶段。
 */
ucn_service_async_stage_t ucn_service_acceptance_stage(
    ucn_service_acceptance_t acceptance)
{
    if (acceptance == UCN_SERVICE_ACCEPTANCE_LOCAL_DELIVERED) {
        return UCN_SERVICE_STAGE_LOCAL_INBOXED;
    }
    if (acceptance == UCN_SERVICE_ACCEPTANCE_REMOTE_ENQUEUED) {
        return UCN_SERVICE_STAGE_REMOTE_ROUTER_QUEUED;
    }
    return UCN_SERVICE_STAGE_NONE;
}

/*
 * EN: Encodes `command_guard_encode` into its bounded Service Router wire representation.
 * 中文：把 `command_guard_encode` 编码为有界的 Service Router 线格式。
 */
ucn_result_t ucn_service_command_guard_encode(
    const ucn_service_command_guard_t *guard,
    uint8_t output[UCN_SERVICE_COMMAND_GUARD_BYTES])
{
    if (guard == NULL || output == NULL || guard->command_id == 0U ||
        guard->valid_for_ms == 0U ||
        !ucn_endpoint_is_static(guard->result_endpoint) || guard->flags != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    output[0] = (uint8_t)(guard->command_id >> 24U);
    output[1] = (uint8_t)(guard->command_id >> 16U);
    output[2] = (uint8_t)(guard->command_id >> 8U);
    output[3] = (uint8_t)guard->command_id;
    output[4] = (uint8_t)(guard->issued_at_ms >> 24U);
    output[5] = (uint8_t)(guard->issued_at_ms >> 16U);
    output[6] = (uint8_t)(guard->issued_at_ms >> 8U);
    output[7] = (uint8_t)guard->issued_at_ms;
    output[8] = (uint8_t)(guard->valid_for_ms >> 8U);
    output[9] = (uint8_t)guard->valid_for_ms;
    output[10] = guard->result_endpoint;
    output[11] = guard->flags;
    return UCN_OK;
}

/*
 * EN: Decodes and validates `command_guard_decode` from its Service Router wire representation.
 * 中文：从 Service Router 线格式解码并验证 `command_guard_decode`。
 */
ucn_result_t ucn_service_command_guard_decode(
    const uint8_t *payload,
    size_t payload_length,
    ucn_service_command_guard_t *guard)
{
    if (payload == NULL || guard == NULL ||
        payload_length < UCN_SERVICE_COMMAND_GUARD_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    guard->command_id = ((uint32_t)payload[0] << 24U) |
                        ((uint32_t)payload[1] << 16U) |
                        ((uint32_t)payload[2] << 8U) | (uint32_t)payload[3];
    guard->issued_at_ms = ((uint32_t)payload[4] << 24U) |
                          ((uint32_t)payload[5] << 16U) |
                          ((uint32_t)payload[6] << 8U) | (uint32_t)payload[7];
    guard->valid_for_ms = (uint16_t)(((uint16_t)payload[8] << 8U) |
                                     (uint16_t)payload[9]);
    guard->result_endpoint = payload[10];
    guard->flags = payload[11];
    if (guard->command_id == 0U || guard->valid_for_ms == 0U ||
        !ucn_endpoint_is_static(guard->result_endpoint) || guard->flags != 0U) {
        return UCN_ERR_MALFORMED;
    }
    return UCN_OK;
}

/*
 * EN: Validates a command against the bounded Service idempotency guard.
 * 中文：使用固定容量的 Service 幂等 Guard 验证命令。
 */
ucn_result_t ucn_service_command_guard_validate(
    const ucn_service_command_guard_t *guard,
    uint32_t now_ms,
    bool has_last_command_id,
    uint32_t last_command_id)
{
    if (guard == NULL || guard->command_id == 0U || guard->valid_for_ms == 0U ||
        !ucn_endpoint_is_static(guard->result_endpoint) || guard->flags != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (has_last_command_id &&
        (int32_t)(guard->command_id - last_command_id) <= 0) {
        return UCN_ERR_REPLAY;
    }
    if ((uint32_t)(now_ms - guard->issued_at_ms) >= guard->valid_for_ms) {
        return UCN_ERR_TTL;
    }
    return UCN_OK;
}

/*
 * EN: Checks whether `result_header` satisfies the Service Router module's validity rules.
 * 中文：检查 `result_header` 是否满足 Service Router 模块的合法性规则。
 */
static bool ucn_service_result_header_is_valid(
    const ucn_service_result_header_t *header)
{
    if (header == NULL || header->command_id == 0U) {
        return false;
    }
    if (header->stage == UCN_SERVICE_STAGE_REMOTE_INBOXED) {
        return header->status == UCN_SERVICE_RESULT_ACCEPTED;
    }
    if (header->stage != UCN_SERVICE_STAGE_REMOTE_EXECUTED) {
        return false;
    }
    return header->status >= UCN_SERVICE_RESULT_SUCCEEDED &&
           header->status <= UCN_SERVICE_RESULT_EXPIRED;
}

/*
 * EN: Encodes `result_header_encode` into its bounded Service Router wire representation.
 * 中文：把 `result_header_encode` 编码为有界的 Service Router 线格式。
 */
ucn_result_t ucn_service_result_header_encode(
    const ucn_service_result_header_t *header,
    uint8_t output[UCN_SERVICE_RESULT_HEADER_BYTES])
{
    if (!ucn_service_result_header_is_valid(header) || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    output[0] = (uint8_t)(header->command_id >> 24U);
    output[1] = (uint8_t)(header->command_id >> 16U);
    output[2] = (uint8_t)(header->command_id >> 8U);
    output[3] = (uint8_t)header->command_id;
    output[4] = (uint8_t)header->stage;
    output[5] = (uint8_t)header->status;
    output[6] = (uint8_t)(header->detail_code >> 8U);
    output[7] = (uint8_t)header->detail_code;
    return UCN_OK;
}

/*
 * EN: Decodes and validates `result_header_decode` from its Service Router wire representation.
 * 中文：从 Service Router 线格式解码并验证 `result_header_decode`。
 */
ucn_result_t ucn_service_result_header_decode(
    const uint8_t *payload,
    size_t payload_length,
    ucn_service_result_header_t *header)
{
    if (payload == NULL || header == NULL ||
        payload_length < UCN_SERVICE_RESULT_HEADER_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    header->command_id = ((uint32_t)payload[0] << 24U) |
                         ((uint32_t)payload[1] << 16U) |
                         ((uint32_t)payload[2] << 8U) | (uint32_t)payload[3];
    header->stage = (ucn_service_async_stage_t)payload[4];
    header->status = (ucn_service_result_status_t)payload[5];
    header->detail_code = (uint16_t)(((uint16_t)payload[6] << 8U) |
                                     (uint16_t)payload[7]);
    return ucn_service_result_header_is_valid(header) ? UCN_OK : UCN_ERR_MALFORMED;
}

/*
 * EN: Checks the `result_matches_command` condition against current Service Router state.
 * 中文：根据当前 Service Router 状态检查 `result_matches_command` 条件。
 */
bool ucn_service_result_matches_command(
    const ucn_service_command_guard_t *command,
    ucn_endpoint_t received_endpoint,
    const ucn_service_result_header_t *result)
{
    return command != NULL && result != NULL &&
           command->command_id != 0U &&
           command->valid_for_ms != 0U && command->flags == 0U &&
           ucn_endpoint_is_static(command->result_endpoint) &&
           received_endpoint == command->result_endpoint &&
           result->command_id == command->command_id &&
           ucn_service_result_header_is_valid(result);
}
