#ifndef UCN_ADAPTER_H
#define UCN_ADAPTER_H

#include "ucn/ucn_node.h"
#include "ucn/ucn_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This is an Adapter-owned queue, not Core state.  Products may reduce it
 * to one frame or replace it with the platform driver's bounded queue. */
#ifndef UCN_ADAPTER_RX_QUEUE_DEPTH
#define UCN_ADAPTER_RX_QUEUE_DEPTH 2U
#endif

#if UCN_ADAPTER_RX_QUEUE_DEPTH < 1
#error "UCN_ADAPTER_RX_QUEUE_DEPTH must be at least 1"
#endif

/* Six-byte MAC addresses and eight-byte EUI addresses are both representable.
 * CAN/UART adapters encode their local physical endpoint in the same field. */
#ifndef UCN_ADAPTER_PHYSICAL_ADDRESS_MAX
#define UCN_ADAPTER_PHYSICAL_ADDRESS_MAX 8U
#endif

#if UCN_ADAPTER_PHYSICAL_ADDRESS_MAX < 1
#error "UCN_ADAPTER_PHYSICAL_ADDRESS_MAX must be at least 1"
#endif

/* Retry jitter is deliberately capped so a configured interval cannot be
 * stretched without a useful upper bound.  The scheduler also validates the
 * resulting worst-case interval against UCN_MAX_SAFE_DURATION_MS. */
#if UCN_FEATURE_DYNAMIC_MESH
#define UCN_ADAPTER_HELLO_MAX_RETRY_JITTER_PERMILLE 500U

typedef enum ucn_adapter_hello_state {
    UCN_ADAPTER_HELLO_DISABLED = 0,
    UCN_ADAPTER_HELLO_INITIAL_JITTER = 1,
    UCN_ADAPTER_HELLO_FAST_RETRY = 2,
    UCN_ADAPTER_HELLO_BACKOFF = 3,
    UCN_ADAPTER_HELLO_ADMITTED_SLOW = 4,
    UCN_ADAPTER_HELLO_ADMITTED_STOP = 5
} ucn_adapter_hello_state_t;

typedef enum ucn_adapter_hello_admitted_policy {
    UCN_ADAPTER_HELLO_ADMITTED_POLICY_SLOW = 0,
    UCN_ADAPTER_HELLO_ADMITTED_POLICY_STOP = 1
} ucn_adapter_hello_admitted_policy_t;

/* This is an Adapter policy, not Core state.  A disabled profile may leave
 * every timing field zero.  An enabled profile supplies one Port-generated
 * random seed at init/restart; adapter_token salts that seed so two media on
 * the same MCU do not inherit the same schedule. */
typedef struct ucn_adapter_hello_config {
    bool enabled;
    ucn_adapter_hello_admitted_policy_t admitted_policy;
    uint8_t max_fast_retries;
    uint16_t retry_jitter_permille;
    uint32_t initial_jitter_min_ms;
    uint32_t initial_jitter_max_ms;
    uint32_t fast_retry_interval_ms;
    uint32_t backoff_initial_ms;
    uint32_t backoff_max_ms;
    uint32_t admitted_slow_interval_ms;
} ucn_adapter_hello_config_t;

typedef struct ucn_adapter_hello_stats {
    uint32_t hellos_due;
    uint32_t discovery_restarts;
    uint32_t admitted_slow_transitions;
    uint32_t admitted_stop_transitions;
} ucn_adapter_hello_stats_t;

typedef struct ucn_adapter_hello_scheduler {
    ucn_adapter_hello_config_t config;
    ucn_adapter_hello_stats_t stats;
    uint32_t adapter_token;
    uint32_t random_state;
    uint32_t next_hello_ms;
    uint32_t backoff_interval_ms;
    ucn_adapter_hello_state_t state;
    uint8_t fast_retries_sent;
    bool initialized;
} ucn_adapter_hello_scheduler_t;
#endif

typedef struct ucn_adapter_address {
    uint8_t length;
    uint8_t bytes[UCN_ADAPTER_PHYSICAL_ADDRESS_MAX];
} ucn_adapter_address_t;

