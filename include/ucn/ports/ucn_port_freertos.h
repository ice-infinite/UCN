#ifndef UCN_PORTS_FREERTOS_H
#define UCN_PORTS_FREERTOS_H

#include "ucn/ports/ucn_port_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility single-Queue wrapper.  New multi-Bearer products should use
 * ucn_event_runtime_t and map its scheduler hooks to the same primitives.
 * Product glue maps these callbacks to xTaskNotifyGive(),
 * vTaskNotifyGiveFromISR(), and ulTaskNotifyTake() (or an equivalent static
 * queue/semaphore).  UCN intentionally does not include FreeRTOS headers. */
typedef struct ucn_freertos_port_ops {
    void (*notify_protocol_task)(void *context, bool from_isr);
    void (*wait_for_work)(void *context, uint32_t max_wait_ms);
} ucn_freertos_port_ops_t;

typedef struct ucn_freertos_port_config {
    ucn_protocol_owner_config_t owner;
    const ucn_freertos_port_ops_t *ops;
    void *runtime_context;
} ucn_freertos_port_config_t;

typedef struct ucn_freertos_port {
    ucn_protocol_owner_t owner;
    const ucn_freertos_port_ops_t *ops;
    void *runtime_context;
    ucn_port_runtime_stats_t runtime_stats;
    bool initialized;
} ucn_freertos_port_t;

ucn_result_t ucn_freertos_port_init(
    ucn_freertos_port_t *port,
    const ucn_freertos_port_config_t *config);
ucn_result_t ucn_freertos_port_rx_enqueue(
    ucn_freertos_port_t *port,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length,
    bool from_isr);
ucn_result_t ucn_freertos_port_task_step(
    ucn_freertos_port_t *port,
    size_t *pumped,
    uint8_t *bridged);
ucn_result_t ucn_freertos_port_task_wait(
    ucn_freertos_port_t *port,
    uint32_t requested_wait_ms);
const ucn_protocol_owner_stats_t *
ucn_freertos_port_get_stats(const ucn_freertos_port_t *port);
const ucn_port_runtime_stats_t *
ucn_freertos_port_get_runtime_stats(const ucn_freertos_port_t *port);

#ifdef __cplusplus
}
#endif

#endif
