#include "ucn/ports/ucn_port_bare_metal.h"

ucn_result_t ucn_bare_metal_port_init(
    ucn_bare_metal_port_t *port,
    const ucn_protocol_owner_config_t *config)
{
    return port == NULL ? UCN_ERR_ARGUMENT :
                          ucn_protocol_owner_init(&port->owner, config);
}

ucn_result_t ucn_bare_metal_port_rx_enqueue(
    ucn_bare_metal_port_t *port,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length)
{
    return port == NULL ? UCN_ERR_ARGUMENT :
                          ucn_protocol_owner_rx_enqueue(&port->owner, ingress_link,
                                                        data, length);
}

ucn_result_t ucn_bare_metal_port_poll(
    ucn_bare_metal_port_t *port,
    size_t *pumped,
    uint8_t *bridged)
{
    return port == NULL ? UCN_ERR_ARGUMENT :
                          ucn_protocol_owner_step(&port->owner, pumped, bridged);
}

const ucn_protocol_owner_stats_t *
ucn_bare_metal_port_get_stats(const ucn_bare_metal_port_t *port)
{
    return port == NULL ? NULL : ucn_protocol_owner_get_stats(&port->owner);
}
