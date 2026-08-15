#include "test_support.h"

#include <string.h>

#include "cluster_test_fixture.h"
#include "ucn/ucn_cluster.h"

#define CLUSTER_TEST_NODES ((size_t)4U)
#define CLUSTER_TEST_QUEUE ((size_t)512U)

typedef struct cluster_test_network cluster_test_network_t;

typedef struct cluster_test_node {
    cluster_test_network_t *network;
    ucn_node_id_t node_id;
    bool alive;
    ucn_cluster_t cluster;
} cluster_test_node_t;

typedef struct cluster_test_packet {
    ucn_node_id_t source;
    ucn_node_id_t destination;
    uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES];
} cluster_test_packet_t;

struct cluster_test_network {
    uint32_t now_ms;
    cluster_test_node_t nodes[CLUSTER_TEST_NODES];
    cluster_test_packet_t queue[CLUSTER_TEST_QUEUE];
    size_t queue_count;
};

#if UCN_FEATURE_DYNAMIC_MESH
static ucn_result_t cluster_summary_link_send(
    ucn_link_t *link,
    const uint8_t *frame,
    size_t length)
{
    (void)link;
    (void)frame;
    (void)length;
    return UCN_OK;
}

static ucn_result_t cluster_summary_link_status(
    const ucn_link_t *link,
    ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t CLUSTER_SUMMARY_LINK_OPS = {
    NULL, cluster_summary_link_send, NULL, cluster_summary_link_status,
    NULL, NULL
};
#endif

static uint32_t cluster_test_now(void *context)
{
    cluster_test_node_t *node = (cluster_test_node_t *)context;

    return node->network->now_ms;
}

static cluster_test_node_t *cluster_test_find_node(
    cluster_test_network_t *network,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        if (network->nodes[index].node_id == node_id) {
            return &network->nodes[index];
        }
    }
    return NULL;
}

static ucn_result_t cluster_test_send(
    void *context,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length)
{
    cluster_test_node_t *source = (cluster_test_node_t *)context;
    cluster_test_network_t *network = source->network;
    cluster_test_node_t *target = cluster_test_find_node(network, destination);
    cluster_test_packet_t *packet;

    if (!source->alive || target == NULL || !target->alive) {
        return UCN_ERR_LINK_DOWN;
    }
    if (endpoint != UCN_CLUSTER_CONTROL_ENDPOINT ||
        payload == NULL || payload_length != UCN_CLUSTER_MESSAGE_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    if (network->queue_count >= CLUSTER_TEST_QUEUE) {
        return UCN_ERR_NO_SPACE;
    }
    packet = &network->queue[network->queue_count++];
    packet->source = source->node_id;
    packet->destination = destination;
    (void)memcpy(packet->payload, payload, sizeof(packet->payload));
    return UCN_OK;
}

static int cluster_test_sync_neighbors(cluster_test_network_t *network)
{
    size_t node_index;

    for (node_index = 0U; node_index < CLUSTER_TEST_NODES; ++node_index) {
        cluster_test_node_t *node = &network->nodes[node_index];
        ucn_neighbor_summary_t summaries[CLUSTER_TEST_NODES - 1U];
        size_t peer_index;
        size_t count = 0U;

        if (!node->alive) {
            continue;
        }
        (void)memset(summaries, 0, sizeof(summaries));
        for (peer_index = 0U; peer_index < CLUSTER_TEST_NODES; ++peer_index) {
            if (peer_index == node_index || !network->nodes[peer_index].alive) {
                continue;
            }
            summaries[count].state = UCN_NEIGHBOR_ADMITTED;
            summaries[count].peer_node_id = network->nodes[peer_index].node_id;
            summaries[count].bearer_count = 1U;
            summaries[count].primary_bearer_index = 0U;
            summaries[count].last_seen_ms = network->now_ms;
            ++count;
        }
        TEST_ASSERT(ucn_cluster_sync_neighbors(&node->cluster, summaries, count) ==
                    UCN_OK);
    }
    return 0;
}

static int cluster_test_deliver(cluster_test_network_t *network)
{
    size_t index = 0U;

    while (index < network->queue_count) {
        cluster_test_packet_t packet = network->queue[index++];
        cluster_test_node_t *target =
            cluster_test_find_node(network, packet.destination);
        ucn_result_t result;

        if (target == NULL || !target->alive) {
            continue;
        }
        result = ucn_cluster_receive(&target->cluster, packet.source, true,
                                     packet.payload, sizeof(packet.payload));
        TEST_ASSERT(result == UCN_OK || result == UCN_ERR_ACCESS ||
                    result == UCN_ERR_NO_SPACE || result == UCN_ERR_REPLAY ||
                    result == UCN_ERR_NOT_FOUND);
    }
    network->queue_count = 0U;
    return 0;
}

/* CLV2-M00-07: fault-injection knobs for the virtual network.  All of
 * these are test-only; the production cluster code is never touched. */
typedef struct cluster_test_fault {
    /* drop_one_in == 0 disables; otherwise every Nth frame is dropped. */
    uint32_t drop_one_in;
    /* per-pair reachability: partition[a][b] == false blocks a->b. */
    bool partition[CLUSTER_TEST_NODES][CLUSTER_TEST_NODES];
    /* deliver at most this many frames per tick (0 = unlimited). */
    size_t deliver_budget;
    /* restart a node at a given tick: re-init its cluster to DETACHED
     * with a fresh nonce counter, simulating a power cycle. */
    uint32_t restart_node_id;
    uint32_t restart_at_ms;
    bool restart_done;
} cluster_test_fault_t;

static int cluster_test_sync_neighbors_faulted(
    cluster_test_network_t *network,
    const cluster_test_fault_t *fault)
{
    size_t node_index;

    for (node_index = 0U; node_index < CLUSTER_TEST_NODES; ++node_index) {
        cluster_test_node_t *node = &network->nodes[node_index];
        ucn_neighbor_summary_t summaries[CLUSTER_TEST_NODES - 1U];
        size_t peer_index;
        size_t count = 0U;

        if (!node->alive) {
            continue;
        }
        (void)memset(summaries, 0, sizeof(summaries));
        for (peer_index = 0U; peer_index < CLUSTER_TEST_NODES; ++peer_index) {
            if (peer_index == node_index ||
                !network->nodes[peer_index].alive) {
                continue;
            }
            if (fault != NULL &&
                !fault->partition[node_index][peer_index]) {
                continue;
            }
            summaries[count].state = UCN_NEIGHBOR_ADMITTED;
            summaries[count].peer_node_id = network->nodes[peer_index].node_id;
            summaries[count].bearer_count = 1U;
            summaries[count].primary_bearer_index = 0U;
            summaries[count].last_seen_ms = network->now_ms;
            ++count;
        }
        TEST_ASSERT(ucn_cluster_sync_neighbors(&node->cluster, summaries, count) ==
                    UCN_OK);
    }
    return 0;
}

static int cluster_test_deliver_faulted(cluster_test_network_t *network,
                                        const cluster_test_fault_t *fault)
{
    size_t index = 0U;
    size_t delivered = 0U;
    uint32_t seen = 0U;

    while (index < network->queue_count) {
        cluster_test_packet_t packet = network->queue[index++];
        cluster_test_node_t *target =
            cluster_test_find_node(network, packet.destination);
        ucn_result_t result;

        if (target == NULL || !target->alive) {
            continue;
        }
        /* Deterministic drop: every Nth frame in the queue is dropped. */
        seen++;
        if (fault != NULL && fault->drop_one_in != 0U &&
            (seen % fault->drop_one_in) == 0U) {
            continue;
        }
        if (fault != NULL && fault->deliver_budget != 0U &&
            delivered >= fault->deliver_budget) {
            /* Preserve the un-delivered tail (including the current
             * packet, already dequeued by index++) for the next tick. */
            size_t remain = network->queue_count - (index - 1U);

            (void)memmove(&network->queue[0], &network->queue[index - 1U],
                          remain * sizeof(network->queue[0]));
            network->queue_count = remain;
            return 0;
        }
        delivered++;
        result = ucn_cluster_receive(&target->cluster, packet.source, true,
                                     packet.payload, sizeof(packet.payload));
        TEST_ASSERT(result == UCN_OK || result == UCN_ERR_ACCESS ||
                    result == UCN_ERR_NO_SPACE || result == UCN_ERR_REPLAY ||
                    result == UCN_ERR_NOT_FOUND);
    }
    network->queue_count = 0U;
    return 0;
}

static int cluster_test_tick_faulted(cluster_test_network_t *network,
                                     uint32_t now_ms,
                                     cluster_test_fault_t *fault)
{
    size_t index;

    network->now_ms = now_ms;
    if (fault != NULL && !fault->restart_done &&
        fault->restart_at_ms != 0U && now_ms >= fault->restart_at_ms) {
        for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
            if (network->nodes[index].node_id == fault->restart_node_id) {
                ucn_cluster_config_t config = network->nodes[index].cluster.config;

                network->nodes[index].alive = true;
                TEST_ASSERT(ucn_cluster_init(&network->nodes[index].cluster,
                                             &config) == UCN_OK);
                fault->restart_done = true;
                break;
            }
        }
    }
    TEST_ASSERT(cluster_test_sync_neighbors_faulted(network, fault) == 0);
    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        if (network->nodes[index].alive) {
            TEST_ASSERT(ucn_cluster_step(&network->nodes[index].cluster) == UCN_OK);
        }
    }
    TEST_ASSERT(cluster_test_deliver_faulted(network, fault) == 0);
    return 0;
}

