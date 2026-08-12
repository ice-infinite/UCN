#include <string.h>

#include "ucn/ports/ucn_port_host_fake.h"

ucn_result_t ucn_host_fake_port_init(
    ucn_host_fake_port_t *port,
    const ucn_host_fake_port_config_t *config)
{
    ucn_result_t result;

    if (port == NULL || config == NULL || config->ops == NULL ||
        config->ops->notify_protocol_runner == NULL ||
        config->ops->wait_for_work == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(port, 0, sizeof(*port));
    result = ucn_protocol_owner_init(&port->owner, &config->owner);
    if (result != UCN_OK) {
        return result;
    }
    port->ops = config->ops;
    port->runtime_context = config->runtime_context;
    port->initialized = true;
    return UCN_OK;
}

ucn_result_t ucn_host_fake_port_rx_enqueue(
    ucn_host_fake_port_t *port,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length,
    bool from_isr)
{
    ucn_result_t result;

    if (port == NULL || !port->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    result = from_isr ?
        ucn_protocol_owner_rx_enqueue_from_isr(&port->owner, ingress_link,
                                                data, length) :
        ucn_protocol_owner_rx_enqueue(&port->owner, ingress_link, data, length);
    if (result != UCN_OK) {
        return result;
    }
    port->ops->notify_protocol_runner(port->runtime_context, from_isr);
    port->runtime_stats.notifications++;
    if (from_isr) {
        port->runtime_stats.notifications_from_isr++;
    }
    return UCN_OK;
}

ucn_result_t ucn_host_fake_port_step(
    ucn_host_fake_port_t *port,
    size_t *pumped,
    uint8_t *bridged)
{
    return port == NULL || !port->initialized ? UCN_ERR_ARGUMENT :
        ucn_protocol_owner_step(&port->owner, pumped, bridged);
}

ucn_result_t ucn_host_fake_port_wait(
    ucn_host_fake_port_t *port,
    uint32_t requested_wait_ms)
{
    uint32_t capped_wait_ms;

    if (port == NULL || !port->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    capped_wait_ms = requested_wait_ms > UCN_MAX_STEP_INTERVAL_MS ?
                         UCN_MAX_STEP_INTERVAL_MS : requested_wait_ms;
    port->ops->wait_for_work(port->runtime_context, capped_wait_ms);
    port->runtime_stats.waits++;
    port->runtime_stats.last_wait_ms = capped_wait_ms;
    return UCN_OK;
}

const ucn_protocol_owner_stats_t *
ucn_host_fake_port_get_stats(const ucn_host_fake_port_t *port)
{
    return port == NULL ? NULL : ucn_protocol_owner_get_stats(&port->owner);
}

const ucn_port_runtime_stats_t *
ucn_host_fake_port_get_runtime_stats(const ucn_host_fake_port_t *port)
{
    return port == NULL ? NULL : &port->runtime_stats;
}
