#include <string.h>

#include "ucn/ports/ucn_event_runtime.h"

/*
 * EN: Checks whether `source_events` satisfies the Event Runtime module's validity rules.
 * 中文：检查 `source_events` 是否满足 Event Runtime 模块的合法性规则。
 */
static bool source_events_are_valid(ucn_event_source_events_t events)
{
    return events != 0U &&
           (events & (ucn_event_source_events_t)~UCN_EVENT_SOURCE_PENDING_MASK) ==
               0U;
}

/*
 * EN: Checks whether `owner_events` satisfies the Event Runtime module's validity rules.
 * 中文：检查 `owner_events` 是否满足 Event Runtime 模块的合法性规则。
 */
static bool owner_events_are_valid(ucn_event_owner_events_t events)
{
    return events != 0U &&
           (events & (ucn_event_owner_events_t)~UCN_EVENT_OWNER_PENDING_MASK) ==
               0U;
}

/*
 * EN: Checks whether `source_id` satisfies the Event Runtime module's validity rules.
 * 中文：检查 `source_id` 是否满足 Event Runtime 模块的合法性规则。
 */
static bool source_id_is_valid(ucn_event_source_id_t source_id)
{
    return source_id < UCN_EVENT_RUNTIME_MAX_SOURCES;
}

/*
 * EN: Enters or leaves the bounded `enter_task` critical section for Event Runtime.
 * 中文：进入或退出 Event Runtime 的有界 `enter_task` 临界区。
 */
static void enter_task(ucn_event_runtime_t *runtime)
{
    if (runtime->port_ops->enter_critical != NULL) {
        runtime->port_ops->enter_critical(runtime->port_context);
    }
}

/*
 * EN: Enters or leaves the bounded `exit_task` critical section for Event Runtime.
 * 中文：进入或退出 Event Runtime 的有界 `exit_task` 临界区。
 */
static void exit_task(ucn_event_runtime_t *runtime)
{
    if (runtime->port_ops->exit_critical != NULL) {
        runtime->port_ops->exit_critical(runtime->port_context);
    }
}

/*
 * EN: Checks the `isr_critical_is_configured` condition against current Event Runtime state.
 * 中文：根据当前 Event Runtime 状态检查 `isr_critical_is_configured` 条件。
 */
static bool isr_critical_is_configured(const ucn_event_runtime_t *runtime)
{
    return runtime->port_ops->enter_critical_from_isr != NULL &&
           runtime->port_ops->exit_critical_from_isr != NULL;
}

/*
 * EN: Enters or leaves the bounded `enter_isr` critical section for Event Runtime.
 * 中文：进入或退出 Event Runtime 的有界 `enter_isr` 临界区。
 */
static ucn_port_critical_token_t enter_isr(ucn_event_runtime_t *runtime)
{
    return runtime->port_ops->enter_critical_from_isr(runtime->port_context);
}

/*
 * EN: Enters or leaves the bounded `exit_isr` critical section for Event Runtime.
 * 中文：进入或退出 Event Runtime 的有界 `exit_isr` 临界区。
 */
static void exit_isr(ucn_event_runtime_t *runtime,
                     ucn_port_critical_token_t token)
{
    runtime->port_ops->exit_critical_from_isr(runtime->port_context, token);
}

/*
 * EN: Schedules `scheduler_notify` using the wrap-safe Event Runtime time domain.
 * 中文：使用回绕安全的 Event Runtime 时间域调度 `scheduler_notify`。
 */
static void scheduler_notify(ucn_event_runtime_t *runtime, bool from_isr)
{
    if (runtime->scheduler_ops != NULL) {
        runtime->scheduler_ops->notify_owner(runtime->scheduler_context,
                                             from_isr);
    }
}

/*
 * EN: Initializes the Event Runtime object from validated caller-owned configuration without heap allocation.
 * 中文：使用经验证的调用方配置初始化 Event Runtime 对象，且不使用堆内存。
 */
