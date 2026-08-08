#include <string.h>

#include "ucn/ucn.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

#define UCN_ROUTE_REQ_PAYLOAD_BYTES ((size_t)16U)
#define UCN_ROUTE_REPLY_PAYLOAD_BYTES ((size_t)18U)
#define UCN_ROUTE_ERROR_PAYLOAD_BYTES ((size_t)4U)
#define UCN_HELLO_PAYLOAD_BYTES ((size_t)4U)
#define UCN_HEARTBEAT_PAYLOAD_BYTES ((size_t)8U)
#define UCN_PATH_PROBE_PAYLOAD_BYTES ((size_t)12U)
#define UCN_PATH_ACTIVATE_PAYLOAD_BYTES ((size_t)6U)
#define UCN_PATH_TRACE_TRACE_ID_OFFSET ((size_t)0U)
#define UCN_PATH_TRACE_RECORD_COUNT_OFFSET ((size_t)4U)
#define UCN_PATH_TRACE_RECORD_LIMIT_OFFSET ((size_t)5U)
#define UCN_PATH_TRACE_STATUS_OFFSET ((size_t)6U)
#define UCN_PATH_TRACE_RESERVED_OFFSET ((size_t)7U)
#define UCN_PATH_TRACE_NODE_IDS_OFFSET UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES
#define UCN_PATH_TRACE_MAX_PAYLOAD_BYTES \
    (UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES + \
     UCN_PATH_TRACE_MAX_NODES * sizeof(ucn_node_id_t))
#define UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET ((size_t)0U)
#define UCN_NODE_SNAPSHOT_REQUEST_FLAGS_OFFSET ((size_t)4U)
#define UCN_NODE_SNAPSHOT_REPLY_NODE_ID_OFFSET ((size_t)4U)
#define UCN_NODE_SNAPSHOT_REPLY_NEIGHBOR_COUNT_OFFSET ((size_t)8U)
#define UCN_NODE_SNAPSHOT_REPLY_FLAGS_OFFSET ((size_t)9U)
#define UCN_HEARTBEAT_REQUEST ((uint8_t)1U)
#define UCN_HEARTBEAT_ACK ((uint8_t)2U)
#define UCN_ROUTE_REQ_FLAG_CANDIDATE ((uint8_t)0x01U)

static void invalidate_routes_by_link(ucn_node_t *node, const ucn_link_t *link);
static void remove_neighbor_by_link(ucn_node_t *node, ucn_link_t *link);
static ucn_result_t get_link_status(const ucn_link_t *link, ucn_link_status_t *status);
static ucn_result_t begin_route_discovery(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          uint32_t now_ms,
                                          bool is_candidate);
static void expire_candidate_routes(ucn_node_t *node);
static bool deadline_expired(uint32_t now_ms, uint32_t deadline_ms);

static uint32_t read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

static void write_u32_be(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static uint16_t read_u16_be(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | (uint16_t)data[1]);
}

static void write_u16_be(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}

static uint16_t add_route_cost(uint16_t left, uint16_t right)
{
    return left > (uint16_t)(UINT16_MAX - right) ? UINT16_MAX :
           (uint16_t)(left + right);
}

static uint16_t route_epoch_from_request_id(uint32_t request_id)
{
    uint16_t route_epoch = (uint16_t)(request_id ^ (request_id >> 16U));

    return route_epoch == 0U ? 1U : route_epoch;
}

static uint16_t link_route_cost(const ucn_link_t *link)
{
    ucn_link_metrics_t metrics;

    if (link != NULL && link->ops != NULL && link->ops->get_metrics != NULL &&
        link->ops->get_metrics(link, &metrics) == UCN_OK &&
        metrics.route_cost_valid && metrics.route_cost != 0U) {
        return metrics.route_cost;
    }
    return UCN_UNKNOWN_LINK_ROUTE_COST;
}

static bool route_is_expired(const ucn_node_t *node, const ucn_route_entry_t *route)
{
    return !route->is_static &&
           (int32_t)(node->now_ms - route->expires_at_ms) >= 0;
}

static bool candidate_is_expired(const ucn_node_t *node,
                                 const ucn_candidate_route_t *candidate)
{
    return (int32_t)(node->now_ms - candidate->expires_at_ms) >= 0;
}

static bool candidate_is_sufficiently_better(uint16_t active_cost,
                                             uint16_t candidate_cost)
{
    uint32_t candidate_scaled;
    uint32_t active_scaled;

    if (candidate_cost >= active_cost) {
        return false;
    }
    candidate_scaled = (uint32_t)candidate_cost * 100U;
    active_scaled = (uint32_t)active_cost *
                    (uint32_t)(100U - UCN_ROUTE_SWITCH_IMPROVEMENT_PERCENT);
    return candidate_scaled <= active_scaled;
}

static ucn_link_t *find_direct_link(const ucn_node_t *node,
                                    ucn_node_id_t destination)
{
    size_t index;
    ucn_link_t *best_direct = NULL;
    uint16_t best_direct_cost = UINT16_MAX;

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index]->peer_node_id == destination &&
            link_route_cost(node->links[index]) < best_direct_cost) {
            best_direct = node->links[index];
            best_direct_cost = link_route_cost(node->links[index]);
        }
    }

    return best_direct;
}

static ucn_link_t *find_link(const ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;
    ucn_link_t *direct = find_direct_link(node, destination);

    if (direct != NULL) {
        return direct;
    }

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            !route_is_expired(node, &node->routes[index]) &&
            node->routes[index].destination == destination) {
            return node->routes[index].egress_link;
        }
    }

    return NULL;
}

static ucn_link_t *find_link_for_route_epoch(const ucn_node_t *node,
                                             ucn_node_id_t destination,
                                             bool has_route_extension,
                                             uint16_t route_epoch)
{
    size_t index;
    ucn_link_t *direct = find_direct_link(node, destination);

    if (direct != NULL) {
        return direct;
    }
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        const ucn_route_entry_t *route = &node->routes[index];

        if (!route->valid || route_is_expired(node, route) ||
            route->destination != destination) {
            continue;
        }
        if (has_route_extension) {
            if (route->route_epoch == route_epoch) {
                return route->egress_link;
            }
            if (route->previous_valid &&
                !deadline_expired(node->now_ms, route->previous_expires_at_ms) &&
                route->previous_route_epoch == route_epoch) {
                return route->previous_egress_link;
            }
        } else if (route->route_epoch == 0U) {
            return route->egress_link;
        } else if (route->previous_valid && route->previous_route_epoch == 0U &&
                   !deadline_expired(node->now_ms, route->previous_expires_at_ms)) {
            return route->previous_egress_link;
        }
    }
    return NULL;
}

static bool route_epoch_is_accepted(const ucn_node_t *node,
                                    ucn_node_id_t source,
                                    const ucn_frame_t *frame)
{
    if (find_direct_link(node, source) != NULL) {
        return true;
    }
    return find_link_for_route_epoch(node, source, frame->has_route_extension,
                                     frame->route_epoch) != NULL;
}

static bool link_is_registered(const ucn_node_t *node, const ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index] == link) {
            return true;
        }
    }

    return false;
}

static ucn_neighbor_entry_t *find_neighbor(ucn_node_t *node,
                                            ucn_node_id_t peer_node_id)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state != UCN_NEIGHBOR_EMPTY &&
            node->neighbors[index].peer_node_id == peer_node_id) {
            return &node->neighbors[index];
        }
    }
    return NULL;
}

static ucn_neighbor_entry_t *find_neighbor_by_link(ucn_node_t *node,
                                                    const ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state != UCN_NEIGHBOR_EMPTY &&
            node->neighbors[index].link == link) {
            return &node->neighbors[index];
        }
    }
    return NULL;
}

/* Static Links have no Neighbor entry.  A dynamically admitted Link may
 * carry its existing active traffic during SUSPECT, but it must not be used
 * to construct or validate a new candidate path. */
static bool link_is_candidate_eligible(ucn_node_t *node, const ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);

    return entry == NULL || entry->state == UCN_NEIGHBOR_ADMITTED;
}

static ucn_endpoint_handler_entry_t *find_endpoint_handler(ucn_node_t *node,
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

static bool security_policy_is_valid(const ucn_security_policy_t *policy)
{
    return policy != NULL && policy->tx_mode <= UCN_SECURITY_TX_AUTO &&
           policy->rx_mode <= UCN_SECURITY_RX_BOTH &&
           policy->forward_mode <= UCN_SECURITY_FORWARD_TERMINAL_ONLY;
}

static ucn_endpoint_security_policy_entry_t *find_endpoint_security_policy(
    ucn_node_t *node,
    ucn_endpoint_t endpoint)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ENDPOINT_SECURITY_POLICIES; ++index) {
        if (node->endpoint_security_policies[index].occupied &&
            node->endpoint_security_policies[index].endpoint == endpoint) {
            return &node->endpoint_security_policies[index];
        }
    }
    return NULL;
}

static const ucn_security_policy_t *resolve_security_policy(ucn_node_t *node,
                                                             uint8_t message_type)
{
    ucn_endpoint_security_policy_entry_t *entry;

    if (!ucn_endpoint_is_static((ucn_endpoint_t)message_type)) {
        return &node->security_policy;
    }
    entry = find_endpoint_security_policy(node, (ucn_endpoint_t)message_type);
    return entry == NULL ? &node->security_policy : &entry->policy;
}

static bool dispatch_endpoint(ucn_node_t *node, const ucn_frame_t *frame)
{
    ucn_endpoint_handler_entry_t *entry;

    if (!ucn_endpoint_is_static((ucn_endpoint_t)frame->message_type)) {
        return false;
    }
    entry = find_endpoint_handler(node, (ucn_endpoint_t)frame->message_type);
    if (entry == NULL || entry->handler == NULL) {
        return false;
    }
    entry->handler(entry->context, frame);
    return true;
}

static bool frame_is_seen(const ucn_node_t *node, const ucn_frame_t *frame)
{
    size_t index;

    for (index = 0U; index < UCN_SEEN_CACHE_SIZE; ++index) {
        if (node->seen[index].valid && node->seen[index].source == frame->source &&
            node->seen[index].sequence == frame->sequence) {
            return true;
        }
    }

    return false;
}

static void remember_frame(ucn_node_t *node, const ucn_frame_t *frame)
{
    ucn_seen_frame_t *slot = &node->seen[node->next_seen_index];

    slot->valid = true;
    slot->source = frame->source;
    slot->sequence = frame->sequence;
    slot->best_route_request_cost = UINT16_MAX;
    node->next_seen_index = (node->next_seen_index + 1U) % UCN_SEEN_CACHE_SIZE;
}

static bool remember_better_route_request(ucn_node_t *node,
                                          const ucn_frame_t *frame,
                                          uint16_t route_cost)
{
    size_t index;

    for (index = 0U; index < UCN_SEEN_CACHE_SIZE; ++index) {
        if (node->seen[index].valid && node->seen[index].source == frame->source &&
            node->seen[index].sequence == frame->sequence) {
            if (route_cost >= node->seen[index].best_route_request_cost) {
                return false;
            }
            node->seen[index].best_route_request_cost = route_cost;
            return true;
        }
    }

    node->seen[node->next_seen_index].valid = true;
    node->seen[node->next_seen_index].source = frame->source;
    node->seen[node->next_seen_index].sequence = frame->sequence;
    node->seen[node->next_seen_index].best_route_request_cost = route_cost;
    node->next_seen_index = (node->next_seen_index + 1U) % UCN_SEEN_CACHE_SIZE;
    return true;
}

static bool deadline_expired(uint32_t now_ms, uint32_t deadline_ms)
{
    return deadline_ms != 0U && (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool take_control_token(ucn_node_t *node)
{
    uint32_t elapsed = node->now_ms - node->control_last_refill_ms;
    uint32_t refill_count = elapsed / UCN_CONTROL_TOKEN_REFILL_MS;

    if (refill_count != 0U) {
        uint32_t new_tokens = (uint32_t)node->control_tokens + refill_count;

        node->control_tokens = (uint8_t)(new_tokens > UCN_CONTROL_TOKEN_BURST ?
                                         UCN_CONTROL_TOKEN_BURST : new_tokens);
        node->control_last_refill_ms += refill_count * UCN_CONTROL_TOKEN_REFILL_MS;
    }
    if (node->control_tokens == 0U) {
        node->stats.control_budget_dropped++;
        return false;
    }
    --node->control_tokens;
    return true;
}

/* Diagnostic traffic has an independent, much smaller budget.  A manual
 * topology query can therefore never consume the Q0 control budget used by
 * join, liveness, and route recovery. */
static bool take_path_trace_token(ucn_node_t *node)
{
    uint32_t elapsed = node->now_ms - node->path_trace_last_refill_ms;
    uint32_t refill_count = elapsed / UCN_PATH_TRACE_TOKEN_REFILL_MS;

    if (refill_count != 0U) {
        uint32_t new_tokens = (uint32_t)node->path_trace_tokens + refill_count;

        node->path_trace_tokens =
            (uint8_t)(new_tokens > UCN_PATH_TRACE_TOKEN_BURST ?
                          UCN_PATH_TRACE_TOKEN_BURST : new_tokens);
        node->path_trace_last_refill_ms +=
            refill_count * UCN_PATH_TRACE_TOKEN_REFILL_MS;
    }
    if (node->path_trace_tokens == 0U) {
        node->stats.path_trace_rate_dropped++;
        return false;
    }
    --node->path_trace_tokens;
    return true;
}

/* A snapshot touches the whole reachable component, unlike a single-path
 * trace.  It therefore has its own, slower token bucket. */
static bool take_node_snapshot_token(ucn_node_t *node)
{
    uint32_t elapsed = node->now_ms - node->node_snapshot_last_refill_ms;
    uint32_t refill_count = elapsed / UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS;

    if (refill_count != 0U) {
        uint32_t new_tokens = (uint32_t)node->node_snapshot_tokens + refill_count;

        node->node_snapshot_tokens =
            (uint8_t)(new_tokens > UCN_NODE_SNAPSHOT_TOKEN_BURST ?
                          UCN_NODE_SNAPSHOT_TOKEN_BURST : new_tokens);
        node->node_snapshot_last_refill_ms +=
            refill_count * UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS;
    }
    if (node->node_snapshot_tokens == 0U) {
        node->stats.node_snapshot_rate_dropped++;
        return false;
    }
    --node->node_snapshot_tokens;
    return true;
}

static void expire_neighbor_candidates(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state == UCN_NEIGHBOR_CANDIDATE &&
            deadline_expired(now_ms, node->neighbors[index].last_seen_ms +
                              UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS)) {
            if (node->neighbors[index].link != NULL &&
                !link_is_registered(node, node->neighbors[index].link)) {
                node->neighbors[index].link->peer_node_id = 0U;
            }
            node->neighbors[index].state = UCN_NEIGHBOR_EXPIRED;
        }
    }
}

