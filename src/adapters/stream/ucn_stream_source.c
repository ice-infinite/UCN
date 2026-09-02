#include <string.h>

#include "ucn/adapters/ucn_stream_source.h"

/*
 * EN: Enters or leaves the bounded `enter_task` critical section for Stream Source.
 * 中文：进入或退出 Stream Source 的有界 `enter_task` 临界区。
 */
static void stream_enter_task(ucn_stream_source_t *source)
{
    if (source->port_ops->enter_critical != NULL) {
        source->port_ops->enter_critical(source->port_context);
    }
}

/*
 * EN: Enters or leaves the bounded `exit_task` critical section for Stream Source.
 * 中文：进入或退出 Stream Source 的有界 `exit_task` 临界区。
 */
static void stream_exit_task(ucn_stream_source_t *source)
{
    if (source->port_ops->exit_critical != NULL) {
        source->port_ops->exit_critical(source->port_context);
    }
}

/*
 * EN: Checks the `isr_lock_is_configured` condition against current Stream Source state.
 * 中文：根据当前 Stream Source 状态检查 `isr_lock_is_configured` 条件。
 */
static bool stream_isr_lock_is_configured(const ucn_stream_source_t *source)
{
    return source->port_ops->enter_critical_from_isr != NULL &&
           source->port_ops->exit_critical_from_isr != NULL;
}

/*
 * EN: Enters or leaves the bounded `enter_isr` critical section for Stream Source.
 * 中文：进入或退出 Stream Source 的有界 `enter_isr` 临界区。
 */
static ucn_port_critical_token_t stream_enter_isr(
    ucn_stream_source_t *source)
{
    return source->port_ops->enter_critical_from_isr(source->port_context);
}

/*
 * EN: Enters or leaves the bounded `exit_isr` critical section for Stream Source.
 * 中文：进入或退出 Stream Source 的有界 `exit_isr` 临界区。
 */
static void stream_exit_isr(ucn_stream_source_t *source,
                            ucn_port_critical_token_t token)
{
    source->port_ops->exit_critical_from_isr(source->port_context, token);
}

/*
 * EN: Reads and validates `cobs_decode_in_place` from the canonical Stream Source representation.
 * 中文：从规范的 Stream Source 表示中读取并验证 `cobs_decode_in_place`。
 */