ucn_result_t ucn_event_runtime_init(
    ucn_event_runtime_t *runtime,
    const ucn_event_runtime_config_t *config)
{
    ucn_result_t result;

    if (runtime == NULL || config == NULL ||
        !ucn_port_ops_is_compatible(config->owner.port_ops) ||
        (config->scheduler_ops != NULL &&
         (config->scheduler_ops->notify_owner == NULL ||
          config->scheduler_ops->wait_owner == NULL))) {
        return UCN_ERR_ARGUMENT;
    }
    if (config->max_drain_rounds > 64U) {
        return UCN_ERR_CONFIG;
    }

    (void)memset(runtime, 0, sizeof(*runtime));
    result = ucn_protocol_owner_init(&runtime->owner, &config->owner);
    if (result != UCN_OK) {
        return result;
    }
    runtime->scheduler_ops = config->scheduler_ops;
    runtime->scheduler_context = config->scheduler_context;
    runtime->port_ops = config->owner.port_ops;
    runtime->port_context = config->owner.port_context;
    runtime->max_drain_rounds = config->max_drain_rounds == 0U ?
        UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS : config->max_drain_rounds;
    runtime->max_source_work_per_round =
        config->max_source_work_per_round == 0U ?
            UCN_EVENT_RUNTIME_DEFAULT_SOURCE_BUDGET :
            config->max_source_work_per_round;
    if (runtime->max_source_work_per_round == 0U) {
        return UCN_ERR_CONFIG;
    }
    runtime->stats.last_source_result = UCN_OK;
    runtime->stats.last_owner_result = UCN_OK;
    runtime->initialized = true;
    return UCN_OK;
}

/*
 * EN: Validates and installs `bind_source` into bounded Event Runtime state.
 * 中文：验证 `bind_source` 并将其安装到固定容量的 Event Runtime 状态中。
 */
ucn_result_t ucn_event_runtime_bind_source(
    ucn_event_runtime_t *runtime,
    ucn_event_source_id_t source_id,
    const ucn_event_source_config_t *config)
{
    ucn_event_runtime_source_t *source;

    if (runtime == NULL || !runtime->initialized || config == NULL ||
        config->ops == NULL || config->ops->service == NULL ||
        !source_id_is_valid(source_id)) {
        return UCN_ERR_ARGUMENT;
    }
    source = &runtime->sources[source_id];
    if (source->occupied) {
        return source->ops == config->ops && source->context == config->context ?
                   UCN_OK : UCN_ERR_CONFIG;
    }
    source->ops = config->ops;
    source->context = config->context;
    source->occupied = true;
    return UCN_OK;
}

/*
 * EN: Records `signal_source` and notifies the bounded Event Runtime owner path.
 * 中文：记录 `signal_source` 并通知有界的 Event Runtime Owner 路径。
 */
static ucn_result_t signal_source(ucn_event_runtime_t *runtime,
                                  ucn_event_source_id_t source_id,
                                  ucn_event_source_events_t events,
                                  bool from_isr)
{
    ucn_port_critical_token_t token = 0U;

    if (runtime == NULL || !runtime->initialized ||
        !source_id_is_valid(source_id) || !source_events_are_valid(events)) {
        return UCN_ERR_ARGUMENT;
    }
    if (!runtime->sources[source_id].occupied) {
        return UCN_ERR_NOT_FOUND;
    }
    if (from_isr && !isr_critical_is_configured(runtime)) {
        return UCN_ERR_CONFIG;
    }

    if (from_isr) {
        token = enter_isr(runtime);
    } else {
        enter_task(runtime);
    }
    runtime->pending_source_events[source_id] |= events;
    runtime->stats.source_signals++;
    if (from_isr) {
        runtime->stats.source_signals_from_isr++;
    }
    runtime->stats.scheduler_notifications +=
        runtime->scheduler_ops != NULL ? 1U : 0U;
    if (from_isr) {
        exit_isr(runtime, token);
    } else {
        exit_task(runtime);
    }
    scheduler_notify(runtime, from_isr);
    return UCN_OK;
}

/*
 * EN: Records `signal_source` and notifies the bounded Event Runtime owner path.
 * 中文：记录 `signal_source` 并通知有界的 Event Runtime Owner 路径。
 */
