/* Optional timestamp-capable Link extension and atomic queues.
 * 可选的时间戳 Link 扩展与原子队列。 */

#include "ucn/ucn_timed_link.h"

#include <stddef.h>
#include <string.h>

/* EN: Saturating-increments a Link diagnostic.
 * 中文：对 Link 诊断值执行饱和递增。 */
static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
}

/* EN: Checks the versioned optional Driver extension contract.
 * 中文：检查带版本的可选驱动扩展合同。 */
bool ucn_time_link_ops_is_compatible(const ucn_time_link_ops_t *ops)
{
    return ops != NULL &&
           ops->struct_size >= offsetof(ucn_time_link_ops_t, quiesce) +
                                   sizeof(ops->quiesce) &&
           ops->api_version == UCN_TIME_LINK_OPS_API_VERSION &&
           ops->reserve_tx_token != NULL &&
           ops->submit_timestamped != NULL &&
           ops->cancel_tx_token != NULL && ops->quiesce != NULL;
}

/* EN: Validates an optional Port lock configuration as paired callbacks.
 * 中文：把可选 Port 锁配置校验为成对回调。 */
static bool port_lock_pairs_are_valid(const ucn_port_ops_t *ops)
{
    bool has_task;
    bool has_isr;

    if (ops == NULL) {
        return true;
    }
    if (!ucn_port_ops_is_compatible(ops) ||
        ((ops->enter_critical == NULL) !=
         (ops->exit_critical == NULL)) ||
        ((ops->enter_critical_from_isr == NULL) !=
         (ops->exit_critical_from_isr == NULL))) {
        return false;
    }
    has_task = ops->enter_critical != NULL;
    has_isr = ops->enter_critical_from_isr != NULL;
    return !has_isr || has_task;
}

/* EN: Requires one lock domain that is shared by task and ISR entry points.
 * 中文：要求任务与 ISR 入口共享同一个底层锁域。 */
static bool callback_gate_port_is_valid(const ucn_port_ops_t *ops)
{
    return port_lock_pairs_are_valid(ops) && ops != NULL &&
           ops->enter_critical != NULL && ops->exit_critical != NULL &&
           ops->enter_critical_from_isr != NULL &&
           ops->exit_critical_from_isr != NULL;
}

/* EN: Initializes the shared callback gate before any Link references it.
 * 中文：在任何 Link 引用前初始化共享回调围栏。 */
ucn_result_t ucn_timed_link_callback_gate_init(
    ucn_timed_link_callback_gate_t *gate,
    const ucn_port_ops_t *port_ops,
    void *port_context)
{
    ucn_timed_link_callback_gate_t initialized;

    if (gate == NULL || !callback_gate_port_is_valid(port_ops)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&initialized, 0, sizeof(initialized));
    initialized.port_ops = port_ops;
    initialized.port_context = port_context;
    initialized.initialized = true;
    *gate = initialized;
    return UCN_OK;
}

/* EN: Enters the execution-domain gate from task context.
 * 中文：从任务上下文进入执行域共享围栏。 */
static void callback_gate_enter_task(ucn_timed_link_callback_gate_t *gate)
{
    gate->port_ops->enter_critical(gate->port_context);
}

/* EN: Leaves the execution-domain gate from task context.
 * 中文：从任务上下文退出执行域共享围栏。 */
static void callback_gate_exit_task(ucn_timed_link_callback_gate_t *gate)
{
    gate->port_ops->exit_critical(gate->port_context);
}

/* EN: Enters the execution-domain gate from ISR context.
 * 中文：从 ISR 上下文进入执行域共享围栏。 */
static ucn_port_critical_token_t callback_gate_enter_isr(
    ucn_timed_link_callback_gate_t *gate)
{
    return gate->port_ops->enter_critical_from_isr(gate->port_context);
}

/* EN: Leaves the execution-domain gate from ISR context.
 * 中文：从 ISR 上下文退出执行域共享围栏。 */
static void callback_gate_exit_isr(
    ucn_timed_link_callback_gate_t *gate,
    ucn_port_critical_token_t token)
{
    gate->port_ops->exit_critical_from_isr(gate->port_context, token);
}

/* EN: Initializes one bounded local Link Instance namespace.
 * 中文：初始化一个有界的本地 Link Instance 命名空间。 */
