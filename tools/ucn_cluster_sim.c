#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucn/ucn_cluster.h"

/* Default group size; smaller topologies select 2/4 via --group. */
#define CLUSTER_SIM_DEFAULT_GROUP_SIZE ((size_t)8U)
#define CLUSTER_SIM_MAX_GROUP_SIZE ((size_t)8U)
#define CLUSTER_SIM_STEP_MS UINT32_C(10)
#define CLUSTER_SIM_CLEAN_DURATION_MS UINT32_C(15000)
#define CLUSTER_SIM_IMPAIRED_DURATION_MS UINT32_C(60000)
#define CLUSTER_SIM_RECOVERY_DURATION_MS UINT32_C(38000)
#define CLUSTER_SIM_SCORE_SHIFT_MS UINT32_C(4000)
#define CLUSTER_SIM_HEAD_FAILURE_MS UINT32_C(12000)
#define CLUSTER_SIM_MEMBER_LEAVE_MS UINT32_C(12000)
#define CLUSTER_SIM_MEMBER_RETURN_MS UINT32_C(23000)
#define CLUSTER_SIM_CONTROL_WINDOW_MS UINT32_C(1000)
#define CLUSTER_SIM_CONTROL_WINDOW_LIMIT ((uint32_t)32U)
#define CLUSTER_SIM_FAST_CONTROL_WINDOW_LIMIT ((uint32_t)40U)

typedef enum cluster_sim_scenario {
    CLUSTER_SIM_SCENARIO_CLEAN = 0,
    CLUSTER_SIM_SCENARIO_IMPAIRED = 1,
    CLUSTER_SIM_SCENARIO_HEAD_FAILOVER = 2,
    CLUSTER_SIM_SCENARIO_MOBILITY = 3,
    CLUSTER_SIM_SCENARIO_SCORE_SHIFT = 4
} cluster_sim_scenario_t;

typedef struct cluster_sim_network cluster_sim_network_t;

typedef struct cluster_sim_node {
    cluster_sim_network_t *network;
    size_t index;
    bool alive;
    uint32_t control_window_started_ms;
    uint32_t control_window_count;
    uint32_t max_control_messages_per_window;
    ucn_cluster_t cluster;
} cluster_sim_node_t;

typedef struct cluster_sim_packet {
    size_t source;
    size_t destination;
    uint32_t deliver_at_ms;
    uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES];
} cluster_sim_packet_t;

struct cluster_sim_network {
    uint32_t now_ms;
    size_t node_count;
    size_t group_size;
    cluster_sim_scenario_t scenario;
    ucn_cluster_timing_profile_t timing_profile;
    uint32_t random_state;
    uint64_t transmit_attempts;
    uint64_t delivered_messages;
    uint64_t rejected_messages;
    uint64_t physically_dropped_messages;
    uint64_t unreachable_messages;
    uint64_t delayed_messages;
    uint64_t duplicated_messages;
    uint64_t transient_asymmetric_drops;
    cluster_sim_node_t *nodes;
    cluster_sim_packet_t *queue;
    size_t queue_capacity;
    size_t queue_count;
};

static const char *scenario_name(cluster_sim_scenario_t scenario)
{
    switch (scenario) {
        case CLUSTER_SIM_SCENARIO_CLEAN:
            return "clean";
        case CLUSTER_SIM_SCENARIO_IMPAIRED:
            return "impaired";
        case CLUSTER_SIM_SCENARIO_HEAD_FAILOVER:
            return "head-failover";
        case CLUSTER_SIM_SCENARIO_MOBILITY:
            return "mobility";
        case CLUSTER_SIM_SCENARIO_SCORE_SHIFT:
            return "score-shift";
        default:
            return "invalid";
    }
}

static bool parse_scenario(
    const char *text,
    cluster_sim_scenario_t *scenario)
{
    if (text == NULL || scenario == NULL) {
        return false;
    }
    if (strcmp(text, "clean") == 0) {
        *scenario = CLUSTER_SIM_SCENARIO_CLEAN;
    } else if (strcmp(text, "impaired") == 0) {
        *scenario = CLUSTER_SIM_SCENARIO_IMPAIRED;
    } else if (strcmp(text, "head-failover") == 0) {
        *scenario = CLUSTER_SIM_SCENARIO_HEAD_FAILOVER;
    } else if (strcmp(text, "mobility") == 0) {
        *scenario = CLUSTER_SIM_SCENARIO_MOBILITY;
    } else if (strcmp(text, "score-shift") == 0) {
        *scenario = CLUSTER_SIM_SCENARIO_SCORE_SHIFT;
    } else {
        return false;
    }
    return true;
}

static const char *timing_profile_name(ucn_cluster_timing_profile_t profile)
{
    switch (profile) {
        case UCN_CLUSTER_TIMING_PROFILE_DEFAULT:
            return "default";
        case UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED:
            return "fast-fixed";
        default:
            return "invalid";
    }
}

static bool parse_timing_profile(
    const char *text,
    ucn_cluster_timing_profile_t *profile)
{
    if (text == NULL || profile == NULL) {
        return false;
    }
    if (strcmp(text, "default") == 0) {
        *profile = UCN_CLUSTER_TIMING_PROFILE_DEFAULT;
    } else if (strcmp(text, "fast-fixed") == 0) {
        *profile = UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED;
    } else {
        return false;
    }
    return true;
}

static uint32_t scenario_duration_ms(cluster_sim_scenario_t scenario)
{
    switch (scenario) {
        case CLUSTER_SIM_SCENARIO_IMPAIRED:
            return CLUSTER_SIM_IMPAIRED_DURATION_MS;
        case CLUSTER_SIM_SCENARIO_HEAD_FAILOVER:
        case CLUSTER_SIM_SCENARIO_MOBILITY:
            return CLUSTER_SIM_RECOVERY_DURATION_MS;
        case CLUSTER_SIM_SCENARIO_SCORE_SHIFT:
            return UINT32_C(20000);
        case CLUSTER_SIM_SCENARIO_CLEAN:
        default:
            return CLUSTER_SIM_CLEAN_DURATION_MS;
    }
}

static uint32_t scenario_final_phase_ms(cluster_sim_scenario_t scenario)
{
    switch (scenario) {
        case CLUSTER_SIM_SCENARIO_SCORE_SHIFT:
            return CLUSTER_SIM_SCORE_SHIFT_MS;
        case CLUSTER_SIM_SCENARIO_HEAD_FAILOVER:
            return CLUSTER_SIM_HEAD_FAILURE_MS;
        case CLUSTER_SIM_SCENARIO_MOBILITY:
            return CLUSTER_SIM_MEMBER_RETURN_MS;
        case CLUSTER_SIM_SCENARIO_CLEAN:
        case CLUSTER_SIM_SCENARIO_IMPAIRED:
        default:
            return 0U;
    }
}

static uint32_t control_window_limit(
    ucn_cluster_timing_profile_t timing_profile)
{
    return timing_profile == UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED ?
               CLUSTER_SIM_FAST_CONTROL_WINDOW_LIMIT :
               CLUSTER_SIM_CONTROL_WINDOW_LIMIT;
}

static uint32_t sim_random(cluster_sim_network_t *network)
{
    uint32_t value = network->random_state;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    network->random_state = value;
    return value;
}

static uint32_t sim_now(void *context)
{
    const cluster_sim_node_t *node = (const cluster_sim_node_t *)context;

    return node->network->now_ms;
}

static void observe_control_attempt(cluster_sim_node_t *node)
{
    uint32_t elapsed = node->network->now_ms -
                       node->control_window_started_ms;

    if (elapsed >= CLUSTER_SIM_CONTROL_WINDOW_MS) {
        if (node->control_window_count >
            node->max_control_messages_per_window) {
            node->max_control_messages_per_window =
                node->control_window_count;
        }
        node->control_window_started_ms = node->network->now_ms;
        node->control_window_count = 0U;
    }
    node->control_window_count++;
}

