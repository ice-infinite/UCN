#include "test_support.h"

#include <stdlib.h>
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
    /* CLV2-01-02 shadow audit bookkeeping (test-only). */
    ucn_cluster_phase_t last_audit_phase;
    bool audit_seen;
} cluster_test_node_t;

typedef struct cluster_test_packet {
    ucn_node_id_t source;
    ucn_node_id_t destination;
    /* Delivery is deferred until network->now_ms >= deliver_at_ms.  The
     * delay_one_in fault knob uses this to model in-flight reordering. */
    uint32_t deliver_at_ms;
    uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES];
} cluster_test_packet_t;

struct cluster_test_network {
    uint32_t now_ms;
    cluster_test_node_t nodes[CLUSTER_TEST_NODES];
    cluster_test_packet_t queue[CLUSTER_TEST_QUEUE];
    size_t queue_count;
};

/* CLV2-01-02: test-side mirror of cluster_phase_from_legacy_state().  This
 * is the SPEC of the mapping (written from the role+bool+deadline combos),
 * duplicated on purpose so a wrong production mapping cannot also silently
 * 'fix' the expectation. */
static ucn_cluster_phase_t test_derive_phase(const ucn_cluster_t *c,
                                             uint32_t now_ms)
{
    if (!c->config.enabled) {
        return UCN_CLUSTER_PHASE_DISABLED;
    }
    switch (c->role) {
    case UCN_CLUSTER_ROLE_DISABLED:
        return UCN_CLUSTER_PHASE_DISABLED;
    case UCN_CLUSTER_ROLE_DETACHED:
        if (c->recovery_eligible) {
            if (c->recovery_backoff_deadline_ms != 0U &&
                (c->recovery_cooldown_until_ms == 0U ||
                 ucn_deadline_expired(now_ms, c->recovery_cooldown_until_ms))) {
                return UCN_CLUSTER_PHASE_RECOVERY_ELECTION;
            }
            return UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
        }
        return UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    case UCN_CLUSTER_ROLE_CANDIDATE:
        return UCN_CLUSTER_PHASE_ELECTION;
    case UCN_CLUSTER_ROLE_JOIN_PENDING:
        return UCN_CLUSTER_PHASE_JOIN_PENDING;
    case UCN_CLUSTER_ROLE_MEMBER:
        /* CLV2-M01.0.1: armed grace deadline IS the grace phase; timer
         * expiry is consumed by the owner's timeout action. */
        if (c->head_grace_deadline_ms != 0U) {
            return UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE;
        }
        return UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    case UCN_CLUSTER_ROLE_HEAD:
        /* CLV2-M01.0.1: assignment cycle -> ASSIGNING, assignment done
         * without READY -> SYNCING, READY -> STABLE.  The mirror flag
         * backup_syncing is Backup-side state, never a Head phase. */
        if (c->backup_node_id == 0U) {
            return UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
        }
        if (c->backup_ready) {
            return UCN_CLUSTER_PHASE_HEAD_STABLE;
        }
        if (c->backup_assign_pending) {
            return UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
        }
        return UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
    case UCN_CLUSTER_ROLE_BACKUP:
        if (c->backup_takeover_active) {
            return UCN_CLUSTER_PHASE_BACKUP_TAKEOVER;
        }
        if (c->backup_ready) {
            return UCN_CLUSTER_PHASE_BACKUP_READY;
        }
        return UCN_CLUSTER_PHASE_BACKUP_SYNCING;
    case UCN_CLUSTER_ROLE_STEPPING_DOWN:
        return UCN_CLUSTER_PHASE_STEPPING_DOWN;
    case UCN_CLUSTER_ROLE_RECOVERY_HEAD:
        return UCN_CLUSTER_PHASE_RECOVERY_HEAD;
    default:
        return UCN_CLUSTER_PHASE_DISABLED;
    }
}

/* CLV2-M01.0.1: test-side mirror of cluster_legacy_state_is_valid(). */
static bool test_legacy_state_valid(const ucn_cluster_t *c)
{
    if (c->role == UCN_CLUSTER_ROLE_HEAD) {
        if (c->backup_ready && c->backup_node_id == 0U) {
            return false;
        }
        if (c->backup_syncing) {
            return false;
        }
    }
    if (c->role == UCN_CLUSTER_ROLE_BACKUP) {
        /* takeover_active && syncing is REACHABLE (delayed Primary
         * Type12 during takeover); see production comment M01.0.2. */
        if (c->backup_ready && c->backup_syncing) {
            return false;
        }
    }
    return true;
}

/* CLV2-01-04a review B (T-A): forward declaration for the observed-pair
 * collector defined below the audit (the audit records into it). */
static void cluster_test_observed_record(ucn_cluster_phase_t old_phase,
                                         ucn_cluster_phase_t new_phase);

/* CLV2-01-02/03: every tick ends with a VALID legacy state, the shadow
 * mirror equal to the spec derivation, and every OBSERVED phase change
 * carrying a non-UNKNOWN reason. */
static int cluster_test_shadow_audit(cluster_test_network_t *network)
{
    size_t index;

    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        cluster_test_node_t *node = &network->nodes[index];
        ucn_cluster_t *c = &node->cluster;
        ucn_cluster_phase_t derived;

        if (!node->alive) {
            continue;
        }
        TEST_ASSERT(test_legacy_state_valid(c));
        derived = test_derive_phase(c, network->now_ms);
        TEST_ASSERT(c->shadow_phase == derived);
        if (node->audit_seen && c->shadow_phase != node->last_audit_phase) {
            /* CLV2-01-04a review B (T-A): capture every real FSM phase
             * change for the observed-pairs subset-of-SPEC guard. */
            cluster_test_observed_record(node->last_audit_phase,
                                         c->shadow_phase);
            if (c->transition_reason == UCN_CLUSTER_REASON_UNKNOWN) {
                (void)fprintf(stderr,
                              "SHADOW-AUDIT: UNKNOWN reason node=%lu "
                              "old=%d new=%d t=%lu\n",
                              (unsigned long)node->node_id,
                              (int)node->last_audit_phase, (int)c->shadow_phase,
                              (unsigned long)network->now_ms);
            }
            TEST_ASSERT(c->transition_reason != UCN_CLUSTER_REASON_UNKNOWN);
        }
        node->last_audit_phase = c->shadow_phase;
        node->audit_seen = true;
    }
    return 0;
}

/* CLV2-01-04a review B (T-A): test-side collector of every (old_phase,
 * new_phase) shadow transition actually OBSERVED across the scenario
 * suite (canonical lifecycle, fault suite incl. golden path, recovery,
 * late-sync takeover).  Records distinct pairs from cluster_test_shadow_
 * audit() and asserts observed SUBSET-OF CLUSTER_TRANSITION_SPEC at the
 * end of test_cluster() - the missing-direction guard (SPEC must not
 * under-cover reality). */
/* Forward declarations: the DIRECT membership test and the OBSERVED
 * membership wrapper (which delegates to the production table hook) are
 * defined near CLUSTER_TRANSITION_DIRECT later in this file. */
static bool cluster_transition_pair_in_direct(ucn_cluster_phase_t old_phase,
                                              ucn_cluster_phase_t new_phase);
static bool cluster_transition_pair_in_observed(ucn_cluster_phase_t old_phase,
                                                ucn_cluster_phase_t new_phase);

#define CLUSTER_TEST_OBSERVED_MAX ((size_t)128U)
static ucn_cluster_phase_t cluster_observed_old[CLUSTER_TEST_OBSERVED_MAX];
static ucn_cluster_phase_t cluster_observed_new[CLUSTER_TEST_OBSERVED_MAX];
static size_t cluster_observed_count;

static void cluster_test_observed_record(ucn_cluster_phase_t old_phase,
                                         ucn_cluster_phase_t new_phase)
{
    size_t index;

    for (index = 0U; index < cluster_observed_count; ++index) {
        if (cluster_observed_old[index] == old_phase &&
            cluster_observed_new[index] == new_phase) {
            return; /* distinct pairs only */
        }
    }
    if (cluster_observed_count < CLUSTER_TEST_OBSERVED_MAX) {
        cluster_observed_old[cluster_observed_count] = old_phase;
        cluster_observed_new[cluster_observed_count] = new_phase;
        cluster_observed_count++;
    }
}

/* Print the captured observed-pair list (deliverable evidence). */
static void cluster_test_observed_dump(void)
{
    size_t index;

    (void)fprintf(stderr, "OBSERVED-PAIRS: count=%lu\n",
                  (unsigned long)cluster_observed_count);
    for (index = 0U; index < cluster_observed_count; ++index) {
        (void)fprintf(stderr, "OBSERVED-PAIRS: %d -> %d\n",
                      (int)cluster_observed_old[index],
                      (int)cluster_observed_new[index]);
    }
}

/* T-A final gate (CLV2-01-04a.1 Item 1): every observed pair must be in
 * OBSERVED_ALLOWED - checked against the SINGLE production
 * CLUSTER_TRANSITION_OBSERVED_ALLOWED table (CLV2-01-04b NIT-1).  A
 * violation is a REAL pair the observed set misses - it must be restored,
 * never hidden. */
static int cluster_test_observed_within_spec(void)
{
    size_t index;
    int violations = 0;
    size_t observed_count = cluster_observed_count;

    /* Dump the collected observations first (deliverable evidence). */
    cluster_test_observed_dump();
    for (index = 0U; index < observed_count; ++index) {
        if (!cluster_transition_pair_in_observed(cluster_observed_old[index],
                                                 cluster_observed_new[index])) {
            (void)fprintf(stderr,
                          "OBSERVED-PAIRS: VIOLATION %d -> %d observed "
                          "but NOT in OBSERVED_ALLOWED - restore this pair\n",
                          (int)cluster_observed_old[index],
                          (int)cluster_observed_new[index]);
            violations++;
        }
    }
    /* CLV2-01-04a review C (G4): the collector is file-static; reset it
     * at the gate so a re-run never sees stale observations. */
    cluster_observed_count = 0U;
    return violations == 0 ? 0 : 1;
}

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
    packet->deliver_at_ms = network->now_ms;
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

/* CLV2-M00-07 fault-injection knobs for the virtual network.  All of
 * these are test-only; the production cluster code is never touched.
 *
 * CLV2-M00.1 additions: deterministic xorshift32 PRNG (seed), duplicate,
 * delay, reorder, Owner step skip, neighbor state override, partition
 * heal, and a storage-failure placeholder that M04 will connect to the
 * real Persistence Provider. */
typedef struct cluster_test_fault {
    /* drop_one_in == 0 disables; otherwise every Nth frame is dropped. */
    uint32_t drop_one_in;
    /* dup_one_in == 0 disables; otherwise every Nth frame is delivered
     * twice (duplicate injection). */
    uint32_t dup_one_in;
    /* delay_one_in == 0 disables; otherwise every Nth frame is deferred
     * by one tick (delay injection). */
    uint32_t delay_one_in;
    /* reorder_one_in == 0 disables; otherwise every Nth frame swaps
     * places with its successor in the queue (reorder injection). */
    uint32_t reorder_one_in;
    /* per-pair reachability: partition[a][b] == false blocks a->b. */
    bool partition[CLUSTER_TEST_NODES][CLUSTER_TEST_NODES];
    /* partition_heal_at_ms > 0: once now_ms reaches it, every pair
     * becomes reachable again (one-shot partition heal). */
    uint32_t partition_heal_at_ms;
    bool partition_healed;
    /* deliver at most this many frames per tick (0 = unlimited). */
    size_t deliver_budget;
    /* restart a node at a given tick: re-init its cluster to DETACHED
     * with a fresh nonce counter, simulating a power cycle. */
    uint32_t restart_node_id;
    uint32_t restart_at_ms;
    bool restart_done;
    /* skip_step_mask: bit (1 << node_index) set => that node's
     * ucn_cluster_step() is skipped this tick (Owner step violation). */
    uint32_t skip_step_mask;
    /* neighbor_override[node][peer]: 0xFF = no override; otherwise the
     * ucn_neighbor_state_t forced into sync_neighbors for that pair
     * (ADMITTED/SUSPECT/REMOVED flapping). */
    uint8_t neighbor_override[CLUSTER_TEST_NODES][CLUSTER_TEST_NODES];
    /* Deterministic xorshift32 state.  0 means uninitialized; the
     * first tick seeds it from CLUSTER_TEST_FAULT_SEED so replays are
     * bit-identical across runs. */
    uint32_t rng_state;
    /* M04 placeholder: a storage failure is not connected yet because
     * Persistence does not exist in this baseline.  M04 will turn this
     * into a real Provider error hook. */
    uint32_t storage_fail_at_ms;
    bool storage_fail_armed;
} cluster_test_fault_t;

#define CLUSTER_TEST_FAULT_SEED UINT32_C(0x5EED1234)

static uint32_t cluster_test_fault_rand(cluster_test_fault_t *fault)
{
    uint32_t x;

    if (fault->rng_state == 0U) {
        fault->rng_state = CLUSTER_TEST_FAULT_SEED;
    }
    x = fault->rng_state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    fault->rng_state = x;
    return x;
}