static int cluster_test_tick(cluster_test_network_t *network, uint32_t now_ms)
{
    size_t index;

    network->now_ms = now_ms;
    TEST_ASSERT(cluster_test_sync_neighbors(network) == 0);
    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        if (network->nodes[index].alive) {
            TEST_ASSERT(ucn_cluster_step(&network->nodes[index].cluster) == UCN_OK);
        }
    }
    TEST_ASSERT(cluster_test_deliver(network) == 0);
    return 0;
}

/* CLV2-M00-03 (Golden Transition Trace): test-only observation layer.
 * Snapshots the public cluster state of every node before/after a tick and
 * records deterministic transition lines.  The production code is NOT
 * touched; this only proves that a later M01 refactor preserves behaviour
 * by byte-comparing traces under the same seed. */
typedef struct cluster_trace_snapshot {
    uint32_t now_ms;
    ucn_node_id_t node_id;
    ucn_cluster_role_t role;
    uint32_t cluster_id;
    uint32_t term;
    uint32_t backup_generation;
    uint32_t membership_sequence;
    uint8_t backup_ready;
    uint8_t backup_syncing;
    uint8_t backup_takeover_active;
    uint8_t recovery_eligible;
} cluster_trace_snapshot_t;

static void cluster_trace_capture(cluster_test_network_t *network,
                                  cluster_trace_snapshot_t *out)
{
    size_t index;

    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        cluster_trace_snapshot_t *entry = &out[index];
        const ucn_cluster_t *c = &network->nodes[index].cluster;

        entry->now_ms = network->now_ms;
        entry->node_id = network->nodes[index].node_id;
        entry->role = c->role;
        entry->cluster_id = c->cluster_id;
        entry->term = c->term;
        entry->backup_generation = c->backup_generation;
        entry->membership_sequence = c->membership_sequence;
        entry->backup_ready = c->backup_ready ? 1U : 0U;
        entry->backup_syncing = c->backup_syncing ? 1U : 0U;
        entry->backup_takeover_active = c->backup_takeover_active ? 1U : 0U;
        entry->recovery_eligible = c->recovery_eligible ? 1U : 0U;
    }
}

static bool cluster_trace_snapshot_equal(const cluster_trace_snapshot_t *left,
                                         const cluster_trace_snapshot_t *right)
{
    return left->role == right->role &&
           left->cluster_id == right->cluster_id &&
           left->term == right->term &&
           left->backup_generation == right->backup_generation &&
           left->membership_sequence == right->membership_sequence &&
           left->backup_ready == right->backup_ready &&
           left->backup_syncing == right->backup_syncing &&
           left->backup_takeover_active == right->backup_takeover_active &&
           left->recovery_eligible == right->recovery_eligible;
}

static void cluster_trace_write_line(FILE *stream,
                                     const cluster_trace_snapshot_t *entry,
                                     const char *event)
{
    (void)fprintf(stream,
        "t=%lu node=%lu %s role=%d cid=%lu term=%lu gen=%lu seq=%lu "
        "ready=%u syncing=%u takeover=%u recovery=%u\n",
        (unsigned long)entry->now_ms, (unsigned long)entry->node_id, event,
        (int)entry->role, (unsigned long)entry->cluster_id,
        (unsigned long)entry->term, (unsigned long)entry->backup_generation,
        (unsigned long)entry->membership_sequence,
        (unsigned)entry->backup_ready, (unsigned)entry->backup_syncing,
        (unsigned)entry->backup_takeover_active,
        (unsigned)entry->recovery_eligible);
}

static int cluster_test_network_init(
    cluster_test_network_t *network,
    uint16_t head_capacity)
{
    static const uint16_t scores[CLUSTER_TEST_NODES] = {
        9000U, 7000U, 3000U, 2000U
    };
    size_t index;

    (void)memset(network, 0, sizeof(*network));
    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        cluster_test_node_t *node = &network->nodes[index];
        ucn_cluster_config_t config;

        node->network = network;
        node->node_id = (ucn_node_id_t)(index + 1U);
        node->alive = true;
        (void)memset(&config, 0, sizeof(config));
        config.local_node_id = node->node_id;
        config.enabled = true;
        config.head_capable = index < 2U;
        config.require_protected_control = true;
        config.head_score = scores[index];
        config.member_capacity = config.head_capable ? head_capacity : 0U;
        config.observation_ms = 10U;
        config.recovery_observation_ms = 10U;
        config.election_window_ms = 20U;
        config.advertise_interval_ms = 9U;
        config.join_retry_ms = 5U;
        config.keepalive_interval_ms = 10U;
        config.lease_ms = 40U;
        config.head_min_tenure_ms = 50U;
        config.switch_improvement_percent = 20U;
        config.switch_required_samples = 3U;
        /* Disable control-plane throttling: these fixtures compress real
         * time (ms-scale lease/advertise), so §9.1 budgets would starve
         * the deterministic state transitions under test. */
        config.token_bucket.burst = UINT16_MAX;
        config.token_bucket.refill_ms = 1U;
        config.recovery_head_ttl_ms = 30U;
        config.recovery_backoff_max_ms = 5U;
        config.now_ms = cluster_test_now;
        config.now_context = node;
        config.send = cluster_test_send;
        config.send_context = node;
        TEST_ASSERT(ucn_cluster_init(&node->cluster, &config) == UCN_OK);
    }
    TEST_ASSERT(cluster_test_sync_neighbors(network) == 0);
    return 0;
}

/* C07.7 P1: a replayed BACKUP_READY from an older epoch (stale
 * membership_sequence or generation) must be rejected so the Head never
 * marks a stale mirror as ready. */
static int cluster_test_backup_ready_fencing(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = network.nodes[1].node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.membership_sequence = 3U;
    head->cluster.backup_ready = false;

    /* Current epoch READY: accepted. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_READY;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.backup_ready == true);

    /* Stale sequence of the same generation: rejected as replay. */
    message.membership_sequence = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);

    /* Stale generation: rejected. */
    head->cluster.backup_ready = false;
    message.membership_sequence = 3U;
    message.backup_generation = 0U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(head->cluster.backup_ready == false);
    return 0;
}

/* C07.7 P1: a replayed PRIMARY_HEARTBEAT from an older generation must
 * not refresh the Backup's liveness. */
static int cluster_test_primary_heartbeat_fencing(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *backup;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    backup = &network.nodes[1];
    backup->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = head->node_id;
    backup->cluster.backup_primary_node_id = head->node_id;
    backup->cluster.backup_generation = 2U;
    backup->cluster.backup_missed_heartbeats = 3U;
    backup->cluster.backup_primary_deadline_ms = 1U;
    backup->cluster.backup_primary_lease_deadline_ms = 1U;
    network.now_ms = 100U;

    /* Stale generation heartbeat: rejected, liveness untouched. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.backup_missed_heartbeats == 3U);

    /* Current generation heartbeat: accepted and refreshes liveness. */
    message.backup_generation = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.backup_missed_heartbeats == 0U);
    return 0;
}

/* C07.7 P1: the Type 12 member nonce and membership sequence round-trip
 * as full 32-bit values (no 16-bit truncation). */
