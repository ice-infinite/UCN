#ifndef UCN_PORTS_PROTOCOL_OWNER_H
#define UCN_PORTS_PROTOCOL_OWNER_H

#include "ucn/ucn_adapter.h"

#if UCN_FEATURE_SERVICE
#include "ucn/ucn_service_bridge.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The common Protocol Owner is deliberately unaware of the scheduler that
 * calls it.  It owns only the execution order, not an RTOS task, a thread, a
 * queue, or any BSP object.  Select a platform wrapper from ucn/ports/ for
 * driver notification and task/thread waiting. */
typedef struct ucn_protocol_owner_config {
    ucn_node_t *node;
    ucn_adapter_rx_queue_t *rx_queue;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    size_t max_rx_frames_per_step;
#if UCN_FEATURE_SERVICE
    ucn_service_protocol_bridge_t *bridge;
    uint8_t max_bridge_requests_per_step;
#endif
} ucn_protocol_owner_config_t;

typedef struct ucn_protocol_owner_stats {
    uint32_t steps;
    uint32_t rx_enqueued;
    uint32_t rx_rejected;
    uint32_t rx_frames_pumped;
#if UCN_FEATURE_SERVICE
    uint32_t bridge_requests_processed;
#endif
    uint32_t last_now_ms;
    ucn_result_t last_rx_result;
#if UCN_FEATURE_SERVICE
    ucn_result_t last_bridge_result;
#endif
    ucn_result_t last_node_step_result;
} ucn_protocol_owner_stats_t;

typedef struct ucn_protocol_owner {
    ucn_protocol_owner_config_t config;
    ucn_protocol_owner_stats_t stats;
    bool initialized;
} ucn_protocol_owner_t;

/* Binds one Node to one bounded Adapter RX queue.  The caller must ensure
 * there is exactly one owner execution context per Node. */
ucn_result_t ucn_protocol_owner_init(
    ucn_protocol_owner_t *owner,
    const ucn_protocol_owner_config_t *config);

/* Driver/owner-task entry: copy a complete frame into the Adapter-owned
 * queue.  This function never invokes Node, Bridge, scheduling, or
 * application code. */
ucn_result_t ucn_protocol_owner_rx_enqueue(
    ucn_protocol_owner_t *owner,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length);

/* ISR-only variant.  It requires the Adapter Queue's paired ISR critical
 * callbacks and returns UCN_ERR_CONFIG rather than using a task lock when
 * they were not supplied.  It still only copies into the bounded Queue. */
ucn_result_t ucn_protocol_owner_rx_enqueue_from_isr(
    ucn_protocol_owner_t *owner,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length);

/* Owner-context entry: reads the clock once, pumps bounded RX work, runs an
 * optional Bridge, then runs Node maintenance with the same timestamp. */
ucn_result_t ucn_protocol_owner_step(
    ucn_protocol_owner_t *owner,
    size_t *pumped,
    uint8_t *bridged);

const ucn_protocol_owner_stats_t *
ucn_protocol_owner_get_stats(const ucn_protocol_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif
