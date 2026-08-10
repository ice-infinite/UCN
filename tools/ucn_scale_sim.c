#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "ucn/ucn_node_storage.h"

#define SCALE_DEFAULT_NODES ((size_t)32U)
#define SCALE_MAX_NODES ((size_t)4096U)
#define SCALE_DEFAULT_TICKS UINT32_C(200)
#define SCALE_DEFAULT_WARMUP_TICKS UINT32_C(150)
#define SCALE_DEFAULT_DRAIN_TICKS UINT32_C(300)
#define SCALE_DEFAULT_STEP_MS UINT32_C(10)
#define SCALE_DEFAULT_MESSAGES_PER_NODE ((uint16_t)1U)
#define SCALE_DEFAULT_PAYLOAD_BYTES ((uint16_t)16U)
#define SCALE_DEFAULT_EVENT_FACTOR ((size_t)64U)
#define SCALE_MAX_MESSAGES_PER_NODE ((uint16_t)16U)
#define SCALE_LATENCY_BUCKET_MS UINT32_C(10)
#define SCALE_LATENCY_BUCKETS ((size_t)2001U)
#define SCALE_ENDPOINT_Q1_BASE ((ucn_endpoint_t)0x40U)
#define SCALE_ENDPOINT_Q0 ((ucn_endpoint_t)0x60U)
#define SCALE_Q1_STREAMS ((uint8_t)4U)
#define SCALE_NETWORK_ID UINT32_C(0x5343414C)
#define SCALE_WIRE_MESSAGE_TYPE_OFFSET ((size_t)3U)
#define SCALE_WIRE_SOURCE_OFFSET ((size_t)12U)
#define SCALE_PAYLOAD_META_BYTES ((uint16_t)12U)

typedef enum scale_topology {
    SCALE_TOPOLOGY_TREE = 0,
    SCALE_TOPOLOGY_RING4 = 1
} scale_topology_t;

typedef enum scale_traffic {
    SCALE_TRAFFIC_LOCAL = 0,
    SCALE_TRAFFIC_TWO_HOP = 1,
    SCALE_TRAFFIC_PAIRS = 2,
    SCALE_TRAFFIC_INCAST = 3,
    SCALE_TRAFFIC_ALL_TO_ALL = 4,
    SCALE_TRAFFIC_MIXED = 5
} scale_traffic_t;

typedef struct scale_options {
    size_t node_count;
    uint32_t ticks;
    uint32_t warmup_ticks;
    uint32_t warmup_batch;
    uint32_t drain_ticks;
    uint32_t step_ms;
    uint16_t messages_per_node;
    uint16_t payload_bytes;
    uint16_t loss_per_mille;
    uint16_t duplicate_per_mille;
    uint16_t max_delay_ms;
    uint32_t flap_every_ticks;
    uint32_t flap_duration_ticks;
    uint32_t q0_every_ticks;
    uint32_t seed;
    size_t event_capacity;
    scale_topology_t topology;
    scale_traffic_t traffic;
    const char *report_prefix;
    bool quiet;
} scale_options_t;

typedef struct scale_network scale_network_t;

typedef struct scale_link_context {
    scale_network_t *network;
    size_t source_index;
    size_t target_index;
    size_t source_slot;
    size_t reverse_flat_index;
    bool configured;
    bool is_up;
    uint16_t route_cost;
    uint32_t down_until_tick;
    uint64_t tx_attempts;
    uint64_t tx_errors;
} scale_link_context_t;

typedef struct scale_event {
    uint32_t due_at_ms;
    uint64_t order;
    scale_link_context_t *source_context;
    size_t target_index;
    size_t ingress_flat_index;
    size_t length;
    uint8_t frame[UCN_MAX_FRAME_BYTES];
} scale_event_t;

typedef struct scale_rx_context {
    scale_network_t *network;
    size_t node_index;
} scale_rx_context_t;

typedef struct scale_node_metrics {
    uint64_t app_generated;
    uint64_t app_accepted;
    uint64_t q0_generated;
    uint64_t q0_accepted;
    uint64_t q0_delivered;
    uint64_t q1_generated;
    uint64_t q1_accepted;
    uint64_t q1_delivered;
    uint64_t app_no_space;
    uint64_t app_no_route;
    uint64_t app_link_down;
    uint64_t app_other_rejected;
    uint64_t origin_delivered;
    uint64_t origin_payload_delivered;
    uint64_t business_received;
    uint64_t duplicate_deliveries;
    uint64_t frames_tx;
    uint64_t frames_rx;
    uint64_t control_frames_tx;
    uint64_t business_frames_tx;
    uint64_t forwarded_frames_tx;
    uint64_t wire_bytes_tx;
    uint64_t wire_bytes_rx;
    uint64_t origin_wire_bytes;
    uint64_t origin_business_wire_bytes;
    uint64_t host_work_ns;
    uint64_t latency_sum_ms;
    uint32_t latency_max_ms;
    size_t routes_current;
    size_t routes_hwm;
    size_t discoveries_hwm;
    size_t candidates_hwm;
    size_t q0_hwm;
    size_t q1_hwm;
    size_t pending_q1_hwm;
    size_t paths_hwm;
    size_t flows_hwm;
    ucn_node_stats_t stats_base;
} scale_node_metrics_t;

struct scale_network {
    scale_options_t options;
    ucn_node_t *nodes;
    ucn_link_t *links;
    scale_link_context_t *link_contexts;
    scale_rx_context_t *rx_contexts;
    scale_node_metrics_t *metrics;
    uint32_t *latency_histograms;
    uint8_t *degrees;
    size_t *fixed_destinations;
    scale_event_t *events;
    uint8_t *delivery_counts;
    size_t delivery_capacity;
    size_t event_count;
    size_t event_high_water;
    size_t event_capacity;
    uint32_t now_ms;
    uint32_t current_tick;
    uint32_t random_state;
    uint32_t next_message_id;
    uint64_t next_event_order;
    uint64_t frames_enqueued;
    uint64_t frames_delivered;
    uint64_t frames_fault_dropped;
    uint64_t duplicate_frames_enqueued;
    uint64_t simulator_backpressure;
    uint64_t receive_rejections;
    uint64_t link_flaps;
    uint64_t allocated_bytes;
    uint64_t wall_started_ns;
    uint64_t wall_elapsed_ns;
    bool measuring;
    bool faults_enabled;
    bool fatal_error;
    bool duplicate_business_delivery;
    bool route_loop_detected;
};

static uint64_t scale_clock_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER counter;
    static LONGLONG frequency_value = 0;

    if (frequency_value == 0) {
        LARGE_INTEGER frequency;

        (void)QueryPerformanceFrequency(&frequency);
        frequency_value = frequency.QuadPart;
    }
    (void)QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart / frequency_value) *
               UINT64_C(1000000000) +
           (uint64_t)((counter.QuadPart % frequency_value) *
                      INT64_C(1000000000) / frequency_value);
#else
    struct timeval value;

    (void)gettimeofday(&value, NULL);
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_usec * UINT64_C(1000);
#endif
}

static uint32_t scale_random(scale_network_t *network)
{
    uint32_t value = network->random_state;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    network->random_state = value;
    return value;
}

static uint32_t scale_read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) |
           (uint32_t)input[3];
}

static void scale_write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static const char *scale_topology_name(scale_topology_t topology)
{
    return topology == SCALE_TOPOLOGY_TREE ? "tree" : "ring4";
}

static const char *scale_traffic_name(scale_traffic_t traffic)
{
    switch (traffic) {
    case SCALE_TRAFFIC_LOCAL:
        return "local";
    case SCALE_TRAFFIC_TWO_HOP:
        return "two-hop";
    case SCALE_TRAFFIC_PAIRS:
        return "pairs";
    case SCALE_TRAFFIC_INCAST:
        return "incast";
    case SCALE_TRAFFIC_ALL_TO_ALL:
        return "all-to-all";
    case SCALE_TRAFFIC_MIXED:
        return "mixed";
    default:
        return "unknown";
    }
}

static bool scale_event_precedes(const scale_event_t *first,
                                 const scale_event_t *second)
{
    return first->due_at_ms < second->due_at_ms ||
           (first->due_at_ms == second->due_at_ms &&
            first->order < second->order);
}