/* CLV2-M00.1: canonical fault setup.  Zeroes the struct, seeds the PRNG
 * and marks every neighbor override as inactive (0xFF).  Tests then set
 * only the knobs they need; partition stays all-false (the legacy
 * default) unless a test explicitly enables reachability. */
static void cluster_test_fault_setup(cluster_test_fault_t *fault)
{
    size_t i;
    size_t j;

    (void)memset(fault, 0, sizeof(*fault));
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
            fault->neighbor_override[i][j] = 0xFFU;
        }
    }
    fault->rng_state = CLUSTER_TEST_FAULT_SEED;
}

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
            if (fault != NULL && fault->neighbor_override[node_index][peer_index] !=
                    0xFFU) {
                /* CLV2-M00.1 neighbor flap: force SUSPECT/REMOVED so
                 * tests exercise liveness downgrades without a real
                 * link-loss timeline. */
                summaries[count].state =
                    (ucn_neighbor_state_t)fault
                        ->neighbor_override[node_index][peer_index];
            } else {
                summaries[count].state = UCN_NEIGHBOR_ADMITTED;
            }
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
                                        cluster_test_fault_t *fault)
{
    size_t index = 0U;
    size_t kept = 0U;
    size_t delivered = 0U;
    uint32_t seen = 0U;

    /* CLV2-M00.1 reorder pass: every Nth frame swaps with its successor
     * before any delivery decision (explicit reorder injection). */
    if (fault != NULL && fault->reorder_one_in != 0U) {
        size_t i;

        for (i = (size_t)fault->reorder_one_in; i < network->queue_count;
             i += (size_t)fault->reorder_one_in) {
            cluster_test_packet_t tmp = network->queue[i - 1U];

            network->queue[i - 1U] = network->queue[i];
            network->queue[i] = tmp;
        }
    }
    while (index < network->queue_count) {
        cluster_test_packet_t packet = network->queue[index];
        cluster_test_node_t *target;
        ucn_result_t result;

        index++;
        /* Deferred frame (delay injection): keep it queued for a later
         * tick. */
        if (packet.deliver_at_ms > network->now_ms) {
            network->queue[kept++] = packet;
            continue;
        }
        target = cluster_test_find_node(network, packet.destination);
        if (target == NULL || !target->alive) {
            continue;
        }
        /* Deterministic drop: every Nth frame in the queue is dropped. */
        seen++;
        if (fault != NULL && fault->drop_one_in != 0U &&
            (seen % fault->drop_one_in) == 0U) {
            continue;
        }
        /* CLV2-M00.1 duplicate injection: every Nth frame is delivered
         * twice (the copy re-enters the queue tail). */
        if (fault != NULL && fault->dup_one_in != 0U &&
            (seen % fault->dup_one_in) == 0U &&
            network->queue_count < CLUSTER_TEST_QUEUE) {
            network->queue[network->queue_count++] = packet;
        }
        /* CLV2-M00.1 delay injection: every Nth frame is deferred by
         * 1-2 ticks; the exact amount comes from the seeded PRNG so the
         * same fault seed replays bit-identically. */
        if (fault != NULL && fault->delay_one_in != 0U &&
            (seen % fault->delay_one_in) == 0U) {
            packet.deliver_at_ms =
                network->now_ms + 1U + (cluster_test_fault_rand(fault) % 2U);
            network->queue[kept++] = packet;
            continue;
        }
        if (fault != NULL && fault->deliver_budget != 0U &&
            delivered >= fault->deliver_budget) {
            /* Budget exhausted: preserve the un-delivered tail (current
             * packet plus everything after it) for the next tick. */
            size_t remain = network->queue_count - index;

            network->queue[kept++] = packet;
            (void)memmove(&network->queue[kept], &network->queue[index],
                          remain * sizeof(network->queue[0]));
            network->queue_count = kept + remain;
            return 0;
        }
        delivered++;
        result = ucn_cluster_receive(&target->cluster, packet.source, true,
                                     packet.payload, sizeof(packet.payload));
        TEST_ASSERT(result == UCN_OK || result == UCN_ERR_ACCESS ||
                    result == UCN_ERR_NO_SPACE || result == UCN_ERR_REPLAY ||
                    result == UCN_ERR_NOT_FOUND);
    }
    network->queue_count = kept;
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
                /* CLV2-01-04a T-A: a restart re-seeds the shadow (init),
                 * it is not a real FSM transition - restart the audit
                 * bookkeeping so the observed-pairs collector stays clean. */
                network->nodes[index].audit_seen = false;
                network->nodes[index].last_audit_phase =
                    network->nodes[index].cluster.shadow_phase;
                fault->restart_done = true;
                break;
            }
        }
    }
    /* CLV2-M00.1 one-shot partition heal: full reachability restored. */
    if (fault != NULL && !fault->partition_healed &&
        fault->partition_heal_at_ms != 0U &&
        now_ms >= fault->partition_heal_at_ms) {
        size_t a;
        size_t b;

        for (a = 0U; a < CLUSTER_TEST_NODES; ++a) {
            for (b = 0U; b < CLUSTER_TEST_NODES; ++b) {
                fault->partition[a][b] = true;
            }
        }
        fault->partition_healed = true;
    }
    /* CLV2-M00.1 storage-failure placeholder: only arms the flag today;
     * M04 connects it to the real Persistence Provider failure path. */
    if (fault != NULL && !fault->storage_fail_armed &&
        fault->storage_fail_at_ms != 0U &&
        now_ms >= fault->storage_fail_at_ms) {
        fault->storage_fail_armed = true;
    }
    TEST_ASSERT(cluster_test_sync_neighbors_faulted(network, fault) == 0);
    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        if (!network->nodes[index].alive) {
            continue;
        }
        /* CLV2-M00.1 Owner step violation: this node's step is skipped
         * while time keeps advancing for everyone else. */
        if (fault != NULL &&
            (fault->skip_step_mask & (UINT32_C(1) << index)) != 0U) {
            continue;
        }
        TEST_ASSERT(ucn_cluster_step(&network->nodes[index].cluster) == UCN_OK);
    }
    TEST_ASSERT(cluster_test_deliver_faulted(network, fault) == 0);
    TEST_ASSERT(cluster_test_shadow_audit(network) == 0);
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
    TEST_ASSERT(cluster_test_shadow_audit(network) == 0);
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

static void cluster_trace_capture_node(const cluster_test_node_t *node,
                                       uint32_t now_ms,
                                       cluster_trace_snapshot_t *entry)
{
    const ucn_cluster_t *c = &node->cluster;

    entry->now_ms = now_ms;
    entry->node_id = node->node_id;
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

static void cluster_trace_capture(cluster_test_network_t *network,
                                  cluster_trace_snapshot_t *out)
{
    size_t index;

    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        cluster_trace_capture_node(&network->nodes[index], network->now_ms,
                                   &out[index]);
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

/* CLV2-M00.1: one line per (node, event) transition carrying BOTH the
 * old and the new snapshot, so a tick that internally passes through
 * several states still shows the net transition and a later M01 Phase
 * refactor must reproduce the same old->new pairs, not just the same
 * final state. */
static void cluster_trace_write_line(FILE *stream,
                                     const char *event,
                                     const cluster_trace_snapshot_t *old_entry,
                                     const cluster_trace_snapshot_t *new_entry)
{
    (void)fprintf(stream,
        "t=%lu node=%lu event=%s "
        "old={role=%d cid=%lu term=%lu gen=%lu seq=%lu ready=%u "
        "syncing=%u takeover=%u recovery=%u} "
        "new={role=%d cid=%lu term=%lu gen=%lu seq=%lu ready=%u "
        "syncing=%u takeover=%u recovery=%u}\n",
        (unsigned long)new_entry->now_ms,
        (unsigned long)new_entry->node_id, event,
        (int)old_entry->role, (unsigned long)old_entry->cluster_id,
        (unsigned long)old_entry->term,
        (unsigned long)old_entry->backup_generation,
        (unsigned long)old_entry->membership_sequence,
        (unsigned)old_entry->backup_ready, (unsigned)old_entry->backup_syncing,
        (unsigned)old_entry->backup_takeover_active,
        (unsigned)old_entry->recovery_eligible,
        (int)new_entry->role, (unsigned long)new_entry->cluster_id,
        (unsigned long)new_entry->term,
        (unsigned long)new_entry->backup_generation,
        (unsigned long)new_entry->membership_sequence,
        (unsigned)new_entry->backup_ready, (unsigned)new_entry->backup_syncing,
        (unsigned)new_entry->backup_takeover_active,
        (unsigned)new_entry->recovery_eligible);
}

/* CLV2-M00.1 traced delivery: every received frame is a traceable event.
 * The wire type (payload byte 1) names the event, e.g. RX:12, so a later
 * M01 refactor must reproduce the same per-frame state transitions. */
static int cluster_test_deliver_traced(cluster_test_network_t *network,
                                       FILE *trace)
{
    size_t index = 0U;
    size_t kept = 0U;

    while (index < network->queue_count) {
        cluster_test_packet_t packet = network->queue[index];
        cluster_test_node_t *target;
        cluster_trace_snapshot_t before;
        cluster_trace_snapshot_t after;
        char event[24];
        ucn_result_t result;

        index++;
        if (packet.deliver_at_ms > network->now_ms) {
            network->queue[kept++] = packet;
            continue;
        }
        target = cluster_test_find_node(network, packet.destination);
        if (target == NULL || !target->alive) {
            continue;
        }
        (void)snprintf(event, sizeof(event), "RX:%u",
                       (unsigned)packet.payload[1U]);
        cluster_trace_capture_node(target, network->now_ms, &before);
        result = ucn_cluster_receive(&target->cluster, packet.source, true,
                                     packet.payload, sizeof(packet.payload));
        TEST_ASSERT(result == UCN_OK || result == UCN_ERR_ACCESS ||
                    result == UCN_ERR_NO_SPACE || result == UCN_ERR_REPLAY ||
                    result == UCN_ERR_NOT_FOUND);
        cluster_trace_capture_node(target, network->now_ms, &after);
        if (!cluster_trace_snapshot_equal(&before, &after)) {
            cluster_trace_write_line(trace, event, &before, &after);
        }
    }
    network->queue_count = kept;
    return 0;
}

/* CLV2-M00.1 traced tick: SYNC (neighbor view), STEP (per owner) and
 * RX:<type> (per frame) transitions are recorded with old/new snapshots.
 * This is the Golden Trace driver; keep it deterministic. */
static int cluster_test_tick_traced(cluster_test_network_t *network,
                                    uint32_t now_ms,
                                    FILE *trace)
{
    cluster_trace_snapshot_t before[CLUSTER_TEST_NODES];
    cluster_trace_snapshot_t after[CLUSTER_TEST_NODES];
    size_t index;

    network->now_ms = now_ms;
    cluster_trace_capture(network, before);
    TEST_ASSERT(cluster_test_sync_neighbors(network) == 0);
    cluster_trace_capture(network, after);
    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        if (!cluster_trace_snapshot_equal(&before[index], &after[index])) {
            cluster_trace_write_line(trace, "SYNC", &before[index],
                                     &after[index]);
        }
    }
    for (index = 0U; index < CLUSTER_TEST_NODES; ++index) {
        cluster_test_node_t *node = &network->nodes[index];

        if (!node->alive) {
            continue;
        }
        cluster_trace_capture_node(node, network->now_ms, &before[index]);
        TEST_ASSERT(ucn_cluster_step(&node->cluster) == UCN_OK);
        cluster_trace_capture_node(node, network->now_ms, &after[index]);
        if (!cluster_trace_snapshot_equal(&before[index], &after[index])) {
            cluster_trace_write_line(trace, "STEP", &before[index],
                                     &after[index]);
        }
    }
    TEST_ASSERT(cluster_test_deliver_traced(network, trace) == 0);
    return 0;
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
    /* CLV2-01-04b.4: the JOIN_REJECT detach now routes through
     * cluster_transition(), which fails closed unless the shadow mirror
     * already derives JOIN_PENDING - align it (a real join flow syncs the
     * shadow at the end of the begin_join step/RX). */
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
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

/* CLV2-01-04b.4 (a): a JOIN_PENDING node that joins a full/frozen Head and
 * receives the exact-epoch JOIN_REJECT detaches THROUGH the single
 * transition entry point: shadow lands on DETACHED_OBSERVE with reason
 * JOIN_REJECTED (not just the legacy role write). */
static int cluster_test_join_reject_shadow_transition(void)
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
    /* Align the shadow mirror: a real join flow syncs it to JOIN_PENDING
     * at the end of the begin_join step/RX; the migrated JOIN_REJECT path
     * fails closed unless the shadow already derives the claimed phase. */
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    network.now_ms = 0U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_REJECT;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.lease_ms = 40U;
    message.nonce = 9U; /* exact join txid */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);

    /* Legacy detach site effects are preserved (epoch/vote/lease clears). */
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.cluster_id == 0U);
    TEST_ASSERT(node->cluster.term == 0U);
    TEST_ASSERT(node->cluster.head_node_id == 0U);
    TEST_ASSERT(node->cluster.head_lease_expires_at_ms == 0U);
    TEST_ASSERT(node->cluster.member_voted_term == 0U);
    TEST_ASSERT(node->cluster.stats.joins_rejected == 1U);
    /* The transition entry point committed the shadow + exact reason. */
    TEST_ASSERT(node->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(node->cluster.transition_reason ==
                UCN_CLUSTER_REASON_JOIN_REJECTED);
    TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
    return 0;
}

