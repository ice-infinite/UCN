#include <string.h>

#include "ucn/adapters/ucn_can_source.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_time.h"

/*
 * EN: Enters or leaves the bounded `enter_task` critical section for CAN Source.
 * 中文：进入或退出 CAN Source 的有界 `enter_task` 临界区。
 */
static void can_enter_task(ucn_can_source_t *source)
{
    if (source->port_ops->enter_critical != NULL) {
        source->port_ops->enter_critical(source->port_context);
    }
}

/*
 * EN: Enters or leaves the bounded `exit_task` critical section for CAN Source.
 * 中文：进入或退出 CAN Source 的有界 `exit_task` 临界区。
 */
static void can_exit_task(ucn_can_source_t *source)
{
    if (source->port_ops->exit_critical != NULL) {
        source->port_ops->exit_critical(source->port_context);
    }
}

/*
 * EN: Checks the `isr_lock_is_configured` condition against current CAN Source state.
 * 中文：根据当前 CAN Source 状态检查 `isr_lock_is_configured` 条件。
 */
static bool can_isr_lock_is_configured(const ucn_can_source_t *source)
{
    return source->port_ops->enter_critical_from_isr != NULL &&
           source->port_ops->exit_critical_from_isr != NULL;
}

/*
 * EN: Enters or leaves the bounded `enter_isr` critical section for CAN Source.
 * 中文：进入或退出 CAN Source 的有界 `enter_isr` 临界区。
 */
static ucn_port_critical_token_t can_enter_isr(ucn_can_source_t *source)
{
    return source->port_ops->enter_critical_from_isr(source->port_context);
}

/*
 * EN: Enters or leaves the bounded `exit_isr` critical section for CAN Source.
 * 中文：进入或退出 CAN Source 的有界 `exit_isr` 临界区。
 */
static void can_exit_isr(ucn_can_source_t *source,
                         ucn_port_critical_token_t token)
{
    source->port_ops->exit_critical_from_isr(source->port_context, token);
}

/*
 * EN: Checks whether `fd_length` satisfies the CAN Source module's validity rules.
 * 中文：检查 `fd_length` 是否满足 CAN Source 模块的合法性规则。
 */
static bool can_fd_length_is_valid(size_t length)
{
    return length <= 8U || length == 12U || length == 16U ||
           length == 20U || length == 24U || length == 32U ||
           length == 48U || length == 64U;
}

/*
 * EN: Rounds `fd_rounded_length` to the next representation accepted by CAN Source.
 * 中文：把 `fd_rounded_length` 向上取整为 CAN Source 可接受的下一种表示。
 */
static size_t can_fd_rounded_length(size_t length)
{
    static const uint8_t lengths[] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
        12U, 16U, 20U, 24U, 32U, 48U, 64U
    };
    size_t index;

    for (index = 0U; index < sizeof(lengths); ++index) {
        if ((size_t)lengths[index] >= length) {
            return lengths[index];
        }
    }
    return 0U;
}

/*
 * EN: Checks whether `frame_shape` satisfies the CAN Source module's validity rules.
 * 中文：检查 `frame_shape` 是否满足 CAN Source 模块的合法性规则。
 */
static bool can_frame_shape_is_valid(const ucn_can_frame_t *frame)
{
    const bool extended =
        (frame->flags & UCN_CAN_FRAME_FLAG_EXTENDED) != 0U;
    const bool fd = (frame->flags & UCN_CAN_FRAME_FLAG_FD) != 0U;

    if ((frame->flags & (ucn_can_frame_flags_t)~UCN_CAN_FRAME_KNOWN_FLAGS) !=
            0U ||
        frame->identifier > (extended ? UINT32_C(0x1FFFFFFF) :
                                        UINT32_C(0x7FF)) ||
        (frame->flags & UCN_CAN_FRAME_FLAG_ERROR) != 0U ||
        ((frame->flags & UCN_CAN_FRAME_FLAG_BRS) != 0U && !fd) ||
        (fd && (frame->flags & UCN_CAN_FRAME_FLAG_RTR) != 0U)) {
        return false;
    }
    return fd ? can_fd_length_is_valid(frame->length) :
                frame->length <= UCN_CAN_CLASSIC_MAX_DATA_BYTES;
}

/*
 * EN: Checks the `mode_accepts` condition against current CAN Source state.
 * 中文：根据当前 CAN Source 状态检查 `mode_accepts` 条件。
 */
static bool can_mode_accepts(const ucn_can_source_t *source, bool fd)
{
    return source->mode == UCN_CAN_SOURCE_MIXED ||
           (fd && source->mode == UCN_CAN_SOURCE_CAN_FD_DIRECT) ||
           (!fd && source->mode == UCN_CAN_SOURCE_CLASSIC_CARRIER);
}

/*
 * EN: Clears `slot` from CAN Source without allocating memory.
 * 中文：从 CAN Source 中清除 `slot`，且不进行动态分配。
 */
