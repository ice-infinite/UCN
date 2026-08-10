#include <string.h>

#include "ucn/ucn.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node_storage.h"
#include "ucn/ucn_time.h"

#include "ucn_duplicate_internal.h"

#define UCN_ROUTE_REQ_FIXED_PAYLOAD_BYTES ((size_t)8U)
#define UCN_ROUTE_REQ_MAX_PAYLOAD_BYTES ((size_t)12U)
#define UCN_ROUTE_REPLY_PAYLOAD_BYTES ((size_t)18U)
#define UCN_ROUTE_ERROR_PAYLOAD_BYTES ((size_t)4U)
#define UCN_PATH_ROUTE_ERROR_PAYLOAD_BYTES ((size_t)16U)
#define UCN_HELLO_PAYLOAD_BYTES ((size_t)0U)
#define UCN_HEARTBEAT_PAYLOAD_BYTES ((size_t)8U)
#define UCN_PATH_PROBE_PAYLOAD_BYTES ((size_t)12U)
#define UCN_PATH_ACTIVATE_PAYLOAD_BYTES ((size_t)6U)
#define UCN_PATH_INSTALL_PAYLOAD_BYTES ((size_t)16U)
#define UCN_PATH_REVOKE_PAYLOAD_BYTES ((size_t)8U)
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
#define UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET ((size_t)0U)
#define UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET ((size_t)4U)
#define UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET ((size_t)5U)
#define UCN_POLICY_DIAGNOSTIC_REQUEST_RESERVED_OFFSET ((size_t)6U)
#define UCN_POLICY_DIAGNOSTIC_REPLY_STATUS_OFFSET ((size_t)6U)
#define UCN_POLICY_DIAGNOSTIC_REPLY_RESERVED_OFFSET ((size_t)7U)
#define UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET ((size_t)8U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_UP ((uint8_t)0x01U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_ROUTE_COST ((uint8_t)0x02U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_RTT ((uint8_t)0x04U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_TX_FAILURE ((uint8_t)0x08U)
#define UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_QUEUE_PRESSURE ((uint8_t)0x10U)
#define UCN_HEARTBEAT_REQUEST ((uint8_t)1U)
#define UCN_HEARTBEAT_ACK ((uint8_t)2U)
#define UCN_ROUTE_REQ_FLAG_CANDIDATE ((uint8_t)0x01U)

static void invalidate_routes_by_link(ucn_node_t *node, const ucn_link_t *link);
static ucn_result_t get_link_status(const ucn_link_t *link, ucn_link_status_t *status);
static bool link_is_usable(const ucn_link_t *link);
static ucn_link_t *resolve_egress_link(ucn_node_t *node, ucn_link_t *link);
#if UCN_FEATURE_PATH
static void revoke_path_and_mark_local_policy(ucn_node_t *node,
                                               ucn_node_id_t owner,
                                               ucn_session_id_t owner_session_id,
                                               ucn_path_id_t path_id,
                                               ucn_node_id_t destination);
static void revoke_paths_by_unavailable_egress(ucn_node_t *node,
                                                ucn_link_t *failed_link);
#endif
static ucn_result_t begin_route_discovery(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          uint32_t now_ms,
                                          bool is_candidate);
#if UCN_FEATURE_CANDIDATE_ROUTING
static void expire_candidate_routes(ucn_node_t *node);
#endif
#if UCN_FEATURE_PATH
static ucn_result_t send_path_route_error(ucn_node_t *node,
                                          ucn_link_t *upstream_link,
                                          ucn_node_id_t origin,
                                          ucn_node_id_t unreachable,
                                          ucn_session_id_t owner_session_id,
                                          ucn_path_id_t path_id);
#endif

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

static uint32_t read_uint_be(const uint8_t *data, uint8_t width)
{
    uint8_t index;
    uint32_t value = 0U;

    for (index = 0U; index < width; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

static void write_uint_be(uint8_t *data, uint8_t width, uint32_t value)
{
    uint8_t index;

    for (index = 0U; index < width; ++index) {
        const uint8_t shift = (uint8_t)((width - index - 1U) * 8U);

        data[index] = (uint8_t)(value >> shift);
    }
}

static size_t route_request_payload_size(ucn_wire_profile_t profile)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);

    return descriptor == NULL ? 0U :
           (size_t)descriptor->address_bytes + UCN_ROUTE_REQ_FIXED_PAYLOAD_BYTES;
}

static size_t route_request_id_offset(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);

    return descriptor == NULL ? 0U : descriptor->address_bytes;
}

static size_t route_request_cost_offset(const ucn_frame_t *frame)
{
    return route_request_id_offset(frame) + 4U;
}

static size_t route_request_hop_offset(const ucn_frame_t *frame)
{
    return route_request_id_offset(frame) + 6U;
}

static size_t route_request_flags_offset(const ucn_frame_t *frame)
{
    return route_request_id_offset(frame) + 7U;
}

static ucn_node_id_t route_request_target(const ucn_frame_t *frame)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(frame->wire_profile);

    return descriptor == NULL ? 0U :
           read_uint_be(frame->payload, descriptor->address_bytes);
}

static uint16_t add_route_cost(uint16_t left, uint16_t right)
{
    if (left == UCN_LINK_ROUTE_COST_UNKNOWN ||
        right == UCN_LINK_ROUTE_COST_UNKNOWN) {
        return UCN_LINK_ROUTE_COST_UNKNOWN;
    }
    return left > (uint16_t)(UCN_LINK_ROUTE_COST_MAX - right) ?
               UCN_LINK_ROUTE_COST_MAX : (uint16_t)(left + right);
}

static bool route_cost_is_known(uint16_t route_cost)
{
    return route_cost != 0U && route_cost != UCN_LINK_ROUTE_COST_UNKNOWN;
}

static bool route_cost_is_better(uint16_t candidate_cost, uint16_t active_cost)
{
    if (!route_cost_is_known(candidate_cost)) {
        return false;
    }
    return !route_cost_is_known(active_cost) || candidate_cost < active_cost;
}

static uint16_t route_epoch_from_request_id(ucn_wire_profile_t profile,
                                            uint32_t request_id)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);
    uint16_t route_epoch = (uint16_t)(request_id ^ (request_id >> 16U));

    if (descriptor != NULL && descriptor->route_epoch_bytes == 1U) {
        route_epoch = (uint16_t)(route_epoch & UINT16_C(0x00FF));
    }
    return route_epoch == 0U ? 1U : route_epoch;
}

static uint16_t link_route_cost(const ucn_link_t *link)
{
    ucn_link_metrics_t metrics;

    (void)memset(&metrics, 0, sizeof(metrics));
    if (link != NULL && link->ops != NULL && link->ops->get_metrics != NULL &&
        link->ops->get_metrics(link, &metrics) == UCN_OK &&
        metrics.route_cost_valid && route_cost_is_known(metrics.route_cost)) {
        return metrics.route_cost;
    }
    return UCN_UNKNOWN_LINK_ROUTE_COST;
}

static bool route_is_expired(const ucn_node_t *node, const ucn_route_entry_t *route)
{
    return !route->is_static &&
           ucn_deadline_expired(node->now_ms, route->expires_at_ms);
}

#if UCN_FEATURE_CANDIDATE_ROUTING
static bool candidate_is_expired(const ucn_node_t *node,
                                 const ucn_candidate_route_t *candidate)
{
    return ucn_deadline_expired(node->now_ms, candidate->expires_at_ms);
}
#endif

static bool cost_is_sufficiently_better(uint16_t active_cost,
                                        uint16_t candidate_cost,
                                        uint8_t improvement_percent)
{
    uint32_t candidate_scaled;
    uint32_t active_scaled;

    if (!route_cost_is_known(candidate_cost)) {
        return false;
    }
    if (!route_cost_is_known(active_cost)) {
        return true;
    }
    if (candidate_cost >= active_cost) {
        return false;
    }
    candidate_scaled = (uint32_t)candidate_cost * 100U;
    active_scaled = (uint32_t)active_cost *
                    (uint32_t)(100U - improvement_percent);
    return candidate_scaled <= active_scaled;
}

#if UCN_FEATURE_CANDIDATE_ROUTING
static bool candidate_is_sufficiently_better(uint16_t active_cost,
                                             uint16_t candidate_cost)
{
    return cost_is_sufficiently_better(active_cost, candidate_cost,
                                       UCN_ROUTE_SWITCH_IMPROVEMENT_PERCENT);
}
#endif

static ucn_link_t *find_direct_link(ucn_node_t *node,
                                    ucn_node_id_t destination)
{
    size_t index;
    ucn_link_t *best_direct = NULL;
    uint16_t best_direct_cost = UINT16_MAX;

    for (index = 0U; index < node->link_count; ++index) {
        ucn_link_t *link = node->links[index];
        ucn_link_t *selected;
        uint16_t route_cost;

        if (link->peer_node_id != destination) {
            continue;
        }
        selected = resolve_egress_link(node, link);
        if (selected != link) {
            continue;
        }
        route_cost = link_route_cost(link);
        if (best_direct == NULL || route_cost_is_better(route_cost, best_direct_cost)) {
            best_direct = link;
            best_direct_cost = route_cost;
        }
    }

    return best_direct;
}

static ucn_link_t *find_link(ucn_node_t *node, ucn_node_id_t destination)
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
            return resolve_egress_link(node, node->routes[index].egress_link);
        }
    }

    return NULL;
}

static ucn_link_t *find_link_for_route_epoch(ucn_node_t *node,
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
                return resolve_egress_link(node, route->egress_link);
            }
            if (route->previous_valid &&
                !ucn_deadline_expired(node->now_ms, route->previous_expires_at_ms) &&
                route->previous_route_epoch == route_epoch) {
                return resolve_egress_link(node, route->previous_egress_link);
            }
        } else if (route->route_epoch == 0U) {
            return resolve_egress_link(node, route->egress_link);
        } else if (route->previous_valid && route->previous_route_epoch == 0U &&
                   !ucn_deadline_expired(node->now_ms, route->previous_expires_at_ms)) {
            return resolve_egress_link(node, route->previous_egress_link);
        }
    }
    return NULL;
}

static bool route_epoch_is_accepted(ucn_node_t *node,
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

#if UCN_FEATURE_PATH
static const ucn_path_forward_entry_t *find_active_path(
    const ucn_node_t *node,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination)
{
    const ucn_path_forward_entry_t *entry = ucn_path_find(
        &node->path_state, owner, owner_session_id, path_id, destination);

    return ucn_path_is_expired(entry, node->now_ms) ? NULL : entry;
}
#endif

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
    size_t bearer_index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        if (node->neighbors[index].state == UCN_NEIGHBOR_EMPTY) {
            continue;
        }
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            if (node->neighbors[index].bearers[bearer_index].link == link) {
                return &node->neighbors[index];
            }
        }
    }
    return NULL;
}

static ucn_neighbor_bearer_t *find_neighbor_bearer(ucn_neighbor_entry_t *entry,
                                                    const ucn_link_t *link)
{
    size_t index;

    if (entry == NULL || link == NULL) {
        return NULL;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (entry->bearers[index].link == link) {
            return &entry->bearers[index];
        }
    }
    return NULL;
}

static ucn_neighbor_bearer_t *allocate_neighbor_bearer(ucn_neighbor_entry_t *entry)
{
    size_t index;

    if (entry == NULL || entry->bearer_count >= UCN_MAX_BEARERS_PER_NEIGHBOR) {
        return NULL;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (entry->bearers[index].state == UCN_NEIGHBOR_BEARER_EMPTY) {
            return &entry->bearers[index];
        }
    }
    return NULL;
}

static size_t bearer_index_from_entry(const ucn_neighbor_entry_t *entry,
                                      const ucn_neighbor_bearer_t *bearer)
{
    return (size_t)(bearer - entry->bearers);
}

static bool bearer_is_active(const ucn_neighbor_bearer_t *bearer)
{
    return bearer != NULL && (bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED ||
                               bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT);
}

static ucn_neighbor_bearer_t *select_neighbor_bearer(ucn_neighbor_entry_t *entry)
{
    size_t index;
    ucn_neighbor_bearer_t *best = NULL;
    uint16_t best_cost = UINT16_MAX;

    if (entry == NULL) {
        return NULL;
    }
    if (entry->primary_bearer_index != UCN_NEIGHBOR_PRIMARY_BEARER_NONE &&
        entry->primary_bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR) {
        ucn_neighbor_bearer_t *primary =
            &entry->bearers[entry->primary_bearer_index];

        if (bearer_is_active(primary) && link_is_usable(primary->link)) {
            return primary;
        }
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];

        if (bearer->state != UCN_NEIGHBOR_BEARER_ADMITTED ||
            !link_is_usable(bearer->link)) {
            continue;
        }
        if (best == NULL || link_route_cost(bearer->link) < best_cost) {
            best = bearer;
            best_cost = link_route_cost(bearer->link);
        }
    }
    if (best != NULL) {
        entry->primary_bearer_index = (uint8_t)bearer_index_from_entry(entry, best);
        return best;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];

        if (bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT &&
            link_is_usable(bearer->link)) {
            entry->primary_bearer_index = (uint8_t)index;
            return bearer;
        }
    }
    entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
    return NULL;
}

/* Quality switching deliberately stays inside one Neighbor's fixed Bearer
 * set.  It never changes an end-to-end route and therefore does not share
 * PATH_PROBE's multi-hop control plane. */
static void reset_bearer_quality_probe(ucn_neighbor_bearer_t *bearer)
{
    if (bearer == NULL) {
        return;
    }
    bearer->quality_probe_id = 0U;
    bearer->quality_probe_sent_at_ms = 0U;
    bearer->quality_better_samples = 0U;
    bearer->quality_probes_sent = 0U;
    bearer->quality_probe_acks = 0U;
    bearer->quality_probe_pending = false;
}

static void reset_neighbor_quality_probes(ucn_neighbor_entry_t *entry)
{
    size_t index;

    if (entry == NULL) {
        return;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        reset_bearer_quality_probe(&entry->bearers[index]);
    }
}

static ucn_neighbor_bearer_t *find_better_neighbor_bearer(
    ucn_neighbor_entry_t *entry,
    const ucn_neighbor_bearer_t *primary)
{
    size_t index;
    ucn_neighbor_bearer_t *best = NULL;
    uint16_t primary_cost;
    uint16_t best_cost = UINT16_MAX;

    if (entry == NULL || primary == NULL || !link_is_usable(primary->link)) {
        return NULL;
    }
    primary_cost = link_route_cost(primary->link);
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];
        uint16_t bearer_cost;

        if (bearer == primary ||
            bearer->state != UCN_NEIGHBOR_BEARER_ADMITTED ||
            !link_is_usable(bearer->link)) {
            continue;
        }
        bearer_cost = link_route_cost(bearer->link);
        if (!cost_is_sufficiently_better(
                primary_cost, bearer_cost,
                UCN_BEARER_SWITCH_IMPROVEMENT_PERCENT)) {
            continue;
        }
        if (best == NULL || bearer_cost < best_cost) {
            best = bearer;
            best_cost = bearer_cost;
        }
    }
    return best;
}

static void switch_neighbor_primary(ucn_node_t *node,
                                    ucn_neighbor_entry_t *entry,
                                    ucn_neighbor_bearer_t *bearer)
{
    uint8_t bearer_index;

    if (node == NULL || entry == NULL || bearer == NULL) {
        return;
    }
    bearer_index = (uint8_t)bearer_index_from_entry(entry, bearer);
    if (entry->primary_bearer_index != bearer_index) {
        entry->primary_bearer_index = bearer_index;
        node->stats.bearer_quality_switches++;
    }
    reset_neighbor_quality_probes(entry);
}

static void evaluate_bearer_quality(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        ucn_neighbor_bearer_t *primary;
        ucn_neighbor_bearer_t *candidate;
        size_t bearer_index;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        primary = select_neighbor_bearer(entry);
        if (primary == NULL) {
            reset_neighbor_quality_probes(entry);
            continue;
        }
        candidate = find_better_neighbor_bearer(entry, primary);
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];

            if (bearer != candidate) {
                reset_bearer_quality_probe(bearer);
            }
        }
        if (candidate == NULL ||
            (entry->bearer_quality_sampled &&
             (uint32_t)(now_ms - entry->last_bearer_quality_sample_ms) <
             UCN_BEARER_QUALITY_SAMPLE_INTERVAL_MS)) {
            continue;
        }
        entry->last_bearer_quality_sample_ms = now_ms;
        entry->bearer_quality_sampled = true;
        if (candidate->quality_better_samples <
            UCN_BEARER_QUALITY_STABLE_SAMPLES) {
            candidate->quality_better_samples++;
        }
        if (candidate->quality_better_samples <
            UCN_BEARER_QUALITY_STABLE_SAMPLES) {
            continue;
        }
        if (candidate->quality_probe_acks >=
            UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS) {
            switch_neighbor_primary(node, entry, candidate);
        } else if (candidate->quality_probes_sent >=
                   UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS &&
                   (!candidate->quality_probe_pending ||
                    (uint32_t)(now_ms - candidate->quality_probe_sent_at_ms) >=
                    UCN_BEARER_QUALITY_PROBE_INTERVAL_MS)) {
            reset_bearer_quality_probe(candidate);
        }
    }
}

/* Routes retain the Link that learned them, while a Neighbor may later move
 * its active physical carrier to a healthy backup.  Resolve at send time so
 * that Bearer failover does not invalidate the logical next hop. */
static ucn_link_t *resolve_egress_link(ucn_node_t *node, ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);
    ucn_neighbor_bearer_t *bearer;

    if (entry == NULL) {
        return link;
    }
    bearer = select_neighbor_bearer(entry);
    return bearer == NULL ? NULL : bearer->link;
}

/* A Path stores the Link through which it was provisioned, but that Link can
 * be one Bearer in a logical Neighbor.  Revoke only after the whole Neighbor
 * Bearer set is unavailable; a primary-to-backup switch must preserve the
 * authenticated Path ID and its forwarding entry. */
#if UCN_FEATURE_PATH
static void revoke_path_and_mark_local_policy(ucn_node_t *node,
                                               ucn_node_id_t owner,
                                               ucn_session_id_t owner_session_id,
                                               ucn_path_id_t path_id,
                                               ucn_node_id_t destination)
{
    size_t index;

    if (node == NULL || owner == 0U || owner_session_id == 0U ||
        path_id == 0U || destination == 0U) {
        return;
    }
    (void)ucn_path_revoke(&node->path_state, owner, owner_session_id,
                          path_id, destination);
    if (owner != node->config.node_id || owner_session_id != node->session_id) {
        return;
    }
    for (index = 0U; index < UCN_MAX_POLICY_PATHS; ++index) {
        const ucn_policy_path_entry_t *policy_path =
            &node->policy_state.paths[index];

        if (policy_path->occupied && policy_path->wire_path_id == path_id &&
            policy_path->destination == destination) {
            ucn_policy_mark_path_down(&node->policy_state,
                                      policy_path->local_path_id);
        }
    }
}

