#include <string.h>

#include "ucn/ucn_adapter.h"
#include "ucn/ucn_time.h"

#if UCN_FEATURE_DYNAMIC_MESH
/*
 * EN: Checks whether `interval_with_jitter` satisfies the Adapter module's validity rules.
 * 中文：检查 `interval_with_jitter` 是否满足 Adapter 模块的合法性规则。
 */
static bool hello_interval_with_jitter_is_valid(uint32_t interval_ms,
                                                uint16_t jitter_permille)
{
    uint64_t worst_case_ms;

    if (!ucn_duration_is_valid(interval_ms) ||
        jitter_permille > UCN_ADAPTER_HELLO_MAX_RETRY_JITTER_PERMILLE) {
        return false;
    }
    worst_case_ms = (uint64_t)interval_ms *
                    (uint64_t)(1000U + jitter_permille);
    return worst_case_ms <=
           (uint64_t)UCN_MAX_SAFE_DURATION_MS * UINT64_C(1000);
}

/*
 * EN: Checks whether `config` satisfies the Adapter module's validity rules.
 * 中文：检查 `config` 是否满足 Adapter 模块的合法性规则。
 */
static bool hello_config_is_valid(const ucn_adapter_hello_config_t *config)
{
    if (config == NULL) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    if (config->admitted_policy != UCN_ADAPTER_HELLO_ADMITTED_POLICY_SLOW &&
        config->admitted_policy != UCN_ADAPTER_HELLO_ADMITTED_POLICY_STOP) {
        return false;
    }
    if (!ucn_duration_is_valid(config->initial_jitter_min_ms) ||
        !ucn_duration_is_valid(config->initial_jitter_max_ms) ||
        config->initial_jitter_max_ms < config->initial_jitter_min_ms ||
        config->retry_jitter_permille >
            UCN_ADAPTER_HELLO_MAX_RETRY_JITTER_PERMILLE ||
        !hello_interval_with_jitter_is_valid(config->backoff_initial_ms,
                                             config->retry_jitter_permille) ||
        !hello_interval_with_jitter_is_valid(config->backoff_max_ms,
                                             config->retry_jitter_permille) ||
        config->backoff_max_ms < config->backoff_initial_ms) {
        return false;
    }
    if ((config->max_fast_retries != 0U ||
         config->admitted_policy == UCN_ADAPTER_HELLO_ADMITTED_POLICY_SLOW) &&
        !hello_interval_with_jitter_is_valid(config->fast_retry_interval_ms,
                                             config->retry_jitter_permille)) {
        return false;
    }
    return config->admitted_policy != UCN_ADAPTER_HELLO_ADMITTED_POLICY_SLOW ||
           hello_interval_with_jitter_is_valid(
               config->admitted_slow_interval_ms,
               config->retry_jitter_permille);
}

/*
 * EN: Advances the deterministic PRNG used for HELLO scheduling jitter.
 * 中文：推进 HELLO 调度抖动使用的确定性伪随机序列。
 */
static uint32_t hello_random_next(ucn_adapter_hello_scheduler_t *scheduler)
{
    uint32_t value = scheduler->random_state;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    if (value == 0U) {
        value = UINT32_C(0x6D2B79F5);
    }
    scheduler->random_state = value;
    return value;
}

/*
 * EN: Initializes the deterministic HELLO scheduler random state.
 * 中文：初始化 HELLO Scheduler 的确定性随机状态。
 */
static void hello_seed(ucn_adapter_hello_scheduler_t *scheduler,
                       uint32_t random_seed)
{
    uint32_t value = random_seed ^ scheduler->adapter_token ^
                     UINT32_C(0x9E3779B9);

    if (value == 0U) {
        value = UINT32_C(0xA341316C);
    }
    scheduler->random_state = value;
    (void)hello_random_next(scheduler);
}

