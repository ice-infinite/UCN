#ifndef UCN_PORTS_BARE_METAL_H
#define UCN_PORTS_BARE_METAL_H

#include "ucn/ports/ucn_protocol_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bare-metal Port: call poll() from the product super loop or timer-owned
 * protocol context.  It deliberately has no wait/notification hooks. */
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