static void revoke_paths_by_unavailable_egress(ucn_node_t *node,
                                                ucn_link_t *failed_link)
{
    ucn_neighbor_entry_t *neighbor;
    size_t index;

    if (node == NULL || failed_link == NULL) {
        return;
    }
    neighbor = find_neighbor_by_link(node, failed_link);
    if (neighbor != NULL && select_neighbor_bearer(neighbor) != NULL) {
        return;
    }
    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        const ucn_path_forward_entry_t *path = &node->path_state.entries[index];
        bool affected = false;

        if (!path->occupied || path->terminal || path->egress_link == NULL) {
            continue;
        }
        if (neighbor != NULL) {
            affected = find_neighbor_bearer(neighbor, path->egress_link) != NULL;
        } else {
            affected = path->egress_link == failed_link &&
                       !link_is_usable(path->egress_link);
        }
        if (affected) {
            revoke_path_and_mark_local_policy(node, path->owner,
                                               path->owner_session_id,
                                               path->path_id,
                                               path->destination);
        }
    }
}
#endif

/* Static Links have no Neighbor entry.  A dynamically admitted Link may
 * carry its existing active traffic during SUSPECT, but it must not be used
 * to construct or validate a new candidate path. */
#if UCN_FEATURE_CANDIDATE_ROUTING || UCN_FEATURE_DIAGNOSTICS
static bool link_is_candidate_eligible(ucn_node_t *node, const ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);
    ucn_neighbor_bearer_t *bearer;

    if (entry == NULL) {
        return true;
    }
    bearer = find_neighbor_bearer(entry, link);
    return bearer != NULL && bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED &&
           select_neighbor_bearer(entry) == bearer;
}
#endif

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

static bool security_policy_is_production_ready(
    const ucn_security_policy_t *policy)
{
    return security_policy_is_valid(policy) &&
           policy->tx_mode != UCN_SECURITY_TX_PLAIN &&
           policy->rx_mode == UCN_SECURITY_RX_ENCRYPTED_ONLY &&
           policy->forward_mode !=
               UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E;
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

typedef enum ucn_rreq_cache_classification {
    UCN_RREQ_CACHE_NEW = 0,
    UCN_RREQ_CACHE_BETTER,
    UCN_RREQ_CACHE_REPLAY,
    UCN_RREQ_CACHE_FULL
} ucn_rreq_cache_classification_t;

static ucn_rreq_cache_classification_t classify_route_request(
    ucn_node_t *node,
    const ucn_frame_t *frame,
    uint16_t route_cost,
    size_t *slot_index)
{
    const uint32_t request_id =
        read_u32_be(frame->payload + route_request_id_offset(frame));
    size_t reusable_index = UCN_RREQ_CACHE_SIZE;
    size_t index;

    for (index = 0U; index < UCN_RREQ_CACHE_SIZE; ++index) {
        const ucn_rreq_cache_entry_t *slot = &node->rreq_cache[index];

        if (slot->valid && slot->origin == frame->source &&
            slot->session_id == frame->session_id &&
            slot->request_id == request_id) {
            *slot_index = index;
            return route_cost < slot->best_route_request_cost ?
                   UCN_RREQ_CACHE_BETTER : UCN_RREQ_CACHE_REPLAY;
        }
        if ((!slot->valid ||
             ucn_elapsed_at_least(node->now_ms, slot->last_observed_ms,
                                  UCN_RREQ_CACHE_TIMEOUT_MS)) &&
            reusable_index == UCN_RREQ_CACHE_SIZE) {
            reusable_index = index;
        }
    }

    if (reusable_index == UCN_RREQ_CACHE_SIZE) {
        return UCN_RREQ_CACHE_FULL;
    }
    *slot_index = reusable_index;
    return UCN_RREQ_CACHE_NEW;
}

static void commit_route_request(ucn_node_t *node,
                                 const ucn_frame_t *frame,
                                 uint16_t route_cost,
                                 size_t slot_index)
{
    ucn_rreq_cache_entry_t *slot = &node->rreq_cache[slot_index];

    slot->valid = true;
    slot->origin = frame->source;
    slot->session_id = frame->session_id;
    slot->request_id = read_u32_be(
        frame->payload + route_request_id_offset(frame));
    slot->best_route_request_cost = route_cost;
    slot->last_observed_ms = node->now_ms;
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

static void note_control_rx_drop(ucn_node_t *node,
                                 ucn_control_rx_budget_type_t type)
{
    if (type == UCN_CONTROL_RX_ROUTE_REQUEST) {
        node->stats.route_request_rx_rate_dropped++;
    } else if (type == UCN_CONTROL_RX_HEARTBEAT_REQUEST) {
        node->stats.heartbeat_rx_rate_dropped++;
    } else if (type == UCN_CONTROL_RX_PATH_TRACE_REQUEST) {
        node->stats.path_trace_rx_rate_dropped++;
    }
}

static ucn_control_rx_peer_budget_t *find_control_rx_peer_budget(
    ucn_node_t *node,
    ucn_node_id_t peer_node_id,
    bool allocate)
{
    ucn_control_rx_peer_budget_t *free_slot = NULL;
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_control_rx_peer_budget_t *slot = &node->control_rx_peer_budgets[index];

        if (slot->occupied && slot->peer_node_id == peer_node_id) {
            return slot;
        }
        if (!slot->occupied && free_slot == NULL) {
            free_slot = slot;
        }
    }
    if (!allocate || free_slot == NULL) {
        return NULL;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->occupied = true;
    free_slot->peer_node_id = peer_node_id;
    free_slot->budgets[UCN_CONTROL_RX_ROUTE_REQUEST].tokens =
        UCN_ROUTE_REQUEST_RX_TOKEN_BURST;
    free_slot->budgets[UCN_CONTROL_RX_HEARTBEAT_REQUEST].tokens =
        UCN_HEARTBEAT_RX_TOKEN_BURST;
    free_slot->budgets[UCN_CONTROL_RX_PATH_TRACE_REQUEST].tokens =
        UCN_PATH_TRACE_RX_TOKEN_BURST;
    free_slot->budgets[UCN_CONTROL_RX_ROUTE_REQUEST].last_refill_ms = node->now_ms;
    free_slot->budgets[UCN_CONTROL_RX_HEARTBEAT_REQUEST].last_refill_ms = node->now_ms;
    free_slot->budgets[UCN_CONTROL_RX_PATH_TRACE_REQUEST].last_refill_ms = node->now_ms;
    return free_slot;
}

static void release_control_rx_peer_budget(ucn_node_t *node,
                                           ucn_node_id_t peer_node_id)
{
    ucn_control_rx_peer_budget_t *slot =
        find_control_rx_peer_budget(node, peer_node_id, false);

    if (slot != NULL) {
        (void)memset(slot, 0, sizeof(*slot));
    }
}

static bool take_control_rx_token(ucn_node_t *node,
                                  const ucn_link_t *ingress_link,
                                  ucn_control_rx_budget_type_t type)
{
    ucn_control_rx_peer_budget_t *peer_budget;
    ucn_control_rx_budget_t *budget;
    uint8_t burst;
    uint32_t refill_ms;
    uint32_t elapsed;
    uint32_t refill_count;

    if (type >= UCN_CONTROL_RX_BUDGET_TYPE_COUNT || ingress_link == NULL ||
        ingress_link->peer_node_id == 0U ||
        ingress_link->peer_node_id == UCN_NODE_BROADCAST) {
        return false;
    }
    peer_budget = find_control_rx_peer_budget(node,
                                               ingress_link->peer_node_id, true);
    if (peer_budget == NULL) {
        note_control_rx_drop(node, type);
        return false;
    }
    budget = &peer_budget->budgets[type];
    switch (type) {
    case UCN_CONTROL_RX_ROUTE_REQUEST:
        burst = UCN_ROUTE_REQUEST_RX_TOKEN_BURST;
        refill_ms = UCN_ROUTE_REQUEST_RX_TOKEN_REFILL_MS;
        break;
    case UCN_CONTROL_RX_HEARTBEAT_REQUEST:
        burst = UCN_HEARTBEAT_RX_TOKEN_BURST;
        refill_ms = UCN_HEARTBEAT_RX_TOKEN_REFILL_MS;
        break;
    case UCN_CONTROL_RX_PATH_TRACE_REQUEST:
        burst = UCN_PATH_TRACE_RX_TOKEN_BURST;
        refill_ms = UCN_PATH_TRACE_RX_TOKEN_REFILL_MS;
        break;
    default:
        return false;
    }

    elapsed = node->now_ms - budget->last_refill_ms;
    refill_count = elapsed / refill_ms;
    if (refill_count != 0U) {
        const uint32_t new_tokens = (uint32_t)budget->tokens + refill_count;

        budget->tokens = (uint8_t)(new_tokens > burst ? burst : new_tokens);
        budget->last_refill_ms += refill_count * refill_ms;
    }
    if (budget->tokens == 0U) {
        note_control_rx_drop(node, type);
        return false;
    }
    --budget->tokens;
    return true;
}

/* Diagnostic traffic has an independent, much smaller budget.  A manual
 * topology query can therefore never consume the Q0 control budget used by
 * join, liveness, and route recovery. */
#if UCN_FEATURE_DIAGNOSTICS
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

/* Per-node strategy inspection is unicast, but still must never consume the
 * Q0 control budget or become a periodic telemetry stream. */
static bool take_policy_diagnostic_token(ucn_node_t *node)
{
    uint32_t elapsed = node->now_ms - node->policy_diagnostic_last_refill_ms;
    uint32_t refill_count = elapsed / UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS;

    if (refill_count != 0U) {
        uint32_t new_tokens = (uint32_t)node->policy_diagnostic_tokens + refill_count;

        node->policy_diagnostic_tokens =
            (uint8_t)(new_tokens > UCN_POLICY_DIAGNOSTIC_TOKEN_BURST ?
                          UCN_POLICY_DIAGNOSTIC_TOKEN_BURST : new_tokens);
        node->policy_diagnostic_last_refill_ms +=
            refill_count * UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS;
    }
    if (node->policy_diagnostic_tokens == 0U) {
        node->stats.policy_diagnostic_rate_dropped++;
        return false;
    }
    --node->policy_diagnostic_tokens;
    return true;
}
#endif

static void expire_neighbor_candidates(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        size_t bearer_index;
        bool has_candidate = false;
        bool has_active = false;

        if (entry->state == UCN_NEIGHBOR_EMPTY) {
            continue;
        }
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];

            if (bearer->state == UCN_NEIGHBOR_BEARER_CANDIDATE &&
                ucn_elapsed_at_least(now_ms, bearer->last_seen_ms,
                                     UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS)) {
                if (bearer->link != NULL && !link_is_registered(node, bearer->link)) {
                    bearer->link->peer_node_id = 0U;
                }
                (void)memset(bearer, 0, sizeof(*bearer));
                --entry->bearer_count;
                continue;
            }
            has_candidate = has_candidate ||
                bearer->state == UCN_NEIGHBOR_BEARER_CANDIDATE;
            has_active = has_active || bearer_is_active(bearer);
        }
        if (!has_active && !has_candidate && entry->bearer_count == 0U &&
            entry->state == UCN_NEIGHBOR_CANDIDATE) {
            entry->state = UCN_NEIGHBOR_EXPIRED;
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
    size_t index;
    ucn_result_t result = UCN_ERR_NOT_FOUND;
    bool admitted = false;

    if (entry == NULL || entry->state == UCN_NEIGHBOR_REJECTED ||
        entry->state == UCN_NEIGHBOR_EXPIRED ||
        entry->state == UCN_NEIGHBOR_REMOVED) {
        return UCN_ERR_NOT_FOUND;
    }
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_neighbor_bearer_t *bearer = &entry->bearers[index];

        if (bearer->state != UCN_NEIGHBOR_BEARER_CANDIDATE) {
            continue;
        }
        if (link_is_registered(node, bearer->link)) {
            bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
            admitted = true;
            continue;
        }
        result = ucn_node_register_link(node, bearer->link);
        if (result != UCN_OK) {
            return result;
        }
        bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
        admitted = true;
    }
    if (admitted || entry->state == UCN_NEIGHBOR_ADMITTED ||
        entry->state == UCN_NEIGHBOR_SUSPECT) {
        entry->state = UCN_NEIGHBOR_ADMITTED;
        entry->suspect_since_ms = 0U;
        (void)select_neighbor_bearer(entry);
        return UCN_OK;
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
#if UCN_FEATURE_CANDIDATE_ROUTING
    expire_candidate_routes(node);
#endif
}

static void expire_dynamic_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    node->now_ms = now_ms;
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && route_is_expired(node, &node->routes[index])) {
            node->routes[index].valid = false;
        } else if (node->routes[index].previous_valid &&
                   ucn_deadline_expired(now_ms,
                                        node->routes[index].previous_expires_at_ms)) {
            node->routes[index].previous_valid = false;
            node->routes[index].previous_egress_link = NULL;
            node->routes[index].previous_route_epoch = 0U;
            node->routes[index].previous_expires_at_ms = 0U;
        }
    }

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            ucn_deadline_expired(now_ms, node->discoveries[index].deadline_ms)) {
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
                    ucn_deadline_from_now(node->now_ms,
                                          UCN_ROUTE_ENTRY_LIFETIME_MS);
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
    free_slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_ROUTE_ENTRY_LIFETIME_MS);
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

#if UCN_FEATURE_CANDIDATE_ROUTING
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
        slot->expires_at_ms =
            ucn_deadline_from_now(node->now_ms,
                                  UCN_ROUTE_CANDIDATE_TIMEOUT_MS);
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
    slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_ROUTE_CANDIDATE_TIMEOUT_MS);
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
        route->previous_expires_at_ms =
            ucn_deadline_from_now(node->now_ms, UCN_ROUTE_EPOCH_GRACE_MS);
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
    route->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_ROUTE_ENTRY_LIFETIME_MS);
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
#endif

static void mark_route_used(ucn_node_t *node, ucn_node_id_t destination)
{
    ucn_route_entry_t *route = find_active_route(node, destination);

    if (route != NULL && !route->is_static) {
        route->last_used_at_ms = node->now_ms;
    }
}

#if UCN_FEATURE_CANDIDATE_ROUTING
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
            !ucn_deadline_due_within(now_ms, route->expires_at_ms,
                                     UCN_ROUTE_REFRESH_ADVANCE_MS)) {
            continue;
        }
        return begin_route_discovery(node, route->destination, now_ms, true);
    }
    return UCN_ERR_NOT_FOUND;
}
#endif

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
#if UCN_FEATURE_CANDIDATE_ROUTING
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].egress_link == link) {
            node->candidates[index].valid = false;
        }
    }
#endif
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
    size_t index;

    if (entry == NULL || entry->state == UCN_NEIGHBOR_EMPTY ||
        entry->state == UCN_NEIGHBOR_REMOVED) {
        return;
    }

    /* The entry is still available here, so the helper can distinguish this
     * complete logical-neighbor loss from a single failed physical Bearer. */
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (entry->bearers[index].link != NULL) {
#if UCN_FEATURE_PATH
            revoke_paths_by_unavailable_egress(node, entry->bearers[index].link);
#endif
            break;
        }
    }
    release_control_rx_peer_budget(node, entry->peer_node_id);
    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        ucn_link_t *link = entry->bearers[index].link;

        if (link == NULL) {
            continue;
        }
        invalidate_routes_by_link(node, link);
        unregister_link(node, link);
        link->peer_node_id = 0U;
    }
    (void)memset(entry, 0, sizeof(*entry));
    entry->state = UCN_NEIGHBOR_REMOVED;
    node->stats.neighbor_removed++;
}

static void touch_neighbor(ucn_node_t *node, ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);
    ucn_neighbor_bearer_t *bearer = find_neighbor_bearer(entry, link);

    if (entry != NULL && bearer != NULL && bearer_is_active(bearer)) {
        bearer->last_seen_ms = node->now_ms;
        if (bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT) {
            bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
        }
        if (entry->state == UCN_NEIGHBOR_SUSPECT) {
            entry->state = UCN_NEIGHBOR_ADMITTED;
            entry->suspect_since_ms = 0U;
        }
        (void)select_neighbor_bearer(entry);
    }
}

static bool neighbor_has_active_bearer(const ucn_neighbor_entry_t *entry)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (bearer_is_active(&entry->bearers[index])) {
            return true;
        }
    }
    return false;
}

static bool neighbor_has_admitted_bearer(const ucn_neighbor_entry_t *entry)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_BEARERS_PER_NEIGHBOR; ++index) {
        if (entry->bearers[index].state == UCN_NEIGHBOR_BEARER_ADMITTED) {
            return true;
        }
    }
    return false;
}

static void refresh_neighbor_liveness_state(ucn_node_t *node,
                                             ucn_neighbor_entry_t *entry,
                                             uint32_t now_ms)
{
    if (neighbor_has_admitted_bearer(entry)) {
        entry->state = UCN_NEIGHBOR_ADMITTED;
        entry->suspect_since_ms = 0U;
        (void)select_neighbor_bearer(entry);
        return;
    }
    if (neighbor_has_active_bearer(entry)) {
        if (entry->state != UCN_NEIGHBOR_SUSPECT) {
            entry->state = UCN_NEIGHBOR_SUSPECT;
            entry->suspect_since_ms = now_ms;
            node->stats.neighbor_suspected++;
        }
        (void)select_neighbor_bearer(entry);
        return;
    }
    remove_neighbor_entry(node, entry);
}

/* A Link may transition down between a caller's selection and the local
 * status check in send_frame_on_link().  Treat that observed local failure
 * like a sampled-down Bearer so a later Path resolution can use its backup. */
static void mark_neighbor_bearer_down(ucn_node_t *node, ucn_link_t *link)
{
    ucn_neighbor_entry_t *entry = find_neighbor_by_link(node, link);
    ucn_neighbor_bearer_t *bearer = find_neighbor_bearer(entry, link);

    if (entry == NULL || bearer == NULL || !bearer_is_active(bearer)) {
        return;
    }
    bearer->state = UCN_NEIGHBOR_BEARER_DOWN;
    if (entry->primary_bearer_index == bearer_index_from_entry(entry, bearer)) {
        entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
    }
    refresh_neighbor_liveness_state(node, entry, node->now_ms);
}