static ucn_neighbor_entry_t *allocate_neighbor_slot(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state == UCN_NEIGHBOR_EMPTY ||
            node->neighbors[index].state == UCN_NEIGHBOR_REMOVED ||
            node->neighbors[index].state == UCN_NEIGHBOR_REJECTED ||
            node->neighbors[index].state == UCN_NEIGHBOR_EXPIRED) {
            return &node->neighbors[index];
        }
    }
    return NULL;
}

static ucn_result_t admit_neighbor_entry(ucn_node_t *node,
                                         ucn_neighbor_entry_t *entry)
{
    ucn_result_t result;

    if (entry == NULL || entry->state != UCN_NEIGHBOR_CANDIDATE) {
        return UCN_ERR_NOT_FOUND;
    }
    if (link_is_registered(node, entry->link)) {
        entry->state = UCN_NEIGHBOR_ADMITTED;
        entry->suspect_since_ms = 0U;
        return UCN_OK;
    }

    result = ucn_node_register_link(node, entry->link);
    if (result == UCN_OK) {
        entry->state = UCN_NEIGHBOR_ADMITTED;
        entry->suspect_since_ms = 0U;
    }
    return result;
}

static void clear_discovery(ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination) {
            node->discoveries[index].active = false;
        }
    }
    expire_candidate_routes(node);
}

static void expire_dynamic_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    node->now_ms = now_ms;
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && route_is_expired(node, &node->routes[index])) {
            node->routes[index].valid = false;
        } else if (node->routes[index].previous_valid &&
                   deadline_expired(now_ms,
                                    node->routes[index].previous_expires_at_ms)) {
            node->routes[index].previous_valid = false;
            node->routes[index].previous_egress_link = NULL;
            node->routes[index].previous_route_epoch = 0U;
            node->routes[index].previous_expires_at_ms = 0U;
        }
    }

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            deadline_expired(now_ms, node->discoveries[index].deadline_ms)) {
            node->discoveries[index].active = false;
        }
    }
}

static ucn_result_t learn_route(ucn_node_t *node,
                                ucn_node_id_t destination,
                                ucn_link_t *egress_link,
                                uint16_t route_cost,
                                uint8_t hop_count,
                                uint16_t route_epoch)
{
    size_t index;
    ucn_route_entry_t *free_slot = NULL;

    if (destination == 0U || destination == UCN_NODE_BROADCAST || route_epoch == 0U ||
        egress_link == NULL || !link_is_registered(node, egress_link)) {
        return UCN_ERR_ARGUMENT;
    }

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination) {
            if (!node->routes[index].is_static) {
                if (route_cost < node->routes[index].route_cost) {
                    node->routes[index].egress_link = egress_link;
                    node->routes[index].route_cost = route_cost;
                    node->routes[index].hop_count = hop_count;
                    node->routes[index].route_epoch = route_epoch;
                }
                node->routes[index].expires_at_ms =
                    node->now_ms + UCN_ROUTE_ENTRY_LIFETIME_MS;
                node->routes[index].last_refresh_started_ms = node->now_ms;
            }
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
    free_slot->is_static = false;
    free_slot->destination = destination;
    free_slot->egress_link = egress_link;
    free_slot->expires_at_ms = node->now_ms + UCN_ROUTE_ENTRY_LIFETIME_MS;
    free_slot->last_used_at_ms = 0U;
    free_slot->last_refresh_started_ms = node->now_ms;
    free_slot->route_cost = route_cost;
    free_slot->hop_count = hop_count;
    free_slot->route_epoch = route_epoch;
    free_slot->previous_valid = false;
    free_slot->previous_egress_link = NULL;
    free_slot->previous_route_epoch = 0U;
    free_slot->previous_expires_at_ms = 0U;
    return UCN_OK;
}

static ucn_route_entry_t *find_active_route(ucn_node_t *node,
                                            ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination &&
            !route_is_expired(node, &node->routes[index])) {
            return &node->routes[index];
        }
    }
    return NULL;
}

static ucn_candidate_route_t *find_candidate_route(ucn_node_t *node,
                                                    ucn_node_id_t destination,
                                                    uint32_t candidate_id)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].destination == destination &&
            node->candidates[index].candidate_id == candidate_id &&
            !candidate_is_expired(node, &node->candidates[index])) {
            return &node->candidates[index];
        }
    }
    return NULL;
}

static ucn_result_t learn_candidate_route(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          uint32_t candidate_id,
                                          ucn_link_t *egress_link,
                                          uint16_t route_cost,
                                          uint8_t hop_count,
                                          bool originated_here)
{
    ucn_candidate_route_t *slot;
    size_t index;

    if (destination == 0U || destination == UCN_NODE_BROADCAST ||
        candidate_id == 0U || egress_link == NULL ||
        !link_is_registered(node, egress_link)) {
        return UCN_ERR_ARGUMENT;
    }
    slot = find_candidate_route(node, destination, candidate_id);
    if (slot != NULL) {
        if (route_cost < slot->route_cost) {
            slot->egress_link = egress_link;
            slot->route_cost = route_cost;
            slot->hop_count = hop_count;
        }
        slot->originated_here = slot->originated_here || originated_here;
        slot->expires_at_ms = node->now_ms + UCN_ROUTE_CANDIDATE_TIMEOUT_MS;
        return UCN_OK;
    }

    slot = NULL;
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (!node->candidates[index].valid ||
            candidate_is_expired(node, &node->candidates[index])) {
            slot = &node->candidates[index];
            break;
        }
    }
    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    (void)memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->originated_here = originated_here;
    slot->destination = destination;
    slot->candidate_id = candidate_id;
    slot->egress_link = egress_link;
    slot->expires_at_ms = node->now_ms + UCN_ROUTE_CANDIDATE_TIMEOUT_MS;
    slot->route_cost = route_cost;
    slot->hop_count = hop_count;
    node->stats.candidate_routes_learned++;
    return UCN_OK;
}

static ucn_result_t activate_candidate_route(ucn_node_t *node,
                                             ucn_node_id_t destination,
                                             uint32_t candidate_id,
                                             uint16_t route_epoch)
{
    ucn_candidate_route_t *candidate;
    ucn_route_entry_t *route;
    size_t index;

    candidate = find_candidate_route(node, destination, candidate_id);
    if (candidate == NULL || route_epoch == 0U) {
        return UCN_ERR_NOT_FOUND;
    }
    route = find_active_route(node, destination);
    if (route != NULL && route->is_static) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (route == NULL) {
        for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
            if (!node->routes[index].valid ||
                route_is_expired(node, &node->routes[index])) {
                route = &node->routes[index];
                break;
            }
        }
    }
    if (route == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    if (route->valid) {
        route->previous_valid = true;
        route->previous_egress_link = route->egress_link;
        route->previous_route_epoch = route->route_epoch;
        route->previous_expires_at_ms = node->now_ms + UCN_ROUTE_EPOCH_GRACE_MS;
    } else {
        route->previous_valid = false;
        route->previous_egress_link = NULL;
        route->previous_route_epoch = 0U;
        route->previous_expires_at_ms = 0U;
    }
    route->valid = true;
    route->is_static = false;
    route->destination = destination;
    route->egress_link = candidate->egress_link;
    route->expires_at_ms = node->now_ms + UCN_ROUTE_ENTRY_LIFETIME_MS;
    route->last_used_at_ms = 0U;
    route->last_refresh_started_ms = node->now_ms;
    route->route_cost = candidate->route_cost;
    route->hop_count = candidate->hop_count;
    route->route_epoch = route_epoch;
    candidate->valid = false;
    return UCN_OK;
}

static void expire_candidate_routes(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            candidate_is_expired(node, &node->candidates[index])) {
            node->candidates[index].valid = false;
        }
    }
}

static void mark_route_used(ucn_node_t *node, ucn_node_id_t destination)
{
    ucn_route_entry_t *route = find_active_route(node, destination);

    if (route != NULL && !route->is_static) {
        route->last_used_at_ms = node->now_ms;
    }
}

static ucn_result_t start_due_route_refresh(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        ucn_route_entry_t *route = &node->routes[index];

        if (!route->valid || route->is_static || route_is_expired(node, route) ||
            (uint32_t)(now_ms - route->last_used_at_ms) >
            UCN_ROUTE_ENTRY_LIFETIME_MS ||
            (uint32_t)(now_ms - route->last_refresh_started_ms) <
            UCN_ROUTE_REFRESH_MIN_INTERVAL_MS ||
            (int32_t)(route->expires_at_ms - now_ms) >
            (int32_t)UCN_ROUTE_REFRESH_ADVANCE_MS) {
            continue;
        }
        return begin_route_discovery(node, route->destination, now_ms, true);
    }
    return UCN_ERR_NOT_FOUND;
}

static void invalidate_routes_by_link(ucn_node_t *node, const ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && !node->routes[index].is_static &&
            node->routes[index].egress_link == link) {
            node->routes[index].valid = false;
            node->routes[index].previous_valid = false;
        } else if (node->routes[index].previous_valid &&
                   node->routes[index].previous_egress_link == link) {
            node->routes[index].previous_valid = false;
            node->routes[index].previous_egress_link = NULL;
        }
    }
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].egress_link == link) {
            node->candidates[index].valid = false;
        }
    }
}

static void unregister_link(ucn_node_t *node, ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index] == link) {
            size_t move_index;

            if (link->ops != NULL && link->ops->close != NULL) {
                link->ops->close(link);
            }
            for (move_index = index + 1U; move_index < node->link_count; ++move_index) {
                node->links[move_index - 1U] = node->links[move_index];
            }
            --node->link_count;
            node->links[node->link_count] = NULL;
            return;
        }
    }
}

static void remove_neighbor_entry(ucn_node_t *node, ucn_neighbor_entry_t *entry)
{
    ucn_link_t *link;

    if (entry == NULL || entry->state == UCN_NEIGHBOR_EMPTY ||
        entry->state == UCN_NEIGHBOR_REMOVED) {
        return;
    }

    link = entry->link;
    if (link != NULL) {
        invalidate_routes_by_link(node, link);
        unregister_link(node, link);
        link->peer_node_id = 0U;
    }
    (void)memset(entry, 0, sizeof(*entry));
    entry->state = UCN_NEIGHBOR_REMOVED;
    node->stats.neighbor_removed++;
}

static void remove_neighbor_by_link(ucn_node_t *node, ucn_link_t *link)
{
    remove_neighbor_entry(node, find_neighbor_by_link(node, link));
}

static void touch_neighbor(ucn_node_t *node, ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);

    if (entry != NULL && (entry->state == UCN_NEIGHBOR_ADMITTED ||
                          entry->state == UCN_NEIGHBOR_SUSPECT)) {
        entry->last_seen_ms = node->now_ms;
        if (entry->state == UCN_NEIGHBOR_SUSPECT) {
            entry->state = UCN_NEIGHBOR_ADMITTED;
            entry->suspect_since_ms = 0U;
        }
    }
}

static void maintain_neighbor_liveness(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        ucn_link_status_t status;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        if (get_link_status(entry->link, &status) != UCN_OK || !status.is_up) {
            remove_neighbor_entry(node, entry);
            continue;
        }
        if (entry->state == UCN_NEIGHBOR_ADMITTED &&
            deadline_expired(now_ms, entry->last_seen_ms +
                             UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS)) {
            entry->state = UCN_NEIGHBOR_SUSPECT;
            entry->suspect_since_ms = now_ms;
            node->stats.neighbor_suspected++;
        }
        if (entry->state == UCN_NEIGHBOR_SUSPECT &&
            deadline_expired(now_ms, entry->last_seen_ms +
                             UCN_NEIGHBOR_REMOVE_TIMEOUT_MS)) {
            remove_neighbor_entry(node, entry);
        }
    }
}

static ucn_result_t allocate_sequence(ucn_node_t *node, ucn_sequence_t *sequence)
{
    ucn_sequence_t next_sequence;
    ucn_result_t result;

    if (node->next_sequence == 0U || node->next_sequence == UINT32_MAX) {
        return UCN_ERR_SECURITY;
    }

    *sequence = node->next_sequence;
    next_sequence = node->next_sequence + 1U;
    if (node->security_ops != NULL) {
        result = node->security_ops->store_next_sequence(node->security_context,
                                                         next_sequence);
        if (result != UCN_OK) {
            return result;
        }
    }
    node->next_sequence = next_sequence;
    return UCN_OK;
}

static ucn_tx_item_t *queue_items(ucn_node_t *node,
                                  ucn_traffic_class_t traffic_class,
                                  size_t *count)
{
    if (traffic_class == UCN_TRAFFIC_Q0_CRITICAL) {
        *count = UCN_TX_Q0_DEPTH;
        return node->q0;
    }

    *count = UCN_TX_Q1_DEPTH;
    return node->q1;
}