static ucn_result_t queue_packet(
    cluster_sim_network_t *network,
    size_t source,
    size_t destination,
    const uint8_t *payload,
    uint32_t delay_ms)
{
    cluster_sim_packet_t *packet;

    if (network->queue_count >= network->queue_capacity) {
        return UCN_ERR_NO_SPACE;
    }
    packet = &network->queue[network->queue_count++];
    packet->source = source;
    packet->destination = destination;
    packet->deliver_at_ms = network->now_ms + delay_ms;
    (void)memcpy(packet->payload, payload, sizeof(packet->payload));
    return UCN_OK;
}

static bool transient_asymmetric_drop(
    const cluster_sim_network_t *network,
    size_t source,
    size_t destination)
{
    size_t source_position = source % network->group_size;
    size_t destination_position = destination % network->group_size;

    return network->scenario == CLUSTER_SIM_SCENARIO_IMPAIRED &&
           network->now_ms >= UINT32_C(5000) &&
           network->now_ms < UINT32_C(7000) && source_position == 0U &&
           (destination_position & 1U) != 0U;
}

static ucn_result_t sim_send(
    void *context,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length)
{
    cluster_sim_node_t *source = (cluster_sim_node_t *)context;
    cluster_sim_network_t *network = source->network;
    size_t destination_index;
    uint32_t random_value;
    uint32_t delay_ms = 0U;
    ucn_result_t result;

    if (destination == 0U || destination > network->node_count ||
        endpoint != UCN_CLUSTER_CONTROL_ENDPOINT || payload == NULL ||
        payload_length != UCN_CLUSTER_MESSAGE_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    destination_index = (size_t)(destination - 1U);
    if (source->index / network->group_size !=
        destination_index / network->group_size) {
        return UCN_ERR_NOT_FOUND;
    }
    network->transmit_attempts++;
    observe_control_attempt(source);
    if (!source->alive || !network->nodes[destination_index].alive) {
        network->unreachable_messages++;
        return UCN_ERR_LINK_DOWN;
    }
    if (transient_asymmetric_drop(network, source->index,
                                  destination_index)) {
        network->physically_dropped_messages++;
        network->transient_asymmetric_drops++;
        return UCN_OK;
    }
    if (network->scenario == CLUSTER_SIM_SCENARIO_IMPAIRED) {
        random_value = sim_random(network);
        if (random_value % 100U < 15U) {
            network->physically_dropped_messages++;
            return UCN_OK;
        }
        delay_ms = sim_random(network) % 41U;
        if (delay_ms != 0U) {
            network->delayed_messages++;
        }
    }
    result = queue_packet(network, source->index, destination_index, payload,
                          delay_ms);
    if (result != UCN_OK) {
        return result;
    }
    if (network->scenario == CLUSTER_SIM_SCENARIO_IMPAIRED &&
        sim_random(network) % 100U < 5U) {
        result = queue_packet(network, source->index, destination_index,
                              payload, delay_ms + 10U);
        if (result != UCN_OK) {
            return result;
        }
        network->duplicated_messages++;
    }
    return UCN_OK;
}

static int sim_sync_neighbors(cluster_sim_network_t *network)
{
    size_t index;

    for (index = 0U; index < network->node_count; ++index) {
        ucn_neighbor_summary_t summaries[CLUSTER_SIM_MAX_GROUP_SIZE - 1U];
        size_t group_start =
            (index / network->group_size) * network->group_size;
        size_t peer;
        size_t count = 0U;

        if (!network->nodes[index].alive) {
            continue;
        }
        (void)memset(summaries, 0, sizeof(summaries));
        for (peer = group_start; peer < group_start + network->group_size;
             ++peer) {
            if (peer == index || !network->nodes[peer].alive) {
                continue;
            }
            summaries[count].state = UCN_NEIGHBOR_ADMITTED;
            summaries[count].peer_node_id = (ucn_node_id_t)(peer + 1U);
            summaries[count].bearer_count = 1U;
            summaries[count].primary_bearer_index = 0U;
            summaries[count].last_seen_ms = network->now_ms;
            ++count;
        }
        if (ucn_cluster_sync_neighbors(&network->nodes[index].cluster,
                                       summaries, count) != UCN_OK) {
            return 1;
        }
    }
    return 0;
}

static int sim_deliver(cluster_sim_network_t *network)
{
    size_t read_index;
    size_t write_index = 0U;

    for (read_index = 0U; read_index < network->queue_count; ++read_index) {
        cluster_sim_packet_t packet = network->queue[read_index];
        ucn_result_t result;

        if (packet.deliver_at_ms > network->now_ms) {
            network->queue[write_index++] = packet;
            continue;
        }
        if (!network->nodes[packet.source].alive ||
            !network->nodes[packet.destination].alive) {
            network->unreachable_messages++;
            continue;
        }
        result = ucn_cluster_receive(
            &network->nodes[packet.destination].cluster,
            (ucn_node_id_t)(packet.source + 1U), true,
            packet.payload, sizeof(packet.payload));
        if (result == UCN_OK) {
            network->delivered_messages++;
        } else if (result == UCN_ERR_ACCESS || result == UCN_ERR_NO_SPACE ||
                   result == UCN_ERR_REPLAY || result == UCN_ERR_NOT_FOUND) {
            network->rejected_messages++;
        } else {
            fprintf(stderr, "unexpected cluster receive result=%d\n",
                    (int)result);
            return 1;
        }
    }
    network->queue_count = write_index;
    return 0;
}

static size_t expected_head_index(
    const cluster_sim_network_t *network,
    size_t group_start,
    bool final_phase)
{
    if (final_phase &&
        (network->scenario == CLUSTER_SIM_SCENARIO_HEAD_FAILOVER ||
         network->scenario == CLUSTER_SIM_SCENARIO_SCORE_SHIFT)) {
        return group_start + 1U;
    }
    return group_start;
}

static bool sim_is_converged(
    const cluster_sim_network_t *network,
    bool final_phase)
{
    size_t group_start;

    for (group_start = 0U; group_start < network->node_count;
         group_start += network->group_size) {
        size_t expected_head = expected_head_index(network, group_start,
                                                   final_phase);
        size_t index;

        if (!network->nodes[expected_head].alive ||
            network->nodes[expected_head].cluster.role !=
                UCN_CLUSTER_ROLE_HEAD) {
            return false;
        }
        for (index = group_start; index < group_start + network->group_size;
             ++index) {
            const cluster_sim_node_t *node = &network->nodes[index];

            if (!node->alive || index == expected_head) {
                continue;
            }
            if ((node->cluster.role != UCN_CLUSTER_ROLE_MEMBER &&
                 node->cluster.role != UCN_CLUSTER_ROLE_BACKUP) ||
                node->cluster.head_node_id !=
                    (ucn_node_id_t)(expected_head + 1U)) {
                return false;
            }
        }
    }
    return true;
}

static int sim_init(
    cluster_sim_network_t *network,
    size_t node_count,
    size_t group_size,
    cluster_sim_scenario_t scenario,
    ucn_cluster_timing_profile_t timing_profile,
    uint32_t seed)
{
    size_t index;

    (void)memset(network, 0, sizeof(*network));
    network->node_count = node_count;
    network->group_size = group_size;
    network->scenario = scenario;
    network->timing_profile = timing_profile;
    network->random_state = seed == 0U ? UINT32_C(1) : seed;
    network->queue_capacity = node_count * 64U;
    network->nodes = (cluster_sim_node_t *)calloc(node_count,
                                                   sizeof(*network->nodes));
    network->queue = (cluster_sim_packet_t *)calloc(network->queue_capacity,
                                                     sizeof(*network->queue));
    if (network->nodes == NULL || network->queue == NULL) {
        return 1;
    }
    for (index = 0U; index < node_count; ++index) {
        cluster_sim_node_t *node = &network->nodes[index];
        ucn_cluster_config_t config;
        size_t group_position = index % network->group_size;

        node->network = network;
        node->index = index;
        node->alive = true;
        (void)memset(&config, 0, sizeof(config));
        config.local_node_id = (ucn_node_id_t)(index + 1U);
        config.enabled = true;
        config.head_capable = group_position < 2U;
        config.require_protected_control = true;
        config.head_score = group_position == 0U ? 9000U :
                            group_position == 1U ? 7000U : 2000U;
        config.member_capacity = config.head_capable ?
                                     (uint16_t)(network->group_size - 1U) : 0U;
        if (ucn_cluster_config_apply_timing_profile(&config,
                                                    timing_profile) != UCN_OK) {
            return 1;
        }
        config.now_ms = sim_now;
        config.now_context = node;
        config.send = sim_send;
        config.send_context = node;
        if (ucn_cluster_init(&node->cluster, &config) != UCN_OK) {
            return 1;
        }
    }
    return sim_sync_neighbors(network);
}

static void sim_destroy(cluster_sim_network_t *network)
{
    free(network->queue);
    free(network->nodes);
}

static int apply_scenario_events(
    cluster_sim_network_t *network,
    uint32_t now_ms)
{
    size_t group_start;

    if (network->scenario == CLUSTER_SIM_SCENARIO_SCORE_SHIFT &&
        now_ms == CLUSTER_SIM_SCORE_SHIFT_MS) {
        for (group_start = 0U; group_start < network->node_count;
             group_start += network->group_size) {
            if (ucn_cluster_set_head_score(
                    &network->nodes[group_start].cluster, 6000U) != UCN_OK ||
                ucn_cluster_set_head_score(
                    &network->nodes[group_start + 1U].cluster, 9500U) != UCN_OK) {
                return 1;
            }
        }
    }
    if (network->scenario == CLUSTER_SIM_SCENARIO_HEAD_FAILOVER &&
        now_ms == CLUSTER_SIM_HEAD_FAILURE_MS) {
        for (group_start = 0U; group_start < network->node_count;
             group_start += network->group_size) {
            network->nodes[group_start].alive = false;
        }
    }
    if (network->scenario == CLUSTER_SIM_SCENARIO_MOBILITY &&
        (now_ms == CLUSTER_SIM_MEMBER_LEAVE_MS ||
         now_ms == CLUSTER_SIM_MEMBER_RETURN_MS)) {
        bool alive = now_ms == CLUSTER_SIM_MEMBER_RETURN_MS;

        for (group_start = 0U; group_start < network->node_count;
             group_start += network->group_size) {
            network->nodes[group_start + network->group_size - 1U].alive =
                alive;
        }
    }
    return 0;
}

static void count_final_state(
    cluster_sim_network_t *network,
    size_t *heads,
    size_t *members,
    size_t *alive_nodes,
    uint64_t *messages_sent,
    uint64_t *head_switches,
    uint32_t *max_control_window)
{
    size_t index;

    *heads = 0U;
    *members = 0U;
    *alive_nodes = 0U;
    *messages_sent = 0U;
    *head_switches = 0U;
    *max_control_window = 0U;
    for (index = 0U; index < network->node_count; ++index) {
        cluster_sim_node_t *node = &network->nodes[index];

        if (node->control_window_count >
            node->max_control_messages_per_window) {
            node->max_control_messages_per_window = node->control_window_count;
        }
        if (node->max_control_messages_per_window > *max_control_window) {
            *max_control_window = node->max_control_messages_per_window;
        }
        if (node->alive) {
            (*alive_nodes)++;
            if (node->cluster.role == UCN_CLUSTER_ROLE_HEAD) {
                (*heads)++;
            } else if (node->cluster.role == UCN_CLUSTER_ROLE_MEMBER ||
                       node->cluster.role == UCN_CLUSTER_ROLE_BACKUP) {
                (*members)++;
            }
        }
        *messages_sent += node->cluster.stats.messages_sent;
        *head_switches += node->cluster.stats.head_switches;
    }
}

static int run_simulation(
    size_t node_count,
    size_t group_size,
    cluster_sim_scenario_t scenario,
    ucn_cluster_timing_profile_t timing_profile,
    uint32_t seed)
{
    cluster_sim_network_t network;
    uint32_t now_ms;
    uint32_t duration_ms = scenario_duration_ms(scenario);
    uint32_t final_phase_ms = scenario_final_phase_ms(scenario);
    uint32_t initial_converged_ms = 0U;
    uint32_t final_converged_ms = 0U;
    uint32_t recovery_ms;
    size_t groups;
    size_t heads;
    size_t members;
    size_t alive_nodes;
    size_t expected_members;
    uint64_t messages_sent;
    uint64_t head_switches;
    uint32_t max_control_window;
    size_t index;

    if (sim_init(&network, node_count, group_size, scenario, timing_profile,
                 seed) != 0) {
        fprintf(stderr, "cluster simulator init failed\n");
        sim_destroy(&network);
        return 1;
    }
    groups = node_count / network.group_size;
    for (now_ms = 0U; now_ms <= duration_ms; now_ms += CLUSTER_SIM_STEP_MS) {
        network.now_ms = now_ms;
        if (apply_scenario_events(&network, now_ms) != 0 ||
            sim_sync_neighbors(&network) != 0) {
            sim_destroy(&network);
            return 1;
        }
        for (index = 0U; index < node_count; ++index) {
            if (network.nodes[index].alive &&
                ucn_cluster_step(&network.nodes[index].cluster) != UCN_OK) {
                sim_destroy(&network);
                return 1;
            }
        }
        if (sim_deliver(&network) != 0) {
            sim_destroy(&network);
            return 1;
        }
        if (initial_converged_ms == 0U &&
            sim_is_converged(&network, false)) {
            initial_converged_ms = now_ms;
        }
        if (now_ms >= final_phase_ms && final_converged_ms == 0U &&
            sim_is_converged(&network, true)) {
            final_converged_ms = now_ms;
        }
    }
    count_final_state(&network, &heads, &members, &alive_nodes,
                      &messages_sent, &head_switches, &max_control_window);
    expected_members = alive_nodes - groups;
    recovery_ms = final_converged_ms >= final_phase_ms ?
                      final_converged_ms - final_phase_ms : 0U;
    printf("CLUSTER_SIM nodes=%zu scenario=%s profile=%s groups=%zu heads=%zu "
           "members=%zu converged_ms=%lu initial_ms=%lu recovery_ms=%lu "
           "sent=%llu tx=%llu delivered=%llu rejected=%llu dropped=%llu "
           "unreachable=%llu delayed=%llu duplicated=%llu asymmetric=%llu "
           "head_switches=%llu max_tx_1s=%lu object_bytes=%zu\n",
            node_count, scenario_name(scenario), timing_profile_name(timing_profile),
            groups, heads, members,
           (unsigned long)final_converged_ms,
           (unsigned long)initial_converged_ms,
           (unsigned long)recovery_ms,
           (unsigned long long)messages_sent,
           (unsigned long long)network.transmit_attempts,
           (unsigned long long)network.delivered_messages,
           (unsigned long long)network.rejected_messages,
           (unsigned long long)network.physically_dropped_messages,
           (unsigned long long)network.unreachable_messages,
           (unsigned long long)network.delayed_messages,
           (unsigned long long)network.duplicated_messages,
           (unsigned long long)network.transient_asymmetric_drops,
           (unsigned long long)head_switches,
           (unsigned long)max_control_window, sizeof(ucn_cluster_t));
    if (final_converged_ms == 0U || heads != groups ||
        members != expected_members ||
         max_control_window > control_window_limit(timing_profile) ||
         ((scenario == CLUSTER_SIM_SCENARIO_HEAD_FAILOVER ||
           scenario == CLUSTER_SIM_SCENARIO_MOBILITY) &&
          initial_converged_ms == 0U) ||
         (timing_profile == UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED &&
          scenario == CLUSTER_SIM_SCENARIO_HEAD_FAILOVER &&
          recovery_ms > UINT32_C(4000)) ||
        (scenario == CLUSTER_SIM_SCENARIO_IMPAIRED &&
         (network.physically_dropped_messages == 0U ||
          network.delayed_messages == 0U ||
          network.duplicated_messages == 0U ||
          network.transient_asymmetric_drops == 0U))) {
        sim_destroy(&network);
        return 1;
    }
    sim_destroy(&network);
    return 0;
}

static void print_usage(void)
{
    fprintf(stderr,
             "usage: ucn_cluster_sim [--nodes N] "
             "[--scenario clean|impaired|head-failover|mobility|score-shift] "
             "[--profile default|fast-fixed] [--group 2|4|8] "
             "[--seed N]\n");
}

int main(int argc, char **argv)
{
    size_t node_count = 64U;
    size_t group_size = CLUSTER_SIM_DEFAULT_GROUP_SIZE;
    cluster_sim_scenario_t scenario = CLUSTER_SIM_SCENARIO_CLEAN;
    ucn_cluster_timing_profile_t timing_profile =
        UCN_CLUSTER_TIMING_PROFILE_DEFAULT;
    uint32_t seed = UINT32_C(0x5EED1234);
    int argument_index;

    for (argument_index = 1; argument_index < argc; ++argument_index) {
        if (strcmp(argv[argument_index], "--nodes") == 0 &&
            argument_index + 1 < argc) {
            char *end = NULL;
            unsigned long parsed = strtoul(argv[++argument_index], &end, 10);

            if (end == NULL || *end != '\0' || parsed > 10000UL) {
                print_usage();
                return 2;
            }
            node_count = (size_t)parsed;
        } else if (strcmp(argv[argument_index], "--group") == 0 &&
                   argument_index + 1 < argc) {
            char *end = NULL;
            unsigned long parsed = strtoul(argv[++argument_index], &end, 10);

            if (end == NULL || *end != 0 ||
                (parsed != 2UL && parsed != 4UL && parsed != 8UL)) {
                print_usage();
                return 2;
            }
            group_size = (size_t)parsed;
        } else if (strcmp(argv[argument_index], "--scenario") == 0 &&
                   argument_index + 1 < argc) {
            if (!parse_scenario(argv[++argument_index], &scenario)) {
                print_usage();
                return 2;
            }
        } else if (strcmp(argv[argument_index], "--seed") == 0 &&
                   argument_index + 1 < argc) {
            char *end = NULL;
            unsigned long parsed = strtoul(argv[++argument_index], &end, 10);

            if (end == NULL || *end != '\0' || parsed > UINT32_MAX) {
                print_usage();
                return 2;
            }
            seed = (uint32_t)parsed;
        } else if (strcmp(argv[argument_index], "--profile") == 0 &&
                   argument_index + 1 < argc) {
            if (!parse_timing_profile(argv[++argument_index],
                                      &timing_profile)) {
                print_usage();
                return 2;
            }
        } else {
            print_usage();
            return 2;
        }
    }
    if (node_count < group_size || node_count % group_size != 0U ||
        group_size > CLUSTER_SIM_MAX_GROUP_SIZE) {
        fprintf(stderr,
                "--nodes must be a multiple of --group (2/4/8)\n");
        return 2;
    }
    return run_simulation(node_count, group_size, scenario, timing_profile,
                           seed);
}