static void maintain_neighbor_liveness(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        size_t bearer_index;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];

            if (!bearer_is_active(bearer)) {
                continue;
            }
            if (!link_is_usable(bearer->link)) {
                bearer->state = UCN_NEIGHBOR_BEARER_DOWN;
                if (entry->primary_bearer_index == bearer_index) {
                    entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
                }
                continue;
            }
            if (bearer->state == UCN_NEIGHBOR_BEARER_ADMITTED &&
                ucn_elapsed_at_least(now_ms, bearer->last_seen_ms,
                                     UCN_NEIGHBOR_SUSPECT_TIMEOUT_MS)) {
                bearer->state = UCN_NEIGHBOR_BEARER_SUSPECT;
            }
            if (bearer->state == UCN_NEIGHBOR_BEARER_SUSPECT &&
                ucn_elapsed_at_least(now_ms, bearer->last_seen_ms,
                                     UCN_NEIGHBOR_REMOVE_TIMEOUT_MS)) {
                bearer->state = UCN_NEIGHBOR_BEARER_DOWN;
                if (entry->primary_bearer_index == bearer_index) {
                    entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
                }
            }
        }
        refresh_neighbor_liveness_state(node, entry, now_ms);
    }
}

static ucn_result_t allocate_sequence(ucn_node_t *node, ucn_sequence_t *sequence)
{
    ucn_sequence_t next_sequence;
    ucn_result_t result;

    if (node->next_sequence == 0U) {
        return UCN_ERR_SECURITY;
    }
    if (node->next_sequence >= UCN_SEQUENCE_ROTATION_THRESHOLD) {
        ucn_session_id_t new_session_id = 0U;
        ucn_sequence_t rotated_sequence = 0U;

        if (node->security_ops == NULL ||
            node->security_ops->rotate_session == NULL) {
            return UCN_ERR_SECURITY;
        }
        result = node->security_ops->rotate_session(node->security_context,
                                                     node->session_id,
                                                     &new_session_id,
                                                     &rotated_sequence);
        if (result != UCN_OK) {
            return result;
        }
        if (new_session_id == 0U || new_session_id == node->session_id ||
            new_session_id > ucn_wire_profile_get_descriptor(
                                 node->tx_wire_profile)->max_wire_value ||
            rotated_sequence == 0U ||
            rotated_sequence >= UCN_SEQUENCE_ROTATION_THRESHOLD) {
            return UCN_ERR_SECURITY;
        }
        node->session_id = new_session_id;
        node->next_sequence = rotated_sequence;
        node->stats.session_rotations++;
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
    slot->deadline_ms =
        ucn_deadline_from_now(node->now_ms, UCN_PENDING_Q1_TIMEOUT_MS);
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
        if (ucn_deadline_expired(now_ms, item->deadline_ms)) {
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

static bool link_is_usable(const ucn_link_t *link)
{
    ucn_link_status_t status;

    return get_link_status(link, &status) == UCN_OK && status.is_up;
}

static ucn_result_t send_frame_on_link(ucn_node_t *node,
                                       ucn_link_t *link,
                                       const ucn_frame_t *frame)
{
    ucn_frame_t prepared;
    ucn_link_status_t status;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_result_t result;

    if (node == NULL || link == NULL || frame == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    prepared = *frame;
    if (prepared.wire_profile == UCN_WIRE_PROFILE_UNSPECIFIED) {
        prepared.wire_profile = node->tx_wire_profile;
    }
    if (!ucn_node_security_ready(node)) {
        return UCN_ERR_SECURITY;
    }
    if (node->security_ops != NULL) {
        result = node->security_ops->authorize_tx(node->security_context,
                                                  &prepared);
        if (result != UCN_OK) {
            return result;
        }
    }

    result = get_link_status(link, &status);
    if (result != UCN_OK) {
        return result;
    }
    if (!status.is_up) {
        mark_neighbor_bearer_down(node, link);
        return UCN_ERR_LINK_DOWN;
    }

    result = ucn_frame_encode(&prepared, encoded, sizeof(encoded),
                              &encoded_length);
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

    if (frame->wire_profile == UCN_WIRE_PROFILE_UNSPECIFIED) {
        frame->wire_profile = node->tx_wire_profile;
    }
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
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = send_frame_on_link(node, link, &frame);
    if (result == UCN_OK) {
        mark_route_used(node, destination);
    }
    return result;
}

#if UCN_FEATURE_PATH
static ucn_result_t send_control_to_node(ucn_node_t *node,
                                         ucn_node_id_t control_target,
                                         uint8_t message_type,
                                         const uint8_t *payload,
                                         uint16_t payload_length)
{
    ucn_link_t *link;

    if (node == NULL || control_target == 0U ||
        control_target == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    link = find_link(node, control_target);
    if (link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    return send_control_on_link(node, link, control_target, message_type,
                                payload, payload_length);
}
#endif

#if UCN_FEATURE_PATH
static uint32_t path_expires_at(const ucn_node_t *node, uint32_t lease_ms)
{
    return node == NULL ? 0U : ucn_deadline_from_now(node->now_ms, lease_ms);
}

static ucn_result_t install_path_forward_entry(ucn_node_t *node,
                                               ucn_node_id_t owner,
                                               ucn_session_id_t owner_session_id,
                                               ucn_path_id_t path_id,
                                               ucn_node_id_t destination,
                                               ucn_node_id_t next_hop,
                                               uint32_t lease_ms)
{
    ucn_path_forward_config_t config;

    if (node == NULL || owner == 0U || owner == UCN_NODE_BROADCAST ||
        owner_session_id == 0U || path_id == 0U || destination == 0U ||
        destination == UCN_NODE_BROADCAST ||
        !ucn_duration_is_valid(lease_ms)) {
        return UCN_ERR_ARGUMENT;
    }

    (void)memset(&config, 0, sizeof(config));
    config.owner = owner;
    config.owner_session_id = owner_session_id;
    config.path_id = path_id;
    config.destination = destination;
    config.next_hop = next_hop;
    config.expires_at_ms = path_expires_at(node, lease_ms);
    if (next_hop == 0U) {
        if (destination != node->config.node_id) {
            return UCN_ERR_ARGUMENT;
        }
    } else {
        if (destination == node->config.node_id || next_hop == node->config.node_id) {
            return UCN_ERR_ARGUMENT;
        }
        config.egress_link = find_direct_link(node, next_hop);
        if (config.egress_link == NULL) {
            return UCN_ERR_NOT_FOUND;
        }
    }
    return ucn_path_install(&node->path_state, &config);
}
#endif

#if UCN_FEATURE_DIAGNOSTICS
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
    frame.session_id = node->session_id;
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
    frame.session_id = node->session_id;
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

        if (entry->occupied &&
            ucn_deadline_expired(node->now_ms, entry->expires_at_ms)) {
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
        existing->expires_at_ms =
            ucn_deadline_from_now(node->now_ms, UCN_PATH_TRACE_TIMEOUT_MS);
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
    free_slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_PATH_TRACE_TIMEOUT_MS);
    return free_slot;
}

static void expire_path_trace_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_TRACE_REVERSE_DEPTH; ++index) {
        if (node->path_trace_reverse[index].occupied &&
            ucn_deadline_expired(now_ms,
                                 node->path_trace_reverse[index].expires_at_ms)) {
            node->path_trace_reverse[index].occupied = false;
        }
    }
    for (index = 0U; index < UCN_PATH_TRACE_PENDING_DEPTH; ++index) {
        ucn_path_trace_pending_t *pending = &node->path_trace_pending[index];

        if (pending->occupied &&
            ucn_deadline_expired(now_ms, pending->deadline_ms)) {
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
    if (node->path_trace_authorize == NULL ||
        !node->path_trace_authorize(node->path_trace_authorize_context,
                                    frame->source)) {
        node->stats.path_trace_rejected++;
        return UCN_ERR_ACCESS;
    }
    if (!take_control_rx_token(node, ingress_link,
                               UCN_CONTROL_RX_PATH_TRACE_REQUEST)) {
        return UCN_ERR_NO_SPACE;
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
    frame.session_id = node->session_id;
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

        if (entry->occupied &&
            ucn_deadline_expired(node->now_ms, entry->expires_at_ms)) {
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
    free_slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_NODE_SNAPSHOT_TIMEOUT_MS);
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
    {
        uint32_t delay_ms = (query_id ^ node->config.node_id) %
                            (UCN_NODE_SNAPSHOT_REPLY_JITTER_MS + UINT32_C(1));

        free_slot->due_at_ms =
            ucn_deadline_from_now(node->now_ms, delay_ms == 0U ? 1U : delay_ms);
    }
    free_slot->expires_at_ms =
        ucn_deadline_from_now(node->now_ms, UCN_NODE_SNAPSHOT_TIMEOUT_MS);
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
            ucn_deadline_expired(
                now_ms, node->node_snapshot_reverse[index].expires_at_ms)) {
            node->node_snapshot_reverse[index].occupied = false;
        }
    }
    for (index = 0U; index < UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH; ++index) {
        if (node->node_snapshot_replies[index].occupied &&
            ucn_deadline_expired(
                now_ms, node->node_snapshot_replies[index].expires_at_ms)) {
            node->node_snapshot_replies[index].occupied = false;
            node->stats.node_snapshot_rejected++;
        }
    }
    for (index = 0U; index < UCN_NODE_SNAPSHOT_PENDING_DEPTH; ++index) {
        ucn_node_snapshot_pending_t *pending = &node->node_snapshot_pending[index];

        if (pending->occupied &&
            ucn_deadline_expired(now_ms, pending->deadline_ms)) {
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

        if (!pending->occupied ||
            !ucn_deadline_expired(now_ms, pending->due_at_ms)) {
            continue;
        }
        pending->occupied = false;
        result = send_node_snapshot_reply_on_link(node, pending->egress_link,
                                                  pending->origin, pending->query_id);
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

static bool policy_diagnostic_selector_is_valid(uint8_t section, uint8_t index)
{
    switch ((ucn_policy_diagnostic_section_t)section) {
    case UCN_POLICY_DIAGNOSTIC_SUMMARY:
        return index < 3U;
    case UCN_POLICY_DIAGNOSTIC_POLICY:
        return index < UCN_MAX_ROUTE_POLICIES;
    case UCN_POLICY_DIAGNOSTIC_PATH:
        return index < UCN_MAX_POLICY_PATHS;
    case UCN_POLICY_DIAGNOSTIC_FLOW:
        return index < UCN_MAX_POLICY_FLOWS;
    case UCN_POLICY_DIAGNOSTIC_LINK_QUALITY:
        return index < UCN_MAX_LINKS;
    default:
        return false;
    }
}

static bool policy_diagnostic_request_is_valid(const ucn_frame_t *frame)
{
    const uint8_t section = frame == NULL || frame->payload == NULL ? UINT8_MAX :
                            frame->payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET];
    const uint8_t index = frame == NULL || frame->payload == NULL ? 0U :
                          frame->payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET];

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length != UCN_POLICY_DIAGNOSTIC_REQUEST_PAYLOAD_BYTES ||
        frame->traffic_class != UCN_TRAFFIC_Q1_REALTIME ||
        frame->flags != UCN_FRAME_FLAG_DIAGNOSTIC || frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET) == 0U ||
        frame->payload[UCN_POLICY_DIAGNOSTIC_REQUEST_RESERVED_OFFSET] != 0U ||
        frame->payload[UCN_POLICY_DIAGNOSTIC_REQUEST_RESERVED_OFFSET + 1U] != 0U) {
        return false;
    }
    return policy_diagnostic_selector_is_valid(section, index);
}

static bool policy_diagnostic_reply_is_valid(const ucn_frame_t *frame)
{
    const uint8_t section = frame == NULL || frame->payload == NULL ? UINT8_MAX :
                            frame->payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET];
    const uint8_t status = frame == NULL || frame->payload == NULL ? UINT8_MAX :
                           frame->payload[UCN_POLICY_DIAGNOSTIC_REPLY_STATUS_OFFSET];

    if (frame == NULL || frame->payload == NULL ||
        frame->payload_length != UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES ||
        frame->traffic_class != UCN_TRAFFIC_Q1_REALTIME ||
        frame->flags != UCN_FRAME_FLAG_DIAGNOSTIC || frame->has_route_extension ||
        read_u32_be(frame->payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET) == 0U ||
        !policy_diagnostic_selector_is_valid(section,
            frame->payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET]) ||
        status > (uint8_t)UCN_POLICY_DIAGNOSTIC_STATUS_EMPTY ||
        frame->payload[UCN_POLICY_DIAGNOSTIC_REPLY_RESERVED_OFFSET] != 0U) {
        return false;
    }
    if (status == (uint8_t)UCN_POLICY_DIAGNOSTIC_STATUS_EMPTY) {
        return true;
    }
    switch ((ucn_policy_diagnostic_section_t)section) {
    case UCN_POLICY_DIAGNOSTIC_POLICY:
        return frame->payload[UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET + 6U] <=
               (uint8_t)UCN_ROUTE_POLICY_AUTO_BALANCE;
    case UCN_POLICY_DIAGNOSTIC_PATH:
        return frame->payload[UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET + 10U] <=
               (uint8_t)UCN_POLICY_PATH_DOWN;
    case UCN_POLICY_DIAGNOSTIC_FLOW:
        return frame->payload[UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET + 20U] <=
               (uint8_t)UCN_POLICY_PATH_DOWN;
    default:
        return true;
    }
}

static uint8_t policy_diagnostic_quality_flags(
    const ucn_policy_link_quality_snapshot_t *quality)
{
    uint8_t flags = 0U;

    if (quality == NULL) {
        return 0U;
    }
    if (quality->is_up) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_UP;
    }
    if (quality->route_cost_valid) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_ROUTE_COST;
    }
    if (quality->rtt_valid) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_RTT;
    }
    if (quality->tx_failure_rate_valid) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_TX_FAILURE;
    }
    if (quality->queue_pressure_valid) {
        flags |= UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_QUEUE_PRESSURE;
    }
    return flags;
}

static void policy_diagnostic_write_quality(
    uint8_t *record,
    size_t offset,
    const ucn_policy_link_quality_snapshot_t *quality)
{
    if (quality == NULL) {
        return;
    }
    write_u16_be(record + offset, quality->route_cost);
    write_u16_be(record + offset + 2U, quality->rtt_ewma_ms);
    write_u16_be(record + offset + 4U, quality->tx_failure_ewma_per_mille);
    write_u16_be(record + offset + 6U, quality->queue_pressure_ewma_per_mille);
}