static int cluster_test_member_nonce_32bit(void)
{
    ucn_cluster_message_t input;
    ucn_cluster_message_t output;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    input.role = UCN_CLUSTER_ROLE_HEAD;
    input.cluster_id = 10U;
    input.term = 2U;
    input.head_node_id = 4U;
    input.backup_generation = 3U;
    input.member_node_id = 55U;
    input.membership_sequence = UINT32_C(0x00010003);
    input.member_nonce = UINT32_C(0x00010010);
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.backup_generation == 3U);
    TEST_ASSERT(output.membership_sequence == UINT32_C(0x00010003));
    TEST_ASSERT(output.member_nonce == UINT32_C(0x00010010));
    return 0;
}

/* C07.7 P1: the Backup's own vote counts toward the takeover majority.
 * Mirror = Backup + one member => active=2, majority=2; the Backup
 * self-vote plus the single member ACK must complete the takeover. */
static int cluster_test_takeover_self_vote(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *backup;
    cluster_test_node_t *member;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    backup = &network.nodes[1];
    member = &network.nodes[2];
    backup->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = network.nodes[0].node_id;
    backup->cluster.backup_primary_node_id = network.nodes[0].node_id;
    backup->cluster.backup_generation = 1U;
    backup->cluster.backup_ready = true;
    /* Mirror: Backup itself + one member. */
    backup->cluster.members[0].occupied = true;
    backup->cluster.members[0].node_id = backup->node_id;
    backup->cluster.members[1].occupied = true;
    backup->cluster.members[1].node_id = member->node_id;
    backup->cluster.backup_primary_deadline_ms = 1U;
    backup->cluster.backup_missed_heartbeats = UCN_CLUSTER_BACKUP_MISS_LIMIT;
    backup->cluster.backup_primary_lease_deadline_ms = 1U;
    network.now_ms = 100U;

    /* Start takeover from step(). */
    TEST_ASSERT(ucn_cluster_step(&backup->cluster) == UCN_OK);
    TEST_ASSERT(backup->cluster.backup_takeover_active == true);

    /* The one member ACKs; the Backup's own vote is already counted. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_TAKEOVER_ACK;
    message.role = UCN_CLUSTER_ROLE_MEMBER;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[0].node_id;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, member->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(backup->cluster.term == 2U);
    return 0;
}

/* C07.7 P1: the member's takeover vote is keyed on
 * (cluster_id, term, backup_generation), so a vote cast in Cluster A at
 * term 10 does not block a legitimate takeover in Cluster B at term 10. */
static int cluster_test_takeover_vote_identity(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *member;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    member = &network.nodes[2];
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 2U; /* Cluster B */
    member->cluster.term = 10U;
    member->cluster.head_node_id = network.nodes[0].node_id;
    member->cluster.known_backup_node_id = network.nodes[1].node_id;
    member->cluster.known_backup_generation = 4U;
    /* Simulate an old vote in Cluster A term 10, generation 2. */
    member->cluster.member_voted_term = 10U;
    member->cluster.member_voted_cluster_id = 1U;
    member->cluster.member_voted_generation = 2U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = 2U;
    message.term = 10U;
    message.head_node_id = network.nodes[0].node_id;
    message.backup_generation = 4U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.member_voted_cluster_id == 2U);
    TEST_ASSERT(member->cluster.member_voted_generation == 4U);
    /* A second PREPARE for the same identity is deduplicated. */
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    return 0;
}

/* C07.7 P1: a Backup that observes a same-Cluster, legitimately
 * higher-Term Head during its takeover window must abandon the takeover
 * and join that Head; a different Cluster's term is NOT comparable
 * (Target v2 §8.3) and must not interrupt the takeover. */
static int cluster_test_takeover_interrupted_by_newer_head(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *backup;
    cluster_test_node_t *newer_head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    backup = &network.nodes[1];
    newer_head = &network.nodes[0];
    backup->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = newer_head->node_id;
    backup->cluster.backup_primary_node_id = newer_head->node_id;
    backup->cluster.backup_generation = 1U;
    backup->cluster.backup_takeover_active = true;
    backup->cluster.backup_primary_lease_deadline_ms = 1U;
    network.now_ms = 50U;

    /* A foreign Cluster with a higher term must NOT interrupt takeover. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 2U; /* different Cluster */
    message.term = 3U;       /* higher number, but NOT comparable */
    message.head_node_id = newer_head->node_id;
    message.head_score = 9000U;
    message.available_capacity = 2U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, newer_head->node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.backup_takeover_active == true);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_BACKUP);

    /* The same Cluster at a higher term interrupts and joins. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = newer_head->node_id;
    message.head_score = 9000U;
    message.available_capacity = 2U;
    message.lease_ms = 8000U;
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, newer_head->node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.backup_takeover_active == false);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    return 0;
}

/* C07.7 P1: available_capacity == 0 only gates new JOINs; it must not
 * block a higher-Term Head from converging a full Head onto it. */
static int cluster_test_full_head_term_convergence(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.config.head_score = 7000U;
    head->cluster.current_head_score = 7000U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 2U;
    message.term = 2U; /* newer generation */
    message.head_node_id = network.nodes[1].node_id;
    message.head_score = 6000U; /* lower score but newer term */
    message.available_capacity = 0U; /* full */
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    network.now_ms = 60U;
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, network.nodes[1].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(head->cluster.pending_head_node_id == network.nodes[1].node_id);
    return 0;
}

/* C07.7 P1 (#4): a Type 12 frame of an older Backup generation is
 * replayed; a PRIMARY_HEARTBEAT with a stale membership_sequence of the
 * same generation is also replayed. */
static int cluster_test_backup_epoch_fencing(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *backup;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    backup = &network.nodes[1];
    cluster_fixture_set_role(&backup->cluster, UCN_CLUSTER_ROLE_BACKUP);
    cluster_fixture_set_epoch(&backup->cluster, 1U, 1U, head->node_id);
    cluster_fixture_set_backup(&backup->cluster, head->node_id, 2U, false, false);
    backup->cluster.membership_sequence = 5U;
    network.now_ms = 0U;

    /* A BEGIN frame of the OLD generation is replayed. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN;
    message.membership_sequence = 6U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.membership_sequence == 5U);

    /* A DELTA frame of the OLD generation is replayed. */
    message.flags = UCN_CLUSTER_FLAG_SYNC_DELTA;
    message.member_node_id = network.nodes[2].node_id;
    message.member_nonce = 9U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_REPLAY);

    /* A heartbeat of the current generation but a stale sequence is
     * replayed and cannot refresh liveness. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 2U;
    message.membership_sequence = 4U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    backup->cluster.backup_missed_heartbeats = 3U;
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.backup_missed_heartbeats == 3U);
    /* A heartbeat at the current sequence refreshes liveness. */
    message.membership_sequence = 5U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.backup_missed_heartbeats == 0U);
    return 0;
}

/* C07.7 P1 (#10): a DELTA sequence gap must mark the mirror not-ready and
 * request a full resync instead of silently skipping the lost member. */
static int cluster_test_delta_gap_resync(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *backup;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    backup = &network.nodes[1];
    backup->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = head->node_id;
    backup->cluster.backup_primary_node_id = head->node_id;
    backup->cluster.backup_generation = 1U;
    backup->cluster.membership_sequence = 10U;
    backup->cluster.backup_ready = true;
    network.now_ms = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.flags = UCN_CLUSTER_FLAG_SYNC_DELTA;
    message.membership_sequence = 12U; /* gap: expected 11 */
    message.member_node_id = network.nodes[2].node_id;
    message.member_nonce = 42U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_RESYNC_REQ pending */
    /* Deliver the resync request: the Head restarts the snapshot. */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = backup->node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_sync_cursor = 5U;
    TEST_ASSERT(cluster_test_deliver(&network) == 0);
    TEST_ASSERT(head->cluster.backup_sync_cursor == 0U);
    return 0;
}

/* C07.7 P1 (#7): a BACKUP_REJECT makes the Head cool the candidate down
 * and immediately pick the next one. */
static int cluster_test_backup_reject_switches_candidate(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *rejected;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    rejected = &network.nodes[1];
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = rejected->node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    /* Mirror: node2 (rejected, score 3000) and node3 (score 2000). */
    head->cluster.members[0].occupied = true;
    head->cluster.members[0].node_id = rejected->node_id;
    head->cluster.members[1].occupied = true;
    head->cluster.members[1].node_id = network.nodes[2].node_id;
    /* Both are head-capable candidates with advertisements observed. */
    head->cluster.candidates[0].occupied = true;
    head->cluster.candidates[0].head_node_id = rejected->node_id;
    head->cluster.candidates[0].head_score = 3000U;
    head->cluster.candidates[1].occupied = true;
    head->cluster.candidates[1].head_node_id = network.nodes[2].node_id;
    head->cluster.candidates[1].head_score = 2000U;
    network.now_ms = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_REJECT;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.reject_reason = UCN_CLUSTER_BACKUP_REJECT_COVERAGE;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&head->cluster, rejected->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(head->cluster.backup_node_id == network.nodes[2].node_id);
    TEST_ASSERT(head->cluster.backup_rejected_node_id == rejected->node_id);
    TEST_ASSERT(head->cluster.backup_candidate_cooldown_until_ms != 0U);
    return 0;
}

/* C07.7 P1 (#12): a JOIN_REJECT of an old join txid is ignored; a
 * replayed HEAD_STEPDOWN nonce is ignored after the first application. */
static int cluster_test_join_txid_and_stepdown_nonce(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    node = &network.nodes[2];
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.pending_head_node_id = network.nodes[0].node_id;
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 5U;
    node->cluster.pending_join_nonce = 9U;
    network.now_ms = 0U;

    /* A reject of an old txid cannot abort the current join. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_REJECT;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.lease_ms = 8000U;
    message.nonce = 8U; /* stale txid */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_ERR_ACCESS);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    /* The current txid reject detaches. */
    message.nonce = 9U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);

    /* STEPDOWN nonce replay guard on a member. */
    node = &network.nodes[1];
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 1U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[0].node_id;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    /* Replaying the same nonce after re-joining the same epoch is
     * ignored (the node is DETACHED now, and the nonce is not newer). */
    node->cluster.last_stepdown_nonce = 1U;
    node->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_ERR_ACCESS);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    return 0;
}

/* CLV2-M00-03: deterministic golden trace over the canonical lifecycle
 * (election -> join -> backup -> failover takeover).  Compares byte-wise
 * against tests/golden/cluster_golden_trace.txt; the file is generated on
 * first run and must be committed.  Later refactors (M01) must keep this
 * trace identical. */
static int cluster_test_golden_trace(void)
{
    cluster_test_network_t network;
    cluster_trace_snapshot_t before[CLUSTER_TEST_NODES];
    cluster_trace_snapshot_t after[CLUSTER_TEST_NODES];
    char golden_path[512];
    char trace_path[512];
    FILE *golden = NULL;
    FILE *trace = NULL;
    uint32_t now_ms;
    size_t index;
    bool first_run = false;
    int line = 0;
    char expected[512];
    char actual[512];

#ifdef UCN_CLUSTER_GOLDEN_DIR
    (void)snprintf(golden_path, sizeof(golden_path),
                   UCN_CLUSTER_GOLDEN_DIR "/cluster_golden_trace.txt");
    (void)snprintf(trace_path, sizeof(trace_path),
                   UCN_CLUSTER_GOLDEN_DIR "/cluster_golden_trace_actual.txt");
#else
    (void)snprintf(golden_path, sizeof(golden_path),
                   "/tmp/cluster_golden_trace.txt");
    (void)snprintf(trace_path, sizeof(trace_path),
                   "/tmp/cluster_golden_trace_actual.txt");
#endif
    golden = fopen(golden_path, "r");
    if (golden == NULL) {
        first_run = true;
        golden = fopen(golden_path, "w");
    }
    TEST_ASSERT(golden != NULL);
    trace = fopen(trace_path, "w");
    TEST_ASSERT(trace != NULL);

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        cluster_trace_capture(&network, before);
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
        cluster_trace_capture(&network, after);
        for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
            if (!cluster_trace_snapshot_equal(&before[index], &after[index])) {
                cluster_trace_write_line(trace, &after[index], "CHG");
            }
        }
    }
    /* Primary dies: failover takeover completes on node 1. */
    network.nodes[0].alive = false;
    for (now_ms = 141U; now_ms <= 300U; ++now_ms) {
        cluster_trace_capture(&network, before);
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
        cluster_trace_capture(&network, after);
        for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
            if (!cluster_trace_snapshot_equal(&before[index], &after[index])) {
                cluster_trace_write_line(trace, &after[index], "CHG");
            }
        }
    }
    (void)fclose(trace);

    if (first_run) {
        (void)fclose(golden);
        (void)rename(trace_path, golden_path);
        fprintf(stderr,
                "CLV2-M00-03: golden trace generated at %s; commit it.\n",
                golden_path);
        return 0;
    }
    /* Byte-wise comparison of the actual trace against the golden file. */
    trace = fopen(trace_path, "r");
    TEST_ASSERT(trace != NULL);
    while (fgets(actual, sizeof(actual), trace) != NULL) {
        if (fgets(expected, sizeof(expected), golden) == NULL) {
            (void)fclose(trace);
            (void)fclose(golden);
            fprintf(stderr, "CLV2-M00-03: trace longer than golden at line %d\n",
                    line);
            return -1;
        }
        line++;
        if (strcmp(actual, expected) != 0) {
            (void)fclose(trace);
            (void)fclose(golden);
            fprintf(stderr,
                    "CLV2-M00-03: trace mismatch at line %d\n  actual:   %s"
                    "  expected: %s", line, actual, expected);
            return -1;
        }
    }
    if (fgets(expected, sizeof(expected), golden) != NULL) {
        (void)fclose(trace);
        (void)fclose(golden);
        fprintf(stderr, "CLV2-M00-03: golden longer than trace at line %d\n",
                line);
        return -1;
    }
    (void)fclose(trace);
    (void)fclose(golden);
    return 0;
}

/* CLV2-M00-07: majority-side Backup takeover under a partition.
 * Partition {H} | {B,M1,M2}: the Backup side holds 3/4 and must
 * complete a majority takeover; this records the current behaviour
 * (M08 will additionally fence the isolated old Head). */
static int cluster_test_fault_partition_takeover(void)
{
    cluster_test_network_t network;
    cluster_test_fault_t fault;
    cluster_test_node_t *backup;
    uint32_t now_ms;
    size_t i;
    size_t j;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    backup = NULL;
    for (i = 1U; i < CLUSTER_TEST_NODES; ++i) {
        if (network.nodes[i].cluster.role == UCN_CLUSTER_ROLE_BACKUP) {
            backup = &network.nodes[i];
            break;
        }
    }
    TEST_ASSERT(backup != NULL);

    (void)memset(&fault, 0, sizeof(fault));
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
            fault.partition[i][j] = true;
        }
    }
    /* Isolate node 0 (Head) from everyone else. */
    for (j = 1U; j < CLUSTER_TEST_NODES; ++j) {
        fault.partition[0][j] = false;
        fault.partition[j][0] = false;
    }

    for (now_ms = 141U; now_ms <= 400U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    /* The majority side (Backup + two members) completes takeover. */
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(backup->cluster.cluster_id == UINT32_C(1));
    TEST_ASSERT(backup->cluster.term >= 2U);
    return 0;
}

/* CLV2-M00-07: a power-cycle restart resets the node to DETACHED with a
 * fresh term (current behaviour, no persistence yet; M04 changes this). */
static int cluster_test_fault_restart_no_old_term(void)
{
    cluster_test_network_t network;
    cluster_test_fault_t fault;
    uint32_t now_ms;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 80U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(network.nodes[0].cluster.term == 1U);

    (void)memset(&fault, 0, sizeof(fault));
    fault.restart_node_id = network.nodes[0].node_id;
    fault.restart_at_ms = 81U;
    for (now_ms = 81U; now_ms <= 90U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    TEST_ASSERT(fault.restart_done == true);
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(network.nodes[0].cluster.term == 0U);
    TEST_ASSERT(network.nodes[0].cluster.cluster_id == 0U);
    return 0;
}

/* CLV2-M00-07: bounded delivery (deliver_budget) simulates a slow/lossy
 * link; the snapshot eventually completes because the Head retransmits.
 * Then a deterministic drop_one_in on the live DELTA phase triggers a
 * gap -> RESYNC_REQ -> full snapshot that eventually converges again. */
static int cluster_test_fault_drop_eventually_converges(void)
{
    cluster_test_network_t network;
    cluster_test_fault_t fault;
    uint32_t now_ms;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    (void)memset(&fault, 0, sizeof(fault));
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    {
        size_t i;
        bool found_backup_ready = false;

        for (i = 1U; i < CLUSTER_TEST_NODES; ++i) {
            if (network.nodes[i].cluster.role == UCN_CLUSTER_ROLE_BACKUP &&
                network.nodes[i].cluster.backup_ready) {
                found_backup_ready = true;
                break;
            }
        }
        TEST_ASSERT(found_backup_ready);
    }
    /* Drop 1 in 7 frames: forces DELTA gaps -> RESYNC_REQ -> full
     * snapshot rounds; the bounded retransmit must keep the system
     * eventually consistent (backup converges back to READY). */
    fault.drop_one_in = 7U;
    for (now_ms = 141U; now_ms <= 800U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    {
        size_t i;
        bool found_backup_ready = false;

        for (i = 1U; i < CLUSTER_TEST_NODES; ++i) {
            if (network.nodes[i].cluster.role == UCN_CLUSTER_ROLE_BACKUP &&
                network.nodes[i].cluster.backup_ready) {
                found_backup_ready = true;
                break;
            }
        }
        TEST_ASSERT(found_backup_ready);
    }
    return 0;
}

static int cluster_test_timing_profiles(void)
{
    ucn_cluster_config_t config;
    cluster_test_network_t network;
    cluster_test_node_t node;

    (void)memset(&config, 0, sizeof(config));
    TEST_ASSERT(ucn_cluster_config_apply_timing_profile(
                    &config, UCN_CLUSTER_TIMING_PROFILE_DEFAULT) == UCN_OK);
    TEST_ASSERT(config.observation_ms == UCN_CLUSTER_OBSERVATION_MS &&
                config.recovery_observation_ms ==
                    UCN_CLUSTER_RECOVERY_OBSERVATION_MS &&
                config.election_window_ms == UCN_CLUSTER_ELECTION_WINDOW_MS &&
                config.advertise_interval_ms ==
                    UCN_CLUSTER_ADVERTISE_INTERVAL_MS &&
                config.join_retry_ms == UCN_CLUSTER_JOIN_RETRY_MS &&
                config.keepalive_interval_ms ==
                    UCN_CLUSTER_KEEPALIVE_INTERVAL_MS &&
                config.lease_ms == UCN_CLUSTER_LEASE_MS &&
                config.head_min_tenure_ms == UCN_CLUSTER_HEAD_MIN_TENURE_MS);
    TEST_ASSERT(ucn_cluster_config_apply_timing_profile(
                    &config, UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED) == UCN_OK);
    TEST_ASSERT(config.observation_ms == UCN_CLUSTER_FAST_OBSERVATION_MS &&
                config.recovery_observation_ms ==
                    UCN_CLUSTER_FAST_RECOVERY_OBSERVATION_MS &&
                config.election_window_ms ==
                    UCN_CLUSTER_FAST_ELECTION_WINDOW_MS &&
                config.advertise_interval_ms ==
                    UCN_CLUSTER_FAST_ADVERTISE_INTERVAL_MS &&
                config.join_retry_ms == UCN_CLUSTER_FAST_JOIN_RETRY_MS &&
                config.keepalive_interval_ms ==
                    UCN_CLUSTER_FAST_KEEPALIVE_INTERVAL_MS &&
                config.lease_ms == UCN_CLUSTER_FAST_LEASE_MS &&
                config.head_min_tenure_ms ==
                    UCN_CLUSTER_FAST_HEAD_MIN_TENURE_MS);
    TEST_ASSERT(ucn_cluster_config_apply_timing_profile(
                    NULL, UCN_CLUSTER_TIMING_PROFILE_DEFAULT) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_cluster_config_apply_timing_profile(
                    &config, (ucn_cluster_timing_profile_t)99) ==
                UCN_ERR_ARGUMENT);

    (void)memset(&network, 0, sizeof(network));
    (void)memset(&node, 0, sizeof(node));
    node.network = &network;
    node.node_id = UINT32_C(1);
    node.alive = true;
    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = node.node_id;
    config.enabled = true;
    config.head_capable = true;
    config.head_score = 9000U;
    config.member_capacity = 1U;
    config.now_ms = cluster_test_now;
    config.now_context = &node;
    config.send = cluster_test_send;
    config.send_context = &node;
    TEST_ASSERT(ucn_cluster_config_apply_timing_profile(
                    &config, UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED) == UCN_OK);
    /* A Head cannot mirror/vote for more live members than its admitted
     * peer table can represent, even when the fixed member array is larger. */
    config.member_capacity = (uint16_t)(UCN_CLUSTER_MAX_PEERS + 1U);
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_ERR_ARGUMENT);
    config.member_capacity = 1U;
    config.keepalive_interval_ms = config.lease_ms;
    TEST_ASSERT(ucn_cluster_init(&node.cluster, &config) == UCN_ERR_CONFIG);
    return 0;
}

static int cluster_test_codec_and_security(void)
{
    ucn_cluster_message_t input;
    ucn_cluster_message_t output;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    cluster_test_network_t network;

    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_ADVERTISE;
    input.role = UCN_CLUSTER_ROLE_HEAD;
    input.cluster_id = UINT32_C(0x10203040);
    input.term = UINT32_C(7);
    input.head_node_id = UINT32_C(3);
    input.head_score = 8123U;
    input.available_capacity = 12U;
    input.lease_ms = 8000U;
    input.nonce = UINT32_C(99);
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.type == input.type && output.role == input.role &&
                output.cluster_id == input.cluster_id && output.term == input.term &&
                output.head_node_id == input.head_node_id &&
                output.head_score == input.head_score &&
                output.available_capacity == input.available_capacity &&
                output.lease_ms == input.lease_ms && output.nonce == input.nonce);
    encoded[0] = 0U;
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_ERR_MALFORMED);

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&network.nodes[0].cluster, UINT32_C(3), false,
                                    encoded, sizeof(encoded)) == UCN_ERR_SECURITY);
    TEST_ASSERT(network.nodes[0].cluster.stats.security_rejected == 1U);
    TEST_ASSERT(ucn_cluster_set_head_score(&network.nodes[0].cluster,
                                           UCN_CLUSTER_SCORE_MAX + 1U) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_cluster_set_head_score(&network.nodes[0].cluster, 8500U) ==
                UCN_OK);
    TEST_ASSERT(network.nodes[0].cluster.config.head_score == 8500U);
    return 0;
}