ucn_result_t ucn_timed_link_init(ucn_timed_link_t *link,
                                 uint8_t link_id,
                                 uint32_t initial_generation,
                                 const ucn_time_link_ops_t *ops,
                                 void *context,
                                 const ucn_port_ops_t *port_ops,
                                 void *port_context,
                                 ucn_timed_link_callback_gate_t *callback_gate)
{
    ucn_timed_link_t initialized;

    if (link == NULL || link_id == 0U || initial_generation == 0U ||
        initial_generation > UCN_TIME_EVENT_SERIAL_MAX ||
        !ucn_time_link_ops_is_compatible(ops) ||
        !port_lock_pairs_are_valid(port_ops) || callback_gate == NULL ||
        !callback_gate->initialized ||
        !callback_gate_port_is_valid(callback_gate->port_ops)) {
        return UCN_ERR_ARGUMENT;
    }
    callback_gate_enter_task(callback_gate);
    if (callback_gate->active) {
        callback_gate_exit_task(callback_gate);
        return UCN_ERR_STATE;
    }
    (void)memset(&initialized, 0, sizeof(initialized));
    initialized.ops = ops;
    initialized.context = context;
    initialized.port_ops = port_ops;
    initialized.port_context = port_context;
    initialized.callback_gate = callback_gate;
    initialized.link_id = link_id;
    initialized.link_instance_generation = initial_generation;
    initialized.initialized = true;
    *link = initialized;
    callback_gate_exit_task(callback_gate);
    return UCN_OK;
}

/* EN: Enters the optional task-context critical section.
 * 中文：进入可选的任务上下文临界区。 */
static void link_enter_task(ucn_timed_link_t *link)
{
    if (link->port_ops != NULL && link->port_ops->enter_critical != NULL) {
        link->port_ops->enter_critical(link->port_context);
    }
}

/* EN: Leaves the optional task-context critical section.
 * 中文：退出可选的任务上下文临界区。 */
static void link_exit_task(ucn_timed_link_t *link)
{
    if (link->port_ops != NULL && link->port_ops->exit_critical != NULL) {
        link->port_ops->exit_critical(link->port_context);
    }
}

/* EN: Marks a foreign Driver callback while the shared gate and Link task
 * locks are both held.
 * 中文：在同时持有共享围栏锁与 Link 任务锁时标记外部 Driver 回调。 */
static bool driver_callback_enter_locked(ucn_timed_link_t *link)
{
    ucn_timed_link_callback_gate_t *gate = link->callback_gate;

    if (gate->active) {
        return false;
    }
    gate->active = true;
    gate->active_owner = link;
    link->io_active = true;
    return true;
}

/* EN: Clears a foreign Driver callback while the shared gate and Link task
 * locks are both held.
 * 中文：在同时持有共享围栏锁与 Link 任务锁时清除外部 Driver 回调标记。 */
static void driver_callback_leave_locked(ucn_timed_link_t *link)
{
    ucn_timed_link_callback_gate_t *gate = link->callback_gate;

    if (gate->active && gate->active_owner == link) {
        gate->active = false;
        gate->active_owner = NULL;
    } else {
        link->faulted = true;
    }
    link->io_active = false;
}

/* EN: Compares event identity without relying on structure padding.
 * 中文：不依赖结构体填充比较事件身份。 */
static bool event_key_equal(const ucn_time_event_key_t *left,
                            const ucn_time_event_key_t *right)
{
    return left->link_id == right->link_id &&
           left->direction == right->direction &&
           left->link_instance_generation ==
               right->link_instance_generation &&
           left->event_token == right->event_token;
}

/* EN: Finds one live reservation while the Link lock is held.
 * 中文：在持有 Link 锁时查找一条活动 reservation。 */
static ucn_time_event_reservation_t *reservation_find_locked(
    ucn_timed_link_t *link,
    const ucn_time_event_key_t *key)
{
    size_t index;

    for (index = 0U; index < UCN_TIME_EVENT_RESERVATION_CAPACITY; ++index) {
        if (link->reservations[index].occupied &&
            event_key_equal(&link->reservations[index].key, key)) {
            return &link->reservations[index];
        }
    }
    return NULL;
}

/* EN: Returns an empty bounded reservation slot.
 * 中文：返回一个空闲的有界 reservation 槽。 */
static ucn_time_event_reservation_t *reservation_free_locked(
    ucn_timed_link_t *link)
{
    size_t index;

    for (index = 0U; index < UCN_TIME_EVENT_RESERVATION_CAPACITY; ++index) {
        if (!link->reservations[index].occupied) {
            return &link->reservations[index];
        }
    }
    return NULL;
}

