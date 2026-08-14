#include <string.h>

#include "ucn/ports/ucn_protocol_owner.h"

ucn_result_t ucn_protocol_owner_init(
    ucn_protocol_owner_t *owner,
    const ucn_protocol_owner_config_t *config)
{
    if (owner == NULL || config == NULL || config->node == NULL ||
        config->rx_queue == NULL ||
        !ucn_port_ops_is_compatible(config->port_ops) ||
        config->port_ops->now_ms == NULL ||
        config->max_rx_frames_per_step == 0U) {
        return UCN_ERR_ARGUMENT;
    }
#if UCN_FEATURE_SERVICE
    if (config->bridge != NULL &&
        (config->bridge->node != config->node ||
         config->max_bridge_requests_per_step == 0U)) {
        return UCN_ERR_CONFIG;
    }
#endif

    (void)memset(owner, 0, sizeof(*owner));
    owner->config = *config;
    owner->stats.last_rx_result = UCN_OK;
#if UCN_FEATURE_SERVICE
    owner->stats.last_bridge_result = UCN_OK;
#endif
    owner->stats.last_node_step_result = UCN_OK;
    owner->initialized = true;
    return UCN_OK;
}

static ucn_result_t protocol_owner_rx_enqueue(
    ucn_protocol_owner_t *owner,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length,
    bool from_isr)
{
    ucn_result_t result;

    if (owner == NULL || !owner->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    result = from_isr ?
        ucn_adapter_rx_enqueue_from_isr(owner->config.rx_queue, ingress_link,
                                        data, length) :
        ucn_adapter_rx_enqueue(owner->config.rx_queue, ingress_link, data,
                               length);
    owner->stats.last_rx_result = result;
    if (result != UCN_OK) {
        owner->stats.rx_rejected++;
        return result;
    }
    owner->stats.rx_enqueued++;
    return UCN_OK;
}

ucn_result_t ucn_protocol_owner_rx_enqueue(
    ucn_protocol_owner_t *owner,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length)
{
    return protocol_owner_rx_enqueue(owner, ingress_link, data, length, false);
}

ucn_result_t ucn_protocol_owner_rx_enqueue_from_isr(
    ucn_protocol_owner_t *owner,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length)
{
    return protocol_owner_rx_enqueue(owner, ingress_link, data, length, true);
}

ucn_result_t ucn_protocol_owner_step(
    ucn_protocol_owner_t *owner,
    size_t *pumped,
    uint8_t *bridged)
{
    ucn_result_t first_error = UCN_OK;
    ucn_result_t result;
    size_t local_pumped = 0U;
    uint8_t local_bridged = 0U;
    uint32_t now_ms;

    if (pumped != NULL) {
        *pumped = 0U;
    }
    if (bridged != NULL) {
        *bridged = 0U;
    }
    if (owner == NULL || !owner->initialized) {
        return UCN_ERR_ARGUMENT;
    }

    now_ms = owner->config.port_ops->now_ms(owner->config.port_context);
    owner->stats.last_now_ms = now_ms;
    result = ucn_adapter_rx_pump(owner->config.rx_queue, owner->config.node,
                                 owner->config.max_rx_frames_per_step,
                                 &local_pumped);
    owner->stats.rx_frames_pumped += (uint32_t)local_pumped;
    owner->stats.last_rx_result = result;
    if (result != UCN_OK) {
        first_error = result;
    }

#if UCN_FEATURE_SERVICE
    if (owner->config.bridge != NULL) {
        result = ucn_service_protocol_bridge_step_at(
            owner->config.bridge, now_ms,
            owner->config.max_bridge_requests_per_step, &local_bridged);
        owner->stats.bridge_requests_processed += (uint32_t)local_bridged;
        owner->stats.last_bridge_result = result;
        if (first_error == UCN_OK && result != UCN_OK) {
            first_error = result;
        }
    }
#endif

    result = ucn_node_step(owner->config.node, now_ms);
    owner->stats.last_node_step_result = result;
    owner->stats.steps++;
    /* Core uses NOT_FOUND as its documented idle sentinel.  Preserve it in
     * stats but do not make a normal periodic owner iteration fail. */
    if (first_error == UCN_OK && result != UCN_OK && result != UCN_ERR_NOT_FOUND) {
        first_error = result;
    }
    if (pumped != NULL) {
        *pumped = local_pumped;
    }
    if (bridged != NULL) {
        *bridged = local_bridged;
    }
    return first_error;
}

const ucn_protocol_owner_stats_t *
ucn_protocol_owner_get_stats(const ucn_protocol_owner_t *owner)
{
    return owner == NULL ? NULL : &owner->stats;
}
