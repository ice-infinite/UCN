#ifndef UCN_ADAPTERS_STREAM_SOURCE_H
#define UCN_ADAPTERS_STREAM_SOURCE_H

#include "ucn/ports/ucn_event_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Conservative COBS bounds.  The encoded value excludes the trailing zero;
 * the wire value includes it.  These macros are integer constant expressions
 * and may therefore size caller-owned static arrays. */
#define UCN_STREAM_COBS_MAX_ENCODED_BYTES(frame_bytes) \
    ((frame_bytes) + ((frame_bytes) / 254U) + 2U)
#define UCN_STREAM_CARRIER_MAX_WIRE_BYTES(frame_bytes) \
    (UCN_STREAM_COBS_MAX_ENCODED_BYTES(frame_bytes) + 1U)

#ifndef UCN_STREAM_SOURCE_DEFAULT_RING_BYTES
#define UCN_STREAM_SOURCE_DEFAULT_RING_BYTES 512U
#endif

#ifndef UCN_STREAM_SOURCE_DEFAULT_BYTE_BUDGET
#define UCN_STREAM_SOURCE_DEFAULT_BYTE_BUDGET 512U
#endif

#ifndef UCN_STREAM_SOURCE_DEFAULT_ERROR_BUDGET
#define UCN_STREAM_SOURCE_DEFAULT_ERROR_BUDGET 4U
#endif

#ifndef UCN_STREAM_SOURCE_READ_CHUNK_BYTES
#define UCN_STREAM_SOURCE_READ_CHUNK_BYTES 32U
#endif

typedef char ucn_stream_default_ring_must_be_positive[
    UCN_STREAM_SOURCE_DEFAULT_RING_BYTES >= 1U ? 1 : -1];
typedef char ucn_stream_default_byte_budget_must_be_positive[
    UCN_STREAM_SOURCE_DEFAULT_BYTE_BUDGET >= 1U ? 1 : -1];
typedef char ucn_stream_default_error_budget_must_be_positive[
    UCN_STREAM_SOURCE_DEFAULT_ERROR_BUDGET >= 1U ? 1 : -1];
typedef char ucn_stream_read_chunk_must_be_1_to_256[
    UCN_STREAM_SOURCE_READ_CHUNK_BYTES >= 1U &&
            UCN_STREAM_SOURCE_READ_CHUNK_BYTES <= 256U ? 1 : -1];

/* Convenience storage only.  Products may instead supply custom static
 * arrays through ucn_stream_source_config_t. */
typedef struct ucn_stream_source_default_storage {
    uint8_t ring[UCN_STREAM_SOURCE_DEFAULT_RING_BYTES];
    uint8_t frame[UCN_STREAM_COBS_MAX_ENCODED_BYTES(UCN_MAX_FRAME_BYTES)];
} ucn_stream_source_default_storage_t;

typedef struct ucn_stream_source_stats {
    uint32_t task_writes;
    uint32_t isr_writes;
    uint32_t bytes_accepted;
    uint32_t bytes_rejected;
    uint32_t ring_no_space;
    size_t ring_high_water;
    uint32_t notification_errors;
    uint32_t service_calls;
    uint32_t fallback_services;
    uint32_t bytes_consumed;
    uint32_t carrier_candidates;
    uint32_t empty_delimiters;
    uint32_t ring_gaps;
    uint32_t resynchronizations;
    uint32_t cobs_decode_errors;
    uint32_t frame_length_errors;
    uint32_t candidate_overflows;
    uint32_t error_budget_hits;
    uint32_t frames_decoded;
    uint32_t frames_submitted;
    uint32_t frames_rejected;
    uint32_t adapter_queue_backpressure;
} ucn_stream_source_stats_t;

typedef struct ucn_stream_source_config {
    ucn_event_runtime_t *runtime;
    ucn_event_source_id_t source_id;
    ucn_link_t *ingress_link;
    uint8_t *ring_storage;
    size_t ring_capacity;
    uint8_t *frame_storage;
    size_t frame_capacity;
    /* Zero selects UCN_MAX_FRAME_BYTES/default budgets. */
    size_t max_frame_bytes;
    size_t max_bytes_per_service;
    size_t max_errors_per_service;
} ucn_stream_source_config_t;

typedef struct ucn_stream_source {
    ucn_event_runtime_t *runtime;
    ucn_link_t *ingress_link;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    uint8_t *ring_storage;
    size_t ring_capacity;
    size_t ring_head;
    size_t ring_tail;
    size_t ring_count;
    /* Number of already accepted bytes that precede the first rejected
     * chunk.  This preserves complete carriers queued before an overflow;
     * only bytes after the actual stream gap are discarded. */
    size_t bytes_before_gap;
    uint8_t *frame_storage;
    size_t frame_capacity;
    size_t max_frame_bytes;
    size_t candidate_length;
    size_t pending_frame_length;
    size_t max_bytes_per_service;
    size_t max_errors_per_service;
    ucn_stream_source_stats_t stats;
    ucn_event_source_id_t source_id;
    bool ring_gap_detected;
    bool discard_until_delimiter;
    bool pending_frame;
    bool initialized;
} ucn_stream_source_t;

/* Initialize and bind this Stream instance to one already-initialized Event
 * Runtime Source slot.  All storage remains caller-owned for the lifetime of
 * the Runtime. */
ucn_result_t ucn_stream_source_init(
    ucn_stream_source_t *source,
    const ucn_stream_source_config_t *config);

/* Whole-chunk writes: either all bytes enter the fixed Ring or none do.  A
 * no-space result marks a stream gap, so RX discards through the next zero
 * delimiter before decoding again.  Accepted data remains stored even if the
 * scheduler notification itself later reports an error. */
ucn_result_t ucn_stream_source_write(
    ucn_stream_source_t *source,
    const uint8_t *data,
    size_t length);
ucn_result_t ucn_stream_source_write_from_isr(
    ucn_stream_source_t *source,
    const uint8_t *data,
    size_t length);

/* Task/Owner-context recovery helper.  It clears Ring and partial Carrier
 * state but preserves configuration, Source binding, and statistics. */
ucn_result_t ucn_stream_source_reset(ucn_stream_source_t *source);

size_t ucn_stream_source_pending_bytes(ucn_stream_source_t *source);
size_t ucn_stream_source_free_bytes(ucn_stream_source_t *source);
const ucn_stream_source_stats_t *ucn_stream_source_get_stats(
    const ucn_stream_source_t *source);

/* Encode exactly one complete UCN Frame as COBS plus a trailing zero.  The
 * product Link must enqueue the returned carrier atomically into its bounded
 * UART/RS-485/USB TX path. */
ucn_result_t ucn_stream_carrier_encode(
    const uint8_t *frame,
    size_t frame_length,
    uint8_t *wire_output,
    size_t wire_capacity,
    size_t *wire_length);

#ifdef __cplusplus
}
#endif

#endif