static ucn_tx_item_t *find_next_item(ucn_tx_item_t *items, size_t count)
{
    size_t index;
    ucn_tx_item_t *oldest = NULL;

    for (index = 0U; index < count; ++index) {
        if (items[index].occupied &&
            (oldest == NULL || (int32_t)(items[index].order - oldest->order) < 0)) {
            oldest = &items[index];
        }
    }

    return oldest;
}

static ucn_result_t queue_pending_q1(ucn_node_t *node,
                                     ucn_node_id_t destination,
                                     uint8_t message_type,
                                     const uint8_t *payload,
                                     uint16_t payload_length)
{
    ucn_pending_q1_item_t *slot = NULL;
    size_t index;

    if (payload_length > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    for (index = 0U; index < UCN_PENDING_Q1_DEPTH; ++index) {
        if (node->pending_q1[index].occupied &&
            node->pending_q1[index].destination == destination &&
            node->pending_q1[index].message_type == message_type) {
            slot = &node->pending_q1[index];
            break;
        }
        if (!node->pending_q1[index].occupied && slot == NULL) {
            slot = &node->pending_q1[index];
        }
    }
    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    slot->occupied = true;
    slot->destination = destination;
    slot->message_type = message_type;
    slot->deadline_ms = node->now_ms + UCN_PENDING_Q1_TIMEOUT_MS;
    slot->payload_length = payload_length;
    if (payload_length != 0U) {
        (void)memcpy(slot->payload, payload, payload_length);
    }
    node->stats.q1_route_wait_queued++;
    return UCN_OK;
}

static ucn_result_t send_pending_q1_if_ready(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_PENDING_Q1_DEPTH; ++index) {
        ucn_pending_q1_item_t *item = &node->pending_q1[index];

        if (!item->occupied) {
            continue;
        }
        if (deadline_expired(now_ms, item->deadline_ms)) {
            item->occupied = false;
            node->stats.q1_route_wait_expired++;
            return UCN_ERR_TTL;
        }
        if (find_link(node, item->destination) != NULL) {
            ucn_result_t result;

            item->occupied = false;
            result = ucn_node_send(node, item->destination, item->message_type,
                                   UCN_TRAFFIC_Q1_REALTIME, item->payload,
                                   item->payload_length);
            return result;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

static ucn_result_t get_link_status(const ucn_link_t *link, ucn_link_status_t *status)
{
    if (link == NULL || link->ops == NULL || link->ops->send == NULL ||
        link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    return link->ops->get_status(link, status);
}

static ucn_result_t send_frame_on_link(ucn_node_t *node,
                                       ucn_link_t *link,
                                       const ucn_frame_t *frame)
{
    ucn_link_status_t status;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_result_t result;

    if (node->security_ops != NULL) {
        result = node->security_ops->authorize_tx(node->security_context, frame);
        if (result != UCN_OK) {
            return result;
        }
    }

    result = get_link_status(link, &status);
    if (result != UCN_OK) {
        return result;
    }
    if (!status.is_up) {
        remove_neighbor_by_link(node, link);
        return UCN_ERR_LINK_DOWN;
    }

    result = ucn_frame_encode(frame, encoded, sizeof(encoded), &encoded_length);
    if (result != UCN_OK) {
        return result;
    }
    if (encoded_length > link->mtu || encoded_length > status.mtu) {
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

static ucn_result_t protect_outbound_business(ucn_node_t *node,
                                              ucn_frame_t *frame,
                                              uint8_t *ciphertext,
                                              uint8_t auth_tag[UCN_E2E_TAG_SIZE])
{
    const ucn_security_policy_t *policy;
    bool protected_frame = false;
    ucn_result_t result;

    if (ucn_message_type_is_control(frame->message_type)) {
        return (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U ?
               UCN_ERR_MALFORMED : UCN_OK;
    }
    policy = resolve_security_policy(node, frame->message_type);
    if (policy->tx_mode == UCN_SECURITY_TX_E2E_PROTECTED) {
        protected_frame = true;
    } else if (policy->tx_mode == UCN_SECURITY_TX_AUTO &&
               node->security_ops != NULL &&
               node->security_ops->select_tx_protection != NULL) {
        result = node->security_ops->select_tx_protection(node->security_context,
                                                          frame, &protected_frame);
        if (result != UCN_OK) {
            return result;
        }
    }
    if (!protected_frame) {
        return UCN_OK;
    }
    if (node->security_ops == NULL || node->security_ops->seal == NULL ||
        node->security_ops->open == NULL) {
        return UCN_ERR_SECURITY;
    }

    frame->flags |= UCN_FRAME_FLAG_E2E_PROTECTED;
    result = node->security_ops->seal(node->security_context, frame, frame->payload,
                                      frame->payload_length, ciphertext, auth_tag);
    if (result != UCN_OK) {
        return result;
    }
    frame->payload = ciphertext;
    frame->auth_tag = auth_tag;
    return UCN_OK;
}

static ucn_result_t validate_inbound_business_security(
    ucn_node_t *node,
    ucn_link_t *ingress_link,
    ucn_frame_t *frame,
    uint8_t *plaintext)
{
    const ucn_security_policy_t *policy;
    bool protected_frame = (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U;

    if (ucn_message_type_is_control(frame->message_type)) {
        return protected_frame ? UCN_ERR_MALFORMED : UCN_OK;
    }
    if (frame->destination == UCN_NODE_BROADCAST) {
        return protected_frame ? UCN_ERR_UNSUPPORTED : UCN_OK;
    }

    policy = resolve_security_policy(node, frame->message_type);
    if (frame->destination != node->config.node_id) {
        if (policy->forward_mode == UCN_SECURITY_FORWARD_TERMINAL_ONLY ||
            (policy->forward_mode == UCN_SECURITY_FORWARD_OPAQUE_E2E_ONLY &&
             !protected_frame)) {
            return UCN_ERR_ACCESS;
        }
        return UCN_OK;
    }

    if ((policy->rx_mode == UCN_SECURITY_RX_PLAIN_ONLY && protected_frame) ||
        (policy->rx_mode == UCN_SECURITY_RX_ENCRYPTED_ONLY && !protected_frame)) {
        return UCN_ERR_ACCESS;
    }
    if (!protected_frame) {
        return UCN_OK;
    }
    if (node->security_ops == NULL || node->security_ops->open == NULL) {
        return UCN_ERR_SECURITY;
    }
    if (frame->auth_tag == NULL) {
        return UCN_ERR_MALFORMED;
    }
    {
        ucn_result_t result = node->security_ops->open(node->security_context,
                                                        ingress_link, frame,
                                                        frame->payload,
                                                        frame->payload_length,
                                                        frame->auth_tag,
                                                        plaintext);
        if (result != UCN_OK) {
            return result;
        }
    }
    frame->payload = plaintext;
    return UCN_OK;
}

static ucn_result_t send_control_on_link(ucn_node_t *node,
                                         ucn_link_t *link,
                                         ucn_node_id_t destination,
                                         uint8_t message_type,
                                         const uint8_t *payload,
                                         uint16_t payload_length)
{
    ucn_frame_t frame;
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = (message_type == UCN_MSG_HELLO ||
                       message_type == UCN_MSG_HEARTBEAT) ? 1U :
                      node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = send_frame_on_link(node, link, &frame);
    if (result == UCN_OK) {
        mark_route_used(node, destination);
    }
    return result;
}

static bool path_trace_status_is_wire_valid(uint8_t status)
{
    return status <= (uint8_t)UCN_PATH_TRACE_STATUS_TRUNCATED;
}

static bool path_trace_payload_is_valid(const ucn_frame_t *frame)
{
    uint8_t record_count;
    uint8_t record_limit;
    size_t expected_length;
    size_t index;

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length < UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES ||
        frame->payload_length > UCN_PATH_TRACE_MAX_PAYLOAD_BYTES ||
        (frame->flags & UCN_FRAME_FLAG_DIAGNOSTIC) == 0U ||
        frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_PATH_TRACE_TRACE_ID_OFFSET) == 0U) {
        return false;
    }
    record_count = frame->payload[UCN_PATH_TRACE_RECORD_COUNT_OFFSET];
    record_limit = frame->payload[UCN_PATH_TRACE_RECORD_LIMIT_OFFSET];
    expected_length = UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES +
                      (size_t)record_count * sizeof(ucn_node_id_t);
    if (record_count == 0U || record_limit == 0U ||
        record_count > record_limit ||
        record_count > UCN_PATH_TRACE_MAX_NODES ||
        record_limit > UCN_PATH_TRACE_MAX_NODES ||
        frame->payload[UCN_PATH_TRACE_RESERVED_OFFSET] != 0U ||
        !path_trace_status_is_wire_valid(frame->payload[UCN_PATH_TRACE_STATUS_OFFSET]) ||
        frame->payload_length != expected_length) {
        return false;
    }
    for (index = 0U; index < record_count; ++index) {
        ucn_node_id_t node_id = read_u32_be(frame->payload +
                                             UCN_PATH_TRACE_NODE_IDS_OFFSET +
                                             index * sizeof(ucn_node_id_t));

        if (node_id == 0U || node_id == UCN_NODE_BROADCAST) {
            return false;
        }
    }
    return true;
}

static bool append_path_trace_node(const ucn_frame_t *frame,
                                   ucn_node_id_t node_id,
                                   uint8_t *output,
                                   uint16_t *output_length)
{
    uint8_t record_count;
    uint8_t record_limit;
    size_t node_offset;

    if (!path_trace_payload_is_valid(frame) || node_id == 0U ||
        node_id == UCN_NODE_BROADCAST || output == NULL || output_length == NULL) {
        return false;
    }
    (void)memcpy(output, frame->payload, frame->payload_length);
    record_count = output[UCN_PATH_TRACE_RECORD_COUNT_OFFSET];
    record_limit = output[UCN_PATH_TRACE_RECORD_LIMIT_OFFSET];
    *output_length = frame->payload_length;
    if (record_count >= record_limit || record_count >= UCN_PATH_TRACE_MAX_NODES) {
        return false;
    }
    node_offset = UCN_PATH_TRACE_NODE_IDS_OFFSET +
                  (size_t)record_count * sizeof(ucn_node_id_t);
    write_u32_be(output + node_offset, node_id);
    output[UCN_PATH_TRACE_RECORD_COUNT_OFFSET] = (uint8_t)(record_count + 1U);
    *output_length = (uint16_t)(frame->payload_length + sizeof(ucn_node_id_t));
    return true;
}

static ucn_result_t send_path_trace_request_on_link(ucn_node_t *node,
                                                     ucn_link_t *link,
                                                     ucn_node_id_t destination,
                                                     const uint8_t *payload,
                                                     uint16_t payload_length)
{
    ucn_frame_t frame;
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_PATH_TRACE_REQ;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    return send_frame_on_link(node, link, &frame);
}

static ucn_result_t send_path_trace_reply_on_link(ucn_node_t *node,
                                                   ucn_link_t *link,
                                                   ucn_node_id_t destination,
                                                   const uint8_t *payload,
                                                   uint16_t payload_length)
{
    ucn_frame_t frame;
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_PATH_TRACE_REPLY;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    result = send_frame_on_link(node, link, &frame);
    if (result == UCN_OK) {
        node->stats.path_trace_replies_sent++;
    }
    return result;
}

static ucn_path_trace_pending_t *find_path_trace_pending(ucn_node_t *node,
                                                          uint32_t trace_id)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_PENDING_DEPTH; ++index) {
        if (node->path_trace_pending[index].occupied &&
            node->path_trace_pending[index].trace_id == trace_id) {
            return &node->path_trace_pending[index];
        }
    }
    return NULL;
}

static ucn_path_trace_pending_t *find_free_path_trace_pending(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_PENDING_DEPTH; ++index) {
        if (!node->path_trace_pending[index].occupied) {
            return &node->path_trace_pending[index];
        }
    }
    return NULL;
}

static ucn_path_trace_reverse_t *find_path_trace_reverse(ucn_node_t *node,
                                                          ucn_node_id_t origin,
                                                          uint32_t trace_id)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_REVERSE_DEPTH; ++index) {
        ucn_path_trace_reverse_t *entry = &node->path_trace_reverse[index];

        if (entry->occupied && deadline_expired(node->now_ms, entry->expires_at_ms)) {
            entry->occupied = false;
        }
        if (entry->occupied && entry->origin == origin &&
            entry->trace_id == trace_id) {
            return entry;
        }
    }
    return NULL;
}

static ucn_path_trace_reverse_t *allocate_path_trace_reverse(
    ucn_node_t *node,
    ucn_node_id_t origin,
    uint32_t trace_id,
    ucn_link_t *ingress_link)
{
    size_t index;
    ucn_path_trace_reverse_t *free_slot = NULL;
    ucn_path_trace_reverse_t *existing =
        find_path_trace_reverse(node, origin, trace_id);

    if (existing != NULL) {
        existing->ingress_link = ingress_link;
        existing->expires_at_ms = node->now_ms + UCN_PATH_TRACE_TIMEOUT_MS;
        return existing;
    }
    for (index = 0U; index < UCN_PATH_TRACE_REVERSE_DEPTH; ++index) {
        if (!node->path_trace_reverse[index].occupied) {
            free_slot = &node->path_trace_reverse[index];
            break;
        }
    }
    if (free_slot == NULL) {
        return NULL;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->origin = origin;
    free_slot->trace_id = trace_id;
    free_slot->ingress_link = ingress_link;
    free_slot->expires_at_ms = node->now_ms + UCN_PATH_TRACE_TIMEOUT_MS;
    return free_slot;
}

static void expire_path_trace_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_REVERSE_DEPTH; ++index) {
        if (node->path_trace_reverse[index].occupied &&
            deadline_expired(now_ms, node->path_trace_reverse[index].expires_at_ms)) {
            node->path_trace_reverse[index].occupied = false;
        }
    }
    for (index = 0U; index < UCN_PATH_TRACE_PENDING_DEPTH; ++index) {
        ucn_path_trace_pending_t *pending = &node->path_trace_pending[index];

        if (pending->occupied && deadline_expired(now_ms, pending->deadline_ms)) {
            ucn_path_trace_handler_t handler = pending->handler;
            void *context = pending->context;
            ucn_path_trace_result_t result;

            (void)memset(&result, 0, sizeof(result));
            result.status = UCN_PATH_TRACE_STATUS_TIMEOUT;
            result.trace_id = pending->trace_id;
            pending->occupied = false;
            node->stats.path_trace_timeouts++;
            if (handler != NULL) {
                handler(context, &result);
            }
        }
    }
}