/* EN: Publishes one reservation only after the matching allocation succeeds.
 * 中文：仅在对应分配成功后发布一条 reservation。 */
static void reservation_publish_locked(
    ucn_time_event_reservation_t *reservation,
    const ucn_time_event_key_t *key)
{
    (void)memset(reservation, 0, sizeof(*reservation));
    reservation->key = *key;
    reservation->lifecycle = UCN_TIME_EVENT_LIFECYCLE_RESERVED;
    reservation->occupied = true;
}

/* EN: Allocates the next token while the caller holds the correct lock.
 * 中文：在调用者持有正确锁时分配下一 token。 */
static ucn_result_t allocate_locked(ucn_timed_link_t *link,
                                    ucn_time_event_direction_t direction,
                                    ucn_time_event_key_t *key)
{
    ucn_time_event_key_t allocated;
    uint32_t *counter;
    if (link->faulted || link->io_active) {
        return UCN_ERR_STATE;
    }
    if (direction != UCN_TIME_EVENT_TX && direction != UCN_TIME_EVENT_RX) {
        return UCN_ERR_ARGUMENT;
    }
    counter = direction == UCN_TIME_EVENT_TX ? &link->next_tx_token :
                                               &link->next_rx_token;
    if (*counter == UCN_TIME_EVENT_SERIAL_MAX) {
        link->faulted = true;
        return UCN_ERR_EXHAUSTED;
    }
    (void)memset(&allocated, 0, sizeof(allocated));
    allocated.link_id = link->link_id;
    allocated.direction = direction;
    allocated.link_instance_generation = link->link_instance_generation;
    allocated.event_token = *counter + 1U;
    *counter = allocated.event_token;
    *key = allocated;
    return UCN_OK;
}

/* EN: Allocates one event key from task context.
 * 中文：从任务上下文分配一个事件键。 */