static void scale_event_swap(scale_event_t *first, scale_event_t *second)
{
    scale_event_t temporary = *first;

    *first = *second;
    *second = temporary;
}

static bool scale_event_push(scale_network_t *network,
                             scale_link_context_t *context,
                             const uint8_t *frame,
                             size_t length,
                             bool duplicate)
{
    scale_event_t *event;
    size_t index;
    uint32_t delay = 0U;

    if (network->event_count >= network->event_capacity) {
        network->simulator_backpressure++;
        return false;
    }
    if (network->faults_enabled && network->options.max_delay_ms > 0U) {
        delay = scale_random(network) %
                ((uint32_t)network->options.max_delay_ms + 1U);
    }
    index = network->event_count++;
    event = &network->events[index];
    event->due_at_ms = network->now_ms + delay;
    event->order = network->next_event_order++;
    event->source_context = context;
    event->target_index = context->target_index;
    event->ingress_flat_index = context->reverse_flat_index;
    event->length = length;
    (void)memcpy(event->frame, frame, length);
    while (index > 0U) {
        size_t parent = (index - 1U) / 2U;

        if (!scale_event_precedes(&network->events[index],
                                  &network->events[parent])) {
            break;
        }
        scale_event_swap(&network->events[index], &network->events[parent]);
        index = parent;
    }
    if (network->event_count > network->event_high_water) {
        network->event_high_water = network->event_count;
    }
    network->frames_enqueued++;
    if (duplicate) {
        network->duplicate_frames_enqueued++;
    }
    return true;
}

static scale_event_t scale_event_pop(scale_network_t *network)
{
    scale_event_t result = network->events[0];
    size_t index = 0U;

    network->event_count--;
    if (network->event_count == 0U) {
        return result;
    }
    network->events[0] = network->events[network->event_count];
    while (true) {
        size_t left = index * 2U + 1U;
        size_t right = left + 1U;
        size_t best = index;

        if (left < network->event_count &&
            scale_event_precedes(&network->events[left],
                                 &network->events[best])) {
            best = left;
        }
        if (right < network->event_count &&
            scale_event_precedes(&network->events[right],
                                 &network->events[best])) {
            best = right;
        }
        if (best == index) {
            break;
        }
        scale_event_swap(&network->events[index], &network->events[best]);
        index = best;
    }
    return result;
}

static bool scale_result_expected(ucn_result_t result)
{
    return result == UCN_OK || result == UCN_ERR_NOT_FOUND ||
           result == UCN_ERR_NO_SPACE || result == UCN_ERR_LINK_DOWN ||
           result == UCN_ERR_TTL || result == UCN_ERR_ACCESS ||
           result == UCN_ERR_REPLAY;
}

static void scale_account_wire_tx(scale_network_t *network,
                                  scale_link_context_t *context,
                                  const uint8_t *frame,
                                  size_t length)
{
    scale_node_metrics_t *local;
    uint8_t message_type;
    ucn_node_id_t source;

    if (!network->measuring || length <= SCALE_WIRE_SOURCE_OFFSET + 3U) {
        return;
    }
    local = &network->metrics[context->source_index];
    message_type = frame[SCALE_WIRE_MESSAGE_TYPE_OFFSET];
    source = scale_read_u32_be(&frame[SCALE_WIRE_SOURCE_OFFSET]);
    local->frames_tx++;
    local->wire_bytes_tx += length;
    if (message_type < 0x40U) {
        local->control_frames_tx++;
    } else {
        local->business_frames_tx++;
    }
    if (source != (ucn_node_id_t)(context->source_index + 1U)) {
        local->forwarded_frames_tx++;
    }
    if (source > 0U && source <= network->options.node_count) {
        scale_node_metrics_t *origin = &network->metrics[source - 1U];

        origin->origin_wire_bytes += length;
        if (message_type >= 0x40U) {
            origin->origin_business_wire_bytes += length;
        }
    }
}

static ucn_result_t scale_link_send(ucn_link_t *link,
                                    const uint8_t *frame,
                                    size_t length)
{
    scale_link_context_t *context = (scale_link_context_t *)link->context;
    scale_network_t *network = context->network;

    context->tx_attempts++;
    if (!context->is_up) {
        context->tx_errors++;
        return UCN_ERR_LINK_DOWN;
    }
    if (length > UCN_MAX_FRAME_BYTES || length > link->mtu) {
        context->tx_errors++;
        return UCN_ERR_TOO_LARGE;
    }
    scale_account_wire_tx(network, context, frame, length);
    if (network->faults_enabled && network->options.loss_per_mille > 0U &&
        scale_random(network) % UINT32_C(1000) <
            network->options.loss_per_mille) {
        network->frames_fault_dropped++;
        return UCN_OK;
    }
    if (!scale_event_push(network, context, frame, length, false)) {
        context->tx_errors++;
        return UCN_ERR_NO_SPACE;
    }
    if (network->faults_enabled &&
        network->options.duplicate_per_mille > 0U &&
        scale_random(network) % UINT32_C(1000) <
            network->options.duplicate_per_mille) {
        (void)scale_event_push(network, context, frame, length, true);
    }
    return UCN_OK;
}