static ucn_result_t complete_path_trace(ucn_node_t *node,
                                        const ucn_frame_t *frame)
{
    ucn_path_trace_pending_t *pending;
    ucn_path_trace_result_t result;
    uint32_t trace_id;
    uint8_t record_count;
    size_t index;

    if (!path_trace_payload_is_valid(frame)) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_MALFORMED;
    }
    trace_id = read_u32_be(frame->payload + UCN_PATH_TRACE_TRACE_ID_OFFSET);
    pending = find_path_trace_pending(node, trace_id);
    if (pending == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    record_count = frame->payload[UCN_PATH_TRACE_RECORD_COUNT_OFFSET];
    if (frame->payload[UCN_PATH_TRACE_STATUS_OFFSET] ==
            (uint8_t)UCN_PATH_TRACE_STATUS_OK &&
        (frame->source != pending->destination ||
         read_u32_be(frame->payload + UCN_PATH_TRACE_NODE_IDS_OFFSET +
                     ((size_t)record_count - 1U) * sizeof(ucn_node_id_t)) !=
             pending->destination)) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_MALFORMED;
    }

    (void)memset(&result, 0, sizeof(result));
    result.status = (ucn_path_trace_status_t)
        frame->payload[UCN_PATH_TRACE_STATUS_OFFSET];
    result.trace_id = trace_id;
    result.node_count = record_count;
    for (index = 0U; index < record_count; ++index) {
        result.node_ids[index] = read_u32_be(frame->payload +
                                              UCN_PATH_TRACE_NODE_IDS_OFFSET +
                                              index * sizeof(ucn_node_id_t));
    }
    {
        ucn_path_trace_handler_t handler = pending->handler;
        void *context = pending->context;

        pending->occupied = false;
        node->stats.path_trace_completed++;
        if (handler != NULL) {
            handler(context, &result);
        }
    }
    return UCN_OK;
}

static ucn_result_t handle_path_trace_request(ucn_node_t *node,
                                              ucn_link_t *ingress_link,
                                              const ucn_frame_t *frame)
{
    uint8_t payload[UCN_PATH_TRACE_MAX_PAYLOAD_BYTES];
    uint16_t payload_length;
    uint32_t trace_id;
    ucn_link_t *egress_link;
    ucn_result_t result;

    if (!path_trace_payload_is_valid(frame) ||
        frame->payload[UCN_PATH_TRACE_STATUS_OFFSET] !=
            (uint8_t)UCN_PATH_TRACE_STATUS_OK ||
        frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        frame->destination == 0U || frame->destination == UCN_NODE_BROADCAST ||
        read_u32_be(frame->payload + UCN_PATH_TRACE_NODE_IDS_OFFSET) != frame->source) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_MALFORMED;
    }
    trace_id = read_u32_be(frame->payload + UCN_PATH_TRACE_TRACE_ID_OFFSET);
    if (!append_path_trace_node(frame, node->config.node_id, payload,
                                &payload_length)) {
        (void)memcpy(payload, frame->payload, frame->payload_length);
        payload[UCN_PATH_TRACE_STATUS_OFFSET] =
            (uint8_t)UCN_PATH_TRACE_STATUS_TRUNCATED;
        node->stats.path_trace_rejected++;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, frame->payload_length);
    }

    if (frame->destination == node->config.node_id) {
        payload[UCN_PATH_TRACE_STATUS_OFFSET] = (uint8_t)UCN_PATH_TRACE_STATUS_OK;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, payload_length);
    }
    if (frame->hop_limit <= 1U) {
        payload[UCN_PATH_TRACE_STATUS_OFFSET] =
            (uint8_t)UCN_PATH_TRACE_STATUS_TTL_EXCEEDED;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, payload_length);
    }
    egress_link = find_link(node, frame->destination);
    if (egress_link == NULL || egress_link == ingress_link) {
        payload[UCN_PATH_TRACE_STATUS_OFFSET] =
            (uint8_t)UCN_PATH_TRACE_STATUS_NO_ROUTE;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, payload_length);
    }
    if (allocate_path_trace_reverse(node, frame->source, trace_id, ingress_link) == NULL) {
        payload[UCN_PATH_TRACE_STATUS_OFFSET] =
            (uint8_t)UCN_PATH_TRACE_STATUS_NO_ROUTE;
        node->stats.path_trace_rejected++;
        return send_path_trace_reply_on_link(node, ingress_link, frame->source,
                                             payload, payload_length);
    }

    {
        ucn_frame_t forwarded = *frame;

        forwarded.payload = payload;
        forwarded.payload_length = payload_length;
        --forwarded.hop_limit;
        result = send_frame_on_link(node, egress_link, &forwarded);
    }
    return result;
}

static ucn_result_t handle_path_trace_reply(ucn_node_t *node,
                                            ucn_link_t *ingress_link,
                                            const ucn_frame_t *frame)
{
    uint32_t trace_id;
    ucn_path_trace_reverse_t *reverse;
    ucn_result_t result;

    if (!path_trace_payload_is_valid(frame) || frame->source == 0U ||
        frame->source == UCN_NODE_BROADCAST || frame->destination == 0U ||
        frame->destination == UCN_NODE_BROADCAST) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination == node->config.node_id) {
        return complete_path_trace(node, frame);
    }
    trace_id = read_u32_be(frame->payload + UCN_PATH_TRACE_TRACE_ID_OFFSET);
    reverse = find_path_trace_reverse(node, frame->destination, trace_id);
    if (reverse == NULL || reverse->ingress_link == NULL ||
        reverse->ingress_link == ingress_link) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_NOT_FOUND;
    }
    if (frame->hop_limit <= 1U) {
        return UCN_ERR_TTL;
    }
    {
        ucn_frame_t forwarded = *frame;

        --forwarded.hop_limit;
        result = send_frame_on_link(node, reverse->ingress_link, &forwarded);
    }
    reverse->occupied = false;
    return result;
}

static bool node_snapshot_request_payload_is_valid(const ucn_frame_t *frame)
{
    size_t index;

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length != UCN_NODE_SNAPSHOT_REQUEST_PAYLOAD_BYTES ||
        (frame->flags & UCN_FRAME_FLAG_DIAGNOSTIC) == 0U ||
        frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET) == 0U) {
        return false;
    }
    for (index = UCN_NODE_SNAPSHOT_REQUEST_FLAGS_OFFSET;
         index < UCN_NODE_SNAPSHOT_REQUEST_PAYLOAD_BYTES; ++index) {
        if (frame->payload[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool node_snapshot_reply_payload_is_valid(const ucn_frame_t *frame)
{
    ucn_node_id_t reported_node_id;

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length != UCN_NODE_SNAPSHOT_REPLY_PAYLOAD_BYTES ||
        (frame->flags & UCN_FRAME_FLAG_DIAGNOSTIC) == 0U ||
        frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET) == 0U ||
        frame->payload[10U] != 0U || frame->payload[11U] != 0U) {
        return false;
    }
    reported_node_id = read_u32_be(frame->payload +
                                   UCN_NODE_SNAPSHOT_REPLY_NODE_ID_OFFSET);
    return reported_node_id != 0U && reported_node_id != UCN_NODE_BROADCAST &&
           reported_node_id == frame->source &&
           frame->payload[UCN_NODE_SNAPSHOT_REPLY_NEIGHBOR_COUNT_OFFSET] <=
               UCN_MAX_NEIGHBORS;
}

static ucn_result_t send_node_snapshot_reply_on_link(
    ucn_node_t *node,
    ucn_link_t *link,
    ucn_node_id_t destination,
    uint32_t query_id)
{
    ucn_frame_t frame;
    uint8_t payload[UCN_NODE_SNAPSHOT_REPLY_PAYLOAD_BYTES];
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    (void)memset(payload, 0, sizeof(payload));
    write_u32_be(payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET, query_id);
    write_u32_be(payload + UCN_NODE_SNAPSHOT_REPLY_NODE_ID_OFFSET,
                 node->config.node_id);
    payload[UCN_NODE_SNAPSHOT_REPLY_NEIGHBOR_COUNT_OFFSET] =
        (uint8_t)node->link_count;
    frame.message_type = UCN_MSG_NODE_SNAPSHOT_REPLY;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    result = send_frame_on_link(node, link, &frame);
    if (result == UCN_OK) {
        node->stats.node_snapshot_replies_sent++;
    }
    return result;
}

static ucn_node_snapshot_pending_t *find_node_snapshot_pending(
    ucn_node_t *node,
    uint32_t query_id)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_PENDING_DEPTH; ++index) {
        if (node->node_snapshot_pending[index].occupied &&
            node->node_snapshot_pending[index].query_id == query_id) {
            return &node->node_snapshot_pending[index];
        }
    }
    return NULL;
}

static ucn_node_snapshot_pending_t *find_free_node_snapshot_pending(
    ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_PENDING_DEPTH; ++index) {
        if (!node->node_snapshot_pending[index].occupied) {
            return &node->node_snapshot_pending[index];
        }
    }
    return NULL;
}

static ucn_node_snapshot_reverse_t *find_node_snapshot_reverse(
    ucn_node_t *node,
    ucn_node_id_t origin,
    uint32_t query_id)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_REVERSE_DEPTH; ++index) {
        ucn_node_snapshot_reverse_t *entry = &node->node_snapshot_reverse[index];

        if (entry->occupied && deadline_expired(node->now_ms, entry->expires_at_ms)) {
            entry->occupied = false;
        }
        if (entry->occupied && entry->origin == origin && entry->query_id == query_id) {
            return entry;
        }
    }
    return NULL;
}

static ucn_node_snapshot_reverse_t *allocate_node_snapshot_reverse(
    ucn_node_t *node,
    ucn_node_id_t origin,
    uint32_t query_id,
    ucn_link_t *ingress_link)
{
    size_t index;
    ucn_node_snapshot_reverse_t *free_slot = NULL;
    ucn_node_snapshot_reverse_t *existing =
        find_node_snapshot_reverse(node, origin, query_id);

    if (existing != NULL) {
        return existing;
    }
    for (index = 0U; index < UCN_NODE_SNAPSHOT_REVERSE_DEPTH; ++index) {
        if (!node->node_snapshot_reverse[index].occupied) {
            free_slot = &node->node_snapshot_reverse[index];
            break;
        }
    }
    if (free_slot == NULL) {
        return NULL;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->origin = origin;
    free_slot->query_id = query_id;
    free_slot->ingress_link = ingress_link;
    free_slot->expires_at_ms = node->now_ms + UCN_NODE_SNAPSHOT_TIMEOUT_MS;
    return free_slot;
}

static bool queue_node_snapshot_reply(ucn_node_t *node,
                                      ucn_node_id_t origin,
                                      uint32_t query_id,
                                      ucn_link_t *egress_link)
{
    size_t index;
    ucn_node_snapshot_reply_pending_t *free_slot = NULL;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH; ++index) {
        ucn_node_snapshot_reply_pending_t *entry = &node->node_snapshot_replies[index];

        if (entry->occupied && entry->origin == origin && entry->query_id == query_id) {
            return true;
        }
        if (!entry->occupied && free_slot == NULL) {
            free_slot = entry;
        }
    }
    if (free_slot == NULL) {
        return false;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->origin = origin;
    free_slot->query_id = query_id;
    free_slot->egress_link = egress_link;
    free_slot->due_at_ms = node->now_ms +
        ((query_id ^ node->config.node_id) %
         (UCN_NODE_SNAPSHOT_REPLY_JITTER_MS + UINT32_C(1)));
    free_slot->expires_at_ms = node->now_ms + UCN_NODE_SNAPSHOT_TIMEOUT_MS;
    return true;
}

static ucn_result_t forward_node_snapshot_request(ucn_node_t *node,
                                                   ucn_link_t *ingress_link,
                                                   const ucn_frame_t *frame)
{
    ucn_frame_t forwarded = *frame;
    size_t index;
    size_t sent_count = 0U;
    ucn_result_t last_error = UCN_ERR_NOT_FOUND;

    if (frame->hop_limit <= 1U) {
        return UCN_ERR_TTL;
    }
    --forwarded.hop_limit;
    for (index = 0U; index < node->link_count; ++index) {
        ucn_result_t result;

        if (node->links[index] == ingress_link ||
            !link_is_candidate_eligible(node, node->links[index])) {
            continue;
        }
        result = send_frame_on_link(node, node->links[index], &forwarded);
        if (result == UCN_OK) {
            ++sent_count;
        } else {
            last_error = result;
        }
    }
    return sent_count != 0U ? UCN_OK : last_error;
}

static void expire_node_snapshot_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_REVERSE_DEPTH; ++index) {
        if (node->node_snapshot_reverse[index].occupied &&
            deadline_expired(now_ms, node->node_snapshot_reverse[index].expires_at_ms)) {
            node->node_snapshot_reverse[index].occupied = false;
        }
    }
    for (index = 0U; index < UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH; ++index) {
        if (node->node_snapshot_replies[index].occupied &&
            deadline_expired(now_ms, node->node_snapshot_replies[index].expires_at_ms)) {
            node->node_snapshot_replies[index].occupied = false;
            node->stats.node_snapshot_rejected++;
        }
    }
    for (index = 0U; index < UCN_NODE_SNAPSHOT_PENDING_DEPTH; ++index) {
        ucn_node_snapshot_pending_t *pending = &node->node_snapshot_pending[index];

        if (pending->occupied && deadline_expired(now_ms, pending->deadline_ms)) {
            ucn_node_snapshot_result_t result;
            ucn_node_snapshot_handler_t handler = pending->handler;
            void *context = pending->context;

            (void)memset(&result, 0, sizeof(result));
            result.status = pending->truncated ? UCN_NODE_SNAPSHOT_STATUS_TRUNCATED :
                                                UCN_NODE_SNAPSHOT_STATUS_COMPLETE;
            result.query_id = pending->query_id;
            result.node_count = pending->node_count;
            if (pending->node_count != 0U) {
                (void)memcpy(result.entries, pending->entries,
                             (size_t)pending->node_count * sizeof(result.entries[0]));
            }
            pending->occupied = false;
            node->stats.node_snapshot_completed++;
            if (result.status == UCN_NODE_SNAPSHOT_STATUS_TRUNCATED) {
                node->stats.node_snapshot_result_truncated++;
            }
            if (handler != NULL) {
                handler(context, &result);
            }
        }
    }
}