static int cluster_test_v3_codec(void)
{
    ucn_cluster_message_t input;
    ucn_cluster_message_t output;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    /* Type 10 BACKUP_ASSIGN: backup_generation + sync_token. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    input.role = UCN_CLUSTER_ROLE_HEAD;
    input.cluster_id = UINT32_C(0xA1B2C3D4);
    input.term = 5U;
    input.head_node_id = 7U;
    input.backup_generation = 3U;
    input.sync_token = UINT32_C(0xDEADBEEF);
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(encoded[0] == UCN_CLUSTER_FORMAT_VERSION);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.type == UCN_CLUSTER_MSG_BACKUP_ASSIGN &&
                output.backup_generation == 3U &&
                output.sync_token == UINT32_C(0xDEADBEEF));

    /* Type 11 BACKUP_READY: membership_sequence. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_BACKUP_READY;
    input.role = UCN_CLUSTER_ROLE_BACKUP;
    input.cluster_id = 9U;
    input.term = 2U;
    input.head_node_id = 4U;
    input.membership_sequence = 17U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.membership_sequence == 17U);

    /* Type 12 BACKUP_MEMBER_SYNC: member fields + sequence/nonce. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    input.role = UCN_CLUSTER_ROLE_HEAD;
    input.cluster_id = 10U;
    input.term = 2U;
    input.head_node_id = 4U;
    input.backup_generation = 3U;
    input.member_node_id = 55U;
    input.membership_sequence = UINT32_C(0x10003);
    input.member_nonce = UINT32_C(0x1000002A);
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.backup_generation == 3U &&
                output.member_node_id == 55U &&
                output.membership_sequence == UINT32_C(0x10003) &&
                output.member_nonce == UINT32_C(0x1000002A));

    /* Type 12 markers must carry generation and zero member id. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    input.role = UCN_CLUSTER_ROLE_HEAD;
    input.cluster_id = 10U;
    input.term = 2U;
    input.head_node_id = 4U;
    input.backup_generation = 3U;
    input.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN;
    input.membership_sequence = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    /* A marker carrying a member id is malformed. */
    input.member_node_id = 55U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_ERR_ARGUMENT);
    /* BEGIN|END and BEGIN|DELTA combinations are malformed. */
    input.member_node_id = 0U;
    input.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN | UCN_CLUSTER_FLAG_SYNC_END;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_ERR_ARGUMENT);
    input.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN | UCN_CLUSTER_FLAG_SYNC_DELTA;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_ERR_ARGUMENT);
    /* A non-Head sender is malformed. */
    input.flags = 0U;
    input.role = UCN_CLUSTER_ROLE_BACKUP;
    input.member_node_id = 55U;
    input.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_ERR_ARGUMENT);

    /* Type 13 PRIMARY_HEARTBEAT. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT;
    input.role = UCN_CLUSTER_ROLE_HEAD;
    input.cluster_id = 11U;
    input.term = 2U;
    input.head_node_id = 4U;
    input.membership_sequence = 99U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.membership_sequence == 99U);

    /* Type 14 TAKEOVER_PREPARE / Type 15 TAKEOVER_ACK. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
    input.role = UCN_CLUSTER_ROLE_BACKUP;
    input.cluster_id = 12U;
    input.term = 2U;
    input.head_node_id = 4U;
    input.backup_generation = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.backup_generation == 7U);
    input.type = UCN_CLUSTER_MSG_TAKEOVER_ACK;
    input.role = UCN_CLUSTER_ROLE_MEMBER;
    input.head_node_id = 4U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.type == UCN_CLUSTER_MSG_TAKEOVER_ACK &&
                output.backup_generation == 7U);

    /* Type 16 RECOVERY_DECLARE: recovery_nonce + recovery_ttl_ms. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_RECOVERY_DECLARE;
    input.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    input.cluster_id = 13U;
    input.term = 2U;
    input.head_node_id = 4U;
    input.recovery_nonce = 12345U;
    input.recovery_ttl_ms = 30000U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.recovery_nonce == 12345U &&
                output.recovery_ttl_ms == 30000U);

    /* Type 17 RECOVERY_ACK: all-zero trailing body. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_RECOVERY_ACK;
    input.role = UCN_CLUSTER_ROLE_MEMBER;
    input.cluster_id = 13U;
    input.term = 2U;
    input.head_node_id = 4U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_OK);
    TEST_ASSERT(output.type == UCN_CLUSTER_MSG_RECOVERY_ACK);

    /* v1/v2 frames are rejected under Format v3. */
    (void)memset(&input, 0, sizeof(input));
    input.type = UCN_CLUSTER_MSG_ADVERTISE;
    input.role = UCN_CLUSTER_ROLE_HEAD;
    input.cluster_id = 1U;
    input.term = 1U;
    input.head_node_id = 2U;
    input.head_score = 100U;
    input.lease_ms = 8000U;
    input.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&input, encoded) == UCN_OK);
    encoded[0] = 1U; /* Format v1 byte. */
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_ERR_MALFORMED);
    encoded[0] = 2U; /* Format v2 byte. */
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_ERR_MALFORMED);

    /* Unknown message type and invalid role are rejected. */
    encoded[0] = UCN_CLUSTER_FORMAT_VERSION;
    encoded[1] = 18U; /* beyond RECOVERY_ACK */
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_ERR_MALFORMED);
    encoded[1] = (uint8_t)UCN_CLUSTER_MSG_ADVERTISE;
    encoded[2] = 9U; /* beyond RECOVERY_HEAD */
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded), &output) ==
                UCN_ERR_MALFORMED);

    /* Wrong length is rejected. */
    encoded[0] = UCN_CLUSTER_FORMAT_VERSION;
    encoded[1] = (uint8_t)UCN_CLUSTER_MSG_ADVERTISE;
    TEST_ASSERT(ucn_cluster_message_decode(encoded, sizeof(encoded) - 1U,
                                           &output) == UCN_ERR_MALFORMED);
    return 0;
}

static int cluster_test_backup_sync(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *backup;
    cluster_test_node_t *third;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    backup = &network.nodes[1];
    third = &network.nodes[2];

    /* Manually stage roles: head=HEAD, others=MEMBER, fully-connected. */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.backup_node_id = backup->node_id;
    head->cluster.backup_generation = 1U;
    head->cluster.backup_ready = false;
    head->cluster.backup_sync_cursor = 0U;
    backup->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = head->node_id;
    backup->cluster.config.head_capable = true;
    third->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    third->cluster.cluster_id = 1U;
    third->cluster.term = 1U;
    third->cluster.head_node_id = head->node_id;
    third->cluster.config.head_capable = false;

    /* BACKUP_ASSIGN: member -> BACKUP syncing. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.sync_token = backup->node_id;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    network.now_ms = 0U;
    TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(backup->cluster.backup_syncing == true);

    /* Snapshot BEGIN. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN;
    message.backup_generation = 1U;
    message.membership_sequence = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);

    /* Member record: third (reached by backup in fully-connected topology). */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 2U;
    message.member_node_id = third->node_id;
    message.member_lease_ms = 8000U;
    message.member_nonce = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);

    /* Snapshot END: coverage passes -> READY -> BACKUP_READY sent to head. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.flags = UCN_CLUSTER_FLAG_SYNC_END;
    message.backup_generation = 1U;
    message.membership_sequence = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.backup_ready == true);
    TEST_ASSERT(backup->cluster.backup_syncing == false);
    TEST_ASSERT(network.queue_count == 1U); /* BACKUP_READY pending. */

    /* The Head's own snapshot sequence reached 3 (END sent); the READY
     * must match this exact epoch (generation 1, sequence 3). */
    head->cluster.membership_sequence = 3U;
    /* Deliver BACKUP_READY to head. */
    TEST_ASSERT(cluster_test_deliver(&network) == 0);
    TEST_ASSERT(head->cluster.backup_ready == true);

    /* Missing sequence -> replay -> backup stays syncing and awaits the
     * bounded snapshot retransmit (no detach on a dropped frame). */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.backup_generation = 1U;
    message.membership_sequence = 9U; /* gap: expected 4 */
    message.member_node_id = third->node_id;
    message.member_lease_ms = 8000U;
    message.member_nonce = 8U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(backup->cluster.backup_syncing == true);
    TEST_ASSERT(backup->cluster.backup_ready == false);
    return 0;
}