/*
 * EN: Initializes `initial_delay` for Adapter using caller-owned fixed storage.
 * 中文：使用调用方提供的固定存储初始化 Adapter 的 `initial_delay`。
 */
static uint32_t hello_initial_delay(ucn_adapter_hello_scheduler_t *scheduler)
{
    const uint32_t minimum = scheduler->config.initial_jitter_min_ms;
    const uint32_t range = scheduler->config.initial_jitter_max_ms - minimum + 1U;

    return minimum + hello_random_next(scheduler) % range;
}

/*
 * EN: Calculates the bounded randomized delay before the next HELLO retry.
 * 中文：计算下一次 HELLO 重试前的有界随机延迟。
 */
static uint32_t hello_retry_delay(ucn_adapter_hello_scheduler_t *scheduler,
                                  uint32_t interval_ms)
{
    const uint32_t span_ms = (uint32_t)(
        ((uint64_t)interval_ms * scheduler->config.retry_jitter_permille) /
        UINT64_C(1000));
    uint32_t offset_range;

    if (span_ms == 0U) {
        return interval_ms;
    }
    offset_range = span_ms * 2U + 1U;
    return interval_ms - span_ms + hello_random_next(scheduler) % offset_range;
}

/*
 * EN: Schedules `schedule_initial` using the wrap-safe Adapter time domain.
 * 中文：使用回绕安全的 Adapter 时间域调度 `schedule_initial`。
 */
static void hello_schedule_initial(ucn_adapter_hello_scheduler_t *scheduler,
                                   uint32_t now_ms)
{
    scheduler->state = UCN_ADAPTER_HELLO_INITIAL_JITTER;
    scheduler->fast_retries_sent = 0U;
    scheduler->backoff_interval_ms = scheduler->config.backoff_initial_ms;
    scheduler->next_hello_ms =
        ucn_deadline_from_now(now_ms, hello_initial_delay(scheduler));
}

/*
 * EN: Schedules `schedule_retry` using the wrap-safe Adapter time domain.
 * 中文：使用回绕安全的 Adapter 时间域调度 `schedule_retry`。
 */
static void hello_schedule_retry(ucn_adapter_hello_scheduler_t *scheduler,
                                 uint32_t now_ms,
                                 uint32_t interval_ms)
{
    scheduler->next_hello_ms = ucn_deadline_from_now(
        now_ms, hello_retry_delay(scheduler, interval_ms));
}

/*
 * EN: Enters or leaves the bounded `enter_admitted` critical section for Adapter.
 * 中文：进入或退出 Adapter 的有界 `enter_admitted` 临界区。
 */
static void hello_enter_admitted(ucn_adapter_hello_scheduler_t *scheduler,
                                 uint32_t now_ms)
{
    if (scheduler->config.admitted_policy ==
        UCN_ADAPTER_HELLO_ADMITTED_POLICY_STOP) {
        scheduler->state = UCN_ADAPTER_HELLO_ADMITTED_STOP;
        scheduler->next_hello_ms = 0U;
        scheduler->stats.admitted_stop_transitions++;
        return;
    }

    scheduler->state = UCN_ADAPTER_HELLO_ADMITTED_SLOW;
    hello_schedule_retry(scheduler, now_ms,
                         scheduler->config.fast_retry_interval_ms);
    scheduler->stats.admitted_slow_transitions++;
}
#endif

/*
 * EN: Enters or leaves the bounded `enter_task` critical section for Adapter.
 * 中文：进入或退出 Adapter 的有界 `enter_task` 临界区。
 */
static void queue_enter_task(ucn_adapter_rx_queue_t *queue)
{
    if (queue->port_ops != NULL) {
        queue->port_ops->enter_critical(queue->port_context);
    }
}

/*
 * EN: Enters or leaves the bounded `exit_task` critical section for Adapter.
 * 中文：进入或退出 Adapter 的有界 `exit_task` 临界区。
 */
