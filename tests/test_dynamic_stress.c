#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"
#include "ucn/ucn_path.h"
#if UCN_FEATURE_SERVICE
#include "ucn/ucn_service.h"
#endif

#define DYNAMIC_NODE_COUNT ((size_t)32U)
#define DYNAMIC_LINKS_PER_NODE ((size_t)4U)
#define DYNAMIC_EVENT_CAPACITY ((size_t)1024U)
#define DYNAMIC_OBSERVATION_CAPACITY ((size_t)2048U)
#define DYNAMIC_DEFAULT_ROUNDS UINT32_C(2000)
#define DYNAMIC_MAX_ROUNDS UINT32_C(1000000)
#define DYNAMIC_DEFAULT_SEED UINT32_C(0x51A05EED)
#define DYNAMIC_STEP_MS UINT32_C(10)
#define DYNAMIC_TRACE_DEPTH ((size_t)16U)
#define DYNAMIC_ENDPOINT_Q1 ((ucn_endpoint_t)0x40U)
#define DYNAMIC_ENDPOINT_Q0 ((ucn_endpoint_t)0x60U)

typedef struct dynamic_network dynamic_network_t;

typedef enum dynamic_trace_kind {
    DYNAMIC_TRACE_LINK_TOGGLE = 1,
    DYNAMIC_TRACE_COST_CHANGE = 2,
    DYNAMIC_TRACE_HELLO_BURST = 3,
    DYNAMIC_TRACE_Q1_SEND = 4,
    DYNAMIC_TRACE_Q0_ENQUEUE = 5
} dynamic_trace_kind_t;

typedef struct dynamic_trace_entry {
    uint32_t round;
    uint8_t kind;
    uint8_t node_index;
    uint8_t link_index;
    uint16_t value;
} dynamic_trace_entry_t;

typedef struct dynamic_link_context {
    dynamic_network_t *network;
    size_t source_index;
    size_t target_index;
    size_t reverse_link_index;
    bool is_up;
    uint16_t route_cost;
    uint32_t tx_attempts;
    uint32_t tx_errors;
} dynamic_link_context_t;

typedef struct dynamic_event {
    bool occupied;
    uint32_t due_at_ms;
    uint32_t order;
    dynamic_link_context_t *source_context;
    size_t target_index;
    size_t ingress_link_index;
    size_t length;
    uint8_t frame[UCN_MAX_FRAME_BYTES];
} dynamic_event_t;

typedef struct dynamic_observation {
    bool occupied;
    uint32_t message_id;
    uint8_t deliveries;
} dynamic_observation_t;

typedef struct dynamic_rx_context {
    dynamic_network_t *network;
    size_t node_index;
} dynamic_rx_context_t;

struct dynamic_network {
    ucn_node_t nodes[DYNAMIC_NODE_COUNT];
    ucn_link_t links[DYNAMIC_NODE_COUNT][DYNAMIC_LINKS_PER_NODE];
    dynamic_link_context_t
        link_contexts[DYNAMIC_NODE_COUNT][DYNAMIC_LINKS_PER_NODE];
    dynamic_rx_context_t rx_contexts[DYNAMIC_NODE_COUNT];
    dynamic_event_t events[DYNAMIC_EVENT_CAPACITY];
    dynamic_observation_t observations[DYNAMIC_OBSERVATION_CAPACITY];
    dynamic_trace_entry_t trace[DYNAMIC_TRACE_DEPTH];
    uint32_t seed;
    uint32_t initial_seed;
    uint32_t round;
    uint32_t now_ms;
    uint32_t next_event_order;
    uint32_t next_message_id;
    uint32_t configured_rounds;
    uint16_t loss_per_mille;
    uint16_t duplicate_per_mille;
    uint16_t max_delay_ms;
    size_t trace_next;
    size_t event_count;
    size_t event_high_water;
    uint32_t frames_enqueued;
    uint32_t frames_delivered;
    uint32_t frames_dropped;
    uint32_t duplicate_frames_enqueued;
    uint32_t queue_backpressure;
    uint32_t receive_rejections;
    uint32_t business_delivered;
    bool faults_enabled;
    bool duplicate_business_delivery;
    bool fatal_receive_result;
    bool pump_guard_exceeded;
};

typedef struct resource_link_context {
    bool is_up;
    uint32_t sends;
} resource_link_context_t;

static uint32_t dynamic_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static uint32_t dynamic_read_u32_env(const char *name,
                                     uint32_t fallback,
                                     uint32_t maximum)
{
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return fallback;
    }
    value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value == 0UL ||
        value > (unsigned long)maximum) {
        return fallback;
    }
    return (uint32_t)value;
}

static void dynamic_record_trace(dynamic_network_t *network,
                                 dynamic_trace_kind_t kind,
                                 size_t node_index,
                                 size_t link_index,
                                 uint16_t value)
{
    dynamic_trace_entry_t *entry =
        &network->trace[network->trace_next % DYNAMIC_TRACE_DEPTH];

    entry->round = network->round;
    entry->kind = (uint8_t)kind;
    entry->node_index = (uint8_t)node_index;
    entry->link_index = (uint8_t)link_index;
    entry->value = value;
    network->trace_next++;
}

static int dynamic_fail(const dynamic_network_t *network,
                        const char *condition,
                        int line)
{
    size_t available = network->trace_next < DYNAMIC_TRACE_DEPTH ?
                           network->trace_next : DYNAMIC_TRACE_DEPTH;
    size_t first = network->trace_next >= available ?
                       network->trace_next - available : 0U;
    size_t index;

    printf("DYNAMIC_STRESS failed: %s (line=%d seed=0x%08" PRIX32
           " round=%" PRIu32 " events=%" PRIu32 ")\n",
           condition, line, network->initial_seed, network->round,
           network->configured_rounds);
    for (index = 0U; index < available; ++index) {
        const dynamic_trace_entry_t *entry =
            &network->trace[(first + index) % DYNAMIC_TRACE_DEPTH];

        printf("  trace round=%" PRIu32 " kind=%u node=%u link=%u value=%u\n",
               entry->round, (unsigned)entry->kind,
               (unsigned)entry->node_index, (unsigned)entry->link_index,
               (unsigned)entry->value);
    }
    return 1;
}

