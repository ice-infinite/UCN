#include <string.h>

#include "ucn/ucn.h"
#include "ucn/ucn_endpoint.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node_storage.h"
#include "ucn/ucn_time.h"

#include "ucn_duplicate_internal.h"

#if UCN_PROFILE != UCN_PROFILE_NANO
#error "ucn_node_nano.c is only for the Nano profile"
#endif

static bool nano_link_is_registered(const ucn_node_t *node,
                                    const ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index] == link) {
            return true;
        }
    }
    return false;
}

static ucn_result_t nano_resolve_link_local_receive_profile(
    const ucn_node_t *node,
    const ucn_link_t *link,
    ucn_wire_profile_t *profile)
{
    ucn_wire_profile_t configured;

    if (node == NULL || link == NULL || profile == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    configured = (ucn_wire_profile_t)link->local_receive_wire_profile;
    if (configured == UCN_WIRE_PROFILE_UNSPECIFIED) {
        *profile = node->max_receive_wire_profile;
        return UCN_OK;
    }
    if (ucn_wire_profile_get_descriptor(configured) == NULL ||
        configured > node->max_receive_wire_profile) {
        return UCN_ERR_CONFIG;
    }
    *profile = configured;
    return UCN_OK;
}

static ucn_result_t nano_link_status(const ucn_link_t *link,
                                     ucn_link_status_t *status)
{
    if (link == NULL || status == NULL || link->ops == NULL ||
        link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(status, 0, sizeof(*status));
    return link->ops->get_status(link, status);
}

static ucn_link_t *nano_find_link(ucn_node_t *node,
                                  ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index]->peer_node_id == destination) {
            return node->links[index];
        }
    }
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination) {
            return node->routes[index].egress_link;
        }
    }
    return NULL;
}

static ucn_endpoint_handler_entry_t *nano_find_endpoint_handler(
    ucn_node_t *node,
    ucn_endpoint_t endpoint)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ENDPOINT_HANDLERS; ++index) {
        if (node->endpoint_handlers[index].occupied &&
            node->endpoint_handlers[index].endpoint == endpoint) {
            return &node->endpoint_handlers[index];
        }
    }
    return NULL;
}

static ucn_sequence_t nano_allocate_sequence(ucn_node_t *node)
{
    ucn_sequence_t sequence = node->next_sequence++;

    if (sequence == 0U) {
        sequence = node->next_sequence++;
    }
    if (node->next_sequence == 0U) {
        node->next_sequence = 1U;
    }
    return sequence;
}

static ucn_result_t nano_send_frame(ucn_node_t *node,
                                    ucn_link_t *link,
                                    ucn_frame_t *frame)
{
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_link_status_t status;
    ucn_result_t result;

    result = nano_link_status(link, &status);
    if (result != UCN_OK) {
        return result;
    }
    if (!status.is_up) {
        return UCN_ERR_LINK_DOWN;
    }
    if (ucn_link_effective_mtu(link, &status) == 0U) {
        return UCN_ERR_LINK_DOWN;
    }
    if (frame->wire_profile == UCN_WIRE_PROFILE_UNSPECIFIED) {
        ucn_wire_profile_t maximum_profile = node->tx_wire_profile;
        const size_t mtu = ucn_link_effective_mtu(link, &status);

        if (link->peer_wire_profile != UCN_WIRE_PROFILE_UNSPECIFIED &&
            link->peer_wire_profile < maximum_profile) {
            maximum_profile = link->peer_wire_profile;
        }
        if (node->automatic_wire_profile) {
            result = ucn_frame_select_min_wire_profile(
                frame, maximum_profile, mtu, &frame->wire_profile);
            if (result != UCN_OK) {
                return result;
            }
        } else {
            if (node->tx_wire_profile > maximum_profile) {
                return UCN_ERR_UNSUPPORTED;
            }
            frame->wire_profile = node->tx_wire_profile;
        }
    } else if (link->peer_wire_profile != UCN_WIRE_PROFILE_UNSPECIFIED &&
               frame->wire_profile > link->peer_wire_profile) {
        return UCN_ERR_UNSUPPORTED;
    }
    result = ucn_frame_encode(frame, encoded, sizeof(encoded), &encoded_length);
    if (result != UCN_OK) {
        return result;
    }
    if (encoded_length > ucn_link_effective_mtu(link, &status)) {
        return UCN_ERR_TOO_LARGE;
    }
    result = link->ops->send(link, encoded, encoded_length);
    if (result == UCN_OK) {
        node->stats.tx_sent++;
    } else {
        node->stats.tx_error_dropped++;
    }
    return result;
}