static void policy_diagnostic_build_reply(
    const ucn_node_t *node,
    uint32_t request_id,
    ucn_policy_diagnostic_section_t section,
    uint8_t index,
    uint8_t *payload)
{
    uint8_t *record = payload + UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET;
    ucn_policy_diagnostic_status_t status = UCN_POLICY_DIAGNOSTIC_STATUS_EMPTY;

    (void)memset(payload, 0, UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES);
    write_u32_be(payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET, request_id);
    payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET] = (uint8_t)section;
    payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET] = index;

    switch (section) {
    case UCN_POLICY_DIAGNOSTIC_SUMMARY: {
        const ucn_policy_stats_t *stats = &node->policy_state.stats;
        const uint32_t *values = NULL;
        uint32_t page[6];
        size_t value_index;

        switch (index) {
        case 0U:
            page[0] = stats->policy_match_hits;
            page[1] = stats->pinned_strict_sends;
            page[2] = stats->pinned_strict_failures;
            page[3] = stats->pinned_failover_primary_sends;
            page[4] = stats->pinned_failover_backup_sends;
            page[5] = stats->pinned_failover_hard_failures;
            values = page;
            break;
        case 1U:
            page[0] = stats->pinned_failover_discovery_fallbacks;
            page[1] = stats->pinned_policy_config_errors;
            page[2] = stats->auto_balance_sends;
            page[3] = stats->auto_balance_flow_bindings;
            page[4] = stats->auto_balance_rebindings;
            page[5] = stats->auto_balance_congestion_rebindings;
            values = page;
            break;
        default:
            page[0] = stats->auto_balance_down_rebindings;
            page[1] = stats->auto_balance_selection_failures;
            page[2] = stats->flow_bindings_expired;
            page[3] = stats->quality_samples;
            page[4] = stats->quality_metrics_unavailable;
            page[5] = stats->quality_link_down;
            values = page;
            break;
        }
        for (value_index = 0U; value_index < 6U; ++value_index) {
            write_u32_be(record + value_index * sizeof(uint32_t), values[value_index]);
        }
        status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_POLICY: {
        const ucn_route_policy_entry_t *entry =
            &node->policy_state.policies[index];

        if (entry->occupied) {
            write_u32_be(record, entry->config.key.destination);
            record[4] = entry->config.key.endpoint;
            record[5] = entry->config.key.traffic_class;
            record[6] = (uint8_t)entry->config.mode;
            record[7] = entry->config.allow_discovery_on_hard_failure ? 1U : 0U;
            write_u16_be(record + 8U, entry->config.primary_local_path_id);
            write_u16_be(record + 10U, entry->config.backup_local_path_id);
            write_u32_be(record + 12U, entry->config.balance_flow_lease_ms);
            write_u32_be(record + 16U, entry->match_hits);
            status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        }
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_PATH: {
        const ucn_policy_path_entry_t *entry = &node->policy_state.paths[index];

        if (entry->occupied) {
            ucn_link_t *active_bearer = resolve_egress_link((ucn_node_t *)node,
                                                             entry->egress_link);
            const ucn_policy_link_quality_snapshot_t *quality =
                active_bearer == NULL ? NULL :
                ucn_node_get_link_quality(node, active_bearer);

            write_u16_be(record, entry->local_path_id);
            write_u32_be(record + 2U, entry->wire_path_id);
            write_u32_be(record + 6U, entry->destination);
            record[10] = (uint8_t)entry->state;
            record[11] = entry->congestion_samples;
            record[12] = entry->egress_link == NULL ? 0U : entry->egress_link->link_id;
            record[13] = active_bearer == NULL ? 0U : active_bearer->link_id;
            record[14] = policy_diagnostic_quality_flags(quality);
            policy_diagnostic_write_quality(record, 16U, quality);
            status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        }
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_FLOW: {
        const ucn_policy_flow_binding_t *entry = &node->policy_state.flows[index];

        if (entry->occupied &&
            !ucn_deadline_expired(node->now_ms, entry->expires_at_ms)) {
            const ucn_policy_path_entry_t *path =
                ucn_node_find_policy_path(node, entry->local_path_id);
            ucn_link_t *active_bearer = path == NULL ? NULL :
                resolve_egress_link((ucn_node_t *)node, path->egress_link);

            write_u32_be(record, entry->key.destination);
            record[4] = entry->key.endpoint;
            record[5] = (uint8_t)entry->key.traffic_class;
            write_u16_be(record + 6U, entry->local_path_id);
            write_u32_be(record + 8U, entry->expires_at_ms);
            write_u32_be(record + 12U, entry->last_used_at_ms);
            write_u32_be(record + 16U, entry->expires_at_ms - node->now_ms);
            record[20] = path == NULL ? (uint8_t)UCN_POLICY_PATH_EMPTY :
                         (uint8_t)path->state;
            record[21] = active_bearer == NULL ? 0U : active_bearer->link_id;
            status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        }
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_LINK_QUALITY: {
        const ucn_policy_link_quality_snapshot_t *quality =
            &node->policy_state.quality[index];

        if (quality->occupied) {
            record[0] = quality->link == NULL ? 0U : quality->link->link_id;
            record[1] = policy_diagnostic_quality_flags(quality);
            policy_diagnostic_write_quality(record, 4U, quality);
            write_u32_be(record + 12U, quality->sampled_at_ms);
            status = UCN_POLICY_DIAGNOSTIC_STATUS_OK;
        }
        break;
    }
    default:
        break;
    }
    payload[UCN_POLICY_DIAGNOSTIC_REPLY_STATUS_OFFSET] = (uint8_t)status;
}

static void policy_diagnostic_decode_result(
    ucn_policy_diagnostic_result_t *result,
    ucn_node_id_t source,
    const uint8_t *payload)
{
    const uint8_t *record = payload + UCN_POLICY_DIAGNOSTIC_RECORD_OFFSET;
    const uint8_t flags = record[14];

    (void)memset(result, 0, sizeof(*result));
    result->request_id = read_u32_be(payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET);
    result->node_id = source;
    result->section = (ucn_policy_diagnostic_section_t)
        payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET];
    result->index = payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET];
    result->status = (ucn_policy_diagnostic_status_t)
        payload[UCN_POLICY_DIAGNOSTIC_REPLY_STATUS_OFFSET];
    if (result->status != UCN_POLICY_DIAGNOSTIC_STATUS_OK) {
        return;
    }

    switch (result->section) {
    case UCN_POLICY_DIAGNOSTIC_SUMMARY: {
        size_t value_index;

        for (value_index = 0U; value_index < 6U; ++value_index) {
            result->record.summary.counters[value_index] =
                read_u32_be(record + value_index * sizeof(uint32_t));
        }
        break;
    }
    case UCN_POLICY_DIAGNOSTIC_POLICY:
        result->record.policy.key.destination = read_u32_be(record);
        result->record.policy.key.endpoint = record[4];
        result->record.policy.key.traffic_class = record[5];
        result->record.policy.mode = (ucn_route_policy_mode_t)record[6];
        result->record.policy.allow_discovery_on_hard_failure = record[7] != 0U;
        result->record.policy.primary_local_path_id = read_u16_be(record + 8U);
        result->record.policy.backup_local_path_id = read_u16_be(record + 10U);
        result->record.policy.balance_flow_lease_ms = read_u32_be(record + 12U);
        result->record.policy.match_hits = read_u32_be(record + 16U);
        break;
    case UCN_POLICY_DIAGNOSTIC_PATH:
        result->record.path.local_path_id = read_u16_be(record);
        result->record.path.wire_path_id = read_u32_be(record + 2U);
        result->record.path.destination = read_u32_be(record + 6U);
        result->record.path.state = (ucn_policy_path_state_t)record[10];
        result->record.path.congestion_samples = record[11];
        result->record.path.configured_egress_link_id = record[12];
        result->record.path.active_bearer_link_id = record[13];
        result->record.path.active_bearer_is_up =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_UP) != 0U;
        result->record.path.route_cost_valid =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_ROUTE_COST) != 0U;
        result->record.path.rtt_valid =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_RTT) != 0U;
        result->record.path.tx_failure_rate_valid =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_TX_FAILURE) != 0U;
        result->record.path.queue_pressure_valid =
            (flags & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_QUEUE_PRESSURE) != 0U;
        result->record.path.route_cost = read_u16_be(record + 16U);
        result->record.path.rtt_ewma_ms = read_u16_be(record + 18U);
        result->record.path.tx_failure_ewma_per_mille = read_u16_be(record + 20U);
        result->record.path.queue_pressure_ewma_per_mille = read_u16_be(record + 22U);
        break;
    case UCN_POLICY_DIAGNOSTIC_FLOW:
        result->record.flow.key.destination = read_u32_be(record);
        result->record.flow.key.endpoint = record[4];
        result->record.flow.key.traffic_class = (ucn_traffic_class_t)record[5];
        result->record.flow.local_path_id = read_u16_be(record + 6U);
        result->record.flow.expires_at_ms = read_u32_be(record + 8U);
        result->record.flow.last_used_at_ms = read_u32_be(record + 12U);
        result->record.flow.remaining_ms = read_u32_be(record + 16U);
        result->record.flow.path_state = (ucn_policy_path_state_t)record[20];
        result->record.flow.active_bearer_link_id = record[21];
        break;
    case UCN_POLICY_DIAGNOSTIC_LINK_QUALITY:
        result->record.link_quality.link_id = record[0];
        result->record.link_quality.is_up =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_UP) != 0U;
        result->record.link_quality.route_cost_valid =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_ROUTE_COST) != 0U;
        result->record.link_quality.rtt_valid =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_RTT) != 0U;
        result->record.link_quality.tx_failure_rate_valid =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_TX_FAILURE) != 0U;
        result->record.link_quality.queue_pressure_valid =
            (record[1] & UCN_POLICY_DIAGNOSTIC_QUALITY_FLAG_QUEUE_PRESSURE) != 0U;
        result->record.link_quality.route_cost = read_u16_be(record + 4U);
        result->record.link_quality.rtt_ewma_ms = read_u16_be(record + 6U);
        result->record.link_quality.tx_failure_ewma_per_mille = read_u16_be(record + 8U);
        result->record.link_quality.queue_pressure_ewma_per_mille =
            read_u16_be(record + 10U);
        result->record.link_quality.sampled_at_ms = read_u32_be(record + 12U);
        break;
    default:
        break;
    }
}

static ucn_policy_diagnostic_pending_t *find_policy_diagnostic_pending(
    ucn_node_t *node,
    uint32_t request_id)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        if (node->policy_diagnostic_pending[index].occupied &&
            node->policy_diagnostic_pending[index].request_id == request_id) {
            return &node->policy_diagnostic_pending[index];
        }
    }
    return NULL;
}

static ucn_policy_diagnostic_pending_t *find_free_policy_diagnostic_pending(
    ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        if (!node->policy_diagnostic_pending[index].occupied) {
            return &node->policy_diagnostic_pending[index];
        }
    }
    return NULL;
}

static bool queue_policy_diagnostic_reply(ucn_node_t *node,
                                          ucn_node_id_t destination,
                                          const uint8_t *payload)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH; ++index) {
        ucn_policy_diagnostic_reply_pending_t *entry =
            &node->policy_diagnostic_replies[index];

        if (!entry->occupied) {
            (void)memset(entry, 0, sizeof(*entry));
            entry->occupied = true;
            entry->destination = destination;
            entry->expires_at_ms =
                ucn_deadline_from_now(node->now_ms,
                                      UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS);
            (void)memcpy(entry->payload, payload, sizeof(entry->payload));
            return true;
        }
    }
    return false;
}

static void expire_policy_diagnostic_state(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH; ++index) {
        ucn_policy_diagnostic_reply_pending_t *entry =
            &node->policy_diagnostic_replies[index];

        if (entry->occupied &&
            ucn_deadline_expired(now_ms, entry->expires_at_ms)) {
            entry->occupied = false;
            node->stats.policy_diagnostic_rejected++;
        }
    }
    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        ucn_policy_diagnostic_pending_t *pending =
            &node->policy_diagnostic_pending[index];

        if (pending->occupied &&
            ucn_deadline_expired(now_ms, pending->deadline_ms)) {
            ucn_policy_diagnostic_result_t result;
            ucn_policy_diagnostic_handler_t handler = pending->handler;
            void *context = pending->context;

            (void)memset(&result, 0, sizeof(result));
            result.request_id = pending->request_id;
            result.node_id = pending->destination;
            result.section = pending->section;
            result.index = pending->index;
            result.status = UCN_POLICY_DIAGNOSTIC_STATUS_TIMEOUT;
            pending->occupied = false;
            node->stats.policy_diagnostic_timeouts++;
            if (handler != NULL) {
                handler(context, &result);
            }
        }
    }
}

static ucn_result_t send_policy_diagnostic_frame_on_link(
    ucn_node_t *node,
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
    frame.session_id = node->session_id;
    return send_frame_on_link(node, link, &frame);
}

static ucn_result_t send_due_policy_diagnostic_request(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH; ++index) {
        ucn_policy_diagnostic_pending_t *pending =
            &node->policy_diagnostic_pending[index];
        uint8_t payload[UCN_POLICY_DIAGNOSTIC_REQUEST_PAYLOAD_BYTES];
        ucn_link_t *link;

        if (!pending->occupied || pending->sent) {
            continue;
        }
        link = find_link(node, pending->destination);
        if (link == NULL) {
            return UCN_ERR_NOT_FOUND;
        }
        (void)memset(payload, 0, sizeof(payload));
        write_u32_be(payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET,
                     pending->request_id);
        payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET] = (uint8_t)pending->section;
        payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET] = pending->index;
        /* Mark before send: virtual transports can synchronously complete the
         * reply and clear this same slot.  A failed attempt is not retried
         * outside the independent rate budget. */
        pending->sent = true;
        node->stats.policy_diagnostic_requests_sent++;
        return send_policy_diagnostic_frame_on_link(
            node, link, pending->destination, UCN_MSG_POLICY_DIAGNOSTIC_REQ,
            payload, (uint16_t)sizeof(payload));
    }
    return UCN_ERR_NOT_FOUND;
}

static ucn_result_t send_due_policy_diagnostic_reply(ucn_node_t *node)
{
    size_t index;

    for (index = 0U; index < UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH; ++index) {
        ucn_policy_diagnostic_reply_pending_t *pending =
            &node->policy_diagnostic_replies[index];
        ucn_link_t *link;
        ucn_result_t result;

        if (!pending->occupied) {
            continue;
        }
        link = find_link(node, pending->destination);
        if (link == NULL) {
            pending->occupied = false;
            node->stats.policy_diagnostic_rejected++;
            return UCN_ERR_NOT_FOUND;
        }
        pending->occupied = false;
        result = send_policy_diagnostic_frame_on_link(
            node, link, pending->destination, UCN_MSG_POLICY_DIAGNOSTIC_REPLY,
            pending->payload, (uint16_t)sizeof(pending->payload));
        if (result == UCN_OK) {
            node->stats.policy_diagnostic_replies_sent++;
        }
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

static ucn_result_t handle_policy_diagnostic_request(
    ucn_node_t *node,
    const ucn_frame_t *frame)
{
    uint8_t payload[UCN_POLICY_DIAGNOSTIC_REPLY_PAYLOAD_BYTES];
    uint32_t request_id;

    if (!policy_diagnostic_request_is_valid(frame) ||
        frame->destination != node->config.node_id ||
        node->policy_diagnostic_authorize == NULL ||
        !node->policy_diagnostic_authorize(node->policy_diagnostic_authorize_context,
                                           frame->source)) {
        node->stats.policy_diagnostic_rejected++;
        return UCN_ERR_ACCESS;
    }
    if (!take_policy_diagnostic_token(node)) {
        return UCN_ERR_NO_SPACE;
    }
    request_id = read_u32_be(frame->payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET);
    policy_diagnostic_build_reply(node, request_id,
        (ucn_policy_diagnostic_section_t)
            frame->payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET],
        frame->payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET], payload);
    if (!queue_policy_diagnostic_reply(node, frame->source, payload)) {
        node->stats.policy_diagnostic_rejected++;
        return UCN_ERR_NO_SPACE;
    }
    node->stats.policy_diagnostic_requests_received++;
    return UCN_OK;
}