/* CLV2-01-04b.4 (b): OUT-OF-ORDER join - BACKUP_ASSIGN(self) wins the race
 * against a late JOIN_ACCEPT.  The node transitions JOIN_PENDING ->
 * BACKUP_SYNCING at the assign; the late ACCEPT must NOT transition (the
 * pre-assigned Backup only refreshes its epoch fields), so the shadow
 * stays BACKUP_SYNCING with no MEMBER transition and no UCN_ERR_STATE. */
static int cluster_test_join_accept_out_of_order_after_backup_assign(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    node = &network.nodes[1]; /* head-capable, so BACKUP_ASSIGN(self) works */
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.pending_head_node_id = network.nodes[0].node_id;
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 5U;
    node->cluster.pending_join_nonce = 9U;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    network.now_ms = 0U;

    /* BACKUP_ASSIGN(self) arrives first: JOIN_PENDING -> BACKUP_SYNCING. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_BACKUP_ASSIGN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.sync_token = node->node_id;
    message.backup_generation = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(node->cluster.backup_syncing == true);
    TEST_ASSERT(node->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);

    /* A late JOIN_ACCEPT (same epoch + txid, fencing still passes) must
     * NOT transition: shadow stays BACKUP_SYNCING, role stays BACKUP. */
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_ACCEPT;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.head_score = 9000U;
    message.lease_ms = 40U;
    message.nonce = 9U; /* exact join txid */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(node->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(node->cluster.shadow_phase != UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    /* One shadow transition only (the BACKUP_ASSIGN); the ACCEPT refreshed
     * epoch fields without moving the phase. */
    TEST_ASSERT(node->cluster.shadow_transition_count == 1U);
    /* The accept side effects still run for the pre-assigned Backup
     * (pending_head/cluster/term are cleared; the C07.7 join txid
     * pending_join_nonce is deliberately left untouched by the site). */
    TEST_ASSERT(node->cluster.pending_head_node_id == 0U);
    TEST_ASSERT(node->cluster.pending_cluster_id == 0U);
    TEST_ASSERT(node->cluster.pending_term == 0U);
    TEST_ASSERT(node->cluster.head_node_id == network.nodes[0].node_id);
    TEST_ASSERT(node->cluster.cluster_id == 1U);
    TEST_ASSERT(node->cluster.term == 5U);
    TEST_ASSERT(node->cluster.stats.joins_accepted == 1U);
    /* known_backup_* retained (retained-state Test A; the ACCEPT never
     * clears the assignment knowledge). */
    TEST_ASSERT(node->cluster.known_backup_node_id == node->node_id);
    TEST_ASSERT(node->cluster.known_backup_generation == 1U);
    return 0;
}

static int cluster_test_join_pending_stepdown(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *node;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 0U;
    node = &network.nodes[2];

    /* (a) A join-pending node receives a HEAD_STEPDOWN of the CURRENT
     * epoch with a fresh nonce: the join attempt ends through the single
     * transition entry point (explicit STEPDOWN_ORDERED - never the
     * JOIN_REJECTED BEST-EFFORT fallback) and the legacy detach site
     * still runs. */
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.pending_head_node_id = network.nodes[0].node_id;
    node->cluster.pending_cluster_id = 1U;
    node->cluster.pending_term = 5U;
    node->cluster.last_stepdown_nonce = 0U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(node->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(node->cluster.transition_reason ==
                UCN_CLUSTER_REASON_STEPDOWN_ORDERED);
    /* the nonce fence advanced and the site-side detach still ran */
    TEST_ASSERT(node->cluster.last_stepdown_nonce == 1U);
    TEST_ASSERT(node->cluster.pending_head_node_id == 0U);

    /* (b) A STALE nonce of the same epoch is fenced BEFORE any transition
     * is attempted: no state change, no reason change. */
    node->cluster.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    node->cluster.shadow_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
    node->cluster.transition_reason = UCN_CLUSTER_REASON_INIT;
    node->cluster.cluster_id = 1U;
    node->cluster.term = 5U;
    node->cluster.head_node_id = network.nodes[0].node_id;
    node->cluster.last_stepdown_nonce = 1U;
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_HEAD_STEPDOWN;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 5U;
    message.head_node_id = network.nodes[0].node_id;
    message.lease_ms = 8000U;
    message.nonce = 1U; /* not strictly greater than last_stepdown_nonce */
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&node->cluster, network.nodes[0].node_id,
                                    true, encoded, sizeof(encoded)) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(node->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(node->cluster.transition_reason ==
                UCN_CLUSTER_REASON_INIT);
    return 0;
}

/* CLV2-M00-03: deterministic golden trace over the canonical lifecycle
 * (election -> join -> backup -> failover takeover).  Compares byte-wise
 * against tests/golden/cluster_golden_trace.txt (committed reference).
 *
 * CLV2-M00.1 fail-closed rules:
 *  - the reference file is read-only during tests; the actual trace is
 *    written into the BUILD directory (per-route, no parallel races);
 *  - a MISSING reference FAILS the test unless the environment variable
 *    UCN_UPDATE_CLUSTER_GOLDEN=1 explicitly requests regeneration (a
 *    maintenance action that must be followed by a commit);
 *  - every transition line carries old/event/new snapshots (SYNC, STEP
 *    and RX:<type> events). */
/* CLV2-M00-03: deterministic golden trace over the canonical lifecycle
 * (election -> join -> backup -> failover takeover).  Compares byte-wise
 * against tests/golden/cluster_golden_trace.txt (committed reference).
 *
 * CLV2-M00.1 fail-closed rules:
 *  - the reference file is read-only during tests; the actual trace is
 *    written into the BUILD directory (per-route, no parallel races);
 *  - a MISSING reference FAILS the test unless the environment variable
 *    UCN_UPDATE_CLUSTER_GOLDEN=1 explicitly requests regeneration (a
 *    maintenance action that must be followed by a commit);
 *  - every transition line carries old/event/new snapshots (SYNC, STEP
 *    and RX:<type> events). */
static int cluster_test_golden_trace(void)
{
    cluster_test_network_t network;
    char golden_path[512];
    char trace_path[512];
    FILE *golden = NULL;
    FILE *trace = NULL;
    uint32_t now_ms;
    bool first_run = false;
    const char *update_env = getenv("UCN_UPDATE_CLUSTER_GOLDEN");
    int line = 0;
    char expected[512];
    char actual[512];

#ifdef UCN_CLUSTER_GOLDEN_REFERENCE_DIR
    (void)snprintf(golden_path, sizeof(golden_path),
                   UCN_CLUSTER_GOLDEN_REFERENCE_DIR "/cluster_golden_trace.txt");
#else
    (void)snprintf(golden_path, sizeof(golden_path),
                   "/tmp/cluster_golden_trace.txt");
#endif
#ifdef UCN_CLUSTER_GOLDEN_ACTUAL_DIR
    (void)snprintf(trace_path, sizeof(trace_path),
                   UCN_CLUSTER_GOLDEN_ACTUAL_DIR "/cluster_golden_trace.txt");
#else
    (void)snprintf(trace_path, sizeof(trace_path),
                   "/tmp/cluster_golden_trace_actual.txt");
#endif
    /* Fail closed: without the explicit maintenance flag a MISSING
     * committed reference is a test failure, not an invitation to
     * silently mint a new golden from current behaviour.  WITH the flag
     * the reference is always (re)generated and must be committed. */
    if (update_env != NULL && strcmp(update_env, "1") == 0) {
        first_run = true;
    } else {
        golden = fopen(golden_path, "r");
        if (golden == NULL) {
            fprintf(stderr,
                    "CLV2-M00-03: golden trace MISSING at %s; the gate "
                    "fails closed. Regenerate explicitly with "
                    "UCN_UPDATE_CLUSTER_GOLDEN=1 and commit the file.\n",
                    golden_path);
            return -1;
        }
        (void)fclose(golden);
        golden = NULL;
    }
    trace = fopen(trace_path, "w");
    TEST_ASSERT(trace != NULL);

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_traced(&network, now_ms, trace) == 0);
    }
    /* Primary dies: failover takeover completes on node 1. */
    network.nodes[0].alive = false;
    for (now_ms = 141U; now_ms <= 300U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_traced(&network, now_ms, trace) == 0);
    }
    (void)fclose(trace);

    if (first_run) {
        if (rename(trace_path, golden_path) != 0) {
            fprintf(stderr,
                    "CLV2-M00-03: failed to install golden at %s\n",
                    golden_path);
            return -1;
        }
        fprintf(stderr,
                "CLV2-M00-03: golden trace generated at %s; commit it.\n",
                golden_path);
        return 0;
    }
    /* Byte-wise comparison of the actual trace against the golden file. */
    golden = fopen(golden_path, "r");
    TEST_ASSERT(golden != NULL);
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

    cluster_test_fault_setup(&fault);
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

    cluster_test_fault_setup(&fault);
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
    cluster_test_fault_setup(&fault);
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

/* CLV2-M00.1: deterministic replay under a mixed fault profile.  The
 * same seed must reproduce bit-identical end states; otherwise a fault
 * test is not a regression gate. */
static int cluster_test_fault_deterministic_replay(void)
{
    cluster_test_fault_t fault;
    uint32_t run;
    ucn_cluster_role_t roles[2U][CLUSTER_TEST_NODES];
    uint32_t terms[2U][CLUSTER_TEST_NODES];

    for (run = 0U; run < 2U; ++run) {
        cluster_test_network_t network;
        uint32_t now_ms;
        size_t i;
        size_t j;

        TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
        cluster_test_fault_setup(&fault);
        for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
            for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
                fault.partition[i][j] = true;
            }
        }
        fault.drop_one_in = 11U;
        fault.dup_one_in = 17U;
        fault.delay_one_in = 13U;
        fault.reorder_one_in = 19U;
        for (now_ms = 0U; now_ms <= 400U; ++now_ms) {
            TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) ==
                        0);
        }
        for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
            roles[run][i] = network.nodes[i].cluster.role;
            terms[run][i] = network.nodes[i].cluster.term;
        }
    }
    for (size_t i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        TEST_ASSERT(roles[0U][i] == roles[1U][i]);
        TEST_ASSERT(terms[0U][i] == terms[1U][i]);
    }
    return 0;
}

/* CLV2-M00.1: duplicate + delay + reorder traffic must not break the
 * single-Head invariant or crash the receiver (anti-replay rejects
 * duplicates; delayed/reordered frames must be tolerated). */
static int cluster_test_fault_dup_delay_reorder_converges(void)
{
    cluster_test_network_t network;
    cluster_test_fault_t fault;
    uint32_t now_ms;
    size_t i;
    size_t heads;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    cluster_test_fault_setup(&fault);
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        size_t j;

        for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
            fault.partition[i][j] = true;
        }
    }
    fault.dup_one_in = 7U;
    fault.delay_one_in = 9U;
    fault.reorder_one_in = 5U;
    fault.drop_one_in = 13U;
    for (now_ms = 0U; now_ms <= 500U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    heads = 0U;
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        if (network.nodes[i].cluster.role == UCN_CLUSTER_ROLE_HEAD) {
            heads++;
        }
    }
    /* Single-Head invariant survives the injections. */
    TEST_ASSERT(heads == 1U);
    return 0;
}

/* CLV2-M00.1: Owner step violation.  Skipping the Head's step makes its
 * lease lapse from the members' point of view; the protocol must keep
 * the single-Head invariant and converge again once steps resume. */
static int cluster_test_fault_skip_owner_step(void)
{
    cluster_test_network_t network;
    cluster_test_fault_t fault;
    uint32_t now_ms;
    size_t i;
    size_t heads;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    cluster_test_fault_setup(&fault);
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        size_t j;

        for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
            fault.partition[i][j] = true;
        }
    }
    /* Skip the Head's step from 141..180 ms (longer than lease_ms=40). */
    fault.skip_step_mask = 1U;
    for (now_ms = 141U; now_ms <= 180U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    /* Resume normal ticking: the system must re-converge to one Head. */
    cluster_test_fault_setup(&fault);
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        size_t j;

        for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
            fault.partition[i][j] = true;
        }
    }
    for (now_ms = 181U; now_ms <= 500U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    heads = 0U;
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        if (network.nodes[i].cluster.role == UCN_CLUSTER_ROLE_HEAD) {
            heads++;
        }
    }
    TEST_ASSERT(heads == 1U);
    return 0;
}

/* CLV2-M00.1: neighbor flap.  Forcing SUSPECT then REMOVED on the
 * Head's view of a member exercises liveness downgrade/removal paths;
 * restoring ADMITTED afterwards must let the cluster stay consistent. */