static ucn_result_t nano_send_existing_frame(ucn_node_t *node,
                                             ucn_link_t *link,
                                             ucn_frame_t *frame)
{
    if (frame->hop_limit <= 1U) {
        return UCN_ERR_TTL;
    }
    --frame->hop_limit;
    return nano_send_frame(node, link, frame);
}

static bool nano_dispatch_endpoint(ucn_node_t *node,
                                   const ucn_frame_t *frame)
{
    ucn_endpoint_handler_entry_t *entry;

    if (!ucn_endpoint_is_static((ucn_endpoint_t)frame->message_type)) {
        return false;
    }
    entry = nano_find_endpoint_handler(node,
                                       (ucn_endpoint_t)frame->message_type);
    if (entry == NULL || entry->handler == NULL) {
        return false;
    }
    entry->handler(entry->context, frame);
    return true;
}

static ucn_tx_item_t *nano_select_queue_item(ucn_tx_item_t *queue,
                                             size_t depth)
{
    size_t index;
    ucn_tx_item_t *oldest = NULL;

    for (index = 0U; index < depth; ++index) {
        if (queue[index].occupied &&
            (oldest == NULL || queue[index].order < oldest->order)) {
            oldest = &queue[index];
        }
    }
    return oldest;
}

ucn_result_t ucn_node_init(ucn_node_t *node, const ucn_config_t *config)
{
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_validate_config(config);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(node, 0, sizeof(*node));
    node->config = *config;
    node->tx_wire_profile = UCN_WIRE_PROFILE_W3_BACKBONE;
    node->max_receive_wire_profile = UCN_WIRE_PROFILE_W3_BACKBONE;
    node->next_sequence = 1U;
    node->next_queue_order = 1U;
    return UCN_OK;
}

ucn_result_t ucn_node_set_wire_profiles(
    ucn_node_t *node,
    ucn_wire_profile_t tx_profile,
    ucn_wire_profile_t max_receive_profile)
{
    const ucn_wire_profile_descriptor_t *tx;
    const ucn_wire_profile_descriptor_t *rx;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->link_count != 0U) {
        return UCN_ERR_CONFIG;
    }
    tx = ucn_wire_profile_get_descriptor(tx_profile);
    rx = ucn_wire_profile_get_descriptor(max_receive_profile);
    if (tx == NULL || rx == NULL || max_receive_profile < tx_profile) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->config.network_id > tx->max_wire_value ||
        node->config.node_id > tx->max_node_id ||
        node->config.default_hop_limit > tx->max_hops ||
        node->session_id > tx->max_wire_value) {
        return UCN_ERR_TOO_LARGE;
    }
    node->tx_wire_profile = tx_profile;
    node->max_receive_wire_profile = max_receive_profile;
    return UCN_OK;
}

ucn_wire_profile_t ucn_node_get_tx_wire_profile(const ucn_node_t *node)
{
    return node == NULL ? UCN_WIRE_PROFILE_UNSPECIFIED : node->tx_wire_profile;
}

ucn_wire_profile_t ucn_node_get_max_receive_wire_profile(
    const ucn_node_t *node)
{
    return node == NULL ? UCN_WIRE_PROFILE_UNSPECIFIED :
                          node->max_receive_wire_profile;
}

ucn_result_t ucn_node_set_wire_profile_auto(ucn_node_t *node, bool enabled)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->automatic_wire_profile = enabled;
    return UCN_OK;
}

bool ucn_node_wire_profile_auto(const ucn_node_t *node)
{
    return node != NULL && node->automatic_wire_profile;
}

