#ifndef UCN_ADAPTERS_CAN_SOURCE_H
#define UCN_ADAPTERS_CAN_SOURCE_H

#include "ucn/ports/ucn_event_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_CAN_FD_MAX_DATA_BYTES 64U
#define UCN_CAN_CLASSIC_MAX_DATA_BYTES 8U
#define UCN_CAN_CLASSIC_CARRIER_START UINT8_C(0xC1)
#define UCN_CAN_CLASSIC_CARRIER_CONTINUE UINT8_C(0xC2)
#define UCN_CAN_CLASSIC_CARRIER_MAX_FRAME_BYTES 1278U

#ifndef UCN_CAN_SOURCE_DEFAULT_RING_FRAMES
#define UCN_CAN_SOURCE_DEFAULT_RING_FRAMES 8U
#endif

#ifndef UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS
#define UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS 2U
#endif

#ifndef UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_TIMEOUT_MS
#define UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_TIMEOUT_MS 250U
#endif

typedef char ucn_can_default_ring_must_be_positive[
    UCN_CAN_SOURCE_DEFAULT_RING_FRAMES >= 1U ? 1 : -1];
typedef char ucn_can_default_slots_must_be_positive[
    UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS >= 1U ? 1 : -1];

typedef uint8_t ucn_can_frame_flags_t;
#define UCN_CAN_FRAME_FLAG_EXTENDED ((ucn_can_frame_flags_t)UINT8_C(0x01))
#define UCN_CAN_FRAME_FLAG_FD ((ucn_can_frame_flags_t)UINT8_C(0x02))
#define UCN_CAN_FRAME_FLAG_BRS ((ucn_can_frame_flags_t)UINT8_C(0x04))
#define UCN_CAN_FRAME_FLAG_RTR ((ucn_can_frame_flags_t)UINT8_C(0x08))
#define UCN_CAN_FRAME_FLAG_ERROR ((ucn_can_frame_flags_t)UINT8_C(0x10))
#define UCN_CAN_FRAME_KNOWN_FLAGS ((ucn_can_frame_flags_t)UINT8_C(0x1F))

/* Normalized complete controller RX item.  length is the DLC-decoded byte
 * count, not the raw four-bit DLC code.  No SDK-specific type enters UCN. */
typedef struct ucn_can_frame {
    uint32_t identifier;
    uint8_t length;
    ucn_can_frame_flags_t flags;
    uint8_t data[UCN_CAN_FD_MAX_DATA_BYTES];
} ucn_can_frame_t;

typedef enum ucn_can_source_mode {
    UCN_CAN_SOURCE_CAN_FD_DIRECT = 0,
    UCN_CAN_SOURCE_CLASSIC_CARRIER = 1,
    UCN_CAN_SOURCE_MIXED = 2
} ucn_can_source_mode_t;

typedef enum ucn_can_bus_state {
    UCN_CAN_BUS_ACTIVE = 0,
    UCN_CAN_BUS_ERROR_PASSIVE = 1,
    UCN_CAN_BUS_OFF = 2,
    UCN_CAN_BUS_RECOVERING = 3
} ucn_can_bus_state_t;

typedef ucn_result_t (*ucn_can_resolve_ingress_fn)(
    void *context,
    uint32_t identifier,
    bool extended,
    ucn_link_t **ingress_link);

/* Descriptor storage and frame bytes are both caller-owned.  The Source
 * assigns frame_storage from the configured flat storage region at init. */
typedef struct ucn_can_reassembly_slot {
    ucn_link_t *ingress_link;
    uint8_t *frame_storage;
    uint32_t identifier;
    uint32_t deadline_ms;
    size_t expected_length;
    size_t received_length;
    uint8_t transfer_id;
    uint8_t next_segment_index;
    bool extended;
    bool active;
    bool complete;
} ucn_can_reassembly_slot_t;

typedef struct ucn_can_source_default_storage {
    ucn_can_frame_t ring[UCN_CAN_SOURCE_DEFAULT_RING_FRAMES];
    ucn_can_reassembly_slot_t
        slots[UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS];
    uint8_t reassembly[UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS]
                      [UCN_MAX_FRAME_BYTES];
} ucn_can_source_default_storage_t;

typedef struct ucn_can_source_stats {
    uint32_t task_writes;
    uint32_t isr_writes;
    uint32_t physical_frames_accepted;
    uint32_t physical_frames_rejected;
    uint32_t ring_no_space;
    size_t ring_high_water;
    uint32_t notification_errors;
    uint32_t service_calls;
    uint32_t fallback_services;
    uint32_t filtered_frames;
    uint32_t resolver_errors;
    uint32_t physical_format_errors;
    uint32_t fd_frames;
    uint32_t fd_padding_errors;
    uint32_t classic_start_frames;
    uint32_t classic_continue_frames;
    uint32_t carrier_restarts;
    uint32_t carrier_order_errors;
    uint32_t carrier_no_slot;
    uint32_t carrier_timeouts;
    uint32_t carrier_completed;
    uint32_t frame_length_errors;
    uint32_t frames_submitted;
    uint32_t frames_rejected;
    uint32_t adapter_queue_backpressure;
    uint32_t status_changes;
    uint32_t error_passive_events;
    uint32_t bus_off_events;
    uint32_t recoveries;
    uint32_t frames_flushed_on_down;
} ucn_can_source_stats_t;