typedef struct ucn_adapter_peer_binding {
    bool occupied;
    ucn_adapter_address_t address;
    ucn_link_t *link;
} ucn_adapter_peer_binding_t;

typedef struct ucn_adapter_rx_item {
    ucn_link_t *ingress_link;
    uint16_t length;
    uint8_t data[UCN_MAX_FRAME_BYTES];
} ucn_adapter_rx_item_t;

typedef struct ucn_adapter_rx_stats {
    uint32_t enqueued;
    uint32_t dropped_full;
    uint32_t pumped;
    uint32_t rejected_by_core;
} ucn_adapter_rx_stats_t;

typedef struct ucn_adapter_rx_queue {
    ucn_adapter_rx_item_t items[UCN_ADAPTER_RX_QUEUE_DEPTH];
    size_t head;
    size_t tail;
    size_t count;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    ucn_adapter_rx_stats_t stats;
} ucn_adapter_rx_queue_t;

bool ucn_adapter_address_is_valid(const ucn_adapter_address_t *address);
bool ucn_adapter_address_equal(const ucn_adapter_address_t *left,
                               const ucn_adapter_address_t *right);
ucn_adapter_peer_binding_t *ucn_adapter_find_peer(
    ucn_adapter_peer_binding_t *bindings,
    size_t binding_count,
    const ucn_adapter_address_t *address);
ucn_result_t ucn_adapter_bind_peer(ucn_adapter_peer_binding_t *bindings,
                                   size_t binding_count,
                                   const ucn_adapter_address_t *address,
                                   ucn_link_t *link);
ucn_result_t ucn_adapter_rx_queue_init(ucn_adapter_rx_queue_t *queue,
                                       const ucn_port_ops_t *port_ops,
                                       void *port_context);
/* Task/owner-context producer.  If this Queue is shared with another task,
 * port_ops supplies the paired task critical callbacks. */
ucn_result_t ucn_adapter_rx_enqueue(ucn_adapter_rx_queue_t *queue,
                                    ucn_link_t *ingress_link,
                                    const uint8_t *data,
                                    size_t length);
/* ISR producer.  This never falls back to task critical callbacks: Queue
 * initialization must have supplied the paired ISR callbacks or this returns
 * UCN_ERR_CONFIG.  The caller still must not pump Node, Bridge, or handlers
 * from the ISR. */
ucn_result_t ucn_adapter_rx_enqueue_from_isr(ucn_adapter_rx_queue_t *queue,
                                             ucn_link_t *ingress_link,
                                             const uint8_t *data,
                                             size_t length);
ucn_result_t ucn_adapter_rx_pump(ucn_adapter_rx_queue_t *queue,
                                 ucn_node_t *node,
                                 size_t max_frames,
                                 size_t *pumped);
const ucn_adapter_rx_stats_t *ucn_adapter_rx_get_stats(
    const ucn_adapter_rx_queue_t *queue);

#if UCN_FEATURE_DYNAMIC_MESH
ucn_result_t ucn_adapter_hello_scheduler_init(
    ucn_adapter_hello_scheduler_t *scheduler,
    const ucn_adapter_hello_config_t *config,
    uint32_t adapter_token,
    uint32_t random_seed,
    uint32_t now_ms);
ucn_result_t ucn_adapter_hello_scheduler_restart(
    ucn_adapter_hello_scheduler_t *scheduler,
    uint32_t random_seed,
    uint32_t now_ms);
/* Call once from the owning Protocol Task.  hello_due is true for one call
 * only; the next bounded deadline is installed before the function returns.
 * STOP is intended for static/pre-admitted or externally reciprocal links.
 * Dynamic discovery media should normally use SLOW, whose first post-admit
 * HELLO is sent after fast_retry_interval_ms to complete reciprocal discovery. */
ucn_result_t ucn_adapter_hello_scheduler_step(
    ucn_adapter_hello_scheduler_t *scheduler,
    uint32_t now_ms,
    bool adapter_has_admitted_peer,
    bool *hello_due);
const ucn_adapter_hello_stats_t *ucn_adapter_hello_scheduler_get_stats(
    const ucn_adapter_hello_scheduler_t *scheduler);
#endif

#ifdef __cplusplus
}
#endif

#endif