ucn_result_t ucn_timed_link_allocate_event(
    ucn_timed_link_t *link,
    ucn_time_event_direction_t direction,
    ucn_time_event_key_t *key)
{
    ucn_time_event_key_t allocated;
    ucn_time_event_reservation_t *reservation;
    ucn_result_t status;

    if (link == NULL || key == NULL || !link->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    callback_gate_enter_task(link->callback_gate);
    if (link->callback_gate->active) {
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_enter_task(link);
    if (reservation_free_locked(link) == NULL) {
        status = UCN_ERR_NO_SPACE;
    } else {
        status = allocate_locked(link, direction, &allocated);
    }
    if (status == UCN_OK && direction == UCN_TIME_EVENT_TX &&
        !driver_callback_enter_locked(link)) {
        status = UCN_ERR_STATE;
    }
    link_exit_task(link);
    if (status != UCN_OK) {
        callback_gate_exit_task(link->callback_gate);
        return status;
    }
    if (direction == UCN_TIME_EVENT_TX) {
        callback_gate_exit_task(link->callback_gate);
        status = link->ops->reserve_tx_token(link->context, &allocated);
        callback_gate_enter_task(link->callback_gate);
        link_enter_task(link);
        driver_callback_leave_locked(link);
        if (status == UCN_OK) {
            reservation = reservation_free_locked(link);
            if (reservation == NULL) {
                status = UCN_ERR_NO_SPACE;
            } else {
                reservation_publish_locked(reservation, &allocated);
                increment_saturated(&link->stats.keys_reserved);
            }
        }
        link_exit_task(link);
        if (status != UCN_OK) {
            callback_gate_exit_task(link->callback_gate);
            return status;
        }
    } else {
        link_enter_task(link);
        reservation = reservation_free_locked(link);
        if (reservation == NULL) {
            status = UCN_ERR_NO_SPACE;
        } else {
            reservation_publish_locked(reservation, &allocated);
            increment_saturated(&link->stats.keys_reserved);
        }
        link_exit_task(link);
    }
    *key = allocated;
    callback_gate_exit_task(link->callback_gate);
    return status;
}

/* EN: Allocates one RX event key under the ISR-specific mask token.
 * 中文：在 ISR 专用 mask token 下分配一个 RX 事件键。 */
ucn_result_t ucn_timed_link_allocate_rx_event_from_isr(
    ucn_timed_link_t *link,
    ucn_time_event_key_t *key)
{
    ucn_time_event_reservation_t *reservation;
    ucn_port_critical_token_t gate_token;
    ucn_port_critical_token_t token;
    ucn_result_t status;

    if (link == NULL || key == NULL || !link->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (link->port_ops == NULL ||
        link->port_ops->enter_critical_from_isr == NULL ||
        link->port_ops->exit_critical_from_isr == NULL) {
        return UCN_ERR_CONFIG;
    }
    gate_token = callback_gate_enter_isr(link->callback_gate);
    if (link->callback_gate->active) {
        callback_gate_exit_isr(link->callback_gate, gate_token);
        return UCN_ERR_STATE;
    }
    token = link->port_ops->enter_critical_from_isr(link->port_context);
    if ((reservation = reservation_free_locked(link)) == NULL) {
        status = UCN_ERR_NO_SPACE;
    } else {
        ucn_time_event_key_t allocated;

        status = allocate_locked(link, UCN_TIME_EVENT_RX, &allocated);
        if (status == UCN_OK) {
            reservation_publish_locked(reservation, &allocated);
            *key = allocated;
        }
    }
    if (status == UCN_OK) {
        increment_saturated(&link->stats.keys_reserved);
    }
    link->port_ops->exit_critical_from_isr(link->port_context, token);
    callback_gate_exit_isr(link->callback_gate, gate_token);
    return status;
}

/* EN: Checks exact current Link Instance ownership of one key.
 * 中文：检查一个 key 是否精确属于当前 Link Instance。 */
bool ucn_timed_link_key_is_current(const ucn_timed_link_t *link,
                                   const ucn_time_event_key_t *key)
{
    return link != NULL && key != NULL && link->initialized &&
           !link->faulted && key->link_id == link->link_id &&
           (key->direction == UCN_TIME_EVENT_TX ||
            key->direction == UCN_TIME_EVENT_RX) &&
           key->link_instance_generation ==
               link->link_instance_generation &&
           key->event_token != 0U &&
           key->event_token <= UCN_TIME_EVENT_SERIAL_MAX;
}

/* EN: Submits an inseparable frame/key pair to the timestamp Driver.
 * 中文：把不可拆分的 frame/key 对提交给时间戳驱动。 */
ucn_result_t ucn_timed_link_submit(ucn_timed_link_t *link,
                                   const ucn_time_event_key_t *key,
                                   const uint8_t *frame,
                                   size_t length)
{
    ucn_result_t status;
    ucn_time_event_reservation_t *reservation;

    if (link == NULL || key == NULL || frame == NULL || length == 0U ||
        length > UCN_MAX_FRAME_BYTES || !link->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    callback_gate_enter_task(link->callback_gate);
    if (link->callback_gate->active) {
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_enter_task(link);
    reservation = reservation_find_locked(link, key);
    if (link->io_active || !ucn_timed_link_key_is_current(link, key) ||
        key->direction != UCN_TIME_EVENT_TX || reservation == NULL ||
        reservation->lifecycle != UCN_TIME_EVENT_LIFECYCLE_RESERVED ||
        !driver_callback_enter_locked(link)) {
        increment_saturated(&link->stats.stale_events);
        link_exit_task(link);
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_exit_task(link);
    callback_gate_exit_task(link->callback_gate);
    status = link->ops->submit_timestamped(link->context, key, frame, length);
    callback_gate_enter_task(link->callback_gate);
    link_enter_task(link);
    driver_callback_leave_locked(link);
    if (status == UCN_OK) {
        reservation = reservation_find_locked(link, key);
        if (reservation == NULL ||
            reservation->lifecycle != UCN_TIME_EVENT_LIFECYCLE_RESERVED) {
            link->faulted = true;
            status = UCN_ERR_STATE;
        } else {
            reservation->lifecycle = UCN_TIME_EVENT_LIFECYCLE_SUBMITTED;
            increment_saturated(&link->stats.timestamped_submissions);
        }
    }
    link_exit_task(link);
    callback_gate_exit_task(link->callback_gate);
    return status;
}

/* EN: Cancels one current TX token without guessing Driver state.
 * 中文：取消一个当前 TX token，且不猜测驱动状态。 */
ucn_result_t ucn_timed_link_cancel(ucn_timed_link_t *link,
                                   const ucn_time_event_key_t *key)
{
    ucn_result_t status;
    ucn_time_event_reservation_t *reservation;

    if (link == NULL || key == NULL || !link->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    callback_gate_enter_task(link->callback_gate);
    if (link->callback_gate->active) {
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_enter_task(link);
    reservation = reservation_find_locked(link, key);
    if (link->io_active || !ucn_timed_link_key_is_current(link, key) ||
        key->direction != UCN_TIME_EVENT_TX || reservation == NULL ||
        reservation->lifecycle != UCN_TIME_EVENT_LIFECYCLE_RESERVED ||
        !driver_callback_enter_locked(link)) {
        link_exit_task(link);
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_exit_task(link);
    callback_gate_exit_task(link->callback_gate);
    status = link->ops->cancel_tx_token(link->context, key);
    callback_gate_enter_task(link->callback_gate);
    link_enter_task(link);
    driver_callback_leave_locked(link);
    if (status == UCN_OK) {
        reservation = reservation_find_locked(link, key);
        if (reservation == NULL ||
            reservation->lifecycle != UCN_TIME_EVENT_LIFECYCLE_RESERVED) {
            link->faulted = true;
            status = UCN_ERR_STATE;
        } else {
            (void)memset(reservation, 0, sizeof(*reservation));
            increment_saturated(&link->stats.cancelled_tokens);
        }
    }
    link_exit_task(link);
    callback_gate_exit_task(link->callback_gate);
    return status;
}

/* EN: Completes a live TX/RX reservation exactly once.
 * 中文：精确一次完成一条活动 TX/RX reservation。 */
ucn_result_t ucn_timed_link_complete_event(
    ucn_timed_link_t *link,
    const ucn_time_event_key_t *key)
{
    ucn_time_event_reservation_t *reservation;

    if (link == NULL || key == NULL || !link->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    callback_gate_enter_task(link->callback_gate);
    if (link->callback_gate->active) {
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_enter_task(link);
    reservation = reservation_find_locked(link, key);
    if (link->io_active || !ucn_timed_link_key_is_current(link, key) ||
        reservation == NULL ||
        (key->direction == UCN_TIME_EVENT_TX &&
         reservation->lifecycle != UCN_TIME_EVENT_LIFECYCLE_SUBMITTED) ||
        (key->direction == UCN_TIME_EVENT_RX &&
         reservation->lifecycle != UCN_TIME_EVENT_LIFECYCLE_RESERVED)) {
        link_exit_task(link);
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    (void)memset(reservation, 0, sizeof(*reservation));
    increment_saturated(&link->stats.completed_tokens);
    link_exit_task(link);
    callback_gate_exit_task(link->callback_gate);
    return UCN_OK;
}

/* EN: Retires an abandoned event and releases any driver-side TX state.
 * 中文：回收废弃事件并释放驱动侧 TX 状态。 */
ucn_result_t ucn_timed_link_retire_event(
    ucn_timed_link_t *link,
    const ucn_time_event_key_t *key)
{
    ucn_time_event_reservation_t *reservation;
    ucn_result_t status = UCN_OK;

    if (link == NULL || key == NULL || !link->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    callback_gate_enter_task(link->callback_gate);
    if (link->callback_gate->active) {
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_enter_task(link);
    reservation = reservation_find_locked(link, key);
    if (link->io_active || !ucn_timed_link_key_is_current(link, key) ||
        reservation == NULL) {
        link_exit_task(link);
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    if (key->direction == UCN_TIME_EVENT_TX) {
        if (!driver_callback_enter_locked(link)) {
            link_exit_task(link);
            callback_gate_exit_task(link->callback_gate);
            return UCN_ERR_STATE;
        }
        link_exit_task(link);
        callback_gate_exit_task(link->callback_gate);
        status = link->ops->cancel_tx_token(link->context, key);
        callback_gate_enter_task(link->callback_gate);
        link_enter_task(link);
        driver_callback_leave_locked(link);
        reservation = reservation_find_locked(link, key);
        if (status == UCN_OK && reservation == NULL) {
            link->faulted = true;
            status = UCN_ERR_STATE;
        }
    }
    if (status == UCN_OK) {
        (void)memset(reservation, 0, sizeof(*reservation));
        increment_saturated(&link->stats.retired_tokens);
    }
    link_exit_task(link);
    callback_gate_exit_task(link->callback_gate);
    return status;
}

/* EN: Quiesces callbacks before advancing Link Instance generation.
 * 中文：推进 Link Instance generation 前先静止全部回调。 */
ucn_result_t ucn_timed_link_reopen(ucn_timed_link_t *link)
{
    ucn_result_t status;

    if (link == NULL || !link->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    callback_gate_enter_task(link->callback_gate);
    if (link->callback_gate->active) {
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_enter_task(link);
    if (link->faulted || link->io_active) {
        link_exit_task(link);
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    if (link->link_instance_generation == UCN_TIME_EVENT_SERIAL_MAX) {
        link->faulted = true;
        link_exit_task(link);
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_EXHAUSTED;
    }
    if (!driver_callback_enter_locked(link)) {
        link_exit_task(link);
        callback_gate_exit_task(link->callback_gate);
        return UCN_ERR_STATE;
    }
    link_exit_task(link);
    callback_gate_exit_task(link->callback_gate);
    status = link->ops->quiesce(link->context);
    callback_gate_enter_task(link->callback_gate);
    link_enter_task(link);
    driver_callback_leave_locked(link);
    if (status == UCN_OK) {
        ++link->link_instance_generation;
        link->next_tx_token = 0U;
        link->next_rx_token = 0U;
        (void)memset(link->reservations, 0, sizeof(link->reservations));
        increment_saturated(&link->stats.reopen_count);
    }
    link_exit_task(link);
    callback_gate_exit_task(link->callback_gate);
    return status;
}

/* EN: Initializes a queue and validates paired task/ISR lock callbacks.
 * 中文：初始化队列并校验成对的任务/ISR 锁回调。 */
static ucn_result_t queue_configure(void *queue,
                                    size_t queue_size,
                                    const ucn_port_ops_t *port_ops,
                                    void *port_context,
                                    const ucn_port_ops_t **stored_ops,
                                    void **stored_context)
{
    if (queue == NULL || stored_ops == NULL || stored_context == NULL ||
        !port_lock_pairs_are_valid(port_ops)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(queue, 0, queue_size);
    *stored_ops = port_ops;
    *stored_context = port_context;
    return UCN_OK;
}

/* EN: Initializes one bounded TX timestamp event queue.
 * 中文：初始化一个有界 TX 时间戳事件队列。 */
ucn_result_t ucn_time_tx_event_queue_init(
    ucn_time_tx_event_queue_t *queue,
    const ucn_port_ops_t *port_ops,
    void *port_context)
{
    if (queue == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (queue_configure(queue, sizeof(*queue), port_ops, port_context,
                        &queue->port_ops, &queue->port_context) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    queue->initialized = true;
    return UCN_OK;
}

/* EN: Initializes one bounded atomic Timed RX queue.
 * 中文：初始化一个有界的原子 Timed RX 队列。 */
ucn_result_t ucn_time_timed_rx_queue_init(
    ucn_time_timed_rx_queue_t *queue,
    const ucn_port_ops_t *port_ops,
    void *port_context)
{
    if (queue == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (queue_configure(queue, sizeof(*queue), port_ops, port_context,
                        &queue->port_ops, &queue->port_context) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    queue->initialized = true;
    return UCN_OK;
}

/* EN: Enters a queue task lock when configured.
 * 中文：在配置后进入队列任务锁。 */
static void queue_enter_task(const ucn_port_ops_t *ops, void *context)
{
    if (ops != NULL && ops->enter_critical != NULL) {
        ops->enter_critical(context);
    }
}

/* EN: Leaves a queue task lock when configured.
 * 中文：在配置后退出队列任务锁。 */
static void queue_exit_task(const ucn_port_ops_t *ops, void *context)
{
    if (ops != NULL && ops->exit_critical != NULL) {
        ops->exit_critical(context);
    }
}

/* EN: Enqueues one TX event while the appropriate lock is held.
 * 中文：在持有正确锁时入队一个 TX 事件。 */
static ucn_result_t tx_enqueue_locked(ucn_time_tx_event_queue_t *queue,
                                      const ucn_time_tx_timestamp_event_t *event)
{
    if (!queue->initialized || queue->count > UCN_TIME_TX_EVENT_QUEUE_DEPTH ||
        queue->head >= UCN_TIME_TX_EVENT_QUEUE_DEPTH ||
        queue->tail >= UCN_TIME_TX_EVENT_QUEUE_DEPTH) {
        return UCN_ERR_STATE;
    }
    if (queue->count == UCN_TIME_TX_EVENT_QUEUE_DEPTH) {
        increment_saturated(&queue->dropped_full);
        return UCN_ERR_NO_SPACE;
    }
    queue->items[queue->tail] = *event;
    queue->tail = (queue->tail + 1U) % UCN_TIME_TX_EVENT_QUEUE_DEPTH;
    ++queue->count;
    increment_saturated(&queue->enqueued);
    return UCN_OK;
}

/* EN: Validates one complete TX timestamp event before ownership transfer.
 * 中文：所有权转移前校验一个完整 TX 时间戳事件。 */
static bool tx_event_is_valid(const ucn_time_tx_timestamp_event_t *event)
{
    return event != NULL && event->key.link_id != 0U &&
           event->key.direction == UCN_TIME_EVENT_TX &&
           event->key.link_instance_generation != 0U &&
           event->key.link_instance_generation <= UCN_TIME_EVENT_SERIAL_MAX &&
           event->key.event_token != 0U &&
           event->key.event_token <= UCN_TIME_EVENT_SERIAL_MAX &&
           event->completion <= UCN_OK &&
           event->completion >= UCN_ERR_EXHAUSTED;
}

/* EN: Enqueues one TX timestamp event from task context.
 * 中文：从任务上下文入队一个 TX 时间戳事件。 */
ucn_result_t ucn_time_tx_event_enqueue(
    ucn_time_tx_event_queue_t *queue,
    const ucn_time_tx_timestamp_event_t *event)
{
    ucn_result_t status;

    if (queue == NULL || !queue->initialized || !tx_event_is_valid(event)) {
        return UCN_ERR_ARGUMENT;
    }
    queue_enter_task(queue->port_ops, queue->port_context);
    status = tx_enqueue_locked(queue, event);
    queue_exit_task(queue->port_ops, queue->port_context);
    return status;
}

/* EN: Enqueues one complete TX timestamp event from ISR context.
 * 中文：从 ISR 上下文原子入队一个完整 TX 时间戳事件。 */
ucn_result_t ucn_time_tx_event_enqueue_from_isr(
    ucn_time_tx_event_queue_t *queue,
    const ucn_time_tx_timestamp_event_t *event)
{
    ucn_port_critical_token_t token;
    ucn_result_t status;

    if (queue == NULL || !queue->initialized || !tx_event_is_valid(event)) {
        return UCN_ERR_ARGUMENT;
    }
    if (queue->port_ops == NULL ||
        queue->port_ops->enter_critical_from_isr == NULL ||
        queue->port_ops->exit_critical_from_isr == NULL) {
        return UCN_ERR_CONFIG;
    }
    token = queue->port_ops->enter_critical_from_isr(queue->port_context);
    status = tx_enqueue_locked(queue, event);
    queue->port_ops->exit_critical_from_isr(queue->port_context, token);
    return status;
}

/* EN: Dequeues one complete TX timestamp event for the Time Owner.
 * 中文：为 Time Owner 出队一个完整 TX 时间戳事件。 */
ucn_result_t ucn_time_tx_event_dequeue(
    ucn_time_tx_event_queue_t *queue,
    ucn_time_tx_timestamp_event_t *event)
{
    ucn_time_tx_timestamp_event_t removed;

    if (queue == NULL || event == NULL || !queue->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    queue_enter_task(queue->port_ops, queue->port_context);
    if (queue->count > UCN_TIME_TX_EVENT_QUEUE_DEPTH ||
        queue->head >= UCN_TIME_TX_EVENT_QUEUE_DEPTH ||
        queue->tail >= UCN_TIME_TX_EVENT_QUEUE_DEPTH) {
        queue_exit_task(queue->port_ops, queue->port_context);
        return UCN_ERR_STATE;
    }
    if (queue->count == 0U) {
        queue_exit_task(queue->port_ops, queue->port_context);
        return UCN_ERR_NOT_FOUND;
    }
    removed = queue->items[queue->head];
    queue->head = (queue->head + 1U) % UCN_TIME_TX_EVENT_QUEUE_DEPTH;
    --queue->count;
    increment_saturated(&queue->dequeued);
    queue_exit_task(queue->port_ops, queue->port_context);
    *event = removed;
    return UCN_OK;
}

/* EN: Validates one complete Timed RX item before queue ownership.
 * 中文：队列取得所有权前校验一个完整 Timed RX Item。 */
static bool rx_item_is_valid(const ucn_time_timed_rx_item_t *item)
{
    return item != NULL && item->ingress_link != NULL &&
           item->key.direction == UCN_TIME_EVENT_RX &&
           item->key.link_id != 0U &&
           item->key.link_id == item->ingress_link->link_id &&
           item->key.link_instance_generation != 0U &&
           item->key.link_instance_generation <= UCN_TIME_EVENT_SERIAL_MAX &&
           item->key.event_token != 0U &&
           item->key.event_token <= UCN_TIME_EVENT_SERIAL_MAX &&
           item->length != 0U && item->length <= UCN_MAX_FRAME_BYTES;
}

/* EN: Enqueues one atomic Timed RX item under the caller's lock.
 * 中文：在调用者锁下原子入队一个 Timed RX Item。 */
static ucn_result_t rx_enqueue_locked(ucn_time_timed_rx_queue_t *queue,
                                      const ucn_time_timed_rx_item_t *item)
{
    if (!queue->initialized ||
        queue->count > UCN_TIME_TIMED_RX_QUEUE_DEPTH ||
        queue->head >= UCN_TIME_TIMED_RX_QUEUE_DEPTH ||
        queue->tail >= UCN_TIME_TIMED_RX_QUEUE_DEPTH) {
        return UCN_ERR_STATE;
    }
    if (queue->count == UCN_TIME_TIMED_RX_QUEUE_DEPTH) {
        increment_saturated(&queue->dropped_full);
        return UCN_ERR_NO_SPACE;
    }
    queue->items[queue->tail] = *item;
    queue->tail = (queue->tail + 1U) % UCN_TIME_TIMED_RX_QUEUE_DEPTH;
    ++queue->count;
    increment_saturated(&queue->enqueued);
    return UCN_OK;
}

/* EN: Enqueues one atomic Timed RX item from task context.
 * 中文：从任务上下文原子入队一个 Timed RX Item。 */
ucn_result_t ucn_time_timed_rx_enqueue(
    ucn_time_timed_rx_queue_t *queue,
    const ucn_time_timed_rx_item_t *item)
{
    ucn_result_t status;

    if (queue == NULL || !queue->initialized || !rx_item_is_valid(item)) {
        return UCN_ERR_ARGUMENT;
    }
    queue_enter_task(queue->port_ops, queue->port_context);
    status = rx_enqueue_locked(queue, item);
    queue_exit_task(queue->port_ops, queue->port_context);
    return status;
}

/* EN: Enqueues one inseparable frame/key/timestamp record from ISR.
 * 中文：从 ISR 入队不可拆分的 frame/key/timestamp 记录。 */
ucn_result_t ucn_time_timed_rx_enqueue_from_isr(
    ucn_time_timed_rx_queue_t *queue,
    const ucn_time_timed_rx_item_t *item)
{
    ucn_port_critical_token_t token;
    ucn_result_t status;

    if (queue == NULL || !queue->initialized || !rx_item_is_valid(item)) {
        return UCN_ERR_ARGUMENT;
    }
    if (queue->port_ops == NULL ||
        queue->port_ops->enter_critical_from_isr == NULL ||
        queue->port_ops->exit_critical_from_isr == NULL) {
        return UCN_ERR_CONFIG;
    }
    token = queue->port_ops->enter_critical_from_isr(queue->port_context);
    status = rx_enqueue_locked(queue, item);
    queue->port_ops->exit_critical_from_isr(queue->port_context, token);
    return status;
}

/* EN: Dequeues one complete Timed RX item for the Time Owner.
 * 中文：为 Time Owner 出队一个完整 Timed RX Item。 */
ucn_result_t ucn_time_timed_rx_dequeue(
    ucn_time_timed_rx_queue_t *queue,
    ucn_time_timed_rx_item_t *item)
{
    ucn_time_timed_rx_item_t removed;

    if (queue == NULL || item == NULL || !queue->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    queue_enter_task(queue->port_ops, queue->port_context);
    if (queue->count > UCN_TIME_TIMED_RX_QUEUE_DEPTH ||
        queue->head >= UCN_TIME_TIMED_RX_QUEUE_DEPTH ||
        queue->tail >= UCN_TIME_TIMED_RX_QUEUE_DEPTH) {
        queue_exit_task(queue->port_ops, queue->port_context);
        return UCN_ERR_STATE;
    }
    if (queue->count == 0U) {
        queue_exit_task(queue->port_ops, queue->port_context);
        return UCN_ERR_NOT_FOUND;
    }
    removed = queue->items[queue->head];
    queue->head = (queue->head + 1U) % UCN_TIME_TIMED_RX_QUEUE_DEPTH;
    --queue->count;
    increment_saturated(&queue->dequeued);
    queue_exit_task(queue->port_ops, queue->port_context);
    *item = removed;
    return UCN_OK;
}