static void can_clear_slot(ucn_can_reassembly_slot_t *slot)
{
    uint8_t *storage = slot->frame_storage;

    (void)memset(slot, 0, sizeof(*slot));
    slot->frame_storage = storage;
}

/*
 * EN: Clears `slots` from CAN Source without allocating memory.
 * 中文：从 CAN Source 中清除 `slots`，且不进行动态分配。
 */
static size_t can_clear_slots(ucn_can_source_t *source)
{
    size_t index;
    size_t cleared = 0U;

    for (index = 0U; index < source->reassembly_slot_count; ++index) {
        if (source->reassembly_slots[index].active) {
            cleared++;
        }
        can_clear_slot(&source->reassembly_slots[index]);
    }
    return cleared;
}

/*
 * EN: Removes and returns `take_reassembly_clear` from a bounded CAN Source queue or slot.
 * 中文：从固定容量的 CAN Source 队列或槽位中移除并返回 `take_reassembly_clear`。
 */
static bool can_take_reassembly_clear(ucn_can_source_t *source)
{
    bool clear;

    can_enter_task(source);
    clear = source->clear_reassembly_pending;
    source->clear_reassembly_pending = false;
    can_exit_task(source);
    return clear;
}

/*
 * EN: Returns the current `bus_state` view from CAN Source state.
 * 中文：从 CAN Source 状态返回当前 `bus_state` 视图。
 */
static ucn_can_bus_state_t can_get_bus_state(ucn_can_source_t *source)
{
    ucn_can_bus_state_t state;

    can_enter_task(source);
    state = source->bus_state;
    can_exit_task(source);
    return state;
}

/*
 * EN: Looks up `ring_peek` in bounded CAN Source state without allocation.
 * 中文：在固定容量的 CAN Source 状态中查找 `ring_peek`，且不进行动态分配。
 */
static bool can_ring_peek(ucn_can_source_t *source, ucn_can_frame_t *frame)
{
    bool present;

    can_enter_task(source);
    present = source->ring_count != 0U;
    if (present) {
        *frame = source->ring_storage[source->ring_tail];
    }
    can_exit_task(source);
    return present;
}

/*
 * EN: Removes and returns `ring_pop` from bounded CAN Source storage.
 * 中文：从固定容量的 CAN Source 存储中移除并返回 `ring_pop`。
 */
static void can_ring_pop(ucn_can_source_t *source)
{
    can_enter_task(source);
    if (source->ring_count != 0U) {
        source->ring_tail = (source->ring_tail + 1U) % source->ring_capacity;
        source->ring_count--;
    }
    can_exit_task(source);
}

/*
 * EN: Calculates the bounded `ring_count` value used by CAN Source.
 * 中文：计算 CAN Source 使用的有界 `ring_count` 值。
 */
static size_t can_ring_count(ucn_can_source_t *source)
{
    size_t count;

    can_enter_task(source);
    count = source->ring_count;
    can_exit_task(source);
    return count;
}

/*
 * EN: Checks the `has_complete_slot` condition in current CAN Source state.
 * 中文：检查当前 CAN Source 状态中的 `has_complete_slot` 条件。
 */
static bool can_has_complete_slot(const ucn_can_source_t *source)
{
    size_t index;

    for (index = 0U; index < source->reassembly_slot_count; ++index) {
        if (source->reassembly_slots[index].active &&
            source->reassembly_slots[index].complete) {
            return true;
        }
    }
    return false;
}

/*
 * EN: Selects or resolves `resolve_link` using deterministic CAN Source rules.
 * 中文：按照确定性的 CAN Source 规则选择或解析 `resolve_link`。
 */
static ucn_result_t can_resolve_link(ucn_can_source_t *source,
                                     const ucn_can_frame_t *frame,
                                     ucn_link_t **link)
{
    ucn_result_t result = source->resolve_ingress(
        source->resolve_context, frame->identifier,
        (frame->flags & UCN_CAN_FRAME_FLAG_EXTENDED) != 0U, link);

    if (result == UCN_ERR_NOT_FOUND) {
        source->stats.filtered_frames++;
        return result;
    }
    if (result != UCN_OK || *link == NULL) {
        source->stats.resolver_errors++;
        return result == UCN_OK ? UCN_ERR_CONFIG : result;
    }
    return UCN_OK;
}

/*
 * EN: Validates and processes `process_fd` in the CAN Source receive path.
 * 中文：在 CAN Source 接收路径中验证并处理 `process_fd`。
 */