static ucn_result_t scale_link_status(const ucn_link_t *link,
                                      ucn_link_status_t *status)
{
    const scale_link_context_t *context =
        (const scale_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = context->tx_errors > UINT32_MAX ?
                            UINT32_MAX : (uint32_t)context->tx_errors;
    status->rx_errors = 0U;
    return UCN_OK;
}

static ucn_result_t scale_link_metrics(const ucn_link_t *link,
                                       ucn_link_metrics_t *metrics)
{
    const scale_link_context_t *context =
        (const scale_link_context_t *)link->context;
    const scale_network_t *network = context->network;

    (void)memset(metrics, 0, sizeof(*metrics));
    metrics->route_cost_valid = true;
    metrics->route_cost = context->route_cost;
    metrics->tx_failure_rate_valid = true;
    metrics->tx_failure_per_mille = network->faults_enabled ?
        network->options.loss_per_mille : 0U;
    metrics->queue_pressure_valid = true;
    metrics->queue_pressure_per_mille =
        network->event_capacity == 0U ? 0U :
        (uint16_t)((network->event_count * UINT32_C(1000)) /
                   network->event_capacity);
    return UCN_OK;
}

static const ucn_link_ops_t SCALE_LINK_OPS = {
    NULL,
    scale_link_send,
    NULL,
    scale_link_status,
    NULL,
    scale_link_metrics
};

static size_t scale_latency_index(size_t node_index, size_t bucket)
{
    return node_index * SCALE_LATENCY_BUCKETS + bucket;
}

static void scale_receive(void *context, const ucn_frame_t *frame)
{
    scale_rx_context_t *rx = (scale_rx_context_t *)context;
    scale_network_t *network = rx->network;
    uint32_t message_id;
    uint32_t source_id;
    uint32_t sent_at_ms;
    uint32_t latency_ms;
    size_t source_index;
    size_t bucket;

    if (!network->measuring || frame->message_type < 0x40U ||
        frame->payload_length < SCALE_PAYLOAD_META_BYTES) {
        return;
    }
    message_id = scale_read_u32_be(&frame->payload[0]);
    source_id = scale_read_u32_be(&frame->payload[4]);
    sent_at_ms = scale_read_u32_be(&frame->payload[8]);
    if (message_id == 0U || message_id >= network->delivery_capacity ||
        source_id == 0U || source_id > network->options.node_count) {
        network->fatal_error = true;
        return;
    }
    source_index = source_id - 1U;
    network->metrics[rx->node_index].business_received++;
    if (network->delivery_counts[message_id] != 0U) {
        network->delivery_counts[message_id]++;
        network->metrics[source_index].duplicate_deliveries++;
        network->duplicate_business_delivery = true;
        return;
    }
    network->delivery_counts[message_id] = 1U;
    latency_ms = network->now_ms - sent_at_ms;
    bucket = latency_ms / SCALE_LATENCY_BUCKET_MS;
    if (bucket >= SCALE_LATENCY_BUCKETS) {
        bucket = SCALE_LATENCY_BUCKETS - 1U;
    }
    network->latency_histograms[scale_latency_index(source_index, bucket)]++;
    network->metrics[source_index].origin_delivered++;
    if (frame->message_type == SCALE_ENDPOINT_Q0) {
        network->metrics[source_index].q0_delivered++;
    } else {
        network->metrics[source_index].q1_delivered++;
    }
    network->metrics[source_index].origin_payload_delivered +=
        frame->payload_length;
    network->metrics[source_index].latency_sum_ms += latency_ms;
    if (latency_ms > network->metrics[source_index].latency_max_ms) {
        network->metrics[source_index].latency_max_ms = latency_ms;
    }
}

static size_t scale_flat_link(const scale_network_t *network,
                              size_t node_index,
                              size_t slot)
{
    (void)network;
    return node_index * UCN_MAX_LINKS + slot;
}

static bool scale_nodes_are_adjacent(const scale_network_t *network,
                                     size_t first,
                                     size_t second)
{
    size_t slot;

    for (slot = 0U; slot < network->degrees[first]; ++slot) {
        size_t flat = scale_flat_link(network, first, slot);

        if (network->link_contexts[flat].target_index == second) {
            return true;
        }
    }
    return false;
}

static bool scale_add_edge(scale_network_t *network,
                           size_t first,
                           size_t second,
                           uint16_t cost)
{
    size_t first_slot;
    size_t second_slot;
    size_t first_flat;
    size_t second_flat;
    scale_link_context_t *first_context;
    scale_link_context_t *second_context;

    if (first == second || scale_nodes_are_adjacent(network, first, second)) {
        return true;
    }
    if (network->degrees[first] >= UCN_MAX_LINKS ||
        network->degrees[second] >= UCN_MAX_LINKS) {
        return false;
    }
    first_slot = network->degrees[first]++;
    second_slot = network->degrees[second]++;
    first_flat = scale_flat_link(network, first, first_slot);
    second_flat = scale_flat_link(network, second, second_slot);
    first_context = &network->link_contexts[first_flat];
    second_context = &network->link_contexts[second_flat];

    first_context->network = network;
    first_context->source_index = first;
    first_context->target_index = second;
    first_context->source_slot = first_slot;
    first_context->reverse_flat_index = second_flat;
    first_context->configured = true;
    first_context->is_up = true;
    first_context->route_cost = cost;

    second_context->network = network;
    second_context->source_index = second;
    second_context->target_index = first;
    second_context->source_slot = second_slot;
    second_context->reverse_flat_index = first_flat;
    second_context->configured = true;
    second_context->is_up = true;
    second_context->route_cost = cost;
    return true;
}

static bool scale_build_topology(scale_network_t *network)
{
    size_t index;

    if (network->options.topology == SCALE_TOPOLOGY_TREE) {
        for (index = 1U; index < network->options.node_count; ++index) {
            size_t parent = (index - 1U) / 3U;

            if (!scale_add_edge(network, parent, index,
                                (uint16_t)(10U + index % 7U))) {
                return false;
            }
        }
        return true;
    }
    for (index = 0U; index < network->options.node_count; ++index) {
        size_t next = (index + 1U) % network->options.node_count;

        if (!scale_add_edge(network, index, next, 10U)) {
            return false;
        }
    }
    if (network->options.node_count >= 6U) {
        for (index = 0U; index < network->options.node_count; ++index) {
            size_t chord = (index + 4U) % network->options.node_count;

            if (!scale_add_edge(network, index, chord, 20U)) {
                return false;
            }
        }
    }
    return true;
}

static void scale_choose_destinations(scale_network_t *network)
{
    size_t source;

    for (source = 0U; source < network->options.node_count; ++source) {
        size_t destination = source;

        if (network->options.traffic == SCALE_TRAFFIC_LOCAL) {
            destination = network->link_contexts[
                scale_flat_link(network, source, 0U)].target_index;
        } else if (network->options.traffic == SCALE_TRAFFIC_TWO_HOP ||
                   network->options.traffic == SCALE_TRAFFIC_MIXED) {
            size_t first_slot;

            for (first_slot = 0U; first_slot < network->degrees[source] &&
                                  destination == source; ++first_slot) {
                size_t first = network->link_contexts[
                    scale_flat_link(network, source, first_slot)].target_index;
                size_t second_slot;

                for (second_slot = 0U;
                     second_slot < network->degrees[first]; ++second_slot) {
                    size_t second = network->link_contexts[
                        scale_flat_link(network, first, second_slot)].target_index;

                    if (second != source &&
                        !scale_nodes_are_adjacent(network, source, second)) {
                        destination = second;
                        break;
                    }
                }
            }
            if (destination == source) {
                destination = network->link_contexts[
                    scale_flat_link(network, source, 0U)].target_index;
            }
        } else if (network->options.traffic == SCALE_TRAFFIC_PAIRS) {
            destination = (source + network->options.node_count / 2U) %
                          network->options.node_count;
            if (destination == source) {
                destination = (source + 1U) % network->options.node_count;
            }
        } else if (network->options.traffic == SCALE_TRAFFIC_INCAST) {
            destination = source == 0U ? 1U : 0U;
        }
        network->fixed_destinations[source] = destination;
    }
}

static bool scale_pump_due_events(scale_network_t *network)
{
    while (network->event_count > 0U &&
           network->events[0].due_at_ms <= network->now_ms) {
        scale_event_t event = scale_event_pop(network);

        if (event.source_context->is_up) {
            uint64_t started = scale_clock_ns();
            ucn_result_t result = ucn_node_receive(
                &network->nodes[event.target_index],
                &network->links[event.ingress_flat_index],
                event.frame, event.length);
            uint64_t elapsed = scale_clock_ns() - started;

            if (network->measuring) {
                scale_node_metrics_t *metrics =
                    &network->metrics[event.target_index];

                metrics->host_work_ns += elapsed;
                metrics->frames_rx++;
                metrics->wire_bytes_rx += event.length;
            }
            if (!scale_result_expected(result)) {
                network->fatal_error = true;
                return false;
            }
            if (result != UCN_OK) {
                network->receive_rejections++;
            }
            network->frames_delivered++;
        } else {
            network->frames_fault_dropped++;
        }
    }
    return true;
}

static void scale_sample_node(scale_network_t *network, size_t node_index)
{
    const ucn_node_t *node = &network->nodes[node_index];
    scale_node_metrics_t *metrics = &network->metrics[node_index];
    size_t index;
    size_t count;

    count = 0U;
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        count += node->routes[index].valid ? 1U : 0U;
    }
    metrics->routes_current = count;
    if (count > metrics->routes_hwm) {
        metrics->routes_hwm = count;
    }
#if UCN_FEATURE_DYNAMIC_MESH
    count = 0U;
    for (index = 0U; index < UCN_MAX_ROUTE_DISCOVERIES; ++index) {
        count += node->discoveries[index].active ? 1U : 0U;
    }
    if (count > metrics->discoveries_hwm) {
        metrics->discoveries_hwm = count;
    }
    count = 0U;
    for (index = 0U; index < UCN_PENDING_Q1_DEPTH; ++index) {
        count += node->pending_q1[index].occupied ? 1U : 0U;
    }
    if (count > metrics->pending_q1_hwm) {
        metrics->pending_q1_hwm = count;
    }
#endif
#if UCN_FEATURE_CANDIDATE_ROUTING
    count = 0U;
    for (index = 0U; index < UCN_MAX_CANDIDATE_ROUTES; ++index) {
        count += node->candidates[index].valid ? 1U : 0U;
    }
    if (count > metrics->candidates_hwm) {
        metrics->candidates_hwm = count;
    }
#endif
    count = 0U;
    for (index = 0U; index < UCN_TX_Q0_DEPTH; ++index) {
        count += node->q0[index].occupied ? 1U : 0U;
    }
    if (count > metrics->q0_hwm) {
        metrics->q0_hwm = count;
    }
    count = 0U;
    for (index = 0U; index < UCN_TX_Q1_DEPTH; ++index) {
        count += node->q1[index].occupied ? 1U : 0U;
    }
    if (count > metrics->q1_hwm) {
        metrics->q1_hwm = count;
    }
#if UCN_FEATURE_PATH
    count = 0U;
    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        count += node->path_state.entries[index].occupied ? 1U : 0U;
    }
    if (count > metrics->paths_hwm) {
        metrics->paths_hwm = count;
    }