ucn_result_t ucn_event_runtime_signal_source(
    ucn_event_runtime_t *runtime,
    ucn_event_source_id_t source_id,
    ucn_event_source_events_t events)
{
    return signal_source(runtime, source_id, events, false);
}

/*
 * EN: Records `signal_source_from_isr` and notifies the bounded Event Runtime owner path.
 * 中文：记录 `signal_source_from_isr` 并通知有界的 Event Runtime Owner 路径。
 */
ucn_result_t ucn_event_runtime_signal_source_from_isr(
    ucn_event_runtime_t *runtime,
    ucn_event_source_id_t source_id,
    ucn_event_source_events_t events)
{
    return signal_source(runtime, source_id, events, true);
}

/*
 * EN: Records `signal_owner` and notifies the bounded Event Runtime owner path.
 * 中文：记录 `signal_owner` 并通知有界的 Event Runtime Owner 路径。
 */
static ucn_result_t signal_owner(ucn_event_runtime_t *runtime,
                                 ucn_event_owner_events_t events,
                                 bool from_isr)
{
    ucn_port_critical_token_t token = 0U;

    if (runtime == NULL || !runtime->initialized ||
        !owner_events_are_valid(events)) {
        return UCN_ERR_ARGUMENT;
    }
    if (from_isr && !isr_critical_is_configured(runtime)) {
        return UCN_ERR_CONFIG;
    }

    if (from_isr) {
        token = enter_isr(runtime);
    } else {
        enter_task(runtime);
    }
    runtime->pending_owner_events |= events;
    runtime->stats.owner_signals++;
    if (from_isr) {
        runtime->stats.owner_signals_from_isr++;
    }
    runtime->stats.scheduler_notifications +=
        runtime->scheduler_ops != NULL ? 1U : 0U;
    if (from_isr) {
        exit_isr(runtime, token);
    } else {
        exit_task(runtime);
    }
    scheduler_notify(runtime, from_isr);
    return UCN_OK;
}

/*
 * EN: Records `signal_owner` and notifies the bounded Event Runtime owner path.
 * 中文：记录 `signal_owner` 并通知有界的 Event Runtime Owner 路径。
 */
ucn_result_t ucn_event_runtime_signal_owner(
    ucn_event_runtime_t *runtime,
    ucn_event_owner_events_t events)
{
    return signal_owner(runtime, events, false);
}

/*
 * EN: Records `signal_owner_from_isr` and notifies the bounded Event Runtime owner path.
 * 中文：记录 `signal_owner_from_isr` 并通知有界的 Event Runtime Owner 路径。
 */
ucn_result_t ucn_event_runtime_signal_owner_from_isr(
    ucn_event_runtime_t *runtime,
    ucn_event_owner_events_t events)
{
    return signal_owner(runtime, events, true);
}

/*
 * EN: Updates `record_frame_result` in bounded Event Runtime state.
 * 中文：更新固定容量 Event Runtime 状态中的 `record_frame_result`。
 */
static void record_frame_result(ucn_event_runtime_t *runtime,
                                ucn_result_t result,
                                bool from_isr)
{
    ucn_port_critical_token_t token = 0U;

    if (from_isr && isr_critical_is_configured(runtime)) {
        token = enter_isr(runtime);
    } else if (!from_isr) {
        enter_task(runtime);
    }
    if (result == UCN_OK) {
        runtime->stats.frames_submitted++;
    } else {
        runtime->stats.frames_rejected++;
    }
    if (from_isr && isr_critical_is_configured(runtime)) {
        exit_isr(runtime, token);
    } else if (!from_isr) {
        exit_task(runtime);
    }
}

/*
 * EN: Copies or submits `submit_frame` to a bounded Event Runtime queue.
 * 中文：把 `submit_frame` 复制或提交到固定容量的 Event Runtime 队列。
 */