static ucn_result_t can_process_fd(ucn_can_source_t *source,
                                   const ucn_can_frame_t *physical,
                                   bool *queue_backpressure)
{
    ucn_link_t *link = NULL;
    size_t encoded_length = 0U;
    size_t index;
    ucn_result_t result;

    *queue_backpressure = false;
    result = can_resolve_link(source, physical, &link);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_frame_peek_encoded_size(physical->data, physical->length,
                                         &encoded_length);
    if (result != UCN_OK || encoded_length > UCN_CAN_FD_MAX_DATA_BYTES) {
        source->stats.frame_length_errors++;
        return result == UCN_OK ? UCN_ERR_TOO_LARGE : result;
    }
    for (index = encoded_length; index < physical->length; ++index) {
        if (physical->data[index] != 0U) {
            source->stats.fd_padding_errors++;
            return UCN_ERR_MALFORMED;
        }
    }
    source->stats.fd_frames++;
    result = ucn_event_runtime_submit_frame(source->runtime, link,
                                             physical->data, encoded_length);
    if (result == UCN_OK) {
        source->stats.frames_submitted++;
    } else if (result == UCN_ERR_NO_SPACE) {
        source->stats.adapter_queue_backpressure++;
        *queue_backpressure = true;
    } else {
        source->stats.frames_rejected++;
    }
    return result;
}

/*
 * EN: Searches bounded CAN Source state for `slot`.
 * 中文：在固定容量的 CAN Source 状态中查找 `slot`。
 */
static ucn_can_reassembly_slot_t *can_find_slot(
    ucn_can_source_t *source,
    uint32_t identifier,
    bool extended)
{
    size_t index;

    for (index = 0U; index < source->reassembly_slot_count; ++index) {
        ucn_can_reassembly_slot_t *slot = &source->reassembly_slots[index];

        if (slot->active && slot->identifier == identifier &&
            slot->extended == extended) {
            return slot;
        }
    }
    return NULL;
}

/*
 * EN: Searches bounded CAN Source state for `free_slot`.
 * 中文：在固定容量的 CAN Source 状态中查找 `free_slot`。
 */
static ucn_can_reassembly_slot_t *can_find_free_slot(ucn_can_source_t *source)
{
    size_t index;

    for (index = 0U; index < source->reassembly_slot_count; ++index) {
        if (!source->reassembly_slots[index].active) {
            return &source->reassembly_slots[index];
        }
    }
    return NULL;
}

/*
 * EN: Validates and processes `process_classic` in the CAN Source receive path.
 * 中文：在 CAN Source 接收路径中验证并处理 `process_classic`。
 */
static ucn_result_t can_process_classic(ucn_can_source_t *source,
                                        const ucn_can_frame_t *physical,
                                        uint32_t now_ms,
                                        bool *hold_physical)
{
    const bool extended =
        (physical->flags & UCN_CAN_FRAME_FLAG_EXTENDED) != 0U;
    ucn_can_reassembly_slot_t *slot;
    ucn_link_t *link = NULL;
    uint8_t marker;
    ucn_result_t result;

    *hold_physical = false;
    if (physical->length == 0U ||
        (physical->flags & UCN_CAN_FRAME_FLAG_RTR) != 0U) {
        source->stats.physical_format_errors++;
        return UCN_ERR_MALFORMED;
    }
    result = can_resolve_link(source, physical, &link);
    if (result != UCN_OK) {
        return result;
    }
    marker = physical->data[0];
    if (marker == UCN_CAN_CLASSIC_CARRIER_START) {
        size_t total_length;

        source->stats.classic_start_frames++;
        if (physical->length != UCN_CAN_CLASSIC_MAX_DATA_BYTES ||
            physical->data[2] != 0U) {
            source->stats.physical_format_errors++;
            return UCN_ERR_MALFORMED;
        }
        total_length = ((size_t)physical->data[3] << 8U) |
                       (size_t)physical->data[4];
        if (total_length < UCN_FRAME_W0_HEADER_SIZE ||
            total_length > source->max_frame_bytes ||
            ucn_can_classic_carrier_segment_count(total_length) == 0U) {
            source->stats.frame_length_errors++;
            return UCN_ERR_TOO_LARGE;
        }
        slot = can_find_slot(source, physical->identifier, extended);
        if (slot != NULL) {
            if (slot->complete) {
                /* A completed Carrier owns its slot until the common Adapter
                 * Queue accepts it.  Do not consume the next START: the
                 * service loop will retry it after submitting this slot. */
                *hold_physical = true;
                return UCN_ERR_NO_SPACE;
            }
            source->stats.carrier_restarts++;
            can_clear_slot(slot);
        } else {
            slot = can_find_free_slot(source);
        }
        if (slot == NULL) {
            source->stats.carrier_no_slot++;
            return UCN_ERR_NO_SPACE;
        }
        slot->identifier = physical->identifier;
        slot->extended = extended;
        slot->ingress_link = link;
        slot->transfer_id = physical->data[1];
        slot->next_segment_index = 1U;
        slot->expected_length = total_length;
        slot->received_length = 3U;
        slot->deadline_ms = ucn_deadline_from_now(
            now_ms, source->reassembly_timeout_ms);
        slot->active = true;
        (void)memcpy(slot->frame_storage, &physical->data[5], 3U);
        return UCN_OK;
    }
    if (marker == UCN_CAN_CLASSIC_CARRIER_CONTINUE) {
        size_t remaining;
        size_t chunk_length;
        size_t expected_chunk;
        size_t encoded_length = 0U;

        source->stats.classic_continue_frames++;
        slot = can_find_slot(source, physical->identifier, extended);
        if (slot == NULL) {
            source->stats.carrier_order_errors++;
            return UCN_ERR_NOT_FOUND;
        }
        chunk_length = physical->length >= 3U ? physical->length - 3U : 0U;
        remaining = slot->expected_length - slot->received_length;
        expected_chunk = remaining < 5U ? remaining : 5U;
        if (physical->length < 4U || physical->data[1] != slot->transfer_id ||
            physical->data[2] == 0U ||
            physical->data[2] != slot->next_segment_index ||
            chunk_length != expected_chunk || slot->ingress_link != link) {
            source->stats.carrier_order_errors++;
            can_clear_slot(slot);
            return UCN_ERR_MALFORMED;
        }
        (void)memcpy(&slot->frame_storage[slot->received_length],
                     &physical->data[3], chunk_length);
        slot->received_length += chunk_length;
        slot->next_segment_index++;
        slot->deadline_ms = ucn_deadline_from_now(
            now_ms, source->reassembly_timeout_ms);
        if (slot->received_length == slot->expected_length) {
            result = ucn_frame_peek_encoded_size(
                slot->frame_storage, slot->expected_length, &encoded_length);
            if (result != UCN_OK || encoded_length != slot->expected_length) {
                source->stats.frame_length_errors++;
                can_clear_slot(slot);
                return result == UCN_OK ? UCN_ERR_MALFORMED : result;
            }
            slot->complete = true;
            slot->deadline_ms = 0U;
            source->stats.carrier_completed++;
        }
        return UCN_OK;
    }
    source->stats.physical_format_errors++;
    return UCN_ERR_MALFORMED;
}