#endif
#if UCN_FEATURE_POLICY
    count = 0U;
    for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
        count += node->policy_state.flows[index].occupied ? 1U : 0U;
    }
    if (count > metrics->flows_hwm) {
        metrics->flows_hwm = count;
    }
#endif
}

static bool scale_step_all(scale_network_t *network, uint32_t delta_ms)
{
    size_t index;

    network->now_ms += delta_ms;
    for (index = 0U; index < network->options.node_count; ++index) {
        uint64_t started = scale_clock_ns();
        ucn_result_t result =
            ucn_node_step(&network->nodes[index], network->now_ms);
        uint64_t elapsed = scale_clock_ns() - started;

        if (network->measuring) {
            network->metrics[index].host_work_ns += elapsed;
        }
        if (!scale_result_expected(result)) {
            network->fatal_error = true;
            return false;
        }
    }
    if (!scale_pump_due_events(network)) {
        return false;
    }
    if (network->measuring) {
        for (index = 0U; index < network->options.node_count; ++index) {
            scale_sample_node(network, index);
        }
    }
    return !network->fatal_error;
}

static void scale_restore_links(scale_network_t *network)
{
    size_t flat;
    size_t total = network->options.node_count * UCN_MAX_LINKS;

    for (flat = 0U; flat < total; ++flat) {
        scale_link_context_t *context = &network->link_contexts[flat];

        if (context->configured && !context->is_up &&
            context->down_until_tick != 0U &&
            network->current_tick >= context->down_until_tick) {
            scale_link_context_t *reverse =
                &network->link_contexts[context->reverse_flat_index];

            context->is_up = true;
            context->down_until_tick = 0U;
            reverse->is_up = true;
            reverse->down_until_tick = 0U;
        }
    }
}

static void scale_maybe_flap_link(scale_network_t *network)
{
    size_t total;
    size_t start;
    size_t offset;

    if (network->options.flap_every_ticks == 0U ||
        network->current_tick == 0U ||
        network->current_tick % network->options.flap_every_ticks != 0U) {
        return;
    }
    total = network->options.node_count * UCN_MAX_LINKS;
    start = scale_random(network) % total;
    for (offset = 0U; offset < total; ++offset) {
        scale_link_context_t *context =
            &network->link_contexts[(start + offset) % total];

        if (context->configured && context->is_up) {
            scale_link_context_t *reverse =
                &network->link_contexts[context->reverse_flat_index];
            uint32_t restore_tick = network->current_tick +
                                    network->options.flap_duration_ticks;

            context->is_up = false;
            context->down_until_tick = restore_tick;
            reverse->is_up = false;
            reverse->down_until_tick = restore_tick;
            network->link_flaps++;
            return;
        }
    }
}

static size_t scale_destination_for(const scale_network_t *network,
                                    size_t source,
                                    uint32_t tick)
{
    if (network->options.traffic != SCALE_TRAFFIC_ALL_TO_ALL) {
        return network->fixed_destinations[source];
    }
    return (source + 1U + (tick % (network->options.node_count - 1U))) %
           network->options.node_count;
}

static ucn_result_t scale_send_one(scale_network_t *network,
                                   size_t source,
                                   size_t destination,
                                   uint8_t stream,
                                   bool q0)
{
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
    uint32_t message_id = 0U;
    ucn_result_t result;
    scale_node_metrics_t *metrics = &network->metrics[source];

    (void)memset(payload, (int)(source & 0xFFU), sizeof(payload));
    if (network->measuring) {
        message_id = network->next_message_id++;
        if (message_id >= network->delivery_capacity) {
            network->fatal_error = true;
            return UCN_ERR_NO_SPACE;
        }
        metrics->app_generated++;
        if (q0) {
            metrics->q0_generated++;
        } else {
            metrics->q1_generated++;
        }
    }
    scale_write_u32_be(&payload[0], message_id);
    scale_write_u32_be(&payload[4], (uint32_t)(source + 1U));
    scale_write_u32_be(&payload[8], network->now_ms);
    if (q0) {
        ucn_send_request_t request;

        (void)memset(&request, 0, sizeof(request));
        request.destination = (ucn_node_id_t)(destination + 1U);
        request.message_type = SCALE_ENDPOINT_Q0;
        request.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
        request.delivery = UCN_DELIVERY_BEST_EFFORT;
        request.deadline_ms = network->now_ms + UINT32_C(1000);
        request.payload = payload;
        request.payload_length = network->options.payload_bytes;
        result = ucn_node_enqueue(&network->nodes[source], &request);
    } else {
        result = ucn_node_send_endpoint(
            &network->nodes[source], (ucn_node_id_t)(destination + 1U),
            (ucn_endpoint_t)(SCALE_ENDPOINT_Q1_BASE +
                             (stream % SCALE_Q1_STREAMS)),
            UCN_TRAFFIC_Q1_REALTIME, payload,
            network->options.payload_bytes);
    }
    if (!network->measuring) {
        return result;
    }
    if (result == UCN_OK) {
        metrics->app_accepted++;
        if (q0) {
            metrics->q0_accepted++;
        } else {
            metrics->q1_accepted++;
        }
    } else if (result == UCN_ERR_NO_SPACE) {
        metrics->app_no_space++;
    } else if (result == UCN_ERR_NOT_FOUND || result == UCN_ERR_TTL) {
        metrics->app_no_route++;
    } else if (result == UCN_ERR_LINK_DOWN) {
        metrics->app_link_down++;
    } else {
        metrics->app_other_rejected++;
    }
    return result;
}

static bool scale_generate_tick(scale_network_t *network, uint32_t tick)
{
    size_t source_offset;
    size_t source_count = network->options.node_count;
    size_t source_start = 0U;

    if (!network->measuring && network->options.warmup_batch > 0U) {
        source_count = network->options.warmup_batch;
        if (source_count > network->options.node_count) {
            source_count = network->options.node_count;
        }
        source_start = ((size_t)tick * source_count) %
                       network->options.node_count;
    }
    for (source_offset = 0U; source_offset < source_count; ++source_offset) {
        size_t source = (source_start + source_offset) %
                        network->options.node_count;
        size_t destination = scale_destination_for(network, source, tick);
        uint16_t message;

        for (message = 0U;
             message < network->options.messages_per_node; ++message) {
            ucn_result_t result = scale_send_one(
                network, source, destination,
                (uint8_t)((tick + message) % SCALE_Q1_STREAMS), false);

            if (!scale_result_expected(result)) {
                network->fatal_error = true;
                return false;
            }
        }
        if (network->options.traffic == SCALE_TRAFFIC_MIXED &&
            network->options.q0_every_ticks > 0U &&
            tick % network->options.q0_every_ticks == 0U) {
            ucn_result_t result = scale_send_one(
                network, source, destination, 0U, true);

            if (!scale_result_expected(result)) {
                network->fatal_error = true;
                return false;
            }
        }
    }
    return true;
}

static void scale_reset_measurement(scale_network_t *network)
{
    size_t index;

    (void)memset(network->metrics, 0,
                 network->options.node_count * sizeof(*network->metrics));
    (void)memset(network->latency_histograms, 0,
                 network->options.node_count * SCALE_LATENCY_BUCKETS *
                     sizeof(*network->latency_histograms));
    (void)memset(network->delivery_counts, 0,
                 network->delivery_capacity * sizeof(*network->delivery_counts));
    network->frames_enqueued = 0U;
    network->frames_delivered = 0U;
    network->frames_fault_dropped = 0U;
    network->duplicate_frames_enqueued = 0U;
    network->simulator_backpressure = 0U;
    network->receive_rejections = 0U;
    network->event_high_water = network->event_count;
    network->link_flaps = 0U;
    network->next_message_id = 1U;
    network->duplicate_business_delivery = false;
    network->fatal_error = false;
    network->route_loop_detected = false;
    for (index = 0U; index < network->options.node_count; ++index) {
        network->metrics[index].stats_base =
            *ucn_node_get_stats(&network->nodes[index]);
    }
}

