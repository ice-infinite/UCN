#include <string.h>

#include "ucn/ucn_service_bridge.h"
#include "ucn/ucn_node_storage.h"

/*
 * EN: Searches bounded Service Bridge state for `binding`.
 * 中文：在固定容量的 Service Bridge 状态中查找 `binding`。
 */
static const ucn_service_binding_t *ucn_service_protocol_bridge_find_binding(
    const ucn_service_protocol_bridge_t *bridge,
    ucn_endpoint_t endpoint)
{
    uint8_t index;

    if (bridge == NULL || bridge->router == NULL) {
        return NULL;
    }
    for (index = 0U; index < bridge->router->config.binding_count; ++index) {
        const ucn_service_binding_t *binding =
            &bridge->router->config.bindings[index];

        if (binding->endpoint == endpoint) {
            return binding;
        }
    }
    return NULL;
}

static ucn_service_bridge_validator_entry_t *
/*
 * EN: Searches bounded Service Bridge state for `validator`.
 * 中文：在固定容量的 Service Bridge 状态中查找 `validator`。
 */
ucn_service_protocol_bridge_find_validator(
    ucn_service_protocol_bridge_t *bridge,
    ucn_endpoint_t endpoint)
{
    uint8_t index;

    for (index = 0U; index < UCN_SERVICE_BRIDGE_MAX_VALIDATORS; ++index) {
        ucn_service_bridge_validator_entry_t *entry = &bridge->validators[index];

        if (entry->occupied && entry->endpoint == endpoint) {
            return entry;
        }
    }
    return NULL;
}

/*
 * EN: Completes `complete_inbound` and records its terminal Service Bridge result.
 * 中文：完成 `complete_inbound` 并记录其 Service Bridge 终态结果。
 */