/*
 * EN: Checks or removes expired `expire_slots` state in CAN Source.
 * 中文：检查或移除 CAN Source 中已过期的 `expire_slots` 状态。
 */
static size_t can_expire_slots(ucn_can_source_t *source,
                               uint32_t now_ms,
                               size_t max_work)
{
    size_t index;
    size_t expired = 0U;

    for (index = 0U; index < source->reassembly_slot_count &&
                     expired < max_work; ++index) {
        ucn_can_reassembly_slot_t *slot = &source->reassembly_slots[index];

        if (slot->active && !slot->complete &&
            ucn_deadline_expired(now_ms, slot->deadline_ms)) {
            can_clear_slot(slot);
            source->stats.carrier_timeouts++;
            expired++;
        }
    }
    return expired;
}

/*
 * EN: Completes `submit_complete_slots` and records its terminal CAN Source result.
 * 中文：完成 `submit_complete_slots` 并记录其 CAN Source 终态结果。
 */
static ucn_result_t can_submit_complete_slots(
    ucn_can_source_t *source,
    size_t max_work,
    size_t *work_done)
{
    size_t index;

    for (index = 0U; index < source->reassembly_slot_count &&
                     *work_done < max_work; ++index) {
        ucn_can_reassembly_slot_t *slot = &source->reassembly_slots[index];
        ucn_result_t result;

        if (!slot->active || !slot->complete) {
            continue;
        }
        result = ucn_event_runtime_submit_frame(
            source->runtime, slot->ingress_link, slot->frame_storage,
            slot->expected_length);
        (*work_done)++;
        if (result == UCN_ERR_NO_SPACE) {
            source->stats.adapter_queue_backpressure++;
            return result;
        }
        if (result == UCN_OK) {
            source->stats.frames_submitted++;
        } else {
            source->stats.frames_rejected++;
        }
        can_clear_slot(slot);
    }
    return UCN_OK;
}

/*
 * EN: Processes one bounded `source_service` work unit for CAN Source.
 * 中文：为 CAN Source 处理一个有界的 `source_service` 工作单元。
 */