static bool scale_broadcast_hello(scale_network_t *network)
{
    size_t node;

    for (node = 0U; node < network->options.node_count; ++node) {
        size_t slot;

        for (slot = 0U; slot < network->degrees[node]; ++slot) {
            ucn_result_t result = ucn_node_broadcast_hello(
                &network->nodes[node],
                &network->links[scale_flat_link(network, node, slot)],
                network->now_ms);

            if (result != UCN_OK) {
                return false;
            }
        }
    }
    return scale_pump_due_events(network);
}

static bool scale_network_init(scale_network_t *network,
                               const scale_options_t *options)
{
    size_t node;
    size_t flat_count;
    size_t message_capacity;

    (void)memset(network, 0, sizeof(*network));
    network->options = *options;
    network->random_state = options->seed == 0U ? UINT32_C(1) : options->seed;
    network->now_ms = UINT32_C(100);
    network->event_capacity = options->event_capacity == 0U ?
        options->node_count * SCALE_DEFAULT_EVENT_FACTOR :
        options->event_capacity;
    flat_count = options->node_count * UCN_MAX_LINKS;
    message_capacity = options->node_count * (size_t)options->ticks *
                       ((size_t)options->messages_per_node + 1U) + 1U;

    network->nodes = (ucn_node_t *)calloc(options->node_count,
                                           sizeof(*network->nodes));
    network->links = (ucn_link_t *)calloc(flat_count, sizeof(*network->links));
    network->link_contexts = (scale_link_context_t *)calloc(
        flat_count, sizeof(*network->link_contexts));
    network->rx_contexts = (scale_rx_context_t *)calloc(
        options->node_count, sizeof(*network->rx_contexts));
    network->metrics = (scale_node_metrics_t *)calloc(
        options->node_count, sizeof(*network->metrics));
    network->latency_histograms = (uint32_t *)calloc(
        options->node_count * SCALE_LATENCY_BUCKETS,
        sizeof(*network->latency_histograms));
    network->degrees = (uint8_t *)calloc(options->node_count,
                                         sizeof(*network->degrees));
    network->fixed_destinations = (size_t *)calloc(
        options->node_count, sizeof(*network->fixed_destinations));
    network->events = (scale_event_t *)calloc(network->event_capacity,
                                               sizeof(*network->events));
    network->delivery_counts = (uint8_t *)calloc(
        message_capacity, sizeof(*network->delivery_counts));
    network->delivery_capacity = message_capacity;
    if (network->nodes == NULL || network->links == NULL ||
        network->link_contexts == NULL || network->rx_contexts == NULL ||
        network->metrics == NULL || network->latency_histograms == NULL ||
        network->degrees == NULL || network->fixed_destinations == NULL ||
        network->events == NULL || network->delivery_counts == NULL) {
        return false;
    }
    network->allocated_bytes =
        options->node_count * sizeof(*network->nodes) +
        flat_count * sizeof(*network->links) +
        flat_count * sizeof(*network->link_contexts) +
        options->node_count * sizeof(*network->rx_contexts) +
        options->node_count * sizeof(*network->metrics) +
        options->node_count * SCALE_LATENCY_BUCKETS *
            sizeof(*network->latency_histograms) +
        options->node_count * sizeof(*network->degrees) +
        options->node_count * sizeof(*network->fixed_destinations) +
        network->event_capacity * sizeof(*network->events) +
        message_capacity * sizeof(*network->delivery_counts);

    if (!scale_build_topology(network)) {
        return false;
    }
    scale_choose_destinations(network);
    for (node = 0U; node < options->node_count; ++node) {
        ucn_config_t config;
        size_t slot;

        config.network_id = SCALE_NETWORK_ID;
        config.node_id = (ucn_node_id_t)(node + 1U);
        config.default_hop_limit = UCN_MAX_HOPS;
        if (ucn_node_init(&network->nodes[node], &config) != UCN_OK ||
            ucn_node_set_join_policy(&network->nodes[node], UCN_JOIN_OPEN,
                                     NULL, NULL) != UCN_OK) {
            return false;
        }
        network->rx_contexts[node].network = network;
        network->rx_contexts[node].node_index = node;
        ucn_node_set_rx_handler(&network->nodes[node], scale_receive,
                                &network->rx_contexts[node]);
        for (slot = 0U; slot < network->degrees[node]; ++slot) {
            size_t flat = scale_flat_link(network, node, slot);
            ucn_link_t *link = &network->links[flat];

            link->ops = &SCALE_LINK_OPS;
            link->context = &network->link_contexts[flat];
            link->link_id = (uint8_t)(slot + 1U);
            link->mtu = UCN_MAX_FRAME_BYTES;
            link->peer_node_id = 0U;
        }
    }
    if (!scale_broadcast_hello(network)) {
        return false;
    }
    for (node = 0U; node < options->node_count; ++node) {
        if (network->nodes[node].link_count != network->degrees[node] ||
            ucn_node_neighbor_count(&network->nodes[node],
                                    UCN_NEIGHBOR_ADMITTED) !=
                network->degrees[node]) {
            return false;
        }
    }
    network->faults_enabled = true;
    return true;
}

static void scale_network_free(scale_network_t *network)
{
    free(network->delivery_counts);
    free(network->events);
    free(network->fixed_destinations);
    free(network->degrees);
    free(network->latency_histograms);
    free(network->metrics);
    free(network->rx_contexts);
    free(network->link_contexts);
    free(network->links);
    free(network->nodes);
    (void)memset(network, 0, sizeof(*network));
}

static bool scale_find_current_route_link(const scale_network_t *network,
                                          size_t node_index,
                                          ucn_node_id_t destination,
                                          uint16_t epoch,
                                          ucn_link_t **output)
{
    const ucn_node_t *node = &network->nodes[node_index];
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        const ucn_route_entry_t *route = &node->routes[index];

        if (route->valid && route->destination == destination &&
            route->route_epoch == epoch) {
            *output = route->egress_link;
            return true;
        }
    }
    return false;
}

static bool scale_routes_have_no_loop(scale_network_t *network)
{
    uint8_t *visited = (uint8_t *)calloc(network->options.node_count,
                                         sizeof(*visited));
    size_t source;

    if (visited == NULL) {
        return false;
    }
    for (source = 0U; source < network->options.node_count; ++source) {
        size_t route_index;

        for (route_index = 0U; route_index < UCN_MAX_ROUTES; ++route_index) {
            const ucn_route_entry_t *origin =
                &network->nodes[source].routes[route_index];
            size_t current = source;
            size_t hop;

            if (!origin->valid) {
                continue;
            }
            (void)memset(visited, 0,
                         network->options.node_count * sizeof(*visited));
            for (hop = 0U; hop <= (size_t)UCN_MAX_HOPS; ++hop) {
                ucn_link_t *egress = NULL;
                size_t next;

                if (visited[current] != 0U) {
                    free(visited);
                    network->route_loop_detected = true;
                    return false;
                }
                visited[current] = 1U;
                if ((ucn_node_id_t)(current + 1U) == origin->destination ||
                    scale_nodes_are_adjacent(network, current,
                                             origin->destination - 1U)) {
                    break;
                }
                if (!scale_find_current_route_link(
                        network, current, origin->destination,
                        origin->route_epoch, &egress) || egress == NULL ||
                    egress->peer_node_id == 0U ||
                    egress->peer_node_id > network->options.node_count) {
                    break;
                }
                next = egress->peer_node_id - 1U;
                current = next;
            }
        }
    }
    free(visited);
    return true;
}

static uint32_t scale_percentile(const scale_network_t *network,
                                 size_t node_index,
                                 uint64_t samples,
                                 uint32_t percentile)
{
    uint64_t target;
    uint64_t cumulative = 0U;
    size_t bucket;

    if (samples == 0U) {
        return 0U;
    }
    target = (samples * percentile + 99U) / 100U;
    for (bucket = 0U; bucket < SCALE_LATENCY_BUCKETS; ++bucket) {
        cumulative += network->latency_histograms[
            scale_latency_index(node_index, bucket)];
        if (cumulative >= target) {
            return (uint32_t)bucket * SCALE_LATENCY_BUCKET_MS;
        }
    }
    return (uint32_t)(SCALE_LATENCY_BUCKETS - 1U) *
           SCALE_LATENCY_BUCKET_MS;
}

static uint64_t scale_stats_delta(uint32_t current, uint32_t base)
{
    return (uint32_t)(current - base);
}