typedef struct ucn_can_source_health {
    ucn_can_bus_state_t bus_state;
    size_t pending_physical_frames;
    size_t active_reassembly_slots;
    uint16_t queue_pressure_per_mille;
    uint32_t receive_failure_count;
} ucn_can_source_health_t;

typedef struct ucn_can_source_config {
    ucn_event_runtime_t *runtime;
    ucn_event_source_id_t source_id;
    ucn_can_source_mode_t mode;
    ucn_can_frame_t *ring_storage;
    size_t ring_capacity;
    ucn_can_reassembly_slot_t *reassembly_slots;
    size_t reassembly_slot_count;
    uint8_t *reassembly_storage;
    size_t reassembly_storage_capacity;
    /* Zero selects UCN_MAX_FRAME_BYTES/default timeout. */
    size_t max_frame_bytes;
    uint32_t reassembly_timeout_ms;
    ucn_can_resolve_ingress_fn resolve_ingress;
    void *resolve_context;
} ucn_can_source_config_t;

typedef struct ucn_can_source {
    ucn_event_runtime_t *runtime;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    ucn_can_frame_t *ring_storage;
    size_t ring_capacity;
    size_t ring_head;
    size_t ring_tail;
    size_t ring_count;
    ucn_can_reassembly_slot_t *reassembly_slots;
    size_t reassembly_slot_count;
    size_t max_frame_bytes;
    uint32_t reassembly_timeout_ms;
    ucn_can_resolve_ingress_fn resolve_ingress;
    void *resolve_context;
    ucn_can_source_stats_t stats;
    ucn_event_source_id_t source_id;
    ucn_can_source_mode_t mode;
    ucn_can_bus_state_t bus_state;
    bool clear_reassembly_pending;
    bool initialized;
} ucn_can_source_t;

ucn_result_t ucn_can_source_init(
    ucn_can_source_t *source,
    const ucn_can_source_config_t *config);

/* Whole-frame admission.  The controller ISR should normalize its SDK frame,
 * copy it here, then return; UCN parsing remains in the Owner task. */
ucn_result_t ucn_can_source_write(
    ucn_can_source_t *source,
    const ucn_can_frame_t *frame);
ucn_result_t ucn_can_source_write_from_isr(
    ucn_can_source_t *source,
    const ucn_can_frame_t *frame);

/* Hardware recovery remains a BSP responsibility.  BUS_OFF/RECOVERING flush
 * queued physical frames and cause subsequent writes to fail LINK_DOWN until
 * the product explicitly reports ACTIVE. */
ucn_result_t ucn_can_source_set_bus_state(
    ucn_can_source_t *source,
    ucn_can_bus_state_t state);
ucn_result_t ucn_can_source_set_bus_state_from_isr(
    ucn_can_source_t *source,
    ucn_can_bus_state_t state);

/* Reset and health snapshot are Owner-context operations unless the product
 * externally serializes them against Source service().  get_stats returns a
 * live counter view and has the same observation rule. */
ucn_result_t ucn_can_source_reset(ucn_can_source_t *source);
ucn_result_t ucn_can_source_get_health(
    ucn_can_source_t *source,
    ucn_can_source_health_t *health);
const ucn_can_source_stats_t *ucn_can_source_get_stats(
    const ucn_can_source_t *source);

/* CAN-FD carries one complete UCN frame.  output_length is the smallest legal
 * DLC-decoded size and bytes above frame_length are canonical zero padding. */
ucn_result_t ucn_can_fd_carrier_encode(
    const uint8_t *frame,
    size_t frame_length,
    uint8_t output[UCN_CAN_FD_MAX_DATA_BYTES],
    size_t *output_length);

size_t ucn_can_classic_carrier_segment_count(size_t frame_length);
/* transfer_id is allocated by the product's per-CAN-ID TX worker.  That
 * worker must serialize complete Carriers for one CAN ID: a new START for the
 * same ID deliberately replaces the receiver's prior incomplete transfer. */
ucn_result_t ucn_can_classic_carrier_encode_segment(
    const uint8_t *frame,
    size_t frame_length,
    uint8_t transfer_id,
    uint16_t segment_index,
    uint8_t output[UCN_CAN_CLASSIC_MAX_DATA_BYTES],
    size_t *output_length);

#ifdef __cplusplus
}
#endif

#endif