#define DYNAMIC_REQUIRE(network, condition) \
    do { \
        if (!(condition)) { \
            return dynamic_fail((network), #condition, __LINE__); \
        } \
    } while (0)

static size_t dynamic_peer_index(size_t node_index, size_t link_index)
{
    static const size_t positive_offsets[DYNAMIC_LINKS_PER_NODE] = {
        1U, 1U, 4U, 4U
    };
    size_t offset = positive_offsets[link_index];

    if (link_index == 0U || link_index == 2U) {
        return (node_index + offset) % DYNAMIC_NODE_COUNT;
    }
    return (node_index + DYNAMIC_NODE_COUNT - offset) % DYNAMIC_NODE_COUNT;
}

static size_t dynamic_reverse_link_index(size_t link_index)
{
    static const size_t reverse[DYNAMIC_LINKS_PER_NODE] = { 1U, 0U, 3U, 2U };

    return reverse[link_index];
}

static bool dynamic_enqueue_event(dynamic_link_context_t *context,
                                  const uint8_t *frame,
                                  size_t length,
                                  bool duplicate)
{
    dynamic_network_t *network = context->network;
    size_t index;
    uint32_t delay = 0U;

    for (index = 0U; index < DYNAMIC_EVENT_CAPACITY; ++index) {
        dynamic_event_t *event = &network->events[index];

        if (event->occupied) {
            continue;
        }
        if (network->faults_enabled && network->max_delay_ms > 0U) {
            delay = dynamic_random(&network->seed) %
                    ((uint32_t)network->max_delay_ms + 1U);
        }
        event->occupied = true;
        event->due_at_ms = network->now_ms + delay;
        event->order = network->next_event_order++;
        event->source_context = context;
        event->target_index = context->target_index;
        event->ingress_link_index = context->reverse_link_index;
        event->length = length;
        (void)memcpy(event->frame, frame, length);
        network->event_count++;
        network->frames_enqueued++;
        if (duplicate) {
            network->duplicate_frames_enqueued++;
        }
        if (network->event_count > network->event_high_water) {
            network->event_high_water = network->event_count;
        }
        return true;
    }
    network->queue_backpressure++;
    return false;
}

static ucn_result_t dynamic_link_send(ucn_link_t *link,
                                      const uint8_t *frame,
                                      size_t length)
{
    dynamic_link_context_t *context =
        (dynamic_link_context_t *)link->context;
    dynamic_network_t *network = context->network;

    context->tx_attempts++;
    if (!context->is_up) {
        context->tx_errors++;
        return UCN_ERR_LINK_DOWN;
    }
    if (length > UCN_MAX_FRAME_BYTES || length > link->mtu) {
        context->tx_errors++;
        return UCN_ERR_TOO_LARGE;
    }
    if (network->faults_enabled && network->loss_per_mille > 0U &&
        dynamic_random(&network->seed) % UINT32_C(1000) <
            network->loss_per_mille) {
        network->frames_dropped++;
        return UCN_OK;
    }
    if (!dynamic_enqueue_event(context, frame, length, false)) {
        context->tx_errors++;
        return UCN_ERR_NO_SPACE;
    }
    if (network->faults_enabled && network->duplicate_per_mille > 0U &&
        dynamic_random(&network->seed) % UINT32_C(1000) <
            network->duplicate_per_mille) {
        (void)dynamic_enqueue_event(context, frame, length, true);
    }
    return UCN_OK;
}

static ucn_result_t dynamic_link_status(const ucn_link_t *link,
                                        ucn_link_status_t *status)
{
    const dynamic_link_context_t *context =
        (const dynamic_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = context->tx_errors;
    status->rx_errors = 0U;
    return UCN_OK;
}

static ucn_result_t dynamic_link_metrics(const ucn_link_t *link,
                                         ucn_link_metrics_t *metrics)
{
    const dynamic_link_context_t *context =
        (const dynamic_link_context_t *)link->context;
    const dynamic_network_t *network = context->network;

    (void)memset(metrics, 0, sizeof(*metrics));
    metrics->route_cost_valid = true;
    metrics->route_cost = context->route_cost;
    metrics->tx_failure_rate_valid = true;
    metrics->tx_failure_per_mille = network->loss_per_mille;
    metrics->queue_pressure_valid = true;
    metrics->queue_pressure_per_mille =
        (uint16_t)((network->event_count * UINT32_C(1000)) /
                   DYNAMIC_EVENT_CAPACITY);
    return UCN_OK;
}

static const ucn_link_ops_t DYNAMIC_LINK_OPS = {
    NULL,
    dynamic_link_send,
    NULL,
    dynamic_link_status,
    NULL,
    dynamic_link_metrics
};

static uint32_t dynamic_read_payload_id(const ucn_frame_t *frame)
{
    return ((uint32_t)frame->payload[0] << 24U) |
           ((uint32_t)frame->payload[1] << 16U) |
           ((uint32_t)frame->payload[2] << 8U) |
           (uint32_t)frame->payload[3];
}

static void dynamic_write_payload_id(uint8_t payload[4], uint32_t message_id)
{
    payload[0] = (uint8_t)(message_id >> 24U);
    payload[1] = (uint8_t)(message_id >> 16U);
    payload[2] = (uint8_t)(message_id >> 8U);
    payload[3] = (uint8_t)message_id;
}

static void dynamic_receive(void *context, const ucn_frame_t *frame)
{
    dynamic_rx_context_t *rx = (dynamic_rx_context_t *)context;
    dynamic_network_t *network = rx->network;
    uint32_t message_id;
    dynamic_observation_t *observation;

    (void)rx->node_index;
    if (frame->message_type < 0x40U || frame->payload_length < 4U) {
        return;
    }
    message_id = dynamic_read_payload_id(frame);
    observation =
        &network->observations[message_id % DYNAMIC_OBSERVATION_CAPACITY];
    if (observation->occupied && observation->message_id == message_id) {
        observation->deliveries++;
        network->duplicate_business_delivery = true;
        return;
    }
    observation->occupied = true;
    observation->message_id = message_id;
    observation->deliveries = 1U;
    network->business_delivered++;
}

static bool dynamic_message_was_delivered(const dynamic_network_t *network,
                                          uint32_t message_id)
{
    const dynamic_observation_t *observation =
        &network->observations[message_id % DYNAMIC_OBSERVATION_CAPACITY];

    return observation->occupied && observation->message_id == message_id &&
           observation->deliveries == 1U;
}

static void dynamic_accept_receive_result(dynamic_network_t *network,
                                          ucn_result_t result)
{
    if (result == UCN_OK || result == UCN_ERR_REPLAY ||
        result == UCN_ERR_NOT_FOUND || result == UCN_ERR_NO_SPACE ||
        result == UCN_ERR_LINK_DOWN || result == UCN_ERR_ACCESS ||
        result == UCN_ERR_TTL) {
        if (result != UCN_OK) {
            network->receive_rejections++;
        }
        return;
    }
    network->fatal_receive_result = true;
}

static void dynamic_pump_due_events(dynamic_network_t *network)
{
    size_t guard = 0U;

    while (guard < DYNAMIC_EVENT_CAPACITY * 8U) {
        dynamic_event_t event;
        size_t best_index = DYNAMIC_EVENT_CAPACITY;
        size_t index;

        for (index = 0U; index < DYNAMIC_EVENT_CAPACITY; ++index) {
            const dynamic_event_t *candidate = &network->events[index];

            if (!candidate->occupied || candidate->due_at_ms > network->now_ms) {
                continue;
            }
            if (best_index == DYNAMIC_EVENT_CAPACITY ||
                candidate->due_at_ms < network->events[best_index].due_at_ms ||
                (candidate->due_at_ms == network->events[best_index].due_at_ms &&
                 candidate->order < network->events[best_index].order)) {
                best_index = index;
            }
        }
        if (best_index == DYNAMIC_EVENT_CAPACITY) {
            return;
        }
        event = network->events[best_index];
        (void)memset(&network->events[best_index], 0,
                     sizeof(network->events[best_index]));
        network->event_count--;
        if (event.source_context->is_up) {
            ucn_result_t result = ucn_node_receive(
                &network->nodes[event.target_index],
                &network->links[event.target_index][event.ingress_link_index],
                event.frame, event.length);

            dynamic_accept_receive_result(network, result);
            network->frames_delivered++;
        } else {
            network->frames_dropped++;
        }
        guard++;
    }
    network->pump_guard_exceeded = true;
}

static bool dynamic_step_result_is_expected(ucn_result_t result)
{
    return result == UCN_OK || result == UCN_ERR_NOT_FOUND ||
           result == UCN_ERR_NO_SPACE || result == UCN_ERR_LINK_DOWN ||
           result == UCN_ERR_TTL ||
           result == UCN_ERR_ACCESS;
}

static void dynamic_step_all(dynamic_network_t *network, uint32_t delta_ms)
{
    size_t index;

    network->now_ms += delta_ms;
    for (index = 0U; index < DYNAMIC_NODE_COUNT; ++index) {
        ucn_result_t result =
            ucn_node_step(&network->nodes[index], network->now_ms);

        if (!dynamic_step_result_is_expected(result)) {
            network->fatal_receive_result = true;
        }
    }
    dynamic_pump_due_events(network);
}

static void dynamic_advance(dynamic_network_t *network, uint32_t duration_ms)
{
    uint32_t remaining = duration_ms;

    while (remaining > 0U) {
        uint32_t delta = remaining > DYNAMIC_STEP_MS ?
                             DYNAMIC_STEP_MS : remaining;

        dynamic_step_all(network, delta);
        remaining -= delta;
        if (network->fatal_receive_result || network->pump_guard_exceeded) {
            return;
        }
    }
}

static int dynamic_broadcast_all_hello(dynamic_network_t *network)
{
    size_t node_index;
    size_t link_index;

    for (node_index = 0U; node_index < DYNAMIC_NODE_COUNT; ++node_index) {
        for (link_index = 0U; link_index < DYNAMIC_LINKS_PER_NODE; ++link_index) {
            ucn_result_t result;

            if (!network->link_contexts[node_index][link_index].is_up) {
                continue;
            }
            result = ucn_node_broadcast_hello(
                &network->nodes[node_index],
                &network->links[node_index][link_index], network->now_ms);
            if (result != UCN_OK && result != UCN_ERR_NO_SPACE) {
                return 1;
            }
        }
    }
    dynamic_record_trace(network, DYNAMIC_TRACE_HELLO_BURST, 0U, 0U, 0U);
    return 0;
}

static int dynamic_network_init(dynamic_network_t *network,
                                uint32_t seed,
                                uint32_t rounds)
{
    size_t node_index;
    size_t link_index;

    (void)memset(network, 0, sizeof(*network));
    network->seed = seed;
    network->initial_seed = seed;
    network->configured_rounds = rounds;
    network->next_message_id = 1U;
    network->now_ms = 100U;
    for (node_index = 0U; node_index < DYNAMIC_NODE_COUNT; ++node_index) {
        ucn_config_t config;

        config.network_id = UINT32_C(0x44594E53);
        config.node_id = (ucn_node_id_t)(node_index + 1U);
        config.default_hop_limit = UCN_MAX_HOPS;
        if (ucn_node_init(&network->nodes[node_index], &config) != UCN_OK ||
            ucn_node_set_join_policy(&network->nodes[node_index], UCN_JOIN_OPEN,
                                     NULL, NULL) != UCN_OK) {
            return 1;
        }
        network->rx_contexts[node_index].network = network;
        network->rx_contexts[node_index].node_index = node_index;
        ucn_node_set_rx_handler(&network->nodes[node_index], dynamic_receive,
                                &network->rx_contexts[node_index]);
        for (link_index = 0U; link_index < DYNAMIC_LINKS_PER_NODE; ++link_index) {
            size_t target_index = dynamic_peer_index(node_index, link_index);
            ucn_link_t *link = &network->links[node_index][link_index];
            dynamic_link_context_t *context =
                &network->link_contexts[node_index][link_index];

            context->network = network;
            context->source_index = node_index;
            context->target_index = target_index;
            context->reverse_link_index =
                dynamic_reverse_link_index(link_index);
            context->is_up = true;
            context->route_cost = (uint16_t)(10U + link_index * 5U);
            link->ops = &DYNAMIC_LINK_OPS;
            link->context = context;
            link->link_id = (uint8_t)(link_index + 1U);
            link->mtu = UCN_MAX_FRAME_BYTES;
            link->peer_node_id = 0U;
        }
    }
    dynamic_pump_due_events(network);
    if (dynamic_broadcast_all_hello(network) != 0) {
        return 1;
    }
    dynamic_pump_due_events(network);
    for (node_index = 0U; node_index < DYNAMIC_NODE_COUNT; ++node_index) {
        if (network->nodes[node_index].link_count != DYNAMIC_LINKS_PER_NODE ||
            ucn_node_neighbor_count(&network->nodes[node_index],
                                    UCN_NEIGHBOR_ADMITTED) !=
                DYNAMIC_LINKS_PER_NODE) {
            return 1;
        }
    }
    return 0;
}

static void dynamic_set_link_pair_up(dynamic_network_t *network,
                                     size_t node_index,
                                     size_t link_index,
                                     bool is_up)
{
    dynamic_link_context_t *first =
        &network->link_contexts[node_index][link_index];
    dynamic_link_context_t *reverse =
        &network->link_contexts[first->target_index][first->reverse_link_index];

    first->is_up = is_up;
    reverse->is_up = is_up;
    dynamic_record_trace(network, DYNAMIC_TRACE_LINK_TOGGLE, node_index,
                         link_index, is_up ? 1U : 0U);
}

static void dynamic_set_link_pair_cost(dynamic_network_t *network,
                                       size_t node_index,
                                       size_t link_index,
                                       uint16_t route_cost)
{
    dynamic_link_context_t *first =
        &network->link_contexts[node_index][link_index];
    dynamic_link_context_t *reverse =
        &network->link_contexts[first->target_index][first->reverse_link_index];

    first->route_cost = route_cost;
    reverse->route_cost = route_cost;
    dynamic_record_trace(network, DYNAMIC_TRACE_COST_CHANGE, node_index,
                         link_index, route_cost);
}

static void dynamic_restore_all_links(dynamic_network_t *network)
{
    size_t node_index;
    size_t link_index;

    for (node_index = 0U; node_index < DYNAMIC_NODE_COUNT; ++node_index) {
        for (link_index = 0U; link_index < DYNAMIC_LINKS_PER_NODE; ++link_index) {
            network->link_contexts[node_index][link_index].is_up = true;
        }
    }
}

static const ucn_route_entry_t *dynamic_find_route(const ucn_node_t *node,
                                                   ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        if (node->routes[index].valid &&
            node->routes[index].destination == destination) {
            return &node->routes[index];
        }
    }
    return NULL;
}

static ucn_link_t *dynamic_find_direct_link(const ucn_node_t *node,
                                            ucn_node_id_t destination)
{
    size_t index;

    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index]->peer_node_id == destination) {
            return node->links[index];
        }
    }
    return NULL;
}