static bool scale_write_node_csv(const scale_network_t *network,
                                 const char *path)
{
    FILE *file = fopen(path, "w");
    size_t node;

    if (file == NULL) {
        return false;
    }
    (void)fprintf(file,
        "node_id,degree,admitted,routes_current,routes_hwm,discoveries_hwm,"
        "candidates_hwm,q0_hwm,q1_hwm,pending_q1_hwm,paths_hwm,flows_hwm,"
        "app_generated,app_accepted,origin_delivered,duplicate_deliveries,"
        "q0_generated,q0_accepted,q0_delivered,q1_generated,q1_accepted,"
        "q1_delivered,"
        "delivery_pct,"
        "payload_delivered_bytes,origin_wire_bytes,wire_efficiency_pct,"
        "latency_p50_ms,latency_p95_ms,latency_p99_ms,latency_max_ms,"
        "frames_tx,frames_rx,forwarded_frames_tx,control_frames_tx,"
        "business_frames_tx,wire_bytes_tx,wire_bytes_rx,no_space,no_route,"
        "link_down,other_rejected,core_tx_sent,core_rx_delivered,"
        "route_requests_sent,route_errors_sent,control_budget_dropped,"
        "host_work_us,node_storage_bytes\n");
    for (node = 0U; node < network->options.node_count; ++node) {
        const scale_node_metrics_t *metrics = &network->metrics[node];
        const ucn_node_stats_t *stats = ucn_node_get_stats(&network->nodes[node]);
        double delivery_pct = metrics->app_accepted == 0U ? 0.0 :
            (double)metrics->origin_delivered * 100.0 /
            (double)metrics->app_accepted;
        double wire_efficiency = metrics->origin_wire_bytes == 0U ? 0.0 :
            (double)metrics->origin_payload_delivered * 100.0 /
            (double)metrics->origin_wire_bytes;

        (void)fprintf(file,
            "%zu,%u,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,"
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRIu64 ",%" PRIu64 ",%.6f,"
            "%" PRIu64 ",%" PRIu64 ",%.6f,%" PRIu32 ",%" PRIu32
            ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%zu\n",
            node + 1U, (unsigned)network->degrees[node],
            ucn_node_neighbor_count(&network->nodes[node],
                                    UCN_NEIGHBOR_ADMITTED),
            metrics->routes_current, metrics->routes_hwm,
            metrics->discoveries_hwm, metrics->candidates_hwm,
            metrics->q0_hwm, metrics->q1_hwm, metrics->pending_q1_hwm,
            metrics->paths_hwm, metrics->flows_hwm,
            metrics->app_generated, metrics->app_accepted,
            metrics->origin_delivered, metrics->duplicate_deliveries,
            metrics->q0_generated, metrics->q0_accepted,
            metrics->q0_delivered, metrics->q1_generated,
            metrics->q1_accepted, metrics->q1_delivered,
            delivery_pct,
            metrics->origin_payload_delivered, metrics->origin_wire_bytes,
            wire_efficiency,
            scale_percentile(network, node, metrics->origin_delivered, 50U),
            scale_percentile(network, node, metrics->origin_delivered, 95U),
            scale_percentile(network, node, metrics->origin_delivered, 99U),
            metrics->latency_max_ms, metrics->frames_tx, metrics->frames_rx,
            metrics->forwarded_frames_tx, metrics->control_frames_tx,
            metrics->business_frames_tx, metrics->wire_bytes_tx,
            metrics->wire_bytes_rx, metrics->app_no_space,
            metrics->app_no_route, metrics->app_link_down,
            metrics->app_other_rejected,
            scale_stats_delta(stats->tx_sent,
                              metrics->stats_base.tx_sent),
            scale_stats_delta(stats->rx_delivered,
                              metrics->stats_base.rx_delivered),
#if UCN_FEATURE_DYNAMIC_MESH
            scale_stats_delta(stats->route_requests_sent,
                              metrics->stats_base.route_requests_sent),
            scale_stats_delta(stats->route_errors_sent,
                              metrics->stats_base.route_errors_sent),
            scale_stats_delta(stats->control_budget_dropped,
                              metrics->stats_base.control_budget_dropped),
#else
            UINT64_C(0), UINT64_C(0), UINT64_C(0),
#endif
            metrics->host_work_ns / UINT64_C(1000), sizeof(ucn_node_t));
    }
    return fclose(file) == 0;
}

typedef struct scale_totals {
    uint64_t generated;
    uint64_t accepted;
    uint64_t delivered;
    uint64_t q0_generated;
    uint64_t q0_accepted;
    uint64_t q0_delivered;
    uint64_t q1_generated;
    uint64_t q1_accepted;
    uint64_t q1_delivered;
    uint64_t payload_bytes;
    uint64_t wire_bytes;
    uint64_t business_wire_bytes;
    uint64_t control_frames;
    uint64_t forwarded_frames;
    uint64_t no_space;
    uint64_t no_route;
    uint64_t link_down;
    uint64_t host_work_ns;
    uint64_t sum_squared_deliveries;
    uint64_t global_latency_samples;
} scale_totals_t;

static scale_totals_t scale_collect_totals(const scale_network_t *network)
{
    scale_totals_t totals;
    size_t node;

    (void)memset(&totals, 0, sizeof(totals));
    for (node = 0U; node < network->options.node_count; ++node) {
        const scale_node_metrics_t *metrics = &network->metrics[node];

        totals.generated += metrics->app_generated;
        totals.accepted += metrics->app_accepted;
        totals.delivered += metrics->origin_delivered;
        totals.q0_generated += metrics->q0_generated;
        totals.q0_accepted += metrics->q0_accepted;
        totals.q0_delivered += metrics->q0_delivered;
        totals.q1_generated += metrics->q1_generated;
        totals.q1_accepted += metrics->q1_accepted;
        totals.q1_delivered += metrics->q1_delivered;
        totals.payload_bytes += metrics->origin_payload_delivered;
        totals.wire_bytes += metrics->wire_bytes_tx;
        totals.business_wire_bytes += metrics->origin_business_wire_bytes;
        totals.control_frames += metrics->control_frames_tx;
        totals.forwarded_frames += metrics->forwarded_frames_tx;
        totals.no_space += metrics->app_no_space;
        totals.no_route += metrics->app_no_route;
        totals.link_down += metrics->app_link_down;
        totals.host_work_ns += metrics->host_work_ns;
        totals.sum_squared_deliveries +=
            metrics->origin_delivered * metrics->origin_delivered;
        totals.global_latency_samples += metrics->origin_delivered;
    }
    return totals;
}

static uint32_t scale_global_percentile(const scale_network_t *network,
                                        uint64_t samples,
                                        uint32_t percentile)
{
    uint64_t target;
    uint64_t cumulative = 0U;
    size_t bucket;

    if (samples == 0U) {
        return 0U;
    }
    target = (samples * percentile + 99U) / 100U;
    for (bucket = 0U; bucket < SCALE_LATENCY_BUCKETS; ++bucket) {
        size_t node;

        for (node = 0U; node < network->options.node_count; ++node) {
            cumulative += network->latency_histograms[
                scale_latency_index(node, bucket)];
        }
        if (cumulative >= target) {
            return (uint32_t)bucket * SCALE_LATENCY_BUCKET_MS;
        }
    }
    return (uint32_t)(SCALE_LATENCY_BUCKETS - 1U) *
           SCALE_LATENCY_BUCKET_MS;
}