static int cluster_test_fault_neighbor_flap(void)
{
    cluster_test_network_t network;
    cluster_test_fault_t fault;
    uint32_t now_ms;
    size_t heads;
    size_t i;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    cluster_test_fault_setup(&fault);
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        size_t j;

        for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
            fault.partition[i][j] = true;
        }
    }
    /* Head sees member 3 as SUSPECT (141..160) then REMOVED (161..200). */
    fault.neighbor_override[0U][2U] = UCN_NEIGHBOR_SUSPECT;
    for (now_ms = 141U; now_ms <= 160U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    fault.neighbor_override[0U][2U] = UCN_NEIGHBOR_REMOVED;
    for (now_ms = 161U; now_ms <= 200U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    /* Heal the view and let the system re-converge. */
    fault.neighbor_override[0U][2U] = UCN_NEIGHBOR_ADMITTED;
    for (now_ms = 201U; now_ms <= 600U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    heads = 0U;
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        if (network.nodes[i].cluster.role == UCN_CLUSTER_ROLE_HEAD) {
            heads++;
        }
    }
    TEST_ASSERT(heads == 1U);
    return 0;
}

/* CLV2-M00.1: partition heal.  A partition isolates the Head, then the
 * network heals; the protocol must end in a single-Head cluster (today:
 * the majority side wins; M08 will additionally fence the old Head). */
static int cluster_test_fault_partition_heal(void)
{
    cluster_test_network_t network;
    cluster_test_fault_t fault;
    uint32_t now_ms;
    size_t i;
    size_t j;
    size_t heads;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    cluster_test_fault_setup(&fault);
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
            fault.partition[i][j] = true;
        }
    }
    for (j = 1U; j < CLUSTER_TEST_NODES; ++j) {
        fault.partition[0][j] = false;
        fault.partition[j][0] = false;
    }
    fault.partition_heal_at_ms = 260U;
    fault.storage_fail_at_ms = 300U; /* M04 placeholder hook arms */
    for (now_ms = 141U; now_ms <= 500U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick_faulted(&network, now_ms, &fault) == 0);
    }
    TEST_ASSERT(fault.partition_healed == true);
    TEST_ASSERT(fault.storage_fail_armed == true);
    heads = 0U;
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        if (network.nodes[i].cluster.role == UCN_CLUSTER_ROLE_HEAD) {
            heads++;
        }
    }
    TEST_ASSERT(heads == 1U);
    return 0;
}

/* CLV2-01-01: static legality of the phase enum and the total, unique
 * legacy->phase mapping.  Every implicit combination is checked against
 * its expected phase name. */
static int cluster_test_phase_mapping_static(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *c;
    ucn_cluster_config_t config;
    const uint32_t now = 1000U;

    TEST_ASSERT(UCN_CLUSTER_PHASE_COUNT == 17);
    TEST_ASSERT(UCN_CLUSTER_REASON_COUNT == 30);
    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    c = &network.nodes[0].cluster;
    config = c->config;

    /* Fresh init: DETACHED_OBSERVE, seeded by the production mapping. */
    TEST_ASSERT(ucn_cluster_init(c, &config) == UCN_OK);
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_INIT);

    /* Disabled. */
    config.enabled = false;
    TEST_ASSERT(ucn_cluster_init(c, &config) == UCN_OK);
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_DISABLED);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_DISABLED);
    config.enabled = true;

    /* Detached + recovery combos. */
    TEST_ASSERT(ucn_cluster_init(c, &config) == UCN_OK);
    c->recovery_eligible = true;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    c->recovery_backoff_deadline_ms = 1200U;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_RECOVERY_ELECTION);
    c->recovery_cooldown_until_ms = 1200U;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    c->recovery_eligible = false;
    c->recovery_backoff_deadline_ms = 0U;
    c->recovery_cooldown_until_ms = 0U;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);

    /* Simple role mappings. */
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_CANDIDATE);
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_ELECTION);
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_JOIN_PENDING);
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_STEPPING_DOWN);
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_STEPPING_DOWN);
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_RECOVERY_HEAD);
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_RECOVERY_HEAD);

    /* Member grace combos: arming the deadline is the phase change; a
     * deadline that has expired but is not yet consumed by the owner's
     * timeout action is STILL grace. */
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_MEMBER);
    c->head_grace_deadline_ms = 0U;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    c->head_grace_deadline_ms = now + 500U;
    TEST_ASSERT(test_derive_phase(c, now) ==
                UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
    c->head_grace_deadline_ms = now - 1U; /* expired, unconsumed */
    TEST_ASSERT(test_derive_phase(c, now) ==
                UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
    c->head_grace_deadline_ms = 0U;

    /* Head backup-progress ladder: NO_BACKUP -> ASSIGNING (assignment
     * cycle armed) -> SYNCING (assignment done, snapshot in flight) ->
     * STABLE (READY), plus the periodic-assignment-refresh combo.  The
     * mirror flag backup_syncing is deliberately NOT a Head phase. */
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_HEAD);
    c->backup_node_id = 0U;
    c->backup_ready = false;
    c->backup_syncing = false;
    c->backup_assign_pending = false;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    c->backup_node_id = 2U;
    c->backup_assign_pending = true;
    TEST_ASSERT(test_derive_phase(c, now) ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    c->backup_assign_pending = false;
    TEST_ASSERT(test_derive_phase(c, now) ==
                UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    c->backup_ready = true;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_HEAD_STABLE);
    c->backup_assign_pending = true; /* periodic assignment refresh */
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_HEAD_STABLE);

    /* Backup mirror combos. */
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_BACKUP);
    c->backup_syncing = true;
    c->backup_ready = false;
    c->backup_takeover_active = false;
    c->backup_assign_pending = false;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    c->backup_syncing = false;
    c->backup_ready = true;
    TEST_ASSERT(test_derive_phase(c, now) == UCN_CLUSTER_PHASE_BACKUP_READY);
    c->backup_takeover_active = true;
    TEST_ASSERT(test_derive_phase(c, now) ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);

    /* CLV2-M01.0.1: contradictory legacy combos are invalid and must
     * never get a phase name. */
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_HEAD);
    c->backup_node_id = 0U;
    c->backup_ready = true;
    c->backup_assign_pending = false;
    c->backup_syncing = false;
    TEST_ASSERT(test_legacy_state_valid(c) == false);
    c->backup_ready = false;
    c->backup_syncing = true; /* Head-side mirror flag: invalid */
    TEST_ASSERT(test_legacy_state_valid(c) == false);
    cluster_fixture_set_role(c, UCN_CLUSTER_ROLE_BACKUP);
    c->backup_syncing = true;
    c->backup_ready = true;
    c->backup_takeover_active = false;
    TEST_ASSERT(test_legacy_state_valid(c) == false);
    /* M01.0.2: takeover_active && syncing is REACHABLE (delayed
     * same-generation Type12 from the old Primary), so it is VALID and
     * the phase is BACKUP_TAKEOVER (takeover takes precedence). */
    c->backup_ready = false;
    c->backup_takeover_active = true;
    c->backup_syncing = true;
    TEST_ASSERT(test_legacy_state_valid(c) == true);
    TEST_ASSERT(test_derive_phase(c, now) ==
                UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    return 0;
}

/* CLV2-01-02/03: the canonical lifecycle must (a) keep the shadow mirror
 * aligned at every tick (audit inside cluster_test_tick already checks
 * this), (b) produce phase names with real semantics at the end, and
 * (c) never record an UNKNOWN transition reason on a phase change.
 *
 * CLV2-M01.0.1: the REAL Head phase ladder must be observed in order:
 * HEAD_NO_BACKUP -> HEAD_BACKUP_ASSIGNING -> HEAD_BACKUP_SYNCING ->
 * HEAD_STABLE (phases are not there just to make the enum look full). */