static ucn_link_t *dynamic_find_epoch_link(const ucn_node_t *node,
                                           ucn_node_id_t destination,
                                           uint16_t route_epoch)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        const ucn_route_entry_t *route = &node->routes[index];

        if (!route->valid || route->destination != destination ||
            (!route->is_static &&
             ucn_deadline_expired(node->now_ms, route->expires_at_ms))) {
            continue;
        }
        if (route->route_epoch == route_epoch) {
            return route->egress_link;
        }
        if (route->previous_valid &&
            route->previous_route_epoch == route_epoch &&
            !ucn_deadline_expired(node->now_ms,
                                  route->previous_expires_at_ms)) {
            return route->previous_egress_link;
        }
    }
    return NULL;
}

static bool dynamic_routes_have_no_loop(const dynamic_network_t *network)
{
    size_t source_index;
    size_t route_index;

    for (source_index = 0U; source_index < DYNAMIC_NODE_COUNT; ++source_index) {
        for (route_index = 0U; route_index < UCN_MAX_ROUTES; ++route_index) {
            const ucn_route_entry_t *origin_route =
                &network->nodes[source_index].routes[route_index];
            ucn_node_id_t destination;
            uint16_t route_epoch;
            size_t current_index = source_index;
            uint32_t visited = 0U;
            size_t chain[DYNAMIC_NODE_COUNT];
            size_t hop;

            if (!origin_route->valid) {
                continue;
            }
            destination = origin_route->destination;
            route_epoch = origin_route->route_epoch;
            for (hop = 0U; hop < DYNAMIC_NODE_COUNT; ++hop) {
                ucn_link_t *egress_link;
                ucn_node_id_t next_hop;

                if ((visited & (UINT32_C(1) << current_index)) != 0U) {
                    size_t chain_index;

                    printf("DYNAMIC_ROUTE_LOOP source=%zu destination=%" PRIu32
                           " epoch=%u repeated=%zu hop=%zu chain=",
                           source_index + 1U, destination,
                           (unsigned)route_epoch, current_index + 1U, hop);
                    for (chain_index = 0U; chain_index < hop; ++chain_index) {
                        printf("%s%zu", chain_index == 0U ? "" : "->",
                               chain[chain_index] + 1U);
                    }
                    printf("->%zu\n", current_index + 1U);
                    return false;
                }
                chain[hop] = current_index;
                visited |= UINT32_C(1) << current_index;
                if (dynamic_find_direct_link(&network->nodes[current_index],
                                             destination) != NULL) {
                    break;
                }
                egress_link = dynamic_find_epoch_link(
                    &network->nodes[current_index], destination, route_epoch);
                if (egress_link == NULL) {
                    break;
                }
                next_hop = egress_link->peer_node_id;
                if (next_hop == destination) {
                    break;
                }
                if (next_hop == 0U || next_hop > DYNAMIC_NODE_COUNT) {
                    printf("DYNAMIC_ROUTE_BAD_NEXT source=%zu destination=%" PRIu32
                           " current=%zu next=%" PRIu32 "\n",
                           source_index + 1U, destination, current_index + 1U,
                           next_hop);
                    return false;
                }
                current_index = (size_t)(next_hop - 1U);
            }
            if (hop == DYNAMIC_NODE_COUNT) {
                printf("DYNAMIC_ROUTE_TOO_LONG source=%zu destination=%" PRIu32
                       "\n", source_index + 1U, destination);
                return false;
            }
        }
    }
    return true;
}