static ucn_result_t can_source_service(
    void *context,
    ucn_event_source_events_t events,
    size_t max_work,
    ucn_event_source_service_result_t *service_result)
{
    ucn_can_source_t *source = (ucn_can_source_t *)context;
    uint32_t now_ms;
    size_t expired;
    ucn_result_t submit_result;

    if (source == NULL || !source->initialized || service_result == NULL ||
        max_work == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(service_result, 0, sizeof(*service_result));
    source->stats.service_calls++;
    if ((events & UCN_EVENT_SOURCE_FALLBACK_SCAN) != 0U) {
        source->stats.fallback_services++;
    }
    now_ms = source->port_ops->now_ms(source->port_context);

    if (can_take_reassembly_clear(source)) {
        (void)can_clear_slots(source);
    }
    if (can_get_bus_state(source) == UCN_CAN_BUS_OFF ||
        can_get_bus_state(source) == UCN_CAN_BUS_RECOVERING) {
        (void)can_clear_slots(source);
        return UCN_OK;
    }

    expired = can_expire_slots(source, now_ms, max_work);
    service_result->work_done += expired;
    submit_result = can_submit_complete_slots(
        source, max_work, &service_result->work_done);
    if (submit_result == UCN_ERR_NO_SPACE) {
        service_result->pending_events = UCN_EVENT_SOURCE_RX_READY;
        return UCN_OK;
    }

    while (service_result->work_done < max_work) {
        ucn_can_frame_t physical;
        const bool have_frame = can_ring_peek(source, &physical);
        bool fd;
        bool queue_backpressure = false;
        bool hold_physical = false;

        if (!have_frame) {
            break;
        }
        fd = (physical.flags & UCN_CAN_FRAME_FLAG_FD) != 0U;
        if (!can_mode_accepts(source, fd)) {
            source->stats.physical_format_errors++;
        } else if (fd) {
            (void)can_process_fd(source, &physical, &queue_backpressure);
        } else {
            (void)can_process_classic(source, &physical, now_ms,
                                      &hold_physical);
        }
        service_result->work_done++;
        if (queue_backpressure || hold_physical) {
            break;
        }
        can_ring_pop(source);
        if (!fd && can_has_complete_slot(source)) {
            /* Submit a completed Classic Carrier before touching a following
             * physical frame.  This preserves back-to-back Carriers that use
             * the same CAN ID even when one Runtime round drains many frames. */
            break;
        }
    }

    if (can_ring_count(source) != 0U || can_has_complete_slot(source)) {
        service_result->pending_events = UCN_EVENT_SOURCE_RX_READY;
    }
    return UCN_OK;
}

static const ucn_event_source_ops_t CAN_EVENT_SOURCE_OPS = {
    can_source_service
};

/*
 * EN: Initializes the CAN Source object from validated caller-owned configuration without heap allocation.
 * 中文：使用经验证的调用方配置初始化 CAN Source 对象，且不使用堆内存。
 */
ucn_result_t ucn_can_source_init(
    ucn_can_source_t *source,
    const ucn_can_source_config_t *config)
{
    ucn_event_source_config_t event_config;
    const ucn_port_ops_t *port_ops;
    size_t max_frame_bytes;
    size_t index;
    ucn_result_t result;

    if (source == NULL || config == NULL || config->runtime == NULL ||
        !config->runtime->initialized || config->ring_storage == NULL ||
        config->ring_capacity == 0U || config->resolve_ingress == NULL ||
        config->mode > UCN_CAN_SOURCE_MIXED) {
        return UCN_ERR_ARGUMENT;
    }
    max_frame_bytes = config->max_frame_bytes == 0U ? UCN_MAX_FRAME_BYTES :
                                                       config->max_frame_bytes;
    if (max_frame_bytes < UCN_FRAME_W0_HEADER_SIZE ||
        max_frame_bytes > UCN_MAX_FRAME_BYTES) {
        return UCN_ERR_CONFIG;
    }
    if (config->mode != UCN_CAN_SOURCE_CAN_FD_DIRECT &&
        (max_frame_bytes > UCN_CAN_CLASSIC_CARRIER_MAX_FRAME_BYTES ||
         config->reassembly_slots == NULL ||
         config->reassembly_slot_count == 0U ||
         config->reassembly_storage == NULL ||
         config->reassembly_slot_count >
             SIZE_MAX / max_frame_bytes ||
         config->reassembly_storage_capacity <
             config->reassembly_slot_count * max_frame_bytes)) {
        return UCN_ERR_CONFIG;
    }
    if (config->mode == UCN_CAN_SOURCE_CAN_FD_DIRECT &&
        max_frame_bytes > UCN_CAN_FD_MAX_DATA_BYTES) {
        return UCN_ERR_CONFIG;
    }
    port_ops = config->runtime->owner.config.port_ops;
    if (!ucn_port_ops_is_compatible(port_ops) ||
        port_ops->now_ms == NULL ||
        (port_ops->enter_critical == NULL) !=
            (port_ops->exit_critical == NULL) ||
        (port_ops->enter_critical_from_isr == NULL) !=
            (port_ops->exit_critical_from_isr == NULL)) {
        return UCN_ERR_CONFIG;
    }

    (void)memset(source, 0, sizeof(*source));
    source->runtime = config->runtime;
    source->source_id = config->source_id;
    source->mode = config->mode;
    source->ring_storage = config->ring_storage;
    source->ring_capacity = config->ring_capacity;
    source->reassembly_slots = config->reassembly_slots;
    source->reassembly_slot_count =
        config->mode == UCN_CAN_SOURCE_CAN_FD_DIRECT ? 0U :
                                                       config->reassembly_slot_count;
    source->max_frame_bytes = max_frame_bytes;
    source->reassembly_timeout_ms =
        config->reassembly_timeout_ms == 0U ?
            UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_TIMEOUT_MS :
            config->reassembly_timeout_ms;
    if (!ucn_duration_is_valid(source->reassembly_timeout_ms)) {
        return UCN_ERR_CONFIG;
    }
    source->resolve_ingress = config->resolve_ingress;
    source->resolve_context = config->resolve_context;
    source->port_ops = port_ops;
    source->port_context = config->runtime->owner.config.port_context;
    source->bus_state = UCN_CAN_BUS_ACTIVE;

    for (index = 0U; index < source->reassembly_slot_count; ++index) {
        (void)memset(&source->reassembly_slots[index], 0,
                     sizeof(source->reassembly_slots[index]));
        source->reassembly_slots[index].frame_storage =
            &config->reassembly_storage[index * max_frame_bytes];
    }

    event_config.ops = &CAN_EVENT_SOURCE_OPS;
    event_config.context = source;
    result = ucn_event_runtime_bind_source(
        config->runtime, config->source_id, &event_config);
    if (result != UCN_OK) {
        return result;
    }
    source->initialized = true;
    return UCN_OK;
}

/*
 * EN: Writes `source_write` using the canonical bounded CAN Source representation.
 * 中文：使用规范且有界的 CAN Source 表示写入 `source_write`。
 */
static ucn_result_t can_source_write(ucn_can_source_t *source,
                                     const ucn_can_frame_t *frame,
                                     bool from_isr)
{
    ucn_port_critical_token_t token = 0U;
    ucn_result_t write_result = UCN_OK;
    ucn_result_t signal_result;

    if (source == NULL || !source->initialized || frame == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!can_frame_shape_is_valid(frame)) {
        return UCN_ERR_MALFORMED;
    }
    if (from_isr && !can_isr_lock_is_configured(source)) {
        return UCN_ERR_CONFIG;
    }

    if (from_isr) {
        token = can_enter_isr(source);
    } else {
        can_enter_task(source);
    }
    if (from_isr) {
        source->stats.isr_writes++;
    } else {
        source->stats.task_writes++;
    }
    if (source->bus_state == UCN_CAN_BUS_OFF ||
        source->bus_state == UCN_CAN_BUS_RECOVERING) {
        source->stats.physical_frames_rejected++;
        write_result = UCN_ERR_LINK_DOWN;
    } else if (source->ring_count == source->ring_capacity) {
        source->stats.ring_no_space++;
        source->stats.physical_frames_rejected++;
        write_result = UCN_ERR_NO_SPACE;
    } else {
        source->ring_storage[source->ring_head] = *frame;
        source->ring_head = (source->ring_head + 1U) % source->ring_capacity;
        source->ring_count++;
        source->stats.physical_frames_accepted++;
        if (source->ring_count > source->stats.ring_high_water) {
            source->stats.ring_high_water = source->ring_count;
        }
    }
    if (from_isr) {
        can_exit_isr(source, token);
    } else {
        can_exit_task(source);
    }
    if (write_result != UCN_OK) {
        return write_result;
    }

    signal_result = from_isr ?
        ucn_event_runtime_signal_source_from_isr(
            source->runtime, source->source_id,
            UCN_EVENT_SOURCE_RX_READY) :
        ucn_event_runtime_signal_source(source->runtime, source->source_id,
                                        UCN_EVENT_SOURCE_RX_READY);
    if (signal_result != UCN_OK) {
        if (from_isr) {
            token = can_enter_isr(source);
            source->stats.notification_errors++;
            can_exit_isr(source, token);
        } else {
            can_enter_task(source);
            source->stats.notification_errors++;
            can_exit_task(source);
        }
    }
    return signal_result;
}

/*
 * EN: Writes `write` using the canonical bounded CAN Source representation.
 * 中文：使用规范且有界的 CAN Source 表示写入 `write`。
 */
ucn_result_t ucn_can_source_write(ucn_can_source_t *source,
                                  const ucn_can_frame_t *frame)
{
    return can_source_write(source, frame, false);
}

/*
 * EN: Writes `from_isr` in the canonical CAN Source byte order.
 * 中文：按规范的 CAN Source 字节序写入 `from_isr`。
 */
ucn_result_t ucn_can_source_write_from_isr(ucn_can_source_t *source,
                                           const ucn_can_frame_t *frame)
{
    return can_source_write(source, frame, true);
}

/*
 * EN: Validates and installs `source_set_bus_state` in bounded CAN Source state.
 * 中文：验证 `source_set_bus_state` 并将其安装到固定容量的 CAN Source 状态中。
 */
static ucn_result_t can_source_set_bus_state(ucn_can_source_t *source,
                                             ucn_can_bus_state_t state,
                                             bool from_isr)
{
    ucn_port_critical_token_t token = 0U;
    ucn_result_t signal_result;
    size_t flushed = 0U;

    if (source == NULL || !source->initialized ||
        state > UCN_CAN_BUS_RECOVERING) {
        return UCN_ERR_ARGUMENT;
    }
    if (from_isr && !can_isr_lock_is_configured(source)) {
        return UCN_ERR_CONFIG;
    }
    if (from_isr) {
        token = can_enter_isr(source);
    } else {
        can_enter_task(source);
    }
    if (state != source->bus_state) {
        const ucn_can_bus_state_t previous = source->bus_state;

        source->bus_state = state;
        source->stats.status_changes++;
        if (state == UCN_CAN_BUS_ERROR_PASSIVE) {
            source->stats.error_passive_events++;
        }
        if (state == UCN_CAN_BUS_OFF) {
            source->stats.bus_off_events++;
        }
        if (state == UCN_CAN_BUS_ACTIVE &&
            (previous == UCN_CAN_BUS_OFF ||
             previous == UCN_CAN_BUS_RECOVERING)) {
            source->stats.recoveries++;
        }
        if (state == UCN_CAN_BUS_OFF ||
            state == UCN_CAN_BUS_RECOVERING) {
            flushed = source->ring_count;
            source->ring_head = 0U;
            source->ring_tail = 0U;
            source->ring_count = 0U;
            source->clear_reassembly_pending = true;
            source->stats.frames_flushed_on_down += (uint32_t)flushed;
        }
    }
    if (from_isr) {
        can_exit_isr(source, token);
    } else {
        can_exit_task(source);
    }

    signal_result = from_isr ?
        ucn_event_runtime_signal_source_from_isr(
            source->runtime, source->source_id,
            UCN_EVENT_SOURCE_STATUS_CHANGED) :
        ucn_event_runtime_signal_source(
            source->runtime, source->source_id,
            UCN_EVENT_SOURCE_STATUS_CHANGED);
    if (signal_result != UCN_OK) {
        if (from_isr) {
            token = can_enter_isr(source);
            source->stats.notification_errors++;
            can_exit_isr(source, token);
        } else {
            can_enter_task(source);
            source->stats.notification_errors++;
            can_exit_task(source);
        }
    }
    return signal_result;
}

/*
 * EN: Validates and sets `bus_state` in CAN Source state.
 * 中文：验证并设置 CAN Source 状态中的 `bus_state`。
 */
ucn_result_t ucn_can_source_set_bus_state(ucn_can_source_t *source,
                                          ucn_can_bus_state_t state)
{
    return can_source_set_bus_state(source, state, false);
}

/*
 * EN: Validates and sets `bus_state_from_isr` in CAN Source state.
 * 中文：验证并设置 CAN Source 状态中的 `bus_state_from_isr`。
 */
ucn_result_t ucn_can_source_set_bus_state_from_isr(
    ucn_can_source_t *source,
    ucn_can_bus_state_t state)
{
    return can_source_set_bus_state(source, state, true);
}

/*
 * EN: Clears or releases `reset` from bounded CAN Source state.
 * 中文：从固定容量的 CAN Source 状态中清除或释放 `reset`。
 */
ucn_result_t ucn_can_source_reset(ucn_can_source_t *source)
{
    if (source == NULL || !source->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    can_enter_task(source);
    source->ring_head = 0U;
    source->ring_tail = 0U;
    source->ring_count = 0U;
    source->clear_reassembly_pending = false;
    can_exit_task(source);
    (void)can_clear_slots(source);
    return UCN_OK;
}

/*
 * EN: Returns the current `health` view from CAN Source state.
 * 中文：从 CAN Source 状态返回当前 `health` 视图。
 */
ucn_result_t ucn_can_source_get_health(
    ucn_can_source_t *source,
    ucn_can_source_health_t *health)
{
    size_t index;

    if (source == NULL || !source->initialized || health == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(health, 0, sizeof(*health));
    can_enter_task(source);
    health->bus_state = source->bus_state;
    health->pending_physical_frames = source->ring_count;
    health->queue_pressure_per_mille = (uint16_t)(
        (source->ring_count * 1000U) / source->ring_capacity);
    health->receive_failure_count =
        source->stats.physical_frames_rejected +
        source->stats.filtered_frames + source->stats.resolver_errors +
        source->stats.physical_format_errors +
        source->stats.fd_padding_errors +
        source->stats.carrier_order_errors +
        source->stats.carrier_no_slot + source->stats.carrier_timeouts +
        source->stats.frame_length_errors +
        source->stats.frames_rejected +
        source->stats.adapter_queue_backpressure;
    can_exit_task(source);
    for (index = 0U; index < source->reassembly_slot_count; ++index) {
        if (source->reassembly_slots[index].active) {
            health->active_reassembly_slots++;
        }
    }
    return UCN_OK;
}

/*
 * EN: Returns the current `stats` view from CAN Source state.
 * 中文：从 CAN Source 状态返回当前 `stats` 视图。
 */
const ucn_can_source_stats_t *ucn_can_source_get_stats(
    const ucn_can_source_t *source)
{
    return source == NULL || !source->initialized ? NULL : &source->stats;
}

/*
 * EN: Encodes `fd_carrier_encode` into its bounded CAN Source wire representation.
 * 中文：把 `fd_carrier_encode` 编码为有界的 CAN Source 线格式。
 */
ucn_result_t ucn_can_fd_carrier_encode(
    const uint8_t *frame,
    size_t frame_length,
    uint8_t output[UCN_CAN_FD_MAX_DATA_BYTES],
    size_t *output_length)
{
    size_t encoded_length = 0U;
    size_t rounded_length;
    ucn_result_t result;

    if (frame == NULL || output == NULL || output_length == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *output_length = 0U;
    if (frame_length < UCN_FRAME_W0_HEADER_SIZE) {
        return UCN_ERR_MALFORMED;
    }
    if (frame_length > UCN_CAN_FD_MAX_DATA_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    result = ucn_frame_peek_encoded_size(frame, frame_length,
                                         &encoded_length);
    if (result != UCN_OK || encoded_length != frame_length) {
        return result == UCN_OK ? UCN_ERR_MALFORMED : result;
    }
    rounded_length = can_fd_rounded_length(frame_length);
    if (rounded_length == 0U) {
        return UCN_ERR_TOO_LARGE;
    }
    (void)memcpy(output, frame, frame_length);
    (void)memset(&output[frame_length], 0,
                 rounded_length - frame_length);
    *output_length = rounded_length;
    return UCN_OK;
}

/*
 * EN: Calculates the bounded `classic_carrier_segment_count` value used by CAN Source.
 * 中文：计算 CAN Source 使用的有界 `classic_carrier_segment_count` 值。
 */
size_t ucn_can_classic_carrier_segment_count(size_t frame_length)
{
    size_t continuation_bytes;
    size_t segments;

    if (frame_length < UCN_FRAME_W0_HEADER_SIZE ||
        frame_length > UCN_CAN_CLASSIC_CARRIER_MAX_FRAME_BYTES) {
        return 0U;
    }
    continuation_bytes = frame_length - 3U;
    segments = 1U + (continuation_bytes + 4U) / 5U;
    return segments <= 256U ? segments : 0U;
}

/*
 * EN: Writes `classic_carrier_encode_segment` using the canonical bounded CAN Source representation.
 * 中文：使用规范且有界的 CAN Source 表示写入 `classic_carrier_encode_segment`。
 */
ucn_result_t ucn_can_classic_carrier_encode_segment(
    const uint8_t *frame,
    size_t frame_length,
    uint8_t transfer_id,
    uint16_t segment_index,
    uint8_t output[UCN_CAN_CLASSIC_MAX_DATA_BYTES],
    size_t *output_length)
{
    size_t encoded_length = 0U;
    size_t segment_count;
    size_t offset;
    size_t chunk_length;
    ucn_result_t result;

    if (frame == NULL || output == NULL || output_length == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *output_length = 0U;
    segment_count = ucn_can_classic_carrier_segment_count(frame_length);
    if (segment_count == 0U) {
        return frame_length > UCN_CAN_CLASSIC_CARRIER_MAX_FRAME_BYTES ?
                   UCN_ERR_TOO_LARGE : UCN_ERR_MALFORMED;
    }
    result = ucn_frame_peek_encoded_size(frame, frame_length,
                                         &encoded_length);
    if (result != UCN_OK || encoded_length != frame_length) {
        return result == UCN_OK ? UCN_ERR_MALFORMED : result;
    }
    if ((size_t)segment_index >= segment_count) {
        return UCN_ERR_NOT_FOUND;
    }
    (void)memset(output, 0, UCN_CAN_CLASSIC_MAX_DATA_BYTES);
    if (segment_index == 0U) {
        output[0] = UCN_CAN_CLASSIC_CARRIER_START;
        output[1] = transfer_id;
        output[2] = 0U;
        output[3] = (uint8_t)(frame_length >> 8U);
        output[4] = (uint8_t)frame_length;
        (void)memcpy(&output[5], frame, 3U);
        *output_length = UCN_CAN_CLASSIC_MAX_DATA_BYTES;
        return UCN_OK;
    }
    offset = 3U + ((size_t)segment_index - 1U) * 5U;
    chunk_length = frame_length - offset;
    if (chunk_length > 5U) {
        chunk_length = 5U;
    }
    output[0] = UCN_CAN_CLASSIC_CARRIER_CONTINUE;
    output[1] = transfer_id;
    output[2] = (uint8_t)segment_index;
    (void)memcpy(&output[3], &frame[offset], chunk_length);
    *output_length = 3U + chunk_length;
    return UCN_OK;
}