static ucn_result_t send_due_node_snapshot_reply(ucn_node_t *node,
                                                  uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH; ++index) {
        ucn_node_snapshot_reply_pending_t *pending = &node->node_snapshot_replies[index];
        ucn_result_t result;

        if (!pending->occupied || !deadline_expired(now_ms, pending->due_at_ms)) {
            continue;
        }
        pending->occupied = false;
        result = send_node_snapshot_reply_on_link(node, pending->egress_link,
                                                  pending->origin, pending->query_id);
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

static ucn_result_t complete_node_snapshot_reply(ucn_node_t *node,
                                                  const ucn_frame_t *frame)
{
    uint32_t query_id;
    ucn_node_snapshot_pending_t *pending;
    size_t index;

    query_id = read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET);
    pending = find_node_snapshot_pending(node, query_id);
    if (pending == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    for (index = 0U; index < pending->node_count; ++index) {
        if (pending->entries[index].node_id == frame->source) {
            return UCN_OK;
        }
    }
    if (pending->node_count >= pending->result_limit) {
        pending->truncated = true;
        return UCN_OK;
    }
    pending->entries[pending->node_count].node_id = frame->source;
    pending->entries[pending->node_count].direct_link_count =
        frame->payload[UCN_NODE_SNAPSHOT_REPLY_NEIGHBOR_COUNT_OFFSET];
    pending->entries[pending->node_count].flags =
        frame->payload[UCN_NODE_SNAPSHOT_REPLY_FLAGS_OFFSET];
    ++pending->node_count;
    node->stats.node_snapshot_replies_received++;
    return UCN_OK;
}

static ucn_result_t handle_node_snapshot_request(ucn_node_t *node,
                                                  ucn_link_t *ingress_link,
                                                  const ucn_frame_t *frame)
{
    uint32_t query_id;
    ucn_result_t result;

    if (!node_snapshot_request_payload_is_valid(frame) ||
        frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        frame->destination != UCN_NODE_BROADCAST ||
        node->node_snapshot_authorize == NULL ||
        !node->node_snapshot_authorize(node->node_snapshot_authorize_context,
                                       frame->source)) {
        node->stats.node_snapshot_rejected++;
        return UCN_ERR_ACCESS;
    }
    if (!take_node_snapshot_token(node)) {
        return UCN_ERR_NO_SPACE;
    }
    query_id = read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET);
    if (allocate_node_snapshot_reverse(node, frame->source, query_id, ingress_link) == NULL) {
        node->stats.node_snapshot_rejected++;
        return UCN_ERR_NO_SPACE;
    }
    if (!queue_node_snapshot_reply(node, frame->source, query_id, ingress_link)) {
        node->stats.node_snapshot_rejected++;
    }
    node->stats.node_snapshot_requests_received++;
    result = forward_node_snapshot_request(node, ingress_link, frame);
    return result == UCN_ERR_NOT_FOUND || result == UCN_ERR_TTL ? UCN_OK : result;
}

static ucn_result_t handle_node_snapshot_reply(ucn_node_t *node,
                                                ucn_link_t *ingress_link,
                                                const ucn_frame_t *frame)
{
    uint32_t query_id;
    ucn_node_snapshot_reverse_t *reverse;
    ucn_result_t result;

    if (!node_snapshot_reply_payload_is_valid(frame) || frame->source == 0U ||
        frame->source == UCN_NODE_BROADCAST || frame->destination == 0U ||
        frame->destination == UCN_NODE_BROADCAST) {
        node->stats.node_snapshot_rejected++;
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination == node->config.node_id) {
        return complete_node_snapshot_reply(node, frame);
    }
    query_id = read_u32_be(frame->payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET);
    reverse = find_node_snapshot_reverse(node, frame->destination, query_id);
    if (reverse == NULL || reverse->ingress_link == NULL ||
        reverse->ingress_link == ingress_link) {
        node->stats.node_snapshot_rejected++;
        return UCN_ERR_NOT_FOUND;
    }
    if (frame->hop_limit <= 1U) {
        return UCN_ERR_TTL;
    }
    {
        ucn_frame_t forwarded = *frame;

        --forwarded.hop_limit;
        result = send_frame_on_link(node, reverse->ingress_link, &forwarded);
    }
    return result;
}

static ucn_result_t send_due_heartbeat(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        uint8_t payload[UCN_HEARTBEAT_PAYLOAD_BYTES];
        ucn_result_t result;

        if ((entry->state != UCN_NEIGHBOR_ADMITTED &&
             entry->state != UCN_NEIGHBOR_SUSPECT) || entry->link == NULL ||
            (entry->heartbeat_sent &&
             (uint32_t)(now_ms - entry->last_heartbeat_sent_ms) <
             UCN_HEARTBEAT_INTERVAL_MS)) {
            continue;
        }
        if (node->next_heartbeat_id == 0U) {
            node->next_heartbeat_id = 1U;
        }
        if (!take_control_token(node)) {
            return UCN_ERR_NO_SPACE;
        }
        (void)memset(payload, 0, sizeof(payload));
        payload[0] = UCN_HEARTBEAT_REQUEST;
        write_u32_be(payload + 4U, node->next_heartbeat_id++);
        result = send_control_on_link(node, entry->link, entry->peer_node_id,
                                      UCN_MSG_HEARTBEAT, payload,
                                      (uint16_t)sizeof(payload));
        if (result == UCN_OK) {
            entry->heartbeat_sent = true;
            entry->last_heartbeat_sent_ms = now_ms;
            node->stats.heartbeat_requests_sent++;
        }
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

static ucn_result_t handle_heartbeat(ucn_node_t *node,
                                     ucn_link_t *ingress_link,
                                     const ucn_frame_t *frame)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, ingress_link);
    uint8_t response[UCN_HEARTBEAT_PAYLOAD_BYTES];
    ucn_result_t result;

    if (entry == NULL || (entry->state != UCN_NEIGHBOR_ADMITTED &&
                          entry->state != UCN_NEIGHBOR_SUSPECT)) {
        return UCN_ERR_ACCESS;
    }
    if (frame->payload_length != UCN_HEARTBEAT_PAYLOAD_BYTES ||
        frame->destination != node->config.node_id || frame->hop_limit != 1U ||
        frame->source != entry->peer_node_id || frame->payload[1] != 0U ||
        frame->payload[2] != 0U || frame->payload[3] != 0U ||
        (frame->payload[0] != UCN_HEARTBEAT_REQUEST &&
         frame->payload[0] != UCN_HEARTBEAT_ACK)) {
        return UCN_ERR_MALFORMED;
    }

    touch_neighbor(node, ingress_link);
    node->stats.heartbeat_received++;
    if (frame->payload[0] == UCN_HEARTBEAT_ACK) {
        return UCN_OK;
    }

    (void)memcpy(response, frame->payload, sizeof(response));
    response[0] = UCN_HEARTBEAT_ACK;
    result = send_control_on_link(node, ingress_link, frame->source,
                                  UCN_MSG_HEARTBEAT, response,
                                  (uint16_t)sizeof(response));
    if (result == UCN_OK) {
        node->stats.heartbeat_acks_sent++;
    }
    return result;
}

static ucn_link_t *find_candidate_link(ucn_node_t *node,
                                       ucn_node_id_t destination,
                                       uint32_t candidate_id)
{
    ucn_candidate_route_t *candidate =
        find_candidate_route(node, destination, candidate_id);

    return candidate == NULL || !link_is_candidate_eligible(node,
                                                             candidate->egress_link) ?
           NULL : candidate->egress_link;
}

static uint16_t allocate_route_epoch(ucn_node_t *node, ucn_node_id_t destination)
{
    ucn_route_entry_t *route = find_active_route(node, destination);

    for (;;) {
        uint16_t route_epoch;

        if (node->next_route_epoch == 0U || node->next_route_epoch == UINT16_MAX) {
            node->next_route_epoch = 1U;
        }
        route_epoch = node->next_route_epoch++;
        if (route == NULL || (route_epoch != route->route_epoch &&
                              (!route->previous_valid ||
                               route_epoch != route->previous_route_epoch))) {
            return route_epoch;
        }
    }
}