static ucn_result_t handle_policy_diagnostic_reply(
    ucn_node_t *node,
    const ucn_frame_t *frame)
{
    uint32_t request_id;
    ucn_policy_diagnostic_pending_t *pending;
    ucn_policy_diagnostic_result_t result;
    ucn_policy_diagnostic_handler_t handler;
    void *context;

    if (!policy_diagnostic_reply_is_valid(frame) ||
        frame->destination != node->config.node_id) {
        node->stats.policy_diagnostic_rejected++;
        return UCN_ERR_MALFORMED;
    }
    request_id = read_u32_be(frame->payload + UCN_POLICY_DIAGNOSTIC_REQUEST_ID_OFFSET);
    pending = find_policy_diagnostic_pending(node, request_id);
    if (pending == NULL || !pending->sent ||
        pending->destination != frame->source ||
        pending->section != (ucn_policy_diagnostic_section_t)
                                frame->payload[UCN_POLICY_DIAGNOSTIC_SECTION_OFFSET] ||
        pending->index != frame->payload[UCN_POLICY_DIAGNOSTIC_INDEX_OFFSET]) {
        node->stats.policy_diagnostic_rejected++;
        return UCN_ERR_NOT_FOUND;
    }
    policy_diagnostic_decode_result(&result, frame->source, frame->payload);
    handler = pending->handler;
    context = pending->context;
    pending->occupied = false;
    node->stats.policy_diagnostic_replies_received++;
    node->stats.policy_diagnostic_completed++;
    if (handler != NULL) {
        handler(context, &result);
    }
    return UCN_OK;
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

#endif

static void record_max_service_delay(uint32_t *maximum_ms, uint32_t delay_ms)
{
    if (delay_ms > *maximum_ms) {
        *maximum_ms = delay_ms;
    }
}

static ucn_result_t send_due_heartbeat(ucn_node_t *node, uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        size_t bearer_index;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        for (bearer_index = 0U;
             bearer_index < UCN_MAX_BEARERS_PER_NEIGHBOR;
             ++bearer_index) {
            ucn_neighbor_bearer_t *bearer = &entry->bearers[bearer_index];
            uint8_t payload[UCN_HEARTBEAT_PAYLOAD_BYTES];
            uint32_t service_delay_ms = 0U;
            ucn_result_t result;

            if (!bearer_is_active(bearer) || !link_is_usable(bearer->link) ||
                (bearer->heartbeat_sent &&
                 (uint32_t)(now_ms - bearer->last_heartbeat_sent_ms) <
                 UCN_HEARTBEAT_INTERVAL_MS)) {
                continue;
            }
            if (bearer->heartbeat_sent) {
                service_delay_ms =
                    (uint32_t)(now_ms - bearer->last_heartbeat_sent_ms) -
                    UCN_HEARTBEAT_INTERVAL_MS;
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
            result = send_control_on_link(node, bearer->link, entry->peer_node_id,
                                          UCN_MSG_HEARTBEAT, payload,
                                          (uint16_t)sizeof(payload));
            if (result == UCN_OK) {
                bearer->heartbeat_sent = true;
                bearer->last_heartbeat_sent_ms = now_ms;
                node->stats.heartbeat_requests_sent++;
                record_max_service_delay(
                    &node->stats.max_heartbeat_service_delay_ms,
                    service_delay_ms);
            }
            return result;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

/* Reuse the one-hop HEARTBEAT request/ACK wire format as a fixed-size Bearer
 * quality probe.  The candidate is addressed on its own Link, so a relay
 * never needs to parse or forward it.  State is installed before the send:
 * virtual Links and some Drivers may synchronously deliver the ACK. */
static ucn_result_t send_due_bearer_quality_probe(ucn_node_t *node,
                                                   uint32_t now_ms)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        ucn_neighbor_entry_t *entry = &node->neighbors[index];
        ucn_neighbor_bearer_t *primary;
        ucn_neighbor_bearer_t *candidate;
        uint8_t payload[UCN_HEARTBEAT_PAYLOAD_BYTES];
        uint32_t probe_id;
        uint32_t service_delay_ms;
        ucn_result_t result;

        if (entry->state != UCN_NEIGHBOR_ADMITTED &&
            entry->state != UCN_NEIGHBOR_SUSPECT) {
            continue;
        }
        primary = select_neighbor_bearer(entry);
        candidate = find_better_neighbor_bearer(entry, primary);
        if (candidate == NULL ||
            candidate->quality_better_samples <
                UCN_BEARER_QUALITY_STABLE_SAMPLES ||
            candidate->quality_probe_acks >=
                UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS ||
            candidate->quality_probes_sent >=
                UCN_BEARER_QUALITY_PROBE_MAX_ATTEMPTS) {
            continue;
        }
        if (candidate->quality_probe_pending) {
            const uint32_t elapsed_ms =
                (uint32_t)(now_ms - candidate->quality_probe_sent_at_ms);

            if (elapsed_ms <
                UCN_BEARER_QUALITY_PROBE_INTERVAL_MS) {
                continue;
            }
            service_delay_ms =
                elapsed_ms - UCN_BEARER_QUALITY_PROBE_INTERVAL_MS;
            candidate->quality_probe_pending = false;
        } else if (candidate->quality_probes_sent == 0U) {
            service_delay_ms =
                (uint32_t)(now_ms - entry->last_bearer_quality_sample_ms);
        } else {
            /* An ACK makes the next required Probe immediately eligible.
             * Using the previous send time is conservative if that ACK was
             * delivered asynchronously after the send. */
            service_delay_ms =
                (uint32_t)(now_ms - candidate->quality_probe_sent_at_ms);
        }
        if (node->next_heartbeat_id == 0U) {
            node->next_heartbeat_id = 1U;
        }
        if (!take_control_token(node)) {
            return UCN_ERR_NO_SPACE;
        }
        probe_id = node->next_heartbeat_id++;
        (void)memset(payload, 0, sizeof(payload));
        payload[0] = UCN_HEARTBEAT_REQUEST;
        write_u32_be(payload + 4U, probe_id);
        candidate->quality_probe_id = probe_id;
        candidate->quality_probe_sent_at_ms = now_ms;
        candidate->quality_probe_pending = true;
        candidate->quality_probes_sent++;
        result = send_control_on_link(node, candidate->link,
                                      entry->peer_node_id,
                                      UCN_MSG_HEARTBEAT, payload,
                                      (uint16_t)sizeof(payload));
        if (result == UCN_OK) {
            node->stats.bearer_quality_probes_sent++;
            record_max_service_delay(&node->stats.max_probe_service_delay_ms,
                                     service_delay_ms);
        } else {
            candidate->quality_probe_pending = false;
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
    ucn_neighbor_bearer_t *bearer = find_neighbor_bearer(entry, ingress_link);
    uint8_t response[UCN_HEARTBEAT_PAYLOAD_BYTES];
    ucn_result_t result;

    if (entry == NULL || bearer == NULL || !bearer_is_active(bearer)) {
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

    if (frame->payload[0] == UCN_HEARTBEAT_REQUEST &&
        !take_control_rx_token(node, ingress_link,
                               UCN_CONTROL_RX_HEARTBEAT_REQUEST)) {
        return UCN_ERR_NO_SPACE;
    }

    touch_neighbor(node, ingress_link);
    node->stats.heartbeat_received++;
    if (frame->payload[0] == UCN_HEARTBEAT_ACK) {
        if (bearer->quality_probe_pending &&
            bearer->quality_probe_id == read_u32_be(frame->payload + 4U)) {
            bearer->quality_probe_pending = false;
            if (bearer->quality_probe_acks <
                UCN_BEARER_QUALITY_PROBE_REQUIRED_ACKS) {
                bearer->quality_probe_acks++;
                node->stats.bearer_quality_probe_acks_received++;
            }
        }
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

#if UCN_FEATURE_CANDIDATE_ROUTING
static ucn_link_t *find_candidate_link(ucn_node_t *node,
                                       ucn_node_id_t destination,
                                       uint32_t candidate_id)
{
    ucn_candidate_route_t *candidate =
        find_candidate_route(node, destination, candidate_id);
    ucn_link_t *egress_link;

    if (candidate == NULL) {
        return NULL;
    }
    egress_link = resolve_egress_link(node, candidate->egress_link);
    return egress_link == NULL || !link_is_candidate_eligible(node, egress_link) ?
           NULL : egress_link;
}

static uint16_t allocate_route_epoch(ucn_node_t *node, ucn_node_id_t destination)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(node->tx_wire_profile);
    const uint16_t maximum =
        descriptor != NULL && descriptor->route_epoch_bytes == 1U ?
            UINT16_C(0x00FF) : UINT16_MAX;
    ucn_route_entry_t *route = find_active_route(node, destination);

    for (;;) {
        uint16_t route_epoch;

        if (node->next_route_epoch == 0U ||
            node->next_route_epoch >= maximum) {
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
        ucn_link_t *egress_link;
        uint32_t service_delay_ms;
        ucn_result_t result;

        if (!candidate->valid || !candidate->originated_here) {
            continue;
        }
        egress_link = find_candidate_link(node, candidate->destination,
                                          candidate->candidate_id);
        if (egress_link == NULL) {
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
            result = send_control_on_link(node, egress_link,
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
             !ucn_deadline_expired(now_ms, candidate->next_probe_at_ms))) {
            continue;
        }

        service_delay_ms = candidate->next_probe_at_ms == 0U ?
            0U : (uint32_t)(now_ms - candidate->next_probe_at_ms);

        if (!take_control_token(node)) {
            return UCN_ERR_NO_SPACE;
        }

        write_u32_be(payload, candidate->candidate_id);
        write_u32_be(payload + 4U, (uint32_t)candidate->probes_sent + 1U);
        write_u32_be(payload + 8U, now_ms);
        result = send_control_on_link(node, egress_link,
                                      candidate->destination, UCN_MSG_PATH_PROBE,
                                      payload, (uint16_t)sizeof(payload));
        if (result == UCN_OK) {
            candidate->probes_sent++;
            candidate->next_probe_at_ms =
                ucn_deadline_from_now(now_ms, UCN_PATH_PROBE_INTERVAL_MS);
            node->stats.path_probes_sent++;
            record_max_service_delay(&node->stats.max_probe_service_delay_ms,
                                     service_delay_ms);
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
        candidate->expires_at_ms =
            ucn_deadline_from_now(node->now_ms,
                                  UCN_ROUTE_CANDIDATE_TIMEOUT_MS);
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
#endif

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
    if (frame->payload_length != route_request_payload_size(
                                     frame->wire_profile)) {
        return UCN_ERR_MALFORMED;
    }
    route_cost = read_u16_be(frame->payload + route_request_cost_offset(frame));
    hop_count = frame->payload[route_request_hop_offset(frame)];
    if (hop_count == UINT8_MAX) {
        return UCN_ERR_TTL;
    }
    --forwarded.hop_limit;

    for (index = 0U; index < node->link_count; ++index) {
        uint8_t payload[UCN_ROUTE_REQ_MAX_PAYLOAD_BYTES];
        ucn_result_t result;

        if (node->links[index] == ingress_link) {
            continue;
        }
#if UCN_FEATURE_CANDIDATE_ROUTING
        if ((frame->payload[route_request_flags_offset(frame)] &
             UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U &&
            !link_is_candidate_eligible(node, node->links[index])) {
            continue;
        }
#endif
        (void)memcpy(payload, frame->payload, frame->payload_length);
        write_u16_be(payload + route_request_cost_offset(frame),
                     add_route_cost(route_cost, link_route_cost(node->links[index])));
        payload[route_request_hop_offset(frame)] = (uint8_t)(hop_count + 1U);
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

#if UCN_FEATURE_PATH
static ucn_result_t send_path_route_error(ucn_node_t *node,
                                          ucn_link_t *upstream_link,
                                          ucn_node_id_t origin,
                                          ucn_node_id_t unreachable,
                                          ucn_session_id_t owner_session_id,
                                          ucn_path_id_t path_id)
{
    uint8_t payload[UCN_PATH_ROUTE_ERROR_PAYLOAD_BYTES];
    ucn_result_t result;

    if (upstream_link == NULL || origin == 0U || origin == UCN_NODE_BROADCAST ||
        unreachable == 0U || unreachable == UCN_NODE_BROADCAST ||
        owner_session_id == 0U || path_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }

    write_u32_be(payload, unreachable);
    write_u32_be(payload + 4U, origin);
    write_u32_be(payload + 8U, owner_session_id);
    write_u32_be(payload + 12U, path_id);
    result = send_control_on_link(node, upstream_link, origin, UCN_MSG_ROUTE_ERROR,
                                  payload, (uint16_t)sizeof(payload));
    if (result == UCN_OK) {
        node->stats.route_errors_sent++;
        node->stats.path_route_errors_sent++;
    }
    return result;
}
#endif

static void invalidate_route_to(ucn_node_t *node, ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid && !node->routes[index].is_static &&
            node->routes[index].destination == destination) {
            node->routes[index].valid = false;
        }
    }
#if UCN_FEATURE_CANDIDATE_ROUTING
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        if (node->candidates[index].valid &&
            node->candidates[index].destination == destination) {
            node->candidates[index].valid = false;
        }
    }
#endif
    clear_discovery(node, destination);
}

static ucn_result_t validate_route_request_frame(ucn_node_t *node,
                                                 ucn_link_t *ingress_link,
                                                 const ucn_frame_t *frame)
{
    ucn_node_id_t target;
    uint32_t request_id;
    bool is_candidate;

    if (frame->payload_length != route_request_payload_size(
                                     frame->wire_profile)) {
        return UCN_ERR_MALFORMED;
    }
    target = route_request_target(frame);
    request_id = read_u32_be(frame->payload + route_request_id_offset(frame));
    is_candidate =
        (frame->payload[route_request_flags_offset(frame)] &
         UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U;
    if (frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        target == 0U || target == UCN_NODE_BROADCAST || request_id == 0U ||
        (frame->payload[route_request_flags_offset(frame)] &
         (uint8_t)~UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U) {
        return UCN_ERR_MALFORMED;
    }
#if !UCN_FEATURE_CANDIDATE_ROUTING
    (void)node;
    (void)ingress_link;
    if (is_candidate) {
        return UCN_ERR_CONFIG;
    }
#else
    if (is_candidate && !link_is_candidate_eligible(node, ingress_link)) {
        return UCN_ERR_ACCESS;
    }
#endif
    return UCN_OK;
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

    result = validate_route_request_frame(node, ingress_link, frame);
    if (result != UCN_OK) {
        return result;
    }
    origin = frame->source;
    target = route_request_target(frame);
    request_id = read_u32_be(frame->payload + route_request_id_offset(frame));
    route_cost = read_u16_be(frame->payload + route_request_cost_offset(frame));
    hop_count = frame->payload[route_request_hop_offset(frame)];
    is_candidate =
        (frame->payload[route_request_flags_offset(frame)] &
         UCN_ROUTE_REQ_FLAG_CANDIDATE) != 0U;

#if UCN_FEATURE_CANDIDATE_ROUTING
    result = is_candidate ?
             learn_candidate_route(node, origin, request_id, ingress_link,
                                   route_cost, hop_count, false) :
             learn_route(node, origin, ingress_link, route_cost, hop_count,
                         route_epoch_from_request_id(frame->wire_profile,
                                                     request_id));
#else
    result = learn_route(node, origin, ingress_link, route_cost, hop_count,
                         route_epoch_from_request_id(frame->wire_profile,
                                                     request_id));
#endif
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
        /* A ROUTE_REPLY advertises distance from the current reply sender to
         * the target, not the completed RREQ's origin-to-target distance.
         * Starting at zero lets every return hop add its own outbound Link
         * Cost/Hop.  Stored route metrics then strictly decrease toward the
         * target, which is the bounded AODV-Lite loop-freedom invariant. */
        write_u16_be(reply + 12U, 0U);
        reply[14] = 0U;
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
    if (hop_count == UINT8_MAX) {
        return UCN_ERR_TTL;
    }
    route_cost = add_route_cost(route_cost, link_route_cost(ingress_link));
    hop_count++;
#if !UCN_FEATURE_CANDIDATE_ROUTING
    if (is_candidate) {
        return UCN_ERR_CONFIG;
    }
#else
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
#endif

#if UCN_FEATURE_CANDIDATE_ROUTING
    result = is_candidate ?
             learn_candidate_route(node, target, request_id, ingress_link,
                                   route_cost, hop_count, false) :
             learn_route(node, target, ingress_link, route_cost, hop_count,
                         route_epoch);
#else
    result = learn_route(node, target, ingress_link, route_cost, hop_count,
                         route_epoch);
#endif
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
    bool path_scoped = false;

    *consumed = false;
    if (frame->payload_length != UCN_ROUTE_ERROR_PAYLOAD_BYTES &&
        frame->payload_length != UCN_PATH_ROUTE_ERROR_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    unreachable = read_u32_be(frame->payload);
    if (unreachable == 0U || unreachable == UCN_NODE_BROADCAST) {
        return UCN_ERR_MALFORMED;
    }
    path_scoped = frame->payload_length == UCN_PATH_ROUTE_ERROR_PAYLOAD_BYTES;
    if (path_scoped) {
#if UCN_FEATURE_PATH
        const ucn_node_id_t owner = read_u32_be(frame->payload + 4U);
        const ucn_session_id_t owner_session_id = read_u32_be(frame->payload + 8U);
        const ucn_path_id_t path_id = read_u32_be(frame->payload + 12U);

        if (owner == 0U || owner == UCN_NODE_BROADCAST ||
            owner != frame->destination || owner_session_id == 0U || path_id == 0U) {
            return UCN_ERR_MALFORMED;
        }
        revoke_path_and_mark_local_policy(node, owner, owner_session_id,
                                           path_id, unreachable);
#else
        return UCN_ERR_CONFIG;
#endif
    } else {
        invalidate_route_to(node, unreachable);
    }
    if (frame->destination == node->config.node_id) {
        *consumed = true;
    }
    return UCN_OK;
}

#if UCN_FEATURE_PATH
static ucn_result_t authorize_path_control(ucn_node_t *node,
                                           ucn_link_t *ingress_link,
                                           const ucn_frame_t *frame,
                                           ucn_path_control_operation_t operation,
                                           ucn_path_id_t path_id,
                                           ucn_node_id_t destination,
                                           ucn_node_id_t next_hop)
{
    if (node->security_ops == NULL || node->path_control_authorize == NULL) {
        return UCN_ERR_ACCESS;
    }
    return node->path_control_authorize(node->path_control_authorize_context,
                                        ingress_link, frame, operation, path_id,
                                        destination, next_hop);
}

typedef enum path_control_budget_take_result {
    PATH_CONTROL_BUDGET_TAKEN = 0,
    PATH_CONTROL_BUDGET_RATE_LIMITED = 1,
    PATH_CONTROL_BUDGET_SOURCE_FULL = 2
} path_control_budget_take_result_t;

static void note_path_control_authorization_rejected(
    ucn_node_t *node,
    uint8_t message_type)
{
    if (message_type == UCN_MSG_PATH_INSTALL) {
        node->stats.path_install_authorization_rejected++;
    } else if (message_type == UCN_MSG_PATH_REVOKE) {
        node->stats.path_revoke_authorization_rejected++;
    }
}

static void initialize_path_control_source_budget(
    ucn_path_control_source_budget_t *budget,
    ucn_node_id_t source,
    ucn_session_id_t session_id,
    uint32_t now_ms)
{
    size_t index;

    (void)memset(budget, 0, sizeof(*budget));
    budget->occupied = true;
    budget->source = source;
    budget->session_id = session_id;
    budget->last_seen_ms = now_ms;
    for (index = 0U; index < UCN_PATH_CONTROL_OPERATION_COUNT; ++index) {
        budget->tokens[index] = UCN_PATH_CONTROL_RX_TOKEN_BURST;
        budget->last_refill_ms[index] = now_ms;
    }
}

static ucn_path_control_source_budget_t *find_path_control_source_budget(
    ucn_node_t *node,
    ucn_node_id_t source,
    ucn_session_id_t session_id)
{
    ucn_path_control_source_budget_t *same_source = NULL;
    ucn_path_control_source_budget_t *free_slot = NULL;
    size_t index;

    /* An inactive authenticated source cannot occupy one of the fixed slots
     * forever.  Reclamation is lazy and therefore adds no periodic scan. */
    for (index = 0U; index < UCN_PATH_CONTROL_RX_SOURCE_DEPTH; ++index) {
        ucn_path_control_source_budget_t *slot =
            &node->path_control_source_budgets[index];

        if (slot->occupied &&
            ucn_elapsed_at_least(node->now_ms, slot->last_seen_ms,
                                 UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS)) {
            (void)memset(slot, 0, sizeof(*slot));
            node->stats.path_control_budget_sources_reclaimed++;
        }
    }

    for (index = 0U; index < UCN_PATH_CONTROL_RX_SOURCE_DEPTH; ++index) {
        ucn_path_control_source_budget_t *slot =
            &node->path_control_source_budgets[index];

        if (slot->occupied && slot->source == source &&
            slot->session_id == session_id) {
            return slot;
        }
        if (slot->occupied && slot->source == source) {
            same_source = slot;
        } else if (!slot->occupied && free_slot == NULL) {
            free_slot = slot;
        }
    }

    /* Security and the product authorizer have already accepted this frame.
     * A new authenticated Session therefore replaces the old generation for
     * the same source instead of leaking one fixed slot per key rotation. */
    if (same_source != NULL) {
        initialize_path_control_source_budget(same_source, source, session_id,
                                              node->now_ms);
        node->stats.path_control_budget_session_rotations++;
        return same_source;
    }
    if (free_slot != NULL) {
        initialize_path_control_source_budget(free_slot, source, session_id,
                                              node->now_ms);
    }
    return free_slot;
}

static path_control_budget_take_result_t take_path_control_source_token(
    ucn_node_t *node,
    const ucn_frame_t *frame,
    ucn_path_control_operation_t operation)
{
    ucn_path_control_source_budget_t *source_budget;
    uint8_t *tokens;
    uint32_t *last_refill_ms;
    uint32_t elapsed;
    uint32_t refill_count;

    source_budget = find_path_control_source_budget(node, frame->source,
                                                    frame->session_id);
    if (source_budget == NULL) {
        return PATH_CONTROL_BUDGET_SOURCE_FULL;
    }

    tokens = &source_budget->tokens[operation];
    last_refill_ms = &source_budget->last_refill_ms[operation];
    elapsed = node->now_ms - *last_refill_ms;
    refill_count = elapsed / UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS;
    if (refill_count != 0U) {
        const uint32_t missing =
            (uint32_t)UCN_PATH_CONTROL_RX_TOKEN_BURST - (uint32_t)*tokens;

        if (refill_count >= missing) {
            *tokens = UCN_PATH_CONTROL_RX_TOKEN_BURST;
        } else {
            *tokens = (uint8_t)((uint32_t)*tokens + refill_count);
        }
        *last_refill_ms += refill_count * UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS;
    }
    source_budget->last_seen_ms = node->now_ms;
    if (*tokens == 0U) {
        return PATH_CONTROL_BUDGET_RATE_LIMITED;
    }
    --(*tokens);
    return PATH_CONTROL_BUDGET_TAKEN;
}

static ucn_result_t handle_path_install(ucn_node_t *node,
                                        ucn_link_t *ingress_link,
                                        const ucn_frame_t *frame)
{
    ucn_path_id_t path_id;
    ucn_node_id_t destination;
    ucn_node_id_t next_hop;
    uint32_t lease_ms;
    ucn_result_t result;

    if (frame->payload_length != UCN_PATH_INSTALL_PAYLOAD_BYTES ||
        frame->destination != node->config.node_id || frame->source == 0U ||
        frame->source == UCN_NODE_BROADCAST || frame->session_id == 0U) {
        return UCN_ERR_MALFORMED;
    }
    path_id = read_u32_be(frame->payload);
    destination = read_u32_be(frame->payload + 4U);
    next_hop = read_u32_be(frame->payload + 8U);
    lease_ms = read_u32_be(frame->payload + 12U);
    if (path_id == 0U || destination == 0U || destination == UCN_NODE_BROADCAST ||
        next_hop == UCN_NODE_BROADCAST ||
        !ucn_duration_is_valid(lease_ms) ||
        (next_hop == 0U && destination != node->config.node_id)) {
        return UCN_ERR_MALFORMED;
    }
    result = authorize_path_control(node, ingress_link, frame,
                                    UCN_PATH_CONTROL_INSTALL, path_id,
                                    destination, next_hop);
    if (result != UCN_OK) {
        node->stats.path_install_authorization_rejected++;
        return result;
    }
    {
        const path_control_budget_take_result_t budget_result =
            take_path_control_source_token(node, frame,
                                           UCN_PATH_CONTROL_INSTALL);

        if (budget_result == PATH_CONTROL_BUDGET_SOURCE_FULL) {
            node->stats.path_control_budget_source_full++;
            return UCN_ERR_NO_SPACE;
        }
        if (budget_result == PATH_CONTROL_BUDGET_RATE_LIMITED) {
            node->stats.path_install_budget_rejected++;
            return UCN_ERR_NO_SPACE;
        }
    }
    result = install_path_forward_entry(node, frame->source, frame->session_id,
                                        path_id, destination, next_hop, lease_ms);
    if (result == UCN_OK) {
        node->stats.path_installs_received++;
    } else if (result == UCN_ERR_NO_SPACE) {
        node->stats.path_install_table_full++;
    }
    return result;
}

static ucn_result_t handle_path_revoke(ucn_node_t *node,
                                       ucn_link_t *ingress_link,
                                       const ucn_frame_t *frame)
{
    ucn_path_id_t path_id;
    ucn_node_id_t destination;
    ucn_result_t result;

    if (frame->payload_length != UCN_PATH_REVOKE_PAYLOAD_BYTES ||
        frame->destination != node->config.node_id || frame->source == 0U ||
        frame->source == UCN_NODE_BROADCAST || frame->session_id == 0U) {
        return UCN_ERR_MALFORMED;
    }
    path_id = read_u32_be(frame->payload);
    destination = read_u32_be(frame->payload + 4U);
    if (path_id == 0U || destination == 0U || destination == UCN_NODE_BROADCAST) {
        return UCN_ERR_MALFORMED;
    }
    result = authorize_path_control(node, ingress_link, frame,
                                    UCN_PATH_CONTROL_REVOKE, path_id,
                                    destination, 0U);
    if (result != UCN_OK) {
        node->stats.path_revoke_authorization_rejected++;
        return result;
    }
    {
        const path_control_budget_take_result_t budget_result =
            take_path_control_source_token(node, frame,
                                           UCN_PATH_CONTROL_REVOKE);

        if (budget_result == PATH_CONTROL_BUDGET_SOURCE_FULL) {
            node->stats.path_control_budget_source_full++;
            return UCN_ERR_NO_SPACE;
        }
        if (budget_result == PATH_CONTROL_BUDGET_RATE_LIMITED) {
            node->stats.path_revoke_budget_rejected++;
            return UCN_ERR_NO_SPACE;
        }
    }
    result = ucn_path_revoke(&node->path_state, frame->source, frame->session_id,
                             path_id, destination);
    if (result != UCN_OK && result != UCN_ERR_NOT_FOUND) {
        return result;
    }
    node->stats.path_revokes_received++;
    return UCN_OK;
}
#endif

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

    peer_node_id = frame->source;
    if (frame->source == 0U || frame->source == UCN_NODE_BROADCAST ||
        peer_node_id == node->config.node_id ||
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
    node->tx_wire_profile = UCN_WIRE_PROFILE_W3_BACKBONE;
    node->max_receive_wire_profile = UCN_WIRE_PROFILE_W3_BACKBONE;
    node->next_sequence = 1U;
    node->next_queue_order = 1U;
    node->next_route_request_id = 1U;
    node->next_route_epoch = 1U;
    node->next_heartbeat_id = 1U;
    node->control_tokens = UCN_CONTROL_TOKEN_BURST;
#if UCN_FEATURE_DIAGNOSTICS
    node->next_path_trace_id = 1U;
    node->next_node_snapshot_id = 1U;
    node->next_policy_diagnostic_id = 1U;
    node->path_trace_tokens = UCN_PATH_TRACE_TOKEN_BURST;
    node->node_snapshot_tokens = UCN_NODE_SNAPSHOT_TOKEN_BURST;
    node->policy_diagnostic_tokens = UCN_POLICY_DIAGNOSTIC_TOKEN_BURST;
#endif
    node->security_policy.tx_mode = UCN_SECURITY_TX_PLAIN;
    node->security_policy.rx_mode = UCN_SECURITY_RX_BOTH;
    node->security_policy.forward_mode = UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E;
    node->security_required = UCN_SECURITY_REQUIRED_BY_DEFAULT != 0;
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
    if (node->link_count != 0U || node->security_ops != NULL) {
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

ucn_result_t ucn_node_set_plain_session_id(ucn_node_t *node,
                                           ucn_session_id_t session_id)
{
    if (node == NULL || session_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops != NULL) {
        return UCN_ERR_CONFIG;
    }
    if (session_id > ucn_wire_profile_get_descriptor(
                         node->tx_wire_profile)->max_wire_value) {
        return UCN_ERR_TOO_LARGE;
    }
    node->session_id = session_id;
    return UCN_OK;
}

bool ucn_node_security_ready(const ucn_node_t *node)
{
    size_t index;

    if (node == NULL) {
        return false;
    }
    if (!node->security_required) {
        return true;
    }
    if (node->security_ops == NULL || node->session_id == 0U ||
        node->security_ops->authorize_tx == NULL ||
        node->security_ops->authorize_rx == NULL ||
        node->security_ops->seal == NULL || node->security_ops->open == NULL ||
        !security_policy_is_production_ready(&node->security_policy)) {
        return false;
    }
    for (index = 0U; index < UCN_MAX_ENDPOINT_SECURITY_POLICIES; ++index) {
        if (node->endpoint_security_policies[index].occupied &&
            !security_policy_is_production_ready(
                &node->endpoint_security_policies[index].policy)) {
            return false;
        }
    }
    return true;
}

ucn_result_t ucn_node_set_security_required(ucn_node_t *node, bool required)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->security_required = required;
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
    if (node->security_required && (ops->seal == NULL || ops->open == NULL)) {
        return UCN_ERR_SECURITY;
    }

    result = ops->load_next_sequence(context, &next_sequence);
    if (result != UCN_OK || next_sequence == 0U || next_sequence == UINT32_MAX) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    result = ops->get_session_id(context, &session_id);
    if (result != UCN_OK || session_id == 0U) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    if (session_id > ucn_wire_profile_get_descriptor(
                         node->tx_wire_profile)->max_wire_value) {
        return UCN_ERR_TOO_LARGE;
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

#if UCN_FEATURE_DIAGNOSTICS
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

ucn_result_t ucn_node_set_path_trace_authorizer(
    ucn_node_t *node,
    ucn_path_trace_authorize_fn authorize,
    void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->path_trace_authorize = authorize;
    node->path_trace_authorize_context = context;
    return UCN_OK;
}

ucn_result_t ucn_node_set_policy_diagnostic_authorizer(
    ucn_node_t *node,
    ucn_policy_diagnostic_authorize_fn authorize,
    void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->policy_diagnostic_authorize = authorize;
    node->policy_diagnostic_authorize_context = context;
    return UCN_OK;
}
#endif

#if UCN_FEATURE_PATH
ucn_result_t ucn_node_set_path_control_authorizer(
    ucn_node_t *node,
    ucn_path_control_authorize_fn authorize,
    void *context)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    node->path_control_authorize = authorize;
    node->path_control_authorize_context = context;
    return UCN_OK;
}
#endif

ucn_result_t ucn_node_observe_neighbor(ucn_node_t *node,
                                       ucn_link_t *link,
                                       uint32_t now_ms)
{
    ucn_neighbor_entry_t *entry;
    ucn_neighbor_bearer_t *bearer;
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
        bearer = find_neighbor_bearer(entry, link);
        if (bearer != NULL && bearer_is_active(bearer)) {
            bearer->last_seen_ms = now_ms;
            bearer->state = UCN_NEIGHBOR_BEARER_ADMITTED;
            entry->state = UCN_NEIGHBOR_ADMITTED;
            entry->suspect_since_ms = 0U;
            (void)select_neighbor_bearer(entry);
            return UCN_OK;
        }
        if (bearer != NULL && bearer->state == UCN_NEIGHBOR_BEARER_CANDIDATE) {
            if (entry->state == UCN_NEIGHBOR_REJECTED ||
                entry->state == UCN_NEIGHBOR_EXPIRED ||
                entry->state == UCN_NEIGHBOR_REMOVED) {
                entry->state = UCN_NEIGHBOR_CANDIDATE;
            }
            bearer->last_seen_ms = now_ms;
        } else if (bearer != NULL) {
            (void)memset(bearer, 0, sizeof(*bearer));
            bearer->state = UCN_NEIGHBOR_BEARER_CANDIDATE;
            bearer->link = link;
            bearer->last_seen_ms = now_ms;
        } else if (entry->state == UCN_NEIGHBOR_ADMITTED ||
                   entry->state == UCN_NEIGHBOR_SUSPECT ||
                   entry->state == UCN_NEIGHBOR_CANDIDATE) {
            bearer = allocate_neighbor_bearer(entry);
            if (bearer == NULL) {
                return UCN_ERR_NO_SPACE;
            }
            (void)memset(bearer, 0, sizeof(*bearer));
            bearer->state = UCN_NEIGHBOR_BEARER_CANDIDATE;
            bearer->link = link;
            bearer->last_seen_ms = now_ms;
            ++entry->bearer_count;
        } else {
            (void)memset(entry, 0, sizeof(*entry));
            entry->state = UCN_NEIGHBOR_CANDIDATE;
            entry->peer_node_id = link->peer_node_id;
            entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
            bearer = &entry->bearers[0];
            bearer->state = UCN_NEIGHBOR_BEARER_CANDIDATE;
            bearer->link = link;
            bearer->last_seen_ms = now_ms;
            entry->bearer_count = 1U;
        }
    } else {
        entry = allocate_neighbor_slot(node);
        if (entry == NULL) {
            return UCN_ERR_NO_SPACE;
        }
        (void)memset(entry, 0, sizeof(*entry));
        entry->state = UCN_NEIGHBOR_CANDIDATE;
        entry->peer_node_id = link->peer_node_id;
        entry->primary_bearer_index = UCN_NEIGHBOR_PRIMARY_BEARER_NONE;
        bearer = &entry->bearers[0];
        bearer->state = UCN_NEIGHBOR_BEARER_CANDIDATE;
        bearer->link = link;
        bearer->last_seen_ms = now_ms;
        entry->bearer_count = 1U;
    }

    if (node->join_policy == UCN_JOIN_MANUAL) {
        return UCN_OK;
    }
    if (node->join_policy == UCN_JOIN_PROVIDER) {
        result = node->neighbor_authorize(node->neighbor_authorize_context,
                                          node->config.node_id,
                                          entry->peer_node_id,
                                          bearer->link);
        if (result != UCN_OK) {
            if (entry->state == UCN_NEIGHBOR_CANDIDATE) {
                entry->state = UCN_NEIGHBOR_REJECTED;
            } else {
                (void)memset(bearer, 0, sizeof(*bearer));
                --entry->bearer_count;
            }
            return result;
        }
    }
    result = admit_neighbor_entry(node, entry);
    if (result == UCN_OK) {
        (void)select_neighbor_bearer(entry);
    }
    return result;
}

ucn_result_t ucn_node_probe_neighbor(ucn_node_t *node,
                                     ucn_link_t *link,
                                     uint32_t now_ms)
{
    ucn_result_t result;

    result = ucn_node_observe_neighbor(node, link, now_ms);
    if (result != UCN_OK) {
        return result;
    }

    node->now_ms = now_ms;
    return send_control_on_link(node, link, link->peer_node_id, UCN_MSG_HELLO,
                                NULL, 0U);
}

ucn_result_t ucn_node_broadcast_hello(ucn_node_t *node,
                                      ucn_link_t *link,
                                      uint32_t now_ms)
{
    if (node == NULL || link == NULL || link->ops == NULL ||
        link->ops->send == NULL || link->ops->get_status == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    node->now_ms = now_ms;
    return send_control_on_link(node, link, UCN_NODE_BROADCAST, UCN_MSG_HELLO,
                                NULL, 0U);
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
    if (entry->state == UCN_NEIGHBOR_ADMITTED ||
        entry->state == UCN_NEIGHBOR_SUSPECT) {
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
        link->mtu < ucn_frame_header_size_for_profile(
                        node->max_receive_wire_profile, 0U)) {
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
    uint8_t payload[UCN_ROUTE_REQ_MAX_PAYLOAD_BYTES];
    ucn_frame_t frame;
    size_t index;
    ucn_result_t result;

    if (node == NULL || destination == 0U || destination == UCN_NODE_BROADCAST ||
        destination == node->config.node_id) {
        return UCN_ERR_ARGUMENT;
    }
    if (destination > ucn_wire_profile_get_descriptor(
                          node->tx_wire_profile)->max_node_id) {
        return UCN_ERR_TOO_LARGE;
    }

    expire_dynamic_state(node, now_ms);
#if !UCN_FEATURE_CANDIDATE_ROUTING
    if (is_candidate) {
        return UCN_ERR_CONFIG;
    }
#else
    if (!is_candidate && find_link(node, destination) != NULL) {
        return UCN_OK;
    }
    if (is_candidate) {
        ucn_route_entry_t *active_route = find_active_route(node, destination);

        if (active_route == NULL || active_route->is_static) {
            return UCN_ERR_NOT_FOUND;
        }
    }
#endif
#if !UCN_FEATURE_CANDIDATE_ROUTING
    if (find_link(node, destination) != NULL) {
        return UCN_OK;
    }
#endif

    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        if (node->discoveries[index].active &&
            node->discoveries[index].destination == destination
#if UCN_FEATURE_CANDIDATE_ROUTING
            &&
            node->discoveries[index].is_candidate == is_candidate) {
#else
            ) {
#endif
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
    slot->deadline_ms =
        ucn_deadline_from_now(now_ms, UCN_ROUTE_REQUEST_TIMEOUT_MS);
#if UCN_FEATURE_CANDIDATE_ROUTING
    slot->is_candidate = is_candidate;
#endif

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_ROUTE_REQ;
    frame.wire_profile = node->tx_wire_profile;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = UCN_NODE_BROADCAST;
    frame.session_id = node->session_id;
    write_uint_be(payload,
                  ucn_wire_profile_get_descriptor(frame.wire_profile)->address_bytes,
                  destination);
    write_u32_be(payload + route_request_id_offset(&frame), slot->request_id);
    write_u16_be(payload + route_request_cost_offset(&frame), 0U);
    payload[route_request_hop_offset(&frame)] = 0U;
    payload[route_request_flags_offset(&frame)] =
        is_candidate ? UCN_ROUTE_REQ_FLAG_CANDIDATE : 0U;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        slot->active = false;
        return result;
    }
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length =
        (uint16_t)route_request_payload_size(frame.wire_profile);

    result = forward_route_request(node, NULL, &frame);
    if (result != UCN_OK) {
        slot->active = false;
        return result;
    }
    node->stats.route_requests_sent++;
#if UCN_FEATURE_CANDIDATE_ROUTING
    if (is_candidate) {
        node->stats.route_refreshes_started++;
        {
            ucn_route_entry_t *active_route = find_active_route(node, destination);
            if (active_route != NULL) {
                active_route->last_refresh_started_ms = now_ms;
            }
        }
    }
#endif
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
#if UCN_FEATURE_CANDIDATE_ROUTING
    return begin_route_discovery(node, destination, now_ms, true);
#else
    (void)destination;
    (void)now_ms;
    return node == NULL ? UCN_ERR_ARGUMENT : UCN_ERR_CONFIG;
#endif
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

#if UCN_FEATURE_PATH
ucn_result_t ucn_node_install_local_path(ucn_node_t *node,
                                         ucn_path_id_t path_id,
                                         ucn_node_id_t destination,
                                         ucn_node_id_t next_hop,
                                         uint32_t lease_ms)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }
    return install_path_forward_entry(node, node->config.node_id, node->session_id,
                                      path_id, destination, next_hop, lease_ms);
}

ucn_result_t ucn_node_revoke_local_path(ucn_node_t *node,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination)
{
    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }
    return ucn_path_revoke(&node->path_state, node->config.node_id,
                           node->session_id, path_id, destination);
}

ucn_result_t ucn_node_send_path_install(ucn_node_t *node,
                                        ucn_node_id_t control_target,
                                        ucn_path_id_t path_id,
                                        ucn_node_id_t destination,
                                        ucn_node_id_t next_hop,
                                        uint32_t lease_ms)
{
    uint8_t payload[UCN_PATH_INSTALL_PAYLOAD_BYTES];
    ucn_result_t result;

    if (node == NULL || control_target == 0U ||
        control_target == UCN_NODE_BROADCAST || control_target == node->config.node_id ||
        path_id == 0U || destination == 0U || destination == UCN_NODE_BROADCAST ||
        !ucn_duration_is_valid(lease_ms) ||
        (next_hop == 0U && control_target != destination)) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }
    write_u32_be(payload, path_id);
    write_u32_be(payload + 4U, destination);
    write_u32_be(payload + 8U, next_hop);
    write_u32_be(payload + 12U, lease_ms);
    result = send_control_to_node(node, control_target, UCN_MSG_PATH_INSTALL,
                                  payload, (uint16_t)sizeof(payload));
    if (result == UCN_OK) {
        node->stats.path_installs_sent++;
    }
    return result;
}

ucn_result_t ucn_node_send_path_revoke(ucn_node_t *node,
                                       ucn_node_id_t control_target,
                                       ucn_path_id_t path_id,
                                       ucn_node_id_t destination)
{
    uint8_t payload[UCN_PATH_REVOKE_PAYLOAD_BYTES];
    ucn_result_t result;

    if (node == NULL || control_target == 0U ||
        control_target == UCN_NODE_BROADCAST || control_target == node->config.node_id ||
        path_id == 0U || destination == 0U || destination == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }
    write_u32_be(payload, path_id);
    write_u32_be(payload + 4U, destination);
    result = send_control_to_node(node, control_target, UCN_MSG_PATH_REVOKE,
                                  payload, (uint16_t)sizeof(payload));
    if (result == UCN_OK) {
        node->stats.path_revokes_sent++;
    }
    return result;
}

const ucn_path_forward_entry_t *ucn_node_find_path_forward(
    const ucn_node_t *node,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination)
{
    if (node == NULL) {
        return NULL;
    }
    return find_active_path(node, owner, owner_session_id, path_id, destination);
}
#endif

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
    if (!ucn_node_security_ready(node)) {
        return UCN_ERR_SECURITY;
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
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = protect_outbound_business(node, &frame, ciphertext, auth_tag);
    if (result != UCN_OK) {
        return result;
    }
    return send_frame_on_link(node, link, &frame);
}

/* Re-resolve a Path only after a definitive physical-Bearer failure.  The
 * first send returned LINK_DOWN, so retrying the unchanged frame once on the
 * newly selected Bearer cannot create a second successful delivery. */
#if UCN_FEATURE_PATH
static ucn_result_t send_frame_on_path_egress(
    ucn_node_t *node,
    const ucn_path_forward_entry_t *path,
    const ucn_frame_t *frame,
    ucn_link_t **last_egress_link)
{
    ucn_link_t *configured_egress_link;
    ucn_link_t *link;
    ucn_result_t result;

    if (last_egress_link != NULL) {
        *last_egress_link = NULL;
    }
    if (node == NULL || path == NULL || path->egress_link == NULL || frame == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    configured_egress_link = path->egress_link;
    link = resolve_egress_link(node, configured_egress_link);
    if (link == NULL) {
        revoke_paths_by_unavailable_egress(node, configured_egress_link);
        return UCN_ERR_LINK_DOWN;
    }
    if (last_egress_link != NULL) {
        *last_egress_link = link;
    }
    result = send_frame_on_link(node, link, frame);
    if (result != UCN_ERR_LINK_DOWN) {
        return result;
    }

    link = resolve_egress_link(node, configured_egress_link);
    if (link == NULL || (last_egress_link != NULL && link == *last_egress_link)) {
        revoke_paths_by_unavailable_egress(node, configured_egress_link);
        return UCN_ERR_LINK_DOWN;
    }
    if (last_egress_link != NULL) {
        *last_egress_link = link;
    }
    result = send_frame_on_link(node, link, frame);
    if (result == UCN_ERR_LINK_DOWN) {
        revoke_paths_by_unavailable_egress(node, configured_egress_link);
    }
    return result;
}

ucn_result_t ucn_node_send_path(ucn_node_t *node,
                                ucn_node_id_t destination,
                                uint8_t message_type,
                                ucn_traffic_class_t traffic_class,
                                ucn_path_id_t path_id,
                                const uint8_t *payload,
                                uint16_t payload_length)
{
    const ucn_path_forward_entry_t *path;
    ucn_frame_t frame;
    ucn_link_t *link;
    ucn_result_t result;
    uint8_t ciphertext[UCN_MAX_PAYLOAD_BYTES];
    uint8_t auth_tag[UCN_E2E_TAG_SIZE];

    if (node == NULL || destination == 0U || path_id == 0U ||
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
    if (node->security_ops == NULL || node->session_id == 0U) {
        return UCN_ERR_SECURITY;
    }

    path = find_active_path(node, node->config.node_id, node->session_id,
                            path_id, destination);
    if (path == NULL || path->terminal || path->egress_link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = traffic_class;
    frame.flags = UCN_FRAME_FLAG_ROUTE_EXTENSION | UCN_FRAME_FLAG_PATH_ID;
    frame.hop_limit = node->config.default_hop_limit;
    frame.network_id = node->config.network_id;
    frame.source = node->config.node_id;
    frame.destination = destination;
    frame.session_id = node->session_id;
    frame.has_route_extension = true;
    frame.has_path_id = true;
    frame.path_id = path_id;
    result = allocate_sequence(node, &frame.sequence);
    if (result != UCN_OK) {
        return result;
    }
    frame.session_id = node->session_id;
    frame.payload = payload;
    frame.payload_length = payload_length;
    result = protect_outbound_business(node, &frame, ciphertext, auth_tag);
    if (result != UCN_OK) {
        return result;
    }
    result = send_frame_on_path_egress(node, path, &frame, &link);
    if (result == UCN_ERR_LINK_DOWN) {
        revoke_path_and_mark_local_policy(node, node->config.node_id,
                                           node->session_id, path_id,
                                           destination);
    }
    return result;
}
#endif

#if UCN_FEATURE_POLICY
static bool pinned_path_has_hard_failure(ucn_result_t result)
{
    /* These two results mean the selected authenticated Path no longer has a
     * usable local forwarding entry.  Backpressure, security failure and
     * generic driver errors must not silently move a deterministic flow. */
    return result == UCN_ERR_LINK_DOWN || result == UCN_ERR_NOT_FOUND;
}
#endif

static ucn_result_t send_endpoint_auto_best(ucn_node_t *node,
                                            ucn_node_id_t destination,
                                            ucn_endpoint_t endpoint,
                                            ucn_traffic_class_t traffic_class,
                                            const uint8_t *payload,
                                            uint16_t payload_length)
{
    ucn_result_t result;

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

#if UCN_FEATURE_POLICY
static ucn_result_t send_endpoint_on_policy_path(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    uint16_t local_path_id,
    const uint8_t *payload,
    uint16_t payload_length)
{
    const ucn_policy_path_entry_t *path;
    ucn_result_t result;

    path = ucn_node_find_policy_path(node, local_path_id);
    if (path == NULL || path->destination != destination ||
        path->wire_path_id == 0U || path->state == UCN_POLICY_PATH_EMPTY ||
        path->state == UCN_POLICY_PATH_CANDIDATE) {
        node->policy_state.stats.pinned_policy_config_errors++;
        return UCN_ERR_CONFIG;
    }
    if (path->state == UCN_POLICY_PATH_DOWN) {
        return UCN_ERR_LINK_DOWN;
    }

    result = ucn_node_send_path(node, destination, (uint8_t)endpoint,
                                traffic_class, path->wire_path_id, payload,
                                payload_length);
    if (pinned_path_has_hard_failure(result)) {
        ucn_policy_mark_path_down(&node->policy_state, local_path_id);
    }
    return result;
}

static bool auto_balance_path_is_member(const ucn_route_policy_config_t *config,
                                        uint16_t local_path_id)
{
    return local_path_id != 0U &&
           (local_path_id == config->primary_local_path_id ||
            local_path_id == config->backup_local_path_id);
}

static bool auto_balance_path_is_usable(const ucn_node_t *node,
                                        ucn_node_id_t destination,
                                        uint16_t local_path_id)
{
    const ucn_policy_path_entry_t *path;
    const ucn_path_forward_entry_t *wire_path;

    path = ucn_node_find_policy_path(node, local_path_id);
    if (path == NULL || path->destination != destination ||
        path->wire_path_id == 0U || path->state != UCN_POLICY_PATH_VERIFIED) {
        return false;
    }
    wire_path = ucn_path_find(&node->path_state, node->config.node_id,
                              node->session_id, path->wire_path_id,
                              destination);
    return wire_path != NULL && !wire_path->terminal &&
           !ucn_path_is_expired(wire_path, node->now_ms) &&
           wire_path->egress_link == path->egress_link;
}

static bool auto_balance_path_is_congested(const ucn_node_t *node,
                                           uint16_t local_path_id)
{
    const ucn_policy_path_entry_t *path =
        ucn_node_find_policy_path(node, local_path_id);

    return path != NULL && path->congestion_samples >=
                               UCN_POLICY_BALANCE_CONGESTED_SAMPLE_LIMIT;
}

static size_t auto_balance_active_flow_count(const ucn_node_t *node,
                                             uint16_t local_path_id)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
        const ucn_policy_flow_binding_t *flow = &node->policy_state.flows[index];

        if (flow->occupied && flow->local_path_id == local_path_id &&
            !ucn_deadline_expired(node->now_ms, flow->expires_at_ms)) {
            count++;
        }
    }
    return count;
}

static uint32_t auto_balance_path_score(const ucn_node_t *node,
                                        uint16_t local_path_id)
{
    const ucn_policy_path_entry_t *path =
        ucn_node_find_policy_path(node, local_path_id);
    const ucn_policy_link_quality_snapshot_t *quality;
    const uint32_t unknown_score_base = UINT32_MAX / 2U;
    uint32_t score;
    size_t active_flows;

    if (path == NULL) {
        return UINT32_MAX;
    }
    quality = ucn_node_get_link_quality(node, path->egress_link);
    active_flows = auto_balance_active_flow_count(node, local_path_id);
    if (quality == NULL || !quality->route_cost_valid ||
        !route_cost_is_known(quality->route_cost)) {
        return active_flows >= (size_t)(UINT32_MAX - unknown_score_base) ?
                   UINT32_MAX : unknown_score_base + (uint32_t)active_flows;
    }
    {
        const uint64_t weighted = (uint64_t)quality->route_cost *
                                  (uint64_t)(active_flows + 1U);

        score = weighted >= unknown_score_base ? unknown_score_base - 1U :
                                                 (uint32_t)weighted;
    }
    return score;
}

static bool auto_balance_has_configured_path(
    const ucn_node_t *node,
    const ucn_route_policy_config_t *config,
    ucn_node_id_t destination)
{
    const ucn_policy_path_entry_t *path;

    path = ucn_node_find_policy_path(node, config->primary_local_path_id);
    if (path != NULL && path->destination == destination) {
        return true;
    }
    path = ucn_node_find_policy_path(node, config->backup_local_path_id);
    return path != NULL && path->destination == destination;
}

static uint16_t auto_balance_select_path(const ucn_node_t *node,
                                         const ucn_route_policy_config_t *config,
                                         ucn_node_id_t destination,
                                         uint16_t excluded_local_path_id)
{
    const uint16_t candidates[2] = {
        config->primary_local_path_id,
        config->backup_local_path_id
    };
    uint16_t selected = 0U;
    uint32_t best_score = UINT32_MAX;
    bool has_noncongested = false;
    size_t index;

    for (index = 0U; index < 2U; ++index) {
        const uint16_t candidate = candidates[index];

        if (candidate != excluded_local_path_id &&
            auto_balance_path_is_usable(node, destination, candidate) &&
            !auto_balance_path_is_congested(node, candidate)) {
            has_noncongested = true;
            break;
        }
    }
    for (index = 0U; index < 2U; ++index) {
        const uint16_t candidate = candidates[index];
        uint32_t score;

        if (candidate == excluded_local_path_id ||
            !auto_balance_path_is_usable(node, destination, candidate) ||
            (has_noncongested && auto_balance_path_is_congested(node, candidate))) {
            continue;
        }
        score = auto_balance_path_score(node, candidate);
        if (selected == 0U || score < best_score) {
            selected = candidate;
            best_score = score;
        }
    }
    return selected;
}

static ucn_result_t auto_balance_bind_path(ucn_node_t *node,
                                           ucn_node_id_t destination,
                                           ucn_endpoint_t endpoint,
                                           uint16_t local_path_id,
                                           uint32_t lease_ms)
{
    return ucn_node_bind_q1_flow(node, destination, endpoint, local_path_id,
                                 lease_ms);
}

static ucn_result_t send_endpoint_auto_balance(
    ucn_node_t *node,
    const ucn_route_policy_entry_t *policy,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length)
{
    const ucn_route_policy_config_t *config = &policy->config;
    const ucn_policy_flow_binding_t *flow;
    const uint32_t lease_ms = config->balance_flow_lease_ms == 0U ?
                                  UCN_POLICY_BALANCE_FLOW_LEASE_MS :
                                  config->balance_flow_lease_ms;
    uint16_t selected_local_path_id;
    bool rebinding = false;
    bool congestion_rebind = false;
    bool down_rebind = false;
    ucn_result_t result;

    flow = ucn_node_find_q1_flow(node, destination, endpoint);
    if (flow != NULL && auto_balance_path_is_member(config, flow->local_path_id) &&
        auto_balance_path_is_usable(node, destination, flow->local_path_id) &&
        !auto_balance_path_is_congested(node, flow->local_path_id)) {
        selected_local_path_id = flow->local_path_id;
    } else {
        rebinding = flow != NULL;
        congestion_rebind = rebinding &&
                             auto_balance_path_is_member(config,
                                                         flow->local_path_id) &&
                             auto_balance_path_is_usable(node, destination,
                                                         flow->local_path_id) &&
                             auto_balance_path_is_congested(node,
                                                            flow->local_path_id);
        down_rebind = rebinding &&
                      auto_balance_path_is_member(config, flow->local_path_id) &&
                      !auto_balance_path_is_usable(node, destination,
                                                    flow->local_path_id);
        selected_local_path_id = auto_balance_select_path(node, config,
                                                           destination, 0U);
        if (selected_local_path_id == 0U) {
            node->policy_state.stats.auto_balance_selection_failures++;
            return auto_balance_has_configured_path(node, config, destination) ?
                       UCN_ERR_LINK_DOWN : UCN_ERR_CONFIG;
        }
        result = auto_balance_bind_path(node, destination, endpoint,
                                        selected_local_path_id, lease_ms);
        if (result != UCN_OK) {
            node->policy_state.stats.auto_balance_selection_failures++;
            return result;
        }
        if (rebinding) {
            node->policy_state.stats.auto_balance_rebindings++;
            if (congestion_rebind) {
                node->policy_state.stats.auto_balance_congestion_rebindings++;
            } else if (down_rebind) {
                node->policy_state.stats.auto_balance_down_rebindings++;
            }
        } else {
            node->policy_state.stats.auto_balance_flow_bindings++;
        }
    }

    result = send_endpoint_on_policy_path(node, destination, endpoint,
                                          UCN_TRAFFIC_Q1_REALTIME,
                                          selected_local_path_id, payload,
                                          payload_length);
    if (result == UCN_OK) {
        node->policy_state.stats.auto_balance_sends++;
        ucn_policy_touch_q1_flow(&node->policy_state, destination, endpoint,
                                 node->now_ms);
        return UCN_OK;
    }
    if (!pinned_path_has_hard_failure(result)) {
        return result;
    }

    /* The selected Path was proven down by this send.  Rebind once and retry
     * only on another verified member; this is failover, never replication. */
    selected_local_path_id = auto_balance_select_path(node, config, destination,
                                                       selected_local_path_id);
    if (selected_local_path_id == 0U) {
        node->policy_state.stats.auto_balance_selection_failures++;
        return result;
    }
    result = auto_balance_bind_path(node, destination, endpoint,
                                    selected_local_path_id, lease_ms);
    if (result != UCN_OK) {
        node->policy_state.stats.auto_balance_selection_failures++;
        return result;
    }
    node->policy_state.stats.auto_balance_rebindings++;
    node->policy_state.stats.auto_balance_down_rebindings++;
    result = send_endpoint_on_policy_path(node, destination, endpoint,
                                          UCN_TRAFFIC_Q1_REALTIME,
                                          selected_local_path_id, payload,
                                          payload_length);
    if (result == UCN_OK) {
        node->policy_state.stats.auto_balance_sends++;
        ucn_policy_touch_q1_flow(&node->policy_state, destination, endpoint,
                                 node->now_ms);
    }
    return result;
}

static ucn_result_t send_endpoint_pinned(
    ucn_node_t *node,
    const ucn_route_policy_entry_t *policy,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length)
{
    const ucn_route_policy_config_t *config = &policy->config;
    ucn_result_t result;

    if (config->primary_local_path_id == 0U) {
        node->policy_state.stats.pinned_policy_config_errors++;
        return UCN_ERR_CONFIG;
    }

    result = send_endpoint_on_policy_path(node, destination, endpoint,
                                          traffic_class,
                                          config->primary_local_path_id,
                                          payload, payload_length);
    if (config->mode == UCN_ROUTE_POLICY_PINNED_STRICT) {
        if (result == UCN_OK) {
            node->policy_state.stats.pinned_strict_sends++;
        } else {
            node->policy_state.stats.pinned_strict_failures++;
        }
        return result;
    }

    if (result == UCN_OK) {
        node->policy_state.stats.pinned_failover_primary_sends++;
        return UCN_OK;
    }
    if (!pinned_path_has_hard_failure(result)) {
        return result;
    }
    node->policy_state.stats.pinned_failover_hard_failures++;

    if (config->backup_local_path_id != 0U) {
        result = send_endpoint_on_policy_path(node, destination, endpoint,
                                              traffic_class,
                                              config->backup_local_path_id,
                                              payload, payload_length);
        if (result == UCN_OK) {
            node->policy_state.stats.pinned_failover_backup_sends++;
            return UCN_OK;
        }
        if (!pinned_path_has_hard_failure(result)) {
            return result;
        }
    }

    /* Discovery is a deliberate last resort of PINNED_FAILOVER.  Q0 never
     * waits for it, and strict mode never reaches this branch. */
    if (config->allow_discovery_on_hard_failure &&
        traffic_class == UCN_TRAFFIC_Q1_REALTIME) {
        result = begin_route_discovery(node, destination, node->now_ms, false);
        if (result != UCN_OK) {
            return result;
        }
        node->policy_state.stats.pinned_failover_discovery_fallbacks++;
        if (find_link(node, destination) == NULL) {
            return queue_pending_q1(node, destination, (uint8_t)endpoint,
                                    payload, payload_length);
        }
        return ucn_node_send(node, destination, (uint8_t)endpoint,
                             traffic_class, payload, payload_length);
    }
    return result;
}
#endif

ucn_result_t ucn_node_send_endpoint(ucn_node_t *node,
                                    ucn_node_id_t destination,
                                    ucn_endpoint_t endpoint,
                                    ucn_traffic_class_t traffic_class,
                                    const uint8_t *payload,
                                    uint16_t payload_length)
{
#if UCN_FEATURE_POLICY
    const ucn_route_policy_entry_t *policy;
#endif

    if (node == NULL || !ucn_endpoint_is_static(endpoint) || destination == 0U ||
        (payload_length != 0U && payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
#if UCN_FEATURE_POLICY
    policy = ucn_node_find_route_policy(node, destination, endpoint, traffic_class);
    if (policy != NULL) {
        size_t policy_index;

        node->policy_state.stats.policy_match_hits++;
        for (policy_index = 0U; policy_index < UCN_MAX_ROUTE_POLICIES;
             ++policy_index) {
            if (&node->policy_state.policies[policy_index] == policy) {
                node->policy_state.policies[policy_index].match_hits++;
                break;
            }
        }
        if (policy->config.mode == UCN_ROUTE_POLICY_PINNED_STRICT ||
            policy->config.mode == UCN_ROUTE_POLICY_PINNED_FAILOVER) {
            return send_endpoint_pinned(node, policy, destination, endpoint,
                                        traffic_class, payload, payload_length);
        }
        if (policy->config.mode == UCN_ROUTE_POLICY_AUTO_BALANCE) {
            return send_endpoint_auto_balance(node, policy, destination, endpoint,
                                              payload, payload_length);
        }
    }
#endif
    return send_endpoint_auto_best(node, destination, endpoint, traffic_class,
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
        request->delivery != UCN_DELIVERY_LATEST_VALUE &&
        request->delivery != UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
        return UCN_ERR_ARGUMENT;
    }
    if (request->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
        (request->traffic_class != UCN_TRAFFIC_Q0_CRITICAL ||
         request->deadline_ms == 0U)) {
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
    slot->next_attempt_ms = 0U;
    slot->order = node->next_queue_order++;
    slot->backpressure_retries = 0U;
    slot->payload_length = request->payload_length;
    if (request->payload_length != 0U) {
        (void)memcpy(slot->payload, request->payload, request->payload_length);
    }
    return UCN_OK;
}

#if UCN_FEATURE_DIAGNOSTICS
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
    pending->deadline_ms =
        ucn_deadline_from_now(node->now_ms, UCN_PATH_TRACE_TIMEOUT_MS);
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
    pending->deadline_ms =
        ucn_deadline_from_now(node->now_ms, UCN_NODE_SNAPSHOT_TIMEOUT_MS);
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
    frame.session_id = node->session_id;

    /* The origin also remembers its own flood so a loop cannot cause it to
     * become a responder or re-flood the same Query ID. */
    result = ucn_duplicate_accept_frame(node, &frame);
    if (result != UCN_OK) {
        pending->occupied = false;
        return result;
    }
    result = forward_node_snapshot_request(node, NULL, &frame);
    if (result != UCN_OK) {
        pending->occupied = false;
        return result;
    }
    node->stats.node_snapshot_requests_sent++;
    return UCN_OK;
}

ucn_result_t ucn_node_request_policy_diagnostic(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_policy_diagnostic_section_t section,
    uint8_t index,
    ucn_policy_diagnostic_handler_t handler,
    void *context)
{
    ucn_policy_diagnostic_pending_t *pending;
    uint32_t request_id;

    if (node == NULL || handler == NULL || destination == 0U ||
        destination == UCN_NODE_BROADCAST || destination == node->config.node_id ||
        !policy_diagnostic_selector_is_valid((uint8_t)section, index)) {
        return UCN_ERR_ARGUMENT;
    }
    expire_policy_diagnostic_state(node, node->now_ms);
    pending = find_free_policy_diagnostic_pending(node);
    if (pending == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    if (find_link(node, destination) == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (!take_policy_diagnostic_token(node)) {
        return UCN_ERR_NO_SPACE;
    }
    request_id = node->next_policy_diagnostic_id;
    if (request_id == 0U || request_id == UINT32_MAX) {
        request_id = 1U;
    }
    node->next_policy_diagnostic_id = request_id + 1U;
    (void)memset(pending, 0, sizeof(*pending));
    pending->occupied = true;
    pending->destination = destination;
    pending->request_id = request_id;
    pending->deadline_ms =
        ucn_deadline_from_now(node->now_ms, UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS);
    pending->section = section;
    pending->index = index;
    pending->handler = handler;
    pending->context = context;
    /* `ucn_node_step()` sends this only after ordinary Q0/Q1 work. */
    return UCN_OK;
}
#endif

static void note_business_transmission(ucn_node_t *node)
{
    if (node->business_tx_since_maintenance <
        UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE) {
        node->business_tx_since_maintenance++;
    }
}

/* Only liveness and path/routing maintenance belongs here.  Snapshot and
 * policy diagnostics remain best-effort background work and must not take a
 * Q0/Q1 budget slot. */
static ucn_result_t send_due_essential_maintenance(ucn_node_t *node,
                                                    uint32_t now_ms)
{
    ucn_result_t result;

    result = send_due_heartbeat(node, now_ms);
    if (result != UCN_ERR_NOT_FOUND) {
        return result;
    }
    result = send_due_bearer_quality_probe(node, now_ms);
    if (result != UCN_ERR_NOT_FOUND) {
        return result;
    }
#if UCN_FEATURE_CANDIDATE_ROUTING
    result = send_due_path_probe(node, now_ms);
    if (result != UCN_ERR_NOT_FOUND) {
        return result;
    }
    return start_due_route_refresh(node, now_ms);
#else
    return UCN_ERR_NOT_FOUND;
#endif
}

static void observe_step_interval(ucn_node_t *node, uint32_t now_ms)
{
    uint32_t gap_ms;

    if (!node->step_observation_started) {
        node->step_observation_started = true;
        node->stats.last_step_ms = now_ms;
        return;
    }

    gap_ms = (uint32_t)(now_ms - node->stats.last_step_ms);
    node->stats.last_step_ms = now_ms;
    if (gap_ms > node->stats.max_step_gap_ms) {
        node->stats.max_step_gap_ms = gap_ms;
    }
    if (gap_ms > UCN_MAX_STEP_INTERVAL_MS) {
        node->stats.step_interval_violations++;
    }
}

ucn_result_t ucn_node_step(ucn_node_t *node, uint32_t now_ms)
{
    ucn_tx_item_t *item;
    size_t count;
    uint32_t error_drops_before;
    ucn_result_t result;

    if (node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_node_security_ready(node)) {
        return UCN_ERR_SECURITY;
    }

    /* Observe before any early return so every valid Protocol Task iteration
     * contributes to the product timing contract.  A violation is diagnostic:
     * stopping an already-late scheduler would only delay liveness further. */
    observe_step_interval(node, now_ms);

    expire_dynamic_state(node, now_ms);
#if UCN_FEATURE_POLICY
    ucn_policy_refresh_link_quality(&node->policy_state, node->links,
                                    node->link_count, now_ms);
    ucn_policy_expire_flows(&node->policy_state, now_ms);
#endif
#if UCN_FEATURE_PATH
    ucn_path_expire(&node->path_state, now_ms);
#endif
#if UCN_FEATURE_DIAGNOSTICS
    expire_path_trace_state(node, now_ms);
    expire_node_snapshot_state(node, now_ms);
    expire_policy_diagnostic_state(node, now_ms);
#endif
    expire_neighbor_candidates(node, now_ms);
    maintain_neighbor_liveness(node, now_ms);
    evaluate_bearer_quality(node, now_ms);

    item = find_next_item(node->q0, UCN_TX_Q0_DEPTH);
    if (item == NULL) {
        item = find_next_item(node->q1, UCN_TX_Q1_DEPTH);
    }

    /* A permanently non-empty business queue must not indefinitely suppress
     * neighbor liveness or path maintenance.  The burst counter saturates,
     * therefore once a control action becomes due it gets a scheduling slot
     * in this call.  Q0 remains FIFO and is delayed only by an actually-due
     * essential maintenance action; diagnostics never preempt either queue. */
    if (node->business_tx_since_maintenance >=
        UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE) {
        result = send_due_essential_maintenance(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            node->business_tx_since_maintenance = 0U;
            node->stats.maintenance_preemptions++;
            return result;
        }
    }

    if (item == NULL) {
        result = send_pending_q1_if_ready(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            note_business_transmission(node);
            return result;
        }
        result = send_due_essential_maintenance(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            node->business_tx_since_maintenance = 0U;
            return result;
        }
#if UCN_FEATURE_DIAGNOSTICS
        result = send_due_node_snapshot_reply(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
        /* T22.6 queries are deliberately after normal Q0/Q1 work, liveness,
         * route maintenance and existing diagnostics.  They never enter the
         * business queues and cannot consume a Q0 slot. */
        result = send_due_policy_diagnostic_reply(node);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
        result = send_due_policy_diagnostic_request(node);
        if (result != UCN_ERR_NOT_FOUND) {
            return result;
        }
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
        return start_due_route_refresh(node, now_ms);
#else
        return UCN_ERR_NOT_FOUND;
#endif
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

    /* A retained Q0 item preserves FIFO ownership while it waits for its next
     * bounded admission attempt.  Lower-priority business and diagnostics do
     * not use the gap, but essential liveness/path maintenance may proceed. */
    if (item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE &&
        item->next_attempt_ms != 0U &&
        !ucn_deadline_expired(now_ms, item->next_attempt_ms)) {
        result = send_due_essential_maintenance(node, now_ms);
        if (result != UCN_ERR_NOT_FOUND) {
            node->business_tx_since_maintenance = 0U;
        }
        return result;
    }

    count = item->payload_length;
    error_drops_before = node->stats.tx_error_dropped;
    result = ucn_node_send(node,
                           item->destination,
                           item->message_type,
                           item->traffic_class,
                           item->payload,
                           (uint16_t)count);
    note_business_transmission(node);
    if (result == UCN_OK) {
        item->occupied = false;
        return UCN_OK;
    }

    if (result == UCN_ERR_NO_SPACE &&
        item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
        const bool retry_budget_available =
            item->backpressure_retries < UCN_Q0_BACKPRESSURE_MAX_RETRIES;
        const bool retry_fits_deadline =
            !ucn_deadline_due_within(
                now_ms, item->deadline_ms,
                UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS);

        if (retry_budget_available && retry_fits_deadline) {
            /* send_frame_on_link() records a failed attempt as a drop.  The
             * item is still owned here, so restore the pre-attempt final-drop
             * count.  A later final failure will be counted exactly once. */
            node->stats.tx_error_dropped = error_drops_before;
            item->backpressure_retries++;
            item->next_attempt_ms = ucn_deadline_from_now(
                now_ms, UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS);
            node->stats.q0_backpressure_retries++;
            return result;
        }
        item->occupied = false;
        node->stats.q0_backpressure_exhausted++;
        if (node->stats.tx_error_dropped == error_drops_before) {
            node->stats.tx_error_dropped++;
        }
        return result;
    }

    item->occupied = false;
    if (node->stats.tx_error_dropped == error_drops_before) {
        node->stats.tx_error_dropped++;
    }
    if (item->delivery == UCN_DELIVERY_RETRY_ON_BACKPRESSURE) {
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
    bool control_consumed = false;
    ucn_result_t result;
    uint8_t plaintext[UCN_MAX_PAYLOAD_BYTES];

    if (node == NULL || ingress_link == NULL || data == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_node_security_ready(node)) {
        return UCN_ERR_SECURITY;
    }

    result = ucn_frame_decode(data, length, &frame);
    if (result != UCN_OK) {
        return result;
    }

    if (frame.wire_profile > node->max_receive_wire_profile) {
        return UCN_ERR_UNSUPPORTED;
    }

    if (frame.network_id != node->config.network_id) {
        return UCN_ERR_NETWORK;
    }

    if (ucn_message_type_is_control(frame.message_type) &&
        (frame.flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U) {
        return UCN_ERR_MALFORMED;
    }
    if (ucn_message_type_is_control(frame.message_type) && frame.has_path_id) {
        return UCN_ERR_MALFORMED;
    }
#if !UCN_FEATURE_PATH
    if (frame.has_path_id || frame.message_type == UCN_MSG_PATH_INSTALL ||
        frame.message_type == UCN_MSG_PATH_REVOKE) {
        return UCN_ERR_CONFIG;
    }
#endif
#if !UCN_FEATURE_CANDIDATE_ROUTING
    if (frame.message_type == UCN_MSG_PATH_PROBE ||
        frame.message_type == UCN_MSG_PATH_PROBE_ACK ||
        frame.message_type == UCN_MSG_PATH_ACTIVATE ||
        frame.message_type == UCN_MSG_PATH_ACTIVATE_ACK) {
        return UCN_ERR_CONFIG;
    }
#endif
#if !UCN_FEATURE_DIAGNOSTICS
    if (frame.message_type == UCN_MSG_PATH_TRACE_REQ ||
        frame.message_type == UCN_MSG_PATH_TRACE_REPLY ||
        frame.message_type == UCN_MSG_NODE_SNAPSHOT_REQ ||
        frame.message_type == UCN_MSG_NODE_SNAPSHOT_REPLY ||
        frame.message_type == UCN_MSG_POLICY_DIAGNOSTIC_REQ ||
        frame.message_type == UCN_MSG_POLICY_DIAGNOSTIC_REPLY ||
        (frame.flags & UCN_FRAME_FLAG_DIAGNOSTIC) != 0U) {
        return UCN_ERR_CONFIG;
    }
#endif

    if (node->security_ops != NULL) {
        result = node->security_ops->authorize_rx(node->security_context,
                                                  ingress_link, &frame);
        if (result != UCN_OK) {
            if (frame.destination == node->config.node_id) {
#if UCN_FEATURE_PATH
                note_path_control_authorization_rejected(node,
                                                         frame.message_type);
#endif
            }
            return result;
        }
    }

    if (frame.message_type == UCN_MSG_HELLO) {
        result = ucn_duplicate_accept_frame(node, &frame);
        if (result != UCN_OK) {
            return result;
        }
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
        ucn_rreq_cache_classification_t classification;
        size_t rreq_slot = 0U;
        uint16_t route_cost;

        if (frame.destination != UCN_NODE_BROADCAST) {
            return UCN_ERR_MALFORMED;
        }
        if (frame.payload_length != route_request_payload_size(
                                       frame.wire_profile)) {
            return UCN_ERR_MALFORMED;
        }
        result = validate_route_request_frame(node, ingress_link, &frame);
        if (result != UCN_OK) {
            return result;
        }
        route_cost = read_u16_be(
            frame.payload + route_request_cost_offset(&frame));
        classification = classify_route_request(node, &frame, route_cost,
                                                &rreq_slot);
        if (classification == UCN_RREQ_CACHE_REPLAY) {
            node->stats.route_request_replayed++;
            return UCN_ERR_REPLAY;
        }
        if (classification == UCN_RREQ_CACHE_FULL) {
            node->stats.route_request_cache_full++;
            return UCN_ERR_NO_SPACE;
        }
        if (!take_control_rx_token(node, ingress_link,
                                   UCN_CONTROL_RX_ROUTE_REQUEST)) {
            return UCN_ERR_NO_SPACE;
        }
        commit_route_request(node, &frame, route_cost, rreq_slot);
        touch_neighbor(node, ingress_link);
        return handle_route_request(node, ingress_link, &frame);
    }

    result = ucn_duplicate_accept_frame(node, &frame);
    if (result != UCN_OK) {
        return result;
    }

    if (frame.message_type == UCN_MSG_HEARTBEAT) {
        return handle_heartbeat(node, ingress_link, &frame);
    }

    touch_neighbor(node, ingress_link);

#if UCN_FEATURE_DIAGNOSTICS
    if (frame.message_type == UCN_MSG_NODE_SNAPSHOT_REQ) {
        return handle_node_snapshot_request(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_NODE_SNAPSHOT_REPLY) {
        return handle_node_snapshot_reply(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_POLICY_DIAGNOSTIC_REQ &&
        frame.destination == node->config.node_id) {
        return handle_policy_diagnostic_request(node, &frame);
    }
    if (frame.message_type == UCN_MSG_POLICY_DIAGNOSTIC_REPLY &&
        frame.destination == node->config.node_id) {
        return handle_policy_diagnostic_reply(node, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_TRACE_REQ) {
        return handle_path_trace_request(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_TRACE_REPLY) {
        return handle_path_trace_reply(node, ingress_link, &frame);
    }
#endif
#if UCN_FEATURE_PATH
    if (frame.message_type == UCN_MSG_PATH_INSTALL &&
        frame.destination == node->config.node_id) {
        return handle_path_install(node, ingress_link, &frame);
    }
    if (frame.message_type == UCN_MSG_PATH_REVOKE &&
        frame.destination == node->config.node_id) {
        return handle_path_revoke(node, ingress_link, &frame);
    }
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
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
#endif

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

#if UCN_FEATURE_PATH
    if (!ucn_message_type_is_control(frame.message_type) && frame.has_path_id) {
        const ucn_path_forward_entry_t *path = find_active_path(
            node, frame.source, frame.session_id, frame.path_id, frame.destination);

        if (path == NULL ||
            (frame.destination == node->config.node_id && !path->terminal) ||
            (frame.destination != node->config.node_id && path->terminal)) {
            node->stats.path_rejected++;
            (void)send_path_route_error(node, ingress_link, frame.source,
                                        frame.destination, frame.session_id,
                                        frame.path_id);
            return UCN_ERR_NOT_FOUND;
        }
    }
#endif

    if (!ucn_message_type_is_control(frame.message_type) && !frame.has_path_id &&
        frame.has_route_extension &&
        frame.destination == node->config.node_id &&
        !route_epoch_is_accepted(node, frame.source, &frame)) {
        node->stats.route_epoch_rejected++;
        return UCN_ERR_NOT_FOUND;
    }

    if (frame.destination != node->config.node_id &&
        frame.destination != UCN_NODE_BROADCAST) {
        ucn_link_t *egress_link;
#if UCN_FEATURE_PATH
        const ucn_path_forward_entry_t *path = NULL;
#endif
        uint8_t route_reply_payload[UCN_ROUTE_REPLY_PAYLOAD_BYTES];

        if (frame.hop_limit <= 1U) {
            return UCN_ERR_TTL;
        }

#if UCN_FEATURE_CANDIDATE_ROUTING
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
#if UCN_FEATURE_PATH
        } else if (!ucn_message_type_is_control(frame.message_type) &&
                   frame.has_path_id) {
            path = find_active_path(
                node, frame.source, frame.session_id, frame.path_id,
                frame.destination);

            egress_link = path == NULL ? NULL :
                          resolve_egress_link(node, path->egress_link);
#endif
        } else if (!ucn_message_type_is_control(frame.message_type)) {
#else
#if UCN_FEATURE_PATH
        if (!ucn_message_type_is_control(frame.message_type) &&
            frame.has_path_id) {
            path = find_active_path(
                node, frame.source, frame.session_id, frame.path_id,
                frame.destination);
            egress_link = path == NULL ? NULL :
                          resolve_egress_link(node, path->egress_link);
        } else
#endif
        if (!ucn_message_type_is_control(frame.message_type)) {
#endif
            egress_link = find_link_for_route_epoch(node, frame.destination,
                                                     frame.has_route_extension,
                                                     frame.route_epoch);
        } else {
            egress_link = find_link(node, frame.destination);
        }
        if (egress_link == NULL || egress_link == ingress_link) {
#if UCN_FEATURE_PATH
            if (frame.has_path_id) {
                if (egress_link == NULL && path != NULL) {
                    revoke_path_and_mark_local_policy(node, path->owner,
                                                       path->owner_session_id,
                                                       path->path_id,
                                                       path->destination);
                }
                (void)send_path_route_error(node, ingress_link, frame.source,
                                            frame.destination, frame.session_id,
                                            frame.path_id);
            } else if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source, frame.destination);
            }
#else
            if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source,
                                       frame.destination);
            }
#endif
            return UCN_ERR_NOT_FOUND;
        }

        /* handle_route_reply() learned this node's target-rooted metric from
         * the ingress Link.  Forward the same accumulated Cost/Hop so the
         * upstream node can extend it by exactly one more Link. */
        if (frame.message_type == UCN_MSG_ROUTE_REPLY &&
            frame.payload_length == UCN_ROUTE_REPLY_PAYLOAD_BYTES) {
            uint8_t reply_hop_count = frame.payload[14];

            if (reply_hop_count == UINT8_MAX) {
                return UCN_ERR_TTL;
            }
            (void)memcpy(route_reply_payload, frame.payload,
                         sizeof(route_reply_payload));
            write_u16_be(
                route_reply_payload + 12U,
                add_route_cost(read_u16_be(frame.payload + 12U),
                               link_route_cost(ingress_link)));
            route_reply_payload[14] = (uint8_t)(reply_hop_count + 1U);
            frame.payload = route_reply_payload;
        }

        --frame.hop_limit;
#if UCN_FEATURE_PATH
        result = frame.has_path_id ?
                 send_frame_on_path_egress(node, path, &frame, &egress_link) :
                 send_frame_on_link(node, egress_link, &frame);
#else
        result = send_frame_on_link(node, egress_link, &frame);
#endif
        if (result == UCN_OK) {
            mark_route_used(node, frame.destination);
#if UCN_FEATURE_PATH
            if (frame.has_path_id) {
                node->stats.path_forwards++;
            }
#endif
            if ((frame.flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U) {
                node->stats.e2e_protected_forwarded++;
            }
        }
        if (result == UCN_ERR_LINK_DOWN) {
            invalidate_routes_by_link(node, egress_link);
#if UCN_FEATURE_PATH
            if (frame.has_path_id) {
                revoke_path_and_mark_local_policy(node, frame.source,
                                                   frame.session_id, frame.path_id,
                                                   frame.destination);
                (void)send_path_route_error(node, ingress_link, frame.source,
                                            frame.destination, frame.session_id,
                                            frame.path_id);
            } else if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source, frame.destination);
            }
#else
            if (frame.message_type != UCN_MSG_ROUTE_ERROR) {
                (void)send_route_error(node, ingress_link, frame.source,
                                       frame.destination);
            }
#endif
        }
        return result;
    }

    if (!dispatch_endpoint(node, &frame) && node->rx_handler != NULL) {
        node->rx_handler(node->rx_context, &frame);
    }

    node->stats.rx_delivered++;

    return UCN_OK;
}