static int cluster_test_takeover_guard(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *member;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    member = &network.nodes[2];
    member->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    member->cluster.cluster_id = 1U;
    member->cluster.term = 5U;
    member->cluster.head_node_id = UINT32_C(1);
    member->cluster.known_backup_node_id = UINT32_C(2);
    member->cluster.known_backup_generation = 7U;
    network.now_ms = 0U;
    TEST_ASSERT(cluster_test_sync_neighbors(&network) == 0);

    /* A stale (same-or-lower term) HEAD_TAKEOVER is rejected. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = UINT32_C(2);
    message.head_score = 5000U;
    message.lease_ms = 8000U;
    message.backup_generation = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, UINT32_C(2), true,
                                    encoded, sizeof(encoded)) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(member->cluster.head_node_id == UINT32_C(1));

    /* A protected but non-Backup member may not solicit a vote or promotion. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_TAKEOVER_PREPARE;
    message.role = UCN_CLUSTER_ROLE_BACKUP;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = UINT32_C(1);
    message.backup_generation = 7U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, UINT32_C(1), true,
                                    encoded, sizeof(encoded)) == UCN_ERR_ACCESS);
    TEST_ASSERT(member->cluster.member_voted_term == 0U);

    /* A higher-term takeover switches the member without rejoin. */
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.term = 6U;
    message.head_node_id = UINT32_C(2);
    message.head_score = 5000U;
    message.lease_ms = 8000U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&member->cluster, UINT32_C(2), true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(member->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(member->cluster.head_node_id == UINT32_C(2));
    TEST_ASSERT(member->cluster.term == 6U);
    return 0;
}

static int cluster_test_election_join_and_failover(void)
{
    cluster_test_network_t network;
    uint32_t now_ms;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    if (ucn_cluster_member_count(&network.nodes[0].cluster) != 3U) {
        printf("CLUSTER_DEBUG head_role=%u members=%zu roles=%u/%u/%u/%u "
               "accepted=%lu rejected=%lu sent=%lu received=%lu\n",
               (unsigned)network.nodes[0].cluster.role,
               ucn_cluster_member_count(&network.nodes[0].cluster),
               (unsigned)network.nodes[0].cluster.role,
               (unsigned)network.nodes[1].cluster.role,
               (unsigned)network.nodes[2].cluster.role,
               (unsigned)network.nodes[3].cluster.role,
               (unsigned long)network.nodes[0].cluster.stats.joins_accepted,
               (unsigned long)network.nodes[0].cluster.stats.joins_rejected,
               (unsigned long)network.nodes[0].cluster.stats.messages_sent,
               (unsigned long)network.nodes[0].cluster.stats.messages_received);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(network.nodes[0].cluster.cluster_id == UINT32_C(1));
    TEST_ASSERT(ucn_cluster_member_count(&network.nodes[0].cluster) == 3U);
    TEST_ASSERT((network.nodes[1].cluster.role == UCN_CLUSTER_ROLE_MEMBER ||
                 network.nodes[1].cluster.role == UCN_CLUSTER_ROLE_BACKUP) &&
                network.nodes[1].cluster.head_node_id == UINT32_C(1));
    TEST_ASSERT(network.nodes[2].cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
                network.nodes[3].cluster.role == UCN_CLUSTER_ROLE_MEMBER);

    network.nodes[0].alive = false;
    for (now_ms = 141U; now_ms <= 300U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[1].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    /* Takeover keeps the cluster identity; only the Term advances. */
    TEST_ASSERT(network.nodes[1].cluster.cluster_id == UINT32_C(1));
    TEST_ASSERT(network.nodes[1].cluster.term == 2U);
    TEST_ASSERT(ucn_cluster_member_count(&network.nodes[1].cluster) == 2U);
    TEST_ASSERT(network.nodes[2].cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
                network.nodes[2].cluster.head_node_id == UINT32_C(2));
    TEST_ASSERT(network.nodes[3].cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
                network.nodes[3].cluster.head_node_id == UINT32_C(2));
    TEST_ASSERT(network.nodes[1].cluster.stats.elections_won == 1U);
    return 0;
}

static int cluster_test_capacity_is_bounded(void)
{
    cluster_test_network_t network;
    ucn_cluster_message_t request;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t now_ms;
    size_t joined = 0U;
    size_t nonmember_index = 0U;
    size_t index;

    TEST_ASSERT(cluster_test_network_init(&network, 1U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(ucn_cluster_member_count(&network.nodes[0].cluster) == 1U);
    for (index = 1U; index < CLUSTER_TEST_NODES; ++index) {
        if (network.nodes[index].cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
            network.nodes[index].cluster.head_node_id == UINT32_C(1)) {
            ++joined;
        } else {
            nonmember_index = index;
        }
    }
    TEST_ASSERT(joined == 1U);
    TEST_ASSERT(nonmember_index != 0U);
    (void)memset(&request, 0, sizeof(request));
    request.type = UCN_CLUSTER_MSG_JOIN_REQUEST;
    request.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    request.cluster_id = network.nodes[0].cluster.cluster_id;
    request.term = network.nodes[0].cluster.term;
    request.head_node_id = network.nodes[0].node_id;
    request.head_score = network.nodes[0].cluster.config.head_score;
    request.lease_ms = network.nodes[0].cluster.config.lease_ms;
    request.nonce = UINT32_C(50000);
    TEST_ASSERT(ucn_cluster_message_encode(&request, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(
                    &network.nodes[0].cluster,
                    network.nodes[nonmember_index].node_id, true,
                    encoded, sizeof(encoded)) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(network.nodes[0].cluster.stats.joins_rejected > 0U);
    return 0;
}

static int cluster_test_neighbor_summary_api(void)
{
#if UCN_FEATURE_DYNAMIC_MESH
    ucn_node_t node;
    ucn_config_t config = { UINT32_C(1), UINT32_C(1), 4U };
    ucn_link_t link;
    ucn_neighbor_summary_t summary;

    (void)memset(&link, 0, sizeof(link));
    link.ops = &CLUSTER_SUMMARY_LINK_OPS;
    link.link_id = 1U;
    link.mtu = UCN_MAX_FRAME_BYTES;
    link.peer_node_id = UINT32_C(2);
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_observe_neighbor(&node, &link, UINT32_C(123)) == UCN_OK);
    TEST_ASSERT(ucn_node_admit_neighbor(&node, UINT32_C(2)) == UCN_OK);
    TEST_ASSERT(ucn_node_copy_neighbor_summaries(&node, NULL, 0U) == 1U);
    TEST_ASSERT(ucn_node_copy_neighbor_summaries(&node, &summary, 1U) == 1U);
    TEST_ASSERT(summary.state == UCN_NEIGHBOR_ADMITTED &&
                summary.peer_node_id == UINT32_C(2) &&
                summary.bearer_count == 1U && summary.last_seen_ms == 123U);
#else
    ucn_node_t node;
    ucn_neighbor_summary_t summary;

    (void)memset(&node, 0, sizeof(node));
    TEST_ASSERT(ucn_node_copy_neighbor_summaries(&node, &summary, 1U) == 0U);
#endif
    return 0;
}

static int cluster_test_recovery_head(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *candidate;
    cluster_test_node_t *peer;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t now_ms;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    /* Primary and Backup are both gone; only the head-capable node 3 and
     * member node 4 survive as a headless domain. */
    network.nodes[0].alive = false;
    network.nodes[1].alive = false;
    candidate = &network.nodes[2];
    peer = &network.nodes[3];
    candidate->cluster.config.head_capable = true;
    candidate->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    candidate->cluster.cluster_id = 1U;
    candidate->cluster.term = 1U;
    candidate->cluster.head_node_id = UINT32_C(1);
    candidate->cluster.head_lease_expires_at_ms = 1U;
    peer->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    peer->cluster.cluster_id = 1U;
    peer->cluster.term = 1U;
    peer->cluster.head_node_id = UINT32_C(1);
    peer->cluster.head_lease_expires_at_ms = 1U;

    /* Headless: lease grace (1 keepalive) + detach observation + backoff
     * precede the Recovery Head declaration. */
    for (now_ms = 0U; now_ms <= 40U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(candidate->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(candidate->cluster.cluster_id == candidate->node_id);
    TEST_ASSERT(candidate->cluster.term == 1U);
    TEST_ASSERT(candidate->cluster.head_node_id == candidate->node_id);
    TEST_ASSERT(candidate->cluster.recovery_ack_count >= 1U);

    /* TTL expiry steps the Recovery Head down immediately. */
    candidate->cluster.recovery_deadline_ms = 1U;
    now_ms = 41U;
    TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    TEST_ASSERT(candidate->cluster.role == UCN_CLUSTER_ROLE_DETACHED);

    /* The still-headless domain re-backs off, then a stable higher-Term
     * HEAD_TAKEOVER makes the re-declared Recovery Head defer. */
    candidate->cluster.recovery_cooldown_until_ms = 0U;
    for (now_ms = 42U; now_ms <= 160U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(candidate->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_TAKEOVER;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 2U;
    message.head_node_id = peer->node_id;
    message.head_score = 9000U;
    message.lease_ms = 8000U;
    message.backup_generation = 1U;
    candidate->cluster.known_backup_node_id = peer->node_id;
    candidate->cluster.known_backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&candidate->cluster, peer->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(candidate->cluster.role == UCN_CLUSTER_ROLE_MEMBER &&
                candidate->cluster.head_node_id == peer->node_id);
    return 0;
}

/* C07.7 P0-1: RECOVERY_DECLARE must actually re-form a working cluster.
 * The surviving headless member must switch to MEMBER of the recovery
 * Cluster (cluster_id/head_node_id = declaring node) and the Recovery
 * Head must track it as a member. */
static int cluster_test_recovery_forms_cluster(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *candidate;
    cluster_test_node_t *peer;
    uint32_t now_ms;
    size_t index;
    bool found = false;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.nodes[0].alive = false; /* Primary gone */
    network.nodes[1].alive = false; /* Backup gone */
    candidate = &network.nodes[2];
    peer = &network.nodes[3];
    candidate->cluster.config.head_capable = true;
    candidate->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    candidate->cluster.cluster_id = 1U;
    candidate->cluster.term = 1U;
    candidate->cluster.head_node_id = UINT32_C(1);
    candidate->cluster.head_lease_expires_at_ms = 1U;
    peer->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    peer->cluster.cluster_id = 1U;
    peer->cluster.term = 1U;
    peer->cluster.head_node_id = UINT32_C(1);
    peer->cluster.head_lease_expires_at_ms = 1U;

    for (now_ms = 0U; now_ms <= 40U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(candidate->cluster.role == UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    /* The surviving member joins the recovery Cluster. */
    TEST_ASSERT(peer->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(peer->cluster.cluster_id == candidate->node_id);
    TEST_ASSERT(peer->cluster.head_node_id == candidate->node_id);
    TEST_ASSERT(peer->cluster.term == 1U);
    /* The Recovery Head tracks the member. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (candidate->cluster.members[index].occupied &&
            candidate->cluster.members[index].node_id == peer->node_id) {
            found = true;
            break;
        }
    }
    TEST_ASSERT(found);
    /* Lease keeps the member attached across the Recovery Head's
     * periodic advertisement. */
    for (now_ms = 41U; now_ms <= 80U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(peer->cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(peer->cluster.head_node_id == candidate->node_id);
    return 0;
}

/* C07.7 P0-2: a completely isolated node (zero visible ADMITTED peers)
 * must NOT self-declare a Recovery Head. */
static int cluster_test_recovery_requires_peers(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *solo;
    uint32_t now_ms;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.nodes[1].alive = false;
    network.nodes[2].alive = false;
    network.nodes[3].alive = false;
    solo = &network.nodes[0];
    solo->cluster.config.head_capable = true;
    solo->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    solo->cluster.cluster_id = 1U;
    solo->cluster.term = 1U;
    /* A dead foreign Head (not this node): the Member must never send a
     * Keepalive to its own node ID. */
    solo->cluster.head_node_id = UINT32_C(2);
    solo->cluster.head_lease_expires_at_ms = 1U;

    for (now_ms = 0U; now_ms <= 300U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
        TEST_ASSERT(solo->cluster.role != UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    }
    return 0;
}

/* C07.7 P0-3: two recovery candidates converge to exactly one Recovery
 * Head via deterministic (recovery_nonce, node-id) arbitration; the loser
 * joins the winner instead of declaring a second Head, and must not
 * clobber its own recovery_nonce with the remote value. */
static int cluster_test_recovery_conflict_resolved(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *a;
    cluster_test_node_t *b;
    uint32_t now_ms;
    size_t index;
    unsigned recovery_heads = 0U;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.nodes[0].alive = false;
    network.nodes[1].alive = false;
    a = &network.nodes[2];
    b = &network.nodes[3];
    a->cluster.config.head_capable = true;
    b->cluster.config.head_capable = true;
    a->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    a->cluster.cluster_id = 1U;
    a->cluster.term = 1U;
    a->cluster.head_node_id = UINT32_C(1);
    a->cluster.head_lease_expires_at_ms = 1U;
    b->cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    b->cluster.cluster_id = 1U;
    b->cluster.term = 1U;
    b->cluster.head_node_id = UINT32_C(1);
    b->cluster.head_lease_expires_at_ms = 1U;

    for (now_ms = 0U; now_ms <= 120U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        if (network.nodes[index].cluster.role ==
            UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
            recovery_heads++;
        }
    }
    TEST_ASSERT(recovery_heads == 1U);
    /* The loser joined the winner. */
    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        if (network.nodes[index].cluster.role ==
            UCN_CLUSTER_ROLE_RECOVERY_HEAD) {
            const ucn_node_id_t winner = network.nodes[index].node_id;
            size_t other;

            for (other = 0U; other < CLUSTER_TEST_NODES; ++other) {
                if (other != index && network.nodes[other].alive) {
                    TEST_ASSERT(network.nodes[other].cluster.role ==
                                UCN_CLUSTER_ROLE_MEMBER);
                    TEST_ASSERT(network.nodes[other].cluster.head_node_id ==
                                winner);
                }
            }
        }
    }
    (void)a;
    (void)b;
    return 0;
}

static int cluster_test_stable_switchback(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t now_ms;
    uint32_t nonce = 0U;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];

    /* node 0: an established low-score Head with tenure already elapsed. */
    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.config.head_score = 6000U;
    head->cluster.current_head_score = 6000U;
    head->cluster.role_since_ms = 0U;
    head->cluster.stepdown_deadline_ms = 0U;

    /* node 1 advertises as a much better Head (split-brain challenger). */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[1].node_id;
    message.head_score = 9500U;
    message.available_capacity = 3U;
    message.lease_ms = 8000U;

    /* Cross switch_required_samples + head_min_tenure_ms (50 ms). */
    for (now_ms = 0U; now_ms <= 60U; ++now_ms) {
        network.now_ms = now_ms;
        message.nonce = ++nonce;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&head->cluster,
                                        network.nodes[1].node_id, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
        TEST_ASSERT(ucn_cluster_step(&head->cluster) == UCN_OK);
        if (head->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN) {
            break;
        }
    }
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(head->cluster.pending_head_node_id ==
                network.nodes[1].node_id);

    /* After the stepdown deadline the Head joins the better Head. */
    for (++now_ms; now_ms <= 80U; ++now_ms) {
        network.now_ms = now_ms;
        TEST_ASSERT(ucn_cluster_step(&head->cluster) == UCN_OK);
        if (head->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING) {
            break;
        }
    }
    TEST_ASSERT(head->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(head->cluster.pending_head_node_id ==
                network.nodes[1].node_id);
    return 0;
}

static int cluster_test_backup_challenge(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *head;
    cluster_test_node_t *backup;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    head = &network.nodes[0];
    backup = &network.nodes[1];

    head->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    head->cluster.cluster_id = 1U;
    head->cluster.term = 1U;
    head->cluster.head_node_id = head->node_id;
    head->cluster.config.head_score = 6000U;
    head->cluster.current_head_score = 6000U;

    backup->cluster.role = UCN_CLUSTER_ROLE_BACKUP;
    backup->cluster.cluster_id = 1U;
    backup->cluster.term = 1U;
    backup->cluster.head_node_id = head->node_id;
    backup->cluster.backup_primary_node_id = head->node_id;
    backup->cluster.config.head_score = 9500U;
    backup->cluster.role_since_ms = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = head->node_id;
    message.head_score = 6000U;
    message.available_capacity = 0U; /* full Head: challenge must still run */
    message.lease_ms = 8000U;
    message.nonce = 1U;
    network.now_ms = 60U; /* past head_min_tenure_ms=50 */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&backup->cluster, head->node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(backup->cluster.role == UCN_CLUSTER_ROLE_CANDIDATE);
    TEST_ASSERT(backup->cluster.cluster_id == 1U); /* same Cluster */
    TEST_ASSERT(backup->cluster.term == 2U); /* Term bumped */
    TEST_ASSERT(backup->cluster.head_node_id == backup->node_id);
    return 0;
}

int test_cluster(void)
{
    TEST_ASSERT(cluster_test_codec_and_security() == 0);
    TEST_ASSERT(cluster_test_v3_codec() == 0);
    TEST_ASSERT(cluster_test_backup_sync() == 0);
    TEST_ASSERT(cluster_test_backup_ready_fencing() == 0);
    TEST_ASSERT(cluster_test_primary_heartbeat_fencing() == 0);
    TEST_ASSERT(cluster_test_member_nonce_32bit() == 0);
    TEST_ASSERT(cluster_test_golden_trace() == 0);
    TEST_ASSERT(cluster_test_backup_epoch_fencing() == 0);
    TEST_ASSERT(cluster_test_delta_gap_resync() == 0);
    TEST_ASSERT(cluster_test_backup_reject_switches_candidate() == 0);
    TEST_ASSERT(cluster_test_join_txid_and_stepdown_nonce() == 0);
    TEST_ASSERT(cluster_test_fault_partition_takeover() == 0);
    TEST_ASSERT(cluster_test_fault_restart_no_old_term() == 0);
    TEST_ASSERT(cluster_test_fault_drop_eventually_converges() == 0);
    TEST_ASSERT(cluster_test_takeover_guard() == 0);
    TEST_ASSERT(cluster_test_takeover_self_vote() == 0);
    TEST_ASSERT(cluster_test_takeover_vote_identity() == 0);
    TEST_ASSERT(cluster_test_takeover_interrupted_by_newer_head() == 0);
    TEST_ASSERT(cluster_test_full_head_term_convergence() == 0);
    TEST_ASSERT(cluster_test_recovery_head() == 0);
    TEST_ASSERT(cluster_test_recovery_forms_cluster() == 0);
    TEST_ASSERT(cluster_test_recovery_requires_peers() == 0);
    TEST_ASSERT(cluster_test_recovery_conflict_resolved() == 0);
    TEST_ASSERT(cluster_test_stable_switchback() == 0);
    TEST_ASSERT(cluster_test_backup_challenge() == 0);
    TEST_ASSERT(cluster_test_timing_profiles() == 0);
    TEST_ASSERT(cluster_test_election_join_and_failover() == 0);
    TEST_ASSERT(cluster_test_capacity_is_bounded() == 0);
    TEST_ASSERT(cluster_test_neighbor_summary_api() == 0);
    return 0;
}