static bool dynamic_tables_within_bounds(const dynamic_network_t *network)
{
    size_t node_index;

    for (node_index = 0U; node_index < DYNAMIC_NODE_COUNT; ++node_index) {
        const ucn_node_t *node = &network->nodes[node_index];
        size_t route_count = 0U;
        size_t candidate_count = 0U;
        size_t discovery_count = 0U;
        size_t path_count = 0U;
        size_t flow_count = 0U;
        size_t index;

        if (node->link_count > UCN_MAX_LINKS) {
            return false;
        }
        for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
            route_count += node->routes[index].valid ? 1U : 0U;
        }
        for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
            candidate_count += node->candidates[index].valid ? 1U : 0U;
        }
        for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
            discovery_count += node->discoveries[index].active ? 1U : 0U;
        }
        for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
            path_count += node->path_state.entries[index].occupied ? 1U : 0U;
        }
        for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
            flow_count += node->policy_state.flows[index].occupied ? 1U : 0U;
        }
        if (route_count > UCN_MAX_ROUTES ||
            candidate_count > UCN_MAX_CANDIDATE_ROUTES ||
            discovery_count > UCN_MAX_ROUTE_DISCOVERIES ||
            path_count > UCN_MAX_PATH_FORWARD_ENTRIES ||
            flow_count > UCN_MAX_POLICY_FLOWS) {
            return false;
        }
    }
    return network->event_count <= DYNAMIC_EVENT_CAPACITY;
}