static ucn_result_t cobs_decode_in_place(uint8_t *buffer,
                                         size_t encoded_length,
                                         size_t decoded_capacity,
                                         size_t *decoded_length)
{
    size_t read_index = 0U;
    size_t write_index = 0U;

    if (buffer == NULL || decoded_length == NULL || encoded_length == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    while (read_index < encoded_length) {
        const uint8_t code = buffer[read_index++];
        uint8_t index;

        if (code == 0U ||
            (size_t)(code - 1U) > encoded_length - read_index) {
            return UCN_ERR_MALFORMED;
        }
        for (index = 1U; index < code; ++index) {
            if (write_index >= decoded_capacity) {
                return UCN_ERR_TOO_LARGE;
            }
            buffer[write_index++] = buffer[read_index++];
        }
        if (code != UINT8_C(0xFF) && read_index < encoded_length) {
            if (write_index >= decoded_capacity) {
                return UCN_ERR_TOO_LARGE;
            }
            buffer[write_index++] = 0U;
        }
    }
    *decoded_length = write_index;
    return UCN_OK;
}

/*
 * EN: Encodes `carrier_encode` into its bounded Stream Source wire representation.
 * 中文：把 `carrier_encode` 编码为有界的 Stream Source 线格式。
 */
ucn_result_t ucn_stream_carrier_encode(const uint8_t *frame,
                                       size_t frame_length,
                                       uint8_t *wire_output,
                                       size_t wire_capacity,
                                       size_t *wire_length)
{
    size_t read_index = 0U;
    size_t write_index = 1U;
    size_t code_index = 0U;
    uint8_t code = 1U;

    if (frame == NULL || wire_output == NULL || wire_length == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    *wire_length = 0U;
    if (frame_length < UCN_FRAME_W0_HEADER_SIZE) {
        return UCN_ERR_MALFORMED;
    }
    if (frame_length > UCN_MAX_FRAME_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    if (wire_capacity < 2U) {
        return UCN_ERR_TOO_LARGE;
    }

    while (read_index < frame_length) {
        if (frame[read_index] == 0U) {
            if (code_index >= wire_capacity ||
                write_index >= wire_capacity - 1U) {
                return UCN_ERR_TOO_LARGE;
            }
            wire_output[code_index] = code;
            code_index = write_index++;
            code = 1U;
            ++read_index;
            continue;
        }
        if (write_index >= wire_capacity - 1U) {
            return UCN_ERR_TOO_LARGE;
        }
        wire_output[write_index++] = frame[read_index++];
        ++code;
        if (code == UINT8_C(0xFF)) {
            if (code_index >= wire_capacity ||
                write_index >= wire_capacity - 1U) {
                return UCN_ERR_TOO_LARGE;
            }
            wire_output[code_index] = code;
            code_index = write_index++;
            code = 1U;
        }
    }
    if (code_index >= wire_capacity || write_index >= wire_capacity) {
        return UCN_ERR_TOO_LARGE;
    }
    wire_output[code_index] = code;
    wire_output[write_index++] = 0U;
    *wire_length = write_index;
    return UCN_OK;
}

/*
 * EN: Calculates the bounded `ring_count` value used by Stream Source.
 * 中文：计算 Stream Source 使用的有界 `ring_count` 值。
 */
static size_t stream_ring_count(ucn_stream_source_t *source)
{
    size_t count;

    stream_enter_task(source);
    count = source->ring_count;
    stream_exit_task(source);
    return count;
}

/*
 * EN: Checks the `has_pending_input` condition in current Stream Source state.
 * 中文：检查当前 Stream Source 状态中的 `has_pending_input` 条件。
 */
static bool stream_has_pending_input(ucn_stream_source_t *source)
{
    bool pending;

    stream_enter_task(source);
    pending = source->ring_count != 0U || source->ring_gap_detected;
    stream_exit_task(source);
    return pending;
}

/* Take at most one delimiter-terminated segment.  Stopping at the first zero
 * means Source may retain a decoded frame on Adapter Queue backpressure
 * without having already consumed bytes from the following carrier. */
/*
 * EN: Removes and returns `ring_take_segment` from bounded Stream Source storage.
 * 中文：从固定容量的 Stream Source 存储中移除并返回 `ring_take_segment`。
 */
static size_t stream_ring_take_segment(ucn_stream_source_t *source,
                                       uint8_t *output,
                                       size_t capacity,
                                       bool *gap_detected)
{
    size_t count;
    size_t index;

    stream_enter_task(source);
    *gap_detected = source->ring_gap_detected &&
                    source->bytes_before_gap == 0U;
    if (*gap_detected) {
        source->ring_gap_detected = false;
    }
    count = source->ring_count < capacity ? source->ring_count : capacity;
    if (source->ring_gap_detected && count > source->bytes_before_gap) {
        count = source->bytes_before_gap;
    }
    for (index = 0U; index < count; ++index) {
        output[index] = source->ring_storage[source->ring_tail];
        source->ring_tail = (source->ring_tail + 1U) % source->ring_capacity;
        source->ring_count--;
        if (source->ring_gap_detected) {
            source->bytes_before_gap--;
        }
        if (output[index] == 0U) {
            ++index;
            break;
        }
    }
    stream_exit_task(source);
    return index;
}

/*
 * EN: Checks the current `submit_pending` condition in Stream Source state.
 * 中文：检查当前 Stream Source 状态中的 `submit_pending` 条件。
 */
static ucn_result_t stream_submit_pending(ucn_stream_source_t *source)
{
    ucn_result_t result;

    result = ucn_event_runtime_submit_frame(
        source->runtime, source->ingress_link, source->frame_storage,
        source->pending_frame_length);
    if (result == UCN_OK) {
        source->stats.frames_submitted++;
        source->pending_frame = false;
        source->pending_frame_length = 0U;
    } else if (result == UCN_ERR_NO_SPACE) {
        source->stats.adapter_queue_backpressure++;
    } else {
        source->stats.frames_rejected++;
        source->pending_frame = false;
        source->pending_frame_length = 0U;
    }
    return result;
}

/*
 * EN: Processes one bounded `source_service` work unit for Stream Source.
 * 中文：为 Stream Source 处理一个有界的 `source_service` 工作单元。
 */
static ucn_result_t stream_source_service(
    void *context,
    ucn_event_source_events_t events,
    size_t max_work,
    ucn_event_source_service_result_t *result)
{
    ucn_stream_source_t *source = (ucn_stream_source_t *)context;
    size_t bytes_remaining;
    size_t errors = 0U;
    ucn_result_t first_error = UCN_OK;
    bool stop = false;

    if (source == NULL || !source->initialized || result == NULL ||
        max_work == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(result, 0, sizeof(*result));
    source->stats.service_calls++;
    if ((events & UCN_EVENT_SOURCE_FALLBACK_SCAN) != 0U) {
        source->stats.fallback_services++;
    }
    bytes_remaining = source->max_bytes_per_service;

    while (!stop && result->work_done < max_work) {
        uint8_t chunk[UCN_STREAM_SOURCE_READ_CHUNK_BYTES];
        size_t chunk_capacity;
        size_t chunk_length;
        size_t index;
        bool gap_detected = false;

        if (source->pending_frame) {
            ucn_result_t submit_result = stream_submit_pending(source);

            result->work_done++;
            if (submit_result == UCN_ERR_NO_SPACE) {
                break;
            }
            if (submit_result != UCN_OK && first_error == UCN_OK) {
                first_error = submit_result;
            }
            continue;
        }
        if (bytes_remaining == 0U) {
            break;
        }
        chunk_capacity = bytes_remaining < sizeof(chunk) ? bytes_remaining :
                                                               sizeof(chunk);
        chunk_length = stream_ring_take_segment(source, chunk, chunk_capacity,
                                                 &gap_detected);
        if (gap_detected) {
            source->candidate_length = 0U;
            source->discard_until_delimiter = true;
            source->stats.ring_gaps++;
            errors++;
        }
        if (chunk_length == 0U) {
            break;
        }
        bytes_remaining -= chunk_length;
        source->stats.bytes_consumed += (uint32_t)chunk_length;

        for (index = 0U; index < chunk_length; ++index) {
            const uint8_t value = chunk[index];

            if (value != 0U) {
                if (source->discard_until_delimiter) {
                    continue;
                }
                if (source->candidate_length >= source->frame_capacity) {
                    source->candidate_length = 0U;
                    source->discard_until_delimiter = true;
                    source->stats.candidate_overflows++;
                    errors++;
                    continue;
                }
                source->frame_storage[source->candidate_length++] = value;
                continue;
            }

            result->work_done++;
            source->stats.carrier_candidates++;
            if (source->discard_until_delimiter) {
                source->discard_until_delimiter = false;
                source->candidate_length = 0U;
                source->stats.resynchronizations++;
            } else if (source->candidate_length == 0U) {
                source->stats.empty_delimiters++;
            } else {
                size_t decoded_length = 0U;
                ucn_result_t decode_result = cobs_decode_in_place(
                    source->frame_storage, source->candidate_length,
                    source->max_frame_bytes, &decoded_length);

                source->candidate_length = 0U;
                if (decode_result != UCN_OK) {
                    source->stats.cobs_decode_errors++;
                    errors++;
                } else if (decoded_length < UCN_FRAME_W0_HEADER_SIZE ||
                           decoded_length > source->max_frame_bytes) {
                    source->stats.frame_length_errors++;
                    errors++;
                } else {
                    ucn_result_t submit_result;

                    source->pending_frame = true;
                    source->pending_frame_length = decoded_length;
                    source->stats.frames_decoded++;
                    submit_result = stream_submit_pending(source);
                    if (submit_result == UCN_ERR_NO_SPACE) {
                        stop = true;
                    } else if (submit_result != UCN_OK &&
                               first_error == UCN_OK) {
                        first_error = submit_result;
                    }
                }
            }
        }
        if (errors >= source->max_errors_per_service) {
            source->stats.error_budget_hits++;
            stop = true;
        }
    }

    if (source->pending_frame || stream_has_pending_input(source)) {
        result->pending_events = UCN_EVENT_SOURCE_RX_READY;
    }
    return first_error;
}

static const ucn_event_source_ops_t STREAM_EVENT_SOURCE_OPS = {
    stream_source_service
};

/*
 * EN: Initializes the Stream Source object from validated caller-owned configuration without heap allocation.
 * 中文：使用经验证的调用方配置初始化 Stream Source 对象，且不使用堆内存。
 */
ucn_result_t ucn_stream_source_init(
    ucn_stream_source_t *source,
    const ucn_stream_source_config_t *config)
{
    ucn_event_source_config_t event_config;
    ucn_result_t bind_result;
    size_t max_frame_bytes;
    const ucn_port_ops_t *port_ops;

    if (source == NULL || config == NULL || config->runtime == NULL ||
        !config->runtime->initialized || config->ingress_link == NULL ||
        config->ring_storage == NULL || config->ring_capacity == 0U ||
        config->frame_storage == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    max_frame_bytes = config->max_frame_bytes == 0U ? UCN_MAX_FRAME_BYTES :
                                                       config->max_frame_bytes;
    if (max_frame_bytes < UCN_FRAME_W0_HEADER_SIZE ||
        max_frame_bytes > UCN_MAX_FRAME_BYTES ||
        config->frame_capacity <
            UCN_STREAM_COBS_MAX_ENCODED_BYTES(max_frame_bytes)) {
        return UCN_ERR_CONFIG;
    }
    port_ops = config->runtime->owner.config.port_ops;
    if (!ucn_port_ops_is_compatible(port_ops) ||
        (port_ops->enter_critical == NULL) !=
            (port_ops->exit_critical == NULL) ||
        (port_ops->enter_critical_from_isr == NULL) !=
            (port_ops->exit_critical_from_isr == NULL)) {
        return UCN_ERR_CONFIG;
    }

    (void)memset(source, 0, sizeof(*source));
    source->runtime = config->runtime;
    source->source_id = config->source_id;
    source->ingress_link = config->ingress_link;
    source->port_ops = port_ops;
    source->port_context = config->runtime->owner.config.port_context;
    source->ring_storage = config->ring_storage;
    source->ring_capacity = config->ring_capacity;
    source->frame_storage = config->frame_storage;
    source->frame_capacity = config->frame_capacity;
    source->max_frame_bytes = max_frame_bytes;
    source->max_bytes_per_service =
        config->max_bytes_per_service == 0U ?
            UCN_STREAM_SOURCE_DEFAULT_BYTE_BUDGET :
            config->max_bytes_per_service;
    source->max_errors_per_service =
        config->max_errors_per_service == 0U ?
            UCN_STREAM_SOURCE_DEFAULT_ERROR_BUDGET :
            config->max_errors_per_service;
    if (source->max_bytes_per_service == 0U ||
        source->max_errors_per_service == 0U) {
        return UCN_ERR_CONFIG;
    }

    event_config.ops = &STREAM_EVENT_SOURCE_OPS;
    event_config.context = source;
    bind_result = ucn_event_runtime_bind_source(
        config->runtime, config->source_id, &event_config);
    if (bind_result != UCN_OK) {
        return bind_result;
    }
    source->initialized = true;
    return UCN_OK;
}

/*
 * EN: Writes `source_write` using the canonical bounded Stream Source representation.
 * 中文：使用规范且有界的 Stream Source 表示写入 `source_write`。
 */
static ucn_result_t stream_source_write(ucn_stream_source_t *source,
                                        const uint8_t *data,
                                        size_t length,
                                        bool from_isr)
{
    ucn_result_t write_result = UCN_OK;
    ucn_result_t signal_result;
    ucn_port_critical_token_t token = 0U;
    size_t first_length;

    if (source == NULL || !source->initialized || data == NULL || length == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (from_isr && !stream_isr_lock_is_configured(source)) {
        return UCN_ERR_CONFIG;
    }

    if (from_isr) {
        token = stream_enter_isr(source);
    } else {
        stream_enter_task(source);
    }
    if (from_isr) {
        source->stats.isr_writes++;
    } else {
        source->stats.task_writes++;
    }
    if (length > source->ring_capacity - source->ring_count) {
        source->stats.ring_no_space++;
        source->stats.bytes_rejected += (uint32_t)length;
        if (!source->ring_gap_detected) {
            source->ring_gap_detected = true;
            source->bytes_before_gap = source->ring_count;
        }
        write_result = UCN_ERR_NO_SPACE;
    } else {
        first_length = source->ring_capacity - source->ring_head;
        if (first_length > length) {
            first_length = length;
        }
        (void)memcpy(&source->ring_storage[source->ring_head], data,
                     first_length);
        if (length > first_length) {
            (void)memcpy(source->ring_storage, &data[first_length],
                         length - first_length);
        }
        source->ring_head = (source->ring_head + length) %
                            source->ring_capacity;
        source->ring_count += length;
        source->stats.bytes_accepted += (uint32_t)length;
        if (source->ring_count > source->stats.ring_high_water) {
            source->stats.ring_high_water = source->ring_count;
        }
    }
    if (from_isr) {
        stream_exit_isr(source, token);
    } else {
        stream_exit_task(source);
    }

    signal_result = from_isr ?
        ucn_event_runtime_signal_source_from_isr(
            source->runtime, source->source_id,
            UCN_EVENT_SOURCE_RX_READY) :
        ucn_event_runtime_signal_source(source->runtime, source->source_id,
                                        UCN_EVENT_SOURCE_RX_READY);
    if (signal_result != UCN_OK) {
        if (from_isr) {
            token = stream_enter_isr(source);
            source->stats.notification_errors++;
            stream_exit_isr(source, token);
        } else {
            stream_enter_task(source);
            source->stats.notification_errors++;
            stream_exit_task(source);
        }
    }
    return write_result != UCN_OK ? write_result : signal_result;
}

/*
 * EN: Writes `write` using the canonical bounded Stream Source representation.
 * 中文：使用规范且有界的 Stream Source 表示写入 `write`。
 */
ucn_result_t ucn_stream_source_write(ucn_stream_source_t *source,
                                     const uint8_t *data,
                                     size_t length)
{
    return stream_source_write(source, data, length, false);
}

/*
 * EN: Writes `from_isr` in the canonical Stream Source byte order.
 * 中文：按规范的 Stream Source 字节序写入 `from_isr`。
 */
ucn_result_t ucn_stream_source_write_from_isr(ucn_stream_source_t *source,
                                              const uint8_t *data,
                                              size_t length)
{
    return stream_source_write(source, data, length, true);
}

/*
 * EN: Clears or releases `reset` from bounded Stream Source state.
 * 中文：从固定容量的 Stream Source 状态中清除或释放 `reset`。
 */
ucn_result_t ucn_stream_source_reset(ucn_stream_source_t *source)
{
    if (source == NULL || !source->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    stream_enter_task(source);
    source->ring_head = 0U;
    source->ring_tail = 0U;
    source->ring_count = 0U;
    source->ring_gap_detected = false;
    source->bytes_before_gap = 0U;
    source->candidate_length = 0U;
    source->pending_frame_length = 0U;
    source->discard_until_delimiter = false;
    source->pending_frame = false;
    stream_exit_task(source);
    return UCN_OK;
}

/*
 * EN: Checks the `pending_bytes` condition against current Stream Source state.
 * 中文：根据当前 Stream Source 状态检查 `pending_bytes` 条件。
 */
size_t ucn_stream_source_pending_bytes(ucn_stream_source_t *source)
{
    return source == NULL || !source->initialized ? 0U :
                                                    stream_ring_count(source);
}

/*
 * EN: Returns the current free-byte capacity of the Stream Source ring.
 * 中文：返回 Stream Source 环形缓冲当前可用的字节容量。
 */
size_t ucn_stream_source_free_bytes(ucn_stream_source_t *source)
{
    size_t pending;

    if (source == NULL || !source->initialized) {
        return 0U;
    }
    pending = stream_ring_count(source);
    return source->ring_capacity - pending;
}

/*
 * EN: Returns the current `stats` view from Stream Source state.
 * 中文：从 Stream Source 状态返回当前 `stats` 视图。
 */
const ucn_stream_source_stats_t *ucn_stream_source_get_stats(
    const ucn_stream_source_t *source)
{
    return source == NULL || !source->initialized ? NULL : &source->stats;
}