static bool scale_write_summary_csv(const scale_network_t *network,
                                    const char *path,
                                    bool passed)
{
    FILE *file = fopen(path, "w");
    scale_totals_t totals = scale_collect_totals(network);
    double delivery_pct = totals.accepted == 0U ? 0.0 :
        (double)totals.delivered * 100.0 / (double)totals.accepted;
    double wire_efficiency = totals.wire_bytes == 0U ? 0.0 :
        (double)totals.payload_bytes * 100.0 / (double)totals.wire_bytes;
    double fairness = totals.sum_squared_deliveries == 0U ? 0.0 :
        ((double)totals.delivered * (double)totals.delivered) /
        ((double)network->options.node_count *
         (double)totals.sum_squared_deliveries);

    if (file == NULL) {
        return false;
    }
    (void)fprintf(file,
        "status,profile,topology,traffic,nodes,ticks,step_ms,"
        "messages_per_node,payload_bytes,warmup_ticks,warmup_batch,"
        "loss_per_mille,"
        "duplicate_per_mille,max_delay_ms,flap_every_ticks,"
        "generated,accepted,delivered,q0_generated,q0_accepted,q0_delivered,"
        "q1_generated,q1_accepted,q1_delivered,delivery_pct,"
        "payload_delivered_bytes,"
        "wire_bytes,wire_efficiency_pct,latency_p95_ms,latency_p99_ms,"
        "fairness,control_frames,forwarded_frames,no_space,no_route,"
        "link_down,event_hwm,event_capacity,simulator_backpressure,"
        "duplicate_business,route_loop,node_storage_bytes,"
        "total_node_storage_bytes,host_allocated_bytes,host_work_ms,"
        "wall_elapsed_ms\n");
    (void)fprintf(file,
        "%s,%s,%s,%s,%zu,%" PRIu32 ",%" PRIu32 ",%u,%u,%" PRIu32
        ",%" PRIu32 ",%u,%u,%u,"
        "%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%.6f,"
        "%" PRIu64 ",%" PRIu64 ",%.6f,%" PRIu32 ",%" PRIu32
        ",%.9f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%zu,%zu,%" PRIu64 ",%u,%u,%zu,%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
        passed ? "PASS" : "FAIL",
#if UCN_PROFILE == UCN_PROFILE_FULL
        "FULL",
#elif UCN_PROFILE == UCN_PROFILE_LITE
        "LITE",
#else
        "NANO",
#endif
        scale_topology_name(network->options.topology),
        scale_traffic_name(network->options.traffic),
        network->options.node_count, network->options.ticks,
        network->options.step_ms,
        (unsigned)network->options.messages_per_node,
        (unsigned)network->options.payload_bytes,
        network->options.warmup_ticks,
        network->options.warmup_batch,
        (unsigned)network->options.loss_per_mille,
        (unsigned)network->options.duplicate_per_mille,
        (unsigned)network->options.max_delay_ms,
        network->options.flap_every_ticks,
        totals.generated, totals.accepted, totals.delivered,
        totals.q0_generated, totals.q0_accepted, totals.q0_delivered,
        totals.q1_generated, totals.q1_accepted, totals.q1_delivered,
        delivery_pct,
        totals.payload_bytes, totals.wire_bytes, wire_efficiency,
        scale_global_percentile(network, totals.global_latency_samples, 95U),
        scale_global_percentile(network, totals.global_latency_samples, 99U),
        fairness, totals.control_frames, totals.forwarded_frames,
        totals.no_space, totals.no_route, totals.link_down,
        network->event_high_water, network->event_capacity,
        network->simulator_backpressure,
        network->duplicate_business_delivery ? 1U : 0U,
        network->route_loop_detected ? 1U : 0U,
        sizeof(ucn_node_t),
        (uint64_t)sizeof(ucn_node_t) * network->options.node_count,
        network->allocated_bytes, totals.host_work_ns / UINT64_C(1000000),
        network->wall_elapsed_ns / UINT64_C(1000000));
    return fclose(file) == 0;
}

static void scale_print_summary(const scale_network_t *network, bool passed)
{
    scale_totals_t totals = scale_collect_totals(network);
    double delivery_pct = totals.accepted == 0U ? 0.0 :
        (double)totals.delivered * 100.0 / (double)totals.accepted;
    double wire_efficiency = totals.wire_bytes == 0U ? 0.0 :
        (double)totals.payload_bytes * 100.0 / (double)totals.wire_bytes;
    double fairness = totals.sum_squared_deliveries == 0U ? 0.0 :
        ((double)totals.delivered * (double)totals.delivered) /
        ((double)network->options.node_count *
         (double)totals.sum_squared_deliveries);

    (void)printf(
        "SCALE_RESULT status=%s topology=%s traffic=%s nodes=%zu ticks=%"
        PRIu32 " generated=%" PRIu64 " accepted=%" PRIu64
        " delivered=%" PRIu64 " delivery=%.3f%% wire_eff=%.3f%%"
        " p95=%" PRIu32 "ms p99=%" PRIu32 "ms fairness=%.6f"
        " event_hwm=%zu/%zu sim_backpressure=%" PRIu64
        " duplicate_business=%u route_loop=%u host_alloc=%" PRIu64
        "B wall=%" PRIu64 "ms\n",
        passed ? "PASS" : "FAIL",
        scale_topology_name(network->options.topology),
        scale_traffic_name(network->options.traffic),
        network->options.node_count, network->options.ticks,
        totals.generated, totals.accepted, totals.delivered, delivery_pct,
        wire_efficiency,
        scale_global_percentile(network, totals.global_latency_samples, 95U),
        scale_global_percentile(network, totals.global_latency_samples, 99U),
        fairness, network->event_high_water, network->event_capacity,
        network->simulator_backpressure,
        network->duplicate_business_delivery ? 1U : 0U,
        network->route_loop_detected ? 1U : 0U,
        network->allocated_bytes,
        network->wall_elapsed_ns / UINT64_C(1000000));
}

static bool scale_write_reports(const scale_network_t *network, bool passed)
{
    char node_path[1024];
    char summary_path[1024];
    int node_length;
    int summary_length;

    if (network->options.report_prefix == NULL) {
        return true;
    }
    node_length = snprintf(node_path, sizeof(node_path), "%s_nodes.csv",
                           network->options.report_prefix);
    summary_length = snprintf(summary_path, sizeof(summary_path),
                              "%s_summary.csv",
                              network->options.report_prefix);
    if (node_length <= 0 || (size_t)node_length >= sizeof(node_path) ||
        summary_length <= 0 ||
        (size_t)summary_length >= sizeof(summary_path)) {
        return false;
    }
    return scale_write_node_csv(network, node_path) &&
           scale_write_summary_csv(network, summary_path, passed);
}

static bool scale_run(scale_network_t *network)
{
    uint32_t tick;

    for (tick = 0U; tick < network->options.warmup_ticks; ++tick) {
        network->current_tick = tick;
        if (!scale_generate_tick(network, tick) ||
            !scale_step_all(network, network->options.step_ms)) {
            return false;
        }
    }
    for (tick = 0U; tick < network->options.drain_ticks; ++tick) {
        if (!scale_step_all(network, network->options.step_ms)) {
            return false;
        }
    }
    scale_reset_measurement(network);
    network->measuring = true;
    network->wall_started_ns = scale_clock_ns();
    for (tick = 0U; tick < network->options.ticks; ++tick) {
        network->current_tick = tick;
        scale_restore_links(network);
        scale_maybe_flap_link(network);
        if (!scale_generate_tick(network, tick) ||
            !scale_step_all(network, network->options.step_ms)) {
            return false;
        }
    }
    for (tick = 0U; tick < network->options.drain_ticks; ++tick) {
        network->current_tick = network->options.ticks + tick;
        scale_restore_links(network);
        if (!scale_step_all(network, network->options.step_ms)) {
            return false;
        }
    }
    network->wall_elapsed_ns = scale_clock_ns() - network->wall_started_ns;
    if (!scale_routes_have_no_loop(network)) {
        return false;
    }
    return !network->fatal_error && !network->duplicate_business_delivery &&
           network->simulator_backpressure == 0U;
}

static void scale_usage(const char *program)
{
    (void)printf(
        "Usage: %s [options]\n"
        "  --nodes N                 2..4096 (default 32)\n"
        "  --ticks N                 measured ticks (default 200)\n"
        "  --warmup-ticks N          route warmup ticks (default 150)\n"
        "  --warmup-batch N          staged warmup sources/tick (0=all)\n"
        "  --drain-ticks N           drain ticks (default 300)\n"
        "  --step-ms N               virtual tick duration (default 10)\n"
        "  --messages-per-node N     Q1 messages per node/tick (default 1)\n"
        "  --payload-bytes N         12..UCN_MAX_PAYLOAD_BYTES\n"
        "  --topology tree|ring4     default tree\n"
        "  --traffic local|two-hop|pairs|incast|all-to-all|mixed\n"
        "  --loss-per-mille N        0..1000\n"
        "  --duplicate-per-mille N   0..1000\n"
        "  --delay-ms N               virtual maximum delay\n"
        "  --flap-every N             down one link pair every N ticks\n"
        "  --flap-duration N          link-down duration in ticks\n"
        "  --q0-every N               mixed-mode Q0 period in ticks\n"
        "  --event-slots N            host event heap slots\n"
        "  --seed N                   reproducible random seed\n"
        "  --report-prefix PATH       write PATH_nodes.csv/summary.csv\n"
        "  --quiet                    only final result\n",
        program);
}