static ucn_result_t dynamic_send_q1(dynamic_network_t *network,
                                    size_t source_index,
                                    size_t destination_index,
                                    uint32_t message_id)
{
    uint8_t payload[4];

    dynamic_write_payload_id(payload, message_id);
    dynamic_record_trace(network, DYNAMIC_TRACE_Q1_SEND, source_index, 0U,
                         (uint16_t)(message_id & UINT16_MAX));
    return ucn_node_send_endpoint(
        &network->nodes[source_index],
        (ucn_node_id_t)(destination_index + 1U), DYNAMIC_ENDPOINT_Q1,
        UCN_TRAFFIC_Q1_REALTIME, payload, (uint16_t)sizeof(payload));
}

static ucn_result_t dynamic_enqueue_q0(dynamic_network_t *network,
                                       size_t source_index,
                                       size_t destination_index,
                                       uint32_t message_id)
{
    ucn_send_request_t request;
    uint8_t payload[4];

    (void)memset(&request, 0, sizeof(request));
    dynamic_write_payload_id(payload, message_id);
    request.destination = (ucn_node_id_t)(destination_index + 1U);
    request.message_type = DYNAMIC_ENDPOINT_Q0;
    request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    request.delivery = UCN_DELIVERY_BEST_EFFORT;
    request.deadline_ms = network->now_ms + UINT32_C(500);
    request.payload = payload;
    request.payload_length = (uint16_t)sizeof(payload);
    dynamic_record_trace(network, DYNAMIC_TRACE_Q0_ENQUEUE, source_index, 0U,
                         (uint16_t)(message_id & UINT16_MAX));
    return ucn_node_enqueue(&network->nodes[source_index], &request);
}

static bool dynamic_send_result_is_expected(ucn_result_t result)
{
    return result == UCN_OK || result == UCN_ERR_NOT_FOUND ||
           result == UCN_ERR_NO_SPACE || result == UCN_ERR_LINK_DOWN ||
           result == UCN_ERR_TTL;
}

static bool dynamic_wait_for_message(dynamic_network_t *network,
                                     uint32_t message_id,
                                     uint32_t timeout_ms)
{
    uint32_t waited = 0U;

    while (waited < timeout_ms) {
        if (dynamic_message_was_delivered(network, message_id)) {
            return true;
        }
        dynamic_step_all(network, DYNAMIC_STEP_MS);
        if (network->fatal_receive_result || network->pump_guard_exceeded ||
            network->duplicate_business_delivery) {
            return false;
        }
        waited += DYNAMIC_STEP_MS;
    }
    return dynamic_message_was_delivered(network, message_id);
}

static int dynamic_initial_discovery_and_repair(dynamic_network_t *network)
{
    const size_t source_index = 0U;
    const size_t destination_index = 16U;
    const ucn_route_entry_t *route;
    dynamic_link_context_t *context;
    uint32_t message_id = network->next_message_id++;
    bool repaired = false;
    uint8_t attempt;
    ucn_result_t result;

    network->faults_enabled = true;
    network->loss_per_mille = 0U;
    network->duplicate_per_mille = 80U;
    network->max_delay_ms = 5U;
    result = dynamic_send_q1(network, source_index, destination_index, message_id);
    DYNAMIC_REQUIRE(network, result == UCN_OK);
    DYNAMIC_REQUIRE(network,
                    dynamic_wait_for_message(network, message_id, UINT32_C(3000)));
    route = dynamic_find_route(&network->nodes[source_index],
                               (ucn_node_id_t)(destination_index + 1U));
    DYNAMIC_REQUIRE(network, route != NULL && route->egress_link != NULL);
    context = (dynamic_link_context_t *)route->egress_link->context;
    dynamic_set_link_pair_up(network, context->source_index,
                             route->egress_link->link_id - 1U, false);

    /* The first post-failure call may only observe LINK_DOWN and emit RERR.
     * Retry Q1 Latest on a fixed schedule after the one-second discovery
     * window; this is a bounded product retry, never an infinite Core loop. */
    for (attempt = 0U; attempt < 5U; ++attempt) {
        message_id = network->next_message_id++;
        result = dynamic_send_q1(network, source_index, destination_index,
                                 message_id);
        DYNAMIC_REQUIRE(network, dynamic_send_result_is_expected(result));
        if (dynamic_wait_for_message(network, message_id, UINT32_C(1500))) {
            repaired = true;
            break;
        }
    }
    DYNAMIC_REQUIRE(network, repaired);
    DYNAMIC_REQUIRE(network, !network->duplicate_business_delivery);
    return 0;
}