static ucn_result_t send_due_path_probe(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        ucn_candidate_route_t *candidate = &node->candidates[index];
        uint8_t payload[UCN_PATH_PROBE_PAYLOAD_BYTES];
        ucn_result_t result;

        if (!candidate->valid || !candidate->originated_here) {
            continue;
        }
        if (candidate->probes_acked >= UCN_PATH_PROBE_REQUIRED_ACKS) {
            if (candidate->activation_sent) {
                continue;
            }
            if (candidate->route_epoch == 0U) {
                candidate->route_epoch = allocate_route_epoch(node,
                                                              candidate->destination);
            }
            if (!take_control_token(node)) {
                return UCN_ERR_NO_SPACE;
            }
            write_u32_be(payload, candidate->candidate_id);
            write_u16_be(payload + 4U, candidate->route_epoch);
            result = send_control_on_link(node, candidate->egress_link,
                                          candidate->destination,
                                          UCN_MSG_PATH_ACTIVATE, payload,
                                          UCN_PATH_ACTIVATE_PAYLOAD_BYTES);
            if (result == UCN_OK) {
                candidate->activation_sent = true;
            }
            return result;
        }
        if (candidate->probes_sent >= UCN_PATH_PROBE_REQUIRED_ACKS ||
            (candidate->next_probe_at_ms != 0U &&
             !deadline_expired(now_ms, candidate->next_probe_at_ms))) {
            continue;
        }

        if (!take_control_token(node)) {
            return UCN_ERR_NO_SPACE;
        }

        write_u32_be(payload, candidate->candidate_id);
        write_u32_be(payload + 4U, (uint32_t)candidate->probes_sent + 1U);
        write_u32_be(payload + 8U, now_ms);
        result = send_control_on_link(node, candidate->egress_link,
                                      candidate->destination, UCN_MSG_PATH_PROBE,
                                      payload, (uint16_t)sizeof(payload));
        if (result == UCN_OK) {
            candidate->probes_sent++;
            candidate->next_probe_at_ms = now_ms + UCN_PATH_PROBE_INTERVAL_MS;
            node->stats.path_probes_sent++;
        }
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

static ucn_result_t handle_path_probe(ucn_node_t *node,
                                      const ucn_frame_t *frame)
{
    uint32_t candidate_id;
    ucn_link_t *egress_link;

    if (frame->payload_length != UCN_PATH_PROBE_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    candidate_id = read_u32_be(frame->payload);
    if (candidate_id == 0U || read_u32_be(frame->payload + 4U) == 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination != node->config.node_id) {
        return UCN_OK;
    }
    egress_link = find_candidate_link(node, frame->source, candidate_id);
    if (egress_link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    return send_control_on_link(node, egress_link, frame->source,
                                UCN_MSG_PATH_PROBE_ACK, frame->payload,
                                frame->payload_length);
}

static ucn_result_t handle_path_probe_ack(ucn_node_t *node,
                                          const ucn_frame_t *frame)
{
    uint32_t candidate_id;
    ucn_candidate_route_t *candidate;

    if (frame->payload_length != UCN_PATH_PROBE_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    candidate_id = read_u32_be(frame->payload);
    if (candidate_id == 0U || read_u32_be(frame->payload + 4U) == 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination != node->config.node_id) {
        return UCN_OK;
    }
    candidate = find_candidate_route(node, frame->source, candidate_id);
    if (candidate == NULL || !candidate->originated_here) {
        return UCN_ERR_NOT_FOUND;
    }
    if (candidate->probes_acked < UCN_PATH_PROBE_REQUIRED_ACKS) {
        candidate->probes_acked++;
        candidate->expires_at_ms = node->now_ms + UCN_ROUTE_CANDIDATE_TIMEOUT_MS;
        node->stats.path_probe_acks_received++;
    }
    return UCN_OK;
}

static ucn_result_t handle_path_activate(ucn_node_t *node,
                                         const ucn_frame_t *frame)
{
    uint32_t candidate_id;
    uint16_t route_epoch;
    ucn_link_t *egress_link;
    ucn_result_t result;

    if (frame->payload_length != UCN_PATH_ACTIVATE_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    candidate_id = read_u32_be(frame->payload);
    route_epoch = read_u16_be(frame->payload + 4U);
    if (candidate_id == 0U || route_epoch == 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (frame->destination == node->config.node_id) {
        result = activate_candidate_route(node, frame->source, candidate_id,
                                          route_epoch);
        if (result != UCN_OK) {
            return result;
        }
        egress_link = find_link(node, frame->source);
        if (egress_link == NULL) {
            return UCN_ERR_NOT_FOUND;
        }
        return send_control_on_link(node, egress_link, frame->source,
                                    UCN_MSG_PATH_ACTIVATE_ACK, frame->payload,
                                    frame->payload_length);
    }

    egress_link = find_candidate_link(node, frame->destination, candidate_id);
    if (egress_link == NULL || frame->hop_limit <= 1U) {
        return egress_link == NULL ? UCN_ERR_NOT_FOUND : UCN_ERR_TTL;
    }
    result = activate_candidate_route(node, frame->source, candidate_id,
                                      route_epoch);
    if (result != UCN_OK) {
        return result;
    }
    {
        ucn_frame_t forwarded = *frame;

        --forwarded.hop_limit;
        result = send_frame_on_link(node, egress_link, &forwarded);
    }
    if (result != UCN_OK) {
        return result;
    }
    return activate_candidate_route(node, frame->destination, candidate_id,
                                    route_epoch);
}

static ucn_result_t handle_path_activate_ack(ucn_node_t *node,
                                             const ucn_frame_t *frame)
{
    uint32_t candidate_id;
    uint16_t route_epoch;
    ucn_result_t result;

    if (frame->payload_length != UCN_PATH_ACTIVATE_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    candidate_id = read_u32_be(frame->payload);
    route_epoch = read_u16_be(frame->payload + 4U);
    if (candidate_id == 0U || route_epoch == 0U ||
        frame->destination != node->config.node_id) {
        return UCN_ERR_MALFORMED;
    }
    result = activate_candidate_route(node, frame->source, candidate_id,
                                      route_epoch);
    if (result == UCN_OK) {
        node->stats.route_switches++;
    }
    return result;
}

static ucn_result_t forward_route_request(ucn_node_t *node,
                                          ucn_link_t *ingress_link,
                                          const ucn_frame_t *frame)
{
    ucn_frame_t forwarded = *frame;
    uint16_t route_cost;
    uint8_t hop_count;
    size_t index;
    size_t sent_count = 0U;
    ucn_result_t last_error = UCN_ERR_NOT_FOUND;

    if (frame->hop_limit <= 1U) {
        return UCN_ERR_TTL;
    }
    if (frame->payload_length != UCN_ROUTE_REQ_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    route_cost = read_u16_be(frame->payload + 12U);
    hop_count = frame->payload[14];
    if (hop_count == UINT8_MAX) {
        return UCN_ERR_TTL;
    }
    --forwarded.hop_limit;

    for (index = 0U; index < node->link_count; ++index) {
        uint8_t payload[UCN_ROUTE_REQ_PAYLOAD_BYTES];
        ucn_result_t result;

        if (node->links[index] == ingress_link ||
            !link_is_candidate_eligible(node, node->links[index])) {
            continue;
        }
        (void)memcpy(payload, frame->payload, sizeof(payload));
        write_u16_be(payload + 12U,
                     add_route_cost(route_cost, link_route_cost(node->links[index])));
        payload[14] = (uint8_t)(hop_count + 1U);
        forwarded.payload = payload;
        result = send_frame_on_link(node, node->links[index], &forwarded);
        if (result == UCN_OK) {
            ++sent_count;
        } else {
            last_error = result;
        }
    }

    return sent_count != 0U ? UCN_OK : last_error;
}

static ucn_result_t send_route_error(ucn_node_t *node,
                                     ucn_link_t *upstream_link,
                                     ucn_node_id_t origin,
                                     ucn_node_id_t unreachable)
{
    uint8_t payload[UCN_ROUTE_ERROR_PAYLOAD_BYTES];
    ucn_result_t result;

    if (upstream_link == NULL || origin == 0U || origin == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }

    write_u32_be(payload, unreachable);
    result = send_control_on_link(node, upstream_link, origin, UCN_MSG_ROUTE_ERROR,
                                  payload, (uint16_t)sizeof(payload));
    if (result == UCN_OK) {
        node->stats.route_errors_sent++;
    }
    return result;
}

static void invalidate_route_to(ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && !node->routes[index].is_static &&
            node->routes[index].destination == destination) {
            node->routes[index].valid = false;
        }
    }
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].destination == destination) {
            node->candidates[index].valid = false;
        }
    }
    clear_discovery(node, destination);
}

static ucn_result_t handle_route_request(ucn_node_t *node,
                                         ucn_link_t *ingress_link,
                                         const ucn_frame_t *frame)
{
    ucn_node_id_t origin;
    ucn_node_id_t target;
    uint32_t request_id;
    uint16_t route_cost;
    uint8_t hop_count;
    bool is_candidate;
    ucn_result_t result;

    if (frame->payload_length != UCN_ROUTE_REQ_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }

    origin = read_u32_be(frame->payload);
    target = read_u32_be(frame->payload + 4U);
    request_id = read_u32_be(frame->payload + 8U);
    route_cost = read_u16_be(frame->payload + 12U);
    hop_count = frame->payload[14];
    is_candidate = (frame->payload[15] & UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U;
    if (origin != frame->source || origin == 0U || target == 0U ||
        target == UCN_NODE_BROADCAST || request_id == 0U ||
        (frame->payload[15] & (uint8_t)~UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (is_candidate && !link_is_candidate_eligible(node, ingress_link)) {
        return UCN_ERR_ACCESS;
    }

    result = is_candidate ?
             learn_candidate_route(node, origin, request_id, ingress_link,
                                   route_cost, hop_count, false) :
             learn_route(node, origin, ingress_link, route_cost, hop_count,
                         route_epoch_from_request_id(request_id));
    if (result != UCN_OK) {
        return result;
    }

    if (target == node->config.node_id) {
        uint8_t reply[UCN_ROUTE_REPLY_PAYLOAD_BYTES];
        ucn_route_entry_t *reverse_route = find_active_route(node, origin);

        if (!is_candidate && (reverse_route == NULL ||
                              reverse_route->route_epoch == 0U)) {
            return UCN_ERR_NOT_FOUND;
        }

        write_u32_be(reply, origin);
        write_u32_be(reply + 4U, target);
        write_u32_be(reply + 8U, request_id);
        write_u16_be(reply + 12U, route_cost);
        reply[14] = hop_count;
        reply[15] = is_candidate ? UCN_ROUTE_REQ_FLAG_CANDIDATE : 0U;
        write_u16_be(reply + 16U,
                     is_candidate ? 0U : reverse_route->route_epoch);
        result = send_control_on_link(node, ingress_link, origin, UCN_MSG_ROUTE_REPLY,
                                      reply, (uint16_t)sizeof(reply));
        if (result == UCN_OK) {
            node->stats.route_replies_sent++;
        }
        return result;
    }

    return forward_route_request(node, ingress_link, frame);
}

static ucn_result_t handle_route_reply(ucn_node_t *node,
                                       ucn_link_t *ingress_link,
                                       const ucn_frame_t *frame,
                                       bool *consumed)
{
    ucn_node_id_t origin;
    ucn_node_id_t target;
    uint32_t request_id;
    uint16_t route_cost;
    uint16_t route_epoch;
    uint8_t hop_count;
    bool is_candidate;
    size_t index;
    ucn_result_t result;

    *consumed = false;
    if (frame->payload_length != UCN_ROUTE_REPLY_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    origin = read_u32_be(frame->payload);
    target = read_u32_be(frame->payload + 4U);
    request_id = read_u32_be(frame->payload + 8U);
    route_cost = read_u16_be(frame->payload + 12U);
    hop_count = frame->payload[14];
    is_candidate = (frame->payload[15] & UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U;
    route_epoch = read_u16_be(frame->payload + 16U);
    if (origin == 0U || target != frame->source || target == UCN_NODE_BROADCAST ||
        request_id == 0U ||
        (frame->payload[15] & (uint8_t)~UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U ||
        (!is_candidate && route_epoch == 0U)) {
        return UCN_ERR_MALFORMED;
    }
    if (is_candidate && !link_is_candidate_eligible(node, ingress_link)) {
        return UCN_ERR_ACCESS;
    }

    if (is_candidate && frame->destination == node->config.node_id) {
        ucn_route_entry_t *active_route = find_active_route(node, target);

        if (active_route == NULL || active_route->is_static ||
            !candidate_is_sufficiently_better(active_route->route_cost, route_cost)) {
            node->stats.candidate_rejected++;
            *consumed = true;
            return UCN_OK;
        }
        result = learn_candidate_route(node, target, request_id, ingress_link,
                                       route_cost, hop_count, true);
        if (result != UCN_OK) {
            return result;
        }
        for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
            if (node->discoveries[index].active &&
                node->discoveries[index].destination == target &&
                node->discoveries[index].request_id == request_id &&
                node->discoveries[index].is_candidate) {
                node->discoveries[index].active = false;
                break;
            }
        }
        *consumed = true;
        return UCN_OK;
    }

    result = is_candidate ?
             learn_candidate_route(node, target, request_id, ingress_link,
                                   route_cost, hop_count, false) :
             learn_route(node, target, ingress_link, route_cost, hop_count,
                         route_epoch);
    if (result != UCN_OK) {
        return result;
    }

    if (frame->destination != node->config.node_id) {
        return UCN_OK;
    }

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == target &&
            node->discoveries[index].request_id == request_id) {
            node->discoveries[index].active = false;
            *consumed = true;
            return UCN_OK;
        }
    }

    *consumed = true;
    return UCN_OK;
}

static ucn_result_t handle_route_error(ucn_node_t *node,
                                       const ucn_frame_t *frame,
                                       bool *consumed)
{
    ucn_node_id_t unreachable;

    *consumed = false;
    if (frame->payload_length != UCN_ROUTE_ERROR_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    unreachable = read_u32_be(frame->payload);
    if (unreachable == 0U || unreachable == UCN_NODE_BROADCAST) {
        return UCN_ERR_MALFORMED;
    }
    invalidate_route_to(node, unreachable);
    if (frame->destination == node->config.node_id) {
        *consumed = true;
    }
    return UCN_OK;
}

/* HELLO is strictly link-local: it binds an ingress Link to one Node ID and
 * then hands the candidate to the configured join policy.  It is never
 * delivered to the application or forwarded by the mesh router. */
static ucn_result_t handle_hello(ucn_node_t *node,
                                 ucn_link_t *ingress_link,
                                 const ucn_frame_t *frame)
{
    ucn_node_id_t peer_node_id;

    if (frame->payload_length != UCN_HELLO_PAYLOAD_BYTES ||
        (frame->destination != node->config.node_id &&
         frame->destination != UCN_NODE_BROADCAST)) {
        return UCN_ERR_MALFORMED;
    }

    peer_node_id = read_u32_be(frame->payload);
    if (frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        peer_node_id == 0U || peer_node_id == UCN_NODE_BROADCAST ||
        peer_node_id != frame->source || peer_node_id == node->config.node_id ||
        (ingress_link->peer_node_id != 0U &&
         ingress_link->peer_node_id != peer_node_id)) {
        return UCN_ERR_MALFORMED;
    }

    ingress_link->peer_node_id = peer_node_id;
    return ucn_node_observe_neighbor(node, ingress_link, node->now_ms);
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
    node->next_sequence = 1U;
    node->next_queue_order = 1U;
    node->next_route_request_id = 1U;
    node->next_route_epoch = 1U;
    node->next_heartbeat_id = 1U;
    node->next_path_trace_id = 1U;
    node->next_node_snapshot_id = 1U;
    node->control_tokens = UCN_CONTROL_TOKEN_BURST;
    node->path_trace_tokens = UCN_PATH_TRACE_TOKEN_BURST;
    node->node_snapshot_tokens = UCN_NODE_SNAPSHOT_TOKEN_BURST;
    node->security_policy.tx_mode = UCN_SECURITY_TX_PLAIN;
    node->security_policy.rx_mode = UCN_SECURITY_RX_BOTH;
    node->security_policy.forward_mode = UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E;
    return UCN_OK;
}

ucn_result_t ucn_node_set_security(ucn_node_t *node,
                                   const ucn_security_ops_t *ops,
                                   void *context)
{
    ucn_sequence_t next_sequence;
    ucn_session_id_t session_id;
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    if (ops == NULL) {
        node->security_ops = NULL;
        node->security_context = NULL;
        node->session_id = 0U;
        return UCN_OK;
    }

    if (ops->load_next_sequence == NULL || ops->store_next_sequence == NULL ||
        ops->get_session_id == NULL || ops->authorize_tx == NULL ||
        ops->authorize_rx == NULL || ((ops->seal == NULL) != (ops->open == NULL))) {
        return UCN_ERR_ARGUMENT;
    }

    result = ops->load_next_sequence(context, &next_sequence);
    if (result != UCN_OK || next_sequence == 0U || next_sequence == UINT32_MAX) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    result = ops->get_session_id(context, &session_id);
    if (result != UCN_OK || session_id == 0U) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }

    node->security_ops = ops;
    node->security_context = context;
    node->next_sequence = next_sequence;
    node->session_id = session_id;
    return UCN_OK;
}

ucn_result_t ucn_node_set_security_policy(ucn_node_t *node,
                                          const ucn_security_policy_t *policy)
{
    if (node == NULL || !security_policy_is_valid(policy)) {
        return UCN_ERR_ARGUMENT;
    }
    node->security_policy = *policy;
    return UCN_OK;
}

ucn_result_t ucn_node_set_endpoint_security_policy(
    ucn_node_t *node,
    ucn_endpoint_t endpoint,
    const ucn_security_policy_t *policy)
{
    ucn_endpoint_security_policy_entry_t *entry;
    size_t index;

    if (node == NULL || !ucn_endpoint_is_static(endpoint)) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_endpoint_security_policy(node, endpoint);
    if (policy == NULL) {
        if (entry != NULL) {
            (void)memset(entry, 0, sizeof(*entry));
        }
        return UCN_OK;
    }
    if (!security_policy_is_valid(policy)) {
        return UCN_ERR_ARGUMENT;
    }
    if (entry != NULL) {
        entry->policy = *policy;
        return UCN_OK;
    }
    for (index = 0U; index < UCN_MAX_ENDPOINT_SECURITY_POLICIES; ++index) {
        if (!node->endpoint_security_policies[index].occupied) {
            node->endpoint_security_policies[index].occupied = true;
            node->endpoint_security_policies[index].endpoint = endpoint;
            node->endpoint_security_policies[index].policy = *policy;
            return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

ucn_result_t ucn_node_set_join_policy(ucn_node_t *node,
                                      ucn_join_policy_t policy,
                                      ucn_neighbor_authorize_fn authorize,
                                      void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (policy != UCN_JOIN_MANUAL && policy != UCN_JOIN_OPEN &&
        policy != UCN_JOIN_PROVIDER) {
        return UCN_ERR_ARGUMENT;
    }
    if (policy == UCN_JOIN_PROVIDER && authorize == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (policy != UCN_JOIN_PROVIDER && authorize != NULL) {
        return UCN_ERR_ARGUMENT;
    }

    node->join_policy = policy;
    node->neighbor_authorize = authorize;
    node->neighbor_authorize_context = context;
    return UCN_OK;
}

ucn_result_t ucn_node_set_node_snapshot_authorizer(
    ucn_node_t *node,
    ucn_node_snapshot_authorize_fn authorize,
    void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->node_snapshot_authorize = authorize;
    node->node_snapshot_authorize_context = context;
    return UCN_OK;
}

ucn_result_t ucn_node_observe_neighbor(ucn_node_t *node,
                                       ucn_link_t *link,
                                       uint32_t now_ms)
{
    ucn_neighbor_entry_t *entry;
    ucn_result_t result;

    if (node == NULL || link == NULL || link->peer_node_id == 0U ||
        link->peer_node_id == UCN_NODE_BROADCAST ||
        link->peer_node_id == node->config.node_id || link->ops == NULL ||
        link->ops->send == NULL || link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    expire_neighbor_candidates(node, now_ms);
    entry = find_neighbor(node, link->peer_node_id);
    if (entry != NULL) {
        if (entry->state == UCN_NEIGHBOR_ADMITTED && entry->link != link) {
            return UCN_ERR_CONFIG;
        }
        if (entry->state == UCN_NEIGHBOR_ADMITTED ||
            entry->state == UCN_NEIGHBOR_SUSPECT) {
            if (entry->link != link) {
                return UCN_ERR_CONFIG;
            }
            entry->last_seen_ms = now_ms;
            entry->state = UCN_NEIGHBOR_ADMITTED;
            entry->suspect_since_ms = 0U;
            return UCN_OK;
        }
        (void)memset(entry, 0, sizeof(*entry));
        entry->state = UCN_NEIGHBOR_CANDIDATE;
        entry->peer_node_id = link->peer_node_id;
        entry->link = link;
        entry->last_seen_ms = now_ms;
    } else {
        entry = allocate_neighbor_slot(node);
        if (entry == NULL) {
            return UCN_ERR_NO_SPACE;
        }
        (void)memset(entry, 0, sizeof(*entry));
        entry->state = UCN_NEIGHBOR_CANDIDATE;
        entry->peer_node_id = link->peer_node_id;
        entry->link = link;
        entry->last_seen_ms = now_ms;
    }

    if (node->join_policy == UCN_JOIN_MANUAL) {
        return UCN_OK;
    }
    if (node->join_policy == UCN_JOIN_PROVIDER) {
        result = node->neighbor_authorize(node->neighbor_authorize_context,
                                          node->config.node_id,
                                          entry->peer_node_id,
                                          entry->link);
        if (result != UCN_OK) {
            entry->state = UCN_NEIGHBOR_REJECTED;
            return result;
        }
    }
    return admit_neighbor_entry(node, entry);
}

ucn_result_t ucn_node_probe_neighbor(ucn_node_t *node,
                                     ucn_link_t *link,
                                     uint32_t now_ms)
{
    uint8_t payload[UCN_HELLO_PAYLOAD_BYTES];
    ucn_result_t result;

    result = ucn_node_observe_neighbor(node, link, now_ms);
    if (result != UCN_OK) {
        return result;
    }

    node->now_ms = now_ms;
    write_u32_be(payload, node->config.node_id);
    return send_control_on_link(node, link, link->peer_node_id, UCN_MSG_HELLO,
                                payload, (uint16_t)sizeof(payload));
}

ucn_result_t ucn_node_broadcast_hello(ucn_node_t *node,
                                      ucn_link_t *link,
                                      uint32_t now_ms)
{
    uint8_t payload[UCN_HELLO_PAYLOAD_BYTES];

    if (node == NULL || link == NULL || link->ops == NULL ||
        link->ops->send == NULL || link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    node->now_ms = now_ms;
    write_u32_be(payload, node->config.node_id);
    return send_control_on_link(node, link, UCN_NODE_BROADCAST, UCN_MSG_HELLO,
                                payload, (uint16_t)sizeof(payload));
}

ucn_result_t ucn_node_admit_neighbor(ucn_node_t *node,
                                     ucn_node_id_t peer_node_id)
{
    ucn_neighbor_entry_t *entry;

    if (node == NULL || peer_node_id == 0U || peer_node_id == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_neighbor(node, peer_node_id);
    if (entry == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (entry->state == UCN_NEIGHBOR_ADMITTED) {
        return UCN_OK;
    }
    return admit_neighbor_entry(node, entry);
}

ucn_result_t ucn_node_reject_neighbor(ucn_node_t *node,
                                      ucn_node_id_t peer_node_id)
{
    ucn_neighbor_entry_t *entry;

    if (node == NULL || peer_node_id == 0U || peer_node_id == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_neighbor(node, peer_node_id);
    if (entry == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (entry->state == UCN_NEIGHBOR_ADMITTED) {
        return UCN_ERR_UNSUPPORTED;
    }
    entry->state = UCN_NEIGHBOR_REJECTED;
    return UCN_OK;
}

size_t ucn_node_neighbor_count(const ucn_node_t *node,
                               ucn_neighbor_state_t state)
{
    size_t index;
    size_t count = 0U;

    if (node == NULL || state == UCN_NEIGHBOR_EMPTY) {
        return 0U;
    }
    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state == state) {
            ++count;
        }
    }
    return count;
}

ucn_result_t ucn_node_register_link(ucn_node_t *node, ucn_link_t *link)
{
    size_t index;
    ucn_result_t result;

    if (node == NULL || link == NULL || link->ops == NULL ||
        link->ops->send == NULL || link->ops->get_status == NULL ||
        link->mtu < UCN_FRAME_HEADER_SIZE) {
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

    node->links[node->link_count] = link;
    ++node->link_count;
    return UCN_OK;
}

ucn_result_t ucn_node_add_route(ucn_node_t *node,
                                ucn_node_id_t destination,
                                ucn_link_t *egress_link)
{
    size_t index;

    if (node == NULL || destination == 0U || destination == UCN_NODE_BROADCAST ||
        egress_link == NULL || !link_is_registered(node, egress_link)) {
        return UCN_ERR_ARGUMENT;
    }

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination) {
            node->routes[index].is_static = true;
            node->routes[index].egress_link = egress_link;
            node->routes[index].expires_at_ms = 0U;
            node->routes[index].route_cost = link_route_cost(egress_link);
            node->routes[index].hop_count = 1U;
            return UCN_OK;
        }
    }

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (!node->routes[index].valid) {
            node->routes[index].valid = true;
            node->routes[index].is_static = true;
            node->routes[index].destination = destination;
            node->routes[index].egress_link = egress_link;
            node->routes[index].expires_at_ms = 0U;
            node->routes[index].route_cost = link_route_cost(egress_link);
            node->routes[index].hop_count = 1U;
            return UCN_OK;
        }
    }

    return UCN_ERR_NO_SPACE;
}

static ucn_result_t begin_route_discovery(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          uint32_t now_ms,
                                          bool is_candidate)
{
    ucn_route_discovery_t *slot = NULL;
    uint8_t payload[UCN_ROUTE_REQ_PAYLOAD_BYTES];
    ucn_frame_t frame;
    size_t index;
    ucn_result_t result;

    if (node == NULL || destination == 0U || destination == UCN_NODE_BROADCAST ||
        destination == node->config.node_id) {
        return UCN_ERR_ARGUMENT;
    }

    expire_dynamic_state(node, now_ms);
    if (!is_candidate && find_link(node, destination) != NULL) {
        return UCN_OK;
    }
    if (is_candidate) {
        ucn_route_entry_t *active_route = find_active_route(node, destination);

        if (active_route == NULL || active_route->is_static) {
            return UCN_ERR_NOT_FOUND;
        }
    }

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination &&
            node->discoveries[index].is_candidate == is_candidate) {
            if ((uint32_t)(now_ms - node->discoveries[index].started_at_ms) <
                UCN_ROUTE_REQUEST_MIN_INTERVAL_MS) {
                return UCN_OK;
            }
            slot = &node->discoveries[index];
            break;
        }
        if (!node->discoveries[index].active && slot == NULL) {
            slot = &node->discoveries[index];
        }
    }

    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    if (!take_control_token(node)) {
        return UCN_ERR_NO_SPACE;
    }

    if (node->next_route_request_id == 0U) {
        node->next_route_request_id = 1U;
    }
    slot->active = true;
    slot->destination = destination;
    slot->request_id = node->next_route_request_id++;
    slot->started_at_ms = now_ms;
    slot->deadline_ms = now_ms + UCN_ROUTE_REQUEST_TIMEOUT_MS;
    slot->is_candidate = is_candidate;

    write_u32_be(payload, node->config.node_id);
    write_u32_be(payload + 4U, destination);
    write_u32_be(payload + 8U, slot->request_id);
    write_u16_be(payload + 12U, 0U);
    payload[14] = 0U;
    payload[15] = is_candidate ? UCN_ROUTE_REQ_FLAG_CANDIDATE : 0U;
    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_ROUTE_REQ;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = UCN_NODE_BROADCAST;
    frame.session_id = node->session_id;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        slot->active = false;
        return result;
    }
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);

    result = forward_route_request(node, NULL, &frame);
    if (result != UCN_OK) {
        slot->active = false;
        return result;
    }
    node->stats.route_requests_sent++;
    if (is_candidate) {
        node->stats.route_refreshes_started++;
        {
            ucn_route_entry_t *active_route = find_active_route(node, destination);
            if (active_route != NULL) {
                active_route->last_refresh_started_ms = now_ms;
            }
        }
    }
    return UCN_OK;
}

ucn_result_t ucn_node_discover_route(ucn_node_t *node,
                                     ucn_node_id_t destination,
                                     uint32_t now_ms)
{
    return begin_route_discovery(node, destination, now_ms, false);
}

ucn_result_t ucn_node_refresh_route(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    uint32_t now_ms)
{
    return begin_route_discovery(node, destination, now_ms, true);
}

bool ucn_node_route_pending(const ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;

    if (node == NULL) {
        return false;
    }
    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination) {
            return true;
        }
    }
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
    entry = find_endpoint_handler(node, endpoint);
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
    ucn_frame_t frame;
    ucn_link_t *link;
    ucn_route_entry_t *route;
    ucn_link_status_t status;
    ucn_result_t result;
    uint8_t ciphertext[UCN_MAX_PAYLOAD_BYTES];
    uint8_t auth_tag[UCN_E2E_TAG_SIZE];

    if (node == NULL || destination == 0U ||
        (payload_length != 0U && payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }

    if (traffic_class != UCN_TRAFFIC_Q0_CRITICAL &&
        traffic_class != UCN_TRAFFIC_Q1_REALTIME) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (ucn_message_type_is_control(message_type)) {
        return UCN_ERR_ARGUMENT;
    }

    link = find_link(node, destination);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }

    result = get_link_status(link, &status);
    if (result != UCN_OK) {
        return result;
    }

    if (!status.is_up) {
        return UCN_ERR_LINK_DOWN;
    }

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = traffic_class;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    route = find_active_route(node, destination);
    if (route != NULL && link == route->egress_link && route->route_epoch != 0U) {
        frame.flags |= UCN_FRAME_FLAG_ROUTE_EXTENSION;
        frame.has_route_extension = true;
        frame.route_epoch = route->route_epoch;
    }
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = protect_outbound_business(node, &frame, ciphertext, auth_tag);
    if (result != UCN_OK) {
        return result;
    }
    return send_frame_on_link(node, link, &frame);
}

ucn_result_t ucn_node_send_endpoint(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    ucn_endpoint_t endpoint,
                                    ucn_traffic_class_t traffic_class,
                                    const uint8_t *payload,
                                    uint16_t payload_length)
{
    ucn_result_t result;

    if (node == NULL || !ucn_endpoint_is_static(endpoint) || destination == 0U ||
        (payload_length != 0U && payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    if (traffic_class == UCN_TRAFFIC_Q1_REALTIME &&
        find_link(node, destination) == NULL) {
        result = begin_route_discovery(node, destination, node->now_ms, false);
        if (result != UCN_OK) {
            return result;
        }
        if (find_link(node, destination) == NULL) {
            return queue_pending_q1(node, destination, (uint8_t)endpoint, payload,
                                    payload_length);
        }
    }
    return ucn_node_send(node, destination, (uint8_t)endpoint, traffic_class,
                         payload, payload_length);
}

ucn_result_t ucn_node_enqueue(ucn_node_t *node,
                              const ucn_send_request_t *request)
{
    ucn_tx_item_t *items;
    ucn_tx_item_t *slot = NULL;
    size_t count;
    size_t index;

    if (node == NULL || request == NULL || request->destination == 0U ||
        (request->payload_length != 0U && request->payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }

    if (request->traffic_class != UCN_TRAFFIC_Q0_CRITICAL &&
        request->traffic_class != UCN_TRAFFIC_Q1_REALTIME) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (ucn_message_type_is_control(request->message_type)) {
        return UCN_ERR_ARGUMENT;
    }

    if (request->delivery != UCN_DELIVERY_BEST_EFFORT &&
        request->delivery != UCN_DELIVERY_LATEST_VALUE) {
        return UCN_ERR_ARGUMENT;
    }

    if (request->payload_length > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }

    items = queue_items(node, request->traffic_class, &count);
    if (request->delivery == UCN_DELIVERY_LATEST_VALUE) {
        for (index = 0U; index < count; ++index) {
            if (items[index].occupied &&
                items[index].destination == request->destination &&
                items[index].message_type == request->message_type) {
                slot = &items[index];
                break;
            }
        }
    }

    if (slot == NULL) {
        for (index = 0U; index < count; ++index) {
            if (!items[index].occupied) {
                slot = &items[index];
                break;
            }
        }
    }

    if (slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }

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

ucn_result_t ucn_node_request_path_trace(ucn_node_t *node,
                                         ucn_node_id_t destination,
                                         uint8_t record_limit,
                                         ucn_path_trace_handler_t handler,
                                         void *context)
{
    ucn_path_trace_pending_t *pending;
    ucn_link_t *link;
    uint8_t payload[UCN_PATH_TRACE_MAX_PAYLOAD_BYTES];
    uint32_t trace_id;
    ucn_result_t result;

    if (node == NULL || handler == NULL || destination == 0U ||
        destination == UCN_NODE_BROADCAST || destination == node->config.node_id) {
        return UCN_ERR_ARGUMENT;
    }
    if (record_limit == 0U) {
        record_limit = (uint8_t)UCN_PATH_TRACE_MAX_NODES;
    }
    if (record_limit > UCN_PATH_TRACE_MAX_NODES) {
        return UCN_ERR_TOO_LARGE;
    }

    expire_path_trace_state(node, node->now_ms);
    pending = find_free_path_trace_pending(node);
    if (pending == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    link = find_link(node, destination);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (!take_path_trace_token(node)) {
        return UCN_ERR_NO_SPACE;
    }

    trace_id = node->next_path_trace_id;
    if (trace_id == 0U || trace_id == UINT32_MAX) {
        trace_id = 1U;
    }
    node->next_path_trace_id = trace_id + 1U;
    (void)memset(pending, 0, sizeof(*pending));
    pending->occupied = true;
    pending->destination = destination;
    pending->trace_id = trace_id;
    pending->deadline_ms = node->now_ms + UCN_PATH_TRACE_TIMEOUT_MS;
    pending->handler = handler;
    pending->context = context;

    (void)memset(payload, 0, sizeof(payload));
    write_u32_be(payload + UCN_PATH_TRACE_TRACE_ID_OFFSET, trace_id);
    payload[UCN_PATH_TRACE_RECORD_COUNT_OFFSET] = 1U;
    payload[UCN_PATH_TRACE_RECORD_LIMIT_OFFSET] = record_limit;
    payload[UCN_PATH_TRACE_STATUS_OFFSET] = (uint8_t)UCN_PATH_TRACE_STATUS_OK;
    write_u32_be(payload + UCN_PATH_TRACE_NODE_IDS_OFFSET, node->config.node_id);
    result = send_path_trace_request_on_link(node, link, destination, payload,
                                             (uint16_t)(UCN_PATH_TRACE_FIXED_PAYLOAD_BYTES +
                                                        sizeof(ucn_node_id_t)));
    if (result != UCN_OK) {
        if (pending->occupied && pending->trace_id == trace_id) {
            pending->occupied = false;
        }
        return result;
    }
    node->stats.path_trace_requests_sent++;
    mark_route_used(node, destination);
    return UCN_OK;
}

ucn_result_t ucn_node_request_node_snapshot(
    ucn_node_t *node,
    uint8_t result_limit,
    ucn_node_snapshot_handler_t handler,
    void *context)
{
    ucn_node_snapshot_pending_t *pending;
    ucn_frame_t frame;
    uint8_t payload[UCN_NODE_SNAPSHOT_REQUEST_PAYLOAD_BYTES];
    uint32_t query_id;
    ucn_result_t result;

    if (node == NULL || handler == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (result_limit == 0U) {
        result_limit = (uint8_t)UCN_NODE_SNAPSHOT_MAX_RESULTS;
    }
    if (result_limit > UCN_NODE_SNAPSHOT_MAX_RESULTS) {
        return UCN_ERR_TOO_LARGE;
    }

    expire_node_snapshot_state(node, node->now_ms);
    pending = find_free_node_snapshot_pending(node);
    if (pending == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    if (!take_node_snapshot_token(node)) {
        return UCN_ERR_NO_SPACE;
    }

    query_id = node->next_node_snapshot_id;
    if (query_id == 0U || query_id == UINT32_MAX) {
        query_id = 1U;
    }
    node->next_node_snapshot_id = query_id + 1U;
    (void)memset(pending, 0, sizeof(*pending));
    pending->occupied = true;
    pending->result_limit = result_limit;
    pending->query_id = query_id;
    pending->deadline_ms = node->now_ms + UCN_NODE_SNAPSHOT_TIMEOUT_MS;
    pending->node_count = 1U;
    pending->entries[0].node_id = node->config.node_id;
    pending->entries[0].direct_link_count = (uint8_t)node->link_count;
    pending->handler = handler;
    pending->context = context;

    (void)memset(&frame, 0, sizeof(frame));
    (void)memset(payload, 0, sizeof(payload));
    write_u32_be(payload + UCN_NODE_SNAPSHOT_QUERY_ID_OFFSET, query_id);
    frame.message_type = UCN_MSG_NODE_SNAPSHOT_REQ;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_DIAGNOSTIC;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = UCN_NODE_BROADCAST;
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        pending->occupied = false;
        return result;
    }

    /* The origin also remembers its own flood so a loop cannot cause it to
     * become a responder or re-flood the same Query ID. */
    remember_frame(node, &frame);
    result = forward_node_snapshot_request(node, NULL, &frame);
    if (result != UCN_OK) {
        pending->occupied = false;
        return result;
    }
    node->stats.node_snapshot_requests_sent++;
    return UCN_OK;
}

ucn_result_t ucn_node_step(ucn_node_t *node, uint32_t now_ms)
{
    ucn_tx_item_t *item;
    size_t count;
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    expire_dynamic_state(node, now_ms);
    expire_path_trace_state(node, now_ms);
    expire_node_snapshot_state(node, now_ms);
    expire_neighbor_candidates(node, now_ms);
    maintain_neighbor_liveness(node, now_ms);

    item = find_next_item(node->q0, UCN_TX_Q0_DEPTH);
    if (item == NULL) {
        item = find_next_item(node->q1, UCN_TX_Q1_DEPTH);
    }

    if (item == NULL) {
        result = send_pending_q1_if_ready(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
        result = send_due_heartbeat(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
        result = send_due_path_probe(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
        result = send_due_node_snapshot_reply(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
        return start_due_route_refresh(node, now_ms);
    }

    if (deadline_expired(now_ms, item->deadline_ms)) {
        item->occupied = false;
        node->stats.tx_expired_dropped++;
        return UCN_ERR_TTL;
    }

    count = item->payload_length;
    result = ucn_node_send(node,
                           item->destination,
                           item->message_type,
                           item->traffic_class,
                           item->payload,
                           (uint16_t)count);
    item->occupied = false;
    if (result != UCN_OK) {
        node->stats.tx_error_dropped++;
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
    bool control_consumed = false;
    ucn_result_t result;
    uint8_t plaintext[UCN_MAX_PAYLOAD_BYTES];

    if (node == NULL || ingress_link == NULL || data == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    result = ucn_frame_decode(data, length, &frame);
    if (result != UCN_OK) {
        return result;
    }

    if (frame.network_id != node->config.network_id) {
        return UCN_ERR_NETWORK;
    }

    if (ucn_message_type_is_control(frame.message_type) &&
        (frame.flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U) {
        return UCN_ERR_MALFORMED;
    }

    if (node->security_ops != NULL) {
        result = node->security_ops->authorize_rx(node->security_context,
                                                  ingress_link, &frame);
        if (result != UCN_OK) {
            return result;
        }
    }

    if (frame.message_type == UCN_MSG_HELLO) {
        if (frame_is_seen(node, &frame)) {
            return UCN_ERR_REPLAY;
        }
        remember_frame(node, &frame);
        return handle_hello(node, ingress_link, &frame);
    }

    /* A Link becomes eligible for regular mesh traffic only after it was
     * configured statically or admitted through HELLO plus the join policy. */
    if (!link_is_registered(node, ingress_link)) {
        return UCN_ERR_ACCESS;
    }

    result = validate_inbound_business_security(node, ingress_link, &frame, plaintext);
    if (result != UCN_OK) {
        return result;
    }

    if (frame.message_type == UCN_MSG_ROUTE_REQ) {
        if (frame.destination != UCN_NODE_BROADCAST) {
            return UCN_ERR_MALFORMED;
        }
        if (frame.payload_length != UCN_ROUTE_REQ_PAYLOAD_BYTES) {
            return UCN_ERR_MALFORMED;
        }
        if (!remember_better_route_request(node, &frame,
                                           read_u16_be(frame.payload + 12U))) {
            return UCN_ERR_REPLAY;
        }
        touch_neighbor(node, ingress_link);
        return handle_route_request(node, ingress_link, &frame);
    }

    if (frame_is_seen(node, &frame)) {
        return UCN_ERR_REPLAY;
    }
    remember_frame(node, &frame);

    if (frame.message_type == UCN_MSG_HEARTBEAT) {
        return handle_heartbeat(node, ingress_link, &frame);
    }

    touch_neighbor(node, ingress_link);

    if (frame.message_type == UCN_MSG_NODE_SNAPSHOT_REQ) {
        return handle_node_snapshot_request(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_NODE_SNAPSHOT_REPLY) {
        return handle_node_snapshot_reply(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_TRACE_REQ) {
        return handle_path_trace_request(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_TRACE_REPLY) {
        return handle_path_trace_reply(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_PROBE &&
        frame.destination == node->config.node_id) {
        return handle_path_probe(node, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_PROBE_ACK &&
        frame.destination == node->config.node_id) {
        return handle_path_probe_ack(node, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_ACTIVATE) {
        return handle_path_activate(node, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_ACTIVATE_ACK &&
        frame.destination == node->config.node_id) {
        return handle_path_activate_ack(node, &frame);
    }

    if (frame.message_type == UCN_MSG_ROUTE_REPLY) {
        result = handle_route_reply(node, ingress_link, &frame, &control_consumed);
        if (result != UCN_OK) {
            return result;
        }
        if (control_consumed) {
            return UCN_OK;
        }
    }

    if (frame.message_type == UCN_MSG_ROUTE_ERROR) {
        result = handle_route_error(node, &frame, &control_consumed);
        if (result != UCN_OK) {
            return result;
        }
        if (control_consumed) {
            return UCN_OK;
        }
    }

    if (!ucn_message_type_is_control(frame.message_type) && frame.has_route_extension &&
        frame.destination == node->config.node_id &&
        !route_epoch_is_accepted(node, frame.source, &frame)) {
        node->stats.route_epoch_rejected++;
        return UCN_ERR_NOT_FOUND;
    }

    if (frame.destination != node->config.node_id &&
        frame.destination != UCN_NODE_BROADCAST) {
        ucn_link_t *egress_link;

        if (frame.hop_limit <= 1U) {
            return UCN_ERR_TTL;
        }

        if (frame.message_type == UCN_MSG_ROUTE_REPLY &&
            frame.payload_length == UCN_ROUTE_REPLY_PAYLOAD_BYTES &&
            (frame.payload[15] & UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U) {
            egress_link = find_candidate_link(node, frame.destination,
                                              read_u32_be(frame.payload + 8U));
        } else if (frame.message_type == UCN_MSG_PATH_PROBE ||
                   frame.message_type == UCN_MSG_PATH_PROBE_ACK) {
            if (frame.payload_length != UCN_PATH_PROBE_PAYLOAD_BYTES ||
                read_u32_be(frame.payload) == 0U) {
                return UCN_ERR_MALFORMED;
            }
            egress_link = find_candidate_link(node, frame.destination,
                                              read_u32_be(frame.payload));
        } else if (!ucn_message_type_is_control(frame.message_type)) {
            egress_link = find_link_for_route_epoch(node, frame.destination,
                                                     frame.has_route_extension,
                                                     frame.route_epoch);
        } else {
            egress_link = find_link(node, frame.destination);
        }
        if (egress_link == NULL || egress_link == ingress_link) {
            if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source, frame.destination);
            }
            return UCN_ERR_NOT_FOUND;
        }

        --frame.hop_limit;
        result = send_frame_on_link(node, egress_link, &frame);
        if (result == UCN_OK) {
            mark_route_used(node, frame.destination);
            if ((frame.flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U) {
                node->stats.e2e_protected_forwarded++;
            }
        }
        if (result == UCN_ERR_LINK_DOWN) {
            invalidate_routes_by_link(node, egress_link);
            if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source, frame.destination);
            }
        }
        return result;
    }

    if (!dispatch_endpoint(node, &frame) && node->rx_handler != NULL) {
        node->rx_handler(node->rx_context, &frame);
    }

    node->stats.rx_delivered++;

    return UCN_OK;
}