ucn_result_t ucn_event_runtime_submit_frame(
    ucn_event_runtime_t *runtime,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length)
{
    ucn_result_t result;

    if (runtime == NULL || !runtime->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_protocol_owner_rx_enqueue(&runtime->owner, ingress_link, data,
                                           length);
    record_frame_result(runtime, result, false);
    if (result != UCN_OK) {
        return result;
    }
    return ucn_event_runtime_signal_owner(runtime, UCN_EVENT_OWNER_RX_QUEUE);
}

/*
 * EN: Derives `submit_frame_from_isr` with the canonical Event Runtime conversion rules.
 * 中文：按照规范的 Event Runtime 转换规则推导 `submit_frame_from_isr`。
 */
ucn_result_t ucn_event_runtime_submit_frame_from_isr(
    ucn_event_runtime_t *runtime,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length)
{
    ucn_result_t result;

    if (runtime == NULL || !runtime->initialized ||
        !isr_critical_is_configured(runtime)) {
        return runtime == NULL || !runtime->initialized ? UCN_ERR_ARGUMENT :
                                                           UCN_ERR_CONFIG;
    }
    result = ucn_protocol_owner_rx_enqueue_from_isr(&runtime->owner,
                                                     ingress_link, data,
                                                     length);
    record_frame_result(runtime, result, true);
    if (result != UCN_OK) {
        return result;
    }
    return ucn_event_runtime_signal_owner_from_isr(
        runtime, UCN_EVENT_OWNER_RX_QUEUE);
}

/*
 * EN: Checks the `has_pending_locked` condition in current Event Runtime state.
 * 中文：检查当前 Event Runtime 状态中的 `has_pending_locked` 条件。
 */
static bool has_pending_locked(const ucn_event_runtime_t *runtime)
{
    ucn_event_source_id_t source_id;

    if (runtime->pending_owner_events != 0U) {
        return true;
    }
    for (source_id = 0U; source_id < UCN_EVENT_RUNTIME_MAX_SOURCES;
         ++source_id) {
        if (runtime->pending_source_events[source_id] != 0U) {
            return true;
        }
    }
    return false;
}

/*
 * EN: Checks the `has_pending` condition in current Event Runtime state.
 * 中文：检查当前 Event Runtime 状态中的 `has_pending` 条件。
 */
bool ucn_event_runtime_has_pending(ucn_event_runtime_t *runtime)
{
    bool pending;

    if (runtime == NULL || !runtime->initialized) {
        return false;
    }
    enter_task(runtime);
    pending = has_pending_locked(runtime);
    exit_task(runtime);
    return pending;
}

/*
 * EN: Checks the current `take_pending` condition in Event Runtime state.
 * 中文：检查当前 Event Runtime 状态中的 `take_pending` 条件。
 */
static void take_pending(ucn_event_runtime_t *runtime,
                         ucn_event_source_events_t *source_events,
                         ucn_event_owner_events_t *owner_events)
{
    enter_task(runtime);
    (void)memcpy(source_events, runtime->pending_source_events,
                 sizeof(runtime->pending_source_events));
    (void)memset(runtime->pending_source_events, 0,
                 sizeof(runtime->pending_source_events));
    *owner_events = runtime->pending_owner_events;
    runtime->pending_owner_events = 0U;
    exit_task(runtime);
}

/*
 * EN: Re-arms pending Source events after an Event Runtime budget boundary.
 * 中文：在 Event Runtime 预算边界后重新挂起 Source 事件。
 */
static void rearm_source(ucn_event_runtime_t *runtime,
                         ucn_event_source_id_t source_id,
                         ucn_event_source_events_t events)
{
    enter_task(runtime);
    runtime->pending_source_events[source_id] |= events;
    exit_task(runtime);
}

/*
 * EN: Re-arms pending Owner work after an Event Runtime budget boundary.
 * 中文：在 Event Runtime 预算边界后重新挂起 Owner 工作。
 */
static void rearm_owner(ucn_event_runtime_t *runtime,
                        ucn_event_owner_events_t events)
{
    enter_task(runtime);
    runtime->pending_owner_events |= events;
    exit_task(runtime);
}

/*
 * EN: Runs one bounded multi-Source drain and Protocol-Owner cycle.
 * 中文：运行一次有界的多 Source 排空与 Protocol Owner 周期。
 */
ucn_result_t ucn_event_runtime_run(
    ucn_event_runtime_t *runtime,
    bool fallback_scan,
    ucn_event_runtime_run_result_t *result)
{
    ucn_event_runtime_run_result_t local_result;
    ucn_result_t first_error = UCN_OK;

    (void)memset(&local_result, 0, sizeof(local_result));
    local_result.fallback_scan = fallback_scan;
    if (result != NULL) {
        *result = local_result;
    }
    if (runtime == NULL || !runtime->initialized) {
        return UCN_ERR_ARGUMENT;
    }

    runtime->stats.runs++;
    if (fallback_scan) {
        runtime->stats.fallback_scans++;
    }

    while (local_result.rounds < runtime->max_drain_rounds) {
        ucn_event_source_events_t
            source_events[UCN_EVENT_RUNTIME_MAX_SOURCES];
        ucn_event_owner_events_t owner_events = 0U;
        ucn_event_source_id_t source_id;
        size_t pumped = 0U;
        uint8_t bridged = 0U;
        ucn_result_t owner_result;
        const ucn_protocol_owner_stats_t *owner_stats;
        bool any_events = false;
        bool round_worked = false;

        (void)memset(source_events, 0, sizeof(source_events));
        take_pending(runtime, source_events, &owner_events);
        if (fallback_scan && local_result.rounds == 0U) {
            for (source_id = 0U;
                 source_id < UCN_EVENT_RUNTIME_MAX_SOURCES; ++source_id) {
                if (runtime->sources[source_id].occupied) {
                    source_events[source_id] |=
                        UCN_EVENT_SOURCE_FALLBACK_SCAN;
                }
            }
            owner_events |= UCN_EVENT_OWNER_TIMER;
        }
        any_events = owner_events != 0U;
        for (source_id = 0U;
             !any_events && source_id < UCN_EVENT_RUNTIME_MAX_SOURCES;
             ++source_id) {
            if (source_events[source_id] != 0U) {
                any_events = true;
            }
        }
        if (!any_events) {
            break;
        }

        local_result.rounds++;
        runtime->stats.drain_rounds++;
        for (source_id = 0U; source_id < UCN_EVENT_RUNTIME_MAX_SOURCES;
             ++source_id) {
            ucn_event_source_service_result_t source_result;
            ucn_result_t source_call_result;

            if (source_events[source_id] == 0U) {
                continue;
            }
            if (!runtime->sources[source_id].occupied) {
                if (first_error == UCN_OK) {
                    first_error = UCN_ERR_NOT_FOUND;
                }
                continue;
            }
            (void)memset(&source_result, 0, sizeof(source_result));
            source_call_result = runtime->sources[source_id].ops->service(
                runtime->sources[source_id].context, source_events[source_id],
                runtime->max_source_work_per_round, &source_result);
            runtime->stats.source_callbacks++;
            runtime->stats.last_source_result = source_call_result;
            if (source_result.work_done > runtime->max_source_work_per_round ||
                (source_result.pending_events &
                 (ucn_event_source_events_t)~UCN_EVENT_SOURCE_PENDING_MASK) !=
                    0U) {
                if (first_error == UCN_OK) {
                    first_error = UCN_ERR_MALFORMED;
                }
                continue;
            }
            runtime->stats.source_work += (uint32_t)source_result.work_done;
            local_result.source_work += source_result.work_done;
            round_worked = round_worked || source_result.work_done != 0U;
            if (source_result.pending_events != 0U) {
                rearm_source(runtime, source_id,
                             source_result.pending_events);
            }
            if (source_call_result != UCN_OK && first_error == UCN_OK) {
                first_error = source_call_result;
            }
        }

        owner_result = ucn_protocol_owner_step(&runtime->owner, &pumped,
                                                &bridged);
        runtime->stats.last_owner_result = owner_result;
        local_result.rx_frames_pumped += pumped;
        local_result.bridge_requests_processed += (uint32_t)bridged;
        owner_stats = ucn_protocol_owner_get_stats(&runtime->owner);
        round_worked = round_worked || pumped != 0U || bridged != 0U ||
                       (owner_stats != NULL &&
                        owner_stats->last_node_step_result == UCN_OK);
        /* Budget saturation is deliberately re-armed.  The equality is
         * conservative (the queue may have become empty exactly at the
         * boundary), but at worst it costs one empty Owner round and avoids
         * sleeping while complete frames or Bridge work are still queued. */
        if (pumped == runtime->owner.config.max_rx_frames_per_step) {
            rearm_owner(runtime, UCN_EVENT_OWNER_RX_QUEUE);
        }
#if UCN_FEATURE_SERVICE
        if (runtime->owner.config.bridge != NULL &&
            bridged == runtime->owner.config.max_bridge_requests_per_step) {
            rearm_owner(runtime, UCN_EVENT_OWNER_SERVICE);
        }
#endif
        /* UCN_OK is the Node's documented "one unit of work was handled"
         * result; NOT_FOUND is idle.  Re-arm after actual work so queued TX
         * or maintenance cannot wait for the periodic fallback timeout. */
        if (owner_stats != NULL &&
            owner_stats->last_node_step_result == UCN_OK) {
            rearm_owner(runtime, UCN_EVENT_OWNER_TIMER);
        }
        if (owner_result != UCN_OK && first_error == UCN_OK) {
            first_error = owner_result;
        }
        if (!round_worked && !ucn_event_runtime_has_pending(runtime)) {
            break;
        }
    }

    local_result.work_remaining = ucn_event_runtime_has_pending(runtime);
    if (local_result.work_remaining &&
        local_result.rounds >= runtime->max_drain_rounds) {
        runtime->stats.drain_budget_hits++;
        if (runtime->scheduler_ops != NULL &&
            runtime->scheduler_ops->yield_owner != NULL) {
            runtime->scheduler_ops->yield_owner(runtime->scheduler_context);
            runtime->stats.yields++;
        }
    }
    if (result != NULL) {
        *result = local_result;
    }
    return first_error;
}

/*
 * EN: Waits when idle and then runs one bounded Event Runtime task cycle.
 * 中文：空闲时等待，然后运行一次有界的 Event Runtime 任务周期。
 */
ucn_result_t ucn_event_runtime_task_cycle(
    ucn_event_runtime_t *runtime,
    uint32_t requested_wait_ms,
    ucn_event_runtime_run_result_t *result)
{
    bool notified = false;
    uint32_t wait_ms;

    if (runtime == NULL || !runtime->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_event_runtime_has_pending(runtime)) {
        wait_ms = requested_wait_ms > UCN_MAX_STEP_INTERVAL_MS ?
                      UCN_MAX_STEP_INTERVAL_MS : requested_wait_ms;
        if (runtime->scheduler_ops != NULL) {
            runtime->stats.waits++;
            notified = runtime->scheduler_ops->wait_owner(
                runtime->scheduler_context, wait_ms);
            if (!notified) {
                runtime->stats.wait_timeouts++;
            }
        }
    } else {
        notified = true;
    }
    return ucn_event_runtime_run(runtime, !notified, result);
}

/*
 * EN: Returns the current `stats` view from Event Runtime state.
 * 中文：从 Event Runtime 状态返回当前 `stats` 视图。
 */
const ucn_event_runtime_stats_t *ucn_event_runtime_get_stats(
    const ucn_event_runtime_t *runtime)
{
    return runtime == NULL || !runtime->initialized ? NULL : &runtime->stats;
}

/*
 * EN: Returns the current `owner_stats` view from Event Runtime state.
 * 中文：从 Event Runtime 状态返回当前 `owner_stats` 视图。
 */
const ucn_protocol_owner_stats_t *ucn_event_runtime_get_owner_stats(
    const ucn_event_runtime_t *runtime)
{
    return runtime == NULL || !runtime->initialized ? NULL :
        ucn_protocol_owner_get_stats(&runtime->owner);
}