static int dynamic_random_fault_run(dynamic_network_t *network)
{
    uint32_t round;

    network->loss_per_mille = 25U;
    network->duplicate_per_mille = 80U;
    network->max_delay_ms = 7U;
    for (round = 0U; round < network->configured_rounds; ++round) {
        size_t source_index;
        size_t destination_index;
        uint32_t message_id;
        ucn_result_t result;

        network->round = round;
        if ((round % UINT32_C(41)) == 0U) {
            size_t node_index = dynamic_random(&network->seed) % DYNAMIC_NODE_COUNT;
            size_t link_index =
                dynamic_random(&network->seed) % DYNAMIC_LINKS_PER_NODE;
            dynamic_link_context_t *context =
                &network->link_contexts[node_index][link_index];

            dynamic_set_link_pair_up(network, node_index, link_index,
                                     !context->is_up);
        }
        if ((round % UINT32_C(47)) == 0U) {
            size_t node_index = dynamic_random(&network->seed) % DYNAMIC_NODE_COUNT;
            size_t link_index =
                dynamic_random(&network->seed) % DYNAMIC_LINKS_PER_NODE;
            uint16_t cost = (uint16_t)(1U +
                (dynamic_random(&network->seed) % UINT32_C(200)));

            dynamic_set_link_pair_cost(network, node_index, link_index, cost);
        }
        if ((round % UINT32_C(251)) == 0U) {
            DYNAMIC_REQUIRE(network, dynamic_broadcast_all_hello(network) == 0);
        }

        source_index = dynamic_random(&network->seed) % DYNAMIC_NODE_COUNT;
        destination_index = dynamic_random(&network->seed) % DYNAMIC_NODE_COUNT;
        if (destination_index == source_index) {
            destination_index = (destination_index + 7U) % DYNAMIC_NODE_COUNT;
        }
        message_id = network->next_message_id++;
        result = dynamic_send_q1(network, source_index, destination_index,
                                 message_id);
        DYNAMIC_REQUIRE(network, dynamic_send_result_is_expected(result));

        if ((round % UINT32_C(7)) == 0U) {
            source_index = dynamic_random(&network->seed) % DYNAMIC_NODE_COUNT;
            destination_index =
                dynamic_random(&network->seed) % DYNAMIC_NODE_COUNT;
            if (destination_index == source_index) {
                destination_index =
                    (destination_index + 11U) % DYNAMIC_NODE_COUNT;
            }
            message_id = network->next_message_id++;
            result = dynamic_enqueue_q0(network, source_index,
                                        destination_index, message_id);
            DYNAMIC_REQUIRE(network, dynamic_send_result_is_expected(result));
        }

        dynamic_step_all(network, DYNAMIC_STEP_MS);
        DYNAMIC_REQUIRE(network, !network->fatal_receive_result);
        DYNAMIC_REQUIRE(network, !network->pump_guard_exceeded);
        DYNAMIC_REQUIRE(network, !network->duplicate_business_delivery);
        DYNAMIC_REQUIRE(network, dynamic_tables_within_bounds(network));
        if ((round % UINT32_C(17)) == 0U) {
            DYNAMIC_REQUIRE(network, dynamic_routes_have_no_loop(network));
        }
    }
    return 0;
}

static int dynamic_cleanup_and_recovery(dynamic_network_t *network)
{
    size_t node_index;
    size_t link_index;
    uint32_t message_id;
    uint32_t total_heartbeats = 0U;
    uint32_t total_route_requests = 0U;
    uint32_t cleanup_jump = UCN_ROUTE_ENTRY_LIFETIME_MS;
    ucn_result_t result;

    network->faults_enabled = false;
    network->loss_per_mille = 0U;
    network->duplicate_per_mille = 0U;
    network->max_delay_ms = 0U;
    dynamic_restore_all_links(network);
    dynamic_advance(network, UINT32_C(100));
    if (UCN_NEIGHBOR_REMOVE_TIMEOUT_MS > cleanup_jump) {
        cleanup_jump = UCN_NEIGHBOR_REMOVE_TIMEOUT_MS;
    }
    dynamic_step_all(network, cleanup_jump + UINT32_C(100));
    DYNAMIC_REQUIRE(network, dynamic_broadcast_all_hello(network) == 0);
    dynamic_advance(network, UINT32_C(500));

    for (node_index = 0U; node_index < DYNAMIC_NODE_COUNT; ++node_index) {
        const ucn_node_stats_t *stats =
            ucn_node_get_stats(&network->nodes[node_index]);

        DYNAMIC_REQUIRE(network,
                        network->nodes[node_index].link_count ==
                            DYNAMIC_LINKS_PER_NODE);
        DYNAMIC_REQUIRE(network,
                        ucn_node_neighbor_count(&network->nodes[node_index],
                                                UCN_NEIGHBOR_ADMITTED) ==
                            DYNAMIC_LINKS_PER_NODE);
        total_heartbeats += stats->heartbeat_requests_sent;
        total_heartbeats += stats->heartbeat_acks_sent;
        total_route_requests += stats->route_requests_sent;
        for (link_index = 0U; link_index < UCN_MAX_ROUTES; ++link_index) {
            DYNAMIC_REQUIRE(network,
                            !network->nodes[node_index].routes[link_index].valid);
        }
    }
    DYNAMIC_REQUIRE(network, total_heartbeats > 0U);
    DYNAMIC_REQUIRE(network, total_route_requests > 0U);
    DYNAMIC_REQUIRE(network, dynamic_routes_have_no_loop(network));

    message_id = network->next_message_id++;
    result = dynamic_send_q1(network, 0U, 16U, message_id);
    DYNAMIC_REQUIRE(network, result == UCN_OK);
    DYNAMIC_REQUIRE(network,
                    dynamic_wait_for_message(network, message_id, UINT32_C(3000)));
    DYNAMIC_REQUIRE(network, !network->duplicate_business_delivery);
    return 0;
}