static bool scale_parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);

    if (end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool scale_parse_options(int argc, char **argv, scale_options_t *options)
{
    int index;

    (void)memset(options, 0, sizeof(*options));
    options->node_count = SCALE_DEFAULT_NODES;
    options->ticks = SCALE_DEFAULT_TICKS;
    options->warmup_ticks = SCALE_DEFAULT_WARMUP_TICKS;
    options->drain_ticks = SCALE_DEFAULT_DRAIN_TICKS;
    options->step_ms = SCALE_DEFAULT_STEP_MS;
    options->messages_per_node = SCALE_DEFAULT_MESSAGES_PER_NODE;
    options->payload_bytes = SCALE_DEFAULT_PAYLOAD_BYTES;
    options->flap_duration_ticks = UINT32_C(50);
    options->q0_every_ticks = UINT32_C(10);
    options->seed = UINT32_C(0x5CA1E123);
    options->topology = SCALE_TOPOLOGY_TREE;
    options->traffic = SCALE_TRAFFIC_TWO_HOP;
    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        const char *value;
        uint32_t parsed;

        if (strcmp(argument, "--help") == 0) {
            scale_usage(argv[0]);
            exit(0);
        }
        if (strcmp(argument, "--quiet") == 0) {
            options->quiet = true;
            continue;
        }
        if (index + 1 >= argc) {
            return false;
        }
        value = argv[++index];
        if (strcmp(argument, "--topology") == 0) {
            if (strcmp(value, "tree") == 0) {
                options->topology = SCALE_TOPOLOGY_TREE;
            } else if (strcmp(value, "ring4") == 0) {
                options->topology = SCALE_TOPOLOGY_RING4;
            } else {
                return false;
            }
            continue;
        }
        if (strcmp(argument, "--traffic") == 0) {
            if (strcmp(value, "local") == 0) {
                options->traffic = SCALE_TRAFFIC_LOCAL;
            } else if (strcmp(value, "two-hop") == 0) {
                options->traffic = SCALE_TRAFFIC_TWO_HOP;
            } else if (strcmp(value, "pairs") == 0) {
                options->traffic = SCALE_TRAFFIC_PAIRS;
            } else if (strcmp(value, "incast") == 0) {
                options->traffic = SCALE_TRAFFIC_INCAST;
            } else if (strcmp(value, "all-to-all") == 0) {
                options->traffic = SCALE_TRAFFIC_ALL_TO_ALL;
            } else if (strcmp(value, "mixed") == 0) {
                options->traffic = SCALE_TRAFFIC_MIXED;
            } else {
                return false;
            }
            continue;
        }
        if (strcmp(argument, "--report-prefix") == 0) {
            options->report_prefix = value;
            continue;
        }
        if (!scale_parse_u32(value, &parsed)) {
            return false;
        }
        if (strcmp(argument, "--nodes") == 0) {
            options->node_count = parsed;
        } else if (strcmp(argument, "--ticks") == 0) {
            options->ticks = parsed;
        } else if (strcmp(argument, "--warmup-ticks") == 0) {
            options->warmup_ticks = parsed;
        } else if (strcmp(argument, "--warmup-batch") == 0) {
            options->warmup_batch = parsed;
        } else if (strcmp(argument, "--drain-ticks") == 0) {
            options->drain_ticks = parsed;
        } else if (strcmp(argument, "--step-ms") == 0) {
            options->step_ms = parsed;
        } else if (strcmp(argument, "--messages-per-node") == 0) {
            if (parsed > UINT16_MAX) {
                return false;
            }
            options->messages_per_node = (uint16_t)parsed;
        } else if (strcmp(argument, "--payload-bytes") == 0) {
            if (parsed > UINT16_MAX) {
                return false;
            }
            options->payload_bytes = (uint16_t)parsed;
        } else if (strcmp(argument, "--loss-per-mille") == 0) {
            if (parsed > UINT16_MAX) {
                return false;
            }
            options->loss_per_mille = (uint16_t)parsed;
        } else if (strcmp(argument, "--duplicate-per-mille") == 0) {
            if (parsed > UINT16_MAX) {
                return false;
            }
            options->duplicate_per_mille = (uint16_t)parsed;
        } else if (strcmp(argument, "--delay-ms") == 0) {
            if (parsed > UINT16_MAX) {
                return false;
            }
            options->max_delay_ms = (uint16_t)parsed;
        } else if (strcmp(argument, "--flap-every") == 0) {
            options->flap_every_ticks = parsed;
        } else if (strcmp(argument, "--flap-duration") == 0) {
            options->flap_duration_ticks = parsed;
        } else if (strcmp(argument, "--q0-every") == 0) {
            options->q0_every_ticks = parsed;
        } else if (strcmp(argument, "--event-slots") == 0) {
            options->event_capacity = parsed;
        } else if (strcmp(argument, "--seed") == 0) {
            options->seed = parsed;
        } else {
            return false;
        }
    }
    return options->node_count >= 2U && options->node_count <= SCALE_MAX_NODES &&
           options->ticks > 0U && options->ticks <= UINT32_C(1000000) &&
           options->warmup_ticks <= UINT32_C(1000000) &&
           options->warmup_batch <= options->node_count &&
           options->drain_ticks <= UINT32_C(1000000) &&
           options->step_ms > 0U && options->step_ms <= UINT32_C(60000) &&
           options->messages_per_node > 0U &&
           options->messages_per_node <= SCALE_MAX_MESSAGES_PER_NODE &&
           options->payload_bytes >= SCALE_PAYLOAD_META_BYTES &&
           options->payload_bytes <= UCN_MAX_PAYLOAD_BYTES &&
           options->loss_per_mille <= 1000U &&
           options->duplicate_per_mille <= 1000U &&
           options->max_delay_ms <= UINT16_C(60000) &&
           (options->event_capacity == 0U ||
            options->event_capacity <= (size_t)UINT32_C(4000000)) &&
           options->node_count * (size_t)options->ticks <=
               (size_t)UINT32_C(100000000) /
                   ((size_t)options->messages_per_node + 1U) &&
           options->flap_duration_ticks > 0U &&
           options->q0_every_ticks > 0U;
}

int main(int argc, char **argv)
{
    scale_options_t options;
    scale_network_t network;
    bool passed;
    bool reports_written;

    if (!scale_parse_options(argc, argv, &options)) {
        scale_usage(argv[0]);
        return 2;
    }
    if (!scale_network_init(&network, &options)) {
        (void)fprintf(stderr, "SCALE_INIT_FAILED nodes=%zu topology=%s\n",
                      options.node_count,
                      scale_topology_name(options.topology));
        scale_network_free(&network);
        return 3;
    }
    if (!options.quiet) {
        (void)printf(
            "SCALE_START nodes=%zu topology=%s traffic=%s ticks=%" PRIu32
            " warmup=%" PRIu32 " messages_per_node=%u event_slots=%zu"
            " node_storage=%zuB host_alloc=%" PRIu64 "B\n",
            options.node_count, scale_topology_name(options.topology),
            scale_traffic_name(options.traffic), options.ticks,
            options.warmup_ticks, (unsigned)options.messages_per_node,
            network.event_capacity, sizeof(ucn_node_t),
            network.allocated_bytes);
    }
    passed = scale_run(&network);
    scale_print_summary(&network, passed);
    reports_written = scale_write_reports(&network, passed);
    if (!reports_written) {
        (void)fprintf(stderr, "SCALE_REPORT_WRITE_FAILED prefix=%s\n",
                      options.report_prefix == NULL ? "(null)" :
                                                      options.report_prefix);
    }
    scale_network_free(&network);
    return passed && reports_written ? 0 : 1;
}
