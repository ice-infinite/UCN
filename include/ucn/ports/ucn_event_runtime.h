#ifndef UCN_PORTS_EVENT_RUNTIME_H
#define UCN_PORTS_EVENT_RUNTIME_H

#include "ucn/ports/ucn_protocol_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed source capacity belongs to the application-owned Runtime object, not
 * ucn_node_t.  Products with fewer UART/CAN/USB/Wireless inputs may override
 * it globally without changing the Wire protocol. */
#ifndef UCN_EVENT_RUNTIME_MAX_SOURCES
#define UCN_EVENT_RUNTIME_MAX_SOURCES 8U
#endif

#ifndef UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS
#define UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS 8U
#endif

#ifndef UCN_EVENT_RUNTIME_DEFAULT_SOURCE_BUDGET
#define UCN_EVENT_RUNTIME_DEFAULT_SOURCE_BUDGET 4U
#endif

typedef char ucn_event_runtime_source_capacity_must_be_1_to_16[
    UCN_EVENT_RUNTIME_MAX_SOURCES >= 1U &&
            UCN_EVENT_RUNTIME_MAX_SOURCES <= 16U ? 1 : -1];
typedef char ucn_event_runtime_round_default_must_be_1_to_64[
    UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS >= 1U &&
            UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS <= 64U ? 1 : -1];
typedef char ucn_event_runtime_source_budget_must_be_positive[
    UCN_EVENT_RUNTIME_DEFAULT_SOURCE_BUDGET >= 1U ? 1 : -1];

typedef uint8_t ucn_event_source_id_t;
typedef uint8_t ucn_event_source_events_t;
typedef uint8_t ucn_event_owner_events_t;

#define UCN_EVENT_SOURCE_INVALID UINT8_MAX

/* Source-local reasons.  FALLBACK is supplied only by Runtime timeout scans;
 * a Source must return only RX/TX/STATUS in pending_events. */
#define UCN_EVENT_SOURCE_RX_READY ((ucn_event_source_events_t)UINT8_C(0x01))
#define UCN_EVENT_SOURCE_TX_READY ((ucn_event_source_events_t)UINT8_C(0x02))
#define UCN_EVENT_SOURCE_STATUS_CHANGED \
    ((ucn_event_source_events_t)UINT8_C(0x04))
#define UCN_EVENT_SOURCE_PENDING_MASK \
    ((ucn_event_source_events_t)(UCN_EVENT_SOURCE_RX_READY | \
                                  UCN_EVENT_SOURCE_TX_READY | \
                                  UCN_EVENT_SOURCE_STATUS_CHANGED))
#define UCN_EVENT_SOURCE_FALLBACK_SCAN \
    ((ucn_event_source_events_t)UINT8_C(0x80))

/* Owner-local reasons cover work that does not originate in a physical
 * Source.  RX_QUEUE is set by direct complete-frame submit. */
#define UCN_EVENT_OWNER_RX_QUEUE \
    ((ucn_event_owner_events_t)UINT8_C(0x01))
#define UCN_EVENT_OWNER_SERVICE \
    ((ucn_event_owner_events_t)UINT8_C(0x02))
#define UCN_EVENT_OWNER_TIMER \
    ((ucn_event_owner_events_t)UINT8_C(0x04))
#define UCN_EVENT_OWNER_APPLICATION \
    ((ucn_event_owner_events_t)UINT8_C(0x08))
#define UCN_EVENT_OWNER_PENDING_MASK \
    ((ucn_event_owner_events_t)(UCN_EVENT_OWNER_RX_QUEUE | \
                                 UCN_EVENT_OWNER_SERVICE | \
                                 UCN_EVENT_OWNER_TIMER | \
                                 UCN_EVENT_OWNER_APPLICATION))

typedef struct ucn_event_source_service_result {
    size_t work_done;
    ucn_event_source_events_t pending_events;
} ucn_event_source_service_result_t;

/* Called only by the unique Owner context.  The callback drains a bounded
 * amount from its driver-owned Ring/DMA/packet queue and may submit complete
 * UCN frames through ucn_event_runtime_submit_frame().  It never runs in ISR.
 * pending_events re-arms work without requiring a second hardware interrupt. */
typedef ucn_result_t (*ucn_event_source_service_fn)(
    void *context,
    ucn_event_source_events_t events,
    size_t max_work,
    ucn_event_source_service_result_t *result);

typedef struct ucn_event_source_ops {
    ucn_event_source_service_fn service;
} ucn_event_source_ops_t;

typedef struct ucn_event_source_config {
    const ucn_event_source_ops_t *ops;
    void *context;
} ucn_event_source_config_t;

/* These three hooks are the only scheduler-specific part of the standard
 * Runtime.  FreeRTOS maps them to Task Notification, Zephyr/NuttX/RT-Thread
 * to an event/semaphore, and bare metal may omit the whole table. */
typedef struct ucn_event_runtime_scheduler_ops {
    void (*notify_owner)(void *context, bool from_isr);
    /* Return true when a notification woke the Owner, false on timeout. */
    bool (*wait_owner)(void *context, uint32_t max_wait_ms);
    /* Optional; used when the fixed drain-round budget was exhausted. */
    void (*yield_owner)(void *context);
} ucn_event_runtime_scheduler_ops_t;