static ucn_result_t resource_link_send(ucn_link_t *link,
                                       const uint8_t *frame,
                                       size_t length)
{
    resource_link_context_t *context =
        (resource_link_context_t *)link->context;

    (void)frame;
    (void)length;
    if (!context->is_up) {
        return UCN_ERR_LINK_DOWN;
    }
    context->sends++;
    return UCN_OK;
}

static ucn_result_t resource_link_status(const ucn_link_t *link,
                                         ucn_link_status_t *status)
{
    const resource_link_context_t *context =
        (const resource_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t RESOURCE_LINK_OPS = {
    NULL, resource_link_send, NULL, resource_link_status, NULL, NULL
};

static void resource_init_config(ucn_config_t *config, ucn_node_id_t node_id)
{
    config->network_id = UINT32_C(0x52455343);
    config->node_id = node_id;
    config->default_hop_limit = UCN_MAX_HOPS;
}

static int dynamic_test_fixed_resource_bounds(void)
{
    ucn_node_t neighbor_node;
    ucn_node_t route_node;
    ucn_link_t candidate_links[UCN_MAX_NEIGHBORS + 1U];
    resource_link_context_t candidate_contexts[UCN_MAX_NEIGHBORS + 1U];
    ucn_link_t route_link;
    resource_link_context_t route_context;
    ucn_config_t config;
    ucn_policy_path_config_t policy_path;
    ucn_path_state_t path_state;
    ucn_path_forward_config_t path_config;
#if UCN_FEATURE_SERVICE
    ucn_service_router_t router;
    ucn_service_message_t service_message;
    uint8_t payload = 0xA5U;
#endif
    size_t index;
#if UCN_FEATURE_SERVICE
    static const ucn_service_binding_t bindings[] = {
        {
            DYNAMIC_ENDPOINT_Q1, 2U, 4U,
            UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q1_REALTIME),
            UCN_SERVICE_DELIVERY_Q1_LATEST,
            UCN_SERVICE_SOURCE_MASK(1U), true, true, false
        },
        {
            DYNAMIC_ENDPOINT_Q0, 2U, 4U,
            UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q0_CRITICAL),
            UCN_SERVICE_DELIVERY_Q0_FIFO,
            UCN_SERVICE_SOURCE_MASK(1U), true, true, false
        }
    };
    const ucn_service_router_config_t router_config = {
        UINT32_C(1), bindings,
        (uint8_t)(sizeof(bindings) / sizeof(bindings[0]))
    };
#endif

    (void)memset(&neighbor_node, 0, sizeof(neighbor_node));
    (void)memset(candidate_links, 0, sizeof(candidate_links));
    (void)memset(candidate_contexts, 0, sizeof(candidate_contexts));
    resource_init_config(&config, UINT32_C(1));
    TEST_ASSERT(ucn_node_init(&neighbor_node, &config) == UCN_OK);
    for (index = 0U; index < UCN_MAX_NEIGHBORS + 1U; ++index) {
        candidate_contexts[index].is_up = true;
        candidate_links[index].ops = &RESOURCE_LINK_OPS;
        candidate_links[index].context = &candidate_contexts[index];
        candidate_links[index].link_id = (uint8_t)(index + 1U);
        candidate_links[index].mtu = UCN_MAX_FRAME_BYTES;
        candidate_links[index].peer_node_id =
            (ucn_node_id_t)(UINT32_C(100) + index);
    }
    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        TEST_ASSERT(ucn_node_observe_neighbor(&neighbor_node,
                                              &candidate_links[index], 1U) ==
                    UCN_OK);
    }
    TEST_ASSERT(ucn_node_observe_neighbor(
                    &neighbor_node, &candidate_links[UCN_MAX_NEIGHBORS], 1U) ==
                UCN_ERR_NO_SPACE);
    (void)ucn_node_step(&neighbor_node,
                        UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS + 2U);
    TEST_ASSERT(ucn_node_neighbor_count(&neighbor_node,
                                        UCN_NEIGHBOR_CANDIDATE) == 0U);
    TEST_ASSERT(ucn_node_observe_neighbor(
                    &neighbor_node, &candidate_links[UCN_MAX_NEIGHBORS],
                    UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS + 3U) == UCN_OK);

    (void)memset(&route_node, 0, sizeof(route_node));
    (void)memset(&route_link, 0, sizeof(route_link));
    (void)memset(&route_context, 0, sizeof(route_context));
    route_context.is_up = true;
    route_link.ops = &RESOURCE_LINK_OPS;
    route_link.context = &route_context;
    route_link.link_id = 1U;
    route_link.mtu = UCN_MAX_FRAME_BYTES;
    route_link.peer_node_id = UINT32_C(2);
    TEST_ASSERT(ucn_node_init(&route_node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&route_node, &route_link) == UCN_OK);
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        TEST_ASSERT(ucn_node_add_route(
                        &route_node, (ucn_node_id_t)(UINT32_C(1000) + index),
                        &route_link) == UCN_OK);
    }
    TEST_ASSERT(ucn_node_add_route(&route_node, UINT32_C(2000), &route_link) ==
                UCN_ERR_NO_SPACE);

    (void)memset(&policy_path, 0, sizeof(policy_path));
    policy_path.destination = UINT32_C(9);
    policy_path.egress_link = &route_link;
    policy_path.verified = true;
    for (index = 0U; index < UCN_MAX_POLICY_PATHS; ++index) {
        policy_path.local_path_id = (uint16_t)(index + 1U);
        TEST_ASSERT(ucn_node_set_policy_path(&route_node, &policy_path) ==
                    UCN_OK);
    }
    policy_path.local_path_id = UINT16_C(99);
    TEST_ASSERT(ucn_node_set_policy_path(&route_node, &policy_path) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_node_clear_policy_path(
                    &route_node, (uint16_t)UCN_MAX_POLICY_PATHS) == UCN_OK);
    TEST_ASSERT(ucn_node_set_policy_path(&route_node, &policy_path) == UCN_OK);

    for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
        TEST_ASSERT(ucn_node_bind_q1_flow(
                        &route_node, UINT32_C(9),
                        (ucn_endpoint_t)(0x40U + index), 1U, 200U) == UCN_OK);
    }
    TEST_ASSERT(ucn_node_bind_q1_flow(&route_node, UINT32_C(9), 0x60U, 1U,
                                      200U) == UCN_ERR_NO_SPACE);
    (void)ucn_node_step(&route_node, 201U);
    TEST_ASSERT(ucn_node_bind_q1_flow(&route_node, UINT32_C(9), 0x60U, 1U,
                                      200U) == UCN_OK);

    (void)memset(&path_state, 0, sizeof(path_state));
    (void)memset(&path_config, 0, sizeof(path_config));
    path_config.owner = UINT32_C(1);
    path_config.owner_session_id = UINT32_C(1);
    path_config.destination = UINT32_C(9);
    path_config.next_hop = UINT32_C(2);
    path_config.remaining_hops = 1U;
    path_config.egress_link = &route_link;
    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        path_config.path_id = (ucn_path_id_t)(index + 1U);
        path_config.expires_at_ms = (uint32_t)(UINT32_C(1000) + index);
        TEST_ASSERT(ucn_path_install(&path_state, &path_config) == UCN_OK);
    }
    path_config.path_id = UINT32_C(99);
    path_config.expires_at_ms = UINT32_C(2000);
    TEST_ASSERT(ucn_path_install(&path_state, &path_config) == UCN_ERR_NO_SPACE);
    ucn_path_expire(&path_state, UINT32_C(1100));
    TEST_ASSERT(ucn_path_install(&path_state, &path_config) == UCN_OK);