static void ucn_service_protocol_bridge_complete_inbound(
    ucn_service_protocol_bridge_t *bridge,
    const ucn_frame_t *frame,
    ucn_result_t result)
{
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

/*
 * EN: Receives a Core Endpoint frame and submits it to the Service Bridge ingress path.
 * 中文：接收 Core Endpoint 帧并提交到 Service Bridge 入站路径。
 */
static void ucn_service_protocol_bridge_endpoint_rx(void *context,
                                                    const ucn_frame_t *frame)
{
    ucn_service_protocol_bridge_t *bridge = (ucn_service_protocol_bridge_t *)context;
    const ucn_service_binding_t *binding;
    ucn_service_bridge_validator_entry_t *validator_entry;
    ucn_result_t result;

    if (bridge == NULL || frame == NULL) {
        return;
    }
    binding = ucn_service_protocol_bridge_find_binding(
        bridge, (ucn_endpoint_t)frame->message_type);
    validator_entry = ucn_service_protocol_bridge_find_validator(
        bridge, (ucn_endpoint_t)frame->message_type);
    if (binding == NULL) {
        ucn_service_protocol_bridge_complete_inbound(
            bridge, frame, UCN_ERR_CONFIG);
        return;
    }
    if (validator_entry == NULL && binding->require_remote_q0_validator) {
        bridge->stats.inbound_validator_missing++;
        ucn_service_protocol_bridge_complete_inbound(
            bridge, frame, UCN_ERR_CONFIG);
        return;
    }
    if (validator_entry != NULL) {
        bridge->stats.inbound_validator_checked++;
        result = validator_entry->validator(
            validator_entry->context, frame, frame->source, frame->session_id,
            (ucn_endpoint_t)frame->message_type, frame->payload,
            frame->payload_length, bridge->node->now_ms);
        if (result != UCN_OK) {
            bridge->stats.inbound_validator_rejected++;
            ucn_service_protocol_bridge_complete_inbound(bridge, frame, result);
            return;
        }
        bridge->stats.inbound_validator_accepted++;
    }
    if (bridge->inbound_hooks.lock != NULL) {
        bridge->inbound_hooks.lock(bridge->inbound_hooks.context);
    }
    result = ucn_service_deliver_remote(bridge->router, frame);
    if (bridge->inbound_hooks.unlock != NULL) {
        bridge->inbound_hooks.unlock(bridge->inbound_hooks.context);
    }
    ucn_service_protocol_bridge_complete_inbound(bridge, frame, result);
}

/*
 * EN: Checks the `binding_has_foreign_handler` condition against current Service Bridge state.
 * 中文：根据当前 Service Bridge 状态检查 `binding_has_foreign_handler` 条件。
 */
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

/*
 * EN: Initializes the Service Bridge object from validated caller-owned configuration without heap allocation.
 * 中文：使用经验证的调用方配置初始化 Service Bridge 对象，且不使用堆内存。
 */
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

/*
 * EN: Validates and sets `validator` in Service Bridge state.
 * 中文：验证并设置 Service Bridge 状态中的 `validator`。
 */
ucn_result_t ucn_service_protocol_bridge_set_validator(
    ucn_service_protocol_bridge_t *bridge,
    ucn_endpoint_t endpoint,
    ucn_service_bridge_validator_fn validator,
    void *context)
{
    const ucn_service_binding_t *binding;
    ucn_service_bridge_validator_entry_t *entry;
    uint8_t index;

    if (bridge == NULL || bridge->router == NULL || bridge->node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (bridge->endpoint_handlers_installed) {
        return UCN_ERR_CONFIG;
    }
    binding = ucn_service_protocol_bridge_find_binding(bridge, endpoint);
    if (binding == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (!binding->accept_remote) {
        return UCN_ERR_CONFIG;
    }

    entry = ucn_service_protocol_bridge_find_validator(bridge, endpoint);
    if (validator == NULL) {
        if (entry == NULL) {
            return UCN_ERR_NOT_FOUND;
        }
        (void)memset(entry, 0, sizeof(*entry));
        return UCN_OK;
    }
    if (entry != NULL) {
        entry->validator = validator;
        entry->context = context;
        return UCN_OK;
    }
    for (index = 0U; index < UCN_SERVICE_BRIDGE_MAX_VALIDATORS; ++index) {
        if (!bridge->validators[index].occupied) {
            bridge->validators[index].occupied = true;
            bridge->validators[index].endpoint = endpoint;
            bridge->validators[index].validator = validator;
            bridge->validators[index].context = context;
            return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

/*
 * EN: Validates and installs `install_endpoint_handlers` into bounded Service Bridge state.
 * 中文：验证 `install_endpoint_handlers` 并将其安装到固定容量的 Service Bridge 状态中。
 */
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
        const ucn_service_binding_t *binding =
            &bridge->router->config.bindings[binding_index];
        bool already_owned;

        if (binding->require_remote_q0_validator &&
            ucn_service_protocol_bridge_find_validator(
                bridge, binding->endpoint) == NULL) {
            return UCN_ERR_CONFIG;
        }
        if (ucn_service_protocol_bridge_binding_has_foreign_handler(
                bridge, binding->endpoint, &already_owned)) {
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

/*
 * EN: Validates and sets `inbound_hooks` in Service Bridge state.
 * 中文：验证并设置 Service Bridge 状态中的 `inbound_hooks`。
 */
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

/*
 * EN: Validates and sets `q0_backpressure_policy` in Service Bridge state.
 * 中文：验证并设置 Service Bridge 状态中的 `q0_backpressure_policy`。
 */
ucn_result_t ucn_service_protocol_bridge_set_q0_backpressure_policy(
    ucn_service_protocol_bridge_t *bridge,
    const ucn_service_bridge_q0_backpressure_policy_t *policy)
{
    if (bridge == NULL || bridge->router == NULL || bridge->node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (bridge->q0_pending.occupied) {
        return UCN_ERR_CONFIG;
    }
    if (policy == NULL) {
        bridge->q0_backpressure_retry_enabled = false;
        (void)memset(&bridge->q0_backpressure_policy, 0,
                     sizeof(bridge->q0_backpressure_policy));
        return UCN_OK;
    }
    if (policy->max_retries == 0U ||
        !ucn_duration_is_valid(policy->retry_interval_ms) ||
        !ucn_duration_is_valid(policy->timeout_ms) ||
        policy->retry_interval_ms >= policy->timeout_ms) {
        return UCN_ERR_CONFIG;
    }
    bridge->q0_backpressure_policy = *policy;
    bridge->q0_backpressure_retry_enabled = true;
    return UCN_OK;
}

/*
 * EN: Validates and sets `outbound_observer` in Service Bridge state.
 * 中文：验证并设置 Service Bridge 状态中的 `outbound_observer`。
 */
ucn_result_t ucn_service_protocol_bridge_set_outbound_observer(
    ucn_service_protocol_bridge_t *bridge,
    ucn_service_bridge_outbound_observer_fn observer,
    void *context)
{
    if (bridge == NULL || bridge->router == NULL || bridge->node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    bridge->outbound_observer = observer;
    bridge->outbound_observer_context = observer == NULL ? NULL : context;
    return UCN_OK;
}

/*
 * EN: Validates and sets `outbound_event_observer` in Service Bridge state.
 * 中文：验证并设置 Service Bridge 状态中的 `outbound_event_observer`。
 */
ucn_result_t ucn_service_protocol_bridge_set_outbound_event_observer(
    ucn_service_protocol_bridge_t *bridge,
    ucn_service_bridge_outbound_event_observer_fn observer,
    void *context)
{
    if (bridge == NULL || bridge->router == NULL || bridge->node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    bridge->outbound_event_observer = observer;
    bridge->outbound_event_observer_context = observer == NULL ? NULL : context;
    return UCN_OK;
}

/*
 * EN: Completes `complete_outbound` and records its terminal Service Bridge result.
 * 中文：完成 `complete_outbound` 并记录其 Service Bridge 终态结果。
 */
static void ucn_service_protocol_bridge_complete_outbound(
    ucn_service_protocol_bridge_t *bridge,
    const ucn_service_message_t *message,
    ucn_result_t result,
    ucn_service_bridge_outbound_outcome_t outcome)
{
    const ucn_service_bridge_outbound_event_t event = {
        result == UCN_OK ? UCN_SERVICE_STAGE_LINK_QUEUE_ACCEPTED
                         : UCN_SERVICE_STAGE_NONE,
        outcome,
        result
    };

    bridge->stats.last_tx_result = result;
    if (result == UCN_OK) {
        bridge->stats.remote_tx_accepted++;
    } else {
        bridge->stats.remote_tx_failed++;
    }
    if (bridge->outbound_observer != NULL) {
        bridge->outbound_observer(bridge->outbound_observer_context,
                                  message, result);
    }
    if (bridge->outbound_event_observer != NULL) {
        bridge->outbound_event_observer(
            bridge->outbound_event_observer_context, message, &event);
    }
}

/*
 * EN: Copies or submits `submit` to a bounded Service Bridge queue.
 * 中文：把 `submit` 复制或提交到固定容量的 Service Bridge 队列。
 */
static ucn_result_t ucn_service_protocol_bridge_submit(
    ucn_service_protocol_bridge_t *bridge,
    ucn_service_message_t *message,
    uint32_t now_ms,
    bool retry_owned)
{
    const uint32_t error_drops_before = bridge->node->stats.tx_error_dropped;
    ucn_result_t result;

    bridge->stats.remote_tx_attempted++;
    result = ucn_node_send_endpoint(bridge->node, message->destination_node_id,
                                    message->endpoint, message->traffic_class,
                                    message->payload, message->payload_length);
    bridge->stats.last_tx_result = result;
    if (result == UCN_OK) {
        ucn_service_protocol_bridge_complete_outbound(
            bridge, message, result, UCN_SERVICE_OUTBOUND_LINK_QUEUE_ACCEPTED);
        if (retry_owned) {
            bridge->q0_pending.occupied = false;
        }
        return result;
    }

    if (retry_owned && result == UCN_ERR_NO_SPACE) {
        const bool retry_budget_available =
            bridge->q0_pending.retries <
            bridge->q0_backpressure_policy.max_retries;
        const bool retry_fits_deadline =
            !ucn_deadline_due_within(
                now_ms, bridge->q0_pending.deadline_ms,
                bridge->q0_backpressure_policy.retry_interval_ms);

        if (retry_budget_available && retry_fits_deadline) {
            /* The Bridge still owns the only pending copy, so a failed Link
             * attempt is not yet a final Core drop. */
            bridge->node->stats.tx_error_dropped = error_drops_before;
            bridge->q0_pending.retries++;
            bridge->q0_pending.next_attempt_ms = ucn_deadline_from_now(
                now_ms, bridge->q0_backpressure_policy.retry_interval_ms);
            bridge->stats.q0_backpressure_retries++;
            return result;
        }
        bridge->stats.q0_backpressure_exhausted++;
    } else if (retry_owned) {
        bridge->stats.q0_backpressure_terminal_failed++;
    }

    ucn_service_protocol_bridge_complete_outbound(
        bridge, message, result,
        result == UCN_ERR_NO_SPACE
            ? (retry_owned ? UCN_SERVICE_OUTBOUND_BACKPRESSURE_EXHAUSTED
                           : UCN_SERVICE_OUTBOUND_BACKPRESSURE_REJECTED)
            : UCN_SERVICE_OUTBOUND_TERMINAL_FAILED);
    if (retry_owned) {
        bridge->q0_pending.occupied = false;
    }
    return result;
}

/*
 * EN: Advances one bounded `step` state-machine step in Service Bridge.
 * 中文：在 Service Bridge 中推进一次有界的 `step` 状态机步骤。
 */
ucn_result_t ucn_service_protocol_bridge_step(
    ucn_service_protocol_bridge_t *bridge,
    uint8_t max_requests,
    uint8_t *processed)
{
    if (bridge == NULL || bridge->node == NULL) {
        if (processed != NULL) {
            *processed = 0U;
        }
        return UCN_ERR_ARGUMENT;
    }
    return ucn_service_protocol_bridge_step_at(
        bridge, bridge->node->now_ms, max_requests, processed);
}

/*
 * EN: Advances one bounded `step_at` state-machine step in Service Bridge.
 * 中文：在 Service Bridge 中推进一次有界的 `step_at` 状态机步骤。
 */
ucn_result_t ucn_service_protocol_bridge_step_at(
    ucn_service_protocol_bridge_t *bridge,
    uint32_t now_ms,
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
        ucn_service_message_t *message_to_submit = &message;
        bool retry_owned = false;
        ucn_result_t result;

        if (bridge->q0_pending.occupied) {
            retry_owned = true;
            message_to_submit = &bridge->q0_pending.message;
            if (ucn_deadline_expired(now_ms, bridge->q0_pending.deadline_ms)) {
                bridge->stats.q0_backpressure_expired++;
                ucn_service_protocol_bridge_complete_outbound(
                    bridge, message_to_submit, UCN_ERR_TTL,
                    UCN_SERVICE_OUTBOUND_EXPIRED);
                bridge->q0_pending.occupied = false;
                count++;
                continue;
            }
            if (bridge->q0_pending.next_attempt_ms != 0U &&
                !ucn_deadline_expired(now_ms,
                                      bridge->q0_pending.next_attempt_ms)) {
                break;
            }
        } else {
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

            if (bridge->q0_backpressure_retry_enabled &&
                message.traffic_class == UCN_TRAFFIC_Q0_CRITICAL) {
                bridge->q0_pending.occupied = true;
                bridge->q0_pending.retries = 0U;
                bridge->q0_pending.next_attempt_ms = 0U;
                bridge->q0_pending.deadline_ms = ucn_deadline_from_now(
                    now_ms, bridge->q0_backpressure_policy.timeout_ms);
                bridge->q0_pending.message = message;
                retry_owned = true;
                message_to_submit = &bridge->q0_pending.message;
            }
        }

        result = ucn_service_protocol_bridge_submit(
            bridge, message_to_submit, now_ms, retry_owned);
        count++;
        if (result != UCN_OK) {
            if (processed != NULL) {
                *processed = count;
            }
            return result;
        }
    }

    if (processed != NULL) {
        *processed = count;
    }
    return UCN_OK;
}

/*
 * EN: Returns the current `stats` view from Service Bridge state.
 * 中文：从 Service Bridge 状态返回当前 `stats` 视图。
 */
const ucn_service_bridge_stats_t *ucn_service_protocol_bridge_get_stats(
    const ucn_service_protocol_bridge_t *bridge)
{
    return bridge == NULL ? NULL : &bridge->stats;
}

/*
 * EN: Initializes `bridge_replay_init` for Service Bridge using caller-owned fixed storage.
 * 中文：使用调用方提供的固定存储初始化 Service Bridge 的 `bridge_replay_init`。
 */
void ucn_service_bridge_replay_init(
    ucn_service_bridge_replay_state_t *state)
{
    if (state != NULL) {
        (void)memset(state, 0, sizeof(*state));
    }
}

/*
 * EN: Checks and records a remote command in the Service replay window.
 * 中文：在 Service 重放窗口中检查并记录远端命令。
 */
ucn_result_t ucn_service_bridge_replay_accept_command(
    ucn_service_bridge_replay_state_t *state,
    ucn_node_id_t source_node_id,
    ucn_session_id_t source_session_id,
    ucn_endpoint_t endpoint,
    uint32_t command_id)
{
    ucn_service_bridge_replay_entry_t *free_entry = NULL;
    uint8_t index;

    if (state == NULL || source_node_id == UCN_NODE_BROADCAST ||
        !ucn_endpoint_is_static(endpoint) || command_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_SERVICE_BRIDGE_REPLAY_DEPTH; ++index) {
        ucn_service_bridge_replay_entry_t *entry = &state->entries[index];

        if (!entry->occupied) {
            if (free_entry == NULL) {
                free_entry = entry;
            }
            continue;
        }
        if (entry->source_node_id != source_node_id ||
            entry->endpoint != endpoint) {
            continue;
        }
        if (entry->source_session_id != source_session_id) {
            return UCN_ERR_SECURITY;
        }
        if (entry->has_last_command_id &&
            (int32_t)(command_id - entry->last_command_id) <= 0) {
            return UCN_ERR_REPLAY;
        }
        entry->last_command_id = command_id;
        entry->has_last_command_id = true;
        return UCN_OK;
    }
    if (free_entry == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    free_entry->occupied = true;
    free_entry->has_last_command_id = true;
    free_entry->source_node_id = source_node_id;
    free_entry->source_session_id = source_session_id;
    free_entry->endpoint = endpoint;
    free_entry->last_command_id = command_id;
    return UCN_OK;
}

/*
 * EN: Rotates the Service replay window to a strictly newer peer Session.
 * 中文：把 Service 重放窗口切换到严格更新的对端 Session。
 */
ucn_result_t ucn_service_bridge_replay_rotate_session(
    ucn_service_bridge_replay_state_t *state,
    ucn_node_id_t source_node_id,
    ucn_endpoint_t endpoint,
    ucn_session_id_t old_session_id,
    ucn_session_id_t new_session_id)
{
    uint8_t index;

    if (state == NULL || source_node_id == UCN_NODE_BROADCAST ||
        !ucn_endpoint_is_static(endpoint) || new_session_id == 0U ||
        old_session_id == new_session_id) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_SERVICE_BRIDGE_REPLAY_DEPTH; ++index) {
        ucn_service_bridge_replay_entry_t *entry = &state->entries[index];

        if (!entry->occupied || entry->source_node_id != source_node_id ||
            entry->endpoint != endpoint) {
            continue;
        }
        if (entry->source_session_id != old_session_id) {
            return UCN_ERR_SECURITY;
        }
        entry->source_session_id = new_session_id;
        entry->has_last_command_id = false;
        entry->last_command_id = 0U;
        return UCN_OK;
    }
    return UCN_ERR_NOT_FOUND;
}
