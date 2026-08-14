#ifndef UCN_PORTS_BARE_METAL_H
#define UCN_PORTS_BARE_METAL_H

#include "ucn/ports/ucn_protocol_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility single-Queue Port: call poll() from the product super loop.
 * Multi-Bearer bare-metal products may instead use ucn_event_runtime_t
 * without scheduler hooks, signal fixed Sources from ISR, and run it from
 * the same unique super-loop context. */
typedef struct ucn_bare_metal_port {
    ucn_protocol_owner_t owner;
} ucn_bare_metal_port_t;

ucn_result_t ucn_bare_metal_port_init(
    ucn_bare_metal_port_t *port,
    const ucn_protocol_owner_config_t *config);
ucn_result_t ucn_bare_metal_port_rx_enqueue(
    ucn_bare_metal_port_t *port,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length);
ucn_result_t ucn_bare_metal_port_poll(
    ucn_bare_metal_port_t *port,
    size_t *pumped,
    uint8_t *bridged);
const ucn_protocol_owner_stats_t *
ucn_bare_metal_port_get_stats(const ucn_bare_metal_port_t *port);

#ifdef __cplusplus
}
#endif

#endif