#if UCN_FEATURE_SERVICE
    (void)memset(&router, 0, sizeof(router));
    TEST_ASSERT(ucn_service_router_init(&router, &router_config) == UCN_OK);
    for (index = 0U; index < UCN_SERVICE_Q0_INBOX_DEPTH; ++index) {
        TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), 1U,
                                     DYNAMIC_ENDPOINT_Q0,
                                     UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) ==
                    UCN_OK);
    }
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), 1U,
                                 DYNAMIC_ENDPOINT_Q0,
                                 UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_service_inbox_take(&router, 2U, DYNAMIC_ENDPOINT_Q0,
                                       &service_message) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), 1U,
                                 DYNAMIC_ENDPOINT_Q0,
                                 UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) ==
                UCN_OK);
    while (ucn_service_inbox_take(&router, 2U, DYNAMIC_ENDPOINT_Q0,
                                  &service_message) == UCN_OK) {
    }

    for (index = 0U; index < UCN_SERVICE_REMOTE_TX_Q0_DEPTH; ++index) {
        TEST_ASSERT(ucn_service_send(&router, UINT32_C(2), 1U,
                                     DYNAMIC_ENDPOINT_Q0,
                                     UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) ==
                    UCN_OK);
    }
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(2), 1U,
                                 DYNAMIC_ENDPOINT_Q0,
                                 UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) ==
                UCN_ERR_NO_SPACE);
    while (ucn_service_remote_tx_take(&router, &service_message) == UCN_OK) {
    }
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(2), 1U,
                                 DYNAMIC_ENDPOINT_Q0,
                                 UCN_TRAFFIC_Q0_CRITICAL, &payload, 1U) ==
                UCN_OK);
    TEST_ASSERT(ucn_service_remote_tx_take(&router, &service_message) == UCN_OK);

    for (index = 0U; index < UCN_SERVICE_REMOTE_TX_Q1_DEPTH; ++index) {
        TEST_ASSERT(ucn_service_send(
                        &router, (ucn_node_id_t)(UINT32_C(10) + index), 1U,
                        DYNAMIC_ENDPOINT_Q1, UCN_TRAFFIC_Q1_REALTIME,
                        &payload, 1U) == UCN_OK);
    }
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(100), 1U,
                                 DYNAMIC_ENDPOINT_Q1,
                                 UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_service_remote_tx_take(&router, &service_message) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(100), 1U,
                                 DYNAMIC_ENDPOINT_Q1,
                                 UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_OK);
#endif
    return 0;
}

int test_dynamic_stress(void)
{
    static dynamic_network_t network;
    uint32_t seed = dynamic_read_u32_env("UCN_STRESS_SEED",
                                         DYNAMIC_DEFAULT_SEED, UINT32_MAX);
    uint32_t rounds = dynamic_read_u32_env("UCN_STRESS_EVENTS",
                                           DYNAMIC_DEFAULT_ROUNDS,
                                           DYNAMIC_MAX_ROUNDS);

    TEST_ASSERT(dynamic_test_fixed_resource_bounds() == 0);
    if (UCN_MAX_LINKS < DYNAMIC_LINKS_PER_NODE ||
        UCN_MAX_NEIGHBORS < DYNAMIC_LINKS_PER_NODE) {
        printf("DYNAMIC_STRESS scenario skipped: configured links=%zu neighbors=%zu "
               "require at least %zu per node\n",
               (size_t)UCN_MAX_LINKS, (size_t)UCN_MAX_NEIGHBORS,
               DYNAMIC_LINKS_PER_NODE);
        return 0;
    }
    TEST_ASSERT(dynamic_network_init(&network, seed, rounds) == 0);
    if (dynamic_initial_discovery_and_repair(&network) != 0) {
        return 1;
    }
    if (dynamic_random_fault_run(&network) != 0) {
        return 1;
    }
    if (dynamic_cleanup_and_recovery(&network) != 0) {
        return 1;
    }
    printf("DYNAMIC_STRESS seed=0x%08" PRIX32 " rounds=%" PRIu32
           " event_hwm=%zu enqueued=%" PRIu32 " delivered=%" PRIu32
           " dropped=%" PRIu32 " duplicated=%" PRIu32
           " backpressure=%" PRIu32 " business=%" PRIu32 "\n",
           seed, rounds, network.event_high_water, network.frames_enqueued,
           network.frames_delivered, network.frames_dropped,
           network.duplicate_frames_enqueued, network.queue_backpressure,
           network.business_delivered);
    return 0;
}