typedef struct ucn_event_runtime_config {
    ucn_protocol_owner_config_t owner;
    const ucn_event_runtime_scheduler_ops_t *scheduler_ops;
    void *scheduler_context;
    /* Zero selects the compile-time defaults above. */
    uint8_t max_drain_rounds;
    size_t max_source_work_per_round;
} ucn_event_runtime_config_t;

typedef struct ucn_event_runtime_stats {
    uint32_t source_signals;
    uint32_t source_signals_from_isr;
    uint32_t owner_signals;
    uint32_t owner_signals_from_isr;
    uint32_t scheduler_notifications;
    uint32_t waits;
    uint32_t wait_timeouts;
    uint32_t fallback_scans;
    uint32_t runs;
    uint32_t drain_rounds;
    uint32_t source_callbacks;
    uint32_t source_work;
    uint32_t frames_submitted;
    uint32_t frames_rejected;
    uint32_t drain_budget_hits;
    uint32_t yields;
    ucn_result_t last_source_result;
    ucn_result_t last_owner_result;
} ucn_event_runtime_stats_t;

typedef struct ucn_event_runtime_run_result {
    uint8_t rounds;
    size_t source_work;
    size_t rx_frames_pumped;
    uint32_t bridge_requests_processed;
    bool fallback_scan;
    bool work_remaining;
} ucn_event_runtime_run_result_t;

typedef struct ucn_event_runtime_source {
    const ucn_event_source_ops_t *ops;
    void *context;
    bool occupied;
} ucn_event_runtime_source_t;

typedef struct ucn_event_runtime {
    ucn_protocol_owner_t owner;
    ucn_event_runtime_source_t sources[UCN_EVENT_RUNTIME_MAX_SOURCES];
    ucn_event_source_events_t
        pending_source_events[UCN_EVENT_RUNTIME_MAX_SOURCES];
    ucn_event_owner_events_t pending_owner_events;
    const ucn_event_runtime_scheduler_ops_t *scheduler_ops;
    void *scheduler_context;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    ucn_event_runtime_stats_t stats;
    uint8_t max_drain_rounds;
    size_t max_source_work_per_round;
    bool initialized;
} ucn_event_runtime_t;

ucn_result_t ucn_event_runtime_init(
    ucn_event_runtime_t *runtime,
    const ucn_event_runtime_config_t *config);

/* Source IDs are fixed slots.  Binding is startup-only and deterministic. */
ucn_result_t ucn_event_runtime_bind_source(
    ucn_event_runtime_t *runtime,
    ucn_event_source_id_t source_id,
    const ucn_event_source_config_t *config);

ucn_result_t ucn_event_runtime_signal_source(
    ucn_event_runtime_t *runtime,
    ucn_event_source_id_t source_id,
    ucn_event_source_events_t events);
ucn_result_t ucn_event_runtime_signal_source_from_isr(
    ucn_event_runtime_t *runtime,
    ucn_event_source_id_t source_id,
    ucn_event_source_events_t events);

ucn_result_t ucn_event_runtime_signal_owner(
    ucn_event_runtime_t *runtime,
    ucn_event_owner_events_t events);
ucn_result_t ucn_event_runtime_signal_owner_from_isr(
    ucn_event_runtime_t *runtime,
    ucn_event_owner_events_t events);

/* Packet-style drivers that already hold one complete UCN frame can use these
 * combined Queue + notification helpers.  Stream/segmented drivers normally
 * signal their Source and submit frames later from service(). */
ucn_result_t ucn_event_runtime_submit_frame(
    ucn_event_runtime_t *runtime,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length);
ucn_result_t ucn_event_runtime_submit_frame_from_isr(
    ucn_event_runtime_t *runtime,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length);

bool ucn_event_runtime_has_pending(ucn_event_runtime_t *runtime);

/* Run in the unique Owner context.  fallback_scan=true services every bound
 * Source once with FALLBACK_SCAN before normal pending events; it is intended
 * for timeout/no-interrupt recovery, never for the primary RX path. */
ucn_result_t ucn_event_runtime_run(
    ucn_event_runtime_t *runtime,
    bool fallback_scan,
    ucn_event_runtime_run_result_t *result);

/* Standard RTOS cycle: skip waiting when work is already pending, otherwise
 * wait up to min(requested, UCN_MAX_STEP_INTERVAL_MS).  Timeout triggers one
 * fallback scan.  Bare-metal products may instead call has_pending()/run()
 * directly and use WFI in their own super loop. */
ucn_result_t ucn_event_runtime_task_cycle(
    ucn_event_runtime_t *runtime,
    uint32_t requested_wait_ms,
    ucn_event_runtime_run_result_t *result);

const ucn_event_runtime_stats_t *ucn_event_runtime_get_stats(
    const ucn_event_runtime_t *runtime);
const ucn_protocol_owner_stats_t *ucn_event_runtime_get_owner_stats(
    const ucn_event_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