ucn_result_t ucn_node_set_link_wire_profile_limit(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_wire_profile_t maximum_profile)
{
    if (node == NULL || link == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!nano_link_is_registered(node, link)) {
        return UCN_ERR_NOT_FOUND;
    }
    if (maximum_profile != UCN_WIRE_PROFILE_UNSPECIFIED &&
        ucn_wire_profile_get_descriptor(maximum_profile) == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    link->peer_wire_profile = maximum_profile;
    return UCN_OK;
}

ucn_wire_profile_t ucn_node_get_link_wire_profile_limit(
    const ucn_node_t *node,
    const ucn_link_t *link)
{
    return node == NULL || link == NULL ||
           !nano_link_is_registered(node, link) ?
               UCN_WIRE_PROFILE_UNSPECIFIED : link->peer_wire_profile;
}

ucn_result_t ucn_node_set_link_local_wire_profile_limit(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_wire_profile_t maximum_profile)
{
    ucn_wire_profile_t effective_profile = maximum_profile;

    if (node == NULL || link == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (maximum_profile == UCN_WIRE_PROFILE_UNSPECIFIED) {
        effective_profile = node->max_receive_wire_profile;
    } else if (ucn_wire_profile_get_descriptor(maximum_profile) == NULL ||
               maximum_profile > node->max_receive_wire_profile) {
        return UCN_ERR_ARGUMENT;
    }
    if (link->mtu != 0U &&
        link->mtu < ucn_frame_header_size_for_profile(effective_profile, 0U)) {
        return UCN_ERR_TOO_LARGE;
    }
    link->local_receive_wire_profile = (uint8_t)maximum_profile;
    return UCN_OK;
}

ucn_wire_profile_t ucn_node_get_link_local_wire_profile_limit(
    const ucn_node_t *node,
    const ucn_link_t *link)
{
    if (node == NULL || link == NULL) {
        return UCN_WIRE_PROFILE_UNSPECIFIED;
    }
    return (ucn_wire_profile_t)link->local_receive_wire_profile;
}

ucn_result_t ucn_node_set_plain_session_id(ucn_node_t *node,
                                           ucn_session_id_t session_id)
{
    if (node == NULL || session_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (session_id > ucn_wire_profile_get_descriptor(
                         node->tx_wire_profile)->max_wire_value) {
        return UCN_ERR_TOO_LARGE;
    }
    node->session_id = session_id;
    return UCN_OK;
}

ucn_result_t ucn_node_set_security_required(ucn_node_t *node, bool required)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    return required ? UCN_ERR_CONFIG : UCN_OK;
}

bool ucn_node_security_ready(const ucn_node_t *node)
{
    return node != NULL;
}

ucn_result_t ucn_node_set_security(ucn_node_t *node,
                                   const ucn_security_ops_t *ops,
                                   void *context)
{
    (void)ops;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_set_security_policy(ucn_node_t *node,
                                          const ucn_security_policy_t *policy)
{
    (void)policy;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_set_endpoint_security_policy(
    ucn_node_t *node,
    ucn_endpoint_t endpoint,
    const ucn_security_policy_t *policy)
{
    (void)endpoint;
    (void)policy;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_set_join_policy(ucn_node_t *node,
                                      ucn_join_policy_t policy,
                                      ucn_neighbor_authorize_fn authorize,
                                      void *context)
{
    (void)policy;
    (void)authorize;
    (void)context;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_observe_neighbor(ucn_node_t *node,
                                       ucn_link_t *link,
                                       uint32_t now_ms)
{
    (void)link;
    (void)now_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_probe_neighbor(ucn_node_t *node,
                                     ucn_link_t *link,
                                     uint32_t now_ms)
{
    (void)link;
    (void)now_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_broadcast_hello(ucn_node_t *node,
                                      ucn_link_t *link,
                                      uint32_t now_ms)
{
    (void)link;
    (void)now_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_admit_neighbor(ucn_node_t *node,
                                     ucn_node_id_t peer_node_id)
{
    (void)peer_node_id;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_reject_neighbor(ucn_node_t *node,
                                      ucn_node_id_t peer_node_id)
{
    (void)peer_node_id;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

size_t ucn_node_neighbor_count(const ucn_node_t *node,
                               ucn_neighbor_state_t state)
{
    (void)node;
    (void)state;
    return 0U;
}

ucn_result_t ucn_node_register_link(ucn_node_t *node, ucn_link_t *link)
{
    size_t index;
    ucn_wire_profile_t local_receive_profile;
    ucn_result_t result;

    if (node == NULL || link == NULL || link->ops == NULL ||
        link->ops->send == NULL || link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_link_liveness_profile_is_valid(link->liveness_profile)) {
        return UCN_ERR_CONFIG;
    }
    result = nano_resolve_link_local_receive_profile(node, link,
                                                     &local_receive_profile);
    if (result != UCN_OK) {
        return result;
    }
    if (link->mtu != 0U &&
        link->mtu < ucn_frame_header_size_for_profile(local_receive_profile,
                                                       0U)) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index] == link ||
            node->links[index]->link_id == link->link_id) {
            return UCN_ERR_CONFIG;
        }
    }
    if (node->link_count >= UCN_MAX_LINKS) {
        return UCN_ERR_NO_SPACE;
    }
    if (link->ops->open != NULL) {
        result = link->ops->open(link);
        if (result != UCN_OK) {
            return result;
        }
    }
    link->peer_wire_profile = UCN_WIRE_PROFILE_UNSPECIFIED;
    node->links[node->link_count++] = link;
    return UCN_OK;
}

ucn_result_t ucn_node_add_route(ucn_node_t *node,
                                ucn_node_id_t destination,
                                ucn_link_t *egress_link)
{
    size_t index;
    ucn_route_entry_t *free_slot = NULL;

    if (node == NULL || destination == 0U ||
        destination == UCN_NODE_BROADCAST || egress_link == NULL ||
        !nano_link_is_registered(node, egress_link)) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination) {
            node->routes[index].is_static = true;
            node->routes[index].egress_link = egress_link;
            return UCN_OK;
        }
        if (!node->routes[index].valid && free_slot == NULL) {
            free_slot = &node->routes[index];
        }
    }
    if (free_slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    free_slot->valid = true;
    free_slot->is_static = true;
    free_slot->destination = destination;
    free_slot->egress_link = egress_link;
    return UCN_OK;
}

ucn_result_t ucn_node_discover_route(ucn_node_t *node,
                                     ucn_node_id_t destination,
                                     uint32_t now_ms)
{
    (void)destination;
    (void)now_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

ucn_result_t ucn_node_refresh_route(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    uint32_t now_ms)
{
    (void)destination;
    (void)now_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
}

bool ucn_node_route_pending(const ucn_node_t *node,
                            ucn_node_id_t destination)
{
    (void)node;
    (void)destination;
    return false;
}

void ucn_node_set_rx_handler(ucn_node_t *node,
                             ucn_rx_handler_t handler,
                             void *context)
{
    if (node != NULL) {
        node->rx_handler = handler;
        node->rx_context = context;
    }
}

ucn_result_t ucn_node_set_endpoint_handler(ucn_node_t *node,
                                            ucn_endpoint_t endpoint,
                                            ucn_endpoint_rx_handler_t handler,
                                            void *context)
{
    ucn_endpoint_handler_entry_t *entry;
    size_t index;

    if (node == NULL || !ucn_endpoint_is_static(endpoint)) {
        return UCN_ERR_ARGUMENT;
    }
    entry = nano_find_endpoint_handler(node, endpoint);
    if (entry != NULL) {
        if (handler == NULL) {
            (void)memset(entry, 0, sizeof(*entry));
        } else {
            entry->handler = handler;
            entry->context = context;
        }
        return UCN_OK;
    }
    if (handler == NULL) {
        return UCN_OK;
    }
    for (index = 0U; index < UCN_MAX_ENDPOINT_HANDLERS; ++index) {
        if (!node->endpoint_handlers[index].occupied) {
            node->endpoint_handlers[index].occupied = true;
            node->endpoint_handlers[index].endpoint = endpoint;
            node->endpoint_handlers[index].handler = handler;
            node->endpoint_handlers[index].context = context;
            return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

ucn_result_t ucn_node_send(ucn_node_t *node,
                           ucn_node_id_t destination,
                           uint8_t message_type,
                           ucn_traffic_class_t traffic_class,
                           const uint8_t *payload,
                           uint16_t payload_length)
{
    ucn_link_t *link;
    ucn_frame_t frame;

    if (node == NULL || destination == 0U ||
        destination == UCN_NODE_BROADCAST ||
        destination == node->config.node_id ||
        ucn_message_type_is_control(message_type) ||
        (payload == NULL && payload_length != 0U)) {
        return UCN_ERR_ARGUMENT;
    }
    if (traffic_class != UCN_TRAFFIC_Q0_CRITICAL &&
        traffic_class != UCN_TRAFFIC_Q1_REALTIME) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (payload_length > ucn_frame_max_payload_for_profile(
                             node->tx_wire_profile, 0U)) {
        return UCN_ERR_TOO_LARGE;
    }
    link = nano_find_link(node, destination);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = traffic_class;
    frame.hop_limit = node->automatic_wire_profile &&
                      link->peer_node_id == destination ?
                          1U : node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.sequence = nano_allocate_sequence(node);
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    return nano_send_frame(node, link, &frame);
}

ucn_result_t ucn_node_send_endpoint(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    ucn_endpoint_t endpoint,
                                    ucn_traffic_class_t traffic_class,
                                    const uint8_t *payload,
                                    uint16_t payload_length)
{
    if (!ucn_endpoint_is_static(endpoint)) {
        return UCN_ERR_ARGUMENT;
    }
    return ucn_node_send(node, destination, endpoint, traffic_class,
                         payload, payload_length);
}

ucn_result_t ucn_node_enqueue(ucn_node_t *node,
                              const ucn_send_request_t *request)
{
    ucn_tx_item_t *queue;
    size_t depth;
    size_t index;
    ucn_tx_item_t *slot = NULL;

    if (node == NULL || request == NULL || request->destination == 0U ||
        request->destination == UCN_NODE_BROADCAST ||
        request->destination == node->config.node_id ||
        (request->payload == NULL && request->payload_length != 0U) ||
        ucn_message_type_is_control(request->message_type)) {
        return UCN_ERR_ARGUMENT;
    }
    if (request->traffic_class != UCN_TRAFFIC_Q0_CRITICAL &&
        request->traffic_class != UCN_TRAFFIC_Q1_REALTIME) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (request->payload_length > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    if (request->delivery != UCN_DELIVERY_BEST_EFFORT &&
        request->delivery != UCN_DELIVERY_LATEST_VALUE &&
        request->delivery != UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
        return UCN_ERR_ARGUMENT;
    }
    if (request->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
        (request->traffic_class != UCN_TRAFFIC_Q0_CRITICAL ||
         request->deadline_ms == 0U)) {
        return UCN_ERR_ARGUMENT;
    }
    queue = request->traffic_class == UCN_TRAFFIC_Q0_CRITICAL ?
                node->q0 : node->q1;
    depth = request->traffic_class == UCN_TRAFFIC_Q0_CRITICAL ?
                UCN_TX_Q0_DEPTH : UCN_TX_Q1_DEPTH;
    for (index = 0U; index < depth; ++index) {
        if (request->delivery == UCN_DELIVERY_LATEST_VALUE &&
            queue[index].occupied &&
            queue[index].destination == request->destination &&
            queue[index].message_type == request->message_type) {
            slot = &queue[index];
            break;
        }
        if (!queue[index].occupied && slot == NULL) {
            slot = &queue[index];
        }
    }
    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    (void)memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->destination = request->destination;
    slot->message_type = request->message_type;
    slot->traffic_class = request->traffic_class;
    slot->delivery = request->delivery;
    slot->deadline_ms = request->deadline_ms;
    slot->order = node->next_queue_order++;
    slot->payload_length = request->payload_length;
    if (request->payload_length != 0U) {
        (void)memcpy(slot->payload, request->payload, request->payload_length);
    }
    return UCN_OK;
}

ucn_result_t ucn_node_step(ucn_node_t *node, uint32_t now_ms)
{
    ucn_tx_item_t *item;
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!node->step_observation_started) {
        node->step_observation_started = true;
        node->stats.last_step_ms = now_ms;
    } else {
        const uint32_t gap_ms = now_ms - node->stats.last_step_ms;

        node->stats.last_step_ms = now_ms;
        if (gap_ms > node->stats.max_step_gap_ms) {
            node->stats.max_step_gap_ms = gap_ms;
        }
        if (gap_ms > UCN_MAX_STEP_INTERVAL_MS) {
            node->stats.step_interval_violations++;
        }
    }
    node->now_ms = now_ms;
    item = nano_select_queue_item(node->q0, UCN_TX_Q0_DEPTH);
    if (item == NULL) {
        item = nano_select_queue_item(node->q1, UCN_TX_Q1_DEPTH);
    }
    if (item == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (ucn_deadline_expired(now_ms, item->deadline_ms)) {
        if (item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
            item->backpressure_retries != 0U) {
            node->stats.q0_backpressure_expired++;
        }
        item->occupied = false;
        node->stats.tx_expired_dropped++;
        return UCN_ERR_TTL;
    }
    if (item->next_attempt_ms != 0U &&
        !ucn_deadline_expired(now_ms, item->next_attempt_ms)) {
        return UCN_ERR_NOT_FOUND;
    }
    {
        const uint32_t drops_before = node->stats.tx_error_dropped;

        result = ucn_node_send(node, item->destination, item->message_type,
                               item->traffic_class, item->payload,
                               item->payload_length);
        if (result == UCN_ERR_NO_SPACE &&
            item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
            item->backpressure_retries < UCN_Q0_BACKPRESSURE_MAX_RETRIES &&
            !ucn_deadline_due_within(
                now_ms, item->deadline_ms,
                UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS)) {
            node->stats.tx_error_dropped = drops_before;
            item->backpressure_retries++;
            item->next_attempt_ms = ucn_deadline_from_now(
                now_ms, UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS);
            node->stats.q0_backpressure_retries++;
            return result;
        }
    }
    item->occupied = false;
    if (result == UCN_ERR_NO_SPACE &&
        item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
        node->stats.q0_backpressure_exhausted++;
    } else if (result != UCN_OK &&
               item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
        node->stats.q0_backpressure_terminal_failed++;
    }
    return result;
}

const ucn_node_stats_t *ucn_node_get_stats(const ucn_node_t *node)
{
    return node == NULL ? NULL : &node->stats;
}

ucn_result_t ucn_node_receive(ucn_node_t *node,
                              ucn_link_t *ingress_link,
                              const uint8_t *data,
                              size_t length)
{
    ucn_frame_t frame;
    ucn_link_t *egress_link;
    ucn_wire_profile_t local_receive_profile;
    ucn_wire_profile_t incoming_profile;
    ucn_result_t result;
    bool delivered = false;

    if (node == NULL || ingress_link == NULL || data == NULL ||
        !nano_link_is_registered(node, ingress_link)) {
        return UCN_ERR_ARGUMENT;
    }
    result = nano_resolve_link_local_receive_profile(node, ingress_link,
                                                     &local_receive_profile);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_frame_peek_wire_profile(data, length, &incoming_profile);
    if (result != UCN_OK) {
        return result;
    }
    if (incoming_profile > local_receive_profile) {
        return UCN_ERR_UNSUPPORTED;
    }
    result = ucn_frame_decode(data, length, &frame);
    if (result != UCN_OK) {
        return result;
    }
    if (frame.network_id != node->config.network_id) {
        return UCN_ERR_NETWORK;
    }
    if (frame.hop_limit > node->config.default_hop_limit) {
        node->stats.hop_scope_rejected++;
        return UCN_ERR_TTL;
    }
    if (frame.source == 0U || frame.source == UCN_NODE_BROADCAST ||
        frame.source == node->config.node_id || frame.sequence == 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (ucn_message_type_is_control(frame.message_type) ||
        frame.has_route_extension || frame.has_path_id ||
        (frame.flags & (UCN_FRAME_FLAG_E2E_PROTECTED |
                        UCN_FRAME_FLAG_DIAGNOSTIC)) != 0U) {
        return UCN_ERR_CONFIG;
    }
    result = ucn_duplicate_accept_frame(node, &frame);
    if (result != UCN_OK) {
        return result;
    }
    if (frame.destination == node->config.node_id) {
        delivered = nano_dispatch_endpoint(node, &frame);
        if (!delivered && node->rx_handler != NULL) {
            node->rx_handler(node->rx_context, &frame);
            delivered = true;
        }
        if (!delivered) {
            return UCN_ERR_NOT_FOUND;
        }
        node->stats.rx_delivered++;
        return UCN_OK;
    }
    if (frame.destination == UCN_NODE_BROADCAST) {
        return UCN_ERR_CONFIG;
    }
    egress_link = nano_find_link(node, frame.destination);
    if (egress_link == NULL || egress_link == ingress_link) {
        return UCN_ERR_NOT_FOUND;
    }
    return nano_send_existing_frame(node, egress_link, &frame);
}