static int cluster_test_shadow_lifecycle(void)
{
    cluster_test_network_t network;
    uint32_t now_ms;
    size_t i;
    ucn_cluster_phase_t head_ladder[4];
    size_t ladder_len = 0U;
    bool saw_backup_ready = false;
    bool saw_member_active = false;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 300U; ++now_ms) {
        ucn_cluster_t *head = &network.nodes[0].cluster;

        if (head->role == UCN_CLUSTER_ROLE_HEAD &&
            (ladder_len == 0U ||
             head->shadow_phase != head_ladder[ladder_len - 1U])) {
            TEST_ASSERT(ladder_len < 4U);
            head_ladder[ladder_len++] = head->shadow_phase;
        }
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(ladder_len == 4U);
    TEST_ASSERT(head_ladder[0U] == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    TEST_ASSERT(head_ladder[1U] == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING);
    TEST_ASSERT(head_ladder[2U] == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    TEST_ASSERT(head_ladder[3U] == UCN_CLUSTER_PHASE_HEAD_STABLE);
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        const ucn_cluster_t *c = &network.nodes[i].cluster;

        TEST_ASSERT(c->shadow_phase == test_derive_phase(c, network.now_ms));
        TEST_ASSERT(c->shadow_transition_count > 0U);
        if (c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_READY) {
            saw_backup_ready = true;
        } else if (c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE) {
            saw_member_active = true;
        }
    }
    TEST_ASSERT(saw_backup_ready);
    TEST_ASSERT(saw_member_active);
    return 0;
}

/* CLV2-M01.0.1: member grace semantics.  ACTIVE -> GRACE when the
 * deadline arms; the deadline expiring does NOT re-derive ACTIVE (the
 * timeout action does that work); the timeout action then moves the
 * member to RECOVERY_OBSERVE (head and backup both dead, no one to
 * take over). */
static int cluster_test_shadow_grace_timeout(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *member;
    uint32_t now_ms;
    bool saw_grace = false;
    bool saw_expired_unconsumed_grace = false;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    /* Both Head and Backup die: nobody can take over, so the member's
     * grace must run to its timeout. */
    network.nodes[0].alive = false;
    network.nodes[1].alive = false;
    member = &network.nodes[2].cluster;
    for (now_ms = 141U; now_ms <= 400U; ++now_ms) {
        /* Before the owner steps: an armed grace deadline that has
         * expired must still derive GRACE (the expiry is an event, not
         * a state change). */
        if (member->role == UCN_CLUSTER_ROLE_MEMBER &&
            member->head_grace_deadline_ms != 0U &&
            ucn_deadline_expired(now_ms, member->head_grace_deadline_ms)) {
            TEST_ASSERT(test_derive_phase(member, now_ms) ==
                        UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
            saw_expired_unconsumed_grace = true;
        }
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
        if (member->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE) {
            saw_grace = true;
        }
    }
    TEST_ASSERT(saw_grace);
    TEST_ASSERT(saw_expired_unconsumed_grace);
    TEST_ASSERT(member->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    return 0;
}

/* CLV2-M01.0.2: a delayed same-generation Type12 (SYNC_BEGIN) from the
 * old Primary can re-arm backup_syncing while takeover is active.
 * Current FSM accepts it (no takeover guard in the handler), so the
 * shadow must express BACKUP_TAKEOVER with takeover_active+syncing and
 * the legacy validity gate must NOT fail.  The underlying deficiency
 * (late Primary sync mutating the takeover mirror) is deferred to
 * M09 committed/staging mirror + M10 frozen TakeoverConfig. */
static int cluster_test_shadow_takeover_late_sync(void)
{
    cluster_test_network_t network;
    cluster_test_fault_t fault;
    cluster_test_node_t *backup;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t now_ms;
    size_t i;
    bool injected = false;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
    }
    TEST_ASSERT(network.nodes[0].cluster.role == UCN_CLUSTER_ROLE_HEAD);
    backup = NULL;
    for (i = 1U; i < CLUSTER_TEST_NODES; ++i) {
        if (network.nodes[i].cluster.role == UCN_CLUSTER_ROLE_BACKUP &&
            network.nodes[i].cluster.backup_ready) {
            backup = &network.nodes[i];
            break;
        }
    }
    TEST_ASSERT(backup != NULL);

    /* Freeze the Head: it stops stepping (no heartbeat/advertise), so
     * the Backup's primary lease lapses and takeover starts, while the
     * Head stays alive in the peer table (RX checks still pass). */
    cluster_test_fault_setup(&fault);
    for (i = 0U; i < CLUSTER_TEST_NODES; ++i) {
        size_t j;

        for (j = 0U; j < CLUSTER_TEST_NODES; ++j) {
            fault.partition[i][j] = true;
        }
    }
    fault.skip_step_mask = 1U;

    for (now_ms = 141U; now_ms <= 260U; ++now_ms) {
        size_t ni;

        /* Manual tick so the injection can happen BETWEEN the owner step
         * (start_takeover) and the delivery phase (takeover completion),
         * which is exactly when a delayed Primary frame can arrive. */
        network.now_ms = now_ms;
        TEST_ASSERT(cluster_test_sync_neighbors_faulted(&network, &fault) ==
                    0);
        for (ni = 0U; ni < CLUSTER_TEST_NODES; ++ni) {
            if (!network.nodes[ni].alive) {
                continue;
            }
            if ((fault.skip_step_mask & (UINT32_C(1) << ni)) != 0U) {
                continue;
            }
            TEST_ASSERT(ucn_cluster_step(&network.nodes[ni].cluster) ==
                        UCN_OK);
        }
        if (!injected && backup->cluster.backup_takeover_active) {
            /* Delayed SYNC_BEGIN from the old Primary (same generation). */
            (void)memset(&message, 0, sizeof(message));
            message.type = UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC;
            message.role = UCN_CLUSTER_ROLE_HEAD;
            message.flags = UCN_CLUSTER_FLAG_SYNC_BEGIN;
            message.cluster_id = backup->cluster.cluster_id;
            message.term = backup->cluster.term;
            message.head_node_id = network.nodes[0].node_id;
            message.backup_generation = backup->cluster.backup_generation;
            TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) ==
                        UCN_OK);
            TEST_ASSERT(ucn_cluster_receive(
                            &backup->cluster, network.nodes[0].node_id,
                            true, encoded, sizeof(encoded)) == UCN_OK);
            /* Current FSM result: takeover stays active, the mirror
             * re-enters syncing.  Shadow must keep BACKUP_TAKEOVER and
             * the legacy gate must not fail. */
            TEST_ASSERT(backup->cluster.backup_takeover_active == true);
            TEST_ASSERT(backup->cluster.backup_syncing == true);
            TEST_ASSERT(backup->cluster.backup_ready == false);
            TEST_ASSERT(test_legacy_state_valid(&backup->cluster) == true);
            TEST_ASSERT(backup->cluster.shadow_phase ==
                        UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
            injected = true;
        }
        TEST_ASSERT(cluster_test_deliver_faulted(&network, &fault) == 0);
        TEST_ASSERT(cluster_test_shadow_audit(&network) == 0);
    }
    TEST_ASSERT(injected);
    /* The remaining ticks (and the final audit inside every tick) must
     * keep the shadow consistent; the takeover then runs its own
     * timeout path, which the Current FSM already defines. */
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
    ucn_cluster_phase_t prev_phase[3] = {
        UCN_CLUSTER_PHASE_DISABLED,
        UCN_CLUSTER_PHASE_DISABLED,
        UCN_CLUSTER_PHASE_DISABLED
    };
    bool saw_election_started = false;
    bool saw_election_won = false;
    bool saw_election_lost = false;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    for (now_ms = 0U; now_ms <= 140U; ++now_ms) {
        size_t index;
        TEST_ASSERT(cluster_test_tick(&network, now_ms) == 0);
        /* CLV2-01-04b.2 NIT: election-lifecycle reason loop - every phase
         * transition each node makes through the first election must carry
         * the exact reason from the migrated sites: ELECTION_STARTED on
         * entering ELECTION, ELECTION_WON on the winner's HEAD_*, and
         * ELECTION_LOST on a loser's ELECTON->DETACHED_OBSERVE. */
        for (index = 0U; index < 3U; ++index) {
            ucn_cluster_t *c = &network.nodes[index].cluster;
            ucn_cluster_phase_t cur = c->shadow_phase;

            if (cur != prev_phase[index]) {
                if (cur == UCN_CLUSTER_PHASE_ELECTION) {
                    TEST_ASSERT(c->transition_reason ==
                                UCN_CLUSTER_REASON_ELECTION_STARTED);
                    saw_election_started = true;
                } else if (prev_phase[index] == UCN_CLUSTER_PHASE_ELECTION &&
                           cur >= UCN_CLUSTER_PHASE_HEAD_NO_BACKUP &&
                           cur <= UCN_CLUSTER_PHASE_HEAD_STABLE) {
                    /* ELECTION -> HEAD_* is the election win itself;
                     * later HEAD sub-phase churn (ASSIGNING/SYNCING/
                     * STABLE) carries its own reasons. */
                    TEST_ASSERT(c->transition_reason ==
                                UCN_CLUSTER_REASON_ELECTION_WON);
                    saw_election_won = true;
                } else if (cur == UCN_CLUSTER_PHASE_DETACHED_OBSERVE &&
                           prev_phase[index] == UCN_CLUSTER_PHASE_ELECTION) {
                    TEST_ASSERT(c->transition_reason ==
                                UCN_CLUSTER_REASON_ELECTION_LOST);
                    saw_election_lost = true;
                }
                prev_phase[index] = cur;
            }
        }
    }
    /* Every election-lifecycle reason was actually observed on the real
     * path (start -> win, start -> loss). */
    TEST_ASSERT(saw_election_started);
    TEST_ASSERT(saw_election_won);
    TEST_ASSERT(saw_election_lost);
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

/* CLV2-01-04b.3: consider_head_offer()'s stable-Head-offer join now runs
 * DETACHED_OBSERVE/ELECTION -> JOIN_PENDING through the single transition
 * entry point (reason JOIN_INITIATED) BEFORE any phase-relevant legacy
 * mutation, then applies the begin_join() field payload at the site via
 * begin_join_prepare_fields().  A CANDIDATE node (the required scenario)
 * and a DETACHED_OBSERVE node both land on shadow == JOIN_PENDING with
 * transition_reason == JOIN_INITIATED; a recovery-eligible node keeps the
 * legacy begin_join() (01-04f owns the RECOVERY_* sources); a shadow
 * mismatch fails closed and applies NO join payload. */
static int cluster_test_head_offer_join_wiring(void)
{
    cluster_test_network_t network;
    cluster_test_node_t *candidate;
    cluster_test_node_t *detached;
    cluster_test_node_t *recovery;
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 0U;

    /* Scenario A (required): a CANDIDATE node (ELECTION) accepts a stable
     * Head ADVERTISE with available_capacity > 0 -> ELECTION -> JOIN_PENDING,
     * shadow committed FIRST with reason JOIN_INITIATED, then the site
     * field payload. */
    candidate = &network.nodes[1];
    TEST_ASSERT(ucn_cluster_test_transition(
                    &candidate->cluster, UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                    UCN_CLUSTER_PHASE_ELECTION,
                    UCN_CLUSTER_REASON_ELECTION_STARTED, 0U) == UCN_OK);
    TEST_ASSERT(candidate->cluster.role == UCN_CLUSTER_ROLE_CANDIDATE);
    candidate->cluster.cluster_id = candidate->node_id;
    candidate->cluster.term = 1U;
    candidate->cluster.head_node_id = candidate->node_id;
    candidate->cluster.election_deadline_ms = 20U;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_ADVERTISE;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = network.nodes[0].node_id;
    message.head_score = 9000U;
    message.available_capacity = 3U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&candidate->cluster,
                                    network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(candidate->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(candidate->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(candidate->cluster.transition_reason ==
                UCN_CLUSTER_REASON_JOIN_INITIATED);
    TEST_ASSERT(candidate->cluster.pending_head_node_id ==
                network.nodes[0].node_id);
    TEST_ASSERT(candidate->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(candidate->cluster.pending_term == 1U);
    TEST_ASSERT(candidate->cluster.pending_head_score == 9000U);
    TEST_ASSERT(candidate->cluster.role_since_ms == 0U);
    TEST_ASSERT(candidate->cluster.next_join_retry_ms == 0U);
    TEST_ASSERT(candidate->cluster.recovery_eligible == false);
    TEST_ASSERT(candidate->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(&candidate->cluster, 0U) ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);

    /* Scenario B: a DETACHED_OBSERVE node accepts the same offer ->
     * DETACHED_OBSERVE -> JOIN_PENDING through the entry point. */
    detached = &network.nodes[2];
    TEST_ASSERT(detached->cluster.role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(detached->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    message.nonce = 2U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&detached->cluster,
                                    network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(detached->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(detached->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(detached->cluster.transition_reason ==
                UCN_CLUSTER_REASON_JOIN_INITIATED);
    TEST_ASSERT(detached->cluster.pending_head_node_id ==
                network.nodes[0].node_id);
    TEST_ASSERT(detached->cluster.pending_cluster_id == 1U);
    TEST_ASSERT(detached->cluster.pending_term == 1U);
    TEST_ASSERT(detached->cluster.recovery_eligible == false);

    /* Scenario C: a recovery-eligible node keeps the legacy begin_join()
     * (RECOVERY_* sources stay legacy until 01-04f) - the join still
     * completes with the full field payload, and the end-of-RX shadow
     * sync derives JOIN_PENDING from the legacy state. */
    recovery = &network.nodes[3];
    recovery->cluster.role = UCN_CLUSTER_ROLE_DETACHED;
    recovery->cluster.recovery_eligible = true;
    recovery->cluster.recovery_backoff_deadline_ms = 0U;
    message.nonce = 3U;
    TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    TEST_ASSERT(ucn_cluster_receive(&recovery->cluster,
                                    network.nodes[0].node_id, true,
                                    encoded, sizeof(encoded)) == UCN_OK);
    TEST_ASSERT(recovery->cluster.role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    TEST_ASSERT(recovery->cluster.pending_head_node_id ==
                network.nodes[0].node_id);
    TEST_ASSERT(recovery->cluster.pending_term == 1U);
    TEST_ASSERT(recovery->cluster.recovery_eligible == false);
    TEST_ASSERT(recovery->cluster.recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(recovery->cluster.shadow_phase ==
                UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(recovery->cluster.transition_reason ==
                UCN_CLUSTER_REASON_JOIN_INITIATED);

    /* Scenario D (fail closed): a node whose legacy state (CANDIDATE) does
     * NOT derive the claimed old phase from the shadow mirror (stale
     * DETACHED_OBSERVE) rejects the transition - the join payload is NOT
     * applied and every field is left untouched.  The end-of-RX shadow
     * sync then reconciles the mirror to the legacy CANDIDATE state. */
    {
        cluster_test_network_t net2;

        TEST_ASSERT(cluster_test_network_init(&net2, 3U) == 0);
        net2.now_ms = 0U;
        net2.nodes[1].cluster.role = UCN_CLUSTER_ROLE_CANDIDATE;
        ucn_cluster_test_transition_asserts_set(false);
        message.nonce = 9U;
        TEST_ASSERT(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        TEST_ASSERT(ucn_cluster_receive(&net2.nodes[1].cluster,
                                        network.nodes[0].node_id, true,
                                        encoded, sizeof(encoded)) == UCN_OK);
        ucn_cluster_test_transition_asserts_set(true);
        TEST_ASSERT(net2.nodes[1].cluster.role == UCN_CLUSTER_ROLE_CANDIDATE);
        TEST_ASSERT(net2.nodes[1].cluster.pending_head_node_id == 0U);
        TEST_ASSERT(net2.nodes[1].cluster.pending_cluster_id == 0U);
        TEST_ASSERT(net2.nodes[1].cluster.pending_term == 0U);
        TEST_ASSERT(net2.nodes[1].cluster.pending_head_score == 0U);
        TEST_ASSERT(net2.nodes[1].cluster.recovery_eligible == false);
        TEST_ASSERT(net2.nodes[1].cluster.shadow_phase ==
                    UCN_CLUSTER_PHASE_ELECTION);
    }
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
                    const ucn_cluster_t *loser = &network.nodes[other].cluster;

                    TEST_ASSERT(loser->role == UCN_CLUSTER_ROLE_MEMBER);
                    TEST_ASSERT(loser->head_node_id == winner);
                    /* CLV2-M01.0.1/0.2: the loser joined the winner, so
                     * its last transition reason is exactly
                     * RECOVERY_YIELDED (never RECOVERY_WIN). */
                    TEST_ASSERT(loser->transition_reason ==
                                UCN_CLUSTER_REASON_RECOVERY_YIELDED);
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

/* CLV2-01-04a.1 (Item 1): the DIRECT edge list - every pair a SINGLE
 * production transition site can perform as one cluster_transition()
 * call.  The production CLUSTER_TRANSITION_DIRECT_ALLOWED must accept
 * EXACTLY this set (the 17x17 sweep below cross-checks both directions);
 * the per-edge site citations live on the production table.  The four
 * tick-granularity COMPOUND pairs are deliberately NOT here - they live
 * only in the production CLUSTER_TRANSITION_OBSERVED_ALLOWED table and
 * must never be passed to cluster_transition() (wiring realizes them via
 * their DIRECT constituents in sequence).  BACKUP_TAKEOVER stays legal
 * even while takeover_active && backup_syncing holds (CLV2-M01.0.2). */
static const struct cluster_transition_spec_pair {
    ucn_cluster_phase_t old_phase;
    ucn_cluster_phase_t new_phase;
} CLUSTER_TRANSITION_DIRECT[] = {
    /* DETACHED_OBSERVE */
    { UCN_CLUSTER_PHASE_DETACHED_OBSERVE, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_DETACHED_OBSERVE, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_DETACHED_OBSERVE, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    /* ELECTION */
    { UCN_CLUSTER_PHASE_ELECTION, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_ELECTION, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING },
    { UCN_CLUSTER_PHASE_ELECTION, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_ELECTION, UCN_CLUSTER_PHASE_HEAD_STABLE },
    { UCN_CLUSTER_PHASE_ELECTION, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_ELECTION, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    /* JOIN_PENDING */
    { UCN_CLUSTER_PHASE_JOIN_PENDING, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_JOIN_PENDING, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_JOIN_PENDING, UCN_CLUSTER_PHASE_BACKUP_SYNCING },
    /* MEMBER_ACTIVE */
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE },
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_MEMBER_ACTIVE, UCN_CLUSTER_PHASE_JOIN_PENDING },
    /* MEMBER_TAKEOVER_GRACE */
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE, UCN_CLUSTER_PHASE_BACKUP_SYNCING },
    /* HEAD_NO_BACKUP (NO_BACKUP->SYNCING / ->STABLE have no single site
     * and are never observed - dropped entirely) */
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING },
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_STEPPING_DOWN },
    /* HEAD_BACKUP_ASSIGNING */
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_HEAD_STABLE },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_STEPPING_DOWN },
    /* HEAD_BACKUP_SYNCING */
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_HEAD_STABLE },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_STEPPING_DOWN },
    /* HEAD_STABLE (->ASSIGNING is an observed compound only) */
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_STEPPING_DOWN },
    /* BACKUP_SYNCING */
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_BACKUP_READY },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_BACKUP_SYNCING, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    /* BACKUP_READY (->HEAD_NO_BACKUP is an observed compound only) */
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    /* BACKUP_TAKEOVER (stays legal under takeover_active && syncing) */
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_HEAD_NO_BACKUP },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_ELECTION },
    /* STEPPING_DOWN (deadline only; ->MEMBER_ACTIVE / ->DETACHED_OBSERVE
     * are observed compounds only) */
    { UCN_CLUSTER_PHASE_STEPPING_DOWN, UCN_CLUSTER_PHASE_JOIN_PENDING },
    /* RECOVERY_OBSERVE */
    { UCN_CLUSTER_PHASE_RECOVERY_OBSERVE, UCN_CLUSTER_PHASE_RECOVERY_ELECTION },
    { UCN_CLUSTER_PHASE_RECOVERY_OBSERVE, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_RECOVERY_OBSERVE, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    /* RECOVERY_ELECTION */
    { UCN_CLUSTER_PHASE_RECOVERY_ELECTION, UCN_CLUSTER_PHASE_RECOVERY_HEAD },
    { UCN_CLUSTER_PHASE_RECOVERY_ELECTION, UCN_CLUSTER_PHASE_JOIN_PENDING },
    { UCN_CLUSTER_PHASE_RECOVERY_ELECTION, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    /* RECOVERY_HEAD */
    { UCN_CLUSTER_PHASE_RECOVERY_HEAD, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    { UCN_CLUSTER_PHASE_RECOVERY_HEAD, UCN_CLUSTER_PHASE_MEMBER_ACTIVE },
    { UCN_CLUSTER_PHASE_RECOVERY_HEAD, UCN_CLUSTER_PHASE_STEPPING_DOWN },
};

/* CLV2-01-04b NIT-1: the tick-granularity COMPOUND pairs are NOT
 * duplicated here - the T-A gate checks the SINGLE production
 * CLUSTER_TRANSITION_OBSERVED_ALLOWED table via
 * ucn_cluster_test_observed_pair_allowed().  cluster_transition() must
 * reject them (wiring realizes them via their DIRECT constituents in
 * sequence); the 17x17 sweep below keeps the DIRECT test table pinned to
 * the production DIRECT table in both directions. */

/* CLV2-01-04a review A (F2): pairs the Current FSM can NEVER perform
 * (see the exclusion comment on CLUSTER_TRANSITION_ALLOWED for the per-pair
 * code evidence).  The matrix/spec must never admit them, and this list is
 * pinned below so a later edit that re-adds one fails the matrix test. */
static const struct cluster_transition_spec_pair CLUSTER_TRANSITION_EXCLUDED[] = {
    /* role CANDIDATE is written only by backup_challenge (BACKUP-only) and
     * start_election (DETACHED + !recovery_eligible): no HEAD->ELECTION. */
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_ELECTION },
    /* set_detached() is never called from a HEAD-role site. */
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_HEAD_STABLE, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    /* recovery_eligible == true never elects; no site clears eligibility
     * (or the armed backoff) while staying role DETACHED. */
    { UCN_CLUSTER_PHASE_RECOVERY_OBSERVE, UCN_CLUSTER_PHASE_ELECTION },
    { UCN_CLUSTER_PHASE_RECOVERY_OBSERVE, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_RECOVERY_ELECTION, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_RECOVERY_ELECTION, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    /* STEPPING_DOWN -> ELECTION only: the deadline always moves to
     * JOIN_PENDING (-> DETACHED_OBSERVE / -> MEMBER_ACTIVE are OBSERVED
     * tick compounds, so they stay allowed). */
    { UCN_CLUSTER_PHASE_STEPPING_DOWN, UCN_CLUSTER_PHASE_ELECTION },
    /* stepdown_recovery_head() keeps recovery_eligible == true. */
    { UCN_CLUSTER_PHASE_RECOVERY_HEAD, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    /* RECOVERY_HEAD exits are STEPPING_DOWN / MEMBER / RECOVERY_OBSERVE;
     * never JOIN_PENDING (empirically never observed, T-A capture). */
    { UCN_CLUSTER_PHASE_RECOVERY_HEAD, UCN_CLUSTER_PHASE_JOIN_PENDING },
    /* DETACHED_OBSERVE -> RECOVERY_OBSERVE: recovery_eligible is only set
     * in the same statement that writes role=DETACHED from MEMBER GRACE or
     * BACKUP (empirically never observed, T-A capture). */
    { UCN_CLUSTER_PHASE_DETACHED_OBSERVE, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    /* DISABLED is init-only: no enable/disable API exists, so neither
     * direction is a real FSM transition. */
    { UCN_CLUSTER_PHASE_DISABLED, UCN_CLUSTER_PHASE_DETACHED_OBSERVE },
    { UCN_CLUSTER_PHASE_DETACHED_OBSERVE, UCN_CLUSTER_PHASE_DISABLED },
    /* complete_takeover() always clears backup_node_id/ready, and a
     * BACKUP-role node always has recovery_eligible == false. */
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_HEAD_STABLE },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING },
    { UCN_CLUSTER_PHASE_BACKUP_TAKEOVER, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    { UCN_CLUSTER_PHASE_BACKUP_READY, UCN_CLUSTER_PHASE_RECOVERY_OBSERVE },
    /* HEAD_NO_BACKUP -> SYNCING / -> STABLE: no single site performs them
     * (assign_backup always enters ASSIGNING first; a READY requires an
     * already-selected backup); parity with the production exclusion
     * comment and never observed (T-A). */
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING },
    { UCN_CLUSTER_PHASE_HEAD_NO_BACKUP, UCN_CLUSTER_PHASE_HEAD_STABLE },
};

static bool cluster_transition_pair_in_direct(ucn_cluster_phase_t old_phase,
                                               ucn_cluster_phase_t new_phase)
{
    size_t index;

    for (index = 0U; index < sizeof(CLUSTER_TRANSITION_DIRECT) /
                              sizeof(CLUSTER_TRANSITION_DIRECT[0U]);
         ++index) {
        if (CLUSTER_TRANSITION_DIRECT[index].old_phase == old_phase &&
            CLUSTER_TRANSITION_DIRECT[index].new_phase == new_phase) {
            return true;
        }
    }
    return false;
}

/* CLV2-01-04b NIT-1: OBSERVED_ALLOWED comes from the SINGLE production
 * table (CLUSTER_TRANSITION_OBSERVED_ALLOWED = DIRECT union the tick
 * compounds) via the test hook - no test-side duplicate to drift. */
static bool cluster_transition_pair_in_observed(ucn_cluster_phase_t old_phase,
                                                ucn_cluster_phase_t new_phase)
{
    return ucn_cluster_test_observed_pair_allowed(old_phase, new_phase);
}

/* CLV2-01-04a.1 (Item 3): set the phase-relevant legacy fields so the
 * phase mapping derives EXACTLY `phase` - the pre-transition discipline
 * requires derive(cluster, now) == old_phase before every accepted call.
 * This mirrors the canonical legacy projection of each phase (the same
 * values the sites produce); it is a test-only duplicate of the spec. */
static void cluster_test_seed_legacy(ucn_cluster_t *cluster,
                                     ucn_cluster_phase_t phase,
                                     uint32_t now_ms)
{
    (void)now_ms;
    switch (phase) {
    case UCN_CLUSTER_PHASE_DISABLED:
        cluster->role = UCN_CLUSTER_ROLE_DISABLED;
        break;
    case UCN_CLUSTER_PHASE_DETACHED_OBSERVE:
        cluster->role = UCN_CLUSTER_ROLE_DETACHED;
        cluster->recovery_eligible = false;
        cluster->recovery_backoff_deadline_ms = 0U;
        cluster->recovery_cooldown_until_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_ELECTION:
        cluster->role = UCN_CLUSTER_ROLE_CANDIDATE;
        break;
    case UCN_CLUSTER_PHASE_JOIN_PENDING:
        cluster->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
        cluster->recovery_eligible = false;
        cluster->recovery_backoff_deadline_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_MEMBER_ACTIVE:
        cluster->role = UCN_CLUSTER_ROLE_MEMBER;
        cluster->head_grace_deadline_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE:
        cluster->role = UCN_CLUSTER_ROLE_MEMBER;
        cluster->head_grace_deadline_ms = now_ms + 100U;
        break;
    case UCN_CLUSTER_PHASE_HEAD_NO_BACKUP:
        cluster->role = UCN_CLUSTER_ROLE_HEAD;
        cluster->backup_node_id = 0U;
        cluster->backup_ready = false;
        cluster->backup_assign_pending = false;
        cluster->backup_syncing = false;
        break;
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING:
        cluster->role = UCN_CLUSTER_ROLE_HEAD;
        cluster->backup_node_id = 2U;
        cluster->backup_ready = false;
        cluster->backup_assign_pending = true;
        cluster->backup_syncing = false;
        break;
    case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
        cluster->role = UCN_CLUSTER_ROLE_HEAD;
        cluster->backup_node_id = 2U;
        cluster->backup_ready = false;
        cluster->backup_assign_pending = false;
        cluster->backup_syncing = false;
        break;
    case UCN_CLUSTER_PHASE_HEAD_STABLE:
        cluster->role = UCN_CLUSTER_ROLE_HEAD;
        cluster->backup_node_id = 2U;
        cluster->backup_ready = true;
        cluster->backup_assign_pending = false;
        cluster->backup_syncing = false;
        break;
    case UCN_CLUSTER_PHASE_BACKUP_SYNCING:
        cluster->role = UCN_CLUSTER_ROLE_BACKUP;
        cluster->backup_takeover_active = false;
        cluster->backup_ready = false;
        cluster->backup_syncing = true;
        break;
    case UCN_CLUSTER_PHASE_BACKUP_READY:
        cluster->role = UCN_CLUSTER_ROLE_BACKUP;
        cluster->backup_takeover_active = false;
        cluster->backup_ready = true;
        cluster->backup_syncing = false;
        break;
    case UCN_CLUSTER_PHASE_BACKUP_TAKEOVER:
        cluster->role = UCN_CLUSTER_ROLE_BACKUP;
        cluster->backup_takeover_active = true;
        cluster->backup_ready = false;
        break;
    case UCN_CLUSTER_PHASE_STEPPING_DOWN:
        cluster->role = UCN_CLUSTER_ROLE_STEPPING_DOWN;
        break;
    case UCN_CLUSTER_PHASE_RECOVERY_OBSERVE:
        cluster->role = UCN_CLUSTER_ROLE_DETACHED;
        cluster->recovery_eligible = true;
        cluster->recovery_backoff_deadline_ms = 0U;
        cluster->recovery_cooldown_until_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_RECOVERY_ELECTION:
        cluster->role = UCN_CLUSTER_ROLE_DETACHED;
        cluster->recovery_eligible = true;
        cluster->recovery_backoff_deadline_ms = now_ms + 1U;
        cluster->recovery_cooldown_until_ms = 0U;
        break;
    case UCN_CLUSTER_PHASE_RECOVERY_HEAD:
        cluster->role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
        break;
    default:
        break;
    }
}

/* Restore a pristine cluster and claim the given shadow phase (test-only). */
static void cluster_test_transition_reset(ucn_cluster_t *cluster,
                                          const ucn_cluster_t *pristine,
                                          ucn_cluster_phase_t phase)
{
    *cluster = *pristine;
    cluster->shadow_phase = phase;
    cluster->transition_reason = UCN_CLUSTER_REASON_UNKNOWN;
    cluster->shadow_transition_count = 0U;
}

/* CLV2-01-04a.1: full legality sweep over the DIRECT single-site matrix
 * (CLUSTER_TRANSITION_DIRECT_ALLOWED).  Every DIRECT pair is accepted and
 * leaves the shadow + legacy mirror consistent; every other pair is
 * rejected with UCN_ERR_STATE and NO state change (fail-closed).  The
 * task's examples (JOIN_PENDING -> HEAD_STABLE, DETACHED_OBSERVE ->
 * BACKUP_READY) are covered by the grid below.  The observed compounds
 * (HEAD_STABLE->HEAD_BACKUP_ASSIGNING, BACKUP_READY->HEAD_NO_BACKUP,
 * STEPPING_DOWN->MEMBER_ACTIVE/DETACHED_OBSERVE) are gate-only: they span
 * a whole tick and are never callable through cluster_transition(). */
static int cluster_test_transition_matrix(void)
{
    cluster_test_network_t network;
    ucn_cluster_t pristine;
    int old_i;
    int new_i;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    network.now_ms = 0U;
    /* Give the pristine copy a selected Backup identity so every Head
     * sub-phase derives correctly after a transition (the mirror writer
     * leaves backup_node_id caller-owned, exactly like the legacy sites). */
    network.nodes[0].cluster.backup_node_id = 2U;
    pristine = network.nodes[0].cluster;

    ucn_cluster_test_transition_asserts_set(false);
    for (old_i = 0; old_i < (int)UCN_CLUSTER_PHASE_COUNT; ++old_i) {
        for (new_i = 0; new_i < (int)UCN_CLUSTER_PHASE_COUNT; ++new_i) {
            ucn_cluster_t *c = &network.nodes[0].cluster;
            ucn_cluster_phase_t old_phase = (ucn_cluster_phase_t)old_i;
            ucn_cluster_phase_t new_phase = (ucn_cluster_phase_t)new_i;
            ucn_result_t result;

            if (old_i == new_i) {
                continue; /* no self-loops: nothing would change */
            }
            cluster_test_transition_reset(c, &pristine, old_phase);
            /* CLV2-01-04a.1 (Item 3): the legacy state must derive the
             * claimed old phase before the call (pre-transition
             * discipline); seed it from the canonical projection. */
            cluster_test_seed_legacy(c, old_phase, network.now_ms);
            if (cluster_transition_pair_in_direct(old_phase, new_phase)) {
                /* CLV2-01-04a review B (F4): pass the pair's REAL reason
                 * (from the BEST-EFFORT table) and assert verbatim
                 * passthrough - UNKNOWN is never used on accepted pairs. */
                ucn_cluster_transition_reason_t expected_reason =
                    ucn_cluster_test_reason_from_diff(old_phase, new_phase);

                TEST_ASSERT(expected_reason !=
                             UCN_CLUSTER_REASON_UNKNOWN);
                result = ucn_cluster_test_transition(
                    c, old_phase, new_phase, expected_reason, 0U);
                TEST_ASSERT(result == UCN_OK);
                TEST_ASSERT(c->shadow_phase == new_phase);
                TEST_ASSERT(c->transition_reason == expected_reason);
                TEST_ASSERT(c->shadow_transition_count == 1U);
                /* CLV2-01-04a.1 (Item 2): destinations whose mapping fields
                 * are caller/site-provided get the site's post-call write
                 * (the end-of-step shadow sync then re-aligns); apply_legacy
                 * itself writes role only for those. */
                switch (new_phase) {
                case UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING:
                    /* site: assign_backup() L2360 selects the Backup,
                     * start_backup_assignment_cycle() L3532 arms it. */
                    c->backup_node_id = 2U;
                    c->backup_assign_pending = true;
                    c->backup_ready = false;
                    break;
                case UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING:
                    /* site: assignment sweep L3566 completes. */
                    c->backup_node_id = 2U;
                    c->backup_assign_pending = false;
                    c->backup_ready = false;
                    break;
                case UCN_CLUSTER_PHASE_HEAD_STABLE:
                    /* site: handle_backup_ready() L2467. */
                    c->backup_node_id = 2U;
                    c->backup_ready = true;
                    break;
                case UCN_CLUSTER_PHASE_RECOVERY_ELECTION:
                    /* Item 4: caller-provided armed backoff. */
                    c->recovery_backoff_deadline_ms = network.now_ms + 1U;
                    break;
                default:
                    break;
                }
                TEST_ASSERT(test_derive_phase(c, network.now_ms) == new_phase);
                /* Item 2/5: the entry action never clears takeover outside
                 * BACKUP_TAKEOVER (site-owned); the only place the entry
                 * action SETS it is the BACKUP_TAKEOVER destination. */
                if (new_phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER) {
                    TEST_ASSERT(c->backup_takeover_active == true);
                }
            } else {
                ucn_cluster_t before = *c;

                result = ucn_cluster_test_transition(
                    c, old_phase, new_phase, UCN_CLUSTER_REASON_UNKNOWN, 0U);
                TEST_ASSERT(result == UCN_ERR_STATE);
                TEST_ASSERT(c->shadow_phase == before.shadow_phase);
                TEST_ASSERT(c->transition_reason == before.transition_reason);
                TEST_ASSERT(c->shadow_transition_count ==
                            before.shadow_transition_count);
                TEST_ASSERT(memcmp(c, &before, sizeof(*c)) == 0);
            }
        }
    }
    /* CLV2-01-04a review A (F2): pin the deliberate exclusions - every
     * pair in CLUSTER_TRANSITION_EXCLUDED must be rejected with
     * UCN_ERR_STATE, so a later edit that re-adds one fails here. */
    {
        size_t index;

        for (index = 0U;
             index < sizeof(CLUSTER_TRANSITION_EXCLUDED) /
                     sizeof(CLUSTER_TRANSITION_EXCLUDED[0U]);
             ++index) {
            ucn_cluster_t *c = &network.nodes[0].cluster;
            ucn_cluster_phase_t old_phase =
                CLUSTER_TRANSITION_EXCLUDED[index].old_phase;
            ucn_cluster_phase_t new_phase =
                CLUSTER_TRANSITION_EXCLUDED[index].new_phase;
            ucn_result_t result;

            TEST_ASSERT(!cluster_transition_pair_in_direct(old_phase,
                                                            new_phase));
            cluster_test_transition_reset(c, &pristine, old_phase);
            result = ucn_cluster_test_transition(
                c, old_phase, new_phase, UCN_CLUSTER_REASON_UNKNOWN, 0U);
            TEST_ASSERT(result == UCN_ERR_STATE);
            TEST_ASSERT(c->shadow_phase == old_phase);
            TEST_ASSERT(c->shadow_transition_count == 0U);
        }
    }

    /* CLV2-01-04a review B (F4): UNKNOWN-rejection case - passing UNKNOWN
     * on a legal pair must never record UNKNOWN: the pair-table fallback
     * derives the real reason. */
    {
        static const ucn_cluster_phase_t fallback_old[2] = {
            UCN_CLUSTER_PHASE_JOIN_PENDING,
            UCN_CLUSTER_PHASE_BACKUP_TAKEOVER
        };
        static const ucn_cluster_phase_t fallback_new[2] = {
            UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
            UCN_CLUSTER_PHASE_HEAD_NO_BACKUP
        };
        size_t index;

        for (index = 0U; index < 2U; ++index) {
            ucn_cluster_t *fc = &network.nodes[0].cluster;
            ucn_cluster_phase_t old_phase = fallback_old[index];
            ucn_cluster_phase_t new_phase = fallback_new[index];
            ucn_cluster_transition_reason_t derived;

            TEST_ASSERT(cluster_transition_pair_in_direct(old_phase,
                                                          new_phase));
            derived = ucn_cluster_test_reason_from_diff(old_phase, new_phase);
            TEST_ASSERT(derived != UCN_CLUSTER_REASON_UNKNOWN);
            cluster_test_transition_reset(fc, &pristine, old_phase);
            cluster_test_seed_legacy(fc, old_phase, network.now_ms);
            TEST_ASSERT(ucn_cluster_test_transition(
                            fc, old_phase, new_phase,
                            UCN_CLUSTER_REASON_UNKNOWN, 0U) == UCN_OK);
            TEST_ASSERT(fc->shadow_phase == new_phase);
            TEST_ASSERT(fc->transition_reason == derived); /* fallback */
        }
    }

    ucn_cluster_test_transition_asserts_set(true);
    return 0;
}

/* CLV2-01-04a: field-level semantics of legal transitions and the
 * fail-closed rejections. */
static int cluster_test_transition_apply(void)
{
    cluster_test_network_t network;
    ucn_cluster_t *c;
    ucn_cluster_t pristine;
    const uint32_t now_ms = 100U;

    TEST_ASSERT(cluster_test_network_init(&network, 3U) == 0);
    c = &network.nodes[0].cluster;
    network.now_ms = now_ms;
    pristine = *c;

    ucn_cluster_test_transition_asserts_set(false);

    /* 1) JOIN_PENDING -> MEMBER_ACTIVE (JOIN_ACCEPTED): role + grace, and
     *    epoch identity must survive (the writer never owns it). */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_JOIN_PENDING);
    c->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    c->cluster_id = 7U;
    c->term = 3U;
    c->head_node_id = 2U;
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                    UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                    UCN_CLUSTER_REASON_JOIN_ACCEPTED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->transition_reason == UCN_CLUSTER_REASON_JOIN_ACCEPTED);
    TEST_ASSERT(c->shadow_transition_count == 1U);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->cluster_id == 7U && c->term == 3U && c->head_node_id == 2U);
    /* CLV2-01-04a.1 (Item 2): apply_legacy writes role/grace/eligible
     * only - the (all-zero) pristine Backup mirror is left untouched, and
     * handle_join_accept() keeps known_backup_* (retained-state Test A). */
    TEST_ASSERT(c->backup_takeover_active == false);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(c->known_backup_node_id == 0U);
    TEST_ASSERT(c->known_backup_generation == 0U);

    /* 2) MEMBER_ACTIVE -> MEMBER_TAKEOVER_GRACE (HEAD_LEASE_EXPIRED):
     *    arming the grace deadline IS the phase. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    c->role = UCN_CLUSTER_ROLE_MEMBER;
    c->head_grace_deadline_ms = 0U;
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                    UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                    UCN_CLUSTER_REASON_HEAD_LEASE_EXPIRED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(c->head_grace_deadline_ms != 0U);
    TEST_ASSERT(c->head_grace_deadline_ms ==
                ucn_deadline_from_now(now_ms, c->config.keepalive_interval_ms));
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);

    /* 3) MEMBER_TAKEOVER_GRACE -> RECOVERY_OBSERVE (GRACE_TIMEOUT):
     *    recovery eligibility armed, grace disarmed, no backoff. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE);
    c->role = UCN_CLUSTER_ROLE_MEMBER;
    c->head_grace_deadline_ms = now_ms + 100U;
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_MEMBER_TAKEOVER_GRACE,
                    UCN_CLUSTER_PHASE_RECOVERY_OBSERVE,
                    UCN_CLUSTER_REASON_GRACE_TIMEOUT, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_DETACHED);
    TEST_ASSERT(c->recovery_eligible == true);
    TEST_ASSERT(c->recovery_backoff_deadline_ms == 0U);
    TEST_ASSERT(c->head_grace_deadline_ms == 0U);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);

    /* 4) HEAD_STABLE -> HEAD_NO_BACKUP (BACKUP_LOST). */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_HEAD_STABLE);
    c->role = UCN_CLUSTER_ROLE_HEAD;
    c->backup_node_id = 2U;
    c->backup_ready = true;
    c->backup_assign_pending = false;
    c->backup_syncing = false;
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_HEAD_STABLE,
                    UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                    UCN_CLUSTER_REASON_BACKUP_LOST, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(c->backup_node_id == 0U);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);

    /* 5) BACKUP_READY -> BACKUP_TAKEOVER (TAKEOVER_STARTED): takeover
     *    arms; ready is kept (start_takeover semantics); syncing untouched. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_BACKUP_READY);
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_ready = true;
    c->backup_syncing = false;
    c->backup_takeover_active = false;
    c->backup_primary_node_id = 1U;
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_BACKUP_READY,
                    UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                    UCN_CLUSTER_REASON_TAKEOVER_STARTED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->backup_ready == true);
    TEST_ASSERT(c->backup_syncing == false);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);

    /* 6) CLV2-M01.0.2: the reachable late-Type12 combo (takeover_active
     *    && backup_syncing, ready == false) is a VALID legacy state that
     *    derives BACKUP_TAKEOVER - the phase must express it, never
     *    reject or 'fix' it.  The exit below is a unit-level entry-action
     *    test: the mirror clears here are deliberate (they mirror what
     *    the real FSM path does), and wiring (01-04b..f) must retain the
     *    SITE's exit actions (consider_head_offer's newer-Term path calls
     *    backup_clear_sync() + clears takeover_active itself). */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_takeover_active = true;
    c->backup_syncing = true;
    c->backup_ready = false;
    TEST_ASSERT(test_legacy_state_valid(c) == true);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                    UCN_CLUSTER_PHASE_JOIN_PENDING,
                    UCN_CLUSTER_REASON_JOIN_INITIATED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_JOIN_PENDING);
    /* CLV2-01-04a.1 (Item 2): the hook does NOT wipe the Backup mirror
     * (the M01.0.2 combo survives the exit intact; the SITE - consider_
     * head_offer()'s newer-Term path - performs backup_clear_sync() +
     * the takeover clear at wiring time).  Phase-level guarantee only. */
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->backup_syncing == true);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(cluster_transition_pair_in_direct(
                    UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                    UCN_CLUSTER_PHASE_JOIN_PENDING));

    /* 6b) BACKUP_TAKEOVER -> HEAD_NO_BACKUP (TAKEOVER_QUORUM):
     *     CLV2-01-04a.1 (Item 2): the hook writes role + the provably-
     *     common Head invariants (node_id=0, ready=false) ONLY; the
     *     Backup mirror (takeover_active/syncing/primary) SURVIVES the
     *     call.  complete_takeover()'s exit clears stay at the SITE
     *     during 01-04b..f (simulated below).  The takeover-complete
     *     state is always HEAD_NO_BACKUP (never a populated Head
     *     sub-phase; review A F2); the HEAD mapper keys on
     *     backup_node_id, so a lingering mirror cannot mislabel it.
     *     backup_generation SURVIVES (complete_takeover leaves it;
     *     review C G1). */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_takeover_active = true;
    c->backup_syncing = true; /* M01.0.2 combo stays expressible */
    c->backup_ready = false;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 5U;
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                    UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                    UCN_CLUSTER_REASON_TAKEOVER_QUORUM, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_HEAD);
    TEST_ASSERT(c->backup_node_id == 0U);
    TEST_ASSERT(c->backup_ready == false);
    /* Item 2: the hook did NOT touch the mirror - the site must clear it. */
    TEST_ASSERT(c->backup_takeover_active == true);
    TEST_ASSERT(c->backup_syncing == true);
    TEST_ASSERT(c->backup_primary_node_id == 1U);
    /* G1: complete_takeover leaves backup_generation caller-owned. */
    TEST_ASSERT(c->backup_generation == 5U);
    /* Phase-level guarantee holds even with the mirror still set. */
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    /* Simulate complete_takeover()'s exit cleanup (site, 01-04b..f). */
    c->backup_takeover_active = false;
    c->backup_syncing = false;
    c->backup_primary_node_id = 0U;
    c->backup_ready = false;
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);

    /* 6c) CLV2-01-04a.1 (Item 5, T-B chain): BACKUP_TAKEOVER -> MEMBER_ACTIVE
     *     is a DIRECT site edge (handle_head_takeover L2917).  apply_legacy
     *     writes role/grace/eligible only, so the M01.0.2 mirror survives
     *     the hook call (phase mapping for MEMBER ignores it).  The SITE
     *     clears (takeover/syncing/ready/known_backup_*) are simulated here
     *     - exactly what 01-04 wiring must do - then re-assignment derives
     *     BACKUP_SYNCING with no phantom BACKUP_TAKEOVER and no
     *     TAKEOVER_PREPARE. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER);
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_takeover_active = true;
    c->backup_syncing = true; /* M01.0.2 combo */
    c->backup_ready = false;
    c->backup_primary_node_id = 1U;
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_BACKUP_TAKEOVER,
                    UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                    UCN_CLUSTER_REASON_TAKEOVER_STARTED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_MEMBER);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    /* Site-side exit clears (handle_head_takeover), simulated: without them
     * a later BACKUP re-assignment would derive a phantom BACKUP_TAKEOVER. */
    c->backup_takeover_active = false;
    c->backup_syncing = false;
    c->backup_ready = false;
    c->known_backup_node_id = 0U;
    c->known_backup_generation = 0U;
    /* Simulate the Head re-assigning this node as Backup. */
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                    UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                    UCN_CLUSTER_REASON_BACKUP_ASSIGNED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_BACKUP);
    TEST_ASSERT(c->backup_takeover_active == false); /* no phantom */
    TEST_ASSERT(c->backup_syncing == true);
    TEST_ASSERT(c->backup_ready == false);
    TEST_ASSERT(test_derive_phase(c, now_ms) ==
                UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    /* Step once: with takeover off, no TAKEOVER_PREPARE may be sent. */
    network.now_ms = now_ms;
    TEST_ASSERT(ucn_cluster_step(c) == UCN_OK);
    {
        size_t qi;
        bool saw_prepare = false;

        for (qi = 0U; qi < network.queue_count; ++qi) {
            if (network.queue[qi].payload[1U] ==
                (uint8_t)UCN_CLUSTER_MSG_TAKEOVER_PREPARE) {
                saw_prepare = true;
            }
        }
        TEST_ASSERT(saw_prepare == false);
    }
    TEST_ASSERT(c->backup_takeover_active == false);

    /* 6d) CLV2-01-04a review C (G1): backup_generation is caller-owned on
     *     every HEAD_* destination - it must SURVIVE the whole Head ladder
     *     (mirrors assign_backup incrementing it and complete_takeover /
     *     handle_backup_ready leaving it; zeroing it would break the C07.7
     *     P1 READY/DELTA generation fencing). */
    cluster_test_transition_reset(c, &pristine,
                                  UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING);
    c->role = UCN_CLUSTER_ROLE_HEAD;
    c->backup_node_id = 2U;
    c->backup_ready = false;
    c->backup_assign_pending = false;
    c->backup_syncing = false;
    c->backup_generation = 5U;
    /* SYNCING -> ASSIGNING -> SYNCING -> STABLE: generation stays 5.
     * The sub-phase-driving fields (assign_pending / ready) are written by
     * the SITE after each transition (start_backup_assignment_cycle /
     * sweep completion / handle_backup_ready), exactly the Item 2 model;
     * without them the next pre-transition derive (Item 3) would fail. */
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                    UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                    UCN_CLUSTER_REASON_BACKUP_ASSIGNED, now_ms) == UCN_OK);
    c->backup_assign_pending = true; /* site: start_backup_assignment_cycle L3532 */
    TEST_ASSERT(c->backup_generation == 5U);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING,
                    UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                    UCN_CLUSTER_REASON_BACKUP_SYNC_STARTED, now_ms) == UCN_OK);
    c->backup_assign_pending = false; /* site: assignment sweep L3566 */
    TEST_ASSERT(c->backup_generation == 5U);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING,
                    UCN_CLUSTER_PHASE_HEAD_STABLE,
                    UCN_CLUSTER_REASON_SNAPSHOT_READY, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_HEAD_STABLE);
    c->backup_ready = true; /* site: handle_backup_ready L2467 */
    TEST_ASSERT(c->backup_ready == true);
    TEST_ASSERT(c->backup_generation == 5U);
    /* STABLE -> NO_BACKUP (Backup lost) also keeps the generation. */
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_HEAD_STABLE,
                    UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                    UCN_CLUSTER_REASON_BACKUP_LOST, now_ms) == UCN_OK);
    TEST_ASSERT(c->backup_generation == 5U);

    /* 7) Claimed old phase != shadow: fail closed, nothing changes. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                    UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                    UCN_CLUSTER_REASON_JOIN_ACCEPTED, now_ms) == UCN_ERR_STATE);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    TEST_ASSERT(c->shadow_transition_count == 0U);

    /* 8) Illegal pair (task example JOIN_PENDING -> HEAD_STABLE):
     *    UCN_ERR_STATE and byte-for-byte no state change. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_JOIN_PENDING);
    c->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    {
        ucn_cluster_t before = *c;

        TEST_ASSERT(ucn_cluster_test_transition(
                        c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                        UCN_CLUSTER_PHASE_HEAD_STABLE,
                        UCN_CLUSTER_REASON_ELECTION_WON, now_ms) == UCN_ERR_STATE);
        TEST_ASSERT(memcmp(c, &before, sizeof(*c)) == 0);
    }

    /* 9) Illegal pair (task example DETACHED_OBSERVE -> BACKUP_READY). */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_DETACHED_OBSERVE);
    {
        ucn_cluster_t before = *c;

        TEST_ASSERT(ucn_cluster_test_transition(
                        c, UCN_CLUSTER_PHASE_DETACHED_OBSERVE,
                        UCN_CLUSTER_PHASE_BACKUP_READY,
                        UCN_CLUSTER_REASON_SNAPSHOT_READY, now_ms) == UCN_ERR_STATE);
        TEST_ASSERT(memcmp(c, &before, sizeof(*c)) == 0);
    }

    /* 10) CLV2-01-04a.1 (Item 5, Test A - join retained state): a
     *     JOIN_PENDING node that first saw BACKUP_ASSIGN for ANOTHER node
     *     records known_backup_node_id/generation; handle_join_accept()
     *     does NOT clear them, so JOIN_PENDING -> MEMBER_ACTIVE must keep
     *     them (apply_legacy must not wipe known_backup_*). */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_JOIN_PENDING);
    c->role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    c->known_backup_node_id = 3U; /* BACKUP_ASSIGN for another node */
    c->known_backup_generation = 7U;
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_JOIN_PENDING,
                    UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                    UCN_CLUSTER_REASON_JOIN_ACCEPTED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    TEST_ASSERT(c->known_backup_node_id == 3U);    /* UNCHANGED */
    TEST_ASSERT(c->known_backup_generation == 7U); /* UNCHANGED */
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_MEMBER_ACTIVE);

    /* 11) CLV2-01-04a.1 (Item 5, Test B - backup-challenge retained
     *     state): backup_challenge() clears ready/syncing/deadlines/
     *     takeover but NOT members[] or backup_generation; apply_legacy
     *     must not wipe the mirror on BACKUP_SYNCING -> ELECTION, and
     *     begin_join() keeps it through ELECTION -> JOIN_PENDING.  The
     *     ready/syncing/takeover clears are SITE-side, deferred to wiring. */
    cluster_test_transition_reset(c, &pristine, UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    c->role = UCN_CLUSTER_ROLE_BACKUP;
    c->backup_syncing = true;
    c->backup_ready = false;
    c->backup_takeover_active = false;
    c->backup_primary_node_id = 1U;
    c->backup_generation = 5U;
    c->members[0].occupied = true;
    c->members[0].node_id = 4U;
    c->members[0].last_nonce = 9U;
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_BACKUP_SYNCING);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_BACKUP_SYNCING,
                    UCN_CLUSTER_PHASE_ELECTION,
                    UCN_CLUSTER_REASON_ELECTION_STARTED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_ELECTION);
    TEST_ASSERT(c->role == UCN_CLUSTER_ROLE_CANDIDATE);
    /* members[] and backup_generation survive (no mirror wipe). */
    TEST_ASSERT(c->members[0].occupied == true);
    TEST_ASSERT(c->members[0].node_id == 4U);
    TEST_ASSERT(c->backup_generation == 5U);
    TEST_ASSERT(ucn_cluster_test_transition(
                    c, UCN_CLUSTER_PHASE_ELECTION,
                    UCN_CLUSTER_PHASE_JOIN_PENDING,
                    UCN_CLUSTER_REASON_JOIN_INITIATED, now_ms) == UCN_OK);
    TEST_ASSERT(c->shadow_phase == UCN_CLUSTER_PHASE_JOIN_PENDING);
    TEST_ASSERT(c->members[0].occupied == true); /* begin_join keeps it */
    TEST_ASSERT(c->backup_generation == 5U);
    TEST_ASSERT(test_derive_phase(c, now_ms) == UCN_CLUSTER_PHASE_JOIN_PENDING);

    ucn_cluster_test_transition_asserts_set(true);
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
    TEST_ASSERT(cluster_test_join_reject_shadow_transition() == 0);
    TEST_ASSERT(cluster_test_join_accept_out_of_order_after_backup_assign() == 0);
    TEST_ASSERT(cluster_test_join_pending_stepdown() == 0);
    TEST_ASSERT(cluster_test_fault_partition_takeover() == 0);
    TEST_ASSERT(cluster_test_fault_restart_no_old_term() == 0);
    TEST_ASSERT(cluster_test_fault_drop_eventually_converges() == 0);
    TEST_ASSERT(cluster_test_fault_deterministic_replay() == 0);
    TEST_ASSERT(cluster_test_fault_dup_delay_reorder_converges() == 0);
    TEST_ASSERT(cluster_test_fault_skip_owner_step() == 0);
    TEST_ASSERT(cluster_test_fault_neighbor_flap() == 0);
    TEST_ASSERT(cluster_test_fault_partition_heal() == 0);
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
    TEST_ASSERT(cluster_test_phase_mapping_static() == 0);
    TEST_ASSERT(cluster_test_shadow_lifecycle() == 0);
    TEST_ASSERT(cluster_test_shadow_grace_timeout() == 0);
    TEST_ASSERT(cluster_test_shadow_takeover_late_sync() == 0);
    TEST_ASSERT(cluster_test_transition_matrix() == 0);
    TEST_ASSERT(cluster_test_transition_apply() == 0);
    TEST_ASSERT(cluster_test_election_join_and_failover() == 0);
    TEST_ASSERT(cluster_test_head_offer_join_wiring() == 0);
    TEST_ASSERT(cluster_test_capacity_is_bounded() == 0);
    TEST_ASSERT(cluster_test_neighbor_summary_api() == 0);
    /* CLV2-01-04a review B (T-A): every phase pair actually OBSERVED by
     * the scenario suite must be a member of the observed SPEC
     * (CLUSTER_TRANSITION_OBSERVED_ALLOWED = DIRECT + tick compounds).
     * Run LAST so the collector has seen every tick. */
    TEST_ASSERT(cluster_test_observed_within_spec() == 0);
    return 0;
}
