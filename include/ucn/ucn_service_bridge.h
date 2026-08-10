#ifndef UCN_SERVICE_BRIDGE_H
#define UCN_SERVICE_BRIDGE_H

#include "ucn/ucn_node.h"
#include "ucn/ucn_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* T25.2 is a product/Protocol-Task adapter, not part of the Router or Node
 * state machine.  It contains no RTOS object and must only be called by the
 * one context that owns ucn_node_t. */
typedef void (*ucn_service_bridge_lock_fn)(void *context);
typedef void (*ucn_service_bridge_inbound_observer_fn)(
    void *context,
    const ucn_frame_t *frame,
    ucn_result_t result);

/* The optional hooks are still platform-neutral: T25.3 uses them to guard
 * short Router copies and wake a FreeRTOS consumer, without placing any RTOS
 * type or dependency in the C99 Core.  lock/unlock must either both be set or
 * both be NULL.  observer runs after unlock in the Protocol Task context. */
typedef struct ucn_service_bridge_inbound_hooks {
    void *context;
    ucn_service_bridge_lock_fn lock;
    ucn_service_bridge_lock_fn unlock;
    ucn_service_bridge_inbound_observer_fn observer;
} ucn_service_bridge_inbound_hooks_t;

typedef struct ucn_service_bridge_stats {
    uint32_t endpoint_handlers_installed;
    uint32_t remote_tx_attempted;
    uint32_t remote_tx_accepted;
    uint32_t remote_tx_failed;
    uint32_t inbound_delivered;
    uint32_t inbound_rejected;
    ucn_result_t last_tx_result;
    ucn_result_t last_inbound_result;
} ucn_service_bridge_stats_t;

typedef struct ucn_service_protocol_bridge {
    ucn_service_router_t *router;
    ucn_node_t *node;
    bool endpoint_handlers_installed;
    ucn_service_bridge_inbound_hooks_t inbound_hooks;
    ucn_service_bridge_stats_t stats;
} ucn_service_protocol_bridge_t;

/* Both objects must already be initialized and represent the same local Node
 * ID.  This function neither registers Link objects nor sends a frame. */
ucn_result_t ucn_service_protocol_bridge_init(
    ucn_service_protocol_bridge_t *bridge,
    ucn_service_router_t *router,
    ucn_node_t *node);

/* Claims every static Endpoint in the Router binding table.  Existing
 * handlers owned by another component are rejected rather than overwritten. */
ucn_result_t ucn_service_protocol_bridge_install_endpoint_handlers(
    ucn_service_protocol_bridge_t *bridge);

/* Installs optional Router critical-section and post-delivery hooks for a
 * product Port.  Passing NULL clears every hook.  This API does not create a
 * Task, Queue, or timer and has no wire-format effect. */
ucn_result_t ucn_service_protocol_bridge_set_inbound_hooks(
    ucn_service_protocol_bridge_t *bridge,
    const ucn_service_bridge_inbound_hooks_t *hooks);

/* Protocol Task only: take at most max_requests Remote TX items (Q0 before
 * Q1) and submit each one through the existing Endpoint API.  A dequeued
 * request that Core rejects is deliberately not retried here: Q0 failure must
 * remain explicit, and Q1 route/pending semantics remain entirely in Core.
 * processed may be NULL. */
ucn_result_t ucn_service_protocol_bridge_step(
    ucn_service_protocol_bridge_t *bridge,
    uint8_t max_requests,
    uint8_t *processed);

const ucn_service_bridge_stats_t *ucn_service_protocol_bridge_get_stats(
    const ucn_service_protocol_bridge_t *bridge);

#ifdef __cplusplus
}
#endif

#endif