static void queue_exit_task(ucn_adapter_rx_queue_t *queue)
{
    if (queue->port_ops != NULL) {
        queue->port_ops->exit_critical(queue->port_context);
    }
}

/*
 * EN: Enters or leaves the bounded `enter_from_isr` critical section for Adapter.
 * 中文：进入或退出 Adapter 的有界 `enter_from_isr` 临界区。
 */
static ucn_port_critical_token_t queue_enter_from_isr(
    ucn_adapter_rx_queue_t *queue)
{
    return queue->port_ops->enter_critical_from_isr(queue->port_context);
}

/*
 * EN: Enters or leaves the bounded `exit_from_isr` critical section for Adapter.
 * 中文：进入或退出 Adapter 的有界 `exit_from_isr` 临界区。
 */
static void queue_exit_from_isr(ucn_adapter_rx_queue_t *queue,
                                ucn_port_critical_token_t token)
{
    queue->port_ops->exit_critical_from_isr(queue->port_context, token);
}

/*
 * EN: Checks the `isr_critical_is_configured` condition against current Adapter state.
 * 中文：根据当前 Adapter 状态检查 `isr_critical_is_configured` 条件。
 */
static bool queue_isr_critical_is_configured(const ucn_adapter_rx_queue_t *queue)
{
    return queue->port_ops != NULL &&
           queue->port_ops->enter_critical_from_isr != NULL &&
           queue->port_ops->exit_critical_from_isr != NULL;
}

/*
 * EN: Checks whether `address` satisfies the Adapter module's validity rules.
 * 中文：检查 `address` 是否满足 Adapter 模块的合法性规则。
 */
bool ucn_adapter_address_is_valid(const ucn_adapter_address_t *address)
{
    return address != NULL && address->length != 0U &&
           address->length <= UCN_ADAPTER_PHYSICAL_ADDRESS_MAX;
}

/*
 * EN: Compares `address_equal` using the canonical Adapter identity rules.
 * 中文：按照规范的 Adapter 身份规则比较 `address_equal`。
 */
bool ucn_adapter_address_equal(const ucn_adapter_address_t *left,
                               const ucn_adapter_address_t *right)
{
    if (!ucn_adapter_address_is_valid(left) ||
        !ucn_adapter_address_is_valid(right) || left->length != right->length) {
        return false;
    }
    return memcmp(left->bytes, right->bytes, left->length) == 0;
}

/*
 * EN: Searches bounded Adapter state for `peer`.
 * 中文：在固定容量的 Adapter 状态中查找 `peer`。
 */
ucn_adapter_peer_binding_t *ucn_adapter_find_peer(
    ucn_adapter_peer_binding_t *bindings,
    size_t binding_count,
    const ucn_adapter_address_t *address)
{
    size_t index;

    if (bindings == NULL || !ucn_adapter_address_is_valid(address)) {
        return NULL;
    }
    for (index = 0U; index < binding_count; ++index) {
        if (bindings[index].occupied &&
            ucn_adapter_address_equal(&bindings[index].address, address)) {
            return &bindings[index];
        }
    }
    return NULL;
}

/*
 * EN: Validates and installs `bind_peer` into bounded Adapter state.
 * 中文：验证 `bind_peer` 并将其安装到固定容量的 Adapter 状态中。
 */
ucn_result_t ucn_adapter_bind_peer(ucn_adapter_peer_binding_t *bindings,
                                   size_t binding_count,
                                   const ucn_adapter_address_t *address,
                                   ucn_link_t *link)
{
    ucn_adapter_peer_binding_t *free_slot = NULL;
    size_t index;

    if (bindings == NULL || binding_count == 0U ||
        !ucn_adapter_address_is_valid(address) || link == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    for (index = 0U; index < binding_count; ++index) {
        if (bindings[index].occupied &&
            ucn_adapter_address_equal(&bindings[index].address, address)) {
            return bindings[index].link == link ? UCN_OK : UCN_ERR_CONFIG;
        }
        if (!bindings[index].occupied && free_slot == NULL) {
            free_slot = &bindings[index];
        }
    }

    if (free_slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    free_slot->occupied = true;
    free_slot->address = *address;
    free_slot->link = link;
    return UCN_OK;
}

/*
 * EN: Initializes `rx_queue_init` for Adapter using caller-owned fixed storage.
 * 中文：使用调用方提供的固定存储初始化 Adapter 的 `rx_queue_init`。
 */
ucn_result_t ucn_adapter_rx_queue_init(ucn_adapter_rx_queue_t *queue,
                                       const ucn_port_ops_t *port_ops,
                                       void *port_context)
{
    if (queue == NULL ||
        (port_ops != NULL &&
         (!ucn_port_ops_is_compatible(port_ops) ||
          port_ops->enter_critical == NULL || port_ops->exit_critical == NULL ||
          (port_ops->enter_critical_from_isr == NULL) !=
              (port_ops->exit_critical_from_isr == NULL)))) {
        return UCN_ERR_ARGUMENT;
    }

    (void)memset(queue, 0, sizeof(*queue));
    queue->port_ops = port_ops;
    queue->port_context = port_context;
    return UCN_OK;
}

/*
 * EN: Copies `rx_enqueue` into a bounded Adapter queue.
 * 中文：把 `rx_enqueue` 复制到固定容量的 Adapter 队列。
 */
ucn_result_t ucn_adapter_rx_enqueue(ucn_adapter_rx_queue_t *queue,
                                    ucn_link_t *ingress_link,
                                    const uint8_t *data,
                                    size_t length)
{
    ucn_adapter_rx_item_t *item;

    if (queue == NULL || ingress_link == NULL || data == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (length > UCN_MAX_FRAME_BYTES || length > UINT16_MAX) {
        return UCN_ERR_TOO_LARGE;
    }

    queue_enter_task(queue);
    if (queue->count >= UCN_ADAPTER_RX_QUEUE_DEPTH) {
        queue->stats.dropped_full++;
        queue_exit_task(queue);
        return UCN_ERR_NO_SPACE;
    }

    item = &queue->items[queue->tail];
    item->ingress_link = ingress_link;
    item->length = (uint16_t)length;
    if (length != 0U) {
        (void)memcpy(item->data, data, length);
    }
    queue->tail = (queue->tail + 1U) % UCN_ADAPTER_RX_QUEUE_DEPTH;
    queue->count++;
    queue->stats.enqueued++;
    queue_exit_task(queue);
    return UCN_OK;
}

/*
 * EN: Copies `rx_enqueue_from_isr` into a bounded Adapter queue.
 * 中文：把 `rx_enqueue_from_isr` 复制到固定容量的 Adapter 队列。
 */
ucn_result_t ucn_adapter_rx_enqueue_from_isr(ucn_adapter_rx_queue_t *queue,
                                             ucn_link_t *ingress_link,
                                             const uint8_t *data,
                                             size_t length)
{
    ucn_adapter_rx_item_t *item;
    ucn_port_critical_token_t token;

    if (queue == NULL || ingress_link == NULL || data == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (length > UCN_MAX_FRAME_BYTES || length > UINT16_MAX) {
        return UCN_ERR_TOO_LARGE;
    }
    if (!queue_isr_critical_is_configured(queue)) {
        return UCN_ERR_CONFIG;
    }

    token = queue_enter_from_isr(queue);
    if (queue->count >= UCN_ADAPTER_RX_QUEUE_DEPTH) {
        queue->stats.dropped_full++;
        queue_exit_from_isr(queue, token);
        return UCN_ERR_NO_SPACE;
    }

    item = &queue->items[queue->tail];
    item->ingress_link = ingress_link;
    item->length = (uint16_t)length;
    if (length != 0U) {
        (void)memcpy(item->data, data, length);
    }
    queue->tail = (queue->tail + 1U) % UCN_ADAPTER_RX_QUEUE_DEPTH;
    queue->count++;
    queue->stats.enqueued++;
    queue_exit_from_isr(queue, token);
    return UCN_OK;
}

/*
 * EN: Processes one bounded `rx_pump` work unit for Adapter.
 * 中文：为 Adapter 处理一个有界的 `rx_pump` 工作单元。
 */
ucn_result_t ucn_adapter_rx_pump(ucn_adapter_rx_queue_t *queue,
                                 ucn_node_t *node,
                                 size_t max_frames,
                                 size_t *pumped)
{
    size_t local_pumped = 0U;

    if (queue == NULL || node == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    while (local_pumped < max_frames) {
        ucn_adapter_rx_item_t item;
        ucn_result_t result;

        queue_enter_task(queue);
        if (queue->count == 0U) {
            queue_exit_task(queue);
            break;
        }
        item = queue->items[queue->head];
        queue->head = (queue->head + 1U) % UCN_ADAPTER_RX_QUEUE_DEPTH;
        queue->count--;
        queue_exit_task(queue);

        result = ucn_node_receive(node, item.ingress_link, item.data, item.length);
        queue->stats.pumped++;
        if (result != UCN_OK) {
            queue->stats.rejected_by_core++;
        }
        local_pumped++;
    }

    if (pumped != NULL) {
        *pumped = local_pumped;
    }
    return UCN_OK;
}

/*
 * EN: Returns the current `rx_get_stats` view from Adapter state.
 * 中文：从 Adapter 状态返回当前 `rx_get_stats` 视图。
 */
const ucn_adapter_rx_stats_t *ucn_adapter_rx_get_stats(
    const ucn_adapter_rx_queue_t *queue)
{
    return queue == NULL ? NULL : &queue->stats;
}

#if UCN_FEATURE_DYNAMIC_MESH
/*
 * EN: Initializes `hello_scheduler_init` for Adapter using caller-owned fixed storage.
 * 中文：使用调用方提供的固定存储初始化 Adapter 的 `hello_scheduler_init`。
 */
ucn_result_t ucn_adapter_hello_scheduler_init(
    ucn_adapter_hello_scheduler_t *scheduler,
    const ucn_adapter_hello_config_t *config,
    uint32_t adapter_token,
    uint32_t random_seed,
    uint32_t now_ms)
{
    if (scheduler == NULL || !hello_config_is_valid(config) ||
        adapter_token == 0U) {
        return UCN_ERR_ARGUMENT;
    }

    (void)memset(scheduler, 0, sizeof(*scheduler));
    scheduler->config = *config;
    scheduler->adapter_token = adapter_token;
    scheduler->initialized = true;
    hello_seed(scheduler, random_seed);
    if (!config->enabled) {
        scheduler->state = UCN_ADAPTER_HELLO_DISABLED;
        return UCN_OK;
    }
    hello_schedule_initial(scheduler, now_ms);
    return UCN_OK;
}

/*
 * EN: Restarts HELLO scheduling with a new seed and current timestamp.
 * 中文：使用新种子和当前时间重新启动 HELLO 调度。
 */
ucn_result_t ucn_adapter_hello_scheduler_restart(
    ucn_adapter_hello_scheduler_t *scheduler,
    uint32_t random_seed,
    uint32_t now_ms)
{
    if (scheduler == NULL || !scheduler->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (!scheduler->config.enabled) {
        scheduler->state = UCN_ADAPTER_HELLO_DISABLED;
        scheduler->next_hello_ms = 0U;
        return UCN_OK;
    }

    hello_seed(scheduler, random_seed);
    hello_schedule_initial(scheduler, now_ms);
    scheduler->stats.discovery_restarts++;
    return UCN_OK;
}

/*
 * EN: Advances one bounded `hello_scheduler_step` state-machine step in Adapter.
 * 中文：在 Adapter 中推进一次有界的 `hello_scheduler_step` 状态机步骤。
 */
ucn_result_t ucn_adapter_hello_scheduler_step(
    ucn_adapter_hello_scheduler_t *scheduler,
    uint32_t now_ms,
    bool adapter_has_admitted_peer,
    bool *hello_due)
{
    if (scheduler == NULL || !scheduler->initialized || hello_due == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *hello_due = false;
    if (scheduler->state == UCN_ADAPTER_HELLO_DISABLED) {
        return UCN_OK;
    }

    if (adapter_has_admitted_peer) {
        if (scheduler->state != UCN_ADAPTER_HELLO_ADMITTED_SLOW &&
            scheduler->state != UCN_ADAPTER_HELLO_ADMITTED_STOP) {
            hello_enter_admitted(scheduler, now_ms);
        }
        if (scheduler->state == UCN_ADAPTER_HELLO_ADMITTED_STOP ||
            !ucn_deadline_expired(now_ms, scheduler->next_hello_ms)) {
            return UCN_OK;
        }

        *hello_due = true;
        scheduler->stats.hellos_due++;
        hello_schedule_retry(scheduler, now_ms,
                             scheduler->config.admitted_slow_interval_ms);
        return UCN_OK;
    }

    if (scheduler->state == UCN_ADAPTER_HELLO_ADMITTED_SLOW ||
        scheduler->state == UCN_ADAPTER_HELLO_ADMITTED_STOP) {
        hello_schedule_initial(scheduler, now_ms);
        scheduler->stats.discovery_restarts++;
        return UCN_OK;
    }
    if (!ucn_deadline_expired(now_ms, scheduler->next_hello_ms)) {
        return UCN_OK;
    }

    *hello_due = true;
    scheduler->stats.hellos_due++;
    if (scheduler->state == UCN_ADAPTER_HELLO_INITIAL_JITTER) {
        if (scheduler->config.max_fast_retries == 0U) {
            scheduler->state = UCN_ADAPTER_HELLO_BACKOFF;
            hello_schedule_retry(scheduler, now_ms,
                                 scheduler->backoff_interval_ms);
        } else {
            scheduler->state = UCN_ADAPTER_HELLO_FAST_RETRY;
            hello_schedule_retry(scheduler, now_ms,
                                 scheduler->config.fast_retry_interval_ms);
        }
        return UCN_OK;
    }
    if (scheduler->state == UCN_ADAPTER_HELLO_FAST_RETRY) {
        scheduler->fast_retries_sent++;
        if (scheduler->fast_retries_sent >=
            scheduler->config.max_fast_retries) {
            scheduler->state = UCN_ADAPTER_HELLO_BACKOFF;
            hello_schedule_retry(scheduler, now_ms,
                                 scheduler->backoff_interval_ms);
        } else {
            hello_schedule_retry(scheduler, now_ms,
                                 scheduler->config.fast_retry_interval_ms);
        }
        return UCN_OK;
    }

    if (scheduler->backoff_interval_ms >=
        scheduler->config.backoff_max_ms / 2U) {
        scheduler->backoff_interval_ms = scheduler->config.backoff_max_ms;
    } else {
        scheduler->backoff_interval_ms *= 2U;
    }
    hello_schedule_retry(scheduler, now_ms,
                         scheduler->backoff_interval_ms);
    return UCN_OK;
}

/*
 * EN: Returns the current `hello_scheduler_get_stats` view from Adapter state.
 * 中文：从 Adapter 状态返回当前 `hello_scheduler_get_stats` 视图。
 */
const ucn_adapter_hello_stats_t *ucn_adapter_hello_scheduler_get_stats(
    const ucn_adapter_hello_scheduler_t *scheduler)
{
    return scheduler == NULL || !scheduler->initialized ? NULL :
                                                            &scheduler->stats;
}
#endif
